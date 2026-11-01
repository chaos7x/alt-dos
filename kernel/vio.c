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

struct session {
    unsigned char used;
    unsigned char row, col;
    unsigned char rows, top;        /* 23/1 normal, 25/0 Vollbild       */
    unsigned char lines;            /* Bildschirmzeilen 25/43/50        */
    unsigned char hidecur;          /* Cursor aus (Vollbild)            */
    signed char   owner;            /* Task, der Vollbild verlangt hat  */
    unsigned char srow, scol;       /* Cursor vor dem Vollbild          */
    unsigned      save;             /* gesicherter Inhalt (Einheit), 0 = keiner */
    unsigned      unit;
    char          title[13];
    signed char   mou_owner;        /* Task mit MouOpen, -1 = keiner    */
    unsigned char mou_show;         /* Zeiger zeigen                    */
    unsigned char mq_head, mq_n;    /* Warteschlange der Mausereignisse */
    unsigned char gfx;              /* Grafikmodus (13h), 0 = Text      */
    signed char   gfx_task;         /* Task mit dem Bildspeicher-Selektor */
    unsigned      gfx_sel;          /* ... dieser Selektor (0 = keiner) */
    unsigned      gfx_save;         /* Puffer: Bild + 768 Byte Palette  */
    unsigned char gfx_plane;        /* 12h: gewaehlte Ebene (VioSetPlane) */
};

static struct session ses[NTASK];
static int fg = -1;
static unsigned char dlo = 0xFF, dhi; /* geaenderte Pufferzeilen dlo..dhi
                                         der Vordergrund-Sitzung (FF = keine) */
static unsigned char cur_dirty;
static unsigned shown_uptime = 0xFFFF;
static unsigned char hw_lines = 25; /* Zeilen, auf die die Karte steht  */

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
struct mouev { unsigned char b, row, col; };
static struct mouev mq[NTASK][MQ_LEN];
static unsigned char mou_nbtn;          /* 0 = keine Maus                */
static int mx, my;                      /* Mickeys                       */
static unsigned char mbtn;              /* Tasten jetzt                  */
static unsigned char p_drawn;           /* Zeiger steht im Bildspeicher  */
static unsigned p_idx;                  /* ... an dieser Zelle           */
static unsigned short p_saved, p_inv;   /* Zelle vorher / mit Zeiger     */

static void ptr_remove(void);
static void ptr_draw(void);

/* ---- Grafik (VGA, Modus 13h) --------------------------------------------
   Wie VioGetPhysBuf bei OS/2 1.x bekommt das Programm einen Selektor auf
   den Bildspeicher - aber bequemer: Beim Sitzungswechsel sichert der Kernel
   Bild (64 KB) und Palette in den Puffer der Sitzung und biegt den
   Selektor dorthin um; das Programm zeichnet im Hintergrund einfach
   weiter. Zurueck im Vordergrund: Karte umstellen, Bild und Palette
   zurueck, Selektor wieder auf A0000h. Kein BIOS im Protected Mode: die
   Register kommen aus den Standardtabellen der IBM-VGA. */
/* Puffer: 13h 64 KB Bild, 12h vier Ebenen zu 38400 Byte; Palette dahinter */
#define PLANE12     38400U
#define GFX_IMG(m)  ((m) == 0x12 ? 4UL * PLANE12 : 65536UL)
#define GFX_UNITS(m) ((unsigned)((GFX_IMG(m) + 768 + MEM_UNIT - 1) / MEM_UNIT))
static unsigned char hw_gfx;            /* Karte steht auf: 0 Text, 13h  */
static unsigned char text_dac[64 * 3], text_dac_ok;
/* Misc, Sequenzer 0-4, CRTC 0-24, Grafik 0-8, Attribut 0-20 */
static const unsigned char vga_mode13[61] = { 0x63,
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00 };
static const unsigned char vga_mode03[61] = { 0x67,
    0x03, 0x00, 0x03, 0x00, 0x02,
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E,
    0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B,
    0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00 };
/* 12h: Attributregister direkt durchgeschaltet (Farbe n = DAC-Eintrag n) */
static const unsigned char vga_mode12[61] = { 0xE3,
    0x03, 0x01, 0x0F, 0x00, 0x06,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xEA, 0x0C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x01, 0x00, 0x0F, 0x00, 0x00 };
