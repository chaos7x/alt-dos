/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RESTORE.EXE - mit BACKUP gesicherte Dateien wiederherstellen (Format
 * MS-DOS 3.3-5.0, bkfmt.h; liest auch Sicherungen von MS-DOS):
 *
 *   RESTORE Laufwerk1: Laufwerk2:[Pfad[Dateiname]] [/S] [/P] [/B:Datum]
 *           [/A:Datum] [/E:Zeit] [/L:Zeit] [/M] [/N] [/D]
 *
 * Wie bei MS-DOS ist der Pfad ein Filter: Dateien kommen dorthin zurueck,
 * wo sie gesichert wurden. Steht auf dem Datentraeger NAMES.nnn (AT-DOS-
 * BACKUP), bekommen Dateien und Verzeichnisse ihre langen Namen wieder;
 * Pfad und Muster duerfen dann auch lang angegeben werden.
 */
#include "fapi.h"
#include "util.h"
#define CTRY_NO_DPAT
#define CTRY_NO_TIME
#include "country.h"
#define BK_READONLY
#include "bkfmt.h"

#define CHUNK   8192
#define NMAX    16384

static char args[FAPI_ARGLEN], w[FAPI_PATHLEN];
static char fdir[FAPI_PATHLEN], fpat[FAPI_PATHLEN];     /* Filter, relativ */
static char adir[FAPI_PATHLEN];  /* Name ohne Platzhalter: auch als Verzeichnis */
static unsigned char buf[CHUNK], hdr[DH_LEN], db[DB_LEN], fh[FH_LEN];
static char nbuf[NMAX + 1];
static unsigned nlen;

static char srcd, dstd;
static int srem, o_s, o_p, o_b, o_a, o_e, o_l, o_m, o_n, o_d;
static unsigned d_b, d_a, t_e, t_l;
static unsigned disk = 1, found, rc = 0;

/* laufende Datei ueber Diskettengrenzen */
static char cur[FAPI_PATHLEN];
static unsigned curh = 0xFFFF, curseq;
static unsigned short cdate, ctime, cattr;

/* Vergleich ohne Gross/klein, ganze Zeichenkette (util same() endet am
   ersten Leerzeichen - lange Namen haben welche) */
static int eq(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b))
        a++, b++;
    return !*a && !*b;
}

static void out(const char *s) { puts(s); }
/* Rueckfragen auf die Konsole (Standardfehler), auch bei Umleitung */
static void con(const char *s) { DosWrite(2, (void *)s, slen(s)); }
static void conc(char c) { DosWrite(2, &c, 1); }

static void spath(char *o, const char *name)
{
    o[0] = srcd;
    o[1] = ':';
    o[2] = '\\';
    o[3] = 0;
    if (!srem)
        scat(o, "BACKUP\\", FAPI_PATHLEN);
    scat(o, name, FAPI_PATHLEN);
}

static void sname(char *o, const char *base, unsigned n)
{
    char x[16];
    scopy(x, base, sizeof(x));
    scat(x, ".", sizeof(x));
    bk_ext(x + slen(x), n);
    spath(o, x);
}

static int seek_to(unsigned h, unsigned long p)
{
    return DosSeek(h, &p) ? -1 : 0;
}

static void wait_key(void)
{
    con("Eine beliebige Taste dr\x81" "cken, um fortzusetzen . . .");
    if (!KbdCharIn())
        DosExit(3);
    con("\r\n");
}

/* ---- lange Namen (NAMES.nnn) ---------------------------------------- */
static void names_load(void)
{
    static char p[FAPI_PATHLEN];
    unsigned h, r, i = 0, k;
    nlen = 0;
    sname(p, "NAMES", disk);
    h = DosOpen(p);
    if (FAPI_FAILED(h))
        return;
    r = DosRead(h, nbuf, NMAX);
    DosClose(h);
    if (FAPI_FAILED(r))
        return;
    for (k = 0; NAMES_ID[k] && k < r && nbuf[k] == NAMES_ID[k]; k++)
        ;
    if (NAMES_ID[k])
        return;                         /* unbekannte Datei */
    while (i < r && nbuf[i] != '\n')
        i++;
    if (i >= r)
        return;
    i++;
    for (k = 0; i + k < r; k++)
        nbuf[k] = nbuf[i + k];
    nlen = k;
    nbuf[nlen] = 0;
}

