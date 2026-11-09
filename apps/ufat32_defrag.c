/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UFAT32.EXE /DEFRAG - Dateien auf FAT32 zusammenhaengend legen
 * (eingebunden in task_ufat32.c nach ufat32_undel.c), aufgerufen von
 * DEFRAG.EXE:
 *
 *   UFAT32 X: /DEFRAG [/U] [/V]
 *
 * Wie ufat_defrag.c (UFAT): Durchgang 1 legt jede fragmentierte Datei in
 * den ersten freien Bereich, der gross genug ist; Durchgang 2 (ohne /U)
 * schiebt Dateien nach ihrem Anfang geordnet in Luecken weiter vorn.
 * Verzeichnisse (auch der Stamm) bleiben, wo sie sind. Belegung als
 * Bitmap in den Segmenten von `useds`, Dateien (Eintragssektor,
 * Startcluster, Laenge) in bis zu 4 DosAllocSeg-Segmenten (16.384
 * Dateien), Verzeichnisse in einer Warteschlange (DMAX).
 * Reihenfolge je Datei: Daten kopieren -> neue Kette in die FAT ->
 * Startcluster im Eintrag umsetzen -> alte Kette frei. Ein Absturz
 * dazwischen kostet keine Daten (CHKDSK findet hoechstens eine verlorene
 * Kette).
 */

#define DMAX  1024                      /* Verzeichnisse                  */
#define NRSEG 4                         /* Segmente fuer Dateisaetze      */
#define RPS   4096                      /* Saetze je Segment (16 Byte)    */

struct drec {
    unsigned long lba;                  /* Sektor des Eintrags            */
    unsigned long first, n;
    unsigned off, frag;
};
static unsigned rsegs[NRSEG], nrseg;
static unsigned long dq[DMAX];
static unsigned ndq, nrec, nfrag0, nmovedf;

static struct drec __far *rec(unsigned i)
{
    return (struct drec __far *)MK_FP(rsegs[i >> 12], (i & (RPS - 1)) * sizeof(struct drec));
}

/* freier Bereich von n Clustern, der vor 'before' beginnt (0 = egal) */
static unsigned long find_run(unsigned long n, unsigned long before)
{
    unsigned long c, run = 0, s = 0;
    unsigned char m, __far *p;
    for (c = 2; c <= maxc; c++) {
        if (before && c - run >= before)
            return 0;
        p = bm(useds, c, &m);
        if (m == 1 && *p == 0xFF && c + 8 <= maxc + 1) {
            run = 0;                    /* 8 belegte am Stueck            */
            c += 7;
            continue;
        }
        if (*p & m)
            run = 0;
        else {
            if (!run)
                s = c;
            if (++run == n)
                return (!before || s < before) ? s : 0;
        }
    }
    return 0;
}

/* Cluster kopieren, je 8 Sektoren ueber zb (ufat32_fmt.c) */
static int copy_clus(unsigned long from, unsigned long to)
{
    unsigned s, k;
    unsigned long a = clus_lba(from), b = clus_lba(to);
    static unsigned long pos;
    for (s = 0; s < spc; s += k) {
        k = spc - s > 8 ? 8 : spc - s;
        if (rd_secs(a + s, zb, k))
            return -1;
        pos = b + s;
        if (DosSeek(hd, &pos) || DosWrite(hd, zb, k << 9) != (k << 9))
            return -1;
    }
    return 0;
}

/* Datei an 'to' legen; 0 = ok */
static int move_file(struct drec __far *r, unsigned long to)
{
    unsigned long i, c, nx;

    for (i = 0, c = r->first; i < r->n; i++) {      /* 1. Daten kopieren */
        if (!valid(c) || copy_clus(c, to + i))
            return -1;
        c = fat_get(c);
    }
    for (i = 0; i < r->n; i++) {                    /* 2. neue Kette     */
        fat_put(to + i, i + 1 < r->n ? to + i + 1 : EOC);
        setbit(useds, to + i);
    }
    fat_flush();
    if (ferr || load_dsec(r->lba))                  /* 3. Eintrag        */
        return -1;
    set_first(dsec + r->off, to);
    if (wr_sec(r->lba, dsec))
        return -1;
    for (i = 0, c = r->first; i < r->n && valid(c); i++) {  /* 4. alt frei */
        nx = fat_get(c);
        fat_put(c, 0);
        clrbit(useds, c);
        c = nx;
    }
    fat_flush();
    r->first = to;
    r->frag = 0;
    nmovedf++;
    return 0;
}

