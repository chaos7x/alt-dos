/* Task B: zaehlt zehn Sekunden mit DosSleep() und beendet sich regulaer. */
#include "fapi.h"

void main(void)
{
    static char line[] = "Sekunde ?? von 10";
    int i;

    VioWrtStr(0, 2, "Ich schlafe jeweils mit DosSleep(1000) und ende nach 10 Sekunden.");
    for (i = 1; i <= 10; i++) {
        DosSleep(1000);
        line[8] = (char)(i >= 10 ? '1' : ' ');
        line[9] = (char)('0' + i % 10);
        VioWrtStr(1, 2, line);
    }
    VioWrtStr(2, 2, "Fertig - zur\x81" "ck aus main(), crt0 ruft DosExit auf.");
}
