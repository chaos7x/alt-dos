/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.1 - Loop-Laufwerke: eine Datei als Blockgeraet (KTEXT2)
 *
 * Stufe 1 lesend, Stufe 2 auch schreibend (docs/LOOP-DBLSPACE.md):
 * Abbilder mit 512er-Bloecken auf einem FAT-Traeger sind beschreibbar,
 * ausser mit /RO, schreibgeschuetzter Datei, ISO oder CD als Traeger.
 * Beim Einhaengen werden die
 * Plattenbereiche der Datei einmal ermittelt (ifs_bmap, Extent-Liste);
 * danach liest das Loop-Geraet direkt Sektoren des Traegers - ueber
 * blk_read/blk_write und damit ueber dessen Platten-Cache (eben
 * geschriebene Daten liegen dort oft noch verzoegert; Schreiben des Loop-
 * Geraets wird dort ebenso verzoegert). Keine verschachtelten Dateisystem-
 * aufrufe. Die Datei haelt ein Eintrag der Dateitabelle offen (kernel.c
 * loop_glue): wer sie schreiben, loeschen oder verschieben will, bekommt
 * "in Benutzung"; CHKDSK /F, DEFRAG, CONVERT sperren nur ohne offene
 * Dateien.
 *
 * Je Loop ein Kernelsegment: Kopf (struct lhdr), dahinter die Bereiche
 * zu 8 Byte (Stelle in der Datei, Sektor auf dem Traeger, beides in 512-
 * Byte-Einheiten), hoechstens 8.188. Blockgeraet: blk[].drv = DRV_LOOP,
 * blk[].unit = Platz.
 */
#include "kern.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "k2.h"

#define LH_EXT  32                      /* Bereiche ab hier              */
#define LMAXEXT ((0x10000UL - LH_EXT) / 8)
struct lhdr {
    int            dev, carrier, fk;    /* Blockgeraet, Traeger, Datei   */
    unsigned       n, last;             /* Bereiche, zuletzt getroffen   */
    unsigned char  lsh, csh;            /* 512 << lsh Loop, << csh Traeger */
    unsigned char  kind, ro;           /* LOOPK_*, schreibgeschuetzt     */
    unsigned long  units;               /* nutzbare 512er-Einheiten       */
    unsigned       dbo;                 /* DriveSpace: Stelle von struct dblh */
};

/* DoubleSpace 6.0/6.2 und DriveSpace 6.22 (CVF). Format nach der
   Beschreibung in dmsdos (doc/cvfinfo.doc, GPL, nur gelesen):
   Sektor 0 Superblock ("MSDBL6.0"/"MSDSP6.0"), BITFAT (ab Sektor 1, ein
   Bit je Datensektor, Worte mit hoechstem Bit zuerst), MDFAT (4 Byte je
   Cluster: u c hhhh llll ? s21), Bootblock (BPB des inneren Laufwerks),
   FAT, Stamm, gepackte Cluster, "MDR" im letzten Sektor. Das Loop-Geraet
   zeigt das innere FAT-Laufwerk: Sektor n < Datenbeginn liegt ungepackt
   im CVF, dahinter wird der Cluster ueber die MDFAT geholt und entpackt
   (DS/JM). Schreiben: Cluster im Puffer aendern (dirty), beim Wechsel des
   Clusters bzw. am Ende der Operation (loop_vsync) packen, freie Sektoren
   in der BITFAT suchen, schreiben, MDFAT umhaengen, alte Sektoren frei.
   FAT-Schreiben vergleicht mit dem alten Sektor: Cluster, die frei
   werden, verlieren ihren MDFAT-Eintrag und ihre Sektoren. */
/* DriveSpace 3 (Windows 95 Plus!/98, Erkennung wie dmsdos: mehr als 16
   Sektoren je Cluster oder Versionsbyte 51 = 3): 64 Sektoren je Cluster
   (32 KB), MDFAT mit 5 Byte je Eintrag, 102 je Sektor (2 Byte Rest):
   s24 | ? f | l6 << 2 (Byte 3) | h6 c u (Byte 4); f = zerstueckelter
   Cluster (erster Sektor = Liste: Anzahl, dann je Stueck 3 Byte Sektor - 1
   und Laenge - 1 << 2 im vierten Byte; gepackte Daten beginnen hinter der
   Liste, ungepackte im naechsten Sektor). Gepackt "JM" oder "SQ" (= rohes
   Deflate nach RFC 1951 hinter dem 4-Byte-Kopf). Geschrieben wird JM, nie
   zerstueckelt. Die Puffer fuer 32-KB-Cluster liegen in einem zweiten
   Segment (loop_sel2, entpackt ab 0, gepackt ab 8000h). */
struct dblh {
    unsigned long  bb, fatc, rootc, mdf; /* Bootblock, FAT, Stamm, MDFAT (CVF) */
    unsigned long  rootin, din;         /* inneres Laufwerk: Stamm, Daten */
    unsigned long  cl;                  /* Cluster im Puffer, 0 = keiner  */
    unsigned long  dst, dend, hint;     /* Datensektoren (BITFAT), Suche */
    unsigned       res, spf, mdn;       /* innen: reserviert, FAT; MDFAT-Sekt. */
    int            dcl;                 /* MDFAT-Verschiebung (signed)    */
    unsigned       ds, oo, io;          /* Puffer: Selektor, entpackt, gepackt */
    unsigned char  spc, sh, dirty, f16, jm, ver, busy, d3;
};
#define DB_T1    64U                    /* hinter dblh: FAT-Sektor neu    */
#define DB_T2    (DB_T1 + 512)          /* FAT-Sektor alt                 */
#define DB_HEAD  (DB_T2 + 512)          /* Packer: 4096 Koepfe; Entpacker
                                           SQ: Tabellen; Stueckliste     */
#define DB_PREV  (DB_HEAD + 8192)       /* Packer: Kette (Ring 8192)      */
#define DB_OUT   (DB_PREV + 16384U)     /* 6.x: Cluster entpackt          */
#define DB_IN    (DB_OUT + 8192)        /* 6.x: gepackte Daten            */
#define DB_SIZE  (DB_IN + 8192)
typedef char dblh_fits[sizeof(struct dblh) <= DB_T1 ? 1 : -1];
extern int current;
unsigned loop_sel[NLOOP], loop_sel2[NLOOP];
unsigned loop_fr[2];                    /* von loop_drop: Segmente frei geben */
#define LH(i)    ((struct lhdr __far *)MK_FP(loop_sel[i], 0))
#define EX(i, k) ((unsigned long __far *)MK_FP(loop_sel[i], LH_EXT + (k) * 8))

/* Einheit u der Datei -> Einheit auf dem Traeger, run = Einheiten bis zum
   Ende des Bereichs; 0 = ok */
