/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TAR.EXE - Bandarchive (POSIX ustar, GNU-Erweiterungen fuer lange Namen)
 * anlegen, auflisten und entpacken; Bedienung wie GNU tar (nur Verhalten
 * nachgebaut, kein GNU-Code):
 *
 *   TAR -c|-t|-x [-v] [-z] [-k] [-f Archiv] [-C Verzeichnis] [Name...]
 *
 * Die Buchstaben duerfen wie bei tar ohne Strich zusammenstehen
 * ("tar xzvf a.tgz"). -z packt mit gzip (Deflate aus flate.c); beim
 * Lesen wird gzip am Kopf erkannt, beim Anlegen an der Endung .tgz/.gz.
 * Ohne -f bzw. mit "-f -": Standardein-/ausgabe. Namen im Archiv mit '/',
 * ohne Laufwerk und ohne fuehrenden Strich; beim Entpacken werden Namen
 * mit ".." uebergangen. Verweise (Links) werden nicht angelegt.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"
#include "flate.h"
#include "arcutil.h"

static char args[FAPI_ARGLEN], arcname[FAPI_PATHLEN], arcfull[FAPI_PATHLEN];
static char cdir[FAPI_PATHLEN], word_buf[FAPI_PATHLEN], full[FAPI_PATHLEN];
static char aname[FAPI_PATHLEN], abase[FAPI_PATHLEN], dir[FAPI_PATHLEN], pat[FAPI_PATHLEN];
static char longname[FAPI_PATHLEN], ename[FAPI_PATHLEN], opath[FAPI_PATHLEN];
static int mode, o_v, o_z, o_k, have_f;
static unsigned harc = 0xFFFF, hfile, errors;
static unsigned fbase;
static unsigned long crc, total;
static unsigned char hdr[512], buf[4096];
static struct fapi_dirent e;

static void say(const char *a, const char *b, const char *c)
{
    if (a) puts(a);
    if (b) puts(b);
    if (c) puts(c);
}

static int wr_h(unsigned h, const unsigned char *p, unsigned n)
{
    unsigned k;
    while (n) {
        k = n > 4096 ? 4096 : n;
        if (DosWrite(h, p, k) != k)
            return 1;
        p += k;
        n -= k;
    }
    return 0;
}

static int arc_out(const unsigned char *p, unsigned n)   /* fl_write  */
{
    return wr_h(harc, p, n);
}

/* ======================================================================== */
/* Anlegen                                                                  */
/* ======================================================================== */
static int werr;

static void arc_write(const unsigned char *p, unsigned n)
{
    if (werr)
        return;
    total += n;
    if (o_z) {
        crc = crc32(crc, p, n);
        if (deflate_feed(p, n))
            werr = 1;
    } else if (wr_h(harc, p, n))
        werr = 1;
}

static void hdr_sum(void)
{
    unsigned i, sum;
    for (i = 148; i < 156; i++)
        hdr[i] = ' ';
    for (sum = 0, i = 0; i < 512; i++)
        sum += hdr[i];
    oct_put((char *)hdr + 148, 7, sum);
    hdr[155] = ' ';
}

static void hdr_clear(void)
{
    unsigned i;
    for (i = 0; i < 512; i++)
        hdr[i] = 0;
}

/* Kopf schreiben; Namen ueber 100 Zeichen: ustar-Vorsilbe (bis 155) oder,
   wenn das nicht passt, ein GNU-Eintrag 'L' mit dem ganzen Namen davor */
