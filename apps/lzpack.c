/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Gemeinsamer Rahmen von LHA.EXE, ARJ.EXE und ZOO.EXE (task_lha.c/
 * task_arj.c/task_zoo.c setzen FMT_LHA, FMT_ARJ bzw. FMT_ZOO): Archive
 * anlegen und pflegen, Bedienung wie die Originale (nur Verhalten
 * nachgebaut, kein fremder Code):
 *
 *   LHA a|u|f|m|d [-r] [-o5|-o6|-o7|-z] [-h1|-h2] [-q] Archiv[.LZH] [Datei...]
 *   ARJ a|u|f|m|d [-r] [-m0..-m3] [-q] Archiv[.ARJ] [Datei...]
 *   ZOO a|u|f|m|d [-r] [-q] Archiv[.ZOO] [Datei...]  (immer lh5 wie "zoo ah")
 *   ... x|e|l|v|t  wie UNLHA/UNARJ (unarc.c)
 *
 * a fuegt hinzu (gleiche Namen werden ersetzt), u nur Neues oder Neueres,
 * f nur schon vorhandene, wenn neuer, m wie a und loescht danach die
 * Quellen, d loescht Eintraege. Das Archiv wird immer ueber eine
 * Zwischendatei (NAME.$$$) neu aufgebaut - alte Eintraege werden roh
 * uebernommen. Packen mit lzhenc.c; was nicht kleiner wird, wird
 * gespeichert (lh0 / Methode 0). Verzeichnisse selbst werden nicht als
 * Eintrag abgelegt (die Pfade der Dateien reichen).
 */
#define UNARC_LIB
#include "unarc.c"
#include "lzhenc.h"

#ifdef FMT_ARJ
#define PNAME "arj"
#elif defined FMT_ZOO
#define PNAME "zoo"
#else
#define PNAME "lha"
#endif

static int cmd, o_r, o_qq;
#ifndef FMT_ARJ
static int lvl = 6, ometh;
#endif
#ifdef FMT_ARJ
static int amethod = 1;                 /* -m1 Vorgabe wie ARJ           */
#elif defined FMT_LHA
static int hlev = 1;
#endif
static char tname[FAPI_PATHLEN], dirp[FAPI_PATHLEN], pat[FAPI_PATHLEN];
static char abase[FAPI_PATHLEN], tree[FAPI_PATHLEN], aname[FAPI_PATHLEN];
static char pbuf[FAPI_PATHLEN];
static unsigned htmp = 0xFFFF, hin = 0xFFFF, fbase, nadd, ndel;
static unsigned long tpos;
static int twerr;
static unsigned char eh[600];
static unsigned long zpos;

/* ---- Liste der neuen Dateien (fern) -------------------------------------- */
struct nf {
    unsigned long size;
    unsigned date, time, attr;
    unsigned po;                        /* Stelle im Namensspeicher     */
    unsigned char ps, flag;             /* Segment, 1 = uebergehen       */
};
#define NF_MAX 4096
#define NPS    3
static struct nf __far *nfl;
static unsigned nfsel, nfn, psel[NPS], npsel, pofs;

static char __far *pool(unsigned s, unsigned o)
{
    return MK_FP(psel[s], o);
}

static int nf_add(const char *full, const char *an, struct fapi_dirent *e)
{
    unsigned l1 = slen(full) + 1, l2 = slen(an) + 1, i;
    char __far *p;

    if (nfn >= NF_MAX)
        return 1;
    if (!npsel || (unsigned long)pofs + l1 + l2 > 65535UL) {
        if (npsel == NPS)
            return 1;
        psel[npsel] = DosAllocSeg(0);
        if (FAPI_FAILED(psel[npsel]) || !psel[npsel])
            return 1;
        npsel++;
        pofs = 0;
    }
    nfl[nfn].size = e->size;
    nfl[nfn].date = e->date;
    nfl[nfn].time = e->time;
    nfl[nfn].attr = e->attr;
    nfl[nfn].ps = (unsigned char)(npsel - 1);
    nfl[nfn].po = pofs;
    nfl[nfn].flag = 0;
    p = pool(npsel - 1, pofs);
    for (i = 0; i < l1; i++)
        p[i] = full[i];
    for (i = 0; i < l2; i++)
        p[l1 + i] = an[i];
    pofs += l1 + l2;
    nfn++;
    return 0;
}

