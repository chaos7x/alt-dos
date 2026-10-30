/*
 * AT-DOS 5.0 - Sitzungen und Konsole (VIO)
 *
 * Jede Sitzung hat einen eigenen Textpuffer (23 x 80) im Arbeitsspeicher,
 * erreichbar ueber einen eigenen GDT-Selektor. Programme schreiben immer
 * in den Puffer ihrer Sitzung; ist die Sitzung im Vordergrund, landet die
 * Ausgabe zusaetzlich direkt im Bildspeicher. Alt+Esc schaltet um.
 *
 * Bildschirm: Zeile 0 Titel, 1-23 Sitzung, 24 Statuszeile.
 */
#include "kern.h"
#include "mem.h"

#define ATTR        0x07
#define TITLE_ATTR  0x1F
#define STATUS_ATTR 0x70

struct session {
    unsigned char used;
    unsigned char row, col;
    unsigned      unit;
    char          title[13];
};

static struct session ses[NTASK];
static int fg = -1;
static unsigned shown_uptime = 0xFFFF;

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
    pos = (ses[fg].row + 1) * SCOLS + ses[fg].col;
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

    if (v == shown_uptime)
        return;
    if (shown_uptime == 0xFFFF) {
        vfill(24, STATUS_ATTR);
        vputs(24, 1, "Alt+Esc: Sitzung wechseln \xB3 Strg+C: Abbruch \xB3", STATUS_ATTR);
        vputs(24, 62, "Uptime:       s", STATUS_ATTR);
    }
    shown_uptime = v;
    for (i = 4; i >= 0; i--) {
        b[i] = (char)('0' + v % 10);
        v /= 10;
    }
    b[5] = 0;
    vputs(24, 70, b, STATUS_ATTR);
}

static void redraw(void)
{
    title_bar();
    if (fg >= 0)
        far_copy(VIDEO, SCOLS * 2, SCR_SEL(fg), 0, SBYTES);
    cursor();
}

static void put(int s, int r, int c, unsigned short v)
{
    BUF(s)[r * SCOLS + c] = v;
    if (s == fg)
        VRAM[(r + 1) * SCOLS + c] = v;
}

static void clear_row(int s, int r)
{
    int c;
    for (c = 0; c < SCOLS; c++)
        BUF(s)[r * SCOLS + c] = CELL(ATTR, ' ');
}

static void scroll(int s)
{
    far_copy(SCR_SEL(s), 0, SCR_SEL(s), SCOLS * 2, (SROWS - 1) * SCOLS * 2);
    clear_row(s, SROWS - 1);
    if (s == fg)
        far_copy(VIDEO, SCOLS * 2, SCR_SEL(s), 0, SBYTES);
}

void vio_cls(int s)
{
    int r;
    for (r = 0; r < SROWS; r++)
        clear_row(s, r);
    ses[s].row = ses[s].col = 0;
    if (s == fg)
        redraw();
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
    if (p->row >= SROWS) {
        scroll(s);
        p->row = SROWS - 1;
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
        cursor();
}

void vio_kputs(int s, const char *str)
{
    unsigned n = 0;
    while (str[n])
        n++;
    vio_tty(s, KDATA, (unsigned)str, n);
}

void vio_str(int s, int row, int col, unsigned sel, unsigned off,
             unsigned limit, unsigned char attr)
{
    const char __far *p = MK_FP(sel, 0);
    char c;

    if (s < 0 || !ses[s].used || row < 0 || row >= SROWS)
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
