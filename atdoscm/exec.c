/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - Programme laden (4Bh), beenden (00h/4Ch/31h, INT 20h/27h),
 * Strg+C (INT 23h) und der erste Prozess (BOXSHELL).
 *
 * PSP wie MS-DOS 4.0 (PDB.INC); .COM ab PSP:100h, .EXE mit Verschiebungen.
 * Das Elternprogramm wartet mit seinem INT-21h-Rahmen auf dem eigenen
 * Stapel; dessen Adresse steht in seinem PSP bei 2Eh, beim Ende des Kindes
 * kehrt der Verteiler ueber ihn zurueck. Vorlage fuer den Ablauf: FreeDOS
 * task.c (nur gelesen), eigene Umsetzung.
 */
#include "cm.h"

u16 ret_code;
extern u16 setver_dx;
extern unsigned char cc_hit;
extern u16 in_ax;
extern char int22_entry[];
extern char ctrlc_stub[];

static u32 lin(u16 seg, u16 off)
{
    return ((u32)seg << 4) + off;
}

/* aus der Datei (SFT s) ab pos lesen */
static unsigned sread(int s, u32 pos, u16 seg, u16 off, unsigned n)
{
    q.handle = sftab[s].hnd;
    q.pos = pos;
    q.size = sftab[s].size;
    q.buf = lin(seg, off);
    q.count = n;
    if (dem(DEM_READ))
        return 0;
    return (unsigned)q.count;
}

/* n Bytes ab pos nach seg:0 (ueber 64 KB hinweg) */
static int sread_big(int s, u32 pos, u16 seg, u32 n)
{
    while (n) {
        unsigned k = n > 0xFE00UL ? 0xFE00 : (unsigned)n;
        if (sread(s, pos, seg, 0, k) != k)
            return 0;
        pos += k;
        n -= k;
        seg += k >> 4;
    }
    return 1;
}

/* Version fuer das Programm: 122Fh, sonst die SETVER-Liste (LoL+37h) */
static u16 prog_ver(const char *path)
{
    const char *b = path;
    u8 __far *t;
    u8 *l = lol_ptr();
    unsigned i, n;
    if (setver_dx)
        return setver_dx;
    for (i = 0; path[i]; i++)
        if (path[i] == '\\')
            b = path + i + 1;
    t = (u8 __far *)MK_FP(*(u16 *)(l + 0x39), *(u16 *)(l + 0x37));
    if (!*(u16 *)(l + 0x39))
        return 0;
    while ((n = *t) != 0 && n < 13) {
        for (i = 0; i < n && t[1 + i] == (u8)b[i]; i++)
            ;
        if (i == n && !b[n])
            return (u16)t[1 + n] | ((u16)t[2 + n] << 8);
        t += n + 3;
    }
    return 0;
}

/* Umgebung kopieren (+ Wort 1 + Programmpfad) -> Segment oder 0 */
static u16 env_copy(u16 src, const char *path, u16 owner)
{
    unsigned len = 0, pl, i;
    u16 seg;
    if (src) {
        while (len < 0x7FF0 && (B(src, len) || B(src, len + 1)))
            len++;
        len += 2;
    } else
        len = 1;
    for (pl = 0; path[pl]; pl++)
        ;
    seg = mem_alloc((len + 3 + pl + 15) >> 4, owner, 0);
    if (!seg)
        return 0;
    if (src)
        for (i = 0; i < len; i++)
            B(seg, i) = B(src, i);
    else {
        B(seg, 0) = 0;
        B(seg, 1) = 0;
        len = 2;
    }
    W(seg, len) = 1;
    for (i = 0; i <= pl; i++)
        B(seg, len + 2 + i) = (u8)path[i];
    return seg;
}

static void mcb_name(u16 seg, const char *path)
{
    const char *b = path;
    unsigned i;
    for (i = 0; path[i]; i++)
        if (path[i] == '\\')
            b = path + i + 1;
    for (i = 0; i < 8 && b[i] && b[i] != '.'; i++)
        B(seg - 1, 8 + i) = (u8)b[i];
    for (; i < 8; i++)
        B(seg - 1, 8 + i) = 0;
}

static u16 root_psp;                    /* erster Prozess (BOXSHELL)     */

