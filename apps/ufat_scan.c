/*
 * UFAT.EXE /R - Oberflaechentest (eingebunden in task_ufat.c), wie
 * CHKDSK /R von Windows NT bzw. SCANDISK von MS-DOS 6.2: jeder Sektor des
 * Datenbereichs wird gelesen (16 Sektoren am Stueck, bei Fehlern Cluster
 * fuer Cluster). Ein defekter freier Cluster wird in der FAT gesperrt
 * (FFF7h). Gehoert er zu einer Datei, kommt sein Inhalt in einen freien
 * Cluster (unlesbare Sektoren als Nullen), die Kette wird umgehaengt und
 * der alte gesperrt - ausser er ist der erste Cluster einer Datei (der
 * steht im Verzeichnis; dann nur Meldung). Laeuft nach der Pruefung mit
 * /F, Aenderungen gehen mit write_fat() hinaus.
 */

static unsigned nmoved, nmarked, nlost_secs;

/* Cluster c lesbar? Zaehlt unlesbare Sektoren */
static int clus_ok(unsigned c)
{
    unsigned s, bad = 0;
    for (s = 0; s < spc; s++)
        if (rd_secs(clus_lba(c) + s, sec, 1))
            bad++;
    nlost_secs += bad;
    return bad == 0;
}

/* Vorgaenger in der Kette, 0 = keiner (erster Cluster einer Datei) */
static unsigned pred(unsigned c)
{
    unsigned i;
    for (i = 2; i <= maxc; i++)
        if (fat_get(i) == c)
            return i;
    return 0;
}

/* defekten belegten Cluster c verlegen; 1 = verlegt */
static int move_bad(unsigned c)
{
    unsigned p = pred(c), n, s;
    if (!p)
        return 0;
    n = alloc_free();
    if (!n)
        return 0;
    setbit(used, n);
    for (s = 0; s < spc; s++) {
        if (rd_secs(clus_lba(c) + s, sec, 1)) {
            unsigned i;
            for (i = 0; i < 512; i++)
                sec[i] = 0;
        }
        if (wr_sec(clus_lba(n) + s, sec))
            return 0;
    }
    fat_put(n, fat_get(c));             /* Rest der Kette               */
    fat_put(p, n);
    fat_put(c, BAD);
    return 1;
}

static void surface_scan(void)
{
    unsigned long lba, end = data_lba + shl((unsigned long)clusters, shift);
    unsigned long c0, c1, step, next;
    unsigned chunk, v, pct = 0;

    puts("Oberfl\x84" "chentest ...\n");
    step = udiv(end - data_lba, 100) ? udiv(end - data_lba, 100) : 1;
    next = data_lba + step;
    for (lba = data_lba; lba < end; lba += chunk) {
        chunk = end - lba > VCHUNK ? VCHUNK : (unsigned)(end - lba);
        if (rd_vsecs(lba, chunk)) {         /* Fehler: clusterweise      */
            c0 = shr(lba - data_lba, shift) + 2;
            c1 = shr(lba + chunk - 1 - data_lba, shift) + 2;
            for (; c0 <= c1; c0++) {
                v = fat_get((unsigned)c0);
                if (v == BAD || clus_ok((unsigned)c0))
                    continue;
                problem();
                puts("\rDefekter Cluster ");
                put_num(c0, 0);
                if (v == 0) {
                    fat_put((unsigned)c0, BAD);
                    nmarked++;
                    puts(" (frei) - gesperrt.\n");
                } else if (move_bad((unsigned)c0)) {
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
