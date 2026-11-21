/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SMBCLI.EXE - Dateien auf Windows-/Samba-Freigaben (SMB2), Bedienung
 * angelehnt an smbclient, eigener Code nach [MS-SMB2], [MS-NLMP] und
 * RFC 4178 (SPNEGO), nur gelesen:
 *   SMBCLI \\Rechner\Freigabe [-U Benutzer[%Kennwort]] [-W Domaene]
 *          [-k] [-c "Befehl; Befehl"] [-p Port] [-4|-6]
 * Befehle: dir/ls [Muster], cd [Verz|..|\], pwd, get Datei [lokal],
 * put lokal [Datei], del Datei, md Verz, rd Verz, lcd [Verz], help, quit.
 * Dialekte SMB 2.0.2 und 2.1, Anmeldung NTLMv2 in SPNEGO (ohne Kennwort:
 * Gast) oder mit -k Kerberos (Ticket von KINIT, krb5.c), signiert mit HMAC-SHA256, wenn der Server es verlangt. Namen in
 * CP437 <-> UTF-16. Dateien bis 4 GB, Stuecke zu 16 KB.
 * ERRORLEVEL: 0 ok, 1 ein Befehl scheiterte, 2 Aufruf, 4 Netz/Anmeldung.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"
#include "crypto.h"
#include "tls.h"                        /* tls_random (rnd.c)            */
#include "arcutil.h"
#include "krb5.h"

#define TMO     30000u
#define CHUNK   16384u

static char args[FAPI_ARGLEN], line[300], host[128], share[128], user[64], pass[64];
static char dom[64], rdir[FAPI_PATHLEN], a1[FAPI_PATHLEN], a2[FAPI_PATHLEN], ts[64];
static char cmds[FAPI_ARGLEN];
static struct fapi_socket so;
static struct fapi_sockio io;
static unsigned char ad[16];
static unsigned sock = 0xFFFF, port = 445, failed, havepass, kerb;
static const char *kmsg;                 /* Meldung der Kerberos-Anmeldung */
static u8 tx[64 + 128 + CHUNK + 4], rx[64 + 64 + CHUNK + 8];
static unsigned rxn;                    /* Laenge der Antwort (ohne NetBIOS) */
static u8 skey[16];
static unsigned signing, dialect;
static u32 msgid;
static u8 sess[8], tree[4];
static u16 ub[300];                     /* UTF-16                        */
static char pwf[FAPI_PATHLEN];          /* -P: Kennwortdatei (NET USE)    */
static unsigned srv;                    /* --serve: Laufwerk + 1          */
static unsigned lost;                   /* Dienst: Verbindung verloren    */
#define FAR8 /* nah */
#define MK_FP(s, o) ((void __far *)(((unsigned long)(s) << 16) | (unsigned)(o)))

static const unsigned short cp437u[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

/* ---- Ausgabe ------------------------------------------------------ */
static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }
static void con(const char *s, unsigned n) { VioWrtTTY(s, n); }

static char *ldec(unsigned long v, char *p)    /* -> Ende (ohne Division) */
{
    static const unsigned long p10[10] = { 1000000000UL, 100000000UL, 10000000UL,
        1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    unsigned i, d, any = 0;
    for (i = 0; i < 10; i++) {
        for (d = 0; v >= p10[i]; d++)
            v -= p10[i];
        if (d || any || i == 9) {
            *p++ = (char)('0' + d);
            any = 1;
        }
    }
    *p = 0;
    return p;
}

static void put16(u8 *p, unsigned v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (unsigned)v); put16(p + 2, (unsigned)(v >> 16)); }
static unsigned get16(const u8 *p) { return p[0] | (unsigned)p[1] << 8; }
static u32 get32(const u8 *p) { return get16(p) | (u32)get16(p + 2) << 16; }

/* CP437 -> UTF-16 (ub), -> Zahl der Zeichen */
static unsigned to16(const char *s, u16 *d, unsigned max)
{
    unsigned n = 0;
    u8 c;
    while ((c = (u8)*s++) != 0 && n < max)
        d[n++] = c < 0x80 ? c : cp437u[c - 0x80];
    return n;
}

static char from16(unsigned u)
{
    unsigned i;
    if (u < 0x80)
        return (char)u;
    for (i = 0; i < 128; i++)
        if (cp437u[i] == u)
            return (char)(0x80 + i);
    return '_';
}

/* ---- Eingabe (wie FTP) --------------------------------------------- */
static char ib[512];
static unsigned ipos, ilen, ieof;

static int in_byte(void)
{
    if (ipos >= ilen) {
        unsigned r = DosRead(STDIN, ib, sizeof(ib));
        if (r == 0 || FAPI_FAILED(r))
            return -1;
        ilen = r;
        ipos = 0;
    }
    return (unsigned char)ib[ipos++];
}

static int get_line(const char *pr, char *b, unsigned max, int echo)
{
    unsigned n = 0, c;
    int k;
    if (pr)
        out(pr);
    if (DosQHandType(STDIN) != HT_CONSOLE) {
        if (ieof)
            return -1;
        while ((k = in_byte()) >= 0 && k != '\n')
            if (k != '\r' && n < max - 1)
                b[n++] = (char)k;
        b[n] = 0;
        if (k < 0) {
            ieof = 1;
            if (!n)
                return -1;
        }
        if (echo)
            out(b);
        out("\r\n");
        return 0;
    }
    for (;;) {
        c = KbdCharIn();
        if ((c & 0xFF) == '\r') {
            b[n] = 0;
            con("\r\n", 2);
            return 0;
        }
        if ((c & 0xFF) == '\b') {
            if (n) {
                n--;
                if (echo)
                    con("\b \b", 3);
            }
        } else if (!KEY_EXT(c & 0xFF, c >> 8) && (c & 0xFF) >= ' ' && n < max - 1) {
            b[n++] = (char)c;
            if (echo)
                con(b + n - 1, 1);
        }
    }
}

/* ---- Netz ------------------------------------------------------------ */
static unsigned sio(unsigned fn, void *b, unsigned len)
{
    io.h = sock;
    io.buf = (unsigned)b;
    io.len = len;
    io.flags = 0;
    io.timeout = TMO;
    return DosNet(fn, &io, sizeof(io));
}

static int sget(u8 *d, unsigned n)
{
    unsigned r;
    while (n) {
        r = sio(NET_RECV, d, n);
        if (r == 0 || FAPI_FAILED(r))
            return 1;
        d += r;
        n -= r;
    }
    return 0;
}

static int sput(const u8 *d, unsigned n)
{
    unsigned r;
    while (n) {
        r = sio(NET_SEND, (void *)d, n);
        if (r == 0 || FAPI_FAILED(r))
            return 1;
        d += r;
        n -= r;
    }
    return 0;
}

/* ---- SMB2 ------------------------------------------------------------ */
#define S_OK            0x00000000UL
#define S_PENDING       0x00000103UL
#define S_MORE          0xC0000016UL
#define S_NOMORE        0x80000006UL
#define S_EOF           0xC0000011UL
#define C_NEG   0
#define C_SESS  1
#define C_LOGOFF 2
#define C_TREE  3
#define C_TDIS  4
#define C_CREATE 5
#define C_CLOSE 6
#define C_READ  8
#define C_WRITE 9
#define C_QDIR  0x0E

/* Anfrage: Kopf in tx+4 bauen, Rumpf liegt schon ab tx+4+64 (blen Byte),
   senden, Antwort nach rx -> NT-Status */
static u32 smb(unsigned cmd, unsigned blen)
{
    static u8 mac[32];
    static struct hmac hm;
    u8 *h = tx + 4;
    unsigned n = 64 + blen, i;
    u32 st;
    for (i = 0; i < 64; i++)
        h[i] = 0;
    h[0] = 0xFE; h[1] = 'S'; h[2] = 'M'; h[3] = 'B';
    h[4] = 64;
    if (dialect >= 0x0210)
        h[6] = 1;                       /* CreditCharge                  */
    put16(h + 12, cmd);
    put16(h + 14, 64);                  /* CreditRequest                 */
    put32(h + 24, msgid);
    msgid++;
    put32(h + 32, 0xFEFFUL);            /* ProcessId                     */
    cr_cpy(h + 36, tree, 4);
    cr_cpy(h + 40, sess, 8);
    if (signing && cmd != C_NEG && cmd != C_SESS) {
        h[16] |= 8;                     /* SMB2_FLAGS_SIGNED             */
        hmac_init(&hm, skey, 16);
        hmac_add(&hm, h, n);
        hmac_end(&hm, mac);
        cr_cpy(h + 48, mac, 16);
    }
    tx[0] = 0;
    tx[1] = 0;
    tx[2] = (u8)(n >> 8);
    tx[3] = (u8)n;
    if (sput(tx, n + 4)) {
        lost = 1;
        return 0xFFFFFFFFUL;
    }
    for (;;) {                          /* Antwort (Zwischenantwort PENDING uebergehen) */
        u8 nb[4];
        unsigned long l;
        if (sget(nb, 4)) {
            lost = 1;
            return 0xFFFFFFFFUL;
        }
        l = (unsigned long)nb[1] << 16 | (unsigned)nb[2] << 8 | nb[3];
        if (l < 64 || l > sizeof(rx)) {
            while (l) {                 /* zu gross: verwerfen            */
                unsigned k = l > sizeof(rx) ? sizeof(rx) : (unsigned)l;
                if (sget(rx, k)) {
                    lost = 1;
                    return 0xFFFFFFFFUL;
                }
                l -= k;
            }
            return 0xFFFFFFFEUL;
        }
        if (sget(rx, (unsigned)l)) {
            lost = 1;
            return 0xFFFFFFFFUL;
        }
        rxn = (unsigned)l;
        st = get32(rx + 8);
        if (st == S_PENDING && (rx[16] & 2))
            continue;
        if (st == 0xC000035CUL || st == 0xC0000203UL || st == 0xC00000C9UL)
            lost = 1;                   /* Sitzung/Freigabe weg (Server neu) */
        return st;
    }
}

