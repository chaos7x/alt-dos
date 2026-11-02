/*
 * COMMAND.EXE - Befehlsinterpreter fuer AT-DOS 5.0
 *
 * Nachfolger von COMMAND.COM fuer den Protected Mode: spricht das System
 * nur ueber die FAPI an. Interne Befehle: DIR, CD, Laufwerk:, TYPE, COPY,
 * DEL, MD, RD, REN, CLS, VER, ECHO, START, DOSKEY, HILFE, EXIT. Zeilen-
 * editor und Befehlsspeicher wie DOSKEY sind eingebaut. Alles andere wird
 * als Programm gestartet. COPY und DEL verstehen * und ? wie MS-DOS.
 *
 * COMMAND.EXE /P  -  permanent, EXIT ist dann gesperrt (erste Sitzung).
 */
#include <i86.h>
#include "fapi.h"

#define LINE 126                        /* wie MS-DOS 127 mit Nullbyte */

static char line[LINE + 1];
static char buf[256];
static int  permanent;
static unsigned errorlevel;             /* Rueckgabewert des letzten Programms */
static int  break_on, verify_on, keys_on = 1, ctty; /* BREAK, VERIFY, KEYS, CTTY */
static volatile int break_req;          /* Strg+C in einer Stapeldatei   */
static int  ran_prog;                   /* letzte Zeile startete ein Programm */

/* ---- Hilfen ------------------------------------------------------------ */
static void putc(char c)
{
    static char b;
    b = c;
    DosWrite(STDOUT, &b, 1);
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

/* "..." haelt lange Namen mit Leerzeichen zusammen */
static char *next_word(char *p)
{
    int q = 0;
    while (*p && (q || *p != ' ')) {
        if (*p == '"')
            q = !q;
        p++;
    }
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

/* ---- Eingabezeile ------------------------------------------------------
   Eingebaut wie DOSKEY: Pfeil auf/ab blaettert in den letzten Befehlen,
   Pfeil links/rechts, Pos1, Ende, Entf, Einfg (Einfuegen/Ueberschreiben),
   Esc loescht die Zeile, F3 holt den Rest des letzten Befehls, Bild auf/ab
   springt zum aeltesten/neuesten Befehl. Der Cursor wird nur mit \b
   bewegt - die Zeile bleibt deshalb in einer Bildschirmzeile. */
#define NHIST 16
static char     hist[NHIST][LINE + 1];
static unsigned hist_n;                 /* gespeicherte Befehle (<= NHIST) */
static unsigned hist_top;               /* Platz fuer den naechsten        */
static int      overwrite;              /* Einfg: Ueberschreiben (je Zeile) */

static char     ob[3 * LINE + 8];       /* Ausgabe sammeln: ein DosWrite   */
static unsigned on;

static void oc(char c)
{
    if (on < sizeof(ob))
        ob[on++] = c;
}

static void oflush(void)
{
    if (on)
        DosWrite(STDOUT, ob, on);
    on = 0;
}

/* i-ter Befehl rueckwaerts (0 = letzter) */
static char *hist_get(unsigned i)
{
    return hist[(hist_top + NHIST - 1 - i) % NHIST];
}

static void hist_add(const char *l, unsigned n)
{
    unsigned k;
    if (n == 0)
        return;
    if (hist_n) {                       /* gleiche Zeile nicht doppelt  */
        char *last = hist_get(0);
        for (k = 0; k < n && last[k] == l[k]; k++)
            ;
        if (k == n && last[k] == 0)
            return;
    }
    for (k = 0; k < n; k++)
        hist[hist_top][k] = l[k];
    hist[hist_top][n] = 0;
    hist_top = (hist_top + 1) % NHIST;
    if (hist_n < NHIST)
        hist_n++;
}

/* ab pos neu ausgeben, extra alte Zeichen loeschen, Cursor nach pos */
static void redraw(unsigned pos, unsigned n, unsigned extra)
{
    unsigned i;
    for (i = pos; i < n; i++)
        oc(line[i]);
    for (i = 0; i < extra; i++)
        oc(' ');
    for (i = pos; i < n + extra; i++)
        oc('\b');
}

/* ganze Zeile ersetzen; Cursor ans Ende */
static unsigned set_line(const char *src, unsigned *pos, unsigned n)
{
    unsigned k, old = n;
    for (k = 0; k < *pos; k++)
        oc('\b');
    for (k = 0; src[k] && k < LINE; k++)
        line[k] = src[k];
    n = k;
    redraw(0, n, old > n ? old - n : 0);
    for (k = 0; k < n; k++)
        oc(line[k]);
    *pos = n;
    return n;
}

/* ---- Vervollstaendigen mit Tab (wie bash) ------------------------------
   Das Wort vor dem Cursor wird ergaenzt: erstes Wort aus internen Befehlen
   und Programmen (.EXE/.CMD/.BAT) im aktuellen Verzeichnis und im PATH,
   sonst aus Dateien und Verzeichnissen. Eindeutig: ganz einsetzen
   (Verzeichnis mit "\", Datei mit Leerzeichen); mehrdeutig: gemeinsamen
   Anfang einsetzen, zweites Tab listet die Moeglichkeiten. */
#define NCAND 64
#define CANDLEN 64                      /* laengere Namen ergaenzt Tab nicht */
static char     cand[NCAND][CANDLEN];
static unsigned ncand;
static unsigned char cand_dir[NCAND];
static const char *const builtin[] = {
    "BREAK", "CALL", "CD", "CHCP", "CHDIR", "CLS", "COPY", "CTTY", "DATE", "DEL",
    "DETACH", "DIR", "DOSKEY", "ECHO", "ENDLOCAL", "ERASE", "EXIT", "FOR", "GOTO",
    "HILFE", "IF", "KEYS", "LH", "LOADHIGH", "MD", "MKDIR", "PATH", "PAUSE",
    "POWEROFF", "PROMPT", "RD", "REBOOT", "REM", "REN", "RENAME", "RMDIR", "SET",
    "SETLOCAL", "SHIFT", "SHUTDOWN", "START", "TIME", "TRUENAME", "TYPE", "VER",
    "VERIFY", "VOL", 0
};

static void show_prompt(void);
static unsigned slen(const char *s);

static int pfx(const char *name, const char *p, unsigned pl)
{
    unsigned i;
    for (i = 0; i < pl; i++)
        if (up(name[i]) != up(p[i]))
            return 0;
    return 1;
}

static void add_cand(const char *name, int dir)
{
    unsigned i;
    for (i = 0; i < ncand; i++)         /* doppelt (PATH)?               */
        if (same(cand[i], name))
            return;
    if (slen(name) >= CANDLEN)
        return;
    if (ncand < NCAND) {
        for (i = 0; name[i] && i < CANDLEN - 1; i++)
            cand[ncand][i] = name[i];
        cand[ncand][i] = 0;
        cand_dir[ncand++] = (unsigned char)dir;
    }
}

/* Eintraege von dir (0 = aktuelles), die mit p beginnen; progs: nur
   Programme, Endung weg */
static void scan_dir(const char *dir, const char *p, unsigned pl, int progs)
{
    static struct fapi_dirent e;
    static char nm[CANDLEN];
    unsigned n, k;

    for (n = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR; n++) {
        if ((e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2]))) || (e.attr & 0x0E) || !pfx(e.name, p, pl))
            continue;
        if (progs) {
            if (e.attr & 0x10)
                continue;
            for (k = 0; e.name[k] && e.name[k] != '.' && k < CANDLEN - 1; k++)
                nm[k] = e.name[k];
            nm[k] = 0;
            if (!same(e.name + k, ".EXE") && !same(e.name + k, ".CMD") &&
                !same(e.name + k, ".BAT"))
                continue;
            add_cand(nm, 0);
        } else
            add_cand(e.name, (e.attr & 0x10) != 0);
    }
}

static void collect(char *w, unsigned wl, int first, unsigned *plen)
{
    static char dir[FAPI_PATHLEN], pv[256];
    unsigned i, k, d = 0;

    ncand = 0;
    for (i = 0; i < wl; i++)            /* Verzeichnisteil abtrennen     */
        if (w[i] == '\\' || w[i] == ':')
            d = i + 1;
    *plen = wl - d;
    if (first && d == 0) {
        for (i = 0; builtin[i]; i++)
            if (pfx(builtin[i], w, wl))
                add_cand(builtin[i], 0);
        scan_dir(0, w, wl, 1);
        if (DosGetEnv("PATH", pv, sizeof(pv)) != FAPI_ERROR)
            for (i = 0; pv[i]; ) {
                for (k = 0; pv[i] && pv[i] != ';' && k < sizeof(dir) - 1; )
                    dir[k++] = pv[i++];
                dir[k] = 0;
                if (pv[i])
                    i++;
                if (k)
                    scan_dir(dir, w, wl, 1);
            }
        return;
    }
    for (i = 0; i < d && i < sizeof(dir) - 2; i++)
        dir[i] = w[i];
    if (d && dir[d - 1] == '\\' && d > 1 && dir[d - 2] != ':')
        d--;                            /* "\DOS\" -> "\DOS", "\" bleibt */
    dir[d] = 0;
    scan_dir(d ? dir : 0, w + (wl - *plen), *plen, 0);
}

/* line[ws..*pos) durch s ersetzen, Cursor dahinter; 0 = zu lang */
static int put_word(unsigned ws, const char *s, unsigned *n, unsigned *pos)
{
    unsigned k, sl = slen(s), old = *pos - ws, nn;
    if (*n - old + sl > LINE) {
        oc('\a');
        return 0;
    }
    for (k = ws; k < *pos; k++)
        oc('\b');
    if (sl > old)
        for (k = *n; k > *pos; k--)
            line[k - 1 + sl - old] = line[k - 1];
    else
        for (k = *pos; k < *n; k++)
            line[k - old + sl] = line[k];
    for (k = 0; k < sl; k++)
        line[ws + k] = s[k];
    nn = *n - old + sl;
    redraw(ws, nn, old > sl ? old - sl : 0);
    for (k = 0; k < sl; k++)
        oc(line[ws + k]);
    *n = nn;
    *pos = ws + sl;
    return 1;
}

/* Tab gedrueckt; again: direkt davor schon Tab ohne Fortschritt.
   Namen mit Leerzeichen kommen wie bei bash gequotet ins Wort: es
   beginnt dann mit '"', eine eindeutige Datei schliesst mit '"' ab,
   ein Verzeichnis bleibt offen (Tab danach ergaenzt darin weiter). */
