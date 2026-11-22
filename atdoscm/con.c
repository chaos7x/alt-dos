/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - Konsole und Zeichengeraete.
 *
 * CON ueber das BIOS der Box (INT 16h Tasten, INT 10h 0Eh Ausgabe - beides
 * bildet der Monitor in v86.c nach), Zeileneingabe mit Rueckschritt, Esc
 * und F3 (Vorlage) wie MS-DOS. ATIN$/ATOUT$/ATERR$ sind die umgeleiteten
 * Standard-Handles des Box-Tasks (INT E0h 1Eh), PRN/LPT1 INT 17h, AUX/COM1
 * INT 14h. Strg+C in den Eingabefunktionen setzt cc_hit; der Verteiler
 * ruft danach INT 23h wie DOS (kern.c cc_frame).
 */
#include "cm.h"

unsigned char cc_hit;
static int pend = -1;                   /* Scancode nach 00h             */
static u8  cl_buf[130];                 /* gekochte Zeile (Handle-Lesen) */
static u8  cl_n, cl_p;

/* ---- Tastatur ----------------------------------------------------------- */
void vsti(void);
#pragma aux vsti = "sti";

int con_key(int wait)
{
    struct rregs r;
    int c;
    if (pend >= 0) {
        c = pend;
        pend = -1;
        return c;
    }
    if (!wait) {
        r.ax = 0x0100;
        bint(0x16, &r);
        if (r.fl & FL_ZF)
            return -1;
    }
    vsti();                             /* Warten: Takt und Tasten-INT   */
    r.ax = 0x0000;                      /* sollen laufen (Eingang ohne   */
    bint(0x16, &r);                     /* STI, entry.asm)               */
    c = r.ax & 0xFF;
    if (c == 0 || c == 0xE0 && (r.ax >> 8) >= 0x47 && (r.ax >> 8) <= 0x53) {
        pend = r.ax >> 8;
        c = 0;
    }
    return c;
}

int con_ready(void)
{
    struct rregs r;
    if (pend >= 0 || cl_p < cl_n)
        return 1;
    r.ax = 0x0100;
    bint(0x16, &r);
    return !(r.fl & FL_ZF);
}

/* Strg+C in der Warteschlange? (vor Ausgaben, wie DOS bei BREAK) */
static void cc_peek(void)
{
    struct rregs r;
    r.ax = 0x0100;
    bint(0x16, &r);
    if (!(r.fl & FL_ZF) && (r.ax & 0xFF) == 3) {
        r.ax = 0;
        bint(0x16, &r);
        cc_hit = 1;
    }
}

/* ---- Bildschirm --------------------------------------------------------- */
static unsigned col(void)
{
    return B(0x40, 0x50 + 2 * B(0x40, 0x62));
}

/* geladener CON-Treiber (Box-CONFIG.SYS, z. B. ANSI.SYS): Ausgabe als
   WRITE-Anforderung (Befehl 8) wie DOS */
u32 con_dev;
static u8 cw_rq[0x16];
static u8 cw_ch;

static void con_dev_write(u16 seg, u16 off, unsigned n)
{
    u16 ds = (u16)(con_dev >> 16), dof = (u16)con_dev;
    unsigned i;
    for (i = 0; i < sizeof(cw_rq); i++)
        cw_rq[i] = 0;
    cw_rq[0] = 0x16;
    cw_rq[2] = 8;
    W(kseg, (u16)cw_rq + 0x0E) = off;
    W(kseg, (u16)cw_rq + 0x10) = seg;
    W(kseg, (u16)cw_rq + 0x12) = n;
    devcall(ds, W(ds, dof + 6), W(ds, dof + 8), (u16)cw_rq);
}

void con_out(unsigned c)
{
    struct rregs r;
    if (con_dev && c != 9) {
        cw_ch = (u8)c;
        con_dev_write(kseg, (u16)&cw_ch, 1);
        return;
    }
    if (c == 9) {
        do
            con_out(' ');
        while (col() & 7);
        return;
    }
    r.ax = 0x0E00 | (c & 0xFF);
    r.bx = (u16)B(0x40, 0x62) << 8 | 7;
    bint(0x10, &r);
}

static void cc_echo(void)
{
    con_out('^');
    con_out('C');
    con_out(13);
    con_out(10);
    cc_hit = 1;
}

