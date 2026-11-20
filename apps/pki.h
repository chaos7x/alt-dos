/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Pruefung von Servern fuer TLS (apps/pki.c)
 *
 * Eigene Umsetzungen nach den Normen (nur gelesen): SHA-384/512 (FIPS
 * 180-4), RSA-Signaturen PKCS#1 v1.5 und PSS (RFC 8017), ECDSA ueber
 * P-256/P-384 (FIPS 186-4, SEC 1), X.509 (RFC 5280) in DER. Grosszahlen
 * in Worten zu 16 Bit mit Montgomery-Multiplikation (RSA bis 4096 Bit).
 * Zertifikate liegen fern (eigenes Segment), Zahlen und Hashes nah.
 */
#ifndef PKI_H
#define PKI_H

#include "crypto.h"
#ifdef CRYPTO_HOST
#define FAR
#else
#define FAR __far
#endif

#define H_SHA256        1
#define H_SHA384        2
#define H_SHA512        3

#define SC_PKCS1        1               /* RSA PKCS#1 v1.5               */
#define SC_PSS          2               /* RSA-PSS (MGF1, gleicher Hash) */
#define SC_ECDSA        3

#define PK_RSA          1
#define PK_EC256        2
#define PK_EC384        3

struct sha512 {
    u32 h[16];                          /* je Wort: hoch, tief           */
    u32 n;                              /* Laenge in Byte                */
    u8  b[128];
    u16 k, is384;
};
void sha512_init(struct sha512 *c, int is384);
void sha512_add(struct sha512 *c, const void *d, unsigned n);
void sha512_end(struct sha512 *c, u8 *out);     /* 48 bzw. 64 Byte   */

unsigned hash_len(int alg);
void hash_far(int alg, const u8 FAR *d, unsigned n, u8 *out);

struct pkey {
    u8 type;                            /* PK_*, 0 = unbekannt           */
    const u8 FAR *a;                    /* RSA: Modul, EC: Punkt 04|x|y  */
    unsigned alen;
    const u8 FAR *e;                    /* RSA: Exponent                 */
    unsigned elen;
};

/* Signatur sig ueber den Hash h (Algorithmus halg) mit Verfahren sc
   pruefen -> 0 = gueltig */
int pk_verify(const struct pkey *k, int sc, int halg, const u8 *h,
              const u8 FAR *sig, unsigned slen);

struct cert {
    const u8 FAR *raw;  unsigned rawlen;    /* ganzes Zertifikat        */
    const u8 FAR *tbs;  unsigned tbslen;    /* mit Kopf (wird gehasht)  */
    const u8 FAR *iss;  unsigned isslen;    /* Name (DER, mit Kopf)     */
    const u8 FAR *sub;  unsigned sublen;
    const u8 FAR *san;  unsigned sanlen;    /* GeneralNames (Inhalt)    */
    const u8 FAR *cn;   unsigned cnlen;     /* CN des Subjekts          */
    const u8 FAR *sig;  unsigned siglen;
    char nb[15], na[15];                    /* JJJJMMTTHHMMSS           */
    int  sc, halg;                          /* Signatur des Ausstellers */
    signed char ca;                         /* basicConstraints: 1, 0, -1 fehlt */
    struct pkey key;
};

/* DER-Zertifikat lesen -> 0 ok */
int x509_parse(const u8 FAR *d, unsigned n, struct cert *c);
/* Signatur von c mit dem Schluessel k (des Ausstellers) pruefen -> 0 ok */
int x509_sig_ok(const struct cert *c, const struct pkey *k);
/* gilt c fuer den Rechnernamen bzw. die Zahlenadresse h? -> 1 ja */
int x509_name_ok(const struct cert *c, const char *h);
/* gleiche DER-Namen? -> 1 ja */
int der_eq(const u8 FAR *a, unsigned an, const u8 FAR *b, unsigned bn);

#endif
