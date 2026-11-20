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
#ifndef MK_FP
#define MK_FP(s, o) ((void __far *)(((unsigned long)(s) << 16) | (unsigned)(o)))
#endif
#include "crypto.h"
#include "pki.h"
#include "tls.h"

#define CS_RSA      0xC02F              /* ECDHE_RSA_WITH_AES_128_GCM_SHA256 */
#define CS_ECDSA    0xC02B
#define RMAX        (16384 + 256)     /* AES-GCM: Klartext 2^14 + 24 */

unsigned char tls_alert;
char tls_why[80];

#define CMAX        40960u              /* Certificate-Nachricht (fern)   */
#define CAOFF       CMAX                /* ein Zertifikat aus der CA-Liste */
#define CASZ        16384u
#define NCRT        8
static unsigned cseg;                   /* 64 KB per DosAllocSeg          */
static struct cert crt[NCRT], cac;
static unsigned ncrt, verify;
static char capath[FAPI_PATHLEN];
static u8 ske[512];                     /* Signatur des ServerKeyExchange  */
static unsigned skelen;

static struct fapi_sockio sio;
static unsigned sk, tmo;
static u8 ib[1024];                     /* Empfang vom Socket             */
static unsigned ipos, ilen;
static u8 rb[RMAX + 8];                 /* ein Datensatz (Klartext ab rp) */
static unsigned rtype, rlen, rpos;      /* Typ, Laenge, gelesen           */
static u8 ob[5 + 8 + 2048 + 16];        /* ein ausgehender Datensatz      */
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

/* Datensatz senden (n <= 2048) */
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
    if (h[0] == 11 && verify) {         /* Certificate fern ablegen       */
        u8 FAR *f = MK_FP(cseg, 0);
        unsigned k, i, got = 0;
        if (n > CMAX) {
            *len = TLSE_CERT;
            return -1;
        }
        while (got < (unsigned)n) {
            k = (unsigned)n - got > sizeof(hs) ? sizeof(hs) : (unsigned)n - got;
            if ((e = hget(hs, k)) != 0) {
                *len = e;
                return -1;
            }
            sha256_add(&th, hs, k);
            for (i = 0; i < k; i++)
                f[got + i] = hs[i];
            got += k;
        }
        *len = (unsigned)n;
        return 11;
    }
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

/* ---- Pruefung des Servers --------------------------------------------- */
void tls_ca(const char *path)
{
    unsigned i;
    for (i = 0; path[i] && i < sizeof(capath) - 1; i++)
        capath[i] = path[i];
    capath[i] = 0;
}

static void why(const char *a, const u8 FAR *b, unsigned bn)
{
    unsigned i = 0;
    while (*a && i < sizeof(tls_why) - 1)
        tls_why[i++] = *a++;
    while (b && bn-- && i < sizeof(tls_why) - 1)
        tls_why[i++] = *b >= 32 && *b < 127 ? (char)*b++ : (b++, '?');
    tls_why[i] = 0;
}

/* Certificate-Nachricht (fern ab 0) zerlegen -> 0 */
static int certs_parse(unsigned len)
{
    const u8 FAR *f = MK_FP(cseg, 0);
    unsigned long tot, k;
    unsigned p = 3;
    ncrt = 0;
    if (len < 3)
        return TLSE_CERT;
    tot = (unsigned long)f[0] << 16 | (unsigned)f[1] << 8 | f[2];
    if (tot + 3 != len)
        return TLSE_PROTO;
    while (p + 3 <= len && ncrt < NCRT) {
        k = (unsigned long)f[p] << 16 | (unsigned)f[p + 1] << 8 | f[p + 2];
        p += 3;
        if (p + k > len || x509_parse(f + p, (unsigned)k, &crt[ncrt]))
            return TLSE_CERT;
        p += (unsigned)k;
        ncrt++;
    }
    return ncrt ? 0 : TLSE_CERT;
}

static unsigned dnum(const char *s)     /* JJJJMMTT -> Tag (ungefaehr)    */
{
    unsigned y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    unsigned m = (s[4] - '0') * 10 + (s[5] - '0'), d = (s[6] - '0') * 10 + (s[7] - '0');
    return (y - 1980) * 372 + (m - 1) * 31 + d;
}

/* Base64-Zeichen -> Wert, 64 = keins */
static unsigned b64(u8 c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 64;
}

