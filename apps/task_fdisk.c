/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FDISK.EXE - Festplatten partitionieren (wie MS-DOS 5/6)
 *
 *   FDISK            Menue
 *   FDISK /STATUS    Partitionen aller Platten anzeigen
 *   FDISK /MBR       Startcode des MBR erneuern (Tabelle bleibt)
 *
 * Zugriff ueber DosOpenDisk (ganze Platte, gesperrt). Geschrieben wird
 * sofort nach jeder bestaetigten Aenderung; die Laufwerksbuchstaben
 * vergibt der Kernel beim Start, daher nach Aenderungen neu starten.
 * Aufbau wie bei MS-DOS: MBR mit bis zu vier Eintraegen, eine erweiterte
 * Partition (05h) mit einer Kette von EBRs fuer die logischen Laufwerke.
 * Erste Partition ab Spur 1, alles weitere an Zylindergrenzen. Die CHS-
 * Felder braucht nur MS-DOS/das BIOS (AT-DOS rechnet mit LBA): Geometrie
 * aus vorhandenen Eintraegen, sonst 16/63 bzw. 255/63 ueber 504 MB.
 * 32-Bit-Rechnen ohne Watcom-Hilfsroutinen (mul/div als Inline).
 */
#include "fapi.h"
#include "util.h"
#include "../build/mbrcode.h"

/* 32 Bit durch 16 Bit (x >> 16 < d) -> Rest << 16 | Quotient */
unsigned long divstep(unsigned long x, unsigned d);
#pragma aux divstep = "div bx" __parm [__dx __ax] [__bx] __value [__dx __ax];
/* 16 x 16 -> 32 Bit */
unsigned long mul16(unsigned a, unsigned b);
#pragma aux mul16 = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];

static unsigned long div32(unsigned long x, unsigned d, unsigned *rem)
{
    unsigned hi = (unsigned)(x >> 16);
    unsigned long t = divstep(((unsigned long)(hi % d) << 16) | (unsigned)x, d);
    if (rem)
        *rem = (unsigned)(t >> 16);
    return ((unsigned long)(hi / d) << 16) | (unsigned)t;
}

static unsigned long mul32(unsigned long a, unsigned b)
{
    return (mul16((unsigned)(a >> 16), b) << 16) + mul16((unsigned)a, b);
}

#define MAXLOG  16
#define MB      2048UL                  /* Sektoren je MB               */
#define FAT_MAX 4194304UL               /* 2 GB: Grenze von FAT16       */

struct part { unsigned char boot, type; unsigned long start, size; };

static struct part pt[4];               /* MBR                          */
static struct part lg[MAXLOG];          /* logische Laufwerke           */
static unsigned nlg, ext = 4;           /* ext: Eintrag der erweiterten */
static unsigned char sec[512];
static struct fapi_diskinfo di;
static unsigned disk, ndisks, hd;       /* hd: Handle                   */
static unsigned heads, spt, cylsz;
static int changed, hd_locked, hd_lock_state;
static char args[FAPI_ARGLEN], lb[16];

static int is_fat(unsigned char t)
{
    return t == 1 || t == 4 || t == 6 || t == 0x0E || t == 0x0B || t == 0x0C;
}
static int big;                         /* Unterstuetzung grosser Platten (FAT32) */
static unsigned long fmax(void) { return big ? 0xFFFFFFFFUL : FAT_MAX; }
static int is_ext(unsigned char t) { return t == 5 || t == 0x0F; }

static unsigned long get32(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* ---- Platte lesen/schreiben -------------------------------------------- */
static int rw(unsigned long lba, int write)
{
    static unsigned long pos;
    pos = lba;                          /* DRV_OPEN_LBA: in Sektoren    */
    if (DosSeek(hd, &pos) == FAPI_ERROR)
        return -1;
    return (write ? DosWrite(hd, sec, 512) : DosRead(hd, sec, 512)) == 512 ? 0 : -1;
}

/* LBA -> CHS-Feld (3 Byte); ueber 1023 Zylindern die Grenzwerte */
static void chs(unsigned char *p, unsigned long lba)
{
    unsigned s, h;
    unsigned long c = div32(div32(lba, spt, &s), heads, &h);
    if (c > 1023) {
        c = 1023;
        h = heads - 1;
        s = spt - 1;
    }
    p[0] = (unsigned char)h;
    p[1] = (unsigned char)((s + 1) | ((c >> 2) & 0xC0));
    p[2] = (unsigned char)c;
}

static void set_entry(unsigned char *e, unsigned char boot, unsigned char type,
                      unsigned long rel, unsigned long abs, unsigned long size)
{
    unsigned i;
    for (i = 0; i < 16; i++)
        e[i] = 0;
    if (!type)
        return;
    e[0] = boot;
    chs(e + 1, abs);
    e[4] = type;
    chs(e + 5, abs + size - 1);
    put32(e + 8, rel);
    put32(e + 12, size);
}

static int load_disk(void)
{
    unsigned i, mh = 0, ms = 0;
    unsigned long ebr, next;
    unsigned char *e;

    nlg = 0;
    ext = 4;
    if (rw(0, 0))
        return -1;
    for (i = 0; i < 4; i++) {
        e = sec + 0x1BE + 16 * i;
        if (sec[510] != 0x55 || sec[511] != 0xAA) {
            pt[i].type = 0;
            continue;
        }
        pt[i].boot = e[0];
        pt[i].type = e[4];
        pt[i].start = get32(e + 8);
        pt[i].size = get32(e + 12);
        if (!pt[i].type)
            continue;
        if (e[5] > mh) mh = e[5];       /* Geometrie aus den Endwerten  */
        if ((e[6] & 0x3F) > ms) ms = e[6] & 0x3F;
        if (is_ext(pt[i].type) && ext == 4)
            ext = i;
    }
    if (di.heads && di.spt) {           /* Geometrie des BIOS (wie FreeDOS
                                           und MS-DOS per INT 13h/08h)  */
        heads = di.heads;
        spt = di.spt;
    } else if (ms && mh) {              /* sonst aus vorhandenen Eintraegen */
        heads = mh + 1;
        spt = ms;
    } else if (di.sectors <= 1032192UL) {   /* 1024 x 16 x 63           */
        heads = 16;
        spt = 63;
    } else {
        heads = 255;
        spt = 63;
    }
    cylsz = heads * spt;
    if (ext < 4) {                      /* Kette der logischen Laufwerke */
        ebr = pt[ext].start;
        while (nlg < MAXLOG && !rw(ebr, 0) && sec[510] == 0x55 && sec[511] == 0xAA) {
            e = sec + 0x1BE;
            if (e[4] && get32(e + 12)) {
                lg[nlg].type = e[4];
                lg[nlg].start = ebr + get32(e + 8);
                lg[nlg].size = get32(e + 12);
                nlg++;
            }
            e += 16;
            next = get32(e + 8);
            if (!is_ext(e[4]) || !next)
                break;
            ebr = pt[ext].start + next;
        }
    }
    return 0;
}

static int write_mbr(void)
{
    unsigned i;
    if (rw(0, 0))
        return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA)   /* leere Platte: Startcode */
        for (i = 0; i < 446; i++)
            sec[i] = mbr_code[i];
    for (i = 0; i < 4; i++)
        set_entry(sec + 0x1BE + 16 * i, pt[i].boot, pt[i].type, pt[i].start,
                  pt[i].start, pt[i].size);
    sec[510] = 0x55;
    sec[511] = 0xAA;
    changed = 1;
    return rw(0, 1);
}

