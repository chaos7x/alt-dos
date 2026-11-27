/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT.EXE - Befehle zu festen Zeiten starten (Bedienung wie AT von NT):
 *
 *   AT [\\Computer] [Kennung] [/DELETE [/YES]]
 *   AT [\\Computer] Zeit [/INTERACTIVE] [/EVERY:Tag[,...] | /NEXT:Tag[,...]] "Befehl"
 *
 * Die Auftraege stehen in \DOS\SYSTEM\AT.LST des Startlaufwerks, je Zeile
 * "Kennung Art Merker Wochentage Monatstage(2) Zeit naechsterTag Befehl"
 * (Tag = Tage seit 1.1.1980). Abgearbeitet werden sie vom Dienst
 * AT /SERVICE: ein Ring-3-Programm ohne Konsole wie der Zeitplandienst von
 * NT, gestartet von RUN= in der CONFIG.SYS oder von AT selbst, sobald ein
 * Auftrag dazukommt. Ob er laeuft, zeigt AT.LCK (offen gehalten, wie bei
 * PRINT). Ohne Auftraege beendet er sich. Jeder Start steht in AT.LOG.
 *
 * Gestartet wird "COMSPEC /C Befehl" losgeloest (EXEC_DETACH, Ausgaben nach
 * NUL), mit /INTERACTIVE in einer eigenen Sitzung. Verpasste Zeiten (Rechner
 * aus) werden bis 60 Minuten nachgeholt, sonst uebersprungen.
 */
#include "fapi.h"
#include "util.h"
#define CTRY_NO_DPAT
#include "country.h"

#define NJOB    32
#define CMDMAX  124
#define LSTMAX  (NJOB * (CMDMAX + 48))

enum { K_ONCE, K_EVERY, K_NEXT };
#define JF_INTER 1

struct job {
    unsigned id, kind, flags, dmask, mm[2], tm, due;
    char     cmd[CMDMAX];
};

static struct job jobs[NJOB];
static unsigned   nj;
static char lst[FAPI_PATHLEN], lck[FAPI_PATHLEN], logp[FAPI_PATHLEN], sysd[FAPI_PATHLEN];
static char fbuf[LSTMAX + 1], args[FAPI_ARGLEN], cs[FAPI_PATHLEN], line[200];
static unsigned short dt[2];
static unsigned today, nowmin, nowsec;

static const char *wdn[7] = { "Mo", "Di", "Mi", "Do", "Fr", "Sa", "So" };
static const char *wfull[14] = {
    "MONTAG", "DIENSTAG", "MITTWOCH", "DONNERSTAG", "FREITAG", "SAMSTAG", "SONNTAG",
    "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY", "SUNDAY"
};
static const unsigned char mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* ---- Datum --------------------------------------------------------- */
static int leap(unsigned y) { return !(y & 3) && (y % 100 || !(y % 400)); }

static unsigned mlen(unsigned y, unsigned m)
{
    return m == 2 && leap(y) ? 29 : mdays[m - 1];
}

static unsigned ymd2dn(unsigned y, unsigned m, unsigned d)
{
    unsigned n = 0, i;
    for (i = 1980; i < y; i++)
        n += leap(i) ? 366 : 365;
    for (i = 1; i < m; i++)
        n += mlen(y, i);
    return n + d - 1;
}

static void dn2ymd(unsigned n, unsigned *y, unsigned *m, unsigned *d)
{
    unsigned k;
    *y = 1980;
    while (n >= (k = leap(*y) ? 366 : 365))
        n -= k, (*y)++;
    *m = 1;
    while (n >= (k = mlen(*y, *m)))
        n -= k, (*m)++;
    *d = n + 1;
}

static unsigned wday(unsigned n) { return (n + 1) % 7; }   /* 0 = Mo; 1.1.1980 Di */

static unsigned mday(unsigned n)
{
    unsigned y, m, d;
    dn2ymd(n, &y, &m, &d);
    return d;
}

static void now(void)
{
    DosGetDateTime(dt);
    today = ymd2dn(1980 + (dt[0] >> 9), (dt[0] >> 5) & 15, dt[0] & 31);
    nowmin = (dt[1] >> 11) * 60 + ((dt[1] >> 5) & 63);
    nowsec = (dt[1] & 31) * 2;
}

