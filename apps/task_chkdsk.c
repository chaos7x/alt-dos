/*
 * CHKDSK.EXE - Datentraeger pruefen, wie MS-DOS:
 *
 *   CHKDSK [Laufwerk:] [/F] [/V]
 *
 * CHKDSK selbst kennt kein Dateisystem. Es fragt den Kernel, welches
 * Dateisystem auf dem Laufwerk liegt (DosQFSInfo, z. B. "FAT") und
 * startet das passende Pruefprogramm U<Name>.EXE aus \DOS\SYSTEM (sonst
 * aus dem PATH) - wie
 * UFAT.DLL/UHPFS.DLL bei OS/2. Ein neues Dateisystem bringt also sein
 * .IFS und sein U<Name>.EXE mit, CHKDSK bleibt unveraendert.
 *
 * /F braucht das Laufwerk exklusiv. Geht das nicht (Dateien offen), bietet
 * CHKDSK wie Windows NT an, beim naechsten Start zu pruefen: Es traegt
 * das Laufwerk in \AUTOCHK.LST auf dem Startlaufwerk ein; COMMAND.EXE /P
 * arbeitet die Liste vor AUTOEXEC.CMD ab (CHKDSK X: /F /AUTO).
 *
 * ERRORLEVEL wie CHKDSK von Windows NT: 0 keine Fehler, 1 Fehler gefunden
 * und behoben, 3 Fehler gefunden und nicht behoben (ohne /F) oder nicht
 * pruefbar.
 */
#include "fapi.h"

static char args[FAPI_ARGLEN], cmd[FAPI_ARGLEN], path[FAPI_PATHLEN], prog[16];
static struct fapi_fsinfo fi;

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void cat(char *d, const char *s)
{
    d += slen(d);
    while ((*d++ = *s++) != 0)
        ;
}

/* Pruefprogramm im PATH suchen -> path; 0 = gefunden */
static int find_prog(void)
{
    static char pv[256];
    unsigned h, i = 0, k;

    /* zuerst fest im Systemordner des Startlaufwerks (wie OS/2 mit
       \OS2\DLL) - ein fremdes U*.EXE im PATH greift dann nicht */
    path[0] = 'C';
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", pv, sizeof(pv))) && pv[1] == ':')
        path[0] = up(pv[0]);
    path[1] = 0;
    cat(path, ":\\DOS\\SYSTEM\\");
    cat(path, prog);
    h = DosOpen(path);
    if (!FAPI_FAILED(h)) {
        DosClose(h);
        return 0;
    }
    if (FAPI_FAILED(DosGetEnv("PATH", pv, sizeof(pv))))
        pv[0] = 0;
    for (;;) {
        k = 0;
        while (pv[i] && pv[i] != ';' && k < sizeof(path) - 16)
            path[k++] = pv[i++];
        path[k] = 0;
        if (k && path[k - 1] != '\\')
            cat(path, "\\");
        cat(path, prog);
        h = DosOpen(path);
        if (!FAPI_FAILED(h)) {
            DosClose(h);
            return 0;
        }
        if (!pv[i])
            return -1;
        i++;
    }
}

static int yes(void)
{
    unsigned c;
    for (;;) {
        c = up((char)KbdCharIn());
        if (c == 'J' || c == 'Y') {
            puts("J\n");
            return 1;
        }
        if (c == 'N') {
            puts("N\n");
            return 0;
        }
    }
}

/* Laufwerk fuer den naechsten Start vormerken */
static void schedule(unsigned drive)
{
    static char lst[16], line[8];
    unsigned h;

    lst[0] = 'C';
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", path, sizeof(path))) && path[1] == ':')
        lst[0] = up(path[0]);
    lst[1] = 0;
    cat(lst, ":\\AUTOCHK.LST");
    line[0] = (char)('A' + drive);
    line[1] = 0;
    cat(line, ": /F\r\n");
    h = DosAppend(lst);
    if (FAPI_FAILED(h)) {
        puts("AUTOCHK.LST kann nicht geschrieben werden.\n");
        return;
    }
    DosWrite(h, line, slen(line));
    DosClose(h);
    puts("Das Laufwerk wird beim n\x84" "chsten Neustart gepr\x81" "ft.\n");
}

