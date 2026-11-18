/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DEVLOAD.EXE - Treiber und IFS-Module zur Laufzeit laden (wie modprobe):
 *   DEVLOAD Treiber [Argumente]     Treiber (.SYS) oder Dateisystem (.IFS)
 *   DEVLOAD /U Name                 Treiber entladen (wie rmmod)
 *   DEVLOAD /L                      geladene Treiber mit Laufwerken
 * Ueber FAPI 58h. Ein Name ohne Pfad wird im aktuellen Verzeichnis, dann
 * in X:\DOS\SYSTEM und X:\DOS des Startlaufwerks gesucht (X aus COMSPEC).
 * Neue Blockgeraete bekommen die naechsten freien Laufwerksbuchstaben.
 * ERRORLEVEL: 0 geladen, 1 Fehler, 2 kein Geraet gefunden, 4 Aufruf.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static char name[FAPI_PATHLEN], path[FAPI_PATHLEN], line[FAPI_ARGLEN + FAPI_PATHLEN];
static struct fapi_drvinfo di;

static void put_drives(unsigned long m)
{
    unsigned x, n = 0;
    for (x = 0; x < 26; x++)
        if (m & (1UL << x)) {
            putc(' ');
            putc((char)('A' + x));
            putc(':');
            n++;
        }
    if (!n)
        puts(" -");
}

static void list(void)
{
    unsigned i, n;
    puts("Treiber        Art      Ger\x84te  Laufwerke\r\n");
    for (i = 0; DosDrvInfo(i, &di) == 0; i++) {
        if (!di.kind)
            continue;
        puts(di.name);
        for (n = slen(di.name); n < 15; n++)
            putc(' ');
        puts(di.kind == 2 ? "IFS      " : "Treiber  ");
        put_num(di.units, 6, 0);
        puts("  ");
        put_drives(di.drives);
        puts("\r\n");
    }
}

static int exists(const char *p)
{
    unsigned h = DosOpen(p);
    if (h >= FAPI_ERR_FIRST)
        return 0;
    DosClose(h);
    return 1;
}

/* Datei suchen: wie angegeben, sonst X:\DOS\SYSTEM, X:\DOS */
static int find1(void);
static int find(void)                   /* ohne Endung: .SYS anhaengen  */
{
    char *b = base_name(name);
    unsigned k;
    if (find1())
        return 1;
    for (k = 0; b[k] && b[k] != '.'; k++)
        ;
    if (b[k] || slen(name) + 4 >= sizeof(name))
        return 0;
    scat(name, ".SYS", sizeof(name));
    return find1();
}

static int find1(void)
{
    char *b = base_name(name);
    if (abs_path(name, path) == 0 && exists(path))
        return 1;
    if (b != name)
        return 0;                       /* mit Pfad: nur dort            */
    if (DosGetEnv("COMSPEC", path, sizeof(path)) >= FAPI_ERR_FIRST || path[1] != ':')
        scopy(path, "C:", sizeof(path));
    path[2] = 0;
    scat(path, "\\DOS\\SYSTEM\\", sizeof(path));
    scat(path, name, sizeof(path));
    if (exists(path))
        return 1;
    path[2] = 0;
    scat(path, "\\DOS\\", sizeof(path));
    scat(path, name, sizeof(path));
    return exists(path);
}

void main(void)
{
    char *a, *rest;
    unsigned rc, n, ifs;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("L\x84" "dt einen Ger\x84tetreiber oder ein Dateisystem zur Laufzeit.\r\n\r\n"
             "DEVLOAD Treiber [Argumente]\r\n"
             "DEVLOAD /U Name\r\n"
             "DEVLOAD /L\r\n\r\n"
             "  Treiber  .SYS (wie DEVICE=) oder .IFS (wie IFS=); ohne Pfad\r\n"
             "           im aktuellen Verzeichnis, dann in \\DOS\\SYSTEM und \\DOS\r\n"
             "  /U       entl\x84" "dt einen Treiber (z. B. /U NCR8XX), wenn auf seinen\r\n"
             "           Laufwerken nichts offen ist\r\n"
             "  /L       geladene Treiber und ihre Laufwerke\r\n");
        return;
    }
    if (!*a || (a[0] == '/' && up(a[1]) == 'L')) {
        list();
        return;
    }
    if (a[0] == '/' && up(a[1]) == 'U') {
        word(skip(next_word(a)), name, sizeof(name));
        for (n = 0; name[n]; n++)
            name[n] = up(name[n]);
        if (!name[0]) {
            puts("Aufruf: DEVLOAD /U Name\r\n");
            DosExit(4);
        }
        rc = DosUnloadDriver(name);
        if (rc == 0) {
            puts(name);
            puts(" entladen.\r\n");
            return;
        }
        puts(name);
        puts(rc == FAPI_ERR_INUSE ? ": auf seinen Laufwerken ist noch etwas offen (Dateien,\r\n"
                                    "  aktuelles Laufwerk eines Programms, SUBST/JOIN), der Ton l\x84uft oder\r\n  TCPIP.SYS benutzt die Netzwerkkarte (erst DEVLOAD /U TCPIP)\r\n" :
             rc == FAPI_ERR_DENIED ? ": tr\x84gt das Startlaufwerk, bleibt geladen\r\n" :
             rc == FAPI_ERR_NOTREADY ? ": kann nicht entladen werden (der Treiber kennt es nicht)\r\n" :
             ": ist nicht geladen\r\n");
        DosExit(1);
    }
    rest = word(a, name, sizeof(name));
    rest = skip(rest);
    if (!find()) {
        puts(name);
        puts(": nicht gefunden\r\n");
        DosExit(1);
    }
    for (n = 0; path[n]; n++)           /* Name gross wie in CONFIG.SYS */
        path[n] = up(path[n]);
    ifs = n > 4 && same(path + n - 4, ".IFS");
    scopy(line, path, sizeof(line));
    if (*rest) {
        scat(line, " ", sizeof(line));
        scat(line, rest, sizeof(line));
    }
    rc = DosLoadDriver(line, ifs ? DRVL_IFS : 0);
    if (rc < FAPI_ERR_FIRST) {
        if (DosDrvInfo(rc, &di) == 0 && di.drives) {
            puts("Laufwerke:");
            put_drives(di.drives);
            puts("\r\n");
        }
        return;
    }
    puts(base_name(path));
    puts(rc == FAPI_ERR_EXIST ? ": ist schon geladen\r\n" :
         rc == FAPI_ERR_FULL ? ": kein Treiberplatz mehr frei\r\n" :
         rc == FAPI_ERR_INUSE ? ": es wird gerade ein anderer Treiber geladen\r\n" :
         rc == FAPI_ERR_NOTREADY ? ": nicht geladen\r\n" :
         ": konnte nicht geladen werden\r\n");
    DosExit(rc == FAPI_ERR_NOTREADY ? 2 : 1);
}
