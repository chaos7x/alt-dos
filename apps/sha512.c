/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - SHA-384/512 (FIPS 180-4, eigene Umsetzung), aus pki.c
 * herausgeloest, damit SMBCLI (SMB 3.1.1) es ohne RSA/X.509 bekommt.
 * Schnittstelle in pki.h.
 */
#include "pki.h"

/* ---- SHA-384/512 (64-Bit-Worte als Paare hoch/tief) --------------------- */
static const u32 K512[160] = {
    0x428a2f98UL, 0xd728ae22UL, 0x71374491UL, 0x23ef65cdUL,
    0xb5c0fbcfUL, 0xec4d3b2fUL, 0xe9b5dba5UL, 0x8189dbbcUL,
    0x3956c25bUL, 0xf348b538UL, 0x59f111f1UL, 0xb605d019UL,
    0x923f82a4UL, 0xaf194f9bUL, 0xab1c5ed5UL, 0xda6d8118UL,
    0xd807aa98UL, 0xa3030242UL, 0x12835b01UL, 0x45706fbeUL,
    0x243185beUL, 0x4ee4b28cUL, 0x550c7dc3UL, 0xd5ffb4e2UL,
    0x72be5d74UL, 0xf27b896fUL, 0x80deb1feUL, 0x3b1696b1UL,
    0x9bdc06a7UL, 0x25c71235UL, 0xc19bf174UL, 0xcf692694UL,
    0xe49b69c1UL, 0x9ef14ad2UL, 0xefbe4786UL, 0x384f25e3UL,
    0x0fc19dc6UL, 0x8b8cd5b5UL, 0x240ca1ccUL, 0x77ac9c65UL,
    0x2de92c6fUL, 0x592b0275UL, 0x4a7484aaUL, 0x6ea6e483UL,
    0x5cb0a9dcUL, 0xbd41fbd4UL, 0x76f988daUL, 0x831153b5UL,
    0x983e5152UL, 0xee66dfabUL, 0xa831c66dUL, 0x2db43210UL,
    0xb00327c8UL, 0x98fb213fUL, 0xbf597fc7UL, 0xbeef0ee4UL,
    0xc6e00bf3UL, 0x3da88fc2UL, 0xd5a79147UL, 0x930aa725UL,
    0x06ca6351UL, 0xe003826fUL, 0x14292967UL, 0x0a0e6e70UL,
    0x27b70a85UL, 0x46d22ffcUL, 0x2e1b2138UL, 0x5c26c926UL,
    0x4d2c6dfcUL, 0x5ac42aedUL, 0x53380d13UL, 0x9d95b3dfUL,
    0x650a7354UL, 0x8baf63deUL, 0x766a0abbUL, 0x3c77b2a8UL,
    0x81c2c92eUL, 0x47edaee6UL, 0x92722c85UL, 0x1482353bUL,
    0xa2bfe8a1UL, 0x4cf10364UL, 0xa81a664bUL, 0xbc423001UL,
    0xc24b8b70UL, 0xd0f89791UL, 0xc76c51a3UL, 0x0654be30UL,
    0xd192e819UL, 0xd6ef5218UL, 0xd6990624UL, 0x5565a910UL,
    0xf40e3585UL, 0x5771202aUL, 0x106aa070UL, 0x32bbd1b8UL,
    0x19a4c116UL, 0xb8d2d0c8UL, 0x1e376c08UL, 0x5141ab53UL,
    0x2748774cUL, 0xdf8eeb99UL, 0x34b0bcb5UL, 0xe19b48a8UL,
    0x391c0cb3UL, 0xc5c95a63UL, 0x4ed8aa4aUL, 0xe3418acbUL,
    0x5b9cca4fUL, 0x7763e373UL, 0x682e6ff3UL, 0xd6b2b8a3UL,
    0x748f82eeUL, 0x5defb2fcUL, 0x78a5636fUL, 0x43172f60UL,
    0x84c87814UL, 0xa1f0ab72UL, 0x8cc70208UL, 0x1a6439ecUL,
    0x90befffaUL, 0x23631e28UL, 0xa4506cebUL, 0xde82bde9UL,
    0xbef9a3f7UL, 0xb2c67915UL, 0xc67178f2UL, 0xe372532bUL,
    0xca273eceUL, 0xea26619cUL, 0xd186b8c7UL, 0x21c0c207UL,
    0xeada7dd6UL, 0xcde0eb1eUL, 0xf57d4f7fUL, 0xee6ed178UL,
    0x06f067aaUL, 0x72176fbaUL, 0x0a637dc5UL, 0xa2c898a6UL,
    0x113f9804UL, 0xbef90daeUL, 0x1b710b35UL, 0x131c471bUL,
    0x28db77f5UL, 0x23047d84UL, 0x32caab7bUL, 0x40c72493UL,
    0x3c9ebe0aUL, 0x15c9bebcUL, 0x431d67c4UL, 0x9c100d4cUL,
    0x4cc5d4beUL, 0xcb3e42b6UL, 0x597f299cUL, 0xfc657e2aUL,
    0x5fcb6fabUL, 0x3ad6faecUL, 0x6c44198cUL, 0x4a475817UL,
};
static const u32 IV512[16] = {
    0x6a09e667UL, 0xf3bcc908UL, 0xbb67ae85UL, 0x84caa73bUL, 0x3c6ef372UL, 0xfe94f82bUL, 0xa54ff53aUL, 0x5f1d36f1UL,
    0x510e527fUL, 0xade682d1UL, 0x9b05688cUL, 0x2b3e6c1fUL, 0x1f83d9abUL, 0xfb41bd6bUL, 0x5be0cd19UL, 0x137e2179UL
};
static const u32 IV384[16] = {
    0xcbbb9d5dUL, 0xc1059ed8UL, 0x629a292aUL, 0x367cd507UL, 0x9159015aUL, 0x3070dd17UL, 0x152fecd8UL, 0xf70e5939UL,
    0x67332667UL, 0xffc00b31UL, 0x8eb44a87UL, 0x68581511UL, 0xdb0c2e0dUL, 0x64f98fa7UL, 0x47b5481dUL, 0xbefa4fa4UL
};

