/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DF.EXE - Belegung aller Laufwerke in einer Tabelle, wie df (GNU):
 *
 *   DF [-h] [-T] [Laufwerk ...]
 *
 * Groesse, belegt, frei in KB (-h lesbar), Verwendung in %, Dateisystem
 * (-T) und Bezeichnung. Laufwerke ohne Medium werden uebergangen.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static struct fapi_fsinfo fi;
static struct fapi_label lb;
static unsigned long fr[2];
static int o_h, o_t;

/* 32/16-Division ohne Laufzeitbibliothek */
static unsigned long div16(unsigned long a, unsigned b)
{
    unsigned long q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= 1UL << i;
        }
    }
    return q;
}

static void sz(unsigned long kb, int w)
{
    if (o_h) {
        if (kb >= 1048576UL) {
            put_num(div16(kb + 524288UL, 1024) >> 10, w - 1, 0);
            putc('G');
        } else if (kb >= 1024) {
            put_num((kb + 512) >> 10, w - 1, 0);
            putc('M');
        } else {
            put_num(kb, w - 1, 0);
            putc('K');
        }
    } else
        put_num(kb, w, 0);
}

static void one(unsigned d)
{
    unsigned long tot, frk, used, pct;
    unsigned i;
    if (FAPI_FAILED(DosQFSInfo(d, &fi)) || (fi.flags & FSI_NOMEDIA))
        return;
    /* Groesse: Sektoren * Bytes je Sektor / 1024 */
    tot = fi.sectors;
    if (fi.bsize == 512) tot >>= 1;
    else if (fi.bsize == 2048) tot <<= 1;
    else if (fi.bsize == 4096) tot <<= 2;
    if (FAPI_FAILED(DosFreeSpaceKB(d, fr)))
        fr[1] = 0;
    frk = fr[1];
    if (frk > tot)
        tot = frk;
    used = tot - frk;
    {                                   /* used * 100 / tot (aufgerundet) */
        unsigned long t = tot, u = used;
        while (t > 65535UL) {
            t >>= 1;
            u >>= 1;
        }
        pct = t ? div16((u << 6) + (u << 5) + (u << 2) + t - 1, (unsigned)t) : 0;
    }
    putc((char)('A' + d));
    puts(":  ");
    if (o_t) {
        puts(fi.fsname);
        for (i = slen(fi.fsname); i < 9; i++)
            putc(' ');
    }
    sz(tot, 11);
    sz(used, 11);
    sz(frk, 11);
    put_num(pct, 5, 0);
    puts("%  ");
    lb.label[0] = 0;
    if (!FAPI_FAILED(DosLabel(d, &lb, 0)))
        puts(lb.label);
    if (fi.flags & FSI_REMOTE)
        puts(" (Netz)");
    puts("\r\n");
}

void main(void)
{
    char *a, w[16];
    unsigned d, any = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '/' && w[1] == '?') {
            puts("Zeigt Gr\x94\xE1" "e, Belegung und freien Platz der Laufwerke.\r\n\r\n"
                 "DF [-h] [-T] [Laufwerk: ...]\r\n\r\n"
                 "  -h  lesbare Gr\x94\xE1" "en (K, M, G)\r\n"
                 "  -T  Dateisystem zeigen\r\n");
            return;
        }
        if (w[0] == '-') {
            if (w[1] == 'h') o_h = 1;
            if (w[1] == 'T') o_t = 1;
        }
    }
    puts(o_t ? "LW  Typ             Gr\x94\xE1" "e     Belegt       Frei  Verw% Bezeichnung\r\n"
             : "LW         Gr\x94\xE1" "e     Belegt       Frei  Verw% Bezeichnung\r\n");
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '-' || w[0] == '/')
            continue;
        d = (unsigned)(up(w[0]) - 'A');
        if (d < 26) {
            one(d);
            any = 1;
        }
    }
    if (!any)
        for (d = 0; d < 26; d++)
            if (d >= 2)                 /* Disketten nur auf Wunsch      */
                one(d);
}
