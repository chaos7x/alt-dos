/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - PROCFS (KTEXT4): Zustand des Kernels als Textdateien auf
 * einem eigenen Laufwerk, Vorbild /proc von Linux. Vorgabe P:, anderer
 * Buchstabe mit SET PROCFS=X: in der CONFIG.SYS, SET PROCFS=OFF schaltet
 * es ab.
 *
 * Nur lesend, ein Verzeichnis. Jede Datei entsteht beim Oeffnen als
 * Momentaufnahme in einem Kernelsegment (erst beim ersten Oeffnen
 * angelegt); liest ein anderes Handle zwischendurch eine andere Datei,
 * wird sie beim naechsten Lesen neu erzeugt. Alles laeuft unter der
 * Kernel- und Laufwerkssperre und schlaeft nie.
 *
 * Die Texte stehen im Codesegment (__based KTEXT4) - DGROUP ist fast voll.
 * Die Tabellen liest procfs.c direkt (Daten sind gemeinsam, DS); nur was
 * in v86.c steckt, liefert v86_pfs als Zahlen. Texte mit __segname("_CODE")
 * liest der Code ueber CS (KCODE4 ist lesbar) - ein __segname mit dem
 * Namen des Segments ergaebe ein eigenes FAR_DATA-Segment hinter DGROUP.
 */
#include "kern.h"
#include "mem.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "task.h"
#include "../build/version.h"
#include "k2.h"                         /* KTEXT4, k2.h zuletzt           */
#include <stdarg.h>

#define KS      __based(__segname("_CODE"))
#define PF(s, ...) do { static const char KS f_[] = s; pf(f_, __VA_ARGS__); } while (0)
#define P0(s)   do { static const char KS f_[] = s; pf(f_); } while (0)
#define PBUF    24576u                  /* groesste Datei                 */

extern int current, klock;
extern unsigned klock_depth;
extern unsigned char bkl_hold, vk_owner[NDRIVE + 1];
extern unsigned char dl_owner[NDRV + 1 + NXG], dl_depth[NDRV + 1 + NXG];
extern signed char xg_drv[NXG];
extern unsigned char xg_grp[NXG], kseg_own[NKSEG];
extern unsigned env_sel, alloc_units;
extern unsigned short kstack[NTASK][KSTACK];
extern unsigned long v86_pages, bc_hits, bc_miss;
extern unsigned bc_n, bc_ndirty, bc_nch, bc_present, bc_minch;
extern unsigned short irq_hcs[16];
void __far v86_pfs(int t, unsigned *v); /* v86.c: Zustand einer Box     */

static unsigned pb_sel, pb_len, pb_cur = 0xFFFF;
static unsigned long pb_ser, ser;
static signed char pfs_drive = -1;

#define NPF 13
static const char KS pnames[NPF][9] = {
    "VERSION", "UPTIME", "MEMINFO", "CACHE", "TASKS", "LOCKS", "KSEG",
    "DRIVERS", "BLKDEV", "DRIVES", "FILES", "BOXES", "DMESG"
};

/* ---- Ausgabe ------------------------------------------------------------ */
static void ch(char c)
{
    if (pb_len < PBUF)
        ((char __far *)MK_FP(pb_sel, 0))[pb_len++] = c;
}

/* v / dv, Rest nach *r - ohne __U4D (zweimal DIV 32/16) */
static unsigned long ddiv(unsigned long v, unsigned dv, unsigned *r)
{
    unsigned hi = (unsigned)(v >> 16), lo = (unsigned)v, qh, ql = 0, rm;
    qh = hi / dv;
    rm = hi % dv;
    _asm {
        mov dx, rm
        mov ax, lo
        mov cx, dv
        div cx
        mov ql, ax
        mov rm, dx
    }
    *r = rm;
    return ((unsigned long)qh << 16) | ql;
}
#define d10(v, r)   ddiv(v, 10, r)

