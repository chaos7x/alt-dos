/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PSTREE.EXE - laufende Programme als Baum, wie pstree:
 *
 *   PSTREE [-p] [-a]   -p PIDs zeigen, -a mit Befehlszeile
 *
 * Wurzeln sind die Programme ohne Aufrufer (Shell jeder Sitzung, Dienste
 * aus RUN=, losgeloeste Programme); davor steht die Sitzung.
 */
#include "fapi.h"
#include "util.h"
#define PR_NONUM
#include "proc.h"

static char args[FAPI_ARGLEN], pre[80];
static int show_pid, show_args;

static void node(unsigned t, unsigned depth, int last)
{
    unsigned c, n, k, i = 0;
    unsigned kids[NP];
    unsigned plen = slen(pre);

    if (depth) {
        puts(pre);
        puts(last ? "\xC0\xC4" : "\xC3\xC4");
    }
    puts(pt[t].p.name);
    if (show_pid) {
        puts("(");
        putc((char)('0' + t));
        puts(")");
    }
    if (show_args && pt[t].p.args[0]) {
        puts(" ");
        puts(skip(pt[t].p.args));
    }
    if (pt[t].p.kind)
        puts(pt[t].p.kind == 2 ? " [DOS-Box, ruht]" : " [DOS-Box]");
    puts("\r\n");
    for (c = 0, n = 0; c < NP; c++)
        if (pu[c] && c != t && pt[c].p.parent == t)
            kids[n++] = c;
    if (depth && plen < sizeof(pre) - 3)
        scat(pre, last ? "  " : "\xB3 ", sizeof(pre));
    for (k = 0; k < n; k++, i++)
        node(kids[k], depth + 1, k == n - 1);
    pre[plen] = 0;
}

void main(void)
{
    char w[16], *a;
    unsigned t, s;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '/' && w[1] == '?') {
            puts("Zeigt laufende Programme als Baum.\r\n\r\n"
                 "PSTREE [-p] [-a]\r\n\r\n"
                 "  -p  PIDs zeigen\r\n"
                 "  -a  mit Befehlszeile\r\n");
            return;
        }
        if (w[0] == '-') {
            for (s = 1; w[s]; s++) {
                if (w[s] == 'p') show_pid = 1;
                if (w[s] == 'a') show_args = 1;
            }
        }
    }
    pr_read();
    for (s = 0; s < NP; s++)            /* je Sitzung die Wurzeln        */
        for (t = 0; t < NP; t++)
            if (pu[t] && pt[t].p.session == s &&
                (pt[t].p.parent >= NP || !pu[pt[t].p.parent] ||
                 pt[t].p.parent == t)) {
                puts("S");
                putc((char)('1' + s));
                puts(" ");
                pre[0] = 0;
                scopy(pre, "   ", sizeof(pre));
                node(t, 0, 1);
            }
}
