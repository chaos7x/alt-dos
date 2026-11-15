/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Packer der LH5-Familie (lh5, lh6, ARJ 1-3) - eigene Umsetzung. Format
 * wie der Entpacker in lzh.c (Beschreibung von Haruhiko Okumura, ar002,
 * nur gelesen): je Block 16 Bit Symbolzahl, Baum der Codelaengen (NT 19,
 * 5 Bit), Literal/Laenge-Baum (NC 510, 9 Bit), Abstandsbaum (np, pbit),
 * dann die Symbole; Bits hoechstwertiges zuerst, kanonische Codes.
 * Die Suche ist die von flate.c (Hash-Ketten, verzoegerte Suche).
 */
#define FAPI_NO_IMPL
#include <i86.h>
#include "fapi.h"
#include "lzhenc.h"

int (*lze_write)(const unsigned char *p, unsigned n);
unsigned long lze_nout;

#define WSIZE    32768U
#define WMASK    32767U
#define MAXM     256                    /* laengster Treffer             */
#define MINLOOK  (MAXM + 4)
#define MAXDIST  (WSIZE - MINLOOK)
#define HMASK    32767U
#define NSYM     16383U
#define TOOFAR   4096
#define NC       510
#define NT       19

static unsigned wsel, hsel, psel, ssel;
static unsigned char __far *W;
static unsigned short __far *HD, __far *PV, __far *SL, __far *SD;
static unsigned strstart, lookahead, match_start, ins_h, nsym, maxd;
static unsigned good_len, max_lazy, nice_len, max_chain;
static unsigned np, pbit;
static int werr;

static unsigned short cfreq[NC], tfreq[NT], pfreq[18];
static unsigned char  clen[NC], ptl[NT];
static unsigned short ccode[NC], ptc[NT];

/* ---- Ausgabe ------------------------------------------------------------- */
static unsigned char ob[2048];
static unsigned opos, acc, accn;

static void put_byte(unsigned c)
{
    ob[opos++] = (unsigned char)c;
    if (opos == sizeof(ob)) {
        if (lze_write(ob, opos))
            werr = 1;
        lze_nout += opos;
        opos = 0;
    }
}

static void put8(unsigned n, unsigned x)        /* n <= 8              */
{
    acc = (acc << n) | (x & ((1u << n) - 1));
    accn += n;
    if (accn >= 8) {
        accn -= 8;
        put_byte(acc >> accn);
        acc &= (1u << accn) - 1;
    }
}

static void putbits(unsigned n, unsigned x)     /* n <= 16             */
{
    if (n > 8) {
        put8(n - 8, x >> 8);
        put8(8, x);
    } else if (n)
        put8(n, x);
}

/* ---- Huffman (wie flate.c, Codes nicht umgedreht) ------------------------ */
static unsigned short hf[2 * NC], hp[2 * NC], heap[NC], tf[NC];

static void sift(unsigned n, unsigned i)
{
    unsigned c, v = heap[i];
    for (; (c = 2 * i + 1) < n; i = c) {
        if (c + 1 < n && hf[heap[c + 1]] < hf[heap[c]])
            c++;
        if (hf[v] <= hf[heap[c]])
            break;
        heap[i] = heap[c];
    }
    heap[i] = v;
}

/* Laengen bis 16 Bit; liefert das einzige Symbol (+1) oder 0 */
static unsigned mklen(const unsigned short *freq, unsigned n, unsigned char *len)
{
    unsigned i, k, m, a, b, node, d, mx, nz;

    for (i = 0; i < n; i++)
        tf[i] = freq[i];
    for (;;) {
        for (i = 0, nz = 0; i < n; i++) {
            len[i] = 0;
            if (tf[i]) {
                hf[i] = tf[i];
                heap[nz++] = (unsigned short)i;
            }
        }
        if (nz == 0)
            return 1;                   /* Symbol 0                      */
        if (nz == 1)
            return heap[0] + 1;
        m = nz;
        for (i = m / 2; i-- > 0;)
            sift(m, i);
        node = n;
        while (m > 1) {
            a = heap[0];
            heap[0] = heap[--m];
            sift(m, 0);
            b = heap[0];
            hf[node] = hf[a] + hf[b];
            hp[a] = hp[b] = (unsigned short)node;
            heap[0] = (unsigned short)node;
            sift(m, 0);
            node++;
        }
        hp[node - 1] = 0xFFFF;
        mx = 0;
        for (i = 0; i < n; i++) {
            if (!tf[i])
                continue;
            for (d = 0, k = i; hp[k] != 0xFFFF; k = hp[k])
                d++;
            len[i] = (unsigned char)d;
            if (d > mx)
                mx = d;
        }
        if (mx <= 16)
            return 0;
        for (i = 0; i < n; i++)
            if (tf[i])
                tf[i] = (tf[i] + 1) >> 1;
    }
}

