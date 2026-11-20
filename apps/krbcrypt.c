/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Kryptografie fuer Kerberos (Schnittstelle in krbcrypt.h)
 */
#include "krbcrypt.h"

/* ---- SHA-1 -------------------------------------------------------------- */
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_blk(struct sha1 *c)
{
    static u32 w[80];                   /* (Stapel der Programme: 2 KB)  */
    u32 a, b, d, e, f, k, t, cc;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = (u32)c->b[4 * i] << 24 | (u32)c->b[4 * i + 1] << 16 |
               (u32)c->b[4 * i + 2] << 8 | c->b[4 * i + 3];
    for (; i < 80; i++) {
        t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = ROL(t, 1);
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999UL;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1UL;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCUL;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6UL;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = ROL(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_init(struct sha1 *c)
{
    c->h[0] = 0x67452301UL;
    c->h[1] = 0xEFCDAB89UL;
    c->h[2] = 0x98BADCFEUL;
    c->h[3] = 0x10325476UL;
    c->h[4] = 0xC3D2E1F0UL;
    c->n = 0;
    c->k = 0;
}

void sha1_add(struct sha1 *c, const void *d, unsigned n)
{
    const u8 *p = d;
    c->n += n;
    while (n--) {
        c->b[c->k++] = *p++;
        if (c->k == 64) {
            sha1_blk(c);
            c->k = 0;
        }
    }
}

void sha1_end(struct sha1 *c, u8 out[20])
{
    u32 bits = c->n << 3, hb = c->n >> 29;
    unsigned i;
    c->b[c->k++] = 0x80;
    if (c->k > 56) {
        while (c->k < 64)
            c->b[c->k++] = 0;
        sha1_blk(c);
        c->k = 0;
    }
    while (c->k < 56)
        c->b[c->k++] = 0;
    for (i = 0; i < 4; i++) {
        c->b[56 + i] = (u8)(hb >> (24 - 8 * i));
        c->b[60 + i] = (u8)(bits >> (24 - 8 * i));
    }
    sha1_blk(c);
    for (i = 0; i < 20; i++)
        out[i] = (u8)(c->h[i >> 2] >> (24 - 8 * (i & 3)));
}

/* ---- HMAC-SHA1 ---------------------------------------------------------- */
void hmac1_key(struct hmac1 *k, const void *key, unsigned klen)
{
    u8 b[64];
    unsigned i;
    cr_zero(b, 64);
    if (klen > 64) {
        sha1_init(&k->in);
        sha1_add(&k->in, key, klen);
        sha1_end(&k->in, b);
    } else
        cr_cpy(b, key, klen);
    for (i = 0; i < 64; i++)
        b[i] ^= 0x36;
    sha1_init(&k->in);
    sha1_add(&k->in, b, 64);
    for (i = 0; i < 64; i++)
        b[i] ^= 0x36 ^ 0x5C;
    sha1_init(&k->out);
    sha1_add(&k->out, b, 64);
    cr_zero(b, 64);
}

void hmac1(const struct hmac1 *k, const void *d, unsigned n, u8 out[20])
{
    static struct sha1 c;
    u8 t[20];
    cr_cpy(&c, &k->in, sizeof(c));
    sha1_add(&c, d, n);
    sha1_end(&c, t);
    cr_cpy(&c, &k->out, sizeof(c));
    sha1_add(&c, t, 20);
    sha1_end(&c, out);
}

/* ---- PBKDF2-HMAC-SHA1 --------------------------------------------------- */
void pbkdf2_sha1(const void *pw, unsigned plen, const void *salt, unsigned slen,
                 u32 iter, u8 *out, unsigned n)
{
    static struct hmac1 k;
    static u8 s[132];
    u8 u[20], t[20];
    unsigned blk = 1, i, m;
    u32 j;
    hmac1_key(&k, pw, plen);
    if (slen > 128)
        slen = 128;
    cr_cpy(s, salt, slen);
    while (n) {
        s[slen] = 0;
        s[slen + 1] = 0;
        s[slen + 2] = (u8)(blk >> 8);
        s[slen + 3] = (u8)blk;
        hmac1(&k, s, slen + 4, u);
        cr_cpy(t, u, 20);
        for (j = 1; j < iter; j++) {
            hmac1(&k, u, 20, u);
            for (i = 0; i < 20; i++)
                t[i] ^= u[i];
        }
        m = n < 20 ? n : 20;
        cr_cpy(out, t, m);
        out += m;
        n -= m;
        blk++;
    }
}

/* ---- n-fold (RFC 3961 Abschnitt 5.1) ------------------------------------
   Eingabe so oft hintereinander, bis kgV(inlen, outlen) erreicht ist, jede
   Kopie um weitere 13 Bit nach rechts gedreht; die Stuecke zu outlen Byte
   werden im Einerkomplement addiert. kgV bis 128 (Kerberos braucht 80). */
void nfold(const u8 *in, unsigned inlen, u8 *out, unsigned outlen)
{
    static u8 big[128];
    unsigned a = inlen, b = outlen, l, k, j, src, rot, bits = inlen * 8, s;
    while (b) {
        k = a % b;
        a = b;
        b = k;
    }
    l = inlen / a * outlen;             /* kgV                           */
    if (l > sizeof(big))
        l = sizeof(big);
    cr_zero(big, l);
    for (k = 0; k * inlen < l; k++) {
        rot = (13 * k) % bits;
        for (j = 0; j < bits; j++) {    /* Bit j der Kopie = Bit j - rot  */
            src = (j + bits - rot) % bits;
            if (in[src >> 3] & (0x80 >> (src & 7)))
                big[k * inlen + (j >> 3)] |= (u8)(0x80 >> (j & 7));
        }
    }
    cr_zero(out, outlen);
    for (k = 0; k < l; k += outlen) {   /* Einerkomplement-Summe         */
        s = 0;
        for (j = outlen; j-- > 0;) {
            s += out[j] + big[k + j];
            out[j] = (u8)s;
            s >>= 8;
        }
        while (s) {                     /* Uebertrag hinten wieder rein  */
            for (j = outlen; j-- > 0 && s;) {
                s += out[j];
                out[j] = (u8)s;
                s >>= 8;
            }
        }
    }
}

/* ---- DK, Schluessel aus Kennwort ---------------------------------------- */
static struct aes ka;

/* DK(key, constant) = random-to-key(DR): E(nfold(c)), E(E(...)), 32 Byte */
static void dk(const u8 key[32], const u8 *c, unsigned clen, u8 out[32])
{
    u8 b[16];
    aes_keyl(&ka, key, 32);
    nfold(c, clen, b, 16);
    aes_enc(&ka, b, out);
    aes_enc(&ka, out, out + 16);
}

void krb_s2k(const void *pw, unsigned plen, const void *salt, unsigned slen,
             u32 iter, u8 key[32])
{
    u8 t[32];
    pbkdf2_sha1(pw, plen, salt, slen, iter, t, 32);
    dk(t, (const u8 *)"kerberos", 8, key);
    cr_zero(t, 32);
}

static void ukey(const u8 key[32], u32 usage, u8 kind, u8 out[32])
{
    u8 c[5];
    c[0] = (u8)(usage >> 24);
    c[1] = (u8)(usage >> 16);
    c[2] = (u8)(usage >> 8);
    c[3] = (u8)usage;
    c[4] = kind;
    dk(key, c, 5, out);
}

/* ---- AES-CTS (RFC 3962), IV 0 ------------------------------------------- */
static void cts_enc(const struct aes *a, u8 *d, unsigned n)
{
    u8 prev[16], t[16], last[16];
    unsigned m = (n + 15) / 16, r = n - 16 * (m - 1), i, k;
    if (n == 16) {
        aes_enc(a, d, d);
        return;
    }
    cr_zero(prev, 16);
    for (k = 0; k + 1 < m; k++) {       /* volle Bloecke CBC              */
        for (i = 0; i < 16; i++)
            t[i] = d[16 * k + i] ^ prev[i];
        aes_enc(a, t, prev);
        cr_cpy(d + 16 * k, prev, 16);
    }
    /* letzter Block mit Nullen aufgefuellt; prev = C(m-1) */
    for (i = 0; i < 16; i++)
        t[i] = (i < r ? d[16 * (m - 1) + i] : 0) ^ prev[i];
    aes_enc(a, t, last);
    /* tauschen: an m-2 kommt C(m), an m-1 die ersten r Byte von C(m-1) */
    cr_cpy(d + 16 * (m - 1), prev, r);
    cr_cpy(d + 16 * (m - 2), last, 16);
}

static void cts_dec(const struct aes *a, u8 *d, unsigned n)
{
    u8 prev[16], c[16], t[16], cm1[16];
    unsigned m = (n + 15) / 16, r = n - 16 * (m - 1), i, k;
    if (n == 16) {
        aes_dec(a, d, d);
        return;
    }
    cr_zero(prev, 16);
    for (k = 0; k + 2 < m; k++) {
        cr_cpy(c, d + 16 * k, 16);
        aes_dec(a, c, t);
        for (i = 0; i < 16; i++)
            d[16 * k + i] = t[i] ^ prev[i];
        cr_cpy(prev, c, 16);
    }
    aes_dec(a, d + 16 * (m - 2), t);    /* D(C(m)) = P(m) ^ C(m-1) aufgef. */
    for (i = 0; i < 16; i++)
        cm1[i] = i < r ? d[16 * (m - 1) + i] : t[i];
    for (i = 0; i < r; i++)
        d[16 * (m - 1) + i] = t[i] ^ cm1[i];
    aes_dec(a, cm1, t);
    for (i = 0; i < 16; i++)
        d[16 * (m - 2) + i] = t[i] ^ prev[i];
}

/* ---- Verschluesselungsrahmen (RFC 3961 Abschnitt 5.3) ------------------- */
static struct hmac1 kh;

void krb_enc(const u8 key[32], u32 usage, const u8 conf[16],
             const u8 *d, unsigned n, u8 *out)
{
    u8 k[32], h[20];
    cr_cpy(out, conf, 16);
    cr_cpy(out + 16, d, n);
    ukey(key, usage, 0x55, k);          /* Ki                            */
    hmac1_key(&kh, k, 32);
    hmac1(&kh, out, n + 16, h);
    ukey(key, usage, 0xAA, k);          /* Ke                            */
    aes_keyl(&ka, k, 32);
    cts_enc(&ka, out, n + 16);
    cr_cpy(out + n + 16, h, 12);
    cr_zero(k, 32);
}

int krb_dec(const u8 key[32], u32 usage, u8 *d, unsigned n,
            u8 **pt, unsigned *plen)
{
    u8 k[32], h[20];
    if (n < 28)
        return -1;
    n -= 12;
    ukey(key, usage, 0xAA, k);
    aes_keyl(&ka, k, 32);
    cts_dec(&ka, d, n);
    ukey(key, usage, 0x55, k);
    hmac1_key(&kh, k, 32);
    hmac1(&kh, d, n, h);
    cr_zero(k, 32);
    if (!cr_eq(h, d + n, 12))
        return -1;
    *pt = d + 16;
    *plen = n - 16;
    return 0;
}

void krb_cksum(const u8 key[32], u32 usage, const u8 *d, unsigned n, u8 out[12])
{
    u8 k[32], h[20];
    ukey(key, usage, 0x99, k);          /* Kc                            */
    hmac1_key(&kh, k, 32);
    hmac1(&kh, d, n, h);
    cr_cpy(out, h, 12);
    cr_zero(k, 32);
}

/* ---- rc4-hmac (Etype 23, RFC 4757) ---------------------------------------
   Rueckfall fuer Konten ohne AES-Schluessel (Kennwort aus der Zeit vor
   Windows 2008/Samba 4). Schluessel = MD4 des Kennworts in UTF-16LE
   (der NT-Hash), Verwendung 3 gilt als 8. */
static u8 rc_s[256];

static void rc4(const u8 *key, unsigned kl, u8 *d, unsigned n)
{
    unsigned i, j = 0, k;
    u8 t;
    for (i = 0; i < 256; i++)
        rc_s[i] = (u8)i;
    for (i = 0; i < 256; i++) {
        j = (j + rc_s[i] + key[i % kl]) & 255;
        t = rc_s[i]; rc_s[i] = rc_s[j]; rc_s[j] = t;
    }
    i = j = 0;
    for (k = 0; k < n; k++) {
        i = (i + 1) & 255;
        j = (j + rc_s[i]) & 255;
        t = rc_s[i]; rc_s[i] = rc_s[j]; rc_s[j] = t;
        d[k] ^= rc_s[(rc_s[i] + rc_s[j]) & 255];
    }
}

void krb_key23(const char *pw, unsigned plen, u8 key[16])
{
    static struct md5 c;
    u8 u[2];
    unsigned i;
    md5_init(&c, 1);
    u[1] = 0;
    for (i = 0; i < plen; i++) {
        u[0] = (u8)pw[i];
        md5_add(&c, u, 2);
    }
    md5_end(&c, key);
}

static void ms_us(u32 usage, u8 t[4])
{
    if (usage == 3)
        usage = 8;
    t[0] = (u8)usage; t[1] = (u8)(usage >> 8);
    t[2] = (u8)(usage >> 16); t[3] = (u8)(usage >> 24);
}

void krb_enc23(const u8 key[16], u32 usage, const u8 conf[8],
               const u8 *d, unsigned n, u8 *out)
{
    u8 t[4], k1[16], k3[16];
    ms_us(usage, t);
    hmac_md5(key, 16, t, 4, k1);
    cr_cpy(out + 16, conf, 8);
    cr_cpy(out + 24, d, n);
    hmac_md5(k1, 16, out + 16, n + 8, out);         /* Pruefsumme vorn  */
    hmac_md5(k1, 16, out, 16, k3);
    rc4(k3, 16, out + 16, n + 8);
    cr_zero(k1, 16);
    cr_zero(k3, 16);
}

int krb_dec23(const u8 key[16], u32 usage, u8 *d, unsigned n,
              u8 **pt, unsigned *plen)
{
    u8 t[4], k1[16], k3[16], h[16];
    if (n < 24)
        return -1;
    ms_us(usage, t);
    hmac_md5(key, 16, t, 4, k1);
    hmac_md5(k1, 16, d, 16, k3);
    rc4(k3, 16, d + 16, n - 16);
    hmac_md5(k1, 16, d + 16, n - 16, h);
    cr_zero(k1, 16);
    cr_zero(k3, 16);
    if (!cr_eq(h, d, 16))
        return -1;
    *pt = d + 24;
    *plen = n - 24;
    return 0;
}

void krb_cksum23(const u8 key[16], u32 usage, const u8 *d, unsigned n, u8 out[16])
{
    static struct md5 c;
    u8 ks[16], t[4], h[16];
    hmac_md5(key, 16, (const u8 *)"signaturekey", 13, ks);
    ms_us(usage, t);
    md5_init(&c, 0);
    md5_add(&c, t, 4);
    md5_add(&c, d, n);
    md5_end(&c, h);
    hmac_md5(ks, 16, h, 16, out);
    cr_zero(ks, 16);
}
