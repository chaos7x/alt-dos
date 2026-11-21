/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - Dateien, Handles, Verzeichnisse, Geraetenamen.
 *
 * Die Dateiarbeit selbst macht das DEM im AT-DOS-Kernel (INT E0h 30h,
 * include/dem.h); hier stehen nur die Strukturen, die Programme sehen
 * (SFT, JFT im PSP, CDS, DTA) und die Uebersetzung der INT-21h-Aufrufe.
 * Pfade gehen kanonisch ("X:\DIR\NAME", gross, 8.3) an das DEM.
 */
#include "cm.h"

struct dem_req q;

/* SFT: Kopf (Fernzeiger auf die naechste Tabelle, Zahl) + Eintraege */
static u8 sft_area[6 + NSFT * sizeof(struct sft)];
struct sft *sftab;
u16 sft_head(void) { return (u16)sft_area; }

/* Geraete: Kette ab NUL wie bei MS-DOS (LoL+22h haelt eine Kopie) */
struct devh devs[NDEV];
static const struct { u16 attr; char name[9]; } devdef[NDEV] = {
    { 0x8004, "NUL     " },
    { 0x8013, "CON     " },
    { 0x8000, "AUX     " },
    { 0xA0C0, "PRN     " },
    { 0x8008, "CLOCK$  " },
    { 0xC000, "EMMXXXX0" },
    { 0x8000, "ATIN$   " },
    { 0x8000, "ATOUT$  " },
    { 0x8000, "ATERR$  " },
    { 0xA0C0, "LPT1    " },
    { 0x8000, "COM1    " },
    { 0x0842, "\032ATDOS  " },         /* Blockgeraet: 26 Einheiten     */
};

extern char retf_entry[];
static const u8 media = 0xF8;

/* ---- DEM ---------------------------------------------------------------- */
u8 crit_abort;                          /* INT 24h: Abbrechen gewaehlt   */
extern char int24_entry[];

/* kritischer Fehler wie MS-DOS: INT 24h auf dem Stapel des Programms
   (ueber dessen INT-21h-Rahmen), AL = Antwort. Ohne eigenen Handler
   (Vektor zeigt noch auf int24_entry) gibt es keinen Aufruf: -1. */
static int crit(unsigned fn, unsigned e)
{
    unsigned ah, di, a;
    if (!user_frame || sda[0] || (W(0, 0x24 * 4) == (u16)int24_entry &&
                                  W(0, 0x24 * 4 + 2) == kseg))
        return -1;
    ah = 0x38;                          /* Fehler/Wiederholen/Ignorieren */
    if (fn == DEM_WRITE || fn == DEM_MKDIR || fn == DEM_RMDIR || fn == DEM_DELETE ||
        fn == DEM_RENAME || fn == DEM_SETATTR || fn == DEM_CLOSE)
        ah |= 1;                        /* Schreiben                     */
    ah |= fn == DEM_READ || fn == DEM_WRITE ? 6 : 4;   /* Daten/Verzeichnis */
    di = e == 0x13 ? 0 : e == 0x15 ? 2 : 0x0C;
    sda[0] = 1;                         /* ErrorMode                     */
    sda[2] = (u8)q.drive;
    a = crit24((ah << 8) | (q.drive & 0xFF), di, kseg, (u16)&devs[DV_BLK]) & 0xFF;
    sda[0] = 0;
    return (int)a;
}

unsigned dem(unsigned fn)
{
    struct rregs r;
    int a;
    for (;;) {
        q.fn = fn;
        q.err = 0;
        r.ax = 0x3000;
        r.es = kseg;
        r.di = (u16)&q;
        bint(0xE0, &r);
        if ((q.err != 0x13 && q.err != 0x15 && q.err != 0x1F) ||
            fn == DEM_DINFO || fn == DEM_ABSREAD || (a = crit(fn, q.err)) < 0)
            return q.err;
        if (a == 1)                     /* Wiederholen                   */
            continue;
        if (a == 0)                     /* Ignorieren                    */
            return 0;
        if (a == 2)                     /* Abbrechen: nach dem Aufruf    */
            crit_abort = 1;
        return 0x53;                    /* Fehler auf INT 24h (MS-DOS)   */
    }
}

static u32 lin(u16 seg, u16 off)
{
    return ((u32)seg << 4) + off;
}