/* langen Pfad zum 8.3-Pfad k (relativ), 0 = keiner */
static const char *names_get(const char *k)
{
    unsigned i = 0, n = slen(k), j;
    static char v[FAPI_PATHLEN];
    while (i < nlen) {
        for (j = 0; j < n && up(nbuf[i + j]) == up(k[j]); j++)
            ;
        if (j == n && nbuf[i + j] == '\t') {
            i += j + 1;
            for (j = 0; i + j < nlen && nbuf[i + j] != '\r' && nbuf[i + j] != '\n' &&
                        j < sizeof(v) - 1; j++)
                v[j] = nbuf[i + j];
            v[j] = 0;
            return v;
        }
        while (i < nlen && nbuf[i] != '\n')
            i++;
        i++;
    }
    return 0;
}

/* 8.3-Pfad -> langer Pfad, Stufe fuer Stufe */
static void longpath(char *o, const char *s)
{
    static char k[FAPI_PATHLEN];
    unsigned i = 0;
    const char *v;
    o[0] = 0;
    for (;;) {
        unsigned st = i;
        while (s[i] && s[i] != '\\')
            i++;
        scopy(k, s, i + 1 < sizeof(k) ? i + 1 : sizeof(k));
        v = names_get(k);
        if (v)
            scopy(o, v, FAPI_PATHLEN);
        else {
            if (o[0])
                scat(o, "\\", FAPI_PATHLEN);
            scopy(k, s + st, i - st + 1);
            scat(o, k, FAPI_PATHLEN);
        }
        if (!s[i])
            break;
        i++;
    }
}

/* ---- Datentraeger --------------------------------------------------- */
static int find_control(unsigned *seq)
{
    static char dir[FAPI_PATHLEN];
    static struct fapi_dirent de;
    unsigned n;
    spath(dir, "");
    for (n = 0; DosFindEntry(&de, n, dir) != FAPI_ERROR; n++) {
        char *x = de.name;
        if (!(de.attr & 0x10) && up(x[0]) == 'C' && up(x[1]) == 'O' && up(x[2]) == 'N' &&
            up(x[3]) == 'T' && up(x[4]) == 'R' && up(x[5]) == 'O' && up(x[6]) == 'L' &&
            x[7] == '.' && x[8] >= '0' && x[8] <= '9' && x[9] >= '0' && x[9] <= '9' &&
            x[10] >= '0' && x[10] <= '9' && !x[11]) {
            *seq = (x[8] - '0') * 100 + (x[9] - '0') * 10 + x[10] - '0';
            if (disk == 1) {
                static char d[16];
                ctry_date(d, de.date);
                out("\r\n*** Dateien gesichert am ");
                out(d);
                out(" ***\r\n");
            }
            return 0;
        }
    }
    return -1;
}

static void ask_disk(void)
{
    char t[4];
    unsigned seq;
    for (;;) {
        if (srem) {
            bk_ext(t, disk);
            con("\r\nSicherungsdiskette ");
            con(t + 1);
            con(" in Laufwerk ");
            conc(srcd);
            con(": einlegen\r\n");
            wait_key();
        }
        if (find_control(&seq)) {
            con("Quelle enth\x84lt keine Sicherungsdateien\r\n");
            if (!srem)
                DosExit(1);
            continue;
        }
        if (seq == disk)
            return;
        con("WARNUNG! Disketten in falscher Reihenfolge\r\n");
        if (!srem)
            DosExit(4);
    }
}

/* ---- Wiederherstellen ----------------------------------------------- */
static int date_ok(unsigned d, unsigned t)
{
    if (o_b && d > d_b) return 0;
    if (o_a && d < d_a) return 0;
    if (o_e && t > t_e) return 0;
    if (o_l && t < t_l) return 0;
    return 1;
}

