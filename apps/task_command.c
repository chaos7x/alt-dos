/*
 * COMMAND.EXE - Befehlsinterpreter fuer AT-DOS 5.0
 *
 * Nachfolger von COMMAND.COM fuer den Protected Mode: spricht das System
 * nur ueber die FAPI an. Interne Befehle: DIR, CD, Laufwerk:, TYPE, COPY,
 * DEL, MD, RD, REN, CLS, VER, ECHO, START, HILFE, EXIT. Alles andere wird
 * als Programm gestartet. COPY und DEL verstehen * und ? wie MS-DOS.
 *
 * COMMAND.EXE /P  -  permanent, EXIT ist dann gesperrt (erste Sitzung).
 */
#include "fapi.h"

#define LINE 78

static char line[LINE + 1];
static char buf[256];
static int  permanent;

/* ---- Hilfen ------------------------------------------------------------ */
static void putc(char c)
{
    VioWrtTTY(&c, 1);
}

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int same(const char *a, const char *b)       /* b in Grossbuchstaben */
{
    while (*b)
        if (up(*a++) != *b++)
            return 0;
    return *a == 0 || *a == ' ';
}

static char *skip(char *p)
{
    while (*p == ' ')
        p++;
    return p;
}

static char *next_word(char *p)
{
    while (*p && *p != ' ')
        p++;
    return skip(p);
}

/* Zahl rechtsbuendig in w Zeichen ausgeben, mit oder ohne Tausenderpunkte */
static void put_numx(unsigned long v, int w, int dots)
{
    static const unsigned long pw[10] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    char d[14], o[14];
    int i, n = 0, k = 0, started = 0;

    for (i = 0; i < 10; i++) {                  /* ohne Langdivision    */
        char c = '0';
        while (v >= pw[i]) {
            v -= pw[i];
            c++;
        }
        if (c != '0' || started || i == 9) {
            d[n++] = c;
            started = 1;
        }
    }
    for (i = 0; i < n; i++) {
        o[k++] = d[i];
        if (dots && (n - i - 1) % 3 == 0 && i != n - 1)
            o[k++] = '.';
    }
    o[k] = 0;
    for (i = k; i < w; i++)
        putc(' ');
    puts(o);
}

#define put_num(v, w) put_numx((v), (w), 1)

static void put2(unsigned v)
{
    putc((char)('0' + v / 10 % 10));
    putc((char)('0' + v % 10));
}

/* ---- Eingabezeile ------------------------------------------------------ */
static void read_line(void)
{
    int n = 0;
    unsigned char c;

    for (;;) {
        c = (unsigned char)KbdCharIn();
        if (c == 13) {
            puts("\n");
            break;
        }
        if (c == 8) {
            if (n) {
                n--;
                puts("\b \b");
            }
        } else if (c == 27) {
            while (n) {
                n--;
                puts("\b \b");
            }
        } else if (c >= 32 && n < LINE) {
            line[n++] = (char)c;
            putc((char)c);
        }
    }
    line[n] = 0;
}

/* ein Wort aus der Eingabe als eigene Zeichenkette */
static char *word(char *p, char *out, int max)
{
    int i = 0;
    while (*p && *p != ' ' && i < max - 1)
        out[i++] = *p++;
    out[i] = 0;
    return out;
}

