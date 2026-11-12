/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AWELOAD.EXE - Klangbank (SoundFont) fuer den Wavetable der AWE32/AWE64
 *
 *   AWELOAD [Datei]   Bank laden; ohne Datei X:\DOS\SYSTEM\SYNTHGM.SBK
 *                     (X = Startlaufwerk). SoundFont 1 (.SBK, Creative)
 *                     und SoundFont 2 (.SF2).
 *   AWELOAD /U        Bank entfernen: MIDI wieder an den MPU-401
 *
 * Wie sfxload unter Linux: das Programm fuehrt Presets, Instrumente und
 * Samples zu Zonen zusammen und rechnet die Parameter in Werte fuer den
 * EMU8000 um; SB16.SYS spielt damit MIDI. Samples der Bank (nicht die
 * ROM-Klaenge) kommen ins RAM der Karte.
 *
 * Herkunft: Ablauf (Zusammenfuehren der Ebenen, SBK -> SF2 -> EMU8000)
 * und die ganzzahligen Umrechnungen ("ADIP"-Fassungen, urspruenglich aus
 * Creatives AWE32 Developer Information Pack) nach awesfx von Takashi
 * Iwai (awelib/parsesf.c, sbkconv.c, awe_parm.c; GPL-2.0-or-later), auf
 * 16 Bit ohne Gleitkomma umgeschrieben (Logarithmen ueber eine Tabelle).
 * Die SoundFont-Dateien selbst (SYNTHGM.SBK usw.) gehoeren Creative/E-mu
 * und liegen den Treiberdisketten der Karte bei - nicht AT-DOS.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

/* ---- Rechnen ohne Laufzeitbibliothek ------------------------------- */
long smul(int a, int b);
#pragma aux smul = "imul dx" __parm [__ax] [__dx] __value [__dx __ax];
int muldiv(int a, int b, int c);    /* a * b / c, Ergebnis passt in 16 Bit */
#pragma aux muldiv = "imul dx" "idiv bx" __parm [__ax] [__dx] [__bx] __value [__ax] __modify [__dx];
int mulhi(int a, int b);            /* (a * b) >> 16                      */
#pragma aux mulhi = "imul dx" __parm [__ax] [__dx] __value [__dx] __modify [__ax];
unsigned muldivu(unsigned a, unsigned b, unsigned c);
#pragma aux muldivu = "mul dx" "div bx" __parm [__ax] [__dx] [__bx] __value [__ax] __modify [__dx];
unsigned divrem(unsigned long a, unsigned b);   /* a / b, Quotient < 65536 */
#pragma aux divrem = "div bx" __parm [__dx __ax] [__bx] __value [__ax] __modify [__dx];

/* 1200 * log2(1 + i/64) */
static const short lgt[65] = {
    0,27,53,79,105,130,155,180,204,228,251,275,298,320,342,365,386,408,429,450,
    471,491,512,532,551,571,590,609,628,647,666,684,702,720,738,755,773,790,807,
    824,841,857,874,890,906,922,938,953,969,984,999,1015,1030,1044,1059,1074,1088,
    1103,1117,1131,1145,1159,1173,1186,1200 };

/* 1200 * log2(x) (Cent), x >= 1 */
static int ilg(unsigned long x)
{
    int b = 31;
    unsigned i, f;
    if (!x)
        return 0;
    while (!(x & 0x80000000UL)) {
        x <<= 1;
        b--;
    }
    i = (unsigned)(x >> 25) & 63;
    f = (unsigned)(x >> 17) & 0xFF;
    return b * 1200 + lgt[i] + (int)(((lgt[i + 1] - lgt[i]) * f) >> 8);
}

/* ---- Generatoren --------------------------------------------------- */
#define NGEN 60
enum { G_KEYR = 43, G_VELR = 44, G_INST = 41, G_SAMP = 53 };
/* Kopierregel: 0 addieren, 1 ueberschreiben, 2 Bereich, 3 nur Preset,
   4 nur Instrument */
