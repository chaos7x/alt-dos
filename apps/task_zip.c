/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ZIP.EXE - Dateien in ein ZIP-Archiv packen (Deflate, wie PKZIP 2.x/
 * Info-ZIP); Bedienung wie Info-ZIP zip (nur Verhalten nachgebaut, kein
 * fremder Code; Format nach APPNOTE.TXT):
 *
 *   ZIP [-r] [-j] [-0..-9] [-q] Archiv[.ZIP] Datei...
 *
 * Gibt es das Archiv schon, kommen die Dateien dazu; ein Eintrag mit
 * gleichem Namen wird ersetzt (seine alten Daten bleiben als Luecke im
 * Archiv - wie bei PKZIP ohne Optimieren). -0 speichert nur. Namen wie
 * eingegeben, mit '/', ohne Laufwerk und fuehrenden Strich; -j nur der
 * Dateiname. Zeichen ueber 7Fh bleiben in der aktiven Codepage (wie
 * PKZIP unter DOS). Hoechstens 4 GB, keine Verschluesselung.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"
#include "flate.h"
#include "arcutil.h"

static char args[FAPI_ARGLEN], arcname[FAPI_PATHLEN], arcfull[FAPI_PATHLEN];
static char wbuf[FAPI_PATHLEN], full[FAPI_PATHLEN], dir[FAPI_PATHLEN], pat[FAPI_PATHLEN];
static char aname[FAPI_PATHLEN], abase[FAPI_PATHLEN], tree[FAPI_PATHLEN];
static int o_r, o_j, o_q, level = 6;
static unsigned harc, hin, errors, added, fbase;
static unsigned long apos;              /* Schreibstelle im Archiv       */
static unsigned long crc, usize;
static unsigned char lh[30], db[2048];
static int werr;

/* Zentralverzeichnis: Saetze (46 Byte + Name) in bis zu 8 Segmenten */
#define NCS 8
static unsigned cseg[NCS], ncseg;
static unsigned long cdlen;             /* Bytes darin                    */
static unsigned nent;

static void say(const char *a, const char *b, const char *c)
{
    if (a) puts(a);
    if (b) puts(b);
    if (c) puts(c);
}

static void p16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void p32(unsigned char *p, unsigned long v)
{
    p16(p, (unsigned)v);
    p16(p + 2, (unsigned)(v >> 16));
}
static unsigned w16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long w32(const unsigned char *p) { return w16(p) | ((unsigned long)w16(p + 2) << 16); }

static unsigned char __far *cd_at(unsigned long off)
{
    return (unsigned char __far *)MK_FP(cseg[(unsigned)(off >> 16)], (unsigned)off);
}

static int cd_put(const unsigned char *p, unsigned n)   /* anhaengen    */
{
    unsigned i, s;
    for (i = 0; i < n; i++) {
        s = (unsigned)(cdlen >> 16);
        if (s >= ncseg) {
            if (ncseg == NCS)
                return -1;
            cseg[ncseg] = DosAllocSeg(0);
            if (FAPI_FAILED(cseg[ncseg]) || !cseg[ncseg])
                return -1;
            ncseg++;
        }
        *cd_at(cdlen++) = p[i];
    }
    return 0;
}

static int arc_wr(const unsigned char *p, unsigned n)
{
    unsigned k;
    while (n) {
        k = n > 4096 ? 4096 : n;
        if (DosWrite(harc, p, k) != k) {
            werr = 1;
            return 1;
        }
        apos += k;
        p += k;
        n -= k;
    }
    return 0;
}

static int arc_seek(unsigned long pos)
{
    static unsigned long sp;
    sp = pos;
    return FAPI_FAILED(DosSeek(harc, &sp));
}

static unsigned rd_in(unsigned char *b, unsigned n)
{
    unsigned r = DosRead(hin, b, n);
    if (FAPI_FAILED(r))
        return 0;
    crc = crc32(crc, b, r);
    usize += r;
    return r;
}

static unsigned w16cd(unsigned long off)
{
    return *cd_at(off) | ((unsigned)*cd_at(off + 1) << 8);
}

