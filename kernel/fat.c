/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - IFS-Treiber FAT (12 und 16 Bit) und IFS-Verteiler
 *
 * Seit 6.0 im zweiten Codesegment (KTEXT2, Platz fuer FAT32): Einspruenge
 * fern, Kernelhilfen ueber k2.h.
 *
 * Wie bei OS/2 steckt FAT fest im Kernel. Ein Laufwerk ist eine Partition
 * (oder eine Diskette ab Sektor 0) auf einem Blockgeraet. Sektornummern
 * sind 32 Bit breit, Multiplikationen mit der Clustergroesse laufen ueber
 * Schiebeoperationen, damit Watcom keine 32-Bit-Hilfsroutinen braucht.
 *
 * Schreiben: Daten und Verzeichniseintraege gehen sofort auf den
 * Datentraeger. Nur die FAT liegt waehrend einer Operation im Cache
 * (2 Sektoren) und wird am Ende jeder Operation in alle FAT-Kopien
 * geschrieben (fat_flush). Nach jedem FAPI-Aufruf ist das Dateisystem
 * damit in sich stimmig; die Dateigroesse im Verzeichnis kommt wie bei
 * MS-DOS erst beim Schliessen.
 *
 * Achtung DMABUF: Zeiger auf Verzeichniseintraege (dir_entry) zeigen in den
 * gemeinsamen Sektorpuffer und gelten nur bis zum naechsten Plattenzugriff.
 */
#include "kern.h"
#include "ifs.h"
#include "mem.h"
#include "k2.h"                         /* zuletzt: lenkt Kernelhilfen um */

struct volume vol[NDRIVE];
char ifs_orig[IFS_NAMELEN];             /* siehe ifs.h                    */
unsigned char lfn_cfg;                  /* LONGNAMES=                     */

static unsigned char __far *dma(void)
{
    return MK_FP(DMABUF, 0);
}

static unsigned rd16(unsigned off)
{
    unsigned char __far *b = dma();
    return b[off] | ((unsigned)b[off + 1] << 8);
}

static unsigned long rd32(unsigned off)
{
    return rd16(off) | ((unsigned long)rd16(off + 2) << 16);
}

static void put32(unsigned char __far *b, unsigned o, unsigned long v)
{
    b[o] = (unsigned char)v;
    b[o + 1] = (unsigned char)(v >> 8);
    b[o + 2] = (unsigned char)(v >> 16);
    b[o + 3] = (unsigned char)(v >> 24);
}

static void wr(int d, unsigned long lba);

static unsigned long shl(unsigned long v, unsigned n)
{
    while (n--)
        v <<= 1;
    return v;
}

static unsigned long shr(unsigned long v, unsigned n)
{
    while (n--)
        v >>= 1;
    return v;
}

/* ---- FAT: zwei aufeinanderfolgende Sektoren im Cache, weil ein
        12-Bit-Eintrag ueber die Sektorgrenze reichen kann; Aenderungen
        bleiben bis fat_flush() im Cache ------------------------------------ */
/* Clusternummern sind 32 Bit (FAT32: 28 Bit). vol[].fat16: 0 FAT12,
   1 FAT16, 2 FAT32; bei FAT32 steht in root_lba der Stammcluster und in
   rootents der FSInfo-Sektor. Verzeichnis-Cluster 0 heisst immer Stamm. */
typedef unsigned long clus_t;
#define FAT_END  0xFFFFFFFFUL
#define NO_DC    0xFFFFFFFFUL           /* Stamm hat keinen Eintrag       */
#define IS32(v)  ((v)->fat16 == 2)
#define EOC(v)   (IS32(v) ? 0x0FFFFFFFUL : (v)->fat16 ? 0xFFFFUL : 0x0FFFUL)
#define ROOTC(v) ((v)->root_lba)
/* FAT-Cache: 1 KB bei linear 8800h (eigener Selektor FCACHE_SEL) statt in
   DGROUP; gemeinsam fuer alle Tasks, anders als DMABUF */
#define fcache   ((unsigned char __far *)MK_FP(FCACHE_SEL, 0))
/* Phase 6: der FAT-Cache gehoert dem Task (FCACHE_SEL wird wie DMABUF beim
   Taskwechsel umgelegt), die Kennung steht mit darin (hinter den 1 KB) -
   so kann ein Task in fat_load schlafen, waehrend ein anderer seine FAT
   liest. Beim Nehmen einer Laufwerkssperre verwirft fat_op_begin ihn
   (ein anderer Task kann die FAT inzwischen geaendert haben). */
struct fct { int drive; unsigned long sec; unsigned char dirty; };
#define FCT      ((struct fct __far *)MK_FP(FCACHE_SEL, 1024))
#define fc_drive (FCT->drive)
#define fc_sec   (FCT->sec)            /* Sektor relativ zum FAT-Anfang  */
#define fc_dirty (FCT->dirty)
static int           ferr;              /* erster Schreibfehler der Operation */

/* geaenderte FAT-Sektoren in alle Kopien schreiben */
static int fat_flush_fat(void)
{
    struct volume *v;
    unsigned long lba;
    unsigned k, i;
    int rc;

    if (fc_drive < 0 || !fc_dirty)
        return 0;
    v = &vol[fc_drive];
    fc_dirty = 0;
    for (k = 0, lba = v->fat_lba + fc_sec; k < v->nfats; k++, lba += v->spf)
        for (i = 0; i < 2 && fc_sec + i < v->spf; i++) {
            far_copy(DMABUF, 0, FCACHE_SEL, i << 9, 512);
            rc = blk_write(v->blk, lba + i);
            if (rc) {
                if (!ferr)
                    ferr = rc == -2 ? IFS_WP : IFS_ERR;
                fc_drive = -1;          /* Cache stimmt nicht mehr      */
                return -1;
            }
        }
    return 0;
}

/* neue Laufwerkssperre (kernel.c vol_lock): eigenen FAT-Cache verwerfen */
void __far fat_op_begin(void)
{
    if (fc_drive >= 0 && fc_dirty)
        fat_flush_fat();                /* sollte nie vorkommen          */
    fc_drive = -1;
}

/* ... und den Spurpuffer der Diskette: nach jeder Operation stimmig */
static int fat_flush(void)
{
    int rc = fat_flush_fat(), r2 = fd_sync();
    if (r2) {
        if (!ferr)
            ferr = r2 == -2 ? IFS_WP : IFS_ERR;
        rc = -1;
    }
    return rc;
}

static int fat_load(int d, unsigned long sec)
{
    struct volume *v = &vol[d];
    if (fc_drive == d && fc_sec == sec)
        return 0;
    if (fat_flush())
        return -1;
    fc_drive = -1;
    if (blk_read(v->blk, v->fat_lba + sec))
        return -1;
    far_copy(FCACHE_SEL, 0, DMABUF, 0, 512);
    if (sec + 1 < v->spf) {
        if (blk_read(v->blk, v->fat_lba + sec + 1))
            return -1;
        far_copy(FCACHE_SEL, 512, DMABUF, 0, 512);
    }
    fc_drive = d;
    fc_sec = sec;
    return 0;
}

static int valid(struct volume *v, clus_t c)
{
    return c >= 2 && c < v->clusters + 2;
}

/* Lage eines Eintrags: Sektor relativ zur FAT, Byte im Cache */
static void fat_pos(struct volume *v, clus_t c, unsigned long *sec, unsigned *off)
{
    unsigned long o;
    if (IS32(v)) {
        *sec = c >> 7;
        *off = ((unsigned)c & 127) * 4;
    } else if (v->fat16) {
        *sec = c >> 8;
        *off = (c & 255) * 2;
    } else {
        o = c + (c >> 1);
        *sec = o >> 9;
        *off = (unsigned)o & 511;
    }
}

/* roher FAT-Eintrag; bei Lesefehler ein "defekt"-Wert, nie 0 */
static clus_t fat_raw(int d, clus_t c)
{
    struct volume *v = &vol[d];
    unsigned long sec;
    unsigned off, val;

    fat_pos(v, c, &sec, &off);
    if (fat_load(d, sec))
        return 0x0FFFFFF7UL;            /* nie gueltig, nie frei          */
    val = fcache[off] | ((unsigned)fcache[off + 1] << 8);
    if (IS32(v))
        return val | ((unsigned long)(fcache[off + 2] |
                     ((unsigned)(fcache[off + 3] & 0x0F) << 8)) << 16);
    if (!v->fat16)
        val = (c & 1) ? val >> 4 : val & 0xFFF;
    return val;
}

/* Sprungindex (Anlass: DOTT liest Sprache kreuz und quer aus der 200-MB-
   Datei MONSTER.SOU - jeder Sprung zurueck lief die FAT-Kette von vorn
   durch). Fuer die zuletzt so gelesene Datei Merkpunkte: Cluster Nr.
   i << skx_shift, beim Vorlaufen eingesammelt. Jede FAT-Aenderung (fat_set)
   und jedes Neueinhaengen verwirft ihn. Tabelle in einem Kernelsegment. */
#define SKX_N 256
static unsigned fat_ksel(void);
/* Laufwerkssperre (Phase 6, kernel.c vol_lock): bis zum Ende des FAPI-Aufrufs */
#define VLOCK(d) kf_vol_lock(d)
static clus_t skx_first;                /* Startcluster der Datei, 0 = keiner */
static signed char skx_drive = -1;
static unsigned char skx_shift;
static unsigned skx_n;                  /* gueltige Merkpunkte (von vorn)  */
#define SKX_OFF 0U                      /* vorn im Segment von fat_ksel   */
#define SKX(i) (((clus_t __far *)MK_FP(fat_ksel(), SKX_OFF))[i])

static int skx_mine(int d, struct ifs_file __far *f)
{
    return skx_first && skx_drive == d && skx_first == f->first;
}

/* beim Vorlaufen: Cluster c beginnt bei Byte cpos */
static void skx_note(int d, struct ifs_file __far *f, clus_t c, unsigned long cpos,
                     unsigned sh)
{
    unsigned long i;
    if (!skx_mine(d, f))
        return;
    i = cpos >> sh;
    if ((i & ((1UL << skx_shift) - 1)) == 0 && (i >> skx_shift) == skx_n && skx_n < SKX_N)
        SKX(skx_n++) = c;
}

static clus_t next_cluster(int d, clus_t c)
{
    clus_t val = fat_raw(d, c);
    return valid(&vol[d], val) ? val : FAT_END;
}

static void fat_set(int d, clus_t c, clus_t val)
{
    struct volume *v = &vol[d];
    unsigned long sec;
    unsigned off;
    clus_t old;

    if (v->nfree_ok) {                  /* Zaehler freier Cluster mitfuehren */
        old = fat_raw(d, c);
        if (old == 0 && val != 0)
            v->nfree--;
        else if (old != 0 && val == 0)
            v->nfree++;
    }
    skx_first = 0;                      /* Kette kann sich aendern      */
    fat_pos(v, c, &sec, &off);
    if (fat_load(d, sec)) {
        if (!ferr)
            ferr = IFS_ERR;
        return;
    }
    if (v->fat16) {
        fcache[off] = (unsigned char)val;
        fcache[off + 1] = (unsigned char)(val >> 8);
        if (IS32(v)) {                  /* obere 4 Bit bleiben            */
            fcache[off + 2] = (unsigned char)(val >> 16);
            fcache[off + 3] = (unsigned char)((fcache[off + 3] & 0xF0) | ((val >> 24) & 0x0F));
        }
    } else if (c & 1) {
        fcache[off] = (unsigned char)((fcache[off] & 0x0F) | ((val & 0x0F) << 4));
        fcache[off + 1] = (unsigned char)(val >> 4);
    } else {
        fcache[off] = (unsigned char)val;
        fcache[off + 1] = (unsigned char)((fcache[off + 1] & 0xF0) | ((val >> 8) & 0x0F));
    }
    fc_dirty = 1;
}

/* freien Cluster belegen und an prev haengen (0 = neue Kette); 0 = voll */
static clus_t alloc_cluster(int d, clus_t prev)
{
    struct volume *v = &vol[d];
    clus_t n, c = v->hint;

    for (n = 0; n < v->clusters; n++, c++) {
        if (!valid(v, c))
            c = 2;
        if (fat_raw(d, c) == 0) {
            fat_set(d, c, EOC(v));
            if (prev)
                fat_set(d, prev, c);
            v->hint = c + 1;
            return ferr ? 0 : c;
        }
    }
    return 0;
}

static void dir_cache_reset(void);

/* Kette ab c freigeben */
static void free_chain(int d, clus_t c)
{
    struct volume *v = &vol[d];
    clus_t n, guard = 0;

    while (valid(v, c) && guard++ < v->clusters) {
        n = fat_raw(d, c);
        fat_set(d, c, 0);
        if (c < v->hint)
            v->hint = c;
        c = n;
    }
    dir_cache_reset();
}

static unsigned long clus_lba(struct volume *v, clus_t c)
{
    return v->data_lba + shl(c - 2, v->spc_shift);
}

