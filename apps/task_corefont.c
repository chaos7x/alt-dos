/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS - COREFONT: Installer fuer die Schriftpakete "Core fonts for the
 * Web" von Microsoft, nach dem Vorbild des Debian-Pakets
 * ttf-mscorefonts-installer (Wunsch des Nutzers, 25.11.2026). Die
 * Schriften liegen nicht bei AT-DOS: COREFONT laedt die unveraenderten
 * Original-Pakete (arial32.exe usw.) von SourceForge, prueft sie per
 * SHA-256 (Liste wie Debian, cabfiles.sha256sums), entpackt die TrueType-
 * Dateien aus dem Cabinet im Paket (LZX, apps/lzx.c) und legt sie in
 * X:\DOS\FONTS ab. Die Lizenz (EULA) gilt zwischen Microsoft und dem
 * Nutzer; COREFONT zeigt sie an und fragt.
 *
 * COREFONT [/Y] [/S:Verzeichnis] [/K] [/Z:Ziel]
 *   /Y   Lizenz ohne Rueckfrage annehmen
 *   /S:  Pakete aus diesem Verzeichnis nehmen statt herunterzuladen
 *   /K   heruntergeladene Pakete behalten (sonst geloescht)
 *   /Z:  Zielverzeichnis statt X:\DOS\FONTS
 * Herunterladen ueber WGET.EXE (HTTPS, Zertifikat gegen CACERT.PEM).
 */
#include "fapi.h"
#include "util.h"
#include "crypto.h"
#include "lzx.h"

typedef unsigned short WORD;
#define MK_FP(s, o) ((void __far *)(((unsigned long)(s) << 16) | (unsigned)(o)))

static const struct pk { const char *name; const char *sha; } pkg[] = {
    { "andale32.exe", "0524fe42951adc3a7eb870e32f0920313c71f170c859b5f770d82b4ee111e970" },
    { "arial32.exe",  "85297a4d146e9c87ac6f74822734bdee5f4b2a722d7eaa584b7f2cbf76f478f6" },
    { "arialb32.exe", "a425f0ffb6a1a5ede5b979ed6177f4f4f4fdef6ae7c302a7b7720ef332fec0a8" },
    { "comic32.exe",  "9c6df3feefde26d4e41d4a4fe5db2a89f9123a772594d7f59afd062625cd204e" },
    { "courie32.exe", "bb511d861655dde879ae552eb86b134d6fae67cb58502e6ff73ec5d9151f3384" },
    { "georgi32.exe", "2c2c7dcda6606ea5cf08918fb7cd3f3359e9e84338dc690013f20cd42e930301" },
    { "impact32.exe", "6061ef3b7401d9642f5dfdb5f2b376aa14663f6275e60a51207ad4facf2fccfb" },
    { "times32.exe",  "db56595ec6ef5d3de5c24994f001f03b2a13e37cee27bc25c58f6f43e8f807ab" },
    { "trebuc32.exe", "5a690d9bb8510be1b8b4fe49f1f2319651fe51bbe54775ddddd8ef0bd07fdac9" },
    { "verdan32.exe", "c1cb61255e363166794e47664e2f21af8e3a26cb6346eb8d2ae2fa85dd5aad96" },
    { "webdin32.exe", "64595b5abc1080fba8610c5c34fab5863408e806aafe84653ca8575bed17d75a" },
};
#define NPKG (sizeof(pkg) / sizeof(pkg[0]))

static const char *mirror[] = {     /* wie Debian (update-ms-fonts)      */
    "https://downloads.sourceforge.net/corefonts/",
    "https://jaist.dl.sourceforge.net/sourceforge/corefonts/",
    "https://netcologne.dl.sourceforge.net/sourceforge/corefonts/",
    "https://nchc.dl.sourceforge.net/sourceforge/corefonts/",
    "https://ufpr.dl.sourceforge.net/sourceforge/corefonts/",
};
#define NMIR (sizeof(mirror) / sizeof(mirror[0]))

static char args[FAPI_ARGLEN];
static char src[FAPI_PATHLEN], dst[FAPI_PATHLEN], tmp[FAPI_PATHLEN];
static char path[FAPI_PATHLEN], cl[400], wget[FAPI_PATHLEN];
static unsigned char buf[4096];
static int keep, yes;

static void out(const char *s) { puts(s); }

static void die(const char *s, int rc)
{
    out("COREFONT: ");
    out(s);
    out("\r\n");
    DosExit(rc);
}

