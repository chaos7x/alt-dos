/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DIFF.EXE - Textdateien zeilenweise vergleichen, Bedienung wie GNU diff:
 *
 *   DIFF [-u | -U n | -c] [-q] [-s] [-i] [-b] [-w] [-r] [-N] [-a] A B
 *
 *   ohne Schalter die normale Form (2c2, < alt, ---, > neu), -u/-U n die
 *   vereinheitlichte Form fuer PATCH (3 bzw. n Zeilen Umgebung), -q nur
 *   "verschieden", -s auch "identisch", -i ohne Gross/klein, -b
 *   Leerzeichen-Folgen gleich, -w ohne Leerzeichen, -r Verzeichnisse mit
 *   Unterverzeichnissen, -N fehlende Datei = leer, -a Binaerdateien als
 *   Text. Ist A oder B ein Verzeichnis, gilt dort der Name der anderen.
 *   ERRORLEVEL 0 gleich, 1 verschieden, 2 Fehler.
 *
 * Verfahren: Myers (O(ND), "An O(ND) Difference Algorithm and Its
 * Variations", 1986) mit mittlerer Schlange und Teilen wie GNU diff -
 * eigene Umsetzung. Zeilen werden vorher zu Klassennummern (Hash +
 * Vergleich), danach ist jeder Vergleich ein Zahlenvergleich. Grenzen:
 * je Datei 256 KB und 16.000 Zeilen.
 */
#include "fapi.h"
#include "util.h"
#include "lines.h"

static char args[FAPI_ARGLEN];
static char nm1[FAPI_PATHLEN], nm2[FAPI_PATHLEN];
static char lb1[TX_LMAX], lb2[TX_LMAX];
static unsigned ctx = 3;
static char fmt_u, quiet, report_same, icase, ispace, iall, recurse, newfile, astext;
static int status;                      /* 0 gleich, 1 verschieden, 2 Fehler */

/* ---- Ausgabe ----------------------------------------------------------- */
static char ob[2048];
static unsigned op;

static void oflush(void)
{
    if (op)
        DosWrite(STDOUT, ob, op);
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

static void onum(unsigned v)
{
    char t[6];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n)
        oc(t[--n]);
}

static void onl(void)
{
    oc('\r');
    oc('\n');
}

/* ---- Zeilen vergleichbar machen --------------------------------------- */
static unsigned norm(char *b, unsigned n)
{
    unsigned i, o = 0;
    int sp = 0;
    for (i = 0; i < n; i++) {
        char c = b[i];
        if (icase && c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if ((iall || ispace) && (c == ' ' || c == '\t')) {
            sp = 1;
            continue;
        }
        if (sp && ispace && !iall && o)
            b[o++] = ' ';
        sp = 0;
        b[o++] = c;
    }
    b[o] = 0;
    return o;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static unsigned hashs(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = (h << 5) + h + (unsigned char)*s++;
    return h;
}

/* ---- Klassen, Myers --------------------------------------------------- */
static struct text ta, tb;
static unsigned sel_ca, sel_cb, sel_rep, sel_nxt, sel_hs, sel_head, sel_fd, sel_bd, sel_ch;
static unsigned nclass;
#define CA(i)   (((unsigned __far *)MK_FP(sel_ca, 0))[i])
#define CB(i)   (((unsigned __far *)MK_FP(sel_cb, 0))[i])
#define REP(i)  (((unsigned __far *)MK_FP(sel_rep, 0))[i])
#define NXT(i)  (((unsigned __far *)MK_FP(sel_nxt, 0))[i])
#define HS(i)   (((unsigned __far *)MK_FP(sel_hs, 0))[i])
#define HEAD(i) (((unsigned __far *)MK_FP(sel_head, 0))[i])
#define FD(i)   (((int __far *)MK_FP(sel_fd, 0))[i])
#define BD(i)   (((int __far *)MK_FP(sel_bd, 0))[i])
#define CHA(i)  (((unsigned char __far *)MK_FP(sel_ch, 0))[i])
#define CHB(i)  (((unsigned char __far *)MK_FP(sel_ch, 0))[TX_MAXL + (i)])
static int voff;                        /* Index-Versatz der V-Felder   */

static unsigned seg(unsigned bytes)
{
    unsigned s = DosAllocSeg(bytes ? bytes : 2);
    return FAPI_FAILED(s) ? 0 : s;
}

static unsigned classify(struct text *t, unsigned fbit, unsigned i)
{
    unsigned n = tx_line(t, i, lb1, sizeof(lb1)), h, c;
    n = norm(lb1, n);
    h = hashs(lb1);
    for (c = HEAD(h & 4095); c; c = NXT(c - 1)) {
        unsigned r = REP(c - 1);
        if (HS(c - 1) != h)
            continue;
        tx_line(r & 0x8000 ? &tb : &ta, r & 0x7FFF, lb2, sizeof(lb2));
        norm(lb2, slen(lb2));
        if (streq(lb1, lb2))
            return c - 1;
    }
    c = nclass++;
    REP(c) = fbit | i;
    HS(c) = h;
    NXT(c) = HEAD(h & 4095);
    HEAD(h & 4095) = c + 1;
    return c;
}

#define EQ(x, y) (CA(x) == CB(y))

static void diag(int xoff, int xlim, int yoff, int ylim, int *px, int *py)
{
    int dmin = xoff - ylim, dmax = xlim - yoff;
    int fmid = xoff - yoff, bmid = xlim - ylim;
    int fmin = fmid, fmax = fmid, bmin = bmid, bmax = bmid;
    int odd = (fmid - bmid) & 1, d, x, y, tlo, thi;

    FD(fmid + voff) = xoff;
    BD(bmid + voff) = xlim;
    for (;;) {
        if (fmin > dmin) FD(--fmin - 1 + voff) = -1; else ++fmin;
        if (fmax < dmax) FD(++fmax + 1 + voff) = -1; else --fmax;
        for (d = fmax; d >= fmin; d -= 2) {
            tlo = FD(d - 1 + voff);
            thi = FD(d + 1 + voff);
            x = tlo >= thi ? tlo + 1 : thi;
            y = x - d;
            while (x < xlim && y < ylim && EQ(x, y))
                x++, y++;
            FD(d + voff) = x;
            if (odd && bmin <= d && d <= bmax && BD(d + voff) <= x) {
                *px = x;
                *py = y;
                return;
            }
        }
        if (bmin > dmin) BD(--bmin - 1 + voff) = 32767; else ++bmin;
        if (bmax < dmax) BD(++bmax + 1 + voff) = 32767; else --bmax;
        for (d = bmax; d >= bmin; d -= 2) {
            tlo = BD(d - 1 + voff);
            thi = BD(d + 1 + voff);
            x = tlo < thi ? tlo : thi - 1;
            y = x - d;
            while (x > xoff && y > yoff && EQ(x - 1, y - 1))
                x--, y--;
            BD(d + voff) = x;
            if (!odd && fmin <= d && d <= fmax && x <= FD(d + voff)) {
                *px = x;
                *py = y;
                return;
            }
        }
    }
}

static void compareseq(int xoff, int xlim, int yoff, int ylim)
{
    int x, y;
    while (xoff < xlim && yoff < ylim && EQ(xoff, yoff))
        xoff++, yoff++;
    while (xlim > xoff && ylim > yoff && EQ(xlim - 1, ylim - 1))
        xlim--, ylim--;
    if (xoff == xlim) {
        while (yoff < ylim)
            CHB(yoff++) = 1;
    } else if (yoff == ylim) {
        while (xoff < xlim)
            CHA(xoff++) = 1;
    } else {
        diag(xoff, xlim, yoff, ylim, &x, &y);
        compareseq(xoff, x, yoff, y);
        compareseq(x, xlim, y, ylim);
    }
}

/* ---- Ausgabe der Unterschiede ----------------------------------------- */
static void line_out(char tag, struct text *t, unsigned i)
{
    tx_line(t, i, lb1, sizeof(lb1));
    if (tag)
        oc(tag);
    if (tag == '<' || tag == '>')
        oc(' ');
    os(lb1);
    onl();
    if (i + 1 == t->nl && t->nonl) {
        os("\\ No newline at end of file");
        onl();
    }
}

static void range(unsigned a, unsigned n)   /* normale Form: a+1[,a+n]  */
{
    if (n == 0) {
        onum(a);
        return;
    }
    onum(a + 1);
    if (n > 1) {
        oc(',');
        onum(a + n);
    }
}

static void out_normal(void)
{
    unsigned i = 0, j = 0, i1, j1, k;
    unsigned na = ta.nl, nb = tb.nl;
    for (;;) {
        while (i < na && j < nb && !CHA(i) && !CHB(j))
            i++, j++;
        if (i >= na && j >= nb)
            break;
        i1 = i;
        j1 = j;
        while (i1 < na && CHA(i1))
            i1++;
        while (j1 < nb && CHB(j1))
            j1++;
        if (i1 == i && j1 == j) {               /* sollte nicht sein    */
            i++, j++;
            continue;
        }
        range(i, i1 - i);
        oc(i1 == i ? 'a' : j1 == j ? 'd' : 'c');
        range(j, j1 - j);
        onl();
        for (k = i; k < i1; k++)
            line_out('<', &ta, k);
        if (i1 > i && j1 > j) {
            os("---");
            onl();
        }
        for (k = j; k < j1; k++)
            line_out('>', &tb, k);
        i = i1;
        j = j1;
    }
}

static void uhead(unsigned a, unsigned n)       /* @@ -a,n           */
{
    onum(n ? a + 1 : a);
    if (n != 1) {
        oc(',');
        onum(n);
    }
}

static void out_unified(const char *n1, const char *n2)
{
    unsigned i = 0, j = 0, a0, b0, a1, b1, k, ii, jj;
    unsigned na = ta.nl, nb = tb.nl;
    os("--- ");
    os(n1);
    onl();
    os("+++ ");
    os(n2);
    onl();
    for (;;) {
        while (i < na && j < nb && !CHA(i) && !CHB(j))
            i++, j++;
        if (i >= na && j >= nb)
            break;
        a0 = i > ctx ? i - ctx : 0;
        b0 = j - (i - a0);
        for (;;) {                              /* Ende der Gruppe      */
            while (i < na && CHA(i))
                i++;
            while (j < nb && CHB(j))
                j++;
            k = 0;
            while (i + k < na && j + k < nb && !CHA(i + k) && !CHB(j + k))
                k++;
            if (i + k >= na && j + k >= nb) {   /* nichts mehr danach   */
                a1 = i + (k < ctx ? k : ctx);
                b1 = j + (k < ctx ? k : ctx);
                i += k;
                j += k;
                break;
            }
            if (k > 2 * ctx) {
                a1 = i + ctx;
                b1 = j + ctx;
                i += k;
                j += k;
                break;
            }
            i += k;
            j += k;
        }
        os("@@ -");
        uhead(a0, a1 - a0);
        os(" +");
        uhead(b0, b1 - b0);
        os(" @@");
        onl();
        for (ii = a0, jj = b0; ii < a1 || jj < b1; ) {
            if (ii < a1 && CHA(ii))
                line_out('-', &ta, ii++);
            else if (jj < b1 && CHB(jj))
                line_out('+', &tb, jj++);
            else {
                line_out(' ', &ta, ii++);
                jj++;
            }
        }
    }
}

/* ---- ein Dateipaar ----------------------------------------------------- */
static int binary(struct text *t)
{
    unsigned long p, e = t->size < 4096 ? t->size : 4096;
    for (p = 0; p < e; p++)
        if (TX_BYTE(t, p) == 0)
            return 1;
    return 0;
}

static int same_bytes(void)
{
    unsigned long p;
    if (ta.size != tb.size)
        return 0;
    for (p = 0; p < ta.size; p++)
        if (TX_BYTE(&ta, p) != TX_BYTE(&tb, p))
            return 0;
    return 1;
}

static void freeall(void)
{
    unsigned *s[] = { &sel_ca, &sel_cb, &sel_rep, &sel_nxt, &sel_hs, &sel_head,
                      &sel_fd, &sel_bd, &sel_ch };
    unsigned i;
    for (i = 0; i < sizeof(s) / sizeof(s[0]); i++)
        if (*s[i]) {
            DosFreeSeg(*s[i]);
            *s[i] = 0;
        }
    tx_free(&ta);
    tx_free(&tb);
}

static void err(const char *a, const char *b)
{
    oflush();
    puts("diff: ");
    puts(a);
    puts(b);
    puts("\r\n");
    status = 2;
}

static void diff_files(const char *n1, const char *n2)
{
    int r1, r2, diff = 0;
    unsigned i, na, nb;

    r1 = tx_load(n1, &ta);
    r2 = tx_load(n2, &tb);
    if ((r1 == 1 && !newfile) || (r2 == 1 && !newfile) || r1 == 2 || r2 == 2) {
        if (r1 == 2 || r2 == 2)
            err(r1 == 2 ? n1 : n2, ": zu gro\xE1 (h\x94" "chstens 256 KB, 16.000 Zeilen)");
        else
            err(r1 == 1 ? n1 : n2, ": Datei oder Verzeichnis nicht gefunden");
        freeall();
        return;
    }
    if (!astext && (binary(&ta) || binary(&tb))) {
        if (!same_bytes()) {
            oflush();
            os("Bin\x84rdateien ");
            os(n1);
            os(" und ");
            os(n2);
            os(" sind verschieden.");
            onl();
            if (status < 1)
                status = 1;
        } else if (report_same) {
            os("Dateien ");
            os(n1);
            os(" und ");
            os(n2);
            os(" sind identisch.");
            onl();
        }
        freeall();
        return;
    }
    na = ta.nl;
    nb = tb.nl;
    if (!(sel_ca = seg(na * 2)) || !(sel_cb = seg(nb * 2)) ||
        !(sel_rep = seg((na + nb) * 2)) || !(sel_nxt = seg((na + nb) * 2)) ||
        !(sel_hs = seg((na + nb) * 2)) || !(sel_head = seg(8192)) ||
        !(sel_fd = seg((na + nb + 4) * 2)) || !(sel_bd = seg((na + nb + 4) * 2)) ||
        !(sel_ch = seg(TX_MAXL * 2))) {
        err(n1, ": nicht gen\x81gend Arbeitsspeicher");
        freeall();
        return;
    }
    for (i = 0; i < 4096; i++)
        HEAD(i) = 0;
    for (i = 0; i < TX_MAXL; i++)
        CHA(i) = CHB(i) = 0;
    nclass = 0;
    for (i = 0; i < na; i++)
        CA(i) = classify(&ta, 0, i);
    for (i = 0; i < nb; i++)
        CB(i) = classify(&tb, 0x8000, i);
    voff = nb + 1;
    compareseq(0, (int)na, 0, (int)nb);
    for (i = 0; i < na && !diff; i++)
        diff = CHA(i);
    for (i = 0; i < nb && !diff; i++)
        diff = CHB(i);
    /* nur das fehlende Zeilenende der letzten Zeile verschieden */
    if (!diff && na && na == nb && ta.nonl != tb.nonl) {
        CHA(na - 1) = CHB(nb - 1) = 1;
        diff = 1;
    }
    if (!diff) {
        if (report_same) {
            os("Dateien ");
            os(n1);
            os(" und ");
            os(n2);
            os(" sind identisch.");
            onl();
        }
    } else {
        if (status < 1)
            status = 1;
        if (quiet) {
            os("Dateien ");
            os(n1);
            os(" und ");
            os(n2);
            os(" sind verschieden.");
            onl();
        } else {
            if (recurse) {
                os("diff ");
                os(fmt_u ? "-u " : "");
                os(n1);
                oc(' ');
                os(n2);
                onl();
            }
            if (fmt_u)
                out_unified(n1, n2);
            else
                out_normal();
        }
    }
    freeall();
}

/* ---- Verzeichnisse ----------------------------------------------------- */
#define MAXDEPTH 8
static char dpa[MAXDEPTH][FAPI_PATHLEN], dpb[MAXDEPTH][FAPI_PATHLEN];
static struct fapi_dirent de;

static int exists_in(const char *dir, const char *name, int *isdir)
{
    char p[FAPI_PATHLEN];
    join(p, dir, name, sizeof(p));
    *isdir = is_dir(p);
    if (*isdir)
        return 1;
    {
        unsigned h = DosOpen(p);
        if (FAPI_FAILED(h))
            return 0;
        DosClose(h);
        return 1;
    }
}

static void diff_dirs(const char *a, const char *b, int depth)
{
    unsigned n;
    int d2;
    char *pa, *pb;
    if (depth >= MAXDEPTH) {
        err(a, ": zu tief verschachtelt");
        return;
    }
    pa = dpa[depth];
    pb = dpb[depth];
    for (n = 0; DosFindEntry(&de, n, a) != FAPI_ERROR; n++) {
        if ((de.name[0] == '.' && (de.name[1] == 0 || (de.name[1] == '.' && de.name[2] == 0))) ||
            (de.attr & 0x08))
            continue;
        join(pa, a, de.name, FAPI_PATHLEN);
        join(pb, b, de.name, FAPI_PATHLEN);
        if (!exists_in(b, de.name, &d2)) {
            if (newfile && !(de.attr & 0x10)) {
                diff_files(pa, pb);
                continue;
            }
            os("Nur in ");
            os(a);
            os(": ");
            os(de.name);
            onl();
            if (status < 1)
                status = 1;
            continue;
        }
        if ((de.attr & 0x10) && d2) {
            if (recurse)
                diff_dirs(pa, pb, depth + 1);
            else {
                os("Gemeinsame Unterverzeichnisse: ");
                os(pa);
                os(" und ");
                os(pb);
                onl();
            }
        } else if ((de.attr & 0x10) || d2) {
            os(de.attr & 0x10 ? "Verzeichnis " : "Datei ");
            os(pa);
            os(" und ");
            os(d2 ? "Verzeichnis " : "Datei ");
            os(pb);
            os(" sind verschieden.");
            onl();
            if (status < 1)
                status = 1;
        } else
            diff_files(pa, pb);
        pa = dpa[depth];                        /* (Rekursion benutzt die */
        pb = dpb[depth];                        /*  naechste Ebene)       */
    }
    for (n = 0; DosFindEntry(&de, n, b) != FAPI_ERROR; n++) {
        if ((de.name[0] == '.' && (de.name[1] == 0 || (de.name[1] == '.' && de.name[2] == 0))) ||
            (de.attr & 0x08))
            continue;
        if (exists_in(a, de.name, &d2))
            continue;
        if (newfile && !(de.attr & 0x10)) {
            join(pa, a, de.name, FAPI_PATHLEN);
            join(pb, b, de.name, FAPI_PATHLEN);
            diff_files(pa, pb);
            continue;
        }
        os("Nur in ");
        os(b);
        os(": ");
        os(de.name);
        onl();
        if (status < 1)
            status = 1;
    }
}

static void usage(void)
{
    puts("Vergleicht Textdateien zeilenweise.\r\n\r\n"
         "DIFF [-u | -U n] [-q] [-s] [-i] [-b] [-w] [-r] [-N] [-a] Datei1 Datei2\r\n\r\n"
         "  -u, -U n  vereinheitlichte Form (f\x81r PATCH), n Zeilen Umgebung (3)\r\n"
         "  -q        nur melden, ob die Dateien verschieden sind\r\n"
         "  -s        auch gleiche Dateien melden\r\n"
         "  -i        Gro\xE1-/Kleinschreibung nicht beachten\r\n"
         "  -b        Folgen von Leerzeichen gelten als eines\r\n"
         "  -w        Leerzeichen ganz \x81" "bergehen\r\n"
         "  -r        Verzeichnisse mit Unterverzeichnissen vergleichen\r\n"
         "  -N        fehlende Datei gilt als leer\r\n"
         "  -a        Bin\x84rdateien wie Text behandeln\r\n\r\n"
         "ERRORLEVEL: 0 gleich, 1 verschieden, 2 Fehler.\r\n");
}

static unsigned num(const char *s)
{
    unsigned v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

void main(void)
{
    char w[FAPI_PATHLEN], *a, *p;
    int nn = 0, want_u = 0, d1, d2;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (want_u) {
            ctx = num(w);
            want_u = 0;
            continue;
        }
        if ((w[0] == '/' && w[1] == '?') || same(w, "--help")) {
            usage();
            DosExit(0);
        }
        if (w[0] == '-' && w[1] == '-') {
            if (same(w, "--unified")) fmt_u = 1;
            else if (same(w, "--brief")) quiet = 1;
            else if (same(w, "--ignore-case")) icase = 1;
            else if (same(w, "--recursive")) recurse = 1;
            else if (same(w, "--new-file")) newfile = 1;
            else if (same(w, "--text")) astext = 1;
            continue;
        }
        if (w[0] == '-' && w[1]) {
            for (p = w + 1; *p; p++) {
                switch (*p) {
                case 'u': fmt_u = 1; break;
                case 'U':
                    fmt_u = 1;
                    if (p[1]) {
                        ctx = num(p + 1);
                        p += slen(p) - 1;
                    } else
                        want_u = 1;
                    break;
                case 'q': quiet = 1; break;
                case 's': report_same = 1; break;
                case 'i': icase = 1; break;
                case 'b': ispace = 1; break;
                case 'w': iall = 1; break;
                case 'r': recurse = 1; break;
                case 'N': newfile = 1; break;
                case 'a': astext = 1; break;
                default:
                    if (*p >= '0' && *p <= '9') {   /* -5 = -U5         */
                        ctx = num(p);
                        fmt_u = 1;
                        p += slen(p) - 1;
                        break;
                    }
                    puts("diff: unbekannter Schalter -");
                    putc(*p);
                    puts("\r\nDIFF /? zeigt die Schalter.\r\n");
                    DosExit(2);
                }
            }
            continue;
        }
        if (nn == 0)
            scopy(nm1, w, sizeof(nm1));
        else if (nn == 1)
            scopy(nm2, w, sizeof(nm2));
        nn++;
    }
    if (nn != 2) {
        puts(nn < 2 ? "diff: Operand fehlt\r\n" : "diff: zu viele Operanden\r\n");
        puts("DIFF /? zeigt die Bedienung.\r\n");
        DosExit(2);
    }
    d1 = !(nm1[0] == '-' && !nm1[1]) && is_dir(nm1);
    d2 = !(nm2[0] == '-' && !nm2[1]) && is_dir(nm2);
    if (d1 && d2)
        diff_dirs(nm1, nm2, 0);
    else if (d1) {
        join(w, nm1, base_name(nm2), sizeof(w));
        diff_files(w, nm2);
    } else if (d2) {
        join(w, nm2, base_name(nm1), sizeof(w));
        diff_files(nm1, w);
    } else
        diff_files(nm1, nm2);
    oflush();
    DosExit((unsigned)status);
}