static int matches(struct job *j, unsigned n)
{
    unsigned d;
    if (j->kind == K_ONCE)
        return 1;
    if (j->dmask & (1u << wday(n)))
        return 1;
    d = mday(n);
    return (j->mm[d >> 4] >> (d & 15)) & 1;
}

static unsigned next_due(struct job *j, unsigned from)
{
    unsigned i;
    for (i = 0; i < 400; i++)
        if (matches(j, from + i))
            return from + i;
    return 0;
}

/* ---- Liste lesen/schreiben ---------------------------------------- */
static char *num(char *p, unsigned *v)
{
    *v = 0;
    p = skip(p);
    while (*p >= '0' && *p <= '9')
        *v = *v * 10 + (*p++ - '0');
    return p;
}

static void read_jobs(void)
{
    unsigned h = DosOpen(lst), n = 0, i = 0, k;
    struct job *j;
    char *p;

    nj = 0;
    if (!FAPI_FAILED(h)) {
        n = DosRead(h, fbuf, LSTMAX);
        if (FAPI_FAILED(n))
            n = 0;
        DosClose(h);
    }
    fbuf[n] = 0;
    while (fbuf[i] && nj < NJOB) {
        p = fbuf + i;
        while (fbuf[i] && fbuf[i] != '\n')
            i++;
        if (fbuf[i])
            fbuf[i++] = 0;
        j = &jobs[nj];
        p = num(p, &j->id);
        if (!j->id)
            continue;
        p = num(p, &j->kind);
        p = num(p, &j->flags);
        p = num(p, &j->dmask);
        p = num(p, &j->mm[0]);
        p = num(p, &j->mm[1]);
        p = num(p, &j->tm);
        p = num(p, &j->due);
        p = skip(p);
        for (k = 0; p[k] && p[k] != '\r' && k < CMDMAX - 1; k++)
            j->cmd[k] = p[k];
        j->cmd[k] = 0;
        if (j->kind > K_NEXT || j->tm >= 1440 || !k)
            continue;
        nj++;
    }
}

static char *unum(char *o, unsigned v)
{
    char t[6];
    int k = 0;
    do t[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k)
        *o++ = t[--k];
    *o++ = ' ';
    return o;
}

static int write_jobs(void)
{
    unsigned i, h;
    char *o = fbuf;
    struct job *j;

    if (!nj) {
        DosDelete(lst);
        return 0;
    }
    for (i = 0; i < nj; i++) {
        j = &jobs[i];
        o = unum(o, j->id);
        o = unum(o, j->kind);
        o = unum(o, j->flags);
        o = unum(o, j->dmask);
        o = unum(o, j->mm[0]);
        o = unum(o, j->mm[1]);
        o = unum(o, j->tm);
        o = unum(o, j->due);
        scopy(o, j->cmd, CMDMAX);
        o += slen(o);
        *o++ = '\r';
        *o++ = '\n';
    }
    h = DosCreate(lst);
    if (FAPI_FAILED(h))
        return -1;
    i = DosWrite(h, fbuf, (unsigned)(o - fbuf));
    DosClose(h);
    return FAPI_FAILED(i) ? -1 : 0;
}

static void jcopy(struct job *d, const struct job *s)
{
    const char *a = (const char *)s;
    char *b = (char *)d;
    unsigned n = sizeof(*d);
    while (n--)
        *b++ = *a++;
}

static void drop(unsigned i)
{
    for (nj--; i < nj; i++)
        jcopy(&jobs[i], &jobs[i + 1]);
}

/* ---- Anzeige ------------------------------------------------------- */
static void hhmm(char *o, unsigned m)
{
    ctry_time(o, ((m / 60) << 11) | ((m % 60) << 5), 0);
}

/* Text der Tage: "Heute", "Morgen", Datum, "Jeden Mo Fr 15", "Nächster 1" */
static void daytext(char *o, struct job *j)
{
    unsigned i, y, m, d;
    *o = 0;
    if (j->kind == K_ONCE) {
        if (j->due == today)
            scopy(o, "Heute", 32);
        else if (j->due == today + 1)
            scopy(o, "Morgen", 32);
        else {
            dn2ymd(j->due, &y, &m, &d);
            ctry_date(o, (unsigned)(((y - 1980) << 9) | (m << 5) | d));
        }
        return;
    }
    scopy(o, j->kind == K_EVERY ? "Jeden" : "N\x84" "chster", 40);
    for (i = 0; i < 7; i++)
        if (j->dmask & (1u << i)) {
            scat(o, " ", 40);
            scat(o, wdn[i], 40);
        }
    for (i = 1; i < 32; i++)
        if ((j->mm[i >> 4] >> (i & 15)) & 1) {
            char t[4], *e = t;
            if (slen(o) > 34) {
                scat(o, " ...", 40);
                break;
            }
            if (i >= 10)
                *e++ = (char)('0' + i / 10);
            *e++ = (char)('0' + i % 10);
            *e = 0;
            scat(o, " ", 40);
            scat(o, t, 40);
        }
}

