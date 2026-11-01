/*
 * MODE.EXE - Geraete einstellen (wie MS-DOS 5/6):
 *
 *   MODE CON[:] [COLS=80] [LINES=n]     n = 25, 43 oder 50 (43/50 nur VGA)
 *   MODE CO80 | BW80 | 80               zurueck auf 80x25
 *   MODE COMn[:] b[,p[,d[,s[,r]]]]      serielle Schnittstelle
 *   MODE COMn BAUD=b [PARITY=p] [DATA=d] [STOP=s] [RETRY=r]
 *   MODE LPTn[:] [c][,[l][,r]]          Drucker (COLS=, LINES=, RETRY=)
 *   MODE [Geraet]                       Zustand anzeigen
 *
 * Baud 110 bis 115200, auch abgekuerzt wie bei MS-DOS (96 = 9600, 19 =
 * 19200); Paritaet N, E, O, M, S; Datenbits 5-8; Stoppbits 1 oder 2;
 * Wiederholen P = endlos (sonst gibt der Kernel nach 30 s auf). Ohne
 * Angabe gilt wie bei MS-DOS gerade Paritaet, 7 Datenbits, 1 Stoppbit.
 * Drucker: 132 Spalten schaltet Schmalschrift ein (SI), 80 aus (DC2);
 * 8 Zeilen je Zoll ESC 0, 6 Zeilen ESC 2 (IBM-/Epson-Drucker).
 * Eingestellt wird ueber das FAPI (DosComCtl); den Port einer seriellen
 * Maus gibt der Kernel nicht her.
 * Wie bei OS/2 gilt die Zeilenzahl fuer die eigene Sitzung.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], w[60];
static struct fapi_comcfg cc;

static const unsigned long bauds[] = { 110, 150, 300, 600, 1200, 2400, 4800, 9600,
                                       19200, 38400, 57600, 115200 };
static const unsigned divs[] = { 1047, 768, 384, 192, 96, 48, 24, 12, 6, 3, 2, 1 };
#define NBAUD 12

static void con_status(void)
{
    unsigned n = VioSetLines(0);
    puts("\r\nStatus f\x81r Ger\x84t CON:\r\n"
         "----------------------\r\n"
         "Spalten=80\r\n"
         "Zeilen=");
    put_num(n, 0, 0);
    puts("\r\n");
}

static void dev_head(const char *d, unsigned n)
{
    puts("\r\nStatus f\x81r Ger\x84t ");
    puts(d);
    putc((char)('1' + n));
    puts(":\r\n----------------------\r\n");
}

/* Zustand eines Anschlusses; 0 = vorhanden */
static int port_status(int lpt, unsigned n)
{
    unsigned r = DosComCtl((lpt ? 0x10 : 0) | n, &cc, 0), i;
    if (r == FAPI_ERROR)
        return -1;
    dev_head(lpt ? "LPT" : "COM", n);
    if (r == FAPI_ERR_INUSE) {
        puts("Von einem Treiber belegt (serielle Maus?)\r\n");
        return 0;
    }
    if (lpt) {
        puts("Wiederholen=");
        puts(cc.retry ? "P (endlos)" : "KEINE");
        puts("\r\nDrucker: ");
        puts(!(cc.lsr & 0x08) ? "Fehler/aus" : (cc.lsr & 0x20) ? "kein Papier" :
             !(cc.lsr & 0x80) ? "belegt" : "bereit");
        puts("\r\n");
        return 0;
    }
    for (i = 0; i < NBAUD && divs[i] != cc.divisor; i++)
        ;
    puts("Baud=");
    if (i < NBAUD)
        put_num(bauds[i], 0, 0);
    else
        puts("?");
    puts("\r\nParit\x84t=");
    puts(!(cc.lcr & 0x08) ? "Keine" : (cc.lcr & 0x30) == 0x10 ? "Gerade" :
         (cc.lcr & 0x30) == 0x00 ? "Ungerade" : (cc.lcr & 0x30) == 0x20 ? "Markierung" : "Leer");
    puts("\r\nDatenbits=");
    put_num((cc.lcr & 3) + 5, 0, 0);
    puts("\r\nStoppbits=");
    puts(cc.lcr & 4 ? ((cc.lcr & 3) == 0 ? "1,5" : "2") : "1");
    puts("\r\nWiederholen=");
    puts(cc.retry ? "P (endlos)" : "KEINE");
    puts("\r\n");
    return 0;
}

static void usage(void)
{
    puts("Stellt Ger\x84te ein.\r\n\r\n"
         "MODE CON[:] [COLS=80] [LINES=n]    n = 25, 43 oder 50 (43/50 nur VGA)\r\n"
         "MODE CO80 | BW80 | 80              80 Spalten, 25 Zeilen\r\n"
         "MODE COMn[:] b[,p[,d[,s[,P]]]]     z. B. MODE COM1:9600,N,8,1\r\n"
         "MODE COMn BAUD=b PARITY=p DATA=d STOP=s RETRY=P\r\n"
         "MODE LPTn[:] [80|132][,[6|8][,P]]  Spalten, Zeilen je Zoll, endlos warten\r\n"
         "MODE [Ger\x84t]                       Zustand anzeigen\r\n");
}