/* PSP anlegen */
static void psp_make(u16 psp, u16 top, u16 parent, u16 env, u16 rip, u16 rcs)
{
    unsigned i;
    for (i = 0; i < 0x100; i++)
        B(psp, i) = 0;
    W(psp, 0x00) = 0x20CD;
    W(psp, 0x02) = top;
    B(psp, 0x05) = 0xC3;                /* CP/M-Einsprung: nicht da      */
    W(psp, 0x0A) = rip;
    W(psp, 0x0C) = rcs;
    D(psp, 0x0E) = D(0, 0x23 * 4);
    D(psp, 0x12) = D(0, 0x24 * 4);
    W(psp, 0x16) = parent ? parent : psp;
    if (!parent && !root_psp)
        root_psp = psp;
    W(psp, 0x2C) = env;
    D(psp, 0x38) = 0xFFFFFFFFUL;
    W(psp, 0x50) = 0x21CD;
    B(psp, 0x52) = 0xCB;
    for (i = 0; i < 11; i++) {
        B(psp, 0x5C + 1 + i) = ' ';
        B(psp, 0x6C + 1 + i) = ' ';
    }
    B(psp, 0x81) = 13;
    psp_handles(psp, parent);
}

struct ld {
    u16 psp, cs, ip, ss, sp, ax;
};

static u8  h[0x1C];
static u16 rel[64];

/* Verschiebungen der .EXE: Segment lseg, Faktor base */
static unsigned relocs(int s, u16 lseg, u16 base)
{
    u16 nrel = *(u16 *)(h + 6), k, m;
    u32 rp = *(u16 *)(h + 0x18);
    while (nrel) {
        m = nrel > 32 ? 32 : nrel;
        if (sread(s, rp, kseg, (u16)rel, m * 4) != m * 4)
            return E_FORMAT;
        for (k = 0; k < m; k++)
            W(lseg + rel[2 * k + 1], rel[2 * k]) += base;
        rp += m * 4;
        nrel -= m;
    }
    return 0;
}

/* Programm path laden. mode 0/1: neuer Prozess (Parameterblock pseg:poff,
   pseg 0 = keiner), mode 3: Overlay. -> 0 oder Fehler */
