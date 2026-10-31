/*
 * CHOICE.EXE - Auswahl in Stapeldateien (wie MS-DOS 6):
 *
 *   CHOICE [/C[:]Tasten] [/N] [/S] [/T[:]t,nn] [Text]
 *
 * ERRORLEVEL = Nummer der gedrueckten Taste (1 = erste). Vorgabe-Tasten JN.
 * /N ohne [J,N]?, /S Gross/klein unterscheiden, /T nach nn Sekunden Taste t.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], keys[40] = "JN";

static int find(char c, int cs)
{
    unsigned i;
    for (i = 0; keys[i]; i++)
        if (cs ? keys[i] == c : up(keys[i]) == up(c))
            return (int)i + 1;
    return 0;
}

void main(void)
{
    char *a, *text = 0, tdef = 0;
    int noprompt = 0, cs = 0, r;
    unsigned secs = 0, ticks, i, k;

    DosGetArgs(args);
    for (a = skip(args); *a == '/'; a = next_word(a)) {
        char o = up(a[1]), *v = a + 2;
        if (*v == ':')
            v++;
        if (o == 'C') {
            for (i = 0; v[i] && v[i] != ' ' && i < sizeof(keys) - 1; i++)
                keys[i] = v[i];
            keys[i] = 0;
        } else if (o == 'N') {
            noprompt = 1;
        } else if (o == 'S') {
            cs = 1;
        } else if (o == 'T') {
            tdef = v[0];
            for (secs = 0, v += 2; *v >= '0' && *v <= '9'; v++)
                secs = secs * 10 + (unsigned)(*v - '0');
            if (!tdef || secs > 99) {
                puts("CHOICE: Ung\x81ltige Zeitangabe, Format /T:t,nn\r\n");
                DosExit(255);
            }
        } else if (o == '?') {
            puts("Wartet auf die Auswahl einer Taste.\r\n\r\n"
                 "CHOICE [/C[:]Tasten] [/N] [/S] [/T[:]t,nn] [Text]\r\n\r\n"
                 "  /C  erlaubte Tasten (Vorgabe JN)   /N  ohne [J,N]?\r\n"
                 "  /S  Gro\xE1/klein unterscheiden      /T  nach nn s Taste t\r\n"
                 "ERRORLEVEL ist die Nummer der Taste in der Liste.\r\n");
            return;
        }
    }
    if (*a)
        text = a;
    if (tdef && !find(tdef, cs)) {
        puts("CHOICE: Vorgabetaste fehlt in /C\r\n");
        DosExit(255);
    }
    if (text)
        puts(text);
    if (!noprompt) {
        if (text)
            putc(' ');
        putc('[');
        for (i = 0; keys[i]; i++) {
            if (i)
                putc(',');
            putc(cs ? keys[i] : up(keys[i]));
        }
        puts("]?");
    }
    ticks = secs * 10;                  /* in 100-ms-Schritten          */
    for (;;) {
        if (tdef) {
            for (k = 0; !KbdPeek() && k < ticks; k++)
                DosSleep(100);
            if (!KbdPeek()) {
                r = find(tdef, cs);
                putc(cs ? tdef : up(tdef));
                break;
            }
            tdef = 0;                   /* Taste gedrueckt: Zeit vorbei */
        }
        i = KbdCharIn() & 0xFF;
        if (i == 0)
            DosExit(0);                 /* Signal                       */
        r = find((char)i, cs);
        if (r) {
            putc(cs ? (char)i : up((char)i));
            break;
        }
        putc('\a');
    }
    puts("\r\n");
    DosExit(r);
}
