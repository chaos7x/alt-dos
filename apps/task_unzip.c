/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UNZIP.EXE - ZIP-Archive (PKZIP 2.x/Info-ZIP: gespeichert und Deflate)
 * auflisten, pruefen und entpacken; Bedienung wie Info-ZIP unzip (nur
 * Verhalten nachgebaut, kein fremder Code; Format nach APPNOTE.TXT):
 *
 *   UNZIP [-l|-v|-t] [-o|-n] [-j] [-q] [-d Ziel] Archiv[.ZIP] [Name...]
 *
 * Nicht unterstuetzt: Verschluesselung, ZIP64 (ueber 4 GB), andere
 * Packarten als 0 (gespeichert) und 8 (Deflate) - solche Eintraege werden
 * gemeldet und uebergangen. Namen mit UTF-8-Kennung (Bit 11): Zeichen
 * ausserhalb ASCII werden zu '_'.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"
#include "flate.h"
#include "arcutil.h"

static char args[FAPI_ARGLEN], arcname[FAPI_PATHLEN], arcfull[FAPI_PATHLEN];
static char ddir[FAPI_PATHLEN], wbuf[FAPI_PATHLEN], ename[FAPI_PATHLEN];
static char opath[FAPI_PATHLEN], full[FAPI_PATHLEN];
static char *mfirst;
static int mode = 'x', o_o, o_n, o_j, o_q, all_yes, all_no;
static unsigned harc, hout, errors, nent;
static unsigned long crc, cleft, cdoff, cdsize, cnt_c, cnt_u;
static unsigned char cb[512], db[2048];
static int wfail;

static void say(const char *a, const char *b, const char *c)
{
    if (a) puts(a);
    if (b) puts(b);
    if (c) puts(c);
}

static unsigned w16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long w32(const unsigned char *p)
{
    return w16(p) | ((unsigned long)w16(p + 2) << 16);
}

static int seek(unsigned long pos)
{
    static unsigned long sp;
    sp = pos;
    return FAPI_FAILED(DosSeek(harc, &sp));
}

static int rd(unsigned char *b, unsigned n)     /* genau n Bytes        */
{
    unsigned r = DosRead(harc, b, n);
    return FAPI_FAILED(r) || r != n;
}

/* Ende des Zentralverzeichnisses suchen (Kommentar bis 64 KB) */
static int find_eocd(unsigned long size)
{
    unsigned long start, pos;
    unsigned n, i;

    for (start = size; start > 0;) {
        n = start > sizeof(db) - 21 ? sizeof(db) - 21 : (unsigned)start;
        pos = start - n;
        if (seek(pos))
            return -1;
        n = DosRead(harc, db, n + 21 > size - pos ? (unsigned)(size - pos) : n + 21);
        if (FAPI_FAILED(n))
            return -1;
        for (i = n >= 22 ? n - 22 + 1 : 0; i-- > 0;)
            if (db[i] == 'P' && db[i + 1] == 'K' && db[i + 2] == 5 && db[i + 3] == 6) {
                nent = w16(db + i + 10);
                cdsize = w32(db + i + 12);
                cdoff = w32(db + i + 16);
                return 0;
            }
        start = pos;
        if (size - start > 65536UL + 22)
            break;
    }
    return -1;
}

static int member_ok(const char *n)
{
    char *a;
    unsigned k, i;
    if (!mfirst || !*mfirst)
        return 1;
    for (a = mfirst; *a; a = next_word(a)) {
        word(a, wbuf, sizeof(wbuf));
        for (k = 0; wbuf[k]; k++)
            if (wbuf[k] == '\\')
                wbuf[k] = '/';
        if (has_wild(wbuf)) {
            if (match(base_name((char *)n), wbuf) || match((char *)n, wbuf))
                return 1;
            continue;
        }
        for (i = 0; i < k && n[i] && up(n[i]) == up(wbuf[i]); i++)
            ;
        if (i == k && (n[k] == 0 || n[k] == '/'))
            return 1;
    }
    return 0;
}

static unsigned rd_entry(unsigned char *b, unsigned n)   /* fl_read      */
{
    unsigned r;
    if (!cleft)
        return 0;
    if (n > cleft)
        n = (unsigned)cleft;
    r = DosRead(harc, b, n);
    if (FAPI_FAILED(r))
        return 0;
    cleft -= r;
    return r;
}

