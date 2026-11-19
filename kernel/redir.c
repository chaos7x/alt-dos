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
int __far fat_geom(int d, unsigned *spc, unsigned long *clusters);
int __far kf_hmeta(unsigned h, unsigned what, unsigned attr, unsigned date, unsigned time);
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
/* (sgen: Generation je Platz, im DTA:  */
                                        /* Byte 16; Platz neu belegt ->  */
                                        /* alte Suche endet)             */
static char ff_dir[80];

/* DOS haelt den Suchzustand im DTA (21 Byte, gehoeren dem Redirector):
   0 Laufwerk|80h, 1-11 Muster, 12 Attribute, 13/14 Eintrag, 15 Platz,
   16 Generation, 17/18 Pruefsumme des zuletzt gefundenen Namens */
static unsigned h11(const char *f)
{
    unsigned h = 0, k;
    for (k = 0; k < 11; k++)
        h = (h << 5) + h + (unsigned char)f[k];
    return h;
}

/* ---- SHARE (18.11.2026, Wunsch des Nutzers) -------------------------------
   Wie SHARE.EXE, aber im Redirector: oeffnet ein Programm der Box eine
   Datei, die die Box schon offen hat, teilen sich beide SFTs denselben
   Handle des Kernels (die Stelle steht je SFT, READ/WRITE setzen sie
   vorher) - die Groesse gibt es nur einmal, nichts laeuft auseinander.
   Freigabemodi (Bits 4-6 des Modus) gegen die anderen Oeffnungen der Box,
   Bereichssperren (110Ah) je Datei. Zwischen Boxen und FAPI-Programmen
   gelten die Regeln des Kernels. SFT+0Bh = E000h | Platz (sonst Handle
   + 1, wenn die Tabelle voll ist: dann ohne Freigabepruefung). */
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
/* Was je Box bleibt (Suchverzeichnisse, SHARE) liegt nicht in DGROUP
   (dort waren nur noch 660 Byte frei), sondern in einem Kernelsegment
   fuer alle Boxen, angelegt beim ersten Start einer Box */
struct rbx {
    char           sdir[4][80];         /* Verzeichnisse laufender Suchen */
    unsigned char  sgen[4], snext;      /* Generation je Platz, naechster */
    struct shf     sh[NSH];
    struct shl     lk[NLK];
};
static unsigned rb_sel;
#define RBX(t)  ((struct rbx __far *)MK_FP(rb_sel, (unsigned)(t) * sizeof(struct rbx)))

/* oem: Kennung aus INT 21h 30h (BH) - FFh MS-DOS/PC-DOS, FDh FreeDOS.
   MS-DOS laesst bei einem Redirector einiges dem Redirector (Aktions-
   code von 6Ch); FreeDOS erledigt es selbst. */
