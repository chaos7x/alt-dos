/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PACKDISK.EXE - komprimierte Laufwerke anlegen und pruefen
 *
 *   PACKDISK /CREATE Datei MB [/DS3] [/RATIO:n] [/V:Bezeichnung]
 *       legt einen leeren Container an: Format von DriveSpace 6.22
 *       (8-KB-Cluster, bis 512 MB) bzw. mit /DS3 von DriveSpace 3
 *       (32-KB-Cluster, bis 2 GB). Das innere Laufwerk wird mit der
 *       geschaetzten Packrate n:1 (Vorgabe 2) groesser angelegt.
 *   PACKDISK /CHECK Datei [/F]
 *       prueft MDFAT gegen BITFAT und FAT (Bereiche, Ueberschneidungen,
 *       verlorene Cluster), zeigt Packrate und Platz; /F schreibt die
 *       BITFAT neu und gibt Cluster frei, die das FAT nicht mehr kennt.
 *
 * Eingehaengt wird mit LOOP (LOOP X: Datei), eingehaengte Container
 * lassen sich nicht pruefen (erst LOOP X: /D). Format nach dmsdos
 * doc/cvfinfo.doc (GPL, nur gelesen) wie kernel/loop.c: Superblock,
 * BITFAT, MDFAT, Bootblock, FAT, Stamm, Daten, "MDR" im letzten Sektor.
 * Kein Microsoft-Produktname im Programm (Entscheidung des Nutzers);
 * Kennung MSDSP6.0 und Dateiname DRVSPACE.nnn gehoeren zum Format.
 * ERRORLEVEL: 0 ok, 1 Fehler gefunden (nicht behoben), 2 Fehler behoben,
 * 3 Container nicht lesbar/anzulegen, 4 Aufruf.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN], tmp[FAPI_PATHLEN], label[12];
static unsigned char sec[512], sb[512], fl[512], md[512], fb[1024];
static unsigned hd;
static unsigned short dt[2];            /* Datum, Uhrzeit (FAT-Format)   */
static struct fapi_dirent de;

static unsigned long mulu(unsigned long a, unsigned b)  /* ohne __U4M   */
{
    unsigned long r = 0;
    for (; b; b >>= 1, a <<= 1)
        if (b & 1)
            r += a;
    return r;
}

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd32(const unsigned char *p) { return rd16(p) | ((unsigned long)rd16(p + 2) << 16); }
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, unsigned long v) { wr16(p, (unsigned)v); wr16(p + 2, (unsigned)(v >> 16)); }

static unsigned long udiv(unsigned long a, unsigned long b)
{
    unsigned long q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= 1UL << i;
        }
    }
    return q;
}

static int rd(unsigned long lba, void *buf, unsigned n)
{
    static unsigned long pos;
    pos = lba << 9;
    if (DosSeek(hd, &pos))
        return -1;
    return DosRead(hd, buf, n << 9) == (n << 9) ? 0 : -1;
}

static int wr(unsigned long lba, void *buf)
{
    static unsigned long pos;
    pos = lba << 9;
    if (DosSeek(hd, &pos))
        return -1;
    return DosWrite(hd, buf, 512) == 512 ? 0 : -1;
}

static void zero(unsigned char *p, unsigned n)
{
    while (n--)
        *p++ = 0;
}

static void say(const char *a, unsigned long v, const char *b)
{
    puts(a);
    put_num(v, 0, 1);
    puts(b);
}

static void hexn(unsigned long v, unsigned d)
{
    char b[9];
    unsigned i;
    for (i = d; i; i--, v >>= 4)
        b[i - 1] = "0123456789ABCDEF"[(unsigned)v & 15];
    b[d] = 0;
    puts(b);
}

static char *opt(char *p, const char *o)   /* "/X" in der Zeile -> dahinter */
{
    unsigned n = slen(o), k;
    for (; *p; p++) {
        if (*p != '/')
            continue;
        for (k = 1; k < n && up(p[k]) == o[k]; k++)
            ;
        if (k == n)
            return p + n;
    }
    return 0;
}

