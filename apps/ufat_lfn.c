/*
 * UFAT.EXE - lange Namen pruefen (Stufe 4, eingebunden in task_ufat.c,
 * laeuft bei jedem CHKDSK nach der Baumpruefung):
 *
 * VFAT: Ketten langer Eintraege (Attribut 0Fh) muessen lueckenlos von der
 * hoechsten Ordnungszahl (| 40h) bis 1 laufen, ueberall dieselbe Pruefsumme
 * tragen und direkt vor dem 8.3-Eintrag mit genau dieser Pruefsumme stehen.
 * Sonst sind sie verwaist (z. B. ein altes DOS-Werkzeug hat die Datei
 * umbenannt). /F gibt sie frei (E5h).
 *
 * Eigenes Format: jeder Satz in EA DATA. SF verweist auf Verzeichnis-
 * cluster + 8.3-Name. Fehlt der Eintrag (geloescht oder umbenannt unter
 * MS-DOS), ist der Satz verwaist; /F gibt ihn frei.
 */

#define FIXMAX 64

static unsigned long fix_lba[FIXMAX];
static unsigned fix_off[FIXMAX], nfix;
static unsigned lfn_orphans, ea_orphans;
static unsigned long pend_lba[20];
static unsigned pend_off[20], npend;

static unsigned char sum83(const unsigned char *e)
{
    unsigned char s = 0;
    unsigned i;
    for (i = 0; i < 11; i++)
        s = (unsigned char)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

static void orphan_pending(void)
{
    unsigned i;
    if (!npend)
        return;
    lfn_orphans++;
    for (i = 0; i < npend && nfix < FIXMAX; i++, nfix++) {
        fix_lba[nfix] = pend_lba[i];
        fix_off[nfix] = pend_off[i];
    }
    npend = 0;
}

/* gibt es im Verzeichnis dc einen (nicht geloeschten) Eintrag n83? */
static int dir_has(unsigned dc, const unsigned char __far *n83)
{
    unsigned char *e;
    unsigned k;
    u_open(dc);
    while ((e = u_next()) != 0) {
        if (e[0] == 0xE5 || e[11] == 0x0F)
            continue;
        for (k = 0; k < 11 && e[k] == n83[k]; k++)
            ;
        if (k == 11)
            return 1;
    }
    return 0;
}

static int is_dir(unsigned dc)
{
    unsigned i;
    for (i = 0; i < ndq; i++)
        if (dq[i] == dc)
            return 1;
    return 0;
}

/* Saetze in EA DATA. SF pruefen (Datei bis 64 KB) */
static void check_ea(void)
{
    static const unsigned char ean[11] = { 'E', 'A', ' ', 'D', 'A', 'T', 'A', ' ', 'S', 'F', ' ' };
    static unsigned char dirty[128];
    unsigned char *e;
    unsigned char __far *b;
    unsigned first = 0, sel, pos, u, k, blk, c, s;
    unsigned long size = 0, lba;

    u_open(0);
    while ((e = u_next()) != 0) {
        for (k = 0; k < 11 && e[k] == ean[k]; k++)
            ;
        if (k == 11 && e[11] != 0x0F) {
            first = rd16(e + 26);
            size = rd32(e + 28);
            break;
        }
    }
    if (!valid(first) || !size)
        return;
    if (size > 65024UL)
        size = 65024UL;                 /* Rest bleibt ungeprueft        */
    sel = DosAllocSeg(0);
    if (FAPI_FAILED(sel))
        return;
    b = MK_FP(sel, 0);
    for (c = first, pos = 0; pos < (unsigned)size && valid(c); c = fat_get(c))
        for (s = 0; s < spc && pos < (unsigned)size; s++, pos += 512) {
            if (rd_secs(clus_lba(c) + s, sec, 1))
                return;
            fcopy(b + pos, (unsigned char __far *)sec, 512);
        }
    for (k = 0; k < 128; k++)
        dirty[k] = 0;
    for (pos = 0; pos + 18 <= (unsigned)size; pos += u << 5) {
        u = b[pos + 1];
        if (!u)
            break;
        if (b[pos] != 'L')
            continue;
        c = b[pos + 2] | ((unsigned)b[pos + 3] << 8);   /* FAT12/16: 16 Bit */
        if ((c == 0 || is_dir(c)) && dir_has(c, b + pos + 6))
            continue;
        ea_orphans++;
        if (fix) {
            b[pos] = 0;                 /* Satz frei                      */
            dirty[pos >> 9] = 1;
        }
    }
    if (!fix || !ea_orphans)
        return;
    for (c = first, blk = 0; blk < 128 && blk * 512U < (unsigned)size && valid(c); c = fat_get(c))
        for (s = 0; s < spc && blk * 512U < (unsigned)size; s++, blk++)
            if (dirty[blk]) {
                lba = clus_lba(c) + s;
                fcopy((unsigned char __far *)sec, b + blk * 512U, 512);
                wr_sec(lba, sec);
            }
}

static void check_lfn(void)
{
    unsigned char *e;
    unsigned i, ord, expect = 0, c, k;
    unsigned char sum = 0;

    root_n = (((unsigned long)rootents << 5) + 511) >> 9;
    ndq = 0;
    dq[ndq++] = 0;
    for (i = 0; i < ndq; i++) {
        npend = 0;
        nfix = 0;
        u_open(dq[i]);
        while ((e = u_next()) != 0) {
            if (e[11] == 0x0F && e[0] != 0xE5) {    /* langer Eintrag    */
                ord = e[0] & 0x3F;
                if (e[0] & 0x40) {
                    orphan_pending();
                    expect = ord;
                    sum = e[13];
                } else if (!npend || ord != expect - 1 || e[13] != sum) {
                    orphan_pending();       /* Kette unterbrochen         */
                    expect = 0xFF;
                }
                if (ord == 0 || ord > 20)
                    expect = 0xFF;
                else if (expect != 0xFF)
                    expect = ord;
                if (npend < 20) {
                    pend_lba[npend] = u_cur;
                    pend_off[npend++] = u_off - 32;
                }
                if (expect == 0xFF)
                    orphan_pending();
                continue;
            }
            if (e[0] == 0xE5) {
                orphan_pending();
                continue;
            }
            if (npend && !(expect == 1 && sum83(e) == sum))
                orphan_pending();       /* passt nicht zum 8.3-Eintrag    */
            npend = 0;
            if ((e[11] & 0x10) && e[0] != '.' && !(e[11] & 0x08)) {
                c = rd16(e + 26);
                for (k = 0; k < ndq && dq[k] != c; k++)
                    ;
                if (valid(c) && k == ndq && ndq < DMAX)
                    dq[ndq++] = c;
            }
        }
        orphan_pending();
        if (fix)                        /* erst nach dem Durchlauf        */
            for (k = 0; k < nfix; k++)
                if (load_dsec(fix_lba[k]) == 0) {
                    dsec[fix_off[k]] = 0xE5;
                    wr_sec(fix_lba[k], dsec);
                }
    }
    check_ea();
    if (lfn_orphans) {
        problem();
        put_num(lfn_orphans, 0);
        puts(lfn_orphans == 1 ? " verwaister langer Name (VFAT)"
                              : " verwaiste lange Namen (VFAT)");
        puts(fix ? " entfernt.\n" : " gefunden.\n");
    }
    if (ea_orphans) {
        problem();
        put_num(ea_orphans, 0);
        puts(ea_orphans == 1 ? " verwaister langer Name" : " verwaiste lange Namen");
        puts(fix ? " in EA DATA. SF entfernt.\n" : " in EA DATA. SF gefunden.\n");
    }
}
