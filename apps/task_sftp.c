/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SFTP.EXE - Dateien ueber SSH uebertragen (SFTP Version 3,
 * draft-ietf-secsh-filexfer-02, eigene Umsetzung; Transport apps/ssh.c):
 *   SFTP [-P Port] [-l Benutzer] [-4|-6] [-v] [-b Datei] [Benutzer@]Rechner
 * Befehle (wie das sftp von OpenSSH): ls [-l] [Pfad], dir, cd, pwd, lcd,
 * lpwd, get Datei [lokal], put Datei [fern], mget Muster, mput Muster,
 * rm/del, mkdir/md, rmdir/rd, rename/ren Alt Neu, chmod Modus Datei,
 * !Befehl (lokal ueber COMSPEC), help/?, bye/exit/quit.
 * -b liest die Befehle aus einer Datei (sonst Tastatur bzw. umgeleitete
 * Standardeingabe). ERRORLEVEL: 0 ok, 1 ein Befehl scheiterte, 255
 * Verbindung/Anmeldung.
 */
#include "fapi.h"
#include "util.h"
#include "ssh.h"

#define FX_INIT     1
#define FX_VERSION  2
#define FX_OPEN     3
#define FX_CLOSE    4
#define FX_READ     5
#define FX_WRITE    6
#define FX_SETSTAT  9
#define FX_OPENDIR  11
#define FX_READDIR  12
#define FX_REMOVE   13
#define FX_MKDIR    14
#define FX_RMDIR    15
#define FX_REALPATH 16
#define FX_STAT     17
#define FX_RENAME   18
#define FX_STATUS   101
#define FX_HANDLE   102
#define FX_DATA     103
#define FX_NAME     104
#define FX_ATTRS    105

#define CHUNK       4096u

static char args[FAPI_ARGLEN], host[128], user[64], line[300];
static char rcwd[512], path[600], path2[600], lname[FAPI_PATHLEN + 2];
static unsigned char __far *ib;         /* Empfang (64 KB)                */
static unsigned ilen;
static unsigned char ob[CHUNK + 1024];  /* Senden / Datenstuecke          */
static unsigned char hnd[256];          /* Handle des Servers             */
static unsigned hlen, ip;               /* ip: Lesestelle im Paket        */
static unsigned long rid;
static unsigned scr = 0xFFFF, ilen_in, ipos_in, ieof;
static char inb[256];
static int failed;

static void out(const char *s) { puts(s); }