/* EBR-Kette aus lg[] neu schreiben; EBR i liegt eine Spur vor lg[i] */
static int write_ebrs(void)
{
    unsigned i, k;
    unsigned long e, en;
    if (ext == 4)
        return 0;
    for (i = 0; i == 0 || i < nlg; i++) {
        for (k = 0; k < 512; k++)
            sec[k] = 0;
        e = i == 0 ? pt[ext].start : lg[i].start - spt;
        if (i < nlg)
            set_entry(sec + 0x1BE, 0, lg[i].type, lg[i].start - e, lg[i].start, lg[i].size);
        if (i + 1 < nlg) {
            en = lg[i + 1].start - spt;
            set_entry(sec + 0x1CE, 0, 5, en - pt[ext].start, en,
                      lg[i + 1].start + lg[i + 1].size - en);
        }
        sec[510] = 0x55;
        sec[511] = 0xAA;
        if (rw(e, 1))
            return -1;
    }
    changed = 1;
    return 0;
}

/* ---- Bildschirm (Aufbau wie das deutsche MS-DOS-6-FDISK) --------------
   Vollbild (VioSetMode), zentrierte Ueberschriften, Eingabefelder in
   eckigen Klammern ab Spalte FC, Fehlerzeile 22, Hinweis unten. */
#define FC      66                      /* Eingabefeld                  */
#define A_HI    0x0F
static unsigned rows = 25;
static char ln[81], nb[12];
static struct fapi_fsinfo fsi;

static void row_out(int r, const char *t, unsigned char a)
{
    VioWrtStrAtt(r, 0, t, a);
}

static void at(int r, int c, const char *t)
{
    VioWrtStrAtt(r, c, t, 0x07);
}

static void center(int r, const char *t, unsigned char a)
{
    VioWrtStrAtt(r, (80 - (int)slen(t)) / 2, t, a);
}

/* Zahl rechtsbuendig in w Stellen nach nb */
static char *ns(unsigned long v, int w)
{
    char t[11];
    int n = 0, i = 0;
    do {
        unsigned r;
        v = div32(v, 10, &r);
        t[n++] = (char)('0' + r);
    } while (v && n < 10);
    while (w > n && i < 10) {
        nb[i++] = ' ';
        w--;
    }
    while (n)
        nb[i++] = t[--n];
    nb[i] = 0;
    return nb;
}

/* Zeile ln aufbauen: lclr, lput(spalte, text) */
static void lclr(void)
{
    unsigned i;
    for (i = 0; i < 80; i++)
        ln[i] = ' ';
    ln[80] = 0;
}

static void lput(int c, const char *t)
{
    while (*t && c < 80)
        ln[c++] = *t++;
}

/* Text t, Zahl v und Rest r hintereinander ab Spalte c */
static void lseq(int c, const char *t, unsigned long v, const char *r)
{
    lput(c, t);
    c += (int)slen(t);
    lput(c, ns(v, 0));
    c += (int)slen(nb);
    lput(c, r);
}

static void lend(void)                  /* Leerzeichen am Ende weg      */
{
    int i = 79;
    while (i >= 0 && ln[i] == ' ')
        ln[i--] = 0;
}

static void screen(const char *title)
{
    VioCls();
    if (title)
        center(3, title, A_HI);
}

static void cur_disk(int r)
{
    lclr();
    lput(4, "Aktuelle Festplatte: ");
    lput(25, ns(disk + 1, 0));
    lend();
    at(r, 0, ln);
}

static void esc_line(const char *t)
{
    lclr();
    lput(4, "ESC");
    lput(8, t);
    lend();
    row_out((int)rows - 1, ln, 0x07);
    VioWrtStrAtt((int)rows - 1, 4, "ESC", A_HI);
}

static void error(const char *t)
{
    static char tb[81];
    scopy(tb, t, sizeof(tb));           /* t darf in ln liegen          */
    lclr();
    lput(4, tb);
    lend();
    row_out(22, ln, A_HI);
}

