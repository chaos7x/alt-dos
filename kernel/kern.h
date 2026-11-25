/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - gemeinsame Kernel-Definitionen
 */
#ifndef KERN_H
#define KERN_H

#include <i86.h>

#pragma pack(1)

/* ---- GDT-Belegung ------------------------------------------------------ */
#define KCODE       0x08
#define KDATA       0x10
#define VIDEO       0x18
#define GATE_IDX    4               /* -> Selektor 0x20, RPL 3 = 0x23      */
#define TSS_K       0x28
#define SCRATCH     0x30            /* Kernel-Fenster auf beliebigen Speicher */
#define DMABUF      0x38            /* DMA-Puffer fuer Laufwerke           */
#define SCRATCH2    0x40            /* zweites Fenster, z. B. fuer Kopien   */

#define NTASK       8               /* Tasks und Sitzungen hoechstens       */
#define TSS_IDX(i)  (9 + (i))
#define LDT_IDX(i)  (9 + NTASK + (i))
#define SCR_IDX(i)  (9 + 2 * NTASK + (i))   /* Bildschirmpuffer je Sitzung */
#define TSS_SEL(i)  (TSS_IDX(i) * 8)
#define LDT_SEL(i)  (LDT_IDX(i) * 8)
#define SCR_SEL(i)  (SCR_IDX(i) * 8)
#define NDRV        16              /* Geraetetreiber und IFS-Module; 6.3:
                                       6 -> 12 (Box-Module VCOM/VSB/VEMS/VEGA),
                                       12 -> 16 (Netz + Box-Module + SCSI) */
#define DRVC_IDX(i) (9 + 3 * NTASK + 2 * (i))   /* Treibercode, Ring 0    */
#define DRVC_SEL(i) (DRVC_IDX(i) * 8)
#define DRVD_SEL(i) (DRVC_SEL(i) + 8)           /* Treiberdaten           */
#define NXG         12              /* Sperrgruppen ueber 0 (alle Treiber) */
#define KSTACK      768             /* Kernelstapel je Task in Worten   */
#define NKSEG       28              /* eigene Kernelsegmente (kseg_alloc); 6.2: 20 -> 28 (acht Loops) */
#define KSEG_IDX(i) (9 + 3 * NTASK + 2 * NDRV + (i))
#define KCODE2_IDX  (9 + 3 * NTASK + 2 * NDRV + NKSEG)
#define KCODE2      0x2E8           /* = KCODE2_IDX * 8; build.sh traegt ihn ein */
#define VWIN_IDX    (KCODE2_IDX + 1) /* Fenster in den Speicher einer DOS-Box */
#define VWIN        (VWIN_IDX * 8)  /* (Basis roh, ohne KALIAS; v86.c)     */
#define RWIN_IDX(t) (KCODE2_IDX + 2 + (t))   /* Fenster je Box (redir.c) */
#define RWIN_SEL(t) (RWIN_IDX(t) * 8)
#define KCODE3_IDX  (KCODE2_IDX + 2 + NTASK)  /* drittes Codesegment (v86.c, redir.c) */
#define KCODE3      0x338           /* = KCODE3_IDX * 8; build.sh traegt ihn ein */
#define FCACHE_IDX  (KCODE3_IDX + 1) /* FAT-Cache (fat.c), fest bei FCACHE_LIN */
#define FCACHE_SEL  (FCACHE_IDX * 8)
#define CTXWIN_IDX  (KCODE3_IDX + 2) /* Ablage des Kontexts je Task (Phase 6) */
#define CTXWIN      (CTXWIN_IDX * 8)
#define WBUF_IDX    (KCODE3_IDX + 3) /* Schreibpuffer je Task (bc_wb, Phase 6) */
#define WBUF        (WBUF_IDX * 8)
#define LDTD_IDX    (KCODE3_IDX + 4) /* Datenfenster auf die LDTs aller Tasks */
#define LDTD        (LDTD_IDX * 8)
#define KCODE4_IDX  (KCODE3_IDX + 5) /* viertes Codesegment (loop.c u. a.) */
#define KCODE4      0x360           /* = KCODE4_IDX * 8; build.sh traegt ihn ein */
#define KCODEI_IDX  (KCODE3_IDX + 6) /* Startcode (ITEXT), nach dem Start frei */
#define KCODEI      0x368           /* = KCODEI_IDX * 8; build.sh traegt ihn ein */
#define KIDATA_IDX  (KCODE3_IDX + 7) /* Texte des Startcodes (IDATA), ebenso */
#define KIDATA      0x370           /* = KIDATA_IDX * 8; build.sh traegt ihn ein */
#define NGDT        (KCODE3_IDX + 8)
/* KSEG_IDX(0) ist fest die Speicher-Bitmap (MEMMAP_SEL in mem.h) */

