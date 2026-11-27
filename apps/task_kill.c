/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * KILL.EXE - Programme beenden, Bedienung wie kill/killall:
 *
 *   KILL PID|Name ...      beenden (Name ohne Endung: alle dieses Namens)
 *   KILL -9 | -KILL | -s SIGNAL ...   angenommen, wirkt gleich
 *   KILL -l                Signale zeigen
 *
 * AT-DOS kennt kein Signal an fremde Tasks: beendet wird immer sofort wie
 * mit Strg+C (Rueckgabewert 255), mitsamt allem, was der Task gestartet
 * hat (wie TASKMAN). Nicht beendbar: KILL selbst, seine Aufrufer und die
 * Shell der ersten Sitzung. ERRORLEVEL 1, wenn einer nicht ging.
 */
#include "fapi.h"
#include "util.h"
#define PR_NAMES
#define PR_KILL
#define PR_NONUM
#include "proc.h"

static char args[FAPI_ARGLEN];

static void usage(void)
{
    puts("Beendet Programme.\r\n\r\n"
         "KILL [-9|-15|-s Signal] PID|Name ...\r\n"
         "KILL -l\r\n\r\n"
         "  PID    Nummer aus PS oder TASKMAN\r\n"
         "  Name   Programmname (ohne Endung), beendet alle dieses Namens\r\n\r\n"
         "Beendet wird immer sofort und mit allem, was das Programm gestartet hat.\r\n");
}

static int kill_one(unsigned t)
{
    unsigned r = pr_kill(t);
    if (r == FAPI_ERR_DENIED) {
        puts("kill: (");
        putc((char)('0' + t));
        puts(") darf nicht beendet werden\r\n");
        return 1;
    }
    if (r) {
        puts("kill: (");
        putc((char)('0' + t));
        puts(") - kein solcher Prozess\r\n");
        return 1;
    }
    return 0;
}

void main(void)
{
    char w[64], *a;
    unsigned t;
    int pid, rc = 0, any = 0, skip_next = 0, hit;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (skip_next) {
            skip_next = 0;
            continue;
        }
        if ((w[0] == '/' && w[1] == '?') || same(w, "--help")) {
            usage();
            return;
        }
        if (w[0] == '-' && w[1] == 'l' && w[2] == 0) {
            puts(" 1) HUP   2) INT   9) KILL  15) TERM\r\n"
                 "(AT-DOS beendet bei jedem Signal sofort)\r\n");
            return;
        }
        if (w[0] == '-' && w[1] == 's' && w[2] == 0) {
            skip_next = 1;              /* Signalname                   */
            continue;
        }
        if (w[0] == '-')
            continue;                   /* -9, -KILL, -TERM ...         */
        any = 1;
        pr_read();
        if ((pid = pr_pid(w)) >= 0) {
            if (!pu[pid]) {
                puts("kill: (");
                puts(w);
                puts(") - kein solcher Prozess\r\n");
                rc = 1;
            } else
                rc |= kill_one((unsigned)pid);
            continue;
        }
        hit = 0;
        for (t = 0; t < NP; t++) {
            pr_read();
            if (pu[t] && pr_name(t, w) && !(pt[t].p.flags & PROC_SELF)) {
                hit = 1;
                rc |= kill_one(t);
            }
        }
        if (!hit) {
            puts("kill: ");
            puts(w);
            puts(": kein Prozess gefunden\r\n");
            rc = 1;
        }
    }
    if (!any) {
        usage();
        rc = 1;
    }
    DosExit((unsigned)rc);
}
