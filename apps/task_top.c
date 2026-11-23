/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TOP.EXE - Programme nach Rechenzeit, laufend aktualisiert, wie top:
 *
 *   TOP [-d s] [-b] [-n Anzahl]
 *
 *   Vollbild: jede Sekunde (-d) neu; q/Esc beendet, k fragt nach einer PID
 *   und beendet sie, Leertaste aktualisiert sofort.
 *   -b  ohne Vollbild auf die Standardausgabe (fuer Umleitung), -n Runden.
 *
 * %CPU = Rechenzeit seit der letzten Runde / verstrichene Zeit; der Rest
 * ist Leerlauf. Kopf: Uhrzeit, Laufzeit, Programme, Speicher (MEM),
 * Auslagerungsdatei.
 */
#include "fapi.h"
#include "util.h"
#define PR_KILL
#define PR_STAT
#define PR_TIME
#include "proc.h"

static char args[FAPI_ARGLEN], ln[100];
static unsigned long last_cpu[NP], last_now;
static unsigned char last_used[NP];
static unsigned pct[NP];                /* Zehntel Prozent               */
static unsigned rows = 25, batch, delay = 10;   /* Zehntelsekunden        */
static struct fapi_meminfo mi;
static unsigned dt[2];           /* FAT-Datum, -Uhrzeit            */
static unsigned row;

/* Zehntel-Prozent: d * 1000 / span (beide klein genug fuer 32 Bit) */
static unsigned tenth(unsigned long d, unsigned long span)
{
    unsigned long v;
    if (!span)
        return 0;
    if (d > span)
        d = span;
    v = (d << 10) - (d << 4) - (d << 3);   /* d * 1000 ohne __U4M */
    return (unsigned)pr_div(v, (unsigned)(span > 65535UL ? 65535UL : span), 0);
}

static void out(const char *s, unsigned a)
{
    unsigned n;
    if (batch) {
        puts(s);
        puts("\r\n");
        return;
    }
    if (row >= rows)
        return;
    scopy(ln, s, 81);
    for (n = slen(ln); n < 80; n++)
        ln[n] = ' ';
    ln[80] = 0;
    VioWrtStrAtt(row, 0, ln, a);
    row++;
}

static char *kbs(char *o, unsigned long kb)
{
    if (kb >= 10240UL) {
        o = pr_num(o, pr_div(kb, 1024, 0), 0);
        *o++ = 'M';
    } else {
        o = pr_num(o, kb, 0);
        *o++ = 'K';
    }
    *o = 0;
    return o;
}

static void sample(void)
{
    unsigned t;
    unsigned long now, span;
    pr_read();
    now = 0;
    for (t = 0; t < NP; t++)
        if (pu[t]) {
            now = pt[t].now;
            break;
        }
    span = now - last_now;
    for (t = 0; t < NP; t++) {
        if (pu[t] && last_used[t] && last_now)
            pct[t] = tenth(pt[t].cpu - last_cpu[t], span);
        else
            pct[t] = 0;
        last_cpu[t] = pt[t].cpu;
        last_used[t] = pu[t];
    }
    last_now = now;
}

