/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FAPIDOS - FAPI unter MS-DOS (Real Mode), Teil des gebundenen Stubs.
 *
 * Wie die Family API von OS/2: ein FAPI-Programm, das mit `mkfx.py --bind`
 * gebaut ist, traegt diese Uebersetzungsschicht als MZ-Stub mit sich und
 * laeuft so auch unter MS-DOS 3.1 und neuer (auf einem 286 oder besser).
 * Jeder FAPI-Aufruf kommt ueber stub/scrt.asm (gate) hier an und wird in
 * DOS-, BIOS- und Maustreiber-Aufrufe uebersetzt. Zeiger der Programme
 * sind Offsets in seinem Datensegment app_ds.
 *
 * Was DOS nicht kann, meldet FAPI_ERROR: Sitzungen, Pipes, Laufwerke als
 * Ganzes (CHKDSK/FORMAT/FDISK), SUBST/JOIN, Tastaturbelegungen, lange
 * Namen (DOS kennt nur 8.3). Hintergrund-Programme laufen nacheinander.
 */
#include <i86.h>
#include "../include/fapi_abi.h"
#include "../include/fx.h"


struct regs {
    unsigned ax, bx, cx, dx, si, di, ds, es, flags, bp;
};
#define CF 0x0001
#define ZF 0x0040

/* scrt.asm */
extern unsigned psp_seg, app_ds, sig_tramp, sig_num, crit_err, fx_rel;
extern unsigned char sig_go, in_sig, brk_flag, crit_hit;
extern void gate_entry(void);
extern void int23(void);
extern void int24(void);
void do_int(unsigned char n, struct regs *r);
unsigned long ulmul(unsigned a, unsigned b);
unsigned get_cs(void);
unsigned cpu_type(void);
void run_app(unsigned cs, unsigned ds, unsigned sp, unsigned ip);

static struct regs r;
static unsigned sig_mask;

#define BIOS(off)   (*(unsigned char __far *)MK_FP(0x40, (off)))
#define BIOSW(off)  (*(unsigned __far *)MK_FP(0x40, (off)))
#define AP(off)     ((char __far *)MK_FP(app_ds, (off)))

/* ---- kleine Hilfen ------------------------------------------------------ */
static void rclr(void)
{
    r.ax = r.bx = r.cx = r.dx = r.si = r.di = r.flags = 0;
    r.ds = r.es = get_cs();
}

static void dos(void)
{
    crit_hit = 0;
    do_int(0x21, &r);
}

static void out(const char *s)          /* Meldung des Stubs (Handle 2) */
{
    unsigned n = 0;
    while (s[n])
        n++;
    rclr();
    r.ax = 0x4000;
    r.bx = 2;
    r.cx = n;
    r.dx = (unsigned)s;
    dos();
}

