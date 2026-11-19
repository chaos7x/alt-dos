/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RBOX: Redirector der Kompatibilitaetsbox (5.1) - AT-DOS-Laufwerke unter
 * FreeDOS. Wird von task_rbox.c eingebunden.
 *
 * Wie in 6.0 (kernel/redir.c, von dort uebernommen): RBSHELL traegt die
 * AT-DOS-Laufwerke ab C: als Netzlaufwerke in die CDS ein (Flags C000h)
 * und reicht INT 2Fh 11xxh mit INT E0h AH = 10h weiter. Anders als in 6.0
 * erledigt das nicht der Kernel, sondern RBOX selbst mit seinen FAPI-
 * Aufrufen (wie eine VDM im Benutzermodus): RB_RUN liefert RB_CALL, RBOX
 * holt die Register (RB_REGS), liest/schreibt den Speicher der Box
 * (RB_MEM), setzt das Ergebnis und laesst die Box mit RB_RUN weiterlaufen.
 * Offene Dateien der Box sind Handles von RBOX - endet RBOX, schliesst der
 * Kernel sie.
 *
 * Weitere INT-E0h-Dienste: 11h SDA ist DS:SI, 12h -> DX:AX Laufwerke,
 * 13h Startlaufwerk (AL = Buchstabe, 0 = X:\DOS\MDOS\COMMAND.COM fehlt:
 * dann bleibt A: das Abbild), 14h aktuelles Verzeichnis von RBOX (8.3)
 * nach ES:DI (leer bei A:/B:, wenn die Box sie nicht ueberlaesst),
 * 15h aktive Tastaturbelegung von AT-DOS (270 Byte: ohne Umschalt, mit
 * Umschalt, mit AltGr, je Scancode 00h-59h) nach ES:DI -> AX = 0.
 *
 * Daten des DOS: SDA (INT 21h 5D06h, Aufbau wie MS-DOS 4+): DTA 0Ch,
 * Dateiname 1 9Eh, Dateiname 2 11Eh, Suchzustand 19Eh (21 Byte),
 * gefundener Eintrag 1B3h, Suchattribut 24Dh, erweitertes Oeffnen
 * 2DDh-2E2h. SFT (ES:DI): Modus 02h, Attribut 04h, Flags 05h (8000h Netz
 * | Laufwerk), Zeit/Datum 0Dh/0Fh, Groesse 11h, Position 15h, Name 20h;
 * das FAPI-Handle + 1 steht in 0Bh.
 */

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

#define FAILED(r)   ((r) >= FAPI_ERR_FIRST)
#define SFTLEN      0x3B              /* bis sf_PID (31h) und mehr  */

static struct fapi_rbregs rg;
static unsigned char rb_perm;
static unsigned char ms_dos;            /* DOS der Box ist nicht FreeDOS */
static char rb_iname[13];               /* Abbild von MKDOSIMG (ohne .IMG),
                                           leer bei FreeDOS */           /* RBOX /P: PROTECTONLY=NO      */
static struct fapi_rbmem rm;
static unsigned long sda_lin;
static char p1[128], p2[128], tmpl[11];
static struct fapi_dirent de;
static struct fapi_fsinfo fsi;
static struct fapi_finfo fin;
static struct fapi_label lb;
static unsigned long l32;
static char sdir[4][80];                /* Verzeichnisse laufender Suchen */
static unsigned char sdir_next;
static unsigned char sgen[4];           /* Generation je Platz (DTA 16)  */
static char ff_dir[80];

/* DOS haelt den Suchzustand im DTA (21 Byte, gehoeren dem Redirector):
   0 Laufwerk|80h, 1-11 Muster, 12 Attribute, 13/14 Eintrag, 15 Platz,
   16 Generation, 17/18 Pruefsumme des zuletzt gefundenen Namens. Word 5.5
   SETUP setzt eine Suche nach anderen Suchen fort - mit 4 reihum belegten
   Plaetzen lief FINDNEXT im falschen Verzeichnis weiter. */
static unsigned h11(const char *f)
{
    unsigned h = 0, k;
    for (k = 0; k < 11; k++)
        h = (h << 5) + h + (unsigned char)f[k];
    return h;
}
static unsigned char sft[SFTLEN], srch[21], hit[32], word2[4];
static char xbuf[8192];                 /* Daten Datei <-> Box           */

/* ---- Speicher der Box --------------------------------------------------- */

static unsigned long lin(unsigned seg, unsigned off)
{
    return ((unsigned long)seg << 4) + off;
}

static int mem(unsigned long l, void *buf, unsigned len, unsigned wr)
{
    rm.lin = l;
    rm.buf = (unsigned)buf;
    rm.len = len;
    rm.write = wr;
    return DosRBox(RB_MEM, (unsigned)&rm, 0) ? -1 : 0;
}
#define peek(l, b, n)   mem((l), (b), (n), 0)
#define poke(l, b, n)   mem((l), (b), (n), 1)

static unsigned r16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static void w16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static unsigned long r32(const unsigned char *p) { return r16(p) | ((unsigned long)r16(p + 2) << 16); }
static void w32(unsigned char *p, unsigned long v) { w16(p, (unsigned)v); w16(p + 2, (unsigned)(v >> 16)); }

