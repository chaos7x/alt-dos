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
static int changed;
static char args[FAPI_ARGLEN], lb[16];

static int is_fat(unsigned char t) { return t == 1 || t == 4 || t == 6 || t == 0x0E; }
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
    pos = lba << 9;
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

/* ---- Ausgabe ----------------------------------------------------------- */
static void mbn(unsigned long sectors, int w)
{
    put_num(sectors >> 11, w, 0);
}

static unsigned pct(unsigned long part, unsigned long total)
{
    while (total > 600) {               /* 16-Bit-Rechnung              */
        total >>= 1;
        part >>= 1;
    }
    return total ? (unsigned)((unsigned)part * 100 / (unsigned)total) : 0;
}

static const char *type_name(unsigned char t)
{
    return is_fat(t) ? "PRI DOS " : is_ext(t) ? "EXT DOS " : "Nicht-DOS";
}

static void header(void)
{
    puts("\r\nAT-DOS-Festplatteneinrichtung (FDISK)\r\n\r\nAktuelle Festplatte: ");
    put_num(disk + 1, 0, 0);
    puts("   (");
    puts(di.driver);
    puts(", Einheit ");
    put_num(di.unit, 0, 0);
    puts(", ");
    mbn(di.sectors, 0);
    puts(" MB, Geometrie ");
    put_num(heads, 0, 0);
    putc('/');
    put_num(spt, 0, 0);
    puts(di.heads ? " vom BIOS)\r\n\r\n" : ")\r\n\r\n");
}

static void show_table(void)
{
    unsigned i, any = 0;
    puts("Partition  Status  Typ        Gr\x94\xE1" "e in MB  Prozent\r\n");
    for (i = 0; i < 4; i++) {
        if (!pt[i].type)
            continue;
        any = 1;
        puts("    ");
        put_num(i + 1, 1, 0);
        puts(pt[i].boot == 0x80 ? "        A     " : "              ");
        puts(type_name(pt[i].type));
        puts("   ");
        mbn(pt[i].size, 8);
        puts("     ");
        put_num(pct(pt[i].size, di.sectors), 3, 0);
        puts("%\r\n");
    }
    if (!any)
        puts("    Keine Partitionen definiert.\r\n");
    if (ext < 4 && nlg) {
        puts("\r\nLogische Laufwerke in der erweiterten Partition:\r\n");
        for (i = 0; i < nlg; i++) {
            puts("    ");
            put_num(i + 1, 2, 0);
            puts("   ");
            mbn(lg[i].size, 8);
            puts(" MB  ");
            put_num(pct(lg[i].size, pt[ext].size), 3, 0);
            puts(is_fat(lg[i].type) ? "%\r\n" : "%  (Nicht-DOS)\r\n");
        }
    }
    puts("\r\nGesamter Speicherplatz: ");
    mbn(di.sectors, 0);
    puts(" MB\r\n");
}

/* ---- Eingabe ----------------------------------------------------------- */
static void wait_esc(void)
{
    puts("\r\nESC dr\x81" "cken, um fortzufahren");
    while ((KbdCharIn() & 0xFF) != 27)
        ;
    puts("\r\n");
}

/* Zahl lesen (Vorgabe def); -1 = Esc */
static long read_num(const char *prompt, unsigned long def)
{
    unsigned n, c;
    unsigned long v;
    for (;;) {
        puts(prompt);
        puts(" [");
        put_num(def, 0, 0);
        puts("]: ");
        n = 0;
        for (;;) {
            c = KbdCharIn() & 0xFF;
            if (c == 27) {
                puts("\r\n");
                return -1;
            }
            if (c == 13)
                break;
            if (c == 8 && n) {
                n--;
                puts("\b \b");
            } else if (c >= '0' && c <= '9' && n < 7) {
                lb[n++] = (char)c;
                putc((char)c);
            }
        }
        puts("\r\n");
        if (!n)
            return (long)def;
        for (v = 0, c = 0; c < n; c++)
            v = mul32(v, 10) + (lb[c] - '0');
        return (long)v;
    }
}

