/*
 * COMMAND.EXE - Befehlsinterpreter fuer DOS 5.0
 *
 * Nachfolger von COMMAND.COM fuer den Protected Mode: spricht das System
 * nur ueber die FAPI an. Interne Befehle: DIR, TYPE, CLS, VER, ECHO, START,
 * HILFE, EXIT. Alles andere wird als Programm (NAME.EXE) gestartet.
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

/* ---- interne Befehle --------------------------------------------------- */
static void cmd_dir(char *arg)
{
    static struct fapi_dirent e;
    unsigned n, files = 0;
    unsigned long total = 0;
    int all = same(arg, "/A");

    puts("\n Verzeichnis von A:\\\n\n");
    for (n = 0; DosFindEntry(&e, n) != FAPI_ERROR; n++) {
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
        put_num(e.size, 18);
        putc(' ');
        puts(e.name);
        puts("\n");
        files++;
        total += e.size;
    }
    put_num(files, 16);
    puts(" Datei(en),");
    put_num(total, 15);
    puts(" Bytes\n\n");
}

static void cmd_type(char *arg)
{
    unsigned h, n;
    char last = '\n';

    if (!*arg) {
        puts("Syntax: TYPE Dateiname\n");
        return;
    }
    h = DosOpen(arg);
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

static void cmd_help(void)
{
    puts("Interne Befehle:\n"
         "  DIR [/A]        Dateien auf A: anzeigen (/A auch versteckte)\n"
         "  TYPE Datei      Textdatei ausgeben\n"
         "  CLS             Bildschirm l\x94schen\n"
         "  VER             Version anzeigen\n"
         "  ECHO Text       Text ausgeben\n"
         "  START Programm  Programm in eigener Sitzung starten\n"
         "  EXIT            Befehlsinterpreter beenden\n"
         "Andere Eingaben starten das gleichnamige Programm, z. B. MEM.\n"
         "Alt+Esc wechselt die Sitzung, Strg+C bricht ein Programm ab.\n");
}

static void cmd_ver(void)
{
    unsigned v = DosGetVersion();
    puts("\nDOS 5.0 (alternative Zeitlinie), 80286 Protected Mode\nFAPI-Version ");
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

    if (same(cmd, "CLS"))
        VioCls();
    else if (same(cmd, "DIR"))
        cmd_dir(arg);
    else if (same(cmd, "TYPE"))
        cmd_type(arg);
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

    puts("COMMAND.EXE - Befehlsinterpreter f\x81r DOS 5.0\n"
         "HILFE zeigt die verf\x81gbaren Befehle.\n\n");
    do {
        puts("A:\\>");
        read_line();
    } while (execute());
}
