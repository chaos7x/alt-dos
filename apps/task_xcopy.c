/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * XCOPY.EXE - Dateien und Verzeichnisbaeume kopieren (wie MS-DOS 6):
 *
 *   XCOPY Quelle [Ziel] [/A | /M] [/P] [/S [/E]] [/K] [/Y | /-Y]
 *
 *   /A  nur Dateien mit Archiv-Attribut      /M  dasselbe, Attribut danach weg
 *   /P  vor jeder Datei fragen               /S  Unterverzeichnisse (nicht leere)
 *   /E  auch leere Unterverzeichnisse        /Y  vorhandene Ziele ohne Frage
 *   /K  Attribute behalten (sonst wird Schreibgeschuetzt zurueckgesetzt wie
 *       bei NT - wichtig beim Kopieren von CD, dort ist alles schreibgeschuetzt)
 * Versteckte und System-Dateien werden wie bei MS-DOS nicht kopiert. Datum
 * und Uhrzeit bleiben erhalten.
 */
#include "fapi.h"
#include "util.h"
#include "namepat.h"

static char args[FAPI_ARGLEN], s_arg[FAPI_PATHLEN], d_arg[FAPI_PATHLEN];
static char src[FAPI_PATHLEN], dst[FAPI_PATHLEN], pat[FAPI_PATHLEN];
static char to[FAPI_PATHLEN], made[FAPI_PATHLEN], dpat[FAPI_PATHLEN], pn[FAPI_PATHLEN];
static int opt_a, opt_m, opt_p, opt_s, opt_e, opt_k, yes_all, to_file, dwild;
static unsigned copied, errors, slen_src;

/* alle Verzeichnisse bis einschliesslich p anlegen */
static void mkdirs(const char *p)
{
    unsigned i, n = slen(p);
    for (i = 3; i <= n; i++)
        if (p[i] == '\\' || p[i] == 0) {
            scopy(made, p, i + 1);
            if (!is_dir(made))
                DosMkDir(made);
        }
}

/* Zielpfad zu einem Quellpfad: Ziel + Rest hinter dem Quellverzeichnis */
static void make_to(const char *path)
{
    const char *r = path + slen_src;
    unsigned n;
    scopy(to, dst, sizeof(to));
    n = slen(to);
    if (n && to[n - 1] == '\\' && *r == '\\')
        r++;
    scat(to, r, sizeof(to));
    if (dpat[0]) {                      /* Ziel mit Muster: *.BAK usw.  */
        char *b = base_name(to);
        apply_pat(b, dpat, pn);
        *b = 0;
        scat(to, pn, sizeof(to));
    }
}

static int exists(const char *p)
{
    unsigned h = DosOpen(p);
    if (FAPI_FAILED(h))
        return 0;
    DosClose(h);
    return 1;
}

static int confirm_overwrite(void)
{
    unsigned c;
    if (yes_all)
        return 1;
    puts("\x9A" "berschreiben ");
    puts(to);
    puts(" (Ja/Nein/Alle)? ");
    for (;;) {
        c = (unsigned)up((char)KbdCharIn());
        if (c == 'J' || c == 'Y') { puts("J\r\n"); return 1; }
        if (c == 'N' || c == 0) { puts("N\r\n"); return 0; }
        if (c == 'A') { puts("A\r\n"); yes_all = 1; return 1; }
    }
}

static void copy_one(char *path, struct fapi_dirent *e)
{
    static struct fapi_finfo fi;
    char *d;
    int rc;

    if (to_file)
        scopy(to, dst, sizeof(to));
    else
        make_to(path);
    if (opt_p) {
        puts(path);
        puts(" (J/N)? ");
        if (!ask_yn())
            return;
    } else {
        puts(path);
        puts("\r\n");
    }
    if (!to_file) {                     /* Zielverzeichnis bei Bedarf   */
        d = base_name(to);
        d[-1] = 0;
        if (slen(to) > 2)
            mkdirs(to);
        d[-1] = '\\';
    }
    if (exists(to) && !confirm_overwrite())
        return;
    if ((rc = copy_file(path, to)) != 0) {
        puts("Fehler beim Kopieren nach ");
        puts(to);
        puts(rc == -6 ? " (Quelle wird von einem anderen Programm geschrieben)\r\n" :
             rc == -7 ? " (unzureichender Speicherplatz)\r\n" :
             rc == -1 ? " (Quelle nicht lesbar)\r\n" :
             rc == -2 ? " (Ziel nicht anlegbar)\r\n" :
             rc == -3 ? " (Lesefehler)\r\n" :
             rc == -4 ? " (Schreibfehler)\r\n" : " (Fehler beim Schlie\xE1" "en)\r\n");
        errors++;
        return;
    }
    fi.mask = 3;                        /* Datum/Uhrzeit wie das Original */
    fi.attr = (opt_k ? e->attr & 0x07 : 0) | 0x20;   /* R nur mit /K */
    fi.date = e->date;
    fi.time = e->time;
    DosSetFInfo(to, &fi);
    if (opt_m) {                        /* Archiv-Attribut der Quelle weg */
        fi.mask = 1;
        fi.attr = e->attr & ~0x20 & 0x27;
        DosSetFInfo(path, &fi);
    }
    copied++;
}