static int pathcmp(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b))
        a++, b++;
    return up(*a) - up(*b);
}

/* passt das Verzeichnis (8.3 oder lang) zum Filter? */
static int dir_ok(const char *d8, const char *dl, const char *fdir)
{
    unsigned n = slen(fdir);
    if (!pathcmp(d8, fdir) || !pathcmp(dl, fdir))
        return 1;
    if (!o_s)
        return 0;
    if (!n)
        return 1;
    {
        static char t[FAPI_PATHLEN];
        scopy(t, d8, n + 1);
        if (!pathcmp(t, fdir) && d8[n] == '\\')
            return 1;
        scopy(t, dl, n + 1);
        if (!pathcmp(t, fdir) && dl[n] == '\\')
            return 1;
    }
    return 0;
}

static int sysname(const char *d8, const char *n)
{
    static const char *sys[] = { "IO.SYS", "MSDOS.SYS", "IBMBIO.COM", "IBMDOS.COM",
                                 "COMMAND.COM", "CMD.EXE", "ATDOS.SYS", 0 };
    unsigned i;
    if (d8[0])
        return 0;
    for (i = 0; sys[i]; i++)
        if (eq(n, sys[i]))
            return 1;
    return 0;
}

static void mkdirs(const char *full)
{
    static char p[FAPI_PATHLEN];
    unsigned i;
    for (i = 3; full[i]; i++)
        if (full[i] == '\\') {
            scopy(p, full, i + 1);
            DosMkDir(p);
        }
}

/* Zieldatei pruefen; 1 = wiederherstellen */
static int check_target(const char *full, unsigned fd, unsigned ft)
{
    static struct fapi_dirent de;
    static char dir[FAPI_PATHLEN];
    unsigned n;
    int exist = 0;
    scopy(dir, full, sizeof(dir));
    *base_name(dir) = 0;
    for (n = 0; DosFindEntry(&de, n, dir) != FAPI_ERROR; n++)
        if (eq(de.name, base_name((char *)full))) {
            exist = 1;
            break;
        }
    if (!exist)
        return 1;
    if (o_n)
        return 0;
    if (o_m && !(de.attr & 0x20))
        return 0;
    if (o_p && ((de.attr & 1) || de.date > fd || (de.date == fd && de.time > ft))) {
        con("WARNUNG! Datei ");
        con(full);
        con(de.attr & 1 ? "\r\nist schreibgesch\x81tzt\r\n"
                        : "\r\nwurde nach der Sicherung ge\x84ndert\r\n");
        con("Datei ersetzen (J/N)? ");
        if (!ask_yn())
            return 0;
    }
    if (de.attr & 1) {
        static struct fapi_finfo fi;
        fi.mask = 1;
        fi.attr = de.attr & 0x26;
        DosSetFInfo((char *)full, &fi);
    }
    return 1;
}

static void close_cur(int ok)
{
    static struct fapi_finfo fi;
    if (curh == 0xFFFF)
        return;
    DosClose(curh);
    curh = 0xFFFF;
    if (ok) {
        fi.mask = 3;
        fi.attr = cattr & 0x27;
        fi.date = cdate;
        fi.time = ctime;
        DosSetFInfo(cur, &fi);
    } else
        out("*** Datei kann nicht wiederhergestellt werden ***\r\n");
}

static int copy_part(unsigned hb, unsigned long begin, unsigned long len)
{
    unsigned n, k;
    if (seek_to(hb, begin))
        return -1;
    while (len) {
        n = len > CHUNK ? CHUNK : (unsigned)len;
        k = DosRead(hb, buf, n);
        if (FAPI_FAILED(k) || k != n)
            return -1;
        k = DosWrite(curh, buf, n);
        if (FAPI_FAILED(k) || k != n)
            return -1;
        len -= n;
    }
    return 0;
}