#define BODY (tx + 4 + 64)
#define RB   (rx + 64)

/* DER-Kopf (Etikett t, Laenge n) nach d -> Laenge des Kopfes */
static unsigned dh(u8 *d, unsigned t, unsigned n)
{
    d[0] = (u8)t;
    if (n < 128) {
        d[1] = (u8)n;
        return 2;
    }
    if (n < 256) {
        d[1] = 0x81;
        d[2] = (u8)n;
        return 3;
    }
    d[1] = 0x82;
    d[2] = (u8)(n >> 8);
    d[3] = (u8)n;
    return 4;
}

static unsigned dl(unsigned n) { return n < 128 ? 2 : n < 256 ? 3 : 4; }

static const u8 oid_spnego[] = { 0x06, 0x06, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02 };
static const u8 oid_ntlm[] = { 0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0A };

static const u8 oid_krb5[] = { 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x12, 0x01, 0x02, 0x02 };

/* Token t (tl Byte, NTLM oder Kerberos) in SPNEGO nach d -> Laenge */
static unsigned spnego2(u8 *d, const u8 *t, unsigned tl, int first, const u8 *mo, unsigned lm)
{
    unsigned l4 = dl(tl) + tl, la2 = dl(l4) + l4, p = 0;
    if (first) {                        /* NegTokenInit                  */
        unsigned l30a = dl(lm) + lm, la0 = dl(l30a) + l30a;
        unsigned l30 = la0 + la2, la0o = dl(dl(l30) + l30) + dl(l30) + l30;
        unsigned tot = sizeof(oid_spnego) + la0o;
        p += dh(d + p, 0x60, tot);
        cr_cpy(d + p, oid_spnego, sizeof(oid_spnego)); p += sizeof(oid_spnego);
        p += dh(d + p, 0xA0, dl(l30) + l30);
        p += dh(d + p, 0x30, l30);
        p += dh(d + p, 0xA0, l30a);
        p += dh(d + p, 0x30, lm);
        cr_cpy(d + p, mo, lm); p += lm;
    } else {                            /* NegTokenResp                  */
        unsigned l30 = la2;
        p += dh(d + p, 0xA1, dl(l30) + l30);
        p += dh(d + p, 0x30, l30);
    }
    p += dh(d + p, 0xA2, l4);
    p += dh(d + p, 0x04, tl);
    cr_cpy(d + p, t, tl);
    return p + tl;
}

static unsigned spnego(u8 *d, const u8 *t, unsigned tl, int first)
{
    return spnego2(d, t, tl, first, oid_ntlm, sizeof(oid_ntlm));
}

/* Anmeldung mit Kerberos (-k): TGT aus der Ablage (KINIT), Dienstticket
   cifs/Rechner aus der Ablage oder vom KDC; Puffer: Anfrage/TGT in tx,
   Antwort/Dienstticket in rx (DGROUP ist knapp) -> 0 ok */
static int klogin(void)
{
    struct kcred *tgt = (struct kcred *)(tx + KBUF), *sc = (struct kcred *)(rx + KBUF);
    static char sn[100];
    unsigned n, i;
    u32 st;
    if (krb_cc_open() <= 0 || krb_cc_get(0, tgt)) {
        kmsg = "Kein Kerberos-Ticket (erst KINIT).";
        return 1;
    }
    if (krb_now() >= tgt->end) {
        kmsg = "Kerberos-Ticket abgelaufen (KINIT).";
        return 1;
    }
    scopy(sn, "cifs/", sizeof(sn));
    for (i = 0; host[i] && i + 6 < sizeof(sn); i++)
        sn[5 + i] = (char)(host[i] >= 'A' && host[i] <= 'Z' ? host[i] + 32 : host[i]);
    sn[5 + i] = 0;
    if (krb_cc_find(sn, sc) < 0 || krb_now() + 60 >= sc->end) {
        krb_bufs(tx, rx);
        if ((kmsg = krb_tgs(tgt, "cifs", host, sc)) != 0)
            return 1;
        krb_cc_put(sc);
    }
    cr_cpy(skey, sc->key, 16);          /* SMB2: erste 16 Byte            */
    n = krb_gss(sc, rx, KBUF);
    if (!n) {
        kmsg = "Ticket zu gross.";
        return 1;
    }
    cr_zero(BODY, 24);
    n = spnego2(BODY + 24, rx, n, 1, oid_krb5, sizeof(oid_krb5));
    BODY[0] = 25;
    BODY[3] = 1;
    put16(BODY + 12, 64 + 24);
    put16(BODY + 14, n);
    st = smb(C_SESS, 24 + n);
    if (st != S_OK) {
        kmsg = "Server lehnt das Kerberos-Ticket ab.";
        return 1;
    }
    cr_cpy(sess, rx + 40, 8);
    return 0;
}

static void field(u8 *m, unsigned at, unsigned len, unsigned off)
{
    put16(m + at, len);
    put16(m + at + 2, len);
    put32(m + at + 4, off);
}

