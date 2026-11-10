/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - Sitzungen und Konsole (VIO)
 *
 * Jede Sitzung hat einen eigenen Textpuffer (23 x 80) im Arbeitsspeicher,
 * erreichbar ueber einen eigenen GDT-Selektor. Programme schreiben immer
 * in den Puffer ihrer Sitzung; ist die Sitzung im Vordergrund, landet die
 * Ausgabe zusaetzlich direkt im Bildspeicher. Alt+Esc schaltet um.
 *
 * Bildschirm: Zeile 0 Titel, 1-23 Sitzung, 24 Statuszeile. Eine Sitzung
 * im Vollbild (VioSetMode, z. B. EDIT) hat alle 25 Zeilen; Titel- und
 * Statusleiste erscheinen erst wieder in einer normalen Sitzung.
 *
 * Zeilenzahl je Sitzung (MODE CON LINES=, wie bei OS/2): 25, 43 oder 50
 * Bildschirmzeilen, nur mit VGA. Beim Sitzungswechsel stellt redraw() die
 * Karte um: 400 Rasterzeilen mit 8x16 (25) bzw. 8x8 (50), 350 Rasterzeilen
 * mit 8x8 (43). Die Zeichensaetze kommen aus dem ROM der Karte (Adressen
 * hat start.asm im Real Mode erfragt) und werden in Ebene 2 geladen.
 *
 * Schattenpuffer: Ausgaben landen nur im Puffer der Sitzung; geaenderte
 * Zeilen der Vordergrund-Sitzung merken sich dlo..dhi. vio_flush() kopiert
 * sie am Stueck in den Bildspeicher - der Scheduler ruft es, sobald ein
 * Programm wartet (Taste, DosSleep, Zeitscheibe um). So sieht man nie
 * halb gezeichnete Bilder (Menue weg, Text neu, Menue wieder da).
 */
#include "kern.h"
#include "mem.h"
#include "../include/fapi_abi.h"     /* MOU_* */

#define ATTR        0x07
#define NBYTES   ((SLINES_MAX - 2) * SCOLS * 2) /* normaler Inhalt      */
#define SAVEU    ((NBYTES + MEM_UNIT - 1) / MEM_UNIT)
#define TITLE_ATTR  0x1F
#define STATUS_ATTR 0x70

#include "vioint.h"

struct session ses[NTASK];              /* mit vgagfx.c (KCODE2) geteilt */
int fg = -1;
static unsigned char dlo = 0xFF, dhi; /* geaenderte Pufferzeilen dlo..dhi
                                         der Vordergrund-Sitzung (FF = keine) */
unsigned char cur_dirty;
static unsigned shown_uptime = 0xFFFF;
unsigned char hw_lines = 25;        /* Zeilen, auf die die Karte steht  */

extern unsigned char  vga_ok;       /* start.asm, aus dem Real Mode     */
extern unsigned short vga_font8a[2], vga_font8b[2], vga_font16[2];

/* ---- Maus ---------------------------------------------------------------
   Position in Mickeys wie beim Maustreiber von Microsoft: 8 je Spalte,
   16 je Zeile (Textzelle = 8x8 Punkte des virtuellen 640x200-Schirms).
   Die Interruptroutine des Treibers (DevHlp MouEvent) rechnet nur und
   stellt Ereignisse in die Warteschlange der Vordergrund-Sitzung;
   gezeichnet wird in vio_flush/redraw. Der Zeiger ist eine Zelle mit
   umgedrehtem Attribut (XOR 77h, Blinken und Helligkeit bleiben), nur im
   Bildspeicher - der Sitzungspuffer bleibt unberuehrt. */
#define MQ_LEN  8
struct mouev { unsigned char b, row, col; unsigned short x, y; };
static struct mouev mq[NTASK][MQ_LEN];
unsigned char mou_nbtn;                 /* 0 = keine Maus                */
int mx, my;                             /* Mickeys                       */
static unsigned char mbtn;              /* Tasten jetzt                  */
static unsigned char p_drawn;           /* Zeiger steht im Bildspeicher  */
static unsigned p_idx;                  /* ... an dieser Zelle           */
static unsigned short p_saved, p_inv;   /* Zelle vorher / mit Zeiger     */

static void ptr_remove(void);
static void ptr_draw(void);


/* Konsolenmaus wie gpm unter Linux: Hat in der Vordergrund-Sitzung kein
   Programm MouOpen gerufen, gehoert die Maus dem Kernel. Linke Taste
   ziehen markiert Text (umgedrehte Zellen im Bildspeicher), Loslassen
   legt ihn in die Zwischenablage; rechte oder mittlere Taste fuegt ihn
   ein, als wuerde getippt (kbd_get holt die Zeichen ueber vio_paste).
   Die Zwischenablage gilt fuer alle Sitzungen, Programme erreichen sie
   ueber ClipPut/ClipGet. */
#define CLIP_MAX 16384
static unsigned clip_sel, clip_len;     /* Kernelsegment, Laenge         */
static unsigned char con_ptr;           /* Zeiger nach erster Bewegung   */
static unsigned char sel_on;            /* Markierung laeuft (Taste unten) */
static signed char sel_s = -1;          /* ... in dieser Sitzung         */
static unsigned sel_a, sel_b;           /* Zellen row*80+col (Sitzung)   */
static volatile unsigned char sel_copy, paste_req;  /* fuer vio_flush   */
static signed char paste_s = -1;        /* Einfuegen laeuft in Sitzung   */
static unsigned paste_pos;
static void sel_paint(unsigned a, unsigned b);
static void sel_finish(void);

