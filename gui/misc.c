/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - kleine Module: KEYBOARD (ANSI <-> OEM), SHELL (Ziehen und
 * Ablegen, Info-Fenster) und die Druck-Teile von GDI, solange es keine
 * Druckertreiber gibt (CreateDC fuer Drucker scheitert, Escape meldet 0).
 */
#include "gui.h"

extern const BYTE a2o[128];

static BYTE oem2ansi(BYTE c)
{
    WORD i;
    if (c < 0x80)
        return c;
    for (i = 0; i < 128; i++)
        if (a2o[i] == c)
            return (BYTE)(0x80 + i);
    return '_';
}
BYTE ansi2oem(BYTE c) { return c < 0x80 ? c : a2o[c - 0x80]; }

/* ---- KEYBOARD ---- */
short API AnsiToOem(LPCSTR a, LPSTR o)
{
    while (*a)
        *o++ = ansi2oem(*a++);
    *o = 0;
    return -1;
}
BOOL API OemToAnsi(LPCSTR o, LPSTR a)
{
    while (*o)
        *a++ = oem2ansi(*o++);
    *a = 0;
    return -1;
}
void API AnsiToOemBuff(LPCSTR a, LPSTR o, WORD n)
{
    while (n--)
        *o++ = ansi2oem(*a++);
}
void API OemToAnsiBuff(LPCSTR o, LPSTR a, WORD n)
{
    while (n--)
        *a++ = oem2ansi(*o++);
}

/* ---- SHELL ---- */
void API DragAcceptFiles(HWND h, BOOL on) { (void)h; (void)on; }
WORD API DragQueryFile(WORD hd, WORD i, LPSTR buf, WORD n)
{
    (void)hd; (void)i; (void)n;
    if (buf)
        buf[0] = 0;
    return 0;
}
void API DragFinish(WORD hd) { (void)hd; }

static char about[160];
short API ShellAbout(HWND h, LPCSTR app, LPCSTR other, WORD icon)
{
    WORD k = 0, n;
    (void)icon;
    if (app) {
        for (n = 0; app[n] && app[n] != '#' && k < 60; n++)
            about[k++] = app[n];
    }
    fstrncpy(about + k, "\nAT-DOS GUI (Win16-Schicht)\n", sizeof(about) - k);
    k = fstrlen(about);
    if (other)
        fstrncpy(about + k, other, sizeof(about) - k);
    MessageBox(h, about, "Info", 0);
    return 1;
}

/* ---- GDI: Drucker ---- */
HDC API CreateDC(LPCSTR drv, LPCSTR dev, LPCSTR port, LPVOID init)
{
    (void)dev; (void)port; (void)init;
    if (drv && !fstricmp(drv, "DISPLAY"))
        return dc_get(0, 0, 0);
    if (drv)
        glogs("CreateDC (kein Druckertreiber): ", drv);
    return 0;
}
short API Escape(HDC h, short esc, short n, LPCSTR in, LPVOID out)
{
    (void)h; (void)esc; (void)n; (void)in; (void)out;
    return 0;
}

