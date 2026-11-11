/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SETVER.EXE - Versionstabelle fuer DOS-Programme in den DOS-Boxen
 * (Bedienung wie MS-DOS 5/6):
 *
 *   SETVER [Laufwerk:Pfad]                  Tabelle anzeigen
 *   SETVER [Laufwerk:Pfad] Datei n.nn       Eintrag setzen
 *   SETVER [Laufwerk:Pfad] Datei /DELETE [/QUIET]
 *
 * Die Tabelle steht als Text in X:\DOS\MDOS\SETVER.LST des Startlaufwerks
 * ("NAME.EXT n.nn" je Zeile). BOXSHELL laedt sie beim Start jeder Box
 * und haengt sie in die Liste der Listen des DOS (+37h, wie der
 * SETVER-Treiber von MS-DOS) - das DOS meldet einem so gestarteten
 * Programm auf INT 21h 30h die eingetragene Version. Aenderungen gelten
 * fuer danach gestartete Boxen. Laeuft unter AT-DOS und (gebunden) in
 * der Box; das Startlaufwerk kommt aus %DOSBOX% (setzt der Kernel bzw.
 * BOXSHELL), sonst aus %COMSPEC%.
 * Ein Laufwerk/Pfad vorn nimmt SETVER wie MS-DOS an und uebergeht ihn.
 */
#include "fapi.h"
#include "util.h"

#define NENT  48                /* BOXSHELL liest bis 1023 Byte */
#define TXT   2048

static char args[FAPI_ARGLEN], file[80], ev[128];
static char txt[TXT];
static char names[NENT][13];
static unsigned char vmaj[NENT], vmin[NENT];
static unsigned n;

static void list_path(void)
{
    char d = 'C';
    if (!FAPI_FAILED(DosGetEnv("DOSBOX", ev, sizeof(ev))) && ev[1] == ':')
        d = up(ev[0]);
    else if (!FAPI_FAILED(DosGetEnv("COMSPEC", ev, sizeof(ev))) && ev[1] == ':')
        d = up(ev[0]);
    file[0] = d;
    file[1] = 0;
    scat(file, ":\\DOS\\MDOS\\SETVER.LST", sizeof(file));
}

/* "n.nn" -> 0 ok */
static int parse_ver(const char *p, unsigned char *ma, unsigned char *mi)
{
    unsigned a = 0, b = 0, k = 0;
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9')
        a = a * 10 + (unsigned)(*p++ - '0');
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && k < 2) {
            b = b * 10 + (unsigned)(*p++ - '0');
            k++;
        }
        if (k == 1)
            b *= 10;                            /* 3.3 = 3.30            */
        if (*p >= '0' && *p <= '9')
            return -1;
    }
    if (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        return -1;
    if (a < 2 || a > 9)
        return -1;
    *ma = (unsigned char)a;
    *mi = (unsigned char)b;
    return 0;
}

static void load(void)
{
    unsigned h, len, i = 0, k;
    n = 0;
    h = DosOpen(file);
    if (FAPI_FAILED(h))
        return;
    len = DosRead(h, txt, TXT - 1);
    DosClose(h);
    if (FAPI_FAILED(len))
        return;
    txt[len] = 0;
    while (txt[i] && n < NENT) {
        while (txt[i] == ' ' || txt[i] == '\t' || txt[i] == '\r' || txt[i] == '\n')
            i++;
        if (!txt[i])
            break;
        if (txt[i] == ';') {
            while (txt[i] && txt[i] != '\n')
                i++;
            continue;
        }
        for (k = 0; txt[i] && txt[i] != ' ' && txt[i] != '\t' &&
                    txt[i] != '\r' && txt[i] != '\n'; i++)
            if (k < 12)
                names[n][k++] = up(txt[i]);
        names[n][k] = 0;
        while (txt[i] == ' ' || txt[i] == '\t')
            i++;
        if (parse_ver(txt + i, &vmaj[n], &vmin[n]) == 0)
            n++;
        while (txt[i] && txt[i] != '\n')
            i++;
    }
}

static void put2(unsigned v)
{
    putc((char)('0' + v / 10));
    putc((char)('0' + v % 10));
}

static int save(void)
{
    unsigned h, i, l = 0, k;
    for (i = 0; i < n; i++) {
        for (k = 0; names[i][k]; k++)
            txt[l++] = names[i][k];
        do
            txt[l++] = ' ';
        while (k++ < 12);
        txt[l++] = (char)('0' + vmaj[i]);
        txt[l++] = '.';
        txt[l++] = (char)('0' + vmin[i] / 10);
        txt[l++] = (char)('0' + vmin[i] % 10);
        txt[l++] = '\r';
        txt[l++] = '\n';
    }
    h = DosCreate(file);
    if (FAPI_FAILED(h))
        return -1;
    k = l ? DosWrite(h, txt, l) : 0;
    DosClose(h);
    return k == l ? 0 : -1;
}

