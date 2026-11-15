/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 7ZDEC.EXE - 7z-Archive auflisten, pruefen und entpacken (nur entpacken,
 * wie 7zdec aus dem LZMA SDK). Format nach 7zFormat.txt (Igor Pavlov,
 * gemeinfrei), eigene Umsetzung:
 *
 *   7ZDEC [e|x|l|t] [-l -t -o -n -j -q -d Ziel] Archiv[.7Z] [Name...]
 *   7ZDEC Datei.LZMA                (LZMA-alone, wie unlzma)
 *
 * Packarten: Copy, LZMA, LZMA2, davor BCJ (x86) oder Delta. Gepackter
 * Kopf (Vorgabe von 7-Zip) geht. Nicht: BCJ2, PPMd, BZip2, Deflate in 7z,
 * Verschluesselung, Dateien ueber 4 GB.
 *
 * Speicher: Kopf bis 128 KB, Tabellen je ein Segment (8.192 Packstroeme,
 * 4.096 Teilstroeme, 1.920 Ordner, 2.730 Dateien), das Woerterbuch so gross
 * wie noetig und moeglich (lzma.c, bis 12 x 64 KB, praktisch etwa 9 frei). Ist ein Ordner groesser
 * als der Ring, geht sein Strom erst in eine Zwischendatei (7ZDEC$$.TMP im
 * Zielverzeichnis), aus der der Entpacker weit zurueckliegende Bytes
 * nachliest; danach wird sie auf die Dateien verteilt.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"
#include "lzma.h"
#include "arcutil.h"

#pragma aux dv16 = "div bx" __parm [__dx] [__ax] [__bx] __value [__dx __ax];
unsigned long dv16(unsigned hi, unsigned lo, unsigned d);  /* DX = Rest, AX = Quotient */

static char args[FAPI_ARGLEN], arcname[FAPI_PATHLEN], arcfull[FAPI_PATHLEN];
static char ddir[FAPI_PATHLEN], wbuf[FAPI_PATHLEN], ename[FAPI_PATHLEN];
static char opath[FAPI_PATHLEN], full[FAPI_PATHLEN], tname[FAPI_PATHLEN];
static char mbuf[FAPI_ARGLEN];
static char *mfirst;
static int mode = 'x', o_o, o_n, o_j, o_q, all_yes, all_no;
static unsigned harc, errors, nsel;
static unsigned char db[2048];
static unsigned long tpos;              /* Schreibstelle Zwischendatei   */

/* ---- Archiv lesen -------------------------------------------------------- */
static unsigned w16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long w32(const unsigned char *p)
{
    return w16(p) | ((unsigned long)w16(p + 2) << 16);
}

static int seek(unsigned h, unsigned long pos)
{
    static unsigned long sp;
    sp = pos;
    return FAPI_FAILED(DosSeek(h, &sp));
}

static unsigned rdn(unsigned h, unsigned char *b, unsigned n)
{
    unsigned r = DosRead(h, b, n);
    return FAPI_FAILED(r) ? 0 : r;
}

static void say(const char *a, const char *b, const char *c)
{
    if (a) puts(a);
    if (b) puts(b);
    if (c) puts(c);
}

/* ---- Kopfpuffer ---------------------------------------------------------- */
#define HSEG 2
static unsigned hsel[HSEG], nhs;
static unsigned long hlen, hp;          /* Laenge, Lesestelle            */
static int herr;

static unsigned char __far *hptr(unsigned long pos)
{
    return MK_FP(hsel[(unsigned)(pos >> 16)], (unsigned)pos);
}

static int h_alloc(unsigned long n)
{
    unsigned s;
    while (((unsigned long)nhs << 16) < n) {
        if (nhs == HSEG)
            return 1;
        s = DosAllocSeg(0);
        if (FAPI_FAILED(s) || !s)
            return 1;
        hsel[nhs++] = s;
    }
    return 0;
}

static unsigned hb(void)
{
    if (hp >= hlen) {
        herr = 1;
        return 0;
    }
    return *hptr(hp++);
}

static unsigned long hnum(void)         /* 7z-Zahl, hoechstens 32 Bit    */
{
    unsigned f = hb(), m = 0x80, i;
    unsigned char v[8];
    for (i = 0; i < 8; i++)
        v[i] = 0;
    for (i = 0; i < 8; i++, m >>= 1) {
        if (!(f & m)) {
            v[i] = (unsigned char)(f & (m - 1));
            break;
        }
        v[i] = (unsigned char)hb();
    }
    if (v[4] | v[5] | v[6] | v[7])
        herr = 1;
    return v[0] | ((unsigned)v[1] << 8) | ((unsigned long)v[2] << 16) | ((unsigned long)v[3] << 24);
}

static unsigned long h32(void)
{
    unsigned long v = hb();
    v |= (unsigned)hb() << 8;
    v |= (unsigned long)hb() << 16;
    v |= (unsigned long)hb() << 24;
    return v;
}

/* ---- Tabellen ------------------------------------------------------------ */
struct fold {                           /* 34 Byte                       */
    unsigned long poff, psz;            /* Stelle im Archiv, Groesse     */
    unsigned long lsz, usz;             /* Groesse nach LZ, am Ende      */
    unsigned long crc;
    unsigned char kind, flt, fprop, crcdef;
    unsigned char pr[5];
    unsigned char nout, lzout, mainout; /* beim Einlesen                 */
    unsigned nsub;
};
struct file {                           /* 24 Byte                       */
    unsigned long name;                 /* Stelle im Kopf (UTF-16)       */
    unsigned long size, crc;
    unsigned date, time;
    unsigned char flags, attr;
    unsigned fold;
    unsigned rsv;
};
#define FF_STREAM 1
#define FF_DIR    2
#define FF_CRC    4
#define FF_TIME   8
#define FF_ANTI   16
#define FF_ATTR   32

