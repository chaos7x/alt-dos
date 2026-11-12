/* SPDX-License-Identifier: GPL-2.0-or-later */
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
 * Namen ohne Versionsnummer (";1") und ohne Schlusspunkt; Joliet (Windows
 * 95: Supplementary Volume Descriptor mit UCS-2-Namen bis 64 Zeichen) -
 * gibt es ihn, gilt sein Verzeichnisbaum: lange Namen in Originalschreibung
 * (UCS-2 -> CP437, Unbekanntes als '_'), dazu je Eintrag ein 8.3-Ersatzname
 * fuer Programme ohne FX_FLAG_LONGNAMES (NAME~n.EXT, n = Nummer des
 * Eintrags im Verzeichnis, also eindeutig und bei jedem Lesen gleich).
 * Pfade duerfen beide Namen mischen. Ohne Joliet: ISO 9660:1999 (Enhanced
 * Volume Descriptor, Version 2, Namen bis 207 Byte) oder Rock Ridge (Unix-
 * CDs: Namen in den NM-Eintraegen des System Use Area, auch ueber Fort-
 * setzungsbloecke CE, verlegte tiefe Verzeichnisse ueber CL/RE). Solche
 * Namen sind Bytes - gueltiges UTF-8 wird als UTF-8 gelesen, sonst
 * Latin-1, beides nach CP437. Vorrang: Joliet, 9660:1999, Rock Ridge,
 * nur ISO. Rechte, Zeiten und Links aus Rock Ridge werden nicht benutzt.
 * Dateien in mehreren Extents (sehr selten) werden nicht gelesen.
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
static unsigned char joliet;            /* Baum aus dem Joliet-Deskriptor  */
static unsigned char lng;               /* lange Namen (Joliet, 1999, RR)  */
static unsigned char bytes;             /* Namen als Bytes (1999, RR)      */
static unsigned char rr;                /* Rock Ridge (SUSP im PVD-Baum)   */
static unsigned char sp_skip;           /* SUSP: Bytes vor dem System Use  */
static char          volid[12];         /* Bezeichnung (aus dem PVD)       */

/* gefundener Verzeichniseintrag */
static struct {
    char          name[IFS_NAMELEN];
    char          alias[13];            /* 8.3-Ersatzname, "" = name ist 8.3 */
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
    unsigned char u = (unsigned char)c;
    if (u >= 'a' && u <= 'z')
        return (char)(u - 32);
    if (u == 0x84) return (char)0x8E;   /* CP437: ae oe ue              */
    if (u == 0x94) return (char)0x99;
    if (u == 0x81) return (char)0x9A;
    return c;
}

/* CP437 80h-FFh -> Unicode (ganze Codepage) */
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

/* Tabelle der aktiven Codepage (CHCP), beim Einhaengen geholt; 0 = 437 */
static unsigned short __far *ctab;

static unsigned char uni2cp(unsigned u)
{
    unsigned i;
    if (u < 0x80)
        return (unsigned char)u;
    for (i = 0; i < 128; i++)
        if ((ctab ? ctab[i] : cp_uni[i]) == u)
            return (unsigned char)(0x80 + i);
    return 0;
}

/* Joliet-Name des letzten take() als UCS-2; ent_lossy: hat '_' fuer
   Zeichen ausserhalb CP437 -> dem Kernel melden (DH_UNINOTE), damit eine
   Kopie auf FAT das Original bekommt (Unicode Stufe 1) */
static unsigned short uname[IFS_NAMELEN];
static unsigned char ent_lossy;

static int ok83(unsigned char c)        /* in 8.3-Namen erlaubt         */
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80 ||
           c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
           c == '@' || c == '~' || c == '!' || c == '(' || c == ')' ||
           c == '{' || c == '}' || c == '^' || c == '#' || c == '&';
}

/* Grundform fuer den 8.3-Ersatznamen (wie Windows 95): bis 6 erlaubte
   Zeichen vor dem letzten Punkt, "." und bis 3 danach. Ist der Name schon
   8.3, steht er gross in b und das Ergebnis ist 1. */
