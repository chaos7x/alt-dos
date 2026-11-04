/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - VGA-Grafik der Sitzungen (13h, 12h, Mode X) und der
 * Mauszeiger in der Grafik. Aus vio.c herausgeloest; liegt im zweiten
 * Codesegment (KCODE2, siehe k2.h), Daten und Sitzungen teilt es mit vio.c
 * ueber vioint.h.
 */
#include "kern.h"
#include "mem.h"
#include "vioint.h"
#include "../include/fapi_abi.h"
#include "k2.h"                         /* zuletzt: lenkt Kernelhilfen um */

static void crtc(unsigned i, unsigned v) { outb(0x3D4, i); outb(0x3D5, v); }
static void seqr(unsigned i, unsigned v) { outb(0x3C4, i); outb(0x3C5, v); }
static void gcr(unsigned i, unsigned v) { outb(0x3CE, i); outb(0x3CF, v); }
static unsigned crtc_in(unsigned i) { outb(0x3D4, i); return inb(0x3D5); }
unsigned char gp_drawn;

/* ---- Grafik (VGA, Modus 13h) --------------------------------------------
   Wie VioGetPhysBuf bei OS/2 1.x bekommt das Programm einen Selektor auf
   den Bildspeicher - aber bequemer: Beim Sitzungswechsel sichert der Kernel
   Bild (64 KB) und Palette in den Puffer der Sitzung und biegt den
   Selektor dorthin um; das Programm zeichnet im Hintergrund einfach
   weiter. Zurueck im Vordergrund: Karte umstellen, Bild und Palette
   zurueck, Selektor wieder auf A0000h. Kein BIOS im Protected Mode: die
   Register kommen aus den Standardtabellen der IBM-VGA. */
/* Puffer: 13h 64 KB Bild, 12h vier Ebenen zu 38400 Byte; Palette dahinter */
unsigned char hw_gfx;            /* Karte steht auf: 0 Text, 13h  */
static unsigned char text_dac[64 * 3], text_dac_ok;
/* Misc, Sequenzer 0-4, CRTC 0-24, Grafik 0-8, Attribut 0-20 */
static const unsigned char vga_mode13[61] = { 0x63,
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00 };
static const unsigned char vga_mode03[61] = { 0x67,
    0x03, 0x00, 0x03, 0x00, 0x02,
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E,
    0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B,
    0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00 };
/* 12h: Attributregister direkt durchgeschaltet (Farbe n = DAC-Eintrag n) */
static const unsigned char vga_mode12[61] = { 0xE3,
    0x03, 0x01, 0x0F, 0x00, 0x06,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xEA, 0x0C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x01, 0x00, 0x0F, 0x00, 0x00 };
/* Mode X (Michael Abrash): 13h ohne Chain-4 (SEQ 4 = 06h), CRTC im
   Bytebetrieb (14h = 0, 17h = E3h), 480 Rasterzeilen mit Doppelung =
   240 Zeilen; Punkt (x,y) in Ebene x & 3, Byte y * 80 + x / 4 */
static const unsigned char vga_modex[61] = { 0xE3,
    0x03, 0x01, 0x0F, 0x00, 0x06,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0D, 0x3E, 0x00, 0x41, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xEA, 0xAC, 0xDF, 0x28, 0x00, 0xE7, 0x06, 0xE3, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00 };

/* ---- Grafik ------------------------------------------------------------- */
static void vga_regs(const unsigned char *t)
{
    unsigned i;
    seqr(0x00, 0x01);                       /* Sequenzer anhalten       */
    outb(0x3C2, t[0]);
    for (i = 1; i < 5; i++)
        seqr(i, t[1 + i]);
    seqr(0x00, 0x03);
    crtc(0x11, crtc_in(0x11) & 0x7F);       /* CRTC 0-7 freigeben       */
    for (i = 0; i < 25; i++)
        crtc(i, t[6 + i]);
    for (i = 0; i < 9; i++)
        gcr(i, t[31 + i]);
    inb(0x3DA);                             /* Attribut: Index zuerst   */
    for (i = 0; i < 21; i++) {
        outb(0x3C0, i);
        outb(0x3C0, t[40 + i]);
    }
    outb(0x3C0, 0x20);                      /* Bild an                  */
}

/* Mode X: Bildanfang (CRTC 0Ch/0Dh, Bytes je Ebene) */
static void set_start(unsigned a)
{
    crtc(0x0C, a >> 8);
    crtc(0x0D, a & 0xFF);
}

static void dac_io(int write, unsigned first, unsigned n, unsigned char __far *p)
{
    unsigned i;
    outb(write ? 0x3C8 : 0x3C7, first);
    for (i = 0; i < n * 3; i++)
        if (write)
            outb(0x3C9, p[i]);
        else
            p[i] = (unsigned char)inb(0x3C9);
}

/* Fenster SCRATCH auf den Puffer: part 0-3 = Bild (13h: nur 0, 64 KB;
   12h: Ebene part), part 4 = Palette */
