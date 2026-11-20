/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - SSH-2-Client (Transport, Anmeldung, Kanal), siehe ssh.h
 *
 * Verfahren (Reihenfolge = Vorzug):
 *   Schluesseltausch  curve25519-sha256(@libssh.org), kex-strict
 *   Hostschluessel    ssh-ed25519, ecdsa-sha2-nistp256/384, rsa-sha2-512/256
 *   Verschluesselung  aes128-gcm, aes256-gcm (@openssh.com), aes128/256-ctr
 *   MAC (nur ctr)     hmac-sha2-256-etm@openssh.com, hmac-sha2-256
 * Bekannte Rechner: %SSH_HOSTS% oder X:\DOS\SYSTEM\SSHHOSTS.TXT (X =
 * Laufwerk von COMSPEC), je Zeile "Rechner[:Port] Typ SHA256:..." -
 * beim ersten Mal wird nachgefragt (wie OpenSSH), ein geaenderter
 * Schluessel bricht ab.
 */
#define FAPI_NO_IMPL
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"
#include "crypto.h"
#include "pki.h"
#include "tls.h"
#include "ssh.h"

#define RBN     (SSH_MAXDATA + 256)     /* Empfang: Paket mit Kopf/Tag   */
#define TBN     (SSH_MAXDATA + 256)
#define LWIN    0x8000UL                /* eigenes Kanalfenster          */

/* Nachrichten */
#define M_DISCONNECT    1
#define M_IGNORE        2
#define M_UNIMPL        3
#define M_DEBUG         4
#define M_SERVICE_REQ   5
#define M_SERVICE_ACC   6
#define M_EXT_INFO      7
#define M_KEXINIT       20
#define M_NEWKEYS       21
#define M_ECDH_INIT     30
#define M_ECDH_REPLY    31
#define M_UA_REQ        50
#define M_UA_FAIL       51
#define M_UA_OK         52
#define M_UA_BANNER     53
#define M_UA_INFO_REQ   60
#define M_UA_INFO_RESP  61
#define M_GLOBAL_REQ    80
#define M_REQ_FAIL      82
#define M_CH_OPEN       90
#define M_CH_OPEN_OK    91
#define M_CH_OPEN_FAIL  92
#define M_CH_WINADJ     93
#define M_CH_DATA       94
#define M_CH_EXTDATA    95
#define M_CH_EOF        96
#define M_CH_CLOSE      97
#define M_CH_REQ        98
#define M_CH_OK         99
#define M_CH_FAIL       100

unsigned ssh_fam;
int ssh_exit = -1;
int ssh_verbose;
void (*ssh_data)(int ext, const unsigned char *d, unsigned n);

static unsigned sk = 0xFFFF;            /* Socket                         */
static u8 rb[RBN], tb[TBN];
static unsigned rhave, rneed, rfirst;   /* Empfangszustand               */
static u8 *rp;                          /* Nutzlast des letzten Pakets   */
static unsigned rn;
static struct fapi_sockio io;

/* Richtung: 0 aus, 1 GCM, 2 CTR */
struct cdir {
    u8 kind, etm, ml, klen;
    struct gcm g;                       /* bei CTR nur g.k               */
    u8 iv[16], mk[32];
    u32 seq;
};
static struct cdir cs, cr;              /* senden, empfangen              */
static u8 strict, keyed, skip_next;

static const char V_C[] = "SSH-2.0-ATDOS_6.3";
static char v_s[256];
static u8 sess_id[32];
static char hname[136];                 /* fuer bekannte Rechner          */
static unsigned hport;

/* ausgehandelt */
static u8 hk_alg, c_alg[2], m_alg[2];   /* [0] c->s, [1] s->c             */
#define HK_ED   1
#define HK_E256 2
#define HK_E384 3
#define HK_R512 4
#define HK_R256 5

static const char L_KEX[] = "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com";
static const char L_HK[] = "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,rsa-sha2-512,rsa-sha2-256";
static const char L_CI[] = "aes128-gcm@openssh.com,aes256-gcm@openssh.com,aes128-ctr,aes256-ctr";
static const char L_MAC[] = "hmac-sha2-256-etm@openssh.com,hmac-sha2-256";

/* Kanal */
static unsigned long rwin, rmax, lused;
static unsigned long rchan;
static u8 ch_open, ch_eof, ch_closed, ch_sentclose;

/* ---- Hilfen ------------------------------------------------------------- */
void ssh_out(const char *s)
{
    VioWrtTTY(s, slen(s));
}

u8 *ssh_put32(u8 *p, unsigned long v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
    return p + 4;
}