static void gfx_out(void);
static int  gfx_in(void);

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

static void redraw(void)
{
    int r;
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
        BUF(s)[r * SCOLS + c] = CELL(ATTR, ' ');
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

static void tty_char(int s, char ch)
{
    struct session *p = &ses[s];

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
        break;
    case '\t':
        p->col = (unsigned char)((p->col + 8) & ~7);
        break;
    case 7:                         /* Klingel: still                   */
        break;
    default:
        put(s, p->row, p->col, CELL(ATTR, ch));
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

    if (fg < 0 || ses[fg].gfx)      /* Grafik: Text bleibt im Puffer    */
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

/* ---- Grafik ------------------------------------------------------------- */
static void vga_regs(const unsigned char *t)
{
    unsigned i;
    seqr(0x00, 0x01);                       /* Sequenzer anhalten       */
    outb(0x3C2, t[0]);
    for (i = 1; i < 5; i++)
        seqr(i, t[1 + i]);
    seqr(0x00, 0x03);
    crtc(0x11, crtc_in(0x11) & 0x7F);       /* CRTC 0-7 freigeben       */
    for (i = 0; i < 25; i++)
        crtc(i, t[6 + i]);
    for (i = 0; i < 9; i++)
        gcr(i, t[31 + i]);
    inb(0x3DA);                             /* Attribut: Index zuerst   */
    for (i = 0; i < 21; i++) {
        outb(0x3C0, i);
        outb(0x3C0, t[40 + i]);
    }
    outb(0x3C0, 0x20);                      /* Bild an                  */
}

static void dac_io(int write, unsigned first, unsigned n, unsigned char __far *p)
{
    unsigned i;
    outb(write ? 0x3C8 : 0x3C7, first);
    for (i = 0; i < n * 3; i++)
        if (write)
            outb(0x3C9, p[i]);
        else
            p[i] = (unsigned char)inb(0x3C9);
}

/* Fenster SCRATCH auf den Puffer: part 0-3 = Bild (13h: nur 0, 64 KB;
   12h: Ebene part), part 4 = Palette */
static unsigned long save_part(int s, int part)
{
    unsigned long b = (unsigned long)ses[s].gfx_save << 8;
    if (part == 4)
        return b + GFX_IMG(ses[s].gfx);
    if (ses[s].gfx == 0x12)
        while (part-- > 0)              /* ohne 32-Bit-Multiplikation   */
            b += PLANE12;
    return b;
}

static void map_save(int s, int part)
{
    scratch_map(save_part(s, part), part == 4 ? 767 :
                ses[s].gfx == 0x12 ? PLANE12 - 1 : 0xFFFF);
}

/* Bild zwischen Bildspeicher und Puffer kopieren; out = 1: sichern */
static void gfx_copy(int s, int out)
{
    unsigned p;
    scratch2_map(0xA0000UL, 0xFFFF);
    if (ses[s].gfx == 0x13) {
        map_save(s, 0);
        if (out) {
            far_copy(SCRATCH, 0, SCRATCH2, 0, 0x8000);
            far_copy(SCRATCH, 0x8000, SCRATCH2, 0x8000, 0x8000);
        } else {
            far_copy(SCRATCH2, 0, SCRATCH, 0, 0x8000);
            far_copy(SCRATCH2, 0x8000, SCRATCH, 0x8000, 0x8000);
        }
        return;
    }
    for (p = 0; p < 4; p++) {               /* 12h: Ebene fuer Ebene    */
        map_save(s, (int)p);
        if (out) {
            gcr(0x04, p);                   /* Ebene p lesen            */
            far_copy(SCRATCH, 0, SCRATCH2, 0, PLANE12);
        } else {
            seqr(0x02, 1 << p);             /* nur Ebene p schreiben    */
            far_copy(SCRATCH2, 0, SCRATCH, 0, PLANE12);
        }
    }
}

/* Selektor des Programms: vorn Bildspeicher, sonst Puffer (12h: Ebene) */
static void gfx_alias(int s)
{
    if (!ses[s].gfx_sel)
        return;
    if (s == fg && hw_gfx == ses[s].gfx)
        ldt_set(ses[s].gfx_task, ses[s].gfx_sel, 0xA0000UL,
                ses[s].gfx == 0x12 ? PLANE12 - 1 : 0xFFFF);
    else
        ldt_set(ses[s].gfx_task, ses[s].gfx_sel, save_part(s, ses[s].gfx_plane),
                ses[s].gfx == 0x12 ? PLANE12 - 1 : 0xFFFF);
}

/* Karte auf Text zurueck: Register, Palette, Zeichensatz (hw_lines = 0
   zwingt set_hw_lines zum Neuprogrammieren - 13h belegt auch Ebene 2) */
static void hw_text(void)
{
    vga_regs(vga_mode03);
    if (text_dac_ok)
        dac_io(1, 0, 64, (unsigned char __far *)text_dac);
    hw_lines = 0;
    hw_gfx = 0;
}

/* Grafiksitzung kommt nach vorn: Karte umstellen, Bild und Palette aus
   dem Puffer, Selektor auf den Bildspeicher. -> 1 = Grafik vorn */
static int gfx_in(void)
{
    unsigned short w[8];
    if (fg < 0 || !ses[fg].gfx) {
        if (hw_gfx)
            hw_text();                      /* Textsitzung vorn         */
        return 0;
    }
    win_save(w);
    if (!text_dac_ok) {                     /* Textpalette einmal merken */
        dac_io(0, 0, 64, (unsigned char __far *)text_dac);
        text_dac_ok = 1;
    }
    vga_regs(ses[fg].gfx == 0x12 ? vga_mode12 : vga_mode13);
    hw_gfx = ses[fg].gfx;
    gfx_copy(fg, 0);
    if (hw_gfx == 0x12) {                   /* Ebene des Programms      */
        seqr(0x02, 1 << ses[fg].gfx_plane);
        gcr(0x04, ses[fg].gfx_plane);
    }
    map_save(fg, 4);
    dac_io(1, 0, 256, MK_FP(SCRATCH, 0));
    win_restore(w);
    gfx_alias(fg);
    return 1;
}

/* Grafiksitzung geht nach hinten: Bild in den Puffer, Selektor dorthin */
static void gfx_out(void)
{
    unsigned short w[8];
    if (fg < 0 || !ses[fg].gfx || hw_gfx != ses[fg].gfx)
        return;
    win_save(w);
    gfx_copy(fg, 1);
    win_restore(w);
    hw_gfx = 0xFF;                          /* Karte haelt nichts mehr  */
    gfx_alias(fg);
}

/* Vorgabepalette (Werte 0-63): 16 EGA-Farben, 6x6x6-Wuerfel, Graustufen */
static void def_palette(unsigned char __far *p)
{
    static const unsigned char ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63} };
    unsigned i, r, g, b, k = 0;
    for (i = 0; i < 16; i++) {
        p[k++] = ega[i][0]; p[k++] = ega[i][1]; p[k++] = ega[i][2];
    }
    for (r = 0; r < 6; r++)
        for (g = 0; g < 6; g++)
            for (b = 0; b < 6; b++) {
                p[k++] = (unsigned char)(r * 63 / 5);
                p[k++] = (unsigned char)(g * 63 / 5);
                p[k++] = (unsigned char)(b * 63 / 5);
            }
    for (i = 0; i < 24; i++) {
        g = i * 63 / 23;
        p[k++] = (unsigned char)g;
        p[k++] = (unsigned char)g;
        p[k++] = (unsigned char)g;
    }
}