/* ---- Standard-Ein-/Ausgabe ueber die Handles 0 und 1 ------------------- */
static int std_con(unsigned h)          /* Handle h ist CON?              */
{
    int s = hnd_sft(h);
    return s >= 0 && (sftab[s].flags & 0x80) && sftab[s].hnd == DV_CON;
}

static void std_put(unsigned c)
{
    u8 b = (u8)c;
    unsigned e;
    if (std_con(1))
        con_out(c);
    else
        h_write(1, kseg, (u16)&b, 1, &e);
}

/* ein Zeichen von der Standardeingabe; -1 = Ende (umgeleitet) */
static int std_get(void)
{
    u8 b;
    unsigned e;
    if (std_con(0))
        return con_key(1);
    if (h_read(0, kseg, (u16)&b, 1, &e) != 1)
        return 0x1A;
    return b;
}

/* ---- Zeileneingabe (0Ah, gekochtes Lesen) ------------------------------- */
void con_line(u8 __far *buf)
{
    unsigned max = buf[0], n = 0, tl = buf[1], tp = 0, c;
    u8 tmp[130];
    unsigned i;
    if (!max)
        return;
    for (i = 0; i < tl && i < 128; i++)
        tmp[i] = buf[2 + i];
    for (;;) {
        c = (unsigned)con_key(1);
        if (c == 0) {
            c = (unsigned)con_key(1);
            if (c == 0x3D || c == 0x4D) {       /* F3 / F1: Vorlage      */
                while (tp < tl && n + 1 < max && (c == 0x3D || tp == n)) {
                    buf[2 + n++] = tmp[tp];
                    con_out(tmp[tp++]);
                    if (c == 0x4D)
                        break;
                }
            }
            continue;
        }
        if (c == 3) {
            cc_echo();
            buf[1] = 0;
            buf[2] = 13;
            return;
        }
        if (c == 13)
            break;
        if (c == 8) {
            if (n) {
                n--;
                if (tp)
                    tp--;
                con_out(8);
                con_out(' ');
                con_out(8);
            }
            continue;
        }
        if (c == 27) {                          /* Zeile verwerfen       */
            con_out('\\');
            con_out(13);
            con_out(10);
            n = tp = 0;
            continue;
        }
        if (n + 1 >= max) {
            con_out(7);
            continue;
        }
        buf[2 + n++] = (u8)c;
        if (tp < tl)
            tp++;
        con_out(c);
    }
    buf[1] = (u8)n;
    buf[2 + n] = 13;
    con_out(13);
}

/* ---- 01h-0Ch ------------------------------------------------------------ */
void con_fn(struct frame __far *f)
{
    int c;
    unsigned i;
    switch (AH(f)) {
    case 0x01:
        c = std_get();
        if (c == 3) {
            cc_echo();
            return;
        }
        std_put((unsigned)c);
        AL(f) = (u8)c;
        return;
    case 0x02:
        cc_peek();
        std_put(DL(f));
        AL(f) = DL(f);
        return;
    case 0x03: {                        /* AUX lesen                     */
        struct rregs r;
        r.ax = 0x0200;
        r.dx = 0;
        bint(0x14, &r);
        AL(f) = (u8)r.ax;
        return;
    }
    case 0x04: case 0x05: {             /* AUX / PRN schreiben           */
        struct rregs r;
        r.ax = (AH(f) == 4 ? 0x0100 : 0x0000) | DL(f);
        r.dx = 0;
        bint(AH(f) == 4 ? 0x14 : 0x17, &r);
        return;
    }
    case 0x06:
        if (DL(f) != 0xFF) {
            std_put(DL(f));
            AL(f) = DL(f);
            return;
        }
        if (std_con(0)) {
            c = con_key(0);
            if (c < 0) {
                f->fl |= FL_ZF;
                AL(f) = 0;
                return;
            }
        } else
            c = std_get();
        f->fl &= ~FL_ZF;
        AL(f) = (u8)c;
        return;
    case 0x07: case 0x08:
        c = std_get();
        if (c == 3 && AH(f) == 8) {
            cc_echo();
            return;
        }
        AL(f) = (u8)c;
        return;
    case 0x09: {
        u8 __far *p = FP(f->ds, f->dx);
        cc_peek();
        if (std_con(1)) {
            for (i = 0; p[i] != '$' && i < 0xFFF0; i++)
                con_out(p[i]);
        } else {
            unsigned e;
            for (i = 0; p[i] != '$' && i < 0xFFF0; i++)
                ;
            h_write(1, f->ds, f->dx, i, &e);
        }
        AL(f) = '$';
        return;
    }
    case 0x0A: {
        u8 __far *b = FP(f->ds, f->dx);
        if (std_con(0)) {
            con_line(b);
            return;
        }
        for (i = 0; i + 1 < b[0]; ) {       /* umgeleitet: bis CR        */
            c = std_get();
            if (c == 0x1A || c == 13)
                break;
            if (c == 10)
                continue;
            b[2 + i++] = (u8)c;
        }
        b[1] = (u8)i;
        b[2 + i] = 13;
        return;
    }
    case 0x0B:
        if (std_con(0)) {
            c = con_ready();
            if (c && pend < 0) {
                struct rregs r;
                r.ax = 0x0100;
                bint(0x16, &r);
                if (!(r.fl & FL_ZF) && (r.ax & 0xFF) == 3) {
                    con_key(1);
                    cc_echo();
                    return;
                }
            }
            AL(f) = c ? 0xFF : 0;
        } else
            AL(f) = 0xFF;
        return;
    case 0x0C: {
        u8 fn = AL(f);
        while (std_con(0) && con_key(0) >= 0)   /* Puffer leeren     */
            ;
        cl_n = cl_p = 0;
        if (fn == 1 || fn == 6 || fn == 7 || fn == 8 || fn == 0x0A) {
            AH(f) = fn;
            con_fn(f);
        }
        return;
    }
    }
}

