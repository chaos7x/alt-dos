/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - TLS-1.2-Client (Schnittstelle tls.h)
 *
 * Eigene Umsetzung nach RFC 5246 (TLS 1.2), RFC 8422 (ECDHE), RFC 5288
 * (AES-GCM), RFC 7627 (Extended Master Secret), RFC 6066 (SNI), RFC 5746
 * (renegotiation_info); nur gelesen. Ablauf: ClientHello -> ServerHello,
 * Certificate, ServerKeyExchange (X25519), ServerHelloDone ->
 * ClientKeyExchange, ChangeCipherSpec, Finished -> ChangeCipherSpec,
 * Finished. Danach Datensaetze mit AES-128-GCM (explizite Nonce = Folge-
 * nummer). Ein Datensatz liegt ganz im Puffer rb (bis 2^14 + 2048 Byte).
 * Alles statisch im DGROUP des Programms (rund 22 KB).
 */
#define FAPI_NO_IMPL
#include "fapi.h"
#include "crypto.h"
#include "tls.h"

#define CS_RSA      0xC02F              /* ECDHE_RSA_WITH_AES_128_GCM_SHA256 */
#define CS_ECDSA    0xC02B
#define RMAX        (16384 + 2048)

unsigned char tls_alert;

static struct fapi_sockio sio;
static unsigned sk, tmo;
static u8 ib[2048];                     /* Empfang vom Socket             */
static unsigned ipos, ilen;
static u8 rb[RMAX + 8];                 /* ein Datensatz (Klartext ab rp) */
static unsigned rtype, rlen, rpos;      /* Typ, Laenge, gelesen           */
static u8 ob[5 + 8 + 4096 + 16];        /* ein ausgehender Datensatz      */
static u8 hs[2048];                     /* Handshake-Nachricht            */
static struct sha256 th;                /* Verlauf (transcript)           */
static u8 crnd[32], srnd[32], msec[48], kb[40];
static u8 cseq[8], sseq[8];
static struct gcm cg, sg;
static unsigned cenc, senc, ems, eof;

