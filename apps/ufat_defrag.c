/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UFAT.EXE /DEFRAG - Dateien zusammenhaengend legen (eingebunden in
 * task_ufat.c), aufgerufen von DEFRAG.EXE:
 *
 *   UFAT X: /DEFRAG [/U] [/V]
 *
 * Durchgang 1: jede fragmentierte Datei kommt in den ersten freien Bereich,
 * der gross genug ist. Durchgang 2 (ohne /U, wie DEFRAG /F): Dateien nach
 * ihrem Anfang geordnet in Luecken weiter vorn schieben, wo sie passen.
 * Verzeichnisse bleiben, wo sie sind (sonst muessten ".."-Eintraege der
 * Unterverzeichnisse mitwandern).
 *
 * Reihenfolge je Datei, damit ein Absturz nichts kostet: Daten in freie
 * Cluster kopieren -> neue Kette in die FAT schreiben (Absturz: CHKDSK
 * findet eine verlorene Kette, die Datei ist heil) -> Startcluster im
 * Verzeichniseintrag umsetzen (ein Sektor) -> alte Kette freigeben.
 */

#define DMAX 1024                       /* Verzeichnisse                  */

struct drec {
    unsigned long lba;                  /* Sektor des Eintrags            */
    unsigned off, first, n, frag;
};
static unsigned rec_sel, nrec, dq[DMAX], ndq;
static unsigned nfrag0, nmovedf;

static struct drec __far *rec(unsigned i)
{
    return (struct drec __far *)MK_FP(rec_sel, i * sizeof(struct drec));
}

/* freier Bereich von n Clustern, der vor 'before' beginnt (0 = egal) */
static unsigned find_run(unsigned n, unsigned before)
{
    unsigned c = 2, run = 0, s = 0;
    for (; c <= maxc; c++) {
        if (before && c - run >= before)
            return 0;
        if ((c & 7) == 0 && used[c >> 3] == 0xFF && c + 8 <= maxc + 1) {
            run = 0;                    /* 8 belegte am Stueck            */
            c += 7;
            continue;
        }
        if (getbit(used, c))
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

/* Datei an 'to' legen; 0 = ok */
static int move_file(struct drec __far *r, unsigned to)
{
    unsigned i, c, nx;

    for (i = 0, c = r->first; i < r->n; i++) {      /* 1. Daten kopieren */
        if (copy_clus(c, to + i))
            return -1;
        c = fat_get(c);
    }
    for (i = 0; i < r->n; i++) {                    /* 2. neue Kette     */
        fat_put(to + i, i + 1 < r->n ? to + i + 1 : EOC);
        setbit(used, to + i);
    }
    write_fat();
    for (i = 0; i < 32; i++)
        fdirty[i] = 0;
    if (load_dsec(r->lba))                          /* 3. Eintrag        */
        return -1;
    wr16(dsec + r->off + 26, to);
    if (wr_sec(r->lba, dsec))
        return -1;
    for (i = 0, c = r->first; i < r->n && valid(c); i++) {  /* 4. alt frei */
        nx = fat_get(c);
        fat_put(c, 0);
        clrbit(used, c);
        c = nx;
    }
    write_fat();
    for (i = 0; i < 32; i++)
        fdirty[i] = 0;
    r->first = to;
    r->frag = 0;
    nmovedf++;
    return 0;
}

/* Verzeichnisbaum ablaufen und Dateien sammeln */
static int collect(void)
{
    unsigned char *e;
    unsigned i, c, n, fr, prev;
    struct drec __far *r;

    ndq = 0;
    dq[ndq++] = 0;                              /* Stamm                 */
    for (i = 0; i < ndq; i++) {
        u_open(dq[i]);
        while ((e = u_next()) != 0) {
            if (e[0] == 0xE5 || e[0] == '.' || e[11] == 0x0F || (e[11] & 0x08))
                continue;
            c = rd16(e + 26);
            if (e[11] & 0x10) {
                if (valid(c) && ndq < DMAX)
                    dq[ndq++] = c;
                continue;
            }
            if (!valid(c) || !rd32(e + 28))
                continue;
            for (n = 0, fr = 0, prev = 0; valid(c) && n <= clusters; n++) {
                if (prev && c != prev + 1)
                    fr++;
                prev = c;
                c = fat_get(c);
            }
            if (n > clusters || nrec >= 65000U / sizeof(struct drec))
                continue;                       /* Schleife / zu viele    */
            r = rec(nrec++);
            r->lba = u_cur;
            r->off = u_off - 32;
            r->first = rd16(e + 26);
            r->n = n;
            r->frag = fr;
            if (fr)
                nfrag0++;
        }
    }
    return 0;
}

static void sort_recs(void)                     /* nach Anfang (Shellsort) */
{
    static struct drec t;
    unsigned gap, i, j, k;
    unsigned char __far *a, __far *b;
    for (gap = nrec / 2; gap; gap /= 2)
        for (i = gap; i < nrec; i++)
            for (j = i; j >= gap && rec(j - gap)->first > rec(j)->first; j -= gap) {
                a = (unsigned char __far *)rec(j - gap);
                b = (unsigned char __far *)rec(j);
                for (k = 0; k < sizeof(t); k++) {
                    unsigned char x = a[k];
                    a[k] = b[k];
                    b[k] = x;
                }
            }
}

static int do_defrag(int only_frag)
{
    unsigned i, c, to, failed = 0;
    struct drec __far *r;

    if (fats_differ || nfats < 1) {
        puts("Die FAT-Kopien sind verschieden - zuerst CHKDSK /F.\n");
        return 3;
    }
    root_n = (((unsigned long)rootents << 5) + 511) >> 9;
    rec_sel = DosAllocSeg(0);
    if (FAPI_FAILED(rec_sel)) {
        puts("Zu wenig Arbeitsspeicher.\n");
        return 3;
    }
    for (i = 0; i < 8192; i++)
        used[i] = 0;
    for (c = 2; c <= maxc; c++)                 /* Belegung aus der FAT   */
        if (fat_get(c) != 0)
            setbit(used, c);
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
        puts(" fragmentierte Datei(en) blieben (kein freier Bereich gross genug)");
    }
    puts(".\n");
    return failed ? 1 : 0;
}