/* VioSetGfx: mode 13h = Grafik, 0 = zurueck auf Text. 0 = ok, -1 Fehler */
int vio_setgfx(int s, int t, unsigned mode)
{
    unsigned short w[8];
    if (s < 0 || !ses[s].used)
        return -1;
    if (mode == 0) {
        if (!ses[s].gfx)
            return 0;
        if (ses[s].gfx_sel)
            ldt_drop(ses[s].gfx_task, ses[s].gfx_sel);
        ses[s].gfx_sel = 0;
        mem_free(ses[s].gfx_save, GFX_UNITS(ses[s].gfx));
        ses[s].gfx = 0;
        ses[s].gfx_save = 0;
        if (s == fg)
            redraw();                       /* Karte auf Text, Bild neu */
        return 0;
    }
    if ((mode != 0x13 && mode != 0x12) || !vga_ok)
        return -1;
    if (ses[s].gfx && ses[s].gfx != mode)   /* anderer Modus: neu       */
        vio_setgfx(s, t, 0);
    if (!ses[s].gfx) {
        ses[s].gfx_save = mem_alloc(GFX_UNITS(mode));
        if (!ses[s].gfx_save)
            return -1;
    }
    ses[s].gfx = (unsigned char)mode;
    ses[s].gfx_plane = 0;
    win_save(w);                            /* Puffer: schwarz, Palette */
    if (mode == 0x13) {
        map_save(s, 0);
        zero_seg(SCRATCH, 0x8000);
    } else {
        unsigned p;
        for (p = 0; p < 4; p++) {
            map_save(s, (int)p);
            zero_seg(SCRATCH, PLANE12 / 2);
        }
    }
    map_save(s, 4);
    def_palette(MK_FP(SCRATCH, 0));
    win_restore(w);
    ses[s].gfx_task = (signed char)t;
    if (s == fg) {
        hw_gfx = 0xFF;                      /* neu einstellen           */
        redraw();
    }
    return 0;
}