/* Mini-printf: %[-][0][Breite][l](u|d|x|c|s|S), %% */
void __far pf(const char __far *f, ...)
{
    va_list ap;
    char b[12], pad;
    const char *s;
    const char __far *fs;
    unsigned w, n, k, r, left, lng;
    unsigned long v;
    int neg;

    va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') {
            if (*f == '\n')
                ch('\r');
            ch(*f);
            continue;
        }
        f++;
        left = lng = 0;
        pad = ' ';
        if (*f == '-') { left = 1; f++; }
        if (*f == '0') { pad = '0'; f++; }
        for (w = 0; *f >= '0' && *f <= '9'; f++)
            w = w * 10 + (unsigned)(*f - '0');
        if (*f == 'l') { lng = 1; f++; }
        n = 0;
        s = 0;
        fs = 0;
        neg = 0;
        switch (*f) {
        case 'c':
            b[n++] = (char)va_arg(ap, int);
            break;
        case 's':
            s = va_arg(ap, const char *);
            break;
        case 'S':
            fs = va_arg(ap, const char __far *);
            break;
        case 'd': case 'u': case 'x':
            if (lng)
                v = va_arg(ap, unsigned long);
            else if (*f == 'd') {
                int i = va_arg(ap, int);
                if (i < 0) { neg = 1; i = -i; }
                v = (unsigned)i;
            } else
                v = va_arg(ap, unsigned);
            if (*f == 'x') {
                do { k = (unsigned)v & 15; b[n++] = (char)(k < 10 ? '0' + k : 'A' + k - 10); v >>= 4; } while (v);
            } else {
                do { v = d10(v, &r); b[n++] = (char)('0' + r); } while (v);
            }
            if (neg)
                b[n++] = '-';
            break;
        default:
            ch(*f);
            if (!*f)
                f--;
            continue;
        }
        if (s || fs) {                  /* Zeichenkette: Laenge zaehlen    */
            for (k = 0; s ? s[k] : fs[k]; k++)
                ;
            if (!left) for (; k < w; w--) ch(' ');
            for (r = 0; s ? s[r] : fs[r]; r++)
                ch(s ? s[r] : fs[r]);
            if (left) for (; k < w; w--) ch(' ');
            continue;
        }
        if (!left) for (k = n; k < w; k++) ch(pad);
        if (*f == 'c')
            ch(b[0]);
        else
            while (n) ch(b[--n]);
        if (left) for (k = (*f == 'c') ? 1 : 0; k < w; k++) ch(' ');
    }
    va_end(ap);
}

/* ---- Hilfen ------------------------------------------------------------- */
static const char *tname(int t)
{
    return (t >= 0 && t < NTASK && task[t].state != T_FREE) ? task[t].name : "-";
}

static void owner(unsigned char o)      /* Task + 1, 0 = frei              */
{
    if (!o)
        P0("frei");
    else if (o == 0xFF)
        P0("Kernel");
    else
        PF("%u %s", o - 1, tname(o - 1));
}

static void sec(unsigned long tk)       /* Takte -> h:mm:ss                */
{
    unsigned long s;
    unsigned r, ss, mm;
    s = ddiv(tk, 100, &r);
    s = ddiv(s, 60, &ss);
    s = ddiv(s, 60, &mm);
    PF("%lu:%02u:%02u", s, mm, ss);
}

static unsigned long dbase(int i)
{
    struct desc *d = &gdt[i];
    return (d->base_lo | ((unsigned long)d->base_hi << 16) |
            ((unsigned long)(d->rsv & 0xFF00) << 16)) - KALIAS;
}

/* ---- Dateien ------------------------------------------------------------ */
static void g_version(void)
{
    PF("AT-DOS 6.3\nBuild %s (%s, %s)\n", (const char *)ATDOS_BUILD,
       (const char *)ATDOS_REV, (const char *)ATDOS_DATE);
    PF("FAPI %x\nTasks %u, Treiber %u, Kernelsegmente %u, Laufwerke %u\n",
       FAPI_VERSION, NTASK, NDRV, NKSEG, NDRIVE);
}