static void screen(void)
{
    static char b[100];
    char *o;
    unsigned t, i, n = 0, nr = 0, ns = 0, nd = 0, busy = 0, order[NP], k;
    unsigned long up = 0;

    for (t = 0; t < NP; t++)
        if (pu[t]) {
            order[n++] = t;
            busy += pct[t];
            if (pt[t].p.state == 2) ns++;
            else if (pt[t].p.state == 3) nd++;
            else nr++;
            up = pt[t].now;
        }
    for (i = 1; i < n; i++)             /* nach %CPU, dann PID           */
        for (k = i; k > 0 && pct[order[k]] > pct[order[k - 1]]; k--) {
            t = order[k]; order[k] = order[k - 1]; order[k - 1] = t;
        }
    row = 0;
    DosGetDateTime(dt);
    o = b;
    scopy(o, "top - ", 8); o += 6;
    o = pr_two(o, dt[1] >> 11); *o++ = ':';
    o = pr_two(o, (dt[1] >> 5) & 63); *o++ = ':';
    o = pr_two(o, (dt[1] & 31) * 2);
    scopy(o, "  l\x84uft seit ", 14); o += slen(o);
    o = pr_time(o, up);
    scopy(o, "  Last: ", 9); o += slen(o);
    if (busy > 1000) busy = 1000;
    o = pr_num(o, busy / 10, 0); *o++ = ','; *o++ = (char)('0' + busy % 10);
    *o++ = '%'; *o = 0;
    out(b, 0x70);
    o = b;
    scopy(o, "Programme: ", 12); o += slen(o);
    o = pr_num(o, n, 0);
    scopy(o, " gesamt, ", 10); o += slen(o);
    o = pr_num(o, nr, 0);
    scopy(o, " bereit, ", 10); o += slen(o);
    o = pr_num(o, ns, 0);
    scopy(o, " warten auf ein Kind, ", 23); o += slen(o);
    o = pr_num(o, nd, 0);
    scopy(o, " im Treiber", 12);
    out(b, 0x07);
    mi.total_kb32 = 0;
    DosMemInfo(&mi);
    o = b;
    scopy(o, "Speicher: ", 11); o += slen(o);
    o = kbs(o, mi.total_kb32 ? mi.total_kb32 : mi.total_kb);
    scopy(o, " gesamt, ", 10); o += slen(o);
    o = kbs(o, mi.total_kb32 ? mi.free_kb32 : mi.free_kb);
    scopy(o, " frei, Programme ", 18); o += slen(o);
    o = kbs(o, (unsigned long)mi.apps_kb + mi.alloc_kb);
    scopy(o, ", DOS-Boxen ", 13); o += slen(o);
    o = kbs(o, mi.box_kb);
    out(b, 0x07);
    o = b;
    scopy(o, "Auslagerung: ", 14); o += slen(o);
    if (mi.swap_kb) {
        o = kbs(o, mi.swap_kb);
        scopy(o, ", davon belegt ", 16); o += slen(o);
        o = kbs(o, mi.swapped_kb);
    } else
        scopy(o, "keine", 6);
    out(b, 0x07);
    out("", 0x07);
    out("  PID S TTY    KB   %CPU     TIME CMD", 0x70);
    for (i = 0; i < n; i++) {
        t = order[i];
        o = b;
        o = pr_num(o, t, 5);
        *o++ = ' ';
        *o++ = pr_stat(t);
        *o++ = ' ';
        *o++ = 'S';
        o = pr_num(o, pt[t].p.session + 1, 0);
        while (o < b + 12) *o++ = ' ';
        o = pr_num(o, pt[t].p.kb, 5);
        o = pr_num(o, pct[t] / 10, 5);
        *o++ = ',';
        *o++ = (char)('0' + pct[t] % 10);
        {
            char tb[16];
            unsigned m;
            pr_time(tb, pt[t].cpu);
            for (m = slen(tb); m < 9; m++)
                *o++ = ' ';
            scopy(o, tb, 16);
            o += slen(o);
        }
        *o++ = ' ';
        scopy(o, pt[t].p.name, 13);
        o += slen(o);
        if (pt[t].p.args[0]) {
            *o++ = ' ';
            scopy(o, skip(pt[t].p.args), 41);
            o += slen(o);
        }
        if (pt[t].p.kind)
            scopy(o, pt[t].p.kind == 2 ? " [DOS-Box, ruht]" : " [DOS-Box]", 20);
        b[79] = 0;
        out(b, (pt[t].p.flags & PROC_SELF) ? 0x0F : 0x07);
    }
    while (!batch && row < rows - 1)
        out("", 0x07);
    if (!batch)
        out(" q Ende   k PID beenden   Leertaste aktualisieren", 0x70);
    else
        out("", 0x07);
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
    char w[16], *a;
    unsigned rounds = 0, n, k, c;
    int want = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '/' && w[1] == '?') {
            puts("Zeigt Programme nach Rechenzeit, laufend aktualisiert.\r\n\r\n"
                 "TOP [-d Sekunden] [-b] [-n Runden]\r\n\r\n"
                 "  -d  Abstand der Runden (Vorgabe 1 s, auch 0.5)\r\n"
                 "  -b  ohne Vollbild auf die Standardausgabe\r\n"
                 "  -n  nach so vielen Runden beenden\r\n\r\n"
                 "Tasten: q Ende, k PID beenden, Leertaste aktualisieren.\r\n");
            return;
        }
        if (want == 'd') {
            delay = num(w) * 10;
            for (k = 0; w[k] && w[k] != '.' && w[k] != ','; k++)
                ;
            if (w[k] && w[k + 1] >= '0' && w[k + 1] <= '9')
                delay += (unsigned)(w[k + 1] - '0');
            if (!delay)
                delay = 1;
            want = 0;
            continue;
        }
        if (want == 'n') {
            rounds = num(w);
            want = 0;
            continue;
        }
        if (w[0] == '-' && w[1] == 'b')
            batch = 1;
        else if (w[0] == '-' && (w[1] == 'd' || w[1] == 'n')) {
            if (w[2]) {
                if (w[1] == 'n')
                    rounds = num(w + 2);
                else
                    delay = num(w + 2) * 10 ? num(w + 2) * 10 : 1;
            } else
                want = w[1];
        }
    }
    sample();
    if (!batch) {
        rows = VioSetMode(1);
        if (FAPI_FAILED(rows) || rows < 25)
            rows = 25;
        VioSetCurPos(0xFF, 0);
    }
    for (n = 0;;) {
        for (k = 0; k < delay && !(batch == 0 && KbdPeek()); k++)
            DosSleep(100);
        sample();
        screen();
        if (rounds && ++n >= rounds)
            break;
        if (batch || !KbdPeek())
            continue;
        k = KbdCharIn();
        c = k & 0xFF;
        if (c == 27 || c == 'q' || c == 'Q')
            break;
        if (c == 'k' || c == 'K') {
            row = rows - 1;
            out(" PID beenden (Ziffer, Esc = nichts): ", 0x1F);
            k = KbdCharIn() & 0xFF;
            if (k >= '0' && k < '0' + NP) {
                pr_read();
                row = rows - 1;
                out(pu[k - '0'] && pr_kill(k - '0') == 0 ? " Beendet." :
                    " Beenden ging nicht.", 0x1F);
                DosSleep(700);
            }
        }
    }
    if (!batch)
        VioSetMode(0);
}
