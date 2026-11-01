/*
 * FC.EXE - Dateien vergleichen (wie MS-DOS):
 *
 *   FC [/B] [/C] [/N] [/nnnn] Datei1 Datei2
 *
 * Text (Vorgabe): zeilenweise, unterschiedliche Bloecke mit der letzten
 * gleichen und der ersten wieder gleichen Zeile; /nnnn = so viele gleiche
 * Zeilen gelten als wieder synchron (Vorgabe 2), /C ohne Gross/klein,
 * /N mit Zeilennummern. /B binaer: Abweichungen als Offset und Bytes.
 * ERRORLEVEL 0 gleich, 1 verschieden, 2 Fehler. Bis 63 KB je Datei (Text).
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

#define MAXL 4000

static char args[FAPI_ARGLEN], n1[FAPI_PATHLEN], n2[FAPI_PATHLEN], buf[XFER], b2[XFER];
static int opt_b, opt_c, opt_n, diffs;
static unsigned resync = 2;

struct txt { unsigned seg, tab, n; };
static struct txt A, B;

static void hex(unsigned long v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    while (digits--)
        putc(hx[(unsigned)(v >> (digits * 4)) & 15]);
}

static int load(const char *name, struct txt *t)
{
    unsigned h, n, i, used = 0, start = 0;
    char __far *d;
    unsigned __far *tb;

    h = DosOpen(name);
    if (FAPI_FAILED(h))
        return h == FAPI_ERR_INUSE ? -4 : -1;
    t->seg = DosAllocSeg(0);
    t->tab = DosAllocSeg(MAXL * 2);
    if (FAPI_FAILED(t->seg) || FAPI_FAILED(t->tab))
        return -2;
    d = MK_FP(t->seg, 0);
    tb = MK_FP(t->tab, 0);
    t->n = 0;
    while ((n = DosRead(h, buf, sizeof(buf))) != 0 && !FAPI_FAILED(n))
        for (i = 0; i < n; i++) {
            if (buf[i] == '\r' || buf[i] == 0x1A)
                continue;
            if (used >= 0xFFF0 || t->n >= MAXL) {
                DosClose(h);
                return -3;
            }
            if (buf[i] == '\n') {
                d[used++] = 0;
                tb[t->n++] = start;
                start = used;
            } else
                d[used++] = buf[i];
        }
    if (used > start) {
        d[used++] = 0;
        tb[t->n++] = start;
    }
    DosClose(h);
    return 0;
}

static char __far *ln(struct txt *t, unsigned i)
{
    unsigned __far *tb = MK_FP(t->tab, 0);
    return MK_FP(t->seg, tb[i]);
}

static int eq(unsigned i, unsigned j)
{
    char __far *a = ln(&A, i), __far *b = ln(&B, j);
    while (*a && (opt_c ? up(*a) == up(*b) : *a == *b))
        a++, b++;
    return *a == *b || (opt_c && up(*a) == up(*b));
}

static void show(const char *name, struct txt *t, unsigned from, unsigned to)
{
    unsigned i, k;
    char __far *s;
    puts("***** ");
    puts(name);
    puts("\r\n");
    for (i = from; i < to && i < t->n; i++) {
        if (opt_n) {
            put_num(i + 1L, 5, 0);
            puts(":  ");
        }
        s = ln(t, i);
        for (k = 0; s[k] && k < sizeof(buf) - 1; k++)
            buf[k] = s[k];
        buf[k] = 0;
        puts(buf);
        puts("\r\n");
    }
}

/* synchron ab (i, j)? resync gleiche Zeilen oder beide am Ende */
static int sync_at(unsigned i, unsigned j)
{
    unsigned k;
    for (k = 0; k < resync; k++) {
        if (i + k >= A.n || j + k >= B.n)
            return i + k >= A.n && j + k >= B.n;
        if (!eq(i + k, j + k))
            return 0;
    }
    return 1;
}

