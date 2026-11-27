/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PATCH.EXE - Unterschiede (vereinheitlichte Form von DIFF -u, auch aus
 * git) auf Dateien anwenden, Bedienung wie GNU patch:
 *
 *   PATCH [-p n] [-R] [-b] [--dry-run] [-i Patchdatei] [Datei]
 *   PATCH ... < Patchdatei
 *
 *   -p n   n Pfadteile vorn weglassen (ohne -p: Pfad wie angegeben, sonst
 *          nur der Dateiname), -R umgekehrt anwenden, -b alte Fassung als
 *          Datei.orig behalten, --dry-run nur pruefen. Mit Datei gelten
 *          alle Teile des Patches fuer diese Datei.
 *
 * Jeder Abschnitt (Hunk) wird an der angegebenen Zeile gesucht, sonst mit
 * Versatz davor und dahinter (ohne Unschaerfe). Was nicht passt, landet in
 * Datei.rej. Zeilenenden: wie in der Datei (CR LF oder LF). /dev/null als
 * Name: Datei neu anlegen bzw. loeschen. ERRORLEVEL 0 alles angewandt,
 * 1 Abschnitte abgelehnt, 2 Fehler. Je Datei 256 KB / 16.000 Zeilen.
 */
#include "fapi.h"
#include "util.h"
#include "lines.h"

static int streq(const char *a, const char *b);
static void utoa_s(char *b, unsigned v);

static char args[FAPI_ARGLEN];
static char pfile[FAPI_PATHLEN], force[FAPI_PATHLEN];
static char oldn[FAPI_PATHLEN], newn[FAPI_PATHLEN], target[FAPI_PATHLEN];
static char tmpn[FAPI_PATHLEN], rejn[FAPI_PATHLEN], orign[FAPI_PATHLEN];
static int strip = -1, reverse, backup, dry;
static int status;

/* ---- Eingabe (Patch) zeilenweise -------------------------------------- */
static unsigned in_h;
static char ib[2048];
static unsigned ip, il;
static char ln[TX_LMAX], ln2[TX_LMAX], lnf[TX_LMAX];   /* lnf: Dateizeile */
static int have_ln, in_eof;

static int in_c(void)
{
    if (ip >= il) {
        il = DosRead(in_h, ib, sizeof(ib));
        if (FAPI_FAILED(il) || il == 0) {
            il = 0;
            return -1;
        }
        ip = 0;
    }
    return (unsigned char)ib[ip++];
}

/* naechste Zeile nach ln (ohne CR LF) -> 0 = Ende */
static int next_line(void)
{
    int c, n = 0;
    if (have_ln) {
        have_ln = 0;
        return 1;
    }
    if (in_eof)
        return 0;
    for (;;) {
        c = in_c();
        if (c < 0) {
            in_eof = 1;
            if (!n)
                return 0;
            break;
        }
        if (c == '\n')
            break;
        if (n < TX_LMAX - 1)
            ln[n++] = (char)c;
    }
    if (n && ln[n - 1] == '\r')
        n--;
    ln[n] = 0;
    return 1;
}

/* ---- Ausgabe in eine Datei --------------------------------------------- */
static unsigned oh;
static char ob[2048];
static unsigned op;

static void oflush(void)
{
    if (op && oh)
        DosWrite(oh, ob, op);
    op = 0;
}

static void oc(char c)
{
    ob[op++] = c;
    if (op == sizeof(ob))
        oflush();
}

static void os(const char *s)
{
    while (*s)
        oc(*s++);
}

/* ---- Abschnitt (Hunk) im Speicher -------------------------------------- */
static unsigned hsel;                   /* Zeilen: Kennung, Laenge, Text */
static unsigned hlen, hcount;
static unsigned old_start, old_cnt, new_start, new_cnt;
static unsigned char old_nonl, new_nonl;

