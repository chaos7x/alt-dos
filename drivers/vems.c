/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VEMS.SYS - EMS 4.0 in der DOS-Box (6.3, Stufe 3 nach docs/VDD.md; wie
 * VEMM.SYS bei OS/2 2.x, vorher in kernel/v86.c)
 *
 * INT 67h der Box kommt ueber den Kernel (Vektor F060:0020 "ARPL 67h;
 * IRET", Geraet EMMXXXX0 = ATBOX.SYS). Der Seitenrahmen (4 x 16 KB) bleibt
 * Sache des Kernels - er waehlt ihn einmal fuer alle Boxen vor den UMBs
 * (VI_EMSFRAME, 0 = keiner: EMS_FRAME_LOCATION=NONE bzw. DOSBOX.INI).
 * Eine logische Seite = 4 zusammenhaengende Kernelseiten (VS_PGALLOC),
 * eingeblendet ueber die Seitentabelle der Box (VS_MAP), 57h kopiert ueber
 * VS_COPY. Verwaltung im DS des Moduls: ein Vorrat von NPOOL logischen
 * Seiten fuer alle Boxen (32 MB), je Box bis 32 Handles.
 * Ohne dieses Modul meldet INT 67h 80h (kein EMS).
 */
#include "drvlib.h"
#include "netlib.h"                     /* fent()                        */
#include "../include/vdd.h"

#define NT       VDD_NTASK
#define NEH      32                     /* Handles je Box                */
#define NPOOL    2048                   /* logische Seiten aller Boxen   */
#define EMS_MAXP 2048                   /* je Box hoechstens (32 MB)     */
#define EMS_KB   2048                   /* Vorgabe (DOSBOX.INI: EMS_MEMORY_LIMIT) */
#define NEMSC    3                      /* 56h-Aufrufe ineinander        */
#define EMS_RET  0x0560                 /* F000:0560 "ARPL E6h; RETF"    */
#define VBIOS    0xF000

static struct epg {
    unsigned long base;                 /* erste Kernelseite (4 am Stueck) */
    unsigned idx;                       /* logische Seite im Handle      */
    unsigned char box, h;               /* box FFh = frei                */
} pool[NPOOL];
static unsigned char on[NT][NEH];
static unsigned np[NT][NEH], fs[NT][NEH];      /* Seiten, Platz von Seite 0 */
static unsigned char mh[NT][4];                 /* Rahmen: Handle + 1    */
static unsigned ml[NT][4];                      /*          logische Seite */
static unsigned char name[NT][NEH][8];
static struct { unsigned char h; unsigned lg; } sav[NT][NEH][4];  /* 47h/48h */
static unsigned char cn[NT];
static struct { unsigned seg, off; unsigned char n, mode, h; } cst[NT][NEMSC];
static unsigned fr[NT];                 /* erste Rahmenseite je Box      */

static struct vdd_svc sv;
static struct vdd_reg reg;

static unsigned svc(unsigned fn, unsigned t, unsigned a1, unsigned a2)
{
    sv.fn = fn;
    sv.task = t;
    sv.a1 = a1;
    sv.a2 = a2;
    return devhlp(DH_VDDSVC, 0, (unsigned)&sv, 0);
}

/* ---- Speicher der Box (Real Mode: seg:off) ------------------------------- */
static unsigned char mb[20];
static void box_io(unsigned t, unsigned seg, unsigned off, unsigned n, int rd)
{
    sv.lin = ((unsigned long)seg << 4) + off;
    sv.buf = (unsigned)mb;
    sv.len = n;
    svc(rd ? VS_RD : VS_WR, t, 0, 0);
}
static unsigned rd16(unsigned t, unsigned seg, unsigned off)
{
    box_io(t, seg, off, 2, 1);
    return mb[0] | (mb[1] << 8);
}
static unsigned char rd8(unsigned t, unsigned seg, unsigned off)
{
    box_io(t, seg, off, 1, 1);
    return mb[0];
}
static void wr16(unsigned t, unsigned seg, unsigned off, unsigned v)
{
    mb[0] = (unsigned char)v;
    mb[1] = (unsigned char)(v >> 8);
    box_io(t, seg, off, 2, 0);
}
static void wr8(unsigned t, unsigned seg, unsigned off, unsigned v)
{
    mb[0] = (unsigned char)v;
    box_io(t, seg, off, 1, 0);
}

/* ---- Verwaltung ------------------------------------------------------------ */
static unsigned pgn(unsigned t)                 /* Grenze in Seiten      */
{
    unsigned l = svc(VS_INFO, t, VI_EMSLIM, 0);
    return l ? l - 1 : EMS_KB / 16;
}

/* Platz s in Rahmenplatz pp, s < 0: leer (Ersatzseite) */
static void show(unsigned t, unsigned pp, int s)
{
    sv.lin = s >= 0 ? pool[s].base : 0;
    svc(VS_MAP, t, fr[t] + pp * 4, 4);
}

static int slot(unsigned t, unsigned h, unsigned k)  /* k-te Seite von h  */
{
    unsigned s = fs[t][h] + k;
    if (s < NPOOL && pool[s].box == t && pool[s].h == h && pool[s].idx == k)
        return (int)s;                  /* meist am Stueck angelegt       */
    for (s = 0; s < NPOOL; s++)
        if (pool[s].box == t && pool[s].h == h && pool[s].idx == k)
            return (int)s;
    return -1;
}

static unsigned used(unsigned t)
{
    unsigned h, u = 0;
    for (h = 0; h < NEH; h++)
        u += np[t][h];
    return u;
}

static unsigned efree(unsigned t)
{
    unsigned u = used(t), n = pgn(t), sys = svc(VS_INFO, t, VI_FREE16, 0);
    n = u >= n ? 0 : n - u;
    return sys < n ? sys : n;
}

static int grow(unsigned t, unsigned h, unsigned n)   /* n Seiten anhaengen */
{
    unsigned s;
    while (n) {
        for (s = 0; s < NPOOL && pool[s].box != 0xFF; s++)
            ;
        if (s == NPOOL || svc(VS_PGALLOC, t, 4, 0))
            return 0;
        pool[s].base = sv.lin;
        pool[s].box = (unsigned char)t;
        pool[s].h = (unsigned char)h;
        pool[s].idx = np[t][h];
        if (!np[t][h])
            fs[t][h] = s;
        np[t][h]++;
        n--;
    }
    return 1;
}

static void shrink(unsigned t, unsigned h, unsigned n) /* auf n Seiten     */
{
    unsigned i;
    int s;
    for (i = 0; i < 4; i++)
        if (mh[t][i] == h + 1 && ml[t][i] >= n) {
            if (fr[t])
                show(t, i, -1);
            mh[t][i] = 0;
        }
    while (np[t][h] > n) {
        s = slot(t, h, np[t][h] - 1);
        if (s >= 0) {
            sv.lin = pool[s].base;
            svc(VS_PGFREE, t, 4, 0);
            pool[s].box = 0xFF;
        }
        np[t][h]--;
    }
}

static void box_reset(unsigned t)
{
    unsigned i;
    cn[t] = 0;
    for (i = 0; i < NEH; i++)
        on[t][i] = (unsigned char)(i == 0);     /* Handle 0: System      */
    for (i = 0; i < 4; i++)
        mh[t][i] = 0;
}

static void box_end(unsigned t)
{
    unsigned h;
    fr[t] = svc(VS_INFO, t, VI_EMSFRAME, 0);
    for (h = 0; h < NEH; h++)
        if (np[t][h])
            shrink(t, h, 0);
    box_reset(t);
}

static int newh(unsigned t)
{
    unsigned h, i;
    for (h = 1; h < NEH && on[t][h]; h++)
        ;
    if (h == NEH)
        return -1;
    for (i = 0; i < 8; i++)
        name[t][h][i] = 0;
    return (int)h;
}

/* n Paare (logische Seite, Rahmenplatz bzw. Segment bei mode 1) ab
   seg:off einblenden -> 0 oder Fehlercode */
static unsigned maplist(unsigned t, unsigned h, unsigned mode, unsigned seg,
                        unsigned off, unsigned n)
{
    unsigned i, r = 0, lg, pp;
    for (i = 0; i < n && !r; i++) {
        lg = rd16(t, seg, off + i * 4);
        pp = rd16(t, seg, off + i * 4 + 2);
        if (mode == 1)                          /* Segment statt Nummer  */
            pp = (pp - (fr[t] << 8)) >> 10;
        if (pp > 3)
            r = 0x8B;
        else if (lg == 0xFFFF) {
            show(t, pp, -1);
            mh[t][pp] = 0;
        } else if (lg >= np[t][h])
            r = 0x8A;
        else {
            show(t, pp, slot(t, h, lg));
            mh[t][pp] = (unsigned char)(h + 1);
            ml[t][pp] = lg;
        }
    }
    return r;
}

#define AH      (c->ax >> 8)
#define AL      (c->ax & 0xFF)
#define SETAH(v) (c->ax = (c->ax & 0xFF) | ((unsigned)(v) << 8))
#define SETAL(v) (c->ax = (c->ax & 0xFF00) | ((v) & 0xFF))
#define ERET(v)  do { SETAH(v); return 0; } while (0)

/* 55h/56h (EMS 4.0): Seiten nach der Liste einblenden, dann springen bzw.
   fern aufrufen; 56h blendet nach dem RETF des Ziels die alte Liste ein
   (Ruecksprung ueber F000:EMS_RET "ARPL E6h; RETF"). INT 67h kommt auf
   zwei Wegen: direkt aus dem Monitor (Rahmen nicht auf dem Stapel, CS:IP =
   Ruecksprung) oder ueber den Stummel F060:0020 (IRET-Rahmen oben auf dem
   Stapel, FLAGS schreibt der Aufrufer danach bei SP+4). */
static unsigned jmpcall(struct vdd_call __far *c, unsigned t, unsigned h)
{
    unsigned fn = AH, ds = c->ds, si = c->si, ss = c->ss, sp, cip, ccs, fl, r, k;
    unsigned tip = rd16(t, ds, si), tcs = rd16(t, ds, si + 2);
    if (fn == 0x56 && AL == 2) {                /* Stapelbedarf          */
        c->bx = 8;
        ERET(0);
    }
    if (AL > 1)
        ERET(0x8F);
    if (fn == 0x56 && cn[t] >= NEMSC)
        ERET(0x80);
    r = maplist(t, h, AL, rd16(t, ds, si + 7), rd16(t, ds, si + 5), rd8(t, ds, si + 4));
    if (r)
        ERET(r);
    if (fn == 0x56) {                           /* alte Liste merken     */
        k = cn[t]++;
        cst[t][k].off = rd16(t, ds, si + 10);
        cst[t][k].seg = rd16(t, ds, si + 12);
        cst[t][k].n = rd8(t, ds, si + 9);
        cst[t][k].mode = (unsigned char)AL;
        cst[t][k].h = (unsigned char)h;
    }
    SETAH(0);
    if (c->cs == 0xF060) {                      /* Rahmen IP CS FL bei SP */
        sp = c->sp;
        cip = rd16(t, ss, sp);
        ccs = rd16(t, ss, sp + 2);
        fl = rd16(t, ss, sp + 4);
        if (fn == 0x55) {                       /* IRET springt hin      */
            wr16(t, ss, sp, tip);
            wr16(t, ss, sp + 2, tcs);
            return 0;
        }
        sp -= 8;                                /* Ziel, FL, Ruecksprung */
        wr16(t, ss, sp, tip);                   /* zu EMS_RET, dann zum  */
        wr16(t, ss, sp + 2, tcs);               /* Aufrufer              */
        wr16(t, ss, sp + 4, fl);
        wr16(t, ss, sp + 6, EMS_RET);
        wr16(t, ss, sp + 8, VBIOS);
        wr16(t, ss, sp + 10, cip);
        wr16(t, ss, sp + 12, ccs);
        c->sp = sp;
        return 0;
    }
    if (fn == 0x56) {                           /* direkt aus dem Monitor */
        sp = c->sp - 8;
        wr16(t, ss, sp, EMS_RET);
        wr16(t, ss, sp + 2, VBIOS);
        wr16(t, ss, sp + 4, c->ip);
        wr16(t, ss, sp + 6, c->cs);
        c->sp = sp;
    }
    c->cs = tcs;
    c->ip = tip;
    return 0;
}

/* 57h: Adresse im Bereich -> Art/Adresse, Bytes bis zur Grenze, 0 = Fehler */
static unsigned x57(unsigned t, unsigned char typ, unsigned h, unsigned off, unsigned sp,
                    unsigned long pos, unsigned long *a, unsigned short *ty)
{
    unsigned long x;
    unsigned lp, o;
    int sl;
    if (typ == 0) {                             /* konventionell         */
        x = ((unsigned long)sp << 4) + off + pos;
        if (x >= 0x110000UL)
            return 0;
        *a = x;
        *ty = VA_BOX;
        return 0x8000;
    }
    x = (unsigned long)off + pos;               /* ab Anfang der Seite sp */
    lp = sp + (unsigned)(x >> 14);
    o = (unsigned)x & 0x3FFF;
    if (h >= NEH || !on[t][h] || lp >= np[t][h] || (sl = slot(t, h, lp)) < 0)
        return 0;
    *a = (pool[sl].base << 12) + o;
    *ty = VA_PHYS;
    return 0x4000 - o;
}

static unsigned copy(unsigned t, unsigned long s, unsigned ts, unsigned long d,
                     unsigned td, unsigned n)
{
    sv.lin = s;
    sv.ty1 = ts;
    sv.lin2 = d;
    sv.ty2 = td;
    sv.len = n;
    return svc(VS_COPY, t, 0, 0);
}

static unsigned ems(struct vdd_call __far *c, unsigned t)
{
    unsigned h = c->dx, n, i, k, es = c->es, di = c->di, ds = c->ds, si = c->si;
    int s;
    fr[t] = svc(VS_INFO, t, VI_EMSFRAME, 0);
    if (!fr[t])
        ERET(0x80);
    switch (AH) {                               /* mit Handle in DX      */
    case 0x44: case 0x45: case 0x47: case 0x48: case 0x4C:
    case 0x50: case 0x51: case 0x53: case 0x55:
        if (h >= NEH || !on[t][h])
            ERET(0x83);
    }
    switch (AH) {
    case 0x40:                                  /* Zustand               */
        ERET(0);
    case 0x41:                                  /* Seitenrahmen          */
        c->bx = fr[t] << 8;
        ERET(0);
    case 0x42:                                  /* freie/alle Seiten     */
        c->bx = efree(t);
        c->dx = pgn(t);
        ERET(0);
    case 0x43: case 0x5A:                       /* anlegen (5A: auch 0)  */
        n = c->bx;
        if (AH == 0x43 && n == 0)
            ERET(0x89);
        if (n > pgn(t) || n > EMS_MAXP)
            ERET(0x87);
        if (n > efree(t))
            ERET(0x88);
        if ((s = newh(t)) < 0)
            ERET(0x85);
        on[t][s] = 1;
        if (!grow(t, (unsigned)s, n)) {
            shrink(t, (unsigned)s, 0);
            on[t][s] = 0;
            ERET(0x88);
        }
        c->dx = (unsigned)s;
        ERET(0);
    case 0x44:                                  /* einblenden            */
        if (AL > 3)
            ERET(0x8B);
        if (c->bx == 0xFFFF) {
            show(t, AL, -1);
            mh[t][AL] = 0;
        } else {
            if (c->bx >= np[t][h])
                ERET(0x8A);
            show(t, AL, slot(t, h, c->bx));
            mh[t][AL] = (unsigned char)(h + 1);
            ml[t][AL] = c->bx;
        }
        ERET(0);
    case 0x45:                                  /* freigeben             */
        shrink(t, h, 0);
        if (h)
            on[t][h] = 0;
        ERET(0);
    case 0x46:                                  /* Version 4.0           */
        SETAL(0x40);
        ERET(0);
    case 0x47: case 0x48:                       /* Zuordnung sichern/zurueck */
        for (i = 0; i < 4; i++)
            if (AH == 0x47) {
                sav[t][h][i].h = mh[t][i];
                sav[t][h][i].lg = ml[t][i];
            } else {
                mh[t][i] = sav[t][h][i].h;
                ml[t][i] = sav[t][h][i].lg;
            }
        if (AH == 0x48)
            for (i = 0; i < 4; i++)
                show(t, i, mh[t][i] ? slot(t, mh[t][i] - 1, ml[t][i]) : -1);
        ERET(0);
    case 0x4B:                                  /* aktive Handles        */
        for (i = 0, n = 0; i < NEH; i++)
            n += on[t][i];
        c->bx = n;
        ERET(0);
    case 0x4C:                                  /* Seiten eines Handles  */
        c->bx = np[t][h];
        ERET(0);
    case 0x4D:                                  /* alle Handles -> ES:DI */
        for (i = 0, n = 0; i < NEH; i++)
            if (on[t][i]) {
                wr16(t, es, di + n * 4, i);
                wr16(t, es, di + n * 4 + 2, np[t][i]);
                n++;
            }
        c->bx = n;
        ERET(0);
    case 0x4E:                                  /* ganze Zuordnung (12 Byte) */
        if (AL == 3) {
            SETAL(12);
            ERET(0);
        }
        if (AL > 3)
            ERET(0x8F);
        if (AL == 0 || AL == 2)
            for (i = 0; i < 4; i++) {
                wr8(t, es, di + i * 3, mh[t][i]);
                wr16(t, es, di + i * 3 + 1, ml[t][i]);
            }
        if (AL == 1 || AL == 2)
            for (i = 0; i < 4; i++) {
                mh[t][i] = rd8(t, ds, si + i * 3);
                ml[t][i] = rd16(t, ds, si + i * 3 + 1);
                k = mh[t][i];
                show(t, i, k && k <= NEH && on[t][k - 1] && ml[t][i] < np[t][k - 1]
                           ? slot(t, k - 1, ml[t][i]) : -1);
            }
        ERET(0);
    case 0x50:                                  /* mehrere einblenden    */
        if (AL > 1)
            ERET(0x8F);
        ERET(maplist(t, h, AL, ds, si, c->cx));
    case 0x55: case 0x56:                       /* einblenden + Sprung/Aufruf */
        return jmpcall(c, t, h);
    case 0x51:                                  /* Groesse aendern       */
        n = c->bx;
        if (n > np[t][h]) {
            if (n - np[t][h] > efree(t) || !grow(t, h, n - np[t][h]))
                ERET(0x88);
        } else
            shrink(t, h, n);
        c->bx = np[t][h];
        ERET(0);
    case 0x53:                                  /* Name holen/setzen     */
        if (AL > 1)
            ERET(0x8F);
        for (i = 0; i < 8; i++)
            if (AL == 0)
                wr8(t, es, di + i, name[t][h][i]);
            else
                name[t][h][i] = rd8(t, ds, si + i);
        ERET(0);
    case 0x54:                                  /* Handle-Verzeichnis    */
        if (AL == 2) {
            c->bx = NEH;
            ERET(0);
        }
        ERET(AL > 2 ? 0x8F : 0xA0);
    case 0x57: {                                /* Bereich kopieren/tauschen */
        static unsigned char xb[512];
        static unsigned long sa, da;    /* im DS (SS ist der Kernelstapel) */
        static unsigned short sty, dty;
        unsigned long len, pos = 0;
        unsigned char st, dt;
        unsigned sh, so, ssp, dh, dof, dsp, a, b;
        box_io(t, ds, si, 18, 1);
        len = mb[0] | ((unsigned)mb[1] << 8) | ((unsigned long)(mb[2] | (mb[3] << 8)) << 16);
        st = mb[4];
        sh = mb[5] | (mb[6] << 8);
        so = mb[7] | (mb[8] << 8);
        ssp = mb[9] | (mb[10] << 8);
        dt = mb[11];
        dh = mb[12] | (mb[13] << 8);
        dof = mb[14] | (mb[15] << 8);
        dsp = mb[16] | (mb[17] << 8);
        if (AL > 1 || st > 1 || dt > 1)
            ERET(0x8F);
        if (len > 0x100000UL)
            ERET(0x96);
        while (pos < len) {
            a = x57(t, st, sh, so, ssp, pos, &sa, &sty);
            b = x57(t, dt, dh, dof, dsp, pos, &da, &dty);
            if (!a || !b)
                ERET(0x8A);
            n = a < b ? a : b;
            if (AL == 1 && n > sizeof(xb))
                n = sizeof(xb);
            if (len - pos < n)
                n = (unsigned)(len - pos);
            if (AL == 0) {
                if (copy(t, sa, sty, da, dty, n))
                    ERET(0x8A);
            } else if (copy(t, da, dty, (unsigned)xb, VA_MOD, n) ||    /* tauschen */
                       copy(t, sa, sty, da, dty, n) ||
                       copy(t, (unsigned)xb, VA_MOD, sa, sty, n))
                ERET(0x8A);
            pos += n;
        }
        ERET(0);
    }
    case 0x58:                                  /* Rahmenadressen        */
        if (AL == 0)
            for (i = 0; i < 4; i++) {
                wr16(t, es, di + i * 4, (fr[t] << 8) + (i << 10));
                wr16(t, es, di + i * 4 + 2, i);
            }
        else if (AL != 1)
            ERET(0x8F);
        c->cx = 4;
        ERET(0);
    case 0x59:                                  /* Hardware: nur Seiten  */
        if (AL != 1)
            ERET(AL ? 0x8F : 0xA4);
        c->bx = efree(t);
        c->dx = pgn(t);
        ERET(0);
    }
    ERET(0x84);                                 /* unbekannt             */
}

static unsigned int_fn(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task, k;
    if (t >= NT)
        return VDD_NOTMINE;
    if (c->port == 0x67)
        return ems(c, t);
    if (c->port == 0xE6) {                      /* RETF des Ziels von 56h */
        if (cn[t]) {
            fr[t] = svc(VS_INFO, t, VI_EMSFRAME, 0);
            k = --cn[t];
            SETAH(maplist(t, cst[t][k].h, cst[t][k].mode,
                          cst[t][k].seg, cst[t][k].off, cst[t][k].n));
        }
        return 0;
    }
    return VDD_NOTMINE;
}

static unsigned event(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task;
    if (t >= NT)
        return 0;
    if (c->fn == VDD_EV_CREATE)
        box_reset(t);
    else if (c->fn == VDD_EV_DESTROY)           /* Seiten zurueck        */
        box_end(t);
    return 0;
}

#pragma code_seg("ITEXT", "ICODE")   /* nur beim INIT */
static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned i;
    for (i = 0; i < NPOOL; i++)
        pool[i].box = 0xFF;
    for (i = 0; i < NT; i++)
        box_reset(i);
    reg.version = VDD_VERSION;
    reg.size = sizeof(reg);
    scat(reg.name, "VEMS");
    reg.int_off = fent(0, int_fn);
    reg.ev_off = fent(1, event);
    reg.ints[0] = 0x67;
    reg.ints[1] = 0xE6;
    if (devhlp(DH_VDDREG, 0, (unsigned)&reg, 0)) {
        dputs("VEMS.SYS: Anmeldung beim Kernel gescheitert\n");
        r->status = DRV_ERR_NODEV;
        return;
    }
    dputs("  EMS 4.0 der DOS-Boxen (Rahmen 64 KB, bis 32 MB je Box)\n");
    r->units = 0;
    r->status = DRV_OK;
}
#pragma code_seg("_TEXT", "CODE")

void strategy(void)
{
    struct drv_req __far *r = REQ;
    unsigned t;
    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    case DRV_DEINIT:                    /* DEVLOAD /U: alle Seiten zurueck */
        for (t = 0; t < NT; t++)
            box_end(t);
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
