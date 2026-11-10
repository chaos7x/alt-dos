/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * namepat.h - Zielmuster auf einen Dateinamen anwenden (wie COPY/COMP/FC
 * unter MS-DOS: *.BAK, NAME.*, ?). Per #include in COMP und FC.
 */
/* Muster 2 auf den Namen aus Datei 1 anwenden: *.BAK, NAME.*, ?-Zeichen */
static void part(const char *n, unsigned nl, const char *p, unsigned pl, char *o, unsigned *k)
{
    unsigned i;
    if (pl == 1 && p[0] == '*') {
        for (i = 0; i < nl; i++)
            o[(*k)++] = n[i];
        return;
    }
    for (i = 0; i < pl && p[i] != '*'; i++)
        o[(*k)++] = p[i] == '?' ? (i < nl ? n[i] : 0) : p[i];
    if (i < pl)                         /* "AB*": Rest des Namens        */
        for (; i < nl; i++)
            o[(*k)++] = n[i];
}

static void apply_pat(const char *name, const char *pat, char *o)
{
    unsigned nd = slen(name), pd = slen(pat), nl = nd, pl = pd, k = 0, i;
    for (i = 0; name[i]; i++)
        if (name[i] == '.')
            nd = i;
    for (i = 0; pat[i]; i++)
        if (pat[i] == '.')
            pd = i;
    part(name, nd, pat, pd, o, &k);
    if (pd < pl) {
        o[k++] = '.';
        part(nd < nl ? name + nd + 1 : "", nd < nl ? nl - nd - 1 : 0, pat + pd + 1, pl - pd - 1, o, &k);
    }
    o[k] = 0;
    for (i = 0, nd = 0; i <= k; i++)    /* ?-Luecken entfernen           */
        if (o[i] || i == k)
            o[nd++] = o[i];
}

