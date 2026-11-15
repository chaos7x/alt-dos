/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Entpackkern fuer LHA/LHarc, ARJ und ZOO - eigene Umsetzung nach den
 * Formatbeschreibungen; als Nachschlagewerk gelesen (kein Code
 * uebernommen): Haruhiko Okumura, "ar002" und LZHUF (Beschreibung des
 * statischen bzw. adaptiven Huffman-Verfahrens), Robert Jung, ARJ
 * TECHNOTE (Methode 4), Rahul Dhesi, Zoo 2.10 lzd.c (gemeinfrei, 13-Bit-
 * LZW) und die Beschreibung von lzs/lz5 aus LHa fuer UNIX.
 *
 * Alle Verfahren schreiben in einen Ring von 64 KB (eigenes Segment):
 * die 16-Bit-Stellen laufen von selbst um, Abstaende bis 65535 passen.
 * Vor Stelle 0 steht je nach Verfahren Vorbelegung (Leerzeichen bzw. das
 * Muster von lz5), weil LHarc und LArc auch darauf verweisen.
 */
#define FAPI_NO_IMPL
#include <i86.h>
#include "fapi.h"
#include "lzh.h"

unsigned (*lz_read)(unsigned char *buf, unsigned n);
int      (*lz_write)(const unsigned char *p, unsigned n);

static unsigned       wsel, lsel;
static unsigned char  __far *win;       /* Ring 64 KB                    */
static unsigned       wp, fp;           /* Schreibstelle, ausgegeben bis */
static unsigned long  left;             /* noch zu erzeugende Bytes      */
static int            bad, werr;

/* ---- Eingabe ------------------------------------------------------------- */
static unsigned char ib[2048];
static unsigned ip, il, over;           /* over: Bytes hinter dem Ende   */

static unsigned get_byte(void)
{
    if (ip == il) {
        ip = 0;
        il = lz_read(ib, sizeof(ib));
        if (il == 0) {
            over++;
            return 0;
        }
    }
    return ib[ip++];
}

/* ---- Ausgabe ------------------------------------------------------------- */
static unsigned char ob[2048];

static void flush(void)
{
    unsigned n, i;
    while (fp != wp && !werr) {
        n = wp - fp;
        if (n > sizeof(ob))
            n = sizeof(ob);
        for (i = 0; i < n; i++)
            ob[i] = win[(unsigned)(fp + i)];
        fp += n;
        if (lz_write(ob, n))
            werr = 1;
    }
}

#define PUT(c)  (win[wp++] = (unsigned char)(c))
#define CHECK() do { if ((unsigned)(wp - fp) >= 2048) flush(); } while (0)

static void copy(unsigned back, unsigned len)
{
    unsigned s = wp - back;
    if (len > left)
        len = (unsigned)left;
    left -= len;
    while (len--)
        win[wp++] = win[s++];
}

#pragma aux fill_w = "rep stosw" __parm [__es] [__di] [__cx] [__ax] __modify [__di __cx];
void fill_w(unsigned seg, unsigned off, unsigned words, unsigned val);

/* ---- Bits, hoechstwertiges zuerst (LH5-Familie, lh1, ARJ, lzs) ------------ */
static unsigned bitbuf, subbit;
static int      bitcnt;

static void fillbuf(int n)              /* n <= 15                       */
{
    bitbuf <<= n;
    while (n > bitcnt) {
        n -= bitcnt;
        bitbuf |= subbit << n;
        subbit = get_byte();
        bitcnt = 8;
    }
    bitcnt -= n;
    bitbuf |= subbit >> bitcnt;
}

static unsigned getbits(int n)          /* n <= 15                       */
{
    unsigned x;
    if (n == 0)
        return 0;
    x = bitbuf >> (16 - n);
    fillbuf(n);
    return x;
}

static void init_bits(void)
{
    bitbuf = subbit = 0;
    bitcnt = 0;
    fillbuf(8);
    fillbuf(8);
}

/* ---- kanonische Huffman-Tabellen ------------------------------------------ */
#define NC   510                        /* 256 + 256 - 3 + 1 ... + 1     */
#define NT   19
#define NPT  64                         /* auch die 64 von lh1           */
#define T1   627                        /* lh1: Baumknoten               */
#define NCH1 314                        /* lh1: Zeichen                  */

static union {
    struct {
        unsigned short tab[4096];       /* 12 Bit                        */
        unsigned short sym[NC];
    } c;
    struct {
        unsigned short freq[T1 + 1], son[T1], prnt[T1 + NCH1];
    } d;
} u;
#define ctab u.c.tab
#define csym u.c.sym
static unsigned char  clen[NC];
static unsigned short ccnt[17];
static unsigned short ptab[256], psym[NPT], pcnt[17];
static unsigned char  plen[NPT];

