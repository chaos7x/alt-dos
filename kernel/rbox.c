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
                      rbs_inte0_off, rbs_int13_off, rbs_int15_off, rbs_int8_off;
extern unsigned short irq_hcs[16], rb_drvirq;
extern unsigned char rb_slice, cpu_386;
unsigned __far kf_rb_busy(void);
void __far kf_yield(void);
extern unsigned char  bios_pic1, bios_pic2;
extern unsigned short mem_conv_kb, fpu_ok;
extern unsigned short gdtr[3];
extern volatile unsigned long ticks;
extern void rb_reentry(void);
void __far kf_vio_rehw(void);
int __far kf_kbd_get(void);
void __far kf_kbd_flush(void);
unsigned __far kf_kbd_mods(unsigned set);
int __far kf_cur(void);                 /* kernel.c: laufender Task */
int __far kf_cur_ses(void);             /* ... und seine Sitzung    */
#define current     kf_cur()

int rb_task = -1;
static unsigned long rb_lin;
static unsigned rb_img_u, rb_img_n;     /* Abbild: erste Einheit, Anzahl */
static unsigned long rb_img_pos;        /* bisher geladen (Byte)         */
static unsigned char rb_kind;           /* 1 .COM, 2 Abbild              */
static unsigned char rb_quiet;          /* letzter Ausflug war ein Dienst */
static unsigned long rb_pm_t;           /* Kerneltakt bei der Rueckkehr  */
static unsigned rb_ms;                  /* Rest fuer den BIOS-Takt (ms)  */
static unsigned rb_gfx_u;               /* Grafik der Box (257 KB)       */
static unsigned char rb_gfx;            /* ... ist gesichert             */
#define RB_GFX_UNITS ((0x40000UL + 1024) / MEM_UNIT)
int __far rb_gfx_save(unsigned long buf);       /* vgagfx.c */
void __far rb_gfx_load(unsigned long buf);
void __far gfx_textdac(void);

#define RB_SIZE     0x2000UL
#define RB_LO       (0x10000UL / MEM_UNIT)
/* Aufteilung der 8 KB (start.asm): 0 Zustand, 80h Stummel, 600h Text */
#define RB_STUBMAX  0x580               /* Stummel hoechstens 80h-5FFh  */
#define RB_TEXT     0x600               /* Bildschirmtext 80x25          */
#define RB_SAVE     0x1600              /* IVT + BIOS-Daten (500h Byte)  */
#define RB_IVHI     0x1B00              /* IVT 300h-3FFh nach Resets     */
#define RB_GDT      0x1C00              /* GDT fuer INT 15h/87h         */
#define RBB(o)      (((unsigned char __far *)MK_FP(SCRATCH, 0))[o])
#define RBW(o)      (*(unsigned short __far *)MK_FP(SCRATCH, (o)))
#define RBS(o)      (*(unsigned short __far *)MK_FP(SCRATCH2, (o)))
#define RBSB(o)     (((unsigned char __far *)MK_FP(SCRATCH2, 0))[o])

/* Vektoren C0h-FFh (300h-3FFh) fuer den Stummel ablegen: rbs_entry holt
   sie nach jedem Reset zurueck - das AT-BIOS legt dann seinen Stapel nach
   0030:0100, mitten in die IVT (INT E0h!). SCRATCH = rb_lin, SCRATCH2 = 0. */
static void rb_ivt_hi(void)
{
    unsigned i;
    for (i = 0x300; i < 0x400; i += 2)
        RBW(RB_IVHI - 0x300 + i) = RBS(i);
}

/* gemeinsamer Teil: Speicher belegen, Stummel und Zustandsblock, IVT und
   BIOS-Daten sichern */