static const unsigned char gcopy[NGEN] = {
    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0, 0,3,0,2,2,0,1,1,0,0, 0,0,0,4,1,1,0,1,1,0 };
/* Art fuer SBK -> SF2 */
enum { T_NO, T_PSH, T_CUT, T_Q, T_CSH, T_TREM, T_TEN, T_PAN, T_TIME, T_FREQ,
       T_MSUS, T_VSUS, T_TMK, T_ATT, T_SCALE };
static const unsigned char gtype[NGEN] = {
    T_NO, T_NO, T_NO, T_NO, T_NO, T_PSH, T_PSH, T_PSH, T_CUT, T_Q,
    T_CSH, T_CSH, T_NO, T_TREM, T_NO, T_TEN, T_TEN, T_PAN, T_NO, T_NO,
    T_NO, T_TIME, T_FREQ, T_TIME, T_FREQ, T_TIME, T_TIME, T_TIME, T_TIME, T_MSUS,
    T_TIME, T_TMK, T_TMK, T_TIME, T_TIME, T_TIME, T_TIME, T_VSUS, T_TIME, T_TMK,
    T_TMK, T_NO, T_NO, T_NO, T_NO, T_NO, T_NO, T_NO, T_ATT, T_NO,
    T_NO, T_NO, T_NO, T_NO, T_NO, T_NO, T_SCALE, T_NO, T_NO, T_NO };

struct tbl {
    int val[NGEN];
    unsigned char set[NGEN];
};
static struct tbl pt, ct;
static int sfver;                       /* 1 = SBK, 2 = SF2             */

static int defv(int g)
{
    switch (g) {
    case 8: return 13500;
    case 21: case 23: case 25: case 26: case 27: case 28: case 30:
    case 33: case 34: case 35: case 36: case 38: return -12000;
    case 43: case 44: return 0x7F00;
    case 46: case 47: case 58: return -1;
    case 56: return 100;
    case 29: case 37: return sfver == 1 ? 1000 : 0;
    case 22: return sfver == 1 ? -725 : 0;
    case 24: return sfver == 1 ? -15600 : 0;
    }
    return 0;
}

static int sbk2sf2(int g, int v)
{
    switch (gtype[g]) {
    case T_CUT: return v >= 127 ? 14400 : 59 * v + 4366;
    case T_Q: return v * 3 / 2;
    case T_TEN: return muldiv(v, 1000, 256);
    case T_PAN: return muldiv(v, 1000, 127) - 500;
    case T_ATT:
        if (v <= 0)
            return 1000;
        return muldiv(ilg(127) - ilg((unsigned)v), 5017, 10000); /* -2000 log10 */
    case T_SCALE: return v ? 50 : 100;
    case T_TIME: return ilg(v > 0 ? (unsigned)v : 1) - 11959;  /* ilg(1000) */
    case T_TMK: return muldiv(v, 111, 20);
    case T_FREQ:
        if (v <= 0)
            return g == 22 ? -725 : -15600;
        return ilg((unsigned)v) - 7925;
    case T_PSH: return (muldiv(v, 1200, 64) + 1) / 2;
    case T_CSH: return muldiv(v, g == 10 ? 3600 : 7200, 64);
    case T_TREM: return muldiv(v, 120, 64);
    case T_MSUS: return v < 96 ? muldiv(96 - v, 1000, 96) : 0;
    case T_VSUS: return v < 96 ? (2000 - 21 * v) / 2 : 0;
    }
    return v;
}

