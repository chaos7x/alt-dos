/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - eigener DOS-Kern fuer die DOS-Boxen von AT-DOS 6.3
 * (docs/DOSKERN.md, Stufe 2): Start, INT-21h-Verteiler, Speicher (MCB),
 * Kleinkram (Datum, Version, Vektoren, Land).
 *
 * Vorlage fuer die Technik: FreeDOS-Kern (memmgr.c, inthndlr.c, nur
 * gelesen); Strukturen nach MS-DOS 4.0 (ARENA.INC, SYSVAR.INC, SDA in
 * DOSSYM.INC/MS_DATA.ASM). Eigene Umsetzung.
 */
#include "cm.h"

/* ---- Daten -------------------------------------------------------------- */
u8  sda[0x80];                  /* Swappable Data Area wie MS-DOS 4.0    */
#define SDA_INDOS   1
#define SDA_LOCUS   3
#define SDA_EXTERR  4
#define SDA_ACTION  6
#define SDA_CLASS   7
#define SDA_DTA     0x0C
#define SDA_PSP     0x10
#define SDA_RET     0x14
#define SDA_DRIVE   0x16
#define SDA_BREAK   0x17

extern u8 lol_area[];           /* entry.asm DS:0024h: LoL-2 = erster MCB */
#define LOL (lol_area + 2)
u16 kseg;
struct cds cdstab[NDRV];
u16 dta_off, dta_seg;
u8  cur_drv;
u16 drives_lo, drives_hi;
u16 first_mcb;
u16 old2f[2];
u16 setver_dx;
static u8  alloc_strat, umb_link, verify, brk;
static u8  sys_drv;
extern char cm_end[];

extern char int21_entry[];
extern char int20_entry[];
extern char int22_entry[];
extern char int23_entry[];
extern char int24_entry[];
extern char int25_entry[];
extern char int26_entry[];
extern char int27_entry[];
extern char iret_entry[];
extern char retf_entry[];
extern char int29_entry[];
extern char int2f_entry[];

u8 *lol_ptr(void) { return LOL; }
u16 in_ax;
extern unsigned char cc_hit;

u16 cur_psp(void) { return *(u16 *)(sda + SDA_PSP); }

static void e0(struct rregs *r) { bint(0xE0, r); }

/* ---- Laufwerke ---------------------------------------------------------- */
int drive_ok(unsigned d)
{
    struct rregs r;
    u16 m;
    if (d >= NDRV)
        return 0;
    m = d < 16 ? drives_lo : drives_hi;
    if (m & (1u << (d & 15)))
        return 1;
    if (d == 1 && (drives_lo & 1))      /* B: = Phantom von A: wie DOS   */
        goto cds;
    r.ax = 0x1200;                      /* neu nachsehen (NET USE, LOOP) */
    e0(&r);
    drives_lo = r.ax;
    drives_hi = r.dx;
    m = d < 16 ? drives_lo : drives_hi;
    if (!(m & (1u << (d & 15))))
        return d == 1 && (drives_lo & 1);
cds:
    if (!cdstab[d].flags) {
        cdstab[d].path[0] = (char)('A' + d);
        cdstab[d].path[1] = ':';
        cdstab[d].path[2] = '\\';
        cdstab[d].path[3] = 0;
        cdstab[d].flags = 0x4000;
        cdstab[d].bsoff = 2;
    }
    return 1;
}

/* ---- Fehler ------------------------------------------------------------- */
void set_err(struct frame __far *f, unsigned e)
{
    u8 cls = 13, act = 4, loc = 1;       /* Daten / Abbruch / unbekannt   */
    switch (e) {
    case E_NOFILE: case E_NOPATH: case E_NOMORE:
        cls = 8; act = 3; loc = 2; break;           /* nicht gefunden     */
    case E_DENIED: case E_ACCESS: case 0x20: case 0x21:
        cls = 3; act = 3; loc = 2; break;           /* Berechtigung       */
    case E_NOMEM: case E_NOHANDLES:
        cls = 1; act = 4; loc = 5; break;           /* keine Mittel       */
    case E_MCB: case E_BLOCK:
        cls = 7; act = 5; loc = 5; break;
    case E_DRIVE: case E_FUNC: case E_HANDLE:
        cls = 7; act = 4; loc = 1; break;
    }
    *(u16 *)(sda + SDA_EXTERR) = e;
    sda[SDA_CLASS] = cls;
    sda[SDA_ACTION] = act;
    sda[SDA_LOCUS] = loc;
    f->ax = e;
    f->fl |= FL_CF;
}

