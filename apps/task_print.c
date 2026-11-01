/*
 * PRINT.EXE - Dateien im Hintergrund drucken (wie MS-DOS):
 *
 *   PRINT [/D:Geraet] [/T] [[Laufwerk:][Pfad]Datei [/C] [/P] ...]
 *
 * Warteschlange: \DOS\SYSTEM\PRINT.QUE auf dem Startlaufwerk, je Zeile
 * "Geraet Pfad". Gedruckt wird von einem Hintergrund-Task in derselben
 * Sitzung (PRINT /WORK, EXEC_BG) - unter MS-DOS war PRINT dafuer ein
 * residentes Programm, hier laeuft es einfach nebenher. Ob er laeuft,
 * zeigt PRINT.LCK: der Task haelt die Datei offen, DosCreate meldet dann
 * "in Benutzung". Vor jedem Stueck prueft er, ob seine Datei noch vorne
 * in der Schlange steht - /T und /C brechen so den laufenden Druck ab.
 * Drucker nicht bereit: alle 5 s neuer Versuch, bis /T.
 *
 *   /D:Geraet  PRN (Vorgabe), LPT1, LPT2, LPT3
 *   /T         Warteschlange leeren (auch den laufenden Druck)
 *   /C         folgende (und die davor genannte) Datei(en) entfernen
 *   /P         folgende Dateien einreihen (Vorgabe)
 */
#include "fapi.h"
#include "util.h"

#define QMAX 2048

static char args[FAPI_ARGLEN], qpath[FAPI_PATHLEN], lpath[FAPI_PATHLEN];
static char q[QMAX + 1], head[FAPI_PATHLEN + 8], full[FAPI_PATHLEN], tmp[FAPI_PATHLEN];
static char dev[8] = "PRN", cs[FAPI_PATHLEN];
static unsigned char buf[2048];

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void sysfile(char *out, const char *name)
{
    out[0] = 'C';
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
        out[0] = up(cs[0]);
    out[1] = 0;
    scat(out, ":\\DOS\\SYSTEM\\", FAPI_PATHLEN);
    scat(out, name, FAPI_PATHLEN);
}

static unsigned read_q(void)
{
    unsigned h = DosOpen(qpath), n = 0;
    if (!FAPI_FAILED(h)) {
        n = DosRead(h, q, QMAX);
        if (FAPI_FAILED(n))
            n = 0;
        DosClose(h);
    }
    q[n] = 0;
    return n;
}

static void write_q(unsigned n)
{
    unsigned h;
    if (!n) {
        DosDelete(qpath);
        return;
    }
    h = DosCreate(qpath);
    if (!FAPI_FAILED(h)) {
        DosWrite(h, q, n);
        DosClose(h);
    }
}

/* erste Zeile der Schlange nach head; 0 = leer */
static int first(void)
{
    unsigned i;
    read_q();
    for (i = 0; q[i] && q[i] != '\r' && q[i] != '\n' && i < sizeof(head) - 1; i++)
        head[i] = q[i];
    head[i] = 0;
    return i != 0;
}

/* Zeile 'line' (ganz) aus der Schlange entfernen; Anzahl */
static unsigned remove_lines(const char *path, int all)
{
    unsigned i = 0, o = 0, s, n = 0, k;
    char *l;
    read_q();
    while (q[i]) {
        s = i;
        while (q[i] && q[i] != '\n')
            i++;
        if (q[i])
            i++;
        l = q + s;
        while (*l && *l != ' ' && l < q + i)
            l++;
        if (*l == ' ')
            l++;
        for (k = 0; path && path[k] && l + k < q + i && up(l[k]) == path[k]; k++)
            ;
        if (all || (path && !path[k] && (l[k] == '\r' || l[k] == '\n' || !l[k]))) {
            n++;
            continue;                   /* weglassen                     */
        }
        while (s < i)
            q[o++] = q[s++];
    }
    q[o] = 0;
    write_q(o);
    return n;
}

