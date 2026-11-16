/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UFAT32.EXE /TO16 - FAT32 an Ort und Stelle in FAT16 umwandeln
 * (eingebunden in task_ufat32.c nach ufat32_conv.c), aufgerufen von
 * CONVERT.EXE:  UFAT32 X: /TO16 [/V]
 *
 * FAT16 hat hoechstens 65.524 Cluster, die Cluster werden also groesser:
 * je k = 2^kj alte Cluster ("Gruppe", ab Cluster 2 abgezaehlt) werden ein
 * neuer an derselben Stelle. Der neue Datenbereich beginnt m Gruppen vor
 * dem alten (im bisherigen FAT32-FAT-Bereich), davor FATs, Stamm (512
 * Eintraege) und reservierte Sektoren - alter Cluster p ist neuer Cluster
 * 2 + m + (p - 2) / k.
 *
 *  A (sicher, das Laufwerk bleibt ein gueltiges FAT32): jede Datei und
 *     jedes Verzeichnis muss aus ganzen Gruppen bestehen (in jeder Gruppe
 *     hintereinander, Gruppe gehoert nur ihr). Was nicht passt, wird wie
 *     bei DEFRAG in freie Gruppen kopiert: Daten -> neue Kette -> Eintrag
 *     -> bei Verzeichnissen ".", ".." der Kinder, Lage der Kindereintraege,
 *     EA DATA. SF -> alte Kette frei. Durchgaenge, bis alles passt.
 *  B (nicht unterbrechen): neue FAT16 im Speicher aus den Ketten, Stamm
 *     (Kette) einlesen, alle Verzeichniseintraege und EA-Saetze umrechnen
 *     (Hochwort 0), Rest der letzten Gruppe eines Verzeichnisses nullen,
 *     dann FATs, Stamm, Bootsektor, zuletzt Partitionstyp 0Bh -> 06h,
 *     0Ch -> 0Eh.
 */

#define TR_NSEG 6                       /* Segmente fuer Saetze           */
#define TR_PS   3640                    /* Saetze je Segment (18 Byte)    */
#define TR_DIR  1                       /* flags                          */
#define TR_MOVE 2
#define T_ROOTP 0xFFFFu                 /* parent: Stamm                  */
#define O_ROOT  0xFFFEu                 /* owner: Stamm, Fehler, Rest      */
#define O_BAD   0xFFFFu
#define T_NDG   256                     /* Gruppen eines Verzeichnisses   */

struct trec {
    unsigned long lba, first, n;        /* Eintrag, Kette, Laenge         */
    unsigned off, flags, parent;
};
static unsigned tsegs[TR_NSEG], ntseg, osegs[2];
static unsigned ntr, t_ea = 0xFFFF, kj, t_k, t_K, t_spf;
static unsigned long t_m, t_ng, t_nd, t_res, t_c16, t_fg, t_moved, t_rootn;
static unsigned t_dg[T_NDG];

static struct trec __far *tr(unsigned i)
{
    return (struct trec __far *)MK_FP(tsegs[i / TR_PS], (i % TR_PS) * sizeof(struct trec));
}
static unsigned __far *own(unsigned long g)
{
    return (unsigned __far *)MK_FP(osegs[(unsigned)(g >> 15)], ((unsigned)g & 0x7FFF) * 2);
}
static unsigned long grp(unsigned long c) { return shr(c - 2, kj); }
static unsigned long gbase(unsigned long g) { return 2 + shl(g, kj); }
static unsigned long nmap16(unsigned long c) { return 2 + t_m + grp(c); }

static unsigned long chain_len(unsigned long c)
{
    unsigned long n = 0;
    while (valid(c) && n <= clusters) {
        n++;
        c = fat_get(c);
    }
    return n;
}

/* Saetze sammeln: Stamm, dann jedes Verzeichnis (Breite zuerst) */
static int t_add(unsigned char *e, unsigned par)
{
    struct trec __far *r;
    unsigned seg;
    if (ntr >= TR_NSEG * TR_PS)
        return -1;
    seg = ntr / TR_PS;
    if (seg >= ntseg) {
        if (!(tsegs[seg] = seg64()))
            return -1;
        ntseg++;
    }
    r = tr(ntr++);
    r->lba = u_cur;
    r->off = (unsigned)(e - dsec);
    r->first = ent_first(e);
    r->n = chain_len(r->first);
    r->flags = (e[11] & 0x10) ? TR_DIR : 0;
    r->parent = par;
    if (par == T_ROOTP && !(e[11] & 0x10)) {
        unsigned i;
        for (i = 0; i < 11 && e[i] == (unsigned char)"EA DATA SF "[i]; i++)
            ;
        if (i == 11)
            t_ea = ntr - 1;
    }
    return 0;
}

