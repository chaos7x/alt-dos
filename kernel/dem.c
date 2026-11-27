/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - DEM (DOS-Emulationsmodul): Dateiarbeit fuer das DOS einer
 * DOS-Box (docs/DOSKERN.md, Stufe 1; Vertrag in include/dem.h)
 *
 * Herausgeloest aus dem Redirector (kernel/redir.c, bis 21.11.2026 alles
 * dort): hier steht nur, was mit Dateien zu tun hat - Pfade, Suche,
 * Oeffnen mit SHARE innerhalb der Box, Lesen/Schreiben direkt zwischen
 * Datei und Speicher der Box, Sperren, Attribute, Laufwerksinfo. Die
 * Strukturen des DOS (SFT, SDA, CDS) kennt das DEM nicht; Eingaenge
 * fuellen struct dem_req und tragen das Ergebnis selbst ein. Erledigt
 * wird alles mit den FAPI-Funktionen im Task der Box (kf_box_fapi):
 * Pfadaufloesung, SUBST/JOIN, Sperren, Cache und NET USE gelten wie fuer
 * FAPI-Programme, task_end schliesst offene Dateien der Box.
 *
 * Laeuft im Kontext der Box unter der Kernelsperre (BOX_LOCK, vom
 * Eingang genommen) - statische Puffer stehen im Kontext je Task
 * (dem_ctx).
 */
#include "k2.h"
#include "../include/fapi_abi.h"
#include "../include/dem.h"
#include "v86int.h"
#include "ifs.h"
#include "dembox.h"
#include "task.h"

extern int current;
unsigned long __far kf_hfirst(unsigned h);

int  __far kf_box_ctl(unsigned op, unsigned a);
int __far fat_geom(int d, unsigned *spc, unsigned long *clusters);
int __far fat_abssec(int d, unsigned long lba, unsigned long *total);
void __far vol_reset(int d);
int __far kf_hmeta(unsigned h, unsigned what, unsigned attr, unsigned date, unsigned time);
unsigned __far kf_box_fapi(unsigned fn, unsigned usel, unsigned a1, unsigned a2, unsigned a3);
unsigned long __far kf_cd_blocks(int d);
int __far kf_cd_read(int d, unsigned long lba, unsigned sel, unsigned off);
#define FAPI(fn, sel, a, b, c)  kf_box_fapi((fn), (sel), (a), (b), (c))

/* lineare Adresse der Box */
static unsigned char __far *bl(int t, unsigned long lin)
{
    return bp(t, (unsigned)(lin >> 4), (unsigned)lin & 15);
}

static char tmpl[11];
static struct fapi_dirent de;
static struct fapi_fsinfo fsi;
static struct fapi_finfo fin;
static unsigned long l32;

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
    case FAPI_ERR_LOCKED:   return E_LOCK;          /* Sperrverletzung       */
    case FAPI_ERR_FULL:     return E_DENIED;
    }
    return def;
}

/* Pruefsumme eines Namens in FCB-Form (Suchzustand) */
static unsigned h11(const char *f)
{
    unsigned h = 0, k;
    for (k = 0; k < 11; k++)
        h = (h << 5) + h + (unsigned char)f[k];
    return h;
}

static unsigned long phash(const char *p)
{
    unsigned long h = 5381;
    while (*p)
        h = ((h << 5) + h) ^ up(*p++);
    return h;
}

/* Eintrag zu Pfad p in de suchen (Stamm: Verzeichnis) -> 0 gefunden */
static char st_dir[80], st_fcb[11];
static int stat_path(const char *p)
{
    const char *n = split(p, st_dir);
    unsigned i, k;
    if (!*n) {                              /* "C:\"                     */
        de.attr = 0x10;
        de.size = 0;
        de.date = de.time = 0;
        return 0;
    }
    to_fcb(n, tmpl);
    for (i = 0; ; i++) {
        if (FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, i, (unsigned)st_dir)))
            return -1;
        to_fcb(de.name, st_fcb);
        for (k = 0; k < 11 && st_fcb[k] == tmpl[k]; k++)
            ;
        if (k == 11)
            return 0;
    }
}

/* Fehlt das Verzeichnis, in dem p liegen soll? MS-DOS unterscheidet:
   Datei fehlt 02h, Verzeichnis fehlt 03h */
static char np_dir[80];
static int no_dir(const char *p)
{
    split(p, np_dir);
    if (!np_dir[3])                         /* "C:" gibt es immer       */
        return 0;
    return stat_path(np_dir) || !(de.attr & 0x10);
}
static unsigned nf(const char *p) { return no_dir(p) ? E_NOPATH : E_NOFILE; }

/* ---- SHARE innerhalb der Box (18.11.2026) ----------------------------------
   Wie SHARE.EXE: oeffnet ein Programm der Box eine Datei, die die Box
   schon offen hat, teilen sich beide Oeffnungen denselben Handle des
   Kernels - die Groesse gibt es nur einmal. Freigabemodi (Bits 4-6 des
   Modus) gegen die anderen Oeffnungen der Box, Bereichssperren je Datei.
   Zwischen Boxen, FAPI-Programmen und anderen Rechnern gelten die Regeln
   des Kernels bzw. des Servers. DEM-Handle = E000h | Platz (sonst Handle
   des Kernels, wenn die Tabelle voll ist: dann ohne Freigabepruefung). */
#define NSH 12
#define NLK 4
#define SH_MARK 0xE000
struct shf {
    unsigned       h1;                  /* Handle des Kernels + 1, 0 frei */
    unsigned long  hash, size;          /* Pfad; groesste geschriebene   */
    unsigned char  refs, rd, wr, dr, dw, kw;    /* Stelle; kw: Handle RW */
};
struct shl {
    unsigned char  used, slot;
    unsigned long  ofs, len;
};
/* Was je Box bleibt (Suchverzeichnisse, SHARE) liegt in einem Kernel-
   segment fuer alle Boxen (DGROUP ist knapp), angelegt beim ersten
   Start einer Box */
struct rbx {
    char           sdir[4][80];         /* Verzeichnisse laufender Suchen */
    unsigned char  sgen[4], snext;      /* Generation je Platz, naechster */
    struct shf     sh[NSH];
    struct shl     lk[NLK];
    unsigned       dlk[26];             /* Laufwerkssperre: Handle + 1    */
    unsigned char  dfl[26];             /* dazu Bit 0 FAT32, 1 ueber 32 MB */
};
static unsigned rb_sel;
static struct fapi_lock rlk;            /* Auftrag an FAPI_FILELOCK       */
#define RBX(t)  ((struct rbx __far *)MK_FP(rb_sel, (unsigned)(t) * sizeof(struct rbx)))

