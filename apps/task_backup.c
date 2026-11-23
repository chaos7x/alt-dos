/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BACKUP.EXE - Dateien sichern, Format wie MS-DOS 3.3-5.0 (bkfmt.h):
 *
 *   BACKUP Quelle Ziellaufwerk: [/S] [/M] [/A] [/F[:Groesse]]
 *          [/D:Datum [/T:Zeit]] [/L[:[Laufwerk:][Pfad]Protokoll]]
 *
 * RESTORE von MS-DOS liest die Sicherungen (und unser RESTORE die von
 * MS-DOS). Lange Namen (EA, VFAT, FAT32, CD) kennt CONTROL.nnn nicht: dort
 * stehen 8.3-Namen, die langen in NAMES.nnn auf demselben Datentraeger
 * (Zeilen "8.3-Pfad<Tab>langer Pfad", relativ zum Stamm). MS-DOS uebergeht
 * die Datei. EA DATA. SF wird nicht gesichert (verweist auf Cluster).
 */
#include "fapi.h"
#include "util.h"
#define CTRY_NO_DPAT
#include "country.h"
#include "bkfmt.h"

#define CHUNK   8192
#define NMAX    16384           /* lange Namen je Datentraeger      */
#define MARGIN  2048            /* Rest fuer CONTROL.nnn je Diskette */
#define MAXLVL  32

static char args[FAPI_ARGLEN], line[FAPI_PATHLEN + 80];
static char src[FAPI_PATHLEN], pat[FAPI_PATHLEN], logp[FAPI_PATHLEN];
static char s8[DB_PATHMAX + 14 + 1];        /* 8.3-Pfad (relativ) */
static char ln[FAPI_PATHLEN];               /* langer Pfad, absolut "X:\..." */
static char dbpath[DB_PATHMAX + 1];
static unsigned char buf[CHUNK], hdr[DH_LEN];
static char nbuf[NMAX + 1];
static unsigned nlen;

static char tgt, sdrv;                 /* Laufwerksbuchstaben            */
static int  trem;                      /* Ziel ist Wechselmedium         */
static int  o_s, o_m, o_a, o_f, o_d, o_t, o_l;
static unsigned u_date, u_time;
static char fsize[16];

static unsigned disk;                  /* Nummer des Datentraegers       */
static unsigned hb, hc, hl;            /* BACKUP.nnn, CONTROL.nnn, Log   */
static unsigned long blen, clen, dbpos, fhpos;
static unsigned dbnum, nfiles, gen;
static int open_t, a_pending, have_db, tfull, rc = 0;
static unsigned long a_lastdb;

static struct fapi_dirent_s e;
static unsigned lvl_idx[MAXLVL];
static unsigned char lvl_s8[MAXLVL];
static unsigned lvl_ln[MAXLVL];

/* ---- kleine Hilfen ------------------------------------------------- */
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

static void tpath(char *o, const char *name)
{
    o[0] = tgt;
    o[1] = ':';
    o[2] = '\\';
    o[3] = 0;
    if (!trem)
        scat(o, "BACKUP\\", FAPI_PATHLEN);
    scat(o, name, FAPI_PATHLEN);
}

static void tname(char *o, const char *base, unsigned n)
{
    char x[16];
    scopy(x, base, sizeof(x));
    scat(x, ".", sizeof(x));
    bk_ext(x + slen(x), n);
    tpath(o, x);
}

static int seek_to(unsigned h, unsigned long p)
{
    return DosSeek(h, &p) ? -1 : 0;
}

static void wr(unsigned h, const void *p, unsigned n, unsigned long *len)
{
    unsigned k = DosWrite(h, (void *)p, n);
    if (FAPI_FAILED(k) || k != n) {
        out("Fehler beim Schreiben der Sicherungsdateien.\r\n");
        DosExit(4);
    }
    if (len)
        *len += n;
}

/* an Stelle p in CONTROL.nnn n Bytes schreiben, dann wieder ans Ende */
static void cpoke(unsigned long p, const void *d, unsigned n)
{
    seek_to(hc, p);
    wr(hc, d, n, 0);
    seek_to(hc, clen);
}

static void cpeek(unsigned long p, void *d, unsigned n)
{
    seek_to(hc, p);
    DosRead(hc, d, n);
    seek_to(hc, clen);
}

static unsigned long freebytes(void)
{
    unsigned long f[2];
    f[0] = 0;
    if (DosFreeSpace(tgt - 'A', f))
        return 0;
    return f[0];
}

static void wait_key(void)
{
    con("Eine beliebige Taste dr\x81" "cken, um fortzusetzen . . .");
    if (!KbdCharIn())
        DosExit(3);
    con("\r\n");
}

