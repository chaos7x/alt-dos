/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - Kernel, Meilenstein 4
 *
 * 80286 Protected Mode, segmentiert, kooperatives Multitasking.
 * Anwendungen laufen in Ring 3, jede mit eigenem TSS, eigener LDT und
 * ohne IOPL. Hardware und Speicher erreichen sie ausschliesslich ueber
 * die Family API (FAPI) per Call Gate. Programme laufen in Sitzungen mit
 * eigenem Bildschirm; die erste Sitzung startet PROTSHELL= (CMD.EXE /P).
 */
#include "kern.h"
#include "../include/fapi_abi.h"
#include "../include/fx.h"
#include "mem.h"
#if MEMMAP_SEL != KSEG_IDX(0) * 8
#error MEMMAP_SEL passt nicht zu KSEG_IDX(0)
#endif
#include "ifs.h"
#include "../include/drv.h"

/* LDT je Task: Eintrag 0 Code, 1 Daten/Stack, ab 2 per DosAllocSeg */
#define NLDT        16
#define LDT_FIRST_FREE 2
#define USEL(idx)   (((idx) << 3) | 4 | 3)  /* TI=1 (LDT), RPL 3          */
#define UCODE       USEL(0)
#define UDATA       USEL(1)

#define UNITS(n)    ((unsigned)(((n) + MEM_UNIT - 1) / MEM_UNIT))

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
extern void fpu_save(unsigned sel, unsigned off);
extern void fpu_restore(unsigned sel, unsigned off);
extern void fpu_reset(void);
extern void fpu_pm_init(void);
extern unsigned sel_readable(unsigned sel);
extern unsigned sel_writable(unsigned sel);
extern unsigned sel_limit(unsigned sel);

extern unsigned short isr_table[48];
extern unsigned short mem_conv_kb, mem_ext_kb;   /* vom BIOS, im Real Mode */
extern unsigned char cpu_386, bios_date[9], bios_model, vga_ok;
extern unsigned short fpu_ok, bios_equip, bios_ports[7];
static void sys_info(struct fapi_sysinfo __far *si);
extern unsigned char bios_hdcount, bios_fdpt[32];    /* INT 41h/46h vom Start (IDE.SYS) */
extern unsigned char  a20_ok;                     /* im Real Mode geprueft  */
extern unsigned short fapi_dx;                    /* viertes FAPI-Argument  */

/* ---- Kerneldaten ------------------------------------------------------- */
struct desc     gdt[NGDT];
struct gate     idt[48];
unsigned short  gdtr[3], idtr[3];

#define T_FREE  0
#define T_READY 1           /* laeuft oder schlaeft bis 'wake'           */
#define T_WAIT  2           /* wartet auf das Ende eines Kindprogramms   */
#define T_BLOCK 3           /* schlaeft in einem Treiber (DevHlp ProcBlock) */

struct task {
    unsigned char state;
    unsigned long wake;
    int           session;
    int           parent;   /* -1: Sitzungsinhaber                       */
    char          name[13];                       /* fuer die Titelzeile   */
    char          args[FAPI_ARGLEN];
    unsigned char drive;                          /* aktuelles Laufwerk    */
    char          cwd[IFS_PATHLEN];               /* "" = Stamm, "\DOS"   */
    unsigned      img_unit, img_cnt;        /* Datensegment (V1: alles)  */
    unsigned      code_unit, code_cnt;      /* V2: Codesegment, geteilt  */
    unsigned long code_key, code_fsize;     /* gleiches Programm?        */
    unsigned      sig_tramp, sig_mask;          /* DosSetSigHandler      */
    unsigned char std[3];                       /* Eintraege fuer Handle 0-2 */
    unsigned char bg;                           /* EXEC_BG: weckt/schliesst nichts */
    unsigned char lfn;                          /* FX_FLAG_LONGNAMES         */
    unsigned char sig_pending, in_sig;
    unsigned short sig_save[15];                /* Rahmen + AX waehrend des Handlers */
    unsigned char rc;                           /* Rueckgabewert (DosExit)  */
    unsigned char child_rc;                     /* des zuletzt beendeten Kindes */
    unsigned char hold;                         /* haelt SHUTDOWN auf (fragt) */
    unsigned char want_lock;                    /* braucht die Kernelsperre zurueck */
    unsigned char lock_depth;                   /* ihre Tiefe beim Abgeben  */
    volatile unsigned char block_rc;            /* ProcBlock: 0 geweckt, 1 Frist */
    unsigned long block_id;                     /* ProcBlock: Ereignis       */
    unsigned long block_until;                  /* ProcBlock: Frist, 0 = keine */
};

static struct task    task[NTASK];
static struct tss286  tss_k;
static struct tss286  tss[NTASK];
static struct desc    ldt[NTASK][NLDT];
#define KSTACK 768                                 /* Worte; IFS-Module schachteln tief */
static unsigned short kstack[NTASK][KSTACK];      /* Ring-0-Stack je Task  */
static unsigned       alloc_units;                /* per DosAllocSeg       */
static unsigned       image_units;                /* Programmabbilder      */
static int            current = -1;

/* Tabelle der offenen Dateien: eigenes Kernelsegment, Groesse aus FILES=
   (wie MS-DOS: fuer alle Programme zusammen) */
struct ofile {
    signed char     owner;  /* Task, die das Handle hat, oder -1         */
    unsigned char   refs;   /* Verweise (Handle + Standard-Handles), 0 = frei */
    unsigned char   kind;   /* OF_*                                      */
    unsigned char   pipe;   /* OF_PRD/OF_PWR: Nummer der Pipe            */
    struct ifs_file f;
    unsigned long   pos;
    char            path[IFS_PATHLEN];  /* kanonisch, fuer "in Benutzung" */
};
#define OF_FILE 0
#define OF_PRD  1           /* Leseende einer Pipe                      */
#define OF_PWR  2           /* Schreibende einer Pipe                   */
#define OF_CON  3           /* Konsole (Tastatur/Bildschirm der Sitzung) */
#define DASD_DISK 1         /* OF_DASD mit f.attr = DASD_DISK: ganze Platte,
                               f.drive = Blockgeraet (DosOpenDisk)      */
#define OF_DASD 5           /* Laufwerk als Ganzes (f.drive), pipe = gesperrt;
                               4 markiert eine schon geschlossene Datei      */
#define OF_LPT  6           /* Drucker PRN/LPT1-3 (f.drive = 0-2)        */
#define OF_NUL  7           /* NUL                                      */
#define OF_COM  8           /* COM1-4/AUX (f.drive = 0-3)               */
#define HBASE   3           /* Handles 0-2: Standard, ab 3: eigene Dateien */
#define CONS    0           /* Eintrag 0 ist die Konsole                */
static struct ofile __far *files;
static unsigned nfiles = 20;            /* FILES=, Vorgabe 20            */
#define NFILE nfiles

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

void set_desc(int i, unsigned long base, unsigned limit, unsigned char acc)
{
    put_desc(&gdt[i], base, limit, acc);
}

void gdt_build(void)
{
    struct gate *g;
    int i;

    set_desc(0, 0, 0, 0);
    set_desc(1, KCODE_LIN, ((kcode_size + 15) & ~15u) - 1, ACC_KCODE);
    set_desc(KCODE2_IDX, KCODE2_LIN, kcode2_size ? kcode2_size - 1 : 0, ACC_KCODE);
    set_desc(2, KDATA_LIN, 0xFFFF, ACC_KDATA);
    set_desc(3, 0xB8000UL, 80 * 50 * 2 - 1, ACC_KDATA);   /* bis 50 Zeilen */

    g = (struct gate *)&gdt[GATE_IDX];          /* FAPI-Call-Gate       */
    g->off    = (unsigned short)fapi_entry;
    g->sel    = KCODE;
    g->wc     = 0;
    g->access = ACC_CGATE3;
    g->rsv    = 0;

    set_desc(5, LIN(&tss_k), sizeof(struct tss286) - 1, ACC_TSS);
    set_desc(6, 0, 0, ACC_KDATA);               /* SCRATCH              */
    set_desc(7, DMABUF_LIN, DMABUF_SIZE - 1, ACC_KDATA);
    set_desc(8, 0, 0, ACC_KDATA);               /* SCRATCH2             */
    set_desc(KSEG_IDX(0), MEMMAP_LIN, 8191, ACC_KDATA); /* mem.c-Bitmap    */
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

/* SCRATCH/SCRATCH2 sichern und zurueckholen (8 Worte): fuer Kernelteile,
   die ohne Kernelsperre laufen (vio), waehrend ein anderer Task mitten in
   einer Dateioperation ueber die Fenster in einem Treiber schlaeft. Beim
   Taskwechsel laedt die CPU die Segmentregister neu aus der GDT. */
void win_save(unsigned short *b)
{
    unsigned short *a = (unsigned short *)&gdt[SCRATCH >> 3];
    unsigned short *c = (unsigned short *)&gdt[SCRATCH2 >> 3];
    int i;
    for (i = 0; i < 4; i++) {
        b[i] = a[i];
        b[4 + i] = c[i];
    }
}

void win_restore(const unsigned short *b)
{
    unsigned short *a = (unsigned short *)&gdt[SCRATCH >> 3];
    unsigned short *c = (unsigned short *)&gdt[SCRATCH2 >> 3];
    int i;
    for (i = 0; i < 4; i++) {
        a[i] = b[i];
        c[i] = b[4 + i];
    }
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

void panic(const char *msg)
{
    unsigned short __far *v = MK_FP(VIDEO, 0);
    int c;
    for (c = 0; c < 80; c++)
        v[24 * 80 + c] = 0x4F20;
    for (c = 0; msg[c] && c < 78; c++)
        v[24 * 80 + 1 + c] = 0x4F00 | (unsigned char)msg[c];
    halt_forever();
}

/* Strg+Alt+Entf: Der Neustart folgt beim naechsten Taskwechsel, wenn
   keine Plattenoperation laeuft (shutdown). Gibt kein Programm ab, startet
   IRQ 0 nach 5 s trotzdem neu; ein zweiter Druck startet sofort neu. */
volatile unsigned reboot_count;
static int boot_session;
static unsigned char going_down;    /* system_down laeuft: Strg+Alt+Entf
                                       startet dann sofort neu */

void reboot_request(void)
{
    if (reboot_count || going_down)
        reboot();
    reboot_count = 500;
}

/* alle Dateien schliessen, Platten-Caches leeren; dann Neustart oder
   anhalten (ein 286 kann sich nicht selbst ausschalten - APM gibt es erst
   ab 1992). Beim Anhalten bleibt Strg+Alt+Entf aktiv: Tastatur-IRQ setzt
   reboot_count, IRQ 0 startet nach 5 s neu. */
static void task_end(int t);
static void fpu_task_end(int t);
static int mod_mount(int drive);
static unsigned aud_fapi(unsigned usel, unsigned fn, unsigned a, unsigned b);
static void aud_task_end(int t);
static void schedule_next(void);
static void klock_enter(void);

/* Programme mit SIG_SHUTDOWN-Handler duerfen aufraeumen (bis 5 s), danach
   werden alle uebrigen Programme regulaer beendet. Ein Programm, das den
   Benutzer fragen muss (EDIT: speichern?), ruft DosShutdownHold: seine
   Sitzung kommt nach vorn, und der Kernel wartet ohne Frist, bis es endet
   (ein zweites Strg+Alt+Entf startet trotzdem sofort neu). */
static void end_programs(void)
{
    unsigned long until;
    int t, n, h;

    for (t = 0, n = 0; t < NTASK; t++)
        if (t != current && task[t].state != T_FREE && (task[t].sig_mask & SIG_SHUTDOWN)) {
            task[t].sig_pending |= SIG_SHUTDOWN;
            task[t].wake = 0;
            n++;
        }
    if (n) {
        vio_kputs(boot_session, "\xFE Programme werden beendet ...\n");
        until = get_ticks() + 500;
        while (n && get_ticks() < until) {
            task[current].wake = 0;
            schedule_next();                    /* die anderen laufen lassen */
            for (t = 0, n = 0, h = 0; t < NTASK; t++)
                if (t != current && task[t].state != T_FREE &&
                    (task[t].sig_mask & SIG_SHUTDOWN)) {
                    n++;
                    if (task[t].hold)
                        h = 1;
                }
            if (h)                              /* jemand fragt: warten  */
                until = get_ticks() + 500;
        }
    }
    /* die Sitzung, in der die Abschlussmeldung steht, bleibt stehen;
       Dateien schliessen nur mit der Kernelsperre (niemand darf mitten in
       einer Plattenoperation beendet werden) - sie bleibt bis zum Ende */
    klock_enter();
    for (t = 0; t < NTASK; t++)
        if (t != current && task[t].state != T_FREE && task[t].session != boot_session)
            task_end(t);
}

static void system_down(int restart)
{
    int k;
    reboot_count = 0;
    going_down = 1;
    vio_focus(boot_session);
    if (current >= 0)
        end_programs();
    vio_focus(boot_session);
    vio_kputs(boot_session, "\n\xFE Dateien werden geschlossen ...\n");
    for (k = 0; k < NFILE; k++)
        if (files[k].refs && files[k].kind == OF_FILE) {
            ifs_close(&files[k].f);
            files[k].refs = 0;
            files[k].owner = -1;
        }
    blk_flush();                                /* erst alle Daten      */
    ifs_shutdown();                             /* dann Platten "sauber" */
    blk_flush();
    outb(0x3F2, 0x0C);                          /* Diskettenmotor aus   */
    if (restart)
        reboot();
    vio_kputs(boot_session, "\n  Alle Dateien sind geschlossen.\n"
                            "  Der Rechner kann jetzt ausgeschaltet werden.\n"
                            "  (Strg+Alt+Entf startet neu.)\n");
    for (;;)
        idle_wait();
}

static void shutdown(void)
{
    system_down(1);
}

/* ---- eigene Kernelsegmente -------------------------------------------- */
/* Kernelsegment fuer ISA-DMA: darf keine 64-KB-Grenze ueberschreiten.
   Doppelt belegen, passendes Stueck behalten, Rest zurueck. */
unsigned kseg_dma(unsigned bytes, unsigned long *lin)
{
    unsigned i, u, n = (bytes + MEM_UNIT - 1) >> 8, s;
    unsigned long a;

    for (i = 0; i < NKSEG && gdt[KSEG_IDX(i)].access; i++)
        ;
    if (i == NKSEG || !(u = mem_alloc(2 * n)))
        return 0;
    a = (unsigned long)u << 8;
    s = u;
    if ((a >> 16) != ((a + bytes - 1) >> 16))
        s = (unsigned)(((a >> 16) + 1) << 8);   /* ab der naechsten Grenze */
    if (s > u)
        mem_free(u, s - u);
    if (u + 2 * n > s + n)
        mem_free(s + n, u + 2 * n - (s + n));
    *lin = (unsigned long)s << 8;
    set_desc(KSEG_IDX(i), *lin, bytes - 1, ACC_KDATA);
    return KSEG_IDX(i) * 8;
}

unsigned kseg_alloc(unsigned long bytes)
{
    unsigned i, u, n;

    if (bytes == 0 || bytes > 0x10000UL)
        return 0;
    bytes = (bytes + 1) & ~1UL;         /* zero_seg schreibt Worte       */
    for (i = 0; i < NKSEG && gdt[KSEG_IDX(i)].access; i++)
        ;
    if (i == NKSEG)
        return 0;
    n = (unsigned)((bytes + MEM_UNIT - 1) >> 8);
    u = mem_alloc(n);
    if (!u)
        return 0;
    set_desc(KSEG_IDX(i), (unsigned long)u << 8, (unsigned)(bytes - 1), ACC_KDATA);
    zero_seg(KSEG_IDX(i) * 8, (unsigned)((bytes + 1) >> 1));
    return KSEG_IDX(i) * 8;
}

/* ---- Echtzeituhr (CMOS) ------------------------------------------------ */
static unsigned cmos(unsigned r)
{
    outb(0x70, (unsigned char)r);
    return inb(0x71);
}

static unsigned from_bcd(unsigned v, unsigned bin)
{
    return bin ? v : (v >> 4) * 10 + (v & 15);
}

/* jetzt, als FAT-Datum << 16 | FAT-Uhrzeit */
unsigned long rtc_stamp(void)
{
    unsigned s, m, h, dd, mm, yy, cc, bin, n;

    for (n = 0; n < 2000 && (cmos(0x0A) & 0x80); n++)
        ;                                       /* Uhr zaehlt gerade     */
    bin = cmos(0x0B) & 4;
    s  = from_bcd(cmos(0x00), bin);
    m  = from_bcd(cmos(0x02), bin);
    h  = from_bcd(cmos(0x04) & 0x7F, bin);
    dd = from_bcd(cmos(0x07), bin);
    mm = from_bcd(cmos(0x08), bin);
    yy = from_bcd(cmos(0x09), bin);
    cc = from_bcd(cmos(0x32), bin);
    if (cc != 19 && cc != 20)
        cc = yy < 80 ? 20 : 19;
    yy += cc == 20 ? 100 : 0;                   /* Jahre seit 1900       */
    yy = yy < 80 ? 0 : yy - 80;                 /* FAT zaehlt ab 1980    */
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || h > 23 || m > 59 || s > 59)
        return (1UL << 5 | 1) << 16;            /* Uhr kaputt: 1.1.1980  */
    return ((unsigned long)((yy << 9) | (mm << 5) | dd) << 16) |
           ((h << 11) | (m << 5) | (s >> 1));
}

static unsigned to_bcd(unsigned v, unsigned bin)
{
    return bin ? v : ((v / 10) << 4) | (v % 10);
}

/* Echtzeituhr stellen (FAT-Format); 0 = ok */
static int rtc_set(unsigned date, unsigned time)
{
    unsigned y = 1980 + (date >> 9), mo = (date >> 5) & 15, dd = date & 31;
    unsigned h = time >> 11, mi = (time >> 5) & 63, sc = (time & 31) << 1;
    unsigned bin = cmos(0x0B) & 4, b = cmos(0x0B), f;

    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || h > 23 || mi > 59 || sc > 59)
        return -1;
    f = irq_off();
    outb(0x70, 0x0B); outb(0x71, b | 0x80);     /* Uhr anhalten          */
    outb(0x70, 0x00); outb(0x71, to_bcd(sc, bin));
    outb(0x70, 0x02); outb(0x71, to_bcd(mi, bin));
    outb(0x70, 0x04); outb(0x71, to_bcd(h, bin));    /* 24-Stunden-Modus */
    outb(0x70, 0x07); outb(0x71, to_bcd(dd, bin));
    outb(0x70, 0x08); outb(0x71, to_bcd(mo, bin));
    outb(0x70, 0x09); outb(0x71, to_bcd(y % 100, bin));
    outb(0x70, 0x32); outb(0x71, to_bcd(y / 100, bin));
    outb(0x70, 0x0B); outb(0x71, b & 0x7F);     /* weiterlaufen          */
    irq_restore(f);
    return 0;
}

/* Warmstart wie Strg+Alt+Entf unter MS-DOS: Kennung 1234h an 0040:0072,
   damit das BIOS den Speichertest ueberspringt, dann CPU-Reset */
void reboot(void)
{
    irq_off();
    /* Hier muss spaeter alles Ungeschriebene auf die Platte (FAT-Schreib-
       Cache, Platten-Cache per FLUSH CACHE / SYNCHRONIZE CACHE). Solange
       der Kernel nur liest, gibt es nichts zu leeren. */
    scratch_map(0, 0x4FF);
    *(unsigned short __far *)MK_FP(SCRATCH, 0x472) = 0x1234;
    outb(0x3F2, 0x0C);                          /* Diskettenmotor aus   */
    cpu_reset();
}

static void hex4(unsigned v, char *b)
{
    static const char h[] = "0123456789ABCDEF";
    b[0] = h[(v >> 12) & 15];
    b[1] = h[(v >> 8) & 15];
    b[2] = h[(v >> 4) & 15];
    b[3] = h[v & 15];
}

static void udec(unsigned v, char *b)          /* ohne fuehrende Nullen */
{
    char t[6];
    int i = 0, j = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i)
        b[j++] = t[--i];
    b[j] = 0;
}