static void make_table(unsigned n, const unsigned char *len, unsigned tbits,
                       unsigned short *tab, unsigned short *sym, unsigned short *cnt)
{
    unsigned short off[18];
    unsigned i, l, j, k, s, m, st;
    unsigned long code;

    for (l = 0; l <= 16; l++)
        cnt[l] = 0;
    for (i = 0; i < n; i++) {
        if (len[i] > 16) {
            bad = 1;
            return;
        }
        cnt[len[i]]++;
    }
    cnt[0] = 0;
    off[1] = 0;
    for (l = 1; l <= 16; l++)
        off[l + 1] = off[l] + cnt[l];
    for (i = 0; i < n; i++)
        if (len[i])
            sym[off[len[i]]++] = (unsigned short)i;
    for (i = 0; i < (1u << tbits); i++)
        tab[i] = 0xFFFF;
    code = 0;
    k = 0;
    for (l = 1; l <= 16; l++) {
        if (code + cnt[l] > (1UL << l)) {
            bad = 1;                    /* ueberbelegt                   */
            return;
        }
        for (j = 0; j < cnt[l]; j++, code++) {
            s = sym[k++];
            if (l <= tbits) {
                m = 1u << (tbits - l);
                st = (unsigned)code << (tbits - l);
                while (m--)
                    tab[st++] = (unsigned short)s;
            }
        }
        code <<= 1;
    }
}

static unsigned hdec(const unsigned short *tab, unsigned tbits, const unsigned char *len,
                     const unsigned short *sym, const unsigned short *cnt)
{
    unsigned s = tab[bitbuf >> (16 - tbits)];
    unsigned code, first, idx, b, l;

    if (s != 0xFFFF) {
        fillbuf(len[s]);
        return s;
    }
    code = first = idx = 0;
    b = bitbuf;
    for (l = 1; l <= 16; l++) {
        code |= b >> 15;
        b <<= 1;
        if (code - first < cnt[l]) {
            if (l > 15) {
                fillbuf(8);
                fillbuf(l - 8);
            } else
                fillbuf(l);
            return sym[idx + code - first];
        }
        idx += cnt[l];
        first += cnt[l];
        first <<= 1;
        code <<= 1;
    }
    bad = 1;
    return 0;
}

static void read_pt_len(unsigned nn, int nbit, unsigned ispec)
{
    unsigned n, i, c, mask;

    n = getbits(nbit);
    if (n == 0) {
        c = getbits(nbit);
        if (c >= nn) {
            bad = 1;
            return;
        }
        for (i = 0; i < nn; i++)
            plen[i] = 0;
        for (i = 0; i < 256; i++)
            ptab[i] = (unsigned short)c;
        return;
    }
    if (n > nn) {
        bad = 1;
        return;
    }
    i = 0;
    while (i < n) {
        c = bitbuf >> 13;
        if (c == 7) {
            for (mask = 1u << 12; mask & bitbuf; mask >>= 1)
                c++;
            if (c > 16) {
                bad = 1;
                return;
            }
        }
        fillbuf(c < 7 ? 3 : (int)c - 3);
        plen[i++] = (unsigned char)c;
        if (i == ispec) {
            c = getbits(2);
            while (c-- && i < nn)
                plen[i++] = 0;
        }
    }
    while (i < nn)
        plen[i++] = 0;
    make_table(nn, plen, 8, ptab, psym, pcnt);
}

static void read_c_len(void)
{
    unsigned n, i, c;

    n = getbits(9);
    if (n == 0) {
        c = getbits(9);
        if (c >= NC) {
            bad = 1;
            return;
        }
        for (i = 0; i < NC; i++)
            clen[i] = 0;
        for (i = 0; i < 4096; i++)
            ctab[i] = (unsigned short)c;
        return;
    }
    if (n > NC) {
        bad = 1;
        return;
    }
    i = 0;
    while (i < n && !bad) {
        c = hdec(ptab, 8, plen, psym, pcnt);
        if (c <= 2) {
            if (c == 0)
                c = 1;
            else if (c == 1)
                c = getbits(4) + 3;
            else
                c = getbits(9) + 20;
            while (c--) {
                if (i >= NC) {
                    bad = 1;
                    return;
                }
                clen[i++] = 0;
            }
        } else
            clen[i++] = (unsigned char)(c - 2);
    }
    while (i < NC)
        clen[i++] = 0;
    make_table(NC, clen, 12, ctab, csym, ccnt);
}