/* neue Box: Tabellen leer -> 0, -1 kein Kernelsegment frei */
int __far dem_box_init(int t)
{
    unsigned i;
    if (!rb_sel)
        rb_sel = kseg_alloc((unsigned long)NTASK * sizeof(struct rbx));
    if (!rb_sel)
        return -1;
    for (i = 0; i < NSH; i++)
        RBX(t)->sh[i].h1 = 0;
    for (i = 0; i < NLK; i++)
        RBX(t)->lk[i].used = 0;
    for (i = 0; i < 26; i++)
        RBX(t)->dlk[i] = 0;
    return 0;
}

int __far dem_ready(void) { return rb_sel != 0; }

static int sh_find(int t, unsigned long hv)
{
    int i;
    for (i = 0; i < NSH; i++)
        if (RBX(t)->sh[i].h1 && RBX(t)->sh[i].hash == hv)
            return i;
    return -1;
}

/* Modus m (Zugriff Bits 0-1, Freigabe Bits 4-6) zaehlen bzw. pruefen */
#define M_RD(m)  (((m) & 3) != 1)
#define M_WR(m)  (((m) & 3) != 0)
#define M_DR(m)  ((((m) >> 4) & 7) == 1 || (((m) >> 4) & 7) == 3)
#define M_DW(m)  ((((m) >> 4) & 7) == 1 || (((m) >> 4) & 7) == 2)
static void sh_add(struct shf __far *e, unsigned m, int d)
{
    e->rd += M_RD(m) ? d : 0;
    e->wr += M_WR(m) ? d : 0;
    e->dr += M_DR(m) ? d : 0;
    e->dw += M_DW(m) ? d : 0;
}
static int sh_ok(const struct shf __far *e, unsigned m)
{
    return !((M_RD(m) && e->dr) || (M_WR(m) && e->dw) ||
             (M_DR(m) && e->rd) || (M_DW(m) && e->wr));
}

/* DEM-Handle -> Handle des Kernels (FFFFh = keiner), *e = geteilter Eintrag */
static unsigned dh(int t, unsigned long v, struct shf __far **e)
{
    *e = 0;
    if (((unsigned)v & 0xFF00) == SH_MARK) {
        if ((v & 0xFF) >= NSH || !RBX(t)->sh[v & 0xFF].h1)
            return 0xFFFF;
        *e = &RBX(t)->sh[v & 0xFF];
        return (*e)->h1 - 1;
    }
    return (unsigned)v;
}

/* Daten zwischen Datei und Speicher der Box in Stuecken (FAPI hoechstens
   7FFFh) - direkt ueber das Fenster der Box, ohne Zwischenpuffer */
unsigned __far dem_xfer(int t, unsigned fn, unsigned h, unsigned long lin, unsigned n,
                        unsigned *err)
{
    unsigned done = 0, k, r;
    *err = 0;
    while (done < n) {
        k = n - done > 0x4000 ? 0x4000 : n - done;
        bl(t, lin & ~15UL);
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

/* ---- Suche ------------------------------------------------------------------ */

/* Suchzustand (21 Byte, legt das DOS im DTA ab, gehoert dem DEM):
   0 Laufwerk|80h, 1-11 Muster, 12 Attribute, 13/14 Eintrag, 15 Platz,
   16 Generation, 17/18 Pruefsumme des zuletzt gefundenen Namens */
static char fn_fcb[11], fn_tp[11], fn_dir[80];
static struct fapi_label fn_lb;
static unsigned g16(const dem_u8 *p) { return p[0] | (p[1] << 8); }
static void p16(dem_u8 *p, unsigned v) { p[0] = (dem_u8)v; p[1] = (dem_u8)(v >> 8); }

static unsigned find_next(int t, struct dem_req *q)
{
    dem_u8 *s = q->find;
    unsigned n = g16(s + 13), slot = s[15] & 3, k, lh = g16(s + 17);
    unsigned char mask = s[12], a;
    if (n == 0xFFFF)
        return E_NOMORE;                    /* nach der Bezeichnung      */
    if (mask != 0x08 && s[16] != RBX(t)->sgen[slot])
        return E_NOMORE;                    /* Platz inzwischen anders   */
    for (k = 0; k < 79 && (fn_dir[k] = RBX(t)->sdir[slot][k]) != 0; k++)
        ;                                   /* Pfad nach DGROUP (FAPI)   */
    fn_dir[k] = 0;
    if (mask == 0x08) {                     /* nur Datentraegerbezeichnung */
        k = FAPI(FAPI_LABEL, KDATA, s[0] & 0x1F, (unsigned)&fn_lb, 0);
        if (k == FAPI_ERR_NOTREADY ||       /* leer: INT 24h wie MS-DOS -  */
            (!FAILED(FAPI(FAPI_QFSINFO, KDATA, s[0] & 0x1F, (unsigned)&fsi, 0)) &&
             (fsi.flags & FSI_NOMEDIA)))    /* der Datei-Manager von       */
            return E_NOTREADY;              /* Windows sah sonst eine leere */
        if (FAILED(k) || !fn_lb.label[0])   /* Diskette ohne Bezeichnung   */
            return E_NOMORE;
        for (k = 0; k < 11; k++)
            fn_fcb[k] = ' ';
        for (k = 0, a = 0; fn_lb.label[k] && a < 11; k++)
            if (fn_lb.label[k] != '.')
                fn_fcb[a++] = fn_lb.label[k];
        de.attr = 0x08;
        de.time = de.date = 0;
        de.size = 0;
        n = 0xFFFE;
        goto found;
    }
    for (k = 0; k < 11; k++)
        fn_tp[k] = s[1 + k];
    /* Eintrag n-1 inzwischen geloescht (DEL *.* loescht zwischen den
       Suchschritten)? Dann sind alle folgenden eins nach vorn gerueckt. */
    if (n && n < 0xFFFE &&
        !FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, n - 1, (unsigned)fn_dir))) {
        to_fcb(de.name, fn_fcb);
        if (h11(fn_fcb) != lh)
            n--;
    }
    for (;; n++) {
        k = FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, n, (unsigned)fn_dir);
        if (FAILED(k))                      /* leeres Laufwerk: INT 24h  */
            return k == FAPI_ERR_NOTREADY ? E_NOTREADY : E_NOMORE;
        a = de.attr;
        if ((a & 0x08) && mask != 0x08)
            continue;                       /* Datentraegerbezeichnung   */
        if (a & ~mask & 0x16)
            continue;                       /* versteckt/System/Verz.    */
        to_fcb(de.name, fn_fcb);
        if (!fcb_match(fn_tp, fn_fcb))
            continue;
        break;
    }
found:
    p16(s + 13, n + 1);
    p16(s + 17, h11(fn_fcb));
    for (k = 0; k < 11; k++)
        q->name[k] = fn_fcb[k];
    q->attr = de.attr;
    q->time = de.time;
    q->date = de.date;
    q->size = de.size;
    return 0;
}

