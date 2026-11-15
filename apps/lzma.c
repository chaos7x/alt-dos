/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LZMA/LZMA2-Entpacker, BCJ (x86) und Delta - eigene Umsetzung nach der
 * LZMA-Spezifikation (Igor Pavlov, lzma-specification.txt und der
 * Beschreibung von LZMA2/BCJ im LZMA SDK, gemeinfrei; nur gelesen).
 *
 * 286-Code ohne Laufzeitsystem: 32-Bit-Multiplikation des Bereichs-
 * decoders ueber zwei 16x16-Multiplikationen (mul16), Wahrscheinlich-
 * keiten in DGROUP (lc + lp <= 4 wie bei LZMA2: 28 KB), Woerterbuch als
 * Ring aus ganzen 64-KB-Segmenten.
 */
#define FAPI_NO_IMPL
#include <i86.h>
#include "fapi.h"
#include "lzma.h"

unsigned (*lm_read)(unsigned char *buf, unsigned n);
int      (*lm_write)(const unsigned char *p, unsigned n);
int      (*lm_back)(unsigned long pos, unsigned char *b, unsigned n);
unsigned long lm_rsize;

#pragma aux mul16 = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];
unsigned long mul16(unsigned a, unsigned b);

/* ---- Ring ---------------------------------------------------------------- */
static unsigned       rsel[LM_NSEG], rn;
static unsigned       cseg, coff;       /* Schreibstelle                 */
static unsigned long  total;            /* erzeugte Bytes des Stroms     */
static unsigned       uf;               /* noch nicht ausgegeben         */
static int            bad, werr;
static unsigned char  ob[2048], bb[276];

int lm_ring(unsigned long want)
{
    unsigned s;
    lm_ring_free();
    while (rn < LM_NSEG && ((unsigned long)rn << 16) < want) {
        s = DosAllocSeg(0);
        if (FAPI_FAILED(s) || !s)
            break;
        rsel[rn++] = s;
    }
    if (!rn)
        return LM_EMEM;
    lm_rsize = (unsigned long)rn << 16;
    return 0;
}

void lm_ring_free(void)
{
    while (rn)
        DosFreeSeg(rsel[--rn]);
    lm_rsize = 0;
}

static void flush(void)
{
    unsigned s, o, n, i;
    unsigned char __far *p;

    s = cseg;                           /* Anfang = Schreibstelle - uf   */
    o = coff - uf;
    if (uf > coff)
        s = (s ? s : rn) - 1;
    while (uf && !werr) {
        n = uf > sizeof(ob) ? sizeof(ob) : uf;
        if (o && (unsigned)(0 - o) < n)
            n = 0 - o;                  /* bis zum Segmentende           */
        p = MK_FP(rsel[s], o);
        for (i = 0; i < n; i++)
            ob[i] = p[i];
        if (lm_write(ob, n))
            werr = 1;
        uf -= n;
        o += n;
        if (o == 0 && ++s == rn)
            s = 0;
    }
}

static void put(unsigned char c)
{
    *(unsigned char __far *)MK_FP(rsel[cseg], coff) = c;
    if (++coff == 0 && ++cseg == rn)
        cseg = 0;
    total++;
    uf++;
}

/* Byte d Stellen zurueck (d >= 1) */
static unsigned char get(unsigned long d)
{
    unsigned s, o;
    if (d > total) {
        bad = 1;
        return 0;
    }
    if (d > lm_rsize) {
        if (!lm_back || lm_back(total - d, bb, 1)) {
            bad = 1;
            return 0;
        }
        return bb[0];
    }
    o = coff - (unsigned)d;
    s = cseg - (unsigned)(d >> 16) - ((unsigned)d > coff);
    if ((int)s < 0)
        s += rn;
    return *(unsigned char __far *)MK_FP(rsel[s], o);
}

static void copy(unsigned long d, unsigned len)
{
    unsigned s, o, n, i;
    unsigned char __far *p, __far *q;

    if (d > total) {
        bad = 1;
        return;
    }
    if (d > lm_rsize) {                 /* aus der Ausgabe nachlesen     */
        if (!lm_back || lm_back(total - d, bb, len)) {
            bad = 1;
            return;
        }
        for (i = 0; i < len; i++)
            put(bb[i]);
        return;
    }
    o = coff - (unsigned)d;
    s = cseg - (unsigned)(d >> 16) - ((unsigned)d > coff);
    if ((int)s < 0)
        s += rn;
    while (len) {
        n = len;
        if (o && (unsigned)(0 - o) < n)
            n = 0 - o;
        if (coff && (unsigned)(0 - coff) < n)
            n = 0 - coff;
        p = MK_FP(rsel[s], o);
        q = MK_FP(rsel[cseg], coff);
        for (i = 0; i < n; i++)         /* vorwaerts: Ueberlappung gilt  */
            q[i] = p[i];
        len -= n;
        total += n;
        uf += n;
        o += n;
        if (o == 0 && ++s == rn)
            s = 0;
        coff += n;
        if (coff == 0 && ++cseg == rn)
            cseg = 0;
    }
}