static unsigned long num(const char *p)
{
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9')
        v = (v << 3) + (v << 1) + (*p++ - '0');
    return v;
}

/* ---- /CREATE ------------------------------------------------------------- */
static int create(char *rest)
{
    unsigned long T, data = 0, inner, ncl = 0, tot, k;
    unsigned spc, sh, nbf = 1, nmd = 1, spf = 1, f16 = 0, ratio, i, d3, mb;
    char *p, *q;
    unsigned long bb, mdf, fatc, rootc, dst;

    rest = word(skip(rest), name, FAPI_PATHLEN);
    word(skip(rest), tmp, 16);
    mb = (unsigned)num(tmp);
    d3 = opt(rest, "/DS3") != 0;
    ratio = (p = opt(rest, "/RATIO:")) != 0 ? (unsigned)num(p) : 2;
    label[0] = 0;
    if ((p = opt(rest, "/V:")) != 0) {
        for (i = 0, q = p; i < 11 && *q && *q != ' ' && *q != '/'; i++, q++)
            label[i] = up(*q);
        label[i] = 0;
    }
    if (!name[0] || !mb || ratio < 1 || ratio > 16) {
        puts("Aufruf: PACKDISK /CREATE Datei MB [/DS3] [/RATIO:n] [/V:Name]\r\n");
        return 4;
    }
    if (mb > (d3 ? 2047 : 512) || mb < 1) {
        puts(d3 ? "Gr\x94\xE1" "e 1 bis 2047 MB.\r\n" : "Gr\x94\xE1" "e 1 bis 512 MB (gr\x94\xE1" "er mit /DS3).\r\n");
        return 4;
    }
    spc = d3 ? 64 : 16;
    sh = d3 ? 6 : 4;
    T = (unsigned long)mb << 11;
    for (i = 0; i < 6; i++) {           /* Aufteilung annaehern          */
        data = T - 1 - nbf - nmd - 1 - spf - 32 - 1;
        for (inner = 0, k = 0; k < ratio; k++)
            inner += data;
        ncl = inner >> sh;
        if (ncl > 65518UL)
            ncl = 65518UL;
        f16 = ncl >= 4085;
        spf = f16 ? (unsigned)((ncl + 2 + 255) >> 8)
                  : (unsigned)(((ncl + 2) + ((ncl + 2) << 1) + 1023) >> 10);
        nmd = d3 ? (unsigned)ncl / 102 + 2 : (unsigned)((ncl + 127) >> 7) + 1;
        nbf = (unsigned)((data + 4095) >> 12);
    }
    mdf = 1 + nbf;
    bb = mdf + nmd;
    fatc = bb + 1;
    rootc = fatc + spf;
    dst = rootc + 32;
    tot = 1 + spf + 32 + (ncl << sh);   /* inneres Laufwerk              */

    if ((hd = DosOpen(name)) != 0 && !FAPI_FAILED(hd)) {
        DosClose(hd);
        puts("Die Datei gibt es schon: ");
        puts(name);
        puts("\r\n");
        return 3;
    }
    hd = DosCreate(name);
    if (FAPI_FAILED(hd)) {
        puts("Datei kann nicht angelegt werden: ");
        puts(name);
        puts("\r\n");
        return 3;
    }
    /* Bootblock = BPB des inneren Laufwerks */
    zero(sec, 512);
    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    for (i = 0; i < 8; i++)
        sec[3 + i] = "ATDOS6.1"[i];
    wr16(sec + 11, 512);
    sec[13] = (unsigned char)spc;
    wr16(sec + 14, 1);                  /* reserviert = Bootblock        */
    sec[16] = 1;                        /* eine FAT                      */
    wr16(sec + 17, 512);
    if (tot < 65536UL)
        wr16(sec + 19, (unsigned)tot);
    else
        wr32(sec + 32, tot);
    sec[21] = 0xF8;
    wr16(sec + 22, spf);
    wr16(sec + 24, 63);
    wr16(sec + 26, 16);
    sec[36] = 0x80;
    sec[38] = 0x29;
    DosGetDateTime(dt);
    wr16(sec + 39, dt[1] ^ DosQTicks());    /* Seriennummer              */
    wr16(sec + 41, dt[0]);
    for (i = 0; i < 11; i++)
        sec[43 + i] = label[0] ? (i < slen(label) ? label[i] : ' ') : "NO NAME    "[i];
    for (i = 0; i < 8; i++)
        sec[54 + i] = (f16 ? "FAT16   " : "FAT12   ")[i];
    sec[62] = 0xCD;                     /* INT 18h: kein Systemdatentraeger */
    sec[63] = 0x18;
    sec[510] = 0x55;
    sec[511] = 0xAA;
    /* Superblock = Bootblock + Lage des Containers */
    for (i = 0; i < 512; i++)
        sb[i] = sec[i];
    for (i = 0; i < 8; i++)
        sb[3 + i] = "MSDSP6.0"[i];
    wr16(sb + 14, 1);                   /* FAT ab Bootblock + 1          */
    wr16(sb + 36, (unsigned)mdf - 1);
    wr16(sb + 39, (unsigned)bb);
    wr16(sb + 41, (unsigned)(rootc - bb));
    wr16(sb + 43, (unsigned)(dst - bb - 2));
    wr16(sb + 45, (unsigned)-2);        /* MDFAT-Eintrag 0 = Cluster 2   */
    sb[51] = d3 ? 3 : 2;
    sb[57] = '1'; sb[58] = f16 ? '6' : '2'; sb[59] = ' '; sb[60] = ' ';
    wr16(sb + 62, mb);
    if (wr(0, sb))
        goto werr;
    zero(fl, 512);
    for (k = 1; k < bb; k++)            /* BITFAT und MDFAT leer         */
        if (wr(k, fl))
            goto werr;
    if (wr(bb, sec))
        goto werr;
    fl[0] = 0xF8; fl[1] = 0xFF; fl[2] = 0xFF;
    if (f16)
        fl[3] = 0xFF;
    if (wr(fatc, fl))
        goto werr;
    zero(fl, 4);
    for (k = fatc + 1; k < dst; k++) {
        if (k == rootc && label[0]) {   /* Bezeichnung im Stamm          */
            for (i = 0; i < 11; i++)
                fl[i] = sec[43 + i];
            fl[11] = 0x08;
            wr16(fl + 22, dt[1]);
            wr16(fl + 24, dt[0]);
        }
        if (wr(k, fl))
            goto werr;
        zero(fl, 32);
    }
    puts("Lege den Datenbereich an ...\r\n");
    {
        static unsigned long sz;
        sz = (T - 1) << 9;
        if (DosNewSize(hd, &sz))
            goto werr;
    }
    zero(fl, 512);
    fl[0] = 'M'; fl[1] = 'D'; fl[2] = 'R';
    if (wr(T - 1, fl))
        goto werr;
    DosClose(hd);
    say("Container angelegt: ", (unsigned long)mb, " MB");
    puts(d3 ? " (32-KB-Cluster)\r\n" : " (8-KB-Cluster)\r\n");
    say("Inneres Laufwerk:   ", ncl << (sh - 1), " KB");
    say(" bei Packrate ", (unsigned long)ratio, ":1\r\n");
    puts("Einh\x84ngen mit: LOOP X: ");
    puts(name);
    puts("\r\n");
    return 0;
werr:
    DosClose(hd);
    DosDelete(name);
    puts("Schreibfehler (Datentr\x84ger voll?) - Container nicht angelegt.\r\n");
    return 3;
}

