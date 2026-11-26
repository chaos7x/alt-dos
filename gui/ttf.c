/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - TrueType-Schriften (docs/GUI-WIN16.md "Stand: TrueType").
 * Eigener Rasterer nach der TrueType-Spezifikation (Apple/Microsoft,
 * oeffentlich): Tabellen head/hhea/maxp/OS2/cmap/loca/hmtx/glyf/name,
 * Umrisse in 26.6-Festkomma, quadratische Kurven als Strecken, Zeilen mit
 * der Nicht-Null-Regel gefuellt (Punktmitte; schmale Stellen setzen einen
 * Punkt - Ausfallschutz), keine Hinweise (Hinting), schwarzweiss.
 *
 * Schriften sucht tt_scan in WINDIR\SYSTEM, WINDIR und X:\DOS\FONTS
 * (COREFONT); die Dateien kommen nie ins Repository. Eine verwirklichte
 * Schrift (Gesicht, Pixelgroesse, Fett nachgebildet) haelt ihre Zeichen 32..255
 * (ANSI) in einem eigenen Segment, gezeichnet wird in gdi.c.
 */
#include "gui.h"
#include "ttf.h"

WORD  API _lopen(LPCSTR name, WORD mode);
WORD  API _lclose(WORD h);
WORD  API _lread(WORD h, LPVOID p, WORD n);
DWORD API _llseek(WORD h, DWORD pos, short how);
WORD  API GetWindowsDirectory(LPSTR buf, WORD n);

/* ---- Gesichter ------------------------------------------------------ */
#define NFACE   24
struct face {
    char  fam[32];              /* Familienname (name-Tabelle ID 1)     */
    char  path[64];
    BYTE  style;                /* Bit 0 fett, Bit 1 kursiv (macStyle)  */
    BYTE  sym;                  /* cmap 3/0 (Symbol)                    */
    BYTE  pitch;                /* 1 = feste Breite (post isFixedPitch) */
};
static WORD fc_sel;             /* Segment: struct face[NFACE]          */
static BYTE fc_n, fc_done;
#define FC(i)   ((struct face FAR *)MKFP(fc_sel, (i) * sizeof(struct face)))

/* ---- verwirklichte Schriften ---------------------------------------- */
#define NRF     4
#define NCH     224             /* Zeichen 32..255                      */
#define GC_HEAD (NCH * sizeof(struct ttg))
struct rfont {
    BYTE  used, face, bold;     /* bold: nachgebildet                    */
    WORD  ppem, sel, top;       /* Glyphensegment, erstes freies Byte    */
    WORD  stamp;
    struct ttm m;
    /* aus der Datei */
    WORD  upem, nhm, ngl, locl;
    DWORD o_loca, o_glyf, o_hmtx;
    WORD  gid[NCH];
};
static struct rfont rf[NRF];
static WORD rf_clock;

/* Arbeitsspeicher fuer einen Umriss (ein Segment) */
#define NPT     1200
#define NEDGE   2400
#define W_PX    0                       /* short x[NPT]                 */
#define W_PY    (W_PX + 2 * NPT)
#define W_FL    (W_PY + 2 * NPT)        /* BYTE flags[NPT]              */
#define W_EP    (W_FL + NPT)            /* WORD end[256]                */
#define W_ED    (W_EP + 512)            /* short e[NEDGE][4]            */
#define W_BUF   (W_ED + 8 * NEDGE)      /* Rohdaten eines Glyphs        */
#define W_BUFN  8192
#define W_ROW   (W_BUF + W_BUFN)        /* Bitmap im Bau (256 Zeilen*32) */
#define W_END   (W_ROW + 8192)
static WORD ws;                         /* Arbeitssegment               */
static WORD npt, nedge;
#define PX(i)   (((short FAR *)MKFP(ws, W_PX))[i])
#define PY(i)   (((short FAR *)MKFP(ws, W_PY))[i])
#define PFL(i)  (((BYTE FAR *)MKFP(ws, W_FL))[i])
#define ED(i)   ((short FAR *)MKFP(ws, W_ED + (i) * 8))