static int visit(char *path, struct fapi_dirent *e, int what, int depth)
{
    (void)depth;
    if (what == WT_LEAVE)
        return 0;
    if (what == WT_DIR) {
        if (!opt_s)
            return 0;
        if (opt_e) {                    /* leere Verzeichnisse mitnehmen */
            make_to(path);
            mkdirs(to);
        }
        return WT_DESCEND;
    }
    if ((e->attr & 0x06) || !match(e->name, pat))
        return 0;
    if ((opt_a || opt_m) && !(e->attr & 0x20))
        return 0;
    copy_one(path, e);
    return 0;
}

void main(void)
{
    char *a, *b;
    unsigned c;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/') {
            switch (up(a[1])) {
            case 'A': opt_a = 1; break;
            case 'M': opt_m = 1; break;
            case 'P': opt_p = 1; break;
            case 'S': opt_s = 1; break;
            case 'E': opt_e = opt_s = 1; break;
            case 'K': opt_k = 1; break;
            case 'Y': yes_all = 1; break;
            case 'V': case 'W': break;      /* angenommen, ohne Wirkung */
            case '-': yes_all = 0; break;
            case '?':
                puts("Kopiert Dateien und Verzeichnisb\x84ume.\r\n\r\n"
                     "XCOPY Quelle [Ziel] [/A | /M] [/P] [/S [/E]] [/K] [/Y | /-Y]\r\n\r\n"
                     "  /A  nur Dateien mit Archiv-Attribut\r\n"
                     "  /M  wie /A, Archiv-Attribut der Quelle danach l\x94schen\r\n"
                     "  /P  vor jeder Datei fragen\r\n"
                     "  /S  Unterverzeichnisse (au\xE1" "er leeren)   /E  auch leere\r\n"
                     "  /K  Attribute behalten (sonst wird Schreibschutz entfernt)\r\n"
                     "  /Y  vorhandene Ziele ohne Frage \x81" "berschreiben\r\n");
                return;
            default:
                puts("Ung\x81ltiger Parameter - ");
                puts(a);
                puts("\r\n");
                DosExit(4);
            }
        } else if (!s_arg[0])
            word(a, s_arg, sizeof(s_arg));
        else
            word(a, d_arg, sizeof(d_arg));
    }
    if (!s_arg[0]) {
        puts("Zu wenige Parameter\r\n");
        DosExit(4);
    }
    abs_path(s_arg, src);
    abs_path(d_arg[0] ? d_arg : ".", dst);
    b = base_name(dst);                 /* Ziel C:\X\*.* bzw. *.BAK: das */
    if (has_wild(b)) {                  /* Verzeichnis davor, das Muster */
        dwild = 1;
        if (!(b[0] == '*' && (!b[1] || (b[1] == '.' && b[2] == '*' && !b[3]))))
            scopy(dpat, b, sizeof(dpat));   /* gilt fuer die Namen      */
        *b = 0;
        if (slen(dst) > 3)
            dst[slen(dst) - 1] = 0;
    }
    if (!has_wild(src) && is_dir(src)) {
        scopy(pat, "*.*", sizeof(pat));
    } else {
        b = base_name(src);
        scopy(pat, b, sizeof(pat));
        *b = 0;
        if (slen(src) > 3)
            src[slen(src) - 1] = 0;
    }
    slen_src = slen(src);
    if (src[slen_src - 1] == '\\')
        slen_src--;                     /* "C:\" -> Rest beginnt mit \ */
    if (!is_dir(dst)) {
        if (!has_wild(pat) && !opt_s && !dwild) {
            puts("Gibt ");
            puts(d_arg);
            puts(" einen Dateinamen oder ein Verzeichnis an\r\n(D = Datei, V = Verzeichnis)? ");
            for (;;) {
                c = (unsigned)up((char)KbdCharIn());
                if (c == 'D') { puts("D\r\n"); to_file = 1; break; }
                if (c == 'V') { puts("V\r\n"); break; }
                if (c == 0) DosExit(2);
            }
        }
        if (!to_file)
            mkdirs(dst);
    }
    if (!to_file && opt_s && up(src[0]) == up(dst[0])) {   /* Ziel in Quelle? */
        unsigned n = slen(src);
        for (c = 0; c < n && up(src[c]) == up(dst[c]); c++)
            ;
        if (c == n && (dst[n] == '\\' || dst[n] == 0 || n == 3)) {
            puts("Zyklische Kopie nicht m\x94glich\r\n");
            DosExit(4);
        }
    }
    puts("Quelldateien werden gelesen...\r\n\r\n");
    walk_tree(src, visit);
    put_num(copied, 0, 0);
    puts(" Datei(en) kopiert\r\n");
    if (errors)
        DosExit(4);
    DosExit(copied ? 0 : 1);
}