static void put_hdr(const char *name, char type, unsigned long size,
                    unsigned date, unsigned time, unsigned attr)
{
    unsigned i, j, k, n = slen(name), cut = 0;

    if (n > 100) {
        for (i = 1; i < n && i <= 155; i++)
            if (name[i] == '/' && n - i - 1 <= 100)
                cut = i;
        if (!cut) {
            hdr_clear();
            scopy((char *)hdr, "././@LongLink", 100);
            oct_put((char *)hdr + 100, 8, 0644);
            oct_put((char *)hdr + 108, 8, 0);
            oct_put((char *)hdr + 116, 8, 0);
            oct_put((char *)hdr + 124, 12, n + 1);
            oct_put((char *)hdr + 136, 12, 0);
            hdr[156] = 'L';
            scopy((char *)hdr + 257, "ustar  ", 8);
            hdr_sum();
            arc_write(hdr, 512);
            for (i = 0; i < n + 1; i += 512) {
                hdr_clear();
                k = n + 1 - i > 512 ? 512 : n + 1 - i;
                for (j = 0; j < k && i + j < n; j++)
                    hdr[j] = (unsigned char)name[i + j];
                arc_write(hdr, 512);
            }
        }
    }
    hdr_clear();
    if (cut) {
        for (i = 0; i < cut; i++)
            hdr[345 + i] = (unsigned char)name[i];
        for (i = cut + 1; i < n; i++)
            hdr[i - cut - 1] = (unsigned char)name[i];
    } else
        for (i = 0; i < n && i < 100; i++)
            hdr[i] = (unsigned char)name[i];
    oct_put((char *)hdr + 100, 8, type == '5' ? 0755 : (attr & 1) ? 0444 : 0644);
    oct_put((char *)hdr + 108, 8, 0);
    oct_put((char *)hdr + 116, 8, 0);
    oct_put((char *)hdr + 124, 12, size);
    oct_put((char *)hdr + 136, 12, fat2unix(date, time));
    hdr[156] = (unsigned char)type;
    scopy((char *)hdr + 257, "ustar", 6);
    hdr[263] = '0';
    hdr[264] = '0';
    hdr_sum();
    arc_write(hdr, 512);
}

static void add_file(const char *path, const char *name, struct fapi_dirent *d)
{
    unsigned h, n;
    unsigned long left = d->size;

    if (same(path, arcfull) && slen(path) == slen(arcfull)) {
        say("tar: ", name, ": ist das Archiv selbst -- \x81" "bergangen\r\n");
        return;
    }
    h = DosOpen(path);
    if (FAPI_FAILED(h)) {
        say("tar: ", path, ": kann nicht gelesen werden\r\n");
        errors++;
        return;
    }
    put_hdr(name, '0', d->size, d->date, d->time, d->attr);
    while (left && !werr) {
        n = DosRead(h, buf, left > sizeof(buf) ? sizeof(buf) : (unsigned)left);
        if (FAPI_FAILED(n) || !n) {     /* kuerzer geworden: auffuellen */
            say("tar: ", path, ": Lesefehler, mit Nullen aufgef\x81llt\r\n");
            errors++;
            for (n = 0; n < sizeof(buf); n++)
                buf[n] = 0;
            while (left && !werr) {
                n = left > sizeof(buf) ? sizeof(buf) : (unsigned)left;
                arc_write(buf, n);
                left -= n;
            }
            break;
        }
        arc_write(buf, n);
        left -= n;
    }
    DosClose(h);
    n = (unsigned)(d->size & 511);
    if (n) {
        for (h = 0; h < 512 - n; h++)
            buf[h] = 0;
        arc_write(buf, 512 - n);
    }
    if (o_v)
        say(name, "\r\n", 0);
}

/* Archivname: Basis (wie eingegeben) + Rest des vollen Pfads, '/' */
static void make_aname(const char *path)
{
    char *p;
    scopy(aname, abase, sizeof(aname));
    scat(aname, path + fbase, sizeof(aname));
    for (p = aname; *p; p++)
        if (*p == '\\')
            *p = '/';
    p = aname;
    while (*p == '/')
        p++;
    if (p != aname)
        scopy(aname, p, sizeof(aname));
}

static void add_dir(const char *path, const char *name, unsigned date,
                    unsigned time, unsigned attr)
{
    scopy(aname, name, sizeof(aname));
    scat(aname, "/", sizeof(aname));
    put_hdr(aname, '5', 0, date, time, attr);
    if (o_v)
        say(aname, "\r\n", 0);
    (void)path;
}