/* ---- Eingabe und Bereichsdecoder ----------------------------------------- */
static unsigned char  ib[4096];
static unsigned       ip, il, over;
static unsigned long  icnt;             /* gelesene Bytes (LZMA2-Stueck) */
static unsigned long  range, code;

static unsigned in_byte(void)
{
    if (ip == il) {
        ip = 0;
        il = lm_read(ib, sizeof(ib));
        if (il == 0) {
            over++;
            return 0;
        }
    }
    icnt++;
    return ib[ip++];
}

static void rc_init(void)
{
    unsigned i;
    if (in_byte() != 0)
        bad = 1;
    code = 0;
    for (i = 0; i < 4; i++)
        code = (code << 8) | in_byte();
    range = 0xFFFFFFFFUL;
    if (code == range)
        bad = 1;
}

static unsigned bit(unsigned short *p)
{
    unsigned long r11 = range >> 11, bound;
    unsigned pr = *p;

    bound = mul16((unsigned)r11, pr) + ((unsigned long)((unsigned)(r11 >> 16) * pr) << 16);
    if (code < bound) {
        range = bound;
        *p = pr + ((2048 - pr) >> 5);
        if (range < 0x1000000UL) {
            range <<= 8;
            code = (code << 8) | in_byte();
        }
        return 0;
    }
    range -= bound;
    code -= bound;
    *p = pr - (pr >> 5);
    if (range < 0x1000000UL) {
        range <<= 8;
        code = (code << 8) | in_byte();
    }
    return 1;
}

static unsigned long direct(unsigned n)
{
    unsigned long r = 0;
    while (n--) {
        range >>= 1;
        r <<= 1;
        if (code >= range) {
            code -= range;
            r |= 1;
        }
        if (range < 0x1000000UL) {
            range <<= 8;
            code = (code << 8) | in_byte();
        }
    }
    return r;
}

static unsigned tree(unsigned short *p, unsigned n)
{
    unsigned m = 1, i;
    for (i = 0; i < n; i++)
        m = (m << 1) | bit(p + m);
    return m - (1u << n);
}

static unsigned rtree(unsigned short *p, unsigned n)
{
    unsigned m = 1, i, s = 0, b;
    for (i = 0; i < n; i++) {
        b = bit(p + m);
        m = (m << 1) | b;
        s |= b << i;
    }
    return s;
}

/* ---- LZMA ---------------------------------------------------------------- */
#define NPROB (1846 + (0x300u << 4))
static unsigned short pr[NPROB];
#define P_ISMATCH  0                    /* 192                           */
#define P_ISREP    192
#define P_ISREPG0  204
#define P_ISREPG1  216
#define P_ISREPG2  228
#define P_ISREP0L  240                  /* 192                           */
#define P_SLOT     432                  /* 4 x 64                        */
#define P_SPEC     688                  /* 115                           */
#define P_ALIGN    803                  /* 16                            */
#define P_LEN      819                  /* 514                           */
#define P_RLEN     1333                 /* 514                           */
#define P_LIT      1847

static unsigned lc, lp, pb, state;
static unsigned long rep0, rep1, rep2, rep3;

static void lm_reset(void)
{
    unsigned i, n = P_LIT + (0x300u << (lc + lp));
    for (i = 0; i < n; i++)
        pr[i] = 1024;
    state = 0;
    rep0 = rep1 = rep2 = rep3 = 0;
}

static unsigned len_dec(unsigned base, unsigned ps)
{
    unsigned short *p = pr + base;
    if (!bit(p))
        return tree(p + 2 + ps * 8, 3);
    if (!bit(p + 1))
        return 8 + tree(p + 2 + 128 + ps * 8, 3);
    return 16 + tree(p + 2 + 256, 8);
}

static int props(unsigned d)
{
    if (d >= 9 * 5 * 5)
        return LM_EDATA;
    lc = d % 9;
    d /= 9;
    lp = d % 5;
    pb = d / 5;
    return lc + lp > 4 ? LM_EPROP : 0;
}