static void mark(unsigned char a, unsigned char b)  /* Zeilen a..b neu */
{
    unsigned f = irq_off();
    if (dlo == 0xFF) {
        dlo = a;
        dhi = b;
    } else {
        if (a < dlo)
            dlo = a;
        if (b > dhi)
            dhi = b;
    }
    irq_restore(f);
}

/* ---- VGA: Zeilenzahl umstellen ----------------------------------------- */
static void crtc(unsigned i, unsigned v) { outb(0x3D4, i); outb(0x3D5, v); }
static unsigned crtc_in(unsigned i) { outb(0x3D4, i); return inb(0x3D5); }
static void seqr(unsigned i, unsigned v) { outb(0x3C4, i); outb(0x3C5, v); }
static void gcr(unsigned i, unsigned v) { outb(0x3CE, i); outb(0x3CF, v); }

static unsigned long seg_lin(const unsigned short *p)
{
    return ((unsigned long)p[1] << 4) + p[0];
}

/* Zeichensatz mit h Rasterzeilen je Zeichen nach Ebene 2 (32 Byte je
   Zeichen); 8x8 liegt im ROM in zwei Haelften */
static void load_font(unsigned h)
{
    unsigned char __far *d;
    const unsigned char __far *f;
    unsigned c, l;
    unsigned short w[8];

    win_save(w);                            /* laeuft ohne Kernelsperre */
    seqr(0x02, 0x04);                       /* nur Ebene 2 schreiben    */
    seqr(0x04, 0x07);                       /* fortlaufend, kein Odd/Even */
    gcr(0x04, 0x02);                        /* Ebene 2 lesen            */
    gcr(0x05, 0x00);
    gcr(0x06, 0x04);                        /* A0000h, 64 KB            */
    scratch2_map(0xA0000UL, 256 * 32 - 1);
    d = MK_FP(SCRATCH2, 0);
    for (c = 0; c < 256; c++) {
        if (h == 16) {
            if (c == 0)
                scratch_map(seg_lin(vga_font16), 256 * 16 - 1);
            f = MK_FP(SCRATCH, c * 16);
        } else {
            if (c == 0)
                scratch_map(seg_lin(vga_font8a), 128 * 8 - 1);
            else if (c == 128)
                scratch_map(seg_lin(vga_font8b), 128 * 8 - 1);
            f = MK_FP(SCRATCH, (c & 127) * 8);
        }
        for (l = 0; l < 32; l++)
            d[c * 32 + l] = l < h ? f[l] : 0;
    }
    seqr(0x02, 0x03);                       /* zurueck: Text            */
    seqr(0x04, 0x03);
    gcr(0x04, 0x00);
    gcr(0x05, 0x10);
    gcr(0x06, 0x0E);                        /* B8000h, 32 KB, Odd/Even  */
    win_restore(w);
}

/* Karte auf n Zeilen (25/43/50) stellen; Zeitwerte der VGA-Textmodi mit
   400 bzw. 350 Rasterzeilen (Gesamt 449, 70 Hz) */
static void set_hw_lines(unsigned n)
{
    static const unsigned char r400[] = { 0xBF, 0x1F, 0x9C, 0x8E, 0x8F, 0x96, 0xB9 };
    static const unsigned char r350[] = { 0xBF, 0x1F, 0x83, 0x85, 0x5D, 0x63, 0xBA };
    static const unsigned char idx[]  = { 0x06, 0x07, 0x10, 0x11, 0x12, 0x15, 0x16 };
    const unsigned char *r = n == 43 ? r350 : r400;
    unsigned h = n == 25 ? 16 : 8, i, misc;

    if (n == hw_lines || !vga_ok)
        return;
    misc = inb(0x3CC) & 0x3F;               /* Takt, Adressen behalten  */
    seqr(0x00, 0x01);                       /* Sequenzer anhalten       */
    outb(0x3C2, misc | (n == 43 ? 0x80 : 0x40));    /* Sync-Polaritaet   */
    seqr(0x00, 0x03);
    crtc(0x11, crtc_in(0x11) & 0x7F);       /* Register 0-7 freigeben   */
    for (i = 0; i < sizeof(idx); i++)
        crtc(idx[i], r[i]);                 /* 11h sperrt wieder        */
    crtc(0x09, (crtc_in(0x09) & 0xE0) | (h - 1));   /* Zeichenhoehe     */
    crtc(0x0A, (crtc_in(0x0A) & 0xE0) | (h - 2));   /* Cursor           */
    crtc(0x0B, (crtc_in(0x0B) & 0xE0) | (h - 1));
    load_font(h);
    hw_lines = (unsigned char)n;
}

#define BUF(s)   ((unsigned short __far *)MK_FP(SCR_SEL(s), 0))
#define VRAM     ((unsigned short __far *)MK_FP(VIDEO, 0))
#define CELL(a, c) (((unsigned short)(a) << 8) | (unsigned char)(c))

static void vputs(int row, int col, const char *s, unsigned char attr)
{
    while (*s && col < SCOLS)
        VRAM[row * SCOLS + col++] = CELL(attr, *s++);
}

static void vfill(int row, unsigned char attr)
{
    int c;
    for (c = 0; c < SCOLS; c++)
        VRAM[row * SCOLS + c] = CELL(attr, ' ');
}