/* ---- Speicher (MCB-Kette, ARENA.INC) ----------------------------------- */
#define MT(m)   B(m, 0)
#define MO(m)   W(m, 1)
#define MS(m)   W(m, 3)
static u16 nextm(u16 m) { return m + MS(m) + 1; }
u16 umb_sc;                             /* Systemblock vor den UMBs (9FFFh) */
/* letzter Block der ganzen Kette. Wie MS-DOS: bei getrennten UMBs ist
   der Block vor dem Systemblock 9FFFh ein 'Z', der Systemblock selbst
   bleibt 'M' (sonst zaehlt MS-DOS-MEM ihn als konventionellen Speicher) */
static int lastm(u16 m)
{
    return MT(m) == 'Z' && !(umb_sc && nextm(m) == umb_sc);
}

/* Block direkt vor dem Systemblock */
static u16 before_sc(void)
{
    u16 m = first_mcb;
    while (nextm(m) != umb_sc && !lastm(m))
        m = nextm(m);
    return m;
}

static void coalesce(void)
{
    u16 m = first_mcb, n;
    for (;;) {
        if (lastm(m))
            return;
        n = nextm(m);
        if (MT(n) != 'M' && MT(n) != 'Z')
            return;                     /* Kette kaputt                  */
        if (MO(m) == 0 && MO(n) == 0) {
            MS(m) += MS(n) + 1;
            MT(m) = MT(n);
            continue;
        }
        m = n;
    }
}

static void split(u16 m, u16 paras)     /* m auf paras kuerzen, Rest frei */
{
    u16 n;
    if (MS(m) <= paras)
        return;
    n = m + paras + 1;
    MT(n) = MT(m);
    MO(n) = 0;
    MS(n) = MS(m) - paras - 1;
    MT(m) = 'M';
    MS(m) = paras;
}

static u16 mem_alloc_from(u16 start, u16 paras, u16 owner, u16 *largest);
u16 mem_alloc(u16 paras, u16 owner, u16 *largest)
{
    coalesce();
    /* 40h nur oben, 80h oben zuerst - nur bei verbundenen UMBs (5803h) */
    if ((alloc_strat & 0xC0) && umb_sc && umb_link) {
        u8 s = alloc_strat;
        u16 r;
        alloc_strat &= 3;
        r = mem_alloc_from(umb_sc, paras, owner, largest);
        alloc_strat = s;
        if (r || (s & 0x40))
            return r;
    } else if (alloc_strat & 0x40) {
        if (largest)
            *largest = 0;
        return 0;
    }
    return mem_alloc_from(first_mcb, paras, owner, largest);
}

/* erster passender Block ab start; bei getrennten UMBs nur unten */
static u16 mem_alloc_from(u16 start, u16 paras, u16 owner, u16 *largest)
{
    u16 m, best = 0, bsz = 0xFFFF, big = 0;
    unsigned i;
    for (m = start; ; m = nextm(m)) {
        if (MO(m) == 0) {
            if (MS(m) > big)
                big = MS(m);
            if (MS(m) >= paras) {
                if ((alloc_strat & 3) == 0 && !best)
                    best = m;
                else if ((alloc_strat & 3) == 1 && MS(m) < bsz) {
                    best = m;
                    bsz = MS(m);
                } else if ((alloc_strat & 3) == 2)
                    best = m;
            }
        }
        if (lastm(m))
            break;
        if (umb_sc && !umb_link && start != umb_sc && nextm(m) == umb_sc)
            break;                      /* getrennt: nur unten          */
    }
    if (largest)
        *largest = big;
    if (!best)
        return 0;
    if ((alloc_strat & 3) == 2 && MS(best) > paras + 1) {   /* oben    */
        u16 sz = MS(best);
        split(best, sz - paras - 1);
        best = nextm(best);
    } else
        split(best, paras);
    MO(best) = owner;
    for (i = 8; i < 16; i++)
        B(best, i) = 0;
    return best + 1;
}