/* Eintrag gleichen Namens im Zentralverzeichnis streichen */
static void cd_drop(const char *name)
{
    unsigned long off = 0, rec, end, j;
    unsigned n, i, k = slen(name);

    while (off < cdlen) {
        rec = 46UL + w16cd(off + 28) + w16cd(off + 30) + w16cd(off + 32);
        n = w16cd(off + 28);
        if (n == k) {
            for (i = 0; i < n; i++)
                if (up(*cd_at(off + 46 + i)) != up(name[i]))
                    break;
            if (i == n) {
                end = off + rec;
                for (j = off; end < cdlen; j++, end++)
                    *cd_at(j) = *cd_at(end);
                cdlen -= rec;
                nent--;
                return;
            }
        }
        off += rec;
    }
}

static void add_file(const char *path, const char *name, struct fapi_dirent *d, int isdir)
{
    static unsigned char ch[46];
    unsigned long hpos = apos, csize;
    unsigned nl = slen(name), meth = level ? 8 : 0;
    int rc = 0;

    if (!isdir && same(path, arcfull) && slen(path) == slen(arcfull))
        return;                         /* das Archiv selbst             */
    if (!isdir) {
        hin = DosOpen(path);
        if (FAPI_FAILED(hin)) {
            say("zip: ", path, ": kann nicht gelesen werden\r\n");
            errors++;
            return;
        }
    } else
        meth = 0;
    p32(lh, 0x04034B50UL);
    p16(lh + 4, 20);
    p16(lh + 6, 0);
    p16(lh + 8, meth);
    p16(lh + 10, d->time);
    p16(lh + 12, d->date);
    p32(lh + 14, 0);
    p32(lh + 18, 0);
    p32(lh + 22, 0);
    p16(lh + 26, nl);
    p16(lh + 28, 0);
    arc_wr(lh, 30);
    arc_wr((const unsigned char *)name, nl);
    crc = 0;
    usize = 0;
    if (!isdir) {
        if (meth == 8) {
            fl_reset();
            fl_read = rd_in;
            fl_write = arc_wr;
            rc = deflate(level);
        } else {
            unsigned n;
            while ((n = rd_in(db, sizeof(db))) != 0 && !werr)
                arc_wr(db, n);
        }
        DosClose(hin);
    }
    if (rc || werr) {
        werr = 1;
        return;
    }
    csize = apos - hpos - 30 - nl;
    p32(lh + 14, crc);                  /* Kopf nachtragen               */
    p32(lh + 18, csize);
    p32(lh + 22, usize);
    if (arc_seek(hpos + 14) || DosWrite(harc, lh + 14, 12) != 12 || arc_seek(apos)) {
        werr = 1;
        return;
    }
    cd_drop(name);
    p32(ch, 0x02014B50UL);
    p16(ch + 4, 20);                    /* erzeugt: MS-DOS (FAT), 2.0    */
    p16(ch + 6, 20);
    p16(ch + 8, 0);
    p16(ch + 10, meth);
    p16(ch + 12, d->time);
    p16(ch + 14, d->date);
    p32(ch + 16, crc);
    p32(ch + 20, csize);
    p32(ch + 24, usize);
    p16(ch + 28, nl);
    p16(ch + 30, 0);
    p16(ch + 32, 0);
    p16(ch + 34, 0);
    p16(ch + 36, 0);
    p32(ch + 38, isdir ? 0x10 : (d->attr & 0x27));
    p32(ch + 42, hpos);
    if (cd_put(ch, 46) || cd_put((const unsigned char *)name, nl)) {
        puts("zip: zu viele Eintr\x84ge\r\n");
        werr = 1;
        return;
    }
    nent++;
    added++;
    if (!o_q) {
        say("  ", isdir ? "angelegt: " : meth ? "gepackt:  " : "speichert:", " ");
        puts(name);
        if (!isdir) {
            puts(" (");
            put_ratio(usize, csize);
            puts(")");
        }
        puts("\r\n");
    }
}

static void make_aname(const char *path)
{
    char *p;
    if (o_j)
        scopy(aname, base_name((char *)path), sizeof(aname));
    else {
        scopy(aname, abase, sizeof(aname));
        scat(aname, path + fbase, sizeof(aname));
    }
    for (p = aname; *p; p++)
        if (*p == '\\')
            *p = '/';
    for (p = aname; *p == '/'; p++)
        ;
    if (p != aname)
        scopy(aname, p, sizeof(aname));
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
        if (!o_j) {
            scat(aname, "/", sizeof(aname));
            add_file(path, aname, d, 1);
        }
        return WT_DESCEND;
    }
    add_file(path, aname, d, 0);
    return 0;
}

