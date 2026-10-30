/* MEM.EXE: zeigt den Arbeitsspeicher laufend an, ueber DosMemInfo(). */
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
    static char l0[] = "Arbeitsspeicher: ????? KB   belegt: ????? KB   frei: ????? KB";
    static char l1[] = "Kernel ??? KB, Programme ??? KB, BIOS/Boot ??? KB, angefordert ????? KB";
    unsigned used;

    VioWrtStr(2, 2, "Systembereich 640 KB - 1 MB (Video, ROMs) ausgespart");
    for (;;) {
        if (DosMemInfo(&mi) == FAPI_ERROR) {
            VioWrtStr(0, 2, "DosMemInfo fehlgeschlagen.");
            return;
        }
        used = mi.total_kb - mi.free_kb;
        put_num(l0, 17, 5, mi.total_kb);
        put_num(l0, 36, 5, used);
        put_num(l0, 53, 5, mi.free_kb);
        put_num(l1, 7, 3, mi.kernel_kb);
        put_num(l1, 25, 3, mi.apps_kb);
        put_num(l1, 43, 3, used - mi.kernel_kb - mi.apps_kb - mi.alloc_kb);
        put_num(l1, 63, 5, mi.alloc_kb);
        VioWrtStr(0, 2, l0);
        VioWrtStr(1, 2, l1);
        DosSleep(250);
    }
}
