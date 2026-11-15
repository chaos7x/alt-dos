/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LZMA-Packer - eigene Umsetzung nach der LZMA-Spezifikation von Igor
 * Pavlov (lzma-specification.txt, gemeinfrei; nur gelesen). Gegenstueck
 * zum Entpacker in lzma.c.
 *
 * Bereichscodierer: low 32 Bit + Uebertrag (`carry`) statt 64 Bit,
 * (range >> 11) * p ueber zwei 16x16-Multiplikationen (kein __U4M).
 * Suche: Woerterbuch 128 KB als Ring in zwei 64-KB-Segmenten, Kopf-
 * tabelle (32K Eintraege zu 32 Bit, 2 Segmente) und Kette mit 16-Bit-
 * Abstaenden je Stelle (4 Segmente). Entscheidung ohne Preisrechnung:
 * Wiederholungsabstand vor neuem Treffer, wenn er hoechstens 1 kuerzer
 * ist; verzoegerte Suche (eine Stelle) ab Stufe 4; 1-Byte-Wiederholung
 * (ShortRep), wenn das Byte an rep0 passt.
 */
#ifndef HOST
#define FAPI_NO_IMPL
#include <i86.h>
#include "fapi.h"
#endif
#include "lzmaenc.h"

int (*lme_write)(const unsigned char *p, unsigned n);
unsigned long lme_nout;

#ifndef HOST
#pragma aux mul16 = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];
unsigned long mul16(unsigned a, unsigned b);
#endif

#define WMASK    0x1FFFFUL
#define MAXM     273
#define DMAX     (0x20000UL - 8192)     /* groesster Abstand             */
#define CHUNK    4096

static unsigned wsel[2], hsel[2], csel[4];
static unsigned long pos, wend;
static unsigned depth, nice, lazy;
static int werr;

/* ---- Ausgabe ------------------------------------------------------------- */
static unsigned char ob[2048];
static unsigned opos;

static void put_byte(unsigned c)
{
    ob[opos++] = (unsigned char)c;
    if (opos == sizeof(ob)) {
        if (lme_write(ob, opos))
            werr = 1;
        lme_nout += opos;
        opos = 0;
    }
}

/* ---- Bereichscodierer ---------------------------------------------------- */
static unsigned long low, range, csize;
static unsigned char cache, carry;

static void shift_low(void)
{
    if (carry || low < 0xFF000000UL) {
        unsigned t = cache;
        do {
            put_byte((t + carry) & 0xFF);
            t = 0xFF;
        } while (--csize);
        cache = (unsigned char)(low >> 24);
    }
    csize++;
    low <<= 8;
    carry = 0;
}

static void norm(void)
{
    while (range < 0x1000000UL) {
        range <<= 8;
        shift_low();
    }
}

static void ebit(unsigned short *p, unsigned b)
{
    unsigned long r = range >> 11, bound;
    unsigned pr = *p;
    bound = mul16((unsigned)r, pr) + ((unsigned long)((unsigned)(r >> 16) * pr) << 16);
    if (!b) {
        range = bound;
        *p = pr + ((2048 - pr) >> 5);
    } else {
        unsigned long ol = low;
        low += bound;
        if (low < ol)
            carry = 1;
        range -= bound;
        *p = pr - (pr >> 5);
    }
    norm();
}

static void edirect(unsigned long v, unsigned n)
{
    while (n--) {
        range >>= 1;
        if ((v >> n) & 1) {
            unsigned long ol = low;
            low += range;
            if (low < ol)
                carry = 1;
        }
        norm();
    }
}

static void etree(unsigned short *p, unsigned nb, unsigned v)
{
    unsigned m = 1, b;
    while (nb--) {
        b = (v >> nb) & 1;
        ebit(&p[m], b);
        m = (m << 1) | b;
    }
}

static void ertree(unsigned short *p, unsigned nb, unsigned v)
{
    unsigned m = 1, b;
    while (nb--) {
        b = v & 1;
        v >>= 1;
        ebit(&p[m], b);
        m = (m << 1) | b;
    }
}

