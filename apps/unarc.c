/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Gemeinsamer Rahmen von UNARJ.EXE, UNZOO.EXE und UNLHA.EXE (task_unarj.c
 * usw. setzen PROG/DEFEXT und binden diese Datei ein): Archive von ARJ,
 * ZOO und LHA/LHarc/LArc auflisten, pruefen und entpacken. Jedes der drei
 * Programme erkennt alle drei Formate am Inhalt (auch selbstentpackende
 * .EXE/.COM). Bedienung wie UNZIP, dazu die alten Befehlsbuchstaben:
 *
 *   PROG [e|x|l|v|t] [-l|-v|-t] [-o|-n] [-j] [-q] [-d Ziel] Archiv [Name...]
 *
 * Formate nach den Beschreibungen (ARJ TECHNOTE, Zoo 2.10 zoo.h, LHA-
 * Kopfstufen 0-2 aus der Doku von LHa fuer UNIX). Nicht unterstuetzt:
 * verschluesselte ARJ-Eintraege, mehrteilige Archive (Teil fuer Teil
 * entpacken geht, die Datei bleibt dann unvollstaendig), LHA-Kopfstufe 3,
 * PMarc, lh2/lh3.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"
#include "lzh.h"
#include "arcutil.h"

static char args[FAPI_ARGLEN], arcname[FAPI_PATHLEN], arcfull[FAPI_PATHLEN];
static char ddir[FAPI_PATHLEN], wbuf[FAPI_PATHLEN], ename[FAPI_PATHLEN];
static char fpart[FAPI_PATHLEN], dpart[FAPI_PATHLEN];
static char opath[FAPI_PATHLEN], full[FAPI_PATHLEN];
static char mbuf[FAPI_ARGLEN];
static char *mfirst;
static int mode = 'x', o_o, o_n, o_j, o_q, all_yes, all_no;
static unsigned harc, hout, errors, nent;
static unsigned long cleft, cnt_c, cnt_u, apos;
static unsigned char hb[3072], db[2048];
static int fmt, first, wfail;

/* der laufende Eintrag */
static int  e_meth;                     /* LZ_*, 0 gespeichert, -1 nicht */
static char e_mname[8];
static unsigned long e_cs, e_us, e_crc, e_data, crc;
static unsigned e_date, e_time, e_attr, e_c32, e_dir, e_nocrc;

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

static unsigned rdn(unsigned char *b, unsigned n)   /* bis n Bytes       */
{
    unsigned r = DosRead(harc, b, n);
    return FAPI_FAILED(r) ? 0 : r;
}

static int rd(unsigned char *b, unsigned n)     /* genau n Bytes        */
{
    return rdn(b, n) != n;
}

static void cpyn(char *d, const unsigned char *s, unsigned n, unsigned max)
{
    unsigned i;
    for (i = 0; i < n && s[i] && i + 1 < max; i++)
        d[i] = (char)s[i];
    d[i] = 0;
}

/* ---- ARJ ----------------------------------------------------------------- */
static int arj_hdr(unsigned long pos, unsigned *bs)  /* 1 = gueltiger Kopf */
{
    if (seek(pos) || rd(hb, 4) || hb[0] != 0x60 || hb[1] != 0xEA)
        return 0;
    *bs = w16(hb + 2);
    if (*bs == 0)
        return 1;
    if (*bs > 2600 || *bs < 30 || rd(hb, *bs + 4))
        return 0;
    return crc32a(0, hb, *bs) == w32(hb + *bs);
}