static void cursor(void)
{
    unsigned pos;
    if (fg < 0 || ses[fg].gfx)
        return;
    pos = (ses[fg].row + ses[fg].top) * SCOLS + ses[fg].col;
    if (ses[fg].hidecur)
        pos = SLINES_MAX * SCOLS;           /* hinter dem sichtbaren Bild */
    outb(0x3D4, 0x0E); outb(0x3D5, pos >> 8);
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
}

static void title_bar(void)
{
    static char n[] = "Sitzung ?";
    vfill(0, TITLE_ATTR);
    vputs(0, 1, "AT-DOS 5.0", TITLE_ATTR);
    if (fg >= 0) {
        n[8] = (char)('1' + fg);
        vputs(0, 12, "\xB3", TITLE_ATTR);
        vputs(0, 14, n, TITLE_ATTR);
        vputs(0, 24, ses[fg].title, TITLE_ATTR);
    }
}

void vio_status(void)
{
    char b[6];
    unsigned v = uptime_s;
    int i;

    int sr = hw_lines - 1;                  /* Statuszeile: unterste    */

    if (v == shown_uptime || (fg >= 0 && (!ses[fg].top || ses[fg].gfx)))
        return;                             /* Vollbild: keine Statuszeile */
    if (shown_uptime == 0xFFFF) {
        vfill(sr, STATUS_ATTR);
        vputs(sr, 1, "Alt+Esc: Sitzung wechseln \xB3 Strg+C: Abbruch \xB3", STATUS_ATTR);
        vputs(sr, 62, "Uptime:       s", STATUS_ATTR);
    }
    shown_uptime = v;
    for (i = 4; i >= 0; i--) {
        b[i] = (char)('0' + v % 10);
        v /= 10;
    }
    b[5] = 0;
    vputs(sr, 70, b, STATUS_ATTR);
}

/* 5.1: solange die Kompatibilitaetsbox vorn ist und nur kurz fuer einen
   Dienst (Redirector) zu AT-DOS kommt, gehoert der Bildschirm ihr */
unsigned char vio_frozen;

void redraw(void)
{
    int r;
    if (vio_frozen)
        return;
    p_drawn = 0;                            /* Bildspeicher wird neu    */
    dlo = 0xFF;
    cur_dirty = 0;
    if (gfx_in())                           /* Grafiksitzung vorn       */
        return;
    if (fg >= 0)
        set_hw_lines(ses[fg].lines);
    for (r = hw_lines; r < SLINES_MAX; r++)     /* 43: Rest der 44. Zeile */
        vfill(r, ATTR);
    if (fg >= 0 && !ses[fg].top) {          /* Vollbild                 */
        far_copy(VIDEO, 0, SCR_SEL(fg), 0, ses[fg].rows * SCOLS * 2);
        sel_paint(0, ses[fg].rows - 1);
    } else {
        title_bar();
        if (fg >= 0) {
            far_copy(VIDEO, SCOLS * 2, SCR_SEL(fg), 0, ses[fg].rows * SCOLS * 2);
            sel_paint(0, ses[fg].rows - 1);
        }
        shown_uptime = 0xFFFF;              /* Statuszeile neu zeichnen  */
        vio_status();
    }
    cursor();
    ptr_draw();
}

static void put(int s, int r, int c, unsigned short v)
{
    BUF(s)[r * SCOLS + c] = v;
    if (s == fg)
        mark((unsigned char)r, (unsigned char)r);   /* erst schreiben, dann merken */
}

static void clear_row(int s, int r)
{
    int c;
    for (c = 0; c < SCOLS; c++)
        BUF(s)[r * SCOLS + c] = CELL(ses[s].attr, ' ');
}

static void scroll(int s)
{
    unsigned n = ses[s].rows;
    far_copy(SCR_SEL(s), 0, SCR_SEL(s), SCOLS * 2, (n - 1) * SCOLS * 2);
    clear_row(s, n - 1);
    if (s == fg)
        mark(0, (unsigned char)(n - 1));
}

void vio_cls(int s)
{
    int r;
    for (r = 0; r < SLINES_MAX; r++)
        clear_row(s, r);
    ses[s].row = ses[s].col = 0;
    if (s == fg) {
        mark(0, (unsigned char)(ses[s].rows - 1));
        cur_dirty = 1;
    }
}

int vio_open(const char *title)
{
    int s;
    unsigned u;

    for (s = 0; s < NTASK; s++)
        if (!ses[s].used)
            break;
    if (s == NTASK)
        return -1;
    u = mem_alloc((SBYTES + MEM_UNIT - 1) / MEM_UNIT);
    if (u == 0)
        return -1;
    ses[s].used = 1;
    ses[s].unit = u;
    ses[s].rows = SROWS;
    ses[s].lines = 25;
    ses[s].top = 1;
    ses[s].hidecur = 0;
    ses[s].owner = -1;
    ses[s].save = 0;
    ses[s].mou_owner = -1;
    ses[s].mou_show = 0;
    ses[s].mq_n = 0;
    ses[s].gfx = 0;
    ses[s].gfx_sel = 0;
    ses[s].gfx_save = 0;
    ses[s].attr = ATTR;
    ses[s].ansi = 1;                /* wie OS/2: ANSI ist an          */
    ses[s].ast = 0;
    ses[s].arow = ses[s].acol = 0;
    set_desc(SCR_IDX(s), (unsigned long)u << 8, SBYTES - 1, ACC_KDATA);
    vio_title(s, title);
    vio_cls(s);
    return s;
}