static int visit(char *path, struct fapi_dirent *d, int what, int depth)
{
    (void)depth;
    if (werr)
        return WT_STOP;
    if (what == WT_LEAVE)
        return 0;
    make_aname(path);
    if (what == WT_DIR) {
        add_dir(path, aname, d->date, d->time, d->attr);
        return WT_DESCEND;
    }
    add_file(path, aname, d);
    return 0;
}

static void add_arg(const char *arg)
{
    static char tree[FAPI_PATHLEN];
    unsigned n, found = 0, k;
    char *b;
    const char *t;

    abs_path(arg, tree);
    k = slen(tree);
    if (k >= 2 && tree[k - 1] == '.' && (tree[k - 2] == '\\' || tree[k - 2] == ':')) {
        tree[--k] = 0;                  /* "." = Inhalt des Verzeichnisses */
        if (k > 3)
            tree[--k] = 0;
    }
    if (k == 3 || (k && arg[slen(arg) - 1] == '.') ) {
        abase[0] = 0;                   /* Stamm oder ".": nur Inhalt   */
        fbase = k + (tree[k - 1] == '\\' ? 0 : 1);
        walk_tree(tree, visit);
        return;
    }
    abs_path(arg, dir);
    b = base_name(dir);
    scopy(pat, b, sizeof(pat));
    *b = 0;
    if (slen(dir) > 3)
        dir[slen(dir) - 1] = 0;
    t = arg;                            /* wie eingegeben, ohne Laufwerk */
    if (t[0] && t[1] == ':')
        t += 2;
    scopy(word_buf, t, sizeof(word_buf));
    b = base_name(word_buf);
    *b = 0;
    for (n = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR && !werr; n++) {
        if (!match(e.name, pat) || (e.attr & 0x08) || (e.name[0] == '.' &&
            (!e.name[1] || (e.name[1] == '.' && !e.name[2]))))
            continue;
        found++;
        join(full, dir, e.name, sizeof(full));
        scopy(abase, word_buf, sizeof(abase));
        scat(abase, e.name, sizeof(abase));
        fbase = slen(full);
        make_aname(full);
        if (e.attr & 0x10) {
            add_dir(full, aname, e.date, e.time, e.attr);
            scopy(tree, full, sizeof(tree));
            walk_tree(tree, visit);
        } else
            add_file(full, aname, &e);
    }
    if (!found) {
        say("tar: ", arg, ": nicht gefunden\r\n");
        errors++;
    }
}