int mem_free(u16 seg)
{
    u16 m = seg - 1;
    if (MT(m) != 'M' && MT(m) != 'Z')
        return E_BLOCK;
    MO(m) = 0;
    return 0;
}

int mem_resize(u16 seg, u16 paras, u16 *maxp)
{
    u16 m = seg - 1, n, avail;
    if (MT(m) != 'M' && MT(m) != 'Z')
        return E_BLOCK;
    if (paras <= MS(m)) {
        split(m, paras);
        coalesce();
        return 0;
    }
    avail = MS(m);
    if (MT(m) == 'M') {
        n = nextm(m);
        coalesce();
        if (MO(n) == 0 && (MT(n) == 'M' || MT(n) == 'Z'))
            avail = MS(m) + MS(n) + 1;
    }
    if (paras > avail) {
        *maxp = avail;
        return E_NOMEM;
    }
    n = nextm(m);
    MS(m) += MS(n) + 1;
    MT(m) = MT(n);
    split(m, paras);
    return 0;
}

void mem_free_owner(u16 psp)
{
    u16 m;
    for (m = first_mcb; ; m = nextm(m)) {
        if (MO(m) == psp)
            MO(m) = 0;
        if (lastm(m) || (MT(m) != 'M' && MT(m) != 'Z'))
            break;
    }
    coalesce();
}

static void mem_fn(struct frame __far *f)
{
    u16 r, big;
    int e;
    switch (AH(f)) {
    case 0x48:
        r = mem_alloc(f->bx, cur_psp(), &big);
        if (!r) {
            set_err(f, E_NOMEM);
            f->bx = big;
        } else
            f->ax = r;
        break;
    case 0x49:
        if ((e = mem_free(f->es)) != 0)
            set_err(f, e);
        coalesce();
        break;
    case 0x4A:
        if ((e = mem_resize(f->es, f->bx, &big)) != 0) {
            set_err(f, e);
            if (e == E_NOMEM)
                f->bx = big;
        }
        break;
    case 0x58:
        switch (AL(f)) {
        case 0: f->ax = alloc_strat; break;
        case 1: alloc_strat = (u8)f->bx; break;
        case 2: AL(f) = umb_link; break;
        case 3:                         /* UMBs verbinden/trennen       */
            if ((f->bx & 1) && !umb_sc) {
                set_err(f, E_FUNC);     /* keine UMBs (wie MS-DOS)       */
                break;
            }
            umb_link = (u8)(f->bx & 1);
            if (umb_sc)
                MT(before_sc()) = umb_link ? 'M' : 'Z';
            LOL[0x63] = umb_link;       /* wie MS-DOS 5+/FreeDOS         */
            break;
        default: set_err(f, E_FUNC);
        }
        break;
    }
}

/* ---- Datum und Uhrzeit (Echtzeituhr ueber INT 1Ah) -------------------- */
static unsigned bcd(unsigned v) { return (v >> 4) * 10 + (v & 15); }
static unsigned tobcd(unsigned v) { return ((v / 10) << 4) | (v % 10); }

