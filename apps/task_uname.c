/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UNAME.EXE - System kurz, wie uname:  UNAME [-a] [-s] [-n] [-r] [-v] [-m]
 * -s Name (AT-DOS), -n Rechnername, -r Version, -v Build, -m Prozessor.
 * Ohne Schalter -s.
 */
#include "fapi.h"
#include "util.h"
#include "../build/version.h"

static char args[FAPI_ARGLEN], host[64];
static struct fapi_sysinfo si;

void main(void)
{
    char *a, *p;
    int s = 0, n = 0, r = 0, v = 0, m = 0, first = 1;
    unsigned ver;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Zeigt Systemname, Rechnername, Version und Prozessor.\r\n\r\n"
                 "UNAME [-a] [-s] [-n] [-r] [-v] [-m]\r\n");
            return;
        }
        if (a[0] != '-')
            continue;
        for (p = a + 1; *p > ' '; p++) {
            if (*p == 'a') s = n = r = v = m = 1;
            else if (*p == 's') s = 1;
            else if (*p == 'n') n = 1;
            else if (*p == 'r') r = 1;
            else if (*p == 'v') v = 1;
            else if (*p == 'm' || *p == 'p') m = 1;
        }
    }
    if (!(s | n | r | v | m))
        s = 1;
    if (s) {
        puts("AT-DOS");
        first = 0;
    }
    if (n) {
        if (!first) putc(' ');
        if (FAPI_FAILED(DosGetEnv("COMPUTERNAME", host, sizeof(host))) || !host[0])
            scopy(host, "ATDOS", sizeof(host));
        puts(host);
        first = 0;
    }
    if (r) {
        if (!first) putc(' ');
        ver = DosGetVersion();
        put_num(ver >> 8, 0, 0);
        putc('.');
        put_num((ver & 0xFF) / 10, 0, 0);
        first = 0;
    }
    if (v) {
        if (!first) putc(' ');
        puts("Build " ATDOS_BUILD " (" ATDOS_REV ", " ATDOS_DATE ")");
        first = 0;
    }
    if (m) {
        if (!first) putc(' ');
        DosSysInfo(&si);
        puts(si.cpu >= 3 ? "i386" : "i286");
    }
    puts("\r\n");
}