static unsigned rb_common(void)
{
    unsigned i, hi;
    unsigned char __far *cod = MK_FP(KCODE2, rbs_code);
    unsigned seg;

    if (!kmoved || rb_task >= 0 || rbs_len > RB_STUBMAX)
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
        RBW(RB_TEXT + 2 * i) = 0x0720;
    for (i = 0; i < 0x500; i += 2)          /* zurueck in rb_free    */
        RBW(RB_SAVE + i) = RBS(i);
    for (i = 0; i < 48; i++)
        RBB(RB_GDT + i) = 0;
    RBW(RB_GDT + 16) = RBW(RB_GDT + 24) = 0xFFFF;
    RBB(RB_GDT + 21) = RBB(RB_GDT + 29) = 0x93;
    RBB(76) = fpu_ok ? 1 : 0;               /* Stummel sichert den 287 */
    gfx_textdac();                          /* AT-DOS zeigt noch Text    */
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
    RBW(82) = RBS(8 * 4);                   /* INT 8: Takt-Ausfluege */
    RBW(84) = RBS(8 * 4 + 2);
    RBS(8 * 4) = rbs_int8_off;
    RBS(8 * 4 + 2) = seg;
    RBW(66) = RBS(0x15 * 4);                /* INT 15h: Tasten (4Fh), */
    RBW(68) = RBS(0x15 * 4 + 2);            /* kein erweiterter Speicher */
    RBS(0x15 * 4) = rbs_int15_off;
    RBS(0x15 * 4 + 2) = seg;
    RBS(0x413) = (unsigned)(rb_lin >> 10);
    rb_ivt_hi();
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
    if (rb_gfx_u) {
        mem_free(rb_gfx_u, (unsigned)RB_GFX_UNITS);
        rb_gfx_u = 0;
    }
    rb_gfx = 0;
    scratch_map(rb_lin, 0xFFFF);            /* IVT und BIOS-Daten wie */
    scratch2_map(0, 0xFFFF);                /* vor der Box            */
    for (i = 0; i < 0x46C; i += 2)
        RBS(i) = RBW(RB_SAVE + i);
    for (i = 0x470; i < 0x500; i += 2)      /* ohne den BIOS-Takt     */
        RBS(i) = RBW(RB_SAVE + i);
    rb_task = -1;
    rb_kind = 0;
    rb_quiet = 0;
    rb_pm_t = 0;
    rb_ms = 0;
    if (vio_frozen) {                       /* RBOX mitten im Dienst beendet */
        vio_frozen = 0;
        kf_vio_rehw();
    }
}

/* Bild der Box sichern, wenn sie nach einem Dienst (Stummel sichert dann
   nicht) weggeschaltet wird: Grafik wie bei Alt+Esc, Text nach rb_lin +
   600h wie der Stummel */
static void rb_save_screen(void)
{
    if (!rb_gfx_u)
        rb_gfx_u = mem_alloc((unsigned)RB_GFX_UNITS);
    if (rb_gfx_u && rb_gfx_save((unsigned long)rb_gfx_u * MEM_UNIT)) {
        rb_gfx = 1;
        return;
    }
    scratch_map(rb_lin + RB_TEXT, 3999);
    scratch2_map(0xB8000UL, 3999);
    far_copy(SCRATCH, 0, SCRATCH2, 0, 4000);
}

/* Umschalttasten zwischen AT-DOS (kbd.c) und dem BIOS der Box abgleichen:
   wird eine Taste gedrueckt oder losgelassen, waehrend die andere Seite
   laeuft, saehe die eine sonst Alt/Umschalt ewig gedrueckt bzw. nie (Alt
   in der Box, Esc waehrend eines Ausflugs: kein Alt+Esc). 417h Bit 0/1
   Umschalt, 2 Strg, 3 Alt; 418h Bit 0 Strg links, 1 Alt links; 496h Bit 3
   AltGr (rechte Alt-Taste, RBSHELL fuehrt es auch). SCRATCH2 = 0. Ruft
   kf_rb_hw mit gesperrten Interrupts direkt vor dem Eintritt bzw. nach der
   Rueckkehr - sonst ging eine Alt-Taste dazwischen verloren (Alt+Esc bei
   RBTIME). */
