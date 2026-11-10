/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CONVERT.EXE - Dateisystem eines Laufwerks umwandeln (wie Windows NT):
 *
 *   CONVERT Laufwerk: /FS:FAT32 [/V]
 *
 * Wie FORMAT und CHKDSK nur der Verteiler: erst CHKDSK Laufwerk: (bricht
 * bei Fehlern ab), dann U<Ziel>.EXE Laufwerk: /CONVERT aus \DOS\SYSTEM
 * des Startlaufwerks. Heute: FAT16 -> FAT32 (UFAT32.EXE, ufat32_conv.c).
 * ERRORLEVEL: 0 umgewandelt, 3 abgebrochen/Fehler, 4 falscher Aufruf.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], cmd[FAPI_ARGLEN], fs[12], path[FAPI_PATHLEN], chk[FAPI_PATHLEN], cs[FAPI_PATHLEN];
static struct fapi_fsinfo fi;

/* Programm im Systemordner des Startlaufwerks: <sys>\DOS\<sub><name> */
static int sys_prog(char *out, const char *sub, const char *name)
{
    unsigned h;
    out[0] = 'C';
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
        out[0] = up(cs[0]);
    out[1] = 0;
    scat(out, ":\\DOS\\", FAPI_PATHLEN);
    scat(out, sub, FAPI_PATHLEN);
    scat(out, name, FAPI_PATHLEN);
    h = DosOpen(out);
    if (FAPI_FAILED(h))
        return -1;
    DosClose(h);
    return 0;
}

static unsigned run(const char *prog, unsigned d, const char *opt)
{
    unsigned k;
    scopy(cmd, prog, sizeof(cmd));
    k = slen(cmd);
    cmd[k++] = ' ';
    cmd[k++] = (char)('A' + d);
    cmd[k++] = ':';
    cmd[k] = 0;
    scat(cmd, opt, sizeof(cmd));
    if (DosExecPgm(cmd, EXEC_SYNC)) {
        puts("Programm kann nicht gestartet werden: ");
        puts(prog);
        puts("\r\n");
        return 3;
    }
    return DosQChildRC();
}

void main(void)
{
    static char prog[16];
    char *a;
    unsigned d = 0xFFFF, k;
    int verb = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[1] == ':' && a[0] != '/')
            d = (unsigned)(up(a[0]) - 'A');
        else if (a[0] == '/' && up(a[1]) == 'F' && up(a[2]) == 'S' && a[3] == ':') {
            for (k = 0; a[4 + k] && a[4 + k] != ' ' && k < sizeof(fs) - 1; k++)
                fs[k] = up(a[4 + k]);
            fs[k] = 0;
        } else if (a[0] == '/' && up(a[1]) == 'V')
            verb = 1;
        else if (a[0] == '/' && a[1] == '?') {
            puts("Wandelt das Dateisystem eines Laufwerks um, ohne Daten zu l\x94schen.\r\n\r\n"
                 "CONVERT Laufwerk: /FS:FAT32 [/V]\r\n\r\n"
                 "  /FS:FAT32  FAT16-Partition in FAT32 umwandeln (kleinere\r\n"
                 "             Zuordnungseinheiten, mehr Platz); nicht umkehrbar\r\n"
                 "  /V         Einzelheiten anzeigen\r\n\r\n"
                 "Vorher wird das Laufwerk mit CHKDSK gepr\x81" "ft.\r\n");
            return;
        } else {
            puts("Ung\x81ltiger Parameter - ");
            puts(a);
            puts("\r\n");
            DosExit(4);
        }
    }
    if (d == 0xFFFF || !fs[0]) {
        puts("Aufruf: CONVERT Laufwerk: /FS:FAT32\r\n");
        DosExit(4);
    }
    if (!same(fs, "FAT32")) {
        puts("Umwandeln nach ");
        puts(fs);
        puts(" wird nicht unterst\x81tzt (nur FAT32).\r\n");
        DosExit(4);
    }
    if (DosQFSInfo(d, &fi) || (fi.flags & (FSI_READONLY | FSI_REMOVABLE))) {
        puts("Ung\x81ltiges Laufwerk, Wechseldatentr\x84ger oder schreibgesch\x81tzt.\r\n");
        DosExit(3);
    }
    if (same(fi.fsname, "FAT32")) {
        puts("Das Laufwerk ist bereits FAT32.\r\n");
        DosExit(3);
    }
    if (!same(fi.fsname, "FAT")) {
        puts("Nur FAT16-Laufwerke lassen sich umwandeln.\r\n");
        DosExit(3);
    }
    scopy(prog, "UFAT32.EXE", sizeof(prog));
    if (sys_prog(path, "SYSTEM\\", prog) || sys_prog(chk, "", "CHKDSK.EXE")) {
        puts("UFAT32.EXE oder CHKDSK.EXE fehlt.\r\n");
        DosExit(3);
    }
    puts("Das Laufwerk wird gepr\x81" "ft ...\r\n");
    if (run(chk, d, "") != 0) {
        puts("CHKDSK hat Fehler gefunden - zuerst CHKDSK ");
        putc((char)('A' + d));
        puts(": /F ausf\x81hren.\r\n");
        DosExit(3);
    }
    if (run(path, d, verb ? " /CONVERT /V" : " /CONVERT"))
        DosExit(3);
}