/* ---- Programm suchen (wie NET: \DOS des COMSPEC-Laufwerks) ---- */
static void find_wget(void)
{
    WORD h;
    if (FAPI_FAILED(DosGetEnv("COMSPEC", wget, sizeof(wget))) || !wget[0])
        scopy(wget, "C:", sizeof(wget));
    wget[2] = 0;
    scat(wget, "\\DOS\\WGET.EXE", sizeof(wget));
    h = DosOpen(wget);
    if (FAPI_FAILED(h))
        scopy(wget, "WGET.EXE", sizeof(wget));
    else
        DosClose(h);
}

/* ---- SHA-256 einer Datei ------------------------------------------ */
static int sha_ok(const char *file, const char *want)
{
    static struct sha256 c;
    static u8 dig[32];
    WORD h, n, i;
    h = DosOpen(file);
    if (FAPI_FAILED(h))
        return 0;
    sha256_init(&c);
    while ((n = DosRead(h, buf, sizeof(buf))) > 0 && !FAPI_FAILED(n))
        sha256_add(&c, buf, n);
    DosClose(h);
    sha256_end(&c, dig);
    for (i = 0; i < 32; i++)
        if ("0123456789abcdef"[dig[i] >> 4] != want[2 * i] ||
            "0123456789abcdef"[dig[i] & 15] != want[2 * i + 1])
            return 0;
    return 1;
}

/* ---- Cabinet im Paket lesen -------------------------------------- */
static WORD fh;                         /* Paketdatei                  */
static unsigned long fpos, cab;         /* Lesestelle, Anfang des CAB  */
static WORD bpos, blen;                 /* Lesepuffer                  */

static int rd_at(unsigned long p, void *d, WORD n)
{
    static unsigned long q;
    q = p;
    if (FAPI_FAILED(DosSeek(fh, &q)))
        return 1;
    bpos = blen = 0;
    fpos = p + n;
    return DosRead(fh, d, n) != n;
}
static int rbyte(void)
{
    if (bpos >= blen) {
        static unsigned long q;
        q = fpos;
        DosSeek(fh, &q);
        blen = DosRead(fh, buf, sizeof(buf));
        if (FAPI_FAILED(blen) || !blen) {
            blen = 0;
            return -1;
        }
        fpos += blen;
        bpos = 0;
    }
    return buf[bpos++];
}
static WORD w16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long w32(const unsigned char *p)
{
    return w16(p) | ((unsigned long)w16(p + 2) << 16);
}

/* CAB-Kopf suchen: "MSCF", reserviert 0, Version 1.3 */
static int find_cab(void)
{
    static unsigned char h[32];
    static unsigned long q;
    unsigned long p = 0;
    WORD n, i;
    for (;;) {
        q = p;
        if (FAPI_FAILED(DosSeek(fh, &q)))
            return 1;
        n = DosRead(fh, buf, sizeof(buf));
        if (FAPI_FAILED(n) || n < 32)
            return 1;
        for (i = 0; i + 4 <= n; i++)
            if (buf[i] == 'M' && buf[i + 1] == 'S' && buf[i + 2] == 'C' && buf[i + 3] == 'F')
                break;
        if (i + 4 > n) {
            p += n - 3;
            continue;
        }
        if (rd_at(p + i, h, 32))
            return 1;
        if (!w32(h + 4) && h[24] == 3 && h[25] == 1) {
            cab = p + i;
            return 0;
        }
        p += i + 1;                     /* Fehltreffer: dahinter weiter */
    }
}

/* Dateien des Ordners (nur einer: die Pakete haben einen) */
#define NF 12
static struct cf { char name[14]; unsigned long size, off; int ttf; } f[NF];
static int nf, cur = -1;
static WORD oh = 0xFFFF;
static unsigned long opos;              /* Stelle im entpackten Strom  */
static unsigned nblk, nttf;
static unsigned long blkpos;            /* naechster CFDATA-Kopf       */
static long blkleft;

int lzx_byte(void)
{
    while (blkleft <= 0) {
        static unsigned char hd[8];
        if (!nblk)
            return -1;
        nblk--;
        if (rd_at(blkpos, hd, 8))
            return -1;
        blkleft = w16(hd + 4);
        blkpos += 8 + blkleft;
    }
    blkleft--;
    return rbyte();
}

