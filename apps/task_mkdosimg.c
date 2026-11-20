/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MKDOSIMG.EXE - Startabbild fuer die DOS-Box aus der eigenen DOS-Kopie
 * (wie "OS/2 fuer Windows" die vorhandene Windows-Kopie nimmt; AT-DOS
 * liefert kein Microsoft-DOS mit):
 *
 *   MKDOSIMG Quelle: [Werkzeuge] [/N:NAME.IMG]
 *
 *   Quelle:     Laufwerk mit dem DOS: Startdiskette oder Festplatte mit
 *               installiertem DOS (auch ein Loop-Laufwerk) - von dort
 *               IO.SYS/MSDOS.SYS bzw. IBMBIO.COM/IBMDOS.COM, COMMAND.COM,
 *               COUNTRY.SYS (Stamm, sonst \DOS)
 *   Werkzeuge:  Verzeichnis mit den Programmen des DOS -> \DOS\MDOS\<NAME>\
 *               (vorn im PATH der Box); ohne Angabe Quelle:\DOS, wenn es
 *               dort ein DOS gibt (nicht auf einem Laufwerk mit AT-DOS)
 *   /N:         Name des Abbilds (Vorgabe MSDOS.IMG bzw. PCDOS.IMG)
 *
 * Ergebnis (5.x, Kompatibilitaetsbox): X:\DOS\MDOS\NAME.IMG (FAT12, das
 * kleinste Diskettenformat 360 KB - 1,44 MB, in das die Dateien mit
 * 32 KB Rand passen - die Box haelt es im Speicher): eigener
 * Startsektor (boot/mdosboot.asm, Ladeprotokoll von DOS 4.0-6.22 - der
 * Startsektor der Quelle wird nicht gebraucht), die Systemdateien zuerst (IO.SYS zusammenhaengend
 * ab Cluster 2, wie es der Startsektor verlangt), RBSHELL.COM und
 * RBHMA.SYS aus \DOS\MDOS, eine CONFIG.SYS fuer die Box. Gestartet wird es
 * mit RBOX NAME.IMG oder - fuer RBOX ohne Angabe - mit DOS_IMAGE=NAME.IMG
 * unter [*] in \DOS\MDOS\DOSBOX.INI. HIMEM, EMM386, DBLSPACE/DRVSPACE und
 * KEYB-Dateien kopiert MKDOSIMG nicht mit - das erledigt AT-DOS selbst.
 * MS-DOS 7 (Windows 95/98) und andere DOS lehnt es ab; DOS vor 4.0 hat
 * ein anderes Ladeprotokoll und startet mit diesem Startsektor nicht.
 * ERRORLEVEL: 0 ok, 1 Fehler, 4 Aufruf.
 */
#include "fapi.h"
#include "util.h"
#define BOOTMD
#include "../build/bootcode.h"

static char args[FAPI_ARGLEN], src[FAPI_PATHLEN], tools[FAPI_PATHLEN];
static char mdos[FAPI_PATHLEN], img[FAPI_PATHLEN], from[FAPI_PATHLEN];
static char to[FAPI_PATHLEN], tdir[FAPI_PATHLEN], iname[16];
static unsigned char bs[512], sec[512];
static struct fapi_dirent e;
static struct fapi_finfo ff;
static unsigned sdrv, ldrv;
static int pcdos;

static void die(const char *m)
{
    puts("MKDOSIMG: ");
    puts(m);
    puts("\r\n");
    if (ldrv)
        DosLoop(LOOP_DETACH, ldrv, 0);
    DosExit(1);
}

/* Datei name in dir? -> Groesse in e */
static int have(const char *dir, const char *name)
{
    unsigned n;
    for (n = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR; n++)
        if (!(e.attr & 0x18) && same(e.name, name))
            return 1;
    return 0;
}

/* d:\name -> o */
static void mk(char *o, unsigned d, const char *name)
{
    o[0] = (char)('A' + d);
    o[1] = ':';
    o[2] = '\\';
    o[3] = 0;
    scat(o, name, FAPI_PATHLEN);
}

/* dir\name -> ldrv:\name, Datum/Uhrzeit wie die Quelle, Attribut attr */
static void put1(const char *dir, const char *name, unsigned attr)
{
    join(from, dir, name, FAPI_PATHLEN);
    mk(to, ldrv, name);
    if (!have(dir, name) || copy_file(from, to)) {
        puts(from);
        puts(": ");
        die("Kopieren fehlgeschlagen");
    }
    ff.mask = 2 | (attr ? 1 : 0);
    ff.attr = attr;
    ff.date = e.date;
    ff.time = e.time;
    DosSetFInfo(to, &ff);
}