static WORD fh = 0xFFFF;                /* offene Datei                 */
static BYTE fh_face = 0xFF;

static WORD be16(const BYTE FAR *p) { return (WORD)p[0] << 8 | p[1]; }
static DWORD be32(const BYTE FAR *p) { return (DWORD)be16(p) << 16 | be16(p + 2); }

static int f_open(int face)
{
    if (fh != 0xFFFF && fh_face == face)
        return 0;
    if (fh != 0xFFFF)
        _lclose(fh);
    fh = _lopen(FC(face)->path, 0);
    fh_face = fh == 0xFFFF ? 0xFF : (BYTE)face;
    return fh == 0xFFFF ? -1 : 0;
}
static int f_rd(DWORD pos, void FAR *p, WORD n)
{
    if (_llseek(fh, pos, 0) != pos)
        return -1;
    return _lread(fh, p, n) == n ? 0 : -1;
}
static WORD f_rd16(DWORD pos)
{
    static BYTE b[2];
    return f_rd(pos, (void FAR *)b, 2) ? 0 : be16(b);
}

/* Tabellenverzeichnis: Lage einer Tabelle, 0 = fehlt */
static DWORD tbl(const char *tag, DWORD FAR *len)
{
    static BYTE h[16];
    WORD n, i;
    if (f_rd(0, (void FAR *)h, 12))
        return 0;
    n = be16(h + 4);
    for (i = 0; i < n && i < 64; i++) {
        if (f_rd(12 + (DWORD)i * 16, (void FAR *)h, 16))
            return 0;
        if (h[0] == tag[0] && h[1] == tag[1] && h[2] == tag[2] && h[3] == tag[3]) {
            if (len)
                *len = be32(h + 12);
            return be32(h + 8);
        }
    }
    return 0;
}

/* ---- Gesichter suchen ------------------------------------------------ */
static void face_name(struct face FAR *f)
{
    static BYTE r[12];
    DWORD o = tbl("name", 0), so;
    WORD n, i, k, best = 0xFFFF, bl = 0, bo = 0;
    if (!o)
        return;
    n = f_rd16(o + 2);
    so = o + f_rd16(o + 4);
    for (i = 0; i < n && i < 64; i++) {
        if (f_rd(o + 6 + (DWORD)i * 12, (void FAR *)r, 12))
            return;
        if (be16(r + 6) != 1)                   /* nameID 1 = Familie   */
            continue;
        if (be16(r) == 3 && best != 3) {        /* Microsoft, UTF-16BE  */
            best = 3; bl = be16(r + 8); bo = be16(r + 10);
        } else if (be16(r) == 1 && best == 0xFFFF) {
            best = 1; bl = be16(r + 8); bo = be16(r + 10);
        }
    }
    if (best == 0xFFFF)
        return;
    if (bl > 62)
        bl = 62;
    if (f_rd(so + bo, MKFP(ws, W_BUF), bl))
        return;
    for (i = k = 0; i < bl && k < 31; i += best == 3 ? 2 : 1) {
        BYTE c = ((BYTE FAR *)MKFP(ws, W_BUF))[best == 3 ? i + 1 : i];
        f->fam[k++] = (char)c;
    }
    f->fam[k] = 0;
}

