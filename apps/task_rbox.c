/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RBOX.EXE - Kompatibilitaetsbox (5.1): DOS im echten Real Mode.
 *
 *   RBOX                startet FreeDOS aus X:\DOS\MDOS\FREEDOS.IMG
 *   RBOX Abbild.IMG     startet ein anderes Diskettenabbild (A: in der Box)
 *   RBOX Datei.COM      ein .COM ohne DOS bei 1000:0100 (Test)
 *
 * Die Box laeuft, solange ihre Sitzung vorn ist; Alt+Esc wechselt zu den
 * anderen Sitzungen, zurueck setzt sie fort. Strg+Alt+Entf beendet sie.
 * Die AT-DOS-Laufwerke ab C: erscheinen in der Box als Netzlaufwerke
 * (rbox_redir.c, RBOX erledigt die Zugriffe).
 */
#include "fapi.h"
#include "util.h"

#include "rbox_redir.c"                 /* AT-DOS-Laufwerke in der Box */

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

/* Diskettenabbild: nur den Anfang (Bootsektor, FAT, Stamm) gleich laden,
   den Rest holt fetch(), wenn FreeDOS ihn per INT 13h liest (RB_FETCH).
   Die Groesse steht im BPB (Gesamtzahl der Sektoren, Offset 13h). Die
   Datei bleibt offen, solange die Box laeuft. */
static unsigned img_h = 0xFFFF;

static void load_img(const char *p)
{
    unsigned n, r, secs;

    if (img_h != 0xFFFF)
        DosClose(img_h);
    img_h = open_or_die(p);
    n = DosRead(img_h, buf, sizeof(buf));
    secs = FAPI_FAILED(n) || n < 512 ? 0 : (unsigned char)buf[0x13] |
           (unsigned)(unsigned char)buf[0x14] << 8;
    if (secs < 320 || secs > 5760 || (secs & 1)) {
        puts("Kein Diskettenabbild (160 KB bis 2,88 MB).\r\n");
        DosExit(3);
    }
    r = DosRBox(RB_BOOT, secs >> 1, 0);
    if (r)
        box_err(r);
    if (DosRBox(RB_IMG, (unsigned)buf, n & ~511u)) {
        puts("Abbild unbrauchbar (Bootsektor).\r\n");
        DosRBox(RB_FREE, 0, 0);
        DosExit(3);
    }
}

/* fehlendes Stueck (4 KB) nachladen, dazu das folgende */
static void fetch(void)
{
    unsigned long pos = (unsigned long)DosRBox(RB_NEED, 0, 0) << 12;
    unsigned n;

    if (FAPI_FAILED(DosSeek(img_h, &pos)) ||
        FAPI_FAILED(n = DosRead(img_h, buf, sizeof(buf))) || (n &= ~511u) == 0 ||
        DosRBox(RB_IMG, (unsigned)buf, n)) {
        puts("Abbild nicht lesbar.\r\n");
        DosRBox(RB_FREE, 0, 0);
        DosExit(3);
    }
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
             "RBOX [/P] [Abbild.IMG | Datei.COM]\r\n\r\n"
             "Ohne Angabe startet FreeDOS aus \\DOS\\MDOS\\FREEDOS.IMG des\r\n"
             "Startlaufwerks. Alt+Esc wechselt zu den anderen Sitzungen,\r\n"
             "Strg+Alt+Entf beendet die Box.\r\n\r\n"
             "/P: Box beim Start (PROTECTONLY=NO in CONFIG.SYS, wie OS/2 1.x):\r\n"
             "nach dem ersten Prompt zur\x81" "ck zu AT-DOS, EXIT und\r\n"
             "Strg+Alt+Entf starten sie neu.\r\n");
        return;
    }
    if (a[0] == '/' && (a[1] == 'P' || a[1] == 'p')) {  /* PROTECTONLY=NO */
        rb_perm = 1;                    /* (CMD /P startet RBOX /P)      */
        a = skip(a + 2);
    }
    if (!*a) {                          /* Startlaufwerk aus COMSPEC */
        if (FAPI_FAILED(DosGetEnv("COMSPEC", path, sizeof(path))) || path[1] != ':')
            scopy(path, "C:", sizeof(path));
        path[2] = 0;
        scat(path, "\\DOS\\MDOS\\FREEDOS.IMG", sizeof(path));
    } else
        word(a, path, sizeof(path));
    a = path;
    l = slen(a);
    e = base_name(a);                   /* ohne Endung: .COM, dann .IMG */
    while (*e && *e != '.')
        e++;
    if (!*e && l + 4 < sizeof(path)) {
        unsigned h;
        scat(a, ".COM", sizeof(path));
        h = DosOpen(a);
        if (FAPI_FAILED(h))
            a[l + 1] = 'I', a[l + 2] = 'M', a[l + 3] = 'G';
        else
            DosClose(h);
        l += 4;
    }
    e = l >= 4 ? a + l - 4 : a;
again:
    if (same(e, ".COM"))
        load_com(a);
    else
        load_img(a);
    for (l = 0;;) {                     /* l = 1: Register zurueck   */
        r = DosRBox(RB_RUN, (unsigned)&rg, l);
        l = 0;
        if (r == RB_SWITCH)
            continue;
        if (r == RB_FETCH) {            /* Stueck des Abbilds holen  */
            fetch();
            continue;
        }
        if (r == RB_CALL) {             /* INT E0h: Redirector usw.  */
            box_call();
            l = 1;
            continue;
        }
        break;
    }
    if ((r & 0xFF) != RB_EXIT) {
        puts("Box: unerwarteter R\x81" "ckweg.\r\n");
        DosExit(255);
    }
    l = r >> 8;                         /* Grund (RBSHELL, Kernel)   */
    if (rb_perm && (l == 0xFF || (l >= 0xE0 && l < 0xE8))) {
        puts("Box mit Strg+Alt+Entf zur\x81" "ckgesetzt, startet neu.\r\n");
        rb_perm = 2;                    /* RBSHELL: ohne RBREADY, die */
        goto again;                     /* Box bleibt vorn (OS/2 1.x) */
    }
    if (l >= 0xE0 && l < 0xE8) {        /* Weg im Stummel (Fehlersuche) */
        puts("Box mit Strg+Alt+Entf beendet (Weg ");
        put_num(l & 7, 0, 0);
        puts(").\r\n");
        DosExit(l);
    }
    if (l >= 0xD0 && l < 0xE0) {        /* RBSHELL: EXEC gescheitert   */
        puts("Box beendet: COMMAND.COM lie\xE1 sich nicht starten (DOS-Fehler ");
        put_num(l & 15, 0, 0);
        puts(l == 0xD2 ? ", Datei nicht gefunden" : l == 0xD8 ? ", zu wenig Speicher" :
             l == 0xDB ? ", ung\x81ltiges Format" : "");
        puts(").\r\n");
        DosExit(l);
    }
    puts(l == 0xFF ? "Box mit Strg+Alt+Entf beendet.\r\n" :
         l == 0xF1 ? "Box beendet: COMMAND.COM mit Strg+C abgebrochen.\r\n" :
         l == 0xF2 ? "Box beendet: COMMAND.COM durch einen Fehler abgebrochen.\r\n" :
         l == 0xF3 ? "Box beendet: COMMAND.COM blieb resident.\r\n" :
         "Box beendet.\r\n");
    if (l && l < 0xF0) {
        puts("R\x81" "ckgabewert ");
        put_num(l, 0, 0);
        puts("\r\n");
    }
    DosExit(l);
}