/* Verzeichnis-Cluster: 0 = Stamm, bei FAT32 eine gewoehnliche Kette */
static clus_t dirc(struct volume *v, clus_t dc)
{
    return dc == 0 && IS32(v) ? ROOTC(v) : dc;
}

/* erster Cluster aus einem Verzeichniseintrag (FAT32: Bytes 20/21 dazu) */
static clus_t ent_clus(struct volume *v, const unsigned char __far *e)
{
    clus_t c = e[26] | ((unsigned)e[27] << 8);
    if (IS32(v))
        c |= (unsigned long)(e[20] | ((unsigned)e[21] << 8)) << 16;
    return c;
}

/* ---- Einhaengen -------------------------------------------------------- */
/* FAT16: FAT[1] Bit 15 = sauber abgemeldet (wie Windows 95/NT; MS-DOS
   ignoriert es). Beim ersten Schreiben loeschen, beim Herunterfahren
   setzen. Beim Start prueft CMD /P Laufwerke ohne das Bit. */
#define CLEAN_BIT 0x8000
#define CLEAN(v)  (IS32(v) ? 0x08000000UL : CLEAN_BIT)   /* FAT32: Bit 27 */

/* FAT32: FSInfo-Sektor (Zahl freier Cluster, naechster freier) - beim
   ersten Schreiben ungueltig (FFFFFFFFh), beim Herunterfahren mit dem
   gezaehlten Stand, wenn er gilt */
static void fsinfo(int d, int valid_now)
{
    struct volume *v = &vol[d];
    unsigned long lba = v->start + v->rootents;
    unsigned char __far *b = dma();
    unsigned i;
    if (!IS32(v) || !v->rootents || blk_read(v->blk, lba) ||
        rd32(0) != 0x41615252UL || rd32(484) != 0x61417272UL)
        return;
    for (i = 0; i < 8; i++)
        b[488 + i] = 0xFF;
    if (valid_now && v->nfree_ok) {
        put32(b, 488, v->nfree);
        put32(b, 492, v->hint);
    }
    wr(d, lba);
}

struct found {
    clus_t first;
    unsigned char attr;
    unsigned long size;
    clus_t dc;                          /* Lage des Eintrags            */
    unsigned idx;
};
static int lookup(int d, clus_t dc, const char *n, struct found *f);
static const char ea_n83[11];

static int __far fat_mount(int d)
{
    struct volume *v = &vol[d];
    unsigned bps, reserved, nfats, spc, n, ext = 0, f32 = 0;
    unsigned long total, cl, spf, fats, rootc = 0;

    if (blk_read(v->blk, v->start))
        return -1;
    if (rd16(510) != 0xAA55)
        return -1;
    bps      = rd16(11);
    spc      = dma()[13];
    reserved = rd16(14);
    nfats    = dma()[16];
    v->rootents = rd16(17);
    total    = rd16(19);
    spf      = rd16(22);
    if (total == 0)
        total = rd32(32);
    if (spf == 0) {                     /* FAT32 (wie Linux am BPB erkannt) */
        if (v->rootents || rd16(42) != 0)
            return -1;                  /* Version 0.0                    */
        spf = rd32(36);
        ext = rd16(40);                 /* Bit 7: nur aktive FAT (0-3)    */
        rootc = rd32(44);
        v->rootents = rd16(48);         /* FSInfo-Sektor                  */
        f32 = 1;
    }
    if (bps != 512 || spf == 0 || nfats == 0 || nfats > 2 || spc == 0 || spc > 64)
        return -1;
    for (n = 0; (1U << n) < spc; n++)
        ;
    if ((1U << n) != spc)
        return -1;
    fats = nfats == 2 ? spf + spf : spf;        /* ohne 32-Bit-Mult.    */
    v->spc = (unsigned char)spc;
    v->nfats = (unsigned char)nfats;
    v->spf = spf;
    v->hint = 2;
    v->nfree_ok = 0;
    v->spc_shift = (unsigned char)n;
    v->sectors  = total;
    v->fat_lba  = v->start + reserved;
    if (f32) {
        v->data_lba = v->fat_lba + fats;
        v->root_lba = rootc;            /* ROOTC: Stammcluster            */
        if (ext & 0x80) {               /* Spiegeln aus: nur aktive FAT   */
            if ((ext & 15) == 1)
                v->fat_lba += spf;
            v->nfats = 1;
        }
    } else {
        v->root_lba = v->fat_lba + fats;
        v->data_lba = v->root_lba + (v->rootents + 15) / 16;
    }
    if (total <= v->data_lba - v->start)
        return -1;
    cl = shr(total - (v->data_lba - v->start), n);
    if (f32 ? cl > 0x0FFFFFF4UL || rootc < 2 || rootc >= cl + 2 : cl >= 65525UL)
        return -1;
    v->clusters = cl;
    v->fat16 = f32 ? 2 : cl >= 4085;
    v->in_use = 0;
    v->was_dirty = 0;
    if (v->fat16 && !IS_FLOPPY(v->blk) && blk_read(v->blk, v->fat_lba) == 0)
        v->was_dirty = f32 ? !(rd32(4) & 0x08000000UL)   /* FAT[1] Bit 27 */
                           : !(rd16(2) & CLEAN_BIT);     /* FAT[1] Bit 15 */
    v->vfat = lfn_cfg == 1 || (f32 && lfn_cfg == 0);  /* FAT32: VFAT (Windows
                                           95/98), ausser LONGNAMES=EA oder
                                           EA DATA. SF im Stamm (AUTO)    */
    if (f32 && lfn_cfg == 0) {
        struct found ef;
        dir_cache_reset();
        if (lookup(d, 0, ea_n83, &ef) == 0)
            v->vfat = 0;
    }
    if (lfn_cfg == 0 && !f32) {         /* AUTO: VFAT, wenn der Stamm schon */
        unsigned i;                     /* lange Eintraege hat (Windows 95) */
        unsigned char __far *e;
        dir_cache_reset();
        for (i = 0; i < v->rootents; i++) {
            if (blk_read(v->blk, v->root_lba + (i >> 4)))
                break;
            e = dma() + ((i & 15) << 5);
            if (e[0] == 0)
                break;
            if (e[0] == 'E' && e[1] == 'A' && e[2] == ' ' && e[3] == 'D' &&
                e[7] == ' ' && e[8] == 'S' && e[9] == 'F' && e[11] != 0x0F) {
                v->vfat = 0;            /* EA DATA. SF: eigenes Format    */
                break;
            }
            if (e[11] == 0x0F && e[0] != 0xE5)
                v->vfat = 1;            /* lange Eintraege (weiter suchen: */
        }                               /* EA DATA. SF geht vor)          */
    }
    return 0;
}

/* ---- Verzeichnisse ----------------------------------------------------- */
/* zuletzt benutzter Cluster beim fortlaufenden Lesen eines Verzeichnisses */
static int lc_drive = -1;
static unsigned lc_n;
static clus_t lc_dir, lc_clus;
static unsigned long de_lba;            /* Sektor des letzten dir_entry()  */

static void ea_forget(int d);
static void dir_cache_reset(void)
{
    int d;
    skx_first = 0;
    lc_drive = -1;
    for (d = 0; d < NDRIVE; d++)        /* EA-Datei/Namen neu lesen       */
        ea_forget(d);
}

/* i-ter Eintrag des Verzeichnisses ab Cluster dc (0 = Stamm) im DMA-Puffer */
static unsigned char __far *dir_entry(int d, clus_t dc, unsigned i)
{
    struct volume *v = &vol[d];
    unsigned epc, n, k;
    clus_t c;

    dc = dirc(v, dc);
    if (dc == 0) {
        if (i >= v->rootents)
            return 0;
        de_lba = v->root_lba + (i >> 4);
        if (blk_read(v->blk, de_lba))
            return 0;
        return dma() + ((i & 15) << 5);
    }
    epc = (unsigned)v->spc * 16;
    n = i / epc;
    if (lc_drive == d && lc_dir == dc && lc_n <= n) {
        c = lc_clus;                            /* fortlaufend weiter   */
        k = lc_n;
    } else {
        c = dc;
        k = 0;
    }
    for (; k < n; k++) {
        c = next_cluster(d, c);
        if (c == FAT_END)
            return 0;
    }
    lc_drive = d; lc_dir = dc; lc_n = n; lc_clus = c;
    de_lba = clus_lba(v, c) + ((i % epc) >> 4);
    if (blk_read(v->blk, de_lba))
        return 0;
    return dma() + ((i & 15) << 5);
}

/* 0 = Ende, 1 = Datei/Ordner, 2 = ueberspringen */
static int kind(unsigned char __far *e)
{
    if (e[0] == 0)
        return 0;
    if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08))
        return 2;                       /* geloescht, langer Name, Label */
    return 1;
}

/* Pfadteil -> 11 Zeichen "NAME    EXT"; -1, wenn kein 8.3-Name */
static int to83(const char *s, unsigned len, char *n)
{
    unsigned i, k = 0, dot = 0;

    for (i = 0; i < 11; i++)
        n[i] = ' ';
    if (len == 1 && s[0] == '.') { n[0] = '.'; return 0; }
    if (len == 2 && s[0] == '.' && s[1] == '.') { n[0] = n[1] = '.'; return 0; }
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c == '.') {
            if (dot++ || k == 0)
                return -1;
            k = 8;
            continue;
        }
        if (c <= ' ' || c == '"' || c == '*' || c == '?' || c == '/' ||
            c == ':' || c == '<' || c == '>' || c == '|' || c == '+' ||
            c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
            return -1;
        if ((!dot && k >= 8) || (dot && k >= 11))
            return -1;                  /* zu lang fuer 8.3 */
        n[k++] = c;
    }
    return k ? 0 : -1;
}


/* ---- Lange Namen (VFAT, Windows 95) lesen --------------------------------
   Vor dem 8.3-Eintrag stehen Eintraege mit Attribut 0Fh, letzter Teil
   zuerst (Ordnungszahl | 40h), je 13 UCS-2-Zeichen an den Stellen 1-10,
   14-25, 28-31, dazu die Pruefsumme des 8.3-Namens (Byte 13). Zeichen
   ausserhalb von CP437 werden fuer Programme zu '_' - das Original merkt
   sich das Namensgedaechtnis (Unicode Stufe 1, weiter unten). */
