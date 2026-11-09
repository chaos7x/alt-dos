/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UFAT32.EXE /R - Oberflaechentest fuer FAT32 (eingebunden in
 * task_ufat32.c nach ufat32_fmt.c), wie ufat_scan.c bei UFAT: jeder
 * Sektor des Datenbereichs wird gelesen (32 Sektoren am Stueck, bei
 * Fehlern Cluster fuer Cluster). Ein defekter freier Cluster wird in der
 * FAT gesperrt (0FFFFFF7h). Gehoert er zu einer Kette, kommt sein Inhalt
 * in einen freien Cluster (Unlesbares als Nullen), die Kette wird
 * umgehaengt und der alte gesperrt - ausser er ist der erste Cluster
 * (steht im Verzeichnis bzw. im Bootsektor; dann nur Meldung). Laeuft
 * nach Baum und verlorenen Ketten, vor dem Zaehlen der freien Cluster.
 */

#define VSECS 32
static unsigned char vbuf[VSECS * 512];
static unsigned long nmoved, nmarked, nlost_secs, fhint = 2;

/* Cluster c lesbar? Zaehlt unlesbare Sektoren */
static int clus_ok(unsigned long c)
{
    unsigned s, bad = 0;
    for (s = 0; s < spc; s++)
        if (rd_secs(clus_lba(c) + s, sec, 1))
            bad++;
    nlost_secs += bad;
    return bad == 0;
}

/* Vorgaenger in der Kette, 0 = keiner (erster Cluster) */
static unsigned long pred(unsigned long c)
{
    unsigned long i;
    for (i = 2; i <= maxc; i++)
        if (fat_get(i) == c)
            return i;
    return 0;
}

/* freier, lesbarer Cluster (0 = keiner) */
static unsigned long alloc_free(void)
{
    unsigned long c, n;
    for (n = 0, c = fhint; n < clusters; n++, c++) {
        if (c > maxc)
            c = 2;
        if (fat_get(c) == 0 && !getbit(useds, c)) {
            if (!clus_ok(c)) {
                fat_put(c, BAD);        /* gleich mit sperren           */
                nmarked++;
                continue;
            }
            fhint = c + 1;
            return c;
        }
    }
    return 0;
}

/* defekten belegten Cluster c verlegen; 1 = verlegt */
static int move_bad(unsigned long c)
{
    unsigned long p = pred(c), n;
    unsigned s, i;
    if (!p)
        return 0;
    n = alloc_free();
    if (!n)
        return 0;
    setbit(useds, n);
    for (s = 0; s < spc; s++) {
        if (rd_secs(clus_lba(c) + s, sec, 1))
            for (i = 0; i < 512; i++)
                sec[i] = 0;
        if (wr_sec(clus_lba(n) + s, sec))
            return 0;
    }
    fat_put(n, fat_get(c));             /* Rest der Kette               */
    fat_put(p, n);
    fat_put(c, BAD);
    fat_flush();
    return 1;
}

static void surface_scan(void)
{
    unsigned long lba, end = data_lba + shl(clusters, shift);
    unsigned long c0, c1, v, step, next;
    unsigned chunk, pct = 0;
    static unsigned long pos;

    puts("Oberfl\x84" "chentest ...\n");
    step = udiv(end - data_lba, 100);
    if (!step)
        step = 1;
    next = data_lba + step;
    for (lba = data_lba; lba < end; lba += chunk) {
        chunk = end - lba > VSECS ? VSECS : (unsigned)(end - lba);
        pos = lba;
        if (DosSeek(hd, &pos) || DosRead(hd, vbuf, chunk << 9) != (chunk << 9)) {
            c0 = shr(lba - data_lba, shift) + 2;    /* clusterweise      */
            c1 = shr(lba + chunk - 1 - data_lba, shift) + 2;
            for (; c0 <= c1 && c0 <= maxc; c0++) {
                v = fat_get(c0);
                if (v == BAD || clus_ok(c0))
                    continue;
                problem();
                puts("\rDefekter Cluster ");
                put_num(c0, 0);
                if (v == 0) {
                    fat_put(c0, BAD);
                    nmarked++;
                    puts(" (frei) - gesperrt.\n");
                } else if (move_bad(c0)) {
                    nmoved++;
                    nmarked++;
                    puts(" (Datei) - Inhalt verlegt, Cluster gesperrt.\n");
                } else
                    puts(" (Dateianfang) - nicht verlegt; Datei sichern und l\x94schen.\n");
            }
        }
        while (lba + chunk >= next && pct < 100) {
            next += step;
            pct++;
            putc('\r');
            put_num(pct, 3);
            puts(" Prozent gepr\x81" "ft");
        }
    }
    fat_flush();
    puts("\n");
    if (nmarked || nlost_secs) {
        put_num(nlost_secs, 0);
        puts(" unlesbare(r) Sektor(en), ");
        put_num(nmarked, 0);
        puts(" Cluster gesperrt, ");
        put_num(nmoved, 0);
        puts(" verlegt.\n");
    } else
        puts("Keine fehlerhaften Sektoren gefunden.\n");
}