/* ---- lange Namen ---------------------------------------------------- */
static int names_has(const char *k)
{
    unsigned i = 0, n = slen(k), j;
    while (i < nlen) {
        for (j = 0; j < n && nbuf[i + j] == k[j]; j++)
            ;
        if (j == n && nbuf[i + j] == '\t')
            return 1;
        while (i < nlen && nbuf[i] != '\n')
            i++;
        i++;
    }
    return 0;
}

static void names_add(const char *k, const char *v)
{
    unsigned a = slen(k), b = slen(v);
    if (names_has(k))
        return;
    if (nlen + a + b + 3 > NMAX) {
        if (nlen <= NMAX) {
            out("WARNUNG! Zu viele lange Namen auf diesem Datentr\x84ger - weitere nur als 8.3.\r\n");
            nlen = NMAX + 1;
        }
        return;
    }
    scopy(nbuf + nlen, k, NMAX + 1 - nlen);
    nlen += a;
    nbuf[nlen++] = '\t';
    scopy(nbuf + nlen, v, NMAX + 1 - nlen);
    nlen += b;
    nbuf[nlen++] = '\r';
    nbuf[nlen++] = '\n';
}

/* langen Namen fuer s8 (relativ) und ln (absolut) merken, wenn er anders
   ist; Verzeichnisse dabei mit (jede Stufe) */
static void note_names(const char *rs, const char *rl)
{
    static char k[DB_PATHMAX + 14 + 1], v[FAPI_PATHLEN];
    unsigned i = 0, j = 0;
    for (;;) {
        while (rs[i] && rs[i] != '\\')
            i++;
        while (rl[j] && rl[j] != '\\')
            j++;
        scopy(k, rs, i + 1 < sizeof(k) ? i + 1 : sizeof(k));
        scopy(v, rl, j + 1 < sizeof(v) ? j + 1 : sizeof(v));
        {
            unsigned x;
            for (x = 0; k[x] && k[x] == v[x]; x++)
                ;
            if (k[x] || v[x])       /* nicht gleich geschrieben */
                names_add(k, v);
        }
        if (!rs[i] || !rl[j])
            break;
        i++, j++;
    }
}

static void names_write(void)
{
    static char p[FAPI_PATHLEN], h[32];
    unsigned f, cp;
    if (!nlen)
        return;
    if (nlen > NMAX)
        nlen = NMAX;
    tname(p, "NAMES", disk);
    f = DosCreate(p);
    if (FAPI_FAILED(f)) {
        out("WARNUNG! NAMES-Datei kann nicht angelegt werden.\r\n");
        nlen = 0;
        return;
    }
    scopy(h, NAMES_ID " CP=", sizeof(h));
    cp = DosGetCp();
    if (FAPI_FAILED(cp))
        cp = 437;
    bk_ext(h + slen(h), cp % 1000);
    scat(h, "\r\n", sizeof(h));
    DosWrite(f, h, slen(h));
    while (nlen) {
        unsigned n = nlen > 0x7000 ? 0x7000 : nlen;
        DosWrite(f, nbuf, n);
        nlen -= n;
    }
    DosClose(f);
}

/* ---- Datentraeger --------------------------------------------------- */
static void setro(const char *base)
{
    static char p[FAPI_PATHLEN];
    static struct fapi_finfo fi;
    tname(p, base, disk);
    fi.mask = 1;
    fi.attr = 0x21;
    DosSetFInfo(p, &fi);
}

static void label(void)
{
    static char l[16];
    static struct fapi_label lb;
    scopy(l, "BACKUP  ", sizeof(l));
    bk_ext(l + 8, disk);
    DosLabel(tgt - 'A', &lb, l);
}

static void new_db(void);

static void put_header(void)
{
    unsigned i;
    for (i = 0; i < DH_LEN; i++)
        hdr[i] = 0;
    hdr[0] = DH_LEN;
    for (i = 0; i < 8; i++)
        hdr[DH_ID + i] = (unsigned char)"BACKUP  "[i];
    hdr[DH_SEQ] = (unsigned char)disk;
    hdr[DH_LAST] = 0;
    wr(hc, hdr, DH_LEN, &clen);
}

static int create_files(void)
{
    static char p[FAPI_PATHLEN];
    tname(p, "BACKUP", disk);
    hb = DosCreate(p);
    if (FAPI_FAILED(hb))
        return -1;
    tname(p, "CONTROL", disk);
    hc = DosCreate(p);
    if (FAPI_FAILED(hc)) {
        DosClose(hb);
        return -1;
    }
    blen = clen = 0;
    open_t = 1;
    put_header();
    have_db = 0;                        /* naechste Datei: neuer Block  */
    tfull = 0;
    return 0;
}

