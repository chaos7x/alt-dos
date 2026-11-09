/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TASKMAN.EXE - Taskmanager: laufende Programme und DOS-Boxen zeigen und
 * beenden.
 *
 *   TASKMAN              Vollbild: Pfeile waehlen, Entf beendet (mit
 *                        Rueckfrage), Esc schliesst; die Liste wird jede
 *                        Sekunde neu gelesen
 *   TASKMAN /L           Liste auf die Standardausgabe
 *   TASKMAN /K n|Name    Task Nummer n bzw. alle mit diesem Programmnamen
 *                        beenden (ohne Rueckfrage)
 *
 * Beendet wird immer mit allem, was der Task gestartet hat (eine Sitzung
 * mit CMD und laufender DOS-Box geht ganz zu). Nicht beendbar: TASKMAN
 * selbst, seine Aufrufer und die Shell der ersten Sitzung. Haengt eine
 * DOS-Box im Vordergrund, geht auch Strg+Alt+Ende.
 */
#include "fapi.h"
#include "util.h"

#define NT      8                       /* Plaetze (NTASK im Kernel)      */

static struct fapi_proc pr[NT];
static unsigned char used[NT];
static char args[FAPI_ARGLEN], row[81];
static unsigned rows;

static void read_all(void)
{
    unsigned t;
    for (t = 0; t < NT; t++)
        used[t] = DosProc(PROC_INFO, t, &pr[t]) == 0 && pr[t].state != 0;
}

static const char *kind_s(unsigned t)
{
    return pr[t].kind == 1 ? "DOS-Box" : pr[t].kind == 2 ? "DOS-Box (ruht)" : "Programm";
}

static const char *state_s(unsigned t)
{
    return pr[t].state == 2 ? "wartet" : pr[t].state == 3 ? "Ger\x84t" : "l\x84uft";
}

/* Zeile fuer Task t in row (80 Zeichen) */
static void fmt(unsigned t)
{
    char *p = row;
    unsigned i;
    const char *s;
    for (i = 0; i < 80; i++)
        row[i] = ' ';
    row[80] = 0;
    row[1] = (char)('0' + t);
    p = row + 4;
    for (s = pr[t].name; *s && p < row + 17; )
        *p++ = *s++;
    p = row + 18;
    for (s = kind_s(t); *s; )
        *p++ = *s++;
    row[34] = (char)('1' + pr[t].session);
    p = row + 39;
    for (s = state_s(t); *s; )
        *p++ = *s++;
    p = row + 47;
    if (pr[t].kb) {
        i = pr[t].kb;
        if (i >= 1000) *p++ = (char)('0' + i / 1000 % 10);
        if (i >= 100) *p++ = (char)('0' + i / 100 % 10);
        if (i >= 10) *p++ = (char)('0' + i / 10 % 10);
        *p++ = (char)('0' + i % 10);
        *p++ = 'K';
    } else
        *p++ = '-';
    p = row + 54;
    for (s = skip(pr[t].args); *s && p < row + 79; )
        *p++ = *s++;
    if (pr[t].flags & PROC_SELF)
        row[0] = '*';
}

static const char head[] =
    "  Nr  Programm     Art            Sitzung  Zustand  Gr\x94\xE1" "e  Befehlszeile";

static void list(void)
{
    unsigned t, n;
    read_all();
    puts(head);
    puts("\r\n");
    for (t = 0; t < NT; t++)
        if (used[t]) {
            fmt(t);
            for (n = 79; n && row[n - 1] == ' '; n--)
                ;
            row[n] = 0;
            puts(row);
            puts("\r\n");
        }
}

/* t und alles darunter beenden (Kinder zuerst) -> 0 ok */
static unsigned kill_tree(unsigned t)
{
    unsigned c, r;
    read_all();
    for (c = 0; c < NT; c++)
        if (used[c] && pr[c].parent == t && c != t)
            kill_tree(c);
    r = DosProc(PROC_KILL, t, &pr[t]);
    return r;
}

static void kill_cmd(char *a)
{
    unsigned t, n = 0, r;
    char w[16];
    word(a, w, sizeof(w));
    read_all();
    if (w[0] >= '0' && w[0] <= '9' && !w[1]) {
        t = (unsigned)(w[0] - '0');
        if (t >= NT || !used[t]) {
            puts("Kein Task mit dieser Nummer.\r\n");
            DosExit(1);
        }
        r = kill_tree(t);
        if (r) {
            puts(r == FAPI_ERR_DENIED ? "Dieser Task kann nicht beendet werden.\r\n" :
                                        "Beenden fehlgeschlagen.\r\n");
            DosExit(1);
        }
        puts("Beendet.\r\n");
        return;
    }
    for (t = 0; t < NT; t++) {
        char nm[16];
        unsigned i;
        if (!used[t])
            continue;
        scopy(nm, pr[t].name, sizeof(nm));
        for (i = 0; nm[i] && nm[i] != '.'; i++)
            ;
        if (!same(pr[t].name, w)) {     /* auch ohne Endung          */
            if (nm[i] != '.')
                continue;
            nm[i] = 0;
            if (!same(nm, w))
                continue;
        }
        if (pr[t].flags & (PROC_SELF | PROC_PROT))
            continue;
        if (kill_tree(t) == 0)
            n++;
        read_all();
    }
    if (!n) {
        puts("Kein passender Task beendet.\r\n");
        DosExit(1);
    }
    puts("Beendet.\r\n");
}

