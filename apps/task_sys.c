/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SYS.EXE - AT-DOS auf einen Datentraeger uebertragen (wie MS-DOS SYS):
 *
 *   SYS [Quelle:] Ziel: [/A]
 *
 * Kopiert ATDOS.SYS (versteckt, System, schreibgeschuetzt) und CMD.EXE,
 * dazu \DOS\SYSTEM (die Treiber braucht der Start von Platte), CHKDSK.EXE
 * (Pruefung beim Start) und - wenn
 * auf dem Ziel noch nicht vorhanden - CONFIG.SYS und AUTOEXEC.CMD. Danach
 * bekommt der Bootsektor den AT-DOS-Startcode, der BPB bleibt.
 * Disketten: ATDOS.SYS muss als erste Datei zusammenhaengend am Anfang
 * liegen - deshalb nur auf eine leere Diskette (FORMAT A: /S).
 * /A kopiert zusaetzlich alle Programme aus \DOS (vollstaendige Installation).
 * Quelle ist das Startlaufwerk. ERRORLEVEL 0 ok, 1 Fehler.
 */
#include "fapi.h"
#include "util.h"
#define BOOT32
#include "../build/bootcode.h"

static char args[FAPI_ARGLEN], from[FAPI_PATHLEN], to[FAPI_PATHLEN];
static char sdir[FAPI_PATHLEN], ddir[FAPI_PATHLEN], cs[FAPI_PATHLEN];
static unsigned char bs[512];
static struct fapi_fsinfo fi;
static struct fapi_finfo ff;
static unsigned src = 0xFFFF, dst = 0xFFFF, all;

static void mk(char *p, unsigned d, const char *rest)
{
    p[0] = (char)('A' + d);
    p[1] = ':';
    p[2] = '\\';
    p[3] = 0;
    scat(p, rest, FAPI_PATHLEN);
}

static int cp(const char *name, int only_new, unsigned attr)
{
    unsigned h;
    mk(from, src, name);
    mk(to, dst, name);
    if (only_new && !FAPI_FAILED(h = DosOpen(to))) {
        DosClose(h);
        return 0;                       /* vorhandene Konfiguration bleibt */
    }
    if (!FAPI_FAILED(h = DosOpen(to))) {    /* altes ATDOS.SYS: Schutz weg */
        DosClose(h);
        ff.mask = 1;
        ff.attr = 0;
        DosSetFInfo(to, &ff);
    }
    if (copy_file(from, to)) {
        if (only_new)
            return 0;                   /* Quelle hat keine: egal        */
        puts("Fehler beim Kopieren von ");
        puts(from);
        puts("\r\n");
        return -1;
    }
    if (attr) {
        ff.mask = 1;
        ff.attr = attr;
        DosSetFInfo(to, &ff);
    }
    return 0;
}

static int copy_dir(const char *rel)
{
    static struct fapi_dirent e;
    static char name[FAPI_PATHLEN];
    unsigned n;
    mk(sdir, src, rel);
    mk(ddir, dst, rel);
    if (!is_dir(ddir))
        DosMkDir(ddir);
    for (n = 0; DosFindEntry(&e, n, sdir) != FAPI_ERROR; n++) {
        if (e.attr & 0x18)
            continue;
        scopy(name, rel, sizeof(name));
        scat(name, "\\", sizeof(name));
        scat(name, e.name, sizeof(name));
        if (cp(name, 0, 0))
            return -1;
    }
    return 0;
}