#define DMABUF_LIN  0x8000UL        /* unter 1 MB, keine 64-KB-Grenze      */
#define DMABUF_SIZE 2048            /* ein CD-Block; Platten nutzen 512    */
#define FCACHE_LIN  (DMABUF_LIN + DMABUF_SIZE)  /* 1 KB FAT-Cache (fat.c)  */
#define FCACHE_SIZE (1024 + 16)     /* + Kennung (fat.c struct fct)        */
void __far fat_op_begin(void);
/* Phase 6: DMABUF gehoert dem laufenden Task - jeder Task hat eine eigene
   Seite unter 16 MB, switch_to legt den Deskriptor um. dmab_phys = ihre
   physische Adresse (fuer Treiber/DMA); vor dem Start der Tasks und ohne
   eigene Seite DMABUF_LIN. */
extern unsigned long dmab_phys;
/* Kontext je Task (Phase 6): Variablen, die eine Operation mit Kernel-
   sperre ueber einen Schlafpunkt hinweg haelt (kernel.c ctx_take) */
struct ctxblk { void *p; unsigned n; };
/* NE-Lader (neload.c, KTEXT4): Startwerte einer NE-EXE */
struct ne_start { unsigned cs, ip, ss, sp, ds; unsigned char lfn; };
int  __far ne_load(int t, int drive, const char *path, struct ne_start *st);
void __far ne_task_end(int t);
extern unsigned char ne_sh[64];         /* LDT-Eintraege der DLLs (alle Tasks) */
#define NE_SH(i) (ne_sh[(i) >> 3] & (1 << ((i) & 7)))
extern const struct ctxblk fat_ctx[], redir_ctx[], dem_ctx[];
extern const unsigned fat_nctx, redir_nctx, dem_nctx;

/* Kernel: Code ab 10000h, Daten ab dem naechsten Paragraphen dahinter */
#define KCODE_LIN   0x10000UL
extern unsigned kcode_size;         /* start.asm: Groesse von CGROUP       */
extern unsigned kcode2_size;        /* start.asm: Groesse von CGROUP2      */
#define KCODE2_LIN  (KCODE_LIN + ((kcode_size + 15) & ~15u))
extern unsigned kcode3_size;        /* start.asm: Groesse von CGROUP3      */
#define KCODE3_LIN  (KCODE2_LIN + ((kcode2_size + 15) & ~15u))
extern unsigned kcode4_size;        /* start.asm: Groesse von CGROUP4      */
#define KCODE4_LIN  (KCODE3_LIN + ((kcode3_size + 15) & ~15u))
extern unsigned kcodei_size;        /* start.asm: Groesse von CGROUPI      */
#define KCODEI_LIN  (KCODE4_LIN + ((kcode4_size + 15) & ~15u))
extern unsigned kidata_size;        /* start.asm: Groesse von CGROUPD      */
#define KIDATA_LIN  (KCODEI_LIN + ((kcodei_size + 15) & ~15u))
extern unsigned kdata_seg;          /* start.asm: Real-Mode-Segment DGROUP */
extern unsigned kdata_size;         /* start.asm: Groesse von DGROUP       */
#define KDATA_LIN   ((unsigned long)kdata_seg << 4)
#define LIN(p)      (KDATA_LIN + (unsigned short)(p))

struct desc  { unsigned short limit, base_lo; unsigned char base_hi, access; unsigned short rsv; };

#define ACC_KDATA   0x92

extern struct desc gdt[];

/* Kernel-Alias (6.0, fuer die DOS-Boxen): Nach dem Einschalten des Pagings
   sieht der Kernel den physischen Speicher ab linear KALIAS (Seitenver-
   zeichnis 512+i = i). Alle Deskriptoren (GDT, LDTs) bekommen KALIAS +
   physische Adresse; put_desc rechnet das selbst um, sobald k_alias
   gesetzt ist. Der lineare Bereich 0-4 MB gehoert damit frei der
   jeweiligen DOS-Box (eigenes erstes MB, eigenes CR3), der Kernel
   erreicht GDT, IDT, TSS, Stacks und Daten in jedem Adressraum. */