/* size Bytes erzeugen (LZMA: Endmarke erlaubt) */
static int lzma_run(unsigned long size, int lzma2)
{
    unsigned ps, sym, mb, ls, slot, nd, i;
    unsigned prev;
    unsigned long dist;
    unsigned short *lit;

    while (size && !bad && !werr) {
        ps = (unsigned)total & ((1u << pb) - 1);
        if (!bit(pr + P_ISMATCH + (state << 4) + ps)) {
            prev = total ? get(1) : 0;
            lit = pr + P_LIT + 0x300u * ((((unsigned)total & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc)));
            sym = 1;
            if (state >= 7) {
                mb = get(rep0 + 1);
                do {
                    i = (mb >> 7) & 1;
                    mb <<= 1;
                    sym = (sym << 1) | bit(lit + ((1 + i) << 8) + sym);
                    if (i != (sym & 1)) {
                        while (sym < 0x100)
                            sym = (sym << 1) | bit(lit + sym);
                        break;
                    }
                } while (sym < 0x100);
            } else
                while (sym < 0x100)
                    sym = (sym << 1) | bit(lit + sym);
            put((unsigned char)sym);
            size--;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
        } else {
            if (bit(pr + P_ISREP + state)) {
                if (!total) {
                    bad = 1;
                    break;
                }
                if (!bit(pr + P_ISREPG0 + state)) {
                    if (!bit(pr + P_ISREP0L + (state << 4) + ps)) {
                        state = state < 7 ? 9 : 11;
                        put(get(rep0 + 1));
                        size--;
                        goto next;
                    }
                } else {
                    if (!bit(pr + P_ISREPG1 + state))
                        dist = rep1;
                    else {
                        if (!bit(pr + P_ISREPG2 + state))
                            dist = rep2;
                        else {
                            dist = rep3;
                            rep3 = rep2;
                        }
                        rep2 = rep1;
                    }
                    rep1 = rep0;
                    rep0 = dist;
                }
                ls = len_dec(P_RLEN, ps);
                state = state < 7 ? 8 : 11;
            } else {
                rep3 = rep2;
                rep2 = rep1;
                rep1 = rep0;
                ls = len_dec(P_LEN, ps);
                state = state < 7 ? 7 : 10;
                slot = tree(pr + P_SLOT + (ls < 3 ? ls : 3) * 64, 6);
                if (slot < 4)
                    rep0 = slot;
                else {
                    nd = (slot >> 1) - 1;
                    dist = (unsigned long)(2 | (slot & 1)) << nd;
                    if (slot < 14)
                        dist += rtree(pr + P_SPEC + (unsigned)dist - slot, nd);
                    else {
                        dist += direct(nd - 4) << 4;
                        dist += rtree(pr + P_ALIGN, 4);
                    }
                    rep0 = dist;
                    if (rep0 == 0xFFFFFFFFUL) {     /* Endmarke          */
                        if (lzma2)
                            bad = 1;
                        return 0;
                    }
                }
            }
            ls += 2;
            if (ls > size)
                ls = (unsigned)size;
            copy(rep0 + 1, ls);
            size -= ls;
        }
next:
        if (uf >= sizeof(ob))
            flush();
        if (over > 8)
            return LM_EEOF;
    }
    return 0;
}

static int lzma2_run(unsigned long size, unsigned char dprop)
{
    unsigned c, mode, n;
    unsigned long us, ps;
    int rc, need = 1;                   /* Eigenschaften fehlen noch     */

    if (dprop > 40)
        return LM_EDATA;
    for (;;) {
        c = in_byte();
        if (over)
            return LM_EEOF;
        if (c == 0)
            break;
        if (c == 1 || c == 2) {         /* ungepackt                     */
            n = in_byte() << 8;
            n |= in_byte();
            us = (unsigned long)n + 1;
            if (us > size)
                return LM_EDATA;
            size -= us;
            while (us--) {
                put((unsigned char)in_byte());
                if (uf >= sizeof(ob))
                    flush();
            }
            if (over)
                return LM_EEOF;
            continue;
        }
        if (c < 0x80)
            return LM_EDATA;
        mode = (c >> 5) & 3;
        us = ((unsigned long)(c & 0x1F) << 16);
        n = in_byte() << 8;
        n |= in_byte();
        us += (unsigned long)n + 1;
        n = in_byte() << 8;
        n |= in_byte();
        ps = (unsigned long)n + 1;
        if (mode >= 2) {
            rc = props(in_byte());
            if (rc)
                return rc;
            need = 0;
        } else if (need)
            return LM_EDATA;
        if (mode >= 1)
            lm_reset();
        if (us > size)
            return LM_EDATA;
        icnt = 0;
        rc_init();
        rc = lzma_run(us, 1);
        if (rc)
            return rc;
        if (bad || werr)
            break;
        size -= us;
        if (icnt > ps)
            return LM_EDATA;
        while (icnt < ps)               /* bis zum Ende des Stuecks      */
            in_byte();
        if (over)
            return LM_EEOF;
    }
    return size ? LM_EEOF : 0;
}