#define H(i) w[2 * (i)]
#define L(i) w[2 * (i) + 1]

/* x = (h,l) rechts rotiert um n (1..63) */
static void ror64(u32 *rh, u32 *rl, u32 h, u32 l, unsigned n)
{
    u32 t;
    if (n >= 32) {
        t = h; h = l; l = t;
        n -= 32;
    }
    if (n) {
        *rh = h >> n | l << (32 - n);
        *rl = l >> n | h << (32 - n);
    } else {
        *rh = h;
        *rl = l;
    }
}

static void add64(u32 *h, u32 *l, u32 ah, u32 al)
{
    *l += al;
    *h += ah + (*l < al);
}

static void sha512_blk(struct sha512 *c)
{
    static u32 w[160], v[16];
    u32 t1h, t1l, t2h, t2l, ah, al, bh, bl;
    unsigned i, j;
    const u8 *p = c->b;
    for (i = 0; i < 32; i++, p += 4)
        w[i] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
    for (i = 16; i < 80; i++) {
        /* s0 = ror1 ^ ror8 ^ shr7 von w[i-15], s1 = ror19 ^ ror61 ^ shr6 von w[i-2] */
        u32 xh = H(i - 15), xl = L(i - 15), yh = H(i - 2), yl = L(i - 2);
        ror64(&ah, &al, xh, xl, 1);
        ror64(&bh, &bl, xh, xl, 8);
        t1h = ah ^ bh ^ (xh >> 7);
        t1l = al ^ bl ^ (xl >> 7 | xh << 25);
        ror64(&ah, &al, yh, yl, 19);
        ror64(&bh, &bl, yh, yl, 61);
        t2h = ah ^ bh ^ (yh >> 6);
        t2l = al ^ bl ^ (yl >> 6 | yh << 26);
        H(i) = H(i - 16);
        L(i) = L(i - 16);
        add64(&H(i), &L(i), t1h, t1l);
        add64(&H(i), &L(i), H(i - 7), L(i - 7));
        add64(&H(i), &L(i), t2h, t2l);
    }
    for (j = 0; j < 16; j++)
        v[j] = c->h[j];
#define VH(k) v[2 * (k)]
#define VL(k) v[2 * (k) + 1]
    for (i = 0; i < 80; i++) {
        /* t1 = h + S1(e) + ch(e,f,g) + K + w */
        ror64(&ah, &al, VH(4), VL(4), 14);
        ror64(&bh, &bl, VH(4), VL(4), 18);
        t1h = ah ^ bh;
        t1l = al ^ bl;
        ror64(&ah, &al, VH(4), VL(4), 41);
        t1h ^= ah;
        t1l ^= al;
        add64(&t1h, &t1l, VH(7), VL(7));
        add64(&t1h, &t1l, (VH(4) & VH(5)) ^ (~VH(4) & VH(6)),
              (VL(4) & VL(5)) ^ (~VL(4) & VL(6)));
        add64(&t1h, &t1l, K512[2 * i], K512[2 * i + 1]);
        add64(&t1h, &t1l, H(i), L(i));
        /* t2 = S0(a) + maj(a,b,c) */
        ror64(&ah, &al, VH(0), VL(0), 28);
        ror64(&bh, &bl, VH(0), VL(0), 34);
        t2h = ah ^ bh;
        t2l = al ^ bl;
        ror64(&ah, &al, VH(0), VL(0), 39);
        t2h ^= ah;
        t2l ^= al;
        add64(&t2h, &t2l, (VH(0) & VH(1)) ^ (VH(0) & VH(2)) ^ (VH(1) & VH(2)),
              (VL(0) & VL(1)) ^ (VL(0) & VL(2)) ^ (VL(1) & VL(2)));
        for (j = 14; j >= 2; j -= 2) {
            v[j] = v[j - 2];
            v[j + 1] = v[j - 1];
        }
        add64(&VH(4), &VL(4), t1h, t1l);    /* e = d + t1 (d jetzt bei 4) */
        VH(0) = t1h;
        VL(0) = t1l;
        add64(&VH(0), &VL(0), t2h, t2l);
    }
    for (j = 0; j < 16; j += 2)
        add64(&c->h[j], &c->h[j + 1], v[j], v[j + 1]);
}

