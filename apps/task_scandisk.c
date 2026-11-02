/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SCANDISK.EXE - fuer MS-DOS-6.2-Gewohnheit: SCANDISK [Laufwerk:] ruft
 * CHKDSK Laufwerk: /F /R auf (Dateisystem pruefen und Oberflaechentest).
 * Die eigentliche Arbeit steckt wie bei Windows NT in CHKDSK /R.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], cmd[FAPI_ARGLEN], cs[FAPI_PATHLEN];

void main(void)
{
    char *a;
    unsigned d = DosQCurDisk(), k, r;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Pr\x81" "ft einen Datentr\x84ger und sucht fehlerhafte Sektoren.\r\n\r\n"
                 "SCANDISK [Laufwerk:]\r\n\r\n"
                 "Entspricht CHKDSK Laufwerk: /F /R.\r\n");
            return;
        }
        if (a[1] == ':' && up(a[0]) >= 'A' && up(a[0]) <= 'Z')
            d = (unsigned)(up(a[0]) - 'A');
    }
    scopy(cmd, "C:\\DOS\\CHKDSK.EXE ", sizeof(cmd));   /* Startlaufwerk */
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
        cmd[0] = up(cs[0]);
    k = slen(cmd);
    cmd[k++] = (char)('A' + d);
    cmd[k] = 0;
    scat(cmd, ": /F /R", sizeof(cmd));
    r = DosExecPgm(cmd, EXEC_SYNC);
    if (r) {
        puts("CHKDSK.EXE kann nicht gestartet werden.\r\n");
        DosExit(3);
    }
    DosExit(DosQChildRC());
}
