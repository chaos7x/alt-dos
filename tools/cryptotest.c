/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pruefprogramm fuer apps/crypto.c auf dem Entwicklungsrechner:
     gcc -DCRYPTO_HOST -I apps -o ct tools/cryptotest.c apps/crypto.c
     ./ct | python3 tools/cryptotest.py
   Liest Befehle von stdin nicht; gibt fuer feste und zufaellige Eingaben
   (Startwert als Argument) Zeilen "art hex-eingaben -> hex-ergebnis" aus,
   die cryptotest.py mit Python (cryptography/hashlib) nachrechnet. */
#include <stdio.h>
#include <stdlib.h>
#include "crypto.h"

static void hx(const u8 *p, unsigned n) { while (n--) printf("%02x", *p++); }
static unsigned long st = 1;
static u8 rb(void) { st = st * 1103515245UL + 12345; return (u8)(st >> 16); }
static void rnd(u8 *p, unsigned n) { while (n--) *p++ = rb(); }

int main(int argc, char **argv)
{
    static u8 a[3000], b[64], k[32], iv[12], o[64], tag[16], ad[40];
    struct sha256 s; struct hmac h; struct gcm g;
    unsigned i, n, r;
    if (argc > 1) st = strtoul(argv[1], 0, 10);
    for (r = 0; r < 60; r++) {
        n = r < 20 ? r * 7 : rb() * 11 % 2900;
        rnd(a, n);
        sha256_init(&s); sha256_add(&s, a, n); sha256_end(&s, o);
        printf("sha|"); hx(a, n); printf("|"); hx(o, 32); printf("\n");
        i = rb() % 100; rnd(b, i > 64 ? 64 : i);
        hmac_init(&h, b, i > 64 ? 64 : i); hmac_add(&h, a, n); hmac_end(&h, o);
        printf("hmac|"); hx(b, i > 64 ? 64 : i); printf("|"); hx(a, n); printf("|"); hx(o, 32); printf("\n");
        rnd(k, 16); rnd(iv, 12); i = rb() % 40; rnd(ad, i);
        printf("gcm|"); hx(k, 16); printf("|"); hx(iv, 12); printf("|"); hx(ad, i); printf("|"); hx(a, n);
        gcm_key(&g, k); gcm_seal(&g, iv, ad, i, a, n, tag);
        printf("|"); hx(a, n); hx(tag, 16); printf("\n");
        if (gcm_open(&g, iv, ad, i, a, n, tag)) printf("FEHLER gcm_open\n");
        rnd(k, 32); rnd(o, 32);
        printf("x25519|"); hx(k, 32); printf("|"); hx(o, 32); printf("|");
        x25519(b, k, o); hx(b, 32); printf("\n");
        { struct md5 m; md5_init(&m, 0); md5_add(&m, a, n); md5_end(&m, o);
          printf("md5|"); hx(a, n); printf("|"); hx(o, 16); printf("\n");
          md5_init(&m, 1); md5_add(&m, a, n); md5_end(&m, o);
          printf("md4|"); hx(a, n); printf("|"); hx(o, 16); printf("\n");
          i = rb() % 80; rnd(b, i > 64 ? 64 : i); hmac_md5(b, i > 64 ? 64 : i, a, n, o);
          printf("hmd5|"); hx(b, i > 64 ? 64 : i); printf("|"); hx(a, n); printf("|"); hx(o, 16); printf("\n"); }
        rnd(b, 48); rnd(a, 64);
        tls_prf(b, 48, "key expansion", a, 64, o, 40);
        printf("prf|"); hx(b, 48); printf("|"); hx(a, 64); printf("|"); hx(o, 40); printf("\n");
    }
    return 0;
}
