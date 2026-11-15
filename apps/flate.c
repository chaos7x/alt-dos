/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Deflate (RFC 1951) fuer AT-DOS: Entpacken (inflate) und Packen
 * (deflate), CRC-32 (ISO 3309, wie gzip/ZIP). Eigene Umsetzung nach der
 * Spezifikation; der Ablauf des Packers (Hash-Ketten, verzoegerte Suche,
 * Stufen 1-9) folgt dem bekannten Verfahren aus RFC 1951 Abschnitt 4.
 *
 * Speicher: Entpacken braucht nur das 32-KB-Fenster in DGROUP. Packen
 * holt vier Segmente zu 64 KB (Fenster 2 x 32 KB, Kopf und Kette der
 * Hash-Tabelle, Symbolpuffer) - ein 286-Programm hat nur 64 KB Daten.
 * Keine 32-Bit-Multiplikation/-Division (kein C-Laufzeitsystem).
 */
#include <i86.h>
#define FAPI_NO_IMPL
#include "fapi.h"
#include "flate.h"

unsigned long fl_mul(unsigned a, unsigned b);
#pragma aux fl_mul = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];

unsigned (*fl_read)(unsigned char *buf, unsigned n);
int      (*fl_write)(const unsigned char *p, unsigned n);
unsigned long fl_nin, fl_nout;

/* ---- Eingabe ------------------------------------------------------------ */
static unsigned char ib[2048];
static unsigned ipos, ilen;
static unsigned char pb[4], npb;        /* zurueckgegebene Bytes         */

void fl_reset(void)
{
    ipos = ilen = 0;
    npb = 0;
}

int fl_getc(void)
{
    if (npb) {
        fl_nin++;
        return pb[--npb];
    }
    if (ipos >= ilen) {
        ilen = fl_read(ib, sizeof(ib));
        ipos = 0;
        if (!ilen || ilen == 0xFFFF) {
            ilen = 0;
            return -1;
        }
    }
    fl_nin++;
    return ib[ipos++];
}

void fl_ungetc(int c)
{
    if (c >= 0 && npb < sizeof(pb)) {
        pb[npb++] = (unsigned char)c;
        fl_nin--;
    }
}

/* bis n Bytes: erst Zurueckgegebenes und Gepuffertes, dann direkt */
unsigned fl_readbuf(unsigned char *b, unsigned n)
{
    unsigned k = 0;
    while (k < n && npb) {
        b[k++] = pb[--npb];
        fl_nin++;
    }
    while (k < n && ipos < ilen) {
        b[k++] = ib[ipos++];
        fl_nin++;
    }
    if (k < n && k == 0) {
        k = fl_read(b, n);
        if (k == 0xFFFF)
            k = 0;
        fl_nin += k;
    }
    return k;
}

/* ---- CRC-32 ------------------------------------------------------------- */
static unsigned long crctab[256];
static char crc_ok;

unsigned long crc32(unsigned long crc, const unsigned char *p, unsigned n)
{
    unsigned i, k;
    unsigned long c;

    if (!crc_ok) {
        for (i = 0; i < 256; i++) {
            c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            crctab[i] = c;
        }
        crc_ok = 1;
    }
    crc = ~crc;
    while (n--)
        crc = crctab[(unsigned char)crc ^ *p++] ^ (crc >> 8);
    return ~crc;
}