static void scan_dir(LPCSTR dir)
{
    static struct fapi_dirent e;
    static char pat[80];
    WORD i, n, x;
    for (n = 0; dir[n] && n < 60; n++)
        pat[n] = dir[n];
    pat[n] = 0;
    for (i = 0; fc_n < NFACE; i++) {
        struct face FAR *f;
        x = fapi_call(FAPI_FINDENTRY, (WORD)&e, i, (WORD)pat);
        if (FAPI_FAILED(x))
            break;
        x = fstrlen(e.name);
        if (x < 5 || (e.attr & 0x10) || fstricmp(e.name + x - 4, ".TTF") || n + x + 2 > 63)
            continue;
        f = FC(fc_n);
        fmemset((void FAR *)f, 0, sizeof(*f));
        fmemcpy(f->path, pat, n);
        f->path[n] = '\\';
        fstrncpy(f->path + n + 1, e.name, 62 - n);
        if (f_open(fc_n))
            continue;
        {
            DWORD h = tbl("head", 0), c = tbl("cmap", 0), p = tbl("post", 0);
            WORD k, nt;
            if (!h || !c)
                continue;
            f->style = (BYTE)(f_rd16(h + 44) & 3);
            nt = f_rd16(c + 2);
            for (k = 0; k < nt && k < 16; k++)
                if (f_rd16(c + 4 + k * 8) == 3 && f_rd16(c + 6 + k * 8) == 0)
                    f->sym = 1;
            if (p)
                f->pitch = f_rd16(p + 14) || f_rd16(p + 12) ? 1 : 0;   /* isFixedPitch */
        }
        face_name(f);
        if (f->fam[0])
            fc_n++;
    }
    if (fh != 0xFFFF) {
        _lclose(fh);
        fh = 0xFFFF;
        fh_face = 0xFF;
    }
}

void tt_scan(void)
{
    static char d[80];
    WORD n;
    if (fc_done)
        return;
    fc_done = 1;
    fc_sel = seg_new(NFACE * sizeof(struct face));
    ws = seg_new(W_END);
    if (!fc_sel || !ws)
        return;
    n = GetWindowsDirectory(d, 60);
    fstrncpy(d + n, "\\SYSTEM", 10);
    scan_dir(d);
    d[n] = 0;
    scan_dir(d);
    d[1] = ':'; d[2] = 0;
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", d + 40, 40)) && d[41] == ':')
        d[0] = d[40];
    else
        d[0] = 'C';
    fstrncpy(d + 2, "\\DOS\\FONTS", 12);
    scan_dir(d);
}

int tt_nface(void) { tt_scan(); return fc_n; }
LPCSTR tt_facename(int i) { return i < fc_n ? FC(i)->fam : 0; }
BYTE tt_facestyle(int i) { return i < fc_n ? FC(i)->style | (FC(i)->pitch << 4) | (FC(i)->sym << 5) : 0; }

/* Gesicht zu Familie und Wunschstil: genau passender Stil zuerst */
int tt_find(LPCSTR fam, BYTE style)
{
    int i, best = -1, bs = 99;
    tt_scan();
    for (i = 0; i < fc_n; i++) {
        int d;
        if (fstricmp(FC(i)->fam, fam))
            continue;
        d = (FC(i)->style ^ style) & 3;
        d = (d & 2) * 2 + (d & 1);              /* kursiv wiegt mehr     */
        if (d < bs) { bs = d; best = i; }
    }
    return best;
}

/* ---- Laden einer verwirklichten Schrift ------------------------------ */
/* Format 4 (Segmente endCode/startCode/idDelta/idRangeOffset), die
   Untertabelle liegt bei der Abfrage im Arbeitssegment ab W_BUF */
static WORD cmap_lookup(WORD ch)
{
    const BYTE FAR *t = MKFP(ws, W_BUF);
    WORD segx2 = be16(t + 6), i, e, s, dl, ro;
    WORD ends = 14, starts = ends + segx2 + 2;
    if ((DWORD)starts + 3 * segx2 > W_BUFN)
        return 0;
    for (i = 0; i < segx2; i += 2) {
        e = be16(t + ends + i);
        if (e < ch)
            continue;
        s = be16(t + starts + i);
        if (s > ch)
            return 0;
        dl = be16(t + starts + segx2 + i);
        ro = be16(t + starts + 2 * segx2 + i);
        if (!ro)
            return (WORD)(ch + dl);
        {
            DWORD o = (DWORD)starts + 2 * segx2 + i + ro + 2 * (ch - s);
            WORD g = o + 2 <= W_BUFN ? be16(t + (WORD)o) : 0;
            return g ? (WORD)(g + dl) : 0;
        }
    }
    return 0;
}

/* ANSI (Windows 1252) 80h..9Fh -> Unicode */
static const WORD __far a1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
};

