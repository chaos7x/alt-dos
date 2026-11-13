/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LESS.EXE - Text ansehen, vor- und zurueckblaettern, nach dem Vorbild von
 * GNU less (nur Bedienung, kein GNU-Code):
 *
 *   LESS [-N] [-i] [DATEI]        befehl | LESS
 *
 * Tasten (wie less):
 *   Leertaste f Bild-ab      eine Seite vor      b Bild-auf   eine Seite zurueck
 *   Eingabe j e Pfeil-ab     eine Zeile vor      k y Pfeil-auf eine Zeile zurueck
 *   d / u                    halbe Seite vor/zurueck
 *   g < Pos1                 Anfang              G > Ende     Ende
 *   Pfeil-links/-rechts      seitlich schieben   123g         zu Zeile 123
 *   /Text                    vorwaerts suchen    ?Text        rueckwaerts suchen
 *   n / N                    weitersuchen / in Gegenrichtung
 *   -N                       Zeilennummern an/aus      -i  Gross/klein egal
 *   =                        Position anzeigen   h            Hilfe
 *   q Q                      beenden
 *
 * Die Eingabe (Datei oder Pipe) wird ganz eingelesen und in Segmenten zu
 * 64 KB gehalten (bis 8 Segmente, bis 10900 Zeilen). Lange Zeilen werden
 * abgeschnitten (wie less -S), Treffer der Suche invers hervorgehoben.
 */
#include <i86.h>
#include "fapi.h"

static unsigned ROWS = 22;             /* Textzeilen, darunter Status;
                                           aus VioGetRows (MODE CON LINES=) */
#define COLS      80
#define NDATA     8
#define MAXLINES  10900                 /* 6 Byte je Zeile im Index      */
#define ATTR_REV  0x70

struct lidx { unsigned seg, off, len; };

static unsigned data[NDATA], ndata, dpos, idxsel;
static unsigned nlines, top, hcol;
static int opt_N, opt_i, truncated;
static char name[FAPI_PATHLEN], pat[64], rowbuf[COLS + 1], tmp[COLS + 1];
static char in[1024], status[COLS + 1];
static int last_dir = 1;

static struct lidx __far *IDX(unsigned i)
{
    return (struct lidx __far *)MK_FP(idxsel, i * sizeof(struct lidx));
}

static char __far *LINE(unsigned i)
{
    struct lidx __far *x = IDX(i);
    return (char __far *)MK_FP(x->seg, x->off);
}

static char low(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* ---- Einlesen ---------------------------------------------------------- */
static int new_seg(void)
{
    unsigned s;
    if (ndata == NDATA)
        return -1;
    s = DosAllocSeg(0);                 /* 64 KB                         */
    if (s == FAPI_ERROR || s == 0)
        return -1;
    data[ndata++] = s;
    dpos = 0;
    return 0;
}

static void read_all(unsigned h)
{
    unsigned n, i, start = 0, len = 0;
    char __far *d;
    char c;

    if (new_seg())
        return;
    for (;;) {
        n = DosRead(h, in, sizeof(in));
        if (n == 0 || n == FAPI_ERROR)
            break;
        for (i = 0; i < n; i++) {
            c = in[i];
            if (c == '\r' || c == 0x1A)
                continue;
            if (c != '\n') {
                if (len >= 2000)        /* sehr lange Zeile: teilen      */
                    c = '\n', i--;
                else {
                    if (dpos == 0xFFFF) {           /* Segment voll: Zeile umziehen */
                        unsigned old = data[ndata - 1], k;
                        if (new_seg()) {
                            truncated = 1;
                            return;
                        }
                        for (k = 0; k < len; k++)
                            ((char __far *)MK_FP(data[ndata - 1], 0))[k] =
                                ((char __far *)MK_FP(old, start))[k];
                        start = 0;
                        dpos = len;
                    }
                    d = MK_FP(data[ndata - 1], dpos++);
                    *d = c;
                    len++;
                    continue;
                }
            }
            if (nlines == MAXLINES) {
                truncated = 1;
                return;
            }
            IDX(nlines)->seg = data[ndata - 1];
            IDX(nlines)->off = start;
            IDX(nlines)->len = len;
            nlines++;
            start = dpos;
            len = 0;
        }
    }
    if (len && nlines < MAXLINES) {     /* letzte Zeile ohne Zeilenende  */
        IDX(nlines)->seg = data[ndata - 1];
        IDX(nlines)->off = start;
        IDX(nlines)->len = len;
        nlines++;
    }
}

/* ---- Anzeige ----------------------------------------------------------- */
static unsigned dec(char *d, unsigned v, unsigned w)
{
    char b[6];
    unsigned i = 0, k = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i + k < w)
        d[k++] = ' ';
    while (i)
        d[k++] = b[--i];
    return k;
}

