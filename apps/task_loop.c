/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LOOP.EXE - Datei als Laufwerk einhaengen (wie mount -o loop):
 *   LOOP [X:] Datei [/RO] ISO, Disketten-/Plattenabbild (IMG, IMA), feste VHD
 *   LOOP X: /D           aushaengen
 *   LOOP                 eingehaengte Dateien zeigen
 * Ueber FAPI 59h. Abbilder sind beschreibbar (Stufe 2), ausser mit /RO,
 * schreibgeschuetzter Datei, ISO oder Datei auf CD. Ohne X: der naechste freie
 * Buchstabe; ein Plattenabbild bekommt je Partition einen.
 * ERRORLEVEL: 0 ok, 1 Fehler, 4 Aufruf.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN], path[FAPI_PATHLEN];
static struct fapi_loopinfo li;

static unsigned put_drives(unsigned long m)  /* -> Zeichen            */
{
    unsigned x, n = 0;
    for (x = 0; x < 26; x++)
        if (m & (1UL << x)) {
            putc((char)('A' + x));
            putc(':');
            putc(' ');
            n += 3;
        }
    return n;
}

static void list(void)
{
    unsigned i, n = 0, k, kd;
    static const char *kind[6] = { "?", "ISO", "Abbild", "Platte", "VHD", "gepackt" };
    for (i = 0; DosLoop(LOOP_INFO, i, &li) == 0; i++) {
        if (!li.drives)
            continue;
        if (!n++)
            puts("Laufwerk  Art       Zugriff   Bereiche  Datei\r\n");
        for (k = put_drives(li.drives); k < 10; k++)
            putc(' ');
        kd = li.kind & ~LOOPK_RO;
        puts(kind[kd < 6 ? kd : 0]);
        for (k = slen(kind[kd < 6 ? kd : 0]); k < 10; k++)
            putc(' ');
        puts(li.kind & LOOPK_RO ? "nur lesen " : "schreiben ");
        put_num(li.extents, 8, 0);
        puts("  ");
        putc((char)('A' + li.cdrive));
        putc(':');
        puts(li.path);
        puts("\r\n");
    }
    if (!n)
        puts("Keine Datei als Laufwerk eingeh\x84ngt.\r\n");
}

static void usage(void)
{
    puts("H\x84ngt eine Datei als Laufwerk ein.\r\n\r\n"
         "LOOP [X:] Datei [/RO]\r\n"
         "LOOP X: /D\r\n"
         "LOOP\r\n\r\n"
         "  Datei  ISO (CD-Abbild), Disketten- oder Plattenabbild (IMG, IMA),\r\n"
         "         feste VHD, DoubleSpace/DriveSpace-Datei (DBLSPACE.nnn,\r\n"
         "         DRVSPACE.nnn, nur lesend)\r\n"
         "  X:     Laufwerksbuchstabe, ohne Angabe der n\x84" "chste freie\r\n"
         "  /RO    nur lesend (ISO und Dateien auf CD immer)\r\n"
         "  /D     h\x84ngt das Laufwerk wieder aus\r\n"
         "In CONFIG.SYS: LOOP=[X:,]Datei[,/RO] (bis zu vier Zeilen)\r\n");
}

void main(void)
{
    char *a, *rest;
    unsigned rc, drive = 0, ro = 0;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        usage();
        return;
    }
    if (!*a) {
        list();
        return;
    }
    if (a[1] == ':' && (a[2] == 0 || a[2] == ' ')) {
        drive = up(a[0]) - 'A';
        if (drive < 2 || drive > 25) {
            puts("Ung\x81ltiges Laufwerk\r\n");
            DosExit(4);
        }
        a = skip(next_word(a));
    }
    if (a[0] == '/' && up(a[1]) == 'D') {
        if (!drive) {
            puts("Aufruf: LOOP X: /D\r\n");
            DosExit(4);
        }
        rc = DosLoop(LOOP_DETACH, drive, 0);
        if (rc == 0) {
            putc((char)('A' + drive));
            puts(": ausgeh\x84ngt.\r\n");
            return;
        }
        putc((char)('A' + drive));
        puts(rc == FAPI_ERR_INUSE ? ": noch in Benutzung (offene Dateien, aktuelles Laufwerk,\r\n"
                                    "  SUBST/JOIN oder eine Datei darauf eingeh\x84ngt)\r\n" :
             ": ist kein eingeh\x84ngtes Abbild\r\n");
        DosExit(1);
    }
    rest = skip(word(a, name, sizeof(name)));
    if (rest[0] == '/' && up(rest[1]) == 'R' && up(rest[2]) == 'O') {
        ro = LOOP_RO;
        rest = skip(next_word(rest));
    }
    if (!name[0] || *rest) {
        usage();
        DosExit(4);
    }
    if (abs_path(name, path))
        scopy(path, name, sizeof(path));
    rc = DosLoop(LOOP_ATTACH, path, drive | ro);
    if (rc < FAPI_ERR_FIRST) {
        unsigned i;
        for (i = 0; DosLoop(LOOP_INFO, i, &li) == 0; i++)
            if (li.drives & (1UL << rc)) {
                puts(base_name(path));
                puts(" -> ");
                put_drives(li.drives);
                puts("\r\n");
                return;
            }
        return;
    }
    puts(base_name(path));
    puts(rc == FAPI_ERR_INUSE ? ": wird gerade geschrieben\r\n" :
         rc == FAPI_ERR_EXIST ? ": der Laufwerksbuchstabe ist belegt\r\n" :
         rc == FAPI_ERR_FULL ? ": kein Platz mehr frei (h\x94" "chstens 4 Abbilder)\r\n" :
         rc == FAPI_ERR_FRAG ? ": Datei zu stark zerst\x81" "ckelt - DEFRAG oder kopieren\r\n" :
         rc == FAPI_ERR_DENIED ? ": geht nicht (Datei nicht auf FAT oder CD, dynamische VHD)\r\n" :
         rc == FAPI_ERR_NOTREADY ? ": kein Dateisystem im Abbild gefunden\r\n" :
         ": nicht gefunden\r\n");
    DosExit(1);
}