static unsigned char ms_dos[NTASK];
void __far redir_sda(int t, unsigned seg, unsigned off, unsigned oem)
{
    unsigned i;
    sda_seg[t] = seg;
    sda_off[t] = off;
    ms_dos[t] = oem != 0xFD;
    if (!rb_sel)
        rb_sel = kseg_alloc((unsigned long)NTASK * sizeof(struct rbx));
    if (!rb_sel)
        return;
    for (i = 0; i < NSH; i++)           /* neue Box: nichts offen         */
        RBX(t)->sh[i].h1 = 0;
    for (i = 0; i < NLK; i++)
        RBX(t)->lk[i].used = 0;
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

static unsigned long cd_size(int d);
static void get_fn(int t, unsigned x, char *d)
{
    unsigned char __far *s = sda(t, x);
    unsigned i, o = 0, k;
    for (i = 0; i < 127 && s[i]; i++) {
        if (s[i] == ' ') {              /* DOS denkt in 8+3-Feldern:      */
            for (k = i; k < 127 && s[k] == ' '; k++)   /* "LW .LLD" ist  */
                ;                       /* LW.LLD (Lotus-Works-INSTALL);  */
            if (k >= 127 || !s[k] || s[k] == '.' || s[k] == '\\') {
                i = k - 1;              /* Leerzeichen vor Punkt, Trenner */
                continue;               /* und Ende fallen weg             */
            }
        }
        d[o++] = s[i];
    }
    if (o > 3 && d[o - 1] == '\\')      /* MS-DOS fragt "C:\T3\" (1105h,  */
        o--;                            /* XCOPY) - wie "C:\T3"           */
    d[o] = 0;
    if (d[0] == '\\' && d[1] == '\\' && d[3] == '.' && d[4] == '\\' &&
        d[6] == '.') {                  /* CD-Laufwerk wie MSCDEX: Stamm   */
        d[1] = ':';                     /* "\\E.\A." -> "E:", Rest dahinter */
        d[0] = d[2];
        for (i = 7, k = 2; d[i]; )
            d[k++] = d[i++];
        if (k == 2)
            d[k++] = '\\';
        d[k] = 0;
    }
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

/* Fehlt das Verzeichnis, in dem p liegen soll? MS-DOS unterscheidet:
   Datei fehlt 02h, Verzeichnis fehlt 03h (BIOSCMP: der Redirector gab
   immer 02h bzw. bei der Suche 12h) */
static char np_dir[80];                         /* Kontext */
static int no_dir(const char *p)
{
    split(p, np_dir);
    if (!np_dir[3])                         /* "C:" gibt es immer       */
        return 0;
    return stat_path(np_dir) || !(de.attr & 0x10);
}
static unsigned nf(const char *p) { return no_dir(p) ? E_NOPATH : E_NOFILE; }

static unsigned long phash(const char *p)
{
    unsigned long h = 5381;
    while (*p)
        h = ((h << 5) + h) ^ up(*p++);
    return h;
}

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

/* SFT+0Bh -> Handle des Kernels (FFFFh = keiner), *e = geteilter Eintrag */
static unsigned sft_h(int t, unsigned v, struct shf __far **e)
{
    *e = 0;
    if ((v & 0xFF00) == SH_MARK) {
        if ((v & 0xFF) >= NSH || !RBX(t)->sh[v & 0xFF].h1)
            return 0xFFFF;
        *e = &RBX(t)->sh[v & 0xFF];
        return (*e)->h1 - 1;
    }
    return v - 1;
}

/* ---- SFT -------------------------------------------------------------------- */

static char sf_dir[80];                         /* Kontext */
#define dir sf_dir
static void sft_fill(int t, unsigned es, unsigned di, unsigned h, unsigned mode,
                     const char *path)
{
    unsigned char __far *s = bp(t, es, di);
    const char *n;
    unsigned k;
    w16(s + 0x02, (r16(s + 0x02) & 0x8000) | mode);  /* sf_isFCB (MS-DOS
                                           setzt es vor dem Oeffnen) bleibt */
    s[0x04] = de.attr;
    w16(s + 0x05, 0x8040 | (up(path[0]) - 'A'));
    w32(s + 0x07, 0);
    w16(s + 0x0B, h + 1);
    w16(s + 0x0D, de.time);
    w16(s + 0x0F, de.date);
    w32(s + 0x11, de.size);
    w32(s + 0x15, 0);
    k = redir_psp(t);                   /* sf_PID = aktueller PSP: MS-DOS */
    s = bp(t, es, di);                  /* setzt es bei Netzdateien nicht, */
    w16(s + 0x31, k);                   /* prueft es aber bei FCBs        */
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

/* Standard-Handle h der Box (umgeleitet aus CMD, ATBOX.SYS-Geraete
   ATIN$/ATOUT$/ATERR$): h = 0 lesen, 1/2 schreiben -> Bytes */
unsigned __far redir_std(int t, unsigned h, unsigned seg, unsigned off, unsigned n)
{
    unsigned err, done = 0, r, idle = 0;
    unsigned long lin = ((unsigned long)seg << 4) + off;
    if (!h)                                 /* lesen: was da ist          */
        return xfer(t, FAPI_READ, 0, lin, n, &err);
    while (done < n) {                      /* schreiben: alles (Pipe voll */
        r = xfer(t, FAPI_WRITE, h, lin + done, n - done, &err);   /* -> warten) */
        done += r;
        if (err)
            break;
        if (r)
            idle = 0;
        else if (++idle > 500)              /* 5 s nichts: aufgeben       */
            break;
        else
            kf_box_ctl(BOX_SLEEP, 1);
    }
    return done;
}

/* ---- Suche -------------------------------------------------------------------- */

/* naechsten passenden Eintrag ab Suchzustand sdb (in der Box) */
static char fn_fcb[11], fn_tp[11], fn_dir[80];  /* Kontext */
static struct fapi_label fn_lb;
#define fcb fn_fcb
#define tp fn_tp
#define lb fn_lb
static int find_next(int t, unsigned char __far *sdb_unused, unsigned seg, unsigned off)
{
    unsigned char __far *s = bp(t, seg, off);
    unsigned n = r16(s + 13), slot = s[15] & 3, k, lh = r16(s + 17);
    unsigned char mask = s[12], a;
    (void)sdb_unused;
    if (n == 0xFFFF)
        return E_NOMORE;                    /* nach der Bezeichnung      */
    if (mask != 0x08 && s[16] != RBX(t)->sgen[slot])
        return E_NOMORE;                    /* Platz inzwischen anders   */
    for (k = 0; k < 79 && (fn_dir[k] = RBX(t)->sdir[slot][k]) != 0; k++)
        ;                                   /* Pfad nach DGROUP (FAPI)   */
    fn_dir[k] = 0;
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
        !FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, n - 1, (unsigned)fn_dir))) {
        to_fcb(de.name, fcb);
        if (h11(fcb) != lh)
            n--;
    }
    for (;; n++) {
        if (FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, n, (unsigned)fn_dir)))
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
    s = bp(t, seg, off);
    w16(s + 13, n + 1);
    w16(s + 17, h11(fcb));
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
    unsigned slot, k, i;
    get_fn(t, 0x9E, p1);
    n = split(p1, ff_dir);
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
    s = sda(t, 0x19E);
    s[0] = (unsigned char)(0x80 | (up(p1[0]) - 'A'));
    for (k = 0; k < 11; k++)
        s[1 + k] = tmpl[k];
    s[12] = *sda(t, 0x24D);
    w16(s + 13, 0);
    s[15] = (unsigned char)slot;
    s[16] = RBX(t)->sgen[slot];
    for (k = 17; k < 21; k++)
        s[k] = 0;
    k = find_next(t, 0, sda_seg[t], sda_off[t] + 0x19E);
    if (k == E_NOMORE && no_dir(p1))
        k = E_NOPATH;                       /* Verzeichnis fehlt: 03h    */
    return k;                           /* ohne Treffer 12h wie FreeDOS
                                           (fatdir.c) und MS-DOS - Lotus
                                           Works INSTALL hielt 02h fuer
                                           einen Lesefehler */
}