void sha512_init(struct sha512 *c, int is384)
{
    unsigned i;
    for (i = 0; i < 16; i++)
        c->h[i] = is384 ? IV384[i] : IV512[i];
    c->n = 0;
    c->k = 0;
    c->is384 = (u16)is384;
}

void sha512_add(struct sha512 *c, const void *d, unsigned n)
{
    const u8 *p = d;
    while (n--) {
        c->b[c->k++] = *p++;
        c->n++;
        if (c->k == 128) {
            sha512_blk(c);
            c->k = 0;
        }
    }
}

void sha512_end(struct sha512 *c, u8 *out)
{
    u32 lo = c->n << 3, hi = c->n >> 29;
    unsigned i;
    c->b[c->k++] = 0x80;
    if (c->k > 112) {
        while (c->k < 128)
            c->b[c->k++] = 0;
        sha512_blk(c);
        c->k = 0;
    }
    while (c->k < 120)
        c->b[c->k++] = 0;
    for (i = 0; i < 4; i++) {
        c->b[120 + i] = (u8)(hi >> (24 - 8 * i));
        c->b[124 + i] = (u8)(lo >> (24 - 8 * i));
    }
    sha512_blk(c);
    for (i = 0; i < (c->is384 ? 48u : 64u); i++)
        out[i] = (u8)(c->h[i >> 2] >> (24 - 8 * (i & 3)));
}