static int h_add(char tag, const char *s)
{
    unsigned n = slen(s), i;
    unsigned char __far *p;
    if ((unsigned long)hlen + n + 3 > 65000UL)
        return 0;
    p = (unsigned char __far *)MK_FP(hsel, hlen);
    p[0] = (unsigned char)tag;
    p[1] = (unsigned char)n;
    p[2] = (unsigned char)(n >> 8);
    for (i = 0; i < n; i++)
        p[3 + i] = (unsigned char)s[i];
    hlen += n + 3;
    hcount++;
    return 1;
}

/* Zeile k des Abschnitts: Kennung, Text nach b */
static char h_get(unsigned k, char *b, unsigned *pos)
{
    unsigned char __far *p = (unsigned char __far *)MK_FP(hsel, *pos);
    unsigned n = p[1] | (unsigned)p[2] << 8, i;
    char tag = (char)p[0];
    (void)k;
    for (i = 0; i < n && i < TX_LMAX - 1; i++)
        b[i] = (char)p[3 + i];
    b[i] = 0;
    *pos += n + 3;
    return tag;
}

static unsigned num(const char **s)
{
    unsigned v = 0;
    while (**s >= '0' && **s <= '9')
        v = v * 10 + (unsigned)(*(*s)++ - '0');
    return v;
}

/* "@@ -a[,b] +c[,d] @@" */
static int parse_at(const char *s)
{
    if (s[0] != '@' || s[1] != '@' || s[2] != ' ' || s[3] != '-')
        return 0;
    s += 4;
    old_start = num(&s);
    old_cnt = 1;
    if (*s == ',') {
        s++;
        old_cnt = num(&s);
    }
    if (*s++ != ' ' || *s++ != '+')
        return 0;
    new_start = num(&s);
    new_cnt = 1;
    if (*s == ',') {
        s++;
        new_cnt = num(&s);
    }
    return 1;
}

/* Abschnitt einlesen (nach der @@-Zeile) -> 0 = kaputt */
static int read_hunk(void)
{
    unsigned o = 0, n = 0;
    char last = 0;
    hlen = hcount = 0;
    old_nonl = new_nonl = 0;
    while (o < old_cnt || n < new_cnt) {
        if (!next_line())
            return 0;
        if (ln[0] == '\\')              /* No newline (ohne Zaehlen)     */
            continue;
        if (ln[0] == ' ' || ln[0] == 0) {
            o++, n++;
            if (!h_add(' ', ln[0] ? ln + 1 : ln))
                return 0;
        } else if (ln[0] == '-') {
            o++;
            if (!h_add('-', ln + 1))
                return 0;
        } else if (ln[0] == '+') {
            n++;
            if (!h_add('+', ln + 1))
                return 0;
        } else
            return 0;
        last = ln[0];
    }
    if (next_line()) {                  /* "\ No newline at end of file" */
        if (ln[0] == '\\') {
            if (last == '-')
                old_nonl = 1;
            else if (last == '+')
                new_nonl = 1;
            else
                old_nonl = new_nonl = 1;
            if (next_line()) {
                if (ln[0] == '\\')
                    new_nonl = 1;
                else
                    have_ln = 1;
            }
        } else
            have_ln = 1;
    }
    if (reverse) {                      /* - und + tauschen              */
        unsigned pos = 0, k, t;
        unsigned char __far *p;
        for (k = 0; k < hcount; k++) {
            p = (unsigned char __far *)MK_FP(hsel, pos);
            if (p[0] == '-')
                p[0] = '+';
            else if (p[0] == '+')
                p[0] = '-';
            pos += (p[1] | (unsigned)p[2] << 8) + 3;
        }
        t = old_start; old_start = new_start; new_start = t;
        t = old_cnt; old_cnt = new_cnt; new_cnt = t;
        t = old_nonl; old_nonl = new_nonl; new_nonl = (unsigned char)t;
    }
    return 1;
}

/* ---- Anwenden ---------------------------------------------------------- */
static struct text tf;
static int have_file, create, remove_file;
static unsigned cur, nhunk, nfail, rh;
static char eol_crlf;

static void eol(void)
{
    if (eol_crlf)
        oc('\r');
    oc('\n');
}

