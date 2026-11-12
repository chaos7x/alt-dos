/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UFAT32.EXE /CONVERT - FAT16 an Ort und Stelle in FAT32 umwandeln
 * (eingebunden in task_ufat32.c), aufgerufen von CONVERT.EXE:
 *
 *   UFAT32 X: /CONVERT [/V]
 *
 * Vorbild fuer das Verhalten: CVT.EXE aus Windows 98 (kein Code). FAT16
 * hat hoechstens 65.524 Cluster, FAT32 braucht 65.525 - die Cluster
 * werden kleiner (Tabelle wie FORMAT), ein alter Cluster wird zu k neuen
 * an derselben Stelle. Der neue Datenbereich beginnt so, dass das
 * aufgeht (reservierte Sektoren ab 32 aufgefuellt); die Daten bleiben
 * liegen, nur was im kuenftigen FAT-Bereich liegt (alte Cluster < K),
 * und das feste Stammverzeichnis werden in freie Cluster kopiert.
 *
 *  A (sicher): alte FAT ganz in den Speicher, kopieren in freie Cluster
 *     (Platte sonst unveraendert - ein Abbruch kostet nichts), Baum
 *     einmal lesend ablaufen und pruefen.
 *  B (nicht unterbrechen): FAT32-Bereich nullen, Baum ablaufen: jeder
 *     Eintrag bekommt die neue Clusternummer (Hochwort Byte 20/21), jede
 *     Kette wird k-fach in die FAT32 geschrieben (Dateien auf ihre Groesse
 *     gekuerzt), EA DATA. SF: Verzeichnis-Startcluster der Saetze
 *     umrechnen; FSInfo, Bootsektor (+ Kopie) zuletzt.
 *  Danach Partitionstyp 06h/04h -> 0Bh, 0Eh -> 0Ch (DosOpenDisk).
 */

#define BOOT32
#include "../build/bootcode.h"

#define CV_NROOT 64                     /* Cluster fuer den alten Stamm   */

static unsigned o_spc, o_shift, o_nf, o_rd, kshift;
static unsigned long o_res, o_spf, o_data, o_n, o_maxc, o_total;
static unsigned long kclus;             /* alte Cluster < kclus verlegen  */
static unsigned f16s[2], rl_seg, dq_seg;
static unsigned rootd[CV_NROOT], nroot;
static unsigned long pick_at, used32, cv_dirs, cv_files;
static unsigned long ea_first, ea_size, ea_live;
static int cv_err;
static int keep_ea;                    /* /EA: Namen in EA DATA. SF lassen */
static unsigned char ob[512];

static unsigned f16(unsigned long x)
{
    return ((unsigned __far *)MK_FP(f16s[(unsigned)(x >> 15)], 0))[(unsigned)x & 0x7FFF];
}
static unsigned __far *rl(unsigned long x)
{
    return (unsigned __far *)MK_FP(rl_seg, (unsigned)x * 2);
}
static unsigned long phys(unsigned long x)
{
    return x < kclus ? *rl(x) : x;
}
static unsigned long olba(unsigned long p)
{
    return o_data + shl(p - 2, o_shift);
}
/* erster neuer Cluster des (physischen) alten Clusters p */
static unsigned long nmap(unsigned long p)
{
    return shr(olba(p) - data_lba, shift) + 2;
}
static int ovalid(unsigned long x) { return x >= 2 && x <= o_maxc; }

static unsigned seg64(void)
{
    unsigned s = DosAllocSeg(0);
    return FAPI_FAILED(s) ? 0 : s;
}

/* freier alter Cluster >= kclus, von hinten */
static unsigned long pick(void)
{
    while (pick_at >= kclus) {
        unsigned long c = pick_at--;
        if (f16(c) == 0 && !getbit(useds, c)) {
            setbit(useds, c);
            return c;
        }
    }
    return 0;
}

