/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RBOX.EXE - Kompatibilitaetsbox (5.1): DOS im echten Real Mode.
 *
 *   RBOX                startet FreeDOS aus X:\DOS\BOX\FREEDOS.IMG
 *   RBOX Abbild.IMG     startet ein anderes Diskettenabbild (A: in der Box)
 *   RBOX Datei.COM      ein .COM ohne DOS bei 1000:0100 (Test)
 *
 * Die Box laeuft, solange ihre Sitzung vorn ist; Alt+Esc wechselt zu den
 * anderen Sitzungen, zurueck setzt sie fort. Strg+Alt+Entf beendet sie.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], path[FAPI_PATHLEN], buf[8192];

static unsigned open_or_die(const char *p)
{
    unsigned h = DosOpen(p);
    if (FAPI_FAILED(h)) {
        puts("Datei nicht gefunden: ");
        puts(p);
        puts("\r\n");
        DosExit(2);
    }
    return h;
}

static void box_err(unsigned r)
{
    puts(r == FAPI_ERR_INUSE ? "Speicher unter 640 KB ist belegt.\r\n" :
         r == FAPI_ERR_FULL  ? "Zu wenig erweiterter Speicher f\x81r das Abbild.\r\n" :
         "Die Box geht hier nicht (Kernel unter 1 MB oder schon belegt).\r\n");
    DosExit(3);
}

/* Diskettenabbild: erst die Groesse, dann stueckweise in den Kernel */
static void load_img(const char *p)
{
    unsigned h, n, r;
    unsigned long size = 0;

    h = open_or_die(p);
    while ((n = DosRead(h, buf, sizeof(buf))) != 0 && !FAPI_FAILED(n))
        size += n;
    DosClose(h);
    if (size < 160UL * 1024 || size > 2880UL * 1024 || (size & 1023)) {
        puts("Kein Diskettenabbild (160 KB bis 2,88 MB).\r\n");
        DosExit(3);
    }
    r = DosRBox(RB_BOOT, (unsigned)(size >> 10), 0);
    if (r)
        box_err(r);
    h = open_or_die(p);
    while ((n = DosRead(h, buf, sizeof(buf))) != 0 && !FAPI_FAILED(n)) {
        n &= ~511u;
        if (DosRBox(RB_IMG, (unsigned)buf, n)) {
            puts("Abbild unbrauchbar (Bootsektor).\r\n");
            DosRBox(RB_FREE, 0, 0);
            DosExit(3);
        }
    }
    DosClose(h);
}

static void load_com(const char *p)
{
    unsigned h, sel, n, i, len = 0, r;
    char __far *seg;

    h = open_or_die(p);
    sel = DosAllocSeg(0);
    if (FAPI_FAILED(sel)) {
        puts("Zu wenig Arbeitsspeicher.\r\n");
        DosExit(3);
    }
    seg = (char __far *)((unsigned long)sel << 16);
    for (;;) {
        n = DosRead(h, buf, 512);
        if (FAPI_FAILED(n) || n == 0)
            break;
        if (len + n > 0xFE00) {
            puts("Programm zu gross.\r\n");
            DosExit(3);
        }
        for (i = 0; i < n; i++)
            seg[len + i] = buf[i];
        len += n;
    }
    DosClose(h);
    r = DosRBox(RB_LOAD, sel, len);
    if (r)
        box_err(r);
}

void main(void)
{
    char *a, *e;
    unsigned r, l;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Startet DOS in der Kompatibilit\x84tsbox (Real Mode).\r\n\r\n"
             "RBOX [Abbild.IMG | Datei.COM]\r\n\r\n"
             "Ohne Angabe startet FreeDOS aus \\DOS\\BOX\\FREEDOS.IMG des\r\n"
             "Startlaufwerks. Alt+Esc wechselt zu den anderen Sitzungen,\r\n"
             "Strg+Alt+Entf beendet die Box.\r\n");
        return;
    }
    if (!*a) {                          /* Startlaufwerk aus COMSPEC */
        if (FAPI_FAILED(DosGetEnv("COMSPEC", path, sizeof(path))) || path[1] != ':')
            scopy(path, "C:", sizeof(path));
        path[2] = 0;
        scat(path, "\\DOS\\BOX\\FREEDOS.IMG", sizeof(path));
    } else
        word(a, path, sizeof(path));
    a = path;
    l = slen(a);
    e = l >= 4 ? a + l - 4 : a;
    if (same(e, ".COM"))
        load_com(a);
    else
        load_img(a);
    while ((r = DosRBox(RB_RUN, 0, 0)) == RB_SWITCH)
        ;
    if ((r & 0xFF) != RB_EXIT) {
        puts("Box: unerwarteter R\x81" "ckweg.\r\n");
        DosExit(255);
    }
    puts("Box beendet.\r\n");
    DosExit(r >> 8);
}