static void scopy(char *d, const char *s, int max)
{
    int i;
    for (i = 0; i < max - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* Kernelmeldung in die Sitzung eines Tasks */
static void tmsg(int t, const char *a, const char *b, const char *c)
{
    int s = task[t].session;
    vio_kputs(s, a);
    if (b) vio_kputs(s, b);
    if (c) vio_kputs(s, c);
}

/* ---- Segmente fuer Anwendungen ----------------------------------------- */
static unsigned seg_alloc(int t, unsigned size)          /* size 0 = 64 KB */
{
    unsigned idx, units, u;
    unsigned long base;

    units = size ? (size >> 8) + ((size & 0xFF) != 0) : 256;
    for (idx = LDT_FIRST_FREE; idx < NLDT; idx++)
        if (ldt[t][idx].access == 0)
            break;
    if (idx == NLDT)
        return FAPI_ERROR;                      /* LDT voll             */
    u = mem_alloc(units);
    if (u == 0)
        return FAPI_ERROR;                      /* kein Speicher        */

    base = (unsigned long)u << 8;
    scratch_map(base, units * MEM_UNIT - 1);
    zero_seg(SCRATCH, units * (MEM_UNIT / 2));  /* nie fremde Altdaten  */

    put_desc(&ldt[t][idx], base, size ? size - 1 : 0xFFFF, ACC_UDATA);
    alloc_units += units;
    return USEL(idx);
}

/* LDT-Eintraege, die nur auf fremden Speicher zeigen (VioGetBuf: der
   Sitzungspuffer gehoert dem Kernel) - beim Freigeben kein mem_free.
   Die reservierten Deskriptorbytes taugen dafuer nicht: beim 386 stehen
   dort Basis und Grenze. */
static unsigned short ldt_alias[NTASK];

/* Alias-Eintrag umbiegen (Grafik: Bildspeicher <-> Puffer) / entfernen.
   Laeuft nur im Kernel; beim Ruecksprung laedt die CPU DS/ES neu. */
void ldt_set(int t, unsigned sel, unsigned long base, unsigned limit)
{
    unsigned idx = sel >> 3;
    if (t < 0 || t >= NTASK || idx < LDT_FIRST_FREE || idx >= NLDT ||
        !(ldt_alias[t] & (1u << idx)))
        return;
    put_desc(&ldt[t][idx], base, limit, ACC_UDATA);
}

void ldt_drop(int t, unsigned sel)
{
    unsigned idx = sel >> 3;
    if (t < 0 || t >= NTASK || idx < LDT_FIRST_FREE || idx >= NLDT ||
        !(ldt_alias[t] & (1u << idx)))
        return;
    ldt[t][idx].access = 0;
    ldt_alias[t] &= ~(1u << idx);
}

static unsigned seg_free(int t, unsigned sel)
{
    unsigned idx = sel >> 3, units, start;
    struct desc *d;

    if (!(sel & 4) || idx < LDT_FIRST_FREE || idx >= NLDT)
        return FAPI_ERROR;
    d = &ldt[t][idx];
    if (d->access == 0)
        return FAPI_ERROR;
    if (ldt_alias[t] & (1u << idx)) {
        ldt_alias[t] &= ~(1u << idx);
        d->access = 0;
        return 0;
    }
    start = (d->base_lo >> 8) | ((unsigned)d->base_hi << 8);
    units = (d->limit >> 8) + 1;
    mem_free(start, units);
    alloc_units -= units;
    d->access = 0;
    return 0;
}

/* ---- Programmlader ----------------------------------------------------- */
static unsigned rd16(unsigned sel, unsigned off)
{
    unsigned char __far *p = MK_FP(sel, off);
    return p[0] | ((unsigned)p[1] << 8);
}

/* ---- Pfade ------------------------------------------------------------ */
static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Pfad eines Tasks (oder t < 0: Laufwerk A:, Stamm) -> Laufwerk und
   kanonischer Pfad "\A\B" bzw. "\". Laenge der Eingabe bis 260 Zeichen. */
/* Laufwerk fuer Kernel-Pfade ohne Task (CONFIG.SYS, PROTSHELL=) und
   erstes Laufwerk der Programme: A: oder die Startpartition */
static int sys_drive = 0;

/* SUBST: Laufwerk X: zeigt auf ein Verzeichnis eines anderen Laufwerks.
   subst_drv[X] = Ziellaufwerk (-1 = keins), Pfad (kanonisch, "\\DOS")
   im Kernelsegment subst_sel, je IFS_PATHLEN Bytes. Programme sehen
   X:-Pfade (aktuelles Verzeichnis, QCurDir); die IFS-Treiber bekommen
   den umgesetzten Pfad auf dem Ziellaufwerk. */
static signed char subst_drv[NDRIVE];
static unsigned subst_sel;

/* Laufwerk + kanonischer Pfad auf das Ziel umsetzen; -1 = zu lang */
static int subst_map(int *d, char *path)
{
    static char tmp[IFS_PATHLEN];
    unsigned n = 0, k;
    char __far *pre;
    if (*d < 0 || *d >= NDRIVE || subst_drv[*d] < 0)
        return 0;
    pre = MK_FP(subst_sel, *d * IFS_PATHLEN);
    while (pre[n] && n < IFS_PATHLEN - 1)
        tmp[n] = pre[n], n++;
    if (path[1])                            /* nicht nur "\"           */
        for (k = 0; path[k]; k++) {
            if (n >= IFS_PATHLEN - 1)
                return -1;
            tmp[n++] = path[k];
        }
    if (!n)
        tmp[n++] = '\\';
    tmp[n] = 0;
    scopy(path, tmp, IFS_PATHLEN);
    *d = subst_drv[*d];
    return 0;
}

/* Laufwerksnummer fuer Abfragen (freier Platz, Bezeichnung) */
static unsigned subst_real(unsigned d)
{
    return d < NDRIVE && subst_drv[d] >= 0 ? (unsigned)subst_drv[d] : d;
}

/* JOIN (wie MS-DOS 3.1-5.0): Laufwerk X: haengt in einem leeren
   Verzeichnis eines anderen Laufwerks. join_drv[X] = Gastgeber (-1 =
   keins), Pfad (kanonisch gross, "\\CDROM") im Kernelsegment join_sel.
   Pfade unter dem Verzeichnis gehen an X:, X: selbst ist nicht mehr
   erreichbar. Gegenstueck zu subst_map, laeuft danach. */
static signed char join_drv[NDRIVE];
static unsigned join_sel;                   /* = subst_sel, hintere Haelfte */
#define JOIN_OFF (NDRIVE * IFS_PATHLEN)

/* Aktuelles Verzeichnis anderer Laufwerke (wie MS-DOS: CD A:\\X von C:
   aus) je Task, bis NDCWD Laufwerke; im selben Kernelsegment hinter SUBST
   und JOIN. Satz: Laufwerk (0xFF frei) + Pfad wie task.cwd. */
#define NDCWD    4
#define DCWD_SZ  (1 + IFS_PATHLEN)
#define DCWD_OFF (2 * NDRIVE * IFS_PATHLEN)
static char __far *dcwd(int t, int d, int create)
{
    char __far *p = MK_FP(subst_sel, DCWD_OFF + (unsigned)t * NDCWD * DCWD_SZ);
    char __far *fr = 0;
    unsigned i;
    if (!subst_sel)
        return 0;
    for (i = 0; i < NDCWD; i++, p += DCWD_SZ) {
        if ((unsigned char)p[0] == (unsigned char)d)
            return p + 1;
        if ((unsigned char)p[0] == 0xFF && !fr)
            fr = p;
    }
    if (!create)
        return 0;
    if (!fr) {                              /* voll: aeltesten ersetzen  */
        fr = MK_FP(subst_sel, DCWD_OFF + (unsigned)t * NDCWD * DCWD_SZ);
        far_copy(subst_sel, FP_OFF(fr), subst_sel, FP_OFF(fr) + DCWD_SZ,
                 (NDCWD - 1) * DCWD_SZ);
        fr += (NDCWD - 1) * DCWD_SZ;
    }
    fr[0] = (char)d;
    fr[1] = 0;
    return fr + 1;
}

static void dcwd_clear(int t)
{
    unsigned i;
    char __far *p = MK_FP(subst_sel, DCWD_OFF + (unsigned)t * NDCWD * DCWD_SZ);
    if (subst_sel)
        for (i = 0; i < NDCWD; i++)
            p[i * DCWD_SZ] = (char)0xFF;
}

static void fscopy(char __far *d, const char __far *s, unsigned max)
{
    unsigned i;
    for (i = 0; s[i] && i + 1 < max; i++)
        d[i] = s[i];
    d[i] = 0;
}

static int join_hidden(unsigned d)
{
    return d < NDRIVE && join_drv[d] >= 0;
}

/* Pfad lag unter einem JOIN-Verzeichnis und zeigt jetzt auf X: ->
   Laenge des Verzeichnisses + 1, sonst 0 */
static int join_map(int *d, char *path)
{
    unsigned x, n, k;
    char __far *jp;
    if (!join_sel)
        return 0;
    for (x = 0; x < NDRIVE; x++) {
        if (join_drv[x] != *d)
            continue;
        jp = MK_FP(join_sel, JOIN_OFF + x * IFS_PATHLEN);
        for (n = 0; jp[n] && up(path[n]) == jp[n]; n++)
            ;
        if (jp[n] || (path[n] && path[n] != '\\'))
            continue;
        if (!path[n])
            path[0] = '\\', path[1] = 0;
        else
            for (k = 0; (path[k] = path[n + k]) != 0; k++)
                ;
        *d = (int)x;
        return n + 1;
    }
    return 0;
}

/* liegt das aktuelle Verzeichnis cwd (Laufwerk d) im JOIN-Verzeichnis
   von x oder darunter? */
static int join_under(int d, const char *cwd, unsigned x)
{
    unsigned n;
    char __far *jp = MK_FP(join_sel, JOIN_OFF + x * IFS_PATHLEN);
    if (join_drv[x] != d)
        return 0;
    for (n = 0; jp[n] && up(cwd[n]) == jp[n]; n++)
        ;
    return !jp[n] && (!cwd[n] || cwd[n] == '\\');
}

/* ifs_namepath ueber eine JOIN-Grenze hinweg: Teil bis zum Verzeichnis
   vom Gastgeber, Rest vom eingehaengten Laufwerk */
static int namepath_j(int d, const char *canon, char *out, int shortn)
{
    static char tp[IFS_PATHLEN], rb[IFS_PATHLEN];
    int rd = d;
    unsigned hl, k, n;
    scopy(tp, canon, IFS_PATHLEN);
    hl = join_map(&rd, tp);
    if (!hl)
        return ifs_namepath(d, canon, out, shortn);
    scopy(rb, canon, IFS_PATHLEN);
    rb[hl - 1] = 0;
    if (ifs_namepath(d, rb, out, shortn))
        return -1;
    if (tp[1]) {
        if (ifs_namepath(rd, tp, rb, shortn))
            return -1;
        for (n = 0; out[n]; n++)
            ;
        for (k = 0; rb[k] && n < IFS_PATHLEN - 1; k++)
            out[n++] = rb[k];
        out[n] = 0;
    }
    return 0;
}

static void set_orig(const char *p);
static int scmp_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static int resolve_v(int t, const char *in, int *drive, char *out);

static int resolve(int t, const char *in, int *drive, char *out)
{
    if (resolve_v(t, in, drive, out) || subst_map(drive, out))
        return -1;
    join_map(drive, out);
    return 0;
}

/* wie resolve, aber ohne SUBST (Pfad aus Sicht des Programms) */
static int resolve_v(int t, const char *in, int *drive, char *out)
{
    const char *p = in, *q;
    int d = t >= 0 ? task[t].drive : sys_drive, n, k;

    if (p[0] && p[1] == ':') {
        if (up(p[0]) < 'A' || up(p[0]) > 'Z')
            return -1;
        d = up(p[0]) - 'A';
        p += 2;
    }
    if (join_hidden(d))
        return -1;                          /* per JOIN eingehaengt     */
    n = 0;
    out[0] = 0;
    if (*p != '\\' && *p != '/' && t >= 0) {
        char __far *oc = 0;
        if (d == task[t].drive)
            scopy(out, task[t].cwd, IFS_PATHLEN);   /* kanonisch = gross */
        else if ((oc = dcwd(t, d, 0)) != 0)
            fscopy((char __far *)out, oc, IFS_PATHLEN);
        for (k = 0; out[k]; k++)
            out[k] = up(out[k]);
        while (out[n])
            n++;
    }
    while (*p) {
        while (*p == '\\' || *p == '/')
            p++;
        for (q = p; *q && *q != '\\' && *q != '/'; q++)
            ;
        k = (int)(q - p);
        if (k == 0)
            break;
        if (k == 1 && p[0] == '.') {
            /* bleibt */
        } else if (k == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '\\')
                n--;
            if (n > 0)
                n--;                            /* den '\' davor auch   */
            out[n] = 0;
        } else {
            if (n + 1 + k >= IFS_PATHLEN)
                return -1;
            out[n++] = '\\';
            while (p < q)
                out[n++] = up(*p++);
            out[n] = 0;
        }
        p = q;
    }
    if (n == 0) {
        out[0] = '\\';
        out[1] = 0;
    }
    *drive = d;
    return 0;
}

/* Zeichenkette aus Ring 3 holen, hoechstens max - 1 Zeichen; -1 = Fehler */
static int user_str(unsigned sel, unsigned off, char *buf, int max)
{
    unsigned lim;
    int i;
    if (!sel_readable(sel))
        return -1;
    lim = sel_limit(sel);
    for (i = 0; i < max; i++) {
        if (off + i > lim || off + i < off)
            return -1;
        buf[i] = *(char __far *)MK_FP(sel, off + i);
        if (!buf[i])
            return 0;
    }
    return -1;                                  /* zu lang              */
}

/* ---- Abbilder im FX-Format laden --------------------------------------- */
/* Datei lesen, Kopf pruefen, Segment anlegen und fuellen. SCRATCH2 zeigt
   danach auf das neue Segment. 0 = ok, sonst EXEC_ERR_*                  */
/* V2: Codesegment des geladenen Programms (ld_shared: von einem anderen
   Task uebernommen) */
static unsigned ld_cu, ld_cn, ld_shared;
static unsigned long ld_key, ld_fsize;

/* Schluessel fuer "dasselbe Programm": Laufwerk und kanonischer Pfad */
static unsigned long path_key(int drive, const char *p)
{
    unsigned long k = (unsigned long)drive + 1;
    while (*p)
        k = (k << 5) + k + (unsigned char)*p++;     /* k * 33 + c       */
    return k;
}

static int load_image(int drive, const char *path, struct fx_header *h,
                      unsigned *unit, unsigned *cnt, unsigned long *segsize)
{
    struct ifs_file f;
    unsigned long base, seg;
    unsigned tu, tn, u, n, fx, csz;
    int err = 0, k;

    if (ifs_open(drive, path, &f))
        return EXEC_ERR_NOTFOUND;
    if (f.size < 64 || f.size > 0xFF00UL)
        return EXEC_ERR_FORMAT;

    tn = UNITS(f.size);                         /* Zwischenpuffer       */
    tu = mem_alloc(tn);
    if (tu == 0)
        return EXEC_ERR_NOMEM;
    scratch_map((unsigned long)tu << 8, tn * MEM_UNIT - 1);
    if (ifs_read(&f, 0, SCRATCH, 0, (unsigned)f.size) != (int)f.size) {
        mem_free(tu, tn);
        return EXEC_ERR_NOTFOUND;
    }
    fx = rd16(SCRATCH, 0x3C);
    if (rd16(SCRATCH, 0) != 0x5A4D || fx > (unsigned)f.size - sizeof(*h)) {
        mem_free(tu, tn);
        return EXEC_ERR_FORMAT;
    }
    far_copy(KDATA, (unsigned)h, SCRATCH, fx, sizeof(*h));
    if (h->version != FX_VERSION2)
        h->code_size = 0;               /* V1: ein Segment               */
    csz = h->code_size;
    seg = (unsigned long)(h->image_size - csz) + h->bss_size + h->stack_size;
    if (h->sig[0] != FX_SIG0 || h->sig[1] != FX_SIG1 ||
        (h->version != FX_VERSION && h->version != FX_VERSION2) ||
        (unsigned long)h->image_off + h->image_size > f.size ||
        seg > 0x10000UL || h->image_size == 0 || csz > h->image_size ||
        (csz & 15) || h->entry >= (csz ? csz : h->image_size) ||
        (h->version == FX_VERSION2 && csz == 0))
        err = EXEC_ERR_FORMAT;
    if (err) {
        mem_free(tu, tn);
        return err;
    }

    ld_cu = ld_cn = ld_shared = 0;
    if (csz) {                          /* V2: Code teilen oder laden    */
        ld_key = path_key(drive, path);
        ld_fsize = f.size;
        for (k = 0; k < NTASK; k++)
            if (task[k].state != T_FREE && task[k].code_unit &&
                task[k].code_key == ld_key && task[k].code_fsize == ld_fsize) {
                ld_cu = task[k].code_unit;
                ld_cn = task[k].code_cnt;
                ld_shared = 1;
                break;
            }
        if (!ld_shared) {
            ld_cn = UNITS(csz);
            ld_cu = mem_alloc(ld_cn);
            if (ld_cu == 0) {
                mem_free(tu, tn);
                return EXEC_ERR_NOMEM;
            }
            scratch2_map((unsigned long)ld_cu << 8, ld_cn * MEM_UNIT - 1);
            far_copy(SCRATCH2, 0, SCRATCH, h->image_off, csz);
        }
    }
    n = UNITS(seg);
    u = mem_alloc(n);
    if (u == 0) {
        if (csz && !ld_shared)
            mem_free(ld_cu, ld_cn);
        mem_free(tu, tn);
        return EXEC_ERR_NOMEM;
    }
    base = (unsigned long)u << 8;
    /* Fenster ueber alle belegten Einheiten, nicht nur bis seg - 1:
       zero_seg schreibt ganze Einheiten, ein echter 286 prueft die Grenze */
    scratch2_map(base, n * MEM_UNIT - 1);
    zero_seg(SCRATCH2, n * (MEM_UNIT / 2));     /* BSS und Stack = 0    */
    far_copy(SCRATCH2, 0, SCRATCH, h->image_off + csz, h->image_size - csz);
    mem_free(tu, tn);
    *unit = u;
    *cnt = n;
    *segsize = seg;
    return 0;
}

/* Programm in Task t laden. 0 = ok, sonst EXEC_ERR_* */
static int load_program(int t, int drive, const char *path)
{
    struct fx_header h;
    struct tss286 *ts;
    unsigned short *w;
    unsigned long base, seg;
    unsigned u, n;
    int err;

    err = load_image(drive, path, &h, &u, &n, &seg);
    if (err)
        return err;
    if (h.flags & FX_FLAG_DRIVER) {
        mem_free(u, n);
        if (ld_cu && !ld_shared)
            mem_free(ld_cu, ld_cn);
        return EXEC_ERR_FORMAT;                 /* Treiber, kein Programm */
    }
    base = (unsigned long)u << 8;
    task[t].lfn = (h.flags & FX_FLAG_LONGNAMES) != 0;
    task[t].img_unit = u;
    task[t].img_cnt  = n;
    image_units     += n;
    task[t].code_unit = ld_cu;
    task[t].code_cnt  = ld_cn;
    task[t].code_key  = ld_key;
    task[t].code_fsize = ld_fsize;
    if (ld_cu && !ld_shared)
        image_units += ld_cn;
    for (n = 0; n < NLDT; n++)
        ldt[t][n].access = 0;
    if (ld_cu)                                  /* V2: eigenes Codesegment */
        put_desc(&ldt[t][0], (unsigned long)ld_cu << 8, h.code_size - 1, ACC_UCODE);
    else
        put_desc(&ldt[t][0], base, h.image_size - 1, ACC_UCODE);
    put_desc(&ldt[t][1], base, (unsigned)(seg - 1), ACC_UDATA);

    ts = &tss[t];
    for (w = (unsigned short *)ts; w < (unsigned short *)(ts + 1); w++)
        *w = 0;
    ts->ss0   = KDATA;
    ts->sp0   = (unsigned short)&kstack[t][KSTACK];
    ts->cs    = UCODE;
    ts->ip    = h.entry;
    ts->ss    = UDATA;
    ts->sp    = (unsigned short)seg;    /* 64 KB -> 0, Stack wickelt korrekt */
    ts->ds    = UDATA;
    ts->es    = UDATA;
    ts->ldt   = LDT_SEL(t);
    ts->flags = 0x0202;         /* IF=1, IOPL=0: kein I/O in Ring 3     */
    return 0;
}

/* Befehlszeile "PFAD\PROG ARGS" -> Pfad (mit .EXE, falls ohne Endung),
   Anzeigename und Argumente */
static int split_cmd(const char *cmd, char *path, char *name, char *args)
{
    int i = 0, last = 0, dot = 0;
    char c;

    while (*cmd == ' ')
        cmd++;
    while (*cmd && *cmd != ' ') {
        if (i >= IFS_PATHLEN - 5)
            return -1;
        c = *cmd++;
        if (c == '\\' || c == '/' || c == ':') {
            last = i + 1;
            dot = 0;
        } else if (c == '.') {
            dot = 1;
        }
        path[i++] = c;
    }
    if (!dot) {
        path[i++] = '.'; path[i++] = 'E'; path[i++] = 'X'; path[i++] = 'E';
    }
    path[i] = 0;
    for (i = 0; i < 12 && path[last + i]; i++)
        name[i] = up(path[last + i]);
    name[i] = 0;
    while (*cmd == ' ')
        cmd++;
    scopy(args, cmd, FAPI_ARGLEN);
    return 0;
}

/* ---- Umgebung ----------------------------------------------------------
   Je Task FAPI_ENVSIZE Bytes "NAME=WERT\0...\0\0" in einem gemeinsamen
   Kernelsegment, dahinter die Systemumgebung (t < 0): COMSPEC und SET= aus
   CONFIG.SYS. Ein neues Programm erbt die Umgebung seines Aufrufers. */
static unsigned env_sel;

static unsigned env_off(int t)
{
    return (t < 0 ? NTASK : t) * FAPI_ENVSIZE;
}

/* Laenge des belegten Teils (ohne die abschliessende Null) */
static unsigned env_used(int t)
{
    char __far *e = MK_FP(env_sel, env_off(t));
    unsigned n = 0;
    while (e[n] && n < FAPI_ENVSIZE - 1)
        while (e[n++] && n < FAPI_ENVSIZE - 1)
            ;
    return n;
}

/* Eintrag zu name (Laenge nl, Grossbuchstaben) suchen -> Offset im Block */
static int env_find(int t, const char *name, unsigned nl)
{
    char __far *e = MK_FP(env_sel, env_off(t));
    unsigned i = 0, k;
    while (e[i]) {
        for (k = 0; k < nl && e[i + k] == name[k]; k++)
            ;
        if (k == nl && e[i + k] == '=')
            return (int)i;
        while (e[i++])
            ;
    }
    return -1;
}

/* "NAME=WERT" setzen; leerer Wert loescht. 0 = ok, -1 Format, -2 voll */
static int env_set(int t, const char *entry)
{
    static char name[64];
    char __far *e = MK_FP(env_sel, env_off(t));
    unsigned nl, used, len, i;
    int at;

    for (nl = 0; entry[nl] && entry[nl] != '=' && nl < sizeof(name) - 1; nl++)
        name[nl] = up(entry[nl]);
    while (nl > 0 && name[nl - 1] == ' ')
        nl--;
    if (nl == 0 || entry[nl] != '=' && entry[nl] != ' ')
        return -1;
    while (*entry && *entry != '=')
        entry++;
    if (!*entry)
        return -1;
    entry++;
    at = env_find(t, name, nl);
    used = env_used(t);
    if (at >= 0) {                      /* alten Eintrag entfernen       */
        for (len = 0; e[at + len]; len++)
            ;
        len++;
        for (i = (unsigned)at; i + len <= used; i++)
            e[i] = e[i + len];
        used -= len;
        e[used] = 0;
    }
    for (len = 0; entry[len]; len++)
        ;
    if (len == 0)
        return 0;                       /* nur loeschen                  */
    if (used + nl + 1 + len + 2 > FAPI_ENVSIZE)
        return -2;
    for (i = 0; i < nl; i++)
        e[used++] = name[i];
    e[used++] = '=';
    for (i = 0; i < len; i++)
        e[used++] = entry[i];
    e[used++] = 0;
    e[used] = 0;
    return 0;
}

/* neuen Task anlegen und laden. Laufwerk und Verzeichnis erbt er von
   'from' (oder A:\ bei from < 0). -> Tasknummer oder -(EXEC_ERR_*)    */
static void of_ref(int k);
static void of_release(int k);

static int task_create(const char *cmd, int session, int parent, int from)
{
    static char path[IFS_PATHLEN], canon[IFS_PATHLEN];
    int t, err, d, i;

    for (t = 0; t < NTASK; t++)
        if (task[t].state == T_FREE)
            break;
    if (t == NTASK)
        return -EXEC_ERR_NOSLOT;
    if (split_cmd(cmd, path, task[t].name, task[t].args) ||
        resolve(from, path, &d, canon))
        return -EXEC_ERR_NOTFOUND;
    err = load_program(t, d, canon);
    if (err)
        return -err;
    task[t].drive = from >= 0 ? task[from].drive : (unsigned char)sys_drive;
    far_copy(env_sel, env_off(t), env_sel, env_off(from), FAPI_ENVSIZE);
    if (from >= 0) {
        scopy(task[t].cwd, task[from].cwd, IFS_PATHLEN);
        if (subst_sel)
            far_copy(subst_sel, DCWD_OFF + (unsigned)t * NDCWD * DCWD_SZ, subst_sel,
                     DCWD_OFF + (unsigned)from * NDCWD * DCWD_SZ, NDCWD * DCWD_SZ);
    } else {
        task[t].cwd[0] = 0;
        dcwd_clear(t);
    }
    task[t].session = session;
    task[t].parent  = parent;
    task[t].wake    = 0;
    task[t].sig_tramp = task[t].sig_mask = 0;
    task[t].sig_pending = task[t].in_sig = 0;
    task[t].bg = 0;
    task[t].rc = 0;
    task[t].child_rc = 0;
    task[t].hold = 0;
    task[t].want_lock = 0;
    for (i = 0; i < 3; i++) {                   /* Standard-Handles erben */
        task[t].std[i] = from >= 0 ? task[from].std[i] : CONS;
        of_ref(task[t].std[i]);
    }
    task[t].state   = T_READY;
    return t;
}

/* ---- Task-Ende --------------------------------------------------------- */
static void task_end(int t)
{
    unsigned idx;
    int p = task[t].parent, s = task[t].session, i;

    fpu_task_end(t);
    aud_task_end(t);
    for (idx = LDT_FIRST_FREE; idx < NLDT; idx++)
        if (ldt[t][idx].access != 0)
            seg_free(t, USEL(idx));
    if (task[t].img_unit) {
        mem_free(task[t].img_unit, task[t].img_cnt);
        image_units -= task[t].img_cnt;
        task[t].img_unit = 0;
    }
    if (task[t].code_unit) {                /* Code: nur der letzte gibt frei */
        for (i = 0; i < NTASK; i++)
            if (i != t && task[i].state != T_FREE &&
                task[i].code_unit == task[t].code_unit)
                break;
        if (i == NTASK) {
            mem_free(task[t].code_unit, task[t].code_cnt);
            image_units -= task[t].code_cnt;
        }
        task[t].code_unit = 0;
    }
    for (i = 0; i < (int)NFILE; i++)       /* eigene Handles schliessen */
        if (files[i].refs && files[i].owner == t) {
            files[i].owner = -1;
            of_release(i);                  /* Datei: Groesse ins Verzeichnis */
        }
    for (i = 0; i < 3; i++) {               /* Standard-Handles freigeben */
        of_release(task[t].std[i]);
        task[t].std[i] = CONS;
    }
    for (i = 0; i < NTASK; i++)
        if (task[i].state != T_FREE && task[i].parent == t)
            task[i].parent = -1;
    task[t].state = T_FREE;
    vio_task_end(s, t);                     /* Vollbild zuruecknehmen   */

    if (p >= 0)
        task[p].child_rc = task[t].rc;      /* fuer ERRORLEVEL          */
    if (task[t].bg)
        ;                                   /* Hintergrund (Pipe): niemand wartet */
    else if (p >= 0 && task[p].state == T_WAIT)
        task[p].state = T_READY;            /* Aufrufer macht weiter     */
    else if (p < 0) {
        /* Sitzungsinhaber beendet: Sitzung schliessen */
        for (i = 0; i < NTASK; i++)
            if (task[i].state != T_FREE && task[i].session == s)
                break;
        if (i == NTASK)
            vio_close(s);
    }
}

/* ---- 287 je Task ----------------------------------------------------------
   Wie OS/2 1.x: der Zustand wird erst getauscht, wenn ein anderer Task
   wirklich einen 287-Befehl ausfuehrt. Der 286 setzt bei jedem
   Hardware-Taskwechsel MSW.TS, der naechste 287-Befehl loest #NM aus
   (start.asm exc7, mit MP auch WAIT). fpu_owner: wessen Zustand gerade
   im 287 steht. */
#define FPU_STATE 94                    /* FNSAVE im 16-Bit-Format        */
static unsigned char fpu_area[NTASK][FPU_STATE];
static int fpu_owner = -1;
static unsigned char fpu_used;          /* Bit t: Task t hat einen Zustand */

int fpu_nm(void)                        /* aus exc7, TS ist schon geloescht */
{
    if (!fpu_ok || current < 0)
        return 1;                       /* kein 287: Programm abbrechen  */
    if (fpu_owner == current)
        return 0;
    if (fpu_owner >= 0)
        fpu_save(KDATA, (unsigned)fpu_area[fpu_owner]);
    if (fpu_used & (1u << current))
        fpu_restore(KDATA, (unsigned)fpu_area[current]);
    else {
        fpu_reset();                    /* frischer 287 (FNINIT) ...      */
        __asm { clts }                  /* ... TS wieder weg: wir sind es */
        fpu_used |= 1u << current;
    }
    fpu_owner = current;
    return 0;
}

static void fpu_task_end(int t)
{
    fpu_used &= ~(1u << t);
    if (fpu_owner == t) {               /* Rest (auch Fehler) wegwerfen  */
        fpu_owner = -1;
        if (fpu_ok)
            fpu_reset();
    }
}

/* ---- Scheduler (kooperativ) -------------------------------------------- */
static void switch_to(int t)
{
    current = t;
    task_jump(TSS_SEL(t));          /* Hardware-Taskwechsel ueber das TSS */
}

/* Strg+C: aktives Programm der Vordergrund-Sitzung beenden. Der Inhaber
   der ersten Sitzung (die Shell aus PROTSHELL=) ist geschuetzt. */
static int klock;
static unsigned klock_depth;

static void do_break(void)
{
    int s = vio_fg(), t;
    for (t = 0; t < NTASK; t++)
        if ((task[t].state == T_READY || task[t].state == T_BLOCK) &&
            task[t].session == s)
            break;
    if (t == NTASK)
        return;
    if (klock >= 0) {                           /* Plattenoperation laeuft: */
        kbd_break_req = 1;                      /* spaeter nochmal          */
        return;
    }
    if (task[t].sig_mask & SIG_BREAK) {         /* Programm faengt Strg+C */
        task[t].sig_pending |= SIG_BREAK;
        task[t].wake = 0;
        return;
    }
    if (task[t].parent < 0 && s == 0)
        return;
    vio_kputs(s, "^C\n");
    task[t].rc = 255;
    if (current >= 0) {                         /* Dateien schliessen: Sperre */
        klock = current;
        klock_depth = 1;
    }
    task_end(t);
    if (current >= 0)
        klock = -1;
}

/* Beginn der laufenden Zeitscheibe. Kooperativ heisst bei AT-DOS: Ein
   Programm gibt die CPU an FAPI-Aufrufen ab. Ist seine Scheibe (SLICE
   Takte) um oder wartet Strg+C/Alt+Esc, kommen beim Ruecksprung aus der
   FAPI die anderen dran - so legt GREP -R ueber eine grosse Platte das
   System nicht lahm. Wer rechnet, ohne die FAPI zu rufen, haelt es weiter
   an (praeemptiv erst ab DOS 6.0). */
#define SLICE 5                         /* 50 ms                         */
static unsigned long slice_start;

/* ---- Kernelsperre -------------------------------------------------------
   Seit Treiber per ProcBlock schlafen, kann mitten in einer Dateisystem-
   oder Plattenoperation ein anderer Task laufen. DMABUF, FAT- und
   Sektor-Cache, die Anforderung 'req', die Fenster SCRATCH/SCRATCH2 und die
   statischen Puffer von fapi_inner sind aber gemeinsam. Deshalb haelt ein
   Task die Sperre fuer jeden FAPI-Aufruf ausser einer Liste harmloser
   (fapi_nolock). An jedem freiwilligen Wartepunkt (schedule_next) gibt er
   sie ab und bekommt sie vom Scheduler zurueck, bevor er weiterlaeuft -
   nur ProcBlock behaelt sie: wer im Treiber schlaeft, bleibt Herr ueber
   die Geraete. Andere Tasks rechnen weiter und duerfen Bildschirm,
   Tastatur, Umgebung usw. benutzen. Vor dem Start der Tasks (current < 0)
   gibt es keine Sperre. */
static int klock = -1;                  /* Inhaber, -1 = frei            */
static unsigned klock_depth;
static volatile unsigned char run_flag; /* ProcRun hat jemanden geweckt  */
static volatile unsigned char resched_req;  /* ... beim naechsten FAPI-Ende wechseln */

static void sched(int keep);

static void klock_enter(void)
{
    if (current < 0)
        return;
    if (klock == current) {
        klock_depth++;
        return;
    }
    if (klock < 0) {
        klock = current;
        klock_depth = 1;
        return;
    }
    task[current].want_lock = 1;        /* Scheduler gibt sie uns        */
    task[current].lock_depth = 0;
    task[current].wake = 0;
    sched(0);
}

static void klock_leave(void)
{
    if (current < 0 || klock != current)
        return;
    if (--klock_depth == 0)
        klock = -1;
}

static void schedule_next(void)
{
    sched(0);
}

/* keep = 1: Sperre behalten (ProcBlock) */
static void sched(int keep)
{
    int n, t, any;
    unsigned long now;
    unsigned f;
    struct task *tk;

    resched_req = 0;
    if (!keep && current >= 0 && klock == current) {
        task[current].want_lock = 1;    /* abgeben, spaeter zurueck      */
        task[current].lock_depth = (unsigned char)klock_depth;
        klock = -1;
    }
    for (;;) {
        run_flag = 0;
        if (kbd_switch_req) {
            kbd_switch_req = 0;
            vio_next();
        }
        if (kbd_break_req) {
            kbd_break_req = 0;
            do_break();
        }
        if (reboot_count && klock < 0)  /* nicht mitten in einer Plattenoperation */
            shutdown();
        now = get_ticks();
        fd_tick();                      /* Diskettenmotoren aus          */
        vio_flush();                    /* Programm wartet: Bild zeigen  */
        vio_status();
        any = 0;
        for (n = 1; n <= NTASK; n++) {
            t = (current + n + NTASK) % NTASK;
            tk = &task[t];
            if (tk->state == T_FREE)
                continue;
            any = 1;
            if (tk->state == T_BLOCK && tk->block_until && now >= tk->block_until) {
                f = irq_off();
                if (tk->state == T_BLOCK) {     /* Frist um              */
                    tk->block_rc = 1;
                    tk->state = T_READY;
                }
                irq_restore(f);
            }
            if (tk->state != T_READY || tk->wake > now)
                continue;
            if (tk->want_lock && klock >= 0 && klock != t)
                continue;               /* wartet auf die Kernelsperre   */
            if (t != current)
                switch_to(t);
            /* hier laeuft der gewaehlte Task weiter */
            tk = &task[current];
            if (tk->want_lock) {
                tk->want_lock = 0;
                klock = current;
                klock_depth = tk->lock_depth ? tk->lock_depth : 1;
            }
            slice_start = get_ticks();  /* (wieder) dran: neue Zeitscheibe */
            return;
        }
        if (!any)
            panic("Alle Programme beendet - System angehalten.");
        vio_idle();                     /* Grafikzeiger im Ruecklauf     */
        f = irq_off();                  /* ohne Luecke zwischen Pruefen  */
        if (!run_flag)                  /* und HLT                        */
            idle_wait();                /* alle warten: HLT bis zum Takt  */
        irq_restore(f);
    }
}

/* DevHlp ProcBlock: mit gesperrten Interrupts gerufen, nachdem der Treiber
   seine Bedingung geprueft hat - so geht kein Wecken verloren. Vor dem
   Start der Tasks wartet der Kernel selbst mit HLT. -> 0 geweckt, 1 Frist */
static unsigned long kblock_id;
static volatile unsigned char kblock_on;

unsigned proc_block(unsigned long id, unsigned timeout)
{
    unsigned long until = timeout ? get_ticks() + timeout : 0;
    struct task *tk;

    if (current < 0) {
        kblock_id = id;
        kblock_on = 1;
        for (;;) {
            irq_off();
            if (!kblock_on)
                break;
            if (until && get_ticks() >= until) {
                kblock_on = 0;
                enable_ints();
                return 1;
            }
            idle_wait();                /* STI; HLT                      */
        }
        enable_ints();
        return 0;
    }
    tk = &task[current];
    tk->block_id = id;
    tk->block_until = until;
    tk->block_rc = 0;
    tk->state = T_BLOCK;
    sched(1);
    enable_ints();
    return tk->block_rc;
}

/* Diskette (IRQ 6, start.asm): wartenden Task wecken */
void fdc_isr(void)
{
    proc_run(FD_EVENT);
}

/* DevHlp ProcRun, auch aus Interruptroutinen: nur Zustaende setzen */
unsigned proc_run(unsigned long id)
{
    unsigned n = 0, f = irq_off();
    int t;

    for (t = 0; t < NTASK; t++)
        if (task[t].state == T_BLOCK && task[t].block_id == id) {
            task[t].block_rc = 0;
            task[t].state = T_READY;
            n++;
        }
    if (kblock_on && kblock_id == id) {
        kblock_on = 0;
        n++;
    }
    if (n) {
        run_flag = 1;
        resched_req = 1;
    }
    irq_restore(f);
    return n;
}

static int bc_due(void);
static void bc_flush(void);
static void bc_regrow(void);
static void cache_info(struct fapi_cacheinfo __far *ci);
static void sleep_ticks(unsigned n)
{
    if (klock < 0 || klock == current) {
        klock_enter();                  /* Leerlauf: verzoegerte Daten   */
        if (bc_due())                   /* schreiben, wenn Frist um;     */
            bc_flush();
        bc_regrow();                    /* abgegebene Stuecke zurueck    */
        klock_leave();
    }
    task[current].wake = get_ticks() + n;
    schedule_next();
}

/* ---- Zeiger aus Ring 3 pruefen ----------------------------------------- */
static int user_ok(unsigned sel, unsigned off, unsigned len, int write)
{
    unsigned lim;
    if (write ? !sel_writable(sel) : !sel_readable(sel))
        return 0;
    if (len == 0)
        return 1;
    lim = sel_limit(sel);
    return off <= lim && len - 1 <= lim - off;
}

/* ---- Dateien in Benutzung ---------------------------------------------- */
static int scmp(const char __far *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a - *b;
}

/* a liegt in b oder ist b ("\\DOS\\X" in "\\DOS"); "\\" enthaelt alles */
static int path_in(const char __far *a, const char *b)
{
    int i;
    if (b[0] == '\\' && b[1] == 0)
        return 1;
    for (i = 0; b[i]; i++)
        if (up(a[i]) != up(b[i]))           /* cwd in Originalschreibung */
            return 0;
    return a[i] == 0 || a[i] == '\\';
}

/* offene Datei oder aktuelles Verzeichnis eines Tasks in canon? */
static int in_use(int d, const char *canon, int dirs)
{
    int k;
    for (k = 0; k < NFILE; k++)
        if (files[k].refs && files[k].kind == OF_FILE && files[k].f.drive == d &&
            (dirs ? path_in(files[k].path, canon) : scmp(files[k].path, canon) == 0))
            return 1;
    if (dirs)
        for (k = 0; k < NTASK; k++)
            if (task[k].state != T_FREE) {
                char __far *oc;
                if (task[k].drive == d && task[k].cwd[0] && path_in(task[k].cwd, canon))
                    return 1;
                if (task[k].drive != d && (oc = dcwd(k, d, 0)) != 0 && oc[0] &&
                    path_in(oc, canon))
                    return 1;
            }
    return 0;
}

/* zum Schreiben offen (anderes Handle)? Wie OS/2 im Normalfall: lesen
   duerfen viele, aber nicht, waehrend jemand schreibt - sonst sieht der
   Leser eine halbe Datei (FC waehrend XCOPY in einer anderen Sitzung) */
static int open_for_write(int d, const char *canon)
{
    int k;
    for (k = 0; k < NFILE; k++)
        if (files[k].refs && files[k].kind == OF_FILE && files[k].f.drive == d &&
            (files[k].f.flags & IFS_F_WRITE) && scmp(files[k].path, canon) == 0)
            return 1;
    return 0;
}

static unsigned ifs_err(int rc)
{
    switch (rc) {
    case IFS_WP:     return FAPI_ERR_WP;
    case IFS_FULL:   return FAPI_ERR_FULL;
    case IFS_EXIST:  return FAPI_ERR_EXIST;
    case IFS_DENIED: return FAPI_ERR_DENIED;
    }
    return FAPI_ERROR;
}

/* ---- FAPI-Verteiler ---------------------------------------------------- */
/* ---- Handles, Konsole, Pipes -------------------------------------------
   Eintraege der Dateitabelle zaehlen Verweise: das Handle des Besitzers und
   jedes Standard-Handle (0-2) eines Programms, das darauf zeigt. Kinder
   erben die Standard-Handles. Freigegeben wird bei 0 Verweisen. */
#define NPIPE   4
#define PIPESZ  2048
static struct pipe {
    unsigned       head, count;
    unsigned char  used, rd, wr;        /* Eintraege der beiden Enden    */
} pipes[NPIPE];
static unsigned pipe_sel;

static int of_alloc(void)
{
    unsigned k;
    for (k = 1; k < NFILE; k++)
        if (!files[k].refs) {
            files[k].refs = 1;
            files[k].owner = (signed char)current;
            files[k].kind = OF_FILE;
            files[k].pos = 0;
            files[k].path[0] = 0;
            return (int)k;
        }
    return -1;
}

static void of_release(int k)
{
    struct ofile __far *o = &files[k];
    if (k == CONS || !o->refs)
        return;
    if (--o->refs)
        return;
    o->owner = -1;
    if (o->kind == OF_FILE)
        ifs_close(&o->f);
    else if (o->kind == OF_DASD && o->f.attr == DASD_DISK) {
        if (o->pipe) {                  /* ganze Platte: alle Laufwerke  */
            int d;                      /* darauf frei und neu einlesen  */
            blk_flush();
            for (d = 0; d < NDRIVE; d++)
                if (vol_present(d) && vol[d].blk == o->f.drive && vol[d].locked) {
                    vol[d].locked = 0;
                    vol_reset(d);
                }
        }
    } else if (o->kind == OF_DASD) {
        if (o->pipe) {                  /* Sperre aufheben, neu einlesen */
            unsigned char was = vol[o->f.drive].in_use;
            vol[o->f.drive].locked = 0;
            blk_flush();
            vol_reset(o->f.drive);
            if (was)                    /* Sauber-Bit ist geloescht: beim */
                vol[o->f.drive].in_use = 1; /* Herunterfahren wieder setzen */
        }
    } else if ((o->kind == OF_PRD && !files[pipes[o->pipe].wr].refs) ||
             (o->kind == OF_PWR && !files[pipes[o->pipe].rd].refs))
        pipes[o->pipe].used = 0;        /* beide Enden zu                */
}

static void of_ref(int k)
{
    if (k != CONS)
        files[k].refs++;
}

static const char *drv_name(int d);

/* Geometrie des BIOS fuer Blockgeraet want (0/0 = unbekannt). Zuordnung
   ueber die Groesse: BIOS-Platten in der Reihenfolge der eigenen Platten,
   jeweils die, deren Zylinder x Koepfe x Sektoren am knappsten unter der
   echten Groesse liegt (hoechstens 4 Zylinder; bei 1024 Zylindern hat das
   BIOS gekappt, dann zaehlt nur, dass sie kleiner ist) */
extern unsigned char bios_geo_h[4], bios_geo_s[4];
extern unsigned short bios_geo_c[4];

static void disk_geo(int want, unsigned *h, unsigned *s)
{
    unsigned char used[4];
    unsigned long cap, diff, bd;
    unsigned i, k, hs;
    int dev, best;

    for (i = 0; i < 4; i++)
        used[i] = 0;
    *h = *s = 0;
    for (dev = 1; dev < nblk; dev++) {
        if (blk[dev].bsize != 512)
            continue;
        best = -1;
        bd = 0xFFFFFFFFUL;
        for (i = 0; i < 4 && i < bios_hdcount; i++) {
            if (used[i] || !bios_geo_s[i])
                continue;
            hs = (bios_geo_h[i] + 1) * bios_geo_s[i];
            for (cap = 0, k = 0; k < bios_geo_c[i]; k++)    /* ohne __U4M */
                cap += hs;
            if (cap > blk[dev].blocks)
                continue;
            diff = blk[dev].blocks - cap;
            if ((diff <= ((unsigned long)hs << 2) || bios_geo_c[i] >= 1024) && diff < bd) {
                bd = diff;
                best = (int)i;
            }
        }
        if (best < 0)
            continue;
        used[best] = 1;
        if (dev == want) {
            *h = bios_geo_h[best] + 1;
            *s = bios_geo_s[best];
            return;
        }
    }
}

/* Platte Nummer n (0 = erste Festplatte; Diskette und CD zaehlen nicht)
   -> Blockgeraet oder -1 */
static int disk_blk(unsigned n)
{
    int dev;
    for (dev = 1; dev < nblk; dev++)
        if (blk[dev].bsize == 512 && !IS_FLOPPY(dev) && n-- == 0)   /* B: nicht */
            return dev;
    return -1;
}

/* DosRead/DosWrite auf ein ganzes Laufwerk: ganze Sektoren ab pos */
static int dasd_rw(int k, unsigned sel, unsigned off, unsigned len, int write)
{
    struct ofile __far *o = &files[k];
    int d = o->f.drive, whole = o->f.attr == DASD_DISK;
    int dev = whole ? d : vol[d].blk;
    unsigned bs = blk[dev].bsize, done = 0, n;
    unsigned long lba, total, start = whole ? 0 : vol[d].start;

    if (write && !o->pipe)
        return -1;                      /* nur mit Sperre             */
    len &= ~(bs - 1);
    if ((unsigned)o->pos & (bs - 1))
        return -1;
    lba = o->pos >> (bs == 2048 ? 11 : 9);
    total = whole ? blk[dev].blocks : vol[d].psize ? vol[d].psize : blk[dev].blocks;
    while (done < len && lba < total) {
        if (write) {
            far_copy(DMABUF, 0, sel, off + done, bs);
            if (blk_write(dev, start + lba))
                return done ? (int)done : -1;
        } else {
            n = (len - done) / bs;      /* Rest am Stueck vorauslesen  */
            if (n > total - lba)
                n = (unsigned)(total - lba);
            if (blk_read_n(dev, start + lba, n))
                return done ? (int)done : -1;
            far_copy(sel, off + done, DMABUF, 0, bs);
        }
        done += bs;
        lba++;
    }
    if (write && IS_FLOPPY(dev) && fd_sync())
        return -1;                      /* Spurpuffer nicht geschrieben */
    o->pos += done;
    return (int)done;
}

/* Handle des laufenden Programms -> Eintrag, -1 = ungueltig */
static int h2e(unsigned h)
{
    if (h < HBASE)
        return task[current].std[h];
    h -= HBASE;
    if (h >= NFILE || !files[h].refs || files[h].owner != current)
        return -1;
    return (int)h;
}

static void yield(void)
{
    task[current].wake = 0;
    schedule_next();
}

/* Geraetenamen wie bei MS-DOS (in jedem Verzeichnis, Endung egal):
   PRN/LPT1 -> 1, LPT2 -> 2, LPT3 -> 3, NUL -> 9, sonst 0 */
static int dev_name(const char *canon)
{
    const char *b = canon, *p;
    for (p = canon; *p; p++)
        if (*p == '\\')
            b = p + 1;
    if (b[0] == 'P' && b[1] == 'R' && b[2] == 'N' && (!b[3] || b[3] == '.'))
        return 1;
    if (b[0] == 'N' && b[1] == 'U' && b[2] == 'L' && (!b[3] || b[3] == '.'))
        return 9;
    if (b[0] == 'L' && b[1] == 'P' && b[2] == 'T' && b[3] >= '1' && b[3] <= '3' &&
        (!b[4] || b[4] == '.'))
        return b[3] - '0';
    if (b[0] == 'A' && b[1] == 'U' && b[2] == 'X' && (!b[3] || b[3] == '.'))
        return 11;                          /* AUX = COM1                */
    if (b[0] == 'C' && b[1] == 'O' && b[2] == 'M' && b[3] >= '1' && b[3] <= '4' &&
        (!b[4] || b[4] == '.'))
        return 10 + (b[3] - '0');           /* COM1-4 -> 11-14           */
    return 0;
}

extern unsigned short irq_hcs[16];
static unsigned char lpt_retry[3], com_retry[4];

/* COM-Port n (0-3) von einem Treiber belegt (IRQ 4 bzw. 3 vergeben)? */
static int com_busy(unsigned n)
{
    return irq_hcs[(n & 1) ? 3 : 4] != 0;
}

/* Abfragebetrieb, wie INT 14h: senden, sobald das Halteregister leer ist
   (LSR Bit 5), empfangen, was da ist (LSR Bit 0). Warten mit yield();
   Senden gibt nach 30 s auf (MODE ... ,P: nie), Empfangen wartet bis 5 s
   auf das erste Zeichen. */
static int com_io(int k, unsigned sel, unsigned off, unsigned len, int write)
{
    unsigned n = files[k].f.drive, base = bios_ports[n], i, spin;
    unsigned char __far *p;
    unsigned long t0;

    for (i = 0; i < len; i++) {
        t0 = get_ticks();
        for (spin = 0; !(inb(base + 5) & (write ? 0x20 : 0x01)); spin++)
            if (spin > 100) {
                if (!write && i)
                    return (int)i;          /* gelesen, was da war       */
                if (get_ticks() - t0 > (write ? 3000UL : 500UL) &&
                    (!write || !com_retry[n]))
                    return i ? (int)i : (write ? -1 : 0);
                yield();
                spin = 0;
            }
        p = MK_FP(sel, off);
        if (write)
            outb(base, p[i]);
        else
            p[i] = (unsigned char)inb(base);
    }
    return (int)len;
}

static unsigned dev_open(int dn)
{
    int k;
    if (dn > 10) {                          /* COM1-4                    */
        if (!bios_ports[dn - 11])
            return FAPI_ERROR;
        if (com_busy(dn - 11))
            return FAPI_ERR_INUSE;          /* z. B. serielle Maus       */
    } else if (dn != 9 && !bios_ports[3 + dn])
        return FAPI_ERROR;                  /* kein solcher Anschluss    */
    k = of_alloc();
    if (k < 0)
        return FAPI_ERROR;
    files[k].kind = dn > 10 ? OF_COM : dn == 9 ? OF_NUL : OF_LPT;
    files[k].f.drive = (unsigned char)(dn > 10 ? dn - 11 : dn - 1);
    files[k].pos = 0;
    files[k].path[0] = 0;
    return k + HBASE;
}

/* Zeichen an den Drucker (Parallelport, Abfragebetrieb): Status Bit 7 =
   nicht BUSY, Bit 5 Papierende, Bit 3 = 0 Fehler/aus; Strobe ueber
   Steuerregister Bit 0. Waehrend BUSY laufen andere Tasks weiter; nach
   30 s ohne Fortschritt -> Fehler. -> Bytes, -1 = nicht bereit */
static int lpt_write(int k, unsigned sel, unsigned off, unsigned len)
{
    unsigned base = bios_ports[4 + files[k].f.drive], i, n, st;
    unsigned char __far *p;
    unsigned long t0;

    for (i = 0; i < len; i++) {
        t0 = get_ticks();
        for (n = 0; !((st = inb(base + 1)) & 0x80); n++)
            if (n > 100) {                  /* belegt: andere vorlassen  */
                if (get_ticks() - t0 > 3000 && !lpt_retry[files[k].f.drive])
                    return i ? (int)i : -1;     /* MODE LPTn ,,P: endlos */
                yield();
                n = 0;
            }
        if ((st & 0x20) || !(st & 0x08))
            return i ? (int)i : -1;         /* Papierende, Fehler, aus   */
        p = MK_FP(sel, off);
        outb(base, p[i]);
        outb(base + 2, 0x0D);               /* Strobe (Init aus, Select) */
        outb(base + 2, 0x0C);
    }
    return (int)len;
}

static int sig_waiting(void)
{
    return (task[current].sig_pending & task[current].sig_mask) != 0;
}

/* Zeile von der Tastatur (mit Echo), wie DOS beim Lesen von der Konsole */
static int con_read(unsigned sel, unsigned off, unsigned len)
{
    static char cl[130];
    int s = task[current].session, k;
    unsigned n = 0;

    for (;;) {
        if (vio_fg() != s || (k = kbd_get()) < 0) {
            if (sig_waiting())
                return 0;
            sleep_ticks(1);
            continue;
        }
        k &= 0xFF;
        if (k == 13) {
            cl[n++] = '\r';
            cl[n++] = '\n';
            vio_tty(s, KDATA, (unsigned)"\n", 1);
            break;
        }
        if (k == 26 && n == 0)
            return 0;                   /* Strg+Z: Dateiende            */
        if (k == 8) {
            if (n) {
                n--;
                vio_tty(s, KDATA, (unsigned)"\b \b", 3);
            }
        } else if (k >= 32 && n < sizeof(cl) - 2) {
            cl[n++] = (char)k;
            vio_tty(s, KDATA, (unsigned)&cl[n - 1], 1);
        }
    }
    if (n > len)
        n = len;
    far_copy(sel, off, KDATA, (unsigned)cl, n);
    return (int)n;
}

static int pipe_read(int k, unsigned sel, unsigned off, unsigned len)
{
    struct pipe *p = &pipes[files[k].pipe];
    unsigned n, first;

    while (!p->count) {
        if (!files[p->wr].refs || sig_waiting())
            return 0;                   /* kein Schreiber mehr: Ende     */
        yield();
    }
    n = len < p->count ? len : p->count;
    first = PIPESZ - p->head;
    if (first > n)
        first = n;
    far_copy(sel, off, pipe_sel, files[k].pipe * PIPESZ + p->head, first);
    if (n > first)
        far_copy(sel, off + first, pipe_sel, files[k].pipe * PIPESZ, n - first);
    p->head = (p->head + n) % PIPESZ;
    p->count -= n;
    return (int)n;
}

static int pipe_write(int k, unsigned sel, unsigned off, unsigned len)
{
    struct pipe *p = &pipes[files[k].pipe];
    unsigned done = 0, n, tail, first;

    while (done < len) {
        if (!files[p->rd].refs)
            return done ? (int)done : -1;   /* niemand liest mehr       */
        if (p->count == PIPESZ) {
            if (sig_waiting())
                return (int)done;
            yield();
            continue;
        }
        n = PIPESZ - p->count;
        if (n > len - done)
            n = len - done;
        tail = (p->head + p->count) % PIPESZ;
        first = PIPESZ - tail;
        if (first > n)
            first = n;
        far_copy(pipe_sel, files[k].pipe * PIPESZ + tail, sel, off + done, first);
        if (n > first)
            far_copy(pipe_sel, files[k].pipe * PIPESZ, sel, off + done + first, n - first);
        p->count += n;
        done += n;
    }
    return (int)done;
}

/* ---- Signale ----------------------------------------------------------
   Rahmen des laufenden FAPI-Aufrufs oben auf dem Kernelstack (start.asm,
   fapi_entry_): DI SI BP SP BX DX CX AX ES DS IP CS SP SS = 14 Worte. */
#define FRAME(t) (&kstack[t][KSTACK - 14])

static unsigned sig_deliver(unsigned r)
{
    struct task *tk;
    unsigned short *fr;
    unsigned sig, i;

    if (current < 0)
        return r;
    tk = &task[current];
    if (tk->state == T_FREE || tk->in_sig || !tk->sig_tramp)
        return r;
    sig = tk->sig_pending & tk->sig_mask;
    if (!sig)
        return r;
    sig = (sig & SIG_SHUTDOWN) ? SIG_SHUTDOWN : SIG_BREAK;
    tk->sig_pending &= (unsigned char)~sig;
    fr = FRAME(current);
    for (i = 0; i < 14; i++)
        tk->sig_save[i] = fr[i];
    tk->sig_save[14] = (unsigned short)r;
    tk->in_sig = 1;
    fr[10] = tk->sig_tramp;                     /* IP: Weiche            */
    fr[8] = fr[9] = USEL(1);                    /* ES, DS: Programmdaten */
    return sig;                                 /* AX = Signal           */
}

static unsigned fapi_inner(unsigned fn, unsigned usel, unsigned a1, unsigned a2,
                           unsigned a3);

/* Aufrufe, die weder Dateisystem noch Laufwerke, DMABUF, SCRATCH oder die
   statischen Puffer von fapi_inner beruehren: sie laufen auch, waehrend ein
   anderer Task in einem Treiber schlaeft. Alles andere nimmt die
   Kernelsperre (im Zweifel sperren). Lesen/Schreiben auf Konsole und Pipe
   gehoert dazu - sonst haelt ein GREP im Hintergrund jede Ausgabe auf. */
static int fapi_nolock(unsigned fn, unsigned a1)
{
    int k;
    switch (fn) {
    case FAPI_GETVERSION: case FAPI_VIOWRTSTR: case FAPI_VIOWRTSTRATT:
    case FAPI_VIOWRTTTY: case FAPI_VIOCLS: case FAPI_VIOSETMODE: case FAPI_VIOSETANSI:
    case FAPI_VIOSETLINES: case FAPI_VIOGETROWS: case FAPI_VIOSETCURPOS:
    case FAPI_KBDSHIFT: case FAPI_KBDCHARIN: case FAPI_KBDPEEK:
    case FAPI_KBDGETLAYOUT: case FAPI_MEMINFO: case FAPI_GETARGS:
    case FAPI_QCHILDRC: case FAPI_QCURDIR: case FAPI_QCURDISK:
    case FAPI_GETENV: case FAPI_SETENV: case FAPI_ENVENTRY:
    case FAPI_GETDATETIME: case FAPI_SETDATETIME: case FAPI_SETSIGHANDLER:
    case FAPI_SIGRETURN: case FAPI_YIELD: case FAPI_SLEEP:
    case FAPI_SHUTDOWNHOLD: case FAPI_MOUOPEN: case FAPI_MOUREADEVENT:
    case FAPI_MOUSHOWPTR: case FAPI_CLIPPUT: case FAPI_CLIPGET:
    case FAPI_VIOGETBUF: case FAPI_VIOSHOWBUF: case FAPI_VIOSETGFX:
    case FAPI_VIOGETPHYSBUF: case FAPI_VIOSETPAL: case FAPI_VIOSETPLANE:
    case FAPI_VIOSETPAGE: case FAPI_AUDIO:     /* Ton wartet auf die Karte */
    case FAPI_QTICKS:
        return 1;
    case FAPI_READ:
    case FAPI_WRITE:
        k = h2e(a1);
        return k >= 0 && (files[k].kind == OF_CON || files[k].kind == OF_PRD ||
                          files[k].kind == OF_PWR);
    }
    return 0;
}

unsigned fapi_dispatch(unsigned fn, unsigned usel, unsigned a1, unsigned a2)
{
    /* viertes Argument (DX) sofort sichern: fapi_dx ist global, und beim
       Warten auf die Kernelsperre laufen andere Tasks, deren FAPI-Aufrufe
       es ueberschreiben (so bekam DosWrite die Laenge eines anderen) */
    unsigned a3 = fapi_dx, r;
    ifs_notready = 0;
    if (fapi_nolock(fn, a1))
        r = fapi_inner(fn, usel, a1, a2, a3);
    else {
        klock_enter();
        r = fapi_inner(fn, usel, a1, a2, a3);
        if (bc_due())
            bc_flush();                 /* Frist um: verzoegerte Daten   */
        fd_sync();                      /* Spurpuffer der Diskette: nach */
        klock_leave();                  /* jedem Aufruf auf der Scheibe  */
    }
    if (ifs_notready && r >= FAPI_ERR_FIRST)
        r = FAPI_ERR_NOTREADY;          /* Diskettenlaufwerk leer        */
    if (kbd_break_req || kbd_switch_req || reboot_count || resched_req ||
        get_ticks() - slice_start >= SLICE) {
        task[current].wake = 0;
        schedule_next();                /* Zeitscheibe um: andere zuerst */
    }
    return sig_deliver(r);
}

static unsigned fapi_inner(unsigned fn, unsigned usel, unsigned a1, unsigned a2,
                           unsigned a3)
{
    int row, col, s = task[current].session, t, k;
    static char buf[FAPI_PATHLEN], canon[IFS_PATHLEN];
    static struct ifs_dirent de;
    int d;

    usel |= 3;                                  /* Aufrufer ist Ring 3  */
    switch (fn) {
    case FAPI_GETVERSION:
        return FAPI_VERSION;

    case FAPI_VIOWRTSTR:
    case FAPI_VIOWRTSTRATT:
        if (!sel_readable(usel))
            return FAPI_ERROR;
        row = a2 >> 8;
        col = a2 & 0xFF;
        if (row >= vio_rows(s) || col >= SCOLS)
            return FAPI_ERROR;
        vio_str(s, row, col, usel, a1, sel_limit(usel),
                fn == FAPI_VIOWRTSTR ? 0x07 : (unsigned char)a3);
        return 0;

    case FAPI_VIOWRTTTY:
        if (!user_ok(usel, a1, a2, 0))
            return FAPI_ERROR;
        vio_tty(s, usel, a1, a2);
        return 0;

    case FAPI_VIOCLS:
        vio_cls(s);
        return 0;

    case FAPI_VIOSETANSI:
        return (unsigned)vio_ansi(s, a1);

    case FAPI_VIOSETMODE:
        vio_setmode(s, a1 != 0, current);
        return vio_rows(s);

    case FAPI_VIOSETLINES:
        k = vio_setlines(s, (int)a1);
        return k < 0 ? FAPI_ERROR : (unsigned)k;

    case FAPI_VIOGETROWS:
        return vio_rows(s);

    case FAPI_SHUTDOWNHOLD:                     /* nur waehrend SHUTDOWN */
        if (!going_down || !(task[current].sig_mask & SIG_SHUTDOWN))
            return FAPI_ERROR;
        task[current].hold = 1;
        vio_focus(s);
        return 0;

    case FAPI_VIOSETCURPOS:
        vio_setcur(s, a1 >> 8, a1 & 0xFF);
        return 0;

    case FAPI_KBDSHIFT:
        return kbd_shift();

    case FAPI_KBDCHARIN:
        for (;;) {
            if (vio_fg() == s && (k = kbd_get()) >= 0)
                return k;
            if (task[current].sig_pending & task[current].sig_mask)
                return 0;                       /* Signal: Zustellung    */
            sleep_ticks(1);                     /* bis zur naechsten Taste */
        }

    case FAPI_KBDPEEK:
        if (vio_fg() == s && (k = kbd_peek()) >= 0)
            return k;
        return 0;

    case FAPI_MEMINFO: {
        unsigned short __far *p;
        if (!user_ok(usel, a1, 10, 1))
            return FAPI_ERROR;
        p = MK_FP(usel, a1);
        p[0] = mem_conv_kb + mem_ext_kb;
        p[1] = mem_free_kb();
        p[2] = (unsigned)((KDATA_LIN + kdata_size - KCODE_LIN + 1023) >> 10);
        p[3] = image_units / 4;
        p[4] = alloc_units / 4;
        return 0;
    }

    case FAPI_ALLOCSEG:
        return seg_alloc(current, a1);

    case FAPI_FREESEG:
        return seg_free(current, a1);

    case FAPI_EXECPGM:
        if (user_str(usel, a1, buf, sizeof(buf)))
            return EXEC_ERR_NOTFOUND;
        if (a2 == EXEC_ASYNC) {
            int ns = vio_open("");
            if (ns < 0)
                return EXEC_ERR_NOMEM;
            k = vio_setlines(s, 0);             /* Zeilenzahl erben     */
            if (k > 25)
                vio_setlines(ns, k);
            t = task_create(buf, ns, -1, current);
            if (t < 0) {
                vio_close(ns);
                return -t;
            }
            vio_title(ns, task[t].name);
            vio_focus(ns);
            return 0;
        }
        t = task_create(buf, s, current, current);
        if (t < 0)
            return -t;
        if (a2 == EXEC_BG) {                    /* Pipe: nicht warten    */
            task[t].bg = 1;
            return 0;
        }
        task[current].state = T_WAIT;           /* bis das Kind endet   */
        schedule_next();
        return 0;

    case FAPI_GETARGS:
        if (!user_ok(usel, a1, FAPI_ARGLEN, 1))
            return FAPI_ERROR;
        far_copy(usel, a1, KDATA, (unsigned)task[current].args, FAPI_ARGLEN);
        return 0;

    case FAPI_OPEN:
        if (user_str(usel, a1, buf, sizeof(buf)) ||
            resolve(current, buf, &d, canon))
            return FAPI_ERROR;
        if ((t = dev_name(canon)) != 0)
            return dev_open(t);
        if (open_for_write(d, canon))
            return FAPI_ERR_INUSE;
        k = of_alloc();
        if (k < 0)
            return FAPI_ERROR;
        if (ifs_open(d, canon, &files[k].f)) {
            files[k].refs = 0;
            files[k].owner = -1;
            return FAPI_ERROR;
        }
        far_copy(FP_SEG(files), FP_OFF(files[k].path), KDATA, (unsigned)canon, IFS_PATHLEN);
        return k + HBASE;

    case FAPI_CREATE:
        if (user_str(usel, a1, buf, sizeof(buf)) ||
            resolve(current, buf, &d, canon))
            return FAPI_ERROR;
        if ((t = dev_name(canon)) != 0)
            return dev_open(t);
        if (in_use(d, canon, 0))
            return FAPI_ERR_INUSE;
        k = of_alloc();
        if (k < 0)
            return FAPI_ERROR;
        if (a2 == 1 && ifs_open(d, canon, &files[k].f) == 0) {
            if (files[k].f.attr & (ATTR_RO | ATTR_DIR)) {   /* anhaengen (>>) */
                files[k].refs = 0;
                files[k].owner = -1;
                return FAPI_ERR_DENIED;
            }
            files[k].f.flags |= IFS_F_WRITE;
            files[k].pos = files[k].f.size;
        } else if (set_orig(buf), t = ifs_create(d, canon, &files[k].f), ifs_orig[0] = 0, t != 0) {
            files[k].refs = 0;
            files[k].owner = -1;
            return ifs_err(t);
        }
        far_copy(FP_SEG(files), FP_OFF(files[k].path), KDATA, (unsigned)canon, IFS_PATHLEN);
        return k + HBASE;

    case FAPI_WRITE:
        if ((k = h2e(a1)) < 0)
            return FAPI_ERROR;
        if (a3 > 0x7FFF)
            a3 = 0x7FFF;
        if (!user_ok(usel, a2, a3, 0))
            return FAPI_ERROR;
        switch (files[k].kind) {
        case OF_CON:
            vio_tty(s, usel, a2, a3);
            return a3;
        case OF_PWR:
            t = pipe_write(k, usel, a2, a3);
            return t < 0 ? FAPI_ERROR : (unsigned)t;
        case OF_PRD:
            return FAPI_ERROR;
        case OF_DASD:
            t = dasd_rw(k, usel, a2, a3, 1);
            return t < 0 ? FAPI_ERROR : (unsigned)t;
        case OF_NUL:
            return a3;
        case OF_LPT:
            t = lpt_write(k, usel, a2, a3);
            return t < 0 ? FAPI_ERR_NOTREADY : (unsigned)t;
        case OF_COM:
            t = com_io(k, usel, a2, a3, 1);
            return t < 0 ? FAPI_ERR_NOTREADY : (unsigned)t;
        }
        t = ifs_write(&files[k].f, files[k].pos, usel, a2, a3);
        if (t < 0)
            return ifs_err(t);
        files[k].pos += t;
        return t;

    case FAPI_DELETE:
    case FAPI_MKDIR:
    case FAPI_RMDIR:
        if (user_str(usel, a1, buf, sizeof(buf)) ||
            resolve(current, buf, &d, canon))
            return FAPI_ERROR;
        if (fn != FAPI_MKDIR && in_use(d, canon, fn == FAPI_RMDIR))
            return FAPI_ERR_INUSE;
        t = fn == FAPI_DELETE ? ifs_remove(d, canon) :
            fn == FAPI_MKDIR  ? (set_orig(buf), ifs_mkdir(d, canon)) : ifs_rmdir(d, canon);
        ifs_orig[0] = 0;
        return t ? ifs_err(t) : 0;

    case FAPI_MOVE: {
        static char canon2[IFS_PATHLEN];
        int d2;
        if (user_str(usel, a1, buf, sizeof(buf)) ||
            resolve(current, buf, &d, canon) ||
            user_str(usel, a2, buf, sizeof(buf)) ||
            resolve(current, buf, &d2, canon2) || d != d2)
            return FAPI_ERROR;
        if (path_in(canon2, canon) && !scmp_eq(canon, canon2))
            return FAPI_ERROR;                  /* in sich selbst (gleicher
                                                   Pfad: nur Schreibung) */
        if (in_use(d, canon, 1))
            return FAPI_ERR_INUSE;
        set_orig(buf);                          /* Ziel in Originalschreibung */
        t = ifs_move(d, canon, canon2);
        ifs_orig[0] = 0;
        return t ? ifs_err(t) : 0;
    }

    case FAPI_COMCTL: {                         /* MODE COMn / LPTn      */
        struct fapi_comcfg __far *cf;
        unsigned n = a1 & 0x0F, base, lcr;
        int lpt = (a1 & 0x10) != 0;
        if (!user_ok(usel, a2, sizeof(*cf), 1) || n > (lpt ? 2u : 3u))
            return FAPI_ERROR;
        cf = MK_FP(usel, a2);
        base = bios_ports[lpt ? 4 + n : n];
        if (!base)
            return FAPI_ERROR;
        if (lpt) {
            if (a3)
                lpt_retry[n] = cf->retry;
            cf->retry = lpt_retry[n];
            cf->lsr = (unsigned char)inb(base + 1);   /* Druckerstatus  */
            cf->msr = 0;
            cf->base = base;
            return 0;
        }
        if (com_busy(n))
            return FAPI_ERR_INUSE;
        if (a3) {                               /* einstellen            */
            if (!cf->divisor)
                return FAPI_ERROR;
            outb(base + 3, (cf->lcr & 0x3F) | 0x80);
            outb(base, cf->divisor & 0xFF);
            outb(base + 1, cf->divisor >> 8);
            outb(base + 3, cf->lcr & 0x3F);
            outb(base + 4, 0x03);               /* DTR, RTS (ohne OUT2)  */
            com_retry[n] = cf->retry;
        }
        lcr = inb(base + 3);                    /* zuruecklesen          */
        outb(base + 3, lcr | 0x80);
        cf->divisor = inb(base) | ((unsigned)inb(base + 1) << 8);
        outb(base + 3, lcr & 0x7F);
        cf->lcr = (unsigned char)(lcr & 0x7F);
        cf->retry = com_retry[n];
        cf->lsr = (unsigned char)inb(base + 5);
        cf->msr = (unsigned char)inb(base + 6);
        cf->base = base;
        return 0;
    }

    case FAPI_SYSINFO:
        if (!user_ok(usel, a1, sizeof(struct fapi_sysinfo), 1))
            return FAPI_ERROR;
        sys_info(MK_FP(usel, a1));
        return 0;

    case FAPI_SUBST: {                          /* SUBST X: Pfad / X: /D */
        static char real[IFS_PATHLEN];
        if (a1 >= NDRIVE || a1 == PRE_DRIVE || vol_present(a1))
            return FAPI_ERROR;                  /* echtes Laufwerk       */
        if (!a2) {                              /* aufheben              */
            if (subst_drv[a1] < 0)
                return FAPI_ERROR;
            for (k = 0; k < NTASK; k++)
                if (task[k].state != T_FREE && task[k].drive == a1)
                    return FAPI_ERR_INUSE;      /* aktuelles Laufwerk    */
            subst_drv[a1] = -1;
            return 0;
        }
        if (subst_drv[a1] >= 0 || !subst_sel ||
            user_str(usel, a2, buf, sizeof(buf)) ||
            resolve(current, buf, &d, real) || !ifs_isdir(d, real))
            return FAPI_ERROR;
        if (real[1] == 0)
            real[0] = 0;                        /* Stamm: leerer Vorsatz */
        far_copy(subst_sel, a1 * IFS_PATHLEN, KDATA, (unsigned)real, IFS_PATHLEN);
        subst_drv[a1] = (signed char)d;
        return 0;
    }

    case FAPI_JOIN: {                           /* JOIN X: Pfad / X: /D  */
        static char real[IFS_PATHLEN];
        static struct ifs_dirent jde;
        if (a1 >= NDRIVE || a1 == PRE_DRIVE)
            return FAPI_ERROR;
        if (!a2) {                              /* aufheben              */
            if (join_drv[a1] < 0)
                return FAPI_ERROR;
            for (k = 0; k < NTASK; k++)         /* steht jemand darin?   */
                if (task[k].state != T_FREE && join_under(task[k].drive, task[k].cwd, a1))
                    return FAPI_ERR_INUSE;
            for (k = 1; k < NFILE; k++)         /* offene Dateien dort?  */
                if (files[k].refs && (files[k].kind == OF_FILE || files[k].kind == OF_DASD) &&
                    files[k].f.drive == a1)
                    return FAPI_ERR_INUSE;
            join_drv[a1] = -1;
            return 0;
        }
        if (join_drv[a1] >= 0 || !join_sel || !vol_present(a1) || !vol[a1].used || a1 == sys_drive ||
            subst_drv[a1] >= 0)
            return FAPI_ERROR;
        for (k = 0; k < NDRIVE; k++)            /* traegt selbst einen JOIN */
            if (join_drv[k] == (signed char)a1 || (subst_drv[k] == (signed char)a1))
                return FAPI_ERR_INUSE;
        for (k = 0; k < NTASK; k++)
            if (task[k].state != T_FREE && task[k].drive == a1)
                return FAPI_ERR_INUSE;          /* aktuelles Laufwerk    */
        for (k = 1; k < NFILE; k++)             /* offene Dateien dort?  */
            if (files[k].refs && (files[k].kind == OF_FILE || files[k].kind == OF_DASD) &&
                files[k].f.drive == a1)
                return FAPI_ERR_INUSE;
        if (user_str(usel, a2, buf, sizeof(buf)) ||
            resolve(current, buf, &d, real) || d == (int)a1 || !real[1])
            return FAPI_ERROR;                  /* nicht der Stamm       */
        if (!ifs_isdir(d, real)) {              /* fehlt: anlegen        */
            set_orig(buf);
            if (ifs_mkdir(d, real) || !ifs_isdir(d, real))
                return FAPI_ERROR;
        }
        for (k = 0; ifs_readdir(d, real, k, &jde) == 0; k++)
            if (!(jde.name[0] == '.' && (!jde.name[1] ||
                  (jde.name[1] == '.' && !jde.name[2]))))
                return FAPI_ERR_DENIED;         /* nicht leer            */
        for (k = 0; real[k]; k++)
            real[k] = up(real[k]);
        far_copy(join_sel, JOIN_OFF + a1 * IFS_PATHLEN, KDATA, (unsigned)real, IFS_PATHLEN);
        join_drv[a1] = (signed char)d;
        return 0;
    }

    case FAPI_CACHEINFO: {
        struct fapi_cacheinfo __far *ci;
        if (!user_ok(usel, a1, sizeof(*ci), 1))
            return FAPI_ERROR;
        ci = MK_FP(usel, a1);
        cache_info(ci);
        return 0;
    }

    case FAPI_QJOIN: {                          /* -> "C:\CDROM"         */
        char __far *jp;
        if (a1 >= NDRIVE || join_drv[a1] < 0 || !user_ok(usel, a2, IFS_PATHLEN + 2, 1))
            return FAPI_ERROR;
        jp = MK_FP(join_sel, JOIN_OFF + a1 * IFS_PATHLEN);
        buf[0] = (char)('A' + join_drv[a1]);
        buf[1] = ':';
        for (k = 0; jp[k] && k < IFS_PATHLEN - 1; k++)
            buf[2 + k] = jp[k];
        buf[2 + k] = 0;
        far_copy(usel, a2, KDATA, (unsigned)buf, k + 3);
        return 0;
    }

    case FAPI_QSUBST: {                         /* -> "C:\DOS"         */
        char __far *pre;
        if (a1 >= NDRIVE || subst_drv[a1] < 0 || !user_ok(usel, a2, IFS_PATHLEN + 2, 1))
            return FAPI_ERROR;
        pre = MK_FP(subst_sel, a1 * IFS_PATHLEN);
        buf[0] = (char)('A' + subst_drv[a1]);
        buf[1] = ':';
        for (k = 0; pre[k] && k < IFS_PATHLEN - 1; k++)
            buf[2 + k] = pre[k];
        if (!k)
            buf[2 + k++] = '\\';
        buf[2 + k] = 0;
        far_copy(usel, a2, KDATA, (unsigned)buf, k + 3);
        return 0;
    }

    case FAPI_FREESPACE: {
        static unsigned long kb;
        if (a1 >= NDRIVE || join_hidden(a1) || !user_ok(usel, a2, 4, 1) || ifs_space(subst_real(a1), &kb))
            return FAPI_ERROR;
        far_copy(usel, a2, KDATA, (unsigned)&kb, 4);
        return 0;
    }

    case FAPI_READ:
        if ((k = h2e(a1)) < 0)
            return FAPI_ERROR;
        if (a3 > 0x7FFF)
            a3 = 0x7FFF;
        if (!user_ok(usel, a2, a3, 1))
            return FAPI_ERROR;
        switch (files[k].kind) {
        case OF_CON:
            return (unsigned)con_read(usel, a2, a3);
        case OF_PRD:
            return (unsigned)pipe_read(k, usel, a2, a3);
        case OF_PWR:
            return FAPI_ERROR;
        case OF_DASD:
            t = dasd_rw(k, usel, a2, a3, 0);
            return t < 0 ? FAPI_ERROR : (unsigned)t;
        case OF_NUL:
        case OF_LPT:
            return 0;                       /* nichts zu lesen           */
        case OF_COM:
            return (unsigned)com_io(k, usel, a2, a3, 0);
        }
        t = ifs_read(&files[k].f, files[k].pos, usel, a2, a3);
        if (t < 0)
            return FAPI_ERROR;
        files[k].pos += t;
        return t;

    case FAPI_CLOSE:
        if (a1 < HBASE) {                       /* Standard-Handle: wieder Konsole */
            of_release(task[current].std[a1]);
            task[current].std[a1] = CONS;
            return 0;
        }
        if ((k = h2e(a1)) < 0)
            return FAPI_ERROR;
        files[k].owner = -1;
        t = 0;
        if (files[k].refs == 1 && files[k].kind == OF_FILE) {
            t = ifs_close(&files[k].f);         /* Fehler melden koennen */
            files[k].kind = OF_CON + 1;         /* nicht noch einmal     */
        }
        of_release(k);
        return t ? ifs_err(t) : 0;

    case FAPI_QHANDTYPE:
        if ((k = h2e(a1)) < 0)
            return FAPI_ERROR;
        return files[k].kind == OF_CON ? HT_CONSOLE :
               files[k].kind == OF_FILE ? HT_FILE :
               files[k].kind >= OF_LPT ? HT_DEVICE :
               files[k].kind == OF_DASD ? HT_DRIVE : HT_PIPE;

    case FAPI_SETFINFO: {
        struct fapi_finfo __far *fi;
        if (user_str(usel, a1, buf, sizeof(buf)) || resolve(current, buf, &d, canon) ||
            !user_ok(usel, a2, sizeof(*fi), 0))
            return FAPI_ERROR;
        if (in_use(d, canon, 0))
            return FAPI_ERR_INUSE;      /* offene Datei: Eintrag gehoert ihr */
        fi = MK_FP(usel, a2);
        t = ifs_setinfo(d, canon, fi->mask, fi->attr, fi->date, fi->time);
        return t ? ifs_err(t) : 0;
    }

    case FAPI_SETDATETIME: {
        unsigned short __far *p;
        if (!user_ok(usel, a1, 4, 0))
            return FAPI_ERROR;
        p = MK_FP(usel, a1);
        return rtc_set(p[0], p[1]) ? FAPI_ERROR : 0;
    }

    case FAPI_LABEL: {
        static char lb[12], nl[12];
        static unsigned long ser;
        struct fapi_label __far *fl;
        if (!a3)                                /* lesen: SUBST-Ziel     */
            a1 = subst_real(a1);
        if (a1 >= NDRIVE || a1 == PRE_DRIVE || !vol_present(a1) || join_hidden(a1) ||
            !user_ok(usel, a2, sizeof(*fl), 1))
            return FAPI_ERROR;
        if (a3 && user_str(usel, a3, nl, sizeof(nl)))
            return FAPI_ERROR;
        if (vol[a1].drv == &mod_driver && mod_mount(a1) && blk_nomedia(vol[a1].blk))
            return FAPI_ERROR;                  /* keine CD: "nicht bereit" */
        t = ifs_label(a1, lb, &ser, a3 ? nl : 0);
        if (t)
            return ifs_err(t);
        fl = MK_FP(usel, a2);
        far_copy(usel, a2, KDATA, (unsigned)lb, 12);
        fl->serial = ser;
        return 0;
    }

    case FAPI_QCHILDRC:
        return task[current].child_rc;

    case FAPI_OPENDRIVE:
        if (a1 >= NDRIVE || a1 == PRE_DRIVE || !vol_present(a1) || vol[a1].locked || join_hidden(a1))
            return a1 < NDRIVE && vol[a1].locked ? FAPI_ERR_INUSE : FAPI_ERROR;
        if (a2 == DRV_OPEN_LOCK) {
            if (blk[vol[a1].blk].bsize != 512)
                return FAPI_ERR_WP;
            for (k = 1; k < NFILE; k++)         /* offene Dateien dort?  */
                if (files[k].refs && (files[k].kind == OF_FILE || files[k].kind == OF_DASD) &&
                    files[k].f.drive == a1)
                    return FAPI_ERR_INUSE;
        }
        k = of_alloc();
        if (k < 0)
            return FAPI_ERROR;
        files[k].kind = OF_DASD;
        files[k].f.drive = (unsigned char)a1;
        files[k].f.attr = 0;
        files[k].pipe = (unsigned char)(a2 == DRV_OPEN_LOCK);
        if (a2 == DRV_OPEN_LOCK)
            vol[a1].locked = 1;
        if (IS_FLOPPY(vol[a1].blk))
            blk_media(vol[a1].blk);             /* gewechselt? neu lesen */
        return k + HBASE;

    case FAPI_DISKINFO: {                       /* fuer FDISK            */
        struct fapi_diskinfo __far *di;
        int dev = disk_blk(a1), n;
        if (dev < 0 || !user_ok(usel, a2, sizeof(*di), 1))
            return FAPI_ERROR;
        di = MK_FP(usel, a2);
        di->sectors = blk[dev].blocks;
        di->bsize = blk[dev].bsize;
        for (n = 0; n < 12 && drv_name(blk[dev].drv)[n]; n++)
            di->driver[n] = drv_name(blk[dev].drv)[n];
        di->driver[n] = 0;
        di->unit = blk[dev].unit;
        for (n = 0, k = 0; k < NDRIVE; k++)
            if (vol_present(k) && vol[k].blk == dev)
                n++;
        di->nvol = (unsigned char)n;
        di->rsv = 0;
        {
            unsigned gh, gs;
            disk_geo(dev, &gh, &gs);
            di->heads = gh;
            di->spt = gs;
        }
        return 0;
    }

    case FAPI_OPENDISK: {                       /* ganze Platte (FDISK)  */
        int dev = disk_blk(a1), d;
        if (dev < 0)
            return FAPI_ERROR;
        if (a2 == DRV_OPEN_LOCK) {              /* nichts offen darauf?  */
            for (k = 1; k < NFILE; k++)
                if (files[k].refs && files[k].kind == OF_FILE &&
                    vol[files[k].f.drive].blk == dev)
                    return FAPI_ERR_INUSE;
                else if (files[k].refs && files[k].kind == OF_DASD &&
                         (files[k].f.attr == DASD_DISK ? files[k].f.drive == dev
                                                       : vol[files[k].f.drive].blk == dev))
                    return FAPI_ERR_INUSE;
        }
        k = of_alloc();
        if (k < 0)
            return FAPI_ERROR;
        files[k].kind = OF_DASD;
        files[k].f.drive = (unsigned char)dev;
        files[k].f.attr = DASD_DISK;
        files[k].pipe = (unsigned char)(a2 == DRV_OPEN_LOCK);
        if (a2 == DRV_OPEN_LOCK)
            for (d = 0; d < NDRIVE; d++)
                if (vol_present(d) && vol[d].blk == dev)
                    vol[d].locked = 1;
        return k + HBASE;
    }

    case FAPI_FMTTRACK: {
        struct fapi_fmttrack __far *ft;
        int dev;
        if ((k = h2e(a1)) < 0 || files[k].kind != OF_DASD || !files[k].pipe ||
            files[k].f.attr == DASD_DISK || !user_ok(usel, a2, sizeof(*ft), 0))
            return FAPI_ERROR;
        dev = vol[files[k].f.drive].blk;
        if (!IS_FLOPPY(dev))
            return FAPI_ERROR;
        ft = MK_FP(usel, a2);
        t = fd_format(blk[dev].unit, ft->cyl, ft->head, ft->blocks);
        if (blk[dev].blocks != fd_size(blk[dev].unit)) {
            blk[dev].blocks = fd_size(blk[dev].unit);
            blk_invalidate();               /* anderes Format: Cache weg */
        }
        return t == 0 ? 0 : t == -2 ? FAPI_ERR_WP : FAPI_ERROR;
    }

    case FAPI_SEEK: {
        unsigned long __far *p;
        if ((k = h2e(a1)) < 0 || !user_ok(usel, a2, 4, 0))
            return FAPI_ERROR;
        if (files[k].kind != OF_FILE && files[k].kind != OF_DASD)
            return FAPI_ERROR;
        p = MK_FP(usel, a2);
        files[k].pos = *p;
        if (files[k].kind == OF_FILE && files[k].pos > files[k].f.size)
            files[k].pos = files[k].f.size;     /* keine Loecher         */
        return 0;
    }

    case FAPI_QFSINFO: {
        struct fapi_fsinfo __far *fi;
        const char *nm;
        int dv;
        if (a1 >= NDRIVE || a1 == PRE_DRIVE || !vol_present(a1) || join_hidden(a1) ||
            !user_ok(usel, a2, sizeof(*fi), 1))
            return FAPI_ERROR;
        fi = MK_FP(usel, a2);
        dv = vol[a1].blk;
        if (IS_FLOPPY(dv) && !vol[a1].locked && blk_media(dv) == 1)
            vol_reset(a1);                  /* andere Diskette: neu      */
        if (vol[a1].drv == &mod_driver && mod_mount(a1))
            ifs_notready = 0;               /* nur FSI_NOMEDIA, kein Fehler */
        nm = vol[a1].drv == &mod_driver ? drv_name(vol[a1].mod) : vol[a1].drv->name;
        for (k = 0; nm[k] && nm[k] != '.' && k < (int)sizeof(fi->fsname) - 1; k++)
            fi->fsname[k] = nm[k];
        fi->fsname[k] = 0;
        if (!vol[a1].used) {                /* nicht formatiert         */
            fi->fsname[0] = 'R'; fi->fsname[1] = 'A'; fi->fsname[2] = 'W';
            fi->fsname[3] = 0;
        }
        fi->sectors = vol[a1].psize ? vol[a1].psize : blk[dv].blocks;
        fi->hidden = vol[a1].start;
        fi->ptype = vol[a1].ptype;
        fi->bsize = blk[dv].bsize;
        fi->flags = (IS_FLOPPY(dv) ? FSI_REMOVABLE : 0) | (blk[dv].bsize != 512 ? FSI_READONLY : 0) |
                    (vol[a1].was_dirty ? FSI_DIRTY : 0) | (blk_nomedia(dv) ? FSI_NOMEDIA : 0);
        return 0;
    }

    case FAPI_DUP2:
        if (a2 >= HBASE)
            return FAPI_ERROR;
        k = a1 == 0xFFFF ? CONS : h2e(a1);
        if (k < 0)
            return FAPI_ERROR;
        of_ref(k);
        of_release(task[current].std[a2]);
        task[current].std[a2] = (unsigned char)k;
        return 0;

    case FAPI_DUP: {
        int n;
        if ((k = h2e(a1)) < 0)
            return FAPI_ERROR;
        n = of_alloc();                         /* neues Handle, gleicher Inhalt */
        if (n < 0)
            return FAPI_ERROR;
        if (files[k].kind == OF_CON) {          /* Konsole: eigener Eintrag */
            files[n].kind = OF_CON;
            return n + HBASE;
        }
        far_copy(FP_SEG(files), FP_OFF(&files[n].f), FP_SEG(files), FP_OFF(&files[k].f),
                 sizeof(struct ifs_file) + 4 + IFS_PATHLEN);
        files[n].kind = files[k].kind;
        files[n].pipe = files[k].pipe;
        return n + HBASE;
    }

    case FAPI_PIPE: {
        static unsigned short hh[2];
        int r, w, p;
        if (!user_ok(usel, a1, 4, 1))
            return FAPI_ERROR;
        for (p = 0; p < NPIPE && pipes[p].used; p++)
            ;
        if (p == NPIPE || (r = of_alloc()) < 0)
            return FAPI_ERROR;
        if ((w = of_alloc()) < 0) {
            files[r].refs = 0;
            return FAPI_ERROR;
        }
        files[r].kind = OF_PRD;
        files[w].kind = OF_PWR;
        files[r].pipe = files[w].pipe = (unsigned char)p;
        pipes[p].used = 1;
        pipes[p].head = pipes[p].count = 0;
        pipes[p].rd = (unsigned char)r;
        pipes[p].wr = (unsigned char)w;
        hh[0] = (unsigned short)(r + HBASE);
        hh[1] = (unsigned short)(w + HBASE);
        far_copy(usel, a1, KDATA, (unsigned)hh, 4);
        return 0;
    }

    case FAPI_FINDENTRY:
        if (!user_ok(usel, a1, sizeof(de), 1))
            return FAPI_ERROR;
        if (a3 == 0)
            buf[0] = 0;                         /* aktuelles Verzeichnis */
        else if (user_str(usel, a3, buf, sizeof(buf)))
            return FAPI_ERROR;
        de.sname[0] = 0;                        /* IFS-Module kennen es nicht */
        if (resolve(current, buf, &d, canon) || ifs_readdir(d, canon, a2, &de))
            return FAPI_ERROR;
        if (!task[current].lfn && de.sname[0])  /* altes Programm: 8.3   */
            scopy(de.name, de.sname, sizeof(de.sname));
        far_copy(usel, a1, KDATA, (unsigned)&de, sizeof(struct fapi_dirent));
        return 0;

    case FAPI_CHDIR: {
        static char real[IFS_PATHLEN];
        int rd;
        if (user_str(usel, a1, buf, sizeof(buf)) ||
            resolve_v(current, buf, &d, canon))
            return FAPI_ERROR;
        rd = d;
        scopy(real, canon, IFS_PATHLEN);
        if (subst_map(&rd, real))
            return FAPI_ERROR;
        join_map(&rd, real);
        if (!ifs_isdir(rd, real))
            return FAPI_ERROR;
        /* gespeichert wird die Schreibung vom Datentraeger (lange Namen:
           "\\Neuer Ordner"), verglichen wird ohne Gross/klein */
        if (subst_drv[d] < 0 && canon[1] && namepath_j(d, canon, real, 0) == 0)
            scopy(canon, real, IFS_PATHLEN);
        if (d != task[current].drive) {         /* anderes Laufwerk: merken */
            char __far *oc = dcwd(current, d, 1);
            fscopy(oc, (char __far *)(canon[1] ? canon : ""), IFS_PATHLEN);
            return 0;
        }
        scopy(task[current].cwd, canon[1] ? canon : "", IFS_PATHLEN);
        return 0;
    }

    case FAPI_QCURDIR: {                        /* "C:\DOS"            */
        /* Programme ohne FX_FLAG_LONGNAMES bekommen den Pfad in 8.3-Namen
           und hoechstens 130 Bytes (so stand es bisher in fapi_abi.h) */
        /* CX = Laufwerk + 1 (0 = aktuelles): Verzeichnis dort (CD X:) */
        static char qb[IFS_PATHLEN + 2], cw[IFS_PATHLEN];
        unsigned n;
        d = task[current].drive;
        if (a2 && a2 - 1 != (unsigned)d) {
            char __far *oc;
            d = (int)a2 - 1;
            if (d >= NDRIVE || join_hidden(d) || (!vol_present(d) && subst_drv[d] < 0))
                return FAPI_ERROR;
            cw[0] = 0;
            if ((oc = dcwd(current, d, 0)) != 0)
                fscopy((char __far *)cw, oc, IFS_PATHLEN);
        } else
            scopy(cw, task[current].cwd, IFS_PATHLEN);
        qb[0] = (char)('A' + d);
        qb[1] = ':';
        scopy(qb + 2, cw[0] ? cw : "\\", IFS_PATHLEN);
        if (!task[current].lfn && subst_drv[d] < 0 && cw[0]) {
            static char uc[IFS_PATHLEN];
            for (n = 0; cw[n]; n++)
                uc[n] = up(cw[n]);
            uc[n] = 0;
            namepath_j(d, uc, qb + 2, 1);
        }
        for (n = 0; qb[n]; n++)
            ;
        if ((!task[current].lfn && n > 129) || !user_ok(usel, a1, n + 1, 1))
            return FAPI_ERROR;
        far_copy(usel, a1, KDATA, (unsigned)qb, n + 1);
        return 0;
    }

    case FAPI_SELECTDISK:                       /* A: ggf. erst einhaengen */
        if (a1 >= NDRIVE || a1 == PRE_DRIVE || join_hidden(a1) || (!vol_present(a1) && subst_drv[a1] < 0))
            return FAPI_ERROR;
        if (vol_present(a1) && IS_FLOPPY(vol[a1].blk) && !vol[a1].locked) {
            blk_media(vol[a1].blk);
            if (blk_nomedia(vol[a1].blk))
                return FAPI_ERR_NOTREADY;       /* keine Diskette        */
        }
        if (vol_present(a1) && vol[a1].drv == &mod_driver && mod_mount(a1) &&
            blk_nomedia(vol[a1].blk))
            return FAPI_ERR_NOTREADY;           /* keine CD              */
        if (a1 != task[current].drive) {        /* Verzeichnisse tauschen */
            char __far *oc = dcwd(current, task[current].drive, 1);
            fscopy(oc, (char __far *)task[current].cwd, IFS_PATHLEN);
            task[current].drive = (unsigned char)a1;
            task[current].cwd[0] = 0;
            if ((oc = dcwd(current, (int)a1, 0)) != 0) {
                fscopy((char __far *)task[current].cwd, oc, IFS_PATHLEN);
                if (task[current].cwd[0] && resolve(current, ".", &d, canon) == 0 &&
                    !ifs_isdir(d, canon))
                    task[current].cwd[0] = 0;   /* gibt es nicht mehr     */
            }
        }
        return 0;

    case FAPI_QCURDISK:
        return task[current].drive;

    case FAPI_KBDSETLAYOUT:
        if (a1 == 0)
            return kbd_set_layout(0, 0) ? FAPI_ERROR : 0;
        if (!user_ok(usel, a1, KBL_SIZE_BYTES, 0))
            return FAPI_ERROR;
        return kbd_set_layout(usel, a1) ? FAPI_ERROR : 0;

    case FAPI_KBDGETLAYOUT:
        if (!user_ok(usel, a1, 4, 1))
            return FAPI_ERROR;
        far_copy(usel, a1, KDATA, (unsigned)kbd_layout_id(), 4);
        return 0;

    case FAPI_GETENV: {
        static char name[64];
        char __far *e;
        unsigned nl, i;
        if (user_str(usel, a1, name, sizeof(name)))
            return FAPI_ERROR;
        for (nl = 0; name[nl]; nl++)
            name[nl] = up(name[nl]);
        k = env_find(current, name, nl);
        if (k < 0)
            return FAPI_ERROR;
        e = MK_FP(env_sel, env_off(current) + k + nl + 1);
        for (i = 0; e[i]; i++)
            ;
        if (a3 == 0 || !user_ok(usel, a2, a3, 1))
            return FAPI_ERROR;
        far_copy(usel, a2, env_sel, FP_OFF(e), i + 1 <= a3 ? i + 1 : a3);
        return i;
    }

    case FAPI_GETDATETIME: {
        static unsigned long st;
        if (!user_ok(usel, a1, 4, 1))
            return FAPI_ERROR;
        st = rtc_stamp();
        st = (st >> 16) | (st << 16);           /* Datum zuerst          */
        far_copy(usel, a1, KDATA, (unsigned)&st, 4);
        return 0;
    }

    case FAPI_SETENV:
        if (user_str(usel, a1, buf, sizeof(buf)))
            return FAPI_ERROR;
        k = env_set(current, buf);
        return k == 0 ? 0 : k == -2 ? FAPI_ERR_FULL : FAPI_ERROR;

    case FAPI_ENVENTRY: {
        char __far *e = MK_FP(env_sel, env_off(current));
        unsigned i = 0, n = 0, l;
        while (e[i] && n < a1) {
            while (e[i++])
                ;
            n++;
        }
        if (!e[i] || a3 == 0 || !user_ok(usel, a2, a3, 1))
            return FAPI_ERROR;
        for (l = 0; e[i + l]; l++)
            ;
        far_copy(usel, a2, env_sel, FP_OFF(e) + i, l + 1 <= a3 ? l + 1 : a3);
        return l;
    }

    case FAPI_SHUTDOWN:
        system_down(a1 != 0);
        return 0;

    case FAPI_SETSIGHANDLER:
        if (a1 && a1 > sel_limit(USEL(0)))
            return FAPI_ERROR;
        task[current].sig_tramp = a1;
        task[current].sig_mask = a1 ? a2 & (SIG_BREAK | SIG_SHUTDOWN) : 0;
        return 0;

    case FAPI_SIGRETURN: {
        struct task *tk = &task[current];
        unsigned short *fr = FRAME(current);
        unsigned i;
        if (!tk->in_sig)
            return FAPI_ERROR;
        for (i = 0; i < 14; i++)
            fr[i] = tk->sig_save[i];
        tk->in_sig = 0;
        return tk->sig_save[14];                /* Ergebnis des unterbrochenen Aufrufs */
    }

    case FAPI_MOUOPEN:
        return vio_mou_open(s, current);

    case FAPI_MOUREADEVENT: {
        static unsigned char ev[8];             /* nolock: nur kurz benutzt */
        unsigned char __far *p;
        unsigned n = (a2 & MOU_XY) ? 8 : 4;    /* mit x/y: 8 Byte        */
        if (!user_ok(usel, a1, n, 1))
            return FAPI_ERROR;
        for (;;) {
            if (vio_mou_read(s, ev)) {
                p = MK_FP(usel, a1);
                for (k = 0; k < (int)n; k++)
                    p[k] = ev[k];
                return 0;
            }
            if (!(a2 & MOU_WAIT) || sig_waiting())
                return 1;
            sleep_ticks(1);                     /* bis zum naechsten Takt */
        }
    }

    case FAPI_MOUSHOWPTR:
        vio_mou_show(s, a1);
        return 0;

    case FAPI_VIOGETBUF: {                      /* LVB wie bei OS/2 1.x  */
        unsigned idx;
        for (idx = LDT_FIRST_FREE; idx < NLDT; idx++)
            if (ldt[current][idx].access == 0)
                break;
        if (idx == NLDT)
            return FAPI_ERROR;
        put_desc(&ldt[current][idx], vio_buf_lin(s), SBYTES - 1, ACC_UDATA);
        ldt_alias[current] |= 1u << idx;
        return USEL(idx);
    }

    case FAPI_VIOSHOWBUF:
        vio_showbuf(s, a1, a2);
        return 0;

    case FAPI_VIOSETGFX:
        return vio_setgfx(s, current, a1) ? FAPI_ERROR : 0;

    case FAPI_VIOGETPHYSBUF: {                  /* wie OS/2 VioGetPhysBuf */
        unsigned idx;
        if (!vio_is_gfx(s))
            return FAPI_ERROR;
        for (idx = LDT_FIRST_FREE; idx < NLDT; idx++)
            if (ldt[current][idx].access == 0)
                break;
        if (idx == NLDT)
            return FAPI_ERROR;
        put_desc(&ldt[current][idx], vio_gfx_base(s), vio_gfx_limit(s), ACC_UDATA);
        ldt_alias[current] |= 1u << idx;
        vio_gfx_reg(s, current, USEL(idx));
        return USEL(idx);
    }

    case FAPI_VIOSETPLANE:
        return vio_setplane(s, a1) ? FAPI_ERROR : 0;

    case FAPI_VIOSETPAGE:
        return vio_setpage(s, a1) ? FAPI_ERROR : 0;

    case FAPI_AUDIO:
        return aud_fapi(usel, a1, a2, a3);

    case FAPI_QTICKS:
        return (unsigned)get_ticks();

    case FAPI_VIOSETPAL:
        if (a2 == 0 || a2 > 256 || !user_ok(usel, a3, a2 * 3, 0))
            return FAPI_ERROR;
        vio_setpal(s, a1, a2, usel, a3);
        return 0;

    case FAPI_CLIPPUT:
    case FAPI_CLIPGET:
        if (a1 == 0)
            a1 = usel;
        a1 |= 3;
        if (!user_ok(a1, a2, a3, fn == FAPI_CLIPGET))
            return FAPI_ERROR;
        return fn == FAPI_CLIPPUT ? vio_clip_put(a1, a2, a3)
                                  : vio_clip_get(a1, a2, a3);

    case FAPI_YIELD:
        schedule_next();
        return 0;

    case FAPI_SLEEP:
        sleep_ticks((a1 + 9) / 10);
        return 0;

    case FAPI_EXIT:
        task[current].rc = (unsigned char)a1;
        task_end(current);
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
    case 7:  return "#NM Kein Koprozessor";
    case 16: return "#MF Koprozessorfehler";
    }
    return "Ausnahme";
}

void fault(unsigned vec, unsigned err, unsigned cs, unsigned ip)
{
    static char msg[] = " bei xxxx:xxxx (Fehlercode xxxxh)";
    static char kmsg[80];
    int i, j;

    hex4(cs, msg + 5);
    hex4(ip, msg + 10);
    hex4(err, msg + 27);

    if ((cs & 3) == 3 && current >= 0) {
        /* Fehler in einer Anwendung: nur dieses Programm beenden */
        tmsg(current, "\n", task[current].name, ": ");
        tmsg(current, exc_name(vec), msg, "\nProgramm abgebrochen.\n");
        task[current].rc = 255;
        klock_enter();                  /* task_end schliesst Dateien    */
        task_end(current);
        schedule_next();
    }
    j = 0;
    for (i = 0; "KERNEL PANIC: "[i]; i++) kmsg[j++] = "KERNEL PANIC: "[i];
    for (i = 0; exc_name(vec)[i]; i++) kmsg[j++] = exc_name(vec)[i];
    for (i = 0; msg[i] && j < 79; i++) kmsg[j++] = msg[i];
    kmsg[j] = 0;
    panic(kmsg);
}

/* ---- Hardware ---------------------------------------------------------- */
static unsigned pic_mask = 0xFFBC;      /* gesetztes Bit = IRQ maskiert  */

static void pic_update(void)
{
    unsigned f = irq_off();
    outb(0x21, pic_mask & 0xFF);
    outb(0xA1, pic_mask >> 8);
    irq_restore(f);
}

static void pic_pit_init(void)
{
    /* PICs auf Vektor 20h/28h legen, sonst kollidiert IRQ0 mit #DF */
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, pic_mask & 0xFF);            /* IRQ 0 Takt, 1 Tastatur, 6 Diskette */
    outb(0xA1, pic_mask >> 8);

    /* PIT Kanal 0: 100 Hz */
    outb(0x43, 0x36);
    outb(0x40, 11932 & 0xFF);
    outb(0x40, 11932 >> 8);
}

