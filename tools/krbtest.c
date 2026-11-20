/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pruefprogramm fuer apps/krbcrypt.c auf dem Entwicklungsrechner:
     gcc -DCRYPTO_HOST -I apps -o kt tools/krbtest.c apps/krbcrypt.c apps/crypto.c
     ./kt | python3 tools/krbtest.py
   Gibt Zeilen "art|hex...|ergebnis" aus, krbtest.py rechnet sie mit
   Python (hashlib, cryptography) nach. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "krbcrypt.h"

static void hx(const u8 *p, unsigned n) { while (n--) printf("%02x", *p++); }
static unsigned long st = 1;
static u8 rb(void) { st = st * 1103515245UL + 12345; return (u8)(st >> 16); }
static void rnd(u8 *p, unsigned n) { while (n--) *p++ = rb(); }

int main(int argc, char **argv)
{
    static u8 a[3000], b[3100], k[32], c[16], o[64];
    struct sha1 s; struct hmac1 h; struct aes ae;
    unsigned i, n, r, pl; u8 *pt;
    if (argc > 1) st = strtoul(argv[1], 0, 10);
    /* RFC 3962 Anhang B */
    krb_s2k("password", 8, "ATHENA.MIT.EDUraeburn", 21, 1, o);
    printf("fix|s2k1|"); hx(o, 32); printf("\n");
    krb_s2k("password", 8, "ATHENA.MIT.EDUraeburn", 21, 1200, o);
    printf("fix|s2k1200|"); hx(o, 32); printf("\n");
    nfold((const u8 *)"kerberos", 8, o, 16);
    printf("fix|nf|"); hx(o, 16); printf("\n");
    nfold((const u8 *)"012345", 6, o, 8);
    printf("fix|nf6|"); hx(o, 8); printf("\n");
    for (r = 0; r < 80; r++) {
        n = r < 40 ? r : rb() * 11 % 2900;
        rnd(a, n);
        sha1_init(&s); sha1_add(&s, a, n); sha1_end(&s, o);
        printf("sha1|"); hx(a, n); printf("|"); hx(o, 20); printf("\n");
        i = rb() % 100; rnd(b, i);
        hmac1_key(&h, b, i); hmac1(&h, a, n, o);
        printf("hmac1|"); hx(b, i); printf("|"); hx(a, n); printf("|"); hx(o, 20); printf("\n");
        rnd(k, 32); rnd(c, 16);
        aes_keyl(&ae, k, (r & 1) ? 32 : 16); aes_enc(&ae, c, o); aes_dec(&ae, o, o + 16);
        printf("aes|"); hx(k, (r & 1) ? 32 : 16); printf("|"); hx(c, 16); printf("|"); hx(o, 32); printf("\n");
        if (r < 8) {
            i = 1 + rb() % 20;
            krb_s2k(a, n > 30 ? 30 : n, b, i, 1 + rb() % 50, o);
        }
        krb_enc(k, r * 7, c, a, n, b);
        printf("enc|"); hx(k, 32); printf("|%u|", r * 7); hx(c, 16); printf("|"); hx(a, n); printf("|"); hx(b, n + 28); printf("\n");
        if (krb_dec(k, r * 7, b, n + 28, &pt, &pl) || pl != n || memcmp(pt, a, n))
            printf("FEHLER dec %u\n", n);
        b[0] ^= 1;
        krb_enc(k, r, c, a, n, b); b[n + 27] ^= 1;
        if (!krb_dec(k, r, b, n + 28, &pt, &pl)) printf("FEHLER dec nimmt Falsches\n");
        krb_cksum(k, r, a, n, o);
        printf("ck|"); hx(k, 32); printf("|%u|", r); hx(a, n); printf("|"); hx(o, 12); printf("\n");
    }
    return 0;
}