static void nf_full(unsigned i, char *out)
{
    char __far *p = pool(nfl[i].ps, nfl[i].po);
    unsigned k;
    for (k = 0; k + 1 < FAPI_PATHLEN && p[k]; k++)
        out[k] = p[k];
    out[k] = 0;
}

static void nf_name(unsigned i, char *out)
{
    char __far *p = pool(nfl[i].ps, nfl[i].po);
    unsigned k;
    while (*p)
        p++;
    p++;
    for (k = 0; k + 1 < FAPI_PATHLEN && p[k]; k++)
        out[k] = p[k];
    out[k] = 0;
}

static int eqname(const char *a, const char *b)  /* ganz, ohne Gross/klein */
{
    while (*a && up(*a) == up(*b)) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int nf_find(const char *n)
{
    unsigned i;
    for (i = 0; i < nfn; i++) {
        nf_name(i, pbuf);
        if (eqname(pbuf, n))
            return (int)i;
    }
    return -1;
}

/* ---- Dateien sammeln (wie ZIP) --------------------------------------------- */
static void make_aname(const char *path)
{
    char *p;
    scopy(aname, abase, sizeof(aname));
    scat(aname, path + fbase, sizeof(aname));
    for (p = aname; *p; p++)
        if (*p == '\\')
            *p = '/';
    for (p = aname; *p == '/' || (p[0] == '.' && p[1] == '/'); p += *p == '/' ? 1 : 2)
        ;
    if (p != aname)
        scopy(aname, p, sizeof(aname));
}

static int collect(const char *full, struct fapi_dirent *e)
{
    if (same(full, arcfull) && slen(full) == slen(arcfull))
        return 0;                       /* das Archiv selbst             */
    if (same(full, tname) && slen(full) == slen(tname))
        return 0;
    make_aname(full);
    if (nf_find(aname) >= 0)
        return 0;
    if (nf_add(full, aname, e)) {
        puts(PNAME ": zu viele Dateien\r\n");
        return 1;
    }
    return 0;
}

static int visit(char *path, struct fapi_dirent *d, int what, int depth)
{
    (void)depth;
    if (what == WT_LEAVE)
        return 0;
    if (what == WT_DIR)
        return WT_DESCEND;
    return collect(path, d) ? WT_STOP : 0;
}

static void add_arg(const char *arg)
{
    static struct fapi_dirent e;
    unsigned n, found = 0;
    char *b;
    const char *t;

    abs_path(arg, dirp);
    if (is_dir(dirp)) {                 /* Verzeichnis: alles darin       */
        scopy(pat, "*.*", sizeof(pat));
        t = arg;
        if (t[0] && t[1] == ':')
            t += 2;
        scopy(abase, t, sizeof(abase));
        if (abase[0] && abase[slen(abase) - 1] != '\\' && abase[slen(abase) - 1] != '/')
            scat(abase, "\\", sizeof(abase));
        scopy(wbuf, abase, sizeof(wbuf));
    } else {
        b = base_name(dirp);
        scopy(pat, b, sizeof(pat));
        *b = 0;
        t = arg;
        if (t[0] && t[1] == ':')
            t += 2;
        scopy(wbuf, t, sizeof(wbuf));
        b = base_name(wbuf);
        *b = 0;
    }
    if (slen(dirp) > 3 && dirp[slen(dirp) - 1] == '\\')
        dirp[slen(dirp) - 1] = 0;
    for (n = 0; DosFindEntry(&e, n, dirp) != FAPI_ERROR; n++) {
        if (!match(e.name, pat) || (e.attr & 0x08) || (e.name[0] == '.' &&
            (!e.name[1] || (e.name[1] == '.' && !e.name[2]))))
            continue;
        if ((e.attr & 0x10) && !o_r)
            continue;
        found++;
        join(full, dirp, e.name, sizeof(full));
        scopy(abase, wbuf, sizeof(abase));
        scat(abase, e.name, sizeof(abase));
        fbase = slen(full);
        if (e.attr & 0x10) {
            scopy(tree, full, sizeof(tree));
            if (walk_tree(tree, visit))
                return;
        } else if (collect(full, &e))
            return;
    }
    if (!found)
        say(PNAME ": ", arg, ": nicht gefunden\r\n");
}

/* d: Pfad wie bei UNZIP, ohne '/' in der Angabe auch der Dateiname allein */
static int d_match(const char *n)
{
    char *a, *b;
    unsigned k, sl;
    if (member_ok(n))
        return 1;
    b = base_name((char *)n);
    for (k = 0; n[k]; k++)
        if (n[k] == '/')
            b = (char *)n + k + 1;
    for (a = mfirst; *a; a = next_word(a)) {
        word(a, pat, sizeof(pat));
        for (k = 0, sl = 0; pat[k]; k++)
            if (pat[k] == '/' || pat[k] == '\\')
                sl = 1;
        if (!sl && (has_wild(pat) ? match(b, pat) : eqname(b, pat)))
            return 1;
    }
    return 0;
}

/* ---- Zwischendatei ------------------------------------------------------- */
static int wr_tmp(const unsigned char *p, unsigned n)
{
    if (twerr || DosWrite(htmp, p, n) != n) {
        twerr = 1;
        return 1;
    }
    tpos += n;
    return 0;
}

static int tseek(unsigned long pos)
{
    static unsigned long sp;
    sp = pos;
    tpos = pos;
    return FAPI_FAILED(DosSeek(htmp, &sp));
}

static void p16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void p32(unsigned char *p, unsigned long v)
{
    p16(p, (unsigned)v);
    p16(p + 2, (unsigned)(v >> 16));
}

/* ---- Koepfe -------------------------------------------------------------- */
#ifdef FMT_ARJ
/* ARJ-Kopf: 60 EA, Groesse, Grundkopf (30 Byte + Name + Kommentar), CRC-32,
 * Groesse der Zusatzkoepfe 0 */
static unsigned arj_mkhdr(unsigned char *h, const char *name, int type, unsigned meth,
                        unsigned long cs, unsigned long us, unsigned long crc,
                        unsigned date, unsigned time, unsigned attr)
{
    unsigned nl = slen(name), bs = 30 + nl + 2, i, fs = 0;
    unsigned char *b = h + 4;

    h[0] = 0x60;
    h[1] = 0xEA;
    p16(h + 2, bs);
    b[0] = 30;
    b[1] = 6;                           /* erzeugt mit „6“ wie ARJ 2.50  */
    b[2] = 1;
    b[3] = 0;                           /* MS-DOS                        */
    b[4] = 0x10;                        /* Pfade mit '/'                 */
    b[5] = (unsigned char)meth;
    b[6] = (unsigned char)type;
    b[7] = 0;
    p16(b + 8, time);
    p16(b + 10, date);
    p32(b + 12, cs);
    p32(b + 16, us);
    p32(b + 20, crc);
    for (i = 0; i < nl; i++)
        if (name[i] == '/')
            fs = i + 1;
    p16(b + 24, fs);
    p16(b + 26, attr & 0x27);
    p16(b + 28, 0);
    for (i = 0; i < nl; i++)
        b[30 + i] = (unsigned char)name[i];
    if (type == 2)                      /* Hauptkopf: 12 = geaendert     */
        b[4] = 0;
    b[30 + nl] = 0;
    b[31 + nl] = 0;
    p32(b + bs, crc32a(0, b, bs));
    p16(b + bs + 4, 0);
    return 4 + bs + 4 + 2;
}
#elif defined FMT_ZOO
/* ZOO-Eintrag Typ 2 (zoo.h von Zoo 2.10): fester Teil 56 Byte, dann
 * Namenslaenge, Verzeichnislaenge, langer Name, Verzeichnis, System-ID,
 * Attribute (3), Versionsmerker (1), Versionsnummer (2); CRC-16 ueber den
 * ganzen Eintrag mit 0 an Stelle 54. Leerer Name = Endeintrag. */
static unsigned zoo_mkhdr(unsigned char *h, const char *name, unsigned meth,
                          unsigned long next, unsigned long off, unsigned long cs,
                          unsigned long us, unsigned crc, unsigned date, unsigned time)
{
    unsigned dl = 0, nl, i, k, b, e, n, vl;
    const char *fn = name;
    char sn[13];

    for (i = 0; i < 56; i++)
        h[i] = 0;
    p32(h, 0xFDC4A7DCUL);
    h[4] = 2;
    if (!name[0]) {                     /* Endeintrag                    */
        p16(h + 54, crc16(0, h, 56));
        return 56;
    }
    for (i = 0; name[i]; i++)
        if (name[i] == '/') {
            dl = i;                     /* Verzeichnis ohne Schraegstrich */
            fn = name + i + 1;
        }
    nl = slen(fn);
    for (e = nl; e > 0 && fn[e - 1] != '.'; e--)
        ;
    b = e ? e - 1 : nl;                 /* Ende des Grundnamens          */
    for (i = k = 0; i < b && k < 8; i++)
        if (fn[i] != '.' && fn[i] != ' ')
            sn[k++] = fn[i];
    if (e) {
        sn[k++] = '.';
        for (i = e; i < nl && i < e + 3; i++)
            sn[k++] = fn[i];
    }
    sn[k] = 0;
    h[5] = (unsigned char)meth;
    p32(h + 6, next);
    p32(h + 10, off);
    p16(h + 14, date);
    p16(h + 16, time);
    p16(h + 18, crc);
    p32(h + 20, us);
    p32(h + 24, cs);
    h[28] = meth ? 2 : 1;               /* noetige Version: lh5 2.1      */
    h[29] = meth ? 1 : 0;
    for (i = 0; i <= k; i++)
        h[38 + i] = (unsigned char)sn[i];
    h[53] = 127;                        /* Zeitzone unbekannt            */
    if (k == nl) {
        for (i = 0; i < nl && sn[i] == fn[i]; i++)
            ;
        if (i == nl)
            nl = 0;                     /* 8.3 reicht                    */
    }
    n = 56;                             /* Laengen mit Null wie Zoo 2.10 */
    h[n++] = (unsigned char)(nl ? nl + 1 : 0);
    h[n++] = (unsigned char)(dl ? dl + 1 : 0);
    if (nl) {
        for (i = 0; i < nl; i++)
            h[n++] = (unsigned char)fn[i];
        h[n++] = 0;
    }
    if (dl) {
        for (i = 0; i < dl; i++)
            h[n++] = (unsigned char)name[i];
        h[n++] = 0;
    }
    p16(h + n, 1);                      /* System MS-DOS                 */
    n += 2;
    h[n++] = 0;                         /* keine Attribute               */
    h[n++] = 0;
    h[n++] = 0;
    h[n++] = 0;                         /* keine Versionen               */
    h[n++] = 0;
    h[n++] = 0;
    vl = n - 56;
    p16(h + 51, vl);
    p16(h + 54, crc16(0, h, n));
    return n;
}
#else
/* LHA-Kopf Stufe 1 (Name im Grundkopf, Verzeichnis und Attribut als
 * Zusatzkopf, Packgroesse mit Zusatzkoepfen) oder Stufe 2 */
static unsigned lha_mkhdr(unsigned char *h, const char *name, unsigned meth,
                        unsigned long cs, unsigned long us, unsigned crc,
                        unsigned date, unsigned time, unsigned attr)
{
    unsigned nl, dl = 0, i, k, n, sum;
    const char *fn = name;
    unsigned long ut;

    for (i = 0; name[i]; i++)
        if (name[i] == '/') {
            dl = i + 1;                 /* Verzeichnis mit Schraegstrich */
            fn = name + i + 1;
        }
    nl = slen(fn);
    h[2] = '-'; h[3] = 'l'; h[4] = 'h'; h[5] = (unsigned char)('0' + meth); h[6] = '-';
    p32(h + 11, us);
    if (hlev == 1) {
        n = 22;
        h[n - 1] = (unsigned char)nl;
        for (i = 0; i < nl; i++)
            h[n++] = (unsigned char)fn[i];
        p16(h + n, crc);
        n += 2;
        h[n++] = 'M';
        k = n;                          /* erste Zusatzgroesse           */
        n += 2;
        h[0] = (unsigned char)(n - 2);
        p16(h + 15, time);
        p16(h + 17, date);
        h[19] = 0x20;
        h[20] = 1;
        if (dl) {
            p16(h + k, 3 + dl);
            h[n++] = 2;
            for (i = 0; i < dl; i++)
                h[n++] = (unsigned char)(name[i] == '/' ? 0xFF : name[i]);
            k = n;
            n += 2;
        }
        p16(h + k, 5);
        h[n++] = 0x40;
        p16(h + n, attr & 0x27);
        n += 2;
        p16(h + n, 0);
        n += 2;
        p32(h + 7, cs + (n - (h[0] + 2)));  /* Zusatzkoepfe zaehlen mit */
        for (i = 2, sum = 0; i < (unsigned)h[0] + 2; i++)
            sum += h[i];
        h[1] = (unsigned char)sum;
        return n;
    }
    p32(h + 7, cs);
    ut = fat2unix(date, time);
    p32(h + 15, ut);
    h[19] = 0x20;
    h[20] = 2;
    p16(h + 21, crc);
    h[23] = 'M';
    n = 24;
    p16(h + n, 3 + nl);                 /* Name                          */
    n += 2;
    h[n++] = 1;
    for (i = 0; i < nl; i++)
        h[n++] = (unsigned char)fn[i];
    if (dl) {
        p16(h + n, 3 + dl);
        n += 2;
        h[n++] = 2;
        for (i = 0; i < dl; i++)
            h[n++] = (unsigned char)(name[i] == '/' ? 0xFF : name[i]);
    }
    p16(h + n, 5);
    n += 2;
    h[n++] = 0x40;
    p16(h + n, attr & 0x27);
    n += 2;
    p16(h + n, 0);
    n += 2;
    if ((n & 0xFF) == 0)                /* 0 vorn hiesse Archivende      */
        h[n++] = 0;
    p16(h, n);
    return n;
}
#endif

/* ---- eine Datei packen ---------------------------------------------------- */
static unsigned rdi(unsigned char *b, unsigned n)
{
    unsigned r = DosRead(hin, b, n);
    return FAPI_FAILED(r) ? 0 : r;
}

static int add_one(unsigned i)
{
    unsigned long hpos, dpos, cs, us = nfl[i].size, crc = 0;
    unsigned hl, n, meth;
    int stored = 0;

    nf_full(i, full);
    nf_name(i, aname);
    hin = DosOpen(full);
    if (FAPI_FAILED(hin)) {
        say(PNAME ": ", full, ": kann nicht gelesen werden\r\n");
        errors++;
        hin = 0xFFFF;
        return 0;
    }
#ifdef FMT_ARJ
    meth = amethod;
    hl = arj_mkhdr(eh, aname, 0, meth, 0, us, 0, nfl[i].date, nfl[i].time, nfl[i].attr);
#elif defined FMT_ZOO
    meth = ometh ? 2 : 0;
    hl = zoo_mkhdr(eh, aname, meth, 0, 0, 0, us, 0, nfl[i].date, nfl[i].time);
#else
    meth = ometh;
    hl = lha_mkhdr(eh, aname, meth, 0, us, 0, nfl[i].date, nfl[i].time, nfl[i].attr);
#endif
    hpos = tpos;
    wr_tmp(eh, hl);
    dpos = tpos;
    if (us == 0)
        meth = 0;
    if (meth) {
#ifdef FMT_ARJ
        lze_begin(LZE_ARJ, meth == 1 ? 9 : meth == 2 ? 6 : 3);
#elif defined FMT_ZOO
        lze_begin(LZE_LH5, lvl);
#else
        lze_begin(meth, lvl);
#endif
        lze_write = wr_tmp;
        while ((n = rdi(db, sizeof(db))) != 0 && !twerr) {
#ifdef FMT_ARJ
            crc = crc32a(crc, db, n);
#else
            crc = crc16((unsigned)crc, db, n);
#endif
            lze_feed(db, n);
        }
#ifdef FMT_ZOO
        lze_end(1);                     /* Zoo erwartet die Endmarke     */
#else
        lze_end(0);
#endif
        if (tpos - dpos >= us) {        /* nicht kleiner: speichern      */
            stored = 1;
            tseek(dpos);
            zpos = 0;
            DosSeek(hin, &zpos);
        }
    }
    if (!meth || stored) {
        meth = 0;
        crc = 0;
        while ((n = rdi(db, sizeof(db))) != 0 && !twerr) {
#ifdef FMT_ARJ
            crc = crc32a(crc, db, n);
#else
            crc = crc16((unsigned)crc, db, n);
#endif
            wr_tmp(db, n);
        }
    }
    DosClose(hin);
    hin = 0xFFFF;
    if (twerr)
        return 1;
    cs = tpos - dpos;
#ifdef FMT_ARJ
    hl = arj_mkhdr(eh, aname, 0, meth, cs, us, crc, nfl[i].date, nfl[i].time, nfl[i].attr);
#elif defined FMT_ZOO
    hl = zoo_mkhdr(eh, aname, meth, tpos, hpos + hl, cs, us, (unsigned)crc,
                   nfl[i].date, nfl[i].time);
#else
    hl = lha_mkhdr(eh, aname, meth, cs, us, (unsigned)crc, nfl[i].date, nfl[i].time, nfl[i].attr);
#endif
    dpos = tpos;
    if (tseek(hpos) || wr_tmp(eh, hl) || tseek(dpos))
        return 1;
    nadd++;
    nfl[i].flag |= 2;                   /* aufgenommen (fuer m)          */
    if (!o_qq) {
        puts(meth ? "  gepackt:  " : " speichert: ");
        puts(aname);
        puts(" (");
        put_ratio(us, cs);
        puts(")\r\n");
    }
    return 0;
}

/* ---- alte Eintraege uebernehmen ------------------------------------------ */
static int copy_old(unsigned long from, unsigned long to)
{
    unsigned n;
    if (seek(from))
        return 1;
    while (from < to && !twerr) {
        n = to - from > sizeof(db) ? sizeof(db) : (unsigned)(to - from);
        if (rd(db, n))
            return 1;
        wr_tmp(db, n);
        from += n;
    }
    return twerr;
}

#ifdef FMT_ZOO
/* ZOO-Eintraege verweisen absolut aufeinander: Kopf mit neuen Stellen
 * schreiben (Daten direkt dahinter), CRC neu */
static int zoo_copy(void)
{
    unsigned long hp = tpos, dp = hp + e_zhl;
    unsigned i;
    for (i = 0; i < e_zhl; i++)
        eh[i] = hb[i];
    p32(eh + 6, dp + e_cs);
    p32(eh + 10, dp);
    if (e_zhl >= 56) {
        p16(eh + 54, 0);
        p16(eh + 54, crc16(0, eh, e_zhl));
    }
    if (wr_tmp(eh, e_zhl))
        return 1;
    return copy_old(e_data, e_data + e_cs);
}
#endif

static void pusage(void)
{
#ifdef FMT_ARJ
    puts("Legt ARJ-Archive an und pflegt sie (Methode 1-3 wie ARJ 2.x).\r\n\r\n"
         "ARJ Befehl [Optionen] Archiv[.ARJ] [Datei...]\r\n\r\n"
         "  a  hinzuf\x81gen   u  Neues/Neueres   f  nur Vorhandenes erneuern\r\n"
         "  m  verschieben  d  l\x94schen   x/e  entpacken   l/v  zeigen   t  pr\x81" "fen\r\n"
         "  -r  Unterverzeichnisse   -m0..-m3  Methode (0 speichern, 1 beste)\r\n"
         "  -q  still\r\n");
#elif defined FMT_ZOO
    puts("Legt ZOO-Archive an und pflegt sie (lh5 wie \"zoo ah\").\r\n\r\n"
         "ZOO Befehl [Optionen] Archiv[.ZOO] [Datei...]\r\n\r\n"
         "  a  hinzuf\x81gen   u  Neues/Neueres   f  nur Vorhandenes erneuern\r\n"
         "  m  verschieben  d  l\x94schen   x/e  entpacken   l/v  zeigen   t  pr\x81" "fen\r\n"
         "  -r  Unterverzeichnisse   -z  nur speichern   -q  still\r\n");
#else
    puts("Legt LHA-Archive an und pflegt sie (lh5/lh6/lh7 wie LHA 2.x).\r\n\r\n"
         "LHA Befehl [Optionen] Archiv[.LZH] [Datei...]\r\n\r\n"
         "  a  hinzuf\x81gen   u  Neues/Neueres   f  nur Vorhandenes erneuern\r\n"
         "  m  verschieben  d  l\x94schen   x/e  entpacken   l/v  zeigen   t  pr\x81" "fen\r\n"
         "  -r  Unterverzeichnisse   -o5/-o6/-o7  Verfahren (Vorgabe lh5)\r\n"
         "  -z  nur speichern   -h1/-h2  Kopfstufe (Vorgabe 1)   -q  still\r\n");
#endif
}

void main(void)
{
    char *a, *w;
    unsigned i, k, ml = 0;
    int r, old = 0;
    static unsigned long nsz;

    DosGetArgs(args);
    a = skip(args);
    if (!*a) {
        pusage();
        DosExit(10);
    }
    word(a, wbuf, sizeof(wbuf));
    k = up(wbuf[0]);
#ifdef FMT_ZOO
    if (k == 'A' || k == 'U' || k == 'F' || k == 'M' || k == 'D')
        wbuf[1] = 0;                    /* Zusaetze wie "ah" uebergehen  */
#endif
    if (wbuf[0] && !wbuf[1] && (k == 'X' || k == 'E' || k == 'L' || k == 'V' || k == 'T')) {
        unarc_main(a);                  /* entpacken, zeigen, pruefen    */
        DosExit(errors ? 1 : 0);
    }
    if (!wbuf[0] || wbuf[1] || (k != 'A' && k != 'U' && k != 'F' && k != 'M' && k != 'D')) {
        pusage();
        DosExit(wbuf[0] == '/' || wbuf[0] == '-' ? 0 : 10);
    }
    cmd = k;
#ifndef FMT_ARJ
    ometh = 5;
#endif
    nfsel = DosAllocSeg(0);
    if (FAPI_FAILED(nfsel) || !nfsel || lze_open()) {
        puts(PNAME ": zu wenig Speicher\r\n");
        DosExit(8);
    }
    nfl = MK_FP(nfsel, 0);
    for (a = next_word(a); *a; a = next_word(a)) {
        word(a, wbuf, sizeof(wbuf));
        w = wbuf;
        if ((w[0] == '-' && w[1]) || (w[0] == '/' && w[1] && slen(w) <= 3)) {
            {
                k = up(w[1]);
                if (k == 'R')
                    o_r = 1;
                else if (k == 'Q')
                    o_qq = o_q = 1;
                else if (k == 'Y')
                    o_o = 1;
#ifdef FMT_ARJ
                else if (k == 'M' && w[2] >= '0' && w[2] <= '4')
                    amethod = w[2] == '4' ? 3 : w[2] - '0';
#elif defined FMT_ZOO
                else if (k == 'Z')
                    ometh = 0;
#else
                else if (k == 'O' && (w[2] == 0 || w[2] == '5' || w[2] == '6' || w[2] == '7'))
                    ometh = w[2] ? w[2] - '0' : 5;
                else if (k == 'Z')
                    ometh = 0;
                else if (k == 'H' && (w[2] == '1' || w[2] == '2'))
                    hlev = w[2] - '0';
#endif
                else {
                    say(PNAME ": unbekannte Option ", wbuf, "\r\n");
                    DosExit(10);
                }
                continue;
            }
        }
        if (!arcname[0]) {
            scopy(arcname, wbuf, sizeof(arcname));
            continue;
        }
        k = slen(wbuf);                 /* Dateiangaben merken           */
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
    if (!arcname[0]) {
        pusage();
        DosExit(10);
    }
    abs_path(arcname, arcfull);
    w = base_name(arcfull);
    for (k = 0; w[k] && w[k] != '.'; k++)
        ;
    if (!w[k])
        scat(arcfull, DEFEXT, sizeof(arcfull));
    scopy(tname, arcfull, sizeof(tname));
    w = base_name(tname);
    for (k = 0; w[k] && w[k] != '.'; k++)
        ;
    w[k] = 0;
    scat(tname, ".$$$", sizeof(tname));
    harc = DosOpen(arcfull);
    if (!FAPI_FAILED(harc)) {
        old = 1;
        fmt = detect();
#ifdef FMT_ARJ
        if (fmt != 'A') {
#elif defined FMT_ZOO
        if (fmt != 'Z') {
#else
        if (fmt != 'L') {
#endif
            say(PNAME ": ", arcfull, ": kein " PROG_UP "-Archiv\r\n");
            DosExit(9);
        }
    } else if (cmd == 'D' || cmd == 'F') {
        say(PNAME ": ", arcfull, ": nicht gefunden\r\n");
        DosExit(9);
    }
    if (cmd == 'D') {
        if (!mbuf[0]) {
            puts(PNAME ": was l\x94schen?\r\n");
            DosExit(10);
        }
        mfirst = mbuf;
    } else {
        if (!mbuf[0])
            scopy(mbuf, "*.*", sizeof(mbuf));
        for (a = mbuf; *a; a = next_word(a)) {
            word(a, pbuf, sizeof(pbuf));
            add_arg(pbuf);
        }
        if (!nfn) {
            puts(PNAME ": keine Dateien\r\n");
            DosExit(1);
        }
    }
    htmp = DosCreate(tname);
    if (FAPI_FAILED(htmp)) {
        say(PNAME ": ", tname, ": kann nicht angelegt werden\r\n");
        DosExit(1);
    }
    tpos = 0;
#ifdef FMT_ARJ
    {
        unsigned d, t;
        fat_now(&d, &t);
        k = arj_mkhdr(eh, base_name(arcfull), 2, 0, ((unsigned long)d << 16) | t, 0, 0, d, t, 0);
        wr_tmp(eh, k);
    }
#elif defined FMT_ZOO
    {                                   /* Archivkopf wie Zoo 2.10       */
        static const char zt[] = "ZOO 2.10 Archive.\x1A";
        for (k = 0; k < 42; k++)
            eh[k] = 0;
        for (k = 0; zt[k]; k++)
            eh[k] = (unsigned char)zt[k];
        p32(eh + 20, 0xFDC4A7DCUL);
        p32(eh + 24, 42);
        p32(eh + 28, 0xFFFFFFD6UL);    /* -42                           */
        eh[32] = 2;
        eh[34] = 1;
        eh[41] = 3;                     /* Generationsgrenze 3           */
        wr_tmp(eh, 42);
    }
#endif
    if (old) {
        if (!o_qq)
            say("Archiv: ", arcfull, "\r\n");
        while ((r = next_entry()) > 0) {
            int keep = 1, j;
            if (!clean_name())
                ;
            if (cmd == 'D') {
                if (d_match(ename)) {
                    keep = 0;
                    ndel++;
                    if (!o_qq)
                        say("  gel\x94scht:  ", ename, "\r\n");
                }
            } else if ((j = nf_find(ename)) >= 0) {
                if (cmd == 'A' || cmd == 'M')
                    keep = 0;
                else if (((unsigned long)nfl[j].date << 16 | nfl[j].time) >
                         ((unsigned long)e_date << 16 | e_time))
                    keep = 0;
                else
                    nfl[j].flag |= 1;   /* Archiv ist aktuell            */
                if (!keep && cmd == 'F')
                    nfl[j].flag |= 4;   /* darf erneuert werden          */
            }
#ifdef FMT_ZOO
            if (keep && zoo_copy()) {
#else
            if (keep && copy_old(e_hstart, apos)) {
#endif
                twerr = 1;
                break;
            }
        }
        if (r < 0) {
            say(PNAME ": ", arcfull, ": Archiv besch\x84" "digt -- nichts ge\x84ndert\r\n");
            twerr = 2;
        }
    }
    if (!twerr && cmd != 'D')
        for (i = 0; i < nfn && !twerr; i++) {
            if (nfl[i].flag & 1)
                continue;
            if (cmd == 'F' && !(nfl[i].flag & 4))
                continue;
            if (add_one(i))
                twerr = 1;
        }
#ifdef FMT_ARJ
    eh[0] = 0x60;
    eh[1] = 0xEA;
    eh[2] = eh[3] = 0;
    wr_tmp(eh, 4);
#elif defined FMT_ZOO
    wr_tmp(eh, zoo_mkhdr(eh, "", 0, 0, 0, 0, 0, 0, 0, 0));
#else
    eh[0] = 0;
    wr_tmp(eh, 1);
#endif
    nsz = tpos;
    DosNewSize(htmp, &nsz);
    DosClose(htmp);
    if (old)
        DosClose(harc);
    if (twerr) {
        DosDelete(tname);
        if (twerr == 1)
            puts(PNAME ": Schreibfehler (Datentr\x84ger voll?) -- Archiv unver\x84ndert\r\n");
        DosExit(1);
    }
    if (old)
        DosDelete(arcfull);
    if (FAPI_FAILED(DosMove(tname, arcfull))) {
        say(PNAME ": ", tname, ": kann nicht umbenannt werden\r\n");
        DosExit(1);
    }
    if (cmd == 'M')                     /* Quellen loeschen              */
        for (i = 0; i < nfn; i++)
            if (nfl[i].flag & 2) {
                nf_full(i, full);
                DosDelete(full);
            }
    if (!o_qq) {
        put_num(cmd == 'D' ? ndel : nadd, 0, 0);
        puts(cmd == 'D' ? " Eintr\x84ge gel\x94scht\r\n" : " Datei(en) aufgenommen\r\n");
    }
    DosExit(errors ? 1 : 0);
}
