/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LZMA-Packer (lzmaenc.c): eigene Umsetzung nach der LZMA-Spezifikation
 * von Igor Pavlov (lzma-specification.txt, gemeinfrei). Woerterbuch
 * 128 KB als Ring in zwei 64-KB-Segmenten, Hash-Ketten, verzoegerte
 * Suche, Wiederholungsabstaende rep0-3. lc 3, lp 0, pb 2.
 * Daten werden zugeschoben (lme_feed), die Ausgabe geht an lme_write.
 */
#ifndef LZMAENC_H
#define LZMAENC_H

extern int (*lme_write)(const unsigned char *p, unsigned n);   /* 0 = ok */
extern unsigned long lme_nout;          /* geschriebene Bytes           */

#define LME_DICT 0x20000UL              /* Woerterbuchgroesse (Kopf)     */

int  lme_open(void);                    /* 8 Segmente zu 64 KB           */
void lme_close(void);
void lme_begin(int level);              /* 1 (schnell) - 9 (gut)         */
int  lme_feed(const unsigned char *p, unsigned n);
int  lme_end(void);                     /* ohne Endmarke                 */
void lme_props(unsigned char *p5);      /* 5 Byte Eigenschaften          */

#endif