/* ---- Hintergrund-Task ------------------------------------------------- */
static void worker(void)
{
    static char line[FAPI_PATHLEN + 8], file[FAPI_PATHLEN];
    unsigned lock, hs, hd, n, w, i, warned = 0;
    int abort;

    lock = DosCreate(lpath);
    if (FAPI_FAILED(lock))
        return;                         /* laeuft schon                  */
    for (;;) {
        if (!first()) {                 /* leer: aufhoeren, aber erst    */
            DosClose(lock);             /* nachsehen, ob inzwischen      */
            DosDelete(lpath);           /* etwas dazukam                 */
            if (!first())
                return;
            lock = DosCreate(lpath);
            if (FAPI_FAILED(lock))
                return;
            continue;
        }
        scopy(line, head, sizeof(line));
        for (i = 0; line[i] && line[i] != ' '; i++)
            dev[i < 7 ? i : 7] = line[i];
        dev[i < 7 ? i : 7] = 0;
        scopy(file, line[i] ? line + i + 1 : "", sizeof(file));
        hs = DosOpen(file);
        if (FAPI_FAILED(hs)) {
            puts("\r\nPRINT: ");
            puts(file);
            puts(" nicht gefunden.\r\n");
            remove_lines(file, 0);      /* Kopf entfernen                */
            continue;
        }
        hd = DosCreate(dev);
        abort = 0;
        while (!FAPI_FAILED(hd) && (n = DosRead(hs, buf, sizeof(buf))) != 0 && !FAPI_FAILED(n)) {
            w = DosWrite(hd, buf, n);
            if (w != n) {
                abort = 2;              /* nicht bereit                  */
                break;
            }
            if (!first() || !streq(line, head)) {
                abort = 1;              /* /T oder /C                    */
                break;
            }
        }
        DosClose(hs);
        if (abort == 1 || FAPI_FAILED(hd)) {
            if (!FAPI_FAILED(hd)) {
                static const char msg[] = "\r\n*** Druck abgebrochen ***\r\n\f";
                DosWrite(hd, (char *)msg, sizeof(msg) - 1);
                DosClose(hd);
            }
            if (FAPI_FAILED(hd)) {      /* Geraet fehlt: Datei weg       */
                puts("\r\nPRINT: Ger\x84t ");
                puts(dev);
                puts(" nicht vorhanden.\r\n");
                remove_lines(file, 0);
            }
            continue;
        }
        DosClose(hd);
        if (abort == 2) {               /* spaeter von vorn               */
            if (!warned) {
                puts("\r\nPRINT: Fehler auf Drucker ");
                puts(dev);
                puts(" (aus, offline, kein Papier?) - neuer Versuch alle 5 s.\r\n");
                warned = 1;
            }
            DosSleep(5000);
            continue;
        }
        warned = 0;
        if (first() && streq(line, head))
            remove_lines(file, 0);      /* fertig                        */
    }
}

/* ---- Aufruf ----------------------------------------------------------- */
static void show_queue(void)
{
    unsigned i = 0, k = 0;
    read_q();
    if (!q[0]) {
        puts("Die Druckwarteschlange ist leer.\r\n");
        return;
    }
    while (q[i]) {
        while (q[i] && q[i] != ' ')
            i++;
        if (q[i] == ' ')
            i++;
        puts("    ");
        while (q[i] && q[i] != '\r' && q[i] != '\n')
            putc(q[i++]);
        puts(k++ ? " ist in der Warteschlange\r\n" : " wird gerade gedruckt\r\n");
        while (q[i] == '\r' || q[i] == '\n')
            i++;
    }
}

static void start_worker(void)
{
    static char cmd[FAPI_PATHLEN];
    unsigned h = DosCreate(lpath);
    if (h == FAPI_ERR_INUSE)
        return;                         /* laeuft schon                  */
    if (!FAPI_FAILED(h)) {
        DosClose(h);
        DosDelete(lpath);
    }
    cmd[0] = lpath[0];
    cmd[1] = 0;
    scat(cmd, ":\\DOS\\PRINT.EXE /WORK", sizeof(cmd));
    if (DosExecPgm(cmd, EXEC_BG))
        puts("PRINT: Hintergrunddruck kann nicht gestartet werden.\r\n");
}