static void outn(unsigned long v)         /* ohne 32-Bit-Division     */
{
    static const unsigned long p10[] = { 1000000000UL, 100000000UL, 10000000UL,
        1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    char b[12];
    unsigned k, n = 0;
    char d;
    for (k = 0; k < 10; k++) {
        d = '0';
        while (v >= p10[k]) {
            v -= p10[k];
            d++;
        }
        if (d != '0' || n || k == 9)
            b[n++] = d;
    }
    b[n] = 0;
    out(b);
}

static void on_data(int ext, const unsigned char *d, unsigned n)
{
    if (ext) {
        VioWrtTTY((const char *)d, n);
        return;
    }
    while (n && ilen < 0xFFF0u) {
        ib[ilen++] = *d++;
        n--;
    }
}

static unsigned long g32(void)
{
    unsigned long v = (unsigned long)ib[ip] << 24 | (unsigned long)ib[ip + 1] << 16 |
                      (unsigned)ib[ip + 2] << 8 | ib[ip + 3];
    ip += 4;
    return v;
}

/* Zeichenkette ins nahe d (bis max-1, mit 0) -> volle Laenge */
static unsigned gstr(char *d, unsigned max)
{
    unsigned n = (unsigned)g32(), k;
    for (k = 0; k < n; k++)
        if (d && k < max - 1)
            d[k] = (char)ib[ip + k];
    if (d)
        d[n < max - 1 ? n : max - 1] = 0;
    ip += n;
    return n;
}

static unsigned long a_size, a_perm;
static void gattrs(void)
{
    unsigned long f = g32(), n;
    a_size = 0;
    a_perm = 0;
    if (f & 1) {
        g32();
        a_size = g32();
    }
    if (f & 2) { g32(); g32(); }
    if (f & 4) a_perm = g32();
    if (f & 8) { g32(); g32(); }
    if (f & 0x80000000UL) {
        n = g32();
        while (n--) {
            gstr(0, 0);
            gstr(0, 0);
        }
    }
}

/* Paket abschicken: ob[4..n) Typ + Inhalt, id wird bei Bedarf eingesetzt */
static int xmit(unsigned n)
{
    ssh_put32(ob, n - 4);
    return ssh_write(ob, n);
}

static unsigned char *head(unsigned char typ)
{
    ob[4] = typ;
    return ssh_put32(ob + 5, ++rid);
}

/* Antwort holen -> Typ, ip zeigt hinter id; 0 Fehler */
static unsigned char reply(void)
{
    unsigned long n;
    int ev;
    unsigned k;
    if (ip) {                           /* letztes Paket entfernen        */
        n = ((unsigned long)ib[0] << 24 | (unsigned long)ib[1] << 16 | (unsigned)ib[2] << 8 | ib[3]) + 4;
        for (k = (unsigned)n; k < ilen; k++)
            ib[k - (unsigned)n] = ib[k];
        ilen -= (unsigned)n;
        ip = 0;
    }
    for (;;) {
        if (ilen >= 4) {
            n = (unsigned long)ib[0] << 24 | (unsigned long)ib[1] << 16 | (unsigned)ib[2] << 8 | ib[3];
            if (n > 0xFFE0u - 4) {
                out("sftp: Antwort zu gross.\r\n");
                return 0;
            }
            if (ilen >= n + 4)
                break;
        }
        ev = ssh_poll(500);
        if (ev < 0 || ev == SSH_EV_CLOSE || ev == SSH_EV_EOF) {
            out("sftp: Verbindung getrennt.\r\n");
            ssh_disconnect();
            DosExit(255);
        }
    }
    ip = 5;
    if (ib[4] != FX_VERSION)
        ip += 4;                        /* id                             */
    return ib[4];
}

static const char *st_text(unsigned long c)
{
    switch ((unsigned)c) {
    case 0: return "OK";
    case 1: return "Ende der Datei";
    case 2: return "Datei oder Verzeichnis nicht gefunden";
    case 3: return "Zugriff verweigert";
    case 4: return "Fehler";
    case 5: return "Fehlerhafte Nachricht";
    case 8: return "Vom Server nicht unterst\x81tzt";
    }
    return "Fehler";
}

/* STATUS auswerten -> Code (0 ok); Meldung bei Fehler */
static unsigned long status(unsigned char t, const char *what)
{
    unsigned long c;
    if (t != FX_STATUS) {
        if (t)
            out("sftp: Unerwartete Antwort.\r\n");
        failed = 1;
        return 4;
    }
    c = g32();
    if (c && what) {
        out(what);
        out(": ");
        out(st_text(c));
        out("\r\n");
        failed = 1;
    }
    return c;
}

/* fernen Pfad bilden: absolut oder rcwd/arg */
static void rpath(char *d, const char *a)
{
    if (*a == '/')
        scopy(d, a, 512);
    else {
        scopy(d, rcwd, 512);
        if (slen(d) && d[slen(d) - 1] != '/')
            scat(d, "/", 512);
        scat(d, a, 512);
    }
}

static unsigned char send_path(unsigned char typ, const char *p)
{
    unsigned char *q = head(typ);
    q = ssh_putstr(q, p, slen(p));
    if (xmit((unsigned)(q - ob)))
        return 0;
    return reply();
}

static int realpath(const char *p, char *d)
{
    unsigned char t = send_path(FX_REALPATH, p);
    if (t == FX_NAME && g32() >= 1) {
        gstr(d, 512);
        return 0;
    }
    status(t, p);
    return -1;
}

static int get_handle(unsigned char t, const char *what)
{
    unsigned n;
    if (t != FX_HANDLE) {
        status(t, what);
        return -1;
    }
    n = (unsigned)g32();
    if (n > sizeof(hnd))
        return -1;
    for (hlen = 0; hlen < n; hlen++)
        hnd[hlen] = ib[ip + hlen];
    ip += n;
    return 0;
}

static void fclose_r(void)
{
    unsigned char *q = head(FX_CLOSE);
    q = ssh_putstr(q, hnd, hlen);
    if (!xmit((unsigned)(q - ob)))
        status(reply(), 0);
}

static int gmatch(const char *n, const char *p)
{
    const char *sn = 0, *sp = 0;
    while (*n) {
        if (*p == '*') {
            sp = ++p;
            sn = n;
        } else if (*p == '?' || *p == *n) {
            p++;
            n++;
        } else if (sp) {
            p = sp;
            n = ++sn;
        } else
            return 0;
    }
    while (*p == '*')
        p++;
    return !*p;
}

static const char *rbase(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '/')
            b = p + 1;
    return b;
}