/* ---- Pfade -------------------------------------------------------------- */
static char upc(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

/* in (Box) -> out "X:\A\B" (gross, 8.3, . und .. aufgeloest); 0 oder Fehler */
int canon(const char __far *in, char *out, unsigned *drv)
{
    unsigned d = cur_drv, o, n, e, dot;
    char c;
    if (in[0] && in[1] == ':') {
        d = (unsigned)(upc(in[0]) - 'A');
        in += 2;
    }
    if (d >= NDRV || !drive_ok(d))
        return E_NOPATH;                /* wie MS-DOS bei Dateiaufrufen  */
    if (drv)
        *drv = d;
    if (*in == '\\' || *in == '/') {
        out[0] = (char)('A' + d);
        out[1] = ':';
        out[2] = 0;
        o = 2;
    } else {
        for (o = 0; cdstab[d].path[o] && o < 66; o++)
            out[o] = cdstab[d].path[o];
        if (out[o - 1] == '\\')
            o--;                        /* Stamm "X:\" -> "X:"           */
        out[o] = 0;
    }
    while (*in) {
        while (*in == '\\' || *in == '/')
            in++;
        if (!*in)
            break;
        if (in[0] == '.' && (in[1] == 0 || in[1] == '\\' || in[1] == '/')) {
            in++;
            continue;
        }
        if (in[0] == '.' && in[1] == '.' &&
            (in[2] == 0 || in[2] == '\\' || in[2] == '/')) {
            in += 2;
            while (o > 2 && out[o - 1] != '\\')
                o--;
            if (o > 2)
                o--;
            out[o] = 0;
            continue;
        }
        if (o > 118)
            return E_NOPATH;
        out[o++] = '\\';
        n = e = dot = 0;
        for (; (c = *in) != 0 && c != '\\' && c != '/'; in++) {
            if (c == '.') {
                if (dot)
                    continue;
                dot = 1;
                out[o++] = '.';
                continue;
            }
            if (c == ' ')
                continue;               /* "A   .TXT" = A.TXT (DOS)      */
            if ((u8)c < ' ' || c == '"' || c == '|' || c == '<' || c == '>' ||
                c == '+' || c == '=' || c == ';' || c == ',' || c == '[' || c == ']')
                return E_NOFILE;
            if (dot ? e++ < 3 : n++ < 8)
                out[o++] = upc(c);
        }
        if (dot && !e)
            o--;                        /* "NAME." -> "NAME"             */
        out[o] = 0;
    }
    if (o == 2) {
        out[2] = '\\';
        out[3] = 0;
    }
    return 0;
}

/* letzter Teil des Pfads ein Geraetename? -> DV_xx oder -1 */
int dev_of(const char *path)
{
    const char *b = path;
    unsigned i, k;
    for (i = 0; path[i]; i++)
        if (path[i] == '\\' || path[i] == ':')
            b = path + i + 1;
    for (k = 0; k < NDEV; k++) {
        for (i = 0; i < 8; i++) {
            char c = b[i] == '.' ? 0 : b[i];
            if (devdef[k].name[i] == ' ') {
                if (c)
                    break;
                i = 8;
                break;
            }
            if (c != devdef[k].name[i])
                break;
        }
        if (i == 8)
            return (int)k;
    }
    return -1;
}

/* ---- SFT und JFT -------------------------------------------------------- */
void file_init(void)
{
    unsigned k, i;
    sftab = (struct sft *)(sft_area + 6);
    *(u16 *)(sft_area + 0) = 0xFFFF;
    *(u16 *)(sft_area + 2) = 0xFFFF;
    *(u16 *)(sft_area + 4) = NSFT;
    for (k = 0; k < NDEV; k++) {
        devs[k].next = k + 1 < NDEV ? ((u32)kseg << 16) | (u16)&devs[k + 1]
                                    : 0xFFFFFFFFUL;
        devs[k].attr = devdef[k].attr;
        devs[k].strat = devs[k].intr = (u16)retf_entry;
        for (i = 0; i < 8; i++)
            devs[k].name[i] = devdef[k].name[i];
    }
}

static int sft_new(void)
{
    int s;
    for (s = 0; s < NSFT; s++)
        if (!sftab[s].refs) {
            u8 *p = (u8 *)&sftab[s];
            unsigned i;
            for (i = 0; i < sizeof(struct sft); i++)
                p[i] = 0;
            sftab[s].refs = 1;
            return s;
        }
    return -1;
}

static u8 __far *jft(u16 psp)
{
    return (u8 __far *)MK_FP(W(psp, 0x36), W(psp, 0x34));
}

int hnd_sft(unsigned h)
{
    u16 p = cur_psp();
    u8 b;
    if (h >= W(p, 0x32))
        return -1;
    b = jft(p)[h];
    if (b >= NSFT || !sftab[b].refs)
        return -1;
    return b;
}

/* neues Handle im aktuellen PSP fuer SFT s; -1 = keins frei */
static int hnd_new(int s)
{
    u16 p = cur_psp();
    unsigned h, n = W(p, 0x32);
    u8 __far *j = jft(p);
    for (h = 0; h < n; h++)
        if (j[h] == 0xFF) {
            j[h] = (u8)s;
            sftab[s].psp = p;
            return (int)h;
        }
    return -1;
}

/* Geraet als SFT */
static int sft_dev(int d, unsigned mode)
{
    int s = sft_new();
    unsigned i;
    if (s < 0)
        return -1;
    sftab[s].mode = mode;
    sftab[s].flags = 0x80 | 0x40 | (d == DV_CON ? 0x13 : d == DV_NUL ? 0x04 :
                                    d == DV_CLOCK ? 0x08 : 0);
    sftab[s].dev = ((u32)kseg << 16) | (u16)&devs[d];
    sftab[s].hnd = (u16)d;
    for (i = 0; i < 8; i++)
        sftab[s].name[i] = devdef[d].name[i];
    for (; i < 11; i++)
        sftab[s].name[i] = ' ';
    sftab[s].psp = cur_psp();
    return s;
}

#define IS_DEV(s)   (sftab[s].flags & 0x80)

/* Name "X:\DIR\NAME.EXT" -> 11 Zeichen FCB-Form */
static void fcbname(const char *p, char *o)
{
    const char *b = p;
    unsigned i, k;
    for (i = 0; p[i]; i++)
        if (p[i] == '\\')
            b = p + i + 1;
    for (k = 0; k < 11; k++)
        o[k] = ' ';
    for (k = 0; *b && *b != '.'; b++)
        if (k < 8)
            o[k++] = *b;
    if (*b == '.')
        for (b++, k = 8; *b; b++)
            if (k < 11)
                o[k++] = *b;
}

/* Datei oder Geraet oeffnen -> SFT oder -Fehler; res = Aktion (6Ch) */
int open_path(const char *path, unsigned mode, unsigned act, unsigned attr, unsigned *res)
{
    int d = dev_of(path), s;
    unsigned i;
    if (d >= 0) {
        if ((s = sft_dev(d, mode & 0x7F)) < 0)
            return -E_NOHANDLES;
        if (res)
            *res = 1;
        return s;
    }
    if ((s = sft_new()) < 0)
        return -E_NOHANDLES;
    for (i = 0; path[i] && i < 127; i++)
        q.path[i] = path[i];
    q.path[i] = 0;
    q.mode = mode;
    q.act = act;
    q.attr = attr;
    if (dem(DEM_OPEN)) {
        sftab[s].refs = 0;
        return -(int)q.err;
    }
    sftab[s].mode = mode & 0x7F;
    sftab[s].attr = (u8)q.attr;
    sftab[s].flags = (u16)(path[0] - 'A') | (q.act == 1 ? 0x40 : 0);
    sftab[s].hnd = (u16)q.handle;
    sftab[s].time = q.time;
    sftab[s].date = q.date;
    sftab[s].size = q.size;
    sftab[s].pos = 0;
    sftab[s].psp = cur_psp();
    fcbname(path, sftab[s].name);
    if (res)
        *res = q.act;
    return s;
}

void sft_close(int s)
{
    struct sft *f = &sftab[s];
    if (!f->refs)
        return;
    if (--f->refs)
        return;
    if (IS_DEV(s))
        return;
    q.handle = f->hnd;
    q.mode = f->mode;
    q.flags = (f->flags & 0x4000) ? DEM_F_TIME : 0;
    q.time = f->time;
    q.date = f->date;
    dem(DEM_CLOSE);
}

/* Handles eines neuen PSP: vom Elternteil erben (ohne Bit 7 im Modus),
   der erste Prozess bekommt CON, CON, CON, AUX, PRN */
void psp_handles(u16 psp, u16 parent)
{
    u8 __far *j;
    unsigned h;
    W(psp, 0x32) = 20;
    W(psp, 0x34) = 0x18;
    W(psp, 0x36) = psp;
    j = jft(psp);
    for (h = 0; h < 20; h++)
        j[h] = 0xFF;
    if (parent) {
        u8 __far *pj = jft(parent);
        unsigned n = W(parent, 0x32);
        for (h = 0; h < 20 && h < n; h++) {
            u8 b = pj[h];
            if (b < NSFT && sftab[b].refs && !(sftab[b].mode & 0x80)) {
                j[h] = b;
                sftab[b].refs++;
            }
        }
    } else {
        int s = sft_dev(DV_CON, 2);
        if (s >= 0) {
            j[0] = j[1] = j[2] = (u8)s;
            sftab[s].refs = 3;
        }
        if ((s = sft_dev(DV_AUX, 2)) >= 0)
            j[3] = (u8)s;
        if ((s = sft_dev(DV_PRN, 1)) >= 0)
            j[4] = (u8)s;
    }
}

void close_all(u16 psp)
{
    u8 __far *j = jft(psp);
    unsigned h, n = W(psp, 0x32);
    for (h = 0; h < n; h++)
        if (j[h] != 0xFF) {
            if (j[h] < NSFT)
                sft_close(j[h]);
            j[h] = 0xFF;
        }
}

/* ---- Lesen und Schreiben ------------------------------------------------ */
unsigned h_read(unsigned h, u16 seg, u16 off, unsigned n, unsigned *err)
{
    int s = hnd_sft(h);
    struct sft *f;
    *err = 0;
    if (s < 0) {
        *err = E_HANDLE;
        return 0;
    }
    f = &sftab[s];
    if ((f->mode & 3) == 1) {
        *err = E_DENIED;
        return 0;
    }
    if (IS_DEV(s))
        return dev_read(f->hnd, f, seg, off, n);
    if (!n)
        return 0;
    q.handle = f->hnd;
    q.pos = f->pos;
    q.size = f->size;
    q.buf = lin(seg, off);
    q.count = n;
    if (dem(DEM_READ)) {
        *err = q.err;
        return 0;
    }
    f->pos = q.pos;
    f->size = q.size;
    return (unsigned)q.count;
}

unsigned h_write(unsigned h, u16 seg, u16 off, unsigned n, unsigned *err)
{
    int s = hnd_sft(h);
    struct sft *f;
    *err = 0;
    if (s < 0) {
        *err = E_HANDLE;
        return 0;
    }
    f = &sftab[s];
    if ((f->mode & 3) == 0 && !IS_DEV(s)) {
        *err = E_DENIED;
        return 0;
    }
    if (IS_DEV(s))
        return dev_write(f->hnd, f, seg, off, n);
    q.handle = f->hnd;
    q.pos = f->pos;
    q.size = f->size;
    q.buf = lin(seg, off);
    q.count = n;                        /* 0 = Groesse setzen            */
    if (dem(DEM_WRITE)) {
        *err = q.err;
        return 0;
    }
    f->pos = q.pos;
    f->size = q.size;
    f->flags &= ~0x40;                  /* geaendert                     */
    return (unsigned)q.count;
}

/* ---- Suche (4Eh/4Fh) ---------------------------------------------------- */
static void find_out(u16 seg, u16 off)
{
    unsigned i, k = 0;
    for (i = 0; i < 21; i++)
        B(seg, off + i) = q.find[i];
    B(seg, off + 0x15) = (u8)q.attr;
    W(seg, off + 0x16) = q.time;
    W(seg, off + 0x18) = q.date;
    D(seg, off + 0x1A) = q.size;
    for (i = 0; i < 8 && q.name[i] != ' '; i++)
        B(seg, off + 0x1E + k++) = q.name[i];
    if (q.name[8] != ' ') {
        B(seg, off + 0x1E + k++) = '.';
        for (i = 8; i < 11 && q.name[i] != ' '; i++)
            B(seg, off + 0x1E + k++) = q.name[i];
    }
    B(seg, off + 0x1E + k) = 0;
}

/* ---- 29h: Dateinamen in ein FCB ---------------------------------------- */
static int sepc(u8 c)
{
    return c == ':' || c == '.' || c == ';' || c == ',' || c == '=' || c == '+' ||
           c == ' ' || c == '\t' || c == '/' || c == '"' || c == '[' || c == ']' ||
           c == '<' || c == '>' || c == '|' || c < ' ';
}

static void parse_fn(struct frame __far *f)
{
    u8 __far *s = FP(f->ds, f->si);
    u8 __far *o = FP(f->es, f->di);
    unsigned fl = AL(f), i, wild = 0;
    if (fl & 1) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == ':' || *s == ';' || *s == ',' || *s == '=' || *s == '+')
            s++;
    }
    while (*s == ' ' || *s == '\t')
        s++;
    if (!(fl & 2))
        o[0] = 0;
    AL(f) = 0;
    if (s[0] && s[1] == ':') {
        unsigned d = (unsigned)(upc((char)s[0]) - 'A');
        o[0] = (u8)(d + 1);
        if (!drive_ok(d))
            AL(f) = 0xFF;
        s += 2;
    }
    if (!(fl & 4))
        for (i = 1; i < 9; i++)
            o[i] = ' ';
    if (!(fl & 8))
        for (i = 9; i < 12; i++)
            o[i] = ' ';
    if (!sepc(*s) || *s == '.') {
        if (*s != '.')
            for (i = 1; i < 9; i++)
                o[i] = ' ';
        for (i = 1; !sepc(*s); s++) {
            if (*s == '*') {
                for (; i < 9; i++)
                    o[i] = '?';
                wild = 1;
                continue;
            }
            if (*s == '?')
                wild = 1;
            if (i < 9)
                o[i++] = (u8)upc((char)*s);
        }
        if (*s == '.') {
            s++;
            for (i = 9; i < 12; i++)
                o[i] = ' ';
            for (i = 9; !sepc(*s); s++) {
                if (*s == '*') {
                    for (; i < 12; i++)
                        o[i] = '?';
                    wild = 1;
                    continue;
                }
                if (*s == '?')
                    wild = 1;
                if (i < 12)
                    o[i++] = (u8)upc((char)*s);
            }
        }
    }
    if (wild && AL(f) != 0xFF)
        AL(f) = 1;
    f->si = FP_OFF(s);
}