static int basis(const char *n, char *b)
{
    unsigned len, dot = 0xFFFF, i, k = 0, e = 0, valid = 1;

    for (len = 0; n[len]; len++)
        if (n[len] == '.')
            dot = len;
    for (i = 0; i < len; i++)
        if (n[i] == '.' ? i != dot : !ok83((unsigned char)up(n[i])))
            valid = 0;
    if (dot == 0xFFFF ? len > 8 : (dot > 8 || len - dot - 1 > 3 || dot == 0))
        valid = 0;
    if (valid) {
        for (i = 0; i < len; i++)
            b[i] = up(n[i]);
        b[len] = 0;
        return 1;
    }
    for (i = 0; i < len && i != dot && k < 6; i++)
        if (ok83((unsigned char)up(n[i])))
            b[k++] = up(n[i]);
    if (dot != 0xFFFF) {
        b[k++] = '.';
        for (i = dot + 1; i < len && e < 3; i++)
            if (ok83((unsigned char)up(n[i])))
                b[k++] = up(n[i]), e++;
        if (!e)
            k--;
    }
    b[k] = 0;
    return 0;
}

/* Ersatzname aus Grundform und Zaehler: STAMM~k.EXT (8 Zeichen vorn) */
static void alias_of(const char *b, unsigned cnt, char *a)
{
    static char sfx[7];
    unsigned i = 6, s, k, o = 0;

    sfx[6] = 0;
    do {
        sfx[--i] = (char)('0' + cnt % 10);
        cnt /= 10;
    } while (cnt && i > 1);
    sfx[--i] = '~';
    s = 6 - i;
    for (k = 0; b[k] && b[k] != '.' && o < 8 - s; k++)
        a[o++] = b[k];
    while (b[k] && b[k] != '.')
        k++;
    for (; sfx[i]; i++)
        a[o++] = sfx[i];
    while (b[k])
        a[o++] = b[k++];
    a[o] = 0;
}

static int strcmp_n(const char *a, const char *b)  /* 0 = gleich */
{
    while (*a && *a == *b)
        a++, b++;
    return *a != *b;
}

static const char *ext_of(const char *s)
{
    const char *e = "";
    for (; *s; s++)
        if (*s == '.')
            e = s;
    return e;
}

/* Primary Volume Descriptor suchen; 0 = ok */
static int mount(unsigned dev)
{
    unsigned long lba;
    unsigned char __far *b;
    int found = 0;
    unsigned k;

    unsigned long ev_ext = 0, ev_len = 0;
    unsigned o;

    joliet = lng = bytes = rr = sp_skip = 0;
    k = devhlp(DH_CPTAB, 0, 0, 0);      /* Codepage kann sich aendern  */
    ctab = k ? MK_FP(k, devhlp(DH_CPTAB, 0, 1, 0)) : 0;
    for (lba = 16; lba < 48; lba++) {
        if (rd(dev, lba))
            return -1;
        b = blk();
        if (b[1] != 'C' || b[2] != 'D' || b[3] != '0' || b[4] != '0' || b[5] != '1')
            break;
        if (b[0] == 1 && !found) {      /* Primary                       */
            if (!joliet) {
                root_ext = le32(b + 156 + 2);
                root_len = le32(b + 156 + 10);
            }
            for (k = 0; k < 11 && b[40 + k] >= ' '; k++)
                volid[k] = (char)b[40 + k];
            while (k > 0 && volid[k - 1] == ' ')
                k--;
            volid[k] = 0;
            found = 1;
        } else if (b[0] == 2 && b[88] == '%' && b[89] == '/' &&
                   (b[90] == '@' || b[90] == 'C' || b[90] == 'E')) {
            root_ext = le32(b + 156 + 2);       /* Joliet: UCS-2-Baum    */
            root_len = le32(b + 156 + 10);
            joliet = 1;
        } else if (b[0] == 2 && b[6] == 2) {    /* ISO 9660:1999 (EVD)   */
            ev_ext = le32(b + 156 + 2);
            ev_len = le32(b + 156 + 10);
        } else if (b[0] == 255)
            break;
    }
    if (!found)
        return -1;
    if (joliet) {
        lng = 1;
        return 0;
    }
    if (ev_ext) {
        root_ext = ev_ext;
        root_len = ev_len;
        lng = bytes = 1;
        return 0;
    }
    /* Rock Ridge: "." des Stamms beginnt sein System Use mit SP BE EF */
    if (rd(dev, root_ext))
        return -1;
    b = blk();
    o = 34;                             /* 33 + Namenslaenge 1 (ungerade) */
    if (b[0] >= o + 7 && b[o] == 'S' && b[o + 1] == 'P' &&
        b[o + 4] == 0xBE && b[o + 5] == 0xEF) {
        sp_skip = b[o + 6];
        rr = lng = bytes = 1;
    }
    return 0;
}