/* Diskettenformate fuer das Abbild: kleinstes, in das die Dateien passen
   (5.x haelt das ganze Abbild im erweiterten Speicher - auf 2-4-MB-
   Rechnern zaehlt jedes KB). Sektoren, Sektoren je Cluster, Stamm-
   eintraege, Medienbyte, Sektoren je FAT, je Spur, nutzbare KB */
static const struct fmt {
    unsigned secs, spc, root, media, spf, spt, kb;
} fmts[] = {
    {  720, 2, 112, 0xFD, 2,  9,  354 },
    { 1440, 2, 112, 0xF9, 3,  9,  713 },
    { 2400, 1, 224, 0xF9, 7, 15, 1185 },
    { 2880, 1, 224, 0xF0, 9, 18, 1423 }
};
#define IMG_RESERVE 32                  /* KB Rand: CONFIG.SYS, Cluster   */

/* KB, die dir\name im Abbild braucht (0, wenn es sie nicht gibt) */
static unsigned need_kb(const char *dir, const char *name)
{
    if (!have(dir, name))
        return 0;
    return (unsigned)((e.size + 1023) >> 10);
}

/* Abbild mit eigenem Startsektor (boot_md), BPB nach dem Format f */
static void make_image(const struct fmt *f)
{
    unsigned h, i, s, rs = f->root / 16;
    unsigned long sz = (unsigned long)f->secs << 9, pos;

    for (i = 0; i < 512; i++)
        bs[i] = boot_md[i];
    bs[0x0B] = 0x00; bs[0x0C] = 0x02;       /* 512 Byte je Sektor    */
    bs[0x0D] = (unsigned char)f->spc;
    bs[0x0E] = 1; bs[0x0F] = 0;             /* reservierte Sektoren  */
    bs[0x10] = 2;                           /* FATs                  */
    bs[0x11] = (unsigned char)f->root; bs[0x12] = (unsigned char)(f->root >> 8);
    bs[0x13] = (unsigned char)f->secs; bs[0x14] = (unsigned char)(f->secs >> 8);
    bs[0x15] = (unsigned char)f->media;
    bs[0x16] = (unsigned char)f->spf; bs[0x17] = 0;
    bs[0x18] = (unsigned char)f->spt; bs[0x19] = 0;
    bs[0x1A] = 2; bs[0x1B] = 0;             /* Koepfe                */
    for (i = 0x1C; i < 0x26; i++)           /* versteckt, gross, Lw. */
        bs[i] = 0;
    bs[0x26] = 0x29;
    bs[0x27] = 0x41; bs[0x28] = 0x54;       /* Seriennummer          */
    bs[0x29] = 0x44; bs[0x2A] = 0x4F;
    for (i = 0; i < 11; i++)
        bs[0x2B + i] = "MDOS       "[i];
    for (i = 0; i < 8; i++)
        bs[0x36 + i] = "FAT12   "[i];
    h = DosCreate(img);
    if (FAPI_FAILED(h))
        die("Abbild laesst sich nicht anlegen");
    if (FAPI_FAILED(DosNewSize(h, &sz)))
        die("kein Platz fuer das Abbild");
    pos = 0;
    DosSeek(h, &pos);
    DosWrite(h, bs, 512);
    for (s = 1; s < 1 + 2 * f->spf + rs; s++) {     /* FATs und Stamm */
        for (i = 0; i < 512; i++)
            sec[i] = 0;
        if (s == 1 || s == 1 + f->spf) {
            sec[0] = (unsigned char)f->media;
            sec[1] = sec[2] = 0xFF;
        }
        if (DosWrite(h, sec, 512) != 512)
            die("Schreibfehler im Abbild");
    }
    DosClose(h);
}

/* Werkzeuge: alle Dateien aus tools nach \DOS\MDOS\<NAME>, ohne das, was
   AT-DOS selbst erledigt */