/* Zeile i (Tabs zu 8, ab Spalte hcol) nach rowbuf, 80 Zeichen */
static void render(unsigned i)
{
    char __far *p = LINE(i);
    unsigned len = IDX(i)->len, k, col = 0, o = 0;

    if (opt_N)
        o = dec(rowbuf, i + 1, 6), rowbuf[o++] = ' ';
    for (k = 0; k < len && o < COLS; k++) {
        char c = p[k];
        if (c == '\t') {
            do {
                if (col >= hcol && o < COLS)
                    rowbuf[o++] = ' ';
                col++;
            } while (col & 7);
            continue;
        }
        if ((unsigned char)c < 32)
            c = '.';
        if (col++ >= hcol)
            rowbuf[o++] = c;
    }
    while (o < COLS)
        rowbuf[o++] = ' ';
    rowbuf[COLS] = 0;
}

static unsigned plen(void)
{
    unsigned n = 0;
    while (pat[n])
        n++;
    return n;
}

static int pat_at(const char *s, unsigned n)
{
    unsigned k;
    for (k = 0; k < n; k++)
        if (opt_i ? low(s[k]) != low(pat[k]) : s[k] != pat[k])
            return 0;
    return 1;
}

static void highlight(unsigned row)
{
    unsigned n = plen(), c, k;
    if (!n)
        return;
    for (c = 0; c + n <= COLS; c++)
        if (pat_at(rowbuf + c, n)) {
            for (k = 0; k < n; k++)
                tmp[k] = rowbuf[c + k];
            tmp[n] = 0;
            VioWrtStrAtt(row, c, tmp, ATTR_REV);
            c += n - 1;
        }
}

static void show_status(const char *msg)
{
    unsigned o = 0, k, bottom = top + ROWS < nlines ? top + ROWS : nlines;
    if (msg) {
        for (k = 0; msg[k] && o < COLS; k++)
            status[o++] = msg[k];
    } else {
        for (k = 0; name[k] && o < 40; k++)
            status[o++] = name[k];
        if (bottom >= nlines) {
            const char *e = "  (ENDE)";
            for (k = 0; e[k]; k++)
                status[o++] = e[k];
        } else {
            status[o++] = ' ';
            status[o++] = ' ';
            o += dec(status + o, bottom < 100 || nlines < 100 ? bottom * 100 / (nlines ? nlines : 1)
                                       : bottom / (nlines / 100 ? nlines / 100 : 1), 1);
            status[o++] = '%';
        }
    }
    while (o < COLS)
        status[o++] = ' ';
    status[COLS] = 0;
    VioWrtStrAtt(ROWS, 0, status, ATTR_REV);
}

static void draw(void)
{
    unsigned r;
    for (r = 0; r < ROWS; r++) {
        if (top + r < nlines) {
            render(top + r);
            VioWrtStr(r, 0, rowbuf);
            highlight(r);
        } else {
            unsigned k;
            rowbuf[0] = '~';                    /* hinter dem Ende, wie less */
            for (k = 1; k < COLS; k++)
                rowbuf[k] = ' ';
            rowbuf[COLS] = 0;
            VioWrtStr(r, 0, rowbuf);
        }
    }
    show_status(0);
}

static unsigned max_top(void)
{
    return nlines > ROWS ? nlines - ROWS : 0;
}

/* ---- Suche ------------------------------------------------------------- */
static int line_has(unsigned i)
{
    char __far *p = LINE(i);
    unsigned len = IDX(i)->len, n = plen(), c, k;
    if (!n)
        return 0;
    for (c = 0; c + n <= len; c++) {
        for (k = 0; k < n; k++)
            if (opt_i ? low(p[c + k]) != low(pat[k]) : p[c + k] != pat[k])
                break;
        if (k == n)
            return 1;
    }
    return 0;
}