/* ---- Geraete ueber Handles --------------------------------------------- */
static unsigned e0_std(unsigned h, u16 seg, u16 off, unsigned n)
{
    struct rregs r;
    r.ax = 0x1E00 | h;
    r.es = seg;
    r.di = off;
    r.cx = n;
    bint(0xE0, &r);
    return r.ax;
}

unsigned dev_read(int d, struct sft *s, u16 seg, u16 off, unsigned n)
{
    unsigned i;
    int c;
    switch (d) {
    case DV_CON:
        if (s->flags & 0x20) {          /* roh                           */
            for (i = 0; i < n; i++)
                B(seg, off + i) = (u8)con_key(1);
            return n;
        }
        if (cl_p >= cl_n) {
            cl_buf[0] = 128;
            cl_buf[1] = 0;
            con_line(cl_buf);
            if (cc_hit)
                return 0;
            cl_n = (u8)(cl_buf[1] + 2);
            cl_buf[2 + cl_buf[1] + 1] = 10;
            cl_p = 0;
            con_out(10);
        }
        for (i = 0; i < n && cl_p < cl_n; i++)
            B(seg, off + i) = cl_buf[2 + cl_p++];
        return i;
    case DV_IN:
        return e0_std(0, seg, off, n);
    case DV_AUX: case DV_COM1:
        for (i = 0; i < n; i++) {
            struct rregs r;
            r.ax = 0x0200;
            r.dx = 0;
            bint(0x14, &r);
            if (r.ax & 0x8000)
                break;
            c = r.ax & 0xFF;
            B(seg, off + i) = (u8)c;
        }
        return i;
    default:
        return 0;
    }
}

unsigned dev_write(int d, struct sft *s, u16 seg, u16 off, unsigned n)
{
    unsigned i;
    struct rregs r;
    (void)s;
    switch (d) {
    case DV_CON:
        if (n > 1)
            cc_peek();
        if (con_dev) {                  /* Bloecke an den Treiber, TAB   */
            unsigned a = 0;             /* wie DOS selbst zu Leerzeichen */
            for (i = 0; i <= n; i++)
                if (i == n || B(seg, off + i) == 9) {
                    if (i > a)
                        con_dev_write(seg, off + a, i - a);
                    if (i < n)
                        con_out(9);
                    a = i + 1;
                }
            return n;
        }
        for (i = 0; i < n; i++)
            con_out(B(seg, off + i));
        return n;
    case DV_OUT:
        return e0_std(1, seg, off, n);
    case DV_ERR:
        return e0_std(2, seg, off, n);
    case DV_PRN: case DV_LPT1:
        for (i = 0; i < n; i++) {
            r.ax = B(seg, off + i);
            r.dx = 0;
            bint(0x17, &r);
        }
        return n;
    case DV_AUX: case DV_COM1:
        for (i = 0; i < n; i++) {
            r.ax = 0x0100 | B(seg, off + i);
            r.dx = 0;
            bint(0x14, &r);
        }
        return n;
    default:
        return n;
    }
}
