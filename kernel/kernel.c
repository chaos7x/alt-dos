/*
 * DOS 5.0 - Kernel, Meilenstein 1
 *
 * 80286 Protected Mode, segmentiert, kooperatives Multitasking.
 * Anwendungen laufen in Ring 3, jede mit eigenem TSS, eigenem Code- und
 * Datensegment und ohne IOPL. Hardware erreichen sie ausschliesslich ueber
 * die Family API (FAPI) per Call Gate.
 */
#include <i86.h>
#include "../include/fapi_abi.h"

#pragma pack(1)

/* ---- GDT-Belegung ------------------------------------------------------ */
#define KCODE       0x08
#define KDATA       0x10
#define VIDEO       0x18
#define GATE_IDX    4               /* -> Selektor 0x20, RPL 3 = 0x23      */
#define TSS_K       0x28
#define NTASK       4
#define TSS_IDX(i)  (6 + (i))
#define ACODE_IDX(i) (6 + NTASK + 2 * (i))
#define TSS_SEL(i)  (TSS_IDX(i) * 8)
#define ACODE(i)    (ACODE_IDX(i) * 8)
#define ADATA(i)    (ACODE(i) + 8)
#define NGDT        (6 + 3 * NTASK)

#define KBASE       0x10000UL
#define APPBASE(i)  (0x18000UL + (unsigned long)(i) * 0x2000UL)
#define APPSIZE     0x2000

#define LIN(p)      (KBASE + (unsigned short)(p))

/* Zugriffsbytes (286) */
#define ACC_KCODE   0x9A    /* P, DPL0, Code, lesbar              */
#define ACC_KDATA   0x92    /* P, DPL0, Daten, schreibbar         */
#define ACC_UCODE   0xFA    /* P, DPL3, Code, lesbar              */
#define ACC_UDATA   0xF2    /* P, DPL3, Daten, schreibbar         */
#define ACC_TSS     0x81    /* P, DPL0, 286-TSS, frei             */
#define ACC_CGATE3  0xE4    /* P, DPL3, 286-Call-Gate             */
#define ACC_IGATE   0x86    /* P, DPL0, 286-Interrupt-Gate        */

struct desc  { unsigned short limit, base_lo; unsigned char base_hi, access; unsigned short rsv; };
struct gate  { unsigned short off, sel; unsigned char wc, access; unsigned short rsv; };
struct tss286 {
    unsigned short link, sp0, ss0, sp1, ss1, sp2, ss2;
    unsigned short ip, flags, ax, cx, dx, bx, sp, bp, si, di;
    unsigned short es, cs, ss, ds, ldt;
};

/* ---- Assembler-Routinen (start.asm) ------------------------------------ */
extern void fapi_entry(void);
extern void task_jump(unsigned sel);
extern void load_tr(unsigned sel);
extern unsigned sel_readable(unsigned sel);
extern unsigned sel_writable(unsigned sel);
extern unsigned sel_limit(unsigned sel);
extern void outb(unsigned port, unsigned val);
extern unsigned long get_ticks(void);
extern void idle_wait(void);
extern void halt_forever(void);

extern unsigned short isr_table[48];
extern unsigned char  boot_drive;
extern volatile unsigned short uptime_s;
extern unsigned short mem_conv_kb, mem_ext_kb;   /* vom BIOS, im Real Mode */
extern unsigned char  kernel_end[];

/* ---- Kerneldaten ------------------------------------------------------- */
struct desc     gdt[NGDT];
struct gate     idt[48];
unsigned short  gdtr[3], idtr[3];

static struct tss286 tss_k;
static struct tss286 tss[NTASK];
static unsigned short kstack[NTASK][256];      /* Ring-0-Stack je Task  */

static unsigned char  alive[NTASK];
static unsigned long  wake[NTASK];
static int            current = -1;
static char           switches[] = "0000000";

static const char * const   tname[NTASK] = { "Task A", "Task B", "Task C", "MEM.EXE" };
static const unsigned char  tattr[NTASK] = { 0x0E, 0x0A, 0x0B, 0x0D };