/* Preset-Tabelle (src) in die Instrument-Tabelle (dst) einrechnen */
static void merge(struct tbl *d, struct tbl *s)
{
    int g, lo, hi;
    for (g = 0; g < NGEN; g++) {
        if (!s->set[g])
            continue;
        if (sfver == 1) {
            if (!d->set[g] || g == G_KEYR || g == G_VELR)
                d->val[g] = s->val[g];
        } else switch (gcopy[g]) {
        case 0: d->val[g] += s->val[g]; break;
        case 1: d->val[g] = s->val[g]; break;
        case 2:
            if (!d->set[g])
                d->val[g] = s->val[g];
            else {
                lo = d->val[g] & 0xFF;
                hi = (d->val[g] >> 8) & 0xFF;
                if ((s->val[g] & 0xFF) > lo) lo = s->val[g] & 0xFF;
                if (((s->val[g] >> 8) & 0xFF) < hi) hi = (s->val[g] >> 8) & 0xFF;
                d->val[g] = lo | (hi << 8);
            }
            break;
        default:
            if (!d->set[g])
                d->val[g] = s->val[g];
        }
        d->set[g] = 1;
    }
}

/* ---- EMU8000-Werte (ADIP) ------------------------------------------ */
/* 0x10000 | Bruchteil von q/1200, geschoben um 16 - ganzzahliger Teil */
static unsigned adip_exp(int q, int bias)
{
    unsigned hi, fr;
    unsigned long t;
    q += bias;
    if (q < 0)
        return 0;
    hi = (unsigned)q / 1200;
    fr = muldivu((unsigned)q % 1200, 32768u, 600);
    if (hi > 16)
        return 0xFFFF;
    t = (0x10000UL | fr) >> (16 - hi);
    return t > 0xFFFF ? 0xFFFF : (unsigned)t;
}

static unsigned c_delay(int tc)
{
    long d;
    if (tc <= -12000)
        return 0x8000;
    d = 0x8000L - adip_exp(tc, 0x30E4);
    if (d < -32768L)
        d = -32768L;
    return (unsigned)d;
}

static unsigned c_attack(int tc)
{
    int a, t1, t2;
    if (tc >= 4300)
        return 1;
    if (tc > -600) {
        a = (tc + 500) / 150;
        t1 = ((~a) & 7) | 8;
        t2 = a < 0 ? -1 : 0;
        a += t2 & 7;
        a >>= 3;
        t1 >>= a;
        return t1 < 1 ? 1 : t1;
    }
    a = (37 - tc) / 75 + 8;
    return a >= 128 ? 127 : a;
}

static unsigned c_hold(int tc)
{
    int h;
    if (tc < -5368)
        return 127;
    h = 127 - (int)adip_exp(tc, 4130);
    return h < 0 ? 0 : h;
}

static unsigned c_decay(int tc)
{
    int d, t1;
    if (tc > 1800) {
        d = (tc - 1800) / 150;
        t1 = ((~d) & 7) | 8;
        t1 >>= d >> 3;
        return t1 < 1 ? 1 : t1;
    }
    d = (37 - tc) / 75 + 39;
    return d >= 128 ? 127 : d;
}

static unsigned c_sus(int a)        /* Lautstaerke cB / Modulation 0,1 % */
{
    int v = 127 - muldiv(a, 127, 1000);
    return v < 0 ? 0 : v > 127 ? 127 : v;
}

static unsigned c_cut(int a)
{
    int c = (a + 15) / 29 - 0x99;
    return c < 0 ? 0 : c > 255 ? 255 : c;
}

static unsigned char sb8(int v)     /* auf -128..127 begrenzt, als Byte */
{
    if (v < -128) v = -128;
    if (v > 127) v = 127;
    return (unsigned char)v;
}

static unsigned c_freq(int ac)
{
    unsigned t;
    if (ac <= -16000)
        return 0;
    t = adip_exp(ac, 0x23A6);
    return t > 255 ? 255 : t;
}

static unsigned c_att(int a)
{
    int t = (a + 12) / 24;
    t = t * 8 / 3;
    return t < 0 ? 0 : t > 255 ? 255 : t;
}

static unsigned c_send(int v)       /* Chorus/Hall 0,1 % -> 0-255 */
{
    if (v < 0) return 0;
    if (v > 1000) return 255;
    return muldiv(v, 255, 1000);
}

#define WORD(h, l) (((unsigned)(unsigned char)(h) << 8) | (unsigned char)(l))