static char ff_dir[80];
static unsigned find_first(int t, struct dem_req *q)
{
    const char *n;
    dem_u8 *s = q->find;
    unsigned slot, k, i;
    n = split(q->path, ff_dir);
    for (slot = 0; slot < 4; slot++) {      /* gleiches Verzeichnis:     */
        for (i = 0; ff_dir[i] && ff_dir[i] == RBX(t)->sdir[slot][i]; i++)
            ;                               /* gleicher Platz (Word-5.5- */
        if (ff_dir[i] == RBX(t)->sdir[slot][i])  /* SETUP sucht verschachtelt */
            break;                          /* in A:\ und C:\WORD)      */
    }
    if (slot == 4) {
        slot = RBX(t)->snext++ & 3;
        for (i = 0; (RBX(t)->sdir[slot][i] = ff_dir[i]) != 0; i++)
            ;
        RBX(t)->sgen[slot]++;
    }
    to_fcb(*n ? n : "????????.???", tmpl);
    s[0] = (dem_u8)(0x80 | (up(q->path[0]) - 'A'));
    for (k = 0; k < 11; k++)
        s[1 + k] = tmpl[k];
    s[12] = (dem_u8)q->attr;
    p16(s + 13, 0);
    s[15] = (dem_u8)slot;
    s[16] = RBX(t)->sgen[slot];
    for (k = 17; k < 21; k++)
        s[k] = 0;
    k = find_next(t, q);
    if (k == E_NOMORE && no_dir(q->path))
        k = E_NOPATH;                       /* Verzeichnis fehlt: 03h    */
    return k;                           /* ohne Treffer 12h wie FreeDOS
                                           (fatdir.c) und MS-DOS - Lotus
                                           Works INSTALL hielt 02h fuer
                                           einen Lesefehler */
}

/* ---- Umbenennen, Loeschen mit Platzhaltern ------------------------------- */

static char op_dir[80], op_fcb[11], op_one[96];

/* "NAME    EXT" -> o */
static void fcb_name(const char *f, char *o)
{
    unsigned k, l = 0;
    for (k = 0; k < 8 && f[k] != ' '; k++)
        o[l++] = f[k];
    if (f[8] != ' ') {
        o[l++] = '.';
        for (k = 8; k < 11 && f[k] != ' '; k++)
            o[l++] = f[k];
    }
    o[l] = 0;
}

/* REN mit Platzhaltern: FreeDOS loest sie selbst auf, MS-DOS reicht sie
   durch: je Treffer von path den neuen Namen nach der FCB-Regel bilden
   (? = Zeichen der Quelle an dieser Stelle, sonst Zeichen des Musters),
   im Zielverzeichnis von path2 */
static unsigned ren_wild(struct dem_req *q)
{
    char t2[11], nn[11];
    const char *n;
    unsigned i = 0, hit = 0, k, l, r, err = 0;
    to_fcb(split(q->path2, ff_dir), t2);        /* Ziel: ff_dir + Muster  */
    n = split(q->path, op_dir);
    to_fcb(n, tmpl);
    while (!FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, i++, (unsigned)op_dir))) {
        to_fcb(de.name, op_fcb);
        if ((de.attr & 0x1E) || !fcb_match(tmpl, op_fcb))
            continue;
        for (k = 0; k < 11; k++)
            nn[k] = t2[k] == '?' ? op_fcb[k] : t2[k];
        for (l = 0; op_dir[l] && l < 80; l++)
            op_one[l] = op_dir[l];
        if (op_one[l - 1] != '\\')
            op_one[l++] = '\\';
        for (k = 0; de.name[k] && l < 95; k++)
            op_one[l++] = de.name[k];
        op_one[l] = 0;
        for (l = 0; ff_dir[l] && l < 80; l++)
            q->path2[l] = ff_dir[l];
        if (q->path2[l - 1] != '\\')
            q->path2[l++] = '\\';
        fcb_name(nn, q->path2 + l);
        r = FAPI(FAPI_MOVE, KDATA, (unsigned)op_one, (unsigned)q->path2, 0);
        if (FAILED(r))
            err = r == FAPI_ERR_EXIST ? E_DENIED : errmap(r, E_DENIED);
        else
            hit++;
    }
    if (!hit)
        return err ? err : nf(q->path);
    return 0;                                   /* wie MS-DOS: Teilerfolg */
}

static unsigned del_wild(struct dem_req *q)
{
    const char *n;
    unsigned i = 0, hit = 0, j, l;
    n = split(q->path, op_dir);
    to_fcb(n, tmpl);
    while (!FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, i, (unsigned)op_dir))) {
        to_fcb(de.name, op_fcb);
        if ((de.attr & 0x1F) || !fcb_match(tmpl, op_fcb)) {
            i++;
            continue;
        }
        for (l = 0; op_dir[l]; l++)
            op_one[l] = op_dir[l];
        if (op_one[l - 1] != '\\')
            op_one[l++] = '\\';
        for (j = 0; de.name[j] && l < 95; j++)
            op_one[l++] = de.name[j];
        op_one[l] = 0;
        if (FAILED(FAPI(FAPI_DELETE, KDATA, (unsigned)op_one, 0, 0)))
            i++;                            /* bleibt stehen: weiter */
        else
            hit++;
    }
    return hit ? 0 : nf(q->path);
}

/* ---- Oeffnen, Schliessen, Lesen, Schreiben, Sperren ------------------------ */

