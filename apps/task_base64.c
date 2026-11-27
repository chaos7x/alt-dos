/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BASE64.EXE - Base64 (RFC 4648) kodieren und dekodieren, wie base64 (GNU):
 *   BASE64 [-d] [-i] [-w Spalten] [Datei | -]
 * -d dekodiert (Zeilenenden werden uebergangen, -i auch andere Zeichen),
 * -w Zeilenlaenge beim Kodieren (Vorgabe 76, 0 = keine Umbrueche).
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN];
static unsigned char ib[3072], ob[4200];
static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static unsigned op, col, wrap = 76;
static int ign;

static void oflush(void)
{
    if (op)
        DosWrite(STDOUT, ob, op);
    op = 0;
}

static void oc(unsigned char c)
{
    ob[op++] = c;
    if (op >= sizeof(ob) - 4)
        oflush();
}

static void enc_ch(char c)
{
    oc((unsigned char)c);
    if (wrap && ++col == wrap) {
        oc('\r');
        oc('\n');
        col = 0;
    }
}

static int val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

void main(void)
{
    char *a, w[FAPI_PATHLEN];
    int dec = 0, want = 0;
    unsigned h, n, i, k, rest = 0;
    unsigned char r[3];
    unsigned long acc = 0;
    int bits = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (want) {
            wrap = 0;
            for (k = 0; w[k] >= '0' && w[k] <= '9'; k++)
                wrap = wrap * 10 + (w[k] - '0');
            want = 0;
            continue;
        }
        if (w[0] == '/' && w[1] == '?') {
            puts("Kodiert oder dekodiert Base64.\r\n\r\n"
                 "BASE64 [-d] [-i] [-w Spalten] [Datei | -]\r\n\r\n"
                 "  -d  dekodieren\r\n  -i  beim Dekodieren fremde Zeichen \x81" "bergehen\r\n"
                 "  -w  Zeilenl\x84nge beim Kodieren (76, 0 = ohne Umbruch)\r\n");
            return;
        }
        if (w[0] == '-' && w[1] == 'd' && !w[2]) { dec = 1; continue; }
        if (w[0] == '-' && w[1] == 'i' && !w[2]) { ign = 1; continue; }
        if (w[0] == '-' && w[1] == 'w' && !w[2]) { want = 1; continue; }
        scopy(name, w, sizeof(name));
    }
    h = !name[0] || (name[0] == '-' && !name[1]) ? STDIN : DosOpen(name);
    if (FAPI_FAILED(h)) {
        puts("base64: Datei nicht gefunden\r\n");
        DosExit(1);
    }
    while ((n = DosRead(h, ib + rest, sizeof(ib) - rest)) != 0 && !FAPI_FAILED(n)) {
        n += rest;
        if (!dec) {
            for (i = 0; i + 3 <= n; i += 3) {
                enc_ch(tb[ib[i] >> 2]);
                enc_ch(tb[(ib[i] & 3) << 4 | ib[i + 1] >> 4]);
                enc_ch(tb[(ib[i + 1] & 15) << 2 | ib[i + 2] >> 6]);
                enc_ch(tb[ib[i + 2] & 63]);
            }
            rest = n - i;
            for (k = 0; k < rest; k++)
                ib[k] = ib[i + k];
        } else {
            for (i = 0; i < n; i++) {
                int v = val(ib[i]);
                if (ib[i] == '=' || ib[i] == '\r' || ib[i] == '\n' || ib[i] == ' ')
                    continue;
                if (v < 0) {
                    if (ign)
                        continue;
                    oflush();
                    puts("base64: ung\x81ltige Eingabe\r\n");
                    DosExit(1);
                }
                acc = (acc << 6) | (unsigned)v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    oc((unsigned char)(acc >> bits));
                }
            }
            rest = 0;
        }
    }
    if (!dec && rest) {
        r[0] = ib[0];
        r[1] = rest > 1 ? ib[1] : 0;
        enc_ch(tb[r[0] >> 2]);
        enc_ch(tb[(r[0] & 3) << 4 | r[1] >> 4]);
        enc_ch(rest > 1 ? tb[(r[1] & 15) << 2] : '=');
        enc_ch('=');
    }
    if (!dec && col) {
        oc('\r');
        oc('\n');
    }
    oflush();
    if (h != STDIN)
        DosClose(h);
}