/* ---- Laufwerksinfo vom DEM (32 Byte, Aufbau in dem.c d_dinfo) ----------- */
static u8 dbuf[32];

static int dinfo(unsigned d)
{
    q.drive = d;
    q.buf = lin(kseg, (u16)dbuf);
    return dem(DEM_DINFO);
}

/* Info-Stufe, Seriennummer, Bezeichnung, Dateisystem (0866h, 6900h) */
static void serial_out(u16 seg, u16 off)
{
    unsigned i;
    W(seg, off) = 0;
    for (i = 0; i < 23; i++)
        B(seg, off + 2 + i) = dbuf[8 + i];
}

/* Geraeteparameter mit BPB (0860h), wie BOXSHELL */
static void params_out(u16 seg, u16 off)
{
    u32 cl = *(u32 *)(dbuf + 4), sec;
    u8 spc = dbuf[1], m;
    B(seg, off + 1) = dbuf[0] == 2 ? 7 : 5;
    W(seg, off + 2) = dbuf[0] == 2 ? 0 : 1;
    W(seg, off + 4) = 1024;
    B(seg, off + 6) = 0;
    W(seg, off + 7) = *(u16 *)(dbuf + 2);
    B(seg, off + 9) = spc;
    W(seg, off + 10) = 1;
    B(seg, off + 12) = 2;
    W(seg, off + 13) = 512;
    W(seg, off + 15) = 0;
    B(seg, off + 17) = dbuf[31];
    W(seg, off + 18) = (u16)(cl >> 8) + 1;
    W(seg, off + 20) = dbuf[0] == 2 ? 18 : 63;     /* Diskette: 1,44 MB */
    W(seg, off + 22) = dbuf[0] == 2 ? 2 : 16;
    if (dbuf[0] == 2)
        W(seg, off + 4) = 80;
    D(seg, off + 24) = 0;
    for (sec = cl, m = spc; m > 1; m >>= 1)
        sec <<= 1;
    D(seg, off + 28) = sec;
}

