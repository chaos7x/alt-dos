/*
 * UFAT.EXE - Pruefprogramm fuer FAT12/FAT16, gestartet von CHKDSK.EXE
 *
 *   UFAT Laufwerk: [/F] [/V] [/AUTO]
 *
 * Liest das Laufwerk als Ganzes (DosOpenDrive), ohne das Dateisystem des
 * Kernels. Die FAT liegt ganz im Speicher (bis 128 KB in zwei Segmenten),
 * dazu zwei Bitmaps (je Cluster ein Bit). Geprueft wird wie bei MS-DOS
 * CHKDSK:
 *   - beide FAT-Kopien gleich
 *   - jede Kette: ungueltige Cluster, Querverbindungen, Laenge passt zur
 *     Dateigroesse ("Zuordnungsfehler")
 *   - Unterverzeichnisse rekursiv (ohne Rekursion im C-Stack: eigener Stapel)
 *   - verlorene Cluster (belegt, aber von keiner Datei erreicht)
 * Mit /F wird repariert: Ketten abschneiden, Groessen anpassen, verlorene
 * Ketten als FILEnnnn.CHK im Stammverzeichnis, FAT in alle Kopien.
 * Querverbindungen werden nur gemeldet (wie MS-DOS CHKDSK).
 * /AUTO (beim Start aus AUTOCHK.LST): alle Fragen mit Ja beantworten.
 */
#include <i86.h>
#include "fapi.h"

#define EOC     0xFFFF
#define BAD     0xFFF7
#define MAXDEPTH 24

static unsigned hd, drive, fix, verbose, autom, errors, warned;
static unsigned spc, shift, cshift, nfats, rootents, spf, clusters, maxc, fat16;
static unsigned long total, fat_lba, root_lba, data_lba;
static unsigned fseg[2];
static unsigned char fdirty[32];        /* geaenderte FAT-Sektoren (bis 256) */
static int fat_changed, fats_differ;
static unsigned char used[8192], aux[8192];
static unsigned char sec[512], dsec[512], big[4096], boot[64];
static unsigned long dsec_lba = 0xFFFFFFFFUL;
static char pathbuf[200], args[FAPI_ARGLEN];

/* Zaehler fuer den Bericht */
static unsigned nfiles, nhidden, ndirs, nbad;
static unsigned long cfiles, chidden, cdirs; /* Cluster */

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

/* Zahl mit Tausenderpunkten, rechtsbuendig in w Stellen */
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

/* ---- Sektoren ---------------------------------------------------------- */
static int rd_secs(unsigned long lba, void *buf, unsigned n)
{
    static unsigned long pos;
    pos = lba << 9;
    if (DosSeek(hd, &pos))
        return -1;
    return DosRead(hd, buf, n << 9) == (n << 9) ? 0 : -1;
}