static void wait_esc(const char *t)
{
    esc_line(t ? t : "dr\x81" "cken, um zu den FDISK-Optionen zur\x81" "ckzukehren");
    VioSetCurPos(0xFF, 0);
    while ((KbdCharIn() & 0xFF) != 27)
        ;
}

static unsigned mb(unsigned long sectors)
{
    return (unsigned)(sectors >> 11);
}

static unsigned pct(unsigned long part, unsigned long total)
{
    while (total > 600) {               /* 16-Bit-Rechnung              */
        total >>= 1;
        part >>= 1;
    }
    return total ? (unsigned)((unsigned)part * 100 / (unsigned)total) : 0;
}

/* Eingabefeld in Zeile r: text, Punkte bis FC, [Vorgabe]. allow: erlaubte
   Zeichen ("0123456789%" oder "JN"). Erste Taste ersetzt die Vorgabe.
   -> 0 und Text in lb, -1 = Esc */
static int field(int r, const char *text, const char *def, const char *allow)
{
    static char tb[81];
    unsigned n, c, i, fc, w = slen(def) ? slen(def) : 1;
    int fresh = 1;

    for (i = 0; allow[i] && allow[i] != '%'; i++)
        ;
    if (allow[i] && w < 5)
        w = 5;                          /* Groesse in MByte oder %      */
    if (slen(allow) > 20)
        w = 11;                         /* Datentraegerbezeichnung      */
    scopy(tb, text, sizeof(tb));        /* text darf in ln liegen       */
    lclr();
    lput(4, tb);
    i = 4 + slen(tb);
    if (i && tb[slen(tb) - 1] == ':')   /* "Wahl ein:" - Feld gleich dahinter */
        fc = i + 1;
    else if (i + 1 >= FC)               /* langer Text: Feld dahinter   */
        fc = i + 1;
    else {                              /* sonst Punkte bis Spalte FC   */
        for (fc = FC; i < fc; i++)
            ln[i] = '.';
    }
    lput(fc, "[");
    lend();
    row_out(r, ln, 0x07);
    scopy(lb, def, sizeof(lb));
    n = slen(lb);
    for (;;) {
        for (i = 0; i < w; i++)
            nb[i] = i < n ? lb[i] : ' ';
        nb[w] = ']';
        nb[w + 1] = 0;
        VioWrtStrAtt(r, fc + 1, nb, A_HI);
        VioSetCurPos(r, fc + 1 + n);
        c = KbdCharIn() & 0xFF;
        if (c == 27)
            return -1;
        if (c == 13) {
            lb[n] = 0;
            return 0;
        }
        if (c == 8) {
            if (n)
                n--;
            fresh = 0;
            continue;
        }
        c = (unsigned char)up((char)c);
        if (c == 'Y')
            c = 'J';
        for (i = 0; allow[i] && (unsigned char)allow[i] != c; i++)
            ;
        if (!allow[i])
            continue;
        if (fresh) {
            n = 0;
            fresh = 0;
        }
        if (n < w && n < sizeof(lb) - 1)
            lb[n++] = (char)c;
    }
}

/* J/N-Feld -> 1 J, 0 N, -1 Esc */
static int yn(int r, const char *text, int def)
{
    for (;;) {
        if (field(r, text, def ? "J" : "N", "JN") < 0)
            return -1;
        if (lb[0] == 'J' && !lb[1]) return 1;
        if (lb[0] == 'N' && !lb[1]) return 0;
    }
}

/* Zahl aus lb; *was_pct bei "%". -1 = ungueltig */
static long numval(int *was_pct)
{
    unsigned long v = 0;
    unsigned i;
    if (was_pct)
        *was_pct = 0;
    if (!lb[0])
        return -1;
    for (i = 0; lb[i]; i++) {
        if (lb[i] == '%' && lb[i + 1] == 0 && i && was_pct) {
            *was_pct = 1;
            break;
        }
        if (lb[i] < '0' || lb[i] > '9')
            return -1;
        v = mul32(v, 10) + (lb[i] - '0');
    }
    return (long)v;
}

/* ---- Partitionsdaten --------------------------------------------------- */
/* Bezeichnung und Dateisystem aus dem Bootsektor der Partition */
static void vol_info(unsigned long start, char *label, char *sys)
{
    unsigned i, o;
    scopy(label, "", 12);
    scopy(sys, "UNBEKANNT", 10);
    if (rw(start, 0) || sec[510] != 0x55 || sec[511] != 0xAA ||
        sec[11] != 0 || sec[12] != 2)   /* 512 Byte je Sektor           */
        return;
    /* erweiterter BPB: FAT12/16 bei 26h, FAT32 (Sektoren je FAT 16 Bit
       = 0) bei 42h - Bezeichnung und Dateisystem liegen dort 1Ch weiter */
    o = !sec[0x16] && !sec[0x17] && sec[0x42] == 0x29 ? 0x1C : 0;
    if (sec[0x26 + o] == 0x29) {
        for (i = 0; i < 11; i++)
            label[i] = sec[0x2B + o + i];
        label[11] = 0;
        for (i = 11; i > 0 && label[i - 1] == ' '; i--)
            label[i - 1] = 0;
        if (label[0] == 'N' && label[1] == 'O' && label[2] == ' ')
            label[0] = 0;               /* "NO NAME"                    */
        for (i = 0; i < 8 && sec[0x36 + o + i] != ' '; i++)
            sys[i] = sec[0x36 + o + i];
        sys[i] = 0;
    } else if (o == 0 && !sec[0x16] && !sec[0x17])
        scopy(sys, "FAT32", 10);
    else
        scopy(sys, "FAT", 10);
}

/* Laufwerksbuchstabe der Partition (Startsektor und Groesse wie bei
   DosQFSInfo), 0 = keiner */
static char letter(unsigned long start, unsigned long size)
{
    unsigned d;
    for (d = 2; d < 25; d++)
        if (DosQFSInfo(d, &fsi) == 0 && fsi.hidden == start && fsi.sectors == size)
            return (char)('A' + d);
    return 0;
}

