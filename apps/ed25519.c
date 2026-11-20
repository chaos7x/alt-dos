/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Ed25519-Signaturen pruefen (RFC 8032, eigene Umsetzung nach
 * der Norm, nur gelesen) fuer SSH-Hostschluessel (apps/ssh.c).
 * Punkte in erweiterten Koordinaten (X:Y:Z:T), Addition nach
 * "add-2008-hwcd-3" (a = -1, auch zum Verdoppeln), Feldrechnung aus
 * crypto.c (16 Worte zu 16 Bit), Reduktion mod L bitweise. Nur pruefen,
 * kein Signieren - also keine Geheimnisse, keine konstante Laufzeit noetig.
 */
#include "crypto.h"
#include "pki.h"

typedef u16 fe[16];
struct ep { fe x, y, z, t; };

static const fe D2 = { 0xF159, 0x26B2, 0x9B94, 0xEBD6, 0xB156, 0x8283, 0x149A, 0x00E0,
    0xD130, 0xEEF3, 0x80F2, 0x198E, 0xFCE7, 0x56DF, 0xD9DC, 0x2406 };
static const fe DD = { 0x78A3, 0x1359, 0x4DCA, 0x75EB, 0xD8AB, 0x4141, 0x0A4D, 0x0070,
    0xE898, 0x7779, 0x4079, 0x8CC7, 0xFE73, 0x2B6F, 0x6CEE, 0x5203 };
static const fe SQM1 = { 0xA0B0, 0x4A0E, 0x1B27, 0xC4EE, 0xE478, 0xAD2F, 0x1806, 0x2F43,
    0xD7A7, 0x3DFB, 0x0099, 0x2B4D, 0xDF0B, 0x4FC1, 0x2480, 0x2B83 };   /* sqrt(-1) */
static const struct ep BP = {
    { 0xD51A, 0x8F25, 0x2D60, 0xC956, 0xA7B2, 0x9525, 0xC760, 0x692C,
      0xDC5C, 0xFDD6, 0xE231, 0xC0A4, 0x53FE, 0xCD6E, 0x36D3, 0x2169 },
    { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
      0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 },
    { 1 },
    { 0xDDA3, 0xA5B7, 0x8AB3, 0x6DDE, 0x52F5, 0x7751, 0x9F80, 0x20F0,
      0xE37D, 0x64AB, 0x4E8E, 0x66EA, 0x7665, 0xD78B, 0x5F0F, 0x6787 } };
static const u16 LL[16] = { 0xD3ED, 0x5CF5, 0x631A, 0x5812, 0x9CD6, 0xA2F7, 0xF9DE, 0x14DE,
    0, 0, 0, 0, 0, 0, 0, 0x1000 };

static void fcpy(u16 *d, const u16 *s)
{
    unsigned i;
    for (i = 0; i < 16; i++)
        d[i] = s[i];
}

static int fe_same(const u16 *a, const u16 *b)  /* nach freeze vergleichen */
{
    static fe x, y;
    fcpy(x, a);
    fcpy(y, b);
    fe_freeze(x);
    fe_freeze(y);
    return cr_eq(x, y, sizeof(fe));
}

/* r = a + b (auch r == a oder r == b) */
static void padd(struct ep *r, const struct ep *a, const struct ep *b)
{
    static fe A, B, C, D, E, F, G, H, t;
    fe_sub(A, a->y, a->x);
    fe_sub(t, b->y, b->x);
    fe_mul(A, A, t);
    fe_add(B, a->y, a->x);
    fe_add(t, b->y, b->x);
    fe_mul(B, B, t);
    fe_mul(C, a->t, D2);
    fe_mul(C, C, b->t);
    fe_add(D, a->z, a->z);
    fe_mul(D, D, b->z);
    fe_sub(E, B, A);
    fe_sub(F, D, C);
    fe_add(G, D, C);
    fe_add(H, B, A);
    fe_mul(r->x, E, F);
    fe_mul(r->y, G, H);
    fe_mul(r->t, E, H);
    fe_mul(r->z, F, G);
}

/* o = z^(2^252 - 3) = z^((p-5)/8) */
static void fe_p58(u16 *o, const u16 *z)
{
    static fe c;
    int a;
    fcpy(c, z);                         /* Bit 251                        */
    for (a = 250; a >= 0; a--) {
        fe_mul(c, c, c);
        if (a != 1)
            fe_mul(c, c, z);
    }
    fcpy(o, c);
}