static unsigned d_open(int t, struct dem_req *q)
{
    unsigned mode = q->mode, act, a = q->act, exists;
    unsigned long hv;
    unsigned h;
    int slot;
    if (q->rsv & DEM_R_LONG) {              /* langer Name, die Box hat */
        exists = 0;                         /* per DEM_LPATH geprueft   */
        de.attr = 0x20;                     /* (8.3-Vergleich fuehrte   */
        de.size = 0;                        /* sonst auf eine andere    */
        de.time = de.date = 0;              /* Datei)                   */
    } else
        exists = stat_path(q->path) == 0;
    if (exists && (de.attr & 0x18))
        return E_DENIED;                    /* Verzeichnis/Bezeichnung */
    if (exists && (a & 0x0F) == 0)
        return E_EXISTS;
    if (!exists && !(a & 0xF0))
        return nf(q->path);
    act = !exists ? 2 : (a & 0x0F) == 2 ? 3 : 1;
    hv = phash(q->path);
    slot = sh_find(t, hv);
    if (slot >= 0) {                        /* Box hat sie schon offen */
        struct shf __far *e = &RBX(t)->sh[slot];
        unsigned ns = v86_noshare(t);       /* SHARE=OFF: wie MS-DOS ohne */
        if (!ns && (act != 1 || !sh_ok(e, mode)))   /* SHARE - Anlegen    */
            return E_DENIED;                /* kuerzt die offene Datei    */
        if ((M_WR(mode) || act != 1) && !e->kw) {   /* (Windows-SETUP und */
                                            /* geladene Schriften)        */
                                            /* Handle nur lesend: neu RW  */
            FAPI(FAPI_CLOSE, KDATA, e->h1 - 1, 0, 0);
            h = FAPI(FAPI_OPEN, KDATA, (unsigned)q->path, OPEN_RW | (mode & OPEN_SHARE_MASK), 0);
            if (FAILED(h)) {
                unsigned r = FAPI(FAPI_OPEN, KDATA, (unsigned)q->path,
                                  OPEN_READ | (mode & OPEN_SHARE_MASK), 0);
                e->h1 = FAILED(r) ? 0 : r + 1;
                return errmap(h, E_DENIED);
            }
            e->h1 = h + 1;
            e->kw = 1;
        }
        if (act != 1) {                     /* nur bei SHARE=OFF: kuerzen */
            l32 = 0;
            FAPI(FAPI_NEWSIZE, KDATA, e->h1 - 1, (unsigned)&l32, 0);
            e->size = 0;
            de.size = 0;
        }
        e->refs++;
        sh_add(e, mode, 1);
        if (e->size > de.size)
            de.size = e->size;
        q->handle = SH_MARK | slot;
        goto ok;
    }
    if (act == 1)                           /* vorhanden: lesen oder      */
        h = FAPI(FAPI_OPEN, KDATA, (unsigned)q->path,     /* Freigabe an  */
                 ((mode & 3) ? OPEN_RW : OPEN_READ) | (mode & OPEN_SHARE_MASK), 0);
    else                                    /* lesen+schreiben            */
        h = FAPI(FAPI_CREATE, KDATA, (unsigned)q->path, mode & OPEN_SHARE_MASK, 0);
    if (FAILED(h))
        return errmap(h, act == 1 ? E_NOFILE : no_dir(q->path) ? E_NOPATH : E_DENIED);
    if (act != 1) {
        if (q->attr & 0x07)                 /* versteckt/System/R:   */
            kf_hmeta(h, 2, (q->attr & 0x27) | 0x20, 0, 0);  /* beim Schliessen */
        de.attr = 0x20 | (q->attr & 0x07);
        de.size = 0;
        if (!(q->rsv & DEM_R_LONG))
            stat_path(q->path);             /* Datum/Uhrzeit         */
        de.size = 0;
    }
    q->handle = h;
    for (slot = 0; slot < NSH && RBX(t)->sh[slot].h1; slot++)
        ;
    if (slot < NSH) {                       /* in die Tabelle         */
        struct shf __far *e = &RBX(t)->sh[slot];
        e->h1 = h + 1;
        e->hash = hv;
        e->size = de.size;
        e->refs = 1;
        e->rd = e->wr = e->dr = e->dw = 0;
        e->kw = act != 1 || (mode & 3);
        sh_add(e, mode, 1);
        q->handle = SH_MARK | slot;
    }
ok:
    q->act = act;
    q->attr = de.attr;
    q->time = de.time;
    q->date = de.date;
    q->size = de.size;
    return 0;
}

static unsigned d_close(int t, struct dem_req *q)
{
    struct shf __far *e;
    unsigned h = dh(t, q->handle, &e), r;
    if (h == 0xFFFF)
        return 0;
    if (q->flags & DEM_F_TIME)              /* Datum per 5701h gesetzt */
        kf_hmeta(h, 1, 0, q->date, q->time);
    if (e) {
        sh_add(e, q->mode, -1);
        if (--e->refs == 0) {               /* letzte Oeffnung: zu,   */
            FAPI(FAPI_CLOSE, KDATA, h, 0, 0);
            e->h1 = 0;                      /* Sperren weg            */
            for (r = 0; r < NLK; r++)
                if (RBX(t)->lk[r].slot == ((unsigned)q->handle & 0xFF))
                    RBX(t)->lk[r].used = 0;
        }
    } else
        FAPI(FAPI_CLOSE, KDATA, h, 0, 0);
    return 0;
}

static unsigned d_rw(int t, struct dem_req *q)
{
    struct shf __far *e;
    unsigned h = dh(t, q->handle, &e), r, k, err;
    if (h == 0xFFFF)
        return E_HANDLE;
    l32 = q->pos;
    if (FAILED(FAPI(FAPI_SEEK, KDATA, h, (unsigned)&l32, 0)))
        return E_DENIED;
    if (l32 < q->pos && q->count && q->fn == DEM_WRITE) {
        /* hinter dem Dateiende schreiben: DOS laesst die Datei bis zur
           Stelle wachsen (WSWAP schreibt erst ab Stelle 4 und traegt den
           Kopf zuletzt ein), der Kernel setzt die Stelle aufs Ende */
        l32 = q->pos;
        r = FAPI(FAPI_NEWSIZE, KDATA, h, (unsigned)&l32, 0);
        if (FAILED(r))
            return errmap(r, E_DENIED);
        if (e && q->pos > e->size)
            e->size = q->pos;
        l32 = q->pos;
        if (FAILED(FAPI(FAPI_SEEK, KDATA, h, (unsigned)&l32, 0)))
            return E_DENIED;
    }
    if (q->count == 0 && q->fn == DEM_WRITE) {  /* Groesse setzen (DOS):  */
        l32 = q->pos;                           /* kuerzen/verlaengern    */
        r = FAPI(FAPI_NEWSIZE, KDATA, h, (unsigned)&l32, 0);
        if (FAILED(r))
            return errmap(r, E_DENIED);
        q->size = q->pos;
        if (e)
            e->size = q->pos;
        return 0;
    }
    k = q->count ? dem_xfer(t, q->fn == DEM_READ ? FAPI_READ : FAPI_WRITE, h,
                            q->buf, (unsigned)q->count, &err) : 0;
    if (q->fn == DEM_WRITE && k < q->count && q->pos + k > q->size) {
        /* Datentraeger voll: MS-DOS legt erst alle Cluster an und
           schreibt sonst gar nichts (DISK3.ASM ALLOCATE/WRTERR, CX = 0).
           BACKUP von MS-DOS fuellt den Rest danach selbst auf und
           zaehlt einen Teil-Schreibvorgang nicht mit (sonst fehlen in
           CONTROL.nnn Bytes). Also auf die alte Groesse zurueck. */
        l32 = q->size > q->pos ? q->size : q->pos;
        FAPI(FAPI_NEWSIZE, KDATA, h, (unsigned)&l32, 0);
        k = 0;
        err = 0;
    }
    if (k == 0 && q->count && err)
        return err;
    q->count = k;
    q->pos += k;
    if (q->fn == DEM_WRITE && q->pos > q->size)
        q->size = q->pos;
    if (e && q->fn == DEM_WRITE && q->pos > e->size)    /* fuer die anderen */
        e->size = q->pos;
    return 0;
}