#define MAXPACK 8192
#define MAXSTR  4096
#define MAXFOLD 1920
#define MAXFILE 2730
static unsigned long __far *packs;      /* Packgroessen                  */
static unsigned long __far *ssz;        /* Teilstrom: Groesse, CRC       */
static unsigned long __far *scrc;
static unsigned char __far *sdef;       /* CRC bekannt                   */
static struct fold __far *fo;
static struct file __far *fi;
static unsigned tsel[3];
static unsigned long packpos;
static unsigned npack, nfold, nstr, nfile;

static int t_alloc(void)
{
    unsigned i;
    for (i = 0; i < 3; i++) {
        if (tsel[i])
            continue;
        tsel[i] = DosAllocSeg(0);
        if (FAPI_FAILED(tsel[i]) || !tsel[i]) {
            tsel[i] = 0;
            return 1;
        }
    }
    packs = MK_FP(tsel[0], 0);
    ssz = MK_FP(tsel[0], 32768U);       /* 4.096 x 4                     */
    scrc = MK_FP(tsel[0], 49152U);
    fo = MK_FP(tsel[1], 0);
    fi = MK_FP(tsel[2], 0);
    sdef = MK_FP(tsel[2], 65520U - MAXSTR / 8 - 16);
    return 0;
}

static unsigned long bits_at;           /* Bitfeld: Stelle im Kopf       */

static int bit_at(unsigned i)
{
    return (*hptr(bits_at + (i >> 3)) >> (7 - (i & 7))) & 1;
}

static void skip_bits(unsigned n)
{
    bits_at = hp;
    hp += (n + 7) / 8;
}

/* Pruefsummen: AllDefined oder Bitfeld, dann je bekannte 4 Byte */
static void digests(unsigned n, unsigned long __far *crc, unsigned char __far *def, unsigned stride)
{
    unsigned i, all = hb();
    if (!all)
        skip_bits(n);
    for (i = 0; i < n && !herr; i++) {
        unsigned d = all || bit_at(i);
        if (def)
            def[i * stride] = (unsigned char)d;
        if (d) {
            unsigned long v = h32();
            if (crc)
                crc[i * stride] = v;
        }
    }
}

static int method(unsigned long hp0, unsigned n, struct fold __far *f, int lz)
{
    unsigned char id[4];
    unsigned i;
    for (i = 0; i < 4; i++)
        id[i] = i < n ? *hptr(hp0 + i) : 0;
    if (n == 1 && id[0] == 0x00) return lz ? 10 : -1;          /* Copy  */
    if (n == 1 && id[0] == 0x21) return lz ? LM_LZMA2 : -1;
    if (n == 3 && id[0] == 3 && id[1] == 1 && id[2] == 1) return lz ? LM_LZMA : -1;
    if (n == 4 && id[0] == 3 && id[1] == 3 && id[2] == 1 && id[3] == 3) return 21; /* BCJ */
    if (n == 1 && id[0] == 3) return 22;                        /* Delta */
    (void)f;
    return -1;
}

/* ein Ordner (kFolder) */
static void folder(struct fold __far *f, unsigned *pk)
{
    unsigned nc = (unsigned)hnum(), c, fl, ni, no, tin = 0, tout = 0, nb, i, npk;
    int m1, m2;
    unsigned long idp[2], prp[2];
    unsigned idn[2], prn[2], cin[2], cout[2];
    unsigned bin = 0, bout = 0;

    f->kind = 0xFF;
    f->flt = 0;
    f->crcdef = 0;
    f->nsub = 1;
    if (nc > 4)
        herr = 1;
    for (c = 0; c < nc && !herr; c++) {
        fl = hb();
        if (c < 2) {
            idp[c] = hp;
            idn[c] = fl & 15;
        }
        hp += fl & 15;
        ni = no = 1;
        if (fl & 0x10) {
            ni = (unsigned)hnum();
            no = (unsigned)hnum();
        }
        if (c < 2) {
            cin[c] = ni;
            cout[c] = no;
            prn[c] = 0;
        }
        if (fl & 0x20) {
            i = (unsigned)hnum();
            if (c < 2) {
                prp[c] = hp;
                prn[c] = i;
            }
            hp += i;
        }
        if (fl & 0x80)
            herr = 1;
        tin += ni;
        tout += no;
    }
    nb = tout - 1;
    for (i = 0; i < nb; i++) {
        bin = (unsigned)hnum();
        bout = (unsigned)hnum();
    }
    npk = tin - nb;
    if (npk > 1)
        for (i = 0; i < npk; i++)
            hnum();
    f->nout = (unsigned char)tout;
    if (*pk < npack) {
        unsigned long o = packpos;
        for (i = 0; i < *pk; i++)
            o += packs[i];
        f->poff = 32 + o;
        f->psz = packs[*pk];
    }
    *pk += npk;
    if (herr || tout > 2 || tin > 2)
        return;
    for (c = 0; c < nc; c++)
        if (cin[c] != 1 || cout[c] != 1)
            return;
    if (nc == 1) {
        m1 = method(idp[0], idn[0], f, 1);
        if (m1 < 0 || m1 > 10)
            return;
        f->kind = (unsigned char)m1;
        f->lzout = f->mainout = 0;
        for (i = 0; i < 5; i++)
            f->pr[i] = i < prn[0] ? *hptr(prp[0] + i) : 0;
        return;
    }
    /* zwei Coder: bin (Filter) <- bout (LZ) */
    if (bin > 1 || bout > 1 || bin == bout)
        return;
    m1 = method(idp[bout], idn[bout], f, 1);
    m2 = method(idp[bin], idn[bin], f, 0);
    if (m1 < 0 || m1 > 10 || (m2 != 21 && m2 != 22))
        return;
    f->kind = (unsigned char)m1;
    f->flt = (unsigned char)(m2 - 20);
    f->fprop = prn[bin] ? *hptr(prp[bin]) : 0;
    if (m2 == 21 && prn[bin])            /* BCJ mit Startadresse: nein   */
        return;
    f->lzout = (unsigned char)bout;
    f->mainout = (unsigned char)bin;
    for (i = 0; i < 5; i++)
        f->pr[i] = i < prn[bout] ? *hptr(prp[bout] + i) : 0;
}