/* ---- Modell -------------------------------------------------------------- */
struct lenc {
    unsigned short ch, ch2, lo[4][8], mid[4][8], hi[256];
};
static unsigned short is_match[12 * 4], is_rep[12], is_g0[12], is_g1[12], is_g2[12];
static unsigned short is_r0l[12 * 4], slotp[4][64], spec[115], alignp[16];
static struct lenc lenc, replenc;
static unsigned short lit[0x300 * 8];
static unsigned state;
static unsigned long rep[4];            /* Abstand - 1                   */

static void pinit(unsigned short *p, unsigned n)
{
    while (n--)
        *p++ = 1024;
}

static void model_init(void)
{
    pinit(is_match, sizeof(is_match) / 2);
    pinit(is_rep, 12);
    pinit(is_g0, 12);
    pinit(is_g1, 12);
    pinit(is_g2, 12);
    pinit(is_r0l, sizeof(is_r0l) / 2);
    pinit(&slotp[0][0], sizeof(slotp) / 2);
    pinit(spec, 115);
    pinit(alignp, 16);
    pinit(&lenc.ch, sizeof(lenc) / 2);
    pinit(&replenc.ch, sizeof(replenc) / 2);
    pinit(lit, sizeof(lit) / 2);
    state = 0;
    rep[0] = rep[1] = rep[2] = rep[3] = 0;
}

/* ---- Fenster ------------------------------------------------------------- */
static unsigned char wb(unsigned long a)
{
    return *((unsigned char __far *)MK_FP(wsel[(unsigned)(a >> 16) & 1], (unsigned)a));
}

static unsigned mlen(unsigned long p, unsigned long c, unsigned max)
{
    unsigned o1 = (unsigned)p, o2 = (unsigned)c, n = 0;
    if (o1 < 0xFFFFU - MAXM && o2 < 0xFFFFU - MAXM) {
        unsigned char __far *a = MK_FP(wsel[(unsigned)(p >> 16) & 1], o1);
        unsigned char __far *b = MK_FP(wsel[(unsigned)(c >> 16) & 1], o2);
        while (n < max && a[n] == b[n])
            n++;
        return n;
    }
    while (n < max && wb(p + n) == wb(c + n))
        n++;
    return n;
}

#define HD(h)  ((unsigned long __far *)MK_FP(hsel[(h) >> 14], ((h) & 0x3FFF) << 2))
#define CH(p)  ((unsigned short __far *)MK_FP(csel[(unsigned)((p) >> 15) & 3], \
                                              ((unsigned)(p) & 0x7FFF) << 1))

static unsigned hash(unsigned long p)
{
    return (((unsigned)wb(p) << 7) ^ ((unsigned)wb(p + 1) << 4) ^ wb(p + 2)) & 0x7FFF;
}

static void insert(unsigned long p)
{
    unsigned h;
    unsigned long prev, d;
    if (p + 2 >= wend)
        return;
    h = hash(p);
    prev = *HD(h);
    *HD(h) = p + 1;
    d = prev ? p + 1 - prev : 0;
    *CH(p) = d < 0x10000UL ? (unsigned short)d : 0;
}

static unsigned find(unsigned long p, unsigned maxl, unsigned long *dist)
{
    unsigned long c1, c, d;
    unsigned best = 2, l, steps = depth, dd;
    if (maxl < 3 || p + 2 >= wend)
        return 0;
    c1 = *HD(hash(p));
    if (!c1)
        return 0;
    c = c1 - 1;
    for (;;) {
        d = p - c;
        if (d == 0 || d > DMAX || d > p)
            break;
        if (wb(c + best) == wb(p + best) && wb(c) == wb(p)) {
            l = mlen(p, c, maxl);
            if (l > best) {
                best = l;
                *dist = d;
                if (l >= nice)
                    break;
            }
        }
        dd = *CH(c);
        if (!dd || !--steps || dd > c)
            break;
        c -= dd;
    }
    if (best < 3)
        return 0;
    if (best == 3 && *dist > 0x8000UL)  /* weiter Abstand: 3 Literale billiger */
        return 0;
    return best;
}