/* VESA: der Kernel stellt den Modus ueber das Video-BIOS ein (vesa.c,
   kurzer Real-Mode-Aufruf); den linearen Bildspeicher bildet paging.c
   pg_lfb in seine Fenster-Seitentabelle ab (vesa_lin = Basis fuer
   Deskriptoren). Das Programm sieht ihn ueber einen 64-KB-Selektor, den
   VioSetPlane(n) auf den n-ten Abschnitt ("Bank") legt - vorn auf den
   Bildspeicher, hinten auf den Puffer (Bild in vpg Seiten, Palette
   dahinter). Steht die Karte in einem VESA-Modus (vesa_hw), stellt der
   Kernel vor jedem anderen Modus mit dem BIOS erst Modus 3 ein - die
   SVGA-Register kennt er nicht. */
static unsigned long vesa_lin;          /* Bildspeicher fuer Deskriptoren */
static unsigned long vesa_phys;         /* ... physisch                  */
static unsigned char vesa_hw;           /* Karte steht auf VESA          */
int __far vesa_bios(unsigned mode);     /* vesa.c                        */

static unsigned vesa_banks(int s)
{
    return (ses[s].vpg + 15) >> 4;
}

static unsigned vesa_limit(int s)       /* Grenze der gewaehlten Bank   */
{
    unsigned r = ses[s].vpg - (ses[s].gfx_plane << 4);
    return r >= 16 ? 0xFFFF : (r << 12) - 1;
}

static unsigned long save_part(int s, int part)
{
    unsigned long b = PG_LIN(ses[s].gfx_save);
    if (ses[s].gfx == VESAGFX)
        return part == 4 ? b + PG_LIN(ses[s].vpg) :
               b + ((unsigned long)ses[s].gfx_plane << 16);
    if (part == 4)
        return b + GFX_IMG(ses[s].gfx);
    if (PLANAR(ses[s].gfx))
        while (part-- > 0)              /* ohne 32-Bit-Multiplikation   */
            b += PLANE12;
    return b;
}

static void map_save(int s, int part)
{
    scratch_map(save_part(s, part), part == 4 ? 767 :
                PLANAR(ses[s].gfx) ? PLANE12 - 1 : 0xFFFF);
}

/* VESA: Bild zwischen Bildspeicher und Puffer, Bank fuer Bank */
static void vesa_copy(int s, int out)
{
    unsigned b, n = vesa_banks(s), rest;
    unsigned long base = PG_LIN(ses[s].gfx_save);
    for (b = 0; b < n; b++) {
        rest = b + 1 < n ? 16 : ses[s].vpg - (b << 4);  /* Seiten       */
        scratch_map(base + ((unsigned long)b << 16), 0xFFFF);
        set_desc(SCRATCH2 >> 3, vesa_lin + ((unsigned long)b << 16), 0xFFFF, ACC_KDATA);
        if (out)
            far_copy(SCRATCH, 0, SCRATCH2, 0, (rest > 8 ? 8 : rest) << 12);
        else
            far_copy(SCRATCH2, 0, SCRATCH, 0, (rest > 8 ? 8 : rest) << 12);
        if (rest > 8) {
            if (out)
                far_copy(SCRATCH, 0x8000, SCRATCH2, 0x8000, (rest - 8) << 12);
            else
                far_copy(SCRATCH2, 0x8000, SCRATCH, 0x8000, (rest - 8) << 12);
        }
    }
}

/* Bild zwischen Bildspeicher und Puffer kopieren; out = 1: sichern */
static void gfx_copy(int s, int out)
{
    unsigned p;
    scratch2_map(0xA0000UL, 0xFFFF);
    if (ses[s].gfx == 0x13) {
        map_save(s, 0);
        if (out) {
            far_copy(SCRATCH, 0, SCRATCH2, 0, 0x8000);
            far_copy(SCRATCH, 0x8000, SCRATCH2, 0x8000, 0x8000);
        } else {
            far_copy(SCRATCH2, 0, SCRATCH, 0, 0x8000);
            far_copy(SCRATCH2, 0x8000, SCRATCH, 0x8000, 0x8000);
        }
        return;
    }
    for (p = 0; p < 4; p++) {               /* 12h/X: Ebene fuer Ebene  */
        map_save(s, (int)p);
        if (out) {
            gcr(0x04, p);                   /* Ebene p lesen            */
            far_copy(SCRATCH, 0, SCRATCH2, 0, PLANE12);
        } else {
            seqr(0x02, 1 << p);             /* nur Ebene p schreiben    */
            far_copy(SCRATCH2, 0, SCRATCH, 0, PLANE12);
        }
    }
}

/* Selektor des Programms: vorn Bildspeicher, sonst Puffer (12h: Ebene) */
static void gfx_alias(int s)
{
    if (!ses[s].gfx_sel)
        return;
    if (ses[s].gfx == VESAGFX && s == fg && hw_gfx == VESAGFX)
        ldt_set(ses[s].gfx_task, ses[s].gfx_sel,
                vesa_lin + ((unsigned long)ses[s].gfx_plane << 16), vesa_limit(s));
    else if (ses[s].gfx == VESAGFX)
        ldt_set(ses[s].gfx_task, ses[s].gfx_sel, save_part(s, 0), vesa_limit(s));
    else if (s == fg && hw_gfx == ses[s].gfx)
        ldt_set(ses[s].gfx_task, ses[s].gfx_sel, 0xA0000UL,
                PLANAR(ses[s].gfx) ? PLANE12 - 1 : 0xFFFF);
    else
        ldt_set(ses[s].gfx_task, ses[s].gfx_sel, save_part(s, ses[s].gfx_plane),
                PLANAR(ses[s].gfx) ? PLANE12 - 1 : 0xFFFF);
}