static unsigned up(unsigned c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

/* Zeichenkette aus dem Programm holen */
static void from_app(char *d, unsigned off, unsigned max)
{
    char __far *s = AP(off);
    unsigned i;
    for (i = 0; i < max - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

static void to_app(unsigned off, const void *src, unsigned n)
{
    char __far *d = AP(off);
    const char *s = src;
    while (n--)
        *d++ = *s++;
}

static void far_zero(unsigned seg, unsigned off, unsigned n)
{
    char __far *p = MK_FP(seg, off);
    while (n--)
        *p++ = 0;
}

/* DOS-Fehler (AX nach CF) -> FAPI */
static unsigned err(void)
{
    unsigned e = r.ax;
    if (crit_hit || e == 0x53) {        /* INT 24h: Fehler gemeldet    */
        crit_hit = 0;
        return (crit_err & 0xFF) == 0 ? FAPI_ERR_WP : FAPI_ERR_NOTREADY;
    }
    switch (e) {
    case 5:    return FAPI_ERR_DENIED;
    case 0x13: return FAPI_ERR_WP;
    case 0x15: return FAPI_ERR_NOTREADY;
    case 0x20: case 0x21: return FAPI_ERR_INUSE;
    case 0x50: return FAPI_ERR_EXIST;
    case 0x27: return FAPI_ERR_FULL;
    }
    return FAPI_ERROR;
}

/* DOS-Aufruf mit DS:DX = Zeichenkette des Programms */
static unsigned dos_path(unsigned ax, unsigned off, unsigned cx)
{
    rclr();
    r.ax = ax;
    r.cx = cx;
    r.ds = app_ds;
    r.dx = off;
    dos();
    return (r.flags & CF) ? err() : 0;
}

static void yield(void)
{
    rclr();
    r.ax = 0x1680;                      /* Zeitscheibe abgeben (Windows,
                                           OS/2-DOS-Box), sonst wirkungslos */
    do_int(0x2F, &r);
}

static unsigned ticks(void)
{
    return BIOSW(0x6C);
}

/* ---- Bildschirm --------------------------------------------------------- */
static unsigned vseg, vcols, vrows, cur_shape, cur_hidden;
static unsigned save_seg, save_cur;     /* VioSetMode(1): alter Inhalt  */
static unsigned lvb_seg;                /* VioGetBuf                    */
static unsigned gfx_mode, text_rows;
static unsigned char vga;

static void video_init(void)
{
    vseg = BIOS(0x49) == 7 ? 0xB000 : 0xB800;
    vcols = BIOS(0x4A);
    if (vcols == 0 || vcols > 132)
        vcols = 80;
    vrows = BIOS(0x84) ? BIOS(0x84) + 1u : 25u;
    if (vrows < 25 || vrows > 50)
        vrows = 25;
}

static void video(void)
{
    do_int(0x10, &r);
}

static void cursor_pos(unsigned row, unsigned col)
{
    rclr();
    r.ax = 0x0200;
    r.dx = (row << 8) | col;
    video();
}

static void cursor_show(int on)
{
    if (on == !cur_hidden)
        return;
    rclr();
    r.ax = 0x0100;
    r.cx = on ? cur_shape : 0x2000;
    video();
    cur_hidden = !on;
}

static void wrt_str(unsigned soff, unsigned pos, unsigned attr)
{
    char __far *s = AP(soff);
    unsigned row = pos >> 8, col = pos & 0xFF;
    unsigned __far *v;
    if (row >= vrows || col >= vcols)
        return;
    v = MK_FP(vseg, (row * vcols + col) * 2);
    while (*s && col < vcols) {
        *v++ = (unsigned char)*s++ | (attr << 8);
        col++;
    }
}

static void tty_char(unsigned char c)
{
    rclr();
    r.ax = 0x0E00 | c;
    r.bx = 0x0007;
    video();
}

static void tty(unsigned off, unsigned n)
{
    char __far *s = AP(off);
    unsigned i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
            tty_char('\r');
        tty_char((unsigned char)s[i]);
    }
}

static void cls(void)
{
    rclr();
    r.ax = 0x0600;
    r.bx = 0x0700;
    r.dx = ((vrows - 1) << 8) | (vcols - 1);
    video();
    cursor_pos(0, 0);
}

static unsigned dos_alloc(unsigned paras)
{
    rclr();
    r.ax = 0x4800;
    r.bx = paras;
    dos();
    return (r.flags & CF) ? 0 : r.ax;
}

static void dos_free(unsigned seg)
{
    rclr();
    r.ax = 0x4900;
    r.es = seg;
    dos();
}

static void screen_copy(unsigned dseg, unsigned sseg, unsigned words)
{
    unsigned __far *d = MK_FP(dseg, 0);
    unsigned __far *s = MK_FP(sseg, 0);
    while (words--)
        *d++ = *s++;
}

static void set_mode(unsigned full)
{
    if (full && !save_seg) {
        save_seg = dos_alloc((vrows * vcols * 2 + 15) >> 4);
        if (save_seg) {
            screen_copy(save_seg, vseg, vrows * vcols);
            rclr();
            r.ax = 0x0300;
            video();
            save_cur = r.dx;
        }
    } else if (!full && save_seg) {
        screen_copy(vseg, save_seg, vrows * vcols);
        dos_free(save_seg);
        save_seg = 0;
        cursor_pos(save_cur >> 8, save_cur & 0xFF);
        cursor_show(1);
    }
}

static unsigned set_lines(unsigned n)
{
    if (n == 0 || n == vrows)
        return vrows;
    if (!vga || (n != 25 && n != 43 && n != 50))
        return FAPI_ERROR;
    if (n == 43) {                      /* 350 Rasterzeilen             */
        rclr();
        r.ax = 0x1201;
        r.bx = 0x0030;
        video();
    } else {                            /* 400 Rasterzeilen             */
        rclr();
        r.ax = 0x1202;
        r.bx = 0x0030;
        video();
    }
    rclr();
    r.ax = 0x0003;
    video();
    if (n != 25) {
        rclr();
        r.ax = 0x1112;                  /* 8x8-Zeichensatz              */
        video();
    }
    video_init();
    cur_hidden = 0;
    return vrows;
}

/* VGA-Register (unter DOS gibt es nur den Vordergrund) */
void outb(unsigned port, unsigned char v);
#pragma aux outb = "out dx, al" __parm [__dx] [__al] __modify __exact [];
unsigned char inb(unsigned port);
#pragma aux inb = "in al, dx" __parm [__dx] __value [__al] __modify __exact [__al];

static void crtc(unsigned char idx, unsigned char v)
{
    outb(0x3D4, idx);
    outb(0x3D5, v);
}

static void set_gfx(unsigned m)
{
    if (m == 0) {
        if (!gfx_mode)
            return;
        rclr();
        r.ax = 0x0003;
        video();
        gfx_mode = 0;
        video_init();
        if (text_rows != 25)
            set_lines(text_rows);
        return;
    }
    if (!gfx_mode)
        text_rows = vrows;
    rclr();
    r.ax = m == VIO_MODEX ? 0x0013 : m;
    video();
    if (m == VIO_MODEX) {               /* 320x240, vier Ebenen, 2 Seiten */
        unsigned i;
        unsigned char __far *p = MK_FP(0xA000, 0);
        outb(0x3C4, 0x04); outb(0x3C5, 0x06);   /* Chain-4 aus          */
        outb(0x3C2, 0xE3);                      /* 480 Rasterzeilen     */
        crtc(0x11, 0x2C);                       /* Schreibschutz weg    */
        crtc(0x06, 0x0D); crtc(0x07, 0x3E);
        crtc(0x10, 0xEA); crtc(0x11, 0xAC); crtc(0x12, 0xDF);
        crtc(0x14, 0x00); crtc(0x15, 0xE7); crtc(0x16, 0x06);
        crtc(0x17, 0xE3);                       /* Bytebetrieb          */
        outb(0x3C4, 0x02); outb(0x3C5, 0x0F);   /* alle Ebenen loeschen */
        for (i = 0; i < 0xFFFF; i++)
            p[i] = 0;
        p[0xFFFF] = 0;
    }
    gfx_mode = m;
}

/* ---- Tastatur ----------------------------------------------------------- */
static unsigned char kbd_enh;

static void exit_now(unsigned rc);

/* Strg+C / Strg+Untbr: Signal an das Programm oder Ende mit 255 (wie der
   Kernel) */
static int brk(void)
{
    brk_flag = 0;
    if (sig_tramp && (sig_mask & SIG_BREAK) && !in_sig) {
        sig_go = 1;
        sig_num = SIG_BREAK;
        return 1;
    }
    exit_now(255);
    return 1;
}

static unsigned kbd_norm(unsigned k)
{
    if ((k & 0xFF) == 0xE0 && (k >> 8))
        k &= 0xFF00;                    /* graue Tasten wie die anderen */
    return k;
}

static int is_break(unsigned k)
{
    return k == 0x2E03 || k == 0x0000;  /* Strg+C, Strg+Untbr (BIOS: 0) */
}

static unsigned kbd_peek(void)          /* 0 = keine Taste              */
{
    rclr();
    r.ax = kbd_enh ? 0x1100 : 0x0100;
    do_int(0x16, &r);
    if (r.flags & ZF)
        return 0xFFFF;
    return kbd_norm(r.ax);
}

static unsigned kbd_get(void)
{
    rclr();
    r.ax = kbd_enh ? 0x1000 : 0x0000;
    do_int(0x16, &r);
    return kbd_norm(r.ax);
}

static unsigned kbd_charin(void)
{
    unsigned k;
    for (;;) {
        if (brk_flag && brk())
            return 0;
        if (kbd_peek() != 0xFFFF) {
            k = kbd_get();
            if (is_break(k)) {
                brk();
                return 0;
            }
            return k;
        }
        yield();
    }
}

static unsigned kbd_shift(void)
{
    unsigned s = BIOS(0x17), f = 0;
    if (s & 3) f |= KBDSHIFT_SHIFT;
    if (s & 4) f |= KBDSHIFT_CTRL;
    if (BIOS(0x18) & 2) f |= KBDSHIFT_ALT;  /* linke Alt-Taste (Erweiterte) */
    else if ((s & 8) && !(BIOS(0x96) & 8)) f |= KBDSHIFT_ALT;
    if (BIOS(0x96) & 8) f |= KBDSHIFT_ALTGR;
    return f;
}

/* ---- Maus (INT 33h) ----------------------------------------------------- */
static unsigned mou_open, mou_shown;
static unsigned mou_lb = 0xFFFF, mou_lx, mou_ly;

static void mouse(void)
{
    do_int(0x33, &r);
}

static unsigned mou_init(void)
{
    rclr();
    if (*(unsigned long __far *)MK_FP(0, 0x33 * 4) == 0)
        return 0;                       /* kein Treiber                 */
    mouse();                            /* AX = 0: zuruecksetzen         */
    if (r.ax != 0xFFFF)
        return 0;
    mou_open = 1;
    mou_shown = 0;
    mou_lb = 0xFFFF;
    return r.bx == 0xFFFF ? 2 : r.bx;
}

static void mou_show(unsigned on)
{
    if (!mou_open || on == mou_shown)
        return;
    rclr();
    r.ax = on ? 1 : 2;
    mouse();
    mou_shown = on;
}

static unsigned mou_event(unsigned off, unsigned flags)
{
    struct fapi_mouevent ev;
    unsigned x, y;
    if (!mou_open)
        return 1;
    for (;;) {
        rclr();
        r.ax = 3;
        mouse();
        x = r.cx;
        y = r.dx;
        if (gfx_mode == 0x13 || gfx_mode == VIO_MODEX)
            x >>= 1;                    /* INT 33h zaehlt 640 Punkte    */
        if (r.bx != mou_lb || x != mou_lx || y != mou_ly) {
            int moved = gfx_mode || (flags & MOU_XY) ?
                        (x != mou_lx || y != mou_ly) :
                        ((x >> 3) != (mou_lx >> 3) || (y >> 3) != (mou_ly >> 3));
            if (r.bx != mou_lb || moved) {
                mou_lb = r.bx;
                mou_lx = x;
                mou_ly = y;
                ev.buttons = (unsigned char)(r.bx & 7);
                ev.row = (unsigned char)(y >> 3);
                ev.col = (unsigned char)(x >> 3);
                ev.rsv = 0;
                ev.x = x;
                ev.y = y;
                to_app(off, &ev, (flags & MOU_XY) ? 8 : 4);
                return 0;
            }
        }
        if (!(flags & MOU_WAIT))
            return 1;
        if (brk_flag && brk())
            return 1;
        if (kbd_peek() != 0xFFFF && is_break(kbd_peek())) {
            kbd_get();
            brk();
            return 1;
        }
        yield();
    }
}

/* ---- Programmende ------------------------------------------------------- */
static void exit_now(unsigned rc)
{
    if (gfx_mode)
        set_gfx(0);
    if (save_seg)
        set_mode(0);
    if (mou_open) {
        rclr();
        mouse();                        /* zuruecksetzen, Zeiger aus     */
    }
    rclr();
    r.ax = 0x4C00 | (rc & 0xFF);
    dos();
}

/* ---- Dateien ------------------------------------------------------------ */
static unsigned char last_con;          /* letztes Zeichen auf CON      */
static char wbuf[256];

static int is_con(unsigned h)
{
    rclr();
    r.ax = 0x4400;
    r.bx = h;
    dos();
    return !(r.flags & CF) && (r.dx & 0x82) == 0x82;   /* Geraet, Ausgabe CON */
}

/* DosWrite: auf die Konsole wird ein einzelnes LF zu CR LF (AT-DOS macht
   das in der Bildschirmausgabe, DOS nicht) */
static unsigned do_write(unsigned h, unsigned off, unsigned n)
{
    char __far *s = AP(off);
    unsigned i, k = 0;
    if (!is_con(h)) {
        rclr();
        r.ax = 0x4000;
        r.bx = h;
        r.cx = n;
        r.ds = app_ds;
        r.dx = off;
        dos();
        if (r.flags & CF)
            return err();
        if (r.ax == 0 && n)
            return FAPI_ERR_FULL;
        return r.ax;
    }
    for (i = 0; i <= n; i++) {
        if (i == n || k >= sizeof(wbuf) - 2) {
            if (k) {
                rclr();
                r.ax = 0x4000;
                r.bx = h;
                r.cx = k;
                r.dx = (unsigned)wbuf;
                dos();
                k = 0;
            }
            if (i == n)
                break;
        }
        if (s[i] == '\n' && last_con != '\r')
            wbuf[k++] = '\r';
        wbuf[k++] = s[i];
        last_con = (unsigned char)s[i];
    }
    return n;
}

static unsigned do_create(unsigned off, unsigned append)
{
    if (append) {
        rclr();
        r.ax = 0x3D01;
        r.ds = app_ds;
        r.dx = off;
        dos();
        if (!(r.flags & CF)) {
            unsigned h = r.ax;
            rclr();
            r.ax = 0x4202;              /* ans Ende                     */
            r.bx = h;
            dos();
            return h;
        }
    }
    rclr();
    r.ax = 0x3C00;
    r.ds = app_ds;
    r.dx = off;
    dos();
    return (r.flags & CF) ? err() : r.ax;
}

/* DosFindEntry: n-ter Eintrag; fortlaufend ueber 4Fh, sonst neu zaehlen */
static char dta[43], find_pat[80];
static unsigned find_n = 0xFFFF;

static void set_dta(void)
{
    rclr();
    r.ax = 0x1A00;
    r.dx = (unsigned)dta;
    dos();
}

static int same_str(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b))
        a++, b++;
    return *a == *b;
}

