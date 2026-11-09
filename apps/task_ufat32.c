/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UFAT32.EXE - Pruefprogramm fuer FAT32, gestartet von CHKDSK.EXE (6.0)
 *
 *   UFAT32 Laufwerk: [/F] [/R] [/V] [/AUTO]   (/R: ufat32_scan.c)
 *   UFAT32 Laufwerk: /FORMAT [/Q] [/V:Bezeichnung]     (ufat32_fmt.c)
 *
 * Wie UFAT, aber fuer grosse Datentraeger gebaut: die FAT (bei FAT32
 * leicht viele MB) bleibt auf der Platte und wird ueber einen kleinen
 * Sektorcache gelesen; die Bitmaps (ein Bit je Cluster) liegen in
 * DosAllocSeg-Segmenten zu 64 KB (je 524.288 Cluster). Das Laufwerk wird
 * mit DRV_OPEN_LBA geoeffnet (Stellen in Sektoren, ueber 4 GB).
 * Geprueft wird: FAT-Kopien gleich, jede Kette (ungueltige Cluster,
 * Querverbindungen, Laenge passt zur Groesse), Verzeichnisbaum ab dem
 * Stammcluster (eigener Stapel), verlorene Ketten. /F repariert wie UFAT
 * (Ketten abschneiden, Groessen anpassen, FILEnnnn.CHK), setzt das
 * Sauber-Bit (FAT[1] Bit 27) und traegt die freien Cluster ins FSInfo ein.
 */
#include <i86.h>
#include "fapi.h"

#define EOC      0x0FFFFFFFUL
#define BAD      0x0FFFFFF7UL
#define MASK     0x0FFFFFFFUL
#define MAXDEPTH 24
#define NBMSEG   7                      /* je Bitmap (LDT hat 14 freie Plaetze): 3,6 Mio. Cluster */

static unsigned hd, drive, fix, verbose, autom, errors, warned;
static unsigned spc, shift, cshift, nfats, fsis, extf;
static unsigned long total, fat_lba, data_lba, spf, clusters, maxc, rootc;
static unsigned char boot[96];
static unsigned char sec[512], dsec[512];
static unsigned long dsec_lba = 0xFFFFFFFFUL;
static char pathbuf[200], args[FAPI_ARGLEN];
static unsigned useds[NBMSEG], auxs[NBMSEG], nbm;

static unsigned long nfiles, nhidden, ndirs, nbad;
static unsigned long cfiles, chidden, cdirs;

/* ---- Hilfen ------------------------------------------------------------ */
static unsigned long shl(unsigned long v, unsigned n) { while (n--) v <<= 1; return v; }
static unsigned long shr(unsigned long v, unsigned n) { while (n--) v >>= 1; return v; }
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void putc(char c)
{
    static char b;
    b = c;
    DosWrite(STDOUT, &b, 1);
}

static void put_num(unsigned long v, int w)
{
    static const unsigned long pw[10] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    static char d[14], o[16];
    int i, n = 0, k = 0, started = 0;
    for (i = 0; i < 10; i++) {
        char c = '0';
        while (v >= pw[i]) {
            v -= pw[i];
            c++;
        }
        if (c != '0' || started || i == 9) {
            d[n++] = c;
            started = 1;
        }
    }
    for (i = 0; i < n; i++) {
        o[k++] = d[i];
        if ((n - i - 1) % 3 == 0 && i != n - 1)
            o[k++] = '.';
    }
    o[k] = 0;
    for (i = k; i < w; i++)
        putc(' ');
    puts(o);
}

static void put_hex4(unsigned v)
{
    int i;
    for (i = 12; i >= 0; i -= 4)
        putc("0123456789ABCDEF"[(v >> i) & 15]);
}

