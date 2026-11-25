/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS - LZX-Entpacker (Cabinet-Dateien, Typ 3, Fenster 2^15..2^21),
 * eigene Umsetzung nach der veroeffentlichten Beschreibung des Formats
 * (Microsoft, "LZX Data Compression Format"). Gebraucht von COREFONT.EXE
 * fuer die Schriftpakete; laeuft im 16-Bit-Programm (Fenster in Segmenten,
 * Zugriff ueber LZX_WGET/LZX_WPUT des Gastgebers) und zum Pruefen auf dem
 * Rechner (gcc, Fenster als Feld).
 *
 * Gastgeber stellt bereit:
 *   int  lzx_byte(void)                naechstes Byte der gepackten Daten,
 *                                      -1 am Ende
 *   void lzx_emit(unsigned char *b, unsigned n)   fertiger Rahmen (32 KB)
 *   LZX_WGET(pos) / LZX_WPUT(pos, b)   Fenster (pos 32 Bit, < Groesse)
 */
#include "lzx.h"

#define NPRE    20
#define NLEN    249
#define NALIGN  8
#define MAXMAIN (256 + 50 * 8)

static unsigned short lb_w;            /* Bitleser: 16-Bit-Woerter,  */
static unsigned char  lb_n;            /* hoechstes Bit zuerst       */
static int lb_eof;

static unsigned getbit(void)
{
    unsigned b;
    if (!lb_n) {
        int lo = lzx_byte(), hi = lzx_byte();
        if (lo < 0 || hi < 0) {
            lb_eof = 1;
            lo = hi = 0;
        }
        lb_w = (unsigned short)(lo | (hi << 8));
        lb_n = 16;
    }
    b = (lb_w >> 15) & 1;
    lb_w <<= 1;
    lb_n--;
    return b;
}

static unsigned long getbits(unsigned n)
{
    unsigned long v = 0;
    while (n--)
        v = (v << 1) | getbit();
    return v;
}

/* kanonischer Huffman-Code: Anzahl je Laenge, Zeichen nach Laenge */
struct huff {
    unsigned short cnt[17];
    unsigned short sym[MAXMAIN];
};

static int build(struct huff *h, const unsigned char *len, unsigned n)
{
    unsigned short offs[17];
    unsigned i, l;
    for (l = 0; l < 17; l++)
        h->cnt[l] = 0;
    for (i = 0; i < n; i++)
        h->cnt[len[i]]++;
    h->cnt[0] = 0;
    offs[1] = 0;
    for (l = 1; l < 16; l++)
        offs[l + 1] = offs[l] + h->cnt[l];
    for (i = 0; i < n; i++)
        if (len[i])
            h->sym[offs[len[i]]++] = (unsigned short)i;
    return 0;
}

