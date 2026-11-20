/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Kryptografie (Schnittstelle und Quellen in crypto.h)
 */
#include "crypto.h"

#ifdef CRYPTO_HOST
static u32 mul16(u16 a, u16 b) { return (u32)a * b; }
#else
u32 mul16(u16 a, u16 b);
#pragma aux mul16 = "mul dx" __parm [__ax] [__dx] __value [__dx __ax] __modify [__ax __dx];
#endif

void cr_cpy(void *d, const void *s, unsigned n)
{
    u8 *a = d;
    const u8 *b = s;
    while (n--)
        *a++ = *b++;
}

void cr_zero(void *d, unsigned n)
{
    u8 *a = d;
    while (n--)
        *a++ = 0;
}

int cr_eq(const void *a, const void *b, unsigned n)
{
    const u8 *x = a, *y = b;
    u8 r = 0;
    while (n--)
        r |= *x++ ^ *y++;
    return r == 0;
}

/* ---- SHA-256 ------------------------------------------------------------ */
static const u32 K256[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL,
    0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL,
    0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
    0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL,
    0x5b9cca4fUL, 0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_blk(struct sha256 *c)
{
    static u32 w[64];                   /* (Stapel der Programme: 2 KB)  */
    u32 a, b, d, e, f, g, h, cc, t1, t2;
    unsigned i;
    const u8 *p = c->b;
    for (i = 0; i < 16; i++, p += 4)
        w[i] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
    for (i = 16; i < 64; i++) {
        a = w[i - 15];
        b = w[i - 2];
        w[i] = w[i - 16] + (ROR(a, 7) ^ ROR(a, 18) ^ (a >> 3)) + w[i - 7] +
               (ROR(b, 17) ^ ROR(b, 19) ^ (b >> 10));
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(struct sha256 *c)
{
    c->h[0] = 0x6a09e667UL; c->h[1] = 0xbb67ae85UL; c->h[2] = 0x3c6ef372UL;
    c->h[3] = 0xa54ff53aUL; c->h[4] = 0x510e527fUL; c->h[5] = 0x9b05688cUL;
    c->h[6] = 0x1f83d9abUL; c->h[7] = 0x5be0cd19UL;
    c->nlo = c->nhi = 0;
    c->n = 0;
}

void sha256_add(struct sha256 *c, const void *d, unsigned n)
{
    const u8 *p = d;
    while (n--) {
        c->b[c->n++] = *p++;
        if (++c->nlo == 0)
            c->nhi++;
        if (c->n == 64) {
            sha256_blk(c);
            c->n = 0;
        }
    }
}

void sha256_end(struct sha256 *c, u8 out[32])
{
    u32 lo = c->nlo << 3, hi = c->nhi << 3 | c->nlo >> 29;
    unsigned i;
    c->b[c->n++] = 0x80;
    if (c->n > 56) {
        while (c->n < 64)
            c->b[c->n++] = 0;
        sha256_blk(c);
        c->n = 0;
    }
    while (c->n < 56)
        c->b[c->n++] = 0;
    for (i = 0; i < 4; i++) {
        c->b[56 + i] = (u8)(hi >> (24 - 8 * i));
        c->b[60 + i] = (u8)(lo >> (24 - 8 * i));
    }
    sha256_blk(c);
    for (i = 0; i < 32; i++)
        out[i] = (u8)(c->h[i >> 2] >> (24 - 8 * (i & 3)));
}

/* ---- HMAC-SHA256, PRF --------------------------------------------------- */
void hmac_init(struct hmac *h, const void *key, unsigned klen)
{
    u8 k[64];
    unsigned i;
    cr_zero(k, 64);
    if (klen > 64) {
        sha256_init(&h->in);
        sha256_add(&h->in, key, klen);
        sha256_end(&h->in, k);
    } else
        cr_cpy(k, key, klen);
    for (i = 0; i < 64; i++) {
        h->ok[i] = k[i] ^ 0x5C;
        k[i] ^= 0x36;
    }
    sha256_init(&h->in);
    sha256_add(&h->in, k, 64);
}

void hmac_add(struct hmac *h, const void *d, unsigned n)
{
    sha256_add(&h->in, d, n);
}

void hmac_end(struct hmac *h, u8 out[32])
{
    u8 t[32];
    sha256_end(&h->in, t);
    sha256_init(&h->in);
    sha256_add(&h->in, h->ok, 64);
    sha256_add(&h->in, t, 32);
    sha256_end(&h->in, out);
}

void tls_prf(const u8 *sec, unsigned slen, const char *label,
             const u8 *seed, unsigned seedlen, u8 *out, unsigned n)
{
    static struct hmac h;
    static u8 a[32], t[32];
    unsigned ll = 0, k;
    while (label[ll])
        ll++;
    hmac_init(&h, sec, slen);           /* A(1) = HMAC(label + seed)     */
    hmac_add(&h, label, ll);
    hmac_add(&h, seed, seedlen);
    hmac_end(&h, a);
    while (n) {
        hmac_init(&h, sec, slen);
        hmac_add(&h, a, 32);
        hmac_add(&h, label, ll);
        hmac_add(&h, seed, seedlen);
        hmac_end(&h, t);
        k = n < 32 ? n : 32;
        cr_cpy(out, t, k);
        out += k;
        n -= k;
        hmac_init(&h, sec, slen);       /* A(i+1) = HMAC(A(i))           */
        hmac_add(&h, a, 32);
        hmac_end(&h, a);
    }
}

/* ---- AES-128 ------------------------------------------------------------ */
static const u8 SB[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

#define XT(x) ((u8)(((x) << 1) ^ (((x) & 0x80) ? 0x1B : 0)))

void aes_key(struct aes *a, const u8 key[16])
{
    unsigned i;
    u8 rc = 1, t0, t1, t2, t3, *r = a->rk;
    cr_cpy(r, key, 16);
    for (i = 16; i < 176; i += 4) {
        t0 = r[i - 4]; t1 = r[i - 3]; t2 = r[i - 2]; t3 = r[i - 1];
        if ((i & 15) == 0) {
            u8 k = t0;
            t0 = SB[t1] ^ rc;
            t1 = SB[t2];
            t2 = SB[t3];
            t3 = SB[k];
            rc = XT(rc);
        }
        r[i] = r[i - 16] ^ t0;
        r[i + 1] = r[i - 15] ^ t1;
        r[i + 2] = r[i - 14] ^ t2;
        r[i + 3] = r[i - 13] ^ t3;
    }
}

void aes_enc(const struct aes *a, const u8 in[16], u8 out[16])
{
    u8 s[16], t[16];
    unsigned i, r, c;
    const u8 *k = a->rk;
    for (i = 0; i < 16; i++)
        s[i] = in[i] ^ k[i];
    for (r = 1; r <= 10; r++) {
        k += 16;
        for (i = 0; i < 16; i++)        /* SubBytes + ShiftRows          */
            t[i] = SB[s[(i + 4 * (i & 3)) & 15]];
        if (r < 10)
            for (c = 0; c < 16; c += 4) {   /* MixColumns                 */
                u8 a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3];
                u8 x = a0 ^ a1 ^ a2 ^ a3;
                t[c] = a0 ^ x ^ XT(a0 ^ a1);
                t[c + 1] = a1 ^ x ^ XT(a1 ^ a2);
                t[c + 2] = a2 ^ x ^ XT(a2 ^ a3);
                t[c + 3] = a3 ^ x ^ XT(a3 ^ a0);
            }
        for (i = 0; i < 16; i++)
            s[i] = t[i] ^ k[i];
    }
    cr_cpy(out, s, 16);
}

/* ---- GCM ---------------------------------------------------------------- */
static void gmul(u8 x[16], const u8 h[16])     /* x = x * h in GF(2^128) */
{
    u8 z[16], v[16];
    unsigned i, j, c;
    cr_zero(z, 16);
    cr_cpy(v, h, 16);
    for (i = 0; i < 128; i++) {
        if (x[i >> 3] & (0x80 >> (i & 7)))
            for (j = 0; j < 16; j++)
                z[j] ^= v[j];
        c = v[15] & 1;
        for (j = 15; j > 0; j--)
            v[j] = (u8)(v[j] >> 1 | v[j - 1] << 7);
        v[0] >>= 1;
        if (c)
            v[0] ^= 0xE1;
    }
    cr_cpy(x, z, 16);
}

static void ghash(const u8 h[16], u8 y[16], const u8 *d, unsigned n)
{
    unsigned i, k;
    while (n) {
        k = n < 16 ? n : 16;
        for (i = 0; i < k; i++)
            y[i] ^= d[i];
        gmul(y, h);
        d += k;
        n -= k;
    }
}

void gcm_key(struct gcm *g, const u8 key[16])
{
    u8 z[16];
    aes_key(&g->k, key);
    cr_zero(z, 16);
    aes_enc(&g->k, z, g->h);
}

static void inc32(u8 c[16])
{
    unsigned i = 16;
    while (i-- > 12)
        if (++c[i])
            break;
}

static void gcm_ctr(const struct gcm *g, const u8 iv[12], u8 *d, unsigned n)
{
    u8 c[16], ks[16];
    unsigned i, k;
    cr_cpy(c, iv, 12);
    c[12] = c[13] = c[14] = 0;
    c[15] = 2;                          /* J0 + 1                         */
    while (n) {
        aes_enc(&g->k, c, ks);
        k = n < 16 ? n : 16;
        for (i = 0; i < k; i++)
            d[i] ^= ks[i];
        inc32(c);
        d += k;
        n -= k;
    }
}

static void gcm_tag(const struct gcm *g, const u8 iv[12], const u8 *aad, unsigned alen,
                    const u8 *c, unsigned n, u8 tag[16])
{
    u8 y[16], l[16], j0[16];
    unsigned i;
    cr_zero(y, 16);
    ghash(g->h, y, aad, alen);
    ghash(g->h, y, c, n);
    cr_zero(l, 16);                     /* Laengen in Bit, je 64 Bit BE   */
    l[5] = (u8)(alen >> 13);
    l[6] = (u8)(alen >> 5);
    l[7] = (u8)(alen << 3);
    l[13] = (u8)(n >> 13);
    l[14] = (u8)(n >> 5);
    l[15] = (u8)(n << 3);
    ghash(g->h, y, l, 16);
    cr_cpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    aes_enc(&g->k, j0, l);
    for (i = 0; i < 16; i++)
        tag[i] = y[i] ^ l[i];
}

void gcm_seal(const struct gcm *g, const u8 iv[12], const u8 *aad, unsigned alen,
              u8 *d, unsigned n, u8 tag[16])
{
    gcm_ctr(g, iv, d, n);
    gcm_tag(g, iv, aad, alen, d, n, tag);
}

int gcm_open(const struct gcm *g, const u8 iv[12], const u8 *aad, unsigned alen,
             u8 *d, unsigned n, const u8 tag[16])
{
    u8 t[16];
    gcm_tag(g, iv, aad, alen, d, n, t);
    if (!cr_eq(t, tag, 16))
        return 1;
    gcm_ctr(g, iv, d, n);
    return 0;
}

/* ---- X25519 -------------------------------------------------------------
   Feldelemente mod p = 2^255 - 19 als 16 Worte zu 16 Bit (Little Endian),
   Wert < 2^256; 2^256 = 38 mod p. */
typedef u16 fe[16];

static void fe_fold(u16 *o, u16 c)      /* o += c * 38                    */
{
    unsigned i;
    u32 t;
    while (c) {
        t = mul16(c, 38);
        for (i = 0; i < 16 && t; i++) {
            t += o[i];
            o[i] = (u16)t;
            t >>= 16;
        }
        c = (u16)t;
    }
}

static void fe_add(u16 *o, const u16 *a, const u16 *b)
{
    unsigned i;
    u32 t = 0;
    for (i = 0; i < 16; i++) {
        t += (u32)a[i] + b[i];
        o[i] = (u16)t;
        t >>= 16;
    }
    fe_fold(o, (u16)t);
}

static void fe_sub(u16 *o, const u16 *a, const u16 *b)
{
    unsigned i, br = 0;
    u32 t;
    for (i = 0; i < 16; i++) {
        t = (u32)a[i] - b[i] - br;
        o[i] = (u16)t;
        br = (unsigned)(t >> 16) & 1;
    }
    while (br) {                        /* -2^256 = -38                   */
        t = (u32)o[0] - 38;
        o[0] = (u16)t;
        br = (unsigned)(t >> 16) & 1;
        for (i = 1; i < 16 && br; i++) {
            t = (u32)o[i] - 1;
            o[i] = (u16)t;
            br = (unsigned)(t >> 16) & 1;
        }
    }
}

static void fe_mul(u16 *o, const u16 *a, const u16 *b)
{
    static u16 r[32];
    unsigned i, j;
    u32 t;
    u16 c;
    for (i = 0; i < 32; i++)
        r[i] = 0;
    for (i = 0; i < 16; i++) {
        c = 0;
        for (j = 0; j < 16; j++) {
            t = mul16(a[i], b[j]) + r[i + j] + c;
            r[i + j] = (u16)t;
            c = (u16)(t >> 16);
        }
        r[i + 16] = c;
    }
    t = 0;
    for (i = 0; i < 16; i++) {
        t += mul16(r[16 + i], 38) + r[i];
        o[i] = (u16)t;
        t >>= 16;
    }
    fe_fold(o, (u16)t);
}

static void fe_cswap(u16 *a, u16 *b, unsigned s)
{
    u16 m = (u16)(0 - s), x;
    unsigned i;
    for (i = 0; i < 16; i++) {
        x = m & (a[i] ^ b[i]);
        a[i] ^= x;
        b[i] ^= x;
    }
}

static void fe_freeze(u16 *o)
{
    static const u16 p[16] = { 0xFFED, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x7FFF };
    u16 m[16];
    unsigned i, k, br;
    u32 t;
    for (k = 0; k < 2; k++) {
        br = 0;
        for (i = 0; i < 16; i++) {
            t = (u32)o[i] - p[i] - br;
            m[i] = (u16)t;
            br = (unsigned)(t >> 16) & 1;
        }
        fe_cswap(o, m, 1 - br);
    }
}

static void fe_inv(u16 *o, const u16 *z)    /* z^(p-2)                    */
{
    static fe c;
    int a;
    for (a = 0; a < 16; a++)
        c[a] = z[a];
    for (a = 253; a >= 0; a--) {
        fe_mul(c, c, c);
        if (a != 2 && a != 4)
            fe_mul(c, c, z);
    }
    for (a = 0; a < 16; a++)
        o[a] = c[a];
}

void x25519(u8 out[32], const u8 k[32], const u8 u[32])
{
    static fe x1, x2, z2, x3, z3, A, AA, B, BB, E, C, D, DA, CB;
    static const fe a24 = { 0xDB41, 1 };
    static u8 e[32];
    unsigned i, sw = 0, b;
    int t;
    cr_cpy(e, k, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;
    for (i = 0; i < 16; i++) {
        x1[i] = (u16)(u[2 * i] | (u16)u[2 * i + 1] << 8);
        x2[i] = z2[i] = z3[i] = 0;
    }
    x1[15] &= 0x7FFF;
    x2[0] = z3[0] = 1;
    for (i = 0; i < 16; i++)
        x3[i] = x1[i];
    for (t = 254; t >= 0; t--) {
        b = (e[t >> 3] >> (t & 7)) & 1;
        sw ^= b;
        fe_cswap(x2, x3, sw);
        fe_cswap(z2, z3, sw);
        sw = b;
        fe_add(A, x2, z2);
        fe_mul(AA, A, A);
        fe_sub(B, x2, z2);
        fe_mul(BB, B, B);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul(DA, D, A);
        fe_mul(CB, C, B);
        fe_add(x3, DA, CB);
        fe_mul(x3, x3, x3);
        fe_sub(z3, DA, CB);
        fe_mul(z3, z3, z3);
        fe_mul(z3, z3, x1);
        fe_mul(x2, AA, BB);
        fe_mul(z2, a24, E);
        fe_add(z2, z2, AA);
        fe_mul(z2, z2, E);
    }
    fe_cswap(x2, x3, sw);
    fe_cswap(z2, z3, sw);
    fe_inv(z2, z2);
    fe_mul(x2, x2, z2);
    fe_freeze(x2);
    for (i = 0; i < 16; i++) {
        out[2 * i] = (u8)x2[i];
        out[2 * i + 1] = (u8)(x2[i] >> 8);
    }
    cr_zero(e, 32);
}

void x25519_base(u8 out[32], const u8 k[32])
{
    static const u8 nine[32] = { 9 };
    x25519(out, k, nine);
}