static int wr_secs(unsigned long lba, void *buf, unsigned n)
{
    static unsigned long pos;
    pos = lba;
    if (DosSeek(hd, &pos))
        return -1;
    return DosWrite(hd, buf, n << 9) == (n << 9) ? 0 : -1;
}

static int ocopy(unsigned long from, unsigned long to)   /* alte Geometrie */
{
    unsigned s, k;
    for (s = 0; s < o_spc; s += k) {
        k = o_spc - s > 8 ? 8 : o_spc - s;
        if (rd_secs(olba(from) + s, zb, k) || wr_secs(olba(to) + s, zb, k))
            return -1;
    }
    return 0;
}

/* Kette ab dem alten Cluster c k-fach in die FAT32, hoechstens need
   neue Cluster (0 = alle) */
static void emit(unsigned long c, unsigned long need)
{
    unsigned long prev = 0, n = 0, nc, steps = 0;
    unsigned j;
    while (ovalid(c) && (!need || n < need) && steps++ <= o_n) {
        nc = nmap(phys(c));
        for (j = 0; j < (1U << kshift) && (!need || n < need); j++, nc++) {
            if (prev)
                fat_put(prev, nc);
            prev = nc;
            n++;
        }
        c = f16(c);
    }
    if (prev)
        fat_put(prev, EOC);
    used32 += n;
}

/* neuer Startcluster fuer einen alten Eintragswert */
static unsigned long nfirst(unsigned long c)
{
    unsigned long p;
    if (!ovalid(c))
        return 0;
    p = phys(c);
    return ovalid(p) && p >= kclus ? nmap(p) : 0;
}

/* Kette pruefen (Phase A): 0 = gueltig, Laenge in *len */
static int chain_ok(unsigned long c, unsigned long *len)
{
    unsigned long n = 0;
    while (ovalid(c)) {
        if (++n > o_n)
            return -1;
        c = f16(c);
    }
    *len = n;
    return c >= 0xFFF8 ? 0 : -1;
}

/* Baum ablaufen. phase 0: pruefen und zaehlen, 1: umschreiben */
static int cv_walk(int phase)
{
    unsigned long i, x, p, lba, c, len, sz, nd = 1;
    unsigned s, o, j, fin, dirty;
    unsigned __far *dq = (unsigned __far *)MK_FP(dq_seg, 0);
    unsigned char *e;
    static char nm[13];

    dq[0] = 0;
    for (i = 0; i < nd; i++) {
        unsigned oc = dq[(unsigned)i];
        fin = 0;
        for (j = 0, x = oc; !fin; j++) {
            if (oc == 0) {
                if (j >= nroot)
                    break;
                p = rootd[j];
            } else {
                if (!ovalid(x) || j > o_n)
                    break;
                p = phys(x);
                x = f16(x);
            }
            for (s = 0; s < o_spc && !fin; s++) {
                lba = olba(p) + s;
                dsec_lba = 0xFFFFFFFFUL;
                if (rd_secs(lba, dsec, 1))
                    return -1;
                dirty = 0;
                for (o = 0; o < 512; o += 32) {
                    e = dsec + o;
                    if (e[0] == 0) {
                        fin = 1;
                        break;
                    }
                    if (e[11] == 0x0F)
                        continue;
                    c = rd16(e + 26);
                    if (phase) {
                        set_first(e, nfirst(c));
                        dirty = 1;
                    }
                    if (e[0] == 0xE5 || e[0] == '.' || (e[11] & 0x08))
                        continue;
                    if (e[11] & 0x10) {             /* Verzeichnis        */
                        if (!c)
                            continue;
                        if (!phase) {
                            if (!ovalid(c) || chain_ok(c, &len)) {
                                name83(e, nm);
                                puts("Fehlerhafte Kette: ");
                                puts(nm);
                                puts("\n");
                                return -1;
                            }
                            cv_dirs++;
                        } else
                            emit(c, 0);
                        if (nd >= 32768UL) {
                            puts("Zu viele Verzeichnisse.\n");
                            return -1;
                        }
                        dq[(unsigned)nd++] = (unsigned)c;
                        continue;
                    }
                    sz = rd32(e + 28);
                    if (oc == 0) {
                        static const char ean[] = "EA DATA SF ";
                        unsigned k;
                        for (k = 0; k < 11 && e[k] == (unsigned char)ean[k]; k++)
                            ;
                        if (k == 11) {
                            ea_first = c;
                            ea_size = sz;
                        }
                    }
                    if (!c)
                        continue;
                    if (!phase) {
                        if (!ovalid(c) || chain_ok(c, &len)) {
                            name83(e, nm);
                            puts("Fehlerhafte Kette: ");
                            puts(nm);
                            puts("\n");
                            return -1;
                        }
                        cv_files++;
                    } else
                        emit(c, sz ? shr(sz + shl(1, cshift) - 1, cshift) : 0);
                }
                if (dirty && wr_sec(lba, dsec))
                    return -1;
            }
        }
    }
    return 0;
}

