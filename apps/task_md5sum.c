/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MD5SUM.EXE / SHA1SUM.EXE - Pruefsummen bilden und pruefen (nach dem
 * Vorbild von GNU md5sum/sha1sum, nur Verhalten und Schalter):
 *
 *   MD5SUM [-b|-t] [--tag] [DATEI...]
 *   MD5SUM -c [--quiet] [--status] [-w] [--ignore-missing] [LISTE...]
 *
 * Ausgabe "Summe  Name" (mit -b "Summe *Name", --tag "MD5 (Name) =
 * Summe"); gelesen wird immer binaer. -c liest solche Zeilen (alle drei
 * Formen, auch mit '/' als Trenner und GNU-Escape '\' vorn, wie sie unter
 * Linux entstehen) und meldet je Datei OK bzw. FEHLER; ERRORLEVEL 1, wenn
 * etwas nicht passt oder fehlt. Ohne Datei oder mit - die Standardeingabe.
 * MD5 nach RFC 1321, SHA-1 nach FIPS 180-1 (eigene Umsetzung).
 * task_sha1sum.c bindet diese Datei mit SHA1 ein.
 */
#include "fapi.h"
#include "util.h"
#define TI_FILE
#include "textin.h"

#if defined(SHA256)
#define PROG "sha256sum"
#define TAG "SHA256"
#define HLEN 32
#include "crypto.h"
#elif defined(SHA1)
#define PROG "sha1sum"
#define TAG "SHA1"
#define HLEN 20
#else
#define PROG "md5sum"
#define TAG "MD5"
#define HLEN 16
#endif

#ifndef SHA256
typedef unsigned long u32;
#endif

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN], dirp[FAPI_PATHLEN];
static char full[FAPI_PATHLEN], pat[FAPI_PATHLEN], line[FAPI_PATHLEN + 80];
static unsigned char dbuf[4096], dig[HLEN];
#ifndef SHA256
static unsigned char blk[64];
static u32 st[5], cnt_lo, cnt_hi;       /* Zustand, Laenge in Bytes      */
static unsigned bn;                     /* Bytes in blk                  */
#endif
static int o_b, o_c, o_q, o_s, o_w, o_im, o_tag, nfiles, rc;
static unsigned n_bad, n_miss, n_fmt, n_ok;

