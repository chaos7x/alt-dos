/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DISKCOMP.EXE - zwei Disketten Spur fuer Spur vergleichen (wie MS-DOS):
 *
 *   DISKCOMP [Laufwerk1: [Laufwerk2:]]
 *
 * Wie DISKCOPY: die erste Diskette kommt in so viele 64-KB-Segmente, wie
 * Speicher da ist (sonst mehrere Durchgaenge), dann wird die zweite
 * gelesen und verglichen; mit einem Laufwerk wird gewechselt. Die
 * Seriennummer im Bootsektor zaehlt nicht mit (DISKCOPY vergibt eine neue).
 * Rueckgabe wie MS-DOS: 0 gleich, 1 verschieden, 2 Abbruch, 3 schwerer
 * Fehler, 4 Speicher/Aufruf.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

#define MAXSPT  36
#define SECSEG  128
#define MAXSEG  45

static char args[FAPI_ARGLEN];
static unsigned char trk[MAXSPT * 512];
static unsigned seg[MAXSEG], nseg;
static struct fapi_fsinfo ffi;
static unsigned d1, d2, hd, spt, heads = 2, total, diffs;

static void drive_txt(unsigned d)
{
    putc((char)('A' + d));
    putc(':');
}

static void insert(const char *what, unsigned d)
{
    puts(what);
    puts(" in Laufwerk ");
    drive_txt(d);
    puts(" einlegen\r\n");
    puts("Dr\x81" "cken Sie eine beliebige Taste, um fortzusetzen . . .");
    KbdCharIn();
    puts("\r\n\r\n");
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

static int rd(unsigned s, unsigned n)
{
    static unsigned long pos;
    pos = (unsigned long)s << 9;
    if (DosSeek(hd, &pos))
        return -1;
    return DosRead(hd, trk, n << 9) == (n << 9) ? 0 : -1;
}

static unsigned spt_of(unsigned n)
{
    return n == 720 || n == 1440 ? 9 : n == 2400 ? 15 : n == 2880 ? 18 : n == 5760 ? 36 : 0;
}

static unsigned char __far *mem_sec(unsigned i)
{
    return (unsigned char __far *)MK_FP(seg[i / SECSEG], (i % SECSEG) << 9);
}

static void where(unsigned s)
{
    unsigned t = s / spt;
    puts("Vergleichsfehler auf Seite ");
    put_num(t % heads, 0, 0);
    puts(", Spur ");
    put_num(t / heads, 0, 0);
    puts("\r\n");
}

/* Format der eingelegten Diskette in d; -> Sektoren oder 0 */
static unsigned fmt_of(unsigned d)
{
    if (DosQFSInfo(d, &ffi) || (ffi.flags & FSI_NOMEDIA))
        return 0;
    return spt_of((unsigned)ffi.sectors) ? (unsigned)ffi.sectors : 0;
}

/* ein Vergleich; -> 0 gleich, 1 verschieden, 3 schwer */
static int comp_once(void)
{
    unsigned first, s, n, cnt, k, j, i, t2;
    unsigned char __far *p;
    int same_drv = d1 == d2;

    diffs = 0;
    insert("ERSTE Diskette", d1);
    if (!same_drv)                          /* zwei Laufwerke: gleich beide */
        insert("ZWEITE Diskette", d2);
    if (!(total = fmt_of(d1))) {
        puts("Die erste Diskette ist nicht lesbar oder hat ein unbekanntes Format.\r\n");
        return 3;
    }
    spt = spt_of(total);
    puts("Vergleiche ");
    put_num(total / spt / heads, 0, 0);
    puts(" Spuren, ");
    put_num(spt, 0, 0);
    puts(" Sektoren pro Spur, 2 Seite(n)\r\n\r\n");
    cnt = nseg * SECSEG;
    cnt -= cnt % (spt * heads);             /* ganze Zylinder            */
    for (first = 0; first < total; first += cnt) {
        n = total - first < cnt ? total - first : cnt;
        if (first && same_drv)
            insert("ERSTE Diskette", d1);
        if (open_drv(d1))
            return 3;
        for (s = 0; s < n; s += spt) {
            if (rd(first + s, spt)) {
                puts("Lesefehler auf der ersten Diskette:\r\n");
                where(first + s);
                close_drv();
                return 3;
            }
            for (k = 0; k < spt; k++) {
                p = mem_sec(s + k);
                for (j = 0; j < 512; j++)
                    p[j] = trk[(k << 9) + j];
            }
        }
        close_drv();
        if (same_drv)
            insert("ZWEITE Diskette", d2);
        if (first == 0) {
            t2 = fmt_of(d2);
            if (t2 != total) {
                puts("Laufwerks- oder Diskettentypen sind nicht kompatibel.\r\n");
                return 3;
            }
        }
        if (open_drv(d2))
            return 3;
        for (s = 0; s < n; s += spt) {
            int bad = 0;
            if (rd(first + s, spt)) {
                puts("Lesefehler auf der zweiten Diskette:\r\n");
                where(first + s);
                diffs++;
                continue;
            }
            for (k = 0; k < spt && !bad; k++) {
                p = mem_sec(s + k);
                for (j = 0; j < 512; j++) {
                    i = (k << 9) + j;
                    if (first + s + k == 0 && j >= 0x27 && j < 0x2B &&
                        trk[0x26] == 0x29 && p[0x26] == 0x29)
                        continue;           /* Seriennummer               */
                    if (p[j] != trk[i]) {
                        bad = 1;
                        break;
                    }
                }
            }
            if (bad) {
                where(first + s);
                diffs++;
            }
        }
        close_drv();
    }
    if (!diffs)
        puts("Vergleich OK\r\n");
    return diffs ? 1 : 0;
}

void main(void)
{
    char *a;
    int rc = 0, r;
    unsigned u;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Vergleicht den Inhalt zweier Disketten.\r\n\r\n"
             "DISKCOMP [Laufwerk1: [Laufwerk2:]]\r\n");
        return;
    }
    d1 = d2 = 0;
    if (a[0] && a[1] == ':') {
        d1 = d2 = (unsigned)(up(a[0]) - 'A');
        a = skip(a + 2);
        if (a[0] && a[1] == ':')
            d2 = (unsigned)(up(a[0]) - 'A');
    }
    if (d1 > 1 || d2 > 1) {
        puts("Ung\x81ltige Laufwerksangabe - nur Diskettenlaufwerke.\r\n");
        DosExit(4);
    }
    for (nseg = 0; nseg < MAXSEG; nseg++) {     /* so viel Speicher wie da */
        u = DosAllocSeg(0);
        if (FAPI_FAILED(u) || !u)
            break;
        seg[nseg] = u;
    }
    if (nseg < 1) {
        puts("Nicht gen\x81gend Arbeitsspeicher.\r\n");
        DosExit(4);
    }
    for (;;) {
        r = comp_once();
        if (r > rc)
            rc = r;
        puts("\r\nWeitere Disketten vergleichen (J/N)? ");
        if (!ask_yn())
            break;
        puts("\r\n");
    }
    DosExit(rc);
}
