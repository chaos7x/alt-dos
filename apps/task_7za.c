/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 7ZA.EXE - 7z-Archive anlegen (eigene Umsetzung nach 7zFormat.txt von
 * Igor Pavlov, gemeinfrei; Packkern lzmaenc.c):
 *
 *   7ZA a [-r] [-mx=0..9] [-q] [-y] Archiv[.7Z] [Datei...]
 *   7ZA x|e|l|t ...       -> an 7ZDEC.EXE weitergereicht
 *
 * Ein Ordner fuer alle Dateien (solid), 32-Bit-Programme (PE/LE/LX/ELF) in
 * einem zweiten mit BCJ x86 davor, Methode LZMA (lc 3, lp 0, pb 2,
 * Woerterbuch 128 KB), -mx=0 speichert (Copy). Kopf ungepackt, Namen in
 * UTF-16 (aus CP437), Zeit (FILETIME) und Attribute. Leere Dateien als
 * leere Stroeme, Verzeichnisse werden nicht abgelegt. Ein vorhandenes
 * Archiv wird (noch) nicht ergaenzt.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"
#include "lzmaenc.h"
#include "arcutil.h"
#include "lzma.h"                      /* bcj_x86 (bcj.c)               */

#pragma aux mul16 = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];
unsigned long mul16(unsigned a, unsigned b);

static char args[FAPI_ARGLEN], arcname[FAPI_PATHLEN], arcfull[FAPI_PATHLEN];
static char wbuf[FAPI_PATHLEN], full[FAPI_PATHLEN], aname[FAPI_PATHLEN];
static char dirp[FAPI_PATHLEN], pat[FAPI_PATHLEN], abase[FAPI_PATHLEN];
static char tree[FAPI_PATHLEN], pbuf[FAPI_PATHLEN], mbuf[FAPI_ARGLEN];
static int o_r, o_q, o_y, level = 5;
static unsigned harc = 0xFFFF, fbase, errors;
static unsigned char db[4096];
static int werr;

