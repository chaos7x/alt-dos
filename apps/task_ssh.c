/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SSH.EXE - Anmeldung auf einem anderen Rechner ueber SSH-2 (apps/ssh.c):
 *   SSH [-p Port] [-l Benutzer] [-4|-6] [-v] [-t] [Benutzer@]Rechner [Befehl]
 * Ohne Befehl: Terminal im Vollbild (xterm-Teilmenge, apps/vt.h); mit
 * Befehl: Ausgabe auf die Standardausgabe (umleitbar), eine umgeleitete
 * Standardeingabe geht an den Befehl, ERRORLEVEL = Rueckgabewert.
 * -t erzwingt das Terminal auch mit Befehl.
 * Am Zeilenanfang: ~. beendet die Verbindung, ~~ sendet ~ (wie OpenSSH).
 * Benutzer ohne Angabe: %USER%, sonst wird gefragt.
 * ERRORLEVEL: Rueckgabewert des Befehls, 255 Verbindung/Anmeldung.
 */
#include "fapi.h"
#include "util.h"
#include "ssh.h"
#include "vt.h"

static char args[FAPI_ARGLEN], host[128], user[64], cmd[FAPI_ARGLEN];
static unsigned char kq[64];            /* Antworten des Terminals         */
static unsigned kqn, term;
static unsigned char obuf[512];

static void die(const char *m, unsigned rc)
{
    if (m) {
        ssh_out(m);
        ssh_out("\r\n");
    }
    ssh_disconnect();
    DosExit(rc);
}

static void on_vt(int ext, const unsigned char *d, unsigned n)
{
    unsigned i;
    (void)ext;
    vt_kn = 0;
    vt_put(d, n);
    for (i = 0; i < vt_kn && kqn < sizeof(kq); i++)
        kq[kqn++] = vt_kb[i];
    vt_kn = 0;
}

static void on_out(int ext, const unsigned char *d, unsigned n)
{
    DosWrite(ext ? STDERR : STDOUT, d, n);
}

static void usage(void)
{
    die("SSH [-p Port] [-l Benutzer] [-4|-6] [-v] [-t] [Benutzer@]Rechner [Befehl]\r\n"
        "  Am Zeilenanfang beendet ~. die Verbindung.", 255);
}

static unsigned num(const char *a)
{
    unsigned v = 0;
    while (*a >= '0' && *a <= '9')
        v = v * 10 + (*a++ - '0');
    return v;
}

static void interactive(void)
{
    static unsigned char x[64];
    unsigned char *q;
    unsigned k, c, bol = 1, tilde = 0;
    int ev;
    if (vt_open())
        die("Kein Vollbild m\x94glich.", 255);
    q = ssh_putstr(x, "xterm", 5);      /* pty-req                        */
    q = ssh_put32(q, 80);
    q = ssh_put32(q, vt_rows);
    q = ssh_put32(q, 0);
    q = ssh_put32(q, 0);
    q = ssh_put32(q, 6);                /* Modi: VERASE = 127, Ende       */
    *q++ = 3;
    q = ssh_put32(q, 127);
    *q++ = 0;
    ssh_data = on_vt;
    if (ssh_request("pty-req", x, (unsigned)(q - x), 1)) {
        vt_close();
        die("ssh: Kein Terminal auf dem Server.", 255);
    }
    if (cmd[0]) {
        static unsigned char cb[FAPI_ARGLEN + 4];
        unsigned l = slen(cmd);
        ssh_putstr(cb, cmd, l);
        ev = ssh_request("exec", cb, l + 4, 1);
    } else
        ev = ssh_request("shell", 0, 0, 1);
    if (ev) {
        vt_close();
        die("ssh: Server verweigert die Shell.", 255);
    }
    for (;;) {
        ev = ssh_poll(30);
        vt_flush();
        if (kqn) {
            if (ssh_write(kq, kqn))
                break;
            kqn = 0;
        }
        if (ev < 0 || ev == SSH_EV_CLOSE)
            break;
        while (KbdPeek()) {
            k = KbdCharIn();
            c = k & 0xFF;
            if (tilde) {                /* ~ am Zeilenanfang             */
                tilde = 0;
                if (c == '.') {
                    vt_close();
                    ssh_out("Verbindung beendet.\r\n");
                    die(0, 255);
                }
                if (c != '~') {
                    vt_kb[0] = '~';
                    if (ssh_write(vt_kb, 1))
                        goto out;
                }
            } else if (bol && c == '~' && !KEY_EXT(c, k >> 8)) {
                tilde = 1;
                continue;
            }
            vt_key(k);
            bol = c == '\r';
            if (vt_kn && ssh_write(vt_kb, vt_kn))
                goto out;
        }
    }
out:
    vt_flush();
    vt_close();
    ssh_out("Verbindung zu ");
    ssh_out(host);
    ssh_out(" beendet.\r\n");
}