/* passt der alte Teil des Abschnitts bei Zeile at? */
static int fits(unsigned at)
{
    unsigned k, pos = 0, l = at;
    char tag;
    if (at + old_cnt > tf.nl)
        return 0;
    for (k = 0; k < hcount; k++) {
        tag = h_get(k, ln2, &pos);
        if (tag == '+')
            continue;
        tx_line(&tf, l, lnf, sizeof(lnf));
        if (!streq(lnf, ln2))
            return 0;
        l++;
    }
    return 1;
}

static void copy_to(unsigned upto)
{
    for (; cur < upto; cur++) {
        tx_line(&tf, cur, lnf, sizeof(lnf));
        os(lnf);
        if (cur + 1 < tf.nl || !tf.nonl)
            eol();
    }
}

static void reject(void)
{
    unsigned k, pos = 0;
    char tag, b[8];
    if (dry)
        return;
    if (!rh) {
        rh = DosCreate(rejn);
        if (FAPI_FAILED(rh)) {
            rh = 0;
            return;
        }
    }
    oflush();
    {
        unsigned save = oh;
        oh = rh;
        os("@@ -");
        utoa_s(b, old_start); os(b); os(","); utoa_s(b, old_cnt); os(b);
        os(" +");
        utoa_s(b, new_start); os(b); os(","); utoa_s(b, new_cnt); os(b);
        os(" @@\r\n");
        for (k = 0; k < hcount; k++) {
            tag = h_get(k, ln2, &pos);
            oc(tag);
            os(ln2);
            os("\r\n");
        }
        oflush();
        oh = save;
    }
}

static void apply_hunk(void)
{
    unsigned want, d, at = 0, k, pos = 0, written = 0;
    int found = 0;
    char tag, b[8];
    nhunk++;
    want = old_cnt ? old_start - 1 : old_start;
    if (old_cnt == 0 && tf.nl == 0)
        want = 0;
    for (d = 0; !found && (want + d <= tf.nl || d <= want); d++) {
        if (want + d >= cur && want + d <= tf.nl && fits(want + d)) {
            at = want + d;
            found = 1;
        } else if (d && d <= want && want - d >= cur && fits(want - d)) {
            at = want - d;
            found = 1;
        }
    }
    if (!found) {
        nfail++;
        puts("Abschnitt #");
        utoa_s(b, nhunk); puts(b);
        puts(" FEHLGESCHLAGEN bei ");
        utoa_s(b, old_start); puts(b);
        puts(".\r\n");
        reject();
        return;
    }
    if (at != want) {
        puts("Abschnitt #");
        utoa_s(b, nhunk); puts(b);
        puts(" erfolgreich bei ");
        utoa_s(b, at + 1); puts(b);
        puts(" (Versatz ");
        if (at < want) { puts("-"); utoa_s(b, want - at); }
        else utoa_s(b, at - want);
        puts(b);
        puts(at - want == 1 || want - at == 1 ? " Zeile).\r\n" : " Zeilen).\r\n");
    }
    copy_to(at);
    for (k = 0; k < hcount; k++) {
        tag = h_get(k, ln2, &pos);
        if (tag == '-')
            continue;
        os(ln2);
        written++;
        if (!(new_nonl && written == new_cnt))
            eol();
    }
    cur = at + old_cnt;
}

/* Name aus "--- a/x\tDatum" holen und -p anwenden */
static void take_name(const char *s, char *out, int p)
{
    char t[FAPI_PATHLEN];
    unsigned n = 0;
    s += 4;
    while (*s == ' ')
        s++;
    while (*s && *s != '\t' && n < sizeof(t) - 1)
        t[n++] = *s++;
    while (n && t[n - 1] == ' ')
        n--;
    t[n] = 0;
    s = t;
    if (p > 0) {
        const char *q = t;
        int k = p;
        while (k && *q) {
            if (*q == '/' || *q == '\\')
                k--, s = q + 1;
            q++;
        }
    }
    for (n = 0; s[n] && n < FAPI_PATHLEN - 1; n++)
        out[n] = s[n] == '/' ? '\\' : s[n];
    out[n] = 0;
}

