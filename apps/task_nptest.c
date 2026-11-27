/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NPTEST.EXE - Test fuer Named Pipes (FAPI_NPIPE, 6.5):
 *
 *   NPTEST S     Diener: \PIPE\TEST (Bytes, Echo) und \PIPE\MSG
 *                (Nachrichten, jedes Byte + 1 zurueck)
 *   NPTEST C     Kunde dazu (anderer Task)
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static unsigned char buf[2100], in[2100];
static struct fapi_nppeek pk;
static struct fapi_nptrans tr;
static struct fapi_npcall cl;
static unsigned long ms = 5000;
static unsigned bad;

static void res(const char *what, unsigned got, unsigned want)
{
    puts(what);
    puts(got == want ? ": OK\n" : ": FEHLER, Ergebnis ");
    if (got != want) {
        put_num(got, 0, 0);
        puts(" statt ");
        put_num(want, 0, 0);
        puts("\n");
        bad++;
    }
}

static void fill(unsigned n, unsigned key)
{
    unsigned i;
    for (i = 0; i < n; i++)
        buf[i] = (unsigned char)(i * 5 + key);
}

static unsigned differ(unsigned n, unsigned key, unsigned add)
{
    unsigned i, e = 0;
    for (i = 0; i < n; i++)
        if (in[i] != (unsigned char)(i * 5 + key + add))
            e++;
    return e;
}

/* genau n Byte lesen (Bytebetrieb liefert, was da ist) */
static unsigned read_all(unsigned h, unsigned n)
{
    unsigned got = 0, r;
    while (got < n) {
        r = DosRead(h, in + got, n - got);
        if (!r || FAPI_FAILED(r))
            break;
        got += r;
    }
    return got;
}

static unsigned trans(unsigned h, unsigned n, unsigned key)
{
    fill(n, key);
    tr.wbuf = (unsigned)buf;
    tr.wn = n;
    tr.rbuf = (unsigned)in;
    tr.rn = sizeof(in);
    if (DosTransactNmPipe(h, &tr) || tr.got != n)
        return 1;
    return differ(n, key, 1) != 0;
}

static void server(void)
{
    unsigned h, hm, r, i, k, n = 0;

    h = DosMakeNmPipe("\\pipe\\test", 0);
    hm = DosMakeNmPipe("\\PIPE\\MSG", NP_MSG);
    res("Diener: anlegen", FAPI_FAILED(h) || FAPI_FAILED(hm), 0);
    res("Diener: Handle-Art", DosQHandType(h), HT_PIPE);
    res("Diener: verbinden (Bytes)", DosConnectNmPipe(h), 0);
    while ((r = DosRead(h, in, sizeof(in))) != 0 && !FAPI_FAILED(r)) {
        n += r;
        DosWrite(h, in, r);
    }
    res("Diener: Ende des Kunden", r, 0);
    res("Diener: Bytes", n, 20000);
    res("Diener: trennen", DosDisConnectNmPipe(h), 0);
    DosClose(h);
    for (k = 0; k < 3; k++) {
        res("Diener: verbinden (Nachrichten)", DosConnectNmPipe(hm), 0);
        while ((r = DosRead(hm, in, sizeof(in))) != 0 && !FAPI_FAILED(r)) {
            for (i = 0; i < r; i++)
                in[i]++;
            DosWrite(hm, in, r);
        }
        res("Diener: Ende des Kunden", r, 0);
        DosDisConnectNmPipe(hm);
    }
    res("Diener: verbinden (Abschied)", DosConnectNmPipe(hm), 0);
    DosWrite(hm, "bye", 3);
    DosClose(hm);
}