/* ---- Codieren ------------------------------------------------------------ */
static void e_lit(void)
{
    unsigned ps = (unsigned)pos & 3, c = wb(pos), prev = pos ? wb(pos - 1) : 0;
    unsigned short *pr = lit + 0x300 * (prev >> 5);
    ebit(&is_match[(state << 2) + ps], 0);
    if (state < 7)
        etree(pr, 8, c);
    else {
        unsigned mb = wb(pos - rep[0] - 1), offs = 0x100, sym = c | 0x100, i;
        for (i = 0; i < 8; i++) {
            mb <<= 1;
            ebit(&pr[offs + (mb & offs) + (sym >> 8)], (sym >> 7) & 1);
            sym <<= 1;
            offs &= ~(mb ^ sym);
        }
    }
    state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
}

static void e_len(struct lenc *L, unsigned l, unsigned ps)
{
    if (l < 8) {
        ebit(&L->ch, 0);
        etree(L->lo[ps], 3, l);
    } else if (l < 16) {
        ebit(&L->ch, 1);
        ebit(&L->ch2, 0);
        etree(L->mid[ps], 3, l - 8);
    } else {
        ebit(&L->ch, 1);
        ebit(&L->ch2, 1);
        etree(L->hi, 8, l - 16);
    }
}

static void e_match(unsigned long dist, unsigned len)
{
    unsigned ps = (unsigned)pos & 3, ls = len - 2 > 3 ? 3 : len - 2, sl, nd, i;
    unsigned long d = dist - 1, base;
    ebit(&is_match[(state << 2) + ps], 1);
    ebit(&is_rep[state], 0);
    e_len(&lenc, len - 2, ps);
    if (d < 4)
        sl = (unsigned)d;
    else {
        for (i = 31; !((d >> i) & 1); i--)
            ;
        sl = 2 * i + (unsigned)((d >> (i - 1)) & 1);
    }
    etree(slotp[ls], 6, sl);
    if (sl >= 4) {
        nd = (sl >> 1) - 1;
        base = (unsigned long)(2 | (sl & 1)) << nd;
        if (sl < 14)
            ertree(spec + (unsigned)base - sl, nd, (unsigned)(d - base));
        else {
            edirect((d - base) >> 4, nd - 4);
            ertree(alignp, 4, (unsigned)(d - base) & 15);
        }
    }
    rep[3] = rep[2];
    rep[2] = rep[1];
    rep[1] = rep[0];
    rep[0] = d;
    state = state < 7 ? 7 : 10;
}

static void e_rep(unsigned i, unsigned len)
{
    unsigned ps = (unsigned)pos & 3;
    unsigned long d;
    ebit(&is_match[(state << 2) + ps], 1);
    ebit(&is_rep[state], 1);
    if (i == 0) {
        ebit(&is_g0[state], 0);
        ebit(&is_r0l[(state << 2) + ps], 1);
    } else {
        ebit(&is_g0[state], 1);
        if (i == 1)
            ebit(&is_g1[state], 0);
        else {
            ebit(&is_g1[state], 1);
            ebit(&is_g2[state], i - 2);
        }
        d = rep[i];
        if (i == 3)
            rep[3] = rep[2];
        if (i >= 2)
            rep[2] = rep[1];
        rep[1] = rep[0];
        rep[0] = d;
    }
    e_len(&replenc, len - 2, ps);
    state = state < 7 ? 8 : 11;
}

static void e_short(void)
{
    unsigned ps = (unsigned)pos & 3;
    ebit(&is_match[(state << 2) + ps], 1);
    ebit(&is_rep[state], 1);
    ebit(&is_g0[state], 0);
    ebit(&is_r0l[(state << 2) + ps], 0);
    state = state < 7 ? 9 : 11;
}

static unsigned nml;                    /* Ergebnis der verzoegerten Suche */
static unsigned long nmd;
static int ncache;

static void advance(unsigned len)
{
    while (--len) {
        pos++;
        insert(pos);
    }
    pos++;
}

