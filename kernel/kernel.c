/*
 * DOS 5.0 - Kernel, Meilenstein 4
 *
 * 80286 Protected Mode, segmentiert, kooperatives Multitasking.
 * Anwendungen laufen in Ring 3, jede mit eigenem TSS, eigener LDT und
 * ohne IOPL. Hardware und Speicher erreichen sie ausschliesslich ueber
 * die Family API (FAPI) per Call Gate. Programme laufen in Sitzungen mit
 * eigenem Bildschirm; die erste Sitzung startet, was STARTUP.CMD nennt.
 */
#include "kern.h"
#include "../include/fapi_abi.h"
#include "../include/fx.h"
#include "mem.h"
#include "ifs.h"

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
    char          name[13];
    char          args[FAPI_ARGLEN];
    unsigned      img_unit, img_cnt;
};

static struct task    task[NTASK];
static struct tss286  tss_k;
static struct tss286  tss[NTASK];
static struct desc    ldt[NTASK][NLDT];
static unsigned short kstack[NTASK][256];         /* Ring-0-Stack je Task  */
static unsigned       alloc_units;                /* per DosAllocSeg       */
static unsigned       image_units;                /* Programmabbilder      */
static int            current = -1;

#define NFILE 8
static struct {
    signed char     owner;  /* Task oder -1                              */
    struct ifs_file f;
    unsigned long   pos;
} files[NFILE];

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

/* Programm im FX-Format in Task t laden. 0 = ok, sonst EXEC_ERR_* */
static int load_program(int t, const char *name)
{
    struct ifs_file f;
    struct fx_header h;
    struct tss286 *ts;
    unsigned short *w;
    unsigned long base, seg;
    unsigned tu, tn, u, n, fx;
    int err = 0;

    if (ifs_open(name, &f))
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
    if (rd16(SCRATCH, 0) != 0x5A4D || fx > (unsigned)f.size - sizeof(h)) {
        mem_free(tu, tn);
        return EXEC_ERR_FORMAT;
    }
    far_copy(KDATA, (unsigned)&h, SCRATCH, fx, sizeof(h));
    seg = (unsigned long)h.image_size + h.bss_size + h.stack_size;
    if (h.sig[0] != FX_SIG0 || h.sig[1] != FX_SIG1 || h.version != FX_VERSION ||
        (unsigned long)h.image_off + h.image_size > f.size ||
        seg > 0x10000UL || h.entry >= h.image_size)
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
    far_copy(SCRATCH2, 0, SCRATCH, h.image_off, h.image_size);
    mem_free(tu, tn);

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
    ts->sp0   = (unsigned short)&kstack[t][256];
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

/* Befehlszeile "PROG ARGS" -> Programmname mit .EXE und Argumente */
static void split_cmd(const char *cmd, char *name, char *args)
{
    int i = 0, dot = 0;
    char c;

    while (*cmd == ' ')
        cmd++;
    while (*cmd && *cmd != ' ' && i < 8 + 1 + 3) {
        c = *cmd++;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c == '.')
            dot = 1;
        name[i++] = c;
    }
    if (!dot && i <= 8) {
        name[i++] = '.'; name[i++] = 'E'; name[i++] = 'X'; name[i++] = 'E';
    }
    name[i] = 0;
    while (*cmd && *cmd != ' ')
        cmd++;
    while (*cmd == ' ')
        cmd++;
    scopy(args, cmd, FAPI_ARGLEN);
}

