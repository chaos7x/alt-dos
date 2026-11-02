/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FM.EXE - AT-DOS Dateimanager: zwei Verzeichnisfenster nebeneinander,
 * Funktionstasten unten, Befehlszeile darueber.
 *
 *   Tab        anderes Fenster          Eingabe    Verzeichnis oeffnen /
 *   Einfg      markieren                           Programm starten
 *   Grau + - * alle / keine / umkehren  F3 Ansehen (LESS)  F4 Bearbeiten (EDIT)
 *   F5 Kopieren  F6 Verschieben/Umbenennen  F7 Verzeichnis  F8 Loeschen
 *   Alt+F1/F2  Laufwerk links/rechts    Strg+O Bildschirm der Sitzung
 *   Strg+R     neu einlesen             F10/Esc (leere Zeile) Ende
 *
 * Tippen geht in die Befehlszeile; Eingabe startet sie ueber COMMAND /C im
 * Verzeichnis des aktiven Fensters (CD wechselt das Fenster). Maus: Klick
 * waehlt, zweiter Klick oeffnet, rechte Taste markiert, Klick auf die
 * Tastenleiste loest die Taste aus. Lange Namen (FX_FLAG_LONGNAMES).
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

#define A_PANEL     0x1B            /* hellcyan auf blau               */
#define A_DIR       0x1F            /* Verzeichnisse weiss             */
#define A_MARK      0x1E            /* markiert gelb                   */
#define A_CUR       0x30            /* Balken schwarz auf cyan         */
#define A_CURMARK   0x3E
#define A_HEAD      0x1E            /* Spaltenkoepfe gelb              */
#define A_KEYNUM    0x07
#define A_KEYTXT    0x30
#define A_CMD       0x07
#define A_DLG       0x70
#define A_FIELD     0x07

#define K_UP        0x48
#define K_DOWN      0x50
#define K_LEFT      0x4B
#define K_RIGHT     0x4D
#define K_HOME      0x47
#define K_END       0x4F
#define K_PGUP      0x49
#define K_PGDN      0x51
#define K_INS       0x52
#define K_DEL       0x53
#define K_F1        0x3B
#define K_F10       0x44
#define K_ALTF1     0x68
#define K_ALTF2     0x69
#define K_GPLUS     0x4E
#define K_GMINUS    0x4A
#define K_GSTAR     0x37
#define K_MOUSE     0xFE00

#define NENT        350
#define POOL        8000

struct ent {
    unsigned       off;             /* Name im Pool                    */
    unsigned char  attr, mark;
    unsigned long  size;
    unsigned short date, time;
};

struct panel {
    char          dir[FAPI_PATHLEN];    /* "C:\DOS", Stamm "C:\"        */
    struct ent    e[NENT];
    char          pool[POOL];
    unsigned      n, pool_n, cur, top, nmark;
    unsigned long mbytes;
};

static struct panel pn[2];
static int act;                     /* aktives Fenster                  */
static unsigned R = 25;             /* Bildschirmzeilen                 */
static unsigned lvb;
static unsigned char dlo = 0xFF, dhi;
static char cmdl[128];
static unsigned cmdn;
static char tmp[FAPI_PATHLEN], tmp2[FAPI_PATHLEN], line[82];
static int mouse, quit;
static struct fapi_mouevent mev;
static unsigned char mprev;
static volatile int shut_req;

#define NAME(p, i)  ((p)->pool + (p)->e[i].off)
#define PROWS()     (R - 7)         /* Eintragszeilen je Fenster        */

/* ---- Bildschirm: logischer Puffer wie in EDIT ------------------------ */
static void put(int r, int c, const char *s, unsigned char a, int w)
{
    unsigned short __far *p;
    if (r < 0 || r >= (int)R || c >= 80)
        return;
    p = MK_FP(lvb, (r * 80 + c) * 2);
    while (w-- > 0 && c++ < 80)
        *p++ = (unsigned char)(*s ? *s++ : ' ') | ((unsigned)a << 8);
    if (dlo == 0xFF)
        dlo = dhi = (unsigned char)r;
    else if (r < dlo)
        dlo = (unsigned char)r;
    else if (r > dhi)
        dhi = (unsigned char)r;
}

static void putch(int r, int c, char ch, unsigned char a)
{
    char s[2];
    s[0] = ch;
    s[1] = 0;
    put(r, c, s, a, 1);
}

static void show(void)
{
    if (dlo != 0xFF) {
        VioShowBuf(dlo, dhi);
        dlo = 0xFF;
        dhi = 0;
    }
}

/* Zahl mit Tausenderpunkten, ohne 32-Bit-Division */
static void fmt_num(unsigned long v, char *o)
{
    static const unsigned long pw[10] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL };
    unsigned i, n = 0, d, any = 0;
    for (i = 0; i < 10; i++) {
        for (d = 0; v >= pw[i]; d++)
            v -= pw[i];
        if (d || any || i == 9) {
            o[n++] = (char)('0' + d);
            any = 1;
            if (i == 0 || i == 3 || i == 6)
                o[n++] = '.';
        }
    }
    o[n] = 0;
}

static void fmt2(unsigned v, char *o)
{
    o[0] = (char)('0' + v / 10);
    o[1] = (char)('0' + v % 10);
    o[2] = 0;
}

/* ---- Tasten und Maus ------------------------------------------------- */
static unsigned getkey(void)
{
    unsigned k;
    show();
    for (;;) {
        if (shut_req)
            DosExit(0);                 /* nichts zu sichern            */
        if (!mouse || KbdPeek()) {
            k = KbdCharIn();
            if (k || !shut_req)
                return k;
            continue;
        }
        if (MouReadEvent(&mev, 0) == 0) {
            if (mev.buttons != mprev)
                return K_MOUSE;
            continue;
        }
        DosSleep(20);
    }
}

static void on_signal(unsigned sig)
{
    if (sig == SIG_SHUTDOWN)
        shut_req = 1;
}

/* ---- Verzeichnis einlesen -------------------------------------------- */
static int ent_less(struct panel *p, unsigned a, unsigned b)
{
    const char *x = NAME(p, a), *y = NAME(p, b);
    unsigned da = (p->e[a].attr & 0x10) != 0, db = (p->e[b].attr & 0x10) != 0;
    if (x[0] == '.' && x[1] == '.' && !x[2])
        return 1;
    if (y[0] == '.' && y[1] == '.' && !y[2])
        return 0;
    if (da != db)
        return da > db;
    while (*x && up(*x) == up(*y))
        x++, y++;
    return (unsigned char)up(*x) < (unsigned char)up(*y);
}

static void ent_swap(struct panel *p, unsigned a, unsigned b)
{
    unsigned char t, *x = (unsigned char *)&p->e[a], *y = (unsigned char *)&p->e[b];
    unsigned i;
    for (i = 0; i < sizeof(struct ent); i++) {
        t = x[i];
        x[i] = y[i];
        y[i] = t;
    }
}

static void ent_add(struct panel *p, const char *name, struct fapi_dirent *d)
{
    unsigned n = slen(name) + 1;
    struct ent *e;
    if (p->n >= NENT || p->pool_n + n > POOL)
        return;
    e = &p->e[p->n++];
    e->off = p->pool_n;
    scopy(p->pool + p->pool_n, name, n);
    p->pool_n += n;
    e->mark = 0;
    e->attr = d ? d->attr : 0x10;
    e->size = d ? d->size : 0;
    e->date = d ? d->date : 0;
    e->time = d ? d->time : 0;
}