static int lmap(int i, unsigned long u, unsigned long *c, unsigned long *run)
{
    struct lhdr __far *h = LH(i);
    unsigned lo, hi, m = h->last;
    unsigned long __far *e = EX(i, m);

    if (u >= h->units || !h->n)
        return -1;
    if (m >= h->n || u < e[0] || (m + 1 < h->n && u >= e[2])) {
        lo = 0;                         /* binaer: e[lo] <= u < e[hi]    */
        hi = h->n;
        while (hi - lo > 1) {
            m = (lo + hi) >> 1;
            if (EX(i, m)[0] <= u)
                lo = m;
            else
                hi = m;
        }
        m = lo;
        h->last = m;
        e = EX(i, m);
    }
    *c = e[1] + (u - e[0]);
    *run = (m + 1 < h->n ? e[2] : h->units) - u;
    return 0;
}

/* 512er-Einheit u der Datei nach DMABUF */
static int raw(int i, unsigned long u)
{
    unsigned long c, run;
    if (lmap(i, u, &c, &run))
        return -1;
    return blk_read_n(LH(i)->carrier, c, run > 4 ? 4 : (unsigned)run);
}

/* ---- Entpacker DS und JM (eigene Umsetzung nach der Beschreibung in
   dmsdos dblspace_dec.c): Bits ab dem niedrigsten, in 16-Bit-Worten ---- */
struct dz {                             /* Bitleser (auf dem Stapel)     */
    const unsigned __far *in, __far *end;
    unsigned w0, w1, p;
};

static unsigned dz_peek(struct dz *z)
{
    return z->p ? (z->w0 >> z->p) | (z->w1 << (16 - z->p)) : z->w0;
}

static void dz_eat(struct dz *z, unsigned n)
{
    z->p += n;
    if (z->p >= 16) {
        z->p -= 16;
        z->w0 = z->w1;
        z->w1 = z->in < z->end ? *z->in++ : 0;
    }
}

static unsigned dz_bits(struct dz *z, unsigned n)   /* n <= 15 */
{
    unsigned u = dz_peek(z) & ((1u << n) - 1);
    dz_eat(z, n);
    return u;
}

static unsigned dz_len(struct dz *z)    /* Laenge wie dmsdos dblb_rdlen  */
{
    unsigned u = dz_peek(z), k;
    for (k = 0; k < 9 && !((u >> k) & 1); k++)
        ;
    if (k == 9)
        return 0;                       /* ungueltig                     */
    if (k == 8) {
        dz_eat(z, 9);
        return dz_bits(z, 8) + 258;
    }
    dz_eat(z, k + 1 + k);
    return ((u >> (k + 1)) & ((1u << k) - 1)) + (1u << k) + 2;
}

static unsigned dz_offs(struct dz *z)   /* JM-Abstand                    */
{
    unsigned u = dz_peek(z);
    if (!(u & 1)) {
        dz_eat(z, 7);
        return (u >> 1) & 63;
    }
    if (!(u & 2)) {
        dz_eat(z, 10);
        return ((u >> 2) & 255) + 64;
    }
    dz_eat(z, 14);
    return ((u >> 2) & 4095) + 320;
}

/* gepackte Daten sel:in (lin Byte) nach sel:out (lout Byte); 0 = ok */
static int dz_unpack(unsigned sel, unsigned in, unsigned lin, unsigned out, unsigned lout)
{
    unsigned char __far *o = MK_FP(sel, out);
    unsigned p = 0, u, off, len, jm;
    struct dz zz, *z = &zz;

    z->in = MK_FP(sel, in);
    z->end = z->in + ((lin + 1) >> 1);
    z->p = 0;
    z->w0 = *z->in++;
    z->w1 = *z->in++;
    u = dz_peek(z);                     /* Kennung (Reihenfolge zweier
                                           Aufrufe in einem Ausdruck ist
                                           in C nicht festgelegt)        */
    dz_eat(z, 16);
    if (u == 0x4D4A)
        jm = 1;                         /* "JM": DriveSpace 6.22         */
    else if (u == 0x5344)
        jm = 0;                         /* "DS": DoubleSpace             */
    else
        return -1;
    dz_eat(z, 16);                      /* Version                       */
    while (p < lout) {
        u = dz_peek(z);
        if (jm) {
            if (!(u & 1)) {
                dz_eat(z, 8);
                o[p++] = (unsigned char)((u >> 1) & 127);
                continue;
            }
            if (u & 2) {
                dz_eat(z, 9);
                o[p++] = (unsigned char)(((u >> 2) & 127) | 128);
                continue;
            }
            dz_eat(z, 2);
            off = dz_offs(z);
        } else {
            switch (u & 3) {
            case 1:
            case 2:
                dz_eat(z, 9);
                o[p++] = (unsigned char)(((u >> 2) & 127) | ((u & 3) == 1 ? 128 : 0));
                continue;
            case 0:
                dz_eat(z, 8);
                off = (u >> 2) & 63;
                break;
            default:
                if (u & 4) {
                    dz_eat(z, 15);
                    off = ((u >> 3) & 4095) + 320;
                } else {
                    dz_eat(z, 11);
                    off = ((u >> 3) & 255) + 64;
                }
            }
        }
        if (off == 0x113F)              /* Gleichlauf (alle 512 Byte)    */
            continue;
        len = dz_len(z);
        if (!jm)
            len--;
        if (!off || !len || off > p || len > lout - p)
            return -1;
        for (; len; len--, p++)
            o[p] = o[p - off];
    }
    return 0;
}

/* ---- Packer DS/JM (Gegenstueck zum Entpacker oben, eigene Umsetzung):
   Hash-Ketten ueber 3 Byte, Treffer nie ueber eine 512-Byte-Grenze, an
   jeder Grenze und am Ende das Gleichlaufzeichen (Abstand 113Fh) ---- */
struct bw {
    unsigned __far *o;
    unsigned n, lim, w, c;
};

static void bw_put(struct bw *b, unsigned v, unsigned k)  /* k <= 15 */
{
    b->w |= v << b->c;
    b->c += k;
    if (b->c >= 16) {
        if (b->n < b->lim)
            b->o[b->n] = b->w;
        b->n++;
        b->c -= 16;
        b->w = b->c ? v >> (k - b->c) : 0;
    }
}

static void bw_off(struct bw *b, unsigned off, unsigned jm)
{
    if (jm) {
        bw_put(b, 1, 2);
        if (off < 64)
            bw_put(b, off << 1, 7);
        else if (off < 320)
            bw_put(b, 1 | ((off - 64) << 2), 10);
        else
            bw_put(b, 3 | ((off - 320) << 2), 14);
    } else if (off < 64)
        bw_put(b, off << 2, 8);
    else if (off < 320)
        bw_put(b, 3 | ((off - 64) << 3), 11);
    else
        bw_put(b, 7 | ((off - 320) << 3), 15);
}

/* len Byte ds:oo nach ds:io -> Worte, 0xFFFF = groesser als lim. Ketten
   als Ring ueber 8192 Stellen: gelesen wird nur bis 4414 zurueck */
