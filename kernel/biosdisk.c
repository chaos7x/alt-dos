/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Startlaufwerk ueber INT 13h (Rueckfall, KTEXT4).
 *
 * F5-Notstart Stufe 2 (docs/NOTSTART.md): findet kmain nach der
 * CONFIG.SYS die Startpartition nicht (kein BASEDEV=-Treiber, Treiber
 * kaputt oder ohne Geraet), legt bios_attach ein Blockgeraet DRV_BIOS
 * fuer das BIOS-Laufwerk an, von dem gestartet wurde, und haengt es ein.
 * Jeder Zugriff ist ein Ausflug in den Real Mode (rm_call13 in vesa.c,
 * rm_int10 in start.asm): PIC auf die BIOS-Vektoren mit der Maske vom
 * Start, PIT auf 18,2 Hz, Interrupts an - die Plattenroutinen des BIOS
 * und der Option-ROMs warten auf ihren IRQ und messen Fristen ueber den
 * Takt. Danach traegt bios_rw die verpassten Takte nach (x 5,5).
 *
 * Puffer: 7000h-77FFh (4 Sektoren), DAP bei 7800h - unterhalb des
 * Kernels, nach dem Start frei (wie die Puffer des Video-BIOS). Daten
 * kommen aus dem DMABUF des Tasks bzw. gehen dorthin.
 *
 * Langsam (jeder Sektor ein Moduswechsel), aber das System kommt hoch -
 * Plattentreiber gehoeren in BASEDEV=. Kein Cache-Rueckschreiben und
 * keine Auslagerungsdatei auf diesem Geraet (kernel.c, swap.c).
 */
#include "kern.h"
#include "mem.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "task.h"
#include "k2.h"                         /* KTEXT4, k2.h zuletzt           */

struct rmregs { unsigned short ax, bx, cx, dx, si, di, bp, es, ds, flags,
                intno, pic, t0, t1, pdiv; };
void __far rm_call13(void *r);
void __far mount_disk(int dev);         /* blkmnt.c                       */
extern unsigned short bios_pic;         /* start.asm: Maske beim Start    */
extern unsigned kpit_div;
extern volatile unsigned long ticks;
extern int boot_session;

#define BD_BUF   0x7000UL               /* 4 Sektoren                     */
#define BD_DAP   0x7800UL
#define BD_MAX   4

static unsigned bd_spt, bd_heads;

/* v / dv, Rest nach *r - ohne Watcom-Hilfsroutinen (wie procfs.c) */
static unsigned long ddiv(unsigned long v, unsigned dv, unsigned *r)
{
    unsigned hi = (unsigned)(v >> 16), lo = (unsigned)v, qh, ql = 0, rr = 0;
    qh = hi / dv;
    hi = hi % dv;
    _asm {
        mov dx, hi
        mov ax, lo
        mov cx, dv
        div cx
        mov ql, ax
        mov rr, dx
    }
    *r = rr;
    return ((unsigned long)qh << 16) | ql;
}
static unsigned char bd_ext;            /* INT 13h/42h-43h vorhanden      */

/* ein INT 13h im Real Mode; -> AX, CF in *cf */
static unsigned i13(struct rmregs *r, int *cf)
{
    unsigned long d;
    r->pic = bios_pic;
    r->pdiv = kpit_div ? kpit_div : 11932;
    r->ds = 0;
    rm_call13(r);
    d = (unsigned)(r->t1 - r->t0);      /* BIOS-Takte (18,2 Hz)       */
    if (d && d < 0x8000U)               /* -> Kerneltakte (100 Hz)    */
        ticks += (unsigned)d * 5 + ((unsigned)d >> 1);
    *cf = r->flags & 1;
    return r->ax;
}

/* n Sektoren ab lba lesen (wr 0) nach DMABUF bzw. schreiben (wr 1) aus
   DMABUF; -> 0 ok, -1 Fehler */