/* ---- DPB (Liste ab LoL+0, 1Fh/32h) wie MS-DOS 4.0 (DPB.INC, 21h Byte) -- */
#define DPBSZ 0x21
static u8 dpbs[NDRV][DPBSZ];
static u16 first_dpb = 0xFFFF;

void dpb_init(void)
{
    unsigned d, prev = 0xFFFF;
    for (d = NDRV; d-- > 0; ) {
        u8 *p = dpbs[d];
        if (!drive_ok(d))
            continue;
        p[0] = (u8)d;
        p[1] = (u8)d;
        *(u16 *)(p + 2) = 512;
        p[4] = 0;                       /* Sektoren je Cluster - 1      */
        p[5] = 0;
        *(u16 *)(p + 6) = 1;
        p[8] = 2;
        *(u16 *)(p + 9) = 512;
        *(u16 *)(p + 0x0B) = 64;
        *(u16 *)(p + 0x0D) = 0xFFF0;
        *(u16 *)(p + 0x0F) = 256;
        *(u16 *)(p + 0x11) = 32;
        *(u16 *)(p + 0x13) = (u16)&devs[DV_BLK];
        *(u16 *)(p + 0x15) = kseg;
        p[0x17] = d < 2 ? 0xF0 : 0xF8;
        p[0x18] = 0xFF;                 /* noch nicht benutzt            */
        *(u16 *)(p + 0x19) = prev;
        *(u16 *)(p + 0x1B) = prev == 0xFFFF ? 0xFFFF : kseg;
        *(u16 *)(p + 0x1D) = 0;
        *(u16 *)(p + 0x1F) = 0xFFFF;    /* freie Cluster unbekannt       */
        prev = (u16)p;
    }
    first_dpb = prev;
}

