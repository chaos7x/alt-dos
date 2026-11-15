/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GZIP.EXE / GUNZIP.EXE - Dateien im gzip-Format (RFC 1952) packen und
 * entpacken, Bedienung wie GNU gzip (nur Verhalten nachgebaut, kein
 * GNU-Code; Deflate aus flate.c):
 *
 *   GZIP [-d] [-c] [-k] [-f] [-l] [-t] [-v] [-q] [-1..-9] [Datei...]
 *   GUNZIP = GZIP -d
 *
 * Ohne Datei: Standardeingabe -> Standardausgabe. Gepackt wird BRIEF.TXT
 * zu BRIEF.TXT.gz (langer Name); geht das nicht (8.3-Laufwerk unter
 * MS-DOS), wie gzip fuer DOS: BRIEF.TXZ bzw. NAME.GZ. Die Quelle wird
 * danach geloescht (nicht mit -k oder -c), das Ziel bekommt ihr Datum.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"
#include "flate.h"
#include "arcutil.h"

static char args[FAPI_ARGLEN], spec[FAPI_PATHLEN], dir[FAPI_PATHLEN], pat[FAPI_PATHLEN];
static char full[FAPI_PATHLEN], tgt[FAPI_PATHLEN], hname[FAPI_PATHLEN];
static int o_d, o_c, o_k, o_f, o_l, o_t, o_v, o_q, level = 6;
static unsigned hin, hout, errors, nfiles;
static unsigned long crc, isize;
static unsigned char hb[16];
static struct fapi_dirent e;

static void say(const char *a, const char *b, const char *c)
{
    if (a) puts(a);
    if (b) puts(b);
    if (c) puts(c);
}

static unsigned rd(unsigned char *b, unsigned n)
{
    unsigned r = DosRead(hin, b, n);
    return FAPI_FAILED(r) ? 0 : r;
}

static unsigned rd_crc(unsigned char *b, unsigned n)
{
    unsigned r = rd(b, n);
    crc = crc32(crc, b, r);
    isize += r;
    return r;
}

static int wr(const unsigned char *p, unsigned n)
{
    unsigned k;
    while (n) {                         /* in Stuecken (Kernel/Pipes)    */
        k = n > 4096 ? 4096 : n;
        if (DosWrite(hout, p, k) != k)
            return 1;
        p += k;
        n -= k;
    }
    return 0;
}

