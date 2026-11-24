/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - lange Dateinamen wie Windows 95 (INT 21h AX = 71xxh) und
 * 440Dh 0871h (erster Cluster), 22.11.2026
 *
 * Der Box-Kern arbeitet intern weiter mit kurzen Pfaden (CDS, SFT); ein
 * Pfad mit langen Teilen wird ueber DEM_LPATH in die kurze und lange Form
 * aufgeloest, gesucht wird mit DEM_LFIND (include/dem.h). Neue Dateien und
 * Verzeichnisse mit langem Namen legt der Kernel mit dem Namen an, den das
 * Programm gibt (FAT: VFAT- oder EA-Eintrag wie bei FAPI-Programmen).
 *
 * Nicht dabei: Zeiten fuer letzten Zugriff/Anlegen (FAT der Box kennt nur
 * die Aenderung - geliefert wird diese), 71AAh (SUBST), Unicode-Namen
 * (CX der Suche immer 0, Namen in der Codepage).
 */
#include "cm.h"

u32 l_mul(u16 a, u16 b);
#pragma aux l_mul = "mul dx" parm [ax] [dx] value [dx ax] modify exact [ax dx];
u16 l_div(u16 hi, u16 lo, u16 d, u16 *rem);
#pragma aux l_div = \
    "div bx" \
    "mov [si], dx" \
    parm [dx] [ax] [bx] [si] value [ax] modify exact [ax dx];

void do_open(struct frame __far *f, const char *path, unsigned mode,
             unsigned act, unsigned attr, int ext);

static char lp1[DEM_PATHLEN], lp2[DEM_PATHLEN];