/* Name aus Bytes (ISO 9660:1999, Rock Ridge): gueltiges UTF-8 als UTF-8,
   sonst Latin-1; nach CP437 wie bei Joliet */
static unsigned char raw[256];
static unsigned rlen;

static unsigned name_bytes(void)
{
    unsigned i = 0, k = 0, c, b;
    ent_lossy = 0;
    while (i < rlen && k < IFS_NAMELEN - 1) {
        c = raw[i];
        if (c >= 0xC2 && c <= 0xDF && i + 1 < rlen && (raw[i + 1] & 0xC0) == 0x80) {
            c = ((c & 0x1F) << 6) | (raw[i + 1] & 0x3F);
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF && i + 2 < rlen &&
                   (raw[i + 1] & 0xC0) == 0x80 && (raw[i + 2] & 0xC0) == 0x80) {
            c = ((c & 0x0F) << 12) | ((raw[i + 1] & 0x3F) << 6) | (raw[i + 2] & 0x3F);
            i += 3;
            if (c < 0x800)
                c = '_';                /* ueberlange Form              */
        } else if (c >= 0xF0 && c <= 0xF4 && i + 3 < rlen &&
                   (raw[i + 1] & 0xC0) == 0x80) {
            c = 0xFFFD;                 /* ausserhalb UCS-2              */
            i += 4;
        } else
            i++;                        /* ASCII oder Latin-1            */
        if (c == ';' && !rr)
            break;                      /* 1999: Versionsnummer          */
        uname[k] = (unsigned short)(c == 0xFFFD ? '_' : c);
        b = c < ' ' ? 0 : uni2cp(c);
        if (!b)
            ent_lossy = 1;
        ent.name[k++] = b ? (char)b : '_';
    }
    return k;
}

/* Rock Ridge: Zustand des laufenden Eintrags */
static unsigned char rr_nm, rr_re;
static unsigned long ce_blk, cl_loc;
static unsigned ce_off, ce_len;

/* SUSP-Eintraege eines System Use Area auswerten */
static void su_parse(unsigned char __far *p, unsigned n)
{
    unsigned l, k;
    while (n >= 4) {
        l = p[2];
        if (l < 4 || l > n)
            break;
        if (p[0] == 'N' && p[1] == 'M' && l >= 5 && !(p[4] & 6)) {
            for (k = 5; k < l && rlen < sizeof(raw); k++)
                raw[rlen++] = p[k];
            rr_nm = 1;
        } else if (p[0] == 'C' && p[1] == 'E' && l >= 28) {
            ce_blk = le32(p + 4);
            ce_off = (unsigned)le32(p + 12);
            ce_len = (unsigned)le32(p + 20);
        } else if (p[0] == 'R' && p[1] == 'E')
            rr_re = 1;
        else if (p[0] == 'C' && p[1] == 'L' && l >= 12)
            cl_loc = le32(p + 4);
        else if (p[0] == 'S' && p[1] == 'T')
            break;
        p += l;
        n -= l;
    }
}