/* Karte auf Text zurueck: Register, Palette, Zeichensatz (hw_lines = 0
   zwingt set_hw_lines zum Neuprogrammieren - 13h belegt auch Ebene 2) */
static void hw_text(void)
{
    gp_drawn = 0;
    vga_regs(vga_mode03);
    if (text_dac_ok)
        dac_io(1, 0, 64, (unsigned char __far *)text_dac);
    hw_lines = 0;
    hw_gfx = 0;
}


/* ---- Rohmodus (DOS-Boxen mit Grafik, virtuelle VGA Stufe 1) --------------
   Die Box stellt den Modus mit dem echten Video-BIOS ein und schreibt
   direkt in Bildspeicher und Register (Ports 3B0h-3DFh frei, A0000-BFFFF
   physisch). Beim Wechsel nach hinten sichert der Kernel alles: Misc,
   Sequenzer, CRTC, Grafik, Attribut (gleiche Folge wie die Tabellen,
   61 Byte), DAC und die vier Ebenen zu je 64 KB; die Box steht solange
   (v86.c friert sie ein). gfx_plane = 1: frisch, die Karte hat noch nichts
   von der Box. */
static unsigned char raw_t[61];

static void raw_regs_in(void)
{
    unsigned i;
    raw_t[0] = (unsigned char)inb(0x3CC);
    for (i = 0; i < 5; i++) {
        outb(0x3C4, i);
        raw_t[1 + i] = (unsigned char)inb(0x3C5);
    }
    for (i = 0; i < 25; i++)
        raw_t[6 + i] = (unsigned char)crtc_in(i);
    for (i = 0; i < 9; i++) {
        outb(0x3CE, i);
        raw_t[31 + i] = (unsigned char)inb(0x3CF);
    }
    for (i = 0; i < 21; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        raw_t[40 + i] = (unsigned char)inb(0x3C1);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);
}

/* Ebenen einzeln ansprechbar: A0000 64 KB, ohne Ketten, Schreibmodus 0 */
static void raw_flat(void)
{
    seqr(0x04, 0x06);
    gcr(0x01, 0x00);
    gcr(0x03, 0x00);
    gcr(0x05, 0x00);
    gcr(0x06, 0x05);
    gcr(0x08, 0xFF);
}

static void raw_copy(int s, int out)
{
    unsigned long b = PG_LIN(ses[s].gfx_save);
    unsigned p;
    raw_flat();
    scratch2_map(0xA0000UL, 0xFFFF);
    for (p = 0; p < 4; p++, b += 0x10000UL) {
        scratch_map(b, 0xFFFF);
        if (out) {
            gcr(0x04, p);
            far_copy(SCRATCH, 0, SCRATCH2, 0, 0x8000);
            far_copy(SCRATCH, 0x8000, SCRATCH2, 0x8000, 0x8000);
        } else {
            seqr(0x02, 1 << p);
            far_copy(SCRATCH2, 0, SCRATCH, 0, 0x8000);
            far_copy(SCRATCH2, 0x8000, SCRATCH, 0x8000, 0x8000);
        }
    }
}

/* Schatten fuer den Hintergrund (v86.c laesst die Box darauf weiterlaufen):
   nur, wenn der BIOS-Modus es erlaubt (13h, CGA 4-6) und die Register
   einfach sind - Chain-4 bzw. B8000, Schreibmodus 0 ohne Set/Reset,
   Drehen, Verknuepfung, Bitmaske FFh, Lesemodus 0. Dann ist das Bild,
   wie die CPU es sieht, ein Stueck linearer Speicher. */
static unsigned raw_kind(unsigned allow)
{
    if ((allow == 1 || allow == 2) && !(raw_t[5] & 0x08) && (raw_t[5] & 0x04) &&
        !(raw_t[36] & 0x10) && ((raw_t[37] >> 2) & 3) <= 1)
        return 2;                           /* planar (12h, EGA, Mode X): */
                                            /* v86.c blendet die Ebene der
                                               Map Mask aus dem Puffer ein */
    if (raw_t[32] || raw_t[34] || (raw_t[36] & 0x0B) || raw_t[39] != 0xFF)
        return 0;
    if (allow == 1 && (raw_t[5] & 0x08) && ((raw_t[37] >> 2) & 3) <= 1)
        return 1;
    if (allow == 3 && ((raw_t[37] >> 2) & 3) == 3 && !(raw_t[35] & 2))
        return 3;
    return 0;
}

/* Sicht der CPU zwischen Bildspeicher und Schatten kopieren */
static void raw_lin(int s, unsigned kind, int out)
{
    unsigned n = kind == 1 ? 0x8000 : 0x4000;   /* zwei Haelften (2*n */
    scratch_map(PG_LIN(ses[s].gfx_save) + RAW_LIN, 0xFFFF);   /* waere 0) */
    scratch2_map(kind == 1 ? 0xA0000UL : 0xB8000UL, 0xFFFF);
    if (out) {
        far_copy(SCRATCH, 0, SCRATCH2, 0, n);
        far_copy(SCRATCH, n, SCRATCH2, n, n);
    } else {
        far_copy(SCRATCH2, 0, SCRATCH, 0, n);
        far_copy(SCRATCH2, n, SCRATCH, n, n);
    }
}