/* Anmeldung (NTLMv2) -> 0 ok */
static int login(void)
{
    static u8 m1[40], m3[700], tok[800], ntv[16], h[16], cch[8], tinfo[400];
    static u8 blob[16 + 8 + 400 + 8], pr[16], nt[16 + sizeof(blob)];
    static struct md5 md;
    static u16 w[160];
    const u8 *sb, *t2 = 0;
    unsigned sl, i, n, tl, til, ul, dl2, wl, p, bl, k, ts_found = 0;
    u32 st, fl;

    /* 1: NEGOTIATE */
    cr_zero(m1, sizeof(m1));
    cr_cpy(m1, "NTLMSSP", 8);
    m1[8] = 1;
    put32(m1 + 12, 0xA0088205UL);   /* UNICODE REQUEST_TARGET NTLM ALWAYS_SIGN ESS 128 56 */
    n = spnego(tok, m1, 32, 1);
    cr_zero(BODY, 24);
    BODY[0] = 25;
    BODY[3] = 1;                        /* SecurityMode: signieren moeglich */
    put16(BODY + 12, 64 + 24);
    put16(BODY + 14, n);
    cr_cpy(BODY + 24, tok, n);
    st = smb(C_SESS, 24 + n);
    if (st != S_MORE)
        return 1;
    cr_cpy(sess, rx + 40, 8);
    sb = rx + get16(RB + 4);
    sl = get16(RB + 6);
    for (i = 0; i + 32 <= sl; i++)      /* CHALLENGE im SPNEGO suchen    */
        if (cr_eq(sb + i, "NTLMSSP", 8) && sb[i + 8] == 2) {
            t2 = sb + i;
            tl = sl - i;
            break;
        }
    if (!t2 || tl < 48)
        return 1;
    fl = get32(t2 + 20);
    til = get16(t2 + 40);
    if (get32(t2 + 44) + til > tl || til > sizeof(tinfo))
        return 1;
    cr_cpy(tinfo, t2 + (unsigned)get32(t2 + 44), til);
    if (!dom[0]) {                      /* Domaene des Servers nehmen    */
        unsigned dn = get16(t2 + 12), doff = (unsigned)get32(t2 + 16);
        for (i = 0; i < dn / 2 && i < sizeof(dom) - 1 && doff + 2 * i + 1 < tl; i++)
            dom[i] = from16(get16(t2 + doff + 2 * i));
        dom[i] = 0;
    }

    /* NTOWFv2 = HMAC-MD5(MD4(UTF-16 Kennwort), UTF-16(GROSS(Benutzer) Domaene)) */
    wl = to16(pass, w, 64);
    md5_init(&md, 1);
    for (i = 0; i < wl; i++) {
        u8 b[2];
        b[0] = (u8)w[i];
        b[1] = (u8)(w[i] >> 8);
        md5_add(&md, b, 2);
    }
    md5_end(&md, h);
    ul = to16(user, w, 64);
    for (i = 0; i < ul; i++)
        if (w[i] >= 'a' && w[i] <= 'z')
            w[i] -= 32;
    dl2 = to16(dom, w + ul, 64);
    for (i = 0; i < ul + dl2; i++) {
        tok[2 * i] = (u8)w[i];
        tok[2 * i + 1] = (u8)(w[i] >> 8);
    }
    hmac_md5(h, 16, tok, 2 * (ul + dl2), ntv);

    /* Blob: 0101 0000 0000 0000, Zeit, Client-Challenge, 0, TargetInfo, 0 */
    tls_random(cch, 8);
    cr_zero(blob, sizeof(blob));
    blob[0] = 1;
    blob[1] = 1;
    for (i = 0; i + 4 <= til; i += 4 + get16(tinfo + i + 2)) {
        if (get16(tinfo + i) == 7 && get16(tinfo + i + 2) == 8) {   /* MsvAvTimestamp */
            cr_cpy(blob + 8, tinfo + i + 4, 8);
            ts_found = 1;
        }
        if (get16(tinfo + i) == 0)
            break;
    }
    cr_cpy(blob + 16, cch, 8);
    cr_cpy(blob + 28, tinfo, til);
    bl = 28 + til + 4;
    cr_cpy(pr, t2 + 24, 8);             /* Server-Challenge              */
    cr_cpy(nt, pr, 8);
    cr_cpy(nt + 8, blob, bl);
    hmac_md5(ntv, 16, nt, 8 + bl, pr);  /* NTProofStr                    */
    cr_cpy(nt, pr, 16);
    cr_cpy(nt + 16, blob, bl);          /* NtChallengeResponse           */
    hmac_md5(ntv, 16, pr, 16, skey);    /* SessionBaseKey                */
    (void)ts_found;

    /* 3: AUTHENTICATE */
    cr_zero(m3, 64);
    cr_cpy(m3, "NTLMSSP", 8);
    m3[8] = 3;
    p = 64;
    field(m3, 12, 24, p);               /* LM: 24 Nullen (NTLMv2 mit Zeit) */
    cr_zero(m3 + p, 24);
    p += 24;
    field(m3, 20, 16 + bl, p);
    cr_cpy(m3 + p, nt, 16 + bl);
    p += 16 + bl;
    k = to16(dom, w, 64);
    field(m3, 28, 2 * k, p);
    for (i = 0; i < k; i++) { m3[p++] = (u8)w[i]; m3[p++] = (u8)(w[i] >> 8); }
    k = to16(user, w, 64);
    field(m3, 36, 2 * k, p);
    for (i = 0; i < k; i++) { m3[p++] = (u8)w[i]; m3[p++] = (u8)(w[i] >> 8); }
    k = to16("ATDOS", w, 16);
    field(m3, 44, 2 * k, p);
    for (i = 0; i < k; i++) { m3[p++] = (u8)w[i]; m3[p++] = (u8)(w[i] >> 8); }
    field(m3, 52, 0, p);
    put32(m3 + 60, (fl & 0xA0088205UL) | 1);
    if (p > sizeof(m3))
        return 1;
    n = spnego(tok, m3, p, 0);
    cr_zero(BODY, 24);
    BODY[0] = 25;
    BODY[3] = 1;
    put16(BODY + 12, 64 + 24);
    put16(BODY + 14, n);
    cr_cpy(BODY + 24, tok, n);
    st = smb(C_SESS, 24 + n);
    if (st != S_OK)
        return 1;
    if (get16(RB + 2) & 1)              /* als Gast: nicht signieren     */
        signing = 0;
    return 0;
}

/* Pfad in der Freigabe aus rdir + Name (ohne fuehrenden \) -> UTF-16 in
   ub, -> Bytes */
static unsigned rpath(const char *name)
{
    static char p[FAPI_PATHLEN * 2];
    unsigned n = 0, i = 0;
    if (name && name[0] == '\\')
        name++;
    else
        for (; rdir[i] && n < sizeof(p) - 2; i++)
            p[n++] = rdir[i];
    if (name && *name) {
        if (n)
            p[n++] = '\\';
        while (*name && n < sizeof(p) - 1)
            p[n++] = *name++;
    }
    while (n && p[n - 1] == '\\')
        n--;
    p[n] = 0;
    return 2 * to16(p, ub, 280);
}

static u8 fid[16];

/* CREATE -> 0 ok (fid gesetzt) */
static u32 c_open(const char *name, u32 access, u32 disp, u32 opt, u32 *size, unsigned *attr)
{
    unsigned nl = rpath(name), i;
    u32 st;
    cr_zero(BODY, 56);
    BODY[0] = 57;
    put32(BODY + 4, 2);                 /* Impersonation                 */
    put32(BODY + 24, access);
    put32(BODY + 28, 0x80);             /* FILE_ATTRIBUTE_NORMAL         */
    put32(BODY + 32, 7);                /* lesen, schreiben, loeschen    */
    put32(BODY + 36, disp);
    put32(BODY + 40, opt);
    put16(BODY + 44, 64 + 56);
    put16(BODY + 46, nl);
    for (i = 0; i < nl / 2; i++)
        put16(BODY + 56 + 2 * i, ub[i]);
    if (!nl)
        BODY[56] = 0;
    st = smb(C_CREATE, 56 + (nl ? nl : 1));
    if (st == S_OK) {
        cr_cpy(fid, RB + 64, 16);
        if (size)
            *size = get32(RB + 48);
        if (attr)
            *attr = get16(RB + 56);
    }
    return st;
}

static void c_close(void)
{
    cr_zero(BODY, 24);
    BODY[0] = 24;
    cr_cpy(BODY + 8, fid, 16);
    smb(C_CLOSE, 24);
}