static unsigned load(const char *path, unsigned mode, u16 pseg, u16 poff,
                     u16 parent, u16 rip, u16 rcs, struct ld *o)
{
    unsigned res, e = 0, k;
    int s, exe;
    u32 isz;
    u16 hdr = 0, env = 0, psp = 0, big, need, want, i, ld;
    s = open_path(path, 0x20, DEM_ACT_OPEN, 0, &res);
    if (s < 0)
        return (unsigned)-s;
    if (sftab[s].flags & 0x80) {
        sft_close(s);
        return E_NOFILE;
    }
    for (i = 0; i < 0x1C; i++)
        h[i] = 0;
    sread(s, 0, kseg, (u16)h, 0x1C);
    exe = (h[0] == 'M' && h[1] == 'Z') || (h[0] == 'Z' && h[1] == 'M');
    if (exe) {
        u16 pages = *(u16 *)(h + 4), last = *(u16 *)(h + 2);
        u32 all = ((u32)pages << 9) - (last ? 512 - last : 0);
        hdr = *(u16 *)(h + 8);
        isz = all > ((u32)hdr << 4) ? all - ((u32)hdr << 4) : 0;
        if (isz > sftab[s].size)
            isz = sftab[s].size - ((u32)hdr << 4);
    } else {
        isz = sftab[s].size;
        if (isz > 0xFF00UL) {
            e = E_FORMAT;
            goto out;
        }
    }

    if (mode == 3) {                    /* Overlay: an die Stelle des Aufrufers */
        u16 lseg = W(pseg, poff), fac = W(pseg, poff + 2);
        if (!sread_big(s, (u32)hdr << 4, lseg, isz))
            e = E_FORMAT;
        else if (exe)
            e = relocs(s, lseg, fac);
        goto out;
    }

    /* Umgebung */
    {
        u16 src = pseg ? W(pseg, poff) : 0;
        if (!src && parent)
            src = W(parent, 0x2C);
        env = env_copy(src, path, 8);   /* Besitzer gleich: das Kind */
        if (!env) {
            e = E_NOMEM;
            goto out;
        }
    }
    /* Speicher */
    if (exe) {
        u32 n = ((isz + 15) >> 4) + 0x10 + *(u16 *)(h + 0x0A);
        u32 w = ((isz + 15) >> 4) + 0x10 + (u32)*(u16 *)(h + 0x0C);
        need = n > 0xFFFFUL ? 0xFFFF : (u16)n;
        want = w > 0xFFFFUL ? 0xFFFF : (u16)w;
        if (*(u16 *)(h + 0x0C) == 0)
            want = 0xFFFF;
        if (want < need)
            want = need;
    } else {
        need = (u16)(((isz + 15) >> 4) + 0x10 + 0x10);  /* + Stapel     */
        want = 0xFFFF;
    }
    psp = mem_alloc_prog(need, want, 1, &big);
    if (!psp) {
        e = E_NOMEM;
        goto out;
    }
    W(psp - 1, 1) = psp;                /* Besitzer = das Kind selbst    */
    W(env - 1, 1) = psp;
    mcb_name(psp, path);
    psp_make(psp, psp + W(psp - 1, 3), parent, env, rip, rcs);
    W(psp, 0x40) = prog_ver(path);

    ld = psp + 0x10;                    /* minalloc = maxalloc = 0: oben  */
    if (exe && !*(u16 *)(h + 0x0A) && !*(u16 *)(h + 0x0C))  /* im Block wie */
        ld = psp + W(psp - 1, 3) - (u16)((isz + 15) >> 4);  /* MS-DOS (EXEC */
                                        /* .ASM; KRNL286 von Windows 3.1) */
    if (!sread_big(s, (u32)hdr << 4, ld, isz)) {
        e = E_FORMAT;
        goto freeit;
    }
    if (exe) {
        if ((e = relocs(s, ld, ld)) != 0)
            goto freeit;
        o->cs = ld + *(u16 *)(h + 0x16);
        o->ip = *(u16 *)(h + 0x14);
        o->ss = ld + *(u16 *)(h + 0x0E);
        o->sp = *(u16 *)(h + 0x10);
    } else {
        u16 sz = W(psp - 1, 3);
        o->cs = o->ss = psp;
        o->ip = 0x100;
        o->sp = sz >= 0x1000 ? 0xFFFE : (u16)(sz << 4) - 2;
        W(psp, o->sp) = 0;              /* RET -> PSP:0 = INT 20h        */
    }
    o->psp = psp;
    o->ax = 0;
    if (pseg) {                         /* Befehlszeile und FCBs          */
        u16 ts = W(pseg, poff + 4), to = W(pseg, poff + 2);
        u8 n = B(ts, to);
        if (n > 126)
            n = 126;
        for (i = 0; i <= n; i++)
            B(psp, 0x80 + i) = B(ts, to + 1 + i - 1);
        B(psp, 0x81 + n) = 13;
        for (k = 0; k < 2; k++) {
            u16 fo = W(pseg, poff + 6 + 4 * k), fs = W(pseg, poff + 8 + 4 * k);
            if (fs || fo) {
                for (i = 0; i < 12; i++)
                    B(psp, 0x5C + 0x10 * k + i) = B(fs, fo + i);
                if (B(psp, 0x5C + 0x10 * k) &&
                    !drive_ok(B(psp, 0x5C + 0x10 * k) - 1))
                    o->ax |= 0xFFu << (8 * k);
            }
        }
    }
    goto out;
freeit:
    close_all(psp);
    mem_free(psp);
out:
    if (e && env)
        mem_free(env);
    sft_close(s);
    return e;
}

/* Rahmen fuer den Start (wie nach INT 21h) unter ss:sp anlegen */
static struct frame __far *mkframe(struct ld *o)
{
    struct frame __far *n = (struct frame __far *)MK_FP(o->ss, o->sp - sizeof(struct frame));
    n->es = n->ds = o->psp;
    n->bp = n->di = n->si = 0;
    n->dx = o->psp;
    n->cx = 0x00FF;
    n->bx = 0;
    n->ax = o->ax;
    n->ip = o->ip;
    n->cs = o->cs;
    n->fl = 0x0202;
    return n;
}

static void set_cur(u16 psp)
{
    extern u8 sda[];
    *(u16 *)(sda + 0x10) = psp;
    dta_seg = psp;
    dta_off = 0x80;
    *(u16 *)(sda + 0x0C) = 0x80;
    *(u16 *)(sda + 0x0E) = psp;
}