/* einlesen; keep: Name, auf den der Balken soll (0 = Position halten) */
static void read_panel(struct panel *p, const char *keep)
{
    static struct fapi_dirent d;
    unsigned i, j, gap, old = p->cur;

    p->n = p->pool_n = p->nmark = 0;
    p->mbytes = 0;
    if (slen(p->dir) > 3)
        ent_add(p, "..", 0);
    for (i = 0; DosFindEntry(&d, i, p->dir) != FAPI_ERROR; i++) {
        if ((d.name[0] == '.' && (!d.name[1] || (d.name[1] == '.' && !d.name[2]))) ||
            (d.attr & 0x0E))            /* versteckt, System, Bezeichnung */
            continue;
        ent_add(p, d.name, &d);
    }
    for (gap = p->n / 2; gap; gap /= 2)         /* Shellsort            */
        for (i = gap; i < p->n; i++)
            for (j = i; j >= gap && ent_less(p, j, j - gap); j -= gap)
                ent_swap(p, j, j - gap);
    p->cur = old < p->n ? old : (p->n ? p->n - 1 : 0);
    if (keep && keep[0])
        for (i = 0; i < p->n; i++)
            if (same(NAME(p, i), keep)) {
                p->cur = i;
                break;
            }
}

/* ---- Zeichnen -------------------------------------------------------- */
/* Spalten wie bei den klassischen Dateimanagern: Rahmen 0, Name 1-13,
   Strich 14, Groesse 15-23, Strich 24, Datum 25-32, Strich 33, Zeit 34-38 */
#define C_S1    14
#define C_S2    24
#define C_S3    33

static void frame_line(int r, int x, char l, char m, char j, char rr, unsigned char a)
{
    int i;
    for (i = 0; i < 40; i++)
        line[i] = m;
    line[0] = l;
    line[39] = rr;
    if (j) {
        line[C_S1] = j;
        line[C_S2] = j;
        line[C_S3] = j;
    }
    line[40] = 0;
    put(r, x, line, a, 40);
}

/* Name in die 13er-Spalte: 8.3 als "NAME     EXT", lang gekuerzt */
static void name_col(char *o, const char *nm, int isdir)
{
    unsigned i, l = slen(nm), dot = 0xFFFF;
    for (i = 0; i < 13; i++)
        o[i] = ' ';
    o[13] = 0;
    for (i = 0; i < l; i++)
        if (nm[i] == '.')
            dot = i;
    (void)isdir;
    if (nm[0] == '.' || l > 12 || (dot == 0xFFFF ? l > 8 : (dot > 8 || l - dot - 1 > 3))) {
        for (i = 0; i < l && i < 13; i++)
            o[i] = nm[i];
        if (l > 13)
            o[12] = '\xAF';
        return;
    }
    for (i = 0; i < l && i < (dot == 0xFFFF ? l : dot); i++)
        o[i] = nm[i];
    if (dot != 0xFFFF)
        for (i = dot + 1; i < l; i++)
            o[9 + i - dot - 1] = nm[i];
}

static void plain_num(unsigned long v, char *o)     /* ohne Punkte  */
{
    static char t[16];
    unsigned i, n = 0;
    fmt_num(v, t);
    for (i = 0; t[i]; i++)
        if (t[i] != '.')
            o[n++] = t[i];
    o[n] = 0;
}

static void date_time(const struct ent *e, char *d, char *t)
{
    static char nb[3];
    fmt2(e->date & 31, nb);
    d[0] = nb[0]; d[1] = nb[1]; d[2] = '.';
    fmt2((e->date >> 5) & 15, nb);
    d[3] = nb[0]; d[4] = nb[1]; d[5] = '.';
    fmt2(((e->date >> 9) + 80) % 100, nb);
    d[6] = nb[0]; d[7] = nb[1]; d[8] = 0;
    fmt2(e->time >> 11, nb);
    t[0] = nb[0]; t[1] = nb[1]; t[2] = ':';
    fmt2((e->time >> 5) & 63, nb);
    t[3] = nb[0]; t[4] = nb[1]; t[5] = 0;
}

static void draw_panel(int k)
{
    struct panel *p = &pn[k];
    int x = k * 40, r, i;
    unsigned rows = PROWS(), idx, n;
    unsigned char a, as;
    static char nb[16], db[10], tb[8], nm13[14];
    struct ent *e;

    if (p->cur < p->top)
        p->top = p->cur;
    if (p->cur >= p->top + rows)
        p->top = p->cur - rows + 1;
    /* oben: Rahmen mit Pfad in der Mitte */
    frame_line(0, x, '\xC9', '\xCD', '\xD1', '\xBB', A_PANEL);
    n = slen(p->dir);
    if (n > 34) {
        scopy(tmp, "...", sizeof(tmp));
        scat(tmp, p->dir + n - 31, sizeof(tmp));
    } else
        scopy(tmp, p->dir, sizeof(tmp));
    n = slen(tmp);
    putch(0, x + 20 - (int)n / 2 - 1, ' ', k == act ? A_CUR : A_PANEL);
    put(0, x + 20 - (int)n / 2, tmp, k == act ? A_CUR : A_PANEL, (int)n);
    putch(0, x + 20 - (int)n / 2 + (int)n, ' ', k == act ? A_CUR : A_PANEL);
    /* Spaltenkoepfe */
    frame_line(1, x, '\xBA', ' ', '\xB3', '\xBA', A_PANEL);
    put(1, x + 5, "Name", A_HEAD, 4);
    put(1, x + 17, "Gr\x94\xE1" "e", A_HEAD, 5);
    put(1, x + 26, "Datum", A_HEAD, 5);
    put(1, x + 35, "Zeit", A_HEAD, 4);
    for (r = 0; r < (int)rows; r++) {
        idx = p->top + r;
        frame_line(2 + r, x, '\xBA', ' ', '\xB3', '\xBA', A_PANEL);
        if (idx >= p->n)
            continue;
        e = &p->e[idx];
        a = e->mark ? A_MARK : A_PANEL;
        as = A_PANEL;                   /* Striche bleiben cyan          */
        if (k == act && idx == p->cur)
            a = as = e->mark ? A_CURMARK : A_CUR;
        name_col(nm13, NAME(p, idx), (e->attr & 0x10) != 0);
        put(2 + r, x + 1, nm13, a, 13);
        if (e->attr & 0x10)
            scopy(nb, NAME(p, idx)[0] == '.' ? "\x10HOCH\x11" : "\x10VERZ\x11", sizeof(nb));
        else
            plain_num(e->size, nb);
        n = slen(nb);
        for (i = 0; i < 9; i++)
            line[i] = ' ';
        for (i = 0; i < (int)n && i < 9; i++)
            line[9 - n + i] = nb[i];
        line[9] = 0;
        putch(2 + r, x + C_S1, '\xB3', as);
        put(2 + r, x + C_S1 + 1, line, a, 9);
        putch(2 + r, x + C_S2, '\xB3', as);
        if (e->date) {
            date_time(e, db, tb);
            put(2 + r, x + C_S2 + 1, db, a, 8);
            putch(2 + r, x + C_S3, '\xB3', as);
            put(2 + r, x + C_S3 + 1, tb, a, 5);
        } else {
            put(2 + r, x + C_S2 + 1, "", a, 8);
            putch(2 + r, x + C_S3, '\xB3', as);
            put(2 + r, x + C_S3 + 1, "", a, 5);
        }
    }
    /* Trennlinie und Kurzanzeige */
    frame_line(2 + rows, x, '\xC7', '\xC4', '\xC1', '\xB6', A_PANEL);
    for (i = 0; i < 38; i++)
        line[i] = ' ';
    line[38] = 0;
    if (p->nmark) {
        plain_num(p->mbytes, tmp);
        plain_num(p->nmark, nb);
        scopy(line, tmp, sizeof(line));
        scat(line, " Bytes in ", sizeof(line));
        scat(line, nb, sizeof(line));
        scat(line, p->nmark == 1 ? " markierten Datei" : " markierten Dateien", sizeof(line));
        n = slen(line);
        frame_line(3 + rows, x, '\xBA', ' ', 0, '\xBA', A_PANEL);
        put(3 + rows, x + 1 + (n < 38 ? (38 - (int)n) / 2 : 0), line, A_MARK, (int)n);
    } else {
        frame_line(3 + rows, x, '\xBA', ' ', 0, '\xBA', A_PANEL);
        if (p->n) {
            e = &p->e[p->cur];
            for (i = 0; NAME(p, p->cur)[i] && i < 18; i++)
                line[i] = NAME(p, p->cur)[i];
            if (slen(NAME(p, p->cur)) > 18)
                line[17] = '\xAF';
            if (e->attr & 0x10)
                scopy(nb, NAME(p, p->cur)[0] == '.' ? "\x10HOCH\x11" : "\x10VERZ\x11", sizeof(nb));
            else
                plain_num(e->size, nb);
            n = slen(nb);
            for (i = 0; i < (int)n && i < 9; i++)
                line[18 + 9 - n + i] = nb[i];
            put(3 + rows, x + 1, line, A_PANEL, 27);
            if (e->date) {
                date_time(e, db, tb);
                put(3 + rows, x + 29, db, A_PANEL, 8);
            }
        }
    }
    frame_line(4 + rows, x, '\xC8', '\xCD', 0, '\xBC', A_PANEL);
}