static int rw0(unsigned drive, unsigned long lba, unsigned n, int wr)
{
    struct rmregs r;
    unsigned long c, t;
    unsigned s, h, try_, k, o = 0;      /* o: Stelle im DMABUF          */
    int cf;

    while (n) {
        k = n > BD_MAX ? BD_MAX : n;
        if (wr) {
            scratch_map(BD_BUF, k * 512 - 1);
            far_copy(SCRATCH, 0, DMABUF, o, k * 512);
        }
        for (try_ = 0; try_ < 3; try_++) {
            t = ddiv(lba, bd_spt, &s);
            s++;
            c = ddiv(t, bd_heads, &h);
            r.es = (unsigned)(BD_BUF >> 4);
            r.bx = 0;
            r.dx = (h << 8) | drive;
            if (c < 1024) {
                if (k > bd_spt - s + 1)     /* CHS: nicht ueber das      */
                    k = bd_spt - s + 1;     /* Spurende (BIOS-abhaengig) */
                r.ax = (wr ? 0x0300 : 0x0200) | k;
                r.cx = ((unsigned)c << 8) | (((unsigned)c >> 2) & 0xC0) | s;
            } else if (bd_ext) {
                scratch_map(BD_DAP, 15);
                {
                    unsigned __far *p = MK_FP(SCRATCH, 0);
                    p[0] = 16;
                    p[1] = k;
                    p[2] = 0;
                    p[3] = (unsigned)(BD_BUF >> 4);
                    p[4] = (unsigned)lba;
                    p[5] = (unsigned)(lba >> 16);
                    p[6] = p[7] = 0;
                }
                r.ax = wr ? 0x4300 : 0x4200;
                r.si = (unsigned)BD_DAP;
                r.dx = drive;
            } else
                return -1;
            i13(&r, &cf);
            if (!cf)
                break;
            r.ax = 0;                   /* Laufwerk zuruecksetzen       */
            r.dx = drive;
            i13(&r, &cf);
        }
        if (try_ == 3)
            return -1;
        if (!wr) {
            scratch_map(BD_BUF, k * 512 - 1);
            far_copy(DMABUF, o, SCRATCH, 0, k * 512);
        }
        lba += k;
        n -= k;
        o += k * 512;
    }
    return 0;
}

/* Die Fenster SCRATCH/SCRATCH2 gehoeren dem Aufrufer (fat.c, load_image
   lesen gerade ueber sie) - bios_rw und rm_call legen sie um */
int __far bios_rw(unsigned drive, unsigned long lba, unsigned n, int wr)
{
    unsigned short w[8];
    int rc;
    win_save(w);
    rc = rw0(drive, lba, n, wr);
    win_restore(w);
    return rc;
}

/* Blockgeraet fuer das Startlaufwerk des BIOS anlegen und einhaengen;
   -> Geraet oder -1 */

/* ---- Startcode (ITEXT, docs/INIT-SEGMENT.md): nach dem Start frei.
   Hilfen dieser Datei, die auch zur Laufzeit laufen, ueber ferne Thunks
   kt_* (Segment KTEXT4; Datei mit -oe=0, sonst eingesetzt) ---- */
static unsigned __far kt_i13(struct rmregs *r, int *cf) { return i13(r, cf); }
#define i13 kt_i13
#pragma code_seg("ITEXT", "CODE")
static const char __based(__segname("IDATA")) kb_1[] = "\xFE Startlaufwerk \x81" "ber INT 13h - Ger\x84te \x81"
              "ber INT 13h laufen langsamer, Treiber in BASEDEV= pr\x81" "fen\n";

int __far bios_attach(void)
{
    struct rmregs r;
    unsigned long blocks;
    int cf, dev;

    if (boot_drive < 0x80 || nblk >= NBLK)
        return -1;
    r.ax = 0x0800;                      /* Geometrie                    */
    r.dx = boot_drive;
    r.es = r.di = 0;
    i13(&r, &cf);
    if (cf || !(r.cx & 0x3F))
        return -1;
    bd_spt = r.cx & 0x3F;
    bd_heads = (r.dx >> 8) + 1;
    {                                   /* Zylinder x Koepfe x Sektoren */
        unsigned cy = ((r.cx >> 8) | ((r.cx & 0xC0) << 2)) + 1, i;
        blocks = 0;                     /* (ohne 32-Bit-Multiplikation) */
        for (i = 0; i < bd_heads * bd_spt; i++)
            blocks += cy;
    }
    r.ax = 0x4100;                      /* LBA-Erweiterungen?           */
    r.bx = 0x55AA;
    r.dx = boot_drive;
    i13(&r, &cf);
    bd_ext = !cf && r.bx == 0xAA55 && (r.cx & 1);
    if (bd_ext) {                       /* 48h: Groesse in Sektoren     */
        scratch_map(BD_DAP, 0x1F);
        *(unsigned __far *)MK_FP(SCRATCH, 0) = 0x1E;
        r.ax = 0x4800;
        r.dx = boot_drive;
        r.ds = 0;
        r.si = (unsigned)BD_DAP;
        i13(&r, &cf);
        scratch_map(BD_DAP, 0x1F);
        if (!cf && !*(unsigned long __far *)MK_FP(SCRATCH, 0x14) &&
            *(unsigned long __far *)MK_FP(SCRATCH, 0x10) > blocks)
            blocks = *(unsigned long __far *)MK_FP(SCRATCH, 0x10);
    }
    dev = nblk++;
    blk[dev].drv = DRV_BIOS;
    blk[dev].unit = boot_drive;
    blk[dev].blocks = blocks;
    blk[dev].bsize = 512;
    blk[dev].rem = 0;
    blk[dev].grp = 0;
    {
        const char __far *m = kb_1;           /* Text in IDATA: stueckweise */
        char b[33];
        unsigned i;
        while (*m) {
            for (i = 0; i < 32 && m[i]; i++)
                b[i] = m[i];
            b[i] = 0;
            vio_kputs(boot_session, b);
            m += i;
        }
    }
    mount_disk(dev);
    return dev;
}