/* ---- Verzeichnis -------------------------------------------------------- */
/* pat: nur passende Namen; fn != 0: je Datei aufrufen statt ausgeben */
static int list(const char *dir, int lng, const char *pat, void (*fn)(const char *))
{
    static char nm[256], ln[300];
    unsigned long n;
    unsigned char t;
    unsigned col = 0, w;
    t = send_path(FX_OPENDIR, dir);
    if (get_handle(t, dir))
        return -1;
    for (;;) {
        unsigned char *q = head(FX_READDIR);
        q = ssh_putstr(q, hnd, hlen);
        if (xmit((unsigned)(q - ob)))
            return -1;
        t = reply();
        if (t != FX_NAME)
            break;
        n = g32();
        while (n--) {
            gstr(nm, sizeof(nm));
            gstr(ln, sizeof(ln));
            gattrs();
            if (pat && !gmatch(nm, pat))
                continue;
            if (fn) {
                if ((a_perm & 0170000UL) == 0100000UL || !(a_perm & 0170000UL))
                    fn(nm);
                continue;
            }
            if (nm[0] == '.' && !lng && !pat)
                continue;
            if (lng) {
                out(ln[0] ? ln : nm);
                out("\r\n");
            } else {
                w = slen(nm);
                if (col && col + w > 78) {
                    out("\r\n");
                    col = 0;
                }
                out(nm);
                if ((a_perm & 0170000UL) == 0040000UL) {
                    out("/");
                    w++;
                }
                for (w = 16 - (w % 16); w; w--)
                    out(" ");
                col += slen(nm) + 16 - (slen(nm) % 16);
            }
        }
    }
    if (col)
        out("\r\n");
    if (t == FX_STATUS && ib[ip] == 0 && ib[ip + 1] == 0 && ib[ip + 2] == 0 && ib[ip + 3] == 1)
        ;                               /* Ende der Liste                 */
    else
        status(t, dir);
    fclose_r();
    return 0;
}

/* ---- Uebertragen -------------------------------------------------------- */
static void do_get(const char *rem, const char *loc)
{
    unsigned char *q, t;
    unsigned long off = 0;
    unsigned h, n, k;
    rpath(path, rem);
    if (!loc || !*loc)
        loc = rbase(path);
    q = head(FX_OPEN);
    q = ssh_putstr(q, path, slen(path));
    q = ssh_put32(q, 1);                /* READ                           */
    q = ssh_put32(q, 0);
    if (xmit((unsigned)(q - ob)) || get_handle(reply(), rem))
        return;
    h = DosCreate(loc);
    if (FAPI_FAILED(h)) {
        out(loc);
        out(": Datei kann nicht angelegt werden.\r\n");
        failed = 1;
        fclose_r();
        return;
    }
    out("Hole ");
    out(path);
    out(" -> ");
    out(loc);
    out("\r\n");
    for (;;) {
        q = head(FX_READ);
        q = ssh_putstr(q, hnd, hlen);
        q = ssh_put32(q, 0);
        q = ssh_put32(q, off);
        q = ssh_put32(q, CHUNK);
        if (xmit((unsigned)(q - ob)))
            break;
        t = reply();
        if (t != FX_DATA) {
            if (status(t, 0) != 1)
                status(FX_STATUS, rem), failed = 1;
            break;
        }
        n = (unsigned)g32();
        if (n > CHUNK)
            n = CHUNK;
        for (k = 0; k < n; k++)
            ob[k] = ib[ip + k];
        if (DosWrite(h, ob, n) != n) {
            out("Schreibfehler (Platte voll?).\r\n");
            failed = 1;
            break;
        }
        off += n;
    }
    DosClose(h);
    fclose_r();
    outn(off);
    out(" Byte.\r\n");
}

