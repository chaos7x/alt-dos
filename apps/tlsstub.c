/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.x - Ersatz fuer apps/tls.c: TLS gibt es erst ab 6.3 (386).
 * CURL (Quelltext wie main) meldet damit bei https:// einen Fehler.
 */
#include "tls.h"

unsigned char tls_alert;
char tls_why[80];

int tls_open(unsigned sock, const char *host, unsigned tmo_s, unsigned flags)
{
    (void)sock; (void)host; (void)tmo_s; (void)flags;
    return TLSE_CIPHER;
}
int tls_read(void *b, unsigned n) { (void)b; (void)n; return -1; }
int tls_write(const void *b, unsigned n) { (void)b; (void)n; return -1; }
void tls_close(void) { }
void tls_ca(const char *path) { (void)path; }
const char *tls_msg(int e)
{
    (void)e;
    return "https:// (TLS) gibt es erst ab AT-DOS 6.3 f\x81r den 386.";
}
