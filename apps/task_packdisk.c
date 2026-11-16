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
 * Kennung MSDBL6.0 und Dateiname DRVSPACE.nnn gehoeren zum Format.
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

/* Schalter "/X" an beliebiger Stelle der Zeile -> Zeiger dahinter.
   Nur am Wortanfang; ohne ':' am Ende muss der Schalter dort enden
   (sonst traefe "/V" auch "/V:Name"). */
static char *opt(char *p, const char *o)
{
    unsigned n = slen(o), k;
    for (p = skip(p); *p; p = next_word(p)) {
        if (*p != '/')
            continue;
        for (k = 1; k < n && up(p[k]) == o[k]; k++)
            ;
        if (k < n)
            continue;
        if (o[n - 1] == ':' || !p[n] || p[n] == ' ' || p[n] == '\t' || p[n] == '/')
            return p + n;
    }
    return 0;
}

/* n-tes Wort der Zeile, das kein Schalter ist (0 = erstes) */
static int posw(char *p, unsigned n, char *b, unsigned len)
{
    b[0] = 0;
    for (p = skip(p); *p; p = next_word(p)) {
        if (*p == '/')
            continue;
        if (!n--) {
            word(p, b, len);
            return 1;
        }
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
static int mkcvf(unsigned mb, unsigned long tnow, unsigned d3, unsigned ratio);

static int create(char *rest)
{
    unsigned ratio, i, d3, mb;
    char *p, *q;

    posw(rest, 0, name, FAPI_PATHLEN);
    posw(rest, 1, tmp, 16);
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
    return mkcvf(mb, 0, d3, ratio);
}

/* Container anlegen: Aufteilung (BITFAT, MDFAT, inneres FAT) fuer mb MB,
   die Datei selbst nur tnow Sektoren lang (0 = ganz) - /COMPRESS laesst
   ihn danach wachsen (cvf_grow), Feld 62 nennt die Hoechstgroesse. */
static unsigned cv_spc, cv_quiet;
static int mkcvf(unsigned mb, unsigned long tnow, unsigned d3, unsigned ratio)
{
    unsigned long T, data = 0, inner, ncl = 0, tot, k;
    unsigned spc, sh, nbf = 1, nmd = 1, spf = 256, f16 = 0, i, res, drel, dcl;
    unsigned long bb, mdf, fatc, rootc, dst;

    spc = d3 ? 64 : 16;
    sh = d3 ? 6 : 4;
    res = d3 ? 32 : 16;                 /* FAT ab Bootblock + res (wie
                                           MS-DOS 6.22 bzw. Windows 98)  */
    T = (unsigned long)mb << 11;
    for (i = 0; i < 6; i++) {           /* Aufteilung annaehern          */
        drel = (res + spf + 32 + spc - 1) & ~(spc - 1); /* Daten - 2      */
        dcl = (drel >> sh) - 2;         /* MDFAT-Eintrag = Cluster + dcl */
        data = T - 1 - nbf - nmd - drel - 2 - 1;
        for (inner = 0, k = 0; k < ratio; k++)
            inner += data;
        ncl = inner >> sh;
        if (ncl > 65518UL)
            ncl = 65518UL;
        f16 = ncl >= 4085;
        spf = 256;                      /* wie MS-DOS/Windows: FAT immer
                                           fuer 65.536 Eintraege, Daten
                                           direkt hinter dem Stamm       */
        nmd = d3 ? (unsigned)ncl / 102 + 3 /* dcl < 100 */
                 : (unsigned)((ncl + dcl + 2 + 127) >> 7) + 1;
        nbf = (unsigned)((data + 4095) >> 12);
    }
    drel = (res + spf + 32 + spc - 1) & ~(spc - 1);
    dcl = (drel >> sh) - 2;
    mdf = 1 + nbf;
    bb = mdf + nmd;
    fatc = bb + res;
    rootc = fatc + spf;
    dst = bb + drel + 2;
    tot = res + spf + 32 + (ncl << sh); /* inneres Laufwerk              */
    cv_spc = spc;
    if (!tnow || tnow > T)
        tnow = T;
    if (tnow < dst + 64) {
        puts("Zu wenig Platz f\x81r den Container.\r\n");
        return 3;
    }

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
    wr16(sec + 14, res);                /* reserviert: Bootblock + Luecke */
    sec[16] = 2;                        /* zwei FATs wie MS-DOS: die
                                           zweite gibt es im Container
                                           nicht, sie spiegelt die erste */
    wr16(sec + 17, 512);
    wr32(sec + 32, tot + spf);          /* innen mit der zweiten FAT     */
    sec[21] = 0xF8;
    wr16(sec + 22, spf);
    wr16(sec + 24, 32);                 /* Geometrie wie MS-DOS innen    */
    wr16(sec + 26, 64);
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
    /* Superblock: Aufbau wie die Container von MS-DOS 6.22 (DriveSpace,
       Version 2) und Windows 98 (DriveSpace 3), Byte fuer Byte verglichen.
       Nur die BPB-Felder bis 1Fh stimmen mit dem Bootblock ueberein. */
    zero(sb, 512);
    sb[0] = 0xEB; sb[1] = d3 ? 0x6E : 0x58; sb[2] = 0x90;
    for (i = 0; i < 8; i++)
        sb[3 + i] = "MSDBL6.0"[i];
    for (i = 11; i < 32; i++)
        sb[i] = sec[i];
    sb[16] = 1;                         /* Superblock: eine FAT, 17/6    */
    wr16(sb + 24, 17);
    wr16(sb + 26, 6);
    wr32(sb + 32, tot);                 /* bestimmt den hoechsten Cluster */
    wr16(sb + 36, (unsigned)mdf - 1);
    sb[38] = 9;                         /* log2 Bytes je Sektor          */
    wr16(sb + 39, (unsigned)bb);
    wr16(sb + 41, (unsigned)(rootc - bb));
    wr16(sb + 43, drel);                /* Daten - 2, Vielfaches von spc */
    wr16(sb + 45, dcl);
    sb[50] = (unsigned char)sh;         /* log2 Sektoren je Cluster      */
    sb[51] = d3 ? 0 : 2;                /* Version: 2 = DriveSpace 6.22,
                                           Windows 98 schreibt bei DS3 0 */
    if (!d3) {
        sb[57] = '1'; sb[58] = f16 ? '6' : '2'; sb[59] = ' '; sb[60] = ' ';
    }
    wr16(sb + 62, mb);
    if (wr(0, sb))
        goto werr;
    zero(fl, 512);
    for (k = 1; k < bb; k++)            /* BITFAT und MDFAT leer         */
        if (wr(k, fl))
            goto werr;
    if (wr(bb, sec))
        goto werr;
    for (k = bb + 1; k < fatc; k++)     /* reservierte Sektoren          */
        if (wr(k, fl))
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
        sz = (tnow - 1) << 9;
        if (DosNewSize(hd, &sz))
            goto werr;
    }
    zero(fl, 512);
    fl[0] = 'M'; fl[1] = 'D'; fl[2] = 'R';
    if (wr(tnow - 1, fl))
        goto werr;
    DosClose(hd);
    if (tnow < T) {
        say("Container angelegt: ", tnow >> 11, " MB");
        say(" (w\x84" "chst bis ", (unsigned long)mb, " MB)");
    } else
        say("Container angelegt: ", (unsigned long)mb, " MB");
    puts(d3 ? " (32-KB-Cluster)\r\n" : " (8-KB-Cluster)\r\n");
    say("Inneres Laufwerk:   ", ncl << (sh - 1), " KB");
    say(" bei Packrate ", (unsigned long)ratio, ":1\r\n");
    if (!cv_quiet) {
        puts("Einh\x84ngen mit: LOOP X: ");
        puts(name);
        puts("\r\n");
    }
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

/* /V: Kopfbytes der gepackten Cluster und Bit 21 des MDFAT zaehlen
   (MS-DOS 6.22 unterscheidet DoubleSpace- und DriveSpace-Daten je
   Cluster - welches Merkmal es nimmt, soll das zeigen) */
static unsigned long sv_n[8], sv_cl[8];
static unsigned char sv_h[8][5];
static void survey(unsigned long ncl)
{
    unsigned long cl, k, s, r21[2];
    unsigned char *e, h[5];
    unsigned i, j, nk = 0;

    r21[0] = r21[1] = 0;
    for (cl = 2; cl < ncl + 2; cl++) {
        if ((e = md_ent((unsigned)cl)) == 0)
            break;
        if (d3) {
            if (!(e[4] & 0x80) || (e[4] & 0x40) || (e[3] & 2))
                continue;
            s = e[0] | ((unsigned)e[1] << 8) | ((unsigned long)e[2] << 16);
            h[4] = e[3] & 1;
        } else {
            k = rd32(e);
            if (!(k & 0x80000000UL))
                continue;
            h[4] = (unsigned char)((k >> 21) & 1);
            if (k & 0x40000000UL) {     /* ungepackt: nur Bit 21 zaehlen */
                r21[h[4]]++;
                continue;
            }
            s = k & 0x1FFFFFUL;
        }
        if (rd(s + 1, sec, 1))
            continue;
        h[0] = sec[0]; h[1] = sec[1]; h[2] = sec[2]; h[3] = sec[3];
        for (i = 0; i < nk; i++) {
            for (j = 0; j < 5 && sv_h[i][j] == h[j]; j++)
                ;
            if (j == 5)
                break;
        }
        if (i == nk) {
            if (nk == 8)
                i = 7;                  /* Rest unter dem letzten        */
            else {
                for (j = 0; j < 5; j++)
                    sv_h[nk][j] = h[j];
                sv_cl[nk++] = cl;
            }
        }
        sv_n[i]++;
    }
    for (i = 0; i < nk; i++) {
        puts("Gepackt, Kopf ");
        for (j = 0; j < 4; j++) {
            hexn(sv_h[i][j], 2);
            putc(' ');
        }
        puts(d3 ? "Bit0 " : "Bit21 ");
        putc((char)('0' + sv_h[i][4]));
        say(": ", sv_n[i], " Cluster");
        say(", erster ", sv_cl[i], "\r\n");
    }
    if (!d3) {
        say("Ungepackt: Bit21 0: ", r21[0], "");
        say(", Bit21 1: ", r21[1], "\r\n");
    }
}

static int check(char *rest)
{
    unsigned long units, tot, din, ncl, used = 0, stor = 0, unp = 0, cl, s, k, bf_bad = 0, bf_lost = 0;
    unsigned res, nfat, rents, i, fc, n, w, x, f16, packed = 0, frag = 0;
    unsigned char *e, m, __far *p;
    fix = opt(rest, "/F") != 0;
    verb = opt(rest, "/V") != 0;
    posw(rest, 0, name, FAPI_PATHLEN);
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
    if (verb) {                         /* Superblock roh (Vergleich mit */
        say("Datei ", units, " Sektoren, Superblock:\r\n");  /* echten Containern) */
        for (i = 0; i < 96; i++) {
            if (!(i & 15)) {
                hexn(i, 2);
                puts(":");
            }
            puts(" ");
            hexn(sb[i], 2);
            if ((i & 15) == 15)
                puts("\r\n");
        }
        say("Bootblock (Sektor ", bb, "):\r\n");  /* innerer BPB    */
        for (i = 0; i < 64; i++) {
            if (!(i & 15)) {
                hexn(i, 2);
                puts(":");
            }
            puts(" ");
            hexn(sec[i], 2);
            if ((i & 15) == 15)
                puts("\r\n");
        }
        if (rd(fatc, fl, 1) == 0) {
            say("FAT (Sektor ", fatc, "):");
            for (i = 0; i < 8; i++) {
                puts(" ");
                hexn(fl[i], 2);
            }
            puts("\r\n");
        }
    }

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
    if (verb) {
        explain(ncl, f16);
        survey(ncl);
    }
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

/* ---- /COMPRESS ----------------------------------------------------------- */
/* Laufwerk X: in einen Container X:\DRVSPACE.000 umziehen, Datei fuer
   Datei: kopieren, Datum/Attribute uebernehmen, Original loeschen - eine
   Datei liegt immer ganz auf einer der beiden Seiten (Abbruch = hoechstens
   eine doppelte Kopie). Der Container beginnt mit dem freien Platz und
   waechst, sobald geloeschte Originale Platz gemacht haben: aushaengen,
   Datei verlaengern, "MDR" ans neue Ende, wieder einhaengen. Ein
   abgebrochener Lauf setzt mit demselben Aufruf fort. */
static unsigned long cv_units, cv_dst, cv_dend, cv_mdf, cv_max;
static unsigned long c_free, c_moved, c_skip, c_bytes;
static unsigned c_drv, c_ldrv, c_grow, c_want;
static char c_path[FAPI_PATHLEN], c_dest[FAPI_PATHLEN], c_root[8];

static unsigned long fsize(const char *nm)   /* Groesse aus dem Verzeichnis */
{
    char *bn;
    unsigned i;
    if (abs_path(nm, tmp))
        return 0;
    bn = base_name(tmp);
    scopy((char *)fl, bn, 260);
    *bn = 0;
    if (slen(tmp) > 3)
        tmp[slen(tmp) - 1] = 0;
    for (i = 0; DosFindEntry(&de, i, tmp) != FAPI_ERROR; i++)
        if (same(de.name, (char *)fl))
            return de.size;
    return 0;
}

/* Lage des (geschlossenen) Containers name; hd offen zum Schreiben */
static int cvf_geom(void)
{
    cv_units = fsize(name) >> 9;
    hd = DosOpenRW(name);
    if (FAPI_FAILED(hd))
        return -1;
    if (cv_units < 64 || rd(0, sb, 1) || sb[3] != 'M' || sb[4] != 'S' || sb[8] != '6') {
        DosClose(hd);
        return -2;
    }
    cv_mdf = rd16(sb + 36) + 1UL;
    cv_dst = rd16(sb + 39) + (unsigned long)rd16(sb + 43) + 2;
    cv_max = (unsigned long)rd16(sb + 62) << 11;
    if (cv_max > cv_dst + ((cv_mdf - 1) << 12) + 1)
        cv_max = cv_dst + ((cv_mdf - 1) << 12) + 1;   /* BITFAT reicht so weit */
    cv_dend = 0;
    if (rd(cv_units - 1, sec, 1) == 0 && sec[0] == 'M' && sec[1] == 'D' && sec[2] == 'R')
        cv_dend = cv_units - 2;
    if (!cv_dend) {
        DosClose(hd);
        return -2;
    }
    return 0;
}

static unsigned long cvf_free(void)     /* freie Datensektoren (BITFAT)  */
{
    unsigned long k, nds = cv_dend - cv_dst + 1, f = 0;
    unsigned i, w, n;
    for (k = 0; k < nds; k += 4096) {
        if (rd(1 + (k >> 12), sec, 1))
            return 0;
        for (i = 0; i < 256 && k + ((unsigned long)i << 4) < nds; i++) {
            w = rd16(sec + 2 * i);
            if (w == 0 && k + ((unsigned long)i << 4) + 16 <= nds) {
                f += 16;
                continue;
            }
            for (n = 0; n < 16 && k + ((unsigned long)i << 4) + n < nds; n++)
                if (!(w & (0x8000 >> n)))
                    f++;
        }
    }
    return f;
}

static int cvf_grow(unsigned long add)  /* -> 1 gewachsen                 */
{
    static unsigned long sz;
    unsigned long nu = cv_units + add;
    if (nu > cv_max)
        nu = cv_max;
    if (nu < cv_units + 64)
        return 0;
    sz = nu << 9;
    if (DosNewSize(hd, &sz))
        return 0;
    zero(fl, 512);
    wr(cv_units - 1, fl);               /* altes Ende wird Datenbereich   */
    fl[0] = 'M'; fl[1] = 'D'; fl[2] = 'R';
    if (wr(nu - 1, fl))
        return 0;
    cv_units = nu;
    cv_dend = nu - 2;
    return 1;
}

static unsigned long host_free(void)    /* freie Sektoren auf X: ohne Rest */
{
    static unsigned long fb4;
    unsigned long f;
    if (DosFreeSpace(c_drv, &fb4))
        return 0;
    f = fb4 >> 9;
    return f > c_grow ? f - c_grow : 0;
}

static int c_attach(void)
{
    unsigned rc = DosLoop(LOOP_ATTACH, name, 0);
    if (rc >= FAPI_ERR_FIRST) {
        puts(rc == FAPI_ERR_FRAG ? "Container zu stark zerst\x81" "ckelt - DEFRAG und danach PACKDISK /COMPRESS erneut.\r\n" :
             rc == FAPI_ERR_INUSE ? "Container schon eingeh\x84ngt - erst LOOP X: /D.\r\n" :
                                    "Container l\x84\xE1t sich nicht einh\x84ngen.\r\n");
        return -1;
    }
    c_ldrv = rc;
    return 0;
}

#define C_MARGIN(spc) (2UL * (spc) + 16)     /* Verzeichnis-Cluster usw.  */

static int c_visit(char *path, struct fapi_dirent *e, int what, int depth)
{
    static struct fapi_finfo fi;
    unsigned long need;
    static unsigned long ifree;
    int rc;

    if (what == WT_LEAVE) {
        if (depth == 0 || slen(path) <= 3)
            return 0;
        return DosRmDir(path) ? 0 : WT_REMOVED;
    }
    if ((e->attr & 0x08) || (depth == 0 && (same(e->name, "EA DATA. SF") || same(e->name, base_name(name)))))
        return 0;
    if (depth == 0 && what == WT_FILE && (e->attr & 0x04))
        return 0;                       /* Systemdateien im Stamm (Start-
                                           dateien eines DOS) bleiben wie
                                           bei DRVSPACE auf dem Traeger  */
    scopy(c_dest, path, FAPI_PATHLEN);
    c_dest[0] = (char)('A' + c_ldrv);
    if (what == WT_DIR) {
        DosMkDir(c_dest);
        return WT_DESCEND;
    }
    need = ((e->size + 511) >> 9) + C_MARGIN(cv_spc);
    if (need > c_free) {
        c_want = 1;                     /* nach dem Wachsen noch einmal  */
        return 0;
    }
    if (DosFreeSpace(c_ldrv, &ifree) == 0 && ifree < e->size + 65536UL) {
        puts("Inneres Laufwerk voll (Packrate zu niedrig gesch\x84tzt) - Rest bleibt.\r\n");
        c_free = 0;
        cv_max = 0;                     /* kein Wachsen mehr               */
        return WT_STOP;
    }
    rc = copy_file(path, c_dest);
    if (rc == -7) {                     /* doch voll: wachsen             */
        c_free = 0;
        c_want = 1;
        return 0;
    }
    if (rc) {
        puts("  \x81" "bersprungen: ");
        puts(path);
        puts(rc == -6 || rc == -1 ? " (in Benutzung oder nicht lesbar)\r\n" : " (Fehler beim Kopieren)\r\n");
        c_skip++;
        return 0;
    }
    fi.mask = 3;
    fi.attr = e->attr & 0x27;
    fi.date = e->date;
    fi.time = e->time;
    DosSetFInfo(c_dest, &fi);
    if (e->attr & 1) {                  /* schreibgeschuetzt: zum Loeschen */
        fi.mask = 1;
        fi.attr = e->attr & 0x26;
        DosSetFInfo(path, &fi);
    }
    if (DosDelete(path)) {              /* Original bleibt -> Kopie weg  */
        DosDelete(c_dest);
        puts("  \x81" "bersprungen (nicht l\x94schbar): ");
        puts(path);
        puts("\r\n");
        c_skip++;
        return 0;
    }
    c_free -= need - C_MARGIN(cv_spc);
    c_moved++;
    c_bytes += e->size;
    if ((c_moved & 15) == 0) {
        say("\r", c_moved, " Dateien, ");
        say("", c_bytes >> 10, " KB verschoben    ");
    }
    return WT_REMOVED;
}

static int compress(char *rest)
{
    static struct fapi_fsinfo fs;
    static unsigned long fb4;
    static struct fapi_loopinfo li;
    unsigned d3, ratio, mb, i, grew;
    unsigned long total, tnow, before;
    char *p;
    int r;

    posw(rest, 0, tmp, 16);
    if (!tmp[0] || tmp[1] != ':' || tmp[2]) {
        puts("Aufruf: PACKDISK /COMPRESS X: [/DS3] [/RATIO:n] [/RESERVE:MB] [/Y]\r\n");
        return 4;
    }
    c_drv = (unsigned)(up(tmp[0]) - 'A');
    d3 = opt(rest, "/DS3") != 0;
    ratio = (p = opt(rest, "/RATIO:")) != 0 ? (unsigned)num(p) : 2;
    c_grow = (unsigned)((p = opt(rest, "/RESERVE:")) != 0 ? num(p) : 1) << 11;
    if (ratio < 1 || ratio > 16 || c_drv > 25)
        return 4;
    if (DosQFSInfo(c_drv, &fs) || (!same(fs.fsname, "FAT") && !same(fs.fsname, "FAT32")) || fs.bsize != 512) {
        puts("Nur FAT-Laufwerke lassen sich komprimieren.\r\n");
        return 3;
    }
    for (i = 0; DosLoop(LOOP_INFO, i, &li) == 0; i++)
        if (li.drives & (1UL << c_drv)) {
            puts("Das ist selbst ein Loop-Laufwerk.\r\n");
            return 3;
        }
    c_root[0] = up(tmp[0]);
    c_root[1] = ':';
    c_root[2] = '\\';
    c_root[3] = 0;
    scopy(name, c_root, FAPI_PATHLEN);
    scat(name, "ATDOS.SYS", FAPI_PATHLEN);
    if (fsize(name)) {
        puts("Das Startlaufwerk l\x84\xE1t sich (noch) nicht komprimieren.\r\n");
        return 3;
    }
    scopy(name, c_root, FAPI_PATHLEN);
    scat(name, "DRVSPACE.000", FAPI_PATHLEN);
    if (fsize(name)) {
        for (i = 0; DosLoop(LOOP_INFO, i, &li) == 0; i++)   /* abgebrochen? */
            if (li.drives && li.cdrive == c_drv && same(li.path, "\\DRVSPACE.000")) {
                for (r = 2; r < 26 && !(li.drives & (1UL << r)); r++)
                    ;
                if (DosLoop(LOOP_DETACH, r, 0)) {
                    puts("Container ist eingeh\x84ngt und in Benutzung - erst LOOP X: /D.\r\n");
                    return 3;
                }
                break;
            }
        puts("Setze fort mit ");
        puts(name);
        puts("\r\n");
    } else {
        total = fs.sectors >> 11;
        mb = total > (d3 ? 2047UL : 512UL) ? (d3 ? 2047 : 512) : (unsigned)total;
        if (DosFreeSpace(c_drv, &fb4))
            return 3;
        tnow = fb4 >> 9;
        if (tnow <= c_grow + 2048) {
            puts("Zu wenig freier Platz (mindestens 1 MB plus Rest /RESERVE).\r\n");
            return 3;
        }
        tnow -= c_grow;
        puts("Laufwerk ");
        puts(c_root);
        say(" komprimieren: Container bis ", (unsigned long)mb, " MB");
        puts(d3 ? ", DriveSpace 3" : ", DriveSpace 6");
        if (!d3 && total > 512)
            puts(" (Laufwerk gr\x94\xE1" "er als 512 MB - /DS3 nimmt bis 2047 MB)");
        puts(".\r\nDateien werden einzeln verschoben.\r\n");
        if (!opt(rest, "/Y")) {
            puts("Fortfahren (J/N)? ");
            if (!ask_yn())
                return 1;
        }
        label[0] = 0;
        cv_quiet = 1;
        if ((r = mkcvf(mb, tnow, d3, ratio)) != 0)
            return r;
        {
            static struct fapi_finfo fi;
            fi.mask = 1;
            fi.attr = 0x06;             /* versteckt + System wie DRVSPACE */
            DosSetFInfo(name, &fi);
        }
    }
    for (;;) {
        if (cvf_geom()) {
            puts("Container nicht lesbar: ");
            puts(name);
            puts("\r\n");
            return 3;
        }
        grew = 0;
        {                               /* wachsen um den freien Platz    */
            unsigned long add = host_free();
            if (add >= 64 && cv_units < cv_max && cvf_grow(add)) {
                grew = 1;
                say("\rContainer jetzt ", cv_units >> 11, " MB                        \r\n");
            }
        }
        rd(rd16(sb + 39), sec, 1);
        cv_spc = sec[13];
        c_free = cvf_free();
        DosClose(hd);
        c_free = c_free > C_MARGIN(cv_spc) ? c_free - C_MARGIN(cv_spc) : 0;
        if (c_attach())
            return 3;
        scopy(c_path, c_root, FAPI_PATHLEN);
        before = c_moved;
        c_want = 0;
        c_skip = 0;
        r = walk_tree(c_path, c_visit);
        DosLoop(LOOP_DETACH, c_ldrv, 0);
        if (r || !c_want)
            break;                      /* fertig oder inneres voll       */
        if (cv_units >= cv_max || host_free() < 64 || (!grew && c_moved == before)) {
            puts("\r\nKein Platz mehr zum Wachsen - der Rest bleibt auf dem Laufwerk.\r\n");
            break;
        }
    }
    say("\r", c_moved, " Dateien verschoben (");
    say("", c_bytes >> 10, " KB)");
    if (c_skip)
        say(", ", c_skip, " \x81" "bersprungen");
    puts(".\r\nEinh\x84ngen mit: LOOP X: ");
    puts(name);
    puts("\r\nbeim Start mit CONFIG.SYS: LOOP=X:,");
    puts(name);
    puts("\r\n");
    return c_skip ? 1 : 0;
}

void main(void)
{
    char *p;
    int rc;
    DosGetArgs(args);
    p = skip(args);                     /* Schalter in jeder Reihenfolge */
    if (opt(p, "/CREATE") && !opt(p, "/CHECK") && !opt(p, "/COMPRESS"))
        rc = create(p);
    else if (opt(p, "/CHECK") && !opt(p, "/CREATE"))
        rc = check(p);
    else if (opt(p, "/COMPRESS") && !opt(p, "/CREATE") && !opt(p, "/CHECK"))
        rc = compress(p);
    else {
        puts("Legt komprimierte Laufwerke an und pr\x81" "ft sie.\r\n\r\n"
             "PACKDISK /CREATE Datei MB [/DS3] [/RATIO:n] [/V:Name]\r\n"
             "PACKDISK /CHECK Datei [/F] [/V]\r\n"
             "PACKDISK /COMPRESS X: [/DS3] [/RATIO:n] [/RESERVE:MB] [/Y]\r\n\r\n"
             "  /CREATE  leerer Container im DriveSpace-6.22-Format (bis 512 MB),\r\n"
             "           mit /DS3 im DriveSpace-3-Format (32-KB-Cluster, bis 2047 MB)\r\n"
             "  /RATIO:n gesch\x84tzte Packrate n:1 f\x81r die Gr\x94\xE1" "e innen (Vorgabe 2)\r\n"
             "  /CHECK   MDFAT, BITFAT und FAT pr\x81" "fen, Packrate zeigen\r\n"
             "  /F       Fehler in BITFAT/MDFAT beheben\r\n"
             "  /V       erste Abweichungen mit Cluster und MDFAT-Eintrag zeigen\r\n"
             "  /COMPRESS Dateien von X: in X:\\DRVSPACE.000 verschieben (fortsetzbar),\r\n"
             "           /RESERVE:MB bleibt auf X: frei (Vorgabe 1)\r\n\r\n"
             "Schalter d\x81rfen in beliebiger Reihenfolge stehen.\r\n"
             "Einh\x84ngen mit LOOP X: Datei, aush\x84ngen mit LOOP X: /D.\r\n");
        rc = 4;
    }
    DosExit(rc);
}