/* ---- Blockgeraete ----------------------------------------------------- */
struct blkdev blk[NBLK];
int nblk;
static unsigned char blk_nm[NBLK];    /* letztes Lesen: kein Medium (CD) */

extern void drv_call(unsigned code_sel, unsigned data_sel, unsigned entry,
                     unsigned req_off);
extern void devhlp_entry(void);
extern unsigned short devhlp_dx;

static struct {
    unsigned char used;
    unsigned      entry;
    unsigned      unit, cnt;
    unsigned      maxcnt;               /* Bloecke je READ (INIT, >= 1) */
    char          name[13];
} drv[NDRV];
static struct drv_req req;
static int boot_session;

static const char *drv_name(int d)
{
    return drv[d].name;
}

/* Text im ROM fuer MSD: der laengste mit "BIOS", "(C)" oder "Copyright",
   sonst der laengste ueberwiegend aus Buchstaben/Ziffern (ohne '%') */
static void rom_str(unsigned long lin, unsigned len, char __far *out, unsigned max)
{
    unsigned char __far *p = MK_FP(SCRATCH, 0);
    unsigned i, s = 0, n = 0, al = 0, best = 0, bs = 0, c, kw = 0, pc = 0, bkw = 0, sp = 0;
    scratch_map(lin, len - 1);
    for (i = 0; i <= len - 1; i++) {
        c = i < len - 1 ? p[i] : 0;
        if (c >= 0x20 && c < 0x7F) {
            if (!n)
                s = i, al = 0, kw = 0, pc = 0, sp = 0;
            n++;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                al++;
            if (c == '%')
                pc = 1;
            if (c == ' ')
                sp = 1, al++;
            if (n >= 4 && p[i - 3] == 'B' && p[i - 2] == 'I' && p[i - 1] == 'O' && c == 'S' &&
                kw < 1)
                kw = 1;                     /* "BIOS" zaehlt weniger als  */
            if (n >= 4 && ((p[i - 2] == '(' && (p[i - 1] | 0x20) == 'c' && c == ')') ||
                           (p[i - 3] == 'r' && p[i - 2] == 'i' && p[i - 1] == 'g' && c == 'h')))
                kw = 2;                     /* "(C)" / "Copyright"        */
            continue;
        }
        /* Zeichensalat aussortieren: mindestens 8 Zeichen, ein Leerzeichen,
           zu drei Vierteln Buchstaben/Ziffern/Leerzeichen */
        if (n >= 8 && sp && al * 4 >= n * 3 && !pc && (kw > bkw || (kw == bkw && n > best)))
            best = n, bs = s, bkw = kw;
        n = 0;
    }
    if (best >= max)
        best = max - 1;
    for (i = 0; i < best; i++)
        out[i] = p[bs + i];
    out[i] = 0;
}