static void add_arg(const char *arg)
{
    static struct fapi_dirent e;
    unsigned n, found = 0;
    char *b;
    const char *t;

    abs_path(arg, dir);
    b = base_name(dir);
    scopy(pat, b, sizeof(pat));
    *b = 0;
    if (slen(dir) > 3)
        dir[slen(dir) - 1] = 0;
    t = arg;
    if (t[0] && t[1] == ':')
        t += 2;
    scopy(wbuf, t, sizeof(wbuf));
    b = base_name(wbuf);
    *b = 0;
    for (n = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR && !werr; n++) {
        if (!match(e.name, pat) || (e.attr & 0x08) || (e.name[0] == '.' &&
            (!e.name[1] || (e.name[1] == '.' && !e.name[2]))))
            continue;
        if ((e.attr & 0x10) && !o_r)
            continue;
        found++;
        join(full, dir, e.name, sizeof(full));
        scopy(abase, wbuf, sizeof(abase));
        scat(abase, e.name, sizeof(abase));
        fbase = slen(full);
        make_aname(full);
        if (e.attr & 0x10) {
            if (!o_j) {
                scat(aname, "/", sizeof(aname));
                add_file(full, aname, &e, 1);
            }
            scopy(tree, full, sizeof(tree));
            walk_tree(tree, visit);
        } else
            add_file(full, aname, &e, 0);
    }
    if (!found) {
        say("zip: ", arg, ": nicht gefunden\r\n");
        errors++;
    }
}

/* vorhandenes Archiv: Zentralverzeichnis einlesen, dort weiterschreiben */
static int open_old(void)
{
    static struct fapi_dirent e;
    static unsigned long sp;
    unsigned n, i, k;
    unsigned long size = 0, start, pos, cdoff, cdsz;
    char *w;

    scopy(full, arcfull, sizeof(full));
    w = base_name(full);
    scopy(wbuf, w, sizeof(wbuf));
    *w = 0;
    if (slen(full) > 3)
        full[slen(full) - 1] = 0;
    for (n = 0; DosFindEntry(&e, n, full) != FAPI_ERROR; n++)
        if (same(e.name, wbuf) && slen(e.name) == slen(wbuf)) {
            size = e.size;
            break;
        }
    for (start = size; start > 0;) {    /* Ende des Verzeichnisses       */
        n = start > sizeof(db) - 21 ? sizeof(db) - 21 : (unsigned)start;
        pos = start - n;
        if (arc_seek(pos))
            return -1;
        k = DosRead(harc, db, n + 21 > size - pos ? (unsigned)(size - pos) : n + 21);
        if (FAPI_FAILED(k))
            return -1;
        for (i = k >= 22 ? k - 22 + 1 : 0; i-- > 0;)
            if (db[i] == 'P' && db[i + 1] == 'K' && db[i + 2] == 5 && db[i + 3] == 6) {
                nent = w16(db + i + 10);
                cdsz = w32(db + i + 12);
                cdoff = w32(db + i + 16);
                if (arc_seek(cdoff))
                    return -1;
                for (pos = 0; pos < cdsz;) {        /* einlesen          */
                    n = cdsz - pos > sizeof(db) ? sizeof(db) : (unsigned)(cdsz - pos);
                    if (DosRead(harc, db, n) != n || cd_put(db, n))
                        return -1;
                    pos += n;
                }
                apos = cdoff;           /* neue Daten ab dort            */
                return arc_seek(apos);
            }
        start = pos;
        if (size - start > 65536UL + 22)
            break;
    }
    (void)sp;
    return -1;
}

static void usage(void)
{
    puts("Packt Dateien in ein ZIP-Archiv (Deflate wie PKZIP 2/Info-ZIP).\r\n\r\n"
         "ZIP [Optionen] Archiv[.ZIP] Datei...\r\n\r\n"
         "  -r  Verzeichnisse mit Inhalt     -j  ohne Verzeichnisnamen\r\n"
         "  -0  nur speichern  -1 schnell ... -9 am kleinsten (Vorgabe -6)\r\n"
         "  -q  still\r\n\r\n"
         "Ein vorhandenes Archiv wird erg\x84nzt (gleiche Namen ersetzt).\r\n");
}

