/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - Fern-Thunks fuer Kernelteile im zweiten Codesegment
 *
 * Code in KCODE2 kann nahe Funktionen des ersten Codesegments nicht
 * direkt rufen (anderes CS). Diese Huellen liegen im ersten Segment und
 * sind fern erreichbar. Neue Hilfen hier und in k2.h eintragen.
 */
#include "kern.h"
#include "mem.h"

void redraw(void);

unsigned long __far kf_get_ticks(void) { return get_ticks(); }
unsigned __far kf_proc_block(unsigned long id, unsigned t) { return proc_block(id, t); }
void __far kf_far_copy(unsigned ds, unsigned dof, unsigned ss, unsigned sof, unsigned n)
{
    far_copy(ds, dof, ss, sof, n);
}
unsigned __far kf_kseg_dma(unsigned bytes, unsigned long *lin) { return kseg_dma(bytes, lin); }
void __far kf_scratch_map(unsigned long b, unsigned l) { scratch_map(b, l); }
void __far kf_scratch2_map(unsigned long b, unsigned l) { scratch2_map(b, l); }
void __far kf_win_save(unsigned short *b) { win_save(b); }
void __far kf_win_restore(const unsigned short *b) { win_restore(b); }
void __far kf_zero_seg(unsigned sel, unsigned w) { zero_seg(sel, w); }
pg_t __far kf_reclaim(unsigned n) { return mem_reclaim ? mem_reclaim(n) : 0; }
void __far kf_set_desc(int i, unsigned long b, unsigned l, unsigned char a) { set_desc(i, b, l, a); }
void __far kf_ldt_set(int t, unsigned sel, unsigned long b, unsigned l) { ldt_set(t, sel, b, l); }
void __far kf_ldt_drop(int t, unsigned sel) { ldt_drop(t, sel); }
void __far kf_redraw(void) { redraw(); }
void __far kf_vio_flush(void) { vio_flush(); }

/* v86.c */
int __far kf_box_ctl(unsigned op, unsigned a) { return box_ctl(op, a); }
int __far kf_vio_fg(void) { return vio_fg(); }
int __far kf_kbd_get(void) { return kbd_get(); }
int __far kf_kbd_peek(void) { return kbd_peek(); }
unsigned __far kf_kbd_shift(void) { return kbd_shift(); }
void __far kf_kbd_flush(void) { kbd_flush(); }
int __far kf_vio_ansi(int s, unsigned on) { return vio_ansi(s, on); }
unsigned box_aud(unsigned fn, unsigned a, unsigned b);
unsigned __far kf_box_aud(unsigned fn, unsigned a, unsigned b) { return box_aud(fn, a, b); }
void __far kf_vio_tty(int s, unsigned sel, unsigned off, unsigned len) { vio_tty(s, sel, off, len); }
void __far kf_vio_setmode(int s, int full, int owner) { vio_setmode(s, full, owner); }
void __far kf_vio_setcur(int s, int row, int col) { vio_setcur(s, row, col); }
void __far kf_vio_dirty(int s) { vio_dirty(s); }
unsigned long __far kf_vio_buf_lin(int s) { return vio_buf_lin(s); }
unsigned long __far kf_mem_avail_kb(void) { return mem_avail_kb(); }
unsigned __far kf_vio_mou_open(int s, int t) { return vio_mou_open(s, t); }
int __far kf_vio_mou_read(int s, unsigned char *e) { return vio_mou_read(s, e); }
void __far kf_vio_mou_show(int s, int on) { vio_mou_show(s, on); }
unsigned __far kf_vio_mou_delta(int s, int *dx, int *dy) { return vio_mou_delta(s, dx, dy); }
void __far kf_vio_mou_pos(int s, int row, int col) { vio_mou_pos(s, row, col); }
void __far kf_vio_mou_close(int s, int t) { vio_mou_close(s, t); }
unsigned __far kf_box_fapi(unsigned fn, unsigned usel, unsigned a1, unsigned a2, unsigned a3)
{
    return box_fapi(fn, usel, a1, a2, a3);
}

/* fat.c (FAT-Treiber in KTEXT2) */
int __far kf_blk_read(int dev, unsigned long lba) { return blk_read(dev, lba); }
void __far kf_vol_lock(int d) { vol_lock(d); }
int __far kf_blk_read_n(int dev, unsigned long lba, unsigned n) { return blk_read_n(dev, lba, n); }
int __far kf_blk_write(int dev, unsigned long lba) { return blk_write(dev, lba); }
void __far kf_blk_invalidate(void) { blk_invalidate(); }
int __far kf_blk_media(int dev) { return blk_media(dev); }
int __far kf_blk_nomedia(int dev) { return blk_nomedia(dev); }
unsigned __far kf_kseg_alloc(unsigned long bytes) { return kseg_alloc(bytes); }
