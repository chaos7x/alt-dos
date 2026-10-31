/*
 * KEYB.EXE - Tastaturbelegung waehlen
 *
 *   KEYB        zeigt die aktuelle Belegung
 *   KEYB GR     deutsche Belegung (fest im Kernel)
 *   KEYB US     laedt US.KBL von der Diskette, ebenso jede andere Kennung
 */
#include "fapi.h"

static unsigned char kbl[KBL_SIZE_BYTES];

void main(void)
{
    static char args[FAPI_ARGLEN];
    static char id[5];
    static char file[13];
    char *a = args;
    unsigned h, n;
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

    for (i = 0; i < 8 && a[i] && a[i] != ' '; i++)
        file[i] = (a[i] >= 'a' && a[i] <= 'z') ? (char)(a[i] - 32) : a[i];
    file[i] = 0;

    if (file[0] == 'G' && file[1] == 'R' && file[2] == 0) {
        KbdSetLayout(0);
        puts("Tastaturbelegung: GR (eingebaut)\n");
        return;
    }

    file[i++] = '.'; file[i++] = 'K'; file[i++] = 'B'; file[i++] = 'L';
    file[i] = 0;
    h = DosOpen(file);
    if (h == FAPI_ERROR) {              /* sonst im DOS-Verzeichnis des Startlaufwerks */
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
        if (h == FAPI_ERROR) {          /* aeltere Ablage: \DOS         */
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
    if (n != sizeof(kbl) || KbdSetLayout(kbl) == FAPI_ERROR) {
        puts("Ung\x81ltige Belegungsdatei.\n");
        return;
    }
    KbdGetLayout(id);
    puts("Tastaturbelegung: ");
    puts(id);
    puts("\n");
}