/* ---- Verteiler ---------------------------------------------------------------- */

static char op_dir[80], op_fcb[11], op_one[96];  /* Kontext */
#define dir op_dir
#define fcb op_fcb
#define one op_one

/* REN mit Platzhaltern (p1, p2 aus der SDA). FreeDOS loest sie selbst
   auf, MS-DOS reicht sie an den Redirector durch: je Treffer von p1 den
   neuen Namen nach der FCB-Regel bilden (? = Zeichen der Quelle an
   dieser Stelle, sonst Zeichen des Musters), im Zielverzeichnis von p2. */
static void fcb_name(const char *f, char *o)    /* "NAME    EXT" -> o */
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
static int ren_wild(void)
{
    char t2[11], nn[11];
    const char *n;
    unsigned i = 0, hit = 0, k, l, r, err = 0;
    to_fcb(split(p2, ff_dir), t2);              /* Ziel: ff_dir + Muster  */
    n = split(p1, dir);
    to_fcb(n, tmpl);
    while (!FAILED(FAPI(FAPI_FINDENTRY, KDATA, (unsigned)&de, i++, (unsigned)dir))) {
        to_fcb(de.name, fcb);
        if ((de.attr & 0x1E) || !fcb_match(tmpl, fcb))
            continue;
        for (k = 0; k < 11; k++)
            nn[k] = t2[k] == '?' ? fcb[k] : t2[k];
        for (l = 0; dir[l] && l < 80; l++)
            one[l] = dir[l];
        if (one[l - 1] != '\\')
            one[l++] = '\\';
        for (k = 0; de.name[k] && l < 95; k++)
            one[l++] = de.name[k];
        one[l] = 0;
        for (l = 0; ff_dir[l] && l < 80; l++)
            p2[l] = ff_dir[l];
        if (p2[l - 1] != '\\')
            p2[l++] = '\\';
        fcb_name(nn, p2 + l);
        r = FAPI(FAPI_MOVE, KDATA, (unsigned)one, (unsigned)p2, 0);
        if (FAILED(r))
            err = r == FAPI_ERR_EXIST ? E_DENIED : errmap(r, E_DENIED);
        else
            hit++;
    }
    if (!hit)
        return err ? err : nf(p1);
    return 0;                                   /* wie MS-DOS: Teilerfolg */
}
/* Aktionscode von 6Ch (1 geoeffnet, 2 angelegt, 3 ersetzt): FreeDOS
   nimmt ihn aus CX, MS-DOS gibt dem Aufrufer bei einem Redirector nur AX
   zurueck - dort steht er in dessen Registerrahmen (user_SS:user_SP aus
   der SDA, +264h bei MS-DOS 6.22, CX bei +4) */