/* Eintrag in ent uebernehmen */
static void take(unsigned char __far *r)
{
    unsigned nl = r[32], k, y, i, c;

    k = 0;
    if (bytes && !rr) {                 /* ISO 9660:1999                 */
        for (rlen = 0; rlen < nl && rlen < sizeof(raw); rlen++)
            raw[rlen] = r[33 + rlen];
        k = name_bytes();
    } else if (joliet) {                       /* UCS-2, gross zuerst           */
        ent_lossy = 0;
        for (i = 0; i + 1 < nl && k < IFS_NAMELEN - 1; i += 2) {
            unsigned b;
            c = (r[33 + i] << 8) | r[34 + i];
            if (c == ';')
                break;
            uname[k] = (unsigned short)c;
            b = c < ' ' ? 0 : uni2cp(c);
            if (!b)
                ent_lossy = 1;
            ent.name[k++] = b ? (char)b : '_';
        }
    } else
        for (k = 0; k < nl && k < IFS_NAMELEN - 1 && r[33 + k] != ';'; k++)
            ent.name[k] = up((char)r[33 + k]);
    while (k > 0 && ent.name[k - 1] == '.')
        k--;                            /* "README." -> "README"        */
    ent.name[k] = 0;
    ent.alias[0] = 0;
    ent.ext = le32(r + 2);
    ent.size = le32(r + 10);
    ent.flags = r[25];
    y = r[18] < 80 ? 0 : r[18] - 80;    /* Jahre seit 1900 -> seit 1980  */
    ent.date = (y << 9) | ((r[19] & 15) << 5) | (r[20] & 31);
    ent.time = ((r[21] & 31) << 11) | ((r[22] & 63) << 5) | ((r[23] & 63) >> 1);
    if (rr) {                           /* System Use hinter dem Namen   */
        i = 33 + nl + !(nl & 1) + sp_skip;
        rlen = 0;
        rr_nm = rr_re = 0;
        ce_len = 0;
        cl_loc = 0;
        if (i < r[0])
            su_parse(r + i, r[0] - i);
    }
}

/* Eintrag bei off im Block lba nach ent; bei Rock Ridge Fortsetzungen
   (CE) und verlegte Verzeichnisse (CL) nachlesen, danach den Verzeich-
   nisblock wieder in den Puffer. 0 = ok, 1 = auslassen (RE), -1 = Fehler */
static int getent(unsigned dev, unsigned long lba, unsigned off)
{
    unsigned n, reread = 0, co, cn;
    take(blk() + off);
    if (!rr)
        return 0;
    for (n = 0; ce_len && n < 4; n++) {
        co = ce_off;
        cn = ce_len;
        ce_len = 0;
        if (co >= 2048 || cn > 2048 - co)
            break;
        reread = 1;
        if (rd(dev, ce_blk))
            break;
        su_parse(blk() + co, cn);
    }
    if (cl_loc) {                       /* tiefes Verzeichnis, verlegt   */
        reread = 1;
        if (rd(dev, cl_loc) == 0) {
            ent.ext = cl_loc;
            ent.size = le32(blk() + 10);    /* aus seinem "."-Eintrag    */
            ent.flags |= 2;
        }
    }
    if (reread && rd(dev, lba))
        return -1;
    if (rr_nm) {
        n = name_bytes();
        ent.name[n] = 0;
    } else
        ent_lossy = 0;
    return rr_re;
}

/* gefundener Eintrag geht an den Kernel: Original des Joliet-Namens melden */
static void uni_tell(void)
{
    unsigned n;
    if (!lng || !ent_lossy)
        return;
    for (n = 0; ent.name[n]; n++)
        ;
    devhlp(DH_UNINOTE, n, (unsigned)ent.name, (unsigned)uname);
}