void main(void)
{
    static unsigned char eo[22];
    static char rest[FAPI_ARGLEN];
    char *a, *w;
    unsigned k, rl = 0, h;
    unsigned long cdstart, off;

    DosGetArgs(args);
    a = skip(args);
    if (!*a) {
        usage();
        DosExit(16);
    }
    for (; *a; a = next_word(a)) {
        word(a, wbuf, sizeof(wbuf));
        w = wbuf;
        if ((w[0] == '-' || (w[0] == '/' && w[1] == '?')) && w[1]) {
            for (w++; *w; w++)
                switch (*w) {
                case 'r': o_r = 1; break;
                case 'j': o_j = 1; break;
                case 'q': o_q = 1; break;
                case 'h': case '?': usage(); DosExit(0);
                default:
                    if (*w >= '0' && *w <= '9')
                        level = *w - '0';
                    else {
                        say("zip: unbekannte Option ", wbuf, " (-h zeigt die Optionen)\r\n");
                        DosExit(16);
                    }
                }
            continue;
        }
        if (!arcname[0]) {
            scopy(arcname, wbuf, sizeof(arcname));
            continue;
        }
        k = slen(wbuf);
        if (rl + k + 4 < sizeof(rest)) {
            unsigned j, sp = 0;
            for (j = 0; j < k; j++)
                if (wbuf[j] == ' ')
                    sp = 1;
            if (sp) rest[rl++] = '"';
            for (j = 0; j < k; j++)
                rest[rl++] = wbuf[j];
            if (sp) rest[rl++] = '"';
            rest[rl++] = ' ';
            rest[rl] = 0;
        }
    }
    if (!arcname[0] || !rl) {
        usage();
        DosExit(16);
    }
    k = slen(arcname);
    {
        unsigned i, dot = 0;
        for (i = 0; i < k; i++)
            if (arcname[i] == '.')
                dot = 1;
            else if (arcname[i] == '\\' || arcname[i] == ':')
                dot = 0;
        if (!dot)
            scat(arcname, ".zip", sizeof(arcname));
    }
    abs_path(arcname, arcfull);
    if (level && deflate_open()) {
        puts("zip: nicht gen\x81gend Arbeitsspeicher\r\n");
        DosExit(4);
    }
    h = DosOpenRW(arcfull);
    if (!FAPI_FAILED(h)) {
        harc = h;
        if (open_old()) {
            say("zip: ", arcname, ": kein ZIP-Archiv - nicht ver\x84ndert\r\n");
            DosClose(harc);
            DosExit(3);
        }
    } else {
        harc = DosCreate(arcfull);
        if (FAPI_FAILED(harc)) {
            say("zip: ", arcname, ": kann nicht angelegt werden\r\n");
            DosExit(15);
        }
    }
    for (a = rest; *a && !werr; a = next_word(a)) {
        word(a, wbuf, sizeof(wbuf));
        scopy(full, wbuf, sizeof(full));
        add_arg(full);
    }
    /* Zentralverzeichnis + Ende schreiben */
    cdstart = apos;
    for (off = 0; off < cdlen && !werr;) {
        unsigned n = cdlen - off > sizeof(db) ? sizeof(db) : (unsigned)(cdlen - off), i;
        for (i = 0; i < n; i++)
            db[i] = *cd_at(off + i);
        arc_wr(db, n);
        off += n;
    }
    p32(eo, 0x06054B50UL);
    p16(eo + 4, 0);
    p16(eo + 6, 0);
    p16(eo + 8, nent);
    p16(eo + 10, nent);
    p32(eo + 12, cdlen);
    p32(eo + 16, cdstart);
    p16(eo + 20, 0);
    arc_wr(eo, 22);
    {
        static unsigned long ns;
        ns = apos;                      /* altes Ende abschneiden        */
        DosNewSize(harc, &ns);
    }
    DosClose(harc);
    if (level)
        deflate_close();
    if (werr) {
        puts("zip: Schreibfehler (Datentr\x84ger voll?) - Archiv unvollst\x84ndig\r\n");
        DosExit(14);
    }
    if (!added && !errors)
        puts("zip: nichts zu tun\r\n");
    DosExit(errors ? 18 : 0);
}