static void streams_info(void)
{
    unsigned t, i, j, k, n, pk;
    unsigned long sum;

    npack = nfold = nstr = 0;
    for (;;) {
        t = hb();
        if (herr || t == 0)
            return;
        if (t == 6) {                   /* PackInfo                      */
            packpos = hnum();
            npack = (unsigned)hnum();
            if (npack > MAXPACK) {
                herr = 2;
                return;
            }
            for (;;) {
                t = hb();
                if (herr || t == 0)
                    break;
                if (t == 9)
                    for (i = 0; i < npack; i++)
                        packs[i] = hnum();
                else if (t == 10)
                    digests(npack, 0, 0, 1);
                else {
                    herr = 1;
                    return;
                }
            }
        } else if (t == 7) {            /* UnpackInfo                    */
            if (hb() != 11) {
                herr = 1;
                return;
            }
            nfold = (unsigned)hnum();
            if (nfold > MAXFOLD) {
                herr = 2;
                return;
            }
            if (hb() != 0) {            /* External                      */
                herr = 1;
                return;
            }
            for (i = 0, pk = 0; i < nfold && !herr; i++)
                folder(fo + i, &pk);
            if (hb() != 12) {
                herr = 1;
                return;
            }
            for (i = 0; i < nfold && !herr; i++)
                for (j = 0; j < fo[i].nout; j++) {
                    sum = hnum();
                    if (j == fo[i].lzout)
                        fo[i].lsz = sum;
                    if (j == fo[i].mainout)
                        fo[i].usz = sum;
                }
            for (;;) {
                t = hb();
                if (herr || t == 0)
                    break;
                if (t == 10) {
                    unsigned all = hb();
                    if (!all)
                        skip_bits(nfold);
                    for (i = 0; i < nfold; i++)
                        if (all || bit_at(i)) {
                            fo[i].crcdef = 1;
                            fo[i].crc = h32();
                        }
                } else {
                    herr = 1;
                    return;
                }
            }
            nstr = nfold;               /* ohne SubStreamsInfo je 1      */
            for (i = 0; i < nfold && i < MAXSTR; i++) {
                ssz[i] = fo[i].usz;
                scrc[i] = fo[i].crc;
                sdef[i] = fo[i].crcdef;
            }
        } else if (t == 8) {            /* SubStreamsInfo                */
            t = hb();
            if (t == 13) {
                for (i = 0; i < nfold; i++)
                    fo[i].nsub = (unsigned)hnum();
                t = hb();
            }
            for (i = 0, nstr = 0; i < nfold; i++)
                nstr += fo[i].nsub;
            if (nstr > MAXSTR) {
                herr = 2;
                return;
            }
            for (i = 0, k = 0; i < nfold; i++) {    /* Groessen          */
                sum = 0;
                for (j = 1; j < fo[i].nsub; j++) {
                    ssz[k] = t == 9 ? hnum() : 0;
                    sum += ssz[k++];
                }
                if (fo[i].nsub)
                    ssz[k++] = fo[i].usz - sum;
            }
            if (t == 9)
                t = hb();
            /* Pruefsummen: Ordner mit einem Teilstrom und bekannter CRC
               geben sie weiter, die anderen kommen hier der Reihe nach */
            for (i = 0, k = 0, n = 0; i < nfold; i++)
                if (fo[i].nsub == 1 && fo[i].crcdef) {
                    scrc[k] = fo[i].crc;
                    sdef[k++] = 1;
                } else
                    for (j = 0; j < fo[i].nsub; j++, n++)
                        sdef[k++] = 0;
            while (t != 0 && !herr) {
                if (t == 10) {
                    unsigned all = hb();
                    if (!all)
                        skip_bits(n);
                    for (i = 0, k = 0, j = 0; i < nfold; i++) {
                        if (fo[i].nsub == 1 && fo[i].crcdef) {
                            k++;
                            continue;
                        }
                        for (n = 0; n < fo[i].nsub; n++, k++, j++)
                            if (all || bit_at(j)) {
                                sdef[k] = 1;
                                scrc[k] = h32();
                            }
                    }
                } else {
                    herr = 1;
                    return;
                }
                t = hb();
            }
        } else {
            herr = 1;
            return;
        }
    }
}

/* FILETIME (100 ns seit 1601) -> FAT */
static void ftime(unsigned long lo, unsigned long hi, unsigned *d, unsigned *t)
{
    unsigned w[4], i, r;
    unsigned long q;
    static const unsigned div[2] = { 10000, 1000 };
    unsigned k;

    w[0] = (unsigned)(hi >> 16);
    w[1] = (unsigned)hi;
    w[2] = (unsigned)(lo >> 16);
    w[3] = (unsigned)lo;
    for (k = 0; k < 2; k++)             /* / 10.000.000                  */
        for (i = 0, r = 0; i < 4; i++) {
            q = dv16(r, w[i], div[k]);
            w[i] = (unsigned)q;
            r = (unsigned)(q >> 16);
        }
    /* minus 11.644.473.600 s (1601 -> 1970) = 2:B6109100h */
    lo = ((unsigned long)w[2] << 16) | w[3];
    hi = ((unsigned long)w[0] << 16) | w[1];
    if (lo < 0xB6109100UL)
        hi--;
    lo -= 0xB6109100UL;
    hi -= 2;
    if ((long)hi < 0) {
        *d = 0x21;
        *t = 0;
        return;
    }
    unix2fat(lo, d, t);
}