u16 dpb_first(void) { return first_dpb; }

/* DPB des Laufwerks d mit Werten vom DEM */
static u8 *dpb_of(unsigned d)
{
    u8 *p = dpbs[d];
    u8 s, sh;
    if (!drive_ok(d) || p[2] == 0 && p[3] == 0)
        return 0;
    q.drive = d;
    q.buf = lin(kseg, (u16)dbuf);
    if (!dem(DEM_DINFO)) {
        s = dbuf[1] ? dbuf[1] : 1;
        for (sh = 0; (1 << sh) < s; sh++)
            ;
        *(u16 *)(p + 2) = *(u16 *)(dbuf + 2);
        p[4] = (u8)(s - 1);
        p[5] = sh;
        *(u16 *)(p + 0x0D) = (u16)(*(u32 *)(dbuf + 4) > 0xFFEEUL ? 0xFFEF :
                                    *(u32 *)(dbuf + 4) + 1);
        p[0x17] = dbuf[31];
    }
    p[0x18] = 0;
    return p;
}

/* ---- 44h IOCTL ---------------------------------------------------------- */
static void ioctl_fn(struct frame __far *f)
{
    int s;
    unsigned d;
    switch (AL(f)) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x06: case 0x07:
    case 0x0A: case 0x0C: case 0x10:
        if ((s = hnd_sft(f->bx)) < 0) {
            set_err(f, E_HANDLE);
            return;
        }
        switch (AL(f)) {
        case 0x00:
            f->dx = sftab[s].flags & 0xFF;
            if (IS_DEV(s))
                f->dx |= devs[sftab[s].hnd].attr & 0xFF00;
            break;
        case 0x01:
            if (IS_DEV(s))
                sftab[s].flags = (sftab[s].flags & ~0x20) | (DL(f) & 0x20);
            break;
        case 0x06:
            if (IS_DEV(s))
                AL(f) = sftab[s].hnd != DV_CON || con_ready() ? 0xFF : 0;
            else
                AL(f) = sftab[s].pos < sftab[s].size ? 0xFF : 0;
            break;
        case 0x07:
            AL(f) = 0xFF;
            break;
        case 0x0A:                      /* Handle fern? lokal: Flags     */
            f->dx = IS_DEV(s) ? devs[sftab[s].hnd].attr : sftab[s].flags;
            break;
        case 0x02:                      /* EMMXXXX0 u. a.: nichts        */
        case 0x03:
            if (!IS_DEV(s) || !(devs[sftab[s].hnd].attr & 0x4000)) {
                set_err(f, E_FUNC);
                return;
            }
            f->ax = 0;
            break;
        default:
            set_err(f, E_FUNC);
        }
        break;
    case 0x08: case 0x09: case 0x0D: case 0x0E: case 0x0F: case 0x11:
        d = BL(f) ? BL(f) - 1 : cur_drv;
        if (!drive_ok(d)) {
            set_err(f, E_DRIVE);
            return;
        }
        if (AL(f) == 0x0E || AL(f) == 0x0F) {
            /* nur ein Buchstabe; B: ohne eigenes Laufwerk ist das
               Phantom von A: (440Eh meldet dann den aktuellen: A:) */
            AL(f) = d == 1 && !(drives_lo & 2) ? 1 : 0;
            break;
        }
        if (AL(f) == 0x09) {            /* lokal, Attribute wie MS-DOS;  */
            struct rregs r;             /* CD fern wie unter MSCDEX      */
            r.ax = 0x200B;
            r.cx = d;
            bint(0xE0, &r);
            f->dx = r.ax == 0x5AD8 ? 0x1000 : 0x08C2;
            break;
        }
        if (dinfo(d)) {                 /* CD, kein Medium               */
            if (d < 2) {                /* leere Diskette: Laufwerk wie  */
                dbuf[0] = 2;            /* MS-DOS ohne Medium (1,44 MB)  */
                dbuf[1] = 1;
                *(u16 *)(dbuf + 2) = 512;
                *(u32 *)(dbuf + 4) = 2847;
                dbuf[31] = 0xF0;
                if (AL(f) == 0x08)
                    f->ax = 0;
                else if (AL(f) == 0x0D && CH(f) == 0x08 && CL(f) == 0x60)
                    params_out(f->ds, f->dx);
                else
                    set_err(f, 0x15);
                break;
            }
            if (AL(f) == 0x08)
                f->ax = 1;
            else
                set_err(f, AL(f) == 0x0D ? E_FUNC : 0x15);
            break;
        }
        if (AL(f) == 0x08)
            f->ax = dbuf[0] == 2 ? 0 : 1;
        else if (AL(f) == 0x0D && CH(f) == 0x08 && CL(f) == 0x66)
            serial_out(f->ds, f->dx);
        else if (AL(f) == 0x0D && CH(f) == 0x08 && CL(f) == 0x60)
            params_out(f->ds, f->dx);
        else if (AL(f) == 0x0D && CH(f) == 0x08 && CL(f) == 0x46)
            set_err(f, E_DENIED);
        else
            set_err(f, E_FUNC);
        break;
    case 0x0B:
        break;
    default:
        set_err(f, E_FUNC);
    }
}

