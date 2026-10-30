/*
 * DOS 5.0 - Kernel, Meilenstein 3
 *
 * 80286 Protected Mode, segmentiert, kooperatives Multitasking.
 * Anwendungen laufen in Ring 3, jede mit eigenem TSS, eigener LDT und
 * ohne IOPL. Hardware und Speicher erreichen sie ausschliesslich ueber
 * die Family API (FAPI) per Call Gate. Programme werden ueber das IFS
 * von Diskette geladen, welche sagt STARTUP.CMD.
 */
#include "kern.h"
#include "../include/fapi_abi.h"
#include "mem.h"
#include "ifs.h"
#include "../include/fx.h"

/* ---- GDT-Belegung (Rest in kern.h) ------------------------------------- */
#define NTASK       4
#define TSS_IDX(i)  (9 + (i))
#define LDT_IDX(i)  (9 + NTASK + (i))
#define TSS_SEL(i)  (TSS_IDX(i) * 8)
#define LDT_SEL(i)  (LDT_IDX(i) * 8)
#define NGDT        (9 + 2 * NTASK)

/* LDT je Task: Eintrag 0 Code, 1 Daten/Stack, ab 2 per DosAllocSeg */
#define NLDT        16
#define LDT_FIRST_FREE 2
#define USEL(idx)   (((idx) << 3) | 4 | 3)  /* TI=1 (LDT), RPL 3          */
#define UCODE       USEL(0)
#define UDATA       USEL(1)

#define KBASE       0x10000UL
#define UNITS(n)    ((unsigned)(((n) + MEM_UNIT - 1) / MEM_UNIT))

#define LIN(p)      (KBASE + (unsigned short)(p))

/* Zugriffsbytes (286) */
#define ACC_KCODE   0x9A    /* P, DPL0, Code, lesbar              */
#define ACC_UCODE   0xFA    /* P, DPL3, Code, lesbar              */
#define ACC_UDATA   0xF2    /* P, DPL3, Daten, schreibbar         */
#define ACC_TSS     0x81    /* P, DPL0, 286-TSS, frei             */
#define ACC_LDT     0x82    /* P, DPL0, LDT                       */
#define ACC_CGATE3  0xE4    /* P, DPL3, 286-Call-Gate             */
#define ACC_IGATE   0x86    /* P, DPL0, 286-Interrupt-Gate        */

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
extern void halt_forever(void);

extern unsigned short isr_table[48];
extern unsigned char  boot_drive;
extern volatile unsigned short uptime_s;
extern unsigned short mem_conv_kb, mem_ext_kb;   /* vom BIOS, im Real Mode */
extern unsigned char  a20_ok;                     /* im Real Mode geprueft  */

/* ---- Kerneldaten ------------------------------------------------------- */
struct desc     gdt[NGDT];
struct gate     idt[48];
unsigned short  gdtr[3], idtr[3];

static struct tss286 tss_k;
static struct tss286 tss[NTASK];
static struct desc   ldt[NTASK][NLDT];
static unsigned      alloc_units;                 /* per DosAllocSeg vergeben */
static unsigned      image_unit[NTASK];           /* Programmsegment je Task */
static unsigned      image_cnt[NTASK];            /* ... und seine Einheiten */
static unsigned      image_units;
static unsigned short kstack[NTASK][256];      /* Ring-0-Stack je Task  */

static unsigned char  alive[NTASK];
static unsigned long  wake[NTASK];
static int            current = -1;
static char           switches[] = "0000000";

static char                 tname[NTASK][13];
static const unsigned char  tattr[NTASK] = { 0x0E, 0x0A, 0x0B, 0x0D };

static unsigned short __far *vram;

/* ---- Tabellen aufbauen (wird noch im Real Mode aufgerufen) ------------- */
static void put_desc(struct desc *d, unsigned long base, unsigned short limit,
                     unsigned char acc)
{
    d->limit   = limit;
    d->base_lo = (unsigned short)base;
    d->base_hi = (unsigned char)(base >> 16);
    d->access  = acc;
    d->rsv     = 0;
}