/* ---- /CHECK -------------------------------------------------------------- */
#define NSEG 8                          /* 8 x 512 Ki Bit = 4 Mi Sektoren */
static unsigned segs[NSEG], nseg;
static unsigned long dst, dend, nds, mdf, bb, fatc;
static unsigned mdn, spc, sh, d3, spf;
static int dcl;
static unsigned long md_sec = 0xFFFFFFFFUL, fat_sec = 0xFFFFFFFFUL;
static int md_dirty, fix, verb;
static unsigned long vsec[8];         /* /V: abweichende Sektoren     */
static unsigned char vkind[8];
static unsigned nv;

static unsigned char __far *bit(unsigned long k, unsigned char *m)
{
    *m = (unsigned char)(0x80 >> ((unsigned)k & 7));
    return (unsigned char __far *)MK_FP(segs[(unsigned)(k >> 19)], (unsigned)(k >> 3) & 0xFFFF);
}

static int md_flush(void)
{
    if (md_dirty && wr(md_sec, md))
        return -1;
    md_dirty = 0;
    return 0;
}

/* MDFAT-Eintrag -> Zeiger in md, 0 = Fehler */
static unsigned char *md_ent(unsigned cl)
{
    unsigned idx = cl + dcl, o;
    unsigned long s;
    if (d3) {
        s = idx / 102;
        o = idx % 102 * 5;
    } else {
        s = idx >> 7;
        o = (idx & 127) << 2;
    }
    if (s >= mdn)
        return 0;
    s += mdf;
    if (s != md_sec) {
        if (md_flush() || rd(s, md, 1))
            return 0;
        md_sec = s;
    }
    return md + o;
}