static int yes(void)
{
    unsigned c;
    if (autom) {
        puts("J\n");
        return 1;
    }
    for (;;) {
        c = (unsigned)up((char)KbdCharIn());
        if (c == 'J' || c == 'Y') { puts("J\n"); return 1; }
        if (c == 'N') { puts("N\n"); return 0; }
    }
}

static unsigned rd16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long rd32(const unsigned char *p)
{
    return rd16(p) | ((unsigned long)rd16(p + 2) << 16);
}
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, unsigned long v) { wr16(p, (unsigned)v); wr16(p + 2, (unsigned)(v >> 16)); }

/* ---- Sektoren (DRV_OPEN_LBA: Stelle = Sektor) --------------------------- */
static int rd_secs(unsigned long lba, void *buf, unsigned n)
{
    static unsigned long pos;
    pos = lba;
    if (DosSeek(hd, &pos))
        return -1;
    return DosRead(hd, buf, n << 9) == (n << 9) ? 0 : -1;
}

static int wr_sec(unsigned long lba, void *buf)
{
    static unsigned long pos;
    pos = lba;
    if (DosSeek(hd, &pos))
        return -1;
    return DosWrite(hd, buf, 512) == 512 ? 0 : -1;
}

static int load_dsec(unsigned long lba)
{
    if (lba == dsec_lba)
        return 0;
    dsec_lba = 0xFFFFFFFFUL;
    if (rd_secs(lba, dsec, 1))
        return -1;
    dsec_lba = lba;
    return 0;
}

/* ---- FAT ueber einen Cache von 8 Sektoren ------------------------------ */
#define NFC 8
static unsigned char fc[NFC][512];
static unsigned long fc_tag[NFC];
static unsigned char fc_dirty[NFC];
static int fat_changed, ferr;

static void fc_init(void)
{
    unsigned i;
    for (i = 0; i < NFC; i++)
        fc_tag[i] = 0xFFFFFFFFUL;
}

/* Sektor s der FAT in alle Kopien schreiben */
static void fc_wb(unsigned i)
{
    unsigned k;
    unsigned long lba = fat_lba + fc_tag[i];
    for (k = 0; k < nfats; k++, lba += spf)
        if (wr_sec(lba, fc[i]))
            ferr = 1;
    fc_dirty[i] = 0;
}

static unsigned char *fc_get(unsigned long s)
{
    unsigned i = (unsigned)s & (NFC - 1);
    if (fc_tag[i] != s) {
        if (fc_dirty[i])
            fc_wb(i);
        fc_tag[i] = 0xFFFFFFFFUL;
        if (rd_secs(fat_lba + s, fc[i], 1))
            return 0;
        fc_tag[i] = s;
    }
    return fc[i];
}

static void fat_flush(void)
{
    unsigned i;
    for (i = 0; i < NFC; i++)
        if (fc_dirty[i])
            fc_wb(i);
}

static int valid(unsigned long c) { return c >= 2 && c <= maxc; }

static unsigned long fat_get(unsigned long c)
{
    unsigned char *p = fc_get(c >> 7);
    if (!p)
        return BAD;
    return rd32(p + ((unsigned)c & 127) * 4) & MASK;
}

static void fat_put(unsigned long c, unsigned long v)
{
    unsigned char *p = fc_get(c >> 7);
    unsigned o = ((unsigned)c & 127) * 4;
    if (!p) {
        ferr = 1;
        return;
    }
    wr32(p + o, (rd32(p + o) & ~MASK) | (v & MASK));   /* obere 4 Bit bleiben */
    fc_dirty[(unsigned)(c >> 7) & (NFC - 1)] = 1;
    fat_changed = 1;
}