/* ---- Verteiler fuer Datei- und Verzeichnisaufrufe ----------------------- */
static char p1[128], p2[128];

static void cpy(char *d, const char *s)
{
    while ((*d++ = *s++) != 0)
        ;
}

static int path_arg(struct frame __far *f, u16 seg, u16 off, char *out)
{
    unsigned e = canon(FP(seg, off), out, 0);
    if (e) {
        set_err(f, e);
        return 0;
    }
    return 1;
}

static void do_open(struct frame __far *f, const char *path, unsigned mode,
                    unsigned act, unsigned attr, int ext)
{
    unsigned res;
    int s = open_path(path, mode, act, attr, &res), h;
    if (s < 0) {
        set_err(f, (unsigned)-s);
        return;
    }
    if ((h = hnd_new(s)) < 0) {
        sft_close(s);
        set_err(f, E_NOHANDLES);
        return;
    }
    f->ax = (u16)h;
    if (ext)
        f->cx = res;
}

void file_fn(struct frame __far *f)
{
    unsigned e, n, d;
    int s, h;
    switch (AH(f)) {
    case 0x36:                          /* freier Platz                  */
        d = DL(f) ? DL(f) - 1 : cur_drv;
        if (!drive_ok(d)) {
            f->ax = 0xFFFF;
            return;
        }
        q.drive = d;
        if (dem(DEM_SPACE)) {
            f->ax = 0xFFFF;
            return;
        }
        f->ax = (u16)q.count;
        f->bx = (u16)q.pos;
        f->cx = q.attr;
        f->dx = (u16)q.size;
        return;
    case 0x1B: case 0x1C:
        d = AH(f) == 0x1C && DL(f) ? DL(f) - 1 : cur_drv;
        if (!drive_ok(d) || (q.drive = d, dem(DEM_SPACE))) {
            AL(f) = 0xFF;
            return;
        }
        AL(f) = (u8)q.count;
        f->cx = q.attr;
        f->dx = (u16)q.size;
        f->ds = kseg;
        f->bx = (u16)&media;
        return;
    case 0x39: case 0x3A: case 0x3B:
        if (!path_arg(f, f->ds, f->dx, p1))
            return;
        cpy(q.path, p1);
        if (AH(f) == 0x3B) {
            if (p1[3] && dem(DEM_CHDIR)) {
                set_err(f, E_NOPATH);
                return;
            }
            d = (unsigned)(p1[0] - 'A');
            for (n = 0; p1[n] && n < 66; n++)
                cdstab[d].path[n] = p1[n];
            cdstab[d].path[n] = 0;
            return;
        }
        if (AH(f) == 0x3A) {            /* aktuelles Verzeichnis: 10h   */
            d = (unsigned)(p1[0] - 'A');
            for (n = 0; p1[n] && p1[n] == cdstab[d].path[n]; n++)
                ;
            if (!p1[n] && !cdstab[d].path[n]) {
                set_err(f, E_CURDIR);
                return;
            }
        }
        if (dem(AH(f) == 0x39 ? DEM_MKDIR : DEM_RMDIR))
            set_err(f, q.err);
        return;
    case 0x47:                          /* aktuelles Verzeichnis         */
        d = DL(f) ? DL(f) - 1 : cur_drv;
        if (!drive_ok(d)) {
            set_err(f, E_DRIVE);
            return;
        }
        for (n = 0; cdstab[d].path[3 + n]; n++)
            B(f->ds, f->si + n) = (u8)cdstab[d].path[3 + n];
        B(f->ds, f->si + n) = 0;
        f->ax = 0x0100;
        return;
    case 0x3C: case 0x5B: case 0x5A:
        if (AH(f) == 0x5A) {            /* Name aus der Uhr bilden       */
            u8 __far *p = FP(f->ds, f->dx);
            struct rregs r;
            unsigned k, i, l;
            for (l = 0; p[l]; l++)
                ;
            if (l && p[l - 1] != '\\' && p[l - 1] != ':')
                p[l++] = '\\';
            for (k = 0; k < 100; k++) {
                u16 v;
                r.ax = 0;
                bint(0x1A, &r);
                v = r.dx + k * 7919u;
                for (i = 0; i < 8; i++, v = (u16)((v >> 4) | (v << 12)))
                    p[l + i] = (u8)("0123456789ABCDEF"[v & 15]);
                p[l + 8] = 0;
                if (!path_arg(f, f->ds, f->dx, p1))
                    return;
                do_open(f, p1, 2, DEM_ACT_NEW, CL(f), 0);
                if (!(f->fl & FL_CF) || f->ax != 0x50)
                    return;
                f->fl &= ~FL_CF;
            }
            return;
        }
        if (!path_arg(f, f->ds, f->dx, p1))
            return;
        do_open(f, p1, 2, AH(f) == 0x3C ? DEM_ACT_CREATE : DEM_ACT_NEW, CL(f), 0);
        return;
    case 0x3D:
        if (!path_arg(f, f->ds, f->dx, p1))
            return;
        do_open(f, p1, AL(f), DEM_ACT_OPEN, 0, 0);
        return;
    case 0x6C:
        if (!path_arg(f, f->ds, f->si, p1))
            return;
        do_open(f, p1, f->bx & 0xFF, DL(f), f->cx, 1);
        return;
    case 0x3E:
        if ((s = hnd_sft(f->bx)) < 0) {
            set_err(f, E_HANDLE);
            return;
        }
        jft(cur_psp())[f->bx] = 0xFF;
        sft_close(s);
        return;
    case 0x3F:
        n = h_read(f->bx, f->ds, f->dx, f->cx, &e);
        if (e)
            set_err(f, e);
        else
            f->ax = n;
        return;
    case 0x40:
        n = h_write(f->bx, f->ds, f->dx, f->cx, &e);
        if (e)
            set_err(f, e);
        else
            f->ax = n;
        return;
    case 0x41:
        if (!path_arg(f, f->ds, f->dx, p1))
            return;
        for (n = 0; p1[n] && p1[n] != '*' && p1[n] != '?'; n++)
            ;
        if (p1[n]) {                    /* Platzhalter nur ueber 5D00h  */
            set_err(f, E_NOPATH);
            return;
        }
        cpy(q.path, p1);
        if (dem(DEM_DELETE))
            set_err(f, q.err);
        return;
    case 0x42: {
        u32 p;
        if ((s = hnd_sft(f->bx)) < 0) {
            set_err(f, E_HANDLE);
            return;
        }
        p = ((u32)f->cx << 16) | f->dx;
        if (AL(f) == 1)
            p += sftab[s].pos;
        else if (AL(f) == 2) {
            if (!IS_DEV(s)) {           /* Groesse frisch (andere Handles) */
                q.handle = sftab[s].hnd;
                q.size = sftab[s].size;
                if (!dem(DEM_SIZE))
                    sftab[s].size = q.size;
            }
            p += sftab[s].size;
        } else if (AL(f) != 0) {
            set_err(f, E_FUNC);
            return;
        }
        sftab[s].pos = IS_DEV(s) ? 0 : p;
        f->ax = (u16)sftab[s].pos;
        f->dx = (u16)(sftab[s].pos >> 16);
        return;
    }
    case 0x43:
        if (!path_arg(f, f->ds, f->dx, p1))
            return;
        if (dev_of(p1) >= 0) {
            f->cx = 0;
            return;
        }
        cpy(q.path, p1);
        if (AL(f) == 0) {
            if (dem(DEM_GETATTR))
                set_err(f, q.err);
            else
                f->cx = q.attr;
        } else if (AL(f) == 1) {
            q.attr = f->cx;
            if (dem(DEM_SETATTR))
                set_err(f, q.err);
        } else
            set_err(f, E_FUNC);
        return;
    case 0x44:
        ioctl_fn(f);
        return;
    case 0x45:
        if ((s = hnd_sft(f->bx)) < 0) {
            set_err(f, E_HANDLE);
            return;
        }
        if ((h = hnd_new(s)) < 0) {
            set_err(f, E_NOHANDLES);
            return;
        }
        sftab[s].refs++;
        f->ax = (u16)h;
        return;
    case 0x46:
        if ((s = hnd_sft(f->bx)) < 0 || f->cx >= W(cur_psp(), 0x32)) {
            set_err(f, E_HANDLE);
            return;
        }
        if ((h = hnd_sft(f->cx)) >= 0)
            sft_close(h);
        jft(cur_psp())[f->cx] = (u8)s;
        sftab[s].refs++;
        return;
    case 0x4E:
        if (!path_arg(f, f->ds, f->dx, p1))
            return;
        cpy(q.path, p1);
        q.attr = f->cx;
        if (dem(DEM_FINDFIRST)) {
            set_err(f, q.err);
            return;
        }
        find_out(dta_seg, dta_off);
        return;
    case 0x4F:
        for (n = 0; n < 21; n++)
            q.find[n] = B(dta_seg, dta_off + n);
        if (dem(DEM_FINDNEXT)) {
            set_err(f, q.err);
            return;
        }
        find_out(dta_seg, dta_off);
        return;
    case 0x56:
        if (!path_arg(f, f->ds, f->dx, p1) || !path_arg(f, f->es, f->di, p2))
            return;
        cpy(q.path, p1);
        cpy(q.path2, p2);
        if (dem(DEM_RENAME))
            set_err(f, q.err);
        return;
    case 0x57:
        if ((s = hnd_sft(f->bx)) < 0) {
            set_err(f, E_HANDLE);
            return;
        }
        if (AL(f) == 0) {
            f->cx = sftab[s].time;
            f->dx = sftab[s].date;
        } else if (AL(f) == 1) {
            sftab[s].time = f->cx;
            sftab[s].date = f->dx;
            sftab[s].flags |= 0x4000;
        } else if (AL(f) == 2 || AL(f) == 3) {
            /* erweiterte Attribute (DOS 4.0): wie dessen EXTATTR.ASM eine
               leere Liste - COMMAND.COM 4.0 fragt so die Codepage einer
               Datei (TYPE, COPY) */
            if (f->cx == 1) {
                set_err(f, E_NOMEM);
                return;
            }
            if (f->cx)
                W(f->es, f->di) = 0;
            f->cx = 2;
        } else if (AL(f) != 4)          /* setzen: ohne Wirkung          */
            set_err(f, E_FUNC);
        return;
    case 0x5C:
        if ((s = hnd_sft(f->bx)) < 0) {
            set_err(f, E_HANDLE);
            return;
        }
        if (IS_DEV(s))
            return;
        q.handle = sftab[s].hnd;
        q.pos = ((u32)f->cx << 16) | f->dx;
        q.count = ((u32)f->si << 16) | f->di;
        q.flags = AL(f) ? DEM_F_UNLOCK : 0;
        if (dem(DEM_LOCK))
            set_err(f, q.err);
        return;
    case 0x60:
        if (!path_arg(f, f->ds, f->si, p1))
            return;
        for (n = 0; p1[n]; n++)
            B(f->es, f->di + n) = (u8)p1[n];
        B(f->es, f->di + n) = 0;
        return;
    case 0x67: {                        /* Zahl der Handles              */
        u16 p = cur_psp(), seg;
        unsigned k, old = W(p, 0x32);
        u8 __far *o = jft(p);
        n = f->bx;
        if (n <= 20) {
            if (n < old) {
                for (k = n; k < old; k++)
                    if (o[k] != 0xFF) {
                        set_err(f, E_NOHANDLES);
                        return;
                    }
            }
            if (W(p, 0x36) != p || W(p, 0x34) != 0x18) {
                for (k = 0; k < 20; k++)
                    B(p, 0x18 + k) = k < old ? o[k] : 0xFF;
                W(p, 0x34) = 0x18;
                W(p, 0x36) = p;
            }
            W(p, 0x32) = n < old ? old : n;
            return;
        }
        if (n <= old)
            return;
        seg = mem_alloc((n + 15) >> 4, p, 0);
        if (!seg) {
            set_err(f, E_NOMEM);
            return;
        }
        for (k = 0; k < n; k++)
            B(seg, k) = k < old ? o[k] : 0xFF;
        W(p, 0x32) = n;
        W(p, 0x34) = 0;
        W(p, 0x36) = seg;
        return;
    }
    case 0x29:
        parse_fn(f);
        return;
    case 0x69:                          /* Seriennummer 00 lesen, 01 setzen */
        d = BL(f) ? BL(f) - 1 : cur_drv;
        if (!drive_ok(d) || dinfo(d)) {
            set_err(f, E_DRIVE);
            return;
        }
        if (AL(f) == 0) {
            serial_out(f->ds, f->dx);
            f->ax = 0;
        } else
            set_err(f, E_DENIED);
        return;
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x27: case 0x28:
        fcb_fn(f);
        return;
    case 0x32: case 0x1F: {
        u8 *p;
        d = AH(f) == 0x32 && DL(f) ? DL(f) - 1 : cur_drv;
        if (d >= NDRV || (p = dpb_of(d)) == 0) {
            AL(f) = 0xFF;
            return;
        }
        f->ds = kseg;
        f->bx = (u16)p;
        AL(f) = 0;
        return;
    }
    default:
        set_err(f, E_FUNC);
        AL(f) = 0;
        return;
    }
}