static int wr_out(const unsigned char *p, unsigned n)    /* fl_write     */
{
    unsigned k;
    crc = crc32(crc, p, n);
    if (mode != 'x' || hout == 0xFFFF)
        return 0;
    while (n) {
        k = n > 4096 ? 4096 : n;
        if (DosWrite(hout, p, k) != k) {
            wfail = 1;
            return 1;
        }
        p += k;
        n -= k;
    }
    return 0;
}

static void mkdirs(char *p)
{
    unsigned i;
    for (i = 3; p[i]; i++)
        if (p[i] == '\\') {
            p[i] = 0;
            DosMkDir(p);
            p[i] = '\\';
        }
}

static int ask(const char *name)
{
    unsigned k;
    if (all_yes)
        return 1;
    if (all_no)
        return 0;
    say("Ersetzen ", name, "? (J)a, (N)ein, (A)lle, (K)eine: ");
    for (;;) {
        k = up((char)KbdCharIn());
        if (k == 'J' || k == 'Y') { puts("J\r\n"); return 1; }
        if (k == 'N') { puts("N\r\n"); return 0; }
        if (k == 'A') { puts("A\r\n"); all_yes = 1; return 1; }
        if (k == 'K') { puts("K\r\n"); all_no = 1; return 0; }
    }
}

static void put_date(unsigned d, unsigned t)
{
    char s[18];
    s[0] = (char)('0' + (d & 31) / 10); s[1] = (char)('0' + (d & 31) % 10); s[2] = '.';
    s[3] = (char)('0' + ((d >> 5) & 15) / 10); s[4] = (char)('0' + ((d >> 5) & 15) % 10);
    s[5] = '.';
    d = 1980 + (d >> 9);
    s[6] = (char)('0' + d / 1000); s[7] = (char)('0' + d / 100 % 10);
    s[8] = (char)('0' + d / 10 % 10); s[9] = (char)('0' + d % 10);
    s[10] = ' ';
    s[11] = (char)('0' + (t >> 11) / 10); s[12] = (char)('0' + (t >> 11) % 10); s[13] = ':';
    s[14] = (char)('0' + ((t >> 5) & 63) / 10); s[15] = (char)('0' + ((t >> 5) & 63) % 10);
    s[16] = 0;
    puts(s);
}