static void step(void)
{
    unsigned long avail = wend - pos, md = 0, d;
    unsigned maxl = avail > MAXM ? MAXM : (unsigned)avail, rl = 0, ri = 0, ml, l, i;

    for (i = 0; i < 4 && maxl >= 2; i++) {
        d = rep[i] + 1;
        if (d > pos)
            continue;
        l = mlen(pos, pos - d, maxl);
        if (l > rl) {
            rl = l;
            ri = i;
        }
    }
    if (ncache) {
        ml = nml;
        md = nmd;
        ncache = 0;
    } else
        ml = find(pos, maxl, &md);
    insert(pos);
    if (rl >= 2 && rl + 1 >= ml) {
        e_rep(ri, rl);
        advance(rl);
        return;
    }
    if (ml >= 3) {
        if (lazy && ml < nice && avail > 1) {
            unsigned long a2 = avail - 1;
            unsigned m2 = a2 > MAXM ? MAXM : (unsigned)a2;
            nml = find(pos + 1, m2, &nmd);
            if (nml > ml) {
                ncache = 1;
                goto single;
            }
        }
        e_match(md, ml);
        advance(ml);
        return;
    }
single:
    if (pos > rep[0] && wb(pos) == wb(pos - rep[0] - 1))
        e_short();
    else
        e_lit();
    pos++;
}

/* ---- Schnittstelle ------------------------------------------------------- */
int lme_open(void)
{
    unsigned i, s;
    for (i = 0; i < 8; i++) {
        s = DosAllocSeg(0);
        if (FAPI_FAILED(s) || !s)
            return 1;
        if (i < 2)
            wsel[i] = s;
        else if (i < 4)
            hsel[i - 2] = s;
        else
            csel[i - 4] = s;
    }
    return 0;
}

void lme_close(void)
{
    unsigned i;
    for (i = 0; i < 2; i++) {
        if (wsel[i]) DosFreeSeg(wsel[i]);
        if (hsel[i]) DosFreeSeg(hsel[i]);
    }
    for (i = 0; i < 4; i++)
        if (csel[i]) DosFreeSeg(csel[i]);
}

void lme_props(unsigned char *p5)
{
    p5[0] = 0x5D;                       /* (pb * 5 + lp) * 9 + lc        */
    p5[1] = 0;
    p5[2] = 0;
    p5[3] = (unsigned char)(LME_DICT >> 16);
    p5[4] = 0;
}

void lme_begin(int level)
{
    unsigned i;
    unsigned long __far *h;
    for (i = 0; i < 2; i++) {           /* Kopftabelle leeren            */
        unsigned j;
        h = MK_FP(hsel[i], 0);
        for (j = 0; j < 0x4000; j++)
            h[j] = 0;
    }
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    depth = level <= 2 ? 4 : level <= 4 ? 16 : level <= 6 ? 48 : level <= 8 ? 128 : 512;
    nice = level <= 4 ? 32 : level <= 6 ? 64 : level <= 8 ? 128 : MAXM;
    lazy = level >= 4;
    pos = wend = 0;
    ncache = 0;
    werr = 0;
    opos = 0;
    lme_nout = 0;
    low = 0;
    range = 0xFFFFFFFFUL;
    cache = 0;
    csize = 1;
    carry = 0;
    model_init();
}

int lme_feed(const unsigned char *p, unsigned n)
{
    unsigned k, off;
    unsigned char __far *w;
    while (n && !werr) {
        while (wend - pos > MAXM + 1)
            step();
        k = n > CHUNK ? CHUNK : n;
        n -= k;
        while (k) {
            unsigned long room;
            unsigned m;
            off = (unsigned)wend;
            room = 0x10000UL - off;
            m = room < k ? (unsigned)room : k;
            w = MK_FP(wsel[(unsigned)(wend >> 16) & 1], off);
            k -= m;
            wend += m;
            while (m--)
                *w++ = *p++;
        }
    }
    return werr;
}

int lme_end(void)
{
    unsigned i;
    while (pos < wend && !werr)
        step();
    for (i = 0; i < 5; i++)
        shift_low();
    if (opos && !werr) {
        if (lme_write(ob, opos))
            werr = 1;
        lme_nout += opos;
        opos = 0;
    }
    return werr;
}
