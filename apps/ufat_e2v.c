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
 * Grenzen: EA-Datei und jedes Verzeichnis bis 64 KB.
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

#define E2V_MAXC 128                    /* Cluster je Verzeichnis (Liste) */
static unsigned e2v_ea, e2v_old, e2v_new;   /* Segmente zu 64 KB          */
static unsigned long e2v_cl[E2V_MAXC];
static unsigned long e2v_hint, e2v_need, e2v_names, e2v_rootn;
static unsigned e2v_eabytes;

static unsigned char __far *fpb(unsigned seg, unsigned off)
{
    return (unsigned char __far *)MK_FP(seg, off);
}
static unsigned rd16f(const unsigned char __far *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long rd32f(const unsigned char __far *p)
{
    return rd16f(p) | ((unsigned long)rd16f(p + 2) << 16);
}

/* Kette ab first in seg lesen (hoechstens 64 KB) -> Bytes, Cluster in
   e2v_cl und *n; 0xFFFF = zu gross oder Fehler */
static unsigned e2v_read(unsigned long first, unsigned seg, unsigned *n)
{
    unsigned long c = first, steps = 0;
    unsigned s, k, off = 0, cb = spc << 9;
    *n = 0;
    while (valid(c) && steps++ <= maxc) {
        if (*n >= E2V_MAXC || (unsigned long)off + cb > 0x10000UL)
            return 0xFFFF;
        e2v_cl[(*n)++] = c;
        for (s = 0; s < spc; s++) {
            if (rd_secs(clus_lba(c) + s, sec, 1))
                return 0xFFFF;
            for (k = 0; k < 512; k++)
                *fpb(seg, off + k) = sec[k];
            off += 512;
        }
        c = fat_get(c);
        if (off == 0)                   /* genau 64 KB voll              */
            return 0xFFFF;
    }
    return off;
}

/* Verzeichnis dc (0 = Stamm) nach seg -> Bytes (0xFFFF = Fehler) */
static unsigned e2v_dread(unsigned long dc, unsigned seg, unsigned *n)
{
#if E2V_FIXROOT
    if (!dc) {                          /* fester Stamm (FAT12/16)        */
        unsigned s, k, ns = (rootents + 15) >> 4;
        *n = 0;
        if (rootents > 2046)
            return 0xFFFF;
        for (s = 0; s < ns; s++) {
            if (rd_secs(root_lba + s, sec, 1))
                return 0xFFFF;
            for (k = 0; k < 512; k++)
                *fpb(seg, (s << 9) + k) = sec[k];
        }
        return rootents << 5;
    }
#endif
    return e2v_read(dc ? dc : rootc, seg, n);
}

/* Sektor, in dem Byte off des eben gelesenen Verzeichnisses dc steht */
static unsigned long e2v_slba(unsigned long dc, unsigned off)
{
#if E2V_FIXROOT
    if (!dc)
        return root_lba + (off >> 9);
#endif
    (void)dc;
    return clus_lba(e2v_cl[(unsigned)shr(off, cshift)]) + ((off & ((spc << 9) - 1)) >> 9);
}

static unsigned char e2v_sum(const unsigned char __far *e)
{
    unsigned char s = 0;
    unsigned i;
    for (i = 0; i < 11; i++)
        s = (unsigned char)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

/* Satz zu diesem 8.3-Namen in Verzeichnis dc? -> Offset in e2v_ea, sonst 0xFFFF */
static unsigned e2v_find(unsigned long dc, const unsigned char __far *e)
{
    unsigned o = 0, k;
    unsigned char __far *r;
    while (o + 32 <= e2v_eabytes) {
        r = fpb(e2v_ea, o);
        if (!r[1])
            break;
        if ((r[0] == 'L' || r[0] == 'l') && rd32f(r + 2) == dc) {
            for (k = 0; k < 11 && r[6 + k] == e[k]; k++)
                ;
            if (k == 11)
                return o;
        }
        if ((unsigned long)o + ((unsigned)r[1] << 5) > 0xFFFFUL)
            break;
        o += (unsigned)r[1] << 5;
    }
    return 0xFFFF;
}

/* ein VFAT-Eintrag an off in e2v_new: Teil k (1..n) des Namens r */
static void e2v_lfn(unsigned off, const unsigned char __far *r, unsigned k, unsigned n,
                    unsigned char sum)
{
    static const unsigned char pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    unsigned char __far *e = fpb(e2v_new, off);
    unsigned len = r[17], i, idx, u;
    for (i = 0; i < 32; i++)
        e[i] = 0;
    e[0] = (unsigned char)(k | (k == n ? 0x40 : 0));
    e[11] = 0x0F;
    e[13] = sum;
    for (i = 0; i < 13; i++) {
        idx = (k - 1) * 13 + i;
        if (idx < len) {
            u = r[18 + idx];
            if (u >= 128)
                u = cp437u[u - 128];
        } else
            u = idx == len ? 0 : 0xFFFF;
        e[pos[i]] = (unsigned char)u;
        e[pos[i] + 1] = (unsigned char)(u >> 8);
    }
}

/* Verzeichnis dc (neue Nummer, 0 = Stamm) neu aufbauen; write = 0 nur
   pruefen (Groesse, e2v_need). -> 0 ok, -1 passt nicht */
static int e2v_dir(unsigned long dc, int write)
{
    unsigned long c, prev;
    unsigned n, bytes, i, o = 0, ro, k, parts, cb = spc << 9;
    unsigned long nb;
    unsigned char __far *e;
    unsigned char sum;

    if ((bytes = e2v_dread(dc, e2v_old, &n)) == 0xFFFF)
        return -1;
    for (i = 0; i + 32 <= bytes; i += 32) {
        e = fpb(e2v_old, i);
        if (!e[0])
            break;
        if (e[0] != 0xE5 && e[11] != 0x0F && !(e[11] & 8) &&
            (ro = e2v_find(dc, e)) != 0xFFFF) {
            parts = (fpb(e2v_ea, ro)[17] + 12) / 13;
            if ((unsigned long)o + 32UL * (parts + 1) > 0xFFE0UL)
                return -1;
            if (write) {
                sum = e2v_sum(e);
                for (k = parts; k >= 1; k--, o += 32)
                    e2v_lfn(o, fpb(e2v_ea, ro), k, parts, sum);
                fpb(e2v_ea, ro)[0] = 'l';           /* erledigt           */
                e2v_names++;
                if (!dc)
                    e2v_rootn++;
            } else
                o += 32 * parts;
        } else if ((unsigned long)o + 32 > 0xFFE0UL)
            return -1;
        if (write)
            for (k = 0; k < 32; k++)
                *fpb(e2v_new, o + k) = e[k];
        o += 32;
    }
#if E2V_FIXROOT
    if (!dc) {                          /* fester Stamm: waechst nicht    */
        if (o > bytes)
            return -1;
        if (!write)
            return 0;
        for (i = o; i < bytes; i++)
            *fpb(e2v_new, i) = 0;
        for (i = 0; i < bytes; i += 512) {
            unsigned j;
            for (j = 0; j < 512; j++)
                sec[j] = *fpb(e2v_new, i + j);
            if (wr_sec(root_lba + (i >> 9), sec))
                return -1;
        }
        return 0;
    }
#endif
    nb = shr((unsigned long)o + 32 + cb - 1, cshift);   /* Platz fuer das Ende */
    if (nb > E2V_MAXC || shl(nb, cshift) > 0x10000UL)
        return -1;
    if (!write) {
        if (nb > n)
            e2v_need += nb - n;
        return 0;
    }
    for (i = o; (unsigned long)i < shl(nb, cshift) && i != 0; i++)
        *fpb(e2v_new, i) = 0;
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
        for (i = 0; i < spc; i++) {
            unsigned long at = shl(k, cshift) + ((unsigned)i << 9);
            unsigned j;
            for (j = 0; j < 512; j++)
                sec[j] = at < shl(nb, cshift) ? *fpb(e2v_new, (unsigned)at + j) : 0;
            if (wr_sec(clus_lba(e2v_cl[k]) + i, sec))
                return -1;
        }
    return 0;
}

/* alle Verzeichnisse mit Saetzen: pruefen (write 0) bzw. umbauen (1) */
static int e2v_pass(int write)
{
    unsigned o = 0, p;
    unsigned char __far *r;
    unsigned long dc;
    while (o + 32 <= e2v_eabytes) {
        r = fpb(e2v_ea, o);
        if (!r[1])
            break;
        if (r[0] == 'L') {
            dc = rd32f(r + 2);
            for (p = 0; p < o; ) {      /* Verzeichnis schon behandelt?  */
                unsigned char __far *q = fpb(e2v_ea, p);
                if ((q[0] == 'L' || q[0] == 'l') && rd32f(q + 2) == dc)
                    break;
                p += (unsigned)q[1] << 5;
            }
            if (p >= o && e2v_dir(dc, write))
                return -1;
        }
        o += (unsigned)r[1] << 5;
    }
    return 0;
}

/* EA DATA. SF im Stamm loeschen (Eintrag frei, Kette frei) */
static int e2v_drop(void)
{
    unsigned n, bytes, i, k;
    unsigned long c, nx;
    static const char ean[] = "EA DATA SF ";
    unsigned char __far *e;
    if ((bytes = e2v_dread(0, e2v_old, &n)) == 0xFFFF)
        return -1;
    for (i = 0; i + 32 <= bytes; i += 32) {
        e = fpb(e2v_old, i);
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
        sec[i & 511] = 0xE5;
        return wr_sec(e2v_slba(0, i), sec);
    }
    return 0;
}

/* -> 0 umgewandelt oder nichts zu tun, 1 Namen bleiben in EA DATA. SF */
static int e2v_run(unsigned long eafirst, unsigned long easize)
{
    unsigned n;
    int rc = 1;
    if (!eafirst || !easize)
        return 0;
    e2v_ea = seg64();
    e2v_old = seg64();
    e2v_new = seg64();
    if (!e2v_ea || !e2v_old || !e2v_new || easize > 0xFFE0UL ||
        (e2v_eabytes = e2v_read(eafirst, e2v_ea, &n)) == 0xFFFF)
        goto out;
    if (e2v_eabytes > (unsigned)easize)
        e2v_eabytes = (unsigned)easize;
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
    if (e2v_ea)
        DosFreeSeg(e2v_ea);
    if (e2v_old)
        DosFreeSeg(e2v_old);
    if (e2v_new)
        DosFreeSeg(e2v_new);
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
        puts("Nicht m\x94glich (Verzeichnis \x81" "ber 64 KB oder zu wenig Platz) - nichts ge\x84ndert.\n");
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
static unsigned v2e_nd, v2e_eab;
static unsigned long v2e_lost;

/* Verzeichnis (dc 0 = Stamm) ablaufen: phase 0 Saetze sammeln und
   Unterverzeichnisse merken, phase 1 VFAT-Eintraege loeschen */
static int v2e_dir(unsigned long dc, int phase)
{
    static unsigned char nm[256], seen;
    static unsigned char chk, parts;
    unsigned n, bytes, i, k, j, len, dirty = 0;
    unsigned char __far *e;
    unsigned long sub;

    if ((bytes = e2v_dread(dc, e2v_old, &n)) == 0xFFFF)
        return -1;
    parts = 0;
    for (i = 0; i + 32 <= bytes; i += 32) {
        e = fpb(e2v_old, i);
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
                        nm[at] = u == 0 || u == 0xFFFF ? 0 : v2e_cp(u);
                    if (u && u != 0xFFFF && u != '_' && v2e_cp(u) == '_')
                        v2e_lost++;
                }
                seen |= (unsigned char)(k == 1);
            }
            continue;
        }
        if (!phase && parts && seen && e2v_sum(e) == chk && !(e[11] & 8)) {
            unsigned units, o = v2e_nd;     /* Satz anhaengen             */
            for (len = 0; len < 255 && nm[len]; len++)
                ;
            units = (18 + len + 31) >> 5;
            if ((unsigned long)o + ((unsigned)units << 5) + 32 > 0xFFFFUL)
                return -2;                  /* EA-Datei zu gross           */
            for (k = 0; k < units * 32; k++)
                *fpb(v2e_eab, o + k) = 0;
            *fpb(v2e_eab, o) = 'L';
            *fpb(v2e_eab, o + 1) = (unsigned char)units;
            for (k = 0; k < 4; k++)
                *fpb(v2e_eab, o + 2 + k) = (unsigned char)(dc >> (8 * k));
            for (k = 0; k < 11; k++)
                *fpb(v2e_eab, o + 6 + k) = e[k];
            *fpb(v2e_eab, o + 17) = (unsigned char)len;
            for (k = 0; k < len; k++)
                *fpb(v2e_eab, o + 18 + k) = nm[k];
            v2e_nd = o + units * 32;
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
        for (k = 0; k < bytes; k += 512) {
            for (j = 0; j < 512; j++)
                sec[j] = *fpb(e2v_old, k + j);
            if (wr_sec(e2v_slba(dc, k), sec))
                return -1;
        }
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
    unsigned cb = spc << 9, nc = (unsigned)shr((unsigned long)v2e_nd + cb - 1, cshift), k, s, b, slot;
    unsigned n, bytes;
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
                sec[b] = at + b < v2e_nd ? *fpb(v2e_eab, (unsigned)at + b) : 0;
            if (wr_sec(clus_lba(c) + s, sec))
                return -1;
        }
    }
    if ((bytes = e2v_dread(0, e2v_old, &n)) == 0xFFFF)
        return -1;
    for (slot = 0; slot + 32 <= bytes; slot += 32) {
        unsigned char x = *fpb(e2v_old, slot);
        if (x == 0 || x == 0xE5)
            break;
    }
    if (slot + 32 > bytes)
        return -1;                      /* Stamm voll (selten)            */
    if (rd_secs(e2v_slba(0, slot), sec, 1))
        return -1;
    b = slot & 511;
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
    e2v_old = seg64();
    v2e_eab = seg64();
    v2e_dq = seg64();
    if (!e2v_old || !v2e_eab || !v2e_dq) {
        puts("Zu wenig Arbeitsspeicher.\n");
        goto out;
    }
    *(unsigned long __far *)MK_FP(v2e_dq, 0) = 0;
    v2e_nd = 0;
    v2e_lost = 0;
    e2v_names = 0;
    if ((rc = v2e_all(0)) != 0) {
        puts(rc == -2 ? "Zu viele lange Namen f\x81r EA DATA. SF (64 KB) - nichts ge\x84ndert.\n"
                      : "Verzeichnis \x81" "ber 64 KB oder Lesefehler - nichts ge\x84ndert.\n");
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
        puts(" Zeichen au\xE1" "erhalb CP437 werden '_')");
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
    if (e2v_old)
        DosFreeSeg(e2v_old);
    if (v2e_eab)
        DosFreeSeg(v2e_eab);
    if (v2e_dq)
        DosFreeSeg(v2e_dq);
    e2v_old = 0;
    return rc;
}
