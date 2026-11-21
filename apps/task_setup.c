/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SETUP.EXE - AT-DOS von den Setup-Disketten auf die Festplatte
 * einrichten (Ablauf wie das Setup von MS-DOS 6, eigener Entwurf):
 *
 *   SETUP [Quelle:] [/START]
 *
 * Diskette 1 ist startfaehig (System, Werkzeuge, Treiber - zugleich
 * Notfalldiskette), Diskette 2 und folgende halten den Rest (DOS-Box,
 * Packprogramme, Zeichensaetze, Demos, COMMAND.COM 4.0). Alle tragen im
 * Stamm SETUP.ID: "ATDOS n kb1 kb2 ..." (Nummer der Diskette, belegte KB
 * aller Disketten - build.sh; so viele Groessen, so viele Disketten).
 *
 * Ablauf: Ziellaufwerk suchen; fehlt eine Partition -> FDISK (startet
 * selbst neu, die AUTOEXEC.CMD der Diskette ruft SETUP /START wieder auf);
 * nicht formatiert -> FORMAT /Q; dann CONFIG.SYS (DEVICE=-Zeilen von
 * Treibern, die beim Start kein Geraet fanden, als REM) und AUTOEXEC.CMD
 * (ohne den SETUP-Aufruf) schreiben, SYS, Diskette 1 kopieren, die
 * weiteren anfordern und kopieren, Neustart. Ist AT-DOS schon da, bleiben
 * CONFIG.SYS und AUTOEXEC.CMD; die neuen Fassungen heissen .NEW.
 * ERRORLEVEL: 0 fertig (Neustart), 1 Fehler, 3 abgebrochen.
 */
#include "fapi.h"
#include "util.h"

#define A_BG    0x1F                    /* weiss auf blau               */
#define A_HI    0x1E                    /* gelb auf blau                */
#define A_BAR   0x70                    /* Statuszeile                  */
#define K_F3    0x3D00

static char args[FAPI_ARGLEN], path[FAPI_PATHLEN], to[FAPI_PATHLEN], cs[FAPI_PATHLEN];
static char ln[81], nb[12], idb[64];
static unsigned char buf[8704];
static struct fapi_fsinfo fi;
static struct fapi_finfo ff;
static struct fapi_drvinfo dv;
static unsigned rows = 25, src = 0xFFFF, dst, update, start_mode;
#define NDISK 8
static unsigned long kbs[NDISK], kb_all, done_b, total_b;
static unsigned ndisk;                  /* Disketten laut SETUP.ID       */
static int quit_rc = 3;

/* ---- Bildschirm ---- */
static void fill(int r, unsigned char a)
{
    unsigned i;
    for (i = 0; i < 80; i++)
        ln[i] = ' ';
    ln[80] = 0;
    VioWrtStrAtt(r, 0, ln, a);
}

static void at(int r, int c, const char *t)
{
    VioWrtStrAtt(r, c, t, A_BG);
}

static void bar(const char *t)          /* Tastenleiste unten           */
{
    fill((int)rows - 1, A_BAR);
    VioWrtStrAtt((int)rows - 1, 1, t, A_BAR);
}

static void screen(const char *keys)
{
    int r;
    for (r = 0; r < (int)rows; r++)
        fill(r, A_BG);
    VioWrtStrAtt(1, 1, "AT-DOS 6 Setup", A_BG);
    for (r = 0; r < 78; r++)
        ln[r] = (char)0xCD;
    ln[78] = 0;
    VioWrtStrAtt(2, 1, ln, A_BG);
    bar(keys);
    VioSetCurPos(0xFF, 0);
}