static void text_cmp(void)
{
    unsigned i = 0, j = 0, s, x, y, lim;

    while (i < A.n || j < B.n) {
        if (i < A.n && j < B.n && eq(i, j)) {
            i++, j++;
            continue;
        }
        lim = (A.n - i) + (B.n - j);
        if (lim > 400)
            lim = 400;                  /* weiter weg: Rest verschieden */
        for (s = 1; s <= lim; s++) {
            for (x = 0; x <= s; x++) {
                y = s - x;
                if (i + x > A.n || j + y > B.n)
                    continue;
                if (sync_at(i + x, j + y))
                    goto found;
            }
        }
        x = A.n - i;                    /* bis zum Ende verschieden     */
        y = B.n - j;
found:
        diffs++;
        show(n1, &A, i ? i - 1 : 0, i + x + 1);
        show(n2, &B, j ? j - 1 : 0, j + y + 1);
        puts("*****\r\n\r\n");
        i += x;
        j += y;
    }
}

static int bin_cmp(void)
{
    unsigned h1, h2, a, b, k, m;
    unsigned long pos = 0;

    h1 = DosOpen(n1);
    h2 = DosOpen(n2);
    if (FAPI_FAILED(h1) || FAPI_FAILED(h2)) {
        if (!FAPI_FAILED(h1))
            DosClose(h1);
        if (!FAPI_FAILED(h2))
            DosClose(h2);
        return h1 == FAPI_ERR_INUSE || h2 == FAPI_ERR_INUSE ? -4 : -1;
    }
    for (;;) {
        a = DosRead(h1, buf, sizeof(buf));
        b = DosRead(h2, b2, sizeof(b2));
        if (FAPI_FAILED(a)) a = 0;
        if (FAPI_FAILED(b)) b = 0;
        m = a < b ? a : b;
        for (k = 0; k < m; k++)
            if (buf[k] != b2[k]) {
                diffs++;
                hex(pos + k, 8);
                puts(": ");
                hex((unsigned char)buf[k], 2);
                putc(' ');
                hex((unsigned char)b2[k], 2);
                puts("\r\n");
            }
        pos += m;
        if (a != b) {
            diffs++;
            puts("FC: ");
            puts(a > b ? n1 : n2);
            puts(" ist l\x84nger als ");
            puts(a > b ? n2 : n1);
            puts("\r\n");
            break;
        }
        if (a == 0)
            break;
    }
    DosClose(h1);
    DosClose(h2);
    return 0;
}

void main(void)
{
    char *a;
    int r;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/') {
            char o = up(a[1]);
            if (o == 'B') opt_b = 1;
            else if (o == 'C') opt_c = 1;
            else if (o == 'N') opt_n = 1;
            else if (o >= '0' && o <= '9') {
                for (resync = 0, a++; *a >= '0' && *a <= '9'; a++)
                    resync = resync * 10 + (unsigned)(*a - '0');
                if (!resync)
                    resync = 1;
            } else if (o == '?') {
                puts("Vergleicht zwei Dateien.\r\n\r\n"
                     "FC [/B] [/C] [/N] [/nnnn] Datei1 Datei2\r\n\r\n"
                     "  /B  bin\x84r    /C  ohne Gro\xE1/klein    /N  Zeilennummern\r\n"
                     "  /nnnn  gleiche Zeilen bis zur Synchronisation (Vorgabe 2)\r\n");
                return;
            }
        } else if (!n1[0])
            word(a, n1, sizeof(n1));
        else
            word(a, n2, sizeof(n2));
    }
    if (!n2[0]) {
        puts("FC: Zu wenige Dateinamen angegeben\r\n");
        DosExit(2);
    }
    puts("Vergleichen der Dateien ");
    puts(n1);
    puts(" und ");
    puts(n2);
    puts("\r\n");
    if (opt_b) {
        r = bin_cmp();
    } else {
        r = load(n1, &A);
        if (!r)
            r = load(n2, &B);
        if (r == -3) {
            puts("FC: Datei zu gro\xE1 f\x81r den Textvergleich, /B benutzen\r\n");
            DosExit(2);
        }
        if (!r)
            text_cmp();
    }
    if (r) {
        puts(r == -2 ? "FC: Nicht genug Arbeitsspeicher\r\n" :
             r == -4 ? "FC: Datei wird gerade von einem anderen Programm geschrieben\r\n" :
                       "FC: Datei kann nicht ge\x94" "ffnet werden\r\n");
        DosExit(2);
    }
    if (!diffs)
        puts("FC: Keine Unterschiede gefunden\r\n");
    DosExit(diffs ? 1 : 0);
}