int lm_decode(int kind, const unsigned char *pp, unsigned np, unsigned long size)
{
    int rc;

    if (!rn)
        return LM_EMEM;
    cseg = coff = 0;
    total = 0;
    uf = 0;
    bad = werr = 0;
    ip = il = over = 0;
    if (kind == LM_LZMA) {
        if (np < 5)
            return LM_EDATA;
        rc = props(pp[0]);
        if (rc)
            return rc;
        lm_reset();
        rc_init();
        rc = lzma_run(size, 0);
    } else if (kind == LM_LZMA2) {
        if (np < 1)
            return LM_EDATA;
        rc = lzma2_run(size, pp[0]);
    } else
        return LM_EDATA;
    if (!werr)
        flush();
    if (werr)
        return LM_EWRITE;
    if (bad)
        return lm_back || total <= lm_rsize ? LM_EDATA : LM_EBACK;
    return rc;
}

/* ---- Filter -------------------------------------------------------------- */
static const unsigned char m_ok[8] = { 1, 1, 1, 0, 1, 0, 0, 0 };
static const unsigned char m_bit[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
#define MSB(b) ((b) == 0 || (b) == 0xFF)

/* BCJ x86 rueckwaerts; liefert die Zahl der fertigen Bytes (Rest < 5 bleibt
 * fuer den naechsten Aufruf). ip = Stromstelle von buf[0]. */
unsigned bcj_x86(unsigned char *buf, unsigned n, unsigned long ip, unsigned *state)
{
    unsigned pos = 0, prevpos = 0xFFFF, mask = *state & 7, lim, idx;
    unsigned char b;
    unsigned long src, dst;

    if (n < 5)
        return 0;
    ip += 5;
    lim = n - 4;
    for (;;) {
        while (pos < lim && (buf[pos] & 0xFE) != 0xE8)
            pos++;
        if (pos >= lim)
            break;
        prevpos = pos - prevpos;
        if (prevpos > 3)
            mask = 0;
        else {
            mask = (mask << (prevpos - 1)) & 7;
            if (mask) {
                b = buf[pos + 4 - m_bit[mask]];
                if (!m_ok[mask] || MSB(b)) {
                    prevpos = pos;
                    mask = ((mask << 1) & 7) | 1;
                    pos++;
                    continue;
                }
            }
        }
        prevpos = pos;
        if (MSB(buf[pos + 4])) {
            src = buf[pos + 1] | ((unsigned long)buf[pos + 2] << 8) |
                  ((unsigned long)buf[pos + 3] << 16) | ((unsigned long)buf[pos + 4] << 24);
            for (;;) {
                dst = src - (ip + pos);
                if (mask == 0)
                    break;
                idx = m_bit[mask];      /* 1..3: Byte 3, 2, 1 von dst     */
                b = (unsigned char)(idx == 1 ? dst >> 16 : idx == 2 ? dst >> 8 : dst);
                if (!MSB(b))
                    break;
                src = dst ^ (idx == 1 ? 0x00FFFFFFUL : idx == 2 ? 0x0000FFFFUL : 0x000000FFUL);
            }
            buf[pos + 4] = (unsigned char)~(((unsigned)(dst >> 24) & 1) - 1);
            buf[pos + 3] = (unsigned char)(dst >> 16);
            buf[pos + 2] = (unsigned char)(dst >> 8);
            buf[pos + 1] = (unsigned char)dst;
            pos += 5;
        } else {
            mask = ((mask << 1) & 7) | 1;
            pos++;
        }
    }
    prevpos = pos - prevpos;
    *state = prevpos > 3 ? 0 : (mask << (prevpos - 1)) & 7;
    return pos;
}

/* Delta rueckwaerts: Ausgabe = Eingabe + Ausgabe dist Stellen zurueck */
void delta_dec(unsigned char *buf, unsigned n, unsigned dist,
               unsigned char *hist, unsigned char *hpos)
{
    unsigned i;
    unsigned char k = *hpos;
    for (i = 0; i < n; i++) {
        buf[i] = (unsigned char)(buf[i] + hist[(unsigned char)(k - dist)]);
        hist[k++] = buf[i];
    }
    *hpos = k;
}

/* CRC-32 (wie ZIP/7z), Tabelle beim ersten Aufruf */
static unsigned long ct32[256];
static char ct32ok;

unsigned long lm_crc32(unsigned long crc, const unsigned char *p, unsigned n)
{
    unsigned i, k;
    unsigned long c;
    if (!ct32ok) {
        for (i = 0; i < 256; i++) {
            c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            ct32[i] = c;
        }
        ct32ok = 1;
    }
    crc = ~crc;
    while (n--)
        crc = ct32[(unsigned char)crc ^ *p++] ^ (crc >> 8);
    return ~crc;
}
