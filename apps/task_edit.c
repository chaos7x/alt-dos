/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * EDIT.EXE - Bildschirm-Editor im Stil von MS-DOS EDIT:
 *
 *   EDIT [Datei]
 *
 * Vollbild (VioSetMode): Zeile 0 Menueleiste, 1 Rahmen mit Dateiname,
 * 2-22 Text, 23 Rahmen, 24 Statuszeile (bei 43/50 Zeilen entsprechend
 * mehr Text, MODE CON LINES=). Menue mit Alt (allein oder
 * Alt+Buchstabe) oder F10, Tasten wie MS-DOS EDIT (CUA): Umschalt+Pfeile
 * markieren, Umschalt+Entf ausschneiden, Strg+Einfg kopieren,
 * Umschalt+Einfg einfuegen, Strg+Y Zeile loeschen, F3 weitersuchen.
 *
 * Text im Speicher als Lueckenpuffer (gap buffer) ueber bis zu NSEG
 * Segmenten zu 64 KB (DosAllocSeg, erst bei Bedarf): Zeichen vor der
 * Schreibmarke am Anfang, danach am Ende, dazwischen die Luecke. Die
 * Schreibmarke ist immer der Lueckenanfang. Positionen sind 32 Bit
 * (tpos): Segment = obere 16 Bit, Offset = untere. Ist die Luecke voll,
 * kommt ein Segment dazu und alles hinter der Luecke rueckt um 64 KB
 * (= gleicher Offset, naechstes Segment). Zeilenende im Speicher nur
 * LF; gespeichert wird CR LF. Tabulatoren werden beim Laden in
 * Leerzeichen umgesetzt (wie MS-DOS EDIT). Grenze: 512 KB Text.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

typedef unsigned long tpos;         /* Stelle im Text                  */
#define NSEG        8               /* hoechstens 8 x 64 KB = 512 KB    */
#define TNONE       0xFFFFFFFFUL
static unsigned TROWS = 21;         /* Textzeilen ab Zeile 2           */
static unsigned SROW = 24;          /* Statuszeile (unterste)          */
#define TCOLS       78              /* Textspalten 1..78               */
#define TY          2
#define TX          1

#define A_TEXT      0x17            /* weiss auf blau                  */
#define A_SEL       0x71            /* Markierung                      */
#define A_BAR       0x70            /* Menue, Dialoge                  */
#define A_HOT       0x7F            /* Kennbuchstabe                   */
#define A_BARSEL    0x07            /* gewaehlter Menuepunkt           */
#define A_HOTSEL    0x0F
#define A_STATUS    0x30            /* schwarz auf tuerkis             */
#define A_FIELD     0x07            /* Eingabefeld                     */
#define A_TITLE     0x71

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
#define K_F3        0x3D
#define K_F10       0x44
#define NONE        0xFFFFU

static unsigned segs[NSEG], nseg;   /* Textsegmente                     */
static tpos     gs, ge;             /* Luecke [gs, ge)                  */
static unsigned nlines, cur_line;   /* Zeilen gesamt, Zeile der Marke   */
static tpos     top;                /* Anfang der obersten Zeile        */
static unsigned top_line, left, want;
static tpos     anchor;             /* Markierung: anchor .. gs         */
static unsigned selon;
static unsigned csel, clen;         /* Zwischenablage                   */
static int      modified, overwrite, quit, need_all = 1;
static unsigned last_thumb = NONE;
static char     fname[FAPI_PATHLEN];
static char     find_s[64], repl_s[64];
static char     row[82], seg[82];
static unsigned char att[82];
static char     args[FAPI_ARGLEN];
static volatile int shut_req;       /* SIG_SHUTDOWN angekommen          */
static int      shutting;           /* Nachfrage laeuft schon           */
static int      mouse;              /* Maus da (MouOpen)                */
static struct fapi_mouevent mev;    /* letztes Mausereignis             */
static unsigned char mprev;         /* Tasten davor                     */
#define K_MOUSE     0xFE00          /* getkey_m: Mausereignis in mev    */

static void shutdown_now(void);

/* Bildschirm ueber den logischen Puffer (VioGetBuf/VioShowBuf wie bei
   OS/2 1.x): EDIT schreibt direkt hinein und merkt sich die geaenderten
   Zeilen; gezeigt wird, wenn EDIT auf eine Eingabe wartet - immer ein
   fertiges Bild, ein FAPI-Aufruf statt einem je Zeile. Ohne Puffer geht
   alles wie frueher ueber VioWrtStrAtt. */
static unsigned lvb, lvb_rows = 25;     /* Selektor (0 = keiner), Zeilen */
static unsigned char dlo = 0xFF, dhi;   /* geaenderte Zeilen             */

static void lvb_str(int r, int c, const char *s, unsigned char a)
{
    unsigned short __far *p;
    if (!lvb) {
        fapi_call(FAPI_VIOWRTSTRATT, (unsigned short)s, (r << 8) | c, a);
        return;
    }
    if (r < 0 || r >= (int)lvb_rows || c < 0 || c >= 80)
        return;
    p = MK_FP(lvb, (r * 80 + c) * 2);
    while (*s && c++ < 80)
        *p++ = (unsigned char)*s++ | ((unsigned)a << 8);
    if (dlo == 0xFF)
        dlo = dhi = (unsigned char)r;
    else if (r < dlo)
        dlo = (unsigned char)r;
    else if (r > dhi)
        dhi = (unsigned char)r;
}

static void lvb_show(void)
{
    if (dlo != 0xFF) {
        VioShowBuf(dlo, dhi);
        dlo = 0xFF;
        dhi = 0;
    }
}

#undef VioWrtStrAtt
#define VioWrtStrAtt(r, c, s, a) lvb_str((r), (c), (s), (a))

/* Taste holen; kommt beim Warten SIG_SHUTDOWN, fragt EDIT nach dem
   Speichern und endet (KbdCharIn kehrt bei einem Signal mit 0 zurueck).
   m = 1: auch Mausereignisse (-> K_MOUSE); sonst werden sie verworfen. */
static unsigned getkey_m(int m)
{
    unsigned k;
    lvb_show();                         /* fertiges Bild zeigen          */
    for (;;) {
        if (shut_req && !shutting)
            shutdown_now();
        if (!mouse || KbdPeek()) {
            k = KbdCharIn();
            if (k || !shut_req)
                return k;
            continue;
        }
        if (MouReadEvent(&mev, 0) == 0) {
            /* blosses Bewegen ohne Taste: nichts zu tun (den Zeiger
               zeichnet der Kernel) - sonst zeichnete EDIT bei jeder
               Bewegung neu, auf dem 286 flackerte der Oeffnen-Dialog */
            if (m && (mev.buttons || mprev))
                return K_MOUSE;
            mprev = mev.buttons;
            continue;
        }
        DosSleep(20);
    }
}

static unsigned getkey(void)
{
    return getkey_m(0);
}

/* Mausereignis: linke Taste gerade gedrueckt / losgelassen / gehalten */
#define M_PRESS()   ((mev.buttons & MOU_LEFT) && !(mprev & MOU_LEFT))
#define M_RELEASE() (!(mev.buttons & MOU_LEFT) && (mprev & MOU_LEFT))

#define TBP(p)  ((char __far *)MK_FP(segs[(unsigned)((p) >> 16)], (unsigned)(p)))
#define CAP     ((tpos)nseg << 16)

/* ---- Textpuffer -------------------------------------------------------- */
static tpos tlen(void)
{
    return CAP - (ge - gs);
}

static char ch(tpos p)
{
    return p < gs ? *TBP(p) : *TBP(p + (ge - gs));
}

/* Luecke (= Schreibmarke) nach p verschieben, Zeilennummer mitfuehren.
   In Laeufen, die in beiden Segmenten bleiben (Fernzeiger je Lauf). */
static void move_to(tpos p)
{
    char __far *s, __far *d;
    unsigned long k;
    unsigned run, i;
    char c;

    while (gs > p) {                    /* rueckwaerts: [.., gs) -> [.., ge) */
        k = gs - p;
        run = (unsigned)(gs - 1) + 1u;  /* bis Segmentanfang (0 = 64 KB) */
        if (run && k > run)
            k = run;
        run = (unsigned)(ge - 1) + 1u;
        if (run && k > run)
            k = run;
        run = k > 0xFFFFUL ? 0xFFFFU : (unsigned)k;
        s = TBP(gs - run);
        d = TBP(ge - run);
        for (i = run; i-- > 0; ) {
            c = s[i];
            d[i] = c;
            if (c == '\n')
                cur_line--;
        }
        gs -= run;
        ge -= run;
    }
    while (gs < p) {                    /* vorwaerts: [ge, ..) -> [gs, ..) */
        k = p - gs;
        run = 0u - (unsigned)gs;        /* bis Segmentende (0 = 64 KB)   */
        if (run && k > run)
            k = run;
        run = 0u - (unsigned)ge;
        if (run && k > run)
            k = run;
        run = k > 0xFFFFUL ? 0xFFFFU : (unsigned)k;
        s = TBP(ge);
        d = TBP(gs);
        for (i = 0; i < run; i++) {
            c = s[i];
            d[i] = c;
            if (c == '\n')
                cur_line++;
        }
        gs += run;
        ge += run;
    }
}