static void status(const char *what, u32 st)
{
    out(what);
    out(": ");
    if (st == 0xC0000034UL || st == 0xC000003AUL)
        out("nicht gefunden");
    else if (st == 0xC0000022UL)
        out("Zugriff verweigert");
    else if (st == 0xC0000035UL)
        out("gibt es schon");
    else if (st == 0xC0000101UL)
        out("Verzeichnis nicht leer");
    else if (st == 0xC00000BAUL)
        out("ist ein Verzeichnis");
    else if (st == 0xC0000103UL)
        out("ist kein Verzeichnis");
    else if (st == 0xC0000043UL)
        out("wird benutzt");
    else if (st >= 0xFFFFFFFEUL)
        out("Verbindung unterbrochen");
    else {
        unsigned i, d;
        out("Fehler ");
        for (i = 0; i < 8; i++) {
            d = (unsigned)(st >> (28 - 4 * i)) & 15;
            ts[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        }
        ts[8] = 'h';
        ts[9] = 0;
        out(ts);
    }
    out("\r\n");
    failed = 1;
}

/* Platzhalter wie DOS/Unix (* ?) ohne Gross/klein */
static int gmatch(const char *n, const char *p)
{
    for (; *p; p++, n++) {
        if (*p == '*') {
            if (!p[1])
                return 1;
            for (; *n; n++)
                if (gmatch(n, p + 1))
                    return 1;
            return 0;
        }
        if (!*n || (*p != '?' && up(*p) != up(*n)))
            return 0;
    }
    return !*n;
}

/* FILETIME -> FAT-Datum/-Zeit, -> 0 ok (vor 1980: 1) */
unsigned long dv16(unsigned hi, unsigned lo, unsigned d);
#pragma aux dv16 = "div bx" __parm [__dx] [__ax] [__bx] __value [__dx __ax];
static int ft2fat(const u8 FAR8 *f, unsigned *fd, unsigned *ft)
{
    static const unsigned div[2] = { 10000, 1000 };
    unsigned w[4], i, r, k;
    unsigned long q, lo, hi;
    lo = get32(f);
    hi = get32(f + 4);
    w[0] = (unsigned)(hi >> 16); w[1] = (unsigned)hi;
    w[2] = (unsigned)(lo >> 16); w[3] = (unsigned)lo;
    for (k = 0; k < 2; k++)
        for (i = 0, r = 0; i < 4; i++) {
            q = dv16(r, w[i], div[k]);
            w[i] = (unsigned)q;
            r = (unsigned)(q >> 16);
        }
    lo = ((unsigned long)w[2] << 16) | w[3];
    hi = ((unsigned long)w[0] << 16) | w[1];
    if (lo < 0xB6109100UL)
        hi--;
    lo -= 0xB6109100UL;
    hi -= 2;
    if ((long)hi < 0 || lo < 315532800UL) {
        *fd = 0x21;
        *ft = 0;
        return 1;
    }
    unix2fat(lo, fd, ft);
    return 0;
}

/* FILETIME -> TT.MM.JJJJ HH:MM nach d */
static void ftime(const u8 *f, char *d)
{
    unsigned k, fd, ft;
    if (ft2fat(f, &fd, &ft)) {
        scopy(d, "                ", 17);
        return;
    }
    d[0] = (char)('0' + (fd & 31) / 10); d[1] = (char)('0' + (fd & 31) % 10); d[2] = '.';
    d[3] = (char)('0' + ((fd >> 5) & 15) / 10); d[4] = (char)('0' + ((fd >> 5) & 15) % 10); d[5] = '.';
    k = (fd >> 9) + 1980;
    d[6] = (char)('0' + k / 1000); d[7] = (char)('0' + k / 100 % 10);
    d[8] = (char)('0' + k / 10 % 10); d[9] = (char)('0' + k % 10); d[10] = ' ';
    d[11] = (char)('0' + (ft >> 11) / 10); d[12] = (char)('0' + (ft >> 11) % 10); d[13] = ':';
    d[14] = (char)('0' + ((ft >> 5) & 63) / 10); d[15] = (char)('0' + ((ft >> 5) & 63) % 10);
    d[16] = 0;
}

static void c_dir(const char *pat)
{
    static char nm[260], nb[12];
    unsigned first = 1, off, i, n, files = 0;
    u32 st, next;
    u8 *e;
    unsigned long tot = 0;
    if (!pat || !*pat)
        pat = "*";
    if ((st = c_open(0, 0x00100081UL, 1, 1, 0, 0)) != S_OK) {
        status("dir", st);
        return;
    }
    for (;;) {
        n = to16("*", ub, 2);
        cr_zero(BODY, 32);
        BODY[0] = 33;
        BODY[2] = 1;                    /* FileDirectoryInformation      */
        BODY[3] = first ? 1 : 0;        /* RESTART_SCANS                 */
        cr_cpy(BODY + 8, fid, 16);
        put16(BODY + 24, 64 + 32);
        put16(BODY + 26, 2 * n);
        put32(BODY + 28, CHUNK);
        put16(BODY + 32, ub[0]);
        st = smb(C_QDIR, 32 + 2);
        first = 0;
        if (st == S_NOMORE)
            break;
        if (st != S_OK) {
            status("dir", st);
            break;
        }
        off = get16(RB + 2);
        e = rx + off;
        for (;;) {
            next = get32(e);
            n = (unsigned)get32(e + 60) / 2;
            for (i = 0; i < n && i < sizeof(nm) - 1; i++)
                nm[i] = from16(get16(e + 64 + 2 * i));
            nm[i] = 0;
            if (gmatch(nm, pat) && !same(nm, ".") && !same(nm, "..")) {
                ftime(e + 24, ts);
                out(ts);
                if (get32(e + 56) & 0x10)
                    out("    <DIR>      ");
                else {
                    unsigned long sz = get32(e + 40), lim = 1000000000UL;
                    unsigned pad;
                    char *q = ldec(sz, nb);
                    pad = (unsigned)(q - nb);
                    out(" ");
                    for (; pad < 14; pad++)
                        out(" ");
                    out(nb);
                    out(" ");
                    tot += sz;
                    (void)lim;
                }
                out(nm);
                out("\r\n");
                files++;
            }
            if (!next || e + next >= rx + rxn)
                break;
            e += (unsigned)next;
        }
    }
    c_close();
    ldec(files, nb);
    out("      ");
    out(nb);
    out(" Eintr\x84ge, ");
    ldec(tot, nb);
    out(nb);
    out(" Byte\r\n");
}

static void c_cd(const char *d)
{
    static char nd[FAPI_PATHLEN];
    unsigned n, attr = 0;
    u32 st;
    if (!d || !*d) {
        out("\\");
        out(rdir);
        out("\r\n");
        return;
    }
    if (same(d, "\\")) {
        rdir[0] = 0;
        return;
    }
    scopy(nd, rdir, sizeof(nd));
    if (same(d, "..")) {
        for (n = slen(nd); n && nd[n - 1] != '\\'; n--)
            ;
        nd[n ? n - 1 : 0] = 0;
    } else if (d[0] == '\\')
        scopy(nd, d + 1, sizeof(nd));
    else {
        n = slen(nd);
        if (n && n < sizeof(nd) - 1)
            nd[n++] = '\\';
        scopy(nd + n, d, sizeof(nd) - n);
    }
    n = slen(nd);
    while (n && nd[n - 1] == '\\')
        nd[--n] = 0;
    scopy(a2, rdir, sizeof(a2));
    scopy(rdir, nd, sizeof(rdir));
    st = c_open(0, 0x00100081UL, 1, 1, 0, &attr);       /* Verzeichnis?  */
    if (st != S_OK) {
        scopy(rdir, a2, sizeof(rdir));
        status("cd", st);
        return;
    }
    c_close();
}

static void x_report(unsigned long n, unsigned t0)
{
    unsigned t = (unsigned)DosQTicks() - t0;
    out("      ");
    ldec(n, ts);
    out(ts);
    out(" Byte");
    if (t) {
        out(" in ");
        ldec(t / 100, ts);
        out(ts);
        out(",");
        ts[0] = (char)('0' + t / 10 % 10);
        ts[1] = 0;
        out(ts);
        out(" s");
    }
    out("\r\n");
}

static void c_get(const char *r, const char *l)
{
    u8 *lb;
    unsigned long off = 0, size = 0;
    unsigned n, t0 = (unsigned)DosQTicks(), k;
    int h;
    u32 st, sz;
    const char *b;
    if (!r || !*r) {
        out("get: Datei fehlt\r\n");
        failed = 1;
        return;
    }
    if (!l || !*l) {
        for (b = r + slen(r); b > r && b[-1] != '\\' && b[-1] != '/'; b--)
            ;
        l = b;
    }
    if ((st = c_open(r, 0x00120089UL, 1, 0x40, &sz, 0)) != S_OK) {
        status(r, st);
        return;
    }
    size = sz;
    h = DosCreate(l);
    if (FAPI_FAILED(h)) {
        c_close();
        out(l);
        out(": l\x84\xE1t sich nicht anlegen\r\n");
        failed = 1;
        return;
    }
    while (off < size) {
        n = size - off > CHUNK ? CHUNK : (unsigned)(size - off);
        cr_zero(BODY, 48);
        BODY[0] = 49;
        BODY[2] = 0x50;
        put32(BODY + 4, n);
        put32(BODY + 8, off);
        cr_cpy(BODY + 16, fid, 16);
        st = smb(C_READ, 49);
        if (st == S_EOF)
            break;
        if (st != S_OK) {
            status(r, st);
            break;
        }
        k = (unsigned)get32(RB + 4);
        lb = rx + RB[2];                /* direkt aus der Antwort         */
        if (!k || RB[2] + k > rxn)
            break;
        if (DosWrite(h, lb, k > 0x7FFF ? 0x7FFF : k) != (k > 0x7FFF ? 0x7FFF : k) ||
            (k > 0x7FFF && DosWrite(h, lb + 0x7FFF, k - 0x7FFF) != k - 0x7FFF)) {
            out(l);
            out(": Schreibfehler\r\n");
            failed = 1;
            break;
        }
        off += k;
    }
    DosClose(h);
    c_close();
    out(r);
    x_report(off, t0);
}

static void c_put(const char *l, const char *r)
{
    unsigned long off = 0;
    unsigned n, t0 = (unsigned)DosQTicks(), k;
    int h;
    u32 st;
    const char *b;
    if (!l || !*l) {
        out("put: Datei fehlt\r\n");
        failed = 1;
        return;
    }
    if (!r || !*r) {
        for (b = l + slen(l); b > l && b[-1] != '\\' && b[-1] != ':'; b--)
            ;
        r = b;
    }
    h = DosOpen(l);
    if (FAPI_FAILED(h)) {
        out(l);
        out(": nicht gefunden\r\n");
        failed = 1;
        return;
    }
    if ((st = c_open(r, 0x0012019FUL, 5, 0x40, 0, 0)) != S_OK) {     /* OVERWRITE_IF */
        DosClose(h);
        status(r, st);
        return;
    }
    for (;;) {
        n = DosRead(h, BODY + 48, 0x4000);     /* direkt in die Anfrage   */
        if (FAPI_FAILED(n) || !n)
            break;
        cr_zero(BODY, 48);
        BODY[0] = 49;
        put16(BODY + 2, 64 + 48);
        put32(BODY + 4, n);
        put32(BODY + 8, off);
        cr_cpy(BODY + 16, fid, 16);
        st = smb(C_WRITE, 48 + n);
        if (st != S_OK) {
            status(r, st);
            break;
        }
        k = (unsigned)get32(RB + 4);
        off += k;
        if (k != n)
            break;
    }
    DosClose(h);
    c_close();
    out(r);
    x_report(off, t0);
}

static void c_del(const char *n, int dir)
{
    u32 st;
    if (!n || !*n) {
        out("Name fehlt\r\n");
        failed = 1;
        return;
    }
    st = c_open(n, 0x00110080UL, 1, dir ? 0x1001UL : 0x1040UL, 0, 0);  /* DELETE_ON_CLOSE */
    if (st != S_OK) {
        status(n, st);
        return;
    }
    c_close();
}

static void c_md(const char *n)
{
    u32 st;
    if (!n || !*n) {
        out("Name fehlt\r\n");
        failed = 1;
        return;
    }
    st = c_open(n, 0x00100081UL, 2, 1, 0, 0);          /* FILE_CREATE, Verzeichnis */
    if (st != S_OK) {
        status(n, st);
        return;
    }
    c_close();
}

/* naechstes Wort aus *p (auch "..."), -> 0 wenn keins */
static char *tok(char **p, char *d, unsigned max)
{
    char *s = *p;
    unsigned n = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && n < max - 1)
            d[n++] = *s++;
        if (*s)
            s++;
    } else
        while (*s && *s != ' ' && *s != '\t' && n < max - 1)
            d[n++] = *s++;
    d[n] = 0;
    *p = s;
    return d;
}