static void files_info(void)
{
    unsigned t, i, k, all, ext;
    unsigned long sz, end, estream = 0, efile = 0;
    unsigned ne;

    nfile = (unsigned)hnum();
    if (nfile > MAXFILE) {
        herr = 2;
        return;
    }
    for (i = 0; i < nfile; i++) {
        fi[i].flags = FF_STREAM;
        fi[i].name = 0;
        fi[i].attr = 0x20;
        fi[i].date = 0x21;
        fi[i].time = 0;
    }
    for (;;) {
        t = (unsigned)hnum();
        if (herr || t == 0)
            break;
        sz = hnum();
        end = hp + sz;
        switch (t) {
        case 0x0E:                      /* EmptyStream                   */
            estream = hp;
            for (i = 0; i < nfile; i++)
                if ((*hptr(hp + (i >> 3)) >> (7 - (i & 7))) & 1)
                    fi[i].flags = FF_DIR;   /* bis EmptyFile das Gegenteil sagt */
            break;
        case 0x0F:                      /* EmptyFile                     */
            efile = hp;
            break;
        case 0x10:                      /* Anti                          */
            for (i = 0, ne = 0; i < nfile; i++)
                if (!(fi[i].flags & FF_STREAM)) {
                    if ((*hptr(hp + (ne >> 3)) >> (7 - (ne & 7))) & 1)
                        fi[i].flags |= FF_ANTI;
                    ne++;
                }
            break;
        case 0x11:                      /* Namen                         */
            if (hb() != 0) {
                herr = 1;
                break;
            }
            for (i = 0; i < nfile && hp + 1 < end; i++) {
                fi[i].name = hp;
                while (hp + 1 < end && (*hptr(hp) | *hptr(hp + 1)))
                    hp += 2;
                hp += 2;
            }
            break;
        case 0x14:                      /* MTime                         */
        case 0x15:                      /* Attribute                     */
            all = hb();
            if (!all)
                skip_bits(nfile);
            ext = hb();
            if (ext) {
                herr = 1;
                break;
            }
            for (i = 0; i < nfile && !herr; i++)
                if (all || bit_at(i)) {
                    unsigned long lo = h32();
                    if (t == 0x14) {
                        unsigned long hi = h32();
                        unsigned dd, tt;
                        ftime(lo, hi, &dd, &tt);
                        fi[i].date = dd;
                        fi[i].time = tt;
                        fi[i].flags |= FF_TIME;
                    } else {
                        fi[i].attr = (unsigned char)lo;
                        fi[i].flags |= FF_ATTR;
                    }
                }
            break;
        }
        hp = end;
    }
    if (estream && efile)               /* leere Dateien statt Ordner    */
        for (i = 0, ne = 0; i < nfile; i++)
            if (!(fi[i].flags & FF_STREAM)) {
                if ((*hptr(efile + (ne >> 3)) >> (7 - (ne & 7))) & 1)
                    fi[i].flags &= ~FF_DIR;
                ne++;
            }
    for (i = 0; i < nfile; i++)         /* Attribut sagt Verzeichnis     */
        if ((fi[i].flags & FF_ATTR) && (fi[i].attr & 0x10) && !(fi[i].flags & FF_STREAM))
            fi[i].flags |= FF_DIR;
    /* Teilstroeme den Dateien mit Inhalt zuordnen */
    for (i = 0, k = 0, t = 0, ne = 0; i < nfile; i++) {
        if (!(fi[i].flags & FF_STREAM)) {
            fi[i].size = 0;
            continue;
        }
        while (t < nfold && ne >= fo[t].nsub) {
            t++;
            ne = 0;
        }
        if (t >= nfold || k >= nstr) {
            herr = 1;
            return;
        }
        fi[i].fold = t;
        fi[i].size = ssz[k];
        fi[i].crc = scrc[k];
        if (sdef[k])
            fi[i].flags |= FF_CRC;
        k++;
        ne++;
    }
}

/* ---- Namen --------------------------------------------------------------- */
static const unsigned char lat1[96] = {     /* A0h-FFh -> CP437, 0 = '_' */
    0xFF, 0xAD, 0x9B, 0x9C, 0, 0x9D, 0x7C, 0x15, 0, 0, 0xA6, 0xAE, 0xAA, 0x2D, 0, 0,
    0xF8, 0xF1, 0xFD, 0, 0, 0xE6, 0x14, 0xFA, 0, 0, 0xA7, 0xAF, 0xAC, 0xAB, 0, 0xA8,
    0, 0, 0, 0, 0x8E, 0x8F, 0x92, 0x80, 0, 0x90, 0, 0, 0, 0, 0, 0,
    0, 0xA5, 0, 0, 0, 0, 0x99, 0, 0, 0, 0, 0, 0x9A, 0, 0, 0xE1,
    0x85, 0xA0, 0x83, 0, 0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
    0, 0xA4, 0x95, 0xA2, 0x93, 0, 0x94, 0xF6, 0, 0x97, 0xA3, 0x96, 0x81, 0, 0, 0x98 };