static void pad(const char *s, unsigned w)
{
    unsigned n = slen(s);
    puts(s);
    while (n++ < w)
        putc(' ');
}

static void list(void)
{
    static char d[48], t[8];
    unsigned i;
    if (!nj) {
        puts("Es sind keine Eintr\x84ge in der Liste vorhanden.\r\n");
        return;
    }
    puts("Kennung  Tag                                     Zeit   Befehlszeile\r\n");
    puts("-------------------------------------------------------------------------------\r\n");
    for (i = 0; i < nj; i++) {
        put_num(jobs[i].id, 7, 0);
        puts("  ");
        daytext(d, &jobs[i]);
        pad(d, 40);
        hhmm(t, jobs[i].tm);
        pad(t, 7);
        puts(jobs[i].cmd);
        puts("\r\n");
    }
}

static void show(struct job *j)
{
    static char d[48], t[8];
    unsigned y, m, dd;
    puts("Auftragskennung: ");
    put_num(j->id, 0, 0);
    puts("\r\nPlanen:          ");
    daytext(d, j);
    puts(d);
    puts("\r\nZeit:            ");
    hhmm(t, j->tm);
    puts(t);
    puts("\r\nN\x84" "chster Lauf:   ");
    dn2ymd(j->due, &y, &m, &dd);
    ctry_date(d, (unsigned)(((y - 1980) << 9) | (m << 5) | dd));
    puts(d);
    puts("\r\nInteraktiv:      ");
    puts(j->flags & JF_INTER ? "Ja" : "Nein");
    puts("\r\nBefehl:          ");
    puts(j->cmd);
    puts("\r\n");
}

/* ---- Dienst -------------------------------------------------------- */
static void logline(struct job *j, unsigned rc)
{
    static struct fapi_dirent e;
    static char d[16], t[8];
    unsigned h, n, big = 0;

    for (n = 0; DosFindEntry(&e, n, sysd) != FAPI_ERROR; n++)
        if (same(e.name, "AT.LOG")) {
            big = e.size > 16384UL;
            break;
        }
    h = big ? DosCreate(logp) : DosAppend(logp);
    if (FAPI_FAILED(h))
        return;
    ctry_date(d, dt[0]);
    hhmm(t, nowmin);
    scopy(line, d, sizeof(line));
    scat(line, " ", sizeof(line));
    scat(line, t, sizeof(line));
    scat(line, "  Kennung ", sizeof(line));
    n = slen(line);
    unum(line + n, j->id)[-1] = 0;
    scat(line, ": ", sizeof(line));
    scat(line, j->cmd, sizeof(line));
    scat(line, rc ? "  - nicht gestartet\r\n" : "\r\n", sizeof(line));
    DosWrite(h, line, slen(line));
    DosClose(h);
}

static unsigned run(struct job *j)
{
    static char cmd[FAPI_PATHLEN + CMDMAX + 8];
    cmd[0] = 0;
    if (FAPI_FAILED(DosGetEnv("COMSPEC", cmd, FAPI_PATHLEN)) || !cmd[0])
        scopy(cmd, "CMD.EXE", sizeof(cmd));
    scat(cmd, " /C ", sizeof(cmd));
    scat(cmd, j->cmd, sizeof(cmd));
    return DosExecPgm(cmd, (j->flags & JF_INTER) ? EXEC_ASYNC : EXEC_DETACH);
}