static unsigned find_entry(unsigned off, unsigned n, unsigned dir)
{
    static char pat[80];
    struct fapi_dirent __far *de = (struct fapi_dirent __far *)AP(off);
    unsigned i = 0, k;
    if (dir)
        from_app(pat, dir, sizeof(pat) - 5);
    else
        pat[0] = 0;
    while (pat[i])
        i++;
    if (i && pat[i - 1] != '\\' && pat[i - 1] != ':')
        pat[i++] = '\\';
    pat[i++] = '*'; pat[i++] = '.'; pat[i++] = '*';
    pat[i] = 0;
    set_dta();
    if (n == find_n + 1 && find_n != 0xFFFF && same_str(pat, find_pat)) {
        rclr();
        r.ax = 0x4F00;
        dos();
    } else {
        for (i = 0; pat[i]; i++)
            find_pat[i] = pat[i];
        find_pat[i] = 0;
        rclr();
        r.ax = 0x4E00;
        r.cx = 0x16;                    /* auch versteckt, System, Verz. */
        r.dx = (unsigned)pat;
        dos();
        for (k = 0; k < n && !(r.flags & CF); k++) {
            rclr();
            r.ax = 0x4F00;
            dos();
        }
    }
    if (r.flags & CF) {
        find_n = 0xFFFF;
        return crit_hit ? err() : FAPI_ERROR;
    }
    find_n = n;
    for (i = 0; i < 13 && dta[30 + i]; i++)
        de->name[i] = dta[30 + i];
    de->name[i] = 0;
    de->attr = (unsigned char)dta[21];
    de->size = *(unsigned long *)(dta + 26);
    de->time = *(unsigned *)(dta + 22);
    de->date = *(unsigned *)(dta + 24);
    return 0;
}