static int t_collect(void)
{
    unsigned char *e;
    unsigned i;
    ntr = 0;
    t_ea = 0xFFFF;
    t_rootn = 0;
    u_open(0);
    while ((e = u_next()) != 0) {
        if (e[0] != 0xE5)
            t_rootn++;
        if (e[0] == 0xE5 || e[0] == '.' || e[11] == 0x0F || (e[11] & 0x08))
            continue;
        if (valid(ent_first(e)) && t_add(e, T_ROOTP))
            return -1;
    }
    for (i = 0; i < ntr; i++) {
        unsigned long f = tr(i)->first;
        if (!(tr(i)->flags & TR_DIR))
            continue;
        u_open(f);
        while ((e = u_next()) != 0) {
            if (e[0] == 0xE5 || e[0] == '.' || e[11] == 0x0F || (e[11] & 0x08))
                continue;
            if (valid(ent_first(e)) && t_add(e, i))
                return -1;
        }
    }
    return 0;
}

static void claim(unsigned long c, unsigned id)
{
    unsigned __far *o;
    unsigned long g = grp(c);
    if (g >= t_ng)
        return;
    o = own(g);
    if (*o == 0)
        *o = id;
    else if (*o != id)
        *o = O_BAD;
}

/* Besitzer der Gruppen neu bestimmen, Saetze mit TR_MOVE markieren;
   -> Zahl der zu verlegenden */
static unsigned long t_owners(void)
{
    unsigned long g, c, i, n, cnt = 0, prev;
    unsigned id;
    struct trec __far *r;
    for (g = 0; g < t_ng; g++)
        *own(g) = 0;
    for (c = 2; c <= maxc; c++)         /* fehlerhafte Cluster           */
        if (fat_get(c) == BAD)
            claim(c, O_BAD);
    for (c = rootc, n = 0; valid(c) && n <= clusters; n++, c = fat_get(c))
        claim(c, O_ROOT);
    for (id = 0; id < ntr; id++) {
        r = tr(id);
        for (c = r->first, n = 0; valid(c) && n < r->n; n++, c = fat_get(c))
            claim(c, id + 1);
    }
    for (id = 0; id < ntr; id++) {
        int ok = 1;
        r = tr(id);
        for (c = r->first, i = 0, prev = 0; valid(c) && i < r->n; i++, c = fat_get(c)) {
            unsigned long g2 = grp(c);
            if (g2 >= t_ng || *own(g2) != id + 1 ||
                ((c - 2) & (t_k - 1)) != (i & (t_k - 1)) ||
                ((i & (t_k - 1)) && c != prev + 1)) {
                ok = 0;
                break;
            }
            prev = c;
        }
        r->flags = (r->flags & TR_DIR) | (ok ? 0 : TR_MOVE);
        if (!ok)
            cnt++;
    }
    return cnt;
}

static unsigned long free_group(void)   /* -> Gruppe oder 0xFFFFFFFF     */
{
    unsigned long g;
    for (g = t_fg; g < t_ng; g++)
        if (*own(g) == 0) {
            t_fg = g + 1;
            return g;
        }
    return 0xFFFFFFFFUL;
}

/* EA DATA. SF: Verzeichnis-Cluster oc -> nc in allen Saetzen */
static int ea_fix(unsigned long oc, unsigned long nc)
{
    unsigned long c, g = 0, nx = 0, size, steps = 0;
    unsigned s, o, dirty, end = 0;
    unsigned char *e;
    if (t_ea == 0xFFFF)
        return 0;
    if (load_dsec(tr(t_ea)->lba))
        return -1;
    e = dsec + tr(t_ea)->off;
    size = rd32(e + 28);
    for (c = ent_first(e); valid(c) && g < size && !end && steps++ <= clusters; c = fat_get(c))
        for (s = 0; s < spc && g < size && !end; s++) {
            if (load_dsec(clus_lba(c) + s))
                return -1;
            dirty = 0;
            for (o = 0; o < 512 && !end; o += 32, g += 32) {
                if (g != nx || g >= size)
                    continue;
                if (!dsec[o + 1]) {
                    end = 1;
                    break;
                }
                nx = g + ((unsigned long)dsec[o + 1] << 5);
                if ((dsec[o] == 'L' || dsec[o] == 'W') && rd32(dsec + o + 2) == oc) {
                    wr32(dsec + o + 2, nc);
                    dirty = 1;
                }
            }
            if (dirty && wr_sec(clus_lba(c) + s, dsec))
                return -1;
        }
    return 0;
}