static void sys_info(struct fapi_sysinfo __far *si)
{
    unsigned i, k;
    si->cpu = cpu_386 ? 3 : 2;
    si->fpu = (unsigned char)fpu_ok;
    si->vga = vga_ok;
    si->model = bios_model;
    for (i = 0; i < 8; i++)
        si->bios_date[i] = bios_date[i];
    si->bios_date[8] = 0;
    si->fdtype[0] = (unsigned char)fd_type(0);
    si->fdtype[1] = (unsigned char)fd_type(1);
    si->rsv = 0;
    si->equip = bios_equip;
    for (i = 0; i < 4; i++)
        si->com[i] = bios_ports[i];
    for (i = 0; i < 3; i++)
        si->lpt[i] = bios_ports[4 + i];
    si->conv_kb = mem_conv_kb;
    si->ext_kb = mem_ext_kb;
    si->kernel_kb = (unsigned)((KDATA_LIN + kdata_size - KCODE_LIN + 1023) >> 10);
    si->irq_used = ~pic_mask & ~4u;         /* ohne Kaskade               */
    si->uptime_s = uptime_s;
    rom_str(0xF0000UL, 0xFFFF, si->bios_name, 64);
    rom_str(0xC0000UL, 0x0800, si->vbios_name, 64);
    for (i = 0; i < 6; i++) {
        si->drivers[i][0] = 0;
        if (i < NDRV && drv[i].used)
            for (k = 0; k < 12 && drv[i].name[k]; k++)
                si->drivers[i][k] = drv[i].name[k], si->drivers[i][k + 1] = 0;
    }
}