static const char *const skip_names[] = {
    "HIMEM.SYS", "EMM386.EXE", "DBLSPACE.BIN", "DRVSPACE.BIN", "DBLSPACE.SYS",
    "DRVSPACE.SYS", "KEYB.COM", "KEYBOARD.SYS", "KEYBRD2.SYS", "SMARTDRV.EXE",
    "SMARTDRV.SYS", "MEMMAKER.EXE", 0
};
static void copy_tools(void)
{
    unsigned n, i, k = 0;
    DosMkDir(tdir);
    for (n = 0; DosFindEntry(&e, n, tools) != FAPI_ERROR; n++) {
        if (e.attr & 0x18)
            continue;
        for (i = 0; skip_names[i] && !same(e.name, skip_names[i]); i++)
            ;
        if (skip_names[i])
            continue;
        join(from, tools, e.name, FAPI_PATHLEN);
        join(to, tdir, e.name, FAPI_PATHLEN);
        if (copy_file(from, to)) {
            puts("  nicht kopiert: ");
            puts(from);
            puts("\r\n");
            continue;
        }
        ff.mask = 2;
        ff.date = e.date;
        ff.time = e.time;
        DosSetFInfo(to, &ff);
        k++;
    }
    puts("  ");
    put_num(k, 0, 0);
    puts(" Werkzeuge nach ");
    puts(tdir);
    puts("\r\n");
}

static void cfg_line(unsigned h, const char *s)
{
    DosWrite(h, s, slen(s));
    DosWrite(h, "\r\n", 2);
}