static int confirm(const char *q)
{
    puts(q);
    puts(" (J/N)? ");
    return ask_yn();
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

static unsigned char fat_type(unsigned long size)
{
    return size < 32680UL ? 0x01 : size < 65536UL ? 0x04 : 0x06;
}

static unsigned long ask_size(unsigned long avail, unsigned long maxs)
{
    long mb;
    unsigned long s;
    if (avail > maxs)
        avail = maxs;
    puts("Verf\x81gbarer Speicherplatz: ");
    mbn(avail, 0);
    puts(" MB\r\n");
    mb = read_num("Gr\x94\xE1" "e in MB", avail >> 11);
    if (mb <= 0)
        return 0;
    if ((unsigned long)mb == avail >> 11)
        return avail;                   /* Vorgabe: wirklich alles      */
    s = (unsigned long)mb << 11;
    if (s > avail)
        s = avail;
    return s;
}

/* ---- Befehle ----------------------------------------------------------- */
static void create_primary(int extended)
{
    unsigned i, slot = 4;
    unsigned long start, avail, size, end;

    for (i = 0; i < 4; i++)
        if (!pt[i].type) {
            slot = i;
            break;
        }
    if (slot == 4) {
        puts("Die Partitionstabelle ist voll (vier Eintr\x84ge).\r\n");
        return;
    }
    if (extended && ext < 4) {
        puts("Es gibt bereits eine erweiterte DOS-Partition.\r\n");
        return;
    }
    avail = free_mbr(&start);
    if (avail < cylsz) {
        puts("Kein freier Speicherplatz auf der Festplatte.\r\n");
        return;
    }
    size = ask_size(avail, extended ? avail : FAT_MAX);
    if (!size)
        return;
    end = align_up(start + size);       /* auf Zylinderende             */
    if (end > start + avail)
        end = start + avail;
    pt[slot].start = start;
    pt[slot].size = end - start;
    pt[slot].boot = 0;
    pt[slot].type = extended ? 0x05 : fat_type(end - start);
    if (extended) {
        ext = slot;
        nlg = 0;
    }
    if (write_mbr() || (extended && write_ebrs()))
        puts("Fehler beim Schreiben der Partitionstabelle.\r\n");
    else
        puts(extended ? "Erweiterte DOS-Partition erstellt.\r\n"
                      : "Prim\x84re DOS-Partition erstellt. Mit FORMAT formatieren.\r\n");
}

static void create_logical(void)
{
    unsigned long ebr, avail, size, end, pend;
    if (ext == 4) {
        puts("Zuerst eine erweiterte DOS-Partition erstellen.\r\n");
        return;
    }
    if (nlg == MAXLOG) {
        puts("Keine weiteren logischen Laufwerke m\x94glich.\r\n");
        return;
    }
    pend = pt[ext].start + pt[ext].size;
    ebr = nlg ? align_up(lg[nlg - 1].start + lg[nlg - 1].size) : pt[ext].start;
    if (ebr + spt + cylsz > pend) {
        puts("Kein freier Platz in der erweiterten Partition.\r\n");
        return;
    }
    avail = pend - (ebr + spt);
    size = ask_size(avail, FAT_MAX);
    if (!size)
        return;
    end = align_up(ebr + spt + size);
    if (end > pend)
        end = pend;
    lg[nlg].start = ebr + spt;
    lg[nlg].size = end - lg[nlg].start;
    lg[nlg].type = fat_type(lg[nlg].size);
    nlg++;
    if (write_ebrs())
        puts("Fehler beim Schreiben.\r\n");
    else
        puts("Logisches DOS-Laufwerk erstellt. Mit FORMAT formatieren.\r\n");
}

static void menu_create(void)
{
    long k;
    header();
    puts("1. Prim\x84re DOS-Partition erstellen\r\n"
         "2. Erweiterte DOS-Partition erstellen\r\n"
         "3. Logische DOS-Laufwerke in der erweiterten Partition erstellen\r\n\r\n");
    k = read_num("Option eingeben", 1);
    if (k == 1) create_primary(0);
    else if (k == 2) create_primary(1);
    else if (k == 3) create_logical();
    if (k >= 1 && k <= 3)
        wait_esc();
}

static void menu_active(void)
{
    long k;
    unsigned i;
    header();
    show_table();
    k = read_num("\r\nNummer der Partition, die aktiv werden soll", 1);
    if (k < 1 || k > 4)
        return;
    if (!is_fat(pt[k - 1].type)) {
        puts("Nur prim\x84re DOS-Partitionen k\x94nnen aktiv sein.\r\n");
        wait_esc();
        return;
    }
    for (i = 0; i < 4; i++)
        pt[i].boot = (unsigned char)(i == (unsigned)(k - 1) ? 0x80 : 0);
    puts(write_mbr() ? "Fehler beim Schreiben.\r\n" : "Partition ist jetzt aktiv.\r\n");
    wait_esc();
}

static void menu_delete(void)
{
    long k, n;
    unsigned i;
    header();
    show_table();
    puts("\r\n1. Prim\x84re DOS-Partition l\x94schen\r\n"
         "2. Erweiterte DOS-Partition l\x94schen\r\n"
         "3. Logisches DOS-Laufwerk l\x94schen\r\n"
         "4. Nicht-DOS-Partition l\x94schen\r\n\r\n");
    k = read_num("Option eingeben", 3);
    if (k < 1 || k > 4)
        return;
    if (k == 3) {
        if (!nlg) {
            puts("Keine logischen Laufwerke vorhanden.\r\n");
        } else {
            n = read_num("Nummer des logischen Laufwerks", nlg);
            if (n >= 1 && n <= (long)nlg &&
                confirm("ACHTUNG: Alle Daten auf dem Laufwerk gehen verloren. Fortfahren")) {
                for (i = (unsigned)n - 1; i + 1 < nlg; i++) {
                    lg[i].type = lg[i + 1].type;
                    lg[i].start = lg[i + 1].start;
                    lg[i].size = lg[i + 1].size;
                }
                nlg--;
                puts(write_ebrs() ? "Fehler beim Schreiben.\r\n" : "Logisches Laufwerk gel\x94scht.\r\n");
            }
        }
        wait_esc();
        return;
    }
    if (k == 2 && nlg) {
        puts("Zuerst die logischen Laufwerke l\x94schen.\r\n");
        wait_esc();
        return;
    }
    n = read_num("Nummer der Partition", k == 2 && ext < 4 ? ext + 1 : 1);
    if (n < 1 || n > 4 || !pt[n - 1].type) {
        wait_esc();
        return;
    }
    i = (unsigned)n - 1;
    if ((k == 1 && !is_fat(pt[i].type)) || (k == 2 && !is_ext(pt[i].type)) ||
        (k == 4 && (is_fat(pt[i].type) || is_ext(pt[i].type)))) {
        puts("Die Partition hat einen anderen Typ.\r\n");
        wait_esc();
        return;
    }
    if (!confirm("ACHTUNG: Alle Daten in der Partition gehen verloren. Fortfahren")) {
        wait_esc();
        return;
    }
    pt[i].type = 0;
    pt[i].boot = 0;
    if (i == ext)
        ext = 4;
    puts(write_mbr() ? "Fehler beim Schreiben.\r\n" : "Partition gel\x94scht.\r\n");
    wait_esc();
}

static int open_disk(unsigned n)
{
    if (hd) {
        DosClose(hd);
        hd = 0;
    }
    if (DosDiskInfo(n, &di) != 0)
        return -1;
    hd = DosOpenDisk(n, DRV_OPEN_LOCK);
    if (FAPI_FAILED(hd)) {
        hd = 0;
        puts(" Die Festplatte wird gerade benutzt (offene Dateien).\r\n");
        return -1;
    }
    disk = n;
    if (load_disk()) {
        puts("Die Festplatte ist nicht lesbar.\r\n");
        return -1;
    }
    return 0;
}

static void status(void)
{
    unsigned n;
    for (n = 0; n < ndisks; n++) {
        if (DosDiskInfo(n, &di) != 0)
            continue;
        hd = DosOpenDisk(n, DRV_OPEN_READ);
        if (FAPI_FAILED(hd))
            continue;
        disk = n;
        if (!load_disk()) {
            header();
            show_table();
        }
        DosClose(hd);
        hd = 0;
    }
}

void main(void)
{
    char *a;
    long k;
    unsigned i;

    DosGetArgs(args);
    a = skip(args);
    for (ndisks = 0; DosDiskInfo(ndisks, &di) == 0; ndisks++)
        ;
    if (!ndisks) {
        puts("Keine Festplatte gefunden.\r\n");
        DosExit(1);
    }
    if (same(a, "/STATUS")) {
        status();
        return;
    }
    if (same(a, "/MBR")) {                  /* Startcode erneuern       */
        if (open_disk(0) || rw(0, 0))
            DosExit(1);
        for (i = 0; i < 446; i++)
            sec[i] = mbr_code[i];
        sec[510] = 0x55;
        sec[511] = 0xAA;
        puts(rw(0, 1) ? "Fehler beim Schreiben.\r\n" : "Startcode der ersten Festplatte erneuert.\r\n");
        DosClose(hd);
        return;
    }
    if (open_disk(0))
        DosExit(1);
    for (;;) {
        header();
        puts("1. DOS-Partition oder logisches DOS-Laufwerk erstellen\r\n"
             "2. Aktive Partition festlegen\r\n"
             "3. Partition oder logisches DOS-Laufwerk l\x94schen\r\n"
             "4. Partitionsdaten anzeigen\r\n");
        if (ndisks > 1)
            puts("5. N\x84" "chste Festplatte w\x84hlen\r\n");
        puts("\r\nESC = FDISK beenden\r\n");
        k = read_num("Option eingeben", 4);
        if (k < 0)
            break;
        switch ((int)k) {
        case 1: menu_create(); break;
        case 2: menu_active(); break;
        case 3: menu_delete(); break;
        case 4: header(); show_table(); wait_esc(); break;
        case 5:
            if (ndisks > 1)
                open_disk((disk + 1) % ndisks);
            break;
        }
    }
    DosClose(hd);
    if (changed) {
        puts("\r\nDie Laufwerksbuchstaben \x84ndern sich erst nach einem Neustart.\r\n");
        if (confirm("Jetzt neu starten"))
            DosShutdown(1);
    }
}