static int complete(unsigned *n, unsigned *pos, int again)
{
    static char w[LINE + 1], nw[LINE + 1];
    unsigned ws = 0, pl, i, k, common, wl = 0, d;
    int first = 1, inq = 0, q = 0;

    for (i = 0; i < *pos; i++)          /* Wortanfang: Leerzeichen ausser-
                                           halb von Anfuehrungszeichen   */
        if (line[i] == '"')
            inq = !inq;
        else if (line[i] == ' ' && !inq)
            ws = i + 1;
    for (i = 0; i < ws; i++)
        if (line[i] != ' ')
            first = 0;                  /* es gibt ein Wort davor        */
    for (i = ws; i < *pos; i++)
        if (line[i] == '"')
            q = 1;
        else
            w[wl++] = line[i];
    w[wl] = 0;
    collect(w, wl, first, &pl);
    if (!ncand) {
        oc('\a');
        return 0;
    }
    common = CANDLEN;                   /* gemeinsamer Anfang            */
    for (i = 1; i < ncand; i++) {
        for (k = 0; k < common && cand[0][k] && up(cand[0][k]) == up(cand[i][k]); k++)
            ;
        common = k;
    }
    if (ncand == 1)
        common = slen(cand[0]);
    if (common < pl)
        common = pl;
    d = wl - pl;                        /* Verzeichnisteil, wie getippt  */
    for (i = 0; i < common; i++)
        if (cand[0][i] == ' ')
            q = 1;
    for (i = 0; i < d; i++)
        if (w[i] == ' ')
            q = 1;
    k = 0;                              /* Wort neu: Getipptes in der
                                           Schreibweise des Treffers     */
    if (q)
        nw[k++] = '"';
    for (i = 0; i < d; i++)
        nw[k++] = w[i];
    for (i = 0; i < common && k < LINE - 2; i++)
        nw[k++] = cand[0][i];
    if (ncand == 1) {
        if (cand_dir[0])
            nw[k++] = '\\';
        else {
            if (q)
                nw[k++] = '"';
            nw[k++] = ' ';
        }
    }
    nw[k] = 0;
    if (!put_word(ws, nw, n, pos))
        return 0;
    if (common > pl || ncand == 1)
        return 1;
    if (!again) {
        oc('\a');
        return 0;
    }
    oc('\n');                           /* Moeglichkeiten zeigen         */
    oflush();
    for (i = 0; i < ncand; i++) {
        puts(cand[i]);
        if (cand_dir[i])
            putc('\\');
        for (k = slen(cand[i]) + cand_dir[i]; k < 15 || k < slen(cand[i]) + cand_dir[i] + 2; k++)
            putc(' ');
        if (i % 5 == 4 || i == ncand - 1)
            puts("\n");
    }
    show_prompt();
    for (k = 0; k < *n; k++)
        oc(line[k]);
    for (k = *pos; k < *n; k++)
        oc('\b');
    return 0;
}

/* CTTY: Zeile ueber die Standardeingabe (z. B. COM1), mit Echo */
static void read_line_tty(void)
{
    unsigned n = 0;
    char c;
    for (;;) {
        if (DosRead(STDIN, &c, 1) != 1) {
            if (!n)
                continue;
            break;
        }
        if (c == '\r' || c == '\n')
            break;
        if ((c == 8 || c == 127) && n) {
            n--;
            puts("\b \b");
        } else if ((unsigned char)c >= ' ' && n < LINE) {
            line[n++] = c;
            putc(c);
        }
    }
    line[n] = 0;
    puts("\r\n");
}

static void read_line(void)
{
    unsigned n = 0, pos = 0, k, key, browse = 0;
    unsigned char c, sc;
    int tabbed = 0;

    if (ctty) {
        read_line_tty();
        return;
    }
    overwrite = 0;
    for (;;) {
        oflush();
        key = KbdCharIn();
        c = (unsigned char)key;
        sc = (unsigned char)(key >> 8);
        if (c != 9)
            tabbed = 0;
        if (c == 13) {
            break;
        } else if (c == 0 || c == 0xE0) {   /* Sondertasten         */
            switch (sc) {
            case 0x4B:                      /* links                */
                if (pos) {
                    pos--;
                    oc('\b');
                }
                break;
            case 0x4D:                      /* rechts               */
                if (pos < n)
                    oc(line[pos++]);
                break;
            case 0x47:                      /* Pos1                 */
                while (pos) {
                    pos--;
                    oc('\b');
                }
                break;
            case 0x4F:                      /* Ende                 */
                while (pos < n)
                    oc(line[pos++]);
                break;
            case 0x53:                      /* Entf                 */
                if (pos < n) {
                    for (k = pos; k + 1 < n; k++)
                        line[k] = line[k + 1];
                    n--;
                    redraw(pos, n, 1);
                }
                break;
            case 0x52:                      /* Einfg                */
                overwrite = !overwrite;
                break;
            case 0x48:                      /* auf: aelterer Befehl */
                if (!keys_on)
                    break;
                if (browse < hist_n)
                    n = set_line(hist_get(browse++), &pos, n);
                break;
            case 0x50:                      /* ab: neuerer Befehl   */
                if (browse > 1)
                    n = set_line(hist_get(--browse - 1), &pos, n);
                else if (browse == 1) {
                    browse = 0;
                    n = set_line("", &pos, n);
                }
                break;
            case 0x49:                      /* Bild auf: aeltester  */
                if (hist_n) {
                    browse = hist_n;
                    n = set_line(hist_get(hist_n - 1), &pos, n);
                }
                break;
            case 0x51:                      /* Bild ab: neuester    */
                if (hist_n) {
                    browse = 1;
                    n = set_line(hist_get(0), &pos, n);
                }
                break;
            case 0x3D:                      /* F3: Rest des letzten */
                if (hist_n) {
                    char *l = hist_get(0);
                    for (k = 0; l[k] && k < n; k++)
                        ;
                    while (l[k] && n < LINE) {
                        line[n++] = l[k++];
                        oc(line[n - 1]);
                    }
                    pos = n;
                }
                break;
            }
        } else if (c == 9) {                /* Tab: vervollstaendigen */
            tabbed = complete(&n, &pos, tabbed) ? 0 : 1;
            continue;
        } else if (c == 8) {                /* Rueckschritt         */
            if (pos) {
                pos--;
                oc('\b');
                for (k = pos; k + 1 < n; k++)
                    line[k] = line[k + 1];
                n--;
                redraw(pos, n, 1);
            }
        } else if (c == 27) {               /* Esc: Zeile leeren    */
            browse = 0;
            n = set_line("", &pos, n);
        } else if (c >= 32) {
            if (overwrite && pos < n) {
                line[pos++] = (char)c;
                oc((char)c);
            } else if (n < LINE) {
                for (k = n; k > pos; k--)
                    line[k] = line[k - 1];
                line[pos] = (char)c;
                n++;
                redraw(pos, n, 0);
                oc(line[pos++]);
            }
        }
    }
    oc('\n');
    oflush();
    line[n] = 0;
    hist_add(line, n);
}

/* DOSKEY /HISTORY: gemerkte Befehle zeigen (DOSKEY selbst ist eingebaut) */
static void cmd_doskey(char *arg)
{
    unsigned i;
    if (!*arg) {
        puts("DOSKEY ist eingebaut: Pfeiltasten, Pos1/Ende, Einfg/Entf, F3, Esc, Tab.\n"
             "DOSKEY /HISTORY zeigt die letzten Befehle.\n");
        return;
    }
    for (i = hist_n; i > 1; i--) {      /* ohne diesen Aufruf selbst */
        puts(hist_get(i - 1));
        puts("\n");
    }
}

/* ein Wort aus der Eingabe als eigene Zeichenkette */
static char *word(char *p, char *out, int max)
{
    int i = 0, q = 0;
    while (*p && (q || *p != ' ')) {
        if (*p == '"')
            q = !q;
        else if (i < max - 1)
            out[i++] = *p;
        p++;
    }
    out[i] = 0;
    return out;
}

/* ---- interne Befehle --------------------------------------------------- */
static char *base_name(char *path);
static int has_wild(const char *s);
static int is_dir(char *p);
static int match(const char *name, const char *pat);
static unsigned slen(const char *s);

static void hex4(unsigned v);

static void scopy(char *d, const char *src, unsigned max)
{
    unsigned i;
    for (i = 0; src[i] && i + 1 < max; i++)
        d[i] = src[i];
    d[i] = 0;
}

static void scat(char *d, const char *src, unsigned max)
{
    unsigned n = slen(d);
    if (n < max)
        scopy(d + n, src, max - n);
}

/* JOIN-Verzeichnisse fuer DIR: wie Windows "<JUNCTION>" mit Ziel */
static unsigned long join_mask;         /* Bit d: Laufwerk d eingehaengt */

static void dir_abs(const char *dir, char *out)    /* "C:\\X\\Y"       */
{
    if (dir[0] && dir[1] == ':' && dir[2] == '\\') {
        scopy(out, dir, FAPI_PATHLEN);
        out[0] = up(out[0]);
        return;
    }
    if (dir[0] && dir[1] == ':' && (unsigned)(up(dir[0]) - 'A') != DosQCurDisk()) {
        out[0] = up(dir[0]);
        out[1] = ':';
        out[2] = '\\';
        scopy(out + 3, dir + 2, FAPI_PATHLEN - 3);
        return;
    }
    DosQCurDir(out);
    if (dir[0] && dir[1] == ':')
        dir += 2;
    if (dir[0] == '\\')
        out[2] = 0;
    else if (dir[0] && slen(out) > 3)
        scat(out, "\\", FAPI_PATHLEN);
    scat(out, dir, FAPI_PATHLEN);
}

/* ist adir\name ein JOIN-Verzeichnis? -> Laufwerk + 1, sonst 0 */
static unsigned join_at(const char *adir, const char *name)
{
    static char p[FAPI_PATHLEN], q[FAPI_CURDIRLEN];
    unsigned d, i;
    if (!join_mask)
        return 0;
    scopy(p, adir, sizeof(p));
    if (slen(p) > 3)
        scat(p, "\\", sizeof(p));
    scat(p, name, sizeof(p));
    for (d = 0; d < 26; d++)
        if ((join_mask >> d) & 1 && DosQJoin(d, q) == 0) {
            for (i = 0; p[i] && up(p[i]) == up(q[i]); i++)
                ;
            if (!p[i] && !q[i])
                return d + 1;
        }
    return 0;
}