/* VioGetPhysBuf: Basis des Bildes (vorn A0000h, sonst Puffer); merkt den
   Selektor, damit der Kernel ihn beim Wechsel umbiegen kann */
unsigned long vio_gfx_base(int s)
{
    if (s == fg && hw_gfx == ses[s].gfx)
        return 0xA0000UL;
    return save_part(s, ses[s].gfx_plane);
}

unsigned vio_gfx_limit(int s)
{
    return ses[s].gfx == 0x12 ? PLANE12 - 1 : 0xFFFF;
}

/* VioSetPlane (12h): Ebene p (0-3) zum Schreiben und Lesen */
int vio_setplane(int s, unsigned p)
{
    if (s < 0 || !ses[s].used || ses[s].gfx != 0x12 || p > 3)
        return -1;
    ses[s].gfx_plane = (unsigned char)p;
    if (s == fg && hw_gfx == 0x12) {
        seqr(0x02, 1 << p);
        gcr(0x04, p);
    } else
        gfx_alias(s);                       /* Hintergrund: Puffer-Ebene */
    return 0;
}

int vio_gfx_reg(int s, int t, unsigned sel)
{
    if (s < 0 || !ses[s].used || !ses[s].gfx)
        return -1;
    if (ses[s].gfx_sel)                     /* nur einer je Sitzung     */
        ldt_drop(ses[s].gfx_task, ses[s].gfx_sel);
    ses[s].gfx_task = (signed char)t;
    ses[s].gfx_sel = sel;
    return 0;
}

int vio_is_gfx(int s)
{
    return s >= 0 && ses[s].used && ses[s].gfx;
}

/* VioSetPalette: n Eintraege ab first (je 3 Byte, 0-63) aus sel:off */
void vio_setpal(int s, unsigned first, unsigned n, unsigned sel, unsigned off)
{
    unsigned short w[8];
    if (!ses[s].gfx || first > 255 || n == 0 || n > 256 - first)
        return;
    win_save(w);
    map_save(s, 4);
    far_copy(SCRATCH, first * 3, sel, off, n * 3);
    if (s == fg && hw_gfx == ses[s].gfx)
        dac_io(1, first, n, MK_FP(sel, off));
    win_restore(w);
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
    struct session *p;
    struct mouev *e;

    mx += dx;
    my += dy;
    if (mx < 0) mx = 0;
    if (mx > SCOLS * 8 - 1) mx = SCOLS * 8 - 1;
    if (my < 0) my = 0;
    if (my > hw_lines * 16 - 1) my = hw_lines * 16 - 1;
    r = (unsigned)my >> 4;
    c = (unsigned)mx >> 3;
    if (r == or_ && c == oc && buttons == mbtn)
        return;                             /* gleiche Zelle            */
    n = mbtn;                               /* Tasten vorher            */
    mbtn = (unsigned char)buttons;
    cur_dirty = 1;                          /* vio_flush zeichnet neu   */
    if (fg < 0)
        return;
    p = &ses[fg];
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
    r -= p->top;                            /* Zeile der Sitzung        */
    if (r >= p->rows)
        r = 0xFF;
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

/* naechstes Ereignis nach e (3 Byte); 0 = keins */
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