/* einen Eintrag (Zentralkopf in cb) behandeln */
static void entry(void)
{
    static struct fapi_finfo fi;
    unsigned flag = w16(cb + 8), meth = w16(cb + 10), tm = w16(cb + 12), dt = w16(cb + 14);
    unsigned long ecrc = w32(cb + 16), csize = w32(cb + 20), usize = w32(cb + 24);
    unsigned long loff = w32(cb + 42);
    unsigned nl = w16(cb + 28), attr = cb[38], k, isdir;
    unsigned char lh[30];
    char *p;
    int rc;

    k = slen(ename);
    isdir = k && ename[k - 1] == '/';
    if (!member_ok(ename))
        return;
    (void)nl;
    if (mode == 'l' || mode == 'v') {
        put_num(usize, 10, 0);
        if (mode == 'v') {
            put_num(csize, 10, 0);
            puts("  ");
            put_ratio(usize, csize);
            puts(meth == 8 ? "  Deflate " : meth == 0 ? "  gespeich" : "  anders  ");
        }
        puts("  ");
        put_date(dt, tm);
        say("  ", ename, "\r\n");
        cnt_u += usize;
        cnt_c += csize;
        return;
    }
    if (flag & 1) {
        say("unzip: ", ename, ": verschl\x81sselt -- \x81" "bergangen\r\n");
        errors++;
        return;
    }
    if (meth != 0 && meth != 8) {
        say("unzip: ", ename, ": Packart nicht unterst\x81tzt -- \x81" "bergangen\r\n");
        errors++;
        return;
    }
    if (seek(loff) || rd(lh, 30) || w32(lh) != 0x04034B50UL) {
        say("unzip: ", ename, ": lokaler Kopf fehlt\r\n");
        errors++;
        return;
    }
    if (seek(loff + 30 + w16(lh + 26) + w16(lh + 28))) {
        errors++;
        return;
    }
    hout = 0xFFFF;
    if (mode == 'x') {
        /* Ziel: -d Verzeichnis + Name (-j: ohne Pfad), '/' -> '\' */
        p = ename;
        while (*p == '/')
            p++;
        if (o_j) {
            if (isdir)
                return;
            for (k = slen(ename); k && ename[k - 1] != '/'; k--)
                ;
            p = ename + k;
        }
        {
            char *s, *q;                /* ".." als Teil: nein           */
            for (s = p; *s; s = q) {
                for (q = s; *q && *q != '/'; q++)
                    ;
                if (q - s == 2 && s[0] == '.' && s[1] == '.') {
                    say("unzip: ", ename, ": enth\x84lt \"..\" -- \x81" "bergangen\r\n");
                    return;
                }
                if (*q)
                    q++;
            }
        }
        scopy(opath, ddir, sizeof(opath));
        if (opath[0] && opath[slen(opath) - 1] != '\\')
            scat(opath, "\\", sizeof(opath));
        scat(opath, p, sizeof(opath));
        for (p = opath; *p; p++)
            if (*p == '/')
                *p = '\\';
        k = slen(opath);
        while (k > 3 && opath[k - 1] == '\\')
            opath[--k] = 0;
        abs_path(opath, full);
        if (isdir) {
            mkdirs(full);
            DosMkDir(full);
            if (!o_q)
                say("   angelegt: ", full, "\r\n");
            return;
        }
        mkdirs(full);
        if (!FAPI_FAILED(hout = DosOpen(full))) {
            DosClose(hout);
            hout = 0xFFFF;
            if (o_n || (!o_o && !ask(full)))
                return;
        }
        hout = DosCreate(full);
        if (FAPI_FAILED(hout)) {
            say("unzip: ", full, ": kann nicht angelegt werden\r\n");
            errors++;
            return;
        }
        if (!o_q)
            say(meth == 8 ? "  entpackt: " : " gespeich.: ", full, "\r\n");
    } else if (isdir)
        return;
    crc = 0;
    cleft = csize;
    wfail = 0;
    if (meth == 8) {
        fl_reset();
        fl_read = rd_entry;
        fl_write = wr_out;
        rc = inflate();
    } else {
        unsigned n;
        rc = 0;
        while (cleft && !rc) {
            n = rd_entry(db, sizeof(db));
            if (!n)
                rc = FL_EEOF;
            else if (wr_out(db, n))
                rc = FL_EWRITE;
        }
    }
    if (hout != 0xFFFF) {
        DosClose(hout);
        if (!rc && crc == ecrc) {
            fi.mask = 3;
            fi.attr = attr & 0x27;
            fi.date = dt;
            fi.time = tm;
            DosSetFInfo(full, &fi);
        }
    }
    if (rc || crc != ecrc) {
        say("unzip: ", ename, rc == FL_EWRITE || wfail ? ": Schreibfehler (Datentr\x84ger voll?)\r\n" :
                              rc ? ": Daten fehlerhaft\r\n" : ": Pr\x81" "fsumme falsch (CRC)\r\n");
        errors++;
        if (hout != 0xFFFF)
            DosDelete(full);
        return;
    }
    if (mode == 't' && !o_q)
        say("    OK: ", ename, "\r\n");
    (void)usize;
}

static void usage(void)
{
    puts("Entpackt ZIP-Archive (gespeichert und Deflate wie PKZIP 2/Info-ZIP).\r\n\r\n"
         "UNZIP [Optionen] Archiv[.ZIP] [Name...]\r\n\r\n"
         "  -l  Inhalt zeigen     -v  ausf\x81hrlich zeigen     -t  pr\x81" "fen\r\n"
         "  -d  Zielverzeichnis   -j  ohne Verzeichnisse entpacken\r\n"
         "  -o  ohne Frage \x81" "berschreiben    -n  nie \x81" "berschreiben\r\n"
         "  -q  still\r\n");
}