/* ---- Bitmaps in 64-KB-Segmenten ---------------------------------------- */
static unsigned char __far *bm(unsigned *segs, unsigned long c, unsigned char *mask)
{
    *mask = (unsigned char)(1 << ((unsigned)c & 7));
    return (unsigned char __far *)MK_FP(segs[(unsigned)(c >> 19)], (unsigned)(c >> 3) & 0xFFFF);
}
static int getbit(unsigned *s, unsigned long c) { unsigned char m; return (*bm(s, c, &m) & m) != 0; }
static void setbit(unsigned *s, unsigned long c) { unsigned char m, __far *p = bm(s, c, &m); *p |= m; }
static void clrbit(unsigned *s, unsigned long c) { unsigned char m, __far *p = bm(s, c, &m); *p &= (unsigned char)~m; }

static int bm_alloc(unsigned *segs)
{
    unsigned i, k;
    for (i = 0; i < nbm; i++) {
        segs[i] = DosAllocSeg(0);       /* 0 = 64 KB                    */
        if (FAPI_FAILED(segs[i]) || !segs[i])
            return -1;
        for (k = 0; ; k++) {
            ((unsigned char __far *)MK_FP(segs[i], 0))[k] = 0;
            if (k == 0xFFFF)
                break;
        }
    }
    return 0;
}

/* ---- Meldungen ---------------------------------------------------------- */
static void problem(void)
{
    errors++;
    if (!fix && !warned) {
        puts("Fehler gefunden, Parameter F wurde nicht angegeben.\n"
             "Korrekturen werden nicht auf den Datentr\x84ger geschrieben.\n\n");
        warned = 1;
    }
}

static void report(const char *name, const char *msg)
{
    problem();
    puts(pathbuf);
    puts(name);
    puts("\n   ");
    puts(msg);
    puts("\n");
}

static void name83(const unsigned char *e, char *out)
{
    int i, k = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++)
        out[k++] = (char)e[i];
    if (e[8] != ' ') {
        out[k++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++)
            out[k++] = (char)e[i];
    }
    out[k] = 0;
}

static unsigned long ent_first(const unsigned char *e)
{
    return rd16(e + 26) | ((unsigned long)rd16(e + 20) << 16);
}

static void set_first(unsigned char *e, unsigned long c)
{
    wr16(e + 26, (unsigned)c);
    wr16(e + 20, (unsigned)(c >> 16));
}

static unsigned long clus_lba(unsigned long c)
{
    return data_lba + shl(c - 2, shift);
}

/* ---- Ketten ------------------------------------------------------------ */
/* Kette ab first ablaufen und belegen -> Laenge. *badf: erster Cluster
   ungueltig. limit: so viele Cluster braucht die Datei (0xFFFFFFFF bei
   Verzeichnissen). Querverbindungen: mit /F wird die Kette davor beendet. */
static unsigned long walk(const char *name, unsigned long first, int *badf, unsigned long limit)
{
    unsigned long c = first, prev = 0, n = 0, v;

    *badf = 0;
    if (c == 0)
        return 0;
    for (;;) {
        if (!valid(c)) {
            if (!prev) {
                report(name, "Erste Zuordnungseinheit ung\x81ltig, Eintrag abgeschnitten.");
                *badf = 1;
            } else {
                report(name, "Ung\x81ltige Zuordnungseinheit, Datei abgeschnitten.");
                if (fix)
                    fat_put(prev, EOC);
            }
            break;
        }
        if (getbit(useds, c)) {
            report(name, fix && prev ? "Ist querverbunden - Kette davor beendet."
                                     : "Ist querverbunden.");
            if (fix && prev && limit != 0xFFFFFFFFUL)
                fat_put(prev, EOC);
            else if (!prev)
                *badf = limit != 0xFFFFFFFFUL;
            break;
        }
        setbit(useds, c);
        n++;
        prev = c;
        v = fat_get(c);
        if (v >= 0x0FFFFFF8UL)
            break;
        if (v == 0 || v == BAD) {
            report(name, "Ung\x81ltige Zuordnungseinheit, Datei abgeschnitten.");
            if (fix)
                fat_put(c, EOC);
            break;
        }
        c = v;
    }
    return n;
}

