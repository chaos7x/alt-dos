/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LZMA/LZMA2-Entpacker und die 7z-Filter BCJ (x86) und Delta (lzma.c,
 * eigene Umsetzung nach der LZMA-Spezifikation von Igor Pavlov,
 * lzma-specification.txt, gemeinfrei).
 *
 * Woerterbuch: Ring aus bis zu LM_NSEG Segmenten zu 64 KB. Weiter
 * zurueckliegende Bytes holt der Entpacker ueber lm_back aus dem schon
 * Ausgegebenen (das Programm schreibt dann in eine Zwischendatei).
 */
#ifndef LZMA_H
#define LZMA_H

#define LM_NSEG 256                     /* Ring bis 16 MB (LDT 512 Eintraege) */
#define LM_NSMALL 12                    /* reicht es nicht: kleiner Ring */

extern unsigned (*lm_read)(unsigned char *buf, unsigned n);        /* 0 = Ende */
extern int      (*lm_write)(const unsigned char *p, unsigned n);   /* 0 = ok   */
extern int      (*lm_back)(unsigned long pos, unsigned char *b, unsigned n);
extern unsigned long lm_rsize;          /* Ringgroesse in Bytes          */

#define LM_LZMA   1
#define LM_LZMA2  2

#define LM_OK      0
#define LM_EDATA  -1
#define LM_EEOF   -2
#define LM_EWRITE -3
#define LM_EMEM   -4
#define LM_EPROP  -5                    /* lc + lp > 4                   */
#define LM_EBACK  -6                    /* Rueckgriff ohne lm_back       */

int  lm_ring(unsigned long want);       /* Segmente fuer min(want, Platz) */
void lm_ring_free(void);
int  lm_decode(int kind, const unsigned char *props, unsigned np, unsigned long size);

unsigned bcj_x86(unsigned char *buf, unsigned n, unsigned long ip, unsigned *state, int enc);  /* bcj.c */
unsigned long lm_crc32(unsigned long crc, const unsigned char *p, unsigned n);
void     delta_dec(unsigned char *buf, unsigned n, unsigned dist,
                   unsigned char *hist, unsigned char *hpos);

#endif