static unsigned d_lock(int t, struct dem_req *q)
{
    struct shf __far *e;
    unsigned long o = q->pos, n = q->count;
    unsigned un = q->flags & DEM_F_UNLOCK, kh, k, r, err;
    if ((kh = dh(t, q->handle, &e)) == 0xFFFF)
        return E_HANDLE;
    k = (unsigned)q->handle & 0xFF;
    if (e) {                                /* Sperren der Box selbst */
        for (r = 0; r < NLK; r++) {
            struct shl __far *l = &RBX(t)->lk[r];
            if (!l->used || l->slot != k)
                continue;
            if (un) {                       /* entsperren: genau der  */
                if (l->ofs == o && l->len == n)     /* Bereich       */
                    break;
            } else if (o < l->ofs + l->len && l->ofs < o + n)
                return E_LOCK;              /* ueberschneidet sich    */
        }
        if (un && r == NLK)
            return E_LOCK;                  /* nicht gesperrt         */
        if (!un) {
            for (r = 0; r < NLK && RBX(t)->lk[r].used; r++)
                ;
            if (r == NLK)
                return E_SHTAB;             /* Sperrtabelle voll      */
        }
    }
    /* dann beim Kernel: auf NET-USE-Laufwerken sperrt der Server (andere
       Rechner sehen die Sperre), lokal immer 0 */
    rlk.ofs = o;
    rlk.len = n;
    rlk.flags = un ? LOCK_UNLOCK : 0;
    if (FAILED(err = FAPI(FAPI_FILELOCK, KDATA, kh, (unsigned)&rlk, 0)))
        return err == FAPI_ERR_LOCKED ? E_LOCK : E_DENIED;
    if (e) {
        if (un)
            RBX(t)->lk[r].used = 0;
        else {
            RBX(t)->lk[r].used = 1;
            RBX(t)->lk[r].slot = (unsigned char)k;
            RBX(t)->lk[r].ofs = o;
            RBX(t)->lk[r].len = n;
        }
    }
    return 0;
}

/* ---- Laufwerke ------------------------------------------------------------- */

/* Groesse einer CD in Bloecken: aus dem PVD (der Treiber kennt sie oft
   nicht), sonst vom Blockgeraet */
unsigned long __far dem_cd_size(int d)
{
    unsigned long n = kf_cd_blocks(d);
    if (!kf_cd_read(d, 16, 0, 0)) {
        unsigned char __far *b = MK_FP(DMABUF, 0);
        if (b[1] == 'C' && b[2] == 'D' && b[3] == '0')
            n = r32(b + 80);
    }
    return n;
}

static unsigned d_space(struct dem_req *q)
{
    unsigned spc = 1, sh = 9, cs, k = q->drive;
    unsigned long tot, fr, cl;
    l32 = 0;                                /* CD: nichts frei (MSCDEX) */
    if (FAILED(FAPI(FAPI_QFSINFO, KDATA, k, (unsigned)&fsi, 0)) ||
        (fsi.flags & FSI_NOMEDIA) ||        /* leer: "nicht bereit" (sonst
                                               "0 KB von 0 KB frei", der
                                               Datei-Manager von Windows
                                               zeigte ein leeres Laufwerk) */
        (fsi.bsize != 2048 &&
         FAILED(FAPI(FAPI_FREESPACE, KDATA, k, (unsigned)&l32, 0))))
        return E_NOTREADY;
    tot = fsi.bsize == 2048 ? dem_cd_size(k) << 2 : fsi.sectors;  /* CD: PVD */
    while ((tot >> (sh - 9)) > 0xFFF0UL && spc < 128) {
        spc <<= 1;
        sh++;
    }
    if (fat_geom(k, &cs, &cl) == 0 && cs && !(cs & (cs - 1))) {
        for (spc = cs, sh = 9; cs > 1; cs >>= 1)
            sh++;                           /* FAT: echte Aufteilung  */
        while (cl > 0xFFF0UL && spc < 128) {
            spc <<= 1;                      /* zu gross fuer 16 Bit:  */
            sh++;                           /* Cluster zusammenfassen */
            cl >>= 1;
        }
        tot = cl << (sh - 9);
    }
    fr = l32 >> sh;                         /* Bytes -> Cluster       */
    q->count = spc;
    q->size = tot >> (sh - 9);
    q->attr = 512;
    q->pos = fr > 0xFFF0UL ? 0xFFF0UL : fr;
    return 0;
}

/* Laufwerksinfo fuer die Laufwerks-IOCTL von BOXSHELL (4408h/4409h/440Dh
   ...): AT-DOS-Laufwerke wie MS-DOS lokale. 32 Byte nach buf: +0 Art (1
   fest, 2 Diskette, 3 Wechselplatte), +1 Sektoren je Cluster, +2 Bytes je Sektor, +4
   Cluster (DWORD), +8 Seriennummer, +12 Bezeichnung (11), +23
   Dateisystem (8), +31 Medienbyte. CD und leere Laufwerke -> Fehler */
static struct fapi_label di_lb;
static unsigned d_dinfo(int t, struct dem_req *q)
{
    unsigned char __far *o;
    unsigned d = q->drive, k, cs = 0, spc;
    unsigned long cl = 0;
    o = bl(t, q->buf);
    o[0] = 0;                               /* Art unbekannt          */
    if (FAILED(FAPI(FAPI_QFSINFO, KDATA, d, (unsigned)&fsi, 0)))
        return E_NOTREADY;
    if (fsi.bsize == 2048 || (fsi.flags & FSI_NOMEDIA)) {
        /* CD/leer: Fehler wie bisher, aber die Art bleibt sichtbar -
           eine leere Wechselplatte (ZIP, in 86Box auch beim Anlaufen)
           ist fuer 4408h weiter "wechselbar" wie unter MS-DOS */
        if (fsi.bsize != 2048)
            o[0] = (fsi.flags & FSI_REMDISK) ? 3 : (fsi.flags & FSI_REMOVABLE) ? 2 : 0;
        return E_NOTREADY;
    }
    if (fat_geom(d, &cs, &cl) || !cs) {
        cs = 1;
        cl = fsi.sectors;
    }
    for (spc = cs; cl > 0xFFF0UL && spc < 128; spc <<= 1)
        cl >>= 1;
    di_lb.label[0] = 0;
    di_lb.serial = 0;
    FAPI(FAPI_LABEL, KDATA, d, (unsigned)&di_lb, 0);
    o = bl(t, q->buf);
    for (k = 0; k < 32; k++)
        o[k] = 0;
    /* 1 fest, 2 Diskette, 3 Wechselplatte (ZIP: 4408h "wechselbar" wie
       MS-DOS ueber das Adaptec-BIOS - WINFILE zeigt das Wechselsymbol) */
    o[0] = (fsi.flags & FSI_REMDISK) ? 3 : (fsi.flags & FSI_REMOVABLE) ? 2 : 1;
    o[1] = (unsigned char)spc;
    w16(o + 2, fsi.bsize);
    w32(o + 4, cl);
    w32(o + 8, di_lb.serial);
    for (k = 0; k < 11; k++)                    /* Bezeichnung ohne Punkt */
        o[12 + k] = ' ';
    for (k = 0, d = 0; di_lb.label[k] && d < 11; k++)
        if (di_lb.label[k] != '.')
            o[12 + d++] = di_lb.label[k];
    for (k = 0; k < 8; k++)
        o[23 + k] = ' ';
    if (fsi.fsname[0] == 'F' && fsi.fsname[1] == 'A' && fsi.fsname[2] == 'T') {
        o[23] = 'F', o[24] = 'A', o[25] = 'T';
        if (fsi.fsname[3] == '3')
            o[26] = '3', o[27] = '2';
        else if (cl >= 4085)
            o[26] = '1', o[27] = '6';
        else
            o[26] = '1', o[27] = '2';
    }
    o[31] = o[0] >= 2 ? 0xF0 : 0xF8;
    return 0;
}

