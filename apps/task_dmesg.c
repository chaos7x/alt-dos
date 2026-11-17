/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DMESG.EXE - Meldungen des Kernels zeigen (nach dem Vorbild von dmesg):
 *
 *   DMESG [-c] [-C] [-n Zeilen]
 *
 * Der Kernel schreibt alles, was er auf dem Bildschirm meldet (Start,
 * Treiber, CONFIG.SYS, abgebrochene Programme und DOS-Boxen, Herunter-
 * fahren), mit der Zeit seit dem Start in einen Ring von 8 KB (FAPI_KLOG).
 * -c zeigt und loescht danach, -C loescht nur, -n zeigt nur die letzten
 * Zeilen. Umleiten wie gewohnt: DMESG > LOG.TXT, DMESG | GREP Fehler.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static char log[KLOG_SIZE];

#define DosKlog(buf, len, pos) fapi_call(FAPI_KLOG, (unsigned)(buf), (len), (pos))

void main(void)
{
    char *a, *o;
    unsigned n, i, k, tail = 0, o_c = 0, o_C = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Zeigt die Meldungen des Kernels (nach dem Vorbild von dmesg).\r\n\r\n"
                 "DMESG [-c] [-C] [-n Zeilen]\r\n\r\n"
                 "  -c  zeigen, danach l\x94schen     -C  nur l\x94schen\r\n"
                 "  -n  nur die letzten n Zeilen\r\n"
                 "Der Kernel h\x84lt die letzten 8 KB seiner Meldungen (Start, Treiber,\r\n"
                 "abgebrochene Programme und DOS-Boxen) mit der Zeit seit dem Start.\r\n");
            return;
        }
        if (a[0] != '-' || !a[1] || a[1] == ' ')
            goto bad;
        for (o = a + 1; *o && *o != ' '; o++)
            switch (*o) {
            case 'c': o_c = 1; break;
            case 'C': o_C = 1; break;
            case 'n':
                if (o[1] && o[1] != ' ')
                    o++;
                else {
                    a = next_word(a);
                    o = a;
                }
                for (tail = 0; *o >= '0' && *o <= '9'; o++)
                    tail = tail * 10 + (*o - '0');
                if (!tail || (*o && *o != ' '))
                    goto bad;
                o--;
                break;
            default:
                goto bad;
            }
    }
    if (!o_C) {
        n = DosKlog(log, sizeof(log), 0);
        if (n == FAPI_ERROR)
            n = 0;
        i = 0;
        if (tail) {                     /* letzte Zeilen: von hinten     */
            k = 0;
            for (i = n; i > 0; i--)
                if (log[i - 1] == '\n' && i != n && ++k == tail)
                    break;
        }
        if (i < n)
            DosWrite(STDOUT, log + i, n - i);
    }
    if (o_c || o_C)
        DosKlog(0, 0, 0xFFFF);
    DosExit(0);
bad:
    puts("dmesg: ung\x81ltiger Schalter - ");
    puts(a);
    puts("\r\n");
    DosExit(1);
}
