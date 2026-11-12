/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Lange Namen EA DATA. SF <-> VFAT, gemeinsam fuer UFAT32.EXE (eingebunden
 * ueber ufat32_conv.c: /CONVERT und /LFN) und UFAT.EXE (FAT12/16, /LFN).
 * Der Gastgeber setzt vorher E2V_FIXROOT (1 = fester Stamm wie FAT12/16,
 * root_lba/rootents; 0 = Stamm als Kette ab rootc), E2V_USED(d) (Zaehler
 * belegter Cluster) und ENT_CLUS(e) (Startcluster eines Eintrags).
 *
 * UFAT32.EXE /CONVERT, letzter Schritt: lange Namen aus EA DATA. SF in
 * VFAT-Eintraege uebertragen. FAT32 schreibt lange Namen als VFAT (wie Windows 95/98);
 * nach der Umwandlung stehen die Namen eines FAT16-Laufwerks aber noch in
 * EA DATA. SF. Ohne /EA (CONVERT setzt es bei LONGNAMES=EA in der
 * CONFIG.SYS) wird jedes betroffene Verzeichnis neu geschrieben: vor dem
 * 8.3-Eintrag eines Satzes stehen danach die VFAT-Eintraege mit dem
 * langen Namen; dann wird EA DATA. SF geloescht.
 *
 * Erst wird alles geprueft (Groesse der Verzeichnisse, freie Cluster),
 * geschrieben nur, wenn alles passt - sonst bleibt EA DATA. SF, und der
 * Kernel liest die Namen weiter daraus (LONGNAMES=AUTO sieht die Datei).
 * Grenzen: EA-Datei und jedes Verzeichnis bis 512 KB.
 */