static char lfn[IFS_NAMELEN];
static unsigned char lfn_sum, lfn_next; /* naechste erwartete Ordnungszahl */
static unsigned char lfn_have;          /* Teile gesammelt                */
static const unsigned char lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
/* CP437 80h-FFh -> Unicode (ganze Codepage, nicht nur Latin-1) */
static const unsigned short cp_uni[128] = {
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

/* Codepage (CHCP, kernel.c): Programme sehen Namen in der aktiven
   Codepage; cp_utab = deren Tabelle 80h-FFh, 0 = CP437 (cp_uni). Saetze
   'L' in EA DATA. SF sind immer CP437 und werden umgerechnet. */
extern unsigned short __far *cp_utab;

static unsigned cp2uni(unsigned char c)
{
    return c < 0x80 ? c : cp_utab ? cp_utab[c - 0x80] : cp_uni[c - 0x80];
}

/* Unicode -> aktive Codepage, 0 = keins */
static unsigned char uni2cp(unsigned u)
{
    unsigned i;
    if (u < 0x80)
        return (unsigned char)u;
    for (i = 0; i < 128; i++)
        if ((cp_utab ? cp_utab[i] : cp_uni[i]) == u)
            return (unsigned char)(0x80 + i);
    return 0;
}

/* Unicode -> CP437 (Saetze 'L'), 0 = keins */
static unsigned char uni437(unsigned u)
{
    unsigned i;
    if (u < 0x80)
        return (unsigned char)u;
    for (i = 0; i < 128; i++)
        if (cp_uni[i] == u)
            return (unsigned char)(0x80 + i);
    return 0;
}

/* ---- Namensgedaechtnis (Unicode Stufe 1) ----------------------------------
   Programme sehen Namen in CP437; was dort fehlt, wird '_'. Bekommt ein
   Task einen solchen Namen (Verzeichnis lesen, Pfadteil gefunden), merkt
   sich der Kernel CP437-Fassung -> UCS-2-Original. Legt derselbe Task
   danach einen Eintrag mit genau dieser CP437-Fassung an (COPY, XCOPY,
   MOVE, FM, Redirector der Box), schreibt fat.c das Original (VFAT-Eintraege
   bzw. Satz 'W' in EA DATA. SF). Ablage im Kernelsegment von fat.c:
   je Task (+1 fuer den Start) eine Mitschrift des gerade gelesenen langen
   Namens (512 Byte) und ein Puffer fuer einen W-Satz (544), dazu zwei
   Haelften zu 16 KB mit Eintraegen [Task+1][Laenge][CP437][UCS-2]; ist die
   laufende voll, wird die andere geleert und benutzt. */
#define UNI_OFF   (SKX_OFF + SKX_N * 4U)
#define UNI_TS    1056U
#define UNI_SLOT  ((unsigned)(current < 0 || current >= NTASK ? NTASK : current))
#define UNI_CAP   (UNI_OFF + UNI_SLOT * UNI_TS)
#define UNI_WB    (UNI_CAP + 512U)
#define UNI_MEM   (UNI_OFF + (NTASK + 1) * UNI_TS)
#define UNI_HALF  16384U
#define UNI_KSIZE (UNI_MEM + 2UL * UNI_HALF)
extern int current;                     /* kernel.c: laufender Task      */
static unsigned uni_fill[2];
static unsigned char uni_cur;
static unsigned char lfn_lossy;         /* Kontext: Name hat '_' fuer Fremdes */

static unsigned char __far *ufp(unsigned off)
{
    return (unsigned char __far *)MK_FP(fat_ksel(), off);
}

/* Eintrag des laufenden Tasks zu name -> Offset der UCS-2-Zeichen, 0 = keiner */
static unsigned uni_find(const char __far *name, unsigned len)
{
    unsigned h, w, o, k, n, base, sl = UNI_SLOT + 1;
    unsigned char __far *p;
    if (!len || len > 255 || !fat_ksel())
        return 0;
    for (h = 0; h < 2; h++) {           /* juengere Haelfte zuerst       */
        w = (uni_cur + h) & 1;
        base = UNI_MEM + w * UNI_HALF;
        for (o = 0; o < uni_fill[w]; o += 2 + 3 * n) {
            p = ufp(base + o);
            n = p[1];
            if (p[0] != sl || n != len)
                continue;
            for (k = 0; k < len && p[2 + k] == (unsigned char)name[k]; k++)
                ;
            if (k == len)
                return base + o + 2 + len;
        }
    }
    return 0;
}

/* name (CP437, len Zeichen) <- UCS-2 bei usel:uoff merken */
static void uni_note(const char __far *name, unsigned len, unsigned usel, unsigned uoff)
{
    unsigned o, k, w, need = 2 + 3 * len;
    unsigned char __far *p;
    const unsigned char __far *u = (const unsigned char __far *)MK_FP(usel, uoff);
    if (!len || len > 255 || !fat_ksel())
        return;
    if ((o = uni_find(name, len)) != 0) {       /* schon da: erneuern    */
        p = ufp(o);
        for (k = 0; k < 2 * len; k++)
            p[k] = u[k];
        return;
    }
    w = uni_cur;
    if (uni_fill[w] + need > UNI_HALF) {
        w ^= 1;
        uni_cur = (unsigned char)w;
        uni_fill[w] = 0;
    }
    p = ufp(UNI_MEM + w * UNI_HALF + uni_fill[w]);
    p[0] = (unsigned char)(UNI_SLOT + 1);
    p[1] = (unsigned char)len;
    for (k = 0; k < len; k++)
        p[2 + k] = (unsigned char)name[k];
    for (k = 0; k < 2 * len; k++)
        p[2 + len + k] = u[k];
    uni_fill[w] += need;
}

/* von aussen (DevHlp DH_UNINOTE, IFS-Module wie Joliet) */
void __far uni_note_ext(unsigned sel, unsigned name, unsigned ucs, unsigned len)
{
    uni_note((const char __far *)MK_FP(sel, name), len, sel, ucs);
}

static unsigned slen_f(const char __far *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static unsigned char sum83(const unsigned char __far *e)
{
    unsigned char s = 0;
    unsigned i;
    for (i = 0; i < 11; i++)
        s = (unsigned char)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

/* einen Verzeichniseintrag mitlesen; 1 = Teil eines langen Namens */
static int lfn_feed(const unsigned char __far *e)
{
    unsigned ord, i, c, p;
    if (e[11] != 0x0F || e[0] == 0xE5) {
        return 0;
    }
    ord = e[0] & 0x3F;
    if (e[0] & 0x40) {                  /* letzter Teil kommt zuerst    */
        if (ord == 0 || ord > 20) {
            lfn_have = 0;
            return 1;
        }
        lfn_sum = e[13];
        lfn[ord * 13 < IFS_NAMELEN ? ord * 13 : IFS_NAMELEN - 1] = 0;
        lfn_have = 1;
        lfn_lossy = 0;
    } else if (!lfn_have || ord != lfn_next - 1 || e[13] != lfn_sum) {
        lfn_have = 0;
        return 1;
    }
    lfn_next = (unsigned char)ord;
    p = (ord - 1) * 13;
    for (i = 0; i < 13 && p + i < IFS_NAMELEN - 1; i++) {
        unsigned char b;
        c = e[lfn_pos[i]] | ((unsigned)e[lfn_pos[i] + 1] << 8);
        if (fat_ksel())                 /* Original mitschreiben         */
            ((unsigned short __far *)ufp(UNI_CAP))[p + i] = (unsigned short)c;
        if (c == 0) {
            lfn[p + i] = 0;
            break;
        }
        b = uni2cp(c);
        if (!b) {
            b = '_';
            lfn_lossy = 1;
        }
        lfn[p + i] = (char)b;
    }
    return 1;
}

/* der eben gueltige VFAT-Name geht an das Programm: Original merken */
static void uni_take_lfn(void)
{
    if (lfn_lossy && fat_ksel())
        uni_note(lfn, slen_f(lfn), fat_ksel(), UNI_CAP);
}

/* eigenes Format (Stufe 3), weiter unten */
static void ea_load(int d, clus_t dc);
static int  ea_find(const unsigned char __far *e, char *out);
static void uni_take_ea(const char *name);
static char ea_tmp[IFS_NAMELEN];

/* gehoert der gesammelte lange Name zu diesem 8.3-Eintrag? */
static int lfn_valid(const unsigned char __far *e)
{
    int ok = lfn_have && lfn_next == 1 && sum83(e) == lfn_sum;
    lfn_have = 0;
    return ok;
}

static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Pfadteil (kanonisch, gross) gleich dem langen Namen? */
static int lfn_eq(const char *s, unsigned len)
{
    unsigned i;
    for (i = 0; i < len; i++)
        if (!lfn[i] || upc(lfn[i]) != s[i])
            return 0;
    return lfn[len] == 0;
}

/* Eintrag als 8.3-Text "NAME.EXT" */
static void name83(const unsigned char __far *e, char *out)
{
    unsigned j = 0, k, n, x;
    for (n = 8; n && e[n - 1] == ' '; n--)  /* nur Leerzeichen am Ende weg */
        ;                                   /* ("EA DATA. SF")            */
    for (x = 11; x > 8 && e[x - 1] == ' '; x--)
        ;
    for (k = 0; k < n; k++)
        out[j++] = (char)e[k];
    if (x > 8) {
        out[j++] = '.';
        for (k = 8; k < x; k++)
            out[j++] = (char)e[k];
    }
    out[j] = 0;
}

/* lange Namen vor Eintrag idx entfernen (beim Loeschen, Umbenennen) */
static void wr(int d, unsigned long lba);
static void drop_lfn(int d, clus_t dc, unsigned idx)
{
    unsigned char __far *e;
    unsigned char s, last;
    if (!idx || dc == NO_DC)
        return;
    e = dir_entry(d, dc, idx);
    if (!e)
        return;
    s = sum83(e);
    while (idx--) {
        e = dir_entry(d, dc, idx);
        if (!e || e[11] != 0x0F || e[0] == 0xE5 || e[13] != s)
            return;
        last = e[0] & 0x40;
        e[0] = 0xE5;
        wr(d, de_lba);
        if (last)
            return;
    }
}

/* Pfadteil suchen: 8.3-Name oder langer Name; n11 = 0, wenn der Teil
   kein 8.3-Name ist */
static int lookup_name(int d, clus_t dc, const char *s, unsigned len,
                       const char *n11, struct found *f)
{
    unsigned char __far *e;
    unsigned i, k;
    int ok;

    lfn_have = 0;
    if (!vol[d].vfat)
        ea_load(d, dc);                 /* lange Namen dieses Verzeichnisses */
    for (i = 0; (e = dir_entry(d, dc, i)) != 0; i++) {
        if (e[0] == 0)
            return -1;
        if (lfn_feed(e))
            continue;
        if (kind(e) != 1) {
            lfn_have = 0;
            continue;
        }
        ok = lfn_valid(e) && lfn_eq(s, len);
        if (ok)
            uni_take_lfn();
        if (!ok && !vol[d].vfat && ea_find(e, ea_tmp)) {   /* EA-Name    */
            for (k = 0; k < len && ea_tmp[k] && upc(ea_tmp[k]) == s[k]; k++)
                ;
            ok = k == len && !ea_tmp[k];
            if (ok)
                uni_take_ea(ea_tmp);
        }
        if (!ok && n11) {
            for (k = 0; k < 11 && e[k] == (unsigned char)n11[k]; k++)
                ;
            ok = k == 11;
        }
        if (ok) {
            f->dc = dc;
            f->idx = i;
            f->first = ent_clus(&vol[d], e);
            f->attr  = e[11];
            f->size  = (e[28] | ((unsigned)e[29] << 8)) |
                       ((unsigned long)(e[30] | ((unsigned)e[31] << 8)) << 16);
            return 0;
        }
    }
    return -1;
}

static int lookup(int d, clus_t dc, const char *n, struct found *f)
{
    unsigned char __far *e;
    unsigned i, k;

    for (i = 0; (e = dir_entry(d, dc, i)) != 0; i++) {
        switch (kind(e)) {
        case 0: return -1;
        case 2: continue;
        }
        for (k = 0; k < 11 && e[k] == (unsigned char)n[k]; k++)
            ;
        if (k == 11) {
            f->dc = dc;
            f->idx = i;
            f->first = ent_clus(&vol[d], e);
            f->attr  = e[11];
            f->size  = (e[28] | ((unsigned)e[29] << 8)) |
                       ((unsigned long)(e[30] | ((unsigned)e[31] << 8)) << 16);
            return 0;
        }
    }
    return -1;
}

/* kanonischen Pfad ablaufen; "\" ist das Stammverzeichnis (first = 0) */
static int walk(int d, const char *path, struct found *f)
{
    char n[11];
    const char *p = path, *q;

    f->first = 0;
    f->attr = ATTR_DIR;
    f->size = 0;
    f->dc = NO_DC;                      /* Stamm hat keinen Eintrag     */
    f->idx = 0;
    while (*p == '\\')
        p++;
    while (*p) {
        for (q = p; *q && *q != '\\'; q++)
            ;
        if (!(f->attr & ATTR_DIR))
            return -1;                  /* Zwischenstueck ist eine Datei */
        if (lookup_name(d, f->first, p, (unsigned)(q - p),
                        to83(p, (unsigned)(q - p), n) ? 0 : n, f))
            return -1;
        p = q;
        while (*p == '\\')
            p++;
    }
    return 0;
}

static int revalidate(int d);

static int __far fat_open(int d, const char *path, struct ifs_file __far *fl)
{
    struct found f;
    if (revalidate(d) || walk(d, path, &f) || (f.attr & ATTR_DIR))
        return -1;
    fl->drive = (unsigned char)d;
    fl->attr = f.attr;
    fl->first = f.first;
    fl->size = f.size;
    fl->cache_clus = 0;
    fl->cache_pos = 0;
    fl->dir_clus = f.dc;
    fl->dir_idx = f.idx;
    fl->flags = 0;
    return 0;
}

static int __far fat_isdir(int d, const char *path)
{
    struct found f;
    return walk(d, path, &f) == 0 && (f.attr & ATTR_DIR);
}

static int __far fat_readdir(int d, const char *dir, unsigned want, struct ifs_dirent *de)
{
    struct found f;
    unsigned char __far *e;
    unsigned i, k, n = 0;

    if ((want == 0 && revalidate(d)) || walk(d, dir, &f) || !(f.attr & ATTR_DIR))
        return -1;
    lfn_have = 0;
    if (!vol[d].vfat)
        ea_load(d, f.first);
    for (i = 0; (e = dir_entry(d, f.first, i)) != 0; i++) {
        if (e[0] == 0)
            return -1;
        if (lfn_feed(e))
            continue;
        switch (kind(e)) {
        case 0: return -1;
        case 2: lfn_have = 0; continue;
        }
        if (n++ != want) {
            lfn_have = 0;
            continue;
        }
        name83(e, de->sname);
        if (lfn_valid(e)) {             /* langer Name                  */
            for (k = 0; lfn[k] && k < IFS_NAMELEN - 1; k++)
                de->name[k] = lfn[k];
            de->name[k] = 0;
            uni_take_lfn();
        } else if (!vol[d].vfat && ea_find(e, de->name)) {
            uni_take_ea(de->name);      /* langer Name aus EA DATA. SF  */
        } else {
            unsigned dot = 0, low = 0;
            for (k = 0; de->sname[k]; k++) {    /* NT: Byte 12 Bit 3/4 =   */
                char c = de->sname[k];          /* Name/Endung klein       */
                if (c == '.')
                    dot = 1;
                else if (c >= 'A' && c <= 'Z' && (e[12] & (dot ? 0x10 : 0x08)))
                    c = (char)(c + 32), low = 1;
                de->name[k] = c;
            }
            de->name[k] = 0;
            if (!low)                   /* schon 8.3; mit Kleinschrift */
                de->sname[0] = 0;       /* bleibt der Kurzname gross fuer */
        }                               /* alte Programme                */
        de->attr = e[11];
        de->time = e[22] | ((unsigned)e[23] << 8);
        de->date = e[24] | ((unsigned)e[25] << 8);
        de->size = (e[28] | ((unsigned)e[29] << 8)) |
                   ((unsigned long)(e[30] | ((unsigned)e[31] << 8)) << 16);
        return 0;
    }
    return -1;
}

static int __far fat_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
                    unsigned off, unsigned len)
{
    int d = f->drive;
    struct volume *v = &vol[d];
    unsigned csz = (unsigned)v->spc * 512, so, b, n, ra, done = 0, sh;
    unsigned long cpos;
    clus_t c;

    if (pos >= f->size)
        return 0;
    if (f->size - pos < len)
        len = (unsigned)(f->size - pos);

    for (sh = 9; (1U << (sh - 9)) < v->spc; sh++)
        ;
    if (f->cache_clus && pos >= f->cache_pos) {
        c = f->cache_clus;                      /* weiter, wo wir waren */
        cpos = f->cache_pos;
    } else {
        c = f->first;
        cpos = 0;
    }
    if ((pos - cpos) >> sh > 64 && valid(v, f->first)) {   /* weit: Sprungindex */
        if (!skx_mine(d, f) && fat_ksel()) {
            skx_drive = (signed char)d;
            skx_first = f->first;
            for (skx_shift = 0; (f->size >> sh) >> skx_shift >= SKX_N; skx_shift++)
                ;
            SKX(0) = f->first;
            skx_n = 1;
        }
        if (skx_mine(d, f)) {
            unsigned long k = (pos >> sh) >> skx_shift, kp;
            if (k >= skx_n)
                k = skx_n - 1;
            kp = (k << skx_shift) << sh;
            if (kp > cpos) {                    /* naeher dran als bisher */
                c = SKX((unsigned)k);
                cpos = kp;
            }
        }
    }
    while (pos - cpos >= csz) {                 /* zum Cluster vorlaufen */
        if (!valid(v, c))
            return -1;
        c = next_cluster(d, c);
        cpos += csz;
        if (valid(v, c))
            skx_note(d, f, c, cpos, sh);
    }
    while (done < len) {
        if (!valid(v, c))
            return -1;                          /* kaputte Kette        */
        f->cache_clus = c;
        f->cache_pos  = cpos;
        so = (unsigned)(pos - cpos);
        b = so & 511;
        ra = (unsigned)(csz - so + 511) >> 9;   /* voraus bis Clusterende */
        if (blk_read_n(v->blk, clus_lba(v, c) + (so >> 9), ra))
            return -1;
        n = 512 - b;
        if (n > len - done)
            n = len - done;
        far_copy(sel, off + done, DMABUF, b, n);
        done += n;
        pos  += n;
        if (pos - cpos >= csz) {
            c = next_cluster(d, c);
            cpos += csz;
            if (valid(v, c))
                skx_note(d, f, c, cpos, sh);
        }
    }
    return done;
}


/* ======================================================================== */
/* ---- Schreiben ---------------------------------------------------------- */

static void put16(unsigned char __far *e, unsigned o, unsigned v)
{
    e[o] = (unsigned char)v;
    e[o + 1] = (unsigned char)(v >> 8);
}

/* ersten Cluster eintragen (FAT32: obere 16 Bit nach 20/21; sonst dort
   0, bei OS/2 waere es der EA-Verweis) */
static void put_clus(int d, unsigned char __far *e, clus_t c)
{
    put16(e, 26, (unsigned)c);
    put16(e, 20, IS32(&vol[d]) ? (unsigned)(c >> 16) : 0);
}

static void stamp(unsigned char __far *e)
{
    unsigned long t = rtc_stamp();
    put16(e, 22, (unsigned)t);
    put16(e, 24, (unsigned)(t >> 16));
}

/* Sektor in DMABUF (vorher per dir_entry/blk_read geholt) zurueckschreiben */
static void wr(int d, unsigned long lba)
{
    int rc = blk_write(vol[d].blk, lba);
    if (rc && !ferr)
        ferr = rc == -2 ? IFS_WP : IFS_ERR;
}

/* Ende einer Operation: FAT schreiben, Fehler melden */
static int done_op(int rc)
{
    fat_flush();
    if (ferr)
        return ferr;
    return rc;
}

static void zero_cluster(int d, clus_t c)
{
    struct volume *v = &vol[d];
    unsigned long lba = clus_lba(v, c);
    unsigned i;
    for (i = 0; i < v->spc && !ferr; i++) {
        zero_seg(DMABUF, 256);
        wr(d, lba + i);
    }
}

/* kanonischen Pfad in Elternverzeichnis und 8.3-Namen zerlegen */
static int name_for(int d, clus_t dc, const char *comp, unsigned clen, char *n83);
static const char *sp_comp;             /* letzter Pfadteil nach split()  */
static unsigned sp_len;

/* Eltern suchen, Namen fuer den neuen Eintrag bestimmen (name_for) */
static char sp_pbuf[IFS_PATHLEN];               /* Kontext (Phase 6) */
#define pbuf sp_pbuf
static int split(int d, const char *path, struct found *par, char *n83)
{
    const char *last = path, *p;
    unsigned k;

    for (p = path; *p; p++)
        if (*p == '\\')
            last = p;
    for (k = 0; path + k < last && k < IFS_PATHLEN - 1; k++)
        pbuf[k] = path[k];
    pbuf[k] = 0;
    if (k == 0) {
        pbuf[0] = '\\';
        pbuf[1] = 0;
    }
    if (*last == '\\')
        last++;
    for (p = last; *p; p++)
        ;
    if (walk(d, pbuf, par) || !(par->attr & ATTR_DIR) || p == last)
        return IFS_ERR;
    sp_comp = last;
    sp_len = (unsigned)(p - last);
    return name_for(d, par->first, last, sp_len, n83);
}
#undef pbuf

/* gibt es den Namen (8.3 oder lang) im Verzeichnis schon? */
static int exists(int d, clus_t dc, struct found *f)
{
    char n[11];
    return lookup_name(d, dc, sp_comp, sp_len,
                       to83(sp_comp, sp_len, n) ? 0 : n, f) == 0;
}

/* freien Eintrag im Verzeichnis dc; erweitert Unterverzeichnisse */
static int dir_slot(int d, clus_t dc, unsigned *idx)
{
    unsigned char __far *e;
    unsigned i;
    clus_t c, n;

    for (i = 0; (e = dir_entry(d, dc, i)) != 0; i++)
        if (e[0] == 0 || e[0] == 0xE5) {
            *idx = i;
            return 0;
        }
    if ((dc = dirc(&vol[d], dc)) == 0)
        return IFS_FULL;                /* Stammverzeichnis hat feste Groesse */
    for (c = dc; (n = next_cluster(d, c)) != FAT_END; c = n)
        ;
    n = alloc_cluster(d, c);
    if (!n)
        return ferr ? ferr : IFS_FULL;
    zero_cluster(d, n);
    *idx = i;
    return 0;
}

/* neuen Eintrag anlegen: Name, Attribut, erster Cluster */
static unsigned char nt_case;            /* Byte 12 fuer den naechsten Eintrag */

static int new_entry(int d, clus_t dc, const char *n83, unsigned char attr,
                     clus_t first, unsigned *idx)
{
    unsigned char __far *e;
    unsigned k;
    int rc = dir_slot(d, dc, idx);

    if (rc)
        return rc;
    e = dir_entry(d, dc, *idx);
    if (!e)
        return IFS_ERR;
    for (k = 0; k < 32; k++)
        e[k] = 0;
    for (k = 0; k < 11; k++)
        e[k] = (unsigned char)n83[k];
    e[11] = attr;
    e[12] = nt_case;
    stamp(e);
    put_clus(d, e, first);
    wr(d, de_lba);
    return 0;
}

/* ---- Lange Namen schreiben (VFAT, Stufe 2) --------------------------------
   Nur auf Laufwerken mit vol[].vfat (LONGNAMES=VFAT oder AUTO mit langen
   Eintraegen im Stamm). Name in 8.3 und nur Klein- oder nur Grossbuch-
   staben je Teil: NT-Kleinschreibungsbits (Byte 12), kein langer Eintrag.
   Sonst lange Eintraege + Kurzname NAME~n.EXT wie Windows 95. */
static char ln_buf[IFS_NAMELEN];        /* zu schreibender langer Name    */

/* n freie Eintraege am Stueck (Unterverzeichnis wird verlaengert) */
static int dir_slots(int d, clus_t dc, unsigned n, unsigned *idx)
{
    unsigned char __far *e;
    unsigned i, run = 0, start = 0;
    clus_t c, nx;

    for (i = 0;; i++) {
        e = dir_entry(d, dc, i);
        if (!e) {
            if (dirc(&vol[d], dc) == 0)
                return IFS_FULL;
            for (c = dirc(&vol[d], dc); (nx = next_cluster(d, c)) != FAT_END; c = nx)
                ;
            nx = alloc_cluster(d, c);
            if (!nx)
                return ferr ? ferr : IFS_FULL;
            zero_cluster(d, nx);
            e = dir_entry(d, dc, i);
            if (!e)
                return IFS_ERR;
        }
        if (e[0] == 0 || e[0] == 0xE5) {
            if (!run)
                start = i;
            if (++run == n) {
                *idx = start;
                return 0;
            }
        } else
            run = 0;
    }
}

/* langen Namen + 8.3-Eintrag anlegen; *idx = Lage des 8.3-Eintrags */
static int new_entry_lfn(int d, clus_t dc, const char *ln, const char *n83,
                         unsigned char attr, clus_t first, unsigned *idx)
{
    unsigned char __far *e;
    unsigned len, k, j, i, pos, ord, start, ch;
    unsigned char sum;
    int rc;

    unsigned uo;
    for (len = 0; ln[len]; len++)
        ;
    k = (len + 12) / 13;
    rc = dir_slots(d, dc, k + 1, &start);
    uo = uni_find(ln, len);             /* Original aus dem Gedaechtnis  */
    if (rc)
        return rc;
    {                                   /* Pruefsumme des Kurznamens      */
        unsigned char s = 0;
        for (i = 0; i < 11; i++)
            s = (unsigned char)(((s & 1) << 7) + (s >> 1) + (unsigned char)n83[i]);
        sum = s;
    }
    for (j = 0; j < k; j++) {           /* letzter Teil zuerst            */
        ord = k - j;
        e = dir_entry(d, dc, start + j);
        if (!e)
            return IFS_ERR;
        for (i = 0; i < 32; i++)
            e[i] = 0;
        e[0] = (unsigned char)(ord | (j == 0 ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (i = 0; i < 13; i++) {
            pos = (ord - 1) * 13 + i;
            ch = pos < len ? (uo ? ((unsigned short __far *)ufp(uo))[pos]
                                 : cp2uni((unsigned char)ln[pos]))
                           : pos == len ? 0 : 0xFFFF;
            e[lfn_pos[i]] = (unsigned char)ch;
            e[lfn_pos[i] + 1] = (unsigned char)(ch >> 8);
        }
        wr(d, de_lba);
    }
    e = dir_entry(d, dc, start + k);
    if (!e)
        return IFS_ERR;
    for (i = 0; i < 32; i++)
        e[i] = 0;
    for (i = 0; i < 11; i++)
        e[i] = (unsigned char)n83[i];
    e[11] = attr;
    stamp(e);
    put_clus(d, e, first);
    wr(d, de_lba);
    *idx = start + k;
    return 0;
}

/* Kurzname zum langen Namen: NAME~n.EXT, im Verzeichnis eindeutig */
static void gen_short(int d, clus_t dc, const char *ln, char *n83)
{
    static const char bad[] = "\"*+,/:;<=>?[\\]|";
    struct found f;
    char base[8], ext[3];
    const char *dot = 0, *p;
    unsigned nb = 0, ne = 0, i, n, k, w;
    char c;

    for (p = ln; *p; p++)
        if (*p == '.')
            dot = p;
    for (p = ln; *p && p != dot; p++) {
        c = upc(*p);
        if (c == ' ' || c == '.')
            continue;
        for (i = 0; bad[i] && bad[i] != c; i++)
            ;
        if (bad[i] || (unsigned char)c < ' ')
            c = '_';
        if (nb < 8)
            base[nb++] = c;
    }
    if (!nb)
        base[nb++] = '_';
    if (dot)
        for (p = dot + 1; *p && ne < 3; p++) {
            c = upc(*p);
            if (c == ' ')
                continue;
            for (i = 0; bad[i] && bad[i] != c; i++)
                ;
            ext[ne++] = (bad[i] || (unsigned char)c < ' ') ? '_' : c;
        }
    for (n = 1; n < 1000; n++) {
        w = n < 10 ? 1 : n < 100 ? 2 : 3;   /* Ziffern                  */
        k = nb < 7 - w ? nb : 7 - w;        /* ~n + Ziffern in 8 Zeichen */
        for (i = 0; i < 11; i++)
            n83[i] = ' ';
        for (i = 0; i < k; i++)
            n83[i] = base[i];
        n83[k] = '~';
        for (i = w; i > 0; i--) {
            unsigned v = n, j;
            for (j = 1; j < i; j++)
                v /= 10;
            n83[k + 1 + (w - i)] = (char)('0' + v % 10);
        }
        for (i = 0; i < ne; i++)
            n83[8 + i] = ext[i];
        if (lookup(d, dc, n83, &f))
            return;                     /* frei                           */
    }
}

/* Namen fuer einen neuen Eintrag bestimmen: n83, ln_buf ("" = kein langer
   Name), nt_case. comp/clen = letzter Pfadteil (kanonisch gross). Lange
   Namen nur mit vol[].vfat (EA-Weg: Stufe 3). -> 0 oder IFS_ERR */
static int name_for(int d, clus_t dc, const char *comp, unsigned clen, char *n83)
{
    const char *o = comp;
    unsigned i, lo = 0, up = 0, dot = 0, mixed = 0, partlo[2], partup[2];

    nt_case = 0;
    ln_buf[0] = 0;
    for (i = 0; ifs_orig[i] && i < clen && upc(ifs_orig[i]) == comp[i]; i++)
        ;
    if (i == clen && !ifs_orig[i])
        o = ifs_orig;                   /* Originalschreibung passt       */
    if (to83(comp, clen, n83) == 0 && n83[0] != '.') {
        partlo[0] = partlo[1] = partup[0] = partup[1] = 0;
        for (i = 0; i < clen; i++) {
            if (o[i] == '.')
                dot = 1;
            else if (o[i] >= 'a' && o[i] <= 'z')
                partlo[dot] = 1, lo = 1;
            else if (o[i] >= 'A' && o[i] <= 'Z')
                partup[dot] = 1, up = 1;
        }
        mixed = (partlo[0] && partup[0]) || (partlo[1] && partup[1]);
        if (!uni_find(o, clen)) {       /* Original bekannt (___.txt aus
                                           Japanisch): langer Eintrag    */
            if (!lo)
                return 0;               /* alles gross: nur 8.3           */
            if (!mixed) {               /* NT: Name/Endung ganz klein     */
                nt_case = (unsigned char)((partlo[0] ? 0x08 : 0) | (partlo[1] ? 0x10 : 0));
                return 0;
            }
        }
        /* gemischt: VFAT-Eintrag bzw. EA-Satz bewahrt die Schreibung */
    } else {
        if (clen >= IFS_NAMELEN)
            return IFS_ERR;
        for (i = 0; i < clen; i++)      /* in langen Namen verboten       */
            if ((unsigned char)o[i] < ' ' || o[i] == '"' || o[i] == '*' ||
                o[i] == '?' || o[i] == '/' || o[i] == ':' || o[i] == '<' ||
                o[i] == '>' || o[i] == '|' || o[i] == '\\')
                return IFS_ERR;
        gen_short(d, dc, o, n83);       /* VFAT und EA: Kurzname NAME~n   */
    }
    for (i = 0; i < clen; i++)
        ln_buf[i] = o[i];
    ln_buf[i] = 0;
    (void)up;
    return 0;
}

/* Eintrag anlegen: mit langem Namen, wenn name_for einen bestimmt hat */
static int ea_add(int d, clus_t dc, const char *n83, const char *name);
static void ea_del(int d, clus_t dc, const unsigned char *n83);

static int new_entry_n(int d, clus_t dc, const char *n83, unsigned char attr,
                       clus_t first, unsigned *idx)
{
    int rc;
    if (ln_buf[0] && vol[d].vfat)
        return new_entry_lfn(d, dc, ln_buf, n83, attr, first, idx);
    rc = new_entry(d, dc, n83, attr, first, idx);
    if (rc == 0 && ln_buf[0])           /* EA-Laufwerk: Satz anlegen      */
        rc = ea_add(d, dc, n83, ln_buf);
    return rc;
}

/* Wechselplatte: Partition auf dem eingelegten Medium suchen - ohne
   Partitionstabelle (Bootsektor mit BPB, "Superfloppy") das ganze Medium,
   sonst die erste FAT-Partition (ZIP-Disketten von Iomega: Nr. 4) */
static int rem_locate(int d)
{
    int dev = vol[d].blk, i;
    unsigned char __far *b;
    unsigned char t;

    if (blk_read(dev, 0))
        return -1;
    b = MK_FP(DMABUF, 0);
    if (b[510] != 0x55 || b[511] != 0xAA)
        return -1;
    if ((b[0] == 0xEB || b[0] == 0xE9) && b[11] == 0 && b[12] == 2 && b[13] &&
        !(b[13] & (b[13] - 1)) && (b[16] == 1 || b[16] == 2)) {
        vol[d].start = 0;
        vol[d].psize = blk[dev].blocks;
        vol[d].ptype = 0;
        vol[d].part = 0;
        return 0;
    }
    for (i = 0; i < 4; i++) {
        t = b[0x1BE + 16 * i + 4];
        if (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0E || t == 0x0B || t == 0x0C) {
            vol[d].start = *(unsigned long __far *)(b + 0x1BE + 16 * i + 8);
            vol[d].psize = *(unsigned long __far *)(b + 0x1BE + 16 * i + 12);
            vol[d].ptype = t;
            vol[d].part = (unsigned char)(i + 1);
            return 0;
        }
    }
    return -1;
}

/* Diskette und Wechselplatte: vor jeder Operation neu einhaengen, sie
   koennte gewechselt sein */
static int revalidate(int d)
{
    int b = vol[d].blk;
    if (!IS_FLOPPY(b) && !IS_REM(b))
        return 0;
    if (blk_media(b) == 0 && vol[d].used && !(IS_REM(b) && blk_nomedia(b)))
        return 0;                       /* nicht gewechselt: alles gilt */
    fc_drive = -1;
    fc_dirty = 0;
    dir_cache_reset();
    blk_invalidate();
    if (blk_nomedia(b)) {
        ifs_notready = 1;               /* FAPI meldet "nicht bereit"   */
        return IFS_ERR;
    }
    if (IS_REM(b)) {
        vol[d].used = 0;
        if (rem_locate(d) || fat_mount(d))
            return IFS_ERR;
        vol[d].used = 1;
        vol[d].locked = 0;
        return 0;
    }
    return fat_mount(d) ? IFS_ERR : 0;
}

static int __far fat_create(int d, const char *path, struct ifs_file __far *fl)
{
    struct found par, f;
    char n[11];
    unsigned char __far *e;
    unsigned idx;
    int rc;

    ferr = 0;
    if (revalidate(d) || split(d, path, &par, n))
        return IFS_ERR;
    if (exists(d, par.first, &f)) {
        /* vorhandene Datei: auf Laenge 0 kuerzen */
        if (f.attr & (ATTR_DIR | ATTR_RO | ATTR_SYSTEM | ATTR_HIDDEN | 0x08))
            return IFS_DENIED;
        free_chain(d, f.first);
        e = dir_entry(d, par.first, f.idx);
        if (!e)
            return done_op(IFS_ERR);
        put_clus(d, e, 0);
        put16(e, 28, 0);
        put16(e, 30, 0);
        e[11] |= 0x20;                  /* Archiv                       */
        stamp(e);
        wr(d, de_lba);
        idx = f.idx;
    } else {
        rc = new_entry_n(d, par.first, n, 0x20, 0, &idx);
        if (rc)
            return done_op(rc);
    }
    fl->drive = (unsigned char)d;
    fl->attr = 0x20;
    fl->first = 0;
    fl->size = 0;
    fl->cache_clus = 0;
    fl->cache_pos = 0;
    fl->dir_clus = par.first;
    fl->dir_idx = idx;
    fl->flags = IFS_F_WRITE;
    return done_op(0);
}

/* Cluster fuer Position cpos+csz holen, notfalls anhaengen; 0 = voll */
static clus_t next_or_alloc(int d, clus_t c)
{
    clus_t n = next_cluster(d, c);
    return n != FAT_END ? n : alloc_cluster(d, c);
}

static int __far fat_write(struct ifs_file __far *f, unsigned long pos, unsigned sel,
                     unsigned off, unsigned len)
{
    int d = f->drive;
    struct volume *v = &vol[d];
    unsigned csz = (unsigned)v->spc * 512, so, b, n, done = 0;
    unsigned long cpos, lba;
    clus_t c;

    ferr = 0;
    if (!(f->flags & IFS_F_WRITE) || pos > f->size)
        return IFS_ERR;
    if (len == 0)
        return 0;
    if (f->first == 0) {
        c = alloc_cluster(d, 0);
        if (!c)
            return done_op(IFS_FULL);
        f->first = c;
        f->flags |= IFS_F_DIRTY;
        f->cache_clus = 0;
    }
    if (f->cache_clus && pos >= f->cache_pos) {
        c = f->cache_clus;
        cpos = f->cache_pos;
    } else {
        c = f->first;
        cpos = 0;
    }
    while (pos - cpos >= csz) {
        c = next_or_alloc(d, c);
        if (!c)
            return done_op(IFS_FULL);
        cpos += csz;
    }
    while (done < len && !ferr) {
        so = (unsigned)(pos - cpos);
        lba = clus_lba(v, c) + (so >> 9);
        b = so & 511;
        n = 512 - b;
        if (n > len - done)
            n = len - done;
        if (b != 0 || n < 512) {
            /* Teil eines Sektors: vorhandene Daten erhalten */
            if (pos - b < f->size) {
                if (blk_read(v->blk, lba)) {
                    ferr = IFS_ERR;
                    break;
                }
            } else
                zero_seg(DMABUF, 256);
        }
        far_copy(DMABUF, b, sel, off + done, n);
        wr(d, lba);
        if (ferr)
            break;
        done += n;
        pos += n;
        if (pos > f->size) {
            f->size = pos;
            f->flags |= IFS_F_DIRTY;
        }
        f->cache_clus = c;
        f->cache_pos = cpos;
        if (pos - cpos >= csz && done < len) {
            c = next_or_alloc(d, c);
            if (!c) {
                fat_flush();
                return done ? (int)done : (ferr ? ferr : IFS_FULL);
            }
            cpos += csz;
        }
    }
    fat_flush();
    if (ferr && done == 0)
        return ferr;
    return (int)done;
}

static int __far fat_close(struct ifs_file __far *f)
{
    unsigned char __far *e;
    int d = f->drive;

    ferr = 0;
    if (!(f->flags & IFS_F_DIRTY))
        return 0;
    e = dir_entry(d, f->dir_clus, f->dir_idx);
    if (!e)
        return IFS_ERR;
    put_clus(d, e, f->first);
    put16(e, 28, (unsigned)f->size);
    put16(e, 30, (unsigned)(f->size >> 16));
    e[11] |= 0x20;
    stamp(e);
    wr(d, de_lba);
    f->flags &= ~IFS_F_DIRTY;
    return done_op(0);
}

/* Datei kuerzen (DosNewSize): Kette hinter dem Cluster mit Byte size-1
   freigeben, Groesse und Eintrag beim Schliessen */
static int __far fat_truncate(struct ifs_file __far *f, unsigned long size)
{
    int d = f->drive;
    struct volume *v = &vol[d];
    unsigned long csz = 512UL << v->spc_shift, p;
    clus_t c, nx;

    ferr = 0;
    if (!(f->flags & IFS_F_WRITE) || size > f->size)
        return IFS_ERR;
    if (size == 0) {
        if (f->first)
            free_chain(d, f->first);
        f->first = 0;
    } else {
        c = f->first;
        for (p = csz; p < size && valid(v, c); p += csz)
            c = next_cluster(d, c);
        if (!valid(v, c))
            return done_op(IFS_ERR);
        nx = fat_raw(d, c);
        fat_set(d, c, EOC(v));
        if (valid(v, nx))
            free_chain(d, nx);
    }
    f->size = size;
    f->flags |= IFS_F_DIRTY;
    f->cache_clus = 0;
    return done_op(0);
}

/* ---- Lange Namen im eigenen Format (Stufe 3, Entscheidung des Nutzers) ---
   "EA DATA. SF" im Stamm (versteckt, System; MS-DOS-Werkzeuge sehen eine
   normale Datei). Saetze in Einheiten zu 32 Byte:
     0  'L' = langer Name, 'W' = langer Name in UTF-16, 0 = frei
     1  Laenge in Einheiten
     2  Startcluster des Verzeichnisses (32 Bit, 0 = Stamm; FAT32-fest)
     6  8.3-Name (11 Byte) - der Rueckverweis auf die Datei
     17 Laenge des Namens in Zeichen, ab 18 der Name (L: CP437, W: UTF-16
        little endian, 2 Byte je Zeichen - nur wenn der Name Zeichen
        ausserhalb von CP437 hat, Unicode Stufe 1)
   Im Verzeichniseintrag steht nichts (OS/2 nimmt Offset 14h, bei FAT32 die
   oberen Clusterbits). Zwischenspeicher: die Namen eines Verzeichnisses
   (das zuletzt gelesene) in einem Kernelsegment, sonst kostete jede Suche
   Lesezugriffe; reicht er nicht (16 KB), fehlen die uebrigen Namen. */
#define EA_HDR   18
static struct ifs_file ea_f;            /* EA DATA. SF von ea_drv         */
static int ea_drv = -1;
static const char ea_n83[11] = { 'E', 'A', ' ', 'D', 'A', 'T', 'A', ' ', 'S', 'F', ' ' };
static unsigned ea_csel;                /* Segment von fat_ksel          */
/* Namen eines Verzeichnisses aus EA DATA. SF: in Stuecken zu 64 KB (bis
   512 KB, ~12.000 Namen), die bei Bedarf geholt werden, alle ueber einen
   Selektor (eac_sel, wird auf das Stueck umgelegt - ein GDT-Platz). Vorher
   16 KB im Segment von fat_ksel: in einem Ordner mit 1.500 langen Namen
   zeigte AT-DOS nur 362 lang, der Rest kam als LAN~1495.TXT. Gehoert wie
   bisher unter die EA-Sperre (VK_EA). */
#define EAC_N  8
#define EAC_SZ 0xFFF0U
static unsigned eac_sel;
static pg_t eac_pg[EAC_N];
static unsigned eac_used[EAC_N];
static unsigned char eac_n, eac_last, eac_cur = 0xFF, ea_uchunk;

static unsigned char __far *eac_map(unsigned k)
{
    if (k != eac_cur) {
        set_desc(eac_sel >> 3, PG_LIN(eac_pg[k]), 0xFFFF, ACC_KDATA);
        eac_cur = (unsigned char)k;
    }
    return (unsigned char __far *)MK_FP(eac_sel, 0);
}
static unsigned ea_uoff;                /* ea_find: UCS-2 des Treffers, 0 = keins */

/* ein Kernelsegment fuer fat.c: Sprungindex + Namensgedaechtnis
   (die GDT-Plaetze fuer Kernelsegmente sind knapp) */
static unsigned fat_ksel(void)
{
    if (!ea_csel)
        ea_csel = kseg_alloc(UNI_KSIZE);    /* + Namensgedaechtnis    */
    return ea_csel;
}
static clus_t ea_cdc;
static int ea_cdrv = -1;
static unsigned char ea_rec[EA_HDR + IFS_NAMELEN + 32];

#define EAF ((struct ifs_file __far *)&ea_f)

static void ea_forget(int d)            /* Laufwerk neu/gewechselt        */
{
    kf_ea_lock();
    if (ea_drv == d)
        ea_drv = -1;
    if (ea_cdrv == d)
        ea_cdrv = -1;
}

/* EA-Datei oeffnen; create = anlegen, wenn sie fehlt. 0 = ok */
static int ea_open(int d, int create)
{
    struct found f;
    unsigned idx;
    unsigned char save = nt_case;
    if (ea_drv == d)
        return 0;
    ea_drv = -1;
    if (lookup(d, 0, ea_n83, &f) == 0) {
        ea_f.first = f.first;
        ea_f.size = f.size;
        ea_f.dir_idx = f.idx;
    } else {
        if (!create)
            return -1;
        nt_case = 0;
        idx = (unsigned)new_entry(d, 0, ea_n83, ATTR_HIDDEN | ATTR_SYSTEM, 0, &idx) ? 0xFFFF : idx;
        nt_case = save;
        if (idx == 0xFFFF)
            return -1;
        ea_f.first = 0;
        ea_f.size = 0;
        ea_f.dir_idx = idx;
    }
    ea_f.drive = (unsigned char)d;
    ea_f.attr = ATTR_HIDDEN | ATTR_SYSTEM;
    ea_f.dir_clus = 0;
    ea_f.cache_clus = 0;
    ea_f.cache_pos = 0;
    ea_f.flags = IFS_F_WRITE;
    ea_drv = d;
    return 0;
}

static unsigned ea_rd(unsigned long pos, void *buf, unsigned n)
{
    int r = fat_read(EAF, pos, KDATA, (unsigned)buf, n);
    return r < 0 ? 0 : (unsigned)r;
}

static unsigned long ea_dc(void)
{
    return ea_rec[2] | ((unsigned)ea_rec[3] << 8) |
           ((unsigned long)(ea_rec[4] | ((unsigned)ea_rec[5] << 8)) << 16);
}

/* Namen des Verzeichnisses dc in den Zwischenspeicher */
static void ea_load(int d, clus_t dc)
{
    unsigned long pos;
    unsigned u, len, k;
    unsigned char __far *c;

    kf_ea_lock();                       /* Cache gehoert allen EA-Laufwerken */
    if (ea_cdrv == d && ea_cdc == dc)
        return;
    ea_cdrv = d;
    ea_cdc = dc;
    eac_last = 0;
    eac_used[0] = 0;
    if (!fat_ksel())
        return;
    if (ea_open(d, 0))
        return;                         /* keine EA-Datei: keine Namen    */
    if (!eac_sel && !(eac_sel = kseg_alloc(16)))
        return;
    while (eac_n > 1)                   /* Stuecke vom letzten grossen Ordner */
        mem_free(eac_pg[--eac_n], 16);
    if (!eac_n) {
        if (!(eac_pg[0] = mem_alloc(16)))
            return;
        eac_n = 1;
    }
    eac_cur = 0xFF;
    c = eac_map(0);
    for (pos = 0; pos + EA_HDR <= ea_f.size; pos += (unsigned long)u << 5) {
        unsigned ea_cused;
        if (ea_rd(pos, ea_rec, EA_HDR) != EA_HDR || !(u = ea_rec[1]))
            break;
        if ((ea_rec[0] != 'L' && ea_rec[0] != 'W') || ea_dc() != dc)
            continue;
        len = ea_rec[17];
        if (eac_used[eac_last] + 13 + 3 * len > EAC_SZ) {   /* naechstes Stueck */
            if (eac_last + 1 >= EAC_N)
                break;                  /* voll: Rest ohne lange Namen    */
            if (eac_last + 1 >= eac_n) {
                if (!(eac_pg[eac_n] = mem_alloc(16)))
                    break;
                eac_n++;
            }
            eac_used[++eac_last] = 0;
            c = eac_map(eac_last);
        }
        ea_cused = eac_used[eac_last];
        for (k = 0; k < 11; k++)
            c[ea_cused + k] = ea_rec[6 + k];
        c[ea_cused + 11] = (unsigned char)len;
        c[ea_cused + 12] = 0;
        if (ea_rec[0] == 'W') {         /* UTF-16 (Unicode Stufe 1)      */
            unsigned short __far *u = (unsigned short __far *)(c + ea_cused + 13 + len);
            unsigned char b;
            if (fat_read(EAF, pos + EA_HDR, eac_sel, ea_cused + 13 + len, 2 * len) != (int)(2 * len))
                break;
            for (k = 0; k < len; k++) {
                b = uni2cp(u[k]);
                if (!b)
                    b = '_', c[ea_cused + 12] = 1;
                c[ea_cused + 13 + k] = b;
            }
        } else {
            if (ea_rd(pos + EA_HDR, ea_rec + EA_HDR, len) != len)
                break;
            for (k = 0; k < len; k++) {
                unsigned char b = ea_rec[EA_HDR + k];
                if (b >= 0x80 && cp_utab) { /* CP437 -> aktive Codepage   */
                    unsigned uc = cp_uni[b - 0x80];
                    ((unsigned short __far *)(c + ea_cused + 13 + len))[k] = uc;
                    if ((b = uni2cp(uc)) == 0)
                        b = '_', c[ea_cused + 12] = 1;
                } else
                    ((unsigned short __far *)(c + ea_cused + 13 + len))[k] = b;
                c[ea_cused + 13 + k] = b;
            }
        }
        eac_used[eac_last] = ea_cused + 13 + len + (c[ea_cused + 12] ? 2 * len : 0);
    }
}

/* langer Name zum 8.3-Eintrag e (aus dem Zwischenspeicher); 1 = gefunden */
static int ea_find(const unsigned char __far *e, char *out)
{
    unsigned char __far *c;
    unsigned o, k, len, ch;
    if (!eac_sel || !eac_n || ea_cdrv < 0)
        return 0;
    for (ch = 0; ch <= eac_last; ch++) {
        c = eac_map(ch);
        for (o = 0; o < eac_used[ch]; o += 13 + len + (c[o + 12] ? 2 * len : 0)) {
            len = c[o + 11];
            for (k = 0; k < 11 && c[o + k] == e[k]; k++)
                ;
            if (k == 11) {
                for (k = 0; k < len; k++)
                    out[k] = (char)c[o + 13 + k];
                out[k] = 0;
                ea_uoff = c[o + 12] ? o + 13 + len : 0;
                ea_uchunk = (unsigned char)ch;
                return 1;
            }
        }
    }
    return 0;
}

/* der eben gefundene EA-Name geht an das Programm: Original merken */
static void uni_take_ea(const char *name)
{
    if (ea_uoff && eac_sel) {
        eac_map(ea_uchunk);
        uni_note(name, slen_f(name), eac_sel, ea_uoff);
    }
}

/* Satz zu (dc, n83) freigeben */
static void ea_del(int d, clus_t dc, const unsigned char *n83)
{
    unsigned long pos;
    unsigned u, k;
    kf_ea_lock();
    if (ea_open(d, 0))
        return;
    for (pos = 0; pos + EA_HDR <= ea_f.size; pos += (unsigned long)u << 5) {
        if (ea_rd(pos, ea_rec, EA_HDR) != EA_HDR || !(u = ea_rec[1]))
            break;
        if ((ea_rec[0] != 'L' && ea_rec[0] != 'W') || ea_dc() != dc)
            continue;
        for (k = 0; k < 11 && ea_rec[6 + k] == n83[k]; k++)
            ;
        if (k == 11) {
            ea_rec[0] = 0;              /* frei, Laenge bleibt            */
            fat_write(EAF, pos, KDATA, (unsigned)ea_rec, 1);
        }
    }
    fat_close(EAF);
    if (ea_cdrv == d && ea_cdc == dc)
        ea_cdrv = -1;
}

/* Satz anlegen (vorher gleichnamigen freigeben); freier Satz, der gross
   genug ist, sonst ans Ende. 0 oder IFS_* */
static int ea_add(int d, clus_t dc, const char *n83, const char *name)
{
    unsigned long pos, at = 0xFFFFFFFFUL;
    unsigned u, len, need, k, use, uo, w437 = 0;
    int rc;

    kf_ea_lock();
    ea_del(d, dc, (const unsigned char *)n83);
    if (ea_open(d, 1))
        return IFS_FULL;
    for (len = 0; name[len] && len < 255; len++)
        ;
    uo = uni_find(name, len);           /* Original bekannt: Satz 'W'    */
    if (!uo && cp_utab)                 /* andere Codepage: geht es in   */
        for (k = 0; k < len; k++)       /* CP437 ('L'), sonst 'W'        */
            if ((unsigned char)name[k] >= 0x80 &&
                !uni437(cp2uni((unsigned char)name[k])))
                w437 = 1;
    need = (EA_HDR + ((uo || w437) ? 2 * len : len) + 31) >> 5;
    use = need;
    for (pos = 0; pos + EA_HDR <= ea_f.size; pos += (unsigned long)u << 5) {
        if (ea_rd(pos, ea_rec, EA_HDR) != EA_HDR || !(u = ea_rec[1]))
            break;
        if (ea_rec[0] == 0 && u >= need) {
            at = pos;                   /* freien Satz nehmen             */
            use = u;
            break;
        }
    }
    if (at == 0xFFFFFFFFUL)
        at = ea_f.size;
    for (k = 0; k < sizeof(ea_rec); k++)
        ea_rec[k] = 0;
    ea_rec[0] = 'L';
    ea_rec[1] = (unsigned char)use;
    ea_rec[2] = (unsigned char)dc;
    ea_rec[3] = (unsigned char)(dc >> 8);
    ea_rec[4] = (unsigned char)(dc >> 16);  /* FAT32: Cluster ueber 65.535 */
    ea_rec[5] = (unsigned char)(dc >> 24);
    for (k = 0; k < 11; k++)
        ea_rec[6 + k] = (unsigned char)n83[k];
    ea_rec[17] = (unsigned char)len;
    if (uo) {                           /* W-Satz im Puffer des Tasks    */
        unsigned char __far *w = ufp(UNI_WB);
        const unsigned char __far *src = ufp(uo);
        ea_rec[0] = 'W';
        for (k = 0; k < (need << 5); k++)
            w[k] = k < EA_HDR ? ea_rec[k] : k < EA_HDR + 2 * len ? src[k - EA_HDR] : 0;
        rc = fat_write(EAF, at, ea_csel, UNI_WB,
                       at == ea_f.size ? need << 5 : EA_HDR + 2 * len);
    } else if (w437) {                  /* W-Satz aus der Codepage       */
        unsigned char __far *w = ufp(UNI_WB);
        ea_rec[0] = 'W';
        for (k = 0; k < (need << 5); k++)
            w[k] = k < EA_HDR ? ea_rec[k] : 0;
        for (k = 0; k < len; k++) {
            u = cp2uni((unsigned char)name[k]);
            w[EA_HDR + 2 * k] = (unsigned char)u;
            w[EA_HDR + 2 * k + 1] = (unsigned char)(u >> 8);
        }
        rc = fat_write(EAF, at, ea_csel, UNI_WB,
                       at == ea_f.size ? need << 5 : EA_HDR + 2 * len);
    } else {
        for (k = 0; k < len; k++) {     /* 'L' immer in CP437            */
            unsigned char b = (unsigned char)name[k];
            ea_rec[EA_HDR + k] = b >= 0x80 && cp_utab ? uni437(cp2uni(b)) : b;
        }
        /* am Ende ganze Einheiten schreiben, damit der naechste Satz passt */
        rc = fat_write(EAF, at, KDATA, (unsigned)ea_rec,
                       at == ea_f.size ? need << 5 : EA_HDR + len);
    }
    k = fat_close(EAF);
    if (ea_cdrv == d && ea_cdc == dc)
        ea_cdrv = -1;
    return rc < 0 ? rc : (int)k;
}

static unsigned char rm_on[11];                 /* Kontext */
#define on rm_on
static int __far fat_remove(int d, const char *path)
{
    struct found f;
    unsigned char __far *e;

    ferr = 0;
    if (revalidate(d) || walk(d, path, &f) || f.dc == NO_DC)
        return IFS_ERR;
    if (f.attr & (ATTR_DIR | 0x08))
        return IFS_ERR;
    if (f.attr & (ATTR_RO | ATTR_SYSTEM | ATTR_HIDDEN))
        return IFS_DENIED;
    drop_lfn(d, f.dc, f.idx);           /* lange Namen mit entfernen    */
    e = dir_entry(d, f.dc, f.idx);
    if (!e)
        return IFS_ERR;
    {
        unsigned k;
        for (k = 0; k < 11; k++)
            on[k] = e[k];
        e[0] = 0xE5;
        wr(d, de_lba);
        if (!ferr)
            free_chain(d, f.first);
        if (!vol[d].vfat)
            ea_del(d, f.dc, on);        /* EA-Satz mit entfernen         */
    }
    return done_op(0);
}
#undef on

static int __far fat_mkdir(int d, const char *path)
{
    struct found par, f;
    char n[11];
    unsigned char __far *e;
    unsigned idx, k;
    clus_t c;
    int rc;

    ferr = 0;
    if (revalidate(d) || split(d, path, &par, n))
        return IFS_ERR;
    if (exists(d, par.first, &f))
        return IFS_EXIST;
    c = alloc_cluster(d, 0);
    if (!c)
        return done_op(IFS_FULL);
    zero_cluster(d, c);
    rc = new_entry_n(d, par.first, n, ATTR_DIR, c, &idx);
    if (rc) {
        free_chain(d, c);
        return done_op(rc);
    }
    /* "." und ".." */
    if (blk_read(vol[d].blk, clus_lba(&vol[d], c)))
        return done_op(IFS_ERR);
    e = dma();
    for (k = 0; k < 11; k++)
        e[k] = e[32 + k] = ' ';
    e[0] = e[32] = e[33] = '.';
    e[11] = e[32 + 11] = ATTR_DIR;
    stamp(e);
    stamp(e + 32);
    put_clus(d, e, c);
    put_clus(d, e + 32, par.first);
    wr(d, clus_lba(&vol[d], c));
    return done_op(0);
}

static unsigned char rd_on[11];                 /* Kontext */
#define on rd_on
static int __far fat_rmdir(int d, const char *path)
{
    struct found f;
    unsigned char __far *e;
    unsigned i;

    ferr = 0;
    if (revalidate(d) || walk(d, path, &f) || f.first == 0 || !(f.attr & ATTR_DIR))
        return IFS_ERR;
    for (i = 0; (e = dir_entry(d, f.first, i)) != 0; i++) {
        if (e[0] == 0)
            break;
        if (kind(e) == 1 && e[0] != '.')
            return IFS_DENIED;          /* nicht leer                   */
    }
    drop_lfn(d, f.dc, f.idx);
    e = dir_entry(d, f.dc, f.idx);
    if (!e)
        return IFS_ERR;
    {
        unsigned k;
        for (k = 0; k < 11; k++)
            on[k] = e[k];
        e[0] = 0xE5;
        wr(d, de_lba);
        if (!ferr)
            free_chain(d, f.first);
        if (!vol[d].vfat)
            ea_del(d, f.dc, on);
    }
    return done_op(0);
}
#undef on

/* umbenennen oder verschieben (gleiches Laufwerk) */
static unsigned char mv_ent[32];                /* Kontext */
#define ent mv_ent
static int __far fat_move(int d, const char *from, const char *to)
{
    struct found f, par, g;
    char n[11];
    unsigned char __far *e;
    unsigned idx, k;
    int rc;

    ferr = 0;
    if (revalidate(d) || walk(d, from, &f) || (f.attr & ATTR_DIR && f.first == 0))
        return IFS_ERR;
    if (f.attr & 0x08)
        return IFS_ERR;
    if (split(d, to, &par, n))
        return IFS_ERR;
    if (exists(d, par.first, &g) && !(g.dc == f.dc && g.idx == f.idx))
        return IFS_EXIST;               /* (nur Schreibung aendern geht) */
    if (walk(d, from, &f))              /* lookup hat DMABUF benutzt    */
        return IFS_ERR;
    drop_lfn(d, f.dc, f.idx);           /* alter langer Name passt nicht mehr */
    if (par.first == f.dc && !ln_buf[0]) {  /* gleiches Verzeichnis, 8.3 */
        e = dir_entry(d, f.dc, f.idx);
        if (!e)
            return IFS_ERR;
        for (k = 0; k < 11; k++)
            ent[k] = e[k], e[k] = (unsigned char)n[k];
        e[12] = nt_case;
        wr(d, de_lba);
        if (!vol[d].vfat)
            ea_del(d, f.dc, ent);
        return done_op(0);
    }
    e = dir_entry(d, f.dc, f.idx);
    if (!e)
        return IFS_ERR;
    far_copy(KDATA, (unsigned)ent, DMABUF, FP_OFF(e), 32);
    e[0] = 0xE5;                        /* alten Platz frei, damit ein    */
    wr(d, de_lba);                      /* langer Name ihn nutzen kann    */
    if (ln_buf[0] && vol[d].vfat)
        rc = new_entry_lfn(d, par.first, ln_buf, n, ent[11], 0, &idx);
    else
        rc = dir_slot(d, par.first, &idx);
    if (rc) {                           /* kein Platz: Eintrag zurueck    */
        e = dir_entry(d, f.dc, f.idx);
        if (e) {
            e[0] = ent[0];
            wr(d, de_lba);
        }
        return done_op(rc);
    }
    e = dir_entry(d, par.first, idx);
    if (!e)
        return done_op(IFS_ERR);
    far_copy(DMABUF, FP_OFF(e), KDATA, (unsigned)ent, 32);
    for (k = 0; k < 11; k++)
        e[k] = (unsigned char)n[k];
    e[12] = ln_buf[0] ? 0 : nt_case;
    wr(d, de_lba);                      /* (alter Platz ist schon frei)   */
    if (!vol[d].vfat) {                 /* EA: Satz umziehen              */
        ea_del(d, f.dc, ent);
        if (ln_buf[0])
            ea_add(d, par.first, n, ln_buf);
    }
    if ((f.attr & ATTR_DIR) && !ferr && par.first != f.dc) { /* ".." zeigt auf das neue Elternverzeichnis */
        e = dir_entry(d, f.first, 1);
        if (e && e[0] == '.' && e[1] == '.') {
            put_clus(d, e, par.first);
            wr(d, de_lba);
        }
    }
    return done_op(0);
}
#undef ent

/* freier Platz in Bytes */
/* freie Cluster: einmal zaehlen, danach fuehrt fat_set() den Zaehler mit.
   FAT16 sektorweise direkt im Cache - 65536 Einzelaufrufe kosten auf dem
   286 spuerbar Zeit. */
static int __far fat_space(int d, unsigned long *bytes)
{
    struct volume *v = &vol[d];
    unsigned c, end = (unsigned)v->clusters + 2, k, last;
    clus_t n = 0, c32;

    if (revalidate(d))
        return IFS_ERR;
    if (!v->nfree_ok) {
        if (IS32(v)) {                  /* sektorweise, 128 Eintraege     */
            for (c32 = 0; c32 < v->clusters + 2; c32 += 128) {
                if (fat_load(d, c32 >> 7))
                    return IFS_ERR;
                for (k = 0; k < 128; k++)
                    if (c32 + k >= 2 && c32 + k < v->clusters + 2 &&
                        !(fcache[k * 4] | fcache[k * 4 + 1] | fcache[k * 4 + 2] |
                          (fcache[k * 4 + 3] & 0x0F)))
                        n++;
            }
        } else if (v->fat16) {
            for (c = 2; c < end && c >= 2; c = last + 1) {
                if (fat_load(d, c >> 8))
                    return IFS_ERR;
                last = c | 255;
                if (last >= end)
                    last = end - 1;
                for (k = c; ; k++) {
                    if (fcache[(k & 255) * 2] == 0 && fcache[(k & 255) * 2 + 1] == 0)
                        n++;
                    if (k == last)
                        break;
                }
                if (last == 0xFFFF)
                    break;
            }
        } else {
            for (c = 2; c < end; c++)
                if (fat_raw(d, c) == 0)
                    n++;
        }
        v->nfree = n;
        v->nfree_ok = 1;
    }
    *bytes = shl((unsigned long)v->nfree, v->spc_shift + 9);
    return 0;
}

/* Attribute und/oder Zeitstempel eines Eintrags setzen */
static int __far fat_setinfo(int d, const char *path, unsigned mask, unsigned attr,
                       unsigned date, unsigned time)
{
    struct found f;
    unsigned char __far *e;

    ferr = 0;
    if (revalidate(d) || walk(d, path, &f) || f.dc == NO_DC)
        return IFS_ERR;
    e = dir_entry(d, f.dc, f.idx);
    if (!e)
        return IFS_ERR;
    if (mask & 1)
        e[11] = (unsigned char)((e[11] & (ATTR_DIR | 0x08)) |
                                (attr & (ATTR_RO | ATTR_HIDDEN | ATTR_SYSTEM | 0x20)));
    if (mask & 2) {
        put16(e, 22, time);
        put16(e, 24, date);
    }
    wr(d, de_lba);
    return done_op(0);
}

/* Datentraegerbezeichnung: Eintrag mit Attribut 08h im Stamm, dazu die
   Kopie im erweiterten BPB (ab 2Bh, wenn Signatur 29h) */
static int __far fat_label(int d, char *lbl, unsigned long *serial, const char *nl)
{
    unsigned char __far *e;
    unsigned i, k, eb, found = 0xFFFF;
    char n[11];

    ferr = 0;
    if (revalidate(d))
        return IFS_ERR;
    lbl[0] = 0;
    for (i = 0; (e = dir_entry(d, 0, i)) != 0 && e[0]; i++)
        if (e[0] != 0xE5 && e[11] != 0x0F && (e[11] & 0x08)) {
            for (k = 0; k < 11; k++)
                lbl[k] = (char)e[k];
            for (k = 11; k && lbl[k - 1] == ' '; k--)
                ;
            lbl[k] = 0;
            found = i;
            break;
        }
    *serial = 0;
    eb = IS32(&vol[d]) ? 28 : 0;        /* erweiterter BPB bei FAT32 +28  */
    if (blk_read(vol[d].blk, vol[d].start) == 0 && dma()[38 + eb] == 0x29)
        *serial = rd32(39 + eb);
    if (!nl)
        return 0;
    for (k = 0; k < 11; k++)
        n[k] = *nl ? *nl++ : ' ';
    if (found != 0xFFFF) {
        e = dir_entry(d, 0, found);
        if (!e)
            return IFS_ERR;
        if (n[0] == ' ')
            e[0] = 0xE5;                /* Bezeichnung loeschen        */
        else
            for (k = 0; k < 11; k++)
                e[k] = (unsigned char)n[k];
        stamp(e);
        wr(d, de_lba);
    } else if (n[0] != ' ') {
        int rc = new_entry(d, 0, n, 0x08, 0, &i);
        if (rc)
            return rc;
    }
    if (!ferr && blk_read(vol[d].blk, vol[d].start) == 0 && dma()[38 + eb] == 0x29) {
        for (k = 0; k < 11; k++)
            dma()[43 + eb + k] = n[0] == ' ' ? "NO NAME    "[k] : n[k];
        wr(d, vol[d].start);
    }
    return done_op(0);
}

/* Pfad -> 8.3-Namen (shortn) oder Namen in Originalschreibung */
static int __far fat_namepath(int d, const char *path, char *out, int shortn)
{
    struct found f;
    char n[11], s[IFS_NAMELEN];
    const char *p = path, *q;
    unsigned o = 0, k;
    unsigned char __far *e;

    f.first = 0;
    f.attr = ATTR_DIR;
    while (*p == '\\')
        p++;
    out[0] = 0;
    while (*p) {
        for (q = p; *q && *q != '\\'; q++)
            ;
        if (!(f.attr & ATTR_DIR) ||
            lookup_name(d, f.first, p, (unsigned)(q - p),
                        to83(p, (unsigned)(q - p), n) ? 0 : n, &f) ||
            !(e = dir_entry(d, f.dc, f.idx)))
            return -1;
        name83(e, s);
        if (!shortn && !vol[d].vfat) {  /* EA: Name aus dem Zwischenspeicher */
            unsigned dc2 = f.dc;
            ea_load(d, dc2);
            e = dir_entry(d, f.dc, f.idx);
            if (e && ea_find(e, ea_tmp)) {
                uni_take_ea(ea_tmp);
                for (k = 0; ea_tmp[k] && k < IFS_NAMELEN - 1; k++)
                    s[k] = ea_tmp[k], s[k + 1] = 0;
            }
        } else if (!shortn) {           /* langen Namen dazu suchen      */
            unsigned i = f.idx;
            unsigned char __far *x;
            lfn_have = 0;
            if (i > 20)
                i -= 20;
            else
                i = 0;
            for (; i <= f.idx && (x = dir_entry(d, f.dc, i)) != 0; i++)
                if (!lfn_feed(x) && i < f.idx)
                    lfn_have = 0;
            if (x && lfn_valid(x)) {
                uni_take_lfn();
                for (k = 0; lfn[k] && k < IFS_NAMELEN - 1; k++)
                    s[k] = lfn[k], s[k + 1] = 0;
            }
            else if (x) {               /* NT-Kleinschreibung            */
                unsigned dot = 0;
                for (k = 0; s[k]; k++)
                    if (s[k] == '.')
                        dot = 1;
                    else if (s[k] >= 'A' && s[k] <= 'Z' && (x[12] & (dot ? 0x10 : 0x08)))
                        s[k] = (char)(s[k] + 32);
            }
        }
        for (k = 0; s[k]; k++)
            ;
        if (o + 1 + k >= IFS_PATHLEN)
            return -1;
        out[o++] = '\\';
        for (k = 0; s[k]; k++)
            out[o++] = s[k];
        out[o] = 0;
        p = q;
        while (*p == '\\')
            p++;
    }
    if (!o) {
        out[0] = '\\';
        out[1] = 0;
    }
    return 0;
}

struct ifs_driver fat_driver = {
    "FAT", fat_mount, fat_open, fat_read, fat_readdir, fat_isdir,
    fat_create, fat_write, fat_close, fat_remove, fat_mkdir, fat_rmdir,
    fat_move, fat_space, fat_setinfo, fat_label, fat_namepath, fat_truncate
};

/* ---- IFS-Verteiler ----------------------------------------------------- */
int __far ifs_mount(int drive, int blk, unsigned long start, struct ifs_driver *drv)
{
    struct volume *v = &vol[drive];
    v->blk = blk;
    v->start = start;
    v->drv = drv;
    v->locked = 0;
    if (drv->mount(drive))
        return -1;
    v->used = 1;
    return 0;
}

unsigned char ifs_notready;

/* Diskette A: beim Start von Festplatte erst beim ersten Zugriff */
int floppy_lazy;

/* Laufwerk vorhanden (auch unformatiert: FORMAT, DosOpenDrive) */
int __far vol_present(int d)
{
    if ((unsigned)d >= NDRIVE)
        return 0;
    if (d < 2 && !vol[d].used && (floppy_lazy & (1 << d))) {
        blk_media(vol[d].blk);          /* Format erkennen            */
        if (blk_nomedia(vol[d].blk))
            ifs_notready = 1;           /* leer: FAPI meldet "nicht bereit" */
        else
            ifs_mount(d, vol[d].blk, 0, &fat_driver);
    }
    if (d >= 2 && !vol[d].used && vol[d].present && IS_REM(vol[d].blk))
        revalidate(d);                  /* Wechselplatte: Medium da?   */
    return vol[d].used || vol[d].present;
}

/* Wechselplatte/Diskette jetzt pruefen (SELECTDISK, QFSInfo) */
int __far vol_check(int d)
{
    VLOCK(d);
    return revalidate(d);
}

/* Dateisystem eingehaengt; gesperrte Laufwerke (CHKDSK /F, FORMAT) sind zu */
static int vol_ok(int d)
{
    return vol_present(d) && vol[d].used && !vol[d].locked;
}
#define VOL_OK(d) vol_ok(d)

int __far ifs_open(int drive, const char *path, struct ifs_file __far *f)
{
    VLOCK(drive);
    return VOL_OK(drive) ? vol[drive].drv->open(drive, path, f) : -1;
}

int __far ifs_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
             unsigned off, unsigned len)
{
    VLOCK(f->drive);
    return VOL_OK(f->drive) ? vol[f->drive].drv->read(f, pos, sel, off, len) : -1;
}

int __far ifs_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *d)
{
    VLOCK(drive);
    return VOL_OK(drive) ? vol[drive].drv->readdir(drive, dir, n, d) : -1;
}

int __far ifs_namepath(int drive, const char *path, char *out, int shortn)
{
    VLOCK(drive);
    if (VOL_OK(drive) && vol[drive].drv->namepath)
        return vol[drive].drv->namepath(drive, path, out, shortn);
    return -1;
}

int __far ifs_isdir(int drive, const char *path)
{
    VLOCK(drive);
    return VOL_OK(drive) ? vol[drive].drv->isdir(drive, path) : 0;
}

#define HAS(d, fn) (VOL_OK(d) && vol[d].drv->fn)

/* vor jedem Schreiben: Laufwerk als "in Benutzung" markieren */
static void mark_in_use(int d)
{
    struct volume *v = &vol[d];
    clus_t val;

    if (v->in_use || v->drv != &fat_driver || !v->fat16 || IS_FLOPPY(v->blk) ||
        v->locked)
        return;
    v->in_use = 1;
    val = fat_raw(d, 1);
    if (val & CLEAN(v)) {               /* sofort auf die Platte, vor   */
        fat_set(d, 1, val & ~CLEAN(v)); /* allen verzoegerten Daten     */
        bc_sync = 1;
        fsinfo(d, 0);
        fat_flush();
        bc_sync = 0;
    }
}

void __far ifs_shutdown(void)
{
    int d;
    clus_t val;

    for (d = 0; d < NDRIVE; d++)
        if (vol[d].used && vol[d].in_use && !vol[d].locked) {
            val = fat_raw(d, 1);
            fat_set(d, 1, val | CLEAN(&vol[d]));
            bc_sync = 1;
            fsinfo(d, 1);
            fat_flush();
            bc_sync = 0;
            vol[d].in_use = 0;
        }
}

#define MARK(d) do { if (VOL_OK(d)) mark_in_use(d); } while (0)

int __far ifs_create(int drive, const char *path, struct ifs_file __far *f)
{
    VLOCK(drive);
    MARK(drive);
    return HAS(drive, create) ? vol[drive].drv->create(drive, path, f) : IFS_WP;
}

int __far ifs_write(struct ifs_file __far *f, unsigned long pos, unsigned sel,
              unsigned off, unsigned len)
{
    VLOCK(f->drive);
    MARK(f->drive);
    return HAS(f->drive, write) ? vol[f->drive].drv->write(f, pos, sel, off, len) : IFS_WP;
}

/* Groesse setzen: kuerzen ueber den Treiber, verlaengern mit Nullen */
int __far ifs_setsize(struct ifs_file __far *f, unsigned long size)
{
    static char zero[512];
    unsigned n;
    int r;
    VLOCK(f->drive);
    MARK(f->drive);
    if (!HAS(f->drive, write) || !(f->flags & IFS_F_WRITE))
        return IFS_WP;
    if (size < f->size)
        return HAS(f->drive, truncate) ? vol[f->drive].drv->truncate(f, size) : IFS_DENIED;
    while (f->size < size) {
        n = size - f->size > sizeof(zero) ? sizeof(zero) : (unsigned)(size - f->size);
        r = vol[f->drive].drv->write(f, f->size, KDATA, (unsigned)zero, n);
        if (r < 0)
            return r;
        if (r == 0)
            return IFS_FULL;
    }
    return 0;
}

int __far ifs_close(struct ifs_file __far *f)
{
    VLOCK(f->drive);
    return HAS(f->drive, close) ? vol[f->drive].drv->close(f) : 0;
}

int __far ifs_remove(int drive, const char *path)
{
    VLOCK(drive);
    MARK(drive);
    return HAS(drive, remove) ? vol[drive].drv->remove(drive, path) : IFS_WP;
}

int __far ifs_mkdir(int drive, const char *path)
{
    VLOCK(drive);
    MARK(drive);
    return HAS(drive, mkdir) ? vol[drive].drv->mkdir(drive, path) : IFS_WP;
}

int __far ifs_rmdir(int drive, const char *path)
{
    VLOCK(drive);
    MARK(drive);
    return HAS(drive, rmdir) ? vol[drive].drv->rmdir(drive, path) : IFS_WP;
}

int __far ifs_move(int drive, const char *from, const char *to)
{
    VLOCK(drive);
    MARK(drive);
    return HAS(drive, move) ? vol[drive].drv->move(drive, from, to) : IFS_WP;
}

int __far ifs_setinfo(int d, const char *path, unsigned mask, unsigned attr,
                unsigned date, unsigned time)
{
    VLOCK(d);
    MARK(d);
    return HAS(d, setinfo) ? vol[d].drv->setinfo(d, path, mask, attr, date, time) : IFS_WP;
}

int __far ifs_label(int d, char *lbl, unsigned long *serial, const char *newlbl)
{
    VLOCK(d);
    if (newlbl)
        MARK(d);
    if (!HAS(d, label)) {
        if (ifs_notready)
            return IFS_ERR;
        lbl[0] = 0;
        *serial = 0;
        return newlbl ? IFS_WP : 0;
    }
    return vol[d].drv->label(d, lbl, serial, newlbl);
}

int __far ifs_space(int drive, unsigned long *free_bytes)
{
    VLOCK(drive);
    if (!VOL_OK(drive))
        return IFS_ERR;
    if (!vol[drive].drv->space) {
        *free_bytes = 0;                /* nur lesbar (CD): nichts frei */
        return 0;
    }
    return vol[drive].drv->space(drive, free_bytes);
}

/* Laufwerk from nach to verschieben, die dazwischen rutschen eins nach
   hinten (from > to). Nur beim Start, solange keine Datei offen ist. */
void __far vol_move(int from, int to)
{
    static struct volume tmp;
    int d;

    if (from <= to)
        return;
    fat_flush();
    fc_drive = -1;
    fc_dirty = 0;
    dir_cache_reset();
    tmp = vol[from];
    for (d = from; d > to; d--)
        vol[d] = vol[d - 1];
    vol[to] = tmp;
}

/* nach DosOpenDrive (Sperre aufgehoben): alles Zwischengespeicherte des
   Laufwerks verwerfen und neu einhaengen - CHKDSK /F kann FAT und
   Verzeichnisse geaendert haben */
void __far vol_reset(int d)
{
    fat_flush();
    if (fc_drive == d) {
        fc_drive = -1;
        fc_dirty = 0;
    }
    dir_cache_reset();
    if (vol[d].drv)                     /* FORMAT: jetzt evtl. lesbar  */
        vol[d].used = vol[d].drv->mount(d) == 0;
}

/* Kontext je Task (Phase 6): laufende Variablen einer Operation, siehe
   kernel.c ctx_take. Gemeinsame Caches (fc_*, lc_*, ea_drv/ea_c*, skx_*)
   gehoeren nicht hinein. */
const struct ctxblk fat_ctx[] = {
    { lfn, sizeof(lfn) }, { &lfn_sum, sizeof(lfn_sum) }, { &lfn_next, sizeof(lfn_next) },
    { &lfn_have, sizeof(lfn_have) }, { &lfn_lossy, sizeof(lfn_lossy) },
    { ea_tmp, sizeof(ea_tmp) }, { ln_buf, sizeof(ln_buf) },
    { ea_rec, sizeof(ea_rec) }, { &sp_comp, sizeof(sp_comp) }, { &sp_len, sizeof(sp_len) },
    { &nt_case, sizeof(nt_case) }, { &ferr, sizeof(ferr) }, { &de_lba, sizeof(de_lba) },
    { ifs_orig, sizeof(ifs_orig) }, { &ifs_notready, sizeof(ifs_notready) },
    { sp_pbuf, sizeof(sp_pbuf) }, { rm_on, sizeof(rm_on) }, { rd_on, sizeof(rd_on) },
    { mv_ent, sizeof(mv_ent) }
};
const unsigned fat_nctx = sizeof(fat_ctx) / sizeof(fat_ctx[0]);