static void one_file(unsigned hb, const char *d8, const char *dl)
{
    static char n83[13], r8[FAPI_PATHLEN], rl[FAPI_PATHLEN], full[FAPI_PATHLEN];
    unsigned i, seq = bk_get16(fh + FH_FSEQ), date = bk_get16(fh + FH_DATE),
             time = bk_get16(fh + FH_TIME);
    unsigned char fl = fh[FH_FLAGS];
    const char *base;

    for (i = 0; i < 12 && fh[FH_NAME + i]; i++)
        n83[i] = (char)fh[FH_NAME + i];
    n83[i] = 0;
    scopy(r8, d8, sizeof(r8));
    if (r8[0])
        scat(r8, "\\", sizeof(r8));
    scat(r8, n83, sizeof(r8));
    longpath(rl, r8);
    full[0] = dstd;
    full[1] = ':';
    full[2] = '\\';
    scopy(full + 3, rl, sizeof(full) - 3);

    if (seq > 1) {                      /* Fortsetzung einer Datei       */
        if (curh == 0xFFFF || seq != curseq + 1 || !eq(full, cur))
            return;
        if (copy_part(hb, bk_get32(fh + FH_BEGIN), bk_get32(fh + FH_PART))) {
            close_cur(0);
            rc = 4;
            return;
        }
        curseq = seq;
        if (fl & FHF_LAST)
            close_cur(1);
        return;
    }
    close_cur(0);                       /* abgebrochene Fortsetzung      */
    base = base_name(rl);
    if (!((match(n83, fpat) || match(base, fpat)) && dir_ok(d8, dl, fdir)) &&
        !(adir[0] && dir_ok(d8, dl, adir)))
        return;
    if (!date_ok(date, time) || sysname(d8, n83))
        return;
    found++;
    if (!o_d && !check_target(full, date, time))
        return;                         /* /N, /M, /P: uebergangen       */
    out("\\");
    out(rl);
    out("\r\n");
    if (o_d)
        return;
    if (!(fl & FHF_OK)) {
        out("*** Datei kann nicht wiederhergestellt werden ***\r\n");
        rc = 4;
        return;
    }
    mkdirs(full);
    curh = DosCreate(full);
    if (FAPI_FAILED(curh)) {
        curh = 0xFFFF;
        out("Fehler beim Erstellen der Datei\r\n");
        rc = 4;
        return;
    }
    scopy(cur, full, sizeof(cur));
    curseq = 1;
    cdate = date;
    ctime = time;
    cattr = bk_get16(fh + FH_ATTR);
    if (copy_part(hb, bk_get32(fh + FH_BEGIN), bk_get32(fh + FH_PART))) {
        close_cur(0);
        rc = 4;
        return;
    }
    if (fl & FHF_LAST)
        close_cur(1);
}

/* einen Datentraeger abarbeiten; 1 = war der letzte */
static int do_disk(void)
{
    static char p[FAPI_PATHLEN], d8[DB_PATHMAX + 1], dl[FAPI_PATHLEN];
    unsigned hc, hb, i, n;
    unsigned long pos, nx, fp;
    int last;

    sname(p, "CONTROL", disk);
    hc = DosOpen(p);
    sname(p, "BACKUP", disk);
    hb = DosOpen(p);
    if (FAPI_FAILED(hc) || FAPI_FAILED(hb) || DosRead(hc, hdr, DH_LEN) != DH_LEN ||
        hdr[0] != DH_LEN) {
        out("Quelle enth\x84lt keine Sicherungsdateien\r\n");
        DosExit(4);
    }
    last = hdr[DH_LAST] == 0xFF;
    names_load();
    if (srem) {
        char t[4];
        bk_ext(t, disk);
        out("Diskette: ");
        out(t + 1);
        out("\r\n");
    }
    pos = DH_LEN;
    for (;;) {
        if (seek_to(hc, pos) || DosRead(hc, db, DB_LEN) != DB_LEN || db[0] != DB_LEN)
            break;
        for (i = 0; i < DB_PATHMAX && db[DB_PATH + i]; i++)
            d8[i] = (char)db[DB_PATH + i];
        d8[i] = 0;
        longpath(dl, d8);
        n = bk_get16(db + DB_NUM);
        fp = pos + DB_LEN;
        for (i = 0; i < n; i++, fp += FH_LEN) {
            if (seek_to(hc, fp) ||
                DosRead(hc, fh, FH_LEN) != FH_LEN || fh[0] != FH_LEN)
                break;
            one_file(hb, d8, dl);
        }
        nx = bk_get32(db + DB_NEXT);
        if (nx == 0xFFFFFFFFUL || nx <= pos)
            break;
        pos = nx;
    }
    DosClose(hc);
    DosClose(hb);
    return last;
}