#define KALIAS      0x80000000UL
extern unsigned char k_alias;

/* 386-TSS fuer alle Tasks (CR3 je Task, V86 fuer die DOS-Boxen) */
struct tss386 {
    unsigned long link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    unsigned long eax, ecx, edx, ebx, esp, ebp, esi, edi;
    unsigned long es, cs, ss, ds, fs, gs, ldt;
    unsigned short trap, iomap;
    unsigned char  irb[32];             /* VME: INT n umleiten (alles 1 = Falle) */
    unsigned char  iob[126];            /* I/O-Bitmap Ports 0-3DFh + Ende (FFh) */
};
extern struct tss386 tss[NTASK];        /* v86.c: VGA-Ports der Box freigeben */

void set_desc(int idx, unsigned long base, unsigned limit, unsigned char acc);
void scratch_map(unsigned long base, unsigned limit);
void scratch2_map(unsigned long base, unsigned limit);
void win_save(unsigned short *b);          /* SCRATCH/SCRATCH2, 8 Worte   */
void win_restore(const unsigned short *b);

/* ---- Assembler-Routinen (start.asm) ------------------------------------ */
void          outb(unsigned port, unsigned val);
unsigned      inb(unsigned port);
unsigned long get_ticks(void);
void          idle_wait(void);
void          enable_ints(void);
void          zero_seg(unsigned sel, unsigned words);
unsigned      irq_off(void);
void          irq_restore(unsigned flags);
void          halt_forever(void);
void          cpu_reset(void);

/* ---- Start von Festplatte (preload.c, start.asm) ----------------------- */
#define PRE_ERR_FS      1           /* Startpartition nicht FAT16          */
#define PRE_ERR_READ    2           /* BIOS-Lesefehler                     */
#define PRE_ERR_FULL    3           /* Vorladebereich voll                 */
#define PRE_DRIVE       25          /* Z:, nur waehrend CONFIG.SYS         */
extern unsigned char pre_on;        /* Z: gehoert noch dem Vorladen (CONFIG.SYS) */
#define IS_PRE(d)       ((d) == PRE_DRIVE && pre_on)
extern unsigned char boot_drive;    /* BIOS-Nummer, ab 80h Festplatte      */
extern unsigned char pre_err;
extern unsigned long boot_hidden, boot_serial;
extern int npre;
extern unsigned pre_seg;            /* Ablage der vorgeladenen Dateien    */
void preload(void);                 /* nur im Real Mode                    */
int  rm_geometry(void);             /* INT 13h/08h fuer boot_drive, 0 = ok */
int  rm_read(unsigned long lba);    /* ein Sektor nach 0000:8000, 0 = ok   */

extern volatile unsigned char  fdc_irq;
extern volatile unsigned short uptime_s;
extern volatile unsigned short uptime_m;   /* Minuten seit dem Start */
extern unsigned long ent_pool[8];      /* start.asm ENTMIX, FAPI_RANDOM */
extern unsigned ent_i;
extern volatile unsigned char up60;

/* ---- Hilfen (kernel.c) ------------------------------------------------- */
void far_copy(unsigned dsel, unsigned doff, unsigned ssel, unsigned soff, unsigned n);
void delay_ticks(unsigned n);
void panic(const char *msg);
#define PN_LIN 0x120000UL               /* Absturzprotokoll (kernel.c, panic.c):
                                           hinter der Bitmap (bis 128 KB ab 1 MB),
                                           SeaBIOS und Warmstart lassen sie stehen */
unsigned __far pn_check(void);
void __far pn_boot(int d, int ses, unsigned n);
int __far pn_disk(void);
void __far pn_save(const char *msg);
extern unsigned subst_sel;              /* SUBST/JOIN-Pfade + DMESG-Ring  */
extern unsigned klog_head, klog_len;
extern unsigned char klog_mid;
#define KLOG_OFF ((2u * NDRIVE * IFS_PATHLEN + NTASK * 4u * (1 + IFS_PATHLEN) + 15) & ~15u)
void reboot(void);
/* Kernelsegment ausserhalb von DGROUP (Tabellen, Puffer), genullt;
   -> Selektor oder 0. Fuer alles, was mit der Konfiguration waechst. */
