/*
 * LABEL.EXE - Datentraegerbezeichnung anzeigen, setzen, loeschen (MS-DOS):
 *
 *   LABEL [Laufwerk:][Bezeichnung]
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], nl[16], in[40];
static struct fapi_label lb;

static void hex8(unsigned long v)
{
    static const char hx[] = "0123456789ABCDEF";
    int k;
    for (k = 28; k >= 0; k -= 4) {
        putc(hx[(unsigned)(v >> k) & 15]);
        if (k == 16)
            putc('-');
    }
}

/* Bezeichnung pruefen und gross schreiben; 0 = ok */
static int clean(char *s)
{
    static const char bad[] = "*?/\\|.,;:+=<>[]\"";
    unsigned i, k;
    for (i = 0; s[i]; i++) {
        if (i >= 11 || s[i] < ' ')
            return -1;
        for (k = 0; bad[k]; k++)
            if (s[i] == bad[k])
                return -1;
        s[i] = up(s[i]);
    }
    return 0;
}

void main(void)
{
    char *a;
    unsigned d = DosQCurDisk(), n, r;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Erstellt, \x84ndert oder l\x94scht die Bezeichnung eines Datentr\x84gers.\r\n\r\n"
             "LABEL [Laufwerk:][Bezeichnung]\r\n");
        return;
    }
    if (a[0] && a[1] == ':') {
        d = (unsigned)(up(a[0]) - 'A');
        a = skip(a + 2);
    }
    if (DosLabel(d, &lb, 0)) {
        puts("Ung\x81ltiges Laufwerk.\r\n");
        DosExit(1);
    }
    if (*a) {                               /* Bezeichnung angegeben    */
        scopy(nl, a, sizeof(nl));
        for (n = slen(nl); n && nl[n - 1] == ' '; n--)
            nl[n - 1] = 0;
    } else {
        puts("Datentr\x84ger in Laufwerk ");
        putc((char)('A' + d));
        puts(lb.label[0] ? ": ist " : ": hat keine Bezeichnung");
        puts(lb.label);
        puts("\r\n");
        if (lb.serial) {
            puts("Datentr\x84ger-Seriennummer: ");
            hex8(lb.serial);
            puts("\r\n");
        }
        puts("Datentr\x84gerbezeichnung (11 Zeichen, EINGABETASTE f\x81r keine)? ");
        n = DosRead(STDIN, in, sizeof(in) - 1);
        if (FAPI_FAILED(n))
            n = 0;
        in[n] = 0;
        for (n = 0; in[n] && in[n] != '\r' && in[n] != '\n'; n++)
            ;
        in[n] = 0;
        scopy(nl, in, sizeof(nl));
        if (!nl[0]) {
            if (!lb.label[0])
                return;
            puts("\r\nAktuelle Bezeichnung l\x94schen (J/N)? ");
            if (!ask_yn())
                return;
        }
    }
    if (clean(nl)) {
        puts("Ung\x81ltige Zeichen in der Bezeichnung (h\x94" "chstens 11).\r\n");
        DosExit(1);
    }
    r = DosLabel(d, &lb, nl);
    if (r) {
        puts(r == FAPI_ERR_WP ? "Datentr\x84ger ist schreibgesch\x81tzt.\r\n"
                              : "Bezeichnung kann nicht geschrieben werden.\r\n");
        DosExit(1);
    }
}
