/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - Redirector der DOS-Boxen: AT-DOS-Laufwerke unter DOS
 *
 * Das DOS in der Box (FreeDOS) haelt die Laufwerke ab C: fuer
 * Netzlaufwerke (BOXSHELL traegt sie in die CDS ein, Flags C000h) und
 * fragt fuer jeden Zugriff ueber INT 2Fh 11xxh nach - wie bei MSCDEX oder
 * einem Netzwerk-Redirector. BOXSHELL reicht den Aufruf mit INT E0h 10h
 * an den Monitor weiter, hier wird er mit den FAPI-Dateifunktionen des
 * Kernels erledigt (box_fapi, wie ein Programm im Task der Box). Dadurch
 * gelten Pfadaufloesung, SUBST/JOIN, Sperren und Cache wie fuer FAPI-
 * Programme, und task_end schliesst offene Dateien der Box.
 *
 * Daten des DOS: SDA (INT 21h 5D06h, Aufbau wie MS-DOS 4+ - FreeDOS haelt
 * die Offsets ein): DTA 0Ch, Dateiname 1 9Eh, Dateiname 2 11Eh, Such-
 * zustand 19Eh (21 Byte), gefundener Eintrag 1B3h, Suchattribut 24Dh,
 * erweitertes Oeffnen 2DDh-2E2h. SFT (ES:DI): Modus 02h, Attribut 04h,
 * Flags 05h (8000h Netz | Laufwerk), Zeit/Datum 0Dh/0Fh, Groesse 11h,
 * Position 15h, Name 20h; das FAPI-Handle steht in 0Bh (+1).
 *
 * Laeuft im Kontext der Box unter der Kernelsperre (BOX_LOCK) - die
 * statischen Puffer hier sind damit sicher, auch wenn ein Plattenzugriff
 * schlaeft. Speicher der Box ueber ihr eigenes Fenster RWIN_SEL(t), das
 * keine andere Box umlegt.
 */
#include "k2.h"
#include "../include/fapi_abi.h"
#include "v86int.h"

int  __far kf_box_ctl(unsigned op, unsigned a);
unsigned __far kf_box_fapi(unsigned fn, unsigned usel, unsigned a1, unsigned a2, unsigned a3);
#define FAPI(fn, sel, a, b, c)  kf_box_fapi((fn), (sel), (a), (b), (c))

#define AL  (*(unsigned char *)&f->eax)
#define AX  (*(unsigned short *)&f->eax)
#define BX  (*(unsigned short *)&f->ebx)
#define CX  (*(unsigned short *)&f->ecx)
#define DX  (*(unsigned short *)&f->edx)
#define DI  (*(unsigned short *)&f->edi)

/* DOS-Fehler */
#define E_NOFILE    2
#define E_NOPATH    3
#define E_DENIED    5
#define E_HANDLE    6
#define E_WP        19
#define E_NOTREADY  21
#define E_SHARE     32
#define E_NOMORE    18
#define E_EXISTS    80

static unsigned sda_seg[NTASK], sda_off[NTASK];
static char p1[128], p2[128], tmpl[11];
static struct fapi_dirent de;
static struct fapi_fsinfo fsi;
static struct fapi_finfo fin;
static unsigned long l32;
static char sdir[NTASK][4][80];         /* Verzeichnisse laufender Suchen */
static unsigned char sdir_next[NTASK];
static char slast[NTASK][4][11];        /* zuletzt gefundener Name       */

void __far redir_sda(int t, unsigned seg, unsigned off)
{
    sda_seg[t] = seg;
    sda_off[t] = off;
}

/* PSP des laufenden DOS-Programms der Box (SDA+10h, fuer DPMI) */
unsigned __far redir_psp(int t);

/* ---- Speicher der Box ----------------------------------------------------- */

static unsigned char __far *bp(int t, unsigned seg, unsigned off)
{
    struct desc *d = &gdt[RWIN_IDX(t)];
    unsigned long b = (unsigned long)seg << 4;
    d->limit = 0xFFFF;
    d->base_lo = (unsigned short)b;
    d->base_hi = (unsigned char)(b >> 16);
    d->access = ACC_KDATA;
    d->rsv = 0;
    return MK_FP(RWIN_SEL(t), off);
}