/* CP437 128-255 -> Unicode (fuer die VFAT-Eintraege) */
static const unsigned short cp437u[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

/* Verzeichnisse bis 512 KB (16.384 Eintraege): der Puffer liegt in bis zu
   acht Segmenten zu 64 KB, geholt erst bei Bedarf (hp). Vorher ein
   Segment - bei 32-KB-Clustern scheiterte schon ein Verzeichnis mit zwei
   Clustern (vom Nutzer gemeldet: "Verzeichnis ueber 64 KB"). Eintraege
   und Sektoren liegen nie ueber einer Segmentgrenze. */
#define E2V_NS   8
#define E2V_MAX  (0x10000UL * E2V_NS - 32)
#define E2V_MAXC 1024                   /* Cluster je Verzeichnis (Liste) */
#define E2V_ERR  0xFFFFFFFFUL
/* EA DATA. SF (gelesen bzw. neu gebaut) bis 512 KB in bis zu acht
   Segmenten (ep); Saetze beginnen auf 32-Byte-Grenzen, ein Kopf (18 Byte)
   liegt nie ueber einer Segmentgrenze, ein ganzer Satz schon - Saetze
   werden ueber e2v_rec (nah) gelesen und geschrieben */
#define E2V_NE   8
#define E2V_EMAX (0x10000UL * E2V_NE)
static unsigned e2v_eh[E2V_NE];
static unsigned char e2v_rec[18 + 512 + 32];
static unsigned e2v_dl, e2v_dn;         /* Saetze des Verzeichnisses (Liste) */
static unsigned e2v_old[E2V_NS], e2v_new[E2V_NS];
static unsigned long e2v_cl[E2V_MAXC];
static unsigned long e2v_hint, e2v_need, e2v_names, e2v_rootn;
static unsigned long e2v_eabytes;

/* Byte off im Puffer h aus ns Segmenten (bei Bedarf geholt); 0 = kein
   Speicher */
static unsigned char __far *hpn(unsigned *h, unsigned ns, unsigned long off)
{
    unsigned k = (unsigned)(off >> 16);
    if (k >= ns)
        return 0;
    if (!h[k] && !(h[k] = seg64()))
        return 0;
    return (unsigned char __far *)MK_FP(h[k], (unsigned)off);
}

#define hp(h, off) hpn(h, E2V_NS, off)
static unsigned char __far *ep(unsigned long off) { return hpn(e2v_eh, E2V_NE, off); }

/* n Byte ab Satz off (32-Byte-Grenze) <-> d */
static int eget(unsigned long off, unsigned char *d, unsigned n)
{
    unsigned k, j;
    unsigned char __far *p;
    for (k = 0; k < n; k += 32) {
        if (!(p = ep(off + k)))
            return -1;
        for (j = 0; j < 32 && k + j < n; j++)
            d[k + j] = p[j];
    }
    return 0;
}
static int eput(unsigned long off, const unsigned char *d, unsigned n)
{
    unsigned k, j;
    unsigned char __far *p;
    for (k = 0; k < n; k += 32) {
        if (!(p = ep(off + k)))
            return -1;
        for (j = 0; j < 32 && k + j < n; j++)
            p[j] = d[k + j];
    }
    return 0;
}

static void hfree_n(unsigned *h, unsigned ns)
{
    unsigned k;
    for (k = 0; k < ns; k++)
        if (h[k]) {
            DosFreeSeg(h[k]);
            h[k] = 0;
        }
}

static void hfree(unsigned *h)
{
    unsigned k;
    for (k = 0; k < E2V_NS; k++)
        if (h[k]) {
            DosFreeSeg(h[k]);
            h[k] = 0;
        }
}

static unsigned rd16f(const unsigned char __far *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long rd32f(const unsigned char __far *p)
{
    return rd16f(p) | ((unsigned long)rd16f(p + 2) << 16);
}

/* Sektor in den Puffer h bei off (512 Byte, nie ueber eine Grenze) */
static int e2v_rsec(unsigned long lba, unsigned *h, unsigned long off)
{
    unsigned char __far *p = hp(h, off);
    unsigned k;
    if (!p || rd_secs(lba, sec, 1))
        return -1;
    for (k = 0; k < 512; k++)
        p[k] = sec[k];
    return 0;
}

/* Sektor aus dem Puffer h bei off schreiben */
static int e2v_wsec(unsigned long lba, unsigned *h, unsigned long off)
{
    unsigned char __far *p = hp(h, off);
    unsigned k;
    if (!p)
        return -1;
    for (k = 0; k < 512; k++)
        sec[k] = p[k];
    return wr_sec(lba, sec);
}

/* Kette ab first nach h lesen (bis E2V_MAX) -> Bytes, Cluster in e2v_cl
   und *n; E2V_ERR = zu gross oder Fehler */
static unsigned long e2v_read(unsigned long first, unsigned *h, unsigned *n)
{
    unsigned long c = first, steps = 0, off = 0, cb = (unsigned long)spc << 9;
    unsigned s;
    *n = 0;
    while (valid(c) && steps++ <= maxc) {
        if (*n >= E2V_MAXC || off + cb > E2V_MAX)
            return E2V_ERR;
        e2v_cl[(*n)++] = c;
        for (s = 0; s < spc; s++, off += 512)
            if (e2v_rsec(clus_lba(c) + s, h, off))
                return E2V_ERR;
        c = fat_get(c);
    }
    return off;
}

/* Verzeichnis dc (0 = Stamm) nach h -> Bytes (E2V_ERR = Fehler) */
static unsigned long e2v_dread(unsigned long dc, unsigned *h, unsigned *n)
{
#if E2V_FIXROOT
    if (!dc) {                          /* fester Stamm (FAT12/16)        */
        unsigned s, ns = (rootents + 15) >> 4;
        *n = 0;
        if (rootents > 2046)
            return E2V_ERR;
        for (s = 0; s < ns; s++)
            if (e2v_rsec(root_lba + s, h, (unsigned long)s << 9))
                return E2V_ERR;
        return (unsigned long)rootents << 5;
    }
#endif
    return e2v_read(dc ? dc : rootc, h, n);
}

/* Sektor, in dem Byte off des eben gelesenen Verzeichnisses dc steht */
static unsigned long e2v_slba(unsigned long dc, unsigned long off)
{
#if E2V_FIXROOT
    if (!dc)
        return root_lba + (off >> 9);
#endif
    (void)dc;
    return clus_lba(e2v_cl[(unsigned)shr(off, cshift)]) +
           (unsigned)((off & (((unsigned long)spc << 9) - 1)) >> 9);
}

/* Satzarten: L CP437, W UTF-16 (Unicode Stufe 1); klein = schon uebertragen */
#define E2V_REC(x) ((x) == 'L' || (x) == 'l' || (x) == 'W' || (x) == 'w')

static unsigned char e2v_sum(const unsigned char __far *e)
{
    unsigned char s = 0;
    unsigned i;
    for (i = 0; i < 11; i++)
        s = (unsigned char)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

/* Liste der Saetze von Verzeichnis dc (Offsets, bis 16.384) */
static void e2v_dlist(unsigned long dc)
{
    unsigned long o, __far *l = (unsigned long __far *)MK_FP(e2v_dl, 0);
    unsigned char __far *r;
    e2v_dn = 0;
    for (o = 0; o + 32 <= e2v_eabytes; o += (unsigned long)r[1] << 5) {
        if (!(r = ep(o)) || !r[1])
            break;
        if (E2V_REC(r[0] & 0x7F) && rd32f(r + 2) == dc && e2v_dn < 16384)
            l[e2v_dn++] = o;
    }
}

/* Satz zu diesem 8.3-Namen (aus der Liste) -> Offset, sonst E2V_ERR */
static unsigned long e2v_find(unsigned long dc, const unsigned char __far *e)
{
    unsigned long __far *l = (unsigned long __far *)MK_FP(e2v_dl, 0);
    unsigned i, k;
    unsigned char __far *r;
    (void)dc;
    for (i = 0; i < e2v_dn; i++) {
        r = ep(l[i]);
        for (k = 0; k < 11 && r[6 + k] == e[k]; k++)
            ;
        if (k == 11)
            return l[i];
    }
    return E2V_ERR;
}

/* ein VFAT-Eintrag an off in e2v_new: Teil k (1..n) des Namens r */
static int e2v_lfn(unsigned long off, const unsigned char __far *r, unsigned k, unsigned n,
                   unsigned char sum)
{
    static const unsigned char pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    unsigned char __far *e = hp(e2v_new, off);
    unsigned len = r[17], i, idx, u;
    if (!e)
        return -1;
    for (i = 0; i < 32; i++)
        e[i] = 0;
    e[0] = (unsigned char)(k | (k == n ? 0x40 : 0));
    e[11] = 0x0F;
    e[13] = sum;
    for (i = 0; i < 13; i++) {
        idx = (k - 1) * 13 + i;
        if (idx < len) {
            if ((r[0] | 0x20) == 'w')   /* UTF-16: unveraendert         */
                u = rd16f(r + 18 + 2 * idx);
            else {
                u = r[18 + idx];
                if (u >= 128)
                    u = cp437u[u - 128];
            }
        } else
            u = idx == len ? 0 : 0xFFFF;
        e[pos[i]] = (unsigned char)u;
        e[pos[i] + 1] = (unsigned char)(u >> 8);
    }
    return 0;
}

/* Verzeichnis dc (neue Nummer, 0 = Stamm) neu aufbauen; write = 0 nur
   pruefen (Groesse, e2v_need). -> 0 ok, -1 passt nicht */
static int e2v_dir(unsigned long dc, int write)
{
    unsigned long c, prev, bytes, i, o = 0, nb, cb = (unsigned long)spc << 9, ro;
    unsigned n, k, parts;
    unsigned char __far *e, __far *d;
    unsigned char sum;

    if ((bytes = e2v_dread(dc, e2v_old, &n)) == E2V_ERR)
        return -1;
    for (i = 0; i + 32 <= bytes; i += 32) {
        if (!(e = hp(e2v_old, i)))
            return -1;
        if (!e[0])
            break;
        if (e[0] == 0xE5)               /* Geloeschtes faellt beim Neuaufbau
                                           weg (sonst passte ein Ordner nach
                                           /LFN:EA nicht mehr zurueck) */
            continue;
        if (e[0] != 0xE5 && e[11] != 0x0F && !(e[11] & 8) &&
            (ro = e2v_find(dc, e)) != E2V_ERR) {
            unsigned char __far *h = ep(ro);
            parts = (h[17] + 12) / 13;
            if (o + 32UL * (parts + 1) > E2V_MAX)
                return -1;
            if (write) {
                if (eget(ro, e2v_rec, 18 + ((h[0] | 0x20) == 'w' ? 2 * h[17] : h[17])))
                    return -1;
                sum = e2v_sum(e);
                for (k = parts; k >= 1; k--, o += 32)
                    if (e2v_lfn(o, e2v_rec, k, parts, sum))
                        return -1;
                ep(ro)[0] |= 0x20;                  /* erledigt (l/w)     */
                e2v_names++;
                if (!dc)
                    e2v_rootn++;
            } else
                o += 32 * parts;
        } else if (o + 32 > E2V_MAX)
            return -1;
        if (write) {
            if (!(d = hp(e2v_new, o)) || !(e = hp(e2v_old, i)))
                return -1;
            for (k = 0; k < 32; k++)
                d[k] = e[k];
        }
        o += 32;
    }
#if E2V_FIXROOT
    if (!dc) {                          /* fester Stamm: waechst nicht    */
        if (o > bytes)
            return -1;
        if (!write)
            return 0;
        for (i = o; i < bytes; i += 32)
            if ((d = hp(e2v_new, i)) != 0)
                for (k = 0; k < 32; k++)
                    d[k] = 0;
        for (i = 0; i < bytes; i += 512)
            if (e2v_wsec(root_lba + (i >> 9), e2v_new, i))
                return -1;
        return 0;
    }
#endif
    nb = shr(o + 32 + cb - 1, cshift);  /* Platz fuer das Ende           */
    if (nb > E2V_MAXC || shl(nb, cshift) > E2V_MAX + 32)
        return -1;
    if (!write) {
        if (nb > n)
            e2v_need += nb - n;
        return 0;
    }
    for (i = o; i < shl(nb, cshift); i += 32) {
        if (!(d = hp(e2v_new, i)))
            return -1;
        for (k = 0; k < 32; k++)
            d[k] = 0;
    }
    prev = e2v_cl[n - 1];
    while (n < nb) {                    /* Verzeichnis waechst            */
        for (c = e2v_hint; c <= maxc && fat_get(c) != 0; c++)
            ;
        if (c > maxc)
            return -1;
        e2v_hint = c + 1;
        fat_put(prev, c);
        fat_put(c, EOC);
        E2V_USED(1);
        e2v_cl[n++] = c;
        prev = c;
    }
    for (k = 0; k < n; k++)
        for (i = 0; i < spc; i++)
            if (e2v_wsec(clus_lba(e2v_cl[k]) + (unsigned)i, e2v_new, shl(k, cshift) + (i << 9)))
                return -1;
    return 0;
}

/* alle Verzeichnisse mit Saetzen: pruefen (write 0) bzw. umbauen (1) */
static int e2v_pass(int write)
{
    unsigned long o, dc, __far *l = (unsigned long __far *)MK_FP(e2v_dl, 0);
    unsigned char __far *r;
    unsigned i;
    int rc = 0;
    for (o = 0; o + 32 <= e2v_eabytes && !rc; o += (unsigned long)r[1] << 5) {
        if (!(r = ep(o)) || !r[1])
            break;
        if (r[0] == 'L' || r[0] == 'W') {   /* Verzeichnis noch nicht dran */
            dc = rd32f(r + 2);
            e2v_dlist(dc);
            rc = e2v_dir(dc, write);
            for (i = 0; i < e2v_dn; i++)    /* Bit 7: Verzeichnis behandelt */
                ep(l[i])[0] |= 0x80;
        }
    }
    for (o = 0; o + 32 <= e2v_eabytes; o += (unsigned long)r[1] << 5) {
        if (!(r = ep(o)) || !r[1])
            break;
        r[0] &= 0x7F;
    }
    return rc;
}

/* EA DATA. SF im Stamm loeschen (Eintrag frei, Kette frei) */
static int e2v_drop(void)
{
    unsigned n, k;
    unsigned long c, nx, bytes, i;
    static const char ean[] = "EA DATA SF ";
    unsigned char __far *e;
    if ((bytes = e2v_dread(0, e2v_old, &n)) == E2V_ERR)
        return -1;
    for (i = 0; i + 32 <= bytes; i += 32) {
        if (!(e = hp(e2v_old, i)))
            return -1;
        if (!e[0])
            break;
        for (k = 0; k < 11 && e[k] == (unsigned char)ean[k]; k++)
            ;
        if (k < 11 || e[11] == 0x0F)
            continue;
        c = ENT_CLUS(e);
        while (valid(c)) {
            nx = fat_get(c);
            fat_put(c, 0);
            E2V_USED(-1);
            c = nx;
        }
        if (rd_secs(e2v_slba(0, i), sec, 1))
            return -1;
        sec[(unsigned)i & 511] = 0xE5;
        return wr_sec(e2v_slba(0, i), sec);
    }
    return 0;
}

/* -> 0 umgewandelt oder nichts zu tun, 1 Namen bleiben in EA DATA. SF */
static int e2v_run(unsigned long eafirst, unsigned long easize)
{
    unsigned long c = eafirst, off = 0, steps = 0;
    unsigned s;
    int rc = 1;
    if (!eafirst || !easize)
        return 0;
    if (easize > E2V_EMAX || !(e2v_dl = seg64()))
        goto out;
    while (valid(c) && off < easize && steps++ <= maxc) {   /* EA-Datei lesen */
        for (s = 0; s < spc && off < easize; s++, off += 512) {
            unsigned char __far *p = ep(off);
            unsigned k;
            if (!p || rd_secs(clus_lba(c) + s, sec, 1))
                goto out;
            for (k = 0; k < 512; k++)
                p[k] = sec[k];
        }
        c = fat_get(c);
    }
    e2v_eabytes = off < easize ? off : easize;
    e2v_need = 0;
    if (e2v_pass(0))
        goto out;
    e2v_hint = 2;
    if (e2v_need) {                     /* genug freie Cluster?          */
        unsigned long c, f = 0;
        for (c = 2; c <= maxc && f < e2v_need; c++)
            if (fat_get(c) == 0)
                f++;
        if (f < e2v_need)
            goto out;
    }
    e2v_names = 0;
    if (e2v_pass(1) || e2v_drop())
        cv_err = 1;
    fat_flush();
    rc = 0;
out:
    hfree_n(e2v_eh, E2V_NE);
    if (e2v_dl)
        DosFreeSeg(e2v_dl);
    e2v_dl = 0;
    hfree(e2v_old);
    hfree(e2v_new);
    return rc;
}

/* ---- CONVERT X: /LFN:VFAT|EA auf einem FAT32-Laufwerk ------------------ */

/* Eintrag "EA DATA SF " im Stamm -> erster Cluster, Groesse; 0 = gefunden */
/* ein Stammsektor in sec: 1 gefunden, -1 Ende des Stamms, 0 weiter */
static int e2v_ckea(unsigned long *first, unsigned long *size)
{
    static const char ean[] = "EA DATA SF ";
    unsigned o, k;
    for (o = 0; o < 512; o += 32) {
        if (!sec[o])
            return -1;
        if (sec[o] == 0xE5 || sec[o + 11] == 0x0F)
            continue;
        for (k = 0; k < 11 && sec[o + k] == (unsigned char)ean[k]; k++)
            ;
        if (k == 11) {
            *first = ENT_CLUS((unsigned char __far *)(sec + o));
            *size = rd32(sec + o + 28);
            return 1;
        }
    }
    return 0;
}

static int e2v_locate(unsigned long *first, unsigned long *size)
{
    int r;
#if E2V_FIXROOT
    unsigned s;
    for (s = 0; s < (rootents + 15) >> 4; s++)
        if (rd_secs(root_lba + s, sec, 1) || (r = e2v_ckea(first, size)) < 0)
            return -1;
        else if (r)
            return 0;
    return -1;
#else
    unsigned long c = rootc, steps = 0;
    unsigned s;
    while (valid(c) && steps++ <= maxc) {
        for (s = 0; s < spc; s++)
            if (rd_secs(clus_lba(c) + s, sec, 1) || (r = e2v_ckea(first, size)) < 0)
                return -1;
            else if (r)
                return 0;
        c = fat_get(c);
    }
    return -1;
#endif
}

/* FSInfo: freie Cluster unbekannt (nach Belegen/Freigeben) */
static void e2v_fsinfo(void)
{
#if E2V_FIXROOT
}
#else
    if (!fsis || fsis == 0xFFFF || rd_secs(fsis, sec, 1) || rd32(sec) != 0x41615252UL)
        return;
    wr32(sec + 488, 0xFFFFFFFFUL);
    wr_sec(fsis, sec);
}
#endif

static int lfn_tovfat(void)
{
    unsigned long first, size;
    if (e2v_locate(&first, &size)) {
        puts("Keine langen Namen im EA-Format (EA DATA. SF fehlt).\n");
        return 0;
    }
    if (!autom) {
        puts("Lange Namen aus EA DATA. SF in VFAT-Eintr\x84ge \x81" "bertragen (J/N)? ");
        if (!yes())
            return 3;
    }
    e2v_names = e2v_rootn = 0;
    if (e2v_run(first, size)) {
        puts("Nicht m\x94glich (Verzeichnis \x81" "ber 512 KB oder zu wenig Platz) - nichts ge\x84ndert.\n");
        return 3;
    }
    e2v_fsinfo();
    put_num(e2v_names, 0);
    puts(" lange Namen nach VFAT \x81" "bertragen, EA DATA. SF gel\x94scht.\n");
#if E2V_FIXROOT
    if (e2v_names && !e2v_rootn)        /* AUTO erkennt VFAT am Stamm     */
        puts("Im Stammverzeichnis steht kein langer Name: damit AT-DOS neue lange\n"
             "Namen auch als VFAT schreibt, LONGNAMES=VFAT in die CONFIG.SYS setzen.\n");
#endif
    return cv_err ? 3 : 0;
}

/* VFAT -> EA: UCS-2 -> CP437, Unbekanntes '_' */
static unsigned char v2e_cp(unsigned u)
{
    unsigned i;
    if (u < 128)
        return (unsigned char)u;
    for (i = 0; i < 128; i++)
        if (cp437u[i] == u)
            return (unsigned char)(128 + i);
    return '_';
}

static unsigned v2e_dq;                 /* Verzeichnis-Startcluster (je 4 Byte) */
static unsigned long v2e_nd;
static unsigned long v2e_lost;

/* Verzeichnis (dc 0 = Stamm) ablaufen: phase 0 Saetze sammeln und
   Unterverzeichnisse merken, phase 1 VFAT-Eintraege loeschen */
static int v2e_dir(unsigned long dc, int phase)
{
    static unsigned short nm[256];      /* UCS-2, 0 = Ende               */
    static unsigned char seen;
    static unsigned char chk, parts;
    unsigned n, k, j, len, dirty = 0;
    unsigned long bytes, i;
    unsigned char __far *e;
    unsigned long sub;

    if ((bytes = e2v_dread(dc, e2v_old, &n)) == E2V_ERR)
        return -1;
    parts = 0;
    for (i = 0; i + 32 <= bytes; i += 32) {
        if (!(e = hp(e2v_old, i)))
            return -1;
        if (!e[0])
            break;
        if (e[0] == 0xE5) {
            parts = 0;
            continue;
        }
        if (e[11] == 0x0F) {
            if (phase) {
                e[0] = 0xE5;
                dirty = 1;
                continue;
            }
            if (e[0] & 0x40) {
                parts = e[0] & 0x1F;
                chk = e[13];
                seen = 0;
                for (k = 0; k < 256; k++)
                    nm[k] = 0;
            }
            k = e[0] & 0x1F;
            if (!parts || !k || k > parts || e[13] != chk || k > 19)
                parts = 0;
            else {
                static const unsigned char pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (j = 0; j < 13; j++) {
                    unsigned u = e[pos[j]] | ((unsigned)e[pos[j] + 1] << 8);
                    unsigned at = (k - 1) * 13 + j;
                    if (at < 255)
                        nm[at] = (unsigned short)(u == 0xFFFF ? 0 : u);
                }
                seen |= (unsigned char)(k == 1);
            }
            continue;
        }
        if (!phase && parts && seen && e2v_sum(e) == chk && !(e[11] & 8)) {
            unsigned units, wide = 0;   /* Satz anhaengen             */
            unsigned long o = v2e_nd;
            for (len = 0; len < 255 && nm[len]; len++)
                if (nm[len] != '_' && v2e_cp(nm[len]) == '_')
                    wide = 1;               /* ausserhalb CP437: Satz W   */
            if (wide)
                v2e_lost++;
            units = (18 + (wide ? 2 * len : len) + 31) >> 5;
            if (o + ((unsigned)units << 5) + 32 > E2V_EMAX)
                return -2;                  /* EA-Datei zu gross           */
            for (k = 0; k < units * 32; k++)
                e2v_rec[k] = 0;
            e2v_rec[0] = (unsigned char)(wide ? 'W' : 'L');
            e2v_rec[1] = (unsigned char)units;
            for (k = 0; k < 4; k++)
                e2v_rec[2 + k] = (unsigned char)(dc >> (8 * k));
            for (k = 0; k < 11; k++)
                e2v_rec[6 + k] = e[k];
            e2v_rec[17] = (unsigned char)len;
            for (k = 0; k < len; k++)
                if (wide) {
                    e2v_rec[18 + 2 * k] = (unsigned char)nm[k];
                    e2v_rec[19 + 2 * k] = (unsigned char)(nm[k] >> 8);
                } else
                    e2v_rec[18 + k] = (unsigned char)v2e_cp(nm[k]);
            if (eput(o, e2v_rec, units * 32))
                return -1;
            v2e_nd = o + (unsigned long)units * 32;
            e2v_names++;
        }
        parts = 0;
        if (!phase && (e[11] & 0x10) && e[0] != '.') {      /* Unterverzeichnis */
            sub = ENT_CLUS(e);
            if (valid(sub)) {
                unsigned long __far *q = (unsigned long __far *)MK_FP(v2e_dq, 0);
                unsigned long __far *cnt = q;           /* q[0] = Anzahl */
                if (*cnt >= 16383)
                    return -1;
                q[++*cnt] = sub;
            }
        }
    }
    if (phase && dirty)                 /* geaenderte Sektoren zurueck    */
        for (i = 0; i < bytes; i += 512)
            if (e2v_wsec(e2v_slba(dc, i), e2v_old, i))
                return -1;
    return 0;
}

/* alle Verzeichnisse (Stamm, dann Warteschlange) */
static int v2e_all(int phase)
{
    unsigned long __far *q = (unsigned long __far *)MK_FP(v2e_dq, 0);
    unsigned long i;
    int rc;
    if ((rc = v2e_dir(0, phase)) != 0)
        return rc;
    for (i = 1; i <= q[0]; i++)
        if ((rc = v2e_dir(q[i], phase)) != 0)
            return rc;
    return 0;
}

/* EA DATA. SF anlegen: Kette fuer v2e_nd Byte, Inhalt, Eintrag im Stamm */
static int v2e_write(void)
{
    unsigned long c, prev = 0, first = 0, hint = 2;
    unsigned long cb = (unsigned long)spc << 9, bytes, slot;
    unsigned nc = (unsigned)shr((unsigned long)v2e_nd + cb - 1, cshift), k, s, b;
    unsigned n;
    for (k = 0; k < nc; k++) {
        for (c = hint; c <= maxc && fat_get(c) != 0; c++)
            ;
        if (c > maxc)
            return -1;
        hint = c + 1;
        if (prev)
            fat_put(prev, c);
        else
            first = c;
        fat_put(c, EOC);
        prev = c;
        for (s = 0; s < spc; s++) {
            unsigned long at = shl(k, cshift) + ((unsigned)s << 9);
            for (b = 0; b < 512; b++)
                sec[b] = at + b < v2e_nd ? ep(at)[b] : 0;
            if (wr_sec(clus_lba(c) + s, sec))
                return -1;
        }
    }
    if ((bytes = e2v_dread(0, e2v_old, &n)) == E2V_ERR)
        return -1;
    for (slot = 0; slot + 32 <= bytes; slot += 32) {
        unsigned char __far *x = hp(e2v_old, slot);
        if (!x)
            return -1;
        if (x[0] == 0 || x[0] == 0xE5)
            break;
    }
    if (slot + 32 > bytes)
        return -1;                      /* Stamm voll (selten)            */
    if (rd_secs(e2v_slba(0, slot), sec, 1))
        return -1;
    b = (unsigned)slot & 511;
    for (k = 0; k < 32; k++)
        sec[b + k] = 0;
    for (k = 0; k < 11; k++)
        sec[b + k] = (unsigned char)"EA DATA SF "[k];
    sec[b + 11] = 0x06;                 /* versteckt + System             */
    wr16(sec + b + 20, (unsigned)(first >> 16));
    wr16(sec + b + 26, (unsigned)first);
    wr32(sec + b + 28, v2e_nd);
    {                                   /* Datum/Zeit jetzt (FAT-Format) */
        static unsigned dt[2];
        if (FAPI_FAILED(DosGetDateTime(dt)) || !dt[0])
            dt[0] = 0x0021, dt[1] = 0;  /* 1.1.1980                      */
        wr16(sec + b + 22, dt[1]);
        wr16(sec + b + 24, dt[0]);
        wr16(sec + b + 16, dt[0]);      /* angelegt / letzter Zugriff    */
        wr16(sec + b + 14, dt[1]);
        wr16(sec + b + 18, dt[0]);
    }
    return wr_sec(e2v_slba(0, slot), sec);
}

static int lfn_toea(void)
{
    unsigned long f, sz;
    int rc = 3;
    if (e2v_locate(&f, &sz) == 0) {
        puts("Das Laufwerk hat schon EA DATA. SF - erst /LFN:VFAT.\n");
        return 3;
    }
    v2e_dq = seg64();
    if (!v2e_dq) {
        puts("Zu wenig Arbeitsspeicher.\n");
        goto out;
    }
    *(unsigned long __far *)MK_FP(v2e_dq, 0) = 0;
    v2e_nd = 0;
    v2e_lost = 0;
    e2v_names = 0;
    if ((rc = v2e_all(0)) != 0) {
        puts(rc == -2 ? "Zu viele lange Namen f\x81r EA DATA. SF (512 KB) - nichts ge\x84ndert.\n"
                      : "Verzeichnis \x81" "ber 512 KB oder Lesefehler - nichts ge\x84ndert.\n");
        rc = 3;
        goto out;
    }
    if (!e2v_names) {
        puts("Keine VFAT-Namen gefunden.\n");
        rc = 0;
        goto out;
    }
    put_num(e2v_names, 0);
    puts(" lange Namen nach EA DATA. SF \x81" "bertragen");
    if (v2e_lost) {
        puts(" (");
        put_num(v2e_lost, 0);
        puts(" mit Zeichen au\xE1" "erhalb CP437 - bleiben erhalten)");
    }
    puts(".\n");
    if (!autom) {
        puts("Windows 95/98 sieht die Namen danach nicht mehr. Fortfahren (J/N)? ");
        if (!yes()) {
            rc = 3;
            goto out;
        }
    }
    if (v2e_write() || v2e_all(1)) {
        puts("Schreibfehler - das Laufwerk ist besch\x84" "digt.\n");
        rc = 3;
    } else
        rc = 0;
    fat_flush();
    e2v_fsinfo();
    if (!rc)
        puts("Fertig. In der CONFIG.SYS LONGNAMES=EA oder AUTO setzen.\n");
out:
    hfree(e2v_old);
    hfree_n(e2v_eh, E2V_NE);
    if (v2e_dq)
        DosFreeSeg(v2e_dq);
    return rc;
}