/* ---- Datei ---------------------------------------------------------- */
static unsigned fh;
static char rbuf[1024];

static int rd(unsigned long pos, void *buf, unsigned n)
{
    unsigned long p = pos;
    DosSeek(fh, &p);
    return DosRead(fh, buf, n) == n ? 0 : -1;
}

static unsigned long rd32(unsigned long pos)
{
    unsigned long v = 0;
    rd(pos, &v, 4);
    return v;
}

/* Teil der Datei in ein eigenes Segment (DosRead liest nur ins DS) */
static unsigned load_seg(unsigned long pos, unsigned long len)
{
    unsigned sel, o, k;
    if (len > 0xFFF0UL)
        return 0;
    sel = DosAllocSeg((unsigned)len + 16);
    if (FAPI_FAILED(sel))
        return 0;
    for (o = 0; o < (unsigned)len; o += k) {
        k = (unsigned)len - o > sizeof(rbuf) ? sizeof(rbuf) : (unsigned)len - o;
        if (rd(pos + o, rbuf, k))
            return 0;
        {
            unsigned i;
            unsigned char __far *d = MK_FP(sel, o);
            for (i = 0; i < k; i++)
                d[i] = rbuf[i];
        }
    }
    return sel;
}

/* Abschnitte der Bank */
enum { C_PHDR, C_PBAG, C_PGEN, C_INST, C_IBAG, C_IGEN, C_SHDR, NCH };
static const char ckid[NCH][5] = { "phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr" };
static unsigned csel[NCH];
static unsigned cnum[NCH];
static const unsigned char csize[NCH] = { 38, 4, 4, 22, 4, 4, 0 };
static unsigned long smpl_pos, smpl_len;

#define W16(c, i, o)  (*(unsigned short __far *)MK_FP(csel[c], (i) * csize[c] + (o)))
#define I16(c, i, o)  (*(short __far *)MK_FP(csel[c], (i) * csize[c] + (o)))