static void mkcode(const unsigned char *len, unsigned n, unsigned short *code)
{
    unsigned short cnt[18], next[18];
    unsigned i, l;
    for (i = 0; i < 18; i++)
        cnt[i] = 0;
    for (i = 0; i < n; i++)
        cnt[len[i]]++;
    cnt[0] = 0;
    next[1] = 0;
    for (l = 1; l < 17; l++)
        next[l + 1] = (unsigned short)((next[l] + cnt[l]) << 1);
    for (i = 0; i < n; i++)
        if (len[i])
            code[i] = next[len[i]]++;
}

/* ---- Block ---------------------------------------------------------------- */
static void write_pt_len(unsigned n, unsigned nbit, unsigned spec)
{
    unsigned i, k;
    while (n > 0 && ptl[n - 1] == 0)
        n--;
    putbits(nbit, n);
    i = 0;
    while (i < n) {
        k = ptl[i++];
        if (k <= 6)
            putbits(3, k);
        else
            putbits(k - 3, (1u << (k - 3)) - 2);
        if (i == spec) {
            while (i < 6 && ptl[i] == 0)
                i++;
            putbits(2, (i - 3) & 3);
        }
    }
}

static unsigned nclen(void)
{
    unsigned n = NC;
    while (n > 0 && clen[n - 1] == 0)
        n--;
    return n;
}

static void count_t(void)
{
    unsigned i, k, n = nclen(), cnt;
    for (i = 0; i < NT; i++)
        tfreq[i] = 0;
    for (i = 0; i < n;) {
        k = clen[i++];
        if (k == 0) {
            for (cnt = 1; i < n && clen[i] == 0; i++)
                cnt++;
            if (cnt <= 2)
                tfreq[0] += cnt;
            else if (cnt <= 18)
                tfreq[1]++;
            else if (cnt == 19) {
                tfreq[0]++;
                tfreq[1]++;
            } else
                tfreq[2]++;
        } else
            tfreq[k + 2]++;
    }
}

static void write_c_len(void)
{
    unsigned i, k, n = nclen(), cnt;
    putbits(9, n);
    for (i = 0; i < n;) {
        k = clen[i++];
        if (k == 0) {
            for (cnt = 1; i < n && clen[i] == 0; i++)
                cnt++;
            if (cnt <= 2)
                while (cnt--)
                    putbits(ptl[0], ptc[0]);
            else if (cnt <= 18) {
                putbits(ptl[1], ptc[1]);
                putbits(4, cnt - 3);
            } else if (cnt == 19) {
                putbits(ptl[0], ptc[0]);
                putbits(ptl[1], ptc[1]);
                putbits(4, 15);
            } else {
                putbits(ptl[2], ptc[2]);
                putbits(9, cnt - 20);
            }
        } else
            putbits(ptl[k + 2], ptc[k + 2]);
    }
}

static unsigned pcode(unsigned p)               /* Bitlaenge von p       */
{
    unsigned c = 0;
    while (p) {
        c++;
        p >>= 1;
    }
    return c;
}

