/*
 * UFAT.EXE /UNDELETE - geloeschte Dateien wiederherstellen (eingebunden in
 * task_ufat.c), aufgerufen von UNDELETE.EXE:
 *
 *   UFAT X: /UNDELETE \Pfad\Muster [/LIST] [/ALL]
 *
 * Wie UNDELETE von MS-DOS 5 ohne Loeschschutz: FAT loescht beim Entfernen
 * nur das erste Zeichen des Namens (E5h) und gibt die Kette frei. Ist der
 * Startcluster noch frei, holt UFAT die Datei zurueck: Startcluster plus
 * so viele der naechsten freien Cluster, wie die Groesse verlangt
 * (stimmt, solange die Datei zusammenhaengend lag und nichts darueber
 * geschrieben wurde). Das erste Zeichen fragt UFAT ab, /ALL setzt '#'
 * (bei Namensgleichheit das naechste aus "#%&-0123456789"). /LIST zeigt
 * nur an ("**" = nicht wiederherstellbar). Verzeichnisse bleiben aussen
 * vor.
 */

#define UMAX 128                        /* Kandidaten je Verzeichnis     */

static struct cand {
    unsigned long lba;                  /* Sektor des Eintrags           */
    unsigned off;
    unsigned char e[32];                /* Eintrag, wie gefunden         */
} cands[UMAX];
static unsigned ncand;
static unsigned long root_n;            /* Sektoren des Stammverzeichnisses */

/* ---- Verzeichnis lesen ------------------------------------------------ */
static unsigned u_first, u_clus, u_left, u_off;
static unsigned long u_lba, u_cur;

static void u_open(unsigned first)
{
    u_first = first;
    u_clus = first;
    u_lba = first ? clus_lba(first) : root_lba;
    u_left = first ? spc : (unsigned)root_n;
    u_off = 512;
}

/* naechster Eintrag (in dsec, Sektor u_cur), 0 = Ende */
static unsigned char *u_next(void)
{
    unsigned char *e;
    unsigned c;
    if (u_off >= 512) {
        if (!u_left) {
            if (!u_first)
                return 0;
            c = fat_get(u_clus);
            if (!valid(c))
                return 0;
            u_clus = c;
            u_lba = clus_lba(c);
            u_left = spc;
        }
        if (load_dsec(u_lba))
            return 0;
        u_cur = u_lba++;
        u_left--;
        u_off = 0;
    }
    e = dsec + u_off;
    u_off += 32;
    return e[0] ? e : 0;
}

/* Name oder Muster in 11 Zeichen ("TEST    TXT", '*' -> '?') */
static void to11(const char *s, char *o)
{
    unsigned i = 0;
    for (; i < 11; i++)
        o[i] = ' ';
    for (i = 0; *s && *s != '.' && i < 8; s++)
        if (*s == '*')
            while (i < 8)
                o[i++] = '?';
        else
            o[i++] = up(*s);
    while (*s && *s != '.')
        s++;
    if (*s == '.')
        s++;
    for (i = 8; *s && i < 11; s++)
        if (*s == '*')
            while (i < 11)
                o[i++] = '?';
        else
            o[i++] = up(*s);
}

/* erstes Zeichen zaehlt nicht (ist E5h) */
static int match11(const unsigned char *n, const char *p)
{
    unsigned i;
    for (i = 1; i < 11; i++)
        if (p[i] != '?' && (unsigned char)p[i] != n[i])
            return 0;
    return 1;
}

/* Verzeichnis zum Pfad suchen -> Startcluster (0 = Stamm), -1 = fehlt */
static long find_dir(const char *path)
{
    static char comp[16], n11[11];
    unsigned first = 0, k;
    unsigned char *e;

    while (*path) {
        while (*path == '\\')
            path++;
        for (k = 0; *path && *path != '\\' && k < sizeof(comp) - 1; )
            comp[k++] = *path++;
        comp[k] = 0;
        if (!k)
            break;
        to11(comp, n11);
        u_open(first);
        for (;;) {
            if (!(e = u_next()))
                return -1;
            if (e[0] != 0xE5 && (e[11] & 0x10) && e[11] != 0x0F) {
                for (k = 0; k < 11 && e[k] == (unsigned char)n11[k]; k++)
                    ;
                if (k == 11)
                    break;
            }
        }
        first = rd16(e + 26);
    }
    return first;
}

/* Name schon vorhanden (nicht geloescht)? */
static int name_used(unsigned dir, const unsigned char *n)
{
    unsigned char *e;
    unsigned k;
    u_open(dir);
    while ((e = u_next()) != 0) {
        if (e[0] == 0xE5 || e[11] == 0x0F)
            continue;
        for (k = 0; k < 11 && e[k] == n[k]; k++)
            ;
        if (k == 11)
            return 1;
    }
    return 0;
}

/* Kette pruefen (und mit build = 1 eintragen); -> 1 wiederherstellbar */
static int chain(const unsigned char *e, int build)
{
    unsigned long size = rd32(e + 28), bytes;
    unsigned c = rd16(e + 26), prev = 0, need, n;

    if (size == 0)
        return 1;                       /* leer: nur der Eintrag         */
    if (!valid(c) || fat_get(c) != 0)
        return 0;
    bytes = (unsigned long)512 << shift;
    for (need = 0; size > 0; need++)    /* ohne 32-Bit-Division           */
        size = size > bytes ? size - bytes : 0;
    for (n = 0; n < need && c <= maxc; c++) {
        if (fat_get(c) != 0)
            continue;                   /* belegt: ueberspringen         */
        if (build) {
            if (prev)
                fat_put(prev, c);
            fat_put(c, EOC);
        }
        prev = c;
        n++;
    }
    return n == need;
}

