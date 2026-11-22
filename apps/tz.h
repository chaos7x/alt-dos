/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Zeitzone nach POSIX-TZ (apps/tz.c), fuer SNTP und NET TIME */
#ifndef TZ_H
#define TZ_H
extern char n_std[12], n_dst[12];       /* Namen (CET/CEST)              */
void tz_parse(const char *s);
void tz_init(void);                     /* TZ aus der Umgebung           */
long tz_off(unsigned long utc, int *dst);   /* Ortszeit = UTC + Ergebnis */
char *tz_put_off(char *p, long o);      /* "UTC+1" -> Ende               */
#endif
