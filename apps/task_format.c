/*
 * FORMAT.EXE - Datentraeger formatieren (wie MS-DOS, Aufbau wie OS/2):
 *
 *   FORMAT Laufwerk: [/V[:Bezeichnung]] [/Q] [/S] [/FS:Dateisystem]
 *
 * FORMAT selbst kennt kein Dateisystem: Es startet U<Dateisystem>.EXE
 * (Vorgabe FAT -> UFAT.EXE) aus \DOS\SYSTEM mit /FORMAT - wie CHKDSK.
 * /S macht den Datentraeger danach mit SYS startfaehig. Das Startlaufwerk
 * wird nicht formatiert.
 * ERRORLEVEL: 0 ok, 3 abgebrochen/Fehler, 4 falscher Aufruf.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], cmd[FAPI_ARGLEN], fs[12] = "FAT", lbl[16];
static char path[FAPI_PATHLEN], cs[FAPI_PATHLEN];
static struct fapi_fsinfo fi;

/* Programm im Systemordner des Startlaufwerks: <sys>\DOS\<sub><name> */
static int sys_prog(char *out, const char *sub, const char *name)
{
    unsigned h;
    out[0] = 'C';
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
        out[0] = up(cs[0]);
    out[1] = 0;
    scat(out, ":\\DOS\\", FAPI_PATHLEN);
    scat(out, sub, FAPI_PATHLEN);
    scat(out, name, FAPI_PATHLEN);
    h = DosOpen(out);
    if (FAPI_FAILED(h))
        return -1;
    DosClose(h);
    return 0;
}

void main(void)
{
    static char prog[16];
    char *a;
    unsigned d = 0xFFFF, k, r;
    int quick = 0, sys = 0, vlab = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[1] == ':' && a[0] != '/')
            d = (unsigned)(up(a[0]) - 'A');
        else if (a[0] == '/' && up(a[1]) == 'Q')
            quick = 1;
        else if (a[0] == '/' && up(a[1]) == 'S')
            sys = 1;
        else if (a[0] == '/' && up(a[1]) == 'V') {
            vlab = 1;
            if (a[2] == ':')
                for (k = 0; a[3 + k] && a[3 + k] != ' ' && k < 11; k++)
                    lbl[k] = up(a[3 + k]);
        } else if (a[0] == '/' && up(a[1]) == 'F' && up(a[2]) == 'S' && a[3] == ':') {
            for (k = 0; a[4 + k] && a[4 + k] != ' ' && k < sizeof(fs) - 1; k++)
                fs[k] = up(a[4 + k]);
            fs[k] = 0;
        } else if (a[0] == '/' && a[1] == '?') {
            puts("Formatiert einen Datentr\x84ger.\r\n\r\n"
                 "FORMAT Laufwerk: [/V[:Bezeichnung]] [/Q] [/S] [/FS:Dateisystem]\r\n\r\n"
                 "  /V  Datentr\x84gerbezeichnung      /Q  Schnellformatierung\r\n"
                 "  /S  Systemdateien \x81" "bertragen  /FS Dateisystem (Vorgabe FAT)\r\n");
            return;
        } else {
            puts("Ung\x81ltiger Parameter - ");
            puts(a);
            puts("\r\n");
            DosExit(4);
        }
    }
    if (d == 0xFFFF) {
        puts("Laufwerksangabe fehlt.\r\n");
        DosExit(4);
    }
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':' &&
        (unsigned)(up(cs[0]) - 'A') == d) {
        puts("Das Startlaufwerk kann nicht formatiert werden.\r\n");
        DosExit(3);
    }
    if (DosQFSInfo(d, &fi) || (fi.flags & FSI_READONLY)) {
        puts("Ung\x81ltiges Laufwerk oder schreibgesch\x81tzt.\r\n");
        DosExit(3);
    }
    scopy(prog, "U", sizeof(prog));
    scat(prog, fs, sizeof(prog));
    scat(prog, ".EXE", sizeof(prog));
    if (sys_prog(path, "SYSTEM\\", prog)) {
        puts("Das Dateisystem ");
        puts(fs);
        puts(" kann nicht angelegt werden (");
        puts(prog);
        puts(" fehlt).\r\n");
        DosExit(3);
    }
    if (fi.flags & FSI_REMOVABLE) {
        puts("Neue Diskette in Laufwerk ");
        putc((char)('A' + d));
        puts(": einlegen\r\nund danach die EINGABETASTE dr\x81" "cken...");
        while ((k = KbdCharIn() & 0xFF) != 13)
            if (k == 0 || k == 27) {
                puts("\r\n");
                DosExit(3);
            }
        puts("\r\n");
    } else {
        puts("ACHTUNG: ALLE DATEN AUF DEM LAUFWERK ");
        putc((char)('A' + d));
        puts(": GEHEN VERLOREN!\r\nFormatierung durchf\x81hren (J/N)? ");
        if (!ask_yn())
            DosExit(3);
    }
    puts(quick ? "Schnellformatierung ...\r\n" : "Formatierung ...\r\n");
    scopy(cmd, path, sizeof(cmd));
    k = slen(cmd);
    cmd[k++] = ' ';
    cmd[k++] = (char)('A' + d);
    cmd[k++] = ':';
    cmd[k] = 0;
    scat(cmd, " /FORMAT", sizeof(cmd));
    if (quick)
        scat(cmd, " /Q", sizeof(cmd));
    if (vlab && lbl[0]) {
        scat(cmd, " /V:", sizeof(cmd));
        scat(cmd, lbl, sizeof(cmd));
    }
    r = DosExecPgm(cmd, EXEC_SYNC);
    if (r || DosQChildRC()) {
        if (r)
            puts("Pr\x81" "fprogramm kann nicht gestartet werden.\r\n");
        DosExit(3);
    }
    if (sys) {
        if (sys_prog(path, "", "SYS.EXE")) {
            puts("SYS.EXE fehlt - System nicht \x81" "bertragen.\r\n");
            DosExit(3);
        }
        k = slen(path);
        path[k++] = ' ';
        path[k++] = (char)('A' + d);
        path[k++] = ':';
        path[k] = 0;
        if (DosExecPgm(path, EXEC_SYNC) || DosQChildRC())
            DosExit(3);
    }
}