static const char *type_name(unsigned char t)
{
    return is_fat(t) ? "PRI DOS" : is_ext(t) ? "EXT DOS" : "Nicht-DOS";
}

/* Tabelle der Partitionen ab Zeile r -> naechste Zeile */
static int draw_parts(int r)
{
    static char label[12], sys[10];
    unsigned i;
    char lw[3];
    int any = 0;

    at(r++, 4, "Partition   Status   Typ       Bezeichnung   MByte   System    benutzt");
    for (i = 0; i < 4; i++) {
        if (!pt[i].type)
            continue;
        any = 1;
        lclr();
        lw[0] = is_fat(pt[i].type) ? letter(pt[i].start, pt[i].size) : 0;
        lw[1] = ':';
        lw[2] = 0;
        if (lw[0])
            lput(5, lw);
        lput(8, ns(i + 1, 1));
        if (pt[i].boot == 0x80)
            lput(18, "A");
        lput(24, type_name(pt[i].type));
        if (is_fat(pt[i].type)) {
            vol_info(pt[i].start, label, sys);
            lput(34, label);
            lput(56, sys);
        }
        lput(46, ns(mb(pt[i].size), 6));
        lput(66, ns(pct(pt[i].size, di.sectors), 4));
        lput(70, "%");
        lend();
        at(r++, 0, ln);
    }
    if (!any)
        at(r++, 4, "Keine Partitionen definiert");
    return r;
}

static void total_line(int r)
{
    lclr();
    lseq(4, "Speicherplatz auf Festplatte insgesamt: ", mb(di.sectors),
         " MByte (1 MByte = 1048576 Byte)");
    lend();
    at(r, 0, ln);
}

/* logische Laufwerke ab Zeile r -> naechste Zeile */
static int draw_logs(int r)
{
    static char label[12], sys[10];
    unsigned i;
    char lw[3];

    at(r++, 4, "Lw  Bezeichnung   MByte   System    benutzt");
    for (i = 0; i < nlg; i++) {
        lclr();
        lw[0] = letter(lg[i].start, lg[i].size);
        lw[1] = ':';
        lw[2] = 0;
        if (!lw[0]) {
            lw[0] = (char)('1' + i);    /* noch ohne Buchstaben: Nummer */
            lw[1] = '.';
        }
        lput(4, lw);
        vol_info(lg[i].start, label, sys);
        lput(8, label);
        lput(20, ns(mb(lg[i].size), 6));
        lput(30, is_fat(lg[i].type) ? sys : "Nicht-DOS");
        lput(40, ns(pct(lg[i].size, pt[ext].size), 4));
        lput(44, "%");
        lend();
        at(r++, 0, ln);
    }
    return r;
}

/* ---- Freier Platz ------------------------------------------------------ */
static unsigned long align_up(unsigned long lba)
{
    unsigned r;
    unsigned long q = div32(lba, cylsz, &r);
    return r ? mul32(q + 1, cylsz) : lba;
}

/* groesste Luecke zwischen den MBR-Eintraegen -> Anfang, Laenge */
static unsigned long free_mbr(unsigned long *start)
{
    unsigned long pos = spt, best = 0, a, b, nx;   /* ab Spur 1           */
    unsigned i;
    *start = 0;
    for (;;) {
        nx = di.sectors;                /* naechster belegter Anfang    */
        for (i = 0; i < 4; i++)
            if (pt[i].type && pt[i].start >= pos && pt[i].start < nx)
                nx = pt[i].start;
        a = pos == spt ? pos : align_up(pos);
        b = nx;
        if (b > a && b - a > best) {
            best = b - a;
            *start = a;
        }
        if (nx >= di.sectors)
            break;
        for (i = 0; i < 4; i++)         /* hinter diese Partition       */
            if (pt[i].type && pt[i].start == nx)
                pos = pt[i].start + pt[i].size;
    }
    return best;
}

/* Typ nach Groesse; mit grossen Platten ab 512 MB FAT32 wie Windows 98
   (0Bh innerhalb der ersten 1024 Zylinder, sonst 0Ch = LBA) */
static unsigned char fat_type(unsigned long size, unsigned long end)
{
    if (big && size >= 512UL * MB)
        return end > ((unsigned long)cylsz << 10) ? 0x0C : 0x0B;
    return size < 32680UL ? 0x01 : size < 65536UL ? 0x04 : 0x06;
}


/* ---- Befehle ----------------------------------------------------------- */
static void write_failed(void)
{
    error("Fehler beim Schreiben der Partitionstabelle.");
}

/* Groesse erfragen (MByte oder %), r = Zeile -> Sektoren, 0 = Esc */
static unsigned long ask_size(int r, const char *text, unsigned long avail,
                              unsigned long maxs, unsigned long base)
{
    long v;
    int was_pct;
    unsigned long s;
    if (avail > maxs)
        avail = maxs;
    for (;;) {
        if (field(r, text, ns(mb(avail), 0), "0123456789%") < 0)
            return 0;
        v = numval(&was_pct);
        if (v > 0 && was_pct && v <= 100)
            s = div32(mul32(base >> 7, (unsigned)v), 100, 0) << 7;   /* % */
        else if (v > 0 && !was_pct)
            s = (unsigned long)v == (avail >> 11) ? avail : (unsigned long)v << 11;
        else
            s = 0;
        if (s && s <= avail)
            return s;
        if (s > avail && s - avail < MB)
            return avail;               /* Rundung                      */
        error("Unzul\x84ssige Eingabe, bitte eine g\x81ltige Gr\x94\xE1" "e eingeben.");
    }
}

static int free_slot(void)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        if (!pt[i].type)
            return (int)i;
    return -1;
}

static void create_logical(void);