/* ---- gemeinsame Tabellen ------------------------------------------------ */
static const unsigned short lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const unsigned char dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const unsigned char clord[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/* ======================================================================== */
/* Entpacken                                                                */
/* ======================================================================== */
#define WSIZE   32768U
#define WMASK   32767U

static unsigned char win[WSIZE];        /* Fenster = Ausgabepuffer      */
static unsigned wpos;
static unsigned long wtot;              /* bisher ausgegeben             */
static unsigned bb;                     /* Bitpuffer (bis 16 Bit)       */
static unsigned char bc, over;          /* gueltige Bits, erfundene Bytes */
static int ferr;
#define OVR (over * 8 > bc)             /* erfundene Bits verbraucht?    */

static void addb(void)
{
    int c = fl_getc();
    if (c < 0) {
        c = 0;
        over++;
    }
    bb |= (unsigned)c << bc;
    bc += 8;
}

static unsigned bits(unsigned n)        /* n <= 8                        */
{
    unsigned v;
    while (bc < n)
        addb();
    v = bb & ((1u << n) - 1);
    bb >>= n;
    bc -= (unsigned char)n;
    return v;
}

static unsigned bitsn(unsigned n)       /* n <= 16                       */
{
    unsigned lo;
    if (n <= 8)
        return bits(n);
    lo = bits(8);
    return lo | (bits(n - 8) << 8);
}

struct huff {
    unsigned short fast[512];           /* Symbol << 4 | Laenge, 0 = lang */
    unsigned short count[16];
    unsigned short sym[288];
};
static struct huff hl, hd;

/* Tabelle aus Codelaengen; Rueckgabe != 0: ueberbelegt */
static int build(struct huff *h, const unsigned char *len, unsigned n)
{
    static unsigned short offs[16], next[16];
    unsigned s, l, code, k, r, i;
    int left;

    for (l = 0; l < 16; l++)
        h->count[l] = 0;
    for (s = 0; s < n; s++)
        h->count[len[s]]++;
    left = 1;
    for (l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0)
            return -1;
    }
    offs[1] = 0;
    for (l = 1; l < 15; l++)
        offs[l + 1] = offs[l] + h->count[l];
    for (s = 0; s < n; s++)
        if (len[s])
            h->sym[offs[len[s]]++] = (unsigned short)s;
    for (i = 0; i < 512; i++)
        h->fast[i] = 0;
    code = 0;
    h->count[0] = 0;
    for (l = 1; l < 16; l++) {
        code = (code + h->count[l - 1]) << 1;
        next[l] = (unsigned short)code;
    }
    for (s = 0; s < n; s++) {
        l = len[s];
        if (!l || l > 9)
            continue;
        code = next[l]++;
        for (r = 0, k = 0; k < l; k++)  /* Bits umdrehen (LSB zuerst)  */
            r = (r << 1) | ((code >> k) & 1);
        for (k = r; k < 512; k += 1u << l)
            h->fast[k] = (unsigned short)((s << 4) | l);
    }
    return 0;
}

