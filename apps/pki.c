/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Pruefung von Servern fuer TLS (Schnittstelle pki.h)
 */
#include "pki.h"

#ifdef CRYPTO_HOST
static u32 mul16(u16 a, u16 b) { return (u32)a * b; }
#else
u32 mul16(u16 a, u16 b);
#pragma aux mul16 = "mul dx" __parm [__ax] [__dx] __value [__dx __ax] __modify [__ax __dx];
#endif

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

unsigned hash_len(int alg)
{
    return alg == H_SHA256 ? 32 : alg == H_SHA384 ? 48 : 64;
}

void hash_far(int alg, const u8 FAR *d, unsigned n, u8 *out)
{
    static struct sha256 s;
    static struct sha512 x;
    static u8 t[128];
    unsigned k, i;
    if (alg == H_SHA256)
        sha256_init(&s);
    else
        sha512_init(&x, alg == H_SHA384);
    while (n) {
        k = n < sizeof(t) ? n : sizeof(t);
        for (i = 0; i < k; i++)
            t[i] = d[i];
        if (alg == H_SHA256)
            sha256_add(&s, t, k);
        else
            sha512_add(&x, t, k);
        d += k;
        n -= k;
    }
    if (alg == H_SHA256)
        sha256_end(&s, out);
    else
        sha512_end(&x, out);
}

/* ---- Grosszahlen (16-Bit-Worte, Little Endian) -------------------------- */
#define BNL 256                         /* 4096 Bit                      */
typedef u16 bn[BNL];

static void bn_zero(u16 *a, unsigned nl) { while (nl--) *a++ = 0; }
static void bn_copy(u16 *d, const u16 *s, unsigned nl) { while (nl--) *d++ = *s++; }

static int bn_cmp(const u16 *a, const u16 *b, unsigned nl)
{
    while (nl--)
        if (a[nl] != b[nl])
            return a[nl] < b[nl] ? -1 : 1;
    return 0;
}

static int bn_iszero(const u16 *a, unsigned nl)
{
    while (nl--)
        if (*a++)
            return 0;
    return 1;
}

static unsigned bn_sub(u16 *r, const u16 *a, const u16 *b, unsigned nl)
{
    unsigned i, br = 0;
    u32 t;
    for (i = 0; i < nl; i++) {
        t = (u32)a[i] - b[i] - br;
        r[i] = (u16)t;
        br = (unsigned)(t >> 16) & 1;
    }
    return br;
}

static unsigned bn_add(u16 *r, const u16 *a, const u16 *b, unsigned nl)
{
    unsigned i;
    u32 t = 0;
    for (i = 0; i < nl; i++) {
        t += (u32)a[i] + b[i];
        r[i] = (u16)t;
        t >>= 16;
    }
    return (unsigned)t;
}

/* Big Endian (fern) -> nl Worte; 1 = passt nicht */
static int bn_from(u16 *r, unsigned nl, const u8 FAR *b, unsigned n)
{
    unsigned i;
    while (n && !*b) {
        b++;
        n--;
    }
    if (n > 2 * nl)
        return 1;
    bn_zero(r, nl);
    for (i = 0; i < n; i++)
        r[i >> 1] |= (u16)b[n - 1 - i] << (8 * (i & 1));
    return 0;
}

static void bn_to(u8 *b, unsigned n, const u16 *a)    /* n Byte Big Endian */
{
    unsigned i;
    for (i = 0; i < n; i++)
        b[n - 1 - i] = (u8)(a[i >> 1] >> (8 * (i & 1)));
}

/* Modul fuer Montgomery: m, nl, -m^-1 mod 2^16, R^2 mod m */
struct mod {
    u16 m[BNL], r2[BNL], one[BNL];      /* one = R mod m                 */
    unsigned nl;
    u16 mi;
};

static void mont(const struct mod *M, u16 *r, const u16 *a, const u16 *b)
{
    static u16 t[BNL + 2];
    unsigned i, j, nl = M->nl;
    const u16 *m = M->m;
    u32 s;
    u16 c, u;
    for (i = 0; i < nl + 2; i++)
        t[i] = 0;
    for (i = 0; i < nl; i++) {
        c = 0;
        for (j = 0; j < nl; j++) {
            s = mul16(a[i], b[j]) + t[j] + c;
            t[j] = (u16)s;
            c = (u16)(s >> 16);
        }
        s = (u32)t[nl] + c;
        t[nl] = (u16)s;
        t[nl + 1] = (u16)(s >> 16);
        u = (u16)mul16(t[0], M->mi);
        s = mul16(u, m[0]) + t[0];
        c = (u16)(s >> 16);
        for (j = 1; j < nl; j++) {
            s = mul16(u, m[j]) + t[j] + c;
            t[j - 1] = (u16)s;
            c = (u16)(s >> 16);
        }
        s = (u32)t[nl] + c;
        t[nl - 1] = (u16)s;
        t[nl] = (u16)(t[nl + 1] + (u16)(s >> 16));
        t[nl + 1] = 0;
    }
    if (t[nl] || bn_cmp(t, m, nl) >= 0)
        bn_sub(t, t, m, nl);
    bn_copy(r, t, nl);
}