static void enqueue(const char *path)
{
    unsigned n = read_q(), h;
    if (n + slen(path) + 10 > QMAX) {
        puts("Die Druckwarteschlange ist voll.\r\n");
        return;
    }
    h = DosOpen((char *)path);
    if (FAPI_FAILED(h)) {
        puts(path);
        puts(": Datei nicht gefunden.\r\n");
        return;
    }
    DosClose(h);
    scopy(q + n, dev, QMAX + 1 - n);
    scat(q + n, " ", QMAX + 1 - n);
    scat(q + n, path, QMAX + 1 - n);
    scat(q + n, "\r\n", QMAX + 1 - n);
    write_q(slen(q));
}

/* eine Angabe (auch * und ?) einreihen oder entfernen */
static void each(char *spec, int cancel)
{
    static struct fapi_dirent e;
    static char dir[FAPI_PATHLEN];
    char *pat;
    unsigned n, dl;

    if (abs_path(spec, full)) {
        puts("Ung\x81ltiger Pfad.\r\n");
        return;
    }
    for (n = 0; full[n]; n++)
        full[n] = up(full[n]);
    if (!has_wild(full)) {
        if (cancel) {
            if (!remove_lines(full, 0)) {
                puts(full);
                puts(" ist nicht in der Warteschlange.\r\n");
            }
        } else
            enqueue(full);
        return;
    }
    pat = base_name(full);
    dl = (unsigned)(pat - full);
    for (n = 0; n < dl; n++)
        dir[n] = full[n];
    dir[dl] = 0;
    for (n = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR; n++) {
        if ((e.attr & 0x18) || !match(e.name, pat))
            continue;
        join(tmp, dir, e.name, sizeof(tmp));
        if (cancel)
            remove_lines(tmp, 0);
        else
            enqueue(tmp);
    }
}

void main(void)
{
    static char w[FAPI_PATHLEN];
    char *a, *last = 0;
    int cancel = 0, any = 0, k;

    sysfile(qpath, "PRINT.QUE");
    sysfile(lpath, "PRINT.LCK");
    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && up(a[1]) == 'W') {
        worker();
        return;
    }
    if (a[0] == '/' && a[1] == '?') {
        puts("Druckt Textdateien im Hintergrund.\r\n\r\n"
             "PRINT [/D:Ger\x84t] [/T] [[Laufwerk:][Pfad]Datei [/C] [/P] ...]\r\n\r\n"
             "  /D:Ger\x84t  PRN (Vorgabe), LPT1, LPT2, LPT3\r\n"
             "  /T        leert die Warteschlange\r\n"
             "  /C        entfernt Dateien aus der Warteschlange\r\n"
             "  /P        reiht Dateien ein (Vorgabe)\r\n\r\n"
             "PRINT ohne Parameter zeigt die Warteschlange.\r\n");
        return;
    }
    for (; *a; a = next_word(a)) {
        if (a[0] == '/') {
            switch (up(a[1])) {
            case 'D':
                for (k = 0; a[3 + k] && a[3 + k] != ' ' && a[3 + k] != ':' && k < 7; k++)
                    dev[k] = up(a[3 + k]);
                dev[k] = 0;
                break;
            case 'T':
                remove_lines(0, 1);
                puts("Alle Dateien wurden aus der Druckwarteschlange gel\x94scht.\r\n");
                any = 1;
                break;
            case 'C':
                cancel = 1;
                if (last) {
                    word(last, w, sizeof(w));
                    each(w, 1);
                }
                break;
            case 'P':
                cancel = 0;
                if (last) {
                    word(last, w, sizeof(w));
                    each(w, 0);
                }
                break;
            default:
                puts("Ung\x81ltiger Parameter - ");
                puts(a);
                puts("\r\n");
                DosExit(1);
            }
            last = 0;
            continue;
        }
        if (last) {                     /* vorige Datei ohne Schalter    */
            word(last, w, sizeof(w));
            each(w, cancel);
        }
        last = a;
        any = 1;
    }
    if (last) {
        word(last, w, sizeof(w));
        each(w, cancel);
    }
    if (first())
        start_worker();
    show_queue();
    (void)any;
}