static int is_null(const char *n)
{
    return same(n, "\\dev\\null") || same(n, "NUL");
}

static int exists(const char *n)
{
    unsigned h = DosOpen(n);
    if (FAPI_FAILED(h))
        return 0;
    DosClose(h);
    return 1;
}

static void pick_target(void)
{
    if (force[0]) {
        scopy(target, force, sizeof(target));
        return;
    }
    if (strip < 0) {                    /* ohne -p: wie angegeben, sonst */
        char a[FAPI_PATHLEN], b[FAPI_PATHLEN];      /* nur der Name    */
        scopy(a, newn, sizeof(a));
        scopy(b, oldn, sizeof(b));
        if (!is_null(a) && exists(a)) { scopy(target, a, sizeof(target)); return; }
        if (!is_null(b) && exists(b)) { scopy(target, b, sizeof(target)); return; }
        if (!is_null(a) && exists(base_name(a))) { scopy(target, base_name(a), sizeof(target)); return; }
        if (!is_null(b) && exists(base_name(b))) { scopy(target, base_name(b), sizeof(target)); return; }
        scopy(target, is_null(a) ? b : a, sizeof(target));
        return;
    }
    if (!is_null(newn) && exists(newn))
        scopy(target, newn, sizeof(target));
    else if (!is_null(oldn) && exists(oldn))
        scopy(target, oldn, sizeof(target));
    else
        scopy(target, is_null(newn) ? oldn : newn, sizeof(target));
}

static void begin_file(void)
{
    char *b;
    int r;
    pick_target();
    create = reverse ? is_null(newn) : is_null(oldn);
    remove_file = reverse ? is_null(oldn) : is_null(newn);
    puts(dry ? "Pr\x81" "fe Datei " : "Datei ");
    puts(target);
    puts(dry ? "\r\n" : " wird gepatcht\r\n");
    tf.nl = 0;
    tf.nseg = 0;
    tf.lsel = 0;
    tf.nonl = 0;
    tf.crlf = 1;
    if (!create || exists(target)) {
        r = tx_load(target, &tf);
        if (r) {
            puts(r == 1 ? "patch: Datei nicht gefunden: " : "patch: Datei zu gro\xE1: ");
            puts(target);
            puts("\r\n");
            status = 2;
            have_file = 0;
            return;
        }
    }
    eol_crlf = (char)(tf.nl ? tf.crlf : 1);
    scopy(tmpn, target, sizeof(tmpn));
    b = base_name(tmpn);
    scopy(b, "PATCH$$$.TMP", sizeof(tmpn) - (unsigned)(b - tmpn));
    scopy(rejn, target, sizeof(rejn));
    scat(rejn, ".rej", sizeof(rejn));
    scopy(orign, target, sizeof(orign));
    scat(orign, ".orig", sizeof(orign));
    oh = 0;
    if (!dry) {
        oh = DosCreate(tmpn);
        if (FAPI_FAILED(oh)) {
            puts("patch: kann nicht schreiben: ");
            puts(tmpn);
            puts("\r\n");
            oh = 0;
            status = 2;
            tx_free(&tf);
            have_file = 0;
            return;
        }
    }
    op = 0;
    cur = 0;
    nhunk = nfail = 0;
    rh = 0;
    have_file = 1;
}

static void end_file(void)
{
    char b[8];
    if (!have_file)
        return;
    copy_to(tf.nl);
    oflush();
    if (oh)
        DosClose(oh);
    if (rh)
        DosClose(rh);
    tx_free(&tf);
    have_file = 0;
    if (nfail) {
        utoa_s(b, nfail); puts(b);
        puts(" von ");
        utoa_s(b, nhunk); puts(b);
        puts(" Abschnitten FEHLGESCHLAGEN");
        if (!dry) {
            puts(" -- Ablehnungen in ");
            puts(rejn);
        }
        puts("\r\n");
        if (status < 1)
            status = 1;
    }
    if (dry)
        return;
    if (backup && exists(target)) {
        DosDelete(orign);
        DosMove(target, orign);
    } else
        DosDelete(target);
    if (remove_file && !nfail)
        DosDelete(tmpn);
    else
        DosMove(tmpn, target);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void utoa_s(char *b, unsigned v)
{
    char t[6];
    int n = 0, i = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n)
        b[i++] = t[--n];
    b[i] = 0;
}