/* Luecke um 64 KB vergroessern: neues Segment hinten, alles ab ge eine
   Segmentnummer weiter (gleicher Offset). -1, wenn kein Platz. */
static int grow(void)
{
    unsigned sel, si, o, lo;
    char __far *s, __far *d;

    if (nseg >= NSEG)
        return -1;
    sel = DosAllocSeg(0);
    if (FAPI_FAILED(sel))
        return -1;
    segs[nseg++] = sel;
    for (si = nseg - 1; si-- > (unsigned)(ge >> 16); ) {
        s = MK_FP(segs[si], 0);
        d = MK_FP(segs[si + 1], 0);
        lo = si == (unsigned)(ge >> 16) ? (unsigned)ge : 0;
        for (o = 0xFFFFU; ; o--) {
            d[o] = s[o];
            if (o == lo)
                break;
        }
    }
    ge += 0x10000UL;
    return 0;
}

static tpos line_start(tpos p)
{
    while (p > 0 && ch(p - 1) != '\n')
        p--;
    return p;
}

static tpos line_end(tpos p)
{
    tpos n = tlen();
    while (p < n && ch(p) != '\n')
        p++;
    return p;
}

static unsigned cur_col(void)
{
    return (unsigned)(gs - line_start(gs));
}

static void text_clear(void)
{
    while (nseg > 1)                    /* nur ein Segment behalten      */
        DosFreeSeg(segs[--nseg]);
    gs = 0;
    ge = CAP;
    nlines = 1;
    cur_line = top = top_line = left = want = 0;
    selon = 0;
    modified = 0;
}

static int ins_char(char c)
{
    if (gs == ge && grow())
        return -1;                  /* Speicher voll                    */
    *TBP(gs) = c;
    gs++;
    if (c == '\n') {
        nlines++;
        cur_line++;
    }
    modified = 1;
    return 0;
}

static void del_range(tpos a, tpos b)   /* [a, b) loeschen     */
{
    move_to(a);
    while (b-- > a) {
        if (*TBP(ge) == '\n')
            nlines--;
        ge++;
    }
    modified = 1;
}

/* Markierung: Bereich, 0 = keine */
static int sel_range(tpos *a, tpos *b)
{
    if (!selon || anchor == gs)
        return 0;
    *a = anchor < gs ? anchor : gs;
    *b = anchor < gs ? gs : anchor;
    return 1;
}

static int del_sel(void)
{
    tpos a, b;
    if (!sel_range(&a, &b)) {
        selon = 0;
        return 0;
    }
    del_range(a, b);
    selon = 0;
    return 1;
}

/* ---- Bildschirm -------------------------------------------------------- */
static void out(int r, int c, const char *s, unsigned n, unsigned char a)
{
    unsigned i;
    for (i = 0; i < n; i++)
        seg[i] = s[i] ? s[i] : ' ';
    seg[n] = 0;
    VioWrtStrAtt(r, c, seg, a);
}

/* row[0..n) mit att[] in Laeufen gleicher Farbe ausgeben */
static void out_row(int r, int c, unsigned n)
{
    unsigned i = 0, j;
    while (i < n) {
        j = i;
        while (j < n && att[j] == att[i])
            j++;
        out(r, c + i, row + i, j - i, att[i]);
        i = j;
    }
}

static void fill(int r, int c, unsigned n, char ch_, unsigned char a)
{
    unsigned i;
    for (i = 0; i < n; i++)
        row[i] = ch_;
    out(r, c, row, n, a);
}

/* Text mit '&' vor dem Kennbuchstaben, auf Breite w aufgefuellt */
static void out_hot(int r, int c, const char *s, unsigned w,
                    unsigned char a, unsigned char ah)
{
    unsigned n = 0;
    int hot = 0;
    while (*s && n < w) {
        if (*s == '&') {
            hot = 1;
            s++;
            continue;
        }
        row[n] = *s++;
        att[n++] = hot ? ah : a;
        hot = 0;
    }
    while (n < w) {
        row[n] = ' ';
        att[n++] = a;
    }
    out_row(r, c, w);
}

static unsigned hot_len(const char *s)
{
    unsigned n = 0;
    for (; *s; s++)
        if (*s != '&')
            n++;
    return n;
}

static char hot_char(const char *s)
{
    for (; *s; s++)
        if (*s == '&')
            return up(s[1]);
    return 0;
}

static void num(char *d, unsigned v, int w)     /* mit Nullen        */
{
    while (w--) {
        d[w] = (char)('0' + v % 10);
        v /= 10;
    }
}

/* ---- Menues ------------------------------------------------------------ */
enum { C_NONE, C_NEW, C_OPEN, C_SAVE, C_SAVEAS, C_EXIT, C_CUT, C_COPY,
       C_PASTE, C_CLEAR, C_FIND, C_NEXT, C_CHANGE, C_KEYS, C_ABOUT };

struct item { const char *text, *key; int cmd; };

static const struct item m_file[] = {
    { "&Neu", "", C_NEW }, { "&\x99" "ffnen...", "", C_OPEN },
    { "&Speichern", "Strg+S", C_SAVE }, { "Speichern &unter...", "", C_SAVEAS },
    { 0, 0, 0 }, { "&Beenden", "", C_EXIT }, { 0, 0, -1 } };
static const struct item m_edit[] = {
    { "&Ausschneiden", "Umschalt+Entf", C_CUT },
    { "&Kopieren", "Strg+Einfg", C_COPY },
    { "&Einf\x81gen", "Umschalt+Einfg", C_PASTE },
    { "&L\x94schen", "Entf", C_CLEAR }, { 0, 0, -1 } };
static const struct item m_search[] = {
    { "&Suchen...", "", C_FIND }, { "&Weitersuchen", "F3", C_NEXT },
    { "\x8E&ndern...", "", C_CHANGE }, { 0, 0, -1 } };
static const struct item m_help[] = {
    { "&Tastenbelegung", "F1", C_KEYS }, { "&Info...", "", C_ABOUT },
    { 0, 0, -1 } };

struct menu { const char *title; int col; unsigned char altsc; const struct item *it; };
static const struct menu menus[4] = {
    { "&Datei", 1, 0x20, m_file }, { "&Bearbeiten", 9, 0x30, m_edit },
    { "&Suchen", 22, 0x1F, m_search }, { "&Hilfe", 72, 0x23, m_help } };

static void draw_bar(int active)
{
    int m;
    fill(0, 0, 80, ' ', A_BAR);
    for (m = 0; m < 4; m++)
        out_hot(0, menus[m].col, menus[m].title, hot_len(menus[m].title) + 2,
                m == active ? A_BARSEL : A_BAR, m == active ? A_HOTSEL : A_HOT);
}

static int nitems(const struct item *it)
{
    int n = 0;
    while (it[n].cmd >= 0)
        n++;
    return n;
}

static void draw_menu(int m, int sel, int *px, int *pw)
{
    const struct item *it = menus[m].it;
    int n = nitems(it), i, w = 0, kw = 0, x, k, r;

    for (i = 0; i < n; i++)
        if (it[i].text) {
            if ((k = (int)hot_len(it[i].text)) > w)
                w = k;
            if ((k = (int)slen(it[i].key)) > kw)
                kw = k;
        }
    w += kw ? kw + 3 : 0;
    x = menus[m].col - 1;
    if (x < 0)
        x = 0;
    if (x + w + 4 > 80)
        x = 80 - (w + 4);
    *px = x;
    *pw = w;
    /* Rahmen */
    row[0] = '\xDA';
    for (i = 1; i <= w + 2; i++)
        row[i] = '\xC4';
    row[w + 3] = '\xBF';
    out(1, x, row, w + 4, A_BAR);
    for (i = 0; i < n; i++) {
        r = 2 + i;
        if (!it[i].text) {
            row[0] = '\xC3';
            for (k = 1; k <= w + 2; k++)
                row[k] = '\xC4';
            row[w + 3] = '\xB4';
            out(r, x, row, w + 4, A_BAR);
            continue;
        }
        out(r, x, "\xB3", 1, A_BAR);
        out(r, x + w + 3, "\xB3", 1, A_BAR);
        out(r, x + 1, " ", 1, i == sel ? A_BARSEL : A_BAR);
        out_hot(r, x + 2, it[i].text, w + 1, i == sel ? A_BARSEL : A_BAR,
                i == sel ? A_HOTSEL : A_HOT);
        if (it[i].key[0]) {
            k = (int)slen(it[i].key);
            out(r, x + 2 + w - k, it[i].key, (unsigned)k, i == sel ? A_BARSEL : A_BAR);
        }
    }
    row[0] = '\xC0';
    for (i = 1; i <= w + 2; i++)
        row[i] = '\xC4';
    row[w + 3] = '\xD9';
    out(2 + n, x, row, w + 4, A_BAR);
}

