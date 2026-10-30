/* Task A: zaehlt endlos und gibt nach jedem Durchlauf per DosYield() ab. */
#include "fapi.h"

static char cnt[] = "0000000";

static void count_up(char *d)
{
    int i = 6;
    while (i >= 0) {
        if (d[i] == '9') { d[i] = '0'; i--; }
        else             { d[i]++;     break; }
    }
}

void main(void)
{
    static char ver[] = "FAPI-Version ?.??";
    unsigned v = DosGetVersion();

    ver[13] = (char)('0' + (v >> 8));
    ver[15] = (char)('0' + (v & 0xFF) / 10);
    ver[16] = (char)('0' + (v & 0xFF) % 10);

    VioWrtStr(0, 2, "Hallo aus Ring 3. Ich gebe nach jedem Durchlauf mit DosYield() ab.");
    VioWrtStr(1, 2, ver);
    for (;;) {
        count_up(cnt);
        VioWrtStr(2, 2, "Durchl\x84ufe:");
        VioWrtStr(2, 15, cnt);
        DosYield();
    }
}