static int run(char *l)
{
    static char c[32];
    char *p = l, *x, *y;
    if (!tok(&p, c, sizeof(c)))
        return 0;
    x = tok(&p, a1, sizeof(a1));
    y = tok(&p, a2, sizeof(a2));
    if (same(c, "dir") || same(c, "ls"))
        c_dir(x);
    else if (same(c, "cd"))
        c_cd(x);
    else if (same(c, "pwd"))
        c_cd(0);
    else if (same(c, "get"))
        c_get(x, y);
    else if (same(c, "put"))
        c_put(x, y);
    else if (same(c, "del") || same(c, "rm"))
        c_del(x, 0);
    else if (same(c, "md") || same(c, "mkdir"))
        c_md(x);
    else if (same(c, "rd") || same(c, "rmdir"))
        c_del(x, 1);
    else if (same(c, "lcd")) {
        if (x && DosChDir(x)) {
            out("lcd: Verzeichnis nicht gefunden\r\n");
            failed = 1;
        }
        DosQCurDir(a2);
        out(a2);
        out("\r\n");
    } else if (same(c, "quit") || same(c, "exit") || same(c, "bye") || same(c, "q"))
        return 1;
    else if (same(c, "help") || same(c, "?"))
        out("dir [Muster]  cd [Verz|..|\\]  pwd  get Datei [lokal]  put lokal [Datei]\r\n"
            "del Datei  md Verz  rd Verz  lcd [Verz]  quit\r\n");
    else {
        out("Unbekannter Befehl (help zeigt alle)\r\n");
        failed = 1;
    }
    return 0;
}

/* ---- Dienst fuer NET USE (--serve X:, kernel/ufs.c) ----------------- */
#include "../include/ifsmod.h"
unsigned long mul16x(unsigned a, unsigned b);
#pragma aux mul16x = "mul dx" __parm [__ax] [__dx] __value [__dx __ax];
static struct fapi_ufsreq rq;
static struct fapi_ufsreg rg;
static struct ifs_dirent ide;
static unsigned lseg;                   /* Verzeichnisliste (64 KB, fern) */
static unsigned lcnt, lend;
static char ldir[FAPI_PATHLEN];         /* Liste gilt fuer dieses Verz.   */
static unsigned char hu[32];            /* Handles: belegt                */
static u8 hf[32][16];                   /* FileId je Handle               */
static u8 hacc[32];                     /* 1 = lesend+schreibend geoeffnet */
static unsigned hseg;                   /* Pfade je Handle, Schreibdaten  */
#define HREC 264                        /* Pfad je Handle in hseg         */
#define WOFF (32 * HREC)                /* geretteter WRITE (16 KB)       */
static const char *sconnect(void);
/* Satz: attr(1) size(4) date(2) time(2) nlen(1) alias(13) name(nlen) */
#define LREC(o) ((u8 __far *)MK_FP(lseg, o))

static int is83(const char *n)
{
    unsigned b = 0, e = 0, dot = 0;
    if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))
        return 1;
    for (; *n; n++) {
        if (*n == '.') {
            if (dot++)
                return 0;
            continue;
        }
        if (*n == ' ' || *n == '+' || *n == ',' || *n == ';' || *n == '=' ||
            *n == '[' || *n == ']' || (u8)*n < 32)
            return 0;
        if (dot)
            e++;
        else
            b++;
    }
    return b && b <= 8 && e <= 3;
}

/* 8.3-Ersatzname wie Windows 95 (BASIS~k.EXT) */
static void alias(const char *n, char *a, unsigned k)
{
    const char *d = 0, *p;
    unsigned i = 0, j;
    for (p = n; *p; p++)
        if (*p == '.')
            d = p;
    for (p = n; *p && p != d && i < 6; p++)
        if (*p != ' ' && *p != '.' && (u8)*p > 32)
            a[i++] = (char)(*p == '+' || *p == ',' || *p == ';' || *p == '=' ||
                            *p == '[' || *p == ']' ? '_' : up(*p));
    a[i++] = '~';
    a[i++] = (char)('0' + (k > 9 ? 9 : k));
    if (d && d[1]) {
        a[i++] = '.';
        for (j = 0, p = d + 1; *p && j < 3; p++, j++)
            a[i++] = (char)(*p == ' ' ? '_' : up(*p));
    }
    a[i] = 0;
}

/* Name (n Byte, fern) gegen b ohne Gross/klein -> 0 gleich */
static int cmpn(const u8 __far *a, unsigned n, const char *b)
{
    for (; n && *b; a++, b++, n--)
        if (up((char)*a) != up(*b))
            return 1;
    return n || *b;
}

static int cmpi(const char __far *a, const char *b)  /* ohne Gross/klein */
{
    for (; *a && *b; a++, b++)
        if (up(*a) != up(*b))
            return 1;
    return *a || *b;
}

/* Verzeichnis (SMB-Pfad ohne fuehrenden \) in die Liste lesen -> 0 */
static u32 lst_load(const char *dir)
{
    static char nm[260], al[13];
    unsigned first = 1, i, n, fd, ft, k, j;
    u32 st, next;
    u8 *e;
    u8 __far *r;
    static char sv[FAPI_PATHLEN];
    if (lcnt != 0xFFFF && same(ldir, dir))
        return 0;
    lcnt = 0xFFFF;
    scopy(sv, rdir, sizeof(sv));
    scopy(rdir, dir, sizeof(rdir));
    st = c_open(0, 0x00100081UL, 1, 1, 0, 0);
    scopy(rdir, sv, sizeof(rdir));
    if (st)
        return st;
    lcnt = lend = 0;
    for (;;) {
        n = to16("*", ub, 2);
        cr_zero(BODY, 32);
        BODY[0] = 33;
        BODY[2] = 1;
        BODY[3] = first ? 1 : 0;
        cr_cpy(BODY + 8, fid, 16);
        put16(BODY + 24, 64 + 32);
        put16(BODY + 26, 2 * n);
        put32(BODY + 28, CHUNK);
        put16(BODY + 32, ub[0]);
        st = smb(C_QDIR, 34);
        first = 0;
        if (st != S_OK)
            break;
        e = rx + get16(RB + 2);
        for (;;) {
            next = get32(e);
            n = (unsigned)get32(e + 60) / 2;
            for (i = 0; i < n && i < sizeof(nm) - 1; i++)
                nm[i] = from16(get16(e + 64 + 2 * i));
            nm[i] = 0;
            if (!(dir[0] == 0 && (same(nm, ".") || same(nm, ".."))) &&
                lend + 23 + i < 0xFF00U) {
                r = LREC(lend);
                r[0] = (u8)get32(e + 56);
                r[1] = e[40]; r[2] = e[41]; r[3] = e[42]; r[4] = e[43];
                if (get32(e + 44))      /* ueber 4 GB                     */
                    r[1] = r[2] = r[3] = r[4] = 0xFF;
                ft2fat(e + 24, &fd, &ft);
                r[5] = (u8)fd; r[6] = (u8)(fd >> 8);
                r[7] = (u8)ft; r[8] = (u8)(ft >> 8);
                r[9] = (u8)i;
                al[0] = 0;
                if (!is83(nm)) {        /* Nummer: gleiche Grundform davor */
                    alias(nm, al, 1);
                    for (k = 1, j = 0; j < lend && k < 9; ) {
                        u8 __far *q = LREC(j);
                        if (q[10] && !cmpi((char __far *)q + 10, al))
                            alias(nm, al, ++k), j = 0;
                        else
                            j += 23 + q[9];
                    }
                }
                for (k = 0; k < 13; k++)
                    r[10 + k] = al[k];
                for (k = 0; k < i; k++)
                    r[23 + k] = nm[k];
                lend += 23 + i;
                lcnt++;
            }
            if (!next || e + next >= rx + rxn)
                break;
            e += (unsigned)next;
        }
    }
    c_close();
    scopy(ldir, dir, sizeof(ldir));
    return 0;
}

/* Satz Nummer n der Liste -> Offset, 0xFFFF */
static unsigned lst_at(unsigned n)
{
    unsigned o = 0;
    while (n-- && o < lend)
        o += 23 + LREC(o)[9];
    return o < lend ? o : 0xFFFF;
}

/* Pfad des Kernels (\GROSS\NAME~1) -> SMB-Pfad (ohne \, echte Namen);
   mode 0: Teile mit ~ ueber die Liste aufloesen, sonst wie sie sind,
   mode 1: jeder Teil als 8.3-Ersatzname, wenn er lang ist (NAMEPATH fuer
   alte Programme), mode 2: jeder Teil in der Schreibung des Servers */