static int decode(const struct huff *h)
{
    unsigned code = 0, first = 0, index = 0, l, c;
    for (l = 1; l <= 16; l++) {
        code |= getbit();
        c = h->cnt[l];
        if (code - first < c)
            return h->sym[index + code - first];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;                          /* kaputter Strom            */
}

static struct huff h_main, h_len, h_pre, h_al;
static unsigned char l_main[MAXMAIN], l_len[NLEN], l_pre[NPRE], l_al[NALIGN];
static unsigned char extra[51];
static unsigned long pbase[51];
static unsigned nmain;

/* Laengen first..last-1 ueber den Vorbaum (Differenz zur alten Laenge) */
static int read_lens(unsigned char *len, unsigned first, unsigned last)
{
    unsigned i, x, y;
    int z;
    for (i = 0; i < NPRE; i++)
        l_pre[i] = (unsigned char)getbits(4);
    build(&h_pre, l_pre, NPRE);
    for (x = first; x < last; ) {
        z = decode(&h_pre);
        if (z < 0)
            return -1;
        if (z == 17 || z == 18) {
            y = z == 17 ? 4 + (unsigned)getbits(4) : 20 + (unsigned)getbits(5);
            while (y-- && x < last)
                len[x++] = 0;
        } else if (z == 19) {
            y = 4 + (unsigned)getbits(1);
            z = decode(&h_pre);
            if (z < 0 || z > 16)
                return -1;
            z = len[x] - z;
            if (z < 0)
                z += 17;
            while (y-- && x < last)
                len[x++] = (unsigned char)z;
        } else {
            z = len[x] - z;
            if (z < 0)
                z += 17;
            len[x++] = (unsigned char)z;
        }
    }
    return 0;
}

static unsigned char frame[32768];
static unsigned long intel_size, intel_pos;
static int intel_on;

static void e8(unsigned n)
{
    unsigned i;
    long cur = (long)intel_pos, a;
    if (!intel_on || n <= 10)
        return;
    for (i = 0; i < n - 10; ) {
        if (frame[i] != 0xE8) {
            i++;
            cur++;
            continue;
        }
        a = (long)((unsigned long)frame[i + 1] | ((unsigned long)frame[i + 2] << 8) |
                   ((unsigned long)frame[i + 3] << 16) | ((unsigned long)frame[i + 4] << 24));
        if (a >= -cur && a < (long)intel_size) {
            long r = a >= 0 ? a - cur : a + (long)intel_size;
            frame[i + 1] = (unsigned char)r;
            frame[i + 2] = (unsigned char)(r >> 8);
            frame[i + 3] = (unsigned char)(r >> 16);
            frame[i + 4] = (unsigned char)(r >> 24);
        }
        i += 5;
        cur += 5;
    }
}

/* Ordner entpacken: wbits Fenstergroesse (15..21), total = Bytes
   -> 0 gut, sonst Fehler */
int lzx_decode(unsigned wbits, unsigned long total)
{
    unsigned long pos = 0, fstart = 0, r0 = 1, r1 = 1, r2 = 1, remain = 0;
    unsigned long wsize = 1UL << wbits, off, blen = 0;
    unsigned i, btype = 0, nslots, len;
    int sym;
    if (wbits < 15 || wbits > 21)
        return 1;
    nslots = wbits == 21 ? 50 : wbits == 20 ? 42 : wbits * 2;
    nmain = 256 + nslots * 8;
    for (i = 0; i < 51; i++) {
        extra[i] = (unsigned char)(i < 4 ? 0 : (i >> 1) - 1 > 17 ? 17 : (i >> 1) - 1);
        pbase[i] = i ? pbase[i - 1] + (1UL << extra[i - 1]) : 0;
    }
    for (i = 0; i < MAXMAIN; i++)
        l_main[i] = 0;
    for (i = 0; i < NLEN; i++)
        l_len[i] = 0;
    lb_n = 0;
    lb_eof = 0;
    intel_on = 0;
    intel_pos = 0;
    if (getbit()) {
        intel_size = getbits(16) << 16;
        intel_size |= getbits(16);
        intel_on = 1;
    } else
        intel_size = 0;
    intel_on = 0;                       /* erst mit E8 im Hauptbaum   */
    while (pos < total) {
        unsigned long fend = fstart + 32768UL;
        if (fend > total)
            fend = total;
        while (pos < fend) {
            if (!remain) {
                btype = (unsigned)getbits(3);
                remain = getbits(16) << 8;
                remain |= getbits(8);
                blen = remain;
                if (btype == 2) {
                    for (i = 0; i < NALIGN; i++)
                        l_al[i] = (unsigned char)getbits(3);
                    build(&h_al, l_al, NALIGN);
                }
                if (btype == 1 || btype == 2) {
                    if (read_lens(l_main, 0, 256) || read_lens(l_main, 256, nmain))
                        return 2;
                    build(&h_main, l_main, nmain);
                    if (l_main[0xE8])
                        intel_on = intel_size != 0;
                    if (read_lens(l_len, 0, NLEN))
                        return 2;
                    build(&h_len, l_len, NLEN);
                } else if (btype == 3) {
                    unsigned char b[12];
                    intel_on = intel_size != 0;
                    if (!lb_n)                  /* auf 16 Bit: 1-16 Bit */
                        (void)getbits(16);      /* ueberspringen        */
                    lb_n = 0;
                    for (i = 0; i < 12; i++) {
                        int c = lzx_byte();
                        if (c < 0)
                            return 3;
                        b[i] = (unsigned char)c;
                    }
                    r0 = b[0] | ((unsigned long)b[1] << 8) | ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
                    r1 = b[4] | ((unsigned long)b[5] << 8) | ((unsigned long)b[6] << 16) | ((unsigned long)b[7] << 24);
                    r2 = b[8] | ((unsigned long)b[9] << 8) | ((unsigned long)b[10] << 16) | ((unsigned long)b[11] << 24);
                } else
                    return 4;
            }
            if (btype == 3) {           /* ungepackt                  */
                while (remain && pos < fend) {
                    int c = lzx_byte();
                    if (c < 0)
                        return 3;
                    LZX_WPUT(pos & (wsize - 1), (unsigned char)c);
                    pos++;
                    remain--;
                }
                if (!remain && (blen & 1))
                    (void)lzx_byte();   /* ungerade Laenge: Fuellbyte */
                continue;
            }
            while (remain && pos < fend) {
                sym = decode(&h_main);
                if (sym < 0)
                    return 5;
                if (sym < 256) {
                    LZX_WPUT(pos & (wsize - 1), (unsigned char)sym);
                    pos++;
                    remain--;
                    continue;
                }
                sym -= 256;
                len = sym & 7;
                if (len == 7) {
                    int e = decode(&h_len);
                    if (e < 0)
                        return 5;
                    len += (unsigned)e;
                }
                len += 2;
                i = (unsigned)sym >> 3;
                if (i > 2) {
                    unsigned x = extra[i];
                    if (btype == 2) {
                        off = pbase[i] - 2;
                        if (x > 3) {
                            off += getbits(x - 3) << 3;
                            sym = decode(&h_al);
                            if (sym < 0)
                                return 5;
                            off += (unsigned)sym;
                        } else if (x == 3) {
                            sym = decode(&h_al);
                            if (sym < 0)
                                return 5;
                            off += (unsigned)sym;
                        } else if (x)
                            off += getbits(x);
                        else
                            off = 1;
                    } else
                        off = i == 3 ? 1 : pbase[i] - 2 + getbits(x);
                    r2 = r1;
                    r1 = r0;
                    r0 = off;
                } else if (i == 0)
                    off = r0;
                else if (i == 1) {
                    off = r1;
                    r1 = r0;
                    r0 = off;
                } else {
                    off = r2;
                    r2 = r0;
                    r0 = off;
                }
                if (off > pos || len > remain)
                    return 6;
                remain -= len;
                while (len--) {
                    LZX_WPUT(pos & (wsize - 1), LZX_WGET((pos - off) & (wsize - 1)));
                    pos++;
                }
            }
        }
        if (lb_eof)
            return 7;
        /* Rahmen fertig: ausgeben (E8 auf einer Kopie), Bitstrom auf
           16 Bit ausrichten */
        len = (unsigned)(pos - fstart);
        for (off = 0; off < len; off++)
            frame[(unsigned)off] = LZX_WGET((fstart + off) & (wsize - 1));
        e8(len);
        intel_pos += len;
        lzx_emit(frame, len);
        fstart = pos;
        lb_n = 0;
    }
    return 0;
}