static void create_primary(int extended)
{
    int slot = free_slot(), k, maxact;
    unsigned long start, avail, size, end;
    unsigned i;

    screen(extended ? "Erstellen einer erweiterten DOS-Partition"
                    : "Erstellen einer prim\x84ren DOS-Partition");
    cur_disk(5);
    if (slot < 0) {
        error("Die Partitionstabelle ist voll.");
        wait_esc(0);
        return;
    }
    if (extended && ext < 4) {
        error("Erweiterte DOS-Partition ist bereits vorhanden.");
        wait_esc(0);
        return;
    }
    avail = free_mbr(&start);
    if (avail < cylsz) {
        error("Kein Speicherplatz zum Erstellen einer DOS-Partition vorhanden.");
        wait_esc(0);
        return;
    }
    size = 0;
    maxact = 0;
    if (!extended) {                    /* wie MS-DOS: alles und aktiv?  */
        at(7, 4, "Soll der maximal verf\x81gbare Speicherplatz f\x81r eine prim\x84re");
        k = yn(8, "DOS-Partition verwendet und diese Partition aktiviert werden (J/N)", 1);
        if (k < 0)
            return;
        if (k) {
            size = avail > fmax() ? fmax() : avail;
            maxact = 1;
        }
    }
    if (!size) {
        screen(extended ? "Erstellen einer erweiterten DOS-Partition"
                        : "Erstellen einer prim\x84ren DOS-Partition");
        cur_disk(5);
        draw_parts(7);
        total_line(14);
        lclr();
        lput(4, "Verf\x81gbarer Speicherplatz f\x81r Partition: ");
        lput(46, ns(mb(avail), 5));
        lput(52, "MByte (");
        lput(59, ns(pct(avail, di.sectors), 0));
        lput(59 + slen(nb), "%)");
        lend();
        at(15, 0, ln);
        at(17, 4, "Geben Sie die Partitionsgr\x94\xE1" "e in MByte oder Prozent des");
        size = ask_size(18, extended ? "Festplattenspeichers (%) ein, um eine erweiterte DOS-Partition"
                                     : "Festplattenspeichers (%) ein, um eine prim\x84re DOS-Partition",
                        avail, extended ? avail : fmax(), di.sectors);
        if (!size)
            return;
    }
    end = align_up(start + size);       /* auf Zylinderende             */
    if (end > start + avail)
        end = start + avail;
    pt[slot].start = start;
    pt[slot].size = end - start;
    pt[slot].boot = 0;
    pt[slot].type = extended ? (big && end > ((unsigned long)cylsz << 10) ? 0x0F : 0x05)
                             : fat_type(end - start, end);
    if (maxact) {                       /* "maximal und aktiv"          */
        for (i = 0; i < 4; i++)
            pt[i].boot = 0;
        pt[slot].boot = 0x80;
    }
    if (extended) {
        ext = (unsigned)slot;
        nlg = 0;
    }
    screen(extended ? "Erstellen einer erweiterten DOS-Partition"
                    : "Erstellen einer prim\x84ren DOS-Partition");
    cur_disk(5);
    if (write_mbr() || (extended && write_ebrs())) {
        write_failed();
        wait_esc(0);
        return;
    }
    draw_parts(7);
    error(extended ? "Erweiterte DOS-Partition erstellt"
                   : "Prim\x84re DOS-Partition erstellt");
    wait_esc("dr\x81" "cken, um fortzufahren");
    if (extended)
        create_logical();               /* wie MS-DOS gleich weiter     */
}

static void create_logical(void)
{
    unsigned long ebr, avail, size, end, pend;
    for (;;) {
        screen("Erstellen logischer DOS-Laufwerke in der erweiterten DOS-Partition");
        if (ext == 4) {
            error("Es gibt keine erweiterte DOS-Partition.");
            wait_esc(0);
            return;
        }
        draw_logs(5);
        pend = pt[ext].start + pt[ext].size;
        ebr = nlg ? align_up(lg[nlg - 1].start + lg[nlg - 1].size) : pt[ext].start;
        lclr();
        lseq(4, "Gesamtgr\x94\xE1" "e der erweiterten DOS-Partition: ", mb(pt[ext].size),
             " MByte");
        lend();
        at(17, 0, ln);
        if (nlg == MAXLOG || ebr + spt + cylsz > pend) {
            error("Der gesamte verf\x81gbare Speicherplatz in der erweiterten DOS-");
            at(23, 4, "Partition ist logischen Laufwerken zugeordnet.");
            wait_esc(0);
            return;
        }
        avail = pend - (ebr + spt);
        lclr();
        lseq(4, "Verf\x81gbarer Speicherplatz f\x81r logische Laufwerke: ", mb(avail),
             " MByte");
        lend();
        at(18, 0, ln);
        at(20, 4, "Geben Sie die Gr\x94\xE1" "e des logischen Laufwerks in MByte oder");
        size = ask_size(21, "Prozent der erweiterten Partition (%) ein", avail, fmax(),
                        pt[ext].size);
        if (!size)
            return;
        end = align_up(ebr + spt + size);
        if (end > pend)
            end = pend;
        lg[nlg].start = ebr + spt;
        lg[nlg].size = end - lg[nlg].start;
        lg[nlg].type = fat_type(lg[nlg].size, end);
        nlg++;
        if (write_ebrs()) {
            nlg--;
            write_failed();
            wait_esc(0);
            return;
        }
    }
}

