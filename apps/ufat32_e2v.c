/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UFAT32.EXE /CONVERT, letzter Schritt: lange Namen aus EA DATA. SF in
 * VFAT-Eintraege uebertragen (eingebunden in task_ufat32.c ueber
 * ufat32_conv.c). FAT32 schreibt lange Namen als VFAT (wie Windows 95/98);
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
static unsigned long e2v_hint, e2v_need, e2v_names;
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
    unsigned long first = dc ? dc : rootc, c, prev;
    unsigned n, bytes, i, o = 0, ro, k, parts, cb = spc << 9;
    unsigned long nb;
    unsigned char __far *e;
    unsigned char sum;

    if ((bytes = e2v_read(first, e2v_old, &n)) == 0xFFFF)
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
            } else
                o += 32 * parts;
        } else if ((unsigned long)o + 32 > 0xFFE0UL)
            return -1;
        if (write)
            for (k = 0; k < 32; k++)
                *fpb(e2v_new, o + k) = e[k];
        o += 32;
    }
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
        used32++;
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
    if ((bytes = e2v_read(rootc, e2v_old, &n)) == 0xFFFF)
        return -1;
    for (i = 0; i + 32 <= bytes; i += 32) {
        e = fpb(e2v_old, i);
        if (!e[0])
            break;
        for (k = 0; k < 11 && e[k] == (unsigned char)ean[k]; k++)
            ;
        if (k < 11 || e[11] == 0x0F)
            continue;
        c = ((unsigned long)rd16f(e + 20) << 16) | rd16f(e + 26);
        while (valid(c)) {
            nx = fat_get(c);
            fat_put(c, 0);
            if (used32)
                used32--;
            c = nx;
        }
        if (rd_secs(clus_lba(e2v_cl[i / (spc << 9)]) + ((i % (spc << 9)) >> 9), sec, 1))
            return -1;
        sec[i & 511] = 0xE5;
        return wr_sec(clus_lba(e2v_cl[i / (spc << 9)]) + ((i % (spc << 9)) >> 9), sec);
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
