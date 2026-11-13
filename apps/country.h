/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * country.h - Laenderformate fuer AT-DOS-Programme (per #include)
 *
 * COUNTRY=nnn in der CONFIG.SYS legt der Kernel als Variable COUNTRY=nnn in
 * die Systemumgebung (wie LONGNAMES=); jede Sitzung erbt sie und kann sie
 * mit SET COUNTRY=... fuer sich aendern. Werte wie bei MS-DOS 5/6
 * (COUNTRY.SYS): Datumsfolge und -trenner, Zeittrenner, 12/24 Stunden,
 * Tausender- und Dezimaltrenner, Waehrung. Ohne Angabe 049 (Deutschland).
 *
 * ctry_init()          einmal am Anfang
 * ctry_date(o, d)      FAT-Datum -> "TT.MM.JJJJ" o. ae. (10 Zeichen + 0)
 * ctry_time(o, t, s)   FAT-Zeit -> "hh:mm[:ss]" bzw. 12 Stunden "hh:mma"
 * ctry.th, ctry.dec    Tausender-/Dezimaltrenner
 * Mit CTRY_MIN nur ctry_init und die Werte (ohne die Ausgabehilfen).
 */

struct ctry {
    unsigned       code;
    unsigned char  dfmt;                /* 0 MTJ, 1 TMJ, 2 JMT            */
    char           dsep, tsep, th, dec;
    unsigned char  h12;                 /* 1 = 12 Stunden (a/p)           */
    unsigned char  cfmt;                /* Bit 0 Waehrung hinten, 1 Leerz. */
    unsigned       cp;                  /* uebliche Codepage              */
    char           cur[5];
};

static const struct ctry ctry_tab[] = {
    {   1, 0, '/', ':', ',', '.', 1, 0, 437, "$"   },  /* USA          */
    {   2, 2, '-', ':', ' ', ',', 0, 3, 863, "$"   },  /* Kanada (frz.) */
    {   3, 1, '/', ':', ',', '.', 1, 0, 850, "$"   },  /* Lateinamerika */
    {   7, 1, '.', ':', ' ', ',', 0, 3, 866, "\xE0." }, /* Russland (CP866 "р") */
    {  30, 1, '/', ':', '.', ',', 0, 3, 737, "Dpx" },  /* Griechenland  */
    {  31, 1, '-', ':', '.', ',', 0, 2, 850, "fl"  },  /* Niederlande   */
    {  32, 1, '/', ':', '.', ',', 0, 3, 850, "BF"  },  /* Belgien       */
    {  33, 1, '.', ':', ' ', ',', 0, 3, 850, "F"   },  /* Frankreich    */
    {  34, 1, '/', ':', '.', ',', 0, 3, 850, "Pts" },  /* Spanien       */
    {  36, 2, '.', ':', ' ', ',', 0, 3, 852, "Ft"  },  /* Ungarn        */
    {  39, 1, '/', '.', '.', ',', 0, 2, 850, "L."  },  /* Italien       */
    {  41, 1, '.', '.', '\'', '.', 0, 2, 850, "Fr." }, /* Schweiz       */
    {  42, 1, '.', ':', ' ', ',', 0, 3, 852, "Kc"  },  /* Tschechien (alt) */
    {  43, 1, '.', ':', '.', ',', 0, 2, 850, "\x94S" }, /* Oesterreich (oeS) */
    {  44, 1, '/', ':', ',', '.', 0, 0, 850, "\x9C" }, /* Grossbritannien (Pfund) */
    {  45, 1, '-', '.', '.', ',', 0, 2, 865, "kr"  },  /* Daenemark     */
    {  46, 2, '-', '.', ' ', ',', 0, 3, 850, "kr"  },  /* Schweden      */
    {  47, 1, '.', ':', '.', ',', 0, 2, 865, "Kr"  },  /* Norwegen      */
    {  48, 2, '-', ':', ' ', ',', 0, 3, 852, "Z\x88" }, /* Polen (CP852 l-Strich) */
    {  49, 1, '.', ':', '.', ',', 0, 3, 850, "DM"  },  /* Deutschland   */
    {  55, 1, '/', ':', '.', ',', 0, 2, 850, "Cr$" },  /* Brasilien     */
    {  61, 1, '-', ':', ',', '.', 1, 0, 437, "$"   },  /* Australien    */
    {  90, 1, '/', ':', '.', ',', 0, 3, 857, "TL"  },  /* Tuerkei       */
    { 351, 1, '-', ':', '.', ',', 0, 3, 860, "Esc." }, /* Portugal      */
    { 358, 1, '.', '.', ' ', ',', 0, 3, 850, "mk"  },  /* Finnland      */
    { 420, 1, '.', ':', ' ', ',', 0, 3, 852, "K\x9F" }, /* Tschechien (CP852 c-Haken) */
    { 421, 1, '.', ':', ' ', ',', 0, 3, 852, "Sk"  },  /* Slowakei      */
};
#define CTRY_N (sizeof(ctry_tab) / sizeof(ctry_tab[0]))
#define CTRY_DE 19                          /* Vorgabe: 049            */

static struct ctry ctry;

/* Land aus der Umgebung (COUNTRY=nnn); unbekannt/fehlt -> 049 */
static void ctry_init(void)
{
    static char cb[12];
    unsigned i, n = 0, k = CTRY_DE;
    if (!FAPI_FAILED(DosGetEnv("COUNTRY", cb, sizeof(cb))))
        for (i = 0; cb[i] >= '0' && cb[i] <= '9'; i++)
            n = n * 10 + (unsigned)(cb[i] - '0');
    for (i = 0; i < CTRY_N; i++)
        if (ctry_tab[i].code == n)
            k = i;
    {
        const char *s = (const char *)&ctry_tab[k];
        char *d = (char *)&ctry;
        for (i = 0; i < sizeof(ctry); i++)
            d[i] = s[i];
    }
}

#ifndef CTRY_MIN                            /* nur Tabelle und Trenner */
static char *ctry_2(char *o, unsigned v)
{
    *o++ = (char)('0' + v / 10 % 10);
    *o++ = (char)('0' + v % 10);
    return o;
}

static char *ctry_4(char *o, unsigned v)
{
    o = ctry_2(o, v / 100);
    return ctry_2(o, v % 100);
}

/* FAT-Datum (Tag 0-4, Monat 5-8, Jahr seit 1980 ab 9) */
static void ctry_date(char *o, unsigned d)
{
    unsigned t = d & 31, m = (d >> 5) & 15, y = 1980 + (d >> 9);
    if (ctry.dfmt == 2) {
        o = ctry_4(o, y); *o++ = ctry.dsep;
        o = ctry_2(o, m); *o++ = ctry.dsep;
        o = ctry_2(o, t);
    } else {
        o = ctry_2(o, ctry.dfmt ? t : m); *o++ = ctry.dsep;
        o = ctry_2(o, ctry.dfmt ? m : t); *o++ = ctry.dsep;
        o = ctry_4(o, y);
    }
    *o = 0;
}

/* FAT-Zeit (Sek./2 0-4, Minute 5-10, Stunde 11-15); sec: mit Sekunden */
static void ctry_time(char *o, unsigned t, int sec)
{
    unsigned h = t >> 11;
    char ap = 0;
    if (ctry.h12) {
        ap = h >= 12 ? 'p' : 'a';
        h %= 12;
        if (!h)
            h = 12;
    }
    o = ctry_2(o, h); *o++ = ctry.tsep;
    o = ctry_2(o, (t >> 5) & 63);
    if (sec) {
        *o++ = ctry.tsep;
        o = ctry_2(o, (t & 31) << 1);
    }
    if (ap)
        *o++ = ap;
    *o = 0;
}

/* Eingabeformat fuer DATE: "TT.MM.JJ" in der Folge des Landes */
static void ctry_dpat(char *o)
{
    static const char *pt[3] = { "MMxTTxJJ", "TTxMMxJJ", "JJxMMxTT" };
    const char *s = pt[ctry.dfmt];
    while (*s) {
        *o++ = *s == 'x' ? ctry.dsep : *s;
        s++;
    }
    *o = 0;
}
#endif