static void search(int dir)
{
    unsigned i = top;
    if (!plen())
        return;
    for (;;) {
        if (dir > 0) {
            if (++i >= nlines)
                break;
        } else {
            if (i == 0)
                break;
            i--;
        }
        if (line_has(i)) {
            top = i > max_top() ? max_top() : i;
            if (i > top)                /* Treffer nahe am Ende: sichtbar */
                ;
            draw();
            return;
        }
    }
    draw();
    show_status("Muster nicht gefunden  (Taste)");
    KbdCharIn();
    show_status(0);
}

/* Text auf der Statuszeile eingeben; 0 = abgebrochen */
static int prompt(char lead, char *out, unsigned max)
{
    unsigned n = 0, k;
    for (;;) {
        status[0] = lead;
        for (k = 0; k < n; k++)
            status[k + 1] = out[k];
        status[n + 1] = 0;
        show_status(status);
        k = KbdCharIn() & 0xFF;
        if (k == 13) {
            out[n] = 0;
            return 1;
        }
        if (k == 27)
            return 0;
        if (k == 8) {
            if (n)
                n--;
            else
                return 0;
        } else if (k >= 32 && n < max - 1)
            out[n++] = (char)k;
    }
}

static void help(void)
{
    static const char *const h[] = {
        "  LESS - Tasten (nach dem Vorbild von GNU less)",
        "",
        "  Leertaste  f  Bild-ab      eine Seite vor",
        "  b  Bild-auf               eine Seite zur\x81" "ck",
        "  Eingabe  j  Pfeil-ab      eine Zeile vor",
        "  k  y  Pfeil-auf           eine Zeile zur\x81" "ck",
        "  d  u                      halbe Seite vor / zur\x81" "ck",
        "  g  <  Pos1                Anfang      G  >  Ende   Ende",
        "  123g                      zu Zeile 123",
        "  Pfeil-links / -rechts     seitlich schieben",
        "  /Text  ?Text              vorw\x84rts / r\x81" "ckw\x84rts suchen",
        "  n  N                      weitersuchen / in Gegenrichtung",
        "  -N  -i                    Zeilennummern / Gro\xE1-klein egal",
        "  =                         Position anzeigen",
        "  q                         beenden",
        0
    };
    unsigned r;
    VioCls();
    for (r = 0; h[r]; r++)
        VioWrtStr(r, 0, h[r]);
    show_status("Weiter mit beliebiger Taste");
    KbdCharIn();
    draw();
}

