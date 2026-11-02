/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DEFRAG.EXE - Dateien zusammenhaengend legen (wie MS-DOS 6):
 *
 *   DEFRAG [Laufwerk:] [/F | /U] [/V]
 *
 * /F (Vorgabe) legt fragmentierte Dateien zusammen und schiebt Dateien in
 * Luecken weiter vorn, /U legt nur fragmentierte Dateien zusammen. Wie
 * CHKDSK nur der Verteiler: die Arbeit macht U<Dateisystem>.EXE /DEFRAG.
 * Das Laufwerk muss sich sperren lassen (keine offenen Dateien).
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], cmd[FAPI_ARGLEN], path[FAPI_PATHLEN], prog[16], cs[FAPI_PATHLEN];
static struct fapi_fsinfo fi;

void main(void)
{
    char *a;
    unsigned d = DosQCurDisk(), k, h;
    int only = 0, verb = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Ordnet Dateien auf einem Datentr\x84ger neu an.\r\n\r\n"
                 "DEFRAG [Laufwerk:] [/F | /U] [/V]\r\n\r\n"
                 "  /F  Dateien zusammenlegen und L\x81" "cken schlie\xE1" "en (Vorgabe)\r\n"
                 "  /U  nur fragmentierte Dateien zusammenlegen\r\n"
                 "  /V  jede verschobene Datei melden\r\n");
            return;
        }
        if (a[1] == ':' && up(a[0]) >= 'A' && up(a[0]) <= 'Z')
            d = (unsigned)(up(a[0]) - 'A');
        else if (a[0] == '/' && up(a[1]) == 'U')
            only = 1;
        else if (a[0] == '/' && up(a[1]) == 'V')
            verb = 1;
        else if (!(a[0] == '/' && up(a[1]) == 'F')) {
            puts("Ung\x81ltiger Parameter - ");
            puts(a);
            puts("\r\n");
            DosExit(1);
        }
    }
    if (DosQFSInfo(d, &fi) || (fi.flags & FSI_READONLY)) {
        puts("Dieses Laufwerk kann nicht defragmentiert werden.\r\n");
        DosExit(1);
    }
    h = DosOpenDrive(d, DRV_OPEN_LOCK);
    if (FAPI_FAILED(h)) {
        puts(h == FAPI_ERR_WP ? "Der Datentr\x84ger ist schreibgesch\x81tzt.\r\n" :
             "Das Laufwerk kann nicht gesperrt werden (Dateien ge\x94" "ffnet?).\r\n");
        DosExit(1);
    }
    DosClose(h);
    scopy(prog, "U", sizeof(prog));
    scat(prog, fi.fsname, sizeof(prog));
    scat(prog, ".EXE", sizeof(prog));
    path[0] = 'C';
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
        path[0] = up(cs[0]);
    path[1] = 0;
    scat(path, ":\\DOS\\SYSTEM\\", sizeof(path));
    scat(path, prog, sizeof(path));
    h = DosOpen(path);
    if (FAPI_FAILED(h)) {
        puts("F\x81r dieses Dateisystem gibt es kein DEFRAG (");
        puts(prog);
        puts(" fehlt).\r\n");
        DosExit(1);
    }
    DosClose(h);
    scopy(cmd, path, sizeof(cmd));
    k = slen(cmd);
    cmd[k++] = ' ';
    cmd[k++] = (char)('A' + d);
    cmd[k++] = ':';
    cmd[k] = 0;
    scat(cmd, " /DEFRAG", sizeof(cmd));
    if (only)
        scat(cmd, " /U", sizeof(cmd));
    if (verb)
        scat(cmd, " /V", sizeof(cmd));
    puts("Laufwerk ");
    putc((char)('A' + d));
    puts(": wird defragmentiert ...\r\n");
    if (DosExecPgm(cmd, EXEC_SYNC)) {
        puts("Pr\x81" "fprogramm kann nicht gestartet werden.\r\n");
        DosExit(1);
    }
    DosExit(DosQChildRC());
}