static unsigned short __far *vram;

/* ---- Tabellen aufbauen (wird noch im Real Mode aufgerufen) ------------- */
static void set_desc(int i, unsigned long base, unsigned short limit, unsigned char acc)
{
    gdt[i].limit   = limit;
    gdt[i].base_lo = (unsigned short)base;
    gdt[i].base_hi = (unsigned char)(base >> 16);
    gdt[i].access  = acc;
    gdt[i].rsv     = 0;
}

void gdt_build(void)
{
    struct gate *g;
    int i;

    set_desc(0, 0, 0, 0);
    set_desc(1, KBASE, 0xFFFF, ACC_KCODE);
    set_desc(2, KBASE, 0xFFFF, ACC_KDATA);
    set_desc(3, 0xB8000UL, 80 * 25 * 2 - 1, ACC_KDATA);

    g = (struct gate *)&gdt[GATE_IDX];          /* FAPI-Call-Gate       */
    g->off    = (unsigned short)fapi_entry;
    g->sel    = KCODE;
    g->wc     = 0;
    g->access = ACC_CGATE3;
    g->rsv    = 0;

    set_desc(5, LIN(&tss_k), sizeof(struct tss286) - 1, ACC_TSS);
    for (i = 0; i < NTASK; i++) {
        set_desc(TSS_IDX(i),       LIN(&tss[i]), sizeof(struct tss286) - 1, ACC_TSS);
        set_desc(ACODE_IDX(i),     APPBASE(i),   APPSIZE - 1, ACC_UCODE);
        set_desc(ACODE_IDX(i) + 1, APPBASE(i),   APPSIZE - 1, ACC_UDATA);
    }

    for (i = 0; i < 48; i++) {
        idt[i].off    = isr_table[i];
        idt[i].sel    = KCODE;
        idt[i].wc     = 0;
        idt[i].access = isr_table[i] ? ACC_IGATE : 0;
        idt[i].rsv    = 0;
    }

    gdtr[0] = sizeof(gdt) - 1;
    gdtr[1] = (unsigned short)LIN(gdt);
    gdtr[2] = (unsigned short)(LIN(gdt) >> 16);
    idtr[0] = sizeof(idt) - 1;
    idtr[1] = (unsigned short)LIN(idt);
    idtr[2] = (unsigned short)(LIN(idt) >> 16);
}

/* ---- Konsolentreiber (nur der Kernel darf in den Bildspeicher) --------- */
static void vio_write(int row, int col, unsigned sel, unsigned off,
                      unsigned limit, unsigned char attr)
{
    const char __far *p = MK_FP(sel, 0);
    char c;

    if (row < 0 || row >= 25)
        return;
    while (col < 80 && off <= limit) {
        c = p[off];
        if (c == 0)
            break;
        vram[row * 80 + col] = ((unsigned short)attr << 8) | (unsigned char)c;
        col++;
        if (++off == 0)
            break;
    }
}

static void kputs(int row, int col, const char *s, unsigned char attr)
{
    vio_write(row, col, KDATA, (unsigned)s, 0xFFFF, attr);
}

static void fill_row(int row, unsigned char attr)
{
    int c;
    for (c = 0; c < 80; c++)
        vram[row * 80 + c] = ((unsigned short)attr << 8) | ' ';
}

static void hex4(unsigned v, char *b)
{
    static const char h[] = "0123456789ABCDEF";
    b[0] = h[(v >> 12) & 15];
    b[1] = h[(v >> 8) & 15];
    b[2] = h[(v >> 4) & 15];
    b[3] = h[v & 15];
    b[4] = 0;
}

static void dec(unsigned v, char *b, int width)
{
    b[width] = 0;
    while (width--) {
        b[width] = (char)('0' + v % 10);
        v /= 10;
    }
}

static void count_up(char *d)
{
    int i = 6;
    while (i >= 0) {
        if (d[i] == '9') { d[i] = '0'; i--; }
        else             { d[i]++;     break; }
    }
}