void __far rb_mods_to_box(void)
{
    unsigned m = kf_kbd_mods(0);
    unsigned char a, b, c;
    scratch2_map(0, 0xFFFF);
    a = RBSB(0x417) & 0xF0;
    b = RBSB(0x418) & 0xFC;
    c = RBSB(0x496) & 0xF3;
    if (m & 1) a |= 0x02;
    if (m & 2) { a |= 0x04; b |= 0x01; }
    if (m & 4) { a |= 0x08; b |= 0x02; }
    if (m & 8) { a |= 0x08; c |= 0x08; }
    RBSB(0x417) = a;
    RBSB(0x418) = b;
    RBSB(0x496) = c;
}

void __far rb_mods_from_box(void)
{
    unsigned char a, c;
    unsigned m = 0x8000;
    scratch2_map(0, 0xFFFF);                /* 386-Weg: war Codefenster */
    a = RBSB(0x417);
    c = RBSB(0x496);
    if (a & 3) m |= 1;
    if (a & 4) m |= 2;
    if (c & 8) m |= 8;
    else if (a & 8) m |= 4;
    kf_kbd_mods(m);
}

/* Tasten, die AT-DOS waehrend eines Dienstes bekam, in den Tastaturpuffer
   des BIOS der Box (gleiches Format: Zeichen | Scancode << 8) */
static void rb_keys(void)
{
    int k;
    unsigned t, n;
    unsigned lo, hi;
    scratch2_map(0, 0xFFFF);
    lo = RBS(0x480);                        /* nur bei stimmigen Zeigern - */
    hi = RBS(0x482);                        /* sonst schriebe RBS(400h+t)  */
    if (lo < 0x1E || hi > 0x100 || lo + 2 >= hi)    /* irgendwohin    */
        lo = 0x1E, hi = 0x3E;
    while ((k = kf_kbd_get()) != -1) {
        t = RBS(0x41C);
        if (t < lo || t >= hi || (t & 1))
            break;
        n = t + 2;
        if (n >= hi)
            n = lo;
        if (n == RBS(0x41A))
            break;                          /* Puffer voll            */
        RBS(0x400 + t) = (unsigned)k;
        RBS(0x41C) = n;
    }
}

/* Sekunden des Tages aus der Echtzeituhr: Obergrenze fuer die Zeit in der
   Box - der BIOS-Takt 46Ch taugt allein nicht, FreeDOS stellt ihn beim
   Start selbst (sonst sprang die Uptime um die Zeit seit dem Start von
   AT-DOS). Ohne 32-Bit-Multiplikation. */
static unsigned char cmos_rd(unsigned r)
{
    outb(0x70, 0x80 | r);                   /* NMI bleibt aus (rb_run) */
    return (unsigned char)inb(0x71);
}

static unsigned long rtc_secs(void)
{
    unsigned n = 0, h, m, sec, i;
    unsigned long t = 0;
    unsigned char bin;
    while ((cmos_rd(0x0A) & 0x80) && --n)   /* Uhr wird gerade gestellt */
        ;
    bin = cmos_rd(0x0B) & 4;
    sec = cmos_rd(0x00);
    m = cmos_rd(0x02);
    h = cmos_rd(0x04) & 0x3F;
    if (!bin) {
        sec = (sec >> 4) * 10 + (sec & 15);
        m = (m >> 4) * 10 + (m & 15);
        h = (h >> 4) * 10 + (h & 15);
    }
    for (i = 0; i < h && i < 24; i++)
        t += 3600;
    return t + m * 60 + sec;
}

/* vor jedem Eintritt: (1) Zeit, die AT-DOS seit der letzten Rueckkehr
   lief, auf den BIOS-Takt der Box (sonst ginge ihre Uhr nach; 10 ms je
   Kerneltakt, 54,9 ms je BIOS-Takt); (2) IRQs der AT-DOS-Treiber im PIC
   der Box maskieren - kamen sie dort an, liest kf_rb_hw sie aus dem IRR
   und holt sie nach; (3) Takte je Ausflug fuer den Stummel. */