static const char *const fkey[10] = {
    "Hilfe", "", "Zeigen", "Bearb.", "Kopie", "UmbVer", "MkDir",
    "L\x94sch.", "", "Ende" };

static void draw_cmd(void)
{
    unsigned n, pl, off = 0;
    scopy(tmp, pn[act].dir, sizeof(tmp));
    scat(tmp, ">", sizeof(tmp));
    pl = slen(tmp);
    if (pl > 40) {
        scopy(tmp, tmp + pl - 40, sizeof(tmp));
        pl = 40;
    }
    n = pl + cmdn;
    if (n > 79)
        off = n - 79;
    put((int)R - 2, 0, tmp, A_CMD, (int)pl);
    put((int)R - 2, (int)pl, cmdl + (off > cmdn ? cmdn : off), A_CMD, 80 - (int)pl);
    VioSetCurPos(R - 2, (int)(n - off < 80 ? n - off : 79));
}

static void draw_all(void)
{
    int i;
    static char nb[3];
    draw_panel(0);
    draw_panel(1);
    for (i = 0; i < 10; i++) {          /* Ziffer weiss auf schwarz,     */
        fmt2((unsigned)(i + 1), nb);    /* Text schwarz auf cyan         */
        put((int)R - 1, i * 8, nb[0] == '0' ? nb + 1 : nb, A_KEYNUM, i == 9 ? 2 : 1);
        put((int)R - 1, i * 8 + (i == 9 ? 2 : 1), fkey[i], A_KEYTXT, i == 9 ? 6 : 6);
        if (i < 9)
            put((int)R - 1, i * 8 + 7, "", A_KEYNUM, 1);
    }
    draw_cmd();
}

/* ---- Dialoge --------------------------------------------------------- */
static void box(int r, int h, const char *title)
{
    static char bl[62];                 /* eigener Puffer: line haelt oft
                                           den Text des Dialogs         */
    int i, c = 10, w = 60;
    for (i = 0; i < h; i++) {
        bl[0] = i == 0 ? '\xC9' : i == h - 1 ? '\xC8' : '\xBA';
        for (c = 1; c < w - 1; c++)
            bl[c] = (i == 0 || i == h - 1) ? '\xCD' : ' ';
        bl[w - 1] = i == 0 ? '\xBB' : i == h - 1 ? '\xBC' : '\xBA';
        bl[w] = 0;
        put(r + i, 10, bl, A_DLG, w);
    }
    i = slen(title);
    put(r, 40 - i / 2 - 1, " ", A_DLG, 1);
    put(r, 40 - i / 2, title, A_DLG, i);
    put(r, 40 - i / 2 + i, " ", A_DLG, 1);
}

/* Knoepfe in einer Dialogzeile, mittig; per Maus anklickbar */
static int btn_row, btn_n, btn_x[4], btn_w[4];

static void buttons(int r, const char *const *lab, int n)
{
    int i, w = 0, x;
    for (i = 0; i < n; i++)
        w += (int)slen(lab[i]) + 4 + (i ? 3 : 0);
    x = 40 - w / 2;
    btn_row = r;
    btn_n = n;
    for (i = 0; i < n; i++) {
        btn_x[i] = x;
        btn_w[i] = (int)slen(lab[i]) + 4;
        put(r, x, "< ", A_DLG, 2);
        put(r, x + 2, lab[i], A_DLG, btn_w[i] - 4);
        put(r, x + btn_w[i] - 2, " >", A_DLG, 2);
        x += btn_w[i] + 3;
    }
}

/* Mausereignis: welcher Knopf? -1 = keiner (Ereignis ist verbraucht) */
static int button_hit(void)
{
    int i, press = (mev.buttons & MOU_LEFT) && !(mprev & MOU_LEFT);
    mprev = mev.buttons;
    if (!press || mev.row != (unsigned char)btn_row)
        return -1;
    for (i = 0; i < btn_n; i++)
        if (mev.col >= btn_x[i] && mev.col < btn_x[i] + btn_w[i])
            return i;
    return -1;
}

/* Meldung mit Ja/Nein; -> 1 ja, 0 nein; ok_only: nur OK */
static int ask(const char *title, const char *l1, const char *l2, int ok_only)
{
    static const char *const yn[2] = { "Ja", "Nein" };
    static const char *const ok[1] = { "OK" };
    unsigned k;
    int r = (int)R / 2 - 3, b;
    box(r, 7, title);
    put(r + 2, 12, l1, A_DLG, 56);
    put(r + 3, 12, l2, A_DLG, 56);
    buttons(r + 5, ok_only ? ok : yn, ok_only ? 1 : 2);
    VioSetCurPos(0xFF, 0);
    for (;;) {
        k = getkey();
        if (k == K_MOUSE) {
            if ((b = button_hit()) >= 0)
                return ok_only ? 1 : b == 0;
            continue;
        }
        if (ok_only)
            return 1;
        k = up((char)k);
        if (k == 'J' || k == 'Y' || k == 13)
            return 1;
        if (k == 'N' || k == 27)
            return 0;
    }
}

/* Eingabefeld; -> 1 OK, 0 Esc */
static int input(const char *title, const char *prompt, char *b, unsigned max)
{
    unsigned k, pos = slen(b), view, n, i;
    unsigned char c, sc;
    static const char *const oc[2] = { "OK", "Abbrechen" };
    int r = (int)R / 2 - 3, bt;
    box(r, 7, title);
    put(r + 2, 12, prompt, A_DLG, 56);
    buttons(r + 5, oc, 2);
    for (;;) {
        n = slen(b);
        view = pos > 54 ? pos - 54 : 0;
        put(r + 3, 12, b + view, A_FIELD, 56);
        VioSetCurPos(r + 3, 12 + pos - view);
        k = getkey();
        if (k == K_MOUSE) {
            if (mev.row == (unsigned char)(r + 3) && mev.col >= 12 && mev.col < 68 &&
                (mev.buttons & MOU_LEFT) && !(mprev & MOU_LEFT)) {
                pos = view + mev.col - 12;      /* Klick ins Feld        */
                if (pos > n)
                    pos = n;
            }
            if ((bt = button_hit()) >= 0)
                return bt == 0;
            continue;
        }
        c = (unsigned char)k;
        sc = (unsigned char)(k >> 8);
        if (c == 13)
            return 1;
        if (c == 27)
            return 0;
        if (c == 0 || c == 0xE0) {
            if (sc == K_LEFT && pos) pos--;
            else if (sc == K_RIGHT && pos < n) pos++;
            else if (sc == K_HOME) pos = 0;
            else if (sc == K_END) pos = n;
            else if (sc == K_DEL && pos < n)
                for (i = pos; i < n; i++)
                    b[i] = b[i + 1];
        } else if (c == 8) {
            if (pos) {
                for (i = pos - 1; i < n; i++)
                    b[i] = b[i + 1];
                pos--;
            }
        } else if (c >= ' ' && n < max - 1) {
            for (i = n + 1; i > pos; i--)
                b[i] = b[i - 1];
            b[pos++] = (char)c;
        }
    }
}

