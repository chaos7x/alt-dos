/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * XARGS.EXE - Befehle aus der Standardeingabe zusammensetzen, wie xargs:
 *
 *   ... | XARGS [-n Zahl] [-I Platzhalter] [-t] [-r] [Befehl [Argumente]]
 *
 * Die Woerter der Eingabe (Leerzeichen/Zeilenenden trennen, "..." und '...'
 * halten zusammen) werden an den Befehl (Vorgabe ECHO) gehaengt, so viele
 * wie in eine Befehlszeile passen (126 Zeichen) bzw. -n. Mit -I laeuft der
 * Befehl je Zeile, der Platzhalter wird durch die Zeile ersetzt. -t zeigt
 * jeden Befehl, -r startet bei leerer Eingabe nichts. Gestartet wird ueber
 * COMSPEC /C. ERRORLEVEL 123, wenn ein Aufruf nicht 0 lieferte.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], base[FAPI_ARGLEN], cmd[300], line[300], ph[16];
static char comspec[FAPI_PATHLEN], word_[200];
static char ib[2048];
static unsigned ip, il, maxn, nadd;
static int trace, noempty, rc, eof_;

static int in_c(void)
{
    if (ip >= il) {
        il = DosRead(STDIN, ib, sizeof(ib));
        if (FAPI_FAILED(il) || !il) {
            il = 0;
            return -1;
        }
        ip = 0;
    }
    return (unsigned char)ib[ip++];
}

static void run(const char *c)
{
    static char full[FAPI_ARGLEN + FAPI_PATHLEN];
    unsigned r;
    if (trace) {
        VioWrtTTY(c, slen(c));
        VioWrtTTY("\r\n", 2);
    }
    scopy(full, comspec, sizeof(full));
    scat(full, " /C ", sizeof(full));
    scat(full, c, sizeof(full));
    r = DosExecPgm(full, EXEC_SYNC);
    if (r || DosQChildRC())
        rc = 123;
}

/* naechstes Wort nach word_ -> 0 = Ende */
static int next_w(void)
{
    int c, n = 0, q = 0;
    do c = in_c(); while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (c < 0)
        return 0;
    for (;;) {
        if (c < 0)
            break;
        if (!q && (c == '"' || c == '\'')) {
            q = c;
        } else if (q && c == q) {
            q = 0;
        } else if (!q && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
            break;
        else if (n < (int)sizeof(word_) - 1)
            word_[n++] = (char)c;
        c = in_c();
    }
    word_[n] = 0;
    return 1;
}

/* naechste Zeile nach line (fuer -I) -> 0 = Ende */
static int next_l(void)
{
    int c, n = 0;
    c = in_c();
    if (c < 0)
        return 0;
    while (c >= 0 && c != '\n') {
        if (c != '\r' && n < (int)sizeof(line) - 1)
            line[n++] = (char)c;
        c = in_c();
    }
    line[n] = 0;
    return 1;
}

void main(void)
{
    char *a, w[64];
    int want = 0, any = 0;
    unsigned n, k;

    DosGetArgs(args);
    for (a = skip(args); *a; ) {
        char *s = a;
        a = word(a, w, sizeof(w));
        if (want == 'n') { maxn = 0; for (k = 0; w[k] >= '0' && w[k] <= '9'; k++) maxn = maxn * 10 + (w[k] - '0'); want = 0; a = skip(a); continue; }
        if (want == 'I') { scopy(ph, w, sizeof(ph)); want = 0; a = skip(a); continue; }
        if (w[0] == '/' && w[1] == '?') {
            puts("Setzt Befehle aus der Standardeingabe zusammen.\r\n\r\n"
                 "... | XARGS [-n Zahl] [-I Platzhalter] [-t] [-r] [Befehl [Argumente]]\r\n\r\n"
                 "  -n  h\x94" "chstens so viele W\x94rter je Aufruf\r\n"
                 "  -I  je Zeile ein Aufruf, Platzhalter wird ersetzt\r\n"
                 "  -t  Befehle zeigen\r\n"
                 "  -r  bei leerer Eingabe nichts starten\r\n");
            return;
        }
        if (w[0] == '-' && w[1] == 'n' && !w[2]) { want = 'n'; a = skip(a); continue; }
        if (w[0] == '-' && w[1] == 'I' && !w[2]) { want = 'I'; a = skip(a); continue; }
        if (w[0] == '-' && w[1] == 't' && !w[2]) { trace = 1; a = skip(a); continue; }
        if (w[0] == '-' && w[1] == 'r' && !w[2]) { noempty = 1; a = skip(a); continue; }
        scopy(base, s, sizeof(base));            /* Rest = Befehl        */
        break;
    }
    if (!base[0])
        scopy(base, "ECHO", sizeof(base));
    if (FAPI_FAILED(DosGetEnv("COMSPEC", comspec, sizeof(comspec))) || !comspec[0])
        scopy(comspec, "CMD.EXE", sizeof(comspec));
    if (ph[0]) {                                /* -I: je Zeile         */
        while (next_l()) {
            char *b = base, *o = cmd;
            unsigned pl = slen(ph);
            if (!line[0])
                continue;
            any = 1;
            while (*b && o < cmd + sizeof(cmd) - 1) {
                for (k = 0; k < pl && b[k] == ph[k]; k++)
                    ;
                if (k == pl) {
                    const char *l = line;
                    while (*l && o < cmd + sizeof(cmd) - 1)
                        *o++ = *l++;
                    b += pl;
                } else
                    *o++ = *b++;
            }
            *o = 0;
            run(cmd);
        }
        DosExit((unsigned)rc);
    }
    scopy(cmd, base, sizeof(cmd));
    nadd = 0;
    eof_ = 0;
    while (!eof_) {
        if (!next_w()) {
            eof_ = 1;
            break;
        }
        any = 1;
        n = slen(word_);
        if ((nadd && slen(cmd) + n + 1 > 126) || (maxn && nadd >= maxn)) {
            run(cmd);
            scopy(cmd, base, sizeof(cmd));
            nadd = 0;
        }
        scat(cmd, " ", sizeof(cmd));
        scat(cmd, word_, sizeof(cmd));
        nadd++;
    }
    if (nadd || (!any && !noempty))
        run(cmd);
    DosExit((unsigned)rc);
}
