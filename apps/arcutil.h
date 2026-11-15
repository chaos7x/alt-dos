/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hilfen der Packprogramme (GZIP, TAR, ZIP): Zeit FAT <-> Unix, 32-Bit-
 * Division ohne Laufzeitsystem, Prozentanzeige, Oktalzahlen (tar).
 * Unix-Zeit gilt hier als Ortszeit (AT-DOS kennt keine Zeitzone).
 */
#ifndef ARCUTIL_H
#define ARCUTIL_H

unsigned long au_div(unsigned long v, unsigned long d, unsigned long *rem);
unsigned long fat2unix(unsigned date, unsigned time);
void          unix2fat(unsigned long t, unsigned *date, unsigned *time);
void          put_ratio(unsigned long in, unsigned long out); /* "63,2%" */
unsigned long oct_get(const char *p, unsigned n);
void          oct_put(char *p, unsigned n, unsigned long v);  /* n-1 Ziffern + 0 */
void          fat_now(unsigned *date, unsigned *time);

#endif
