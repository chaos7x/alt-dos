/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UNDELETE.EXE - geloeschte Dateien wiederherstellen (wie MS-DOS 5):
 *
 *   UNDELETE [Laufwerk:][Pfad][Dateiname] [/LIST] [/ALL]
 *
 * Wie CHKDSK und FORMAT nur der Verteiler: die Arbeit macht das
 * Pruefprogramm des Dateisystems (UFAT.EXE X: /UNDELETE \Pfad\Muster).
 * Ohne /LIST braucht es das Laufwerk exklusiv (keine offenen Dateien).
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], cmd[FAPI_ARGLEN], spec[FAPI_PATHLEN], full[FAPI_PATHLEN];
static char path[FAPI_PATHLEN], prog[16], cs[FAPI_PATHLEN];
static struct fapi_fsinfo fi;

/* U<FS>.EXE im Systemordner des Startlaufwerks */
static int find_prog(void)
{
    unsigned h;
    path[0] = 'C';
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
        path[0] = up(cs[0]);
    path[1] = 0;
    scat(path, ":\\DOS\\SYSTEM\\", sizeof(path));
    scat(path, prog, sizeof(path));
    h = DosOpen(path);
    if (FAPI_FAILED(h))
        return -1;
    DosClose(h);
    return 0;
}

void main(void)
{
    char *a;
    int list = 0, all = 0;
    unsigned d, k;

    DosGetArgs(args);
    spec[0] = 0;
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Stellt gel\x94schte Dateien wieder her.\r\n\r\n"
                 "UNDELETE [Laufwerk:][Pfad][Dateiname] [/LIST] [/ALL]\r\n\r\n"
                 "  /LIST  zeigt die gel\x94schten Dateien nur an\r\n"
                 "  /ALL   stellt alles ohne R\x81" "ckfrage wieder her (erstes Zeichen #)\r\n");
            return;
        }
        if (a[0] == '/' && up(a[1]) == 'L')
            list = 1;
        else if (a[0] == '/' && up(a[1]) == 'A')
            all = 1;
        else if (a[0] == '/') {
            puts("Ung\x81ltiger Parameter - ");
            puts(a);
            puts("\r\n");
            DosExit(1);
        } else
            word(a, spec, sizeof(spec));
    }
    if (!spec[0])
        scopy(spec, "*.*", sizeof(spec));
    if (abs_path(spec, full)) {
        puts("Ung\x81ltiger Pfad.\r\n");
        DosExit(1);
    }
    if (is_dir(full))                       /* Verzeichnis: alle Dateien */
    {
        join(spec, full, "*.*", sizeof(spec));
        scopy(full, spec, sizeof(full));
    }
    d = (unsigned)(up(full[0]) - 'A');
    if (DosQFSInfo(d, &fi)) {
        puts("Ung\x81ltiges Laufwerk.\r\n");
        DosExit(1);
    }
    if (fi.flags & FSI_READONLY) {
        puts("Auf diesem Laufwerk kann nichts wiederhergestellt werden.\r\n");
        DosExit(1);
    }
    scopy(prog, "U", sizeof(prog));
    scat(prog, fi.fsname, sizeof(prog));
    scat(prog, ".EXE", sizeof(prog));
    if (find_prog()) {
        puts("F\x81r dieses Dateisystem gibt es kein UNDELETE (");
        puts(prog);
        puts(" fehlt).\r\n");
        DosExit(1);
    }
    scopy(cmd, path, sizeof(cmd));
    k = slen(cmd);
    cmd[k++] = ' ';
    cmd[k++] = full[0];
    cmd[k++] = ':';
    cmd[k] = 0;
    scat(cmd, " /UNDELETE ", sizeof(cmd));
    scat(cmd, full + 2, sizeof(cmd));
    if (list)
        scat(cmd, " /LIST", sizeof(cmd));
    if (all)
        scat(cmd, " /ALL", sizeof(cmd));
    if (DosExecPgm(cmd, EXEC_SYNC)) {
        puts("Pr\x81" "fprogramm kann nicht gestartet werden.\r\n");
        DosExit(1);
    }
    DosExit(DosQChildRC());
}