static void redraw_all(void);

/* Menue unter Spalte c der Menueleiste, -1 = keins */
static int bar_hit(unsigned c)
{
    int m;
    for (m = 0; m < 4; m++)
        if (c >= (unsigned)menus[m].col &&
            c < (unsigned)menus[m].col + hot_len(menus[m].title) + 2)
            return m;
    return -1;
}

/* Menue bedienen; open: gleich aufgeklappt. -> Befehl oder C_NONE */
static int menu_run(int m, int open)
{
    int sel = 0, n, x = 0, w = 0, i, shown = -2;
    unsigned k, c, sc;

    VioSetCurPos(0xFF, 0);
    need_all = 1;                       /* danach Text neu zeichnen      */
    for (;;) {
        n = nitems(menus[m].it);
        if (shown != (open ? m : -1)) { /* anderes Menue: Reste weg      */
            if (shown >= 0)
                redraw_all();
            shown = open ? m : -1;
        }
        draw_bar(m);
        if (open) {
            while (!menus[m].it[sel].text)
                sel++;
            draw_menu(m, sel, &x, &w);
        }
        k = getkey_m(1);
        if (k == K_MOUSE) {
            int press = M_PRESS(), rel = M_RELEASE(), mm;
            mprev = mev.buttons;
            if (mev.row == 0) {             /* Menueleiste              */
                mm = bar_hit(mev.col);
                if (mm >= 0 && (mev.buttons & MOU_LEFT) && (mm != m || !open)) {
                    m = mm;
                    sel = 0;
                    open = 1;
                } else if (mm < 0 && press)
                    return C_NONE;
                continue;
            }
            if (open && mev.row >= 2 && mev.row < 2 + n &&
                mev.col >= x && mev.col <= x + w + 3) {
                i = mev.row - 2;            /* Eintrag unter dem Zeiger */
                if (menus[m].it[i].text) {
                    sel = i;
                    if (rel) {
                        redraw_all();
                        return menus[m].it[i].cmd;
                    }
                }
                continue;
            }
            if (press)                      /* daneben geklickt: zu     */
                return C_NONE;
            continue;
        }
        c = k & 0xFF;
        sc = k >> 8;
        if (k == KEY_ALT || c == 27 || (c == 0 && sc == K_F10))
            return C_NONE;
        if (c == 0 || c == 0xE0) {
            switch (sc) {
            case K_LEFT:  m = (m + 3) % 4; sel = 0; continue;
            case K_RIGHT: m = (m + 1) % 4; sel = 0; continue;
            case K_DOWN:
                if (!open) { open = 1; sel = 0; continue; }
                do sel = (sel + 1) % n; while (!menus[m].it[sel].text);
                continue;
            case K_UP:
                if (!open) { open = 1; sel = n - 1; continue; }
                do sel = (sel + n - 1) % n; while (!menus[m].it[sel].text);
                continue;
            }
            for (i = 0; i < 4; i++)     /* Alt+Buchstabe: anderes Menue  */
                if (sc == menus[i].altsc) {
                    m = i;
                    sel = 0;
                    open = 1;
                }
            continue;
        }
        if (c == 13) {
            if (open) {
                redraw_all();           /* Menue weg vor dem Befehl      */
                return menus[m].it[sel].cmd;
            }
            open = 1;
            sel = 0;
            continue;
        }
        c = (unsigned char)up((char)c);
        if (open) {
            for (i = 0; i < n; i++)
                if (menus[m].it[i].text && (unsigned char)hot_char(menus[m].it[i].text) == c) {
                    redraw_all();
                    return menus[m].it[i].cmd;
                }
        } else {
            for (i = 0; i < 4; i++)
                if ((unsigned char)hot_char(menus[i].title) == c) {
                    m = i;
                    sel = 0;
                    open = 1;
                }
        }
    }
}

/* passt der Name in 8.3 (ohne Leerzeichen, ein Punkt)? */
static int is83(const char *s)
{
    unsigned n = 0, e = 0, dot = 0;
    for (; *s; s++)
        if (*s == '.') {
            if (dot++)
                return 0;
        } else if (*s == ' ')
            return 0;
        else if (dot)
            e++;
        else
            n++;
    return n >= 1 && n <= 8 && e <= 3;
}

/* ---- Textfenster ------------------------------------------------------- */
static void draw_frame(void)
{
    const char *b = fname[0] ? base_name(fname) : "UNBENANNT";
    unsigned n = slen(b), x;

    row[0] = '\xDA';
    for (x = 1; x < 79; x++)
        row[x] = '\xC4';
    row[79] = '\xBF';
    out(1, 0, row, 80, A_TEXT);
    x = (80 - (n + 2)) / 2;
    seg[0] = ' ';
    scopy(seg + 1, b, 70);
    if (is83(seg + 1))                      /* 8.3 gross wie MS-DOS,   */
        for (n = 1; seg[n]; n++)            /* lange Namen wie sie sind */
            seg[n] = up(seg[n]);
    for (n = 1; seg[n]; n++)
        ;
    seg[n++] = ' ';
    seg[n] = 0;
    VioWrtStrAtt(1, x, seg, A_TITLE);
    row[0] = '\xC0';
    row[79] = '\xD9';
    out((int)TROWS + 2, 0, row, 80, A_TEXT);
}

static void draw_status(void)
{
    static char s[81];
    unsigned i;
    for (i = 0; i < 80; i++)
        s[i] = ' ';
    scopy(s + 1, "AT-DOS Editor  <F1=Hilfe>  Alt: Men\x81", 60);
    s[slen(s)] = ' ';
    if (overwrite) {
        s[60] = '\x9A';
        s[61] = 'B';
    }
    num(s + 69, cur_line + 1, 5);
    s[74] = ':';
    num(s + 75, cur_col() + 1, 3);
    out((int)SROW, 0, s, 80, A_STATUS);
}

static void draw_text(void)
{
    tpos p = top, n = tlen(), q, a = 0, b = 0;
    unsigned r, c, sb, thumb, step;
    int sel = sel_range(&a, &b), more = 1;

    for (r = 0; r < TROWS; r++) {
        for (c = 0; c < TCOLS; c++) {
            row[c] = ' ';
            att[c] = A_TEXT;
        }
        if (more) {
            q = p;
            c = 0;
            while (q < n && ch(q) != '\n') {
                if (c >= left && c < left + TCOLS) {
                    row[c - left] = ch(q);
                    if (sel && q >= a && q < b)
                        att[c - left] = A_SEL;
                }
                c++;
                q++;
            }
            if (q < n)
                p = q + 1;
            else
                more = 0;
        }
        out_row(TY + r, TX, TCOLS);
        out(TY + r, 0, "\xB3", 1, A_TEXT);
    }
    /* Bildlaufleiste rechts, nur wenn sich etwas aendert */
    step = nlines / (TROWS - 2) + 1;
    thumb = cur_line / step;
    if (thumb > TROWS - 3)
        thumb = TROWS - 3;
    if (thumb == last_thumb)
        return;
    last_thumb = thumb;
    for (sb = 0; sb < TROWS; sb++) {
        row[0] = sb == 0 ? '\x1E' : sb == TROWS - 1 ? '\x1F' :
                 sb - 1 == thumb ? '\xDB' : '\xB0';
        out(TY + sb, 79, row, 1, A_BAR);
    }
}

/* Anzeige an die Schreibmarke anpassen */
static void follow(void)
{
    unsigned c = cur_col(), i;
    if (cur_line < top_line || cur_line >= top_line + TROWS) {
        top = line_start(gs);
        top_line = cur_line;
        if (cur_line >= TROWS)          /* Marke unten, nicht oben       */
            for (i = 0; i < TROWS - 1 && top > 0; i++) {
                top = line_start(top - 1);
                top_line--;
            }
    }
    if (c < left)
        left = c;
    else if (c >= left + TCOLS)
        left = c - TCOLS + 1;
}

static void place_cursor(void)
{
    VioSetCurPos(TY + cur_line - top_line, TX + cur_col() - left);
}

static void redraw_all(void)
{
    need_all = 0;
    last_thumb = NONE;
    draw_bar(-1);
    draw_frame();
    draw_text();
    draw_status();
}