static unsigned sda16(unsigned x)
{
    peek(sda_lin + x, word2, 2);
    return r16(word2);
}

static void get_fn(unsigned x, char *d)
{
    unsigned i, o = 0, k;
    peek(sda_lin + x, d, 128);
    d[127] = 0;
    for (i = 0; d[i]; i++) {            /* DOS denkt in 8+3-Feldern:      */
        if (d[i] == ' ') {              /* "LW .LLD" ist LW.LLD (Lotus    */
            for (k = i; d[k] == ' '; k++)   /* Works INSTALL) - Leerzeichen */
                ;                       /* vor Punkt, Trenner, Ende weg   */
            if (!d[k] || d[k] == '.' || d[k] == '\\') {
                i = k - 1;
                continue;
            }
        }
        d[o++] = d[i];
    }
    if (o > 3 && d[o - 1] == '\\')      /* MS-DOS fragt "C:\T3\" (1105h,  */
        o--;                            /* XCOPY) - wie "C:\T3"           */
    d[o] = 0;
}

/* ---- Namen ---------------------------------------------------------------- */

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
            o[k++] = up(n[i]);
    if (n[i] == '.')
        i++;
    for (k = 8; n[i]; i++)
        if (n[i] == '*')
            while (k < 11)
                o[k++] = '?';
        else if (k < 11)
            o[k++] = up(n[i]);
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

/* Eintrag zu Pfad p in de suchen (Stamm: Verzeichnis) -> 0 gefunden */
static char st_dir[80], st_fcb[11];
static int stat_path(const char *p);

/* Fehlt das Verzeichnis, in dem p liegen soll? MS-DOS unterscheidet:
   Datei fehlt 02h, Verzeichnis fehlt 03h (BIOSCMP) */
static char np_dir[80];
static int no_dir(const char *p)
{
    split(p, np_dir);
    if (!np_dir[3])
        return 0;
    return stat_path(np_dir) || !(de.attr & 0x10);
}
static unsigned nf(const char *p) { return no_dir(p) ? E_NOPATH : E_NOFILE; }

/* Was beim Schliessen nachzutragen ist (DOS setzt 5701h nur im SFT, ein
   Attribut beim Anlegen kennt DosCreate nicht): Pfad je Handle */
#define NHP 32
static char hp[NHP][80];
static unsigned char hpa[NHP];          /* Attribut + 80h = nachtragen  */
static unsigned hpd[NHP][3];            /* Datum aus 5701h eines SFT,   */
                                        /* der vor dem letzten schloss  */
static void hp_note(unsigned h, const char *p, unsigned a)
{
    unsigned i;
    if (h >= NHP)
        return;
    for (i = 0; i < 79 && p[i]; i++)
        hp[h][i] = p[i];
    hp[h][i] = 0;
    hpa[h] = (unsigned char)a;
}

/* SHARE (18.11.2026, wie auf main redir.c): oeffnet die Box eine Datei
   noch einmal, teilen sich beide SFTs den Handle von AT-DOS (Stelle je
   SFT, Groesse nur einmal); Freigabemodi gegen die anderen Oeffnungen,
   Bereichssperren (110Ah). SFT+0Bh = E000h | Platz, sonst Handle + 1. */
#define NSH 16
#define NLK 8
#define SH_MARK 0xE000
static struct shf {
    unsigned       h1;
    unsigned long  hash, size;
    unsigned char  refs, rd, wr, dr, dw, kw;
} sh[NSH];
static struct shl {
    unsigned char  used, slot;
    unsigned long  ofs, len;
} lk[NLK];

static unsigned long phash(const char *p)
{
    unsigned long h = 5381;
    while (*p)
        h = ((h << 5) + h) ^ up(*p++);
    return h;
}
static int sh_find(unsigned long hv)
{
    int i;
    for (i = 0; i < NSH; i++)
        if (sh[i].h1 && sh[i].hash == hv)
            return i;
    return -1;
}
#define M_RD(m)  (((m) & 3) != 1)
#define M_WR(m)  (((m) & 3) != 0)
#define M_DR(m)  ((((m) >> 4) & 7) == 1 || (((m) >> 4) & 7) == 3)
#define M_DW(m)  ((((m) >> 4) & 7) == 1 || (((m) >> 4) & 7) == 2)
static void sh_add(struct shf *e, unsigned m, int d)
{
    e->rd += M_RD(m) ? d : 0;
    e->wr += M_WR(m) ? d : 0;
    e->dr += M_DR(m) ? d : 0;
    e->dw += M_DW(m) ? d : 0;
}
static int sh_ok(const struct shf *e, unsigned m)
{
    return !((M_RD(m) && e->dr) || (M_WR(m) && e->dw) ||
             (M_DR(m) && e->rd) || (M_DW(m) && e->wr));
}
static unsigned sft_h(unsigned v, struct shf **e)
{
    *e = 0;
    if ((v & 0xFF00) == SH_MARK) {
        if ((v & 0xFF) >= NSH || !sh[v & 0xFF].h1)
            return 0xFFFF;
        *e = &sh[v & 0xFF];
        return (*e)->h1 - 1;
    }
    return v - 1;
}

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
        if (FAILED(DosFindEntry(&de, i, st_dir)))
            return -1;
        to_fcb(de.name, st_fcb);
        for (k = 0; k < 11 && st_fcb[k] == tmpl[k]; k++)
            ;
        if (k == 11)
            return 0;
    }
}