/* Satz id in freie Gruppen legen; 0 = ok, 1 = kein Platz, -1 Fehler */
static int t_move(unsigned id)
{
    struct trec __far *r = tr(id);
    unsigned long c, i, g, nc = 0, last = 0, of = r->first, nf = 0;
    unsigned ng = 0, isdir = (r->flags & TR_DIR) != 0;
    unsigned long save_fg = t_fg;

    if (isdir && shr(r->n + t_k - 1, kj) > T_NDG)
        return 1;
    for (c = of, i = 0; i < r->n; i++, c = fat_get(c)) {    /* 1. Daten   */
        if ((i & (t_k - 1)) == 0) {
            g = free_group();
            if (g == 0xFFFFFFFFUL)
                goto nospace;
            *own(g) = id + 1;
            nc = gbase(g);
            if (isdir)
                t_dg[ng++] = (unsigned)g;
        } else
            nc++;
        if (!valid(c) || copy_clus(c, nc))
            return -1;
        if (!nf)
            nf = nc;
        if (last)
            fat_put(last, nc);              /* 2. neue Kette (noch frei)  */
        last = nc;
    }
    fat_put(last, EOC);
    fat_flush();
    if (ferr || load_dsec(r->lba))                          /* 3. Eintrag */
        return -1;
    set_first(dsec + r->off, nf);
    if (wr_sec(r->lba, dsec))
        return -1;
    if (isdir) {                        /* ".", ".." der Kinder, Lagen    */
        unsigned j;
        if (load_dsec(clus_lba(nf)))
            return -1;
        set_first(dsec, nf);
        if (wr_sec(clus_lba(nf), dsec))
            return -1;
        for (j = 0; j < ntr; j++) {
            struct trec __far *q = tr(j);
            unsigned long oc, s, x;
            if (q->parent != id)
                continue;
            oc = shr(q->lba - data_lba, shift) + 2;
            s = (q->lba - data_lba) & (spc - 1);
            for (x = 0, c = of; valid(c) && c != oc && x < r->n; x++)
                c = fat_get(c);
            if (c != oc)
                return -1;
            q->lba = clus_lba(gbase(t_dg[(unsigned)shr(x, kj)]) + (x & (t_k - 1))) + s;
            if ((q->flags & TR_DIR) && valid(q->first)) {
                if (load_dsec(clus_lba(q->first)))
                    return -1;
                set_first(dsec + 32, nf);
                if (wr_sec(clus_lba(q->first), dsec))
                    return -1;
            }
        }
        if (ea_fix(of, nf))
            return -1;
    }
    for (c = of, i = 0; i < r->n && valid(c); i++) {        /* 4. alt frei */
        unsigned long nx = fat_get(c);
        fat_put(c, 0);
        c = nx;
    }
    fat_flush();
    r->first = nf;
    t_moved++;
    return ferr ? -1 : 0;
nospace:
    for (c = nf, i = 0; valid(c) && i < r->n; i++) {        /* Teilkette zurueck */
        unsigned long nx = fat_get(c);
        if (c == last) {
            fat_put(c, 0);
            break;
        }
        fat_put(c, 0);
        c = nx;
    }
    fat_flush();
    t_fg = save_fg;
    return 1;
}

/* ---- B --------------------------------------------------------------- */
static unsigned __far *f16w(unsigned long n)    /* neue FAT16 (osegs)    */
{
    return (unsigned __far *)MK_FP(osegs[(unsigned)(n >> 15)], ((unsigned)n & 0x7FFF) * 2);
}

static void remap_ent(unsigned char *e, unsigned long self, unsigned long parent)
{
    unsigned long c;
    if (e[0] == 0 || e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08))
        return;
    if (e[0] == '.' && e[1] == ' ')
        c = self;
    else if (e[0] == '.' && e[1] == '.' && e[2] == ' ')
        c = parent;
    else
        c = ent_first(e);
    c = valid(c) ? nmap16(c) : 0;
    wr16(e + 20, 0);
    wr16(e + 26, (unsigned)c);
}