/* ---- Dialoge ----------------------------------------------------------- */
static void box(int r, int c, int h, int w, const char *title)
{
    int i;
    unsigned n;
    row[0] = '\xDA';
    for (i = 1; i < w - 1; i++)
        row[i] = '\xC4';
    row[w - 1] = '\xBF';
    out(r, c, row, w, A_BAR);
    for (i = 1; i < h - 1; i++) {
        fill(r + i, c, w, ' ', A_BAR);
        out(r + i, c, "\xB3", 1, A_BAR);
        out(r + i, c + w - 1, "\xB3", 1, A_BAR);
    }
    row[0] = '\xC0';
    for (i = 1; i < w - 1; i++)
        row[i] = '\xC4';
    row[w - 1] = '\xD9';
    out(r + h - 1, c, row, w, A_BAR);
    if (title) {
        n = slen(title);
        seg[0] = ' ';
        scopy(seg + 1, title, 60);
        seg[n + 1] = ' ';
        seg[n + 2] = 0;
        VioWrtStrAtt(r, c + (w - (int)n - 2) / 2, seg, A_BAR);
    }
}

static void center(int r, const char *s, unsigned char a)
{
    VioWrtStrAtt(r, (80 - (int)slen(s)) / 2, s, a);
}

/* Knoepfe "< Ja >  < Nein >" zentriert in Zeile r, der mit Nummer foc
   hervorgehoben (-1 = keiner); -> Anzahl Knoepfe */
static int draw_buttons(int r, const char *b, int foc)
{
    int c0 = (80 - (int)slen(b)) / 2, i, start = 0, n = 0;
    VioWrtStrAtt(r, c0, b, A_BAR);
    for (i = 0; b[i]; i++) {
        if (b[i] == '<')
            start = i;
        else if (b[i] == '>') {
            if (n == foc)
                out(r, c0 + start, b + start, (unsigned)(i - start + 1), A_BARSEL);
            n++;
        }
    }
    return n;
}

/* Knopf unter dem Mausklick (mev) in Zeile r, -1 = keiner */
static int button_hit(int r, const char *b)
{
    int c0 = (80 - (int)slen(b)) / 2, i, start = 0, n = 0;
    if (mev.row != r)
        return -1;
    for (i = 0; b[i]; i++) {
        if (b[i] == '<')
            start = i;
        else if (b[i] == '>') {
            if ((int)mev.col >= c0 + start && (int)mev.col <= c0 + i)
                return n;
            n++;
        }
    }
    return -1;
}

/* Fokus weiterschalten: Tab/Rechts vor, Umschalt+Tab/Links zurueck;
   -> neuer Fokus, -1 = keine Wechseltaste */
static int next_focus(unsigned c, unsigned sc, int foc, int n)
{
    if (c == 9)
        return (KbdShift() & KBDSHIFT_SHIFT) ? (foc + n - 1) % n : (foc + 1) % n;
    if (c == 0 || c == 0xE0) {
        if (sc == 0x0F || sc == K_LEFT)         /* Umschalt+Tab, links  */
            return (foc + n - 1) % n;
        if (sc == K_RIGHT)
            return (foc + 1) % n;
    }
    return -1;
}

/* Meldung mit Knoepfen; keys: ein Zeichen je Knopf ("JN\x1B"), Kennbuchstabe
   waehlt direkt, Tab/Pfeile wechseln, Eingabe drueckt den gewaehlten,
   Esc = 27 */
static int msgbox(const char *t1, const char *t2, const char *buttons, const char *keys)
{
    unsigned c, i, k;
    int foc = 0, nb, f;
    need_all = 1;
    box(8, 12, 8, 56, 0);
    center(10, t1, A_BAR);
    if (t2)
        center(11, t2, A_BAR);
    VioSetCurPos(0xFF, 0);
    for (;;) {
        nb = draw_buttons(13, buttons, foc);
        while ((k = getkey_m(1)) == K_MOUSE && !M_PRESS())
            mprev = mev.buttons;            /* nur Druecken zaehlt       */
        if (k == K_MOUSE) {                 /* Klick auf einen Knopf     */
            f = M_PRESS() ? button_hit(13, buttons) : -1;
            mprev = mev.buttons;
            if (f >= 0)
                return (unsigned char)keys[f];
            continue;
        }
        c = k & 0xFF;
        if (c == 27)
            return 27;
        f = next_focus(c, k >> 8, foc, nb);
        if (f >= 0) {
            foc = f;
            continue;
        }
        if (c == 13 || c == ' ')
            return (unsigned char)keys[foc];
        c = (unsigned char)up((char)c);
        if (c == 'Y')
            c = 'J';
        for (i = 0; keys[i]; i++)
            if ((unsigned char)keys[i] == c && c != 27)
                return (int)c;
    }
}

static void message(const char *t1, const char *t2)
{
    msgbox(t1, t2, "< OK >", "O");
}

/* Eingabefelder und Knoepfe OK/Abbrechen; Tab wechselt reihum (Felder,
   OK, Abbrechen), Eingabe im Feld = OK. Maus: Klick ins Feld setzt den
   Cursor, Klick auf einen Knopf drueckt ihn. -> 1 OK, 0 Abbrechen */
#define FX 27                       /* Eingabefeld: Spalte, Breite   */
#define FW 40
static int input(const char *title, int nf, const char **prompt, char **buf, unsigned max)
{
    static const char btn[] = "< OK >   < Abbrechen >";
    int f = 0, i, fresh = 1, g;
    unsigned pos, n, view, c, sc, k;

    need_all = 1;
    box(7, 10, 6 + 2 * nf, 60, title);
    for (i = 0; i < nf; i++)
        VioWrtStrAtt(9 + 2 * i, 13, prompt[i], A_BAR);
    pos = slen(buf[0]);
    for (;;) {
        for (i = 0; i < nf; i++) {      /* Felder zeichnen               */
            n = slen(buf[i]);
            view = (i == f && pos > FW - 1) ? pos - (FW - 1) : 0;
            for (k = 0; k < FW; k++) {
                row[k] = view + k < n ? buf[i][view + k] : ' ';
                att[k] = A_FIELD;
            }
            out_row(9 + 2 * i, FX, FW);
            if (i == f)
                VioSetCurPos(9 + 2 * i, FX + pos - view);
        }
        draw_buttons(10 + 2 * nf, btn, f >= nf ? f - nf : -1);
        if (f >= nf)
            VioSetCurPos(0xFF, 0);
        while ((k = getkey_m(1)) == K_MOUSE && !M_PRESS())
            mprev = mev.buttons;            /* nur Druecken zaehlt       */
        if (k == K_MOUSE) {
            mprev = mev.buttons;
            g = button_hit(10 + 2 * nf, btn);
            if (g >= 0)
                return g == 0;              /* OK / Abbrechen            */
            for (i = 0; i < nf; i++)
                if (mev.row == 9 + 2 * i && mev.col >= FX && mev.col < FX + FW) {
                    view = (i == f && pos > FW - 1) ? pos - (FW - 1) : 0;
                    f = i;
                    n = slen(buf[f]);
                    pos = view + mev.col - FX;
                    if (pos > n)
                        pos = n;
                    fresh = 0;
                }
            continue;
        }
        c = k & 0xFF;
        sc = k >> 8;
        if (c == 27)
            return 0;
        if (c == 13)
            return f != nf + 1;         /* auf "Abbrechen": abbrechen    */
        g = -1;
        if (c == 9 || (c == 0 && sc == 0x0F))   /* Tab, Umschalt+Tab     */
            g = next_focus(c, sc, f, nf + 2);
        else if (f >= nf && (c == 0 || c == 0xE0) &&
                 (sc == K_LEFT || sc == K_RIGHT))   /* zwischen Knoepfen */
            g = nf + (f - nf + 1) % 2;
        if (g >= 0) {
            f = g;
            if (f < nf) {
                pos = slen(buf[f]);
                fresh = 1;
            }
            continue;
        }
        if (f >= nf) {                  /* auf einem Knopf               */
            if (c == ' ')
                return f == nf;
            continue;
        }
        n = slen(buf[f]);
        if (c == 0 || c == 0xE0) {
            fresh = 0;
            if (sc == K_LEFT && pos > 0) pos--;
            else if (sc == K_RIGHT && pos < n) pos++;
            else if (sc == K_HOME) pos = 0;
            else if (sc == K_END) pos = n;
            else if (sc == K_DEL && pos < n)
                for (i = (int)pos; buf[f][i]; i++)
                    buf[f][i] = buf[f][i + 1];
            continue;
        }
        if (fresh) {                    /* erste Taste ersetzt Vorgabe   */
            fresh = 0;
            if (c != 8) {
                buf[f][0] = 0;
                pos = n = 0;
            }
        }
        if (c == 8) {
            if (pos > 0) {
                pos--;
                for (i = (int)pos; buf[f][i]; i++)
                    buf[f][i] = buf[f][i + 1];
            }
        } else if (c >= 32 && n + 1 < max) {
            for (i = (int)n + 1; i > (int)pos; i--)
                buf[f][i] = buf[f][i - 1];
            buf[f][pos++] = (char)c;
        }
    }
}