static short scl(struct rfont *r, short v)      /* Schrifteinheiten -> Punkte */
{
    long t = (long)v * r->ppem;
    return (short)((t >= 0 ? t + r->upem / 2 : t - r->upem / 2) / (long)r->upem);
}

static int rf_load(struct rfont *r, int with_cmap)
{
    DWORD h, hh, mx, os, cm, len;
    WORD nt, k, sub3 = 0, sub0 = 0, c;
    DWORD sub = 0;
    if (f_open(r->face))
        return -1;
    h = tbl("head", 0); hh = tbl("hhea", 0); mx = tbl("maxp", 0);
    cm = tbl("cmap", 0); os = tbl("OS/2", 0);
    r->o_loca = tbl("loca", 0); r->o_glyf = tbl("glyf", &len); r->o_hmtx = tbl("hmtx", 0);
    if (!h || !hh || !mx || !cm || !r->o_loca || !r->o_glyf || !r->o_hmtx)
        return -1;
    r->upem = f_rd16(h + 18);
    r->locl = f_rd16(h + 50);
    r->ngl = f_rd16(mx + 4);
    r->nhm = f_rd16(hh + 34);
    if (!r->upem)
        return -1;
    if (os) {
        r->m.asc = scl(r, f_rd16(os + 74));
        r->m.desc = scl(r, f_rd16(os + 76));
        r->m.avgw = scl(r, f_rd16(os + 2));
    } else {
        r->m.asc = scl(r, f_rd16(hh + 4));
        r->m.desc = -scl(r, f_rd16(hh + 6));
        r->m.avgw = r->ppem / 2;
    }
    r->m.maxw = scl(r, f_rd16(hh + 10));
    r->m.extlead = scl(r, f_rd16(hh + 8));
    if (r->m.desc < 0)
        r->m.desc = -r->m.desc;
    r->m.height = r->m.asc + r->m.desc;
    r->m.intlead = r->m.height - r->ppem;
    if (r->m.intlead < 0)
        r->m.intlead = 0;
    if (r->bold) {
        r->m.avgw++;
        r->m.maxw++;
    }
    if (!with_cmap)
        return 0;
    nt = f_rd16(cm + 2);
    for (k = 0; k < nt && k < 16; k++) {
        WORD pl = f_rd16(cm + 4 + k * 8), en = f_rd16(cm + 6 + k * 8);
        DWORD so = cm + ((DWORD)f_rd16(cm + 8 + k * 8) << 16 | f_rd16(cm + 10 + k * 8));
        if (f_rd16(so) != 4)
            continue;
        if (pl == 3 && en == 1) { sub = so; sub3 = 1; }
        else if (pl == 3 && en == 0 && !sub3) { sub = so; sub0 = 1; }
    }
    if (sub) {
        WORD sl = f_rd16(sub + 2);
        if (sl > W_BUFN || f_rd(sub, MKFP(ws, W_BUF), sl))
            sub = 0;
    }
    for (c = 0; c < NCH; c++) {
        WORD ch = c + 32, u = ch;
        if (!sub)
            r->gid[c] = 0;
        else if (sub0 && !sub3)
            r->gid[c] = cmap_lookup(0xF000 + ch);
        else {
            if (ch >= 0x80 && ch < 0xA0)
                u = a1252[ch - 0x80];
            r->gid[c] = u ? cmap_lookup(u) : 0;
        }
    }
    return 0;
}