static char ep[128];

/* Windows 3.1 im Standardmodus: DOSX.EXE kennt nur VCPI oder den Real
   Mode - statt seiner startet X:\DOS\MDOS\WINLDR.COM den Kern auf dem
   DPMI des Wirts (Weg B, docs/DOSKERN.md). WIN.COM gibt DOSX als erstes
   Wort den Kern ("krnl286.exe /b ..."): davor kommt das Verzeichnis von
   DOSX (ohne Befehlszeile KRNL286.EXE). */
extern u8 sys_drv;
static char wl_tail[130];
static u16 wl_pb[7];

static void dosx_redirect(u16 *pseg, u16 *poff)
{
    static const char kr[] = "KRNL286.EXE", wl[] = ":\\DOS\\MDOS\\WINLDR.COM";
    unsigned n, i, d, k, j;
    u16 ts, to;
    char c;
    for (n = 0; ep[n]; n++)
        ;
    if (n < 9)
        return;
    for (i = 0; i < 9; i++)
        if (ep[n - 9 + i] != "\\DOSX.EXE"[i])
            return;
    to = W(*pseg, *poff + 2);           /* alte Befehlszeile             */
    ts = W(*pseg, *poff + 4);
    k = B(ts, to);
    for (j = 0; j < k && B(ts, to + 1 + j) == ' '; j++)
        ;
    d = 1;
    wl_tail[d++] = ' ';
    for (i = j; i < k && (c = (char)B(ts, to + 1 + i)) != ' '; i++)
        if (c == '\\' || c == ':')    /* Kern mit Pfad: so lassen      */
            break;
    if (i == k || B(ts, to + 1 + i) == ' ')
        for (i = 0; i < n - 8; i++)     /* Verzeichnis mit '\'          */
            wl_tail[d++] = ep[i];
    if (j == k)                         /* ohne Befehlszeile             */
        for (i = 0; kr[i]; i++)
            wl_tail[d++] = kr[i];
    for (i = j; i < k && d < 127; i++)
        wl_tail[d++] = B(ts, to + 1 + i);
    wl_tail[0] = (char)(d - 1);
    wl_tail[d] = 13;
    for (i = 0; i < 7; i++)
        wl_pb[i] = W(*pseg, *poff + 2 * i);
    wl_pb[1] = (u16)wl_tail;
    wl_pb[2] = kseg;
    ep[0] = (char)('A' + sys_drv);
    for (i = 0; wl[i]; i++)
        ep[1 + i] = wl[i];
    ep[1 + i] = 0;
    *pseg = kseg;
    *poff = (u16)wl_pb;
}

struct frame __far *exec_fn(struct frame __far *f)
{
    struct ld o;
    unsigned e, al = AL(f);
    u16 par = cur_psp(), pseg = f->es, poff = f->bx;
    if (al != 0 && al != 1 && al != 3) {
        set_err(f, E_FUNC);
        return f;
    }
    if ((e = canon(FP(f->ds, f->dx), ep, 0)) == 0 && al == 0)
        dosx_redirect(&pseg, &poff);
    if (e != 0 ||
        (e = load(ep, al, pseg, poff, par, f->ip, f->cs, &o)) != 0) {
        set_err(f, e);
        return f;
    }
    setver_dx = 0;
    if (al == 3)
        return f;
    net_sync();                         /* Netz, CDS Bit 15 (dosint.c)   */
    W(par, 0x2E) = FP_OFF(f);           /* Rahmen des Elternteils        */
    W(par, 0x30) = FP_SEG(f);
    set_cur(o.psp);
    if (al == 1) {                      /* nur laden: SS:SP (mit AX), CS:IP */
        o.sp -= 2;
        W(o.ss, o.sp) = o.ax;
        W(f->es, f->bx + 0x0E) = o.sp;
        W(f->es, f->bx + 0x10) = o.ss;
        W(f->es, f->bx + 0x12) = o.ip;
        W(f->es, f->bx + 0x14) = o.cs;
        return f;
    }
    a20_drop = in_hma;                  /* EXEPACK: Programm ohne A20    */
    return mkframe(&o);
}

