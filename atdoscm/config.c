/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - CONFIG.SYS der Box (DOSKERN Stufe 4).
 *
 * Datei X:\DOS\MDOS\CONFIG.SYS des Startlaufwerks, beim Start der Box
 * vor BOXSHELL gelesen. Die Einstellungen, die beim Start der ganzen Box
 * gelten (DOS=, RMSIZE=), stehen in der CONFIG.SYS von AT-DOS, die je
 * Programm in DOSBOX.INI. Hier:
 *   DEVICE=Datei [Argumente]       Zeichentreiber laden (wie MS-DOS)
 *   DEVICEHIGH=Datei [Argumente]   ebenso, in den oberen Speicher
 *   BUFFERS=n                      nur Anzeige (LoL+3Fh), Puffer hat der Kernel
 *   BREAK=ON|OFF
 *   REM, ;                         Kommentar
 * Andere Schluesselwoerter (FILES=, DOS=, LASTDRIVE= ...) gelten als
 * erledigt und werden still uebergangen.
 *
 * Treiber laden wie MS-DOS 4.0 (SYSINIT): Datei als Overlay an den Anfang
 * eines Blocks, INIT-Anforderung (Befehl 0) mit hoechster Endadresse und
 * Zeiger auf die Befehlszeile, Block auf die gemeldete Endadresse
 * kuerzen, Geraetekoepfe hinter NUL in die Kette. Ein Treiber mit
 * Bits STDIN/STDOUT ersetzt CON (ANSI.SYS): con.c gibt dann darueber aus.
 * Blocktreiber gehen nicht - die Laufwerke kommen vom DEM.
 */
#include "cm.h"

extern u16 brk_set(unsigned on);
static u8  rq[0x20];                    /* INIT: bis 18h (Fehlermerker 17h) */
static char dpath[80];

static void msg(const char *s)
{
    while (*s)
        con_out((u8)*s++);
}

static void msg_line(unsigned n)
{
    char b[6];
    int i = 5;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n && i);
    msg("Fehler in CONFIG.SYS der Box, Zeile ");
    msg(b + i);
    msg("\r\n");
}

static int key(const u8 __far *p, const char *k)
{
    while (*k) {
        u8 c = *p++;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c != (u8)*k++)
            return 0;
    }
    return 1;
}

/* Treiber laden; p -> Text hinter '=' (Zeile endet mit CR/LF/0) */
static void device(u8 __far *p, int high, unsigned line)
{
    u16 seg, big, end;
    u32 sz;
    unsigned i, n, o, e;
    u8 __far *a;
    while (*p == ' ' || *p == '\t')
        p++;
    a = p;
    for (i = 0; i < 79 && p[i] > ' '; i++)
        dpath[i] = (char)p[i];
    dpath[i] = 0;
    if (!i) {
        msg_line(line);
        return;
    }
    sz = file_size(dpath);
    if (!sz) {
        msg("Nicht gefunden: ");
        msg(dpath);
        msg("\r\n");
        return;
    }
    seg = sz < 0xFFF0UL ? dev_alloc(high, (u16)((sz + 15) >> 4) + 0x10, &big) : 0;
    if (!seg) {
        msg("Zu wenig Speicher fuer ");
        msg(dpath);
        msg("\r\n");
        return;
    }
    if ((e = load_ovl(dpath, seg)) != 0) {
        mem_free(seg);
        msg(e == E_NOFILE || e == E_NOPATH ? "Nicht gefunden: " : "Fehler beim Laden: ");
        msg(dpath);
        msg("\r\n");
        return;
    }
    if (!(W(seg, 4) & 0x8000)) {                /* Blockgeraet           */
        mem_free(seg);
        msg("Blockgeraete-Treiber gehen in der Box nicht: ");
        msg(dpath);
        msg("\r\n");
        return;
    }
    for (i = 0; i < sizeof(rq); i++)
        rq[i] = 0;
    rq[0] = 0x17;                               /* INIT                  */
    W(kseg, (u16)rq + 0x0E) = 0;                /* hoechstes Ende        */
    W(kseg, (u16)rq + 0x10) = seg + big;
    W(kseg, (u16)rq + 0x12) = FP_OFF(a);        /* Befehlszeile          */
    W(kseg, (u16)rq + 0x14) = FP_SEG(a);
    sda[1]++;                                   /* InDOS: INT 21h im INIT
                                                   nimmt den 2. Stapel   */
    devcall(seg, W(seg, 6), W(seg, 8), (u16)rq);
    sda[1]--;
    o = W(kseg, (u16)rq + 0x0E);
    end = W(kseg, (u16)rq + 0x10) + ((o + 15) >> 4);
    if ((W(kseg, (u16)rq + 3) & 0x8000) || end <= seg) {
        mem_free(seg);                          /* Treiber will nicht    */
        msg("Treiber nicht geladen: ");
        msg(dpath);
        msg("\r\n");
        return;
    }
    mem_resize(seg, end - seg, &big);
    dev_name(seg, dpath);
    /* Koepfe der Datei verketten, der letzte zeigt auf den alten Nachfolger
       von NUL */
    for (o = 0, n = 0; n < 16; n++) {
        if (!con_dev && (W(seg, o + 4) & 0x8003) == 0x8003)
            con_dev = ((u32)seg << 16) | o;     /* neues CON (ANSI.SYS)  */
        if (W(seg, o) == 0xFFFF)
            break;
        W(seg, o + 2) = seg;
        o = W(seg, o);
    }
    dev_link(((u32)seg << 16) | 0, seg, o);
}

void config_box(unsigned drv)
{
    static char path[] = "?:\\DOS\\MDOS\\CONFIG.SYS";
    unsigned n, line = 0;
    u16 tb;
    u8 __far *p, __far *e, __far *z;
    path[0] = (char)('A' + drv);
    tb = mem_alloc_top(0x100);              /* 4 KB, nur waehrend des Lesens */
    if (!tb)
        return;
    n = read_file(path, tb, 0x1000);
    z = (u8 __far *)MK_FP(tb, n);
    for (p = (u8 __far *)MK_FP(tb, 0); p < z; p = e) {
        line++;
        for (e = p; e < z && *e != '\n'; e++)
            ;
        if (e < z)
            e++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\r' || *p == '\n' || *p == ';' || !*p || key(p, "REM"))
            continue;
        if (key(p, "DEVICEHIGH="))
            device(p + 11, 1, line);
        else if (key(p, "DEVICE="))
            device(p + 7, 0, line);
        else if (key(p, "BUFFERS=")) {
            unsigned v = 0;
            for (p += 8; *p >= '0' && *p <= '9'; p++)
                v = v * 10 + (*p - '0');
            buffers_set(v);
        } else if (key(p, "BREAK=ON"))
            brk_set(1);
        else if (key(p, "BREAK=OFF"))
            brk_set(0);
        else {
            u8 __far *q = p;                    /* Schluessel=...: still */
            while (q < e && *q != '=' && *q > ' ')
                q++;
            if (q >= e || *q != '=')
                msg_line(line);
        }
    }
    mem_free(tb);
}