/* faellige Auftraege starten, Termine weiterruecken; 1 = Liste geaendert */
static int tick(void)
{
    unsigned i, late, d, ch = 0;
    struct job *j;

    for (i = 0; i < nj;) {
        j = &jobs[i];
        if (!j->due || j->due > today || (j->due == today && j->tm > nowmin)) {
            i++;
            continue;
        }
        late = today - j->due > 1 ? 0xFFFF
             : (today - j->due) * 1440 + nowmin - j->tm;
        if (late <= 60)
            logline(j, run(j));
        ch = 1;
        d = j->due;
        if (j->kind == K_ONCE) {
            drop(i);
            continue;
        }
        if (j->kind == K_NEXT) {
            unsigned m = mday(d);
            j->dmask &= ~(1u << wday(d));
            j->mm[m >> 4] &= ~(1u << (m & 15));
            if (!j->dmask && !j->mm[0] && !j->mm[1]) {
                drop(i);
                continue;
            }
        }
        j->due = next_due(j, d + 1);
        i++;
    }
    return ch;
}

static void service(void)
{
    unsigned lh, k;

    k = DosOpen("NUL");                 /* keine Ausgaben                */
    if (!FAPI_FAILED(k)) {
        DosDup2(k, 0);
        DosDup2(k, 1);
        DosDup2(k, 2);
        DosClose(k);
    }
    DosSelectDisk(up(lst[0]) - 'A');    /* Befehle laufen im Stamm       */
    DosChDir("\\");
    for (;;) {
        lh = DosCreate(lck);
        if (FAPI_FAILED(lh))            /* laeuft schon                  */
            return;
        for (;;) {
            read_jobs();
            if (!nj)
                break;
            now();
            if (tick())
                write_jobs();
            DosSleep((60 - nowsec) * 1000u + 500);
        }
        DosClose(lh);
        DosDelete(lck);
        read_jobs();                    /* inzwischen etwas dazugekommen? */
        if (!nj)
            return;
    }
}

static void start_service(void)
{
    static char cmd[FAPI_PATHLEN];
    unsigned h = DosCreate(lck);
    if (h == FAPI_ERR_INUSE)
        return;
    if (!FAPI_FAILED(h)) {
        DosClose(h);
        DosDelete(lck);
    }
    cmd[0] = lst[0];
    cmd[1] = 0;
    scat(cmd, ":\\DOS\\AT.EXE /SERVICE", sizeof(cmd));
    /* Der Dienst erbt die Standard-Handles - eine Umleitung des Aufrufers
       (AT ... > DATEI) bliebe sonst offen, bis er sie selbst nach NUL
       legt. Also vorher hier schon NUL einsetzen (AT ist danach fertig). */
    h = DosOpen("NUL");
    if (!FAPI_FAILED(h)) {
        DosDup2(h, 0);
        DosDup2(h, 1);
        DosDup2(h, 2);
        DosClose(h);
    }
    if (DosExecPgm(cmd, EXEC_DETACH)) {
        DosDup2(0xFFFF, 1);
        puts("AT: Der Zeitplandienst kann nicht gestartet werden.\r\n");
    }
}

/* ---- Eingabe ------------------------------------------------------- */
static int is_opt(const char *a, const char *name)
{
    unsigned i;
    if (*a != '/')
        return 0;
    for (i = 0; name[i]; i++)
        if (up(a[1 + i]) != name[i])
            return 0;
    return a[1 + i] == 0 || a[1 + i] == ' ' || a[1 + i] == ':' || a[1 + i] == '\t';
}

/* Zeit hh:mm[:ss] [A|AM|P|PM] -> Minuten; p hinter das Wort */
static int parse_time(char **pp, unsigned *out)
{
    char *p = *pp;
    unsigned h = 0, m = 0, n = 0, s;
    while (*p >= '0' && *p <= '9' && n < 2)
        h = h * 10 + (*p++ - '0'), n++;
    if (!n)
        return -1;
    if (*p == ':' || *p == '.') {
        p++;
        if (!(*p >= '0' && *p <= '9' && p[1] >= '0' && p[1] <= '9'))
            return -1;
        m = (p[0] - '0') * 10 + (p[1] - '0');
        p += 2;
        if (*p == ':' || *p == '.') {
            p++;
            p = num(p, &s);
        }
    }
    if (*p == ' ' && (up(p[1]) == 'A' || up(p[1]) == 'P') &&
        (p[2] == ' ' || p[2] == 0 || (up(p[2]) == 'M' && (p[3] == ' ' || !p[3]))))
        p++;
    if (up(*p) == 'A' || up(*p) == 'P') {
        if (h < 1 || h > 12)
            return -1;
        if (h == 12)
            h = 0;
        if (up(*p) == 'P')
            h += 12;
        p++;
        if (up(*p) == 'M')
            p++;
    }
    if (h > 23 || m > 59 || (*p && *p != ' ' && *p != '\t'))
        return -1;
    *out = h * 60 + m;
    *pp = p;
    return 0;
}