static void send_block(void)
{
    unsigned i, r, v, d, c;
    unsigned char plen[18];
    unsigned short pc[18];

    if (!nsym)
        return;
    putbits(16, nsym);
    r = mklen(cfreq, NC, clen);
    if (!r) {
        mkcode(clen, NC, ccode);
        count_t();
        r = mklen(tfreq, NT, ptl);
        if (!r) {
            mkcode(ptl, NT, ptc);
            write_pt_len(NT, 5, 3);
        } else {
            putbits(5, 0);
            putbits(5, r - 1);
            for (i = 0; i < NT; i++)
                ptl[i] = 0;
        }
        write_c_len();
    } else {
        for (i = 0; i < NC; i++)
            clen[i] = 0;
        putbits(5, 0);
        putbits(5, 0);
        putbits(9, 0);
        putbits(9, r - 1);
    }
    r = mklen(pfreq, np, ptl);
    if (!r) {
        mkcode(ptl, np, ptc);
        write_pt_len(np, pbit, 0xFFFF);
    } else {
        putbits(pbit, 0);
        putbits(pbit, r - 1);
        for (i = 0; i < np; i++)
            ptl[i] = 0;
    }
    for (i = 0; i < np; i++) {
        plen[i] = ptl[i];
        pc[i] = ptc[i];
    }
    for (i = 0; i < nsym; i++) {
        v = SL[i];
        d = SD[i];
        if (!d)
            putbits(clen[v], ccode[v]);
        else {
            putbits(clen[v + 253], ccode[v + 253]);
            d--;
            c = pcode(d);
            putbits(plen[c], pc[c]);
            if (c > 1)
                putbits(c - 1, d & (0xFFFFu >> (17 - c)));
        }
    }
    nsym = 0;
    for (i = 0; i < NC; i++)
        cfreq[i] = 0;
    for (i = 0; i < 18; i++)
        pfreq[i] = 0;
}

static void tally_lit(unsigned char c)
{
    SL[nsym] = c;
    SD[nsym++] = 0;
    cfreq[c]++;
    if (nsym >= NSYM)
        send_block();
}

static void tally_dist(unsigned dist, unsigned len)
{
    SL[nsym] = len;
    SD[nsym++] = dist;
    cfreq[len + 253]++;
    pfreq[pcode(dist - 1)]++;
    if (nsym >= NSYM)
        send_block();
}

/* ---- Suche (wie flate.c) ------------------------------------------------- */
static void slide(void)
{
    unsigned i, m;
    if (strstart < WSIZE + MAXDIST)
        return;
    for (i = 0; i < WSIZE; i++)
        W[i] = W[i + WSIZE];
    if (match_start >= WSIZE)
        match_start -= WSIZE;
    else
        match_start = 0;
    strstart -= WSIZE;
    for (i = 0; i <= HMASK; i++) {
        m = HD[i];
        HD[i] = m >= WSIZE ? m - WSIZE : 0;
        m = PV[i];
        PV[i] = m >= WSIZE ? m - WSIZE : 0;
    }
}

static unsigned insert(unsigned s)
{
    unsigned h;
    ins_h = ((ins_h << 5) ^ W[s + 2]) & HMASK;
    h = HD[ins_h];
    PV[s & WMASK] = (unsigned short)h;
    HD[ins_h] = (unsigned short)s;
    return h;
}

static unsigned prev_length, prev_match, match_length;
static int avail;

static unsigned longest(unsigned cur)
{
    unsigned chain = max_chain, best = prev_length, len, limit, nice = nice_len;
    unsigned char __far *scan = W + strstart;
    unsigned char __far *m;
    unsigned char e1, e0;

    if (prev_length >= good_len)
        chain >>= 2;
    if (nice > lookahead)
        nice = lookahead;
    limit = strstart > maxd ? strstart - maxd : 0;
    e0 = scan[best - 1];
    e1 = scan[best];
    do {
        m = W + cur;
        if (m[best] != e1 || m[best - 1] != e0 || m[0] != scan[0] || m[1] != scan[1])
            continue;
        for (len = 2; len < MAXM && m[len] == scan[len]; len++)
            ;
        if (len > best) {
            match_start = cur;
            best = len;
            if (len >= nice)
                break;
            e0 = scan[best - 1];
            e1 = scan[best];
        }
    } while ((cur = PV[cur & WMASK]) > limit && --chain);
    return best <= lookahead ? best : lookahead;
}

static void run(int fin)
{
    unsigned hh, max_ins;

    for (;;) {
        if (werr)
            return;
        if (lookahead < MINLOOK && (!fin || !lookahead))
            return;
        hh = 0;
        if (lookahead >= 3)
            hh = insert(strstart);
        prev_length = match_length;
        prev_match = match_start;
        match_length = 2;
        if (hh && prev_length < max_lazy && strstart - hh <= maxd) {
            match_length = longest(hh);
            if (match_length == 3 && strstart - match_start > TOOFAR)
                match_length = 2;
            if (match_length < 3)
                match_length = 2;
        }
        if (prev_length >= 3 && match_length <= prev_length) {
            max_ins = strstart + lookahead - 3;
            tally_dist(strstart - 1 - prev_match, prev_length);
            lookahead -= prev_length - 1;
            prev_length -= 2;
            do {
                if (++strstart <= max_ins)
                    insert(strstart);
            } while (--prev_length);
            avail = 0;
            match_length = 2;
            strstart++;
        } else if (avail) {
            tally_lit(W[strstart - 1]);
            strstart++;
            lookahead--;
        } else {
            avail = 1;
            strstart++;
            lookahead--;
        }
    }
}

