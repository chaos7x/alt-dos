/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AT-DOS GUI - TrueType-Rasterer (gui/ttf.c) */
#ifndef TTF_H
#define TTF_H
struct ttm {                    /* Masse einer verwirklichten Schrift   */
    short height, asc, desc, intlead, extlead, avgw, maxw;
};
struct ttg {                    /* ein Zeichen im Speicher              */
    WORD  off;                  /* Bitmap im Segment der Schrift        */
    BYTE  w, h;                 /* Punkte; Zeilen zu (w+7)/8 Bytes      */
    short xo, yo;               /* links oben relativ zu Stift/Grundlinie */
    BYTE  adv, ok;
};
void  tt_scan(void);
int   tt_nface(void);
LPCSTR tt_facename(int i);
BYTE  tt_facestyle(int i);      /* Bit 0 fett, 1 kursiv, 4 feste Breite, 5 Symbol */
int   tt_find(LPCSTR fam, BYTE style);          /* -> Gesicht oder -1   */
int   tt_realize(int face, short lfh, BYTE bold);   /* -> Schrift oder -1 */
const struct ttm FAR *tt_metrics(int rf);
const BYTE FAR *tt_glyph(int rf, BYTE ch, const struct ttg FAR * FAR *g);
WORD  tt_adv(int rf, BYTE ch);
#endif