static void menu_create(void)
{
    long k;
    for (;;) {
        screen("Erstellen einer DOS-Partition oder eines logischen DOS-Laufwerks");
        cur_disk(5);
        at(7, 4, "W\x84hlen Sie eine der folgenden Optionen:");
        at(9, 4, "1. Erstellen einer prim\x84ren DOS-Partition");
        at(10, 4, "2. Erstellen einer erweiterten DOS-Partition");
        at(11, 4, "3. Erstellen logischer DOS-Laufwerke in der erweiterten DOS-Partition");
        esc_line("dr\x81" "cken, um zu den FDISK-Optionen zur\x81" "ckzukehren");
        if (field(14, "Geben Sie Ihre Wahl ein:", "1", "123") < 0)
            return;
        k = numval(0);
        if (k == 1) { create_primary(0); return; }
        if (k == 2) { create_primary(1); return; }
        if (k == 3) { create_logical(); return; }
    }
}

static void menu_active(void)
{
    long k;
    unsigned i;
    for (;;) {
        screen("Festlegen der aktiven Partition");
        cur_disk(5);
        draw_parts(7);
        total_line(14);
        esc_line("dr\x81" "cken, um zu den FDISK-Optionen zur\x81" "ckzukehren");
        if (field(16, "Geben Sie die Nummer der Partition ein, die aktiviert werden soll", "", "1234") < 0)
            return;
        k = numval(0);
        if (k < 1 || k > 4 || !pt[k - 1].type) {
            error("Unzul\x84ssige Eingabe.");
            continue;
        }
        if (!is_fat(pt[k - 1].type)) {
            error("Nur prim\x84re DOS-Partitionen k\x94nnen aktiviert werden.");
            wait_esc(0);
            return;
        }
        for (i = 0; i < 4; i++)
            pt[i].boot = (unsigned char)(i == (unsigned)(k - 1) ? 0x80 : 0);
        if (write_mbr())
            write_failed();
        else {
            lclr();
            lput(4, "Partition ");
            lput(14, ns((unsigned long)k, 0));
            lput(16, "wurde aktiviert.");
            lend();
            error(ln + 4);
        }
        wait_esc(0);
        return;
    }
}