/* ---- USER: wsprintf (cdecl, Argumente auf dem Stapel des Programms) ---- */
static WORD fmtv(LPSTR out, LPCSTR f, const BYTE FAR *ap)
{
    static char num[16];
    WORD o = 0;
    while (*f && o < 1023) {
        BYTE left = 0, zero = 0, lng = 0, c;
        short wd = 0, prec = -1, n = 0, i;
        LPCSTR s;
        if (*f != '%') {
            out[o++] = *f++;
            continue;
        }
        f++;
        if (*f == '-') { left = 1; f++; }
        if (*f == '#') f++;
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') wd = wd * 10 + (*f++ - '0');
        if (*f == '.') {
            prec = 0;
            for (f++; *f >= '0' && *f <= '9'; f++) prec = prec * 10 + (*f - '0');
        }
        if (*f == 'l' || *f == 'L') { lng = 1; f++; }
        c = *f;
        if (!c)                                 /* '%' am Ende           */
            break;
        f++;
        s = num;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': {
            DWORD v;
            BYTE neg = 0, base = (c == 'x' || c == 'X') ? 16 : 10;
            if (lng) { v = *(const DWORD FAR *)ap; ap += 4; }
            else { v = *(const WORD FAR *)ap; ap += 2; if (c == 'd' || c == 'i') v = (DWORD)(long)(short)v; }
            if ((c == 'd' || c == 'i') && (long)v < 0) { neg = 1; v = (DWORD)-(long)v; }
            i = sizeof(num) - 1;
            num[i] = 0;
            do {
                BYTE d = (BYTE)(v % base);
                num[--i] = (char)(d < 10 ? '0' + d : (c == 'x' ? 'a' : 'A') + d - 10);
                v /= base;
            } while (v && i > 1);
            if (neg) num[--i] = '-';
            s = num + i;
            n = fstrlen(s);
            break;
        }
        case 'c':
            num[0] = (char)*(const WORD FAR *)ap; ap += 2;
            num[1] = 0;
            n = 1;
            break;
        case 's':
            s = *(LPCSTR const FAR *)ap; ap += 4;
            if (!s) s = "(null)";
            n = fstrlen(s);
            if (prec >= 0 && n > prec) n = prec;
            break;
        case '%':
            num[0] = '%'; num[1] = 0; n = 1;
            break;
        default:
            continue;
        }
        if (!left)
            for (i = n; i < wd && o < 1023; i++)
                out[o++] = (zero && c != 's') ? '0' : ' ';
        for (i = 0; i < n && o < 1023; i++)
            out[o++] = s[i];
        if (left)
            for (i = n; i < wd && o < 1023; i++)
                out[o++] = ' ';
    }
    out[o] = 0;
    return o;
}
short APIC _wsprintf(LPSTR out, LPCSTR fmt, ...)
{
    short r = fmtv(out, fmt, (const BYTE FAR *)&fmt + sizeof(fmt));
    if (w16_trace == 2) {                       /* Vollspur: Text dazu   */
        glogs("wsprintf ", fmt);
        glogs("  -> ", out);
    }
    return r;
}
short API wvsprintf(LPSTR out, LPCSTR fmt, const BYTE FAR *args)
{
    return fmtv(out, fmt, args);
}

/* ---- USER: registrierte Nachrichten (C000h..) ---- */
#define NRMSG 16
static char rmsg[NRMSG][32];
WORD API RegisterWindowMessage(LPCSTR name)
{
    short i;
    for (i = 0; i < NRMSG && rmsg[i][0]; i++)
        if (!fstricmp(rmsg[i], name))
            return 0xC100 + i;
    if (i == NRMSG)
        return 0;
    fstrncpy(rmsg[i], name, 32);
    return 0xC100 + i;
}

BOOL API WinHelp(HWND h, LPCSTR file, WORD cmd, DWORD data)
{
    (void)data;
    if (cmd == 2)                               /* HELP_QUIT             */
        return 1;
    glogs("WinHelp: ", file ? file : "");
    MessageBox(h, "Die Hilfe (WINHELP) gibt es unter AT-DOS noch nicht.", "Hilfe", 0);
    return 1;
}

/* Text mit Tabulatoren (USER): Stopps aus der Tabelle oder alle 8 Zeichen */
DWORD API TabbedTextOut(HDC dc, short x, short y, LPCSTR s, short n, short nt,
                        short FAR *tabs, short org)
{
    short i, a = 0, px = x;
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '\t') {
            if (i > a)
                ExtTextOut(dc, px, y, 0, 0, s + a, i - a, 0);
            px += (i - a) * FONT_W;
            if (i < n) {
                short k, st = 0, rel = px - org;
                for (k = 0; k < nt && tabs; k++)
                    if (tabs[k] > rel) { st = tabs[k]; break; }
                if (!st) {
                    short tw = (nt == 1 && tabs) ? tabs[0] : 8 * FONT_W;
                    if (tw <= 0) tw = 8 * FONT_W;
                    st = (rel / tw + 1) * tw;
                }
                px = org + st;
            }
            a = i + 1;
        }
    }
    return MAKELONG(px - x, FONT_H);
}
DWORD API GetTabbedTextExtent(HDC dc, LPCSTR s, short n, short nt, short FAR *tabs)
{
    short i, px = 0;
    (void)dc; (void)nt; (void)tabs;
    for (i = 0; i < n; i++)
        px = s[i] == '\t' ? (px / (8 * FONT_W) + 1) * 8 * FONT_W : px + FONT_W;
    return MAKELONG(px, FONT_H);
}