static unsigned dz_pack(unsigned sel, unsigned base, struct dblh __far *d,
                        unsigned len, unsigned lim)
{
    unsigned char __far *s = MK_FP(d->ds, d->oo);
    unsigned __far *hd = MK_FP(sel, base + DB_HEAD);
    unsigned __far *pv = MK_FP(sel, base + DB_PREV);
    struct bw bb, *b = &bb;
    unsigned p = 0, q, m, k, best, bo, room, dep, x, jm = d->jm, ver = d->ver;

    b->o = MK_FP(d->ds, d->io);
    b->n = 0;
    b->lim = lim;
    b->w = b->c = 0;
    for (k = 0; k < 4096; k++)
        hd[k] = 0;
    bw_put(b, jm ? 0x4A : 0x44, 8);     /* "JM" bzw. "DS", Version       */
    bw_put(b, jm ? 0x4D : 0x53, 8);
    bw_put(b, 0, 8);
    bw_put(b, ver, 8);
    while (p < len) {
        if (b->n > lim)
            return 0xFFFF;
        if (p && !(p & 511))
            bw_off(b, 0x113F, jm);
        room = 512 - (p & 511);
        best = bo = 0;
        if (room >= 3) {
            m = hd[((s[p] << 4) ^ (s[p + 1] << 2) ^ s[p + 2]) & 0xFFF];
            for (dep = 16; m && dep; dep--) {
                q = m - 1;
                if (p - q > 4414)
                    break;
                for (k = 0; k < room && s[q + k] == s[p + k]; k++)
                    ;
                if (k > best) {
                    best = k;
                    bo = p - q;
                    if (k == room)
                        break;
                }
                m = pv[q & 8191];
            }
        }
        if (best >= 3) {
            bw_off(b, bo, jm);
            if (!jm)
                best++;                 /* DS zaehlt eins mehr           */
            if (best >= 258) {
                bw_put(b, 256, 9);
                bw_put(b, best - 258, 8);
            } else {
                x = best - 2;
                for (k = 0; x >> (k + 1); k++)
                    ;
                bw_put(b, 1u << k, k + 1);
                if (k)
                    bw_put(b, x - (1u << k), k);
            }
            if (!jm)
                best--;
        } else {
            k = s[p];
            if (jm)
                bw_put(b, k < 128 ? k << 1 : 3 | ((k & 127) << 2), k < 128 ? 8 : 9);
            else
                bw_put(b, (k >= 128 ? 1 : 2) | ((k & 127) << 2), 9);
            best = 1;
        }
        for (; best; best--, p++)
            if (p + 2 < len) {
                x = ((s[p] << 4) ^ (s[p + 1] << 2) ^ s[p + 2]) & 0xFFF;
                pv[p & 8191] = hd[x];
                hd[x] = p + 1;
            }
    }
    bw_off(b, 0x113F, jm);
    if (b->c)
        bw_put(b, 0, 16 - b->c);
    return b->n > lim ? 0xFFFF : b->n;
}

/* CVF-Sektor u aus DMABUF schreiben */
static int rawwr(int i, unsigned long u)
{
    unsigned long c, run;
    if (lmap(i, u, &c, &run))
        return -1;
    return blk_write(LH(i)->carrier, c);
}

/* MDFAT-Eintrag, zerlegt */
struct mde {
    unsigned long  s;                   /* erster Sektor - 1              */
    unsigned       l, h;                /* gepackt / entpackt, Sektoren - 1 */
    unsigned char  u, r, f;             /* belegt, ungepackt, zerstueckelt */
};

/* MDFAT-Eintrag des Clusters: Sektor nach DMABUF (*ms = CVF-Sektor)
   -> Offset darin, -1 Fehler */
static int md_get(int i, struct dblh __far *d, unsigned long cl, struct mde *m,
                  unsigned long *ms)
{
    unsigned long idx = cl + (long)d->dcl, e;
    unsigned o;
    unsigned char __far *p;

    if (d->d3) {
        if (idx > 0xFFFFUL)
            return -1;
        *ms = (unsigned)idx / 102;
        o = (unsigned)idx % 102 * 5;
    } else {
        *ms = idx >> 7;
        o = ((unsigned)idx & 127) << 2;
    }
    if (*ms >= d->mdn || raw(i, d->mdf + *ms))
        return -1;
    *ms += d->mdf;
    p = MK_FP(DMABUF, o);
    if (d->d3) {
        m->s = p[0] | ((unsigned)p[1] << 8) | ((unsigned long)p[2] << 16);
        m->f = (p[3] >> 1) & 1;
        m->l = p[3] >> 2;
        m->h = p[4] & 63;
        m->r = (p[4] >> 6) & 1;
        m->u = p[4] >> 7;
    } else {
        e = *(unsigned long __far *)p;
        m->s = e & 0x1FFFFFUL;
        m->f = 0;
        m->l = (unsigned)(e >> 22) & 15;
        m->h = (unsigned)(e >> 26) & 15;
        m->r = (unsigned char)(e >> 30) & 1;
        m->u = (unsigned char)(e >> 31);
    }
    return o;
}

/* Eintrag m (u = 0: alles 0) an Offset o in den MDFAT-Sektor in DMABUF */
static void md_put(struct dblh __far *d, unsigned o, struct mde *m)
{
    unsigned char __far *p = MK_FP(DMABUF, o);
    if (!m->u) {
        p[0] = p[1] = p[2] = p[3] = 0;
        if (d->d3)
            p[4] = 0;
    } else if (d->d3) {
        p[0] = (unsigned char)m->s;
        p[1] = (unsigned char)(m->s >> 8);
        p[2] = (unsigned char)(m->s >> 16);
        p[3] = (unsigned char)(m->l << 2);
        p[4] = (unsigned char)(0x80 | (m->r ? 0x40 : 0) | m->h);
    } else
        *(unsigned long __far *)p = 0x80000000UL | (m->r ? 0x40000000UL : 0) |
            ((unsigned long)m->h << 26) | ((unsigned long)m->l << 22) |
            (m->s & 0x1FFFFFUL);
}

/* n Datensektoren ab s in der BITFAT setzen (v 1) oder freigeben */
static int bf_mark(int i, struct dblh __far *d, unsigned long s, unsigned n, int v)
{
    unsigned long k, sec = 0;
    unsigned __far *w;
    unsigned m;

    for (; n; n--, s++) {
        if (s < d->dst || s > d->dend)
            continue;
        k = s - d->dst;
        if (1 + (k >> 12) != sec) {
            if (sec && rawwr(i, sec))
                return -1;
            sec = 1 + (k >> 12);
            if (raw(i, sec))
                return -1;
        }
        w = MK_FP(DMABUF, ((unsigned)(k >> 4) & 255) << 1);
        m = 0x8000 >> ((unsigned)k & 15);
        *w = v ? *w | m : *w & ~m;
    }
    return sec ? rawwr(i, sec) : 0;
}

