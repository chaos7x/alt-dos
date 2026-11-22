/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Zufall fuer Schluessel (TLS, SMB): Pool des Kernels
 * (FAPI_RANDOM) mit Datum/Zeit und Zittern einer Zaehlschleife gehasht
 */
#define FAPI_NO_IMPL
#include "fapi.h"
#include "crypto.h"
#include "tls.h"

/* ---- Zufall ------------------------------------------------------------- */
void tls_random(u8 *d, unsigned n)
{
    static u8 pool[34], seed[32], t[32];
    static struct sha256 c;
    static unsigned long ctr;
    unsigned k, i, t0;
    if (!ctr) {                         /* einmal: Pool + Zeit + Zittern  */
        sha256_init(&c);
        fapi_call(FAPI_RANDOM, (unsigned)pool, sizeof(pool), 0);
        sha256_add(&c, pool, sizeof(pool));
        DosGetDateTime(t);
        sha256_add(&c, t, 16);
        for (k = 0; k < 4; k++) {       /* Schleifen je Takt schwanken    */
            t0 = (unsigned)DosQTicks();
            for (i = 0; (unsigned)DosQTicks() == t0; i++)
                ;
            sha256_add(&c, &i, sizeof(i));
        }
        sha256_end(&c, seed);
    }
    while (n) {
        ctr++;
        sha256_init(&c);
        sha256_add(&c, seed, 32);
        sha256_add(&c, &ctr, sizeof(ctr));
        fapi_call(FAPI_RANDOM, (unsigned)pool, sizeof(pool), 0);
        sha256_add(&c, pool, sizeof(pool));
        sha256_end(&c, t);
        k = n < 32 ? n : 32;
        cr_cpy(d, t, k);
        d += k;
        n -= k;
    }
}