static void do_put(const char *loc, const char *rem)
{
    unsigned char *q;
    unsigned long off = 0;
    unsigned h, n, hd;
    h = DosOpen(loc);
    if (FAPI_FAILED(h)) {
        out(loc);
        out(": Datei nicht gefunden.\r\n");
        failed = 1;
        return;
    }
    rpath(path, rem && *rem ? rem : base_name((char *)loc));
    q = head(FX_OPEN);
    q = ssh_putstr(q, path, slen(path));
    q = ssh_put32(q, 2 | 8 | 0x10);     /* WRITE CREAT TRUNC              */
    q = ssh_put32(q, 0);
    if (xmit((unsigned)(q - ob)) || get_handle(reply(), path)) {
        DosClose(h);
        return;
    }
    out("Sende ");
    out(loc);
    out(" -> ");
    out(path);
    out("\r\n");
    hd = 4 + 1 + 4 + 4 + hlen + 8 + 4;
    for (;;) {
        n = DosRead(h, ob + hd, CHUNK);
        if (FAPI_FAILED(n)) {
            out("Lesefehler.\r\n");
            failed = 1;
            break;
        }
        if (!n)
            break;
        q = head(FX_WRITE);
        q = ssh_putstr(q, hnd, hlen);
        q = ssh_put32(q, 0);
        q = ssh_put32(q, off);
        ssh_put32(q, n);
        if (xmit(hd + n) || status(reply(), path))
            break;
        off += n;
    }
    DosClose(h);
    fclose_r();
    outn(off);
    out(" Byte.\r\n");
}

static void mget_one(const char *n)
{
    static char rn[512];
    char *s;
    scopy(rn, path2, sizeof(rn));
    s = (char *)rbase(rn);
    *s = 0;
    scat(rn, n, sizeof(rn));
    {   /* do_get nutzt path und ib - Namen vorher sichern */
        static char nn[256];
        scopy(nn, n, sizeof(nn));
        do_get(rn, nn);
    }
}

/* mget: erst alle Namen sammeln (die Liste belegt ib), dann holen */
static char names[2048];
static unsigned nn_len;
static void collect(const char *n)
{
    unsigned l = slen(n);
    if (nn_len + l + 1 < sizeof(names)) {
        scopy(names + nn_len, n, l + 1);
        nn_len += l + 1;
    }
}

static void do_mget(const char *pat)
{
    static char dir[512], np[256];
    unsigned k;
    rpath(path2, pat);
    scopy(dir, path2, sizeof(dir));
    scopy(np, rbase(path2), sizeof(np));
    *(char *)rbase(dir) = 0;
    if (!dir[0])
        scopy(dir, "/", sizeof(dir));
    nn_len = 0;
    list(dir, 0, np, collect);
    if (!nn_len) {
        out("Keine passenden Dateien.\r\n");
        failed = 1;
    }
    for (k = 0; k < nn_len; k += slen(names + k) + 1)
        mget_one(names + k);
}

static void do_mput(const char *pat)
{
    static struct fapi_dirent de;
    static char dir[FAPI_PATHLEN + 2], fn[FAPI_PATHLEN + 2];
    char *b;
    unsigned n, hit = 0;
    scopy(dir, pat, sizeof(dir));
    b = base_name(dir);
    scopy(fn, b, sizeof(fn));           /* Muster                         */
    *b = 0;
    for (n = 0; DosFindEntry(&de, n, dir[0] ? dir : 0) != FAPI_ERROR; n++) {
        if (de.attr & 0x18)
            continue;
        if (!match(de.name, fn))
            continue;
        join(lname, dir, de.name, sizeof(lname));
        do_put(lname, de.name);
        hit = 1;
    }
    if (!hit) {
        out("Keine passenden Dateien.\r\n");
        failed = 1;
    }
}

/* ---- Eingabe ------------------------------------------------------------ */
static int in_byte(void)
{
    unsigned h = scr != 0xFFFF ? scr : STDIN, r;
    if (ipos_in >= ilen_in) {
        r = DosRead(h, inb, sizeof(inb));
        if (r == 0 || FAPI_FAILED(r))
            return -1;
        ilen_in = r;
        ipos_in = 0;
    }
    return (unsigned char)inb[ipos_in++];
}

static int get_line(void)
{
    unsigned n = 0, c;
    int k;
    if (scr != 0xFFFF || DosQHandType(STDIN) != HT_CONSOLE) {
        if (ieof)
            return -1;
        while ((k = in_byte()) >= 0 && k != '\n')
            if (k != '\r' && n < sizeof(line) - 1)
                line[n++] = (char)k;
        line[n] = 0;
        if (k < 0) {
            ieof = 1;
            if (!n)
                return -1;
        }
        out(line);
        out("\r\n");
        return 0;
    }
    for (;;) {
        c = KbdCharIn() & 0xFF;
        if (c == '\r')
            break;
        if (c == '\b') {
            if (n) {
                n--;
                VioWrtTTY("\b \b", 3);
            }
        } else if (c == 27) {
            while (n) {
                n--;
                VioWrtTTY("\b \b", 3);
            }
        } else if (c >= ' ' && n < sizeof(line) - 1) {
            line[n++] = (char)c;
            VioWrtTTY(line + n - 1, 1);
        }
    }
    line[n] = 0;
    VioWrtTTY("\r\n", 2);
    return 0;
}