static void fail(const char *m)
{
    puts("MODE: ");
    puts(m);
    puts("\r\n");
    DosExit(1);
}

static unsigned long num(const char **p)
{
    unsigned long v = 0;
    while (**p >= '0' && **p <= '9')
        v = (v << 3) + (v << 1) + (unsigned)(*(*p)++ - '0');   /* ohne __U4M */
    return v;
}

/* Angaben nach COMn bzw. LPTn: "a,b,c" oder "NAME=Wert ..." -> f[0..4] */
static void fields(char *a, char *f[5])
{
    unsigned i = 0;
    for (i = 0; i < 5; i++)
        f[i] = 0;
    i = 0;
    for (;;) {
        while (*a == ' ' || *a == ':')
            a++;
        if (!*a)
            break;
        if (*a == ',') {                    /* leeres Feld                */
            i++;
            a++;
            continue;
        }
        if (i < 5)
            f[i] = a;
        while (*a && *a != ',' && *a != ' ')
            a++;
        if (*a == ',') {
            *a++ = 0;
            i++;
        } else if (*a) {
            *a++ = 0;
            i++;
        }
    }
}

/* "NAME=Wert" -> Feldnummer, sonst -1 */
static int keyword(char **f, const char *const *names)
{
    unsigned i, k;
    for (i = 0; names[i]; i++) {
        for (k = 0; names[i][k] && (*f)[k] == names[i][k]; k++)
            ;
        if (!names[i][k] && (*f)[k] == '=') {
            *f += k + 1;
            return (int)i;
        }
    }
    return -1;
}

static void set_com(unsigned n, char *rest)
{
    static const char *const kw[] = { "BAUD", "PARITY", "DATA", "STOP", "RETRY", 0 };
    char *f[5], *g[5];
    const char *p;
    unsigned long b;
    unsigned i, lcr, r;

    fields(rest, g);
    for (i = 0; i < 5; i++)
        f[i] = 0;
    for (i = 0; i < 5; i++) {               /* Schluesselwoerter einsortieren */
        int k;
        char *x = g[i];
        if (!x)
            continue;
        k = keyword(&x, kw);
        if (k >= 0)
            f[k] = x;
        else
            f[i] = x;
    }
    if (!f[0] || !*f[0])
        fail("Baudrate fehlt (z. B. MODE COM1:9600,N,8,1).");
    p = f[0];
    b = num(&p);
    if (*p)
        fail("Ung\x81ltige Baudrate.");
    if (b == 11) b = 110;                   /* MS-DOS-Abkuerzungen        */
    else if (b == 15) b = 150;
    else if (b == 30) b = 300;
    else if (b == 60) b = 600;
    else if (b == 12) b = 1200;
    else if (b == 24) b = 2400;
    else if (b == 48) b = 4800;
    else if (b == 96) b = 9600;
    else if (b == 19) b = 19200;
    for (i = 0; i < NBAUD && bauds[i] != b; i++)
        ;
    if (i == NBAUD)
        fail("Ung\x81ltige Baudrate (110 bis 115200).");
    cc.divisor = divs[i];
    lcr = 0x18;                             /* gerade Paritaet            */
    if (f[1] && *f[1]) {
        switch (*f[1]) {
        case 'N': lcr = 0x00; break;
        case 'E': lcr = 0x18; break;
        case 'O': lcr = 0x08; break;
        case 'M': lcr = 0x28; break;
        case 'S': lcr = 0x38; break;
        default: fail("Parit\x84t muss N, E, O, M oder S sein.");
        }
    }
    i = 7;
    if (f[2] && *f[2]) {
        p = f[2];
        i = (unsigned)num(&p);
        if (i < 5 || i > 8 || *p)
            fail("Datenbits m\x81ssen 5 bis 8 sein.");
    }
    lcr |= i - 5;
    i = b == 110 ? 2 : 1;
    if (f[3] && *f[3]) {
        p = f[3];
        i = (unsigned)num(&p);
        if (*p == ',' || *p == '.')         /* 1,5 / 1.5                  */
            i = 2;
        else if ((i != 1 && i != 2) || *p)
            fail("Stoppbits m\x81ssen 1 oder 2 sein.");
    }
    if (i == 2)
        lcr |= 4;
    cc.lcr = (unsigned char)lcr;
    cc.retry = f[4] && *f[4] == 'P';
    r = DosComCtl(n, &cc, 1);
    if (r == FAPI_ERR_INUSE)
        fail("Die Schnittstelle wird von einem Treiber benutzt (serielle Maus?).");
    if (r)
        fail("Diese Schnittstelle gibt es nicht.");
    port_status(0, n);
}

