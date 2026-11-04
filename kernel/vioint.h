/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - gemeinsame Teile von vio.c und vgagfx.c (Grafik in KCODE2)
 */
#ifndef VIOINT_H
#define VIOINT_H

struct session {
    unsigned char used;
    unsigned char row, col;
    unsigned char rows, top;        /* 23/1 normal, 25/0 Vollbild       */
    unsigned char lines;            /* Bildschirmzeilen 25/43/50        */
    unsigned char hidecur;          /* Cursor aus (Vollbild)            */
    signed char   owner;            /* Task, der Vollbild verlangt hat  */
    unsigned char srow, scol;       /* Cursor vor dem Vollbild          */
    pg_t          save;             /* gesicherter Inhalt (Seite), 0 = keiner */
    pg_t          unit;             /* Sitzungspuffer (Seite)           */
    char          title[13];
    signed char   mou_owner;        /* Task mit MouOpen, -1 = keiner    */
    unsigned char mou_show;         /* Zeiger zeigen                    */
    unsigned char mq_head, mq_n;    /* Warteschlange der Mausereignisse */
    unsigned char gfx;              /* Grafikmodus (13h), 0 = Text      */
    signed char   gfx_task;         /* Task mit dem Bildspeicher-Selektor */
    unsigned      gfx_sel;          /* ... dieser Selektor (0 = keiner) */
    pg_t          gfx_save;         /* Puffer: Bild + 768 Byte Palette  */
    unsigned char gfx_plane;        /* 12h/X: gewaehlte Ebene (VioSetPlane) */
    unsigned char gfx_page;         /* Mode X: sichtbare Seite (VioSetPage) */
    unsigned char attr;             /* Textfarbe fuer TTY (ANSI ESC[..m) */
    unsigned char ansi;             /* ANSI-Folgen auswerten (ANSI ON)  */
    unsigned char ast, np;          /* ANSI: Zustand, Zahl der Parameter */
    unsigned char par[6];           /* ANSI: Parameter                  */
    unsigned char arow, acol;       /* ANSI: ESC[s gemerkter Cursor     */
    unsigned short vmode, vpg;      /* VESA: Modus, Seiten des Bildes   */
};

/* Grafikpuffer: 13h 64 KB Bild, 12h/X vier Ebenen zu 38400 Byte; Palette dahinter */
#define PLANE12     38400U
#define MODEX       0x58            /* 320x240x256 unverkettet, 2 Seiten */
#define PAGEX       19200U          /* Mode X: Seite je Ebene (80 x 240) */
#define PLANAR(m)   ((m) == 0x12 || (m) == MODEX)   /* 4 Ebenen zu 38400 */
#define GFX_IMG(m)  (PLANAR(m) ? 4UL * PLANE12 : 65536UL)
#define RAWGFX      0x80            /* DOS-Box: Karte gehoert der Box (echtes Video-BIOS) */
#define RAW_REGS    0x40000UL       /* Rohmodus: 4 Ebenen zu 64 KB, dann Register */
#define RAW_LIN     0x41000UL       /* Rohmodus: Schatten (Sicht der CPU, 64 KB) */
#define RAW_KIND    61              /* Bytes hinter den 61 Registern: Art des Schattens */
#define RAW_ALLOW   62              /*   (0 keiner, 1 A0000 64 KB, 3 B8000 32 KB), BIOS-Modus erlaubt es */
#define RAW_ST      0x340           /* Zustand der Ports hinten (Indizes, DAC-Zaehler) */
#define GFX_UNITS(m) ((m) == RAWGFX ? PAGES(RAW_LIN + 0x10000UL) : PAGES(GFX_IMG(m) + 768))
#define VESAGFX     0x81            /* VESA-Modus (linearer Bildspeicher), Bild in vpg Seiten */
#define GFX_UNITS_S(s) (ses[s].gfx == VESAGFX ? ses[s].vpg + 1u : GFX_UNITS(ses[s].gfx))

extern struct session ses[NTASK];
extern int fg;
extern unsigned char cur_dirty, hw_lines, mou_nbtn;
extern int mx, my;
extern unsigned char hw_gfx;            /* vgagfx.c: Karte steht auf 0 Text, 12h, 13h, X */
extern int vp_xm, vp_ym;                /* vgagfx.c: Mausgrenzen VESA     */
extern unsigned char gp_drawn;          /* vgagfx.c: Grafikzeiger gezeichnet */
extern unsigned char  vga_ok;           /* start.asm, aus dem Real Mode     */

void redraw(void);                      /* vio.c                            */

/* vgagfx.c liegt in KCODE2: fern */
int  __far gfx_in(void);
void __far gfx_out(void);
void __far gptr_update(void);
void __far gptr_remove(void);

#endif