/* EA DATA. SF: Verzeichnis-Startcluster der Saetze umrechnen (phase 1)
   bzw. lebende Saetze zaehlen (phase 0) */
static int cv_ea(int phase)
{
    unsigned long x = ea_first, g = 0, nx = 0, p, steps = 0, dc;
    unsigned s, o, dirty, end = 0;
    while (ovalid(x) && g < ea_size && !end && steps++ <= o_n) {
        p = phys(x);
        for (s = 0; s < o_spc && g < ea_size && !end; s++) {
            if (rd_secs(olba(p) + s, dsec, 1))
                return -1;
            dsec_lba = 0xFFFFFFFFUL;
            dirty = 0;
            for (o = 0; o < 512 && !end; o += 32, g += 32) {
                if (g != nx || g >= ea_size)
                    continue;
                if (!dsec[o + 1]) {
                    end = 1;                /* Ende                       */
                    break;
                }
                nx = g + ((unsigned long)dsec[o + 1] << 5);
                if (dsec[o] != 'L' && dsec[o] != 'W')
                    continue;
                if (!phase) {
                    ea_live++;
                    continue;
                }
                dc = rd32(dsec + o + 2);
                if (dc) {
                    wr32(dsec + o + 2, nfirst(dc));
                    dirty = 1;
                }
            }
            if (dirty && wr_sec(olba(p) + s, dsec))
                return -1;
        }
        x = f16(x);
    }
    return 0;
}

/* Eintrag mit Typ old und Anfang start in Tabelle sec (Sektor at) -> 1 */
static int pt_patch(unsigned h, unsigned long at, unsigned long base, unsigned n,
                    unsigned long start, unsigned char old)
{
    static unsigned long pos;
    unsigned i;
    unsigned char *t;
    for (i = 0; i < n; i++) {
        t = sec + 446 + i * 16;
        if (t[4] == old && base + rd32(t + 8) == start) {
            unsigned k;                 /* derselbe Bootsektor? (ob)     */
            pos = start;
            if (DosSeek(h, &pos) || DosRead(h, dsec, 512) != 512)
                return 0;
            dsec_lba = 0xFFFFFFFFUL;
            for (k = 0; k < 90 && dsec[k] == ob[k]; k++)
                ;
            if (k < 90)
                continue;               /* andere Platte, gleiche Lage    */
            t[4] = old == 0x0E ? 0x0C : 0x0B;
            pos = at;
            return DosSeek(h, &pos) || DosWrite(h, sec, 512) != 512 ? -1 : 1;
        }
    }
    return 0;
}

static int pt_read(unsigned h, unsigned long at)
{
    static unsigned long pos;
    pos = at;
    return DosSeek(h, &pos) || DosRead(h, sec, 512) != 512 ||
           sec[510] != 0x55 || sec[511] != 0xAA ? -1 : 0;
}