unsigned kseg_alloc(unsigned long bytes);
void reboot_request(void);          /* Strg+Alt+Entf: erst Daten sichern   */
extern volatile unsigned reboot_count;
/* k2misc.c (KTEXT2) */
void __far procfs_boot(void);          /* procfs.c: Laufwerk P:   */
unsigned long __far rtc_stamp(void);    /* Datum << 16 | Uhrzeit im FAT-Format */
int __far rtc_set(unsigned date, unsigned time);    /* 0 = ok              */
void __far rom_str(unsigned long lin, unsigned len, char __far *out, unsigned max);
struct fapi_sysinfo;
void __far sys_info2(struct fapi_sysinfo __far *si, unsigned irq_used);  /* MSD */

/* ---- Blockgeraet: Diskette A: (floppy.c) ------------------------------- */
/* floppy.c liegt in KCODE2: alle Einspruenge fern */
int  __far fd_init(unsigned cmos_types);    /* CMOS 10h; 0 = Controller da */
int  __far fd_write(int d, unsigned lba);   /* aus DMABUF in den Spurpuffer */
int  __far fd_read(int d, unsigned lba);    /* Sektor in den DMA-Puffer     */
int  __far fd_sync(void);                   /* Spurpuffer schreiben; -2 = WP */
int  __far fd_check(int d);                 /* 1 = gewechselt/neu erkannt   */
unsigned __far fd_size(int d);              /* Sektoren des Formats         */
int  __far fd_type(int d);                  /* CMOS-Typ, 0 = kein Laufwerk  */
int  __far fd_nomedia(int d);               /* 1 = keine Diskette drin      */
int  __far fd_format(int d, unsigned cyl, unsigned head, unsigned blocks);
void __far fd_tick(void);
void __far fd_motor_off(void);
unsigned kseg_dma(unsigned bytes, unsigned long *lin);  /* ohne 64-KB-Grenze */

/* Schlafen im Kernel wie DevHlp ProcBlock/ProcRun (mit gesperrten
   Interrupts rufen, kehrt mit STI zurueck; 0 geweckt, 1 Frist).
   Ereignisse des Kernels: Treibernummer FFh */
unsigned proc_block(unsigned long id, unsigned timeout);
unsigned proc_run(unsigned long id);
#define FD_EVENT    0xFF0006UL          /* Diskette, IRQ 6               */
#define SLEEP_EVENT 0xFF0000UL          /* niemand weckt: nur Frist      */

/* ---- Blockgeraete (kernel.c): Diskette und Einheiten der Treiber ------- */
#define NBLK        16              /* 6.2: 12 -> 16 (acht Loops)       */
#define BLK_FLOPPY  0               /* Geraet 0 ist immer Diskette A:      */
#define IS_FLOPPY(dev) (blk[dev].drv == -1) /* B: ist Geraet 1, wenn da   */
#define DRV_LOOP    (-2)            /* Loop-Laufwerk (loop.c), unit = Platz */
#define DRV_UFS     (-3)            /* Netzlaufwerk (ufs.c), keine Bloecke */
#define DRV_BIOS    (-4)            /* Startlaufwerk ueber INT 13h (biosdisk.c),
                                       unit = BIOS-Laufwerk (80h ...)   */