static void create(char *a)
{
    static unsigned char gh[10] = { 0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0 };
    unsigned i, any = 0;
    unsigned long pad;

    if (!have_f || (arcname[0] == '-' && !arcname[1])) {
        harc = STDOUT;
        if (DosQHandType(STDOUT) == HT_CONSOLE) {
            puts("tar: Archiv nicht auf den Bildschirm (-f Datei)\r\n");
            DosExit(2);
        }
        arcfull[0] = 0;
    } else {
        i = slen(arcname);
        if ((i > 3 && same(arcname + i - 3, ".GZ")) || (i > 4 && same(arcname + i - 4, ".TGZ")))
            o_z = 1;
        harc = DosCreate(arcfull);
        if (FAPI_FAILED(harc)) {
            say("tar: ", arcname, ": kann nicht angelegt werden\r\n");
            DosExit(2);
        }
    }
    if (o_z) {
        if (deflate_open()) {
            puts("tar: nicht gen\x81gend Arbeitsspeicher\r\n");
            DosExit(2);
        }
        wr_h(harc, gh, 10);
        fl_write = arc_out;
        crc = 0;
        deflate_begin(6);
    }
    if (cdir[0] && FAPI_FAILED(DosChDir(cdir))) {
        say("tar: ", cdir, ": Verzeichnis nicht gefunden\r\n");
        DosExit(2);
    }
    for (; *a && !werr; a = next_word(a)) {
        word(a, word_buf, sizeof(word_buf));
        scopy(full, word_buf, sizeof(full));
        any = 1;
        add_arg(full);
    }
    if (!any) {
        puts("tar: keine Dateien angegeben\r\n");
        errors++;
    }
    for (i = 0; i < 512; i++)           /* Ende: zwei leere Bloecke,     */
        buf[i] = 0;                     /* auf 10 KB aufgefuellt         */
    arc_write(buf, 512);
    arc_write(buf, 512);
    au_div(total, 10240, &pad);         /* Rest auffuellen               */
    if (pad)
        for (pad = 10240 - pad; pad; ) {
            arc_write(buf, 512);
            pad -= 512;
        }
    if (o_z) {
        unsigned long tot = total;
        if (deflate_end())
            werr = 1;
        gh[0] = (unsigned char)crc; gh[1] = (unsigned char)(crc >> 8);
        gh[2] = (unsigned char)(crc >> 16); gh[3] = (unsigned char)(crc >> 24);
        gh[4] = (unsigned char)tot; gh[5] = (unsigned char)(tot >> 8);
        gh[6] = (unsigned char)(tot >> 16); gh[7] = (unsigned char)(tot >> 24);
        if (wr_h(harc, gh, 8))
            werr = 1;
        deflate_close();
    }
    if (harc != STDOUT)
        DosClose(harc);
    if (werr) {
        puts("tar: Schreibfehler (Datentr\x84ger voll?)\r\n");
        errors++;
    }
}

/* ======================================================================== */
/* Lesen (Liste, Entpacken) - Daten kommen stueckweise (auch aus inflate)    */
/* ======================================================================== */
static unsigned hpos;                   /* Bytes im Kopfpuffer           */
static unsigned long dleft, pleft;      /* Daten / Fuellbytes des Eintrags */
static char etype;
static int zeros, ended, skipping, want_ln, want_px;
static unsigned lpos;
static unsigned edate, etime;
static char *mfirst;                    /* Namen aus der Befehlszeile    */

static int member_ok(const char *n)
{
    char *a;
    unsigned k, i;
    if (!mfirst || !*mfirst)
        return 1;
    for (a = mfirst; *a; a = next_word(a)) {
        word(a, word_buf, sizeof(word_buf));
        for (k = 0; word_buf[k]; k++)
            if (word_buf[k] == '\\')
                word_buf[k] = '/';
        while (k && word_buf[k - 1] == '/')
            word_buf[--k] = 0;
        if (has_wild(word_buf)) {
            if (match(base_name((char *)n), word_buf))
                return 1;
            continue;
        }
        for (i = 0; i < k && n[i] && up(n[i]) == up(word_buf[i]); i++)
            ;
        if (i == k && (n[k] == 0 || n[k] == '/'))
            return 1;
    }
    return 0;
}

static void mkdirs(char *p)             /* alle Elternverzeichnisse     */
{
    unsigned i;
    for (i = 3; p[i]; i++)
        if (p[i] == '\\') {
            p[i] = 0;
            DosMkDir(p);
            p[i] = '\\';
        }
}

static void list_line(void)
{
    static char t[24];
    unsigned d = edate, tm = etime;
    if (!o_v) {
        say(ename, "\r\n", 0);
        return;
    }
    puts(etype == '5' ? "d " : (etype == '1' || etype == '2') ? "l " : "- ");
    put_num(dleft, 11, 0);
    t[0] = ' ';
    t[1] = (char)('0' + (1980 + (d >> 9)) / 1000);
    t[2] = (char)('0' + (1980 + (d >> 9)) / 100 % 10);
    t[3] = (char)('0' + (1980 + (d >> 9)) / 10 % 10);
    t[4] = (char)('0' + (1980 + (d >> 9)) % 10);
    t[5] = '-';
    t[6] = (char)('0' + ((d >> 5) & 15) / 10);
    t[7] = (char)('0' + ((d >> 5) & 15) % 10);
    t[8] = '-';
    t[9] = (char)('0' + (d & 31) / 10);
    t[10] = (char)('0' + (d & 31) % 10);
    t[11] = ' ';
    t[12] = (char)('0' + (tm >> 11) / 10);
    t[13] = (char)('0' + (tm >> 11) % 10);
    t[14] = ':';
    t[15] = (char)('0' + ((tm >> 5) & 63) / 10);
    t[16] = (char)('0' + ((tm >> 5) & 63) % 10);
    t[17] = ' ';
    t[18] = 0;
    say(t, ename, "\r\n");
}