/* Kette nach keep Clustern abschneiden, Rest freigeben */
static void cut(unsigned long first, unsigned long keep)
{
    unsigned long c = first, prev = 0, i, v;
    for (i = 0; i < keep && valid(c); i++) {
        prev = c;
        c = fat_get(c);
    }
    if (prev)
        fat_put(prev, EOC);
    while (valid(c) && getbit(useds, c)) {
        v = fat_get(c);
        fat_put(c, 0);
        clrbit(useds, c);
        c = v;
    }
}

/* ---- Verzeichnisse ------------------------------------------------------ */
static struct frame {
    unsigned long first, cur, left;
    unsigned idx, plen;
} stk[MAXDEPTH];
static unsigned depth;
static unsigned long ent_lba;
static unsigned ent_off;

static unsigned char *next_ent(void)
{
    struct frame *f = &stk[depth - 1];
    unsigned long lba;
    unsigned char *e;

    for (;;) {
        if (f->idx >= (spc << 4)) {
            if (--f->left == 0)
                return 0;
            f->cur = fat_get(f->cur);
            f->idx = 0;
        }
        if (!valid(f->cur))
            return 0;
        lba = clus_lba(f->cur) + (f->idx >> 4);
        if (load_dsec(lba))
            return 0;
        ent_lba = lba;
        ent_off = (f->idx & 15) << 5;
        f->idx++;
        e = dsec + ent_off;
        if (e[0] == 0)
            return 0;
        if (e[0] == 0xE5 || e[11] == 0x0F)
            continue;
        if (e[0] == '.' && depth > 1)
            continue;
        return e;
    }
}

static void save_ent(void)
{
    if (fix && wr_sec(ent_lba, dsec))
        puts("Fehler beim Schreiben eines Verzeichnisses.\n");
}

static void check_tree(void)
{
    static char name[14];
    unsigned char *e;
    unsigned long first, n, size, need, csz = shl(1, cshift);
    unsigned k, i;
    int badf;

    n = walk("", rootc, &badf, 0xFFFFFFFFUL);
    if (badf || !n) {
        puts("Stammverzeichnis besch\x84" "digt - Pr\x81" "fung abgebrochen.\n");
        problem();
        return;
    }
    cdirs += n;
    depth = 1;
    stk[0].first = stk[0].cur = rootc;
    stk[0].left = n;
    stk[0].idx = 0;
    stk[0].plen = 3;
    while (depth) {
        e = next_ent();
        if (!e) {
            depth--;
            if (depth)
                pathbuf[stk[depth - 1].plen] = 0;
            continue;
        }
        if (e[11] & 0x08)
            continue;
        name83(e, name);
        first = ent_first(e);
        size = rd32(e + 28);
        if (verbose) {
            puts(pathbuf);
            puts(name);
            puts("\n");
        }
        if (e[11] & 0x10) {
            int seen = valid(first) && getbit(useds, first);
            unsigned long cnt;
            n = walk(name, first, &badf, 0xFFFFFFFFUL);
            cnt = n;
            ndirs++;
            cdirs += n;
            if (seen) {
                report(name, fix ? "Verzeichnis ist querverbunden (Schleife), Eintrag entfernt."
                                 : "Verzeichnis ist querverbunden (Schleife), nicht durchsucht.");
                if (fix) {
                    e[0] = 0xE5;
                    save_ent();
                    ndirs--;
                }
                continue;
            }
            if (badf || !first || !n) {
                report(name, "Ung\x81ltiges Unterverzeichnis.");
                continue;
            }
            if (depth < MAXDEPTH) {
                k = stk[depth - 1].plen;
                for (i = 0; name[i] && k < sizeof(pathbuf) - 2; i++)
                    pathbuf[k++] = name[i];
                pathbuf[k++] = '\\';
                pathbuf[k] = 0;
                stk[depth].first = stk[depth].cur = first;
                stk[depth].idx = 0;
                stk[depth].left = cnt;
                stk[depth].plen = k;
                depth++;
            }
            continue;
        }
        need = shr(size, cshift) + ((size & (csz - 1)) != 0);
        n = walk(name, first, &badf, need);
        if (badf) {
            if (fix) {
                set_first(e, 0);
                e[28] = e[29] = e[30] = e[31] = 0;
                save_ent();
            }
            n = 0;
        } else {
            if (!first)
                need = 0;
            if (n != need) {
                report(name, "Zuordnungsfehler, Gr\x94\xE1" "e angepa\xE1t.");
                if (fix) {
                    if (n > need) {
                        cut(first, need);
                        n = need;
                        if (!need)
                            set_first(e, 0);
                    } else
                        wr32(e + 28, shl(n, cshift));
                    save_ent();
                }
            }
        }
        if (e[11] & 0x06) {
            nhidden++;
            chidden += n;
        } else {
            nfiles++;
            cfiles += n;
        }
    }
}

