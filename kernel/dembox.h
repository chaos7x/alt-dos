/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Zugriff auf den Speicher einer DOS-Box fuer DEM und
 * Redirector (kernel/dem.c, kernel/redir.c): ueber das Fenster RWIN_SEL(t)
 * der Box, das keine andere Box umlegt. Ein Zeiger gilt bis zum naechsten
 * bp() derselben Box.
 */
#ifndef DEMBOX_H
#define DEMBOX_H

static unsigned char __far *bp(int t, unsigned seg, unsigned off)
{
    struct desc *d = &gdt[RWIN_IDX(t)];
    unsigned long b = (unsigned long)seg << 4;
    d->limit = 0xFFFF;
    d->base_lo = (unsigned short)b;
    d->base_hi = (unsigned char)(b >> 16);
    d->access = ACC_KDATA;
    d->rsv = 0;
    return MK_FP(RWIN_SEL(t), off);
}

static unsigned r16(unsigned char __far *p) { return p[0] | (p[1] << 8); }
static void w16(unsigned char __far *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static unsigned long r32(unsigned char __far *p) { return r16(p) | ((unsigned long)r16(p + 2) << 16); }
static void w32(unsigned char __far *p, unsigned long v) { w16(p, (unsigned)v); w16(p + 2, (unsigned)(v >> 16)); }

#define FAILED(r)   ((r) >= FAPI_ERR_FIRST)

/* DOS-Fehler */
#define E_NOFILE    2
#define E_NOPATH    3
#define E_DENIED    5
#define E_HANDLE    6
#define E_NOMORE    18
#define E_WP        19
#define E_NOTREADY  21
#define E_SHARE     32
#define E_LOCK      0x21
#define E_SHTAB     0x24
#define E_EXISTS    80

#endif
