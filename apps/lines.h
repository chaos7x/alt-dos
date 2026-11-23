/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Textdateien zeilenweise im Speicher fuer DIFF und PATCH (per #include
 * nach fapi.h und util.h): Inhalt in bis zu TX_NSEG DosAllocSeg-Segmenten
 * (je 64 KB, also bis 256 KB), dazu je Zeile der Anfang (32 Bit) in einem
 * eigenen Segment - hoechstens TX_MAXL Zeilen. Zeilenende LF oder CR LF;
 * die Zeile selbst ohne CR LF, `nonl` merkt eine letzte Zeile ohne Ende.
 * DosRead liest nur ins eigene DS - daher ueber einen Puffer.
 */
#ifndef LINES_H
#define LINES_H
#include <i86.h>

#define TX_NSEG     4
#define TX_MAXL     16000u
#define TX_LMAX     1024                /* Zeile beim Kopieren (Rest weg) */

struct text {
    unsigned seg[TX_NSEG];
    unsigned nseg;
    unsigned long size;
    unsigned nl;                        /* Zeilen                         */
    unsigned lsel;                      /* Zeilenanfaenge (u32)           */
    unsigned char nonl;                 /* letzte Zeile ohne Zeilenende   */
    unsigned char crlf;                 /* Datei benutzt CR LF            */
};

static char tx_buf[2048];

#define TX_BYTE(t, p) (((unsigned char __far *)MK_FP((t)->seg[(unsigned)((p) >> 16)], 0))[(unsigned)(p)])
#define TX_START(t, i) (((unsigned long __far *)MK_FP((t)->lsel, 0))[i])

/* -> 0 ok, 1 Datei fehlt, 2 zu gross / kein Speicher */
static int tx_load(const char *name, struct text *t)
{
    unsigned h, n, i, s;
    unsigned long pos = 0, p;
    unsigned char c, prev = '\n';
    t->nseg = 0;
    t->nl = 0;
    t->lsel = 0;
    t->nonl = 0;
    t->crlf = 0;
    h = name[0] == '-' && name[1] == 0 ? STDIN : DosOpen(name);
    if (FAPI_FAILED(h))
        return 1;
    for (;;) {
        n = DosRead(h, tx_buf, sizeof(tx_buf));
        if (FAPI_FAILED(n) || n == 0)
            break;
        for (i = 0; i < n; ) {
            s = (unsigned)(pos >> 16);
            if (s >= TX_NSEG) {
                if (h != STDIN)
                    DosClose(h);
                return 2;
            }
            if (s >= t->nseg) {
                t->seg[s] = DosAllocSeg(0);
                if (FAPI_FAILED(t->seg[s]) || !t->seg[s]) {
                    if (h != STDIN)
                        DosClose(h);
                    return 2;
                }
                t->nseg = s + 1;
            }
            {
                unsigned long room = 0x10000UL - (pos & 0xFFFFUL);
                unsigned k = (unsigned long)(n - i) < room ? n - i : (unsigned)room;
                unsigned j;
                unsigned char __far *d = (unsigned char __far *)MK_FP(t->seg[s], (unsigned)pos);
                for (j = 0; j < k; j++)
                    d[j] = (unsigned char)tx_buf[i + j];
                i += k;
                pos += k;
            }
        }
    }
    if (h != STDIN)
        DosClose(h);
    t->size = pos;
    /* Zeilen zaehlen */
    n = 0;
    for (p = 0; p < pos; p++) {
        c = TX_BYTE(t, p);
        if (prev == '\n')
            n++;
        prev = c;
    }
    if (n > TX_MAXL)
        return 2;
    t->lsel = DosAllocSeg(n ? n * 4 : 4);
    if (FAPI_FAILED(t->lsel) || !t->lsel)
        return 2;
    n = 0;
    prev = '\n';
    for (p = 0; p < pos; p++) {
        c = TX_BYTE(t, p);
        if (prev == '\n')
            TX_START(t, n++) = p;
        if (c == '\n' && p && TX_BYTE(t, p - 1) == '\r')
            t->crlf = 1;
        prev = c;
    }
    t->nl = n;
    t->nonl = pos && prev != '\n';
    return 0;
}

/* Zeile i nach b (ohne CR LF, abgeschnitten auf max-1) -> Laenge */
static unsigned tx_line(struct text *t, unsigned i, char *b, unsigned max)
{
    unsigned long p = TX_START(t, i), e = i + 1 < t->nl ? TX_START(t, i + 1) : t->size;
    unsigned n = 0;
    while (p < e) {
        unsigned char c = TX_BYTE(t, p);
        p++;
        if (c == '\n' || (c == '\r' && p < e && TX_BYTE(t, p) == '\n' && p + 1 == e))
            break;
        if (n < max - 1)
            b[n++] = (char)c;
    }
    b[n] = 0;
    return n;
}

static void tx_free(struct text *t)
{
    unsigned i;
    for (i = 0; i < t->nseg; i++)
        DosFreeSeg(t->seg[i]);
    if (t->lsel)
        DosFreeSeg(t->lsel);
    t->nseg = 0;
    t->lsel = 0;
}

#endif