/* Dateien im Stamm (Diskette) bzw. in \BACKUP loeschen */
static void delete_all(void)
{
    static char dir[FAPI_PATHLEN], p[FAPI_PATHLEN];
    static struct fapi_dirent de;
    static struct fapi_finfo fi;
    unsigned n = 0;
    tpath(dir, "");
    while (DosFindEntry(&de, n, dir) != FAPI_ERROR) {
        if (de.attr & 0x18) {           /* Verzeichnis, Bezeichnung     */
            n++;
            continue;
        }
        if (o_l && logp[0] == tgt) {    /* Protokoll auf dem Ziel bleibt */
            join(p, dir, de.name, sizeof(p));
            if (eq(p, logp)) {
                n++;
                continue;
            }
        }
        join(p, dir, de.name, sizeof(p));
        fi.mask = 1;
        fi.attr = 0;
        DosSetFInfo(p, &fi);
        if (DosDelete(p))
            n++;
    }
}

static void say_disk(void)
{
    char t[4];
    out("Diskette Nummer: ");
    bk_ext(t, disk);
    out(t + 1);
    out("\r\n");
}

static int is_raw(void)
{
    static struct fapi_fsinfo fi;
    if (DosQFSInfo(tgt - 'A', &fi))
        return 1;
    return fi.fsname[0] == 'R' && fi.fsname[1] == 'A' && fi.fsname[2] == 'W';
}

static void format_disk(void)
{
    static char cmd[FAPI_PATHLEN + 40];
    cmd[0] = 0;
    if (FAPI_FAILED(DosGetEnv("COMSPEC", cmd, FAPI_PATHLEN)) || !cmd[0])
        scopy(cmd, "CMD.EXE", sizeof(cmd));
    scat(cmd, " /C FORMAT ", sizeof(cmd));
    line[0] = tgt, line[1] = ':', line[2] = 0;
    scat(cmd, line, sizeof(cmd));
    if (o_f && fsize[0]) {
        scat(cmd, " /F:", sizeof(cmd));
        scat(cmd, fsize, sizeof(cmd));
    }
    scat(cmd, " /V:BACKUP", sizeof(cmd));
    DosExecPgm(cmd, EXEC_SYNC);
}

static void ask_disk(int last)
{
    char t[4];
    for (;;) {
        bk_ext(t, disk);
        if (last)
            con("Letzte Sicherungsdiskette");
        else {
            con("Sicherungsdiskette ");
            con(t + 1);
        }
        con(" in Laufwerk ");
        conc(tgt);
        con(": einlegen\r\n");
        if (!last) {
            con("WARNUNG! Dateien im Stammverzeichnis ");
            conc(tgt);
            con(":\\ des Ziellaufwerks werden gel\x94scht\r\n");
        }
        wait_key();
        if (!last && is_raw())
            format_disk();
        if (!is_raw())
            return;
        con("Diskette nicht bereit oder nicht formatiert.\r\n");
    }
}

static void finish_disk(int last)
{
    unsigned char b = (unsigned char)(last ? 0xFF : 0);
    if (!open_t)
        return;
    cpoke(DH_LAST, &b, 1);
    DosClose(hb);
    DosClose(hc);
    open_t = 0;
    names_write();
    setro("BACKUP");
    setro("CONTROL");
    setro("NAMES");
    if (trem)
        label();
}

static void next_disk(void)
{
    finish_disk(0);
    if (!trem) {
        out("Datensicherungsger\x84t ");
        putc(tgt);
        out(": ist voll\r\n");
        DosExit(4);
    }
    disk++;
    out("\r\n");
    ask_disk(0);
    delete_all();
    if (create_files()) {
        out("Zieldatentr\x84ger kann nicht zum Sichern verwendet werden\r\n");
        DosExit(4);
    }
    say_disk();
}