static int prefix(const char *t, const char *full)
{
    while (*t && *t == *full)
        t++, full++;
    return !*t;
}

/* Tagesliste "Mo,Mi,15" -> Masken; 0 = in Ordnung */
static int parse_days(char *p, struct job *j)
{
    static char t[16];
    unsigned k, i, d;
    while (*p && *p != ' ' && *p != '\t') {
        for (k = 0; *p && *p != ',' && *p != ' ' && *p != '\t'; p++)
            if (k < sizeof(t) - 1)
                t[k++] = up(*p);
        t[k] = 0;
        if (*p == ',')
            p++;
        if (!k)
            continue;
        if (t[0] >= '0' && t[0] <= '9') {
            num(t, &d);
            if (d < 1 || d > 31)
                return -1;
            j->mm[d >> 4] |= 1u << (d & 15);
            continue;
        }
        d = 7;
        if (k == 1) {
            static const char one[] = "MTW?F";
            for (i = 0; i < 5; i++)
                if (one[i] == t[0])
                    d = i;
            if (t[0] == 'S')
                d = 5;
        } else if (same(t, "TH") || same(t, "DO"))
            d = 3;
        else if (same(t, "SU") || same(t, "SO"))
            d = 6;
        else
            for (i = 0; i < 14; i++)
                if (prefix(t, wfull[i])) {
                    d = i % 7;
                    break;
                }
        if (d > 6)
            return -1;
        j->dmask |= 1u << d;
    }
    return 0;
}