static void g_uptime(void)
{
    unsigned long tk = get_ticks();
    P0("Laufzeit ");
    sec(tk);
    PF("\nTakte %lu\nMinuten %u\n", tk, uptime_m);
}

static void g_meminfo(void)
{
    PF("RAM:         %8lu KB\n", mem_ram_kb());
    PF("Gesamt:      %8lu KB\n", mem_total_kb());
    PF("Frei:        %8lu KB\n", mem_free_kb());
    PF("Programme:   %8lu KB\n", (unsigned long)image_units << 2);
    PF("AllocSeg:    %8lu KB\n", (unsigned long)alloc_units << 2);
    PF("Cache:       %8lu KB\n", (unsigned long)bc_present << 4);
    PF("DOS-Boxen:   %8lu KB\n", v86_pages << 2);
}

static void g_cache(void)
{
    unsigned long a = bc_hits + bc_miss;
    PF("Bloecke %u zu 2 KB, Stuecke %u von %u da (mindestens %u)\n",
       bc_n, bc_present, bc_nch, bc_minch);
    PF("Groesse %lu KB, verzoegert %u Bloecke\n", (unsigned long)bc_present << 4,
       bc_ndirty);
    PF("Treffer %lu, Fehltreffer %lu", bc_hits, bc_miss);
    if (a) {
        unsigned long q = bc_hits;
        while (a > 655) { a >>= 1; q >>= 1; }
        PF(" (%u %%)", (unsigned)q * 100 / (unsigned)a);
    }
    P0("\n");
}

static void g_tasks(void)
{
    static const char KS st[] = "-RWB";
    int t;
    unsigned w, fr;
    P0("Nr S Sitz Elt Box Sperre LW EA Warten Tiefe Kill Stapel Name\n");
    for (t = 0; t < NTASK; t++) {
        struct task *p = &task[t];
        if (p->state == T_FREE)
            continue;
        for (fr = 0; fr < KSTACK && !kstack[t][fr]; fr++)
            ;
        PF("%2u %c %4d %3d %3u ", t, st[p->state & 3], p->session, p->parent, p->v86);
        PF("%-6s ", klock == t ? "K" : p->want_lock ? "will" : "-");
        if (p->vkey) PF("%c:", 'A' + p->vkey - 1); else P0("- ");
        PF(" %2u ", p->eakey);
        w = p->wait_k;
        if (!w) P0("-     ");
        else if (w & 0x80) PF("T%-4u ", w & 0x7F);
        else PF("%c:    ", 'A' + w - 1);
        PF(" %5u %4u %6u", p->lock_depth, p->kill_req, fr * 2);
        PF(" %s %S\n", p->name, (const char __far *)p->args);
    }
    P0("\nS: R bereit, W wartet auf Kind, B im Treiber; Stapel: nie benutzte Bytes\n");
}

static void g_locks(void)
{
    int i, n = 0;
    P0("Kernelsperre: ");
    if (klock < 0) P0("frei"); else PF("%d %s, Tiefe %u", klock, tname(klock), klock_depth);
    PF("%s\nlaufend: %d %s\n\nLaufwerkssperren:\n", bkl_hold ? " (gehalten)" : "",
       current, tname(current));
    for (i = 0; i <= NDRIVE; i++)
        if (vk_owner[i]) {
            if (i == NDRIVE) P0("  EA-Cache  ");
            else PF("  %c:        ", 'A' + i);
            owner(vk_owner[i]);
            P0("\n");
            n++;
        }
    if (!n) P0("  keine\n");
    P0("\nTreibersperren:\n");
    for (i = 0, n = 0; i < NDRV + 1 + NXG; i++) {
        if (!dl_owner[i])
            continue;
        if (i < NDRV) PF("  %-12s ", drv[i].name);
        else if (i == NDRV) P0("  IFS-Module   ");
        else PF("  %-8s Gr.%u ", drv[xg_drv[i - NDRV - 1]].name, xg_grp[i - NDRV - 1]);
        owner(dl_owner[i]);
        PF(", Tiefe %u\n", dl_depth[i]);
        n++;
    }
    if (!n) P0("  keine\n");
    P0("\nWartende:\n");
    for (i = 0, n = 0; i < NTASK; i++)
        if (task[i].state != T_FREE && (task[i].wait_k || task[i].want_lock)) {
            PF("  %d %s: ", i, task[i].name);
            if (task[i].want_lock) P0("Kernelsperre ");
            if (task[i].wait_k & 0x80) PF("Treiber %u", task[i].wait_k & 0x7F);
            else if (task[i].wait_k) PF("Laufwerk %c:", 'A' + task[i].wait_k - 1);
            P0("\n");
            n++;
        }
    if (!n) P0("  keine\n");
}