void main(void)
{
    static struct fapi_dirent e;
    char *a;
    unsigned h, i, k;
    int f32;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("\x9A" "bertr\x84gt AT-DOS auf einen Datentr\x84ger.\r\n\r\n"
                 "SYS [Quelle:] Ziel: [/A]\r\n\r\n"
                 "  /A  auch alle Programme aus \\DOS kopieren\r\n");
            return;
        }
        if (a[0] == '/' && up(a[1]) == 'A') {
            all = 1;
            continue;
        }
        if (a[1] == ':') {
            if (dst != 0xFFFF)
                src = dst;
            dst = (unsigned)(up(a[0]) - 'A');
        }
    }
    if (src == 0xFFFF) {
        src = 2;
        if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
            src = (unsigned)(up(cs[0]) - 'A');
    }
    if (dst == 0xFFFF || dst == src) {
        puts("Aufruf: SYS [Quelle:] Ziel: [/A]\r\n");
        DosExit(1);
    }
    if (DosQFSInfo(dst, &fi) || (!same(fi.fsname, "FAT") && !same(fi.fsname, "FAT32"))) {
        puts("Ziel ist kein FAT-Datentr\x84ger (zuerst FORMAT).\r\n");
        DosExit(1);
    }
    h = DosOpenDrive(dst, DRV_OPEN_READ);       /* FAT12 oder FAT16?     */
    if (FAPI_FAILED(h) || DosRead(h, bs, 512) != 512) {
        puts("Bootsektor nicht lesbar.\r\n");
        DosExit(1);
    }
    DosClose(h);
    i = (bs[54 + 3] == '1' && bs[54 + 4] == '6') ||     /* "FAT16   "  */
        (bs[82 + 3] == '3' && bs[82 + 4] == '2');       /* "FAT32   "  */
    if (!(fi.flags & FSI_REMOVABLE) && !i) {
        puts("Startf\x84hig sind nur FAT16-/FAT32-Partitionen und Disketten.\r\n");
        DosExit(1);
    }
    if (fi.flags & FSI_REMOVABLE) {
        mk(ddir, dst, "");
        if (DosFindEntry(&e, 0, ddir) != FAPI_ERROR) {
            puts("Die Diskette ist nicht leer. ATDOS.SYS muss am Anfang liegen:\r\n"
                 "FORMAT ");
            putc((char)('A' + dst));
            puts(": /S benutzen.\r\n");
            DosExit(1);
        }
    }
    if (cp("ATDOS.SYS", 0, 0x07) || cp("CMD.EXE", 0, 0))
        DosExit(1);
    cp("CONFIG.SYS", 1, 0);
    cp("AUTOEXEC.CMD", 1, 0);
    mk(ddir, dst, "DOS");
    if (!is_dir(ddir))
        DosMkDir(ddir);
    if (copy_dir("DOS\\SYSTEM"))
        DosExit(1);
    if (all) {
        if (copy_dir("DOS"))
            DosExit(1);
    } else
        cp("DOS\\CHKDSK.EXE", 0, 0);  /* Pruefung beim Start braucht es */

    /* Startcode in den Bootsektor, BPB (Bytes 3..61) bleibt */
    h = DosOpenDrive(dst, DRV_OPEN_LOCK);
    if (FAPI_FAILED(h) || DosRead(h, bs, 512) != 512) {
        puts("Laufwerk kann nicht gesperrt werden - Bootsektor nicht ge\x84ndert.\r\n");
        DosExit(1);
    }
    f32 = fi.fsname[3] == '3' && fi.fsname[4] == '2';  /* "FAT32"   */
    for (i = 0; i < 512; i++)           /* FAT32: BPB bis 59h            */
        if (i < 3 || i >= (f32 ? 0x5A : 0x3E))
            bs[i] = (f32 ? boot_32 : (fi.flags & FSI_REMOVABLE) ? boot_fd : boot_hd)[i];
    {
        static unsigned long pos;
        pos = 0;
        DosSeek(h, &pos);
        if (DosWrite(h, bs, 512) != 512) {
            DosClose(h);
            puts("Bootsektor nicht schreibbar.\r\n");
            DosExit(1);
        }
        if (f32) {                      /* zweiter Teil des Startcodes   */
            pos = 2UL << 9;             /* in den reservierten Sektor 2  */
            if (DosSeek(h, &pos) || DosWrite(h, (void *)boot_32b, 512) != 512) {
                DosClose(h);
                puts("Sektor 2 nicht schreibbar.\r\n");
                DosExit(1);
            }
        }
        k = bs[50] | (bs[51] << 8);     /* FAT32: Kopie (meist Sektor 6) */
        if (f32 && k && k < (bs[14] | (bs[15] << 8))) {
            pos = (unsigned long)k << 9;
            if (DosSeek(h, &pos) || DosWrite(h, bs, 512) != 512)
                puts("Kopie des Bootsektors nicht schreibbar.\r\n");
        }
    }
    DosClose(h);
    puts("System \x81" "bertragen\r\n");
}