static unsigned qcurdir(unsigned off, unsigned d1)
{
    static char b[70];
    unsigned d, i;
    if (d1)
        d = d1 - 1;
    else {
        rclr();
        r.ax = 0x1900;
        dos();
        d = r.ax & 0xFF;
    }
    b[0] = (char)('A' + d);
    b[1] = ':';
    b[2] = '\\';
    rclr();
    r.ax = 0x4700;
    r.dx = d + 1;
    r.si = (unsigned)(b + 3);
    dos();
    if (r.flags & CF)
        return err();
    for (i = 0; b[i]; i++)
        ;
    to_app(off, b, i + 1);
    return 0;
}

/* freie Bytes, Sektoren: AX Sektoren/Cluster, BX frei, CX Bytes/Sektor */
static int disk_free(unsigned d)
{
    rclr();
    r.ax = 0x3600;
    r.dx = d + 1;
    dos();
    return r.ax != 0xFFFF && !crit_hit;
}

static unsigned long times(unsigned long a, unsigned b)
{
    if (a < 0x10000UL)
        return ulmul((unsigned)a, b);
    return (unsigned long)b << 16;      /* 64-KB-Cluster                */
}

static int is_cdrom(unsigned d)
{
    rclr();
    r.ax = 0x150B;                      /* MSCDEX: Laufwerk pruefen      */
    r.cx = d;
    do_int(0x2F, &r);
    return r.bx == 0xADAD && r.ax != 0;
}

static unsigned qfsinfo(unsigned d, unsigned off)
{
    static struct fapi_fsinfo fi;
    static const char fat[] = "FAT", iso[] = "ISO9660";
    const char *nm = fat;
    unsigned i, spc;
    char *p = (char *)&fi;
    for (i = 0; i < sizeof(fi); i++)
        p[i] = 0;
    if (d >= 26)
        return FAPI_ERROR;
    if (is_cdrom(d)) {
        nm = iso;
        fi.bsize = 2048;
        fi.flags = FSI_REMOVABLE | FSI_READONLY;
        if (!disk_free(d))
            fi.flags |= FSI_NOMEDIA;
    } else {
        rclr();
        r.ax = 0x4408;                  /* Wechselmedium?               */
        r.bx = d + 1;
        dos();
        if ((r.flags & CF) && r.ax == 0x0F)
            return FAPI_ERROR;          /* kein solches Laufwerk         */
        if (!(r.flags & CF) && r.ax == 0)
            fi.flags |= FSI_REMOVABLE;
        if (!disk_free(d)) {
            if (!crit_hit)
                return FAPI_ERROR;
            fi.flags |= FSI_NOMEDIA;
            fi.bsize = 512;
        } else {
            spc = r.ax;
            fi.bsize = r.cx;
            fi.sectors = ulmul(r.dx, spc);
        }
    }
    for (i = 0; nm[i]; i++)
        fi.fsname[i] = nm[i];
    to_app(off, &fi, sizeof(fi));
    return 0;
}