/* ---- Oeffnen-Dialog mit Dateiliste (wie MS-DOS EDIT) ------------------ */
#define NENT        300             /* Eintraege in der Liste           */
#define LROWS       10              /* sichtbare Zeilen, je 4 Spalten   */
#define LCOLS       4
#define POOL        12288           /* Namen (lange Namen), je mit Null */
static char          pool[POOL];
static unsigned      ent_off[NENT], pool_n;
#define ent(k)       (pool + ent_off[k])
static unsigned char ent_dir[NENT];
static unsigned      nent;
static char          odir[FAPI_PATHLEN], opat[64] = "*.*";

static void ent_add(const char *name, unsigned char d)
{
    unsigned n = slen(name) + 1;
    if (nent >= NENT || pool_n + n > POOL)
        return;
    ent_off[nent] = pool_n;
    scopy(pool + pool_n, name, n);
    pool_n += n;
    ent_dir[nent++] = d;
}

static int ent_less(unsigned a, unsigned b)     /* Verzeichnisse zuerst */
{
    const char *x = ent(a), *y = ent(b);
    if (ent_dir[a] != ent_dir[b])
        return ent_dir[a] > ent_dir[b];
    if (x[0] == '.' && x[1] == '.' && !x[2])
        return 1;
    if (y[0] == '.' && y[1] == '.' && !y[2])
        return 0;
    while (*x && up(*x) == up(*y))      /* ohne Gross/klein          */
        x++, y++;
    return (unsigned char)up(*x) < (unsigned char)up(*y);
}

static void read_dir(void)
{
    static struct fapi_dirent e;
    unsigned i, j, n;
    unsigned char d;

    nent = 0;
    pool_n = 0;
    if (slen(odir) > 3)                         /* nicht im Stamm: ..   */
        ent_add("..", 1);
    for (i = 0; nent < NENT && DosFindEntry(&e, i, odir) != FAPI_ERROR; i++) {
        if ((e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2]))) ||
            (e.attr & 0x0E))                    /* ., .., versteckt,
                                                   System, Bezeichnung */
            continue;
        d = (e.attr & 0x10) != 0;
        if (!d && !match(e.name, opat))
            continue;
        ent_add(e.name, d);
    }
    for (i = 1; i < nent; i++)                  /* einsortieren         */
        for (j = i; j > 0 && ent_less(j, j - 1); j--) {
            n = ent_off[j];
            ent_off[j] = ent_off[j - 1];
            ent_off[j - 1] = n;
            n = ent_dir[j];
            ent_dir[j] = ent_dir[j - 1];
            ent_dir[j - 1] = (unsigned char)n;
        }
}

/* in Unterverzeichnis name bzw. ".." wechseln */
static void enter_dir(const char *name)
{
    unsigned n;
    if (name[0] == '.' && name[1] == '.') {
        n = slen(odir);
        while (n > 3 && odir[n - 1] != '\\')
            n--;
        odir[n > 3 ? n - 1 : 3] = 0;
    } else
        join(odir, odir, name, sizeof(odir));
}

/* Feld bearbeiten (Tasten wie in input); fresh: erste Taste ersetzt */
static void field_key(char *b, unsigned max, unsigned *pos, int *fresh,
                      unsigned c, unsigned sc)
{
    unsigned n = slen(b), i;
    if (c == 0 || c == 0xE0) {
        *fresh = 0;
        if (sc == K_LEFT && *pos > 0) (*pos)--;
        else if (sc == K_RIGHT && *pos < n) (*pos)++;
        else if (sc == K_HOME) *pos = 0;
        else if (sc == K_END) *pos = n;
        else if (sc == K_DEL && *pos < n)
            for (i = *pos; b[i]; i++)
                b[i] = b[i + 1];
        return;
    }
    if (*fresh) {
        *fresh = 0;
        if (c != 8) {
            b[0] = 0;
            *pos = n = 0;
        }
    }
    if (c == 8) {
        if (*pos > 0) {
            (*pos)--;
            for (i = *pos; b[i]; i++)
                b[i] = b[i + 1];
        }
    } else if (c >= 32 && n + 1 < max) {
        for (i = n + 1; i > *pos; i--)
            b[i] = b[i - 1];
        b[(*pos)++] = (char)c;
    }
}

/* Dialog "Oeffnen": Feld, Verzeichnis, Liste, Knoepfe. Tab wechselt
   Feld -> Liste -> OK -> Abbrechen. Im Feld: Name oder Muster (*.TXT),
   Verzeichnis oder Laufwerk (D:) mit Eingabe. In der Liste: Pfeile,
   Bild auf/ab, Pos1/Ende; Eingabe oeffnet Datei bzw. Verzeichnis.
   -> 1 und voller Pfad in out, 0 = abgebrochen */