unsigned long ssh_get32(const u8 *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

u8 *ssh_putstr(u8 *p, const void *s, unsigned n)
{
    p = ssh_put32(p, n);
    cr_cpy(p, s, n);
    return p + n;
}

static u8 *putcs(u8 *p, const char *s)
{
    return ssh_putstr(p, s, slen(s));
}

/* mpint aus n Byte Big Endian (vorne Nullen weg, 00 vor gesetztem Bit 7) */
static u8 *putmp(u8 *p, const u8 *b, unsigned n)
{
    while (n && !*b) {
        b++;
        n--;
    }
    if (n && (*b & 0x80)) {
        p = ssh_put32(p, n + 1);
        *p++ = 0;
    } else
        p = ssh_put32(p, n);
    cr_cpy(p, b, n);
    return p + n;
}

/* Lesezeiger: Zeichenkette ab *p (bis e) -> Laenge, Zeiger in *s; -1 Fehler */
static int getstr(const u8 **p, const u8 *e, const u8 **s)
{
    unsigned long n;
    if (e - *p < 4)
        return -1;
    n = ssh_get32(*p);
    if (n > (unsigned long)(e - *p - 4))
        return -1;
    *s = *p + 4;
    *p += 4 + (unsigned)n;
    return (int)n;
}

static void fail(const char *m)
{
    ssh_out("ssh: ");
    ssh_out(m);
    ssh_out("\r\n");
}

/* Name-Liste: steht name (n Zeichen) in l (ln Zeichen)? */
static int nl_has(const u8 *l, unsigned ln, const char *name, unsigned n)
{
    unsigned i = 0, j;
    while (i < ln) {
        for (j = i; j < ln && l[j] != ','; j++)
            ;
        if (j - i == n && cr_eq(l + i, name, n))
            return 1;
        i = j + 1;
    }
    return 0;
}

/* erstes Verfahren aus unserer Liste (ours), das der Server kennt ->
   Nummer ab 1, 0 keins */
static int pick(const char *ours, const u8 *srv, unsigned sn)
{
    unsigned i = 0, j, k = 1;
    for (;;) {
        for (j = i; ours[j] && ours[j] != ','; j++)
            ;
        if (nl_has(srv, sn, ours + i, j - i))
            return k;
        if (!ours[j])
            return 0;
        i = j + 1;
        k++;
    }
}

/* ---- Socket ------------------------------------------------------------- */
static unsigned sio(unsigned fn, void *b, unsigned len, unsigned ms)
{
    io.h = sk;
    io.buf = (unsigned)b;
    io.len = len;
    io.flags = 0;
    io.timeout = ms;
    return DosNet(fn, &io, sizeof(io));
}

static int tx_raw(const u8 *b, unsigned n)
{
    unsigned r;
    while (n) {
        r = sio(NET_SEND, (void *)b, n, 0);
        if (r == 0 || FAPI_FAILED(r))
            return -1;
        b += r;
        n -= r;
    }
    return 0;
}

/* ---- Pakete ------------------------------------------------------------- */
static void iv_inc(u8 *iv)              /* GCM: Zaehler in Byte 4..11     */
{
    unsigned i = 12;
    while (i-- > 4)
        if (++iv[i])
            break;
}

static void ctr_xor(struct cdir *c, u8 *d, unsigned n)   /* n % 16 == 0 */
{
    u8 ks[16];
    unsigned i, k;
    for (i = 0; i < n; i += 16) {
        aes_enc(&c->g.k, c->iv, ks);
        for (k = 0; k < 16; k++)
            d[i + k] ^= ks[k];
        for (k = 16; k--; )
            if (++c->iv[k])
                break;
    }
}

static void mac(struct cdir *c, const u8 *d, unsigned n, u8 out[32])
{
    static struct hmac h;
    u8 sq[4];
    hmac_init(&h, c->mk, 32);
    ssh_put32(sq, c->seq);
    hmac_add(&h, sq, 4);
    hmac_add(&h, d, n);
    hmac_end(&h, out);
}

/* Nutzlast tb[5..5+n) senden */
static int send_pk(unsigned n)
{
    static u8 padc;
    unsigned bs = cs.kind ? 16 : 8, al, pad, tot;
    al = cs.kind == 1 || cs.etm ? 1 + n : 5 + n;
    pad = bs - al % bs;
    if (pad < 4)
        pad += bs;
    tot = 5 + n + pad;
    if (tot + 32 > TBN)
        return -1;
    ssh_put32(tb, tot - 4);
    tb[4] = (u8)pad;
    for (al = 5 + n; al < tot; al++)
        tb[al] = (u8)(padc += 0x5B);
    if (cs.kind == 1) {
        gcm_seal(&cs.g, cs.iv, tb, 4, tb + 4, tot - 4, tb + tot);
        iv_inc(cs.iv);
        tot += 16;
    } else if (cs.kind == 2) {
        if (cs.etm) {
            ctr_xor(&cs, tb + 4, tot - 4);
            mac(&cs, tb, tot, tb + tot);
        } else {
            mac(&cs, tb, tot, tb + tot);
            ctr_xor(&cs, tb, tot);
        }
        tot += 32;
    }
    cs.seq++;
    return tx_raw(tb, tot);
}

/* Teile des naechsten Pakets holen, hoechstens ms warten ->
   1 Paket da (rp/rn), 0 noch nicht, -1 Fehler */
static int rx_pk(unsigned ms)
{
    unsigned r, len, padl, ml = cr.kind == 1 ? 16 : cr.kind == 2 ? 32 : 0;
    u8 t[32];
    if (!rneed) {
        rneed = cr.kind == 2 && !cr.etm ? 16 : 4;
        rhave = 0;
        rfirst = 0;
    }
    while (rhave < rneed) {
        r = sio(NET_RECV, rb + rhave, rneed - rhave, ms);
        if (r == FAPI_ERR_TIMEOUT || r == FAPI_ERR_AGAIN)
            return 0;
        if (r == 0 || FAPI_FAILED(r)) {
            fail("Verbindung getrennt.");
            return -1;
        }
        rhave += r;
        if (rhave < rneed)
            continue;
        if (!rfirst) {                  /* Laenge bekannt                 */
            rfirst = 1;
            if (cr.kind == 2 && !cr.etm)
                ctr_xor(&cr, rb, 16);
            len = (unsigned)ssh_get32(rb);
            if (ssh_get32(rb) > RBN - 40 || len < 5) {
                fail("Paket zu gross oder kaputt.");
                return -1;
            }
            rneed = 4 + len + ml;
        }
    }
    len = rneed - 4 - ml;
    rneed = 0;
    if (cr.kind == 1) {
        if (gcm_open(&cr.g, cr.iv, rb, 4, rb + 4, len, rb + 4 + len)) {
            fail("Paket verfaelscht (GCM).");
            return -1;
        }
        iv_inc(cr.iv);
    } else if (cr.kind == 2) {
        if (cr.etm) {
            mac(&cr, rb, 4 + len, t);
            if (!cr_eq(t, rb + 4 + len, 32)) {
                fail("Paket verfaelscht (MAC).");
                return -1;
            }
            ctr_xor(&cr, rb + 4, len);
        } else {
            if (len > 12)
                ctr_xor(&cr, rb + 16, len - 12);
            mac(&cr, rb, 4 + len, t);
            if (!cr_eq(t, rb + 4 + len, 32)) {
                fail("Paket verfaelscht (MAC).");
                return -1;
            }
        }
    }
    cr.seq++;
    padl = rb[4];
    if (padl + 1 >= len) {
        fail("Paket kaputt.");
        return -1;
    }
    rp = rb + 5;
    rn = len - 1 - padl;
    return 1;
}

static int kex(void);

/* naechstes Paket, das nicht schon hier erledigt wird; wartet hoechstens
   ms (0xFFFF: 60 s) -> Nachrichtentyp, 0 nichts, -1 Fehler */
static int get(unsigned ms)
{
    int r;
    const u8 *p, *e, *s;
    unsigned long n, t0 = DosQTicks();
    for (;;) {
        r = rx_pk(ms == 0xFFFF ? 200 : ms);
        if (r < 0)
            return -1;
        if (!r) {
            if (ms != 0xFFFF)
                return 0;
            if (DosQTicks() - t0 > 6000) {
                fail("Keine Antwort vom Server.");
                return -1;
            }
            continue;
        }
        if (skip_next) {
            skip_next = 0;
            continue;
        }
        if (!rn)
            continue;
        p = rp + 1;
        e = rp + rn;
        switch (rp[0]) {
        case M_DISCONNECT:
            p += 4;
            if (getstr(&p, e, &s) >= 0) {
                ssh_out("ssh: Server trennt: ");
                VioWrtTTY((const char *)s, (unsigned)(p - s));
                ssh_out("\r\n");
            }
            return -1;
        case M_IGNORE:
        case M_DEBUG:
        case M_UNIMPL:
        case M_EXT_INFO:
            if (strict && !keyed) {
                fail("Unerwartete Nachricht im Schl\x81" "sseltausch.");
                return -1;
            }
            continue;
        case M_UA_BANNER:
            if (getstr(&p, e, &s) >= 0)
                VioWrtTTY((const char *)s, (unsigned)(p - s));
            continue;
        case M_GLOBAL_REQ:
            if (getstr(&p, e, &s) >= 0 && p < e && *p) {
                tb[5] = M_REQ_FAIL;
                if (send_pk(1))
                    return -1;
            }
            continue;
        case M_KEXINIT:
            if (keyed) {                /* neuer Schluesseltausch         */
                if (kex())
                    return -1;
                continue;
            }
            return M_KEXINIT;
        case M_CH_WINADJ:
            if (rn >= 9)
                rwin += ssh_get32(rp + 5);
            if (ms != 0xFFFF)
                return 0;
            continue;
        case M_CH_DATA:
        case M_CH_EXTDATA:
            if (!ssh_data)
                continue;
            if (rp[0] == M_CH_EXTDATA)
                p += 4;
            p += 4;
            if (getstr(&p, e, &s) >= 0) {
                n = (unsigned long)(p - s);
                ssh_data(rp[0] == M_CH_EXTDATA, s, (unsigned)n);
                lused += n;
                if (lused >= LWIN / 2) {        /* Fenster nachschieben  */
                    tb[5] = M_CH_WINADJ;
                    ssh_put32(ssh_put32(tb + 6, rchan), lused);
                    lused = 0;
                    if (send_pk(9))
                        return -1;
                }
            }
            if (ms != 0xFFFF)           /* Aufrufer sieht nach den Daten  */
                return 0;
            continue;
        default:
            return rp[0];
        }
    }
}

/* auf Nachricht typ warten (60 s) -> 0 ok */
static int expect(int typ)
{
    int r = get(0xFFFF);
    if (r == typ)
        return 0;
    if (r > 0)
        fail("Unerwartete Antwort des Servers.");
    return -1;
}

/* ---- Bekannte Rechner --------------------------------------------------- */
static void b64(char *o, const u8 *d, unsigned n)   /* ohne '=' wie OpenSSH */
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned i, v;
    for (i = 0; i + 2 < n; i += 3) {
        v = d[i] << 8 | d[i + 1];
        *o++ = t[d[i] >> 2];
        *o++ = t[(v >> 4) & 63];
        *o++ = t[((d[i + 1] & 15) << 2) | d[i + 2] >> 6];
        *o++ = t[d[i + 2] & 63];
    }
    if (n - i == 1) {
        *o++ = t[d[i] >> 2];
        *o++ = t[(d[i] & 3) << 4];
    } else if (n - i == 2) {
        *o++ = t[d[i] >> 2];
        *o++ = t[((d[i] & 3) << 4) | d[i + 1] >> 4];
        *o++ = t[(d[i + 1] & 15) << 2];
    }
    *o = 0;
}