static void usage(void)
{
    puts("Plant Befehle und Programme f\x81r feste Zeiten.\r\n\r\n"
         "AT [\\\\Computer] [Kennung] [/DELETE [/YES]]\r\n"
         "AT [\\\\Computer] Zeit [/INTERACTIVE] [/EVERY:Tag[,...] | /NEXT:Tag[,...]]\r\n"
         "   \"Befehl\"\r\n\r\n"
         "  Kennung       Nummer eines geplanten Auftrags\r\n"
         "  /DELETE       l\x94scht den Auftrag, ohne Kennung alle\r\n"
         "  /YES          ohne R\x81" "ckfrage l\x94schen\r\n"
         "  Zeit          hh:mm (24 Stunden, auch 8:00PM)\r\n"
         "  /INTERACTIVE  Befehl in einer eigenen Sitzung mit Tastatur\r\n"
         "  /EVERY:Tag    jeden genannten Wochentag (Mo Di Mi Do Fr Sa So,\r\n"
         "                auch M T W Th F S Su) oder Monatstag (1-31)\r\n"
         "  /NEXT:Tag     nur am n\x84" "chsten genannten Tag\r\n"
         "  \"Befehl\"      Programm, Stapeldatei oder Befehl (l\x84uft mit COMSPEC /C)\r\n\r\n"
         "Ohne /EVERY und /NEXT l\x84uft der Befehl einmal (heute, sonst morgen).\r\n"
         "Die Auftr\x84ge stehen in \\DOS\\SYSTEM\\AT.LST, ausgef\x81hrt vom Dienst\r\n"
         "AT /SERVICE (RUN= in der CONFIG.SYS); Protokoll in \\DOS\\SYSTEM\\AT.LOG.\r\n");
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

void main(void)
{
    static struct job nw;
    char *a, *n;
    unsigned id = 0, i, k;
    int del = 0, yes = 0, timed = 0;

    ctry_init();
    sysfile(lst, "AT.LST");
    sysfile(lck, "AT.LCK");
    sysfile(logp, "AT.LOG");
    sysfile(sysd, "");
    DosGetArgs(args);
    a = skip(args);
    if (is_opt(a, "SERVICE")) {
        service();
        return;
    }
    if (a[0] == '/' && a[1] == '?') {
        usage();
        return;
    }
    if (a[0] == '\\' && a[1] == '\\') {
        puts("AT: Auftr\x84ge auf anderen Computern werden nicht unterst\x81tzt.\r\n");
        DosExit(1);
    }
    now();
    read_jobs();

    /* Zeit gefolgt von Optionen und Befehl? */
    if (*a >= '0' && *a <= '9') {
        n = a;
        while (*n >= '0' && *n <= '9')
            n++;
        if (*n == ':' || *n == '.' || up(*n) == 'A' || up(*n) == 'P')
            timed = 1;
    }
    if (!timed) {
        for (; *a; a = next_word(a)) {
            if (is_opt(a, "DELETE"))
                del = 1;
            else if (is_opt(a, "YES"))
                yes = 1;
            else if (*a >= '0' && *a <= '9')
                num(a, &id);
            else {
                puts("Ung\x81ltiger Parameter - ");
                word(a, line, sizeof(line));
                puts(line);
                puts("\r\n");
                DosExit(1);
            }
        }
        if (del && !id) {
            if (!nj) {
                puts("Es sind keine Eintr\x84ge in der Liste vorhanden.\r\n");
                return;
            }
            if (!yes) {
                puts("Dieser Vorgang l\x94scht alle geplanten Auftr\x84ge.\r\n"
                     "M\x94" "chten Sie den Vorgang fortsetzen (J/N)? ");
                if (!ask_yn())
                    return;
            }
            nj = 0;
            write_jobs();
            return;
        }
        if (id) {
            for (i = 0; i < nj && jobs[i].id != id; i++)
                ;
            if (i == nj) {
                puts("Die AT-Auftragskennung ist nicht vorhanden.\r\n");
                DosExit(1);
            }
            if (del) {
                drop(i);
                if (write_jobs())
                    goto werr;
            } else
                show(&jobs[i]);
            return;
        }
        list();
        return;
    }

    if (parse_time(&a, &nw.tm)) {
        puts("Ung\x81ltige Zeitangabe.\r\n");
        DosExit(1);
    }
    nw.kind = K_ONCE;
    for (a = skip(a); *a == '/'; a = skip(next_word(a))) {
        if (is_opt(a, "INTERACTIVE"))
            nw.flags |= JF_INTER;
        else if (is_opt(a, "EVERY") || is_opt(a, "NEXT")) {
            nw.kind = up(a[1]) == 'E' ? K_EVERY : K_NEXT;
            n = a + 1;
            while (*n && *n != ':' && *n != ' ')
                n++;
            if (*n == ':' && n[1] && n[1] != ' ') {
                if (parse_days(n + 1, &nw)) {
                    puts("Ung\x81ltige Tagesangabe.\r\n");
                    DosExit(1);
                }
            } else {                    /* ohne Tag: heutiger Monatstag */
                k = mday(today);
                nw.mm[k >> 4] |= 1u << (k & 15);
            }
        } else {
            puts("Ung\x81ltiger Parameter - ");
            word(a, line, sizeof(line));
            puts(line);
            puts("\r\n");
            DosExit(1);
        }
    }
    /* Rest = Befehl, aeussere Anfuehrungszeichen weg */
    k = slen(a);
    while (k && (a[k - 1] == ' ' || a[k - 1] == '\t'))
        a[--k] = 0;
    if (*a == '"' && k >= 2 && a[k - 1] == '"') {
        a[k - 1] = 0;
        a++;
    }
    if (!*a) {
        puts("Es wurde kein Befehl angegeben.\r\n");
        DosExit(1);
    }
    if (slen(a) >= CMDMAX) {
        puts("Der Befehl ist zu lang.\r\n");
        DosExit(1);
    }
    if (nj >= NJOB) {
        puts("Es sind schon zu viele Auftr\x84ge geplant.\r\n");
        DosExit(1);
    }
    scopy(nw.cmd, a, CMDMAX);
    nw.due = next_due(&nw, nw.tm > nowmin ? today : today + 1);
    nw.id = 1;
    for (i = 0; i < nj; i++)
        if (jobs[i].id >= nw.id)
            nw.id = jobs[i].id + 1;
    jcopy(&jobs[nj++], &nw);
    if (write_jobs())
        goto werr;
    puts("Neuer Auftrag mit Kennung = ");
    put_num(nw.id, 0, 0);
    puts(" hinzugef\x81gt.\r\n");
    start_service();
    return;
werr:
    puts("AT: ");
    puts(lst);
    puts(" kann nicht geschrieben werden.\r\n");
    DosExit(1);
}