/* Aussteller von c in der CA-Liste suchen und Signatur pruefen ->
   0 gefunden und gueltig, 1 nicht gefunden, TLSE_NOCA */
static int ca_find(const struct cert *c)
{
    static u8 lb[512];
    static char line[80];
    u8 FAR *f = MK_FP(cseg, CAOFF);
    unsigned ll = 0, n, i, in = 0, acc = 0, bits = 0, got = 0, v;
    int h = DosOpen(capath);
    if (FAPI_FAILED(h))
        return TLSE_NOCA;
    while ((n = DosRead(h, lb, sizeof(lb))) != 0 && n < 0xFFF0) {
        for (i = 0; i < n; i++) {
            u8 ch = lb[i];
            if (ch != '\n') {
                if (ll < sizeof(line) - 1 && ch != '\r')
                    line[ll++] = (char)ch;
                continue;
            }
            line[ll] = 0;
            if (line[0] == '-' && line[5] == 'B') {         /* BEGIN       */
                in = 1;
                acc = bits = got = 0;
            } else if (line[0] == '-' && line[5] == 'E' && in) {    /* END */
                in = 0;
                if (!x509_parse(f, got, &cac) &&
                    der_eq(cac.sub, cac.sublen, c->iss, c->isslen) &&
                    x509_sig_ok(c, &cac.key)) {
                    DosClose(h);
                    return 0;
                }
            } else if (in) {
                for (ll = 0; line[ll]; ll++) {
                    if ((v = b64((u8)line[ll])) == 64)
                        continue;
                    acc = acc << 6 | v;
                    bits += 6;
                    if (bits >= 8) {
                        bits -= 8;
                        if (got < CASZ)
                            f[got++] = (u8)(acc >> bits);
                    }
                }
            }
            ll = 0;
        }
    }
    DosClose(h);
    return 1;
}

static int chain_ok(const char *host)
{
    static u8 dt[4];
    static char now[9];
    unsigned d, t, i, j, cur = 0, depth, used = 1, today;
    int r;
    if (!x509_name_ok(&crt[0], host)) {
        why("Zertifikat ist f\x81r ", crt[0].cn, crt[0].cnlen);
        return TLSE_NAME;
    }
    DosGetDateTime(dt);
    d = dt[0] | dt[1] << 8;
    t = (d >> 9) + 1980;
    now[0] = (char)('0' + t / 1000);
    now[1] = (char)('0' + t / 100 % 10);
    now[2] = (char)('0' + t / 10 % 10);
    now[3] = (char)('0' + t % 10);
    now[4] = (char)('0' + ((d >> 5) & 15) / 10);
    now[5] = (char)('0' + ((d >> 5) & 15) % 10);
    now[6] = (char)('0' + (d & 31) / 10);
    now[7] = (char)('0' + (d & 31) % 10);
    today = dnum(now);
    for (depth = 0; depth < NCRT; depth++) {
        if (dnum(crt[cur].nb) > today + 1 || dnum(crt[cur].na) + 1 < today) {
            why("G\x81ltig bis ", (const u8 FAR *)crt[cur].na, 8);
            return TLSE_DATE;
        }
        if ((r = ca_find(&crt[cur])) == 0)
            return 0;
        if (r == TLSE_NOCA) {
            why(capath, 0, 0);
            return TLSE_NOCA;
        }
        for (j = 0; j < ncrt; j++)      /* Aussteller in der Kette       */
            if (!(used >> j & 1) &&
                der_eq(crt[j].sub, crt[j].sublen, crt[cur].iss, crt[cur].isslen))
                break;
        if (j == ncrt || crt[j].ca != 1 || !x509_sig_ok(&crt[cur], &crt[j].key)) {
            i = cur;
            why("unbekannter Aussteller von ", crt[i].cn, crt[i].cnlen);
            return TLSE_CERT;
        }
        used |= 1 << j;
        cur = j;
    }
    return TLSE_CERT;
}

/* Signatur des ServerKeyExchange (ske: Signaturverfahren, Laenge, Signatur)
   ueber Zufall + Parameter mit dem Schluessel des ersten Zertifikats */
