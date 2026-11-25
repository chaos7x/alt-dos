/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Lader fuer Win16-Programme (NE) im Ring 3.
 *
 * Anders als der NE-Lader des Kernels (kernel/neload.c, Programme mit
 * eigener LDT je Task) laedt dieser in die LDT des GUI-Prozesses: alle
 * Win16-Programme teilen einen Adressraum wie unter Windows. Segmente
 * ueber DosAllocSeg, Codesegmente danach per FAPI_SEGCTL SEG_TOCODE.
 * Importe aus KERNEL, USER, GDI (und weiteren eingebauten Modulen) loest
 * exports.c ueber die Ordinalzahl auf; was fehlt, zeigt auf w16_stub und
 * steht in GUI.LOG.
 *
 * Format nach der veroeffentlichten NE-Beschreibung (Microsoft, "Executable
 * File Header Format"); eigener Code.
 */
#include "gui.h"

struct w16mod wm[NMOD];

static char lpath[128];
static WORD lh;                         /* Handle der Datei beim Laden  */
static unsigned long lpos;
static BYTE nh[64];                     /* NE-Kopf                       */
static WORD tsel;                       /* Kopftabellen (Segment)        */
static WORD tbase;                      /* Dateistelle des NE-Kopfes     */
static BYTE rb[8 * 32];
static WORD nmiss;

#define W(p)    (*(WORD *)(p))
#define TB(o)   (*(BYTE FAR *)MKFP(tsel, (o)))
#define TW(o)   (*(WORD FAR *)MKFP(tsel, (o)))

static int lseek(unsigned long p)
{
    lpos = p;
    return FAPI_FAILED(DosSeek(lh, &lpos));
}
static int lread_near(void *b, WORD n)
{
    return DosRead(lh, b, n) != n;
}
static int lread_sel(WORD sel, WORD off, WORD n)
{
    WORD r;
    while (n) {
        WORD k = n > 0x4000 ? 0x4000 : n;
        r = fapi_sel(FAPI_READ, sel, lh, off, k);
        if (r != k)
            return 1;
        off += k;
        n -= k;
    }
    return 0;
}

/* Eintrag `ord` der Eintragstabelle (Kopie in ent_sel) -> Segment/Offset */
static int ent_lookup(struct w16mod *w, WORD ord, WORD FAR *seg, WORD FAR *off)
{
    BYTE FAR *e = MKFP(w->ent_sel, 0);
    WORD i = 0, n, k = 1;
    BYTE ty;
    while (i < w->ent_len) {
        n = e[i];
        if (!n)
            break;
        ty = e[i + 1];
        i += 2;
        if (ty == 0) {                          /* leere Nummern         */
            k += n;
            continue;
        }
        if (ord < k + n) {
            i += (ord - k) * (ty == 0xFF ? 6 : 3);
            if (ty == 0xFF) {
                *seg = e[i + 3];
                *off = e[i + 4] | (e[i + 5] << 8);
            } else {
                *seg = ty;
                *off = e[i + 1] | (e[i + 2] << 8);
            }
            return 0;
        }
        i += n * (ty == 0xFF ? 6 : 3);
        k += n;
    }
    return 1;
}

/* Modulname Nr. k (1..) aus der Referenztabelle */
static void modname(WORD k, char *d)
{
    WORD o = TW(W(nh + 0x28) + (k - 1) * 2), l, i;
    o += W(nh + 0x2A);
    l = TB(o);
    for (i = 0; i < l && i < 8; i++)
        d[i] = TB(o + 1 + i);
    d[i] = 0;
}

static int reloc_seg(struct w16mod *w, int s)
{
    static WORD cnt;
    WORD i, j, src, fl, a, b, tsg, toff, cur, nxt;
    static char mn[10];
    WORD isc;
    void FAR *fp;
    WORD sel = w->seg[s].sel;
    if (lread_near(&cnt, 2))
        return 1;
    for (i = 0; i < cnt; i += 32) {
        WORD k = cnt - i > 32 ? 32 : cnt - i;
        if (lread_near(rb, k * 8))
            return 1;
        for (j = 0; j < k; j++) {
            BYTE *r = rb + j * 8;
            src = r[0];
            fl = r[1];
            cur = W(r + 2);
            a = W(r + 4);
            b = W(r + 6);
            switch (fl & 3) {
            case 0:                             /* intern                */
                if ((a & 0xFF) == 0xFF) {
                    if (ent_lookup(w, b, &tsg, &toff))
                        continue;
                } else {
                    tsg = a & 0xFF;
                    toff = b;
                }
                if (!tsg || tsg > w->nseg)
                    continue;
                tsg = w->seg[tsg - 1].sel;
                break;
            case 1:                             /* Import Ordinalzahl    */
                modname(a, mn);
                fp = exp_find(mn, b, &isc);
                if (!fp) {
                    glogn(mn, b);
                    nmiss++;
                    fp = (void FAR *)w16_stub;
                }
                tsg = SELOF(fp);
                toff = OFFOF(fp);
                if (isc)
                    tsg = toff;
                break;
            case 2: {                           /* Import Name           */
                WORD o = W(nh + 0x2A) + b, l = TB(o), q;
                static char nm[40];
                for (q = 0; q < l && q < 39; q++)
                    nm[q] = TB(o + 1 + q);
                nm[q] = 0;
                modname(a, mn);
                glogs("Import ueber Namen fehlt: ", nm);
                nmiss++;
                fp = (void FAR *)w16_stub;
                tsg = SELOF(fp);
                toff = OFFOF(fp);
                break;
            }
            default:
                continue;                       /* OSFIXUP               */
            }
            /* Kette (nicht additiv) bzw. einmal (additiv) */
            for (;;) {
                BYTE FAR *p = MKFP(sel, cur);
                nxt = *(WORD FAR *)p;
                switch (src & 0x0F) {
                case 0:
                    if (fl & 4) *p += (BYTE)toff; else *p = (BYTE)toff;
                    break;
                case 2:
                    *(WORD FAR *)p = tsg;
                    break;
                case 3:
                    if (fl & 4)
                        *(WORD FAR *)p += toff;
                    else
                        *(WORD FAR *)p = toff;
                    *(WORD FAR *)(p + 2) = tsg;
                    break;
                case 5:
                    if (fl & 4) *(WORD FAR *)p += toff; else *(WORD FAR *)p = toff;
                    break;
                default:
                    break;
                }
                if (fl & 4 || nxt == 0xFFFF || (src & 0x0F) == 0)
                    break;
                cur = nxt;
            }
        }
    }
    return 0;
}

int w16_load(LPCSTR path)
{
    int m, s, rc = -1;
    struct w16mod *w;
    WORD tlen, sh, o, len, min, sz, i;
    static BYTE mz[64];
    for (m = 0; m < NMOD && wm[m].used; m++)
        ;
    if (m == NMOD) {
        glog("keine freie Modultabelle");
        return -1;
    }
    w = &wm[m];
    fmemset((void FAR *)w, 0, sizeof(*w));
    fstrncpy(lpath, path, sizeof(lpath));
    lh = DosOpen(lpath);
    if (FAPI_FAILED(lh)) {
        glogs("nicht gefunden: ", path);
        return -1;
    }
    tsel = 0;
    nmiss = 0;
    if (lread_near(mz, 64) || mz[0] != 'M' || mz[1] != 'Z')
        goto out;
    tbase = W(mz + 0x3C);
    if (lseek(tbase) || lread_near(nh, 64) || nh[0] != 'N' || nh[1] != 'E')
        goto out;
    /* Tabellen vom Kopf bis zum Ende der Eintragstabelle in ein Segment */
    tlen = W(nh + 4) + W(nh + 6);
    if (W(nh + 0x2A) > tlen)
        tlen = W(nh + 0x2A) + 0x400;
    if (tlen < 0x40)
        tlen = 0x40;
    tsel = seg_new(tlen);
    if (!tsel || lseek(tbase) || lread_sel(tsel, 0, tlen))
        goto out;
    w->used = 1;
    w->refs = 1;
    fstrncpy(w->path, path, sizeof(w->path));
    o = W(nh + 0x26);                           /* residente Namen: erster */
    for (i = 0; i < TB(o) && i < 8; i++)
        w->name[i] = TB(o + 1 + i);
    w->flags = W(nh + 0x0C);
    w->autodata = W(nh + 0x0E);
    w->heap = W(nh + 0x10);
    w->stack = W(nh + 0x12);
    w->entip = W(nh + 0x14);
    w->entcs = W(nh + 0x16);
    w->sp = W(nh + 0x18);
    w->ss = W(nh + 0x1A);
    w->nseg = W(nh + 0x1C);
    if (w->nseg > 24)
        goto out;
    sh = W(nh + 0x32);
    if (!sh)
        sh = 9;
    /* Eintragstabelle aufheben */
    w->ent_len = W(nh + 6);
    w->ent_sel = seg_new(w->ent_len + 2);
    if (!w->ent_sel)
        goto out;
    fmemcpy(MKFP(w->ent_sel, 0), MKFP(tsel, W(nh + 4)), w->ent_len);
    /* Ressourcentabelle */
    if (W(nh + 0x24) < W(nh + 0x26)) {
        w->res_len = W(nh + 0x26) - W(nh + 0x24);
        w->res_sel = seg_new(w->res_len);
        if (w->res_sel)
            fmemcpy(MKFP(w->res_sel, 0), MKFP(tsel, W(nh + 0x24)), w->res_len);
    }
    /* Segmente anlegen */
    for (s = 0; s < w->nseg; s++) {
        WORD e = W(nh + 0x22) + s * 8;
        unsigned long sz32;
        o = TW(e);
        len = TW(e + 2);
        w->seg[s].flags = TW(e + 4);
        min = TW(e + 6);
        sz32 = min ? min : 0x10000UL;
        if (len == 0 && o)
            len = 0;                            /* 64 KB Daten (selten)  */
        if (s + 1 == w->autodata) {
            sz32 += w->stack;
            if (w->ss == w->autodata && w->sp == 0)
                w->sp = (WORD)(sz32 > 0xFFFE ? 0xFFFE : sz32);
            sz32 += w->heap ? w->heap : 1024;
            if (sz32 > 0x10000UL)
                sz32 = 0x10000UL;
        }
        sz = sz32 >= 0x10000UL ? 0 : (WORD)sz32;
        w->seg[s].minalloc = sz;
        w->seg[s].sel = seg_new(sz);
        if (!w->seg[s].sel)
            goto out;
    }
    /* Inhalt und Relokationen */
    for (s = 0; s < w->nseg; s++) {
        WORD e = W(nh + 0x22) + s * 8;
        o = TW(e);
        len = TW(e + 2);
        if (!o)
            continue;
        if (lseek((unsigned long)o << sh))
            goto out;
        if (len ? lread_sel(w->seg[s].sel, 0, len) :
                  (lread_sel(w->seg[s].sel, 0, 0x8000U) ||
                   lread_sel(w->seg[s].sel, 0x8000U, 0x8000U)))
            goto out;
        if (w->seg[s].flags & 0x100) {
            if (lseek(((unsigned long)o << sh) + (len ? len : 0x10000UL)) ||
                reloc_seg(w, s))
                goto out;
        }
    }
    for (s = 0; s < w->nseg; s++)
        if (!(w->seg[s].flags & 1))
            fapi_call(FAPI_SEGCTL, SEG_TOCODE, w->seg[s].sel, 0);
    w->rsh = w->res_sel ? *(WORD FAR *)MKFP(w->res_sel, 0) : 0;
    if (nmiss)
        glogn("fehlende Importe: ", nmiss);
    rc = m;
out:
    if (rc < 0)
        glogs("Laden abgebrochen: ", path);
    if (tsel)
        seg_del(tsel);
    DosClose(lh);
    if (rc < 0 && w->used)
        w16_free(m);
    return rc;
}

void w16_free(int m)
{
    struct w16mod *w = &wm[m];
    int s;
    if (!w->used || --w->refs)
        return;
    for (s = 0; s < w->nseg; s++)
        if (w->seg[s].sel) {
            fapi_call(FAPI_SEGCTL, SEG_TODATA, w->seg[s].sel, 0);
            seg_del(w->seg[s].sel);
        }
    seg_del(w->ent_sel);
    seg_del(w->res_sel);
    w->used = 0;
}

/* ---- Ressourcen ----------------------------------------------------
   Kennung: Zahl (Selektor 0, MAKEINTRESOURCE) oder Name. Ergebnis von
   w16_res_find: Modul + 1 im Hochwort, Stelle des Eintrags (12 Byte) in
   der Tabelle im Niedrigwort; 0 = nicht gefunden. */
static int res_eq(struct w16mod *w, WORD id, LPCSTR want)
{
    BYTE FAR *t = MKFP(w->res_sel, 0);
    WORD l, i;
    if (SELOF(want) == 0)
        return id == (OFFOF(want) | 0x8000u);
    if (id & 0x8000u) {                         /* "#123" als Zahl       */
        WORD v = 0;
        if (want[0] != '#')
            return 0;
        for (i = 1; want[i] >= '0' && want[i] <= '9'; i++)
            v = v * 10 + (want[i] - '0');
        return id == (v | 0x8000u);
    }
    l = t[id];
    for (i = 0; i < l; i++) {
        char a = t[id + 1 + i], b = want[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b)
            return 0;
    }
    return want[l] == 0;
}

DWORD w16_res_find(int m, LPCSTR type, LPCSTR name)
{
    struct w16mod *w;
    WORD o = 2, cnt, k;
    if (m < 0 || m >= NMOD || !wm[m].used || !wm[m].res_sel)
        return 0;
    w = &wm[m];
    while (o + 8 <= w->res_len) {
        WORD tid = *(WORD FAR *)MKFP(w->res_sel, o);
        if (!tid)
            break;
        cnt = *(WORD FAR *)MKFP(w->res_sel, o + 2);
        o += 8;
        if (res_eq(w, tid, type)) {
            for (k = 0; k < cnt; k++, o += 12)
                if (res_eq(w, *(WORD FAR *)MKFP(w->res_sel, o + 6), name))
                    return MAKELONG(o, m + 1);
            return 0;
        }
        o += cnt * 12;
    }
    return 0;
}

WORD w16_res_size(DWORD id)
{
    int m = HIWORD(id) - 1;
    if (m < 0 || m >= NMOD || !wm[m].used)
        return 0;
    return *(WORD FAR *)MKFP(wm[m].res_sel, LOWORD(id) + 2) << wm[m].rsh;
}

WORD w16_res_load(int m, DWORD id)
{
    struct w16mod *w;
    unsigned long pos;
    WORD len, sel;
    (void)m;
    m = HIWORD(id) - 1;
    if (m < 0 || m >= NMOD || !wm[m].used)
        return 0;
    w = &wm[m];
    pos = (unsigned long)*(WORD FAR *)MKFP(w->res_sel, LOWORD(id)) << w->rsh;
    len = w16_res_size(id);
    if (!len)
        return 0;
    sel = seg_new(len);
    if (!sel)
        return 0;
    fstrncpy(lpath, w->path, sizeof(lpath));
    lh = DosOpen(lpath);
    if (FAPI_FAILED(lh) || lseek(pos) || lread_sel(sel, 0, len)) {
        if (!FAPI_FAILED(lh))
            DosClose(lh);
        seg_del(sel);
        return 0;
    }
    DosClose(lh);
    return sel;
}

void w16_missing(void)
{
    glog("Aufruf einer fehlenden Win16-Funktion - Programm beendet");
    if (cur_task >= 0)
        task_exit(cur_task, 0xFF);
}