static void kpath(const char *k, char *o, int mode)
{
    static char comp[260], par[FAPI_PATHLEN];
    unsigned n = 0, c, i, x, need;
    u8 __far *r;
    if (*k == '\\')
        k++;
    o[0] = 0;
    while (*k) {
        for (c = 0; *k && *k != '\\' && c < sizeof(comp) - 1; )
            comp[c++] = *k++;
        comp[c] = 0;
        if (*k)
            k++;
        for (need = mode != 0, i = 0; comp[i]; i++)
            if (comp[i] == '~')
                need = 1;
        if (need) {
            scopy(par, o, sizeof(par));
            if (lst_load(par) == 0)
                for (i = 0; i < lcnt; i++) {
                    x = lst_at(i);
                    r = LREC(x);
                    if (!(r[10] && !cmpi((char __far *)r + 10, comp)) &&
                        cmpn(r + 23, r[9], comp))
                        continue;
                    if (mode == 1 && r[10])
                        for (c = 0; r[10 + c] && c < 12; c++)
                            comp[c] = r[10 + c];
                    else
                        for (c = 0; c < r[9] && c < sizeof(comp) - 1; c++)
                            comp[c] = r[23 + c];
                    comp[c] = 0;
                    break;
                }
        }
        if (n && n < FAPI_PATHLEN - 2)
            o[n++] = '\\';
        for (i = 0; comp[i] && n < FAPI_PATHLEN - 1; i++)
            o[n++] = comp[i];
        o[n] = 0;
    }
}

/* letzter Teil von o durch die Originalschreibung orig ersetzen */
static void with_orig(char *o, const char *orig)
{
    unsigned n = slen(o), b = n;
    while (b && o[b - 1] != '\\')
        b--;
    if (orig && orig[0] && slen(orig) == n - b)
        scopy(o + b, orig, FAPI_PATHLEN - b);
}

static unsigned hnew(const char *path, unsigned rw)
{
    unsigned i, k;
    char __far *q;
    for (i = 0; i < 32 && hu[i]; i++)
        ;
    if (i == 32)
        return 0;
    hu[i] = 1;
    hacc[i] = (u8)rw;
    cr_cpy(hf[i], fid, 16);
    q = (char __far *)MK_FP(hseg, i * HREC);
    for (k = 0; k < HREC - 1 && path[k]; k++)
        q[k] = path[k];
    q[k] = 0;
    return i + 1;
}

static int hget(unsigned long h)
{
    if (!h || h > 32 || hu[(unsigned)h - 1] != 1)
        return 0;
    cr_cpy(fid, hf[(unsigned)h - 1], 16);
    return 1;
}

static short ierr(u32 st)
{
    if (st == 0xC0000022UL || st == 0xC00000A2UL)
        return IFS_DENIED;
    if (st == 0xC0000035UL)
        return IFS_EXIST;
    if (st == 0xC000007FUL)
        return IFS_FULL;
    if (st == 0xC0000101UL)
        return IFS_DENIED;
    return IFS_ERR;
}

/* SET_INFO (Datei, Klasse) mit len Byte ab BODY+32 */
static u32 setinfo(unsigned cls, unsigned len)
{
    BODY[0] = 33;
    BODY[2] = 1;
    BODY[3] = (u8)cls;
    put32(BODY + 4, len);
    put16(BODY + 8, 64 + 32);
    BODY[10] = BODY[11] = 0;
    put32(BODY + 12, 0);
    cr_cpy(BODY + 16, fid, 16);
    return smb(0x11, 32 + len);
}

/* FAT -> FILETIME (8 Byte) */
static void fat2ft(unsigned d, unsigned t, u8 *o)
{
    unsigned long u = fat2unix(d, t);
    unsigned w[4], c, i;
    unsigned long p;
    static const unsigned m[2] = { 0x9680, 0x0098 };        /* 10^7      */
    unsigned uw[2];
    uw[0] = (unsigned)u;
    uw[1] = (unsigned)(u >> 16);
    w[0] = w[1] = w[2] = w[3] = 0;
    for (i = 0; i < 2; i++) {           /* u * 10^7                       */
        unsigned j;
        c = 0;
        for (j = 0; j < 2; j++) {
            p = mul16x(uw[i], m[j]) + w[i + j] + c;
            w[i + j] = (unsigned)p;
            c = (unsigned)(p >> 16);
        }
        for (j = i + 2; j < 4 && c; j++) {
            p = (unsigned long)w[j] + c;
            w[j] = (unsigned)p;
            c = (unsigned)(p >> 16);
        }
    }
    {                                   /* + 116444736000000000           */
        static const unsigned a[4] = { 0x8000, 0xD53E, 0xB1DE, 0x019D };
        c = 0;
        for (i = 0; i < 4; i++) {
            p = (unsigned long)w[i] + a[i] + c;
            w[i] = (unsigned)p;
            c = (unsigned)(p >> 16);
        }
    }
    for (i = 0; i < 4; i++)
        put16(o + 2 * i, w[i]);
}

static char sp[FAPI_PATHLEN], sp2[FAPI_PATHLEN];
static unsigned h;                      /* READ: Daten ab rx + h          */