static unsigned label(unsigned d, unsigned off)
{
    static struct fapi_label lb;
    static char pat[] = "A:\\*.*";
    static unsigned char info[25];
    unsigned i, k;
    for (i = 0; i < sizeof(lb); i++)
        ((char *)&lb)[i] = 0;
    pat[0] = (char)('A' + d);
    set_dta();
    rclr();
    r.ax = 0x4E00;
    r.cx = 0x08;
    r.dx = (unsigned)pat;
    dos();
    if (crit_hit)
        return err();
    while (!(r.flags & CF) && !(dta[21] & 0x08)) {
        rclr();
        r.ax = 0x4F00;
        dos();
    }
    if (!(r.flags & CF))
        for (i = k = 0; dta[30 + i] && k < 11; i++)
            if (dta[30 + i] != '.')
                lb.label[k++] = dta[30 + i];
    rclr();
    r.ax = 0x6900;                      /* Seriennummer (DOS 4+)         */
    r.bx = d + 1;
    r.dx = (unsigned)info;
    dos();
    if (!(r.flags & CF))
        lb.serial = *(unsigned long *)(info + 2);
    to_app(off, &lb, sizeof(lb));
    return 0;
}

/* ---- Umgebung ----------------------------------------------------------- */
static char __far *env(void)
{
    return MK_FP(*(unsigned __far *)MK_FP(psp_seg, 0x2C), 0);
}

static unsigned getenv(unsigned noff, unsigned boff, unsigned len)
{
    static char name[64];
    char __far *e = env();
    unsigned i, n;
    from_app(name, noff, sizeof(name));
    while (*e) {
        for (i = 0; name[i] && up(e[i]) == up(name[i]); i++)
            ;
        if (!name[i] && e[i] == '=') {
            e += i + 1;
            for (n = 0; e[n]; n++)
                ;
            if (!len)
                return FAPI_ERROR;
            for (i = 0; i < n && i < len - 1; i++)
                AP(boff)[i] = e[i];
            AP(boff)[i] = 0;
            return n;
        }
        while (*e++)
            ;
    }
    return FAPI_ERROR;
}

static unsigned enventry(unsigned idx, unsigned boff, unsigned len)
{
    char __far *e = env();
    unsigned i, n;
    while (*e && idx) {
        while (*e++)
            ;
        idx--;
    }
    if (!*e || !len)
        return FAPI_ERROR;
    for (n = 0; e[n]; n++)
        ;
    for (i = 0; i < n && i < len - 1; i++)
        AP(boff)[i] = e[i];
    AP(boff)[i] = 0;
    return n;
}

/* ---- Programme starten -------------------------------------------------- */
static struct {
    unsigned env;
    unsigned tail_off, tail_seg;
    unsigned fcb1_off, fcb1_seg, fcb2_off, fcb2_seg;
} pblk;
static char fcb[16];

static unsigned exec(unsigned off, unsigned mode)
{
    static char path[80], tail[130];
    char __far *s = AP(off);
    unsigned i = 0, k = 0, last = 0, dot = 0;
    if (mode == EXEC_BG)
        return EXEC_ERR_NOSLOT;         /* DOS hat keine Pipes           */
    while (*s == ' ')
        s++;
    while (*s && *s != ' ' && i < sizeof(path) - 5) {
        if (*s == '\\' || *s == '/' || *s == ':') {
            last = i + 1;
            dot = 0;
        } else if (*s == '.')
            dot = 1;
        path[i++] = *s++;
    }
    (void)last;
    if (!dot) {
        path[i++] = '.'; path[i++] = 'E'; path[i++] = 'X'; path[i++] = 'E';
    }
    path[i] = 0;
    while (*s == ' ')
        s++;
    tail[1] = ' ';
    for (k = 2; *s && k < 127; )
        tail[k++] = *s++;
    if (k == 2)
        k = 1;                          /* keine Argumente               */
    tail[0] = (char)(k - 1);
    tail[k] = '\r';
    pblk.env = 0;
    pblk.tail_off = (unsigned)tail;
    pblk.fcb1_off = pblk.fcb2_off = (unsigned)fcb;
    pblk.tail_seg = pblk.fcb1_seg = pblk.fcb2_seg = get_cs();
    rclr();
    r.ax = 0x4B00;
    r.dx = (unsigned)path;
    r.bx = (unsigned)&pblk;
    dos();
    video_init();                       /* das Kind kann umgeschaltet haben */
    if (r.flags & CF) {
        if (r.ax == 8)
            return EXEC_ERR_NOMEM;
        if (r.ax == 0x0B)
            return EXEC_ERR_FORMAT;
        return EXEC_ERR_NOTFOUND;
    }
    return 0;
}

/* ---- Systemangaben ------------------------------------------------------ */
static unsigned sysinfo(unsigned off)
{
    static struct fapi_sysinfo si;
    char __far *f;
    unsigned i;
    char *p = (char *)&si;
    for (i = 0; i < sizeof(si); i++)
        p[i] = 0;
    si.cpu = (unsigned char)cpu_type();
    rclr();
    do_int(0x11, &r);
    si.equip = r.ax;
    si.fpu = (r.ax & 2) ? 1 : 0;
    si.vga = vga;
    si.model = *(unsigned char __far *)MK_FP(0xF000, 0xFFFE);
    f = MK_FP(0xF000, 0xFFF5);
    for (i = 0; i < 8; i++)
        si.bios_date[i] = f[i];
    for (i = 0; i < 4; i++)
        si.com[i] = BIOSW(i * 2);
    for (i = 0; i < 3; i++)
        si.lpt[i] = BIOSW(8 + i * 2);
    rclr();
    do_int(0x12, &r);
    si.conv_kb = r.ax;
    rclr();
    r.ax = 0x8800;
    do_int(0x15, &r);
    if (!(r.flags & CF) && si.cpu >= 2)
        si.ext_kb = r.ax;
    to_app(off, &si, sizeof(si));
    return 0;
}

