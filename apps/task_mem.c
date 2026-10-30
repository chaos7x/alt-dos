/* MEM.EXE: zeigt den vorhandenen Speicher, ermittelt ueber DosMemInfo(). */
#include "fapi.h"

/* v rechtsbuendig mit w Stellen ab b[pos] eintragen */
static void put_num(char *b, int pos, int w, unsigned v)
{
    int i = pos + w - 1;
    do {
        b[i--] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i >= pos);
    while (i >= pos)
        b[i--] = ' ';
}

void main(void)
{
    static struct fapi_meminfo mi;
    static char l0[] = "Konventionell: ????? KB   Erweitert: ????? KB   Gesamt: ????? KB";
    static char l1[] = "Belegt:  Kernel ????? KB, Anwendungen ????? KB";
    static char l2[] = "Ab 1 MB: ????? KB frei (noch ohne Speicherverwaltung)";

    if (DosMemInfo(&mi) == FAPI_ERROR) {
        VioWrtStr(0, 2, "DosMemInfo fehlgeschlagen.");
        return;
    }
    put_num(l0, 15, 5, mi.conv_kb);
    put_num(l0, 37, 5, mi.ext_kb);
    put_num(l0, 56, 5, mi.conv_kb + mi.ext_kb);
    put_num(l1, 16, 5, mi.kernel_kb);
    put_num(l1, 38, 5, mi.apps_kb);
    put_num(l2, 9, 5, mi.ext_kb);

    VioWrtStr(0, 2, l0);
    VioWrtStr(1, 2, l1);
    VioWrtStr(2, 2, l2);
}