#define set_desc(i, b, l, a)  put_desc(&gdt[i], (b), (l), (a))

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
    set_desc(6, 0, 0, ACC_KDATA);
    set_desc(7, DMABUF_LIN, DMABUF_SIZE - 1, ACC_KDATA);
    set_desc(8, 0, 0, ACC_KDATA);
    for (i = 0; i < NTASK; i++) {
        set_desc(TSS_IDX(i), LIN(&tss[i]), sizeof(struct tss286) - 1, ACC_TSS);
        set_desc(LDT_IDX(i), LIN(ldt[i]),  sizeof(ldt[i]) - 1, ACC_LDT);
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

/* ---- Kernel-Hilfen ----------------------------------------------------- */
void scratch_map(unsigned long base, unsigned limit)
{
    set_desc(SCRATCH >> 3, base, limit, ACC_KDATA);
}

void scratch2_map(unsigned long base, unsigned limit)
{
    set_desc(SCRATCH2 >> 3, base, limit, ACC_KDATA);
}

void far_copy(unsigned dsel, unsigned doff, unsigned ssel, unsigned soff, unsigned n)
{
    char __far *d = MK_FP(dsel, doff);
    const char __far *s = MK_FP(ssel, soff);
    while (n--)
        *d++ = *s++;
}

void delay_ticks(unsigned n)
{
    unsigned long until = get_ticks() + n;
    while (get_ticks() < until)
        idle_wait();
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
#define HDR_W       14

static void task_header(int i, const char *state, unsigned char attr)
{
    unsigned char hattr = (unsigned char)(((tattr[i] & 7) << 4) | 0x0F);
    int c;
    fill_row(WIN_ROW(i), 0x07);
    for (c = 0; c < HDR_W; c++)
        vram[WIN_ROW(i) * 80 + c] = ((unsigned short)hattr << 8) | ' ';
    kputs(WIN_ROW(i), 1, tname[i], hattr);
    kputs(WIN_ROW(i), HDR_W + 1, state, attr);
}

static void status_bar(void)
{
    char buf[8];
    dec(uptime_s, buf, 5);
    kputs(24, 10, buf, 0x70);
    kputs(24, 38, switches, 0x70);
}

/* ---- Segmente fuer Anwendungen ----------------------------------------- */
static unsigned seg_alloc(int task, unsigned size)      /* size 0 = 64 KB */
{
    unsigned idx, units, u;
    unsigned long base;

    units = size ? (size >> 8) + ((size & 0xFF) != 0) : 256;
    for (idx = LDT_FIRST_FREE; idx < NLDT; idx++)
        if (ldt[task][idx].access == 0)
            break;
    if (idx == NLDT)
        return FAPI_ERROR;                      /* LDT voll             */
    u = mem_alloc(units);
    if (u == 0)
        return FAPI_ERROR;                      /* kein Speicher        */

    base = (unsigned long)u << 8;
    scratch_map(base, units * MEM_UNIT - 1);
    zero_seg(SCRATCH, units * (MEM_UNIT / 2));  /* nie fremde Altdaten  */

    put_desc(&ldt[task][idx], base, size ? size - 1 : 0xFFFF, ACC_UDATA);
    alloc_units += units;
    return USEL(idx);
}

static unsigned seg_free(int task, unsigned sel)
{
    unsigned idx = sel >> 3, units, start;
    struct desc *d;

    if (!(sel & 4) || idx < LDT_FIRST_FREE || idx >= NLDT)
        return FAPI_ERROR;
    d = &ldt[task][idx];
    if (d->access == 0)
        return FAPI_ERROR;
    start = (d->base_lo >> 8) | ((unsigned)d->base_hi << 8);
    units = (d->limit >> 8) + 1;
    mem_free(start, units);
    alloc_units -= units;
    d->access = 0;
    return 0;
}

static void task_cleanup(int task)                  /* bei Ende/Abbruch */
{
    unsigned idx;
    for (idx = LDT_FIRST_FREE; idx < NLDT; idx++)
        if (ldt[task][idx].access != 0)
            seg_free(task, USEL(idx));
    if (image_unit[task]) {                         /* Programmabbild     */
        mem_free(image_unit[task], image_cnt[task]);
        image_units -= image_cnt[task];
        image_unit[task] = 0;
        ldt[task][0].access = 0;
        ldt[task][1].access = 0;
    }
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
        if (!sel_writable(usel) || a1 > sel_limit(usel) - 9)
            return FAPI_ERROR;
        p = MK_FP(usel, a1);
        p[0] = mem_conv_kb + mem_ext_kb;
        p[1] = mem_free_kb();
        p[2] = 32;                              /* Kernel-Fenster       */
        p[3] = image_units / 4;
        p[4] = alloc_units / 4;
        return 0;
    }

    case FAPI_ALLOCSEG:
        return seg_alloc(current, a1);

    case FAPI_FREESEG:
        return seg_free(current, a1);

    case FAPI_YIELD:
        schedule_next();
        return 0;

    case FAPI_SLEEP:
        wake[current] = get_ticks() + (a1 + 9) / 10;
        schedule_next();
        return 0;

    case FAPI_EXIT:
        alive[current] = 0;
        task_cleanup(current);
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
        task_cleanup(current);
        task_header(current, "abgebrochen: ", 0x0C);
        kputs(WIN_ROW(current), HDR_W + 14, exc_name(vec), 0x0C);
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
    outb(0x21, 0xBE); outb(0xA1, 0xFF);     /* IRQ0 Takt, IRQ6 Diskette */

    /* PIT Kanal 0: 100 Hz */
    outb(0x43, 0x36);
    outb(0x40, 11932 & 0xFF);
    outb(0x40, 11932 >> 8);
}

/* ---- Programmlader ---------------------------------------------------- */
static char startup[512];

/* Zeilen aus STARTUP.CMD: je ein Programmname, REM und Leerzeilen zaehlen
   nicht. Gibt die Anzahl der gefundenen Namen zurueck. */
static int parse_startup(unsigned len, char names[][13], int max)
{
    unsigned p = 0, k;
    int n = 0;
    char c;

    while (p < len && n < max) {
        while (p < len && (startup[p] == ' ' || startup[p] == '\t'))
            p++;
        k = 0;
        while (p < len && startup[p] != '\r' && startup[p] != '\n' &&
               startup[p] != ' ' && startup[p] != '\t') {
            c = startup[p++];
            if (c >= 'a' && c <= 'z')
                c -= 32;
            if (k < 12)
                names[n][k++] = c;
        }
        names[n][k] = 0;
        while (p < len && startup[p] != '\n')
            p++;
        p++;
        if (k == 0 || (k == 3 && names[n][0] == 'R' && names[n][1] == 'E' &&
                       names[n][2] == 'M'))
            continue;
        n++;
    }
    return n;
}

static unsigned rd16(unsigned sel, unsigned off)
{
    unsigned char __far *p = MK_FP(sel, off);
    return p[0] | ((unsigned)p[1] << 8);
}

/* Programm im FX-Format laden: Datei in einen Zwischenpuffer, Kopf pruefen,
   Segment der richtigen Groesse anlegen, Abbild hineinkopieren. */
static const char *load_program(int i, const char *name)
{
    struct ifs_file f;
    struct fx_header h;
    struct tss286 *t;
    unsigned short *w;
    unsigned long base, seg;
    unsigned tu, tn, u, n, fx;
    const char *err = 0;

    if (ifs_open(name, &f))
        return "nicht gefunden";
    if (f.size < 64 || f.size > 0xFF00UL)
        return "kein FAPI-Programm";

    tn = UNITS(f.size);                         /* Zwischenpuffer       */
    tu = mem_alloc(tn);
    if (tu == 0)
        return "kein Speicher";
    if (ifs_read(&f, (unsigned long)tu << 8, tn * MEM_UNIT)) {
        mem_free(tu, tn);
        return "Lesefehler";
    }
    /* ifs_read hat SCRATCH auf den Puffer gelegt */
    fx = rd16(SCRATCH, 0x3C);
    if (rd16(SCRATCH, 0) != 0x5A4D || fx > (unsigned)f.size - sizeof(h)) {
        mem_free(tu, tn);
        return "kein FAPI-Programm";
    }
    far_copy(KDATA, (unsigned)&h, SCRATCH, fx, sizeof(h));
    seg = (unsigned long)h.image_size + h.bss_size + h.stack_size;
    if (h.sig[0] != FX_SIG0 || h.sig[1] != FX_SIG1 || h.version != FX_VERSION)
        err = "kein FAPI-Programm";
    else if ((unsigned long)h.image_off + h.image_size > f.size ||
             seg > 0x10000UL || h.entry >= h.image_size)
        err = "Programmkopf fehlerhaft";
    if (err) {
        mem_free(tu, tn);
        return err;
    }

    n = UNITS(seg);
    u = mem_alloc(n);
    if (u == 0) {
        mem_free(tu, tn);
        return "kein Speicher";
    }
    base = (unsigned long)u << 8;
    /* Fenster ueber alle belegten Einheiten, nicht nur bis seg - 1:
       zero_seg schreibt ganze Einheiten, ein echter 286 prueft die Grenze */
    scratch2_map(base, n * MEM_UNIT - 1);
    zero_seg(SCRATCH2, n * (MEM_UNIT / 2));     /* BSS und Stack = 0    */
    far_copy(SCRATCH2, 0, SCRATCH, h.image_off, h.image_size);
    mem_free(tu, tn);

    image_unit[i] = u;
    image_cnt[i]  = n;
    image_units  += n;
    put_desc(&ldt[i][0], base, h.image_size - 1, ACC_UCODE);
    put_desc(&ldt[i][1], base, (unsigned)(seg - 1), ACC_UDATA);

    t = &tss[i];
    for (w = (unsigned short *)t; w < (unsigned short *)(t + 1); w++)
        *w = 0;
    t->ss0   = KDATA;
    t->sp0   = (unsigned short)&kstack[i][256];
    t->cs    = UCODE;
    t->ip    = h.entry;
    t->ss    = UDATA;
    t->sp    = (unsigned short)seg;     /* 64 KB -> 0, Stack wickelt korrekt */
    t->ds    = UDATA;
    t->es    = UDATA;
    t->ldt   = LDT_SEL(i);
    t->flags = 0x0202;          /* IF=1, IOPL=0: kein I/O in Ring 3  */
    return 0;
}

/* Texte hintereinander in eine Zeile schreiben, gibt die naechste Spalte */
static int kputs_at(int row, int col, const char *s)
{
    kputs(row, col, s, 0x07);
    while (*s++)
        col++;
    return col;
}

static void log_num(int row, const char *pre, unsigned v, const char *post)
{
    char b[6], *p = b;
    int col;
    dec(v, b, 5);
    while (*p == '0' && p[1])
        p++;                                    /* fuehrende Nullen weg  */
    col = kputs_at(row, 2, pre);
    col = kputs_at(row, col + 1, p);
    kputs_at(row, col, post);
}

/* ---- Einstieg im Protected Mode ---------------------------------------- */
void kmain(void)
{
    struct ifs_file f;
    const char *err;
    int i, r, n, loaded = 0;

    vram = MK_FP(VIDEO, 0);
    outb(0x3D4, 0x0A);              /* Hardware-Cursor ausblenden        */
    outb(0x3D5, 0x20);
    for (r = 0; r < 25; r++)
        fill_row(r, 0x07);

    fill_row(0, 0x1F);
    kputs(0, 2, "DOS 5.0  -  alternative Zeitlinie  -  Kernel-Meilenstein 3", 0x1F);

    pic_pit_init();
    kputs(2, 2, "\xFE 286 Protected Mode, GDT 17, LDT je Task, FAPI-Gate 0023h, Takt 100 Hz", 0x07);

    if (a20_ok) {
        mem_init(mem_conv_kb, mem_ext_kb);
        log_num(3, "\xFE A20 frei, Speicherverwaltung:", mem_free_kb(), " KB frei");
    } else {
        /* ohne A20 laeuft jeder Zugriff ueber 1 MB unten herum */
        mem_init(mem_conv_kb, 0);
        log_num(3, "\xFE A20 GESPERRT - nur unter 1 MB:", mem_free_kb(), " KB frei");
    }

    fill_row(24, 0x70);
    kputs(24, 1, "Uptime:       s", 0x70);
    kputs(24, 24, "Taskwechsel:", 0x70);
    kputs(24, 55, "kooperatives Multitasking", 0x70);

    enable_ints();                  /* Diskette meldet sich per IRQ 6    */
    kputs(4, 2, "\xFE Diskette A: ...", 0x07);
    if (fd_init())
        panic("Diskettencontroller antwortet nicht.");
    if (ifs_mount(&fat12_driver))
        panic("Diskette A: hat kein lesbares FAT12-Dateisystem.");
    fill_row(4, 0x07);
    r = kputs_at(4, 2, "\xFE Diskette A: IFS-Treiber ");
    r = kputs_at(4, r, ifs_name());
    kputs_at(4, r, " eingeh\x84ngt");

    if (ifs_open("STARTUP.CMD", &f) || f.size > sizeof(startup) ||
        ifs_read(&f, LIN(startup), sizeof(startup)))
        panic("STARTUP.CMD fehlt oder ist unlesbar.");
    n = parse_startup((unsigned)f.size, tname, NTASK);

    for (i = 0; i < n; i++) {
        task_header(i, "wird geladen ...", 0x07);
        err = load_program(i, tname[i]);
        if (err) {
            task_header(i, err, 0x0C);
            continue;
        }
        alive[i] = 1;
        wake[i]  = 0;
        loaded++;
        task_header(i, "l\x84uft", 0x07);
    }
    fd_motor_off();
    log_num(5, "\xFE STARTUP.CMD:", loaded, " Programme geladen");

    load_tr(TSS_K);
    schedule_next();                /* kehrt nie zurueck                 */
}