static void help(void)
{
    out("ls [-l] [Pfad]   Inhalt (dir = ls -l)    cd Pfad    pwd\r\n"
        "lcd Pfad         lokales Verzeichnis     lpwd\r\n"
        "get Datei [lokal]   put Datei [fern]     mget Muster   mput Muster\r\n"
        "rm Datei   mkdir Verz.   rmdir Verz.   rename Alt Neu   chmod Modus Datei\r\n"
        "!Befehl          lokaler Befehl          bye, exit, quit: Ende\r\n");
}

static int is(const char *w, const char *a, const char *b)
{
    return same(w, a) || (b && same(w, b));
}

static void run_local(const char *c)
{
    static char cl[FAPI_PATHLEN + FAPI_ARGLEN];
    if (FAPI_FAILED(DosGetEnv("COMSPEC", cl, FAPI_PATHLEN)) || !cl[0])
        scopy(cl, "CMD.EXE", sizeof(cl));
    scat(cl, " /C ", sizeof(cl));
    scat(cl, c, sizeof(cl));
    DosExecPgm(cl, EXEC_SYNC);
}

static void command(void)
{
    static char w[32], a1[300], a2[300];
    unsigned char t;
    unsigned char *q;
    char *p = skip(line);
    unsigned long mode;
    if (!*p)
        return;
    if (*p == '!') {
        run_local(p + 1);
        return;
    }
    p = word(p, w, sizeof(w));
    p = skip(p);
    p = word(p, a1, sizeof(a1));
    p = skip(p);
    word(p, a2, sizeof(a2));
    if (is(w, "bye", "exit") || is(w, "quit", 0)) {
        ssh_disconnect();
        DosExit(failed);
    } else if (is(w, "help", "?"))
        help();
    else if (is(w, "pwd", 0)) {
        out("Fernes Verzeichnis: ");
        out(rcwd);
        out("\r\n");
    } else if (is(w, "lpwd", 0)) {
        DosQCurDir(lname);
        out("Lokales Verzeichnis: ");
        out(lname);
        out("\r\n");
    } else if (is(w, "lcd", 0)) {
        if (FAPI_FAILED(DosChDir(a1))) {
            out("Verzeichnis nicht gefunden.\r\n");
            failed = 1;
        }
    } else if (is(w, "cd", 0)) {
        rpath(path, a1[0] ? a1 : ".");
        if (!realpath(path, a2)) {
            t = send_path(FX_STAT, a2);
            if (t == FX_ATTRS) {
                gattrs();
                if ((a_perm & 0170000UL) == 0040000UL || !a_perm)
                    scopy(rcwd, a2, sizeof(rcwd));
                else {
                    out("Kein Verzeichnis.\r\n");
                    failed = 1;
                }
            } else
                status(t, a1);
        }
    } else if (is(w, "ls", "dir")) {
        int lng = same(w, "dir");
        if (same(a1, "-l")) {
            lng = 1;
            scopy(a1, a2, sizeof(a1));
        }
        rpath(path2, a1[0] ? a1 : ".");
        list(path2, lng, 0, 0);
    } else if (is(w, "get", 0) && a1[0])
        do_get(a1, a2);
    else if (is(w, "put", 0) && a1[0])
        do_put(a1, a2);
    else if (is(w, "mget", 0) && a1[0])
        do_mget(a1);
    else if (is(w, "mput", 0) && a1[0])
        do_mput(a1);
    else if (is(w, "rm", "del") && a1[0]) {
        rpath(path, a1);
        status(send_path(FX_REMOVE, path), a1);
    } else if (is(w, "rmdir", "rd") && a1[0]) {
        rpath(path, a1);
        status(send_path(FX_RMDIR, path), a1);
    } else if (is(w, "mkdir", "md") && a1[0]) {
        rpath(path, a1);
        q = head(FX_MKDIR);
        q = ssh_putstr(q, path, slen(path));
        q = ssh_put32(q, 0);
        if (!xmit((unsigned)(q - ob)))
            status(reply(), a1);
    } else if (is(w, "rename", "ren") && a1[0] && a2[0]) {
        rpath(path, a1);
        rpath(path2, a2);
        q = head(FX_RENAME);
        q = ssh_putstr(q, path, slen(path));
        q = ssh_putstr(q, path2, slen(path2));
        if (!xmit((unsigned)(q - ob)))
            status(reply(), a1);
    } else if (is(w, "chmod", 0) && a1[0] && a2[0]) {
        for (mode = 0, p = a1; *p >= '0' && *p <= '7'; p++)
            mode = mode * 8 + (*p - '0');
        rpath(path, a2);
        q = head(FX_SETSTAT);
        q = ssh_putstr(q, path, slen(path));
        q = ssh_put32(q, 4);
        q = ssh_put32(q, mode);
        if (!xmit((unsigned)(q - ob)))
            status(reply(), a2);
    } else {
        out("Unbekannter Befehl oder Angabe fehlt (help zeigt die Befehle).\r\n");
        failed = 1;
    }
}