static int wr_sec(unsigned long lba, void *buf)
{
    static unsigned long pos;
    pos = lba << 9;
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

/* ---- FAT im Speicher --------------------------------------------------- */
static unsigned char __far *fatb(unsigned long off)
{
    return (unsigned char __far *)MK_FP(fseg[(unsigned)(off >> 16)], (unsigned)off);
}

static int valid(unsigned c) { return c >= 2 && c <= maxc; }

static unsigned fat_get(unsigned c)
{
    unsigned char __far *p;
    unsigned o, v;
    if (fat16) {
        p = fatb((unsigned long)c << 1);
        return p[0] | ((unsigned)p[1] << 8);
    }
    o = c + (c >> 1);
    p = fatb(o);
    v = p[0] | ((unsigned)p[1] << 8);
    v = (c & 1) ? v >> 4 : v & 0xFFF;
    if (v >= 0xFF0)
        v |= 0xF000;
    return v;
}

static void fat_put(unsigned c, unsigned v)
{
    unsigned char __far *p;
    unsigned o;
    if (fat16) {
        p = fatb((unsigned long)c << 1);
        p[0] = (unsigned char)v;
        p[1] = (unsigned char)(v >> 8);
        o = c >> 8;
        fdirty[o >> 3] |= (unsigned char)(1 << (o & 7));
    } else {
        v &= 0xFFF;
        o = c + (c >> 1);
        p = fatb(o);
        if (c & 1) {
            p[0] = (unsigned char)((p[0] & 0x0F) | ((v & 0x0F) << 4));
            p[1] = (unsigned char)(v >> 4);
        } else {
            p[0] = (unsigned char)v;
            p[1] = (unsigned char)((p[1] & 0xF0) | (v >> 8));
        }
        fdirty[(o >> 9) >> 3] |= (unsigned char)(1 << ((o >> 9) & 7));
        o++;
        fdirty[(o >> 9) >> 3] |= (unsigned char)(1 << ((o >> 9) & 7));
    }
    fat_changed = 1;
}

static int getbit(unsigned char *m, unsigned c) { return m[c >> 3] & (1 << (c & 7)); }
static void setbit(unsigned char *m, unsigned c) { m[c >> 3] |= (unsigned char)(1 << (c & 7)); }
static void clrbit(unsigned char *m, unsigned c) { m[c >> 3] &= (unsigned char)~(1 << (c & 7)); }

static void fcopy(unsigned char __far *d, const unsigned char __far *s, unsigned n)
{
    while (n--)
        *d++ = *s++;
}

/* Anfang der FAT-Kopie f relativ zur ersten (ohne 32-Bit-Multiplikation) */
static unsigned long fbase(unsigned f)
{
    unsigned long b = 0;
    while (f--)
        b += spf;
    return b;
}

/* FAT 1 laden, FAT 2.. vergleichen */
static int load_fat(void)
{
    unsigned s, n, k, f;
    unsigned char __far *p;

    fseg[0] = DosAllocSeg(0);
    fseg[1] = spf > 128 ? DosAllocSeg(0) : fseg[0];
    if (FAPI_FAILED(fseg[0]) || FAPI_FAILED(fseg[1])) {
        puts("Zu wenig Arbeitsspeicher f\x81r die FAT.\n");
        return -1;
    }
    for (f = 0; f < nfats; f++)
        for (s = 0; s < spf; s += n) {
            n = spf - s < 8 ? spf - s : 8;
            if (rd_secs(fat_lba + s + fbase(f), big, n)) {
                if (f == 0) {
                    puts("FAT nicht lesbar.\n");
                    return -1;
                }
                fats_differ = 1;
                break;
            }
            p = fatb((unsigned long)s << 9);
            if (f == 0) {
                fcopy(p, (unsigned char __far *)big, n << 9);
            } else if (!fats_differ) {
                for (k = 0; k < (n << 9); k++)
                    if (p[k] != big[k]) {
                        fats_differ = 1;
                        break;
                    }
            }
        }
    return 0;
}

/* FAT in alle Kopien zurueckschreiben (geaenderte Sektoren, bei
   abweichenden Kopien alles) */
static void write_fat(void)
{
    unsigned s, f;
    for (f = 0; f < nfats; f++)
        for (s = 0; s < spf; s++) {
            if (!(fdirty[s >> 3] & (1 << (s & 7))) && !(fats_differ && f > 0))
                continue;
            fcopy((unsigned char __far *)sec, fatb((unsigned long)s << 9), 512);
            if (wr_sec(fat_lba + s + fbase(f), sec))
                puts("Fehler beim Schreiben der FAT.\n");
        }
}

/* ---- Meldungen --------------------------------------------------------- */
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

/* 8.3 aus dem Eintrag -> "NAME.EXT" */
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

/* ---- Ketten ------------------------------------------------------------ */
static unsigned free_hint = 2;

/* freien Cluster suchen (FAT 0) -> Nummer, 0 = Platte voll */
static unsigned alloc_free(void)
{
    unsigned c = free_hint, i;
    for (i = 0; i < clusters; i++, c++) {
        if (!valid(c))
            c = 2;
        if (fat_get(c) == 0 && !getbit(used, c)) {
            free_hint = c + 1;
            return c;
        }
    }
    return 0;
}

static unsigned long clus_lba(unsigned c)
{
    return data_lba + shl((unsigned long)(c - 2), shift);
}

/* Inhalt eines Clusters kopieren; 0 = ok */
static int copy_clus(unsigned from, unsigned to)
{
    unsigned s;
    for (s = 0; s < spc; s++)
        if (rd_secs(clus_lba(from) + s, sec, 1) || wr_sec(clus_lba(to) + s, sec))
            return -1;
    return 0;
}

static void num_msg(char *m, const char *a, unsigned x, const char *b)
{
    unsigned k = 0, d = 10000;
    while (*a)
        m[k++] = *a++;
    while (d > 1 && x < d)
        d /= 10;
    for (; d; d /= 10)
        m[k++] = (char)('0' + x / d % 10);
    while (*b)
        m[k++] = *b++;
    m[k] = 0;
}

/* Kette ab *first ablaufen und belegen; -> Laenge. *bad_first: erster
   Cluster ungueltig (Eintrag muss auf 0). limit: so viele Cluster braucht
   die Datei (FFFFh: Verzeichnis). Querverbindung mit /F bei Dateien: der
   gemeinsame Rest wird in freie Cluster kopiert, wie ScanDisk - beide
   Dateien behalten ihren Inhalt; *first kann sich dabei aendern. */
static unsigned walk(const char *name, unsigned *first, int *bad_first, unsigned limit)
{
    static char m[80];
    unsigned c = *first, prev = 0, n = 0, v, nc;

    *bad_first = 0;
    if (c == 0)
        return 0;
    for (;;) {
        if (!valid(c)) {
            if (!prev) {
                report(name, "Erste Zuordnungseinheit ung\x81ltig, Eintrag abgeschnitten.");
                *bad_first = 1;
            } else {
                report(name, "Ung\x81ltige Zuordnungseinheit, Datei abgeschnitten.");
                if (fix)
                    fat_put(prev, EOC);
            }
            break;
        }
        if (getbit(used, c)) {
            if (!fix || limit == 0xFFFF) {
                num_msg(m, "Ist mit Zuordnungseinheit ", c, " querverbunden.");
                report(name, m);
                break;
            }
            num_msg(m, "Ist mit Zuordnungseinheit ", c, " querverbunden - Kopie angelegt.");
            report(name, m);
            while (n < limit) {             /* gemeinsamen Rest kopieren */
                nc = alloc_free();
                if (!nc || copy_clus(c, nc)) {
                    report(name, "Kein Platz f\x81r die Kopie, Datei abgeschnitten.");
                    break;
                }
                setbit(used, nc);
                fat_put(nc, EOC);
                if (prev)
                    fat_put(prev, nc);
                else
                    *first = nc;
                prev = nc;
                n++;
                v = fat_get(c);
                if (v >= 0xFFF8 || !valid(v))
                    break;
                c = v;
            }
            if (prev)
                fat_put(prev, EOC);
            else
                *first = 0;                 /* nichts uebrig: leere Datei */
            break;
        }
        setbit(used, c);
        n++;
        prev = c;
        v = fat_get(c);
        if (v >= 0xFFF8)
            break;
        if (v == 0) {                   /* Kette endet im Freien        */
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
static void cut(unsigned first, unsigned keep)
{
    unsigned c = first, prev = 0, i, v;
    for (i = 0; i < keep && valid(c); i++) {
        prev = c;
        c = fat_get(c);
    }
    if (prev)
        fat_put(prev, EOC);
    while (valid(c) && getbit(used, c)) {
        v = fat_get(c);
        fat_put(c, 0);
        clrbit(used, c);
        c = v;
    }
}

/* ---- Verzeichnisse ------------------------------------------------------ */
static struct frame {
    unsigned first, cur, idx, plen;
} stk[MAXDEPTH];
static unsigned depth;
static unsigned long ent_lba;
static unsigned ent_off;

/* naechster Eintrag des obersten Verzeichnisses in dsec; 0 = zu Ende */
static unsigned char *next_ent(void)
{
    struct frame *f = &stk[depth - 1];
    unsigned long lba;
    unsigned char *e;

    for (;;) {
        if (f->first == 0) {            /* Stammverzeichnis            */
            if (f->idx >= rootents)
                return 0;
            lba = root_lba + (f->idx >> 4);
        } else {
            if (f->idx >= (spc << 4)) {
                f->cur = fat_get(f->cur);
                f->idx = 0;
            }
            if (!valid(f->cur) || !getbit(used, f->cur))
                return 0;
            lba = data_lba + shl((unsigned long)(f->cur - 2), shift) + (f->idx >> 4);
        }
        if (load_dsec(lba))
            return 0;
        ent_lba = lba;
        ent_off = (f->idx & 15) << 5;
        f->idx++;
        e = dsec + ent_off;
        if (e[0] == 0)
            return 0;                   /* Ende des Verzeichnisses     */
        if (e[0] == 0xE5 || e[11] == 0x0F)
            continue;                   /* geloescht, lange Namen      */
        if (e[0] == '.' && f->first)
            continue;                   /* "." und ".."                */
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
    unsigned first, n, k;
    unsigned long size, need;
    int badf;

    depth = 1;
    stk[0].first = 0;
    stk[0].idx = 0;
    stk[0].plen = 3;
    while (depth) {
        e = next_ent();
        if (!e) {
            pathbuf[stk[depth - 1].plen] = 0;
            depth--;
            if (depth)
                pathbuf[stk[depth - 1].plen] = 0;
            continue;
        }
        if (e[11] & 0x08)
            continue;                   /* Datentraegerbezeichnung     */
        name83(e, name);
        first = rd16(e + 26);
        size = rd32(e + 28);
        if (verbose) {
            puts(pathbuf);
            puts(name);
            puts("\n");
        }
        if (e[11] & 0x10) {             /* Unterverzeichnis            */
            n = walk(name, &first, &badf, 0xFFFF);
            ndirs++;
            cdirs += n;
            if (badf || !first) {
                if (!first)
                    report(name, "Ung\x81ltiges Unterverzeichnis.");
                continue;
            }
            if (depth < MAXDEPTH) {
                k = stk[depth - 1].plen;
                for (n = 0; name[n] && k < sizeof(pathbuf) - 2; n++)
                    pathbuf[k++] = name[n];
                pathbuf[k++] = '\\';
                pathbuf[k] = 0;
                stk[depth].first = first;
                stk[depth].cur = first;
                stk[depth].idx = 0;
                stk[depth].plen = k;
                depth++;
            }
            continue;
        }
        need = shr(size + shl(1, cshift) - 1, cshift);
        k = first;
        n = walk(name, &first, &badf, need > 0xFFFEUL ? 0xFFFE : (unsigned)need);
        if (first != k && fix) {        /* Querverbindung: neue Kette   */
            wr16(e + 26, first);
            if (!first)
                e[28] = e[29] = e[30] = e[31] = 0;
            save_ent();
        }
        if (badf) {
            if (fix) {
                wr16(e + 26, 0);
                e[28] = e[29] = e[30] = e[31] = 0;
                save_ent();
            }
            n = 0;
        } else {
            if (!first)
                need = 0;
            if ((unsigned long)n != need) {
                report(name, "Zuordnungsfehler, Gr\x94\xE1" "e angepa\xE1t.");
                if (fix) {
                    if ((unsigned long)n > need) {
                        cut(first, (unsigned)need);
                        n = (unsigned)need;
                        if (!need)
                            wr16(e + 26, 0);
                    } else {
                        size = shl((unsigned long)n, cshift);
                        e[28] = (unsigned char)size;
                        e[29] = (unsigned char)(size >> 8);
                        e[30] = (unsigned char)(size >> 16);
                        e[31] = (unsigned char)(size >> 24);
                    }
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
/* freier Eintrag im Stammverzeichnis -> dsec/ent_*; 0 = keiner */
static unsigned char *free_root(void)
{
    unsigned i;
    for (i = 0; i < rootents; i++) {
        if (load_dsec(root_lba + (i >> 4)))
            return 0;
        ent_lba = dsec_lba;
        ent_off = (i & 15) << 5;
        if (dsec[ent_off] == 0 || dsec[ent_off] == 0xE5)
            return dsec + ent_off;
    }
    return 0;
}

/* gibt es FILEnnnn.CHK schon? */
static int chk_exists(const char *n11)
{
    unsigned i, k;
    for (i = 0; i < rootents; i++) {
        if (load_dsec(root_lba + (i >> 4)))
            return 0;
        if (dsec[(i & 15) << 5] == 0)
            return 0;
        for (k = 0; k < 11 && dsec[((i & 15) << 5) + k] == (unsigned char)n11[k]; k++)
            ;
        if (k == 11)
            return 1;
    }
    return 0;
}

static void lost_chains(void)
{
    static char n11[12] = "FILE0000CHK";
    static unsigned short dt[2];
    unsigned c, v, lost = 0, heads = 0, num = 0, len, made = 0;
    unsigned char *e;
    unsigned long sz;
    int conv;

    for (c = 2; ; c++) {
        v = fat_get(c);
        if (v != 0 && v != BAD && !getbit(used, c)) {
            lost++;
            if (valid(v))
                setbit(aux, v);
        }
        if (c == maxc)
            break;
    }
    if (!lost)
        return;
    for (c = 2; ; c++) {
        v = fat_get(c);
        if (v != 0 && v != BAD && !getbit(used, c) && !getbit(aux, c))
            heads++;
        if (c == maxc)
            break;
    }
    problem();
    put_num(lost, 0);
    puts(" verlorene Zuordnungseinheiten in ");
    put_num(heads ? heads : 1, 0);
    puts(" Ketten gefunden.\n");
    if (!fix) {
        put_num(shl((unsigned long)lost, cshift), 0);
        puts(" Byte Speicherplatz w\x81rden freigegeben.\n\n");
        return;
    }
    puts("Verlorene Ketten in Dateien umwandeln (J/N)? ");
    conv = yes();
    DosGetDateTime(dt);
    if (conv)
        for (c = 2; ; c++) {
            v = fat_get(c);
            if (v != 0 && v != BAD && !getbit(used, c) && !getbit(aux, c)) {
                do {                    /* naechster freie Name        */
                    n11[4] = (char)('0' + num / 1000 % 10);
                    n11[5] = (char)('0' + num / 100 % 10);
                    n11[6] = (char)('0' + num / 10 % 10);
                    n11[7] = (char)('0' + num % 10);
                    num++;
                } while (chk_exists(n11) && num < 10000);
                e = free_root();
                if (!e)
                    break;              /* Stamm voll: Rest freigeben  */
                len = 0;
                v = c;
                for (;;) {              /* Kette belegen               */
                    unsigned nx;
                    setbit(used, v);
                    len++;
                    nx = fat_get(v);
                    if (nx >= 0xFFF8)
                        break;
                    if (!valid(nx) || getbit(used, nx) || nx == 0 || nx == BAD) {
                        fat_put(v, EOC);
                        break;
                    }
                    v = nx;
                }
                for (v = 0; v < 11; v++)
                    e[v] = (unsigned char)n11[v];
                e[11] = 0x20;
                for (v = 12; v < 32; v++)
                    e[v] = 0;
                wr16(e + 22, dt[1]);
                wr16(e + 24, dt[0]);
                wr16(e + 26, c);
                sz = shl((unsigned long)len, cshift);
                e[28] = (unsigned char)sz;
                e[29] = (unsigned char)(sz >> 8);
                e[30] = (unsigned char)(sz >> 16);
                e[31] = (unsigned char)(sz >> 24);
                save_ent();
                made++;
            }
            if (c == maxc)
                break;
        }
    for (c = 2; ; c++) {                /* was uebrig ist, freigeben   */
        v = fat_get(c);
        if (v != 0 && v != BAD && !getbit(used, c))
            fat_put(c, 0);
        if (c == maxc)
            break;
    }
    if (made) {
        put_num(made, 0);
        puts(" Datei(en) FILEnnnn.CHK im Stammverzeichnis angelegt.\n");
    }
    puts("\n");
}

/* ---- Bericht ----------------------------------------------------------- */
static void line(unsigned long bytes, const char *text, unsigned n, const char *what)
{
    put_num(bytes, 14);
    puts(text);
    if (what) {
        put_num(n, 0);
        puts(what);
    }
    puts("\n");
}

static void summary(const unsigned char *bs)
{
    static struct { unsigned short tot, fre, k, img, al; } mi;
    unsigned c, nfree = 0;
    unsigned long csz = shl(1, cshift);

    for (c = 2; ; c++) {
        unsigned v = fat_get(c);
        if (v == 0)
            nfree++;
        else if (v == BAD)
            nbad++;
        if (c == maxc)
            break;
    }
    if (bs[38] == 0x29) {
        puts("Datentr\x84ger-Seriennummer: ");
        put_hex4(rd16(bs + 41));
        putc('-');
        put_hex4(rd16(bs + 39));
        puts("\n");
    }
    puts("\n");
    line(shl((unsigned long)clusters, cshift), " Byte Speicherplatz insgesamt", 0, 0);
    if (nbad)
        line(shl((unsigned long)nbad, cshift), " Byte in fehlerhaften Sektoren", 0, 0);
    if (nhidden)
        line(shl(chidden, cshift), " Byte in ", nhidden, " versteckten Dateien");
    if (ndirs)
        line(shl(cdirs, cshift), " Byte in ", ndirs, " Verzeichnissen");
    line(shl(cfiles, cshift), " Byte in ", nfiles, " Benutzerdateien");
    line(shl((unsigned long)nfree, cshift), " Byte verf\x81gbar", 0, 0);
    puts("\n");
    line(csz, " Byte in jeder Zuordnungseinheit", 0, 0);
    line(clusters, " Zuordnungseinheiten insgesamt", 0, 0);
    line(nfree, " Zuordnungseinheiten verf\x81gbar", 0, 0);
    if (DosMemInfo(&mi) == 0) {
        puts("\n");
        line(shl((unsigned long)mi.tot, 10), " Byte Arbeitsspeicher insgesamt", 0, 0);
        line(shl((unsigned long)mi.fre, 10), " Byte Arbeitsspeicher frei", 0, 0);
    }
}

void main(void)
{
    char *a = args;
    unsigned reserved;
    unsigned long rootsecs, fatsecs;

    drive = DosQCurDisk();
    DosGetArgs(args);
    for (;;) {
        while (*a == ' ')
            a++;
        if (!*a)
            break;
        if (a[1] == ':')
            drive = (unsigned)(up(a[0]) - 'A');
        else if (a[0] == '/' && up(a[1]) == 'F')
            fix = 1;
        else if (a[0] == '/' && up(a[1]) == 'V')
            verbose = 1;
        else if (a[0] == '/' && up(a[1]) == 'A')
            autom = 1;
        while (*a && *a != ' ')
            a++;
    }
    pathbuf[0] = (char)('A' + drive);
    pathbuf[1] = ':';
    pathbuf[2] = '\\';
    pathbuf[3] = 0;

    hd = DosOpenDrive(drive, fix ? DRV_OPEN_LOCK : DRV_OPEN_READ);
    if (FAPI_FAILED(hd)) {
        puts(hd == FAPI_ERR_INUSE ? "Das Laufwerk ist gesperrt oder in Benutzung.\n"
                                  : "Laufwerk kann nicht ge\x94" "ffnet werden.\n");
        DosExit(3);
    }
    if (rd_secs(0, sec, 1) || rd16(sec + 11) != 512 || !sec[13] || !sec[16]) {
        puts("Bootsektor nicht lesbar oder kein FAT-Dateisystem.\n");
        DosClose(hd);
        DosExit(3);
    }
    for (reserved = 0; reserved < sizeof(boot); reserved++)
        boot[reserved] = sec[reserved];     /* sec dient spaeter als Puffer */
    spc = sec[13];
    for (shift = 0; (1U << shift) < spc; shift++)
        ;
    cshift = shift + 9;
    reserved = rd16(sec + 14);
    nfats = sec[16];
    rootents = rd16(sec + 17);
    total = rd16(sec + 19) ? rd16(sec + 19) : rd32(sec + 32);
    spf = rd16(sec + 22);
    fat_lba = reserved;
    fatsecs = fbase(nfats);
    root_lba = fat_lba + fatsecs;
    rootsecs = (((unsigned long)rootents << 5) + 511) >> 9;
    data_lba = root_lba + rootsecs;
    clusters = (unsigned)shr(total - data_lba, shift);
    maxc = clusters + 1;
    fat16 = clusters >= 4085;
    if (spf == 0 || spf > 256 || (1U << shift) != spc) {
        puts("Ung\x81ltiger Bootsektor.\n");
        DosClose(hd);
        DosExit(3);
    }
    if (load_fat()) {
        DosClose(hd);
        DosExit(3);
    }
    if (fats_differ) {
        problem();
        puts("Die Kopien der FAT sind verschieden");
        puts(fix ? " - Kopie wird aus der ersten erneuert.\n" : ".\n");
    }
    check_tree();
    lost_chains();
    if (fix && fat16) {                 /* geprueft: wieder "sauber"   */
        unsigned char __far *p1 = fatb(2);
        if (!(p1[1] & 0x80)) {
            p1[1] |= 0x80;
            fdirty[0] |= 1;
            fat_changed = 1;
        }
    }
    if (fix && (fat_changed || fats_differ))
        write_fat();
    if (errors == 0)
        puts("Keine Fehler gefunden.\n");
    summary(boot);
    DosClose(hd);                       /* gibt die Sperre frei         */
    DosExit(errors == 0 ? 0 : fix ? 1 : 3);     /* wie CHKDSK von NT */
}