static void drv_request(int d)
{
    drv_call(DRVC_SEL(d), DRVD_SEL(d), drv[d].entry, (unsigned)&req);
}

/* zuletzt gelesener oder geschriebener Sektor, steht noch in DMABUF */
static int lc_dev = -1;
static unsigned long lc_lba;
static unsigned char blk_dirty[NBLK];   /* seit dem letzten Leeren beschrieben */

/* ---- Platten-Cache (CACHE=), wie SMARTDRV ------------------------------
   Daten in einem Stueck erweiterten Speichers (bc_lin), Bloecke zu 2 KB
   (4 Sektoren, Blockanfang lba & ~3), Zugriff ueber ein eigenes GDT-Fenster
   (bc_wsel), das vor jedem Zugriff auf den Block gelegt wird. Verwaltung
   (je Block 10 Byte + Hash-Tabelle) in einem Kernelsegment. Nur 512er-
   Geraete. Verdraengt wird nach dem Uhr-Verfahren (ref-Bit); Vorausgelesenes
   ("Strom", fat_read) kommt ohne ref-Bit herein und geht zuerst.
   Schreiben: Festplatten verzoegert (bc_delay Takte, CACHE=n,s), die Daten
   gehen beim Verdraengen, nach Ablauf der Frist (bc_lazy: Ende eines FAPI-
   Aufrufs mit Sperre, Warten auf Tasten) und in blk_flush auf die Platte;
   Disketten und bc_sync (Sauber-Bit) sofort. Verzoegert geschriebene
   Sektoren werden nie von vorausgelesenen ueberschrieben. */
