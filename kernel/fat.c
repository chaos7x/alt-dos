/*
 * AT-DOS 5.0 - IFS-Treiber FAT (12 und 16 Bit) und IFS-Verteiler
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
#define FAT_END 0xFFFF
static unsigned char fcache[1024];
static int           fc_drive = -1;
static unsigned      fc_sec;            /* Sektor relativ zum FAT-Anfang  */
static unsigned char fc_dirty;
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
            far_copy(DMABUF, 0, KDATA, (unsigned)fcache + (i << 9), 512);
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

static int fat_load(int d, unsigned sec)
{
    struct volume *v = &vol[d];
    if (fc_drive == d && fc_sec == sec)
        return 0;
    if (fat_flush())
        return -1;
    fc_drive = -1;
    if (blk_read(v->blk, v->fat_lba + sec))
        return -1;
    far_copy(KDATA, (unsigned)fcache, DMABUF, 0, 512);
    if (sec + 1 < v->spf) {
        if (blk_read(v->blk, v->fat_lba + sec + 1))
            return -1;
        far_copy(KDATA, (unsigned)fcache + 512, DMABUF, 0, 512);
    }
    fc_drive = d;
    fc_sec = sec;
    return 0;
}

static int valid(struct volume *v, unsigned c)
{
    return c >= 2 && c < v->clusters + 2;
}

/* Lage eines Eintrags: Sektor relativ zur FAT, Byte im Cache */
static void fat_pos(struct volume *v, unsigned c, unsigned *sec, unsigned *off)
{
    unsigned o;
    if (v->fat16) {
        *sec = c >> 8;
        *off = (c & 255) * 2;
    } else {
        o = c + (c >> 1);
        *sec = o >> 9;
        *off = o & 511;
    }
}

/* roher FAT-Eintrag; bei Lesefehler ein "defekt"-Wert, nie 0 */
static unsigned fat_raw(int d, unsigned c)
{
    struct volume *v = &vol[d];
    unsigned sec, off, val;

    fat_pos(v, c, &sec, &off);
    if (fat_load(d, sec))
        return 0xFFF7;
    val = fcache[off] | ((unsigned)fcache[off + 1] << 8);
    if (!v->fat16)
        val = (c & 1) ? val >> 4 : val & 0xFFF;
    return val;
}

static unsigned next_cluster(int d, unsigned c)
{
    unsigned val = fat_raw(d, c);
    return valid(&vol[d], val) ? val : FAT_END;
}

