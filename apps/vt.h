/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Terminal wie xterm (Teilmenge, per #include, zuerst fuer
 * SSH.EXE): schreibt in den logischen Bildschirmpuffer (VioGetBuf) der
 * Vollbild-Sitzung. Eigene Umsetzung nach ECMA-48 und der Beschreibung der
 * xterm-Steuerfolgen (nur gelesen).
 *   C0: BEL BS HT LF VT FF CR SO SI; ESC 7 8 D E M c ( ) = >; OSC/DCS
 *   werden geschluckt.
 *   CSI @ A-H J K L M P S T X d f m n r s u ` c h l (4, ?1 ?6 ?7 ?25
 *   ?47 ?1047 ?1049).
 * Zeichen kommen als UTF-8 und werden nach CP437 umgesetzt (sonst '?'),
 * DEC-Liniengrafik (ESC ( 0) auf die Rahmenzeichen von CP437.
 *
 * Vorher: vt_open() (Vollbild, -> Zeilen), danach vt_close().
 * vt_put(d, n) wertet aus, vt_flush() zeigt an; vt_key(KbdCharIn()) liefert
 * die zu sendenden Bytes in vt_kb/vt_kn. Antworten (DSR, DA) landen
 * ebenfalls in vt_kb (vt_kn > 0 nach vt_put).
 */
#define MK_FP(s, o) ((void __far *)(((unsigned long)(s) << 16) | (unsigned)(o)))

static unsigned vt_rows = 25, vt_sel;
static unsigned __far *vt_scr;
static unsigned vt_x, vt_y, vt_top, vt_bot, vt_wrapnx;
static unsigned char vt_fg = 7, vt_bg, vt_bold, vt_rev, vt_ckm, vt_awm = 1,
    vt_irm, vt_curon = 1, vt_om, vt_g[2], vt_gl;
static unsigned vt_sx, vt_sy;
static unsigned char vt_sfg, vt_sbg, vt_sbold, vt_srev, vt_sg0;
static unsigned vt_dlo = 0xFFFF, vt_dhi;
static unsigned char vt_st;             /* 0 Text, 1 ESC, 2 CSI, 3 OSC, 4 OSC-ESC,
                                           5 ESC ( / ), 6 DCS, 7 DCS-ESC, 8 ESC # */
static unsigned char vt_priv, vt_np, vt_gsel;
static unsigned vt_p[16];
static unsigned vt_uc, vt_un;           /* UTF-8 im Bau                   */
static unsigned char vt_kb[64];
static unsigned vt_kn;
static unsigned vt_alt[50 * 80];        /* Hauptbild beim Wechsel (?1049) */
static unsigned char vt_altd, vt_altx, vt_alty;

static const unsigned vt_437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0 };
static const unsigned char vt_dec[31] = {     /* ` a ... ~ in Liniengrafik */
    0x04, 0xB1, 0x62, 0x63, 0x64, 0x65, 0xF8, 0xF1, 0x68, 0x69, 0xD9, 0xBF,
    0xDA, 0xC0, 0xC5, 0xC4, 0xC4, 0xC4, 0xC4, 0x5F, 0xC3, 0xB4, 0xC1, 0xC2,
    0xB3, 0xF3, 0xF2, 0xE3, 0xD8, 0x9C, 0xFA };
static const unsigned char vt_col[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };  /* ANSI -> VGA */

static unsigned char vt_from_uni(unsigned u)
{
    unsigned i;
    if (u < 0x80)
        return (unsigned char)u;
    for (i = 0; i < 128; i++)
        if (vt_437[i] == u)
            return (unsigned char)(0x80 + i);
    if (u >= 0x2500 && u < 0x2580) {    /* runde/schwere Rahmen -> einfach */
        if (u == 0x256D) return 0xDA;
        if (u == 0x256E) return 0xBF;
        if (u == 0x256F) return 0xD9;
        if (u == 0x2570) return 0xC0;
        if (u == 0x2501) return 0xC4;
        if (u == 0x2503) return 0xB3;
    }
    if (u == 0x2022 || u == 0x25CF) return 0x07;
    if (u == 0x2026) return '.';
    if (u == 0x2018 || u == 0x2019) return '\'';
    if (u == 0x201C || u == 0x201D) return '"';
    if (u == 0x2013 || u == 0x2014) return '-';
    if (u == 0x00A9) return 'c';
    if (u == 0x2190) return 0x1B;
    if (u == 0x2192) return 0x1A;
    if (u == 0x2191) return 0x18;
    if (u == 0x2193) return 0x19;
    return '?';
}