int __far bios_rw(unsigned drive, unsigned long lba, unsigned n, int wr);
int __far bios_attach(void);
#define NLOOP       8               /* 6.2: 4 -> 8                       */
struct blkdev {
    signed char    drv;             /* Treiber, -1 = Diskette im Kernel     */
    unsigned char  unit;
    unsigned long  blocks;
    unsigned       bsize;           /* 512, CD-ROM 2048                    */
    unsigned char  rem;             /* Wechselplatte (ZIP, MO): DRV_MEDIA  */
    unsigned char  grp;             /* Sperrplatz x + 1 (Treiber, Gruppe), 0 = Treiber */
};
#define IS_REM(dev) (blk[dev].rem)
extern struct blkdev blk[NBLK];
int __far cd_ra(int dev, unsigned long lba);   /* cdra.c (KTEXT4)  */
extern int nblk;
/* Loop-Laufwerke (loop.c, KTEXT2) */
extern unsigned loop_sel[NLOOP], loop_fr[2];
int __far loop_read(int dev, unsigned long lba);
int __far loop_write(int dev, unsigned long lba);
int __far loop_drop(int dev);
void __far loop_vsync(int x);
void __far loop_sync_all(void);
int __far loop_room(int dev);
/* net.c (KTEXT4): Netzwerk, Vermittlung Karte <-> Stapel (6.2) */
unsigned __far net_devhlp(unsigned fn, int d, unsigned dsel, unsigned bx, unsigned cx);
void __far net_poll(void);
unsigned __far net_fapi(unsigned usel, unsigned fn, unsigned p, unsigned n);
int __far net_inuse(int d);
void __far net_drop(int d);
unsigned __far net_srw(unsigned id, int wr, unsigned usel, unsigned off, unsigned n);
void __far net_sclose(unsigned id);
unsigned __far kf_sock_new(unsigned id);       /* kernel.c: Socket -> Handle */
unsigned __far kf_sock_id(unsigned h);
/* vdd.c (KTEXT4): Module fuer die DOS-Box (6.3, include/vdd.h) */
struct vdd_call;
unsigned __far vdd_reg(int d, unsigned dsel, unsigned bx);
unsigned __far vdd_svc(int d, unsigned dsel, unsigned bx);
void __far vdd_drop(int d);
unsigned __far vdd_io(int t, unsigned fn, unsigned port, unsigned val, int fgb);
unsigned __far vdd_int(int t, unsigned n, struct vdd_call *c);
unsigned __far vdd_cbq(int t, struct vdd_call *c);
unsigned __far ufs_fapi(unsigned usel, unsigned a1, unsigned a2, unsigned a3);
void __far ufs_task_end(int t);
struct fapi_lock;
struct ifs_file;
int  __far ufs_lock(struct ifs_file __far *f, struct fapi_lock __far *l);
int  __far ufs_lpt(int n);              /* LPTn umgeleitet (NET USE LPT1:) */
int  __far ufs_lpt_write(int n, unsigned key, unsigned sel, unsigned off, unsigned len);
void __far ufs_lpt_close(int n, unsigned key);
int  __far ufs_unc(int d, char *out, unsigned max);  /* NET-USE-Ziel */
void __far vdd_event(int t, unsigned ev, unsigned val);
void __far vdd_tick(int t, int fgb);
unsigned __far vid_hwclass(void);
extern unsigned char vid_class, vid_regs[64];
struct vdd_svc;
unsigned __far v86_noshare(int t);                    /* SHARE=OFF   */
unsigned __far v86_vsvc(int t, struct vdd_svc *k, unsigned dsel);           /* loop_attach/loop_info: ifs.h */
/* Auslagerungsdatei (swap.c, KTEXT2): ausgelagerte Seite einer Box =
   Eintrag ohne P, mit PTE_SWP und der Platznummer in Bit 12-31 */
#define PTE_SWP     0x400UL
/* reservierte DPMI-Seite (0501h): Eintrag ohne P mit PTE_LAZY, die Seite
   holt swap_pf erst beim ersten Zugriff (Windows nimmt sich allen freien
   Speicher, benutzt aber nur einen Teil) */
#define PTE_LAZY    0x800UL
extern unsigned long lazy_pg;           /* so reservierte Seiten          */
unsigned __far swap_out(unsigned n);    /* n Seiten hinaus -> geschafft */
unsigned __far swap_make_run(unsigned n);   /* n Seiten am Stueck freiraeumen */
int __far swap_pf(void);                /* Seitenfehler: 1 = eingelagert */
int __far swap_need(int t, unsigned long lp);
void __far swap_slot_free(unsigned long e);
void __far swap_info(unsigned long __far *total_kb, unsigned long __far *used_kb);
void __far swap_boot(const char __far *spec, int sys, char *msg, char *sw_path);
/* Codesegmente verwerfen und nachladen (codeswap.c, KTEXT2) */
int __far code_drop(void);
unsigned __far seg_compact(unsigned n, int cur); /* Datensegmente verschieben */
unsigned __far seg_out(unsigned n);              /* ein Datensegment auslagern */
int __far seg_drop_sw(int t, unsigned idx);      /* ausgelagert: Plaetze frei  */
int __far code_np(unsigned err);
unsigned __far code_sum(unsigned sel, unsigned n);
void __far code_note(int t, int drive, const char *path, unsigned sum);
void vol_lock(int d);                       /* Phase 6: Laufwerkssperre */
int  blk_read(int dev, unsigned long lba);  /* -> DMABUF, 0 = ok         */
int  blk_read_n(int dev, unsigned long lba, unsigned n);  /* dazu n-1 voraus */
int  blk_write(int dev, unsigned long lba); /* DMABUF ->, -2 = schreibgesch. */
void blk_flush(void);               /* Platten-Caches leeren              */
void blk_invalidate(void);          /* Diskette aus dem Cache (Wechsel)    */
int  blk_media(int dev);            /* Diskette gewechselt? 1 = ja (neu)   */
int  blk_nomedia(int dev);          /* Diskettenlaufwerk leer? (nach blk_media) */
extern unsigned char ifs_notready;  /* im FAPI-Aufruf: Laufwerk leer       */
void blk_cache_init(unsigned kb);   /* CACHE=: Platten-Cache in KB         */
unsigned long mem_avail_kb(void);  /* frei + abgebbarer Cache            */
unsigned blk_cache_kb(void);
extern unsigned char bc_sync;       /* 1: blk_write sofort (Sauber-Bit)    */