/* ---- /A: an die letzte Sicherung anhaengen -------------------------- */
static int open_last(void)
{
    static char dir[FAPI_PATHLEN], p[FAPI_PATHLEN];
    static struct fapi_dirent de;
    static struct fapi_finfo fi;
    unsigned n, best = 0, k;
    unsigned long pos, nx;
    unsigned char b4[4];

    tpath(dir, "");
    for (n = 0; DosFindEntry(&de, n, dir) != FAPI_ERROR; n++) {
        char *x = de.name;
        if (up(x[0]) == 'C' && up(x[1]) == 'O' && up(x[2]) == 'N' && up(x[3]) == 'T' &&
            up(x[4]) == 'R' && up(x[5]) == 'O' && up(x[6]) == 'L' && x[7] == '.' &&
            x[8] >= '0' && x[8] <= '9' && x[9] >= '0' && x[9] <= '9' &&
            x[10] >= '0' && x[10] <= '9' && !x[11]) {
            k = (x[8] - '0') * 100 + (x[9] - '0') * 10 + x[10] - '0';
            if (k > best)
                best = k;
        }
    }
    if (!best)
        return -1;
    disk = best;
    fi.mask = 1;
    fi.attr = 0x20;
    tname(p, "BACKUP", disk);
    DosSetFInfo(p, &fi);
    hb = DosOpenRW(p);
    tname(p, "CONTROL", disk);
    DosSetFInfo(p, &fi);
    hc = DosOpenRW(p);
    if (FAPI_FAILED(hb) || FAPI_FAILED(hc))
        return -1;
    /* Laengen ueber die Suche (DosSeek meldet keine Stelle) */
    blen = clen = 0;
    for (n = 0; DosFindEntry(&de, n, dir) != FAPI_ERROR; n++) {
        tname(p, "BACKUP", disk);
        if (eq(base_name(p), de.name))
            blen = de.size;
        tname(p, "CONTROL", disk);
        if (eq(base_name(p), de.name))
            clen = de.size;
    }
    if (DosRead(hc, hdr, DH_LEN) != DH_LEN || hdr[DH_LAST] != 0xFF) {
        out("Letzte Sicherungsdiskette nicht eingelegt\r\n");
        DosExit(4);
    }
    pos = DH_LEN;                       /* letzten Verzeichnisblock suchen */
    for (k = 0; k < 60000u; k++) {
        cpeek(pos + DB_NEXT, b4, 4);
        nx = bk_get32(b4);
        if (nx == 0xFFFFFFFFUL || nx <= pos || nx >= clen)
            break;
        pos = nx;
    }
    a_lastdb = pos;
    a_pending = 1;                      /* beim ersten Dateikopf umtragen */
    seek_to(hb, blen);
    seek_to(hc, clen);
    open_t = 1;
    have_db = 0;
    /* vorhandene lange Namen behalten */
    tname(p, "NAMES", disk);
    n = DosOpen(p);
    nlen = 0;
    if (!FAPI_FAILED(n)) {
        unsigned i = 0, r;
        r = DosRead(n, nbuf, NMAX);
        if (!FAPI_FAILED(r)) {
            while (i < r && nbuf[i] != '\n')
                i++;
            if (i < r) {
                i++;
                for (k = 0; i + k < r; k++)
                    nbuf[k] = nbuf[i + k];
                nlen = k;
            }
        }
        DosClose(n);
        DosSetFInfo(p, &fi);
    }
    return 0;
}

/* ---- Verzeichnisblock und Dateikopf --------------------------------- */
static void new_db(void)
{
    static unsigned char db[DB_LEN];
    unsigned i;
    unsigned char b4[4];
    if (a_pending) {                    /* /A: alter letzter Block -> hier */
        unsigned char z = 0;
        bk_put32(b4, clen);
        cpoke(a_lastdb + DB_NEXT, b4, 4);
        cpoke(DH_LAST, &z, 1);
        a_pending = 0;
    } else if (have_db) {               /* voriger Block zeigt auf diesen */
        bk_put32(b4, clen);
        cpoke(dbpos + DB_NEXT, b4, 4);
    }
    have_db = 1;
    for (i = 0; i < DB_LEN; i++)
        db[i] = 0;
    db[0] = DB_LEN;
    for (i = 0; s8[i] && i < DB_PATHMAX; i++)
        db[DB_PATH + i] = (unsigned char)s8[i];
    bk_put32(db + DB_NEXT, 0xFFFFFFFFUL);
    dbpos = clen;
    dbnum = 0;
    wr(hc, db, DB_LEN, &clen);
    scopy(dbpath, s8, sizeof(dbpath));
}

static void put_fh(const char *n83, unsigned seq, unsigned long rest)
{
    static unsigned char fh[FH_LEN];
    unsigned i;
    unsigned char b2[2];
    for (i = 0; i < FH_LEN; i++)
        fh[i] = 0;
    fh[0] = FH_LEN;
    for (i = 0; n83[i] && i < 12; i++)
        fh[FH_NAME + i] = (unsigned char)n83[i];
    fh[FH_FLAGS] = FHF_LAST | FHF_OK;
    bk_put32(fh + FH_FLEN, e.size);
    bk_put16(fh + FH_FSEQ, seq);
    bk_put32(fh + FH_BEGIN, blen);
    bk_put32(fh + FH_PART, rest);
    bk_put16(fh + FH_ATTR, e.attr);
    bk_put16(fh + FH_TIME, e.time);
    bk_put16(fh + FH_DATE, e.date);
    fhpos = clen;
    wr(hc, fh, FH_LEN, &clen);
    dbnum++;
    bk_put16(b2, dbnum);
    cpoke(dbpos + DB_NUM, b2, 2);
}