static void raw_out(int s)
{
    unsigned i, k;
    unsigned char __far *q;
    raw_regs_in();
    scratch_map(PG_LIN(ses[s].gfx_save) + RAW_REGS, 1023);
    q = MK_FP(SCRATCH, 0);
    for (i = 0; i < 61; i++)
        q[i] = raw_t[i];
    dac_io(0, 0, 256, q + 64);
    k = raw_kind(q[RAW_ALLOW]);
    q[RAW_KIND] = (unsigned char)k;
    for (i = 0; i < 8; i++)
        q[RAW_ST + i] = 0;
    if (k & 1)
        raw_lin(s, k, 1);                   /* vor raw_flat (Register!)  */
    raw_copy(s, 1);
}

static void raw_in(int s)
{
    unsigned i, k;
    unsigned char __far *q;
    raw_copy(s, 0);
    scratch_map(PG_LIN(ses[s].gfx_save) + RAW_REGS, 1023);
    q = MK_FP(SCRATCH, 0);
    for (i = 0; i < 61; i++)
        raw_t[i] = q[i];
    k = q[RAW_KIND];
    dac_io(1, 0, 256, q + 64);
    vga_regs(raw_t);
    if (k & 1) {                            /* was die Box hinten schrieb
                                               (planar: steht schon in den
                                               Ebenen, raw_copy) */
        seqr(0x02, k == 1 ? 0x0F : 0x03);
        raw_lin(s, k, 0);
        seqr(0x02, raw_t[3]);
    }
}

/* fuer v86.c: Puffer einer Sitzung im Rohmodus (0 = keiner) */
unsigned long __far vio_raw_buf(int s)
{
    if (s < 0 || ses[s].gfx != RAWGFX || !ses[s].gfx_save)
        return 0;
    return PG_LIN(ses[s].gfx_save);
}

/* Grafiksitzung kommt nach vorn: Karte umstellen, Bild und Palette aus
   dem Puffer, Selektor auf den Bildspeicher. -> 1 = Grafik vorn */
int __far gfx_in(void)
{
    unsigned short w[8];
    if (vesa_hw && (fg < 0 || ses[fg].gfx != VESAGFX)) {
        vesa_bios(3);                       /* SVGA-Register ueber das BIOS */
        vesa_hw = 0;                        /* zuruecksetzen            */
        hw_gfx = 0xFF;
    }
    if (fg < 0 || !ses[fg].gfx) {
        if (hw_gfx)
            hw_text();                      /* Textsitzung vorn         */
        return 0;
    }
    win_save(w);
    if (!text_dac_ok) {                     /* Textpalette einmal merken */
        dac_io(0, 0, 64, (unsigned char __far *)text_dac);
        text_dac_ok = 1;
    }
    if (ses[fg].gfx == VESAGFX) {           /* VESA: BIOS, dann Bild     */
        if (vesa_bios(ses[fg].vmode | 0x4000)) {
            win_restore(w);
            return 0;
        }
        vesa_hw = 1;
        vesa_lin = pg_lfb(vesa_phys, ses[fg].vpg);
        hw_gfx = VESAGFX;
        gp_drawn = 0;
        vesa_copy(fg, 0);
        map_save(fg, 4);
        dac_io(1, 0, 256, MK_FP(SCRATCH, 0));
        win_restore(w);
        gfx_alias(fg);
        return 1;
    }
    if (ses[fg].gfx == RAWGFX) {            /* DOS-Box: alles zurueck    */
        if (!ses[fg].gfx_plane)
            raw_in(fg);
        hw_gfx = RAWGFX;
        gp_drawn = 0;
        win_restore(w);
        return 1;
    }
    vga_regs(ses[fg].gfx == 0x12 ? vga_mode12 : ses[fg].gfx == MODEX ? vga_modex
                                                                     : vga_mode13);
    hw_gfx = ses[fg].gfx;
    gp_drawn = 0;                           /* Bild kommt aus dem Puffer */
    gfx_copy(fg, 0);
    if (PLANAR(hw_gfx)) {                   /* Ebene des Programms      */
        seqr(0x02, 1 << ses[fg].gfx_plane);
        gcr(0x04, ses[fg].gfx_plane);
    }
    if (hw_gfx == MODEX)
        set_start(ses[fg].gfx_page ? PAGEX : 0);
    map_save(fg, 4);
    dac_io(1, 0, 256, MK_FP(SCRATCH, 0));
    win_restore(w);
    gfx_alias(fg);
    return 1;
}

/* Grafiksitzung geht nach hinten: Bild in den Puffer, Selektor dorthin */
void __far gfx_out(void)
{
    unsigned short w[8];
    if (fg < 0 || !ses[fg].gfx || hw_gfx != ses[fg].gfx)
        return;
    gptr_remove();                          /* Zeiger nicht mitsichern  */
    win_save(w);
    if (hw_gfx == RAWGFX) {
        raw_out(fg);
        ses[fg].gfx_plane = 0;
    } else if (hw_gfx == VESAGFX) {
        vesa_copy(fg, 1);
        map_save(fg, 4);
        dac_io(0, 0, 256, MK_FP(SCRATCH, 0));
    } else
        gfx_copy(fg, 1);
    win_restore(w);
    hw_gfx = 0xFF;                          /* Karte haelt nichts mehr  */
    gfx_alias(fg);
}

