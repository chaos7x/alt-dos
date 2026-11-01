/*
 * DISKCOPY.EXE - Diskette Spur fuer Spur kopieren (wie MS-DOS):
 *
 *   DISKCOPY [Laufwerk1: [Laufwerk2:]] [/V]
 *
 * Formate 360 KB, 720 KB, 1,2 MB, 1,44 MB, 2,88 MB - das der Quelle
 * erkennt der Kernel. Die Quelle kommt in so viele 64-KB-Segmente, wie
 * Speicher da ist (2,88 MB = 45 Segmente; reicht er nicht, in mehreren
 * Durchgaengen zu ganzen Zylindern). Bei einem Laufwerk (DISKCOPY A: A:)
 * wird zwischen Quelle und Ziel gewechselt; vor jedem Wechsel wird das
 * Laufwerk geschlossen, beim Oeffnen liest der Kernel die Diskette frisch.
 * Ist das Ziel unformatiert oder anders formatiert, wird wie bei MS-DOS
 * beim Kopieren formatiert (DosFmtTrack, Spur vor dem Schreiben). Die
 * Kopie bekommt eine neue Seriennummer. /V liest jede geschriebene Spur
 * zurueck und vergleicht.
 * Rueckgabe wie MS-DOS: 0 ok, 1 Lese-/Schreibfehler, 2 Abbruch,
 * 3 schwerer Fehler, 4 Speicher/Aufruf.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

#define MAXSPT  36                      /* 2,88 MB                       */
#define SECSEG  128                     /* Sektoren je 64-KB-Segment     */
#define MAXSEG  45

static char args[FAPI_ARGLEN];
static unsigned char trk[MAXSPT * 512], chk[MAXSPT * 512];
static unsigned seg[MAXSEG], nseg;
static struct fapi_fsinfo ffi;
static struct fapi_fmttrack ft;
static unsigned src, dst, hd = 0, verify, errors, fmt_all;
static unsigned total;                  /* Sektoren (hoechstens 5760)    */
static unsigned spt, heads = 2;

static void drive_txt(unsigned d)
{
    putc((char)('A' + d));
    putc(':');
}

static void wait_key(void)
{
    puts("Dr\x81" "cken Sie eine beliebige Taste, um fortzusetzen . . .");
    KbdCharIn();
    puts("\r\n\r\n");
}

static void insert(const char *what, unsigned d)
{
    puts(what);
    puts(" in Laufwerk ");
    drive_txt(d);
    puts(" einlegen\r\n");
    wait_key();
}

static int open_drv(unsigned d)
{
    hd = DosOpenDrive(d, DRV_OPEN_LOCK);
    if (FAPI_FAILED(hd)) {
        hd = 0;
        puts("Laufwerk ");
        drive_txt(d);
        puts(" kann nicht ge\x94" "ffnet werden.\r\n");
        return -1;
    }
    return 0;
}

static void close_drv(void)
{
    if (hd)
        DosClose(hd);
    hd = 0;
}

/* n Sektoren ab s nach / aus dem DS-Puffer b */
static int rw(unsigned s, unsigned n, unsigned char *b, int write)
{
    static unsigned long pos;
    unsigned r;
    pos = (unsigned long)s << 9;
    if (DosSeek(hd, &pos))
        return -1;
    r = write ? DosWrite(hd, b, n << 9) : DosRead(hd, b, n << 9);
    return r == (n << 9) ? 0 : -1;
}

/* Sektor i der Kopie im Speicher (relativ zum Durchgang) */
static unsigned char __far *mem_sec(unsigned i)
{
    return (unsigned char __far *)MK_FP(seg[i / SECSEG], (i % SECSEG) << 9);
}

static void copy_mem(unsigned i, unsigned n, unsigned char *b, int out)
{
    unsigned k, j;
    unsigned char __far *p;
    for (k = 0; k < n; k++) {
        p = mem_sec(i + k);
        for (j = 0; j < 512; j++)
            if (out)
                p[j] = b[(k << 9) + j];
            else
                b[(k << 9) + j] = p[j];
    }
}

static void where(unsigned s)
{
    unsigned t = s / spt;
    puts(" Seite ");
    put_num(t % heads, 0, 0);
    puts(", Spur ");
    put_num(t / heads, 0, 0);
    puts("\r\n");
}

/* Seriennummer aus Datum und Uhrzeit (wie MS-DOS) */
static unsigned long new_serial(void)
{
    static unsigned short dt[2];
    DosGetDateTime(dt);
    return ((unsigned long)(dt[0] + dt[1]) << 16) | (unsigned short)(dt[1] ^ (dt[0] << 3) ^ 0x5A5A);
}