void vio_title(int s, const char *title)
{
    int i;
    for (i = 0; i < 12 && title[i]; i++)
        ses[s].title[i] = title[i];
    ses[s].title[i] = 0;
    if (s == fg)
        title_bar();
}

void vio_close(int s)
{
    if (!ses[s].used)
        return;
    mem_free(ses[s].unit, (SBYTES + MEM_UNIT - 1) / MEM_UNIT);
    if (ses[s].save) {
        mem_free(ses[s].save, SAVEU);
        ses[s].save = 0;
    }
    if (ses[s].gfx_save) {
        mem_free(ses[s].gfx_save, GFX_UNITS(ses[s].gfx));
        ses[s].gfx_save = 0;
    }
    ses[s].used = 0;
    gdt[SCR_IDX(s)].access = 0;
    if (s == fg) {
        fg = -1;
        vio_next();
    }
}

int vio_used(int s)
{
    return ses[s].used;
}

/* nach der Kompatibilitaetsbox: Textmodus neu einstellen, alles neu zeichnen */
void vio_rehw(void)
{
    gfx_hwreset();
    redraw();
}

int vio_fg(void)
{
    return fg;
}

void vio_focus(int s)
{
    if (s < 0 || s >= NTASK || !ses[s].used || s == fg)
        return;
    gfx_out();                      /* Grafik der alten Sitzung sichern */
    fg = s;
    kbd_flush();
    redraw();
}

void vio_next(void)
{
    int n, s;
    for (n = 1; n <= NTASK; n++) {
        s = (fg + n + NTASK) % NTASK;
        if (ses[s].used) {
            if (s != fg)
                vio_focus(s);
            return;
        }
    }
}

/* ANSI wie ANSI.SYS bzw. OS/2 (VioSetAnsi): ESC[ Zeile;Spalte H/f, n A/B/C/D,
   s/u, 2J, K, Farben mit m. Tastenbelegung (p) und Modi (h/l) werden
   geschluckt. Farben: ANSI-Reihenfolge -> PC-Attribut */
