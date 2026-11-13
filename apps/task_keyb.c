/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * KEYB.EXE - Tastaturbelegung waehlen
 *
 *   KEYB        zeigt die aktuelle Belegung
 *   KEYB GR     deutsche Belegung (fest im Kernel)
 *   KEYB US     laedt US.KBL (\DOS\SYSTEM), ebenso jede andere Kennung
 *   KEYB DK,865 stellt vorher die Codepage um (wie CHCP, muss vorbereitet
 *               sein: CODEPAGE= in der CONFIG.SYS)
 *
 * Belegungen der Version 2 sind Unicode; der Kernel leitet die Zeichen der
 * aktiven Codepage ab und meldet, wie viele dort fehlen.
 */
#include "fapi.h"

static unsigned char kbl[KBL2_SIZE_BYTES];

static void put_u(unsigned v)
{
    char d[6];
    int k = 5;
    d[5] = 0;
    do {
        d[--k] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    puts(d + k);
}

/* Zeichen der Belegung, die die aktive Codepage nicht hat */
static void missing(unsigned r)
{
    if (!r || r == FAPI_ERROR)
        return;
    put_u(r);
    puts(" Zeichen");
    puts(r == 1 ? " der Belegung fehlt in Codepage " : " der Belegung fehlen in Codepage ");
    put_u(DosGetCp());
    puts(" (CHCP 850 o. \x84.).\n");
}

void main(void)
{
    static char args[FAPI_ARGLEN];
    static char id[5];
    static char file[13];
    char *a = args;
    unsigned h, n, r, cp;
    int i;

    DosGetArgs(args);
    while (*a == ' ')
        a++;

    if (!*a) {
        KbdGetLayout(id);
        puts("Aktuelle Tastaturbelegung: ");
        puts(id);
        puts("\n");
        return;
    }

    for (i = 0; i < 8 && a[i] && a[i] != ' ' && a[i] != ','; i++)
        file[i] = (a[i] >= 'a' && a[i] <= 'z') ? (char)(a[i] - 32) : a[i];
    file[i] = 0;
    a += i;
    while (*a == ' ' || *a == ',')
        a++;
    if (*a >= '0' && *a <= '9') {               /* Codepage dazu         */
        cp = 0;
        while (*a >= '0' && *a <= '9')
            cp = cp * 10 + (unsigned)(*a++ - '0');
        if (FAPI_FAILED(DosSetCp(cp))) {
            puts("Codepage nicht vorbereitet (CODEPAGE= in der CONFIG.SYS).\n");
            DosExit(1);
        }
    }

    if (file[0] == 'G' && file[1] == 'R' && file[2] == 0) {
        r = KbdSetLayout(0);
        puts("Tastaturbelegung: GR (eingebaut)\n");
        missing(r);
        return;
    }

    file[i++] = '.'; file[i++] = 'K'; file[i++] = 'B'; file[i++] = 'L';
    file[i] = 0;
    h = DosOpen(file);
    if (FAPI_FAILED(h)) {              /* sonst im DOS-Verzeichnis des Startlaufwerks */
        static char alt[FAPI_PATHLEN], cs[FAPI_PATHLEN];
        unsigned k = 0, j;
        if (DosGetEnv("COMSPEC", cs, sizeof(cs)) != FAPI_ERROR && cs[1] == ':') {
            alt[k++] = cs[0];
            alt[k++] = ':';
        }
        for (j = 0; "\\DOS\\SYSTEM\\"[j]; j++)
            alt[k++] = "\\DOS\\SYSTEM\\"[j];
        for (j = 0; file[j]; j++)
            alt[k + j] = file[j];
        alt[k + j] = 0;
        h = DosOpen(alt);
        if (FAPI_FAILED(h)) {          /* aeltere Ablage: \DOS         */
            for (j = 0; file[j]; j++)
                alt[k - 7 + j] = file[j];   /* "SYSTEM\" ueberschreiben */
            alt[k - 7 + j] = 0;
            h = DosOpen(alt);
        }
    }
    if (h == FAPI_ERROR) {
        puts("Belegungsdatei ");
        puts(file);
        puts(" nicht gefunden.\n");
        return;
    }
    n = DosRead(h, kbl, sizeof(kbl));
    DosClose(h);
    if (!((n == KBL_SIZE_BYTES && kbl[3] == 1) || (n == KBL2_SIZE_BYTES && kbl[3] == 2)) ||
        (r = KbdSetLayout(kbl)) == FAPI_ERROR) {
        puts("Ung\x81ltige Belegungsdatei.\n");
        DosExit(1);
    }
    KbdGetLayout(id);
    puts("Tastaturbelegung: ");
    puts(id);
    puts("\n");
    missing(r);
}
