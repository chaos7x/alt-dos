/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - eigener DOS-Kern fuer die DOS-Boxen (docs/DOSKERN.md)
 * Gemeinsame Definitionen. Modell tiny (CS = DS = SS), Real Mode, 8086.
 *
 * Strukturen, die Programme sehen koennen, haben das Layout von MS-DOS 4.0
 * (INC/SF.INC, CURDIR.INC, ARENA.INC, PDB.INC, SYSVAR.INC, DOSSYM.INC);
 * die Dateiarbeit erledigt das DEM im AT-DOS-Kernel (include/dem.h) ueber
 * INT E0h 30h.
 */
#ifndef CM_H
#define CM_H

#include <i86.h>
#include "../include/dem.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

#define FP(s, o)    ((u8 __far *)MK_FP((s), (o)))
#define W(s, o)     (*(u16 __far *)MK_FP((s), (o)))
#define B(s, o)     (*(u8 __far *)MK_FP((s), (o)))
#define D(s, o)     (*(u32 __far *)MK_FP((s), (o)))

/* Rahmen des INT-21h-Aufrufs (entry.asm, Stapel des Aufrufers) */
struct frame {
    u16 es, ds, bp, di, si, dx, cx, bx, ax, ip, cs, fl;
};
#define AL(f)   (*(u8 __far *)&(f)->ax)
#define AH(f)   (*((u8 __far *)&(f)->ax + 1))
#define BL(f)   (*(u8 __far *)&(f)->bx)
#define BH(f)   (*((u8 __far *)&(f)->bx + 1))
#define CL(f)   (*(u8 __far *)&(f)->cx)
#define CH(f)   (*((u8 __far *)&(f)->cx + 1))
#define DL(f)   (*(u8 __far *)&(f)->dx)
#define DH(f)   (*((u8 __far *)&(f)->dx + 1))
#define FL_CF   0x0001
#define FL_ZF   0x0040

/* Register fuer bint() (entry.asm) */
struct rregs {
    u16 ax, bx, cx, dx, si, di, bp, ds, es, fl;
};
void bint(unsigned n, struct rregs *r);
unsigned getcs(void);

/* SFT-Eintrag wie MS-DOS 4.0 (SF.INC, 3Bh Byte) */
struct sft {
    u16 refs;           /* 00 */
    u16 mode;           /* 02 Zugriff/Freigabe wie 3Dh, 8000h FCB      */
    u8  attr;           /* 04 */
    u16 flags;          /* 05 Datei: Laufwerk Bit 0-5, 40h unveraendert,
                              4000h Datum gesetzt; Geraet: 80h + Bits  */
    u32 dev;            /* 07 Geraetekopf bzw. DPB                     */
    u16 hnd;            /* 0B hier: DEM-Handle                         */
    u16 time, date;     /* 0D 0F */
    u32 size, pos;      /* 11 15 */
    u16 relcl;          /* 19 */
    u32 dirsec;         /* 1B */
    u8  dirpos;         /* 1F */
    char name[11];      /* 20 */
    u32 chain;          /* 2B */
    u16 uid, psp, mft;  /* 2F 31 33 */
    u16 lstcl;          /* 35 */
    u32 ifs;            /* 37 */
};
#define NSFT    40

/* CDS-Eintrag wie MS-DOS 4.0 (CURDIR.INC, 58h Byte) */
struct cds {
    char path[67];      /* 00 "C:\DIR"                                 */
    u16  flags;         /* 43 4000h physisch, 0 ungueltig              */
    u32  dpb;           /* 45 */
    u16  start;         /* 49 */
    u16  rsv1, rsv2;    /* 4B 4D */
    u16  bsoff;         /* 4F Stelle des Stamm-Backslash (2)           */
    u8   rsv3[7];       /* 51 */
};
#define NDRV    26

/* Geraetekopf (18 Byte) */
struct devh {
    u32 next;
    u16 attr, strat, intr;
    char name[8];
};
enum { DV_NUL, DV_CON, DV_AUX, DV_PRN, DV_CLOCK, DV_EMM, DV_IN, DV_OUT, DV_ERR,
       DV_LPT1, DV_COM1, NDEV };

/* Fehler (DOS) */
#define E_FUNC      1
#define E_NOFILE    2
#define E_NOPATH    3
#define E_NOHANDLES 4
#define E_DENIED    5
#define E_HANDLE    6
#define E_MCB       7
#define E_NOMEM     8
#define E_BLOCK     9
#define E_ENV       10
#define E_FORMAT    11
#define E_ACCESS    12
#define E_DRIVE     15
#define E_CURDIR    16
#define E_NOMORE    18

/* kern.c */
extern u8  sda[0x80];
extern u16 kseg;
extern u16 cur_psp(void);
extern u16 dta_off, dta_seg;
extern u8  cur_drv;
extern u16 drives_lo, drives_hi;
extern struct cds cdstab[NDRV];
extern struct sft *sftab;
u16  sft_head(void);
extern struct devh devs[NDEV];
extern char setver_dx_dummy;
int  drive_ok(unsigned d);
void set_err(struct frame __far *f, unsigned e);
u16  mem_alloc(u16 paras, u16 owner, u16 *largest);
void mem_free_owner(u16 psp);
int  mem_free(u16 seg);
int  mem_resize(u16 seg, u16 paras, u16 *maxp);

/* file.c */
extern struct dem_req q;
unsigned dem(unsigned fn);
int  canon(const char __far *in, char *out, unsigned *drv);
int  dev_of(const char *path);
void file_init(void);
void psp_handles(u16 psp, u16 parent);
void close_all(u16 psp);
int  hnd_sft(unsigned h);
unsigned h_read(unsigned h, u16 seg, u16 off, unsigned n, unsigned *err);
unsigned h_write(unsigned h, u16 seg, u16 off, unsigned n, unsigned *err);
void file_fn(struct frame __far *f);
int  open_path(const char *path, unsigned mode, unsigned act, unsigned attr, unsigned *res);
void sft_close(int s);

/* con.c */
void con_out(unsigned c);
int  con_key(int wait);
int  con_ready(void);
void con_fn(struct frame __far *f);
unsigned dev_read(int d, struct sft *s, u16 seg, u16 off, unsigned n);
unsigned dev_write(int d, struct sft *s, u16 seg, u16 off, unsigned n);
void con_line(u8 __far *buf);

/* exec.c */
struct frame __far *exec_fn(struct frame __far *f);
struct frame __far *term(struct frame __far *f, unsigned type, unsigned rc, u16 keep);
extern u16 ret_code;
u16 load_shell(const char *path);
struct frame __far *cc_frame(struct frame __far *f);
u8  *lol_ptr(void);
void start_frame(u16 ss, u16 sp);

#endif