/* 2x mod m (x < m) */
static void bn_dblmod(u16 *x, const u16 *m, unsigned nl)
{
    unsigned c = bn_add(x, x, x, nl);
    if (c || bn_cmp(x, m, nl) >= 0)
        bn_sub(x, x, m, nl);
}

static int mod_init(struct mod *M, const u16 *m, unsigned nl)
{
    u16 x, m0;
    unsigned i;
    while (nl > 1 && !m[nl - 1])
        nl--;
    if (!(m[0] & 1))
        return 1;
    M->nl = nl;
    bn_copy(M->m, m, nl);
    m0 = m[0];
    x = m0;                             /* Newton: 3 -> 6 -> 12 -> 24 Bit */
    for (i = 0; i < 4; i++)
        x = (u16)mul16(x, (u16)(2 - (u16)mul16(m0, x)));
    M->mi = (u16)(0 - x);
    bn_zero(M->one, nl);                /* R mod m durch Verdoppeln von 1 */
    M->one[0] = 1;
    for (i = 0; i < 16 * nl; i++)
        bn_dblmod(M->one, M->m, nl);
    bn_copy(M->r2, M->one, nl);
    for (i = 0; i < 16 * nl; i++)
        bn_dblmod(M->r2, M->m, nl);
    return 0;
}

static void to_m(const struct mod *M, u16 *r, const u16 *a) { mont(M, r, a, M->r2); }
static void from_m(const struct mod *M, u16 *r, const u16 *a)
{
    static u16 one[BNL];
    bn_zero(one, M->nl);
    one[0] = 1;
    mont(M, r, a, one);
}

/* r = a^e (Montgomery-Form rein und raus), e Big Endian in Bytes */
static void mexp(const struct mod *M, u16 *r, const u16 *a, const u8 *e, unsigned en)
{
    static u16 x[BNL];
    unsigned i;
    int b;
    bn_copy(x, M->one, M->nl);
    for (i = 0; i < en; i++)
        for (b = 7; b >= 0; b--) {
            mont(M, x, x, x);
            if (e[i] >> b & 1)
                mont(M, x, x, a);
        }
    bn_copy(r, x, M->nl);
}

static void madd(const struct mod *M, u16 *r, const u16 *a, const u16 *b)
{
    if (bn_add(r, a, b, M->nl) || bn_cmp(r, M->m, M->nl) >= 0)
        bn_sub(r, r, M->m, M->nl);
}

static void msub(const struct mod *M, u16 *r, const u16 *a, const u16 *b)
{
    if (bn_sub(r, a, b, M->nl))
        bn_add(r, r, M->m, M->nl);
}

/* ---- RSA ---------------------------------------------------------------- */
static struct mod mods[2];             /* RSA und EC nie gleichzeitig   */
#define rm mods[0]
static bn rs, rx;
static u8 em[512];

static const u8 di256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
static const u8 di384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
static const u8 di512[] = { 0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };

static void hash_near(int alg, const u8 *d, unsigned n, u8 *out)
{
    hash_far(alg, (const u8 FAR *)d, n, out);
}

/* MGF1 mit dem Hash alg: Maske ueber d (n Byte) aus seed legen */
static void mgf1(int alg, const u8 *seed, unsigned sl, u8 *d, unsigned n)
{
    static u8 b[64 + 4], h[64];
    unsigned hl = hash_len(alg), i, k;
    u32 c = 0;
    cr_cpy(b, seed, sl);
    while (n) {
        b[sl] = (u8)(c >> 24); b[sl + 1] = (u8)(c >> 16);
        b[sl + 2] = (u8)(c >> 8); b[sl + 3] = (u8)c;
        hash_near(alg, b, sl + 4, h);
        k = n < hl ? n : hl;
        for (i = 0; i < k; i++)
            *d++ ^= h[i];
        n -= k;
        c++;
    }
}