/* ---- Eingabe -------------------------------------------------------- */
static int pnum3(char *p, unsigned *v)
{
    unsigned i;
    for (i = 0; i < 3; i++) {
        v[i] = 0;
        if (!(*p >= '0' && *p <= '9'))
            return -1;
        while (*p >= '0' && *p <= '9')
            v[i] = v[i] * 10 + (*p++ - '0');
        if (i < 2) {
            if (*p != '.' && *p != '-' && *p != '/' && *p != ':') {
                if (i == 1) {
                    v[2] = 0;
                    return 2;
                }
                return -1;
            }
            p++;
        }
    }
    return 3;
}

static int parse_date(char *p, unsigned *o)
{
    unsigned v[3], d, m, y;
    if (pnum3(p, v) != 3)
        return -1;
    if (ctry.dfmt == 0)
        m = v[0], d = v[1], y = v[2];
    else if (ctry.dfmt == 2)
        y = v[0], m = v[1], d = v[2];
    else
        d = v[0], m = v[1], y = v[2];
    if (y < 80)
        y += 2000;
    else if (y < 100)
        y += 1900;
    if (y < 1980 || y > 2107 || m < 1 || m > 12 || d < 1 || d > 31)
        return -1;
    *o = ((y - 1980) << 9) | (m << 5) | d;
    return 0;
}

static int parse_time(char *p, unsigned *o)
{
    unsigned v[3];
    if (pnum3(p, v) < 2 || v[0] > 23 || v[1] > 59 || v[2] > 59)
        return -1;
    *o = (v[0] << 11) | (v[1] << 5) | (v[2] >> 1);
    return 0;
}

static void bad(const char *m)
{
    out(m);
    out("\r\n");
    DosExit(4);
}

static void usage(void)
{
    out("Stellt mit BACKUP gesicherte Dateien wieder her.\r\n\r\n"
        "RESTORE Laufwerk1: Laufwerk2:[Pfad[Dateiname]] [/S] [/P] [/B:Datum] [/A:Datum]\r\n"
        "  [/E:Zeitpunkt] [/L:Zeitpunkt] [/M] [/N] [/D]\r\n\r\n"
        "  Laufwerk1:  Laufwerk, auf dem sich die Sicherungsdateien befinden.\r\n"
        "  Laufwerk2:[Pfad[Dateiname]]\r\n"
        "              Wiederherzustellende Datei(en).\r\n"
        "  /S   Stellt Dateien in allen Unterverzeichnissen des Pfades wieder her.\r\n"
        "  /P   Fragt nach vor der Ersetzung schreibgesch\x81tzter oder seit der\r\n"
        "       Sicherung ge\x84nderter Dateien.\r\n"
        "  /B   Stellt nur an oder vor dem Datum ge\x84nderte Dateien wieder her.\r\n"
        "  /A   Stellt nur an oder nach dem Datum ge\x84nderte Dateien wieder her.\r\n"
        "  /E   Stellt nur an oder vor dem Zeitpunkt ge\x84nderte Dateien wieder her.\r\n"
        "  /L   Stellt nur an oder nach dem Zeitpunkt ge\x84nderte Dateien wieder her.\r\n"
        "  /M   Stellt nur seit der letzten Sicherung ge\x84nderte Dateien wieder her.\r\n"
        "  /N   Stellt nur im Zielpfad nicht mehr existierende Dateien wieder her.\r\n"
        "  /D   Listet die den Angaben entsprechenden gesicherten Dateien auf.\r\n");
}