#define BC_HASH     1024
#define BC_END      0xFFFFu
struct bce {
    unsigned long lba;                  /* erster Sektor des Blocks      */
    signed char   dev;                  /* -1 = frei                     */
    unsigned char valid, dirty, ref;    /* Bit s: Sektor lba + s         */
    unsigned      next;                 /* Hash-Kette                    */
};
static unsigned       bc_msel, bc_wsel, bc_n, bc_hand, bc_ndirty, bc_delay;
static unsigned long  bc_since, bc_grow_t;
/* Daten in Stuecken zu 16 KB (8 Bloecke, 64 Einheiten), einzeln belegt:
   bei Speichermangel gibt mem_alloc saubere Stuecke zurueck (bc_reclaim),
   im Leerlauf waechst der Cache wieder bis zur CACHE=-Groesse (bc_regrow).
   Bloecke fehlender Stuecke haben dev = -2. Vorbereitung fuer den
   dynamischen Cache von 6.0 (dort mit Seiten statt Stuecken). */
#define BC_CHB      8                   /* Bloecke je Stueck             */
#define BC_CHU      64                  /* Einheiten je Stueck (16 KB)   */
#define BC_MAXCH    512                 /* 8 MB                          */
static unsigned       bc_unit[BC_MAXCH], bc_nch, bc_present, bc_growing;
#define BC_LIN(i)   (((unsigned long)bc_unit[(i) >> 3] << 8) + ((unsigned)((i) & 7) << 11))
unsigned char         bc_sync;          /* fat.c: sofort schreiben       */
unsigned long         bc_hits, bc_miss;
static unsigned       cache_kb = 0xFFFF, cache_s = 5;   /* CACHE=kb,s     */
#define BCE(i)  ((struct bce __far *)MK_FP(bc_msel, (unsigned)(i) * sizeof(struct bce)))
#define BCH(h)  (((unsigned __far *)MK_FP(bc_msel, bc_n * sizeof(struct bce)))[h])

static void bc_unlink(unsigned i);
static unsigned bc_reclaim(unsigned units);

/* ein fehlendes Stueck holen; 0 = ok */
static int bc_chunk_get(unsigned c)
{
    unsigned u, k;
    bc_growing = 1;                     /* kein bc_reclaim waehrenddessen */
    u = mem_alloc(BC_CHU);
    bc_growing = 0;
    if (!u)
        return -1;
    bc_unit[c] = u;
    for (k = 0; k < BC_CHB; k++)
        BCE(c * BC_CHB + k)->dev = -1;
    bc_present++;
    return 0;
}

void blk_cache_init(unsigned kb)
{
    unsigned i, w;
    if (kb > 8192)
        kb = 8192;
    kb &= ~15u;                         /* ganze Stuecke                 */
    bc_n = 0;
    if (kb < 16)
        return;
    w = kseg_alloc(16);                 /* nur fuer den Deskriptor       */
    bc_msel = kseg_alloc((unsigned long)((kb >> 1) * sizeof(struct bce) + BC_HASH * 2));
    if (!w || !bc_msel)
        return;
    bc_wsel = w;
    bc_n = kb >> 1;
    bc_nch = bc_n / BC_CHB;
    for (i = 0; i < bc_n; i++)
        BCE(i)->dev = -2;
    for (i = 0; i < BC_HASH; i++)
        BCH(i) = BC_END;
    for (i = 0; i < bc_nch; i++)        /* so viel, wie der Speicher hergibt */
        if (bc_chunk_get(i))
            break;
    bc_delay = cache_s > 60 ? 60 * 18 : cache_s * 18 + cache_s / 5;
    mem_reclaim = bc_reclaim;
}

unsigned blk_cache_kb(void)
{
    return bc_present << 4;
}

/* Speichermangel: saubere Stuecke von hinten freigeben, bis units Platz
   gemacht sind. Nur Tabellen, keine Plattenzugriffe. */
static unsigned bc_reclaim(unsigned units)
{
    unsigned c, k, freed = 0;
    struct bce __far *e;
    if (bc_growing)
        return 0;
    for (c = bc_nch; c-- > 0 && freed < units; ) {
        if (!bc_unit[c])
            continue;
        for (k = 0; k < BC_CHB; k++)
            if (BCE(c * BC_CHB + k)->dirty)
                break;
        if (k < BC_CHB)
            continue;                   /* noch Ungeschriebenes drin     */
        for (k = 0; k < BC_CHB; k++) {
            e = BCE(c * BC_CHB + k);
            if (e->dev >= 0)
                bc_unlink(c * BC_CHB + k);
            e->dev = -2;
        }
        mem_free(bc_unit[c], BC_CHU);
        bc_unit[c] = 0;
        bc_present--;
        freed += BC_CHU;
    }
    return freed;
}

/* im Leerlauf (mit Kernelsperre): fehlende Stuecke zurueckholen, wenn
   genug frei ist (256 KB bleiben fuer Programme) */
static void bc_regrow(void)
{
    unsigned c;
    if (bc_present == bc_nch || get_ticks() - bc_grow_t < 91)
        return;
    bc_grow_t = get_ticks();
    for (c = 0; c < bc_nch; c++)
        if (!bc_unit[c]) {
            if (mem_free_kb() < 256 + 16 || bc_chunk_get(c))
                return;
        }
}

static void cache_info(struct fapi_cacheinfo __far *ci)
{
    ci->kb = bc_present << 4;           /* gerade belegt (kann schrumpfen) */
    ci->delay_s = bc_n && bc_delay ? cache_s : 0;
    ci->hits = bc_hits;
    ci->misses = bc_miss;
    ci->dirty = bc_ndirty;
}

static unsigned bc_hash(int dev, unsigned long blba)
{
    return ((unsigned)(blba >> 2) ^ (unsigned)(blba >> 12) ^ ((unsigned)dev << 7)) & (BC_HASH - 1);
}

static void bc_win(unsigned i)          /* Fenster auf Block i            */
{
    set_desc(bc_wsel >> 3, BC_LIN(i), 2047, ACC_KDATA);
}

static unsigned bc_lookup(int dev, unsigned long blba)
{
    unsigned i = BCH(bc_hash(dev, blba));
    while (i != BC_END && (BCE(i)->dev != dev || BCE(i)->lba != blba))
        i = BCE(i)->next;
    return i;
}

static void bc_unlink(unsigned i)
{
    struct bce __far *e = BCE(i);
    unsigned h = bc_hash(e->dev, e->lba), k = BCH(h);
    if (k == i)
        BCH(h) = e->next;
    else {
        while (k != BC_END && BCE(k)->next != i)
            k = BCE(k)->next;
        if (k != BC_END)
            BCE(k)->next = e->next;
    }
    e->dev = -1;
}

static int bc_writeback(int dev);

/* verzoegerte Sektoren eines Blocks auf die Platte, direkt aus dem Cache */
static void bc_wb(unsigned i)
{
    struct bce __far *e = BCE(i);
    struct blkdev *b = &blk[e->dev];
    unsigned s, tries;

    for (s = 0; s < 4; s++) {
        if (!(e->dirty & (1 << s)))
            continue;
        for (tries = 0; tries < 2; tries++) {
            bc_win(i);
            req.cmd = DRV_WRITE;
            req.unit = b->unit;
            req.count = 1;
            req.status = DRV_ERR_IO;
            req.lba = e->lba + s;
            req.buf_phys = BC_LIN(i) + (s << 9);
            req.buf_sel = bc_wsel;
            req.buf_off = s << 9;
            drv_request(b->drv);
            if (req.status == DRV_OK)
                break;
        }
        if (req.status != DRV_OK) {
            vio_kputs(boot_session, "\n\xFE Cache: Schreibfehler, Daten verloren!\n");
            e->valid &= ~(1 << s);
        }
        e->dirty &= ~(1 << s);
        blk_dirty[e->dev] = 1;
    }
    if (bc_ndirty)
        bc_ndirty--;
}

/* Platz fuer einen Block: frei, sonst ohne ref-Bit und sauber, sonst ohne
   ref-Bit (vorher schreiben) */
static unsigned bc_victim(void)
{
    unsigned n, i, dirty_c = BC_END;
    struct bce __far *e;
    for (n = 0; n < 2 * bc_n; n++) {
        i = bc_hand;
        if (++bc_hand >= bc_n)
            bc_hand = 0;
        e = BCE(i);
        if (e->dev == -1)
            return i;
        if (e->dev < 0)
            continue;                   /* Stueck fehlt                  */
        if (e->ref) {
            e->ref = 0;
            continue;
        }
        if (!e->dirty)
            return i;
        if (dirty_c == BC_END)
            dirty_c = i;
    }
    return dirty_c;                     /* BC_END: kein Platz            */
}

/* Sektor aus DMABUF+off in den Cache legen (oder dort erneuern) -> Block */
static unsigned bc_put(int dev, unsigned long lba, unsigned off, int stream)
{
    unsigned long blba = lba & ~3UL;
    unsigned s = (unsigned)lba & 3, i, h;
    struct bce __far *e;
    if (!bc_n || blk[dev].bsize != 512)
        return BC_END;
    i = bc_lookup(dev, blba);
    if (i == BC_END) {
        i = bc_victim();
        if (i == BC_END)
            return BC_END;
        e = BCE(i);
        if (e->dev >= 0) {
            if (e->dirty)
                bc_wb(i);
            bc_unlink(i);
        }
        e->lba = blba;
        e->dev = (signed char)dev;
        e->valid = e->dirty = e->ref = 0;
        h = bc_hash(dev, blba);
        e->next = BCH(h);
        BCH(h) = i;
    }
    e = BCE(i);
    if (stream && (e->dirty & (1 << s)))
        return i;                       /* Cache ist neuer als die Platte */
    if (!stream)
        e->ref = 1;
    bc_win(i);
    far_copy(bc_wsel, s << 9, DMABUF, off, 512);
    e->valid |= 1 << s;
    return i;
}

static void bc_drop_dev(int dev)        /* Diskette gewechselt           */
{
    unsigned i;
    for (i = 0; i < bc_n; i++)
        if (BCE(i)->dev == dev && !BCE(i)->dirty)
            bc_unlink(i);
}

/* alle verzoegerten Daten schreiben */
static void bc_flush(void)
{
    unsigned i;
    for (i = 0; i < bc_n && bc_ndirty; i++)
        if (BCE(i)->dev >= 0 && BCE(i)->dirty)
            bc_wb(i);
    bc_ndirty = 0;
}

/* Frist abgelaufen? Dann schreiben (mit Kernelsperre gerufen) */
static int bc_due(void)
{
    return bc_ndirty && get_ticks() - bc_since >= bc_delay;
}

static int bc_writeback(int dev)
{
    return bc_delay && !bc_sync && !IS_FLOPPY(dev);
}

/* Diskette gewechselt? Der Treiber erkennt Wechsel und Format; dann
   Groesse neu und ihre Cache-Eintraege weg. -> 1 gewechselt/neu */
int blk_media(int dev)
{
    int r;
    if (dev < 0 || !IS_FLOPPY(dev))
        return 0;
    r = fd_check(blk[dev].unit);
    blk[dev].blocks = fd_size(blk[dev].unit);
    if (r) {
        lc_dev = -1;
        bc_drop_dev(dev);
    }
    return r;
}

/* kein Medium: Diskette (Wechselleitung/READ ID) oder CD (der Treiber
   meldet beim letzten Lesen DRV_ERR_NOTREADY) */
int blk_nomedia(int dev)
{
    if (dev < 0)
        return 0;
    if (IS_FLOPPY(dev))
        return fd_nomedia(blk[dev].unit);
    return blk_nm[dev];
}

/* Diskette kann gewechselt sein: ihre Eintraege weg */
void blk_invalidate(void)
{
    int dev;
    lc_dev = -1;
    for (dev = 0; dev < nblk; dev++)
        if (IS_FLOPPY(dev))
            bc_drop_dev(dev);
}

/* Treiberaufruf: n Bloecke ab lba nach DMABUF; 0 = ok */
static int dev_read(int dev, unsigned long lba, unsigned n)
{
    struct blkdev *b = &blk[dev];

    if (b->drv < 0)
        return fd_read(b->unit, (unsigned)lba) ? -1 : 0;   /* immer einer */
    req.cmd = DRV_READ;
    req.unit = b->unit;
    req.count = (unsigned char)n;
    req.status = DRV_ERR_IO;
    req.lba = lba;
    req.buf_phys = DMABUF_LIN;
    req.buf_sel = DMABUF;
    req.buf_off = 0;
    drv_request(b->drv);
    blk_nm[dev] = req.status == DRV_ERR_NOTREADY;   /* CD: keine drin */
    return req.status == DRV_OK ? 0 : -1;
}

/* Sektor lba nach DMABUF (Offset 0). Fehlt er im Cache, liest es bis zu n
   Sektoren am Stueck und legt die folgenden als Strom-Eintraege ab. */
int blk_read_n(int dev, unsigned long lba, unsigned n)
{
    struct blkdev *b;
    unsigned k, max, i, s;

    if (dev == lc_dev && lba == lc_lba)
        return 0;
    if (dev < 0 || dev >= nblk)
        return -1;
    b = &blk[dev];
    if (lba >= b->blocks)
        return -1;
    lc_dev = -1;
    s = (unsigned)lba & 3;
    if (bc_n && b->bsize == 512 && (i = bc_lookup(dev, lba & ~3UL)) != BC_END &&
        (BCE(i)->valid & (1 << s))) {
        bc_win(i);
        far_copy(DMABUF, 0, bc_wsel, s << 9, 512);
        if (n == 1)
            BCE(i)->ref = 1;
        bc_hits++;
        lc_dev = dev;
        lc_lba = lba;
        return 0;
    }
    bc_miss++;
    max = b->drv < 0 ? 1 : drv[b->drv].maxcnt;
    if (b->bsize != 512 || !bc_n)
        max = 1;                        /* ohne Cache haette es keinen Sinn */
    if (n > max)
        n = max;
    if (n > DMABUF_SIZE / 512)
        n = DMABUF_SIZE / 512;
    if (n < 1)
        n = 1;
    if (b->blocks - lba < n)
        n = (unsigned)(b->blocks - lba);
    if (dev_read(dev, lba, n)) {
        if (n == 1 || dev_read(dev, lba, 1))    /* Fehler: einzeln nochmal */
            return -1;
        n = 1;
    }
    bc_put(dev, lba, 0, n > 1);
    for (k = 1; k < n; k++)
        bc_put(dev, lba + k, k << 9, 1);
    if (bc_n && b->bsize == 512 && (i = bc_lookup(dev, lba & ~3UL)) != BC_END &&
        (BCE(i)->dirty & (1 << s))) {   /* kann nicht sein, sicher ist sicher */
        bc_win(i);
        far_copy(DMABUF, 0, bc_wsel, s << 9, 512);
    }
    lc_dev = dev;
    lc_lba = lba;
    return 0;
}

int blk_read(int dev, unsigned long lba)
{
    return blk_read_n(dev, lba, 1);
}

/* DMABUF in einen Sektor schreiben (Festplatte: verzoegert im Cache).
   0 = ok, -1 Fehler, -2 schreibgeschuetzt */
int blk_write(int dev, unsigned long lba)
{
    struct blkdev *b = &blk[dev];
    int rc;
    unsigned i, bit = 1 << ((unsigned)lba & 3);

    lc_dev = -1;
    if (dev < 0 || dev >= nblk || lba >= b->blocks)
        return -1;
    if (b->bsize != 512)
        return -2;                      /* CD-ROM                     */
    if (bc_n && bc_writeback(dev)) {    /* verzoegert                 */
        i = bc_put(dev, lba, 0, 0);
        if (i != BC_END) {
            if (!BCE(i)->dirty) {
                if (!bc_ndirty)
                    bc_since = get_ticks();
                bc_ndirty++;
            }
            BCE(i)->dirty |= bit;
            lc_dev = dev;
            lc_lba = lba;
            return 0;
        }
    }
    if (b->drv < 0) {
        rc = fd_write(b->unit, (unsigned)lba);
    } else {
        req.cmd = DRV_WRITE;
        req.unit = b->unit;
        req.count = 1;
        req.status = DRV_ERR_IO;
        req.lba = lba;
        req.buf_phys = DMABUF_LIN;
        req.buf_sel = DMABUF;
        req.buf_off = 0;
        drv_request(b->drv);
        rc = req.status == DRV_OK ? 0 : req.status == DRV_ERR_WP ? -2 : -1;
        if (!rc)
            blk_dirty[dev] = 1;
    }
    if (bc_n && b->bsize == 512 && (i = bc_lookup(dev, lba & ~3UL)) != BC_END) {
        if (rc)
            BCE(i)->valid &= ~bit;      /* Inhalt unsicher            */
        else if (BCE(i)->dirty & bit) { /* jetzt auf der Platte       */
            BCE(i)->dirty &= ~bit;
            if (!BCE(i)->dirty && bc_ndirty)
                bc_ndirty--;
        }
    }
    if (rc)
        return rc;
    bc_put(dev, lba, 0, 0);
    lc_dev = dev;                       /* DMABUF entspricht jetzt dem Sektor */
    lc_lba = lba;
    return 0;
}

/* Schreib-Caches der Platten leeren (FLUSH CACHE / SYNCHRONIZE CACHE) */
void blk_flush(void)
{
    int dev, d;
    bc_flush();                         /* verzoegerte Daten zuerst      */
    fd_sync();                          /* Spurpuffer der Diskette       */
    for (dev = 1; dev < nblk; dev++) {
        if (!blk_dirty[dev] || IS_FLOPPY(dev))
            continue;
        d = blk[dev].drv;
        for (; dev < nblk; dev++)       /* je Treiber einmal          */
            if (blk[dev].drv == d)
                blk_dirty[dev] = 0;
        req.cmd = DRV_FLUSH;
        req.status = DRV_ERR_IO;
        drv_request(d);
        dev = 0;                        /* von vorn: naechster Treiber */
    }
}

/* ---- Ton (FAPI_AUDIO) --------------------------------------------------
   Mehrere Programme duerfen das Audiogeraet oeffnen (bis NAUD). Zu hoeren
   ist das Programm der Vordergrund-Sitzung, sonst das zuletzt geoeffnete
   (Musik im Hintergrund, waehrend man in EDIT schreibt). Wer nicht zu
   hoeren ist, laeuft im Echtzeittempo weiter: seine Daten werden nach
   Laenge / Bytes je Takt verworfen - ein Spiel bleibt im Takt und ist
   nach dem Zurueckholen sofort wieder zu hoeren. Der Kernel entscheidet
   bei jedem Schreiben, der Treiber spielt einen Strom (aud_cur). */
#define NAUD 4
static struct {
    signed char   task;                 /* -1 = frei                      */
    unsigned      rate, bpt, seq;       /* bpt: Bytes je Takt (10 ms)     */
    unsigned char ch, bits;
} aud[NAUD] = { { -1 }, { -1 }, { -1 }, { -1 } };
static int aud_drv = -1, aud_cur = -1;
static unsigned aud_seq;
static struct aud_req areq[NAUD + 1];   /* je Programm eine, [NAUD] fuer
                                           START/STOP/MIXER (ein schlafender
                                           Schreiber behaelt seine)      */