/* Vorgabepalette (Werte 0-63): 16 EGA-Farben, 6x6x6-Wuerfel, Graustufen */
static void def_palette(unsigned char __far *p)
{
    static const unsigned char ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63} };
    unsigned i, r, g, b, k = 0;
    for (i = 0; i < 16; i++) {
        p[k++] = ega[i][0]; p[k++] = ega[i][1]; p[k++] = ega[i][2];
    }
    for (r = 0; r < 6; r++)
        for (g = 0; g < 6; g++)
            for (b = 0; b < 6; b++) {
                p[k++] = (unsigned char)(r * 63 / 5);
                p[k++] = (unsigned char)(g * 63 / 5);
                p[k++] = (unsigned char)(b * 63 / 5);
            }
    for (i = 0; i < 24; i++) {
        g = i * 63 / 23;
        p[k++] = (unsigned char)g;
        p[k++] = (unsigned char)g;
        p[k++] = (unsigned char)g;
    }
}

/* VioSetGfx: mode 13h = Grafik, 0 = zurueck auf Text. 0 = ok, -1 Fehler */
int __far vio_setgfx(int s, int t, unsigned mode)
{
    unsigned short w[8];
    if (s < 0 || !ses[s].used)
        return -1;
    if (mode == 0) {
        if (!ses[s].gfx)
            return 0;
        if (ses[s].gfx_sel)
            ldt_drop(ses[s].gfx_task, ses[s].gfx_sel);
        ses[s].gfx_sel = 0;
        mem_free(ses[s].gfx_save, GFX_UNITS_S(s));
        ses[s].gfx = 0;
        ses[s].gfx_save = 0;
        if (s == fg)
            redraw();                       /* Karte auf Text, Bild neu */
        return 0;
    }
    if ((mode != 0x13 && !PLANAR(mode) && mode != RAWGFX) || !vga_ok)
        return -1;
    if (ses[s].gfx && ses[s].gfx != mode)   /* anderer Modus: neu       */
        vio_setgfx(s, t, 0);
    if (!ses[s].gfx) {
        ses[s].gfx_save = mem_alloc(GFX_UNITS(mode));
        if (!ses[s].gfx_save)
            return -1;
    }
    ses[s].gfx = (unsigned char)mode;
    ses[s].gfx_plane = 0;
    ses[s].gfx_page = 0;
    if (mode == RAWGFX) {                   /* Inhalt kommt vom BIOS    */
        win_save(w);
        scratch_map(PG_LIN(ses[s].gfx_save) + RAW_REGS, 1023);
        *(unsigned char __far *)MK_FP(SCRATCH, RAW_KIND) = 0;
        *(unsigned char __far *)MK_FP(SCRATCH, RAW_ALLOW) = 0;
        win_restore(w);
        ses[s].gfx_plane = 1;               /* frisch                   */
        ses[s].gfx_task = (signed char)t;
        if (s == fg) {
            hw_gfx = 0xFF;
            redraw();
        }
        return 0;
    }
    win_save(w);                            /* Puffer: schwarz, Palette */
    if (mode == 0x13) {
        map_save(s, 0);
        zero_seg(SCRATCH, 0x8000);
    } else {
        unsigned p;
        for (p = 0; p < 4; p++) {
            map_save(s, (int)p);
            zero_seg(SCRATCH, PLANE12 / 2);
        }
    }
    map_save(s, 4);
    def_palette(MK_FP(SCRATCH, 0));
    win_restore(w);
    ses[s].gfx_task = (signed char)t;
    if (s == fg) {
        hw_gfx = 0xFF;                      /* neu einstellen           */
        redraw();
    }
    return 0;
}

/* VESA-Modus mode (Bild pages Seiten, Bildspeicher physisch phys) fuer
   Sitzung s: Puffer anlegen (schwarz, Vorgabepalette); vorn stellt
   redraw -> gfx_in das BIOS um. 0 = ok, -1 Fehler (kein Speicher) */
int __far vio_setvesa(int s, int t, unsigned mode, unsigned pages, unsigned long phys)
{
    unsigned short w[8];
    unsigned b;
    if (s < 0 || !ses[s].used || !vga_ok)
        return -1;
    if (ses[s].gfx)
        vio_setgfx(s, t, 0);
    ses[s].gfx_save = mem_alloc(pages + 1);
    if (!ses[s].gfx_save)
        return -1;
    ses[s].gfx = VESAGFX;
    ses[s].vmode = mode;
    ses[s].vpg = pages;
    ses[s].gfx_plane = 0;
    ses[s].gfx_page = 0;
    vesa_phys = phys;
    win_save(w);
    for (b = 0; b < vesa_banks(s); b++) {
        scratch_map(PG_LIN(ses[s].gfx_save) + ((unsigned long)b << 16), 0xFFFF);
        zero_seg(SCRATCH, b + 1 < vesa_banks(s) ? 0x8000 : (pages - (b << 4)) << 11);
    }
    map_save(s, 4);
    def_palette(MK_FP(SCRATCH, 0));
    win_restore(w);
    ses[s].gfx_task = (signed char)t;
    if (s == fg) {
        hw_gfx = 0xFF;
        redraw();
    }
    return 0;
}