/* Pixelgroesse aus lfHeight: > 0 Zellenhoehe, < 0 Zeichenhoehe (em) */
int tt_realize(int face, short lfh, BYTE bold)
{
    int i, old = 0;
    WORD ppem;
    struct rfont *r;
    long a;
    if (face < 0 || face >= fc_n)
        return -1;
    if (lfh == 0)
        lfh = -13;                              /* wie 10 Punkt          */
    if (lfh < 0)
        ppem = -lfh;
    else {
        /* Zellenhoehe -> em ueber das Verhaeltnis winAscent+winDescent */
        static struct rfont t;
        fmemset((void FAR *)&t, 0, sizeof(t));
        t.face = (BYTE)face; t.ppem = 1000;
        if (rf_load(&t, 0))
            return -1;
        a = t.m.asc + t.m.desc;
        ppem = (WORD)(a ? ((long)lfh * 1000 + a / 2) / a : lfh);
    }
    if (ppem < 4) ppem = 4;
    if (ppem > 200) ppem = 200;
    for (i = 0; i < NRF; i++)
        if (rf[i].used && rf[i].face == face && rf[i].ppem == ppem && rf[i].bold == bold) {
            rf[i].stamp = ++rf_clock;
            return i;
        }
    for (i = 0; i < NRF; i++) {
        if (!rf[i].used) { old = i; break; }
        if (rf[i].stamp < rf[old].stamp)
            old = i;
    }
    r = &rf[old];
    if (r->used && r->sel)
        seg_del(r->sel);
    fmemset((void FAR *)r, 0, sizeof(*r));
    r->face = (BYTE)face; r->ppem = ppem; r->bold = bold;
    if (rf_load(r, 1))
        return -1;
    r->sel = seg_new(0xC000u);
    if (!r->sel)
        return -1;
    fmemset(MKFP(r->sel, 0), 0, GC_HEAD);
    r->top = GC_HEAD;
    r->used = 1;
    r->stamp = ++rf_clock;
    return old;
}

const struct ttm FAR *tt_metrics(int i) { return (const struct ttm FAR *)&rf[i].m; }

/* ---- Umriss lesen ---------------------------------------------------- */
static DWORD glyph_at(struct rfont *r, WORD g, WORD FAR *len)
{
    static BYTE b[8];
    DWORD a, e;
    if (g >= r->ngl)
        return 0;
    if (r->locl) {
        if (f_rd(r->o_loca + (DWORD)g * 4, (void FAR *)b, 8))
            return 0;
        a = be32(b); e = be32(b + 4);
    } else {
        if (f_rd(r->o_loca + (DWORD)g * 2, (void FAR *)b, 4))
            return 0;
        a = (DWORD)be16(b) * 2; e = (DWORD)be16(b + 2) * 2;
    }
    *len = e > a ? (WORD)(e - a > W_BUFN ? W_BUFN : e - a) : 0;
    return r->o_glyf + a;
}

