/*
 * ISO9660.IFS - nachladbares Dateisystem fuer CD-ROMs (ISO 9660), nur lesend
 *
 * CONFIG.SYS:  IFS=ISO9660.IFS
 *
 * Ein IFS-Modul ist aufgebaut wie ein Geraetetreiber (dcrt0.asm, Ring 0).
 * Der Kernel ruft es mit struct ifs_req (include/ifsmod.h) auf; Bloecke
 * (2048 Byte) liest es ueber DevHlp DH_BLKREAD in den DMA-Puffer des
 * Kernels. Vor jedem Oeffnen und jeder Verzeichnisliste prueft der Kernel
 * den Datentraeger neu (IFSM_MOUNT) - CDs werden gewechselt.
 *
 * Unterstuetzt: Primary Volume Descriptor, Verzeichnisse beliebiger Tiefe,
 * Namen ohne Versionsnummer (";1") und ohne Schlusspunkt. Kein Joliet und
 * kein Rock Ridge - die kurzen ISO-Namen sind in der 8.3-Welt ohnehin die
 * richtigen. Dateien in mehreren Extents (sehr selten) werden nicht gelesen.
 *
 * Gebaut mit -zu wie die Treiber: SS ist der Kernelstack, also keine
 * Adressen lokaler Variablen weitergeben.
 */
#include "../drivers/drvlib.h"
#include "../include/ifsmod.h"

#define IREQ ((struct ifs_req __far *)MK_FP(req_sel, req_off))

/* n Bytes von src nach dst (beide far) */
void far_cpy(void __far *dst, const void __far *src, unsigned n);
#pragma aux far_cpy = "push ds" "mov ds,dx" "rep movsb" "pop ds" \
    __parm [__es __di] [__dx __si] [__cx] __modify [__di __si __cx];

static unsigned      dmasel;            /* DMA-Puffer des Kernels (2048 Byte) */
static unsigned long root_ext, root_len;

/* gefundener Verzeichniseintrag */
static struct {
    char          name[IFS_NAMELEN];
    unsigned long ext, size;
    unsigned char flags;                /* 02h Verzeichnis, 01h versteckt  */
    unsigned      date, time;           /* FAT-Format                      */
} ent;

static unsigned char __far *blk(void)
{
    return MK_FP(dmasel, 0);
}

static int rd(unsigned dev, unsigned long lba)
{
    return devhlp(DH_BLKREAD, (unsigned)(lba >> 16), dev, (unsigned)lba) ? -1 : 0;
}

static unsigned long le32(unsigned char __far *p)
{
    return (p[0] | ((unsigned)p[1] << 8)) |
           ((unsigned long)(p[2] | ((unsigned)p[3] << 8)) << 16);
}

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Primary Volume Descriptor suchen; 0 = ok */
static int mount(unsigned dev)
{
    unsigned long lba;
    unsigned char __far *b;

    for (lba = 16; lba < 32; lba++) {
        if (rd(dev, lba))
            return -1;
        b = blk();
        if (b[1] != 'C' || b[2] != 'D' || b[3] != '0' || b[4] != '0' || b[5] != '1')
            return -1;
        if (b[0] == 1) {
            root_ext = le32(b + 156 + 2);
            root_len = le32(b + 156 + 10);
            return 0;
        }
        if (b[0] == 255)
            break;
    }
    return -1;
}

/* Eintrag in ent uebernehmen */
static void take(unsigned char __far *r)
{
    unsigned nl = r[32], k, y;

    for (k = 0; k < nl && k < IFS_NAMELEN - 1 && r[33 + k] != ';'; k++)
        ent.name[k] = up((char)r[33 + k]);
    while (k > 0 && ent.name[k - 1] == '.')
        k--;                            /* "README." -> "README"        */
    ent.name[k] = 0;
    ent.ext = le32(r + 2);
    ent.size = le32(r + 10);
    ent.flags = r[25];
    y = r[18] < 80 ? 0 : r[18] - 80;    /* Jahre seit 1900 -> seit 1980  */
    ent.date = (y << 9) | ((r[19] & 15) << 5) | (r[20] & 31);
    ent.time = ((r[21] & 31) << 11) | ((r[22] & 63) << 5) | ((r[23] & 63) >> 1);
}

/* Verzeichnis (ext, len) durchgehen: name != 0 sucht diesen Namen,
   sonst den Eintrag Nummer n. 0 = gefunden (in ent) */