static void fh_close(unsigned char flags, unsigned long part)
{
    unsigned char b4[4];
    cpoke(fhpos + FH_FLAGS, &flags, 1);
    bk_put32(b4, part);
    cpoke(fhpos + FH_PART, b4, 4);
}

/* ---- eine Datei ----------------------------------------------------- */
static void log_line(const char *p)
{
    char t[4];
    if (!o_l || FAPI_FAILED(hl))
        return;
    bk_ext(t, disk);
    DosWrite(hl, "\r\n", 2);
    DosWrite(hl, t, 3);
    DosWrite(hl, "  \\", 3);
    DosWrite(hl, (void *)p, slen(p));
}

static void make83(char *o, const char *n)
{
    /* fuer Namen ohne 8.3-Gegenstueck (CD-Module): NAME~k.EXT */
    unsigned i, k = 0;
    const char *ext = 0, *x;
    char num[6];
    for (x = n; *x; x++)
        if (*x == '.')
            ext = x;
    for (i = 0; n + i != ext && n[i] && k < 6; i++) {
        char c = up(n[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            o[k++] = c;
    }
    if (!k)
        o[k++] = '_';
    gen++;
    i = gen;
    num[0] = '~';
    num[1] = (char)('0' + i % 10);
    num[2] = 0;
    if (i >= 10) {
        num[1] = (char)('0' + i / 10 % 10);
        num[2] = (char)('0' + i % 10);
        num[3] = 0;
        if (k > 5)
            k = 5;
    }
    o[k] = 0;
    scat(o, num, 9);
    if (ext) {
        k = slen(o);
        o[k++] = '.';
        for (i = 1; ext[i] && i < 4; i++)
            o[k++] = up(ext[i]);
        o[k] = 0;
    }
}

static int name83_ok(const char *n)
{
    unsigned i, b = 0, x = 0, dot = 0;
    for (i = 0; n[i]; i++) {
        char c = n[i];
        if (c == '.') {
            if (dot++ || !b)
                return 0;
            continue;
        }
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
            return 0;
        if (dot)
            x++;
        else
            b++;
    }
    return b && b <= 8 && x <= 3;
}

static void sname_of(char *o)
{
    unsigned i;
    if (e.sname[0])
        scopy(o, e.sname, 13);
    else if (name83_ok(e.name)) {
        for (i = 0; e.name[i] && i < 12; i++)
            o[i] = up(e.name[i]);
        o[i] = 0;
    } else
        make83(o, e.name);
}

static int excluded(const char *n83)
{
    static const char *sys[] = { "IO.SYS", "MSDOS.SYS", "IBMBIO.COM", "IBMDOS.COM",
                                 "COMMAND.COM", "CMD.EXE", "ATDOS.SYS", 0 };
    unsigned i;
    if (eq(n83, "EA DATA. SF"))
        return 1;
    if (s8[0])
        return 0;
    for (i = 0; sys[i]; i++)
        if (eq(n83, sys[i]))
            return 1;
    return 0;
}

static void backup_file(void)
{
    static char n83[14], full[FAPI_PATHLEN], rel8[DB_PATHMAX + 14 + 1], rell[FAPI_PATHLEN];
    static struct fapi_finfo fi;
    unsigned h, n, w, seq = 1;
    unsigned long done = 0, part = 0, fr, allow;

    sname_of(n83);
    if (excluded(n83))
        return;
    join(full, ln, e.name, sizeof(full));
    if (o_l && eq(full, logp))
        return;
    h = DosOpen(full);
    if (FAPI_FAILED(h)) {
        out("\r\nWARNUNG! Datei kann nicht gelesen werden: ");
        out(full);
        out("\r\n");
        rc = 4;
        return;
    }
    if (trem && freebytes() < MARGIN + 512 + nlen)
        next_disk();
    if (!have_db || !eq(dbpath, s8))
        new_db();
    scopy(rel8, s8, sizeof(rel8));
    if (rel8[0])
        scat(rel8, "\\", sizeof(rel8));
    scat(rel8, n83, sizeof(rel8));
    scopy(rell, ln + 3, sizeof(rell));  /* ln endet mit \ (oder ist "X:\") */
    scat(rell, e.name, sizeof(rell));
    note_names(rel8, rell);
    out("\\");
    out(rell);
    out("\r\n");
    log_line(rell);
    put_fh(n83, seq, e.size);
    for (;;) {
        n = DosRead(h, buf, CHUNK);
        if (FAPI_FAILED(n) || !n)
            break;
        w = 0;
        while (w < n) {
            unsigned k = n - w;
            if (trem) {
                fr = freebytes();
                allow = fr > MARGIN + 512 + nlen ? fr - MARGIN - 512 - nlen : 0;
                if (allow < k)
                    k = (unsigned)allow;
                if (tfull)
                    k = 0;
                if (!k) {               /* Diskette voll: Teil schliessen */
                    fh_close(FHF_OK, part);
                    next_disk();
                    new_db();
                    note_names(rel8, rell);
                    seq++;
                    part = 0;
                    put_fh(n83, seq, e.size - done);
                    continue;
                }
            }
            k = DosWrite(hb, buf + w, k);
            if (FAPI_FAILED(k) || !k) {
                if (!trem) {
                    fh_close(FHF_LAST, part);
                    DosClose(h);
                    out("Letzte Datei nicht gesichert\r\n");
                    finish_disk(1);
                    out("Datensicherungsger\x84t ");
                    putc(tgt);
                    out(": ist voll\r\n");
                    DosExit(4);
                }
                tfull = 1;              /* Diskette voll (Cluster)    */
                continue;
            }
            w += k;
            blen += k;
            part += k;
            done += k;
        }
        if (n < CHUNK)
            break;
    }
    DosClose(h);
    fh_close(FHF_LAST | FHF_OK, part);
    nfiles++;
    if (e.attr & 0x20) {                /* Archivbit loeschen */
        fi.mask = 1;
        fi.attr = e.attr & 0x27 & ~0x20;
        DosSetFInfo(full, &fi);
    }
}

static int wanted(void)
{
    if (e.attr & 0x18)
        return 0;
    if (!match(e.name, pat) && !(e.sname[0] && match(e.sname, pat)))
        return 0;
    if (o_m && !(e.attr & 0x20))
        return 0;
    if (o_d && e.date < u_date)
        return 0;
    if (o_t) {
        if (o_d ? (e.date == u_date && e.time < u_time) : e.time < u_time)
            return 0;
    }
    return 1;
}

/* alle Dateien eines Verzeichnisses (ln, s8) */
static void do_dir(void)
{
    unsigned n;
    for (n = 0; fapi_call(FAPI_FINDENTRY, (unsigned short)&e, n | FIND_SNAME,
                          (unsigned short)ln) != FAPI_ERROR; n++)
        if (wanted())
            backup_file();
}

static void walk(void)
{
    int lv = 0;
    unsigned n;
    static char sn[14];

    gen = 0;
    do_dir();
    if (!o_s)
        return;
    lvl_idx[0] = 0;
    lvl_s8[0] = (unsigned char)slen(s8);
    lvl_ln[0] = slen(ln);
    while (lv >= 0) {
        n = lvl_idx[lv];
        if (fapi_call(FAPI_FINDENTRY, (unsigned short)&e, n | FIND_SNAME,
                      (unsigned short)ln) == FAPI_ERROR) {
            lv--;
            if (lv >= 0) {
                s8[lvl_s8[lv]] = 0;
                ln[lvl_ln[lv]] = 0;
            }
            continue;
        }
        lvl_idx[lv]++;
        if (!(e.attr & 0x10) || (e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2]))))
            continue;
        if (lv + 1 >= MAXLVL)
            continue;
        sname_of(sn);
        if (slen(s8) + slen(sn) + 1 > DB_PATHMAX || slen(ln) + slen(e.name) + 2 >= FAPI_PATHLEN) {
            out("WARNUNG! Pfad zu lang, \x81" "bergangen: ");
            out(ln);
            out(e.name);
            out("\r\n");
            continue;
        }
        if (s8[0])
            scat(s8, "\\", sizeof(s8));
        scat(s8, sn, sizeof(s8));
        scat(ln, e.name, sizeof(ln));
        scat(ln, "\\", sizeof(ln));
        lv++;
        lvl_idx[lv] = 0;
        lvl_s8[lv] = (unsigned char)slen(s8);
        lvl_ln[lv] = slen(ln);
        gen = 0;
        do_dir();
    }
}

