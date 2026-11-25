/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - FCB-Dateiarbeit (0Fh-17h, 21h-24h, 27h, 28h) wie DOS 1.x
 * bis 6.x, auf dem DEM wie die Handles.
 *
 * Aufbau des FCB wie MS-DOS 4.0 (FCB.INC): 0 Laufwerk (0 = aktuelles),
 * 1 Name, 9 Endung, 0Ch Block, 0Eh Satzlaenge, 10h Groesse, 14h Datum,
 * 16h Zeit, 18h-1Fh frei fuer DOS (hier: 18h SFT-Nummer, 19h Kennung),
 * 20h Satz im Block, 21h Satz fuer wahlfreien Zugriff. Erweiterter FCB:
 * FFh, 5 Byte frei, Attribut, dann der FCB. Bei der Suche (11h/12h) steht
 * der Suchzustand des DEM ab 0Ch im FCB des Aufrufers (wie MS-DOS: DOS
 * fuehrt ihn dort) - keine Tabelle im Kern.
 */
#include "cm.h"

#define FCB_MARK    0xA5                /* 19h: von uns geoeffnet        */

/* 32 Bit x 16 Bit und 32 Bit / 16 Bit ohne Laufzeitbibliothek */
u32 lmul(u16 a, u16 b);
#pragma aux lmul = "mul dx" parm [ax] [dx] value [dx ax] modify exact [ax dx];
static u32 mul32(u32 a, u16 b)
{
    return lmul((u16)a, b) + ((u32)(u16)((u16)(a >> 16) * b) << 16);
}
u16 ldiv16(u16 hi, u16 lo, u16 d, u16 *rem);
#pragma aux ldiv16 = \
    "div bx" \
    "mov [si], dx" \
    parm [dx] [ax] [bx] [si] value [ax] modify exact [ax dx];
/* a / d (Ergebnis 32 Bit), Rest in *r */
static u32 div32(u32 a, u16 d, u16 *r)
{
    u16 rem, qh, ql;
    qh = ldiv16(0, (u16)(a >> 16), d, &rem);
    ql = ldiv16(rem, (u16)a, d, &rem);
    *r = rem;
    return ((u32)qh << 16) | ql;
}

static char fp[128];

/* FCB-Zeiger (normal) aus DS:DX; attr des erweiterten FCB */
static u8 __far *fcb_of(struct frame __far *f, u8 *attr)
{
    u8 __far *p = FP(f->ds, f->dx);
    *attr = 0;
    if (p[0] == 0xFF) {
        *attr = p[6];
        return p + 7;
    }
    return p;
}

/* "X:\DIR\NAME.EXT" aus Laufwerk und Name an n (11 Zeichen) */
static int fcb_path(u8 __far *b, const u8 __far *n, char *o)
{
    unsigned d = b[0] ? b[0] - 1 : cur_drv, k, l, i;
    if (!drive_ok(d))
        return E_DRIVE;
    for (l = 0; cdstab[d].path[l] && l < 66; l++)
        o[l] = cdstab[d].path[l];
    if (o[l - 1] != '\\')
        o[l++] = '\\';
    for (k = 0; k < 8 && n[k] != ' '; k++)
        o[l++] = (char)n[k];
    for (i = 8; i < 11 && n[i] == ' '; i++)
        ;
    if (i < 11) {
        o[l++] = '.';
        for (k = 8; k < 11 && n[k] != ' '; k++)
            o[l++] = (char)n[k];
    }
    o[l] = 0;
    for (k = 0; o[k]; k++)
        if (o[k] >= 'a' && o[k] <= 'z')
            o[k] -= 32;
    return 0;
}

static void cpyq(char *d, const char *s)
{
    while ((*d++ = *s++) != 0)
        ;
}

/* geoeffnete SFT des FCB oder -1 */
static int fcb_sft(u8 __far *b)
{
    u8 s = b[0x18];
    if (b[0x19] != FCB_MARK || s >= NSFT || !sftab[s].refs)
        return -1;
    return s;
}

static void fcb_fill(u8 __far *b, int s)
{
    b[0x0C] = b[0x0D] = 0;              /* Block 0                       */
    W(FP_SEG(b), FP_OFF(b) + 0x0E) = 128;
    D(FP_SEG(b), FP_OFF(b) + 0x10) = sftab[s].size;
    W(FP_SEG(b), FP_OFF(b) + 0x14) = sftab[s].date;
    W(FP_SEG(b), FP_OFF(b) + 0x16) = sftab[s].time;
    b[0x18] = (u8)s;
    b[0x19] = FCB_MARK;
    b[0x20] = 0;
    if (!b[0])
        b[0] = (u8)(cur_drv + 1);
}

