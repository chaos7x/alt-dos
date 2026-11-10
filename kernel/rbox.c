/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.1 - Kompatibilitaetsbox (liegt in KCODE2)
 *
 * Eine DOS-Sitzung im echten Real Mode, wie bei OS/2 1.x. Der Kernel liegt
 * ueber 1 MB (kmove), 0-640 KB gehoert der Box. Oben im konventionellen
 * Speicher 8 KB fuer den Real-Mode-Teil (start.asm rbs_*), Zustand, den
 * gesicherten Bildschirmtext und die Kopie von IVT und BIOS-Datenbereich;
 * das BIOS meldet der Box entsprechend weniger (0413h). Solange die Box
 * vorn ist, steht AT-DOS.
 *
 * Zwei Arten: RB_LOAD ein .COM bei 1000:0100 ohne DOS (INT 20h beendet),
 * RB_BOOT/RB_IMG ein Diskettenabbild im erweiterten Speicher, gestartet
 * ueber seinen Bootsektor (FreeDOS). INT 13h fuer Laufwerk 0 liest das
 * Abbild selbst per INT 15h/87h - ohne Ausflug zu AT-DOS.
 */
#include "kern.h"
#include "mem.h"
#include "../include/fapi_abi.h"
#include "k2.h"

extern unsigned short rbs_code, rbs_len, rbs_entry_off, rbs_int9_off, rbs_int20_off,
                      rbs_inte0_off, rbs_int13_off, rbs_int15_off;
extern unsigned char  bios_pic1, bios_pic2;
extern unsigned short mem_conv_kb;
extern unsigned short gdtr[3];
extern volatile unsigned long ticks;
extern void rb_reentry(void);
int __far kf_cur(void);                 /* kernel.c: laufender Task */
int __far kf_cur_ses(void);             /* ... und seine Sitzung    */
#define current     kf_cur()

int rb_task = -1;
static unsigned long rb_lin;
static unsigned rb_img_u, rb_img_n;     /* Abbild: erste Einheit, Anzahl */
static unsigned long rb_img_pos;        /* bisher geladen (Byte)         */
static unsigned char rb_kind;           /* 1 .COM, 2 Abbild              */

#define RB_SIZE     0x2000UL
#define RB_LO       (0x10000UL / MEM_UNIT)
#define RB_SAVE     0x1400              /* IVT + BIOS-Daten (500h Byte)  */
#define RB_GDT      0x1A00              /* GDT fuer INT 15h/87h         */
#define RBB(o)      (((unsigned char __far *)MK_FP(SCRATCH, 0))[o])
#define RBW(o)      (*(unsigned short __far *)MK_FP(SCRATCH, (o)))
#define RBS(o)      (*(unsigned short __far *)MK_FP(SCRATCH2, (o)))
#define RBSB(o)     (((unsigned char __far *)MK_FP(SCRATCH2, 0))[o])

/* gemeinsamer Teil: Speicher belegen, Stummel und Zustandsblock, IVT und
   BIOS-Daten sichern */
static unsigned rb_common(void)
{
    unsigned i, hi;
    unsigned char __far *cod = MK_FP(KCODE2, rbs_code);
    unsigned seg;

    if (!kmoved || rb_task >= 0)
        return FAPI_ERROR;
    rb_lin = ((unsigned long)mem_conv_kb << 10) - RB_SIZE;
    hi = (unsigned)((rb_lin + RB_SIZE) / MEM_UNIT);
    if (!mem_is_free(RB_LO, hi))
        return FAPI_ERR_INUSE;
    mem_reserve(RB_LO, hi);
    rb_task = current;
    seg = (unsigned)(rb_lin >> 4);
    scratch_map(rb_lin, 0xFFFF);            /* Real-Mode-Teil        */
    scratch2_map(0, 0xFFFF);                /* IVT und BIOS-Daten    */
    for (i = 0; i < 0x80; i++)              /* Zustandsblock         */
        RBB(i) = 0;
    for (i = 0; i < rbs_len; i++)           /* Stummel ab 80h        */
        RBB(0x80 + i) = cod[i];
    for (i = 0; i < 2000; i++)
        RBW(0x400 + 2 * i) = 0x0720;
    for (i = 0; i < 0x500; i += 2)          /* zurueck in rb_free    */
        RBW(RB_SAVE + i) = RBS(i);
    for (i = 0; i < 48; i++)
        RBB(RB_GDT + i) = 0;
    RBW(RB_GDT + 16) = RBW(RB_GDT + 24) = 0xFFFF;
    RBB(RB_GDT + 21) = RBB(RB_GDT + 29) = 0x93;
    RBB(28) = bios_pic1;
    RBB(29) = bios_pic2;
    RBW(32) = gdtr[0];
    RBW(34) = gdtr[1];
    RBW(36) = gdtr[2];
    RBW(38) = (unsigned)rb_reentry;
    RBW(40) = KCODE;
    RBW(42) = RBS(9 * 4);
    RBW(44) = RBS(9 * 4 + 2);
    RBW(50) = seg;
    RBW(26) = 0x0202;
    RBS(9 * 4) = rbs_int9_off;
    RBS(9 * 4 + 2) = seg;
    RBS(0xE0 * 4) = rbs_inte0_off;
    RBS(0xE0 * 4 + 2) = seg;
    RBW(66) = RBS(0x15 * 4);                /* INT 15h: Tasten (4Fh), */
    RBW(68) = RBS(0x15 * 4 + 2);            /* kein erweiterter Speicher */
    RBS(0x15 * 4) = rbs_int15_off;
    RBS(0x15 * 4 + 2) = seg;
    RBS(0x413) = (unsigned)(rb_lin >> 10);
    return 0;
}