/* einfache Glyphe nach Punkten (Konturenden in W_EP), dx/dy in Einheiten */
static int add_simple(struct rfont *r, WORD g, short dx, short dy, WORD FAR *nc)
{
    WORD len, nct, i, n, ninst, p;
    BYTE FAR *b = MKFP(ws, W_BUF), fl = 0;
    WORD FAR *ep = MKFP(ws, W_EP);
    DWORD at = glyph_at(r, g, &len);
    short v;
    WORD base = npt;
    if (!at || len < 10)
        return 0;
    if (f_rd(at, b, len))
        return -1;
    nct = be16(b);
    if (nct & 0x8000u) {                        /* zusammengesetzt       */
        WORD o = 10, f, cg, depth = 0;
        do {
            short a1, a2;
            f = be16(b + o); cg = be16(b + o + 2); o += 4;
            if (f & 1) { a1 = (short)be16(b + o); a2 = (short)be16(b + o + 2); o += 4; }
            else { a1 = (signed char)b[o]; a2 = (signed char)b[o + 1]; o += 2; }
            if (f & 8) o += 2;
            else if (f & 0x40) o += 4;
            else if (f & 0x80) o += 8;
            if (!(f & 2))                       /* Punkte statt Versatz  */
                a1 = a2 = 0;
            {
                /* Puffer wird ueberschrieben: Rest sichern */
                static BYTE rest[256];
                WORD rl = len - o > 256 ? 256 : len - o, k;
                for (k = 0; k < rl; k++)
                    rest[k] = b[o + k];
                if (add_simple(r, cg, dx + a1, dy + a2, nc) < 0)
                    return -1;
                for (k = 0; k < rl; k++)
                    b[k] = rest[k];
                len = rl;
                o = 0;
            }
        } while ((f & 0x20) && ++depth < 16 && o + 4 <= len);
        return 0;
    }
    if (*nc + nct > 255)
        return 0;
    for (i = 0; i < nct; i++)
        ep[*nc + i] = base + be16(b + 10 + i * 2);
    n = nct ? be16(b + 10 + (nct - 1) * 2) + 1 : 0;
    if (base + n > NPT)
        return 0;
    p = 10 + nct * 2;
    ninst = be16(b + p);
    p += 2 + ninst;
    for (i = 0; i < n; ) {                      /* Merker                */
        BYTE rep = 0;
        fl = b[p++];
        if (fl & 8)
            rep = b[p++];
        PFL(base + i++) = fl;
        while (rep-- && i < n)
            PFL(base + i++) = fl;
    }
    v = 0;
    for (i = 0; i < n; i++) {                   /* x                     */
        fl = PFL(base + i);
        if (fl & 2) { v += (fl & 16) ? b[p] : -(short)b[p]; p++; }
        else if (!(fl & 16)) { v += (short)be16(b + p); p += 2; }
        PX(base + i) = v;
    }
    v = 0;
    for (i = 0; i < n; i++) {                   /* y                     */
        fl = PFL(base + i);
        if (fl & 4) { v += (fl & 32) ? b[p] : -(short)b[p]; p++; }
        else if (!(fl & 32)) { v += (short)be16(b + p); p += 2; }
        PY(base + i) = v;
    }
    for (i = 0; i < n; i++) {                   /* 26.6, y nach unten    */
        long x = (long)(PX(base + i) + dx) * r->ppem * 64, y = (long)(PY(base + i) + dy) * r->ppem * 64;
        PX(base + i) = (short)((x >= 0 ? x + r->upem / 2 : x - r->upem / 2) / (long)r->upem);
        PY(base + i) = -(short)((y >= 0 ? y + r->upem / 2 : y - r->upem / 2) / (long)r->upem);
    }
    npt = base + n;
    *nc += nct;
    return 0;
}

static void edge(short x0, short y0, short x1, short y1)
{
    short FAR *e;
    if ((y0 == y1 && x0 == x1) || nedge >= NEDGE)
        return;                         /* waagerechte bleiben: Spalten */
    e = ED(nedge++);
    e[0] = x0; e[1] = y0; e[2] = x1; e[3] = y1;
}
static void quad(short x0, short y0, short cx, short cy, short x1, short y1)
{
    short n, i, dx = x1 - x0, dy = y1 - y0, px = x0, py = y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    n = (dx + dy) / 128 + 2;                    /* etwa alle 2 Punkte    */
    if (n > 12) n = 12;
    for (i = 1; i <= n; i++) {
        long t = (long)i * 64 / n, u = 64 - t;
        short x = (short)((u * u * x0 + 2 * u * t * cx + t * t * x1) / 4096L);
        short y = (short)((u * u * y0 + 2 * u * t * cy + t * t * y1) / 4096L);
        edge(px, py, x, y);
        px = x; py = y;
    }
}

/* Konturen -> Kanten (Punkte ohne Kurvenmerker: Mittelpunkte einfuegen) */
static void outline(WORD nc)
{
    WORD FAR *ep = MKFP(ws, W_EP);
    WORD c, s = 0;
    nedge = 0;
    for (c = 0; c < nc; c++) {
        WORD e = ep[c], i, n = e - s + 1, k, st;
        short sx, sy, lx, ly;
        if (e < s || e >= npt) { s = e + 1; continue; }
        /* Anfang auf einem Punkt der Kurve */
        for (st = 0; st < n && !(PFL(s + st) & 1); st++)
            ;
        if (st == n) {                          /* nur Kontrollpunkte    */
            sx = (PX(s) + PX(s + 1)) / 2; sy = (PY(s) + PY(s + 1)) / 2;
            st = 0;
        } else {
            sx = PX(s + st); sy = PY(s + st);
        }
        lx = sx; ly = sy;
        for (k = 1; k <= n; k++) {
            i = s + (st + k) % n;
            if (PFL(i) & 1) {
                edge(lx, ly, PX(i), PY(i));
                lx = PX(i); ly = PY(i);
            } else {
                WORD j = s + (st + k + 1) % n;
                short nx, ny;
                if (PFL(j) & 1) { nx = PX(j); ny = PY(j); k++; }
                else { nx = (PX(i) + PX(j)) / 2; ny = (PY(i) + PY(j)) / 2; }
                if (k > n) { nx = sx; ny = sy; }
                quad(lx, ly, PX(i), PY(i), nx, ny);
                lx = nx; ly = ny;
            }
        }
        if (lx != sx || ly != sy)
            edge(lx, ly, sx, sy);
        s = e + 1;
    }
}