static void batch(void)
{
    static unsigned char cb[FAPI_ARGLEN + 4];
    unsigned l = slen(cmd), r;
    int ev;
    ssh_data = on_out;
    ssh_putstr(cb, cmd, l);
    if (ssh_request("exec", cb, l + 4, 1))
        die("ssh: Server verweigert den Befehl.", 255);
    if (DosQHandType(STDIN) != HT_CONSOLE) {    /* Eingabe weiterreichen  */
        for (;;) {
            r = DosRead(STDIN, obuf, sizeof(obuf));
            if (r == 0 || FAPI_FAILED(r))
                break;
            if (ssh_write(obuf, r))
                die(0, 255);
            if (ssh_poll(0) < 0)
                die(0, 255);
        }
    }
    ssh_send_eof();
    for (;;) {
        ev = ssh_poll(200);
        if (ev < 0 || ev == SSH_EV_CLOSE)
            break;
    }
}

void main(void)
{
    char *a, *at;
    unsigned port = 22, k;
    int force_t = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (host[0]) {                  /* Rest = Befehl                  */
            scopy(cmd, a, sizeof(cmd));
            break;
        }
        if (*a == '-' && a[1] == '4' && a[2] <= ' ') ssh_fam = AF_INET;
        else if (*a == '-' && a[1] == '6' && a[2] <= ' ') ssh_fam = AF_INET6;
        else if (*a == '-' && a[1] == 'v' && a[2] <= ' ') ssh_verbose = 1;
        else if (*a == '-' && a[1] == 't' && a[2] <= ' ') force_t = 1;
        else if (*a == '-' && a[1] == 'T' && a[2] <= ' ') force_t = -1;
        else if (*a == '-' && a[1] == 'p' && a[2] <= ' ') {
            a = next_word(a);
            port = num(a);
            if (!port)
                usage();
        } else if (*a == '-' && a[1] == 'l' && a[2] <= ' ') {
            a = next_word(a);
            word(a, user, sizeof(user));
        } else if (*a == '-' || (*a == '/' && a[1] == '?'))
            usage();
        else {
            word(a, host, sizeof(host));
            for (at = host; *at && *at != '@'; at++)
                ;
            if (*at) {
                *at = 0;
                scopy(user, host, sizeof(user));
                scopy(host, at + 1, sizeof(host));
            }
        }
    }
    if (!host[0])
        usage();
    for (k = slen(cmd); k && cmd[k - 1] <= ' '; k--)
        cmd[k - 1] = 0;
    if (!user[0] && (FAPI_FAILED(DosGetEnv("USER", user, sizeof(user))) || !user[0])) {
        ssh_out("Benutzer: ");
        k = 0;
        for (;;) {
            unsigned c = KbdCharIn() & 0xFF;
            if (c == '\r')
                break;
            if (c == 27 || c == 3)
                die("", 255);
            if (c == '\b' && k) {
                k--;
                ssh_out("\b \b");
            } else if (c > ' ' && k < sizeof(user) - 1) {
                user[k++] = (char)c;
                VioWrtTTY(user + k - 1, 1);
            }
        }
        user[k] = 0;
        ssh_out("\r\n");
        if (!k)
            die(0, 255);
    }
    if (ssh_connect(host, port) || ssh_login(user) || ssh_session())
        die(0, 255);
    term = force_t > 0 || (!cmd[0] && force_t == 0);
    if (term)
        interactive();
    else
        batch();
    ssh_disconnect();
    DosExit(ssh_exit < 0 ? 0 : ssh_exit > 255 ? 255 : (unsigned)ssh_exit);
}
