/*
 * TR.EXE - Zeichen ersetzen oder loeschen (nach dem Vorbild von GNU tr),
 * von der Standardeingabe auf die Standardausgabe:
 *
 *   TR [-c] [-d] [-s] SATZ1 [SATZ2]
 *
 *   TR a-z A-Z            Kleinbuchstaben gross
 *   TR -d \r              Wagenruecklaeufe loeschen (Text von DOS -> Unix)
 *   TR -s " "             mehrere Leerzeichen zu einem
 *   -c  Ergaenzung von SATZ1 (alle Zeichen, die NICHT darin sind)
 * Saetze: Zeichen, Bereiche a-z, \n \r \t \\ \NNN (oktal),
 * [:upper:] [:lower:] [:digit:] [:alpha:] [:alnum:] [:space:] [:punct:];
 * "..." haelt Leerzeichen zusammen. Ist SATZ2 kuerzer, wird sein letztes
 * Zeichen wiederholt.
 */
#include "fapi.h"
#include "util.h"
#include "textin.h"

static char args[FAPI_ARGLEN];
static unsigned char s1[256], s2[256], map[256], del[256], sq[256];
static unsigned n1, n2;
static int o_c, o_d, o_s;

static int is_cls(int c, const char *cls)
{
    if (same(cls, "upper")) return c >= 'A' && c <= 'Z';
    if (same(cls, "lower")) return c >= 'a' && c <= 'z';
    if (same(cls, "digit")) return c >= '0' && c <= '9';
    if (same(cls, "alpha")) return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (same(cls, "alnum")) return is_cls(c, "alpha") || is_cls(c, "digit");
    if (same(cls, "space")) return c == ' ' || (c >= 9 && c <= 13);
    if (same(cls, "punct")) return c > 32 && c < 127 && !is_cls(c, "alnum");
    return 0;
}

static int esc(const char **pp)
{
    const char *p = *pp;
    int v = 0, k;
    if (*p != '\\' || !p[1]) {
        *pp = p + 1;
        return (unsigned char)*p;
    }
    p++;
    switch (*p) {
    case 'n': v = '\n'; p++; break;
    case 'r': v = '\r'; p++; break;
    case 't': v = '\t'; p++; break;
    case 'f': v = '\f'; p++; break;
    case 'v': v = '\v'; p++; break;
    case 'a': v = 7; p++; break;
    case 'b': v = 8; p++; break;
    default:
        if (*p >= '0' && *p <= '7') {
            for (k = 0; k < 3 && *p >= '0' && *p <= '7'; k++)
                v = v * 8 + (*p++ - '0');
        } else
            v = (unsigned char)*p++;
    }
    *pp = p;
    return v;
}

/* Satz aus dem Text s nach out; -> Anzahl */
static unsigned expand(const char *s, unsigned char *out)
{
    static char cls[12];
    unsigned n = 0, i;
    int a, b;
    while (*s && n < 256) {
        if (s[0] == '[' && s[1] == ':') {
            for (i = 0, s += 2; *s && *s != ':' && i < sizeof(cls) - 1; )
                cls[i++] = *s++;
            cls[i] = 0;
            if (s[0] == ':' && s[1] == ']')
                s += 2;
            for (a = 0; a < 256 && n < 256; a++)
                if (is_cls(a, cls))
                    out[n++] = (unsigned char)a;
            continue;
        }
        a = esc(&s);
        if (s[0] == '-' && s[1]) {      /* Bereich                       */
            s++;
            b = esc(&s);
            for (; a <= b && n < 256; a++)
                out[n++] = (unsigned char)a;
            continue;
        }
        out[n++] = (unsigned char)a;
    }
    return n;
}

static char *take(char *a, char *o, unsigned max)    /* "..." oder Wort */
{
    unsigned n = 0;
    if (*a == '"') {
        for (a++; *a && *a != '"' && n < max - 1; )
            o[n++] = *a++;
        if (*a == '"')
            a++;
    } else
        while (*a && *a != ' ' && n < max - 1)
            o[n++] = *a++;
    o[n] = 0;
    return a;
}

void main(void)
{
    static char t1[128], t2[128];
    char *a;
    int c, last = -1, have1 = 0, have2 = 0;
    unsigned i;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Ersetzt oder l\x94scht Zeichen (nach dem Vorbild von GNU tr).\r\n\r\n"
                 "TR [-c] [-d] [-s] Satz1 [Satz2]   (liest die Standardeingabe)\r\n\r\n"
                 "  -d  Zeichen aus Satz1 l\x94schen    -s  Wiederholungen zusammenfassen\r\n"
                 "  -c  alle Zeichen au\xE1" "er Satz1\r\n"
                 "S\x84tze: a-z, \\n \\r \\t \\NNN, [:upper:] [:lower:] [:digit:] [:space:] ...\r\n"
                 "Beispiel: TYPE BRIEF.TXT | TR a-z A-Z\r\n");
            return;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ' && !have1) {
            char *o;
            int opt = 1;
            for (o = a + 1; *o && *o != ' '; o++)
                if (*o != 'c' && *o != 'd' && *o != 's' && *o != 'C')
                    opt = 0;
            if (opt) {
                for (o = a + 1; *o && *o != ' '; o++) {
                    if (*o == 'c' || *o == 'C') o_c = 1;
                    if (*o == 'd') o_d = 1;
                    if (*o == 's') o_s = 1;
                }
                a = o;
                continue;
            }
        }
        if (!have1) {
            a = take(a, t1, sizeof(t1));
            have1 = 1;
        } else if (!have2) {
            a = take(a, t2, sizeof(t2));
            have2 = 1;
        } else
            a = take(a, t2, sizeof(t2));
    }
    if (!have1 || (!have2 && !o_d && !o_s)) {
        puts("TR: Satz fehlt (TR /? zeigt die Hilfe)\r\n");
        DosExit(1);
    }
    n1 = expand(t1, s1);
    n2 = have2 ? expand(t2, s2) : 0;
    if (o_c) {                          /* Ergaenzung                    */
        static unsigned char in1[256], cs[256];
        unsigned k = 0;
        for (i = 0; i < n1; i++)
            in1[s1[i]] = 1;
        for (i = 0; i < 256; i++)
            if (!in1[i])
                cs[k++] = (unsigned char)i;
        for (i = 0; i < k; i++)
            s1[i] = cs[i];
        n1 = k;
    }
    for (i = 0; i < 256; i++)
        map[i] = (unsigned char)i;
    for (i = 0; i < n1; i++) {
        if (o_d)
            del[s1[i]] = 1;
        else if (n2)
            map[s1[i]] = s2[i < n2 ? i : n2 - 1];
        if (o_s && !n2)
            sq[s1[i]] = 1;
    }
    if (o_s && n2)                      /* -s mit Satz2: dessen Zeichen  */
        for (i = 0; i < n2; i++)
            sq[s2[i]] = 1;
    else if (o_s && o_d && n2)
        for (i = 0; i < n2; i++)
            sq[s2[i]] = 1;
    ti_open(STDIN);
    while ((c = ti_get()) >= 0) {
        if (del[c])
            continue;
        c = map[c];
        if (o_s && sq[c] && c == last)
            continue;
        last = c;
        to_put((char)c);
    }
    to_flush();
}