/* Zeilen fuellen -> Bitmap in W_ROW (pitch Bytes je Zeile) */
static void fill(short x0, short y0, WORD w, WORD h, WORD pitch)
{
    static short xs[64];
    static signed char ds[64];
    BYTE FAR *bm = MKFP(ws, W_ROW);
    WORD r, i, k, n;
    fmemset(bm, 0, pitch * h);
    for (r = 0; r < h; r++) {
        short yc = (y0 + (short)r) * 64 + 32;
        n = 0;
        for (i = 0; i < nedge && n < 64; i++) {
            short FAR *e = ED(i);
            short ya = e[1], yb = e[3];
            signed char d = 1;
            if (ya > yb) { short t = ya; ya = yb; yb = t; d = -1; }
            if (yc < ya || yc >= yb)
                continue;
            xs[n] = (short)(e[0] + (long)(yc - e[1]) * (e[2] - e[0]) / (e[3] - e[1]));
            ds[n] = d;
            for (k = n; k > 0 && xs[k - 1] > xs[k]; k--) {   /* sortieren */
                short t = xs[k]; signed char td = ds[k];
                xs[k] = xs[k - 1]; ds[k] = ds[k - 1];
                xs[k - 1] = t; ds[k - 1] = td;
            }
            n++;
        }
        {
            short wind = 0;
            for (k = 0; k + 1 < n; k++) {
                short a, b, pa, pb, p;
                wind += ds[k];
                if (!wind)
                    continue;
                a = xs[k] - x0 * 64; b = xs[k + 1] - x0 * 64;
                pa = (a + 31) >> 6; pb = (b + 31) >> 6;     /* Mitten in [a,b) */
                if (pa >= pb && b > a)                      /* Ausfallschutz   */
                    pa = (a + b) / 128, pb = pa + 1;
                for (p = pa < 0 ? 0 : pa; p < pb && p < (short)w; p++)
                    bm[r * pitch + (p >> 3)] |= 0x80 >> (p & 7);
            }
        }
    }
    /* Spalten: duenne waagerechte Striche (Querstrich des e) nicht
       ausfallen lassen - nur Stuecke ohne Zeilenmitte setzen */
    for (r = 0; r < w; r++) {
        short xc = (x0 + (short)r) * 64 + 32;
        n = 0;
        for (i = 0; i < nedge && n < 64; i++) {
            short FAR *e = ED(i);
            short xa = e[0], xb = e[2];
            signed char d = 1;
            if (xa > xb) { short t = xa; xa = xb; xb = t; d = -1; }
            if (xc < xa || xc >= xb)
                continue;
            xs[n] = (short)(e[1] + (long)(xc - e[0]) * (e[3] - e[1]) / (e[2] - e[0]));
            ds[n] = d;
            for (k = n; k > 0 && xs[k - 1] > xs[k]; k--) {
                short t = xs[k]; signed char td = ds[k];
                xs[k] = xs[k - 1]; ds[k] = ds[k - 1];
                xs[k - 1] = t; ds[k - 1] = td;
            }
            n++;
        }
        {
            short wind = 0;
            for (k = 0; k + 1 < n; k++) {
                short a, b, q;
                wind += ds[k];
                if (!wind)
                    continue;
                a = xs[k] - y0 * 64; b = xs[k + 1] - y0 * 64;
                if (b <= a || ((a + 31) >> 6) < ((b + 31) >> 6))
                    continue;                           /* trifft eine Mitte */
                q = (a + b) >> 7;
                if (q >= 0 && q < (short)h)
                    bm[q * pitch + (r >> 3)] |= 0x80 >> (r & 7);
            }
        }
    }
}