static void ext_act(struct v86_frame *f, int t, unsigned act)
{
    unsigned sp, ss;
    CX = act;
    if (!ms_dos[t])
        return;
    sp = r16(sda(t, 0x264));
    ss = r16(sda(t, 0x266));
    w16(bp(t, ss, sp + 4), act);
}

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
        if (fn == 1) {                          /* aktuelles Verzeichnis */
            unsigned char __far *c;             /* des Laufwerks (CDS bei */
            s = sda(t, 0x282);                  /* SDA+282h): 10h wie DOS */
            c = bp(t, r16(s + 2), r16(s));
            for (k = 0; k < 67 && p1[k] && up(c[k]) == up(p1[k]); k++)
                ;
            if (k < 67 && !p1[k] && !c[k])
                return 0x10;
        }
        r = FAPI(fn == 1 ? FAPI_RMDIR : FAPI_MKDIR, KDATA, (unsigned)p1, 0, 0);
        if (!FAILED(r))
            return 0;
        if (fn == 3 && r == FAPI_ERR_EXIST)
            return E_DENIED;                    /* MKDIR wie MS-DOS: 05h */
        if (fn == 1 && r == FAPI_ERR_INUSE)
            return 0x10;                        /* aktuelles Verzeichnis */
        return errmap(r, fn == 1 ? (no_dir(p1) ? E_NOPATH : E_DENIED) : E_NOPATH);
    case 0x05:                                  /* CHDIR: gibt es das?   */
        get_fn(t, 0x9E, p1);
        if (stat_path(p1) || !(de.attr & 0x10))
            return E_NOPATH;
        return 0;
    case 0x06: {                                /* CLOSE                 */
        struct shf __far *e;
        s = bp(t, es, di);
        cnt = r16(s);
        k = r16(s + 0x0B);
        h = sft_h(t, k, &e);
        if (cnt)
            w16(s, --cnt);
        if (cnt == 0 && h != 0xFFFF) {
            if (r16(s + 5) & 0x4000)            /* Datum per 5701h gesetzt */
                kf_hmeta(h, 1, 0, r16(s + 0x0F), r16(s + 0x0D));
            if (e) {
                sh_add(e, r16(s + 2), -1);
                if (--e->refs == 0) {           /* letzte Oeffnung: zu,   */
                    FAPI(FAPI_CLOSE, KDATA, h, 0, 0);
                    e->h1 = 0;                  /* Sperren weg            */
                    for (r = 0; r < NLK; r++)
                        if (RBX(t)->lk[r].slot == (k & 0xFF))
                            RBX(t)->lk[r].used = 0;
                }
            } else
                FAPI(FAPI_CLOSE, KDATA, h, 0, 0);
            w16(bp(t, es, di + 0x0B), 0);
        }
        return 0;
    }
    case 0x0A: {                                /* LOCK/UNLOCK: BL = 1    */
        struct shf __far *e;                          /* entsperren, DS:DX ->   */
        unsigned long o, n;                     /* Stelle, Laenge (DWORD) */
        unsigned char __far *pb;
        k = r16(bp(t, es, di + 0x0B));
        if (sft_h(t, k, &e) == 0xFFFF)
            return E_HANDLE;
        if (!e)                                 /* ohne Tabellenplatz     */
            return 0;
        pb = bp(t, (unsigned)f->r.ds, DX);
        o = r32(pb);
        n = r32(pb + 4);
        k &= 0xFF;
        for (r = 0; r < NLK; r++) {
            struct shl __far *l = &RBX(t)->lk[r];
            if (!l->used || l->slot != k)
                continue;
            if (*(unsigned char *)&f->ebx) {    /* entsperren: genau der  */
                if (l->ofs == o && l->len == n) {   /* Bereich           */
                    l->used = 0;
                    return 0;
                }
            } else if (o < l->ofs + l->len && l->ofs < o + n)
                return 0x21;                    /* ueberschneidet sich    */
        }
        if (*(unsigned char *)&f->ebx)
            return 0x21;                        /* nicht gesperrt         */
        for (r = 0; r < NLK; r++)
            if (!RBX(t)->lk[r].used) {
                RBX(t)->lk[r].used = 1;
                RBX(t)->lk[r].slot = (unsigned char)k;
                RBX(t)->lk[r].ofs = o;
                RBX(t)->lk[r].len = n;
                return 0;
            }
        return 0x24;                            /* Sperrtabelle voll      */
    }
    case 0x07:                                  /* COMMIT                */
    case 0x0B:                                  /* (FreeDOS: 0Ah + BL)   */
    case 0x1D: case 0x22:                       /* alle zu, Prozessende  */
        return 0;
    case 0x08:                                  /* READ                  */
    case 0x09: {                                /* WRITE                 */
        struct shf __far *e;
        s = bp(t, es, di);
        h = sft_h(t, r16(s + 0x0B), &e);
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
            if (e)
                e->size = pos;
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
        if (e && fn == 9 && pos > e->size)      /* fuer die anderen SFTs */
            e->size = pos;
        CX = k;
        return 0;
    }
    case 0x0C: {                                /* freier Platz          */
        unsigned spc = 1, sh = 9, cs;
        unsigned long tot, fr, cl;
        s = bp(t, es, di);
        k = up(s[0] == '\\' ? s[2] : s[0]) - 'A';  /* CD: "\\X.\A." */
        l32 = 0;                                /* CD: nichts frei (MSCDEX) */
        if (FAILED(FAPI(FAPI_QFSINFO, KDATA, k, (unsigned)&fsi, 0)) ||
            (fsi.bsize != 2048 &&
             FAILED(FAPI(FAPI_FREESPACE, KDATA, k, (unsigned)&l32, 0))))
            return E_NOTREADY;
        tot = fsi.bsize == 2048 ? cd_size(k) << 2 : fsi.sectors;  /* CD: PVD */
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
        return FAILED(r) ? errmap(r, nf(p1)) : 0;
    case 0x0F:                                  /* Attribut holen        */
        get_fn(t, 0x9E, p1);
        if (stat_path(p1))
            return nf(p1);
        AX = de.attr;
        BX = (unsigned)(de.size >> 16);
        DI = (unsigned)de.size;
        CX = de.time;
        DX = de.date;
        return 0;
    case 0x11:                                  /* RENAME                */
        get_fn(t, 0x9E, p1);
        get_fn(t, 0x11E, p2);
        if (has_wild(p1) || has_wild(p2))
            return ren_wild();
        r = FAPI(FAPI_MOVE, KDATA, (unsigned)p1, (unsigned)p2, 0);
        if (r == FAPI_ERR_EXIST)
            return E_DENIED;                    /* Ziel gibt es: 05h     */
        return FAILED(r) ? errmap(r, nf(p1)) : 0;
    case 0x13: {                                /* DELETE (auch *.*)     */
        const char *n;
        unsigned i = 0, hit = 0, j, l;
        get_fn(t, 0x9E, p1);
        if (!has_wild(p1)) {
            r = FAPI(FAPI_DELETE, KDATA, (unsigned)p1, 0, 0);
            return FAILED(r) ? errmap(r, nf(p1)) : 0;
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
        return hit ? 0 : nf(p1);
    }
    case 0x16:                                  /* OPEN                  */
    case 0x17:                                  /* CREATE (kuerzen)      */
    case 0x2E: {                                /* erweitert             */
        unsigned mode = arg & 0xFF, act = 0, exists, cattr = arg;
        unsigned long hv;
        int slot;
        get_fn(t, 0x9E, p1);
        exists = stat_path(p1) == 0;
        if (exists && (de.attr & 0x18))
            return E_DENIED;                    /* Verzeichnis/Bezeichnung */
        if (fn == 0x2E) {
            unsigned a = r16(sda(t, 0x2DD));
            mode = r16(sda(t, 0x2E1)) & 0xFF;
            cattr = r16(sda(t, 0x2DF));
            if (exists && (a & 0x0F) == 0)
                return E_EXISTS;
            if (!exists && !(a & 0xF0))
                return nf(p1);
            act = !exists ? 2 : (a & 0x0F) == 2 ? 3 : 1;
        } else if (fn == 0x17 && (arg & 0x100) && exists)
            return E_EXISTS;                    /* 5Bh: nur neu anlegen  */
        else
            act = fn == 0x17 ? (exists ? 3 : 2) : 1;
        if (act == 1 && !exists)
            return nf(p1);
        if (fn == 0x17)
            mode = 2;                           /* arg ist das Attribut;  */
        hv = phash(p1);                         /* DOS prueft den Modus   */
        slot = sh_find(t, hv);
        if (slot >= 0) {                        /* Box hat sie schon offen */
            struct shf __far *e = &RBX(t)->sh[slot];
            if (act != 1 || !sh_ok(e, mode))
                return E_DENIED;                /* MS-DOS 6.22 mit SHARE: */
                                                /* 05h (BIOSCMP)          */
            if (M_WR(mode) && !e->kw) {         /* Handle nur lesend:     */
                FAPI(FAPI_CLOSE, KDATA, e->h1 - 1, 0, 0);   /* neu RW     */
                h = FAPI(FAPI_OPEN, KDATA, (unsigned)p1, OPEN_RW, 0);
                if (FAILED(h)) {
                    r = FAPI(FAPI_OPEN, KDATA, (unsigned)p1, OPEN_READ, 0);
                    e->h1 = FAILED(r) ? 0 : r + 1;
                    return errmap(h, E_DENIED);
                }
                e->h1 = h + 1;
                e->kw = 1;
            }
            e->refs++;
            sh_add(e, mode, 1);
            if (e->size > de.size)
                de.size = e->size;
            sft_fill(t, es, di, (SH_MARK | slot) - 1, mode, p1);
            if (fn == 0x2E)
                ext_act(f, t, act);
            return 0;
        }
        if (act == 1)                           /* vorhanden: lesen oder  */
            h = FAPI(FAPI_OPEN, KDATA, (unsigned)p1, (mode & 3) ? OPEN_RW : OPEN_READ, 0);
        else                                    /* lesen+schreiben        */
            h = FAPI(FAPI_CREATE, KDATA, (unsigned)p1, 0, 0);
        if (FAILED(h))
            return errmap(h, act == 1 ? E_NOFILE : no_dir(p1) ? E_NOPATH : E_DENIED);
        if (act != 1) {
            if (cattr & 0x07)                   /* versteckt/System/R:   */
                kf_hmeta(h, 2, (cattr & 0x27) | 0x20, 0, 0);  /* beim Schliessen */
            de.attr = 0x20 | (cattr & 0x07);
            de.size = 0;
            stat_path(p1);                      /* Datum/Uhrzeit         */
            de.size = 0;
        }
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
            h = (SH_MARK | slot) - 1;
        }
        sft_fill(t, es, di, h, mode, p1);
        if (fn == 0x2E)
            ext_act(f, t, act);
        return 0;
    }
    case 0x1B:                                  /* FINDFIRST             */
        return find_first(t);
    case 0x1C:                                  /* FINDNEXT (ES:DI)      */
        return find_next(t, 0, es, di);
    case 0x21: {                                /* SEEK vom Ende         */
        unsigned long off = ((unsigned long)CX << 16) | DX;
        struct shf __far *e;
        s = bp(t, es, di);
        size = r32(s + 0x11);
        if (sft_h(t, r16(s + 0x0B), &e) != 0xFFFF && e && e->size > size)
            size = e->size;                     /* ein anderer SFT schrieb */
        pos = size + off;
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
    if (!sda_seg[t] || !rb_sel)             /* (kein Kernelsegment frei) */
        return -1;
    kf_box_ctl(BOX_LOCK, 1);
    r = op(f, t, AL);
    kf_box_ctl(BOX_LOCK, 0);
    return r;
}

/* INT E0h 21h (BOXSHELL): Laufwerk AL (0 = A:) fuer die IOCTL-Abfragen,
   die DOS bei einem Netzlaufwerk ablehnt (4408h, 440Dh, 6900h) - die Box
   meldet die AT-DOS-Laufwerke wie MS-DOS lokale (Wunsch des Nutzers,
   BIOSCMP gegen MS-DOS 6.22). Nach ES:DI, 32 Byte: +0 Art (1 fest, 2
   Wechsel, 3 CD), +1 Sektoren je Cluster, +2 Bytes je Sektor, +4 Cluster
   (DWORD), +8 Seriennummer, +12 Bezeichnung (11), +23 Dateisystem (8),
   +31 Medienbyte. -> 0, sonst kein solches Laufwerk */
static struct fapi_label di_lb;                 /* Kontext                */
static int dinfo(struct v86_frame *f, int t);
int __far redir_dinfo(struct v86_frame *f, int t)
{
    int r;
    kf_box_ctl(BOX_LOCK, 1);
    r = dinfo(f, t);
    kf_box_ctl(BOX_LOCK, 0);
    return r;
}
static int dinfo(struct v86_frame *f, int t)
{
    unsigned char __far *o;
    unsigned d = AL, k, cs = 0, spc;
    unsigned long cl = 0;
    if (FAILED(FAPI(FAPI_QFSINFO, KDATA, d, (unsigned)&fsi, 0)))
        return 1;
    if (fsi.bsize == 2048 || (fsi.flags & FSI_NOMEDIA))
        return 1;                               /* CD/leer: wie bisher    */
    if (fat_geom(d, &cs, &cl) || !cs) {
        cs = 1;
        cl = fsi.sectors;
    }
    for (spc = cs; cl > 0xFFF0UL && spc < 128; spc <<= 1)
        cl >>= 1;
    di_lb.label[0] = 0;
    di_lb.serial = 0;
    FAPI(FAPI_LABEL, KDATA, d, (unsigned)&di_lb, 0);
    o = bp(t, (unsigned)f->r.es, DI);
    for (k = 0; k < 32; k++)
        o[k] = 0;
    o[0] = (fsi.flags & FSI_REMOVABLE) && !(fsi.flags & FSI_REMDISK) ? 2 : 1;
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
    o[31] = o[0] == 2 || (fsi.flags & FSI_REMDISK) ? 0xF0 : 0xF8;
    return 0;
}

/* Kontext je Task (Phase 6, kernel.c ctx_take) */
const struct ctxblk redir_ctx[] = {
    { p1, sizeof(p1) }, { p2, sizeof(p2) }, { tmpl, sizeof(tmpl) }, { &de, sizeof(de) },
    { &fsi, sizeof(fsi) }, { &fin, sizeof(fin) }, { &l32, sizeof(l32) },
    { st_dir, sizeof(st_dir) }, { st_fcb, sizeof(st_fcb) }, { sf_dir, sizeof(sf_dir) },
    { fn_fcb, sizeof(fn_fcb) }, { fn_tp, sizeof(fn_tp) }, { fn_dir, sizeof(fn_dir) }, { &fn_lb, sizeof(fn_lb) },
    { op_dir, sizeof(op_dir) }, { op_fcb, sizeof(op_fcb) }, { op_one, sizeof(op_one) },
    { np_dir, sizeof(np_dir) }, { ff_dir, sizeof(ff_dir) }, { &di_lb, sizeof(di_lb) }
};
const unsigned redir_nctx = sizeof(redir_ctx) / sizeof(redir_ctx[0]);



/* ---- MSCDEX (INT 2Fh 15xxh) ----------------------------------------------
   Viele CD-Spiele und Installationsprogramme fragen MSCDEX ab, bevor sie
   die CD ueber DOS lesen (Dateien gehen ohnehin ueber den Redirector).
   BOXSHELL reicht AH = 15h mit INT E0h 20h hierher (AL = Unterfunktion).
   Version 2.21 wie MSCDEX von MS-DOS 6.22; CD-Laufwerke sind alle AT-DOS-
   Laufwerke mit 2048-Byte-Bloecken (auch ISO-Loop). Gelesen wird direkt
   ueber das Blockgeraet (kf_cd_read). Keine CD-Audio-Wiedergabe: Abspiel-
   befehle melden "erledigt", Spuren gibt es nur als Datenspur.
   -> 0 erledigt, > 0 Fehler (CF, AX), -1 nicht zustaendig */
unsigned long __far kf_cd_blocks(int d);
int __far kf_cd_read(int d, unsigned long lba, unsigned sel, unsigned off);
#define SI  (*(unsigned short *)&f->esi)
#define E_BADDRV 15

static int cd_first(unsigned *n)
{
    int d, first = -1;
    *n = 0;
    for (d = 0; d < 26; d++)
        if (kf_cd_blocks(d)) {
            if (first < 0)
                first = d;
            ++*n;
        }
    return first;
}

/* count Bloecke ab lba nach seg:off der Box; 0 = ok */
static int cd_rd(int t, int d, unsigned long lba, unsigned count, unsigned seg, unsigned off)
{
    unsigned long lin = ((unsigned long)seg << 4) + off;
    int r;
    while (count--) {
        bp(t, (unsigned)(lin >> 4), 0);
        if ((r = kf_cd_read(d, lba++, RWIN_SEL(t), (unsigned)lin & 15)) != 0)
            return r;
        lin += 2048;
    }
    return 0;
}

/* Groesse der CD in Bloecken: aus dem PVD (der Treiber kennt sie oft
   nicht), sonst vom Blockgeraet */
static unsigned long cd_size(int d)
{
    unsigned long n = kf_cd_blocks(d);
    if (!kf_cd_read(d, 16, 0, 0)) {
        unsigned char __far *b = MK_FP(DMABUF, 0);
        if (b[1] == 'C' && b[2] == 'D' && b[3] == '0')
            n = r32(b + 80);
    }
    return n;
}

/* Geraeteauftrag (1510h) bei ES:BX fuer Laufwerk d */
static void cd_req(int t, int d, unsigned seg, unsigned off)
{
    unsigned char __far *q = bp(t, seg, off);
    unsigned st = 0x0100, cmd = q[2], ts, to, n;
    unsigned long lba, v = 0;
    unsigned char code;
    unsigned char __far *c;

    ts = r16(q + 0x10);                 /* Uebergabeadresse (fern)        */
    to = r16(q + 0x0E);
    if (cmd == 3) {                     /* IOCTL Eingabe                  */
        c = bp(t, ts, to);
        code = c[0];
        switch (code) {
        case 0: w32(c + 1, 0); break;               /* Kopf des Treibers */
        case 1: c[1] = 0; w32(c + 2, 0); break;     /* Lage des Kopfs    */
        case 4: for (n = 1; n < 9; n++) c[n] = (unsigned char)(n & 1 ? n / 2 : 0xFF); break;
        case 6: w32(c + 1, 0x00000202UL); break;    /* entriegelt, Red Book */
        case 7: c[1] = 0; w16(c + 2, 2048); break;  /* Sektorgroesse      */
        case 8: v = cd_size(d); c = bp(t, ts, to); w32(c + 1, v); break;
        case 9: c[1] = 1; break;                    /* nicht gewechselt   */
        case 10: c[1] = 1; c[2] = 1;                /* eine (Daten-)Spur  */
            v = cd_size(d) + 150;                   /* Ende als MSF       */
            c = bp(t, ts, to);
            for (n = 0; v >= 4500 && n < 99; n++)   /* ohne 32-Bit-Division */
                v -= 4500;
            c[5] = (unsigned char)n;
            c[4] = (unsigned char)((unsigned)v / 75);
            c[3] = (unsigned char)((unsigned)v % 75);
            c[6] = 0;
            break;
        case 11: c[2] = 2; c[3] = 0; c[4] = 0; c[5] = 0; c[6] = 0x40; break;  /* Spur 1: 00:02:00, Daten */
        case 12: case 15: for (n = 1; n < 11; n++) c[n] = 0; break;
        default: st = 0x8103;
        }
        q = bp(t, seg, off);
    } else if (cmd == 128) {            /* READ LONG                      */
        lba = r32(q + 0x14);
        if (q[0x0D] == 1)               /* Red Book (MSF) -> Block        */
        {
            unsigned m = (unsigned)(lba >> 16) & 0xFF;
            v = (unsigned)((lba >> 8) & 0xFF) * 75u + ((unsigned)lba & 0xFF);
            while (m--)                 /* ohne 32-Bit-Multiplikation    */
                v += 4500;
            lba = v - 150;
        }
        n = r16(q + 0x12);
        if (q[0x18])                    /* roh (2352 Byte) gibt es nicht  */
            st = 0x8103;
        else if ((v = (unsigned long)cd_rd(t, d, lba, n, ts, to)) != 0)
            st = v == 1 ? 0x8102 : 0x810F;
        q = bp(t, seg, off);
    } else if (cmd != 13 && cmd != 14 && cmd != 7 && cmd != 12 && cmd != 131 &&
               cmd != 132 && cmd != 133 && cmd != 136)
        st = 0x8103;                    /* unbekannter Befehl             */
    w16(q + 3, st);
}

int __far mscdex(struct v86_frame *f, int t)
{
    unsigned char sub = AL;
    unsigned n, i, k, seg = f->r.es;
    int d, r;
    unsigned char __far *p;
    unsigned char __far *s;

    if (sub != 0 && sub != 0x0B && sub != 0x0C && sub != 0x0D &&
        (CX >= 26 || !kf_cd_blocks(CX)))
        return sub <= 0x10 ? (AX = E_BADDRV, 1) : -1;
    kf_box_ctl(BOX_LOCK, 1);
    r = 0;
    switch (sub) {
    case 0x00:                          /* Installation: Zahl, erstes     */
        d = cd_first(&n);
        BX = n;
        CX = d < 0 ? 0 : (unsigned)d;
        break;
    case 0x01:                          /* Treiberliste: Untereinheit +   */
        for (d = 0, k = 0, i = 0xFF; d < 26; d++)    /* Geraetekopf      */
            if (kf_cd_blocks(d)) {
                if (i == 0xFF)
                    i = d;
                p = bp(t, seg, BX + k * 5);
                p[0] = (unsigned char)k;
                w32(p + 1, 0xF0000AE0UL);
                k++;
            }
        /* Kopf eines CD-Treibers bei F000:0AE0 (Stummelseite, hinter der
           XMS-Tabelle): Zeichengeraet C800h, "MSCD001", Laufwerk und
           Einheiten wie MSCDEX sie eintragen; Strategie/Interrupt = RETF.
           Vorher Nullzeiger - CheckIt las dort die Interrupttabelle und
           hielt die CD fuer ein Netzlaufwerk. */
        p = bp(t, 0xF000, 0x0AE0);
        w32(p, 0xFFFFFFFFUL);
        p[4] = 0x00; p[5] = 0xC8;
        p[6] = 0x02; p[7] = 0x05;       /* XMS-Stummel +2: CB (RETF)      */
        p[8] = 0x02; p[9] = 0x05;
        for (n = 0; n < 8; n++)
            p[10 + n] = (unsigned char)"MSCD001 "[n];
        p[18] = p[19] = 0;
        p[20] = (unsigned char)(i == 0xFF ? 0 : i + 1);
        p[21] = (unsigned char)k;
        break;
    case 0x02: case 0x03: case 0x04:    /* Dateinamen aus dem PVD         */
        if (kf_cd_read(CX, 16, 0, 0)) {
            AX = E_NOTREADY;
            r = 1;
            break;
        }
        s = MK_FP(DMABUF, 702 + (sub - 2) * 37);
        p = bp(t, seg, BX);
        for (i = 0; i < 37 && s[i] != ' ' && s[i]; i++)
            p[i] = s[i];
        p[i] = 0;
        break;
    case 0x05:                          /* VTOC lesen (DX = Index)        */
        if ((k = (unsigned)cd_rd(t, CX, 16UL + DX, 1, seg, BX)) != 0) {
            AX = k == 1 ? E_NOTREADY : 0x1F;
            r = 1;
        } else
            AX = bp(t, seg, BX)[0] == 1 ? 1 : bp(t, seg, BX)[0] == 0xFF ? 0xFF : 0;
        break;
    case 0x08:                          /* absolut lesen: SI:DI, DX Stueck */
        if ((k = (unsigned)cd_rd(t, CX, ((unsigned long)SI << 16) | DI, DX, seg, BX)) != 0) {
            AX = k == 1 ? E_NOTREADY : 0x1F;
            r = 1;
        }
        break;
    case 0x09:                          /* schreiben: nein                */
        AX = 0x13;                      /* schreibgeschuetzt              */
        r = 1;
        break;
    case 0x0B:                          /* Laufwerk CX ein CD?            */
        AX = CX < 26 && kf_cd_blocks(CX) ? 0x5AD8 : 0;
        BX = 0xADAD;
        break;
    case 0x0C:
        BX = 0x0215;                    /* Version 2.21                   */
        break;
    case 0x0D:                          /* Buchstaben (0 = A:) nach ES:BX */
        for (d = 0, k = 0; d < 26; d++)
            if (kf_cd_blocks(d))
                bp(t, seg, BX + k++)[0] = (unsigned char)d;
        break;
    case 0x0E: case 0x0F:               /* Vorgaben / Verzeichniseintrag   */
        AX = 1;                         /* nicht unterstuetzt              */
        r = 1;
        break;
    case 0x10:                          /* Geraeteauftrag ES:BX            */
        cd_req(t, CX, seg, BX);
        break;
    default:
        r = -1;
    }
    kf_box_ctl(BOX_LOCK, 0);
    return r;
}