unsigned __far rb_load(unsigned sel, unsigned len)
{
    unsigned i, r;
    unsigned char __far *src = MK_FP(sel, 0);

    if (len > 0xFE00)
        return FAPI_ERROR;
    if ((r = rb_common()) != 0)
        return r;
    rb_kind = 1;
    RBS(0x20 * 4) = rbs_int20_off;
    RBS(0x20 * 4 + 2) = (unsigned)(rb_lin >> 4);
    RBW(14) = RBW(16) = RBW(20) = RBW(24) = 0x1000;
    RBW(18) = 0xFFFE;
    RBW(22) = 0x100;
    scratch_map(0x10000UL, 0xFFFF);         /* PSP und Programm      */
    for (i = 0; i < 0x100; i++)
        RBB(i) = 0;
    RBB(0) = 0xCD;                          /* INT 20h               */
    RBB(1) = 0x20;
    RBW(2) = (unsigned)(rb_lin >> 4);
    for (i = 0; i < len; i++)
        RBB(0x100 + i) = src[i];
    RBW(0xFFFE) = 0;                        /* RET -> PSP:0          */
    return 0;
}

/* Diskettenabbild: kb KB im erweiterten Speicher (unter 16 MB, das
   BIOS kopiert mit 24-Bit-Adressen) */
unsigned __far rb_boot(unsigned kb)
{
    unsigned r, n;
    unsigned long lin;

    if (kb < 160 || kb > 2880)
        return FAPI_ERROR;
    n = kb << 2;                            /* Einheiten zu 256 Byte */
    rb_img_u = mem_alloc(n);
    if (!rb_img_u)
        return FAPI_ERR_FULL;
    lin = (unsigned long)rb_img_u * MEM_UNIT;
    if (lin < 0x100000UL || lin + ((unsigned long)kb << 10) > 0x1000000UL) {
        mem_free(rb_img_u, n);
        rb_img_u = 0;
        return FAPI_ERR_FULL;
    }
    if ((r = rb_common()) != 0) {
        mem_free(rb_img_u, n);
        rb_img_u = 0;
        return r;
    }
    rb_img_n = n;
    rb_img_pos = 0;
    rb_kind = 2;
    scratch_map(rb_lin, 0xFFFF);
    RBW(56) = (unsigned)lin;
    RBW(58) = (unsigned)(lin >> 16);
    RBW(60) = kb << 1;                      /* Sektoren              */
    RBB(70) = kb <= 360 ? 1 : kb <= 720 ? 3 : kb <= 1200 ? 2 : kb <= 1440 ? 4 : 6;
    RBW(62) = RBS(0x13 * 4);
    RBW(64) = RBS(0x13 * 4 + 2);
    RBS(0x13 * 4) = rbs_int13_off;
    RBS(0x13 * 4 + 2) = (unsigned)(rb_lin >> 4);
    RBS(0x410) = (RBS(0x410) & ~0xC0u) | 1; /* ein Diskettenlaufwerk  */
    RBSB(0x475) = 0;                        /* keine Platten          */
    RBW(0) = RBW(2) = RBW(4) = RBW(6) = 0;  /* DL = 0: Laufwerk A:    */
    RBW(14) = RBW(16) = RBW(20) = RBW(24) = 0;
    RBW(18) = 0x7C00;
    RBW(22) = 0x7C00;
    return 0;
}