static unsigned r16(unsigned char __far *p) { return p[0] | (p[1] << 8); }
static void w16(unsigned char __far *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static unsigned long r32(unsigned char __far *p) { return r16(p) | ((unsigned long)r16(p + 2) << 16); }
static void w32(unsigned char __far *p, unsigned long v) { w16(p, (unsigned)v); w16(p + 2, (unsigned)(v >> 16)); }

static unsigned char __far *sda(int t, unsigned x)
{
    return bp(t, sda_seg[t], sda_off[t] + x);
}

unsigned __far redir_psp(int t)
{
    unsigned char __far *p;
    if (!sda_seg[t])
        return 0;
    p = sda(t, 0x10);
    return p[0] | (p[1] << 8);
}

static void get_fn(int t, unsigned x, char *d)
{
    unsigned char __far *s = sda(t, x);
    unsigned i;
    for (i = 0; i < 127 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* ---- Namen ------------------------------------------------------------------ */

static unsigned up(unsigned c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* "NAME.EXT" -> FCB-Form "NAME    EXT" (Platzhalter * -> ?) */
static void to_fcb(const char *n, char *o)
{
    unsigned i = 0, k;
    for (k = 0; k < 11; k++)
        o[k] = ' ';
    if (n[0] == '.') {                      /* "." und ".."              */
        o[0] = '.';
        if (n[1] == '.')
            o[1] = '.';
        return;
    }
    for (k = 0; n[i] && n[i] != '.'; i++)
        if (n[i] == '*')
            while (k < 8)
                o[k++] = '?';
        else if (k < 8)
            o[k++] = (char)up(n[i]);
    if (n[i] == '.')
        i++;
    for (k = 8; n[i]; i++)
        if (n[i] == '*')
            while (k < 11)
                o[k++] = '?';
        else if (k < 11)
            o[k++] = (char)up(n[i]);
}

static int fcb_match(const char *t, const char *n)
{
    unsigned k;
    for (k = 0; k < 11; k++)
        if (t[k] != '?' && t[k] != n[k])
            return 0;
    return 1;
}

/* p -> Verzeichnis in d ("C:\DIR" bzw. "C:\"), -> Zeiger auf den Namen */
static const char *split(const char *p, char *d)
{
    unsigned i, last = 2;
    for (i = 0; p[i]; i++)
        if (p[i] == '\\')
            last = i;
    for (i = 0; i < last && i < 78; i++)
        d[i] = p[i];
    if (last <= 2)
        d[i++] = '\\';
    d[i] = 0;
    return p + last + 1;
}

static int has_wild(const char *p)
{
    for (; *p; p++)
        if (*p == '?' || *p == '*')
            return 1;
    return 0;
}

static unsigned errmap(unsigned r, unsigned def)
{
    switch (r) {
    case FAPI_ERR_WP:       return E_WP;
    case FAPI_ERR_DENIED:   return E_DENIED;
    case FAPI_ERR_EXIST:    return E_EXISTS;
    case FAPI_ERR_NOTREADY: return E_NOTREADY;
    case FAPI_ERR_INUSE:    return E_SHARE;
    case FAPI_ERR_FULL:     return E_DENIED;
    }
    return def;
}

#define FAILED(r)   ((r) >= FAPI_ERR_FIRST)

/* Eintrag zu Pfad p in de suchen (Stamm: Verzeichnis) -> 0 gefunden */
static char st_dir[80], st_fcb[11];             /* Kontext (Phase 6) */
#define dir st_dir
#define fcb st_fcb
static int stat_path(const char *p)
{
    const char *n = split(p, dir);
    unsigned i, k;
    if (!*n) {                              /* "C:\"                     */
        de.attr = 0x10;
        de.size = 0;
        de.date = de.time = 0;
        return 0;
    }
    to_fcb(n, tmpl);
    for (i = 0; ; i++) {
        if (FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, i, (unsigned)dir)))
            return -1;
        to_fcb(de.name, fcb);
        for (k = 0; k < 11 && fcb[k] == tmpl[k]; k++)
            ;
        if (k == 11)
            return 0;
    }
}
#undef dir
#undef fcb

/* ---- SFT -------------------------------------------------------------------- */

static char sf_dir[80];                         /* Kontext */
#define dir sf_dir
static void sft_fill(int t, unsigned es, unsigned di, unsigned h, unsigned mode,
                     const char *path)
{
    unsigned char __far *s = bp(t, es, di);
    const char *n;
    unsigned k;
    w16(s + 0x02, mode);
    s[0x04] = de.attr;
    w16(s + 0x05, 0x8040 | (up(path[0]) - 'A'));
    w32(s + 0x07, 0);
    w16(s + 0x0B, h + 1);
    w16(s + 0x0D, de.time);
    w16(s + 0x0F, de.date);
    w32(s + 0x11, de.size);
    w32(s + 0x15, 0);
    n = split(path, dir);
    to_fcb(n, tmpl);
    s = bp(t, es, di + 0x20);
    for (k = 0; k < 11; k++)
        s[k] = tmpl[k];
}
#undef dir

/* Daten zwischen Datei und Box (DTA) in Stuecken (FAPI hoechstens 7FFFh) */
static unsigned xfer(int t, unsigned fn, unsigned h, unsigned long lin, unsigned n,
                     unsigned *err)
{
    unsigned done = 0, k, r;
    *err = 0;
    while (done < n) {
        k = n - done > 0x4000 ? 0x4000 : n - done;
        bp(t, (unsigned)(lin >> 4), 0);
        r = FAPI(fn, RWIN_SEL(t), h, (unsigned)lin & 15, k);
        if (FAILED(r)) {
            *err = errmap(r, E_DENIED);
            break;
        }
        done += r;
        lin += r;
        if (r < k)
            break;
    }
    return done;
}

/* ---- Suche -------------------------------------------------------------------- */

/* naechsten passenden Eintrag ab Suchzustand sdb (in der Box) */
static char fn_fcb[11], fn_tp[11];              /* Kontext */
static struct fapi_label fn_lb;
#define fcb fn_fcb
#define tp fn_tp
#define lb fn_lb
static int find_next(int t, unsigned char __far *sdb_unused, unsigned seg, unsigned off)
{
    unsigned char __far *s = bp(t, seg, off);
    unsigned n = r16(s + 13), slot = s[15] & 3, k;
    unsigned char mask = s[12], a;
    (void)sdb_unused;
    if (n == 0xFFFF)
        return E_NOMORE;                    /* nach der Bezeichnung      */
    if (mask == 0x08) {                     /* nur Datentraegerbezeichnung */
        if (FAILED(FAPI(FAPI_LABEL, KDATA, s[0] & 0x1F, (unsigned)&lb, 0)) || !lb.label[0])
            return E_NOMORE;
        for (k = 0; k < 11; k++)
            fcb[k] = ' ';
        for (k = 0, a = 0; lb.label[k] && a < 11; k++)
            if (lb.label[k] != '.')
                fcb[a++] = lb.label[k];
        de.attr = 0x08;
        de.time = de.date = 0;
        de.size = 0;
        n = 0xFFFE;
        goto found;
    }
    for (k = 0; k < 11; k++)
        tp[k] = s[1 + k];
    /* Eintrag n-1 inzwischen geloescht (DEL *.* loescht zwischen den
       Suchschritten)? Dann sind alle folgenden eins nach vorn gerueckt. */
    if (n && n < 0xFFFE &&
        !FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, n - 1, (unsigned)sdir[t][slot]))) {
        to_fcb(de.name, fcb);
        for (k = 0; k < 11 && fcb[k] == slast[t][slot][k]; k++)
            ;
        if (k < 11)
            n--;
    }
    for (;; n++) {
        if (FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, n, (unsigned)sdir[t][slot])))
            return E_NOMORE;
        a = de.attr;
        if ((a & 0x08) && mask != 0x08)
            continue;                       /* Datentraegerbezeichnung   */
        if (a & ~mask & 0x16)
            continue;                       /* versteckt/System/Verz.    */
        to_fcb(de.name, fcb);
        if (!fcb_match(tp, fcb))
            continue;
        break;
    }
