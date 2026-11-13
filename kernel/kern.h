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
#define NDRV        6               /* Geraetetreiber und IFS-Module      */
#define DRVC_IDX(i) (9 + 3 * NTASK + 2 * (i))   /* Treibercode, Ring 0    */
#define DRVC_SEL(i) (DRVC_IDX(i) * 8)
#define DRVD_SEL(i) (DRVC_SEL(i) + 8)           /* Treiberdaten           */
#define NKSEG       20              /* eigene Kernelsegmente (kseg_alloc)  */
#define KSEG_IDX(i) (9 + 3 * NTASK + 2 * NDRV + (i))
#define KCODE2_IDX  (9 + 3 * NTASK + 2 * NDRV + NKSEG)
#define KCODE2      0x208           /* = KCODE2_IDX * 8; build.sh traegt ihn ein */
#define VWIN_IDX    (KCODE2_IDX + 1) /* Fenster in den Speicher einer DOS-Box */
#define VWIN        (VWIN_IDX * 8)  /* (Basis roh, ohne KALIAS; v86.c)     */
#define RWIN_IDX(t) (KCODE2_IDX + 2 + (t))   /* Fenster je Box (redir.c) */
#define RWIN_SEL(t) (RWIN_IDX(t) * 8)
#define KCODE3_IDX  (KCODE2_IDX + 2 + NTASK)  /* drittes Codesegment (v86.c, redir.c) */
#define KCODE3      0x258           /* = KCODE3_IDX * 8; build.sh traegt ihn ein */
#define FCACHE_IDX  (KCODE3_IDX + 1) /* FAT-Cache (fat.c), fest bei FCACHE_LIN */
#define FCACHE_SEL  (FCACHE_IDX * 8)
#define CTXWIN_IDX  (KCODE3_IDX + 2) /* Ablage des Kontexts je Task (Phase 6) */
#define CTXWIN      (CTXWIN_IDX * 8)
#define WBUF_IDX    (KCODE3_IDX + 3) /* Schreibpuffer je Task (bc_wb, Phase 6) */
#define WBUF        (WBUF_IDX * 8)
#define NGDT        (KCODE3_IDX + 4)
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
extern const struct ctxblk fat_ctx[], redir_ctx[];
extern const unsigned fat_nctx, redir_nctx;

/* Kernel: Code ab 10000h, Daten ab dem naechsten Paragraphen dahinter */
#define KCODE_LIN   0x10000UL
extern unsigned kcode_size;         /* start.asm: Groesse von CGROUP       */
extern unsigned kcode2_size;        /* start.asm: Groesse von CGROUP2      */
#define KCODE2_LIN  (KCODE_LIN + ((kcode_size + 15) & ~15u))
extern unsigned kcode3_size;        /* start.asm: Groesse von CGROUP3      */
#define KCODE3_LIN  (KCODE2_LIN + ((kcode2_size + 15) & ~15u))
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
extern volatile unsigned char up60;

/* ---- Hilfen (kernel.c) ------------------------------------------------- */
void far_copy(unsigned dsel, unsigned doff, unsigned ssel, unsigned soff, unsigned n);
void delay_ticks(unsigned n);
void panic(const char *msg);
void reboot(void);
/* Kernelsegment ausserhalb von DGROUP (Tabellen, Puffer), genullt;
   -> Selektor oder 0. Fuer alles, was mit der Konfiguration waechst. */
unsigned kseg_alloc(unsigned long bytes);
void reboot_request(void);          /* Strg+Alt+Entf: erst Daten sichern   */
extern volatile unsigned reboot_count;
/* k2misc.c (KTEXT2) */
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
#define NBLK        8
#define BLK_FLOPPY  0               /* Geraet 0 ist immer Diskette A:      */
#define IS_FLOPPY(dev) (blk[dev].drv < 0)   /* B: ist Geraet 1, wenn da   */
struct blkdev {
    signed char    drv;             /* Treiber, -1 = Diskette im Kernel     */
    unsigned char  unit;
    unsigned long  blocks;
    unsigned       bsize;           /* 512, CD-ROM 2048                    */
    unsigned char  rem;             /* Wechselplatte (ZIP, MO): DRV_MEDIA  */
    unsigned char  grp;             /* Sperrgruppe im Treiber (IDE-Kanal)  */
};
#define IS_REM(dev) (blk[dev].rem)
extern struct blkdev blk[NBLK];
extern int nblk;
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

int  vio_open(const char *title);   /* -> Sitzung oder -1                  */
void vio_close(int s);
void vio_title(int s, const char *title);
void vio_focus(int s);
void vio_next(void);
int  vio_fg(void);
int  vio_used(int s);
void vio_cls(int s);
void vio_tty(int s, unsigned sel, unsigned off, unsigned len);
void vio_str(int s, int row, int col, unsigned sel, unsigned off,
             unsigned limit, unsigned char attr);