static int open_dialog(char *out)
{
    static const char btn[] = "< OK >   < Abbrechen >";
    static char fld[FAPI_PATHLEN], tmp[FAPI_PATHLEN];
    unsigned pos, k, c, sc, i, r, col, ltop = 0, sel = 0, view;
    int f = 0, fresh = 1, g, fresh_list = 1;

    need_all = 1;
    if (!odir[0]) {
        DosQCurDir(odir);
        if (slen(odir) == 2)
            scat(odir, "\\", sizeof(odir));
    }
    scopy(fld, opat, sizeof(fld));
    pos = slen(fld);
    for (;;) {
        if (fresh_list) {
            read_dir();
            sel = ltop = 0;
            fresh_list = 0;
        }
        box(3, 8, 19, 64, "\x99" "ffnen");
        VioWrtStrAtt(5, 11, "Dateiname:", A_BAR);
        i = slen(odir);                         /* lang: Ende zeigen    */
        if (i > 56) {
            scopy(tmp, "...", sizeof(tmp));
            scat(tmp, odir + i - 53, sizeof(tmp));
        } else
            scopy(tmp, odir, sizeof(tmp));
        VioWrtStrAtt(7, 11, tmp, A_BAR);
        VioWrtStrAtt(8, 11, "Dateien und Verzeichnisse:", A_BAR);
        /* Feld */
        view = pos > 43 ? pos - 43 : 0;
        for (i = 0; i < 44; i++) {
            row[i] = view + i < slen(fld) ? fld[view + i] : ' ';
            att[i] = A_FIELD;
        }
        out_row(5, 23, 44);
        /* Liste */
        if (sel / LCOLS < ltop)
            ltop = sel / LCOLS;
        if (sel / LCOLS >= ltop + LROWS)
            ltop = sel / LCOLS - LROWS + 1;
        for (r = 0; r < LROWS; r++) {
            for (i = 0; i < 56; i++) {
                row[i] = ' ';
                att[i] = A_FIELD;
            }
            for (col = 0; col < LCOLS; col++) {
                k = (ltop + r) * LCOLS + col;
                if (k >= nent)
                    break;
                {                       /* 12 Zellen, laenger: >>   */
                    const char *nm = ent(k);
                    unsigned n = slen(nm), w = n + ent_dir[k];
                    for (i = 0; i < n && (w <= 12 ? 1 : i < 11); i++)
                        row[col * 14 + 1 + i] = nm[i];
                    if (w > 12)
                        row[col * 14 + 12] = '\xAF';
                    else if (ent_dir[k])
                        row[col * 14 + 1 + i] = '\\';
                }
                if (f == 1 && k == sel)
                    for (i = 0; i < 14; i++)
                        att[col * 14 + i] = A_SEL;
            }
            out_row(9 + (int)r, 12, 56);
        }
        if (!nent)
            VioWrtStrAtt(9, 13, "(keine passenden Dateien)", A_FIELD);
        draw_buttons(20, btn, f >= 2 ? f - 2 : -1);
        if (f == 0)
            VioSetCurPos(5, 23 + pos - view);
        else
            VioSetCurPos(0xFF, 0);

        /* nur Druecken zaehlt; Loslassen/Ziehen ohne Neuzeichnen */
        while ((k = getkey_m(1)) == K_MOUSE && !M_PRESS())
            mprev = mev.buttons;
        if (k == K_MOUSE) {
            int press = M_PRESS();
            mprev = mev.buttons;
            if (!press)
                continue;
            r = mev.row;
            col = mev.col;
            if (r >= 9 && r < 9 + LROWS && col >= 12 && col < 12 + LCOLS * 14) {
                k = (ltop + r - 9) * LCOLS + (col - 12) / 14;
                if (k >= nent)
                    continue;
                if (f == 1 && k == sel) {   /* zweiter Klick: oeffnen    */
                    if (ent_dir[sel]) {
                        enter_dir(ent(sel));
                        fresh_list = 1;
                        scopy(fld, opat, sizeof(fld));
                        continue;
                    }
                    join(out, odir, ent(sel), FAPI_PATHLEN);
                    return 1;
                }
                f = 1;
                sel = k;
                if (!ent_dir[sel])
                    scopy(fld, ent(sel), sizeof(fld));
                continue;
            }
            if (r == 5 && col >= 23 && col < 23 + 44) {  /* Namensfeld */
                f = 0;
                fresh = 0;
                pos = view + col - 23;
                if (pos > slen(fld))
                    pos = slen(fld);
                continue;
            }
            g = button_hit(20, btn);
            if (g == 1)
                return 0;                   /* Abbrechen                 */
            if (g != 0)
                continue;
            c = 13;                         /* OK: wie Eingabe im Feld   */
            sc = 0;
            f = 2;
        } else {
            c = k & 0xFF;
            sc = k >> 8;
        }
        if (c == 27)
            return 0;
        if (c == 9 || (c == 0 && sc == 0x0F)) {     /* Tab, Umschalt+Tab */
            g = next_focus(c, sc, f, 4);
            if (g >= 0)
                f = g;
            if (f == 0) {
                pos = slen(fld);
                fresh = 1;
            }
            continue;
        }
        if (f == 1 && nent) {                       /* Liste             */
            if (c == 0 || c == 0xE0) {
                if (sc == K_LEFT && sel > 0) sel--;
                else if (sc == K_RIGHT && sel + 1 < nent) sel++;
                else if (sc == K_UP && sel >= LCOLS) sel -= LCOLS;
                else if (sc == K_DOWN && sel + LCOLS < nent) sel += LCOLS;
                else if (sc == K_PGUP) sel = sel >= LCOLS * LROWS ? sel - LCOLS * LROWS : 0;
                else if (sc == K_PGDN) sel = sel + LCOLS * LROWS < nent ? sel + LCOLS * LROWS : nent - 1;
                else if (sc == K_HOME) sel = 0;
                else if (sc == K_END) sel = nent - 1;
                if (!ent_dir[sel])
                    scopy(fld, ent(sel), sizeof(fld));
                continue;
            }
            if (c != 13)
                continue;
            if (ent_dir[sel]) {
                enter_dir(ent(sel));
                fresh_list = 1;
                scopy(fld, opat, sizeof(fld));
                continue;
            }
            join(out, odir, ent(sel), FAPI_PATHLEN);
            return 1;
        }
        if (f >= 2) {                               /* Knoepfe           */
            if ((c == 0 || c == 0xE0) && (sc == K_LEFT || sc == K_RIGHT))
                f = f == 2 ? 3 : 2;
            if (c != 13 && c != ' ')
                continue;
            if (f == 3)
                return 0;
        } else if (c != 13) {                       /* Feld              */
            field_key(fld, sizeof(fld), &pos, &fresh, c, sc);
            continue;
        }
        /* Eingabe im Feld oder OK: Muster, Verzeichnis oder Datei */
        if (!fld[0])
            continue;
        if (has_wild(fld)) {
            char *b = base_name(fld);
            scopy(opat, b, sizeof(opat));
            *b = 0;
            if (fld[0]) {                           /* Pfad davor        */
                if (fld[0] && fld[1] == ':' && !fld[2])
                    scat(fld, "\\", sizeof(fld));
                abs_path(fld, odir);
            }
            for (i = 0; opat[i]; i++)
                opat[i] = up(opat[i]);
            fresh_list = 1;
            scopy(fld, opat, sizeof(fld));
            pos = slen(fld);
            fresh = 1;
            continue;
        }
        if (fld[0] == '.' && fld[1] == '.' && !fld[2]) {
            enter_dir("..");
            fresh_list = 1;
        } else {
            if (fld[1] == ':' || fld[0] == '\\')
                abs_path(fld, tmp);
            else
                join(tmp, odir, fld, sizeof(tmp));
            if (slen(tmp) == 2 && tmp[1] == ':')
                scat(tmp, "\\", sizeof(tmp));
            if (is_dir(tmp)) {
                scopy(odir, tmp, sizeof(odir));
                fresh_list = 1;
            } else {
                scopy(out, tmp, FAPI_PATHLEN);
                return 1;
            }
        }
        scopy(fld, opat, sizeof(fld));
        pos = slen(fld);
        fresh = 1;
    }
}

/* ---- Dateien ----------------------------------------------------------- */
static int load(const char *name)       /* 0 ok, 1 nicht da, -1 Fehler  */
{
    static char b[512];
    unsigned h, n, i, col = 0;
    char c;

    h = DosOpen(name);
    if (FAPI_FAILED(h))
        return 1;
    text_clear();
    for (;;) {
        n = DosRead(h, b, sizeof(b));
        if (FAPI_FAILED(n) || n == 0)
            break;
        for (i = 0; i < n; i++) {
            c = b[i];
            if (c == '\r' || c == 0x1A)
                continue;
            if (c == '\t') {            /* Tabulator -> Leerzeichen     */
                do {
                    if (ins_char(' '))
                        goto full;
                } while (++col & 7);
                continue;
            }
            if (ins_char(c))
                goto full;
            col = c == '\n' ? 0 : col + 1;
        }
    }
    DosClose(h);
    move_to(0);
    modified = 0;
    return 0;
full:
    DosClose(h);
    text_clear();
    message("Die Datei ist zu gro\xE1 (h\x94" "chstens 512 KB).", 0);
    return -1;
}

static int save(const char *name)       /* 0 ok                          */
{
    static char b[1024];
    unsigned h, n = 0, w;
    tpos p, len = tlen();
    char c;

    h = DosCreate(name);
    if (FAPI_FAILED(h)) {
        message("Die Datei kann nicht angelegt werden.",
                h == FAPI_ERR_WP ? "Der Datentr\x84ger ist schreibgesch\x81tzt." : 0);
        return -1;
    }
    for (p = 0; p <= len; p++) {
        if (n >= sizeof(b) - 2 || p == len) {
            w = DosWrite(h, b, n);
            if (FAPI_FAILED(w) || w != n) {
                DosClose(h);
                message("Fehler beim Schreiben - Datentr\x84ger voll?", 0);
                return -1;
            }
            n = 0;
        }
        if (p == len)
            break;
        c = ch(p);
        if (c == '\n')
            b[n++] = '\r';
        b[n++] = c;
    }
    DosClose(h);
    modified = 0;
    return 0;
}

static int save_as(void)
{
    static char nb[FAPI_PATHLEN];
    static const char *pr[1] = { "Dateiname:" };
    char *bp[1];
    unsigned h;

    scopy(nb, fname, sizeof(nb));
    bp[0] = nb;
    if (!input("Speichern unter", 1, pr, bp, sizeof(nb)) || !nb[0])
        return -1;
    h = DosOpen(nb);
    if (!FAPI_FAILED(h)) {
        DosClose(h);
        if (msgbox("Die Datei gibt es schon. \x9A" "berschreiben?", 0,
                   "< Ja >   < Nein >", "JN") != 'J')
            return -1;
    }
    if (save(nb))
        return -1;
    scopy(fname, nb, sizeof(fname));
    return 0;
}

static int do_save(void)
{
    if (!fname[0])
        return save_as();
    return save(fname);
}

/* Aenderungen verwerfen? -> 0 weiter, -1 abbrechen */
static int ask_save(void)
{
    int k;
    if (!modified)
        return 0;
    k = msgbox("Die Datei wurde ge\x84ndert.", "Jetzt speichern?",
               "< Ja >   < Nein >   < Abbrechen >", "JN\x1B");
    if (k == 27)
        return -1;
    if (k == 'J')
        return do_save();
    return 0;
}

/* ---- Zwischenablage ---------------------------------------------------- */
static void copy_sel(void)
{
    tpos a, b, i;
    char __far *d;
    if (!sel_range(&a, &b))
        return;
    if (b - a > 0xFFF0UL) {
        message("Die Markierung ist zu gro\xE1 f\x81r die",
                "Zwischenablage (h\x94" "chstens 64 KB).");
        return;
    }
    if (csel)
        DosFreeSeg(csel);
    csel = DosAllocSeg((unsigned)(b - a));
    clen = 0;
    if (FAPI_FAILED(csel)) {
        csel = 0;
        message("Kein Speicher f\x81r die Zwischenablage.", 0);
        return;
    }
    d = MK_FP(csel, 0);
    for (i = a; i < b; i++)
        d[clen++] = ch(i);
    /* gemeinsame Zwischenablage aller Sitzungen (Konsolenmaus); zu gross:
       dort leeren, damit Einfuegen nicht den abgeschnittenen Rest nimmt */
    DosClipPut(csel, 0, clen <= CLIP_MAX ? clen : 0);
}