static int arj_next(void)
{
    unsigned bs, fhs, es;
    unsigned long pos;

    for (;;) {
        if (!arj_hdr(apos, &bs))
            return -1;
        if (bs == 0)
            return 0;
        pos = apos + 4 + bs + 4;
        for (;;) {                      /* erweiterte Koepfe uebergehen */
            if (seek(pos) || rd(db, 2))
                return -1;
            pos += 2;
            es = w16(db);
            if (!es)
                break;
            pos += es + 4UL;
        }
        if (first) {                    /* Hauptkopf des Archivs        */
            first = 0;
            apos = pos;
            continue;
        }
        fhs = hb[0];
        e_cs = w32(hb + 12);
        e_us = w32(hb + 16);
        e_crc = w32(hb + 20);
        if (hb[3] == 2 || hb[3] == 8)   /* ARJ fuer UNIX/NeXT: Unix-Zeit */
            unix2fat(w32(hb + 8), &e_date, &e_time);
        else {
            e_time = w16(hb + 8);
            e_date = w16(hb + 10);
        }
        e_attr = hb[3] == 0 ? hb[26] : 0x20;  /* nur MS-DOS hat DOS-Attribute */
        e_c32 = 1;
        e_data = pos;
        apos = pos + e_cs;
        if (fhs >= bs)
            return -1;
        cpyn(ename, hb + fhs, bs - fhs, sizeof(ename));
        if (hb[6] == 4)                 /* Datentraegerbezeichnung      */
            continue;
        e_dir = hb[6] == 3;
        e_mname[0] = 'm';
        e_mname[1] = (char)('0' + (hb[5] & 7));
        e_mname[2] = 0;
        e_meth = hb[5] == 0 ? 0 : hb[5] <= 3 ? LZ_ARJ : hb[5] == 4 ? LZ_ARJ4 : -1;
        if (hb[4] & 1) {                /* verschluesselt               */
            e_meth = -1;
            scopy(e_mname, "versch", sizeof(e_mname));
        }
        if (hb[4] & 4)
            say(PROG ": ", ename, ": geht im n\x84" "chsten Teil weiter\r\n");
        return 1;
    }
}

/* ---- ZOO ----------------------------------------------------------------- */
static int zoo_next(void)
{
    unsigned n, vl, nl, dl;

    for (;;) {
        if (seek(apos))
            return -1;
        n = rdn(hb, 600);
        if (n < 51 || w32(hb) != 0xFDC4A7DCUL)
            return -1;
        if (w32(hb + 6) == 0)
            return 0;
        apos = w32(hb + 6);
        if (hb[30])                     /* geloescht                    */
            continue;
        e_data = w32(hb + 10);
        e_date = w16(hb + 14);
        e_time = w16(hb + 16);
        e_crc = w16(hb + 18);
        e_us = w32(hb + 20);
        e_cs = w32(hb + 24);
        e_attr = 0x20;
        e_c32 = 0;
        e_dir = 0;
        cpyn(fpart, hb + 38, 13, sizeof(fpart));
        dpart[0] = 0;
        if (hb[4] == 2 && n >= 58) {
            vl = w16(hb + 51);
            nl = vl > 0 ? hb[56] : 0;
            dl = vl > 1 ? hb[57] : 0;
            if (58 + nl + dl <= n) {
                if (nl && hb[58])
                    cpyn(fpart, hb + 58, nl, sizeof(fpart));
                cpyn(dpart, hb + 58 + nl, dl, sizeof(dpart));
            }
        }
        scopy(ename, dpart, sizeof(ename));
        if (ename[0])
            scat(ename, "/", sizeof(ename));
        scat(ename, fpart, sizeof(ename));
        switch (hb[5]) {
        case 0: e_meth = 0; scopy(e_mname, "gesp.", 8); break;
        case 1: e_meth = LZ_LZW; scopy(e_mname, "LZW", 8); break;
        case 2: e_meth = LZ_LH5; scopy(e_mname, "lh5", 8); break;
        default: e_meth = -1; scopy(e_mname, "?", 8);
        }
        return 1;
    }
}

/* ---- LHA ----------------------------------------------------------------- */
static void lha_ext(const unsigned char *x, unsigned es)
{
    unsigned i;
    if (es < 3)
        return;
    switch (x[0]) {
    case 1:
        cpyn(fpart, x + 1, es - 3, sizeof(fpart));
        break;
    case 2:
        cpyn(dpart, x + 1, es - 3, sizeof(dpart));
        for (i = 0; dpart[i]; i++)
            if ((unsigned char)dpart[i] == 0xFF)
                dpart[i] = '/';
        break;
    case 0x40:
        if (es >= 5)
            e_attr = x[1];
        break;
    }
}

static int lha_meth(const unsigned char *m)
{
    static const char *const nm[] = { "lh0", "lh1", "lh4", "lh5", "lh6", "lh7",
                                      "lz4", "lzs", "lz5", "lhd" };
    static const int lz[] = { 0, LZ_LH1, LZ_LH4, LZ_LH5, LZ_LH6, LZ_LH7,
                              0, LZ_LZS, LZ_LZ5, 0 };
    unsigned i;
    for (i = 0; i < 10; i++)
        if (m[0] == '-' && m[1] == nm[i][0] && m[2] == nm[i][1] && m[3] == nm[i][2] &&
            m[4] == '-') {
            e_dir = i == 9;
            return lz[i];
        }
    return -1;
}

static int lha_ok(const unsigned char *p)       /* sieht wie ein Kopf aus */
{
    return p[2] == '-' && p[3] == 'l' && p[6] == '-' && p[20] <= 2;
}