static void hex8(unsigned long v)
{
    static const char hx[] = "0123456789ABCDEF";
    int k;
    for (k = 28; k >= 0; k -= 4) {
        putc(hx[(unsigned)(v >> k) & 15]);
        if (k == 16)
            putc('-');
    }
}

/* Spur mit Sektor s formatieren; -> 0, 1 schreibgeschuetzt, -1 Fehler */
static int fmt_track(unsigned s)
{
    unsigned t = s / spt, r;
    ft.cyl = t >> 1;
    ft.head = t & 1;
    ft.blocks = total;
    r = DosFmtTrack(hd, &ft);
    return r == 0 ? 0 : r == FAPI_ERR_WP ? 1 : -1;
}

/* eine Spur aus trk schreiben, notfalls vorher formatieren; -> 0 ok,
   1 Fehler (weiter), 3 schwer */
static int write_track(unsigned s, unsigned cnt)
{
    int r = 0;
    if (fmt_all)
        r = fmt_track(s);
    if (r == 0 && rw(s, cnt, trk, 1) == 0)
        return 0;
    if (r == 0 && !fmt_all) {               /* unformatiert?             */
        fmt_all = 1;
        puts("Formatieren w\x84hrend des Kopierens\r\n");
        r = fmt_track(s);
        if (r == 0 && rw(s, cnt, trk, 1) == 0)
            return 0;
    }
    puts(r == 1 ? "Die Zieldiskette ist schreibgesch\x81tzt.\r\n" :
         r < 0 ? "Die Zieldiskette kann nicht formatiert werden (Format passt nicht zum Laufwerk?).\r\n" :
                 "Schreibfehler auf Laufwerk ");
    if (r == 0) {
        drive_txt(dst);
        where(s);
        errors++;
        return 1;
    }
    return 3;
}

static unsigned spt_of(unsigned n)
{
    return n == 720 || n == 1440 ? 9 : n == 2400 ? 15 : n == 2880 ? 18 : n == 5760 ? 36 : 0;
}

/* ein Kopiervorgang; -> 0 ok, 1 Fehler (weiter), 3 schwer */
static int copy_once(void)
{
    unsigned long serial = 0;
    unsigned first, s, n, cnt, k, j, i, cyl;
    int ext, r;

    errors = 0;
    fmt_all = 0;
    insert("Quelldiskette", src);
    if (src != dst)
        insert("Zieldiskette", dst);
    if (DosQFSInfo(src, &ffi) || !(spt = spt_of((unsigned)ffi.sectors))) {
        puts("Die Quelldiskette ist nicht lesbar oder hat ein unbekanntes Format.\r\n");
        return 3;
    }
    total = (unsigned)ffi.sectors;
    while (nseg < MAXSEG && nseg * SECSEG < total) {   /* Speicher        */
        seg[nseg] = DosAllocSeg(0);
        if (FAPI_FAILED(seg[nseg]))
            break;
        nseg++;
    }
    cyl = spt * heads;
    n = (nseg * SECSEG > total ? total : nseg * SECSEG) / cyl * cyl;
    if (!n) {
        puts("Zu wenig Arbeitsspeicher.\r\n");
        return 3;
    }
    if (open_drv(src))
        return 3;
    if (rw(0, 1, trk, 0)) {
        close_drv();
        puts("Die Quelldiskette kann nicht gelesen werden.\r\n");
        return 3;
    }
    ext = trk[38] == 0x29 && trk[11] == 0 && trk[12] == 2;  /* Seriennr. */
    puts("Kopieren von ");
    put_num(total / cyl, 0, 0);
    puts(" Spuren, ");
    put_num(spt, 0, 0);
    puts(" Sektoren pro Spur, ");
    put_num(heads, 0, 0);
    puts(" Seite(n)\r\n\r\n");

    for (first = 0; first < total; first += cnt) {
        cnt = n > total - first ? total - first : n;  /* Durchgang       */
        if (first && src == dst) {
            insert("Quelldiskette", src);
            if (open_drv(src))
                return 3;
        } else if (first && open_drv(src))
            return 3;
        puts("Lesen von der Quelldiskette . . .\r\n");
        for (s = 0; s < cnt; s += spt) {
            if (rw(first + s, spt, trk, 0)) {
                for (k = 0; k < spt; k++) { /* sektorweise, Rest leer    */
                    if (rw(first + s + k, 1, trk + (k << 9), 0)) {
                        for (j = 0; j < 512; j++)
                            trk[(k << 9) + j] = 0;
                        puts("Lesefehler auf Laufwerk ");
                        drive_txt(src);
                        where(first + s + k);
                        errors++;
                    }
                }
            }
            if (first + s == 0 && ext) {    /* neue Seriennummer          */
                serial = new_serial();
                trk[39] = (unsigned char)serial;
                trk[40] = (unsigned char)(serial >> 8);
                trk[41] = (unsigned char)(serial >> 16);
                trk[42] = (unsigned char)(serial >> 24);
            }
            copy_mem(s, spt, trk, 1);
        }
        close_drv();
        if (src == dst)
            insert("Zieldiskette", dst);
        if (first == 0) {                   /* Ziel anders formatiert?    */
            if (DosQFSInfo(dst, &ffi) || ffi.sectors != total)
                fmt_all = 1;
            if (fmt_all)
                puts("Formatieren w\x84hrend des Kopierens\r\n");
        }
        if (open_drv(dst))
            return 3;
        puts("Schreiben auf die Zieldiskette . . .\r\n");
        for (s = 0; s < cnt; s += spt) {
            copy_mem(s, spt, trk, 0);
            r = write_track(first + s, spt);
            if (r == 3) {
                close_drv();
                return 3;
            }
            if (verify && r == 0) {
                if (rw(first + s, spt, chk, 0))
                    i = 1;
                else
                    for (i = 0, j = 0; j < (spt << 9); j++)
                        if (chk[j] != trk[j]) {
                            i = 1;
                            break;
                        }
                if (i) {
                    puts("\x9A" "berpr\x81" "fung fehlgeschlagen auf Laufwerk ");
                    drive_txt(dst);
                    where(first + s);
                    errors++;
                }
            }
        }
        close_drv();
    }
    if (serial) {
        puts("\r\nDatentr\x84ger-Seriennummer: ");
        hex8(serial);
        puts("\r\n");
    }
    if (errors)
        puts("\r\nDie Zieldiskette ist m\x94glicherweise unbrauchbar.\r\n");
    return errors ? 1 : 0;
}