static void hosts_path(char *p)
{
    if (!FAPI_FAILED(DosGetEnv("SSH_HOSTS", p, 128)) && p[0])
        return;
    if (FAPI_FAILED(DosGetEnv("COMSPEC", p, 128)) || p[1] != ':')
        scopy(p, "C:", 128);
    p[2] = 0;
    scat(p, "\\DOS\\SYSTEM\\SSHHOSTS.TXT", 128);
}

static const char *hk_name(void)
{
    static const char *const n[] = { "", "ssh-ed25519", "ecdsa-sha2-nistp256",
        "ecdsa-sha2-nistp384", "rsa-sha2-512", "rsa-sha2-256" };
    return n[hk_alg];
}

static const char *hk_type(void)        /* Art des Schluessels            */
{
    return hk_alg >= HK_R512 ? "ssh-rsa" : hk_name();
}

/* Fingerabdruck fp gegen die Liste -> 0 ok (bekannt oder angenommen) */
static int check_host(const char *fp)
{
    static char path[130], fbuf[2048], key[160], ln[200];
    unsigned h, n = 0, i, j, k, other = 0;
    char c;
    hosts_path(path);
    scopy(key, hname, sizeof(key));
    if (hport != 22) {
        k = slen(key);
        key[k++] = ':';
        n = hport;
        i = 10000;
        while (i > n && i > 1)
            i /= 10;
        while (i) {
            key[k++] = (char)('0' + n / i % 10);
            i /= 10;
        }
        key[k] = 0;
        n = 0;
    }
    h = DosOpen(path);
    if (!FAPI_FAILED(h)) {
        n = DosRead(h, fbuf, sizeof(fbuf) - 1);
        DosClose(h);
        if (FAPI_FAILED(n))
            n = 0;
    }
    fbuf[n] = 0;
    for (i = 0; i < n; i = j + 1) {     /* Zeilen: Rechner Typ Abdruck    */
        for (j = i; j < n && fbuf[j] != '\n'; j++)
            ;
        k = j - i < sizeof(ln) - 1 ? j - i : sizeof(ln) - 1;
        cr_cpy(ln, fbuf + i, k);
        ln[k] = 0;
        while (k && (ln[k - 1] == '\r' || ln[k - 1] == ' '))
            ln[--k] = 0;
        k = 0;
        while (ln[k] && ln[k] != ' ')
            k++;
        c = ln[k];
        ln[k] = 0;
        if (!c || !same(ln, key))
            continue;
        {
            char *ty = skip(ln + k + 1), *fq;
            unsigned tl = 0;
            while (ty[tl] && ty[tl] != ' ')
                tl++;
            fq = skip(ty + tl);
            if (tl != slen(hk_type()) || !cr_eq(ty, hk_type(), tl)) {
                other = 1;
                continue;
            }
            if (same(fq, fp))
                return 0;
            ssh_out("\r\n@@@ WARNUNG: DER HOSTSCHL\x9A" "SSEL VON ");
            ssh_out(key);
            ssh_out(" HAT SICH GE\x8E" "NDERT! @@@\r\n"
                    "Jemand k\x94nnte die Verbindung abh\x94ren (Mann in der Mitte),\r\n"
                    "oder der Schl\x81" "ssel wurde neu erzeugt. Neu: ");
            ssh_out(fp);
            ssh_out("\r\nAlter Eintrag in ");
            ssh_out(path);
            ssh_out(" - dort l\x94schen, wenn die \x8E" "nderung gewollt ist.\r\n");
            return -1;
        }
    }
    ssh_out(other ? "Der Rechner ist mit einem anderen Schl\x81" "sseltyp bekannt.\r\n"
                  : "Der Rechner ist noch nicht bekannt.\r\n");
    ssh_out(hk_type());
    ssh_out("-Fingerabdruck: ");
    ssh_out(fp);
    ssh_out("\r\nVerbindung fortsetzen und Rechner merken (J/N)? ");
    if (!ask_yn())
        return -1;
    /* anhaengen: alte Datei + neue Zeile neu schreiben */
    k = slen(key);
    if (n + k + slen(hk_type()) + slen(fp) + 6 < sizeof(fbuf)) {
        if (n && fbuf[n - 1] != '\n') {
            fbuf[n++] = '\r';
            fbuf[n++] = '\n';
        }
        fbuf[n] = 0;
        scat(fbuf, key, sizeof(fbuf));
        scat(fbuf, " ", sizeof(fbuf));
        scat(fbuf, hk_type(), sizeof(fbuf));
        scat(fbuf, " ", sizeof(fbuf));
        scat(fbuf, fp, sizeof(fbuf));
        scat(fbuf, "\r\n", sizeof(fbuf));
        h = DosCreate(path);
        if (!FAPI_FAILED(h)) {
            DosWrite(h, fbuf, slen(fbuf));
            DosClose(h);
        } else
            ssh_out("(Liste der bekannten Rechner nicht schreibbar)\r\n");
    }
    return 0;
}

