/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Kryptografie fuer Kerberos v5 (aes256-cts-hmac-sha1-96)
 *
 * Eigene Umsetzungen nach den Normen (nur gelesen): SHA-1 (FIPS 180-4),
 * HMAC (RFC 2104), PBKDF2 (RFC 2898), n-fold/DK und Verschluesselungs-
 * rahmen (RFC 3961), AES-CTS fuer Kerberos (RFC 3962). Baut auf crypto.c
 * (AES) auf, 286-tauglich wie dort. Auf dem Entwicklungsrechner gegen
 * Python geprueft (tools/krbtest.c, tools/krbtest.py).
 */
#ifndef KRBCRYPT_H
#define KRBCRYPT_H
#include "crypto.h"

struct sha1 {
    u32 h[5];
    u32 n;                              /* Laenge in Byte                */
    u8  b[64];
    u16 k;
};
void sha1_init(struct sha1 *c);
void sha1_add(struct sha1 *c, const void *d, unsigned n);
void sha1_end(struct sha1 *c, u8 out[20]);

/* HMAC-SHA1: Schluesselzustand einmal vorbereiten, dann beliebig oft */
struct hmac1 { struct sha1 in, out; };
void hmac1_key(struct hmac1 *k, const void *key, unsigned klen);
void hmac1(const struct hmac1 *k, const void *d, unsigned n, u8 out[20]);

/* PBKDF2-HMAC-SHA1 */
void pbkdf2_sha1(const void *pw, unsigned plen, const void *salt, unsigned slen,
                 u32 iter, u8 *out, unsigned n);

/* n-fold; kgV(inlen, outlen) hoechstens 128 */
void nfold(const u8 *in, unsigned inlen, u8 *out, unsigned outlen);

/* Schluessel aus Kennwort (RFC 3962): 32 Byte, Salz z. B. "REALMname" */
void krb_s2k(const void *pw, unsigned plen, const void *salt, unsigned slen,
             u32 iter, u8 key[32]);

/* Verschluesseln: out = AES-CTS(Ke, conf || d) || HMAC(Ki)[0..11],
   Laenge n + 28; conf 16 Zufallsbyte */
void krb_enc(const u8 key[32], u32 usage, const u8 conf[16],
             const u8 *d, unsigned n, u8 *out);
/* Entschluesseln an Ort und Stelle: 0 = ok, *pt = Klartext (ohne
   Konfusor), *plen = Laenge */
int  krb_dec(const u8 key[32], u32 usage, u8 *d, unsigned n,
             u8 **pt, unsigned *plen);
/* Pruefsumme hmac-sha1-96-aes256 mit Kc */
void krb_cksum(const u8 key[32], u32 usage, const u8 *d, unsigned n, u8 out[12]);

/* rc4-hmac (Etype 23, RFC 4757): Schluessel 16 Byte = MD4(UTF-16LE(pw));
   Verschluesseln: out = HMAC-MD5 (16) || RC4(conf 8 || d), Laenge n + 24;
   Pruefsumme hmac-md5 (Typ -138) 16 Byte */
void krb_key23(const char *pw, unsigned plen, u8 key[16]);
void krb_enc23(const u8 key[16], u32 usage, const u8 conf[8],
               const u8 *d, unsigned n, u8 *out);
int  krb_dec23(const u8 key[16], u32 usage, u8 *d, unsigned n,
               u8 **pt, unsigned *plen);
void krb_cksum23(const u8 key[16], u32 usage, const u8 *d, unsigned n, u8 out[16]);

#endif