static void get_name(unsigned i)
{
    unsigned long p = fi[i].name;
    unsigned k = 0, c;

    if (!p) {
        scopy(ename, "?", sizeof(ename));
        return;
    }
    while (p + 1 < hlen && k + 1 < sizeof(ename)) {
        c = *hptr(p) | ((unsigned)*hptr(p + 1) << 8);
        p += 2;
        if (!c)
            break;
        if (c >= 0xD800 && c < 0xDC00) {    /* Ersatzpaar: ein Zeichen    */
            p += 2;
            c = '_';
        } else if (c == '\\')
            c = '/';
        else if (c >= 0x80)
            c = c >= 0xA0 && c <= 0xFF && lat1[c - 0xA0] ? lat1[c - 0xA0] : '_';
        else if (c < 0x20)
            c = '_';
        ename[k++] = (char)c;
    }
    ename[k] = 0;
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

/* Name pruefen und Zielpfad bilden (full); 0 = uebergehen */
static int target(unsigned i)
{
    char *s, *q, *p;
    unsigned k;

    get_name(i);
    p = ename;
    if (p[0] && p[1] == ':')
        p += 2;
    while (*p == '/')
        p++;
    if (p != ename)
        scopy(ename, p, sizeof(ename));
    for (s = ename; *s; s = q) {
        for (q = s; *q && *q != '/'; q++)
            ;
        if (q - s == 2 && s[0] == '.' && s[1] == '.') {
            say("7zdec: ", ename, ": enth\x84lt \"..\" -- \x81" "bergangen\r\n");
            return 0;
        }
        if (*q)
            q++;
    }
    if (!ename[0] || (fi[i].flags & FF_ANTI) || !member_ok(ename))
        return 0;
    p = ename;
    if (o_j) {
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
    return 1;
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

static void set_info(unsigned i)
{
    static struct fapi_finfo f;
    f.mask = 0;
    if (fi[i].flags & FF_ATTR) {
        f.mask |= 1;
        f.attr = fi[i].attr & 0x27;
    }
    if (fi[i].flags & FF_TIME) {
        f.mask |= 2;
        f.date = fi[i].date;
        f.time = fi[i].time;
    }
    if (f.mask)
        DosSetFInfo(full, &f);
}

/* ---- Ausgabe eines Ordners ---------------------------------------------- */
static unsigned cur, nleft;             /* laufende Datei, Ordner        */
static unsigned hout = 0xFFFF;
static unsigned long oleft, ocrc;
static int ofail, oopen;

static void out_close(void)
{
    if (!oopen)
        return;
    oopen = 0;
    if (hout != 0xFFFF)
        DosClose(hout);
    get_name(cur);
    if (ofail) {
        say("7zdec: ", ename, ": Schreibfehler (Datentr\x84ger voll?)\r\n");
        errors++;
        if (hout != 0xFFFF)
            DosDelete(full);
    } else if ((fi[cur].flags & FF_CRC) && ocrc != fi[cur].crc) {
        say("7zdec: ", ename, ": Pr\x81" "fsumme falsch (CRC)\r\n");
        errors++;
        if (hout != 0xFFFF)
            DosDelete(full);
    } else if (hout != 0xFFFF)
        set_info(cur);
    else if (mode == 't' && !o_q && nsel)
        say("    OK: ", ename, "\r\n");
    hout = 0xFFFF;
}

static void out_open(void)              /* naechste Datei des Ordners    */
{
    unsigned i = cur;
    while (++i < nfile && !(fi[i].flags & FF_STREAM))
        ;
    cur = i;
    oopen = 1;
    ofail = 0;
    ocrc = 0;
    oleft = fi[cur].size;
    hout = 0xFFFF;
    nsel = 0;
    if (!target(cur))
        return;
    nsel = 1;
    if (mode != 'x')
        return;
    mkdirs(full);
    if (!FAPI_FAILED(hout = DosOpen(full))) {
        DosClose(hout);
        hout = 0xFFFF;
        if (o_n || (!o_o && !ask(full))) {
            nsel = 0;
            return;
        }
    }
    hout = DosCreate(full);
    if (FAPI_FAILED(hout)) {
        say("7zdec: ", full, ": kann nicht angelegt werden\r\n");
        errors++;
        hout = 0xFFFF;
        nsel = 0;
        return;
    }
    if (!o_q)
        say("  entpackt: ", full, "\r\n");
}

static int split(const unsigned char *p, unsigned n)
{
    unsigned k;
    while (n) {
        if (!oopen) {
            if (!nleft)
                return 0;               /* Ueberhang des Ordners         */
            nleft--;
            out_open();
        }
        k = oleft < n ? (unsigned)oleft : n;
        ocrc = lm_crc32(ocrc, p, k);
        if (hout != 0xFFFF && !ofail && DosWrite(hout, p, k) != k)
            ofail = 1;
        p += k;
        n -= k;
        oleft -= k;
        if (!oleft)
            out_close();
    }
    return 0;
}

/* Filter vor der Verteilung */
static unsigned char fb[4096 + 8], dh[256], dhp;
static unsigned fn, fst, fkind, fdist;
static unsigned long fip;

static int filt(const unsigned char *p, unsigned n)
{
    unsigned k, i;
    if (fkind == 2) {                   /* Delta: am Stueck               */
        while (n) {
            k = n > 4096 ? 4096 : n;
            for (i = 0; i < k; i++)
                fb[i] = p[i];
            delta_dec(fb, k, fdist, dh, &dhp);
            split(fb, k);
            p += k;
            n -= k;
        }
        return 0;
    }
    if (fkind != 1)
        return split(p, n);
    while (n) {
        k = 4096 - fn;
        if (k > n)
            k = n;
        for (i = 0; i < k; i++)
            fb[fn + i] = p[i];
        fn += k;
        p += k;
        n -= k;
        if (fn == 4096) {
            k = bcj_x86(fb, fn, fip, &fst);
            split(fb, k);
            fip += k;
            for (i = k; i < fn; i++)
                fb[i - k] = fb[i];
            fn -= k;
        }
    }
    return 0;
}

static void filt_end(void)
{
    unsigned k;
    if (fkind == 1 && fn) {
        k = bcj_x86(fb, fn, fip, &fst);
        (void)k;
        split(fb, fn);                  /* Rest unveraendert             */
    }
    fn = 0;
}

/* Eingabe des Ordners: Packstrom */
static unsigned long pleft;
static unsigned rd_pack(unsigned char *b, unsigned n)
{
    unsigned r;
    if (!pleft)
        return 0;
    if (n > pleft)
        n = (unsigned)pleft;
    r = rdn(harc, b, n);
    pleft -= r;
    return r;
}

/* Zwischendatei */
static unsigned htmp = 0xFFFF;
static int wr_tmp(const unsigned char *p, unsigned n)
{
    if (DosWrite(htmp, p, n) != n)
        return 1;
    tpos += n;
    return 0;
}

static int back_tmp(unsigned long pos, unsigned char *b, unsigned n)
{
    int bad = seek(htmp, pos) || rdn(htmp, b, n) != n;
    return seek(htmp, tpos) || bad;
}

static const char *lm_err(int rc)
{
    return rc == LM_EMEM ? ": zu wenig Speicher\r\n" :
           rc == LM_EPROP ? ": LZMA mit lc+lp > 4 nicht unterst\x81tzt\r\n" :
           rc == LM_EWRITE ? ": Schreibfehler (Datentr\x84ger voll?)\r\n" :
           rc == LM_EEOF ? ": Daten zu kurz\r\n" : ": Daten fehlerhaft\r\n";
}

static void do_folder(unsigned f, unsigned first)
{
    struct fold __far *F = fo + f;
    unsigned char pp[5];
    unsigned long want;
    unsigned i, n;
    int rc = 0, any = 0;

    for (i = first; i < nfile; i++)     /* etwas Gewaehltes darin?       */
        if ((fi[i].flags & FF_STREAM) && fi[i].fold == f) {
            if (target(i))
                any = 1;
        } else if (fi[i].flags & FF_STREAM)
            break;
    if (!any)
        return;
    if (F->kind == 0xFF) {
        get_name(first);
        say("7zdec: ", ename, ": Packart nicht unterst\x81tzt (BCJ2, PPMd, AES ...) -- \x81" "bergangen\r\n");
        errors++;
        return;
    }
    cur = first - 1;                    /* out_open nimmt die naechste   */
    nleft = F->nsub;
    oopen = 0;
    fkind = F->flt;
    fdist = F->fprop + 1;
    fn = fst = 0;
    fip = 0;
    dhp = 0;
    for (i = 0; i < 256; i++)
        dh[i] = 0;
    if (seek(harc, F->poff)) {
        errors++;
        return;
    }
    pleft = F->psz;
    for (i = 0; i < 5; i++)
        pp[i] = F->pr[i];
    if (F->kind == 10) {                /* Copy                          */
        unsigned long l = F->lsz;
        while (l && !rc) {
            n = rd_pack(db, l > sizeof(db) ? sizeof(db) : (unsigned)l);
            if (!n)
                rc = LM_EEOF;
            filt(db, n);
            l -= n;
        }
    } else {
        if (F->kind == LM_LZMA)
            want = w32(pp + 1);
        else
            want = F->pr[0] >= 40 ? 0xFFFFFFFFUL :
                   (unsigned long)(2 | (F->pr[0] & 1)) << (F->pr[0] / 2 + 11);
        if (want > F->lsz)
            want = F->lsz;
        if (want < 4096)
            want = 4096;
        rc = lm_ring(want);
        lm_read = rd_pack;
        lm_back = 0;
        htmp = 0xFFFF;
        if (!rc && lm_rsize < F->lsz) {     /* ueber eine Zwischendatei */
            scopy(tname, ddir[0] ? ddir : ".", sizeof(tname));
            if (tname[slen(tname) - 1] != '\\')
                scat(tname, "\\", sizeof(tname));
            scat(tname, "7ZDEC$$.TMP", sizeof(tname));
            abs_path(tname, opath);
            scopy(tname, opath, sizeof(tname));
            mkdirs(tname);
            htmp = DosCreate(tname);
            if (!FAPI_FAILED(htmp)) {
                DosClose(htmp);
                htmp = DosOpenRW(tname);
            }
            if (FAPI_FAILED(htmp)) {
                say("7zdec: ", tname, ": Zwischendatei geht nicht\r\n");
                htmp = 0xFFFF;
                rc = LM_EWRITE;
            } else {
                if (!o_q)
                    puts("  (Ordner gr\x94\xE1" "er als der Speicher: \x81" "ber eine Zwischendatei)\r\n");
                tpos = 0;
                lm_write = wr_tmp;
                lm_back = back_tmp;
            }
        } else
            lm_write = filt;
        if (!rc)
            rc = lm_decode(F->kind, pp, 5, F->lsz);
        lm_ring_free();
        if (htmp != 0xFFFF) {           /* Zwischendatei verteilen       */
            if (!rc && !seek(htmp, 0)) {
                unsigned long l = F->lsz;
                while (l) {
                    n = rdn(htmp, db, l > sizeof(db) ? sizeof(db) : (unsigned)l);
                    if (!n) {
                        rc = LM_EEOF;
                        break;
                    }
                    filt(db, n);
                    l -= n;
                }
            }
            DosClose(htmp);
            DosDelete(tname);
            htmp = 0xFFFF;
        }
    }
    if (!rc)
        filt_end();
    if (oopen) {                        /* abgebrochen: Datei weg        */
        if (!rc)
            rc = LM_EEOF;
        ofail = 0;
        if (hout != 0xFFFF) {
            DosClose(hout);
            DosDelete(full);
            hout = 0xFFFF;
        }
        oopen = 0;
    }
    if (rc) {
        get_name(first);
        say("7zdec: ", ename, lm_err(rc));
        errors++;
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

static void list(void)
{
    unsigned i, n = 0;
    unsigned long tot = 0;

    puts("     L\x84nge  Datum      Zeit   Attr  Name\r\n"
         "  --------  ---------- -----  ----  ----\r\n");
    for (i = 0; i < nfile; i++) {
        get_name(i);
        if (!member_ok(ename) || (fi[i].flags & FF_ANTI))
            continue;
        put_num(fi[i].size, 10, 0);
        puts("  ");
        put2(fi[i].date & 31, '.');
        put2((fi[i].date >> 5) & 15, '.');
        put_num(1980 + (fi[i].date >> 9), 0, 0);
        puts(" ");
        put2(fi[i].time >> 11, ':');
        put2((fi[i].time >> 5) & 63, 0);
        puts((fi[i].flags & FF_DIR) ? "  D   " : (fi[i].attr & 1) ? "  R   " : "  .   ");
        say(ename, (fi[i].flags & FF_DIR) ? "/\r\n" : "\r\n", 0);
        tot += fi[i].size;
        n++;
    }
    puts("  --------\r\n");
    put_num(tot, 10, 0);
    puts("  ");
    put_num(n, 0, 0);
    puts(" Eintr\x84ge\r\n");
}

static void extract(void)
{
    unsigned i, f;

    if (mode == 'x')                    /* zuerst Verzeichnisse, leere Dateien */
        for (i = 0; i < nfile; i++) {
            if (fi[i].flags & FF_STREAM)
                continue;
            if (!target(i))
                continue;
            if (fi[i].flags & FF_DIR) {
                if (o_j)
                    continue;
                mkdirs(full);
                DosMkDir(full);
                if (!o_q)
                    say("   angelegt: ", full, "\r\n");
            } else {
                mkdirs(full);
                if (!FAPI_FAILED(hout = DosOpen(full))) {
                    DosClose(hout);
                    if (o_n || (!o_o && !ask(full)))
                        continue;
                }
                hout = DosCreate(full);
                if (FAPI_FAILED(hout)) {
                    say("7zdec: ", full, ": kann nicht angelegt werden\r\n");
                    errors++;
                } else {
                    DosClose(hout);
                    set_info(i);
                    if (!o_q)
                        say("  entpackt: ", full, "\r\n");
                }
            }
            hout = 0xFFFF;
        }
    for (f = 0, i = 0; f < nfold; f++) {
        while (i < nfile && (!(fi[i].flags & FF_STREAM) || fi[i].fold != f))
            i++;
        if (i >= nfile)
            break;
        do_folder(f, i);
        while (i < nfile && (!(fi[i].flags & FF_STREAM) || fi[i].fold == f))
            i++;
    }
}

/* ---- Kopf ---------------------------------------------------------------- */
static unsigned long khoff;

static unsigned long hwp;
static int wr_head(const unsigned char *p, unsigned n)
{
    while (n--) {
        if (hwp >= ((unsigned long)nhs << 16))
            return 1;
        *hptr(hwp++) = *p++;
    }
    return 0;
}

static int read_header(void)
{
    unsigned char sig[32];
    unsigned long hsize, i;
    unsigned t, n;
    int rc;

    if (rdn(harc, sig, 32) != 32 || sig[0] != '7' || sig[1] != 'z' || sig[2] != 0xBC ||
        sig[3] != 0xAF || sig[4] != 0x27 || sig[5] != 0x1C)
        return 1;
    if (w32(sig + 16) || w32(sig + 24)) {
        say("7zdec: Archiv \x81" "ber 4 GB\r\n", 0, 0);
        return 2;
    }
    khoff = 32 + w32(sig + 12);
    hsize = w32(sig + 20);
    if (hsize == 0) {                   /* leeres Archiv                 */
        nfile = 0;
        return 0;
    }
    if (h_alloc(hsize)) {
        say("7zdec: Kopf zu gro\xE1\r\n", 0, 0);
        return 2;
    }
    if (seek(harc, khoff))
        return 1;
    for (i = 0; i < hsize; i += n) {
        n = hsize - i > sizeof(db) ? sizeof(db) : (unsigned)(hsize - i);
        if (rdn(harc, db, n) != n)
            return 1;
        hwp = i;
        wr_head(db, n);
    }
    hlen = hsize;
    for (;;) {
        hp = 0;
        herr = 0;
        t = hb();
        if (t == 0x17) {                /* gepackter Kopf                */
            streams_info();
            if (herr || nfold < 1 || fo[0].kind == 0xFF || fo[0].flt) {
                say("7zdec: gepackter Kopf nicht lesbar\r\n", 0, 0);
                return 2;
            }
            if (h_alloc(fo[0].lsz))
                return 2;
            if (seek(harc, fo[0].poff))
                return 1;
            pleft = fo[0].psz;
            hwp = 0;
            if (fo[0].kind == 10) {
                for (i = 0; i < fo[0].lsz; i += n) {
                    n = rd_pack(db, sizeof(db));
                    if (!n || wr_head(db, n))
                        return 1;
                }
                rc = 0;
            } else {
                rc = lm_ring(fo[0].lsz < 4096 ? 4096 : fo[0].lsz);
                if (rc || lm_rsize < fo[0].lsz) {
                    say("7zdec: Kopf zu gro\xE1\r\n", 0, 0);
                    return 2;
                }
                lm_read = rd_pack;
                lm_write = wr_head;
                lm_back = 0;
                for (n = 0; n < 5; n++)
                    sig[n] = fo[0].pr[n];
                rc = lm_decode(fo[0].kind, sig, 5, fo[0].lsz);
                lm_ring_free();
            }
            if (rc) {
                say("7zdec: Kopf", lm_err(rc), 0);
                return 2;
            }
            hlen = fo[0].lsz;
            continue;
        }
        if (t != 1)
            return 1;
        nfold = nstr = nfile = 0;
        for (;;) {
            t = hb();
            if (herr || t == 0)
                break;
            if (t == 2 || t == 3) {     /* Eigenschaften, Zusatzstroeme  */
                say("7zdec: Archiv mit Zusatzstr\x94men nicht unterst\x81tzt\r\n", 0, 0);
                return 2;
            }
            if (t == 4)
                streams_info();
            else if (t == 5)
                files_info();
            else
                herr = 1;
        }
        if (herr == 2) {
            say("7zdec: Archiv hat zu viele Eintr\x84ge\r\n", 0, 0);
            return 2;
        }
        return herr ? 1 : 0;
    }
}

/* ---- LZMA-alone ---------------------------------------------------------- */
static unsigned hlz = 0xFFFF;
static int wr_lz(const unsigned char *p, unsigned n)
{
    if (hlz != 0xFFFF && DosWrite(hlz, p, n) != n)
        return 1;
    tpos += n;
    return 0;
}

static int back_lz(unsigned long pos, unsigned char *b, unsigned n)  /* aus der Ausgabe */
{
    int bad = seek(hlz, pos) || rdn(hlz, b, n) != n;
    return seek(hlz, tpos) || bad;
}

static int lzma_alone(void)
{
    unsigned char h[13];
    unsigned long size, want;
    unsigned k;
    int rc;

    if (!seek(harc, 0) && rdn(harc, h, 13) == 13 && h[0] < 225 &&
        (h[9] == 0 || h[9] == 0xFF) && (h[10] == 0 || h[10] == 0xFF)) {
        size = w32(h + 5);
        if (w32(h + 9) && w32(h + 9) != 0xFFFFFFFFUL)
            return 1;
        if (w32(h + 9))
            size = 0xFFFFFFFFUL;        /* unbekannt: bis zur Endmarke   */
        k = slen(arcfull);
        if (k < 6 || !same(arcfull + k - 5, ".lzma"))
            return 1;
        scopy(full, arcfull, sizeof(full));
        full[k - 5] = 0;
        if (ddir[0]) {
            scopy(opath, ddir, sizeof(opath));
            if (opath[slen(opath) - 1] != '\\')
                scat(opath, "\\", sizeof(opath));
            scat(opath, base_name(full), sizeof(opath));
            abs_path(opath, full);
        }
        if (mode == 'x') {
            mkdirs(full);
            if (!FAPI_FAILED(hlz = DosOpen(full))) {
                DosClose(hlz);
                hlz = 0xFFFF;
                if (o_n || (!o_o && !ask(full)))
                    return 0;
            }
            hlz = DosCreate(full);
            if (!FAPI_FAILED(hlz)) {
                DosClose(hlz);
                hlz = DosOpenRW(full);
            }
            if (FAPI_FAILED(hlz)) {
                say("7zdec: ", full, ": kann nicht angelegt werden\r\n");
                DosExit(1);
            }
        }
        want = w32(h + 1);
        if (want > size)
            want = size;
        if (want < 4096)
            want = 4096;
        if (lm_ring(want))
            rc = LM_EMEM;
        else {
            pleft = 0xFFFFFFFFUL;
            lm_read = rd_pack;
            lm_write = wr_lz;
            lm_back = hlz != 0xFFFF ? back_lz : 0;
            tpos = 0;
            rc = lm_decode(LM_LZMA, h, 5, size);
            if (rc == LM_EBACK)
                rc = LM_EMEM;           /* pruefen: Woerterbuch zu gross */
        }
        if (hlz != 0xFFFF) {
            DosClose(hlz);
            if (rc)
                DosDelete(full);
        }
        if (rc) {
            say("7zdec: ", arcname, lm_err(rc));
            DosExit(1);
        }
        if (!o_q)
            say(mode == 'x' ? "  entpackt: " : "    OK: ", mode == 'x' ? full : arcname, "\r\n");
        return 0;
    }
    return 1;
}

static void usage(void)
{
    puts("Entpackt 7z-Archive (Copy, LZMA, LZMA2, BCJ, Delta) und .LZMA-Dateien.\r\n\r\n"
         "7ZDEC [Befehl] [Optionen] Archiv[.7Z] [Ziel\\] [Name...]\r\n\r\n"
         "  x  entpacken (Vorgabe)   e  ohne Verzeichnisse   l  zeigen   t  pr\x81" "fen\r\n"
         "  -l  Inhalt zeigen     -t  pr\x81" "fen     -d  Zielverzeichnis\r\n"
         "  -j  ohne Verzeichnisse entpacken\r\n"
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
                case 'l': case 'v': mode = 'l'; break;
                case 't': mode = 't'; break;
                case 'o': o_o = 1; break;
                case 'n': o_n = 1; break;
                case 'j': o_j = 1; break;
                case 'q': o_q = 1; break;
                case 'y': o_o = 1; break;
                case 'd': need_d = 1; break;
                case 'h': case '?': usage(); DosExit(0);
                default:
                    say("7zdec: unbekannte Option ", wbuf, " (-h zeigt die Optionen)\r\n");
                    DosExit(10);
                }
            continue;
        }
        if (npos++ == 0 && !wbuf[1] && *skip(next_word(a))) {
            k = up(wbuf[0]);
            if (k == 'E' || k == 'X' || k == 'L' || k == 'T') {
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
            scopy(ddir, wbuf, sizeof(ddir));
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
    if (!open_arc(0) && !open_arc(".7z")) {
        say("7zdec: ", arcname, ": nicht gefunden\r\n");
        DosExit(9);
    }
    if (t_alloc()) {
        say("7zdec: zu wenig Speicher\r\n", 0, 0);
        DosExit(8);
    }
    r = read_header();
    if (r == 1) {
        if (!lzma_alone()) {
            DosClose(harc);
            DosExit(0);
        }
        say("7zdec: ", arcname, ": kein 7z-Archiv oder Kopf besch\x84" "digt\r\n");
        DosExit(9);
    }
    if (r)
        DosExit(9);
    if (mode == 'l')
        list();
    else {
        if (mode == 't' || !o_q)
            say("Archiv: ", arcfull, "\r\n");
        extract();
        if (mode == 't' && !errors)
            say("Keine Fehler in ", arcfull, "\r\n");
    }
    DosClose(harc);
    DosExit(errors ? 1 : 0);
}