static const unsigned char ansi_col[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void ansi_sgr(struct session *p)
{
    unsigned i, v;
    if (!p->np)
        p->np = 1, p->par[0] = 0;
    for (i = 0; i < p->np; i++) {
        v = p->par[i];
        if (v == 0)
            p->attr = ATTR;
        else if (v == 1)
            p->attr |= 0x08;                /* hell                       */
        else if (v == 22)
            p->attr &= ~0x08;
        else if (v == 5)
            p->attr |= 0x80;                /* blinkend                   */
        else if (v == 25)
            p->attr &= ~0x80;
        else if (v == 7)                    /* invers                     */
            p->attr = (unsigned char)((p->attr & 0x88) | ((p->attr >> 4) & 7) |
                                      ((p->attr & 7) << 4));
        else if (v == 8)                    /* verdeckt                   */
            p->attr = (unsigned char)((p->attr & 0xF0) | ((p->attr >> 4) & 7));
        else if (v >= 30 && v <= 37)
            p->attr = (unsigned char)((p->attr & 0xF8) | ansi_col[v - 30]);
        else if (v == 39)
            p->attr = (unsigned char)((p->attr & 0xF8) | 7);
        else if (v >= 40 && v <= 47)
            p->attr = (unsigned char)((p->attr & 0x8F) | (ansi_col[v - 40] << 4));
        else if (v == 49)
            p->attr &= 0x8F;
    }
}

/* 1 = Zeichen gehoerte zu einer ANSI-Folge */
static int ansi_char(int s, char ch)
{
    struct session *p = &ses[s];
    unsigned n, c;

    if (p->ast == 0) {
        if (ch != 27 || !p->ansi)
            return 0;
        p->ast = 1;
        return 1;
    }
    if (p->ast == 1) {                      /* nach ESC                   */
        if (ch == '[') {
            p->ast = 2;
            p->np = 0;
            p->par[0] = 0;
            return 1;
        }
        p->ast = 0;                         /* anderes ESC x: weg         */
        return 1;
    }
    if (p->ast == 3) {                      /* in "..." (Tastenbelegung)  */
        if (ch == '"')
            p->ast = 2;
        return 1;
    }
    if (ch >= '0' && ch <= '9') {
        if (!p->np)
            p->np = 1, p->par[0] = 0;
        n = p->par[p->np - 1] * 10 + (unsigned)(ch - '0');
        p->par[p->np - 1] = (unsigned char)(n > 255 ? 255 : n);
        return 1;
    }
    if (ch == ';') {
        if (!p->np)
            p->np = 1, p->par[0] = 0;
        if (p->np < sizeof(p->par))
            p->par[p->np++] = 0;
        return 1;
    }
    if (ch == '"') {
        p->ast = 3;
        return 1;
    }
    if (ch == '=' || ch == '?')             /* ESC[=7h, ESC[?7h           */
        return 1;
    p->ast = 0;
    n = p->np ? p->par[0] : 0;
    switch (ch) {
    case 'H': case 'f':                     /* Cursor setzen (1-basiert)  */
        c = p->np > 1 ? p->par[1] : 0;
        p->row = (unsigned char)(n ? (n > p->rows ? p->rows - 1 : n - 1) : 0);
        p->col = (unsigned char)(c ? (c > SCOLS ? SCOLS - 1 : c - 1) : 0);
        break;
    case 'A':
        if (!n) n = 1;
        p->row = (unsigned char)(p->row > n ? p->row - n : 0);
        break;
    case 'B':
        if (!n) n = 1;
        p->row = (unsigned char)(p->row + n < p->rows ? p->row + n : p->rows - 1);
        break;
    case 'C':
        if (!n) n = 1;
        p->col = (unsigned char)(p->col + n < SCOLS ? p->col + n : SCOLS - 1);
        break;
    case 'D':
        if (!n) n = 1;
        p->col = (unsigned char)(p->col > n ? p->col - n : 0);
        break;
    case 's':
        p->arow = p->row;
        p->acol = p->col;
        break;
    case 'u':
        p->row = p->arow < p->rows ? p->arow : p->rows - 1;
        p->col = p->acol;
        break;
    case 'J':                               /* 2J (und J): ganz loeschen  */
        vio_cls(s);
        break;
    case 'K':                               /* bis Zeilenende loeschen    */
        for (c = p->col; c < SCOLS; c++)
            put(s, p->row, c, CELL(p->attr, ' '));
        break;
    case 'm':
        ansi_sgr(p);
        break;
    default:                                /* h, l, p, n ...: schlucken  */
        break;
    }
    return 1;
}

static void tty_char(int s, char ch)
{
    struct session *p = &ses[s];

    if (ansi_char(s, ch))
        return;
    switch (ch) {
    case '\r':
        p->col = 0;
        break;
    case '\n':                      /* LF allein reicht fuer neue Zeile */
        p->col = 0;
        p->row++;
        break;
    case '\b':
        if (p->col > 0)
            p->col--;
        else if (p->row > 0) {      /* lange Eingabezeile: zurueck in   */
            p->row--;               /* die vorige Bildschirmzeile       */
            p->col = SCOLS - 1;
        }
        break;
    case '\t':
        p->col = (unsigned char)((p->col + 8) & ~7);
        break;
    case 7:                         /* Klingel: still                   */
        break;
    default:
        put(s, p->row, p->col, CELL(p->attr, ch));
        p->col++;
    }
    if (p->col >= SCOLS) {
        p->col = 0;
        p->row++;
    }
    if (p->row >= p->rows) {
        scroll(s);
        p->row = (unsigned char)(p->rows - 1);
    }
}

void vio_tty(int s, unsigned sel, unsigned off, unsigned len)
{
    const char __far *p = MK_FP(sel, 0);
    if (s < 0 || !ses[s].used)
        return;
    while (len--) {
        tty_char(s, p[off]);
        off++;
    }
    if (s == fg)
        cur_dirty = 1;
}

/* VioSetAnsi: on 0/1, 2 = nur abfragen -> bisheriger Zustand */
int vio_ansi(int s, unsigned on)
{
    int old;
    if (s < 0 || !ses[s].used)
        return 0;
    old = ses[s].ansi;
    if (on < 2) {
        ses[s].ansi = (unsigned char)on;
        ses[s].ast = 0;
        if (!on)
            ses[s].attr = ATTR;
    }
    return old;
}

void vio_kputs(int s, const char *str)
{
    unsigned n = 0;
    while (str[n])
        n++;
    vio_tty(s, KDATA, (unsigned)str, n);
    vio_flush();                    /* Kernelmeldungen sofort zeigen    */
}

/* geaenderte Zeilen der Vordergrund-Sitzung in den Bildspeicher */
void vio_flush(void)
{
    unsigned a, b, f;

    if (vio_frozen)
        return;
    if (fg >= 0 && ses[fg].gfx) {   /* Grafik: Text bleibt im Puffer,   */
        gptr_update();              /* nur der Mauszeiger               */
        return;
    }
    if (fg < 0)
        return;
    if (sel_copy)                   /* Taste losgelassen: kopieren      */
        sel_finish();
    if (paste_req) {                /* rechte/mittlere Taste: einfuegen */
        paste_req = 0;
        if (clip_len) {
            paste_pos = 0;
            paste_s = (signed char)fg;
        }
    }
    f = irq_off();
    a = dlo;
    b = dhi;
    dlo = 0xFF;                     /* vor dem Kopieren: neue Aenderungen
                                       bleiben fuer den naechsten Lauf */
    dhi = 0;
    irq_restore(f);
    ptr_remove();
    if (a != 0xFF) {
        if (b >= ses[fg].rows)
            b = ses[fg].rows - 1;
        if (a <= b) {
            far_copy(VIDEO, (a + ses[fg].top) * SCOLS * 2, SCR_SEL(fg),
                     a * SCOLS * 2, (b - a + 1) * SCOLS * 2);
            sel_paint(a, b);
        }
    }
    if (cur_dirty) {
        cur_dirty = 0;
        cursor();
    }
    ptr_draw();
}

void vio_str(int s, int row, int col, unsigned sel, unsigned off,
             unsigned limit, unsigned char attr)
{
    const char __far *p = MK_FP(sel, 0);
    char c;

    if (s < 0 || !ses[s].used || row < 0 || row >= ses[s].rows)
        return;
    while (col < SCOLS && off <= limit) {
        c = p[off];
        if (c == 0)
            break;
        put(s, row, col++, CELL(attr, c));
        if (++off == 0)
            break;
    }
}

int vio_rows(int s)
{
    return (s >= 0 && ses[s].used) ? ses[s].rows : SROWS;
}


/* Vollbild ein/aus. Der normale Inhalt wird gesichert und danach wieder
   hergestellt (wie bei MS-DOS EDIT); das Vollbild beginnt leer. */
void vio_setmode(int s, int full, int owner)
{
    struct session *p;
    unsigned short w[8];
    if (s < 0 || !ses[s].used || full == !ses[s].top)
        return;
    p = &ses[s];
    win_save(w);                            /* laeuft ohne Kernelsperre */
    if (full) {
        p->save = mem_alloc(SAVEU);
        if (p->save) {
            scratch_map((unsigned long)p->save << 8, NBYTES - 1);
            far_copy(SCRATCH, 0, SCR_SEL(s), 0, NBYTES);
            p->srow = p->row;
            p->scol = p->col;
        }
    }
    p->rows = (unsigned char)(full ? p->lines : p->lines - 2);
    p->top = (unsigned char)(full ? 0 : 1);
    p->hidecur = 0;
    p->owner = (signed char)(full ? owner : -1);
    if (!full && p->save) {
        scratch_map((unsigned long)p->save << 8, NBYTES - 1);
        far_copy(SCR_SEL(s), 0, SCRATCH, 0, NBYTES);
        mem_free(p->save, SAVEU);
        p->save = 0;
        p->row = p->srow;
        p->col = p->scol;
        win_restore(w);
        if (s == fg)
            redraw();
        return;
    }
    win_restore(w);
    vio_cls(s);                     /* zeichnet im Vordergrund neu     */
}

/* VioGetBuf/VioShowBuf (logischer Bildschirmpuffer wie bei OS/2 1.x):
   Das Programm schreibt direkt in den Sitzungspuffer und meldet fertige
   Zeilen - erst dann werden sie als geaendert markiert und gezeigt. So
   sieht man nie halbfertige Bilder, und es braucht keinen FAPI-Aufruf je
   Zeile. */
unsigned long vio_buf_lin(int s)
{
    return (unsigned long)ses[s].unit << 8;
}

void vio_showbuf(int s, unsigned a, unsigned b)
{
    if (s < 0 || !ses[s].used || s != fg || a > b)
        return;                             /* Hintergrund: beim Wechsel
                                               zeichnet redraw alles      */
    if (a >= ses[s].rows)
        return;
    if (b >= ses[s].rows)
        b = ses[s].rows - 1;
    mark((unsigned char)a, (unsigned char)b);
}

/* Programm endet: hatte es das Vollbild verlangt, wieder normal */
void vio_task_end(int s, int t)
{
    if (s >= 0 && ses[s].used && ses[s].gfx && ses[s].gfx_task == t)
        vio_setgfx(s, t, 0);                /* Grafik zuruecknehmen     */
    if (s >= 0 && ses[s].used && ses[s].mou_owner == t) {
        ses[s].mou_owner = -1;              /* Maus wieder frei         */
        ses[s].mou_show = 0;
        ses[s].mq_n = 0;
    }
    if (s >= 0 && ses[s].used && !ses[s].top && ses[s].owner == t)
        vio_setmode(s, 0, -1);
}

/* ---- Maus --------------------------------------------------------------- */
static void ptr_remove(void)
{
    if (p_drawn) {
        if (VRAM[p_idx] == p_inv)           /* sonst schon ueberschrieben */
            VRAM[p_idx] = p_saved;
        p_drawn = 0;
    }
}

static void ptr_draw(void)
{
    unsigned r, c;
    if (p_drawn || fg < 0 || !mou_nbtn || ses[fg].gfx)
        return;
    if (ses[fg].mou_owner >= 0 ? !ses[fg].mou_show : !con_ptr)
        return;
    r = (unsigned)my >> 4;
    c = (unsigned)mx >> 3;
    if (r >= hw_lines || c >= SCOLS)
        return;
    p_idx = r * SCOLS + c;
    p_saved = VRAM[p_idx];
    p_inv = p_saved ^ 0x7700;
    VRAM[p_idx] = p_inv;
    p_drawn = 1;
}

/* Zwischenablage anlegen (einmal, beim Start) */
static void clip_init(void)
{
    if (!clip_sel)
        clip_sel = kseg_alloc(CLIP_MAX);
}

/* Zeilen der Markierung (Sitzung) neu zeichnen lassen */
static void sel_mark(unsigned a, unsigned b, unsigned c)
{
    unsigned lo = a, hi = a;
    if (b < lo) lo = b;
    if (c < lo) lo = c;
    if (b > hi) hi = b;
    if (c > hi) hi = c;
    mark((unsigned char)(lo / SCOLS), (unsigned char)(hi / SCOLS));
}

/* markierte Zellen in den Pufferzeilen a..b im Bildspeicher umdrehen
   (nur frisch kopierte Zeilen - sonst hoebe XOR die Markierung auf) */
static void sel_paint(unsigned a, unsigned b)
{
    unsigned lo, hi, i, top;
    if (!sel_on || sel_s != fg || fg < 0)
        return;
    lo = sel_a < sel_b ? sel_a : sel_b;
    hi = sel_a < sel_b ? sel_b : sel_a;
    if (lo < a * SCOLS)
        lo = a * SCOLS;
    if (hi > b * SCOLS + SCOLS - 1)
        hi = b * SCOLS + SCOLS - 1;
    top = ses[fg].top * SCOLS;
    for (i = lo; i <= hi; i++)
        VRAM[top + i] ^= 0x7700;
}

/* Markierung beendet: Text in die Zwischenablage (Zeilenende CR LF,
   Leerzeichen am Zeilenende weg), Markierung vom Bildschirm nehmen */
static void sel_finish(void)
{
    unsigned lo, hi, i, r, e, n = 0, f;
    unsigned short __far *bp;
    char __far *d;

    f = irq_off();
    sel_copy = 0;
    if (!sel_on) {
        irq_restore(f);
        return;
    }
    sel_on = 0;
    lo = sel_a < sel_b ? sel_a : sel_b;
    hi = sel_a < sel_b ? sel_b : sel_a;
    irq_restore(f);
    if (sel_s != fg || fg < 0)
        return;
    mark((unsigned char)(lo / SCOLS), (unsigned char)(hi / SCOLS));
    if (!clip_sel || lo == hi)
        return;                         /* nur geklickt: nichts kopieren */
    bp = BUF(fg);
    d = MK_FP(clip_sel, 0);
    for (r = lo / SCOLS; r <= hi / SCOLS; r++) {
        i = r == lo / SCOLS ? lo : r * SCOLS;
        e = r == hi / SCOLS ? hi : r * SCOLS + SCOLS - 1;
        if (r != hi / SCOLS)            /* Leerzeichen am Ende weg       */
            while (e > i && (unsigned char)bp[e] == ' ')
                e--;
        for (; i <= e && n < CLIP_MAX - 2; i++)
            d[n++] = (char)bp[i];
        if (r != hi / SCOLS && n < CLIP_MAX - 2) {
            d[n++] = '\r';
            d[n++] = '\n';
        }
    }
    clip_len = n;
}

/* naechstes eingefuegtes Zeichen fuer kbd_get (take = 1) / kbd_peek;
   -1 = nichts einzufuegen. CR, LF und CR LF werden zu Enter. */
int vio_paste(int take)
{
    const char __far *d;
    unsigned char c;
    unsigned f = irq_off();
    int k = -1;

    if (paste_s >= 0 && (paste_s != fg || paste_pos >= clip_len))
        paste_s = -1;                   /* Sitzung gewechselt / fertig   */
    if (paste_s >= 0) {
        d = MK_FP(clip_sel, 0);
        c = (unsigned char)d[paste_pos];
        if (c == '\n' && paste_pos > 0 && d[paste_pos - 1] == '\r') {
            paste_pos++;                /* LF nach CR: schon Enter       */
            if (paste_pos >= clip_len) {
                paste_s = -1;
                irq_restore(f);
                return -1;
            }
            c = (unsigned char)d[paste_pos];
        }
        k = (c == '\r' || c == '\n') ? 0x1C0D : (c == '\t' ? 0x0F09 : c);
        if (take)
            paste_pos++;
    }
    irq_restore(f);
    return k;
}

/* ClipPut/ClipGet: Zwischenablage <-> sel:off (Laenge geprueft) */
unsigned vio_clip_put(unsigned sel, unsigned off, unsigned len)
{
    if (!clip_sel)
        return 0;
    if (len > CLIP_MAX)
        len = CLIP_MAX;
    far_copy(clip_sel, 0, sel, off, len);
    clip_len = len;
    return len;
}

unsigned vio_clip_get(unsigned sel, unsigned off, unsigned max)
{
    unsigned n = clip_len < max ? clip_len : max;
    if (clip_sel && n)
        far_copy(sel, off, clip_sel, 0, n);
    return clip_len;
}

/* Treiber meldet die Maus an (DevHlp MouReg) */
void vio_mou_reg(unsigned buttons)
{
    mou_nbtn = (unsigned char)(buttons > 3 ? 3 : buttons);
    mx = SCOLS * 8 / 2;
    my = 12 * 16;
    clip_init();
}

/* aus der Interruptroutine (IF aus): bewegen, Ereignis einreihen */
void vio_mou_event(unsigned buttons, int dx, int dy)
{
    unsigned or_ = (unsigned)my >> 4, oc = (unsigned)mx >> 3, r, c, n;
    unsigned char gm = fg >= 0 ? ses[fg].gfx : 0;
    int ox = mx, oy = my, gx, gy;
    struct session *p;
    struct mouev *e;

    mx += dx;
    my += dy;
    if (mx < 0) mx = 0;
    if (mx > SCOLS * 8 - 1) mx = SCOLS * 8 - 1;
    if (my < 0) my = 0;
    /* Grafik: virtuelles Raster wie beim Maustreiber von Microsoft -
       12h 640x480 Punkte, 13h 640x400 (halbiert 320x200), Mode X
       640x480 (halbiert 320x240) */
    n = PLANAR(gm) ? 479 : gm == 0x13 ? 399 : hw_lines * 16 - 1;
    if (my > (int)n) my = (int)n;
    gx = gm == 0x13 || gm == MODEX ? mx >> 1 : mx;
    gy = gm == 0x13 || gm == MODEX ? my >> 1 : my;
    r = (unsigned)my >> 4;
    c = (unsigned)mx >> 3;
    if (gm) {                               /* Grafik: Bildpunkt zaehlt */
        if ((gm == 0x13 || gm == MODEX ? (ox >> 1 == gx && oy >> 1 == gy) : (ox == gx && oy == gy)) &&
            buttons == mbtn)
            return;
    } else if (r == or_ && c == oc && buttons == mbtn)
        return;                             /* gleiche Zelle            */
    n = mbtn;                               /* Tasten vorher            */
    mbtn = (unsigned char)buttons;
    cur_dirty = 1;                          /* vio_flush zeichnet neu   */
    if (fg < 0)
        return;
    p = &ses[fg];
    if (gm && p->mou_owner < 0)
        return;                             /* Grafik ohne Mausprogramm */
    if (p->mou_owner < 0) {                 /* Konsolenmaus (Kernel)    */
        con_ptr = 1;
        r -= p->top;
        if (r < p->rows) {
            c += r * SCOLS;                 /* Zelle in der Sitzung     */
            if ((buttons & MOU_LEFT) && !(n & MOU_LEFT)) {
                if (sel_on && sel_s == fg)
                    sel_mark(sel_a, sel_b, c);
                sel_on = 1;
                sel_s = (signed char)fg;
                sel_a = sel_b = c;
                sel_mark(c, c, c);
            } else if ((buttons & MOU_LEFT) && sel_on && sel_s == fg && c != sel_b) {
                sel_mark(sel_a, sel_b, c);
                sel_b = c;
            }
        }
        if (!(buttons & MOU_LEFT) && (n & MOU_LEFT) && sel_on)
            sel_copy = 1;                   /* kopieren in vio_flush    */
        if ((buttons & (MOU_RIGHT | MOU_MIDDLE)) && !(n & (MOU_RIGHT | MOU_MIDDLE)))
            paste_req = 1;
        return;
    }
    if (gm) {                               /* Grafik: Zelle zu 8x8 Punkten */
        r = (unsigned)gy >> 3;
        c = (unsigned)gx >> 3;
    } else {
        r -= p->top;                        /* Zeile der Sitzung        */
        if (r >= p->rows)
            r = 0xFF;
    }
    if (p->mq_n == MQ_LEN && mq[fg][(p->mq_head + MQ_LEN - 1) % MQ_LEN].b == buttons)
        n = (p->mq_head + MQ_LEN - 1) % MQ_LEN;   /* voll: letzte Bewegung ersetzen */
    else if (p->mq_n == MQ_LEN)
        return;                             /* voll, Taste geht vor: verwerfen */
    else
        n = (p->mq_head + p->mq_n++) % MQ_LEN;
    e = &mq[fg][n];
    e->b = (unsigned char)buttons;
    e->row = (unsigned char)r;
    e->col = (unsigned char)c;
    e->x = (unsigned short)(gm ? gx : mx);  /* Text: virtuell 640x... */
    e->y = (unsigned short)(gm ? gy : my);
}

/* MouOpen: Ereignisse fuer die Sitzung an -> Tasten, 0 = keine Maus */
unsigned vio_mou_open(int s, int t)
{
    unsigned f;
    if (!mou_nbtn || s < 0 || !ses[s].used)
        return 0;
    f = irq_off();
    ses[s].mou_owner = (signed char)t;
    ses[s].mou_show = 0;
    ses[s].mq_n = 0;
    irq_restore(f);
    return mou_nbtn;
}

/* naechstes Ereignis nach e (8 Byte: Tasten, Zeile, Spalte, 0, x, y);
   0 = keins */
int vio_mou_read(int s, unsigned char *e)
{
    unsigned f;
    struct mouev *q;
    if (s < 0 || !ses[s].used || ses[s].mou_owner < 0)
        return 0;
    f = irq_off();
    if (!ses[s].mq_n) {
        irq_restore(f);
        return 0;
    }
    q = &mq[s][ses[s].mq_head];
    e[0] = q->b;
    e[1] = q->row;
    e[2] = q->col;
    e[3] = 0;
    e[4] = (unsigned char)q->x;
    e[5] = (unsigned char)(q->x >> 8);
    e[6] = (unsigned char)q->y;
    e[7] = (unsigned char)(q->y >> 8);
    ses[s].mq_head = (unsigned char)((ses[s].mq_head + 1) % MQ_LEN);
    ses[s].mq_n--;
    irq_restore(f);
    return 1;
}

void vio_mou_show(int s, int on)
{
    if (s < 0 || !ses[s].used || ses[s].mou_owner < 0)
        return;
    ses[s].mou_show = (unsigned char)(on != 0);
    cur_dirty = 1;
}

void vio_setcur(int s, int row, int col)
{
    if (s < 0 || !ses[s].used)
        return;
    if (row == 0xFF) {
        ses[s].hidecur = 1;
    } else {
        if (row >= ses[s].rows || col >= SCOLS)
            return;
        ses[s].hidecur = 0;
        ses[s].row = (unsigned char)row;
        ses[s].col = (unsigned char)col;
    }
    if (s == fg)
        cur_dirty = 1;
}

/* Bildschirmzeilen der Sitzung (MODE CON LINES=): 25, 43, 50 (nur VGA);
   0 = nur fragen. Nicht im Vollbild. Der Inhalt wird geloescht.
   -> Zeilenzahl oder -1 */
int vio_setlines(int s, int n)
{
    if (s < 0 || !ses[s].used)
        return -1;
    if (n == 0)
        return ses[s].lines;
    if ((n != 25 && n != 43 && n != 50) || (n != 25 && !vga_ok) ||
        !ses[s].top)
        return -1;
    ses[s].lines = (unsigned char)n;
    ses[s].rows = (unsigned char)(n - 2);
    vio_cls(s);
    if (s == fg)
        redraw();
    return n;
}
