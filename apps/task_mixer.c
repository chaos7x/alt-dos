/*
 * MIXER.EXE - Lautstaerke der Soundkarte zeigen und einstellen
 *
 *   MIXER                 alle Regler zeigen
 *   MIXER Regler Wert     Regler auf 0-100 setzen, z. B. MIXER MASTER 80
 *
 * Regler: MASTER, WAVE, FM, CD, LINE, MIC (was die Karte kennt).
 */
#include "fapi.h"
#include "util.h"

static const char *const names[6] = { "MASTER", "WAVE", "FM", "CD", "LINE", "MIC" };
static const char *const desc[6] = {
    "Gesamtlautst\x84rke", "Digitaler Ton (PLAY)", "FM-Synthese (OPL)",
    "CD-Audio", "Line-Eingang", "Mikrofon" };
static char args[FAPI_ARGLEN], w1[16], w2[16], info[64];

static void show(int c)
{
    unsigned v = AudMixer(c, 0xFFFF), i;
    if (FAPI_FAILED(v))
        return;
    puts("  ");
    puts(names[c]);
    for (i = slen(names[c]); i < 8; i++)
        putc(' ');
    put_num(v, 3, 0);
    puts(" %  ");
    for (i = 0; i < 20; i++)
        putc(i < (v + 2) / 5 ? (char)0xDB : (char)0xB0);
    puts("  ");
    puts(desc[c]);
    puts("\r\n");
}

void main(void)
{
    char *a;
    int c;
    unsigned v = 0, i;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Zeigt und stellt die Lautst\x84rke der Soundkarte ein.\r\n\r\n"
             "MIXER [Regler Wert]\r\n\r\n"
             "  Regler: MASTER, WAVE, FM, CD, LINE, MIC   Wert: 0-100\r\n"
             "Beispiel: MIXER MASTER 80\r\n");
        return;
    }
    if (FAPI_FAILED(AudInfo(info))) {
        puts("MIXER: keine Soundkarte (DEVICE=SB16.SYS in CONFIG.SYS)\r\n");
        DosExit(1);
    }
    if (*a) {
        a = word(a, w1, sizeof(w1));
        word(skip(a), w2, sizeof(w2));
        for (c = 0; c < 6 && !same(w1, names[c]); c++)
            ;
        for (i = 0; w2[i] >= '0' && w2[i] <= '9'; i++)
            v = v * 10 + (w2[i] - '0');
        if (c == 6 || !w2[0] || w2[i] || v > 100) {
            puts("MIXER: Regler und Wert 0-100 angeben, z. B. MIXER MASTER 80\r\n");
            DosExit(1);
        }
        if (FAPI_FAILED(AudMixer(c, v))) {
            puts("MIXER: diesen Regler hat die Karte nicht\r\n");
            DosExit(1);
        }
    }
    puts(info);
    puts("\r\n");
    for (c = 0; c < 6; c++)
        show(c);
}