static void rb_entry_prep(void)
{
    unsigned long dt = rb_pm_t ? get_ticks() - rb_pm_t : 0;
    unsigned n, m = 0;
    unsigned long __far *bt;

    scratch_map(0, 0xFFFF);
    bt = (unsigned long __far *)MK_FP(SCRATCH, 0x46C);
    while (dt) {                            /* in Stuecken: 16 Bit     */
        unsigned k = dt > 6000 ? 6000 : (unsigned)dt;
        dt -= k;
        rb_ms += k * 10;
        while (rb_ms >= 55) {
            rb_ms -= 55;
            if (++*bt >= 0x1800B0UL) {      /* Mitternacht            */
                *bt = 0;
                RBB(0x470) = 1;
            }
        }
    }
    for (n = 3; n < 16; n++)
        if (irq_hcs[n])
            m |= 1 << n;
    rb_drvirq = m;
    scratch_map(rb_lin, 0xFFFF);
    RBB(28) |= (unsigned char)m;
    RBB(29) |= (unsigned char)(m >> 8);
    n = rb_slice == 0xFF ? (cpu_386 ? 2 : 4) : rb_slice;
    if (!kf_rb_busy())                      /* nichts im Hintergrund: die */
        n = 0;                              /* Box bekommt alle Zeit      */
    RBB(80) = (unsigned char)n;             /* Zaehler bei jedem Eintritt */
    RBB(81) = (unsigned char)(n + 1);       /* neu: der erste Takt kommt  */
                                            /* sofort (IRQ 0 wartet)      */
}

static unsigned char rb_way;
#define RB_DISK     5                   /* nur im Kernel: INT 13h lesen  */

/* INT 13h 02h der Box (Grund 5): Sektoren aus dem Abbild nach ES:BX.
   Register aus dem Zustandsblock, Ergebnis dorthin (AX, CF im FLAGS-Wort,
   das der Stummel beim IRET nimmt). Ohne 32-Bit-Multiplikation: CHS einer
   Diskette passt in 16 Bit. */
static void rb_disk(void)
{
    unsigned ax, cx, dx, cyl, sec, lba, cnt, len;
    unsigned long src, dst;

    scratch_map(rb_lin, 0xFFFF);
    ax = RBW(0);
    cx = RBW(4);
    dx = RBW(6);
    cnt = ax & 0xFF;
    cyl = (cx >> 8) | ((cx & 0xC0) << 2);
    sec = cx & 0x3F;
    lba = (cyl * RBW(54) + (dx >> 8)) * RBW(52) + sec - 1;
    dst = ((unsigned long)RBW(16) << 4) + RBW(2);
    if (!cnt || !sec || cnt > 128 || lba + cnt > RBW(60) ||
        dst + ((unsigned long)cnt << 9) > rb_lin) {
        RBW(0) = 0x0400;                    /* Sektor nicht gefunden  */
        RBW(26) |= 1;
        return;
    }
    src = (unsigned long)rb_img_u * MEM_UNIT + ((unsigned long)lba << 9);
    len = cnt << 9;                         /* hoechstens 64 KB - 512 */
    scratch_map(dst, len - 1);
    scratch2_map(src, len - 1);
    far_copy(SCRATCH, 0, SCRATCH2, 0, len);
    scratch_map(rb_lin, 0xFFFF);
    RBW(0) = cnt;                           /* AH = 0: gut            */
    RBW(26) &= ~1u;
}