/* Kopf auswerten; setzt dleft/pleft und den Zustand */
static void header(void)
{
    unsigned i, s1 = 0, k;
    int s2 = 0;
    unsigned long size, sum;
    char *p, *q;

    for (i = 0; i < 512 && !hdr[i]; i++)
        ;
    if (i == 512) {
        if (++zeros >= 2)
            ended = 1;
        return;
    }
    zeros = 0;
    sum = oct_get((char *)hdr + 148, 8);
    for (i = 0; i < 512; i++) {
        unsigned char c = (i >= 148 && i < 156) ? ' ' : hdr[i];
        s1 += c;
        s2 += (signed char)c;
    }
    if (sum != s1 && sum != (unsigned)s2) {
        puts("tar: Archiv besch\x84" "digt (Pr\x81" "fsumme im Kopf falsch)\r\n");
        errors++;
        ended = 1;
        return;
    }
    etype = (char)hdr[156];
    size = oct_get((char *)hdr + 124, 12);
    if (etype == '5')
        size = 0;
    dleft = size;
    pleft = (512 - (unsigned)(size & 511)) & 511;
    if (etype == 'L' || etype == 'x') { /* langer Name folgt als Daten  */
        want_ln = etype == 'L';
        want_px = etype == 'x';
        lpos = 0;
        skipping = 2;                   /* Daten sammeln                */
        return;
    }
    if (longname[0]) {
        scopy(ename, longname, sizeof(ename));
        longname[0] = 0;
    } else {
        k = 0;
        if (hdr[257] == 'u' && hdr[345]) {  /* ustar-Vorsilbe          */
            for (i = 0; i < 155 && hdr[345 + i]; i++)
                ename[k++] = (char)hdr[345 + i];
            ename[k++] = '/';
        }
        for (i = 0; i < 100 && hdr[i]; i++)
            ename[k++] = (char)hdr[i];
        ename[k] = 0;
    }
    unix2fat(oct_get((char *)hdr + 136, 12), &edate, &etime);
    /* Namen bereinigen: fuehrende / und ./ weg */
    p = ename;
    while (*p == '/' || (p[0] == '.' && p[1] == '/'))
        p += *p == '/' ? 1 : 2;
    if (p != ename)
        scopy(ename, p, sizeof(ename));
    skipping = 1;
    if (!ename[0] || !member_ok(ename))
        return;
    if (mode == 't') {
        list_line();
        return;
    }
    /* entpacken */
    for (p = ename; *p; p = q) {        /* ".." als Teil: nein           */
        for (q = p; *q && *q != '/'; q++)
            ;
        if (q - p == 2 && p[0] == '.' && p[1] == '.') {
            say("tar: ", ename, ": enth\x84lt \"..\" -- \x81" "bergangen\r\n");
            return;
        }
        if (*q)
            q++;
    }
    scopy(opath, ename, sizeof(opath));
    for (p = opath; *p; p++)
        if (*p == '/')
            *p = '\\';
    k = slen(opath);
    if (k && opath[k - 1] == '\\')
        opath[--k] = 0;
    abs_path(opath, full);
    if (o_v)
        say(ename, "\r\n", 0);
    if (etype == '5') {
        mkdirs(full);
        DosMkDir(full);
        return;
    }
    if (etype != '0' && etype != 0 && etype != '7') {
        if (etype == '1' || etype == '2')
            say("tar: ", ename, ": Verweis -- \x81" "bergangen\r\n");
        return;
    }
    mkdirs(full);
    if (o_k && !FAPI_FAILED(hfile = DosOpen(full))) {
        DosClose(hfile);
        say("tar: ", ename, ": existiert -- nicht \x81" "berschrieben (-k)\r\n");
        return;
    }
    hfile = DosCreate(full);
    if (FAPI_FAILED(hfile)) {
        say("tar: ", ename, ": kann nicht angelegt werden\r\n");
        errors++;
        return;
    }
    skipping = 0;                       /* Daten in die Datei            */
}