static int op_abort;                    /* Vorgang abgebrochen          */

static void progress(const char *what, const char *name)
{
    static const char *const ab[1] = { "Abbrechen" };
    int r = (int)R / 2 - 2;
    box(r, 6, what);
    put(r + 2, 12, name, A_DLG, 56);
    buttons(r + 4, ab, 1);
    VioSetCurPos(0xFF, 0);
    show();
}

/* waehrend Kopieren/Verschieben/Loeschen: Esc oder Klick auf Abbrechen?
   -> 1 = aufhoeren (nach Rueckfrage) */
static int abort_check(void)
{
    int want = 0;
    if (op_abort)
        return 1;
    if (KbdPeek()) {
        if ((KbdCharIn() & 0xFF) == 27)
            want = 1;
    } else if (mouse && MouReadEvent(&mev, 0) == 0) {
        if (button_hit() == 0)
            want = 1;
    }
    if (!want)
        return 0;
    if (ask("Abbrechen", "Den laufenden Vorgang abbrechen?", "", 0))
        op_abort = 1;
    return op_abort;
}

/* ---- Programme starten ----------------------------------------------- */
static void enter_dir_of_panel(void)
{
    DosSelectDisk(up(pn[act].dir[0]) - 'A');
    DosChDir(pn[act].dir);
}

static void screen_back(void)
{
    VioSetMode(1);
    if (mouse) {
        MouOpen();
        MouShowPtr(1);
    }
    read_panel(&pn[0], 0);
    read_panel(&pn[1], 0);
    draw_all();
}

/* Befehl ueber COMMAND /C im Verzeichnis des aktiven Fensters */
static void run(const char *cmd, int show_cmd)
{
    static char cl[FAPI_PATHLEN + 140];
    unsigned k = 0;

    if (DosGetEnv("COMSPEC", cl, sizeof(cl)) == FAPI_ERROR)
        scopy(cl, "CMD.EXE", sizeof(cl));
    scat(cl, " /C ", sizeof(cl));
    scat(cl, cmd, sizeof(cl));
    enter_dir_of_panel();
    VioSetCurPos(0xFF, 0);
    VioSetMode(0);
    if (show_cmd) {
        puts(pn[act].dir);
        puts(">");
        puts(cmd);
        puts("\r\n");
    }
    k = DosExecPgm(cl, EXEC_SYNC);
    if (k)
        puts("Befehl konnte nicht gestartet werden.\r\n");
    screen_back();
}

static void quote(char *o, const char *path, unsigned max)
{
    unsigned i, sp = 0;
    for (i = 0; path[i]; i++)
        if (path[i] == ' ')
            sp = 1;
    o[0] = 0;
    if (sp)
        scat(o, "\"", max);
    scat(o, path, max);
    if (sp)
        scat(o, "\"", max);
}

static void cur_path(char *o)
{
    struct panel *p = &pn[act];
    join(o, p->dir, NAME(p, p->cur), FAPI_PATHLEN);
}

/* ---- Markieren ------------------------------------------------------- */
static void set_mark(struct panel *p, unsigned i, int m)
{
    struct ent *e = &p->e[i];
    const char *nm = NAME(p, i);
    if (nm[0] == '.' && nm[1] == '.' && !nm[2])
        return;
    if (e->mark == (unsigned char)m)
        return;
    e->mark = (unsigned char)m;
    if (m) {
        p->nmark++;
        p->mbytes += e->size;
    } else {
        p->nmark--;
        p->mbytes -= e->size;
    }
}

/* ---- Kopieren, Verschieben, Loeschen --------------------------------- */
static unsigned src_len;            /* Laenge des Quellstamms           */
static char dst_root[FAPI_PATHLEN];
static int op_err, op_all;

static int confirm_over(const char *dst)
{
    unsigned h;
    if (op_all)
        return 1;
    h = DosOpen(dst);                   /* gibt es das Ziel schon?      */
    if (FAPI_FAILED(h))
        return 1;
    DosClose(h);
    {
        static const char *const ja[4] = { "Ja", "Nein", "Alle", "Abbrechen" };
        int r = (int)R / 2 - 3, b;
        unsigned k;
        box(r, 7, "Datei existiert");
        put(r + 2, 12, dst, A_DLG, 56);
        put(r + 3, 12, "\x9A" "berschreiben?", A_DLG, 56);
        buttons(r + 5, ja, 4);
        for (;;) {
            k = getkey();
            if (k == K_MOUSE) {
                if ((b = button_hit()) < 0)
                    continue;
                k = b == 0 ? 'J' : b == 1 ? 'N' : b == 2 ? 'A' : 27;
            }
            k = up((char)k);
            if (k == 'J' || k == 'Y')
                return 1;
            if (k == 'A') {
                op_all = 1;
                return 1;
            }
            if (k == 'N')
                return 0;
            if (k == 27) {
                op_abort = 1;           /* abbrechen                    */
                return 0;
            }
        }
    }
}

static void dst_of(const char *src, char *o)
{
    scopy(o, dst_root, FAPI_PATHLEN);
    if (src[src_len]) {
        if (slen(o) > 3)
            scat(o, "\\", FAPI_PATHLEN);
        scat(o, src + src_len + (src[src_len] == '\\'), FAPI_PATHLEN);
    }
}

static int copy_visit(char *path, struct fapi_dirent *e, int what, int depth)
{
    (void)e;
    (void)depth;
    if (op_abort || abort_check())
        return WT_STOP;
    if (what == WT_LEAVE)
        return 0;
    dst_of(path, tmp2);
    if (what == WT_DIR) {
        DosMkDir(tmp2);
        return WT_DESCEND;
    }
    progress("Kopieren", path);
    if (confirm_over(tmp2) && copy_file(path, tmp2) && !op_abort)
        op_err++;
    return op_abort ? WT_STOP : 0;
}

static int del_visit(char *path, struct fapi_dirent *e, int what, int depth)
{
    static struct fapi_finfo fi;
    (void)depth;
    if (op_abort || abort_check())
        return WT_STOP;
    if (what == WT_DIR)
        return WT_DESCEND;
    if (what == WT_FILE) {
        progress("L\x94schen", path);
        if (e->attr & 0x07) {
            fi.mask = 1;
            fi.attr = 0;
            DosSetFInfo(path, &fi);
        }
        if (FAPI_FAILED(DosDelete(path))) {
            op_err++;
            return 0;
        }
        return WT_REMOVED;
    }
    if (FAPI_FAILED(DosRmDir(path))) {
        op_err++;
        return 0;
    }
    return WT_REMOVED;
}

/* eine Quelle kopieren (Datei oder ganzer Baum) nach dst */
static void copy_tree(const char *src, int isdir, const char *dst)
{
    static char sp[FAPI_PATHLEN];
    if (!isdir) {
        progress("Kopieren", src);
        if (confirm_over(dst) && copy_file(src, dst) && !op_abort)
            op_err++;
        return;
    }
    scopy(sp, src, sizeof(sp));
    src_len = slen(sp);
    scopy(dst_root, dst, sizeof(dst_root));
    DosMkDir(dst);
    walk_tree(sp, copy_visit);
}

static void remove_tree(const char *src, int isdir)
{
    static char sp[FAPI_PATHLEN];
    if (!isdir) {
        progress("L\x94schen", src);
        if (FAPI_FAILED(DosDelete(src)))
            op_err++;
        return;
    }
    scopy(sp, src, sizeof(sp));
    walk_tree(sp, del_visit);
}

/* Auswahl: markierte Eintraege oder der unter dem Balken */
static unsigned nsel(void)
{
    struct panel *p = &pn[act];
    const char *nm;
    if (p->nmark)
        return p->nmark;
    if (!p->n)
        return 0;
    nm = NAME(p, p->cur);
    return (nm[0] == '.' && nm[1] == '.' && !nm[2]) ? 0 : 1;
}

static int sel_item(unsigned i)
{
    struct panel *p = &pn[act];
    return p->nmark ? p->e[i].mark : i == p->cur;
}