/* Verzeichnis (ext, len) durchgehen:
   SC_NAME  sucht den Namen (lang, ohne Gross/klein, oder 8.3-Ersatzname),
   SC_INDEX den Eintrag Nummer n,
   SC_COUNT zaehlt die Eintraege vor Nummer n mit Grundform want (-> Anzahl).
   0 = gefunden (in ent, Nummer in ent_idx) */
#define SC_NAME  0
#define SC_INDEX 1
#define SC_COUNT 2
static unsigned ent_idx;
static char want[13], cand[13], eb[13], nm[IFS_NAMELEN];

static int scan(unsigned dev, unsigned long ext, unsigned long len,
                const char __far *name, unsigned nlen, unsigned n, int mode)
{
    unsigned long i, nblk = (len + 2047) >> 11;
    unsigned char __far *b;
    unsigned off, rl, k, count = 0, t = 0xFFFF, kw = 0, hits = 0;

    if (mode == SC_NAME) {              /* "STAMM~k.EXT"?                */
        for (k = 0; k < nlen && k < IFS_NAMELEN - 1; k++)
            nm[k] = up(name[k]);
        nm[k] = 0;
        for (k = 0; nm[k] && nm[k] != '.'; k++)
            if (nm[k] == '~')
                t = k;
        if (lng && t != 0xFFFF && nm[t + 1] >= '1' && nm[t + 1] <= '9') {
            for (k = t + 1; nm[k] >= '0' && nm[k] <= '9'; k++)
                kw = kw * 10 + (nm[k] - '0');
            if (nm[k] && nm[k] != '.')
                kw = 0;
        }
        cand[0] = 0;
    }
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
            if ((k = getent(dev, ext + i, off)) != 0) {
                if (k == 1)
                    continue;           /* RR: verlegt (RE)              */
                return -1;
            }
            if (rr && ext == root_ext && !strcmp_n(ent.name, "rr_moved"))
                ent.flags |= 1;         /* wie -hide-rr-moved: versteckt */
            if (mode == SC_INDEX) {
                if (count++ == n) {
                    ent_idx = n;
                    uni_tell();
                    return 0;
                }
                continue;
            }
            if (mode == SC_COUNT) {
                if (count++ >= n)
                    return hits;
                if (!basis(ent.name, eb) && !strcmp_n(eb, want))
                    hits++;
                continue;
            }
            for (k = 0; k < nlen && up(ent.name[k]) == nm[k]; k++)
                ;
            if (k == nlen && ent.name[k] == 0) {
                ent_idx = count;
                uni_tell();
                return 0;
            }
            if (kw && !basis(ent.name, eb)) {
                /* Ersatzname: Stamm (vor ~) und Endung muessen passen */
                for (k = 0; k < t && eb[k] == nm[k]; k++)
                    ;
                if (k == t && !strcmp_n(ext_of(eb), ext_of(nm)) &&
                    (!cand[0] || !strcmp_n(eb, cand))) {
                    if (!cand[0])
                        for (k = 0; (cand[k] = eb[k]) != 0; k++)
                            ;
                    if (++hits == kw) {
                        ent_idx = count;
                        uni_tell();
                        return 0;
                    }
                }
            }
            count++;
        }
    }
    return mode == SC_COUNT ? (int)hits : -1;
}