void main(void)
{
    char *p, w[FAPI_PATHLEN];
    unsigned h, i, lt, need;

    DosGetArgs(args);
    p = skip(args);
    while (*p) {
        p = word(p, w, sizeof(w));
        if (w[0] == '/' && up(w[1]) == 'N' && w[2] == ':')
            scopy(iname, w + 3, sizeof(iname));
        else if (w[0] == '/') {
            puts("MKDOSIMG Quelle: [Werkzeuge] [/N:NAME.IMG]\r\n"
                 "  Startabbild f\x81r die DOS-Box aus der eigenen DOS-Kopie.\r\n");
            DosExit(w[1] == '?' ? 0 : 4);
        } else if (!src[0])
            scopy(src, w, sizeof(src));
        else
            abs_path(w, tools);
        p = skip(p);
    }
    if (!src[0] || src[1] != ':' || src[2]) {
        puts("MKDOSIMG Quelle: [Werkzeuge] [/N:NAME.IMG]\r\n"
             "  Quelle ist ein Laufwerk (Startdiskette des DOS), z. B. A:\r\n");
        DosExit(4);
    }
    sdrv = (unsigned)(up(src[0]) - 'A');
    src[2] = '\\';
    src[3] = 0;

    /* welches DOS? */
    if (have(src, "IO.SYS") && have(src, "MSDOS.SYS")) {
        if (e.size < 4096)
            die("MS-DOS 7 (Windows 95/98) wird noch nicht unterst\x81tzt");
    } else if (have(src, "IBMBIO.COM") && have(src, "IBMDOS.COM"))
        pcdos = 1;
    else
        die("keine Systemdateien (IO.SYS/MSDOS.SYS oder IBMBIO.COM/IBMDOS.COM)");
    if (!have(src, "COMMAND.COM"))
        die("COMMAND.COM fehlt auf der Quelle");

    /* Werkzeuge ohne Angabe: Quelle:\DOS, wenn dort ein DOS liegt - nicht
       auf einem Laufwerk mit AT-DOS (dessen \DOS sind AT-DOS-Werkzeuge) */
    if (!tools[0] && !have(src, "ATDOS.SYS")) {
        mk(from, sdrv, "DOS");
        if (have(from, "COMMAND.COM") || have(from, "MEM.EXE"))
            scopy(tools, from, sizeof(tools));
    }

    /* Ziel: \DOS\MDOS des Startlaufwerks (aus %COMSPEC%, wie RBOX) */
    if (FAPI_FAILED(DosGetEnv("COMSPEC", mdos, sizeof(mdos))) || mdos[1] != ':')
        die("%COMSPEC% nicht gesetzt");
    mdos[2] = 0;
    scat(mdos, "\\DOS\\MDOS", sizeof(mdos)); /* "X:\DOS\MDOS"         */
    if (!iname[0])
        scopy(iname, pcdos ? "PCDOS.IMG" : "MSDOS.IMG", sizeof(iname));
    for (i = 0; iname[i]; i++)
        iname[i] = up(iname[i]);
    for (i = 0; iname[i] && iname[i] != '.'; i++)
        ;
    if (!iname[i] || i > 8 || slen(iname + i) > 4 || same(iname, "FREEDOS.IMG"))
        die("Name: NAME.IMG mit h\x94" "chstens 8 Zeichen, nicht FREEDOS.IMG");
    join(img, mdos, iname, sizeof(img));
    join(tdir, mdos, iname, sizeof(tdir));
    *(base_name(tdir) + i) = 0;             /* "X:\DOS\MDOS\MSDOS"    */
    if (!have(mdos, "RBSHELL.COM") || !have(mdos, "RBHMA.SYS"))
        die("RBSHELL.COM/RBHMA.SYS fehlen in \\DOS\\MDOS");

    /* Groesse: Summe der Dateien + Rand -> kleinstes Format */
    need = need_kb(src, pcdos ? "IBMBIO.COM" : "IO.SYS")
         + need_kb(src, pcdos ? "IBMDOS.COM" : "MSDOS.SYS")
         + need_kb(src, "COMMAND.COM")
         + (have(src, "COUNTRY.SYS") ? need_kb(src, "COUNTRY.SYS")
            : tools[0] ? need_kb(tools, "COUNTRY.SYS") : 0)
         + need_kb(mdos, "RBSHELL.COM") + need_kb(mdos, "RBHMA.SYS")
         + need_kb(mdos, "LTEMM.EXE") + need_kb(mdos, "LTEMM.TXT")
         + IMG_RESERVE;
    for (i = 0; i < 3 && fmts[i].kb < need; i++)
        ;
    if (fmts[i].kb < need)
        die("Systemdateien passen nicht in 1,44 MB");
    puts("Abbild ");
    puts(img);
    puts(" (");
    put_num(fmts[i].secs >> 1, 0, 0);
    puts(" KB) wird angelegt\r\n");
    make_image(&fmts[i]);
    h = DosLoop(LOOP_ATTACH, img, 0);
    if (FAPI_FAILED(h))
        die("Abbild l\x84\xDFt sich nicht einh\x84ngen (LOOP)");
    ldrv = h;

    /* Systemdateien zuerst und in dieser Reihenfolge (Startsektor) */
    put1(src, pcdos ? "IBMBIO.COM" : "IO.SYS", 7);
    put1(src, pcdos ? "IBMDOS.COM" : "MSDOS.SYS", 7);
    put1(src, "COMMAND.COM", 0);
    if (have(src, "COUNTRY.SYS"))
        put1(src, "COUNTRY.SYS", 0);
    else if (tools[0] && have(tools, "COUNTRY.SYS"))
        put1(tools, "COUNTRY.SYS", 0);
    put1(mdos, "RBSHELL.COM", 0);
    put1(mdos, "RBHMA.SYS", 0);
    lt = have(mdos, "LTEMM.EXE");           /* EMS der Lo-tech-Karte     */
    if (lt) {
        put1(mdos, "LTEMM.EXE", 0);
        if (have(mdos, "LTEMM.TXT"))
            put1(mdos, "LTEMM.TXT", 0);
    }

    mk(to, ldrv, "CONFIG.SYS");
    h = DosCreate(to);
    if (FAPI_FAILED(h))
        die("CONFIG.SYS im Abbild");
    cfg_line(h, "DEVICE=A:\\RBHMA.SYS");     /* wie FREEDOS.IMG der Box */
    if (lt)                                 /* nur mit Lo-tech-Karte     */
        cfg_line(h, "REM DEVICE=A:\\LTEMM.EXE /p:D000 /i:260");
    cfg_line(h, "DOS=HIGH");
    cfg_line(h, "FILES=30");
    cfg_line(h, "FCBS=16,8");
    cfg_line(h, "BUFFERS=10");
    cfg_line(h, "BREAK=OFF");
    cfg_line(h, "STACKS=9,256");
    cfg_line(h, "LASTDRIVE=Z");
    mk(from, ldrv, "COUNTRY.SYS");
    if (!FAPI_FAILED(i = DosOpen(from))) {
        DosClose(i);
        cfg_line(h, "COUNTRY=049,437,A:\\COUNTRY.SYS");
    }
    cfg_line(h, "SHELL=A:\\RBSHELL.COM");
    DosClose(h);
    DosLoop(LOOP_DETACH, ldrv, 0);
    ldrv = 0;

    DosMkDir(tdir);                         /* COMMAND.COM fuer COMSPEC: */
    join(from, src, "COMMAND.COM", FAPI_PATHLEN);   /* A: ist nach dem Start */
    join(to, tdir, "COMMAND.COM", FAPI_PATHLEN);    /* nicht mehr das Abbild */
    if (copy_file(from, to))
        die("COMMAND.COM nach \\DOS\\MDOS\\Name nicht kopiert");
    if (tools[0])
        copy_tools();
    puts("Fertig. Starten mit RBOX ");
    puts(iname);
    puts(" - oder in ");
    puts(mdos);
    puts("\\DOSBOX.INI\r\n  [*]\r\n  DOS_IMAGE=");
    puts(iname);
    puts("\r\nf\x81r RBOX ohne Angabe.\r\n");
    DosExit(0);
}