/* ---- verlorene Ketten -------------------------------------------------- */
/* freier Eintrag in den vorhandenen Clustern des Stamms; 0 = keiner */
static unsigned char *free_root(void)
{
    unsigned long c = rootc, g = 0;
    unsigned i;
    while (valid(c) && g++ <= clusters) {
        for (i = 0; i < (spc << 4); i++) {
            if (load_dsec(clus_lba(c) + (i >> 4)))
                return 0;
            ent_lba = dsec_lba;
            ent_off = (i & 15) << 5;
            if (dsec[ent_off] == 0 || dsec[ent_off] == 0xE5)
                return dsec + ent_off;
        }
        c = fat_get(c);
    }
    return 0;
}

static int chk_exists(const char *n11)
{
    unsigned long c = rootc, g = 0;
    unsigned i, k;
    while (valid(c) && g++ <= clusters) {
        for (i = 0; i < (spc << 4); i++) {
            if (load_dsec(clus_lba(c) + (i >> 4)))
                return 0;
            if (dsec[(i & 15) << 5] == 0)
                return 0;
            for (k = 0; k < 11 && dsec[((i & 15) << 5) + k] == (unsigned char)n11[k]; k++)
                ;
            if (k == 11)
                return 1;
        }
        c = fat_get(c);
    }
    return 0;
}

static int lost_at(unsigned long c, unsigned long *v)
{
    *v = fat_get(c);
    return *v != 0 && *v != BAD && !getbit(useds, c);
}

static void lost_chains(void)
{
    static char n11[12] = "FILE0000CHK";
    static unsigned short dt[2];
    unsigned long c, v, nx, lost = 0, heads = 0, len, made = 0, sz;
    unsigned num = 0, k;
    unsigned char *e;
    int conv;

    for (c = 2; c <= maxc; c++)
        if (lost_at(c, &v)) {
            lost++;
            if (valid(v))
                setbit(auxs, v);
        }
    if (!lost)
        return;
    for (c = 2; c <= maxc; c++)
        if (lost_at(c, &v) && !getbit(auxs, c))
            heads++;
    problem();
    put_num(lost, 0);
    puts(" verlorene Zuordnungseinheiten in ");
    put_num(heads ? heads : 1, 0);
    puts(" Ketten gefunden.\n");
    if (!fix) {
        put_num(shl(lost, cshift), 0);
        puts(" Byte Speicherplatz w\x81rden freigegeben.\n\n");
        return;
    }
    puts("Verlorene Ketten in Dateien umwandeln (J/N)? ");
    conv = yes();
    DosGetDateTime(dt);
    if (conv)
        for (c = 2; c <= maxc; c++) {
            if (!lost_at(c, &v) || getbit(auxs, c))
                continue;
            do {
                n11[4] = (char)('0' + num / 1000 % 10);
                n11[5] = (char)('0' + num / 100 % 10);
                n11[6] = (char)('0' + num / 10 % 10);
                n11[7] = (char)('0' + num % 10);
                num++;
            } while (chk_exists(n11) && num < 10000);
            e = free_root();
            if (!e)
                break;                  /* Stamm voll: Rest freigeben  */
            len = 0;
            v = c;
            for (;;) {
                setbit(useds, v);
                len++;
                nx = fat_get(v);
                if (nx >= 0x0FFFFFF8UL)
                    break;
                if (!valid(nx) || getbit(useds, nx)) {
                    fat_put(v, EOC);
                    break;
                }
                v = nx;
            }
            for (k = 0; k < 11; k++)
                e[k] = (unsigned char)n11[k];
            e[11] = 0x20;
            for (k = 12; k < 32; k++)
                e[k] = 0;
            wr16(e + 22, dt[1]);
            wr16(e + 24, dt[0]);
            set_first(e, c);
            sz = shl(len, cshift);
            wr32(e + 28, sz);
            save_ent();
            made++;
        }
    for (c = 2; c <= maxc; c++)         /* was uebrig ist, freigeben   */
        if (lost_at(c, &v))
            fat_put(c, 0);
    if (made) {
        put_num(made, 0);
        puts(" Datei(en) FILEnnnn.CHK im Stammverzeichnis angelegt.\n");
    }
    puts("\n");
}