static void file_done(void)
{
    static struct fapi_finfo fi;
    DosClose(hfile);
    fi.mask = 2;
    fi.date = edate;
    fi.time = etime;
    DosSetFInfo(full, &fi);
}

/* Archivdaten zufuehren (Rueckgabe != 0: Schreibfehler) */
static int feed(const unsigned char *p, unsigned n)
{
    unsigned k;
    while (n && !ended) {
        if (dleft) {                    /* Daten eines Eintrags          */
            k = dleft > n ? n : (unsigned)dleft;
            if (skipping == 0) {
                if (wr_h(hfile, p, k)) {
                    say("tar: ", ename, ": Schreibfehler (Datentr\x84ger voll?)\r\n");
                    DosClose(hfile);
                    errors++;
                    return 1;
                }
            } else if (skipping == 2) {
                unsigned j;
                for (j = 0; j < k; j++)
                    if (lpos < FAPI_PATHLEN - 1 && want_ln)
                        longname[lpos++] = (char)p[j];
                    else if (want_px && lpos < sizeof(buf) - 1)
                        buf[lpos++] = p[j];
            }
            dleft -= k;
            p += k;
            n -= k;
            if (!dleft) {
                if (skipping == 0)
                    file_done();
                if (skipping == 2 && want_ln) {
                    longname[lpos < FAPI_PATHLEN ? lpos : FAPI_PATHLEN - 1] = 0;
                    want_ln = 0;
                }
                if (skipping == 2 && want_px) {    /* "n path=...\n"   */
                    unsigned i = 0, s, j;
                    buf[lpos] = 0;
                    while (i < lpos) {
                        for (s = i; s < lpos && buf[s] != ' '; s++)
                            ;
                        s++;
                        if (s + 5 < lpos && buf[s] == 'p' && buf[s + 1] == 'a' &&
                            buf[s + 2] == 't' && buf[s + 3] == 'h' && buf[s + 4] == '=') {
                            for (j = 0, s += 5; s < lpos && buf[s] != '\n' &&
                                 j < FAPI_PATHLEN - 1; s++)
                                longname[j++] = (char)buf[s];
                            longname[j] = 0;
                        }
                        while (i < lpos && buf[i] != '\n')
                            i++;
                        i++;
                    }
                    want_px = 0;
                }
                skipping = 1;
            }
            continue;
        }
        if (pleft) {                    /* Fuellbytes                    */
            k = pleft > n ? n : (unsigned)pleft;
            pleft -= k;
            p += k;
            n -= k;
            continue;
        }
        k = 512 - hpos;                 /* Kopf sammeln                  */
        if (k > n)
            k = n;
        {
            unsigned j;
            for (j = 0; j < k; j++)
                hdr[hpos + j] = p[j];
        }
        hpos += k;
        p += k;
        n -= k;
        if (hpos == 512) {
            hpos = 0;
            header();
            if (!dleft && skipping == 0)
                file_done();
        }
    }
    return 0;
}

static unsigned rd_arc(unsigned char *b, unsigned n)
{
    unsigned r = DosRead(harc, b, n);
    return FAPI_FAILED(r) ? 0 : r;
}

static int feed_cb(const unsigned char *p, unsigned n)
{
    crc = crc32(crc, p, n);
    total += n;
    return feed(p, n);
}

