/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - VESA (VBE 2.0/3.0) fuer FAPI-Programme (Phase 5, Schritt 2)
 *
 * Umschalten kann nur das Video-BIOS. Wie OS/2 1.x geht der Kernel dafuer
 * kurz in den Real Mode (start.asm rm_int10): Register bei physisch 6F00h,
 * Puffer ab 7000h (VbeInfoBlock 512 Byte, dahinter ModeInfoBlock 256).
 * Nur mit Kernelsperre (FAPI_VESA steht nicht in fapi_nolock): SCRATCH.
 *
 * Laeuft im zweiten Codesegment (KTEXT2).
 */
#include "../include/fapi_abi.h"
#include "k2.h"

struct rmregs { unsigned short ax, bx, cx, dx, si, di, bp, es, ds, flags; };
#define RM_REGS   0x6F00UL
#define RM_BUF    0x7000UL              /* 7000h: VbeInfoBlock, 7200h: ModeInfo */

unsigned long rm_kcr3;                  /* start.asm: CR3 waehrend des Aufrufs */
unsigned      rm_cs16;                  /* Real-Mode-Segment von CGROUP2   */
void __far rm_int10(void);

/* INT 10h im Real Mode; r wird mit den Rueckgabewerten ueberschrieben */
static void rm_call(struct rmregs *r)
{
    struct rmregs __far *p;
    unsigned i;
    rm_kcr3 = paging_cr3();
    rm_cs16 = (unsigned)(KCODE2_LIN >> 4);
    scratch_map(RM_REGS, sizeof(struct rmregs) - 1);
    p = MK_FP(SCRATCH, 0);
    for (i = 0; i < sizeof(struct rmregs) / 2; i++)
        ((unsigned short __far *)p)[i] = ((unsigned short *)r)[i];
    rm_int10();
    scratch_map(RM_REGS, sizeof(struct rmregs) - 1);
    for (i = 0; i < sizeof(struct rmregs) / 2; i++)
        ((unsigned short *)r)[i] = ((unsigned short __far *)p)[i];
}

/* 16 x 16 -> 32 Bit ohne Laufzeitbibliothek */
static unsigned long mul16(unsigned a, unsigned b);
#pragma aux mul16 = "mul dx" __parm [__ax] [__dx] __value [__dx __ax] __modify [__ax __dx];

static unsigned rd16(unsigned long lin)
{
    scratch_map(lin, 1);
    return *(unsigned short __far *)MK_FP(SCRATCH, 0);
}

static unsigned char rd8(unsigned long lin)
{
    scratch_map(lin, 0);
    return *(unsigned char __far *)MK_FP(SCRATCH, 0);
}

/* VbeInfoBlock holen: -> Version, 0 = kein VBE */
static unsigned vbe_info(unsigned long *list, unsigned *mem_kb)
{
    struct rmregs r;
    scratch_map(RM_BUF, 3);
    *(unsigned long __far *)MK_FP(SCRATCH, 0) = 0x32454256UL;   /* "VBE2" */
    r.ax = 0x4F00;
    r.es = (unsigned)(RM_BUF >> 4);
    r.di = 0;
    rm_call(&r);
    if (r.ax != 0x004F || rd16(RM_BUF) != 0x4556)               /* "VE" */
        return 0;
    *list = ((unsigned long)rd16(RM_BUF + 16) << 4) + rd16(RM_BUF + 14);
    *mem_kb = rd16(RM_BUF + 18) << 6;                           /* 64-KB-Bloecke */
    return rd16(RM_BUF + 4);
}

/* ModeInfoBlock nach RM_BUF + 200h; -> 1, wenn brauchbar (Grafik, LFB,
   Palette oder direkte Farben, 8/15/16/24/32 Bit) */
static int vbe_mode(unsigned m, struct fapi_vesamode __far *o)
{
    struct rmregs r;
    unsigned attr;
    unsigned char bpp, model;
    r.ax = 0x4F01;
    r.cx = m;
    r.es = (unsigned)(RM_BUF >> 4);
    r.di = 0x200;
    rm_call(&r);
    if (r.ax != 0x004F)
        return 0;
    attr = rd16(RM_BUF + 0x200);
    bpp = rd8(RM_BUF + 0x200 + 25);
    model = rd8(RM_BUF + 0x200 + 27);
    if ((attr & 0x99) != 0x99 ||                /* da, Farbe, Grafik, LFB */
        (model != 4 && model != 6) ||
        (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32))
        return 0;
    o->mode = m;
    o->width = rd16(RM_BUF + 0x200 + 18);
    o->height = rd16(RM_BUF + 0x200 + 20);
    o->pitch = rd16(RM_BUF + 0x200 + 50);       /* LinBytesPerScanLine (3.0) */
    if (!o->pitch)
        o->pitch = rd16(RM_BUF + 0x200 + 16);
    o->bpp = bpp;
    o->model = model;
    return 1;
}

/* Modus setzen (mode = 3: Text, sonst VBE-Nummer mit Bit 14 = LFB):
   -> 0 = ok */
int __far vesa_bios(unsigned mode)
{
    struct rmregs r;
    if (mode == 3) {
        r.ax = 0x0003;
        rm_call(&r);
        return 0;
    }
    r.ax = 0x4F02;
    r.bx = mode;
    rm_call(&r);
    return r.ax != 0x004F;
}


unsigned __far vesa_fapi(int s, int t, unsigned sub, unsigned n,
                         struct fapi_vesamode __far *o)
{
    unsigned long list, phys;
    unsigned mem_kb, ver, m, k = 0, i, pages;
    if (sub == VESA_SET) {
        if (!vbe_info(&list, &mem_kb) || !vbe_mode(n, o))
            return FAPI_ERROR;
        phys = ((unsigned long)rd16(RM_BUF + 0x200 + 42) << 16) | rd16(RM_BUF + 0x200 + 40);
        pages = (unsigned)((mul16(o->pitch, o->height) + 4095) >> 12);
        if (!phys || pages > 896 || vio_setvesa(s, t, o, pages, phys))
            return FAPI_ERROR;
        o->mem_kb = mem_kb;
        o->version = 0;
        return 0;
    }
    if (sub != VESA_MODE || !(ver = vbe_info(&list, &mem_kb)))
        return FAPI_ERROR;
    for (i = 0; i < 256; i++) {
        m = rd16(list + i * 2);
        if (m == 0xFFFF)
            break;
        if (vbe_mode(m, o) && ((mul16(o->pitch, o->height) + 4095) >> 12) <= 896 &&
            k++ == n) {
            o->mem_kb = mem_kb;
            o->version = ver;
            return 0;
        }
    }
    return FAPI_ERROR;
}