/* ---- FAT-Kopien vergleichen (und mit /F angleichen) -------------------- */
static unsigned char cmpb[512];
static int fats_differ;
static void cmp_fats(void)
{
    unsigned long s;
    unsigned i;
    int differ = 0;
    if (nfats < 2)
        return;
    for (s = 0; s < spf; s++) {
        if (rd_secs(fat_lba + s, sec, 1) || rd_secs(fat_lba + spf + s, cmpb, 1))
            break;
        for (i = 0; i < 512 && sec[i] == cmpb[i]; i++)
            ;
        if (i < 512) {
            if (!differ) {
                fats_differ = !fix;
                problem();
                puts("Die Kopien der FAT sind verschieden");
                puts(fix ? " - Kopie wird aus der ersten erneuert.\n" : ".\n");
                differ = 1;
            }
            if (!fix)
                return;
            if (wr_sec(fat_lba + spf + s, sec))
                ferr = 1;
        }
    }
}

/* ---- Bericht ----------------------------------------------------------- */
static void line(unsigned long bytes, const char *text, unsigned long n, const char *what)
{
    put_num(bytes, 14);
    puts(text);
    if (what) {
        put_num(n, 0);
        puts(what);
    }
    puts("\n");
}

/* Byte = Cluster << cshift; ueber 4 GB in KB anzeigen */
static void cline(unsigned long cl, const char *text, unsigned long n, const char *what)
{
    if (cshift < 32 && shr(cl, 32 - cshift - 1) == 0)
        line(shl(cl, cshift), text, n, what);
    else {
        put_num(shl(cl, cshift - 10), 11);
        puts(" KB");
        puts(text + 5);                 /* " Byte" weglassen            */
        if (what) {
            put_num(n, 0);
            puts(what);
        }
        puts("\n");
    }
}

static unsigned long nfree, ffirst;

static void count_free(void)
{
    unsigned long c, v;
    nfree = 0;
    ffirst = 0;
    for (c = 2; c <= maxc; c++) {
        v = fat_get(c);
        if (v == 0) {
            nfree++;
            if (!ffirst)
                ffirst = c;
        } else if (v == BAD)
            nbad++;
    }
}