static void do_copy_move(int move)
{
    static char target[FAPI_PATHLEN], src[FAPI_PATHLEN], dst[FAPI_PATHLEN], nb[12];
    struct panel *p = &pn[act];
    unsigned n = nsel(), i;
    int todir;

    if (!n)
        return;
    scopy(target, pn[!act].dir, sizeof(target));
    fmt_num(n, nb);
    scopy(line, n == 1 ? NAME(p, p->cur) : nb, sizeof(line));
    if (n > 1)
        scat(line, " Eintr\x84ge", sizeof(line));
    scat(line, move ? " verschieben nach:" : " kopieren nach:", sizeof(line));
    if (n == 1 && p->nmark)
        for (i = 0; i < p->n; i++)
            if (p->e[i].mark) {
                scopy(line, NAME(p, i), sizeof(line));
                scat(line, move ? " verschieben nach:" : " kopieren nach:", sizeof(line));
            }
    if (!input(move ? "Verschieben / Umbenennen" : "Kopieren", line, target, sizeof(target)))
        return;
    if (!target[0])
        return;
    if (target[1] != ':' && target[0] != '\\') {        /* relativ: zum Fenster */
        join(dst, p->dir, target, sizeof(dst));
        scopy(target, dst, sizeof(target));
    } else
        abs_path(target, dst), scopy(target, dst, sizeof(target));
    todir = is_dir(target);
    if (n > 1 && !todir) {
        ask("Fehler", "Das Ziel ist kein Verzeichnis:", target, 1);
        return;
    }
    op_err = 0;
    op_all = 0;
    op_abort = 0;
    copy_abort = abort_check;
    for (i = 0; i < p->n && !op_abort; i++) {
        if (!sel_item(i))
            continue;
        join(src, p->dir, NAME(p, i), sizeof(src));
        if (todir)
            join(dst, target, NAME(p, i), sizeof(dst));
        else
            scopy(dst, target, sizeof(dst));
        if (same(src, dst))
            continue;
        if (move && up(src[0]) == up(dst[0]) && DosMove(src, dst) == 0)
            continue;                   /* gleiches Laufwerk: umhaengen */
        copy_tree(src, (p->e[i].attr & 0x10) != 0, dst);
        if (move && op_err == 0 && !op_abort)   /* Quelle nur weg, wenn  */
            remove_tree(src, (p->e[i].attr & 0x10) != 0);   /* alles kam */
        if (abort_check())
            break;
    }
    copy_abort = 0;
    read_panel(&pn[0], 0);
    read_panel(&pn[1], 0);
    draw_all();
    if (op_abort)
        ask("Hinweis", "Abgebrochen.", "Bereits Kopiertes bleibt erhalten.", 1);
    else if (op_err > 0)
        ask("Hinweis", "Nicht alles lie\xE1 sich kopieren/verschieben.", "", 1);
}

static void do_delete(void)
{
    static char src[FAPI_PATHLEN], nb[12];
    struct panel *p = &pn[act];
    unsigned n = nsel(), i, dirs = 0;

    if (!n)
        return;
    for (i = 0; i < p->n; i++)
        if (sel_item(i) && (p->e[i].attr & 0x10))
            dirs++;
    if (n == 1)
        scopy(line, p->nmark ? "" : NAME(p, p->cur), sizeof(line));
    else {
        fmt_num(n, nb);
        scopy(line, nb, sizeof(line));
        scat(line, " Eintr\x84ge", sizeof(line));
    }
    if (n == 1 && p->nmark)
        for (i = 0; i < p->n; i++)
            if (p->e[i].mark)
                scopy(line, NAME(p, i), sizeof(line));
    if (!ask("L\x94schen", line, dirs ? "l\x94schen - Verzeichnisse mit ihrem ganzen Inhalt?"
                                     : "l\x94schen?", 0))
        return;
    op_err = 0;
    op_abort = 0;
    for (i = 0; i < p->n && !op_abort; i++)
        if (sel_item(i)) {
            join(src, p->dir, NAME(p, i), sizeof(src));
            remove_tree(src, (p->e[i].attr & 0x10) != 0);
            abort_check();
        }
    read_panel(&pn[0], 0);
    read_panel(&pn[1], 0);
    draw_all();
    if (op_abort)
        ask("Hinweis", "Abgebrochen.", "", 1);
    else if (op_err)
        ask("Hinweis", "Nicht alles lie\xE1 sich l\x94schen.", "", 1);
}

static void do_mkdir(void)
{
    static char nm[FAPI_PATHLEN], path[FAPI_PATHLEN];
    nm[0] = 0;
    if (!input("Verzeichnis anlegen", "Name des neuen Verzeichnisses:", nm, sizeof(nm)) || !nm[0])
        return;
    join(path, pn[act].dir, nm, sizeof(path));
    if (DosMkDir(path))
        ask("Fehler", "Verzeichnis kann nicht angelegt werden:", nm, 1);
    read_panel(&pn[act], nm);
    if (same(pn[!act].dir, pn[act].dir))
        read_panel(&pn[!act], 0);
    draw_all();
}

static void do_drive(int k)
{
    static char d[4];
    static struct fapi_fsinfo fi;
    unsigned dv;
    d[0] = 0;
    if (!input(k ? "Laufwerk rechts" : "Laufwerk links", "Laufwerksbuchstabe:", d, 3) || !d[0]) {
        draw_all();
        return;
    }
    dv = (unsigned)(up(d[0]) - 'A');
    if (dv >= 26 || (DosQFSInfo(dv, &fi) && DosQSubst(dv, tmp))) {
        ask("Fehler", "Ung\x81ltiges Laufwerk.", "", 1);
    } else if (fi.flags & FSI_NOMEDIA) {
        ask("Fehler", fi.bsize == 2048 ? "Laufwerk nicht bereit - keine CD eingelegt."
                                       : "Laufwerk nicht bereit - keine Diskette eingelegt.", "", 1);
    } else {
        if (DosQCurDirD(dv, tmp) != 0) {
            tmp[0] = (char)('A' + dv);
            tmp[1] = ':';
            tmp[2] = '\\';
            tmp[3] = 0;
        }
        scopy(pn[k].dir, tmp, sizeof(pn[k].dir));
        pn[k].cur = pn[k].top = 0;
        read_panel(&pn[k], 0);
        act = k;
    }
    draw_all();
}

static void open_cur(void)
{
    static char old[FAPI_PATHLEN];
    struct panel *p = &pn[act];
    const char *nm;
    unsigned n;

    if (!p->n)
        return;
    nm = NAME(p, p->cur);
    if (p->e[p->cur].attr & 0x10) {
        if (nm[0] == '.' && nm[1] == '.' && !nm[2]) {   /* hoch       */
            n = slen(p->dir);
            while (n > 3 && p->dir[n - 1] != '\\')
                n--;
            scopy(old, p->dir + n, sizeof(old));
            p->dir[n > 3 ? n - 1 : 3] = 0;
        } else {
            join(tmp, p->dir, nm, sizeof(tmp));
            scopy(p->dir, tmp, sizeof(p->dir));
            old[0] = 0;
        }
        p->cur = p->top = 0;
        read_panel(p, old);
        draw_all();
        return;
    }
    n = slen(nm);                       /* Programm oder Stapeldatei   */
    if (n > 4 && (same(nm + n - 4, ".EXE") || same(nm + n - 4, ".CMD") ||
                  same(nm + n - 4, ".BAT"))) {
        cur_path(tmp);
        quote(tmp2, tmp, sizeof(tmp2));
        run(tmp2, 1);
    }
}

/* ---- Betrachter (F3), Verhalten nach dem Vorbild von mcview ----------
   Datei beliebiger Groesse: ein 32-KB-Fenster in einem eigenen Segment
   wird bei Bedarf nachgeladen. Text mit/ohne Umbruch, Hex-Ansicht,
   Suchen mit Hervorhebung, Springen zu Zeile oder Offset. */