void vio_kputs(int s, const char *str);     /* Kerneltext als TTY        */
int  vio_ansi(int s, unsigned on);          /* ANSI an/aus, 2 = abfragen */
void vio_status(void);
void vio_flush(void);               /* Schattenpuffer -> Bildspeicher      */
int  vio_rows(int s);               /* Zeilen der Sitzung (z. B. 23/25)    */
int  vio_setlines(int s, int n);    /* 25/43/50 Bildschirmzeilen, 0 = fragen
                                       -> Zeilenzahl oder -1                 */
void vio_setmode(int s, int full, int owner);   /* owner: Task oder -1 */
void vio_task_end(int s, int t);    /* Vollbild des Tasks zuruecknehmen   */
void vio_setcur(int s, int row, int col);       /* row 0xFF = verbergen */
void vio_mou_reg(unsigned buttons, unsigned irq);   /* Maus angemeldet    */
extern unsigned char mou_kind, mou_irqn;        /* INT 33h 24h           */
void vio_mou_event(unsigned buttons, int dx, int dy);  /* aus dem Interrupt */
unsigned vio_mou_open(int s, int t);            /* -> Tasten, 0 = keine  */
int  vio_mou_read(int s, unsigned char *e);     /* 1 = Ereignis (3 Byte) */
void vio_mou_show(int s, int on);
unsigned vio_mou_delta(int s, int *dx, int *dy);    /* DOS-Box: Mickeys */
void vio_mou_pos(int s, int row, int col);
void vio_mou_close(int s, int t);
int  vio_paste(int take);                       /* Konsolenmaus: einfuegen */
unsigned long vio_buf_lin(int s);               /* VioGetBuf: Pufferbasis */
void vio_showbuf(int s, unsigned a, unsigned b);
void vio_dirty(int s);                          /* ganze Sitzung neu zeigen */
void vio_refont(void);                          /* Zeichensatz der Codepage laden */
/* paging.c (KTEXT2): Seitentabellen 1:1 und Alias ab KALIAS, CR0.PG */
unsigned __far paging_init(void);              /* -> Seitentabellen, 0 = aus */
unsigned __far paging_tables(void);
unsigned long __far paging_cr3(void);          /* Seitenverzeichnis des Kernels */
unsigned long __far pg_win(unsigned sel, unsigned long base, unsigned limit);
unsigned long __far pg_lfb(unsigned long phys, unsigned pages);   /* VESA */
#define VB_SPLIT 0x100                  /* gmul: Fenster A schreiben, B lesen */
int __far vio_setvesa(int s, int t, struct fapi_vesamode __far *o, unsigned pages,
                      unsigned long phys, unsigned gmul);
extern unsigned win_csel;                       /* Cache-Fenster fuer pg_win */

/* v86.c (KTEXT2): DOS-Boxen (V86) */
struct v86_frame;
unsigned long __far v86_create(int t, unsigned kb);    /* -> CR3, 0 = kein Speicher */
void __far v86_copy(int t, unsigned long lin, unsigned sel, unsigned off, unsigned n);
void __far v86_destroy(int t);
unsigned __far v86_exc(struct v86_frame *f);           /* 0 = erledigt  */
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
/* SVGA-Zustand der DOS-Boxen (vesa.c, fuer vgagfx.c) */
struct vbox { unsigned long bytes, phys; unsigned gmul; unsigned char split; };
int __far vesa_boxinfo(unsigned m, struct vbox __far *o);
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
unsigned vio_clip_put(unsigned sel, unsigned off, unsigned len);
unsigned vio_clip_get(unsigned sel, unsigned off, unsigned max);

/* ---- Tastatur (kbd.c) -------------------------------------------------- */
void kbd_init(void);
int  kbd_get(void);                 /* Zeichen | Scancode << 8, -1 = leer  */
int  kbd_peek(void);
unsigned kbd_shift(void);           /* KBDSHIFT_*                          */
void kbd_flush(void);
int  kbd_set_layout(unsigned sel, unsigned off);   /* fehlende Zeichen, -1 */
int  kbd_derive(void);                  /* nach CHCP                        */
extern unsigned kbd_sel;                /* Belegung (Kennung bei Offset 4)  */
unsigned __far cp_from_uni(unsigned u); /* fat.c: Unicode -> aktive Codepage */
unsigned __far cp437_uni(unsigned c);   /* fat.c: CP437 -> Unicode          */
extern volatile unsigned char kbd_switch_req, kbd_break_req, kbd_tm_req;

#endif