/* Satz rec lesen/schreiben (n Saetze) -> AL wie DOS */
static u8 fcb_io(u8 __far *b, u32 rec, unsigned n, int wr, unsigned *done)
{
    int s = fcb_sft(b);
    u16 rs = W(FP_SEG(b), FP_OFF(b) + 0x0E), k;
    u32 len;
    unsigned got;
    *done = 0;
    if (s < 0)
        return 1;
    if (!rs)
        rs = 128;
    len = mul32((u32)n, rs);
    if (len > 0xFFF0UL)
        return 2;                       /* ueber das Segment             */
    q.handle = sftab[s].hnd;
    q.pos = mul32(rec, rs);
    q.size = sftab[s].size;
    q.buf = ((u32)dta_seg << 4) + dta_off;
    q.count = len;
    if (!wr && q.pos >= sftab[s].size)
        return 1;                       /* Dateiende                     */
    if (dem(wr ? DEM_WRITE : DEM_READ))
        return 1;
    sftab[s].size = q.size;
    D(FP_SEG(b), FP_OFF(b) + 0x10) = q.size;
    got = (unsigned)q.count;
    if (wr) {
        sftab[s].flags &= ~0x40;
        *done = n;
        return 0;
    }
    *done = (got + rs - 1) / rs;
    if (got == (unsigned)len)
        return 0;
    for (k = got; k < *done * rs; k++)  /* Rest des Satzes mit Nullen */
        B(dta_seg, dta_off + k) = 0;
    return got ? 3 : 1;
}

static u32 cur_rec(u8 __far *b)
{
    return ((u32)W(FP_SEG(b), FP_OFF(b) + 0x0C) << 7) + b[0x20];
}

static void set_cur(u8 __far *b, u32 r)
{
    W(FP_SEG(b), FP_OFF(b) + 0x0C) = (u16)(r >> 7);
    b[0x20] = (u8)(r & 127);
}

static u32 rnd_rec(u8 __far *b)
{
    u32 r = D(FP_SEG(b), FP_OFF(b) + 0x21);
    if (W(FP_SEG(b), FP_OFF(b) + 0x0E) >= 64)
        r &= 0xFFFFFFUL;                /* nur 3 Byte                    */
    return r;
}

static void set_rnd(u8 __far *b, u32 r)
{
    W(FP_SEG(b), FP_OFF(b) + 0x21) = (u16)r;
    b[0x23] = (u8)(r >> 16);
    if (W(FP_SEG(b), FP_OFF(b) + 0x0E) < 64)
        b[0x24] = (u8)(r >> 24);
}

/* gefundenen Eintrag (q) ins DTA wie DOS: [erweitert] Laufwerk + Eintrag */
static void find_dta(u8 __far *p, u8 __far *b, u8 xattr)
{
    u16 o = dta_off;
    unsigned i;
    if (p[0] == 0xFF) {
        B(dta_seg, o) = 0xFF;
        for (i = 1; i < 6; i++)
            B(dta_seg, o + i) = 0;
        B(dta_seg, o + 6) = xattr;
        o += 7;
    }
    B(dta_seg, o) = b[0] ? b[0] : (u8)(cur_drv + 1);
    for (i = 0; i < 11; i++)
        B(dta_seg, o + 1 + i) = q.name[i];
    B(dta_seg, o + 12) = (u8)q.attr;
    for (i = 13; i < 23; i++)
        B(dta_seg, o + i) = 0;
    W(dta_seg, o + 23) = q.time;
    W(dta_seg, o + 25) = q.date;
    W(dta_seg, o + 27) = 0;
    D(dta_seg, o + 29) = q.size;
}

/* Fehler wie MS-DOS: AL = FFh, erweiterter Fehler (59h) gesetzt - DIR von
   COMMAND.COM 4.0 sucht per FCB und erwartet am Ende 12h */
static void fcb_fail(struct frame __far *f, unsigned e)
{
    u8 ah = AH(f);
    if (e == E_NOFILE)
        e = E_NOMORE;
    set_err(f, e ? e : E_NOMORE);
    f->fl &= ~FL_CF;
    f->ax = ((u16)ah << 8) | 0xFF;
}