/* VioGetPhysBuf: Basis des Bildes (vorn A0000h, sonst Puffer); merkt den
   Selektor, damit der Kernel ihn beim Wechsel umbiegen kann */
unsigned long __far vio_gfx_base(int s)
{
    if (ses[s].gfx == VESAGFX && s == fg && hw_gfx == VESAGFX)
        return vesa_lin + ((unsigned long)ses[s].gfx_plane << 16);
    if (s == fg && hw_gfx == ses[s].gfx)
        return 0xA0000UL;
    return save_part(s, ses[s].gfx_plane);
}

unsigned __far vio_gfx_limit(int s)
{
    if (ses[s].gfx == VESAGFX)
        return vesa_limit(s);
    return PLANAR(ses[s].gfx) ? PLANE12 - 1 : 0xFFFF;
}

/* VioSetPlane (12h): Ebene p (0-3) zum Schreiben und Lesen */
int __far vio_setplane(int s, unsigned p)
{
    if (s >= 0 && ses[s].used && ses[s].gfx == VESAGFX) {   /* Bank */
        if (p >= vesa_banks(s))
            return -1;
        ses[s].gfx_plane = (unsigned char)p;
        gfx_alias(s);
        return 0;
    }
    if (s < 0 || !ses[s].used || !PLANAR(ses[s].gfx) || p > 3)
        return -1;
    ses[s].gfx_plane = (unsigned char)p;
    if (s == fg && hw_gfx == ses[s].gfx) {
        seqr(0x02, 1 << p);
        gcr(0x04, p);
    } else
        gfx_alias(s);                       /* Hintergrund: Puffer-Ebene */
    return 0;
}

int __far vio_gfx_reg(int s, int t, unsigned sel)
{
    if (s < 0 || !ses[s].used || !ses[s].gfx)
        return -1;
    if (ses[s].gfx_sel)                     /* nur einer je Sitzung     */
        ldt_drop(ses[s].gfx_task, ses[s].gfx_sel);
    ses[s].gfx_task = (signed char)t;
    ses[s].gfx_sel = sel;
    return 0;
}

int __far vio_is_gfx(int s)
{
    return s >= 0 && ses[s].used && ses[s].gfx;
}

/* VioSetPalette: n Eintraege ab first (je 3 Byte, 0-63) aus sel:off */
void __far vio_setpal(int s, unsigned first, unsigned n, unsigned sel, unsigned off)
{
    unsigned short w[8];
    if (!ses[s].gfx || first > 255 || n == 0 || n > 256 - first)
        return;
    win_save(w);
    map_save(s, 4);
    far_copy(SCRATCH, first * 3, sel, off, n * 3);
    if (s == fg && hw_gfx == ses[s].gfx)
        dac_io(1, first, n, MK_FP(sel, off));
    win_restore(w);
}

/* ---- Mauszeiger in der Grafik ---------------------------------------------
   Pfeil 10x16, weiss mit schwarzem Rand (Bit 15 = linke Spalte). Das
   Programm schreibt jederzeit selbst in den Bildspeicher: beim Entfernen
   kommt nur zurueck, was noch genau so aussieht, wie der Kernel es
   gezeichnet hat - darueber Gemaltes bleibt (wie beim Textzeiger).
   Gegen Flackern (auf dem 286 sichtbar): Entfernen und neu Zeichnen in
   einem Durchgang ueber beide Flaechen - jeder Punkt wird hoechstens
   einmal geschrieben, der Zeiger ist nie weg. Zwei Zustaende (alt/neu)
   im Wechsel; Mode X malt beim Seitenwechsel erst auf die neue Seite. */
static const unsigned short gptr_w[16] = {
    0x0000, 0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00,
    0x7F00, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000 };
static const unsigned short gptr_b[16] = {
    0x8000, 0xC000, 0xA000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100,
    0x8080, 0x83C0, 0x9200, 0xA900, 0xC900, 0x8480, 0x0480, 0x0300 };
struct gpst {
    int x, y;
    unsigned page;                      /* Mode X: Anfang der Seite     */
    unsigned char mode;
    unsigned char save[16][12];         /* 13h/X: Punkte; 12h: 3 Byte x 4 Ebenen */
};
static struct gpst gst[2];
static unsigned char gci;               /* gst[gci] ist gezeichnet      */
static unsigned long gp_check;          /* letzte Pruefung (Takte)      */

/* Bytemaske des Pfeils fuer Byte i (0-2) einer Zeile, Verschiebung sh */
static unsigned char gmask(unsigned short m, unsigned i, unsigned sh)
{
    return (unsigned char)(i == 0 ? m >> (8 + sh) : i == 1 ? (m >> sh) & 0xFF
                                                          : (m << (8 - sh)) & 0xFF);
}