static int scan(unsigned dev, unsigned long ext, unsigned long len,
                const char __far *name, unsigned nlen, unsigned n)
{
    unsigned long i, nblk = (len + 2047) >> 11;
    unsigned char __far *b;
    unsigned off, rl, k, count = 0;

    for (i = 0; i < nblk; i++) {
        if (rd(dev, ext + i))
            return -1;
        b = blk();
        for (off = 0; off < 2048; off += rl) {
            rl = b[off];
            if (rl == 0 || off + rl > 2048)
                break;                  /* Rest des Blocks ist Fuellung  */
            if (rl < 34 || b[off + 32] == 0)
                continue;
            if (b[off + 32] == 1 && b[off + 33] <= 1)
                continue;               /* "." und ".."                  */
            if (b[off + 25] & 0x04)
                continue;               /* zugeordnete Datei             */
            if (name) {
                take(b + off);
                for (k = 0; k < nlen && ent.name[k] == up(name[k]); k++)
                    ;
                if (k == nlen && ent.name[k] == 0)
                    return 0;
            } else if (count++ == n) {
                take(b + off);
                return 0;
            }
        }
    }
    return -1;
}

/* kanonischen Pfad ablaufen; Stamm: ent.flags = 2 */
static int walk(unsigned dev, const char __far *path)
{
    const char __far *p = path, __far *q;

    ent.name[0] = 0;
    ent.ext = root_ext;
    ent.size = root_len;
    ent.flags = 2;
    ent.date = ent.time = 0;
    while (*p == '\\')
        p++;
    while (*p) {
        for (q = p; *q && *q != '\\'; q++)
            ;
        if (!(ent.flags & 2))
            return -1;
        if (scan(dev, ent.ext, ent.size, p, (unsigned)(q - p), 0))
            return -1;
        p = q;
        while (*p == '\\')
            p++;
    }
    return 0;
}

static void do_read(struct ifs_req __far *r)
{
    unsigned long pos = r->pos, size = r->f.size;
    unsigned len = r->len, done = 0, o, n;

    r->result = 0;
    if (pos >= size) {
        r->status = 0;
        return;
    }
    if (size - pos < len)
        len = (unsigned)(size - pos);
    while (done < len) {
        if (rd(r->dev, r->f.fsdata + (pos >> 11))) {
            r->status = done ? 0 : 1;   /* Teilergebnis zaehlt        */
            r->result = done;
            return;
        }
        o = (unsigned)pos & 2047;
        n = 2048 - o;
        if (n > len - done)
            n = len - done;
        far_cpy(MK_FP(r->sel, r->off + done), blk() + o, n);
        done += n;
        pos += n;
    }
    r->result = done;
    r->status = 0;
}

static void request(void)
{
    struct ifs_req __far *r = IREQ;
    unsigned k;

    r->status = 1;
    switch (r->cmd) {
    case IFSM_MOUNT:
        if (mount(r->dev) == 0)
            r->status = 0;
        break;
    case IFSM_OPEN:
        if (walk(r->dev, r->path) || (ent.flags & 2))
            break;
        r->f.attr = ATTR_RO | ((ent.flags & 1) ? ATTR_HIDDEN : 0);
        r->f.first = 0;
        r->f.size = ent.size;
        r->f.fsdata = ent.ext;
        r->f.cache_clus = 0;
        r->f.cache_pos = 0;
        r->status = 0;
        break;
    case IFSM_READ:
        do_read(r);
        break;
    case IFSM_READDIR:
        if (walk(r->dev, r->path) || !(ent.flags & 2) ||
            scan(r->dev, ent.ext, ent.size, 0, 0, r->n))
            break;
        for (k = 0; ent.name[k]; k++)
            r->de.name[k] = ent.name[k];
        r->de.name[k] = 0;
        r->de.attr = (unsigned char)(ATTR_RO | ((ent.flags & 2) ? ATTR_DIR : 0) |
                                     ((ent.flags & 1) ? ATTR_HIDDEN : 0));
        r->de.size = (ent.flags & 2) ? 0 : ent.size;
        r->de.date = ent.date;
        r->de.time = ent.time;
        r->status = 0;
        break;
    case IFSM_ISDIR:
        if (walk(r->dev, r->path) == 0 && (ent.flags & 2))
            r->status = 0;
        break;
    }
}

void strategy(void)
{
    struct drv_req __far *r = REQ;

    if (r->cmd == DRV_INIT) {
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        dmasel = r->buf_sel;
        r->units = IFS_CLASS_CD;
        r->status = DRV_OK;
        dputs("  ISO 9660 f\x81r CD-ROMs, nur lesend\n");
        return;
    }
    request();
}
