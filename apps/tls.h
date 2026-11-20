/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - TLS-1.2-Client fuer FAPI-Programme (WGET), apps/tls.c
 *
 * Eine Verbindung zur Zeit ueber einen verbundenen TCP-Socket (FAPI_NET).
 * Schluesselaustausch ECDHE mit X25519, Verschluesselung AES-128-GCM
 * (TLS_ECDHE_RSA/ECDSA_WITH_AES_128_GCM_SHA256), Extended Master Secret
 * (RFC 7627), SNI. Stufe 1: der Server wird noch NICHT geprueft
 * (Zertifikatskette, Signatur, Name) - tls_open verlangt deshalb
 * TLS_NOVERIFY; ohne liefert es TLSE_NOVERIFY.
 */
#ifndef TLS_H
#define TLS_H

#define TLS_NOVERIFY    1               /* Server nicht pruefen (Stufe 1) */

#define TLSE_OK         0
#define TLSE_NET        1               /* Verbindung/Frist              */
#define TLSE_PROTO      2               /* unerwartete Nachricht         */
#define TLSE_ALERT      3               /* Server meldet Fehler (tls_alert) */
#define TLSE_CIPHER     4               /* kein gemeinsames Verfahren    */
#define TLSE_MAC        5               /* Pruefsumme falsch (manipuliert?) */
#define TLSE_NOVERIFY   6               /* Pruefung des Servers fehlt noch */
#define TLSE_CERTREQ    7               /* Server will ein Client-Zertifikat */

extern unsigned char tls_alert;         /* letzte Fehlermeldung des Servers */

int  tls_open(unsigned sock, const char *host, unsigned tmo_s, unsigned flags);
int  tls_read(void *b, unsigned n);     /* -> Bytes, 0 = Ende, -1 Fehler  */
int  tls_write(const void *b, unsigned n);  /* -> n oder -1              */
void tls_close(void);                   /* close_notify                  */
const char *tls_msg(int e);
void tls_random(unsigned char *d, unsigned n);  /* Zufall (FAPI_RANDOM + Takt) */

#endif