static int wr_crc(const unsigned char *p, unsigned n)
{
    crc = crc32(crc, p, n);
    isize += n;
    return o_t ? 0 : wr(p, n);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static unsigned long get32(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

static int is_console(unsigned h)
{
    return DosQHandType(h) == HT_CONSOLE;
}

/* ---- packen -------------------------------------------------------------- */
static int pack(const char *name, unsigned date, unsigned time)
{
    unsigned n, rc;
    const char *b = name ? base_name((char *)name) : "";

    hb[0] = 0x1F; hb[1] = 0x8B; hb[2] = 8;
    hb[3] = name ? 8 : 0;               /* FNAME                         */
    put32(hb + 4, name ? fat2unix(date, time) : 0);
    hb[8] = (unsigned char)(level == 9 ? 2 : level == 1 ? 4 : 0);
    hb[9] = 0;                          /* FAT-Dateisystem (MS-DOS)      */
    if (wr(hb, 10))
        return FL_EWRITE;
    if (name) {
        n = slen(b);
        if (wr((const unsigned char *)b, n + 1))
            return FL_EWRITE;
    }
    crc = 0;
    isize = 0;
    fl_reset();
    fl_read = rd_crc;
    fl_write = wr;
    fl_nin = fl_nout = 0;
    rc = deflate(level);
    if (rc)
        return rc;
    put32(hb, crc);
    put32(hb + 4, isize);
    return wr(hb, 8) ? FL_EWRITE : FL_OK;
}

/* Zielname: NAME.EXT.gz, sonst 8.3 wie gzip fuer DOS */
static unsigned create_gz(const char *from)
{
    unsigned h, k, dot = 0xFFFF, i;
    scopy(tgt, from, sizeof(tgt));
    scat(tgt, ".gz", sizeof(tgt));
    if (!o_f && !FAPI_FAILED(h = DosOpen(tgt))) {
        DosClose(h);
        return 0xFFFE;
    }
    h = DosCreate(tgt);
    if (!FAPI_FAILED(h))
        return h;
    scopy(tgt, from, sizeof(tgt));      /* 8.3: letztes Zeichen -> z    */
    k = slen(tgt);
    for (i = 0; i < k; i++)
        if (tgt[i] == '.')
            dot = i;
        else if (tgt[i] == '\\')
            dot = 0xFFFF;
    if (dot == 0xFFFF)
        scat(tgt, ".gz", sizeof(tgt));
    else if (k - dot - 1 < 3)
        scat(tgt, "z", sizeof(tgt));
    else
        tgt[k - 1] = 'z';
    if (!o_f && !FAPI_FAILED(h = DosOpen(tgt))) {
        DosClose(h);
        return 0xFFFE;
    }
    return DosCreate(tgt);
}

static void compress_file(const char *path, unsigned date, unsigned time)
{
    static struct fapi_finfo fi;
    int rc;
    unsigned k = slen(path);

    if (k > 3 && (same(path + k - 3, ".GZ") || same(path + k - 4, ".TGZ"))) {
        if (!o_q)
            say("gzip: ", path, " hat schon die Endung .gz -- unver\x84ndert\r\n");
        return;
    }
    hin = DosOpen(path);
    if (FAPI_FAILED(hin)) {
        say("gzip: ", path, ": nicht gefunden\r\n");
        errors++;
        return;
    }
    if (o_c)
        hout = STDOUT;
    else {
        hout = create_gz(path);
        if (hout == 0xFFFE) {
            say("gzip: ", tgt, " existiert bereits; nicht \x81" "berschrieben (-f)\r\n");
            DosClose(hin);
            errors++;
            return;
        }
        if (FAPI_FAILED(hout)) {
            say("gzip: ", tgt, ": kann nicht angelegt werden\r\n");
            DosClose(hin);
            errors++;
            return;
        }
    }
    rc = pack(path, date, time);
    DosClose(hin);
    if (o_c)
        return;
    DosClose(hout);
    if (rc) {
        DosDelete(tgt);
        say("gzip: ", path, rc == FL_EWRITE ? ": Schreibfehler (Datentr\x84ger voll?)\r\n" :
                            ": nicht gen\x81gend Arbeitsspeicher\r\n");
        errors++;
        return;
    }
    fi.mask = 2;
    fi.date = date;
    fi.time = time;
    DosSetFInfo(tgt, &fi);
    if (o_v) {
        say(path, ":\t", 0);
        put_ratio(isize, fl_nout + 18 + slen(base_name((char *)path)) + 1);
        say(" -- ", o_k ? "erzeugt " : "ersetzt durch ", tgt);
        puts("\r\n");
    }
    if (!o_k)
        DosDelete(path);
}

/* ---- entpacken ----------------------------------------------------------- */
/* Kopf lesen; 0 = ok, 1 = kein gzip, 2 = Ende der Daten */
static int header(int first)
{
    int c, f, i;
    unsigned n;

    c = fl_getc();
    if (c < 0)
        return 2;
    if (c != 0x1F || fl_getc() != 0x8B)
        return first ? 1 : 3;
    if (fl_getc() != 8)
        return 1;
    f = fl_getc();
    for (i = 0; i < 6; i++)             /* Zeit, XFL, OS                */
        fl_getc();
    if (f & 4) {                        /* FEXTRA                        */
        n = fl_getc();
        n |= fl_getc() << 8;
        while (n--)
            if (fl_getc() < 0)
                return 1;
    }
    hname[0] = 0;
    if (f & 8) {                        /* FNAME                         */
        i = 0;
        while ((c = fl_getc()) > 0)
            if (i < FAPI_PATHLEN - 1)
                hname[i++] = (char)c;
        hname[i] = 0;
    }
    if (f & 16)                         /* FCOMMENT                      */
        while ((c = fl_getc()) > 0)
            ;
    if (f & 2) {                        /* FHCRC                         */
        fl_getc();
        fl_getc();
    }
    return 0;
}

/* alle Glieder von hin nach hout; Rueckgabe 0 = ok */
static int unpack(const char *name)
{
    int r, first = 1, k;
    unsigned char t[8];

    fl_reset();
    fl_read = rd;
    fl_write = wr_crc;
    fl_nin = fl_nout = 0;
    for (;;) {
        r = header(first);
        if (r == 2 && !first)
            return 0;
        if (r == 3) {
            if (!o_q)
                say("gzip: ", name, ": Rest hinter den Daten \x81" "bergangen\r\n");
            return 0;
        }
        if (r) {
            say("gzip: ", name, ": kein gzip-Format\r\n");
            return 1;
        }
        first = 0;
        crc = 0;
        isize = 0;
        r = inflate();
        if (r) {
            say("gzip: ", name, r == FL_EWRITE ? ": Schreibfehler (Datentr\x84ger voll?)\r\n" :
                                r == FL_EEOF ? ": unerwartetes Ende der Daten\r\n" :
                                ": Daten fehlerhaft\r\n");
            return 1;
        }
        for (k = 0; k < 8; k++) {
            r = fl_getc();
            if (r < 0) {
                say("gzip: ", name, ": unerwartetes Ende der Daten\r\n");
                return 1;
            }
            t[k] = (unsigned char)r;
        }
        if (get32(t) != crc || get32(t + 4) != isize) {
            say("gzip: ", name, ": Pr\x81" "fsumme falsch (CRC)\r\n");
            return 1;
        }
    }
}

/* Name ohne Endung: .gz .z -gz -z _z; .tgz/.taz -> .tar */
static int strip_name(const char *in)
{
    unsigned k;
    scopy(tgt, in, sizeof(tgt));
    k = slen(tgt);
    if (k > 3 && (same(tgt + k - 3, ".GZ") || same(tgt + k - 3, "-GZ"))) {
        tgt[k - 3] = 0;
        return 1;
    }
    if (k > 4 && (same(tgt + k - 4, ".TGZ") || same(tgt + k - 4, ".TAZ"))) {
        scopy(tgt + k - 4, ".tar", 5);
        return 1;
    }
    if (k > 2 && (same(tgt + k - 2, ".Z") || same(tgt + k - 2, "-Z") || same(tgt + k - 2, "_Z"))) {
        tgt[k - 2] = 0;
        return 1;
    }
    return 0;
}

static void decompress_file(const char *path, unsigned date, unsigned time)
{
    static struct fapi_finfo fi;
    unsigned h;
    int rc;

    if (!o_c && !o_t && !strip_name(path)) {
        if (!o_q)
            say("gzip: ", path, ": unbekannte Endung -- \x81" "bergangen\r\n");
        return;
    }
    hin = DosOpen(path);
    if (FAPI_FAILED(hin)) {
        say("gzip: ", path, ": nicht gefunden\r\n");
        errors++;
        return;
    }
    if (o_c)
        hout = STDOUT;
    else if (!o_t) {
        if (!o_f && !FAPI_FAILED(h = DosOpen(tgt))) {
            DosClose(h);
            say("gzip: ", tgt, " existiert bereits; nicht \x81" "berschrieben (-f)\r\n");
            DosClose(hin);
            errors++;
            return;
        }
        hout = DosCreate(tgt);
        if (FAPI_FAILED(hout)) {
            say("gzip: ", tgt, ": kann nicht angelegt werden\r\n");
            DosClose(hin);
            errors++;
            return;
        }
    }
    rc = unpack(path);
    DosClose(hin);
    if (o_c)
        return;
    if (o_t) {
        if (rc)
            errors++;
        else if (o_v)
            say(path, ":\tOK\r\n", 0);
        return;
    }
    DosClose(hout);
    if (rc) {
        DosDelete(tgt);
        errors++;
        return;
    }
    fi.mask = 2;
    fi.date = date;
    fi.time = time;
    DosSetFInfo(tgt, &fi);
    if (o_v) {
        say(path, ":\t", 0);
        put_ratio(fl_nout, e.size);
        say(" -- ", o_k ? "erzeugt " : "ersetzt durch ", tgt);
        puts("\r\n");
    }
    if (!o_k)
        DosDelete(path);
}

/* ---- -l ------------------------------------------------------------------ */
static unsigned long tot_c, tot_u;

static void list_file(const char *path, unsigned long size)
{
    static unsigned long pos;
    unsigned char t[4];

    hin = DosOpen(path);
    if (FAPI_FAILED(hin)) {
        say("gzip: ", path, ": nicht gefunden\r\n");
        errors++;
        return;
    }
    if (DosRead(hin, t, 2) != 2 || t[0] != 0x1F || t[1] != 0x8B || size < 18) {
        DosClose(hin);
        say("gzip: ", path, ": kein gzip-Format\r\n");
        errors++;
        return;
    }
    pos = size - 4;
    DosSeek(hin, &pos);
    if (DosRead(hin, t, 4) != 4) {
        DosClose(hin);
        errors++;
        return;
    }
    DosClose(hin);
    if (!nfiles++)
        puts("     gepackt     entpackt  Ersparnis  Name\r\n");
    put_num(size, 12, 0);
    put_num(get32(t), 13, 0);
    puts("     ");
    put_ratio(get32(t), size);
    puts("  ");
    strip_name(path);
    puts(tgt);
    puts("\r\n");
    tot_c += size;
    tot_u += get32(t);
}

/* ---- Hauptprogramm ------------------------------------------------------- */
static void usage(void)
{
    puts("Packt Dateien im gzip-Format (Endung .gz) bzw. entpackt sie.\r\n\r\n"
         "GZIP [Optionen] [Datei...]      GUNZIP [Optionen] [Datei...]\r\n\r\n"
         "  -d  entpacken              -c  auf die Standardausgabe\r\n"
         "  -k  Quelle behalten        -f  vorhandenes Ziel \x81" "berschreiben\r\n"
         "  -l  Inhalt auflisten       -t  pr\x81" "fen\r\n"
         "  -v  ausf\x81hrlich            -q  still\r\n"
         "  -1  schnell ... -9  am kleinsten (Vorgabe -6)\r\n\r\n"
         "Ohne Datei: Standardeingabe -> Standardausgabe.\r\n");
}

static int opt(char *a)
{
    if (a[1] == '-') {                  /* lange Formen wie GNU         */
        a += 2;
        if (same(a, "decompress") || same(a, "uncompress")) o_d = 1;
        else if (same(a, "stdout") || same(a, "to-stdout")) o_c = 1;
        else if (same(a, "keep")) o_k = 1;
        else if (same(a, "force")) o_f = 1;
        else if (same(a, "list")) o_l = 1;
        else if (same(a, "test")) o_t = 1;
        else if (same(a, "verbose")) o_v = 1;
        else if (same(a, "quiet")) o_q = 1;
        else if (same(a, "fast")) level = 1;
        else if (same(a, "best")) level = 9;
        else if (same(a, "help")) { usage(); DosExit(0); }
        else return 0;
        return 1;
    }
    for (a++; *a && *a != ' '; a++) {
        switch (*a) {
        case 'd': case 'D': o_d = 1; break;
        case 'c': case 'C': o_c = 1; break;
        case 'k': case 'K': o_k = 1; break;
        case 'f': case 'F': o_f = 1; break;
        case 'l': case 'L': o_l = 1; break;
        case 't': case 'T': o_t = 1; break;
        case 'v': case 'V': o_v = 1; break;
        case 'q': case 'Q': o_q = 1; break;
        case 'n': case 'N': break;      /* Name/Zeit: immer gespeichert  */
        case 'h': case 'H': case '?': usage(); DosExit(0);
        default:
            if (*a >= '1' && *a <= '9')
                level = *a - '0';
            else
                return 0;
        }
    }
    return 1;
}

static void one(const char *path, unsigned date, unsigned time, unsigned long size)
{
    if (o_l)
        list_file(path, size);
    else if (o_d || o_t)
        decompress_file(path, date, time);
    else
        compress_file(path, date, time);
}

void main(void)
{
    char *a, *b;
    unsigned n, found, d = 0, t = 0;
    int any = 0;

#ifdef GUNZIP
    o_d = 1;
#endif
    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a))
        if ((a[0] == '-' && a[1] && a[1] != ' ') || (a[0] == '/' && a[1] == '?')) {
            if (!opt(a)) {
                word(a, spec, sizeof(spec));
                say("gzip: unbekannte Option ", spec, " (-h zeigt die Optionen)\r\n");
                DosExit(1);
            }
        }
    if (o_l || o_t)
        o_d = 1;
    if (!o_d && deflate_open()) {
        puts("gzip: nicht gen\x81gend Arbeitsspeicher\r\n");
        DosExit(1);
    }
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '-' && a[1] && a[1] != ' ')
            continue;
        if (a[0] == '/' && a[1] == '?')
            continue;
        any = 1;
        word(a, spec, sizeof(spec));
        abs_path(spec, dir);
        b = base_name(dir);
        scopy(pat, b, sizeof(pat));
        *b = 0;
        if (slen(dir) > 3)
            dir[slen(dir) - 1] = 0;
        for (n = 0, found = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR; n++) {
            if ((e.attr & 0x18) || !match(e.name, pat))
                continue;
            found++;
            join(full, dir, e.name, sizeof(full));
            one(full, e.date, e.time, e.size);
        }
        if (!found) {
            say("gzip: ", spec, ": nicht gefunden\r\n");
            errors++;
        }
    }
    if (!any) {                         /* Filter                        */
        hin = STDIN;
        hout = STDOUT;
        if (o_l) {
            puts("gzip: -l braucht einen Dateinamen\r\n");
            DosExit(1);
        }
        if (!o_d && !o_f && is_console(STDOUT)) {
            puts("gzip: gepackte Daten nicht auf den Bildschirm (-f erzwingt; Hilfe: -h)\r\n");
            DosExit(1);
        }
        if (o_d) {
            if (unpack("Standardeingabe"))
                errors++;
        } else {
            fat_now(&d, &t);
            if (pack(0, d, t))
                errors++;
        }
    }
    if (o_l && nfiles > 1) {
        put_num(tot_c, 12, 0);
        put_num(tot_u, 13, 0);
        puts("     ");
        put_ratio(tot_u, tot_c);
        puts("  (zusammen)\r\n");
    }
    if (!o_d)
        deflate_close();
    DosExit(errors ? 1 : 0);
}
