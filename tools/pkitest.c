/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pruefprogramm fuer apps/pki.c auf dem Entwicklungsrechner (mit
   tools/pkitest.py):
     gcc -DCRYPTO_HOST -I apps -o pt tools/pkitest.c apps/pki.c apps/crypto.c
     python3 tools/pkitest.py | ./pt
   Zeilen von stdin: "sha alg hexdaten hexhash" | "sig typ sc halg hexhash
   hexsig hexA hexE erwartet" | "crt hexder" (selbstsigniert: mit eigenem
   Schluessel pruefen) | "name hexder rechner erwartet"; Ausgabe: Zahl der
   Abweichungen. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pki.h"

static unsigned hexin(const char *s, u8 *o)
{
    unsigned n = 0;
    while (s[0] && s[1] && s[0] != ' ' && s[0] != '\n') {
        sscanf(s, "%2hhx", &o[n++]);
        s += 2;
    }
    return n;
}

static char ln[40000];
static u8 b1[8192], b2[8192], b3[8192], b4[64];

int main(void)
{
    unsigned bad = 0, cnt = 0, n1, n2, n3, n4;
    char *f[10];
    while (fgets(ln, sizeof(ln), stdin)) {
        int k = 0;
        char *p = strtok(ln, " \n");
        while (p && k < 10) { f[k++] = p; p = strtok(0, " \n"); }
        if (!k) continue;
        cnt++;
        if (!strcmp(f[0], "sha")) {
            int alg = atoi(f[1]);
            u8 h[64];
            n1 = hexin(f[2][0] == '-' ? "" : f[2], b1);
            n2 = hexin(f[3], b2);
            hash_far(alg, b1, n1, h);
            if (memcmp(h, b2, n2)) { bad++; printf("FALSCH sha %d len %u\n", alg, n1); }
        } else if (!strcmp(f[0], "sig")) {
            struct pkey key;
            int r;
            key.type = (u8)atoi(f[1]);
            n1 = hexin(f[4], b4);
            n2 = hexin(f[5], b1);
            n3 = hexin(f[6], b2);
            n4 = hexin(f[7], b3);
            key.a = b2; key.alen = n3; key.e = b3; key.elen = n4;
            r = pk_verify(&key, atoi(f[2]), atoi(f[3]), b4, b1, n2);
            if ((r == 0) != (atoi(f[8]) == 1)) { bad++; printf("FALSCH sig typ %s sc %s h %s erwartet %s\n", f[1], f[2], f[3], f[8]); }
        } else if (!strcmp(f[0], "crt")) {
            struct cert c;
            n1 = hexin(f[1], b1);
            if (x509_parse(b1, n1, &c)) { bad++; printf("FALSCH parse\n"); continue; }
            if (!c.key.type || !c.sc) { printf("uebergangen (Verfahren %d/%d)\n", c.key.type, c.sc); cnt--; continue; }
            if (!x509_sig_ok(&c, &c.key)) { bad++; printf("FALSCH crt sig key %d sc %d h %d\n", c.key.type, c.sc, c.halg); }
            if (!der_eq(c.iss, c.isslen, c.sub, c.sublen)) { bad++; printf("FALSCH iss!=sub\n"); }
        } else if (!strcmp(f[0], "name")) {
            struct cert c;
            n1 = hexin(f[1], b1);
            if (x509_parse(b1, n1, &c)) { bad++; printf("FALSCH parse name\n"); continue; }
            if (x509_name_ok(&c, f[2]) != atoi(f[3])) { bad++; printf("FALSCH name %s erwartet %s\n", f[2], f[3]); }
        }
    }
    printf("%u Faelle, %u falsch\n", cnt, bad);
    return bad != 0;
}
