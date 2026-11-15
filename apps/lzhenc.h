/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Packer der LH5-Familie (lzhenc.c): lh5 (8 KB), lh6 (32 KB) und ARJ
 * Methode 1-3 (26 KB), Bloecke mit statischem Huffman wie ar002. Daten
 * werden zugeschoben (lze_feed), die Ausgabe geht an lze_write.
 */
#ifndef LZHENC_H
#define LZHENC_H

extern int (*lze_write)(const unsigned char *p, unsigned n);   /* 0 = ok */
extern unsigned long lze_nout;          /* geschriebene Bytes           */

#define LZE_LH5   5
#define LZE_LH6   6
#define LZE_LH7   7                     /* 32 KB, np 17                  */
#define LZE_ARJ   8

int  lze_open(void);                    /* 4 Segmente zu 64 KB           */
void lze_close(void);
void lze_begin(int meth, int level);    /* level 1 (schnell) - 9 (gut)   */
int  lze_feed(const unsigned char *p, unsigned n);
int  lze_end(int zoo);                  /* zoo: Endmarke (Block 0)       */

#endif