static int lha_next(void)
{
    unsigned hs, nl, es, lvl;
    unsigned long data;
    unsigned char *x;

    if (seek(apos) || rdn(hb, 22) < 22 || hb[0] == 0)
        return 0;                       /* Ende (0-Byte oder Dateiende)  */
    if (!lha_ok(hb))
        return -1;
    lvl = hb[20];
    cpyn(e_mname, hb + 2, 5, sizeof(e_mname));
    e_cs = w32(hb + 7);
    e_us = w32(hb + 11);
    e_attr = 0x20;
    e_c32 = 0;
    e_nocrc = 0;
    e_dir = 0;
    fpart[0] = dpart[0] = 0;
    e_meth = lha_meth(hb + 2);
    if (lvl < 2) {
        hs = hb[0] + 2;
        if (seek(apos) || rd(hb, hs))
            return -1;
        e_time = w16(hb + 15);
        e_date = w16(hb + 17);
        if (lvl == 0)
            e_attr = hb[19];
        nl = hb[21];
        if (22 + nl > hs)
            return -1;
        cpyn(fpart, hb + 22, nl, sizeof(fpart));
        if (22 + nl + 2 <= hs)
            e_crc = w16(hb + 22 + nl);
        else
            e_nocrc = 1;
        data = apos + hs;
        if (lvl == 1) {
            x = hb + 1024;
            for (es = w16(hb + hs - 2); es; es = w16(x + es - 2)) {
                if (es < 3 || es > 2048 || seek(data) || rd(x, es))
                    return -1;
                lha_ext(x, es);
                e_cs -= es;
                data += es;
            }
        }
    } else {
        hs = w16(hb);
        if (hs < 26 || hs > sizeof(hb) || seek(apos) || rd(hb, hs))
            return -1;
        unix2fat(w32(hb + 15), &e_date, &e_time);
        e_crc = w16(hb + 21);
        for (nl = 24; nl + 2 <= hs; nl += es) {
            es = w16(hb + nl);
            if (!es)
                break;
            nl += 2;
            if (nl + es > hs + 2 || es < 3)
                return -1;
            lha_ext(hb + nl, es);
            nl -= 2;
        }
        data = apos + hs;
    }
    for (nl = 0; fpart[nl]; nl++)
        if (fpart[nl] == '\\' || (unsigned char)fpart[nl] == 0xFF)
            fpart[nl] = '/';
    scopy(ename, dpart, sizeof(ename));
    nl = slen(ename);
    if (nl && ename[nl - 1] != '/')
        scat(ename, "/", sizeof(ename));
    scat(ename, fpart, sizeof(ename));
    e_data = data;
    apos = data + e_cs;
    return 1;
}

/* ---- Format erkennen (auch hinter einem Selbstentpacker) ------------------ */
static int detect(void)
{
    unsigned long pos = 0;
    unsigned n, i, bs;

    if (rdn(hb, 42) >= 42 && w32(hb + 20) == 0xFDC4A7DCUL) {
        apos = w32(hb + 24);
        return 'Z';
    }
    for (pos = 0; pos < 0x40000UL; pos += n - 24) {     /* bis 256 KB    */
        if (seek(pos))
            return 0;
        n = rdn(db, sizeof(db));
        if (n < 25)
            return 0;
        for (i = 0; i + 24 <= n; i++) {
            if (db[i] == 0x60 && db[i + 1] == 0xEA && w16(db + i + 2) <= 2600) {
                unsigned long p = pos + i;
                if (arj_hdr(p, &bs) && bs) {
                    apos = p;
                    first = 1;
                    return 'A';
                }
            }
            if (lha_ok(db + i) && (db[i + 20] == 2 || db[i] >= 20)) {
                apos = pos + i;
                return 'L';
            }
        }
        if (n < sizeof(db))
            return 0;
    }
    return 0;
}

static int next_entry(void)
{
    e_nocrc = 0;
    return fmt == 'A' ? arj_next() : fmt == 'Z' ? zoo_next() : lha_next();
}

/* ---- Eintraege -------------------------------------------------------- */
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

static unsigned rd_entry(unsigned char *b, unsigned n)   /* lz_read      */
{
    unsigned r;
    if (!cleft)
        return 0;
    if (n > cleft)
        n = (unsigned)cleft;
    r = rdn(b, n);
    cleft -= r;
    return r;
}