/* ---- Einstieg ------------------------------------------------------------ */
static const struct { unsigned char good, lazy; unsigned short nice, chain; } cfg[10] = {
    { 0, 0, 0, 0 },
    { 4, 4, 16, 16 }, { 4, 5, 32, 32 }, { 4, 6, 64, 64 },
    { 4, 8, 32, 64 }, { 8, 16, 64, 128 }, { 8, 16, 128, 128 },
    { 8, 32, 128, 256 }, { 32, 128, 256, 1024 }, { 32, 255, 256, 4096 } };

int lze_open(void)
{
    if (wsel)
        return 0;
    wsel = DosAllocSeg(0);
    hsel = DosAllocSeg(0);
    psel = DosAllocSeg(0);
    ssel = DosAllocSeg(0);
    if (FAPI_FAILED(wsel) || FAPI_FAILED(hsel) || FAPI_FAILED(psel) ||
        FAPI_FAILED(ssel) || !wsel || !hsel || !psel || !ssel) {
        lze_close();
        return 1;
    }
    W = MK_FP(wsel, 0);
    HD = MK_FP(hsel, 0);
    PV = MK_FP(psel, 0);
    SL = MK_FP(ssel, 0);
    SD = MK_FP(ssel, 32768U);
    return 0;
}

void lze_close(void)
{
    if (wsel && !FAPI_FAILED(wsel)) DosFreeSeg(wsel);
    if (hsel && !FAPI_FAILED(hsel)) DosFreeSeg(hsel);
    if (psel && !FAPI_FAILED(psel)) DosFreeSeg(psel);
    if (ssel && !FAPI_FAILED(ssel)) DosFreeSeg(ssel);
    wsel = hsel = psel = ssel = 0;
}

void lze_begin(int meth, int level)
{
    unsigned i;

    if (level < 1 || level > 9)
        level = 6;
    good_len = cfg[level].good;
    max_lazy = cfg[level].lazy;
    nice_len = cfg[level].nice;
    max_chain = cfg[level].chain;
    switch (meth) {
    case LZE_LH5: maxd = 8192; np = 14; pbit = 4; break;
    case LZE_LH7: maxd = MAXDIST; np = 17; pbit = 5; break;
    case LZE_ARJ: maxd = 26112; np = 17; pbit = 5; break;   /* Ring 26.624 */
    default:      maxd = MAXDIST; np = 16; pbit = 5; break;
    }
    for (i = 0; i <= HMASK; i++)
        HD[i] = 0;
    lookahead = match_start = 0;
    nsym = 0;
    prev_length = match_length = 2;
    avail = 0;
    for (i = 0; i < NC; i++)
        cfreq[i] = 0;
    for (i = 0; i < 18; i++)
        pfreq[i] = 0;
    opos = acc = accn = 0;
    werr = 0;
    lze_nout = 0;
    strstart = 1;
}

int lze_feed(const unsigned char *p, unsigned n)
{
    unsigned k, i;
    while (n && !werr) {
        slide();
        k = 65535U - lookahead - strstart;
        if (k > n)
            k = n;
        for (i = 0; i < k; i++)
            W[strstart + lookahead + i] = p[i];
        lookahead += k;
        p += k;
        n -= k;
        if (lookahead >= 2)
            ins_h = (((unsigned)W[strstart] << 5) ^ W[strstart + 1]) & HMASK;
        run(0);
    }
    return werr;
}

int lze_end(int zoo)
{
    slide();
    run(1);
    if (avail)
        tally_lit(W[strstart - 1]);
    send_block();
    if (zoo) {
        putbits(7, 0);
        putbits(16, 0);
    }
    if (accn)                           /* letztes Byte auffuellen       */
        put8(8 - accn, 0);
    if (opos && !werr) {
        if (lze_write(ob, opos))
            werr = 1;
        lze_nout += opos;
        opos = 0;
    }
    return werr;
}