static unsigned fat_get(unsigned cl, int f16)
{
    unsigned long o = f16 ? (unsigned long)cl << 1 : (unsigned long)(cl + (cl >> 1));
    unsigned long s = fatc + (o >> 9);
    unsigned v;
    if (s != fat_sec) {
        if (rd(s, fb, 2))
            zero(fb, 1024);
        fat_sec = s;
    }
    o &= 511;
    v = fb[(unsigned)o] | (fb[(unsigned)o + 1] << 8);
    if (!f16)
        v = (cl & 1) ? v >> 4 : v & 0xFFF;
    return v;
}

static unsigned long n_err, n_cross, n_range, n_lost;

/* n Sektoren ab s als belegt eintragen; Ueberschneidung zaehlen */
static void claim(unsigned long s, unsigned n)
{
    unsigned char m, __far *p;
    for (; n; n--, s++) {
        if (s < dst || s > dend) {
            n_range++;
            continue;
        }
        p = bit(s - dst, &m);
        if (*p & m)
            n_cross++;
        *p |= m;
    }
}

/* /V: zu den abweichenden Sektoren den Cluster mit rohem MDFAT-Eintrag */
static void explain(unsigned long ncl, int f16)
{
    unsigned long cl, s, k;
    unsigned char *e;
    unsigned i, n, hit;
    puts("Aufbau: Bootblock ");
    put_num(bb, 0, 1);
    puts(", MDFAT ab ");
    put_num(mdf, 0, 1);
    puts(", Daten ");
    put_num(dst, 0, 1);
    puts("-");
    put_num(dend, 0, 1);
    puts(", dcluster ");
    hexn((unsigned)dcl, 4);
    puts("\r\n");
    for (i = 0; i < nv; i++) {
        puts(vkind[i] ? "frei, belegt markiert: Sektor " : "belegt, frei markiert: Sektor ");
        put_num(vsec[i], 0, 1);
        hit = 0;
        for (cl = 2; cl < ncl + 2 && !hit; cl++) {
            if ((e = md_ent((unsigned)cl)) == 0)
                break;
            if (d3) {
                s = e[0] | ((unsigned)e[1] << 8) | ((unsigned long)e[2] << 16);
                n = (e[3] >> 2) + 1;
                k = e[4] & 0x80;
            } else {
                k = rd32(e);
                s = k & 0x1FFFFFUL;
                n = ((unsigned)(k >> 22) & 15) + 1;
                k &= 0x80000000UL;
            }
            if (!k || vsec[i] < s + 1 || vsec[i] > s + n)
                continue;
            hit = 1;
            puts(", Cluster ");
            put_num(cl, 0, 1);
            puts(", MDFAT ");
            if (d3) {
                hexn(e[4], 2); hexn(e[3], 2); hexn(e[2], 2); hexn(e[1], 2); hexn(e[0], 2);
            } else
                hexn(rd32(e), 8);
            puts(", FAT ");
            hexn(fat_get((unsigned)cl, f16), 4);
        }
        puts("\r\n");
    }
}