static int wr_out(const unsigned char *p, unsigned n)    /* lz_write     */
{
    if (e_c32)
        crc = crc32a(crc, p, n);
    else
        crc = crc16((unsigned)crc, p, n);
    if (mode != 'x' || hout == 0xFFFF)
        return 0;
    if (DosWrite(hout, p, n) != n) {
        wfail = 1;
        return 1;
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

static void put2(unsigned v, char sep)
{
    char s[4];
    s[0] = (char)('0' + v / 10 % 10);
    s[1] = (char)('0' + v % 10);
    s[2] = sep;
    s[3] = 0;
    puts(s);
}

static void put_date(unsigned d, unsigned t)
{
    put2(d & 31, '.');
    put2((d >> 5) & 15, '.');
    put_num(1980 + (d >> 9), 0, 0);
    puts(" ");
    put2(t >> 11, ':');
    put2((t >> 5) & 63, 0);
}

/* Name bereinigen: '\' -> '/', Laufwerk und fuehrende '/' weg; 0 = ".." */
static int clean_name(void)
{
    char *s, *q, *p = ename;
    unsigned k;

    for (s = ename; *s; s++)
        if (*s == '\\')
            *s = '/';
    if (p[0] && p[1] == ':')
        p += 2;
    while (*p == '/' || (p[0] == '.' && p[1] == '/'))
        p += *p == '/' ? 1 : 2;
    if (p != ename)
        scopy(ename, p, sizeof(ename));
    k = slen(ename);
    while (k && ename[k - 1] == '/') {
        ename[--k] = 0;
        e_dir = 1;
    }
    for (s = ename; *s; s = q) {
        for (q = s; *q && *q != '/'; q++)
            ;
        if (q - s == 2 && s[0] == '.' && s[1] == '.')
            return 0;
        if (*q)
            q++;
    }
    return 1;
}

static void entry(void)
{
    static struct fapi_finfo fi;
    char *p;
    unsigned k;
    int rc;

    if (!clean_name()) {
        say(PROG ": ", ename, ": enth\x84lt \"..\" -- \x81" "bergangen\r\n");
        return;
    }
    if (!ename[0] || !member_ok(ename))
        return;
    nent++;
    if (mode == 'l' || mode == 'v') {
        put_num(e_us, 10, 0);
        if (mode == 'v') {
            put_num(e_cs, 10, 0);
            puts("  ");
            put_ratio(e_us, e_cs);
            puts("  ");
            puts(e_mname);
            for (k = slen(e_mname); k < 6; k++)
                puts(" ");
        }
        puts("  ");
        put_date(e_date, e_time);
        say("  ", ename, e_dir ? "/\r\n" : "\r\n");
        cnt_u += e_us;
        cnt_c += e_cs;
        return;
    }
    if (e_meth < 0) {
        say(PROG ": ", ename, ": Packart ");
        say(e_mname, " nicht unterst\x81tzt -- \x81" "bergangen\r\n", 0);
        errors++;
        return;
    }
    hout = 0xFFFF;
    if (mode == 'x') {
        p = ename;
        if (o_j) {
            if (e_dir)
                return;
            for (k = slen(ename); k && ename[k - 1] != '/'; k--)
                ;
            p = ename + k;
        }
        scopy(opath, ddir, sizeof(opath));
        if (opath[0] && opath[slen(opath) - 1] != '\\')
            scat(opath, "\\", sizeof(opath));
        scat(opath, p, sizeof(opath));
        for (p = opath; *p; p++)
            if (*p == '/')
                *p = '\\';
        abs_path(opath, full);
        if (e_dir) {
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
            say(PROG ": ", full, ": kann nicht angelegt werden\r\n");
            errors++;
            return;
        }
        if (!o_q)
            say(e_meth ? "  entpackt: " : " gespeich.: ", full, "\r\n");
    } else if (e_dir)
        return;
    if (seek(e_data)) {
        errors++;
        return;
    }
    crc = 0;
    cleft = e_cs;
    wfail = 0;
    if (e_meth) {
        lz_read = rd_entry;
        lz_write = wr_out;
        rc = lz_decode(e_meth, e_us);
    } else {
        unsigned n;
        rc = e_cs != e_us ? LZ_EDATA : 0;
        while (cleft && !rc) {
            n = rd_entry(db, sizeof(db));
            if (!n)
                rc = LZ_EEOF;
            else if (wr_out(db, n))
                rc = LZ_EWRITE;
        }
    }
    if (e_nocrc)
        crc = e_crc;
    if (hout != 0xFFFF) {
        DosClose(hout);
        if (!rc && crc == e_crc) {
            fi.mask = 3;
            fi.attr = e_attr & 0x27;
            fi.date = e_date;
            fi.time = e_time;
            DosSetFInfo(full, &fi);
        }
    }
    if (rc || crc != e_crc) {
        say(PROG ": ", ename, rc == LZ_EWRITE || wfail ? ": Schreibfehler (Datentr\x84ger voll?)\r\n" :
                              rc == LZ_EMEM ? ": zu wenig Speicher\r\n" :
                              rc ? ": Daten fehlerhaft\r\n" : ": Pr\x81" "fsumme falsch (CRC)\r\n");
        errors++;
        if (hout != 0xFFFF)
            DosDelete(full);
        return;
    }
    if (mode == 't' && !o_q)
        say("    OK: ", ename, "\r\n");
}

static void usage(void)
{
    puts("Entpackt Archive von ARJ, ZOO und LHA/LHarc/LArc (Format wird erkannt).\r\n\r\n"
         PROG_UP " [Befehl] [Optionen] Archiv[" DEFEXT_UP "] [Ziel\\] [Name...]\r\n\r\n"
         "  x  entpacken (Vorgabe)   e  ohne Verzeichnisse   l/v  zeigen   t  pr\x81" "fen\r\n"
         "  -l  Inhalt zeigen     -v  ausf\x81hrlich zeigen     -t  pr\x81" "fen\r\n"
         "  -d  Zielverzeichnis   -j  ohne Verzeichnisse entpacken\r\n"
         "  -o  ohne Frage \x81" "berschreiben    -n  nie \x81" "berschreiben\r\n"
         "  -q  still\r\n");
}

static int open_arc(const char *ext)
{
    abs_path(arcname, arcfull);
    if (ext)
        scat(arcfull, ext, sizeof(arcfull));
    harc = DosOpen(arcfull);
    return !FAPI_FAILED(harc);
}

void main(void)
{
    char *a, *w;
    unsigned k, need_d = 0, ml = 0, npos = 0;
    int r;

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
        if ((w[0] == '-' || (w[0] == '/' && (w[1] == '?' || !w[2]))) && w[1]) {
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
                    say(PROG ": unbekannte Option ", wbuf, " (-h zeigt die Optionen)\r\n");
                    DosExit(10);
                }
            continue;
        }
        /* alter Befehlsbuchstabe vor dem Archiv (unarj e, lha x ...) */
        if (npos++ == 0 && !arcname[0] && !wbuf[1] && *skip(next_word(a))) {
            k = up(wbuf[0]);
            if (k == 'E' || k == 'X' || k == 'L' || k == 'V' || k == 'T') {
                mode = k == 'E' ? 'x' : k + 'a' - 'A';
                if (k == 'E')
                    o_j = 1;
                continue;
            }
        }
        if (!arcname[0]) {
            scopy(arcname, wbuf, sizeof(arcname));
            continue;
        }
        k = slen(wbuf);
        if (k && (wbuf[k - 1] == '\\' || (k == 2 && wbuf[1] == ':'))) {
            scopy(ddir, wbuf, sizeof(ddir));    /* "ziel\" wie bei UNARJ/LHA */
            continue;
        }
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
    if (!open_arc(0) && !open_arc(DEFEXT) && !open_arc(".arj") && !open_arc(".zoo") &&
        !open_arc(".lzh") && !open_arc(".lha")) {
        say(PROG ": ", arcname, ": nicht gefunden\r\n");
        DosExit(9);
    }
    fmt = detect();
    if (!fmt) {
        say(PROG ": ", arcname, ": kein ARJ-, ZOO- oder LHA-Archiv\r\n");
        DosExit(9);
    }
    if (lz_open()) {
        say(PROG ": zu wenig Speicher\r\n", 0, 0);
        DosExit(8);
    }
    if (mode == 'l')
        puts("     L\x84nge  Datum      Zeit   Name\r\n"
             "  --------  ---------- -----  ----\r\n");
    else if (mode == 'v')
        puts("     L\x84nge   gepackt  Ersparnis Art     Datum      Zeit   Name\r\n");
    else if (mode == 't' || !o_q) {
        say("Archiv: ", arcfull, fmt == 'A' ? " (ARJ)\r\n" : fmt == 'Z' ? " (ZOO)\r\n" : " (LHA)\r\n");
    }
    while ((r = next_entry()) > 0)
        entry();
    if (r < 0) {
        say(PROG ": ", arcname, ": Archiv besch\x84" "digt\r\n");
        errors++;
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