/* Stueck des Abbilds anhaengen; das erste enthaelt den Bootsektor */
unsigned __far rb_img(unsigned sel, unsigned off, unsigned len)
{
    unsigned i;
    unsigned char __far *src = MK_FP(sel, off);
    unsigned long lin;

    if (rb_task != current || rb_kind != 2 || (len & 511) ||
        rb_img_pos + len > ((unsigned long)rb_img_n * MEM_UNIT))
        return FAPI_ERROR;
    lin = (unsigned long)rb_img_u * MEM_UNIT + rb_img_pos;
    scratch_map(lin, len - 1);
    for (i = 0; i < len; i++)
        RBB(i) = src[i];
    if (rb_img_pos == 0) {
        scratch_map(rb_lin, 0xFFFF);
        RBW(52) = src[0x18] | (src[0x19] << 8);     /* Sektoren je Spur */
        RBW(54) = src[0x1A] | (src[0x1B] << 8);     /* Koepfe          */
        if (RBW(52) == 0 || RBW(52) > 63 || RBW(54) == 0 || RBW(54) > 2)
            return FAPI_ERROR;
        scratch_map(0x7C00, 511);
        for (i = 0; i < 512; i++)
            RBB(i) = src[i];
    }
    rb_img_pos += len;
    return 0;
}

void __far rb_free(void)
{
    unsigned i;

    if (rb_task < 0)
        return;
    mem_free(RB_LO, (unsigned)((rb_lin + RB_SIZE) / MEM_UNIT) - RB_LO);
    if (rb_img_u) {
        mem_free(rb_img_u, rb_img_n);
        rb_img_u = 0;
    }
    scratch_map(rb_lin, 0xFFFF);            /* IVT und BIOS-Daten wie */
    scratch2_map(0, 0xFFFF);                /* vor der Box            */
    for (i = 0; i < 0x46C; i += 2)
        RBS(i) = RBW(RB_SAVE + i);
    for (i = 0x470; i < 0x500; i += 2)      /* ohne den BIOS-Takt     */
        RBS(i) = RBW(RB_SAVE + i);
    rb_task = -1;
    rb_kind = 0;
}

unsigned __far rb_run(void)
{
    unsigned r, t0, d = 0;
    unsigned char rb_kind_was = rb_kind;

    if (rb_task != current)
        return FAPI_ERROR;
    if (rb_kind == 2 && rb_img_pos < 512)
        return FAPI_ERROR;
    for (;;) {
        while (vio_fg() != kf_cur_ses())
            sleep_ticks(5);
        scratch_map(0, 0xFFFF);
        t0 = RBW(0x46C);                    /* BIOS-Takt: steht, solange
                                               AT-DOS laeuft          */
        RBW(0x467) = rbs_entry_off;         /* Einsprung nach Reset  */
        RBW(0x469) = (unsigned)(rb_lin >> 4);
        outb(0x70, 0x8F);                   /* Shutdown-Code 0Ah: BIOS */
        outb(0x71, 0x0A);                   /* springt nach 40:67    */
        kf_rb_hw(rb_lin);
        outb(0x70, 0x8F);
        outb(0x71, 0x00);
        outb(0x70, 0x0D);                   /* NMI wieder an          */
        scratch_map(0, 0xFFFF);
        d = RBW(0x46C) - t0;                /* Zeit in der Box nachholen: */
        ticks += (((unsigned long)d << 3) + ((unsigned long)d << 1) + d) >> 1;
        uptime_s += d / 18;                 /* 55 ms je BIOS-Takt     */
        scratch_map(rb_lin, 0xFFFF);
        r = RBW(30);
        if (r == RB_CALL) {                 /* INT E0h               */
            if ((RBW(0) >> 8) == 1) {       /* AH = 01h: Box beenden */
                r = RB_EXIT;            /* AL = Rueckgabewert    */
                d = RBW(0) & 0xFF;
            } else {
                RBW(0) = 0xFFFF;            /* unbekannt             */
                continue;
            }
        }
        break;
    }
    if (r == RB_SWITCH)
        kbd_switch_req = 1;
    else if (r == RB_EXIT) {
        rb_free();
        if (rb_kind_was == 2)
            r |= d << 8;                    /* Rueckgabewert in AH   */
    }
    return r;
}