/* ---- Hostschluessel pruefen --------------------------------------------- */
/* r/s als mpint -> DER SEQ { INTEGER r, INTEGER s } in d */
static unsigned der_rs(u8 *d, const u8 *sig, unsigned sn)
{
    const u8 *p = sig, *e = sig + sn, *v[2];
    int l[2], k;
    unsigned o = 2, m;
    for (k = 0; k < 2; k++) {
        l[k] = getstr(&p, e, &v[k]);
        if (l[k] < 0 || l[k] > 66)
            return 0;
        while (l[k] > 1 && !v[k][0] && !(v[k][1] & 0x80)) {
            v[k]++;
            l[k]--;
        }
    }
    for (k = 0; k < 2; k++) {
        m = (unsigned)l[k];
        d[o++] = 2;
        if (m && (v[k][0] & 0x80)) {
            d[o++] = (u8)(m + 1);
            d[o++] = 0;
        } else
            d[o++] = (u8)m;
        cr_cpy(d + o, v[k], m);
        o += m;
    }
    d[0] = 0x30;
    d[1] = (u8)(o - 2);
    return o;
}

/* Signatur sig (Blob) ueber H mit dem Hostschluessel ks pruefen -> 0 ok */
static int hk_verify(const u8 *ks, unsigned kn, const u8 *sig, unsigned sn, const u8 *H)
{
    static struct pkey pk;
    static u8 der[160], hh[64];
    const u8 *p = ks, *e = ks + kn, *t, *a, *b, *sd, *st;
    int tl, al, bl, sl, stl;
    const u8 *q = sig, *qe = sig + sn;
    tl = getstr(&p, e, &t);
    stl = getstr(&q, qe, &st);
    sl = getstr(&q, qe, &sd);
    if (tl < 0 || stl < 0 || sl < 0 || stl != slen(hk_name()) || !cr_eq(st, hk_name(), stl))
        return -1;
    if (tl != slen(hk_type()) || !cr_eq(t, hk_type(), tl))
        return -1;
    if (hk_alg == HK_ED) {
        al = getstr(&p, e, &a);
        if (al != 32 || sl != 64)
            return -1;
        return ed25519_verify(a, H, 32, sd);
    }
    if (hk_alg == HK_E256 || hk_alg == HK_E384) {
        getstr(&p, e, &a);              /* Kurvenname                     */
        al = getstr(&p, e, &a);         /* Punkt 04|x|y                   */
        if (al < 0)
            return -1;
        pk.type = hk_alg == HK_E256 ? PK_EC256 : PK_EC384;
        pk.a = (const u8 FAR *)a;
        pk.alen = al;
        bl = der_rs(der, sd, sl);
        if (!bl)
            return -1;
        if (hk_alg == HK_E256) {
            static struct sha256 c;
            sha256_init(&c);
            sha256_add(&c, H, 32);
            sha256_end(&c, hh);
            return pk_verify(&pk, SC_ECDSA, H_SHA256, hh, (const u8 FAR *)der, bl);
        } else {
            static struct sha512 c;
            sha512_init(&c, 1);
            sha512_add(&c, H, 32);
            sha512_end(&c, hh);
            return pk_verify(&pk, SC_ECDSA, H_SHA384, hh, (const u8 FAR *)der, bl);
        }
    }
    /* RSA: mpint e, mpint n */
    bl = getstr(&p, e, &b);
    al = getstr(&p, e, &a);
    if (al < 0 || bl < 0)
        return -1;
    while (al > 1 && !a[0]) {
        a++;
        al--;
    }
    pk.type = PK_RSA;
    pk.a = (const u8 FAR *)a;
    pk.alen = al;
    pk.e = (const u8 FAR *)b;
    pk.elen = bl;
    if (hk_alg == HK_R256) {
        static struct sha256 c;
        sha256_init(&c);
        sha256_add(&c, H, 32);
        sha256_end(&c, hh);
        return pk_verify(&pk, SC_PKCS1, H_SHA256, hh, (const u8 FAR *)sd, sl);
    } else {
        static struct sha512 c;
        sha512_init(&c, 0);
        sha512_add(&c, H, 32);
        sha512_end(&c, hh);
        return pk_verify(&pk, SC_PKCS1, H_SHA512, hh, (const u8 FAR *)sd, sl);
    }
}