/* Verzeichnisbaum ablaufen und Dateien sammeln */
static void collect(void)
{
    unsigned char *e;
    unsigned i;
    unsigned long c, n, prev;
    unsigned fr;
    struct drec __far *r;

    ndq = 0;
    dq[ndq++] = 0;                              /* Stamm                 */
    for (i = 0; i < ndq; i++) {
        u_open(dq[i]);
        while ((e = u_next()) != 0) {
            if (e[0] == 0xE5 || e[0] == '.' || e[11] == 0x0F || (e[11] & 0x08))
                continue;
            c = ent_first(e);
            if (e[11] & 0x10) {
                if (valid(c) && ndq < DMAX)
                    dq[ndq++] = c;
                continue;
            }
            if (!valid(c) || !rd32(e + 28))
                continue;
            for (n = 0, fr = 0, prev = 0; valid(c) && n <= clusters; n++) {
                if (prev && c != prev + 1)
                    fr = 1;
                prev = c;
                c = fat_get(c);
            }
            if (n > clusters)
                continue;                       /* Schleife               */
            if ((nrec >> 12) >= nrseg) {        /* naechstes Segment      */
                if (nrseg >= NRSEG)
                    continue;
                rsegs[nrseg] = DosAllocSeg(0);
                if (FAPI_FAILED(rsegs[nrseg]) || !rsegs[nrseg])
                    continue;
                nrseg++;
            }
            r = rec(nrec++);
            r->lba = u_cur;
            r->off = u_off - 32;
            r->first = ent_first(e);
            r->n = n;
            r->frag = fr;
            if (fr)
                nfrag0++;
        }
    }
}

static void sort_recs(void)                     /* nach Anfang (Shellsort) */
{
    unsigned gap, i, j, k;
    unsigned char __far *a, __far *b, x;
    for (gap = nrec / 2; gap; gap /= 2)
        for (i = gap; i < nrec; i++)
            for (j = i; j >= gap && rec(j - gap)->first > rec(j)->first; j -= gap) {
                a = (unsigned char __far *)rec(j - gap);
                b = (unsigned char __far *)rec(j);
                for (k = 0; k < sizeof(struct drec); k++) {
                    x = a[k];
                    a[k] = b[k];
                    b[k] = x;
                }
            }
}

static int do_defrag(int only_frag)
{
    unsigned i, failed = 0;
    unsigned long c, to;
    struct drec __far *r;

    if (fats_differ) {
        puts("Die FAT-Kopien sind verschieden - zuerst CHKDSK /F.\n");
        return 3;
    }
    puts("Belegung wird gelesen ...\n");
    for (c = 2; c <= maxc; c++)                 /* Belegung aus der FAT   */
        if (fat_get(c) != 0)
            setbit(useds, c);
    collect();
    puts("Dateien: ");
    put_num(nrec, 0);
    puts(", davon fragmentiert: ");
    put_num(nfrag0, 0);
    puts("\n");

    for (i = 0; i < nrec; i++) {                /* 1. zusammenhaengend    */
        r = rec(i);
        if (!r->frag)
            continue;
        to = find_run(r->n, 0);
        if (!to || move_file(r, to)) {
            failed++;
            continue;
        }
        if (verbose) {
            puts("  zusammengelegt: Cluster ");
            put_num(to, 0);
            puts("\n");
        }
        putc('.');
    }
    if (!only_frag) {                           /* 2. Luecken fuellen     */
        sort_recs();
        for (i = 0; i < nrec; i++) {
            r = rec(i);
            if (r->frag)
                continue;
            to = find_run(r->n, r->first);
            if (to && to < r->first && move_file(r, to) == 0)
                putc('.');
        }
    }
    puts("\n");
    put_num(nmovedf, 0);
    puts(" Datei(en) verschoben");
    if (failed) {
        puts(", ");
        put_num(failed, 0);
        puts(" fragmentierte Datei(en) blieben (kein freier Bereich gro\xE1 genug)");
    }
    puts(".\n");
    return failed ? 1 : 0;
}