/* neuen Task anlegen und laden; -> Tasknummer oder -(EXEC_ERR_*) */
static int task_create(const char *cmd, int session, int parent)
{
    int t, err;

    for (t = 0; t < NTASK; t++)
        if (task[t].state == T_FREE)
            break;
    if (t == NTASK)
        return -EXEC_ERR_NOSLOT;
    split_cmd(cmd, task[t].name, task[t].args);
    err = load_program(t, task[t].name);
    if (err)
        return -err;
    task[t].session = session;
    task[t].parent  = parent;
    task[t].wake    = 0;
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
    for (i = 0; i < NFILE; i++)
        if (files[i].owner == t)
            files[i].owner = -1;
    for (i = 0; i < NTASK; i++)
        if (task[i].state != T_FREE && task[i].parent == t)
            task[i].parent = -1;
    task[t].state = T_FREE;

    if (p >= 0 && task[p].state == T_WAIT)
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
   der ersten Sitzung (die Shell aus STARTUP.CMD) ist geschuetzt. */
static void do_break(void)
{
    int s = vio_fg(), t;
    for (t = 0; t < NTASK; t++)
        if (task[t].state == T_READY && task[t].session == s)
            break;
    if (t == NTASK || (task[t].parent < 0 && s == 0))
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

/* ---- FAPI-Verteiler ---------------------------------------------------- */
unsigned fapi_dispatch(unsigned fn, unsigned usel, unsigned a1, unsigned a2)
{
    int row, col, s = task[current].session, t, k, i;
    unsigned a3 = fapi_dx;
    char buf[FAPI_ARGLEN];
    struct ifs_dirent de;

    usel |= 3;                                  /* Aufrufer ist Ring 3  */
    switch (fn) {
    case FAPI_GETVERSION:
        return FAPI_VERSION;

    case FAPI_VIOWRTSTR:
        if (!sel_readable(usel))
            return FAPI_ERROR;
        row = a2 >> 8;
        col = a2 & 0xFF;
        if (row >= SROWS || col >= SCOLS)
            return FAPI_ERROR;
        vio_str(s, row, col, usel, a1, sel_limit(usel), 0x07);
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
        if (!sel_readable(usel))
            return FAPI_ERROR;
        for (i = 0; i < FAPI_ARGLEN - 1; i++) {
            if (!user_ok(usel, a1 + i, 1, 0))
                break;
            buf[i] = *(char __far *)MK_FP(usel, a1 + i);
            if (!buf[i])
                break;
        }
        buf[i] = 0;
        if (a2 == EXEC_ASYNC) {
            int ns = vio_open("");
            if (ns < 0)
                return EXEC_ERR_NOMEM;
            t = task_create(buf, ns, -1);
            if (t < 0) {
                vio_close(ns);
                return -t;
            }
            vio_title(ns, task[t].name);
            vio_focus(ns);
            return 0;
        }
        t = task_create(buf, s, current);
        if (t < 0)
            return -t;
        task[current].state = T_WAIT;           /* bis das Kind endet   */
        schedule_next();
        return 0;

    case FAPI_GETARGS:
        if (!user_ok(usel, a1, FAPI_ARGLEN, 1))
            return FAPI_ERROR;
        far_copy(usel, a1, KDATA, (unsigned)task[current].args, FAPI_ARGLEN);
        return 0;

    case FAPI_OPEN:
        for (i = 0; i < 12; i++) {
            if (!user_ok(usel, a1 + i, 1, 0))
                return FAPI_ERROR;
            buf[i] = *(char __far *)MK_FP(usel, a1 + i);
            if (buf[i] >= 'a' && buf[i] <= 'z')
                buf[i] -= 32;
            if (!buf[i])
                break;
        }
        buf[i] = 0;
        for (k = 0; k < NFILE; k++)
            if (files[k].owner < 0)
                break;
        if (k == NFILE || ifs_open(buf, &files[k].f))
            return FAPI_ERROR;
        files[k].owner = (signed char)current;
        files[k].pos = 0;
        return k;

    case FAPI_READ:
        if (a1 >= NFILE || files[a1].owner != current)
            return FAPI_ERROR;
        if (a3 > 0x7FFF)
            a3 = 0x7FFF;
        if (!user_ok(usel, a2, a3, 1))
            return FAPI_ERROR;
        k = ifs_read(&files[a1].f, files[a1].pos, usel, a2, a3);
        if (k < 0)
            return FAPI_ERROR;
        files[a1].pos += k;
        return k;

    case FAPI_CLOSE:
        if (a1 >= NFILE || files[a1].owner != current)
            return FAPI_ERROR;
        files[a1].owner = -1;
        return 0;

    case FAPI_FINDENTRY:
        if (!user_ok(usel, a1, sizeof(de), 1))
            return FAPI_ERROR;
        if (ifs_readdir(a2, &de))
            return FAPI_ERROR;
        far_copy(usel, a1, KDATA, (unsigned)&de, sizeof(de));
        return 0;

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

/* ---- Start ------------------------------------------------------------- */
static char startup[512];

/* naechste Zeile aus STARTUP.CMD: REM und Leerzeilen zaehlen nicht */
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

void kmain(void)
{
    struct ifs_file f;
    char line[FAPI_ARGLEN], num[6];
    unsigned p, size;
    int s0, s, t, i, n = 0;

    for (i = 0; i < NFILE; i++)
        files[i].owner = -1;

    pic_pit_init();

    /* 286-Deskriptoren haben 24 Bit Basis: mehr als 16 MB gibt es fuer
       DOS 5.0 nicht, auch wenn ein 486 mit 128 MB darunter steckt */
    if (mem_conv_kb > 640)
        mem_conv_kb = 640;
    if (mem_ext_kb > 15360 || !a20_ok)
        mem_ext_kb = a20_ok ? 15360 : 0;
    mem_init(mem_conv_kb, mem_ext_kb);

    s0 = vio_open("SYSTEM");
    if (s0 < 0)
        panic("Kein Speicher fuer die erste Sitzung.");
    vio_focus(s0);
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0D);       /* Cursor: Unterstrich   */
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0E);

    vio_kputs(s0, "DOS 5.0 (alternative Zeitlinie) - 80286 Protected Mode\n");
    vio_kputs(s0, a20_ok ? "\xFE A20 frei, " : "\xFE A20 GESPERRT, nur unter 1 MB, ");
    udec(mem_free_kb(), num);
    vio_kputs(s0, num);
    vio_kputs(s0, " KB Arbeitsspeicher frei\n");

    enable_ints();                  /* Diskette und Tastatur per IRQ     */
    kbd_init();
    if (fd_init())
        panic("Diskettencontroller antwortet nicht.");
    if (ifs_mount(&fat12_driver))
        panic("Diskette A: hat kein lesbares FAT12-Dateisystem.");
    vio_kputs(s0, "\xFE Diskette A: IFS-Treiber ");
    vio_kputs(s0, ifs_name());
    vio_kputs(s0, " eingeh\x84ngt\n");

    if (ifs_open("STARTUP.CMD", &f) || f.size > sizeof(startup))
        panic("STARTUP.CMD fehlt oder ist zu gross.");
    size = (unsigned)f.size;
    if (ifs_read(&f, 0, KDATA, (unsigned)startup, size) != (int)size)
        panic("STARTUP.CMD ist unlesbar.");

    /* erstes Programm in Sitzung 1, jedes weitere in einer eigenen */
    p = 0;
    while (next_line(&p, size, line)) {
        s = n == 0 ? s0 : vio_open("");
        if (s < 0)
            break;
        t = task_create(line, s, -1);
        if (t < 0) {
            vio_kputs(s0, "\xFE STARTUP.CMD: ");
            vio_kputs(s0, line);
            vio_kputs(s0, " - ");
            vio_kputs(s0, exec_err(-t));
            vio_kputs(s0, "\n");
            if (s != s0)
                vio_close(s);
            continue;
        }
        vio_title(s, task[t].name);
        n++;
    }
    fd_motor_off();
    vio_kputs(s0, "\n");
    vio_focus(s0);

    load_tr(TSS_K);
    schedule_next();                /* kehrt nie zurueck                 */
}
