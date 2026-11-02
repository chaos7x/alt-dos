/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * EXPAND.EXE - komprimierte Dateien von Microsoft-Disketten entpacken
 * (SZDD-Format, *.EX_, *.DL_ ... wie EXPAND von MS-DOS 5/6):
 *
 *   EXPAND [Laufwerk:][Pfad]Datei ... Ziel
 *
 * Ziel ist eine Datei oder ein Verzeichnis; bei einem Verzeichnis oder
 * mehreren Quellen bekommt die Datei ihren Namen zurueck (das letzte
 * Zeichen steht im Kopf). Ohne Angaben fragt EXPAND nach den Namen.
 * Verfahren (aus der Formatbeschreibung): LZSS, 4-KB-Fenster mit
 * Leerzeichen vorbelegt, Startposition 4096-16; Steuerbyte, je Bit 1 ein
 * Zeichen, 0 ein Verweis aus 2 Bytes (12 Bit Position, 4 Bit Laenge+3).
 * Nicht komprimierte Dateien werden einfach kopiert.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], src[FAPI_PATHLEN], dst[FAPI_PATHLEN], tgt[FAPI_PATHLEN];
static char *srcs[16];
static unsigned nsrc;
static char names[16][FAPI_PATHLEN / 2];
static unsigned char win[4096], ib[2048], ob[2048];
static unsigned hin, hout, ipos, ilen, opos, errors, done_n;
static int ieof;

static int getb(void)
{
    if (ipos >= ilen) {
        if (ieof)
            return -1;
        ilen = DosRead(hin, ib, sizeof(ib));
        if (ilen == FAPI_ERROR || ilen == 0) {
            ilen = 0;
            ieof = 1;
            return -1;
        }
        ipos = 0;
    }
    return ib[ipos++];
}

static int flush_out(void)
{
    if (opos && DosWrite(hout, ob, opos) != opos)
        return -1;
    opos = 0;
    return 0;
}

static int putb(unsigned char c)
{
    ob[opos++] = c;
    return opos == sizeof(ob) ? flush_out() : 0;
}

static void ask_name(const char *q, char *o)
{
    unsigned n = 0;
    char c;
    puts(q);
    for (;;) {
        c = (char)KbdCharIn();
        if (c == 13)
            break;
        if (c == 8 && n) {
            n--;
            puts("\b \b");
        } else if ((unsigned char)c >= ' ' && n < FAPI_PATHLEN - 1) {
            o[n++] = c;
            putc(c);
        }
    }
    o[n] = 0;
    puts("\r\n");
}

/* eine Datei entpacken; tdir: Ziel ist ein Verzeichnis */
static void expand_one(const char *from, const char *to, int tdir)
{
    static const unsigned char sig[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33 };
    static unsigned char hdr[14];
    unsigned i, k, pos, len, n;
    int c, b1, b2, flags, packed;
    char last;

    hin = DosOpen(from);
    if (FAPI_FAILED(hin)) {
        puts("Datei nicht gefunden - ");
        puts(from);
        puts("\r\n");
        errors++;
        return;
    }
    ipos = ilen = 0;
    ieof = 0;
    for (n = 0; n < 14; n++) {
        if ((c = getb()) < 0)
            break;
        hdr[n] = (unsigned char)c;
    }
    packed = n == 14;
    for (i = 0; i < 8 && packed; i++)
        if (hdr[i] != sig[i])
            packed = 0;
    last = packed ? (char)hdr[9] : 0;
    if (tdir) {                         /* Name zurueckholen: EX_ -> EXE */
        join(tgt, to, base_name((char *)from), sizeof(tgt));
        k = slen(tgt);
        if (packed && k && tgt[k - 1] == '_' && last)
            tgt[k - 1] = last;
    } else
        scopy(tgt, to, sizeof(tgt));
    if (same(tgt, from)) {
        puts("Quelle und Ziel sind gleich - ");
        puts(from);
        puts("\r\n");
        DosClose(hin);
        errors++;
        return;
    }
    hout = DosCreate(tgt);
    if (FAPI_FAILED(hout)) {
        puts("Ziel kann nicht angelegt werden - ");
        puts(tgt);
        puts("\r\n");
        DosClose(hin);
        errors++;
        return;
    }
    puts(from);
    puts(" -> ");
    puts(tgt);
    puts(packed ? "  entpackt\r\n" : "  kopiert\r\n");
    opos = 0;
    if (!packed) {                      /* gelesenen Kopf mitkopieren    */
        for (i = 0; i < n; i++)
            putb(hdr[i]);
        while ((c = getb()) >= 0)
            putb((unsigned char)c);
    } else {
        for (i = 0; i < 4096; i++)
            win[i] = ' ';
        pos = 4096 - 16;
        for (;;) {
            if ((flags = getb()) < 0)
                break;
            for (k = 0; k < 8; k++, flags >>= 1) {
                if (flags & 1) {        /* Zeichen                       */
                    if ((c = getb()) < 0)
                        break;
                    putb((unsigned char)c);
                    win[pos] = (unsigned char)c;
                    pos = (pos + 1) & 4095;
                } else {                /* Verweis ins Fenster           */
                    if ((b1 = getb()) < 0 || (b2 = getb()) < 0)
                        break;
                    i = (unsigned)b1 | ((unsigned)(b2 & 0xF0) << 4);
                    len = (b2 & 0x0F) + 3;
                    while (len--) {
                        c = win[i];
                        putb((unsigned char)c);
                        win[pos] = (unsigned char)c;
                        pos = (pos + 1) & 4095;
                        i = (i + 1) & 4095;
                    }
                }
            }
            if (ieof)
                break;
        }
    }
    if (flush_out()) {
        puts("Schreibfehler (Datentr\x84ger voll?) - ");
        puts(tgt);
        puts("\r\n");
        errors++;
    } else
        done_n++;
    DosClose(hout);
    DosClose(hin);
}