/* LH5-Familie und ARJ 1-3 */
static int lh_static(unsigned np, int pbit)
{
    unsigned blocksize = 0, c, j;

    while (left && !bad && !werr) {
        if (blocksize == 0) {
            blocksize = getbits(8) << 8;
            blocksize |= getbits(8);
            if (blocksize == 0)         /* Ende (ZOO)                    */
                break;
            read_pt_len(NT, 5, 3);
            if (bad)
                break;
            read_c_len();
            if (bad)
                break;
            read_pt_len(np, pbit, 0xFFFF);
            if (bad)
                break;
        }
        blocksize--;
        c = hdec(ctab, 12, clen, csym, ccnt);
        if (c < 256) {
            PUT(c);
            left--;
        } else {
            j = hdec(ptab, 8, plen, psym, pcnt);
            if (j) {
                j--;
                j = (1u << j) + getbits(j);
            }
            copy(j + 1, c - 253);
        }
        CHECK();
        if (over > 4)
            return LZ_EEOF;
    }
    return 0;
}

/* ---- lh1: adaptiver Huffman wie LZHUF ------------------------------------- */
#define FREQ u.d.freq
#define SON  u.d.son
#define PRNT u.d.prnt
#define R1   (T1 - 1)

static void dyn_start(void)
{
    unsigned i, j;
    for (i = 0; i < NCH1; i++) {
        FREQ[i] = 1;
        SON[i] = (unsigned short)(i + T1);
        PRNT[i + T1] = (unsigned short)i;
    }
    for (i = 0, j = NCH1; j <= R1; i += 2, j++) {
        FREQ[j] = FREQ[i] + FREQ[i + 1];
        SON[j] = (unsigned short)i;
        PRNT[i] = PRNT[i + 1] = (unsigned short)j;
    }
    FREQ[T1] = 0xFFFF;
    PRNT[R1] = 0;
}

static void dyn_reconst(void)
{
    unsigned i, j, k, f, l;

    for (i = j = 0; i < T1; i++)        /* Blaetter sammeln, halbieren   */
        if (SON[i] >= T1) {
            FREQ[j] = (FREQ[i] + 1) / 2;
            SON[j] = SON[i];
            j++;
        }
    for (i = 0, j = NCH1; j < T1; i += 2, j++) {
        f = FREQ[i] + FREQ[i + 1];
        for (k = j - 1; f < FREQ[k]; k--)
            ;
        k++;
        for (l = j; l > k; l--) {       /* Platz bei k                   */
            FREQ[l] = FREQ[l - 1];
            SON[l] = SON[l - 1];
        }
        FREQ[k] = (unsigned short)f;
        SON[k] = (unsigned short)i;
    }
    for (i = 0; i < T1; i++) {
        k = SON[i];
        if (k >= T1)
            PRNT[k] = (unsigned short)i;
        else
            PRNT[k] = PRNT[k + 1] = (unsigned short)i;
    }
}

static void dyn_update(unsigned c)
{
    unsigned i, j, k, l;

    if (FREQ[R1] == 0x8000)
        dyn_reconst();
    c = PRNT[c + T1];
    do {
        k = ++FREQ[c];
        l = c + 1;
        if (k > FREQ[l]) {
            while (k > FREQ[++l])
                ;
            l--;
            FREQ[c] = FREQ[l];
            FREQ[l] = (unsigned short)k;
            i = SON[c];
            PRNT[i] = (unsigned short)l;
            if (i < T1)
                PRNT[i + 1] = (unsigned short)l;
            j = SON[l];
            SON[l] = (unsigned short)i;
            PRNT[j] = (unsigned short)c;
            if (j < T1)
                PRNT[j + 1] = (unsigned short)c;
            SON[c] = (unsigned short)j;
            c = l;
        }
    } while ((c = PRNT[c]) != 0);
}

static int lh1(void)
{
    unsigned i, j, c, n;
    static const unsigned char grp[6] = { 1, 3, 8, 12, 24, 16 };

    for (i = j = 0; i < 6; i++)         /* feste Laengen der oberen 6 Bit */
        for (n = 0; n < grp[i]; n++)
            plen[j++] = (unsigned char)(3 + i);
    make_table(64, plen, 8, ptab, psym, pcnt);
    dyn_start();
    while (left && !bad && !werr) {
        c = SON[R1];
        while (c < T1) {
            c += bitbuf >> 15;
            fillbuf(1);
            c = SON[c];
        }
        c -= T1;
        dyn_update(c);
        if (c < 256) {
            PUT(c);
            left--;
        } else {
            j = hdec(ptab, 8, plen, psym, pcnt);
            j = (j << 6) | getbits(6);
            copy(j + 1, c - 253);
        }
        CHECK();
        if (over > 4)
            return LZ_EEOF;
    }
    return 0;
}