/* Fenster je Task: Kopfzeile + 3 Zeilen fuer die Anwendung */
#define WIN_ROW(i)  (7 + (i) * 4)

static void task_header(int i, const char *state, unsigned char attr)
{
    unsigned char hattr = (unsigned char)(((tattr[i] & 7) << 4) | 0x0F);
    int c;
    fill_row(WIN_ROW(i), 0x07);
    for (c = 0; c < 9; c++)
        vram[WIN_ROW(i) * 80 + c] = ((unsigned short)hattr << 8) | ' ';
    kputs(WIN_ROW(i), 1, tname[i], hattr);
    kputs(WIN_ROW(i), 10, state, attr);
}

static void status_bar(void)
{
    char buf[8];
    dec(uptime_s, buf, 5);
    kputs(24, 10, buf, 0x70);
    kputs(24, 38, switches, 0x70);
}

/* ---- Scheduler (kooperativ) -------------------------------------------- */
static void schedule_next(void);

static void panic(const char *msg)
{
    fill_row(24, 0x4F);
    kputs(24, 1, msg, 0x4F);
    halt_forever();
}

static void switch_to(int i)
{
    current = i;
    count_up(switches);
    status_bar();
    task_jump(TSS_SEL(i));          /* Hardware-Taskwechsel ueber das TSS */
}

static void schedule_next(void)
{
    int n, i, any;
    unsigned long now;

    for (;;) {
        now = get_ticks();
        status_bar();
        any = 0;
        for (n = 1; n <= NTASK; n++) {
            i = (current + n + NTASK) % NTASK;
            if (!alive[i])
                continue;
            any = 1;
            if (wake[i] > now)
                continue;
            if (i != current)
                switch_to(i);
            return;
        }
        if (!any)
            panic("Alle Tasks beendet - System angehalten.");
        idle_wait();                /* alle schlafen: HLT bis zum Takt  */
    }
}

/* ---- FAPI-Verteiler ---------------------------------------------------- */
unsigned fapi_dispatch(unsigned fn, unsigned usel, unsigned a1, unsigned a2)
{
    int row, col;

    switch (fn) {
    case FAPI_GETVERSION:
        return FAPI_VERSION;

    case FAPI_VIOWRTSTR:
        usel |= 3;                              /* Aufrufer ist Ring 3  */
        if (!sel_readable(usel))
            return FAPI_ERROR;
        row = a2 >> 8;
        col = a2 & 0xFF;
        if (row > 2 || col >= 80)
            return FAPI_ERROR;
        vio_write(WIN_ROW(current) + 1 + row, col, usel, a1,
                  sel_limit(usel), tattr[current]);
        return 0;

    case FAPI_MEMINFO: {
        unsigned short __far *p;
        usel |= 3;
        if (!sel_writable(usel) || a1 > sel_limit(usel) - 7)
            return FAPI_ERROR;
        p = MK_FP(usel, a1);
        p[0] = mem_conv_kb;
        p[1] = mem_ext_kb;
        p[2] = ((unsigned)kernel_end + 1023) / 1024;
        p[3] = NTASK * (APPSIZE / 1024);
        return 0;
    }

    case FAPI_YIELD:
        schedule_next();
        return 0;

    case FAPI_SLEEP:
        wake[current] = get_ticks() + (a1 + 9) / 10;
        schedule_next();
        return 0;

    case FAPI_EXIT:
        alive[current] = 0;
        task_header(current, "beendet (DosExit)", 0x07);
        schedule_next();
        return 0;                               /* nie erreicht         */
    }
    return FAPI_ERROR;
}

/* ---- Ausnahmebehandlung ------------------------------------------------ */
static const char *exc_name(unsigned v)
{
    switch (v) {
    case 0:  return "#DE Division durch Null";
    case 6:  return "#UD Ungueltiger Befehl";
    case 8:  return "#DF Doppelfehler";
    case 10: return "#TS Ungueltiges TSS";
    case 11: return "#NP Segment nicht vorhanden";
    case 12: return "#SS Stackfehler";
    case 13: return "#GP Allgemeine Schutzverletzung";
    }
    return "Ausnahme";
}