/* ---- Sitzungen und Konsole (vio.c) ------------------------------------- */
#define SROWS       23              /* Zeilen 1-23, 0 Titel, 24 Status     */
#define SCOLS       80
#define SLINES_MAX  50              /* Bildschirmzeilen hoechstens (VGA)   */
#define SBYTES      (SLINES_MAX * SCOLS * 2)    /* Puffer reicht fuer alles */

int __far vio_open(const char *title);   /* -> Sitzung oder -1                  */
void __far vio_close(int s);
void __far vio_title(int s, const char *title);
void __far vio_focus(int s);
void __far vio_next(void);
int __far vio_fg(void);
int __far vio_used(int s);
void __far vio_cls(int s);
void __far vio_tty(int s, unsigned sel, unsigned off, unsigned len);
void __far vio_str(int s, int row, int col, unsigned sel, unsigned off,
             unsigned limit, unsigned char attr);
void __far vio_kputs(int s, const char *str);     /* Kerneltext als TTY        */
void klog(unsigned sel, unsigned off, unsigned n);  /* ins Protokoll (DMESG)  */
int __far vio_ansi(int s, unsigned on);          /* ANSI an/aus, 2 = abfragen */
void __far vio_status(void);
void __far vio_flush(void);               /* Schattenpuffer -> Bildspeicher      */
int __far vio_rows(int s);               /* Zeilen der Sitzung (z. B. 23/25)    */
int __far vio_setlines(int s, int n);    /* 25/43/50 Bildschirmzeilen, 0 = fragen
                                       -> Zeilenzahl oder -1                 */
void __far vio_setmode(int s, int full, int owner);   /* owner: Task oder -1 */
void __far vio_task_end(int s, int t);    /* Vollbild des Tasks zuruecknehmen   */
void __far vio_setcur(int s, int row, int col);       /* row 0xFF = verbergen */
void __far vio_mou_reg(unsigned buttons, unsigned irq);   /* Maus angemeldet    */
void __far vio_mou_unreg(void);                           /* Maustreiber entladen */
extern unsigned char mou_kind, mou_irqn;        /* INT 33h 24h           */
void __far vio_mou_event(unsigned buttons, int dx, int dy);  /* aus dem Interrupt */
unsigned __far vio_mou_open(int s, int t);            /* -> Tasten, 0 = keine  */
int __far vio_mou_read(int s, unsigned char *e);     /* 1 = Ereignis (3 Byte) */
void __far vio_mou_show(int s, int on);
unsigned __far vio_mou_delta(int s, int *dx, int *dy);    /* DOS-Box: Mickeys */
void __far vio_mou_pos(int s, int row, int col);
void __far vio_mou_close(int s, int t);
int __far vio_paste(int take);                       /* Konsolenmaus: einfuegen */
unsigned long __far vio_buf_lin(int s);               /* VioGetBuf: Pufferbasis */
void __far vio_showbuf(int s, unsigned a, unsigned b);
void __far vio_dirty(int s);                          /* ganze Sitzung neu zeigen */
void __far vio_refont(void);                          /* Zeichensatz der Codepage laden */
/* paging.c (KTEXT2): Seitentabellen 1:1 und Alias ab KALIAS, CR0.PG */
unsigned __far paging_init(void);              /* -> Seitentabellen, 0 = aus */
unsigned __far paging_tables(void);
unsigned long __far paging_cr3(void);          /* Seitenverzeichnis des Kernels */
unsigned long __far pg_win(unsigned sel, unsigned long base, unsigned limit);
unsigned long __far pg_lfb(unsigned long phys, unsigned pages);   /* VESA */
unsigned long __far pg_mmio(unsigned long phys, unsigned pages); /* Geraeteregister */
void __far pg_mmio_free(unsigned long base, unsigned pages);
#define VB_SPLIT 0x100                  /* gmul: Fenster A schreiben, B lesen */
int __far vio_setvesa(int s, int t, struct fapi_vesamode __far *o, unsigned pages,
                      unsigned long phys, unsigned gmul);