static int rsa_verify(const struct pkey *k, int sc, int halg, const u8 *hh,
                      const u8 FAR *sig, unsigned slen)
{
    static u8 ex[8], h2[64], mp[8 + 64 + 512];
    unsigned nl, nb, i, hl = hash_len(halg), bits;
    const u8 *di;
    u16 top;
    nl = (k->alen + 1) / 2;
    if (k->elen == 0 || k->elen > 8 || k->alen < 64)
        return 1;
    if (bn_from(rx, BNL, k->a, k->alen) || mod_init(&rm, rx, (k->alen + 1) / 2))
        return 1;
    nl = rm.nl;
    for (i = 0; i < k->elen; i++)
        ex[i] = k->e[i];
    if (bn_from(rs, nl, sig, slen) || bn_cmp(rs, rm.m, nl) >= 0)
        return 1;
    /* Laenge des Moduls in Byte und Bit */
    top = rm.m[nl - 1];
    for (bits = 16 * nl; !(top & 0x8000); top <<= 1)
        bits--;
    nb = (bits + 7) / 8;
    to_m(&rm, rx, rs);
    mexp(&rm, rx, rx, ex, k->elen);
    from_m(&rm, rs, rx);
    bn_to(em, nb, rs);
    if (sc == SC_PKCS1) {
        di = halg == H_SHA256 ? di256 : halg == H_SHA384 ? di384 : di512;
        if (nb < 11 + 19 + hl || em[0] != 0 || em[1] != 1)
            return 1;
        for (i = 2; i < nb - 20 - hl; i++)
            if (em[i] != 0xFF)
                return 1;
        if (em[i++] != 0)
            return 1;
        return !cr_eq(em + i, di, 19) || !cr_eq(em + i + 19, hh, hl);
    }
    if (sc == SC_PSS) {                 /* RFC 8017 9.1.2, emBits = bits-1 */
        unsigned emb = bits - 1, en = (emb + 7) / 8, dl, ps;
        u8 *e = em + (nb - en);         /* nb - en ist 0 oder 1           */
        if (nb != en && em[0])
            return 1;
        if (en < hl + 2 || e[en - 1] != 0xBC)
            return 1;
        dl = en - hl - 1;
        if (e[0] & (u8)(0xFF << (8 - (8 * en - emb))))
            return 1;
        mgf1(halg, e + dl, hl, e, dl);
        e[0] &= (u8)(0xFF >> (8 * en - emb));
        for (ps = 0; ps < dl && !e[ps]; ps++)
            ;
        if (ps == dl || e[ps] != 1)
            return 1;
        ps++;                           /* Salz: e[ps..dl)                */
        cr_zero(mp, 8);
        cr_cpy(mp + 8, hh, hl);
        cr_cpy(mp + 8 + hl, e + ps, dl - ps);
        hash_near(halg, mp, 8 + hl + dl - ps, h2);
        return !cr_eq(h2, e + dl, hl);
    }
    return 1;
}