/* Einfuegen: aus der Zwischenablage des Kernels, wenn sie etwas enthaelt
   (auch mit der Maus in einer anderen Sitzung markierter Text), sonst aus
   der eigenen (Markierung ueber CLIP_MAX). CR faellt weg (Zeilenende LF). */
static void paste(void)
{
    char __far *s;
    unsigned i, n = DosClipGet(0, 0, 0);
    if (!FAPI_FAILED(n) && n) {
        if (csel)
            DosFreeSeg(csel);
        csel = DosAllocSeg(n);
        clen = 0;
        if (FAPI_FAILED(csel)) {
            csel = 0;
            message("Kein Speicher f\x81r die Zwischenablage.", 0);
            return;
        }
        clen = DosClipGet(csel, 0, n);
        if (clen > n)
            clen = n;
    }
    if (!csel)
        return;
    s = MK_FP(csel, 0);
    del_sel();
    for (i = 0; i < clen; i++)
        if (s[i] != '\r' && ins_char(s[i])) {
            message("Kein Platz mehr im Text (512 KB).", 0);
            break;
        }
}

/* ---- Suchen ------------------------------------------------------------ */
static tpos find_from(tpos from, tpos to)   /* -> Fund oder TNONE */
{
    unsigned n = slen(find_s), i;
    tpos p;
    if (!n || to < n)
        return TNONE;
    for (p = from; p + n <= to; p++) {
        for (i = 0; i < n && up(ch(p + i)) == up(find_s[i]); i++)
            ;
        if (i == n)
            return p;
    }
    return TNONE;
}

static int find_next(void)              /* 1 gefunden                   */
{
    unsigned n = slen(find_s);
    tpos p = find_from(gs, tlen());
    if (p == TNONE)
        p = find_from(0, gs + n - 1 < tlen() ? gs + n - 1 : tlen());
    if (p == TNONE) {
        message("Nicht gefunden:", find_s);
        return 0;
    }
    selon = 1;
    anchor = p;
    move_to(p + n);
    return 1;
}

static void word_at_cursor(char *d, unsigned max)
{
    tpos p = gs, len = tlen();
    unsigned n = 0;
    char c;
    while (p > 0 && (c = ch(p - 1)) != ' ' && c != '\n')
        p--;
    while (p < len && (c = ch(p)) != ' ' && c != '\n' && n + 1 < max)
        d[n++] = c, p++;
    d[n] = 0;
}

static void do_find(void)
{
    static const char *pr[1] = { "Suchen nach:" };
    char *bp[1];
    if (!find_s[0])
        word_at_cursor(find_s, sizeof(find_s));
    bp[0] = find_s;
    if (input("Suchen", 1, pr, bp, sizeof(find_s)) && find_s[0]) {
        redraw_all();
        find_next();
    }
}

static void replace_here(void)
{
    unsigned i;
    del_sel();
    for (i = 0; repl_s[i]; i++)
        if (ins_char(repl_s[i]))
            break;
}

static void do_change(void)
{
    static const char *pr[2] = { "Suchen nach:", "\x8Endern in:" };
    char *bp[2];
    tpos start, p, end;
    unsigned n, rn, cnt = 0, pass, all = 0;
    int k;
    static char cnts[8];

    if (!find_s[0])
        word_at_cursor(find_s, sizeof(find_s));
    bp[0] = find_s;
    bp[1] = repl_s;
    if (!input("\x8Endern", 2, pr, bp, sizeof(find_s)) || !find_s[0])
        return;
    n = slen(find_s);
    rn = slen(repl_s);
    start = gs;
    selon = 0;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1)
            move_to(0);
        for (;;) {
            end = pass == 0 ? tlen() : start;
            p = find_from(gs, end);
            if (p == TNONE)
                break;
            selon = 1;
            anchor = p;
            move_to(p + n);
            if (!all) {
                follow();
                redraw_all();
                k = msgbox("Diese Stelle \x84ndern?", 0,
                           "< Ja >  < Nein >  < Alle >  < Abbrechen >", "JNA\x1B");
                redraw_all();
                if (k == 27)
                    goto done;
                if (k == 'A')
                    all = 1;
                if (k == 'N')
                    continue;
            }
            replace_here();
            cnt++;
            if (pass == 1)              /* Ersetzungen vor dem Start     */
                start = start + rn - n;
        }
    }
done:
    selon = 0;
    follow();
    redraw_all();
    num(cnts, cnt, 5);
    cnts[5] = 0;
    for (k = 0; k < 4 && cnts[k] == '0'; k++)
        cnts[k] = ' ';
    message("\x8Enderungen vorgenommen:", cnts);
}

/* ---- Hilfe ------------------------------------------------------------- */
static void show_keys(void)
{
    static const char *t[] = {
        "Pfeile, Pos1, Ende, Bild auf/ab   bewegen",
        "Strg+Pfeil links/rechts           Wort zur\x81" "ck/vor",
        "Strg+Pos1 / Strg+Ende             Anfang/Ende des Textes",
        "Umschalt+Bewegung                 markieren",
        "Umschalt+Entf / Strg+Einfg        ausschneiden / kopieren",
        "Umschalt+Einfg                    einf\x81gen",
        "Entf                              Zeichen/Markierung l\x94schen",
        "Strg+Y                            Zeile ausschneiden",
        "Einfg                             \x9A" "berschreiben ein/aus",
        "Strg+S                            speichern",
        "F3                                weitersuchen",
        "Alt oder F10                      Men\x81",
        0 };
    int i;
    box(4, 6, 17, 68, "Tastenbelegung");
    for (i = 0; t[i]; i++)
        VioWrtStrAtt(6 + i, 9, t[i], A_BAR);
    center(19, "< OK >", A_BAR);
    VioSetCurPos(0xFF, 0);
    getkey();
}

/* ---- Befehle ----------------------------------------------------------- */
static void command(int cmd)
{
    static char nb[FAPI_PATHLEN];
    int r;

    need_all = 1;                       /* Menue/Dialog uebermalt Text   */
    switch (cmd) {
    case C_NEW:
        if (ask_save() == 0) {
            text_clear();
            fname[0] = 0;
        }
        break;
    case C_OPEN:
        if (ask_save())
            break;
        if (!open_dialog(nb))
            break;
        r = load(nb);
        if (r == 1)
            message("Datei nicht gefunden:", nb);
        else if (r == 0)
            scopy(fname, nb, sizeof(fname));
        else
            fname[0] = 0;
        break;
    case C_SAVE:
        do_save();
        break;
    case C_SAVEAS:
        save_as();
        break;
    case C_EXIT:
        if (ask_save() == 0)
            quit = 1;
        break;
    case C_CUT:
        copy_sel();
        del_sel();
        break;
    case C_COPY:
        copy_sel();
        break;
    case C_PASTE:
        paste();
        break;
    case C_CLEAR:
        del_sel();
        break;
    case C_FIND:
        do_find();
        break;
    case C_NEXT:
        if (find_s[0])
            find_next();
        else
            do_find();
        break;
    case C_CHANGE:
        do_change();
        break;
    case C_KEYS:
        show_keys();
        break;
    case C_ABOUT:
        msgbox("AT-DOS Editor 1.0", "Teil von AT-DOS 6.0", "< OK >", "O");
        break;
    }
}

/* ---- Tasten im Text ---------------------------------------------------- */
static int is_word(char c)
{
    return c != ' ' && c != '\n';
}

static void goto_col(tpos ls, unsigned col)
{
    tpos e = line_end(ls);
    move_to(ls + col < e ? ls + col : e);
}