static void put2(unsigned v)
{
    putc((char)('0' + v / 10 % 10));
    putc((char)('0' + v % 10));
}

static void show(const unsigned char *e, int ok)
{
    unsigned d = rd16(e + 24), t = rd16(e + 22), i;
    puts(ok ? "   " : "** ");
    putc('?');
    for (i = 1; i < 8; i++)
        putc((char)e[i]);
    putc(' ');
    for (i = 8; i < 11; i++)
        putc((char)e[i]);
    put_num(rd32(e + 28), 10);
    puts("  ");
    put2(d & 31);
    putc('.');
    put2((d >> 5) & 15);
    putc('.');
    put2(((d >> 9) + 80) % 100);
    puts("  ");
    put2(t >> 11);
    putc(':');
    put2((t >> 5) & 63);
    puts("  ");
    putc((e[11] & 0x01) ? 'R' : '.');
    putc((e[11] & 0x02) ? 'H' : '.');
    putc((e[11] & 0x04) ? 'S' : '.');
    putc((e[11] & 0x20) ? 'A' : '.');
    puts("\n");
}

static int do_undelete(const char *spec, int list, int all)
{
    static char dir[200], pat[11];
    static const char alts[] = "#%&-0123456789";
    unsigned char *e;
    unsigned i, k, ok, done = 0, nok = 0;
    long first;
    char *p;
    unsigned c;

    root_n = (((unsigned long)rootents << 5) + 511) >> 9;
    for (i = 0; spec[i] && i < sizeof(dir) - 1; i++)
        dir[i] = spec[i];
    dir[i] = 0;
    p = dir;                            /* letzter Teil = Muster          */
    for (i = 0; dir[i]; i++)
        if (dir[i] == '\\')
            p = dir + i;
    to11(p[0] == '\\' ? p + 1 : p, pat);
    *p = 0;
    first = find_dir(dir);
    if (first < 0) {
        puts("Verzeichnis nicht gefunden.\n");
        return 3;
    }
    puts("Verzeichnis: ");
    putc((char)('A' + drive));
    puts(":");
    puts(dir[0] ? dir : "\\");
    puts("\nDateiangaben: ");
    puts(p[0] ? p + 1 : "*.*");
    puts("\n\n");

    ncand = 0;
    u_open((unsigned)first);
    while ((e = u_next()) != 0 && ncand < UMAX)
        if (e[0] == 0xE5 && e[11] != 0x0F && !(e[11] & 0x18) && match11(e, pat)) {
            cands[ncand].lba = u_cur;
            cands[ncand].off = u_off - 32;
            for (k = 0; k < 32; k++)
                cands[ncand].e[k] = e[k];
            ncand++;
        }
    if (!ncand) {
        puts("Keine gel\x94schten Dateien gefunden.\n");
        return 0;
    }
    for (i = 0; i < ncand; i++)
        if (chain(cands[i].e, 0))
            nok++;
    put_num(ncand, 0);
    puts(" gel\x94schte Datei(en) gefunden, davon ");
    put_num(nok, 0);
    puts(" wiederherstellbar.\n\n");
    if (list) {
        for (i = 0; i < ncand; i++)
            show(cands[i].e, chain(cands[i].e, 0));
        return 0;
    }
    for (i = 0; i < ncand; i++) {
        e = cands[i].e;
        ok = chain(e, 0);               /* frueher Wiederhergestelltes   */
        show(e, ok);                    /* kann Cluster belegt haben     */
        if (!ok) {
            puts("   Kann nicht wiederhergestellt werden (Cluster belegt).\n\n");
            continue;
        }
        if (!all) {
            puts("   Wiederherstellen (J/N)? ");
            if (!yes()) {
                puts("\n");
                continue;
            }
        }
        for (k = 0;; k++) {             /* erstes Zeichen                 */
            if (all) {
                if (!alts[k])
                    break;
                c = (unsigned char)alts[k];
            } else {
                puts("   Erstes Zeichen f\x81r ?");
                for (c = 1; c < 8 && e[c] != ' '; c++)
                    putc((char)e[c]);
                puts(": ");
                do
                    c = (unsigned char)up((char)KbdCharIn());
                while (c <= ' ' || c == '.' || c == '\\' || c == '?' || c == '*');
                putc((char)c);
                puts("\n");
            }
            e[0] = (unsigned char)c;
            if (!name_used((unsigned)first, e))
                break;
            puts("   Eine Datei mit diesem Namen gibt es schon.\n");
            e[0] = 0xE5;
        }
        if (e[0] == 0xE5)
            continue;
        chain(e, 1);
        if (load_dsec(cands[i].lba)) {
            puts("   Fehler beim Lesen des Verzeichnisses.\n");
            continue;
        }
        dsec[cands[i].off] = e[0];
        if (wr_sec(cands[i].lba, dsec)) {
            puts("   Fehler beim Schreiben des Verzeichnisses.\n");
            continue;
        }
        write_fat();
        for (k = 0; k < 32; k++)
            fdirty[k] = 0;
        puts("   Datei wiederhergestellt.\n\n");
        done++;
    }
    put_num(done, 0);
    puts(" Datei(en) wiederhergestellt.\n");
    return 0;
}