/* ---- Schluesseltausch --------------------------------------------------- */
static u8 ic[512];                      /* eigenes KEXINIT (Nutzlast)     */
static unsigned icn;
static u8 kex_sent;

static int send_kexinit(void)
{
    u8 *p = ic;
    *p++ = M_KEXINIT;
    tls_random(p, 16);
    p += 16;
    p = putcs(p, L_KEX);
    p = putcs(p, L_HK);
    p = putcs(p, L_CI);
    p = putcs(p, L_CI);
    p = putcs(p, L_MAC);
    p = putcs(p, L_MAC);
    p = putcs(p, "none");
    p = putcs(p, "none");
    p = ssh_put32(p, 0);
    p = ssh_put32(p, 0);
    *p++ = 0;                           /* first_kex_packet_follows       */
    p = ssh_put32(p, 0);
    icn = (unsigned)(p - ic);
    cr_cpy(tb + 5, ic, icn);
    kex_sent = 1;
    return send_pk(icn);
}

static void hstr(struct sha256 *h, const void *d, unsigned n)
{
    u8 l[4];
    ssh_put32(l, n);
    sha256_add(h, l, 4);
    sha256_add(h, d, n);
}

static const char *const ci_name[] = { "", "aes128-gcm", "aes256-gcm", "aes128-ctr", "aes256-ctr" };

/* Schluessel einer Richtung aus K (mpint kb, kn) und H ableiten */
static void derive(struct cdir *c, int ci, int ma, const u8 *kb, unsigned kn,
                   const u8 *H, char lt_iv, char lt_k, char lt_m)
{
    static struct sha256 h;
    static u8 o[32];
    char x;
    unsigned k;
    c->kind = ci <= 2 ? 1 : 2;
    c->klen = ci == 1 || ci == 3 ? 16 : 32;
    c->etm = c->kind == 2 && ma == 1;
    for (k = 0; k < 3; k++) {
        x = k == 0 ? lt_iv : k == 1 ? lt_k : lt_m;
        if (k == 2 && c->kind == 1)
            break;
        sha256_init(&h);
        sha256_add(&h, kb, kn);
        sha256_add(&h, H, 32);
        sha256_add(&h, &x, 1);
        sha256_add(&h, sess_id, 32);
        sha256_end(&h, o);
        if (k == 0)
            cr_cpy(c->iv, o, 16);
        else if (k == 1) {
            if (c->kind == 1)
                gcm_keyl(&c->g, o, c->klen);
            else
                aes_keyl(&c->g.k, o, c->klen);
        } else
            cr_cpy(c->mk, o, 32);
    }
    cr_zero(o, 32);
}