void datetime_fn(struct frame __far *f)
{
    struct rregs r;
    unsigned y, mo, d, a, yy, mm;
    switch (AH(f)) {
    case 0x2A:
        r.ax = 0x0400;
        bint(0x1A, &r);
        y = bcd(r.cx >> 8) * 100 + bcd(r.cx & 0xFF);
        mo = bcd(r.dx >> 8);
        d = bcd(r.dx & 0xFF);
        f->cx = y;
        DH(f) = (u8)mo;
        DL(f) = (u8)d;
        a = (14 - mo) / 12;             /* Wochentag (Zeller)            */
        yy = y - a;
        mm = mo + 12 * a - 2;
        AL(f) = (u8)((d + yy + yy / 4 - yy / 100 + yy / 400 + (31 * mm) / 12) % 7);
        break;
    case 0x2B:
        r.ax = 0x0500;
        r.cx = (tobcd(f->cx / 100) << 8) | tobcd(f->cx % 100);
        r.dx = (tobcd(DH(f)) << 8) | tobcd(DL(f));
        bint(0x1A, &r);
        AL(f) = 0;
        break;
    case 0x2C:
        r.ax = 0x0200;
        bint(0x1A, &r);
        CH(f) = (u8)bcd(r.cx >> 8);
        CL(f) = (u8)bcd(r.cx & 0xFF);
        DH(f) = (u8)bcd(r.dx >> 8);
        r.ax = 0;                       /* Hundertstel aus dem Takt      */
        bint(0x1A, &r);
        DL(f) = (u8)(((r.dx % 18) * 100) / 18);
        break;
    case 0x2D:
        r.ax = 0x0300;
        r.cx = (tobcd(CH(f)) << 8) | tobcd(CL(f));
        r.dx = tobcd(DH(f)) << 8;
        bint(0x1A, &r);
        AL(f) = 0;
        break;
    }
}

/* ---- Laenderinfo (38h), fest Deutschland bis Stufe 3 ------------------ */
static void country_fn(struct frame __far *f)
{
    static const u8 de[34] = {
        1, 0,                       /* Datum TT.MM.JJ                       */
        'D', 'M', 0, 0, 0,          /* Waehrung                             */
        '.', 0, ',', 0, '.', 0, ':', 0,
        3, 2, 1,                    /* Waehrung hinten mit Leerz., 2 Stellen, 24 h */
        0, 0, 0, 0,                 /* Umsetzung (Fernaufruf)               */
        ';', 0
    };
    unsigned i;
    if (AL(f) != 0 && AL(f) != 49 && !(AL(f) == 0xFF && f->bx == 49)) {
        if (f->dx == 0xFFFF) {      /* setzen: hinnehmen                  */
            f->fl &= ~FL_CF;
            return;
        }
    }
    if (f->dx == 0xFFFF)
        return;
    for (i = 0; i < 34; i++)
        B(f->ds, f->dx + i) = de[i];
    W(f->ds, f->dx + 18) = (u16)retf_entry; /* Fernaufruf: RETF im Kern */
    W(f->ds, f->dx + 20) = kseg;
    f->bx = 49;
    f->ax = 49;
}