static int check(char *rest)
{
    unsigned long units, tot, din, ncl, used = 0, stor = 0, unp = 0, cl, s, k, bf_bad = 0, bf_lost = 0;
    unsigned res, nfat, rents, i, fc, n, w, x, f16, packed = 0, frag = 0;
    unsigned char *e, m, __far *p;
    fix = opt(rest, "/F") != 0;
    verb = opt(rest, "/V") != 0;
    rest = skip(rest);                  /* Schalter vor dem Namen        */
    while (*rest == '/')
        rest = skip(next_word(rest));
    word(rest, name, FAPI_PATHLEN);
    if (!name[0]) {
        puts("Aufruf: PACKDISK /CHECK Datei [/F] [/V]\r\n");
        return 4;
    }
    hd = fix ? DosOpenRW(name) : DosOpen(name);
    if (FAPI_FAILED(hd)) {
        puts(hd == FAPI_ERR_INUSE ? "Die Datei ist in Benutzung (eingeh\x84ngt? Erst LOOP X: /D).\r\n"
             : hd == FAPI_ERR_DENIED ? "Die Datei ist schreibgesch\x81tzt - f\x81r /F erst ATTRIB -R.\r\n"
                                     : "Datei nicht gefunden.\r\n");
        return 3;
    }
    units = 0;                          /* Groesse aus dem Verzeichnis   */
    if (abs_path(name, tmp) == 0) {
        char *bn = base_name(tmp);
        scopy((char *)fl, bn, 260);
        *bn = 0;
        if (slen(tmp) > 3)
            tmp[slen(tmp) - 1] = 0;
        for (i = 0; DosFindEntry(&de, i, tmp) != FAPI_ERROR; i++)
            if (same(de.name, (char *)fl)) {
                units = de.size >> 9;
                break;
            }
    }
    if (units < 8) {
        puts("Datei nicht gefunden.\r\n");
        DosClose(hd);
        return 3;
    }
    if (rd(0, sb, 1) || sb[3] != 'M' || sb[4] != 'S' || sb[5] != 'D' || sb[8] != '6') {
        puts("Kein komprimiertes Laufwerk (Kennung MSDSP/MSDBL fehlt).\r\n");
        DosClose(hd);
        return 3;
    }
    d3 = sb[13] > 16 || sb[51] == 3;
    bb = rd16(sb + 39);
    fatc = bb + rd16(sb + 14);
    mdf = rd16(sb + 36) + 1UL;
    dcl = (int)rd16(sb + 45);
    dst = bb + rd16(sb + 43) + 2UL;
    mdn = (unsigned)(bb - mdf);
    dend = 0;
    if (rd(units - 1, sec, 1) == 0 && sec[0] == 'M' && sec[1] == 'D' && sec[2] == 'R')
        dend = units - 2;
    else if (rd(units - 2, sec, 1) == 0 && sec[0] == 'M' && sec[1] == 'D' && sec[2] == 'R')
        dend = units - 3;
    if (!dend) {
        puts("Warnung: Schlusssektor \"MDR\" fehlt.\r\n");
        dend = units - 1;
        n_err++;
    }
    if (rd(bb, sec, 1) || rd16(sec + 11) != 512) {
        puts("Bootblock des inneren Laufwerks unlesbar.\r\n");
        DosClose(hd);
        return 3;
    }
    spc = sec[13];
    for (sh = 0; sh < 7 && (1u << sh) != spc; sh++)
        ;
    res = rd16(sec + 14);
    nfat = sec[16];
    rents = rd16(sec + 17);
    spf = rd16(sec + 22);
    tot = rd16(sec + 19);
    if (!tot)
        tot = rd32(sec + 32);
    din = res + mulu(spf, nfat) + (rents >> 4);
    if (sh == 7 || dend < dst || tot <= din) {
        puts("Aufbau des Containers ung\x81ltig.\r\n");
        DosClose(hd);
        return 3;
    }
    ncl = (tot - din) >> sh;
    f16 = ncl >= 4085;
    nds = dend - dst + 1;
    nseg = (unsigned)((nds + 524287UL) >> 19);
    if (nseg > NSEG) {
        puts("Container zu gro\xE1.\r\n");
        DosClose(hd);
        return 3;
    }
    for (i = 0; i < nseg; i++) {
        segs[i] = DosAllocSeg(0);
        if (FAPI_FAILED(segs[i]) || !segs[i]) {
            puts("Zu wenig Speicher.\r\n");
            DosClose(hd);
            return 3;
        }
        for (w = 0; ; w++) {
            ((unsigned char __far *)MK_FP(segs[i], 0))[w] = 0;
            if (w == 0xFFFF)
                break;
        }
    }
    say("Pr\x81" "fe ", units >> 11, d3 ? " MB (DriveSpace-3-Format)" : " MB (DriveSpace-6-Format)");
    say(", ", ncl, " Cluster innen ...\r\n");

    /* Durchgang 1: MDFAT gegen FAT, Bereiche, Stuecke */
    for (cl = 2; cl < ncl + 2; cl++) {
        if ((e = md_ent((unsigned)cl)) == 0) {
            n_err++;
            break;
        }
        if (d3) {
            if (!(e[4] & 0x80))
                continue;
            s = e[0] | ((unsigned)e[1] << 8) | ((unsigned long)e[2] << 16);
            x = e[3] & 2;               /* zerstueckelt                  */
            n = (e[3] >> 2) + 1;        /* gepackt, Sektoren             */
            w = (e[4] & 63) + 1;        /* entpackt                      */
            m = e[4] & 0x40;            /* ungepackt                     */
        } else {
            k = rd32(e);
            if (!(k & 0x80000000UL))
                continue;
            s = k & 0x1FFFFFUL;
            x = 0;
            n = ((unsigned)(k >> 22) & 15) + 1;
            w = ((unsigned)(k >> 26) & 15) + 1;
            m = (unsigned char)((k >> 30) & 1);
        }
        if (!fat_get((unsigned)cl, f16)) {  /* das FAT kennt ihn nicht   */
            n_lost++;
            if (fix) {
                zero(e, d3 ? 5 : 4);
                md_dirty = 1;
                continue;
            }
        }
        if (w > spc || (!x && (m ? n != w : n > w))) {
            n_err++;
            if (n_err < 6)
                say("Cluster ", cl, ": L\x84ngen im MDFAT ung\x81ltig.\r\n");
        }
        used++;
        unp += w;
        if (!m)
            packed++;
        if (!x) {
            claim(s + 1, n);
            stor += n;
            continue;
        }
        frag++;
        if (rd(s + 1, fl, 1) || !fl[0] || fl[0] > 64 || fl[1] || fl[2] || fl[3]) {
            n_err++;
            say("Cluster ", cl, ": Liste der St\x81" "cke unlesbar.\r\n");
            claim(s + 1, n);
            stor += n;
            continue;
        }
        fc = fl[0];
        for (i = 1; i <= fc; i++) {
            k = fl[i * 4] | ((unsigned)fl[i * 4 + 1] << 8) | ((unsigned long)fl[i * 4 + 2] << 16);
            n = (fl[i * 4 + 3] >> 2) + 1;
            claim(k + 1, n);
            stor += n;
        }
    }
    if (md_flush())
        n_err++;

    /* Durchgang 2: BITFAT gegen die belegten Sektoren */
    for (k = 0; k < nds; k += 4096) {
        if (rd(1 + (k >> 12), sec, 1)) {
            n_err++;
            break;
        }
        x = 0;
        for (i = 0; i < 256 && k + ((unsigned long)i << 4) < nds; i++) {
            w = 0;
            for (n = 0; n < 16; n++) {
                s = k + ((unsigned long)i << 4) + n;
                if (s >= nds)
                    break;
                p = bit(s, &m);
                if (*p & m)
                    w |= 0x8000 >> n;
            }
            n = rd16(sec + 2 * i);
            if (n != w) {
                for (fc = 0; fc < 16; fc++) {
                    if ((w & ~n) & (0x8000 >> fc)) {
                        bf_bad++;       /* belegt, als frei markiert     */
                        if (nv < 6) {
                            vkind[nv] = 0;
                            vsec[nv++] = dst + k + ((unsigned long)i << 4) + fc;
                        }
                    } else if ((n & ~w) & (0x8000 >> fc)) {
                        bf_lost++;      /* frei, als belegt markiert     */
                        if (nv < 8) {
                            vkind[nv] = 1;
                            vsec[nv++] = dst + k + ((unsigned long)i << 4) + fc;
                        }
                    }
                }
                wr16(sec + 2 * i, w);
                x = 1;
            }
        }
        if (x && fix && wr(1 + (k >> 12), sec))
            n_err++;
    }
    if (verb)
        explain(ncl, f16);
    DosClose(hd);

    say("", used, " Cluster belegt");
    say(", davon ", packed, " gepackt");
    say(", ", frag, " zerst\x81" "ckelt\r\n");
    say("", stor >> 1, " KB im Container belegt");
    say(", ", (nds - stor) >> 1, " KB frei\r\n");
    if (stor) {
        k = udiv(mulu(unp, 10), stor);  /* Packrate in Zehnteln          */
        say("Packrate ", udiv(k, 10), ",");
        putc((char)('0' + (unsigned)(k - mulu(udiv(k, 10), 10))));
        puts(":1");
        say(", gesch\x84tzt frei ", udiv(mulu((nds - stor) >> 1, (unsigned)k), 10), " KB\r\n");
    }
    if (n_lost)
        say(fix ? "Freigegeben: " : "Verlorene Cluster (im MDFAT, nicht im FAT): ", n_lost, "\r\n");
    if (bf_bad)
        say("BITFAT: belegte Sektoren als frei markiert: ", bf_bad, "\r\n");
    if (bf_lost)
        say("BITFAT: freie Sektoren als belegt markiert: ", bf_lost, "\r\n");
    if (n_cross)
        say("Sektoren mehrfach belegt: ", n_cross, " (nicht behebbar)\r\n");
    if (n_range)
        say("Sektoren au\xE1" "erhalb des Datenbereichs: ", n_range, "\r\n");
    if (fix && (n_lost || bf_bad || bf_lost))
        puts("BITFAT und MDFAT wurden berichtigt.\r\n");
    if (!n_err && !n_cross && !n_range && !n_lost && !bf_bad && !bf_lost) {
        puts("Keine Fehler gefunden.\r\n");
        return 0;
    }
    if (!fix && (n_lost || bf_bad || bf_lost))
        puts("Mit /F beheben.\r\n");
    return fix && !n_err && !n_cross && !n_range ? 2 : 1;
}