static unsigned meminfo(unsigned off, unsigned size)
{
    static struct fapi_meminfo mi;
    rclr();
    do_int(0x12, &r);
    mi.total_kb = r.ax;
    rclr();
    r.ax = 0x4800;
    r.bx = 0xFFFF;
    dos();
    mi.free_kb = r.bx >> 6;
    mi.kernel_kb = mi.apps_kb = mi.alloc_kb = 0;
    mi.total_kb32 = mi.total_kb;
    mi.free_kb32 = mi.free_kb;
    to_app(off, &mi, size >= sizeof(mi) ? sizeof(mi) : 10);
    return 0;
}

static unsigned datetime(unsigned off, int set)
{
    unsigned dt[2];
    if (set) {
        dt[0] = *(unsigned __far *)AP(off);
        dt[1] = *(unsigned __far *)AP(off + 2);
        rclr();
        r.ax = 0x2B00;
        r.cx = (dt[0] >> 9) + 1980;
        r.dx = (((dt[0] >> 5) & 15) << 8) | (dt[0] & 31);
        dos();
        rclr();
        r.ax = 0x2D00;
        r.cx = ((dt[1] >> 11) << 8) | ((dt[1] >> 5) & 63);
        r.dx = ((dt[1] & 31) * 2) << 8;
        dos();
        return 0;
    }
    rclr();
    r.ax = 0x2A00;
    dos();
    dt[0] = ((r.cx - 1980) << 9) | ((r.dx >> 8) << 5) | (r.dx & 0xFF);
    rclr();
    r.ax = 0x2C00;
    dos();
    dt[1] = ((r.cx >> 8) << 11) | ((r.cx & 0xFF) << 5) | ((r.dx >> 8) >> 1);
    to_app(off, dt, 4);
    return 0;
}

static unsigned sleep_ms(unsigned ms)
{
    unsigned t0, n;
    if (ms <= 65) {                     /* fein: INT 15h/86h (AT)        */
        rclr();
        r.ax = 0x8600;
        r.dx = ms * 1000u;
        do_int(0x15, &r);
        if (!(r.flags & CF))
            return 0;
    }
    n = ms / 55u + 1;
    t0 = ticks();
    while ((unsigned)(ticks() - t0) < n) {
        if (brk_flag && brk())
            return 0;
        yield();
    }
    return 0;
}

static unsigned setfinfo(unsigned poff, unsigned ioff)
{
    struct fapi_finfo fi;
    unsigned h, rc = 0;
    fi.mask = *(unsigned __far *)AP(ioff);
    fi.attr = *(unsigned __far *)AP(ioff + 2);
    fi.date = *(unsigned __far *)AP(ioff + 4);
    fi.time = *(unsigned __far *)AP(ioff + 6);
    if (fi.mask & 2) {
        if ((rc = dos_path(0x3D00, poff, 0)) != 0)
            return rc;
        h = r.ax;
        rclr();
        r.ax = 0x5701;
        r.bx = h;
        r.cx = fi.time;
        r.dx = fi.date;
        dos();
        rclr();
        r.ax = 0x3E00;
        r.bx = h;
        dos();
    }
    if (fi.mask & 1)
        rc = dos_path(0x4301, poff, fi.attr & 0x27);
    return rc;
}