static void g_kseg(void)
{
    int i, n = 0;
    P0("Nr  Sel   Basis     Groesse  Besitzer\n");
    for (i = 0; i < NKSEG; i++) {
        struct desc *d = &gdt[KSEG_IDX(i)];
        if (!d->access) {
            n++;
            continue;
        }
        PF("%2u %04x %08lx %8lu  ", i, KSEG_IDX(i) * 8, dbase(KSEG_IDX(i)),
           (unsigned long)d->limit + 1);
        if (kseg_own[i] & 0x7F)
            PF("%s%s", drv[(kseg_own[i] & 0x7F) - 1].name, kseg_own[i] & 0x80 ? " (MMIO)" : "");
        else
            P0("Kernel");
        P0("\n");
    }
    PF("\n%u von %u frei\n", n, NKSEG);
}

static void g_drivers(void)
{
    int d, i;
    P0("Nr Name         Einh. IRQ\n");
    for (d = 0; d < NDRV; d++) {
        if (!drv[d].used)
            continue;
        PF("%2u %-12s %5u ", d, drv[d].name, drv[d].cnt);
        for (i = 0; i < 16; i++)
            if (irq_hcs[i] == DRVC_SEL(d))
                PF(" %u", i);
        P0("\n");
    }
}

static void g_blkdev(void)
{
    int b;
    P0("Nr Treiber      Einh. Bloecke    Groesse  Art\n");
    for (b = 0; b < nblk; b++) {
        if (!blk[b].bsize)
            continue;
        PF("%2u ", b);
        if (blk[b].drv >= 0) PF("%-12s", drv[blk[b].drv].name);
        else if (blk[b].drv == -1) P0("Diskette    ");
        else if (blk[b].drv == DRV_LOOP) P0("Loop        ");
        else P0("UFS/PROC    ");
        PF(" %5u %10lu %6u   %s%s\n", blk[b].unit, blk[b].blocks, blk[b].bsize & 0xFFF,
           blk[b].rem ? "Wechsel " : "", blk[b].grp ? "Gruppe" : "");
    }
}

static void g_drives(void)
{
    int d;
    P0("LW Dateisystem Geraet Start      Sektoren   Zustand\n");
    for (d = 0; d < NDRIVE; d++) {
        struct volume *v = &vol[d];
        if (!v->present && !v->used)
            continue;
        PF("%c: %-11S %6d %10lu %10lu ", 'A' + d,
           v->used && v->drv ? (const char __far *)v->drv->name : (const char __far *)"RAW",
           v->blk, v->start, v->psize ? v->psize : v->sectors);
        if (v->locked) P0(" gesperrt");
        if (v->in_use) P0(" beschrieben");
        if (v->was_dirty) P0(" unsauber");
        P0("\n");
    }
}

