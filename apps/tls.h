/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - TLS-1.2-Client fuer FAPI-Programme (WGET), apps/tls.c
 *
 * Eine Verbindung zur Zeit ueber einen verbundenen TCP-Socket (FAPI_NET).
 * Schluesselaustausch ECDHE mit X25519, Verschluesselung AES-128-GCM
 * (TLS_ECDHE_RSA/ECDSA_WITH_AES_128_GCM_SHA256), Extended Master Secret
 * (RFC 7627), SNI. Der Server wird geprueft (apps/pki.c): Signatur des
 * ServerKeyExchange mit dem Schluessel des Zertifikats, Kette bis zu einer
 * Wurzel aus der CA-Liste (PEM, Vorgabe X:\DOS\SYSTEM\CACERT.PEM), Name
 * (subjectAltName, sonst CN) und Gueltigkeit (auf den Tag, +-1 Tag fuer
 * die Zeitzone). TLS_NOVERIFY laesst das weg.
 */
#ifndef TLS_H
#define TLS_H

#define TLS_NOVERIFY    1               /* Server nicht pruefen           */

#define TLSE_OK         0
#define TLSE_NET        1               /* Verbindung/Frist              */
#define TLSE_PROTO      2               /* unerwartete Nachricht         */
#define TLSE_ALERT      3               /* Server meldet Fehler (tls_alert) */
#define TLSE_CIPHER     4               /* kein gemeinsames Verfahren    */
#define TLSE_MAC        5               /* Pruefsumme falsch (manipuliert?) */
#define TLSE_NOVERIFY   6               /* Pruefung des Servers fehlt noch */
#define TLSE_CERTREQ    7               /* Server will ein Client-Zertifikat */
#define TLSE_CERT       8               /* Kette ungueltig/Aussteller unbekannt */
#define TLSE_NAME       9               /* Zertifikat gilt nicht fuer den Namen */
#define TLSE_DATE       10              /* abgelaufen oder noch nicht gueltig */
#define TLSE_SIG        11              /* Signatur des Servers falsch    */
#define TLSE_NOCA       12              /* CA-Liste fehlt                 */
#define TLSE_MEM        13              /* kein Speicher                  */

extern unsigned char tls_alert;         /* letzte Fehlermeldung des Servers */

int  tls_open(unsigned sock, const char *host, unsigned tmo_s, unsigned flags);
int  tls_read(void *b, unsigned n);     /* -> Bytes, 0 = Ende, -1 Fehler  */
int  tls_write(const void *b, unsigned n);  /* -> n oder -1              */
void tls_close(void);                   /* close_notify                  */
const char *tls_msg(int e);
void tls_random(unsigned char *d, unsigned n);  /* Zufall (FAPI_RANDOM + Takt) */
void tls_ca(const char *path);          /* CA-Liste (sonst Vorgabe)       */
extern char tls_why[80];                /* Einzelheit zur letzten Pruefung */

#endif