static void usage(void)
{
    puts("Wendet Unterschiede (DIFF -u) auf Dateien an.\r\n\r\n"
         "PATCH [-p n] [-R] [-b] [--dry-run] [-i Patchdatei] [Datei]\r\n\r\n"
         "  -p n        n Pfadteile vorn weglassen\r\n"
         "  -R          umgekehrt anwenden (r\x81" "ckg\x84ngig machen)\r\n"
         "  -b          alte Fassung als Datei.orig behalten\r\n"
         "  --dry-run   nur pr\x81" "fen, nichts \x84ndern\r\n"
         "  -i          Patch aus dieser Datei (sonst Standardeingabe)\r\n\r\n"
         "ERRORLEVEL: 0 angewandt, 1 Abschnitte abgelehnt (Datei.rej), 2 Fehler.\r\n");
}

void main(void)
{
    char w[FAPI_PATHLEN], *a;
    const char *q;
    int want = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (want == 'p') { q = w; strip = (int)num(&q); want = 0; continue; }
        if (want == 'i') { scopy(pfile, w, sizeof(pfile)); want = 0; continue; }
        if ((w[0] == '/' && w[1] == '?') || same(w, "--help")) {
            usage();
            DosExit(0);
        }
        if (same(w, "--dry-run")) { dry = 1; continue; }
        if (same(w, "--reverse")) { reverse = 1; continue; }
        if (same(w, "--backup")) { backup = 1; continue; }
        if (w[0] == '-' && w[1] == 'p') {
            if (w[2]) { q = w + 2; strip = (int)num(&q); } else want = 'p';
            continue;
        }
        if (w[0] == '-' && w[1] == 'i') {
            if (w[2]) scopy(pfile, w + 2, sizeof(pfile)); else want = 'i';
            continue;
        }
        if (w[0] == '-' && w[1] == 'R' && !w[2]) { reverse = 1; continue; }
        if (w[0] == '-' && w[1] == 'b' && !w[2]) { backup = 1; continue; }
        if (w[0] == '-' && w[1]) {
            puts("patch: unbekannter Schalter ");
            puts(w);
            puts("\r\n");
            DosExit(2);
        }
        if (!force[0])
            scopy(force, w, sizeof(force));
        else if (!pfile[0])
            scopy(pfile, w, sizeof(pfile));     /* PATCH Datei Patch     */
    }
    hsel = DosAllocSeg(0);
    if (FAPI_FAILED(hsel) || !hsel) {
        puts("patch: nicht gen\x81gend Arbeitsspeicher\r\n");
        DosExit(2);
    }
    in_h = pfile[0] ? DosOpen(pfile) : STDIN;
    if (FAPI_FAILED(in_h)) {
        puts("patch: Patchdatei nicht gefunden: ");
        puts(pfile);
        puts("\r\n");
        DosExit(2);
    }
    oldn[0] = newn[0] = 0;
    while (next_line()) {
        if (ln[0] == '-' && ln[1] == '-' && ln[2] == '-' && ln[3] == ' ') {
            end_file();
            take_name(ln, oldn, strip);
            if (next_line() && ln[0] == '+' && ln[1] == '+' && ln[2] == '+' && ln[3] == ' ')
                take_name(ln, newn, strip);
            else {
                have_ln = 1;
                newn[0] = 0;
            }
            begin_file();
            continue;
        }
        if (ln[0] == '@' && ln[1] == '@') {
            if (!parse_at(ln) || !read_hunk()) {
                puts("patch: kaputter Abschnitt: ");
                puts(ln);
                puts("\r\n");
                status = 2;
                continue;
            }
            if (have_file)
                apply_hunk();
        }
    }
    end_file();
    if (pfile[0])
        DosClose(in_h);
    DosExit((unsigned)status);
}
