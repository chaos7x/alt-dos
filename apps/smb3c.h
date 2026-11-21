/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.4 - Kryptografie fuer SMB 3 (apps/smb3c.c): AES-CMAC (RFC 4493),
 * AES-CCM mit 11-Byte-Nonce und 16-Byte-Tag (NIST SP 800-38C, RFC 3610),
 * Schluesselableitung nach NIST SP 800-108 im Zaehlerbetrieb mit
 * HMAC-SHA256 wie [MS-SMB2] 3.1.4.2. Eigene Umsetzungen nach den Normen.
 */
#ifndef SMB3C_H
#define SMB3C_H
#include "crypto.h"

/* CMAC ueber d (n Byte) mit dem Schluessel in a -> 16 Byte */
void aes_cmac(const struct aes *a, const u8 *d, unsigned n, u8 mac[16]);

/* CCM: Daten an Ort und Stelle, nonce 11 Byte; ccm_open -> 0 = Tag stimmt */
void ccm_seal(const struct aes *a, const u8 nonce[11], const u8 *aad, unsigned alen,
              u8 *d, unsigned n, u8 tag[16]);
int  ccm_open(const struct aes *a, const u8 nonce[11], const u8 *aad, unsigned alen,
              u8 *d, unsigned n, const u8 tag[16]);

/* Ko = HMAC-SHA256(Ki, 1 || Label || 0 || Kontext || L)[0..ol-1], L = 8*ol;
   ll/cl sind die Laengen samt der abschliessenden Null, wie MS-SMB2 sie
   nennt (z. B. "SMB2AESCMAC" 12) */
void smb_kdf(const u8 *ki, unsigned kl, const char *label, unsigned ll,
             const u8 *ctx, unsigned cl, u8 *ko, unsigned ol);   /* ol 16/32 */
#endif