/* ---- Verteiler ---------------------------------------------------------- */
struct frame __far *int21_c(struct frame __far *f)
{
    u8 ah = AH(f);
    in_ax = f->ax;
    if (sda[SDA_INDOS] == 1)            /* fuer INT 2Fh 1218h            */
        user_frame = f;
    f->fl &= ~FL_CF;
    switch (ah) {
    case 0x00:
        return term(f, 0, 0, 0);
    case 0x4C:
        return term(f, 0, AL(f), 0);
    case 0x31:
        return term(f, 3, AL(f), f->dx);
    case 0x4B:
        return exec_fn(f);
    case 0x4D:
        f->ax = ret_code;
        ret_code = 0;
        break;
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
        con_fn(f);
        break;
    case 0x0D:                          /* Platten zuruecksetzen         */
    case 0x68: case 0x6A:               /* Datei festschreiben           */
        break;
    case 0x0E:
        if (drive_ok(DL(f)))
            cur_drv = DL(f);
        sda[SDA_DRIVE] = cur_drv;
        AL(f) = NDRV;
        break;
    case 0x19:
        AL(f) = cur_drv;
        break;
    case 0x1A:
        dta_off = f->dx;
        dta_seg = f->ds;
        *(u16 *)(sda + SDA_DTA) = dta_off;
        *(u16 *)(sda + SDA_DTA + 2) = dta_seg;
        break;
    case 0x2F:
        f->bx = dta_off;
        f->es = dta_seg;
        break;
    case 0x25:
        W(0, AL(f) * 4) = f->dx;
        W(0, AL(f) * 4 + 2) = f->ds;
        break;
    case 0x35:
        f->bx = W(0, AL(f) * 4);
        f->es = W(0, AL(f) * 4 + 2);
        break;
    case 0x2A: case 0x2B: case 0x2C: case 0x2D:
        datetime_fn(f);
        break;
    case 0x2E:
        verify = AL(f) & 1;
        break;
    case 0x54:
        AL(f) = verify;
        break;
    case 0x30: {
        u16 v = cur_psp() ? W(cur_psp(), 0x40) : 0;     /* INIT: kein PSP */
        if (!v)
            v = 0x1606;
        f->ax = v;
        f->bx = AL(f) == 1 ? 0 : 0xFF00;    /* OEM FFh wie MS-DOS        */
        f->cx = 0;
        break;
    }
    case 0x33:
        switch (AL(f)) {
        case 0: DL(f) = brk; break;
        case 1: brk = DL(f) & 1; break;
        case 2: { u8 o = brk; brk = DL(f) & 1; DL(f) = o; break; }
        case 5: DL(f) = (u8)(sys_drv + 1); break;
        case 6: f->bx = 0x1606; f->dx = in_hma ? 0x1000 : 0; break; /* DH 10h: HMA */
        default: AL(f) = 0xFF;
        }
        break;
    case 0x34:
        f->es = kseg;
        f->bx = (u16)(sda + SDA_INDOS);
        break;
    case 0x37:
        if (AL(f) == 0)
            DL(f) = '/';
        AL(f) = 0;
        break;
    case 0x38:
        country_fn(f);
        break;
    case 0x48: case 0x49: case 0x4A: case 0x58:
        mem_fn(f);
        break;
    case 0x50:
        *(u16 *)(sda + SDA_PSP) = f->bx;
        break;
    case 0x51: case 0x62:
        f->bx = cur_psp();
        break;
    case 0x52:
        f->es = kseg;
        f->bx = (u16)LOL;
        break;
    case 0x59:
        f->ax = *(u16 *)(sda + SDA_EXTERR);
        BH(f) = sda[SDA_CLASS];
        BL(f) = sda[SDA_ACTION];
        CH(f) = sda[SDA_LOCUS];
        break;
    case 0x5D:
        if (AL(f) == 6) {               /* SDA                           */
            f->ds = kseg;
            f->si = (u16)sda;
            f->cx = sizeof(sda);
            f->dx = 0x18;
        } else if (AL(f) == 0x0A) {     /* erweiterten Fehler setzen     */
            *(u16 *)(sda + SDA_EXTERR) = W(f->ds, f->dx);
        } else
            set_err(f, E_FUNC);
        break;
    case 0x5E: case 0x5F:               /* Netz (dosint.c)               */
        net_fn(f);
        break;
    case 0x63:                          /* DBCS: keine                   */
        f->ds = kseg;
        f->si = (u16)(lol_area + 0x7E);
        AL(f) = 0;
        break;
    case 0x66:
        if (AL(f) == 1) {
            f->bx = 437;
            f->dx = 437;
        } else if (AL(f) != 2)
            set_err(f, E_FUNC);
        break;
    case 0x65:
        if (AL(f) == 0x01) {            /* erweiterte Laenderinfo        */
            u16 n = f->cx < 41 ? f->cx : 41, i;
            static u8 ci[41];
            struct frame tf;
            ci[0] = 1;
            *(u16 *)(ci + 1) = 38;
            *(u16 *)(ci + 3) = 49;
            *(u16 *)(ci + 5) = 437;
            tf.ds = kseg;
            tf.dx = (u16)(ci + 7);
            tf.ax = 0x3800;
            tf.bx = 0;
            country_fn((struct frame __far *)&tf);
            for (i = 0; i < n; i++)
                B(f->es, f->di + i) = ci[i];
            f->cx = n;
            f->ax = 49;
        } else if (AL(f) == 0x20) {            /* Zeichen DL gross              */
            if (DL(f) >= 'a' && DL(f) <= 'z')
                DL(f) -= 32;
        } else if (AL(f) == 0x21 || AL(f) == 0x22) {
            u16 i;
            for (i = 0; AL(f) == 0x21 ? i < f->cx : B(f->ds, f->dx + i); i++)
                if (B(f->ds, f->dx + i) >= 'a' && B(f->ds, f->dx + i) <= 'z')
                    B(f->ds, f->dx + i) -= 32;
        } else
            set_err(f, E_FUNC);
        break;
    default:
        file_fn(f);
        break;
    }
    if (crit_abort) {                   /* INT 24h: Abbrechen            */
        crit_abort = 0;
        cc_hit = 0;
        return term(f, 2, 0, 0);
    }
    if (cc_hit) {
        cc_hit = 0;
        return cc_frame(f);
    }
    return f;
}