static void cmd_dir(char *arg)
{
    static struct fapi_dirent e;
    static char dir[FAPI_PATHLEN], cur[FAPI_CURDIRLEN + 2], pat[FAPI_PATHLEN], adir[FAPI_PATHLEN];
    unsigned jd;
    unsigned n, files = 0, dirs = 0, want = 0, nowant = 0, pass;
    unsigned long total = 0;
    int all = 0, page = 0, lines = 4, bare = 0, wide = 0;
    unsigned wcol = 0;

    dir[0] = 0;
    for (; *arg; arg = next_word(arg)) {
        if (arg[0] == '/' && up(arg[1]) == 'A') {   /* /A, /A:H, /A-D ...  */
            char *q = arg + 2;
            int neg = 0;
            all = 1;
            if (*q == ':')
                q++;
            for (; *q && *q != ' '; q++) {
                unsigned b = up(*q) == 'H' ? 0x02 : up(*q) == 'S' ? 0x04 :
                             up(*q) == 'D' ? 0x10 : up(*q) == 'R' ? 0x01 :
                             up(*q) == 'A' ? 0x20 : 0;
                if (*q == '-')
                    neg = 1;
                else if (b) {
                    if (neg)
                        nowant |= b;
                    else
                        want |= b;
                    neg = 0;
                }
            }
        } else if (same(arg, "/P"))
            page = 1;
        else if (same(arg, "/B"))
            bare = 1;                   /* nur Namen                     */
        else if (same(arg, "/W"))
            wide = 1;                   /* breit, Verzeichnisse in [ ]   */
        else
            word(arg, dir, sizeof(dir));
    }
    pat[0] = 0;
    if (dir[0] && (has_wild(dir) || !is_dir(dir))) {    /* DIR \DOS\*.EXE */
        char *b = base_name(dir);
        for (n = 0; b[n] && n < sizeof(pat) - 1; n++)
            pat[n] = b[n];
        pat[n] = 0;
        *b = 0;
        n = slen(dir);                          /* "\DOS\" -> "\DOS"   */
        if (n > 1 && dir[n - 1] == '\\' && dir[n - 2] != ':')
            dir[n - 1] = 0;
    }
    if (dir[0] && dir[1] == ':') {          /* Laufwerk gibt es?         */
        static struct fapi_fsinfo fi;
        static char sb[FAPI_CURDIRLEN];
        unsigned dv = (unsigned)(up(dir[0]) - 'A');
        if (dv >= 26 || (FAPI_FAILED(DosQFSInfo(dv, &fi)) && DosQSubst(dv, sb) != 0)) {
            puts("Ung\x81ltiges Laufwerk.\n");
            return;
        }
    }
    if (DosFindEntry(&e, 0, dir) == FAPI_ERR_NOTREADY) {
        puts("Laufwerk nicht bereit - keine Diskette eingelegt.\n");
        return;
    }
    n = slen(dir);                          /* Stamm ("D:", "D:\\", "\\")
                                               gibt es immer, auch leer */
    if (dir[0] && !(n <= 3 && (dir[n - 1] == ':' || dir[n - 1] == '\\')) &&
        DosFindEntry(&e, 0, dir) == FAPI_ERROR) {
        puts("Verzeichnis nicht gefunden.\n");
        return;
    }
    if (!bare)
    {                                       /* Kopf wie MS-DOS          */
        static struct fapi_label lb;
        unsigned dv = (dir[0] && dir[1] == ':') ? (unsigned)(up(dir[0]) - 'A') : DosQCurDisk();
        if (DosLabel(dv, &lb, 0) == 0) {
            puts("\n Datentr\x84ger in Laufwerk ");
            putc((char)('A' + dv));
            puts(lb.label[0] ? " ist " : " hat keine Bezeichnung");
            puts(lb.label);
            if (lb.serial) {
                puts("\n Datentr\x84gernummer: ");
                hex4((unsigned)(lb.serial >> 16));
                putc('-');
                hex4((unsigned)lb.serial);
            }
        }
    }
    if (!bare) {
    puts("\n Verzeichnis von ");
    if (dir[0] && dir[1] == ':') {
        dir[0] = up(dir[0]);
        puts(dir);
        if (!dir[2])
            putc('\\');                    /* "D:" -> "D:\\"           */
    } else {
        DosQCurDir(cur);
        if (dir[0] == '\\') {                  /* absolut: nur Laufwerk */
            cur[2] = 0;
        } else if (dir[0] && cur[3]) {
            n = slen(cur);
            cur[n] = '\\';
            cur[n + 1] = 0;
        }
        puts(cur);
        puts(dir);
    }
    puts("\n\n");
    }
    {
        static char q[FAPI_CURDIRLEN];
        join_mask = 0;
        for (n = 0; n < 26; n++)
            if (DosQJoin(n, q) == 0)
                join_mask |= 1UL << n;
        if (join_mask)
            dir_abs(dir, adir);
    }
    for (pass = 0; pass < 2; pass++)            /* Verzeichnisse zuerst */
    for (n = 0; DosFindEntry(&e, n, dir[0] ? dir : 0) != FAPI_ERROR; n++) {
        if (((e.attr & 0x10) != 0) != (pass == 0))
            continue;
        if (!all && (e.attr & 0x06) && !(e.name[0] == '.' &&
            (!e.name[1] || (e.name[1] == '.' && !e.name[2]))))
            continue;                           /* versteckt, System - "."
                                                   und ".." immer (macOS
                                                   legt sie versteckt an) */
        if ((e.attr & want) != want || (e.attr & nowant))
            continue;
        if (pat[0] && !match(e.name, pat))
            continue;
        if (e.attr & 0x10)
            dirs++;
        else {
            files++;
            total += e.size;
        }
        if (bare) {                             /* DIR /B                */
            if (e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2])))
                continue;
            puts(e.name);
            puts("\n");
            continue;
        }
        if (wide) {                             /* DIR /W: Spalten zu 16 */
            unsigned l = slen(e.name) + ((e.attr & 0x10) ? 2 : 0);
            if (wcol && wcol + l > 79) {
                puts("\n");
                wcol = 0;
            }
            if (e.attr & 0x10)
                putc('[');
            puts(e.name);
            if (e.attr & 0x10)
                putc(']');
            wcol += l;
            for (; wcol % 16; wcol++)
                putc(' ');
            if (wcol >= 80) {
                puts("\n");
                wcol = 0;
            }
            continue;
        }
        put2(e.date & 31);
        putc('.');
        put2((e.date >> 5) & 15);
        putc('.');
        put_numx(1980 + (e.date >> 9), 4, 0);
        puts("  ");
        put2(e.time >> 11);
        putc(':');
        put2((e.time >> 5) & 63);
        jd = (e.attr & 0x10) ? join_at(adir, e.name) : 0;
        if (jd)
            puts("    <JUNCTION>    ");
        else if (e.attr & 0x10)
            puts("    <DIR>         ");
        else
            put_num(e.size, 18);
        putc(' ');
        puts(e.name);
        if (jd) {                               /* Ziel wie bei Windows  */
            puts(" [");
            putc((char)('A' + jd - 1));
            puts(":\\]");
        }
        puts("\n");
        if (page && ++lines >= VioGetRows() - 1) {  /* eine Bildschirmseite */
            puts("Weiter mit beliebiger Taste . . .");
            KbdCharIn();
            puts("\n");
            lines = 0;
        }
    }
    if (wcol)
        puts("\n");
    if (pat[0] && !files && !dirs) {
        if (!bare)
            puts("Datei nicht gefunden.\n\n");
        return;
    }
    if (bare)
        return;
    if (wide)
        puts("\n");
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

    if (!*arg || (arg[1] == ':' && !*skip(arg + 2))) {    /* CD, CD A:   */
        if (*arg ? DosQCurDirD(up(arg[0]) - 'A', cur) != 0 : DosQCurDir(cur) != 0) {
            puts("Ung\x81ltiges Laufwerk.\n");
            return;
        }
        puts(cur);
        puts("\n");
        return;
    }
    if (*arg == '"')
        word(arg, p, sizeof(p));
    else {                              /* wie Windows: CD Neuer Ordner  */
        unsigned n;                     /* nimmt den Rest der Zeile      */
        for (n = 0; arg[n] && n < sizeof(p) - 1; n++)
            p[n] = arg[n];
        while (n && p[n - 1] == ' ')
            n--;
        p[n] = 0;
    }
    if (DosChDir(p) == FAPI_ERROR)
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
    if (FAPI_FAILED(h)) {
        puts(h == FAPI_ERR_INUSE ? "Datei wird gerade von einem anderen Programm geschrieben.\n" :
             h == FAPI_ERR_NOTREADY ? "Laufwerk nicht bereit - keine Diskette eingelegt.\n"
                                 : "Datei nicht gefunden.\n");
        return;
    }
    while ((n = DosRead(h, buf, sizeof(buf))) != 0 && n != FAPI_ERROR) {
        DosWrite(STDOUT, buf, n);
        last = buf[n - 1];
    }
    DosClose(h);
    if (last != '\n')
        puts("\n");
}

static char *base_name(char *path);

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
    case FAPI_ERR_NOTREADY: puts("Laufwerk oder Ger\x84t nicht bereit (keine Diskette, Drucker aus?).\n"); break;
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
    {                                   /* Endung: nach dem letzten Punkt */
        const char *x;
        for (x = name; *x; x++)
            if (*x == '.')
                nd = x;
        if (*nd != '.')
            nd = x;
    }
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

static char cbuf[4096];                 /* Kopieren, Stapeldatei laden */

/* Datum/Uhrzeit einer Datei (Eintrag im Verzeichnis); 0 = gefunden */
static int file_stamp(const char *path, unsigned short *date, unsigned short *time)
{
    static struct fapi_dirent e;
    static char dir[FAPI_PATHLEN];
    char *nm = base_name((char *)path);
    unsigned dl = (unsigned)(nm - path), n;
    for (n = 0; n < dl; n++)
        dir[n] = path[n];
    dir[dl] = 0;
    for (n = 0; DosFindEntry(&e, n, dl ? dir : 0) != FAPI_ERROR; n++)
        if (same(nm, e.name)) {          /* e.name gross */
            *date = e.date;
            *time = e.time;
            return 0;
        }
    return -1;
}

static int copy_data(unsigned hs, unsigned hd);