/* ---- Verteiler ---------------------------------------------------------- */
unsigned fapi_dos(unsigned fn, unsigned a1, unsigned a2, unsigned a3)
{
    unsigned rc = FAPI_ERROR, i;

    switch (fn) {
    case FAPI_GETVERSION:   rc = FAPI_VERSION; break;
    case FAPI_EXIT:         exit_now(a1); break;
    case FAPI_YIELD:        yield(); rc = 0; break;
    case FAPI_SLEEP:        rc = sleep_ms(a1); break;
    case FAPI_MEMINFO:      rc = meminfo(a1, a2); break;
    case FAPI_ALLOCSEG:
        i = dos_alloc(a1 ? (a1 + 15) >> 4 : 0x1000);
        if (i) {
            far_zero(i, 0, a1 ? a1 : 0x8000);
            if (!a1)
                far_zero(i, 0x8000, 0x8000);
            rc = i;
        }
        break;
    case FAPI_FREESEG:      dos_free(a1); rc = (r.flags & CF) ? FAPI_ERROR : 0; break;
    case FAPI_EXECPGM:      rc = exec(a1, a2); break;
    case FAPI_QCHILDRC:
        rclr();
        r.ax = 0x4D00;
        dos();
        rc = (r.ax >> 8) ? 255 : (r.ax & 0xFF);
        break;
    case FAPI_GETARGS: {
        char __far *t = MK_FP(psp_seg, 0x81);
        unsigned n = *(unsigned char __far *)MK_FP(psp_seg, 0x80), k = 0;
        i = 0;
        while (i < n && t[i] == ' ')
            i++;
        while (i < n && t[i] != '\r' && k < FAPI_ARGLEN - 1)
            AP(a1)[k++] = t[i++];
        AP(a1)[k] = 0;
        rc = 0;
        break;
    }
    case FAPI_OPEN:
        rc = dos_path(0x3D00, a1, 0);
        if (!rc)
            rc = r.ax;
        break;
    case FAPI_CREATE:       rc = do_create(a1, a2); break;
    case FAPI_READ:
        rclr();
        r.ax = 0x3F00;
        r.bx = a1;
        r.cx = a3;
        r.ds = app_ds;
        r.dx = a2;
        dos();
        rc = (r.flags & CF) ? err() : r.ax;
        break;
    case FAPI_WRITE:        rc = do_write(a1, a2, a3); break;
    case FAPI_CLOSE:
        rclr();
        r.ax = 0x3E00;
        r.bx = a1;
        dos();
        rc = (r.flags & CF) ? FAPI_ERROR : 0;
        break;
    case FAPI_SEEK:
        rclr();
        r.ax = 0x4200;
        r.bx = a1;
        r.dx = *(unsigned __far *)AP(a2);
        r.cx = *(unsigned __far *)AP(a2 + 2);
        dos();
        rc = (r.flags & CF) ? FAPI_ERROR : 0;
        break;
    case FAPI_DELETE:       rc = dos_path(0x4100, a1, 0); break;
    case FAPI_MKDIR:        rc = dos_path(0x3900, a1, 0); break;
    case FAPI_RMDIR:        rc = dos_path(0x3A00, a1, 0); break;
    case FAPI_CHDIR:        rc = dos_path(0x3B00, a1, 0); break;
    case FAPI_MOVE:
        rclr();
        r.ax = 0x5600;
        r.ds = r.es = app_ds;
        r.dx = a1;
        r.di = a2;
        dos();
        rc = (r.flags & CF) ? err() : 0;
        break;
    case FAPI_FINDENTRY:    rc = find_entry(a1, a2, a3); break;
    case FAPI_QCURDIR:      rc = qcurdir(a1, a2); break;
    case FAPI_QCURDISK:
        rclr();
        r.ax = 0x1900;
        dos();
        rc = r.ax & 0xFF;
        break;
    case FAPI_SELECTDISK:
        if (a1 >= 26)
            break;
        if (!disk_free(a1)) {
            rc = crit_hit ? FAPI_ERR_NOTREADY : FAPI_ERROR;
            crit_hit = 0;
            break;
        }
        rclr();
        r.ax = 0x0E00;
        r.dx = a1;
        dos();
        rclr();
        r.ax = 0x1900;
        dos();
        rc = (r.ax & 0xFF) == a1 ? 0 : FAPI_ERROR;
        break;
    case FAPI_FREESPACE:
        if (!disk_free(a1)) {
            rc = crit_hit ? err() : FAPI_ERROR;
            break;
        }
        {
            unsigned long b = times(ulmul(r.ax, r.cx), r.bx);
            to_app(a2, &b, 4);
        }
        rc = 0;
        break;
    case FAPI_QFSINFO:      rc = qfsinfo(a1, a2); break;
    case FAPI_LABEL:        rc = a3 ? FAPI_ERR_WP : label(a1, a2); break;
    case FAPI_SETFINFO:     rc = setfinfo(a1, a2); break;
    case FAPI_GETENV:       rc = getenv(a1, a2, a3); break;
    case FAPI_ENVENTRY:     rc = enventry(a1, a2, a3); break;
    case FAPI_GETDATETIME:  rc = datetime(a1, 0); break;
    case FAPI_SETDATETIME:  rc = datetime(a1, 1); break;
    case FAPI_DUP:
        rclr();
        r.ax = 0x4500;
        r.bx = a1;
        dos();
        rc = (r.flags & CF) ? FAPI_ERROR : r.ax;
        break;
    case FAPI_DUP2: {
        static char con[] = "CON";
        unsigned h = a1;
        if (a1 == 0xFFFF) {
            rclr();
            r.ax = 0x3D02;
            r.dx = (unsigned)con;
            dos();
            if (r.flags & CF)
                break;
            h = r.ax;
        }
        rclr();
        r.ax = 0x4600;
        r.bx = h;
        r.cx = a2;
        dos();
        rc = (r.flags & CF) ? FAPI_ERROR : 0;
        if (a1 == 0xFFFF) {
            rclr();
            r.ax = 0x3E00;
            r.bx = h;
            dos();
        }
        break;
    }
    case FAPI_QHANDTYPE:
        rclr();
        r.ax = 0x4400;
        r.bx = a1;
        dos();
        if (r.flags & CF)
            break;
        rc = !(r.dx & 0x80) ? HT_FILE : (r.dx & 3) ? HT_CONSOLE : HT_DEVICE;
        break;
    case FAPI_SYSINFO:      rc = sysinfo(a1); break;
    case FAPI_CACHEINFO:
        far_zero(app_ds, a1, sizeof(struct fapi_cacheinfo));
        rc = 0;
        break;

    /* Bildschirm */
    case FAPI_VIOWRTSTR:    wrt_str(a1, a2, 0x07); rc = 0; break;
    case FAPI_VIOWRTSTRATT: wrt_str(a1, a2, a3); rc = 0; break;
    case FAPI_VIOWRTTTY:    tty(a1, a2); rc = 0; break;
    case FAPI_VIOCLS:       cls(); rc = 0; break;
    case FAPI_VIOSETCURPOS:
        if ((a1 >> 8) == 0xFF)
            cursor_show(0);
        else {
            cursor_pos(a1 >> 8, a1 & 0xFF);
            cursor_show(1);
        }
        rc = 0;
        break;
    case FAPI_VIOSETMODE:   set_mode(a1); rc = vrows; break;
    case FAPI_VIOGETROWS:   rc = vrows; break;
    case FAPI_VIOSETLINES:  rc = set_lines(a1); break;
    case FAPI_VIOGETBUF:
        if (!lvb_seg) {
            lvb_seg = dos_alloc(8000 / 16);
            if (!lvb_seg)
                break;
            far_zero(lvb_seg, 0, 8000);
            if (vcols == 80)
                screen_copy(lvb_seg, vseg, vrows * 80);
        }
        rc = lvb_seg;
        break;
    case FAPI_VIOSHOWBUF:
        if (lvb_seg && vcols == 80 && !gfx_mode) {
            unsigned __far *d, __far *s;
            if (a2 >= vrows)
                a2 = vrows - 1;
            if (a1 <= a2) {
                d = MK_FP(vseg, a1 * 160);
                s = MK_FP(lvb_seg, a1 * 160);
                for (i = (a2 - a1 + 1) * 80; i; i--)
                    *d++ = *s++;
            }
        }
        rc = 0;
        break;
    case FAPI_VIOSETGFX:
        if (a1 && !vga)
            break;
        if (a1 && a1 != 0x13 && a1 != 0x12 && a1 != VIO_MODEX)
            break;
        set_gfx(a1);
        rc = 0;
        break;
    case FAPI_VIOGETPHYSBUF: rc = gfx_mode ? 0xA000 : FAPI_ERROR; break;
    case FAPI_VIOSETPAL:
        outb(0x3C8, (unsigned char)a1);
        for (i = 0; i < a2 * 3; i++)
            outb(0x3C9, (unsigned char)AP(a3)[i]);
        rc = 0;
        break;
    case FAPI_VIOSETPLANE:
        outb(0x3C4, 0x02); outb(0x3C5, (unsigned char)(1 << (a1 & 3)));
        outb(0x3CE, 0x04); outb(0x3CF, (unsigned char)(a1 & 3));
        rc = 0;
        break;
    case FAPI_VIOSETPAGE: {
        unsigned start = a1 ? 19200 : 0, t0 = ticks();
        while ((inb(0x3DA) & 8) && (unsigned)(ticks() - t0) < 2)
            ;
        while (!(inb(0x3DA) & 8) && (unsigned)(ticks() - t0) < 2)
            ;
        crtc(0x0C, (unsigned char)(start >> 8));
        crtc(0x0D, (unsigned char)start);
        rc = 0;
        break;
    }
    case FAPI_VIOSETANSI:   rc = 0; break;
    case FAPI_QTICKS:       rc = (unsigned)(ulmul(ticks(), 11) >> 1); break;  /* 55 ms -> 10 ms */

    /* Tastatur, Maus */
    case FAPI_KBDCHARIN:    rc = kbd_charin(); break;
    case FAPI_KBDPEEK:
        rc = kbd_peek();
        if (rc == 0xFFFF)
            rc = 0;
        else if (is_break(rc)) {
            kbd_get();
            brk();
            rc = 0;
        }
        break;
    case FAPI_KBDSHIFT:     rc = kbd_shift(); break;
    case FAPI_MOUOPEN:      rc = mou_init(); break;
    case FAPI_MOUREADEVENT: rc = mou_event(a1, a2); break;
    case FAPI_MOUSHOWPTR:   mou_show(a1); rc = 0; break;
    case FAPI_CLIPPUT:
    case FAPI_CLIPGET:      rc = 0; break;      /* keine Ablage unter DOS */

    /* Signale */
    case FAPI_SETSIGHANDLER:
        sig_tramp = a1;
        sig_mask = a1 ? a2 & (SIG_BREAK | SIG_SHUTDOWN) : 0;
        rc = 0;
        break;
    case FAPI_SHUTDOWNHOLD: rc = 0; break;

    /* nicht unter DOS: OPENDRIVE, DISKINFO, OPENDISK, FMTTRACK, SUBST,
       JOIN, PIPE, SETENV, KBDSETLAYOUT, COMCTL, SHUTDOWN ... */
    }
    if (brk_flag)                       /* Strg+C waehrend eines DOS-Aufrufs */
        brk();
    return rc;
}