static void put_file(unsigned char *b, unsigned n)
{
    while (n) {
        unsigned k;
        unsigned long end;
        while (cur < nf && (cur < 0 || opos >= f[cur].off + f[cur].size)) {
            if (oh != 0xFFFF) {
                DosClose(oh);
                oh = 0xFFFF;
            }
            cur++;
            if (cur < nf && f[cur].ttf) {
                join(path, dst, f[cur].name, sizeof(path));
                oh = DosCreate(path);
                if (FAPI_FAILED(oh)) {
                    out("  kann nicht anlegen: ");
                    out(path);
                    out("\r\n");
                    oh = 0xFFFF;
                } else {
                    out("  ");
                    out(f[cur].name);
                    out("\r\n");
                    nttf++;
                }
            }
        }
        if (cur >= nf)
            return;
        end = f[cur].off + f[cur].size;
        k = (end - opos) < n ? (unsigned)(end - opos) : n;
        if (oh != 0xFFFF && opos >= f[cur].off)
            DosWrite(oh, b, k);
        b += k;
        n -= k;
        opos += k;
    }
}

void lzx_emit(unsigned char *b, unsigned n)
{
    unsigned k;
    while (n) {                         /* DosWrite hoechstens 7FFFh    */
        k = n > 0x4000 ? 0x4000 : n;
        put_file(b, k);
        b += k;
        n -= k;
    }
}

/* Fenster in Segmenten zu 64 KB */
#define NSEG 32
static WORD wseg[NSEG];
unsigned char lzx_wget(unsigned long p)
{
    return *(unsigned char __far *)MK_FP(wseg[(unsigned)(p >> 16)], (unsigned)p);
}
void lzx_wput(unsigned long p, unsigned char b)
{
    *(unsigned char __far *)MK_FP(wseg[(unsigned)(p >> 16)], (unsigned)p) = b;
}

static int extract(const char *file)
{
    static unsigned char h[64];
    unsigned long coff, total = 0;
    WORD nfi, typ, i, k, ns, p, rc;
    fh = DosOpen(file);
    if (FAPI_FAILED(fh))
        return 1;
    rc = 2;
    if (find_cab() || rd_at(cab, h, 44))
        goto out;
    coff = w32(h + 16);
    nfi = w16(h + 28);
    if (w16(h + 26) != 1 || (w16(h + 30) & 4))
        goto out;                       /* ein Ordner, ohne Reserve    */
    blkpos = cab + w32(h + 36);
    nblk = w16(h + 40);
    typ = w16(h + 42);
    if ((typ & 15) != 3)
        goto out;                       /* nur LZX                     */
    nf = 0;
    p = 0;
    for (i = 0; i < nfi; i++) {
        static unsigned char e[16 + 64];
        if (rd_at(cab + coff + p, e, sizeof(e)))
            goto out;
        if (nf < NF) {
            f[nf].size = w32(e);
            f[nf].off = w32(e + 4);
            for (k = 0; k < 13 && e[16 + k]; k++)
                f[nf].name[k] = e[16 + k];
            f[nf].name[k] = 0;
            f[nf].ttf = k > 4 && up(f[nf].name[k - 3]) == 'T' &&
                        up(f[nf].name[k - 2]) == 'T' && up(f[nf].name[k - 1]) == 'F';
            if (f[nf].off + f[nf].size > total)
                total = f[nf].off + f[nf].size;
            nf++;
        }
        for (k = 16; k < sizeof(e) && e[k]; k++)
            ;
        p += k + 1;
    }
    ns = (unsigned)((total + 0xFFFFUL) >> 16);
    if (ns > NSEG)
        goto out;
    for (i = 0; i < ns; i++) {
        wseg[i] = DosAllocSeg(0);
        if (FAPI_FAILED(wseg[i])) {
            out("  zu wenig Speicher\r\n");
            while (i--)
                DosFreeSeg(wseg[i]);
            rc = 3;
            goto out;
        }
    }
    cur = -1;
    opos = 0;
    blkleft = 0;
    oh = 0xFFFF;
    rc = lzx_decode((typ >> 8) & 0x1F, total) ? 4 : 0;
    put_file(buf, 0);                   /* letzte Datei schliessen     */
    if (oh != 0xFFFF) {
        DosClose(oh);
        oh = 0xFFFF;
    }
    for (i = 0; i < ns; i++)
        DosFreeSeg(wseg[i]);
out:
    DosClose(fh);
    return rc;
}

static int opt(const char *a, char c, char *val, unsigned max)
{
    const char *p;
    for (p = a; *p; p++)
        if (*p == '/' && up(p[1]) == c) {
            if (val) {
                if (p[2] != ':')
                    return 0;
                p += 3;
                while (*p && *p != ' ' && max > 1) {
                    *val++ = *p++;
                    max--;
                }
                *val = 0;
            }
            return 1;
        }
    return 0;
}