extern unsigned win_csel;                       /* Cache-Fenster fuer pg_win */

/* v86.c (KTEXT2): DOS-Boxen (V86) */
struct v86_frame;
unsigned long __far v86_create(int t, unsigned kb);    /* -> CR3, 0 = kein Speicher */
void __far v86_copy(int t, unsigned long lin, unsigned sel, unsigned off, unsigned n);
void __far v86_xfer(int t, unsigned long lin, unsigned sel, unsigned off, unsigned n, int rd);
void __far v86_destroy(int t);
unsigned __far v86_exc(struct v86_frame *f);           /* 0 = erledigt  */
unsigned __far joy_fapi(unsigned usel, unsigned off);  /* v86.c, Gameport */
void __far v86_start(int t, int s);     /* Sitzung fest: Vollbild, B800  */
void __far v86_replay(int s);           /* Text der beendeten Box zeigen */
void __far v86_park(int t);             /* Box offen halten: Vollbild weg */
extern unsigned long v86_pages;         /* Seiten aller DOS-Boxen (MEM)   */
extern unsigned char dos_high, dos_umb; /* DOS=HIGH,UMB (CONFIG.SYS)      */
void __far v86_poll(void);              /* Scheduler: Bildschirm geaendert? */
void __far v86_key(unsigned sc);        /* kbd_irq: Scancode             */
struct v86_iret;
void __far v86_tick(struct v86_iret *r);    /* irq0 aus der Box          */
int box_ctl(unsigned op, unsigned a);                  /* kernel.c, fuer v86.c */
#define BOX_AUD_FM  0xFF                /* box_aud: Box nimmt (1)/gibt (0) den OPL */
#define BOX_AUD_FMQ 0xFE                /* box_aud: wie viele FAPI-Programme haben FM offen? */
#define BOX_TASK    0                   /* -> current                   */
#define BOX_SESSION 1                   /* -> Sitzung von current        */
#define BOX_SLEEP   2                   /* a Takte schlafen              */
#define BOX_EXIT    3                   /* Box beenden, Rueckgabewert a  */
#define BOX_READ    4                   /* Sektor a des Startabbilds nach box_sec, 0 = ok */
#define BOX_ARGS    5                   /* -> Befehlszeile (near, DS)    */
#define BOX_DRIVES  6                   /* a=0: Laufwerke A-P, 1: Q-Z (Bitmaske) */
#define BOX_LOCK    7                   /* a=1 Kernelsperre nehmen, 0 abgeben */
#define BOX_SYSDRV  8                   /* -> Startlaufwerk (0 = A:)       */
#define BOX_PARK    9                   /* a = Rueckgabewert: parken/beenden */
#define BOX_STDRED  10                  /* -> Bit i: Handle i umgeleitet (nicht Konsole) */
unsigned box_fapi(unsigned fn, unsigned usel, unsigned a1, unsigned a2, unsigned a3);
extern unsigned char box_sec[512];
void __far v86_disk(int t, unsigned spt, unsigned heads, unsigned secs);
int __far v86_fgbox(void);              /* Vordergrund ist eine Box mit DOS */