void main(void)
{
    char *a = args;
    unsigned drive = DosQCurDisk(), fix = 0, h, k;

    DosGetArgs(args);
    for (;;) {
        while (*a == ' ')
            a++;
        if (!*a)
            break;
        if (a[1] == ':' && up(a[0]) >= 'A' && up(a[0]) <= 'Z')
            drive = (unsigned)(up(a[0]) - 'A');
        else if (a[0] == '/' && up(a[1]) == 'F')
            fix = 1;
        else if (a[0] == '/' && a[1] == '?') {
            puts("Pr\x81" "ft einen Datentr\x84ger und zeigt einen Statusbericht an.\n\n"
                 "CHKDSK [Laufwerk:] [/F] [/V]\n\n"
                 "  /F  Fehler auf dem Datentr\x84ger beheben\n"
                 "  /V  jede Datei mit Pfad anzeigen\n\n"
                 "Gepr\x81" "ft wird mit U<Dateisystem>.EXE (z. B. UFAT.EXE) aus dem PATH.\n");
            return;
        }
        while (*a && *a != ' ')
            a++;
    }

    if (FAPI_FAILED(DosQFSInfo(drive, &fi))) {
        puts("Ung\x81ltiges Laufwerk.\n");
        DosExit(3);
    }
    prog[0] = 'U';
    prog[1] = 0;
    cat(prog, fi.fsname);
    cat(prog, ".EXE");
    if (find_prog()) {
        puts("F\x81r das Dateisystem ");
        puts(fi.fsname);
        puts(" gibt es kein Pr\x81" "fprogramm (");
        puts(prog);
        puts(").\n");
        DosExit(3);
    }

    if (fix) {                          /* Sperre vorab pruefen         */
        h = DosOpenDrive(drive, DRV_OPEN_LOCK);
        if (h == FAPI_ERR_WP) {
            puts("Der Datentr\x84ger ist schreibgesch\x81tzt.\n");
            DosExit(3);
        }
        if (FAPI_FAILED(h)) {
            puts("Das Laufwerk kann nicht gesperrt werden, weil Dateien darauf\n"
                 "ge\x94" "ffnet sind. Soll es beim n\x84" "chsten Neustart gepr\x81" "ft werden (J/N)? ");
            if (yes())
                schedule(drive);
            DosExit(3);
        }
        DosClose(h);
    }

    /* Pruefprogramm mit denselben Argumenten, Laufwerk ausdruecklich */
    cmd[0] = 0;
    cat(cmd, path);
    k = slen(cmd);
    cmd[k++] = ' ';
    cmd[k++] = (char)('A' + drive);
    cmd[k++] = ':';
    cmd[k] = 0;
    for (a = args; *a; ) {
        while (*a == ' ')
            a++;
        if (*a == '/') {
            k = slen(cmd);
            cmd[k++] = ' ';
            while (*a && *a != ' ' && k < sizeof(cmd) - 1)
                cmd[k++] = *a++;
            cmd[k] = 0;
        } else {
            while (*a && *a != ' ')
                a++;
        }
    }
    h = DosExecPgm(cmd, EXEC_SYNC);
    if (h) {
        puts(prog);
        puts(" kann nicht gestartet werden (");
        puts(h == EXEC_ERR_NOTFOUND ? "nicht gefunden" :
             h == EXEC_ERR_NOMEM ? "kein Speicher" :
             h == EXEC_ERR_FORMAT ? "ung\x81ltiges Format" :
             h == EXEC_ERR_NOSLOT ? "zu viele Programme" : "Fehler");
        puts("):\n  ");
        puts(cmd);
        puts("\n");
        DosExit(3);
    }
    DosExit(DosQChildRC());             /* Ergebnis von U<Name>.EXE    */
}