static void g_files(void)
{
    static const char KS kinds[10][5] = {
        "DAT", "PIPR", "PIPW", "CON", "?", "DASD", "LPT", "NUL", "COM", "SOCK" };
    unsigned i;
    int n = 0;
    P0("Nr Besitzer Ref Art  Stelle     Groesse    Pfad\n");
    for (i = 0; i < nfiles; i++) {
        struct ofile __far *f = &files[i];
        if (!f->refs)
            continue;
        n++;
        PF("%2u %8d %3u %-4S %10lu %10lu ", i, f->owner, f->refs,
           (const char __far *)MK_FP(KCODE4, (unsigned)kinds[f->kind < 10 ? f->kind : 4]), f->pos, f->f.size);
        if (f->kind == OF_FILE)
            PF("%c:%S", 'A' + f->f.drive, (const char __far *)f->path);
        P0("\n");
    }
    PF("\n%d von %u belegt\n", n, nfiles);
}

static void g_boxes(void)
{
    int t, n = 0;
    for (t = 0; t < NTASK; t++)
        if (task[t].state != T_FREE && task[t].v86) {
            static unsigned v[20];
            v86_pfs(t, v);
            PF("Box %d %s\n  Sitzung %d, vorn %u, Kern %s, VME %u\n", t, task[t].name,
               (int)v[0], v[1], v[2] ? (const char *)"ATDOSCM" : (const char *)"Abbild", v[3]);
            PF("  IF %u, PIC Maske %02x ISR %02x IRR %02x, Takte offen %u, PIT-Teiler %u\n",
               v[4], v[5], v[6], v[7], v[8], v[9]);
            PF("  DPMI %u (%u Bit), Speicher %u KB, XMS %u KB, A20 %u, Hilfen aus %02x\n",
               v[10], v[11], v[12], v[13], v[14], v[15]);
            PF("  INT 2Fh %u, Grafik roh %u, Lautsprecher %u, Joystick %u\n",
               v[16], v[17], v[18], v[19]);
            P0("\n");
            n++;
        }
    if (!n)
        P0("keine DOS-Box\n");
}

static void g_dmesg(void)
{
    unsigned st, i;
    const char __far *r;
    if (!subst_sel || !klog_len)
        return;
    st = (klog_head - klog_len) & (KLOG_SIZE - 1);
    r = MK_FP(subst_sel, KLOG_OFF);
    for (i = 0; i < klog_len; i++)
        ch(r[(st + i) & (KLOG_SIZE - 1)]);
}

static int gen(unsigned i)
{
    if (!pb_sel && !(pb_sel = kseg_alloc(PBUF)))
        return -1;
    pb_len = 0;
    switch (i) {
    case 0: g_version(); break;
    case 1: g_uptime(); break;
    case 2: g_meminfo(); break;
    case 3: g_cache(); break;
    case 4: g_tasks(); break;
    case 5: g_locks(); break;
    case 6: g_kseg(); break;
    case 7: g_drivers(); break;
    case 8: g_blkdev(); break;
    case 9: g_drives(); break;
    case 10: g_files(); break;
    case 11: g_boxes(); break;
    case 12: g_dmesg(); break;
    }
    pb_cur = i;
    return 0;
}

/* ---- IFS ---------------------------------------------------------------- */
static int find(const char *path)
{
    unsigned i, k;
    if (*path == '\\')
        path++;
    for (i = 0; i < NPF; i++) {
        for (k = 0; pnames[i][k] && path[k] == pnames[i][k]; k++)
            ;
        if (!pnames[i][k] && (!path[k] || (path[k] == '.' && !path[k + 1])))
            return (int)i;
    }
    return -1;
}

static int isroot(const char *p)
{
    return !p[0] || (p[0] == '\\' && !p[1]);
}

static int __far p_mount(int d)
{
    (void)d;
    return 0;
}

static int __far p_open(int d, const char *path, struct ifs_file __far *f)
{
    int i = find(path);
    if (i < 0 || gen(i))
        return IFS_ERR;
    f->drive = (unsigned char)d;
    f->first = i;
    f->cache_clus = f->cache_pos = 0;
    f->size = pb_len;
    f->attr = ATTR_RO;
    f->fsdata = pb_ser = ++ser;
    return 0;
}