/* ---- interne Befehle --------------------------------------------------- */
static void cmd_dir(char *arg)
{
    static struct fapi_dirent e;
    static char dir[FAPI_PATHLEN], cur[FAPI_CURDIRLEN];
    unsigned n, files = 0, dirs = 0;
    unsigned long total = 0;
    int all = 0;

    dir[0] = 0;
    for (; *arg; arg = next_word(arg)) {
        if (same(arg, "/A"))
            all = 1;
        else
            word(arg, dir, sizeof(dir));
    }
    if (dir[0] && DosFindEntry(&e, 0, dir) == FAPI_ERROR) {
        puts("Verzeichnis nicht gefunden.\n");
        return;
    }
    puts("\n Verzeichnis von ");
    if (dir[0] && dir[1] == ':') {
        puts(dir);
    } else {
        DosQCurDir(cur);
        puts(cur);
        if (dir[0]) {
            if (cur[3])
                putc('\\');
            puts(dir);
        }
    }
    puts("\n\n");
    for (n = 0; DosFindEntry(&e, n, dir[0] ? dir : 0) != FAPI_ERROR; n++) {
        if (!all && (e.attr & 0x06))            /* versteckt, System    */
            continue;
        put2(e.date & 31);
        putc('.');
        put2((e.date >> 5) & 15);
        putc('.');
        put_numx(1980 + (e.date >> 9), 4, 0);
        puts("  ");
        put2(e.time >> 11);
        putc(':');
        put2((e.time >> 5) & 63);
        if (e.attr & 0x10) {
            puts("    <DIR>         ");
            dirs++;
        } else {
            put_num(e.size, 18);
            files++;
            total += e.size;
        }
        putc(' ');
        puts(e.name);
        puts("\n");
    }
    put_num(files, 16);
    puts(" Datei(en),");
    put_num(total, 15);
    puts(" Bytes\n");
    put_num(dirs, 16);
    puts(" Verzeichnis(se),");
    {
        static unsigned long fb;
        unsigned drv = (dir[0] && dir[1] == ':') ? (unsigned)(up(dir[0]) - 'A') : DosQCurDisk();
        if (DosFreeSpace(drv, &fb) == 0)
            put_num(fb, 15);
        else
            puts("               ?");
    }
    puts(" Bytes frei\n\n");
}

static void cmd_cd(char *arg)
{
    static char p[FAPI_PATHLEN], cur[FAPI_CURDIRLEN];

    if (!*arg) {
        DosQCurDir(cur);
        puts(cur);
        puts("\n");
        return;
    }
    if (DosChDir(word(arg, p, sizeof(p))) == FAPI_ERROR)
        puts("Ung\x81ltiges Verzeichnis.\n");
}

static void cmd_type(char *arg)
{
    static char name[FAPI_PATHLEN];
    unsigned h, n;
    char last = '\n';

    if (!*arg) {
        puts("Syntax: TYPE Dateiname\n");
        return;
    }
    h = DosOpen(word(arg, name, sizeof(name)));
    if (h == FAPI_ERROR) {
        puts("Datei nicht gefunden.\n");
        return;
    }
    while ((n = DosRead(h, buf, sizeof(buf))) != 0 && n != FAPI_ERROR) {
        VioWrtTTY(buf, n);
        last = buf[n - 1];
    }
    DosClose(h);
    if (last != '\n')
        puts("\n");
}

static void exec_error(unsigned r)
{
    switch (r) {
    case EXEC_ERR_NOTFOUND: puts("Befehl oder Dateiname nicht gefunden.\n"); break;
    case EXEC_ERR_NOMEM:    puts("Nicht gen\x81gend Arbeitsspeicher.\n"); break;
    case EXEC_ERR_FORMAT:   puts("Kein FAPI-Programm.\n"); break;
    case EXEC_ERR_NOSLOT:   puts("Zu viele Programme gleichzeitig.\n"); break;
    default:                puts("Programm konnte nicht gestartet werden.\n");
    }
}

/* ---- Dateien schreiben ------------------------------------------------- */
static void file_error(unsigned r)
{
    switch (r) {
    case FAPI_ERR_WP:     puts("Datentr\x84ger ist schreibgesch\x81tzt.\n"); break;
    case FAPI_ERR_FULL:   puts("Datentr\x84ger oder Verzeichnis voll.\n"); break;
    case FAPI_ERR_EXIST:  puts("Datei oder Verzeichnis existiert bereits.\n"); break;
    case FAPI_ERR_DENIED: puts("Zugriff verweigert.\n"); break;
    case FAPI_ERR_INUSE:  puts("Datei oder Verzeichnis wird gerade benutzt.\n"); break;
    default:              puts("Datei oder Pfad nicht gefunden.\n");
    }
}

static int has_wild(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return 1;
    return 0;
}

/* ein Teil (Name oder Endung) gegen ein Muster mit * und ? */
static int match_part(const char *n, unsigned nl, const char *p, unsigned pl)
{
    unsigned i, k;
    for (i = 0, k = 0; k < pl; k++) {
        if (p[k] == '*')
            return 1;
        if (i >= nl || (p[k] != '?' && up(p[k]) != up(n[i])))
            return 0;
        i++;
    }
    return i == nl;
}