static char *ns(unsigned long v)        /* ohne 32-Bit-Division         */
{
    static const unsigned long p10[10] = { 1000000000UL, 100000000UL, 10000000UL,
        1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    unsigned i, n = 0;
    char c;
    for (i = 0; i < 10; i++) {
        for (c = '0'; v >= p10[i]; v -= p10[i])
            c++;
        if (c != '0' || n || i == 9)
            nb[n++] = c;
    }
    nb[n] = 0;
    return nb;
}

static int pre(const char *s, const char *p)    /* beginnt s mit p? */
{
    while (*p)
        if (up(*s++) != up(*p++))
            return 0;
    return 1;
}

/* a/b in Hundertsteln-Stufen: Anteil von a an b, skaliert auf m (16 Bit) */
static unsigned part(unsigned long a, unsigned long b, unsigned m)
{
    if (a > b)
        a = b;
    while (b > 600) {
        a >>= 1;
        b >>= 1;
    }
    return b ? (unsigned)a * m / (unsigned)b : 0;
}

/* Taste: EINGABE -> 13, Esc -> 27, F3 -> K_F3, sonst Zeichen gross */
static unsigned key(void)
{
    unsigned k = KbdCharIn();
    if (KEY_EXT(k & 0xFF, k >> 8))
        return k & 0xFF00;
    return (unsigned char)up((char)k);
}

static void leave(void)
{
    VioSetMode(0);
    DosExit(quit_rc);
}

/* Warten auf EINGABE; F3 beendet Setup */
static void wait_enter(void)
{
    unsigned k;
    while ((k = key()) != 13)
        if (k == K_F3)
            leave();
}

static void msg(const char *t)          /* einzelne Meldung + EINGABE   */
{
    screen("EINGABE=Weiter  F3=Setup beenden");
    at(5, 4, t);
    wait_enter();
}

/* ---- Laufwerke ---- */
static void mk(char *p, unsigned d, const char *rest)
{
    p[0] = (char)('A' + d);
    p[1] = ':';
    p[2] = '\\';
    p[3] = 0;
    scat(p, rest, FAPI_PATHLEN);
}

static int exists(const char *p)
{
    unsigned h = DosOpen(p);
    if (FAPI_FAILED(h))
        return 0;
    DosClose(h);
    return 1;
}

static unsigned long num(char **pp)
{
    char *p = skip(*pp);
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9')
        v = (v << 3) + (v << 1) + (unsigned)(*p++ - '0');
    *pp = p;
    return v;
}

/* SETUP.ID der Diskette in src lesen -> Nummer der Diskette, 0 = keine */
static unsigned disk_id(void)
{
    unsigned h, n;
    char *p;
    mk(path, src, "SETUP.ID");
    h = DosOpen(path);
    if (FAPI_FAILED(h))
        return 0;
    n = DosRead(h, idb, sizeof(idb) - 1);
    DosClose(h);
    if (FAPI_FAILED(n) || n < 7)
        return 0;
    idb[n] = 0;
    if (!pre(idb, "ATDOS "))
        return 0;
    p = idb + 6;
    n = (unsigned)num(&p);
    ndisk = 0;
    kb_all = 0;
    while (ndisk < NDISK) {
        kbs[ndisk] = num(&p);
        if (!kbs[ndisk])
            break;
        kb_all += kbs[ndisk++];
    }
    return n;
}

static int is_target(unsigned d)
{
    return d != src && DosQFSInfo(d, &fi) == 0 && !(fi.flags & FSI_REMOVABLE) &&
           fi.bsize == 512;
}

/* Programm von der Diskette (\DOS\name) mit Argumenten im Textbetrieb */
static unsigned run(const char *name, const char *a)
{
    static char cmd[FAPI_ARGLEN];
    mk(cmd, src, "DOS\\");
    scat(cmd, name, sizeof(cmd));
    scat(cmd, " ", sizeof(cmd));
    scat(cmd, a, sizeof(cmd));
    VioSetMode(0);
    DosExecPgm(cmd, EXEC_SYNC);
    VioSetMode(1);
    return DosQChildRC();
}

/* Ziellaufwerk finden, bei Bedarf FDISK/FORMAT -> dst */
static void find_target(void)
{
    static char a[20];
    unsigned d, n;
    for (;;) {
        for (d = 2; d < 26 && !is_target(d); d++)
            ;
        if (d == 26) {
            static struct fapi_diskinfo di;
            if (DosDiskInfo(0, &di)) {
                screen("F3=Setup beenden");
                at(5, 4, "Setup hat keine Festplatte gefunden. Pr\x81" "fen Sie die Verkabelung");
                at(6, 4, "und die Treiber in CONFIG.SYS (IDE.SYS, SCSI-Treiber).");
                while (key() != K_F3)
                    ;
                leave();
            }
            screen("EINGABE=FDISK starten  F3=Setup beenden");
            at(5, 4, "Auf der Festplatte ist noch kein Laufwerk f\x81r AT-DOS eingerichtet.");
            at(7, 4, "Setup startet jetzt FDISK. Legen Sie dort eine prim\x84re DOS-Partition");
            at(8, 4, "an (Men\x81punkt 1). Danach startet der Computer neu; lassen Sie die");
            at(9, 4, "Setup-Diskette 1 im Laufwerk, Setup macht dann weiter.");
            wait_enter();
            run("FDISK.EXE", "");
            continue;
        }
        dst = d;
        for (n = 0, d = 2; d < 26; d++)
            n += is_target(d);
        is_target(dst);
        if (n > 1) {                    /* mehrere: waehlen lassen      */
            screen("EINGABE=Weiter  F3=Setup beenden");
            at(5, 4, "Auf welchem Laufwerk soll AT-DOS eingerichtet werden?");
            at(7, 4, "Laufwerk (Buchstabe):");
            for (;;) {
                a[0] = (char)('A' + dst);
                a[1] = ':';
                a[2] = 0;
                VioWrtStrAtt(7, 26, a, A_HI);
                VioSetCurPos(7, 26);
                d = key();
                if (d == 13)
                    break;
                if (d == K_F3)
                    leave();
                if (d >= 'C' && d <= 'Z' && is_target(d - 'A'))
                    dst = d - 'A';
            }
            VioSetCurPos(0xFF, 0);
            is_target(dst);
        }
        if (same(fi.fsname, "RAW")) {
            screen("EINGABE=Formatieren  F3=Setup beenden");
            a[0] = (char)('A' + dst);
            a[1] = ':';
            a[2] = 0;
            at(5, 4, "Laufwerk");
            at(5, 13, a);
            at(5, 16, "ist nicht formatiert. Setup formatiert es jetzt");
            at(6, 4, "(Schnellformatierung; ab 512 MB mit FAT32, wenn FDISK es so");
            at(7, 4, "angelegt hat). Die Abfrage danach mit J beantworten.");
            wait_enter();
            scat(a, " /Q /V:AT-DOS", sizeof(a));
            run("FORMAT.EXE", a);
            continue;
        }
        if (!same(fi.fsname, "FAT") && !same(fi.fsname, "FAT32")) {
            msg("Das Ziellaufwerk hat kein FAT-Dateisystem - AT-DOS braucht FAT oder FAT32.");
            leave();
        }
        return;
    }
}

/* ---- CONFIG.SYS / AUTOEXEC.CMD ---- */
static int drv_loaded(const char *name)
{
    unsigned i;
    for (i = 0; DosDrvInfo(i, &dv) == 0; i++)
        if (dv.kind && same(dv.name, name))
            return 1;
    return 0;
}

/* Datei name der Diskette nach X:\name (bei Aktualisierung X:\neu),
   Zeilen gefiltert: cfg -> DEVICE=/IFS=-Zeilen nicht geladener Treiber
   als REM; sonst faellt die Zeile "SETUP /START" weg. 0 = ok */
static int write_cfg(const char *name, const char *neu, int cfg)
{
    static char nm[16];
    unsigned hi, ho, n, i, s0, j, k;
    int r = 0;
    mk(to, dst, name);
    if (update && exists(to))
        mk(to, dst, neu);
    mk(path, src, name);
    hi = DosOpen(path);
    if (FAPI_FAILED(hi))
        return -1;
    n = DosRead(hi, buf, sizeof(buf) - 1);  /* CONFIG.SYS bis 8 KB      */
    DosClose(hi);
    if (FAPI_FAILED(n))
        return -1;
    ho = DosCreate(to);
    if (FAPI_FAILED(ho))
        return -1;
    for (s0 = 0; s0 < n; s0 = i) {
        char *p;
        for (i = s0; i < n && buf[i] != '\n'; i++)
            ;
        if (i < n)
            i++;                        /* mit LF                       */
        p = (char *)buf + s0;
        while (p < (char *)buf + i && *p == ' ')
            p++;
        k = 0;
        if (cfg && (pre(p, "DEVICE=") || pre(p, "IFS="))) {
            while (*p != '=')
                p++;
            for (p++; *p == ' '; p++)
                ;
            for (j = 0; j < sizeof(nm) - 1 && (unsigned char)p[j] > ' '; j++)
                nm[j] = p[j];
            nm[j] = 0;
            if (!drv_loaded(base_name(nm)) && DosWrite(ho, "REM ", 4) != 4)
                r = -1;
        } else if (!cfg && pre(p, "SETUP /START"))
            k = 1;                      /* Aufruf der Setup-Diskette weg */
        if (!k && DosWrite(ho, buf + s0, i - s0) != i - s0)
            r = -1;
    }
    DosClose(ho);
    return r;
}

/* ---- Kopieren ---- */
static void progress(const char *file)
{
    unsigned i, w;
    fill(12, A_BG);
    at(12, 4, "Kopiere ");
    at(12, 12, file);
    w = part(done_b, total_b, 60);
    for (i = 0; i < 60; i++)
        ln[i] = (char)(i < w ? 0xDB : 0xB0);
    ln[60] = 0;
    VioWrtStrAtt(15, 10, ln, A_BG);
    ns(part(done_b, total_b, 100));
    scat(nb, " %  ", sizeof(nb));
    at(15, 72, nb);
}

/* root: 1 = Dateien im Stamm auslassen (die schreibt SYS/write_cfg),
   skip_sys: \DOS\SYSTEM hat SYS schon kopiert */
static unsigned skip_root, skip_sys;

static int visit(char *p, struct fapi_dirent *e, int what, int depth)
{
    const char *rel = p + 3;            /* hinter "A:\"                 */
    unsigned k;
    if (what == WT_LEAVE)
        return 0;
    to[0] = (char)('A' + dst);
    to[1] = ':';
    to[2] = '\\';
    to[3] = 0;
    scat(to, rel, sizeof(to));
    if (what == WT_DIR) {
        if (!is_dir(to))
            DosMkDir(to);
        return WT_DESCEND;
    }
    if (depth == 0 && skip_root)
        return 0;
    if (depth == 0 && same(e->name, "SETUP.ID"))
        return 0;
    if (skip_sys && depth == 2 && pre(rel, "DOS\\SYSTEM\\"))
        return 0;
    progress(rel);
    for (;;) {
        ff.mask = 1;                    /* altes schreibgeschuetztes Ziel */
        ff.attr = 0;
        DosSetFInfo(to, &ff);
        if (copy_file(p, to) == 0)
            break;
        screen("EINGABE=Wiederholen  ESC=\x9A" "berspringen  F3=Setup beenden");
        at(5, 4, "Fehler beim Kopieren von");
        at(6, 6, p);
        at(8, 4, "Ist die Diskette eingelegt und das Ziel nicht voll?");
        k = key();
        while (k != 13 && k != 27 && k != K_F3)
            k = key();
        if (k == K_F3)
            leave();
        screen("F3=Setup beenden");
        if (k == 27)
            break;
    }
    ff.mask = 3;
    ff.attr = e->attr & 0x06;
    ff.date = e->date;
    ff.time = e->time;
    DosSetFInfo(to, &ff);
    done_b += e->size;
    return 0;
}

static void copy_disk(unsigned n)       /* Diskette n (1: Stamm und      */
{                                       /* \DOS\SYSTEM hat SYS schon)    */
    static char t[3];
    skip_root = skip_sys = n == 1;
    mk(path, src, "");
    screen("F3=Setup beenden");
    at(5, 4, "Setup kopiert die Dateien von Diskette");
    t[0] = (char)('0' + n);
    t[1] = '.';
    t[2] = 0;
    at(5, 43, t);
    walk_tree(path, visit);
}

static void insert_disk(unsigned n)
{
    static char t[4];
    t[0] = (char)('0' + n);
    t[1] = 0;
    for (;;) {
        screen("EINGABE=Weiter  F3=Setup beenden");
        at(5, 4, "Legen Sie die Diskette");
        at(7, 8, "AT-DOS 6 Setup-Diskette");
        at(7, 32, t);
        at(9, 4, "in Laufwerk");
        mk(path, src, "");
        path[2] = 0;
        at(9, 16, path);
        at(9, 19, "ein und dr\x81" "cken Sie die EINGABETASTE.");
        wait_enter();
        if (disk_id() == n)
            return;
        quit_rc = 1;
    }
}

void main(void)
{
    char *a;
    unsigned k, nd;
    unsigned long fr;
    static unsigned long fb;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(next_word(a))) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Richtet AT-DOS von den Setup-Disketten auf der Festplatte ein.\r\n\r\n"
                 "SETUP [Quelle:]\r\n\r\n"
                 "  Quelle  Laufwerk mit der Setup-Diskette 1 (Vorgabe: Startlaufwerk)\r\n");
            return;
        }
        if (a[0] == '/' && pre(a, "/START"))
            start_mode = 1;
        else if (a[1] == ':')
            src = (unsigned)(up(a[0]) - 'A');
    }
    if (src == 0xFFFF) {
        src = 0;
        if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
            src = (unsigned)(up(cs[0]) - 'A');
    }
    if (disk_id() != 1) {
        puts("SETUP l\x84uft nur von der AT-DOS-Setup-Diskette 1 (SETUP.ID fehlt).\r\n");
        DosExit(1);
    }
    k = VioSetMode(1);
    if (!FAPI_FAILED(k) && k >= 25)
        rows = k;

    screen(start_mode ? "EINGABE=Weiter  F3=Zur Eingabeaufforderung"
                      : "EINGABE=Weiter  F3=Setup beenden");
    at(4, 4, "Willkommen bei AT-DOS!");
    at(6, 4, "Setup richtet AT-DOS 6 auf der Festplatte Ihres Computers ein. Ist");
    at(7, 4, "schon ein AT-DOS darauf, wird es aktualisiert; CONFIG.SYS und");
    at(8, 4, "AUTOEXEC.CMD bleiben dann erhalten.");
    at(10, 4, "Sie brauchen alle   Setup-Disketten. Diskette 1 ist zugleich eine");
    {
        static char nd[2];
        nd[0] = (char)('0' + ndisk);    /* Zahl aus SETUP.ID              */
        at(10, 22, nd);
    }
    at(11, 4, "Notfalldiskette: mit F3 kommen Sie zur Eingabeaufforderung (FDISK,");
    at(12, 4, "FORMAT, CHKDSK, EDIT, ...).");
    at(14, 4, "Weiter mit der EINGABETASTE.");
    wait_enter();

    find_target();
    mk(path, dst, "ATDOS.SYS");
    update = exists(path);
    fb = 0;
    DosFreeSpace(dst, &fb);
    fr = fb >> 10;
    if (!update && fr < kb_all + 64) {
        screen("F3=Setup beenden");
        at(5, 4, "Auf dem Ziellaufwerk ist nicht genug Platz frei.");
        at(7, 4, "Ben\x94tigt (KB):");
        at(7, 20, ns(kb_all + 64));
        at(8, 4, "Frei (KB):");
        at(8, 20, ns(fr));
        while (key() != K_F3)
            ;
        leave();
    }
    screen("EINGABE=Einrichten  F3=Setup beenden");
    at(5, 4, "Setup ist bereit:");
    mk(path, dst, "");
    path[2] = 0;
    at(7, 6, "Ziellaufwerk:");
    at(7, 21, path);
    at(7, 24, "(");
    at(7, 25, fi.fsname);
    at(7, 25 + slen(fi.fsname), ",");
    at(7, 27 + slen(fi.fsname), ns(fr >> 10));
    at(7, 27 + slen(fi.fsname) + slen(nb), " MB frei)");
    at(8, 6, update ? "Vorhandenes AT-DOS wird aktualisiert (CONFIG.SYS und AUTOEXEC.CMD"
                    : "AT-DOS wird neu eingerichtet.");
    if (update)
        at(9, 6, "bleiben, neue Fassungen als CONFIG.NEW und AUTOEXEC.NEW).");
    at(11, 4, "In CONFIG.SYS stehen nur die Treiber aktiv, die beim Start dieser");
    at(12, 4, "Diskette ein Ger\x84t gefunden haben; die \x81" "brigen als REM-Zeile.");
    wait_enter();

    quit_rc = 1;
    screen("");
    at(5, 4, "Setup schreibt CONFIG.SYS und AUTOEXEC.CMD und \x81" "bertr\x84gt das System ...");
    if (write_cfg("CONFIG.SYS", "CONFIG.NEW", 1) || write_cfg("AUTOEXEC.CMD", "AUTOEXEC.NEW", 0)) {
        msg("CONFIG.SYS/AUTOEXEC.CMD konnten nicht geschrieben werden.");
        leave();
    }
    {
        static char sa[8];
        sa[0] = (char)('A' + src);
        sa[1] = ':';
        sa[2] = ' ';
        sa[3] = (char)('A' + dst);
        sa[4] = ':';
        sa[5] = 0;
        if (run("SYS.EXE", sa)) {
            msg("SYS ist fehlgeschlagen - das Ziellaufwerk ist nicht startf\x84hig.");
            leave();
        }
    }
    total_b = kb_all << 10;
    done_b = 0;
    copy_disk(1);
    for (k = 2, nd = ndisk; k <= nd; k++) {   /* disk_id setzt ndisk neu */
        insert_disk(k);
        copy_disk(k);
    }

    quit_rc = 0;
    screen("EINGABE=Neu starten  F3=Zur Eingabeaufforderung");
    at(5, 4, "AT-DOS ist eingerichtet.");
    at(7, 4, "Entfernen Sie die Diskette aus dem Laufwerk und dr\x81" "cken Sie die");
    at(8, 4, "EINGABETASTE, um den Computer von der Festplatte neu zu starten.");
    wait_enter();
    VioSetMode(0);
    DosShutdown(1);
}