static int ske_ok(const u8 *prm, unsigned plen)
{
    static u8 h[64];
    static struct sha256 s;
    static struct sha512 x;
    unsigned alg, sl;
    int sc, ha;
    if (skelen < 4)
        return TLSE_SIG;
    alg = get16(ske);
    sl = get16(ske + 2);
    if (sl + 4 > skelen)
        return TLSE_SIG;
    ha = (alg >> 8) == 4 || alg == 0x0804 ? H_SHA256 : (alg >> 8) == 5 || alg == 0x0805 ? H_SHA384 : H_SHA512;
    sc = (alg & 0xFF) == 1 ? SC_PKCS1 : (alg & 0xFF) == 3 ? SC_ECDSA :
         (alg >= 0x0804 && alg <= 0x0806) ? SC_PSS : 0;
    if (alg >= 0x0804 && alg <= 0x0806)
        ha = alg == 0x0804 ? H_SHA256 : alg == 0x0805 ? H_SHA384 : H_SHA512;
    if (!sc)
        return TLSE_SIG;
    if (ha == H_SHA256) {
        sha256_init(&s);
        sha256_add(&s, crnd, 32);
        sha256_add(&s, srnd, 32);
        sha256_add(&s, prm, plen);
        sha256_end(&s, h);
    } else {
        sha512_init(&x, ha == H_SHA384);
        sha512_add(&x, crnd, 32);
        sha512_add(&x, srnd, 32);
        sha512_add(&x, prm, plen);
        sha512_end(&x, h);
    }
    return pk_verify(&crt[0].key, sc, ha, h, (const u8 FAR *)(ske + 4), sl) ? TLSE_SIG : 0;
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

    verify = !(flags & TLS_NOVERIFY);
    tls_why[0] = 0;
    if (verify && !cseg) {
        cseg = DosAllocSeg(0);
        if (FAPI_FAILED(cseg)) {
            cseg = 0;
            return TLSE_MEM;
        }
    }
    if (verify && !capath[0]) {         /* Vorgabe: Startlaufwerk          */
        static char cs[FAPI_PATHLEN];
        static const char tail[] = ":\\DOS\\SYSTEM\\CACERT.PEM";
        cs[0] = 'C';
        DosGetEnv("COMSPEC", cs, sizeof(cs));
        capath[0] = cs[0];
        for (i = 0; tail[i]; i++)
            capath[1 + i] = tail[i];
        capath[1 + i] = 0;
    }
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
    put16(p, 14); p += 2;
    put16(p, 12); p += 2;
    put16(p, 0x0403); p += 2;           /* ecdsa_secp256r1_sha256         */
    put16(p, 0x0503); p += 2;           /* ecdsa_secp384r1_sha384         */
    put16(p, 0x0805); p += 2;           /* rsa_pss_rsae_sha384            */
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
        } else if (t == 11) {           /* Certificate                    */
            if (verify && (n = certs_parse(len)) != 0)
                return fail(n == TLSE_PROTO ? TLSE_PROTO : TLSE_CIPHER), (int)n;
        } else if (t == 12) {           /* ServerKeyExchange              */
            if (len < 4 + 32 || hs[0] != 3 || get16(hs + 1) != 0x001D || hs[3] != 32)
                return fail(TLSE_CIPHER);
            cr_cpy(spub, hs + 4, 32);
            skelen = len - 36 > sizeof(ske) ? sizeof(ske) : len - 36;
            cr_cpy(ske, hs + 36, skelen);
            if (verify) {
                if (!ncrt)
                    return fail(TLSE_PROTO);
                if ((n = ske_ok(hs, 36)) != 0) {
                    alert_send(2, 51);  /* decrypt_error                  */
                    return (int)n;
                }
                if ((n = chain_ok(host)) != 0) {
                    alert_send(2, n == TLSE_CERT ? 48 : n == TLSE_DATE ? 45 : 42);
                    return (int)n;
                }
            }
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
        k = left > 2048 ? 2048 : left;
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
    case TLSE_CERT:     return "TLS: Zertifikat nicht vertrauensw\x81rdig (Aussteller unbekannt oder Kette ung\x81ltig).";
    case TLSE_NAME:     return "TLS: Zertifikat gilt nicht f\x81r diesen Rechner.";
    case TLSE_DATE:     return "TLS: Zertifikat abgelaufen oder noch nicht g\x81ltig (Uhr pr\x81" "fen, SNTP).";
    case TLSE_SIG:      return "TLS: Signatur des Servers falsch.";
    case TLSE_NOCA:     return "TLS: CA-Liste fehlt (\\DOS\\SYSTEM\\CACERT.PEM).";
    case TLSE_MEM:      return "TLS: zu wenig Speicher.";
    }
    return "TLS: Fehler.";
}