/* "NAME.EXT" gegen Muster wie MS-DOS: Name und Endung getrennt */
static int match(const char *name, const char *pat)
{
    const char *nd = name, *pd = pat;
    unsigned nl, pl;

    if (pat[0] == '*' && pat[1] == 0)
        return 1;
    while (*nd && *nd != '.') nd++;
    while (*pd && *pd != '.') pd++;
    nl = (unsigned)(nd - name);
    pl = (unsigned)(pd - pat);
    if (!match_part(name, nl, pat, pl))
        return 0;
    if (*nd) nd++;
    if (*pd) pd++;
    for (nl = 0; nd[nl]; nl++) ;
    for (pl = 0; pd[pl]; pl++) ;
    return match_part(nd, nl, pd, pl);
}

/* Pfad in Verzeichnisteil (mit \ oder :) und Namen zerlegen */
static char *base_name(char *path)
{
    char *b = path, *p;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == ':')
            b = p + 1;
    return b;
}

/* dir + name -> out */
static void join(char *out, const char *dir, unsigned dl, const char *name)
{
    unsigned i = 0;
    while (i < dl && i < FAPI_PATHLEN - 14) {
        out[i] = dir[i];
        i++;
    }
    if (i && out[i - 1] != '\\' && out[i - 1] != ':')
        out[i++] = '\\';
    while (*name && i < FAPI_PATHLEN - 1)
        out[i++] = *name++;
    out[i] = 0;
}

static int same_path(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b))
        a++, b++;
    return *a == 0 && *b == 0;
}

/* ist p ein vorhandenes Verzeichnis? */
static int is_dir(char *p)
{
    static struct fapi_dirent e;
    static char cur[FAPI_CURDIRLEN];
    unsigned n = 0;
    while (*p == ' ') p++;
    for (; p[n]; n++) ;
    if (n == 0 || p[n - 1] == '\\' || p[n - 1] == ':' || (p[0] == '.' && p[1] == 0))
        return 1;
    if (DosFindEntry(&e, 0, p) != FAPI_ERROR)
        return 1;
    /* leeres Stammverzeichnis oder leeres Unterverzeichnis: ueber CD pruefen */
    DosQCurDir(cur);
    if (DosChDir(p) != FAPI_ERROR) {
        DosChDir(cur + 2);
        return 1;
    }
    return 0;
}

static char cbuf[8192];

