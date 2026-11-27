/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DU.EXE - Belegung von Verzeichnissen, wie du (GNU):
 *
 *   DU [-s] [-a] [-h] [-c] [Verz ...]
 *
 * Groessen in KB (aufgerundet je Datei), -h lesbar (K, M, G), -s nur die
 * Summe je Angabe, -a auch Dateien, -c Gesamtsumme am Ende.
 */
#include "fapi.h"
#include "util.h"

#define MAXD 16
static char args[FAPI_ARGLEN], path[FAPI_PATHLEN];
static unsigned long sum[MAXD + 1], grand;
static int o_s, o_a, o_h, o_c;

static void size_out(unsigned long kb)
{
    if (o_h && kb >= 1024) {
        unsigned long m = kb >> 10;
        char u = 'M';
        if (m >= 1024) {
            m = (m + 512) >> 10;
            u = 'G';
        } else
            m = (kb + 512) >> 10;
        put_num(m, 6, 0);
        putc(u);
    } else {
        put_num(kb, o_h ? 6 : 10, 0);
        if (o_h)
            putc('K');
    }
    puts("\t");
}

static unsigned long kb_of(unsigned long bytes)
{
    return (bytes + 1023) >> 10;
}

static int visit(char *p, struct fapi_dirent *e, int what, int depth)
{
    if (depth >= MAXD)
        return 0;
    if (what == WT_DIR) {
        sum[depth + 1] = 0;
        return WT_DESCEND;
    }
    if (what == WT_FILE) {
        sum[depth] += kb_of(e->size);
        if (o_a && !o_s) {
            size_out(kb_of(e->size));
            puts(p);
            puts("\r\n");
        }
        return 0;
    }
    /* WT_LEAVE: Verzeichnis p fertig */
    if (!o_s) {
        size_out(sum[depth]);
        puts(p);
        puts("\r\n");
    }
    if (depth)
        sum[depth - 1] += sum[depth];
    return 0;
}

static void one(const char *w)
{
    if (abs_path(w, path) || !is_dir(path)) {
        puts("du: kein Verzeichnis: ");
        puts(w);
        puts("\r\n");
        return;
    }
    sum[0] = 0;
    walk_tree(path, visit);
    if (o_s) {
        size_out(sum[0]);
        puts(w);
        puts("\r\n");
    }
    grand += sum[0];
}

void main(void)
{
    char *a, w[FAPI_PATHLEN], *p;
    int any = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '/' && w[1] == '?') {
            puts("Zeigt die Belegung von Verzeichnissen.\r\n\r\n"
                 "DU [-s] [-a] [-h] [-c] [Verz ...]\r\n\r\n"
                 "  -s  nur die Summe je Verzeichnis\r\n"
                 "  -a  auch einzelne Dateien\r\n"
                 "  -h  lesbare Gr\x94\xE1" "en (K, M, G)\r\n"
                 "  -c  Gesamtsumme am Ende\r\n");
            return;
        }
        if (w[0] == '-' && w[1]) {
            for (p = w + 1; *p; p++) {
                if (*p == 's') o_s = 1;
                else if (*p == 'a') o_a = 1;
                else if (*p == 'h') o_h = 1;
                else if (*p == 'c') o_c = 1;
            }
            continue;
        }
    }
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '-' || (w[0] == '/' && w[1] == '?'))
            continue;
        one(w);
        any = 1;
    }
    if (!any)
        one(".");
    if (o_c) {
        size_out(grand);
        puts("insgesamt\r\n");
    }
}