/* Laufwerkssperre wie MS-DOS 7 (440Dh 4Ah): die Box oeffnet das Laufwerk
   wie CHKDSK mit DRV_OPEN_LOCK (nur ohne fremde offene Dateien dort, sonst
   05h), mit DRV_OPEN_OWNER darf sie selbst weiter Dateien benutzen;
   beim Entsperren oder Ende der Box schliesst der Kernel es, verwirft die
   Caches und haengt neu ein (of_release). */
static unsigned d_dlock(int t, struct dem_req *q)
{
    unsigned d = q->drive, r;
    if (d >= 26)
        return 0x0F;
    if (q->flags & 1) {
        if (RBX(t)->dlk[d])
            FAPI(FAPI_CLOSE, KDATA, RBX(t)->dlk[d] - 1, 0, 0);
        RBX(t)->dlk[d] = 0;
        return 0;
    }
    if (RBX(t)->dlk[d])
        return 0;
    if (FAILED(FAPI(FAPI_QFSINFO, KDATA, d, (unsigned)&fsi, 0)))
        return E_NOTREADY;
    if (fsi.bsize != 512 || (fsi.flags & FSI_REMOTE))
        return E_DENIED;
    r = FAPI(FAPI_OPENDRIVE, KDATA, d, DRV_OPEN_LOCK | DRV_OPEN_LBA | DRV_OPEN_OWNER, 0);
    if (FAILED(r))
        return r == FAPI_ERR_NOTREADY ? E_NOTREADY : E_DENIED;
    RBX(t)->dlk[d] = r + 1;
    RBX(t)->dfl[d] = (unsigned char)((fsi.fsname[3] == '3' ? 1 : 0) |
                                     (fsi.sectors > 0xFFFFUL ? 2 : 0));
    return 0;
}

/* INT 25h/26h und 7305h wie MS-DOS 7: lesen frei, schreiben nur mit
   Sperre (sonst 0300h Schreibschutz); FAT32 nur ueber 7305h (flags 2),
   alte Form CX/DX nur bis 32 MB (0207h). Fehler sind INT-25h-Codes:
   8002h nicht bereit, 0408h nicht gefunden, 080Ch Lese-, 040Ah
   Schreibfehler. Mit Sperre ueber das Laufwerks-Handle (Platten-Cache),
   ohne Sektor fuer Sektor ueber fat_abssec. */
static unsigned d_abs(int t, struct dem_req *q, int wr)
{
    unsigned long lba = q->pos, n = q->count, buf = q->buf, tot;
    unsigned char __far *s = MK_FP(DMABUF, 0), __far *o;
    unsigned k, h, c, err, fl;
    int r;
    if (!n)
        return 0;
    if (q->drive < 26 && (h = RBX(t)->dlk[q->drive]) != 0) {
        fl = RBX(t)->dfl[q->drive];
        if ((fl & 1) && !(q->flags & 2))
            return 0x0207;
        if ((fl & 2) && !(q->flags & 3))
            return 0x0207;
        h--;
        l32 = lba;
        if (FAILED(FAPI(FAPI_SEEK, KDATA, h, (unsigned)&l32, 0)))
            return 0x8002;
        for (; n; n -= c, buf += (unsigned long)c << 9) {
            c = n > 64 ? 64 : (unsigned)n;
            k = dem_xfer(t, wr ? FAPI_WRITE : FAPI_READ, h, buf, c << 9, &err);
            if (k != c << 9)
                return k || !err ? 0x0408 : (wr ? 0x040A : 0x080C);
        }
        if (wr)                             /* Dateisystem-Caches weg:   */
            vol_reset(q->drive);            /* Box liest danach Dateien  */
        return 0;
    }
    if (wr)
        return 0x0300;
    if (!(q->flags & 2)) {                  /* FAT32: nur 7305h (Win 95)  */
        if (FAILED(FAPI(FAPI_QFSINFO, KDATA, q->drive, (unsigned)&fsi, 0)))
            return 0x8002;
        if (fsi.fsname[3] == '3')
            return 0x0207;
    }
    for (; n; n--, lba++, buf += 512) {
        r = fat_abssec(q->drive, lba, &tot);
        if (r == -1)
            return 0x8002;
        if (!(q->flags & 3) && tot > 0xFFFFUL)
            return 0x0207;
        if (r == -2)
            return 0x0408;
        if (r)
            return 0x080C;
        o = bl(t, buf);
        for (k = 0; k < 512; k++)
            o[k] = s[k];
    }
    return 0;
}

/* ---- Lange Namen (INT 21h 71xxh des Box-Kerns, 22.11.2026) -------------
   Die Box arbeitet weiter mit kurzen Pfaden; DEM_LPATH loest einen Pfad
   mit langen Teilen in die kurze und die lange Form auf, DEM_LFIND sucht
   mit langem Muster. Der Kernel gibt der Box sonst nur 8.3 (task.lfn 0) -
   fuer jeden Verzeichniseintrag hier beide Namen ueber FINDENTRY mit
   umgeschaltetem Merker. */
static char lp_in[DEM_PATHLEN], lp_c[256], lp_sn[13];

static unsigned fe(unsigned i, const char *dir, int lng)
{
    unsigned char o = task[current].lfn;
    unsigned r;
    task[current].lfn = (unsigned char)lng;
    r = FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, i, (unsigned)dir);
    task[current].lfn = o;
    return r;
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b && up((unsigned char)*a) == up((unsigned char)*b))
        a++, b++;
    return !*a && !*b;
}

/* Muster (* ?) gegen Namen ohne Gross/klein; "*.*" wie "*" (Windows) */
static int wm(const char *p, const char *n)
{
    const char *sp = 0, *sn = 0;
    if (p[0] == '*' && p[1] == '.' && p[2] == '*' && !p[3])
        p += 2;
    while (*n) {
        if (*p == '*') {
            sp = ++p;
            sn = n;
        } else if (*p == '?' || (*p && up((unsigned char)*p) == up((unsigned char)*n))) {
            p++;
            n++;
        } else if (sp) {
            p = sp;
            n = ++sn;
        } else
            return 0;
    }
    while (*p == '*' || (*p == '.' && p[1] == '*'))
        p++;
    return !*p;
}