unsigned __far rb_run(void)
{
    unsigned r, t0, d = 0;
    unsigned long s0, s1;
    unsigned char rb_kind_was = rb_kind;

    if (rb_task != current)
        return FAPI_ERROR;
    if (rb_kind == 2 && rb_img_pos < 512)
        return FAPI_ERROR;
    for (;;) {
        while (vio_fg() != kf_cur_ses()) {
            if (vio_frozen) {               /* waehrend eines Dienstes   */
                rb_save_screen();           /* weggeschaltet: Bild der   */
                vio_frozen = 0;             /* Box sichern (der Stummel  */
                rb_quiet = 0;               /* tat es nicht)            */
                kf_vio_rehw();
            }
            sleep_ticks(5);
        }
        vio_frozen = 1;
        if (rb_gfx) {                       /* Grafik der Box zurueck    */
            rb_gfx_load((unsigned long)rb_gfx_u * MEM_UNIT);
            rb_gfx = 0;
            rb_quiet = 1;                   /* Stummel: Modus nicht neu  */
        }
        scratch_map(rb_lin, 0xFFFF);
        RBB(74) = rb_quiet;                 /* Stummel: Bild steht noch */
        if (rb_quiet)
            rb_keys();
        rb_entry_prep();
        scratch_map(0, 0xFFFF);
        t0 = RBW(0x46C);                    /* BIOS-Takt: steht, solange
                                               AT-DOS laeuft          */
        s0 = rtc_secs();
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
        s1 = rtc_secs();
        s1 = s1 >= s0 ? s1 - s0 : s1 + 86400UL - s0;
        if (s1 < 3600 && d > (unsigned)s1 * 19 + 38)
            d = (unsigned)s1 * 18 + (unsigned)s1 / 5;   /* 18,2 je s   */
        ticks += (((unsigned long)d << 3) + ((unsigned long)d << 1) + d) >> 1;
        uptime_s += d / 18;                 /* 55 ms je BIOS-Takt     */
        scratch_map(rb_lin, 0xFFFF);
        r = RBW(30);
        rb_way = r >> 8;                    /* Fehlersuche: wer setzte r */
        r &= 0xFF;
        rb_pm_t = get_ticks();              /* ab hier laeuft AT-DOS   */
        if (r == RB_DISK) {                 /* INT 13h: Sektoren holen, */
            rb_disk();                      /* gleich weiter in die Box */
            rb_quiet = 1;
            continue;
        }
        if (r == RB_TICK) {                 /* Takt: andere Programme  */
            rb_quiet = 1;                   /* laufen lassen, Bild bleibt */
            kf_yield();
            continue;
        }
        if (r == RB_CALL && (RBW(0) >> 8) == 1) {   /* INT E0h 01h:  */
            r = RB_EXIT;                    /* Box beenden, AL = Wert */
            d = RBW(0) & 0xFF;
        } else if (r == RB_EXIT)
            d = 0xE0 | rb_way;              /* Strg+Alt+Entf (Stummel), */
                                            /* Weg: 1 INT 9, 2 INT 15h, */
                                            /* 3 INT 20h, +4 vertagt    */
        break;
    }
    rb_quiet = r == RB_CALL;                /* Dienst: Bild bleibt der Box */
    if (r == RB_SWITCH) {                   /* Grafik sichern, bevor AT-DOS */
        if (!rb_gfx_u)                      /* die Karte auf Text stellt  */
            rb_gfx_u = mem_alloc((unsigned)RB_GFX_UNITS);
        if (rb_gfx_u)
            rb_gfx = (unsigned char)rb_gfx_save((unsigned long)rb_gfx_u * MEM_UNIT);
    }
    if (!rb_quiet) {
        vio_frozen = 0;
        kf_vio_rehw();
        kf_kbd_flush();
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

/* Register der Box bei RB_CALL (INT E0h) holen/setzen: AX..ES frei, von
   FLAGS nur die Rechenbits (IF/TF/IOPL bleiben) */
unsigned __far rb_regs(unsigned sel, unsigned off, unsigned set)
{
    unsigned short __far *u = MK_FP(sel, off);
    unsigned i;

    if (rb_task != current)
        return FAPI_ERROR;
    scratch_map(rb_lin, 0xFFFF);
    if (set) {
        for (i = 0; i < 9; i++)
            RBW(2 * i) = u[i];
        RBW(26) = (RBW(26) & ~0x0CD5u) | (u[13] & 0x0CD5u);
    } else
        for (i = 0; i < 14; i++)
            u[i] = RBW(2 * i);
    return 0;
}

/* Speicher der Box (0 .. rb_lin) lesen oder schreiben */
unsigned __far rb_mem(unsigned sel, unsigned long lin, unsigned buf,
                      unsigned len, unsigned wr)
{
    unsigned char __far *u = MK_FP(sel, buf);
    unsigned i;

    if (rb_task != current || !len || lin >= rb_lin || len > rb_lin - lin)
        return FAPI_ERROR;
    scratch_map(lin, len - 1);
    if (wr)
        for (i = 0; i < len; i++)
            RBB(i) = u[i];
    else
        for (i = 0; i < len; i++)
            u[i] = RBB(i);
    return 0;
}