#ifndef SHA256
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#ifdef SHA1
static u32 ld_be(const unsigned char *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
#else
static u32 ld_le(const unsigned char *p)
{
    return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
#endif

#ifdef SHA1
static void h_init(void)
{
    st[0] = 0x67452301UL; st[1] = 0xEFCDAB89UL; st[2] = 0x98BADCFEUL;
    st[3] = 0x10325476UL; st[4] = 0xC3D2E1F0UL;
}

static void h_block(void)
{
    static u32 w[16];
    u32 a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f, k, t;
    unsigned i;

    for (i = 0; i < 16; i++)
        w[i] = ld_be(blk + 4 * i);
    for (i = 0; i < 80; i++) {
        if (i >= 16) {
            t = w[(i + 13) & 15] ^ w[(i + 8) & 15] ^ w[(i + 2) & 15] ^ w[i & 15];
            w[i & 15] = ROL(t, 1);
        }
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999UL;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1UL;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCUL;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6UL;
        }
        t = ROL(a, 5) + f + e + k + w[i & 15];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}
#else
static const u32 K[64] = {
    0xd76aa478UL, 0xe8c7b756UL, 0x242070dbUL, 0xc1bdceeeUL, 0xf57c0fafUL,
    0x4787c62aUL, 0xa8304613UL, 0xfd469501UL, 0x698098d8UL, 0x8b44f7afUL,
    0xffff5bb1UL, 0x895cd7beUL, 0x6b901122UL, 0xfd987193UL, 0xa679438eUL,
    0x49b40821UL, 0xf61e2562UL, 0xc040b340UL, 0x265e5a51UL, 0xe9b6c7aaUL,
    0xd62f105dUL, 0x02441453UL, 0xd8a1e681UL, 0xe7d3fbc8UL, 0x21e1cde6UL,
    0xc33707d6UL, 0xf4d50d87UL, 0x455a14edUL, 0xa9e3e905UL, 0xfcefa3f8UL,
    0x676f02d9UL, 0x8d2a4c8aUL, 0xfffa3942UL, 0x8771f681UL, 0x6d9d6122UL,
    0xfde5380cUL, 0xa4beea44UL, 0x4bdecfa9UL, 0xf6bb4b60UL, 0xbebfbc70UL,
    0x289b7ec6UL, 0xeaa127faUL, 0xd4ef3085UL, 0x04881d05UL, 0xd9d4d039UL,
    0xe6db99e5UL, 0x1fa27cf8UL, 0xc4ac5665UL, 0xf4292244UL, 0x432aff97UL,
    0xab9423a7UL, 0xfc93a039UL, 0x655b59c3UL, 0x8f0ccc92UL, 0xffeff47dUL,
    0x85845dd1UL, 0x6fa87e4fUL, 0xfe2ce6e0UL, 0xa3014314UL, 0x4e0811a1UL,
    0xf7537e82UL, 0xbd3af235UL, 0x2ad7d2bbUL, 0xeb86d391UL
};
static const unsigned char R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void h_init(void)
{
    st[0] = 0x67452301UL; st[1] = 0xEFCDAB89UL;
    st[2] = 0x98BADCFEUL; st[3] = 0x10325476UL;
}

static void h_block(void)
{
    static u32 m[16];
    u32 a = st[0], b = st[1], c = st[2], d = st[3], f, t;
    unsigned i, g, r;

    for (i = 0; i < 16; i++)
        m[i] = ld_le(blk + 4 * i);
    for (i = 0; i < 64; i++) {
        switch (i >> 4) {
        case 0: f = (b & c) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
        case 2: f = b ^ c ^ d;          g = (3 * i + 5) & 15; break;
        default: f = c ^ (b | ~d);      g = (7 * i) & 15; break;
        }
        r = R[((i >> 4) << 2) | (i & 3)];
        t = a + f + K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += (t << r) | (t >> (32 - r));
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}
#endif

static void h_start(void)
{
    h_init();
    bn = 0;
    cnt_lo = cnt_hi = 0;
}

static void h_feed(const unsigned char *p, unsigned n)
{
    u32 old = cnt_lo;
    cnt_lo += n;
    if (cnt_lo < old)
        cnt_hi++;
    while (n) {
        unsigned k = 64 - bn;
        if (k > n)
            k = n;
        n -= k;
        while (k--)
            blk[bn++] = *p++;
        if (bn == 64) {
            h_block();
            bn = 0;
        }
    }
}

static void h_end(void)
{
    u32 lo = cnt_lo << 3, hi = (cnt_hi << 3) | (cnt_lo >> 29);
    unsigned i;

    blk[bn++] = 0x80;
    if (bn > 56) {
        while (bn < 64)
            blk[bn++] = 0;
        h_block();
        bn = 0;
    }
    while (bn < 56)
        blk[bn++] = 0;
    for (i = 0; i < 4; i++) {
#ifdef SHA1
        blk[56 + i] = (unsigned char)(hi >> (24 - 8 * i));
        blk[60 + i] = (unsigned char)(lo >> (24 - 8 * i));
#else
        blk[56 + i] = (unsigned char)(lo >> (8 * i));
        blk[60 + i] = (unsigned char)(hi >> (8 * i));
#endif
    }
    h_block();
    for (i = 0; i < HLEN; i++)
#ifdef SHA1
        dig[i] = (unsigned char)(st[i >> 2] >> (24 - 8 * (i & 3)));
#else
        dig[i] = (unsigned char)(st[i >> 2] >> (8 * (i & 3)));
#endif
}
#else
static struct sha256 hc;                /* crypto.c                      */
static void h_start(void) { sha256_init(&hc); }
static void h_feed(const unsigned char *p, unsigned n) { sha256_add(&hc, p, n); }
static void h_end(void) { sha256_end(&hc, dig); }
#endif

/* Datei/Handle ganz lesen -> dig; 0 = ok */
static int hash_handle(unsigned h)
{
    unsigned n;
    h_start();
    while ((n = DosRead(h, dbuf, sizeof(dbuf))) != 0) {
        if (n == FAPI_ERROR)
            return -1;
        h_feed(dbuf, n);
    }
    h_end();
    return 0;
}

static void put_hex(void)
{
    static const char hx[] = "0123456789abcdef";
    unsigned i;
    for (i = 0; i < HLEN; i++) {
        to_put(hx[dig[i] >> 4]);
        to_put(hx[dig[i] & 15]);
    }
}

static void print_sum(const char *n)
{
    if (o_tag) {
        to_str(TAG " (");
        to_str(n);
        to_str(") = ");
        put_hex();
    } else {
        put_hex();
        to_str(o_b ? " *" : "  ");
        to_str(n);
    }
    to_str("\r\n");
}

static void err(const char *a, const char *m)
{
    to_flush();
    puts(PROG ": ");
    puts(a);
    puts(m);
    rc = 1;
}

/* ---- Pruefsummen bilden --------------------------------------------- */
static void sum_file(const char *a)
{
    static struct fapi_dirent e;
    unsigned h, n, found = 0;
    char *b;

    if (!(a[0] == '-' && !a[1]) && has_wild(a)) {
        abs_path(a, dirp);
        b = base_name(dirp);
        scopy(pat, b, sizeof(pat));
        *b = 0;
        if (slen(dirp) > 3 && dirp[slen(dirp) - 1] == '\\')
            dirp[slen(dirp) - 1] = 0;
        for (n = 0; DosFindEntry(&e, n, dirp) != FAPI_ERROR; n++) {
            if ((e.attr & 0x18) || !match(e.name, pat))
                continue;
            join(full, dirp, e.name, sizeof(full));
            h = DosOpen(full);
            if (FAPI_FAILED(h))
                continue;
            found++;
            if (hash_handle(h) == 0) {
                scopy(line, a, sizeof(line));    /* Name wie angegeben */
                *base_name(line) = 0;
                scat(line, e.name, sizeof(line));
                print_sum(line);
            } else
                err(full, ": Lesefehler\r\n");
            DosClose(h);
        }
        if (!found)
            err(a, ": Datei nicht gefunden\r\n");
        return;
    }
    h = ti_file(a);
    if (FAPI_FAILED(h)) {
        err(a, ": Datei nicht gefunden\r\n");
        return;
    }
    if (hash_handle(h) == 0)
        print_sum(a);
    else
        err(a, ": Lesefehler\r\n");
    if (h != STDIN)
        DosClose(h);
}

/* ---- Pruefsummen pruefen (-c) --------------------------------------- */
static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 2*HLEN Hex-Ziffern bei p -> want; 0 = ok */
static int get_hex(const char *p, unsigned char *want)
{
    unsigned i;
    int x, y;
    for (i = 0; i < HLEN; i++) {
        if ((x = hexv(p[2 * i])) < 0 || (y = hexv(p[2 * i + 1])) < 0)
            return -1;
        want[i] = (unsigned char)((x << 4) | y);
    }
    return 0;
}

/* beginnt mit "TAG (" ? */
static int is_tag(const char *l)
{
    const char *t = TAG " (";
    while (*t)
        if (*l++ != *t++)
            return 0;
    return 1;
}

/* GNU-Escape (Zeile beginnt mit '\'): \\ -> \, \n -> LF */
static void unescape(char *s)
{
    char *d = s;
    while (*s) {
        if (*s == '\\' && s[1] == '\\') {
            *d++ = '\\';
            s += 2;
        } else if (*s == '\\' && s[1] == 'n') {
            *d++ = '\n';
            s += 2;
        } else
            *d++ = *s++;
    }
    *d = 0;
}

static void check_line(char *l, const char *lname, unsigned lnr)
{
    static unsigned char want[HLEN];
    char *fn;
    unsigned i, h, esc = 0;

    if (!l[0])
        return;
    if (l[0] == '\\') {
        esc = 1;
        l++;
    }
    if (is_tag(l)) {
        /* BSD-Form: TAG (Name) = Summe */
        fn = l + slen(TAG " (");
        for (i = slen(fn); i > 0 && !(fn[i - 1] == ')' && fn[i] == ' '); i--)
            ;
        if (!i || fn[i] != ' ' || fn[i + 1] != '=' || fn[i + 2] != ' '
            || slen(fn + i + 3) != 2 * HLEN || get_hex(fn + i + 3, want))
            goto bad;
        fn[i - 1] = 0;
    } else {
        if (slen(l) < 2 * HLEN + 2 || get_hex(l, want) || l[2 * HLEN] != ' '
            || (l[2 * HLEN + 1] != ' ' && l[2 * HLEN + 1] != '*'))
            goto bad;
        fn = l + 2 * HLEN + 2;
    }
    if (esc)
        unescape(fn);
    for (i = 0; fn[i]; i++)             /* Listen von Linux: / -> \      */
        if (fn[i] == '/')
            fn[i] = '\\';
    h = ti_file(fn);
    if (FAPI_FAILED(h)) {
        if (o_im)
            return;
        n_miss++;
        if (!o_s) {
            to_str(fn);
            to_str(": FEHLER beim \x99" "ffnen oder Lesen\r\n");
        }
        return;
    }
    i = hash_handle(h);
    if (h != STDIN)
        DosClose(h);
    if (i) {
        n_miss++;
        if (!o_s) {
            to_str(fn);
            to_str(": FEHLER beim \x99" "ffnen oder Lesen\r\n");
        }
        return;
    }
    for (i = 0; i < HLEN && dig[i] == want[i]; i++)
        ;
    if (i < HLEN) {
        n_bad++;
        if (!o_s) {
            to_str(fn);
            to_str(": FEHLER\r\n");
        }
    } else {
        n_ok++;
        if (!o_s && !o_q) {
            to_str(fn);
            to_str(": OK\r\n");
        }
    }
    return;
bad:
    n_fmt++;
    if (o_w) {
        to_flush();
        puts(PROG ": ");
        puts(lname);
        puts(": Zeile ");
        put_num(lnr, 0, 0);
        puts(": falsches Format\r\n");
    }
}

static void check_list(const char *a)
{
    unsigned h, n = 0, lnr = 0;
    int c;

    h = ti_file(a);
    if (FAPI_FAILED(h)) {
        err(a, ": Datei nicht gefunden\r\n");
        return;
    }
    /* Liste ganz in den Speicher? Nein: Zeilen lesen, je Zeile pruefen -
       hash_handle benutzt dbuf, die Liste laeuft ueber ti_buf. */
    ti_open(h);
    for (;;) {
        c = ti_get();
        if (c == 0x1A)                  /* ^Z: Ende (alte Editoren)      */
            c = -1;
        if (c < 0 || c == '\n') {
            if (n && line[n - 1] == '\r')
                n--;
            line[n] = 0;
            lnr++;
            if (n || c >= 0)
                check_line(line, a, lnr);
            n = 0;
            if (c < 0)
                break;
            continue;
        }
        if (n < sizeof(line) - 1)
            line[n++] = (char)c;
    }
    if (h != STDIN)
        DosClose(h);
}

static int same_cs(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void bad_opt(const char *a)
{
    puts(PROG ": ung\x81ltiger Schalter - ");
    puts(a);
    puts("\r\n");
    DosExit(1);
}

void main(void)
{
    char *a, *o;
    int pass;

    DosGetArgs(args);
    for (pass = 0; pass < 2; pass++)
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            if (!pass)
                puts("Bildet und pr\x81" "ft " TAG "-Pr\x81" "fsummen (nach dem Vorbild von GNU " PROG ").\r\n\r\n"
                     PROG " [-b|-t] [--tag] [Datei...]\r\n"
                     PROG " -c [--quiet] [--status] [-w] [--ignore-missing] [Liste...]\r\n\r\n"
                     "  -b       Ausgabe \"Summe *Name\" (bin\x84r; gelesen wird immer bin\x84r)\r\n"
                     "  --tag    Ausgabe \"" TAG " (Name) = Summe\"\r\n"
                     "  -c       Summen aus der Liste pr\x81" "fen (auch Listen von Linux)\r\n"
                     "  --quiet  kein OK melden          --status  gar nichts, nur ERRORLEVEL\r\n"
                     "  -w       Zeilen mit falschem Format melden\r\n"
                     "  --ignore-missing  fehlende Dateien \x81bergehen\r\n"
                     "Ohne Datei oder mit - die Standardeingabe; Platzhalter gehen.\r\n"
                     "Beispiel: " PROG " *.EXE > LISTE.TXT, sp\x84ter " PROG " -c LISTE.TXT\r\n");
            return;
        }
        if (a[0] == '-' && a[1] == '-' && a[2] && a[2] != ' ') {
            if (pass)
                continue;
            word(a, name, sizeof(name));
            if (same_cs(name, "--binary")) o_b = 1;
            else if (same_cs(name, "--text")) o_b = 0;
            else if (same_cs(name, "--check")) o_c = 1;
            else if (same_cs(name, "--quiet")) o_q = 1;
            else if (same_cs(name, "--status")) o_s = 1;
            else if (same_cs(name, "--warn")) o_w = 1;
            else if (same_cs(name, "--ignore-missing")) o_im = 1;
            else if (same_cs(name, "--tag")) o_tag = 1;
            else if (same_cs(name, "--strict")) o_w = 1;
            else bad_opt(name);
            continue;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ') {
            if (pass)
                continue;
            for (o = a + 1; *o && *o != ' '; o++)
                switch (*o) {
                case 'b': o_b = 1; break;
                case 't': o_b = 0; break;
                case 'c': o_c = 1; break;
                case 'w': o_w = 1; break;
                default: bad_opt(a);
                }
            continue;
        }
        if (!pass) {
            nfiles++;
            continue;
        }
        word(a, name, sizeof(name));
        if (o_c)
            check_list(name);
        else
            sum_file(name);
    }
    if (!nfiles) {
        if (o_c)
            check_list("-");
        else
            sum_file("-");
    }
    to_flush();
    if (o_c && !o_s) {
        if (n_fmt && !n_ok && !n_bad && !n_miss) {
            puts(PROG ": keine g\x81ltigen Zeilen mit " TAG "-Pr\x81" "fsummen gefunden\r\n");
            rc = 1;
        } else {
            if (n_fmt && !o_w) {
                puts(PROG ": WARNUNG: ");
                put_num(n_fmt, 0, 0);
                puts(" Zeile(n) mit falschem Format\r\n");
            }
            if (n_miss) {
                puts(PROG ": WARNUNG: ");
                put_num(n_miss, 0, 0);
                puts(" Datei(en) nicht lesbar\r\n");
            }
            if (n_bad) {
                puts(PROG ": WARNUNG: ");
                put_num(n_bad, 0, 0);
                puts(" Pr\x81" "fsumme(n) passen NICHT\r\n");
            }
        }
    }
    if (o_c && (n_bad || n_miss || (n_fmt && !n_ok && !n_bad && !n_miss)))
        rc = 1;
    DosExit(rc);
}