/* ---- SFT ------------------------------------------------------------------ */

static void sft_fill(unsigned h, unsigned mode, const char *path)
{
    static char dir[80];
    unsigned k;
    w16(sft + 0x02, (r16(sft + 0x02) & 0x8000) | mode);  /* sf_isFCB (MS-DOS
                                           setzt es vor dem Oeffnen) bleibt */
    sft[0x04] = de.attr;
    w16(sft + 0x05, 0x8040 | (up(path[0]) - 'A'));
    w32(sft + 0x07, 0);
    w16(sft + 0x0B, h + 1);
    w16(sft + 0x0D, de.time);
    w16(sft + 0x0F, de.date);
    w32(sft + 0x11, de.size);
    w32(sft + 0x15, 0);
    w16(sft + 0x31, sda16(0x10));       /* sf_PID = aktueller PSP: MS-DOS */
                                        /* setzt es bei Netzdateien nicht, */
                                        /* prueft es aber bei FCBs        */
    to_fcb(split(path, dir), tmpl);
    for (k = 0; k < 11; k++)
        sft[0x20 + k] = tmpl[k];
}

/* Daten zwischen Datei und Box in Stuecken ueber xbuf */
static unsigned xfer(int wr, unsigned h, unsigned long l, unsigned n, unsigned *err)
{
    unsigned done = 0, k, r;
    *err = 0;
    while (done < n) {
        k = n - done > sizeof(xbuf) ? sizeof(xbuf) : n - done;
        if (wr) {
            peek(l, xbuf, k);
            r = DosWrite(h, xbuf, k);
        } else
            r = DosRead(h, xbuf, k);
        if (FAILED(r)) {
            *err = errmap(r, E_DENIED);
            break;
        }
        if (!wr && r)
            poke(l, xbuf, r);
        done += r;
        l += r;
        if (r < k)
            break;
    }
    return done;
}

/* ---- Suche ------------------------------------------------------------------ */