static void put16(u8 *p, unsigned v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static unsigned get16(const u8 *p) { return (unsigned)p[0] << 8 | p[1]; }

static void seq_inc(u8 *s)
{
    int i;
    for (i = 7; i >= 0; i--)
        if (++s[i])
            break;
}

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

/* ---- Socket ------------------------------------------------------------- */
static int sget(u8 *d, unsigned n)      /* genau n Byte -> 0              */
{
    unsigned r, k;
    while (n) {
        if (ipos >= ilen) {
            sio.h = sk;
            sio.buf = (unsigned)ib;
            sio.len = sizeof(ib);
            sio.flags = 0;
            sio.timeout = tmo;
            r = fapi_call(FAPI_NET, NET_RECV, (unsigned)&sio, sizeof(sio));
            if (r == 0 || r >= 0xFFF0)
                return 1;
            ipos = 0;
            ilen = r;
        }
        k = ilen - ipos;
        if (k > n)
            k = n;
        cr_cpy(d, ib + ipos, k);
        ipos += k;
        d += k;
        n -= k;
    }
    return 0;
}

static int sput(const u8 *d, unsigned n)
{
    unsigned r;
    while (n) {
        sio.h = sk;
        sio.buf = (unsigned)d;
        sio.len = n;
        sio.flags = 0;
        sio.timeout = tmo;
        r = fapi_call(FAPI_NET, NET_SEND, (unsigned)&sio, sizeof(sio));
        if (r == 0 || r >= 0xFFF0)
            return 1;
        d += r;
        n -= r;
    }
    return 0;
}

/* ---- Datensaetze -------------------------------------------------------- */
static void mknonce(u8 *iv, const u8 *fix, const u8 *ex)
{
    cr_cpy(iv, fix, 4);
    cr_cpy(iv + 4, ex, 8);
}

static void mkaad(u8 *a, const u8 *seq, unsigned type, unsigned len)
{
    cr_cpy(a, seq, 8);
    a[8] = (u8)type;
    a[9] = 3;
    a[10] = 3;
    put16(a + 11, len);
}

/* Datensatz senden (n <= 4096) */
static int rec_send(unsigned type, const u8 *d, unsigned n)
{
    u8 iv[12], aad[13];
    ob[0] = (u8)type;
    ob[1] = 3;
    ob[2] = 3;
    if (!cenc) {
        put16(ob + 3, n);
        cr_cpy(ob + 5, d, n);
        return sput(ob, 5 + n);
    }
    put16(ob + 3, n + 24);
    cr_cpy(ob + 5, cseq, 8);            /* explizite Nonce = Folgenummer  */
    cr_cpy(ob + 13, d, n);
    mknonce(iv, kb + 32, cseq);
    mkaad(aad, cseq, type, n);
    gcm_seal(&cg, iv, aad, 13, ob + 13, n, ob + 13 + n);
    seq_inc(cseq);
    return sput(ob, 5 + 8 + n + 16);
}

static int fail(int e);

static void alert_send(unsigned lvl, unsigned desc)
{
    u8 a[2];
    a[0] = (u8)lvl;
    a[1] = (u8)desc;
    rec_send(21, a, 2);
}

/* naechsten Datensatz holen (entschluesselt) -> 0, sonst TLSE_* */
static int rec_next(void)
{
    u8 h[5], iv[12], aad[13];
    unsigned n;
    if (sget(h, 5))
        return TLSE_NET;
    n = get16(h + 3);
    if (h[1] != 3 || n > RMAX || (h[0] < 20 || h[0] > 23))
        return TLSE_PROTO;
    if (sget(rb, n))
        return TLSE_NET;
    rtype = h[0];
    rpos = 0;
    if (senc) {
        if (n < 24)
            return TLSE_MAC;
        mknonce(iv, kb + 36, rb);
        mkaad(aad, sseq, rtype, n - 24);
        if (gcm_open(&sg, iv, aad, 13, rb + 8, n - 24, rb + n - 16))
            return TLSE_MAC;
        seq_inc(sseq);
        cr_cpy(rb, rb + 8, n - 24);
        n -= 24;
    }
    rlen = n;
    if (rtype == 21 && n >= 2) {        /* Meldung                        */
        tls_alert = rb[1];
        if (rb[1] == 0) {               /* close_notify                   */
            eof = 1;
            return TLSE_OK;
        }
        if (rb[0] == 2)
            return TLSE_ALERT;
        rlen = 0;                       /* Warnung: uebergehen            */
    }
    return TLSE_OK;
}

/* Handshake-Strom: genau n Byte, auch ueber Datensaetze hinweg */
static int hget(u8 *d, unsigned n)
{
    unsigned k;
    int e;
    while (n) {
        while (rpos >= rlen || rtype != 22) {
            if ((e = rec_next()) != 0)
                return e;
            if (eof || (rtype != 22 && rlen))
                return TLSE_PROTO;
        }
        k = rlen - rpos;
        if (k > n)
            k = n;
        if (d) {
            cr_cpy(d, rb + rpos, k);
            d += k;
        } else
            sha256_add(&th, rb + rpos, k);  /* ueberspringen, nur zaehlen */
        rpos += k;
        n -= k;
    }
    return 0;
}

/* naechste Handshake-Nachricht -> Typ, Laenge in *len; Inhalt in hs (bis
   sizeof(hs), groessere - Certificate - werden nur in den Verlauf
   gerechnet), -1 bei Fehler (Fehler in *len) */
static int hmsg(unsigned *len)
{
    u8 h[4];
    unsigned long n;
    int e;
    if ((e = hget(h, 4)) != 0) {
        *len = e;
        return -1;
    }
    n = (unsigned long)h[1] << 16 | (unsigned)h[2] << 8 | h[3];
    sha256_add(&th, h, 4);
    if (n > sizeof(hs)) {
        while (n) {
            unsigned k = n > 4096 ? 4096 : (unsigned)n;
            if ((e = hget(0, k)) != 0) {
                *len = e;
                return -1;
            }
            n -= k;
        }
        *len = 0;
        return h[0];
    }
    if ((e = hget(hs, (unsigned)n)) != 0) {
        *len = e;
        return -1;
    }
    sha256_add(&th, hs, (unsigned)n);
    *len = (unsigned)n;
    return h[0];
}

/* Handshake-Nachricht senden (Inhalt in ob ab 9, Laenge n) */
static int hsend(unsigned type, const u8 *d, unsigned n)
{
    static u8 m[4 + 512];
    m[0] = (u8)type;
    m[1] = 0;
    put16(m + 2, n);
    cr_cpy(m + 4, d, n);
    sha256_add(&th, m, 4 + n);
    return rec_send(22, m, 4 + n);
}

static int fail(int e)
{
    if (e == TLSE_PROTO)
        alert_send(2, 10);              /* unexpected_message             */
    else if (e == TLSE_MAC)
        alert_send(2, 20);              /* bad_record_mac                 */
    else if (e == TLSE_CIPHER)
        alert_send(2, 40);              /* handshake_failure              */
    return e;
}

static unsigned is_ip(const char *h)
{
    for (; *h; h++)
        if (!(*h >= '0' && *h <= '9') && *h != '.' && *h != ':' &&
            !((*h | 0x20) >= 'a' && (*h | 0x20) <= 'f'))
            return 0;
    return 1;
}

int tls_open(unsigned sock, const char *host, unsigned tmo_s, unsigned flags)
{
    static u8 m[512], cpriv[32], cpub[32], spub[32], pms[32], vd[12], hh[32], seed[64];
    static struct sha256 tc;
    unsigned n, len, hl, i, cs = 0, ext;
    int t;
    u8 *p;

    if (!(flags & TLS_NOVERIFY))
        return TLSE_NOVERIFY;
    sk = sock;
    tmo = tmo_s > 60 ? 60000U : tmo_s * 1000U;
    ipos = ilen = rlen = rpos = 0;
    cenc = senc = ems = eof = 0;
    tls_alert = 0;
    cr_zero(cseq, 8);
    cr_zero(sseq, 8);
    sha256_init(&th);
    tls_random(crnd, 32);

    /* ClientHello */
    p = m;
    *p++ = 3; *p++ = 3;
    cr_cpy(p, crnd, 32); p += 32;
    *p++ = 0;                           /* keine Sitzung                  */
    *p++ = 0; *p++ = 4;
    put16(p, CS_ECDSA); p += 2;
    put16(p, CS_RSA); p += 2;
    *p++ = 1; *p++ = 0;                 /* keine Kompression              */
    p += 2;                             /* Laenge der Erweiterungen       */
    ext = (unsigned)(p - m);
    for (hl = 0; host[hl]; hl++)
        ;
    if (hl && hl < 200 && !is_ip(host)) {   /* server_name                */
        put16(p, 0); p += 2;
        put16(p, hl + 5); p += 2;
        put16(p, hl + 3); p += 2;
        *p++ = 0;
        put16(p, hl); p += 2;
        cr_cpy(p, host, hl); p += hl;
    }
    put16(p, 0x000A); p += 2;           /* supported_groups: x25519, dazu */
    put16(p, 6); p += 2;                /* secp256r1 - ohne die Kurve des */
    put16(p, 4); p += 2;                /* Zertifikats nimmt ein Server    */
    put16(p, 0x001D); p += 2;           /* sein ECDSA-Zertifikat nicht    */
    put16(p, 0x0017); p += 2;           /* (RFC 8422 5.1); ECDHE nur X25519 */
    put16(p, 0x000B); p += 2;           /* ec_point_formats: uncompressed */
    put16(p, 2); p += 2;
    *p++ = 1; *p++ = 0;
    put16(p, 0x000D); p += 2;           /* signature_algorithms           */
    put16(p, 10); p += 2;
    put16(p, 8); p += 2;
    put16(p, 0x0403); p += 2;           /* ecdsa_secp256r1_sha256         */
    put16(p, 0x0804); p += 2;           /* rsa_pss_rsae_sha256            */
    put16(p, 0x0401); p += 2;           /* rsa_pkcs1_sha256               */
    put16(p, 0x0501); p += 2;           /* rsa_pkcs1_sha384               */
    put16(p, 0x0017); p += 2;           /* extended_master_secret         */
    put16(p, 0); p += 2;
    put16(p, 0xFF01); p += 2;           /* renegotiation_info (leer)      */
    put16(p, 1); p += 2;
    *p++ = 0;
    put16(m + ext - 2, (unsigned)(p - m) - ext);
    if (hsend(1, m, (unsigned)(p - m)))
        return TLSE_NET;

    /* Antwort des Servers bis ServerHelloDone */
    for (;;) {
        t = hmsg(&len);
        if (t < 0)
            return len == TLSE_PROTO ? fail(TLSE_PROTO) : (int)len;
        if (t == 2) {                   /* ServerHello                    */
            if (len < 38 || hs[0] != 3 || hs[1] != 3)
                return fail(TLSE_PROTO);
            cr_cpy(srnd, hs + 2, 32);
            n = 35 + hs[34];
            if (n + 3 > len)
                return fail(TLSE_PROTO);
            cs = get16(hs + n);
            if ((cs != CS_RSA && cs != CS_ECDSA) || hs[n + 2] != 0)
                return fail(TLSE_CIPHER);
            n += 3;
            if (n + 2 <= len) {         /* Erweiterungen                  */
                unsigned e = n + 2, end = n + 2 + get16(hs + n);
                if (end > len)
                    return fail(TLSE_PROTO);
                while (e + 4 <= end) {
                    if (get16(hs + e) == 0x0017)
                        ems = 1;
                    e += 4 + get16(hs + e + 2);
                }
            }
        } else if (t == 11) {           /* Certificate (Stufe 1: nicht geprueft) */
        } else if (t == 12) {           /* ServerKeyExchange              */
            if (len < 4 + 32 || hs[0] != 3 || get16(hs + 1) != 0x001D || hs[3] != 32)
                return fail(TLSE_CIPHER);
            cr_cpy(spub, hs + 4, 32);
        } else if (t == 13) {
            fail(TLSE_CIPHER);
            return TLSE_CERTREQ;
        } else if (t == 14)             /* ServerHelloDone                */
            break;
        else
            return fail(TLSE_PROTO);
    }
    if (!cs)
        return fail(TLSE_PROTO);

    /* ClientKeyExchange, Schluessel */
    tls_random(cpriv, 32);
    x25519_base(cpub, cpriv);
    x25519(pms, cpriv, spub);
    for (i = n = 0; i < 32; i++)
        n |= pms[i];
    if (!n)
        return fail(TLSE_CIPHER);
    m[0] = 32;
    cr_cpy(m + 1, cpub, 32);
    if (hsend(16, m, 33))
        return TLSE_NET;
    if (ems) {                          /* RFC 7627: Hash bis hierher     */
        cr_cpy(&tc, &th, sizeof(tc));
        sha256_end(&tc, hh);
        tls_prf(pms, 32, "extended master secret", hh, 32, msec, 48);
    } else {
        cr_cpy(seed, crnd, 32);
        cr_cpy(seed + 32, srnd, 32);
        tls_prf(pms, 32, "master secret", seed, 64, msec, 48);
    }
    cr_cpy(seed, srnd, 32);
    cr_cpy(seed + 32, crnd, 32);
    tls_prf(msec, 48, "key expansion", seed, 64, kb, 40);
    gcm_key(&cg, kb);
    gcm_key(&sg, kb + 16);
    cr_zero(cpriv, 32);
    cr_zero(pms, 32);

    m[0] = 1;                           /* ChangeCipherSpec               */
    if (rec_send(20, m, 1))
        return TLSE_NET;
    cenc = 1;
    cr_cpy(&tc, &th, sizeof(tc));
    sha256_end(&tc, hh);
    tls_prf(msec, 48, "client finished", hh, 32, vd, 12);
    if (hsend(20, vd, 12))
        return TLSE_NET;

    /* ChangeCipherSpec und Finished des Servers */
    for (;;) {
        if (rpos >= rlen && (t = rec_next()) != 0)
            return t == TLSE_PROTO || t == TLSE_MAC ? fail(t) : t;
        if (eof)
            return TLSE_NET;
        if (rtype == 20) {
            if (rlen != 1 || rb[0] != 1 || senc)
                return fail(TLSE_PROTO);
            senc = 1;
            rlen = rpos = 0;
            continue;
        }
        if (rtype == 22 && senc)
            break;
        if (rpos < rlen)
            return fail(TLSE_PROTO);
    }
    cr_cpy(&tc, &th, sizeof(tc));
    sha256_end(&tc, hh);
    tls_prf(msec, 48, "server finished", hh, 32, vd, 12);
    t = hmsg(&len);
    if (t != 20 || len != 12 || !cr_eq(hs, vd, 12))
        return fail(TLSE_MAC);
    return TLSE_OK;
}

int tls_read(void *b, unsigned n)
{
    unsigned k;
    int e;
    while (rpos >= rlen || rtype != 23) {
        if (eof)
            return 0;
        if ((e = rec_next()) != 0) {
            fail(e);
            return -1;
        }
        if (eof)
            return 0;
        if (rtype == 22) {              /* HelloRequest u. Ae.: ablehnen  */
            alert_send(1, 100);         /* no_renegotiation (Warnung)     */
            rlen = 0;
        }
    }
    k = rlen - rpos;
    if (k > n)
        k = n;
    cr_cpy(b, rb + rpos, k);
    rpos += k;
    return (int)k;
}

int tls_write(const void *b, unsigned n)
{
    const u8 *p = b;
    unsigned k, left = n;
    while (left) {
        k = left > 4096 ? 4096 : left;
        if (rec_send(23, p, k))
            return -1;
        p += k;
        left -= k;
    }
    return (int)n;
}

void tls_close(void)
{
    if (cenc)
        alert_send(1, 0);               /* close_notify                   */
    cenc = senc = 0;
    cr_zero(msec, 48);
    cr_zero(kb, 40);
}

const char *tls_msg(int e)
{
    switch (e) {
    case TLSE_NET:      return "TLS: Verbindung abgebrochen oder keine Antwort.";
    case TLSE_PROTO:    return "TLS: unerwartete Antwort des Servers.";
    case TLSE_ALERT:    return "TLS: der Server hat die Verbindung abgelehnt.";
    case TLSE_CIPHER:   return "TLS: kein gemeinsames Verfahren (n\x94tig: ECDHE X25519 mit AES-128-GCM).";
    case TLSE_MAC:      return "TLS: Pr\x81" "fsumme falsch - Daten unterwegs ver\x84ndert?";
    case TLSE_NOVERIFY: return "TLS: Pr\x81" "fung des Servers fehlt noch - mit --no-check-certificate trotzdem.";
    case TLSE_CERTREQ:  return "TLS: der Server verlangt ein Client-Zertifikat.";
    }
    return "TLS: Fehler.";
}