void main(void)
{
    static char args[FAPI_ARGLEN], num[8];
    unsigned h = STDIN, key, ch, sc, count = 0;
    char *a = args, *f;

    ROWS = VioGetRows();
    ROWS = FAPI_FAILED(ROWS) || ROWS < 3 ? 22 : ROWS - 1;
    DosGetArgs(args);
    for (;;) {
        while (*a == ' ')
            a++;
        if (a[0] != '-')
            break;
        for (a++; *a && *a != ' '; a++)
            if (*a == 'N')
                opt_N = 1;
            else if (*a == 'i' || *a == 'I')
                opt_i = 1;
    }
    f = a;
    while (*a && *a != ' ')
        a++;
    *a = 0;
    if (*f) {
        unsigned k;
        for (k = 0; f[k] && k < sizeof(name) - 1; k++)
            name[k] = f[k];
        h = DosOpen(f);
        if (FAPI_FAILED(h)) {
            puts("LESS: ");
            puts(f);
            puts(": Datei nicht gefunden\r\n");
            return;
        }
    } else {
        if (DosQHandType(STDIN) == HT_CONSOLE) {
            puts("Aufruf: LESS [-N] [-i] Datei   oder   Befehl | LESS\r\n");
            return;
        }
        name[0] = '('; name[1] = 'E'; name[2] = 'i'; name[3] = 'n';
        name[4] = 'g'; name[5] = 'a'; name[6] = 'b'; name[7] = 'e'; name[8] = ')';
    }
    idxsel = DosAllocSeg(0);
    if (idxsel == FAPI_ERROR || idxsel == 0) {
        puts("LESS: nicht gen\x81gend Arbeitsspeicher\r\n");
        return;
    }
    read_all(h);
    if (h != STDIN)
        DosClose(h);
    VioCls();
    draw();
    if (truncated)
        show_status("Eingabe zu gro\xE1 - nur der Anfang wird gezeigt");

    for (;;) {
        key = KbdCharIn();
        ch = key & 0xFF;
        sc = key >> 8;
        if (ch >= '0' && ch <= '9') {           /* Zahl vor g           */
            count = count * 10 + (ch - '0');
            num[0] = ':';
            dec(num + 1, count, 1);
            show_status(num);
            continue;
        }
        if KEY_EXT(ch, sc) {            /* Sondertasten         */
            switch (sc) {
            case 0x50: ch = 'j'; break;         /* Pfeil ab             */
            case 0x48: ch = 'k'; break;         /* Pfeil auf            */
            case 0x51: ch = 'f'; break;         /* Bild ab              */
            case 0x49: ch = 'b'; break;         /* Bild auf             */
            case 0x47: ch = 'g'; break;         /* Pos1                 */
            case 0x4F: ch = 'G'; break;         /* Ende                 */
            case 0x4D: hcol += 40; draw(); continue;
            case 0x4B: hcol = hcol >= 40 ? hcol - 40 : 0; draw(); continue;
            default: continue;
            }
        }
        switch (ch) {
        case 'q': case 'Q':
            VioCls();
            return;
        case ' ': case 'f': case 6: case 22:
            top = top + ROWS < max_top() ? top + ROWS : max_top();
            break;
        case 'b': case 2:
            top = top > ROWS ? top - ROWS : 0;
            break;
        case 13: case 'j': case 'e': case 14: case 5:
            if (top < max_top())
                top++;
            break;
        case 'k': case 'y': case 16: case 25:
            if (top)
                top--;
            break;
        case 'd': case 4:
            top = top + ROWS / 2 < max_top() ? top + ROWS / 2 : max_top();
            break;
        case 'u': case 21:
            top = top > ROWS / 2 ? top - ROWS / 2 : 0;
            break;
        case 'g': case '<':
            top = count ? (count - 1 > max_top() ? max_top() : count - 1) : 0;
            break;
        case 'G': case '>':
            top = count ? (count - 1 > max_top() ? max_top() : count - 1) : max_top();
            break;
        case '/': case '?':
            if (prompt((char)ch, pat, sizeof(pat))) {
                last_dir = ch == '/' ? 1 : -1;
                search(last_dir);
            } else
                draw();
            count = 0;
            continue;
        case 'n':
            search(last_dir);
            count = 0;
            continue;
        case 'N':
            search(-last_dir);
            count = 0;
            continue;
        case '-':
            show_status("-");
            ch = KbdCharIn() & 0xFF;
            if (ch == 'N')
                opt_N = !opt_N;
            else if (ch == 'i' || ch == 'I')
                opt_i = !opt_i;
            draw();
            show_status(ch == 'N' ? (opt_N ? "Zeilennummern an" : "Zeilennummern aus")
                                  : (opt_i ? "Gro\xE1/klein egal" : "Gro\xE1/klein beachten"));
            count = 0;
            continue;
        case '=': {
            static char msg[COLS + 1];
            unsigned o = 0, k;
            for (k = 0; name[k] && o < 40; k++)
                msg[o++] = name[k];
            msg[o++] = ':'; msg[o++] = ' ';
            msg[o++] = 'Z'; msg[o++] = 'e'; msg[o++] = 'i'; msg[o++] = 'l';
            msg[o++] = 'e'; msg[o++] = ' ';
            o += dec(msg + o, top + 1, 1);
            msg[o++] = '-';
            o += dec(msg + o, top + ROWS < nlines ? top + ROWS : nlines, 1);
            msg[o++] = ' '; msg[o++] = 'v'; msg[o++] = 'o'; msg[o++] = 'n'; msg[o++] = ' ';
            o += dec(msg + o, nlines, 1);
            msg[o] = 0;
            show_status(msg);
            count = 0;
            continue;
        }
        case 'h': case 'H':
            help();
            count = 0;
            continue;
        default:
            count = 0;
            continue;
        }
        count = 0;
        draw();
    }
}