static const unsigned short cp437u[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

static void say(const char *a, const char *b, const char *c)
{
    if (a) puts(a);
    if (b) puts(b);
    if (c) puts(c);
}

/* ---- Liste der Dateien (fern) -------------------------------------------- */
struct nf {
    unsigned long size, crc;
    unsigned date, time, attr;
    unsigned po;
    unsigned char ps, flag;             /* flag 1: weglassen, 2: BCJ      */
};
#define NF_MAX 3640
#define NPS    4
static struct nf __far *nfl;
static unsigned nfsel, nfn, psel[NPS], npsel, pofs;

static char __far *pool(unsigned s, unsigned o)
{
    return MK_FP(psel[s], o);
}

static int nf_add(const char *fl, const char *an, struct fapi_dirent *e)
{
    unsigned l1 = slen(fl) + 1, l2 = slen(an) + 1, i;
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
    nfl[nfn].crc = 0;
    nfl[nfn].date = e->date;
    nfl[nfn].time = e->time;
    nfl[nfn].attr = e->attr;
    nfl[nfn].ps = (unsigned char)(npsel - 1);
    nfl[nfn].po = pofs;
    nfl[nfn].flag = 0;
    p = pool(npsel - 1, pofs);
    for (i = 0; i < l1; i++)
        p[i] = fl[i];
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

static char __far *nf_aname(unsigned i)
{
    char __far *p = pool(nfl[i].ps, nfl[i].po);
    while (*p)
        p++;
    return p + 1;
}

static int nf_find(const char *n)
{
    unsigned i, k;
    char __far *p;
    for (i = 0; i < nfn; i++) {
        p = nf_aname(i);
        for (k = 0; p[k] && up(p[k]) == up(n[k]); k++)
            ;
        if (!p[k] && !n[k])
            return (int)i;
    }
    return -1;
}

/* ---- Dateien sammeln (wie LHA/ARJ) ---------------------------------------- */
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

static int collect(const char *fl, struct fapi_dirent *e)
{
    if (same(fl, arcfull) && slen(fl) == slen(arcfull))
        return 0;
    make_aname(fl);
    if (nf_find(aname) >= 0)
        return 0;
    if (nf_add(fl, aname, e)) {
        puts("7za: zu viele Dateien\r\n");
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
    if (is_dir(dirp)) {
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
        say("7za: ", arg, ": nicht gefunden\r\n");
}

/* ---- CRC-32 -------------------------------------------------------------- */
static unsigned long ct[256];

static void crc_init(void)
{
    unsigned i, k;
    unsigned long c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0xEDB88320UL : c >> 1;
        ct[i] = c;
    }
}

static unsigned long crc32(unsigned long c, const unsigned char *p, unsigned n)
{
    c = ~c;
    while (n--)
        c = ct[(unsigned char)c ^ *p++] ^ (c >> 8);
    return ~c;
}

/* ---- Ausgabe ------------------------------------------------------------- */
static unsigned long apos;              /* Schreibstelle im Archiv       */

static int wr_arc(const unsigned char *p, unsigned n)
{
    if (werr || DosWrite(harc, p, n) != n) {
        werr = 1;
        return 1;
    }
    apos += n;
    return 0;
}

static unsigned char hb[2048];
static unsigned hn;
static unsigned long hcrc, hsize;

static void hflush(void)
{
    if (hn) {
        hcrc = crc32(hcrc, hb, hn);
        wr_arc(hb, hn);
        hn = 0;
    }
}

static void hw(unsigned c)
{
    hb[hn++] = (unsigned char)c;
    hsize++;
    if (hn == sizeof(hb))
        hflush();
}

static void hw32(unsigned long v)
{
    hw((unsigned)v & 0xFF);
    hw((unsigned)(v >> 8) & 0xFF);
    hw((unsigned)(v >> 16) & 0xFF);
    hw((unsigned)(v >> 24));
}

static void wnum(unsigned long v)       /* 7z-Zahl (bis 32 Bit)          */
{
    unsigned i, first = 0, mask = 0x80;
    for (i = 0; i < 4; i++) {
        if (v < (1UL << (7 * (i + 1)))) {
            first |= (unsigned)(v >> (8 * i));
            break;
        }
        first |= mask;
        mask >>= 1;
    }
    hw(first);
    while (i) {                         /* niedrige Bytes zuerst         */
        hw((unsigned)v & 0xFF);
        v >>= 8;
        i--;
    }
}

/* FAT-Zeit -> FILETIME (100 ns seit 1601) = Unix * 10^7 + 116444736000000000 */
static void filetime(unsigned date, unsigned time, unsigned *w)
{
    unsigned long u = fat2unix(date, time), p;
    unsigned a0 = (unsigned)u, a1 = (unsigned)(u >> 16), k0 = 0x9680, k1 = 0x0098, i;
    static const unsigned add[4] = { 0x8000, 0xD53E, 0xB1DE, 0x019D };
    unsigned long s;
    w[0] = w[1] = w[2] = w[3] = 0;
    p = mul16(a0, k0);
    w[0] = (unsigned)p;
    w[1] = (unsigned)(p >> 16);
    p = mul16(a0, k1) + w[1];
    w[1] = (unsigned)p;
    w[2] = (unsigned)(p >> 16);
    p = mul16(a1, k0) + w[1];
    w[1] = (unsigned)p;
    p = (p >> 16) + w[2] + mul16(a1, k1);
    w[2] = (unsigned)p;
    w[3] = (unsigned)(p >> 16);
    s = 0;
    for (i = 0; i < 4; i++) {
        s += (unsigned long)w[i] + add[i];
        w[i] = (unsigned)s;
        s >>= 16;
    }
}

static unsigned uni(unsigned char c)
{
    return c < 0x80 ? c : cp437u[c - 0x80];
}

/* ---- Packen -------------------------------------------------------------- */
/* Zwei Ordner: 0 alles andere (LZMA bzw. Copy), 1 32-Bit-Programme mit BCJ
 * davor. In jedem Ordner liegen die Dateien in der Reihenfolge von ord[]. */
static unsigned long total, tot[2], pk[2];
static unsigned nds[2], fkind[2], nfold, ord[NF_MAX];

/* 32-Bit-Code (PE, LE/LX von DOS/4GW, OS/2 2.x, ELF)? Nur nach Endung
 * geoeffnet - BCJ hilft 16-Bit-Programmen nicht. */
static int is_x32(unsigned i)
{
    static const char *ext[] = { "EXE", "DLL", "DRV", "SYS", "OCX", "CPL",
                                 "SCR", "VXD", "386", "OVL", "BIN", 0 };
    static unsigned char b[64];
    static unsigned long sp;
    char __far *a = nf_aname(i), __far *e = 0;
    unsigned h, k, j, r = 0;
    for (; *a; a++)
        if (*a == '.')
            e = a + 1;
        else if (*a == '/')
            e = 0;
    if (!e)
        return 0;
    for (k = 0; ext[k]; k++) {
        for (j = 0; e[j] && ext[k][j] && up(e[j]) == ext[k][j]; j++)
            ;
        if (!e[j] && !ext[k][j])
            break;
    }
    if (!ext[k])
        return 0;
    nf_full(i, full);
    h = DosOpen(full);
    if (FAPI_FAILED(h))
        return 0;
    if (DosRead(h, b, 64) == 64) {
        if (b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')
            r = 1;
        else if (b[0] == 'M' && b[1] == 'Z') {
            sp = b[60] | ((unsigned long)b[61] << 8) | ((unsigned long)b[62] << 16) |
                 ((unsigned long)b[63] << 24);
            if (sp >= 64 && sp + 2 < nfl[i].size && !FAPI_FAILED(DosSeek(h, &sp)) &&
                DosRead(h, b, 2) == 2 &&
                ((b[0] == 'P' && b[1] == 'E') || (b[0] == 'L' && (b[1] == 'E' || b[1] == 'X'))))
                r = 1;
        }
    }
    DosClose(h);
    return r;
}

static void classify(void)
{
    unsigned i, p, n = 0;
    if (level)
        for (i = 0; i < nfn; i++)
            if (nfl[i].size >= 64 && is_x32(i))
                nfl[i].flag |= 2;
    for (p = 0; p < 2; p++)
        for (i = 0; i < nfn; i++)
            if ((nfl[i].flag & 2) == p * 2)
                ord[n++] = i;
}

static unsigned char bb[4096];          /* BCJ-Puffer                     */
static unsigned bn, bst;
static unsigned long bip;

static void feed(int bcj, const unsigned char *p, unsigned n)
{
    unsigned k, m;
    if (!level) {
        wr_arc(p, n);
        return;
    }
    if (!bcj) {
        lme_feed(p, n);
        return;
    }
    while (n) {
        k = sizeof(bb) - bn;
        if (k > n)
            k = n;
        for (m = 0; m < k; m++)
            bb[bn + m] = p[m];
        bn += k;
        p += k;
        n -= k;
        if (bn == sizeof(bb)) {
            k = bcj_x86(bb, bn, bip, &bst, 1);
            lme_feed(bb, k);
            bip += k;
            for (m = k; m < bn; m++)
                bb[m - k] = bb[m];
            bn -= k;
        }
    }
}

static int pack_all(void)
{
    unsigned i, j, h, n, p, any;
    unsigned long got, crc, start;
    for (p = 0; p < 2 && !werr; p++) {
        for (j = any = 0; j < nfn; j++)
            if ((nfl[ord[j]].flag & 3) == p * 2 && nfl[ord[j]].size)
                any = 1;
        if (!any)
            continue;
        fkind[nfold] = p;
        start = apos;
        tot[nfold] = 0;
        bn = bst = 0;
        bip = 0;
        if (level)
            lme_begin(level);
        for (j = 0; j < nfn && !werr; j++) {
            i = ord[j];
            if ((nfl[i].flag & 3) != p * 2 || !nfl[i].size)
                continue;
            nf_full(i, full);
            h = DosOpen(full);
            if (FAPI_FAILED(h)) {
                say("7za: ", full, ": kann nicht gelesen werden\r\n");
                errors++;
                nfl[i].flag |= 1;
                continue;
            }
            got = 0;
            crc = 0;
            while ((n = DosRead(h, db, sizeof(db))) != 0 && !FAPI_FAILED(n)) {
                crc = crc32(crc, db, n);
                got += n;
                feed(p, db, n);
            }
            DosClose(h);
            nfl[i].size = got;
            nfl[i].crc = crc;
            tot[nfold] += got;
            if (!o_q) {
                char __far *a = nf_aname(i);
                unsigned k;
                puts(p ? "  gepackt:  (BCJ) " : "  gepackt:  ");
                for (k = 0; a[k] && k + 1 < sizeof(pbuf); k++)
                    pbuf[k] = a[k];
                pbuf[k] = 0;
                puts(pbuf);
                puts("\r\n");
            }
        }
        if (level) {
            if (p && bn) {              /* Rest: was geht umrechnen      */
                bcj_x86(bb, bn, bip, &bst, 1);
                lme_feed(bb, bn);
            }
            lme_end();
        }
        pk[nfold] = apos - start;
        total += tot[nfold];
        nfold++;
    }
    return werr;
}

static void coder_lzma(void)
{
    unsigned char pr[5];
    unsigned i;
    if (level) {
        hw(0x23);
        hw(0x03); hw(0x01); hw(0x01);
        wnum(5);
        lme_props(pr);
        for (i = 0; i < 5; i++)
            hw(pr[i]);
    } else {
        hw(0x01);
        hw(0x00);
    }
}

/* Datei i liegt in Ordner f (nur fuer Dateien mit Daten) */
static unsigned in_fold(unsigned i)
{
    return nfold == 2 ? (nfl[i].flag & 2) >> 1 : 0;
}

static void write_header(void)
{
    unsigned i, j, k, f, nd = 0, ne = 0, nfiles = 0, w[4], more = 0;
    unsigned long nlen = 0;
    unsigned char bits;
    char __far *a;

    nds[0] = nds[1] = 0;
    for (i = 0; i < nfn; i++) {
        if (nfl[i].flag & 1)
            continue;
        nfiles++;
        if (nfl[i].size) {
            nd++;
            nds[in_fold(i)]++;
        } else
            ne++;
        for (a = nf_aname(i), k = 0; a[k]; k++)
            ;
        nlen += 2UL * (k + 1);
    }
    hw(0x01);                           /* Header                        */
    if (nd) {
        hw(0x04);                       /* MainStreamsInfo               */
        hw(0x06);                       /* PackInfo                      */
        wnum(0);
        wnum(nfold);
        hw(0x09);
        for (f = 0; f < nfold; f++)
            wnum(pk[f]);
        hw(0x00);
        hw(0x07);                       /* UnPackInfo                    */
        hw(0x0B);
        wnum(nfold);
        hw(0x00);
        for (f = 0; f < nfold; f++) {
            if (fkind[f]) {             /* BCJ <- LZMA                   */
                wnum(2);
                hw(0x04);
                hw(0x03); hw(0x03); hw(0x01); hw(0x03);
                coder_lzma();
                wnum(0);                /* Bindepaar: Eingang 0 <- Ausgang 1 */
                wnum(1);
            } else {
                wnum(1);
                coder_lzma();
            }
        }
        hw(0x0C);
        for (f = 0; f < nfold; f++) {
            wnum(tot[f]);
            if (fkind[f])
                wnum(tot[f]);
        }
        hw(0x00);
        hw(0x08);                       /* SubStreamsInfo                */
        hw(0x0D);
        for (f = 0; f < nfold; f++) {
            wnum(nds[f]);
            if (nds[f] > 1)
                more = 1;
        }
        if (more) {
            hw(0x09);
            for (f = 0; f < nfold; f++)
                for (j = 0, k = 0; j < nfn; j++) {
                    i = ord[j];
                    if (!(nfl[i].flag & 1) && nfl[i].size && in_fold(i) == f &&
                        ++k < nds[f])
                        wnum(nfl[i].size);
                }
        }
        hw(0x0A);
        hw(0x01);
        for (f = 0; f < nfold; f++)
            for (j = 0; j < nfn; j++) {
                i = ord[j];
                if (!(nfl[i].flag & 1) && nfl[i].size && in_fold(i) == f)
                    hw32(nfl[i].crc);
            }
        hw(0x00);
        hw(0x00);
    }
    hw(0x05);                           /* FilesInfo                     */
    wnum(nfiles);
    if (ne) {
        hw(0x0E);                       /* EmptyStream                   */
        wnum((nfiles + 7) / 8);
        for (j = 0, k = 0, bits = 0; j < nfn; j++) {
            i = ord[j];
            if (nfl[i].flag & 1)
                continue;
            if (!nfl[i].size)
                bits |= (unsigned char)(0x80 >> (k & 7));
            if ((++k & 7) == 0) {
                hw(bits);
                bits = 0;
            }
        }
        if (k & 7)
            hw(bits);
        hw(0x0F);                       /* EmptyFile: alle sind Dateien  */
        wnum((ne + 7) / 8);
        for (k = 0; k < ne; k += 8)
            hw(ne - k >= 8 ? 0xFF : (0xFF00 >> (ne - k)) & 0xFF);
    }
    hw(0x11);                           /* Name                          */
    wnum(nlen + 1);
    hw(0x00);
    for (j = 0; j < nfn; j++) {
        i = ord[j];
        if (nfl[i].flag & 1)
            continue;
        for (a = nf_aname(i); *a; a++) {
            unsigned u = uni((unsigned char)*a);
            hw(u & 0xFF);
            hw(u >> 8);
        }
        hw(0);
        hw(0);
    }
    hw(0x14);                           /* MTime                         */
    wnum(2 + 8UL * nfiles);
    hw(0x01);
    hw(0x00);
    for (j = 0; j < nfn; j++) {
        i = ord[j];
        if (nfl[i].flag & 1)
            continue;
        filetime(nfl[i].date, nfl[i].time, w);
        for (k = 0; k < 4; k++) {
            hw(w[k] & 0xFF);
            hw(w[k] >> 8);
        }
    }
    hw(0x15);                           /* Attribute                     */
    wnum(2 + 4UL * nfiles);
    hw(0x01);
    hw(0x00);
    for (j = 0; j < nfn; j++) {
        i = ord[j];
        if (!(nfl[i].flag & 1))
            hw32(nfl[i].attr & 0x27);
    }
    hw(0x00);
    hw(0x00);
    hflush();
}

static int seek(unsigned long p)
{
    static unsigned long sp;
    sp = p;
    return FAPI_FAILED(DosSeek(harc, &sp));
}

static void pusage(void)
{
    puts("Legt 7z-Archive an (LZMA, 32-Bit-Programme mit BCJ).\r\n\r\n"
         "7ZA a [-r] [-mx=0..9] [-q] [-y] Archiv[.7Z] [Datei...]\r\n"
         "7ZA x|e|l|t ...     wie 7ZDEC (entpacken, zeigen, pruefen)\r\n\r\n"
         "  -r   Unterverzeichnisse   -mx=N  Stufe (0 speichern, 5 Vorgabe, 9 beste)\r\n"
         "  -q   still                -y     vorhandenes Archiv ersetzen\r\n");
}

/* x/e/l/t: 7ZDEC.EXE aus dem PATH (oder neben 7ZA) mit denselben Angaben */
static void run_dec(const char *rest)
{
    static char pv[256], cmd[FAPI_ARGLEN + FAPI_PATHLEN];
    unsigned i = 0, k, h;
    if (FAPI_FAILED(DosGetEnv("PATH", pv, sizeof(pv))))
        pv[0] = 0;
    for (;;) {
        k = 0;
        while (pv[i] && pv[i] != ';' && k < FAPI_PATHLEN - 16)
            cmd[k++] = pv[i++];
        cmd[k] = 0;
        if (k && cmd[k - 1] != '\\')
            scat(cmd, "\\", sizeof(cmd));
        scat(cmd, "7ZDEC.EXE", sizeof(cmd));
        h = DosOpen(cmd);
        if (!FAPI_FAILED(h)) {
            DosClose(h);
            break;
        }
        if (!pv[i]) {
            puts("7za: 7ZDEC.EXE nicht gefunden\r\n");
            DosExit(3);
        }
        i++;
    }
    scat(cmd, " ", sizeof(cmd));
    scat(cmd, rest, sizeof(cmd));
    if (DosExecPgm(cmd, EXEC_SYNC)) {
        puts("7za: 7ZDEC kann nicht gestartet werden\r\n");
        DosExit(3);
    }
    DosExit(DosQChildRC());
}

void main(void)
{
    char *a, *w;
    unsigned k, ml = 0;
    unsigned long packed, asize;
    unsigned char sh[32];

    DosGetArgs(args);
    a = skip(args);
    if (!*a) {
        pusage();
        DosExit(10);
    }
    word(a, wbuf, sizeof(wbuf));
    k = up(wbuf[0]);
    if (wbuf[0] && !wbuf[1] && (k == 'X' || k == 'E' || k == 'L' || k == 'T'))
        run_dec(a);
    if (k != 'A' || wbuf[1]) {
        pusage();
        DosExit(wbuf[0] == '/' || wbuf[0] == '-' ? 0 : 10);
    }
    for (a = next_word(a); *a; a = next_word(a)) {
        word(a, wbuf, sizeof(wbuf));
        w = wbuf;
        if (w[0] == '-' && w[1]) {
            k = up(w[1]);
            if (k == 'R' && !w[2])
                o_r = 1;
            else if (k == 'Q' && !w[2])
                o_q = 1;
            else if (k == 'Y' && !w[2])
                o_y = 1;
            else if (k == 'M' && up(w[2]) == 'X') {
                w += 3;
                if (*w == '=')
                    w++;
                if (*w >= '0' && *w <= '9' && !w[1])
                    level = *w - '0';
                else
                    level = 5;
            } else {
                say("7za: unbekannte Option ", wbuf, "\r\n");
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
    if (!arcname[0]) {
        pusage();
        DosExit(10);
    }
    abs_path(arcname, arcfull);
    w = base_name(arcfull);
    for (k = 0; w[k] && w[k] != '.'; k++)
        ;
    if (!w[k])
        scat(arcfull, ".7z", sizeof(arcfull));
    harc = DosOpen(arcfull);
    if (!FAPI_FAILED(harc)) {
        DosClose(harc);
        if (!o_y) {
            say("7za: ", arcfull, ": gibt es schon (Erg\x84nzen geht noch nicht, -y ersetzt)\r\n");
            DosExit(9);
        }
        DosDelete(arcfull);
    }
    nfsel = DosAllocSeg(0);
    if (FAPI_FAILED(nfsel) || !nfsel || (level && lme_open())) {
        puts("7za: zu wenig Speicher\r\n");
        DosExit(8);
    }
    nfl = MK_FP(nfsel, 0);
    crc_init();
    if (!mbuf[0])
        scopy(mbuf, "*.*", sizeof(mbuf));
    for (a = mbuf; *a; a = next_word(a)) {
        word(a, pbuf, sizeof(pbuf));
        add_arg(pbuf);
    }
    if (!nfn) {
        puts("7za: keine Dateien\r\n");
        DosExit(1);
    }
    harc = DosCreate(arcfull);
    if (FAPI_FAILED(harc)) {
        say("7za: ", arcfull, ": kann nicht angelegt werden\r\n");
        DosExit(1);
    }
    for (k = 0; k < 32; k++)
        sh[k] = 0;
    wr_arc(sh, 32);
    if (!o_q)
        say("Archiv: ", arcfull, "\r\n");
    if (level)
        lme_write = wr_arc;
    classify();
    pack_all();
    packed = apos - 32;
    hcrc = 0;
    hsize = 0;
    write_header();
    asize = apos;
    if (!werr) {                        /* Startkopf                     */
        sh[0] = '7'; sh[1] = 'z'; sh[2] = 0xBC; sh[3] = 0xAF; sh[4] = 0x27; sh[5] = 0x1C;
        sh[6] = 0; sh[7] = 4;
        sh[12] = (unsigned char)packed;
        sh[13] = (unsigned char)(packed >> 8);
        sh[14] = (unsigned char)(packed >> 16);
        sh[15] = (unsigned char)(packed >> 24);
        sh[20] = (unsigned char)hsize;
        sh[21] = (unsigned char)(hsize >> 8);
        sh[22] = (unsigned char)(hsize >> 16);
        sh[23] = (unsigned char)(hsize >> 24);
        sh[28] = (unsigned char)hcrc;
        sh[29] = (unsigned char)(hcrc >> 8);
        sh[30] = (unsigned char)(hcrc >> 16);
        sh[31] = (unsigned char)(hcrc >> 24);
        packed = crc32(0, sh + 12, 20);
        sh[8] = (unsigned char)packed;
        sh[9] = (unsigned char)(packed >> 8);
        sh[10] = (unsigned char)(packed >> 16);
        sh[11] = (unsigned char)(packed >> 24);
        if (seek(0))
            werr = 1;
        else
            wr_arc(sh, 32);
    }
    DosClose(harc);
    if (werr) {
        DosDelete(arcfull);
        puts("7za: Schreibfehler (Datentr\x84ger voll?) -- kein Archiv\r\n");
        DosExit(1);
    }
    if (!o_q) {
        put_num(nfn, 0, 0);
        puts(" Datei(en), ");
        put_num(total, 0, 0);
        puts(" -> ");
        put_num(asize, 0, 0);
        puts(" Byte\r\n");
    }
    DosExit(errors ? 1 : 0);
}