/* Farbe des Pfeils an (x,y): 15, 0 oder -1 (nicht Teil des Pfeils) */
static int gpix(const struct gpst *g, int x, int y)
{
    unsigned r, c;
    if (!g || x < g->x || y < g->y || x >= g->x + 10 || y >= g->y + 16)
        return -1;
    r = (unsigned)(y - g->y);
    c = (unsigned)(x - g->x);
    if (gptr_w[r] & (0x8000 >> c))
        return 15;
    if (gptr_b[r] & (0x8000 >> c))
        return 0;
    return -1;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* 13h/Mode X: o entfernen und n zeichnen (je 0 = entfaellt), Punkt fuer
   Punkt ueber beide Flaechen */
static void gcomb_px(const struct gpst *o, struct gpst *n)
{
    unsigned char __far *v = MK_FP(SCRATCH2, 0);
    const struct gpst *g = n ? n : o;
    int x, y, oc, nc, x0, x1, y0, y1, w = 320, h = g->mode == MODEX ? 240 : 200;
    unsigned a;
    unsigned char cur, bg, val;

    x0 = o && n ? imin(o->x, n->x) : g->x;
    y0 = o && n ? imin(o->y, n->y) : g->y;
    x1 = imin((o && n ? imax(o->x, n->x) : g->x) + 10, w);
    y1 = imin((o && n ? imax(o->y, n->y) : g->y) + 16, h);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            oc = gpix(o, x, y);
            nc = gpix(n, x, y);
            if (oc < 0 && nc < 0)
                continue;
            if (g->mode == MODEX) {         /* Ebene x & 3              */
                a = (unsigned)y * 80 + ((unsigned)x >> 2) + g->page;
                seqr(0x02, 1 << (x & 3));
                gcr(0x04, x & 3);
            } else
                a = (unsigned)y * 320 + (unsigned)x;
            cur = v[a];
            bg = oc >= 0 && cur == (unsigned char)oc ? o->save[y - o->y][x - o->x] : cur;
            if (nc >= 0) {
                n->save[y - n->y][x - n->x] = bg;
                val = (unsigned char)nc;
            } else
                val = bg;
            if (val != cur)
                v[a] = val;
        }
}

/* Masken des Pfeils g fuer Byte bx in Zeile y (12h); -> 0 = nicht dabei */
static int gbyte(const struct gpst *g, int y, int bx, unsigned char *wm,
                 unsigned char *bm, unsigned *r, unsigned *i)
{
    int b0;
    *wm = *bm = 0;
    if (!g || y < g->y || y >= g->y + 16)
        return 0;
    b0 = g->x >> 3;
    if (bx < b0 || bx > b0 + 2)
        return 0;
    *r = (unsigned)(y - g->y);
    *i = (unsigned)(bx - b0);
    *wm = gmask(gptr_w[*r], *i, (unsigned)g->x & 7);
    *bm = gmask(gptr_b[*r], *i, (unsigned)g->x & 7);
    return (*wm | *bm) != 0;
}

/* 12h: dasselbe Byte fuer Byte, je Ebene ein Schreibzugriff */
static void gcomb_12(const struct gpst *o, struct gpst *n)
{
    unsigned char __far *v = MK_FP(SCRATCH2, 0);
    volatile unsigned char __far *vv = MK_FP(SCRATCH2, 0);
    const struct gpst *g = n ? n : o;
    int y, bx, y0, y1, b0, b1, ho, hn;
    unsigned a, pl, orow = 0, oi = 0, nrow = 0, ni = 0;
    unsigned char owm, obm, nwm, nbm, cur[4], an, orr, ko, bg, val;

    y0 = o && n ? imin(o->y, n->y) : g->y;
    y1 = imin((o && n ? imax(o->y, n->y) : g->y) + 16, 480);
    b0 = o && n ? imin(o->x, n->x) >> 3 : g->x >> 3;
    b1 = imin(((o && n ? imax(o->x, n->x) : g->x) >> 3) + 3, 80);
    gcr(0x08, 0xFF);
    for (y = y0; y < y1; y++)
        for (bx = b0; bx < b1; bx++) {
            ho = gbyte(o, y, bx, &owm, &obm, &orow, &oi);
            hn = gbyte(n, y, bx, &nwm, &nbm, &nrow, &ni);
            if (!ho && !hn)
                continue;
            a = (unsigned)y * 80 + (unsigned)bx;
            for (an = 0xFF, orr = 0, pl = 0; pl < 4; pl++) {
                gcr(0x04, pl);
                cur[pl] = vv[a];
                an &= cur[pl];
                orr |= cur[pl];
            }
            /* noch unveraenderte Punkte des alten Pfeils: weiss = alle
               Ebenen 1, schwarz = alle 0 */
            ko = (unsigned char)((owm & an) | (obm & ~orr));
            for (pl = 0; pl < 4; pl++) {
                bg = ko ? (unsigned char)((cur[pl] & ~ko) | (o->save[orow][oi * 4 + pl] & ko))
                        : cur[pl];
                if (hn) {
                    n->save[nrow][ni * 4 + pl] = bg;
                    val = (unsigned char)((bg & ~(nwm | nbm)) | nwm);
                } else
                    val = bg;
                if (val != cur[pl]) {
                    seqr(0x02, 1 << pl);
                    v[a] = val;
                }
            }
        }
}

/* o entfernen, n zeichnen (0 = entfaellt); sich nicht ueberlappende
   Flaechen nacheinander (erst neu, dann alt) */
