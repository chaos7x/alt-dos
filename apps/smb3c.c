/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.4 - Kryptografie fuer SMB 3 (Schnittstelle smb3c.h)
 * 286-tauglich, keine eigenen statischen Daten (SMBCLI ist knapp).
 */
#include "smb3c.h"

static void dbl(u8 *k)                  /* k = k << 1 in GF(2^128)       */
{
    unsigned i, c = k[0] & 0x80;
    for (i = 0; i < 15; i++)
        k[i] = (u8)(k[i] << 1 | k[i + 1] >> 7);
    k[15] = (u8)(k[15] << 1);
    if (c)
        k[15] ^= 0x87;
}

void aes_cmac(const struct aes *a, const u8 *d, unsigned n, u8 mac[16])
{
    u8 k[16], x[16];
    unsigned i, last;
    cr_zero(k, 16);
    aes_enc(a, k, k);                   /* L                              */
    dbl(k);                             /* K1                             */
    cr_zero(x, 16);
    while (n > 16) {
        for (i = 0; i < 16; i++)
            x[i] ^= d[i];
        aes_enc(a, x, x);
        d += 16;
        n -= 16;
    }
    last = n;                           /* 0..16                          */
    if (last < 16)
        dbl(k);                         /* K2, auffuellen                 */
    for (i = 0; i < last; i++)
        x[i] ^= d[i];
    if (last < 16)
        x[last] ^= 0x80;
    for (i = 0; i < 16; i++)
        x[i] ^= k[i];
    aes_enc(a, x, mac);
}

/* CBC-MAC von CCM (M = 16, L = 4) -> t */
static void ccm_mac(const struct aes *a, const u8 nonce[11], const u8 *aad, unsigned alen,
                    const u8 *d, unsigned n, u8 t[16])
{
    unsigned i, k, p;
    t[0] = 0x7B;                        /* Adata, M' 7, L' 3              */
    cr_cpy(t + 1, nonce, 11);
    t[12] = 0;
    t[13] = 0;
    t[14] = (u8)(n >> 8);
    t[15] = (u8)n;
    aes_enc(a, t, t);
    t[0] ^= (u8)(alen >> 8);            /* AAD mit 2-Byte-Laenge vorn     */
    t[1] ^= (u8)alen;
    p = 2;
    while (alen) {
        k = 16 - p < alen ? 16 - p : alen;
        for (i = 0; i < k; i++)
            t[p + i] ^= aad[i];
        aad += k;
        alen -= k;
        aes_enc(a, t, t);
        p = 0;
    }
    if (p)                              /* nur Laenge, keine AAD          */
        aes_enc(a, t, t);
    while (n) {
        k = n < 16 ? n : 16;
        for (i = 0; i < k; i++)
            t[i] ^= d[i];
        aes_enc(a, t, t);
        d += k;
        n -= k;
    }
}

/* CTR: Zaehler ab i (A_i) */
static void ccm_ctr(const struct aes *a, const u8 nonce[11], u8 *d, unsigned n, u8 s0[16])
{
    u8 c[16], ks[16];
    unsigned i, k, ctr = 0;
    c[0] = 0x03;
    cr_cpy(c + 1, nonce, 11);
    c[12] = c[13] = c[14] = c[15] = 0;
    aes_enc(a, c, s0);
    while (n) {
        ctr++;
        c[14] = (u8)(ctr >> 8);
        c[15] = (u8)ctr;
        aes_enc(a, c, ks);
        k = n < 16 ? n : 16;
        for (i = 0; i < k; i++)
            d[i] ^= ks[i];
        d += k;
        n -= k;
    }
}

void ccm_seal(const struct aes *a, const u8 nonce[11], const u8 *aad, unsigned alen,
              u8 *d, unsigned n, u8 tag[16])
{
    u8 t[16], s0[16];
    unsigned i;
    ccm_mac(a, nonce, aad, alen, d, n, t);
    ccm_ctr(a, nonce, d, n, s0);
    for (i = 0; i < 16; i++)
        tag[i] = t[i] ^ s0[i];
}

int ccm_open(const struct aes *a, const u8 nonce[11], const u8 *aad, unsigned alen,
             u8 *d, unsigned n, const u8 tag[16])
{
    u8 t[16], s0[16];
    unsigned i;
    ccm_ctr(a, nonce, d, n, s0);
    ccm_mac(a, nonce, aad, alen, d, n, t);
    for (i = 0; i < 16; i++)
        t[i] ^= s0[i];
    if (cr_eq(t, tag, 16))
        return 0;
    ccm_ctr(a, nonce, d, n, s0);        /* zurueck (Daten wieder wie vorher) */
    return 1;
}

void smb_kdf(const u8 ki[16], const char *label, unsigned ll,
             const u8 *ctx, unsigned cl, u8 ko[16])
{
    static const u8 one[4] = { 0, 0, 0, 1 }, z = 0, bits[4] = { 0, 0, 0, 0x80 };
    struct hmac h;
    u8 o[32];
    hmac_init(&h, ki, 16);
    hmac_add(&h, one, 4);
    hmac_add(&h, label, ll);
    hmac_add(&h, &z, 1);
    hmac_add(&h, ctx, cl);
    hmac_add(&h, bits, 4);
    hmac_end(&h, o);
    cr_cpy(ko, o, 16);
}
