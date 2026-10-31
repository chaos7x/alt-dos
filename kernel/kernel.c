/*
 * AT-DOS 5.0 - Kernel, Meilenstein 4
 *
 * 80286 Protected Mode, segmentiert, kooperatives Multitasking.
 * Anwendungen laufen in Ring 3, jede mit eigenem TSS, eigener LDT und
 * ohne IOPL. Hardware und Speicher erreichen sie ausschliesslich ueber
 * die Family API (FAPI) per Call Gate. Programme laufen in Sitzungen mit
 * eigenem Bildschirm; die erste Sitzung startet PROTSHELL= (COMMAND.EXE /P).
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
#define KERNEL_KB   64

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

extern unsigned short isr_table[48];
extern unsigned short mem_conv_kb, mem_ext_kb;   /* vom BIOS, im Real Mode */
extern unsigned char  a20_ok;                     /* im Real Mode geprueft  */
extern unsigned short fapi_dx;                    /* viertes FAPI-Argument  */

/* ---- Kerneldaten ------------------------------------------------------- */
struct desc     gdt[NGDT];
struct gate     idt[48];
unsigned short  gdtr[3], idtr[3];

#define T_FREE  0
#define T_READY 1           /* laeuft oder schlaeft bis 'wake'           */
#define T_WAIT  2           /* wartet auf das Ende eines Kindprogramms   */

struct task {
    unsigned char state;
    unsigned long wake;
    int           session;
    int           parent;   /* -1: Sitzungsinhaber                       */
    char          name[13];                       /* fuer die Titelzeile   */
    char          args[FAPI_ARGLEN];
    unsigned char drive;                          /* aktuelles Laufwerk    */
    char          cwd[IFS_PATHLEN];               /* "" = Stamm, "\DOS"   */
    unsigned      img_unit, img_cnt;
    unsigned      sig_tramp, sig_mask;          /* DosSetSigHandler      */
    unsigned char std[3];                       /* Eintraege fuer Handle 0-2 */
    unsigned char bg;                           /* EXEC_BG: weckt/schliesst nichts */
    unsigned char sig_pending, in_sig;
    unsigned short sig_save[15];                /* Rahmen + AX waehrend des Handlers */
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

void reboot_request(void)
{
    if (reboot_count)
        reboot();
    reboot_count = 500;
}

/* alle Dateien schliessen, Platten-Caches leeren; dann Neustart oder
   anhalten (ein 286 kann sich nicht selbst ausschalten - APM gibt es erst
   ab 1992). Beim Anhalten bleibt Strg+Alt+Entf aktiv: Tastatur-IRQ setzt
   reboot_count, IRQ 0 startet nach 5 s neu. */
static void task_end(int t);
static void schedule_next(void);

/* Programme mit SIG_SHUTDOWN-Handler duerfen aufraeumen (bis 5 s), danach
   werden alle uebrigen Programme regulaer beendet */
static void end_programs(void)
{
    unsigned long until;
    int t, n;

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
            for (t = 0, n = 0; t < NTASK; t++)
                if (t != current && task[t].state != T_FREE &&
                    (task[t].sig_mask & SIG_SHUTDOWN))
                    n++;
        }
    }
    /* die Sitzung, in der die Abschlussmeldung steht, bleibt stehen */
    for (t = 0; t < NTASK; t++)
        if (t != current && task[t].state != T_FREE && task[t].session != boot_session)
            task_end(t);
}

