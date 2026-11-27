/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SED.EXE - Text zeilenweise bearbeiten, Teilmenge von GNU sed:
 *
 *   SED [-n] [-E|-r] [-i[Endung]] [-e Skript]... [-f Datei] [Skript] [Datei ...]
 *
 * Befehle (mit ; oder Zeilenende getrennt): s/re/ersatz/[g][p][I][n],
 * y/abc/xyz/, d, p, q, =, a Text, i Text, c Text; davor eine Adresse
 * (Zeile, $, /re/) oder ein Bereich a,b, ! kehrt um.
 * Reguläre Ausdruecke: BRE wie GNU (. * [] [^] ^ $ \( \) \{m,n\} \+ \?
 * \| \1-\9), mit -E erweiterte (( ) | + ? {m,n}). Im Ersatz & und \1-\9.
 * -i schreibt die Datei um (mit Endung: alte Fassung als Sicherung).
 *
 * Regex: eigene Umsetzung als Backtracking ueber uebersetzten Code (nach
 * der Idee von Thompson/Pike, siehe Russ Cox, "Regular Expression
 * Matching: the Virtual Machine Approach").
 */
#include "fapi.h"
#include "util.h"

#define LMAX    1024
#define NCMD    32
#define PROG    2400                    /* Code aller Ausdruecke (Worte) */

/* ---- Regex ------------------------------------------------------------ */
enum { R_CHAR = 1, R_ANY, R_CLASS, R_BOL, R_EOL, R_MATCH, R_JMP, R_SPLIT, R_SAVE, R_BREF };
static unsigned prog[PROG];             /* Befehl, Argumente            */
static unsigned pn;
static unsigned char cls[64][32];       /* Zeichenklassen (Bitfelder)   */
static unsigned ncls;
static int ere, icase_all;
static const char *rp;                  /* Leser beim Uebersetzen       */
static int rerr;
static unsigned ngrp;

static char lo(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static unsigned emit(unsigned v)
{
    if (pn >= PROG) {
        rerr = 1;
        return pn;
    }
    prog[pn] = v;
    return pn++;
}

/* Fragment [a, b) an Stelle at einfuegen: prog um n verschieben */
static void insert(unsigned at, unsigned n)
{
    unsigned i;
    if (pn + n > PROG) {
        rerr = 1;
        return;
    }
    for (i = pn; i > at; i--)
        prog[i - 1 + n] = prog[i - 1];
    /* Sprungziele hinter at anpassen */
    for (i = at + n; i < pn + n; ) {
        unsigned op = prog[i];
        if (op == R_JMP) {
            if (prog[i + 1] >= at) prog[i + 1] += n;
            i += 2;
        } else if (op == R_SPLIT) {
            if (prog[i + 1] >= at) prog[i + 1] += n;
            if (prog[i + 2] >= at) prog[i + 2] += n;
            i += 3;
        } else if (op == R_CHAR || op == R_CLASS || op == R_SAVE || op == R_BREF)
            i += 2;
        else
            i++;
    }
    pn += n;
}

static int is_meta_close(void)
{
    if (ere)
        return *rp == ')' || *rp == '|';
    return rp[0] == '\\' && (rp[1] == ')' || rp[1] == '|');
}

static void alt(void);

static void atom(void)
{
    char c = *rp;
    if (ere ? c == '(' : (c == '\\' && rp[1] == '(')) {
        unsigned g = ++ngrp;
        rp += ere ? 1 : 2;
        emit(R_SAVE); emit(2 * g);
        alt();
        if (ere ? *rp != ')' : !(rp[0] == '\\' && rp[1] == ')')) {
            rerr = 1;
            return;
        }
        rp += ere ? 1 : 2;
        emit(R_SAVE); emit(2 * g + 1);
        return;
    }
    if (c == '.') {
        rp++;
        emit(R_ANY);
        return;
    }
    if (c == '^') {
        rp++;
        emit(R_BOL);
        return;
    }
    if (c == '$' && (rp[1] == 0 || (ere ? rp[1] == ')' || rp[1] == '|' :
                                    rp[1] == '\\' && (rp[2] == ')' || rp[2] == '|')))) {
        rp++;
        emit(R_EOL);
        return;
    }
    if (c == '[') {
        unsigned char *b;
        int neg = 0, first = 1;
        unsigned i;
        if (ncls >= 64) { rerr = 1; return; }
        b = cls[ncls];
        for (i = 0; i < 32; i++) b[i] = 0;
        rp++;
        if (*rp == '^') { neg = 1; rp++; }
        while (*rp && (*rp != ']' || first)) {
            unsigned char a = (unsigned char)*rp++, z = a;
            first = 0;
            if (*rp == '-' && rp[1] && rp[1] != ']') {
                z = (unsigned char)rp[1];
                rp += 2;
            }
            for (i = a; i <= z; i++) {
                b[i >> 3] |= (unsigned char)(1 << (i & 7));
                if (icase_all) {
                    char l = lo((char)i), u = (char)(l >= 'a' && l <= 'z' ? l - 32 : l);
                    b[(unsigned char)l >> 3] |= (unsigned char)(1 << (l & 7));
                    b[(unsigned char)u >> 3] |= (unsigned char)(1 << (u & 7));
                }
                if (i == 255) break;
            }
        }
        if (*rp != ']') { rerr = 1; return; }
        rp++;
        if (neg) {
            for (i = 0; i < 32; i++) b[i] = (unsigned char)~b[i];
            b[1] &= (unsigned char)~(1 << 2);   /* \n nie */
        }
        emit(R_CLASS); emit(ncls++);
        return;
    }
    if (c == '\\' && rp[1] >= '1' && rp[1] <= '9') {
        emit(R_BREF); emit((unsigned)(rp[1] - '0'));
        rp += 2;
        return;
    }
    if (c == '\\' && rp[1]) {
        c = rp[1];
        rp += 2;
        if (c == 'n') c = '\n';
        else if (c == 't') c = '\t';
        emit(R_CHAR); emit((unsigned char)c);
        return;
    }
    rp++;
    emit(R_CHAR); emit((unsigned char)c);
}

static unsigned rnum(void)
{
    unsigned v = 0;
    while (*rp >= '0' && *rp <= '9')
        v = v * 10 + (unsigned)(*rp++ - '0');
    return v;
}

/* Fragment ab start nochmal anhaengen (fuer {m,n}) */
static void dup_frag(unsigned start, unsigned end)
{
    unsigned i, d = pn - start;
    for (i = start; i < end; i++) {
        unsigned v = prog[i];
        emit(v);
    }
    /* Spruenge im Duplikat versetzen */
    for (i = end; i < pn; ) {
        unsigned op = prog[i];
        if (op == R_JMP) { prog[i + 1] += d; i += 2; }
        else if (op == R_SPLIT) { prog[i + 1] += d; prog[i + 2] += d; i += 3; }
        else if (op == R_CHAR || op == R_CLASS || op == R_SAVE || op == R_BREF) i += 2;
        else i++;
    }
}

static void piece(void)
{
    unsigned start = pn, end, m, n, k;
    char q;
    atom();
    for (;;) {
        end = pn;
        q = *rp;
        if (q == '*' || (ere && (q == '+' || q == '?')) ||
            (!ere && q == '\\' && (rp[1] == '+' || rp[1] == '?'))) {
            if (q == '\\') { rp++; q = *rp; }
            rp++;
            if (q == '*') {             /* L: split body, out; body; jmp L */
                insert(start, 3);
                prog[start] = R_SPLIT;
                prog[start + 1] = start + 3;
                prog[start + 2] = pn + 2;
                emit(R_JMP); emit(start);
            } else if (q == '+') {      /* body; split body, out         */
                emit(R_SPLIT); emit(start); emit(pn + 1);
            } else {                    /* split body, out; body         */
                insert(start, 3);
                prog[start] = R_SPLIT;
                prog[start + 1] = start + 3;
                prog[start + 2] = pn;
            }
            continue;
        }
        if ((ere && q == '{') || (!ere && q == '\\' && rp[1] == '{')) {
            const char *save = rp;
            rp += ere ? 1 : 2;
            m = rnum();
            n = m;
            if (*rp == ',') {
                rp++;
                n = (*rp >= '0' && *rp <= '9') ? rnum() : 0xFFFF;
            }
            if (ere ? *rp != '}' : !(rp[0] == '\\' && rp[1] == '}')) {
                rp = save;
                rerr = 1;
                return;
            }
            rp += ere ? 1 : 2;
            /* body m-mal, dann (n-m) optional bzw. * */
            {
                unsigned fs = start, fe = end, i;
                unsigned len = fe - fs;
                static unsigned tmp[400];
                if (len > 400) { rerr = 1; return; }
                for (i = 0; i < len; i++) tmp[i] = prog[fs + i];
                pn = fs;                /* neu aufbauen                  */
                for (k = 0; k < m; k++) {
                    unsigned at = pn;
                    for (i = 0; i < len; i++) emit(tmp[i]);
                    /* Spruenge relativ zu fs -> at */
                    for (i = at; i < pn; ) {
                        unsigned op = prog[i];
                        if (op == R_JMP) { prog[i + 1] = prog[i + 1] - fs + at; i += 2; }
                        else if (op == R_SPLIT) { prog[i + 1] = prog[i + 1] - fs + at; prog[i + 2] = prog[i + 2] - fs + at; i += 3; }
                        else if (op == R_CHAR || op == R_CLASS || op == R_SAVE || op == R_BREF) i += 2;
                        else i++;
                    }
                }
                if (n == 0xFFFF) {      /* {m,}: body*                    */
                    unsigned at = pn;
                    emit(R_SPLIT); emit(at + 3); emit(0);
                    for (i = 0; i < len; i++) emit(tmp[i]);
                    for (i = at + 3; i < pn; ) {
                        unsigned op = prog[i];
                        if (op == R_JMP) { prog[i + 1] = prog[i + 1] - fs + at + 3; i += 2; }
                        else if (op == R_SPLIT) { prog[i + 1] = prog[i + 1] - fs + at + 3; prog[i + 2] = prog[i + 2] - fs + at + 3; i += 3; }
                        else if (op == R_CHAR || op == R_CLASS || op == R_SAVE || op == R_BREF) i += 2;
                        else i++;
                    }
                    emit(R_JMP); emit(at);
                    prog[at + 2] = pn;
                } else {
                    for (k = m; k < n; k++) {   /* optionale Kopien     */
                        unsigned at = pn;
                        emit(R_SPLIT); emit(at + 3); emit(0);
                        for (i = 0; i < len; i++) emit(tmp[i]);
                        for (i = at + 3; i < pn; ) {
                            unsigned op = prog[i];
                            if (op == R_JMP) { prog[i + 1] = prog[i + 1] - fs + at + 3; i += 2; }
                            else if (op == R_SPLIT) { prog[i + 1] = prog[i + 1] - fs + at + 3; prog[i + 2] = prog[i + 2] - fs + at + 3; i += 3; }
                            else if (op == R_CHAR || op == R_CLASS || op == R_SAVE || op == R_BREF) i += 2;
                            else i++;
                        }
                        prog[at + 2] = pn;
                    }
                }
            }
            continue;
        }
        break;
    }
    (void)dup_frag;
}

static void concat(void)
{
    while (*rp && !is_meta_close() && !rerr)
        piece();
}

static void alt(void)
{
    unsigned start = pn, j;
    concat();
    while (!rerr && (ere ? *rp == '|' : rp[0] == '\\' && rp[1] == '|')) {
        rp += ere ? 1 : 2;
        insert(start, 3);
        prog[start] = R_SPLIT;
        prog[start + 1] = start + 3;
        j = emit(R_JMP);
        emit(0);
        prog[start + 2] = pn;
        concat();
        prog[j + 1] = pn;
    }
}

/* Ausdruck s uebersetzen -> Startstelle im Code, 0xFFFF = Fehler */
static unsigned re_comp(const char *s)
{
    unsigned st = pn;
    rp = s;
    rerr = 0;
    ngrp = 0;
    emit(R_SAVE); emit(0);
    alt();
    emit(R_SAVE); emit(1);
    emit(R_MATCH);
    if (rerr || *rp)
        return 0xFFFF;
    return st;
}

/* ---- Ausfuehren (Backtracking mit eigenem Stapel) --------------------- */
static const char *sbeg, *send_;
static const char *cap[20];
static int ricase;
#define BTMAX 2200
static struct { unsigned pc; const char *sp; unsigned char slot; const char *old; } bt[BTMAX];

static int re_run(unsigned pc, const char *sp)
{
    int top = 0;
    unsigned op;
    for (;;) {
        op = prog[pc];
        switch (op) {
        case R_CHAR:
            if (sp < send_ && (ricase ? lo(*sp) == lo((char)prog[pc + 1]) : (unsigned char)*sp == prog[pc + 1])) {
                sp++;
                pc += 2;
                continue;
            }
            goto fail;
        case R_ANY:
            if (sp < send_) {
                sp++;
                pc++;
                continue;
            }
            goto fail;
        case R_CLASS:
            if (sp < send_) {
                unsigned char c = (unsigned char)*sp;
                if (cls[prog[pc + 1]][c >> 3] & (1 << (c & 7))) {
                    sp++;
                    pc += 2;
                    continue;
                }
            }
            goto fail;
        case R_BOL:
            if (sp == sbeg) { pc++; continue; }
            goto fail;
        case R_EOL:
            if (sp == send_) { pc++; continue; }
            goto fail;
        case R_MATCH:
            return 1;
        case R_JMP:
            pc = prog[pc + 1];
            continue;
        case R_SPLIT:
            if (top >= BTMAX)
                return 0;
            bt[top].pc = prog[pc + 2];
            bt[top].sp = sp;
            bt[top].slot = 0xFF;
            top++;
            pc = prog[pc + 1];
            continue;
        case R_SAVE:
            if (top >= BTMAX)
                return 0;
            bt[top].pc = 0xFFFF;        /* Rueckgaengig-Eintrag          */
            bt[top].slot = (unsigned char)prog[pc + 1];
            bt[top].old = cap[prog[pc + 1]];
            top++;
            cap[prog[pc + 1]] = sp;
            pc += 2;
            continue;
        case R_BREF: {
            unsigned g = prog[pc + 1];
            const char *a = cap[2 * g], *b = cap[2 * g + 1];
            if (!a || !b)
                goto fail;
            while (a < b) {
                if (sp >= send_ || (ricase ? lo(*a) != lo(*sp) : *a != *sp))
                    goto fail;
                a++, sp++;
            }
            pc += 2;
            continue;
        }
        }
fail:
        for (;;) {
            if (!top)
                return 0;
            top--;
            if (bt[top].pc == 0xFFFF) {
                cap[bt[top].slot] = bt[top].old;
                continue;
            }
            pc = bt[top].pc;
            sp = bt[top].sp;
            break;
        }
    }
}

/* Suche ab from in [beg, end) -> 1 gefunden (cap[0..1]) */
static int re_search(unsigned start, const char *beg, const char *from, const char *end, int ic)
{
    const char *s;
    unsigned i;
    sbeg = beg;
    send_ = end;
    ricase = ic;
    for (s = from; s <= end; s++) {
        for (i = 0; i < 20; i++)
            cap[i] = 0;
        if (re_run(start, s))
            return 1;
        if (prog[start + 2] == R_BOL)   /* ^: nur am Anfang             */
            break;
    }
    return 0;
}

/* ---- Skript ------------------------------------------------------------ */
struct addr {
    unsigned char kind;                 /* 0 keine, 1 Zeile, 2 $, 3 Regex */
    unsigned long line;
    unsigned re;
};
struct cmd {
    struct addr a1, a2;
    unsigned char neg, inrange, cmd, flags;   /* flags: 1 g, 2 p, 4 I    */
    unsigned re, nth;
    unsigned toff;                      /* Text (Ersatz, a/i/c, y)       */
    unsigned toff2;
};
static struct cmd cm[NCMD];
static unsigned ncmd;
static char txt[2048];                  /* Ersatztexte                   */
static unsigned tn;
static char script[1024];

static int nflag;
static char *sp_;                       /* Leser im Skript               */

static void serr(const char *m)
{
    puts("sed: ");
    puts(m);
    puts("\r\n");
    DosExit(1);
}

static unsigned store(const char *s, unsigned n)
{
    unsigned at = tn, i;
    if (tn + n + 1 > sizeof(txt))
        serr("Skript zu lang");
    for (i = 0; i < n; i++)
        txt[tn++] = s[i];
    txt[tn++] = 0;
    return at;
}

/* Text bis Trenner d lesen (\d -> d), -> in buf */
static void delim_text(char d, char *buf, unsigned max, int keep_bs)
{
    unsigned n = 0;
    while (*sp_ && *sp_ != d) {
        if (*sp_ == '\\' && sp_[1] == d) {
            sp_++;
        } else if (*sp_ == '\\' && sp_[1] && !keep_bs) {
            if (n < max - 1) buf[n++] = *sp_;
            sp_++;
        }
        if (n < max - 1)
            buf[n++] = *sp_;
        sp_++;
    }
    if (*sp_ != d)
        serr("unvollst\x84ndiger Befehl");
    sp_++;
    buf[n] = 0;
}

static void parse_addr(struct addr *a)
{
    static char re[256];
    a->kind = 0;
    if (*sp_ >= '0' && *sp_ <= '9') {
        a->line = 0;
        while (*sp_ >= '0' && *sp_ <= '9')
            a->line = (a->line << 3) + (a->line << 1) + (*sp_++ - '0');
        a->kind = 1;
    } else if (*sp_ == '$') {
        sp_++;
        a->kind = 2;
    } else if (*sp_ == '/' || *sp_ == '\\') {
        char d = '/';
        if (*sp_ == '\\') { sp_++; d = *sp_; }
        sp_++;
        delim_text(d, re, sizeof(re), 1);
        a->re = re_comp(re);
        if (a->re == 0xFFFF)
            serr("ung\x81ltiger regul\x84rer Ausdruck");
        a->kind = 3;
    }
}

static void parse_script(char *s)
{
    static char re[256], rep[256];
    sp_ = s;
    for (;;) {
        struct cmd *c;
        while (*sp_ == ' ' || *sp_ == ';' || *sp_ == '\n' || *sp_ == '\r' || *sp_ == '\t')
            sp_++;
        if (!*sp_)
            break;
        if (ncmd >= NCMD)
            serr("zu viele Befehle");
        c = &cm[ncmd++];
        parse_addr(&c->a1);
        c->a2.kind = 0;
        if (*sp_ == ',') {
            sp_++;
            parse_addr(&c->a2);
        }
        while (*sp_ == ' ')
            sp_++;
        if (*sp_ == '!') {
            c->neg = 1;
            sp_++;
            while (*sp_ == ' ') sp_++;
        }
        c->cmd = *sp_++;
        switch (c->cmd) {
        case 's': {
            char d = *sp_++;
            unsigned f = 0;
            delim_text(d, re, sizeof(re), 1);
            delim_text(d, rep, sizeof(rep), 1);
            c->nth = 0;
            for (;;) {
                if (*sp_ == 'g') f |= 1;
                else if (*sp_ == 'p') f |= 2;
                else if (*sp_ == 'I' || *sp_ == 'i') f |= 4;
                else if (*sp_ >= '0' && *sp_ <= '9') c->nth = c->nth * 10 + (*sp_ - '0');
                else break;
                sp_++;
            }
            c->flags = (unsigned char)f;
            icase_all = (f & 4) != 0;
            c->re = re_comp(re);
            icase_all = 0;
            if (c->re == 0xFFFF)
                serr("ung\x81ltiger regul\x84rer Ausdruck");
            c->toff = store(rep, slen(rep));
            break;
        }
        case 'y': {
            char d = *sp_++;
            delim_text(d, re, sizeof(re), 0);
            delim_text(d, rep, sizeof(rep), 0);
            if (slen(re) != slen(rep))
                serr("y: ungleich lange Zeichenfolgen");
            c->toff = store(re, slen(re));
            c->toff2 = store(rep, slen(rep));
            break;
        }
        case 'a': case 'i': case 'c': {
            unsigned n = 0;
            if (*sp_ == '\\') sp_++;
            while (*sp_ == ' ') sp_++;
            while (sp_[n] && sp_[n] != '\n' && sp_[n] != '\r' && sp_[n] != ';')
                n++;
            c->toff = store(sp_, n);
            sp_ += n;
            break;
        }
        case 'd': case 'p': case 'q': case '=':
            break;
        default:
            serr("unbekannter Befehl");
        }
    }
}

/* ---- Ausfuehren -------------------------------------------------------- */
static char ln[LMAX], nb[LMAX];
static unsigned long lno;
static int last_line;
static unsigned oh;
static char ob[2048];
static unsigned op;

static void oflush(void)
{
    if (op)
        DosWrite(oh, ob, op);
    op = 0;
}

static void os(const char *s)
{
    while (*s) {
        ob[op++] = *s++;
        if (op == sizeof(ob))
            oflush();
    }
}

static void oline(const char *s)
{
    os(s);
    os("\r\n");
}

static int amatch(struct addr *a)
{
    unsigned n;
    if (a->kind == 1)
        return lno == a->line;
    if (a->kind == 2)
        return last_line;
    n = slen(ln);
    return re_search(a->re, ln, ln, ln + n, 0);
}

static int selected(struct cmd *c)
{
    int r;
    if (!c->a1.kind)
        r = 1;
    else if (!c->a2.kind)
        r = amatch(&c->a1);
    else if (c->inrange) {
        r = 1;
        if (c->a2.kind == 1 ? lno >= c->a2.line : amatch(&c->a2))
            c->inrange = 0;
    } else if (amatch(&c->a1)) {
        r = 1;
        c->inrange = 1;
        if (c->a2.kind == 1 && c->a2.line <= lno)
            c->inrange = 0;
    } else
        r = 0;
    return c->neg ? !r : r;
}

/* s-Befehl auf ln -> 1 ersetzt */
static int subst(struct cmd *c)
{
    const char *s = ln, *e = ln + slen(ln);
    const char *from = s;
    unsigned o = 0, count = 0;
    int done = 0;
    const char *r;
    while (from <= e && re_search(c->re, s, from, e, (c->flags & 4) != 0)) {
        const char *m0 = cap[0], *m1 = cap[1];
        count++;
        while (from < m0 && o < LMAX - 1)
            nb[o++] = *from++;
        if (c->nth && count != c->nth) {
            while (from < m1 && o < LMAX - 1)
                nb[o++] = *from++;
        } else {
            for (r = txt + c->toff; *r; r++) {
                if (*r == '&') {
                    const char *q;
                    for (q = m0; q < m1 && o < LMAX - 1; q++)
                        nb[o++] = *q;
                } else if (*r == '\\' && r[1] >= '0' && r[1] <= '9') {
                    unsigned g = (unsigned)(r[1] - '0');
                    const char *q;
                    if (cap[2 * g] && cap[2 * g + 1])
                        for (q = cap[2 * g]; q < cap[2 * g + 1] && o < LMAX - 1; q++)
                            nb[o++] = *q;
                    r++;
                } else if (*r == '\\' && r[1] == 'n') {
                    if (o < LMAX - 2) { nb[o++] = '\r'; nb[o++] = '\n'; }
                    r++;
                } else if (*r == '\\' && r[1]) {
                    if (o < LMAX - 1) nb[o++] = r[1];
                    r++;
                } else if (o < LMAX - 1)
                    nb[o++] = *r;
            }
            done = 1;
            from = m1;
            if (!(c->flags & 1) && !c->nth)
                break;
        }
        if (m1 == m0) {                 /* leerer Treffer: ein Zeichen   */
            if (from < e && o < LMAX - 1)
                nb[o++] = *from;
            from++;
        }
        if (c->nth && count >= c->nth && done)
            break;
    }
    if (!done)
        return 0;
    while (from < e && o < LMAX - 1)
        nb[o++] = *from++;
    nb[o] = 0;
    scopy(ln, nb, LMAX);
    return 1;
}

/* eine Zeile durch das Skript; -> 1 = q (Ende) */
static int run_line(void)
{
    unsigned k, i;
    int deleted = 0, quit = 0;
    static char after[512];
    after[0] = 0;
    for (k = 0; k < ncmd && !deleted; k++) {
        struct cmd *c = &cm[k];
        if (!selected(c))
            continue;
        switch (c->cmd) {
        case 's':
            if (subst(c) && (c->flags & 2))
                oline(ln);
            break;
        case 'y':
            for (i = 0; ln[i]; i++) {
                const char *f = txt + c->toff, *t = txt + c->toff2;
                unsigned j;
                for (j = 0; f[j]; j++)
                    if (ln[i] == f[j]) {
                        ln[i] = t[j];
                        break;
                    }
            }
            break;
        case 'd':
            deleted = 1;
            break;
        case 'p':
            oline(ln);
            break;
        case '=': {
            char b[12];
            unsigned long v = lno;
            int n = 0;
            do { unsigned long q = 0, r = 0; int bb;
                 for (bb = 31; bb >= 0; bb--) { r = (r << 1) | ((v >> bb) & 1); if (r >= 10) { r -= 10; q |= 1UL << bb; } }
                 b[n++] = (char)('0' + (unsigned)r); v = q; } while (v);
            {
                char t[12];
                int m = 0;
                while (n) t[m++] = b[--n];
                t[m] = 0;
                oline(t);
            }
            break;
        }
        case 'a':
            scat(after, txt + c->toff, sizeof(after));
            scat(after, "\r\n", sizeof(after));
            break;
        case 'i':
            oline(txt + c->toff);
            break;
        case 'c':
            if (!c->a2.kind || !c->inrange)
                oline(txt + c->toff);
            deleted = 1;
            break;
        case 'q':
            quit = 1;
            k = ncmd;
            break;
        }
    }
    if (!deleted && !nflag)
        oline(ln);
    if (after[0])
        os(after);
    return quit;
}

/* Datei zeilenweise bearbeiten */
static char ib[2048];
static unsigned ip_, il_;
static unsigned ih;

static int rc_(void)
{
    if (ip_ >= il_) {
        il_ = DosRead(ih, ib, sizeof(ib));
        if (FAPI_FAILED(il_) || !il_) {
            il_ = 0;
            return -1;
        }
        ip_ = 0;
    }
    return (unsigned char)ib[ip_++];
}

static int peek_end(void)
{
    if (ip_ < il_)
        return 0;
    il_ = DosRead(ih, ib, sizeof(ib));
    if (FAPI_FAILED(il_) || !il_) {
        il_ = 0;
        return 1;
    }
    ip_ = 0;
    return 0;
}

/* -> 1 = q */
static int process(unsigned h, int is_last_file)
{
    int c, n;
    ih = h;
    ip_ = il_ = 0;
    for (;;) {
        n = 0;
        c = rc_();
        if (c < 0)
            return 0;
        while (c >= 0 && c != '\n') {
            if (c != '\r' && n < LMAX - 1)
                ln[n++] = (char)c;
            c = rc_();
        }
        ln[n] = 0;
        lno++;
        last_line = is_last_file && peek_end();
        if (run_line())
            return 1;
    }
}

static void usage(void)
{
    puts("Bearbeitet Text zeilenweise (Teilmenge von GNU sed).\r\n\r\n"
         "SED [-n] [-E] [-i[Endung]] [-e Skript]... [-f Datei] [Skript] [Datei ...]\r\n\r\n"
         "  -n  nur ausgeben, was p ausgibt\r\n"
         "  -E  erweiterte regul\x84re Ausdr\x81" "cke ( ) | + ? {m,n}\r\n"
         "  -i  Dateien direkt \x84ndern (mit Endung: Sicherung)\r\n"
         "Befehle: s/re/ersatz/[g][p][I][n]  y/abc/xyz/  d  p  q  =  a Text  i Text  c Text\r\n"
         "Adressen: Zeile, $, /re/, Bereich a,b, ! kehrt um.\r\n");
}

void main(void)
{
    static char args[FAPI_ARGLEN], w[FAPI_PATHLEN], sfx[16], tmp[FAPI_PATHLEN], files[8][FAPI_PATHLEN];
    char *a;
    unsigned nf = 0, k, h;
    int have_script = 0, inplace = 0, want = 0, quit = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        char *startw = a;
        a = word(a, w, sizeof(w));
        if (*startw == '\'' || *startw == '"') {     /* Skript in Quotes */
            char q = *startw;
            unsigned n = 0;
            a = startw + 1;
            while (*a && *a != q && n < sizeof(w) - 1)
                w[n++] = *a++;
            w[n] = 0;
            if (*a == q) a++;
            if (want == 'e' || !have_script) {
                scat(script, w, sizeof(script));
                scat(script, "\n", sizeof(script));
                have_script = 1;
                want = 0;
                continue;
            }
        }
        if (want == 'e') {
            scat(script, w, sizeof(script));
            scat(script, "\n", sizeof(script));
            have_script = 1;
            want = 0;
            continue;
        }
        if (want == 'f') {
            unsigned fh = DosOpen(w), n;
            if (FAPI_FAILED(fh))
                serr("Skriptdatei nicht gefunden");
            n = DosRead(fh, script + slen(script), sizeof(script) - slen(script) - 1);
            if (!FAPI_FAILED(n))
                script[slen(script) + n] = 0;
            DosClose(fh);
            have_script = 1;
            want = 0;
            continue;
        }
        if ((w[0] == '/' && w[1] == '?') || same(w, "--help")) {
            usage();
            return;
        }
        if (w[0] == '-' && w[1] == 'n' && !w[2]) { nflag = 1; continue; }
        if (w[0] == '-' && (w[1] == 'E' || w[1] == 'r') && !w[2]) { ere = 1; continue; }
        if (w[0] == '-' && w[1] == 'e' && !w[2]) { want = 'e'; continue; }
        if (w[0] == '-' && w[1] == 'f' && !w[2]) { want = 'f'; continue; }
        if (w[0] == '-' && w[1] == 'i') { inplace = 1; scopy(sfx, w + 2, sizeof(sfx)); continue; }
        if (!have_script) {
            scopy(script, w, sizeof(script));
            have_script = 1;
            continue;
        }
        if (nf < 8)
            scopy(files[nf++], w, sizeof(files[0]));
    }
    if (!have_script) {
        usage();
        DosExit(1);
    }
    parse_script(script);
    oh = STDOUT;
    if (!nf) {
        process(STDIN, 1);
        oflush();
        return;
    }
    for (k = 0; k < nf && !quit; k++) {
        h = DosOpen(files[k]);
        if (FAPI_FAILED(h)) {
            oflush();
            puts("sed: Datei nicht gefunden: ");
            puts(files[k]);
            puts("\r\n");
            continue;
        }
        if (inplace) {
            char *b;
            scopy(tmp, files[k], sizeof(tmp));
            b = base_name(tmp);
            scopy(b, "SED$$$.TMP", sizeof(tmp) - (unsigned)(b - tmp));
            oh = DosCreate(tmp);
            if (FAPI_FAILED(oh))
                serr("kann Zwischendatei nicht anlegen");
            lno = 0;
            quit = process(h, 1);
            oflush();
            DosClose(oh);
            DosClose(h);
            if (sfx[0]) {
                char bak[FAPI_PATHLEN];
                scopy(bak, files[k], sizeof(bak));
                scat(bak, sfx, sizeof(bak));
                DosDelete(bak);
                DosMove(files[k], bak);
            } else
                DosDelete(files[k]);
            DosMove(tmp, files[k]);
            quit = 0;
        } else {
            quit = process(h, k == nf - 1);
            DosClose(h);
        }
    }
    oflush();
}