static int __far p_read(struct ifs_file __far *f, unsigned long pos,
                        unsigned sel, unsigned off, unsigned len)
{
    unsigned long end = f->size;
    if (pb_cur != (unsigned)f->first || pb_ser != f->fsdata) {
        if (gen((unsigned)f->first))
            return -1;
        pb_ser = f->fsdata;
    }
    if (pb_len < end)
        end = pb_len;
    if (pos >= end)
        return 0;
    if (len > end - pos)
        len = (unsigned)(end - pos);
    far_copy(sel, off, pb_sel, (unsigned)pos, len);
    return (int)len;
}

static int __far p_readdir(int d, const char *dir, unsigned n, struct ifs_dirent *e)
{
    unsigned long st;
    unsigned k;
    (void)d;
    if (!isroot(dir) || n >= NPF || gen(n))
        return -1;
    for (k = 0; pnames[n][k]; k++)
        e->name[k] = pnames[n][k];
    e->name[k] = 0;
    e->sname[0] = 0;
    e->attr = ATTR_RO;
    e->size = pb_len;
    st = rtc_stamp();
    e->date = (unsigned short)(st >> 16);
    e->time = (unsigned short)st;
    pb_cur = 0xFFFF;                    /* Puffer gehoert keinem Handle    */
    return 0;
}

static int __far p_isdir(int d, const char *path)
{
    (void)d;
    return isroot(path);
}

static int __far p_label(int d, char *lbl, unsigned long *serial, const char *nl)
{
    (void)d;
    if (nl)
        return IFS_WP;
    lbl[0] = 'P'; lbl[1] = 'R'; lbl[2] = 'O'; lbl[3] = 'C'; lbl[4] = 0;
    if (serial)
        *serial = 0;
    return 0;
}

static int __far p_space(int d, unsigned long *r)
{
    (void)d;
    r[0] = r[1] = 0;
    return 0;
}

static int __far p_namepath(int d, const char *path, char *out, int shortn)
{
    unsigned k;
    (void)d; (void)shortn;
    for (k = 0; path[k] && k < IFS_PATHLEN - 1; k++)
        out[k] = path[k];
    out[k] = 0;
    return 0;
}

struct ifs_driver pfs_driver = {
    "PROCFS", p_mount, p_open, p_read, p_readdir, p_isdir, 0, 0, 0, 0, 0, 0,
    0, p_space, 0, p_label, p_namepath, 0
};

/* Beim Start (kmain): Laufwerk anlegen. SET PROCFS=X: oder OFF in der
   Systemumgebung (CONFIG.SYS) */
void __far procfs_boot(void)
{
    const char __far *e = MK_FP(env_sel, NTASK * FAPI_ENVSIZE);
    unsigned i = 0;
    int d = -1, b;                      /* ab Werk aus                     */
    static const char KS key[] = "PROCFS=";

    if (env_sel)
        while (e[i]) {
            unsigned k;
            for (k = 0; key[k] && e[i + k] == key[k]; k++)
                ;
            if (!key[k]) {
                char c = e[i + k];
                if (c >= 'a' && c <= 'z')
                    c -= 32;
                if (c >= 'C' && c <= 'Z')
                    d = c - 'A';
                break;
            }
            while (e[i++])
                ;
        }
    if (d < 0 || (e[i + 7 + 1] && e[i + 7 + 1] != ':'))
        return;
    if (vol[d].used || vol[d].present)
        return;
    for (b = 2; b < nblk && blk[b].bsize; b++)
        ;
    if (b >= NBLK)
        return;
    if (b == nblk)
        nblk++;
    blk[b].drv = DRV_UFS;
    blk[b].bsize = 512;
    blk[b].blocks = 0;
    blk[b].rem = blk[b].grp = 0;
    vol[d].blk = b;
    vol[d].start = 0;
    vol[d].drv = &pfs_driver;
    vol[d].mod = -1;
    vol[d].locked = 0;
    vol[d].present = 1;
    vol[d].used = 1;
    pfs_drive = (signed char)d;
}