/* ---- Eingabe -------------------------------------------------------- */
static int pnum3(char *p, unsigned *v)
{
    unsigned k = 0, i;
    for (i = 0; i < 3; i++) {
        v[i] = 0;
        if (!(*p >= '0' && *p <= '9'))
            return -1;
        while (*p >= '0' && *p <= '9')
            v[i] = v[i] * 10 + (*p++ - '0');
        k++;
        if (i < 2) {
            if (*p != '.' && *p != '-' && *p != '/' && *p != ':')
                return i == 1 ? (v[2] = 0, 2) : -1;
            p++;
        }
    }
    return (int)k;
}

static int parse_date(char *p, unsigned *out)
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
    *out = ((y - 1980) << 9) | (m << 5) | d;
    return 0;
}

static int parse_time(char *p, unsigned *out)
{
    unsigned v[3];
    int k = pnum3(p, v);
    if (k < 2 || v[0] > 23 || v[1] > 59 || v[2] > 59)
        return -1;
    *out = (v[0] << 11) | (v[1] << 5) | (v[2] >> 1);
    return 0;
}

static void usage(void)
{
    out("Sichert eine oder mehrere Dateien von einem Datentr\x84ger auf einen anderen.\r\n\r\n"
        "BACKUP Quelle Ziellaufwerk: [/S] [/M] [/A] [/F[:Gr\x94\xE1" "e]]\r\n"
        "       [/D:Datum [/T:Zeit]] [/L[:[Laufwerk:][Pfad]Protokolldatei]]\r\n\r\n"
        "  Quelle        Zu sichernde(s) Laufwerk, Verzeichnis und/oder Dateien.\r\n"
        "  Ziellaufwerk: Laufwerk, auf dem Sicherungskopien angelegt werden sollen.\r\n"
        "  /S            Sichert auch Dateien in allen Unterverzeichnissen.\r\n"
        "  /M            Sichert nur seit der letzten Sicherung ge\x84nderte Dateien.\r\n"
        "  /A            F\x81gt neue Dateien zu denen auf dem Sicherungsdatentr\x84ger hinzu.\r\n"
        "  /F:Gr\x94\xE1" "e       Gibt die Gr\x94\xE1" "e der zu formatierenden Diskette an.\r\n"
        "  /D:Datum      Sichert nur die an oder nach dem Datum ge\x84nderten Dateien.\r\n"
        "  /T:Zeit       Sichert nur die an oder nach dem Zeitpunkt ge\x84nderten Dateien.\r\n"
        "  /L:Datei      Legt eine Protokolldatei an (Vorgabe Quelle:\\BACKUP.LOG).\r\n\r\n"
        "Lange Namen stehen in NAMES.nnn; RESTORE von MS-DOS stellt 8.3-Namen her.\r\n");
}