/* KEYBOARD.132: Codepage der Tastatur = aktive Codepage des Systems */
short API GetKBCodePage(void)
{
    WORD cp = fapi_call(FAPI_CODEPAGE, 0, 0, 0);
    return FAPI_FAILED(cp) || !cp ? 437 : (short)cp;
}

/* Drucken gibt es noch nicht: Seiten melden Fehler (SP_ERROR) */
short API StartPage(HDC h) { (void)h; return -1; }
short API EndPage(HDC h) { (void)h; return -1; }

/* ---- fuer Write & Co. ---- */
WORD atom_add(LPCSTR s);
WORD API RegisterClipboardFormat(LPCSTR name) { return atom_add(name); }
HDC API CreateIC(LPCSTR drv, LPCSTR dev, LPCSTR port, LPVOID init)
{
    return CreateDC(drv, dev, port, init);
}
/* Taste zu einem Zeichen (deutsche Belegung grob): Hochbyte 1 = Umschalt */
WORD API VkKeyScan(WORD ch)
{
    BYTE c = (BYTE)ch;
    if (c >= 'a' && c <= 'z')
        return c - 32;
    if (c >= 'A' && c <= 'Z')
        return 0x100 | c;
    if (c >= '0' && c <= '9' || c == ' ' || c == 8 || c == 9 || c == 13 || c == 27)
        return c;
    return 0xFFFF;
}

/* Schriften wie die Rasterschriften von Windows 3.1 (gezeichnet wird mit
   der Systemschrift; Write & Co. brauchen proportionale Familien):
   Name, Familie|Abstand (lfPitchAndFamily), Zeichensatz */
static const struct { const char *n; BYTE pf, cs; } fonts[] = {
    { "System",        0x22, 0 },               /* FF_SWISS, variabel   */
    { "MS Sans Serif", 0x22, 0 },
    { "MS Serif",      0x12, 0 },               /* FF_ROMAN             */
    { "Courier",       0x31, 0 },               /* FF_MODERN, fest      */
    { "Small Fonts",   0x22, 0 },
    { "Symbol",        0x52, 2 },               /* FF_DECORATIVE        */
    { "Terminal",      0x31, 0xFF },            /* OEM                  */
};
static BYTE ef_lf[50], ef_tm[31];
static WORD ef_w[7];
void tm_fill(BYTE FAR *tm);
short API EnumFonts(HDC h, LPCSTR face, DWORD proc, DWORD data)
{
    short r = 1;
    WORD i;
    (void)h;
    if (!proc)
        return 0;
    for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]) && r; i++) {
        if (face && fstricmp(face, fonts[i].n))
            continue;
        fmemset((void FAR *)ef_lf, 0, sizeof(ef_lf));
        ((short *)ef_lf)[0] = FONT_H;           /* lfHeight             */
        ((short *)ef_lf)[1] = FONT_W;           /* lfWidth              */
        ((short *)ef_lf)[4] = 400;              /* lfWeight             */
        ef_lf[13] = fonts[i].cs;                /* lfCharSet            */
        ef_lf[17] = fonts[i].pf;                /* lfPitchAndFamily     */
        fstrncpy((LPSTR)(ef_lf + 18), fonts[i].n, 32);
        tm_fill(ef_tm);
        ef_tm[23] = (BYTE)((fonts[i].pf & 0xF0) | ((fonts[i].pf & 3) == 2));  /* Bit 0 = variabel */
        ef_tm[24] = fonts[i].cs;
        ef_w[0] = SELOF((void FAR *)ef_lf); ef_w[1] = OFFOF((void FAR *)ef_lf);
        ef_w[2] = SELOF((void FAR *)ef_tm); ef_w[3] = OFFOF((void FAR *)ef_tm);
        ef_w[4] = 1;                            /* RASTER_FONTTYPE      */
        ef_w[5] = HIWORD(data); ef_w[6] = LOWORD(data);
        r = (short)call16((void FAR *)proc,
                          cur_task >= 0 ? wt[cur_task].hinst : 0, 7, ef_w);
    }
    return r;
}