static unsigned aud_call(int r, unsigned fn, unsigned a1, unsigned a2, unsigned a3)
{
    struct aud_req *q = &areq[r];
    q->cmd = DRV_AUDIO;
    q->fn = (unsigned char)fn;
    q->status = DRV_ERR_CMD;
    q->a1 = a1;
    q->a2 = a2;
    q->a3 = a3;
    q->result = 0;
    drv_call(DRVC_SEL(aud_drv), DRVD_SEL(aud_drv), drv[aud_drv].entry, (unsigned)q);
    return q->status;
}

static int aud_find(int t)
{
    int i;
    for (i = 0; i < NAUD; i++)
        if (aud[i].task == t)
            return i;
    return -1;
}

/* wer ist zu hoeren? Vordergrund, sonst der zuletzt geoeffnete */
static int aud_pick(void)
{
    int i, best = -1, fg = -1, fgs = vio_fg();
    for (i = 0; i < NAUD; i++) {
        if (aud[i].task < 0)
            continue;
        if (task[aud[i].task].session == fgs && (fg < 0 || aud[i].seq > aud[fg].seq))
            fg = i;
        if (best < 0 || aud[i].seq > aud[best].seq)
            best = i;
    }
    return fg >= 0 ? fg : best;
}

static void aud_stop(void)
{
    if (aud_cur >= 0)
        aud_call(NAUD, AR_STOP, 0, 0, 0);
    aud_cur = -1;
}

static void aud_close(int i)
{
    if (aud_cur == i)
        aud_stop();
    aud[i].task = -1;
}

#define NFM 4
struct aclient {
    signed char   task;                 /* -1 = frei                      */
    unsigned      seq;
    unsigned char rs, need, d1;         /* MIDI: laufender Status         */
    unsigned char prog[16], vol[16];
};
static struct aclient fmc[NFM] = { { -1 }, { -1 }, { -1 }, { -1 } },
                      mdc[NFM] = { { -1 }, { -1 }, { -1 }, { -1 } };
static int fm_cur = -1, md_cur = -1;
static unsigned fm_sel;                 /* NFM * 512 Byte Registerkopien  */
static struct fapi_fmreg fm_buf[64];

static int cl_find(struct aclient *c, int t)
{
    int i;
    for (i = 0; i < NFM; i++)
        if (c[i].task == t)
            return i;
    return -1;
}

/* wer ist zu hoeren? Vordergrund, sonst der zuletzt geoeffnete */
static int cl_pick(struct aclient *c)
{
    int i, best = -1, fg = -1, fgs = vio_fg();
    for (i = 0; i < NFM; i++) {
        if (c[i].task < 0)
            continue;
        if (task[c[i].task].session == fgs && (fg < 0 || c[i].seq > c[fg].seq))
            fg = i;
        if (best < 0 || c[i].seq > c[best].seq)
            best = i;
    }
    return fg >= 0 ? fg : best;
}

static void aud_task_end(int t)
{
    int i = aud_find(t);
    if (i >= 0)
        aud_close(i);
    if (aud_drv < 0)
        return;
    if ((i = cl_find(fmc, t)) >= 0) {
        if (fm_cur == i) {
            aud_call(NAUD, AR_FMRESET, 0, 0, 0);
            fm_cur = -1;
        }
        fmc[i].task = -1;
    }
    if ((i = cl_find(mdc, t)) >= 0) {
        if (md_cur == i) {
            aud_call(NAUD, AR_MIDIRESET, 0, 0, 0);
            md_cur = -1;
        }
        mdc[i].task = -1;
    }
}

/* FM und MIDI: dieselbe Vorrangregel. FM: der Kernel haelt je Programm
   alle 512 OPL-Register (Kernelsegment), beim Wechsel laedt er die des
   neuen Programms (Tasten los). MIDI: je Kanal Programm und Lautstaerke,
   beim Wechsel alle Noten aus und den Stand des neuen Programms. */
static unsigned char __far *fm_shadow(int i)
{
    return MK_FP(fm_sel, (unsigned)i * 512u);
}

/* Register des Programms i in den OPL laden (B0h-B8h ohne Taste) */
static void fm_load(int i)
{
    unsigned char __far *sh = fm_shadow(i);
    struct aud_req *q = &areq[NAUD];
    unsigned r, n = 0;
    static const unsigned first[4] = { 0x105, 0x104, 0x001, 0x008 };

    aud_call(NAUD, AR_FMRESET, 0, 0, 0);
    for (r = 0; r < 4 + 2 * 0xD6; r++) {
        unsigned reg = r < 4 ? first[r] : ((r - 4) < 0xD6 ? 0x20 + (r - 4) : 0x120 + (r - 4 - 0xD6));
        unsigned char v = sh[reg];
        if ((reg & 0xFF) >= 0xB0 && (reg & 0xFF) <= 0xB8)
            v &= ~0x20;
        fm_buf[n].reg = reg;
        fm_buf[n].val = v;
        if (++n == 64 || r == 4 + 2 * 0xD6 - 1) {
            q->sel = KDATA;
            q->off = (unsigned)fm_buf;
            q->len = n;
            aud_call(NAUD, AR_FMWRITE, 0, 0, 0);
            n = 0;
        }
    }
}

/* MIDI-Strom mitlesen: Programm (Cn) und Lautstaerke (Bn 07) je Kanal */
static void md_track(int i, unsigned char c)
{
    if (c >= 0xF8)
        return;                         /* Echtzeit                      */
    if (c & 0x80) {
        mdc[i].rs = c >= 0xF0 ? 0 : c;
        mdc[i].need = (unsigned char)(((c & 0xF0) == 0xC0 || (c & 0xF0) == 0xD0) ? 1 : 2);
        mdc[i].d1 = 0xFF;
        return;
    }
    if (!mdc[i].rs)
        return;
    if (mdc[i].need == 1) {
        if ((mdc[i].rs & 0xF0) == 0xC0)
            mdc[i].prog[mdc[i].rs & 15] = c;
        return;
    }
    if (mdc[i].d1 == 0xFF) {
        mdc[i].d1 = c;
        return;
    }
    if ((mdc[i].rs & 0xF0) == 0xB0 && mdc[i].d1 == 7)
        mdc[i].vol[mdc[i].rs & 15] = c;
    mdc[i].d1 = 0xFF;                   /* naechstes Paar (laufender Status) */
}

static void md_load(int i)
{
    static unsigned char m[16 * 5];
    struct aud_req *q = &areq[NAUD];
    unsigned k, n = 0;
    aud_call(NAUD, AR_MIDIRESET, 0, 0, 0);
    for (k = 0; k < 16; k++) {
        m[n++] = (unsigned char)(0xC0 | k);
        m[n++] = mdc[i].prog[k];
        m[n++] = (unsigned char)(0xB0 | k);
        m[n++] = 7;
        m[n++] = mdc[i].vol[k];
    }
    q->sel = KDATA;
    q->off = (unsigned)m;
    q->len = n;
    aud_call(NAUD, AR_MIDIWRITE, 0, 0, 0);
}

static unsigned aud_caps(void)
{
    aud_call(NAUD, AR_CAPS, 0, 0, 0);
    return areq[NAUD].result;
}

static unsigned aud_fapi(unsigned usel, unsigned fn, unsigned a, unsigned b)
{
    int i = aud_find(current), w;
    struct aud_req *q;

    if (aud_drv < 0)
        return FAPI_ERROR;
    switch (fn) {
    case AUD_CAPS:
        return aud_caps();
    case AUD_FMOPEN:
    case AUD_MIDIOPEN: {
        int fm = fn == AUD_FMOPEN, k;
        if (!(aud_caps() & (fm ? (AUDCAP_OPL2 | AUDCAP_OPL3) : AUDCAP_MPU401)))
            return FAPI_ERROR;
        if (fm && !fm_sel && !(fm_sel = kseg_alloc(NFM * 512UL)))
            return FAPI_ERROR;
        i = cl_find(fm ? fmc : mdc, current);
        if (i < 0)
            for (i = 0; i < NFM && (fm ? fmc[i].task : mdc[i].task) >= 0; i++)
                ;
        if (i == NFM)
            return FAPI_ERR_INUSE;
        if (fm) {
            unsigned char __far *sh = fm_shadow(i);
            for (k = 0; k < 512; k++)
                sh[k] = (unsigned char)(((k & 0xFF) >= 0x40 && (k & 0xFF) <= 0x55) ? 0x3F : 0);
            fmc[i].task = (signed char)current;
            fmc[i].seq = ++aud_seq;
            if (fm_cur == i)
                fm_cur = -1;
        } else {
            for (k = 0; k < 16; k++) {
                mdc[i].prog[k] = 0;
                mdc[i].vol[k] = 100;
            }
            mdc[i].rs = 0;
            mdc[i].task = (signed char)current;
            mdc[i].seq = ++aud_seq;
            if (md_cur == i)
                md_cur = -1;
        }
        return 0;
    }
    case AUD_FMWRITE: {
        struct fapi_fmreg __far *f = MK_FP(usel, a);
        unsigned k;
        i = cl_find(fmc, current);
        if (i < 0 || b > 512 || !user_ok(usel, a, b * sizeof(*f), 0))
            return FAPI_ERROR;
        for (k = 0; k < b; k++)
            fm_shadow(i)[f[k].reg & 0x1FF] = f[k].val;
        w = cl_pick(fmc);
        if (w == i && fm_cur != i) {
            fm_load(i);                 /* Stand laden (Tasten los) ...  */
            fm_cur = i;
        }
        if (fm_cur == i) {              /* ... und diesen Auftrag dazu   */
            q = &areq[i];
            q->sel = usel;
            q->off = a;
            q->len = b;
            aud_call(i, AR_FMWRITE, 0, 0, 0);
        }
        return 0;
    }
    case AUD_MIDIWRITE: {
        unsigned char __far *m = MK_FP(usel, a);
        unsigned k;
        i = cl_find(mdc, current);
        if (i < 0 || !user_ok(usel, a, b, 0))
            return FAPI_ERROR;
        for (k = 0; k < b; k++)
            md_track(i, m[k]);
        w = cl_pick(mdc);
        if (w == i && md_cur != i) {
            md_load(i);
            md_cur = i;
        }
        if (md_cur == i) {
            q = &areq[i];
            q->sel = usel;
            q->off = a;
            q->len = b;
            aud_call(i, AR_MIDIWRITE, 0, 0, 0);
        }
        return 0;
    }
    case AUD_FMCLOSE:
    case AUD_MIDICLOSE: {
        int fm = fn == AUD_FMCLOSE;
        i = cl_find(fm ? fmc : mdc, current);
        if (i < 0)
            return FAPI_ERROR;
        if (fm) {
            if (fm_cur == i) {
                aud_call(NAUD, AR_FMRESET, 0, 0, 0);
                fm_cur = -1;
            }
            fmc[i].task = -1;
        } else {
            if (md_cur == i) {
                aud_call(NAUD, AR_MIDIRESET, 0, 0, 0);
                md_cur = -1;
            }
            mdc[i].task = -1;
        }
        return 0;
    }
    case AUD_OPEN: {
        struct fapi_audfmt __far *f = MK_FP(usel, a);
        unsigned bytes;
        if (!user_ok(usel, a, sizeof(*f), 0))
            return FAPI_ERROR;
        if (i >= 0)
            aud_close(i);
        for (i = 0; i < NAUD && aud[i].task >= 0; i++)
            ;
        if (i == NAUD)
            return FAPI_ERR_INUSE;
        if (aud_call(NAUD, AR_CHECK, f->rate, f->channels, f->bits) != DRV_OK)
            return FAPI_ERROR;
        bytes = (unsigned)f->channels * (f->bits >> 3);
        aud[i].rate = f->rate;
        aud[i].ch = f->channels;
        aud[i].bits = f->bits;
        aud[i].bpt = (f->rate / 100) * bytes;
        aud[i].seq = ++aud_seq;
        aud[i].task = (signed char)current;
        return 0;
    }
    case AUD_WRITE:
        if (i < 0 || !user_ok(usel, a, b, 0))
            return FAPI_ERROR;
        w = aud_pick();
        if (w == i && aud_cur != i) {           /* wechseln              */
            aud_stop();
            if (aud_call(NAUD, AR_START, aud[i].rate, aud[i].ch, aud[i].bits) == DRV_OK)
                aud_cur = i;
        }
        if (aud_cur == i) {
            q = &areq[i];
            q->sel = usel;
            q->off = a;
            q->len = b;
            aud_call(i, AR_WRITE, 0, 0, 0);
            return b;                           /* Rest ging beim Wechsel unter */
        }
        if (aud[i].bpt)                         /* stumm: Echtzeit abwarten */
            sleep_ticks(b / aud[i].bpt + 1);
        return b;
    case AUD_DRAIN:
        if (i >= 0 && aud_cur == i)
            aud_call(i, AR_DRAIN, 0, 0, 0);
        return i >= 0 ? 0 : FAPI_ERROR;
    case AUD_CLOSE:
        if (i < 0)
            return FAPI_ERROR;
        aud_close(i);
        return 0;
    case AUD_MIXER:
        if (aud_call(NAUD, AR_MIXER, a, b, 0) != DRV_OK)
            return FAPI_ERROR;
        return areq[NAUD].result;
    case AUD_INFO:
        if (!user_ok(usel, a, 64, 1))
            return FAPI_ERROR;
        q = &areq[NAUD];
        q->sel = usel;
        q->off = a;
        return aud_call(NAUD, AR_INFO, 0, 0, 0) == DRV_OK ? 0 : FAPI_ERROR;
    }
    return FAPI_ERROR;
}

/* Interruptroutinen der Treiber (start.asm: irq_common) */
extern unsigned char kbd_aux;           /* start.asm: PS/2-Maus aktiv    */
extern unsigned short irq_hoff[16], irq_hcs[16], irq_hds[16];

/* Treiber zu seinem Datenselektor, -1 = keiner */
static int drv_of(unsigned dsel)
{
    int d;
    for (d = 0; d < NDRV; d++)
        if (drv[d].used && DRVD_SEL(d) == (dsel & ~3))
            return d;
    return -1;
}

/* DevHlp: Dienste des Kernels fuer Treiber */
unsigned devhlp_dispatch(unsigned fn, unsigned dsel, unsigned bx, unsigned cx)
{
    const char __far *p;
    unsigned n, f;
    int d;

    switch (fn) {
    case DH_SETIRQ:
        d = drv_of(dsel);
        /* 0 Takt, 1 Tastatur, 2 Kaskade, 6 Diskette gehoeren dem Kernel */
        if (d < 0 || bx > 15 || bx < 3 || bx == 6 || irq_hcs[bx])
            return 1;
        f = irq_off();
        irq_hoff[bx] = cx;
        irq_hds[bx] = DRVD_SEL(d);
        irq_hcs[bx] = DRVC_SEL(d);
        irq_restore(f);
        pic_mask &= ~(1u << bx);
        if (bx >= 8)
            pic_mask &= ~4u;            /* Kaskade zum zweiten PIC       */
        pic_update();
        return 0;
    case DH_UNSETIRQ:
        d = drv_of(dsel);
        if (d < 0 || bx > 15 || irq_hcs[bx] != DRVC_SEL(d))
            return 1;
        pic_mask |= 1u << bx;
        pic_update();
        f = irq_off();
        irq_hcs[bx] = 0;
        irq_restore(f);
        return 0;
    case DH_PROCBLOCK:
        d = drv_of(dsel);
        return proc_block(((unsigned long)(d & 0xFF) << 16) | bx, cx);
    case DH_PROCRUN:
        d = drv_of(dsel);
        return proc_run(((unsigned long)(d & 0xFF) << 16) | bx);
    case DH_MOUREG:
        vio_mou_reg(bx);
        return 0;
    case DH_MOUEVENT:
        vio_mou_event(bx, (int)cx, (int)devhlp_dx);
        return 0;
    case DH_KBDLOCK:
        kbd_aux = (unsigned char)((bx & 2) != 0);
        if (bx & 1)
            pic_mask |= 2u;
        else
            pic_mask &= ~2u;
        pic_update();
        return 0;
    case DH_BLKREAD:
        return blk_read((int)bx, ((unsigned long)devhlp_dx << 16) | cx) ? 1 : 0;
    case DH_PUTS:
        p = MK_FP(dsel, bx);
        for (n = 0; n < 200 && p[n]; n++)
            ;
        vio_tty(boot_session, dsel, bx, n);
        return 0;
    case DH_TICKS:
        return (unsigned)get_ticks();
    case DH_DELAY:
        delay_ticks(bx);
        return 0;
    case DH_DMABUF: {
        static unsigned long lin;
        n = kseg_dma(bx, &lin);
        if (n)
            *(unsigned long __far *)MK_FP(dsel, cx) = lin;
        return n;
    }
    case DH_AUDREG:
        aud_drv = drv_of(dsel);
        return aud_drv < 0;
    case DH_BIOSDISK:
        if (bx > 1 || bx >= bios_hdcount)
            return 1;
        for (n = 0; n < 16; n++)
            ((unsigned char __far *)MK_FP(dsel, cx))[n] = bios_fdpt[bx * 16 + n];
        return 0;
    }
    return 0xFFFF;
}

/* nachgeladene Dateisysteme (IFS=): Treiberplatz und Medienklassen */
#define NIFSMOD 2
static struct { int drv; unsigned classes; } ifsmod[NIFSMOD];
static int nifsmod;

/* Treiber oder IFS-Modul laden und initialisieren;
   -> Anzahl Einheiten (IFS: 1) oder -1 */
static int load_driver(const char *line, int is_ifs)
{
    static char path[IFS_PATHLEN], canon[IFS_PATHLEN];
    struct fx_header h;
    unsigned long base, seg;
    unsigned u, n, entry;
    int d, k, drive, err;
    const char *args = line;

    for (d = 0; d < NDRV; d++)
        if (!drv[d].used)
            break;
    if (d == NDRV)
        return -1;
    for (k = 0; *args && *args != ' ' && k < IFS_PATHLEN - 1; k++)
        path[k] = *args++;
    path[k] = 0;
    while (*args == ' ')
        args++;
    if (resolve(-1, path, &drive, canon))
        return -1;
    err = load_image(drive, canon, &h, &u, &n, &seg);
    for (k = 0; path[k] && path[k] != '\\' && path[k] != ':'; k++)
        ;
    if (err == EXEC_ERR_NOTFOUND && !path[k]) {    /* nur Name: \DOS\SYSTEM, \DOS */
        static char alt[IFS_PATHLEN];
        scopy(alt, "\\DOS\\SYSTEM", sizeof(alt));
        scopy(alt + 11, canon, sizeof(alt) - 11);
        err = load_image(drive, alt, &h, &u, &n, &seg);
        if (err == EXEC_ERR_NOTFOUND) {
            scopy(alt + 4, canon, sizeof(alt) - 4);
            err = load_image(drive, alt, &h, &u, &n, &seg);
        }
    }
    if (err) {
        vio_kputs(boot_session, "  ");
        vio_kputs(boot_session, path);
        vio_kputs(boot_session, err == EXEC_ERR_NOTFOUND ? ": nicht gefunden\n" :
                                err == EXEC_ERR_NOMEM ? ": kein Speicher\n" :
                                                        ": kein Treiber\n");
        return -1;
    }
    if (ld_cu && !ld_shared)            /* Programm (V2) statt Treiber   */
        mem_free(ld_cu, ld_cn);
    /* Kopf am Anfang des Abbilds: "DRV1", Einsprung */
    if (!(h.flags & FX_FLAG_DRIVER) || h.version != FX_VERSION ||
        rd16(SCRATCH2, 0) != 0x5244 ||
        rd16(SCRATCH2, 2) != 0x3156 || (entry = rd16(SCRATCH2, 4)) >= h.image_size) {
        mem_free(u, n);
        vio_kputs(boot_session, "  ");
        vio_kputs(boot_session, path);
        vio_kputs(boot_session, ": kein Treiber\n");
        return -1;
    }
    base = (unsigned long)u << 8;
    set_desc(DRVC_IDX(d), base, h.image_size - 1, ACC_KCODE);
    set_desc(DRVC_IDX(d) + 1, base, (unsigned)(seg - 1), ACC_KDATA);
    drv[d].used = 1;
    for (k = 0, n = 0; path[k]; k++)                /* Name ohne Pfad    */
        if (path[k] == '\\' || path[k] == ':')
            n = k + 1;
    scopy(drv[d].name, path + n, sizeof(drv[d].name));
    n = 0;
    drv[d].entry = entry;
    drv[d].unit = u;
    drv[d].cnt = n;

    req.cmd = DRV_INIT;
    req.status = DRV_ERR_NODEV;
    req.units = 0;
    req.count = 0;
    req.base = base;
    req.buf_phys = DMABUF_LIN;
    req.buf_sel = DMABUF;
    req.buf_off = 0;
    req.devhlp_off = (unsigned)devhlp_entry;
    req.devhlp_sel = KCODE;
    for (k = 0; k < DRV_MAXUNITS; k++)
        req.bsize[k] = 0;
    scopy(req.args, args, sizeof(req.args));
    drv_request(d);

    if (req.status != DRV_OK || (req.units == 0 && is_ifs)) {
        /* nichts gefunden: Treiber wieder entfernen (DRV_OK mit 0
           Einheiten: Treiber ohne Blockgeraete, z. B. MOUSE.SYS) */
        gdt[DRVC_IDX(d)].access = 0;
        gdt[DRVC_IDX(d) + 1].access = 0;
        mem_free(u, n);
        drv[d].used = 0;
        vio_kputs(boot_session, "  ");
        vio_kputs(boot_session, path);
        vio_kputs(boot_session, is_ifs ? ": Initialisierung fehlgeschlagen\n"
                                       : ": kein Ger\x84t gefunden\n");
        return 0;
    }
    drv[d].maxcnt = req.count ? req.count : 1;
    if (is_ifs) {
        if (nifsmod < NIFSMOD) {
            ifsmod[nifsmod].drv = d;
            ifsmod[nifsmod].classes = req.units;
            nifsmod++;
        }
        return 1;
    }
    for (k = 0; k < req.units && nblk < NBLK; k++) {
        blk[nblk].drv = (signed char)d;
        blk[nblk].unit = (unsigned char)k;
        blk[nblk].blocks = req.blocks[k];
        blk[nblk].bsize = req.bsize[k] ? req.bsize[k] : 512;
        nblk++;
    }
    return req.units;
}

/* Partitionstabelle lesen und FAT-Partitionen als C:, D:, ... einhaengen */
static int next_drive = 2;

#define IS_FATPART(t) ((t) == 0x01 || (t) == 0x04 || (t) == 0x06 || (t) == 0x0E)
#define IS_EXTPART(t) ((t) == 0x05 || (t) == 0x0F)

static unsigned long rd32(unsigned sel, unsigned off)
{
    return rd16(sel, off) | ((unsigned long)rd16(sel, off + 2) << 16);
}

static void mount_one(int dev, unsigned long start, unsigned long size,
                      unsigned char type, unsigned part)
{
    vol[next_drive].psize = size;
    ifs_mount(next_drive, dev, start, &fat_driver);  /* auch unformatiert:
                                       Buchstabe fuer FORMAT    */
    vol[next_drive].present = 1;
    vol[next_drive].ptype = type;
    vol[next_drive].part = (unsigned char)part;
    next_drive++;
}

/* Partitionen einer Platte: erst die primaeren (FAT), dann die logischen
   Laufwerke in einer erweiterten Partition (05h/0Fh, Kette von EBRs: im
   EBR Eintrag 1 = logisches Laufwerk relativ zum EBR, Eintrag 2 = naechster
   EBR relativ zum Anfang der erweiterten Partition - wie bei MS-DOS) */
static void mount_disk(int dev)
{
    unsigned char __far *b;
    unsigned char t[4], lt, nt;
    unsigned long st[4], sz[4], ebr, ls, lz, ns;
    int i, n;

    if (blk[dev].bsize != 512)
        return;                         /* CD-ROM: siehe cd_drives()    */
    if (blk_read(dev, 0))
        return;
    b = MK_FP(DMABUF, 0);
    if (b[510] != 0x55 || b[511] != 0xAA)
        return;
    for (i = 0; i < 4; i++) {           /* Tabelle merken (ifs_mount    */
        t[i] = b[0x1BE + 16 * i + 4];   /* benutzt DMABUF)              */
        st[i] = rd32(DMABUF, 0x1BE + 16 * i + 8);
        sz[i] = rd32(DMABUF, 0x1BE + 16 * i + 12);
    }
    for (i = 0; i < 4 && next_drive < NDRIVE; i++)
        if (IS_FATPART(t[i]))
            mount_one(dev, st[i], sz[i], t[i], (unsigned)i + 1);
    for (i = 0; i < 4; i++) {
        if (!IS_EXTPART(t[i]))
            continue;
        ebr = st[i];
        for (n = 0; n < 24 && next_drive < NDRIVE; n++) {
            if (blk_read(dev, ebr))
                break;
            b = MK_FP(DMABUF, 0);
            if (b[510] != 0x55 || b[511] != 0xAA)
                break;
            lt = b[0x1BE + 4];
            ls = rd32(DMABUF, 0x1BE + 8);
            lz = rd32(DMABUF, 0x1BE + 12);
            nt = b[0x1CE + 4];
            ns = rd32(DMABUF, 0x1CE + 8);
            if (IS_FATPART(lt) && lz)
                mount_one(dev, ebr + ls, lz, lt, 5 + (unsigned)n);
            if (!IS_EXTPART(nt) || !ns)
                break;
            ebr = st[i] + ns;
        }
    }
}