/* einen Auftrag (rq) erledigen */
static void op(void)
{
    u32 st, sz;
    unsigned at, n;
    rq.status = 0;
    h = 0;
    switch (rq.op) {
    case UFSOP_OPEN:
        kpath(rq.path, sp, 0);
        rdir[0] = 0;
        n = 1;
        st = c_open(sp, 0x0012019FUL, 1, 0x40, &sz, &at);
        if (st == 0xC0000022UL) {
            n = 0;
            st = c_open(sp, 0x00120089UL, 1, 0x40, &sz, &at);
        }
        if (st) {
            rq.status = ierr(st);
            break;
        }
        if (!(rq.h = hnew(sp, n))) {
            c_close();
            rq.status = IFS_FULL;
            break;
        }
        rq.size = sz;
        rq.attr = at & 0x37;
        break;
    case UFSOP_CREATE:
        kpath(rq.path, sp, 0);
        with_orig(sp, rq.path2);
        rdir[0] = 0;
        st = c_open(sp, 0x0012019FUL, 5, 0x40, 0, 0);
        if (st) {
            rq.status = ierr(st);
            break;
        }
        lcnt = 0xFFFF;
        if (!(rq.h = hnew(sp, 1))) {
            c_close();
            rq.status = IFS_FULL;
        }
        break;
    case UFSOP_READ:
        if (!hget(rq.h)) {
            rq.status = IFS_ERR;
            break;
        }
        n = rq.len > CHUNK ? CHUNK : rq.len;
        cr_zero(BODY, 48);
        BODY[0] = 49;
        BODY[2] = 0x50;
        put32(BODY + 4, n);
        put32(BODY + 8, rq.pos);
        cr_cpy(BODY + 16, fid, 16);
        st = smb(C_READ, 49);
        if (st == S_EOF)
            break;
        if (st) {
            rq.status = IFS_ERR;
            break;
        }
        rq.len = (unsigned)get32(RB + 4);
        h = RB[2];                  /* Daten ab rx + h                */
        break;
    case UFSOP_WRITE:
        if (!hget(rq.h)) {
            rq.status = IFS_ERR;
            break;
        }
        n = rq.len;                 /* Daten stehen schon ab BODY+48  */
        cr_zero(BODY, 48);
        BODY[0] = 49;
        put16(BODY + 2, 64 + 48);
        put32(BODY + 4, n);
        put32(BODY + 8, rq.pos);
        cr_cpy(BODY + 16, fid, 16);
        st = smb(C_WRITE, 48 + n);
        if (st) {
            rq.status = ierr(st);
            rq.len = 0;
        } else
            rq.len = (unsigned)get32(RB + 4);
        h = 0xFFFF;                 /* keine Daten zurueck            */
        break;
    case UFSOP_CLOSE:
        if (rq.h && rq.h <= 32 && hu[(unsigned)rq.h - 1] == 2)
            hu[(unsigned)rq.h - 1] = 0;     /* nach Wiederverbinden verloren */
        if (!hget(rq.h))
            break;
        if (rq.mask) {
            cr_zero(BODY, 32 + 40);
            if (rq.mask & 2)
                fat2ft(rq.date, rq.time, BODY + 32 + 16);
            if (rq.mask & 1)
                put32(BODY + 32 + 32, rq.attr ? (u32)(rq.attr & 0x27) : 0x80);
            setinfo(4, 40);
        }
        c_close();
        hu[(unsigned)rq.h - 1] = 0;
        lcnt = 0xFFFF;
        break;
    case UFSOP_READDIR:
        kpath(rq.path, sp, 0);
        if (lst_load(sp) || rq.n >= lcnt || (at = lst_at(rq.n)) == 0xFFFF) {
            rq.status = -1;
            break;
        }
        {
            u8 __far *r = LREC(at);
            unsigned k;
            cr_zero(&ide, sizeof(ide));
            for (k = 0; k < r[9] && k < IFS_NAMELEN - 1; k++)
                ide.name[k] = r[23 + k];
            ide.attr = r[0] & 0x37;
            ide.size = r[1] | (u32)r[2] << 8 | (u32)r[3] << 16 | (u32)r[4] << 24;
            ide.date = r[5] | r[6] << 8;
            ide.time = r[7] | r[8] << 8;
            for (k = 0; k < 13; k++)
                ide.sname[k] = r[10 + k];
        }
        rq.len = sizeof(ide);
        break;
    case UFSOP_STAT:
        kpath(rq.path, sp, 0);
        rdir[0] = 0;
        st = c_open(sp, 0x00100080UL, 1, 0, 0, &at);
        if (st) {
            rq.status = -1;
            break;
        }
        c_close();
        rq.attr = at & 0x37;
        break;
    case UFSOP_REMOVE:
    case UFSOP_RMDIR:
        kpath(rq.path, sp, 0);
        rdir[0] = 0;
        st = c_open(sp, 0x00110080UL, 1, rq.op == UFSOP_RMDIR ? 0x1001UL : 0x1040UL, 0, 0);
        if (st)
            rq.status = ierr(st);
        else
            c_close();
        lcnt = 0xFFFF;
        break;
    case UFSOP_MKDIR:
        kpath(rq.path, sp, 0);
        with_orig(sp, rq.path2);
        rdir[0] = 0;
        st = c_open(sp, 0x00100081UL, 2, 1, 0, 0);
        if (st)
            rq.status = ierr(st);
        else
            c_close();
        lcnt = 0xFFFF;
        break;
    case UFSOP_MOVE:
        kpath(rq.path, sp, 0);
        kpath(rq.path2, sp2, 0);
        with_orig(sp2, rq.path2 + slen(rq.path2) + 1);
        rdir[0] = 0;
        st = c_open(sp, 0x00110080UL, 1, 0, 0, 0);
        if (st) {
            rq.status = ierr(st);
            break;
        }
        n = to16(sp2, ub, 280);
        cr_zero(BODY, 32 + 20);
        for (h = 0; h < n; h++)
            put16(BODY + 32 + 20 + 2 * h, ub[h]);
        put32(BODY + 32 + 16, 2 * n);
        st = setinfo(10, 20 + 2 * n);
        c_close();
        if (st)
            rq.status = ierr(st);
        h = 0;
        lcnt = 0xFFFF;
        break;
    case UFSOP_SPACE:
        rdir[0] = 0;
        rq.pos = 0x7FFFFFFFUL;
        if (c_open(0, 0x00100081UL, 1, 1, 0, 0) == 0) {
            cr_zero(BODY, 40);
            BODY[0] = 41;
            BODY[2] = 2;            /* Dateisystem                    */
            BODY[3] = 3;            /* FileFsSizeInformation          */
            put32(BODY + 4, 24);
            cr_cpy(BODY + 24, fid, 16);
            if (smb(0x10, 41) == S_OK) {
                u8 *b = rx + get16(RB + 2);
                unsigned long au = get32(b + 8), sc = get32(b + 16),
                              bs = get32(b + 20);
                unsigned long fb = au, hi = get32(b + 12), lo = au;
                int e = -10;            /* KB = frei * sc * bs / 1024     */
                while (sc > 1 && fb < 0x40000000UL) { fb <<= 1; sc >>= 1; }
                while (bs > 1 && fb < 0x40000000UL) { fb <<= 1; bs >>= 1; }
                if (hi || sc > 1 || bs > 1)
                    fb = 0x7FFFFFFFUL;
                rq.pos = fb;
                for (sc = get32(b + 16); sc > 1; sc >>= 1)
                    e++;
                for (bs = get32(b + 20); bs > 1; bs >>= 1)
                    e++;
                for (; e < 0; e++) {    /* 64 Bit hi:lo schieben          */
                    lo = (lo >> 1) | (hi << 31);
                    hi >>= 1;
                }
                for (; e > 0 && !(hi & 0x80000000UL); e--) {
                    hi = (hi << 1) | (lo >> 31);
                    lo <<= 1;
                }
                rq.size = (hi || e) ? 0xFFFFFFFFUL : lo;
            }
            c_close();
        }
        break;
    case UFSOP_SETINFO:
        kpath(rq.path, sp, 0);
        rdir[0] = 0;
        st = c_open(sp, 0x00100180UL, 1, 0, 0, 0);
        if (st) {
            rq.status = ierr(st);
            break;
        }
        cr_zero(BODY, 32 + 40);
        if (rq.mask & 2)
            fat2ft(rq.date, rq.time, BODY + 32 + 16);
        if (rq.mask & 1)
            put32(BODY + 32 + 32, rq.attr ? (u32)(rq.attr & 0x27) : 0x80);
        st = setinfo(4, 40);
        c_close();
        if (st)
            rq.status = ierr(st);
        lcnt = 0xFFFF;
        break;
    case UFSOP_LABEL:
        for (n = 0; n < 11 && share[n]; n++)
            sp[n] = up(share[n]);
        sp[n] = 0;
        rq.len = 12;
        break;
    case UFSOP_NAMEPATH:
        kpath(rq.path, sp + 1, rq.shortn ? 1 : 2);
        sp[0] = '\\';
        rq.len = FAPI_PATHLEN > IFS_PATHLEN ? IFS_PATHLEN : FAPI_PATHLEN;
        break;
    case UFSOP_TRUNC:
        if (!hget(rq.h)) {
            rq.status = IFS_ERR;
            break;
        }
        cr_zero(BODY, 40);
        put32(BODY + 32, rq.size);
        st = setinfo(20, 8);
        if (st)
            rq.status = ierr(st);
        break;
    default:
        rq.status = IFS_ERR;
    }
}

/* Wiederverbinden: Sitzung und Freigabe neu, offene Dateien wieder
   oeffnen (Pfad und Zugriff je Handle im Segment hseg) -> 0 ok */
static int reconnect(void)
{
    static char pp[FAPI_PATHLEN];
    unsigned i, k;
    char __far *q;
    lcnt = 0xFFFF;
    if (sconnect())
        return 1;
    rdir[0] = 0;
    for (i = 0; i < 32; i++) {
        if (!hu[i])
            continue;
        q = (char __far *)MK_FP(hseg, i * HREC);
        for (k = 0; k < FAPI_PATHLEN - 1 && q[k]; k++)
            pp[k] = q[k];
        pp[k] = 0;
        if (c_open(pp, hacc[i] ? 0x0012019FUL : 0x00120089UL, 1, 0x40, 0, 0)) {
            hu[i] = 2;                  /* nicht wieder da: Fehler je Zugriff */
            continue;
        }
        cr_cpy(hf[i], fid, 16);
    }
    lost = 0;
    return 0;
}

static void serve(void)
{
    static struct fapi_ufsreq rq0;
    unsigned k;
    for (;;) {
        if (fapi_call(FAPI_UFS, UFS_GET, (unsigned)&rq, (unsigned)(BODY + 48)))
            break;
        if (rq.op == UFSOP_QUIT)
            break;
        cr_cpy(&rq0, &rq, sizeof(rq));
        lost = 0;
        op();
        if (lost) {                     /* Verbindung weg: neu und einmal wiederholen */
            u8 __far *w = (u8 __far *)MK_FP(hseg, WOFF);
            if (rq0.op == UFSOP_WRITE)  /* Daten retten (sconnect braucht tx) */
                for (k = 0; k < rq0.len; k++)
                    w[k] = BODY[48 + k];
            if (reconnect() == 0) {
                cr_cpy(&rq, &rq0, sizeof(rq));
                if (rq0.op == UFSOP_WRITE)
                    for (k = 0; k < rq0.len; k++)
                        BODY[48 + k] = w[k];
                op();
            } else {
                cr_cpy(&rq, &rq0, sizeof(rq));
                rq.status = IFS_ERR;
                rq.len = 0;
                if (rq.op == UFSOP_CLOSE && rq.h && rq.h <= 32) {
                    hu[(unsigned)rq.h - 1] = 0; /* Server weg: Handle ist ohnehin zu */
                    rq.status = 0;
                }
            }
        }
        if (rq.op == UFSOP_READDIR && !rq.status)
            fapi_call(FAPI_UFS, UFS_PUT, (unsigned)&rq, (unsigned)&ide);
        else if ((rq.op == UFSOP_LABEL || rq.op == UFSOP_NAMEPATH) && !rq.status)
            fapi_call(FAPI_UFS, UFS_PUT, (unsigned)&rq, (unsigned)sp);
        else if (rq.op == UFSOP_READ && !rq.status && h)
            fapi_call(FAPI_UFS, UFS_PUT, (unsigned)&rq, (unsigned)(rx + h));
        else {
            if (rq.op != UFSOP_WRITE)
                rq.len = 0;
            fapi_call(FAPI_UFS, UFS_PUT, (unsigned)&rq, 0);
        }
    }
}

/* Verbindung aufbauen: Socket, NEGOTIATE, Anmeldung, TREE_CONNECT -> 0
   oder Meldung. Auch zum Wiederverbinden (alter Socket wird geschlossen). */