/* kurzer Name zu Eintrag i -> lp_sn (gross), danach steht in de der lange */
static unsigned both(unsigned i, const char *dir)
{
    unsigned r = fe(i, dir, 0), k;
    if (FAILED(r))
        return r;
    for (k = 0; k < 12 && de.name[k]; k++)
        lp_sn[k] = (char)up((unsigned char)de.name[k]);
    lp_sn[k] = 0;
    return fe(i, dir, 1);
}

static unsigned app(char *p, const char *n)
{
    unsigned l = 0, k;
    while (p[l])
        l++;
    if (p[l - 1] != '\\')
        p[l++] = '\\';
    for (k = 0; n[k]; k++) {
        if (l >= DEM_PATHLEN - 1)
            return 1;
        p[l++] = n[k];
    }
    p[l] = 0;
    return 0;
}

static unsigned d_lpath(struct dem_req *q)
{
    unsigned i, k, r, last;
    const char *p;
    for (k = 0; k < DEM_PATHLEN - 1 && (lp_in[k] = q->path[k]) != 0; k++)
        ;
    lp_in[k] = 0;
    if (lp_in[1] != ':' || lp_in[2] != '\\')
        return E_NOPATH;
    q->path[3] = q->path2[3] = 0;
    q->path2[0] = q->path[0];
    q->path2[1] = ':';
    q->path2[2] = '\\';
    q->attr = 0x10;
    q->size = 0;
    q->time = q->date = 0;
    for (p = lp_in + 3; *p; ) {
        for (k = 0; *p && *p != '\\'; p++)
            if (k < 255)
                lp_c[k++] = *p;
        lp_c[k] = 0;
        while (*p == '\\')
            p++;
        last = !*p;
        if (!k)
            continue;
        if (has_wild(lp_c)) {
            if (!last)
                return E_NOPATH;
            goto miss;
        }
        for (i = 0; ; i++) {
            r = both(i, q->path);
            if (FAILED(r)) {
                if (r == FAPI_ERR_NOTREADY)
                    return E_NOTREADY;
                if (!last)
                    return E_NOPATH;
                goto miss;
            }
            if (ieq(lp_c, de.name) || ieq(lp_c, lp_sn))
                break;
        }
        if (!last && !(de.attr & 0x10))
            return E_NOPATH;
        if (app(q->path, lp_sn) || app(q->path2, de.name))
            return E_NOPATH;
        q->attr = de.attr;
        q->size = de.size;
        q->time = de.time;
        q->date = de.date;
    }
    if (!*p)
        return 0;
miss:                                       /* letzter Teil fehlt        */
    if (app(q->path, lp_c) || app(q->path2, lp_c))
        return E_NOPATH;
    return E_NOFILE;
}

static unsigned d_lfind(struct dem_req *q)
{
    unsigned i, k, last = 2, r;
    unsigned char al = (unsigned char)q->attr, rq2 = (unsigned char)(q->attr >> 8), a;
    for (k = 0; k < DEM_PATHLEN - 1 && (lp_in[k] = q->path[k]) != 0; k++)
        if (lp_in[k] == '\\')
            last = k;
    lp_in[k] = 0;
    for (k = 0; (lp_c[k] = lp_in[last + 1 + k]) != 0 && k < 254; k++)
        ;
    lp_c[k] = 0;
    lp_in[last == 2 ? 3 : last] = 0;
    if (!lp_c[0]) {
        lp_c[0] = '*';
        lp_c[1] = 0;
    }
    for (i = g16(q->find); ; i++) {
        r = fe(i, lp_in, 1);
        if (FAILED(r))
            return r == FAPI_ERR_NOTREADY ? E_NOTREADY : E_NOMORE;
        a = de.attr;
        if ((a & 0x08) || (a & ~al & 0x16) || (a & rq2) != rq2)
            continue;
        for (k = 0; k < DEM_PATHLEN - 1 && (q->path2[k] = de.name[k]) != 0; k++)
            ;
        q->path2[k] = 0;
        if (FAILED(both(i, lp_in)))
            return E_NOMORE;
        if (lp_sn[0] == '.')                /* "." und "..": lfn 1 nennt  */
            for (k = 0; (q->path2[k] = lp_sn[k]) != 0; k++)  /* sonst den */
                ;                           /* Namen des Verzeichnisses  */
        if (!wm(lp_c, q->path2) && !wm(lp_c, lp_sn))
            continue;
        to_fcb(lp_sn, (char *)q->name);
        p16(q->find, i + 1);
        q->attr = a;
        q->time = de.time;
        q->date = de.date;
        q->size = de.size;
        return 0;
    }
}

static unsigned d_fclus(struct dem_req *q)
{
    unsigned h = FAPI(FAPI_OPEN, KDATA, (unsigned)q->path, OPEN_READ, 0);
    if (FAILED(h))
        return errmap(h, nf(q->path));
    q->pos = kf_hfirst(h);
    FAPI(FAPI_CLOSE, KDATA, h, 0, 0);
    return 0;
}

/* ---- Netz in der Box (Redirector-Antworten, docs/DOSKERN.md) ------------- */

int __far ufs_unc(int d, char *out, unsigned max);

/* Rechnername, Domaene, NET-USE-Laufwerke und -Drucker (DEM_NETINFO).
   Name aus COMPUTERNAME der Box (geerbt von CMD/LOGON), sonst aus der
   ersten Karte (DHCP/HOSTNAME); NET_IFINFO legt seine Antwort in
   path/path2 ab (zusammen 522 Byte, die Struktur ist kleiner). */
static unsigned d_netinfo(struct dem_req *q)
{
    struct fapi_netif *ni = (struct fapi_netif *)q->path;
    unsigned k, r;
    char hn[32], dn[32];
    if (q->drive != 0xFFFF) {
        if (q->drive >= 26 && (q->drive < UFS_LPT || q->drive > UFS_LPT + 2))
            return 0x0F;
        return ufs_unc(q->drive, q->path, sizeof(q->path)) ? 0x0F : 0;
    }
    q->pos = 0;
    q->flags = 0;
    for (k = 0; k < 26; k++)
        if (!ufs_unc(k, hn, sizeof(hn)))
            q->pos |= 1UL << k;
    for (k = 0; k < 3; k++)
        if (!ufs_unc(UFS_LPT + k, hn, sizeof(hn)))
            q->flags |= 1 << k;
    hn[0] = dn[0] = 0;
    q->act = q->pos || q->flags;
    ni->ifc = 0;
    if (!FAILED(FAPI(FAPI_NET, KDATA, NET_IFINFO, (unsigned)ni, sizeof(*ni)))) {
        q->act = 1;
        for (k = 0; k < 31 && ni->host[k]; k++)
            hn[k] = ni->host[k];
        hn[k] = 0;
        for (k = 0; k < 31 && ni->domain[k]; k++)
            dn[k] = ni->domain[k];
        dn[k] = 0;
    }
    r = FAPI(FAPI_GETENV, KDATA, (unsigned)"COMPUTERNAME", (unsigned)q->path, 32);
    if (FAILED(r) || !q->path[0])
        for (k = 0; (q->path[k] = hn[k]) != 0; k++)
            ;
    r = FAPI(FAPI_GETENV, KDATA, (unsigned)"USERDOMAIN", (unsigned)q->path2, 32);
    if (FAILED(r) || !q->path2[0])
        for (k = 0; (q->path2[k] = dn[k]) != 0; k++)
            ;
    return 0;
}