static const char eula[] =
    "Die Schriften (Andale Mono, Arial, Arial Black, Comic Sans MS, Courier\r\n"
    "New, Georgia, Impact, Times New Roman, Trebuchet MS, Verdana, Webdings)\r\n"
    "gehoeren Microsoft und liegen AT-DOS nicht bei. COREFONT laedt die\r\n"
    "unveraenderten Pakete von SourceForge (Projekt corefonts) und\r\n"
    "entpackt sie. Es gilt der Lizenzvertrag von Microsoft fuer die\r\n"
    "\"Core fonts for the Web\" (in jedem Paket, Kurzfassung: Nutzung\r\n"
    "erlaubt, Weitergabe nur unveraendert, keine Gewaehr).\r\n\r\n"
    "Lizenz annehmen und installieren (J/N)? ";

void main(void)
{
    unsigned i, m, ok = 0, bad = 0;
    DosGetArgs(args);
    if (opt(args, '?', 0, 0)) {
        out("COREFONT [/Y] [/S:Verzeichnis] [/K] [/Z:Ziel]\r\n"
            "Installiert die \"Core fonts for the Web\" von Microsoft\r\n"
            "(Download von SourceForge, Pruefung per SHA-256) nach X:\\DOS\\FONTS.\r\n"
            "  /Y  Lizenz ohne Rueckfrage annehmen\r\n"
            "  /S: Pakete (arial32.exe usw.) aus diesem Verzeichnis nehmen\r\n"
            "  /K  heruntergeladene Pakete behalten\r\n"
            "  /Z: anderes Zielverzeichnis\r\n");
        DosExit(0);
    }
    yes = opt(args, 'Y', 0, 0);
    keep = opt(args, 'K', 0, 0);
    if (!opt(args, 'S', src, sizeof(src)))
        src[0] = 0;
    if (!opt(args, 'Z', dst, sizeof(dst))) {
        if (FAPI_FAILED(DosGetEnv("COMSPEC", dst, sizeof(dst))) || !dst[0])
            scopy(dst, "C:", sizeof(dst));
        dst[2] = 0;
        scat(dst, "\\DOS\\FONTS", sizeof(dst));
    }
    if (!src[0]) {
        if (FAPI_FAILED(DosGetEnv("TEMP", tmp, sizeof(tmp))) || !tmp[0])
            scopy(tmp, "C:\\TEMP", sizeof(tmp));
    }
    out(eula);
    if (yes)
        out("J\r\n");
    else if (!ask_yn()) {
        out("Abgebrochen.\r\n");
        DosExit(1);
    }
    DosMkDir(dst);
    if (!is_dir(dst))
        die("Zielverzeichnis fehlt und laesst sich nicht anlegen.", 2);
    find_wget();
    for (i = 0; i < NPKG; i++) {
        out(pkg[i].name);
        out(": ");
        if (src[0]) {
            join(path, src, pkg[i].name, sizeof(path));
        } else {
            join(path, tmp, pkg[i].name, sizeof(path));
            if (!sha_ok(path, pkg[i].sha)) {
                for (m = 0; m < NMIR; m++) {
                    out(m ? "neuer Versuch ueber einen Spiegel ... " : "laden ... ");
                    scopy(cl, wget, sizeof(cl));
                    scat(cl, " -q -O ", sizeof(cl));
                    scat(cl, path, sizeof(cl));
                    scat(cl, " ", sizeof(cl));
                    scat(cl, mirror[m], sizeof(cl));
                    scat(cl, pkg[i].name, sizeof(cl));
                    if (!DosExecPgm(cl, EXEC_SYNC) && !DosQChildRC() &&
                        sha_ok(path, pkg[i].sha))
                        break;
                    DosDelete(path);
                }
                if (m == NMIR) {
                    out("nicht geladen\r\n");
                    bad++;
                    continue;
                }
            }
        }
        if (!sha_ok(path, pkg[i].sha)) {
            out("Pruefsumme falsch oder Datei fehlt\r\n");
            bad++;
            continue;
        }
        out("geprueft\r\n");
        nttf = 0;
        m = extract(path);
        if (m) {
            out(m == 3 ? "  zu wenig Speicher\r\n" : "  Paket nicht lesbar\r\n");
            bad++;
        } else
            ok += nttf;
        if (!src[0] && !keep)
            DosDelete(path);
    }
    out("\r\n");
    put_num(ok, 0, 0);
    out(" Schriften in ");
    out(dst);
    out(bad ? " installiert, mit Fehlern.\r\n" : " installiert.\r\n");
    DosExit(bad ? 1 : 0);
}