static char upc(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static void scp(char *d, const char *s)
{
    while ((*d++ = *s++) != 0)
        ;
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

/* ---- Pfade -------------------------------------------------------------- */

/* in (Box) -> out "X:\Lang\Name" (Schreibung bleibt, . und .. aufgeloest,
   Leerzeichen und Punkte am Ende eines Teils weg wie Windows) */
static unsigned lcanon(const char __far *in, char *out, int wild)
{
    unsigned d = cur_drv, o, b;
    char c;
    if (in[0] && in[1] == ':') {
        d = (unsigned)(upc(in[0]) - 'A');
        in += 2;
    }
    if (d >= NDRV || !drive_ok(d))
        return E_NOPATH;
    if (*in == '\\' || *in == '/') {
        out[0] = (char)('A' + d);
        out[1] = ':';
        o = 2;
    } else {
        for (o = 0; cdstab[d].path[o] && o < 66; o++)
            out[o] = cdstab[d].path[o];
        if (out[o - 1] == '\\')
            o--;
    }
    out[o] = 0;
    while (*in) {
        while (*in == '\\' || *in == '/')
            in++;
        if (!*in)
            break;
        if (in[0] == '.' && (!in[1] || in[1] == '\\' || in[1] == '/')) {
            in++;
            continue;
        }
        if (in[0] == '.' && in[1] == '.' && (!in[2] || in[2] == '\\' || in[2] == '/')) {
            in += 2;
            while (o > 2 && out[o - 1] != '\\')
                o--;
            if (o > 2)
                o--;
            out[o] = 0;
            continue;
        }
        if (o > DEM_PATHLEN - 3)
            return E_NOPATH;
        out[o++] = '\\';
        b = o;
        for (; (c = *in) != 0 && c != '\\' && c != '/'; in++) {
            if ((u8)c < ' ' || c == '"' || c == '|' || c == '<' || c == '>' || c == ':' ||
                (!wild && (c == '*' || c == '?')))
                return E_NOFILE;
            if (o >= DEM_PATHLEN - 2)
                return E_NOPATH;
            out[o++] = c;
        }
        while (o > b && (out[o - 1] == ' ' || out[o - 1] == '.'))
            o--;
        if (o == b)
            o--;
        out[o] = 0;
    }
    if (o == 2) {
        out[2] = '\\';
        out[3] = 0;
    }
    cfg_redirect(out);                  /* X:\CONFIG.SYS -> CONFIG.WIN */
    return 0;
}

/* Pfad des Programms aufloesen -> 0 (q.path kurz, q.path2 lang, q.attr),
   E_NOFILE (letzter Teil fehlt: q.path = kurzes Verzeichnis + Name) oder
   Fehler. Geraete (CON, NUL ...) gelten als vorhanden. */
static unsigned lres(const char __far *in, int wild)
{
    unsigned e = lcanon(in, lp1, wild);
    if (e)
        return e;
    if (dev_of(lp1) >= 0) {
        scp(q.path, lp1);
        scp(q.path2, lp1);
        q.attr = 0;
        return 0;
    }
    scp(q.path, lp1);
    return dem(DEM_LPATH);
}

/* alter Aufruf (3Dh, 4Eh ...) mit langem Pfad -> out = kurze Form, der
   letzte Teil bleibt, wenn es ihn nicht gibt (canon kuerzt ihn auf 8.3) */
int lfn_canon(const char __far *in, char *out)
{
    unsigned e = lcanon(in, lp1, 1);
    if (e)
        return e;
    scp(q.path, lp1);
    e = dem(DEM_LPATH);
    if (e && e != E_NOFILE)
        return e;
    scp(out, q.path);
    return 0;
}

/* ---- Zeiten: DOS <-> FILETIME (100 ns seit 1601, hier ohne Zeitzone) --- */

static const u16 mstart[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
#define LEAP(y) (((y) & 3) == 0)        /* 1980-2107: 2100 kommt nicht vor */
#define D1601   138426UL                /* Tage 1601-01-01 bis 1980-01-01 */

static void m64(u16 *v, u16 m)
{
    u32 c = 0;
    unsigned i;
    for (i = 0; i < 4; i++) {
        c += l_mul(v[i], m);
        v[i] = (u16)c;
        c >>= 16;
    }
}

static void a64(u16 *v, u32 x)
{
    u32 c = x;
    unsigned i;
    for (i = 0; i < 4 && c; i++) {
        c += v[i];
        v[i] = (u16)c;
        c >>= 16;
    }
}

static u16 d64(u16 *v, u16 dv)
{
    u16 r = 0;
    int i;
    for (i = 3; i >= 0; i--)
        v[i] = l_div(r, v[i], dv, &r);
    return r;
}

static void dos2ft(u16 date, u16 time, u16 cs, u16 __far *out)
{
    u16 v[4], y, m, days = 0, k;
    if (!date)
        date = 0x21;                    /* 1.1.1980                      */
    y = date >> 9;
    m = (date >> 5) & 15;
    if (m < 1 || m > 12)
        m = 1;
    for (k = 0; k < y; k++)
        days += LEAP(k) ? 366 : 365;
    days += mstart[m - 1] + ((m > 2 && LEAP(y)) ? 1 : 0) + (date & 31) - 1;
    v[0] = days;
    v[1] = v[2] = v[3] = 0;
    a64(v, D1601);
    m64(v, 675);
    m64(v, 128);                        /* * 86400                      */
    a64(v, l_mul(time >> 11, 3600) + ((time >> 5) & 63) * 60 + (time & 31) * 2);
    m64(v, 10000);
    m64(v, 1000);
    a64(v, l_mul(cs, 50000) * 2);
    for (k = 0; k < 4; k++)
        out[k] = v[k];
}

static void ft2dos(const u16 __far *in, u16 *date, u16 *time, u16 *cs)
{
    u16 v[4], r, r2, y, m, len, h, mi, sc;
    u32 d, sd;
    unsigned k;
    for (k = 0; k < 4; k++)
        v[k] = in[k];
    d64(v, 1000);
    r = d64(v, 10000);                  /* Zehntelmillisekunden           */
    *cs = r / 100;
    r = d64(v, 128);
    r2 = d64(v, 675);
    sd = ((u32)r2 << 7) + r;            /* Sekunden im Tag (< 86400)     */
    d = ((u32)v[1] << 16) | v[0];
    if (d < D1601 || v[2] || v[3]) {
        *date = 0x21;
        *time = 0;
        return;
    }
    d -= D1601;
    for (y = 0; y < 127; y++) {
        len = LEAP(y) ? 366 : 365;
        if (d < len)
            break;
        d -= len;
    }
    for (m = 11; m > 0 && (u16)d < mstart[m] + ((m > 1 && LEAP(y)) ? 1 : 0); m--)
        ;
    d -= mstart[m] + ((m > 1 && LEAP(y)) ? 1 : 0);
    *date = (y << 9) | ((m + 1) << 5) | ((u16)d + 1);
    h = l_div((u16)(sd >> 16), (u16)sd, 3600, &r);
    mi = r / 60;
    sc = r % 60;
    *time = (h << 11) | (mi << 5) | (sc >> 1);
    if (sc & 1)
        *cs += 100;
}

/* ---- Suche 714Eh/714Fh/71A1h -------------------------------------------- */
#define NLF 8
static struct {
    u16  psp;                           /* 0 = frei                      */
    u16  idx, attr, fmt;
    char path[DEM_PATHLEN];             /* "X:\KURZ\Muster"              */
} lf[NLF];

/* naechster Treffer fuer Platz i -> Win32-Suchdaten nach ES:DI */
static unsigned lf_next(unsigned i, u16 seg, u16 off)
{
    unsigned k, l;
    u8 __far *p = FP(seg, off);
    scp(q.path, lf[i].path);
    q.attr = lf[i].attr;
    q.find[0] = (u8)lf[i].idx;
    q.find[1] = (u8)(lf[i].idx >> 8);
    if (dem(DEM_LFIND))
        return q.err;
    lf[i].idx = q.find[0] | (q.find[1] << 8);
    for (k = 0; k < 0x13E; k++)
        p[k] = 0;
    p[0] = (u8)q.attr;
    for (k = 0; k < 3; k++) {           /* Anlegen, Zugriff, Aenderung   */
        u16 __far *t = (u16 __far *)(p + 4 + k * 8);
        if (lf[i].fmt) {                /* SI = 1: DOS-Zeit              */
            t[0] = q.time;
            t[1] = q.date;
        } else
            dos2ft(q.date, q.time, 0, t);
    }
    D(seg, off + 0x20) = q.size;
    for (k = 0; k < 259 && q.path2[k]; k++)
        p[0x2C + k] = q.path2[k];
    /* kurzer Name nur, wenn er sich vom langen unterscheidet */
    for (k = 0; k < 8 && q.name[k] != ' '; k++)
        lp2[k] = q.name[k];
    l = k;
    if (q.name[8] != ' ') {
        lp2[l++] = '.';
        for (k = 8; k < 11 && q.name[k] != ' '; k++)
            lp2[l++] = q.name[k];
    }
    lp2[l] = 0;
    for (k = 0; lp2[k] && lp2[k] == q.path2[k]; k++)
        ;
    if (lp2[k] || q.path2[k])
        for (k = 0; k <= l; k++)
            p[0x130 + k] = lp2[k];
    return 0;
}

static void lf_first(struct frame __far *f)
{
    unsigned i, e, n, last;
    for (i = 0; i < NLF && lf[i].psp; i++)
        ;
    if (i == NLF) {
        set_err(f, E_NOHANDLES);
        return;
    }
    if ((e = lcanon(FP(f->ds, f->dx), lp1, 1)) != 0) {
        set_err(f, e == E_NOFILE ? E_NOPATH : e);
        return;
    }
    for (n = last = 0; lp1[n]; n++)
        if (lp1[n] == '\\')
            last = n;
    scp(lp2, lp1 + last + 1);           /* Muster                        */
    if (last == 2)
        lp1[3] = 0;
    else
        lp1[last] = 0;
    scp(q.path, lp1);                   /* Verzeichnis kurz machen       */
    if (lp1[3] && (dem(DEM_LPATH) || !(q.attr & 0x10))) {
        set_err(f, E_NOPATH);
        return;
    }
    n = slen(q.path);
    if (n + slen(lp2) + 2 > DEM_PATHLEN) {
        set_err(f, E_NOPATH);
        return;
    }
    scp(lf[i].path, q.path);
    if (lf[i].path[n - 1] != '\\')
        lf[i].path[n++] = '\\';
    scp(lf[i].path + n, lp2);
    lf[i].idx = 0;
    lf[i].attr = f->cx;
    lf[i].fmt = f->si & 1;
    if ((e = lf_next(i, f->es, f->di)) != 0) {
        set_err(f, e == E_NOMORE ? E_NOFILE : e);
        return;
    }
    lf[i].psp = cur_psp();
    f->ax = i + 1;
    f->cx = 0;
}

void lfn_end(u16 psp)
{
    unsigned i;
    for (i = 0; i < NLF; i++)
        if (lf[i].psp == psp)
            lf[i].psp = 0;
}

/* ---- 440Dh 0871h: erster Cluster einer Datei (DS:DX) -------------------- */
void lfn_fclus(struct frame __far *f)
{
    unsigned e = lres(FP(f->ds, f->dx), 0);
    if (e || (q.attr & 0x18)) {
        set_err(f, e ? e : E_DENIED);
        return;
    }
    if (dem(DEM_FCLUS)) {
        set_err(f, q.err);
        return;
    }
    f->ax = (u16)q.pos;
    f->dx = (u16)(q.pos >> 16);
}

/* ---- Verteiler ---------------------------------------------------------- */
static void unsup(struct frame __far *f)
{
    f->ax = 0x7100;                     /* wie DOS ohne lange Namen      */
    f->fl |= FL_CF;
}

void lfn_fn(struct frame __far *f)
{
    unsigned e, i, n, d;
    u8 __far *p;
    switch (AL(f)) {
    case 0x0D:                          /* Laufwerk zuruecksetzen        */
        return;
    case 0x39:                          /* Verzeichnis anlegen           */
        e = lres(FP(f->ds, f->dx), 0);
        if (e == E_NOFILE) {
            q.rsv = DEM_R_LONG;
            e = dem(DEM_MKDIR);
            q.rsv = 0;
        } else if (!e)
            e = E_DENIED;
        if (e)
            set_err(f, e);
        return;
    case 0x3A:                          /* Verzeichnis entfernen         */
        e = lres(FP(f->ds, f->dx), 0);
        if (!e && !(q.attr & 0x10))
            e = E_NOPATH;
        if (!e) {
            d = (unsigned)(q.path[0] - 'A');
            for (n = 0; q.path[n] && q.path[n] == cdstab[d].path[n]; n++)
                ;
            e = !q.path[n] && !cdstab[d].path[n] ? E_CURDIR : dem(DEM_RMDIR);
        }
        if (e)
            set_err(f, e == E_NOFILE ? E_NOPATH : e);
        return;
    case 0x3B:                          /* Verzeichnis wechseln          */
        e = lres(FP(f->ds, f->dx), 0);
        if (e || !(q.attr & 0x10) || slen(q.path) > 66) {
            set_err(f, E_NOPATH);
            return;
        }
        d = (unsigned)(q.path[0] - 'A');
        scp(cdstab[d].path, q.path);
        return;
    case 0x41:                          /* loeschen, SI = 1: Platzhalter */
        if (f->si != 1) {
            e = lres(FP(f->ds, f->dx), 0);
            if (!e && (q.attr & 0x18))
                e = E_DENIED;
            if (!e)
                e = dem(DEM_DELETE);
            if (e)
                set_err(f, e);
            return;
        }
        for (i = 0; i < NLF && lf[i].psp; i++)
            ;
        if (i == NLF) {
            set_err(f, E_NOHANDLES);
            return;
        }
        {   /* ueber die Suche: Platz i kurz belegen */
            u16 scx = f->cx, sdi = f->di, ses = f->es;
            static u8 fd[0x13E];
            n = 0;
            f->cx = (scx & 0xFF) | ((scx & 0xFF00));
            f->di = (u16)fd;
            f->es = getds();
            f->si = 1;
            lf_first(f);
            f->cx = scx;
            f->di = sdi;
            f->es = ses;
            if (f->fl & FL_CF)
                return;
            i = f->ax - 1;
            do {
                for (e = 0; lf[i].path[e]; e++)
                    ;
                while (e && lf[i].path[e - 1] != '\\')
                    e--;
                scp(lp2, lf[i].path);
                lp2[e] = 0;
                for (d = 0; d < 13 && fd[0x130 + d]; d++)
                    lp2[e + d] = fd[0x130 + d];
                if (!d)
                    for (d = 0; fd[0x2C + d] && e + d < DEM_PATHLEN - 1; d++)
                        lp2[e + d] = fd[0x2C + d];
                lp2[e + d] = 0;
                if (!(fd[0] & 0x18)) {
                    scp(q.path, lp2);
                    if (!dem(DEM_DELETE)) {
                        n++;
                        lf[i].idx--;
                    }
                }
            } while (!lf_next(i, getds(), (u16)fd));
            lf[i].psp = 0;
            f->ax = 0;
            if (!n)
                set_err(f, E_NOFILE);
        }
        return;
    case 0x43:                          /* Attribute, Zeiten             */
        e = lres(FP(f->ds, f->dx), 0);
        if (e) {
            set_err(f, e);
            return;
        }
        switch (BL(f)) {
        case 0:
            f->cx = q.attr;
            return;
        case 1:
            q.attr = f->cx;
            if (dem(DEM_SETATTR))
                set_err(f, q.err);
            return;
        case 2:
            f->ax = (u16)q.size;
            f->dx = (u16)(q.size >> 16);
            return;
        case 3:                         /* letzte Aenderung setzen       */
            q.mode = 2;
            q.act = DEM_ACT_OPEN;
            q.attr = 0;
            if (dem(DEM_OPEN)) {
                set_err(f, q.err);
                return;
            }
            q.flags = DEM_F_TIME;
            q.date = f->di;
            q.time = f->cx;
            dem(DEM_CLOSE);
            q.flags = 0;
            return;
        case 4: case 8:
            f->cx = q.time;
            f->di = q.date;
            if (BL(f) == 8)
                f->si = 0;
            return;
        case 6:
            f->di = q.date;
            return;
        case 5: case 7:                 /* FAT der Box: nur Aenderung    */
            return;
        }
        set_err(f, E_FUNC);
        return;
    case 0x47:                          /* aktuelles Verzeichnis, lang   */
        d = DL(f) ? DL(f) - 1 : cur_drv;
        if (!drive_ok(d)) {
            set_err(f, E_DRIVE);
            return;
        }
        scp(q.path, cdstab[d].path);
        if (q.path[3] && dem(DEM_LPATH)) {
            set_err(f, E_NOPATH);
            return;
        }
        p = FP(f->ds, f->si);
        for (n = 0; q.path[3] && q.path2[3 + n]; n++)
            p[n] = q.path2[3 + n];
        p[n] = 0;
        f->ax = 0x0100;
        return;
    case 0x4E:
        lf_first(f);
        return;
    case 0x4F:
        i = f->bx - 1;
        if (i >= NLF || !lf[i].psp) {
            set_err(f, E_HANDLE);
            return;
        }
        if ((e = lf_next(i, f->es, f->di)) != 0)
            set_err(f, e);
        else
            f->cx = 0;
        return;
    case 0xA1:
        i = f->bx - 1;
        if (i >= NLF || !lf[i].psp) {
            set_err(f, E_HANDLE);
            return;
        }
        lf[i].psp = 0;
        return;
    case 0x56:                          /* umbenennen/verschieben        */
        e = lres(FP(f->ds, f->dx), 0);
        if (e) {
            set_err(f, e);
            return;
        }
        scp(lp2, q.path);               /* Quelle kurz                   */
        e = lcanon(FP(f->es, f->di), lp1, 0);
        if (!e) {
            scp(q.path, lp1);
            e = dem(DEM_LPATH);
        }
        if (!e) {                       /* Ziel gibt es: nur die eigene  */
            for (n = 0; q.path[n] && q.path[n] == lp2[n]; n++)
                ;                       /* Datei (andere Schreibung)    */
            if (q.path[n] || lp2[n]) {
                set_err(f, E_DENIED);
                return;
            }
            for (n = slen(q.path); n && q.path[n - 1] != '\\'; n--)
                ;
            for (i = slen(lp1); i && lp1[i - 1] != '\\'; i--)
                ;
            scp(q.path + n, lp1 + i);   /* kurzes Verzeichnis + Name     */
        } else if (e != E_NOFILE) {
            set_err(f, e);
            return;
        }
        scp(q.path2, q.path);
        scp(q.path, lp2);
        if (dem(DEM_RENAME))
            set_err(f, q.err);
        return;
    case 0x60:                          /* voller Pfad: CL 0/1 kurz/2 lang */
        if (CL(f) == 0)
            e = lcanon(FP(f->ds, f->si), q.path2, 1);
        else {
            e = lres(FP(f->ds, f->si), 0);
            if (CL(f) == 1)
                scp(q.path2, q.path);
        }
        if (e) {
            set_err(f, e);
            return;
        }
        p = FP(f->es, f->di);
        for (n = 0; q.path2[n]; n++)
            p[n] = q.path2[n];
        p[n] = 0;
        f->ax = 0;
        return;
    case 0x6C: case 0xA9:               /* erweitertes Oeffnen           */
        e = lres(FP(f->ds, f->si), 0);
        if (e && e != E_NOFILE) {
            set_err(f, e);
            return;
        }
        if (!e && (q.attr & 0x18)) {
            set_err(f, E_DENIED);
            return;
        }
        if (e && !(DL(f) & 0xF0)) {
            set_err(f, E_NOFILE);
            return;
        }
        scp(lp1, q.path);
        if (e)
            q.rsv = DEM_R_LONG;
        do_open(f, lp1, f->bx & 0xFF, DL(f), f->cx, 1);
        q.rsv = 0;
        return;
    case 0xA0:                          /* Datentraeger: LFN, FAT        */
        p = FP(f->es, f->di);
        if (f->cx >= 4) {
            p[0] = 'F'; p[1] = 'A'; p[2] = 'T'; p[3] = 0;
        }
        f->bx = 0x4006;                 /* Schreibung bleibt, LFN       */
        f->cx = 255;
        f->dx = 260;
        f->ax = 0;
        return;
    case 0xA6: {                        /* Dateiinfo zum Handle          */
        extern struct sft *sftab;
        int s = hnd_sft(f->bx);
        u16 __far *w;
        if (s < 0) {
            set_err(f, E_HANDLE);
            return;
        }
        p = FP(f->ds, f->dx);
        for (n = 0; n < 0x34; n++)
            p[n] = 0;
        p[0] = sftab[s].attr;
        for (n = 0; n < 3; n++) {
            w = (u16 __far *)(p + 4 + n * 8);
            dos2ft(sftab[s].date, sftab[s].time, 0, w);
        }
        D(f->ds, f->dx + 0x24) = sftab[s].size;
        p[0x28] = 1;
        return;
    }
    case 0xA7:                          /* Zeit umrechnen                */
        if (BL(f) == 0) {
            u16 dd, tt, cc;
            ft2dos((u16 __far *)FP(f->ds, f->si), &dd, &tt, &cc);
            f->dx = dd;
            f->cx = tt;
            BH(f) = (u8)cc;
        } else if (BL(f) == 1)
            dos2ft(f->dx, f->cx, BH(f), (u16 __far *)FP(f->es, f->di));
        else
            set_err(f, E_FUNC);
        return;
    case 0xA8: {                        /* kurzen Namen bilden           */
        u8 __far *s = FP(f->ds, f->si);
        char o[11], c;
        unsigned k, dot = 0;
        for (k = 0; k < 11; k++)
            o[k] = ' ';
        for (n = 0; s[n]; n++)
            if (s[n] == '.')
                dot = n + 1;
        if (dot == 1)
            dot = 0;                    /* ".profil": kein Name-Teil    */
        for (n = k = 0; s[n] && (!dot || n < dot - 1) && k < 8; n++) {
            c = upc((char)s[n]);
            if (c != ' ' && c != '.')
                o[k++] = (c == '+' || c == ',' || c == ';' || c == '=' ||
                          c == '[' || c == ']') ? '_' : c;
        }
        for (n = dot, k = 8; dot && s[n] && k < 11; n++) {
            c = upc((char)s[n]);
            if (c != ' ' && c != '.')
                o[k++] = (c == '+' || c == ',' || c == ';' || c == '=' ||
                          c == '[' || c == ']') ? '_' : c;
        }
        p = FP(f->es, f->di);
        if (DH(f) == 0)
            for (k = 0; k < 11; k++)
                p[k] = o[k];
        else {
            for (k = n = 0; k < 8 && o[k] != ' '; k++)
                p[n++] = o[k];
            if (o[8] != ' ') {
                p[n++] = '.';
                for (k = 8; k < 11 && o[k] != ' '; k++)
                    p[n++] = o[k];
            }
            p[n] = 0;
        }
        f->ax = 0;
        return;
    }
    }
    unsup(f);
}