/* Partitionstyp im MBR bzw. in der EBR-Kette aendern -> 0 */
static int set_ptype(unsigned long start, unsigned char old)
{
    static struct fapi_diskinfo di;
    unsigned n, h, i, k;
    int r;
    unsigned long ebr, ext;
    unsigned char *t;

    for (n = 0; DosDiskInfo(n, &di) == 0; n++) {
        if (di.bsize != 512 || !di.nvol)
            continue;
        h = DosOpenDisk(n, DRV_OPEN_LOCK | DRV_OPEN_LBA);
        if (FAPI_FAILED(h))
            continue;
        r = 0;
        ext = 0;
        if (pt_read(h, 0) == 0) {
            for (i = 0; i < 4; i++) {
                t = sec + 446 + i * 16;
                if (t[4] == 5 || t[4] == 0x0F)
                    ext = rd32(t + 8);
            }
            r = pt_patch(h, 0, 0, 4, start, old);
            for (ebr = ext, k = 0; !r && ext && k < 64; k++) {
                if (pt_read(h, ebr))
                    break;
                r = pt_patch(h, ebr, ebr, 1, start, old);
                t = sec + 446 + 16;
                if (t[4] != 5 && t[4] != 0x0F)
                    break;
                ebr = ext + rd32(t + 8);
            }
        }
        DosClose(h);
        if (r)
            return r > 0 ? 0 : -1;
    }
    return -1;
}

#define E2V_FIXROOT 0
#define E2V_USED(d) (used32 += (d))
#define ENT_CLUS(e) (((unsigned long)rd16f((e) + 20) << 16) | rd16f((e) + 26))
#include "ufat_e2v.c"

