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
    return 0;
}

/* ---- Verzeichnisse ----------------------------------------------------- */
/* zuletzt benutzter Cluster beim fortlaufenden Lesen eines Verzeichnisses */
static int lc_drive = -1;
static unsigned lc_dir, lc_n, lc_clus;
static unsigned long de_lba;            /* Sektor des letzten dir_entry()  */

static void dir_cache_reset(void)
{
    lc_drive = -1;
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
        if (to83(p, (unsigned)(q - p), n) || lookup(d, f->first, n, f))
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
    unsigned i, k, j, n = 0;

    if ((want == 0 && revalidate(d)) || walk(d, dir, &f) || !(f.attr & ATTR_DIR))
        return -1;
    for (i = 0; (e = dir_entry(d, f.first, i)) != 0; i++) {
        switch (kind(e)) {
        case 0: return -1;
        case 2: continue;
        }
        if (n++ != want)
            continue;
        j = 0;
        for (k = 0; k < 8 && e[k] != ' '; k++)
            de->name[j++] = e[k];
        if (e[8] != ' ') {
            de->name[j++] = '.';
            for (k = 8; k < 11 && e[k] != ' '; k++)
                de->name[j++] = e[k];
        }
        de->name[j] = 0;
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
    if (walk(d, pbuf, par) || !(par->attr & ATTR_DIR))
        return IFS_ERR;
    if (to83(last, (unsigned)(p - last), n83) || n83[0] == '.')
        return IFS_ERR;
    return 0;
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
    stamp(e);
    put16(e, 26, first);
    wr(d, de_lba);
    return 0;
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
    if (lookup(d, par.first, n, &f) == 0) {
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
        rc = new_entry(d, par.first, n, 0x20, 0, &idx);
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
    e = dir_entry(d, f.dc, f.idx);
    if (!e)
        return IFS_ERR;
    e[0] = 0xE5;
    wr(d, de_lba);
    if (!ferr)
        free_chain(d, f.first);
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
    if (lookup(d, par.first, n, &f) == 0)
        return IFS_EXIST;
    c = alloc_cluster(d, 0);
    if (!c)
        return done_op(IFS_FULL);
    zero_cluster(d, c);
    rc = new_entry(d, par.first, n, ATTR_DIR, c, &idx);
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
    e = dir_entry(d, f.dc, f.idx);
    if (!e)
        return IFS_ERR;
    e[0] = 0xE5;
    wr(d, de_lba);
    if (!ferr)
        free_chain(d, f.first);
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
    if (lookup(d, par.first, n, &g) == 0)
        return IFS_EXIST;
    if (walk(d, from, &f))              /* lookup hat DMABUF benutzt    */
        return IFS_ERR;
    if (par.first == f.dc) {            /* gleiches Verzeichnis         */
        e = dir_entry(d, f.dc, f.idx);
        if (!e)
            return IFS_ERR;
        for (k = 0; k < 11; k++)
            e[k] = (unsigned char)n[k];
        wr(d, de_lba);
        return done_op(0);
    }
    e = dir_entry(d, f.dc, f.idx);
    if (!e)
        return IFS_ERR;
    far_copy(KDATA, (unsigned)ent, DMABUF, FP_OFF(e), 32);
    rc = dir_slot(d, par.first, &idx);
    if (rc)
        return done_op(rc);
    e = dir_entry(d, par.first, idx);
    if (!e)
        return done_op(IFS_ERR);
    far_copy(DMABUF, FP_OFF(e), KDATA, (unsigned)ent, 32);
    for (k = 0; k < 11; k++)
        e[k] = (unsigned char)n[k];
    wr(d, de_lba);
    e = dir_entry(d, f.dc, f.idx);
    if (!e)
        return done_op(IFS_ERR);
    e[0] = 0xE5;
    wr(d, de_lba);
    if ((f.attr & ATTR_DIR) && !ferr) { /* ".." zeigt auf das neue Elternverzeichnis */
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

struct ifs_driver fat_driver = {
    "FAT", fat_mount, fat_open, fat_read, fat_readdir, fat_isdir,
    fat_create, fat_write, fat_close, fat_remove, fat_mkdir, fat_rmdir,
    fat_move, fat_space, fat_setinfo, fat_label
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