#define VWIN    32768U
static unsigned vsel, vh, vhex, vwrap = 1, vleft;
static unsigned long vbase, vlen = 0, vsize, vtop, vmpos = 0xFFFFFFFFUL, vmlen;
static char vname[FAPI_PATHLEN], vfind[64];

static int vb(unsigned long off)
{
    unsigned char __far *w = MK_FP(vsel, 0);
    unsigned n, got;
    if (off >= vsize)
        return -1;
    if (off < vbase || off >= vbase + vlen) {
        vbase = off > VWIN / 2 ? (off - VWIN / 2) & ~511UL : 0;
        DosSeek(vh, &vbase);
        /* DosRead liest nur ins eigene Datensegment: ueber vbuf */
        for (vlen = 0, got = 1; vlen < VWIN && got; vlen += got) {
            static unsigned char vbuf[2048];
            unsigned i;
            n = VWIN - (unsigned)vlen > sizeof(vbuf) ? sizeof(vbuf) : VWIN - (unsigned)vlen;
            got = DosRead(vh, vbuf, n);
            if (got == FAPI_ERROR)
                got = 0;
            for (i = 0; i < got; i++)
                w[(unsigned)vlen + i] = vbuf[i];
        }
        if (off >= vbase + vlen)
            return -1;
    }
    return w[(unsigned)(off - vbase)];
}

/* Textzeile ab off: Anfang der naechsten Bildschirmzeile */
static unsigned long vnext(unsigned long off)
{
    unsigned col = 0;
    int c;
    if (vhex)
        return off + 16 < vsize ? off + 16 : vsize;
    while ((c = vb(off)) >= 0) {
        off++;
        if (c == '\n')
            break;
        col = c == '\t' ? (col + 8) & ~7u : col + 1;
        if (vwrap && col >= 80 && vb(off) != '\n' &&
            !(vb(off) == '\r' && vb(off + 1) == '\n'))
            break;                      /* umbrechen, ausser Zeilenende */
    }
    return off;
}

static unsigned long vline_start(unsigned long off)    /* logische Zeile */
{
    while (off && vb(off - 1) != '\n')
        off--;
    return off;
}

static unsigned long vprev(unsigned long off)
{
    unsigned long s, n;
    if (!off)
        return 0;
    if (vhex)
        return off >= 16 ? off - 16 : 0;
    s = vline_start(off - 1);
    if (!vwrap)
        return s;
    for (;;) {                          /* letzter Umbruch vor off       */
        n = vnext(s);
        if (n >= off)
            return s;
        s = n;
    }
}

static unsigned long vlast_page(void)
{
    unsigned long t = vhex ? (vsize ? (vsize - 1) & ~15UL : 0) : vsize;
    unsigned i;
    for (i = 0; i < PROWS() + 5 - (vhex ? 1 : 0) && t; i++)
        t = vprev(t);
    return t;
}

static unsigned char vattr(unsigned long off)
{
    return (off >= vmpos && off < vmpos + vmlen) ? A_CUR : 0x17;
}

static void vdraw(void)
{
    static const char hx[] = "0123456789ABCDEF";
    static char st[82];
    unsigned rows = R - 2, r, c, col, i;
    unsigned long off = vtop, t, sz;
    int ch;
    char cell[2];

    cell[1] = 0;
    for (r = 0; r < rows; r++) {
        put(1 + r, 0, "", 0x17, 80);
        if (off >= vsize)
            continue;
        if (vhex) {
            for (i = 0; i < 8; i++)
                cell[0] = hx[(unsigned)(off >> (28 - 4 * i)) & 15], put(1 + r, i, cell, 0x13, 1);
            for (i = 0; i < 16 && off + i < vsize; i++) {
                ch = vb(off + i);
                col = 10 + i * 3 + (i >= 8);
                cell[0] = hx[(ch >> 4) & 15];
                put(1 + r, col, cell, vattr(off + i), 1);
                cell[0] = hx[ch & 15];
                put(1 + r, col + 1, cell, vattr(off + i), 1);
                cell[0] = ch < 32 ? '.' : (char)ch;
                put(1 + r, 60 + i, cell, vattr(off + i), 1);
            }
            off += 16;
            continue;
        }
        t = vnext(off);
        for (col = 0, c = 0; off < t; off++) {
            ch = vb(off);
            if (ch == '\n' || ch == '\r')
                continue;
            if (ch == '\t') {
                unsigned nc = (col + 8) & ~7u;
                for (; col < nc; col++)
                    if (col >= vleft && col - vleft < 80)
                        put(1 + r, (int)(col - vleft), " ", vattr(off), 1);
                continue;
            }
            if (col >= vleft && col - vleft < 80) {
                cell[0] = ch < 32 ? '.' : (char)ch;
                put(1 + r, (int)(col - vleft), cell, vattr(off), 1);
            }
            col++;
            c++;
        }
        off = t;
    }
    /* Statuszeile: Name, Position, Prozent (ohne 32-Bit-Division) */
    for (i = 0; i < 80; i++)
        st[i] = ' ';
    st[80] = 0;
    put(0, 0, st, A_CUR, 80);
    put(0, 1, base_name(vname), A_CUR, 40);
    scopy(st, vhex ? "Hex" : vwrap ? "Text, Umbruch" : "Text", sizeof(st));
    put(0, 42, st, A_CUR, 14);
    st[0] = '0';
    st[1] = 'x';
    for (i = 0; i < 8; i++)
        st[2 + i] = hx[(unsigned)(vtop >> (28 - 4 * i)) & 15];
    st[10] = 0;
    put(0, 57, st, A_CUR, 10);
    t = vtop;
    sz = vsize ? vsize : 1;
    while (sz > 600) {
        sz >>= 1;
        t >>= 1;
    }
    fmt_num((unsigned)t * 100 / (unsigned)sz, st);
    scat(st, " %", sizeof(st));
    put(0, 74, st, A_CUR, 6);
}

static void vkeys(void)
{
    static const char *const vk[10] = {
        "Hilfe", "Umbr.", "Ende", "Hex", "Gehe", "", "Suchen", "", "", "Ende" };
    static char nb[3];
    int i;
    for (i = 0; i < 10; i++) {
        fmt2((unsigned)(i + 1), nb);
        put((int)R - 1, i * 8, nb[0] == '0' ? nb + 1 : nb, A_KEYNUM, i == 9 ? 2 : 1);
        put((int)R - 1, i * 8 + (i == 9 ? 2 : 1), i == 1 && vwrap ? "Unumbr" : i == 3 && vhex ? "Text" : vk[i],
            A_KEYTXT, 6);
        if (i < 9)
            put((int)R - 1, i * 8 + 7, "", A_KEYNUM, 1);
    }
}