/* ---- UMBs (DOS=UMB): beim XMS-Treiber holen, hinter 9FFFh anhaengen ---- */
u16 xms_ent[2];
u16 xmscall(u16 ax, u16 dx, u16 *bx, u16 *dxo);

static void mcb_set(u16 m, u8 t, u16 owner, u16 size, const char *nm)
{
    unsigned i;
    MT(m) = t;
    MO(m) = owner;
    MS(m) = size;
    for (i = 0; i < 8; i++) {           /* nach der Null nicht weiterlesen */
        B(m, 8 + i) = nm && *nm ? (u8)*nm : 0;
        if (nm && *nm)
            nm++;
    }
}

static void umb_init(u16 top)
{
    struct rregs r;
    u16 seg[8], sz[8], n = 0, bx, dx, i, end;
    r.ax = 0x4300;
    bint(0x2F, &r);
    if ((r.ax & 0xFF) != 0x80 || top != 0xA000)
        return;
    r.ax = 0x4310;
    bint(0x2F, &r);
    xms_ent[0] = r.bx;
    xms_ent[1] = r.es;
    while (n < 8) {
        if (xmscall(0x1000, 0xFFFF, &bx, &dx) || dx < 2)
            break;                      /* (Erfolg mit FFFFh: unmoeglich) */
        if (!xmscall(0x1000, dx, &bx, &dx) || bx <= top)
            break;
        for (i = n; i && seg[i - 1] > bx; i--) {    /* nach Lage sortiert */
            seg[i] = seg[i - 1];
            sz[i] = sz[i - 1];
        }
        seg[i] = bx;
        sz[i] = dx;
        n++;
    }
    if (!n)
        return;
    /* unten: freier Block bis 9FFEh, dann Systemblock "SC" bis zur UMB */
    MT(first_mcb) = 'Z';                /* getrennt wie beim Start       */
    MS(first_mcb) = top - 1 - first_mcb - 1;
    umb_sc = top - 1;
    mcb_set(umb_sc, 'M', 8, seg[0] - umb_sc - 1, "SC");
    for (i = 0; i < n; i++) {
        end = seg[i] + sz[i];
        if (i + 1 < n && seg[i + 1] > end) {    /* Luecke: Systemblock */
            mcb_set(seg[i], 'M', 0, sz[i] - 2, 0);
            mcb_set(end - 1, 'M', 8, seg[i + 1] - end, "SC");
        } else
            mcb_set(seg[i], i + 1 < n ? 'M' : 'Z', 0, sz[i] - 1, 0);
    }
    *(u16 *)(LOL + 0x66) = umb_sc;     /* erster UMB-Block (9FFFh)      */
}

/* Programmblock fuer EXEC: mindestens need, gern want Absaetze. Bei
   Strategie "oben zuerst" (LOADHIGH) wie MS-DOS der groesste obere Block,
   wenn das Programm hineinpasst. *big = groesster Block (Fehlermeldung) */