/* 32 Byte -> Punkt (RFC 8032 5.1.3) -> 0 ok */
static int decode(struct ep *P, const u8 *s)
{
    static fe u, v, v3, x2, t;
    static const fe one = { 1 };
    unsigned i, sign = s[31] >> 7;
    for (i = 0; i < 16; i++)
        P->y[i] = (u16)(s[2 * i] | (u16)s[2 * i + 1] << 8);
    P->y[15] &= 0x7FFF;
    fcpy(t, P->y);
    fe_freeze(t);
    if (!cr_eq(t, P->y, sizeof(fe)))    /* y >= p                         */
        return 1;
    fe_mul(u, P->y, P->y);
    fe_mul(v, u, DD);
    fe_sub(u, u, one);                  /* u = y^2 - 1                    */
    fe_add(v, v, one);                  /* v = d y^2 + 1                  */
    fe_mul(v3, v, v);
    fe_mul(v3, v3, v);                  /* v^3                            */
    fe_mul(t, v3, v3);
    fe_mul(t, t, v);                    /* v^7                            */
    fe_mul(t, t, u);
    fe_p58(t, t);
    fe_mul(t, t, v3);
    fe_mul(P->x, t, u);                 /* x = u v^3 (u v^7)^((p-5)/8)    */
    fe_mul(x2, P->x, P->x);
    fe_mul(x2, x2, v);                  /* v x^2                          */
    if (!fe_same(x2, u)) {
        fe_sub(t, u, u);
        fe_sub(t, t, u);                /* -u                             */
        if (!fe_same(x2, t))
            return 1;
        fe_mul(P->x, P->x, SQM1);
    }
    fe_freeze(P->x);
    if ((P->x[0] & 1) != sign) {
        fe_sub(t, t, t);
        fe_sub(P->x, t, P->x);
        fe_freeze(P->x);
    }
    for (i = 0; i < 16; i++)
        P->z[i] = 0;
    P->z[0] = 1;
    fe_mul(P->t, P->x, P->y);
    return 0;
}

static void encode(u8 *s, const struct ep *P)
{
    static fe zi, x, y;
    unsigned i;
    fe_inv(zi, P->z);
    fe_mul(x, P->x, zi);
    fe_mul(y, P->y, zi);
    fe_freeze(x);
    fe_freeze(y);
    for (i = 0; i < 16; i++) {
        s[2 * i] = (u8)y[i];
        s[2 * i + 1] = (u8)(y[i] >> 8);
    }
    s[31] |= (u8)((x[0] & 1) << 7);
}

/* r = h (64 Byte, Little Endian) mod L, bitweise */
static void modl(u16 *r, const u8 *h)
{
    static u16 t[16];
    int b;
    unsigned i, c, br;
    u32 v;
    for (i = 0; i < 16; i++)
        r[i] = 0;
    for (b = 511; b >= 0; b--) {
        c = (h[b >> 3] >> (b & 7)) & 1;
        for (i = 0; i < 16; i++) {      /* r = 2 r + Bit                  */
            v = (u32)r[i] << 1 | c;
            r[i] = (u16)v;
            c = (unsigned)(v >> 16);
        }
        br = 0;
        for (i = 0; i < 16; i++) {      /* r >= L -> r -= L               */
            v = (u32)r[i] - LL[i] - br;
            t[i] = (u16)v;
            br = (unsigned)(v >> 16) & 1;
        }
        if (!br)
            for (i = 0; i < 16; i++)
                r[i] = t[i];
    }
}

int ed25519_verify(const u8 pk[32], const u8 *m, unsigned n, const u8 sig[64])
{
    static struct sha512 c;
    static struct ep A, Q;
    static u16 k[16];
    static u8 h[64], rr[32];
    unsigned i, sb, kb;
    int b;
    /* S < L */
    for (b = 15; b >= 0; b--) {
        i = sig[32 + 2 * b] | (unsigned)sig[33 + 2 * b] << 8;
        if (i != LL[b])
            break;
    }
    if (b < 0 || i > LL[b])
        return 1;
    if (decode(&A, pk))
        return 1;
    fe_sub(Q.x, Q.x, Q.x);              /* -A                             */
    fe_sub(A.x, Q.x, A.x);
    fe_sub(A.t, Q.x, A.t);
    sha512_init(&c, 0);
    sha512_add(&c, sig, 32);
    sha512_add(&c, pk, 32);
    sha512_add(&c, m, n);
    sha512_end(&c, h);
    modl(k, h);
    for (i = 0; i < 16; i++)            /* Q = neutral (0, 1, 1, 0)       */
        Q.x[i] = Q.y[i] = Q.z[i] = Q.t[i] = 0;
    Q.y[0] = Q.z[0] = 1;
    for (b = 255; b >= 0; b--) {        /* Q = [S] B + [k] (-A)           */
        padd(&Q, &Q, &Q);
        sb = (sig[32 + (b >> 3)] >> (b & 7)) & 1;
        kb = (k[b >> 4] >> (b & 15)) & 1;
        if (sb)
            padd(&Q, &Q, &BP);
        if (kb)
            padd(&Q, &Q, &A);
    }
    encode(rr, &Q);
    return !cr_eq(rr, sig, 32);
}