/* Schluesseltausch; das KEXINIT des Servers liegt in rp/rn */
static int kex(void)
{
    static struct sha256 xh;
    static u8 priv[32], qc[32], kk[32], H[32], kmp[40], fpd[32];
    static char fp[64];
    static struct cdir ncs, ncr;        /* neue Schluessel                */
    const u8 *p, *e, *l[10], *ks, *qs, *sg;
    int ln[10], k, kx, kn, ksn, qsn, sgn;
    unsigned i;
    u8 *q;
    /* Server-KEXINIT auswerten */
    p = rp + 17;
    e = rp + rn;
    for (k = 0; k < 10; k++)
        if ((ln[k] = getstr(&p, e, &l[k])) < 0) {
            fail("KEXINIT kaputt.");
            return -1;
        }
    if (!keyed)
        strict = (u8)nl_has(l[0], ln[0], "kex-strict-s-v00@openssh.com", 28);
    kx = pick("curve25519-sha256,curve25519-sha256@libssh.org", l[0], ln[0]);
    hk_alg = (u8)pick(L_HK, l[1], ln[1]);
    c_alg[0] = (u8)pick(L_CI, l[2], ln[2]);
    c_alg[1] = (u8)pick(L_CI, l[3], ln[3]);
    m_alg[0] = (u8)pick(L_MAC, l[4], ln[4]);
    m_alg[1] = (u8)pick(L_MAC, l[5], ln[5]);
    if (!kx || !hk_alg || !c_alg[0] || !c_alg[1] || !nl_has(l[6], ln[6], "none", 4) ||
        !nl_has(l[7], ln[7], "none", 4) ||
        (c_alg[0] > 2 && !m_alg[0]) || (c_alg[1] > 2 && !m_alg[1])) {
        fail("Keine gemeinsamen Verfahren mit dem Server "
             "(n\x94tig: curve25519, ed25519/ecdsa/rsa, aes-gcm/aes-ctr).");
        return -1;
    }
    if (p < e && *p) {                  /* Server raet ein KEX-Paket      */
        for (i = 0; i < (unsigned)ln[0] && l[0][i] != ','; i++)
            ;
        if (pick("curve25519-sha256,curve25519-sha256@libssh.org", l[0], i) != kx)
            skip_next = 1;
    }
    sha256_init(&xh);
    hstr(&xh, V_C, sizeof(V_C) - 1);
    hstr(&xh, v_s, slen(v_s));
    if (!kex_sent && send_kexinit())    /* Rekey vom Server: unseres nach */
        return -1;
    hstr(&xh, ic, icn);
    hstr(&xh, rp, rn);
    kex_sent = 0;
    if (ssh_verbose) {
        ssh_out("Verfahren: curve25519-sha256, ");
        ssh_out(hk_name());
        ssh_out(", ");
        ssh_out(ci_name[c_alg[0]]);
        if (c_alg[0] > 2)
            ssh_out(m_alg[0] == 1 ? " + hmac-sha2-256-etm" : " + hmac-sha2-256");
        ssh_out(strict ? ", kex-strict\r\n" : "\r\n");
    }
    /* ECDH */
    tls_random(priv, 32);
    x25519_base(qc, priv);
    tb[5] = M_ECDH_INIT;
    ssh_putstr(tb + 6, qc, 32);
    if (send_pk(37) || expect(M_ECDH_REPLY))
        return -1;
    p = rp + 1;
    e = rp + rn;
    ksn = getstr(&p, e, &ks);
    qsn = getstr(&p, e, &qs);
    sgn = getstr(&p, e, &sg);
    if (ksn < 0 || qsn != 32 || sgn < 0) {
        fail("Antwort des Schl\x81" "sseltauschs kaputt.");
        return -1;
    }
    x25519(kk, priv, qs);
    cr_zero(priv, 32);
    for (i = 0, k = 0; i < 32; i++)
        k |= kk[i];
    if (!k) {
        fail("Ung\x81ltiger Schl\x81" "ssel des Servers.");
        return -1;
    }
    q = putmp(kmp, kk, 32);             /* K als mpint                    */
    kn = (int)(q - kmp);
    hstr(&xh, ks, ksn);
    hstr(&xh, qc, 32);
    hstr(&xh, qs, 32);
    sha256_add(&xh, kmp, kn);
    sha256_end(&xh, H);
    if (!keyed)
        cr_cpy(sess_id, H, 32);
    if (hk_verify(ks, ksn, sg, sgn, H)) {
        fail("Signatur des Hostschl\x81" "ssels ung\x81ltig!");
        return -1;
    }
    if (!keyed) {
        static struct sha256 fh;
        sha256_init(&fh);
        sha256_add(&fh, ks, ksn);
        sha256_end(&fh, fpd);
        scopy(fp, "SHA256:", sizeof(fp));
        b64(fp + 7, fpd, 32);
        if (check_host(fp))
            return -1;
    }
    derive(&ncs, c_alg[0], m_alg[0], kmp, kn, H, 'A', 'C', 'E');
    derive(&ncr, c_alg[1], m_alg[1], kmp, kn, H, 'B', 'D', 'F');
    cr_zero(kk, 32);
    cr_zero(kmp, sizeof(kmp));
    tb[5] = M_NEWKEYS;
    if (send_pk(1))
        return -1;
    ncs.seq = strict ? 0 : cs.seq;
    cr_cpy(&cs, &ncs, sizeof(cs));
    if (expect(M_NEWKEYS))
        return -1;
    ncr.seq = strict ? 0 : cr.seq;
    cr_cpy(&cr, &ncr, sizeof(cr));
    cr_zero(&ncs, sizeof(ncs));
    cr_zero(&ncr, sizeof(ncr));
    keyed = 1;
    return 0;
}