static int decode(struct huff *h)
{
    unsigned e, len, code, first, index, cnt;

    while (bc < 9)
        addb();
    e = h->fast[bb & 511];
    if (e) {
        len = e & 15;
        bb >>= len;
        bc -= (unsigned char)len;
        return (int)(e >> 4);
    }
    code = first = index = 0;           /* langer Code: Bit fuer Bit     */
    for (len = 1; len < 16; len++) {
        code |= bits(1);
        cnt = h->count[len];
        if (code - first < cnt)
            return h->sym[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void out(unsigned char c)
{
    win[wpos++] = c;
    wtot++;
    if (wpos == WSIZE) {
        if (fl_write(win, WSIZE))
            ferr = FL_EWRITE;
        fl_nout += WSIZE;
        wpos = 0;
    }
}

static int codes(void)
{
    int s;
    unsigned len, dist, k, from;

    for (;;) {
        if (ferr)
            return ferr;
        s = decode(&hl);
        if (s < 0)
            return FL_EDATA;
        if (s < 256) {
            out((unsigned char)s);
            continue;
        }
        if (s == 256)
            return OVR ? FL_EEOF : FL_OK;
        s -= 257;
        if (s >= 29)
            return FL_EDATA;
        len = lbase[s] + bits(lext[s]);
        s = decode(&hd);
        if (s < 0 || s >= 30)
            return FL_EDATA;
        dist = dbase[s] + bitsn(dext[s]);
        if (OVR)
            return FL_EEOF;
        if (dist > wtot)
            return FL_EDATA;
        from = (wpos - dist) & WMASK;
        for (k = 0; k < len; k++) {
            out(win[from]);
            from = (from + 1) & WMASK;
        }
    }
}

static void fixed(void)
{
    static unsigned char l[288];
    unsigned s;
    for (s = 0; s < 144; s++) l[s] = 8;
    for (; s < 256; s++) l[s] = 9;
    for (; s < 280; s++) l[s] = 7;
    for (; s < 288; s++) l[s] = 8;
    build(&hl, l, 288);
    for (s = 0; s < 30; s++) l[s] = 5;
    build(&hd, l, 30);
}

static int dynamic(void)
{
    static unsigned char l[320];
    unsigned nlen, ndist, ncode, i, k, rep;
    int s;
    unsigned char prev;

    nlen = bits(5) + 257;
    ndist = bits(5) + 1;
    ncode = bits(4) + 4;
    if (nlen > 286 || ndist > 30)
        return FL_EDATA;
    for (i = 0; i < 19; i++)
        l[clord[i]] = (unsigned char)(i < ncode ? bits(3) : 0);
    if (build(&hl, l, 19))
        return FL_EDATA;
    for (i = 0; i < nlen + ndist;) {
        s = decode(&hl);
        if (s < 0)
            return FL_EDATA;
        if (s < 16) {
            l[i++] = (unsigned char)s;
            continue;
        }
        prev = 0;
        if (s == 16) {
            if (!i)
                return FL_EDATA;
            prev = l[i - 1];
            rep = 3 + bits(2);
        } else if (s == 17)
            rep = 3 + bits(3);
        else
            rep = 11 + bits(7);
        if (i + rep > nlen + ndist)
            return FL_EDATA;
        for (k = 0; k < rep; k++)
            l[i++] = prev;
    }
    if (!l[256])
        return FL_EDATA;
    if (build(&hl, l, nlen) || build(&hd, l + nlen, ndist))
        return FL_EDATA;
    return FL_OK;
}

int inflate(void)
{
    unsigned last, type, n, k;
    int c, rc = FL_OK;

    wpos = 0;
    wtot = 0;
    bb = 0;
    bc = 0;
    over = 0;
    ferr = 0;
    do {
        last = bits(1);
        type = bits(2);
        if (type == 0) {                /* gespeichert                  */
            bb >>= bc & 7;
            bc &= ~7;
            n = bitsn(16);
            k = bitsn(16);
            if ((n ^ k) != 0xFFFFU) {
                rc = FL_EDATA;
                break;
            }
            while (n && bc) {
                out((unsigned char)bits(8));
                n--;
            }
            if (OVR) {
                rc = FL_EEOF;
                break;
            }
            while (n--) {
                if ((c = fl_getc()) < 0) {
                    rc = FL_EEOF;
                    break;
                }
                out((unsigned char)c);
            }
            if (rc)
                break;
        } else if (type == 1) {
            fixed();
            rc = codes();
        } else if (type == 2) {
            rc = dynamic();
            if (!rc)
                rc = codes();
        } else
            rc = FL_EDATA;
        if (!rc && ferr)
            rc = ferr;
    } while (!rc && !last);
    if (!rc && wpos) {
        if (fl_write(win, wpos))
            rc = FL_EWRITE;
        fl_nout += wpos;
    }
    /* angebrochenes Byte verwerfen, ganze Bytes im Bitpuffer zurueckgeben
       (sie gehoeren schon zum Folgenden, z. B. dem gzip-Schluss) */
    bb >>= bc & 7;
    bc &= ~7;
    if (OVR)
        return rc ? rc : FL_EEOF;
    bc -= over * 8;
    k = bc >> 3;
    while (k) {
        k--;
        pb[npb++] = (unsigned char)(bb >> (k * 8));
        fl_nin--;
    }
    return rc;
}

/* ======================================================================== */
/* Packen                                                                   */
/* ======================================================================== */
#define MINLOOK  262                    /* MAX_MATCH + MIN_MATCH + 1    */
#define MAXDIST  (WSIZE - MINLOOK)
#define HMASK    32767U
#define NSYM     16383U                 /* Symbole je Block              */
#define TOOFAR   4096

static unsigned wsel, hsel, psel, ssel;
static unsigned char __far *W;          /* 64 KB: Fenster                */
static unsigned short __far *HD;        /* Kopf je Hashwert              */
static unsigned short __far *PV;        /* Kette je Fensterstelle        */
static unsigned short __far *SL;        /* Symbol: Literal/Laenge        */
static unsigned short __far *SD;        /* Abstand (0 = Literal)         */

static unsigned strstart, lookahead, match_start, block_start, ins_h;
static unsigned nsym, blk_bytes;
static unsigned good_len, max_lazy, nice_len, max_chain;
static unsigned short lfreq[286], dfreq[30];
static unsigned long xbits;             /* Extrabits der Symbole        */
static unsigned char lcode[256];        /* Laenge-3 -> Code 0..28       */
static unsigned char dcode[512];        /* Abstand-1 -> Code            */

/* Puffer des Packers liegen im Fenster des Entpackers (nie gleichzeitig) */
#define ob      win                     /* 2 KB Ausgabe                  */
#define OBSZ    2048
#define fbuf    (win + 2048)            /* 2 KB Eingabe                  */
#define hf      ((unsigned short *)(win + 4096))         /* 572 Worte */
#define hp      ((unsigned short *)(win + 4096 + 1144))
#define heap    ((unsigned short *)(win + 4096 + 2288))  /* 286 Worte */
#define tf      ((unsigned short *)(win + 4096 + 2860))
#define rc      (win + 8192)            /* 320 Byte                      */
#define ra      (win + 8192 + 320)
#define seq     (win + 8192 + 640)
static unsigned opos;
static unsigned obuf;
static unsigned char ovalid;
static int werr;

static const struct { unsigned char good, lazy; unsigned short nice, chain; } cfg[10] = {
    { 0, 0, 0, 0 },
    { 4, 4, 8, 4 }, { 4, 5, 16, 8 }, { 4, 6, 32, 32 },
    { 4, 4, 16, 16 }, { 8, 16, 32, 32 }, { 8, 16, 128, 128 },
    { 8, 32, 128, 256 }, { 32, 128, 258, 1024 }, { 32, 255, 258, 4096 } };

int deflate_open(void)
{
    unsigned i, c, n, d;

    wsel = DosAllocSeg(0);
    hsel = DosAllocSeg(0);
    psel = DosAllocSeg(0);
    ssel = DosAllocSeg(0);
    if (FAPI_FAILED(wsel) || FAPI_FAILED(hsel) || FAPI_FAILED(psel) ||
        FAPI_FAILED(ssel) || !wsel || !hsel || !psel || !ssel) {
        deflate_close();
        return FL_EMEM;
    }
    W = MK_FP(wsel, 0);
    HD = MK_FP(hsel, 0);
    PV = MK_FP(psel, 0);
    SL = MK_FP(ssel, 0);
    SD = MK_FP(ssel, 32768U);
    for (c = 0, n = 0; c < 28; c++)     /* Laenge -> Code               */
        for (i = 0; i < (1u << lext[c]); i++)
            lcode[n++] = (unsigned char)c;
    lcode[255] = 28;
    for (c = 0, d = 0; c < 16; c++)     /* Abstand bis 256              */
        for (i = 0; i < (1u << dext[c]); i++)
            dcode[d++] = (unsigned char)c;
    for (d >>= 7; c < 30; c++)          /* darueber in 128er-Schritten  */
        for (i = 0; i < (1u << (dext[c] - 7)); i++)
            dcode[256 + d++] = (unsigned char)c;
    return FL_OK;
}

void deflate_close(void)
{
    unsigned *s[4];
    unsigned i;
    s[0] = &wsel; s[1] = &hsel; s[2] = &psel; s[3] = &ssel;
    for (i = 0; i < 4; i++) {
        if (*s[i] && !FAPI_FAILED(*s[i]))
            DosFreeSeg(*s[i]);
        *s[i] = 0;
    }
}

/* ---- Bits schreiben ------------------------------------------------------ */
static void put_byte(unsigned char c)
{
    ob[opos++] = c;
    if (opos == OBSZ) {
        if (fl_write(ob, opos))
            werr = FL_EWRITE;
        fl_nout += opos;
        opos = 0;
    }
}

static void send(unsigned v, unsigned n)        /* n <= 16           */
{
    if (ovalid + n >= 16) {
        obuf |= v << ovalid;
        put_byte((unsigned char)obuf);
        put_byte((unsigned char)(obuf >> 8));
        obuf = ovalid ? v >> (16 - ovalid) : 0;
        ovalid = (unsigned char)(ovalid + n - 16);
    } else {
        obuf |= v << ovalid;
        ovalid += (unsigned char)n;
    }
}

static void align(void)
{
    if (ovalid > 8) {
        put_byte((unsigned char)obuf);
        put_byte((unsigned char)(obuf >> 8));
    } else if (ovalid)
        put_byte((unsigned char)obuf);
    obuf = 0;
    ovalid = 0;
}

/* ---- Huffman-Baum mit Laengengrenze ------------------------------------- */

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

/* Laengen aus Haeufigkeiten (n Symbole, hoechstens lim Bit) */
static void mklen(const unsigned short *freq, unsigned n, unsigned lim, unsigned char *len)
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
            return;
        if (nz == 1) {
            len[heap[0]] = 1;
            return;
        }
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
        hp[node - 1] = 0xFFFF;          /* Wurzel                       */
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
        if (mx <= lim)
            return;
        for (i = 0; i < n; i++)         /* zu tief: Haeufigkeiten halbieren */
            if (tf[i])
                tf[i] = (tf[i] + 1) >> 1;
    }
}

/* kanonische Codes, Bits umgedreht (LSB zuerst ausgeben) */
static void mkcode(const unsigned char *len, unsigned n, unsigned short *code)
{
    static unsigned short cnt[16], next[16];
    unsigned i, l, c, r, k;
    for (i = 0; i < 16; i++)
        cnt[i] = 0;
    for (i = 0; i < n; i++)
        cnt[len[i]]++;
    cnt[0] = 0;
    for (c = 0, l = 1; l < 16; l++) {
        c = (c + cnt[l - 1]) << 1;
        next[l] = (unsigned short)c;
    }
    for (i = 0; i < n; i++) {
        l = len[i];
        if (!l)
            continue;
        c = next[l]++;
        for (r = 0, k = 0; k < l; k++)
            r = (r << 1) | ((c >> k) & 1);
        code[i] = (unsigned short)r;
    }
}

static unsigned char ll[286], dl[30], cl[19];
static unsigned short lc[286], dc[30], cc[19], cf[19];

static void emit(const unsigned char *llen, const unsigned short *lcd,
                 const unsigned char *dlen, const unsigned short *dcd)
{
    unsigned i, v, d, c;
    for (i = 0; i < nsym; i++) {
        v = SL[i];
        d = SD[i];
        if (!d) {
            send(lcd[v], llen[v]);
            continue;
        }
        c = lcode[v - 3];
        send(lcd[257 + c], llen[257 + c]);
        if (lext[c])
            send(v - lbase[c], lext[c]);
        c = d <= 256 ? dcode[d - 1] : dcode[256 + ((d - 1) >> 7)];
        send(dcd[c], dlen[c]);
        if (dext[c])
            send(d - dbase[c], dext[c]);
    }
    send(lcd[256], llen[256]);
}

static void flush_block(int last)
{
    static unsigned char fll[288], fdl[30];
    static unsigned short flc[288], fdc[30];
    unsigned long dyn, fix, sto;
    unsigned i, n, hlit, hdist, hclen, rep, k;
    unsigned char cur;

    lfreq[256]++;
    for (i = 0; i < 30 && !dfreq[i]; i++)
        ;
    if (i == 30)
        dfreq[0] = 1;
    mklen(lfreq, 286, 15, ll);
    mklen(dfreq, 30, 15, dl);
    for (hlit = 286; hlit > 257 && !ll[hlit - 1]; hlit--)
        ;
    for (hdist = 30; hdist > 1 && !dl[hdist - 1]; hdist--)
        ;
    for (i = 0; i < hlit; i++)
        seq[i] = ll[i];
    for (i = 0; i < hdist; i++)
        seq[hlit + i] = dl[i];
    n = hlit + hdist;
    /* Laengenfolge lauflaengenkodieren: in seq bleiben Eintraege mit
       Code (Bit 0..4) und Wiederholungszahl im zweiten Feld (cl-Puffer
       wird erst danach benutzt) */
    for (i = 0; i < 19; i++)
        cf[i] = 0;
    {
        unsigned m = 0;
        for (i = 0; i < n;) {
            cur = seq[i];
            for (rep = 1; i + rep < n && seq[i + rep] == cur; rep++)
                ;
            if (cur == 0 && rep >= 3) {
                k = rep > 138 ? 138 : rep;
                rc[m] = (unsigned char)(k >= 11 ? 18 : 17);
                ra[m++] = (unsigned char)k;
                i += k;
            } else if (cur && rep >= 4) {
                rc[m] = cur;
                ra[m++] = 0;
                k = rep - 1 > 6 ? 6 : rep - 1;
                rc[m] = 16;
                ra[m++] = (unsigned char)k;
                i += 1 + k;
            } else {
                rc[m] = cur;
                ra[m++] = 0;
                i++;
            }
        }
        for (i = 0; i < m; i++)
            cf[rc[i]]++;
        mklen(cf, 19, 7, cl);
        mkcode(cl, 19, cc);
        for (hclen = 19; hclen > 4 && !cl[clord[hclen - 1]]; hclen--)
            ;
        /* Kosten */
        dyn = 3 + 14 + 3 * hclen + xbits;
        for (i = 0; i < m; i++)
            dyn += cl[rc[i]] + (rc[i] == 16 ? 2 : rc[i] == 17 ? 3 : rc[i] == 18 ? 7 : 0);
        for (i = 0; i < 286; i++)
            dyn += fl_mul(lfreq[i], ll[i]);
        for (i = 0; i < 30; i++)
            dyn += fl_mul(dfreq[i], dl[i]);
        fix = 3 + xbits;
        for (i = 0; i < 286; i++)
            fix += fl_mul(lfreq[i], i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        for (i = 0; i < 30; i++)
            fix += fl_mul(dfreq[i], 5);
        sto = 3 + 7 + 32 + ((unsigned long)blk_bytes << 3);

        if (sto <= dyn && sto <= fix) {
            send(last, 3);              /* gespeichert (Typ 0)          */
            align();
            put_byte((unsigned char)blk_bytes);
            put_byte((unsigned char)(blk_bytes >> 8));
            put_byte((unsigned char)~blk_bytes);
            put_byte((unsigned char)(~blk_bytes >> 8));
            for (i = 0; i < blk_bytes; i++)
                put_byte(W[block_start + i]);
        } else if (fix <= dyn) {
            for (i = 0; i < 288; i++)
                fll[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
            for (i = 0; i < 30; i++)
                fdl[i] = 5;
            mkcode(fll, 288, flc);
            mkcode(fdl, 30, fdc);
            send(2 | last, 3);
            emit(fll, flc, fdl, fdc);
        } else {
            mkcode(ll, 286, lc);
            mkcode(dl, 30, dc);
            send(4 | last, 3);
            send(hlit - 257, 5);
            send(hdist - 1, 5);
            send(hclen - 4, 4);
            for (i = 0; i < hclen; i++)
                send(cl[clord[i]], 3);
            for (i = 0; i < m; i++) {
                send(cc[rc[i]], cl[rc[i]]);
                if (rc[i] == 16)
                    send(ra[i] - 3, 2);
                else if (rc[i] == 17)
                    send(ra[i] - 3, 3);
                else if (rc[i] == 18)
                    send(ra[i] - 11, 7);
            }
            emit(ll, lc, dl, dc);
        }
    }
    block_start += blk_bytes;
    blk_bytes = 0;
    nsym = 0;
    xbits = 0;
    for (i = 0; i < 286; i++)
        lfreq[i] = 0;
    for (i = 0; i < 30; i++)
        dfreq[i] = 0;
}

static void tally_lit(unsigned char c)
{
    SL[nsym] = c;
    SD[nsym++] = 0;
    lfreq[c]++;
    blk_bytes++;
}

static void tally_dist(unsigned dist, unsigned len)
{
    unsigned c;
    SL[nsym] = len;
    SD[nsym++] = dist;
    c = lcode[len - 3];
    lfreq[257 + c]++;
    xbits += lext[c];
    c = dist <= 256 ? dcode[dist - 1] : dcode[256 + ((dist - 1) >> 7)];
    dfreq[c]++;
    xbits += dext[c];
    blk_bytes += len;
}

/* Fenster um 32 KB schieben, wenn strstart in der oberen Haelfte steht */
static void slide(void)
{
    unsigned i, m;
    if (strstart < WSIZE + MAXDIST)
        return;
    if (nsym)
        flush_block(0);
    for (i = 0; i < WSIZE; i++)
        W[i] = W[i + WSIZE];
    if (match_start >= WSIZE)
        match_start -= WSIZE;
    else
        match_start = 0;
    strstart -= WSIZE;
    block_start -= WSIZE;
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

static unsigned prev_length;

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
    limit = strstart > MAXDIST ? strstart - MAXDIST : 0;
    e0 = scan[best - 1];
    e1 = scan[best];
    do {
        m = W + cur;
        if (m[best] != e1 || m[best - 1] != e0 || m[0] != scan[0] || m[1] != scan[1])
            continue;
        for (len = 2; len < 258 && m[len] == scan[len]; len++)
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

static unsigned prev_match, match_length;
static int avail;

/* so weit packen, wie Vorlauf da ist (fin: bis zum Ende) */
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
        if (hh && prev_length < max_lazy && strstart - hh <= MAXDIST) {
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
            if (nsym >= NSYM - 1)
                flush_block(0);
        } else if (avail) {
            tally_lit(W[strstart - 1]);
            if (nsym >= NSYM - 1)
                flush_block(0);
            strstart++;
            lookahead--;
        } else {
            avail = 1;
            strstart++;
            lookahead--;
        }
    }
}

void deflate_begin(int level)
{
    unsigned i;

    if (level < 1 || level > 9)
        level = 6;
    good_len = cfg[level].good;
    max_lazy = cfg[level].lazy;
    nice_len = cfg[level].nice;
    max_chain = cfg[level].chain;
    for (i = 0; i <= HMASK; i++)
        HD[i] = 0;
    lookahead = match_start = 0;
    nsym = blk_bytes = 0;
    xbits = 0;
    prev_length = 2;
    match_length = 2;
    avail = 0;
    for (i = 0; i < 286; i++)
        lfreq[i] = 0;
    for (i = 0; i < 30; i++)
        dfreq[i] = 0;
    opos = 0;
    obuf = 0;
    ovalid = 0;
    werr = 0;
    strstart = 1;                       /* Stelle 0 = keine Kette       */
    block_start = 1;
}

/* Daten zuschieben (fuer Programme, die selbst erzeugen, z. B. TAR) */
int deflate_feed(const unsigned char *p, unsigned n)
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
        fl_nin += k;
        p += k;
        n -= k;
        if (lookahead >= 2)
            ins_h = (((unsigned)W[strstart] << 5) ^ W[strstart + 1]) & HMASK;
        run(0);
    }
    return werr;
}

int deflate_end(void)
{
    slide();
    run(1);
    if (avail)
        tally_lit(W[strstart - 1]);
    flush_block(1);
    align();
    if (opos) {
        if (fl_write(ob, opos))
            werr = FL_EWRITE;
        fl_nout += opos;
        opos = 0;
    }
    return werr;
}

int deflate(int level)
{
    static unsigned char buf[1024];
    unsigned n;

    deflate_begin(level);
    for (;;) {
        n = fl_read(buf, sizeof(buf));
        if (!n || n == 0xFFFF)
            break;
        fl_nin -= n;                    /* deflate_feed zaehlt selbst    */
        if (deflate_feed(buf, n))
            return werr;
    }
    return deflate_end();
}