/* ---- ARJ Methode 4 -------------------------------------------------------- */
static int arj4(void)
{
    unsigned c, w, plus, pw;

    while (left && !bad && !werr) {
        for (c = 0, pw = 1, w = 0; w < 7; w++) {
            if (!getbits(1))
                break;
            c += pw;
            pw <<= 1;
        }
        c += getbits(w);
        if (c == 0) {
            PUT(getbits(8));
            left--;
        } else {
            for (plus = 0, pw = 512, w = 9; w < 13; w++) {
                if (!getbits(1))
                    break;
                plus += pw;
                pw <<= 1;
            }
            copy(getbits(w) + plus + 1, c + 2);
        }
        CHECK();
        if (over > 4)
            return LZ_EEOF;
    }
    return 0;
}

/* ---- LArc lzs (2 KB) und lz5 (4 KB): absolute Ringstellen wie LArc 3.33,
 * Schreibstelle beginnt 17 bzw. 18 vor dem Ringende -------------------------- */
static int larc(int five)
{
    unsigned mask = five ? 0xFFF : 0x7FF, flag = 0, fl = 0, c, mp, len, idx;

    while (left && !bad && !werr) {
        if (five) {
            if (fl == 0) {
                flag = getbits(8);
                fl = 8;
            }
            fl--;
            c = getbits(8);
            if (flag & 1) {
                PUT(c);
                left--;
                len = 0;
            } else {
                mp = getbits(8);
                len = (mp & 15) + 3;
                mp = c | ((mp & 0xF0) << 4);
                idx = mp;
            }
            flag >>= 1;
        } else {
            if (getbits(1)) {
                PUT(getbits(8));
                left--;
                len = 0;
            } else {
                mp = getbits(11);
                len = getbits(4) + 2;
                idx = mp;
            }
        }
        if (len)
            copy(((wp - idx - 1) & mask) + 1, len);
        CHECK();
        if (over > 4)
            return LZ_EEOF;
    }
    return 0;
}

static void lz5_fill(void)              /* Vorbelegung wie LArc 3.33     */
{
    unsigned i, j, k = 0;
    for (i = 0; i < 256; i++)
        for (j = 0; j < 13; j++)
            win[k++] = (unsigned char)i;
    for (i = 0; i < 256; i++)
        win[k++] = (unsigned char)i;
    for (i = 0; i < 256; i++)
        win[k++] = (unsigned char)(255 - i);
    for (i = 0; i < 128; i++)
        win[k++] = 0;
    for (i = 0; i < 110; i++)
        win[k++] = ' ';
    for (i = 0; i < 18; i++)
        win[k++] = 0;
}

/* ---- ZOO: LZW 9-13 Bit, niedrigstwertiges Bit zuerst ----------------------- */
#define LZW_N 8200
static unsigned lacc, lcnt, nbits;

static unsigned lzw_code(void)
{
    unsigned b, code, mask = (1u << nbits) - 1;

    if (lcnt < nbits) {
        if (lcnt <= 8) {
            lacc |= get_byte() << lcnt;
            lcnt += 8;
        }
        if (lcnt < nbits) {
            b = get_byte();
            code = (lacc | (b << lcnt)) & mask;
            lacc = b >> (nbits - lcnt);
            lcnt = lcnt + 8 - nbits;
            return code;
        }
    }
    code = lacc & mask;
    lacc >>= nbits;
    lcnt -= nbits;
    return code;
}

