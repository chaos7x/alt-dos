/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Deflate (RFC 1951) zum Packen und Entpacken, CRC-32 - gemeinsamer Kern
 * von GZIP, ZIP/UNZIP und TAR (flate.c wird dazugelinkt). Eigene
 * Umsetzung nach der Spezifikation.
 *
 * Ein- und Ausgabe laufen ueber Rueckrufe des Programms; die Eingabe
 * puffert flate.c selbst (fl_getc liefert dem Programm die Bytes, die
 * nach dem Ende eines Deflate-Stroms folgen, z. B. den gzip-Schluss).
 */
#ifndef FLATE_H
#define FLATE_H

extern unsigned (*fl_read)(unsigned char *buf, unsigned n);   /* 0 = Ende   */
extern int      (*fl_write)(const unsigned char *p, unsigned n); /* 0 = ok   */

void     fl_reset(void);                /* Eingabepuffer verwerfen      */
int      fl_getc(void);                 /* naechstes Byte, -1 = Ende    */
void     fl_ungetc(int c);              /* ein Byte zurueck (bis 4)     */
unsigned fl_readbuf(unsigned char *b, unsigned n);    /* 0 = Ende    */

#define FL_OK      0
#define FL_EDATA  -1                    /* Daten kaputt                 */
#define FL_EEOF   -2                    /* Daten zu kurz                */
#define FL_EWRITE -3                    /* fl_write meldete Fehler      */
#define FL_EMEM   -4                    /* kein Speicher (deflate_open) */

int      inflate(void);                 /* einen Deflate-Strom entpacken */

int      deflate_open(void);            /* 4 Segmente zu 64 KB holen    */
int      deflate(int level);            /* alles aus fl_read packen, 1-9 */
void     deflate_begin(int level);      /* oder schrittweise: Daten     */
int      deflate_feed(const unsigned char *p, unsigned n);  /* zuschieben */
int      deflate_end(void);
void     deflate_close(void);

unsigned long crc32(unsigned long crc, const unsigned char *p, unsigned n);

extern unsigned long fl_nin, fl_nout;  /* gelesene/geschriebene Bytes  */

#endif