void main(void)
{
    static struct fapi_fsinfo fi;
    static char dst[FAPI_PATHLEN];
    char *a;
    int ns = 0;
    unsigned i;

    ctry_init();
    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        usage();
        return;
    }
    for (; *a; a = skip(next_word(a))) {
        word(a, w, sizeof(w));
        if (*a == '/') {
            char c = up(w[1]);
            if (c == 'S' && !w[2]) o_s = 1;
            else if (c == 'P' && !w[2]) o_p = 1;
            else if (c == 'M' && !w[2]) o_m = 1;
            else if (c == 'N' && !w[2]) o_n = 1;
            else if (c == 'D' && !w[2]) o_d = 1;
            else if (c == 'B' && w[2] == ':') { o_b = 1; if (parse_date(w + 3, &d_b)) bad("Unzul\x84ssige Datumsangabe"); }
            else if (c == 'A' && w[2] == ':') { o_a = 1; if (parse_date(w + 3, &d_a)) bad("Unzul\x84ssige Datumsangabe"); }
            else if (c == 'E' && w[2] == ':') { o_e = 1; if (parse_time(w + 3, &t_e)) bad("Unzul\x84ssige Zeitangabe"); }
            else if (c == 'L' && w[2] == ':') { o_l = 1; if (parse_time(w + 3, &t_l)) bad("Unzul\x84ssige Zeitangabe"); }
            else {
                out("Ung\x81ltiger Schalter - ");
                bad(w);
            }
            continue;
        }
        if (ns == 0) {
            if (!w[0] || w[1] != ':' || w[2])
                bad("Ung\x81ltige Laufwerksangabe");
            srcd = up(w[0]);
        } else if (ns == 1)
            scopy(dst, w, sizeof(dst));
        else
            bad("Unzul\x84ssige Anzahl von Parametern");
        ns++;
    }
    if (ns < 1)
        bad("Kein Quellaufwerk angegeben");
    if (ns < 2)
        bad("Kein Ziellaufwerk angegeben");
    if (abs_path(dst, w))
        bad("Ung\x81ltiger Pfad");
    dstd = up(w[0]);
    if (dstd == srcd)
        bad("Quell- und Ziellaufwerk sind identisch");
    /* Filter: Verzeichnis (relativ, ohne \ am Ende) und Muster */
    i = slen(w);
    if (w[i - 1] == '\\' || is_dir(w)) {
        if (w[i - 1] == '\\' && i > 3)
            w[i - 1] = 0;
        scopy(fpat, "*.*", sizeof(fpat));
        scopy(fdir, w + 3, sizeof(fdir));
    } else {
        char *b = base_name(w);
        scopy(fpat, b, sizeof(fpat));
        if (b > w + 3)
            b[-1] = 0;
        else
            *b = 0;
        scopy(fdir, w + 3, sizeof(fdir));
        if (!has_wild(fpat)) {          /* RESTORE A: C:\SPIELE (weg)  */
            scopy(adir, fdir, sizeof(adir));
            if (adir[0])
                scat(adir, "\\", sizeof(adir));
            scat(adir, fpat, sizeof(adir));
        }
    }
    if (DosQFSInfo(srcd - 'A', &fi))
        fi.flags = FSI_REMOVABLE;
    srem = (fi.flags & (FSI_REMOVABLE | FSI_REMDISK)) != 0;

    for (;;) {
        ask_disk();
        if (disk == 1) {
            out("\r\n*** Dateien werden von Laufwerk ");
            putc(srcd);
            out(": wiederhergestellt ***\r\n");
        }
        if (do_disk())
            break;
        if (!srem)
            break;
        disk++;
    }
    close_cur(curh == 0xFFFF);
    if (!found) {
        out("WARNUNG! Keine Dateien zum Wiederherstellen gefunden\r\n");
        DosExit(1);
    }
    DosExit(rc);
}