/* ---- Verbinden ---------------------------------------------------------- */
int ssh_connect(const char *host, unsigned port)
{
    static struct fapi_socket so;
    static u8 ad[16];
    static char ts[64];
    unsigned k, r, n;
    char c;
    scopy(hname, host, sizeof(hname));
    for (k = 0; hname[k]; k++)
        if (hname[k] >= 'A' && hname[k] <= 'Z')
            hname[k] += 32;
    hport = port;
    net_fam = ssh_fam;
    if (net_lookup(host, ad)) {
        fail("Rechner nicht gefunden.");
        return -1;
    }
    net_ntop(ad, ts);
    ssh_out("Verbinde mit ");
    ssh_out(host);
    if (!same(host, ts)) {
        ssh_out(" (");
        ssh_out(ts);
        ssh_out(")");
    }
    ssh_out(" ...\r\n");
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    sk = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(sk)) {
        fail(sk == FAPI_ERR_NONET ? "Kein Netz (TCPIP.SYS fehlt)." : "Kein Socket frei.");
        sk = 0xFFFF;
        return -1;
    }
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = ad[k];
    io.h = sk;
    io.timeout = 20000;
    r = DosNet(NET_CONNECT, &io, sizeof(io));
    if (r) {
        fail(r == FAPI_ERR_TIMEOUT ? "Keine Antwort (Zeit\x81" "berschreitung)." :
             r == FAPI_ERR_RESET ? "Verbindung abgelehnt." : "Verbinden fehlgeschlagen.");
        return -1;
    }
    /* Kennungen tauschen; Zeilen vor "SSH-" ueberspringen */
    cr_cpy(tb, V_C, sizeof(V_C) - 1);
    tb[sizeof(V_C) - 1] = '\r';
    tb[sizeof(V_C)] = '\n';
    if (tx_raw(tb, sizeof(V_C) + 1))
        return -1;
    for (k = 0; k < 50; k++) {
        n = 0;
        for (;;) {
            r = sio(NET_RECV, &c, 1, 20000);
            if (r != 1) {
                fail("Keine SSH-Kennung vom Server.");
                return -1;
            }
            if (c == '\n')
                break;
            if (c != '\r' && n < sizeof(v_s) - 1)
                v_s[n++] = c;
        }
        v_s[n] = 0;
        if (n >= 4 && cr_eq(v_s, "SSH-", 4))
            break;
    }
    if (k == 50 || !(cr_eq(v_s, "SSH-2.0-", 8) || cr_eq(v_s, "SSH-1.99-", 9))) {
        fail("Server spricht kein SSH-2.");
        return -1;
    }
    if (ssh_verbose) {
        ssh_out("Server: ");
        ssh_out(v_s);
        ssh_out("\r\n");
    }
    if (send_kexinit() || get(0xFFFF) != M_KEXINIT) {
        fail("Schl\x81" "sseltausch nicht begonnen.");
        return -1;
    }
    return kex();
}

/* ---- Anmeldung ---------------------------------------------------------- */
static int read_line(char *b, unsigned max, int echo)
{
    unsigned n = 0, c;
    for (;;) {
        c = KbdCharIn() & 0xFF;
        if (c == '\r')
            break;
        if (c == 27 || c == 3) {
            ssh_out("\r\n");
            return -1;
        }
        if (c == '\b') {
            if (n) {
                n--;
                if (echo)
                    ssh_out("\b \b");
            }
            continue;
        }
        if (c >= ' ' && n < max - 1) {
            b[n++] = (char)c;
            if (echo)
                VioWrtTTY(b + n - 1, 1);
        }
    }
    b[n] = 0;
    ssh_out("\r\n");
    return 0;
}

static u8 *ua_head(const char *user, const char *meth)
{
    u8 *p = tb + 5;
    *p++ = M_UA_REQ;
    p = putcs(p, user);
    p = putcs(p, "ssh-connection");
    return putcs(p, meth);
}

int ssh_login(const char *user)
{
    static char pw[128], pr[200];
    static u8 meth[256];
    unsigned mn = 0, try, k, np;
    int r, ln;
    u8 *q;
    const u8 *p, *e, *s;
    tb[5] = M_SERVICE_REQ;
    q = putcs(tb + 6, "ssh-userauth");
    if (send_pk((unsigned)(q - tb - 5)) || expect(M_SERVICE_ACC))
        return -1;
    q = ua_head(user, "none");
    if (send_pk((unsigned)(q - tb - 5)))
        return -1;
    r = get(0xFFFF);
    if (r == M_UA_OK)
        return 0;
    if (r != M_UA_FAIL)
        return -1;
    p = rp + 1;
    ln = getstr(&p, rp + rn, &s);
    if (ln > 0 && ln < (int)sizeof(meth)) {
        cr_cpy(meth, s, ln);
        mn = ln;
    }
    for (try = 0; try < 3; try++) {
        if (nl_has(meth, mn, "password", 8)) {
            ssh_out("Kennwort f\x81r ");
            ssh_out(user);
            ssh_out("@");
            ssh_out(hname);
            ssh_out(": ");
            if (read_line(pw, sizeof(pw), 0))
                return -1;
            q = ua_head(user, "password");
            *q++ = 0;
            q = putcs(q, pw);
            cr_zero(pw, sizeof(pw));
            if (send_pk((unsigned)(q - tb - 5)))
                return -1;
            cr_zero(tb, TBN);
            r = get(0xFFFF);
        } else if (nl_has(meth, mn, "keyboard-interactive", 20)) {
            q = ua_head(user, "keyboard-interactive");
            q = ssh_put32(q, 0);
            q = ssh_put32(q, 0);
            if (send_pk((unsigned)(q - tb - 5)))
                return -1;
            while ((r = get(0xFFFF)) == M_UA_INFO_REQ) {
                p = rp + 1;
                e = rp + rn;
                for (k = 0; k < 3; k++) {       /* Name, Anleitung, Sprache */
                    ln = getstr(&p, e, &s);
                    if (ln > 0 && k < 2) {
                        VioWrtTTY((const char *)s, ln);
                        ssh_out("\r\n");
                    }
                }
                if (e - p < 4)
                    return -1;
                np = (unsigned)ssh_get32(p);
                p += 4;
                q = tb + 5;
                *q++ = M_UA_INFO_RESP;
                q = ssh_put32(q, np);
                for (k = 0; k < np; k++) {
                    ln = getstr(&p, e, &s);
                    if (ln < 0 || p >= e)
                        return -1;
                    if (ln >= (int)sizeof(pr))
                        ln = sizeof(pr) - 1;
                    cr_cpy(pr, s, ln);
                    pr[ln] = 0;
                    ssh_out(pr);
                    if (read_line(pw, sizeof(pw), *p++))
                        return -1;
                    if (q + slen(pw) + 8 > tb + TBN - 64)
                        return -1;
                    q = putcs(q, pw);
                    cr_zero(pw, sizeof(pw));
                }
                if (send_pk((unsigned)(q - tb - 5)))
                    return -1;
                cr_zero(tb, TBN);
            }
        } else {
            fail("Der Server erlaubt weder Kennwort noch keyboard-interactive.");
            return -1;
        }
        if (r == M_UA_OK)
            return 0;
        if (r != M_UA_FAIL)
            return -1;
        ssh_out("Zugriff verweigert.\r\n");
        p = rp + 1;
        ln = getstr(&p, rp + rn, &s);
        if (ln > 0 && ln < (int)sizeof(meth)) {
            cr_cpy(meth, s, ln);
            mn = ln;
        }
    }
    fail("Anmeldung fehlgeschlagen.");
    return -1;
}

