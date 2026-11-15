/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Entpackkern fuer LHA/LHarc, ARJ und ZOO (lzh.c wird dazugelinkt):
 * statischer Huffman der LH5-Familie (lh4-7, ARJ 1-3, ZOO lh5),
 * adaptiver Huffman lh1 (LHarc 1.x), LArc lzs/lz5, ARJ 4 und das
 * 13-Bit-LZW von ZOO; dazu CRC-16 (LHA/ZOO) und CRC-32 (ARJ).
 * Eigene Umsetzung nach den Formatbeschreibungen (siehe lzh.c).
 */
#ifndef LZH_H
#define LZH_H

extern unsigned (*lz_read)(unsigned char *buf, unsigned n);   /* 0 = Ende   */
extern int      (*lz_write)(const unsigned char *p, unsigned n); /* 0 = ok   */

#define LZ_LH1    1                     /* LHarc 1.x, 4 KB, adaptiv     */
#define LZ_LH4    4                     /* 4 KB                          */
#define LZ_LH5    5                     /* 8 KB (auch ZOO -h)            */
#define LZ_LH6    6                     /* 32 KB                         */
#define LZ_LH7    7                     /* 64 KB                         */
#define LZ_ARJ    8                     /* ARJ Methode 1-3               */
#define LZ_ARJ4   9                     /* ARJ Methode 4                 */
#define LZ_LZS   10                     /* LArc -lzs-                    */
#define LZ_LZ5   11                     /* LArc -lz5-                    */
#define LZ_LZW   12                     /* ZOO Methode 1                 */

#define LZ_OK      0
#define LZ_EDATA  -1
#define LZ_EEOF   -2
#define LZ_EWRITE -3
#define LZ_EMEM   -4

int      lz_open(void);                 /* 64-KB-Fenster holen           */
int      lz_decode(int meth, unsigned long size);

unsigned      crc16(unsigned crc, const unsigned char *p, unsigned n);
unsigned long crc32a(unsigned long crc, const unsigned char *p, unsigned n);

#endif