static void client(void)
{
    unsigned h, i, e;

    res("Kunde: unbekannter Name", DosOpen("\\PIPE\\NIX"), FAPI_ERROR);
    res("Kunde: warten (Bytes)", DosWaitNmPipe("\\PIPE\\TEST", &ms), 0);
    h = DosOpen("\\pipe\\Test");
    res("Kunde: oeffnen", FAPI_FAILED(h), 0);
    res("Kunde: zweites Oeffnen (besetzt)", DosOpen("\\PIPE\\TEST"), FAPI_ERR_INUSE);
    res("Kunde: Handle-Art", DosQHandType(h), HT_PIPE);
    for (i = e = 0; i < 20; i++) {
        fill(1000, i);
        if (DosWrite(h, buf, 1000) != 1000 || read_all(h, 1000) != 1000 || differ(1000, i, 0))
            e++;
    }
    res("Kunde: 20 x 1000 Byte hin und zurueck", e, 0);
    DosClose(h);

    res("Kunde: warten (Nachrichten)", DosWaitNmPipe("\\PIPE\\MSG", &ms), 0);
    h = DosOpen("\\PIPE\\MSG");
    res("Kunde: oeffnen", FAPI_FAILED(h), 0);
    res("Kunde: Nachricht 1 Byte", trans(h, 1, 3), 0);
    res("Kunde: Nachricht 100 Byte", trans(h, 100, 4), 0);
    res("Kunde: Nachricht 2046 Byte", trans(h, 2046, 5), 0);
    res("Kunde: Nachricht zu gross", DosWrite(h, buf, 2047), FAPI_ERROR);
    fill(30, 9);
    DosWrite(h, buf, 10);
    DosWrite(h, buf + 10, 20);
    DosSleep(500);
    pk.buf = (unsigned)in;
    pk.n = 4;
    res("Kunde: nachsehen", DosPeekNmPipe(h, &pk), 0);
    res("Kunde: nachsehen, kopiert", pk.got, 4);
    res("Kunde: nachsehen, Nachricht", pk.msg, 10);
    res("Kunde: nachsehen, im Puffer", pk.avail, 34);
    res("Kunde: nachsehen, Zustand", pk.state, NPS_CONN);
    res("Kunde: Teil lesen", DosRead(h, in, 4), 4);
    res("Kunde: Rest lesen", DosRead(h, in + 4, 100), 6);
    res("Kunde: zweite Nachricht", DosRead(h, in + 10, 100), 20);
    res("Kunde: Inhalt", differ(30, 9, 1), 0);
    DosClose(h);

    for (i = 0; i < 2; i++) {
        fill(50, 20 + i);
        cl.ms = 5000;
        cl.t.wbuf = (unsigned)buf;
        cl.t.wn = 50;
        cl.t.rbuf = (unsigned)in;
        cl.t.rn = sizeof(in);
        res("Kunde: Aufruf (CALL)", DosCallNmPipe("\\PIPE\\MSG", &cl), 0);
        res("Kunde: Antwort", cl.t.got == 50 && !differ(50, 20 + i, 1), 1);
    }

    res("Kunde: warten (Abschied)", DosWaitNmPipe("\\PIPE\\MSG", &ms), 0);
    h = DosOpen("\\PIPE\\MSG");
    DosSleep(500);                      /* der Diener ist dann schon weg  */
    pk.n = 0;
    DosPeekNmPipe(h, &pk);
    res("Kunde: Zustand (Diener zu)", pk.state, NPS_CLOSING);
    res("Kunde: Rest lesen", DosRead(h, in, 100), 3);
    res("Kunde: Dateiende", DosRead(h, in, 100), 0);
    res("Kunde: Schreiben ohne Diener", DosWrite(h, buf, 5), FAPI_ERROR);
    DosClose(h);
    res("Kunde: Name ist weg", DosOpen("\\PIPE\\MSG"), FAPI_ERROR);
}

void main(void)
{
    char m;

    DosGetArgs(args);
    m = up(*skip(args));
    if (m == 'S')
        server();
    else if (m == 'C')
        client();
    else
        puts("NPTEST S | C\n");
    puts(bad ? "NPTEST: Fehler\n" : "NPTEST: alles in Ordnung\n");
    DosExit(bad ? 1 : 0);
}