/* eine Datei kopieren; 0 = ok */
static int copy_one(const char *src, const char *dst)
{
    unsigned hs, hd, n, w;

    if (same_path(src, dst)) {
        puts("Datei kann nicht auf sich selbst kopiert werden.\n");
        return -1;
    }
    hs = DosOpen(src);
    if (hs == FAPI_ERROR) {
        puts("Datei nicht gefunden.\n");
        return -1;
    }
    hd = DosCreate(dst);
    if (FAPI_FAILED(hd)) {
        DosClose(hs);
        file_error(hd);
        return -1;
    }
    while ((n = DosRead(hs, cbuf, sizeof(cbuf))) != 0) {
        if (n == FAPI_ERROR) {
            puts("Lesefehler.\n");
            break;
        }
        w = DosWrite(hd, cbuf, n);
        if (FAPI_FAILED(w) || w != n) {
            file_error(FAPI_FAILED(w) ? w : FAPI_ERR_FULL);
            n = FAPI_ERROR;
            break;
        }
    }
    DosClose(hs);
    w = DosClose(hd);
    if (n == FAPI_ERROR || FAPI_FAILED(w)) {
        DosDelete(dst);                         /* halbe Datei weg      */
        return -1;
    }
    return 0;
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void cmd_copy(char *arg)
{
    static struct fapi_dirent e;
    static char src[FAPI_PATHLEN], dst[FAPI_PATHLEN], from[FAPI_PATHLEN],
                to[FAPI_PATHLEN], sdir[FAPI_PATHLEN];
    char *pat;
    unsigned n, count = 0, dl;

    word(arg, src, sizeof(src));
    word(next_word(arg), dst, sizeof(dst));
    if (!src[0]) {
        puts("Syntax: COPY Quelle [Ziel]\n");
        return;
    }
    pat = base_name(src);
    dl = (unsigned)(pat - src);
    if (!has_wild(pat) && is_dir(src)) {        /* COPY VERZ = VERZ\*.* */
        dl = slen(src);
        pat = "*.*";
    }
    for (n = 0; n < dl; n++)
        sdir[n] = src[n];
    sdir[dl] = 0;

    if (has_wild(pat)) {
        if (!is_dir(dst)) {
            puts("Bei mehreren Dateien muss das Ziel ein Verzeichnis sein.\n");
            return;
        }
        for (n = 0; DosFindEntry(&e, n, dl ? sdir : 0) != FAPI_ERROR; n++) {
            if ((e.attr & 0x16) || !match(e.name, pat))
                continue;                       /* Ordner, versteckt    */
            join(from, sdir, dl, e.name);
            join(to, dst, slen(dst), e.name);
            puts(e.name);
            puts("\n");
            if (copy_one(from, to))
                break;
            count++;
        }
    } else {
        if (is_dir(dst))
            join(to, dst, slen(dst), pat);
        else
            join(to, "", 0, dst);
        if (copy_one(src, to) == 0)
            count = 1;
    }
    put_numx(count, 8, 0);
    puts(" Datei(en) kopiert\n");
}

static int confirm(const char *q)
{
    char c;
    puts(q);
    c = up((char)KbdCharIn());
    putc(c);
    puts("\n");
    return c == 'J' || c == 'Y';
}

static void cmd_del(char *arg)
{
    static struct fapi_dirent e;
    static char p[FAPI_PATHLEN], dir[FAPI_PATHLEN], f[FAPI_PATHLEN];
    char *pat;
    unsigned n, dl, r, count = 0;

    word(arg, p, sizeof(p));
    if (!p[0]) {
        puts("Syntax: DEL Datei (auch * und ?)\n");
        return;
    }
    if (!has_wild(p) && is_dir(p)) {            /* DEL VERZ = VERZ\*.*  */
        join(f, p, slen(p), "*.*");
        join(p, "", 0, f);
    }
    pat = base_name(p);
    dl = (unsigned)(pat - p);
    for (n = 0; n < dl; n++)
        dir[n] = p[n];
    dir[dl] = 0;
    if (!has_wild(pat)) {
        r = DosDelete(p);
        if (r)
            file_error(r);
        return;
    }
    if (match("A.B", pat) && match("XYZ.Q", pat) && match("Z", pat) &&
        !confirm("Alle Dateien im Verzeichnis werden gel\x94scht! Sind Sie sicher (J/N)? "))
        return;
    for (n = 0; DosFindEntry(&e, n, dl ? dir : 0) != FAPI_ERROR; ) {
        if ((e.attr & 0x16) || !match(e.name, pat)) {
            n++;
            continue;
        }
        join(f, dir, dl, e.name);
        r = DosDelete(f);
        if (r) {
            puts(e.name);
            puts(": ");
            file_error(r);
            n++;
        } else
            count++;                            /* Eintrag ist weg: n bleibt */
    }
    if (!count)
        puts("Keine Datei gefunden.\n");
}

static void cmd_md(char *arg, int rd)
{
    static char p[FAPI_PATHLEN];
    unsigned r;
    if (!*arg) {
        puts(rd ? "Syntax: RD Verzeichnis\n" : "Syntax: MD Verzeichnis\n");
        return;
    }
    word(arg, p, sizeof(p));
    r = rd ? DosRmDir(p) : DosMkDir(p);
    if (r == FAPI_ERR_DENIED && rd)
        puts("Verzeichnis ist nicht leer.\n");
    else if (r)
        file_error(r);
}

static void cmd_ren(char *arg)
{
    static char from[FAPI_PATHLEN], name[FAPI_PATHLEN], to[FAPI_PATHLEN];
    unsigned r;
    word(arg, from, sizeof(from));
    word(next_word(arg), name, sizeof(name));
    if (!from[0] || !name[0]) {
        puts("Syntax: REN Datei Neuer_Name\n");
        return;
    }
    if (base_name(name) != name) {
        puts("Der neue Name darf keinen Pfad enthalten.\n");
        return;
    }
    join(to, from, (unsigned)(base_name(from) - from), name);
    r = DosMove(from, to);
    if (r)
        file_error(r);
}

static void cmd_help(void)
{
    puts("Interne Befehle:\n"
         "  DIR [Pfad] [/A] Dateien anzeigen (/A auch versteckte)\n"
         "  CD [Pfad]       Verzeichnis wechseln oder anzeigen\n"
         "  C:              Laufwerk wechseln\n"
         "  TYPE Datei      Textdatei ausgeben\n"
         "  COPY Quelle [Ziel]  Dateien kopieren (auch * und ?)\n"
         "  DEL Datei       Dateien l\x94schen (auch * und ?)\n"
         "  MD / RD Verz.   Verzeichnis anlegen / entfernen\n"
         "  REN Datei Name  Datei oder Verzeichnis umbenennen\n"
         "  CLS             Bildschirm l\x94schen\n"
         "  VER             Version anzeigen\n"
         "  ECHO Text       Text ausgeben\n"
         "  START Programm  Programm in eigener Sitzung starten\n"
         "  EXIT            Befehlsinterpreter beenden\n"
         "Andere Eingaben starten das gleichnamige Programm, z. B. MEM.\n"
         "Alt+Esc wechselt die Sitzung, Strg+C bricht ein Programm ab,\n"
         "Strg+Alt+Entf schlie\xE1t alle Dateien und startet neu.\n");
}

static void cmd_ver(void)
{
    unsigned v = DosGetVersion();
    puts("\nAT-DOS 5.0 (alternative Zeitlinie), 80286 Protected Mode\nFAPI-Version ");
    putc((char)('0' + (v >> 8)));
    putc('.');
    put2(v & 0xFF);
    puts("\n\n");
}

/* ---- Hauptschleife ----------------------------------------------------- */
static int execute(void)
{
    char *cmd = skip(line), *arg;
    unsigned r;

    if (!*cmd)
        return 1;
    arg = next_word(cmd);

    if (cmd[0] && cmd[1] == ':' && (cmd[2] == 0 || cmd[2] == ' ')) {
        if (DosSelectDisk(up(cmd[0]) - 'A') == FAPI_ERROR)
            puts("Ung\x81ltiges Laufwerk.\n");
    } else if (same(cmd, "CLS"))
        VioCls();
    else if (same(cmd, "DIR"))
        cmd_dir(arg);
    else if (same(cmd, "TYPE"))
        cmd_type(arg);
    else if (same(cmd, "COPY"))
        cmd_copy(arg);
    else if (same(cmd, "DEL") || same(cmd, "ERASE"))
        cmd_del(arg);
    else if (same(cmd, "MD") || same(cmd, "MKDIR"))
        cmd_md(arg, 0);
    else if (same(cmd, "RD") || same(cmd, "RMDIR"))
        cmd_md(arg, 1);
    else if (same(cmd, "REN") || same(cmd, "RENAME"))
        cmd_ren(arg);
    else if (same(cmd, "CD") || same(cmd, "CHDIR"))
        cmd_cd(arg);
    else if (same(cmd, "VER"))
        cmd_ver();
    else if (same(cmd, "ECHO")) {
        puts(arg);
        puts("\n");
    } else if (same(cmd, "HILFE") || same(cmd, "HELP") || same(cmd, "?"))
        cmd_help();
    else if (same(cmd, "START")) {
        if (!*arg)
            puts("Syntax: START Programm [Argumente]\n");
        else if ((r = DosExecPgm(arg, EXEC_ASYNC)) != 0)
            exec_error(r);
    } else if (same(cmd, "EXIT")) {
        if (!permanent)
            return 0;
        puts("Dieser Befehlsinterpreter ist permanent (/P) und kann nicht beendet werden.\n");
    } else if ((r = DosExecPgm(cmd, EXEC_SYNC)) != 0)
        exec_error(r);
    return 1;
}

void main(void)
{
    static char args[FAPI_ARGLEN];
    char *p;

    DosGetArgs(args);
    for (p = args; *p; p = next_word(p))
        if (same(p, "/P"))
            permanent = 1;

    puts("COMMAND.EXE - Befehlsinterpreter f\x81r AT-DOS 5.0\n"
         "HILFE zeigt die verf\x81gbaren Befehle.\n\n");
    do {
        static char cur[FAPI_CURDIRLEN];
        DosQCurDir(cur);
        puts(cur);
        puts(">");
        read_line();
    } while (execute());
}
