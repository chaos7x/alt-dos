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
#define SFTLEN      0x2B

static struct fapi_rbregs rg;
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
static char slast[4][11];               /* zuletzt gefundener Name       */
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
    peek(sda_lin + x, d, 128);
    d[127] = 0;
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
    w16(sft + 0x02, mode);
    sft[0x04] = de.attr;
    w16(sft + 0x05, 0x8040 | (up(path[0]) - 'A'));
    w32(sft + 0x07, 0);
    w16(sft + 0x0B, h + 1);
    w16(sft + 0x0D, de.time);
    w16(sft + 0x0F, de.date);
    w32(sft + 0x11, de.size);
    w32(sft + 0x15, 0);
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
    unsigned n = r16(srch + 13), slot = srch[15] & 3, k;
    unsigned char mask = srch[12], a;
    if (n == 0xFFFF)
        return E_NOMORE;                    /* nach der Bezeichnung      */
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
        for (k = 0; k < 11 && fcb[k] == slast[slot][k]; k++)
            ;
        if (k < 11)
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
    for (k = 0; k < 11; k++)
        slast[slot][k] = fcb[k];
    w16(srch + 13, n + 1);
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
    unsigned slot = sdir_next++ & 3, k;
    unsigned char a;
    get_fn(0x9E, p1);
    n = split(p1, sdir[slot]);
    to_fcb(*n ? n : "????????.???", tmpl);
    peek(sda_lin + 0x24D, &a, 1);
    srch[0] = (unsigned char)(0x80 | (up(p1[0]) - 'A'));
    for (k = 0; k < 11; k++)
        srch[1 + k] = tmpl[k];
    srch[12] = a;
    w16(srch + 13, 0);
    srch[15] = (unsigned char)slot;
    for (k = 16; k < 21; k++)
        srch[k] = 0;
    k = find_next();
    poke(sda_lin + 0x19E, srch, 21);
    return k == E_NOMORE ? E_NOFILE : k;
}

/* ---- Verteiler ---------------------------------------------------------------- */

/* -> -1 nicht unser, 0 erledigt, sonst DOS-Fehler */
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
        r = fn == 1 ? DosRmDir(p1) : DosMkDir(p1);
        return FAILED(r) ? errmap(r, fn == 1 ? E_DENIED : E_NOPATH) : 0;
    case 0x05:                                  /* CHDIR: gibt es das?   */
        get_fn(0x9E, p1);
        if (stat_path(p1) || !(de.attr & 0x10))
            return E_NOPATH;
        return 0;
    case 0x06:                                  /* CLOSE                 */
        peek(sl, sft, SFTLEN);
        cnt = r16(sft);
        h = r16(sft + 0x0B) - 1;
        if (cnt)
            w16(sft, --cnt);
        if (cnt == 0 && h != 0xFFFF) {
            DosClose(h);
            w16(sft + 0x0B, 0);
        }
        poke(sl, sft, SFTLEN);
        return 0;
    case 0x07:                                  /* COMMIT                */
    case 0x0A: case 0x0B:                       /* LOCK/UNLOCK           */
    case 0x1D: case 0x22:                       /* alle zu, Prozessende  */
        return 0;
    case 0x08:                                  /* READ                  */
    case 0x09:                                  /* WRITE                 */
        peek(sl, sft, SFTLEN);
        h = r16(sft + 0x0B) - 1;
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
        rg.cx = k;
        return 0;
    case 0x0C: {                                /* freier Platz          */
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
        return FAILED(r) ? errmap(r, E_NOFILE) : 0;
    case 0x0F:                                  /* Attribut holen        */
        get_fn(0x9E, p1);
        if (stat_path(p1))
            return E_NOFILE;
        rg.ax = de.attr;
        rg.bx = (unsigned)(de.size >> 16);
        rg.di = (unsigned)de.size;
        rg.cx = de.time;
        rg.dx = de.date;
        return 0;
    case 0x11:                                  /* RENAME                */
        get_fn(0x9E, p1);
        get_fn(0x11E, p2);
        r = DosMove(p1, p2);
        return FAILED(r) ? errmap(r, E_NOFILE) : 0;
    case 0x13: {                                /* DELETE (auch *.*)     */
        const char *n;
        unsigned i = 0, hits = 0, j, l;
        get_fn(0x9E, p1);
        if (!has_wild(p1)) {
            r = DosDelete(p1);
            return FAILED(r) ? errmap(r, E_NOFILE) : 0;
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
        return hits ? 0 : E_NOFILE;
    }
    case 0x16:                                  /* OPEN                  */
    case 0x17:                                  /* CREATE (kuerzen)      */
    case 0x2E: {                                /* erweitert             */
        unsigned mode = arg & 0xFF, act, exists;
        get_fn(0x9E, p1);
        exists = stat_path(p1) == 0;
        if (exists && (de.attr & 0x18))
            return E_DENIED;                    /* Verzeichnis/Bezeichnung */
        if (fn == 0x2E) {
            unsigned a = sda16(0x2DD);
            mode = sda16(0x2E1) & 0xFF;
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
            h = (mode & 3) ? DosOpenRW(p1) : DosOpen(p1);   /* lesen+schr. */
        else
            h = DosCreate(p1);
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
        peek(sl, sft, SFTLEN);                  /* DOS prueft den Modus   */
        sft_fill(h, mode, p1);
        poke(sl, sft, SFTLEN);
        if (fn == 0x2E)
            rg.cx = act;
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
        peek(sl, sft, SFTLEN);
        pos = r32(sft + 0x11) + off;
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
    scat(cs, "\\DOS\\MDOS\\COMMAND.COM", sizeof(cs));
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
        break;
    case 0x12:                                  /* -> DX:AX Laufwerke     */
        m = drive_mask();
        rg.ax = (unsigned)m;
        rg.dx = (unsigned)(m >> 16);
        break;
    case 0x13:                                  /* Startlaufwerk, AL       */
        rg.ax = (unsigned char)boot_drv();
        break;
    case 0x14: {                                /* aktuelles Verzeichnis  */
        static char cd[FAPI_CURDIRLEN];         /* von RBOX nach ES:DI    */
        if (FAILED(DosQCurDir(cd)) || slen(cd) > 127 ||
            (up(cd[0]) < 'C' && !boot_drv()))
            cd[0] = 0;                          /* A:/B: ist das Abbild    */
        poke(lin(rg.es, rg.di), cd, slen(cd) + 1);
        break;
    }
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