void main(void)
{
    static struct fapi_dirent e;
    char *a, *w;
    unsigned i, n, k, need_d = 0;
    static char mbuf[FAPI_ARGLEN];
    unsigned ml = 0;
    unsigned long pos;

    DosGetArgs(args);
    a = skip(args);
    if (!*a) {
        usage();
        DosExit(10);
    }
    for (; *a; a = next_word(a)) {
        word(a, wbuf, sizeof(wbuf));
        w = wbuf;
        if (need_d) {
            scopy(ddir, w, sizeof(ddir));
            need_d = 0;
            continue;
        }
        if ((w[0] == '-' || (w[0] == '/' && w[1] == '?')) && w[1]) {
            for (w++; *w; w++)
                switch (*w) {
                case 'l': mode = 'l'; break;
                case 'v': mode = 'v'; break;
                case 't': mode = 't'; break;
                case 'o': o_o = 1; break;
                case 'n': o_n = 1; break;
                case 'j': o_j = 1; break;
                case 'q': o_q = 1; break;
                case 'd': need_d = 1; break;
                case 'h': case '?': usage(); DosExit(0);
                default:
                    say("unzip: unbekannte Option ", wbuf, " (-h zeigt die Optionen)\r\n");
                    DosExit(10);
                }
            continue;
        }
        if (!arcname[0]) {
            scopy(arcname, wbuf, sizeof(arcname));
            continue;
        }
        k = slen(wbuf);
        if (ml + k + 4 < sizeof(mbuf)) {
            unsigned j, sp = 0;
            for (j = 0; j < k; j++)
                if (wbuf[j] == ' ')
                    sp = 1;
            if (sp) mbuf[ml++] = '"';
            for (j = 0; j < k; j++)
                mbuf[ml++] = wbuf[j];
            if (sp) mbuf[ml++] = '"';
            mbuf[ml++] = ' ';
            mbuf[ml] = 0;
        }
    }
    mfirst = mbuf;
    if (!arcname[0]) {
        usage();
        DosExit(10);
    }
    abs_path(arcname, arcfull);
    harc = DosOpen(arcfull);
    if (FAPI_FAILED(harc)) {
        scat(arcfull, ".zip", sizeof(arcfull));
        harc = DosOpen(arcfull);
        if (FAPI_FAILED(harc)) {
            say("unzip: ", arcname, ": nicht gefunden\r\n");
            DosExit(9);
        }
    }
    /* Groesse ueber die Verzeichnissuche */
    scopy(full, arcfull, sizeof(full));
    w = base_name(full);
    scopy(wbuf, w, sizeof(wbuf));
    *w = 0;
    if (slen(full) > 3)
        full[slen(full) - 1] = 0;
    for (n = 0; DosFindEntry(&e, n, full) != FAPI_ERROR; n++)
        if (same(e.name, wbuf) && slen(e.name) == slen(wbuf))
            break;
    if (find_eocd(e.size)) {
        say("unzip: ", arcname, ": kein ZIP-Archiv (Ende des Verzeichnisses fehlt)\r\n");
        DosExit(9);
    }
    if (mode == 'l')
        puts("     L\x84nge  Datum      Zeit   Name\r\n"
             "  --------  ---------- -----  ----\r\n");
    else if (mode == 'v')
        puts("     L\x84nge   gepackt  Ersparnis Art      Datum      Zeit   Name\r\n");
    else if (mode == 't' || !o_q)
        say("Archiv: ", arcfull, "\r\n");
    pos = cdoff;
    for (i = 0; i < nent; i++) {
        if (seek(pos) || rd(cb, 46) || w32(cb) != 0x02014B50UL) {
            say("unzip: ", arcname, ": Zentralverzeichnis besch\x84" "digt\r\n");
            errors++;
            break;
        }
        n = w16(cb + 28);
        k = n < FAPI_PATHLEN - 1 ? n : FAPI_PATHLEN - 1;
        if (rd((unsigned char *)ename, k)) {
            errors++;
            break;
        }
        ename[k] = 0;
        if (w16(cb + 8) & 0x800)        /* UTF-8: nur ASCII uebernehmen */
            for (k = 0; ename[k]; k++)
                if ((unsigned char)ename[k] >= 0x80) {
                    unsigned char c = (unsigned char)ename[k];
                    unsigned j = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
                    ename[k] = '_';
                    if (j)
                        scopy(ename + k + 1, ename + k + 1 + j, sizeof(ename) - k - 1);
                }
        for (k = 0; ename[k]; k++)
            if (ename[k] == '\\')
                ename[k] = '/';
        pos += 46UL + n + w16(cb + 30) + w16(cb + 32);
        entry();
    }
    if (mode == 'l' || mode == 'v') {
        puts("  --------\r\n");
        put_num(cnt_u, 10, 0);
        if (mode == 'v') {
            put_num(cnt_c, 10, 0);
            puts("  ");
            put_ratio(cnt_u, cnt_c);
        }
        puts("  ");
        put_num(nent, 0, 0);
        puts(" Eintr\x84ge\r\n");
    } else if (mode == 't' && !errors)
        say("Keine Fehler in ", arcfull, "\r\n");
    DosClose(harc);
    DosExit(errors ? 1 : 0);
}