/* eine Datei kopieren, Datum/Uhrzeit wie die Quelle (MS-DOS); 0 = ok */
static int copy_one(const char *src, const char *dst)
{
    static struct fapi_finfo fi;
    unsigned hs, hd, n, w;

    if (same_path(src, dst)) {
        puts("Datei kann nicht auf sich selbst kopiert werden.\n");
        return -1;
    }
    hs = DosOpen(src);
    if (FAPI_FAILED(hs)) {
        file_error(hs == FAPI_ERR_NOTREADY ? hs : FAPI_ERROR);
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
    if (file_stamp(src, &fi.date, &fi.time) == 0) {
        fi.mask = 2;
        DosSetFInfo((char *)dst, &fi);
    }
    return 0;
}

/* Inhalt von hs an hd anhaengen; 0 = ok */
static int copy_data(unsigned hs, unsigned hd)
{
    unsigned n, w;
    while ((n = DosRead(hs, cbuf, sizeof(cbuf))) != 0) {
        if (n == FAPI_ERROR) {
            puts("Lesefehler.\n");
            return -1;
        }
        w = DosWrite(hd, cbuf, n);
        if (FAPI_FAILED(w) || w != n) {
            file_error(FAPI_FAILED(w) ? w : FAPI_ERR_FULL);
            return -1;
        }
    }
    return 0;
}

/* COPY A+B[+C] [Ziel]: ohne Ziel wird an A angehaengt (wie MS-DOS) */
static void copy_concat(char *list, char *dst)
{
    static char part[FAPI_PATHLEN];
    char *p = list, *q;
    unsigned hd, hs, k, first = 1, n = 0;

    for (q = list; *q && *q != '+'; q++)
        ;
    k = (unsigned)(q - list);
    if (!dst[0]) {                              /* an die erste anhaengen */
        for (n = 0; n < k; n++)
            dst[n] = list[n];
        dst[k] = 0;
        hd = DosAppend(dst);
        p = *q ? q + 1 : q;
        first = 0;
    } else
        hd = DosCreate(dst);
    if (FAPI_FAILED(hd)) {
        file_error(hd);
        return;
    }
    n = 0;
    while (*p) {
        for (q = p, k = 0; *q && *q != '+' && k < sizeof(part) - 1; q++)
            part[k++] = *q;
        part[k] = 0;
        p = *q ? q + 1 : q;
        if (!k)
            continue;
        if (same_path(part, dst) && !first) {
            puts("Inhalt der Zieldatei ging vor dem Kopieren verloren.\n");
            continue;
        }
        hs = DosOpen(part);
        if (FAPI_FAILED(hs)) {
            puts(part);
            puts(": Datei nicht gefunden.\n");
            continue;
        }
        puts(part);
        puts("\n");
        k = copy_data(hs, hd);
        DosClose(hs);
        if (k)
            break;
        n++;
    }
    DosClose(hd);
    put_numx(n ? 1 : 0, 8, 0);
    puts(" Datei(en) kopiert\n");
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

    {                                           /* "A + B" -> "A+B"     */
        char *r = arg, *w = arg;
        for (; *r; r++) {
            if (*r == ' ' && (r[1] == '+' || (w > arg && w[-1] == '+')))
                continue;
            *w++ = *r;
        }
        *w = 0;
    }
    word(arg, src, sizeof(src));
    word(next_word(arg), dst, sizeof(dst));
    if (!src[0]) {
        puts("Syntax: COPY Quelle [Ziel], COPY A+B [Ziel]\n");
        return;
    }
    for (n = 0; src[n] && src[n] != '+'; n++)
        ;
    if (src[n]) {                               /* Verketten            */
        copy_concat(src, dst);
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
         "  DIR [Pfad] [/A[:HSDRA]] [/P]  Dateien, Verzeichnisse zuerst (/A alle,\n"
         "                  /A:H nur versteckte, /A:-D ohne Verzeichnisse; /P Seiten)\n"
         "  CD [Pfad]       Verzeichnis wechseln oder anzeigen\n"
         "  C:              Laufwerk wechseln\n"
         "  TYPE Datei      Textdatei ausgeben\n"
         "  COPY Quelle [Ziel]  Dateien kopieren (auch * und ?)\n"
         "  DEL Datei       Dateien l\x94schen (auch * und ?)\n"
         "  MD / RD Verz.   Verzeichnis anlegen / entfernen\n"
         "  REN Datei Name  Datei oder Verzeichnis umbenennen\n"
         "  SHUTDOWN [/H|/R] herunterfahren (/H anhalten, /R neu starten), auch POWEROFF\n"
         "  REBOOT          neu starten (wie SHUTDOWN /R)\n"
         "  CLS             Bildschirm l\x94schen\n"
         "  VER / VOL       Version / Datentr\x84gerbezeichnung anzeigen\n"
         "  DATE / TIME     Datum / Uhrzeit anzeigen und stellen\n"
         "  DOSKEY /HISTORY letzte Befehle; Pfeil auf/ab holt sie zur\x81" "ck, F3, Einfg\n"
         "  ECHO Text       Text ausgeben\n"
         "  START Programm  Programm in eigener Sitzung starten, DETACH: im Hintergrund\n"
         "  SET [Name=Wert] Umgebung anzeigen/setzen, PATH [Pfade], PROMPT [Text]\n"
         "  Stapeldateien:  ECHO [ON|OFF], ECHO., @, REM, PAUSE, GOTO [:EOF], CALL,\n"
         "                  SHIFT, %0-%9, %NAME%, IF [NOT] ERRORLEVEL|EXIST|a==b,\n"
         "                  FOR %%v IN (Satz) DO, SETLOCAL/ENDLOCAL, EXTPROC\n"
         "  a & b, a && b, a || b  nacheinander / nur bei Erfolg / nur bei Fehler\n"
         "  TRUENAME, BREAK, VERIFY, CHCP, CTTY, KEYS [ON|OFF|LIST], LH\n"
         "  EXIT            Befehlsinterpreter beenden\n"
         "Programme in \\DOS (HELP Befehl zeigt die Hilfe dazu):\n"
         "  ANSI ATTRIB CHKDSK CHOICE COMP COMPRESS CUT DEFRAG DELTREE DISKCOMP\n"
         "  DISKCOPY EDIT EXPAND\n"
         "  FC FDISK FIND\n"
         "  FM FORMAT GREP JOIN KEYB LABEL LESS MEM MODE MORE MOVE MSD PRINT SCANDISK\n"
         "  REPLACE SORT SUBST SYS TREE UNDELETE XCOPY\n"
         "Andere Eingaben starten das gleichnamige Programm, z. B. MEM.\n"
         "Alt+Esc wechselt die Sitzung, Strg+C bricht ein Programm ab,\n"
         "Strg+Alt+Entf schlie\xE1t alle Dateien und startet neu.\n");
}

/* ---- VOL, DATE, TIME -------------------------------------------------- */
static void hex4(unsigned v)
{
    int i;
    for (i = 12; i >= 0; i -= 4)
        putc("0123456789ABCDEF"[(v >> i) & 15]);
}

static void cmd_vol(char *arg)
{
    static struct fapi_label lb;
    unsigned d = DosQCurDisk(), n;
    if (arg[0] && arg[1] == ':')
        d = (unsigned)(up(arg[0]) - 'A');
    if ((n = DosLabel(d, &lb, 0)) != 0) {
        puts(n == FAPI_ERR_NOTREADY ? "Laufwerk nicht bereit - keine Diskette eingelegt.\n"
                                    : "Ung\x81ltiges Laufwerk.\n");
        return;
    }
    puts(" Datentr\x84ger in Laufwerk ");
    putc((char)('A' + d));
    puts(lb.label[0] ? " ist " : " hat keine Bezeichnung");
    puts(lb.label);
    puts("\n");
    if (lb.serial) {
        puts(" Datentr\x84gernummer: ");
        hex4((unsigned)(lb.serial >> 16));
        putc('-');
        hex4((unsigned)lb.serial);
        puts("\n");
    }
}

/* Zahlen aus "12.10.26" oder "16:05:30" -> v[0..2]; -> Anzahl */
static int nums(const char *p, unsigned *v)
{
    int n = 0;
    while (*p == ' ')
        p++;
    while (n < 3 && *p >= '0' && *p <= '9') {
        v[n] = 0;
        while (*p >= '0' && *p <= '9')
            v[n] = v[n] * 10 + (unsigned)(*p++ - '0');
        n++;
        if (*p == '.' || *p == ':' || *p == '-' || *p == '/' || *p == ',')
            p++;
        else
            break;
    }
    return n;
}

static char *ask_line(const char *prompt)
{
    unsigned n;
    puts(prompt);
    n = DosRead(STDIN, buf, sizeof(buf) - 1);
    if (FAPI_FAILED(n))
        n = 0;
    buf[n] = 0;
    for (n = 0; buf[n] && buf[n] != '\r' && buf[n] != '\n'; n++)
        ;
    buf[n] = 0;
    return buf;
}

static void cmd_date(char *arg)
{
    static unsigned short dt[2];
    static const char *wd[] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" };
    unsigned v[3], y, m, d, k, j;
    int w;
    char *p = arg;

    DosGetDateTime(dt);
    if (!*p) {
        d = dt[0] & 31; m = (dt[0] >> 5) & 15; y = 1980 + (dt[0] >> 9);
        k = m < 3 ? y - 1 : y;          /* Wochentag (Zeller)          */
        j = m < 3 ? m + 12 : m;
        w = (int)((d + (13 * (j + 1)) / 5 + k % 100 + (k % 100) / 4 + (k / 100) / 4 + 5 * (k / 100)) % 7);
        puts("Aktuelles Datum: ");
        puts(wd[(w + 6) % 7]);
        putc(' ');
        put2(d); putc('.'); put2(m); putc('.');
        put_numx(y, 4, 0);
        p = ask_line("\nNeues Datum eingeben (TT.MM.JJ): ");
        if (!*p)
            return;
    }
    if (nums(p, v) != 3 || v[0] < 1 || v[0] > 31 || v[1] < 1 || v[1] > 12) {
        puts("Ung\x81ltiges Datum\n");
        return;
    }
    y = v[2] < 100 ? (v[2] < 80 ? 2000 + v[2] : 1900 + v[2]) : v[2];
    if (y < 1980 || y > 2099) {
        puts("Ung\x81ltiges Datum\n");
        return;
    }
    DosGetDateTime(dt);
    dt[0] = (unsigned short)(((y - 1980) << 9) | (v[1] << 5) | v[0]);
    if (DosSetDateTime(dt))
        puts("Ung\x81ltiges Datum\n");
}

static void cmd_time(char *arg)
{
    static unsigned short dt[2];
    unsigned v[3];
    int n;
    char *p = arg;

    DosGetDateTime(dt);
    if (!*p) {
        puts("Aktuelle Uhrzeit: ");
        put2(dt[1] >> 11); putc(':'); put2((dt[1] >> 5) & 63); putc(':');
        put2((dt[1] & 31) << 1);
        p = ask_line("\nNeue Uhrzeit eingeben: ");
        if (!*p)
            return;
    }
    v[2] = 0;
    n = nums(p, v);
    if (n < 2 || v[0] > 23 || v[1] > 59 || v[2] > 59) {
        puts("Ung\x81ltige Uhrzeit\n");
        return;
    }
    DosGetDateTime(dt);
    dt[1] = (unsigned short)((v[0] << 11) | (v[1] << 5) | (v[2] >> 1));
    if (DosSetDateTime(dt))
        puts("Ung\x81ltige Uhrzeit\n");
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

/* ---- Umgebung, Prompt, Programmsuche, Stapeldateien -------------------- */
#define CMDLEN 256
static int echo_on = 1;
static char pbuf[FAPI_PATHLEN], ebuf[CMDLEN];

static int execute(char *l);
static int execute_line(char *l);

static int exists(const char *p)
{
    unsigned h = DosOpen(p);
    if (FAPI_FAILED(h))
        return 0;
    DosClose(h);
    return 1;
}

static void cmd_set(char *arg)
{
    static char e[FAPI_ENVSIZE];
    unsigned i, r;
    char *q;

    if (!*arg) {
        for (i = 0; DosEnvEntry(i, e, sizeof(e)) != FAPI_ERROR; i++) {
            puts(e);
            puts("\n");
        }
        return;
    }
    for (q = arg; *q && *q != '='; q++)
        ;
    if (!*q) {
        puts("Syntaxfehler.\n");
        return;
    }
    r = DosSetEnv(arg);
    if (r == FAPI_ERR_FULL)
        puts("Kein Platz mehr in der Umgebung.\n");
    else if (r)
        puts("Syntaxfehler.\n");
}

/* NAME=arg setzen (PATH, PROMPT); leeres arg loescht */
static void set_var(const char *name, const char *arg)
{
    unsigned i = 0;
    while (*name)
        ebuf[i++] = *name++;
    ebuf[i++] = '=';
    while (*arg && i < CMDLEN - 1)
        ebuf[i++] = *arg++;
    ebuf[i] = 0;
    if (DosSetEnv(ebuf) == FAPI_ERR_FULL)
        puts("Kein Platz mehr in der Umgebung.\n");
}

static void show_prompt(void)
{
    static char pr[128], cur[FAPI_CURDIRLEN];
    static unsigned dt[2];
    char *p;

    if (DosGetEnv("PROMPT", pr, sizeof(pr)) == FAPI_ERROR)
        pr[0] = 0;
    if (!pr[0]) {
        pr[0] = '$'; pr[1] = 'P'; pr[2] = '$'; pr[3] = 'G'; pr[4] = 0;
    }
    for (p = pr; *p; p++) {
        if (*p != '$' || !p[1]) {
            putc(*p);
            continue;
        }
        switch (up(*++p)) {
        case 'P': DosQCurDir(cur); puts(cur); break;
        case 'N': putc((char)('A' + DosQCurDisk())); break;
        case 'G': putc('>'); break;
        case 'L': putc('<'); break;
        case 'B': putc('|'); break;
        case 'Q': putc('='); break;
        case '$': putc('$'); break;
        case '_': puts("\n"); break;
        case 'H': puts("\b \b"); break;
        case 'V': puts("AT-DOS 5.0"); break;
        case 'E': putc(27); break;      /* ESC, fuer ANSI-Folgen        */
        case 'D':
            DosGetDateTime(dt);
            put2(dt[0] & 31); putc('.'); put2((dt[0] >> 5) & 15); putc('.');
            put_numx(1980 + (dt[0] >> 9), 4, 0);
            break;
        case 'T':
            DosGetDateTime(dt);
            put2(dt[1] >> 11); putc(':'); put2((dt[1] >> 5) & 63); putc(':');
            put2((dt[1] & 31) * 2);
            break;
        }
    }
}

/* Programm suchen wie COMMAND.COM: aktuelles Verzeichnis, dann PATH; je
   Verzeichnis .EXE, .CMD, .BAT, .COM. -> 0 nicht gefunden, 1 EXE,
   2 Stapeldatei (.CMD nativ; .BAT gehoert ab 6.0 der DOS-Box), 3 COM */
static int find_program(const char *w, unsigned wl, char *out)
{
    static const char *const ext[4] = { ".EXE", ".CMD", ".BAT", ".COM" };
    static const char type[4] = { 1, 2, 2, 3 };
    static char dir[FAPI_PATHLEN];
    const char *pp = 0;
    unsigned i, k, n, dl = 0;
    int has_ext = 0, has_path = 0, e;

    for (i = 0; i < wl; i++) {
        if (w[i] == '\\' || w[i] == ':') {
            has_path = 1;
            has_ext = 0;
        } else if (w[i] == '.')
            has_ext = 1;
    }
    if (!has_path && DosGetEnv("PATH", pbuf, sizeof(pbuf)) != FAPI_ERROR)
        pp = pbuf;
    for (;;) {                          /* erst "", dann PATH-Eintraege */
        for (k = 0; k < dl; k++)
            out[k] = dir[k];
        if (dl && out[dl - 1] != '\\' && out[dl - 1] != ':')
            out[k++] = '\\';
        for (i = 0; i < wl && k < FAPI_PATHLEN - 5; i++)
            out[k++] = w[i];
        out[k] = 0;
        if (has_ext) {
            if (exists(out)) {
                for (e = 0; e < 4; e++) {
                    for (i = 0; i < 4 && up(out[k - 4 + i]) == ext[e][i]; i++)
                        ;
                    if (i == 4)
                        return type[e];
                }
                return 1;               /* andere Endung: als EXE versuchen */
            }
        } else
            for (e = 0; e < 4; e++) {
                for (i = 0; i < 4; i++)
                    out[k + i] = ext[e][i];
                out[k + 4] = 0;
                if (exists(out))
                    return type[e];
            }
        if (!pp || !*pp)
            return 0;
        for (n = 0; *pp && *pp != ';' && n < FAPI_PATHLEN - 14; n++)
            dir[n] = *pp++;
        while (*pp == ';')
            pp++;
        dl = n;
        if (dl == 0)
            continue;
    }
}

/* ---- Stapeldateien ----------------------------------------------------- */
#define BMAX 4
static struct batch {
    unsigned sel, len, pos;
    char     argbuf[FAPI_ARGLEN + FAPI_PATHLEN];
    char    *argv[10];
    int      argc, shift;
} bat[BMAX];
static int bdepth, batch_stop, batch_abort;
static int run_chain(char *l);
static int run_program(char *cmd, int sync, int call);
static int confirm(const char *q);
static void endlocal(void);
static int nlocal, local_lvl[4];

static void on_break(unsigned sig)
{
    (void)sig;
    break_req = 1;
}

static char __far *bdata(struct batch *b)
{
    return MK_FP(b->sel, 0);
}

/* naechste Zeile nach out; 0 = Ende */
static int batch_line(struct batch *b, char *out)
{
    char __far *d = bdata(b);
    unsigned n = 0;
    if (b->pos >= b->len)
        return 0;
    while (b->pos < b->len && d[b->pos] != '\n' && d[b->pos] != 0x1A) {
        if (d[b->pos] != '\r' && n < CMDLEN - 1)
            out[n++] = d[b->pos];
        b->pos++;
    }
    if (b->pos < b->len && d[b->pos] == 0x1A)
        b->pos = b->len;
    else
        b->pos++;
    out[n] = 0;
    return 1;
}

/* %0..%9, %NAME%, %% ersetzen: in -> ebuf */
static void expand(struct batch *b, const char *in)
{
    static char val[FAPI_PATHLEN], nm[64];
    unsigned o = 0, k;
    const char *s, *q;

    while (*in && o < CMDLEN - 1) {
        if (*in != '%') {
            ebuf[o++] = *in++;
            continue;
        }
        in++;
        if (*in == '%') {
            ebuf[o++] = '%';
            in++;
        } else if (*in >= '0' && *in <= '9') {
            k = (unsigned)(*in++ - '0') + (unsigned)b->shift;
            if ((int)k < b->argc)
                for (s = b->argv[k]; *s && o < CMDLEN - 1; )
                    ebuf[o++] = *s++;
        } else {
            for (q = in; *q && *q != '%' && *q != ' '; q++)
                ;
            if (*q != '%' || q == in || q - in >= (int)sizeof(nm)) {
                ebuf[o++] = '%';        /* kein Variablenname: so lassen */
                continue;
            }
            for (k = 0; in + k < q; k++)
                nm[k] = in[k];
            nm[k] = 0;
            if (DosGetEnv(nm, val, sizeof(val)) != FAPI_ERROR)
                for (s = val; *s && o < CMDLEN - 1; )
                    ebuf[o++] = *s++;
            in = q + 1;
        }
    }
    ebuf[o] = 0;
}

static void cmd_goto(struct batch *b, char *label)
{
    static char l[CMDLEN];
    char *p;
    unsigned i;

    if (*label == ':')
        label++;
    if (same(label, "EOF")) {           /* wie NT: an das Dateiende     */
        b->pos = b->len;
        return;
    }
    b->pos = 0;
    while (batch_line(b, l)) {
        p = skip(l);
        if (*p != ':')
            continue;
        p++;
        for (i = 0; label[i] && label[i] != ' ' && up(label[i]) == up(p[i]); i++)
            ;
        if ((!label[i] || label[i] == ' ') && (!p[i] || p[i] == ' '))
            return;
    }
    puts("Sprungmarke nicht gefunden.\n");
    batch_stop = 1;
}

/* IF [NOT] ERRORLEVEL n | EXIST datei | a==b  befehl  -> 0 = EXIT */
static int cmd_if(char *arg)
{
    static char w[FAPI_PATHLEN];
    int neg = 0, cond;
    char *p = arg, *q;
    unsigned n, i;

    if (same(p, "NOT")) {
        neg = 1;
        p = next_word(p);
    }
    if (same(p, "ERRORLEVEL")) {
        p = next_word(p);
        for (n = 0; *p >= '0' && *p <= '9'; p++)
            n = n * 10 + (unsigned)(*p - '0');
        cond = errorlevel >= n;         /* wie MS-DOS: n oder groesser  */
        p = skip(p);
    } else if (same(p, "EXIST")) {
        p = next_word(p);
        word(p, w, sizeof(w));
        cond = exists(w);
        p = next_word(p);
    } else {
        for (q = p; *q && !(q[0] == '=' && q[1] == '='); q++)
            ;
        if (!*q) {
            puts("Syntaxfehler.\n");
            return 1;
        }
        n = (unsigned)(q - p);
        q += 2;
        for (i = 0; q[i] && q[i] != ' '; i++)
            ;
        cond = i == n;
        for (i = 0; cond && i < n; i++)
            cond = p[i] == q[i];
        p = next_word(q);
    }
    if (cond != neg && *p)
        return execute(p);
    return 1;
}

static int run_batch(const char *path, const char *args)
{
    static char l[CMDLEN];
    struct batch *b;
    unsigned h, n, total = 0, sel, i;
    char *p, __far *d;
    int rc = 1, quiet;

    if (bdepth == BMAX) {
        puts("Zu viele verschachtelte Stapeldateien.\n");
        return 1;
    }
    h = DosOpen(path);                  /* Laenge bestimmen             */
    if (FAPI_FAILED(h)) {
        puts("Stapeldatei nicht gefunden.\n");
        return 1;
    }
    while ((n = DosRead(h, cbuf, sizeof(cbuf))) != 0 && n != FAPI_ERROR)
        total += n;
    DosClose(h);
    sel = DosAllocSeg(total ? total : 1);
    if (sel == FAPI_ERROR || sel == 0) {
        puts("Nicht gen\x81gend Arbeitsspeicher.\n");
        return 1;
    }
    b = &bat[bdepth];
    b->sel = sel;
    b->len = total;
    b->pos = 0;
    b->shift = 0;
    d = bdata(b);
    h = DosOpen(path);
    for (total = 0; !FAPI_FAILED(h) &&
         (n = DosRead(h, cbuf, sizeof(cbuf))) != 0 && n != FAPI_ERROR; total += n)
        for (i = 0; i < n && total + i < b->len; i++)
            d[total + i] = cbuf[i];
    if (!FAPI_FAILED(h))
        DosClose(h);
    /* %0 = Name der Datei, %1.. = Argumente */
    p = b->argbuf;
    b->argc = 0;
    for (i = 0; path[i] && i < FAPI_PATHLEN - 1; i++)
        *p++ = path[i];
    *p++ = 0;
    b->argv[b->argc++] = b->argbuf;
    for (args = skip((char *)args); *args && b->argc < 10; args = skip((char *)args)) {
        int q = 0;                      /* "Lange Namen" bleiben ein Wort */
        b->argv[b->argc++] = p;
        while (*args && (q || *args != ' ') && p < b->argbuf + sizeof(b->argbuf) - 1) {
            if (*args == '"')
                q = !q;
            *p++ = *args++;
        }
        *p++ = 0;
    }
    /* OS/2: EXTPROC prog in der ersten Zeile - prog bearbeitet die Datei */
    if (batch_line(b, l) && same(skip(l), "EXTPROC")) {
        static char xc[CMDLEN];
        unsigned k = 0;
        char *x = next_word(skip(l));
        DosFreeSeg(sel);
        for (; *x && k < CMDLEN - 2; )
            xc[k++] = *x++;
        xc[k++] = ' ';
        for (i = 0; path[i] && k < CMDLEN - 2; i++)
            xc[k++] = path[i];
        if (*args || b->argc > 1) {
            for (i = 1; (int)i < b->argc; i++) {
                xc[k++] = ' ';
                for (x = b->argv[i]; *x && k < CMDLEN - 1; )
                    xc[k++] = *x++;
            }
        }
        xc[k] = 0;
        return run_program(xc, 1, 1);
    }
    b->pos = 0;
    if (bdepth == 0) {
        break_req = 0;
        DosSetSigHandler(on_break, SIG_BREAK);
    }
    bdepth++;
    while (!batch_stop && batch_line(b, l)) {
        if (break_req || (ran_prog && errorlevel == 255)) {
            break_req = 0;
            ran_prog = 0;
            if (confirm("Stapelverarbeitung abbrechen (J/N)? ")) {
                batch_abort = 1;
                break;
            }
        }
        if (batch_abort)
            break;
        p = skip(l);
        quiet = *p == '@';
        if (quiet)
            p = skip(p + 1);
        if (!*p || *p == ':')
            continue;                   /* Leerzeile, Sprungmarke       */
        expand(b, p);
        if (echo_on && !quiet) {
            show_prompt();
            puts(ebuf);
            puts("\n");
        }
        for (i = 0; ebuf[i]; i++)
            l[i] = ebuf[i];
        l[i] = 0;
        ran_prog = 0;
        if (!run_chain(l)) {
            rc = 0;
            break;
        }
    }
    batch_stop = 0;
    bdepth--;
    DosFreeSeg(sel);
    while (nlocal && local_lvl[nlocal - 1] > bdepth)
        endlocal();                     /* wie OS/2: am Dateiende       */
    if (bdepth == 0) {
        echo_on = 1;                    /* wie MS-DOS: an der Eingabe wieder an */
        batch_abort = 0;
        F2(FAPI_SETSIGHANDLER, 0, 0);
    }
    return rc;
}

/* ---- Verketten wie OS/2 CMD: a & b, a && b, a || b, ^ maskiert -------- */
static int run_chain(char *l)
{
    static char part[3][CMDLEN];
    static int depth;
    char *d;
    unsigned i = 0;
    int q = 0, op = 0, next, run, rc = 1;

    if (depth >= 3)
        return execute_line(l);
    d = part[depth++];
    for (;;) {
        char c = *l;
        next = -1;
        if (c == 0)
            next = 0;
        else if (c == '"') {
            q = !q;
        } else if (c == '^' && l[1] && !q) {
            l++;
            c = *l;
            if (i < CMDLEN - 1)
                d[i++] = c;
            l++;
            continue;
        } else if (!q && c == '&') {
            next = l[1] == '&' ? 2 : 1;
            l += next;
        } else if (!q && c == '|' && l[1] == '|') {
            next = 3;
            l += 2;
        }
        if (next < 0) {
            if (i < CMDLEN - 1)
                d[i++] = c;
            l++;
            continue;
        }
        d[i] = 0;
        run = op == 0 || op == 1 || (op == 2 && errorlevel == 0) ||
              (op == 3 && errorlevel != 0);
        if (run && *skip(d) && !execute_line(d)) {
            rc = 0;
            break;
        }
        if (next == 0)
            break;
        op = next;
        i = 0;
    }
    depth--;
    return rc;
}

/* ---- FOR %v IN (satz) DO befehl (in Stapeldateien %%v) ----------------- */
static int for_active;

static int for_one(const char *body, char v, const char *val)
{
    static char run[CMDLEN];
    unsigned o = 0, k;
    for (; *body && o < CMDLEN - 1; body++) {
        if (body[0] == '%' && body[1] == v) {
            for (k = 0; val[k] && o < CMDLEN - 1; k++)
                run[o++] = val[k];
            body++;
        } else
            run[o++] = *body;
    }
    run[o] = 0;
    return run_chain(run);
}

static int cmd_for(char *arg)
{
    static char set[CMDLEN], body[CMDLEN], item[FAPI_PATHLEN], dir[FAPI_PATHLEN],
                full[FAPI_PATHLEN];
    static struct fapi_dirent e;
    char v, *p, *b;
    unsigned i, n, k, dl;
    int rc = 1;

    if (arg[0] != '%' || !arg[1] || (arg[2] && arg[2] != ' '))
        goto syntax;
    v = arg[1];
    p = next_word(arg);
    if (!same(p, "IN"))
        goto syntax;
    p = skip(p + 2);
    if (*p != '(')
        goto syntax;
    for (i = 0, p++; *p && *p != ')' && i < CMDLEN - 1; )
        set[i++] = *p++;
    set[i] = 0;
    if (*p != ')')
        goto syntax;
    p = skip(p + 1);
    if (!same(p, "DO"))
        goto syntax;
    p = next_word(p);
    if (!*p)
        goto syntax;
    if (for_active) {
        puts("FOR kann nicht geschachtelt werden.\n");
        return 1;
    }
    for (i = 0; p[i] && i < CMDLEN - 1; i++)
        body[i] = p[i];
    body[i] = 0;
    for_active = 1;
    for (p = set; rc; ) {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '=' || *p == '\t')
            p++;
        if (!*p)
            break;
        for (i = 0; *p && *p != ' ' && *p != ',' && *p != ';' && *p != '=' &&
             *p != '\t' && i < sizeof(item) - 1; )
            item[i++] = *p++;
        item[i] = 0;
        if (!has_wild(item)) {
            rc = for_one(body, v, item);
            continue;
        }
        b = base_name(item);                    /* Dateien zum Muster     */
        dl = (unsigned)(b - item);
        for (k = 0; k < dl; k++)
            dir[k] = item[k];
        dir[dl] = 0;
        if (dl > 1 && dir[dl - 1] == '\\' && dir[dl - 2] != ':')
            dir[dl - 1] = 0;
        for (n = 0; rc && DosFindEntry(&e, n, dir[0] ? dir : 0) != FAPI_ERROR; n++) {
            if ((e.attr & 0x16) || !match(e.name, b))
                continue;               /* wie MS-DOS: nur sichtbare Dateien */
            for (k = 0; k < dl && k < sizeof(full) - 1; k++)
                full[k] = item[k];
            for (i = 0; e.name[i] && k < sizeof(full) - 1; i++)
                full[k++] = e.name[i];
            full[k] = 0;
            rc = for_one(body, v, full);
        }
    }
    for_active = 0;
    return rc;
syntax:
    puts("Syntaxfehler (FOR %v IN (Satz) DO Befehl).\n");
    return 1;
}

/* ---- SETLOCAL/ENDLOCAL wie OS/2: Umgebung, Laufwerk, Verzeichnis ------- */
static unsigned local_sel[4];

static void setlocal(void)
{
    static char e[FAPI_ENVSIZE];
    char __far *d;
    unsigned sel, i, o = 0, k;

    if (nlocal == 4) {
        puts("Zu viele verschachtelte SETLOCAL.\n");
        return;
    }
    sel = DosAllocSeg(FAPI_ENVSIZE + FAPI_CURDIRLEN + 4);
    if (FAPI_FAILED(sel) || !sel) {
        puts("Nicht gen\x81gend Arbeitsspeicher.\n");
        return;
    }
    d = MK_FP(sel, 0);
    DosQCurDir(e);                              /* "C:\\DOS" zuerst        */
    for (k = 0; e[k]; k++)
        d[o++] = e[k];
    d[o++] = 0;
    for (i = 0; DosEnvEntry(i, e, sizeof(e)) != FAPI_ERROR; i++) {
        for (k = 0; e[k] && o < FAPI_ENVSIZE + FAPI_CURDIRLEN; k++)
            d[o++] = e[k];
        d[o++] = 0;
    }
    d[o] = 0;
    local_sel[nlocal] = sel;
    local_lvl[nlocal++] = bdepth;
}

static void endlocal(void)
{
    static char e[FAPI_ENVSIZE];
    char __far *d;
    unsigned o = 0, k;

    if (!nlocal)
        return;
    d = MK_FP(local_sel[--nlocal], 0);
    while (DosEnvEntry(0, e, sizeof(e)) != FAPI_ERROR) {   /* alles weg */
        for (k = 0; e[k] && e[k] != '='; k++)
            ;
        e[k] = '=';
        e[k + 1] = 0;
        if (DosSetEnv(e))
            break;
    }
    for (k = 0; d[o]; )                         /* Verzeichnis            */
        e[k++] = d[o++];
    e[k] = 0;
    o++;
    if (e[0] && e[1] == ':') {
        DosSelectDisk(up(e[0]) - 'A');
        DosChDir(e);
    }
    while (d[o]) {                              /* Umgebung zurueck       */
        for (k = 0; d[o] && k < sizeof(e) - 1; )
            e[k++] = d[o++];
        e[k] = 0;
        o++;
        DosSetEnv(e);
    }
    DosFreeSeg(local_sel[nlocal]);
}

/* ---- TRUENAME: echter Pfad hinter SUBST und JOIN ---------------------- */
static void cmd_truename(char *arg)
{
    static char in[FAPI_PATHLEN], out[FAPI_PATHLEN], t[FAPI_CURDIRLEN];
    unsigned i, o, k, d;

    word(arg, in, sizeof(in));
    dir_abs(in, out);                           /* "C:\\A\\..\\B" */
    for (i = 3, o = 3; out[i]; ) {              /* . und .. auflösen     */
        if (out[i] == '.' && (out[i + 1] == '\\' || !out[i + 1])) {
            i += out[i + 1] ? 2 : 1;
        } else if (out[i] == '.' && out[i + 1] == '.' &&
                   (out[i + 2] == '\\' || !out[i + 2])) {
            i += out[i + 2] ? 3 : 2;
            if (o > 3)
                o--;
            while (o > 3 && out[o - 1] != '\\')
                o--;
        } else {
            while (out[i] && out[i] != '\\')
                out[o++] = out[i++];
            if (out[i])
                out[o++] = out[i++];
        }
    }
    if (o > 3 && out[o - 1] == '\\')
        o--;
    out[o] = 0;
    for (i = 0; out[i]; i++)
        out[i] = up(out[i]);
    d = (unsigned)(out[0] - 'A');
    if (DosQSubst(d, t) == 0) {                 /* X:\\REST -> ZIEL\\REST */
        for (k = 0; t[k]; k++)
            ;
        if (k > 3 && out[3])
            t[k++] = '\\';
        for (i = 3; out[i] && k < sizeof(t) - 1; i++)
            t[k++] = out[i];
        t[k] = 0;
        scopy(out, t, sizeof(out));
    }
    for (d = 0; d < 26; d++) {                  /* C:\\CDROM\\X -> D:\\X  */
        if (DosQJoin(d, t) != 0)
            continue;
        for (k = 0; t[k] && up(t[k]) == out[k]; k++)
            ;
        if (t[k] || (out[k] && out[k] != '\\'))
            continue;
        in[0] = (char)('A' + d);
        in[1] = ':';
        in[2] = '\\';
        for (i = 3, k += out[k] ? 1 : 0; out[k] && i < sizeof(in) - 1; )
            in[i++] = out[k++];
        in[i] = 0;
        scopy(out, in, sizeof(out));
        break;
    }
    puts(out);
    puts("\n");
}

static void on_off(const char *name, int *flag, char *arg)
{
    if (same(arg, "ON"))
        *flag = 1;
    else if (same(arg, "OFF"))
        *flag = 0;
    else if (*arg) {
        puts("Ung\x81ltiger Parameter - ");
        puts(arg);
        puts("\n");
        return;
    } else {
        puts(name);
        puts(*flag ? " ist eingeschaltet.\n" : " ist ausgeschaltet.\n");
    }
}

/* CTTY CON | COM1..4 | AUX: Ein-/Ausgabe ueber ein anderes Geraet */
static void cmd_ctty(char *arg)
{
    static char nm[16];
    unsigned h;
    word(arg, nm, sizeof(nm));
    if (same(nm, "CON")) {
        DosDup2(0xFFFF, STDIN);
        DosDup2(0xFFFF, STDOUT);
        DosDup2(0xFFFF, 2);
        ctty = 0;
        return;
    }
    if (!nm[0]) {
        puts("Syntax: CTTY CON | COM1 | COM2 | COM3 | COM4 | AUX\n");
        return;
    }
    h = DosOpen(nm);
    if (FAPI_FAILED(h)) {
        puts("Ung\x81ltiges Ger\x84t.\n");
        return;
    }
    DosDup2(h, STDIN);
    DosDup2(h, STDOUT);
    DosDup2(h, 2);
    DosClose(h);
    ctty = 1;
}

/* Programm oder Stapeldatei starten; sync: 1 warten, 0 eigene Sitzung.
   call: Stapeldatei als Unterprogramm (CALL) statt als Fortsetzung. */
static int run_program(char *cmd, int sync, int call);

/* Beim Start (COMMAND /P), wie autochk bei Windows NT: geprueft werden
   die Laufwerke, die CHKDSK in \AUTOCHK.LST vorgemerkt hat, und die
   Festplatten, die beim letzten Mal nicht sauber abgemeldet wurden
   (DosQFSInfo: FSI_DIRTY). Welche Platten automatisch geprueft werden,
   legt AUTOCHECK= in der CONFIG.SYS fest (Vorgabe: alle; "AUTOCHECK=" ohne
   Buchstaben: keine). Die Liste wird vorher geloescht, damit ein Absturz
   beim Pruefen nicht bei jedem Start wiederkommt. */
static void autochk(void)
{
    static char lst[256], cl[40], ac[32], todo[26];
    static struct fapi_fsinfo fi;
    unsigned h, n, i, k, d;

    h = DosOpen("\\AUTOCHK.LST");
    if (!FAPI_FAILED(h)) {
        n = DosRead(h, lst, sizeof(lst) - 1);
        DosClose(h);
        DosDelete("\\AUTOCHK.LST");
        if (FAPI_FAILED(n))
            n = 0;
        lst[n] = 0;
        for (i = 0; lst[i]; i++)
            if (lst[i + 1] == ':' && up(lst[i]) >= 'A' && up(lst[i]) <= 'Z' &&
                (i == 0 || lst[i - 1] == '\n'))
                todo[up(lst[i]) - 'A'] = 1;
    }
    if (FAPI_FAILED(DosGetEnv("AUTOCHECK", ac, sizeof(ac))))
        ac[0] = 0;                          /* nicht gesetzt: alle      */
    for (d = 2; d < 26; d++) {
        if (FAPI_FAILED(DosQFSInfo(d, &fi)) || !(fi.flags & FSI_DIRTY))
            continue;
        if (ac[0]) {
            for (k = 0; ac[k] && up(ac[k]) != (char)('A' + d); k++)
                ;
            if (!ac[k])
                continue;
        }
        puts("Laufwerk ");
        putc((char)('A' + d));
        puts(": wurde nicht ordnungsgem\x84\xE1 abgemeldet.\n");
        todo[d] = 1;
    }
    for (d = 0; d < 26; d++) {
        if (!todo[d])
            continue;
        for (k = 0; "CHKDSK "[k]; k++)
            cl[k] = "CHKDSK "[k];
        cl[k++] = (char)('A' + d);
        cl[k++] = ':';
        for (n = 0; " /F /AUTO"[n]; n++)
            cl[k++] = " /F /AUTO"[n];
        cl[k] = 0;
        puts("Pr\x81" "fung beim Start: ");
        puts(cl);
        puts("\n");
        run_program(cl, 1, 0);
        puts("\n");
    }
}

static int run_program(char *cmd, int sync, int call)
{
    static char path[FAPI_PATHLEN], cl[FAPI_PATHLEN + FAPI_ARGLEN];
    char *args = next_word(cmd);
    unsigned wl, i, k, r;
    int type;

    for (wl = 0; cmd[wl] && cmd[wl] != ' '; wl++)
        ;
    type = find_program(cmd, wl, path);
    if (type == 0) {
        puts("Befehl oder Dateiname nicht gefunden.\n");
        errorlevel = 1;                 /* fuer ||                      */
        return 1;
    }
    if (type == 3) {
        for (i = 0; path[i]; i++)
            path[i] = up(path[i]);
        puts(base_name(path));
        puts(" ist ein MS-DOS-Programm und l\x84uft nicht unter AT-DOS.\n");
        return 1;
    }
    if (type == 2 && sync == 1) {
        r = run_batch(path, args);
        if (bdepth > 0 && !call)
            batch_stop = 1;             /* ohne CALL: Kette, nicht zurueck */
        return r;
    }
    k = 0;
    if (type == 2) {                    /* START x.BAT: COMMAND /C x.BAT */
        if (DosGetEnv("COMSPEC", cl, sizeof(cl)) != FAPI_ERROR)
            for (; cl[k]; k++)
                ;
        else
            for (; "COMMAND.EXE"[k]; k++)
                cl[k] = "COMMAND.EXE"[k];
        cl[k++] = ' '; cl[k++] = '/'; cl[k++] = 'C'; cl[k++] = ' ';
    }
    for (i = 0; path[i] && k < sizeof(cl) - 2; i++)
        cl[k++] = path[i];
    if (slen(args) >= FAPI_ARGLEN) {        /* Argumente, ohne Programmpfad */
        puts("Befehlszeile zu lang.\n");
        return 1;
    }
    if (*args) {
        cl[k++] = ' ';
        for (i = 0; args[i] && k < sizeof(cl) - 1; i++)
            cl[k++] = args[i];
    }
    cl[k] = 0;
    if (k >= FAPI_PATHLEN) {
        puts("Befehlszeile zu lang.\n");
        return 1;
    }
    r = DosExecPgm(cl, sync == 2 ? EXEC_BG : sync ? EXEC_SYNC : EXEC_ASYNC);
    if (r) {
        exec_error(r);
        errorlevel = 1;
    } else if (sync == 1) {
        errorlevel = DosQChildRC();     /* fuer IF ERRORLEVEL          */
        ran_prog = 1;
    } else if (sync == 2)
        puts("Das Programm l\x84uft im Hintergrund (DETACH).\n");
    return 1;
}

/* ---- Hauptschleife ----------------------------------------------------- */
static int execute(char *l)
{
    char *cmd = skip(l), *arg;

    if (!*cmd)
        return 1;
    arg = next_word(cmd);

    if (cmd[0] && cmd[1] == ':' && (cmd[2] == 0 || cmd[2] == ' ')) {
        unsigned r = DosSelectDisk(up(cmd[0]) - 'A');
        if (r == FAPI_ERR_NOTREADY)
            puts("Laufwerk nicht bereit - keine Diskette eingelegt.\n");
        else if (r == FAPI_ERROR)
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
    else if (same(cmd, "SHUTDOWN") || same(cmd, "POWEROFF") || same(cmd, "REBOOT")) {
        int r = same(cmd, "REBOOT") || same(arg, "/R");
        if (confirm(r ? "Alle Programme beenden und neu starten (J/N)? "
                      : "Alle Programme beenden und herunterfahren (J/N)? "))
            DosShutdown(r);
    }
    else if (same(cmd, "CD") || same(cmd, "CHDIR"))
        cmd_cd(arg);
    else if (same(cmd, "VER"))
        cmd_ver();
    else if (same(cmd, "VOL"))
        cmd_vol(arg);
    else if (same(cmd, "DATE"))
        cmd_date(arg);
    else if (same(cmd, "TIME"))
        cmd_time(arg);
    else if (same(cmd, "DOSKEY"))
        cmd_doskey(arg);
    else if (same(cmd, "ECHO")) {
        if (same(arg, "OFF"))
            echo_on = 0;
        else if (same(arg, "ON"))
            echo_on = 1;
        else if (!*arg)
            puts(echo_on ? "ECHO ist eingeschaltet.\n" : "ECHO ist ausgeschaltet.\n");
        else {
            puts(arg);
            puts("\n");
        }
    } else if (up(cmd[0]) == 'E' && up(cmd[1]) == 'C' && up(cmd[2]) == 'H' &&
               up(cmd[3]) == 'O' && (cmd[4] == '.' || cmd[4] == ':')) {
        puts(cmd + 5);                  /* ECHO. = Leerzeile            */
        puts("\n");
    } else if (same(cmd, "SET"))
        cmd_set(arg);
    else if (same(cmd, "PATH")) {
        if (!*arg) {
            if (DosGetEnv("PATH", pbuf, sizeof(pbuf)) == FAPI_ERROR || !pbuf[0])
                puts("Kein Pfad\n");
            else {
                puts("PATH=");
                puts(pbuf);
                puts("\n");
            }
        } else
            set_var("PATH", arg[0] == ';' && !arg[1] ? "" : arg);
    } else if (same(cmd, "PROMPT"))
        set_var("PROMPT", arg);
    else if (same(cmd, "REM"))
        ;
    else if (same(cmd, "PAUSE")) {
        puts("Eine beliebige Taste dr\x81\x63ken . . .");
        KbdCharIn();
        puts("\n");
    } else if (same(cmd, "GOTO")) {
        if (bdepth)
            cmd_goto(&bat[bdepth - 1], arg);
    } else if (same(cmd, "SHIFT")) {
        if (bdepth)
            bat[bdepth - 1].shift++;
    } else if (same(cmd, "IF"))
        return cmd_if(arg);
    else if (same(cmd, "FOR"))
        return cmd_for(arg);
    else if (same(cmd, "BREAK"))
        on_off("BREAK", &break_on, arg);
    else if (same(cmd, "VERIFY"))
        on_off("VERIFY", &verify_on, arg);
    else if (same(cmd, "KEYS")) {               /* OS/2: Befehlsspeicher */
        if (same(arg, "LIST"))
            cmd_doskey("/HISTORY");
        else
            on_off("KEYS", &keys_on, arg);
    } else if (same(cmd, "TRUENAME"))
        cmd_truename(arg);
    else if (same(cmd, "CHCP")) {
        if (!*arg || same(arg, "437"))
            puts("Aktive Codeseite: 437\n");
        else if (same(arg, "850"))
            puts("Codeseite 850 ist nicht vorbereitet (Zeichensatz fehlt).\n");
        else
            puts("Ung\x81ltige Codeseite.\n");
    } else if (same(cmd, "CTTY"))
        cmd_ctty(arg);
    else if (same(cmd, "LH") || same(cmd, "LOADHIGH")) {
        while (*arg == '/')                     /* /L:... /S: ohne Wirkung */
            arg = next_word(arg);
        if (*arg)
            return run_program(arg, 1, 0);
    } else if (same(cmd, "DETACH")) {
        if (*arg)
            run_program(arg, 2, 0);
        else
            puts("Syntax: DETACH Programm [Argumente]\n");
    } else if (same(cmd, "SETLOCAL"))
        setlocal();
    else if (same(cmd, "ENDLOCAL"))
        endlocal();
    else if (same(cmd, "CALL")) {
        if (*arg)
            return run_program(arg, 1, 1);
    } else if (same(cmd, "HILFE") || same(cmd, "HELP") || same(cmd, "?")) {
        static char hb[80];
        unsigned bi;
        if (*arg) {                     /* HELP Befehl = Befehl /?       */
            word(arg, hb, sizeof(hb) - 4);
            for (bi = 0; builtin[bi] && !same(hb, builtin[bi]); bi++)
                ;
            if (!builtin[bi]) {         /* intern: Liste unten           */
                bi = slen(hb);
                hb[bi] = ' '; hb[bi + 1] = '/'; hb[bi + 2] = '?'; hb[bi + 3] = 0;
                return run_program(hb, 1, 0);
            }
        }
        cmd_help();
    }
    else if (same(cmd, "START")) {
        if (!*arg)
            puts("Syntax: START Programm [Argumente]\n");
        else
            run_program(arg, 0, 0);
    } else if (same(cmd, "EXIT")) {
        if (!permanent)
            return 0;
        puts("Dieser Befehlsinterpreter ist permanent (/P) und kann nicht beendet werden.\n");
    } else
        return run_program(cmd, 1, 0);
    return 1;
}

/* ---- Umleitung und Pipes ----------------------------------------------
   befehl < ein > aus, >> anhaengen, a | b | c. Wie COMMAND.COM biegt der
   Befehlsinterpreter seine eigenen Standard-Handles um; gestartete Programme
   erben sie. Linke Teile einer Pipe laufen gleichzeitig im Hintergrund
   (COMMAND /C), der Kernel puffert dazwischen. */
#define NSEG 4
static struct seg {
    char *cmd, *in, *out;
    int   append;
} sg[NSEG];
static char lbuf[CMDLEN];

/* Dateinamen hinter < oder > herausloesen: in eigenen Puffer kopieren und
   in der Zeile durch Leerzeichen ersetzen */
static char *take_name(char **pp)
{
    static char nb[NSEG * 2][FAPI_PATHLEN];
    static int ni;
    char *p = *pp, *d = nb[ni++ % (NSEG * 2)];
    unsigned k = 0;

    while (*p == ' ')
        p++;
    while (*p && *p != ' ' && *p != '<' && *p != '>' && *p != '|') {
        if (k < FAPI_PATHLEN - 1)
            d[k++] = *p;
        *p++ = ' ';
    }
    d[k] = 0;
    *pp = p;
    return d;
}

static int parse_line(char *l)
{
    int n = 0, q = 0;
    char *p;

    for (p = l; *p && p - l < CMDLEN - 1; p++)
        lbuf[p - l] = *p;
    lbuf[p - l] = 0;
    sg[0].cmd = lbuf;
    sg[0].in = sg[0].out = 0;
    sg[0].append = 0;
    for (p = lbuf; *p; ) {
        if (*p == '"') {
            q = !q;
            p++;
        } else if (q) {
            p++;
        } else if (*p == '|') {
            *p++ = 0;
            if (++n == NSEG)
                return -1;
            sg[n].cmd = p;
            sg[n].in = sg[n].out = 0;
            sg[n].append = 0;
        } else if (*p == '<') {
            *p++ = ' ';
            sg[n].in = take_name(&p);
        } else if (*p == '>') {
            *p++ = ' ';
            if (*p == '>') {
                *p++ = ' ';
                sg[n].append = 1;
            }
            sg[n].out = take_name(&p);
        } else
            p++;
    }
    return n + 1;
}

static unsigned slen2(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

/* Teil einer Pipe im Hintergrund: COMMAND /C teil */
static void run_bg(char *cmd)
{
    static char cl[FAPI_PATHLEN + FAPI_ARGLEN];
    unsigned k = 0, i;

    if (DosGetEnv("COMSPEC", cl, sizeof(cl)) == FAPI_ERROR)
        for (; "COMMAND.EXE"[k]; k++)
            cl[k] = "COMMAND.EXE"[k];
    else
        k = slen2(cl);
    cl[k++] = ' '; cl[k++] = '/'; cl[k++] = 'C'; cl[k++] = ' ';
    cmd = skip(cmd);
    for (i = 0; cmd[i] && k < sizeof(cl) - 1; i++)
        cl[k++] = cmd[i];
    cl[k] = 0;
    if (k >= FAPI_PATHLEN) {
        puts("Befehlszeile zu lang.\n");
        return;
    }
    i = DosExecPgm(cl, EXEC_BG);
    if (i)
        exec_error(i);
}

/* eine Eingabezeile mit Umleitungen und Pipes ausfuehren; 0 = EXIT */
static int execute_line(char *l)
{
    static unsigned hh[2];
    unsigned save_in, save_out, h, prev = 0;
    int n, i, rc = 1, simple = 1;
    char *p;

    for (p = l; *p; p++)
        if (*p == '|' || *p == '<' || *p == '>')
            simple = 0;
    if (simple)
        return execute(l);
    n = parse_line(l);
    if (n < 0) {
        puts("Zu viele Teile in der Pipe.\n");
        return 1;
    }
    save_in = DosDup(STDIN);
    save_out = DosDup(STDOUT);
    if (FAPI_FAILED(save_in) || FAPI_FAILED(save_out)) {
        puts("Zu viele offene Dateien (FILES=).\n");
        return 1;
    }
    for (i = 0; i < n; i++) {
        if (prev) {                             /* Eingabe aus der Pipe */
            DosDup2(prev, STDIN);
            DosClose(prev);
            prev = 0;
        }
        if (sg[i].in) {
            h = DosOpen(sg[i].in);
            if (FAPI_FAILED(h)) {
                puts("Datei nicht gefunden.\n");
                break;
            }
            DosDup2(h, STDIN);
            DosClose(h);
        }
        if (i < n - 1) {
            if (DosPipe(hh)) {
                puts("Keine Pipe mehr frei.\n");
                break;
            }
            DosDup2(hh[1], STDOUT);
            DosClose(hh[1]);
            prev = hh[0];
        } else
            DosDup2(save_out, STDOUT);
        if (sg[i].out) {
            h = sg[i].append ? DosAppend(sg[i].out) : DosCreate(sg[i].out);
            if (FAPI_FAILED(h)) {
                DosDup2(save_out, STDOUT);
                file_error(h);
                break;
            }
            DosDup2(h, STDOUT);
            DosClose(h);
        }
        if (i < n - 1)
            run_bg(sg[i].cmd);
        else
            rc = execute(sg[i].cmd);
        DosDup2(save_out, STDOUT);              /* eigene Ausgabe zurueck */
        DosDup2(save_in, STDIN);
    }
    if (prev)
        DosClose(prev);
    DosDup2(save_in, STDIN);
    DosDup2(save_out, STDOUT);
    DosClose(save_in);
    DosClose(save_out);
    return rc;
}

void main(void)
{
    static char args[FAPI_ARGLEN];
    char *p;

    DosGetArgs(args);
    for (p = args; *p; p = next_word(p)) {
        if (same(p, "/P"))
            permanent = 1;
        else if (up(p[0]) == '/' || p[0] == '/') {
            if (up(p[1]) == 'C' && (p[2] == ' ' || !p[2])) {
                run_chain(next_word(p));    /* COMMAND /C befehl: ausfuehren, Ende */
                DosExit(errorlevel);        /* Rueckgabewert durchreichen */
            }
        }
    }

    puts("COMMAND.EXE - Befehlsinterpreter f\x81r AT-DOS 5.0\n"
         "HILFE zeigt die verf\x81gbaren Befehle.\n\n");
    if (permanent) {
        autochk();
        if (DosGetEnv("TEMP", pbuf, sizeof(pbuf)) != FAPI_ERROR && pbuf[0])
            DosMkDir(pbuf);             /* fehlt %TEMP%: anlegen        */
    }
    /* AUTOEXEC.CMD ist die native Startdatei; AUTOEXEC.BAT gehoert (wie bei
       OS/2) der DOS-Box und bleibt unangetastet */
    if (permanent && exists("\\AUTOEXEC.CMD")) {
        if (!run_batch("\\AUTOEXEC.CMD", ""))
            return;
        echo_on = 1;
        puts("\n");
    }
    for (;;) {
        if (echo_on)
            show_prompt();
        read_line();
        if (!run_chain(line))
            break;
    }
}