static void show(void)
{
    unsigned i, k;
    if (!n) {
        puts("Die Versionstabelle ist leer.\r\n");
        return;
    }
    for (i = 0; i < n; i++) {
        puts(names[i]);
        for (k = slen(names[i]); k < 14; k++)
            putc(' ');
        putc((char)('0' + vmaj[i]));
        putc('.');
        put2(vmin[i]);
        puts("\r\n");
    }
}

static int valid_name(const char *s)
{
    unsigned i, dot = 0, base = 0, ext = 0;
    if (!*s || has_wild(s))
        return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] == '.') {
            if (dot++)
                return 0;
        } else if (s[i] == '\\' || s[i] == ':' || s[i] == '/')
            return 0;
        else if (dot)
            ext++;
        else
            base++;
    }
    return base >= 1 && base <= 8 && ext <= 3;
}

static int find(const char *s)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (same(names[i], s))
            return (int)i;
    return -1;
}

void main(void)
{
    static char w1[80], w2[80], w3[80];
    char *a;
    int del = 0, quiet = 0, k;
    unsigned i, nw = 0;
    unsigned char ma, mi;

    DosGetArgs(args);
    w1[0] = w2[0] = w3[0] = 0;
    for (a = skip(args); *a; a = skip(a)) {
        if (*a == '/') {
            char o = up(a[1]);
            if (o == '?') {
                puts("Setzt die Versionsnummer, die AT-DOS-Boxen einem DOS-Programm melden.\r\n\r\n"
                     "SETVER [Laufwerk:Pfad] [Dateiname n.nn]\r\n"
                     "SETVER [Laufwerk:Pfad] Dateiname /DELETE [/QUIET]\r\n\r\n"
                     "  Dateiname   Name des Programms (ohne Pfad)\r\n"
                     "  n.nn        zu meldende DOS-Version (2.00 bis 9.99)\r\n"
                     "  /DELETE /D  Eintrag l\x94schen\r\n"
                     "  /QUIET      keine Meldung beim L\x94schen\r\n\r\n"
                     "Ohne Angaben zeigt SETVER die Tabelle. Sie gilt f\x81r danach\r\n"
                     "gestartete DOS-Boxen.\r\n");
                return;
            }
            if (o == 'D')
                del = 1;
            else if (o == 'Q')
                quiet = 1;
            else {
                puts("SETVER: Ung\x81ltiger Schalter.\r\n");
                DosExit(1);
            }
            a = next_word(a);
            continue;
        }
        a = word(a, nw == 0 ? w1 : nw == 1 ? w2 : w3, 80);
        nw++;
    }
    /* Laufwerk/Pfad vorn (wie MS-DOS: Ort von SETVER.EXE) uebergehen */
    if (nw && (w1[slen(w1) - 1] == '\\' || w1[slen(w1) - 1] == ':' ||
               (w1[1] == ':' && is_dir(w1)) || (nw == 3) || (nw == 2 && del))) {
        scopy(w1, w2, sizeof(w1));
        scopy(w2, w3, sizeof(w2));
        nw--;
    }
    list_path();
    load();
    if (nw == 0) {
        if (del) {
            puts("SETVER: Dateiname fehlt.\r\n");
            DosExit(1);
        }
        show();
        return;
    }
    if (!valid_name(w1)) {
        puts("SETVER: Ung\x81ltiger Dateiname.\r\n");
        DosExit(1);
    }
    for (i = 0; w1[i]; i++)
        w1[i] = up(w1[i]);
    k = find(w1);
    if (del) {
        if (k < 0) {
            puts("SETVER: Eintrag nicht in der Versionstabelle.\r\n");
            DosExit(1);
        }
        for (i = (unsigned)k; i + 1 < n; i++) {
            scopy(names[i], names[i + 1], 13);
            vmaj[i] = vmaj[i + 1];
            vmin[i] = vmin[i + 1];
        }
        n--;
    } else {
        if (nw < 2 || parse_ver(w2, &ma, &mi)) {
            puts("SETVER: Ung\x81ltige Versionsnummer (2.00 bis 9.99).\r\n");
            DosExit(1);
        }
        if (k < 0) {
            if (n >= NENT) {
                puts("SETVER: Versionstabelle ist voll.\r\n");
                DosExit(1);
            }
            k = (int)n++;
            scopy(names[k], w1, 13);
        }
        vmaj[k] = ma;
        vmin[k] = mi;
    }
    if (save()) {
        puts("SETVER: Kann ");
        puts(file);
        puts(" nicht schreiben.\r\n");
        DosExit(1);
    }
    if (!del || !quiet)
        puts("Versionstabelle erfolgreich aktualisiert.\r\n"
             "Die \x8Enderung gilt f\x81r DOS-Boxen, die ab jetzt gestartet werden.\r\n");
    if (!del)
        puts("\r\nHINWEIS: Wenn ein Programm mit der gemeldeten Version nicht\r\n"
             "richtig arbeitet, kann es Daten verlieren oder besch\x84\x64igen.\r\n");
}