/* bus.c (KTEXT2): PCI und Belegungsliste */
struct fapi_pcidev;
unsigned __far pci_init(void);                 /* -> Anzahl PCI-Geraete, 0 = kein PCI */
unsigned long __far pci_read(unsigned loc, unsigned reg);
void __far pci_write(unsigned loc, unsigned reg, unsigned long v, unsigned size);
unsigned __far pci_find(unsigned vendor, unsigned device, unsigned n);
unsigned __far pci_findclass(unsigned cs, unsigned n);
int __far pci_info(unsigned i, struct fapi_pcidev __far *p);
/* pnp.c (KTEXT2): ISA Plug and Play beim Start */
struct fapi_pnpdev;
unsigned __far pnp_init(void);                 /* -> Anzahl logischer Geraete */
int __far pnp_info(unsigned i, struct fapi_pnpdev __far *p);
void __far pnp_line(unsigned i, char __far *buf);  /* Startmeldung       */
/* vesa.c (KTEXT2): VBE ueber einen kurzen Real-Mode-Aufruf */
unsigned __far apm_ver(void);               /* vesa.c: APM-Version (12h = 1.2) */
int __far apm_off(void);                   /* vesa.c: APM aus, nur bei Fehler zurueck */
int __far acpi_off(void);                  /* vesa.c: ACPI S5, nur bei Fehler zurueck */
/* SVGA-Zustand der DOS-Boxen (vesa.c, fuer vgagfx.c) */
struct vbox { unsigned long bytes, phys; unsigned gmul; unsigned char split; };
int __far vesa_boxinfo(unsigned m, struct vbox __far *o);
void __far vio_raw_mode(int s, unsigned m);    /* vgagfx.c: BIOS-Modus der Box */
unsigned __far vesa_boxq(unsigned ax, unsigned cx, unsigned long sig,
                         unsigned char __far *out);    /* VBE-Abfrage einer Box */
unsigned __far vesa_state(unsigned op, unsigned long lin);
unsigned __far vesa_getbank(unsigned w);
void __far vesa_setwin(unsigned w, unsigned n);
unsigned __far vesa_fapi(int s, int t, unsigned sub, unsigned n,
                         struct fapi_vesamode __far *o);
unsigned __far res_claim(int drv, unsigned type, unsigned start, unsigned len);
void __far res_free(int drv, unsigned type, unsigned start);
int __far res_get(unsigned i, unsigned *type, unsigned *start, unsigned *len);

int __far vio_setgfx(int s, int t, unsigned mode);   /* 13h / 0 = Text        */
unsigned long __far vio_gfx_base(int s);
unsigned __far vio_gfx_limit(int s);
int __far vio_setplane(int s, unsigned p);
int __far vio_setpage(int s, unsigned p);
void __far vio_idle(void);
int __far vio_gfx_reg(int s, int t, unsigned sel);
int __far vio_is_gfx(int s);
void __far vio_setpal(int s, unsigned first, unsigned n, unsigned sel, unsigned off);
void ldt_set(int t, unsigned sel, unsigned long base, unsigned limit);
void ldt_drop(int t, unsigned sel);
unsigned __far vio_clip_put(unsigned sel, unsigned off, unsigned len);
unsigned __far vio_clip_get(unsigned sel, unsigned off, unsigned max);

/* ---- Tastatur (kbd.c) -------------------------------------------------- */
void kbd_init(void);
extern unsigned char bios_kbf;       /* 0040:0017 beim Start (start.asm) */
extern unsigned char boot_f8;        /* 1 = F8: CONFIG.SYS schrittweise   */
extern unsigned char boot_f5;        /* 1 = F5: Notstart (docs/NOTSTART.md) */
void __far kinit_done(void);        /* k2misc.c: ITEXT freigeben       */
extern unsigned kinit_pg;           /* davon freigegebene Seiten (MEM)  */
void __far cfg_wait(void);           /* cfgboot.c (KTEXT4): CONFIG.SYS-Start */
int  __far cfg_skip(const char *l, int pass);
void __far cfg_step(void);
void __far cfg_pnp(int size);
int  kbd_get(void);                 /* Zeichen | Scancode << 8, -1 = leer  */
int  kbd_peek(void);
unsigned kbd_rate(unsigned v);      /* Tastatur F3h, FFFFh = abfragen      */
unsigned kbd_shift(void);           /* KBDSHIFT_*                          */
void kbd_flush(void);
int  kbd_set_layout(unsigned sel, unsigned off);   /* fehlende Zeichen, -1 */
int  kbd_derive(void);                  /* nach CHCP                        */
extern unsigned kbd_sel;                /* Belegung (Kennung bei Offset 4)  */
unsigned __far cp_from_uni(unsigned u); /* fat.c: Unicode -> aktive Codepage */
unsigned __far cp437_uni(unsigned c);   /* fat.c: CP437 -> Unicode          */
extern volatile unsigned char kbd_switch_req, kbd_break_req, kbd_tm_req;

#endif
