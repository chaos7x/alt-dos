/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HEXEDIT.EXE - Dateien als Hex ansehen und Byte fuer Byte aendern
 * (Ueberschreiben, keine Laengenaenderung):
 *
 *   HEXEDIT Datei
 *
 * Tasten: Pfeile, Bild auf/ab, Pos1/Ende (Zeile), Strg+Pos1/Ende (Datei),
 * Tab wechselt zwischen Hex- und Textspalte, 0-9/A-F bzw. Zeichen
 * ueberschreiben, F2 speichern, F5 Stelle anspringen (hex), F7 suchen
 * (Text, oder Hex mit # vorn: #4D5A), Shift+F7 bzw. F3 weitersuchen,
 * Esc/F10 Ende (fragt bei Aenderungen). Geaenderte Bytes erscheinen gelb.
 * Bis 256 KB.
 */
#include "fapi.h"
#include "util.h"
#include <i86.h>

#define NSEG    4
static unsigned seg[NSEG], mseg;        /* Inhalt, Aenderungsbits        */
static unsigned long fsize, top, cur;
static unsigned rows = 25, half, dirty;
static int asc;
static char name[FAPI_PATHLEN], args[FAPI_ARGLEN], ln[100], msg[80], sbuf[64];
static char rb[2048];

#define BYTE(p)  (((unsigned char __far *)MK_FP(seg[(unsigned)((p) >> 16)], 0))[(unsigned)(p)])
#define MOD(p)   (((unsigned char __far *)MK_FP(mseg, 0))[(unsigned)((p) >> 3)])

static const char hx[] = "0123456789ABCDEF";

static void put_at(unsigned r, const char *s, unsigned a)
{
    VioWrtStrAtt(r, 0, (char *)s, a);
}

static void draw(void)
{
    unsigned r, i, n;
    unsigned long p;
    char *o;
    /* Kopf */
    o = ln;
    for (i = 0; i < 80; i++) ln[i] = ' ';
    ln[80] = 0;
    scopy(ln + 1, base_name(name), 40);
    ln[slen(ln)] = ' ';
    o = ln + 42;
    scopy(o, dirty ? "ge\x84ndert" : "", 10);
    if (o[0]) o[slen(o)] = ' ';
    o = ln + 54;
    for (i = 0; i < 8; i++)
        *o++ = hx[(unsigned)(cur >> (28 - 4 * i)) & 15];
    *o++ = ' ';
    *o++ = '/';
    *o++ = ' ';
    for (i = 0; i < 8; i++)
        *o++ = hx[(unsigned)(fsize >> (28 - 4 * i)) & 15];
    put_at(0, ln, 0x70);
    for (r = 1; r < rows - 1; r++) {
        p = top + ((unsigned long)(r - 1) << 4);
        for (i = 0; i < 80; i++) ln[i] = ' ';
        ln[80] = 0;
        if (p < fsize) {
            for (i = 0; i < 8; i++)
                ln[i] = hx[(unsigned)(p >> (28 - 4 * i)) & 15];
            for (n = 0; n < 16 && p + n < fsize; n++) {
                unsigned char c = BYTE(p + n);
                ln[10 + n * 3 + (n >= 8)] = hx[c >> 4];
                ln[11 + n * 3 + (n >= 8)] = hx[c & 15];
                ln[61 + n] = c < 32 || c == 127 ? '.' : (char)c;
            }
        }
        put_at(r, ln, 0x17);
        /* geaenderte und aktuelle Bytes einzeln faerben */
        for (n = 0; n < 16 && p + n < fsize; n++) {
            unsigned long q = p + n;
            unsigned a = (MOD(q) & (1 << (unsigned)(q & 7))) ? 0x1E : 0x17;
            char t[3];
            if (q == cur)
                a = 0x70;
            if (a == 0x17)
                continue;
            t[0] = ln[10 + n * 3 + (n >= 8)];
            t[1] = ln[11 + n * 3 + (n >= 8)];
            t[2] = 0;
            VioWrtStrAtt(r, 10 + n * 3 + (n >= 8), t, q == cur && asc ? 0x1E : a);
            t[0] = ln[61 + n];
            t[1] = 0;
            VioWrtStrAtt(r, 61 + n, t, q == cur && !asc ? 0x1E : a);
        }
    }
    for (i = 0; i < 80; i++) ln[i] = ' ';
    ln[80] = 0;
    if (msg[0])
        scopy(ln + 1, msg, 79);
    else
        scopy(ln + 1, "Tab Hex/Text  F2 Speichern  F5 Gehe zu  F7 Suchen  F3 Weiter  Esc Ende", 79);
    ln[slen(ln)] = ' ';
    put_at(rows - 1, ln, 0x70);
    /* Cursor auf die Spalte */
    {
        unsigned rr = 1 + (unsigned)((cur - top) >> 4), n2 = (unsigned)(cur & 15);
        VioSetCurPos(rr, asc ? 61 + n2 : 10 + n2 * 3 + (n2 >= 8) + half);
    }
}

static void fix_top(void)
{
    unsigned long page = (unsigned long)(rows - 2) << 4;
    if (cur < top)
        top = cur & ~15UL;
    if (cur >= top + page)
        top = (cur & ~15UL) - page + 16;
}

static int ask_line(const char *prompt, char *b, unsigned max)
{
    unsigned n = 0, k;
    char c;
    b[0] = 0;
    for (;;) {
        for (k = 0; k < 80; k++) ln[k] = ' ';
        ln[80] = 0;
        scopy(ln + 1, prompt, 40);
        scopy(ln + 1 + slen(prompt), b, 38);
        ln[slen(ln)] = ' ';
        put_at(rows - 1, ln, 0x1F);
        VioSetCurPos(rows - 1, 1 + slen(prompt) + n);
        k = KbdCharIn();
        c = (char)k;
        if (c == 13)
            return 1;
        if (c == 27)
            return 0;
        if (c == 8 && n)
            b[--n] = 0;
        else if ((unsigned char)c >= ' ' && n < max - 1) {
            b[n++] = c;
            b[n] = 0;
        }
    }
}

static int hexv(char c)
{
    c = up(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Suchmuster aus sbuf -> pat, Laenge */
static unsigned char pat[32];
static unsigned plen;

static int make_pat(void)
{
    unsigned i;
    plen = 0;
    if (sbuf[0] == '#') {
        for (i = 1; sbuf[i] && sbuf[i + 1] && plen < 32; ) {
            int a = hexv(sbuf[i]), b = hexv(sbuf[i + 1]);
            if (a < 0 || b < 0) { i++; continue; }
            pat[plen++] = (unsigned char)(a << 4 | b);
            i += 2;
        }
    } else
        for (i = 0; sbuf[i] && plen < 32; i++)
            pat[plen++] = (unsigned char)sbuf[i];
    return plen != 0;
}

static void search(unsigned long from)
{
    unsigned long p;
    unsigned i;
    if (!plen)
        return;
    for (p = from; p + plen <= fsize; p++) {
        for (i = 0; i < plen; i++) {
            unsigned char c = BYTE(p + i);
            if (sbuf[0] != '#' && c >= 'a' && c <= 'z' && pat[i] >= 'A' && pat[i] <= 'Z')
                c = (unsigned char)(c - 32);
            if (sbuf[0] != '#' && pat[i] >= 'a' && pat[i] <= 'z' && c == pat[i] - 32)
                c = pat[i];
            if (c != pat[i])
                break;
        }
        if (i == plen) {
            cur = p;
            half = 0;
            fix_top();
            msg[0] = 0;
            return;
        }
    }
    scopy(msg, "Nicht gefunden.", sizeof(msg));
}

static int save(void)
{
    unsigned h, k, n;
    unsigned long p = 0;
    h = DosOpenRW(name);
    if (FAPI_FAILED(h)) {
        scopy(msg, "Speichern nicht m\x94glich (schreibgesch\x81tzt oder in Benutzung).", sizeof(msg));
        return 0;
    }
    while (p < fsize) {
        n = fsize - p > sizeof(rb) ? sizeof(rb) : (unsigned)(fsize - p);
        for (k = 0; k < n; k++)
            rb[k] = (char)BYTE(p + k);
        if (DosWrite(h, rb, n) != n) {
            DosClose(h);
            scopy(msg, "Schreibfehler.", sizeof(msg));
            return 0;
        }
        p += n;
    }
    DosClose(h);
    for (p = 0; p < fsize; p += 8)
        MOD(p) = 0;
    dirty = 0;
    scopy(msg, "Gespeichert.", sizeof(msg));
    return 1;
}

void main(void)
{
    unsigned h, n, k, s;
    unsigned long p = 0;
    char c, sc;

    DosGetArgs(args);
    word(skip(args), name, sizeof(name));
    if (!name[0] || (name[0] == '/' && name[1] == '?')) {
        puts("Zeigt eine Datei als Hex und \x84ndert einzelne Bytes.\r\n\r\n"
             "HEXEDIT Datei\r\n\r\n"
             "Tab Hex/Text, F2 speichern, F5 Stelle (hex), F7 suchen (#4D5A = hex),\r\n"
             "F3 weitersuchen, Esc Ende. Bis 256 KB, ohne L\x84nge zu \x84ndern.\r\n");
        DosExit(name[0] ? 0 : 1);
    }
    h = DosOpen(name);
    if (FAPI_FAILED(h)) {
        puts("hexedit: Datei nicht gefunden\r\n");
        DosExit(1);
    }
    while ((n = DosRead(h, rb, sizeof(rb))) != 0 && !FAPI_FAILED(n)) {
        for (k = 0; k < n; k++, p++) {
            s = (unsigned)(p >> 16);
            if (s >= NSEG) {
                DosClose(h);
                puts("hexedit: Datei gr\x94\xE1" "er als 256 KB\r\n");
                DosExit(1);
            }
            if (!seg[s]) {
                seg[s] = DosAllocSeg(0);
                if (FAPI_FAILED(seg[s]) || !seg[s]) {
                    DosClose(h);
                    puts("hexedit: nicht gen\x81gend Arbeitsspeicher\r\n");
                    DosExit(1);
                }
            }
            BYTE(p) = (unsigned char)rb[k];
        }
    }
    DosClose(h);
    fsize = p;
    mseg = DosAllocSeg((unsigned)((fsize >> 3) + 16));
    if (FAPI_FAILED(mseg) || !mseg) {
        puts("hexedit: nicht gen\x81gend Arbeitsspeicher\r\n");
        DosExit(1);
    }
    for (p = 0; p < fsize; p += 8)
        MOD(p) = 0;
    if (!fsize) {
        puts("hexedit: Datei ist leer\r\n");
        DosExit(1);
    }
    rows = VioSetMode(1);
    if (FAPI_FAILED(rows) || rows < 25)
        rows = 25;
    for (;;) {
        fix_top();
        draw();
        k = KbdCharIn();
        c = (char)k;
        sc = (char)(k >> 8);
        msg[0] = 0;
        if (KEY_EXT((unsigned char)c, (unsigned char)sc)) {
            unsigned long page = (unsigned long)(rows - 2) << 4;
            switch ((unsigned char)sc) {
            case 0x48: if (cur >= 16) cur -= 16; break;
            case 0x50: if (cur + 16 < fsize) cur += 16; break;
            case 0x4B:
                if (!asc && half) half = 0;
                else if (cur) { cur--; half = asc ? 0 : 1; }
                break;
            case 0x4D:
                if (!asc && !half) half = 1;
                else if (cur + 1 < fsize) { cur++; half = 0; }
                break;
            case 0x49: cur = cur > page ? cur - page : 0; top = top > page ? top - page : 0; break;
            case 0x51: cur = cur + page < fsize ? cur + page : fsize - 1; top += page; if (top > cur) top = cur & ~15UL; break;
            case 0x47: cur &= ~15UL; half = 0; break;
            case 0x4F: cur |= 15; if (cur >= fsize) cur = fsize - 1; break;
            case 0x77: cur = 0; top = 0; break;          /* Strg+Pos1 */
            case 0x75: cur = fsize - 1; break;           /* Strg+Ende */
            case 0x3C: save(); break;                    /* F2       */
            case 0x3F:                                   /* F5       */
                if (ask_line("Gehe zu (hex): ", sbuf, 12)) {
                    unsigned long v = 0;
                    for (n = 0; sbuf[n]; n++)
                        if (hexv(sbuf[n]) >= 0)
                            v = (v << 4) | (unsigned)hexv(sbuf[n]);
                    if (v < fsize) { cur = v; half = 0; }
                    else scopy(msg, "Stelle hinter dem Dateiende.", sizeof(msg));
                }
                break;
            case 0x41:                                   /* F7       */
                if (ask_line("Suchen (#hex oder Text): ", sbuf, sizeof(sbuf)) && make_pat())
                    search(cur + 1);
                break;
            case 0x3D: case 0x5A:                        /* F3, Shift+F7 */
                search(cur + 1);
                break;
            case 0x44: goto quit;                        /* F10      */
            }
            continue;
        }
        if (c == 27)
            goto quit;
        if (c == 9) {
            asc = !asc;
            half = 0;
            continue;
        }
        if (asc) {
            if ((unsigned char)c >= ' ') {
                BYTE(cur) = (unsigned char)c;
                MOD(cur) |= (unsigned char)(1 << (unsigned)(cur & 7));
                dirty = 1;
                if (cur + 1 < fsize) cur++;
            }
        } else if (hexv(c) >= 0) {
            unsigned char b = BYTE(cur);
            b = half ? (unsigned char)((b & 0xF0) | hexv(c)) : (unsigned char)((b & 0x0F) | hexv(c) << 4);
            BYTE(cur) = b;
            MOD(cur) |= (unsigned char)(1 << (unsigned)(cur & 7));
            dirty = 1;
            if (half) {
                half = 0;
                if (cur + 1 < fsize) cur++;
            } else
                half = 1;
        }
        continue;
quit:
        if (dirty) {
            put_at(rows - 1, " Ge\x84ndert - speichern? (J/N, Esc = weiter bearbeiten)                           ", 0x1F);
            k = KbdCharIn() & 0xFF;
            if (k == 27)
                continue;
            if (up((char)k) == 'J' && !save())
                continue;
        }
        break;
    }
    VioSetMode(0);
}