static void extract(char *a)
{
    int c, f = -1, i, rc;
    unsigned n;

    mfirst = a;
    if (!have_f || (arcname[0] == '-' && !arcname[1]))
        harc = STDIN;
    else {
        harc = DosOpen(arcfull);
        if (FAPI_FAILED(harc)) {
            say("tar: ", arcname, ": nicht gefunden\r\n");
            DosExit(2);
        }
    }
    if (cdir[0] && FAPI_FAILED(DosChDir(cdir))) {
        say("tar: ", cdir, ": Verzeichnis nicht gefunden\r\n");
        DosExit(2);
    }
    fl_reset();
    fl_read = rd_arc;
    c = fl_getc();
    if (c == 0x1F && (f = fl_getc()) == 0x8B) {     /* gzip            */
        for (;;) {
            if (fl_getc() != 8) {
                puts("tar: unbekannte gzip-Packart\r\n");
                errors++;
                break;
            }
            f = fl_getc();
            for (i = 0; i < 6; i++)
                fl_getc();
            if (f & 4) {
                n = fl_getc();
                n |= fl_getc() << 8;
                while (n--)
                    fl_getc();
            }
            if (f & 8)
                while (fl_getc() > 0)
                    ;
            if (f & 16)
                while (fl_getc() > 0)
                    ;
            if (f & 2) {
                fl_getc();
                fl_getc();
            }
            crc = 0;
            total = 0;
            fl_write = feed_cb;
            rc = inflate();
            if (rc) {
                puts(rc == FL_EWRITE ? "tar: abgebrochen\r\n" : rc == FL_EEOF ?
                     "tar: unerwartetes Ende des Archivs\r\n" : "tar: gzip-Daten fehlerhaft\r\n");
                errors++;
                break;
            }
            {
                unsigned char t[8];
                for (i = 0; i < 8; i++)
                    t[i] = (unsigned char)fl_getc();
                if ((t[0] | ((unsigned)t[1] << 8) | ((unsigned long)t[2] << 16) |
                     ((unsigned long)t[3] << 24)) != crc) {
                    puts("tar: Pr\x81" "fsumme falsch (gzip-CRC)\r\n");
                    errors++;
                    break;
                }
            }
            c = fl_getc();
            if (c != 0x1F || fl_getc() != 0x8B)
                break;
        }
    } else {
        if (c >= 0) {
            if (c == 0x1F)
                fl_ungetc(f);
            fl_ungetc(c);
        }
        while (!ended && (n = fl_readbuf(buf, sizeof(buf))) != 0)
            if (feed(buf, n))
                break;
    }
    if (dleft && skipping == 0) {
        DosClose(hfile);
        say("tar: ", ename, ": Archiv endet mitten in der Datei\r\n");
        errors++;
    }
    if (harc != STDIN)
        DosClose(harc);
}

/* ======================================================================== */
static int is_cluster(const char *w)    /* "cvf" als erstes Wort        */
{
    const char *q;
    if (!*w)
        return 0;
    for (q = w; *q; q++)
        if (!(*q == 'c' || *q == 't' || *q == 'x' || *q == 'v' || *q == 'z' ||
              *q == 'k' || *q == 'f' || *q == 'C' || *q == 'p' || *q == 'o' || *q == 'm'))
            return 0;
    return 1;
}

static void usage(void)
{
    puts("Legt Bandarchive (.TAR, mit gzip .TGZ) an, listet und entpackt sie.\r\n\r\n"
         "TAR -c|-t|-x [-v] [-z] [-k] [-f Archiv] [-C Verzeichnis] [Name...]\r\n\r\n"
         "  -c  anlegen        -t  Inhalt zeigen     -x  entpacken\r\n"
         "  -f  Archivdatei (- = Standardein-/ausgabe)\r\n"
         "  -z  mit gzip packen (beim Lesen erkannt, beim Anlegen auch an .tgz/.gz)\r\n"
         "  -v  ausf\x81hrlich      -k  vorhandene Dateien nicht \x81" "berschreiben\r\n"
         "  -C  vorher in dieses Verzeichnis wechseln\r\n\r\n"
         "Beispiele: TAR cvf DOS.TAR \\DOS     TAR xzvf QUELLEN.TGZ\r\n");
}

