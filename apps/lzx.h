/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AT-DOS - LZX-Entpacker (apps/lzx.c) */
#ifndef LZX_H
#define LZX_H
int  lzx_byte(void);
void lzx_emit(unsigned char *b, unsigned n);
int  lzx_decode(unsigned wbits, unsigned long total);
#ifndef LZX_WGET
unsigned char lzx_wget(unsigned long pos);
void lzx_wput(unsigned long pos, unsigned char b);
#define LZX_WGET(p)     lzx_wget(p)
#define LZX_WPUT(p, b)  lzx_wput((p), (b))
#endif
#endif