static void nav(unsigned sc, unsigned shiftst)
{
    tpos ls, e, len = tlen();
    unsigned i;
    int ctrl = (shiftst & KBDSHIFT_CTRL) != 0, vert = 0;

    if (shiftst & KBDSHIFT_SHIFT) {
        if (!selon) {
            selon = 1;
            anchor = gs;
        }
    } else
        selon = 0;

    switch (sc) {
    case K_LEFT:
        if (ctrl) {
            while (gs > 0 && !is_word(ch(gs - 1)))
                move_to(gs - 1);
            while (gs > 0 && is_word(ch(gs - 1)))
                move_to(gs - 1);
        } else if (gs > 0)
            move_to(gs - 1);
        break;
    case K_RIGHT:
        if (ctrl) {
            while (gs < len && is_word(ch(gs)))
                move_to(gs + 1);
            while (gs < len && !is_word(ch(gs)))
                move_to(gs + 1);
        } else if (gs < len)
            move_to(gs + 1);
        break;
    case K_HOME:
        move_to(ctrl ? 0 : line_start(gs));
        break;
    case K_END:
        move_to(ctrl ? len : line_end(gs));
        break;
    case K_UP:
        vert = 1;
        ls = line_start(gs);
        if (ls > 0)
            goto_col(line_start(ls - 1), want);
        break;
    case K_DOWN:
        vert = 1;
        e = line_end(gs);
        if (e < len)
            goto_col(e + 1, want);
        break;
    case K_PGUP:
        vert = 1;
        for (i = 0; i < TROWS && top > 0; i++) {
            top = line_start(top - 1);
            top_line--;
        }
        ls = line_start(gs);
        for (i = 0; i < TROWS && ls > 0; i++)
            ls = line_start(ls - 1);
        goto_col(ls, want);
        break;
    case K_PGDN:
        vert = 1;
        ls = line_start(gs);
        for (i = 0; i < TROWS; i++) {
            e = line_end(ls);
            if (e >= len)
                break;
            ls = e + 1;
            e = line_end(top);
            if (e < len) {
                top = e + 1;
                top_line++;
            }
        }
        goto_col(ls, want);
        break;
    }
    if (!vert)
        want = cur_col();
}

/* Anfang der Zeile Nummer l (0 = erste), ab der obersten sichtbaren */
static tpos line_pos(unsigned l)
{
    tpos p = top, e, n = tlen();
    unsigned i = top_line;
    while (i > l && p > 0) {
        p = line_start(p - 1);
        i--;
    }
    while (i < l) {
        e = line_end(p);
        if (e >= n)
            break;                      /* letzte Zeile                 */
        p = e + 1;
        i++;
    }
    return p;
}

/* Mausereignis im Hauptfenster */
static void mouse_ev(void)
{
    unsigned r = mev.row, c = mev.col, l;
    int press = M_PRESS(), held = (mev.buttons & MOU_LEFT) != 0, m;

    mprev = mev.buttons;
    if (r == 0xFF)
        return;
    if (press && r == 0) {              /* Menueleiste                  */
        if ((m = bar_hit(c)) >= 0)
            command(menu_run(m, 1));
        return;
    }
    if (press && c == 79 && r >= TY && r < TY + TROWS) {
        selon = 0;                      /* Bildlaufleiste               */
        if (r == TY)
            nav(K_UP, 0);
        else if (r == TY + TROWS - 1)
            nav(K_DOWN, 0);
        else
            nav((last_thumb != NONE && r - TY - 1 < last_thumb) ? K_PGUP : K_PGDN, 0);
        return;
    }
    if (!held || c == 79)
        return;
    if (!press && !selon) {             /* Ziehen beginnt: Markierung   */
        selon = 1;
        anchor = gs;
    } else if (press)
        selon = 0;
    if (r < TY)                         /* ueber den Rand: rollen       */
        l = top_line ? top_line - 1 : 0;
    else if (r >= TY + TROWS)
        l = top_line + TROWS;
    else
        l = top_line + (r - TY);
    goto_col(line_pos(l), left + (c > TX ? c - TX : 0));
    want = cur_col();
}

static void type_char(char c)
{
    unsigned i, ind = 0;
    tpos ls;

    del_sel();
    if (c == '\n') {                    /* Einrueckung uebernehmen       */
        ls = line_start(gs);
        while (ls + ind < gs && ch(ls + ind) == ' ')
            ind++;
        if (ins_char('\n'))
            goto full;
        for (i = 0; i < ind; i++)
            if (ins_char(' '))
                goto full;
    } else if (c == '\t') {
        i = cur_col();
        do {
            if (ins_char(' '))
                goto full;
        } while (++i & 7);
    } else {
        if (overwrite && gs < tlen() && ch(gs) != '\n')
            ge++;
        if (ins_char(c))
            goto full;
    }
    want = cur_col();
    return;
full:
    message("Kein Platz mehr im Text (512 KB).", 0);
}

static void key(unsigned k)
{
    unsigned c = k & 0xFF, sc = k >> 8, st, i;
    tpos ls, e;

    if (k == 0)
        return;                         /* Signal (Strg+C)              */
    if (k == K_MOUSE) {
        mouse_ev();
        return;
    }
    if (k == KEY_ALT || (c == 0 && sc == K_F10)) {
        command(menu_run(0, 0));
        return;
    }
    if (c == 0 || c == 0xE0) {
        st = KbdShift();
        for (i = 0; i < 4; i++)
            if (c == 0 && sc == menus[i].altsc) {
                command(menu_run((int)i, 1));
                return;
            }
        switch (sc) {
        case K_F1:
            show_keys();
            need_all = 1;
            return;
        case K_F3:
            command(C_NEXT);
            return;
        case K_INS:
            if (st & KBDSHIFT_SHIFT)
                paste();
            else if (st & KBDSHIFT_CTRL)
                copy_sel();
            else
                overwrite = !overwrite;
            return;
        case K_DEL:
            if (st & KBDSHIFT_SHIFT) {
                copy_sel();
                del_sel();
            } else if (!del_sel() && gs < tlen())
                del_range(gs, gs + 1);
            want = cur_col();
            return;
        }
        nav(sc, st);
        return;
    }
    switch (c) {
    case 8:
        if (!del_sel() && gs > 0)
            del_range(gs - 1, gs);
        want = cur_col();
        return;
    case 13:
        type_char('\n');
        return;
    case 19:                            /* Strg+S                       */
        do_save();
        return;
    case 25:                            /* Strg+Y: Zeile ausschneiden   */
        ls = line_start(gs);
        e = line_end(gs);
        if (e < tlen())
            e++;
        selon = 1;
        anchor = ls;
        move_to(e);
        copy_sel();
        del_sel();
        want = 0;
        return;
    case 27:
        selon = 0;
        return;
    }
    if (c == 9 || c >= 32)
        type_char((char)c);
}

/* Herunterfahren (SHUTDOWN, Strg+Alt+Entf): ungespeicherte Aenderungen?
   Dann das System aufhalten, Sitzung nach vorn und fragen. */
static void shutdown_now(void)
{
    int k;
    shutting = 1;
    if (modified) {
        DosShutdownHold();
        redraw_all();
        for (;;) {
            k = msgbox("Das System wird heruntergefahren.",
                       "Ge\x84nderte Datei vorher speichern?",
                       "< Ja >   < Nein >", "JN");
            if (k == 'N' || (k == 'J' && do_save() == 0))
                break;
            redraw_all();               /* Esc oder Speichern misslungen */
        }
    }
    DosExit(0);
}

static void on_signal(unsigned sig)
{
    if (sig == SIG_SHUTDOWN)            /* erst beim naechsten Warten     */
        shut_req = 1;                   /* auf eine Taste fragen          */
    /* Strg+C: im Editor ohne Wirkung */
}

void main(void)
{
    char *a;
    unsigned k;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Bildschirm-Editor.\r\n\r\nEDIT [Datei]\r\n");
        return;
    }
    segs[0] = DosAllocSeg(0);
    nseg = 1;
    if (FAPI_FAILED(segs[0])) {
        puts("Nicht genug Speicher f\x81r EDIT.\r\n");
        DosExit(1);
    }
    k = VioSetMode(1);                  /* -> Zeilen: 25, 43 oder 50     */
    if (FAPI_FAILED(k) || k < 25) {
        puts("Vollbild nicht verf\x81gbar.\r\n");
        DosExit(1);
    }
    TROWS = k - 4;
    SROW = k - 1;
    lvb_rows = k;
    lvb = VioGetBuf();
    if (FAPI_FAILED(lvb))
        lvb = 0;                        /* dann ueber VioWrtStrAtt       */
    if (MouOpen()) {                    /* Maus: Zeiger zeigen          */
        mouse = 1;
        MouShowPtr(1);
    }
    DosSetSigHandler(on_signal, SIG_BREAK | SIG_SHUTDOWN);
    text_clear();
    if (*a) {
        if (*a == '"')
            word(a, fname, sizeof(fname));
        else {                          /* wie CD: Rest der Zeile, damit
                                           EDIT Brief an Hans.txt geht */
            unsigned n;
            scopy(fname, a, sizeof(fname));
            for (n = slen(fname); n && fname[n - 1] == ' '; n--)
                fname[n - 1] = 0;
        }
        if (load(fname) < 0)
            fname[0] = 0;
    }
    while (!quit) {
        follow();
        if (!KbdPeek()) {               /* Tastenpuffer leer: zeichnen   */
            if (need_all)
                redraw_all();
            else {
                draw_text();
                draw_status();
            }
            place_cursor();
        }
        key(getkey_m(1));
    }
    VioSetMode(0);
    DosExit(0);
}