void main(void)
{
    char *a, *rest, *w;
    int first = 1, need_f = 0, need_c = 0;
    static char rbuf[FAPI_ARGLEN];
    unsigned rl = 0;

    DosGetArgs(args);
    a = skip(args);
    if (!*a || (a[0] == '/' && a[1] == '?') || (a[0] == '-' && (a[1] == 'h' || a[1] == '?')) ||
        (a[0] == '-' && a[1] == '-' && up(a[2]) == 'H')) {
        usage();
        DosExit(*a ? 0 : 2);
    }
    rbuf[0] = 0;
    for (; *a; a = next_word(a)) {
        word(a, word_buf, sizeof(word_buf));
        if (need_f) {
            scopy(arcname, word_buf, sizeof(arcname));
            have_f = 1;
            need_f = 0;
            continue;
        }
        if (need_c) {
            abs_path(word_buf, cdir);
            need_c = 0;
            continue;
        }
        w = word_buf;
        if ((w[0] == '-' && w[1] && w[1] != '-') || (first && is_cluster(w))) {
            /* Buchstaben: -cvf oder (erstes Wort) cvf */
            int bad = 0;
            char *q = w[0] == '-' ? w + 1 : w;
            for (; *q; q++)
                switch (*q) {
                case 'c': case 't': case 'x':
                    if (mode && mode != *q) bad = 1;
                    mode = *q;
                    break;
                case 'v': o_v = 1; break;
                case 'z': o_z = 1; break;
                case 'k': o_k = 1; break;
                case 'f': need_f = 1; break;
                case 'C': need_c = 1; break;
                case 'p': case 'o': case 'm': break;
                default: bad = 1;
                }
            if (bad) {
                say("tar: unbekannte Option ", word_buf, " (TAR /? zeigt die Optionen)\r\n");
                DosExit(2);
            }
            first = 0;
            continue;
        }
        first = 0;
        if (w[0] == '-' && w[1] == '-') {   /* lange Formen            */
            if (same(w, "--create")) mode = 'c';
            else if (same(w, "--extract") || same(w, "--get")) mode = 'x';
            else if (same(w, "--list")) mode = 't';
            else if (same(w, "--verbose")) o_v = 1;
            else if (same(w, "--gzip") || same(w, "--gunzip")) o_z = 1;
            else if (same(w, "--keep-old-files")) o_k = 1;
            else if (same(w, "--file")) need_f = 1;
            else if (same(w, "--directory")) need_c = 1;
            else {
                say("tar: unbekannte Option ", w, "\r\n");
                DosExit(2);
            }
            continue;
        }
        /* Name: in rbuf sammeln (in Anfuehrungszeichen, wenn Leerzeichen) */
        rest = word_buf;
        if (rl + slen(rest) + 4 < sizeof(rbuf)) {
            unsigned j, sp = 0;
            for (j = 0; rest[j]; j++)
                if (rest[j] == ' ')
                    sp = 1;
            if (sp) rbuf[rl++] = '"';
            for (j = 0; rest[j]; j++)
                rbuf[rl++] = rest[j];
            if (sp) rbuf[rl++] = '"';
            rbuf[rl++] = ' ';
            rbuf[rl] = 0;
        }
    }
    if (need_f || need_c || !mode) {
        puts(!mode ? "tar: -c, -t oder -x angeben (TAR /? zeigt die Optionen)\r\n" :
                     "tar: Name hinter -f bzw. -C fehlt\r\n");
        DosExit(2);
    }
    if (have_f && !(arcname[0] == '-' && !arcname[1]))
        abs_path(arcname, arcfull);
    if (mode == 'c')
        create(rbuf);
    else
        extract(rbuf);
    DosExit(errors ? 2 : 0);
}