/* n freie Datensektoren am Stueck -> erster, 0 = kein Platz */
static unsigned long bf_alloc(int i, struct dblh __far *d, unsigned n)
{
    unsigned long k, end = d->dend - d->dst + 1, start = 0, sec = 0, from;
    unsigned run, pass, w;

    from = d->hint < end ? d->hint : 0;
    for (pass = 0; pass < 2; pass++) {
        run = 0;
        for (k = pass ? 0 : from; k < end; k++) {
            if (1 + (k >> 12) != sec) {
                sec = 1 + (k >> 12);
                if (raw(i, sec))
                    return 0;
            }
            w = *(unsigned __far *)MK_FP(DMABUF, ((unsigned)(k >> 4) & 255) << 1);
            if (w == 0xFFFF && !((unsigned)k & 15)) {
                run = 0;
                k += 15;
                continue;
            }
            if (w & (0x8000 >> ((unsigned)k & 15))) {
                run = 0;
                continue;
            }
            if (!run)
                start = k;
            if (++run == n) {
                d->hint = start + n;
                return d->dst + start;
            }
        }
    }
    return 0;
}

/* Sektor - 1 aus Eintrag k der Stueckliste t (DriveSpace 3) */
static unsigned long fr_sec(unsigned char __far *t, unsigned k)
{
    return t[k * 4] | ((unsigned)t[k * 4 + 1] << 8) | ((unsigned long)t[k * 4 + 2] << 16);
}

/* Stueckliste aus DMABUF nach base + DB_HEAD -> Anzahl, 0 = ungueltig */
static unsigned fr_list(int i, unsigned base)
{
    unsigned char __far *t = MK_FP(loop_sel[i], base + DB_HEAD);
    far_copy(loop_sel[i], base + DB_HEAD, DMABUF, 0, 512);
    if (!t[0] || t[0] > 64 || t[1] || t[2] || t[3])
        return 0;
    return t[0];
}

/* Sektoren des (alten) Eintrags m in der BITFAT frei geben */
static int md_rel(int i, struct dblh __far *d, struct mde *m)
{
    unsigned base = LH(i)->dbo, fc, k;
    unsigned char __far *t = MK_FP(loop_sel[i], base + DB_HEAD);

    if (!m->u)
        return 0;
    if (m->f && raw(i, m->s + 1) == 0 && (fc = fr_list(i, base)) != 0) {
        for (k = 1; k <= fc; k++)
            if (bf_mark(i, d, fr_sec(t, k) + 1, (t[k * 4 + 3] >> 2) + 1, 0))
                return -1;
        return 0;
    }
    return bf_mark(i, d, m->s + 1, m->l + 1, 0);
}

/* Cluster cl frei: MDFAT-Eintrag 0, Sektoren in der BITFAT frei */
static int dbl_free(int i, struct dblh __far *d, unsigned long cl)
{
    struct mde m, z;
    unsigned long ms;
    int o;
    if (d->cl == cl)
        d->cl = d->dirty = 0;
    if ((o = md_get(i, d, cl, &m, &ms)) < 0)
        return -1;
    if (!m.u)
        return 0;
    z.u = 0;
    md_put(d, o, &z);
    if (rawwr(i, ms))
        return -1;
    return md_rel(i, d, &m);
}

/* geaenderten Cluster packen und schreiben; 0 = ok */
static int dbl_sync(int i, struct dblh __far *d)
{
    unsigned sel = loop_sel[i], base = LH(i)->dbo, h, l, k, ol, src;
    unsigned __far *o = MK_FP(d->ds, d->oo);
    unsigned long s, ms;
    struct mde m, n;
    int off, inpl = 0, rw;

    if (!d->dirty)
        return 0;
    for (h = d->spc; h > 1; h--) {      /* Nullsektoren am Ende weglassen */
        for (k = 0; k < 256 && !o[((h - 1) << 8) + k]; k++)
            ;
        if (k < 256)
            break;
    }
    k = h > 1 ? dz_pack(sel, base, d, h << 9, (h - 1) << 8) : 0xFFFF;
    rw = k == 0xFFFF;
    l = rw ? h : (k + 255) >> 8;
    src = rw ? d->oo : d->io;
    if ((off = md_get(i, d, d->cl, &m, &ms)) < 0)
        return -1;
    ol = m.u ? m.l + 1 : 0;
    if ((s = bf_alloc(i, d, l)) == 0) {
        if (l > ol || m.f)
            return -3;                  /* voll (IFS_FULL)               */
        s = m.s + 1;                    /* an Ort und Stelle             */
        inpl = 1;
    }
    for (k = 0; k < l; k++) {
        far_copy(DMABUF, 0, d->ds, src + (k << 9), 512);
        if (rawwr(i, s + k))
            return -1;
    }
    if (!inpl && bf_mark(i, d, s, l, 1))
        return -1;
    if (raw(i, ms))
        return -1;
    n.u = 1;
    n.r = (unsigned char)rw;
    n.f = 0;
    n.h = h - 1;
    n.l = l - 1;
    n.s = s - 1;
    md_put(d, off, &n);
    if (rawwr(i, ms))
        return -1;
    d->dirty = 0;
    if (inpl)
        return l < ol ? bf_mark(i, d, s + l, ol - l, 0) : 0;
    return md_rel(i, d, &m);
}

/* ---- Entpacker SQ (DriveSpace 3): Deflate nach RFC 1951, Decodieren
   Bit fuer Bit ueber kanonische Codes (Anzahl je Laenge + Zeichen) ---- */