static void system_down(int restart)
{
    int k;
    reboot_count = 0;
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

static unsigned seg_free(int t, unsigned sel)
{
    unsigned idx = sel >> 3, units, start;
    struct desc *d;

    if (!(sel & 4) || idx < LDT_FIRST_FREE || idx >= NLDT)
        return FAPI_ERROR;
    d = &ldt[t][idx];
    if (d->access == 0)
        return FAPI_ERROR;
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

static int resolve(int t, const char *in, int *drive, char *out)
{
    const char *p = in, *q;
    int d = t >= 0 ? task[t].drive : sys_drive, n, k;

    if (p[0] && p[1] == ':') {
        if (up(p[0]) < 'A' || up(p[0]) > 'Z')
            return -1;
        d = up(p[0]) - 'A';
        p += 2;
    }
    n = 0;
    out[0] = 0;
    if (*p != '\\' && *p != '/' && t >= 0 && d == task[t].drive) {
        scopy(out, task[t].cwd, IFS_PATHLEN);
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
static int load_image(int drive, const char *path, struct fx_header *h,
                      unsigned *unit, unsigned *cnt, unsigned long *segsize)
{
    struct ifs_file f;
    unsigned long base, seg;
    unsigned tu, tn, u, n, fx;
    int err = 0;

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
    seg = (unsigned long)h->image_size + h->bss_size + h->stack_size;
    if (h->sig[0] != FX_SIG0 || h->sig[1] != FX_SIG1 || h->version != FX_VERSION ||
        (unsigned long)h->image_off + h->image_size > f.size ||
        seg > 0x10000UL || h->image_size == 0 || h->entry >= h->image_size)
        err = EXEC_ERR_FORMAT;
    if (err) {
        mem_free(tu, tn);
        return err;
    }

    n = UNITS(seg);
    u = mem_alloc(n);
    if (u == 0) {
        mem_free(tu, tn);
        return EXEC_ERR_NOMEM;
    }
    base = (unsigned long)u << 8;
    /* Fenster ueber alle belegten Einheiten, nicht nur bis seg - 1:
       zero_seg schreibt ganze Einheiten, ein echter 286 prueft die Grenze */
    scratch2_map(base, n * MEM_UNIT - 1);
    zero_seg(SCRATCH2, n * (MEM_UNIT / 2));     /* BSS und Stack = 0    */
    far_copy(SCRATCH2, 0, SCRATCH, h->image_off, h->image_size);
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
        return EXEC_ERR_FORMAT;                 /* Treiber, kein Programm */
    }
    base = (unsigned long)u << 8;
    task[t].img_unit = u;
    task[t].img_cnt  = n;
    image_units     += n;
    for (n = 0; n < NLDT; n++)
        ldt[t][n].access = 0;
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
    if (from >= 0)
        scopy(task[t].cwd, task[from].cwd, IFS_PATHLEN);
    else
        task[t].cwd[0] = 0;
    task[t].session = session;
    task[t].parent  = parent;
    task[t].wake    = 0;
    task[t].sig_tramp = task[t].sig_mask = 0;
    task[t].sig_pending = task[t].in_sig = 0;
    task[t].bg = 0;
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

    for (idx = LDT_FIRST_FREE; idx < NLDT; idx++)
        if (ldt[t][idx].access != 0)
            seg_free(t, USEL(idx));
    if (task[t].img_unit) {
        mem_free(task[t].img_unit, task[t].img_cnt);
        image_units -= task[t].img_cnt;
        task[t].img_unit = 0;
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

/* ---- Scheduler (kooperativ) -------------------------------------------- */
static void switch_to(int t)
{
    current = t;
    task_jump(TSS_SEL(t));          /* Hardware-Taskwechsel ueber das TSS */
}

/* Strg+C: aktives Programm der Vordergrund-Sitzung beenden. Der Inhaber
   der ersten Sitzung (die Shell aus PROTSHELL=) ist geschuetzt. */
static void do_break(void)
{
    int s = vio_fg(), t;
    for (t = 0; t < NTASK; t++)
        if (task[t].state == T_READY && task[t].session == s)
            break;
    if (t == NTASK)
        return;
    if (task[t].sig_mask & SIG_BREAK) {         /* Programm faengt Strg+C */
        task[t].sig_pending |= SIG_BREAK;
        task[t].wake = 0;
        return;
    }
    if (task[t].parent < 0 && s == 0)
        return;
    vio_kputs(s, "^C\n");
    task_end(t);
}

static void schedule_next(void)
{
    int n, t, any;
    unsigned long now;

    for (;;) {
        if (kbd_switch_req) {
            kbd_switch_req = 0;
            vio_next();
        }
        if (kbd_break_req) {
            kbd_break_req = 0;
            do_break();
        }
        if (reboot_count)
            shutdown();
        now = get_ticks();
        vio_status();
        any = 0;
        for (n = 1; n <= NTASK; n++) {
            t = (current + n + NTASK) % NTASK;
            if (task[t].state == T_FREE)
                continue;
            any = 1;
            if (task[t].state != T_READY || task[t].wake > now)
                continue;
            if (t != current)
                switch_to(t);
            return;
        }
        if (!any)
            panic("Alle Programme beendet - System angehalten.");
        idle_wait();                /* alle warten: HLT bis zum Takt     */
    }
}

static void sleep_ticks(unsigned n)
{
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
        if (a[i] != b[i])
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
            if (task[k].state != T_FREE && task[k].drive == d &&
                task[k].cwd[0] && path_in(task[k].cwd, canon))
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
    else if ((o->kind == OF_PRD && !files[pipes[o->pipe].wr].refs) ||
             (o->kind == OF_PWR && !files[pipes[o->pipe].rd].refs))
        pipes[o->pipe].used = 0;        /* beide Enden zu                */
}

static void of_ref(int k)
{
    if (k != CONS)
        files[k].refs++;
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

static unsigned fapi_inner(unsigned fn, unsigned usel, unsigned a1, unsigned a2);

unsigned fapi_dispatch(unsigned fn, unsigned usel, unsigned a1, unsigned a2)
{
    return sig_deliver(fapi_inner(fn, usel, a1, a2));
}

static unsigned fapi_inner(unsigned fn, unsigned usel, unsigned a1, unsigned a2)
{
    int row, col, s = task[current].session, t, k;
    unsigned a3 = fapi_dx;
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
        if (row >= SROWS || col >= SCOLS)
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
        p[2] = KERNEL_KB;
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
        } else if ((t = ifs_create(d, canon, &files[k].f)) != 0) {
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
            fn == FAPI_MKDIR  ? ifs_mkdir(d, canon) : ifs_rmdir(d, canon);
        return t ? ifs_err(t) : 0;

    case FAPI_MOVE: {
        static char canon2[IFS_PATHLEN];
        int d2;
        if (user_str(usel, a1, buf, sizeof(buf)) ||
            resolve(current, buf, &d, canon) ||
            user_str(usel, a2, buf, sizeof(buf)) ||
            resolve(current, buf, &d2, canon2) || d != d2)
            return FAPI_ERROR;
        if (path_in(canon2, canon))
            return FAPI_ERROR;                  /* in sich selbst       */
        if (in_use(d, canon, 1))
            return FAPI_ERR_INUSE;
        t = ifs_move(d, canon, canon2);
        return t ? ifs_err(t) : 0;
    }

    case FAPI_FREESPACE: {
        static unsigned long kb;
        if (a1 >= NDRIVE || !user_ok(usel, a2, 4, 1) || ifs_space(a1, &kb))
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
               files[k].kind == OF_FILE ? HT_FILE : HT_PIPE;

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
        if (resolve(current, buf, &d, canon) || ifs_readdir(d, canon, a2, &de))
            return FAPI_ERROR;
        far_copy(usel, a1, KDATA, (unsigned)&de, sizeof(de));
        return 0;

    case FAPI_CHDIR:
        if (user_str(usel, a1, buf, sizeof(buf)) ||
            resolve(current, buf, &d, canon) || !ifs_isdir(d, canon))
            return FAPI_ERROR;
        if (d != task[current].drive)
            return FAPI_ERROR;                  /* nur aktuelles Laufwerk */
        scopy(task[current].cwd, canon[1] ? canon : "", IFS_PATHLEN);
        return 0;

    case FAPI_QCURDIR:                          /* "C:\DOS"            */
        if (!user_ok(usel, a1, IFS_PATHLEN + 2, 1))
            return FAPI_ERROR;
        buf[0] = (char)('A' + task[current].drive);
        buf[1] = ':';
        scopy(buf + 2, task[current].cwd[0] ? task[current].cwd : "\\", IFS_PATHLEN);
        far_copy(usel, a1, KDATA, (unsigned)buf, IFS_PATHLEN + 2);
        return 0;

    case FAPI_SELECTDISK:
        if (a1 >= NDRIVE || !vol[a1].used)
            return FAPI_ERROR;
        if (a1 != task[current].drive) {
            task[current].drive = (unsigned char)a1;
            task[current].cwd[0] = 0;
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

    case FAPI_YIELD:
        schedule_next();
        return 0;

    case FAPI_SLEEP:
        sleep_ticks((a1 + 9) / 10);
        return 0;

    case FAPI_EXIT:
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
static void pic_pit_init(void)
{
    /* PICs auf Vektor 20h/28h legen, sonst kollidiert IRQ0 mit #DF */
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xBC); outb(0xA1, 0xFF);     /* IRQ 0 Takt, 1 Tastatur, 6 Diskette */

    /* PIT Kanal 0: 100 Hz */
    outb(0x43, 0x36);
    outb(0x40, 11932 & 0xFF);
    outb(0x40, 11932 >> 8);
}

/* ---- Blockgeraete ----------------------------------------------------- */
struct blkdev blk[NBLK];
int nblk;

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

static void drv_request(int d)
{
    drv_call(DRVC_SEL(d), DRVD_SEL(d), drv[d].entry, (unsigned)&req);
}

/* zuletzt gelesener oder geschriebener Sektor, steht noch in DMABUF */
static int lc_dev = -1;
static unsigned long lc_lba;
static unsigned char blk_dirty[NBLK];   /* seit dem letzten Leeren beschrieben */

/* ---- Sektor-Cache (BUFFERS=) -------------------------------------------
   Ein Kernelsegment mit bc_n Sektoren zu 512 Byte, nur fuer 512er-Geraete.
   Schreiben geht sofort durch (write-through), der Cache wird nur mit
   aktualisiert. Daten, die fat_read() vorausliest, sind "Strom"-Eintraege:
   sie werden zuerst verdraengt, damit eine grosse Datei nicht FAT und
   Verzeichnisse aus dem Cache wirft. */
#define BC_MAX      99
#define BC_STREAM   8                   /* so viele Strom-Eintraege bleiben */
static unsigned       bc_sel, bc_n, bc_clock, bc_nstream;
static signed char    bc_dev[BC_MAX];   /* -1 = frei                    */
static unsigned char  bc_stream[BC_MAX];
static unsigned long  bc_lba[BC_MAX];
static unsigned       bc_age[BC_MAX];
unsigned long         bc_hits, bc_miss; /* fuer spaetere Statistik      */

void blk_cache_init(unsigned n)
{
    unsigned i;
    if (n > BC_MAX)
        n = BC_MAX;
    bc_sel = kseg_alloc((unsigned long)n << 9);
    bc_n = bc_sel ? n : 0;
    for (i = 0; i < BC_MAX; i++)
        bc_dev[i] = -1;
}

static int bc_find(int dev, unsigned long lba)
{
    unsigned i;
    for (i = 0; i < bc_n; i++)
        if (bc_dev[i] == dev && bc_lba[i] == lba)
            return (int)i;
    return -1;
}

/* Platz fuer einen neuen Eintrag: frei, sonst aeltester Strom-Eintrag
   (wenn genug davon da sind), sonst aeltester ueberhaupt */
static unsigned bc_victim(void)
{
    unsigned i, v = 0, best = 0xFFFF, age;
    int stream_only = bc_nstream >= BC_STREAM;

    for (i = 0; i < bc_n; i++) {
        if (bc_dev[i] < 0)
            return i;
        if (stream_only && !bc_stream[i])
            continue;
        age = bc_clock - bc_age[i];
        if (best == 0xFFFF || age > best) {
            best = age;
            v = i;
        }
    }
    return v;
}

/* Sektor aus DMABUF+off in den Cache legen (oder dort erneuern) */
static void bc_put(int dev, unsigned long lba, unsigned off, int stream)
{
    int i;
    if (!bc_n || blk[dev].bsize != 512)
        return;
    i = bc_find(dev, lba);
    if (i < 0) {
        i = (int)bc_victim();
        if (bc_dev[i] >= 0 && bc_stream[i])
            bc_nstream--;
        bc_dev[i] = (signed char)dev;
        bc_lba[i] = lba;
        bc_stream[i] = (unsigned char)stream;
        if (stream)
            bc_nstream++;
    } else if (!stream && bc_stream[i]) {
        bc_stream[i] = 0;               /* wieder gebraucht: bleibt     */
        bc_nstream--;
    }
    bc_age[i] = ++bc_clock;
    far_copy(bc_sel, (unsigned)i << 9, DMABUF, off, 512);
}

/* Diskette kann gewechselt sein: ihre Eintraege weg */
void blk_invalidate(void)
{
    unsigned i;
    lc_dev = -1;
    for (i = 0; i < bc_n; i++)
        if (bc_dev[i] == BLK_FLOPPY) {
            if (bc_stream[i])
                bc_nstream--;
            bc_dev[i] = -1;
        }
}

/* Treiberaufruf: n Bloecke ab lba nach DMABUF; 0 = ok */
static int dev_read(int dev, unsigned long lba, unsigned n)
{
    struct blkdev *b = &blk[dev];

    if (b->drv < 0)
        return fd_read((unsigned)lba) ? -1 : 0;     /* immer einer   */
    req.cmd = DRV_READ;
    req.unit = b->unit;
    req.count = (unsigned char)n;
    req.status = DRV_ERR_IO;
    req.lba = lba;
    req.buf_phys = DMABUF_LIN;
    req.buf_sel = DMABUF;
    req.buf_off = 0;
    drv_request(b->drv);
    return req.status == DRV_OK ? 0 : -1;
}

/* Sektor lba nach DMABUF (Offset 0). Fehlt er im Cache, liest es bis zu n
   Sektoren am Stueck und legt die folgenden als Strom-Eintraege ab. */
int blk_read_n(int dev, unsigned long lba, unsigned n)
{
    struct blkdev *b;
    unsigned k, max;
    int i;

    if (dev == lc_dev && lba == lc_lba)
        return 0;
    if (dev < 0 || dev >= nblk)
        return -1;
    b = &blk[dev];
    if (lba >= b->blocks)
        return -1;
    lc_dev = -1;
    if (bc_n && b->bsize == 512 && (i = bc_find(dev, lba)) >= 0) {
        far_copy(DMABUF, 0, bc_sel, (unsigned)i << 9, 512);
        bc_age[i] = ++bc_clock;
        if (n == 1 && bc_stream[i]) {
            bc_stream[i] = 0;
            bc_nstream--;
        }
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
    lc_dev = dev;
    lc_lba = lba;
    return 0;
}

int blk_read(int dev, unsigned long lba)
{
    return blk_read_n(dev, lba, 1);
}

/* DMABUF in einen Sektor schreiben (sofort; der Cache wird mitgefuehrt).
   0 = ok, -1 Fehler, -2 schreibgeschuetzt */
int blk_write(int dev, unsigned long lba)
{
    struct blkdev *b = &blk[dev];
    int rc, i;

    lc_dev = -1;
    if (dev < 0 || dev >= nblk || lba >= b->blocks)
        return -1;
    if (b->bsize != 512)
        return -2;                      /* CD-ROM                     */
    if (b->drv < 0) {
        rc = fd_write((unsigned)lba);
        if (rc) {
            if (bc_n && (i = bc_find(dev, lba)) >= 0)
                bc_dev[i] = -1, bc_nstream -= bc_stream[i];
            return rc;
        }
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
        if (req.status != DRV_OK) {
            if (bc_n && (i = bc_find(dev, lba)) >= 0)
                bc_dev[i] = -1, bc_nstream -= bc_stream[i];
            return req.status == DRV_ERR_WP ? -2 : -1;
        }
        blk_dirty[dev] = 1;
    }
    bc_put(dev, lba, 0, 0);
    lc_dev = dev;                       /* DMABUF entspricht jetzt dem Sektor */
    lc_lba = lba;
    return 0;
}

/* Schreib-Caches der Platten leeren (FLUSH CACHE / SYNCHRONIZE CACHE) */
void blk_flush(void)
{
    int dev, d;
    for (dev = 1; dev < nblk; dev++) {
        if (!blk_dirty[dev])
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

/* DevHlp: Dienste des Kernels fuer Treiber */
unsigned devhlp_dispatch(unsigned fn, unsigned dsel, unsigned bx, unsigned cx)
{
    const char __far *p;
    unsigned n;

    switch (fn) {
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
    if (err == EXEC_ERR_NOTFOUND && !path[k]) {    /* nur Name: auch in \DOS */
        static char alt[IFS_PATHLEN];
        scopy(alt, "\\DOS", sizeof(alt));
        scopy(alt + 4, canon, sizeof(alt) - 4);
        err = load_image(drive, alt, &h, &u, &n, &seg);
    }
    if (err) {
        vio_kputs(boot_session, "  ");
        vio_kputs(boot_session, path);
        vio_kputs(boot_session, err == EXEC_ERR_NOTFOUND ? ": nicht gefunden\n" :
                                err == EXEC_ERR_NOMEM ? ": kein Speicher\n" :
                                                        ": kein Treiber\n");
        return -1;
    }
    /* Kopf am Anfang des Abbilds: "DRV1", Einsprung */
    if (!(h.flags & FX_FLAG_DRIVER) || rd16(SCRATCH2, 0) != 0x5244 ||
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

    if (req.status != DRV_OK || req.units == 0) {
        /* nichts gefunden: Treiber wieder entfernen */
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

static void mount_disk(int dev)
{
    unsigned char __far *b;
    unsigned long start;
    unsigned char type;
    int i, mounted = 0;

    if (blk[dev].bsize != 512)
        return;                         /* CD-ROM: siehe cd_drives()    */
    if (blk_read(dev, 0))
        return;
    b = MK_FP(DMABUF, 0);
    if (b[510] != 0x55 || b[511] != 0xAA)
        return;
    for (i = 0; i < 4 && next_drive < NDRIVE; i++) {
        if (blk_read(dev, 0))
            return;
        b = MK_FP(DMABUF, 0);
        type = b[0x1BE + 16 * i + 4];
        start = rd16(DMABUF, 0x1BE + 16 * i + 8) |
                ((unsigned long)rd16(DMABUF, 0x1BE + 16 * i + 10) << 16);
        if (type != 0x01 && type != 0x04 && type != 0x06 && type != 0x0E)
            continue;
        if (ifs_mount(next_drive, dev, start, &fat_driver))
            continue;
        vol[next_drive].part = (unsigned char)(i + 1);
        next_drive++;
        mounted++;
    }
    (void)mounted;
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
    return mod_call(drive, IFSM_MOUNT);
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

struct ifs_driver mod_driver = {
    "IFS", mod_mount, mod_open, mod_read, mod_readdir, mod_isdir
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
   Protected Mode ist alles "hoch"), IFS=, FILES=, BUFFERS=. Angenommen
   und ohne Bedeutung im Protected Mode: STACKS=, FCBS=, BREAK=,
   LASTDRIVE=, DOS=, SHELL=, COUNTRY=, INSTALL=, NUMLOCK=, SWITCHES=, SET=
   und die Menuebefehle von MS-DOS 6. Anderes wird gemeldet und uebergangen. */
static unsigned nbuffers = 15;          /* BUFFERS=, fuer den Platten-Cache */
static char protshell[FAPI_ARGLEN] = "COMMAND.EXE /P";   /* PROTSHELL=   */

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
        } else if (kw(line, "PROTSHELL")) {
            scopy(protshell, v, sizeof(protshell));
        } else if (kw(line, "BUFFERS")) {
            n = num_arg(v);
            nbuffers = n < 1 ? 1 : n > 99 ? 99 : n;
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
static void list_drives(void)
{
    static char msg[80];
    int d, n;
    struct blkdev *b;

    for (d = 2; d < NDRIVE; d++) {
        if (!vol[d].used || d == PRE_DRIVE)
            continue;
        b = &blk[vol[d].blk];
        msg[0] = (char)('A' + d);
        scopy(msg + 1, ": ", sizeof(msg) - 1);
        n = 3;
        if (vol[d].drv == &mod_driver) {
            scopy(msg + n, "CD-ROM", sizeof(msg) - n);
        } else {
            scopy(msg + n, "Partition ", sizeof(msg) - n);
            n += 10;
            msg[n++] = (char)('0' + vol[d].part);
            scopy(msg + n, vol[d].fat16 ? ", FAT16, " : ", FAT12, ", sizeof(msg) - n);
            n += 9;
            udec((unsigned)(vol[d].sectors >> 11), msg + n);
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

    /* 286-Deskriptoren haben 24 Bit Basis: mehr als 16 MB gibt es fuer
       AT-DOS 5.0 nicht, auch wenn ein 486 mit 128 MB darunter steckt */
    if (mem_conv_kb > 640)
        mem_conv_kb = 640;
    if (mem_ext_kb > 15360 || !a20_ok)
        mem_ext_kb = a20_ok ? 15360 : 0;
    mem_init(mem_conv_kb, mem_ext_kb);

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
    blk[BLK_FLOPPY].drv = -1;
    blk[BLK_FLOPPY].unit = 0;
    blk[BLK_FLOPPY].blocks = 2880;
    blk[BLK_FLOPPY].bsize = 512;
    nblk = 1;
    if (!(boot_drive & 0x80)) {
        /* Start von Diskette: A: ist Pflicht */
        if (fd_init())
            panic("Diskettencontroller antwortet nicht.");
        if (ifs_mount(0, BLK_FLOPPY, 0, &fat_driver))
            panic("Diskette A: hat kein lesbares FAT-Dateisystem.");
        vio_kputs(s0, "\xFE Laufwerk A: Diskette, ");
        vio_kputs(s0, vol[0].fat16 ? "FAT16\n" : "FAT12\n");
        config_sys();
    } else {
        /* Start von Festplatte: A: erst beim ersten Zugriff einhaengen */
        if (fd_init() == 0)
            floppy_lazy = 1;
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
    blk_cache_init(nbuffers);           /* nach CONFIG.SYS: BUFFERS= */

    /* Systemumgebung: COMSPEC wie bei MS-DOS */
    scopy(line, "COMSPEC=A:\\COMMAND.EXE", sizeof(line));
    line[8] = (char)('A' + sys_drive);
    env_set(-1, line);
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

    /* Befehlsinterpreter aus PROTSHELL= (Vorgabe COMMAND.EXE /P) in der
       ersten Sitzung; er fuehrt AUTOEXEC.CMD aus. SHELL= bleibt wie bei
       OS/2 dem Befehlsinterpreter der DOS-Box (ab 6.0) vorbehalten. */
    t = task_create(protshell, s0, -1, -1);
    if (t < 0) {
        vio_kputs(s0, "\xFE PROTSHELL=");
        vio_kputs(s0, protshell);
        vio_kputs(s0, " - ");
        vio_kputs(s0, exec_err(-t));
        vio_kputs(s0, "\n");
        panic("Kein Befehlsinterpreter.");
    }
    vio_title(s0, task[t].name);
    fd_motor_off();
    vio_kputs(s0, "\n");
    vio_focus(s0);

    load_tr(TSS_K);
    schedule_next();                /* kehrt nie zurueck                 */
}