static void summary(void)
{
    static struct fapi_meminfo mi;

    if (boot[66] == 0x29) {
        puts("Datentr\x84ger-Seriennummer: ");
        put_hex4(rd16(boot + 69));
        putc('-');
        put_hex4(rd16(boot + 67));
        puts("\n");
    }
    puts("\n");
    cline(clusters, " Byte Speicherplatz insgesamt", 0, 0);
    if (nbad)
        cline(nbad, " Byte in fehlerhaften Sektoren", 0, 0);
    if (nhidden)
        cline(chidden, " Byte in ", nhidden, " versteckten Dateien");
    if (ndirs)
        cline(cdirs, " Byte in ", ndirs, " Verzeichnissen");
    cline(cfiles, " Byte in ", nfiles, " Benutzerdateien");
    cline(nfree, " Byte verf\x81gbar", 0, 0);
    puts("\n");
    line(shl(1, cshift), " Byte in jeder Zuordnungseinheit", 0, 0);
    line(clusters, " Zuordnungseinheiten insgesamt", 0, 0);
    line(nfree, " Zuordnungseinheiten verf\x81gbar", 0, 0);
    if (DosMemInfo(&mi) == 0) {
        puts("\n");
        line(shl(mi.total_kb32 ? mi.total_kb32 : mi.total_kb, 10), " Byte Arbeitsspeicher insgesamt", 0, 0);
        line(shl(mi.total_kb32 ? mi.free_kb32 : mi.free_kb, 10), " Byte Arbeitsspeicher frei", 0, 0);
    }
}

/* /F: Sauber-Bit setzen, FSInfo mit dem gezaehlten Stand */
static void mark_clean(void)
{
    unsigned long v = fat_get(1);
    if (!(v & 0x08000000UL))
        fat_put(1, v | 0x08000000UL);
    fat_flush();
    if (fsis && rd_secs(fsis, sec, 1) == 0 && rd32(sec) == 0x41615252UL &&
        rd32(sec + 484) == 0x61417272UL) {
        wr32(sec + 488, nfree);
        wr32(sec + 492, ffirst ? ffirst : 0xFFFFFFFFUL);
        if (wr_sec(fsis, sec))
            ferr = 1;
    }
}

static char flabel[12];
#include "ufat32_fmt.c"
#include "ufat32_scan.c"
#include "ufat32_undel.c"
#include "ufat32_defrag.c"