static int get_drive(char **pp, unsigned *d)
{
    char *p = skip(*pp);
    if (!p[0] || p[0] == '/')
        return 0;
    if (p[1] != ':' || up(p[0]) < 'A' || up(p[0]) > 'Z')
        return -1;
    *d = (unsigned)(up(p[0]) - 'A');
    *pp = p + 2;
    return 1;
}

static int floppy(unsigned d)
{
    return DosQFSInfo(d, &ffi) == 0 && (ffi.flags & FSI_REMOVABLE) && ffi.bsize == 512;
}

void main(void)
{
    char *a, *p;
    int r, rc = 0;

    DosGetArgs(args);
    for (p = args; *p; p++)
        if (p[0] == '/') {
            if (p[1] == '?') {
                puts("Kopiert den Inhalt einer Diskette auf eine andere Diskette.\r\n\r\n"
                     "DISKCOPY [Laufwerk1: [Laufwerk2:]] [/V]\r\n\r\n"
                     "  /V   \x9A" "berpr\x81" "ft, ob die Daten richtig kopiert wurden.\r\n\r\n"
                     "Laufwerk1 und Laufwerk2 k\x94nnen gleich sein. Ist die Zieldiskette\r\n"
                     "nicht oder anders formatiert, wird sie beim Kopieren formatiert.\r\n");
                return;
            }
            if (up(p[1]) == 'V')
                verify = 1;
            *p = ' ';
            if (p[1])
                p[1] = ' ';
        }
    a = args;
    src = dst = DosQCurDisk();
    r = get_drive(&a, &src);
    if (r > 0) {
        dst = src;
        r = get_drive(&a, &dst);
    }
    if (r < 0 || *skip(a)) {
        puts("Ung\x81ltige Laufwerksangabe.\r\n");
        DosExit(4);
    }
    if (!floppy(src) || (dst != src && !floppy(dst))) {
        puts("Ung\x81ltige Laufwerksangabe.\r\nDISKCOPY kopiert nur Disketten.\r\n");
        DosExit(4);
    }
    for (;;) {
        r = copy_once();
        if (r > rc)
            rc = r;
        puts("\r\nM\x94" "chten Sie die Diskette noch einmal kopieren (J/N)? ");
        if (!ask_yn())
            break;
        puts("\r\n");
    }
    puts("\r\n");
    DosExit(rc);
}