void fault(unsigned vec, unsigned err, unsigned cs, unsigned ip)
{
    static char msg[] = "Fehlercode ????h bei CS:IP ????:????";
    char h[5];

    hex4(err, h); msg[11] = h[0]; msg[12] = h[1]; msg[13] = h[2]; msg[14] = h[3];
    hex4(cs, h);  msg[27] = h[0]; msg[28] = h[1]; msg[29] = h[2]; msg[30] = h[3];
    hex4(ip, h);  msg[32] = h[0]; msg[33] = h[1]; msg[34] = h[2]; msg[35] = h[3];

    if ((cs & 3) == 3 && current >= 0) {
        /* Fehler in einer Anwendung: nur diesen Task beenden */
        alive[current] = 0;
        task_header(current, "abgebrochen: ", 0x0C);
        kputs(WIN_ROW(current), 23, exc_name(vec), 0x0C);
        kputs(WIN_ROW(current) + 3, 2, msg, 0x0C);
        schedule_next();
    }
    fill_row(23, 0x4F);
    kputs(23, 1, "KERNEL PANIC: ", 0x4F);
    kputs(23, 15, exc_name(vec), 0x4F);
    panic(msg);
}

/* ---- Hardware ---------------------------------------------------------- */
static void pic_pit_init(void)
{
    /* PICs auf Vektor 20h/28h legen, sonst kollidiert IRQ0 mit #DF */
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFE); outb(0xA1, 0xFF);     /* nur IRQ0 (Takt) frei    */

    /* PIT Kanal 0: 100 Hz */
    outb(0x43, 0x36);
    outb(0x40, 11932 & 0xFF);
    outb(0x40, 11932 >> 8);
}

/* ---- Einstieg im Protected Mode ---------------------------------------- */
void kmain(void)
{
    struct tss286 *t;
    unsigned short *w;
    int i, r;

    vram = MK_FP(VIDEO, 0);
    outb(0x3D4, 0x0A);              /* Hardware-Cursor ausblenden        */
    outb(0x3D5, 0x20);
    for (r = 0; r < 25; r++)
        fill_row(r, 0x07);

    fill_row(0, 0x1F);
    kputs(0, 2, "DOS 5.0  -  alternative Zeitlinie  -  Kernel-Meilenstein 1", 0x1F);

    kputs(2, 2, "\xFE 80286 Protected Mode aktiv (MSW.PE), A20 frei", 0x07);
    kputs(3, 2, "\xFE GDT: 18 Deskriptoren, IDT: 48 Vektoren, FAPI-Call-Gate 0023h", 0x07);

    pic_pit_init();
    kputs(4, 2, "\xFE PIC umgelegt (20h/28h), Systemtakt 100 Hz", 0x07);

    for (i = 0; i < NTASK; i++) {
        t = &tss[i];
        for (w = (unsigned short *)t; w < (unsigned short *)(t + 1); w++)
            *w = 0;
        t->ss0   = KDATA;
        t->sp0   = (unsigned short)&kstack[i][256];
        t->cs    = ACODE(i) | 3;
        t->ip    = 0;
        t->ss    = ADATA(i) | 3;
        t->sp    = APPSIZE;
        t->ds    = ADATA(i) | 3;
        t->es    = ADATA(i) | 3;
        t->flags = 0x0202;          /* IF=1, IOPL=0: kein I/O in Ring 3  */
        alive[i] = 1;
        wake[i]  = 0;
        task_header(i, "bereit", 0x07);
    }
    kputs(5, 2, "\xFE 4 Tasks angelegt (je TSS, Code-/Datensegment, Ring 3)", 0x07);

    fill_row(24, 0x70);
    kputs(24, 1, "Uptime:       s", 0x70);
    kputs(24, 24, "Taskwechsel:", 0x70);
    kputs(24, 55, "kooperatives Multitasking", 0x70);

    for (i = 0; i < NTASK; i++)
        task_header(i, "l\x84uft", 0x07);

    load_tr(TSS_K);
    schedule_next();                /* kehrt nie zurueck                 */
}