static int same4(const char *a, const char *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int scan(unsigned long pos, unsigned long end)
{
    char id[4], ty[4];
    unsigned long n;
    int c;
    while (pos + 8 <= end) {
        if (rd(pos, id, 4))
            return -1;
        n = rd32(pos + 4);
        if (same4(id, "LIST")) {
            if (rd(pos + 8, ty, 4) || scan(pos + 12, pos + 8 + n))
                return -1;
        } else if (same4(id, "ifil")) {
            unsigned short v;
            rd(pos + 8, &v, 2);
            sfver = v;
        } else if (same4(id, "smpl")) {
            smpl_pos = pos + 8;
            smpl_len = n;
        } else
            for (c = 0; c < NCH; c++)
                if (same4(id, ckid[c])) {
                    csel[c] = load_seg(pos + 8, n);
                    if (!csel[c])
                        return -1;
                    cnum[c] = (unsigned)n;      /* Bytes, unten Saetze */
                }
        pos += 8 + n + (n & 1);
    }
    return 0;
}

/* ---- Samples --------------------------------------------------------- */
static unsigned shsize;                 /* 16 (SBK) oder 46 (SF2)        */
static unsigned nsamp;
static unsigned first_ram;              /* SBK: erstes Sample im RAM     */
static unsigned long dram_words;
static int ram_ok;                      /* Samples hochgeladen           */
static unsigned skipped;

static unsigned long sh32(unsigned i, unsigned o)
{
    return *(unsigned long __far *)MK_FP(csel[C_SHDR], i * shsize + o);
}

static int s_rom(unsigned i)
{
    if (sfver == 1)
        return i < first_ram;
    return (*(unsigned short __far *)MK_FP(csel[C_SHDR], i * shsize + 44) & 0x8000) != 0;
}

/* ---- Ausgabe an den Treiber ----------------------------------------- */
#define ZBUF 16
static struct {
    struct fapi_awecmd k;
    struct fapi_awezone z[ZBUF];
} out;
static unsigned nz, zsent;
static struct {
    struct fapi_awecmd k;
    struct fapi_awepre p[256];
} pc;
#define pre pc.p
static unsigned npre;

static int flush_zones(void)
{
    if (!nz)
        return 0;
    out.k.op = AWE_ZONES;
    out.k.n = nz;
    if (FAPI_FAILED(AweCmd(&out, sizeof(out.k) + nz * sizeof(struct fapi_awezone))))
        return -1;
    zsent += nz;
    nz = 0;
    return 0;
}

/* eine fertige Zone (Tabelle ct, Preset-Bank/Programm) umrechnen */
static int make_zone(void)
{
    struct fapi_awezone *z;
    int *v = ct.val, root, tune, scale, si, g;
    unsigned long base, st, en, ls, le, shot;
    int lo, hi;

    for (g = 0; g < NGEN; g++)          /* Vorgaben, dann SBK umrechnen  */
        if (!ct.set[g])
            v[g] = defv(g);
    if (sfver == 1)
        for (g = 0; g < NGEN; g++)
            if (ct.set[g])
                v[g] = sbk2sf2(g, v[g]);
    lo = v[G_KEYR] & 0xFF; hi = (v[G_KEYR] >> 8) & 0xFF;
    if (lo > hi || hi > 127)
        return 0;
    if ((v[G_VELR] & 0xFF) > ((v[G_VELR] >> 8) & 0xFF))
        return 0;
    si = v[G_SAMP];
    if (si < 0 || (unsigned)si >= nsamp)
        return 0;
    if (!s_rom(si) && !ram_ok) {
        skipped++;
        return 0;
    }
    if (nz == ZBUF && flush_zones())
        return -1;
    z = &out.z[nz];

    st = sh32(si, sfver == 1 ? 0 : 20);
    en = sh32(si, sfver == 1 ? 4 : 24);
    ls = sh32(si, sfver == 1 ? 8 : 28);
    le = sh32(si, sfver == 1 ? 12 : 32);
    st += ((long)v[4] << 15) + v[0];
    en += ((long)v[12] << 15) + v[1];
    ls += ((long)v[45] << 15) + v[2];
    le += ((long)v[50] << 15) + v[3];
    if (v[54] != 1 && v[54] != 3) {     /* ohne Schleife: stille Schleife */
        shot = (unsigned)si + 1 < nsamp ? sh32(si + 1, sfver == 1 ? 0 : 20) - en : 48;
        if (shot < 16 || shot > 4096)
            shot = 48;
        ls = en + 8;
        le = en + shot - 8;
    }
    base = s_rom(si) ? 0 : 0x200000UL;
    z->start = base + st;
    z->lstart = base + ls;
    z->lend = base + le;
    z->klo = (unsigned char)lo; z->khi = (unsigned char)hi;
    z->vlo = (unsigned char)(v[G_VELR] & 0xFF);
    z->vhi = (unsigned char)((v[G_VELR] >> 8) & 0xFF);
    z->fixkey = (unsigned char)(v[46] >= 0 && v[46] <= 127 ? v[46] : 0xFF);
    z->fixvel = (unsigned char)(v[47] >= 0 && v[47] <= 127 ? v[47] : 0xFF);
    z->pan = (unsigned char)(v[17] < -500 ? 0 : v[17] > 500 ? 127 : muldiv(v[17] + 500, 127, 1000));
    z->att = (unsigned char)c_att(v[48]);
    z->chorus = (unsigned char)c_send(v[15]);
    z->reverb = (unsigned char)c_send(v[16]);
    z->cutoff = (unsigned char)c_cut(v[8]);
    z->filterq = (unsigned char)(v[9] / 12 > 15 ? 15 : v[9] < 0 ? 0 : v[9] / 12);
    z->excl = (unsigned char)v[57];
    z->rsv = 0;

    /* Grundtaste und Stimmung */
    scale = v[56];
    if (sfver == 1 && ct.set[55]) {
        root = v[55] / 100;
        tune = -(v[55] % 100);
        if (tune <= -50) {
            root++;
            tune += 100;
        }
        if (scale == 50)
            tune /= 2;
    } else if (sfver == 1) {
        root = 60;
        tune = 0;
    } else {
        root = *(unsigned char __far *)MK_FP(csel[C_SHDR], si * shsize + 40);
        tune = *(signed char __far *)MK_FP(csel[C_SHDR], si * shsize + 41);
    }
    if (ct.set[58] && v[58] >= 0)
        root += v[58] - (sfver == 1 ? 60 :
                *(unsigned char __far *)MK_FP(csel[C_SHDR], si * shsize + 40));
    if (sfver == 1)
        tune += v[51] * scale + muldiv(v[52], scale, 100);
    else
        tune += v[51] * 100 + v[52];
    if (root >= hi + 60)
        root -= 60;
    z->root = (unsigned char)root;
    z->scale = (unsigned char)(scale > 255 ? 255 : scale < 0 ? 0 : scale);
    z->rate = (short)muldiv(tune, 4096, 1200);
    if (sfver != 1) {
        unsigned long hz = sh32(si, 36);
        if (hz && hz != 44100UL)
            z->rate += muldiv(ilg(hz) - ilg(44100UL), 4096, 1200);
    }

    /* Huellkurven und LFOs (awesfx set_modenv/volenv/lfo1/lfo2) */
    z->moddelay = c_delay(v[25]);
    z->modatkhld = c_attack(v[26]) | (c_hold(v[27]) << 8);
    z->moddcysus = WORD(c_sus(v[29]), c_decay(v[28]));
    z->modrelease = WORD(0x80, c_decay(v[30]));
    z->pefe = WORD(sb8(mulhi(v[7], 0x1B4F)), sb8(mulhi(v[11], 0x048D)));
    z->voldelay = c_delay(v[33]);
    z->volatkhld = c_attack(v[34]) | (c_hold(v[35]) << 8);
    z->voldcysus = WORD(c_sus(v[37]), c_decay(v[36]));
    z->volrelease = WORD(0x80, c_decay(v[38]));
    z->lfo1delay = c_delay(v[21]);
    z->fmmod = WORD(sb8(mulhi(v[5], 0x1B4F)), sb8(mulhi(v[10], 0x0919)));
    z->tremfrq = WORD(sb8(muldiv(v[13], 128, 120)), c_freq(v[22]));
    z->lfo2delay = c_delay(v[23]);
    z->fm2frq2 = WORD(sb8(mulhi(v[6], 0x1B4F)), c_freq(v[24]));
    nz++;
    return 1;
}

/* Generatoren einer Zone (Bag b) in die Tabelle */
static void take_gens(struct tbl *t, int genc, unsigned b, unsigned nbag, int bagc)
{
    unsigned i, e = b + 1 < nbag ? W16(bagc, b + 1, 0) : cnum[genc];
    for (i = W16(bagc, b, 0); i < e && i < cnum[genc]; i++) {
        unsigned g = W16(genc, i, 0);
        if (g < NGEN) {
            t->val[g] = I16(genc, i, 2);
            t->set[g] = 1;
        }
    }
}

static int has_gen(int genc, unsigned b, unsigned nbag, int bagc, unsigned g)
{
    unsigned i, e = b + 1 < nbag ? W16(bagc, b + 1, 0) : cnum[genc];
    for (i = W16(bagc, b, 0); i < e && i < cnum[genc]; i++)
        if (W16(genc, i, 0) == g)
            return 1;
    return 0;
}

static void tclear(struct tbl *t)
{
    unsigned g;
    for (g = 0; g < NGEN; g++) {
        t->val[g] = 0;
        t->set[g] = 0;
    }
}

static void tcopy(struct tbl *d, struct tbl *s)
{
    unsigned g;
    for (g = 0; g < NGEN; g++) {
        d->val[g] = s->val[g];
        d->set[g] = s->set[g];
    }
}

static struct tbl pg, ig;               /* globale Zonen                 */

static int do_inst(unsigned in)
{
    unsigned b0, b1, b, nb = cnum[C_IBAG];
    int globl = 0, r, n = 0;
    if (in + 1 >= cnum[C_INST])
        return 0;
    b0 = W16(C_INST, in, 20);
    b1 = W16(C_INST, in + 1, 20);
    tclear(&ig);
    for (b = b0; b < b1 && b < nb; b++) {
        if (b == b0 && !has_gen(C_IGEN, b, nb, C_IBAG, G_SAMP)) {
            take_gens(&ig, C_IGEN, b, nb, C_IBAG);
            globl = 1;
            continue;
        }
        tcopy(&ct, &ig);
        take_gens(&ct, C_IGEN, b, nb, C_IBAG);
        if (!ct.set[G_SAMP])
            continue;
        merge(&ct, &pt);
        r = make_zone();
        if (r < 0)
            return -1;
        n += r;
    }
    (void)globl;
    return n;
}

static int do_preset(unsigned p)
{
    unsigned b0, b1, b, nb = cnum[C_PBAG], first = zsent + nz;
    int n = 0, r, i;
    unsigned prog = W16(C_PHDR, p, 20), bank = W16(C_PHDR, p, 22);
    if (bank > 128 || prog > 127)
        return 0;
    for (i = 0; i < (int)npre; i++)     /* doppelt: erstes gilt          */
        if (pre[i].bank == bank && pre[i].prog == prog)
            return 0;
    b0 = W16(C_PHDR, p, 24);
    b1 = W16(C_PHDR, p + 1, 24);
    tclear(&pg);
    for (b = b0; b < b1 && b < nb; b++) {
        if (b == b0 && !has_gen(C_PGEN, b, nb, C_PBAG, G_INST)) {
            take_gens(&pg, C_PGEN, b, nb, C_PBAG);
            continue;
        }
        tcopy(&pt, &pg);
        take_gens(&pt, C_PGEN, b, nb, C_PBAG);
        if (!pt.set[G_INST])
            continue;
        r = do_inst((unsigned)pt.val[G_INST]);
        if (r < 0)
            return -1;
        n += r;
    }
    if (n && npre < 256) {
        pre[npre].bank = (unsigned char)bank;
        pre[npre].prog = (unsigned char)prog;
        pre[npre].first = first;
        pre[npre].n = n;
        npre++;
    }
    return 0;
}

/* RAM-Samples (der ganze smpl-Abschnitt) ins RAM der Karte */
static int upload(void)
{
    unsigned long o;
    unsigned k;
    static struct {
        struct fapi_awecmd k;
        unsigned short w[512];
    } sb;
    for (o = 0; o < smpl_len; o += k) {
        k = smpl_len - o > sizeof(sb.w) ? sizeof(sb.w) : (unsigned)(smpl_len - o);
        if (rd(smpl_pos + o, sb.w, k))
            return -1;
        sb.k.op = AWE_SAMPLE;
        sb.k.n = k / 2;
        sb.k.addr = o >> 1;
        if (FAPI_FAILED(AweCmd(&sb, sizeof(sb.k) + (k & ~1u))))
            return -1;
    }
    return 0;
}

static char args[FAPI_ARGLEN], file[FAPI_PATHLEN], pv[FAPI_PATHLEN];

static void fail(const char *m)
{
    puts("AWELOAD: ");
    puts(m);
    puts("\r\n");
    DosExit(1);
}

void main(void)
{
    struct fapi_awecmd k;
    char *a;
    unsigned c, p, nram = 0;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && (a[1] == '?' )) {
        puts("AWELOAD [Datei]  Klangbank (SBK/SF2) f\x81r den Wavetable der AWE32/64 laden\r\n"
             "                 ohne Datei: \\DOS\\SYSTEM\\SYNTHGM.SBK des Startlaufwerks\r\n"
             "AWELOAD /U       Klangbank entfernen (MIDI wieder an den MPU-401)\r\n");
        return;
    }
    k.op = AWE_INFO;
    k.n = 0;
    k.addr = 0;
    if (FAPI_FAILED(AweCmd(&k, sizeof(k))))
        fail("keine AWE32/AWE64 (EMU8000) gefunden");
    dram_words = k.addr;
    if (a[0] == '/' && (a[1] == 'U' || a[1] == 'u')) {
        k.op = AWE_RESET;
        AweCmd(&k, sizeof(k));
        puts("AWELOAD: Klangbank entfernt\r\n");
        return;
    }
    if (*a)
        word(a, file, sizeof(file));
    else {
        file[0] = 'C';
        if (!FAPI_FAILED(DosGetEnv("COMSPEC", pv, sizeof(pv))) && pv[1] == ':')
            file[0] = pv[0];
        file[1] = 0;
        scat(file, ":\\DOS\\SYSTEM\\SYNTHGM.SBK", sizeof(file));
    }
    fh = DosOpen(file);
    if (FAPI_FAILED(fh)) {
        puts("AWELOAD: ");
        puts(file);
        fail("nicht gefunden (die Klangbank liegt den Treiberdisketten der Karte bei)");
    }
    if (rd(0, rbuf, 12) || !same4(rbuf, "RIFF") || !same4(rbuf + 8, "sfbk"))
        fail("keine SoundFont-Datei");
    if (scan(12, rd32(4) + 8))
        fail("Datei unvollst\x84ndig oder zu gro\xE1 (Abschnitt \x81""ber 64 KB)");
    for (c = 0; c < NCH; c++)
        if (!csel[c])
            fail("Datei unvollst\x84ndig");
    shsize = sfver == 1 ? 16 : 46;
    nsamp = cnum[C_SHDR] / shsize;
    for (c = 0; c < C_SHDR; c++)
        cnum[c] /= csize[c];
    first_ram = nsamp;
    if (sfver == 1)                     /* RAM beginnt bei Adresse 0     */
        for (c = 0; c < nsamp; c++)
            if (sh32(c, 0) == 0) {
                first_ram = c;
                break;
            }
    for (c = 0; c < nsamp; c++)
        if (!s_rom(c))
            nram++;

    k.op = AWE_RESET;                   /* alte Bank weg (auch MIDI)     */
    AweCmd(&k, sizeof(k));
    if (nram && smpl_len) {
        if ((smpl_len >> 1) > dram_words)
            puts("AWELOAD: zu wenig RAM auf der Karte - nur ROM-Kl\x84nge\r\n");
        else if (upload())
            fail("Fehler beim Laden der Samples");
        else
            ram_ok = 1;
    }
    for (p = 0; p + 1 < cnum[C_PHDR]; p++)
        if (do_preset(p) < 0)
            fail("zu viele Zonen f\x81r den Treiber");
    if (flush_zones())
        fail("zu viele Zonen f\x81r den Treiber");
    {
        pc.k.op = AWE_PRESETS;
        pc.k.n = npre;
        if (FAPI_FAILED(AweCmd(&pc, sizeof(pc.k) + npre * sizeof(struct fapi_awepre))))
            fail("Treiber nimmt die Presets nicht");
    }
    k.op = AWE_DONE;
    AweCmd(&k, sizeof(k));
    DosClose(fh);
    puts("AWELOAD: ");
    puts(file);
    puts(sfver == 1 ? " (SoundFont 1)" : " (SoundFont 2)");
    puts("\r\n  ");
    put_num(npre, 0, 0);
    puts(" Presets, ");
    put_num(zsent, 0, 0);
    puts(" Zonen");
    if (ram_ok) {
        puts(", ");
        put_num((smpl_len + 1023) >> 10, 0, 0);
        puts(" KB Samples im RAM der Karte");
    }
    if (skipped) {
        puts(", ");
        put_num(skipped, 0, 0);
        puts(" Zonen ohne Platz \x81""bergangen");
    }
    puts("\r\n");
}