static void usage(void)
{
    VioWrtTTY("SFTP [-P Port] [-l Benutzer] [-4|-6] [-v] [-b Datei] [Benutzer@]Rechner\r\n", 74);
    DosExit(255);
}

void main(void)
{
    char *a, *at;
    unsigned port = 22, k, sel;
    unsigned char *q;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' && a[1] == '4' && a[2] <= ' ') ssh_fam = AF_INET;
        else if (*a == '-' && a[1] == '6' && a[2] <= ' ') ssh_fam = AF_INET6;
        else if (*a == '-' && a[1] == 'v' && a[2] <= ' ') ssh_verbose = 1;
        else if (*a == '-' && (a[1] == 'P' || a[1] == 'p') && a[2] <= ' ') {
            a = next_word(a);
            for (port = 0; *a >= '0' && *a <= '9'; a++)
                port = port * 10 + (*a - '0');
            if (!port)
                usage();
        } else if (*a == '-' && a[1] == 'l' && a[2] <= ' ') {
            a = next_word(a);
            word(a, user, sizeof(user));
        } else if (*a == '-' && a[1] == 'b' && a[2] <= ' ') {
            a = next_word(a);
            word(a, lname, sizeof(lname));
            scr = DosOpen(lname);
            if (FAPI_FAILED(scr)) {
                VioWrtTTY("Befehlsdatei nicht gefunden.\r\n", 30);
                DosExit(255);
            }
        } else if (*a == '-' || *a == '/')
            usage();
        else if (!host[0]) {
            word(a, host, sizeof(host));
            for (at = host; *at && *at != '@'; at++)
                ;
            if (*at) {
                *at = 0;
                scopy(user, host, sizeof(user));
                scopy(host, at + 1, sizeof(host));
            }
        } else
            usage();
    }
    if (!host[0])
        usage();
    if (!user[0] && (FAPI_FAILED(DosGetEnv("USER", user, sizeof(user))) || !user[0])) {
        ssh_out("Benutzer: ");
        k = 0;
        for (;;) {
            unsigned c = KbdCharIn() & 0xFF;
            if (c == '\r')
                break;
            if (c == 27 || c == 3)
                DosExit(255);
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
    }
    sel = DosAllocSeg(0);
    if (FAPI_FAILED(sel)) {
        ssh_out("Zu wenig Speicher.\r\n");
        DosExit(255);
    }
    ib = (unsigned char __far *)((unsigned long)sel << 16);
    ssh_data = on_data;
    if (ssh_connect(host, port) || ssh_login(user) || ssh_session()) {
        ssh_disconnect();
        DosExit(255);
    }
    q = ssh_putstr(ob, "sftp", 4);
    if (ssh_request("subsystem", ob, (unsigned)(q - ob), 1)) {
        ssh_out("sftp: Der Server bietet kein SFTP an.\r\n");
        ssh_disconnect();
        DosExit(255);
    }
    ob[4] = FX_INIT;
    ssh_put32(ob + 5, 3);
    if (xmit(9) || reply() != FX_VERSION) {
        ssh_out("sftp: Keine SFTP-Antwort.\r\n");
        ssh_disconnect();
        DosExit(255);
    }
    if (realpath(".", rcwd))
        scopy(rcwd, "/", sizeof(rcwd));
    out("Verbunden mit ");
    out(host);
    out(". help zeigt die Befehle.\r\n");
    for (;;) {
        out("sftp> ");
        if (get_line() < 0)
            break;
        command();
    }
    ssh_disconnect();
    DosExit(failed);
}