static void fat_set(int d, unsigned c, unsigned val)
{
    struct volume *v = &vol[d];
    unsigned sec, off, old;

    if (v->nfree_ok) {                  /* Zaehler freier Cluster mitfuehren */
        old = fat_raw(d, c);
        if (old == 0 && val != 0)
            v->nfree--;
        else if (old != 0 && val == 0)
            v->nfree++;
    }
    fat_pos(v, c, &sec, &off);
    if (fat_load(d, sec)) {
        if (!ferr)
            ferr = IFS_ERR;
        return;
    }
    if (v->fat16) {
        fcache[off] = (unsigned char)val;
        fcache[off + 1] = (unsigned char)(val >> 8);
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
static unsigned alloc_cluster(int d, unsigned prev)
{
    struct volume *v = &vol[d];
    unsigned n, c = v->hint;

    for (n = 0; n < v->clusters; n++, c++) {
        if (!valid(v, c))
            c = 2;
        if (fat_raw(d, c) == 0) {
            fat_set(d, c, v->fat16 ? 0xFFFF : 0x0FFF);
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
static void free_chain(int d, unsigned c)
{
    struct volume *v = &vol[d];
    unsigned n, guard = 0;

    while (valid(v, c) && guard++ < v->clusters) {
        n = fat_raw(d, c);
        fat_set(d, c, 0);
        if (c < v->hint)
            v->hint = c;
        c = n;
    }
    dir_cache_reset();
}

static unsigned long clus_lba(struct volume *v, unsigned c)
{
    return v->data_lba + shl((unsigned long)(c - 2), v->spc_shift);
}

/* ---- Einhaengen -------------------------------------------------------- */
/* FAT16: FAT[1] Bit 15 = sauber abgemeldet (wie Windows 95/NT; MS-DOS
   ignoriert es). Beim ersten Schreiben loeschen, beim Herunterfahren
   setzen. Beim Start prueft COMMAND /P Laufwerke ohne das Bit. */
#define CLEAN_BIT 0x8000

static int fat_mount(int d)
{
    struct volume *v = &vol[d];
    unsigned bps, reserved, nfats, spf, spc, n;
    unsigned long total, cl;

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
    if (bps != 512 || spf == 0 || nfats == 0 || nfats > 2 || spc == 0 || spc > 64)
        return -1;                              /* FAT32 hat spf == 0   */
    for (n = 0; (1U << n) < spc; n++)
        ;
    if ((1U << n) != spc)
        return -1;
    v->spc = (unsigned char)spc;
    v->nfats = (unsigned char)nfats;
    v->spf = spf;
    v->hint = 2;
    v->nfree_ok = 0;
    v->spc_shift = (unsigned char)n;
    v->sectors  = total;
    v->fat_lba  = v->start + reserved;
    v->root_lba = v->fat_lba + nfats * spf;
    v->data_lba = v->root_lba + (v->rootents + 15) / 16;
    cl = shr(total - (v->data_lba - v->start), n);
    if (cl >= 65525UL)
        return -1;                              /* waere FAT32          */
    v->clusters = (unsigned)cl;
    v->fat16 = cl >= 4085;
    v->in_use = 0;
    v->was_dirty = 0;
    if (v->fat16 && !IS_FLOPPY(v->blk) && blk_read(v->blk, v->fat_lba) == 0)
        v->was_dirty = !(rd16(2) & CLEAN_BIT);  /* FAT[1] Bit 15        */
    v->vfat = lfn_cfg == 1;
    if (lfn_cfg == 0) {                 /* AUTO: VFAT, wenn der Stamm schon */
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
static unsigned lc_dir, lc_n, lc_clus;
static unsigned long de_lba;            /* Sektor des letzten dir_entry()  */

static void ea_forget(int d);
static void dir_cache_reset(void)
{
    int d;
    lc_drive = -1;
    for (d = 0; d < NDRIVE; d++)        /* EA-Datei/Namen neu lesen       */
        ea_forget(d);
}

/* i-ter Eintrag des Verzeichnisses ab Cluster dc (0 = Stamm) im DMA-Puffer */
static unsigned char __far *dir_entry(int d, unsigned dc, unsigned i)
{
    struct volume *v = &vol[d];
    unsigned epc, n, c, k;

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

struct found {
    unsigned first;
    unsigned char attr;
    unsigned long size;
    unsigned dc, idx;                   /* Lage des Eintrags            */
};

/* ---- Lange Namen (VFAT, Windows 95) lesen --------------------------------
   Vor dem 8.3-Eintrag stehen Eintraege mit Attribut 0Fh, letzter Teil
   zuerst (Ordnungszahl | 40h), je 13 UCS-2-Zeichen an den Stellen 1-10,
   14-25, 28-31, dazu die Pruefsumme des 8.3-Namens (Byte 13). Zeichen
   ausserhalb von CP437 werden zu '_'. */
static char lfn[IFS_NAMELEN];
static unsigned char lfn_sum, lfn_next; /* naechste erwartete Ordnungszahl */
static unsigned char lfn_have;          /* Teile gesammelt                */
static const unsigned char lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
/* Latin-1 A0h-FFh -> CP437 (0 = keins) */
static const unsigned char lat2cp[96] = {
    0xFF, 0xAD, 0x9B, 0x9C, 0, 0x9D, 0x7C, 0x15, 0, 0, 0xA6, 0xAE, 0xAA, 0x2D, 0, 0,
    0xF8, 0xF1, 0xFD, 0, 0, 0xE6, 0x14, 0xFA, 0, 0, 0xA7, 0xAF, 0xAC, 0xAB, 0, 0xA8,
    0, 0, 0, 0, 0x8E, 0x8F, 0x92, 0x80, 0, 0x90, 0, 0, 0, 0, 0, 0,
    0, 0xA5, 0, 0, 0, 0, 0x99, 0, 0, 0, 0, 0, 0x9A, 0, 0, 0xE1,
    0x85, 0xA0, 0x83, 0, 0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
    0, 0xA4, 0x95, 0xA2, 0x93, 0, 0x94, 0xF6, 0, 0x97, 0xA3, 0x96, 0x81, 0, 0, 0x98 };

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
    } else if (!lfn_have || ord != lfn_next - 1 || e[13] != lfn_sum) {
        lfn_have = 0;
        return 1;
    }
    lfn_next = (unsigned char)ord;
    p = (ord - 1) * 13;
    for (i = 0; i < 13; i++) {
        c = e[lfn_pos[i]] | ((unsigned)e[lfn_pos[i] + 1] << 8);
        if (c == 0) {
            lfn[p + i] = 0;
            break;
        }
        lfn[p + i] = (char)(c < 0x80 ? c : c >= 0xA0 && c <= 0xFF && lat2cp[c - 0xA0] ?
                                lat2cp[c - 0xA0] : '_');
    }
    return 1;
}

/* eigenes Format (Stufe 3), weiter unten */
static void ea_load(int d, unsigned dc);
static int  ea_find(const unsigned char __far *e, char *out);
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
static void drop_lfn(int d, unsigned dc, unsigned idx)
{
    unsigned char __far *e;
    unsigned char s, last;
    if (!idx || dc == 0xFFFF)
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
static int lookup_name(int d, unsigned dc, const char *s, unsigned len,
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
        if (!ok && !vol[d].vfat && ea_find(e, ea_tmp)) {   /* EA-Name    */
            for (k = 0; k < len && ea_tmp[k] && upc(ea_tmp[k]) == s[k]; k++)
                ;
            ok = k == len && !ea_tmp[k];
        }
        if (!ok && n11) {
            for (k = 0; k < 11 && e[k] == (unsigned char)n11[k]; k++)
                ;
            ok = k == 11;
        }
        if (ok) {
            f->dc = dc;
            f->idx = i;
            f->first = e[26] | ((unsigned)e[27] << 8);
            f->attr  = e[11];
            f->size  = (e[28] | ((unsigned)e[29] << 8)) |
                       ((unsigned long)(e[30] | ((unsigned)e[31] << 8)) << 16);
            return 0;
        }
    }
    return -1;
}

static int lookup(int d, unsigned dc, const char *n, struct found *f)
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
            f->first = e[26] | ((unsigned)e[27] << 8);
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
    f->dc = 0xFFFF;                     /* Stamm hat keinen Eintrag     */
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

static int fat_open(int d, const char *path, struct ifs_file __far *fl)
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

static int fat_isdir(int d, const char *path)
{
    struct found f;
    return walk(d, path, &f) == 0 && (f.attr & ATTR_DIR);
}

static int fat_readdir(int d, const char *dir, unsigned want, struct ifs_dirent *de)
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
        } else if (!vol[d].vfat && ea_find(e, de->name)) {
            /* langer Name aus EA DATA. SF                              */
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

static int fat_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
                    unsigned off, unsigned len)
{
    int d = f->drive;
    struct volume *v = &vol[d];
    unsigned csz = (unsigned)v->spc * 512, c, so, b, n, ra, done = 0;
    unsigned long cpos;

    if (pos >= f->size)
        return 0;
    if (f->size - pos < len)
        len = (unsigned)(f->size - pos);

    if (f->cache_clus && pos >= f->cache_pos) {
        c = f->cache_clus;                      /* weiter, wo wir waren */
        cpos = f->cache_pos;
    } else {
        c = f->first;
        cpos = 0;
    }
    while (pos - cpos >= csz) {                 /* zum Cluster vorlaufen */
        if (!valid(v, c))
            return -1;
        c = next_cluster(d, c);
        cpos += csz;
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

static void zero_cluster(int d, unsigned c)
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
static int name_for(int d, unsigned dc, const char *comp, unsigned clen, char *n83);
static const char *sp_comp;             /* letzter Pfadteil nach split()  */
static unsigned sp_len;

/* Eltern suchen, Namen fuer den neuen Eintrag bestimmen (name_for) */
static int split(int d, const char *path, struct found *par, char *n83)
{
    static char pbuf[IFS_PATHLEN];
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

/* gibt es den Namen (8.3 oder lang) im Verzeichnis schon? */
static int exists(int d, unsigned dc, struct found *f)
{
    char n[11];
    return lookup_name(d, dc, sp_comp, sp_len,
                       to83(sp_comp, sp_len, n) ? 0 : n, f) == 0;
}

/* freien Eintrag im Verzeichnis dc; erweitert Unterverzeichnisse */
static int dir_slot(int d, unsigned dc, unsigned *idx)
{
    unsigned char __far *e;
    unsigned i, c, n;

    for (i = 0; (e = dir_entry(d, dc, i)) != 0; i++)
        if (e[0] == 0 || e[0] == 0xE5) {
            *idx = i;
            return 0;
        }
    if (dc == 0)
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

static int new_entry(int d, unsigned dc, const char *n83, unsigned char attr,
                     unsigned first, unsigned *idx)
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
    put16(e, 26, first);
    wr(d, de_lba);
    return 0;
}

/* ---- Lange Namen schreiben (VFAT, Stufe 2) --------------------------------
   Nur auf Laufwerken mit vol[].vfat (LONGNAMES=VFAT oder AUTO mit langen
   Eintraegen im Stamm). Name in 8.3 und nur Klein- oder nur Grossbuch-
   staben je Teil: NT-Kleinschreibungsbits (Byte 12), kein langer Eintrag.
   Sonst lange Eintraege + Kurzname NAME~n.EXT wie Windows 95. */
static char ln_buf[IFS_NAMELEN];        /* zu schreibender langer Name    */

static unsigned cp2uni(unsigned char c)
{
    unsigned i;
    if (c < 0x80)
        return c;
    for (i = 0; i < 96; i++)
        if (lat2cp[i] == c)
            return 0xA0 + i;
    return '_';
}

/* n freie Eintraege am Stueck (Unterverzeichnis wird verlaengert) */
static int dir_slots(int d, unsigned dc, unsigned n, unsigned *idx)
{
    unsigned char __far *e;
    unsigned i, run = 0, start = 0, c, nx;

    for (i = 0;; i++) {
        e = dir_entry(d, dc, i);
        if (!e) {
            if (dc == 0)
                return IFS_FULL;
            for (c = dc; (nx = next_cluster(d, c)) != FAT_END; c = nx)
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
static int new_entry_lfn(int d, unsigned dc, const char *ln, const char *n83,
                         unsigned char attr, unsigned first, unsigned *idx)
{
    unsigned char __far *e;
    unsigned len, k, j, i, pos, ord, start, ch;
    unsigned char sum;
    int rc;

    for (len = 0; ln[len]; len++)
        ;
    k = (len + 12) / 13;
    rc = dir_slots(d, dc, k + 1, &start);
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
            ch = pos < len ? cp2uni((unsigned char)ln[pos]) : pos == len ? 0 : 0xFFFF;
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
    put16(e, 26, first);
    wr(d, de_lba);
    *idx = start + k;
    return 0;
}

/* Kurzname zum langen Namen: NAME~n.EXT, im Verzeichnis eindeutig */
static void gen_short(int d, unsigned dc, const char *ln, char *n83)
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
static int name_for(int d, unsigned dc, const char *comp, unsigned clen, char *n83)
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
        if (!lo)
            return 0;                   /* alles gross: nur 8.3           */
        if (!mixed) {                   /* NT: Name/Endung ganz klein     */
            nt_case = (unsigned char)((partlo[0] ? 0x08 : 0) | (partlo[1] ? 0x10 : 0));
            return 0;
        }
        /* gemischt: VFAT-Eintrag bzw. EA-Satz bewahrt die Schreibung */
    } else {
        if (clen >= IFS_NAMELEN)
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
static int ea_add(int d, unsigned dc, const char *n83, const char *name);
static void ea_del(int d, unsigned dc, const unsigned char *n83);

static int new_entry_n(int d, unsigned dc, const char *n83, unsigned char attr,
                       unsigned first, unsigned *idx)
{
    int rc;
    if (ln_buf[0] && vol[d].vfat)
        return new_entry_lfn(d, dc, ln_buf, n83, attr, first, idx);
    rc = new_entry(d, dc, n83, attr, first, idx);
    if (rc == 0 && ln_buf[0])           /* EA-Laufwerk: Satz anlegen      */
        rc = ea_add(d, dc, n83, ln_buf);
    return rc;
}

/* Diskette: vor jeder Operation neu einhaengen, sie koennte gewechselt sein */
static int revalidate(int d)
{
    if (!IS_FLOPPY(vol[d].blk))
        return 0;
    if (blk_media(vol[d].blk) == 0 && vol[d].used)
        return 0;                       /* nicht gewechselt: alles gilt */
    fc_drive = -1;
    fc_dirty = 0;
    dir_cache_reset();
    blk_invalidate();
    return fat_mount(d) ? IFS_ERR : 0;
}

static int fat_create(int d, const char *path, struct ifs_file __far *fl)
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
        put16(e, 26, 0);
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
static unsigned next_or_alloc(int d, unsigned c)
{
    unsigned n = next_cluster(d, c);
    return n != FAT_END ? n : alloc_cluster(d, c);
}

static int fat_write(struct ifs_file __far *f, unsigned long pos, unsigned sel,
                     unsigned off, unsigned len)
{
    int d = f->drive;
    struct volume *v = &vol[d];
    unsigned csz = (unsigned)v->spc * 512, c, so, b, n, done = 0;
    unsigned long cpos, lba;

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

static int fat_close(struct ifs_file __far *f)
{
    unsigned char __far *e;
    int d = f->drive;

    ferr = 0;
    if (!(f->flags & IFS_F_DIRTY))
        return 0;
    e = dir_entry(d, f->dir_clus, f->dir_idx);
    if (!e)
        return IFS_ERR;
    put16(e, 26, f->first);
    put16(e, 28, (unsigned)f->size);
    put16(e, 30, (unsigned)(f->size >> 16));
    e[11] |= 0x20;
    stamp(e);
    wr(d, de_lba);
    f->flags &= ~IFS_F_DIRTY;
    return done_op(0);
}

/* ---- Lange Namen im eigenen Format (Stufe 3, Entscheidung des Nutzers) ---
   "EA DATA. SF" im Stamm (versteckt, System; MS-DOS-Werkzeuge sehen eine
   normale Datei). Saetze in Einheiten zu 32 Byte:
     0  'L' = langer Name, 0 = frei      1  Laenge in Einheiten
     2  Startcluster des Verzeichnisses (32 Bit, 0 = Stamm; FAT32-fest)
     6  8.3-Name (11 Byte) - der Rueckverweis auf die Datei
     17 Laenge des Namens, ab 18 der Name (CP437)
   Im Verzeichniseintrag steht nichts (OS/2 nimmt Offset 14h, bei FAT32 die
   oberen Clusterbits). Zwischenspeicher: die Namen eines Verzeichnisses
   (das zuletzt gelesene) in einem Kernelsegment, sonst kostete jede Suche
   Lesezugriffe; reicht er nicht (16 KB), fehlen die uebrigen Namen. */
#define EA_HDR   18
#define EA_CSIZE 16384U
static struct ifs_file ea_f;            /* EA DATA. SF von ea_drv         */
static int ea_drv = -1;
static const char ea_n83[11] = { 'E', 'A', ' ', 'D', 'A', 'T', 'A', ' ', 'S', 'F', ' ' };
static unsigned ea_csel, ea_cused, ea_cdc;
static int ea_cdrv = -1;
static unsigned char ea_rec[EA_HDR + IFS_NAMELEN + 32];

#define EAF ((struct ifs_file __far *)&ea_f)

static void ea_forget(int d)            /* Laufwerk neu/gewechselt        */
{
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
static void ea_load(int d, unsigned dc)
{
    unsigned long pos;
    unsigned u, len, k;
    unsigned char __far *c;

    if (ea_cdrv == d && ea_cdc == dc)
        return;
    ea_cdrv = d;
    ea_cdc = dc;
    ea_cused = 0;
    if (!ea_csel && !(ea_csel = kseg_alloc(EA_CSIZE)))
        return;
    if (ea_open(d, 0))
        return;                         /* keine EA-Datei: keine Namen    */
    c = MK_FP(ea_csel, 0);
    for (pos = 0; pos + EA_HDR <= ea_f.size; pos += (unsigned long)u << 5) {
        if (ea_rd(pos, ea_rec, EA_HDR) != EA_HDR || !(u = ea_rec[1]))
            break;
        if (ea_rec[0] != 'L' || ea_dc() != dc)
            continue;
        len = ea_rec[17];
        if (ea_cused + 12 + len > EA_CSIZE)
            break;                      /* voll: Rest ohne lange Namen    */
        if (ea_rd(pos + EA_HDR, ea_rec + EA_HDR, len) != len)
            break;
        for (k = 0; k < 11; k++)
            c[ea_cused + k] = ea_rec[6 + k];
        c[ea_cused + 11] = (unsigned char)len;
        for (k = 0; k < len; k++)
            c[ea_cused + 12 + k] = ea_rec[EA_HDR + k];
        ea_cused += 12 + len;
    }
}

/* langer Name zum 8.3-Eintrag e (aus dem Zwischenspeicher); 1 = gefunden */
static int ea_find(const unsigned char __far *e, char *out)
{
    unsigned char __far *c;
    unsigned o, k, len;
    if (!ea_csel || ea_cdrv < 0)
        return 0;
    c = MK_FP(ea_csel, 0);
    for (o = 0; o < ea_cused; o += 12 + len) {
        len = c[o + 11];
        for (k = 0; k < 11 && c[o + k] == e[k]; k++)
            ;
        if (k == 11) {
            for (k = 0; k < len; k++)
                out[k] = (char)c[o + 12 + k];
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

/* Satz zu (dc, n83) freigeben */
static void ea_del(int d, unsigned dc, const unsigned char *n83)
{
    unsigned long pos;
    unsigned u, k;
    if (ea_open(d, 0))
        return;
    for (pos = 0; pos + EA_HDR <= ea_f.size; pos += (unsigned long)u << 5) {
        if (ea_rd(pos, ea_rec, EA_HDR) != EA_HDR || !(u = ea_rec[1]))
            break;
        if (ea_rec[0] != 'L' || ea_dc() != dc)
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
static int ea_add(int d, unsigned dc, const char *n83, const char *name)
{
    unsigned long pos, at = 0xFFFFFFFFUL;
    unsigned u, len, need, k, use;
    int rc;

    ea_del(d, dc, (const unsigned char *)n83);
    if (ea_open(d, 1))
        return IFS_FULL;
    for (len = 0; name[len] && len < 255; len++)
        ;
    need = (EA_HDR + len + 31) >> 5;
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
    for (k = 0; k < 11; k++)
        ea_rec[6 + k] = (unsigned char)n83[k];
    ea_rec[17] = (unsigned char)len;
    for (k = 0; k < len; k++)
        ea_rec[EA_HDR + k] = (unsigned char)name[k];
    /* am Ende ganze Einheiten schreiben, damit der naechste Satz passt */
    rc = fat_write(EAF, at, KDATA, (unsigned)ea_rec,
                   at == ea_f.size ? need << 5 : EA_HDR + len);
    k = fat_close(EAF);
    if (ea_cdrv == d && ea_cdc == dc)
        ea_cdrv = -1;
    return rc < 0 ? rc : (int)k;
}

static int fat_remove(int d, const char *path)
{
    struct found f;
    unsigned char __far *e;

    ferr = 0;
    if (revalidate(d) || walk(d, path, &f) || f.dc == 0xFFFF)
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
        static unsigned char on[11];
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

static int fat_mkdir(int d, const char *path)
{
    struct found par, f;
    char n[11];
    unsigned char __far *e;
    unsigned c, idx, k;
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
    put16(e, 26, c);
    put16(e, 32 + 26, par.first);
    wr(d, clus_lba(&vol[d], c));
    return done_op(0);
}

static int fat_rmdir(int d, const char *path)
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
        static unsigned char on[11];
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

/* umbenennen oder verschieben (gleiches Laufwerk) */
static int fat_move(int d, const char *from, const char *to)
{
    static unsigned char ent[32];
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
            put16(e, 26, par.first);
            wr(d, de_lba);
        }
    }
    return done_op(0);
}

/* freier Platz in Bytes */
/* freie Cluster: einmal zaehlen, danach fuehrt fat_set() den Zaehler mit.
   FAT16 sektorweise direkt im Cache - 65536 Einzelaufrufe kosten auf dem
   286 spuerbar Zeit. */
static int fat_space(int d, unsigned long *bytes)
{
    struct volume *v = &vol[d];
    unsigned c, n = 0, end = v->clusters + 2, k, last;

    if (revalidate(d))
        return IFS_ERR;
    if (!v->nfree_ok) {
        if (v->fat16) {
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
static int fat_setinfo(int d, const char *path, unsigned mask, unsigned attr,
                       unsigned date, unsigned time)
{
    struct found f;
    unsigned char __far *e;

    ferr = 0;
    if (revalidate(d) || walk(d, path, &f) || f.dc == 0xFFFF)
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
static int fat_label(int d, char *lbl, unsigned long *serial, const char *nl)
{
    unsigned char __far *e;
    unsigned i, k, found = 0xFFFF;
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
    if (blk_read(vol[d].blk, vol[d].start) == 0 && dma()[38] == 0x29)
        *serial = rd32(39);
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
    if (!ferr && blk_read(vol[d].blk, vol[d].start) == 0 && dma()[38] == 0x29) {
        for (k = 0; k < 11; k++)
            dma()[43 + k] = n[0] == ' ' ? "NO NAME    "[k] : n[k];
        wr(d, vol[d].start);
    }
    return done_op(0);
}

/* Pfad -> 8.3-Namen (shortn) oder Namen in Originalschreibung */
static int fat_namepath(int d, const char *path, char *out, int shortn)
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
            if (e && ea_find(e, ea_tmp))
                for (k = 0; ea_tmp[k] && k < IFS_NAMELEN - 1; k++)
                    s[k] = ea_tmp[k], s[k + 1] = 0;
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
            if (x && lfn_valid(x))
                for (k = 0; lfn[k] && k < IFS_NAMELEN - 1; k++)
                    s[k] = lfn[k], s[k + 1] = 0;
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
    fat_move, fat_space, fat_setinfo, fat_label, fat_namepath
};

/* ---- IFS-Verteiler ----------------------------------------------------- */
int ifs_mount(int drive, int blk, unsigned long start, struct ifs_driver *drv)
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

/* Diskette A: beim Start von Festplatte erst beim ersten Zugriff */
int floppy_lazy;

/* Laufwerk vorhanden (auch unformatiert: FORMAT, DosOpenDrive) */
int vol_present(int d)
{
    if ((unsigned)d >= NDRIVE)
        return 0;
    if (d < 2 && !vol[d].used && (floppy_lazy & (1 << d))) {
        blk_media(vol[d].blk);          /* Format erkennen            */
        ifs_mount(d, vol[d].blk, 0, &fat_driver);
    }
    return vol[d].used || vol[d].present;
}

/* Dateisystem eingehaengt; gesperrte Laufwerke (CHKDSK /F, FORMAT) sind zu */
static int vol_ok(int d)
{
    return vol_present(d) && vol[d].used && !vol[d].locked;
}
#define VOL_OK(d) vol_ok(d)

int ifs_open(int drive, const char *path, struct ifs_file __far *f)
{
    return VOL_OK(drive) ? vol[drive].drv->open(drive, path, f) : -1;
}

int ifs_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
             unsigned off, unsigned len)
{
    return VOL_OK(f->drive) ? vol[f->drive].drv->read(f, pos, sel, off, len) : -1;
}

int ifs_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *d)
{
    return VOL_OK(drive) ? vol[drive].drv->readdir(drive, dir, n, d) : -1;
}

int ifs_namepath(int drive, const char *path, char *out, int shortn)
{
    if (VOL_OK(drive) && vol[drive].drv->namepath)
        return vol[drive].drv->namepath(drive, path, out, shortn);
    return -1;
}

int ifs_isdir(int drive, const char *path)
{
    return VOL_OK(drive) ? vol[drive].drv->isdir(drive, path) : 0;
}

#define HAS(d, fn) (VOL_OK(d) && vol[d].drv->fn)

/* vor jedem Schreiben: Laufwerk als "in Benutzung" markieren */
static void mark_in_use(int d)
{
    struct volume *v = &vol[d];
    unsigned val;

    if (v->in_use || v->drv != &fat_driver || !v->fat16 || IS_FLOPPY(v->blk) ||
        v->locked)
        return;
    v->in_use = 1;
    val = fat_raw(d, 1);
    if (val & CLEAN_BIT) {
        fat_set(d, 1, val & ~CLEAN_BIT);
        fat_flush();
    }
}

void ifs_shutdown(void)
{
    int d;
    unsigned val;

    for (d = 0; d < NDRIVE; d++)
        if (vol[d].used && vol[d].in_use && !vol[d].locked) {
            val = fat_raw(d, 1);
            fat_set(d, 1, val | CLEAN_BIT);
            fat_flush();
            vol[d].in_use = 0;
        }
}

#define MARK(d) do { if (VOL_OK(d)) mark_in_use(d); } while (0)

int ifs_create(int drive, const char *path, struct ifs_file __far *f)
{
    MARK(drive);
    return HAS(drive, create) ? vol[drive].drv->create(drive, path, f) : IFS_WP;
}

int ifs_write(struct ifs_file __far *f, unsigned long pos, unsigned sel,
              unsigned off, unsigned len)
{
    MARK(f->drive);
    return HAS(f->drive, write) ? vol[f->drive].drv->write(f, pos, sel, off, len) : IFS_WP;
}

int ifs_close(struct ifs_file __far *f)
{
    return HAS(f->drive, close) ? vol[f->drive].drv->close(f) : 0;
}

int ifs_remove(int drive, const char *path)
{
    MARK(drive);
    return HAS(drive, remove) ? vol[drive].drv->remove(drive, path) : IFS_WP;
}

int ifs_mkdir(int drive, const char *path)
{
    MARK(drive);
    return HAS(drive, mkdir) ? vol[drive].drv->mkdir(drive, path) : IFS_WP;
}

int ifs_rmdir(int drive, const char *path)
{
    MARK(drive);
    return HAS(drive, rmdir) ? vol[drive].drv->rmdir(drive, path) : IFS_WP;
}

int ifs_move(int drive, const char *from, const char *to)
{
    MARK(drive);
    return HAS(drive, move) ? vol[drive].drv->move(drive, from, to) : IFS_WP;
}

int ifs_setinfo(int d, const char *path, unsigned mask, unsigned attr,
                unsigned date, unsigned time)
{
    MARK(d);
    return HAS(d, setinfo) ? vol[d].drv->setinfo(d, path, mask, attr, date, time) : IFS_WP;
}

int ifs_label(int d, char *lbl, unsigned long *serial, const char *newlbl)
{
    if (newlbl)
        MARK(d);
    if (!HAS(d, label)) {
        lbl[0] = 0;
        *serial = 0;
        return newlbl ? IFS_WP : 0;
    }
    return vol[d].drv->label(d, lbl, serial, newlbl);
}

int ifs_space(int drive, unsigned long *free_bytes)
{
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
void vol_move(int from, int to)
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
void vol_reset(int d)
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