/* ---- Verteiler ----------------------------------------------------------- */

/* Auftrag q (im Kernel) fuer die Box t erledigen -> q->err (DOS-Fehler,
   0 = ok). Die Kernelsperre (BOX_LOCK) haelt der Eingang. */
unsigned __far dem_call(int t, struct dem_req *q)
{
    unsigned r;
    q->err = 0;
    if (!rb_sel)
        return q->err = E_DENIED;
    switch (q->fn) {
    case DEM_OPEN:      r = d_open(t, q); break;
    case DEM_CLOSE:     r = d_close(t, q); break;
    case DEM_READ:
    case DEM_WRITE:     r = d_rw(t, q); break;
    case DEM_SIZE: {
        struct shf __far *e;
        r = 0;
        if (dh(t, q->handle, &e) != 0xFFFF && e && e->size > q->size)
            q->size = e->size;              /* ein anderer schrieb    */
        break;
    }
    case DEM_LOCK:      r = d_lock(t, q); break;
    case DEM_MKDIR:
    case DEM_RMDIR:
        r = FAPI(q->fn == DEM_RMDIR ? FAPI_RMDIR : FAPI_MKDIR, KDATA, (unsigned)q->path, 0, 0);
        if (!FAILED(r))
            r = 0;
        else if (q->fn == DEM_MKDIR && r == FAPI_ERR_EXIST)
            r = E_DENIED;                   /* MKDIR wie MS-DOS: 05h */
        else if (q->fn == DEM_RMDIR && r == FAPI_ERR_INUSE)
            r = 0x10;                       /* aktuelles Verzeichnis */
        else
            r = errmap(r, q->fn == DEM_RMDIR ? (no_dir(q->path) ? E_NOPATH : E_DENIED)
                                             : E_NOPATH);
        break;
    case DEM_CHDIR:
        r = stat_path(q->path) || !(de.attr & 0x10) ? E_NOPATH : 0;
        break;
    case DEM_GETATTR:
        if (stat_path(q->path)) {
            r = nf(q->path);
            break;
        }
        q->attr = de.attr;
        q->size = de.size;
        q->time = de.time;
        q->date = de.date;
        r = 0;
        break;
    case DEM_SETATTR:
        fin.mask = 1;
        fin.attr = q->attr & 0x27;
        r = FAPI(FAPI_SETFINFO, KDATA, (unsigned)q->path, (unsigned)&fin, 0);
        r = FAILED(r) ? errmap(r, nf(q->path)) : 0;
        break;
    case DEM_RENAME:
        if (has_wild(q->path) || has_wild(q->path2)) {
            r = ren_wild(q);
            break;
        }
        r = FAPI(FAPI_MOVE, KDATA, (unsigned)q->path, (unsigned)q->path2, 0);
        if (r == FAPI_ERR_EXIST)
            r = E_DENIED;                   /* Ziel gibt es: 05h     */
        else
            r = FAILED(r) ? errmap(r, nf(q->path)) : 0;
        break;
    case DEM_DELETE:
        if (has_wild(q->path)) {
            r = del_wild(q);
            break;
        }
        r = FAPI(FAPI_DELETE, KDATA, (unsigned)q->path, 0, 0);
        r = FAILED(r) ? errmap(r, nf(q->path)) : 0;
        break;
    case DEM_FINDFIRST: r = find_first(t, q); break;
    case DEM_FINDNEXT:  r = find_next(t, q); break;
    case DEM_SPACE:     r = d_space(q); break;
    case DEM_DINFO:     r = d_dinfo(t, q); break;
    case DEM_ABSREAD:   r = d_abs(t, q, 0); break;
    case DEM_ABSWRITE:  r = d_abs(t, q, 1); break;
    case DEM_DLOCK:     r = d_dlock(t, q); break;
    case DEM_LPATH:     r = d_lpath(q); break;
    case DEM_LFIND:     r = d_lfind(q); break;
    case DEM_FCLUS:     r = d_fclus(q); break;
    case DEM_NETINFO:   r = d_netinfo(q); break;
    case DEM_LABEL: {                           /* Bezeichnung setzen    */
        static char lb_n[12];
        unsigned k = 2, j = 0;
        if (q->path[k] == '\\')
            k++;
        for (; q->path[k] && j < 11; k++)       /* "BACKUP.001" -> 11    */
            if (q->path[k] != '.')               /* Zeichen ohne Punkt    */
                lb_n[j++] = q->path[k];
            else
                while (j < 8)
                    lb_n[j++] = ' ';
        while (j && lb_n[j - 1] == ' ')
            j--;
        lb_n[j] = 0;
        r = FAPI(FAPI_LABEL, KDATA, (q->path[0] & 0x1F) - 1, (unsigned)&fn_lb,
                 (unsigned)lb_n);
        r = FAILED(r) ? E_DENIED : 0;
        break;
    }
    case DEM_MEDIA:                             /* 47h: Medium da?       */
        r = FAILED(FAPI(FAPI_QFSINFO, KDATA, q->drive, (unsigned)&fsi, 0)) ||
            (fsi.flags & FSI_NOMEDIA) ? E_NOTREADY : 0;
        break;
    default:            r = 1;              /* unbekannt (01h)       */
    }
    return q->err = r;
}

/* Kontext je Task (Phase 6, kernel.c ctx_take) */
const struct ctxblk dem_ctx[] = {
    { tmpl, sizeof(tmpl) }, { &de, sizeof(de) }, { &fsi, sizeof(fsi) },
    { &fin, sizeof(fin) }, { &l32, sizeof(l32) },
    { st_dir, sizeof(st_dir) }, { st_fcb, sizeof(st_fcb) }, { np_dir, sizeof(np_dir) },
    { fn_fcb, sizeof(fn_fcb) }, { fn_tp, sizeof(fn_tp) }, { fn_dir, sizeof(fn_dir) },
    { &fn_lb, sizeof(fn_lb) }, { ff_dir, sizeof(ff_dir) },
    { op_dir, sizeof(op_dir) }, { op_fcb, sizeof(op_fcb) }, { op_one, sizeof(op_one) },
    { &di_lb, sizeof(di_lb) }, { &rlk, sizeof(rlk) },
    { lp_in, sizeof(lp_in) }, { lp_c, sizeof(lp_c) }, { lp_sn, sizeof(lp_sn) }
};
const unsigned dem_nctx = sizeof(dem_ctx) / sizeof(dem_ctx[0]);