static void gcomb(const struct gpst *o, struct gpst *n)
{
    unsigned short w[8];
    unsigned char m = n ? n->mode : o->mode;

    if (o && n && (o->page != n->page || o->x >= n->x + 16 || n->x >= o->x + 16 ||
                   o->y >= n->y + 16 || n->y >= o->y + 16)) {
        gcomb(0, n);
        gcomb(o, 0);
        return;
    }
    win_save(w);
    scratch2_map(0xA0000UL, 0xFFFF);
    if (m == 0x12)
        gcomb_12(o, n);
    else
        gcomb_px(o, n);
    if (PLANAR(m) && fg >= 0) {             /* Ebene des Programms      */
        seqr(0x02, 1 << ses[fg].gfx_plane);
        gcr(0x04, ses[fg].gfx_plane);
    }
    win_restore(w);
}

void __far gptr_remove(void)
{
    if (gp_drawn) {
        if (hw_gfx == gst[gci].mode)
            gcomb(&gst[gci], 0);
        gp_drawn = 0;
    }
}

/* Zeiger nach (x,y) auf Seite page; alter wird im selben Zug entfernt */
static void gptr_to(int x, int y, unsigned page)
{
    struct gpst *n = &gst[gci ^ 1];
    n->x = x;
    n->y = y;
    n->page = page;
    n->mode = hw_gfx;
    gcomb(gp_drawn && gst[gci].mode == hw_gfx ? &gst[gci] : 0, n);
    gci ^= 1;
    gp_drawn = 1;
}

static unsigned gfx_pagestart(void)
{
    return hw_gfx == MODEX && ses[fg].gfx_page ? PAGEX : 0;
}

/* Zeiger nachfuehren. Gezeichnet wird moeglichst im Strahlruecklauf
   (3DAh Bit 3), sonst kreuzt der Strahl auf dem 286 den halb gezeichneten
   Zeiger und er flackert: idle = 1 (alle Tasks warten) wartet auf den
   Beginn des Ruecklaufs (hoechstens 2 Takte); sonst nur, wenn gerade
   Ruecklauf ist oder die Aenderung schon 30 ms wartet. */
static unsigned long gp_pend;           /* Aenderung wartet seit        */
static unsigned char gp_pending;

static int gptr_vsync(int idle)
{
    unsigned long t = get_ticks();
    if (!gp_pending) {
        gp_pending = 1;
        gp_pend = t;
    }
    if (idle) {
        while ((inb(0x3DA) & 0x08) && get_ticks() - t < 2)
            ;
        while (!(inb(0x3DA) & 0x08) && get_ticks() - t < 2)
            ;
        return 1;
    }
    return (inb(0x3DA) & 0x08) || t - gp_pend >= 3;
}

static void gptr_update_v(int idle)
{
    int show = fg >= 0 && ses[fg].gfx && hw_gfx == ses[fg].gfx && hw_gfx != VESAGFX && mou_nbtn &&
               ses[fg].mou_owner >= 0 && ses[fg].mou_show;
    int x, y;
    struct gpst *g = &gst[gci];

    if (!show) {
        gptr_remove();
        return;
    }
    x = hw_gfx == 0x13 || hw_gfx == MODEX ? mx >> 1 : mx;
    y = hw_gfx == 0x13 || hw_gfx == MODEX ? my >> 1 : my;
    if ((!gp_drawn || x != g->x || y != g->y || g->mode != hw_gfx ||
         g->page != gfx_pagestart() || get_ticks() - gp_check >= 5) &&
        gptr_vsync(idle)) {
        gp_pending = 0;
        /* auch ohne Bewegung alle 50 ms: hat das Programm ueber den
           Zeiger gemalt, wird das neuer Hintergrund und der Zeiger
           kommt zurueck (geschrieben wird nur, was fehlt) */
        gp_check = get_ticks();
        gptr_to(x, y, gfx_pagestart());
    }
}

void __far gptr_update(void)
{
    gptr_update_v(0);
}

/* Leerlauf des Schedulers (vor HLT): Zeiger im Strahlruecklauf zeichnen */
void __far vio_idle(void)
{
    if (fg >= 0 && ses[fg].gfx)
        gptr_update_v(1);
}

/* VioSetPage (Mode X): Seite p (0/1) zeigen. Der neue Anfang gilt ab dem
   naechsten Bildwechsel; vorn wartet der Aufruf auf den Beginn des
   Strahlruecklaufs (hoechstens 2 Takte), damit das Programm danach
   gefahrlos in die andere Seite zeichnet. */
int __far vio_setpage(int s, unsigned p)
{
    unsigned long t;
    if (s < 0 || !ses[s].used || ses[s].gfx != MODEX || p > 1)
        return -1;
    if (s == fg && hw_gfx == MODEX) {
        /* Zeiger erst auf die neue (noch verdeckte) Seite, dann
           umschalten, dann von der alten nehmen - nie ohne Zeiger */
        int was = gp_drawn;
        struct gpst *o = &gst[gci];
        if (was)
            gptr_to(o->x, o->y, p ? PAGEX : 0);   /* gci zeigt nun auf neu */
        t = get_ticks();
        while ((inb(0x3DA) & 0x08) && get_ticks() - t < 2)
            ;                               /* laufenden Ruecklauf abwarten */
        set_start(p ? PAGEX : 0);
        ses[s].gfx_page = (unsigned char)p;
        while (!(inb(0x3DA) & 0x08) && get_ticks() - t < 2)
            ;                               /* neuer Anfang uebernommen */
        if (was)
            gcomb(o, 0);                    /* alte Seite aufraeumen    */
        cur_dirty = 1;
    } else
        ses[s].gfx_page = (unsigned char)p;
    return 0;
}
