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
    if (fg < 0)
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

    if (v == shown_uptime || (fg >= 0 && !ses[fg].top))
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
    dlo = 0xFF;
    cur_dirty = 0;
    if (fg >= 0)
        set_hw_lines(ses[fg].lines);
    for (r = hw_lines; r < SLINES_MAX; r++)     /* 43: Rest der 44. Zeile */
        vfill(r, ATTR);
    if (fg >= 0 && !ses[fg].top) {          /* Vollbild                 */
        far_copy(VIDEO, 0, SCR_SEL(fg), 0, ses[fg].rows * SCOLS * 2);
    } else {
        title_bar();
        if (fg >= 0)
            far_copy(VIDEO, SCOLS * 2, SCR_SEL(fg), 0, ses[fg].rows * SCOLS * 2);
        shown_uptime = 0xFFFF;              /* Statuszeile neu zeichnen  */
        vio_status();
    }
    cursor();
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

    if (fg < 0)
        return;
    f = irq_off();
    a = dlo;
    b = dhi;
    dlo = 0xFF;                     /* vor dem Kopieren: neue Aenderungen
                                       bleiben fuer den naechsten Lauf */
    dhi = 0;
    irq_restore(f);
    if (a != 0xFF) {
        if (b >= ses[fg].rows)
            b = ses[fg].rows - 1;
        if (a <= b)
            far_copy(VIDEO, (a + ses[fg].top) * SCOLS * 2, SCR_SEL(fg),
                     a * SCOLS * 2, (b - a + 1) * SCOLS * 2);
    }
    if (cur_dirty) {
        cur_dirty = 0;
        cursor();
    }
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
    if (s < 0 || !ses[s].used || full == !ses[s].top)
        return;
    p = &ses[s];
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
        if (s == fg)
            redraw();
    } else
        vio_cls(s);                 /* zeichnet im Vordergrund neu     */
}

/* Programm endet: hatte es das Vollbild verlangt, wieder normal */
void vio_task_end(int s, int t)
{
    if (s >= 0 && ses[s].used && !ses[s].top && ses[s].owner == t)
        vio_setmode(s, 0, -1);
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