/* Startpartition unter den eingehaengten Laufwerken: gleicher Startsektor
   und gleiche Seriennummer wie der Bootsektor, den das BIOS geladen hat */
static int find_boot_vol(void)
{
    int d;
    for (d = 2; d < NDRIVE; d++)
        if (vol[d].used && vol[d].drv == &fat_driver &&
            vol[d].start == boot_hidden && blk_read(vol[d].blk, vol[d].start) == 0 &&
            rd16(DMABUF, 39) == (unsigned)boot_serial &&
            rd16(DMABUF, 41) == (unsigned)(boot_serial >> 16))
            return d;
    return -1;
}

/* ---- Start ------------------------------------------------------------- */
static char startup[512];

/* Datei vom Laufwerk A: nach startup[] lesen; -> Laenge oder -1 */
static int read_cfg(const char *name)
{
    struct ifs_file f;
    if (ifs_open(sys_drive, name, &f) || f.size > sizeof(startup))
        return -1;
    if (ifs_read(&f, 0, KDATA, (unsigned)startup, (unsigned)f.size) != (int)f.size)
        return -1;
    return (int)f.size;
}

/* naechste Zeile aus CONFIG.SYS: REM und Leerzeilen zaehlen nicht */
static int next_line(unsigned *p, unsigned len, char *out)
{
    unsigned k;
    while (*p < len) {
        k = 0;
        while (*p < len && (startup[*p] == ' ' || startup[*p] == '\t'))
            (*p)++;
        while (*p < len && startup[*p] != '\r' && startup[*p] != '\n') {
            if (k < FAPI_ARGLEN - 1)
                out[k++] = startup[*p];
            (*p)++;
        }
        while (*p < len && (startup[*p] == '\r' || startup[*p] == '\n'))
            (*p)++;
        out[k] = 0;
        if (k == 0)
            continue;
        if (k >= 3 && (out[0] | 32) == 'r' && (out[1] | 32) == 'e' &&
            (out[2] | 32) == 'm' && (k == 3 || out[3] == ' '))
            continue;
        return 1;
    }
    return 0;
}

static const char *exec_err(int e)
{
    switch (e) {
    case EXEC_ERR_NOTFOUND: return "nicht gefunden";
    case EXEC_ERR_NOMEM:    return "kein Speicher";
    case EXEC_ERR_FORMAT:   return "kein FAPI-Programm";
    }
    return "keine Task frei";
}

/* ---- Bruecke zu nachgeladenen Dateisystemen --------------------------- */
static struct ifs_req ireq;

static int mod_call(int drive, unsigned char cmd)
{
    int d = vol[drive].mod;
    ireq.cmd = cmd;
    ireq.status = 1;
    ireq.dev = (unsigned short)vol[drive].blk;
    drv_call(DRVC_SEL(d), DRVD_SEL(d), drv[d].entry, (unsigned)&ireq);
    return ireq.status ? -1 : 0;
}

static int mod_mount(int drive)
{
    blk_invalidate();                   /* CD koennte gewechselt sein  */
    if (mod_call(drive, IFSM_MOUNT) == 0)
        return 0;
    if (blk_nomedia(vol[drive].blk))
        ifs_notready = 1;               /* FAPI meldet "nicht bereit"   */
    return -1;
}

static int mod_open(int drive, const char *path, struct ifs_file __far *f)
{
    scopy(ireq.path, path, IFS_PATHLEN);
    if (mod_mount(drive) || mod_call(drive, IFSM_OPEN))
        return -1;
    far_copy(FP_SEG(f), FP_OFF(f), KDATA, (unsigned)&ireq.f, sizeof(*f));
    f->drive = (unsigned char)drive;
    f->flags = 0;
    return 0;
}

static int mod_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
                    unsigned off, unsigned len)
{
    far_copy(KDATA, (unsigned)&ireq.f, FP_SEG(f), FP_OFF(f), sizeof(*f));
    ireq.pos = pos;
    ireq.sel = (unsigned short)sel;
    ireq.off = (unsigned short)off;
    ireq.len = (unsigned short)len;
    if (mod_call(f->drive, IFSM_READ))
        return -1;
    far_copy(FP_SEG(f), FP_OFF(f), KDATA, (unsigned)&ireq.f, sizeof(*f));
    return ireq.result;
}

static int mod_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *de)
{
    scopy(ireq.path, dir, IFS_PATHLEN);
    ireq.n = (unsigned short)n;
    if ((n == 0 && mod_mount(drive)) || mod_call(drive, IFSM_READDIR))
        return -1;
    far_copy(KDATA, (unsigned)de, KDATA, (unsigned)&ireq.de, sizeof(*de));
    return 0;
}

static int mod_isdir(int drive, const char *path)
{
    scopy(ireq.path, path, IFS_PATHLEN);
    return mod_mount(drive) == 0 && mod_call(drive, IFSM_ISDIR) == 0;
}

static int mod_label(int drive, char *lbl, unsigned long *serial, const char *newlbl)
{
    if (newlbl)
        return IFS_WP;
    if (mod_mount(drive) || mod_call(drive, IFSM_LABEL))
        return IFS_ERR;
    scopy(lbl, ireq.de.name, 12);
    *serial = 0;
    return 0;
}

static int mod_namepath(int drive, const char *path, char *out, int shortn)
{
    scopy(ireq.path, path, IFS_PATHLEN);
    ireq.n = (unsigned short)shortn;
    if (mod_mount(drive) || mod_call(drive, IFSM_NAMEPATH))
        return -1;
    scopy(out, ireq.path, IFS_PATHLEN);
    return 0;
}

struct ifs_driver mod_driver = {
    "IFS", mod_mount, mod_open, mod_read, mod_readdir, mod_isdir,
    0, 0, 0, 0, 0, 0, 0, 0, 0, mod_label, mod_namepath
};

/* CD-Laufwerke bekommen Buchstaben, wenn ein IFS-Modul CDs kann. Der
   Datentraeger wird erst beim Zugriff geprueft (er kann fehlen/wechseln). */
static void cd_drives(void)
{
    int dev, m;

    for (dev = 1; dev < nblk && next_drive < PRE_DRIVE; dev++) {
        if (blk[dev].bsize != 2048)
            continue;
        for (m = 0; m < nifsmod && !(ifsmod[m].classes & IFS_CLASS_CD); m++)
            ;
        if (m == nifsmod) {
            vio_kputs(boot_session, "\xFE CD-ROM ohne Dateisystem (IFS=ISO9660.IFS fehlt)\n");
            continue;
        }
        vol[next_drive].used = 1;
        vol[next_drive].blk = dev;
        vol[next_drive].start = 0;
        vol[next_drive].drv = &mod_driver;
        vol[next_drive].mod = ifsmod[m].drv;
        vol[next_drive].part = 0;
        next_drive++;
    }
}

/* CONFIG.SYS wie MS-DOS. Wirksam: DEVICE=, DEVICEHIGH= (wie DEVICE, im
   Protected Mode ist alles "hoch"), IFS=, FILES=, CACHE=kb[,s] (Platten-
   Cache, s Sekunden verzoegertes Schreiben, 0 = sofort; BUFFERS=n ohne
   CACHE= gibt n * 512 Byte, mindestens 16 KB). Angenommen
   und ohne Bedeutung im Protected Mode: STACKS=, FCBS=, BREAK=,
   LASTDRIVE=, DOS=, SHELL=, COUNTRY=, INSTALL=, NUMLOCK=, SWITCHES=, SET=
   und die Menuebefehle von MS-DOS 6. Anderes wird gemeldet und uebergangen. */
static unsigned nbuffers = 15;          /* BUFFERS=, fuer den Platten-Cache */

/* letzten Pfadteil in Originalschreibung fuer das Dateisystem merken */
static void set_orig(const char *p)
{
    const char *b = p;
    unsigned k;
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            b = p + 1;
    for (k = 0; b[k] && k < IFS_NAMELEN - 1; k++)
        ifs_orig[k] = b[k];
    ifs_orig[k] = 0;
}
static char protshell[FAPI_ARGLEN] = "CMD.EXE /P";   /* PROTSHELL=   */

static int kw(const char *l, const char *k)
{
    while (*k && *l == *k)
        l++, k++;
    return *k == 0 && *l == 0;
}

static unsigned num_arg(const char *v)
{
    unsigned n = 0;
    while (*v >= '0' && *v <= '9')
        n = n * 10 + (unsigned)(*v++ - '0');
    return n;
}

static void config_sys(void)
{
    static char line[FAPI_ARGLEN];
    static const char *const quiet[] = {
        "STACKS", "FCBS", "BREAK", "LASTDRIVE", "DOS", "SHELL", "COUNTRY",
        "INSTALL", "INSTALLHIGH", "NUMLOCK", "SWITCHES", "SET", "MENUITEM",
        "MENUDEFAULT", "MENUCOLOR", "SUBMENU", "INCLUDE", "DRIVPARM", 0
    };
    unsigned p = 0, n;
    int size, dev0, i;
    char *v;

    size = read_cfg("\\CONFIG.SYS");
    if (size < 0)
        return;
    while (next_line(&p, (unsigned)size, line)) {
        if (line[0] == ';')
            continue;
        for (v = line; *v && *v != '='; v++)
            ;
        if (!*v)
            continue;                   /* [MENU]-Abschnitte u. ae.     */
        *v++ = 0;
        while (*v == ' ')
            v++;
        for (i = 0; line[i] && line[i] != ' ' && line[i] != '\t'; i++)
            line[i] = up(line[i]);
        if (line[i]) {                  /* SET NAME=WERT (MS-DOS 6)     */
            static char ent[FAPI_ARGLEN];
            char *nm = line + i;
            unsigned k2 = 0;
            line[i] = 0;
            if (kw(line, "SET")) {
                while (*++nm == ' ' || *nm == '\t')
                    ;
                while (*nm && k2 < sizeof(ent) - 2)
                    ent[k2++] = *nm++;
                ent[k2++] = '=';
                scopy(ent + k2, v, sizeof(ent) - k2);
                if (env_set(-1, ent))
                    vio_kputs(boot_session, "\xFE CONFIG.SYS: Umgebung voll\n");
                continue;
            }
        }
        line[i] = 0;                    /* "DEVICEHIGH /L:1 " -> Wort   */
        if (kw(line, "DEVICE") || kw(line, "DEVICEHIGH")) {
            while (*v == '/' || (up(v[0]) == 'S' && up(v[1]) == 'I' && up(v[2]) == 'Z' &&
                                 up(v[3]) == 'E' && v[4] == '=')) {
                while (*v && *v != ' ')             /* /L:1,1234 SIZE=... */
                    v++;
                while (*v == ' ')
                    v++;
            }
            vio_kputs(boot_session, "\xFE DEVICE=");
            vio_kputs(boot_session, v);
            vio_kputs(boot_session, "\n");
            dev0 = nblk;
            if (load_driver(v, 0) > 0)
                for (i = dev0; i < nblk; i++)
                    mount_disk(i);
        } else if (kw(line, "IFS")) {
            vio_kputs(boot_session, "\xFE IFS=");
            vio_kputs(boot_session, v);
            vio_kputs(boot_session, "\n");
            load_driver(v, 1);
        } else if (kw(line, "FILES")) {
            n = num_arg(v);
            nfiles = n < 8 ? 8 : n > 255 ? 255 : n;
        } else if (kw(line, "AUTOCHECK")) {     /* wie OS/2 /AUTOCHECK:  */
            static char ac[40];
            scopy(ac, "AUTOCHECK=", sizeof(ac));
            scopy(ac + 10, *v ? v : "-", sizeof(ac) - 10);
            env_set(-1, ac);
        } else if (kw(line, "PROTSHELL")) {
            scopy(protshell, v, sizeof(protshell));
        } else if (kw(line, "BUFFERS")) {
            n = num_arg(v);
            nbuffers = n < 1 ? 1 : n > 99 ? 99 : n;
        } else if (kw(line, "CACHE")) {        /* CACHE=1024[,5]         */
            const char *q = v;
            cache_kb = num_arg(v);
            while (*q && *q != ',')
                q++;
            if (*q == ',')
                cache_s = num_arg(q + 1);
        } else if (kw(line, "LONGNAMES")) {    /* AUTO, VFAT, EA          */
            lfn_cfg = (up(v[0]) == 'V') ? 1 : (up(v[0]) == 'E') ? 2 : 0;
        } else {
            for (i = 0; quiet[i] && !kw(line, quiet[i]); i++)
                ;
            if (!quiet[i]) {
                vio_kputs(boot_session, "\xFE CONFIG.SYS: unbekannt: ");
                vio_kputs(boot_session, line);
                vio_kputs(boot_session, "\n");
            }
        }
    }
    cd_drives();
}

/* Laufwerke mit ihren endgueltigen Buchstaben anzeigen (nach vol_move) */
static int fd_ok;

static const char *fd_name(unsigned blocks)
{
    return blocks == 720 ? "360 KB" : blocks == 1440 ? "720 KB" : blocks == 2400 ? "1,2 MB" :
           blocks == 5760 ? "2,88 MB" : "1,44 MB";
}

static void list_drives(void)
{
    static char msg[80];
    static const unsigned tsize[6] = { 0, 720, 2400, 1440, 2880, 5760 };
    int d, n;
    struct blkdev *b;

    for (d = (boot_drive & 0x80) ? 0 : 1; d < 2; d++)
        if (fd_ok && fd_type(d)) {          /* Diskettenlaufwerke         */
            vio_kputs(boot_session, d ? "\xFE Laufwerk B: Diskette " : "\xFE Laufwerk A: Diskette ");
            vio_kputs(boot_session, fd_name(tsize[fd_type(d)]));
            vio_kputs(boot_session, "\n");
        }

    for (d = 2; d < NDRIVE; d++) {
        if (!(vol[d].used || vol[d].present) || d == PRE_DRIVE)
            continue;
        b = &blk[vol[d].blk];
        msg[0] = (char)('A' + d);
        scopy(msg + 1, ": ", sizeof(msg) - 1);
        n = 3;
        if (vol[d].drv == &mod_driver) {
            scopy(msg + n, "CD-ROM", sizeof(msg) - n);
        } else {
            if (vol[d].part >= 5) {     /* in der erweiterten Partition */
                scopy(msg + n, "logisch ", sizeof(msg) - n);
                n += 8;
                udec(vol[d].part - 4, msg + n);
                while (msg[n])
                    n++;
            } else {
                scopy(msg + n, "Partition ", sizeof(msg) - n);
                n += 10;
                msg[n++] = (char)('0' + vol[d].part);
            }
            scopy(msg + n, !vol[d].used ? ", nicht formatiert, " :
                           vol[d].fat16 ? ", FAT16, " : ", FAT12, ", sizeof(msg) - n);
            while (msg[n])
                n++;
            udec((unsigned)(vol[d].psize >> 11), msg + n);
            while (msg[n])
                n++;
            scopy(msg + n, " MB", sizeof(msg) - n);
        }
        while (msg[n])
            n++;
        if (b->drv >= 0) {
            scopy(msg + n, "  (", sizeof(msg) - n);
            n += 3;
            scopy(msg + n, drv[b->drv].name, sizeof(msg) - n);
            while (msg[n])
                n++;
            scopy(msg + n, ", Einheit ", sizeof(msg) - n);
            n += 10;
            msg[n++] = (char)('0' + b->unit);
            msg[n++] = ')';
        }
        msg[n++] = '\n';
        msg[n] = 0;
        vio_kputs(boot_session, "\xFE Laufwerk ");
        vio_kputs(boot_session, msg);
    }
}

void kmain(void)
{
    char line[FAPI_ARGLEN], num[6];
    unsigned k;
    int s0, t, i;

    pic_pit_init();
    if (fpu_ok) {                       /* 287: Protected Mode, IRQ 13 frei */
        fpu_pm_init();
        pic_mask &= ~((1u << 13) | 4u);
        pic_update();
    }

    /* 286-Deskriptoren haben 24 Bit Basis: mehr als 16 MB gibt es fuer
       AT-DOS 5.0 nicht, auch wenn ein 486 mit 128 MB darunter steckt */
    if (mem_conv_kb > 640)
        mem_conv_kb = 640;
    if (mem_ext_kb > 15360 || !a20_ok)
        mem_ext_kb = a20_ok ? 15360 : 0;
    /* frei ab dem Ende der Kernel-Daten (Code und Daten liegen ab 10000h) */
    mem_init((unsigned)((KDATA_LIN + kdata_size + MEM_UNIT - 1) / MEM_UNIT),
             mem_conv_kb, mem_ext_kb);
    for (i = 0; i < NDRIVE; i++)        /* vor dem ersten resolve()   */
        subst_drv[i] = -1;
    /* SUBST- und JOIN-Pfade teilen sich ein Kernelsegment */
    subst_sel = kseg_alloc(2UL * NDRIVE * IFS_PATHLEN + (unsigned long)NTASK * NDCWD * DCWD_SZ);
    for (i = 0; i < NDRIVE; i++)
        join_drv[i] = -1;
    join_sel = subst_sel;
    for (i = 0; i < NTASK; i++)
        dcwd_clear(i);

    s0 = vio_open("SYSTEM");
    if (s0 < 0)
        panic("Kein Speicher fuer die erste Sitzung.");
    boot_session = s0;
    vio_focus(s0);
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0D);       /* Cursor: Unterstrich   */
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0E);

    vio_kputs(s0, "AT-DOS 5.0 - 80286 Protected Mode\n");
    vio_kputs(s0, a20_ok ? "\xFE A20 frei, " : "\xFE A20 GESPERRT, nur unter 1 MB, ");
    udec(mem_free_kb(), num);
    vio_kputs(s0, num);
    vio_kputs(s0, " KB Arbeitsspeicher frei\n");

    enable_ints();                  /* Diskette und Tastatur per IRQ     */
    kbd_init();
    env_sel = kseg_alloc((NTASK + 1) * FAPI_ENVSIZE);  /* < 64 KB          */
    if (!env_sel)
        panic("Kein Speicher f\x81r die Umgebung.");
    /* Disketten: Typen aus dem CMOS (10h); A: ist Geraet 0, B: Geraet 1 */
    k = cmos(0x10);
    if (!(boot_drive & 0x80) && !(k & 0xF0))
        k |= 0x40;                          /* von A: gestartet: 1,44 MB */
    blk[BLK_FLOPPY].drv = -1;
    blk[BLK_FLOPPY].unit = 0;
    blk[BLK_FLOPPY].blocks = 2880;
    blk[BLK_FLOPPY].bsize = 512;
    nblk = 1;
    fd_ok = fd_init((unsigned)k) == 0;
    if (fd_ok && fd_type(1)) {
        blk[1].drv = -1;
        blk[1].unit = 1;
        blk[1].blocks = fd_size(1);
        blk[1].bsize = 512;
        nblk = 2;
        floppy_lazy |= 2;                   /* B: beim ersten Zugriff    */
        vol[1].present = 1;
        vol[1].blk = 1;
        vol[1].drv = &fat_driver;
    }
    if (!(boot_drive & 0x80)) {
        /* Start von Diskette: A: ist Pflicht */
        if (!fd_ok)
            panic("Diskettencontroller antwortet nicht.");
        vol[0].present = 1;
        blk_media(BLK_FLOPPY);
        if (ifs_mount(0, BLK_FLOPPY, 0, &fat_driver))
            panic("Diskette A: hat kein lesbares FAT-Dateisystem.");
        vio_kputs(s0, "\xFE Laufwerk A: Diskette ");
        vio_kputs(s0, fd_name(fd_size(0)));
        vio_kputs(s0, vol[0].fat16 ? ", FAT16\n" : ", FAT12\n");
        config_sys();
    } else {
        /* Start von Festplatte: A: erst beim ersten Zugriff einhaengen */
        if (fd_ok && fd_type(0)) {
            floppy_lazy |= 1;
            vol[0].present = 1;             /* auch unformatiert (FORMAT) */
            vol[0].blk = BLK_FLOPPY;
            vol[0].drv = &fat_driver;
        }
        vio_kputs(s0, "\xFE Start von Festplatte, ");
        udec(npre, num);
        vio_kputs(s0, num);
        vio_kputs(s0, " Datei(en) vorgeladen\n");
        if (pre_err)
            vio_kputs(s0, pre_err == PRE_ERR_FS ? "  Startpartition ist nicht FAT16\n" :
                          pre_err == PRE_ERR_FULL ? "  Vorladebereich voll (30 KB)\n" :
                                                    "  BIOS-Lesefehler beim Vorladen\n");
        ifs_mount(PRE_DRIVE, -1, 0, &pre_driver);
        sys_drive = PRE_DRIVE;
        config_sys();
        vol[PRE_DRIVE].used = 0;                /* Z: wieder weg        */
        sys_drive = find_boot_vol();
        if (sys_drive < 0)
            panic("Startpartition nicht gefunden - Treiber in CONFIG.SYS?");
        vio_kputs(s0, "\xFE Startlaufwerk C:\n");
        if (sys_drive != 2) {                   /* immer C:, Rest rueckt nach */
            vol_move(sys_drive, 2);
            sys_drive = 2;
        }
    }

    list_drives();
    if (cache_kb == 0xFFFF) {           /* ohne CACHE=: nach Speicher    */
        cache_kb = mem_ext_kb >= 4096 ? 1024 : mem_ext_kb >= 1536 ? 256 :
                   mem_ext_kb >= 512 ? 128 : 32;
        if (cache_kb < nbuffers / 2)
            cache_kb = nbuffers / 2;
    }
    blk_cache_init(cache_kb);           /* nach CONFIG.SYS: CACHE=       */

    /* Systemumgebung: COMSPEC wie bei MS-DOS */
    scopy(line, "COMSPEC=A:\\CMD.EXE", sizeof(line));
    line[8] = (char)('A' + sys_drive);
    env_set(-1, line);
    {                                           /* TEMP/TMP wie Windows:  */
        char td = (char)('A' + sys_drive);      /* Startlaufwerk, von     */
        if (sys_drive < 2 && vol_present(2) && vol[2].used)
            td = 'C';                           /* Diskette lieber C:     */
        if (env_find(-1, "TEMP", 4) < 0) {
            scopy(line, "TEMP=A:\\TEMP", sizeof(line));
            line[5] = td;
            env_set(-1, line);
        }
        if (env_find(-1, "TMP", 3) < 0) {
            scopy(line, "TMP=A:\\TEMP", sizeof(line));
            line[4] = td;
            env_set(-1, line);
        }
    }
    if (env_find(-1, "PATH", 4) < 0) {          /* Vorgabe: X:\DOS;X:\ */
        scopy(line, "PATH=A:\\DOS;A:\\", sizeof(line));
        line[5] = line[12] = (char)('A' + sys_drive);
        env_set(-1, line);
    }

    /* Dateitabelle nach FILES= */
    k = kseg_alloc(nfiles * sizeof(struct ofile));   /* < 64 KB, ohne __U4M */
    if (!k)
        panic("Kein Speicher f\x81r die Dateitabelle (FILES=).");
    files = MK_FP(k, 0);
    for (i = 0; i < (int)nfiles; i++)
        files[i].owner = -1;                    /* refs = 0: frei        */
    files[CONS].kind = OF_CON;                  /* Eintrag 0: Konsole    */
    files[CONS].refs = 255;
    pipe_sel = kseg_alloc(NPIPE * PIPESZ);
    if (!pipe_sel)
        panic("Kein Speicher f\x81r Pipes.");

    /* Befehlsinterpreter aus PROTSHELL= (Vorgabe CMD.EXE /P) in der
       ersten Sitzung; er fuehrt AUTOEXEC.CMD aus. SHELL= bleibt wie bei
       OS/2 dem Befehlsinterpreter der DOS-Box (ab 6.0) vorbehalten.
       Fehlt er (alte CONFIG.SYS mit COMMAND.EXE nach dem Umbenennen),
       wird CMD.EXE /P genommen. */
    t = task_create(protshell, s0, -1, -1);
    if (t < 0) {
        vio_kputs(s0, "\xFE PROTSHELL=");
        vio_kputs(s0, protshell);
        vio_kputs(s0, " - ");
        vio_kputs(s0, exec_err(-t));
        vio_kputs(s0, ", nehme CMD.EXE /P\n");
        scopy(protshell, "CMD.EXE /P", sizeof(protshell));
        t = task_create(protshell, s0, -1, -1);
        if (t < 0)
            panic("Kein Befehlsinterpreter.");
    }
    vio_title(s0, task[t].name);
    fd_motor_off();
    vio_kputs(s0, "\n");
    vio_focus(s0);

    load_tr(TSS_K);
    schedule_next();                /* kehrt nie zurueck                 */
}