static unsigned vt_attr(void)
{
    unsigned char f = vt_fg, b = vt_bg, t;
    if (vt_bold)
        f |= 8;
    if (vt_rev) {
        t = f;
        f = (unsigned char)(b | (f & 8));
        b = (unsigned char)(t & 7);
    }
    return (unsigned)((b & 7) << 4 | f) << 8;
}

static void vt_dirty(unsigned a, unsigned b)
{
    if (a < vt_dlo)
        vt_dlo = a;
    if (b > vt_dhi)
        vt_dhi = b;
}

static void vt_clr(unsigned y, unsigned x0, unsigned x1)   /* [x0, x1) */
{
    unsigned v = vt_attr() & 0x7000 | 0x0700 | ' ';
    if (vt_rev || vt_bg)
        v = (unsigned)((vt_rev ? vt_fg : vt_bg) & 7) << 12 | 0x0700 | ' ';
    while (x0 < x1)
        vt_scr[y * 80 + x0++] = v;
    vt_dirty(y, y);
}

/* Zeilen top..bot um n nach oben (n > 0) bzw. unten (n < 0) */
static void vt_scroll(unsigned top, unsigned bot, int n)
{
    unsigned y, x, k;
    if (n > 0) {
        k = (unsigned)n;
        if (k > bot - top + 1)
            k = bot - top + 1;
        for (y = top; y + k <= bot; y++)
            for (x = 0; x < 80; x++)
                vt_scr[y * 80 + x] = vt_scr[(y + k) * 80 + x];
        for (y = bot + 1 - k; y <= bot; y++)
            vt_clr(y, 0, 80);
    } else {
        k = (unsigned)-n;
        if (k > bot - top + 1)
            k = bot - top + 1;
        for (y = bot; y >= top + k; y--)
            for (x = 0; x < 80; x++)
                vt_scr[y * 80 + x] = vt_scr[(y - k) * 80 + x];
        for (y = top; y < top + k; y++)
            vt_clr(y, 0, 80);
    }
    vt_dirty(top, bot);
}

static void vt_lf(void)
{
    if (vt_y == vt_bot)
        vt_scroll(vt_top, vt_bot, 1);
    else if (vt_y < vt_rows - 1)
        vt_y++;
}

static void vt_char(unsigned char c)
{
    unsigned x;
    if (vt_wrapnx) {
        vt_wrapnx = 0;
        vt_x = 0;
        vt_lf();
    }
    if (vt_irm)
        for (x = 79; x > vt_x; x--)
            vt_scr[vt_y * 80 + x] = vt_scr[vt_y * 80 + x - 1];
    vt_scr[vt_y * 80 + vt_x] = vt_attr() | c;
    vt_dirty(vt_y, vt_y);
    if (vt_x < 79)
        vt_x++;
    else if (vt_awm)
        vt_wrapnx = 1;
}

static void vt_print(unsigned u)
{
    if (u >= 0x60 && u <= 0x7E && vt_g[vt_gl])
        vt_char(vt_dec[u - 0x60]);
    else
        vt_char(vt_from_uni(u));
}

static void vt_reply(const char *s)
{
    while (*s && vt_kn < sizeof(vt_kb))
        vt_kb[vt_kn++] = (unsigned char)*s++;
}