static int do_convert(void)
{
    static struct fapi_fsinfo fi;
    unsigned long c, mb, nfree = 0, nmove = 0, cl;
    unsigned i, k, ours;
    unsigned char *b = sec;

    if (DosQFSInfo(drive, &fi) || (fi.flags & (FSI_READONLY | FSI_REMOVABLE))) {
        puts("Umwandeln geht nur f\x81r Festplatten-Partitionen.\n");
        return 3;
    }
    if (fi.fsname[0] != 'F' || fi.fsname[1] != 'A' || fi.fsname[2] != 'T' || fi.fsname[3]) {
        puts(fi.fsname[3] == '3' ? "Das Laufwerk ist bereits FAT32.\n"
                                 : "Nur FAT16-Laufwerke lassen sich umwandeln.\n");
        return 3;
    }
    if (fi.ptype != 0x04 && fi.ptype != 0x06 && fi.ptype != 0x0E) {
        puts("Unbekannter Partitionstyp - nicht umgewandelt.\n");
        return 3;
    }
    hd = DosOpenDrive(drive, DRV_OPEN_LOCK | DRV_OPEN_LBA);
    if (FAPI_FAILED(hd)) {
        puts("Das Laufwerk kann nicht gesperrt werden (Dateien ge\x94" "ffnet?).\n");
        return 3;
    }
    if (rd_secs(0, ob, 1) || rd16(ob + 11) != 512 || !ob[13] || !ob[16] || ob[16] > 2 ||
        !rd16(ob + 22) || !rd16(ob + 17)) {
        puts("Bootsektor nicht lesbar oder kein FAT16-Dateisystem.\n");
        goto fail;
    }
    o_spc = ob[13];
    for (o_shift = 0; o_shift < 8 && (1U << o_shift) < o_spc; o_shift++)
        ;
    o_res = rd16(ob + 14);
    o_nf = ob[16];
    o_spf = rd16(ob + 22);
    o_rd = (rd16(ob + 17) + 15) >> 4;
    o_total = rd16(ob + 19) ? rd16(ob + 19) : rd32(ob + 32);
    o_data = o_res + (o_nf == 2 ? o_spf + o_spf : o_spf) + o_rd;
    o_n = o_total > o_data ? shr(o_total - o_data, o_shift) : 0;
    o_maxc = o_n + 1;
    if ((1U << o_shift) != o_spc || o_n < 4085 || o_n > 65524UL ||
        o_spf < shr(2 * (o_maxc + 1) + 511, 9)) {
        puts("Kein g\x81ltiges FAT16-Dateisystem.\n");
        goto fail;
    }

    /* alte FAT in den Speicher */
    f16s[0] = seg64();
    f16s[1] = o_maxc >= 32768UL ? seg64() : f16s[0];
    rl_seg = seg64();
    dq_seg = seg64();
    nbm = 1;
    if (!f16s[0] || !f16s[1] || !rl_seg || !dq_seg || bm_alloc(useds)) {
        puts("Zu wenig Arbeitsspeicher.\n");
        goto fail;
    }
    for (c = 0; c <= o_maxc; c += 2048) {      /* je 8 Sektoren           */
        unsigned n = (unsigned)(o_spf - (c >> 8) > 8 ? 8 : o_spf - (c >> 8));
        unsigned __far *d = (unsigned __far *)MK_FP(f16s[(unsigned)(c >> 15)],
                                                    ((unsigned)c & 0x7FFF) * 2);
        if (rd_secs(o_res + (c >> 8), zb, n)) {
            puts("Die FAT ist nicht lesbar.\n");
            goto fail;
        }
        for (i = 0; i < n * 256; i++)
            d[i] = rd16(zb + i * 2);
    }

    /* neue Aufteilung (wie FORMAT, aber hoechstens die alte Clustergroesse) */
    mb = o_total >> 11;
    spc = mb < 260 ? 1 : mb <= 8192 ? 8 : mb <= 16384 ? 16 : mb <= 32768 ? 32 : 64;
    if (spc > o_spc)
        spc = o_spc;
    for (;;) {
        for (shift = 0; (1U << shift) < spc; shift++)
            ;
        fat_lba = 32;
        spf = udiv(o_total - 32 + (128UL * spc + 1) - 1, 128UL * spc + 1);
        data_lba = fat_lba + spf + spf;
        fat_lba += (o_data - data_lba) & (spc - 1);     /* passend auffuellen */
        data_lba = fat_lba + spf + spf;
        cl = shr(o_total - data_lba, shift);
        if (cl >= 65525UL || spc == 1)
            break;
        spc >>= 1;
    }
    if (cl < 65525UL) {
        puts("Das Laufwerk ist f\x81r FAT32 zu klein (mindestens etwa 33 MB).\n");
        goto fail;
    }
    cshift = shift + 9;
    kshift = o_shift - shift;
    clusters = cl;
    maxc = cl + 1;
    nfats = 2;
    kclus = data_lba > o_data ? 2 + shr(data_lba - o_data + o_spc - 1, o_shift) : 2;
    nroot = (o_rd + o_spc - 1) >> o_shift;
    if (nroot > CV_NROOT || kclus > 32768UL) {
        puts("Aufbau des Laufwerks wird nicht unterst\x81tzt.\n");
        goto fail;
    }
    for (c = 2; c <= o_maxc; c++) {
        unsigned v = f16(c);
        if (c < kclus) {
            if (v == 0xFFF7) {
                puts("Fehlerhafte Sektoren im k\x81nftigen FAT-Bereich - nicht umwandelbar.\n");
                goto fail;
            }
            if (v)
                nmove++;
        } else if (!v)
            nfree++;
    }
    if (nfree < nmove + nroot) {
        puts("Zu wenig freier Platz (");
        put_num(shl(nmove + nroot, o_shift - 1), 0);
        puts(" KB n\x94tig).\n");
        goto fail;
    }
    puts("Laufwerk ");
    putc((char)('A' + drive));
    puts(": FAT16 -> FAT32, Zuordnungseinheit ");
    put_num(o_spc / 2, 0);
    puts(o_spc == 1 ? ",5" : "");
    puts(" KB -> ");
    put_num(spc / 2, 0);
    puts(spc == 1 ? ",5" : "");
    puts(" KB, ");
    put_num(cl, 0);
    puts(" Zuordnungseinheiten.\n");
    if (verbose) {
        puts("Reserviert ");
        put_num(fat_lba, 0);
        puts(", FAT ");
        put_num(spf, 0);
        puts(" Sektoren, zu verlegen ");
        put_num(nmove, 0);
        puts(" Cluster.\n");
    }

    /* A: kopieren (die Platte bleibt sonst unveraendert) */
    pick_at = o_maxc;
    for (c = 2; c < kclus; c++) {
        *rl(c) = 0;
        if (f16(c)) {
            unsigned long d = pick();
            if (!d || ocopy(c, d)) {
                puts("Lese-/Schreibfehler beim Verlegen - nichts ge\x84ndert.\n");
                goto fail;
            }
            *rl(c) = (unsigned)d;
        }
    }
    for (i = 0; i < nroot; i++) {               /* Stamm in Cluster        */
        unsigned s, n;
        rootd[i] = (unsigned)pick();
        for (s = 0; s < o_spc; s += n) {
            unsigned long rs = shl(i, o_shift) + s;
            n = o_spc - s > 8 ? 8 : o_spc - s;
            for (k = 0; k < 4096; k++)
                zb[k] = 0;
            for (k = 0; k < n; k++)
                if (rs + k < o_rd && rd_secs(o_res + (o_nf == 2 ? o_spf + o_spf : o_spf) + rs + k, zb + k * 512, 1))
                    goto rerr;
            if (wr_secs(olba(rootd[i]) + s, zb, n))
                goto rerr;
        }
    }
    for (k = 0; k < 4096; k++)
        zb[k] = 0;
    if (cv_walk(0) || (ea_first && cv_ea(0))) {
        puts("Das Dateisystem ist fehlerhaft - zuerst CHKDSK /F. Nichts ge\x84ndert.\n");
        goto fail;
    }
    put_num(cv_files, 0);
    puts(" Dateien, ");
    put_num(cv_dirs, 0);
    puts(" Verzeichnisse.\n");
    if (ea_live) {
        put_num(ea_live, 0);
        puts(keep_ea ? " lange Namen in EA DATA. SF bleiben dort (LONGNAMES=EA).\n"
                     : " lange Namen in EA DATA. SF werden nach VFAT \x81" "bertragen.\n");
    }
    if (!autom) {
        puts("W\x84hrend der Umwandlung darf der Rechner nicht ausgeschaltet werden.\n"
             "Umwandeln (J/N)? ");
        if (!yes())
            goto fail;
    }

    /* B: ab hier nicht unterbrechen */
    puts("Wandle um ...\n");
    if (zero_secs(1, data_lba - 1))
        goto werr;
    fc_init();
    fat_put(0, 0x0FFFFFF8UL);
    fat_put(1, 0x0FFFFFFFUL);
    rootc = nmap(rootd[0]);
    {
        unsigned long need = (o_rd + spc - 1) >> shift, n = 0, prev = 0, nc;
        for (i = 0; i < nroot && n < need; i++)
            for (k = 0, nc = nmap(rootd[i]); k < (1U << kshift) && n < need; k++, nc++, n++) {
                if (prev)
                    fat_put(prev, nc);
                prev = nc;
            }
        fat_put(prev, EOC);
        used32 += n;
    }
    for (c = kclus; c <= o_maxc; c++)
        if (f16(c) == 0xFFF7)
            for (k = 0; k < (1U << kshift); k++) {
                fat_put(nmap(c) + k, BAD);
                used32++;
            }
    if (cv_walk(1) || (ea_first && cv_ea(1)))
        cv_err = 1;
    fat_flush();
    if (ferr)
        cv_err = 1;
    /* lange Namen aus EA DATA. SF nach VFAT (ohne /EA = LONGNAMES=EA) */
    if (!cv_err && ea_live && !keep_ea) {
        if (e2v_run(nfirst(ea_first), ea_size) == 0) {
            put_num(e2v_names, 0);
            puts(" lange Namen nach VFAT \x81" "bertragen, EA DATA. SF gel\x94scht.\n");
        } else
            puts("Lange Namen bleiben in EA DATA. SF (Verzeichnis zu gro\xE1 oder kein\n"
                 "Platz) - AT-DOS liest sie weiter daraus.\n");
        if (ferr)
            cv_err = 1;
    }

    /* FSInfo */
    for (i = 0; i < 512; i++)
        b[i] = 0;
    wr32(b, 0x41615252UL);
    wr32(b + 484, 0x61417272UL);
    wr32(b + 488, cl - used32);
    wr32(b + 492, 0xFFFFFFFFUL);
    wr32(b + 508, 0xAA550000UL);
    if (wr_sec(1, b) || wr_sec(7, b))
        goto werr;
    /* Bootsektor: alter Startcode von AT-DOS -> FAT32-Startcode */
    for (i = 0x3E, ours = 3; i < 0x50; i++) {
        if (ob[i] != boot_hd[i])
            ours &= ~1;
        if (ob[i] != boot_fd[i])
            ours &= ~2;
    }
    for (i = 0; i < 512; i++)
        b[i] = i < 36 ? ob[i] : 0;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    for (i = 0; i < 8; i++)
        b[3 + i] = (unsigned char)"ALTDOS60"[i];
    b[13] = (unsigned char)spc;
    wr16(b + 14, (unsigned)fat_lba);
    b[16] = 2;
    wr16(b + 17, 0);
    wr16(b + 19, 0);
    wr16(b + 22, 0);
    wr32(b + 32, o_total);
    wr32(b + 36, spf);
    wr32(b + 44, rootc);
    wr16(b + 48, 1);
    wr16(b + 50, 6);
    b[64] = 0x80;
    b[66] = 0x29;
    for (i = 0; i < 4; i++)
        b[67 + i] = ob[38] == 0x29 ? ob[39 + i] : (unsigned char)(o_total >> (i * 8)) ^ 0x5A;
    for (i = 0; i < 11; i++)
        b[71 + i] = ob[38] == 0x29 ? ob[43 + i] : (unsigned char)"NO NAME    "[i];
    for (i = 0; i < 8; i++)
        b[82 + i] = (unsigned char)"FAT32   "[i];
    if (ours) {
        for (i = 0; i < 3; i++)
            b[i] = boot_32[i];
        for (i = 0x5A; i < 510; i++)
            b[i] = boot_32[i];
    } else {
        for (i = 0; i < sizeof(nosys); i++)
            b[90 + i] = nosys[i];
        for (i = 0; nosys_msg[i]; i++)
            b[0x80 + i] = (unsigned char)nosys_msg[i];
    }
    b[510] = 0x55; b[511] = 0xAA;
    if (ours && (wr_sec(2, (void *)boot_32b) || wr_sec(8, (void *)boot_32b)))
        goto werr;
    if (wr_sec(6, b) || wr_sec(0, b))
        goto werr;
    for (i = 0; i < 512; i++)           /* zum Wiederfinden (set_ptype) */
        ob[i] = b[i];
    DosClose(hd);
    if (cv_err) {
        puts("Fehler beim Umwandeln - das Laufwerk ist besch\x84" "digt.\n");
        return 3;
    }
    if (set_ptype(fi.hidden, fi.ptype))
        puts("Hinweis: Partitionstyp konnte nicht ge\x84ndert werden (AT-DOS liest das\n"
             "Laufwerk trotzdem, Windows 95/98 erst mit Typ 0Bh/0Ch).\n");
    puts("Umwandlung beendet: FAT32, ");
    put_num(cl - used32, 0);
    puts(" Zuordnungseinheiten frei.\n");
    if (ours)
        puts("Startcode f\x81r FAT32 geschrieben.\n");
    return 0;
rerr:
    puts("Lese-/Schreibfehler beim Kopieren des Stammverzeichnisses - nichts ge\x84ndert.\n");
fail:
    DosClose(hd);
    return 3;
werr:
    puts("Schreibfehler - das Laufwerk ist besch\x84" "digt.\n");
    DosClose(hd);
    return 3;
}