/* Programmende: type 0 normal, 1 Strg+C, 2 kritischer Fehler, 3 resident */
struct frame __far *term(struct frame __far *f, unsigned type, unsigned rc, u16 keep)
{
    u16 psp = cur_psp(), par = W(psp, 0x16), dummy;
    struct frame __far *r;
    struct rregs rr;
    ret_code = (u16)((type << 8) | (rc & 0xFF));
    if (psp == root_psp || !par) {      /* erster Prozess: Box zu Ende  */
        rr.ax = 0x0100 | (rc & 0xFF);
        bint(0xE0, &rr);
        return f;
    }
    D(0, 0x22 * 4) = D(psp, 0x0A);
    D(0, 0x23 * 4) = D(psp, 0x0E);
    D(0, 0x24 * 4) = D(psp, 0x12);
    if (par == psp) {                   /* sein eigener Elternprozess   */
        f->ip = W(psp, 0x0A);           /* (COMMAND.COM waehrend der    */
        f->cs = W(psp, 0x0C);           /* Arbeit): wie MS-DOS nichts   */
        f->fl &= ~FL_CF;                /* freigeben, weiter an INT 22h */
        return f;                       /* - "Abbrechen" bei DIR A:     */
    }
    if (type == 3) {
        if (keep < 6)
            keep = 6;
        mem_resize(psp, keep, &dummy);
    } else {
        dlock_end(psp);                 /* Laufwerkssperren (440Dh 4Ah)  */
        lfn_end(psp);                   /* LFN-Suchen (714Eh)            */
        close_all(psp);
        mem_free_owner(psp);
    }
    set_cur(par);
    r = (struct frame __far *)MK_FP(W(par, 0x30), W(par, 0x2E));
    r->ip = W(psp, 0x0A);
    r->cs = W(psp, 0x0C);
    r->fl &= ~FL_CF;
    return r;
}

/* Strg+C: Register wie beim Aufruf, ueber ctrlc_stub (entry.asm) INT 23h;
   kehrt der Handler zurueck, wird der Aufruf wiederholt */
struct frame __far *cc_frame(struct frame __far *f)
{
    struct frame __far *n = (struct frame __far *)((u16 __far *)f - 3);
    u16 __far *a = (u16 __far *)f, __far *b = (u16 __far *)n;
    unsigned i;
    for (i = 0; i < 9; i++)
        b[i] = a[i];
    n->ax = in_ax;
    n->ip = (u16)ctrlc_stub;
    n->cs = kseg;
    n->fl = a[11];
    return n;
}

/* erster Prozess: BOXSHELL. Kehrt nur bei Fehlern zurueck (0). */
u16 load_shell(const char *path)
{
    struct ld o;
    if (load(path, 0, 0, 0, 0, (u16)int22_entry, kseg, &o))
        return 0;
    set_cur(o.psp);
    {
        struct frame __far *n = mkframe(&o);
        start_frame(FP_SEG(n), FP_OFF(n));
    }
    return o.psp;
}

/* ---- fuer config.c ------------------------------------------------------ */
/* Datei ganz nach buf (hoechstens max - 1 Byte, Null dahinter) -> Laenge,
   0 = nicht da */
unsigned read_file(const char *path, u16 seg, unsigned max)
{
    unsigned res, n;
    int s = open_path(path, 0x20, DEM_ACT_OPEN, 0, &res);
    if (s < 0)
        return 0;
    n = (sftab[s].flags & 0x80) ? 0 : sread(s, 0, seg, 0, max - 1);
    B(seg, n) = 0;
    sft_close(s);
    return n;
}

/* Groesse einer Datei, 0 = nicht da */
u32 file_size(const char *path)
{
    unsigned res;
    u32 n;
    int s = open_path(path, 0x20, DEM_ACT_OPEN, 0, &res);
    if (s < 0)
        return 0;
    n = (sftab[s].flags & 0x80) ? 0 : sftab[s].size;
    sft_close(s);
    return n;
}

/* Treiberdatei (.SYS roh oder MZ) an seg:0 laden (wie 4B03h) */
unsigned load_ovl(const char *path, u16 seg)
{
    static u16 pb[2];
    struct ld o;
    pb[0] = seg;
    pb[1] = seg;
    return load(path, 3, kseg, (u16)pb, 0, 0, 0, &o);
}