void main(void)
{
    char *p;
    int rc;
    DosGetArgs(args);
    p = skip(args);
    word(p, tmp, 16);
    if (same(tmp, "/CREATE"))
        rc = create(next_word(p));
    else if (same(tmp, "/CHECK"))
        rc = check(next_word(p));
    else {
        puts("Legt komprimierte Laufwerke an und pr\x81" "ft sie.\r\n\r\n"
             "PACKDISK /CREATE Datei MB [/DS3] [/RATIO:n] [/V:Name]\r\n"
             "PACKDISK /CHECK Datei [/F] [/V]\r\n\r\n"
             "  /CREATE  leerer Container im DriveSpace-6.22-Format (bis 512 MB),\r\n"
             "           mit /DS3 im DriveSpace-3-Format (32-KB-Cluster, bis 2047 MB)\r\n"
             "  /RATIO:n gesch\x84tzte Packrate n:1 f\x81r die Gr\x94\xE1" "e innen (Vorgabe 2)\r\n"
             "  /CHECK   MDFAT, BITFAT und FAT pr\x81" "fen, Packrate zeigen\r\n"
             "  /F       Fehler in BITFAT/MDFAT beheben\r\n"
             "  /V       erste Abweichungen mit Cluster und MDFAT-Eintrag zeigen\r\n\r\n"
             "Einh\x84ngen mit LOOP X: Datei, aush\x84ngen mit LOOP X: /D.\r\n");
        rc = 4;
    }
    DosExit(rc);
}