found:
    for (k = 0; k < 11; k++)
        slast[t][slot][k] = fcb[k];
    s = bp(t, seg, off);
    w16(s + 13, n + 1);
    s = sda(t, 0x1B3);                      /* gefundener Eintrag        */
    for (k = 0; k < 11; k++)
        s[k] = fcb[k];
    s[11] = de.attr;
    for (k = 12; k < 22; k++)
        s[k] = 0;
    w16(s + 22, de.time);
    w16(s + 24, de.date);
    w16(s + 26, 0);
    w32(s + 28, de.size);
    return 0;
}
#undef fcb
#undef tp
#undef lb

static int find_first(int t)
{
    const char *n;
    unsigned char __far *s;
    unsigned slot = sdir_next[t]++ & 3, k;
    get_fn(t, 0x9E, p1);
    n = split(p1, sdir[t][slot]);
    to_fcb(*n ? n : "????????.???", tmpl);
    s = sda(t, 0x19E);
    s[0] = (unsigned char)(0x80 | (up(p1[0]) - 'A'));
    for (k = 0; k < 11; k++)
        s[1 + k] = tmpl[k];
    s[12] = *sda(t, 0x24D);
    w16(s + 13, 0);
    s[15] = (unsigned char)slot;
    for (k = 16; k < 21; k++)
        s[k] = 0;
    k = find_next(t, 0, sda_seg[t], sda_off[t] + 0x19E);
    return k == E_NOMORE ? E_NOFILE : k;
}