/* ---- Start -------------------------------------------------------------- */
void stub_main(void)
{
    struct fx_header *fx;
    unsigned cs = get_cs(), app, dseg, dinit, need;
    unsigned long total;

    if (cpu_type() < 2) {
        out("Dieses Programm braucht einen 80286 oder neuer.\r\n");
        rclr();
        r.ax = 0x4C01;
        dos();
    }
    rclr();
    r.ax = 0x3000;
    dos();
    fx = (struct fx_header *)fx_rel;
    if ((r.ax & 0xFF) < 3 || ((r.ax & 0xFF) == 3 && (r.ax >> 8) < 10) ||
        fx_rel == 0 || fx->sig[0] != FX_SIG0 || fx->sig[1] != FX_SIG1 ||
        !(fx->flags & FX_FLAG_DOS) || (fx->image_off & 15)) {
        out("Diese FAPI-Anwendung braucht AT-DOS oder MS-DOS 3.1 und neuer.\r\n");
        rclr();
        r.ax = 0x4C01;
        dos();
    }
    /* Abbild steht ab image_off der Datei; der Rumpf beginnt nach dem
       64-Byte-MZ-Kopf an cs:0. Version 2: Code und Daten getrennt, das
       Datensegment liegt direkt hinter dem Code (an Ort und Stelle) */
    app = cs + ((fx->image_off - 64) >> 4);
    if (fx->version == FX_VERSION2) {
        dseg = app + (fx->code_size >> 4);
        dinit = fx->image_size - fx->code_size;
    } else {
        dseg = app;
        dinit = fx->image_size;
    }
    total = (unsigned long)dinit + fx->bss_size + fx->stack_size;
    need = dseg + (unsigned)((total + 15) >> 4) - psp_seg;
    rclr();
    r.ax = 0x4A00;
    r.es = psp_seg;
    r.bx = need;
    dos();
    if (r.flags & CF) {
        out("Nicht gen\x81gend Arbeitsspeicher.\r\n");
        rclr();
        r.ax = 0x4C08;
        dos();
    }
    far_zero(dseg, dinit, fx->bss_size);
    {
        unsigned __far *g = MK_FP(dseg, 2);  /* Gate-Zeiger (crt0)      */
        if (g[0] != 0 || g[1] != FAPI_GATE) {
            out("Programm ohne Gate-Zeiger (mit altem crt0 gebaut).\r\n");
            rclr();
            r.ax = 0x4C01;
            dos();
        }
        g[0] = (unsigned)gate_entry;
        g[1] = cs;
    }
    rclr();
    r.ax = 0x2523;                      /* Strg+C                        */
    r.dx = (unsigned)int23;
    dos();
    rclr();
    r.ax = 0x2524;                      /* kritische Fehler              */
    r.dx = (unsigned)int24;
    dos();

    video_init();
    cur_shape = BIOSW(0x60);
    rclr();
    r.ax = 0x1A00;
    video();
    vga = (r.ax & 0xFF) == 0x1A && (r.bx & 0xFF) >= 7;
    kbd_enh = (BIOS(0x96) & 0x10) != 0;

    run_app(app, dseg, (unsigned)total, fx->entry);
}