static const char *sconnect(void)
{
    unsigned k, n;
    u32 st;
    if (sock != 0xFFFF)
        DosClose(sock);
    sock = 0xFFFF;
    msgid = 0;
    signing = 0;
    cr_zero(sess, 8);
    cr_zero(tree, 4);
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    sock = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(sock)) {
        sock = 0xFFFF;
        return "Kein Netz (TCPIP.SYS fehlt) oder kein Socket frei.";
    }
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = ad[k];
    io.h = sock;
    io.timeout = srv ? 10000u : TMO;
    if (DosNet(NET_CONNECT, &io, sizeof(io)))
        return "Verbinden fehlgeschlagen (Port 445).";

    /* NEGOTIATE: 2.0.2 und 2.1 */
    cr_zero(BODY, 36);
    BODY[0] = 36;
    BODY[2] = 2;
    BODY[4] = 1;                        /* signieren moeglich            */
    tls_random(BODY + 12, 16);          /* ClientGuid                    */
    put16(BODY + 36, 0x0202);
    put16(BODY + 38, 0x0210);
    st = smb(C_NEG, 40);
    if (st != S_OK)
        return "Server spricht kein SMB2 (2.0.2/2.1).";
    dialect = get16(RB + 4);
    signing = (get16(RB + 2) & 2) != 0;
    if (!srv)
        out("Verbunden mit ");
    if (!srv)
        out(host);
    net_ntop(ad, line);
    if (!srv && !same(host, line)) {
        out(" (");
        out(line);
        out(")");
    }
    if (!srv)
        out(", SMB ");
    ts[0] = (char)('0' + (dialect >> 8));
    ts[1] = '.';
    ts[2] = (char)('0' + ((dialect >> 4) & 15));
    ts[3] = dialect == 0x0202 ? '.' : 0;
    ts[4] = '2';
    ts[5] = 0;
    if (!srv) {
        out(ts);
        out(signing ? ", signiert" : "");
        out(".\r\n");
    }
    if (kerb ? klogin() : login())
        return kerb ? kmsg : "Anmeldung abgelehnt (Benutzer/Kennwort?).";

    /* TREE_CONNECT \\Rechner\Freigabe */
    scopy(line, "\\\\", sizeof(line));
    scopy(line + 2, host, sizeof(line) - 2);
    n = slen(line);
    line[n++] = '\\';
    scopy(line + n, share, sizeof(line) - n);
    n = to16(line, ub, 200);
    cr_zero(BODY, 8);
    BODY[0] = 9;
    put16(BODY + 4, 64 + 8);
    put16(BODY + 6, 2 * n);
    for (k = 0; k < n; k++)
        put16(BODY + 8 + 2 * k, ub[k]);
    st = smb(C_TREE, 8 + 2 * n);
    if (st != S_OK)
        return st == 0xC00000CCUL ? "Freigabe gibt es nicht." :
               st == 0xC0000022UL ? "Zugriff auf die Freigabe verweigert." :
                                    "Freigabe nicht verbunden.";
    cr_cpy(tree, rx + 36, 4);
    return 0;
}


static void die(const char *m, unsigned rc)
{
    if (pwf[0]) {                       /* NET liest die Meldung hier     */
        unsigned h = DosCreate(pwf);
        if (!FAPI_FAILED(h)) {
            DosWrite(h, "!", 1);
            DosWrite(h, m, slen(m));
            DosClose(h);
        }
    }
    if (!srv) {                         /* als Dienst still (NET meldet)  */
        out("smbcli: ");
        out(m);
        out("\r\n");
    }
    if (sock != 0xFFFF)
        DosClose(sock);
    DosExit(rc);
}

void main(void)
{
    char *a, *p;
    unsigned k, n;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' && a[1] == 'U' && a[2] <= ' ') {
            a = next_word(a);
            word(a, line, sizeof(line));
            for (p = line; *p && *p != '%'; p++)
                ;
            if (*p) {
                *p++ = 0;
                scopy(pass, p, sizeof(pass));
                havepass = 1;
            }
            scopy(user, line, sizeof(user));
        } else if (*a == '-' && a[1] == 'W' && a[2] <= ' ') {
            a = next_word(a);
            word(a, dom, sizeof(dom));
        } else if (*a == '-' && a[1] == 'p' && a[2] <= ' ') {
            a = next_word(a);
            for (port = 0; *a >= '0' && *a <= '9'; a++)
                port = port * 10 + (*a - '0');
        } else if (*a == '-' && a[1] == 'c' && a[2] <= ' ') {
            a = next_word(a);
            p = cmds;
            if (*a == '"') {
                a++;
                while (*a && *a != '"' && p < cmds + sizeof(cmds) - 1)
                    *p++ = *a++;
            } else
                while (*a > ' ' && p < cmds + sizeof(cmds) - 1)
                    *p++ = *a++;
            *p = 0;
        } else if (*a == '-' && a[1] == 'P' && a[2] <= ' ') {
            a = next_word(a);
            word(a, pwf, sizeof(pwf));
        } else if (*a == '-' && a[1] == '-' && a[2] == 's') {   /* --serve X: */
            a = next_word(a);
            srv = (unsigned)(up(*a) - 'A' + 1);
            if (srv < 3 || srv > 26 || a[1] != ':')
                srv = 0xFFFF;
        } else if (*a == '-' && a[1] == 'k' && a[2] <= ' ')
            kerb = 1;
        else if (*a == '-' && a[1] == '4' && a[2] <= ' ')
            net_fam = AF_INET;
        else if (*a == '-' && a[1] == '6' && a[2] <= ' ')
            net_fam = AF_INET6;
        else if (*a == '\\' && a[1] == '\\') {
            word(a + 2, line, sizeof(line));
            for (p = line; *p && *p != '\\' && *p != '/'; p++)
                ;
            if (*p)
                *p++ = 0;
            scopy(host, line, sizeof(host));
            scopy(share, p, sizeof(share));
        } else {
            out("SMBCLI \\\\Rechner\\Freigabe [-U Benutzer[%Kennwort]] [-W Dom\x84ne]\r\n"
                "       [-k] [-c \"Befehl; Befehl\"] [-p Port] [-4|-6]\r\n");
            DosExit(2);
        }
    }
    if (pwf[0]) {                       /* Kennwort aus der Datei, dann weg */
        unsigned h = DosOpen(pwf);
        if (!FAPI_FAILED(h)) {
            n = DosRead(h, pass, sizeof(pass) - 1);
            DosClose(h);
            pass[FAPI_FAILED(n) ? 0 : n] = 0;
            havepass = 1;
        }
        DosDelete(pwf);
    }
    if (srv) {                          /* losgeloest: keine Handles der  */
        DosClose(STDIN);                /* Sitzung offen halten (sonst    */
        DosClose(STDOUT);               /* bliebe eine Umleitung von NET  */
        DosClose(STDERR);               /* belegt)                        */
    }
    if (srv == 0xFFFF)
        die("--serve braucht ein Laufwerk C: bis Z:.", 2);
    if (!host[0] || !share[0])
        die("\\\\Rechner\\Freigabe fehlt (SMBCLI /? zeigt die Schalter).", 2);
    if (!kerb && user[0] && !havepass && get_line("Kennwort: ", pass, sizeof(pass), 0))
        die("Kennwort fehlt.", 2);

    if (net_lookup(host, ad))
        die("Rechner nicht gefunden.", 4);
    if ((p = (char *)sconnect()) != 0)
        die(p, 4);

    if (srv) {                          /* NET USE: Laufwerk anmelden     */
        lseg = DosAllocSeg(0);
        hseg = DosAllocSeg(WOFF + UFS_DATA);
        if (FAPI_FAILED(lseg) || FAPI_FAILED(hseg))
            die("Zu wenig Speicher.", 4);
        lcnt = 0xFFFF;
        rg.drive = srv - 1;
        scopy(rg.name, line, sizeof(rg.name));
        k = fapi_call(FAPI_UFS, UFS_REG, (unsigned)&rg, 0);
        if (k)
            die(k == FAPI_ERR_EXIST ? "Laufwerk ist schon belegt." :
                                      "Kein Platz f\x81r ein weiteres Netzlaufwerk.", 4);
        if (pwf[0]) {                   /* leere Datei = verbunden        */
            unsigned h = DosCreate(pwf);
            if (!FAPI_FAILED(h))
                DosClose(h);
        }
        pwf[0] = 0;
        serve();
    } else if (cmds[0]) {                      /* -c "a; b"                     */
        for (p = cmds; *p; ) {
            char *e = p;
            while (*e && *e != ';')
                e++;
            k = *e;
            *e = 0;
            scopy(line, p, sizeof(line));
            if (run(line))
                break;
            p = k ? e + 1 : e;
        }
    } else
        for (;;) {
            scopy(ts, "smb:\\", sizeof(ts));
            n = slen(ts);
            scopy(ts + n, rdir, sizeof(ts) - n - 2);
            n = slen(ts);
            ts[n] = '>';
            ts[n + 1] = ' ';
            ts[n + 2] = 0;
            if (get_line(ts, line, sizeof(line), 1) || run(line))
                break;
        }

    cr_zero(BODY, 4);                   /* abmelden                      */
    BODY[0] = 4;
    smb(C_TDIS, 4);
    tree[0] = tree[1] = tree[2] = tree[3] = 0;
    smb(C_LOGOFF, 4);
    DosClose(sock);
    DosExit(failed ? 1 : 0);
}