/* ---- ECDSA P-256 / P-384 ------------------------------------------------ */
static const u8 P256[6][32] = {
  { 0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
  { 0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51 },
  { 0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96 },
  { 0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5 }
};
static const u8 P384[4][48] = {
  { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF },
  { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC7,0x63,0x4D,0x81,0xF4,0x37,0x2D,0xDF,
    0x58,0x1A,0x0D,0xB2,0x48,0xB0,0xA7,0x7A,0xEC,0xEC,0x19,0x6A,0xCC,0xC5,0x29,0x73 },
  { 0xAA,0x87,0xCA,0x22,0xBE,0x8B,0x05,0x37,0x8E,0xB1,0xC7,0x1E,0xF3,0x20,0xAD,0x74,
    0x6E,0x1D,0x3B,0x62,0x8B,0xA7,0x9B,0x98,0x59,0xF7,0x41,0xE0,0x82,0x54,0x2A,0x38,
    0x55,0x02,0xF2,0x5D,0xBF,0x55,0x29,0x6C,0x3A,0x54,0x5E,0x38,0x72,0x76,0x0A,0xB7 },
  { 0x36,0x17,0xDE,0x4A,0x96,0x26,0x2C,0x6F,0x5D,0x9E,0x98,0xBF,0x92,0x92,0xDC,0x29,
    0xF8,0xF4,0x1D,0xBD,0x28,0x9A,0x14,0x7C,0xE9,0xDA,0x31,0x13,0xB5,0xF0,0xB8,0xC0,
    0x0A,0x60,0xB1,0xCE,0x1D,0x7E,0x81,0x9D,0x7A,0x43,0x1D,0x7C,0x90,0xEA,0x0E,0x5F }
};

#define EL 24                           /* Worte bis P-384               */
struct jp { u16 x[EL], y[EL], z[EL]; u8 inf; };
#define ep mods[0]                      /* Feld p                        */
#define en mods[1]                      /* Ordnung n                     */

static void jdbl(struct jp *r, const struct jp *a)  /* a = -3, dbl-2001-b */
{
    static u16 d[EL], g[EL], be[EL], al[EL], t[EL], u[EL];
    const struct mod *M = &ep;
    if (a->inf || bn_iszero(a->y, M->nl)) {
        r->inf = 1;
        return;
    }
    mont(M, d, a->z, a->z);             /* delta = Z^2                   */
    mont(M, g, a->y, a->y);             /* gamma = Y^2                   */
    mont(M, be, a->x, g);               /* beta = X*gamma                */
    msub(M, t, a->x, d);
    madd(M, u, a->x, d);
    mont(M, al, t, u);
    madd(M, t, al, al);
    madd(M, al, t, al);                 /* alpha = 3(X-d)(X+d)           */
    madd(M, t, a->y, a->z);
    mont(M, t, t, t);
    msub(M, t, t, g);
    msub(M, r->z, t, d);                /* Z3 = (Y+Z)^2 - g - d          */
    madd(M, u, be, be);
    madd(M, u, u, u);                   /* 4 beta                        */
    mont(M, t, al, al);
    madd(M, d, u, u);                   /* 8 beta                        */
    msub(M, r->x, t, d);                /* X3 = alpha^2 - 8 beta         */
    msub(M, u, u, r->x);
    mont(M, u, al, u);
    mont(M, g, g, g);                   /* gamma^2                       */
    madd(M, g, g, g);
    madd(M, g, g, g);
    madd(M, g, g, g);                   /* 8 gamma^2                     */
    msub(M, r->y, u, g);
    r->inf = 0;
}

static void jadd(struct jp *r, const struct jp *a, const struct jp *b)  /* add-2007-bl */
{
    static u16 z1z1[EL], z2z2[EL], u1[EL], u2[EL], s1[EL], s2[EL], h[EL], i_[EL], j[EL], rr[EL], v[EL], t[EL];
    const struct mod *M = &ep;
    unsigned nl = M->nl;
    if (a->inf) {
        if (r != b) { bn_copy(r->x, b->x, nl); bn_copy(r->y, b->y, nl); bn_copy(r->z, b->z, nl); }
        r->inf = b->inf;
        return;
    }
    if (b->inf) {
        if (r != a) { bn_copy(r->x, a->x, nl); bn_copy(r->y, a->y, nl); bn_copy(r->z, a->z, nl); }
        r->inf = a->inf;
        return;
    }
    mont(M, z1z1, a->z, a->z);
    mont(M, z2z2, b->z, b->z);
    mont(M, u1, a->x, z2z2);
    mont(M, u2, b->x, z1z1);
    mont(M, t, b->z, z2z2);
    mont(M, s1, a->y, t);
    mont(M, t, a->z, z1z1);
    mont(M, s2, b->y, t);
    msub(M, h, u2, u1);
    msub(M, rr, s2, s1);
    if (bn_iszero(h, nl)) {
        if (bn_iszero(rr, nl)) {
            jdbl(r, a);
            return;
        }
        r->inf = 1;
        return;
    }
    madd(M, rr, rr, rr);                /* r = 2(S2-S1)                  */
    madd(M, t, h, h);
    mont(M, i_, t, t);                  /* I = (2H)^2                    */
    mont(M, j, h, i_);                  /* J = H*I                       */
    mont(M, v, u1, i_);                 /* V = U1*I                      */
    madd(M, t, a->z, b->z);
    mont(M, t, t, t);
    msub(M, t, t, z1z1);
    msub(M, t, t, z2z2);
    mont(M, r->z, t, h);                /* Z3                            */
    mont(M, t, rr, rr);
    msub(M, t, t, j);
    msub(M, t, t, v);
    msub(M, r->x, t, v);                /* X3 = r^2 - J - 2V             */
    msub(M, t, v, r->x);
    mont(M, t, rr, t);
    mont(M, s1, s1, j);
    madd(M, s1, s1, s1);
    msub(M, r->y, t, s1);               /* Y3 = r(V-X3) - 2 S1 J         */
    r->inf = 0;
}

struct tlv { u8 tag; const u8 FAR *v; unsigned n; const u8 FAR *all; unsigned alln; };
static int der(const u8 FAR **p, const u8 FAR *e, struct tlv *t);

static void be_sub2(u8 *b, unsigned n)  /* b -= 2 (Big Endian)            */
{
    unsigned br = 2, v;
    while (n-- && br) {
        v = b[n];
        if (v >= br) {
            b[n] = (u8)(v - br);
            br = 0;
        } else {
            b[n] = (u8)(v + 256 - br);
            br = 1;
        }
    }
}

static int ec_verify(const struct pkey *k, int halg, const u8 *hh,
                     const u8 FAR *sig, unsigned slen)
{
    static struct jp G, Q, GQ, R;
    static u16 r[EL], s[EL], w[EL], u1[EL], u2[EL], e[EL], t[EL], tmp[EL];
    static u8 nm2[48], ub1[48], ub2[48];
    const u8 (*c)[48] = P384;
    struct tlv sq, ri, si;
    const u8 FAR *q = sig;
    unsigned bl, nl, i, hl = hash_len(halg);
    int b;
    bl = k->type == PK_EC256 ? 32 : 48;
    nl = bl / 2;
    if (k->alen != 1 + 2 * bl || k->a[0] != 4)
        return 1;
    if (der(&q, sig + slen, &sq) || sq.tag != 0x30)     /* SEQ { r, s }  */
        return 1;
    q = sq.v;
    if (der(&q, sq.v + sq.n, &ri) || ri.tag != 2 || der(&q, sq.v + sq.n, &si) || si.tag != 2)
        return 1;
    /* Konstanten */
    if (k->type == PK_EC256) {
        bn_from(t, nl, P256[0], 32); mod_init(&ep, t, nl);
        bn_from(t, nl, P256[1], 32); mod_init(&en, t, nl);
        bn_from(G.x, nl, P256[2], 32);
        bn_from(G.y, nl, P256[3], 32);
    } else {
        bn_from(t, nl, c[0], 48); mod_init(&ep, t, nl);
        bn_from(t, nl, c[1], 48); mod_init(&en, t, nl);
        bn_from(G.x, nl, c[2], 48);
        bn_from(G.y, nl, c[3], 48);
    }
    if (bn_from(r, nl, ri.v, ri.n) || bn_from(s, nl, si.v, si.n) ||
        bn_iszero(r, nl) || bn_iszero(s, nl) ||
        bn_cmp(r, en.m, nl) >= 0 || bn_cmp(s, en.m, nl) >= 0)
        return 1;
    /* e = linke Bits des Hashes */
    bn_from(e, nl, (const u8 FAR *)hh, hl < bl ? hl : bl);
    if (bn_cmp(e, en.m, nl) >= 0)
        bn_sub(e, e, en.m, nl);
    /* w = s^-1 mod n (Fermat), u1 = e w, u2 = r w */
    bn_to(nm2, bl, en.m);
    be_sub2(nm2, bl);                   /* n - 2                          */
    to_m(&en, tmp, s);
    mexp(&en, w, tmp, nm2, bl);
    to_m(&en, tmp, e);
    mont(&en, u1, tmp, w);
    from_m(&en, u1, u1);
    to_m(&en, tmp, r);
    mont(&en, u2, tmp, w);
    from_m(&en, u2, u2);
    bn_to(ub1, bl, u1);
    bn_to(ub2, bl, u2);
    /* Punkte in Montgomery-Form */
    to_m(&ep, G.x, G.x);
    to_m(&ep, G.y, G.y);
    bn_copy(G.z, ep.one, nl);
    G.inf = 0;
    bn_from(Q.x, nl, k->a + 1, bl);
    bn_from(Q.y, nl, k->a + 1 + bl, bl);
    if (bn_cmp(Q.x, ep.m, nl) >= 0 || bn_cmp(Q.y, ep.m, nl) >= 0)
        return 1;
    to_m(&ep, Q.x, Q.x);
    to_m(&ep, Q.y, Q.y);
    bn_copy(Q.z, ep.one, nl);
    Q.inf = 0;
    jadd(&GQ, &G, &Q);
    R.inf = 1;
    for (i = 0; i < bl; i++)            /* Shamir: u1 G + u2 Q            */
        for (b = 7; b >= 0; b--) {
            jdbl(&R, &R);
            if ((ub1[i] >> b & 1) && (ub2[i] >> b & 1))
                jadd(&R, &R, &GQ);
            else if (ub1[i] >> b & 1)
                jadd(&R, &R, &G);
            else if (ub2[i] >> b & 1)
                jadd(&R, &R, &Q);
        }
    if (R.inf)
        return 1;
    /* x = X / Z^2 */
    bn_to(nm2, bl, ep.m);
    be_sub2(nm2, bl);                   /* p - 2                          */
    mont(&ep, t, R.z, R.z);
    mexp(&ep, t, t, nm2, bl);
    mont(&ep, t, R.x, t);
    from_m(&ep, t, t);
    if (bn_cmp(t, en.m, nl) >= 0)
        bn_sub(t, t, en.m, nl);
    return bn_cmp(t, r, nl) != 0;
}

int pk_verify(const struct pkey *k, int sc, int halg, const u8 *h,
              const u8 FAR *sig, unsigned slen)
{
    if (k->type == PK_RSA && (sc == SC_PKCS1 || sc == SC_PSS))
        return rsa_verify(k, sc, halg, h, sig, slen);
    if ((k->type == PK_EC256 || k->type == PK_EC384) && sc == SC_ECDSA)
        return ec_verify(k, halg, h, sig, slen);
    return 1;
}

/* ---- DER / X.509 -------------------------------------------------------- */

/* naechstes TLV ab *p (bis e) -> 0 ok */
static int der(const u8 FAR **p, const u8 FAR *e, struct tlv *t)
{
    const u8 FAR *q = *p;
    unsigned long n;
    unsigned k;
    if (e - q < 2)
        return 1;
    t->all = q;
    t->tag = *q++;
    n = *q++;
    if (n & 0x80) {
        k = (unsigned)n & 0x7F;
        if (k == 0 || k > 2 || e - q < (long)k)
            return 1;
        n = 0;
        while (k--)
            n = n << 8 | *q++;
    }
    if ((unsigned long)(e - q) < n)
        return 1;
    t->v = q;
    t->n = (unsigned)n;
    t->alln = (unsigned)(q - t->all) + t->n;
    *p = q + t->n;
    return 0;
}

static int oid_is(const struct tlv *t, const u8 *o, unsigned n)
{
    unsigned i;
    if (t->tag != 6 || t->n != n)
        return 0;
    for (i = 0; i < n; i++)
        if (t->v[i] != o[i])
            return 0;
    return 1;
}

static const u8 O_RSA[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01 };
static const u8 O_R256[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B };
static const u8 O_R384[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C };
static const u8 O_R512[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D };
static const u8 O_EC[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01 };
static const u8 O_E256[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };
static const u8 O_E384[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03 };
static const u8 O_P256[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07 };
static const u8 O_P384[] = { 0x2B,0x81,0x04,0x00,0x22 };
static const u8 O_SAN[] = { 0x55,0x1D,0x11 };
static const u8 O_BC[] = { 0x55,0x1D,0x13 };
static const u8 O_CN[] = { 0x55,0x04,0x03 };

static void dtime(const struct tlv *t, char *o)  /* -> JJJJMMTTHHMMSS     */
{
    unsigned i, k = 0;
    if (t->tag == 0x17 && t->n >= 12) {        /* UTCTime JJMMTT...      */
        o[0] = t->v[0] >= '5' ? '1' : '2';
        o[1] = t->v[0] >= '5' ? '9' : '0';
        k = 2;
        for (i = 0; i < 12; i++)
            o[k++] = (char)t->v[i];
    } else if (t->tag == 0x18 && t->n >= 14)
        for (i = 0; i < 14; i++)
            o[k++] = (char)t->v[i];
    else
        for (i = 0; i < 14; i++)
            o[k++] = '0';
    o[14] = 0;
}

static void spki(const u8 FAR *p, const u8 FAR *e, struct pkey *k)
{
    struct tlv a, o, prm, bs, n, ex;
    const u8 FAR *q;
    k->type = 0;
    if (der(&p, e, &a) || a.tag != 0x30)
        return;
    q = a.v;
    if (der(&q, a.v + a.n, &o))
        return;
    if (der(&p, e, &bs) || bs.tag != 3 || bs.n < 2 || bs.v[0] != 0)
        return;
    if (oid_is(&o, O_RSA, sizeof(O_RSA))) {
        q = bs.v + 1;
        if (der(&q, bs.v + bs.n, &a) || a.tag != 0x30)
            return;
        q = a.v;
        if (der(&q, a.v + a.n, &n) || n.tag != 2 || der(&q, a.v + a.n, &ex) || ex.tag != 2)
            return;
        while (n.n && !n.v[0]) {
            n.v++;
            n.n--;
        }
        while (ex.n && !ex.v[0]) {
            ex.v++;
            ex.n--;
        }
        if (n.n > 512)
            return;
        k->type = PK_RSA;
        k->a = n.v;
        k->alen = n.n;
        k->e = ex.v;
        k->elen = ex.n;
    } else if (oid_is(&o, O_EC, sizeof(O_EC))) {
        if (der(&q, a.v + a.n, &prm))
            return;
        k->a = bs.v + 1;
        k->alen = bs.n - 1;
        if (oid_is(&prm, O_P256, sizeof(O_P256)))
            k->type = PK_EC256;
        else if (oid_is(&prm, O_P384, sizeof(O_P384)))
            k->type = PK_EC384;
    }
}

static void exts(const u8 FAR *p, const u8 FAR *e, struct cert *c)
{
    struct tlv s, x, o, v, b;
    const u8 FAR *q, FAR *r;
    if (der(&p, e, &s) || s.tag != 0x30)
        return;
    p = s.v;
    e = s.v + s.n;
    while (p < e) {
        if (der(&p, e, &x) || x.tag != 0x30)
            return;
        q = x.v;
        if (der(&q, x.v + x.n, &o))
            return;
        if (der(&q, x.v + x.n, &v))
            return;
        if (v.tag == 1 && der(&q, x.v + x.n, &v))   /* critical       */
            return;
        if (v.tag != 4)
            continue;
        if (oid_is(&o, O_SAN, sizeof(O_SAN))) {
            r = v.v;
            if (!der(&r, v.v + v.n, &b) && b.tag == 0x30) {
                c->san = b.v;
                c->sanlen = b.n;
            }
        } else if (oid_is(&o, O_BC, sizeof(O_BC))) {
            r = v.v;
            c->ca = 0;
            if (!der(&r, v.v + v.n, &b) && b.tag == 0x30 && b.n >= 3) {
                r = b.v;
                if (!der(&r, b.v + b.n, &b) && b.tag == 1 && b.n == 1 && b.v[0])
                    c->ca = 1;
            }
        }
    }
}

/* CN aus einem Namen (SEQ of SET of SEQ{OID, Wert}) */
static void find_cn(const u8 FAR *p, unsigned n, struct cert *c)
{
    struct tlv nm, set, av, o, val;
    const u8 FAR *e = p + n, FAR *q, FAR *r;
    if (der(&p, e, &nm) || nm.tag != 0x30)
        return;
    p = nm.v;
    e = nm.v + nm.n;
    while (p < e) {
        if (der(&p, e, &set) || set.tag != 0x31)
            return;
        q = set.v;
        while (q < set.v + set.n) {
            if (der(&q, set.v + set.n, &av) || av.tag != 0x30)
                return;
            r = av.v;
            if (!der(&r, av.v + av.n, &o) && oid_is(&o, O_CN, sizeof(O_CN)) &&
                !der(&r, av.v + av.n, &val)) {
                c->cn = val.v;
                c->cnlen = val.n;
            }
        }
    }
}

static void sigalg(const struct tlv *a, struct cert *c)
{
    struct tlv o;
    const u8 FAR *q = a->v;
    c->sc = 0;
    if (a->tag != 0x30 || der(&q, a->v + a->n, &o))
        return;
    if (oid_is(&o, O_R256, sizeof(O_R256))) { c->sc = SC_PKCS1; c->halg = H_SHA256; }
    else if (oid_is(&o, O_R384, sizeof(O_R384))) { c->sc = SC_PKCS1; c->halg = H_SHA384; }
    else if (oid_is(&o, O_R512, sizeof(O_R512))) { c->sc = SC_PKCS1; c->halg = H_SHA512; }
    else if (oid_is(&o, O_E256, sizeof(O_E256))) { c->sc = SC_ECDSA; c->halg = H_SHA256; }
    else if (oid_is(&o, O_E384, sizeof(O_E384))) { c->sc = SC_ECDSA; c->halg = H_SHA384; }
}

int x509_parse(const u8 FAR *d, unsigned n, struct cert *c)
{
    struct tlv crt, tbs, t, sa, sv, v;
    const u8 FAR *p = d, FAR *q;
    unsigned i;
    for (i = 0; i < sizeof(*c); i++)
        ((u8 *)c)[i] = 0;
    c->ca = -1;
    if (der(&p, d + n, &crt) || crt.tag != 0x30)
        return 1;
    c->raw = crt.all;
    c->rawlen = crt.alln;
    p = crt.v;
    if (der(&p, crt.v + crt.n, &tbs) || tbs.tag != 0x30 ||
        der(&p, crt.v + crt.n, &sa) || der(&p, crt.v + crt.n, &sv) || sv.tag != 3 || sv.n < 1)
        return 1;
    c->tbs = tbs.all;
    c->tbslen = tbs.alln;
    sigalg(&sa, c);
    c->sig = sv.v + 1;
    c->siglen = sv.n - 1;
    q = tbs.v;
    if (der(&q, tbs.v + tbs.n, &t))
        return 1;
    if (t.tag == 0xA0 && der(&q, tbs.v + tbs.n, &t))    /* Version, Seriennr. */
        return 1;
    if (der(&q, tbs.v + tbs.n, &t))                     /* Signatur      */
        return 1;
    if (der(&q, tbs.v + tbs.n, &t) || t.tag != 0x30)    /* Aussteller    */
        return 1;
    c->iss = t.all;
    c->isslen = t.alln;
    if (der(&q, tbs.v + tbs.n, &t) || t.tag != 0x30)    /* Gueltigkeit   */
        return 1;
    p = t.v;
    if (der(&p, t.v + t.n, &v))
        return 1;
    dtime(&v, c->nb);
    if (der(&p, t.v + t.n, &v))
        return 1;
    dtime(&v, c->na);
    if (der(&q, tbs.v + tbs.n, &t) || t.tag != 0x30)    /* Subjekt       */
        return 1;
    c->sub = t.all;
    c->sublen = t.alln;
    find_cn(t.all, t.alln, c);
    if (der(&q, tbs.v + tbs.n, &t) || t.tag != 0x30)    /* Schluessel    */
        return 1;
    spki(t.v, t.v + t.n, &c->key);
    while (q < tbs.v + tbs.n) {                         /* [1] [2] [3]   */
        if (der(&q, tbs.v + tbs.n, &t))
            return 1;
        if (t.tag == 0xA3)
            exts(t.v, t.v + t.n, c);
    }
    return 0;
}

int x509_sig_ok(const struct cert *c, const struct pkey *k)
{
    static u8 h[64];
    if (!c->sc)
        return 0;
    hash_far(c->halg, c->tbs, c->tbslen, h);
    return pk_verify(k, c->sc, c->halg, h, c->sig, c->siglen) == 0;
}

int der_eq(const u8 FAR *a, unsigned an, const u8 FAR *b, unsigned bn)
{
    if (an != bn)
        return 0;
    while (an--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

static u8 lc(u8 c) { return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c; }

/* Muster (dNSName, auch *.domain) gegen Rechnernamen */
static int dns_match(const u8 FAR *p, unsigned n, const char *h)
{
    unsigned hl = 0, i, k;
    while (h[hl])
        hl++;
    while (n && p[n - 1] == '.')
        n--;
    if (n >= 2 && p[0] == '*' && p[1] == '.') {
        for (k = 0; k < hl && h[k] != '.'; k++)
            ;
        if (k == 0 || k == hl || hl - k != n - 1)
            return 0;
        for (i = 1; i < n; i++)
            if (lc(p[i]) != lc((u8)h[k + i - 1]))
                return 0;
        return 1;
    }
    if (n != hl)
        return 0;
    for (i = 0; i < n; i++)
        if (lc(p[i]) != lc((u8)h[i]))
            return 0;
    return 1;
}

/* Zahlenadresse (a.b.c.d) -> 4 Byte, 1 = ist eine */
static int ip4(const char *h, u8 *a)
{
    unsigned i, v;
    for (i = 0; i < 4; i++) {
        if (*h < '0' || *h > '9')
            return 0;
        for (v = 0; *h >= '0' && *h <= '9'; h++)
            v = v * 10 + (*h - '0');
        if (v > 255 || (i < 3 && *h++ != '.'))
            return 0;
        a[i] = (u8)v;
    }
    return *h == 0;
}

int x509_name_ok(const struct cert *c, const char *h)
{
    struct tlv g;
    const u8 FAR *p = c->san, FAR *e = c->san + c->sanlen;
    u8 a[4];
    int isip = ip4(h, a), any = 0;
    while (c->san && p < e) {
        if (der(&p, e, &g))
            break;
        if (g.tag == 0x82) {            /* dNSName                        */
            any = 1;
            if (!isip && dns_match(g.v, g.n, h))
                return 1;
        } else if (g.tag == 0x87 && isip && g.n == 4 && g.v[0] == a[0] &&
                   g.v[1] == a[1] && g.v[2] == a[2] && g.v[3] == a[3])
            return 1;
    }
    if (!any && !isip && c->cn)         /* ohne dNSName: CN (alt)         */
        return dns_match(c->cn, c->cnlen, h);
    return 0;
}