static int lzw(void)
{
    unsigned short __far *pre;
    unsigned char __far *ch, __far *stk;
    unsigned code, in, old = 0xFFFF, fin = 0, freec = 258, maxc = 512, sp;

    if (!lsel) {
        lsel = DosAllocSeg(0);
        if (FAPI_FAILED(lsel) || !lsel) {
            lsel = 0;
            return LZ_EMEM;
        }
    }
    pre = MK_FP(lsel, 0);
    ch = MK_FP(lsel, 2 * LZW_N);
    stk = MK_FP(lsel, 3 * LZW_N);
    lacc = lcnt = 0;
    nbits = 9;
    while (left && !bad && !werr) {
        code = lzw_code();
        if (over > 4)
            return LZ_EEOF;
        if (code == 257)
            break;
        if (code == 256 || old == 0xFFFF) {
            if (code == 256) {
                nbits = 9;
                maxc = 512;
                freec = 258;
                code = lzw_code();
                if (code == 257)
                    break;
            }
            if (code > 255) {
                bad = 1;
                break;
            }
            PUT(code);
            left--;
            old = fin = code;
            CHECK();
            continue;
        }
        in = code;
        sp = 0;
        if (code >= freec) {
            if (code > freec) {
                bad = 1;
                break;
            }
            code = old;
            stk[sp++] = (unsigned char)fin;
        }
        while (code > 255) {
            if (sp >= LZW_N || code >= LZW_N) {
                bad = 1;
                return LZ_EDATA;
            }
            stk[sp++] = ch[code];
            code = pre[code];
        }
        fin = code;
        stk[sp++] = (unsigned char)code;
        while (sp && left) {
            PUT(stk[--sp]);
            left--;
        }
        if (freec < LZW_N) {
            pre[freec] = (unsigned short)old;
            ch[freec] = (unsigned char)fin;
            freec++;
            if (freec >= maxc && nbits < 13) {
                nbits++;
                maxc <<= 1;
            }
        }
        old = in;
        CHECK();
    }
    return 0;
}

/* ---- Einstieg ------------------------------------------------------------- */
int lz_open(void)
{
    if (wsel)
        return 0;
    wsel = DosAllocSeg(0);
    if (FAPI_FAILED(wsel) || !wsel) {
        wsel = 0;
        return LZ_EMEM;
    }
    win = MK_FP(wsel, 0);
    return 0;
}

int lz_decode(int meth, unsigned long size)
{
    int rc;
    unsigned pre = 0;

    if (lz_open())
        return LZ_EMEM;
    wp = fp = 0;
    left = size;
    bad = werr = 0;
    ip = il = over = 0;
    switch (meth) {                     /* Vorbelegung: Woerterbuch      */
    case LZ_LH1: case LZ_LH4: pre = 4096; break;
    case LZ_LZS: fill_w(wsel, 0, 1024, 0x2020); wp = fp = 2048 - 17; break;
    case LZ_LZ5: lz5_fill(); wp = fp = 4096 - 18; break;
    case LZ_LH5: pre = 8192; break;
    case LZ_LH6: pre = 32768U; break;
    case LZ_LH7: pre = 0; fill_w(wsel, 0, 32768U, 0x2020); break;
    }
    if (pre)
        fill_w(wsel, (unsigned)(0 - pre), pre / 2, 0x2020);
    if (meth != LZ_LZW)
        init_bits();
    switch (meth) {
    case LZ_LH1:  rc = lh1(); break;
    case LZ_LH4:  rc = lh_static(13, 4); break;
    case LZ_LH5:  rc = lh_static(14, 4); break;
    case LZ_LH6:  rc = lh_static(16, 5); break;
    case LZ_LH7:
    case LZ_ARJ:  rc = lh_static(17, 5); break;
    case LZ_ARJ4: rc = arj4(); break;
    case LZ_LZS:  rc = larc(0); break;
    case LZ_LZ5:  rc = larc(1); break;
    case LZ_LZW:  rc = lzw(); break;
    default:      return LZ_EDATA;
    }
    if (!werr)
        flush();
    if (werr)
        return LZ_EWRITE;
    if (rc)
        return rc;
    if (bad)
        return LZ_EDATA;
    return left ? LZ_EEOF : 0;
}

/* ---- Pruefsummen ---------------------------------------------------------- */
static unsigned short ctab16[256];
static unsigned long  ctab32[256];
static char           c16ok, c32ok;

unsigned crc16(unsigned crc, const unsigned char *p, unsigned n)
{
    unsigned i, k, c;
    if (!c16ok) {
        for (i = 0; i < 256; i++) {
            c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xA001 ^ (c >> 1) : c >> 1;
            ctab16[i] = (unsigned short)c;
        }
        c16ok = 1;
    }
    while (n--)
        crc = ctab16[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

unsigned long crc32a(unsigned long crc, const unsigned char *p, unsigned n)
{
    unsigned i, k;
    unsigned long c;
    if (!c32ok) {
        for (i = 0; i < 256; i++) {
            c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            ctab32[i] = c;
        }
        c32ok = 1;
    }
    crc = ~crc;
    while (n--)
        crc = ctab32[(unsigned char)crc ^ *p++] ^ (crc >> 8);
    return ~crc;
}