/* Bezeichnung zur Bestaetigung (wie MS-DOS), danach J/N */
static int sure(int r, unsigned long start)
{
    static char label[12], sys[10];
    vol_info(start, label, sys);
    if (label[0]) {
        if (field(r++, "Geben Sie die Datentr\x84gerbezeichnung ein", "",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") < 0)
            return 0;
        if (!same(lb, label)) {
            error("Datentr\x84gerbezeichnung stimmt nicht \x81" "berein.");
            wait_esc(0);
            return 0;
        }
    }
    return yn(r, "Sind Sie sicher (J/N)", 0) == 1;
}

static void delete_part(int kind)       /* 1 primaer, 2 erweitert, 4 Nicht-DOS */
{
    long k;
    unsigned i;
    screen(kind == 1 ? "L\x94schen der prim\x84ren DOS-Partition" :
           kind == 2 ? "L\x94schen der erweiterten DOS-Partition" :
                       "L\x94schen einer Nicht-DOS-Partition");
    cur_disk(5);
    draw_parts(7);
    total_line(14);
    esc_line("dr\x81" "cken, um zu den FDISK-Optionen zur\x81" "ckzukehren");
    if (kind == 2) {
        if (ext == 4) {
            error("Es gibt keine erweiterte DOS-Partition.");
            wait_esc(0);
            return;
        }
        if (nlg) {
            error("Zuerst die logischen Laufwerke in der erweiterten Partition l\x94schen.");
            wait_esc(0);
            return;
        }
        at(16, 4, "ACHTUNG! Die Daten in der gel\x94schten erweiterten DOS-Partition");
        at(17, 4, "gehen verloren.");
        if (yn(18, "M\x94" "chten Sie fortfahren (J/N)", 0) != 1)
            return;
        i = ext;
    } else {
        at(16, 4, kind == 1 ? "ACHTUNG! Die Daten in der gel\x94schten prim\x84ren DOS-Partition"
                            : "ACHTUNG! Die Daten in der gel\x94schten Nicht-DOS-Partition");
        at(17, 4, "gehen verloren.");
        if (field(18, kind == 1 ? "Welche prim\x84re Partition m\x94" "chten Sie l\x94schen"
                                : "Welche Nicht-DOS-Partition m\x94" "chten Sie l\x94schen",
                  "", "1234") < 0)
            return;
        k = numval(0);
        if (k < 1 || k > 4 || !pt[k - 1].type ||
            (kind == 1 && !is_fat(pt[k - 1].type)) ||
            (kind == 4 && (is_fat(pt[k - 1].type) || is_ext(pt[k - 1].type)))) {
            error("Unzul\x84ssige Eingabe.");
            wait_esc(0);
            return;
        }
        i = (unsigned)k - 1;
        if (!(kind == 1 ? sure(19, pt[i].start) : yn(19, "Sind Sie sicher (J/N)", 0) == 1))
            return;
    }
    pt[i].type = 0;
    pt[i].boot = 0;
    if (i == ext)
        ext = 4;
    if (write_mbr())
        write_failed();
    else
        error(kind == 2 ? "Erweiterte DOS-Partition gel\x94scht" : "Partition gel\x94scht");
    wait_esc(0);
}

static void delete_logical(void)
{
    long k;
    unsigned i, n;
    char c;
    screen("L\x94schen logischer DOS-Laufwerke in der erweiterten DOS-Partition");
    if (!nlg) {
        error("Keine logischen DOS-Laufwerke definiert.");
        wait_esc(0);
        return;
    }
    draw_logs(5);
    esc_line("dr\x81" "cken, um zu den FDISK-Optionen zur\x81" "ckzukehren");
    at(17, 4, "ACHTUNG! Die Daten im gel\x94schten logischen DOS-Laufwerk gehen verloren.");
    if (field(18, "Welches Laufwerk (Buchstabe oder Nummer) m\x94" "chten Sie l\x94schen", "",
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") < 0)
        return;
    k = -1;
    c = lb[0];
    if (c >= '1' && c <= '9' && !lb[1])
        k = c - '1';
    else if (c >= 'C' && c <= 'Z' && !lb[1])
        for (n = 0; n < nlg; n++)
            if (letter(lg[n].start, lg[n].size) == c)
                k = (long)n;
    if (k < 0 || k >= (long)nlg) {
        error("Unzul\x84ssige Eingabe.");
        wait_esc(0);
        return;
    }
    if (!sure(19, lg[k].start))
        return;
    for (i = (unsigned)k; i + 1 < nlg; i++) {
        lg[i].type = lg[i + 1].type;
        lg[i].start = lg[i + 1].start;
        lg[i].size = lg[i + 1].size;
    }
    nlg--;
    if (write_ebrs())
        write_failed();
    else
        error("Logisches DOS-Laufwerk gel\x94scht");
    wait_esc(0);
}

static void menu_delete(void)
{
    long k;
    for (;;) {
        screen("L\x94schen einer DOS-Partition oder eines logischen DOS-Laufwerks");
        cur_disk(5);
        at(7, 4, "W\x84hlen Sie eine der folgenden Optionen:");
        at(9, 4, "1. L\x94schen der prim\x84ren DOS-Partition");
        at(10, 4, "2. L\x94schen der erweiterten DOS-Partition");
        at(11, 4, "3. L\x94schen logischer DOS-Laufwerke in der erweiterten DOS-Partition");
        at(12, 4, "4. L\x94schen einer Nicht-DOS-Partition");
        esc_line("dr\x81" "cken, um zu den FDISK-Optionen zur\x81" "ckzukehren");
        if (field(15, "Geben Sie Ihre Wahl ein:", "", "1234") < 0)
            return;
        k = numval(0);
        if (k == 3) { delete_logical(); return; }
        if (k >= 1 && k <= 4) { delete_part((int)k); return; }
    }
}

static void show_data(void)
{
    screen("Anzeigen der Partitionierungsdaten");
    cur_disk(5);
    draw_parts(7);
    total_line(14);
    esc_line("dr\x81" "cken, um zu den FDISK-Optionen zur\x81" "ckzukehren");
    if (ext < 4 && nlg) {
        at(16, 4, "Die erweiterte DOS-Partition enth\x84lt logische DOS-Laufwerke.");
        if (yn(17, "Sollen die Daten der logischen Laufwerke angezeigt werden (J/N)", 1) != 1)
            return;
        screen("Anzeigen der Daten der logischen DOS-Laufwerke");
        draw_logs(5);
        lclr();
        lseq(4, "Gesamtgr\x94\xE1" "e der erweiterten DOS-Partition: ", mb(pt[ext].size),
             " MByte (1 MByte = 1048576 Byte)");
        lend();
        at(17, 0, ln);
    }
    wait_esc(0);
}

static int open_disk(unsigned n, int lock)
{
    if (hd) {
        DosClose(hd);
        hd = 0;
    }
    if (DosDiskInfo(n, &di) != 0)
        return -1;
    hd = DosOpenDisk(n, (lock ? DRV_OPEN_LOCK : DRV_OPEN_READ) | DRV_OPEN_LBA);
    if (FAPI_FAILED(hd)) {
        hd = 0;
        return -2;
    }
    hd_lock_state = lock;
    disk = n;
    return load_disk() ? -3 : 0;
}

/* Uebersicht aller Platten (Wechseln, /STATUS); out: Zeile ausgeben */
static void overview(void (*out)(void))
{
    unsigned n, i, cur = disk;
    unsigned long used;
    char lw[3];

    for (n = 0; n < ndisks; n++) {
        if (open_disk(n, 0))
            continue;
        for (used = 0, i = 0; i < 4; i++)
            if (pt[i].type)
                used += pt[i].size;
        lclr();
        lput(4, ns(n + 1, 1));
        lput(18, ns(mb(di.sectors), 6));
        lput(28, ns(mb(di.sectors > used ? di.sectors - used : 0), 6));
        lput(37, ns(pct(used, di.sectors), 4));
        lput(41, "%");
        lend();
        out();
        for (i = 0; i < 4; i++)         /* Laufwerke darauf             */
            if (is_fat(pt[i].type) && (lw[0] = letter(pt[i].start, pt[i].size)) != 0) {
                lw[1] = ':';
                lw[2] = 0;
                lclr();
                lput(12, lw);
                lput(18, ns(mb(pt[i].size), 6));
                lend();
                out();
            }
        for (i = 0; i < nlg; i++)
            if ((lw[0] = letter(lg[i].start, lg[i].size)) != 0) {
                lw[1] = ':';
                lw[2] = 0;
                lclr();
                lput(12, lw);
                lput(18, ns(mb(lg[i].size), 6));
                lend();
                out();
            }
    }
    disk = cur;
}

static int ov_row;
static void out_screen(void)
{
    at(ov_row++, 0, ln);
}

static void out_text(void)
{
    puts(ln);
    puts("\r\n");
}

static void change_disk(void)
{
    long k;
    unsigned cur = disk;
    screen("Wechseln der aktuellen Festplatte");
    at(5, 4, "Festpl.   Lw   MByte    frei  benutzt");
    ov_row = 6;
    overview(out_screen);
    at(20, 4, "(1 MByte = 1048576 Byte)");
    esc_line("dr\x81" "cken, um zu den FDISK-Optionen zur\x81" "ckzukehren");
    for (;;) {
        lclr();
        lput(0, "Geben Sie die Nummer der Festplatte ein (1-");
        lput(43, ns(ndisks, 0));
        lput(43 + slen(nb), ")");
        lend();
        if (field(21, ln, "", "123456789") < 0)
            break;
        k = numval(0);
        if (k >= 1 && k <= (long)ndisks) {
            cur = (unsigned)k - 1;
            break;
        }
        lclr();
        lput(0, "Unzul\x84ssige Eingabe, bitte 1-");
        lput(29, ns(ndisks, 0));
        lput(29 + slen(nb), " eingeben.");
        lend();
        error(ln);
    }
    if (open_disk(cur, 1))
        open_disk(cur, 0);              /* belegt: nur anzeigen         */
}

static void main_menu(void)
{
    unsigned i, act;
    screen(0);
    center(1, "AT-DOS Version 6.0", 0x07);
    center(2, "Festplatten-Setup-Programm", 0x07);
    center(4, "FDISK-Optionen", A_HI);
    cur_disk(6);
    at(8, 4, "W\x84hlen Sie eine der folgenden Optionen:");
    at(10, 4, "1. Erstellen einer DOS-Partition oder eines logischen DOS-Laufwerks");
    at(11, 4, "2. Festlegen der aktiven Partition");
    at(12, 4, "3. L\x94schen einer Partition oder eines logischen DOS-Laufwerks");
    at(13, 4, "4. Anzeigen der Partitionierungsdaten");
    if (ndisks > 1)
        at(14, 4, "5. Wechseln der aktuellen Festplatte");
    for (act = 0, i = 0; i < 4; i++)
        if (pt[i].type && pt[i].boot == 0x80)
            act = 1;
    if (!act && disk == 0) {
        error("ACHTUNG! Keine Partition ist aktiv - Festplatte 1 kann nicht gestartet");
        at(23, 4, "werden, bis eine Partition aktiviert wurde.");
    }
    if (!hd_locked)
        at(16, 4, "(Festplatte wird benutzt - nur Anzeige m\x94glich)");
    esc_line("dr\x81" "cken, um FDISK zu beenden");
}

void main(void)
{
    char *a;
    long k;
    unsigned i;
    int rc;

    DosGetArgs(args);
    a = skip(args);
    for (ndisks = 0; DosDiskInfo(ndisks, &di) == 0; ndisks++)
        ;
    if (!ndisks) {
        puts("Keine Festplatte gefunden.\r\n");
        DosExit(1);
    }
    if (same(a, "/STATUS")) {           /* wie MS-DOS: Uebersicht als Text */
        puts("\r\n                Festplattenspeicher \x81" "bersicht\r\n\r\n"
             "    Festpl.   Lw   MByte    frei  benutzt\r\n");
        overview(out_text);
        puts("\r\n    (1 MByte = 1048576 Byte)\r\n");
        if (hd)
            DosClose(hd);
        return;
    }
    if (same(a, "/MBR")) {              /* Startcode erneuern           */
        if (open_disk(0, 1) || rw(0, 0)) {
            puts("Die erste Festplatte ist nicht verf\x81gbar.\r\n");
            DosExit(1);
        }
        for (i = 0; i < 446; i++)
            sec[i] = mbr_code[i];
        sec[510] = 0x55;
        sec[511] = 0xAA;
        puts(rw(0, 1) ? "Fehler beim Schreiben.\r\n"
                      : "Startcode der ersten Festplatte erneuert.\r\n");
        DosClose(hd);
        return;
    }
    for (i = 0; i < ndisks; i++)        /* wie Windows 98: ueber 512 MB fragen */
        if (DosDiskInfo(i, &di) == 0 && di.sectors > 512UL * MB)
            big = -1;
    rc = open_disk(0, 1);
    if (rc == -2)
        rc = open_disk(0, 0);           /* belegt: nur anzeigen         */
    if (rc) {
        puts("Die Festplatte ist nicht lesbar.\r\n");
        DosExit(1);
    }
    k = VioSetMode(1);
    if (!FAPI_FAILED(k) && k >= 25)
        rows = (unsigned)k;
    if (big) {
        screen(0);
        at(4, 4, "Ihr Computer hat eine Festplatte mit mehr als 512 MB. AT-DOS");
        at(5, 4, "unterst\x81tzt gro\xE1" "e Festplatten mit FAT32: Laufwerke \x81" "ber 2 GB als");
        at(6, 4, "ein Laufwerk, kleinere Zuordnungseinheiten. Neue Partitionen ab");
        at(7, 4, "512 MB werden dann FAT32 (danach FORMAT, FAT32 ist dort die Vorgabe).");
        at(9, 4, "MS-DOS 6 und \x84lter und Windows 95 vor OSR2 k\x94nnen auf");
        at(10, 4, "FAT32-Laufwerke nicht zugreifen.");
        k = yn(13, "Unterst\x81tzung f\x81r gro\xE1" "e Festplatten aktivieren (J/N)", 1);
        if (k < 0) {
            VioSetMode(0);
            DosClose(hd);
            return;
        }
        big = (int)k;
    }
    for (;;) {
        hd_locked = hd_lock_state;
        main_menu();
        if (field(18, "Geben Sie Ihre Wahl ein:", "1", ndisks > 1 ? "12345" : "1234") < 0)
            break;
        k = numval(0);
        if (!hd_locked && k >= 1 && k <= 3) {
            error("Die Festplatte wird gerade benutzt (offene Dateien).");
            wait_esc(0);
            continue;
        }
        switch ((int)k) {
        case 1: menu_create(); break;
        case 2: menu_active(); break;
        case 3: menu_delete(); break;
        case 4: show_data(); break;
        case 5: if (ndisks > 1) change_disk(); break;
        }
    }
    DosClose(hd);
    if (changed) {                      /* wie MS-DOS: Neustart          */
        screen(0);
        at(10, 4, "Sie M\x9A" "SSEN das System neu starten, damit die \x8E" "nderungen wirksam");
        at(11, 4, "werden. Alle Laufwerke, die Sie erstellt oder ge\x84ndert haben, m\x81ssen");
        at(12, 4, "nach dem Neustart formatiert werden.");
        at(15, 4, "Das System wird neu gestartet.");
        wait_esc("dr\x81" "cken, um FDISK zu beenden");
        DosShutdown(1);
    }
    VioSetMode(0);
}