/* ---- Verteiler ---------------------------------------------------------------- */

static char op_dir[80], op_fcb[11], op_one[96];  /* Kontext */
#define dir op_dir
#define fcb op_fcb
#define one op_one
static int op(struct v86_frame *f, int t, unsigned fn)
{
    unsigned char __far *s;
    unsigned es = (unsigned)f->r.es, di = DI, h, r, k, arg, cnt, err;
    unsigned long pos, size, lin;

    /* Argument, das DOS vor INT 2Fh auf den Stack legte: [SP] AX von
       BOXSHELL, [SP+2] IP, CS, FLAGS, dann das Argument */
    arg = r16(bp(t, (unsigned)f->r.ss, (unsigned)f->r.esp + 8));

    switch (fn) {
    case 0x01:                                  /* RMDIR                 */
    case 0x03:                                  /* MKDIR                 */
        get_fn(t, 0x9E, p1);
        r = FAPI(fn == 1 ? FAPI_RMDIR : FAPI_MKDIR, KDATA, (unsigned)p1, 0, 0);
        return FAILED(r) ? errmap(r, fn == 1 ? E_DENIED : E_NOPATH) : 0;
    case 0x05:                                  /* CHDIR: gibt es das?   */
        get_fn(t, 0x9E, p1);
        if (stat_path(p1) || !(de.attr & 0x10))
            return E_NOPATH;
        return 0;
    case 0x06:                                  /* CLOSE                 */
        s = bp(t, es, di);
        cnt = r16(s);
        h = r16(s + 0x0B) - 1;
        if (cnt)
            w16(s, --cnt);
        if (cnt == 0 && h != 0xFFFF) {
            FAPI(FAPI_CLOSE, KDATA, h, 0, 0);
            w16(bp(t, es, di + 0x0B), 0);
        }
        return 0;
    case 0x07:                                  /* COMMIT                */
    case 0x0A: case 0x0B:                       /* LOCK/UNLOCK           */
    case 0x1D: case 0x22:                       /* alle zu, Prozessende  */
        return 0;
    case 0x08:                                  /* READ                  */
    case 0x09:                                  /* WRITE                 */
        s = bp(t, es, di);
        h = r16(s + 0x0B) - 1;
        pos = r32(s + 0x15);
        size = r32(s + 0x11);
        if (h == 0xFFFF)
            return E_HANDLE;
        l32 = pos;
        if (FAILED(FAPI(FAPI_SEEK, KDATA, h, (unsigned)&l32, 0)))
            return E_DENIED;
        cnt = CX;
        err = 0;
        if (cnt == 0 && fn == 9) {              /* Groesse setzen (DOS): */
            l32 = pos;                          /* kuerzen/verlaengern   */
            r = FAPI(FAPI_NEWSIZE, KDATA, h, (unsigned)&l32, 0);
            if (FAILED(r))
                return errmap(r, E_DENIED);
            w32(bp(t, es, di + 0x11), pos);
            CX = 0;
            return 0;
        }
        s = sda(t, 0x0C);
        lin = ((unsigned long)r16(s + 2) << 4) + r16(s);
        k = cnt ? xfer(t, fn == 8 ? FAPI_READ : FAPI_WRITE, h, lin, cnt, &err) : 0;
        if (k == 0 && err)
            return err;
        pos += k;
        s = bp(t, es, di);
        w32(s + 0x15, pos);
        if (fn == 9 && pos > size)
            w32(s + 0x11, pos);
        CX = k;
        return 0;
    case 0x0C: {                                /* freier Platz          */
        unsigned spc = 1, sh = 9;
        unsigned long tot, fr;
        s = bp(t, es, di);
        k = up(s[0]) - 'A';
        if (FAILED(FAPI(FAPI_QFSINFO, KDATA, k, (unsigned)&fsi, 0)) ||
            FAILED(FAPI(FAPI_FREESPACE, KDATA, k, (unsigned)&l32, 0)))
            return E_NOTREADY;
        tot = fsi.bsize == 2048 ? fsi.sectors << 2 : fsi.sectors;
        while ((tot >> (sh - 9)) > 0xFFF0UL && spc < 128) {
            spc <<= 1;
            sh++;
        }
        fr = l32 >> sh;                         /* Bytes -> Cluster       */
        AX = spc;
        BX = (unsigned)(tot >> (sh - 9));
        CX = 512;
        DX = fr > 0xFFF0UL ? 0xFFF0 : (unsigned)fr;
        return 0;
    }
    case 0x0E:                                  /* Attribut setzen       */
        get_fn(t, 0x9E, p1);
        fin.mask = 1;
        fin.attr = arg & 0x27;
        r = FAPI(FAPI_SETFINFO, KDATA, (unsigned)p1, (unsigned)&fin, 0);
        return FAILED(r) ? errmap(r, E_NOFILE) : 0;
    case 0x0F:                                  /* Attribut holen        */
        get_fn(t, 0x9E, p1);
        if (stat_path(p1))
            return E_NOFILE;
        AX = de.attr;
        BX = (unsigned)(de.size >> 16);
        DI = (unsigned)de.size;
        CX = de.time;
        DX = de.date;
        return 0;
    case 0x11:                                  /* RENAME                */
        get_fn(t, 0x9E, p1);
        get_fn(t, 0x11E, p2);
        r = FAPI(FAPI_MOVE, KDATA, (unsigned)p1, (unsigned)p2, 0);
        return FAILED(r) ? errmap(r, E_NOFILE) : 0;
    case 0x13: {                                /* DELETE (auch *.*)     */
        const char *n;
        unsigned i = 0, hit = 0, j, l;
        get_fn(t, 0x9E, p1);
        if (!has_wild(p1)) {
            r = FAPI(FAPI_DELETE, KDATA, (unsigned)p1, 0, 0);
            return FAILED(r) ? errmap(r, E_NOFILE) : 0;
        }
        n = split(p1, dir);
        to_fcb(n, tmpl);
        while (!FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, i, (unsigned)dir))) {
            to_fcb(de.name, fcb);
            if ((de.attr & 0x1F) || !fcb_match(tmpl, fcb)) {
                i++;
                continue;
            }
            for (l = 0; dir[l]; l++)
                one[l] = dir[l];
            if (one[l - 1] != '\\')
                one[l++] = '\\';
            for (j = 0; de.name[j] && l < 95; j++)
                one[l++] = de.name[j];
            one[l] = 0;
            if (FAILED(FAPI(FAPI_DELETE, KDATA, (unsigned)one, 0, 0)))
                i++;                            /* bleibt stehen: weiter */
            else
                hit++;
        }
        return hit ? 0 : E_NOFILE;
    }
    case 0x16:                                  /* OPEN                  */
    case 0x17:                                  /* CREATE (kuerzen)      */
    case 0x2E: {                                /* erweitert             */
        unsigned mode = arg & 0xFF, act = 0, exists;
        get_fn(t, 0x9E, p1);
        exists = stat_path(p1) == 0;
        if (exists && (de.attr & 0x18))
            return E_DENIED;                    /* Verzeichnis/Bezeichnung */
        if (fn == 0x2E) {
            unsigned a = r16(sda(t, 0x2DD));
            mode = r16(sda(t, 0x2E1)) & 0xFF;
            if (exists && (a & 0x0F) == 0)
                return E_EXISTS;
            if (!exists && !(a & 0xF0))
                return E_NOFILE;
            act = !exists ? 2 : (a & 0x0F) == 2 ? 3 : 1;
        } else
            act = fn == 0x17 ? (exists ? 3 : 2) : 1;
        if (act == 1 && !exists)
            return E_NOFILE;
        if (act == 1)                           /* vorhanden: lesen oder  */
            h = FAPI(FAPI_OPEN, KDATA, (unsigned)p1, (mode & 3) ? OPEN_RW : OPEN_READ, 0);
        else                                    /* lesen+schreiben        */
            h = FAPI(FAPI_CREATE, KDATA, (unsigned)p1, 0, 0);
        if (FAILED(h))
            return errmap(h, act == 1 ? E_NOFILE : E_DENIED);
        if (act != 1) {
            de.attr = 0x20;
            de.size = 0;
            stat_path(p1);                      /* Datum/Uhrzeit         */
            de.size = 0;
        }
        if (fn == 0x17)
            mode = 2;                           /* arg ist das Attribut;  */
        sft_fill(t, es, di, h, mode, p1);       /* DOS prueft den Modus   */
        if (fn == 0x2E)
            CX = act;
        return 0;
    }
    case 0x1B:                                  /* FINDFIRST             */
        return find_first(t);
    case 0x1C:                                  /* FINDNEXT (ES:DI)      */
        return find_next(t, 0, es, di);
    case 0x21: {                                /* SEEK vom Ende         */
        unsigned long off = ((unsigned long)CX << 16) | DX;
        s = bp(t, es, di);
        pos = r32(s + 0x11) + off;
        w32(s + 0x15, pos);
        DX = (unsigned)(pos >> 16);
        AX = (unsigned)pos;
        return 0;
    }
    }
    return -1;                                  /* nicht unser           */
}
#undef dir
#undef fcb
#undef one

int __far redir(struct v86_frame *f, int t)
{
    int r;
    if (!sda_seg[t])
        return -1;
    kf_box_ctl(BOX_LOCK, 1);
    r = op(f, t, AL);
    kf_box_ctl(BOX_LOCK, 0);
    return r;
}

/* Kontext je Task (Phase 6, kernel.c ctx_take) */
const struct ctxblk redir_ctx[] = {
    { p1, sizeof(p1) }, { p2, sizeof(p2) }, { tmpl, sizeof(tmpl) }, { &de, sizeof(de) },
    { &fsi, sizeof(fsi) }, { &fin, sizeof(fin) }, { &l32, sizeof(l32) },
    { st_dir, sizeof(st_dir) }, { st_fcb, sizeof(st_fcb) }, { sf_dir, sizeof(sf_dir) },
    { fn_fcb, sizeof(fn_fcb) }, { fn_tp, sizeof(fn_tp) }, { &fn_lb, sizeof(fn_lb) },
    { op_dir, sizeof(op_dir) }, { op_fcb, sizeof(op_fcb) }, { op_one, sizeof(op_one) }
};
const unsigned redir_nctx = sizeof(redir_ctx) / sizeof(redir_ctx[0]);