static void set_lpt(unsigned n, char *rest)
{
    static const char *const kw[] = { "COLS", "LINES", "RETRY", 0 };
    static char dev[6] = "LPT1";
    static unsigned char seq[4];
    char *f[5], *g[5];
    const char *p;
    unsigned i, len = 0, h, v;

    fields(rest, g);
    for (i = 0; i < 5; i++)
        f[i] = 0;
    for (i = 0; i < 5; i++) {
        int k;
        char *x = g[i];
        if (!x)
            continue;
        k = keyword(&x, kw);
        if (k >= 0)
            f[k] = x;
        else if (i < 3)
            f[i] = x;
    }
    if (DosComCtl(0x10 | n, &cc, 0))
        fail("Diesen Druckeranschluss gibt es nicht.");
    if (f[0] && *f[0]) {
        p = f[0];
        v = (unsigned)num(&p);
        if (v == 132) seq[len++] = 0x0F;    /* SI: Schmalschrift          */
        else if (v == 80) seq[len++] = 0x12;    /* DC2: normal            */
        else fail("Spalten m\x81ssen 80 oder 132 sein.");
    }
    if (f[1] && *f[1]) {
        p = f[1];
        v = (unsigned)num(&p);
        if (v != 6 && v != 8)
            fail("Zeilen je Zoll m\x81ssen 6 oder 8 sein.");
        seq[len++] = 0x1B;
        seq[len++] = v == 8 ? '0' : '2';
    }
    if (f[2])
        cc.retry = *f[2] == 'P';
    DosComCtl(0x10 | n, &cc, 1);
    if (len) {
        dev[3] = (char)('1' + n);
        h = DosCreate(dev);
        if (FAPI_FAILED(h) || DosWrite(h, seq, len) != len)
            puts("MODE: Drucker nicht bereit - Einstellung nicht gesendet.\r\n");
        if (!FAPI_FAILED(h))
            DosClose(h);
    }
    port_status(1, n);
}

static int set_lines(unsigned n)
{
    if (FAPI_FAILED(VioSetLines(n))) {
        if (n == 25)
            puts("MODE: Zeilenzahl kann nicht ge\x84ndert werden (Vollbild?).\r\n");
        else
            puts("MODE: Diese Zeilenzahl kann die Grafikkarte nicht (43 und 50 nur mit VGA).\r\n");
        DosExit(1);
    }
    return 0;
}

/* "COM1", "COM1:", "COM1:9600,..." -> Nummer 0-3, *rest = Angaben */
static int devnum(char *s, const char *pre, unsigned max, char **rest)
{
    if (s[0] != pre[0] || s[1] != pre[1] || s[2] != pre[2] || s[3] < '1' ||
        s[3] > (char)('0' + max))
        return -1;
    if (s[4] && s[4] != ':' && s[4] != ' ')
        return -1;
    *rest = s + 4;
    return s[3] - '1';
}

void main(void)
{
    char *a, *v, *rest;
    unsigned lines = 0, n;
    int con = 0, any = 0, d;

    DosGetArgs(args);
    for (v = args; *v; v++)
        *v = up(*v);
    a = skip(args);
    if (!*a) {                              /* alles anzeigen             */
        con_status();
        for (n = 0; n < 3; n++)
            port_status(1, n);
        for (n = 0; n < 4; n++)
            port_status(0, n);
        return;
    }
    if (a[0] == '/' && a[1] == '?') {
        usage();
        return;
    }
    if ((d = devnum(a, "COM", 4, &rest)) >= 0) {
        if (!*skip(rest[0] == ':' ? rest + 1 : rest)) {
            if (port_status(0, (unsigned)d))
                fail("Diese Schnittstelle gibt es nicht.");
        } else
            set_com((unsigned)d, rest);
        return;
    }
    if ((d = devnum(a, "LPT", 3, &rest)) >= 0) {
        if (!*skip(rest[0] == ':' ? rest + 1 : rest)) {
            if (port_status(1, (unsigned)d))
                fail("Diesen Druckeranschluss gibt es nicht.");
        } else
            set_lpt((unsigned)d, rest);
        return;
    }
    while (*a) {
        a = word(a, w, sizeof(w));
        a = skip(a);
        if (same(w, "CON") || same(w, "CON:")) {
            con = 1;
            continue;
        }
        if (same(w, "CO80") || same(w, "BW80") || same(w, "80")) {
            lines = 25;
            any = 1;
            continue;
        }
        if (con && w[0] == 'L' && w[1] == 'I' && w[2] == 'N' && w[3] == 'E' &&
            w[4] == 'S' && w[5] == '=') {
            for (n = 0, v = w + 6; *v >= '0' && *v <= '9'; v++)
                n = n * 10 + (unsigned)(*v - '0');
            if (*v || (n != 25 && n != 43 && n != 50))
                fail("Zeilen m\x81ssen 25, 43 oder 50 sein.");
            lines = n;
            any = 1;
            continue;
        }
        if (con && w[0] == 'C' && w[1] == 'O' && w[2] == 'L' && w[3] == 'S' &&
            w[4] == '=') {
            if (!same(w + 5, "80"))
                fail("Nur 80 Spalten m\x94glich.");
            any = 1;
            continue;
        }
        puts("MODE: Ung\x81ltiger Parameter - ");
        puts(w);
        puts("\r\n");
        DosExit(1);
    }
    if (!any) {
        con_status();
        return;
    }
    if (lines)
        set_lines(lines);
}