/* ---- Vollbild --------------------------------------------------------- */
static void put_at(unsigned r, unsigned c, const char *s, unsigned a)
{
    VioWrtStrAtt(r, c, (char *)s, a);
}

static void fill(unsigned r, unsigned a)
{
    unsigned i;
    for (i = 0; i < 80; i++)
        row[i] = ' ';
    row[80] = 0;
    put_at(r, 0, row, a);
}

static void draw(unsigned sel, const char *msg)
{
    unsigned t, r = 3;
    fill(0, 0x70);
    put_at(0, 30, "AT-DOS Taskmanager", 0x70);
    fill(1, 0x17);
    fill(2, 0x1E);
    put_at(2, 0, head, 0x1E);
    for (t = 0; t < NT; t++) {
        if (!used[t])
            continue;
        fmt(t);
        put_at(r++, 0, row, t == sel ? 0x70 : (pr[t].flags & PROC_PROT) ? 0x18 : 0x17);
    }
    while (r < rows - 2)
        fill(r++, 0x17);
    fill(rows - 2, 0x17);
    if (msg)
        put_at(rows - 2, 2, msg, 0x1F);
    fill(rows - 1, 0x70);
    put_at(rows - 1, 1, "Pfeile w\x84hlen  Entf beendet (mit allem, was es gestartet hat)  Esc schlie\xE1t", 0x70);
}

static int valid(unsigned *sel)
{
    unsigned t;
    if (*sel < NT && used[*sel])
        return 1;
    for (t = 0; t < NT; t++)
        if (used[t]) {
            *sel = t;
            return 1;
        }
    return 0;
}

static void step(unsigned *sel, int d)
{
    int t = (int)*sel;
    for (;;) {
        t += d;
        if (t < 0 || t >= NT)
            return;
        if (used[t]) {
            *sel = (unsigned)t;
            return;
        }
    }
}

static void interactive(void)
{
    unsigned sel = 0, k, c, n, r;
    const char *msg = 0;
    static char q[80];
    rows = VioSetMode(1);
    if (FAPI_FAILED(rows) || rows < 25)
        rows = 25;
    VioSetCurPos(0xFF, 0);
    for (;;) {
        read_all();
        valid(&sel);
        draw(sel, msg);
        for (n = 0; n < 20 && !KbdPeek(); n++)  /* 1 s, dann neu lesen */
            DosSleep(50);
        if (!KbdPeek())
            continue;
        msg = 0;
        k = KbdCharIn();
        c = k & 0xFF;
        if (c == 27)
            break;
        if (c && c != 0xE0)
            continue;
        c = k >> 8;
        if (c == 0x48)
            step(&sel, -1);
        else if (c == 0x50)
            step(&sel, 1);
        else if (c == 0x53 && valid(&sel)) {    /* Entf                */
            if (pr[sel].flags & (PROC_PROT | PROC_SELF)) {
                msg = "Dieser Task kann nicht beendet werden.";
                continue;
            }
            scopy(q, pr[sel].name, 13);
            scat(q, " beenden (J/N)?", sizeof(q));
            fill(rows - 2, 0x17);
            put_at(rows - 2, 2, q, 0x1F);
            k = KbdCharIn() & 0xFF;
            if (up((char)k) != 'J')
                continue;
            r = kill_tree(sel);
            msg = r ? "Beenden fehlgeschlagen." : "Beendet.";
        }
    }
    VioSetMode(0);
}

void main(void)
{
    char *a;
    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Zeigt laufende Programme und DOS-Boxen und beendet sie.\r\n\r\n"
             "TASKMAN [/L | /K Nummer|Name]\r\n\r\n"
             "  (ohne)  Vollbild: Pfeile w\x84hlen, Entf beendet, Esc schlie\xE1t\r\n"
             "  /L      Liste ausgeben\r\n"
             "  /K      Task (mit allem, was er gestartet hat) beenden\r\n\r\n"
             "Eine h\x84ngende DOS-Box im Vordergrund beendet auch Strg+Alt+Ende.\r\n");
        return;
    }
    if (a[0] == '/' && up(a[1]) == 'L') {
        list();
        return;
    }
    if (a[0] == '/' && up(a[1]) == 'K') {
        kill_cmd(skip(a + 2));
        return;
    }
    interactive();
}