void main(void)
{
    static struct fapi_dirent e;
    static char dir[FAPI_PATHLEN], pat[FAPI_PATHLEN], full[FAPI_PATHLEN];
    char *a, *b;
    unsigned i, n, cnt = 0;
    int tdir;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Entpackt komprimierte Dateien von Microsoft-Disketten (*.EX_ usw.).\r\n\r\n"
             "EXPAND [Laufwerk:][Pfad]Datei [...] Ziel\r\n\r\n"
             "  Ziel  Datei oder Verzeichnis; bei einem Verzeichnis bekommen die\r\n"
             "        Dateien ihren urspr\x81nglichen Namen zur\x81" "ck.\r\n");
        return;
    }
    for (; *a && nsrc < 16; a = next_word(a)) {
        word(a, names[nsrc], sizeof(names[0]));
        srcs[nsrc] = names[nsrc];
        nsrc++;
    }
    if (nsrc == 0) {
        ask_name("Name der komprimierten Datei: ", src);
        ask_name("Name f\x81r die entpackte Datei: ", dst);
        if (!src[0] || !dst[0])
            DosExit(1);
        abs_path(src, full);
        abs_path(dst, tgt);
        scopy(dst, tgt, sizeof(dst));
        expand_one(full, dst, is_dir(dst));
        DosExit(errors ? 1 : 0);
    }
    if (nsrc < 2) {
        puts("Ziel fehlt: EXPAND Quelle Ziel\r\n");
        DosExit(1);
    }
    abs_path(srcs[nsrc - 1], dst);
    nsrc--;
    tdir = is_dir(dst);
    if (!tdir) {                        /* erst zaehlen: mehrere Quellen */
        for (i = 0, cnt = 0; i < nsrc; i++) {   /* brauchen ein Verzeichnis */
            abs_path(srcs[i], dir);
            b = base_name(dir);
            scopy(pat, b, sizeof(pat));
            *b = 0;
            if (slen(dir) > 3)
                dir[slen(dir) - 1] = 0;
            for (n = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR; n++)
                if (!(e.attr & 0x18) && match(e.name, pat))
                    cnt++;
        }
        if (cnt > 1) {
            puts("Mehrere Quelldateien: das Ziel muss ein Verzeichnis sein.\r\n");
            DosExit(1);
        }
        cnt = 0;
    }
    for (i = 0; i < nsrc; i++) {
        abs_path(srcs[i], dir);
        b = base_name(dir);
        scopy(pat, b, sizeof(pat));
        *b = 0;
        if (slen(dir) > 3)
            dir[slen(dir) - 1] = 0;
        for (n = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR; n++) {
            if ((e.attr & 0x18) || !match(e.name, pat))
                continue;
            cnt++;
            if (cnt > 1 && !tdir) {
                puts("Mehrere Quelldateien: das Ziel muss ein Verzeichnis sein.\r\n");
                DosExit(1);
            }
            join(full, dir, e.name, sizeof(full));
            expand_one(full, dst, tdir);
        }
    }
    if (!cnt) {
        puts("Datei nicht gefunden.\r\n");
        DosExit(1);
    }
    put_num(done_n, 0, 0);
    puts(" Datei(en) entpackt\r\n");
    DosExit(errors ? 1 : 0);
}