struct sq {
    const unsigned char __far *in, __far *end;
    unsigned long b;
    unsigned n;
};
static const unsigned short sq_lb[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17,
    19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char sq_le[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short sq_db[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49,
    65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const unsigned char sq_ord[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4,
    12, 3, 13, 2, 14, 1, 15 };
#define SQ_LENS  0                      /* in DB_HEAD: 320 Laengen        */
#define SQ_LC    320                    /* Anzahl je Laenge (16 Worte)    */
#define SQ_LS    352                    /* Zeichen 288 Worte              */
#define SQ_DC    928
#define SQ_DS    960                    /* 32 Worte                       */

static unsigned sq_bits(struct sq *z, unsigned n)   /* n <= 16 */
{
    unsigned v;
    while (z->n < n) {
        z->b |= (unsigned long)(z->in < z->end ? *z->in++ : 0) << z->n;
        z->n += 8;
    }
    v = (unsigned)z->b & (n < 16 ? (1u << n) - 1 : 0xFFFF);
    z->b >>= n;
    z->n -= n;
    return v;
}

/* Tabelle aus n Laengen bauen; 0 = ok */
static int sq_build(unsigned short __far *cnt, unsigned short __far *sym,
                    const unsigned char __far *len, unsigned n)
{
    unsigned short offs[16];
    unsigned k;
    long left = 1;
    for (k = 0; k < 16; k++)
        cnt[k] = 0;
    for (k = 0; k < n; k++)
        cnt[len[k]]++;
    for (k = 1; k < 16; k++) {
        left = (left << 1) - cnt[k];
        if (left < 0)
            return -1;                  /* ueberbelegt                   */
    }
    offs[1] = 0;
    for (k = 1; k < 15; k++)
        offs[k + 1] = offs[k] + cnt[k];
    for (k = 0; k < n; k++)
        if (len[k])
            sym[offs[len[k]]++] = k;
    return 0;
}

static int sq_dec(struct sq *z, unsigned short __far *cnt, unsigned short __far *sym)
{
    long code = 0, first = 0, index = 0;
    int len, count;
    for (len = 1; len < 16; len++) {
        code |= sq_bits(z, 1);
        count = cnt[len];
        if (code - count < first)
            return sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* SQ-Daten in:lin (ab Kopf) nach out (lout Byte), beides in sel; Tabellen
   in ts:to; 0 = ok */
static int sq_unpack(unsigned sel, unsigned in, unsigned lin, unsigned out,
                     unsigned lout, unsigned ts, unsigned to)
{
    unsigned char __far *o = MK_FP(sel, out);
    unsigned char __far *ln = MK_FP(ts, to + SQ_LENS);
    unsigned short __far *lc = MK_FP(ts, to + SQ_LC), __far *ls = MK_FP(ts, to + SQ_LS);
    unsigned short __far *dc = MK_FP(ts, to + SQ_DC), __far *ds = MK_FP(ts, to + SQ_DS);
    unsigned p = 0, last, k, nl, nd, len, dist;
    int sym;
    struct sq zz, *z = &zz;

    z->in = MK_FP(sel, in);
    z->end = z->in + lin;
    z->b = 0;
    z->n = 0;
    if (sq_bits(z, 8) != 'S' || sq_bits(z, 8) != 'Q')
        return -1;
    sq_bits(z, 16);                     /* Version                       */
    do {
        last = sq_bits(z, 1);
        switch (sq_bits(z, 2)) {
        case 0:                         /* gespeichert                   */
            k = z->n & 7;
            z->b >>= k;
            z->n -= k;
            len = sq_bits(z, 16);
            if ((sq_bits(z, 16) ^ 0xFFFF) != len || len > lout - p)
                return -1;
            while (len--)
                o[p++] = (unsigned char)sq_bits(z, 8);
            continue;
        case 1:                         /* feste Codes                   */
            for (k = 0; k < 288; k++)
                ln[k] = k < 144 ? 8 : k < 256 ? 9 : k < 280 ? 7 : 8;
            for (k = 0; k < 30; k++)
                ln[288 + k] = 5;
            nl = 288;
            nd = 30;
            break;
        case 2:                         /* eigene Codes                  */
            nl = sq_bits(z, 5) + 257;
            nd = sq_bits(z, 5) + 1;
            len = sq_bits(z, 4) + 4;
            if (nl > 286 || nd > 30)
                return -1;
            for (k = 0; k < 19; k++)
                ln[sq_ord[k]] = k < len ? (unsigned char)sq_bits(z, 3) : 0;
            if (sq_build(lc, ls, ln, 19))
                return -1;
            for (k = 0; k < nl + nd; ) {
                if ((sym = sq_dec(z, lc, ls)) < 0)
                    return -1;
                if (sym < 16) {
                    ln[k++] = (unsigned char)sym;
                    continue;
                }
                if (sym == 16) {
                    if (!k)
                        return -1;
                    len = ln[k - 1];
                    dist = 3 + sq_bits(z, 2);
                } else {
                    len = 0;
                    dist = sym == 17 ? 3 + sq_bits(z, 3) : 11 + sq_bits(z, 7);
                }
                if (k + dist > nl + nd)
                    return -1;
                while (dist--)
                    ln[k++] = (unsigned char)len;
            }
            if (nl < 288)               /* Abstaende bei 288 erwartet    */
                for (k = nd; k--; )
                    ln[288 + k] = ln[nl + k];
            break;
        default:
            return -1;
        }
        if (sq_build(lc, ls, ln, nl) || sq_build(dc, ds, ln + 288, nd))
            return -1;
        while (p < lout) {
            if ((sym = sq_dec(z, lc, ls)) < 0)
                return -1;
            if (sym < 256) {
                o[p++] = (unsigned char)sym;
                continue;
            }
            if (sym == 256)
                break;
            sym -= 257;
            if (sym >= 29)
                return -1;
            len = sq_lb[sym] + sq_bits(z, sq_le[sym]);
            if ((sym = sq_dec(z, dc, ds)) < 0 || sym >= 30)
                return -1;
            dist = sq_db[sym] + sq_bits(z, sym < 4 ? 0 : (sym >> 1) - 1);
            if (dist > p)
                return -1;
            if (len > lout - p)
                len = lout - p;
            for (; len; len--, p++)
                o[p] = o[p - dist];
        }
    } while (!last && p < lout);
    return 0;
}

/* Cluster cl des inneren Laufwerks entpackt in den Puffer; 0 = ok */
static int dbl_load(int i, struct dblh __far *d, unsigned long cl)
{
    unsigned sel = loop_sel[i], base = LH(i)->dbo, k, n, fc = 0, st = 0, cap, lo;
    unsigned long s, ms;
    unsigned char __far *o = MK_FP(d->ds, d->oo);
    unsigned char __far *in = MK_FP(d->ds, d->io);
    unsigned char __far *t = MK_FP(sel, base + DB_HEAD);
    struct mde m;
    int rc;

    if ((rc = dbl_sync(i, d)) != 0)
        return rc;
    d->cl = 0;
    if (md_get(i, d, cl, &m, &ms) < 0)
        return -1;
    for (k = 0; k < (unsigned)d->spc << 9; k++)
        o[k] = 0;                       /* unbelegt oder kurz: Nullen    */
    if (m.u) {
        cap = d->d3 ? 0x8000 : 8192;    /* Platz fuer gepackte Daten     */
        lo = (m.h + 1) << 9;            /* entpackt                      */
        if (m.h >= d->spc || (!m.f && m.l > m.h))
            return -1;
        s = m.s + 1;                    /* erster Sektor im CVF          */
        n = 0;                          /* gelesene Byte                 */
        if (m.f) {                      /* zerstueckelt: erst die Liste  */
            if (raw(i, s) || (fc = fr_list(i, base)) == 0)
                return -1;
            if (!m.r) {                 /* gepackt: Rest des Sektors     */
                far_copy(d->ds, d->io, DMABUF, 0, 512);
                st = (fc + 1) * 4;
                n = 512;
            }
            s++;                        /* Stueck 1 ohne die Liste       */
        }
        for (k = 1; ; k++) {
            unsigned c = m.f ? (k == 1 ? m.l : (t[k * 4 + 3] >> 2) + 1) : m.l + 1;
            if (m.f && k > 1)
                s = fr_sec(t, k) + 1;
            for (; c && n < cap; c--, s++, n += 512) {
                if (raw(i, s))
                    return -1;
                far_copy(d->ds, d->io + n, DMABUF, 0, 512);
            }
            if (!m.f || k >= fc)
                break;
        }
        if (m.r)                        /* ungepackt                     */
            far_copy(d->ds, d->oo, d->ds, d->io, n < lo ? n : lo);
        else if (in[st] == 'S' && in[st + 1] == 'Q') {
            if (sq_unpack(d->ds, d->io + st, n - st, d->oo, lo, sel, base + DB_HEAD))
                return -1;
        } else {
            if (dz_unpack(d->ds, d->io + st, n - st, d->oo, lo))
                return -1;
            if (!d->d3 && (in[st] == 'J' || !d->jm)) {
                d->jm = in[st] == 'J';  /* JM gesehen: so schreiben     */
                d->ver = in[st + 3];
            }
        }
    }
    d->cl = cl;
    return 0;
}

/* DS6: Kopf des ersten gepackten Clusters lesen (die ersten 32 MDFAT-
   Sektoren) -> jm/ver. Die Kennung im Superblock sagt es nicht sicher:
   von MS-DOS 6.22 umgewandelte oder angelegte Container tragen teils
   "MSDBL6.0", packen aber JM; DoubleSpace-Daten ("DS") in einem solchen
   Container liest MS-DOS 6.22 nur mit DRVSPACE.MR1. */
static void dbl_probe(int i, struct dblh __far *d)
{
    unsigned k, o;
    unsigned long e;
    unsigned char __far *b = MK_FP(DMABUF, 0);

    for (k = 0; k < d->mdn && k < 32; k++) {
        if (raw(i, d->mdf + k))
            return;
        for (o = 0; o < 512; o += 4) {
            e = *(unsigned long __far *)(b + o);
            if ((e & 0xC0000000UL) != 0x80000000UL)
                continue;               /* frei oder ungepackt           */
            if (raw(i, (e & 0x1FFFFFUL) + 1))
                return;
            if ((b[0] == 'J' && b[1] == 'M') || (b[0] == 'D' && b[1] == 'S')) {
                d->jm = b[0] == 'J';
                d->ver = b[3];
            }
            return;
        }
    }
}

/* FAT-Sektor fl aus DMABUF schreiben: frei gewordene Cluster abgeben */
static int dbl_fatw(int i, struct dblh __far *d, unsigned fl)
{
    unsigned sel = loop_sel[i], base = LH(i)->dbo, j, e, eo, en, bo, k;
    unsigned char __far *nb = MK_FP(sel, base + DB_T1);
    unsigned char __far *ob = MK_FP(sel, base + DB_T2);
    unsigned char pb = 0, xb = 0, c[4];

    far_copy(sel, base + DB_T1, DMABUF, 0, 512);
    if (raw(i, d->fatc + fl))
        return -1;
    far_copy(sel, base + DB_T2, DMABUF, 0, 512);
    if (d->f16) {
        for (j = 0; j < 256; j++) {
            e = (fl << 8) + j;
            if (e >= 2 && (ob[2 * j] | ob[2 * j + 1]) && !(nb[2 * j] | nb[2 * j + 1]) &&
                dbl_free(i, d, e))
                return -1;
        }
    } else {                            /* FAT12: Eintraege ueber die    */
        if (fl && raw(i, d->fatc + fl - 1) == 0)    /* Sektorgrenze      */
            pb = *(unsigned char __far *)MK_FP(DMABUF, 511);
        if (fl + 1 < d->spf && raw(i, d->fatc + fl + 1) == 0)
            xb = *(unsigned char __far *)MK_FP(DMABUF, 0);
        for (e = fl ? fl * 1024 / 3 - 1 : 2; ; e++) {
            bo = (e * 3 >> 1) + 1;              /* +1: pb vorn           */
            if (bo < fl * 512)
                continue;
            bo -= fl * 512;
            if (bo > 512)
                break;
            for (k = 0; k < 2; k++) {
                j = bo + k;
                c[k] = j == 0 ? pb : j == 513 ? xb : ob[j - 1];
                c[k + 2] = j == 0 ? pb : j == 513 ? xb : nb[j - 1];
            }
            eo = c[0] | (c[1] << 8);
            en = c[2] | (c[3] << 8);
            if (e & 1) {
                eo >>= 4;
                en >>= 4;
            }
            if (e >= 2 && (eo & 0xFFF) && !(en & 0xFFF) && dbl_free(i, d, e))
                return -1;
        }
    }
    far_copy(DMABUF, 0, sel, base + DB_T1, 512);
    return rawwr(i, d->fatc + fl);
}

/* innerer Sektor lba nach DMABUF (wr 0) bzw. aus DMABUF (wr 1) */
static int dbl_io(int i, unsigned long lba, int wr)
{
    struct dblh __far *d = MK_FP(loop_sel[i], LH(i)->dbo);
    unsigned base = LH(i)->dbo, off;
    unsigned long rel, cl, fl;
    int rc;

    if (wr == 2)
        return dbl_sync(i, d);

    if (lba < d->res)                   /* Bootblock, reserviert         */
        return wr ? rawwr(i, d->bb + lba) : raw(i, d->bb + lba);
    if (lba < d->rootin) {              /* FAT (nur eine; Kopie 2 = 1)   */
        fl = lba - d->res;
        while (fl >= d->spf)
            if (wr)
                return 0;
            else
                fl -= d->spf;
        return wr ? dbl_fatw(i, d, (unsigned)fl) : raw(i, d->fatc + fl);
    }
    if (lba < d->din) {                 /* Stamm                         */
        lba = d->rootc + (lba - d->rootin);
        return wr ? rawwr(i, lba) : raw(i, lba);
    }
    rel = lba - d->din;
    cl = (rel >> d->sh) + 2;
    off = d->oo + (((unsigned)rel & (d->spc - 1)) << 9);
    if (wr)
        far_copy(loop_sel[i], base + DB_T1, DMABUF, 0, 512);
    if (d->cl != cl && (rc = dbl_load(i, d, cl)) != 0)
        return rc;                      /* -3: Container voll            */
    if (!wr) {
        far_copy(DMABUF, 0, d->ds, off, 512);
        return 0;
    }
    far_copy(d->ds, off, loop_sel[i], base + DB_T1, 512);
    d->dirty = 1;
    return 0;
}

static int dbl_call(int i, unsigned long lba, int wr)
{
    struct dblh __far *d = MK_FP(loop_sel[i], LH(i)->dbo);
    int rc;
    d->busy = 1;
    rc = dbl_io(i, lba, wr);
    d->busy = 0;
    return rc;
}

/* Ende einer Operation auf Laufwerk x (kernel.c, mit dessen Sperre):
   gepufferten Cluster schreiben */
void __far loop_vsync(int x)
{
    int dev, i;
    if (x < 0 || x >= NDRIVE || !vol[x].used)
        return;
    dev = vol[x].blk;
    i = blk[dev].unit;
    if (blk[dev].drv == DRV_LOOP && i < NLOOP && loop_sel[i] && LH(i)->kind == LOOPK_DBL)
        dbl_call(i, 0, 2);
}

/* blk_flush: alle, die gerade niemand bearbeitet */
void __far loop_sync_all(void)
{
    int i, x;
    struct dblh __far *d;
    for (i = 0; i < NLOOP; i++) {
        if (!loop_sel[i] || LH(i)->kind != LOOPK_DBL)
            continue;
        d = MK_FP(loop_sel[i], LH(i)->dbo);
        for (x = 0; x < NDRIVE && !(vol[x].used && vol[x].blk == LH(i)->dev); x++)
            ;
        if (d->dirty && !d->busy)       /* 5.x: alles unter klock        */
            dbl_call(i, 0, 2);
    }
}

/* Block lba des Loop-Geraets nach DMABUF (Offset 0); 0 = ok */
int __far loop_read(int dev, unsigned long lba)
{
    int i = blk[dev].unit, cd;
    struct lhdr __far *h;
    unsigned long u, c, run;
    unsigned k, n;

    if (i >= NLOOP || !loop_sel[i])
        return -1;
    h = LH(i);
    if (h->kind == LOOPK_DBL)
        return dbl_call(i, lba, 0);
    cd = h->carrier;
    n = 1 << h->lsh;                    /* Einheiten je Loop-Block       */
    u = lba << h->lsh;
    if (lmap(i, u, &c, &run))
        return -1;
    if (h->csh) {                       /* Traeger CD (2048)             */
        if (n == 4)
            return (c & 3) ? -1 : blk_read(cd, c >> 2);
        if (blk_read(cd, c >> 2))
            return -1;
        if (c & 3)
            far_copy(DMABUF, 0, DMABUF, (unsigned)(c & 3) << 9, 512);
        return 0;
    }
    if (blk_read_n(cd, c, run > 4 ? 4 : (unsigned)run))    /* auch voraus */
        return -1;
    if (n == 1)
        return 0;
    /* 2048er-Block (ISO auf einer Platte) aus vier Sektoren, die ueber
       Bereichsgrenzen gehen koennen: von hinten nach vorn, jeder Sektor
       kommt bei 0 an und wird an seine Stelle kopiert */
    for (k = n; --k > 0; ) {
        if (lmap(i, u + k, &c, &run) || blk_read(cd, c))
            return -1;
        far_copy(DMABUF, k << 9, DMABUF, 0, 512);
    }
    return lmap(i, u, &c, &run) || blk_read(cd, c) ? -1 : 0;
}

/* DMABUF (Offset 0) in Block lba des Loop-Geraets; 0 = ok, -2 schreib-
   geschuetzt. Beschreibbar sind nur 512er-Geraete auf 512er-Traegern. */
int __far loop_write(int dev, unsigned long lba)
{
    int i = blk[dev].unit;
    unsigned long c, run;

    if (i >= NLOOP || !loop_sel[i])
        return -1;
    if (LH(i)->ro)
        return -2;
    if (LH(i)->kind == LOOPK_DBL)
        return dbl_call(i, lba, 1);
    if (lmap(i, lba, &c, &run))
        return -1;
    return blk_write(LH(i)->carrier, c);
}

/* Datei (offen in files[fk], f) auf Laufwerk d als Loop-Geraet anmelden
   -> Blockgeraet oder FAPI-Fehler (negativ). Eingehaengt wird danach in
   kernel.c (loop_glue). */
int __far loop_attach(int fk, struct ifs_file __far *f, int d, int ro)
{
    struct volume *v = &vol[d];
    struct lhdr __far *h;
    int i, dev, n, dbl;
    unsigned sz;
    unsigned char __far *b;
    unsigned long units, e1[2];

    for (i = 0; i < NLOOP && loop_sel[i]; i++)
        ;
    for (dev = 1; dev < nblk && !(blk[dev].bsize == 0 && blk[dev].drv == NDRV); dev++)
        ;
    if (i == NLOOP || dev == NBLK)
        return (int)FAPI_ERR_FULL;
    if (f->attr & ATTR_DIR)
        return (int)FAPI_ERROR;
    if (v->drv == &fat_driver)
        n = ifs_bmap(f, 0, 0, 0);       /* erst zaehlen                  */
    else if (v->drv == &mod_driver && blk[v->blk].bsize == 2048)
        n = 1;                          /* ISO9660.IFS: am Stueck ab fsdata */
    else
        return (int)FAPI_ERR_DENIED;
    if (n < 0)
        return (int)FAPI_ERROR;
    dbl = 0;                            /* DoubleSpace/DriveSpace-CVF?   */
    if (v->drv == &fat_driver && n > 0 && ifs_bmap(f, KDATA, (unsigned)e1, 1) > 0 &&
        blk_read(v->blk, e1[1]) == 0) {
        b = MK_FP(DMABUF, 0);
        if (b[3] == 'M' && b[4] == 'S' && b[5] == 'D' && b[8] == '6' && b[9] == '.' &&
            ((b[6] == 'B' && b[7] == 'L') || (b[6] == 'S' && b[7] == 'P')))
            dbl = b[13] > 16 || b[51] == 3 ? 2 : 1;     /* 2: DriveSpace 3 */
    }
    sz = dbl == 2 ? DB_OUT : dbl ? DB_SIZE : 0;    /* DS3: Puffer im 2. Segment */
    if ((unsigned)n > (dbl ? LMAXEXT - (sz + 64) / 8 : LMAXEXT))
        return (int)FAPI_ERR_FRAG;
    if ((loop_sel[i] = kseg_alloc(LH_EXT + (unsigned long)n * 8 + (dbl ? sz + 16 : 0))) == 0)
        return (int)FAPI_ERR_FULL;
    loop_sel2[i] = 0;
    h = LH(i);
    h->dev = dev;
    h->carrier = v->blk;
    h->fk = fk;
    h->n = n;
    h->last = 0;
    h->lsh = 0;
    h->csh = blk[v->blk].bsize == 2048 ? 2 : 0;
    h->ro = ro || h->csh || (!dbl && (f->attr & ATTR_RO));
                                        /* CVF: MS-DOS/Windows legen sie
                                           mit R an - nur /RO gilt       */
    h->units = units = f->size >> 9;
    if (v->drv == &fat_driver)
        ifs_bmap(f, loop_sel[i], LH_EXT, n);
    else {
        EX(i, 0)[0] = 0;
        EX(i, 0)[1] = f->fsdata << 2;
    }
    if (dev == nblk)
        nblk++;
    blk[dev].drv = DRV_LOOP;
    blk[dev].unit = (unsigned char)i;
    blk[dev].bsize = 512;
    blk[dev].blocks = units;
    blk[dev].rem = 0;
    b = MK_FP(DMABUF, 0);
    if (dbl) {                          /* inneres Laufwerk einrichten   */
        struct dblh __far *db;
        unsigned k, spc;
        h->dbo = (LH_EXT + n * 8 + 15) & ~15;
        h->kind = LOOPK_DBL;
        db = MK_FP(loop_sel[i], h->dbo);
        if (raw(i, 0))
            goto bad;
        db->d3 = dbl == 2;
        db->jm = db->d3 || b[51] == 2;  /* DS3 oder Version 2 (DriveSpace
                                           6.22): JM; Version 0 (Double-
                                           Space 6.0/6.2) DS - die Kennung
                                           MSDBL/MSDSP sagt nichts: 6.20
                                           schreibt MSDSP6.0, 6.22
                                           MSDBL6.0                      */
        db->ver = db->jm ? 0 : 2;       /* bis ein Cluster es verraet    */
        if (db->d3) {
            if ((loop_sel2[i] = kseg_alloc(0x10000UL)) == 0)
                goto bad;
            db->ds = loop_sel2[i];
            db->oo = 0;
            db->io = 0x8000;
        } else {
            db->ds = loop_sel[i];
            db->oo = h->dbo + DB_OUT;
            db->io = h->dbo + DB_IN;
        }
        db->dirty = db->busy = 0;
        db->hint = 0;
        db->dst = *(unsigned __far *)(b + 39) + (unsigned long)*(unsigned __far *)(b + 43) + 2;
        db->bb = *(unsigned __far *)(b + 39);
        db->fatc = db->bb + *(unsigned __far *)(b + 14);
        db->rootc = db->bb + *(unsigned __far *)(b + 41);
        db->mdf = *(unsigned __far *)(b + 36) + 1UL;
        db->dcl = *(int __far *)(b + 45);
        db->cl = 0;
        db->mdn = (unsigned)(db->bb - db->mdf);   /* MDFAT-Sektoren */
        if (!db->d3)                    /* Packart am ersten gepackten   */
            dbl_probe(i, db);           /* Cluster (MS-DOS 6.22 behaelt
                                           oft "MSDBL6.0", packt aber JM) */
        db->dend = 0;                   /* "MDR" im letzten Sektor       */
        if (raw(i, units - 1) == 0 && b[0] == 'M' && b[1] == 'D' && b[2] == 'R')
            db->dend = units - 2;
        else if (raw(i, units - 2) == 0 && b[0] == 'M' && b[1] == 'D' && b[2] == 'R')
            db->dend = units - 3;
        if (db->dend > db->dst + ((db->mdf - 1) << 12) - 1)
            db->dend = db->dst + ((db->mdf - 1) << 12) - 1;   /* BITFAT  */
        if (db->dend < db->dst)
            h->ro = 1;                  /* Ende nicht gefunden: lesen    */
        if (raw(i, db->bb) || b[510] != 0x55 || b[511] != 0xAA ||
            *(unsigned __far *)(b + 11) != 512)
            goto bad;
        spc = b[13];
        for (k = 0; k < 7 && (1u << k) != spc; k++)
            ;
        if (k == 7 || !b[16] || (!db->d3 && k > 4))
            goto bad;
        db->spc = (unsigned char)spc;
        db->sh = (unsigned char)k;
        db->res = *(unsigned __far *)(b + 14);
        db->spf = *(unsigned __far *)(b + 22);
        db->rootin = db->res +
                     (unsigned long)(b[16] * *(unsigned __far *)(b + 22));
        db->din = db->rootin + (*(unsigned __far *)(b + 17) >> 4);
        units = *(unsigned __far *)(b + 19);
        if (!units)
            units = *(unsigned long __far *)(b + 32);
        blk[dev].blocks = units;
        db->f16 = ((units - db->din) >> k) >= 4085;
        if (!h->ro)
            f->flags |= IFS_F_WRITE;
        return dev;
bad:
        loop_drop(dev);
        return (int)FAPI_ERR_DENIED;
    }
    h->kind = LOOPK_IMG;
    if (units > 64 && blk_read(dev, 64) == 0 && b[1] == 'C' && b[2] == 'D' &&
        b[3] == '0' && b[4] == '0' && b[5] == '1') {
        h->kind = LOOPK_ISO;            /* Deskriptor bei 8000h          */
        h->ro = 1;
        h->lsh = 2;
        blk[dev].bsize = 2048;
        blk[dev].blocks = units >> 2;
    } else if (units > 1 && blk_read(dev, 0) == 0 && b[0] == 'c' && b[1] == 'o' &&
               b[2] == 'n' && b[3] == 'e' && b[7] == 'x') {
        loop_drop(dev);                 /* dynamische VHD (Kopf vorn)    */
        return (int)FAPI_ERR_DENIED;
    } else if (units > 1 && blk_read(dev, units - 1) == 0 && b[0] == 'c' &&
               b[1] == 'o' && b[2] == 'n' && b[3] == 'e' && b[7] == 'x') {
        if (b[63] != 2) {               /* Typ (big endian): 2 = fest    */
            loop_drop(dev);
            return (int)FAPI_ERR_DENIED;
        }
        h->kind = LOOPK_VHD;
        h->units = --units;             /* Fussteil gehoert nicht dazu   */
        blk[dev].blocks = units;
    }
    if (h->kind != LOOPK_ISO && blk_read(dev, 0) == 0 &&
        !((b[0] == 0xEB || b[0] == 0xE9) && b[11] == 0 && b[12] == 2) &&
        b[510] == 0x55 && b[511] == 0xAA)
        h->kind = LOOPK_DISK;           /* Partitionstabelle, kein BPB   */
    if (!h->ro)                         /* beschreibbar: niemand sonst   */
        f->flags |= IFS_F_WRITE;        /* darf die Datei oeffnen        */
    return dev;
}

/* Loop-Geraet abmelden (Laufwerke sind schon weg) -> Eintrag der Datei-
   tabelle, den kernel.c schliesst, oder -1. Das Kernelsegment gibt
   kernel.c frei (loop_fr). */
int __far loop_drop(int dev)
{
    int i = blk[dev].unit, fk;
    if (blk[dev].drv != DRV_LOOP || i >= NLOOP || !loop_sel[i])
        return -1;
    if (LH(i)->kind == LOOPK_DBL)
        dbl_call(i, 0, 2);              /* gepufferten Cluster schreiben */
    fk = LH(i)->fk;
    loop_fr[0] = loop_sel[i];
    loop_fr[1] = loop_sel2[i];
    loop_sel[i] = loop_sel2[i] = 0;
    blk[dev].drv = NDRV;                /* Luecke, wie DEVLOAD /U        */
    blk[dev].bsize = 0;
    blk[dev].blocks = 0;
    blk[dev].unit = 0;
    return fk;
}

/* Platz i fuer LOOP_INFO -> Eintrag der Dateitabelle, -1 frei, -2 Ende */
int __far loop_info(unsigned i, struct fapi_loopinfo __far *li)
{
    struct lhdr __far *h;
    int x;
    if (i >= NLOOP)
        return -2;
    li->drives = 0;
    if (!loop_sel[i])
        return -1;
    h = LH(i);
    for (x = 0; x < NDRIVE; x++)
        if ((vol[x].used || vol[x].present) && vol[x].blk == h->dev)
            li->drives |= 1UL << x;
    li->blocks = blk[h->dev].blocks;
    li->bsize = blk[h->dev].bsize;
    li->extents = h->n;
    li->kind = h->kind | (h->ro ? LOOPK_RO : 0);
    return h->fk;
}
