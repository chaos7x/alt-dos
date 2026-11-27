/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Gemeinsamer Teil von PS, KILL, PSTREE und TOP: Prozessliste ueber
 * DosProc(PROC_INFO2) lesen (PIDs = Plaetze 0-7 im Kernel), Zeiten und
 * Zahlen ohne 32-Bit-Division ausgeben. Per #include (nach fapi.h, util.h).
 */
#ifndef PROC_H
#define PROC_H

#define NP      8                       /* Plaetze (NTASK im Kernel)      */

static struct fapi_proc2 pt[NP];
static unsigned char pu[NP];            /* Platz belegt                   */

static void pr_read(void)
{
    unsigned t;
    for (t = 0; t < NP; t++)
    {
        unsigned n;
        pu[t] = DosProc(PROC_INFO2, t, &pt[t]) == 0 && pt[t].p.state != 0;
        for (n = slen(pt[t].p.args); n && pt[t].p.args[n - 1] == ' '; n--)
            pt[t].p.args[n - 1] = 0;    /* Leerzeichen am Ende weg       */
    }
}

#ifndef PR_NONUM
/* v / d (d 16 Bit) ohne __U4D -> Quotient, Rest in *r */
static unsigned long pr_div(unsigned long v, unsigned d, unsigned *r)
{
    unsigned long q = 0, rem = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((v >> i) & 1);
        if (rem >= d) {
            rem -= d;
            q |= 1UL << i;
        }
    }
    if (r)
        *r = (unsigned)rem;
    return q;
}

/* Zahl rechtsbuendig in w Zeichen (w 0 = ohne Auffuellen) nach o */
static char *pr_num(char *o, unsigned long v, int w)
{
    char t[11];
    int n = 0;
    unsigned r;
    do {
        v = pr_div(v, 10, &r);
        t[n++] = (char)('0' + r);
    } while (v);
    while (w-- > n)
        *o++ = ' ';
    while (n)
        *o++ = t[--n];
    *o = 0;
    return o;
}

static char *pr_two(char *o, unsigned v)
{
    *o++ = (char)('0' + v / 10);
    *o++ = (char)('0' + v % 10);
    *o = 0;
    return o;
}

#endif

#ifdef PR_TIME
/* Takte (100 je s) als [[h:]m]m:ss wie ps (TIME) */
static char *pr_time(char *o, unsigned long tk)
{
    unsigned s, m;
    unsigned long sec = pr_div(tk, 100, 0);
    unsigned long mins = pr_div(sec, 60, &s);
    unsigned long h = pr_div(mins, 60, &m);
    if (h) {
        o = pr_num(o, h, 0);
        *o++ = ':';
        o = pr_two(o, m);
    } else
        o = pr_num(o, m, 0);
    *o++ = ':';
    return pr_two(o, s);
}
#endif

#ifdef PR_KIND
static const char *pr_kind(unsigned t)
{
    return pt[t].p.kind == 1 ? "DOS-Box" : pt[t].p.kind == 2 ? "DOS-Box (ruht)" : "";
}
#endif

#ifdef PR_STAT
/* Zustand wie ps: R laeuft/bereit, S wartet auf ein Kind, D im Treiber */
static char pr_stat(unsigned t)
{
    return pt[t].p.state == 2 ? 'S' : pt[t].p.state == 3 ? 'D' : 'R';
}
#endif

#ifdef PR_NAMES
/* Pid aus Text ("3") -> 0..7, sonst -1 */
static int pr_pid(const char *s)
{
    if (s[0] >= '0' && s[0] < '0' + NP && (s[1] == 0 || s[1] == ' '))
        return s[0] - '0';
    return -1;
}

/* Name vergleichen, auch ohne Endung ("edit" = EDIT.EXE) */
static int pr_name(unsigned t, const char *n)
{
    const char *a = pt[t].p.name;
    while (*n && *a && up(*n) == up(*a))
        n++, a++;
    return *n == 0 && (*a == 0 || *a == '.');
}
#endif

#ifdef PR_KILL
/* t und alles darunter beenden (Kinder zuerst) -> 0 ok */
static unsigned pr_kill(unsigned t)
{
    unsigned c;
    pr_read();
    for (c = 0; c < NP; c++)
        if (pu[c] && pt[c].p.parent == t && c != t)
            pr_kill(c);
    return DosProc(PROC_KILL, t, &pt[t]);
}
#endif

#endif