/* naechster passender Eintrag ab Suchzustand srch (aus der Box) */
static int find_next(void)
{
    static char fcb[11], tp[11];
    unsigned n = r16(srch + 13), slot = srch[15] & 3, k, lh = r16(srch + 17);
    unsigned char mask = srch[12], a;
    if (n == 0xFFFF)
        return E_NOMORE;                    /* nach der Bezeichnung      */
    if (mask != 0x08 && srch[16] != sgen[slot])
        return E_NOMORE;                    /* Platz inzwischen anders   */
    if (mask == 0x08) {                     /* nur Datentraegerbezeichnung */
        if (FAILED(DosLabel(srch[0] & 0x1F, &lb, 0)) || !lb.label[0])
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
        tp[k] = srch[1 + k];
    /* Eintrag n-1 inzwischen geloescht (DEL *.* loescht zwischen den
       Suchschritten)? Dann sind alle folgenden eins nach vorn gerueckt. */
    if (n && n < 0xFFFE && !FAILED(DosFindEntry(&de, n - 1, sdir[slot]))) {
        to_fcb(de.name, fcb);
        if (h11(fcb) != lh)
            n--;
    }
    for (;; n++) {
        if (FAILED(DosFindEntry(&de, n, sdir[slot])))
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
    w16(srch + 13, n + 1);
    w16(srch + 17, h11(fcb));
    for (k = 0; k < 11; k++)                /* gefundener Eintrag        */
        hit[k] = fcb[k];
    hit[11] = de.attr;
    for (k = 12; k < 22; k++)
        hit[k] = 0;
    w16(hit + 22, de.time);
    w16(hit + 24, de.date);
    w16(hit + 26, 0);
    w32(hit + 28, de.size);
    poke(sda_lin + 0x1B3, hit, 32);
    return 0;
}

static int find_first(void)
{
    const char *n;
    unsigned slot, k, i;
    unsigned char a;
    get_fn(0x9E, p1);
    n = split(p1, ff_dir);
    for (slot = 0; slot < 4; slot++) {      /* gleiches Verzeichnis:     */
        for (i = 0; ff_dir[i] && ff_dir[i] == sdir[slot][i]; i++)
            ;                               /* gleicher Platz            */
        if (ff_dir[i] == sdir[slot][i])
            break;
    }
    if (slot == 4) {
        slot = sdir_next++ & 3;
        for (i = 0; (sdir[slot][i] = ff_dir[i]) != 0; i++)
            ;
        sgen[slot]++;
    }
    to_fcb(*n ? n : "????????.???", tmpl);
    peek(sda_lin + 0x24D, &a, 1);
    srch[0] = (unsigned char)(0x80 | (up(p1[0]) - 'A'));
    for (k = 0; k < 11; k++)
        srch[1 + k] = tmpl[k];
    srch[12] = a;
    w16(srch + 13, 0);
    srch[15] = (unsigned char)slot;
    srch[16] = sgen[slot];
    for (k = 17; k < 21; k++)
        srch[k] = 0;
    k = find_next();
    poke(sda_lin + 0x19E, srch, 21);
    if (k == E_NOMORE && no_dir(p1))
        k = E_NOPATH;                       /* Verzeichnis fehlt: 03h    */
    return k;                           /* ohne Treffer 12h wie FreeDOS
                                           (fatdir.c) und MS-DOS - Lotus
                                           Works INSTALL hielt 02h fuer
                                           einen Lesefehler */
}

/* ---- Verteiler ---------------------------------------------------------------- */

/* -> -1 nicht unser, 0 erledigt, sonst DOS-Fehler */
/* REN mit Platzhaltern (p1, p2 aus der SDA). FreeDOS loest sie selbst
   auf, MS-DOS reicht sie an den Redirector durch: je Treffer von p1 den
   neuen Namen nach der FCB-Regel bilden (? = Zeichen der Quelle an
   dieser Stelle, sonst Zeichen des Musters), im Zielverzeichnis von p2. */
static char rw_dir[80], rw_fcb[11], rw_one[96];
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
    unsigned i = 0, hits = 0, k, l, r, err = 0;
    to_fcb(split(p2, ff_dir), t2);              /* Ziel: ff_dir + Muster  */
    n = split(p1, rw_dir);
    to_fcb(n, tmpl);
    while (!FAILED(DosFindEntry(&de, i++, rw_dir))) {
        to_fcb(de.name, rw_fcb);
        if ((de.attr & 0x1E) || !fcb_match(tmpl, rw_fcb))
            continue;
        for (k = 0; k < 11; k++)
            nn[k] = t2[k] == '?' ? rw_fcb[k] : t2[k];
        for (l = 0; rw_dir[l] && l < 80; l++)
            rw_one[l] = rw_dir[l];
        if (rw_one[l - 1] != '\\')
            rw_one[l++] = '\\';
        for (k = 0; de.name[k] && l < 95; k++)
            rw_one[l++] = de.name[k];
        rw_one[l] = 0;
        for (l = 0; ff_dir[l] && l < 80; l++)
            p2[l] = ff_dir[l];
        if (p2[l - 1] != '\\')
            p2[l++] = '\\';
        fcb_name(nn, p2 + l);
        r = DosMove(rw_one, p2);
        if (FAILED(r))
            err = r == FAPI_ERR_EXIST ? E_DENIED : errmap(r, E_DENIED);
        else
            hits++;
    }
    if (!hits)
        return err ? err : nf(p1);
    return 0;                                   /* wie MS-DOS: Teilerfolg */
}

/* Aktionscode von 6Ch (1 geoeffnet, 2 angelegt, 3 ersetzt): FreeDOS
   nimmt ihn aus CX, MS-DOS gibt dem Aufrufer bei einem Redirector nur AX
   zurueck - dort steht er in dessen Registerrahmen (SDA+264h/266h, CX
   bei +4; MS-DOS 5.0 bis 6.22, PC DOS) */
static void ext_act(unsigned act)
{
    unsigned sp, ss;
    rg.cx = act;
    if (!ms_dos)
        return;
    sp = sda16(0x264);
    ss = sda16(0x266);
    w16(word2, act);
    poke(lin(ss, sp + 4), word2, 2);
}

static int op(unsigned fn)
{
    static char dir[80], fcb[11], one[96];
    unsigned long sl = lin(rg.es, rg.di), pos, size;
    unsigned h, r, k, arg, cnt, err;

    /* Argument, das DOS vor INT 2Fh auf den Stack legte: [SP] AX von
       RBSHELL, [SP+2] IP, CS, FLAGS, dann das Argument */
    peek(lin(rg.ss, rg.sp + 8), word2, 2);
    arg = r16(word2);

    switch (fn) {
    case 0x01:                                  /* RMDIR                 */
    case 0x03:                                  /* MKDIR                 */
        get_fn(0x9E, p1);
        if (fn == 1) {                          /* aktuelles Verzeichnis */
            peek(sda_lin + 0x282, word2, 4);    /* (CDS bei SDA+282h):   */
            peek(lin(r16(word2 + 2), r16(word2)), one, 67);   /* 10h      */
            for (k = 0; k < 67 && p1[k] && up(one[k]) == up(p1[k]); k++)
                ;
            if (k < 67 && !p1[k] && !one[k])
                return 0x10;
        }
        r = fn == 1 ? DosRmDir(p1) : DosMkDir(p1);
        if (!FAILED(r))
            return 0;
        if (fn == 3 && r == FAPI_ERR_EXIST)
            return E_DENIED;                    /* MKDIR wie MS-DOS: 05h */
        return errmap(r, fn == 1 ? (no_dir(p1) ? E_NOPATH : E_DENIED) : E_NOPATH);
    case 0x05:                                  /* CHDIR: gibt es das?   */
        get_fn(0x9E, p1);
        if (stat_path(p1) || !(de.attr & 0x10))
            return E_NOPATH;
        return 0;
    case 0x06: {                                /* CLOSE                 */
        struct shf *e;
        peek(sl, sft, SFTLEN);
        cnt = r16(sft);
        k = r16(sft + 0x0B);
        h = sft_h(k, &e);
        if (cnt)
            w16(sft, --cnt);
        if (cnt == 0 && h != 0xFFFF && e) {     /* geteilt: erst die      */
            sh_add(e, r16(sft + 2), -1);        /* letzte Oeffnung        */
            if (--e->refs) {                    /* schliesst              */
                if (r16(sft + 5) & 0x4000 && h < NHP) {     /* Datum bis */
                    hpd[h][2] = 1;              /* dahin merken           */
                    hpd[h][0] = r16(sft + 0x0D);
                    hpd[h][1] = r16(sft + 0x0F);
                }
                w16(sft + 0x0B, 0);
                poke(sl, sft, SFTLEN);
                return 0;
            }
            e->h1 = 0;
            for (r = 0; r < NLK; r++)
                if (lk[r].slot == (k & 0xFF))
                    lk[r].used = 0;
            if (!(r16(sft + 5) & 0x4000) && h < NHP && hpd[h][2]) {
                w16(sft + 5, r16(sft + 5) | 0x4000);    /* Datum eines   */
                w16(sft + 0x0D, hpd[h][0]);     /* frueheren SFT          */
                w16(sft + 0x0F, hpd[h][1]);
            }
        }
        if (cnt == 0 && h != 0xFFFF) {
            DosClose(h);
            w16(sft + 0x0B, 0);
            if (h < NHP && hp[h][0] && ((r16(sft + 5) & 0x4000) || (hpa[h] & 0x80))) {
                fin.mask = 0;                   /* Datum (5701h) und      */
                if (r16(sft + 5) & 0x4000) {    /* Attribut nachtragen    */
                    fin.mask |= 2;
                    fin.time = r16(sft + 0x0D);
                    fin.date = r16(sft + 0x0F);
                }
                if (hpa[h] & 0x80) {
                    fin.mask |= 1;
                    fin.attr = hpa[h] & 0x27;
                }
                DosSetFInfo(hp[h], &fin);
            }
            if (h < NHP)
                hp[h][0] = 0, hpd[h][2] = 0;
        }
        poke(sl, sft, SFTLEN);
        return 0;
    }
    case 0x0A: {                                /* LOCK/UNLOCK: BL = 1    */
        struct shf *e;                          /* entsperren, DS:DX ->   */
        unsigned long o, n;                     /* Stelle, Laenge         */
        static unsigned char pb[8];
        peek(sl, sft, SFTLEN);
        k = r16(sft + 0x0B);
        if (sft_h(k, &e) == 0xFFFF)
            return E_HANDLE;
        if (!e)
            return 0;
        peek(lin(rg.ds, rg.dx), pb, 8);
        o = r32(pb);
        n = r32(pb + 4);
        k &= 0xFF;
        for (r = 0; r < NLK; r++) {
            struct shl *l = &lk[r];
            if (!l->used || l->slot != k)
                continue;
            if (rg.bx & 0xFF) {
                if (l->ofs == o && l->len == n) {
                    l->used = 0;
                    return 0;
                }
            } else if (o < l->ofs + l->len && l->ofs < o + n)
                return 0x21;
        }
        if (rg.bx & 0xFF)
            return 0x21;
        for (r = 0; r < NLK; r++)
            if (!lk[r].used) {
                lk[r].used = 1;
                lk[r].slot = (unsigned char)k;
                lk[r].ofs = o;
                lk[r].len = n;
                return 0;
            }
        return 0x24;
    }
    case 0x07:                                  /* COMMIT                */
    case 0x0B:                                  /* (FreeDOS: 0Ah + BL)   */
    case 0x1D: case 0x22:                       /* alle zu, Prozessende  */
        return 0;
    case 0x08:                                  /* READ                  */
    case 0x09: {                                /* WRITE                 */
        struct shf *e;
        peek(sl, sft, SFTLEN);
        h = sft_h(r16(sft + 0x0B), &e);
        pos = r32(sft + 0x15);
        size = r32(sft + 0x11);
        if (h == 0xFFFF)
            return E_HANDLE;
        l32 = pos;
        if (FAILED(DosSeek(h, &l32)))
            return E_DENIED;
        cnt = rg.cx;
        if (cnt == 0 && fn == 9) {              /* Groesse setzen (DOS): */
            l32 = pos;                          /* kuerzen/verlaengern   */
            r = DosNewSize(h, &l32);
            if (FAILED(r))
                return errmap(r, E_DENIED);
            w32(sft + 0x11, pos);
            poke(sl, sft, SFTLEN);
            if (e)
                e->size = pos;
            return 0;
        }
        peek(sda_lin + 0x0C, word2, 4);         /* DTA                   */
        k = cnt ? xfer(fn == 9, h, lin(r16(word2 + 2), r16(word2)), cnt, &err) : 0;
        if (k == 0 && err)
            return err;
        pos += k;
        w32(sft + 0x15, pos);
        if (fn == 9 && pos > size)
            w32(sft + 0x11, pos);
        poke(sl, sft, SFTLEN);
        if (e && fn == 9 && pos > e->size)
            e->size = pos;
        rg.cx = k;
        return 0;
    }
    case 0x0C: {                                /* freier Platz          */
        static struct fapi_fsinfo2 fs2;
        unsigned spc = 1, sh = 9;
        unsigned long tot, fr;
        peek(sl, sft, 1);
        k = up(sft[0]) - 'A';
        if (FAILED(DosQFSInfo(k, &fsi)) || FAILED(DosFreeSpace(k, &l32)))
            return E_NOTREADY;
        tot = fsi.bsize == 2048 ? fsi.sectors << 2 : fsi.sectors;
        while ((tot >> (sh - 9)) > 0xFFF0UL && spc < 128) {
            spc <<= 1;
            sh++;
        }
        if (!FAILED(fapi_call(FAPI_QFSINFO, k, (unsigned short)&fs2, 1)) &&
            fs2.spc && !(fs2.spc & (fs2.spc - 1))) {
            unsigned long cl = fs2.clusters;    /* FAT: echte Aufteilung  */
            unsigned cs;                        /* wie MS-DOS - Lotus     */
            for (spc = cs = fs2.spc, sh = 9; cs > 1; cs >>= 1)
                sh++;                           /* Works waehlt danach die */
            while (cl > 0xFFF0UL && spc < 128) {    /* Diskettenaufteilung */
                spc <<= 1;
                sh++;
                cl >>= 1;
            }
            tot = cl << (sh - 9);
        }
        fr = l32 >> sh;                         /* Bytes -> Cluster       */
        rg.ax = spc;
        rg.bx = (unsigned)(tot >> (sh - 9));
        rg.cx = 512;
        rg.dx = fr > 0xFFF0UL ? 0xFFF0 : (unsigned)fr;
        return 0;
    }
    case 0x0E:                                  /* Attribut setzen       */
        get_fn(0x9E, p1);
        fin.mask = 1;
        fin.attr = arg & 0x27;
        r = DosSetFInfo(p1, &fin);
        return FAILED(r) ? errmap(r, nf(p1)) : 0;
    case 0x0F:                                  /* Attribut holen        */
        get_fn(0x9E, p1);
        if (stat_path(p1))
            return nf(p1);
        rg.ax = de.attr;
        rg.bx = (unsigned)(de.size >> 16);
        rg.di = (unsigned)de.size;
        rg.cx = de.time;
        rg.dx = de.date;
        return 0;
    case 0x11:                                  /* RENAME                */
        get_fn(0x9E, p1);
        get_fn(0x11E, p2);
        if (has_wild(p1) || has_wild(p2))
            return ren_wild();
        r = DosMove(p1, p2);
        if (r == FAPI_ERR_EXIST)
            return E_DENIED;                    /* Ziel gibt es: 05h     */
        return FAILED(r) ? errmap(r, nf(p1)) : 0;
    case 0x13: {                                /* DELETE (auch *.*)     */
        const char *n;
        unsigned i = 0, hits = 0, j, l;
        get_fn(0x9E, p1);
        if (!has_wild(p1)) {
            r = DosDelete(p1);
            return FAILED(r) ? errmap(r, nf(p1)) : 0;
        }
        n = split(p1, dir);
        to_fcb(n, tmpl);
        while (!FAILED(DosFindEntry(&de, i, dir))) {
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
            if (FAILED(DosDelete(one)))
                i++;                            /* bleibt stehen: weiter */
            else
                hits++;
        }
        return hits ? 0 : nf(p1);
    }
    case 0x16:                                  /* OPEN                  */
    case 0x17:                                  /* CREATE (kuerzen)      */
    case 0x2E: {                                /* erweitert             */
        unsigned mode = arg & 0xFF, act, exists, cattr = arg;
        unsigned long hv;
        int slot;
        get_fn(0x9E, p1);
        exists = stat_path(p1) == 0;
        if (exists && (de.attr & 0x18))
            return E_DENIED;                    /* Verzeichnis/Bezeichnung */
        if (fn == 0x2E) {
            unsigned a = sda16(0x2DD);
            mode = sda16(0x2E1) & 0xFF;
            cattr = sda16(0x2DF);
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
            mode = 2;                           /* arg ist das Attribut   */
        hv = phash(p1);
        slot = sh_find(hv);
        if (slot >= 0) {                        /* Box hat sie schon offen */
            struct shf *e = &sh[slot];
            if (act != 1 || !sh_ok(e, mode))
                return E_DENIED;                /* MS-DOS 6.22 mit SHARE: */
                                                /* 05h (BIOSCMP)          */
            if (M_WR(mode) && !e->kw) {         /* Handle nur lesend      */
                DosClose(e->h1 - 1);
                h = DosOpenRW(p1);
                if (FAILED(h)) {
                    r = DosOpen(p1);
                    e->h1 = FAILED(r) ? 0 : r + 1;
                    return errmap(h, E_DENIED);
                }
                hp_note(h, p1, 0);
                e->h1 = h + 1;
                e->kw = 1;
            }
            e->refs++;
            sh_add(e, mode, 1);
            if (e->size > de.size)
                de.size = e->size;
            peek(sl, sft, SFTLEN);
            sft_fill((SH_MARK | slot) - 1, mode, p1);
            poke(sl, sft, SFTLEN);
            if (fn == 0x2E)
                ext_act(act);
            return 0;
        }
        if (act == 1)                           /* vorhanden: lesen oder  */
            h = (mode & 3) ? DosOpenRW(p1) : DosOpen(p1);   /* lesen+schr. */
        else
            h = DosCreate(p1);
        if (FAILED(h))
            return errmap(h, act == 1 ? E_NOFILE : no_dir(p1) ? E_NOPATH : E_DENIED);
        hp_note(h, p1, act != 1 && (cattr & 0x07) ? 0x80 | 0x20 | (cattr & 0x27) : 0);
        if (act != 1) {
            de.attr = 0x20 | (cattr & 0x07);
            de.size = 0;
            stat_path(p1);                      /* Datum/Uhrzeit         */
            de.size = 0;
        }
        for (slot = 0; slot < NSH && sh[slot].h1; slot++)
            ;
        if (slot < NSH) {
            struct shf *e = &sh[slot];
            e->h1 = h + 1;
            e->hash = hv;
            e->size = de.size;
            e->refs = 1;
            e->rd = e->wr = e->dr = e->dw = 0;
            e->kw = act != 1 || (mode & 3);
            sh_add(e, mode, 1);
            h = (SH_MARK | slot) - 1;
        }
        peek(sl, sft, SFTLEN);                  /* DOS prueft den Modus   */
        sft_fill(h, mode, p1);
        poke(sl, sft, SFTLEN);
        if (fn == 0x2E)
            ext_act(act);
        return 0;
    }
    case 0x1B:                                  /* FINDFIRST             */
        return find_first();
    case 0x1C:                                  /* FINDNEXT (ES:DI)      */
        peek(sl, srch, 21);
        k = find_next();
        poke(sl, srch, 21);
        return k;
    case 0x21: {                                /* SEEK vom Ende         */
        unsigned long off = ((unsigned long)rg.cx << 16) | rg.dx;
        struct shf *e;
        peek(sl, sft, SFTLEN);
        size = r32(sft + 0x11);
        if (sft_h(r16(sft + 0x0B), &e) != 0xFFFF && e && e->size > size)
            size = e->size;
        pos = size + off;
        w32(sft + 0x15, pos);
        poke(sl, sft, SFTLEN);
        rg.dx = (unsigned)(pos >> 16);
        rg.ax = (unsigned)pos;
        return 0;
    }
    }
    return -1;                                  /* nicht unser           */
}

/* Startlaufwerk (aus COMSPEC), wenn dort \DOS\MDOS\COMMAND.COM liegt -
   dann startet RBSHELL FreeCOM von dort, und A:/B: der Box sind die
   Disketten von AT-DOS (wie in 6.0); sonst 0, A: bleibt das Abbild */
static char boot_drv(void)
{
    static char cs[FAPI_PATHLEN];
    static char done, drv;
    unsigned h;
    if (done)
        return drv;
    done = 1;
    if (FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) || cs[1] != ':')
        return 0;
    cs[2] = 0;
    scat(cs, "\\DOS\\MDOS\\", sizeof(cs));
    if (rb_iname[0]) {                  /* anderes DOS: dessen COMMAND.COM */
        scat(cs, rb_iname, sizeof(cs)); /* in \DOS\MDOS\NAME (MKDOSIMG)  */
        scat(cs, "\\", sizeof(cs));
    }
    scat(cs, "COMMAND.COM", sizeof(cs));
    h = DosOpen(cs);
    if (FAILED(h))
        return 0;
    DosClose(h);
    drv = up(cs[0]);
    return drv;
}

/* AT-DOS-Laufwerke als Bitmaske - A:/B: nur, wenn die Box ihr Abbild nach
   dem Start nicht mehr braucht (boot_drv) */
static unsigned long drive_mask(void)
{
    unsigned long m = 0;
    unsigned d, r;
    for (d = boot_drv() ? 0 : 2; d < 26; d++) {
        r = DosQFSInfo(d, &fsi);
        if (!FAILED(r) || r == FAPI_ERR_NOTREADY)
            m |= 1UL << d;
    }
    return m;
}

/* INT E0h der Box (ausser 01h, das erledigt der Kernel); RB_RUN hat die
   Register nach rg geholt und setzt sie beim naechsten Aufruf zurueck */
/* INT E0h 18h (RBSHELL): Laufwerk d (0 = A:) fuer die IOCTL-Abfragen, die
   DOS bei einem Netzlaufwerk ablehnt (wie main redir.c redir_dinfo):
   nach ES:DI 32 Byte - +0 Art (1 fest, 2 Wechsel), +1 Sektoren je
   Cluster, +2 Bytes je Sektor, +4 Cluster, +8 Seriennummer, +12
   Bezeichnung, +23 Dateisystem, +31 Medienbyte. -> 0, sonst nicht */
static int dinfo(unsigned d)
{
    static struct fapi_fsinfo2 fs2;
    static unsigned char o[32];
    unsigned k, n, spc;
    unsigned long cl;
    if (FAILED(fapi_call(FAPI_QFSINFO, d, (unsigned short)&fs2, 1)))
        return 1;
    if (fs2.f.bsize == 2048 || (fs2.f.flags & FSI_NOMEDIA))
        return 1;
    spc = fs2.spc ? fs2.spc : 1;
    cl = fs2.spc ? fs2.clusters : fs2.f.sectors;
    while (cl > 0xFFF0UL && spc < 128)
        spc <<= 1, cl >>= 1;
    lb.label[0] = 0;
    lb.serial = 0;
    DosLabel(d, &lb, 0);
    for (k = 0; k < 32; k++)
        o[k] = 0;
    o[0] = (fs2.f.flags & FSI_REMOVABLE) && !(fs2.f.flags & FSI_REMDISK) ? 2 : 1;
    o[1] = (unsigned char)spc;
    w16(o + 2, fs2.f.bsize);
    w32(o + 4, cl);
    w32(o + 8, lb.serial);
    for (k = 0; k < 11 + 8; k++)
        o[12 + k] = ' ';
    for (k = 0, n = 0; lb.label[k] && n < 11; k++)
        if (lb.label[k] != '.')
            o[12 + n++] = lb.label[k];
    if (fs2.f.fsname[0] == 'F' && fs2.f.fsname[1] == 'A' && fs2.f.fsname[2] == 'T') {
        o[23] = 'F', o[24] = 'A', o[25] = 'T';
        if (fs2.f.fsname[3] == '3')
            o[26] = '3', o[27] = '2';
        else if (cl >= 4085)
            o[26] = '1', o[27] = '6';
        else
            o[26] = '1', o[27] = '2';
    }
    o[31] = o[0] == 2 || (fs2.f.flags & FSI_REMDISK) ? 0xF0 : 0xF8;
    poke(lin(rg.es, rg.di), o, 32);
    return 0;
}

static void box_call(void)
{
    int r;
    unsigned long m;

    switch (rg.ax >> 8) {                       /* rg fuellte RB_RUN */
    case 0x10:                                  /* Redirector, AL = Funktion */
        if (!sda_lin) {
            rg.flags &= ~0x40;                  /* ZF = 0: nicht unser    */
            break;
        }
        r = op(rg.ax & 0xFF);
        if (r < 0)
            rg.flags &= ~0x40;
        else {
            rg.flags |= 0x40;
            if (r) {
                rg.flags |= 1;
                rg.ax = r;
            } else
                rg.flags &= ~1;
        }
        break;
    case 0x11:                                  /* SDA ist DS:SI          */
        sda_lin = lin(rg.ds, rg.si);
        ms_dos = (rg.bx & 0xFF) != 0xFD;        /* BL: OEM aus INT 21h 30h */
        for (r = 0; r < NSH; r++)           /* neue Box: nichts offen     */
            sh[r].h1 = 0;
        for (r = 0; r < NLK; r++)
            lk[r].used = 0;
        break;
    case 0x12:                                  /* -> DX:AX Laufwerke     */
        m = drive_mask();
        rg.ax = (unsigned)m;
        rg.dx = (unsigned)(m >> 16);
        break;
    case 0x18:                                  /* Laufwerk AL wie lokal  */
        if (dinfo(rg.ax & 0xFF))                /* (IOCTL, RBSHELL)       */
            rg.flags |= 1;
        else
            rg.flags &= ~1;
        break;
    case 0x16: {                                /* Maus da? AX = Tasten,  */
        static struct fapi_sysinfo si;          /* BL = Anschluss fuer    */
        rg.ax = MouOpen();                      /* CuteMouse: 'P' PS/2,   */
        if (FAILED(rg.ax))                      /* '1'/'2' COM, 0 selbst  */
            rg.ax = 0;                          /* suchen. Die Sitzung    */
        rg.bx = 0;                              /* behaelt die Maus (keine */
        if (rg.ax && !DosSysInfo(&si))          /* Konsolenmaus); der     */
            rg.bx = (si.irq_used >> 12) & 1 ? 'P' :     /* Anschluss nach */
                    (si.irq_used >> 4) & 1 ? '1' :      /* den IRQs von  */
                    (si.irq_used >> 3) & 1 ? '2' : 0;   /* MOUSE.SYS     */
        break;
    }
    case 0x13:                                  /* Startlaufwerk, AL       */
        rg.ax = (unsigned char)boot_drv() | (unsigned)rb_perm << 8;  /* AH = 1: Box */
                                                /* bleibt (RBREADY, EXIT) */
        break;
    case 0x14: {                                /* aktuelles Verzeichnis  */
        static char cd[FAPI_CURDIRLEN];         /* von RBOX nach ES:DI    */
        if (FAILED(DosQCurDir(cd)) || slen(cd) > 127 ||
            (up(cd[0]) < 'C' && !boot_drv()))
            cd[0] = 0;                          /* A:/B: ist das Abbild    */
        poke(lin(rg.es, rg.di), cd, slen(cd) + 1);
        break;
    }
    case 0x19:                                  /* Name des Abbilds       */
        poke(lin(rg.es, rg.di), rb_iname, slen(rb_iname) + 1);
        break;
    case 0x15: {                                /* Tastaturbelegung von   */
        static unsigned char kb[KBL_BYTES];     /* AT-DOS (3 x 90 Zeichen) */
        if (FAILED(fapi_call(FAPI_KBDGETLAYOUT, (unsigned)kb, KBL_BYTES, 0)) ||
            kb[0] != 'K') {
            rg.ax = 0xFFFF;                     /* alter Kernel: BIOS (US) */
            break;
        }
        poke(lin(rg.es, rg.di), kb + 8, KBL_BYTES - 8);
        rg.ax = 0;
        break;
    }
    default:
        rg.ax = 0xFFFF;
    }
}                                               /* setzt das naechste RB_RUN */