static void vt_num(unsigned v)
{
    char b[6];
    unsigned k = 5;
    b[5] = 0;
    do {
        b[--k] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    vt_reply(b + k);
}

static void vt_save(void)
{
    vt_sx = vt_x; vt_sy = vt_y;
    vt_sfg = vt_fg; vt_sbg = vt_bg; vt_sbold = vt_bold; vt_srev = vt_rev;
    vt_sg0 = vt_g[0];
}

static void vt_restore(void)
{
    vt_x = vt_sx; vt_y = vt_sy;
    vt_fg = vt_sfg; vt_bg = vt_sbg; vt_bold = vt_sbold; vt_rev = vt_srev;
    vt_g[0] = vt_sg0;
    vt_wrapnx = 0;
    if (vt_y >= vt_rows) vt_y = vt_rows - 1;
}

static void vt_reset(void)
{
    vt_fg = 7; vt_bg = vt_bold = vt_rev = 0;
    vt_top = 0; vt_bot = vt_rows - 1;
    vt_x = vt_y = vt_wrapnx = 0;
    vt_ckm = vt_irm = vt_om = 0; vt_awm = vt_curon = 1;
    vt_g[0] = vt_g[1] = vt_gl = 0;
    vt_save();
}

static void vt_altscr(int on)
{
    unsigned i, n = vt_rows * 80;
    if (on && !vt_altd) {
        for (i = 0; i < n; i++)
            vt_alt[i] = vt_scr[i];
        vt_altx = (unsigned char)vt_x;
        vt_alty = (unsigned char)vt_y;
        vt_altd = 1;
        for (i = 0; i < vt_rows; i++)
            vt_clr(i, 0, 80);
    } else if (!on && vt_altd) {
        for (i = 0; i < n; i++)
            vt_scr[i] = vt_alt[i];
        vt_x = vt_altx;
        vt_y = vt_alty;
        vt_altd = 0;
        vt_dirty(0, vt_rows - 1);
    }
}

static void vt_sgr(void)
{
    unsigned i, p;
    if (!vt_np) {
        vt_np = 1;
        vt_p[0] = 0;
    }
    for (i = 0; i < vt_np; i++) {
        p = vt_p[i];
        if (p == 0) { vt_fg = 7; vt_bg = 0; vt_bold = vt_rev = 0; }
        else if (p == 1) vt_bold = 1;
        else if (p == 22) vt_bold = 0;
        else if (p == 7) vt_rev = 1;
        else if (p == 27) vt_rev = 0;
        else if (p >= 30 && p <= 37) vt_fg = vt_col[p - 30];
        else if (p == 39) vt_fg = 7;
        else if (p >= 40 && p <= 47) vt_bg = vt_col[p - 40];
        else if (p == 49) vt_bg = 0;
        else if (p >= 90 && p <= 97) { vt_fg = vt_col[p - 90]; vt_bold = 1; }
        else if (p >= 100 && p <= 107) vt_bg = vt_col[p - 100];
        else if ((p == 38 || p == 48) && i + 2 < vt_np && vt_p[i + 1] == 5) {
            unsigned c = vt_p[i + 2];   /* 256 Farben: grob                */
            unsigned char v = (unsigned char)(c < 8 ? vt_col[c] : c < 16 ? vt_col[c - 8] : 7);
            if (p == 38) {
                vt_fg = v;
                if (c >= 8 && c < 16)
                    vt_bold = 1;
            } else
                vt_bg = v;
            i += 2;
        } else if ((p == 38 || p == 48) && i + 4 < vt_np && vt_p[i + 1] == 2)
            i += 4;                     /* Echtfarben: uebergangen        */
    }
}

static void vt_mode(int on)
{
    unsigned i, p;
    for (i = 0; i < vt_np; i++) {
        p = vt_p[i];
        if (!vt_priv) {
            if (p == 4)
                vt_irm = (unsigned char)on;
            continue;
        }
        if (p == 1) vt_ckm = (unsigned char)on;
        else if (p == 6) { vt_om = (unsigned char)on; vt_x = 0; vt_y = on ? vt_top : 0; }
        else if (p == 7) vt_awm = (unsigned char)on;
        else if (p == 25) vt_curon = (unsigned char)on;
        else if (p == 47 || p == 1047) vt_altscr(on);
        else if (p == 1049) {
            if (on) {
                vt_save();
                vt_altscr(1);
            } else {
                vt_altscr(0);
                vt_restore();
            }
        }
    }
}

static void vt_csi(unsigned char f)
{
    unsigned p0 = vt_np ? vt_p[0] : 0, n = p0 ? p0 : 1, x, y;
    if (vt_priv && f != 'h' && f != 'l')
        return;                         /* ?-Abfragen u. a. uebergehen    */
    switch (f) {
    case '@':                           /* Zeichen einfuegen              */
        for (x = 79; x >= vt_x + n && x < 80; x--)
            vt_scr[vt_y * 80 + x] = vt_scr[vt_y * 80 + x - n];
        vt_clr(vt_y, vt_x, vt_x + n < 80 ? vt_x + n : 80);
        break;
    case 'A':
        vt_y = vt_y >= vt_top + n ? vt_y - n : vt_y >= vt_top ? vt_top : (vt_y >= n ? vt_y - n : 0);
        break;
    case 'B':
        vt_y = vt_y + n <= vt_bot ? vt_y + n : vt_y <= vt_bot ? vt_bot : (vt_y + n < vt_rows ? vt_y + n : vt_rows - 1);
        break;
    case 'C': case 'a':
        vt_x = vt_x + n < 80 ? vt_x + n : 79;
        break;
    case 'D':
        vt_x = vt_x >= n ? vt_x - n : 0;
        break;
    case 'E':
        vt_y = vt_y + n < vt_rows ? vt_y + n : vt_rows - 1;
        vt_x = 0;
        break;
    case 'F':
        vt_y = vt_y >= n ? vt_y - n : 0;
        vt_x = 0;
        break;
    case 'G': case '`':
        vt_x = n <= 80 ? n - 1 : 79;
        break;
    case 'H': case 'f':
        y = n - 1 + (vt_om ? vt_top : 0);
        x = vt_np > 1 && vt_p[1] ? vt_p[1] - 1 : 0;
        vt_y = y < vt_rows ? y : vt_rows - 1;
        vt_x = x < 80 ? x : 79;
        break;
    case 'd':
        vt_y = n <= vt_rows ? n - 1 : vt_rows - 1;
        break;
    case 'J':
        if (p0 == 0) {
            vt_clr(vt_y, vt_x, 80);
            for (y = vt_y + 1; y < vt_rows; y++)
                vt_clr(y, 0, 80);
        } else if (p0 == 1) {
            for (y = 0; y < vt_y; y++)
                vt_clr(y, 0, 80);
            vt_clr(vt_y, 0, vt_x + 1);
        } else
            for (y = 0; y < vt_rows; y++)
                vt_clr(y, 0, 80);
        break;
    case 'K':
        if (p0 == 0)
            vt_clr(vt_y, vt_x, 80);
        else if (p0 == 1)
            vt_clr(vt_y, 0, vt_x + 1);
        else
            vt_clr(vt_y, 0, 80);
        break;
    case 'L':
        if (vt_y >= vt_top && vt_y <= vt_bot)
            vt_scroll(vt_y, vt_bot, -(int)n);
        break;
    case 'M':
        if (vt_y >= vt_top && vt_y <= vt_bot)
            vt_scroll(vt_y, vt_bot, (int)n);
        break;
    case 'P':                           /* Zeichen loeschen               */
        for (x = vt_x; x < 80; x++)
            vt_scr[vt_y * 80 + x] = x + n < 80 ? vt_scr[vt_y * 80 + x + n] : (vt_attr() & 0x7000) | 0x0720;
        vt_dirty(vt_y, vt_y);
        break;
    case 'X':
        vt_clr(vt_y, vt_x, vt_x + n < 80 ? vt_x + n : 80);
        break;
    case 'S':
        vt_scroll(vt_top, vt_bot, (int)n);
        break;
    case 'T':
        vt_scroll(vt_top, vt_bot, -(int)n);
        break;
    case 'm':
        vt_sgr();
        return;
    case 'r':
        x = p0 ? p0 - 1 : 0;
        y = vt_np > 1 && vt_p[1] ? vt_p[1] - 1 : vt_rows - 1;
        if (y >= vt_rows)
            y = vt_rows - 1;
        if (x < y) {
            vt_top = x;
            vt_bot = y;
            vt_x = 0;
            vt_y = vt_om ? vt_top : 0;
        }
        break;
    case 's':
        vt_save();
        break;
    case 'u':
        vt_restore();
        break;
    case 'h':
        vt_mode(1);
        break;
    case 'l':
        vt_mode(0);
        break;
    case 'n':
        if (p0 == 6) {
            vt_reply("\033[");
            vt_num(vt_y + 1 - (vt_om ? vt_top : 0));
            vt_reply(";");
            vt_num(vt_x + 1);
            vt_reply("R");
        } else if (p0 == 5)
            vt_reply("\033[0n");
        break;
    case 'c':
        if (!p0)
            vt_reply("\033[?1;2c");
        break;
    }
    vt_wrapnx = 0;
}

static void vt_esc(unsigned char c)
{
    vt_st = 0;
    switch (c) {
    case '[': vt_st = 2; vt_np = 0; vt_priv = 0; vt_p[0] = 0; return;
    case ']': vt_st = 3; return;
    case 'P': case '_': case '^': vt_st = 6; return;
    case '(': vt_gsel = 0; vt_st = 5; return;
    case ')': vt_gsel = 1; vt_st = 5; return;
    case '#': vt_st = 8; return;
    case '7': vt_save(); return;
    case '8': vt_restore(); return;
    case 'D': vt_lf(); return;
    case 'E': vt_x = 0; vt_lf(); return;
    case 'M':
        if (vt_y == vt_top)
            vt_scroll(vt_top, vt_bot, -1);
        else if (vt_y)
            vt_y--;
        return;
    case 'c': {
        unsigned y;
        vt_reset();
        for (y = 0; y < vt_rows; y++)
            vt_clr(y, 0, 80);
        return;
    }
    }
}

static void vt_byte(unsigned char c)
{
    switch (vt_st) {
    case 1:
        vt_esc(c);
        return;
    case 2:                             /* CSI                            */
        if (c >= '0' && c <= '9') {
            if (!vt_np)
                vt_np = 1;
            vt_p[vt_np - 1] = vt_p[vt_np - 1] * 10 + (c - '0');
        } else if (c == ';' || c == ':') {
            if (!vt_np)
                vt_np = 1;
            if (vt_np < 16)
                vt_p[vt_np++] = 0;
        } else if (c == '?' || c == '>' || c == '=' || c == '<')
            vt_priv = c;
        else if (c >= 0x40 && c <= 0x7E) {
            vt_st = 0;
            vt_csi(c);
        } else if (c == 27)
            vt_st = 1;
        else if (c == 24 || c == 26)
            vt_st = 0;
        return;
    case 3:                             /* OSC bis BEL oder ESC \         */
        if (c == 7)
            vt_st = 0;
        else if (c == 27)
            vt_st = 4;
        return;
    case 4:
        vt_st = c == '\\' ? 0 : 3;
        return;
    case 5:
        vt_g[vt_gsel] = (unsigned char)(c == '0');
        vt_st = 0;
        return;
    case 6:
        if (c == 27)
            vt_st = 7;
        return;
    case 7:
        vt_st = c == '\\' ? 0 : 6;
        return;
    case 8:
        vt_st = 0;
        return;
    }
    /* Text */
    if (vt_un) {                        /* UTF-8 fortsetzen               */
        if ((c & 0xC0) == 0x80) {
            vt_uc = vt_uc << 6 | (c & 0x3F);
            if (!--vt_un)
                vt_print(vt_uc);
            return;
        }
        vt_un = 0;
        vt_print('?');
    }
    if (c >= 0xC2 && c < 0xE0) { vt_uc = c & 0x1F; vt_un = 1; return; }
    if (c >= 0xE0 && c < 0xF0) { vt_uc = c & 0x0F; vt_un = 2; return; }
    if (c >= 0xF0 && c < 0xF5) { vt_uc = 0xFFFF; vt_un = 3; return; }  /* ausserhalb BMP */
    if (c >= 0x80) { vt_print('?'); return; }
    if (c >= ' ' && c != 0x7F) {
        vt_print(c);
        return;
    }
    switch (c) {
    case 7: break;
    case 8:
        if (vt_x)
            vt_x--;
        vt_wrapnx = 0;
        break;
    case 9:
        vt_x = (vt_x + 8) & ~7;
        if (vt_x > 79)
            vt_x = 79;
        break;
    case 10: case 11: case 12:
        vt_lf();
        vt_wrapnx = 0;
        break;
    case 13:
        vt_x = 0;
        vt_wrapnx = 0;
        break;
    case 14: vt_gl = 1; break;
    case 15: vt_gl = 0; break;
    case 27: vt_st = 1; break;
    }
}

static void vt_put(const unsigned char *d, unsigned n)
{
    while (n--)
        vt_byte(*d++);
}

static void vt_flush(void)
{
    if (vt_dlo != 0xFFFF) {
        VioShowBuf(vt_dlo, vt_dhi);
        vt_dlo = 0xFFFF;
        vt_dhi = 0;
    }
    if (vt_curon)
        VioSetCurPos(vt_y, vt_x);
    else
        VioSetCurPos(0xFF, 0);
}

/* -> 0 ok */
static int vt_open(void)
{
    unsigned y;
    vt_rows = VioSetMode(1);
    if (FAPI_FAILED(vt_rows) || vt_rows < 25 || vt_rows > 50)
        vt_rows = 25;
    vt_sel = VioGetBuf();
    if (FAPI_FAILED(vt_sel)) {
        VioSetMode(0);
        return -1;
    }
    vt_scr = (unsigned __far *)MK_FP(vt_sel, 0);
    vt_reset();
    for (y = 0; y < vt_rows; y++)
        vt_clr(y, 0, 80);
    vt_flush();
    return 0;
}

static void vt_close(void)
{
    VioSetMode(0);
}

/* Taste -> Bytes fuer die Gegenseite in vt_kb/vt_kn (UTF-8) */
static void vt_key(unsigned k)
{
    unsigned char ch = (unsigned char)k, sc = (unsigned char)(k >> 8);
    unsigned u;
    static const unsigned char fsc[] = { 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x85, 0x86 };
    static const char *const fsq[] = { "15", "17", "18", "19", "20", "21", "23", "24" };
    unsigned i;
    vt_kn = 0;
    if (KEY_EXT(ch, sc)) {
        static const unsigned char cs[] = { 0x48, 0x50, 0x4D, 0x4B, 0x47, 0x4F };
        static const char cq[] = "ABCDHF";
        for (i = 0; i < sizeof(cs); i++)
            if (sc == cs[i]) {
                vt_kb[0] = 27;
                vt_kb[1] = vt_ckm ? 'O' : '[';
                vt_kb[2] = (unsigned char)cq[i];
                vt_kn = 3;
                return;
            }
        if (sc == 0x52 || sc == 0x53 || sc == 0x49 || sc == 0x51) {
            vt_reply("\033[");
            vt_kb[vt_kn++] = sc == 0x52 ? '2' : sc == 0x53 ? '3' : sc == 0x49 ? '5' : '6';
            vt_kb[vt_kn++] = '~';
            return;
        }
        if (sc >= 0x3B && sc <= 0x3E) {
            vt_kb[0] = 27;
            vt_kb[1] = 'O';
            vt_kb[2] = (unsigned char)('P' + sc - 0x3B);
            vt_kn = 3;
            return;
        }
        for (i = 0; i < sizeof(fsc); i++)
            if (sc == fsc[i]) {
                vt_reply("\033[");
                vt_reply(fsq[i]);
                vt_reply("~");
                return;
            }
        if (sc == 0x0F) {               /* Umschalt+Tab                   */
            vt_reply("\033[Z");
            return;
        }
        return;
    }
    if (ch == '\b')
        ch = 0x7F;
    if (ch < 0x80) {
        vt_kb[0] = ch;
        vt_kn = 1;
        return;
    }
    u = vt_437[ch - 0x80];
    if (u < 0x800) {
        vt_kb[0] = (unsigned char)(0xC0 | u >> 6);
        vt_kb[1] = (unsigned char)(0x80 | (u & 0x3F));
        vt_kn = 2;
    } else {
        vt_kb[0] = (unsigned char)(0xE0 | u >> 12);
        vt_kb[1] = (unsigned char)(0x80 | ((u >> 6) & 0x3F));
        vt_kb[2] = (unsigned char)(0x80 | (u & 0x3F));
        vt_kn = 3;
    }
}
