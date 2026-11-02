/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WC.EXE - Zeilen, Woerter und Bytes zaehlen (nach dem Vorbild von GNU wc):
 *
 *   WC [-l] [-w] [-c] [-m] [-L] [DATEI...]
 *
 * Ohne Schalter: Zeilen, Woerter, Bytes. -m Zeichen (= Bytes, CP437),
 * -L laengste Zeile. Mehrere Dateien bekommen eine Summenzeile. Ohne
 * Datei oder mit - die Standardeingabe.
 */
#include "fapi.h"
#include "util.h"
#define TI_FILE
#include "textin.h"

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN];
static int o_l, o_w, o_c, o_m, o_L, nfiles, rc;
static unsigned long t_l, t_w, t_c, t_L;

static int ncol, pad;

static void num(unsigned long v)
{
    static char b[12];
    static const unsigned long pw[10] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL };
    unsigned i, n = 0, d, any = 0;
    for (i = 0; i < 10; i++) {
        for (d = 0; v >= pw[i]; d++)
            v -= pw[i];
        if (d || any || i == 9) {
            b[n++] = (char)('0' + d);
            any = 1;
        }
    }
    b[n] = 0;
    if (ncol++)
        to_put(' ');
    for (i = n; pad && i < 7; i++)
        to_put(' ');
    to_str(b);
}

static void show(unsigned long l, unsigned long w, unsigned long c, unsigned long L,
                 const char *nm)
{
    if (o_l) num(l);
    if (o_w) num(w);
    if (o_c || o_m) num(c);
    if (o_L) num(L);
    if (*nm) {
        to_put(' ');
        to_str(nm);
    }
    ncol = 0;
    to_str("\r\n");
}

static void wc(unsigned h, const char *nm)
{
    unsigned long l = 0, w = 0, c = 0, L = 0, col = 0;
    int ch, inw = 0;
    ti_open(h);
    while ((ch = ti_get()) >= 0) {
        c++;
        if (ch == '\n') {
            l++;
            if (col > L)
                L = col;
            col = 0;
        } else if (ch != '\r')
            col = ch == '\t' ? (col + 8) & ~7UL : col + 1;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v')
            inw = 0;
        else if (!inw) {
            inw = 1;
            w++;
        }
    }
    if (col > L)
        L = col;
    t_l += l;
    t_w += w;
    t_c += c;
    if (L > t_L)
        t_L = L;
    show(l, w, c, L, nm);
}

void main(void)
{
    char *a;
    int pass;
    unsigned h;

    DosGetArgs(args);
    for (pass = 0; pass < 2; pass++)
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            if (!pass)
                puts("Z\x84hlt Zeilen, W\x94rter und Bytes (nach dem Vorbild von GNU wc).\r\n\r\n"
                     "WC [-l] [-w] [-c] [-m] [-L] [Datei...]\r\n\r\n"
                     "  -l Zeilen  -w W\x94rter  -c Bytes  -m Zeichen  -L l\x84ngste Zeile\r\n");
            return;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ') {
            char *o;
            if (pass)
                continue;
            for (o = a + 1; *o && *o != ' '; o++)
                switch (*o) {
                case 'l': o_l = 1; break;
                case 'w': o_w = 1; break;
                case 'c': o_c = 1; break;
                case 'm': o_m = 1; break;
                case 'L': o_L = 1; break;
                default:
                    puts("WC: unbekannter Schalter - ");
                    puts(a);
                    puts("\r\n");
                    DosExit(1);
                }
            continue;
        }
        if (!pass) {
            nfiles++;
            continue;
        }
        if (!o_l && !o_w && !o_c && !o_m && !o_L)
            o_l = o_w = o_c = 1;
        pad = 1;
        word(a, name, sizeof(name));
        h = ti_file(name);
        if (FAPI_FAILED(h)) {
            to_flush();
            puts("WC: Datei nicht gefunden - ");
            puts(name);
            puts("\r\n");
            rc = 1;
            continue;
        }
        wc(h, name);
        if (h != STDIN)
            DosClose(h);
    }
    if (!o_l && !o_w && !o_c && !o_m && !o_L)
        o_l = o_w = o_c = 1;
    if (!nfiles) {                      /* eine Zahl ohne Namen: ungepolstert */
        pad = o_l + o_w + (o_c | o_m) + o_L > 1;
        wc(STDIN, "");
    }
    else if (nfiles > 1)
        show(t_l, t_w, t_c, t_L, "insgesamt");
    to_flush();
    DosExit(rc);
}