void main(void)
{
    char *a = args;
    unsigned reserved;
    int fmt = 0, quick = 0, surface = 0, undel = 0, defrag = 0, ulist = 0, uall = 0;
    static char spec[FAPI_PATHLEN];

    drive = DosQCurDisk();
    DosGetArgs(args);
    for (;;) {
        while (*a == ' ')
            a++;
        if (!*a)
            break;
        if (a[1] == ':')
            drive = (unsigned)(up(a[0]) - 'A');
        else if (a[0] == '/' && up(a[1]) == 'F' && up(a[2]) == 'O')
            fmt = 1;
        else if (a[0] == '/' && up(a[1]) == 'Q')
            quick = 1;
        else if (a[0] == '/' && up(a[1]) == 'V' && a[2] == ':') {
            unsigned k;
            for (k = 0; a[3 + k] && a[3 + k] != ' ' && k < 11; k++)
                flabel[k] = up(a[3 + k]);
            flabel[k] = 0;
        } else if (a[0] == '/' && up(a[1]) == 'U' && up(a[2]) == 'N')
            undel = 1;
        else if (a[0] == '/' && up(a[1]) == 'D' && up(a[2]) == 'E')
            defrag = 1;
        else if (a[0] == '/' && up(a[1]) == 'U')
            quick = 1;                  /* DEFRAG /U: nur fragmentierte  */
        else if (a[0] == '/' && up(a[1]) == 'L' && up(a[2]) == 'I')
            ulist = 1;
        else if (a[0] == '/' && up(a[1]) == 'A' && up(a[2]) == 'L')
            uall = 1;
        else if (a[0] == '\\') {
            unsigned k;
            for (k = 0; a[k] && a[k] != ' ' && k < sizeof(spec) - 1; k++)
                spec[k] = a[k];
            spec[k] = 0;
        } else if (a[0] == '/' && up(a[1]) == 'R')
            surface = fix = 1;          /* /R: Oberflaechentest, mit /F  */
        else if (a[0] == '/' && up(a[1]) == 'F')
            fix = 1;
        else if (a[0] == '/' && up(a[1]) == 'V')
            verbose = 1;
        else if (a[0] == '/' && up(a[1]) == 'A')
            autom = 1;
        while (*a && *a != ' ')
            a++;
    }
    if (fmt)
        DosExit(do_format32(quick));
    pathbuf[0] = (char)('A' + drive);
    pathbuf[1] = ':';
    pathbuf[2] = '\\';
    pathbuf[3] = 0;

    if (undel || defrag)
        fix = 1;                        /* schreibt: Laufwerk sperren    */
    hd = DosOpenDrive(drive, (fix ? DRV_OPEN_LOCK : DRV_OPEN_READ) | DRV_OPEN_LBA);
    if (FAPI_FAILED(hd)) {
        puts(hd == FAPI_ERR_INUSE ? "Das Laufwerk ist gesperrt oder in Benutzung.\n"
                                  : "Laufwerk kann nicht ge\x94" "ffnet werden.\n");
        DosExit(3);
    }
    if (rd_secs(0, sec, 1) || rd16(sec + 11) != 512 || !sec[13] || !sec[16] ||
        rd16(sec + 22) != 0 || rd16(sec + 17) != 0) {
        puts("Bootsektor nicht lesbar oder kein FAT32-Dateisystem.\n");
        DosClose(hd);
        DosExit(3);
    }
    for (reserved = 0; reserved < sizeof(boot); reserved++)
        boot[reserved] = sec[reserved];
    spc = sec[13];
    for (shift = 0; shift < 8 && (1U << shift) < spc; shift++)
        ;
    cshift = shift + 9;
    reserved = rd16(sec + 14);
    nfats = sec[16];
    total = rd16(sec + 19) ? rd16(sec + 19) : rd32(sec + 32);
    spf = rd32(sec + 36);
    extf = rd16(sec + 40);
    rootc = rd32(sec + 44);
    fsis = rd16(sec + 48);
    fat_lba = reserved;
    data_lba = fat_lba + (nfats == 2 ? spf + spf : spf);
    if (extf & 0x80) {                  /* Spiegeln aus: nur aktive FAT */
        if ((extf & 15) == 1)
            fat_lba += spf;
        nfats = 1;
    }
    clusters = total > data_lba ? shr(total - data_lba, shift) : 0;
    maxc = clusters + 1;
    if ((1U << shift) != spc || !spf || nfats > 2 || !clusters ||
        clusters > 0x0FFFFFF4UL || !valid(rootc)) {
        puts("Ung\x81ltiger Bootsektor.\n");
        DosClose(hd);
        DosExit(3);
    }
    nbm = (unsigned)((maxc >> 19) + 1);
    if (nbm > NBMSEG || bm_alloc(useds) || (!undel && !defrag && bm_alloc(auxs))) {
        puts("Zu wenig Arbeitsspeicher f\x81r die Pr\x81" "fung.\n");
        DosClose(hd);
        DosExit(3);
    }
    fc_init();
    if (undel || defrag) {
        int rc;
        fix = 0;                        /* cmp_fats nur pruefen          */
        if (defrag)
            cmp_fats();
        rc = undel ? do_undelete(spec[0] ? spec : "\\*.*", ulist, uall)
                   : do_defrag(quick);
        fat_flush();
        DosClose(hd);
        DosExit(rc);
    }
    cmp_fats();
    check_tree();
    lost_chains();
    if (surface)
        surface_scan();
    count_free();
    if (fix)
        mark_clean();
    else
        fat_flush();
    if (ferr)
        puts("Fehler beim Schreiben der FAT.\n");
    if (errors == 0)
        puts("Keine Fehler gefunden.\n");
    summary();
    DosClose(hd);
    DosExit(errors == 0 ? 0 : fix ? 1 : 3);
}