static int hexval(char c)
{
    c = up(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Suchen ab from: Text ohne Gross/klein, in der Hex-Ansicht Hexbytes */
static void vsearch(unsigned long from)
{
    static unsigned char pat[64];
    unsigned n = 0, i, k;
    unsigned long off;
    int v;
    if (vhex) {
        for (i = 0; vfind[i] && n < sizeof(pat); ) {
            while (vfind[i] == ' ')
                i++;
            if (hexval(vfind[i]) < 0 || hexval(vfind[i + 1]) < 0)
                break;
            pat[n++] = (unsigned char)(hexval(vfind[i]) * 16 + hexval(vfind[i + 1]));
            i += 2;
        }
    } else
        for (n = 0; vfind[n] && n < sizeof(pat); n++)
            pat[n] = (unsigned char)up(vfind[n]);
    if (!n)
        return;
    progress("Suchen", vfind);
    for (off = from; off + n <= vsize; off++) {
        for (k = 0; k < n; k++) {
            v = vb(off + k);
            if (v < 0 || (unsigned char)(vhex ? v : up((char)v)) != pat[k])
                break;
        }
        if (k == n) {
            vmpos = off;
            vmlen = n;
            vtop = vhex ? off & ~15UL : vline_start(off);
            if (!vhex && vwrap)
                while (vnext(vtop) <= off)
                    vtop = vnext(vtop);
            return;
        }
        if ((off & 4095) == 0 && KbdPeek() && (KbdCharIn() & 0xFF) == 27)
            return;                     /* Esc bricht ab                 */
    }
    ask("Suchen", "Nicht gefunden:", vfind, 1);
}

static void vgoto(void)
{
    static char g[16];
    unsigned long v = 0, line = 0, off = 0;
    unsigned i;
    int h;
    g[0] = 0;
    if (!input("Gehe zu", vhex ? "Offset (hex), z. B. 1F00:" : "Zeile, oder 0x... f\x81r einen Offset:",
               g, sizeof(g)) || !g[0])
        return;
    if (vhex || (g[0] == '0' && up(g[1]) == 'X')) {
        for (i = (g[0] == '0' && up(g[1]) == 'X') ? 2 : 0; (h = hexval(g[i])) >= 0; i++)
            v = (v << 4) | (unsigned)h;
        if (v > vsize)
            v = vsize;
        vtop = vhex ? v & ~15UL : vline_start(v);
        return;
    }
    for (i = 0; g[i] >= '0' && g[i] <= '9'; i++)
        v = (v << 3) + (v << 1) + (unsigned)(g[i] - '0');
    if (v)
        v--;
    for (off = 0; line < v && off < vsize; off++)  /* Zeilen zaehlen     */
        if (vb(off) == '\n')
            line++;
    vtop = off;
}

static void viewer(const char *path, unsigned long size)
{
    unsigned k, rows;
    unsigned char c, sc;
    unsigned long n;
    int i;

    vh = DosOpen(path);
    if (FAPI_FAILED(vh)) {
        ask("Ansehen", "Datei kann nicht ge\x94" "ffnet werden:", path, 1);
        draw_all();
        return;
    }
    vsel = DosAllocSeg(0);
    if (FAPI_FAILED(vsel) || !vsel) {
        DosClose(vh);
        ask("Ansehen", "Nicht gen\x81gend Arbeitsspeicher.", "", 1);
        draw_all();
        return;
    }
    scopy(vname, path, sizeof(vname));
    vsize = size;
    vbase = vlen = 0;
    vtop = 0;
    vleft = 0;
    vmpos = 0xFFFFFFFFUL;
    vfind[0] = 0;
    VioSetCurPos(0xFF, 0);
    for (;;) {
        rows = R - 2;
        vdraw();
        vkeys();
        k = getkey();
        if (k == K_MOUSE) {
            if ((mev.buttons & MOU_LEFT) && !(mprev & MOU_LEFT) && mev.row == R - 1) {
                mprev = mev.buttons;
                k = (unsigned)(K_F1 + mev.col / 8) << 8;    /* Tastenleiste */
            } else {
                mprev = mev.buttons;
                continue;
            }
        }
        c = (unsigned char)k;
        sc = (unsigned char)(k >> 8);
        if (c == 27 || (c == 0 && (sc == K_F1 + 2 || sc == K_F10)) || c == 'q' || c == 'Q')
            break;
        if (c == 0 || c == 0xE0) {
            switch (sc) {
            case K_DOWN:
                if (vnext(vtop) < vsize)
                    vtop = vnext(vtop);
                break;
            case K_UP:
                vtop = vprev(vtop);
                break;
            case K_PGDN:
                for (i = 0; i < (int)rows - 1; i++) {
                    n = vnext(vtop);
                    if (n >= vsize)
                        break;
                    vtop = n;
                }
                break;
            case K_PGUP:
                for (i = 0; i < (int)rows - 1; i++)
                    vtop = vprev(vtop);
                break;
            case K_HOME:
                vtop = 0;
                vleft = 0;
                break;
            case K_END:
                vtop = vlast_page();
                break;
            case K_RIGHT:
                if (!vwrap && !vhex)
                    vleft += 8;
                break;
            case K_LEFT:
                vleft = vleft >= 8 ? vleft - 8 : 0;
                break;
            case K_F1:
                box(3, 13, "Betrachter");
                put(5, 13, "Pfeile, Bild auf/ab, Pos1/Ende   bl\x84ttern", A_DLG, 54);
                put(6, 13, "Pfeil links/rechts               waagerecht (ohne Umbruch)", A_DLG, 54);
                put(7, 13, "F2  Zeilenumbruch an/aus", A_DLG, 54);
                put(8, 13, "F4  Hex-Ansicht / Text", A_DLG, 54);
                put(9, 13, "F5  Gehe zu Zeile oder Offset (0x...)", A_DLG, 54);
                put(10, 13, "F7  Suchen (Hex-Ansicht: Bytes wie 4D 5A), n weiter", A_DLG, 54);
                put(11, 13, "F3, F10, Esc, q  zur\x81" "ck", A_DLG, 54);
                getkey();
                break;
            case K_F1 + 1:                  /* F2: Umbruch                */
                vwrap = !vwrap;
                vleft = 0;
                vtop = vhex ? vtop : vline_start(vtop);
                break;
            case K_F1 + 3:                  /* F4: Hex                    */
                vhex = !vhex;
                vtop = vhex ? vtop & ~15UL : vline_start(vtop);
                break;
            case K_F1 + 4:
                vgoto();
                break;
            case K_F1 + 6:                  /* F7: Suchen                 */
                if (input("Suchen", vhex ? "Bytes (hex), z. B. 4D 5A:" : "Text:", vfind, sizeof(vfind)) &&
                    vfind[0])
                    vsearch(vmpos != 0xFFFFFFFFUL ? vmpos + 1 : vtop);
                break;
            }
            continue;
        }
        if ((c == 'n' || c == 'N') && vfind[0])
            vsearch(vmpos != 0xFFFFFFFFUL ? vmpos + 1 : vtop);
        else if (c == ' ')
            for (i = 0; i < (int)rows - 1 && vnext(vtop) < vsize; i++)
                vtop = vnext(vtop);
    }
    DosFreeSeg(vsel);
    DosClose(vh);
    draw_all();
}

static void view_edit(int edit)
{
    static char c[FAPI_PATHLEN + 8];
    struct panel *p = &pn[act];
    if (!p->n || (p->e[p->cur].attr & 0x10))
        return;
    cur_path(tmp);
    if (!edit) {                        /* eigener Betrachter            */
        viewer(tmp, p->e[p->cur].size);
        return;
    }
    scopy(c, "EDIT ", sizeof(c));
    quote(c + 5, tmp, sizeof(c) - 5);
    run(c, 0);
}

static void help(void)
{
    int r = 2, i;
    static const char *const h[] = {
        "Tab           anderes Fenster",
        "Eingabe       Verzeichnis \x94" "ffnen, Programm starten,",
        "              sonst die Befehlszeile ausf\x81hren",
        "Einfg         markieren      Grau + - *  alle/keine/umkehren",
        "F3 / F4       Ansehen (Betrachter, F1 dort) / Bearbeiten (EDIT)",
        "F5 / F6       Kopieren / Verschieben oder Umbenennen",
        "F7 / F8       Verzeichnis anlegen / L\x94schen (auch Entf)",
        "Alt+F1/F2     Laufwerk links / rechts",
        "Strg+O        Bildschirm der Sitzung zeigen",
        "Strg+R        neu einlesen   Strg+Eingabe  Name in die Befehlszeile",
        "F10           Ende (Esc leert die Befehlszeile)",
        "Maus: Klick w\x84hlt, zweiter Klick \x94" "ffnet, rechts markiert,",
        "      Klick auf die Tastenleiste l\x94st die Taste aus.",
        0 };
    box(r, 17, "Hilfe");
    for (i = 0; h[i]; i++)
        put(r + 2 + i, 13, h[i], A_DLG, 54);
    VioSetCurPos(0xFF, 0);
    getkey();
    if (mprev != mev.buttons)
        mprev = mev.buttons;
    draw_all();
}

/* Befehlszeile ausfuehren; CD wechselt das Fenster */
static void exec_cmdline(void)
{
    char *a = skip(cmdl);
    if (!*a)
        return;
    if ((same(a, "CD") || same(a, "CHDIR")) && *skip(next_word(a))) {
        static char t[FAPI_PATHLEN];
        a = skip(next_word(a));
        if (a[0] == '"')
            word(a, t, sizeof(t));
        else
            scopy(t, a, sizeof(t));
        enter_dir_of_panel();
        if (DosChDir(t) == 0 && DosQCurDir(tmp) == 0) {
            scopy(pn[act].dir, tmp, sizeof(pn[act].dir));
            pn[act].cur = pn[act].top = 0;
            read_panel(&pn[act], 0);
        }
    } else
        run(a, 1);
    cmdn = 0;
    cmdl[0] = 0;
    draw_all();
}

static void mouse_event(void)
{
    static unsigned last_r, last_c;
    int press = (mev.buttons & MOU_LEFT) && !(mprev & MOU_LEFT);
    int rpress = (mev.buttons & MOU_RIGHT) && !(mprev & MOU_RIGHT);
    unsigned r = mev.row, c = mev.col, rows = PROWS(), k, idx;
    struct panel *p;

    mprev = mev.buttons;
    if (!press && !rpress)
        return;
    if (r == R - 1 && press) {          /* Tastenleiste               */
        k = c / 8;
        switch (k) {
        case 0: help(); break;
        case 2: view_edit(0); break;
        case 3: view_edit(1); break;
        case 4: do_copy_move(0); break;
        case 5: do_copy_move(1); break;
        case 6: do_mkdir(); break;
        case 7: do_delete(); break;
        case 9: quit = 1; break;
        }
        return;
    }
    if (r >= 2 && r < 2 + rows) {
        k = c >= 40;
        p = &pn[k];
        idx = p->top + r - 2;
        if (idx >= p->n)
            return;
        if (rpress) {
            act = (int)k;
            set_mark(p, idx, !p->e[idx].mark);
            p->cur = idx;
        } else if ((int)k == act && idx == p->cur && last_r == r && last_c / 40 == c / 40) {
            open_cur();                 /* zweiter Klick: oeffnen      */
            last_r = 0xFFFF;
            return;
        } else {
            act = (int)k;
            p->cur = idx;
        }
        last_r = r;
        last_c = c;
        draw_all();
    }
}

static void key(unsigned k)
{
    struct panel *p = &pn[act];
    unsigned char c = (unsigned char)k, sc = (unsigned char)(k >> 8);
    unsigned rows = PROWS(), i;

    if (k == K_MOUSE) {
        mouse_event();
        return;
    }
    if ((c == 0 || c == 0xE0) && (sc == K_F1 || sc == K_F1 + 1) &&
        (KbdShift() & KBDSHIFT_ALT)) {  /* Alt+F1/F2 (Treiber: Scancode) */
        do_drive(sc == K_F1 + 1);
        return;
    }
    if (c == 0 || c == 0xE0) {
        switch (sc) {
        case K_UP:    if (p->cur) p->cur--; break;
        case K_DOWN:  if (p->cur + 1 < p->n) p->cur++; break;
        case K_PGUP:  p->cur = p->cur > rows ? p->cur - rows : 0; break;
        case K_PGDN:  p->cur = p->cur + rows < p->n ? p->cur + rows : (p->n ? p->n - 1 : 0); break;
        case K_HOME:  p->cur = 0; break;
        case K_END:   p->cur = p->n ? p->n - 1 : 0; break;
        case K_INS:
            if (p->n) {
                set_mark(p, p->cur, !p->e[p->cur].mark);
                if (p->cur + 1 < p->n)
                    p->cur++;
            }
            break;
        case K_DEL:   do_delete(); return;
        case K_F1:    help(); return;
        case K_F1 + 2: view_edit(0); return;
        case K_F1 + 3: view_edit(1); return;
        case K_F1 + 4: do_copy_move(0); return;
        case K_F1 + 5: do_copy_move(1); return;
        case K_F1 + 6: do_mkdir(); return;
        case K_F1 + 7: do_delete(); return;
        case K_F10:   quit = 1; return;
        case K_ALTF1: do_drive(0); return;
        case K_ALTF2: do_drive(1); return;
        }
        draw_all();
        return;
    }
    if (c == 9) {                       /* Tab                         */
        act = !act;
        draw_all();
        return;
    }
    if (sc == K_GPLUS || sc == K_GMINUS || sc == K_GSTAR) {
        if (!cmdn) {                    /* Grautasten: Gruppen         */
            for (i = 0; i < p->n; i++)
                if (!(p->e[i].attr & 0x10))
                    set_mark(p, i, sc == K_GPLUS ? 1 : sc == K_GMINUS ? 0 : !p->e[i].mark);
            draw_all();
            return;
        }
    }
    if (c == 13) {
        if (cmdn)
            exec_cmdline();
        else
            open_cur();
        return;
    }
    if (c == 10 && p->n) {              /* Strg+Eingabe: Name einfuegen */
        quote(tmp, NAME(p, p->cur), sizeof(tmp));
        scat(tmp, " ", sizeof(tmp));
        if (cmdn + slen(tmp) < sizeof(cmdl) - 1) {
            scat(cmdl, tmp, sizeof(cmdl));
            cmdn = slen(cmdl);
        }
        draw_cmd();
        return;
    }
    if (c == 27) {
        if (cmdn) {
            cmdn = 0;
            cmdl[0] = 0;
            draw_cmd();
        } else
            quit = 1;
        return;
    }
    if (c == 15) {                      /* Strg+O: Sitzung zeigen      */
        VioSetMode(0);
        getkey();
        screen_back();
        return;
    }
    if (c == 18) {                      /* Strg+R: neu einlesen        */
        read_panel(&pn[0], 0);
        read_panel(&pn[1], 0);
        draw_all();
        return;
    }
    if (c == 8) {
        if (cmdn)
            cmdl[--cmdn] = 0;
        draw_cmd();
        return;
    }
    if (c >= ' ' && cmdn < sizeof(cmdl) - 1) {
        cmdl[cmdn++] = (char)c;
        cmdl[cmdn] = 0;
        draw_cmd();
    }
}

void main(void)
{
    static char args[FAPI_ARGLEN];
    unsigned k;
    char *a;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("AT-DOS Dateimanager: zwei Verzeichnisfenster, Funktionstasten.\r\n\r\n"
             "FM [Verzeichnis1 [Verzeichnis2]]\r\n\r\n"
             "Im Programm zeigt F1 die Tasten.\r\n");
        return;
    }
    k = VioSetMode(1);
    if (FAPI_FAILED(k) || k < 25) {
        puts("Vollbild nicht verf\x81gbar.\r\n");
        DosExit(1);
    }
    R = k;
    lvb = VioGetBuf();
    if (FAPI_FAILED(lvb) || !lvb) {
        VioSetMode(0);
        puts("Bildschirmpuffer nicht verf\x81gbar.\r\n");
        DosExit(1);
    }
    if (MouOpen()) {
        mouse = 1;
        MouShowPtr(1);
    }
    DosSetSigHandler(on_signal, SIG_BREAK | SIG_SHUTDOWN);
    DosQCurDir(pn[0].dir);
    scopy(pn[1].dir, pn[0].dir, sizeof(pn[1].dir));
    if (*a) {                           /* FM links [rechts]            */
        word(a, tmp, sizeof(tmp));
        if (abs_path(tmp, tmp2) == 0 && is_dir(tmp2))
            scopy(pn[0].dir, tmp2, sizeof(pn[0].dir));
        a = skip(next_word(a));
        if (*a) {
            word(a, tmp, sizeof(tmp));
            if (abs_path(tmp, tmp2) == 0 && is_dir(tmp2))
                scopy(pn[1].dir, tmp2, sizeof(pn[1].dir));
        }
    }
    read_panel(&pn[0], 0);
    read_panel(&pn[1], 0);
    draw_all();
    while (!quit)
        key(getkey());
    VioSetCurPos(0xFF, 0);
    VioSetMode(0);
}