static unsigned t_rseg;                /* Stamm, 16 KB, eigenes Segment  */
#define TROOT ((unsigned char __far *)MK_FP(t_rseg, 0))

static int do_to16(void)
{
    static struct fapi_fsinfo fi;
    unsigned long need, c, n, i, g, lfree = 0, sp, nmove;
    unsigned id, pass, ours, s;
    unsigned char *b = sec, *e;
    int r;

    if (DosQFSInfo(drive, &fi) || (fi.flags & (FSI_READONLY | FSI_REMOVABLE)) ||
        (fi.ptype != 0x0B && fi.ptype != 0x0C)) {
        puts("Umwandeln geht nur f\x81r FAT32-Partitionen (Typ 0Bh/0Ch) auf Festplatten.\n");
        return 3;
    }
    for (kj = 1; kj < 7 && (spc << kj) <= 64; kj++) {      /* Clustergroesse */
        t_k = 1U << kj;
        t_K = spc << kj;
        t_spf = (unsigned)udiv(2 * (shr(total, shift + kj) + 2) + 511, 512);
        t_m = shr(data_lba - 1 - t_spf - t_spf - 32, shift + kj);
        t_nd = data_lba - shl(t_m, shift + kj);
        t_c16 = shr(total - t_nd, shift + kj);
        if (t_c16 <= 65524UL)
            break;
    }
    if (kj >= 7 || (spc << kj) > 64 || t_c16 > 65524UL || t_c16 < 4085) {
        puts("Das Laufwerk ist f\x81r FAT16 zu gro\xE1 (h\x94" "chstens etwa 2 GB).\n");
        return 3;
    }
    t_spf = (unsigned)udiv(2 * (t_c16 + 2) + 511, 512);
    t_res = t_nd - t_spf - t_spf - 32;
    t_ng = t_c16 - t_m;                 /* Gruppen im alten Datenbereich */
    if (!(osegs[0] = seg64()) || !(osegs[1] = seg64()) || !(t_rseg = seg64())) {
        puts("Zu wenig Arbeitsspeicher.\n");
        return 3;
    }
    puts("Lese das Verzeichnis ...\n");
    if (t_collect()) {
        puts("Zu viele Dateien f\x81r die Umwandlung.\n");
        return 3;
    }
    if (t_rootn > 512) {
        puts("Das Stammverzeichnis hat mehr als 512 Eintr\x84ge (lange Namen z\x84hlen\n"
             "mit) - erst Dateien in Unterverzeichnisse verschieben.\n");
        return 3;
    }
    for (id = 0, need = 0; id < ntr; id++)
        need += shr(tr(id)->n + t_k - 1, kj);
    nmove = t_owners();
    for (g = 0; g < t_ng; g++)
        if (*own(g) == 0)
            lfree++;
    puts("Laufwerk ");
    putc((char)('A' + drive));
    puts(": FAT32 -> FAT16, Zuordnungseinheit ");
    put_num(spc / 2, 0);
    puts(spc == 1 ? ",5" : "");
    puts(" KB -> ");
    put_num(t_K / 2, 0);
    puts(" KB, ");
    put_num(t_c16, 0);
    puts(" Zuordnungseinheiten.\n");
    put_num(ntr, 0);
    puts(" Dateien und Verzeichnisse, davon ");
    put_num(nmove, 0);
    puts(" neu zu ordnen.\n");
    if (verbose) {
        puts("Reserviert ");
        put_num(t_res, 0);
        puts(", FAT ");
        put_num(t_spf, 0);
        puts(" Sektoren, m ");
        put_num(t_m, 0);
        puts(", freie Gruppen ");
        put_num(lfree, 0);
        puts(".\n");
    }
    if (need > t_c16 - 2) {
        puts("Mit ");
        put_num(t_K / 2, 0);
        puts("-KB-Zuordnungseinheiten passen die Daten nicht (");
        put_num(shl(need, kj + shift) >> 11, 0);
        puts(" MB n\x94tig) - nichts ge\x84ndert.\n");
        return 3;
    }
    if (!autom) {
        puts("Erst werden Dateien neu geordnet (unterbrechbar), dann wird umgeschrieben\n"
             "(dabei darf der Rechner nicht ausgeschaltet werden). Umwandeln (J/N)? ");
        if (!yes())
            return 3;
    }

    /* A: neu ordnen, bis alles in ganzen Gruppen liegt */
    for (pass = 0; nmove && pass < 8; pass++) {
        unsigned long before = t_moved;
        t_fg = 0;
        for (id = 0; id < ntr; id++) {
            if (!(tr(id)->flags & TR_MOVE))
                continue;
            r = t_move(id);
            if (r < 0) {
                puts("\nLese-/Schreibfehler beim Ordnen - Laufwerk bleibt FAT32 (gepr\x81" "ft\n"
                     "mit CHKDSK).\n");
                return 3;
            }
            if ((t_moved & 31) == 0) {
                putc('\r');
                put_num(t_moved, 0);
                puts(" verlegt ");
            }
        }
        nmove = t_owners();
        if (t_moved == before)
            break;
    }
    putc('\r');
    put_num(t_moved, 0);
    puts(" verlegt.\n");
    if (nmove) {
        puts("Zu wenig freier Platz zum Ordnen (");
        put_num(nmove, 0);
        puts(" Eintr\x84ge) - Laufwerk bleibt FAT32.\n"
             "Platz schaffen oder DEFRAG, dann erneut.\n");
        return 3;
    }

    /* B: ab hier nicht unterbrechen */
    puts("Wandle um ...\n");
    for (n = 0; n < 65536UL; n++)       /* neue FAT16 in osegs           */
        *f16w(n) = 0;
    *f16w(0) = 0xFFF8;
    *f16w(1) = 0xFFFF;
    for (c = 2; c <= maxc; c++)
        if (fat_get(c) == BAD && grp(c) < t_ng && *f16w(nmap16(c)) == 0)
            *f16w(nmap16(c)) = 0xFFF7;
    for (id = 0; id < ntr; id++) {
        struct trec __far *q = tr(id);
        unsigned long prevn = 0;
        for (c = q->first, i = 0; valid(c) && i < q->n; i++, c = fat_get(c))
            if ((i & (t_k - 1)) == 0) {
                if (prevn)
                    *f16w(prevn) = (unsigned)nmap16(c);
                prevn = nmap16(c);
            }
        if (prevn)
            *f16w(prevn) = 0xFFFF;
    }
    /* Stamm einlesen und umrechnen */
    for (i = 0; i < 16384; i++)
        TROOT[(unsigned)i] = 0;
    u_open(0);
    for (sp = 0; (e = u_next()) != 0 && sp < 16384; ) {
        if (e[0] == 0xE5)
            continue;
        for (i = 0; i < 32; i++)
            sec[(unsigned)i] = e[(unsigned)i];
        remap_ent(sec, 0, 0);
        for (i = 0; i < 32; i++)
            TROOT[(unsigned)(sp + i)] = sec[(unsigned)i];
        sp += 32;
    }
    /* Verzeichnisse an Ort und Stelle umrechnen, Rest der Gruppe nullen */
    for (id = 0; id < ntr; id++) {
        struct trec __far *q = tr(id);
        unsigned long par = q->parent == T_ROOTP ? 0 : tr(q->parent)->first, lastc = 0;
        if (!(q->flags & TR_DIR))
            continue;
        for (c = q->first, i = 0; valid(c) && i < q->n; i++, c = fat_get(c)) {
            for (s = 0; s < spc; s++) {
                unsigned o;
                if (load_dsec(clus_lba(c) + s))
                    goto werr;
                for (o = 0; o < 512; o += 32)
                    remap_ent(dsec + o, q->first, par);
                if (wr_sec(clus_lba(c) + s, dsec))
                    goto werr;
            }
            lastc = c;
        }
        for (i = 0; i < 512; i++)
            sec[i] = 0;
        for (c = lastc + 1; c < gbase(grp(lastc)) + t_k; c++)
            for (s = 0; s < spc; s++)
                if (wr_sec(clus_lba(c) + s, sec))
                    goto werr;
    }
    /* EA DATA. SF: Verzeichnis-Cluster umrechnen */
    if (t_ea != 0xFFFF) {
        unsigned long gg = 0, nx = 0, size;
        unsigned o, end = 0;
        int dirty;
        if (load_dsec(tr(t_ea)->lba))
            goto werr;
        size = rd32(dsec + tr(t_ea)->off + 28);
        for (c = tr(t_ea)->first, i = 0; valid(c) && gg < size && !end && i <= clusters; c = fat_get(c), i++)
            for (s = 0; s < spc && gg < size && !end; s++) {
                if (load_dsec(clus_lba(c) + s))
                    goto werr;
                dirty = 0;
                for (o = 0; o < 512 && !end; o += 32, gg += 32) {
                    if (gg != nx || gg >= size)
                        continue;
                    if (!dsec[o + 1]) {
                        end = 1;
                        break;
                    }
                    nx = gg + ((unsigned long)dsec[o + 1] << 5);
                    if ((dsec[o] == 'L' || dsec[o] == 'W') && valid(rd32(dsec + o + 2))) {
                        wr32(dsec + o + 2, nmap16(rd32(dsec + o + 2)));
                        dirty = 1;
                    }
                }
                if (dirty && wr_sec(clus_lba(c) + s, dsec))
                    goto werr;
            }
    }
    /* FATs, Stamm, alte FSInfo/Kopie weg, Bootsektor zuletzt */
    for (i = 0; i < t_spf; i++) {
        unsigned w;
        for (w = 0; w < 256; w++)
            wr16(sec + 2 * w, *f16w(shl(i, 8) + w));
        if (wr_sec(t_res + i, sec) || wr_sec(t_res + t_spf + i, sec))
            goto werr;
    }
    for (i = 0; i < 32; i++) {
        unsigned w;
        for (w = 0; w < 512; w++)
            sec[w] = TROOT[(unsigned)i * 512 + w];
        if (wr_sec(t_res + t_spf + t_spf + i, sec))
            goto werr;
    }
    for (i = 0; i < 512; i++)
        sec[i] = 0;
    for (i = 1; i < 16 && i < t_res; i++)
        if (wr_sec(i, sec))
            goto werr;
    if (rd_secs(0, ob, 1))
        goto werr;
    for (i = 0x5A, ours = 1; i < 0x80; i++)
        if (ob[i] != boot_32[i])
            ours = 0;
    for (i = 0; i < 512; i++)
        b[i] = i < 36 ? ob[i] : 0;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    for (i = 0; i < 8; i++)
        b[3 + i] = (unsigned char)"ALTDOS60"[i];
    b[13] = (unsigned char)t_K;
    wr16(b + 14, (unsigned)t_res);
    b[16] = 2;
    wr16(b + 17, 512);
    if (total < 65536UL) {
        wr16(b + 19, (unsigned)total);
        wr32(b + 32, 0);
    } else {
        wr16(b + 19, 0);
        wr32(b + 32, total);
    }
    b[21] = 0xF8;
    wr16(b + 22, t_spf);
    b[36] = 0x80;
    b[38] = 0x29;
    for (i = 0; i < 4; i++)
        b[39 + i] = ob[66] == 0x29 ? ob[67 + i] : (unsigned char)(total >> (i * 8)) ^ 0x5A;
    for (i = 0; i < 11; i++)
        b[43 + i] = ob[66] == 0x29 ? ob[71 + i] : (unsigned char)"NO NAME    "[i];
    for (i = 0; i < 8; i++)
        b[54 + i] = (unsigned char)"FAT16   "[i];
    if (ours) {
        for (i = 0; i < 3; i++)
            b[i] = boot_hd[i];
        for (i = 0x3E; i < 510; i++)
            b[i] = boot_hd[i];
    } else {
        b[1] = 0x58;
        for (i = 0; i < sizeof(nosys); i++)
            b[90 + i] = nosys[i];
        for (i = 0; nosys_msg[i]; i++)
            b[0x80 + i] = (unsigned char)nosys_msg[i];
    }
    b[510] = 0x55; b[511] = 0xAA;
    if (wr_sec(0, b))
        goto werr;
    for (i = 0; i < 512; i++)           /* zum Wiederfinden (set_ptype) */
        ob[i] = b[i];
    DosClose(hd);
    if (set_ptype2(fi.hidden, fi.ptype, fi.ptype == 0x0C ? 0x0E : 0x06))
        puts("Hinweis: Partitionstyp konnte nicht ge\x84ndert werden (AT-DOS liest das\n"
             "Laufwerk trotzdem, MS-DOS erst mit Typ 06h/0Eh).\n");
    puts("Umwandlung beendet: FAT16, ");
    for (n = 2, i = 0; n < t_c16 + 2; n++)
        if (*f16w(n) == 0)
            i++;
    put_num(i, 0);
    puts(" Zuordnungseinheiten frei.\n");
    if (ours)
        puts("Startcode f\x81r FAT16 geschrieben.\n");
    return 0;
werr:
    puts("Schreibfehler - das Laufwerk ist besch\x84" "digt.\n");
    return 3;
}