u16 mem_alloc_prog(u16 need, u16 want, u16 owner, u16 *big)
{
    u16 b = 0, r;
    u8 s = alloc_strat;
    coalesce();
    if ((alloc_strat & 0xC0) && umb_sc && umb_link) {
        alloc_strat &= 3;
        mem_alloc_from(umb_sc, 0xFFFF, 0, &b);
        r = b >= need ? mem_alloc_from(umb_sc, want < b ? want : b, owner, 0) : 0;
        alloc_strat = s;
        if (r || (s & 0x40)) {
            *big = b;
            return r;
        }
    }
    alloc_strat &= 3;
    mem_alloc_from(first_mcb, 0xFFFF, 0, &b);
    r = b >= need ? mem_alloc_from(first_mcb, want < b ? want : b, owner, 0) : 0;
    alloc_strat = s;
    *big = b;
    return r;
}

/* ---- fuer config.c (Treiber der Box-CONFIG.SYS) ------------------------ */
/* groesster Block fuer einen Treiber, mindestens need Absaetze, Besitzer
   8; high: erst oben (nur UMBs, Strategie 40h), passt es dort nicht,
   unten wie DEVICEHIGH bei MS-DOS */
u16 dev_alloc(int high, u16 need, u16 *size)
{
    u8 s = alloc_strat, l = umb_link;
    u16 big = 0, r = 0;
    if (high && umb_sc) {
        alloc_strat = 0x40;
        umb_link = 1;
        MT(before_sc()) = 'M';
        mem_alloc(0xFFFF, 0, &big);
        if (big >= need)
            r = mem_alloc(big, 8, 0);
        umb_link = l;
        MT(before_sc()) = l ? 'M' : 'Z';
        alloc_strat = s;
    }
    if (!r) {
        mem_alloc(0xFFFF, 0, &big);
        if (big >= need)
            r = mem_alloc(big, 8, 0);
    }
    *size = big;
    return r;
}

void dev_name(u16 seg, const char *path)
{
    const char *b = path;
    unsigned i;
    MO(seg - 1) = seg;                  /* eigener Besitzer: MEM zeigt ihn */
    for (; *path; path++)
        if (*path == '\\' || *path == ':')
            b = path + 1;
    for (i = 0; i < 8; i++) {
        B(seg - 1, 8 + i) = *b && *b != '.' ? (u8)*b : 0;
        if (*b && *b != '.')
            b++;
    }
}

/* Treiberkoepfe first .. seg:last hinter NUL (LoL+22h und devs) */
void dev_link(u32 first, u16 seg, u16 last_off)
{
    W(seg, last_off) = W(kseg, (u16)(LOL + 0x22));
    W(seg, last_off + 2) = W(kseg, (u16)(LOL + 0x24));
    *(u32 *)(LOL + 0x22) = first;
    devs[DV_NUL].next = first;
}

/* Block vom oberen Ende (unten, Strategie 2) - fuer Vorruebergehendes */
u16 mem_alloc_top(u16 paras)
{
    u8 s = alloc_strat;
    u16 r;
    alloc_strat = 2;
    r = mem_alloc(paras, 8, 0);
    alloc_strat = s;
    return r;
}

void buffers_set(unsigned n)
{
    *(u16 *)(LOL + 0x3F) = n;
}

u16 brk_set(unsigned on)
{
    brk = (u8)on;
    return 0;
}

/* ---- Start -------------------------------------------------------------- */
static void setvec(unsigned n, char *h)  /* Eingang in LTEXT */
{
    W(0, n * 4) = (u16)h;
    W(0, n * 4 + 2) = kseg;
}