void fcb_fn(struct frame __far *f)
{
    u8 attr;
    u8 __far *p = FP(f->ds, f->dx);
    u8 __far *b = fcb_of(f, &attr);
    unsigned res, done, i;
    int s;
    u32 r;
    switch (AH(f)) {
    case 0x0F: case 0x16:               /* oeffnen / anlegen             */
        if (fcb_path(b, b + 1, fp)) {
            AL(f) = 0xFF;
            return;
        }
        s = open_path(fp, 2, AH(f) == 0x0F ? DEM_ACT_OPEN : DEM_ACT_CREATE,
                      attr & 0x27, &res);
        if (s < 0 && AH(f) == 0x0F)
            s = open_path(fp, 0, DEM_ACT_OPEN, 0, &res);
        if (s < 0) {
            u8 ah = AH(f);
            set_err(f, (unsigned)-s);
            f->fl &= ~FL_CF;
            f->ax = ((u16)ah << 8) | 0xFF;
            return;
        }
        sftab[s].mode |= 0x8000;        /* FCB                           */
        fcb_fill(b, s);
        AL(f) = 0;
        return;
    case 0x10:
        if ((s = fcb_sft(b)) >= 0) {
            sftab[s].date = W(FP_SEG(b), FP_OFF(b) + 0x14);
            sftab[s].time = W(FP_SEG(b), FP_OFF(b) + 0x16);
            sft_close(s);
            b[0x19] = 0;
        }
        AL(f) = 0;
        return;
    case 0x11: case 0x12:
        if (AH(f) == 0x11) {
            if (fcb_path(b, b + 1, fp)) {
                AL(f) = 0xFF;
                return;
            }
            cpyq(q.path, fp);
            q.attr = p[0] == 0xFF ? attr : 0;
            if (dem(DEM_FINDFIRST)) {
                fcb_fail(f, q.err);
                return;
            }
        } else {
            q.find[0] = (u8)(0x80 | (b[0] ? b[0] - 1 : cur_drv));
            for (i = 1; i < 21; i++)
                q.find[i] = b[0x0B + i];
            if (dem(DEM_FINDNEXT)) {
                fcb_fail(f, E_NOMORE);
                return;
            }
        }
        for (i = 1; i < 21; i++)        /* Zustand in den FCB (ab 0Ch)   */
            b[0x0B + i] = q.find[i];
        find_dta(p, b, (u8)q.attr);
        AL(f) = 0;
        return;
    case 0x13:
        if (p[0] == 0xFF && (attr & 0x08)) {   /* Bezeichnung loeschen  */
            unsigned d = b[0] ? b[0] - 1 : cur_drv;
            if (!drive_ok(d)) {
                AL(f) = 0xFF;
                return;
            }
            q.path[0] = (char)('A' + d);
            q.path[1] = ':';
            q.path[2] = 0;
            AL(f) = dem(DEM_LABEL) ? 0xFF : 0;
            return;
        }
        if (fcb_path(b, b + 1, fp)) {
            AL(f) = 0xFF;
            return;
        }
        cpyq(q.path, fp);
        AL(f) = dem(DEM_DELETE) ? 0xFF : 0;
        return;
    case 0x17: {
        static char np[128];
        if (fcb_path(b, b + 1, fp) || fcb_path(b, b + 0x11, np)) {
            AL(f) = 0xFF;
            return;
        }
        cpyq(q.path, fp);
        cpyq(q.path2, np);
        AL(f) = dem(DEM_RENAME) ? 0xFF : 0;
        return;
    }
    case 0x14: case 0x15:               /* der Reihe nach                */
        r = cur_rec(b);
        AL(f) = fcb_io(b, r, 1, AH(f) == 0x15, &done);
        if (done)
            set_cur(b, r + 1);
        return;
    case 0x21: case 0x22:               /* wahlfrei                      */
        r = rnd_rec(b);
        set_cur(b, r);
        AL(f) = fcb_io(b, r, 1, AH(f) == 0x22, &done);
        return;
    case 0x27: case 0x28:               /* wahlfrei, CX Saetze           */
        r = rnd_rec(b);
        if (AH(f) == 0x28 && !f->cx) {  /* Groesse setzen                */
            if ((s = fcb_sft(b)) < 0) {
                AL(f) = 1;
                return;
            }
            q.handle = sftab[s].hnd;
            q.pos = mul32(r, W(FP_SEG(b), FP_OFF(b) + 0x0E));
            q.size = sftab[s].size;
            q.count = 0;
            AL(f) = dem(DEM_WRITE) ? 1 : 0;
            sftab[s].size = q.size;
            D(FP_SEG(b), FP_OFF(b) + 0x10) = q.size;
            return;
        }
        AL(f) = fcb_io(b, r, f->cx, AH(f) == 0x28, &done);
        f->cx = done;
        set_rnd(b, r + done);
        set_cur(b, r + done);
        return;
    case 0x23: {                        /* Groesse in Saetzen            */
        u16 rs = W(FP_SEG(b), FP_OFF(b) + 0x0E), rem;
        if (fcb_path(b, b + 1, fp)) {
            AL(f) = 0xFF;
            return;
        }
        cpyq(q.path, fp);
        if (!rs) {
            AL(f) = 0xFF;
            return;
        }
        if (dem(DEM_GETATTR)) {
            AL(f) = 0xFF;
            return;
        }
        r = div32(q.size, rs, &rem);
        set_rnd(b, r + (rem ? 1 : 0));
        AL(f) = 0;
        return;
    }
    case 0x24:
        set_rnd(b, cur_rec(b));
        return;
    }
}