/* ---- Glyphe holen (aus dem Speicher oder neu gerastert) -------------- */
const BYTE FAR *tt_glyph(int ri, BYTE ch, const struct ttg FAR * FAR *gout)
{
    struct rfont *r = &rf[ri];
    struct ttg FAR *g;
    WORD c = ch < 32 ? 0 : ch - 32, nc = 0, i, gid;
    short xmin = 32767, ymin = 32767, xmax = -32768, ymax = -32768;
    WORD w, h, pitch, sz, adv;
    g = (struct ttg FAR *)MKFP(r->sel, c * sizeof(struct ttg));
    *gout = g;
    if (g->ok)
        return (const BYTE FAR *)MKFP(r->sel, g->off);
    if (f_open(r->face))
        return 0;
    gid = r->gid[c];
    /* Vorschub aus hmtx */
    adv = f_rd16(r->o_hmtx + (DWORD)(gid < r->nhm ? gid : r->nhm - 1) * 4);
    adv = (WORD)scl(r, (short)adv) + r->bold;
    npt = 0;
    if (gid && add_simple(r, gid, 0, 0, &nc) < 0)
        nc = 0;
    if (nc)
        outline(nc);
    else
        nedge = 0;
    for (i = 0; i < npt; i++) {
        if (PX(i) < xmin) xmin = PX(i);
        if (PX(i) > xmax) xmax = PX(i);
        if (PY(i) < ymin) ymin = PY(i);
        if (PY(i) > ymax) ymax = PY(i);
    }
    g->adv = (BYTE)(adv > 255 ? 255 : adv);
    g->ok = 1;
    if (!nedge || xmax <= xmin) {
        g->w = g->h = 0;
        g->off = 0;
        return (const BYTE FAR *)MKFP(r->sel, 0);
    }
    xmin >>= 6; ymin >>= 6;                     /* ganze Punkte (abrunden) */
    xmax = (xmax + 63) >> 6; ymax = (ymax + 63) >> 6;
    w = xmax - xmin + r->bold;
    h = ymax - ymin;
    if (w > 255) w = 255;
    if (h > 255) h = 255;
    pitch = (w + 7) >> 3;
    fill(xmin, ymin, w, h, pitch);
    if (r->bold) {                              /* fett nachbilden        */
        BYTE FAR *bm = MKFP(ws, W_ROW);
        WORD rr, k;
        for (rr = 0; rr < h; rr++) {
            BYTE carry = 0;
            for (k = 0; k < pitch; k++) {
                BYTE v = bm[rr * pitch + k], nv = (BYTE)(v >> 1 | carry);
                carry = (BYTE)(v << 7);
                bm[rr * pitch + k] = v | nv;
            }
        }
    }
    sz = pitch * h;
    if ((DWORD)r->top + sz > 0xC000u) {         /* voll: alles verwerfen  */
        fmemset(MKFP(r->sel, 0), 0, GC_HEAD);
        r->top = GC_HEAD;
        g->adv = (BYTE)(adv > 255 ? 255 : adv);
        g->ok = 1;
    }
    fmemcpy(MKFP(r->sel, r->top), MKFP(ws, W_ROW), sz);
    g->off = r->top;
    r->top += sz;
    g->w = (BYTE)w; g->h = (BYTE)h;
    g->xo = xmin;
    g->yo = ymin;
    return (const BYTE FAR *)MKFP(r->sel, g->off);
}

WORD tt_adv(int ri, BYTE ch)
{
    const struct ttg FAR *g;
    tt_glyph(ri, ch, &g);
    return g->adv;
}
