/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Kryptografie fuer TLS (apps/tls.c) und spaeter SMB/Kerberos
 *
 * Eigene Umsetzungen nach den Normen (nur gelesen): SHA-256 (FIPS 180-4),
 * HMAC (RFC 2104), TLS-PRF (RFC 5246 Abschnitt 5), AES-128 (FIPS 197),
 * GCM (NIST SP 800-38D), X25519 (RFC 7748). 286-tauglich: keine 32-Bit-
 * Register, Multiplikation nur 16 x 16 -> 32 Bit (ein MUL), keine
 * 32-Bit-Multiplikation/-Division (sonst zoege Watcom __U4M/__U4D).
 * Auf dem Entwicklungsrechner mit gcc uebersetzbar (CRYPTO_HOST) und dort
 * gegen Python/OpenSSL geprueft (tools/cryptotest.c).
 */
#ifndef CRYPTO_H
#define CRYPTO_H

#ifdef CRYPTO_HOST
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#else
typedef unsigned char  u8;
typedef unsigned       u16;
typedef unsigned long  u32;
#endif

void cr_cpy(void *d, const void *s, unsigned n);
void cr_zero(void *d, unsigned n);
int  cr_eq(const void *a, const void *b, unsigned n);  /* 1 = gleich (ohne Abbruch) */

/* SHA-256 */
struct sha256 {
    u32 h[8];
    u32 nlo, nhi;                       /* Laenge in Byte                */
    u8  b[64];
    u16 n;
};
void sha256_init(struct sha256 *c);
void sha256_add(struct sha256 *c, const void *d, unsigned n);
void sha256_end(struct sha256 *c, u8 out[32]);  /* c danach unbrauchbar  */

/* MD4 (RFC 1320, fuer den NT-Hash von NTLM) und MD5 (RFC 1321) */
struct md5 {
    u32 h[4];
    u32 n;                              /* Laenge in Byte                */
    u8  b[64];
    u16 k, md4;
};
void md5_init(struct md5 *c, int md4);
void md5_add(struct md5 *c, const void *d, unsigned n);
void md5_end(struct md5 *c, u8 out[16]);
void hmac_md5(const u8 *key, unsigned klen, const u8 *d, unsigned n, u8 out[16]);

/* HMAC-SHA256 */
struct hmac {
    struct sha256 in;
    u8 ok[64];                          /* Schluessel ^ opad             */
};
void hmac_init(struct hmac *h, const void *key, unsigned klen);
void hmac_add(struct hmac *h, const void *d, unsigned n);
void hmac_end(struct hmac *h, u8 out[32]);

/* TLS 1.2 PRF mit SHA-256: out = P_SHA256(secret, label + seed) */
void tls_prf(const u8 *sec, unsigned slen, const char *label,
             const u8 *seed, unsigned seedlen, u8 *out, unsigned n);

/* AES-128 (nur Verschluesseln, GCM braucht nur das) */
struct aes { u8 rk[176]; };
void aes_key(struct aes *a, const u8 key[16]);
void aes_enc(const struct aes *a, const u8 in[16], u8 out[16]);

/* GCM mit 12-Byte-IV: Daten an Ort und Stelle (n Byte), tag 16 Byte.
   gcm_open -> 0 = Tag stimmt (Daten entschluesselt) */
struct gcm { struct aes k; u8 h[16]; };
void gcm_key(struct gcm *g, const u8 key[16]);
void gcm_seal(const struct gcm *g, const u8 iv[12], const u8 *aad, unsigned alen,
              u8 *d, unsigned n, u8 tag[16]);
int  gcm_open(const struct gcm *g, const u8 iv[12], const u8 *aad, unsigned alen,
              u8 *d, unsigned n, const u8 tag[16]);

/* X25519: out = k * u (32 Byte, Little Endian) */
void x25519(u8 out[32], const u8 k[32], const u8 u[32]);
void x25519_base(u8 out[32], const u8 k[32]);   /* u = 9             */

#endif