static void bad(const char *m)
{
    out(m);
    out("\r\n");
    DosExit(4);
}

void main(void)
{
    static char w[FAPI_PATHLEN], full[FAPI_PATHLEN], dt[16];
    static struct fapi_fsinfo fi;
    static unsigned short now[2];
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
        if (*a == '/') {
            char c = up(a[1]);
            word(a, w, sizeof(w));
            if (c == 'S' && !a[2]) o_s = 1;
            else if (c == 'S' && (a[2] == ' ' || a[2] == '/')) o_s = 1;
            else if (c == 'M') o_m = 1;
            else if (c == 'A') o_a = 1;
            else if (c == 'F') {
                o_f = 1;
                if (w[2] == ':')
                    scopy(fsize, w + 3, sizeof(fsize));
            } else if (c == 'D' && w[2] == ':') {
                o_d = 1;
                if (parse_date(w + 3, &u_date))
                    bad("Unzul\x84ssige Datumsangabe");
            } else if (c == 'T' && w[2] == ':') {
                o_t = 1;
                if (parse_time(w + 3, &u_time))
                    bad("Unzul\x84ssige Zeitangabe");
            } else if (c == 'L') {
                o_l = 1;
                if (w[2] == ':')
                    scopy(logp, w + 3, sizeof(logp));
            } else {
                out("Ung\x81ltiger Schalter - ");
                bad(w);
            }
            continue;
        }
        word(a, w, sizeof(w));
        if (ns == 0)
            scopy(src, w, sizeof(src));
        else if (ns == 1) {
            if (!w[0] || w[1] != ':' || w[2])
                bad("Ung\x81ltige Laufwerksangabe");
            tgt = up(w[0]);
        } else
            bad("Unzul\x84ssige Anzahl von Parametern");
        ns++;
    }
    if (ns < 1)
        bad("Kein Quellaufwerk angegeben");
    if (ns < 2)
        bad("Kein Ziellaufwerk angegeben");

    /* Quelle: Laufwerk, Verzeichnis, Muster */
    if (abs_path(src, full))
        bad("Ung\x81ltiger Pfad");
    sdrv = up(full[0]);
    if (sdrv == tgt)
        bad("Quell- und Ziellaufwerk sind identisch");
    i = slen(full);
    if (full[i - 1] == '\\' || is_dir(full)) {
        if (full[i - 1] != '\\')
            scat(full, "\\", sizeof(full));
        scopy(pat, "*.*", sizeof(pat));
    } else {
        char *b = base_name(full);
        scopy(pat, b, sizeof(pat));
        *b = 0;
    }
    scopy(ln, full, sizeof(ln));        /* "X:\DIR\" */
    /* 8.3-Pfad des Startverzeichnisses: ueber die Suche je Stufe */
    s8[0] = 0;
    {
        static char part[FAPI_PATHLEN], sn[14];
        unsigned p = 3, q;
        while (ln[p]) {
            unsigned n;
            for (q = p; ln[q] && ln[q] != '\\'; q++)
                ;
            scopy(part, ln, p + 1);         /* Elternverzeichnis "X:\..\" */
            scopy(w, ln + p, q - p + 1);
            for (n = 0; fapi_call(FAPI_FINDENTRY, (unsigned short)&e, n | FIND_SNAME,
                                  (unsigned short)part) != FAPI_ERROR; n++)
                if ((e.attr & 0x10) && (eq(e.name, w) || (e.sname[0] && eq(e.sname, w))))
                    break;
            if (!(e.attr & 0x10) || !(eq(e.name, w) || (e.sname[0] && eq(e.sname, w))))
                bad("Ung\x81ltiger Pfad");
            gen = 0;
            sname_of(sn);
            if (s8[0])
                scat(s8, "\\", sizeof(s8));
            scat(s8, sn, sizeof(s8));
            /* Originalschreibung in ln uebernehmen */
            for (n = 0; n < q - p && e.name[n]; n++)
                ln[p + n] = e.name[n];
            p = ln[q] ? q + 1 : q;
        }
        if (slen(s8) > DB_PATHMAX)
            bad("Ung\x81ltiger Pfad");
    }

    if (DosQFSInfo(tgt - 'A', &fi) && !is_raw())
        bad("Ung\x81ltige Laufwerksangabe");
    trem = (fi.flags & (FSI_REMOVABLE | FSI_REMDISK)) != 0;

    if (o_l) {
        if (!logp[0]) {
            logp[0] = sdrv;
            scopy(logp + 1, ":\\BACKUP.LOG", sizeof(logp) - 1);
        }
        abs_path(logp, w);
        scopy(logp, w, sizeof(logp));
        hl = DosOpenRW(logp);
        if (FAPI_FAILED(hl))
            hl = DosCreate(logp);
        else {
            static struct fapi_dirent de;
            static char dir[FAPI_PATHLEN];
            unsigned long sz = 0;
            char *b;
            scopy(dir, logp, sizeof(dir));
            b = base_name(dir);
            *b = 0;
            for (i = 0; DosFindEntry(&de, i, dir) != FAPI_ERROR; i++)
                if (eq(de.name, base_name(logp)))
                    sz = de.size;
            seek_to(hl, sz);
        }
        if (FAPI_FAILED(hl))
            bad("Fehler beim \x99" "ffnen der Protokolldatei");
        out("Protokoll wird in Datei ");
        out(logp);
        out(" geschrieben\r\n");
        DosGetDateTime(now);
        ctry_date(dt, now[0]);
        DosWrite(hl, "\r\n", 2);
        DosWrite(hl, dt, slen(dt));
        DosWrite(hl, " ", 1);
        ctry_time(dt, now[1], 1);
        DosWrite(hl, dt, slen(dt));
    }

    /* erster Datentraeger */
    disk = 1;
    if (trem) {
        if (o_a) {
            ask_disk(1);
            if (open_last()) {
                out("Letzte Sicherungsdiskette nicht eingelegt\r\n");
                DosExit(4);
            }
        } else {
            ask_disk(0);
            delete_all();
            if (create_files())
                bad("Zieldatentr\x84ger kann nicht zum Sichern verwendet werden");
        }
    } else {
        tpath(w, "");
        w[slen(w) - 1] = 0;
        if (!o_a || open_last()) {
            if (is_dir(w)) {
                con("WARNUNG! Dateien im Verzeichnis ");
                con(w);
                con(" des Ziellaufwerks werden gel\x94scht\r\n");
                wait_key();
                delete_all();
            } else if (DosMkDir(w))
                bad("Zieldatentr\x84ger kann nicht zum Sichern verwendet werden");
            disk = 1;
            if (create_files())
                bad("Zieldatentr\x84ger kann nicht zum Sichern verwendet werden");
        }
    }
    out("\r\n*** Sicherung der Dateien auf Laufwerk ");
    putc(tgt);
    out(": ***\r\n");
    if (trem)
        say_disk();

    walk();

    finish_disk(1);
    if (o_l && !FAPI_FAILED(hl)) {
        DosWrite(hl, "\r\n", 2);
        DosClose(hl);
    }
    if (!nfiles) {
        out("WARNUNG! Keine Dateien zum Sichern gefunden\r\n");
        DosExit(1);
    }
    DosExit(rc);
}