/* ---- Kanal -------------------------------------------------------------- */
int ssh_session(void)
{
    u8 *q = tb + 5;
    int r;
    *q++ = M_CH_OPEN;
    q = putcs(q, "session");
    q = ssh_put32(q, 0);                /* eigene Nummer                  */
    q = ssh_put32(q, LWIN);
    q = ssh_put32(q, SSH_MAXDATA);
    if (send_pk((unsigned)(q - tb - 5)))
        return -1;
    r = get(0xFFFF);
    if (r == M_CH_OPEN_OK && rn >= 17) {
        rchan = ssh_get32(rp + 5);
        rwin = ssh_get32(rp + 9);
        rmax = ssh_get32(rp + 13);
        ch_open = 1;
        ch_eof = ch_closed = ch_sentclose = 0;
        lused = 0;
        return 0;
    }
    fail("Sitzung abgelehnt.");
    return -1;
}

int ssh_request(const char *type, const u8 *x, unsigned n, int want)
{
    u8 *q = tb + 5;
    int r;
    *q++ = M_CH_REQ;
    q = ssh_put32(q, rchan);
    q = putcs(q, type);
    *q++ = (u8)(want != 0);
    if (n)
        cr_cpy(q, x, n);
    q += n;
    if (send_pk((unsigned)(q - tb - 5)))
        return -1;
    if (!want)
        return 0;
    for (;;) {
        r = get(0xFFFF);
        if (r == M_CH_OK)
            return 0;
        if (r == M_CH_FAIL || r < 0)
            return -1;
        if (r == M_CH_REQ)              /* z. B. exit-status dazwischen  */
            continue;
        if (r == M_CH_CLOSE || r == M_CH_EOF)
            return -1;
    }
}

/* Kanal-Nachrichten, die get() zurueckgibt, auswerten */
static int chan_msg(int r)
{
    const u8 *p, *e, *s;
    int ln;
    if (r == M_CH_EOF) {
        ch_eof = 1;
        return SSH_EV_EOF;
    }
    if (r == M_CH_CLOSE) {
        ch_closed = 1;
        if (!ch_sentclose) {
            tb[5] = M_CH_CLOSE;
            ssh_put32(tb + 6, rchan);
            ch_sentclose = 1;
            send_pk(5);
        }
        return SSH_EV_CLOSE;
    }
    if (r == M_CH_REQ) {
        p = rp + 5;
        e = rp + rn;
        ln = getstr(&p, e, &s);
        if (ln == 11 && cr_eq(s, "exit-status", 11) && e - p >= 5)
            ssh_exit = (int)ssh_get32(p + 1);
        else if (ln == 11 && cr_eq(s, "exit-signal", 11))
            ssh_exit = 255;
        if (ln >= 0 && p < e && *p) {   /* Antwort gewuenscht             */
            tb[5] = M_CH_FAIL;
            ssh_put32(tb + 6, rchan);
            send_pk(5);
        }
    }
    return SSH_EV_NONE;
}

int ssh_poll(unsigned ms)
{
    int r = get(ms);
    if (r < 0)
        return -1;
    if (!r)
        return SSH_EV_NONE;
    return chan_msg(r);
}

int ssh_write(const u8 *d, unsigned n)
{
    unsigned k;
    u8 *q;
    int r;
    while (n) {
        while (!rwin) {                 /* auf Fenster warten             */
            if (ch_closed)
                return -1;
            r = get(500);
            if (r < 0)
                return -1;
            if (r)
                chan_msg(r);
        }
        k = n;
        if (k > SSH_MAXDATA)
            k = SSH_MAXDATA;
        if (k > rmax)
            k = (unsigned)rmax;
        if (k > rwin)
            k = (unsigned)rwin;
        q = tb + 5;
        *q++ = M_CH_DATA;
        q = ssh_put32(q, rchan);
        q = ssh_putstr(q, d, k);
        if (send_pk((unsigned)(q - tb - 5)))
            return -1;
        rwin -= k;
        d += k;
        n -= k;
    }
    return 0;
}

int ssh_send_eof(void)
{
    tb[5] = M_CH_EOF;
    ssh_put32(tb + 6, rchan);
    return send_pk(5);
}

void ssh_disconnect(void)
{
    u8 *q;
    if (sk == 0xFFFF)
        return;
    if (keyed) {
        if (ch_open && !ch_sentclose) {
            tb[5] = M_CH_CLOSE;
            ssh_put32(tb + 6, rchan);
            ch_sentclose = 1;
            send_pk(5);
        }
        q = tb + 5;
        *q++ = M_DISCONNECT;
        q = ssh_put32(q, 11);           /* by application                 */
        q = putcs(q, "Ende");
        q = ssh_put32(q, 0);
        send_pk((unsigned)(q - tb - 5));
    }
    DosClose(sk);
    sk = 0xFFFF;
    cr_zero(&cs, sizeof(cs));
    cr_zero(&cr, sizeof(cr));
}