/* 8.3-Ersatzname des gefundenen Eintrags (ent, ent_idx) bilden */
static void fill_alias(unsigned dev, unsigned long ext, unsigned long len)
{
    static char sv[IFS_NAMELEN], b[13];
    unsigned k, cnt;
    unsigned long e2 = ent.ext, s2 = ent.size;
    unsigned char f2 = ent.flags;
    unsigned d2 = ent.date, t2 = ent.time;

    ent.alias[0] = 0;
    if (!lng)
        return;
    if (basis(ent.name, b)) {           /* schon 8.3: nur Schreibung     */
        for (k = 0; b[k] && b[k] == ent.name[k]; k++)
            ;
        if (b[k] || ent.name[k])
            for (k = 0; (ent.alias[k] = b[k]) != 0; k++)
                ;
        return;
    }
    for (k = 0; (sv[k] = ent.name[k]) != 0; k++)
        ;
    for (k = 0; (want[k] = b[k]) != 0; k++)
        ;
    cnt = (unsigned)scan(dev, ext, len, 0, 0, ent_idx, SC_COUNT) + 1;
    for (k = 0; (ent.name[k] = sv[k]) != 0; k++)
        ;
    ent.ext = e2; ent.size = s2; ent.flags = f2; ent.date = d2; ent.time = t2;
    alias_of(b, cnt, ent.alias);
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
        if (scan(dev, ent.ext, ent.size, p, (unsigned)(q - p), 0, SC_NAME))
            return -1;
        p = q;
        while (*p == '\\')
            p++;
    }
    return 0;
}

/* Pfad Teil fuer Teil in die Schreibung der CD umsetzen (n = 1: 8.3) */
static void namepath(struct ifs_req __far *r)
{
    static char out[IFS_PATHLEN], comp[IFS_PATHLEN];
    const char __far *p = r->path, __far *q;
    const char *src;
    unsigned o = 0, k;
    unsigned long pe, pl;

    ent.ext = root_ext;
    ent.size = root_len;
    ent.flags = 2;
    while (*p == '\\')
        p++;
    while (*p) {
        for (q = p, k = 0; *q && *q != '\\'; q++)
            comp[k++] = *q;
        comp[k] = 0;
        if (!(ent.flags & 2))
            return;
        pe = ent.ext;
        pl = ent.size;
        if (scan(r->dev, pe, pl, (const char __far *)comp, k, 0, SC_NAME))
            return;
        if (r->n)
            fill_alias(r->dev, pe, pl);
        src = (r->n && ent.alias[0]) ? ent.alias : ent.name;
        if (o + 1 >= IFS_PATHLEN)
            return;
        out[o++] = '\\';
        for (k = 0; src[k] && o < IFS_PATHLEN - 1; k++)
            out[o++] = src[k];
        p = q;
        while (*p == '\\')
            p++;
    }
    out[o] = 0;
    for (k = 0; k <= o; k++)
        r->path[k] = out[k];
    r->status = 0;
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
    case IFSM_READDIR: {
        unsigned long pe, pl;
        if (walk(r->dev, r->path) || !(ent.flags & 2))
            break;
        pe = ent.ext;
        pl = ent.size;
        if (scan(r->dev, pe, pl, 0, 0, r->n, SC_INDEX))
            break;
        fill_alias(r->dev, pe, pl);
        for (k = 0; ent.name[k]; k++)
            r->de.name[k] = ent.name[k];
        r->de.name[k] = 0;
        for (k = 0; ent.alias[k]; k++)
            r->de.sname[k] = ent.alias[k];
        r->de.sname[k] = 0;
        r->de.attr = (unsigned char)(ATTR_RO | ((ent.flags & 2) ? ATTR_DIR : 0) |
                                     ((ent.flags & 1) ? ATTR_HIDDEN : 0));
        r->de.size = (ent.flags & 2) ? 0 : ent.size;
        r->de.date = ent.date;
        r->de.time = ent.time;
        r->status = 0;
        break;
    }
    case IFSM_ISDIR:
        if (walk(r->dev, r->path) == 0 && (ent.flags & 2))
            r->status = 0;
        break;
    case IFSM_LABEL:                    /* Bezeichnung -> de.name        */
        for (k = 0; volid[k]; k++)
            r->de.name[k] = volid[k];
        r->de.name[k] = 0;
        r->status = 0;
        break;
    case IFSM_NAMEPATH:                 /* path -> Namen vom Datentraeger */
        namepath(r);
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
        dputs("  ISO 9660, Joliet, ISO 9660:1999 und Rock Ridge f\x81r CD-ROMs, nur lesend\n");
        return;
    }
    request();
}