void cm_main(void)
{
    struct rregs r;
    u16 top, m;
    unsigned d, i;
    static char shell[32];

    kseg = getds();
    /* LoL: erster MCB (LoL-2), SFT (04h), CDS (16h), Laufwerke (21h),
       NUL-Geraet (22h) */
    first_mcb = kseg + (((u16)cm_end + 15) >> 4);
    *(u16 *)(LOL + 0x68) = first_mcb;   /* letzte Suchstelle (MS-DOS)    */
    *(u16 *)(lol_area) = first_mcb;
    file_init();
    *(u16 *)(LOL + 0x04) = sft_head();      /* Kopf (next, count) davor */
    *(u16 *)(LOL + 0x06) = kseg;
    *(u16 *)(LOL + 0x0C) = (u16)&devs[DV_CON];
    *(u16 *)(LOL + 0x0E) = kseg;
    *(u16 *)(LOL + 0x08) = (u16)&devs[DV_CLOCK];
    *(u16 *)(LOL + 0x0A) = kseg;
    *(u16 *)(LOL + 0x10) = 512;
    *(u16 *)(LOL + 0x16) = (u16)cdstab;
    *(u16 *)(LOL + 0x18) = kseg;
    LOL[0x20] = 0;
    LOL[0x63] = 0;                      /* UMBs getrennt                 */
    *(u16 *)(LOL + 0x66) = 0xFFFF;      /* keine UMB-Kette              */
    LOL[0x21] = NDRV;
    {
        unsigned i;
        u8 *p = (u8 *)&devs[DV_NUL];
        for (i = 0; i < 18; i++)
            LOL[0x22 + i] = p[i];
    }

    /* Vektoren */
    old2f[0] = W(0, 0x2F * 4);
    old2f[1] = W(0, 0x2F * 4 + 2);
    setvec(0x20, int20_entry);
    setvec(0x21, int21_entry);
    setvec(0x22, int22_entry);
    setvec(0x23, int23_entry);
    setvec(0x24, int24_entry);
    setvec(0x25, int25_entry);
    setvec(0x26, int26_entry);
    setvec(0x27, int27_entry);
    setvec(0x28, iret_entry);
    setvec(0x29, int29_entry);
    setvec(0x2F, int2f_entry);
    /* Nutzervektoren 60h-66h wie beim BIOS auf 0000:0000 (die Box legt
       jeden Vektor auf einen eigenen Stummel). DOS/4GW sucht in der IVT
       zwei gleiche benachbarte Vektoren als freie Plaetze und kreist
       sonst ewig (DOOM blieb als erstes Programm der Box stehen). */
    for (i = 0x60 * 4; i < 0x67 * 4; i++)
        B(0, i) = 0;

    /* beim AT-DOS-Kernel melden: SDA (PSP fuer DPMI, DEM bereit), eigener Kern */
    r.ax = 0x1100;
    r.bx = 0xFF;
    r.ds = kseg;
    r.si = (u16)sda;
    e0(&r);
    r.ax = 0x3101;
    e0(&r);
    r.ax = 0x1200;
    e0(&r);
    drives_lo = r.ax;
    drives_hi = r.dx;
    r.ax = 0x1300;
    e0(&r);
    sys_drv = (u8)r.ax;
    for (d = 0; d < NDRV; d++)
        drive_ok(d);
    cur_drv = sys_drv;
    sda[SDA_DRIVE] = cur_drv;
    dpb_init();                         /* DPB-Liste (LoL+0, MEM /D)    */
    *(u16 *)(LOL + 0x00) = dpb_first();
    *(u16 *)(LOL + 0x02) = dpb_first() == 0xFFFF ? 0xFFFF : kseg;
    LOL[0x20] = NDRV;                   /* Blockgeraete                  */
    *(u16 *)(LOL + 0x3F) = 15;          /* BUFFERS wie MS-DOS (Puffer hat
                                           der Kernel; Installer pruefen) */
    LOL[0x43] = (u8)(sys_drv + 1);

    /* Speicher: ein freier Block bis zum Ende des konventionellen */
    top = W(0x40, 0x13) * 64;
    m = first_mcb;
    MT(m) = 'Z';
    MO(m) = 0;
    MS(m) = top - m - 1;
    umb_init(top);
    config_box(sys_drv);                /* \DOS\MDOS\CONFIG.SYS (config.c) */

    shell[0] = (char)('A' + sys_drv);
    {
        const char *s = ":\\DOS\\MDOS\\BOXSHELL.COM";
        unsigned i;
        for (i = 0; s[i]; i++)
            shell[1 + i] = s[i];
        shell[1 + i] = 0;
    }
    m = load_shell(shell);
    if (!m) {                           /* ohne Schale: Box beenden     */
        r.ax = 0x01FF;
        e0(&r);
        for (;;)
            ;
    }
}
