/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - DOS-Boxen (V86), Phase 3 Stufe 1 und 2
 *
 * Eine Box ist ein Task, dessen "Ring 3" der V86-Modus des 386 ist. Sie
 * hat ein eigenes Seitenverzeichnis (CR3 im 386-TSS): linear 0-4 MB ist
 * ihr eigenes erstes MB, ab 80000000h liegt wie ueberall der Kernel-Alias
 * (paging.c). Seitentabelle der Box:
 *   0 .. kb/4-1     eigene Seiten (konventioneller Speicher)
 *   A0000-B7FFF     eine Ersatzseite (Grafik sieht man noch nicht)
 *   B8000-B9FFF     der Puffer ihrer Sitzung (Textbild 80x25, direkt!)
 *   BA000-BFFFF     die Ersatzseite
 *   C0000-EFFFF     ROM 1:1, nur lesen
 *   F0000-F0FFF     eigene Seite: "BIOS" der Box (Dienst-Stummel)
 *   F1000-FFFFF     ROM 1:1, nur lesen
 *   100000-10FFFF   wie 0-FFFF (A20 aus: Umlauf wie beim echten 8086)
 *
 * Bildschirm (Stufe 2): die Box laeuft im Vollbild ihrer Sitzung, ihr
 * B800 ist der Sitzungspuffer selbst - Schreiben kostet keinen Trap. Ob
 * sich etwas geaendert hat, sagt das Dirty-Bit der beiden Seiten
 * (v86_poll im Scheduler: loeschen, Sitzung neu zeichnen lassen; die CPU
 * setzt es nach dem naechsten CR3-Laden wieder). Endet die Box in der
 * Sitzung ihres Aufrufers, bleibt ihr Text stehen (v86_replay, wie NT).
 *
 * Monitor: Die Box laeuft mit IOPL 0. CLI, STI, PUSHF, POPF, INT n, IRET,
 * HLT und jeder Portzugriff (kein I/O-Bitmap) loesen #GP aus; v86_exc
 * fuehrt sie nach (virtuelles IF in vfl). INT n springt wie im Real Mode
 * ueber die Vektortabelle der Box. Anfangs zeigt jeder Vektor n auf
 * F000:n*4, dort steht "ARPL n; IRET": ARPL gibt es im V86-Modus nicht
 * (#UD), der Monitor fuehrt Dienst n aus (wie die BIOS-Haken bei
 * Windows 386). Zeigt ein Vektor noch dorthin, wird der Dienst bei INT n
 * gleich ausgefuehrt, ohne Umweg ueber den Stack der Box. INT 8 hat einen
 * eigenen Stummel bei F000:0400 ("ARPL 8; INT 1Ch; IRET").
 *
 * Virtuelle Interrupts (Stufe 2): Takt 18,2 Hz aus dem 100-Hz-Takt des
 * Kernels (vt_acc), Tastatur aus den Scancodes von kbd_irq (v86_key).
 * Zugestellt wird beim Ruecksprung in die Box (v86_inject: am Ende des
 * Monitors und aus irq0), nur bei virtuellem IF, nicht maskiert (vimr)
 * und nicht schon in Bearbeitung (visr, geloescht durch EOI an Port 20h
 * oder den Standard-Stummel). Solange INT 8 und 1Ch nicht umgebogen sind,
 * zaehlt der Kernel nur den Takt im BIOS-Datenbereich weiter; INT 9 wird
 * nur zugestellt, wenn das Programm ihn umgebogen hat (sonst liest es
 * Tasten ueber INT 16h).
 *
 * Laeuft im dritten Codesegment (KTEXT3), im Kontext des Box-Tasks: dann
 * ist das CR3 der Box geladen und ihr Speicher linear 0-10FFEFh ueber das
 * rohe Fenster VWIN erreichbar.
 */
#include "k2.h"

#define PTE_P   1
#define PTE_W   2
#define PTE_U   4
#define PTE_D   0x40            /* Dirty: die CPU hat hineingeschrieben  */
#define PTE_BOX (PTE_P | PTE_W | PTE_U)
#define PTE_ROM (PTE_P | PTE_U)

#define FL_TF   0x0100
#define FL_IF   0x0200
#define FL_V    0x7200          /* IF, IOPL, NT: gehoeren der Box (vfl)  */
#define FL_SET  0x0DD5          /* CF PF AF ZF SF TF DF OF: echt         */

#define VBIOS   0xF000          /* Segment der Dienst-Stummel            */
#define INT8_STUB 0x0400        /* ARPL 8; INT 1Ch; IRET                 */
#define DPT_OFF 0x0800          /* Diskettenparametertabelle             */
#define XMS_STUB 0x0500         /* XMS-Einsprung: ARPL E1h; RETF          */
#define EMS_RET  0x0560         /* EMS 56h zurueck: ARPL E6h; RETF        */
#define XMS_TAB 0x0900          /* Handle-Tabelle wie HIMEM (INT 2Fh 4309h) */
#define CFG_OFF 0x0890          /* Konfigurationstabelle INT 15h C0h     */
#define PTE_OWN 0x200           /* eigene Seite (EMB/UMB): bei Ende frei  */
#define BDA     0x0040
#define VSEG    0xB800
#define ROWS    25
#define COLS    80

#include "v86int.h"
#include "../include/fapi_abi.h"
#include "../include/vdd.h"

/* Je Falle gebraucht - direkt statt ueber Fern-Thunks (gleiches DS):
   auf dem 486 trappt jedes CLI/STI einer Box, tausende Male je Sekunde */
extern volatile unsigned long ticks;    /* start.asm                     */
extern int fg;                          /* vio.c: Vordergrund-Sitzung    */
extern int current;                     /* kernel.c: laufender Task      */
static unsigned long k3_ticks(void)
{
    unsigned f = irq_off();
    unsigned long v = ticks;
    irq_restore(f);
    return v;
}
#undef get_ticks
#define get_ticks()     k3_ticks()

pg_t pd_box[NTASK], pt_box[NTASK];   /* auch swap.c */
static unsigned char wtr_n[NTASK];     /* Messung Windows: Zeilen je Box */
static unsigned short vfl[NTASK];       /* IF/IOPL/NT der Box            */
static signed char box_ses[NTASK] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static unsigned long vt_last[NTASK];    /* Kerneltakt der letzten Abrechnung */
static unsigned vt_acc[NTASK];          /* Tausendstel eines BIOS-Takts  */
static unsigned vt_pend[NTASK];         /* fehlende BIOS-Takte           */
static unsigned char visr[NTASK], vimr[NTASK];  /* virtueller PIC: Bit 0 Takt, 1 Tastatur */
static unsigned char kq[NTASK][16], kq_h[NTASK], kq_t[NTASK];   /* Scancodes */
static unsigned char vcur[NTASK];       /* zuletzt an Port 60h           */
static unsigned char vhide[NTASK];      /* Cursor aus (INT 10h 01h)      */
static unsigned char crt_idx[NTASK], pit_ff[NTASK], tgl[NTASK];
static unsigned char cmos_idx[NTASK];
static unsigned shf_last[NTASK];         /* zuletzt nach 0417h gelegt    */
static unsigned char vimr2[NTASK];       /* Maske des zweiten PIC (A1h)   */
static unsigned char picw[NTASK][2];     /* noch erwartete ICW2-4 je PIC  */
static unsigned char pic_isr[NTASK][2];  /* OCW3: Lesen liefert ISR statt IRR */
static unsigned alrm[NTASK];            /* INT 1Ah 06h: Weckzeit (DOS-Zeit) */
static unsigned char alrm_on[NTASK];     /* 1 gesetzt, 2 erst nach Mitternacht */
static unsigned alrm_tk[NTASK];          /* zuletzt geprueft (Kerneltakt)  */
static unsigned char cmos_ov[NTASK][128], cmos_om[NTASK][16];  /* geschriebene
                                           CMOS-Bytes (nur fuer die Box)    */    /* Port 70h der Box              */
static unsigned char noblink[NTASK];     /* INT 10h 1003h: Hintergrund hell */
static unsigned pit_c[NTASK];          /* (alt, ungenutzt)              */
/* virtueller PIT: Kanal 0 mit der Rate des Programms, Zaehler nach
   echter Zeit (in PIT-Schritten, 1,193 MHz) */
static unsigned pit_rl[NTASK];          /* Teiler Kanal 0, 0 = 65536     */
static unsigned pit_cnt[NTASK];         /* laufender Zaehler Kanal 0     */
static unsigned pit_lat[NTASK];         /* gelatchter Wert               */
static unsigned char pit_acc_m[NTASK];  /* Zugriff 1 lo, 2 hi, 3 lo/hi   */
static unsigned char pit_latd[NTASK];   /* Latch gueltig                 */
static unsigned char pit_wlo[NTASK];    /* lo/hi: lo schon geschrieben   */
static unsigned long pit_acc[NTASK];
static unsigned pit_n0[NTASK];          /* irq0_n bei der letzten Messung */
static unsigned pit_lastc;              /* zuletzt gelesener echter Zaehler */    /* Schritte bis zum naechsten INT 8 */
static unsigned long pit_t0[NTASK];     /* Takt der letzten Messung      */
static unsigned pit_c0[NTASK];          /* Schritte seit dem Kerneltakt dabei */
unsigned kpit_div, kpit_acc;            /* start.asm irq0: PIT-Teiler (0 = 11932) */
static unsigned char spk[NTASK];        /* Lautsprecher-Bits 61h (0-1)   */
static unsigned img_spt[NTASK], img_heads[NTASK], img_secs[NTASK];  /* Startabbild */
/* Hilfen der Nachbildung, je Programm abschaltbar (DOSBOX.INI, INT E0h
   18h): gesetztes Bit = aus; 0 = alle an (Vorgabe) */
#define HK_POPF   1                     /* POPF_PATCH:  POPF -> HLT      */
#define HK_IDLE   2                     /* SB_IRQ_IDLE: ruhende Leitung  */
#define HK_WDOG   4                     /* IF_WATCHDOG: IF nach 3 Takten */
static unsigned char box_hk[NTASK];
static unsigned xms_lim[NTASK];         /* XMS-Grenze in Seiten + 1 (0 = Vorgabe) */
static unsigned char idle_n[NTASK];     /* Abfragen im laufenden Kerneltakt */
static unsigned char idle_sens[NTASK];  /* IDLE_SENSITIVITY + 1, 0 = Vorgabe */
static unsigned idle_tk[NTASK];         /* Takt, zu dem idle_n zaehlt      */

unsigned long v86_pages;                /* Seiten aller Boxen (MEM)      */
/* reicht der Speicher nicht (auch nicht nach dem Cache), lagert swap_out
   selten benutzte Seiten der Boxen aus (Auslagerungsdatei) */
static pg_t vp_alloc(void)
{
    pg_t p = mem_alloc(1);
    if (!p && swap_out(16))
        p = mem_alloc(1);
    if (p)
        v86_pages++;
    return p;
}
static pg_t vp_alloc_hi(void)           /* Box-Speicher: auch ueber 2 GB */
{
    pg_t p = mem_alloc_hi(1);
    if (!p && swap_out(16))
        p = mem_alloc_hi(1);
    if (p)
        v86_pages++;
    return p;
}
static void vp_free(pg_t p)
{
    mem_free(p, 1);
    v86_pages--;
}
static void pte_drop(unsigned long e)   /* Seite aus einem Eintrag frei,   */
{                                       /* auch ausgelagert               */
    if (e & PTE_P)
        vp_free(e >> 12);
    else if (e & PTE_SWP)
        swap_slot_free(e);
}

static void xms_end(int t);
static void vga_freeze(int t);
/* Module (VSB.SYS u. a.): virtuelle IRQ-Leitungen, Takt, Hardware vorn */
static unsigned char virq[NTASK];       /* Leitungen 2-7 warten (Bit n)  */
static unsigned char vidle[NTASK];      /* Leitung ruht: kein Handler     */
static unsigned char vtk[NTASK];        /* Bit 0 Takt, 1 auch gleich      */
static unsigned vtl[NTASK];             /* Kerneltakt des letzten Takts   */
static unsigned char vfgr[NTASK];       /* Modul gab Hardware (vorn)     */
static void lwin(unsigned idx, unsigned long lin);
static void kpit_set(unsigned dv);
static void hlt_wait(int t);
static unsigned long key_t0[NTASK];     /* Takt der letzten zugestellten Taste */
/* naechster Scancode darf jetzt kommen? Ein Loslassen-Code fruehestens
   3 Takte (30 ms) nach dem vorigen Code: Spiele fragen den Tastenzustand
   je Spieltakt ab (DOOM 35 Hz) - kamen Druecken und Loslassen zusammen,
   sah DOOM die Taste nie (Leertaste, Strg). Wie der PIC: IRQ 1 nicht,
   solange IRQ 0 bedient wird. */
static int key_ready(int t)
{
    if (kq_h[t] == kq_t[t] || (visr[t] & 3) || (vimr[t] & 2))
        return 0;
    if ((kq[t][kq_t[t]] & 0x80) && kq[t][kq_t[t]] != 0xE0 &&
        get_ticks() - key_t0[t] < 3)
        return 0;
    return 1;
}
static void rom_uncow(int t);
static unsigned char from_tick;
/* STI-Schatten: die CPU nimmt Interrupts erst nach dem Befehl hinter STI
   an. Digger biegt INT 9 um und merkt sich den alten Vektor erst nach
   "STI; POP DS" - eine beim STI sofort zugestellte Taste sprang ueber
   den leeren Zeiger ins Nichts (#UD bei 0000:000B, auf dem P2). Nach
   einem STI, das IF einschaltet, kommt der Interrupt daher erst mit der
   naechsten Falle oder dem naechsten Takt. */
static unsigned char sti_sh;
/* Wartet beim STI ein Interrupt, laeuft der naechste Befehl im
   Einzelschritt (TF); dessen #DB stellt ihn zu - genau ein Befehl
   spaeter wie bei der CPU. Ohne das wartete er bis zur naechsten
   Falle mit IF an: in Diggers Schleife "CLI; Zaehler lesen; STI"
   kamen die Takte unregelmaessig, der Ton klang verzerrt. */
static unsigned char sti_step[NTASK];
static unsigned char k9_def[NTASK];     /* INT 9 stand zuletzt auf dem Stummel */
static unsigned t8_last[NTASK];         /* irq0_n bei der letzten Zustellung */
extern volatile unsigned irq0_n;        /* start.asm: zaehlt jeden irq0  */
/* Hardware-Vektoren beim Programmstart (INT E0h 00h): bricht ein Spiel
   ab (Strg+C), ohne INT 8/9 zurueckzunehmen, zeigen sie in freigegebenen
   Speicher - die offen gehaltene Box stellt sie beim Parken wieder her */
static const unsigned char hwvec[] = { 0x08, 0x09, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x1C,
                                       0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77 };
static unsigned long ivt_save[NTASK][sizeof(hwvec)];    /* Kerneltakt des letzten INT 8 der Box */         /* Zustellung aus irq0 (Takt erlaubt) */
static int pm_hooked(int t, unsigned n);
static unsigned pm_hk[NTASK];           /* PM-Vektor 0-15 eingehaengt (Bit n) */
static void dp_hwint(struct v86_iret *r, int t);
static void pvi_in(struct v86_iret *r, int t);
static void vme_in(struct v86_iret *r, int t);
static unsigned char b2f[NTASK];         /* BOXSHELL hat INT 2Fh eingehaengt */
static unsigned char box_cm[NTASK];      /* eigener Kern ATDOSCM.SYS laeuft */
static unsigned char vme_on[NTASK];     /* VIF gilt (letzter Ruecksprung mit VME) */
static void vme_out(struct v86_iret *r, int t);
static void pvi_out(struct v86_iret *r, int t);
static int box_fg(int t);
static void blink_hw(int off);
static void spk_hw(int t, int on);
static void spk_ports(int t, int on);
static void vdd_bg(int t);
static void vdd_tk(int t);
static pg_t own_page(void);
static unsigned char spk_open[NTASK];
static unsigned char joy_open[NTASK];   /* Gameport 201h direkt          */
static void vga_leave(int t);
static void vga_svmap(int t, int bg);
static void tlb(int t);
static unsigned long pte_get(int t, unsigned i);
static void pte_set(int t, unsigned i, unsigned long e);
static unsigned char umb_ok;
static void umb_scan(void);
static unsigned char vga_raw[NTASK];
static unsigned char vga_bgk[NTASK];            /* hinten auf Schatten (Art) */
static void dpmi_end(int t);
static unsigned char dp_inirq[NTASK];
static unsigned long dp_cli_t[NTASK];   /* Kerneltakt des letzten CLI im PM */
#define NPATCH 24
static unsigned long dp_patch[NTASK][NPATCH];   /* POPF -> HLT (linear)  */
#define NCLID 16                        /* untersuchte CLI-Stellen       */
static unsigned long cli_done[NCLID];
static unsigned char cli_nd;
static signed char cli_dt = -1;         /* fuer diese Box                */
static unsigned char dp_npatch[NTASK], dp_popf_if[NTASK];
static int dp_patched(int t, unsigned long lin);
static void xms_table(int t);
static unsigned xms_snap[NTASK];        /* CMOS 30h/31h zuletzt: Seiten + 1 */
static int mon2f(struct v86_frame *f);
static unsigned emb_free_pg(int t);

/* PVI (CR4 Bit 1, ab Pentium): CLI/STI eines DPMI-Programms in Ring 3
   setzen nur EFLAGS.VIF statt zu trappen (DMX mischt mit CLI/STI, ~700
   Fallen je s). Das virtuelle IF steht dann in VIF: pvi_in holt es beim
   Eintritt in den Kernel nach vfl, pvi_out schreibt es zurueck und setzt
   VIP, wenn ein Interrupt wartet (das naechste STI trappt dann). CR4
   gilt fuer alle: pvi_sw (start.asm) schaltet es beim Taskwechsel nach
   pvi_t[t] um, FAPI-Programme behalten ihr #GP bei CLI. POPF aendert VIF
   nicht - wie ohne PVI das IF; den POPF-Patch findet nur ein CLI, das
   trappt: feuert der Wachhund, laeuft die Box 1 s ohne PVI (pvi_back). */
#define FL_VIF  0x80000UL
#define FL_VIP  0x100000UL
#define HK_PVI  8
#define HK_ADLIB 16                     /* ADLIB_COPY=ON (Bit = an!)     */
#define HK_HWT   32                     /* HW_TIMER=ON (Bit = an!)       */
#define CR4_VME 1
#define CR4_PVI 2
unsigned char pvi_t[NTASK], pvi_cur;    /* je Task / gerade in CR4       */
static signed char pvi_ok = -1;         /* CPU kann PVI (CPUID 1: VME)   */
static unsigned long pvi_back[NTASK];   /* PVI erst wieder ab diesem Takt */
static unsigned long cpu_flid(void);    /* EFLAGS.ID kippbar -> != 0     */
#pragma aux cpu_flid = "pushfd" "pop eax" "mov ecx,eax" "xor eax,200000h" \
    "push eax" "popfd" "pushfd" "pop eax" "push ecx" "popfd" "xor eax,ecx" \
    "mov edx,eax" "shr edx,16" __value [__dx __ax] __modify [__ax __cx __dx];
static unsigned long cpu_feat(void);    /* CPUID 1: EDX                  */
#pragma aux cpu_feat = "mov eax,1" 0x0F 0xA2 "mov eax,edx" "shr edx,16" \
    __value [__dx __ax] __modify [__ax __bx __cx __dx];
static void k2_cr4(unsigned on);
#pragma aux k2_cr4 = 0x0F 0x20 0xE1 "and cl,0FCh" "or cl,al" \
    0x0F 0x22 0xE1 __parm [__ax] __modify [__cx];
static unsigned dp_switch(struct v86_frame *f, int t);
static unsigned dp_back(struct v86_frame *f, int t);
unsigned __far redir_psp(int t);
static void ems_pick_frame(void);
static unsigned char ems_frame;         /* erste EMS-Rahmenseite, 0 = keiner */
unsigned char ems_nf[NTASK];            /* diese Box ohne Rahmen (DOSBOX.INI,
                                           setzt load_img in kernel.c)   */
#define EFR(t) (ems_nf[t] ? 0 : ems_frame)
static unsigned ems_lim[NTASK];         /* EMS: logische Seiten + 1, 0 = Vorgabe */


/* Thunks (erstes Codesegment) */
unsigned __far kf_box_fapi(unsigned fn, unsigned usel, unsigned a1, unsigned a2, unsigned a3);
int  __far kf_box_ctl(unsigned op, unsigned a);
int  __far kf_vio_fg(void);
int  __far kf_kbd_get(void);
int  __far kf_kbd_peek(void);
unsigned __far kf_kbd_shift(void);
void __far kf_kbd_flush(void);
unsigned __far kf_kbd_rate(unsigned v);
void __far kf_vio_tty(int s, unsigned sel, unsigned off, unsigned len);
void __far kf_vio_setmode(int s, int full, int owner);
void __far kf_vio_setcur(int s, int row, int col);
void __far kf_vio_dirty(int s);
unsigned long __far kf_vio_buf_lin(int s);
unsigned long __far kf_mem_avail_kb(void);
unsigned __far kf_vio_mou_open(int s, int t);
int __far kf_vio_mou_read(int s, unsigned char *e);
void __far kf_vio_mou_show(int s, int on);
unsigned __far kf_vio_mou_delta(int s, int *dx, int *dy);
void __far kf_vio_mou_pos(int s, int row, int col);
void __far kf_vio_mou_close(int s, int t);
static unsigned cb_seg[NTASK], cb_ofs[NTASK];
static unsigned char cb_in[NTASK];      /* Rueckruf laeuft gerade        */
static void cb_prog(int t);
static int pm_esbad(int t, unsigned es);
static void cb_v86(struct v86_frame *f, int t, int force);
static void cb_tick(struct v86_iret *r, int t);
static int cb_ret(struct v86_frame *f, int t);
static void cb_pmtick(struct v86_iret *r, int t);
static unsigned cb_pmcb(struct v86_frame *f, int t);
#define MOU_RET  0x0570         /* Maus-Rueckruf zurueck: ARPL E7h        */
#define MOU_TICK 0x0574         /* Maus-Rueckruf aus dem Takt: ARPL E8h; IRET */
#define MOU_PM   0x0578         /* Maus-Rueckruf aus dem PM: HLT; IRET(D)  */

static void k2_cr3(unsigned lo, unsigned hi);
#pragma aux k2_cr3 = "shl edx,16" "mov dx,ax" "mov cr3,edx" \
    __parm [__ax] [__dx] __modify [__dx];
static unsigned long k2_get_cr3(void);
#pragma aux k2_get_cr3 = "mov eax,cr3" "mov edx,eax" "shr edx,16" \
    __value [__dx __ax] __modify [__ax __dx];

static unsigned long k2_cr2(void);
#pragma aux k2_cr2 = "mov eax,cr2" "mov edx,eax" "shr edx,16" \
    __value [__dx __ax] __modify [__ax __dx];

static void put32(unsigned sel, unsigned i, unsigned long v)
{
    unsigned __far *p = MK_FP(sel, i << 2);
    p[0] = (unsigned)v;
    p[1] = (unsigned)(v >> 16);
}

static unsigned long get32(unsigned sel, unsigned i)
{
    unsigned __far *p = MK_FP(sel, i << 2);
    return p[0] | ((unsigned long)p[1] << 16);
}

/* ---- Box anlegen und abbauen (im Kontext des Aufrufers) ---------------- */

/* letzter Text einer Box (Zeichen + Farbe, 25 x 80 Worte) in einer eigenen
   Seite statt in DGROUP (4.000 Byte), nur von capture bis v86_replay belegt */
static pg_t cap_pg;
static unsigned char cap_rows, cap_len_r[ROWS];
static unsigned cap_len;                /* 0 = nichts gemerkt            */

static void cap_free(void)
{
    if (cap_pg)
        mem_free(cap_pg, 1);
    cap_pg = 0;
    cap_len = 0;
}

static unsigned cap_get(unsigned i)
{
    scratch2_map(PG_LIN(cap_pg), 4095);
    return *(unsigned short __far *)MK_FP(SCRATCH2, i * 2);
}

/* Text der Box (Zeilen bis zum Cursor) mit Farben fuer v86_replay merken
   (DOOMs Abschlussseite ENDOOM ist farbig) */
static void capture(int t)
{
    unsigned long e;
    unsigned r, c, last, crow, v;

    cap_free();
    if ((cap_pg = mem_alloc(1)) == 0)
        return;                         /* kein Speicher: Text geht verloren */
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    e = get32(SCRATCH2, 0xB8);
    scratch_map(get32(SCRATCH2, 0) & ~0xFFFUL, 4095);
    crow = *(unsigned char __far *)MK_FP(SCRATCH, 0x451);
    if (*(unsigned char __far *)MK_FP(SCRATCH, 0x450))
        crow++;                         /* Cursor mitten in der Zeile    */
    if (crow > ROWS)
        crow = ROWS;
    scratch_map(e & ~0xFFFUL, 4095);
    scratch2_map(PG_LIN(cap_pg), 4095);
    for (r = 0; r < crow; r++) {
        for (c = 0, last = 0; c < COLS; c++) {
            v = *(unsigned short __far *)MK_FP(SCRATCH, (r * COLS + c) * 2);
            *(unsigned short __far *)MK_FP(SCRATCH2, (r * COLS + c) * 2) = (unsigned short)v;
            if ((v & 0xFF) != ' ' || (v & 0x7000))  /* Zeichen oder Hintergrund */
                last = c + 1;
        }
        cap_len_r[r] = (unsigned char)last;
    }
    cap_rows = (unsigned char)crow;
    cap_len = crow ? 1 : 0;
    if (!cap_len)
        cap_free();
}

void __far v86_destroy(int t)
{
    unsigned i;
    unsigned long e;

    if (!pd_box[t])
        return;
    vdd_event(t, VDD_EV_DESTROY, 0);       /* Module (VCOM.SYS)             */
    dpmi_end(t);
    if (box_ses[t] >= 0 && box_fg(t))
        kf_kbd_flush();                 /* wie v86_park                  */
    if (spk[t] && box_fg(t))
        spk_hw(t, 0);
    spk_ports(t, 0);
    vfgr[t] = vtk[t] = virq[t] = 0;     /* Ton: VSB.SYS (Ereignis oben)  */
    spk[t] = 0;
    if (noblink[t])
        blink_hw(0);
    noblink[t] = 0;
    if (pit_rl[t] && box_fg(t))
        kpit_set(0);
    pit_rl[t] = 0;
    if (pt_box[t])
        vga_leave(t);                           /* A0-BF vor dem Freigeben */
    if (box_ses[t] >= 0 && pt_box[t])
        capture(t);
    box_ses[t] = -1;
    /* laeuft gerade das CR3 der Box (sie beendet sich selbst)? Dann auf
       das des Kernels - der Kernel lebt ganz im Alias */
    if (k2_get_cr3() == PG_LIN(pd_box[t]))
        k2_cr3((unsigned)paging_cr3(), (unsigned)(paging_cr3() >> 16));
    for (i = 1; i < 512; i++) {         /* eigene Seitentabellen (DPMI) */
        scratch2_map(PG_LIN(pd_box[t]), 4095);
        e = get32(SCRATCH2, i);
        if (e & PTE_OWN) {
            unsigned j;
            unsigned long ee;
            for (j = 0; j < 1024; j++) {
                scratch_map(e & ~0xFFFUL, 4095);
                ee = get32(SCRATCH, j);
                if (ee & PTE_OWN)
                    pte_drop(ee);
            }
            vp_free(e >> 12);
        }
    }
    if (pt_box[t]) {
        scratch_map(PG_LIN(pt_box[t]), 4095);
        for (i = 0; i < 0x400; i++) {           /* bis 4 MB, auch XMS     */
            e = get32(SCRATCH, i);
            if ((e & (PTE_P | PTE_SWP)) && (i <= 0xA0 || i == 0xF0 || (e & PTE_OWN)))  /* nicht B8/B9! */
                pte_drop(e);
        }
        vp_free(pt_box[t]);
        pt_box[t] = 0;
    }
    vp_free(pd_box[t]);
    pd_box[t] = 0;
    xms_end(t);
}

/* nach dem Ende einer Box in der Sitzung ihres Aufrufers: ihren Text dort
   ausgeben (s < 0: verwerfen) */
int __far kf_vio_ansi(int s, unsigned on);
/* Farbe (VGA-Attribut) als ANSI-Folge: VGA zaehlt BGR, ANSI RGB */
static unsigned ansi_seq(char *b, unsigned a)
{
    static const char map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    unsigned n = 0;
    b[n++] = 27; b[n++] = '['; b[n++] = '0';
    if (a & 0x08) { b[n++] = ';'; b[n++] = '1'; }
    if (a & 0x80) { b[n++] = ';'; b[n++] = '5'; }
    b[n++] = ';'; b[n++] = '3'; b[n++] = (char)('0' + map[a & 7]);
    b[n++] = ';'; b[n++] = '4'; b[n++] = (char)('0' + map[(a >> 4) & 7]);
    b[n++] = 'm';
    return n;
}

static struct { unsigned char v; unsigned cs, ip, ax; } trc[8];
static unsigned char trn;
static unsigned long trc_ivt[3];
static unsigned char trc_code[8];       /* Befehlsbytes bei CS:IP        */
static void replay_text(int s);
extern char box_fmsg[];                 /* kernel.c fault: Abbruch der Box */

static unsigned hx(char *b, unsigned n, unsigned v, unsigned d)
{
    while (d--)
        b[n++] = "0123456789ABCDEF"[(v >> (d * 4)) & 15];
    return n;
}

static void trace_out(int s)
{
    static char b[96];
    unsigned i, k, n;
    for (i = 0; i < 8; i++) {
        k = (trn + i) & 7;
        n = 0;
        b[n++] = i ? ' ' : '\n';
        n = hx(b, n, trc[k].v, 2);
        b[n++] = ' ';
        n = hx(b, n, trc[k].cs, 4);
        b[n++] = ':';
        n = hx(b, n, trc[k].ip, 4);
        b[n++] = '/';
        n = hx(b, n, trc[k].ax, 4);
        kf_vio_tty(s, KDATA, (unsigned)b, n);
    }
    n = 0;
    b[n++] = '\n';
    for (i = 0; i < 3; i++) {                   /* INT 08h, 09h, 1Ch     */
        b[n++] = ' ';
        n = hx(b, n, (unsigned)(trc_ivt[i] >> 16), 4);
        b[n++] = ':';
        n = hx(b, n, (unsigned)trc_ivt[i], 4);
    }
    b[n++] = ' ';                               /* Befehl bei CS:IP      */
    for (i = 0; i < 8; i++)
        n = hx(b, n, trc_code[i], 2);
    b[n++] = '\n';
    kf_vio_tty(s, KDATA, (unsigned)b, n);
}

void __far v86_replay(int s)
{
    unsigned n;
    replay_text(s);
    if (s >= 0 && box_fmsg[0]) {            /* warum die Box endete      */
        for (n = 0; box_fmsg[n]; n++)
            ;
        kf_vio_tty(s, KDATA, (unsigned)"\nDOS-Box abgebrochen: ", 23);
        kf_vio_tty(s, KDATA, (unsigned)box_fmsg, n);
        trace_out(s);
    }
    box_fmsg[0] = 0;
}

static void replay_text(int s)
{
    static char b[96];
    unsigned r, c, n, a, v, cur = 0x07, col = kf_vio_ansi(s, 2);
    unsigned char ch;
    if (s < 0 || !cap_len) {
        cap_free();
        return;
    }
    for (r = 0; r < cap_rows; r++) {
        for (c = 0, n = 0; c < cap_len_r[r]; c++) {
            v = cap_get(r * COLS + c);
            a = v >> 8;
            if (col && a != cur) {
                if (n)
                    kf_vio_tty(s, KDATA, (unsigned)b, n);
                n = ansi_seq(b, a);
                cur = a;
            }
            ch = (unsigned char)v;
            b[n++] = ch < ' ' ? ' ' : (char)ch;
            if (n > sizeof(b) - 20) {
                kf_vio_tty(s, KDATA, (unsigned)b, n);
                n = 0;
            }
        }
        if (col && cur != 0x07) {               /* Zeilenende ohne Farbe */
            b[n++] = 27; b[n++] = '['; b[n++] = '0'; b[n++] = 'm';
            cur = 0x07;
        }
        if (cap_len_r[r] < COLS)                /* volle Zeile bricht selbst um */
            b[n++] = '\n';
        kf_vio_tty(s, KDATA, (unsigned)b, n);
    }
    cap_free();
}

/* BIOS-Takte seit Mitternacht aus der Echtzeituhr (ohne 32-Bit-Division) */
static unsigned long rtc_ticks(void)
{
    unsigned long st = rtc_stamp(), tk = 0;
    unsigned tm = (unsigned)st, h = tm >> 11, m = (tm >> 5) & 63, s = (tm & 31) << 1, i;
    for (i = 0; i < h; i++)
        tk += 65543UL;                  /* 3600 * 18,2065                */
    tk += (unsigned)(m * 1092u) + s * 18 + s / 5;   /* <= 64428 + 1073 */
    return tk;
}

/* -> CR3 der neuen Box, 0 = kein Speicher. Speicher genullt. */

unsigned long __far v86_create(int t, unsigned kb)
{
    unsigned long kcr3 = paging_cr3(), e, tk;
    unsigned i, n = kb >> 2;
    pg_t p;

    if (!kcr3 || pd_box[t] || n == 0 || n > 0xA0)
        return 0;
    box_ses[t] = -1;
    box_cm[t] = 0;
    wtr_n[t] = 0;
    pd_box[t] = vp_alloc();
    pt_box[t] = vp_alloc();
    if (!pd_box[t] || !pt_box[t]) {
        v86_destroy(t);
        return 0;
    }
    /* Seitenverzeichnis: 0 = eigene Tabelle, ab 512 wie der Kernel */
    for (i = 0; i < 1024; i++) {
        e = 0;
        if (i >= 512) {                 /* Kernel-Alias: nur Ring 0 (sonst  */
            scratch_map(kcr3, 4095);    /* kaemen DPMI-Programme mit 4-GB-  */
            e = get32(SCRATCH, i) & ~(unsigned long)PTE_U;  /* Selektoren dran) */
        } else if (i == 0)
            e = PG_LIN(pt_box[t]) | PTE_BOX;
        scratch2_map(PG_LIN(pd_box[t]), 4095);
        put32(SCRATCH2, i, e);
    }
    /* Seitentabelle: erst leer und ROM, dann eigene Seiten */
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    for (i = 0; i < 1024; i++)
        put32(SCRATCH2, i, i >= 0xC0 && i < 0x100 ? PG_LIN(i) | PTE_ROM : 0);
    for (i = 0; i <= n + 1; i++) {      /* n: Ersatzseite, n+1: BIOS-Seite */
        p = vp_alloc_hi();
        if (!p) {
            v86_destroy(t);
            return 0;
        }
        scratch_map(PG_LIN(p), 4095);
        zero_seg(SCRATCH, 2048);
        scratch2_map(PG_LIN(pt_box[t]), 4095);
        if (i == n + 1)
            put32(SCRATCH2, 0xF0, PG_LIN(p) | PTE_ROM);
        else if (i == n) {              /* A0000-BFFFF (B8/B9: v86_start) */
            for (e = 0xA0; e < 0xC0; e++)
                put32(SCRATCH2, (unsigned)e, PG_LIN(p) | PTE_BOX);
        } else
            put32(SCRATCH2, i, PG_LIN(p) | PTE_BOX);
        if (i < 0x10 && i < n)                  /* HMA: Umlauf           */
            put32(SCRATCH2, 0x100 + i, PG_LIN(p) | PTE_BOX);
    }
    vfl[t] = FL_IF;
    vme_on[t] = 0;
    pvi_t[t] &= ~CR4_VME;
    visr[t] = vimr[t] = 0;
    kq_h[t] = kq_t[t] = 0;
    pit_rl[t] = 0;
    pit_acc[t] = pit_t0[t] = 0;
    pit_acc_m[t] = 3;
    pit_latd[t] = pit_wlo[t] = pit_ff[t] = 0;
    spk[t] = spk_open[t] = 0;           /* (tss_init sperrt alle Ports) */
    virq[t] = vidle[t] = vtk[t] = vfgr[t] = 0;
    vt_pend[t] = 0;
    vcur[t] = vhide[t] = 0;
    vt_last[t] = get_ticks();
    vt_acc[t] = vt_pend[t] = 0;

    /* Vektortabelle (Seite 0) -> F000:n*4, INT 8 -> F000:0400 */
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    scratch_map(get32(SCRATCH2, 0) & ~0xFFFUL, 4095);
    for (i = 0; i < 256; i++)
        put32(SCRATCH, i, i == 0x67 ? 0xF0600020UL :      /* EMS: F060:0020 */
                          ((unsigned long)VBIOS << 16) | (i == 8 ? INT8_STUB : i << 2));
    /* Datenzeiger statt Stummel (BIOSCMP: INT 1Eh zeigte auf Code):
       Diskettenparameter, Videoparameter und Zeichensaetze des echten
       BIOS (liegen im ROM, 1:1 eingeblendet) */
    put32(SCRATCH, 0x1E, ((unsigned long)VBIOS << 16) | DPT_OFF);
    scratch2_map(0, 0x3FF);
    put32(SCRATCH, 0x1D, get32(SCRATCH2, 0x1D));
    put32(SCRATCH, 0x1F, get32(SCRATCH2, 0x1F));
    put32(SCRATCH, 0x43, get32(SCRATCH2, 0x43));
    /* BIOS-Datenbereich: der echte (Ports, Ausstattung), dann Bildschirm
       80x25 Farbe, Speichergroesse, Takt seit Mitternacht */
    tk = rtc_ticks();
    scratch2_map(0x400, 0xFF);
    far_copy(SCRATCH, 0x400, SCRATCH2, 0, 0x100);
    *(unsigned __far *)MK_FP(SCRATCH, 0x413) = kb;
    *(unsigned char __far *)MK_FP(SCRATCH, 0x449) = 3;
    *(unsigned __far *)MK_FP(SCRATCH, 0x44A) = COLS;
    *(unsigned __far *)MK_FP(SCRATCH, 0x44C) = 4096;
    *(unsigned __far *)MK_FP(SCRATCH, 0x44E) = 0;
    for (i = 0x450; i < 0x460; i++)
        *(unsigned char __far *)MK_FP(SCRATCH, i) = 0;
    *(unsigned __far *)MK_FP(SCRATCH, 0x460) = 0x0607;
    *(unsigned char __far *)MK_FP(SCRATCH, 0x462) = 0;
    *(unsigned __far *)MK_FP(SCRATCH, 0x463) = 0x3D4;
    *(unsigned __far *)MK_FP(SCRATCH, 0x46C) = (unsigned)tk;
    *(unsigned __far *)MK_FP(SCRATCH, 0x46E) = (unsigned)(tk >> 16);
    *(unsigned char __far *)MK_FP(SCRATCH, 0x470) = 0;
    *(unsigned char __far *)MK_FP(SCRATCH, 0x484) = ROWS - 1;
    *(unsigned __far *)MK_FP(SCRATCH, 0x41A) = 0x1E;    /* Tastaturpuffer   */
    *(unsigned __far *)MK_FP(SCRATCH, 0x41C) = 0x1E;    /* leer (INT 16h 05h) */
    *(unsigned __far *)MK_FP(SCRATCH, 0x480) = 0x1E;
    *(unsigned __far *)MK_FP(SCRATCH, 0x482) = 0x3E;
    /* Laufwerke: ein Diskettenlaufwerk (das Startabbild), keine Platten -
       die AT-DOS-Laufwerke kommen ueber den Redirector */
    *(unsigned __far *)MK_FP(SCRATCH, 0x410) =
        (*(unsigned __far *)MK_FP(SCRATCH, 0x410) & ~0xC1u) | 1;
    *(unsigned char __far *)MK_FP(SCRATCH, 0x475) = 0;
    *(unsigned __far *)MK_FP(SCRATCH, 0x485) = 16;
    /* Stummel bei F000:0000: je Vektor "ARPL n; IRET; NOP", bei 0400
       der Takt */
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    scratch_map(get32(SCRATCH2, 0xF0) & ~0xFFFUL, 4095);
    for (i = 0; i < 256; i++)
        put32(SCRATCH, i, 0x90CF0063UL | ((unsigned long)i << 8));
    put32(SCRATCH, INT8_STUB >> 2, 0x1CCD0863UL);   /* 63 08 CD 1C       */
    put32(SCRATCH, 0x2A, 0x909090CFUL);             /* INT 2Ah: IRET     */
    put32(SCRATCH, XMS_STUB >> 2, 0x90CBE163UL);    /* 63 E1 CB: XMS     */
    put32(SCRATCH, EMS_RET >> 2, 0x90CBE663UL);     /* 63 E6 CB: EMS 56h */
    put32(SCRATCH, MOU_RET >> 2, 0x9090E763UL);     /* 63 E7: Maus       */
    put32(SCRATCH, MOU_TICK >> 2, 0x90CFE863UL);    /* 63 E8 CF: Maus/Takt */
    put32(SCRATCH, 0x60A >> 2, 0);                  /* F060:000A EMMXXXX0 */
    for (i = 0; i < 8; i++)
        *(unsigned char __far *)MK_FP(SCRATCH, 0x60A + i) = "EMMXXXX0"[i];
    put32(SCRATCH, 0x620 >> 2, 0x90CF6763UL);       /* 63 67 CF: INT 67h */
    put32(SCRATCH, 0x510 >> 2, 0x90CBE263UL);       /* DPMI-Einsprung    */
    put32(SCRATCH, 0x520 >> 2, 0x90CFE363UL);       /* DPMI-Ruecksprung  */
    put32(SCRATCH, 0x528 >> 2, 0x909090CBUL);       /* RETF (0305h)      */
    put32(SCRATCH, 0x52C >> 2, 0x9090CB66UL);       /* RETFD             */
    put32(SCRATCH, 0x530 >> 2, 0x909090F4UL);       /* Raw PM -> RM: HLT */
    put32(SCRATCH, 0x548 >> 2, 0x909090F4UL);       /* Callback-Rueckkehr */
    for (i = 0; i < 16; i++)                        /* Callbacks: ARPL E5 */
        put32(SCRATCH, (0x700 >> 2) + i, 0x90CBE563UL);
    put32(SCRATCH, 0x534 >> 2, 0x90CFE463UL);       /* Raw RM -> PM: ARPL E4 */
    *(unsigned char __far *)MK_FP(SCRATCH, INT8_STUB + 4) = 0xCF;
    /* Diskettenparameter (INT 13h 08h, INT 1Eh) bei F000:0800, 1,44 MB */
    /* AF 02 25 02 12 1B FF 6C F6 0F 08 (vorher fehlte die Sektorgroesse
       02 - FreeDOS trug die Sektoren je Spur an falscher Stelle ein) */
    put32(SCRATCH, DPT_OFF >> 2, 0x022502AFUL);
    put32(SCRATCH, (DPT_OFF >> 2) + 1, 0x6CFF1B12UL);
    put32(SCRATCH, (DPT_OFF >> 2) + 2, 0x00080FF6UL);
    /* Rechnerkonfiguration (INT 15h C0h) bei F000:0890: Laenge 8, AT (FCh),
       Untermodell 0, Stand 1, zweiter PIC + Echtzeituhr, INT 16h 09h */
    put32(SCRATCH, CFG_OFF >> 2, 0x00FC0008UL);
    put32(SCRATCH, (CFG_OFF >> 2) + 1, 0x00406001UL);
    img_spt[t] = img_heads[t] = img_secs[t] = 0;
    idle_n[t] = 0;
    b2f[t] = 0;
    for (i = 0; i < 16; i++)
        cmos_om[t][i] = 0;
    alrm_on[t] = 0;
    shf_last[t] = 0xFFFF;
    vimr2[t] = 0xFF;                    /* wie nach dem BIOS: alle gesperrt */
    picw[t][0] = picw[t][1] = 0;
    pic_isr[t][0] = pic_isr[t][1] = 0;
    cb_seg[t] = cb_ofs[t] = 0;
    cb_in[t] = 0;
    vdd_event(t, VDD_EV_CREATE, 0);        /* Module (COM/LPT: VCOM.SYS)    */
    if (!umb_ok)                        /* EMS: VEMS.SYS; Rahmen erst leer */
        umb_scan();
    if (EFR(t)) {
        unsigned long sp = pte_get(t, 0xA0) & ~0xFFFUL;
        for (i = 0; i < 16; i++)
            pte_set(t, EFR(t) + i, sp | PTE_BOX);
    }
    return PG_LIN(pd_box[t]);
}

/* Sitzung s steht fest (task_create): Vollbild, B800 = Sitzungspuffer */
void __far v86_start(int t, int s)
{
    unsigned long b;

    if (!pt_box[t])
        return;
    if (s < 0) {                        /* Teil einer Pipe im Hintergrund: */
        box_ses[t] = -1;                /* Bildschirm der Sitzung bleibt  */
        return;                         /* (B800 = Ersatzseite)           */
    }
    kf_vio_setmode(s, 1, t);            /* leer, alle Zeilen             */
    b = kf_vio_buf_lin(s);
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    put32(SCRATCH2, 0xB8, b | PTE_BOX);
    put32(SCRATCH2, 0xB9, (b + 4096) | PTE_BOX);
    box_ses[t] = (signed char)s;
    kf_vio_setcur(s, 0, 0);
}

/* Box parken (BOXSHELL meldet: Programm fertig, wie NTVDM offen halten):
   Vollbild zuruecknehmen, ihren Text in der Sitzung stehen lassen, keine
   Tasten und keine Bildschirmabfrage mehr, bis v86_start sie wieder an
   eine Sitzung haengt */
void __far v86_park(int t)
{
    int s = box_ses[t];
    vdd_event(t, VDD_EV_PARK, 0);          /* Module                        */
    if (s < 0)
        return;
    if (box_fg(t))                      /* Tasten des DOS-Programms (Spiele
                                           mit eigenem INT 9) nicht an CMD -
                                           sonst holt Pfeil hoch + Enter den
                                           letzten Befehl zurueck (DOOM lief
                                           endlos neu) */
        kf_kbd_flush();
    if (spk[t] && box_fg(t))
        spk_hw(t, 0);
    spk_ports(t, 0);
    vfgr[t] = vtk[t] = virq[t] = 0;     /* Ton: VSB.SYS (Ereignis oben)  */
    spk[t] = 0;
    if (pit_rl[t] && box_fg(t))
        kpit_set(0);
    pit_rl[t] = 0;
    if (noblink[t])
        blink_hw(0);
    noblink[t] = 0;
    vga_leave(t);
    if (pt_box[t])
        capture(t);
    kf_vio_mou_close(s, t);
    box_ses[t] = -1;
    kq_h[t] = kq_t[t] = 0;
    kf_vio_setmode(s, 0, -1);
    v86_replay(s);
}

/* Box startet von einem Abbild (load_img): Geometrie fuer INT 13h */
void __far v86_disk(int t, unsigned spt, unsigned heads, unsigned secs)
{
    if (spt == 0 || spt > 63 || heads == 0 || heads > 255)
        spt = 9, heads = 2;             /* BPB unbrauchbar: 360 KB      */
    img_spt[t] = spt;
    img_heads[t] = heads;
    img_secs[t] = secs;
}

/* Tastatur-Interrupt: ist die Vordergrund-Sitzung eine Box mit eigenem
   DOS (Startabbild)? Dann gehoert Strg+C dem DOS darin. */
int __far v86_fgbox(void)
{
    int t, s;
    for (t = 0; t < NTASK && (box_ses[t] < 0 || !(img_secs[t] || box_cm[t])); t++)
        ;
    if (t == NTASK)
        return 0;
    s = fg;
    for (t = 0; t < NTASK; t++)
        if (box_ses[t] == s && (img_secs[t] || box_cm[t]))
            return 1;
    return 0;
}

/* n Bytes aus dem Kernel (sel:off) an linear lin der Box, Seite fuer Seite
   ueber die physischen Seiten (die Box laeuft dabei nicht) */
/* rd = 0: sel:off -> Box (v86_copy), rd = 1: Box -> sel:off (Module,
   VS_RD). Laeuft auch fuer eine fremde Box (pt_box[t]). */
void __far v86_xfer(int t, unsigned long lin, unsigned sel, unsigned off, unsigned n, int rd)
{
    unsigned k, o;
    unsigned long e;

    while (n) {
        o = (unsigned)lin & 0xFFF;
        k = 4096 - o;
        if (k > n)
            k = n;
        scratch2_map(PG_LIN(pt_box[t]), 4095);
        e = get32(SCRATCH2, (unsigned)(lin >> 12));
        if (!(e & PTE_P)) {
            if ((e & PTE_SWP) && swap_need(t, lin >> 12))
                continue;               /* eingelagert: noch einmal       */
            return;
        }
        scratch2_map(e & ~0xFFFUL, 4095);
        if (rd)
            far_copy(sel, off, SCRATCH2, o, k);
        else
            far_copy(SCRATCH2, o, sel, off, k);
        lin += k;
        off += k;
        n -= k;
    }
}

void __far v86_copy(int t, unsigned long lin, unsigned sel, unsigned off, unsigned n)
{
    v86_xfer(t, lin, sel, off, n, 0);
}

/* Scheduler: hat eine Box in ihren Bildschirm geschrieben (Dirty-Bit der
   Seiten B8/B9)? Dann Sitzung neu zeichnen. Laeuft ohne Kernelsperre in
   irgendeinem Kontext - SCRATCH sichern. */
void __far v86_poll(void)
{
    unsigned short w[8];
    unsigned long e8, e9;
    int t, cur = -2;

    for (t = 0; t < NTASK; t++)         /* Lautsprecher nur vorn         */
        if ((spk_open[t] || vfgr[t]) &&
            !(box_ses[t] >= 0 && box_ses[t] == fg)) {
            spk_ports(t, 0);
            vdd_bg(t);
        }
    if (kpit_div) {                     /* schneller Takt nur fuer die Box vorn */
        int s = fg, ok = 0;
        for (t = 0; t < NTASK; t++)
            if (box_ses[t] == s && s >= 0 && pit_rl[t])
                ok = 1;
        if (!ok)
            kpit_set(0);
    }

    for (t = 0; t < NTASK; t++) {
        if (box_ses[t] < 0 || !pt_box[t] || vga_raw[t])
            continue;
        win_save(w);
        scratch_map(PG_LIN(pt_box[t]), 4095);
        e8 = get32(SCRATCH, 0xB8);
        e9 = get32(SCRATCH, 0xB9);
        if ((e8 | e9) & PTE_D) {
            put32(SCRATCH, 0xB8, e8 & ~(unsigned long)PTE_D);
            put32(SCRATCH, 0xB9, e9 & ~(unsigned long)PTE_D);
        }
        win_restore(w);
        if ((e8 | e9) & PTE_D) {
            kf_vio_dirty(box_ses[t]);
            if (cur == -2)
                cur = current;
            if (cur == t)                   /* TLB hat noch D=1: leeren  */
                k2_cr3((unsigned)k2_get_cr3(), (unsigned)(k2_get_cr3() >> 16));
        }
    }
}

/* Tastatur-Interrupt (kbd_irq): Scancode an die Box im Vordergrund */
void __far v86_key(unsigned sc)
{
    int s, t;
    unsigned char n;

    for (t = 0; t < NTASK && box_ses[t] < 0; t++)
        ;
    if (t == NTASK)
        return;                         /* keine Box: schnell zurueck    */
    s = fg;
    for (t = 0; t < NTASK; t++)
        if (box_ses[t] == s && pt_box[t]) {
            n = (unsigned char)((kq_h[t] + 1) & 15);
            if (n != kq_t[t]) {
                kq[t][kq_h[t]] = (unsigned char)sc;
                kq_h[t] = n;
            }
        }
}

static int kq_get(int t)
{
    int c;
    if (kq_h[t] == kq_t[t])
        return -1;
    c = kq[t][kq_t[t]];
    kq_t[t] = (unsigned char)((kq_t[t] + 1) & 15);
    return c;
}

/* ---- Speicher der Box (im eigenen Kontext, CR3 der Box) ----------------- */

/* Fenster VWIN auf seg:0, Grenze 64 KB; Basis roh (kein Alias) */
static void vwin(unsigned seg)
{
    struct desc *d = &gdt[VWIN_IDX];
    unsigned long b = (unsigned long)seg << 4;
    d->limit = 0xFFFF;
    d->base_lo = (unsigned short)b;
    d->base_hi = (unsigned char)(b >> 16);
    d->access = ACC_KDATA;
    d->rsv = 0;
}

static unsigned char rd8(unsigned seg, unsigned off)
{
    vwin(seg);
    return *(unsigned char __far *)MK_FP(VWIN, off);
}

static void wr8(unsigned seg, unsigned off, unsigned char v)
{
    vwin(seg);
    *(unsigned char __far *)MK_FP(VWIN, off) = v;
}

static unsigned rd16(unsigned seg, unsigned off)
{
    vwin(seg);
    return *(unsigned char __far *)MK_FP(VWIN, off) |
           (*(unsigned char __far *)MK_FP(VWIN, (unsigned)(off + 1)) << 8);
}

static void wr16(unsigned seg, unsigned off, unsigned v)
{
    vwin(seg);
    *(unsigned char __far *)MK_FP(VWIN, off) = (unsigned char)v;
    *(unsigned char __far *)MK_FP(VWIN, (unsigned)(off + 1)) = (unsigned char)(v >> 8);
}

static void push16(struct v86_iret *r, unsigned v)
{
    unsigned sp = (unsigned)r->esp - 2;
    wr16((unsigned)r->ss, sp, v);
    r->esp = (r->esp & 0xFFFF0000UL) | sp;
}

static unsigned pop16(struct v86_iret *r)
{
    unsigned sp = (unsigned)r->esp, v = rd16((unsigned)r->ss, sp);
    r->esp = (r->esp & 0xFFFF0000UL) | (unsigned)(sp + 2);
    return v;
}

/* FLAGS, wie die Box sie sieht */
static unsigned vflags(struct v86_iret *r, int t)
{
    return ((unsigned)r->eflags & ~FL_V) | vfl[t] | 0x0002;
}

/* POPF/IRET: Rechenflags echt, IF/IOPL/NT nur virtuell */
static void set_flags(struct v86_iret *r, int t, unsigned v)
{
    vfl[t] = v & FL_V;
    r->eflags = (r->eflags & ~(unsigned long)FL_SET) | (v & FL_SET);
}

/* Vektor n zeigt noch auf den Stummel des Kernels? */
static int vec_def(unsigned n)
{
    if (n == 0x67)
        return rd16(0, 0x19E) == 0xF060 && rd16(0, 0x19C) == 0x0020;
    return rd16(0, n * 4 + 2) == VBIOS && rd16(0, n * 4) == (n == 8 ? INT8_STUB : n * 4);
}

/* Umleitungs-Bitmap (VME): Bit 0 = INT n laeuft ohne Falle ueber die IVT
   der Box. Nur fuer Vektoren, bei denen der Monitor nichts tut, solange
   sie nicht auf seinen Stummel zeigen (INT 21h/29h nach dem Laden des
   DOS: FreeDOS-MEM schreibt jedes Zeichen einzeln - je Zeichen INT 21h,
   INT 29h und INT 10h, vorher drei Fallen, jetzt eine). */
static void irb_set(int t, unsigned n)
{
    unsigned char m = (unsigned char)(1 << (n & 7));
    if (n == 0x2F ? !b2f[t] : n != 0x2A && vec_def(n))
        tss[t].irb[n >> 3] |= m;
    else
        tss[t].irb[n >> 3] &= (unsigned char)~m;
}

/* INT n ohne Dienst: FLAGS, CS, IP auf den Stack, ueber die IVT */
/* Spur fuer die Fehlersuche: die letzten 8 Fallen der Box (Ausnahme
   bzw. 80h + zugestellter Interrupt, CS:IP, AX); bricht die Box ab,
   gibt v86_replay sie mit der Meldung in CMD aus, dazu die Vektoren
   08h/09h/1Ch der Box im Moment des Abbruchs */
static void trace(unsigned v, unsigned cs, unsigned ip, unsigned ax)
{
    unsigned i = trn++ & 7;
    trc[i].v = (unsigned char)v;
    trc[i].cs = cs;
    trc[i].ip = ip;
    trc[i].ax = ax;
}

static void int_raw(struct v86_iret *r, int t, unsigned n, unsigned ip)
{
    push16(r, vflags(r, t));
    push16(r, (unsigned)r->cs);
    push16(r, ip);
    vfl[t] &= ~FL_IF;
    r->eflags &= ~(unsigned long)FL_TF;
    r->cs = rd16(0, n * 4 + 2);
    r->eip = rd16(0, n * 4);
    trace(0x80 | n, (unsigned)r->cs, (unsigned)r->eip, 0);
}

/* ---- Takt ---------------------------------------------------------------- */

static void bda_ticks(unsigned n)
{
    unsigned long tk = rd16(BDA, 0x6C) | ((unsigned long)rd16(BDA, 0x6E) << 16);
    tk += n;
    if (tk >= 0x1800B0UL) {             /* Mitternacht                   */
        tk -= 0x1800B0UL;
        wr8(BDA, 0x70, 1);
    }
    wr16(BDA, 0x6C, (unsigned)tk);
    wr16(BDA, 0x6E, (unsigned)(tk >> 16));
}


/* ---- virtueller Zeitgeber ------------------------------------------------ */
#define KPIT 11932U                     /* Teiler des Kernels (100 Hz)   */

/* Gameport messen: Monoflops anstossen, 201h lesen, bis die vier Achsen-
   bits fallen; Zeit ueber Kanal 0 des PIT (zaehlt abwaerts, Teiler kpit_
   div). Hoechstens 3 ms mit gesperrten Interrupts (3580 PIT-Schritte).
   ax[i] in us, FFFFh = Achse nicht da. -> 201h (Tasten Bit 4-7, 0 =
   gedrueckt) */
static unsigned pit_now0(void)
{
    unsigned c;
    outb(0x43, 0x00);
    c = inb(0x40);
    return c | inb(0x40) << 8;
}

static unsigned joy_read(unsigned *ax)
{
    unsigned f, b, prev, now, d, tot = 0, done = 0, i;
    unsigned rl = kpit_div ? kpit_div : KPIT;
    f = irq_off();
    outb(0x201, 0xFF);
    prev = pit_now0();
    do {
        b = inb(0x201);
        now = pit_now0();
        d = prev >= now ? prev - now : prev + rl - now;
        prev = now;
        tot += d;
        for (i = 0; i < 4; i++)
            if (!(done & (1 << i)) && !(b & (1 << i))) {
                done |= 1 << i;         /* us = Schritte * 0,838         */
                ax[i] = tot - (tot >> 3) - (tot >> 5) - (tot >> 7);
            }
    } while (done != 15 && tot < 3580);
    irq_restore(f);
    for (i = 0; i < 4; i++)
        if (!(done & (1 << i)))
            ax[i] = 0xFFFF;
    return b;
}

/* FAPI_JOYSTICK (kernel.c) */
unsigned __far joy_fapi(unsigned usel, unsigned off)
{
    struct fapi_joy __far *j = MK_FP(usel, off);
    unsigned ax[4], b, i;
    b = joy_read(ax);
    if (ax[0] == 0xFFFF && ax[1] == 0xFFFF && ax[2] == 0xFFFF &&
        ax[3] == 0xFFFF && (b & 0xF0) == 0xF0)
        return FAPI_ERROR;              /* kein Gameport (alles 1)       */
    j->buttons = ~b >> 4 & 15;
    for (i = 0; i < 4; i++)
        j->axis[i] = ax[i];
    return 0;
}

/* vergangene PIT-Schritte seit der letzten Messung der Box (echte Zeit:
   Kerneltakt + echter Zaehler von Kanal 0) */
static unsigned long pit_elapsed(int t)
{
    unsigned f = irq_off(), c, d, pos;
    unsigned long tk, el = 0;
    tk = get_ticks();
    outb(0x43, 0x00);
    c = inb(0x40);
    c |= inb(0x40) << 8;
    pit_lastc = c;
    pos = kpit_acc + ((kpit_div ? kpit_div : KPIT) - c);   /* seit dem Takt */
    irq_restore(f);
    if (tk < pit_t0[t] || tk - pit_t0[t] > 50) {        /* Anfang/lange weg */
        pit_t0[t] = tk;
        pit_c0[t] = pos;
        return 0;
    }
    d = (unsigned)(tk - pit_t0[t]);
    while (d--)
        el += KPIT;
    el += pos;
    if (el < pit_c0[t])
        return 0;                       /* Takt noch nicht gezaehlt      */
    el -= pit_c0[t];
    pit_t0[t] = tk;
    pit_c0[t] = pos;
    return el;
}

/* echten Kanal 0 auf den Teiler der Box im Vordergrund (schneller als
   100 Hz), sonst zurueck auf 100 Hz; irq0 zaehlt den Kerneltakt weiter
   je 11932 Schritte */
#define KPIT_MIN 1193U                  /* hoechstens 1 kHz              */
static unsigned char kpit_slow;         /* auch langsamer als 100 Hz (HW_TIMER) */
static void kpit_set(unsigned dv)
{
    unsigned f;
    if (dv && dv < KPIT_MIN)
        dv = KPIT_MIN;
    if (dv >= KPIT && !kpit_slow)
        dv = 0;
    kpit_slow = 0;
    if (dv == kpit_div)
        return;
    f = irq_off();
    kpit_div = dv;
    outb(0x43, 0x34);                   /* Modus 2 wie pic_pit_init */
    outb(0x40, (dv ? dv : KPIT) & 0xFF);
    outb(0x40, (dv ? dv : KPIT) >> 8);
    irq_restore(f);
}

/* HW_TIMER=ON (DOSBOX.INI, angelehnt an OS/2): vorn laeuft der echte
   Kanal 0 mit dem Teiler der Box auch dann, wenn er langsamer als 100 Hz
   ist (irq0 zaehlt dann mehrere Kerneltakte je Interrupt) - jeder INT 8
   der Box kommt von einem echten Interrupt statt aus dem 100-Hz-Raster
   (bis 10 ms Zittern). Die Ports bleiben gefangen: Digger stellt den
   Teiler laufend direkt ueber 40h um (ohne Befehl an 43h); ganz frei
   gegeben lief die Uhr des Kernels 40 % vor. */
static int kpit_hw(int t)
{
    return (box_hk[t] & HK_HWT) && pit_rl[t] && box_fg(t);
}

static void kpit_check(int t)
{
    kpit_slow = (unsigned char)kpit_hw(t);
    kpit_set(pit_rl[t] && box_fg(t) ? pit_rl[t] : 0);
}

static unsigned long pit_div(int t)
{
    return pit_rl[t] ? pit_rl[t] : 65536UL;
}

/* Zeit fortschreiben: Zaehler und faellige INT 8 */
static int pit_sync(int t)
{
    return kpit_div && kpit_div == pit_div(t) && box_fg(t);
}

static void pit_run(int t)
{
    unsigned long el, dv = pit_div(t), c;
    unsigned n = irq0_n;
    if (pit_sync(t)) {
        /* Der echte Kanal 0 laeuft mit dem Teiler der Box: jeder echte
           irq0 ist genau ein INT 8 der Box, der Zaehler ist der echte.
           Ueber die gemessene Zeit kam ein Takt, der gerade faellig war,
           oft erst mit der naechsten Falle - der Lautsprecher (Digger
           schaltet ihn je INT 8 um) klang dadurch unsauber. */
        n -= pit_n0[t];
        pit_n0[t] = irq0_n;
        if (n > 2)
            n = 2;
        while (n-- && vt_pend[t] < 2)
            vt_pend[t]++;
        pit_acc[t] = 0;
        /* den echten Zaehler liest pit_in nur, wenn die Box ihn will -
           pit_run laeuft nach jeder Falle (auf dem 486 tausende Male je
           Sekunde, drei ISA-Zugriffe) */
        pit_t0[t] = 0;                  /* danach pit_elapsed: neu anfangen */
        return;
    }
    pit_n0[t] = n;
    el = pit_elapsed(t);
    if (!el)
        return;
    pit_acc[t] += el;
    while (pit_acc[t] >= dv) {
        pit_acc[t] -= dv;
        if (vt_pend[t] < 2)
            vt_pend[t]++;
    }
    c = dv - pit_acc[t];                /* Rest bis zum naechsten        */
    pit_cnt[t] = (unsigned)c;
}

/* Zaehlerstand fuer die Box (Lesen/Latch): im Gleichlauf der echte */
static void pit_now(int t)
{
    pit_run(t);
    if (pit_sync(t)) {
        pit_elapsed(t);
        pit_cnt[t] = pit_lastc;
    }
}

static unsigned char pit_in(int t, unsigned p)
{
    unsigned v;
    if (p != 0x40) {                    /* Kanal 1/2: grob laufen lassen */
        pit_c[t] -= 0x1F3;
        pit_ff[t] ^= 1;
        return (unsigned char)(pit_ff[t] ? pit_c[t] : pit_c[t] >> 8);
    }
    pit_now(t);
    v = pit_latd[t] ? pit_lat[t] : pit_cnt[t];
    if (pit_acc_m[t] == 1) {
        pit_latd[t] = 0;
        return (unsigned char)v;
    }
    if (pit_acc_m[t] == 2) {
        pit_latd[t] = 0;
        return (unsigned char)(v >> 8);
    }
    if (!pit_ff[t]) {
        pit_ff[t] = 1;
        return (unsigned char)v;
    }
    pit_ff[t] = 0;
    pit_latd[t] = 0;
    return (unsigned char)(v >> 8);
}

/* Lautsprecher gehoert der Box nur im Vordergrund */
static int box_fg(int t)
{
    return box_ses[t] >= 0 && fg == box_ses[t];
}

/* ---- Dienste fuer die Module (VSB.SYS: Ton) -------------------------------
   Die Nachbildung der Karte steht im Modul; hier bleibt, was den Monitor
   selbst betrifft: virtuelle IRQ-Leitungen (zugestellt in inject und
   dp_hwint wie die eigenen), der Takt-Rueckruf, Ports in der I/O-Bitmap
   und das Abgeben von Hardware beim Wechsel nach hinten. */

/* Modul gab der Box Hardware (OPL, EMU8000) - Box ist nicht mehr vorn */
static void vdd_bg(int t)
{
    if (!vfgr[t])
        return;
    vfgr[t] = 0;
    vdd_event(t, VDD_EV_BG, 0);
}

/* Takt an die Module, hoechstens einmal je Kerneltakt (oder sofort nach
   VS_TICK 2); im Ruecksprung in die Box mit IF aus */
static void vdd_tk(int t)
{
    unsigned now;
    if (!vtk[t])
        return;
    now = (unsigned)get_ticks();
    if (!(vtk[t] & 2) && now == vtl[t])
        return;
    vtl[t] = now;
    vtk[t] &= 1;
    enable_ints();
    vdd_tick(t, box_fg(t));
    irq_off();
}

/* VS_COPY: Fenster sel auf Adresse a der Art ty -> Bytes bis zur Grenze
   (0 = nicht da), *o = Offset. Jede Seite ueber ihren eigenen Selektor
   nachgeschlagen (die Seitentabelle der Box ueber sel selbst). */
static unsigned vs_win(int t, unsigned ty, unsigned long a, unsigned sel,
                       unsigned dsel, unsigned *ps, unsigned *o)
{
    unsigned long e;
    unsigned k;
    if (ty == VA_MOD) {
        *ps = dsel;
        *o = (unsigned)a;
        return a > 0xF000UL ? (unsigned)(0x10000UL - a) : 0x1000;
    }
    *ps = sel;
    *o = 0;
    k = 4096 - ((unsigned)a & 0xFFF);
    if (ty == VA_BOX) {
        if (a >= 0x400000UL)
            return 0;
        for (;;) {
            if (sel == SCRATCH)
                scratch_map(PG_LIN(pt_box[t]), 4095);
            else
                scratch2_map(PG_LIN(pt_box[t]), 4095);
            e = get32(sel, (unsigned)(a >> 12));
            if (e & PTE_P)
                break;
            if (!(e & PTE_SWP) || !swap_need(t, a >> 12))
                return 0;
        }
        a = (e & ~0xFFFUL) | ((unsigned)a & 0xFFF);
    } else if (ty != VA_PHYS)
        return 0;
    if (sel == SCRATCH)
        scratch_map(a, k - 1);
    else
        scratch2_map(a, k - 1);
    return k;
}

unsigned __far v86_vsvc(int t, struct vdd_svc *k, unsigned dsel)
{
    unsigned m, i, r, a1 = k->a1, a2 = k->a2;
    if (t < 0 || t >= NTASK || !pd_box[t])
        return 0xFFFF;
    switch (k->fn) {
    case VS_IRQ:                        /* Leitung a1 der Box            */
        if (a1 < 2 || a1 > 7 || t != current)
            return 0xFFFF;
        m = 1 << a1;
        if (vidle[t] & m) {             /* ruht (DOOM 1.9: DMX liest die  */
            if (vec_def(8 + a1))        /* DMA-Stelle, der IRQ ging ueber */
                return 1;               /* DOS/4GW zum leeren Stummel),  */
            vidle[t] &= (unsigned char)~m;  /* bis ein Handler kommt     */
        }
        virq[t] |= (unsigned char)m;
        return 0;
    case VS_VECDEF:
        if (t != current)
            return 0xFFFF;
        for (r = 0, i = 0; i < 8; i++) {
            if (vec_def(8 + i))
                r |= 1 << i;
            if (pm_hooked(t, 8 + i))
                r |= 0x100 << i;
        }
        return r;
    case VS_TICK:
        vtk[t] = (unsigned char)(a1 == 2 ? 3 : a1 ? 1 : 0);
        return 0;
    case VS_IOPERM:
        for (i = 0; i < a2 && a1 + i < 0x3E0; i++) {
            m = a1 + i;
            if (k->a3)
                tss[t].iob[m >> 3] &= (unsigned char)~(1 << (m & 7));
            else
                tss[t].iob[m >> 3] |= (unsigned char)(1 << (m & 7));
        }
        return 0;
    case VS_FGRES:
        vfgr[t] = (unsigned char)(a1 != 0);
        return 0;
    case VS_HK:
        return box_hk[t];
    case VS_PGALLOC:
        if (!a1 || !(k->lin = mem_alloc_hi(a1)))
            return 0xFFFF;
        v86_pages += a1;
        return 0;
    case VS_PGFREE:
        if (k->lin && a1) {
            mem_free((pg_t)k->lin, a1);
            v86_pages -= a1;
        }
        return 0;
    case VS_MAP: {                      /* Seiten in den Oberen Speicher  */
        unsigned long e, spare = pte_get(t, 0xA0) & ~0xFFFUL;
        if (a1 < 0xC0 || a2 > 0x30 || a1 + a2 > 0xF0)
            return 0xFFFF;
        for (i = 0; i < a2; i++)        /* nie eine eigene Seite der Box  */
            if (pte_get(t, a1 + i) & PTE_OWN)   /* ueberdecken (UMB)      */
                return 0xFFFF;
        for (i = 0; i < a2; i++) {
            e = k->lin ? PG_LIN((pg_t)k->lin + i) : spare;
            pte_set(t, a1 + i, e | PTE_BOX);
        }
        tlb(t);
        return 0;
    }
    case VS_COPY: {                     /* EMS 57h: Box/Seiten/Modul      */
        unsigned long s = k->lin, d = k->lin2;
        unsigned n = k->len, ss, so, ds, dof, x, y;
        while (n) {
            x = vs_win(t, k->ty1, s, SCRATCH, dsel, &ss, &so);
            y = vs_win(t, k->ty2, d, SCRATCH2, dsel, &ds, &dof);
            if (!x || !y)
                return 0xFFFF;
            if (x > y)
                x = y;
            if (x > n)
                x = n;
            far_copy(ds, dof, ss, so, x);
            s += x;
            d += x;
            n -= x;
        }
        return 0;
    }
    case VS_CBARM:                      /* Rueckruf (vdd.c merkt das Modul) */
        cb_seg[t] = a1;
        cb_ofs[t] = a2;
        if (!a1 && !a2)
            cb_in[t] = 0;
        return 0;
    case VS_MOUSE: {                    /* Maus der Sitzung (VMOUSE.SYS)  */
        int s = box_ses[t], dx, dy;
        unsigned char ev[8];
        unsigned __far *w = MK_FP(dsel, k->buf);
        if (a1 == VM_INFO)
            return mou_kind ? (unsigned)mou_kind << 8 | (mou_kind == 4 ? 0 : mou_irqn) : 0;
        if (s < 0)
            return 0;
        switch (a1) {
        case VM_OPEN:
            return kf_vio_mou_open(s, t);
        case VM_CLOSE:
            kf_vio_mou_close(s, t);
            return 0;
        case VM_DELTA:
            kf_vio_mou_delta(s, &dx, &dy);
            w[0] = (unsigned)dx;
            w[1] = (unsigned)dy;
            return 0;
        case VM_READ:
            if (!kf_vio_mou_read(s, ev))
                return 0;
            for (i = 0; i < 4; i++)
                w[i] = ((unsigned *)ev)[i];
            return 1;
        case VM_SHOW:
            kf_vio_mou_show(s, a2);
            return 0;
        case VM_POS:
            kf_vio_mou_pos(s, a2, k->a3);
            return 0;
        }
        return 0xFFFF;
    }
    case VS_INFO:
        if (a1 == VI_EMSFRAME)
            return EFR(t);
        if (a1 == VI_EMSLIM)
            return ems_lim[t];
        if (a1 == VI_FREE16) {
            unsigned long f = kf_mem_avail_kb() >> 4;
            return f > 0xFFFEUL ? 0xFFFE : (unsigned)f;
        }
        return 0;
    }
    return 0xFFFF;
}

/* Lautsprecher-Ports 42h/61h direkt freigeben (I/O-Bitmap), solange die
   Box vorn ist: Spiele (Digger) schreiben bei viel Action staendig
   dorthin, jeder Trap kostet - das Spiel wurde langsamer */
/* Gameport 201h direkt (I/O-Bitmap), solange die Box vorn ist: Spiele
   messen die Achsen, indem sie 201h in einer Schleife lesen - durch den
   Monitor waeren die Zeiten falsch (und jede Abfrage eine Falle) */
static void joy_ports(int t, int on)
{
    if (on == joy_open[t])
        return;
    joy_open[t] = (unsigned char)on;
    if (on)
        tss[t].iob[0x201 >> 3] &= (unsigned char)~(1 << (0x201 & 7));
    else
        tss[t].iob[0x201 >> 3] |= (unsigned char)(1 << (0x201 & 7));
}

static void spk_ports(int t, int on)
{
    if (!on)
        joy_ports(t, 0);                /* hinten/Ende: Gameport auch zu  */
    if (on == spk_open[t])
        return;
    spk_open[t] = (unsigned char)on;
    if (on) {
        tss[t].iob[0x42 >> 3] &= (unsigned char)~(1 << (0x42 & 7));
        tss[t].iob[0x61 >> 3] &= (unsigned char)~(1 << (0x61 & 7));
    } else {
        tss[t].iob[0x42 >> 3] |= (unsigned char)(1 << (0x42 & 7));
        tss[t].iob[0x61 >> 3] |= (unsigned char)(1 << (0x61 & 7));
        outb(0x61, inb(0x61) & ~3);             /* stumm                 */
        spk[t] = 0;
    }
}

unsigned k3_inw(unsigned port);
#pragma aux k3_inw = "in ax,dx" __parm [__dx] __value [__ax];
void k3_outw(unsigned port, unsigned v);
#pragma aux k3_outw = "out dx,ax" __parm [__dx] [__ax];
static char pf_buf[1024];              /* Suche in dp_patch_popf        */

static void spk_hw(int t, int on)
{
    unsigned char b = (unsigned char)inb(0x61);
    b = (unsigned char)((b & ~3) | (on ? spk[t] & 3 : 0));
    outb(0x61, b);
}

static void pit_out(int t, unsigned p, unsigned char v)
{
    if (p == 0x43) {
        unsigned ch = v >> 6;
        if (ch == 2) {                  /* Lautsprecher: echte Hardware  */
            if (box_fg(t))
                outb(0x43, v);
            return;
        }
        if (ch != 0)
            return;
        if (!(v & 0x30)) {              /* Latch                         */
            pit_now(t);
            pit_lat[t] = pit_cnt[t];
            pit_latd[t] = 1;
            pit_ff[t] = 0;
            return;
        }
        pit_acc_m[t] = (unsigned char)((v >> 4) & 3);
        pit_ff[t] = pit_wlo[t] = 0;
        return;
    }
    if (p == 0x42) {
        if (box_fg(t)) {
            outb(0x42, v);
            spk_ports(t, 1);                    /* weitere direkt        */
        }
        return;
    }
    if (p != 0x40)
        return;
    if (pit_acc_m[t] == 1)
        pit_rl[t] = (pit_rl[t] & 0xFF00) | v;
    else if (pit_acc_m[t] == 2)
        pit_rl[t] = (pit_rl[t] & 0x00FF) | (v << 8);
    else if (!pit_wlo[t]) {
        pit_rl[t] = (pit_rl[t] & 0xFF00) | v;
        pit_wlo[t] = 1;
        return;
    } else {
        pit_rl[t] = (pit_rl[t] & 0x00FF) | (v << 8);
        pit_wlo[t] = 0;
    }
    pit_run(t);                         /* neuer Teiler gilt ab jetzt    */
    pit_acc[t] = 0;
    pit_cnt[t] = (unsigned)pit_div(t);
    kpit_check(t);
}

/* 100 Hz des Kernels in 18,2 Hz der Box umrechnen; ohne eigene Takt-
   Routine nur den Zaehler im BIOS-Datenbereich weiterzaehlen */
static void account(int t)
{
    unsigned long now, d;
    if (pit_rl[t] || pm_hooked(t, 8) || !vec_def(8)) {  /* eigener Takt/Teiler */
        pit_run(t);                     /* Rate des Programms            */
        if (vt_pend[t] && !pm_hooked(t, 8) && vec_def(8) && vec_def(0x1C)) {
            bda_ticks(vt_pend[t]);      /* (Teiler ohne Routine)         */
            vt_pend[t] = 0;
        }
        vt_last[t] = get_ticks();
        return;
    }
    now = get_ticks();
    d = now - vt_last[t];
    vt_last[t] = now;
    if (d > 100) {                      /* lange nicht gelaufen (geparkt, */
        unsigned long tk = rtc_ticks(), o = ((unsigned long)rd16(BDA, 0x6E) << 16) | rd16(BDA, 0x6C);
        if (o > tk + 786000UL)          /* angehalten): Uhr der Box aus der */
            wr8(BDA, 0x70, 1);          /* Echtzeituhr neu (vorher gingen */
        wr16(BDA, 0x6C, (unsigned)tk);  /* die Sekunden verloren - CheckIt: */
        wr16(BDA, 0x6E, (unsigned)(tk >> 16));  /* DOS 17 s hinter RTC)  */
        d = 0;
    }
    vt_acc[t] += (unsigned)d * 182;
    while (vt_acc[t] >= 1000) {
        vt_acc[t] -= 1000;
        vt_pend[t]++;
    }
    if (vt_pend[t] && vec_def(0x1C)) {
        bda_ticks(vt_pend[t]);
        vt_pend[t] = 0;
    } else if (vt_pend[t] > 4)
        vt_pend[t] = 4;                 /* lange gesperrt: nicht nachholen */
}

/* beim Ruecksprung in die Box: ausstehenden Interrupt zustellen */
/* darf jetzt ein Takt zugestellt werden? Wie der PIC: jeder echte
   irq0 gibt hoechstens einen INT 8 frei, auch wenn er gerade den Kernel
   traf - zugestellt wird dann beim naechsten Ruecksprung in die Box.
   (Vorher: ausserhalb von irq0 hoechstens einer je Kerneltakt; mit
   schnellem PIT (DOOM 140 Hz) und vielen Fallen kamen nur ~87 Takte je
   Sekunde an, DOOM 1.9 lief bei viel Ton langsamer. Nachgeholte Takte
   direkt hintereinander -> DOS/4GW "transfer stack overflow".) */
static int t8_ok(int t)
{
    unsigned n = irq0_n;
    if (!from_tick && n == t8_last[t])
        return 0;
    t8_last[t] = n;
    return 1;
}

/* Umschalttasten im BIOS-Format (0040:0017/18) - der Kernel zaehlt
   anders (KBDSHIFT_*: 1 Shift, 2 Strg, 4 Alt, 8 AltGr). Die Oberflaeche
   von MSD/EDIT/QBASIC erkennt die blosse Alt-Taste nur hier. Sperrtasten
   (Bits 4-7) bleiben, wie sie im Datenbereich der Box stehen.
   -> AL = 0417h, AH = 0418h (fuer INT 16h 12h: Bit 0 Strg, 1 Alt links,
   2 Strg rechts, 3 Alt rechts) */
static unsigned bios_shift(void)
{
    unsigned k = kf_kbd_shift(), lo, hi;
    lo = (rd8(BDA, 0x17) & 0x80) |
         (k & (KBDSHIFT_NUM | KBDSHIFT_CAPS | KBDSHIFT_SCROLL)) |
         (k & KBDSHIFT_SHIFT ? 0x02 : 0) |
         (k & KBDSHIFT_CTRL ? 0x04 : 0) | (k & (KBDSHIFT_ALT | KBDSHIFT_ALTGR) ? 0x08 : 0);
    hi = (rd8(BDA, 0x18) & 0xF0) | (k & KBDSHIFT_CTRL ? 0x01 : 0) |
         (k & KBDSHIFT_ALT ? 0x02 : 0) | (k & KBDSHIFT_ALTGR ? 0x08 : 0);
    wr8(BDA, 0x17, (unsigned char)lo);
    wr8(BDA, 0x18, (unsigned char)hi);
    return lo | hi << 8;
}

static void kb_pump(int t);

static void inject(struct v86_iret *r, int t)
{
    if (sti_step[t]) {                          /* Einzelschritt erledigt */
        sti_step[t] = 0;
        r->eflags &= ~(unsigned long)FL_TF;
    }
    if (box_fg(t) && kf_kbd_shift() != shf_last[t]) {   /* 0417h nur bei */
        shf_last[t] = kf_kbd_shift();           /* Aenderung (wie das BIOS: */
        bios_shift();                           /* Speichertests legen dort */
    }                                           /* Muster ab - CheckIt)     */
    if ((spk[t] || spk_open[t] || vfgr[t]) && !box_fg(t)) {
        spk_hw(t, 0);                           /* hinten: stumm         */
        spk_ports(t, 0);
        vdd_bg(t);
    }
    if (pit_rl[t])
        kpit_check(t);                          /* vorn schneller Takt   */
    vga_freeze(t);
    account(t);
    vdd_tk(t);
    if (!(vfl[t] & FL_IF) || sti_sh)
        return;
    if (virq[t]) {                              /* Modul (Sound Blaster) */
        unsigned q;
        for (q = 2; q <= 7; q++) {
            unsigned m = 1 << q;
            if (!(virq[t] & m) || (visr[t] & ((m << 1) - 1)) || (vimr[t] & m))
                continue;
            if (!vec_def(8 + q)) {              /* Real-Mode-Handler     */
                virq[t] &= (unsigned char)~m;
                visr[t] |= (unsigned char)m;
                int_raw(r, t, 8 + q, (unsigned)r->eip);
                return;
            }
            if (!pm_hooked(t, 8 + q))
                virq[t] &= (unsigned char)~m;   /* niemand will ihn      */
        }
    }
    if (alrm_on[t] && (unsigned)get_ticks() - alrm_tk[t] >= 50) {
        unsigned now = (unsigned)rtc_stamp();   /* Weckzeit (INT 1Ah 06h): */
        alrm_tk[t] = (unsigned)get_ticks();     /* je halbe Sekunde pruefen */
        if (alrm_on[t] == 2 && now < alrm[t])
            alrm_on[t] = 1;                     /* Mitternacht vorbei      */
        else if (alrm_on[t] == 1 && now >= alrm[t]) {
            alrm_on[t] = 0;                     /* wie der BIOS-Handler    */
            int_raw(r, t, 0x4A, (unsigned)r->eip);  /* von IRQ 8: INT 4Ah */
            return;
        }
    }
    if (vt_pend[t] && !(visr[t] & 1) && !(vimr[t] & 1) && t8_ok(t)) {
        vt_pend[t]--;
        visr[t] |= 1;
        int_raw(r, t, 8, (unsigned)r->eip);
        return;
    }
    /* Bit 1: ein Programm hatte INT 9 (Real Mode oder DPMI). Gibt es ihn
       zurueck, liegen seine Tasten auch im Puffer der Sitzung (INT 16h
       kommt aus dem Kernel) - DOOM in einer Box mit FreeCOM: Pfeil hoch
       + Enter aus dem Spiel holten "doom" zurueck, es startete endlos neu */
    if (!vec_def(9) || pm_hooked(t, 9))
        k9_def[t] |= 2;
    else if (k9_def[t] & 2) {
        k9_def[t] &= (unsigned char)~2;
        if (box_fg(t))
            kf_kbd_flush();
    }
    if (vec_def(9))
        k9_def[t] |= 1;
    else if (k9_def[t] & 1) {
        /* INT 9 gerade umgebogen: was vorher im Ring lag, haette auf
           echter Hardware das BIOS laengst verarbeitet (bei uns kommt
           es ueber INT 16h aus dem Kernel). Sonst traf z. B. das
           Loslassen von Enter Diggers Handler, bevor der alten Vektor
           gemerkt hatte. */
        k9_def[t] &= (unsigned char)~1;
        if (!pm_hooked(t, 9))
            kq_t[t] = kq_h[t];
    }
    if (vec_def(9) && !pm_hooked(t, 9) && box_fg(t))
        kb_pump(t);                             /* wie das BIOS: Puffer 41Eh */
    if (!vec_def(9) && key_ready(t)) {
        key_t0[t] = get_ticks();
        vcur[t] = (unsigned char)kq_get(t);
        visr[t] |= 2;
        int_raw(r, t, 9, (unsigned)r->eip);
    }
}

/* aus irq0, wenn er die Box unterbrochen hat (nach preempt) */
void __far v86_tick(struct v86_iret *r)
{
    int t = current;
    if (t < 0 || !pt_box[t])
        return;
    from_tick = 1;                              /* je irq0 hoechstens ein */
    if (r->eflags & 0x20000UL) {                /* Takt (sonst schachteln */
        unsigned long ocs = r->cs, oip = r->eip;
        vme_in(r, t);                           /* nachgeholte Takte, DOS/4GW: */
        inject(r, t);
        if (r->cs == ocs && r->eip == oip)      /* nichts zugestellt:     */
            cb_tick(r, t);                      /* Rueckruf faellig (Maus)? */
        vme_out(r, t);
    } else {                                      /* "transfer stack overflow") */
        pvi_in(r, t);                           /* DPMI-Programm (Ring 3) */
        vga_freeze(t);
        if (pit_rl[t])
            kpit_check(t);
        dp_hwint(r, t);
        pvi_out(r, t);
    }
    from_tick = 0;
}

/* ---- Bildschirm (B800 der Box = Sitzungspuffer) ------------------------- */

#define AL  (*(unsigned char *)&f->eax)
#define AH  (*((unsigned char *)&f->eax + 1))
#define AX  (*(unsigned short *)&f->eax)
#define BL  (*(unsigned char *)&f->ebx)
#define BH  (*((unsigned char *)&f->ebx + 1))
#define BX  (*(unsigned short *)&f->ebx)
#define CL  (*(unsigned char *)&f->ecx)
#define CH  (*((unsigned char *)&f->ecx + 1))
#define CX  (*(unsigned short *)&f->ecx)
#define DL  (*(unsigned char *)&f->edx)
#define DH  (*((unsigned char *)&f->edx + 1))
#define DX  (*(unsigned short *)&f->edx)
#define BP  (*(unsigned short *)&f->ebp)
#define DI  (*(unsigned short *)&f->edi)

static unsigned char crow, ccol;        /* Cursor waehrend eines Dienstes */

static void cur_get(void)
{
    unsigned v = rd16(BDA, 0x50);
    ccol = (unsigned char)v;
    crow = (unsigned char)(v >> 8);
    if (ccol >= COLS) ccol = COLS - 1;
    if (crow >= ROWS) crow = ROWS - 1;
}

static void cur_put(int t)
{
    wr16(BDA, 0x50, (crow << 8) | ccol);
    if (!vhide[t])
        kf_vio_setcur(box_ses[t], crow, ccol);
}

static unsigned vcell(unsigned r, unsigned c)
{
    return rd16(VSEG, (r * COLS + c) * 2);
}

static void vset(unsigned r, unsigned c, unsigned cell)
{
    wr16(VSEG, (r * COLS + c) * 2, cell);
}

/* Fenster r1..r2 x c1..c2 um n Zeilen hoch (up) oder runter, n = 0 oder
   zu gross: leeren */
static void vscroll(int up, unsigned n, unsigned r1, unsigned c1, unsigned r2,
                    unsigned c2, unsigned char attr)
{
    unsigned r, c, h;
    if (r2 >= ROWS) r2 = ROWS - 1;
    if (c2 >= COLS) c2 = COLS - 1;
    if (r1 > r2 || c1 > c2)
        return;
    h = r2 - r1 + 1;
    if (n == 0 || n > h)
        n = h;
    for (r = 0; r < h; r++)
        for (c = c1; c <= c2; c++) {
            unsigned dst = up ? r1 + r : r2 - r;
            unsigned cell = 0x20 | (attr << 8);
            if (r + n < h)
                cell = vcell(up ? dst + n : dst - n, c);
            vset(dst, c, cell);
        }
}

static void newline(void)
{
    if (++crow >= ROWS) {
        vscroll(1, 1, 0, 0, ROWS - 1, COLS - 1, (unsigned char)(vcell(ROWS - 1, 0) >> 8));
        crow = ROWS - 1;
    }
}

static void advance(void)
{
    if (++ccol == COLS) {
        ccol = 0;
        newline();
    }
}

/* Zeichen wie BIOS-TTY (Attribut bleibt) */
static void vtty(unsigned char ch)
{
    switch (ch) {
    case 7:
        return;
    case 8:
        if (ccol)
            ccol--;
        return;
    case 10:
        newline();
        return;
    case 13:
        ccol = 0;
        return;
    default:
        vset(crow, ccol, (vcell(crow, ccol) & 0xFF00) | ch);
        advance();
    }
}

/* Attribut-Modusregister Bit 3: 1 = Blinken, 0 = helle Hintergruende */
static void blink_hw(int off)
{
    unsigned char v;
    inb(0x3DA);
    outb(0x3C0, 0x30);                          /* Index 10h, Bild an    */
    v = (unsigned char)inb(0x3C1);
    v = (unsigned char)(off ? v & ~8 : v | 8);
    outb(0x3C0, v);
}

/* Farbe i der Standardpalette im Textmodus (die ersten 64 = EGA-Farben
   rgbRGB, Rest 0), Anteil c (0 rot, 1 gruen, 2 blau) -> 0..63 */
static unsigned char dac_def(unsigned i, int c)
{
    if (i >= 64)
        return 0;
    return (unsigned char)(((i >> (2 - c)) & 1) * 42 + ((i >> (5 - c)) & 1) * 21);
}

static void int10(struct v86_frame *f, int t)
{
    unsigned i, r, c, n;

    cur_get();
    switch (AH) {
    case 0x00:                                  /* Modus                 */
        wr8(BDA, 0x49, AL & 0x7F);
        if ((AL & 0x7F) <= 3 || (AL & 0x7F) == 7) {
            /* Textbild der Box ist immer 80x25: Spalten/Zeilen/Seitengroesse
               neu setzen - ein Grafikmodus ueber das echte BIOS (DOOM: 13h)
               liess 40 Spalten stehen, gebundene Programme (EDIT im Stub)
               zeigten danach nichts */
            wr16(BDA, 0x4A, COLS);
            wr16(BDA, 0x4C, 4096);
            wr8(BDA, 0x84, ROWS - 1);
            if (!(AL & 0x80))
                vscroll(1, 0, 0, 0, ROWS - 1, COLS - 1, 7);
            crow = ccol = 0;
        }
        break;
    case 0x01:                                  /* Cursorform            */
        wr16(BDA, 0x60, CX);
        vhide[t] = (CH & 0x20) != 0;
        if (vhide[t])
            kf_vio_setcur(box_ses[t], 0xFF, 0);
        break;
    case 0x02:                                  /* Cursor setzen         */
        if (BH == 0 && DH < ROWS && DL < COLS) {
            crow = DH;
            ccol = DL;
        }
        break;
    case 0x03:
        DX = (crow << 8) | ccol;
        CX = rd16(BDA, 0x60);
        break;
    case 0x05:                                  /* nur Seite 0           */
        break;
    case 0x06: case 0x07:
        vscroll(AH == 6, AL, CH, CL, DH, DL, BH);
        break;
    case 0x08:
        AX = vcell(crow, ccol);
        break;
    case 0x09: case 0x0A:                       /* Zeichen (+Attribut) CX-mal */
        for (i = 0, r = crow, c = ccol; i < CX && r < ROWS; i++) {
            n = AH == 9 ? (BL << 8) | AL : (vcell(r, c) & 0xFF00) | AL;
            vset(r, c, n);
            if (++c == COLS)
                c = 0, r++;
        }
        break;
    case 0x0E:
        vtty(AL);
        break;
    case 0x0F:
        AL = rd8(BDA, 0x49);
        AH = COLS;
        BH = 0;
        break;
    case 0x10:                                  /* 1003h: Blinken/hell   */
        if (AL == 0x03) {
            noblink[t] = BL == 0;
            if (box_fg(t))
                blink_hw(noblink[t]);
        } else if (AL == 0x07) {                /* Attributregister lesen */
            static const unsigned char atr[16] = {  /* Textmodus 03h     */
                0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B,
                0x3C, 0x3D, 0x3E, 0x3F };
            BH = BL < 16 ? atr[BL] : 0;
        } else if (AL == 0x08)                  /* Rand                  */
            BH = 0;
        else if (AL == 0x15) {                  /* DAC-Farbe lesen       */
            DH = dac_def(BL, 0);
            CH = dac_def(BL, 1);
            CL = dac_def(BL, 2);
        } else if (AL == 0x17) {                /* DAC-Block ES:DX       */
            for (i = 0; i < CX * 3; i++)
                wr8((unsigned)f->r.es, DX + i, dac_def(BX + i / 3, i % 3));
        } else if (AL == 0x1A)                  /* DAC-Seite: Modus 0, 0 */
            BX = 0;
        break;
    case 0x11:
        if (AL == 0x30) {
            CX = 16;
            DL = ROWS - 1;
        }
        break;
    case 0x12:
        if (BL == 0x10) {
            BH = 0;                             /* Farbe                 */
            BL = 3;                             /* 256 KB                */
            CH = 0;                             /* Merkmalsbits          */
            CL = 9;                             /* Schalter: EGA Farbe   */
        }
        break;
    case 0x1C:                                  /* Zustand sichern: nur   */
        if (AL == 0) {                          /* BIOS-Daten (128 Byte)  */
            BX = 2;
        } else if (AL == 1 || AL == 2) {
            for (i = 0; i < 0x1E; i++)
                if (AL == 1)
                    wr8((unsigned)f->r.es, BX + i, rd8(BDA, 0x49 + i));
                else
                    wr8(BDA, 0x49 + i, rd8((unsigned)f->r.es, BX + i));
            for (i = 0; i < 7; i++)
                if (AL == 1)
                    wr8((unsigned)f->r.es, BX + 0x20 + i, rd8(BDA, 0x84 + i));
                else
                    wr8(BDA, 0x84 + i, rd8((unsigned)f->r.es, BX + 0x20 + i));
            if (AL == 2) {
                cur_get();                      /* Cursor aus dem BDA    */
                cur_put(t);
                AL = 0x1C;
                return;
            }
        }
        AL = 0x1C;
        break;
    case 0x13:                                  /* Zeichenkette ES:BP    */
        if (DH >= ROWS || DL >= COLS)
            break;
        r = crow;
        c = ccol;
        crow = DH;
        ccol = DL;
        for (i = 0, n = BP; i < CX; i++) {
            unsigned char ch = rd8((unsigned)f->r.es, n++), a = BL;
            if (AL & 2)
                a = rd8((unsigned)f->r.es, n++);
            if (ch == 7 || ch == 8 || ch == 10 || ch == 13)
                vtty(ch);
            else {
                vset(crow, ccol, (a << 8) | ch);
                advance();
            }
        }
        if (!(AL & 1)) {
            crow = (unsigned char)r;
            ccol = (unsigned char)c;
        }
        break;
    case 0x1A:
        if (AL == 0) {
            AL = 0x1A;
            BX = 0x0008;                        /* VGA Farbe             */
        }
        break;
    case 0x1B:                                  /* Zustand nach ES:DI    */
        if (BX)                                 /* (Lotus Works INSTALL: */
            break;                              /* fehlte -> Modus 7 und */
        {                                       /* Teilung durch 0)      */
            static const unsigned char sft[16] = {      /* bei F000:0880 */
                0xFF, 0xE0, 0x0F, 0, 0, 0, 0, 0x07, 2, 8, 0xFF, 0x0E, 0, 0, 0x3F, 0 };
            unsigned es = (unsigned)f->r.es, di = (unsigned)f->edi;
            cur_put(t);
            for (i = 0; i < 16; i++)
                wr8(VBIOS, 0x880 + i, sft[i]);
            for (i = 0; i < 64; i++)
                wr8(es, di + i, 0);
            wr16(es, di, 0x880);
            wr16(es, di + 2, VBIOS);
            for (i = 0; i < 0x1E; i++)      /* Modus .. aktive Seite: */
                wr8(es, di + 4 + i, rd8(BDA, 0x49 + i));   /* BDA 449h-466h */
            wr16(es, di + 0x1E, rd16(BDA, 0x63));
            wr8(es, di + 0x20, rd8(BDA, 0x65));
            wr8(es, di + 0x21, rd8(BDA, 0x66));
            wr8(es, di + 0x22, (unsigned char)(rd8(BDA, 0x84) + 1));
            wr16(es, di + 0x23, rd16(BDA, 0x85) ? rd16(BDA, 0x85) : 16);
            wr8(es, di + 0x25, 8);              /* VGA Farbe             */
            wr16(es, di + 0x27, 16);            /* Farben                */
            wr8(es, di + 0x29, 8);              /* Seiten                */
            wr8(es, di + 0x2A, 2);              /* 400 Zeilen            */
            wr8(es, di + 0x2D, 0x20);           /* Blinken an            */
            wr8(es, di + 0x31, 3);              /* 256 KB                */
            AL = 0x1B;
        }
        return;
    }
    cur_put(t);
}

/* Leerlauf wie IDLE_SENSITIVITY bei OS/2 (0-100, Vorgabe 75, 100 = aus):
   leere Tastaturabfrage, Uhr lesen (INT 1Ah 00h), Ruecklauf (3DAh) zaehlen
   je Kerneltakt; mehr als sens/4 + 1 davon -> bis zum naechsten Takt
   schlafen. Ohne INI-Wert nicht bei Spielen mit eigenem Takt (pit_rl) -
   Digger fragt in seiner Warteschleife staendig ab und lief langsamer. */
/* Hat die Box seit dem letzten v86_poll in den Textbildschirm geschrieben?
   (Dirty-Bit von B8/B9; v86_poll loescht es) */
static int scr_written(int t)
{
    unsigned short w[8];
    unsigned long e;

    if (!pt_box[t] || vga_raw[t])
        return 0;
    win_save(w);
    scratch_map(PG_LIN(pt_box[t]), 4095);
    e = get32(SCRATCH, 0xB8) | get32(SCRATCH, 0xB9);
    win_restore(w);
    return (e & PTE_D) != 0;
}

/* vid: Abfrage des Strahlruecklaufs (3DAh/3BAh) - wer dabei in den
   Bildschirm schreibt (NC, Programme gegen CGA-Schnee), wartet nicht */
static void idle_poll(int t, int vid)
{
    unsigned sens = idle_sens[t] ? idle_sens[t] - 1u : 75u, tk;
    if (sens >= 100 || (!idle_sens[t] && pit_rl[t]))
        return;
    tk = (unsigned)get_ticks();
    if (tk != idle_tk[t]) {
        idle_tk[t] = tk;
        idle_n[t] = 0;
    }
    if (++idle_n[t] >= sens / 4 + 1) {
        idle_n[t] = 0;
        if (vid && scr_written(t))
            return;
        kf_box_ctl(BOX_SLEEP, 1);
    }
}

/* ---- Dienste ------------------------------------------------------------ */

static unsigned char svc_cf, svc_zf;    /* 0xFF = unveraendert          */

/* PCI-Konfiguration ueber CF8h/CFCh (Mechanismus 1), nur lesend - PCI-
   Video-BIOSe lesen so ihre BARs */
static unsigned long pci_cfg[NTASK];

static unsigned long pci_pin(int t, unsigned p)
{
    if (p < 0xCFC)
        return pci_cfg[t] >> ((p - 0xCF8) * 8);
    if (!(pci_cfg[t] & 0x80000000UL))
        return 0xFFFFFFFFUL;
    return pci_read((unsigned)(pci_cfg[t] >> 8), (unsigned)pci_cfg[t] & 0xFC) >>
           ((p - 0xCFC) * 8);
}

/* INT 1Ah B1xxh, PCI-BIOS 2.1 lesend: PCI-Video-BIOSe suchen fuer VBE
   ihre eigene Karte (Cirrus GD54xx: ohne das scheitert 4F01h in der
   Box). Ueber die Busdienste des Kernels (bus.c); Schreiben wird nur
   bestaetigt, eine Box verstellt nichts am Bus. */
static void pci_svc(struct v86_frame *f)
{
    unsigned loc, n, i;
    unsigned long v;
    svc_cf = 1;
    switch (AL) {
    case 0x01:                                  /* da?                   */
        if (pci_read(0, 0) == 0xFFFFFFFFUL) {
            AH = 0x81;
            return;
        }
        AX = 0x0001;                            /* Mechanismus 1         */
        BX = 0x0210;
        CL = 0;
        f->edx = 0x20494350UL;                  /* "PCI "                */
        break;
    case 0x02:                                  /* Geraet CX, Hersteller DX */
        loc = pci_find(DX, CX, (unsigned)f->esi);
        if (loc == 0xFFFF) {
            AH = 0x86;
            return;
        }
        BX = loc;
        AH = 0;
        break;
    case 0x03:                                  /* Klasse ECX            */
        for (i = 0, n = (unsigned)f->esi; ; i++) {
            loc = pci_findclass((unsigned)(f->ecx >> 8), i);
            if (loc == 0xFFFF) {
                AH = 0x86;
                return;
            }
            if ((unsigned char)(pci_read(loc, 8) >> 8) == CL && !n--)
                break;
        }
        BX = loc;
        AH = 0;
        break;
    case 0x08: case 0x09: case 0x0A:            /* lesen Byte/Wort/Dword */
        v = pci_read(BX, DI & 0xFC) >> ((DI & 3) * 8);
        if (AL == 0x08)
            CL = (unsigned char)v;
        else if (AL == 0x09)
            CX = (unsigned)v;
        else
            f->ecx = v;
        AH = 0;
        break;
    case 0x0B: case 0x0C: case 0x0D:            /* schreiben: nur ja     */
        AH = 0;
        break;
    default:
        AH = 0x81;
        return;
    }
    svc_cf = 0;
}


/* Taste: warten (wait = 1) oder nachsehen; -> Zeichen | Scancode << 8,
   -1 = keine */
/* Tastaturpuffer der Box im BIOS-Datenbereich (41Ah Kopf, 41Ch Ende,
   Grenzen 480h/482h): dort legt INT 16h AH=05h Tasten ab - Lotus Works
   INSTALL schreibt eine hinein und wartet, bis AH=11h sie meldet */
static unsigned kb_lo(void)
{
    unsigned a = rd16(BDA, 0x80), b = rd16(BDA, 0x82);
    return a >= 0x1E && b > a + 2 && b <= 0x100 ? a : 0x1E;
}

static unsigned kb_hi(void)
{
    unsigned a = rd16(BDA, 0x80), b = rd16(BDA, 0x82);
    return a >= 0x1E && b > a + 2 && b <= 0x100 ? b : 0x3E;
}

static unsigned kb_next(unsigned p)
{
    p += 2;
    return p >= kb_hi() ? kb_lo() : p;
}

/* Getippte Tasten in den Puffer der Box legen, wie der INT 9 des BIOS -
   Programme lesen ihn auch direkt (Lotus Works INSTALL fragt nur INT 16h
   AH=02 und schaut in 41Ah/41Ch). INT 16h nimmt sie von dort (key). */
static void kb_pump(int t)
{
    unsigned i;
    int k;
    (void)t;
    for (;;) {
        i = rd16(BDA, 0x1C);
        if (i < kb_lo() || i >= kb_hi() || rd16(BDA, 0x1A) < kb_lo() ||
            rd16(BDA, 0x1A) >= kb_hi()) {
            wr16(BDA, 0x1A, kb_lo());           /* zerstoert: leeren      */
            wr16(BDA, 0x1C, i = kb_lo());
        }
        if (kb_next(i) == rd16(BDA, 0x1A) || (k = kf_kbd_peek()) < 0)
            return;
        k = kf_kbd_get();
        if ((k & 0xFF) == 0xE0 && ((unsigned)k >> 8 < 0x47 || (unsigned)k >> 8 > 0x53))
            k = 0xE0;                           /* wie key()              */
        wr16(BDA, i, (unsigned)k);
        wr16(BDA, 0x1C, kb_next(i));
    }
}

static long key(int t, int wait)       /* -1L: keine Taste (FFFFh ist eine) */
{
    int s = box_ses[t], k;
    unsigned h;
    for (;;) {
        h = rd16(BDA, 0x1A);
        if (h != rd16(BDA, 0x1C) && h >= kb_lo() && h < kb_hi()) {
            k = (int)rd16(BDA, h);      /* abgelegte Taste zuerst         */
            if (wait)
                wr16(BDA, 0x1A, kb_next(h));
            return (long)(unsigned)k;
        }
        if (fg == s) {
            k = wait ? kf_kbd_get() : kf_kbd_peek();
            if (k >= 0 && (k & 0xFF) == 0xE0 && ((unsigned)k >> 8 < 0x47 ||
                                                (unsigned)k >> 8 > 0x53))
                k = 0xE0;               /* Buchstabe E0h (852: Ó) wie Alt+224:
                                           AH = 0, sonst gilt er als graue Taste */
            if (k >= 0)
                return k;
        }
        if (!wait)
            return -1L;
        kf_box_ctl(BOX_SLEEP, 1);
        account(t);                     /* Takt im BIOS-Datenbereich     */
    }
}

static unsigned bcd(unsigned v)
{
    return ((v / 10) << 4) | (v % 10);
}
static unsigned ubcd(unsigned v)
{
    return (v >> 4) * 10 + (v & 15);
}

static void out_ch(int t, unsigned char ch)
{
    cur_get();
    vtty(ch);
    cur_put(t);
    (void)t;
}

/* INT 13h: Laufwerk 0 ist das Startabbild (nur lesen), sonst nichts */
static void int13x(struct v86_frame *f, int t)
{
    unsigned i, n, c, h, sct, lba, cyl;
    unsigned long lin;

    if (AH == 0x01) {                           /* letzter Status        */
        AH = rd8(BDA, 0x41);
        AL = 0;
        svc_cf = AH != 0;
        return;
    }
    if (!img_secs[t] || (DL != 0 && AH != 0x08)) {
        AH = DL >= 0x80 && AH == 0x15 ? 0 : 1;  /* keine Platte         */
        svc_cf = 1;
        return;
    }
    switch (AH) {
    case 0x00: case 0x0D:                       /* Reset                 */
        AH = 0;
        svc_cf = 0;
        return;
    case 0x02: case 0x04:                       /* lesen / pruefen       */
        cyl = CH | ((CL & 0xC0) << 2);
        sct = CL & 0x3F;
        h = DH;
        n = AL;
        if (sct == 0 || sct > img_spt[t] || h >= img_heads[t]) {
            AH = 4;
            svc_cf = 1;
            return;
        }
        lba = (cyl * img_heads[t] + h) * img_spt[t] + sct - 1;
        lin = ((unsigned long)(unsigned)f->r.es << 4) + BX;
        for (i = 0; i < n; i++, lba++, lin += 512) {
            if (lba >= img_secs[t] || kf_box_ctl(BOX_READ, lba)) {
                AX = (unsigned)(0x0400 | i);    /* Sektor nicht gefunden */
                svc_cf = 1;
                return;
            }
            if (AH == 0x02) {                   /* (VWIN erst nach dem   */
                vwin((unsigned)(lin >> 4));     /* Lesen: es kann schlafen) */
                far_copy(VWIN, (unsigned)lin & 15, KDATA, (unsigned)box_sec, 512);
            }
        }
        AH = 0;
        AL = (unsigned char)n;
        svc_cf = 0;
        return;
    case 0x03: case 0x05:                       /* schreibgeschuetzt     */
        AH = 3;
        svc_cf = 1;
        return;
    case 0x08:                                  /* Geometrie             */
        if (DL != 0) {
            AH = 1;
            svc_cf = 1;
            return;
        }
        c = img_secs[t] / (img_spt[t] * img_heads[t]) - 1;
        BL = img_secs[t] <= 720 ? 1 : img_secs[t] <= 1440 ? 3 :
             img_secs[t] <= 2400 ? 2 : 4;
        CH = (unsigned char)c;
        CL = (unsigned char)(img_spt[t] | ((c >> 2) & 0xC0));
        DH = (unsigned char)(img_heads[t] - 1);
        DL = 1;
        f->r.es = VBIOS;
        DI = DPT_OFF;
        AX = 0;
        svc_cf = 0;
        return;
    case 0x15:                                  /* Diskette ohne Wechsel- */
        AH = 1;                                 /* leitung               */
        svc_cf = 0;
        return;
    case 0x16:                                  /* nicht gewechselt      */
        AH = 0;
        svc_cf = 0;
        return;
    case 0x17: case 0x18:
        AH = 0;
        svc_cf = 0;
        return;
    }
    AH = 1;
    svc_cf = 1;
}

static void int13(struct v86_frame *f, int t)
{
    unsigned fn = AH;
    int13x(f, t);
    if (fn != 0x01 && fn != 0x08 && fn != 0x15)
        wr8(BDA, 0x41, AH);                     /* Status fuer 01h       */
}

static int a20_set(int t, int on);
static unsigned char a20_on[NTASK];

/* Interrupt n an die Module (vdd.c): Register der Box hin und zurueck
   -> 0 erledigt */
static int vdd_svc_int(int n, struct v86_frame *f, int t)
{
    struct vdd_call c;
    c.ax = AX; c.bx = BX; c.cx = CX; c.dx = DX;
    c.si = (unsigned short)f->esi; c.di = DI;
    c.ds = (unsigned short)f->r.ds; c.es = (unsigned short)f->r.es;
    c.flags = (unsigned short)f->r.eflags;
    c.cs = (unsigned short)f->r.cs; c.ip = (unsigned short)f->r.eip;
    c.ss = (unsigned short)f->r.ss; c.sp = (unsigned short)f->r.esp;
    c.val = (unsigned short)pm_esbad(t, (unsigned)f->r.es);
    if (vdd_int(t, n, &c) == VDD_NOTMINE)
        return 1;
    f->r.cs = c.cs; f->r.eip = c.ip;            /* (EMS 55h/56h)         */
    f->r.ss = c.ss; f->r.esp = (f->r.esp & 0xFFFF0000UL) | c.sp;
    AX = c.ax; BX = c.bx; CX = c.cx; DX = c.dx;
    *(unsigned short *)&f->esi = c.si; DI = c.di;
    f->r.ds = c.ds; f->r.es = c.es;
    *(unsigned short *)&f->r.eflags = (unsigned short)
        (((unsigned short)f->r.eflags & ~0x8D5u) | (c.flags & 0x8D5u));
    return 0;
}

/* Messung Windows 3.1 in der Box (vorlaeufig): INT 2Fh AH=16h/43h und
   INT 67h (EMS-Abfrage, VCPI DEh) ins DMESG, hoechstens WTR_MAX Zeilen je
   Box. '>' = Aufruf, '<' = Ergebnis. */
#define WTR_MAX 48
static char *hx2(char *o, unsigned v);

static int wtr_want(unsigned n, struct v86_frame *f)
{
    if (n == 0x2F)
        return (AH == 0x16 && AX != 0x1680) || AH == 0x43;
    if (n == 0x67)
        return AH == 0xDE || AH == 0x40 || AH == 0x41 || AH == 0x46;
    return 0;
}

static void wtrace(struct v86_frame *f, int t, unsigned n, char tag,
                   unsigned cs, unsigned ip)
{
    char m[64], *o = m;
    if (wtr_n[t] >= WTR_MAX)
        return;
    wtr_n[t]++;
    *o++ = 'W'; *o++ = 'I'; *o++ = 'N'; *o++ = ' '; *o++ = tag;
    o = hx2(o, n); *o++ = ' ';
    o = hx2(hx2(o, AH), AL); *o++ = ' ';
    o = hx2(hx2(o, BH), BL); *o++ = ' ';
    o = hx2(hx2(o, CH), CL); *o++ = ' ';
    o = hx2(hx2(o, DH), DL);
    if (tag == '>') {
        *o++ = ' ';
        o = hx2(hx2(o, cs >> 8), cs); *o++ = ':';
        o = hx2(hx2(o, ip >> 8), ip);
    } else {
        *o++ = ' '; *o++ = 'E'; *o++ = 'S'; *o++ = ':'; *o++ = 'D'; *o++ = 'I';
        *o++ = ' ';
        o = hx2(hx2(o, (unsigned)f->r.es >> 8), (unsigned)f->r.es); *o++ = ':';
        o = hx2(hx2(o, DI >> 8), DI);
    }
    *o++ = '\n';
    klog(KDATA, (unsigned)m, (unsigned)(o - m));
}

static void svc(int n, struct v86_frame *f, int t)
{
    unsigned i, off;
    unsigned long st;
    int k;

    switch (n) {
    case 0x08:                                  /* Takt (umgebogen, verkettet) */
        bda_ticks(1);
        visr[t] &= ~1;
        break;
    case 0x09:                                  /* Tastatur (verkettet)  */
        visr[t] &= ~2;                          /* wie das BIOS: Taste in */
        if (box_fg(t))                          /* den Puffer 41Eh (Lotus */
            kb_pump(t);                         /* Works INSTALL kettet)  */
        break;
    case 0x0D:                                  /* IRQ 5/7 ohne Handler: */
        visr[t] &= ~0x20;                       /* quittieren            */
        break;
    case 0x0F:
        visr[t] &= ~0x80;
        break;
    case 0x10:
        int10(f, t);
        break;
    case 0x11:
        AX = rd16(BDA, 0x10);
        break;
    case 0x12:
        AX = rd16(BDA, 0x13);
        break;
    case 0x13:                                  /* Diskette = Startabbild */
        int13(f, t);
        break;
    case 0x15:
        if (AH == 0x86) {                       /* warten CX:DX us       */
            kf_box_ctl(BOX_SLEEP, CX * 7 + DX / 10000 + 1);
            svc_cf = 0;
        } else if (AH == 0x84 && DX <= 1) {     /* Joystick: Tasten bzw.  */
            unsigned ja[4], jb;                 /* Achsen (in us statt    */
            jb = box_fg(t) ? joy_read(ja) : 0xFF;   /* BIOS-Schleifen)    */
            if (!box_fg(t))
                ja[0] = ja[1] = ja[2] = ja[3] = 0;
            if (DX == 0)
                AL = (unsigned char)(jb & 0xF0);
            else {
                AX = ja[0]; BX = ja[1]; CX = ja[2]; DX = ja[3];
            }
            svc_cf = 0;
        } else if (AH == 0x88) {                /* erweiterter Speicher  */
            AX = 0;
            svc_cf = 0;
        } else if (AH == 0xC0) {                /* Konfigurationstabelle */
            i = rd16(VBIOS, 0xE6F5);            /* die des echten BIOS (feste */
            if (i >= 8 && i <= 16 &&            /* Stelle E6F5h, Seite FE = */
                rd8(VBIOS, 0xE6F7) == rd8(VBIOS, 0xFFFE)) {  /* ROM 1:1):  */
                for (k = 2; k < 10; k++)        /* Modell, Untermodell, Re-  */
                    wr8(VBIOS, CFG_OFF + k, rd8(VBIOS, 0xE6F5 + k));  /* vision */
                wr8(VBIOS, CFG_OFF + 5,         /* ohne EBDA und INT 15h 4Fh */
                    (unsigned char)(rd8(VBIOS, CFG_OFF + 5) & ~0x14)); /* aus INT 9 */
            }
            f->r.es = VBIOS;
            BX = CFG_OFF;
            AH = 0;
            svc_cf = 0;
        } else if (AH == 0x90 || AH == 0x91) {  /* Geraet beschaeftigt/  */
            AH = 0;                             /* fertig: nichts zu tun */
            svc_cf = 0;
        } else if (AH == 0x24 && AL <= 3) {     /* A20 (wie ueber XMS)   */
            if (AL < 2)
                a20_set(t, AL);
            else if (AL == 2)
                AL = a20_on[t];
            else
                BX = 0;                         /* weder 8042 noch 92h   */
            AH = 0;
            svc_cf = 0;
        } else if (AH != 0x4F) {
            AH = 0x86;
            svc_cf = 1;
        }
        break;
    case 0x16:                                  /* Tastatur              */
        if (AH == 0x00 || AH == 0x10) {
            i = AH;
            AX = (unsigned)key(t, 1);
            if (i == 0 && AL == 0xE0 && AH >= 0x47 && AH <= 0x53)
                AL = 0;                         /* graue Tasten wie AT-BIOS 00h */
        } else if (AH == 0x01 || AH == 0x11) {
            long kk = key(t, 0);
            svc_zf = kk < 0;
            if (kk >= 0) {
                AX = (unsigned)kk;
                idle_n[t] = 0;
            } else
                idle_poll(t, 0);                /* fragt nur ab          */
        } else if (AH == 0x02 || AH == 0x12) {
            k = bios_shift();
            AL = (unsigned char)k;
            if (AH == 0x12)
                AH = (unsigned char)(k >> 8);
        }
        else if (AH == 0x09)                    /* Faehigkeiten: 0305h,  */
            AL = 0x3C;                          /* 0306h, 0Ah, 1xh       */
        else if (AH == 0x0A)                    /* Tastaturkennung MF2   */
            BX = 0x41AB;
        else if (AX == 0x0305 && box_fg(t))     /* Wiederholrate setzen  */
            kf_kbd_rate(((BH & 3) << 5) | (BL & 0x1F));   /* (MODE)    */
        else if (AX == 0x0306) {                /* Wiederholrate abfragen */
            k = kf_kbd_rate(0xFFFFu);
            BX = ((k & 0x60) << 3) | (k & 0x1F);
        }
        else if (AH == 0x05) {                  /* Taste CX ablegen      */
            i = rd16(BDA, 0x1C);
            if (i < kb_lo() || i >= kb_hi())
                i = kb_lo(), wr16(BDA, 0x1A, i);
            if (kb_next(i) == rd16(BDA, 0x1A))
                AL = 1;                         /* Puffer voll           */
            else {
                wr16(BDA, i, CX);
                wr16(BDA, 0x1C, kb_next(i));
                AL = 0;
            }
        }
        break;
    case 0x14: case 0x17:                       /* COM/LPT: Modul VCOM.SYS */
        if (vdd_svc_int(n, f, t))
            AH = n == 0x17 ? 0x30 : 0x80;       /* ohne Modul: kein Port  */
        break;
    case 0x18: case 0x19:                       /* kein BASIC, kein Neustart */
    case 0x20:
        kf_box_ctl(BOX_EXIT, 0);
        break;
    case 0x1A:                                  /* Uhr                   */
        if (AH == 0xB1) {                       /* PCI-BIOS (Video-BIOS) */
            pci_svc(f);
            break;
        }
        st = rtc_stamp();
        if (AH == 0x00) {
            idle_poll(t, 0);                    /* Warteschleife?        */
            DX = rd16(BDA, 0x6C);
            CX = rd16(BDA, 0x6E);
            AL = rd8(BDA, 0x70);
            wr8(BDA, 0x70, 0);
        } else if (AH == 0x01) {
            wr16(BDA, 0x6C, DX);
            wr16(BDA, 0x6E, CX);
        } else if (AH == 0x02) {
            i = (unsigned)st;
            CH = (unsigned char)bcd(i >> 11);
            CL = (unsigned char)bcd((i >> 5) & 63);
            DH = (unsigned char)bcd((i & 31) << 1);
            DL = 0;
            svc_cf = 0;
        } else if (AH == 0x06) {                /* Weckzeit setzen: INT 4Ah */
            if (alrm_on[t]) {                   /* zur Zeit (inject)       */
                svc_cf = 1;                     /* schon gesetzt: CF       */
                break;
            }
            alrm[t] = (ubcd(CH) << 11) | (ubcd(CL) << 5) | (ubcd(DH) >> 1);
            alrm_on[t] = alrm[t] <= (unsigned)st ? 2 : 1;
            alrm_tk[t] = (unsigned)get_ticks();
            svc_cf = 0;
        } else if (AH == 0x07) {                /* Weckzeit loeschen       */
            alrm_on[t] = 0;
            svc_cf = 0;
        } else if (AH == 0x04) {
            i = (unsigned)(st >> 16);
            k = 1980 + (i >> 9);
            CH = (unsigned char)bcd(k / 100);
            CL = (unsigned char)bcd(k % 100);
            DH = (unsigned char)bcd((i >> 5) & 15);
            DL = (unsigned char)bcd(i & 31);
            svc_cf = 0;
        }
        break;
    case 0x21:                                  /* DOS (bis Stufe 3: Auszug) */
        switch (AH) {
        case 0x01:
            AL = (unsigned char)key(t, 1);
            out_ch(t, AL);
            break;
        case 0x02:
            out_ch(t, DL);
            break;
        case 0x06:
            if (DL != 0xFF)
                out_ch(t, DL);
            else {
                svc_zf = key(t, 0) < 0;
                AL = svc_zf ? 0 : (unsigned char)key(t, 1);
            }
            break;
        case 0x07: case 0x08:
            AL = (unsigned char)key(t, 1);
            break;
        case 0x09:                              /* Text bis '$'          */
            cur_get();
            for (off = DX, i = 0; i < 0xFFFF; i++, off++) {
                unsigned char ch = rd8((unsigned)f->r.ds, off);
                if (ch == '$')
                    break;
                vtty(ch);
            }
            cur_put(t);
            break;
        case 0x0B:
            AL = key(t, 0) < 0 ? 0 : 0xFF;
            break;
        case 0x25:                              /* Vektor setzen         */
            wr16(0, AL * 4, DX);
            wr16(0, AL * 4 + 2, (unsigned)f->r.ds);
            break;
        case 0x2A:                              /* Datum                 */
            st = rtc_stamp();
            i = (unsigned)(st >> 16);
            CX = 1980 + (i >> 9);
            DH = (unsigned char)((i >> 5) & 15);
            DL = (unsigned char)(i & 31);
            AL = 0;
            break;
        case 0x2C:                              /* Uhrzeit               */
            st = rtc_stamp();
            i = (unsigned)st;
            CH = (unsigned char)(i >> 11);
            CL = (unsigned char)((i >> 5) & 63);
            DH = (unsigned char)((i & 31) << 1);
            DL = 0;
            break;
        case 0x30:                              /* Version 6.00          */
            AX = 0x0006;
            BX = 0;
            CX = 0;
            break;
        case 0x35:                              /* Vektor holen          */
            BX = rd16(0, AL * 4);
            f->r.es = rd16(0, AL * 4 + 2);
            break;
        case 0x40:                              /* Schreiben (1/2)       */
            if (BX == 1 || BX == 2) {
                cur_get();
                for (i = 0, off = DX; i < CX; i++, off++)
                    vtty(rd8((unsigned)f->r.ds, off));
                cur_put(t);
                AX = CX;
                svc_cf = 0;
            } else {
                AX = 6;                         /* ungueltiges Handle    */
                svc_cf = 1;
            }
            break;
        case 0x4C:
            kf_box_ctl(BOX_EXIT, AL);
            break;
        default:
            AL = 0;
            break;
        }
        break;
    case 0x33:                                  /* Maus: VMOUSE.SYS      */
        vdd_svc_int(0x33, f, t);
        break;
    case 0x28:                                  /* DOS wartet: abgeben   */
        kf_box_ctl(BOX_SLEEP, 1);
        break;
    case 0x67:                                  /* EMS: VEMS.SYS         */
        k = wtr_want(0x67, f);
        if (k)
            wtrace(f, t, 0x67, '>', rd16(f->r.ss, (unsigned)f->r.esp + 2),
                   rd16(f->r.ss, (unsigned)f->r.esp));
        if (vdd_svc_int(0x67, f, t))
            AH = 0x80;                          /* kein EMS              */
        if (k)
            wtrace(f, t, 0x67, '<', 0, 0);
        break;
    case 0x29:                                  /* schnelle Ausgabe      */
        out_ch(t, AL);
        break;
    case 0x2F:
        if (wtr_want(0x2F, f))
            wtrace(f, t, 0x2F, '>', rd16(f->r.ss, (unsigned)f->r.esp + 2),
                   rd16(f->r.ss, (unsigned)f->r.esp));
        if (AX == 0x1680) {                     /* Zeitscheibe abgeben   */
            kf_box_ctl(BOX_SLEEP, 1);
            AL = 0;
        }
        break;
    case 0xE0:                                  /* Box-Schnittstelle     */
        if (AH == 0x00) {                       /* Befehlszeile -> ES:DI */
            const char *a;
            for (i = 0; i < sizeof(hwvec); i++)  /* Vektoren merken    */
                ivt_save[t][i] = ((unsigned long)rd16(0, hwvec[i] * 4 + 2) << 16) |
                                 rd16(0, hwvec[i] * 4);
            cb_prog(t);                         /* Rueckrufe, Maus (Module) */
            a = (const char *)kf_box_ctl(BOX_ARGS, 0);
            for (i = 0; a[i] && i < 120; i++)
                wr8((unsigned)f->r.es, DI + i, a[i]);
            CX = i;
        } else if (AH == 0x01)
            kf_box_ctl(BOX_EXIT, AL);
        else if (AH == 0x1B) {                  /* INT 2Fh DX nachbilden */
            AX = DX;                            /* (BOXSHELL, VME)       */
            if (wtr_want(0x2F, f))
                wtrace(f, t, 0x2F, 'B', 0, 0);
            svc_zf = mon2f(f);
        } else if (AH == 0x1C)                  /* BOXSHELL haengt in 2Fh */
            b2f[t] = 1;
        else if (AH == 0x30)                    /* DEM-Auftrag (ATDOSCM)  */
            redir_cm(f, t);
        else if (AH == 0x31) {                  /* eigener Kern: AL 1 =   */
            if (AL == 1)                        /* melden, 0 = fragen     */
                box_cm[t] = 1;
            if (box_cm[t])
                AX = 0x4D43;                    /* 'CM'                   */
        }
        else if (AH == 0x21)                    /* Laufwerk AL wie lokal  */
            svc_cf = redir_dinfo(f, t) != 0;    /* (IOCTL, BOXSHELL)      */
        else if (AH == 0x20) {                  /* MSCDEX (INT 2Fh 15xxh) */
            k = mscdex(f, t);
            svc_zf = k >= 0;
            if (k >= 0)
                svc_cf = k > 0;
        }
        else if (AH == 0x10) {                  /* Redirector (BOXSHELL) */
            k = redir(f, t);
            svc_zf = k >= 0;                    /* ZF: erledigt          */
            if (k > 0) {
                AX = (unsigned)k;
                svc_cf = 1;
            } else if (k == 0)
                svc_cf = 0;
        } else if (AH == 0x1D) {                /* umgeleitete Handles   */
            AL = (unsigned char)kf_box_ctl(BOX_STDRED, 0);
        }
        else if (AH == 0x1E)                    /* Handle AL lesen/schreiben */
            AX = redir_std(t, AL < 3 ? AL : 1, (unsigned)f->r.es, DI, CX);
        else if (AH == 0x22) {                  /* Name des Abbilds ohne */
            extern char box_iname[][13];        /* .IMG nach ES:DI (Werk- */
            for (i = 0; box_iname[t][i] && box_iname[t][i] != '.'; i++)  /* zeuge */
                wr8((unsigned)f->r.es, DI + i, box_iname[t][i]);  /* des DOS) */
            wr8((unsigned)f->r.es, DI + i, 0);
        }
        else if (AH == 0x13)                    /* Startlaufwerk (0 = A:) */
            AL = (unsigned char)kf_box_ctl(BOX_SYSDRV, 0);
        else if (AH == 0x17) {                  /* EMS-Grenze (BX KB, DOSBOX.INI) */
            unsigned pg = BX >> 4;
            ems_lim[t] = (pg > 2048 ? 2048 : pg) + 1;   /* bis 32 MB */
        } else if (AH == 0x18)                  /* Hilfen an (BL Bits 0-2) */
            box_hk[t] = (unsigned char)((~BX & 15) | (BX & (HK_ADLIB | HK_HWT)));
        else if (AH == 0x19)                    /* IDLE_SENSITIVITY + 1 (0 = Vorgabe) */
            idle_sens[t] = (unsigned char)(BX > 101 ? 101 : BX);
        else if (AH == 0x16) {                  /* XMS-Grenze (BX KB, DOSBOX.INI) */
            unsigned pg = BX >> 2;
            xms_lim[t] = (pg > 0x4000 - 0x110 ? 0x4000 - 0x110 : pg) + 1;  /* bis 63.424 KB */
            xms_snap[t] = 0;                    /* BOXSHELL liest gleich CMOS */
        } else if (AH == 0x15) {                /* Programm fertig (AL):  */
            for (i = 0; i < sizeof(hwvec); i++) {   /* Haken zuruecknehmen */
                wr16(0, hwvec[i] * 4, (unsigned)ivt_save[t][i]);
                wr16(0, hwvec[i] * 4 + 2, (unsigned)(ivt_save[t][i] >> 16));
            }
            visr[t] = vimr[t] = 0;              /* PIC wie neu            */
            vt_pend[t] = 0;
            kq_h[t] = kq_t[t] = 0;
            kf_box_ctl(BOX_PARK, AL);           /* parken oder beenden    */
        }
        else if (AH == 0x14) {                  /* Verzeichnis des Aufrufers */
            static char cd[132];                /* "C:\DOS" (8.3) -> ES:DI */
            cd[0] = 0;
            kf_box_fapi(FAPI_QCURDIR, KDATA, (unsigned)cd, 0, 0);
            for (i = 0; cd[i] && i < 127; i++)
                wr8((unsigned)f->r.es, DI + i, cd[i]);
            wr8((unsigned)f->r.es, DI + i, 0);
        }
        else if (AH == 0x11)                    /* SDA bei DS:SI, BL OEM */
            redir_sda(t, (unsigned)f->r.ds, (unsigned)f->esi, (unsigned)f->ebx & 0xFF);
        else if (AH == 0x12) {                  /* AT-DOS-Laufwerke      */
            AX = (unsigned)kf_box_ctl(BOX_DRIVES, 0);
            DX = (unsigned)kf_box_ctl(BOX_DRIVES, 1);
        }
        break;
    }
}

/* Dienst n mit Flag-Ergebnis; fl: FLAGS, die danach gelten sollen */
static unsigned run_svc(int n, struct v86_frame *f, int t, unsigned fl)
{
    svc_cf = svc_zf = 0xFF;
    svc(n, f, t);
    if (svc_cf != 0xFF)
        fl = svc_cf ? fl | 1 : fl & ~1;
    if (svc_zf != 0xFF)
        fl = svc_zf ? fl | 0x40 : fl & ~0x40;
    return fl;
}


/* ---- Virtuelle VGA, Stufe 1: Grafik ueber das echte Video-BIOS -----------
   Verlangt die Box einen Grafikmodus (INT 10h 00h, VESA 4F02h), bekommt sie
   die Karte: A0000-BFFFF physisch, Ports 3B0h-3DFh in der I/O-Bitmap frei,
   INT 10h geht an das Video-BIOS (Vektor aus der echten IVT, Zeichensatz-
   Vektoren 1Fh/43h/6Dh dazu). Die Sitzung steht dann im Rohmodus
   (vgagfx.c RAWGFX): beim Wechsel sichert der Kernel Register und alle
   vier Ebenen, die Box steht im Hintergrund still (vga_freeze), bis sie
   wieder vorn ist. Ein Textmodus (00h mit 0-3/7) gibt die Karte zurueck,
   danach wieder die Nachbildung in den Sitzungspuffer. */
int __far vio_setgfx(int s, int t, unsigned mode);  /* vgagfx.c (KTEXT2) */
#define RAWGFX 0x80                     /* wie vioint.h */
extern unsigned char hw_gfx;
static unsigned long vga_spare[NTASK];  /* Ersatzseite A0000-BFFFF       */

extern unsigned char vga_ok;            /* start.asm                     */
static void vga_ports(int t, int on)
{
    unsigned i;
    if (on && !vga_ok)                  /* EGA/CGA: Register nicht lesbar -
                                           gesperrt lassen, das Grafikmodul
                                           schreibt jeden Zugriff mit    */
        return;
    for (i = 0x3B0 >> 3; i <= 0x3DF >> 3; i++)
        tss[t].iob[i] = on ? 0 : 0xFF;
    if (on) {                           /* VBE der Bochs-/QEMU-/Proxmox- */
        tss[t].iob[0x1CE >> 3] &= 0x3F; /* Standard-VGA (1CEh/1CFh); ein  */
        tss[t].iob[0x1D0 >> 3] &= 0xFE; /* Wort an 1CFh beruehrt auch 1D0h
                                           (sonst liest das BIOS FFFFh und
                                           4F05h scheitert)              */
    } else {
        tss[t].iob[0x1CE >> 3] |= 0xC0;
        tss[t].iob[0x1D0 >> 3] |= 0x01;
    }
}

static int vga_enter(int t)
{
    unsigned i;
    unsigned long v[3];
    int s = box_ses[t];
    if (s < 0 || vio_setgfx(s, t, RAWGFX))
        return -1;
    scratch_map(0, 0x3FF);                      /* echte IVT             */
    v[0] = get32(SCRATCH, 0x1F);
    v[1] = get32(SCRATCH, 0x43);
    v[2] = get32(SCRATCH, 0x6D);
    wr16(0, 0x1F * 4, (unsigned)v[0]); wr16(0, 0x1F * 4 + 2, (unsigned)(v[0] >> 16));
    wr16(0, 0x43 * 4, (unsigned)v[1]); wr16(0, 0x43 * 4 + 2, (unsigned)(v[1] >> 16));
    wr16(0, 0x6D * 4, (unsigned)v[2]); wr16(0, 0x6D * 4 + 2, (unsigned)(v[2] >> 16));
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    vga_spare[t] = get32(SCRATCH2, 0xA0);
    for (i = 0xA0; i < 0xC0; i++)
        put32(SCRATCH2, i, ((unsigned long)i << 12) | PTE_BOX);
    vga_ports(t, 1);
    vga_raw[t] = 1;
    tlb(t);
    return 0;
}

static void vga_leave(int t)
{
    unsigned i;
    unsigned long b;
    int s = box_ses[t];
    if (!vga_raw[t])
        return;
    if (vga_bgk[t] == 4)
        vga_svmap(t, 0);                        /* nicht in freie Seiten */
    vga_raw[t] = 0;
    vga_bgk[t] = 0;
    vga_ports(t, 0);
    rom_uncow(t);                               /* BIOS-Kopien verwerfen  */
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    for (i = 0xA0; i < 0xC0; i++)
        put32(SCRATCH2, i, vga_spare[t]);
    if (s >= 0) {
        b = kf_vio_buf_lin(s);
        put32(SCRATCH2, 0xB8, b | PTE_BOX);
        put32(SCRATCH2, 0xB9, (b + 4096) | PTE_BOX);
        vio_setgfx(s, t, 0);                    /* Karte auf Text, neu   */
        kf_vio_dirty(s);
    }
    tlb(t);
}

/* ---- Virtuelle VGA, Stufe 2: Grafik-Boxen im Hintergrund ---------------
   Wie VVGA bei OS/2 2.x, aber nur fuer Modi, deren Bild fuer die CPU ein
   Stueck linearer Speicher ist: 13h (Chain-4, A0000 64 KB) und CGA 4-6
   (B8000 32 KB), mit einfachen Registern. gfx_out (vgagfx.c raw_out)
   legt dann zusaetzlich die Sicht der CPU in einen Schatten im Puffer
   der Sitzung (RAW_LIN) und vermerkt die Art (RAW_KIND); die Seiten-
   tabelle der Box zeigt hinten dorthin - Schreiben kostet nichts. Die
   VGA-Ports werden hinten in die gesicherte Registerkopie nachgebildet
   (Palette, CRTC, Sequenzer, Grafik, Attribut, 3DAh). Was die Abbildung
   aendern wuerde (Chain-4/Odd-Even, Schreibmodus, Bitmaske ...), das
   Video-BIOS, planare Modi, Mode X und SVGA: die Box wartet, bis sie
   vorn ist, und macht dann auf der Karte weiter. raw_in schreibt den
   Schatten beim Zurueckholen ueber die Ebenen. */
unsigned long __far vio_raw_buf(int s);         /* vgagfx.c (KTEXT2) */
#define RAW_REGS  0x40000UL                     /* wie vioint.h          */
#define RAW_LIN   0x41000UL
#define RAW_KIND  61
#define RAW_ALLOW 62
#define RAW_VMODE 0x350                         /* VESA-Modus (vioint.h)  */
#define RAW_SVPG    0x354                       /* VESA hinten (Art 4):  */
#define RAW_SVPHYS  0x360                       /*   Sicherung, LFB, Groesse, */
#define RAW_SVBYTES 0x364                       /*   Granularitaet, Fenster */
#define RAW_SVGMUL  0x368
#define RAW_SVSPLIT 0x36A
#define RAW_SVBANK  0x36C
#define RAW_SVOK    0x35A
#define NDPP      4                             /* DPMI 0800h je Box     */
static struct { unsigned long lpg, npg, ppg; } dp_phys[NTASK][NDPP];
static void pte_set(int t, unsigned i, unsigned long e);
static int lpte_set(int t, unsigned long lp, unsigned long e);
#define RAW_ST    0x340                         /* Port-Zustand hinten:  */
#define ST_SEQ    0                             /* Index Sequenzer       */
#define ST_GC     1                             /* Index Grafik          */
#define ST_AFF    2                             /* Attribut: Flipflop    */
#define ST_AIDX   3                             /*   und Index           */
#define ST_WI     4                             /* DAC schreiben: Farbe  */
#define ST_WC     5                             /*   Anteil              */
#define ST_RI     6                             /* DAC lesen             */
#define ST_RC     7

/* Registerkopie im Puffer der Sitzung (SCRATCH, Interrupts aus) */
static unsigned char __far *vga_vr(int t)
{
    unsigned long b = vio_raw_buf(box_ses[t]);
    if (!b)
        return 0;
    scratch_map(b + RAW_REGS, 1023);
    return MK_FP(SCRATCH, 0);
}

/* Box vorn auf der Karte (k = 0) oder hinten auf dem Schatten */
/* planar hinten (Art 2): A0000-AFFFF zeigt auf die Ebene im Puffer, in
   die die Karte jetzt schriebe - nur bei einfachen Registern (Schreib-
   modus 0 ohne Set/Reset, Drehen, Verknuepfung, Bitmaske FFh, Lesemodus
   0) und genau einer Ebene in der Map Mask; Map Mask 0: Read Map nur
   lesend. Sonst nicht eingeblendet: der Seitenfehler laesst die Box
   warten, bis sie vorn ist (vga_pf). Nach jedem Umstellen neu. */
static void vga_plmap(int t)
{
    unsigned char __far *q = vga_vr(t);
    unsigned i, mm, p = 4, rw = PTE_BOX;
    unsigned long b;
    if (!q)
        return;
    b = vio_raw_buf(box_ses[t]);
    mm = q[3] & 15;                             /* SEQ 2 Map Mask        */
    if (q[32] || q[34] || (q[36] & 0x0B) || q[39] != 0xFF)
        mm = 0xFF;
    if (mm == 1 || mm == 2 || mm == 4 || mm == 8)
        p = mm == 1 ? 0 : mm == 2 ? 1 : mm == 4 ? 2 : 3;
    else if (mm == 0) {
        p = q[35] & 3;                          /* GC 4 Read Map         */
        rw = PTE_P | PTE_U;
    }
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    for (i = 0; i < 16; i++)
        put32(SCRATCH2, 0xA0 + i, p < 4 ? (b + ((unsigned long)p << 16) +
                                           ((unsigned long)i << 12)) | rw : 0);
    tlb(t);
}

/* SVGA hinten (Art 4, VESA-Modus): gfx_out hat Zustand und Bild in
   eigene Seiten gesichert (vgagfx.c raw_svout); dorthin zeigen hinten das
   Fenster A0000-AFFFF (Stellung wie zuletzt per 4F05h) bzw. die per DPMI
   0800h eingeblendeten Seiten des linearen Bildspeichers. Was ausserhalb
   des Bildes liegt (Speicher hinter dem Bild, geteilte Fenster mit
   verschiedener Stellung), fehlt: der Seitenfehler laesst die Box warten.
   bg = 0: LFB wieder auf die Karte (das Fenster stellt vga_map). */
static void vga_svmap(int t, int bg)
{
    unsigned char __far *q;
    unsigned long phys = 0, bytes = 0, sv = 0, np, off = 0, o, e, pp, i;
    unsigned gmul = 0, ba = 0, bb = 0, k, f;
    unsigned char split = 0;
    f = irq_off();
    if (bg && (q = vga_vr(t)) != 0 && q[RAW_SVOK]) {
        sv = PG_LIN(*(pg_t __far *)(q + RAW_SVPG));
        phys = *(unsigned long __far *)(q + RAW_SVPHYS);
        bytes = *(unsigned long __far *)(q + RAW_SVBYTES);
        gmul = *(unsigned __far *)(q + RAW_SVGMUL);
        split = q[RAW_SVSPLIT];
        ba = *(unsigned __far *)(q + RAW_SVBANK);
        bb = *(unsigned __far *)(q + RAW_SVBANK + 2);
    }
    irq_restore(f);
    np = (bytes + 4095) >> 12;
    if (bg && !phys) {                          /* Fenster A0000         */
        off = (unsigned long)ba << 16;          /* ba * 64 KB / gmul     */
        for (k = gmul; k > 1; k >>= 1)          /* (gmul ist 2^n)        */
            off >>= 1;
        for (k = 0; k < 16; k++) {
            o = off + ((unsigned long)k << 12);
            pte_set(t, 0xA0 + k, sv && gmul && (!split || ba == bb) && o < bytes ?
                    (sv + o) | PTE_BOX : 0);
        }
    }
    for (k = 0; k < NDPP; k++)                  /* LFB ueber DPMI 0800h  */
        for (i = 0; i < dp_phys[t][k].npg; i++) {
            pp = dp_phys[t][k].ppg + i;
            if (!bg)
                e = PG_LIN(pp) | PTE_BOX;
            else if (sv && phys && pp >= (phys >> 12) && pp - (phys >> 12) < np)
                e = (sv + ((pp - (phys >> 12)) << 12)) | PTE_BOX;
            else
                e = 0;
            lpte_set(t, dp_phys[t][k].lpg + i, e);
        }
    tlb(t);
}

static void vga_map(int t, unsigned k)
{
    unsigned i, a = k == 1 ? 0xA0 : 0xB8, n = k == 1 ? 16 : 8;
    unsigned long b = k ? vio_raw_buf(box_ses[t]) + RAW_LIN : 0;
    if (!k && vga_bgk[t] == 4)
        vga_svmap(t, 0);                        /* LFB wieder auf die Karte */
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    for (i = 0xA0; i < 0xC0; i++)
        put32(SCRATCH2, i, k ? vga_spare[t] : ((unsigned long)i << 12) | PTE_BOX);
    if (k & 1)
        for (i = 0; i < n; i++)
            put32(SCRATCH2, a + i, (b + ((unsigned long)i << 12)) | PTE_BOX);
    vga_ports(t, !k);
    vga_bgk[t] = (unsigned char)k;
    if (k == 2)
        vga_plmap(t);
    if (k == 4)
        vga_svmap(t, 1);
    tlb(t);
}

/* Seitenfehler im Bildspeicher einer planaren Box hinten: warten, bis sie
   vorn ist, dann laeuft der Befehl auf der Karte noch einmal */
static void vga_wait(int t);
static int vga_pf(int t)
{
    unsigned long a = k2_cr2(), p = a >> 12;
    unsigned k;
    if (!vga_raw[t] || (vga_bgk[t] != 2 && vga_bgk[t] != 4))
        return 0;
    if (a < 0xA0000UL || a >= 0xB0000UL) {
        if (vga_bgk[t] != 4)
            return 0;
        for (k = 0; k < NDPP; k++)              /* LFB hinten, nicht im Bild */
            if (dp_phys[t][k].npg && p >= dp_phys[t][k].lpg &&
                p < dp_phys[t][k].lpg + dp_phys[t][k].npg)
                break;
        if (k == NDPP)
            return 0;
    }
    vga_wait(t);
    return 1;
}

/* Box mit der Karte wartet, bis ihre Sitzung vorn ist und gfx_in alles
   zurueckgeladen hat; danach wieder auf der Karte */
static void vga_wait(int t)
{
    unsigned f = irq_off();
    if (vga_raw[t] && (fg != box_ses[t] || hw_gfx != RAWGFX)) {
        if (spk[t])
            spk_hw(t, 0);
        spk_ports(t, 0);
        vdd_bg(t);
        kpit_set(0);                    /* die virtuelle SB nicht anhalten:
                                           sonst wartet das Spiel nach dem
                                           Zurueckholen ewig auf den Block-
                                           IRQ (DOOM ohne Effekte) */
        enable_ints();
        while (vga_raw[t] && (fg != box_ses[t] || hw_gfx != RAWGFX))
            kf_box_ctl(BOX_SLEEP, 2);
        irq_off();
        vdd_event(t, VDD_EV_RESUME, 0); /* Pause nicht nachholen         */
    }
    if (vga_raw[t] && vga_bgk[t])
        vga_map(t, 0);
    irq_restore(f);
}

/* vor jedem Ruecksprung in die Box: vorn Karte, hinten Schatten oder warten */
static void vga_freeze(int t)
{
    unsigned char __far *q;
    unsigned k;
    if (!vga_raw[t])
        return;
    if (fg == box_ses[t] && hw_gfx == RAWGFX) {
        if (vga_bgk[t])
            vga_map(t, 0);
        return;
    }
    if (vga_bgk[t])
        return;                                 /* laeuft schon hinten   */
    q = vga_vr(t);
    k = q ? q[RAW_KIND] : 0;                    /* 0 auch, solange frisch */
    if (!k) {
        vga_wait(t);
        return;
    }
    if (spk[t])
        spk_hw(t, 0);
    spk_ports(t, 0);
    vdd_bg(t);
    kpit_set(0);
    vga_map(t, k);
}

/* Modus vom BIOS: darf die Box damit hinten auf dem Schatten laufen? */
static void vga_allow(struct v86_frame *f, int t)
{
    unsigned m = AH == 0x00 ? AL & 0x7F : (BX & 0x1FF) < 0x100 ? BX & 0x7F : 0xFF;
    unsigned char __far *q;
    unsigned fl = irq_off();
    vio_raw_mode(box_ses[t], AH == 0x00 ? AL & 0x7F : BX & 0x41FF);
    q = vga_vr(t);
    if (q) {
        q[RAW_ALLOW] = (unsigned char)(m == 0x13 ? 1 : m >= 4 && m <= 6 ? 3 :
                                       m == 0x0D || m == 0x0E || m == 0x10 || m == 0x12 ? 2 : 0);
        if (AX == 0x4F02 && (BX & 0x1FF) >= 0x100)  /* VESA: beim Wegschalten */
            *(unsigned __far *)(q + RAW_VMODE) = BX;    /* 4F04h + Bild  */
        else if (AH == 0x00 && m > 0x13) {      /* Chip-Modus (2Eh ...): */
            if (!*(unsigned __far *)(q + RAW_VMODE))    /* SVGA, Text    */
                *(unsigned __far *)(q + RAW_VMODE) = 0x7F00 | m;   /* per BIOS */
        } else                                  /* (das Tseng-BIOS setzt   */
            *(unsigned __far *)(q + RAW_VMODE) = 0; /* 4F02h intern per 00h) */
    }
    irq_restore(fl);
}

/* INT 10h einer Box, die hinten auf dem Schatten laeuft: Modus abfragen
   und Palette/Attribute (1000h-1017h) gehen an die gesicherte Register-
   kopie, statt die Box bis vorn anhalten -> 1 = erledigt */
static int vga_bgint(struct v86_frame *f, int t)
{
    unsigned char __far *q;
    unsigned i, n, es = (unsigned)f->r.es, dx = DX, fl;
    if (AH == 0x0F) {                           /* Modus aus dem BDA     */
        AL = (unsigned char)rd8(BDA, 0x49);
        AH = (unsigned char)rd8(BDA, 0x4A);
        BH = (unsigned char)rd8(BDA, 0x62);
        return 1;
    }
    if (vga_bgk[t] == 4) {                      /* SVGA: nur das Fenster */
        if (AX != 0x4F05 || BH > 1 || BL > 1)
            return 0;
        fl = irq_off();
        if (!(q = vga_vr(t)) || *(unsigned long __far *)(q + RAW_SVPHYS)) {
            irq_restore(fl);
            return 0;
        }
        if (BL && !q[RAW_SVSPLIT]) {            /* kein Fenster B: wie das */
            irq_restore(fl);                    /* BIOS ablehnen           */
            AX = 0x014F;
            return 1;
        }
        if (BH)
            DX = *(unsigned __far *)(q + RAW_SVBANK + 2 * BL);
        else
            *(unsigned __far *)(q + RAW_SVBANK + 2 * BL) = dx;
        irq_restore(fl);
        if (!BH)
            vga_svmap(t, 1);                    /* Fenster neu einblenden */
        AX = 0x004F;
        return 1;
    }
    if (AH != 0x10 || !(q = vga_vr(t)))
        return 0;
    switch (AL) {
    case 0x00:                                  /* Attribut BL = BH      */
        if (BL < 21)
            q[40 + BL] = BH;
        return 1;
    case 0x01:                                  /* Rand                  */
        q[40 + 0x11] = BH;
        return 1;
    case 0x02:                                  /* 16 Attribute + Rand   */
        for (i = 0; i < 17; i++)
            q[40 + (i < 16 ? i : 0x11)] = (unsigned char)rd8(es, dx + i);
        return 1;
    case 0x07:
        BH = BL < 21 ? q[40 + BL] : 0;
        return 1;
    case 0x08:
        BH = q[40 + 0x11];
        return 1;
    case 0x10:                                  /* eine DAC-Farbe        */
        i = 64 + (BX & 0xFF) * 3;
        q[i] = (unsigned char)(DH & 63);
        q[i + 1] = (unsigned char)(CH & 63);
        q[i + 2] = (unsigned char)(CL & 63);
        return 1;
    case 0x12: case 0x17:                       /* Block setzen/lesen    */
        for (n = 0; n < CX && (BX & 0xFF) + n < 256; n++)
            for (i = 0; i < 3; i++) {
                unsigned o = 64 + ((BX & 0xFF) + n) * 3 + i;
                if (AL == 0x12)
                    q[o] = (unsigned char)(rd8(es, dx + n * 3 + i) & 63);
                else
                    wr8(es, dx + n * 3 + i, q[o]);  /* (VWIN, nicht SCRATCH) */
            }
        return 1;
    case 0x15:                                  /* eine DAC-Farbe lesen  */
        i = 64 + BL * 3;
        DH = q[i];
        CH = q[i + 1];
        CL = q[i + 2];
        return 1;
    }
    return 0;
}

/* VBE 4F00h/4F01h ueber den Real-Mode-Weg des Kernels (vesa_boxq), das
   Ergebnis in den Puffer der Box; Zeiger des VBE-2.0-Infoblocks, die in
   den Kernelpuffer (0700h:xxxx) zeigen, auf den Puffer der Box umrechnen */
static unsigned char vbe_buf[512];
static void vbe_box(struct v86_frame *f)
{
    static const unsigned char po[5] = { 6, 14, 22, 26, 30 };
    unsigned es = (unsigned)f->r.es, di = DI, n = 256, i, o, q = AL;
    unsigned long sig = rd16(es, di) | ((unsigned long)rd16(es, di + 2) << 16);
    if (q == 0 && sig == 0x32454256UL)         /* "VBE2": 512 Byte, sonst */
        n = 512;                                /* nur 256 (MSD 2.11 legt */
    AX = vesa_boxq(AX, CX, sig, (unsigned char __far *)vbe_buf); /* 256 an) */
    if (AX != 0x004F)
        return;
    if (q == 0)
        for (i = 0; i < 5; i++) {
            o = po[i];
            if (*(unsigned *)(vbe_buf + o + 2) == 0x0700 &&
                *(unsigned *)(vbe_buf + o) < n) {
                *(unsigned *)(vbe_buf + o) += di;
                *(unsigned *)(vbe_buf + o + 2) = es;
            }
        }
    for (i = 0; i < n; i++)
        wr8(es, di + i, vbe_buf[i]);
}

/* INT 10h: 1 = an das Video-BIOS geben */
static int vga_want(struct v86_frame *f, int t)
{
    unsigned m = AL & 0x7F;
    int txt;
    txt = (AH == 0x00 && (m <= 3 || m == 7)) ||
              (AX == 0x4F02 && (BX & 0x1FF) < 0x100 && ((BX & 0x7F) <= 3 || (BX & 0x7F) == 7));
    if (vga_raw[t]) {
        if (!txt) {
            if (vga_bgk[t] && vga_ok && vga_bgint(f, t))
                return 2;                       /* hinten nachgebildet   */
            vga_wait(t);                        /* BIOS: nur auf der Karte */
            if (AH == 0x00 || AX == 0x4F02)
                vga_allow(f, t);
            return 1;
        }
        vga_leave(t);                           /* Nachbildung macht 03h */
        if (AH == 0x4F) {
            AX = 0x0003;
            int10(f, t);
            AX = 0x004F;
            return 2;
        }
        return 0;
    }
    if (AH == 0x00 || AX == 0x4F02) {
        if (txt || vga_enter(t))
            return 0;
        vga_wait(t);                            /* hinten gestartet: erst vorn */
        vga_allow(f, t);
        return 1;
    }
    if (AX == 0x4F00 || AX == 0x4F01) {         /* Info: ueber den Kernel */
        vbe_box(f);
        return 2;
    }
    return AH == 0x4F;                          /* VESA-Abfragen: BIOS   */
}

static void vga_bios(struct v86_frame *f)
{
    unsigned long v;
    scratch_map(0, 0x3FF);
    v = get32(SCRATCH, 0x10);
    f->r.cs = (unsigned)(v >> 16);
    f->r.eip = (unsigned)v;
}

/* INT n wie im Real Mode, ip = Ruecksprung */
/* INT 2Fh, die der Monitor selbst beantwortet (DPMI/XMS da?): aus do_int
   oder - hat BOXSHELL INT 2Fh eingehaengt und laesst VME den Vektor ohne
   Falle durch - von BOXSHELL ueber INT E0h 1Bh. 1 = erledigt. */
static int mon2f(struct v86_frame *f)
{
    if (AX == 0x1687) {                         /* DPMI da?              */
        AX = 0;
        BX = 1;                                 /* 32-Bit-Programme gehen */
        CL = 3;
        DX = 0x005A;                            /* 0.90                  */
        *(unsigned short *)&f->esi = 0x0400 >> 4;     /* Absaetze fuer den Host (DP_RMSTK) */
        f->r.es = VBIOS;
        DI = 0x0510;
        return 1;
    }
    if (AX == 0x4309) {                         /* HIMEM-Handle-Tabelle  */
        xms_table(current);                     /* (MS-DOS-MEM zaehlt    */
        AL = 0x43;                              /* daraus XMS insgesamt) */
        f->r.es = VBIOS;
        BX = XMS_TAB;
        return 1;
    }
    if (AX == 0x4300 || AX == 0x4310) {         /* XMS da? (vor dem DOS: */
        if (AL == 0)                            /* FreeDOS reicht 43h    */
            AL = 0x80;                          /* nicht weiter)         */
        else {
            f->r.es = VBIOS;
            BX = XMS_STUB;
        }
        return 1;
    }
    return 0;
}

static void do_int(struct v86_frame *f, int t, unsigned n, unsigned ip)
{
    if ((n == 0x2F || n == 0x67) && wtr_want(n, f))
        wtrace(f, t, n, '>', (unsigned)f->r.cs, ip);
    if (n == 0x2F && mon2f(f)) {
        wtrace(f, t, n, '<', 0, 0);
        f->r.eip = ip;
        return;
    }
    if (n == 0x10 && vec_def(0x10)) {           /* Grafik: Video-BIOS    */
        int w = vga_want(f, t);
        if (w == 1) {
            push16(&f->r, vflags(&f->r, t));
            push16(&f->r, (unsigned)f->r.cs);
            push16(&f->r, ip);
            vfl[t] &= ~FL_IF;
            f->r.eflags &= ~(unsigned long)FL_TF;
            vga_bios(f);
            return;
        }
        if (w == 2) {
            f->r.eip = ip;
            return;
        }
    }
    if (vec_def(n) && n != 8) {                 /* Vektor unveraendert:  */
        f->r.eip = ip;                          /* Dienst gleich         */
        set_flags(&f->r, t, run_svc(n, f, t, vflags(&f->r, t)));
        return;
    }
    int_raw(&f->r, t, n, ip);
}

/* ---- Ports --------------------------------------------------------------
   Gehoeren noch niemandem: PIC, Tastaturcontroller, CRTC-Cursor und die
   Ports, an denen Warteschleifen haengen (3DAh Strahlruecklauf, 61h
   Refresh-Bit, PIT-Zaehler), werden nachgebildet; der Rest liest FFh,
   Schreiben verpufft. */
/* VGA-Ports einer Box, die hinten auf dem Schatten laeuft: Lesen und
   Schreiben gehen an die gesicherte Registerkopie. Was die Abbildung
   aendern wuerde, wartet bis vorn und geht dann an die Karte. */
static int vga_simple(unsigned char __far *q, unsigned i, unsigned v)
{
    switch (i) {
    case 1: case 3: return v == 0;              /* Set/Reset, Drehen     */
    case 5: return !(v & 0x0B) && !((v ^ q[36]) & 0x10);
    case 6: return v == q[37];                  /* Speicherabbildung     */
    case 8: return v == 0xFF;                   /* Bitmaske              */
    }
    return 1;
}

static unsigned char vga_pin(int t, unsigned p)
{
    unsigned char __far *q = vga_vr(t), __far *st, v;
    if (!q)
        return 0xFF;
    st = q + RAW_ST;
    switch (p) {
    case 0x3C0: return st[ST_AIDX];
    case 0x3C1: return (st[ST_AIDX] & 0x1F) < 21 ? q[40 + (st[ST_AIDX] & 0x1F)] : 0;
    case 0x3C4: return st[ST_SEQ];
    case 0x3C5: return st[ST_SEQ] < 5 ? q[1 + st[ST_SEQ]] : 0xFF;
    case 0x3C8: return st[ST_WI];
    case 0x3C9:
        v = q[64 + st[ST_RI] * 3 + st[ST_RC]];
        if (++st[ST_RC] == 3) {
            st[ST_RC] = 0;
            st[ST_RI]++;
        }
        return v;
    case 0x3CC: return q[0];
    case 0x3CE: return st[ST_GC];
    case 0x3CF: return st[ST_GC] < 9 ? q[31 + st[ST_GC]] : 0xFF;
    case 0x3D4: return crt_idx[t];
    case 0x3D5: return crt_idx[t] < 25 ? q[6 + crt_idx[t]] : 0xFF;
    case 0x3DA:
        st[ST_AFF] = 0;
        idle_poll(t, 1);
        tgl[t] ^= 0x09;
        return (unsigned char)(tgl[t] & 0x09);
    }
    return 0xFF;
}

static void vga_pout(int t, unsigned p, unsigned char v)
{
    unsigned char __far *q = vga_vr(t), __far *st;
    unsigned i;
    if (!q)
        return;
    st = q + RAW_ST;
    switch (p) {
    case 0x3C0:
        if (!st[ST_AFF])
            st[ST_AIDX] = v;
        else if ((st[ST_AIDX] & 0x1F) < 21)
            q[40 + (st[ST_AIDX] & 0x1F)] = v;
        st[ST_AFF] ^= 1;
        return;
    case 0x3C2: q[0] = v; return;
    case 0x3C4: st[ST_SEQ] = v; return;
    case 0x3C5:
        i = st[ST_SEQ];
        if (i == 4 && ((v ^ q[5]) & 0x0C)) {    /* Chain-4/Odd-Even      */
            vga_wait(t);
            outb(0x3C4, i);
            outb(0x3C5, v);
        } else if (i < 5) {
            q[1 + i] = v;
            if (i == 2 && vga_bgk[t] == 2)
                vga_plmap(t);                   /* andere Ebene          */
        }
        return;
    case 0x3C7: st[ST_RI] = v; st[ST_RC] = 0; return;
    case 0x3C8: st[ST_WI] = v; st[ST_WC] = 0; return;
    case 0x3C9:
        q[64 + st[ST_WI] * 3 + st[ST_WC]] = (unsigned char)(v & 0x3F);
        if (++st[ST_WC] == 3) {
            st[ST_WC] = 0;
            st[ST_WI]++;
        }
        return;
    case 0x3CE: st[ST_GC] = v; return;
    case 0x3CF:
        i = st[ST_GC];
        if (vga_bgk[t] == 2 && i < 9 && i != 6) {   /* planar: neu einblenden */
            q[31 + i] = v;
            vga_plmap(t);
            return;
        }
        if (i < 9 && !vga_simple(q, i, v)) {
            vga_wait(t);
            outb(0x3CE, i);
            outb(0x3CF, v);
        } else if (i < 9)
            q[31 + i] = v;
        return;
    case 0x3D4: crt_idx[t] = v; return;
    case 0x3D5:
        if (crt_idx[t] < 25)
            q[6 + crt_idx[t]] = v;
        return;
    }
}

/* VBE-Register der Bochs-/QEMU-Karte (1CEh/1CFh, Wort): vorn frei in der
   I/O-Bitmap; faellt ein Zugriff, war die Box hinten -> warten, dann
   als Wort an die Karte (bytweise verstaende die Karte ihn nicht) */
static int vbe_io(int t, unsigned p)
{
    if (!vga_raw[t] || p < 0x1CE || p > 0x1D0)
        return 0;
    vga_wait(t);
    return 1;
}

static unsigned char port_in(int t, unsigned p)
{
    unsigned o;
    int k;
    if (p >= 0x3B0 && p <= 0x3DF && vga_raw[t]) {
        if (vga_bgk[t] && (vga_bgk[t] != 4 || p == 0x3DA)) {
            o = vga_pin(t, p);                  /* SVGA hinten: nur 3DAh */
            if (!vga_ok)                        /* Grafikmodul: Flipflop */
                vdd_io(t, VDD_IN, p, 0, 2);     /* des ATC mitfuehren    */
            return (unsigned char)o;
        }
        vga_wait(t);                            /* (Index 3D4 o. ae. vorn: */
        if (!vga_ok && (o = vdd_io(t, VDD_IN, p, 0, 3)) != VDD_NOTMINE)
            return (unsigned char)o;            /* EGA/CGA: ueber das Modul */
        return (unsigned char)inb(p);           /* Bitmap war noch zu)   */
    }
    switch (p) {
    case 0x20:                                  /* ISR nach OCW3 0Bh (DOS/4GW */
        return pic_isr[t][0] ? visr[t] : 0;     /* trennt so IRQ und Ausnahme */
    case 0x21:                                  /* bei Vektor 08h-0Fh), sonst */
        return vimr[t];                         /* IRR - Wartendes ist hier  */
    case 0xA0:                                  /* nicht nachgebildet         */
        return 0;
    case 0xA1:
        return vimr2[t];
    case 0x60:
        if (vec_def(9) && !pm_hooked(t, 9) && (k = kq_get(t)) >= 0)
            vcur[t] = (unsigned char)k;         /* ohne INT 9: abfragen  */
        return vcur[t];
    case 0x64:
        return (unsigned char)(0x14 | (kq_h[t] != kq_t[t]));
    case 0x61:
        tgl[t] ^= 0x10;                         /* Refresh-Bit kippt     */
        return (unsigned char)(0x20 | (tgl[t] & 0x10) | spk[t]);
    case 0x40: case 0x41: case 0x42:
        return pit_in(t, p);
    case 0x71: {                                /* CMOS: Speicher der Box, */
        unsigned kb, f, n = emb_free_pg(t), lim = xms_lim[t] ? xms_lim[t] - 1 : 512;
        unsigned long fr = kf_mem_avail_kb() >> 2;
        unsigned char v;
        if (n > fr)                             /* nur, was die Box bekommen */
            lim -= n - (unsigned)fr;            /* kann (XMS_MEMORY_LIMIT=-1) */
        if (!xms_snap[t])                       /* erstes Lesen nach dem   */
            xms_snap[t] = lim + 1;              /* Programmstart: BOXSHELL */
        else                                    /* -> LoL+45h; danach fest, */
            lim = xms_snap[t] - 1;              /* sonst sieht MEM (liest  */
                                                /* CMOS selbst) frei > gesamt */
        kb = 64 + (lim << 2);
        if (cmos_idx[t] == 0x17 || cmos_idx[t] == 0x30)
            return (unsigned char)kb;
        if (cmos_idx[t] == 0x18 || cmos_idx[t] == 0x31)
            return (unsigned char)(kb >> 8);
        if (cmos_om[t][cmos_idx[t] >> 3] & (1 << (cmos_idx[t] & 7)))
            return cmos_ov[t][cmos_idx[t]];
        f = irq_off();
        outb(0x70, cmos_idx[t]);
        v = (unsigned char)inb(0x71);
        irq_restore(f);
        return v;
    }
    case 0x3DA: case 0x3BA:
        idle_poll(t, 1);
        tgl[t] ^= 0x09;                         /* Ruecklauf kommt und geht */
        return (unsigned char)(tgl[t] & 0x09);
    case 0x3D5:
        if (crt_idx[t] == 0x0E || crt_idx[t] == 0x0F) {
            unsigned v = rd16(BDA, 0x50);
            unsigned o = (v >> 8) * COLS + (v & 0xFF);
            return (unsigned char)(crt_idx[t] == 0x0E ? o >> 8 : o);
        }
        return 0;
    case 0x3CC:
        return 0x67;
    case 0x201:                                 /* Gameport: vorn direkt, */
        if (!box_fg(t))                         /* hinten "keiner"        */
            return 0xFF;
        joy_ports(t, 1);
        return (unsigned char)inb(0x201);
    }
    if ((o = vdd_io(t, VDD_IN, p, 0, box_fg(t))) != VDD_NOTMINE)  /* Module */
        return (unsigned char)o;
    return 0xFF;
}

static void port_out(int t, unsigned p, unsigned char v)
{
    unsigned o, cur;
    if (p >= 0x3B0 && p <= 0x3DF && vga_raw[t]) {
        if (vga_bgk[t] && vga_bgk[t] != 4) {
            vga_pout(t, p, v);
            if (!vga_ok)                        /* Grafikmodul schreibt mit */
                vdd_io(t, VDD_OUT, p, v, 2);
        } else {
            vga_wait(t);
            if (vga_ok || vdd_io(t, VDD_OUT, p, v, 3) == VDD_NOTMINE)
                outb(p, v);
        }
        return;
    }
    switch (p) {
    case 0x201:                                 /* Gameport anstossen     */
        if (box_fg(t)) {
            joy_ports(t, 1);
            outb(0x201, v);
        }
        return;
    case 0xA0:                                  /* zweiter PIC: ICW1     */
        if (v & 0x10) {                         /* (Masken behalten ihren */
            picw[t][1] = (unsigned char)(v & 1 ? 3 : 2);    /* Wert)      */
            pic_isr[t][1] = 0;
        } else if ((v & 0x1A) == 0x0A)          /* OCW3 mit RR: IRR/ISR  */
            pic_isr[t][1] = (unsigned char)(v & 1);
        break;
    case 0xA1:
        if (picw[t][1])
            picw[t][1]--;
        else
            vimr2[t] = v;
        break;
    case 0x20:
        if (v & 0x10) {                         /* ICW1: Folge ICW2-4    */
            picw[t][0] = (unsigned char)(v & 1 ? 3 : 2);
            pic_isr[t][0] = 0;
            break;
        }
        if ((v & 0x1A) == 0x0A) {               /* OCW3 mit RR: IRR/ISR  */
            pic_isr[t][0] = (unsigned char)(v & 1);
            break;
        }
        if (v == 0x20)                          /* EOI: niedrigstes Bit  */
            visr[t] &= (unsigned char)(visr[t] - 1);
        else if ((v & 0xF8) == 0x60)            /* spezifisches EOI (Digger) */
            visr[t] &= (unsigned char)~(1 << (v & 7));
        break;
    case 0x21:
        if (picw[t][0])                         /* ICW2-4, nicht Maske   */
            picw[t][0]--;
        else
            vimr[t] = v;
        break;
    case 0x40: case 0x41: case 0x42: case 0x43:
        pit_out(t, p, v);
        break;
    case 0x70:
        cmos_idx[t] = (unsigned char)(v & 0x7F);
        break;
    case 0x71:                                  /* CMOS schreiben: nur in */
        o = cmos_idx[t];                        /* die Kopie der Box: die */
        if (o >= 0x0E || o == 1 || o == 3 || o == 5 || o == 0x0B) {
            cmos_ov[t][o] = v;                  /* Uhr laeuft weiter, Weck- */
                                                /* register und Status B  */
                                                /* liest die Box zurueck  */
            cmos_om[t][cmos_idx[t] >> 3] |= (unsigned char)(1 << (cmos_idx[t] & 7));
        }
        break;
    default:
        vdd_io(t, VDD_OUT, p, v, box_fg(t));    /* Module (VCOM, VSB)    */
        break;
    case 0x61:
        spk[t] = (unsigned char)(v & 3);
        if (box_fg(t)) {
            spk_hw(t, 1);
            spk_ports(t, 1);                    /* weitere direkt        */
        }
        break;
    case 0x3D4:
        crt_idx[t] = v;
        break;
    case 0x3D5:                                 /* Cursor ueber den CRTC */
        if (crt_idx[t] == 0x0E || crt_idx[t] == 0x0F) {
            cur = rd16(BDA, 0x50);
            o = (cur >> 8) * COLS + (cur & 0xFF);
            o = crt_idx[t] == 0x0E ? (o & 0xFF) | (v << 8) : (o & 0xFF00) | v;
            crow = (unsigned char)(o / COLS);
            ccol = (unsigned char)(o % COLS);
            if (crow < ROWS)
                cur_put(t);
        }
        break;
    }
}

/* ---- Monitor -------------------------------------------------------------
   Aus start.asm exc_common, wenn eine Ausnahme aus der Box kam. -> 0:
   erledigt, weiter in der Box; sonst beendet fault() die Box. */
/* ---- XMS 3.0 fuer die Box (wie VXMS.SYS bei OS/2) -------------------------
   Kein HIMEM in der Box: INT 2Fh 4300h/4310h beantwortet der Monitor
   selbst (FreeDOS reicht 43h nicht weiter), Einsprung F000:0500 =
   "ARPL E1h; RETF" -> xms(). A20 je Box ueber die Seitentabelle: aus =
   100000-10FFFF spiegelt 0-FFFF, an = eigene 16 Seiten (hma_pg). HMA nur
   mit DOS=HIGH (CONFIG.SYS der AT-DOS), UMBs nur mit DOS=UMB: freie
   Lueckenseiten zwischen C8000 und EFFFF (kein ROM, Inhalt einfoermig)
   bekommen eigene Seiten. Speicherbloecke (EMB) liegen im Adressraum der
   Box ab 110000h bis 64 MB (Seitentabellen ab 4 MB wie DPMI ueber
   lpte_set, DPMI-Speicher deshalb erst ab 64 MB), Lock liefert diese
   lineare Adresse (fuer DPMI spaeter), Move kopiert ueber zwei Fenster.
   Eigene Seiten tragen PTE_OWN und werden in v86_destroy frei. */
#define rd8_k(i) (*(unsigned char __far *)MK_FP(SCRATCH, (i)))
#define XMS_PG0  0x110                  /* erste EMB-Seite               */
#define XMS_KB   2048                   /* Vorgabe je Box (DOSBOX.INI: XMS_MEMORY_LIMIT) */
#define XMS_TOP  0x4000                 /* bis 64 MB (DPMI dahinter)     */
#define XMS_MAXPG (XMS_TOP - XMS_PG0)    /* 63.424 KB                     */
static int lpte_set(int t, unsigned long lp, unsigned long e);
static unsigned long lpte_get(int t, unsigned long lp);
#define XMS_PGN  (xms_lim[t] ? xms_lim[t] - 1 : XMS_KB / 4)
#define NXH      16
struct xmsh { unsigned base, npg; unsigned char used, lock; };
static struct xmsh xh[NTASK][NXH];
static pg_t hma_pg[NTASK][16];
static unsigned char a20_glob[NTASK], a20_loc[NTASK], a20_on[NTASK], hma_used[NTASK];
static unsigned char umb_ok, umb_free[6];       /* Seiten C8..EF frei    */
static unsigned char umb_used[NTASK][6], umb_head[NTASK][6];

#define BIT(m, p)   ((m)[((p) - 0xC8) >> 3] & (1 << (((p) - 0xC8) & 7)))
#define BSET(m, p)  ((m)[((p) - 0xC8) >> 3] |= (unsigned char)(1 << (((p) - 0xC8) & 7)))
#define BCLR(m, p)  ((m)[((p) - 0xC8) >> 3] &= (unsigned char)~(1 << (((p) - 0xC8) & 7)))

/* EMS-Seitenrahmen (die Verwaltung steht in VEMS.SYS): die erste freie
   64-KB-Luecke ab C8000, auf 16 KB ausgerichtet, einmal fuer alle Boxen
   vor den UMBs; EMS_FRAME_LOCATION=NONE -> keiner, 64 KB mehr UMB */
extern unsigned char ems_none;
static char *hx2(char *o, unsigned v)  /* zwei Hexziffern                */
{
    o[0] = "0123456789ABCDEF"[(v >> 4) & 15];
    o[1] = "0123456789ABCDEF"[v & 15];
    return o + 2;
}

static void ems_pick_frame(void)        /* einmal, nach umb_scan          */
{
    char m[120], *o;
    unsigned p, i;
    if (ems_none)
        return;
    for (p = 0xC8; p + 16 <= 0xF0; p += 4) {
        for (i = 0; i < 16 && BIT(umb_free, p + i); i++)
            ;
        if (i == 16) {
            ems_frame = (unsigned char)p;
            break;
        }
    }
    /* DMESG: Lage des Rahmens bzw. warum keiner passt (freie 4-KB-Seiten
       C8-EF des oberen Speichers, als Bereiche) */
    o = m;
    {
        static const char t1[] = "EMS-Rahmen ";
        static const char t2[] = "EMS: kein 64-KB-Rahmen frei, oberer Speicher frei:";
        const char *t = ems_frame ? t1 : t2;
        while (*t)
            *o++ = *t++;
    }
    if (ems_frame) {
        o = hx2(o, ems_frame);
        *o++ = '0';
        *o++ = '0';
        *o++ = 'h';
    } else
        for (p = 0xC8; p < 0xF0 && o < m + sizeof(m) - 10; p++) {
            if (!BIT(umb_free, p) || (p > 0xC8 && BIT(umb_free, p - 1)))
                continue;
            for (i = p; i + 1 < 0xF0 && BIT(umb_free, i + 1); i++)
                ;
            *o++ = ' ';
            o = hx2(o, p);
            *o++ = '0';
            *o++ = '0';
            *o++ = '-';
            o = hx2(o, i);
            *o++ = 'F';
            *o++ = 'F';
        }
    *o++ = '\n';
    klog(KDATA, (unsigned)m, (unsigned)(o - m));
}

static unsigned long pte_get(int t, unsigned i)
{
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    return get32(SCRATCH2, i);
}

static void pte_set(int t, unsigned i, unsigned long e)
{
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    put32(SCRATCH2, i, e);
}

static void tlb(int t)                  /* Seitentabelle der laufenden Box geaendert */
{
    if (k2_get_cr3() == PG_LIN(pd_box[t]))
        k2_cr3((unsigned)k2_get_cr3(), (unsigned)(k2_get_cr3() >> 16));
}

static pg_t zero_page(pg_t p)
{
    if (p) {
        scratch_map(PG_LIN(p), 4095);
        zero_seg(SCRATCH, 2048);
    }
    return p;
}

static pg_t own_page(void)              /* genullte eigene Seite (Tabellen,
                                           Verwaltung: unter 2 GB)        */
{
    return zero_page(vp_alloc());
}

static pg_t own_page_hi(void)           /* genullte Seite fuer Box-Daten  */
{
    return zero_page(vp_alloc_hi());
}

static int a20_set(int t, int on)
{
    unsigned i;
    if (on && !hma_pg[t][0])
        for (i = 0; i < 16; i++)
            if (!(hma_pg[t][i] = own_page_hi())) {
                while (i--) {
                    vp_free(hma_pg[t][i]);
                    hma_pg[t][i] = 0;
                }
                return 0;
            }
    for (i = 0; i < 16; i++)
        pte_set(t, 0x100 + i, on ? PG_LIN(hma_pg[t][i]) | PTE_BOX
                                 : (pte_get(t, i) & ~0xFFFUL) | PTE_BOX);
    a20_on[t] = (unsigned char)on;
    tlb(t);
    return 1;
}

static void xms_end(int t)              /* aus v86_destroy               */
{
    unsigned i;
    for (i = 0; i < 16; i++)
        if (hma_pg[t][i]) {
            vp_free(hma_pg[t][i]);
            hma_pg[t][i] = 0;
        }
    for (i = 0; i < NXH; i++)
        xh[t][i].used = 0;
    a20_glob[t] = a20_loc[t] = a20_on[t] = hma_used[t] = 0;
    xms_lim[t] = 0;
    xms_snap[t] = 0;
    box_hk[t] = 0;
    idle_sens[t] = 0;
    for (i = 0; i < 6; i++)
        umb_used[t][i] = umb_head[t][i] = 0;
}

/* einmal: welche Seiten C8..EF sind frei? ROMs (55AA, Groesse * 512 ab
   C0000 in 2-KB-Schritten) und uneinheitliche Seiten scheiden aus */
static void umb_scan(void)
{
    static unsigned char rom[6];
    unsigned long a;
    unsigned p, k, n, i;
    unsigned char c;

    for (a = 0xC0000UL; a < 0xF0000UL; a += 2048) {
        scratch_map(a, 2047);
        if (rd8_k(0) == 0x55 && rd8_k(1) == 0xAA) {
            n = rd8_k(2);                       /* 512-Byte-Bloecke    */
            for (k = 0; k < (n + 7) / 8 + 1; k++) {
                p = (unsigned)((a + (unsigned long)k * 4096) >> 12);
                if (p >= 0xC8 && p < 0xF0)
                    BSET(rom, p);
            }
        }
    }
    /* SeaBIOS (QEMU, Proxmox) haelt Daten im E-Segment (zonelow, auch
       das SeaVGABIOS beim Moduswechsel) - dort keine UMBs, sonst
       ueberschreiben sich DOS und BIOS gegenseitig */
    scratch_map(0xF0000UL, 0xFFFF);
    for (i = 0; i < 0xFFF8; i++)
        if (rd8_k(i) == 'S' && rd8_k(i + 1) == 'e' && rd8_k(i + 2) == 'a' &&
            rd8_k(i + 3) == 'B' && rd8_k(i + 4) == 'I' && rd8_k(i + 5) == 'O') {
            for (p = 0xE0; p < 0xF0; p++)
                BSET(rom, p);
            break;
        }
    for (p = 0xC8; p < 0xF0; p++) {
        if (BIT(rom, p))
            continue;
        scratch_map(PG_LIN(p), 4095);
        c = rd8_k(0);
        if (c != 0 && c != 0xFF)
            continue;
        for (i = 1; i < 4096 && rd8_k(i) == c; i++)
            ;
        if (i == 4096)
            BSET(umb_free, p);
    }
    umb_ok = 1;
    ems_pick_frame();
}

static unsigned umb_run(int t, unsigned p)      /* freie Seiten ab p     */
{
    unsigned n = 0;
    while (p + n < 0xF0 && BIT(umb_free, p + n) && !BIT(umb_used[t], p + n) &&
           (!EFR(t) || p + n < EFR(t) || p + n >= EFR(t) + 16u))
        n++;
    return n;
}

/* EMB: Luecke fuer n Seiten im Adressraum der Box (erste passende) */
static unsigned emb_gap(int t, unsigned n, unsigned *largest)
{
    unsigned s, e, i, best = 0, found = 0, nx;
    s = XMS_PG0;
    for (;;) {
        for (i = 0; i < NXH; i++)               /* liegt s in einem Block? */
            if (xh[t][i].used && xh[t][i].npg && s >= xh[t][i].base &&
                s < xh[t][i].base + xh[t][i].npg)
                break;
        if (i < NXH) {
            s = xh[t][i].base + xh[t][i].npg;
            continue;
        }
        if (s >= XMS_PG0 + XMS_PGN)
            break;
        nx = XMS_PG0 + XMS_PGN;                 /* naechster Block dahinter */
        for (i = 0; i < NXH; i++)
            if (xh[t][i].used && xh[t][i].npg && xh[t][i].base > s && xh[t][i].base < nx)
                nx = xh[t][i].base;
        e = nx - s;
        if (e > best)
            best = e;
        if (!found && e >= n)
            found = s;
        s = nx;
    }
    if (largest)
        *largest = best;
    return found;
}

static void emb_unmap(int t, unsigned base, unsigned n);
static int emb_map(int t, unsigned base, unsigned n)
{
    unsigned i;
    pg_t p;
    for (i = 0; i < n; i++)
        if (!(p = own_page_hi()) || !lpte_set(t, base + i, PG_LIN(p) | PTE_BOX | PTE_OWN)) {
            if (p)
                vp_free(p);
            emb_unmap(t, base, i);
            return 0;
        }
    return 1;
}

static void emb_unmap(int t, unsigned base, unsigned n)
{
    unsigned long e;
    while (n--) {
        e = lpte_get(t, base + n);
        pte_drop(e);
        lpte_set(t, base + n, 0);
    }
}

static unsigned emb_free_pg(int t)
{
    unsigned i, u = 0;
    for (i = 0; i < NXH; i++)
        if (xh[t][i].used)
            u += xh[t][i].npg;
    return u >= XMS_PGN ? 0 : XMS_PGN - u;
}

/* Handle-Tabelle wie HIMEM fuer INT 2Fh 4309h (MS-DOS-MEM liest daraus
   XMS insgesamt/frei): Kopf (Kennung, Eintragsgroesse 10, Anzahl, Zeiger),
   je Eintrag Art (1 frei, 2 belegt, 4 leer), Sperren, Adresse und Groesse
   in KB. Der freie Rest der Box steht als ein Block hinter den belegten. */
static void xms_ent(unsigned *e, unsigned fl, unsigned lk, unsigned long ad, unsigned long kb)
{
    wr8(VBIOS, *e, (unsigned char)fl);
    wr8(VBIOS, *e + 1, (unsigned char)lk);
    wr16(VBIOS, *e + 2, (unsigned)ad);
    wr16(VBIOS, *e + 4, (unsigned)(ad >> 16));
    wr16(VBIOS, *e + 6, (unsigned)kb);
    wr16(VBIOS, *e + 8, (unsigned)(kb >> 16));
    *e += 10;
}

static void xms_table(int t)
{
    unsigned i, e = XMS_TAB + 16, s = XMS_PG0, nx, n = 0, u = 0;
    unsigned long bud = kf_mem_avail_kb() >> 2, g;  /* Seiten, die das System hat */
    for (i = 0; i < NXH; i++)
        if (xh[t][i].used)
            u += xh[t][i].npg;
    if (xms_snap[t])                            /* fest wie die Gesamt- */
        bud = xms_snap[t] - 1 > u ? xms_snap[t] - 1 - u : 0;  /* groesse,
                                           die MEM aus LoL+45h (CMOS beim
                                           Programmstart) nimmt - sonst frei
                                           > gesamt, wenn der Cache seither
                                           Speicher abgab */
    wr8(VBIOS, XMS_TAB, 1);
    wr8(VBIOS, XMS_TAB + 1, 10);
    wr16(VBIOS, XMS_TAB + 4, XMS_TAB + 16);
    wr16(VBIOS, XMS_TAB + 6, VBIOS);
    for (i = 0; i < NXH; i++)                   /* belegte Bloecke       */
        if (xh[t][i].used) {
            xms_ent(&e, 2, xh[t][i].lock, (unsigned long)xh[t][i].base << 2,
                    (unsigned long)xh[t][i].npg << 2);
            n++;
        }
    /* oberer Speicher als belegter, gesperrter Block wie EMM386 unter
       HIMEM: MEM von MS-DOS rechnet XMS gesamt als 1 MB + Summe der Tabelle
       - konventionell - UMB - reserviert, nimmt also an, die UMBs kaemen
       aus dem erweiterten Speicher. Ohne den Block war XMS gesamt um die
       UMBs kleiner als frei („Verwendet“ negativ) */
    if (umb_ok) {
        for (g = 0, i = 0xC8; i < 0xF0; i++)
            if (BIT(umb_free, i) && (!EFR(t) || i < EFR(t) || i >= EFR(t) + 16u))
                g += 4;
        if (g) {
            xms_ent(&e, 2, 1, (unsigned long)(XMS_PG0 + XMS_PGN) << 2, g);
            n++;
        }
    }
    for (;;) {                                  /* freie Luecken einzeln, */
        for (i = 0; i < NXH; i++)               /* damit die Summe die    */
            if (xh[t][i].used && xh[t][i].npg && s >= xh[t][i].base &&  /* Grenze trifft */
                s < xh[t][i].base + xh[t][i].npg)
                break;
        if (i < NXH) {
            s = xh[t][i].base + xh[t][i].npg;
            continue;
        }
        if (s >= XMS_PG0 + XMS_PGN)
            break;
        nx = XMS_PG0 + XMS_PGN;
        for (i = 0; i < NXH; i++)
            if (xh[t][i].used && xh[t][i].npg && xh[t][i].base > s && xh[t][i].base < nx)
                nx = xh[t][i].base;
        g = nx - s;
        if (g > bud)                            /* mehr hat das System nicht */
            g = bud;
        bud -= g;
        if (g) {
            xms_ent(&e, 1, 0, (unsigned long)s << 2, g << 2);
            n++;
        }
        s = nx;
    }
    wr16(VBIOS, XMS_TAB + 2, n);
}

static void lwin(unsigned idx, unsigned long lin)  /* Fenster auf lineare Adresse der Box */
{
    struct desc *d = &gdt[idx];
    d->limit = 0xFFFF;
    d->base_lo = (unsigned short)lin;
    d->base_hi = (unsigned char)(lin >> 16);
    d->access = ACC_KDATA;
    d->rsv = (unsigned short)((lin >> 24) << 8);
}

/* Adresse fuer Move: Handle 0 = seg:off der Box, sonst Block + Versatz */
static int xaddr(int t, unsigned h, unsigned long off, unsigned long len,
                 unsigned long *lin)
{
    if (h == 0) {
        *lin = ((off >> 16) << 4) + (unsigned)off;
        return *lin + len <= 0x110000UL;
    }
    if (h > NXH || !xh[t][h - 1].used ||
        off + len > ((unsigned long)xh[t][h - 1].npg << 12))
        return 0;
    *lin = ((unsigned long)xh[t][h - 1].base << 12) + off;
    return 1;
}

#define XFAIL(c)  do { AX = 0; BL = (c); return; } while (0)

static void xms(struct v86_frame *f, int t)
{
    unsigned h = DX, i, n, lg;
    unsigned long kb, len, so, dof, sl, dl;
    unsigned ds = (unsigned)f->r.ds, si = (unsigned)f->esi;

    switch (AH) {
    case 0x00:                                  /* Version               */
        AX = 0x0300;
        BX = 0x0301;
        DX = dos_high;
        return;
    case 0x01:                                  /* HMA anfordern         */
        if (!dos_high)
            XFAIL(0x90);
        if (hma_used[t])
            XFAIL(0x91);
        hma_used[t] = 1;
        AX = 1;
        return;
    case 0x02:
        if (!hma_used[t])
            XFAIL(0x93);
        hma_used[t] = 0;
        AX = 1;
        return;
    case 0x03: case 0x05:                       /* A20 ein (global/lokal) */
        if (AH == 3)
            a20_glob[t] = 1;
        else
            a20_loc[t]++;
        if (!a20_on[t] && !a20_set(t, 1))
            XFAIL(0x82);
        AX = 1;
        return;
    case 0x04: case 0x06:                       /* A20 aus               */
        if (AH == 4)
            a20_glob[t] = 0;
        else if (a20_loc[t])
            a20_loc[t]--;
        if (a20_on[t] && !a20_glob[t] && !a20_loc[t])
            a20_set(t, 0);
        AX = 1;
        return;
    case 0x07:
        AX = a20_on[t];
        BL = 0;
        return;
    case 0x08: case 0x88:                       /* freier Speicher       */
        emb_gap(t, 0xFFFF, &lg);
        n = emb_free_pg(t);
        kb = kf_mem_avail_kb() >> 2;                /* auch das System zaehlt */
        if (xms_snap[t]) {                          /* wie xms_table: fest  */
            i = XMS_PGN - n;                        /* aus der Gesamtgroesse */
            kb = xms_snap[t] - 1 > i ? xms_snap[t] - 1 - i : 0;  /* (LoL+45h) */
        }                                           /* - MEM rechnet beides */
        if (lg > kb) lg = (unsigned)kb;
        if (n > kb) n = (unsigned)kb;
        if (AH == 0x88) {
            f->eax = (unsigned long)lg << 2;
            f->edx = (unsigned long)n << 2;
            f->ecx = ((unsigned long)(XMS_PG0 + XMS_PGN) << 12) - 1;
        } else {
            AX = lg << 2;
            DX = n << 2;
        }
        BL = lg ? 0 : 0xA0;
        return;
    case 0x09: case 0x89:                       /* Block anlegen         */
        kb = AH == 0x89 ? f->edx : DX;
        if (kb > (unsigned long)XMS_PGN << 2)
            XFAIL(0xA0);
        n = (unsigned)((kb + 3) >> 2);
        for (i = 0; i < NXH && xh[t][i].used; i++)
            ;
        if (i == NXH)
            XFAIL(0xA1);
        h = n ? emb_gap(t, n, 0) : XMS_PG0;
        if (!h || (n && !emb_map(t, h, n)))
            XFAIL(0xA0);
        xh[t][i].base = h;
        xh[t][i].npg = n;
        xh[t][i].used = 1;
        xh[t][i].lock = 0;
        tlb(t);
        DX = i + 1;
        AX = 1;
        return;
    case 0x0A:                                  /* freigeben             */
        if (h == 0 || h > NXH || !xh[t][h - 1].used)
            XFAIL(0xA2);
        if (xh[t][h - 1].lock)
            XFAIL(0xAB);
        emb_unmap(t, xh[t][h - 1].base, xh[t][h - 1].npg);
        xh[t][h - 1].used = 0;
        tlb(t);
        AX = 1;
        return;
    case 0x0B:                                  /* kopieren (DS:SI)      */
        len = ((unsigned long)rd16(ds, si + 2) << 16) | rd16(ds, si);
        so  = ((unsigned long)rd16(ds, si + 8) << 16) | rd16(ds, si + 6);
        dof = ((unsigned long)rd16(ds, si + 14) << 16) | rd16(ds, si + 12);
        if (!xaddr(t, rd16(ds, si + 4), so, len, &sl))
            XFAIL(0xA4);
        if (!xaddr(t, rd16(ds, si + 10), dof, len, &dl))
            XFAIL(0xA6);
        while (len) {
            n = len > 0x8000UL ? 0x8000 : (unsigned)len;
            lwin(VWIN_IDX, sl);
            lwin(RWIN_IDX(t), dl);
            far_copy(RWIN_SEL(t), 0, VWIN, 0, n);
            sl += n;
            dl += n;
            len -= n;
        }
        AX = 1;
        BL = 0;
        return;
    case 0x0C:                                  /* sperren -> DX:BX      */
        if (h == 0 || h > NXH || !xh[t][h - 1].used)
            XFAIL(0xA2);
        if (xh[t][h - 1].lock == 255)
            XFAIL(0xAC);
        xh[t][h - 1].lock++;
        kb = (unsigned long)xh[t][h - 1].base << 12;
        DX = (unsigned)(kb >> 16);
        BX = (unsigned)kb;
        AX = 1;
        return;
    case 0x0D:
        if (h == 0 || h > NXH || !xh[t][h - 1].used)
            XFAIL(0xA2);
        if (!xh[t][h - 1].lock)
            XFAIL(0xAA);
        xh[t][h - 1].lock--;
        AX = 1;
        return;
    case 0x0E: case 0x8E:                       /* Angaben zum Block     */
        if (h == 0 || h > NXH || !xh[t][h - 1].used)
            XFAIL(0xA2);
        for (i = 0, n = 0; i < NXH; i++)
            if (!xh[t][i].used)
                n++;
        BH = xh[t][h - 1].lock;
        if (AH == 0x8E) {
            CX = n;
            f->edx = (unsigned long)xh[t][h - 1].npg << 2;
        } else {
            BL = (unsigned char)n;
            DX = xh[t][h - 1].npg << 2;
        }
        AX = 1;
        return;
    case 0x0F: case 0x8F: {                     /* Groesse aendern       */
        struct xmsh *x;
        kb = AH == 0x8F ? f->ebx : BX;
        if (h == 0 || h > NXH || !xh[t][h - 1].used)
            XFAIL(0xA2);
        x = &xh[t][h - 1];
        if (x->lock)
            XFAIL(0xAB);
        n = (unsigned)((kb + 3) >> 2);
        if (kb > (unsigned long)XMS_PGN << 2)
            XFAIL(0xA0);
        if (n < x->npg) {
            emb_unmap(t, x->base + n, x->npg - n);
            x->npg = n;
        } else if (n > x->npg) {                /* nur an Ort und Stelle */
            for (i = 0; i < NXH; i++)
                if (i != h - 1 && xh[t][i].used && xh[t][i].npg &&
                    xh[t][i].base < x->base + n && xh[t][i].base >= x->base)
                    XFAIL(0xA0);
            if (x->base + n > XMS_PG0 + XMS_PGN || !emb_map(t, x->base + x->npg, n - x->npg))
                XFAIL(0xA0);
            x->npg = n;
        }
        tlb(t);
        AX = 1;
        return;
    }
    case 0x10: {                                /* UMB anfordern (DX Par.) */
        unsigned p, best = 0, at = 0, need = (DX + 255) >> 8;
        if (!dos_umb)
            XFAIL(0xB1);
        if (!umb_ok)
            umb_scan();
        for (p = 0xC8; p < 0xF0; p++) {
            n = umb_run(t, p);
            if (n > best)
                best = n;
            if (!at && n >= need && need)
                at = p;
            if (n)
                p += n - 1;
        }
        if (!best) {
            DX = 0;
            XFAIL(0xB1);
        }
        if (!at) {
            DX = best << 8;
            XFAIL(0xB0);
        }
        for (p = 0; p < need; p++) {
            pg_t q = own_page_hi();
            if (!q) {
                while (p--) {
                    vp_free(pte_get(t, at + p) >> 12);
                    pte_set(t, at + p, PG_LIN(at + p) | PTE_ROM);
                    BCLR(umb_used[t], at + p);
                }
                DX = 0;
                XFAIL(0xB1);
            }
            pte_set(t, at + p, PG_LIN(q) | PTE_BOX | PTE_OWN);
            BSET(umb_used[t], at + p);
        }
        BSET(umb_head[t], at);
        tlb(t);
        BX = at << 8;
        DX = need << 8;
        AX = 1;
        return;
    }
    case 0x11: {                                /* UMB freigeben (DX Seg.) */
        unsigned p = DX >> 8;
        if ((DX & 0xFF) || p < 0xC8 || p >= 0xF0 || !BIT(umb_head[t], p))
            XFAIL(0xB2);
        BCLR(umb_head[t], p);
        do {
            pte_drop(pte_get(t, p));
            pte_set(t, p, PG_LIN(p) | PTE_ROM);
            BCLR(umb_used[t], p);
            p++;
        } while (p < 0xF0 && BIT(umb_used[t], p) && !BIT(umb_head[t], p));
        tlb(t);
        AX = 1;
        return;
    }
    }
    XFAIL(0x80);                                /* nicht vorhanden       */
}

/* ---- DPMI 0.9 fuer die Box, Stufe 1 (wie VDPMI bei OS/2) ------------------
   Erkennung INT 2Fh 1687h (im Monitor, vor dem DOS), Einsprung F000:0510
   "ARPL E2h; RETF": das Programm laeuft danach in Ring 3 im Protected
   Mode der Box (gleiches CR3, eigene LDT-Seite statt der 16 Eintraege der
   Task-LDT). Ausnahmen/INT n aus Ring 3 kommen ueber start.asm (.pm) nach
   dpmi_trap: INT n trifft ein IDT-Tor mit DPL 0 -> #GP mit Fehlercode
   n*8+2. INT 31h = Dienste; andere INT n an den PM-Handler des Programms
   (0205h) oder, ohne Handler, an den Real Mode (V86): Rahmen sichern,
   Ruecksprung F000:0520 "ARPL E3h" holt ihn zurueck. Die Standard-PM-
   Vektoren zeigen auf Stummel "HLT; n; IRET(D)" im Stummel-Segment (HLT
   in Ring 3 = #GP), so koennen Programme zum alten Handler weiterreichen.
   Wechsel V86 <-> PM ueber Rueckgabe 2 an start.asm (der Rahmen wird
   umgeschrieben; esp0 der Boxen liegt 16 Byte tiefer, damit die vier
   V86-Segmentfelder auch ueber einem PM-Rahmen Platz haben).
   Speicher (0501h) ab 4 MB im Adressraum der Box, Seitentabellen dafuer
   mit PTE_OWN im Seitenverzeichnis. Noch nicht: Callbacks (0303h), Raw
   Switch (0306h), Ausnahme-Handler (0202h), Hardware-IRQs im PM. */
#define SI  (*(unsigned short *)&f->esi)
static void k2_lldt(unsigned sel);
#pragma aux k2_lldt = "lldt ax" parm [ax] modify exact [];
#define DP_SW     0x0510                /* Einsprung (Moduswechsel)      */
#define DP_BACK   0x0520                /* Ruecksprung aus dem Real Mode */
#define DP_STUB   0x0C00                /* Standard-PM-Vektoren (4 Byte) */
#define DP_RMSTK  0x0400                /* Real-Mode-Stapel (Bytes, vom Programm) */
#define NDPM      24                    /* Speicherbloecke               */
#define DPCT      6                     /* Schachtelung Real Mode        */
#define LDT_N     512
#define SEL(i)    (((i) << 3) | 7)      /* LDT, RPL 3                    */
#define IDX(s)    ((s) >> 3)
#define CT_INT    1
#define CT_CALL   2                     /* 0300h-0302h                   */
#define CT_DOSAL  3                     /* 0100h                         */
#define CT_DOSFR  4                     /* 0101h                         */
#define CT_MOU    5                     /* Maus-Rueckruf aus dem PM      */

static unsigned char dp_on[NTASK], dp_32[NTASK], dp_dep[NTASK];
static unsigned dp_exss[NTASK];         /* Selektor des Ausnahme-Stapels  */
static unsigned dp_cbss[NTASK];         /* Selektor fuer den RM-Stapel (Callback) */
struct dpcb { unsigned short psel, ssel; unsigned long poff, soff; };
#define NCB       16
#define DP_CB     0x0700                /* Callback-Einspruenge (4 Byte) */
#define DP_CBRET  0x0548                /* Ruecksprung aus einem Callback */
#define CB(t, k)  ((struct dpcb __far *)MK_FP(SCRATCH, 1728 + (k) * 12))
#define DP_EXC    0x0580                /* Standard-Ausnahme-Handler (32 x 4 Byte;
                                           nicht 0600: dort liegt F060:000A EMMXXXX0
                                           und F060:0020 INT 67h) */
#define DP_EXRET  0x0540                /* Ruecksprung aus einem Handler  */
#define EXV(t, n) ((unsigned long __far *)MK_FP(SCRATCH, 1536 + (n) * 6))
static unsigned dp_rmseg[NTASK], dp_stub[NTASK], dp_psp[NTASK];
static pg_t dp_ldt[NTASK], dp_st[NTASK];        /* LDT-Seite, Zustandsseite */
static struct desc dp_oldldt[NTASK];
static unsigned char dp_lmap[NTASK][LDT_N / 8];
static struct { unsigned long lpg, npg; } dp_mem[NTASK][NDPM];
/* DPMI 0800h: eingeblendeter Geraetespeicher (z. B. linearer Bild-
   speicher einer VBE-2.0-Karte); Seiten ohne PTE_OWN, nie freigegeben */


/* Zustandsseite: 0 PM-Vektoren (256 x 6), 1536 Ausnahme-Handler (32 x 6),
   2048 Rahmen (DPCT x 64) */
struct dpctx {
    unsigned short type, cs, ss, ds, es, fs, gs, rsv;
    unsigned long eip, eflags, esp, lin, r[8];  /* r: EDI..EAX       */
};

/* 1/2/4 Byte mit einem Zugriff passender Breite (je Falle mehrfach:
   Befehl lesen, Stapel - byteweise mit 32-Bit-Schieben war es teuer) */
static unsigned long lrd(unsigned long lin, unsigned n)
{
    lwin(VWIN_IDX, lin);
    if (n == 1)
        return *(unsigned char __far *)MK_FP(VWIN, 0);
    if (n == 2)
        return *(unsigned short __far *)MK_FP(VWIN, 0);
    return *(unsigned long __far *)MK_FP(VWIN, 0);
}

static void lwr(unsigned long lin, unsigned long v, unsigned n)
{
    lwin(VWIN_IDX, lin);
    if (n == 1)
        *(unsigned char __far *)MK_FP(VWIN, 0) = (unsigned char)v;
    else if (n == 2)
        *(unsigned short __far *)MK_FP(VWIN, 0) = (unsigned short)v;
    else
        *(unsigned long __far *)MK_FP(VWIN, 0) = v;
}

/* Basis des zuletzt gefragten Selektors (dp_trap liest je Falle CS):
   jede Aenderung der LDT laeuft ueber ldesc/ldt_free/dp31 und verwirft
   ihn */
#define NSBC 4                          /* je Selektor (sel >> 3) & 3    */
static unsigned sbc_sel[NSBC];
static signed char sbc_t = -1;          /* gilt fuer diese Box            */
static unsigned long sbc_base[NSBC];

static unsigned char __far *ldt_e(int t, unsigned i)      /* 8 Byte  */
{
    scratch_map(PG_LIN(dp_ldt[t]), 4095);
    return (unsigned char __far *)MK_FP(SCRATCH, i * 8);
}

static void ldesc(int t, unsigned i, unsigned long base, unsigned long lim,
                  unsigned char acc, unsigned char fl)
{
    unsigned char __far *d = ldt_e(t, i);
    sbc_t = -1;
    if (lim > 0xFFFFFUL) {                      /* Seitengranularitaet  */
        lim >>= 12;
        fl |= 0x80;
    }
    d[0] = (unsigned char)lim;
    d[1] = (unsigned char)(lim >> 8);
    d[2] = (unsigned char)base;
    d[3] = (unsigned char)(base >> 8);
    d[4] = (unsigned char)(base >> 16);
    d[5] = acc;
    d[6] = (unsigned char)(fl | ((lim >> 16) & 0x0F));
    d[7] = (unsigned char)(base >> 24);
}

static unsigned long sel_base(int t, unsigned s)
{
    unsigned char __far *d;
    unsigned k;
    if (!(s & 4) || IDX(s) >= LDT_N)
        return 0;
    k = (s >> 3) & (NSBC - 1);
    if (sbc_t == t && sbc_sel[k] == s)
        return sbc_base[k];
    if (sbc_t != t) {
        for (k = 0; k < NSBC; k++)
            sbc_sel[k] = 0;
        sbc_t = (signed char)t;
        k = (s >> 3) & (NSBC - 1);
    }
    d = ldt_e(t, IDX(s));
    sbc_base[k] = d[2] | ((unsigned long)d[3] << 8) | ((unsigned long)d[4] << 16) |
                  ((unsigned long)d[7] << 24);
    sbc_sel[k] = s;
    return sbc_base[k];
}

static int sel_ok(int t, unsigned s)            /* gueltiger Datenselektor */
{
    return s == 0 || ((s & 7) == 7 && IDX(s) < LDT_N &&
           (dp_lmap[t][IDX(s) >> 3] & (1 << (IDX(s) & 7))));
}

static int ldt_alloc(int t, unsigned n)         /* n am Stueck -> Index */
{
    unsigned i, k;
    for (i = 1; i + n <= LDT_N; i++) {
        for (k = 0; k < n && !(dp_lmap[t][(i + k) >> 3] & (1 << ((i + k) & 7))); k++)
            ;
        if (k == n) {
            for (k = 0; k < n; k++) {
                dp_lmap[t][(i + k) >> 3] |= (unsigned char)(1 << ((i + k) & 7));
                ldesc(t, i + k, 0, 0, 0xF2, 0);  /* Daten, Basis 0, Laenge 1 */
            }
            return (int)i;
        }
        i += k;
    }
    return -1;
}

static void ldt_free(int t, unsigned i)
{
    unsigned char __far *d;
    sbc_t = -1;
    dp_lmap[t][i >> 3] &= (unsigned char)~(1 << (i & 7));
    d = ldt_e(t, i);
    d[5] = 0;
}

static struct dpctx __far *ctx(int t, unsigned k)
{
    scratch_map(PG_LIN(dp_st[t]), 4095);
    return (struct dpctx __far *)MK_FP(SCRATCH, 2048 + k * 64);
}

static unsigned long __far *pmvec(int t, unsigned n)    /* Offset, dann Sel */
{
    scratch_map(PG_LIN(dp_st[t]), 4095);
    return (unsigned long __far *)MK_FP(SCRATCH, n * 6);
}

/* Seitentabelle fuer die lineare Seite lp der Box (ab 4 MB eigene) */
static int lpte_set(int t, unsigned long lp, unsigned long e)
{
    unsigned pdi = (unsigned)(lp >> 10);
    unsigned long pde;
    pg_t pt;
    scratch2_map(PG_LIN(pd_box[t]), 4095);
    pde = get32(SCRATCH2, pdi);
    if (!(pde & PTE_P)) {
        if (!e || !(pt = own_page()))
            return !e;
        scratch2_map(PG_LIN(pd_box[t]), 4095);
        put32(SCRATCH2, pdi, PG_LIN(pt) | PTE_BOX | PTE_OWN);
        pde = PG_LIN(pt);
    }
    scratch2_map(pde & ~0xFFFUL, 4095);
    put32(SCRATCH2, (unsigned)(lp & 0x3FF), e);
    return 1;
}

static unsigned long lpte_get(int t, unsigned long lp)
{
    unsigned long pde;
    scratch2_map(PG_LIN(pd_box[t]), 4095);
    pde = get32(SCRATCH2, (unsigned)(lp >> 10));
    if (!(pde & PTE_P))
        return 0;
    scratch2_map(pde & ~0xFFFUL, 4095);
    return get32(SCRATCH2, (unsigned)(lp & 0x3FF));
}

static void dmem_unmap(int t, unsigned long lp, unsigned long n)
{
    unsigned long e;
    while (n--) {
        e = lpte_get(t, lp + n);
        if (e & (PTE_P | PTE_SWP)) {
            pte_drop(e);
            lpte_set(t, lp + n, 0);
        }
    }
}

static int dmem_map(int t, unsigned long lp, unsigned long n)
{
    unsigned long i;
    pg_t p;
    for (i = 0; i < n; i++)
        if (!(p = own_page_hi()) || !lpte_set(t, lp + i, PG_LIN(p) | PTE_BOX | PTE_OWN)) {
            if (p)
                vp_free(p);
            dmem_unmap(t, lp, i);
            return 0;
        }
    return 1;
}

/* Luecke fuer n Seiten ab 64 MB (Seite 4000h, darunter XMS), bis 1 GB */
static unsigned long dmem_gap(int t, unsigned long n)
{
    unsigned long s = XMS_TOP;
    unsigned i;
again:
    for (i = 0; i < NDPM; i++)
        if (dp_mem[t][i].npg && s < dp_mem[t][i].lpg + dp_mem[t][i].npg &&
            s + n > dp_mem[t][i].lpg) {
            s = dp_mem[t][i].lpg + dp_mem[t][i].npg;
            goto again;
        }
    for (i = 0; i < NDPP; i++)
        if (dp_phys[t][i].npg && s < dp_phys[t][i].lpg + dp_phys[t][i].npg &&
            s + n > dp_phys[t][i].lpg) {
            s = dp_phys[t][i].lpg + dp_phys[t][i].npg;
            goto again;
        }
    return s + n <= 0x40000UL ? s : 0;
}

static void dphys_unmap(int t, unsigned i)
{
    unsigned long k;
    for (k = 0; k < dp_phys[t][i].npg; k++)
        lpte_set(t, dp_phys[t][i].lpg + k, 0);
    dp_phys[t][i].npg = 0;
}

static void dpmi_end(int t)             /* Programm fertig / Box zu      */
{
    unsigned i;
    dp_inirq[t] = 0;
    dp_npatch[t] = 0;
    cli_dt = -1;
    pvi_t[t] = 0;
    vidle[t] = 0;
    vdd_event(t, VDD_EV_PMEND, 0);
    if (!dp_on[t])
        return;
    for (i = 0; i < NDPM; i++)
        if (dp_mem[t][i].npg) {
            dmem_unmap(t, dp_mem[t][i].lpg, dp_mem[t][i].npg);
            dp_mem[t][i].npg = 0;
        }
    for (i = 0; i < NDPP; i++)
        if (dp_phys[t][i].npg) {
            dphys_unmap(t, i);
        }
    gdt[LDT_IDX(t)] = dp_oldldt[t];     /* wieder die Task-LDT          */
    tss[t].ldt = 0;
    if (current == t)
        k2_lldt(LDT_SEL(t));
    vp_free(dp_ldt[t]);
    vp_free(dp_st[t]);
    dp_ldt[t] = dp_st[t] = 0;
    sbc_t = -1;
    dp_on[t] = dp_dep[t] = 0;
    tlb(t);
}

/* Moduswechsel (Einsprung F000:0510 per Fernaufruf aus dem Real Mode).
   AX Bit 0 = 32-Bit-Programm, ES = Speicher fuer den Host (DP_RMSTK). */
static unsigned dp_switch(struct v86_frame *f, int t)
{
    unsigned rcs, rip, i, ci, di_, si_, ei, pi, k;
    unsigned long sp = (unsigned)f->r.esp;
    unsigned ss = (unsigned)f->r.ss;

    rip = rd16(ss, (unsigned)sp);               /* Ruecksprung (RETF)    */
    rcs = rd16(ss, (unsigned)sp + 2);
    if (dp_on[t] || !(dp_ldt[t] = own_page()) || !(dp_st[t] = own_page())) {
        if (!dp_on[t] && dp_ldt[t]) {
            vp_free(dp_ldt[t]);
            dp_ldt[t] = 0;
        }
        f->r.eflags |= 1;                       /* CF: geht nicht        */
        return 0;
    }
    for (i = 0; i < LDT_N / 8; i++)
        dp_lmap[t][i] = 0;
    dp_32[t] = (unsigned char)(AX & 1);
    dp_rmseg[t] = (unsigned)f->r.es;
    dp_dep[t] = 0;
    dp_psp[t] = redir_psp(t);
    /* LDT der Task umbiegen (Basis = Kernel-Alias der Seite) */
    dp_oldldt[t] = gdt[LDT_IDX(t)];
    {
        struct desc *g = &gdt[LDT_IDX(t)];
        unsigned long b = KALIAS + PG_LIN(dp_ldt[t]);
        g->limit = LDT_N * 8 - 1;
        g->base_lo = (unsigned short)b;
        g->base_hi = (unsigned char)(b >> 16);
        g->access = 0x82;
        g->rsv = (unsigned short)((b >> 24) << 8);
    }
    if (pvi_ok < 0)                             /* PVI: CPUID 1, EDX Bit 1 */
        pvi_ok = (signed char)(cpu_flid() && (cpu_feat() & 2));
    pvi_back[t] = get_ticks();                  /* erst 1 s lernen (POPF) */
    tss[t].ldt = LDT_SEL(t);                    /* (das TSS der Box hatte
                                                   0: nach jedem Taskwechsel
                                                   war LDTR leer -> #GP) */
    k2_lldt(LDT_SEL(t));                        /* LDTR neu laden        */
    ci = ldt_alloc(t, 1);
    ldesc(t, ci, (unsigned long)rcs << 4, 0xFFFF, 0xFA, 0);
    di_ = ldt_alloc(t, 1);
    ldesc(t, di_, (unsigned long)(unsigned)f->r.ds << 4, 0xFFFF, 0xF2, 0);
    si_ = ldt_alloc(t, 1);
    ldesc(t, si_, (unsigned long)ss << 4, 0xFFFF, 0xF2, 0);
    pi = ldt_alloc(t, 1);
    ldesc(t, pi, (unsigned long)dp_psp[t] << 4, 0xFF, 0xF2, 0);
    ei = ldt_alloc(t, 1);                       /* Umgebung: PSP:2Ch -> Sel */
    k = rd16(dp_psp[t], 0x2C);
    ldesc(t, ei, (unsigned long)k << 4, 0x7FFF, 0xF2, 0);
    wr16(dp_psp[t], 0x2C, SEL(ei));
    k = ldt_alloc(t, 1);                        /* Stummel-Segment F000  */
    ldesc(t, k, 0xF0000UL, 0xFFFF, 0xFA, 0);
    dp_stub[t] = SEL(k);
    /* Standard-PM-Vektoren: Stummel "HLT; n; IRET(D)" in der BIOS-Seite */
    scratch_map(pte_get(t, 0xF0) & ~0xFFFUL, 4095);
    for (i = 0; i < 256; i++)
        put32(SCRATCH, (DP_STUB >> 2) + i,
              dp_32[t] ? 0xCF6600F4UL | ((unsigned long)i << 8) : 0x90CF00F4UL | ((unsigned long)i << 8));
    for (i = 0; i < 256; i++) {
        unsigned long __far *v = pmvec(t, i);
        v[0] = DP_STUB + i * 4;
        *(unsigned short __far *)((char __far *)v + 4) = dp_stub[t];
    }
    pm_hk[t] = 0;
    scratch_map(pte_get(t, 0xF0) & ~0xFFFUL, 4095);
    for (i = 0; i < 32; i++)                    /* Standard: HLT -> Ende  */
        put32(SCRATCH, (DP_EXC >> 2) + i, 0x909090F4UL);
    put32(SCRATCH, DP_EXRET >> 2, 0x909090F4UL);
    put32(SCRATCH, MOU_PM >> 2, dp_32[t] ? 0x90CF66F4UL : 0x9090CFF4UL);
    scratch_map(PG_LIN(dp_st[t]), 4095);
    for (i = 0; i < 32; i++) {
        EXV(t, i)[0] = DP_EXC + i * 4;
        *(unsigned short __far *)((char __far *)EXV(t, i) + 4) = dp_stub[t];
    }
    dp_exss[t] = dp_cbss[t] = 0;                /* Stapel bei Bedarf     */
    for (i = 0; i < NCB; i++)
        CB(t, i)->psel = 0;
    dp_on[t] = 1;
    /* Rahmen: weiter im PM hinter dem Fernaufruf */
    f->r.eip = rip;
    f->r.cs = SEL(ci);
    f->r.esp = (sp + 4) & 0xFFFF;
    f->r.ss = SEL(si_);
    f->r.eflags = (f->r.eflags & 0x0CD4UL) | FL_IF | 2;  /* VM aus, CF 0 */
    f->r.ds = SEL(di_);
    f->r.es = SEL(pi);
    f->r.fs = f->r.gs = 0;
    vfl[t] |= FL_IF;
    return 2;
}

/* PM-Zustand aus dem Rahmen sichern (fs/gs liegen unter dem Rahmen) */
static struct dpctx __far *ctx_push(struct v86_frame *f, int t, unsigned type)
{
    struct dpctx __far *c;
    unsigned short *seg = (unsigned short *)f;
    unsigned i;
    if (dp_dep[t] >= DPCT)
        return 0;
    c = ctx(t, dp_dep[t]++);
    c->type = type;
    c->eip = f->r.eip;
    c->cs = (unsigned short)f->r.cs;
    c->eflags = f->r.eflags;
    c->esp = f->r.esp;
    c->ss = (unsigned short)f->r.ss;
    c->es = seg[0];
    c->ds = seg[1];
    c->fs = seg[-1];
    c->gs = seg[-2];
    for (i = 0; i < 8; i++)
        c->r[i] = (&f->edi)[i];
    return c;
}

/* Real Mode vorbereiten: Stapel (Host-Bereich je Ebene 128 Byte), Rueck-
   sprung F000:0520 (mit FLAGS bei iret = 1), dann cs:ip */
static unsigned rm_go(struct v86_frame *f, int t, unsigned cs, unsigned ip,
                      unsigned ss, unsigned sp, int iret)
{
    if (!ss) {
        ss = dp_rmseg[t];
        sp = DP_RMSTK - (dp_dep[t] - 1) * 128;
    }
    f->r.ss = ss;
    f->r.esp = sp;
    if (iret)
        push16(&f->r, (unsigned)(f->r.eflags & 0x0CD5) | 0x0202);
    push16(&f->r, VBIOS);
    push16(&f->r, DP_BACK);
    f->r.cs = cs;
    f->r.eip = ip;
    f->r.eflags = (f->r.eflags & 0x0CD5UL) | 0x20000UL | FL_IF | 2;  /* VM */
    vfl[t] &= ~FL_IF;                           /* wie INT: IF aus       */
    return 2;
}

/* INT n an den Real Mode */
static unsigned dp_reflect(struct v86_frame *f, int t, unsigned n)
{
    unsigned seg = rd16(0, n * 4 + 2), off = rd16(0, n * 4);
    if ((n == 0x21 && AH == 0x4C) || n == 0x20) {   /* Programmende     */
        dpmi_end(t);                            /* (DOS kehrt nicht zurueck) */
        f->r.ds = f->r.es = f->r.fs = f->r.gs = dp_psp[t];
        f->r.ss = dp_rmseg[t];
        f->r.esp = DP_RMSTK;
        push16(&f->r, 0x0202);
        push16(&f->r, VBIOS);
        push16(&f->r, DP_BACK);
        f->r.cs = seg;
        f->r.eip = off;
        f->r.eflags = (f->r.eflags & 0x0CD5UL) | 0x20000UL | FL_IF | 2;
        vfl[t] &= ~FL_IF;
        return 2;
    }
    if (!ctx_push(f, t, CT_INT))
        return 1;
    f->r.ds = f->r.es = f->r.fs = f->r.gs = dp_rmseg[t];
    return rm_go(f, t, seg, off, 0, 0, 1);
}

/* Ruecksprung aus dem Real Mode (Stummel F000:0520) -> weiter im PM */
static unsigned dp_back(struct v86_frame *f, int t)
{
    struct dpctx __far *c;
    unsigned long rfl = f->r.eflags, lin;
    unsigned i, type, rds = (unsigned)f->r.ds, res = (unsigned)f->r.es;
    if (!dp_on[t] || !dp_dep[t])
        return 1;
    c = ctx(t, --dp_dep[t]);
    type = c->type;
    lin = c->lin;
    if (type == CT_CALL) {                      /* Register -> Struktur  */
        unsigned long r[8];
        for (i = 0; i < 8; i++)
            r[i] = (&f->edi)[i];
        for (i = 0; i < 8; i++)
            if (i != 3)
                lwr(lin + i * 4, r[i], 4);
        lwr(lin + 0x20, rfl, 2);
        lwr(lin + 0x22, res, 2);
        lwr(lin + 0x24, rds, 2);
        lwr(lin + 0x26, f->r.fs, 2);
        lwr(lin + 0x28, f->r.gs, 2);
        c = ctx(t, dp_dep[t]);
        for (i = 0; i < 8; i++)
            (&f->edi)[i] = c->r[i];
        rfl &= ~1UL;                            /* 0300h selbst: geklappt */
    } else if (type == CT_MOU) {                /* Register des PM zurueck */
        c = ctx(t, dp_dep[t]);
        for (i = 0; i < 8; i++)
            (&f->edi)[i] = c->r[i];
        rfl = c->eflags;
        cb_in[t] = 0;
    } else if (type == CT_DOSAL || type == CT_DOSFR) {
        unsigned seg = (unsigned)f->eax, cf = (unsigned)(rfl & 1), bx = (unsigned)f->ebx;
        unsigned paras = (unsigned)c->r[4];     /* BX vorher             */
        int k;
        c = ctx(t, dp_dep[t]);
        for (i = 0; i < 8; i++)
            (&f->edi)[i] = c->r[i];
        if (type == CT_DOSAL) {
            if (!cf && (k = ldt_alloc(t, 1)) >= 0) {
                ldesc(t, (unsigned)k, (unsigned long)seg << 4,
                      (unsigned long)paras * 16 - 1, 0xF2, 0);
                *(unsigned short *)&f->eax = seg;
                *(unsigned short *)&f->edx = SEL(k);
            } else {
                *(unsigned short *)&f->eax = cf ? seg : 8;
                *(unsigned short *)&f->ebx = bx;
                cf = 1;
            }
        } else if (cf)
            *(unsigned short *)&f->eax = seg;
        rfl = cf ? rfl | 1 : rfl & ~1UL;
    }
    c = ctx(t, dp_dep[t]);
    f->r.eip = c->eip;
    f->r.cs = c->cs;
    f->r.eflags = (c->eflags & ~0x8D5UL) | (rfl & 0x8D5UL);
    f->r.eflags = (f->r.eflags & ~0x20000UL) | FL_IF;
    f->r.esp = c->esp;
    f->r.ss = c->ss;
    f->r.es = c->es;
    f->r.ds = c->ds;
    f->r.fs = c->fs;
    f->r.gs = c->gs;
    vfl[t] |= FL_IF;
    return 2;
}

#define DOK      (f->r.eflags &= ~1UL)
#define DERR(c)  do { *(unsigned short *)&f->eax = (c); f->r.eflags |= 1; return 0; } while (0)
#define EDI32    (dp_32[t] ? f->edi : (unsigned)f->edi)
#define ESI32    (dp_32[t] ? f->esi : (unsigned)f->esi)

static unsigned dp31(struct v86_frame *f, int t)
{
    unsigned short *es_ = (unsigned short *)f;   /* es_[0] ES, es_[1] DS */
    unsigned i, s, k;
    unsigned long lin, base, lim, n;
    struct dpctx __far *c;

    sbc_t = -1;                                 /* LDT kann sich aendern */
    DOK;
    switch (AX) {
    case 0x0000:                                /* Deskriptoren anlegen  */
        if (!CX || (k = (unsigned)ldt_alloc(t, CX)) == 0xFFFF)
            DERR(0x8011);
        AX = SEL(k);
        return 0;
    case 0x0001:                                /* freigeben             */
        if (!sel_ok(t, BX) || !BX)
            DERR(0x8022);
        ldt_free(t, IDX(BX));
        return 0;
    case 0x0002:                                /* Segment -> Selektor   */
        if ((k = (unsigned)ldt_alloc(t, 1)) == 0xFFFF)
            DERR(0x8011);
        ldesc(t, k, (unsigned long)BX << 4, 0xFFFF, 0xF2, 0);
        AX = SEL(k);
        return 0;
    case 0x0003:
        AX = 8;
        return 0;
    case 0x0006:                                /* Basis holen           */
        if (!sel_ok(t, BX) || !BX)
            DERR(0x8022);
        base = sel_base(t, BX);
        CX = (unsigned)(base >> 16);
        DX = (unsigned)base;
        return 0;
    case 0x0007:                                /* Basis setzen          */
    case 0x0008:                                /* Grenze setzen         */
    case 0x0009: {                              /* Zugriff setzen        */
        unsigned char __far *d;
        if (!sel_ok(t, BX) || !BX)
            DERR(0x8022);
        d = ldt_e(t, IDX(BX));
        if (AX == 7) {
            base = ((unsigned long)CX << 16) | DX;
            d[2] = (unsigned char)base;
            d[3] = (unsigned char)(base >> 8);
            d[4] = (unsigned char)(base >> 16);
            d[7] = (unsigned char)(base >> 24);
        } else if (AX == 8) {
            lim = ((unsigned long)CX << 16) | DX;
            d[6] &= 0x70;
            if (lim > 0xFFFFFUL) {
                lim >>= 12;
                d[6] |= 0x80;
            }
            d[0] = (unsigned char)lim;
            d[1] = (unsigned char)(lim >> 8);
            d[6] |= (unsigned char)((lim >> 16) & 0x0F);
        } else {
            if ((CL & 0x60) != 0x60 || !(CL & 0x10))    /* DPL 3, Code/Daten */
                DERR(0x8021);
            d[5] = CL | 0x80;
            d[6] = (unsigned char)((d[6] & 0x0F) | (*((unsigned char *)&f->ecx + 1) & 0xD0));
        }
        return 0;
    }
    case 0x000A:                                /* Daten-Alias           */
        if (!sel_ok(t, BX) || !BX || (k = (unsigned)ldt_alloc(t, 1)) == 0xFFFF)
            DERR(0x8022);
        {
            unsigned char __far *d = ldt_e(t, IDX(BX));
            unsigned char b[8];
            for (i = 0; i < 8; i++)
                b[i] = d[i];
            b[5] = 0xF2;
            d = ldt_e(t, k);
            for (i = 0; i < 8; i++)
                d[i] = b[i];
        }
        AX = SEL(k);
        return 0;
    case 0x000B: case 0x000C: {                 /* Deskriptor holen/setzen */
        unsigned char b[8];
        unsigned char __far *d;
        if (!sel_ok(t, BX) || !BX)
            DERR(0x8022);
        lin = sel_base(t, es_[0]) + EDI32;
        if (AX == 0x0B) {
            d = ldt_e(t, IDX(BX));
            for (i = 0; i < 8; i++)
                b[i] = d[i];
            for (i = 0; i < 8; i++)
                lwr(lin + i, b[i], 1);
        } else {
            for (i = 0; i < 8; i++)
                b[i] = (unsigned char)lrd(lin + i, 1);
            if ((b[5] & 0x70) != 0x70)
                DERR(0x8021);
            d = ldt_e(t, IDX(BX));
            for (i = 0; i < 8; i++)
                d[i] = b[i];
        }
        return 0;
    }
    case 0x0100: case 0x0101:                   /* DOS-Speicher          */
        k = AX;
        if (k == 0x101 && (!sel_ok(t, DX) || !DX))
            DERR(0x8022);
        if (!ctx_push(f, t, k == 0x100 ? CT_DOSAL : CT_DOSFR))
            DERR(0x8010);
        if (k == 0x101) {
            f->r.es = (unsigned)(sel_base(t, DX) >> 4);
            ldt_free(t, IDX(DX));
            AX = 0x4900;
        } else
            AX = 0x4800;
        f->r.ds = f->r.fs = f->r.gs = dp_rmseg[t];
        if (k == 0x100)
            f->r.es = dp_rmseg[t];
        return rm_go(f, t, rd16(0, 0x86), rd16(0, 0x84), 0, 0, 1);
    case 0x0200:                                /* RM-Vektor holen       */
        CX = rd16(0, BL * 4 + 2);
        DX = rd16(0, BL * 4);
        return 0;
    case 0x0201:
        wr16(0, BL * 4 + 2, CX);
        wr16(0, BL * 4, DX);
        return 0;
    case 0x0202: case 0x0203: {                 /* Ausnahme-Handler      */
        unsigned long __far *v;
        if (BL > 31)
            DERR(0x8021);
        if (AX == 0x0203 && !sel_ok(t, CX) && CX != dp_stub[t])
            DERR(0x8022);
        scratch_map(PG_LIN(dp_st[t]), 4095);
        v = EXV(t, BL);
        if (AX == 0x0202) {
            CX = *(unsigned short __far *)((char __far *)v + 4);
            f->edx = dp_32[t] ? v[0] : (unsigned)v[0];
        } else {
            v[0] = dp_32[t] ? f->edx : DX;
            *(unsigned short __far *)((char __far *)v + 4) = CX;
        }
        return 0;
    }
    case 0x0204: {                              /* PM-Vektor holen       */
        unsigned long __far *v = pmvec(t, BL);
        CX = *(unsigned short __far *)((char __far *)v + 4);
        f->edx = dp_32[t] ? v[0] : (unsigned)v[0];
        return 0;
    }
    case 0x0205: {
        unsigned long __far *v;
        if (!sel_ok(t, CX) && CX != dp_stub[t])
            DERR(0x8022);
        v = pmvec(t, BL);
        v[0] = dp_32[t] ? f->edx : DX;
        *(unsigned short __far *)((char __far *)v + 4) = CX;
        if (BL < 16) {                          /* Merker fuer pm_hooked */
            if (CX == dp_stub[t] && v[0] == DP_STUB + BL * 4)
                pm_hk[t] &= ~(1u << BL);
            else
                pm_hk[t] |= 1u << BL;
        }
        if (BL >= 8 && BL < 16)                 /* Leitung hat einen Handler */
            vidle[t] &= (unsigned char)~(1 << (BL - 8));
        vdd_event(t, VDD_EV_PMHOOK, BL);        /* (VSB: Leitung der SB) */
        return 0;
    }
    case 0x0300: case 0x0301: case 0x0302: {    /* Real Mode aufrufen    */
        unsigned fn = AX, rcs, rip, rss, rsp;
        s = BL;
        lin = sel_base(t, es_[0]) + EDI32;
        if (!(c = ctx_push(f, t, CT_CALL)))
            DERR(0x8010);
        c->lin = lin;
        for (i = 0; i < 8; i++)
            if (i != 3)
                (&f->edi)[i] = lrd(lin + i * 4, 4);
        f->r.es = lrd(lin + 0x22, 2);
        f->r.ds = lrd(lin + 0x24, 2);
        f->r.fs = lrd(lin + 0x26, 2);
        f->r.gs = lrd(lin + 0x28, 2);
        rip = (unsigned)lrd(lin + 0x2A, 2);
        rcs = (unsigned)lrd(lin + 0x2C, 2);
        rsp = (unsigned)lrd(lin + 0x2E, 2);
        rss = (unsigned)lrd(lin + 0x30, 2);
        f->r.eflags = (f->r.eflags & ~0x0CD5UL) | (lrd(lin + 0x20, 2) & 0x0CD5);
        if (fn == 0x0300) {                     /* INT BL ueber die IVT  */
            rcs = rd16(0, s * 4 + 2);
            rip = rd16(0, s * 4);
        }
        return rm_go(f, t, rcs, rip, rss, rsp, fn != 0x0301);
    }
    case 0x0305:                                /* Zustand sichern: keiner */
        AX = 0;                                 /* (Puffer 0, RETF genuegt) */
        BX = VBIOS;
        CX = DP_BACK + 8;                       /* F000:0528 RETF        */
        SI = dp_stub[t];
        f->edi = dp_32[t] ? DP_BACK + 12 : DP_BACK + 8;  /* 052C: RETFD */
        return 0;
    case 0x0306:                                /* Raw Switch: Adressen  */
        BX = VBIOS;                             /* RM -> PM: F000:0534   */
        CX = 0x0534;
        SI = dp_stub[t];                        /* PM -> RM: Stummel:0530 */
        f->edi = 0x0530;
        return 0;
    case 0x0400:                                /* Version 0.90          */
        AX = 0x005A;
        BX = 0x0003;                            /* 32 Bit, an V86 weiter */
        CL = 3;
        DX = 0x0870;
        return 0;
    case 0x0500: {                              /* freier Speicher       */
        unsigned long fr = kf_mem_avail_kb() >> 2;
        if (fr > 0x3C000UL)             /* mehr als der DPMI-Bereich der Box */
            fr = 0x3C000UL;             /* (64 MB - 1 GB) geht nicht; ueber */
                                        /* 2 GB in Byte hielt DOS/4GW den Wert */
                                        /* fuer negativ ("not enough memory") */
        lin = sel_base(t, es_[0]) + EDI32;
        lwr(lin, fr << 12, 4);
        for (i = 1; i < 12; i++)
            lwr(lin + i * 4, i == 1 || i == 2 || i == 4 || i == 6 ? fr : 0xFFFFFFFFUL, 4);
        return 0;
    }
    case 0x0501: case 0x0503: {                 /* Speicher anlegen/aendern */
        n = ((((unsigned long)BX << 16) | CX) + 4095) >> 12;
        if (!n)
            DERR(0x8021);
        if (AX == 0x0503) {                     /* neu anlegen, kopieren */
            k = SI;                             /* Handle = Index + 1    */
            if (!k || k > NDPM || !dp_mem[t][k - 1].npg)
                DERR(0x8023);
            k--;
            if (n <= dp_mem[t][k].npg) {
                dmem_unmap(t, dp_mem[t][k].lpg + n, dp_mem[t][k].npg - n);
                dp_mem[t][k].npg = n;
                lin = dp_mem[t][k].lpg << 12;
                goto done501;
            }
            if (!(lin = dmem_gap(t, n)) || !dmem_map(t, lin, n))
                DERR(0x8013);
            {                                   /* Seiten umhaengen      */
                unsigned long j;
                for (j = 0; j < dp_mem[t][k].npg; j++) {
                    unsigned long e = lpte_get(t, lin + j), o = lpte_get(t, dp_mem[t][k].lpg + j);
                    vp_free(e >> 12);
                    lpte_set(t, lin + j, o);
                    lpte_set(t, dp_mem[t][k].lpg + j, 0);
                }
            }
            dp_mem[t][k].lpg = lin;
            dp_mem[t][k].npg = n;
            lin <<= 12;
            tlb(t);
            goto done501;
        }
        for (k = 0; k < NDPM && dp_mem[t][k].npg; k++)
            ;
        if (k == NDPM)
            DERR(0x8016);
        if (n > kf_mem_avail_kb() >> 2 || !(lin = dmem_gap(t, n)) || !dmem_map(t, lin, n))
            DERR(0x8013);
        dp_mem[t][k].lpg = lin;
        dp_mem[t][k].npg = n;
        lin <<= 12;
        tlb(t);
done501:
        BX = (unsigned)(lin >> 16);
        CX = (unsigned)lin;
        SI = k + 1;
        DI = 0;
        return 0;
    }
    case 0x0502:                                /* Speicher freigeben    */
        k = SI;
        if (!k || k > NDPM || !dp_mem[t][k - 1].npg)
            DERR(0x8023);
        dmem_unmap(t, dp_mem[t][k - 1].lpg, dp_mem[t][k - 1].npg);
        dp_mem[t][k - 1].npg = 0;
        tlb(t);
        return 0;
    case 0x0800: {                              /* Geraetespeicher einblenden */
        /* BX:CX physisch, SI:DI Groesse -> BX:CX linear. Nur oberhalb
           des RAM (Bildspeicher, Karten) - nie Speicher des Systems */
        unsigned long ph = ((unsigned long)BX << 16) | CX;
        unsigned long sz = ((unsigned long)SI << 16) | DI, pp, np;
        if (!sz || ph + sz - 1 < ph)
            DERR(0x8021);
        pp = ph >> 12;
        np = ((ph & 0xFFF) + sz + 4095) >> 12;
        if (pp < mem_top_pg())
            DERR(0x8025);                       /* RAM: nicht erlaubt    */
        for (k = 0; k < NDPP; k++)              /* schon da? gleich zurueck */
            if (dp_phys[t][k].npg && dp_phys[t][k].ppg == pp &&
                dp_phys[t][k].npg >= np)
                goto done800;
        for (k = 0; k < NDPP && dp_phys[t][k].npg; k++)
            ;
        if (k == NDPP || !(lin = dmem_gap(t, np)))
            DERR(0x8012);
        dp_phys[t][k].lpg = lin;
        dp_phys[t][k].ppg = pp;
        dp_phys[t][k].npg = np;
        for (i = 0; i < np; i++)
            if (!lpte_set(t, lin + i, PG_LIN(pp + i) | PTE_BOX)) {
                dp_phys[t][k].npg = i;
                dphys_unmap(t, k);
                DERR(0x8013);
            }
        if (vga_bgk[t] == 4)
            vga_svmap(t, 1);                    /* hinten: auf die Sicherung */
        tlb(t);
done800:
        lin = (dp_phys[t][k].lpg << 12) + (ph & 0xFFF);
        BX = (unsigned)(lin >> 16);
        CX = (unsigned)lin;
        return 0;
    }
    case 0x0801: {                              /* wieder ausblenden     */
        unsigned long l = (((unsigned long)BX << 16) | CX) >> 12;
        for (k = 0; k < NDPP; k++)
            if (dp_phys[t][k].npg && dp_phys[t][k].lpg == l) {
                dphys_unmap(t, k);
                tlb(t);
                return 0;
            }
        DERR(0x8025);
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603:
    case 0x0702: case 0x0703:                   /* (DPMI 1.0) Auslagern  */
        return 0;                               /* nichts auszulagern    */
    case 0x0303: {                              /* Real-Mode-Callback    */
        struct dpcb __far *cb;
        scratch_map(PG_LIN(dp_st[t]), 4095);
        for (k = 0; k < NCB; k++)
            if (!CB(t, k)->psel)
                break;
        if (k == NCB)
            DERR(0x8015);
        cb = CB(t, k);
        cb->psel = es_[1];                      /* DS:(E)SI Prozedur     */
        cb->poff = ESI32;
        cb->ssel = es_[0];                      /* ES:(E)DI Struktur     */
        cb->soff = EDI32;
        CX = VBIOS;
        DX = DP_CB + k * 4;
        return 0;
    }
    case 0x0304:
        scratch_map(PG_LIN(dp_st[t]), 4095);
        k = (DX - DP_CB) >> 2;
        if (CX != VBIOS || DX < DP_CB || k >= NCB || !CB(t, k)->psel)
            DERR(0x8024);
        CB(t, k)->psel = 0;
        return 0;
    case 0x0604:
        BX = 0;
        CX = 4096;
        return 0;
    case 0x0900: case 0x0901: case 0x0902:      /* virtuelles IF         */
        k = (vfl[t] & FL_IF) != 0;
        if (AX == 0x0900)
            vfl[t] &= ~FL_IF;
        else if (AX == 0x0901)
            vfl[t] |= FL_IF;
        AL = (unsigned char)k;
        return 0;
    }
    DERR(0x8001);                               /* nicht unterstuetzt    */
}

/* INT n aus dem PM: 31h, sonst Handler des Programms oder Real Mode */
static unsigned dp_int(struct v86_frame *f, int t, unsigned n)
{
    unsigned long __far *v, off, sp, b;
    unsigned sel;
    if (n == 0x31)
        return dp31(f, t);
    if (n == 0x2F && AX == 0x1686) {            /* bin ich im PM?        */
        AX = 0;
        return 0;
    }
    v = pmvec(t, n);
    off = v[0];
    sel = *(unsigned short __far *)((char __far *)v + 4);
    if (sel == dp_stub[t] && off == DP_STUB + n * 4)
        return dp_reflect(f, t, n);             /* kein eigener Handler  */
    /* Unterbrechung im PM nachbilden: FLAGS, CS, IP auf den Stapel */
    b = sel_base(t, (unsigned)f->r.ss);
    sp = dp_32[t] ? f->r.esp : (unsigned)f->r.esp;
    if (dp_32[t]) {
        sp -= 12;
        lwr(b + sp + 8, (f->r.eflags & ~FL_IF) | (vfl[t] & FL_IF), 4);
        lwr(b + sp + 4, f->r.cs, 4);
        lwr(b + sp, f->r.eip, 4);
        f->r.esp = sp;
    } else {
        sp = (sp - 6) & 0xFFFF;
        lwr(b + sp + 4, (f->r.eflags & ~FL_IF) | (vfl[t] & FL_IF), 2);
        lwr(b + sp + 2, f->r.cs, 2);
        lwr(b + sp, f->r.eip, 2);
        f->r.esp = (f->r.esp & 0xFFFF0000UL) | sp;
    }
    f->r.cs = sel;
    f->r.eip = off;
    vfl[t] &= ~FL_IF;
    f->r.eflags &= ~FL_TF;
    return 0;
}


/* hat das Programm im PM einen eigenen Handler fuer n (0205h)? */
static int pm_hooked(int t, unsigned n)
{
    unsigned long __far *v;
    if (!dp_on[t] || !dp_st[t])
        return 0;
    if (n < 16)                                 /* ohne Fenster: je Falle */
        return (pm_hk[t] >> n) & 1;             /* gefragt (dp_hwint)    */
    v = pmvec(t, n);
    return !(*(unsigned short __far *)((char __far *)v + 4) == dp_stub[t] &&
             v[0] == DP_STUB + n * 4);
}

/* Unterbrechung im PM nachbilden: FLAGS, CS, IP auf den Stapel des
   Programms, weiter beim Handler (wie INT n) */
static unsigned dp_iss[NTASK];          /* Stapel vor der Unterbrechung  */
static unsigned long dp_isp[NTASK];
static void dp_push(struct v86_iret *r, int t, unsigned n)
{
    unsigned long __far *v = pmvec(t, n), off = v[0], sp, b, fl;
    unsigned sel = *(unsigned short __far *)((char __far *)v + 4);
    fl = (r->eflags & ~(unsigned long)FL_IF) | (vfl[t] & FL_IF);
    b = sel_base(t, (unsigned)r->ss);
    dp_inirq[t] = 1;
    dp_iss[t] = (unsigned)r->ss;
    dp_isp[t] = dp_32[t] ? r->esp : (r->esp & 0xFFFF);
    if (dp_32[t]) {
        sp = r->esp - 12;
        lwr(b + sp + 8, fl, 4);
        lwr(b + sp + 4, r->cs, 4);
        lwr(b + sp, r->eip, 4);
        r->esp = sp;
    } else {
        sp = (r->esp - 6) & 0xFFFF;
        lwr(b + sp + 4, fl, 2);
        lwr(b + sp + 2, r->cs, 2);
        lwr(b + sp, r->eip, 2);
        r->esp = (r->esp & 0xFFFF0000UL) | sp;
    }
    r->cs = sel;
    r->eip = off;
    /* virtuelles IF bleibt an: das IRET des Handlers trappt im PM nicht,
       ein geloeschtes IF bliebe bis zum naechsten STI aus (Tasten kamen
       verspaetet). Gegen Schachteln derselben Quelle: visr bis zum EOI,
       der Takt hoechstens einmal je irq0. */
    r->eflags &= ~(unsigned long)FL_TF;
}

/* Takt und Tastatur an die PM-Handler des Programms (DOOM/DMX: eigener
   INT 8 mit 140 Hz, INT 9). Ohne PM-Handler bleiben sie fuer den Real
   Mode liegen. Rahmen: Ring 3 aus irq0 oder nach einem PM-Trap. */

static void pvi_cr4(int t)
{
    if (pvi_cur != pvi_t[t]) {
        pvi_cur = pvi_t[t];
        k2_cr4(pvi_cur);
    }
}

/* VME (CR4 Bit 0, ab Pentium, wie OS/2 2.11/Windows 95): in einer Box
   ohne DPMI setzen CLI/STI und 16-Bit-PUSHF/POPF/IRET nur EFLAGS.VIF,
   ohne Falle; INT n faengt weiter der Monitor (Umleitungs-Bitmap im TSS
   ganz 1). Das virtuelle IF lebt dann in VIF: vme_in holt es beim
   Eintritt (v86_exc, v86_tick), vme_out legt es beim Ruecksprung zurueck
   und setzt VIP, wenn ein Interrupt wartet (das naechste STI/POPF/IRET,
   das IF einschaltet, trappt dann). Anlass: Diggers Zaehlerschleife
   "CLI; Latch; IN; IN; STI" - waehrend der Fallen war IF aus, ein Takt
   kam verspaetet, der Lautsprecher-Ton klang leicht verzerrt. */

static void vme_in(struct v86_iret *r, int t)
{
    if (!vme_on[t])
        return;
    vme_on[t] = 0;
    pvi_t[t] &= ~CR4_VME;               /* CR4 stellt der naechste Ausgang */
    if (r->eflags & FL_VIF)
        vfl[t] |= FL_IF;
    else
        vfl[t] &= ~FL_IF;
    r->eflags &= ~(FL_VIF | FL_VIP);
}

static void vme_out(struct v86_iret *r, int t)
{
    if (pvi_ok < 0)
        pvi_ok = (signed char)(cpu_flid() && (cpu_feat() & 2));
    r->eflags &= ~(FL_VIF | FL_VIP);
    if (pvi_ok && !dp_on[t] && !(box_hk[t] & HK_PVI) && !(r->eflags & FL_TF)) {
        if (vfl[t] & FL_IF)
            r->eflags |= FL_VIF;
        else if (vt_pend[t] || virq[t] || kq_h[t] != kq_t[t])
            r->eflags |= FL_VIP;
        irb_set(t, 0x21);               /* gehoert INT 21h/29h dem DOS,  */
        irb_set(t, 0x29);               /* ohne Falle durch die IVT;     */
        irb_set(t, 0x2A);               /* 2Ah nie (Stummel ist IRET),   */
        irb_set(t, 0x2F);               /* 2Fh nach BOXSHELL             */
        vme_on[t] = 1;
        pvi_t[t] |= CR4_VME;
    } else
        pvi_t[t] &= ~CR4_VME;
    pvi_cr4(t);
}

static void pvi_in(struct v86_iret *r, int t)
{
    if (!(pvi_cur & CR4_PVI) || (r->cs & 3) != 3)
        return;
    if (!(r->eflags & FL_VIF)) {
        if (vfl[t] & FL_IF)
            dp_cli_t[t] = get_ticks();          /* fuer den Wachhund     */
        vfl[t] &= ~FL_IF;
    } else
        vfl[t] |= FL_IF;
}

static void pvi_out(struct v86_iret *r, int t)
{
    if (pvi_ok && dp_on[t] && !(pvi_t[t] & CR4_PVI) && !(box_hk[t] & HK_PVI) &&
        get_ticks() - pvi_back[t] >= 100)
        pvi_t[t] |= CR4_PVI;
    pvi_cr4(t);
    r->eflags &= ~(FL_VIF | FL_VIP);
    if (!(pvi_t[t] & CR4_PVI) || (r->cs & 3) != 3)
        return;
    if (vfl[t] & FL_IF)
        r->eflags |= FL_VIF;
    else if (vt_pend[t] || virq[t] || kq_h[t] != kq_t[t])
        r->eflags |= FL_VIP;                    /* STI soll trappen      */
}

static void dp_hwint(struct v86_iret *r, int t)
{
    if (!dp_on[t] || (r->cs & 3) != 3)
        return;
    account(t);
    vdd_tk(t);
    if (!(vfl[t] & FL_IF)) {
        /* PUSHF/CLI/.../POPF: das POPF trappt in Ring 3 nicht und laesst
           das IF stehen - das virtuelle IF bliebe fuer immer aus (DMX in
           DOOM: Takt und Sound standen). Wachhund wie bei DOSEMU: nach
           2 Takten ohne STI gilt es als wieder an. */
        if ((box_hk[t] & HK_WDOG) || get_ticks() - dp_cli_t[t] < 3) {
            return;
        }
        vfl[t] |= FL_IF;
        if ((pvi_t[t] & CR4_PVI) && dp_npatch[t] < NPATCH) {  /* POPF ohne Patch: */
            pvi_t[t] &= ~CR4_PVI;                      /* eine Weile ohne PVI,  */
            pvi_back[t] = get_ticks();          /* damit das CLI trappt  */
        }
    }
    if (vt_pend[t] && !(visr[t] & 1) && !(vimr[t] & 1) && pm_hooked(t, 8) && t8_ok(t)) {
        vt_pend[t]--;
        visr[t] |= 1;
        dp_push(r, t, 8);
        return;
    }
    if (virq[t]) {                              /* Modul (DMX: SB)       */
        unsigned q;
        for (q = 2; q <= 7; q++) {
            unsigned m = 1 << q;
            if ((virq[t] & m) && !(visr[t] & ((m << 1) - 1)) && !(vimr[t] & m) &&
                pm_hooked(t, 8 + q)) {
                virq[t] &= (unsigned char)~m;
                visr[t] |= (unsigned char)m;
                dp_push(r, t, 8 + q);
                return;
            }
        }
    }
    if (key_ready(t) && pm_hooked(t, 9)) {
        key_t0[t] = get_ticks();
        vcur[t] = (unsigned char)kq_get(t);
        visr[t] |= 2;
        dp_push(r, t, 9);
        return;
    }
    cb_pmtick(r, t);                            /* Rueckruf (Maus 0Ch)   */
}

/* gesperrter Stapel des Hosts (eine Seite): oben Ausnahmen, ab 800h
   abwaerts Callbacks */
static int dp_exstack(int t)
{
    unsigned long lp;
    unsigned k;
    int k2;
    if (dp_exss[t])
        return 1;
    lp = dmem_gap(t, 1);
    if (!lp || !dmem_map(t, lp, 1) || (k2 = ldt_alloc(t, 1)) < 0)
        return 0;
    ldesc(t, (unsigned)k2, lp << 12, 0xFFF, 0xF2, dp_32[t] ? 0x40 : 0);
    dp_exss[t] = SEL(k2);
    for (k = 0; k < NDPM && dp_mem[t][k].npg; k++)
        ;
    if (k < NDPM) {                             /* beim Ende freigeben   */
        dp_mem[t][k].lpg = lp;
        dp_mem[t][k].npg = 1;
    }
    tlb(t);
    return 1;
}

/* Callback (0303h) aus dem Real Mode: Register in die Struktur, PM-
   Prozedur mit DS:(E)SI = RM-Stapel, ES:(E)DI = Struktur, IRET-Rahmen
   zurueck nach Stummel:0548 */
static unsigned dp_cbcall(struct v86_frame *f, int t, unsigned k)
{
    struct dpcb __far *cb;
    unsigned short psel, ssel;
    unsigned long poff, soff, b, sp, rsp;
    unsigned i, w = dp_32[t] ? 4 : 2;
    int k2;
    if (!dp_on[t] || k >= NCB || !dp_exstack(t))
        return 1;
    scratch_map(PG_LIN(dp_st[t]), 4095);
    cb = CB(t, k);
    psel = cb->psel;
    ssel = cb->ssel;
    poff = cb->poff;
    soff = cb->soff;
    if (!psel)
        return 1;
    b = sel_base(t, ssel) + soff;
    for (i = 0; i < 8; i++)
        if (i != 3)
            lwr(b + i * 4, (&f->edi)[i], 4);
    lwr(b + 0x20, f->r.eflags, 2);
    lwr(b + 0x22, f->r.es, 2);
    lwr(b + 0x24, f->r.ds, 2);
    lwr(b + 0x26, f->r.fs, 2);
    lwr(b + 0x28, f->r.gs, 2);
    lwr(b + 0x2A, f->r.eip, 2);
    lwr(b + 0x2C, f->r.cs, 2);
    rsp = (unsigned)f->r.esp;
    lwr(b + 0x2E, rsp, 2);
    lwr(b + 0x30, f->r.ss, 2);
    if (!dp_cbss[t]) {
        if ((k2 = ldt_alloc(t, 1)) < 0)
            return 1;
        dp_cbss[t] = SEL(k2);
    }
    ldesc(t, IDX(dp_cbss[t]), (unsigned long)(unsigned)f->r.ss << 4, 0xFFFF, 0xF2, 0);
    sp = 0x800 - 3 * w;                         /* IRET-Rahmen           */
    b = sel_base(t, dp_exss[t]);
    lwr(b + sp, DP_CBRET, w);
    lwr(b + sp + w, dp_stub[t], w);
    lwr(b + sp + 2 * w, (f->r.eflags & 0x0CD5UL) | 2, w);
    f->r.ss = dp_exss[t];
    f->r.esp = sp;
    f->r.cs = psel;
    f->r.eip = poff;
    f->r.ds = dp_cbss[t];
    f->r.es = ssel;
    f->r.fs = f->r.gs = 0;
    f->esi = rsp;
    f->edi = soff;
    f->r.eflags = (f->r.eflags & 0x0CD5UL) | FL_IF | 2;   /* VM aus      */
    vfl[t] &= ~FL_IF;
    return 2;
}

/* nach dem IRET der Callback-Prozedur: ES:(E)DI = Struktur -> Real Mode */
static unsigned dp_cbret(struct v86_frame *f, int t)
{
    unsigned short *seg = (unsigned short *)f;
    unsigned long b = sel_base(t, seg[0]) + (dp_32[t] ? f->edi : (unsigned)f->edi);
    unsigned i;
    unsigned long r[8];
    for (i = 0; i < 8; i++)
        r[i] = i == 3 ? 0 : lrd(b + i * 4, 4);
    for (i = 0; i < 8; i++)
        if (i != 3)
            (&f->edi)[i] = r[i];
    f->r.eflags = (lrd(b + 0x20, 2) & 0x0CD5UL) | 0x20000UL | FL_IF | 2;
    f->r.es = lrd(b + 0x22, 2);
    f->r.ds = lrd(b + 0x24, 2);
    f->r.fs = lrd(b + 0x26, 2);
    f->r.gs = lrd(b + 0x28, 2);
    f->r.eip = lrd(b + 0x2A, 2);
    f->r.cs = lrd(b + 0x2C, 2);
    f->r.esp = lrd(b + 0x2E, 2);
    f->r.ss = lrd(b + 0x30, 2);
    vfl[t] |= FL_IF;
    return 2;
}

/* Ausnahme an den Handler des Programms (0203h): Rahmen nach DPMI 0.9
   auf dem Ausnahme-Stapel, Ruecksprung per RETF nach Stummel:0540 */
static unsigned dp_exc(struct v86_frame *f, int t)
{
    unsigned long __far *v, off, sp, b;
    unsigned sel, k, w = dp_32[t] ? 4 : 2;
    unsigned long fr[8];
    if (f->vec > 31)
        return 1;
    scratch_map(PG_LIN(dp_st[t]), 4095);
    v = EXV(t, f->vec);
    off = v[0];
    sel = *(unsigned short __far *)((char __far *)v + 4);
    if (sel == dp_stub[t] && off == DP_EXC + f->vec * 4)
        return 1;                               /* kein eigener Handler  */
    if (!dp_exstack(t))
        return 1;
    fr[0] = DP_EXRET;                           /* Ruecksprung           */
    fr[1] = dp_stub[t];
    fr[2] = f->err;
    fr[3] = f->r.eip;
    fr[4] = f->r.cs;
    fr[5] = (f->r.eflags & ~FL_IF) | (vfl[t] & FL_IF);
    fr[6] = f->r.esp;
    fr[7] = f->r.ss;
    b = sel_base(t, dp_exss[t]);
    sp = 0x1000 - 8 * w;
    for (k = 0; k < 8; k++)
        lwr(b + sp + k * w, fr[k], w);
    f->r.ss = dp_exss[t];
    f->r.esp = sp;
    f->r.cs = sel;
    f->r.eip = off;
    vfl[t] &= ~FL_IF;
    f->r.eflags &= ~FL_TF;
    return 0;
}

static unsigned dp_exret(struct v86_frame *f, int t)  /* nach RETF        */
{
    unsigned w = dp_32[t] ? 4 : 2;
    unsigned long b = sel_base(t, (unsigned)f->r.ss) + (dp_32[t] ? f->r.esp : (unsigned)f->r.esp);
    /* RETF hat ret-EIP/CS genommen: Fehlercode, EIP, CS, EFLAGS, ESP, SS */
    unsigned long eip = lrd(b + w, w), cs = lrd(b + 2 * w, w), fl = lrd(b + 3 * w, w);
    unsigned long esp = lrd(b + 4 * w, w), ss = lrd(b + 5 * w, w);
    if (!sel_ok(t, (unsigned)cs) || !cs || !sel_ok(t, (unsigned)ss) || !ss)
        return 1;
    f->r.eip = eip;
    f->r.cs = cs;
    f->r.esp = esp;
    f->r.ss = ss;
    if (fl & FL_IF)
        vfl[t] |= FL_IF;
    else
        vfl[t] &= ~FL_IF;
    f->r.eflags = (fl & 0x0CD5UL) | FL_IF | 2;
    return 0;
}

/* DMX (DOOM 1.9) sperrt mit PUSHFD; POP reg; CLI ... PUSH reg; POPFD.
   POPF trappt im PM nicht - das virtuelle IF bliebe bis zum Wachhund
   aus (82 statt 140 Takte je Sekunde, Tasten und Ton hakten). Beim CLI
   nach PUSHF; POP reg das erste PUSH reg (dasselbe Register); POPF(D)
   der naechsten 1 KB suchen und das POPF durch HLT ersetzen: das trappt
   (#GP), dp_trap bildet POPF nach (dp_patched). Das Register muss
   passen - so trifft die Suche keine Daten. Hardware-Haltepunkte
   gingen auf echten CPUs, 86Box bildet sie aber nur mit einer Option
   nach.
   Jede schon untersuchte CLI-Stelle merkt sich cli_done (die letzten 16,
   fuer eine Box): ohne PVI (486) trappt jedes CLI, und die Suche kopiert
   jedes Mal 1 KB - DOOM 1.9 lief auf dem DX4-100 deshalb spuerbar
   langsamer als unter MS-DOS. */
static void dp_patch_popf(int t, unsigned long lin)
{
    unsigned i;
    unsigned char r = (unsigned char)lrd(lin - 1, 1);
    if (lrd(lin - 2, 1) != 0x9C || r < 0x58 || r > 0x5F || dp_npatch[t] >= NPATCH ||
        (box_hk[t] & HK_POPF))
        return;
    if (cli_dt != t) {
        cli_dt = (signed char)t;
        cli_nd = 0;
    }
    for (i = 0; i < cli_nd && i < NCLID; i++)
        if (cli_done[i] == lin)
            return;
    cli_done[cli_nd & (NCLID - 1)] = lin;
    if (!++cli_nd)
        cli_nd = NCLID;                 /* Ueberlauf: alle 16 gelten     */
    r = (unsigned char)(r - 8);                 /* POP reg -> PUSH reg   */
    lwin(VWIN_IDX, lin + 1);
    far_copy(KDATA, (unsigned)pf_buf, VWIN, 0, sizeof(pf_buf));
    for (i = 1; i < sizeof(pf_buf); i++) {
        if ((unsigned char)pf_buf[i - 1] != r)
            continue;
        if ((unsigned char)pf_buf[i] == 0xF4 && dp_patched(t, lin + 1 + i))
            return;                             /* schon erledigt        */
        if ((unsigned char)pf_buf[i] == 0x9D) {
            lwr(lin + 1 + i, 0xF4, 1);
            dp_patch[t][dp_npatch[t]++] = lin + 1 + i;
            return;
        }
    }
}

static int dp_patched(int t, unsigned long lin)
{
    unsigned i;
    for (i = 0; i < dp_npatch[t]; i++)
        if (dp_patch[t][i] == lin)
            return 1;
    return 0;
}

static unsigned dp_trap(struct v86_frame *f, int t)
{
    unsigned long lin;
    unsigned char op;
    unsigned n, cs = (unsigned)f->r.cs;

    if (f->vec == 14 && (swap_pf() || vga_pf(t)))   /* ausgelagert, Bild hinten */
        return 0;
    if (f->vec != 13)
        return dp_exc(f, t);
    lin = sel_base(t, cs) + (dp_32[t] ? f->r.eip : (unsigned)f->r.eip);
    op = (unsigned char)lrd(lin, 1);
    if (f->err & 2) {                           /* INT n                 */
        n = (unsigned)f->err >> 3;
        f->r.eip += op == 0xCD ? 2 : 1;
        return dp_int(f, t, n);
    }
    if (f->err)
        return dp_exc(f, t);
    if (cs == dp_stub[t] && op == 0xF4 && (unsigned)f->r.eip == DP_EXRET)
        return dp_exret(f, t);
    if (cs == dp_stub[t] && op == 0xF4 && (unsigned)f->r.eip == DP_CBRET)
        return dp_cbret(f, t);
    if (cs == dp_stub[t] && op == 0xF4 && (unsigned)f->r.eip == MOU_PM)
        return cb_pmcb(f, t);
    if (cs == dp_stub[t] && op == 0xF4 && (unsigned)f->r.eip >= DP_EXC &&
        (unsigned)f->r.eip < DP_EXC + 128)
        return 1;                               /* Standard: Ende        */
    if (cs == dp_stub[t] && op == 0xF4 && (unsigned)f->r.eip == 0x0530) {
        /* Raw Switch PM -> RM: AX DS, CX ES, DX SS, (E)BX (E)SP, SI CS,
           (E)DI (E)IP - alles Real-Mode-Segmente */
        f->r.ds = AX;
        f->r.es = CX;
        f->r.ss = DX;
        f->r.esp = dp_32[t] ? f->ebx : BX;
        f->r.cs = SI;
        f->r.eip = dp_32[t] ? f->edi : DI;
        f->r.fs = f->r.gs = 0;
        f->r.eflags = (f->r.eflags & 0x0CD5UL) | 0x20000UL | FL_IF | 2;
        return 2;
    }
    if (cs == dp_stub[t] && op == 0xF4) {       /* Standard-Vektor: an RM */
        n = (unsigned)lrd(lin + 1, 1);
        f->r.eip += 2;                          /* weiter bei IRET(D)    */
        if (n >= 0x0B && n <= 0x0F && vec_def(n)) {
            /* IRQ 3-7 ohne Handler auch im Real Mode: gleich quittieren
               statt PM -> RM -> PM (der Stummel tut dort nichts) */
            visr[t] &= (unsigned char)~(1 << (n - 8));
            if (!(box_hk[t] & HK_IDLE))         /* Leitung ruht: ein Modul */
                vidle[t] |= (unsigned char)(1 << (n - 8));  /* (SB) loest  */
                                                /* sie nicht mehr aus     */
            return 0;
        }
        return dp_reflect(f, t, n);
    }
    {                                           /* Praefixe uebergehen   */
        unsigned pl = 0, o16 = 0;
        while ((op == 0x66 || op == 0x67 || op == 0xF2 || op == 0xF3 || op == 0x2E ||
                op == 0x3E || op == 0x26 || op == 0x36 || op == 0x64 || op == 0x65) && pl < 8) {
            if (op == 0x66)
                o16 ^= 1;
            pl++;
            op = (unsigned char)lrd(lin + pl, 1);
        }
        f->r.eip += pl;
        lin += pl;
        (void)o16;
    }
    switch (op) {
    case 0xFA:                                  /* CLI/STI: virtuell     */
        dp_patch_popf(t, lin);                  /* POPF dazu (DMX)       */
        dp_popf_if[t] = (unsigned char)((vfl[t] & FL_IF) != 0);
        vfl[t] &= ~FL_IF;
        dp_cli_t[t] = get_ticks();
        f->r.eip++;
        return 0;
    case 0xFB:
        vfl[t] |= FL_IF;
        f->r.eip++;
        return 0;
    case 0xF4:                                  /* HLT: abgeben          */
        if (dp_patched(t, lin)) {               /* gepatchtes POPF(D)    */
            unsigned long sb_ = sel_base(t, (unsigned)f->r.ss), sp, v;
            unsigned o32 = dp_32[t] ^ (lrd(lin - 1, 1) == 0x66);
            sp = dp_32[t] ? f->r.esp : (f->r.esp & 0xFFFF);
            v = lrd(sb_ + sp, o32 ? 4 : 2);
            if (dp_32[t])
                f->r.esp += o32 ? 4 : 2;
            else
                f->r.esp = (f->r.esp & 0xFFFF0000UL) | ((sp + (o32 ? 4 : 2)) & 0xFFFF);
            f->r.eflags = (f->r.eflags & ~0x0CD5UL) | (v & 0x0CD5UL);   /* CF..OF, DF */
            if ((pvi_cur & CR4_PVI) ? o32 && (v & FL_VIF) : dp_popf_if[t])
                vfl[t] |= FL_IF;                /* IF wie vor dem CLI    */
            f->r.eip++;
            return 0;
        }
        f->r.eip++;
        hlt_wait(t);
        return 0;
    case 0xE4: case 0xE5: case 0xEC: case 0xED: /* IN                    */
        n = op <= 0xE5 ? (unsigned)lrd(lin + 1, 1) : DX;
        if (vbe_io(t, n)) {                     /* Bochs-VBE: vorn, Wort */
            if (op & 1)
                AX = k3_inw(n);
            else
                AL = (unsigned char)inb(n);
        } else if (n >= 0x400 && (op & 1))     /* ueber 3FFh als Wort an */
            AX = vdd_io(t, VDD_INW, n, 0, box_fg(t));   /* die Module (EMU8000) */
        else if (op & 1)
            AX = port_in(t, n) | (port_in(t, n + 1) << 8);
        else
            AL = port_in(t, n);
        f->r.eip += op <= 0xE5 ? 2 : 1;
        return 0;
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: /* OUT                   */
        n = op <= 0xE7 ? (unsigned)lrd(lin + 1, 1) : DX;
        if (vbe_io(t, n)) {
            if (op & 1)
                k3_outw(n, AX);
            else
                outb(n, AL);
        } else if (n >= 0x400 && (op & 1))
            vdd_io(t, VDD_OUTW, n, AX, box_fg(t));
        else {
            port_out(t, n, AL);
            if (op & 1)
                port_out(t, n + 1, AH);
        }
        f->r.eip += op <= 0xE7 ? 2 : 1;
        return 0;
    }
    return dp_exc(f, t);
}

/* Port trappt? (I/O-Bitmap des TSS reicht bis 3DFh, darueber immer) */
static int io_trap(int t, unsigned p)
{
    return p >= 0x3E0 || (tss[t].iob[p >> 3] & (1 << (p & 7)));
}

/* Nach einer erledigten Falle: folgt gleich die naechste (CLI/STI, IN/OUT
   auf einen gefangenen Port, HLT), hoechstens nach MOV EDX,imm32 oder
   XOR/SUB reg,reg (EAX/EDX)? Dann diese einfachen Befehle hier
   ausfuehren und die Falle gleich mit erledigen - DMX (DOOM-Ton) hat
   Folgen wie OUT; MOV EDX,20h; OUT; MOV EDX,0A0h; SUB EAX,EAX; IN, auf
   dem 486 (ohne PVI) je Befehl eine Falle. Nur 32-Bit-Code ohne
   Praefixe. -> 1 naechster Befehl trappt (eip steht davor) */
static int dp_more(struct v86_frame *f, int t)
{
    unsigned long lin = sel_base(t, (unsigned)f->r.cs) + f->r.eip, v;
    unsigned k, op, p;
    if (!dp_32[t] || (f->r.eflags & FL_TF))
        return 0;
    for (k = 0; ; k++) {
        if (((unsigned)lin & 0xFFF) > 0xFF7)    /* nicht ueber die Seite */
            return 0;                           /* hinaus lesen          */
        v = lrd(lin, 4);
        op = (unsigned)v & 0xFF;
        if (k < 3 && op == 0xBA) {              /* MOV EDX,imm32         */
            f->edx = lrd(lin + 1, 4);
            f->r.eip += 5;
            lin += 5;
            continue;
        }
        if (k < 3 && ((unsigned)v == 0xD231 || (unsigned)v == 0xC031 ||
                      (unsigned)v == 0xD229 || (unsigned)v == 0xC029)) {
            if (((unsigned)v >> 8) == 0xD2)     /* XOR/SUB EDX/EAX, selbst */
                f->edx = 0;
            else
                f->eax = 0;
            f->r.eflags = (f->r.eflags & ~0x08D5UL) | 0x44;    /* ZF PF */
            f->r.eip += 2;
            lin += 2;
            continue;
        }
        break;
    }
    if (op == 0xFA || op == 0xFB)
        return !(pvi_cur & CR4_PVI);
    if (op == 0xF4)
        return 1;
    if (op >= 0xEC && op <= 0xEF)
        p = (unsigned)f->edx;
    else if (op >= 0xE4 && op <= 0xE7)
        p = (unsigned)(v >> 8) & 0xFF;
    else
        return 0;
    return io_trap(t, p) || (op & 1 && io_trap(t, p + 1));
}

unsigned __far dpmi_trap(struct v86_frame *f)
{
    int t = current;
    unsigned rc, k;
    if (t < 0 || !dp_on[t])
        return 1;
    pvi_in(&f->r, t);
    enable_ints();
    rc = dp_trap(f, t);
    if (f->vec == 13 && !f->err)                /* gleich die naechste   */
        for (k = 0; rc == 0 && k < 4 && dp_on[t] && dp_more(f, t); k++)
            rc = dp_trap(f, t);
    irq_off();
    vga_freeze(t);
    if (rc == 0) {
        dp_hwint(&f->r, t);
        pvi_out(&f->r, t);
    }
    from_tick = 0;
    return rc;
}

/* Schreiben auf eine ROM-Seite (C0000-FFFFF ausser den eigenen Stummeln
   bei F0000): eigene, beschreibbare Kopie wie Schatten-RAM. Echte ROMs
   ignorieren Schreibzugriffe, manche BIOS (SeaBIOS/SeaVGABIOS in QEMU)
   halten Daten in ihrem Schattenbereich. -> 1 erledigt */
static int rom_cow(struct v86_frame *f, int t)
{
    unsigned long a = k2_cr2(), e;
    unsigned i = (unsigned)(a >> 12);
    pg_t pg;
    if ((f->err & 7) != 7 || a < 0xC0000UL || a >= 0x100000UL || i == 0xF0)
        return 0;
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    e = get32(SCRATCH2, i);
    if ((e & (PTE_P | PTE_W)) != PTE_P || (e & PTE_OWN))
        return 0;
    pg = vp_alloc_hi();
    if (!pg)
        return 0;
    scratch_map(e & 0xFFFFF000UL, 4095);
    scratch2_map(PG_LIN(pg), 4095);
    far_copy(SCRATCH2, 0, SCRATCH, 0, 4096);
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    put32(SCRATCH2, i, PG_LIN(pg) | PTE_BOX | PTE_OWN);
    tlb(t);
    return 1;
}

/* Schreiben auf die Stummelseite F0000h (bei echter Hardware ROM):
   verpufft wie beim ROM.  Den Befehl bei CS:IP decodieren und ueber-
   springen (RAM-Proben im oberen Speicher, z. B. CheckIt: 55AAh nach
   F000:0000 schreiben und zuruecklesen).  -> 1 uebersprungen */
static int rom_skip(struct v86_frame *f, int t)
{
    unsigned long a = k2_cr2();
    unsigned cs = (unsigned)f->r.cs, ip = (unsigned)f->r.eip, i, m, md, rm;
    unsigned char op, o32 = 0, a32 = 0, rep = 0, imm = 0, mod = 1;
    (void)t;
    if ((f->err & 7) != 7 || (unsigned)(a >> 12) != 0xF0)
        return 0;
    for (i = 0; i < 15; i++) {
        op = rd8(cs, ip + i);
        if (op == 0x66) o32 = 1;
        else if (op == 0x67) a32 = 1;
        else if (op == 0xF2 || op == 0xF3) rep = 1;
        else if (op != 0xF0 && op != 0x26 && op != 0x2E && op != 0x36 &&
                 op != 0x3E && op != 0x64 && op != 0x65)
            break;
    }
    ip += i + 1;
    if (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB) {  /* MOVS/STOS */
        unsigned long n = rep ? (a32 ? f->ecx : (f->ecx & 0xFFFF)) : 1, d;
        d = n << (op & 1 ? (o32 ? 2 : 1) : 0);   /* ohne __U4M */
        if (f->r.eflags & 0x400)
            d = 0 - d;
        if (a32) {
            f->edi += d;
            if (op < 0xAA) f->esi += d;
        } else {
            f->edi = (f->edi & 0xFFFF0000UL) | ((unsigned)f->edi + (unsigned)d);
            if (op < 0xAA)
                f->esi = (f->esi & 0xFFFF0000UL) | ((unsigned)f->esi + (unsigned)d);
        }
        if (rep)
            f->ecx = a32 ? 0 : f->ecx & 0xFFFF0000UL;
        f->r.eip = ip;
        return 1;
    }
    if (op == 0x0F) {                   /* SETcc, BTS/BTR/BTC, SHLD/SHRD */
        op = rd8(cs, ip++);
        if (op == 0xA4 || op == 0xAC || op == 0xBA)
            imm = 1;
        else if (!((op >= 0x90 && op <= 0x9F) || op == 0xA5 || op == 0xAD ||
                   op == 0xAB || op == 0xB3 || op == 0xBB))
            return 0;
    } else if (op < 0x40 && (op & 7) < 4)
        ;                               /* ALU r/m, reg               */
    else if (op == 0x80 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6)
        imm = 1;
    else if (op == 0x81 || op == 0xC7)
        imm = o32 ? 4 : 2;
    else if (op == 0x86 || op == 0x87 || op == 0x88 || op == 0x89 ||
             op == 0x8C || op == 0x8F || (op >= 0xD0 && op <= 0xD3) ||
             op == 0xFE || op == 0xFF || (op >= 0xD8 && op <= 0xDF))
        ;
    else if (op == 0xF6 || op == 0xF7) {
        if (!(rd8(cs, ip) & 0x30))      /* TEST mit Wert (schreibt nicht) */
            return 0;
    } else if (op == 0xA2 || op == 0xA3) {  /* MOV moffs, AL/AX          */
        mod = 0;
        imm = a32 ? 4 : 2;
    } else
        return 0;
    if (mod) {
        m = rd8(cs, ip++);
        md = m >> 6;
        rm = m & 7;
        if (md == 3)
            return 0;
        if (a32) {
            if (rm == 4 && (rd8(cs, ip++) & 7) == 5 && md == 0)
                ip += 4;
            if (md == 0 && rm == 5) ip += 4;
            else if (md == 1) ip++;
            else if (md == 2) ip += 4;
        } else {
            if (md == 0 && rm == 6) ip += 2;
            else if (md == 1) ip++;
            else if (md == 2) ip += 2;
        }
    }
    f->r.eip = ip + imm;
    return 1;
}

/* Kopien der ROM-Seiten verwerfen (Ende einer Grafiksitzung): das
   naechste Mal beginnt das Video-BIOS wieder mit seinem Startzustand;
   eigene Seiten fuer UMBs und den EMS-Rahmen bleiben */
static void rom_uncow(int t)
{
    unsigned p;
    unsigned long e;
    for (p = 0xC0; p < 0x100; p++) {
        if (p == 0xF0)
            continue;
        if (p >= 0xC8 && p < 0xF0 && (BIT(umb_used[t], p) ||
            (EFR(t) && p >= EFR(t) && p < EFR(t) + 16)))
            continue;
        scratch2_map(PG_LIN(pt_box[t]), 4095);
        e = get32(SCRATCH2, p);
        if (!(e & PTE_OWN))
            continue;
        put32(SCRATCH2, p, ((unsigned long)p << 12) | PTE_ROM);
        vp_free(e >> 12);
    }
    tlb(t);
}

/* HLT: bis zum naechsten (virtuellen) Interrupt warten - steht schon
   einer an, gar nicht schlafen; danach darf der Takt gleich kommen
   (Digger wartet am Highscore per HLT auf seinen INT 8 und fror ein,
   weil der Takt nur aus irq0 heraus zugestellt wurde) */
static void hlt_wait(int t)
{
    account(t);
    if (!vt_pend[t] && kq_h[t] == kq_t[t])
        kf_box_ctl(BOX_SLEEP, 1);
    from_tick = 1;                              /* inject/dp_hwint danach */
}

static unsigned monitor(struct v86_frame *f, int t)
{
    unsigned cs = (unsigned)f->r.cs, ip = (unsigned)f->r.eip, i, v, p;
    unsigned char op, o32 = 0, rep = 0;

    switch (f->vec) {
    case 0: case 1: case 3: case 4: case 5:     /* wie im Real Mode      */
        do_int(f, t, f->vec, ip);
        return 0;
    case 6:                                     /* ARPL n: Dienst n      */
        if (rd8(cs, ip) != 0x63)
            return 1;
        v = rd8(cs, ip + 1);
        f->r.eip = ip + 2;
        if (cs == VBIOS && ip == 0x0510)        /* DPMI: in den PM       */
            return dp_switch(f, t);
        if (cs == VBIOS && ip == 0x0520)        /* DPMI: zurueck in den PM */
            return dp_back(f, t);
        if (cs == VBIOS && ip >= 0x0700 && ip < 0x0740)   /* DPMI-Callback */
            return dp_cbcall(f, t, (ip - 0x0700) >> 2);
        if (cs == VBIOS && ip == 0x0534) {      /* DPMI Raw Switch RM -> PM */
            if (!dp_on[t] || !sel_ok(t, AX) || !sel_ok(t, CX) || !sel_ok(t, DX) ||
                !sel_ok(t, SI) || !SI || !DX)
                return 1;
            f->r.ds = AX;
            f->r.es = CX;
            f->r.ss = DX;
            f->r.esp = dp_32[t] ? f->ebx : BX;
            f->r.cs = SI;
            f->r.eip = dp_32[t] ? f->edi : DI;
            f->r.fs = f->r.gs = 0;
            f->r.eflags = (f->r.eflags & 0x0CD5UL) | FL_IF | 2;
            return 2;
        }
        if (cs == VBIOS && ip == XMS_STUB) {     /* Fernaufruf: kein FLAGS */
            xms(f, t);
            return 0;
        }
        if (cs == VBIOS && ip == MOU_RET)       /* Rueckruf fertig       */
            return cb_ret(f, t);
        if (cs == VBIOS && ip == MOU_TICK) {    /* Rueckruf aus dem Takt */
            cb_v86(f, t, 1);                    /* (danach IRET des Stummels) */
            return 0;
        }
        if (cs == VBIOS && ip == EMS_RET) {      /* EMS 56h: Ziel fertig  */
            vdd_svc_int(0xE6, f, t);            /* (VEMS.SYS)            */
            return 0;
        }
        if (v == 0x10) {                        /* umgebogenes INT 10h kam an */
            int w = vga_want(f, t);
            if (w == 1) {                       /* Rahmen liegt schon: BIOS */
                vga_bios(f);                    /* kehrt per IRET zurueck */
                return 0;
            }
            if (w == 2)
                return 0;
        }
        /* FLAGS des INT liegen auf dem Stack der Box: dort setzen */
        wr16((unsigned)f->r.ss, (unsigned)f->r.esp + 4,
             run_svc(v, f, t, rd16((unsigned)f->r.ss, (unsigned)f->r.esp + 4)));
        return 0;
    case 13:
        break;
    case 14:                                    /* Seitenfehler          */
        return swap_pf() || vga_pf(t) || rom_cow(f, t) || rom_skip(f, t) ? 0 : 1;
    default:
        return 1;
    }

    /* #GP: privilegierten Befehl nachfuehren */
    for (i = 0; i < 15; i++) {
        op = rd8(cs, ip + i);
        if (op == 0x66)
            o32 = 1;
        else if (op == 0xF2 || op == 0xF3)
            rep = 1;
        else if (op == 0x67 || op == 0xF0 || op == 0x26 || op == 0x2E ||
                 op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65)
            ;
        else
            break;
    }
    ip += i + 1;
    switch (op) {
    case 0x0F:                                  /* MOV r32,CRn/DRn (lesen) */
        v = rd8(cs, ip);                        /* und MOV CRn/DRn,r32: wie */
        if (v == 0x06 || v == 0x08 || v == 0x09) {  /* CLTS, INVD, WBINVD */
            ip++;                               /* (Cache-Tests): ohne      */
            break;                              /* Wirkung                 */
        }
        p = rd8(cs, ip + 1);                    /* EMM386 nachgebildet,    */
        if (v < 0x20 || v > 0x23 || (p & 0xC0) != 0xC0)   /* Schreiben    */
            return 1;                           /* wirkt nicht             */
        ip += 2;
        if (!(v & 2)) {
            unsigned long x = 0;
            if (v == 0x20 && !(p & 0x38))
                x = 0x80000011UL;               /* CR0: PG, ET, PE         */
            else if (v == 0x21 && (p & 0x38) == 0x30)
                x = 0xFFFF0FF0UL;               /* DR6 Ruhewert            */
            else if (v == 0x21 && (p & 0x38) == 0x38)
                x = 0x00000400UL;               /* DR7 Ruhewert            */
            switch (p & 7) {
            case 0: f->eax = x; break;
            case 1: f->ecx = x; break;
            case 2: f->edx = x; break;
            case 3: f->ebx = x; break;
            case 4: f->r.esp = x; break;
            case 5: f->ebp = x; break;
            case 6: f->esi = x; break;
            default: f->edi = x; break;
            }
        }
        break;
    case 0xFA:                                  /* CLI                   */
        vfl[t] &= ~FL_IF;
        break;
    case 0xFB:                                  /* STI                   */
        if (!(vfl[t] & FL_IF))
            sti_sh = !(f->r.eflags & FL_TF);    /* Schatten: erst spaeter */
        vfl[t] |= FL_IF;
        break;
    case 0x9C:                                  /* PUSHF(D)              */
        if (o32)
            push16(&f->r, (unsigned)(f->r.eflags >> 16) & ~3);  /* ohne VM/RF */
        push16(&f->r, vflags(&f->r, t));
        break;
    case 0x9D:                                  /* POPF(D)               */
        v = pop16(&f->r);
        if (o32)                        /* AC (Bit 18), ID (21): echt -
                                           486/CPUID-Erkennung (MSD) */
            f->r.eflags = (f->r.eflags & ~0x240000UL) |
                          ((unsigned long)(pop16(&f->r) & 0x24) << 16);
        set_flags(&f->r, t, v);
        break;
    case 0xCD:                                  /* INT n                 */
        do_int(f, t, rd8(cs, ip), ip + 1);
        return 0;
    case 0xCC:                                  /* INT 3                 */
        do_int(f, t, 3, ip);
        return 0;
    case 0xCE:                                  /* INTO                  */
        if (f->r.eflags & 0x800) {
            do_int(f, t, 4, ip);
            return 0;
        }
        break;
    case 0xCF:                                  /* IRET(D)               */
        if (o32) {
            ip = pop16(&f->r);
            pop16(&f->r);
            f->r.cs = pop16(&f->r);
            pop16(&f->r);
            v = pop16(&f->r);
            pop16(&f->r);
        } else {
            ip = pop16(&f->r);
            f->r.cs = pop16(&f->r);
            v = pop16(&f->r);
        }
        set_flags(&f->r, t, v);
        break;
    case 0xF4:                                  /* HLT: abgeben          */
        f->r.eip = ip;
        hlt_wait(t);
        return 0;
    case 0xE4: case 0xE5: case 0xEC: case 0xED: /* IN                    */
        p = op <= 0xE5 ? rd8(cs, ip++) : DX;
        if (p >= 0xCF8 && p <= 0xCFF) {         /* PCI-Konfiguration     */
            unsigned long pv = pci_pin(t, p);
            if (!(op & 1))
                AL = (unsigned char)pv;
            else if (o32)
                f->eax = pv;
            else
                AX = (unsigned)pv;
        } else if (vbe_io(t, p)) {              /* Bochs-VBE: vorn, Wort */
            if (op & 1)
                AX = k3_inw(p);
            else
                AL = (unsigned char)inb(p);
        } else if (p >= 0x400 && (op & 1)) {   /* ueber 3FFh als Wort an */
            unsigned fb = box_fg(t), lo;        /* die Module (EMU8000); */
            lo = vdd_io(t, VDD_INW, p, 0, fb);  /* 32 Bit = zwei Worte   */
            if (o32)
                f->eax = ((unsigned long)vdd_io(t, VDD_INW, p + 2, 0, fb) << 16) | lo;
            else
                AX = lo;
        } else if (op & 1) {
            AX = port_in(t, p) | (port_in(t, p + 1) << 8);
            if (o32)
                f->eax = AX | 0xFFFF0000UL;
        } else
            AL = port_in(t, p);
        break;
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: /* OUT                   */
        p = op <= 0xE7 ? rd8(cs, ip++) : DX;
        if (p >= 0xCF8 && p <= 0xCFB) {         /* PCI-Adresse merken    */
            if ((op & 1) && o32 && p == 0xCF8)
                pci_cfg[t] = f->eax;
            else {
                unsigned sh = (p - 0xCF8) * 8, w = op & 1 ? 16 : 8;
                unsigned long m = (w == 16 ? 0xFFFFUL : 0xFFUL) << sh;
                pci_cfg[t] = (pci_cfg[t] & ~m) |
                             (((unsigned long)(op & 1 ? AX : AL) << sh) & m);
            }
        } else if (p >= 0xCFC && p <= 0xCFF) {
            ;                                   /* schreiben: verpufft   */
        } else if (vbe_io(t, p)) {
            if (op & 1)
                k3_outw(p, AX);
            else
                outb(p, AL);
        } else if (p >= 0x400 && (op & 1)) {
            unsigned fb = box_fg(t);
            vdd_io(t, VDD_OUTW, p, AX, fb);
            if (o32)
                vdd_io(t, VDD_OUTW, p + 2, (unsigned)(f->eax >> 16), fb);
        } else {
            port_out(t, p, AL);
            if (op & 1)
                port_out(t, p + 1, AH);
        }
        break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: /* INS/OUTS              */
        if (rep)
            CX = 0;
        break;
    default:
        return 1;                               /* unbekannt: Abbruch    */
    }
    f->r.eip = ip;
    return 0;
}

/* ---- Rueckruf eines Moduls in die Box (VS_CBARM) -----------------------
   Wie VDHPushFarCall bei OS/2: ein Modul (VMOUSE.SYS fuer INT 33h 0Ch/14h)
   setzt einen Rueckruf seg:ofs; der Kernel ruft ihn, wenn die Box ihn
   annehmen kann, und fragt vorher das Modul (VDD_CBQ), ob etwas ansteht und
   mit welchen Registern. Zustellung im V86-Modus, wenn die Box mit IF an
   trappt (Register auf ihren Stapel, Ruecksprung ueber F000:0570 "ARPL
   E7h"), aus dem Takt ueber den Stummel F000:0574 (mit VME faellt sonst
   nichts mehr: MSD 2.11 wartet ohne INT) und waehrend eines DPMI-Programms
   im PM ueber F000:0578 (CT_MOU, Aufruf im Real Mode). */
#define CB_ON(t)  ((cb_seg[t] || cb_ofs[t]) && !cb_in[t])

/* INT aus dem PM ohne Uebersetzung (dp_reflect setzt ES auf den Host-
   Bereich): ES ist dann keine Real-Mode-Adresse */
static int pm_esbad(int t, unsigned es)
{
    return dp_on[t] && dp_dep[t] && es == dp_rmseg[t];
}

static int cb_q(int t, int take, struct vdd_call *c)
{
    c->val = (unsigned short)take;
    return vdd_cbq(t, c) == 0;
}

static void cb_v86(struct v86_frame *f, int t, int force)
{
    struct v86_iret *r = &f->r;
    struct vdd_call c;
    if (!CB_ON(t) || (!force && (!(vfl[t] & FL_IF) || sti_sh)) || !cb_q(t, 1, &c))
        return;
    cb_in[t] = 1;
    push16(r, vflags(r, t));                    /* wie ein Interrupt     */
    push16(r, (unsigned)r->cs);
    push16(r, (unsigned)r->eip);
    push16(r, AX); push16(r, BX); push16(r, CX); push16(r, DX);
    push16(r, (unsigned)f->esi); push16(r, DI); push16(r, BP);
    push16(r, (unsigned)r->ds); push16(r, (unsigned)r->es);
    push16(r, VBIOS);                           /* RETF -> ARPL E7h       */
    push16(r, MOU_RET);
    AX = c.ax; BX = c.bx; CX = c.cx; DX = c.dx;
    *(unsigned short *)&f->esi = c.si;
    DI = c.di;
    vfl[t] &= ~FL_IF;
    r->eflags &= ~(unsigned long)FL_TF;
    r->cs = cb_seg[t];
    r->eip = cb_ofs[t];
}

/* Aus dem Takt (v86_tick): dort fehlen die Register der Box - steht etwas
   an, auf den Stummel F000:0574 umlenken, dessen Falle ruft cb_v86 */
static void cb_tick(struct v86_iret *r, int t)
{
    struct vdd_call c;
    if (!CB_ON(t) || !(vfl[t] & FL_IF) || sti_sh || !cb_q(t, 0, &c))
        return;
    push16(r, vflags(r, t));                    /* wie ein Interrupt     */
    push16(r, (unsigned)r->cs);
    push16(r, (unsigned)r->eip);
    vfl[t] &= ~FL_IF;
    r->eflags &= ~(unsigned long)FL_TF;
    r->cs = VBIOS;
    r->eip = MOU_TICK;
}

/* Rueckruf ist fertig (RETF nach F000:0570): Register und IRET */
static int cb_ret(struct v86_frame *f, int t)
{
    struct v86_iret *r = &f->r;
    r->es = pop16(r);
    r->ds = pop16(r);
    BP = pop16(r);
    DI = pop16(r);
    *(unsigned short *)&f->esi = pop16(r);
    DX = pop16(r); CX = pop16(r); BX = pop16(r); AX = pop16(r);
    r->eip = pop16(r);
    r->cs = pop16(r);
    set_flags(r, t, pop16(r));
    cb_in[t] = 0;
    return 0;
}

/* Waehrend ein DPMI-Programm im PM laeuft (DOS-Extender melden ihren
   Rueckruf mit einer Real-Mode-Adresse an): dp_hwint lenkt das Programm
   per IRET-Rahmen auf den Stummel F000:0578 "HLT; IRET(D)", cb_pmcb sichert
   den PM-Stand (CT_MOU) und ruft den Rueckruf im Real Mode, sein RETF
   fuehrt ueber DP_BACK zurueck, dp_back stellt die Register her. */
static void cb_pmtick(struct v86_iret *r, int t)
{
    unsigned long sp, b, fl;
    struct vdd_call c;
    if (!CB_ON(t) || dp_dep[t] >= DPCT || !cb_q(t, 0, &c))
        return;
    cb_in[t] = 1;                               /* bis der Rueckruf fertig */
    fl = (r->eflags & ~(unsigned long)FL_IF) | (vfl[t] & FL_IF);
    b = sel_base(t, (unsigned)r->ss);
    if (dp_32[t]) {
        sp = r->esp - 12;
        lwr(b + sp + 8, fl, 4);
        lwr(b + sp + 4, r->cs, 4);
        lwr(b + sp, r->eip, 4);
        r->esp = sp;
    } else {
        sp = (r->esp - 6) & 0xFFFF;
        lwr(b + sp + 4, fl, 2);
        lwr(b + sp + 2, r->cs, 2);
        lwr(b + sp, r->eip, 2);
        r->esp = (r->esp & 0xFFFF0000UL) | sp;
    }
    r->cs = dp_stub[t];
    r->eip = MOU_PM;
    r->eflags &= ~(unsigned long)FL_TF;
}

static unsigned cb_pmcb(struct v86_frame *f, int t)
{
    struct vdd_call c;
    f->r.eip++;                                 /* weiter beim IRET(D)   */
    if ((!cb_seg[t] && !cb_ofs[t]) || !cb_q(t, 1, &c) || !ctx_push(f, t, CT_MOU)) {
        cb_in[t] = 0;
        return 0;
    }
    f->eax = c.ax;
    f->ebx = c.bx;
    f->ecx = c.cx;
    f->edx = c.dx;
    f->esi = c.si;
    f->edi = c.di;
    f->r.ds = f->r.es = f->r.fs = f->r.gs = dp_rmseg[t];
    return rm_go(f, t, cb_seg[t], cb_ofs[t], 0, 0, 0);
}

/* Programmstart (INT E0h 00h): Rueckruf weg, Module setzen ihren Treiber
   zurueck (VDD_EV_PROG) */
static void cb_prog(int t)
{
    /* FreeDOS legt INT 33h auf ein blosses IRET (Programme ohne Treiber
       sollen nicht abstuerzen) - zurueck auf den Stummel des Kernels, damit
       ein Modul (VMOUSE.SYS) den Vektor bekommt */
    if (rd8(rd16(0, 0xCE), rd16(0, 0xCC)) == 0xCF) {
        wr16(0, 0xCC, 0x33 * 4);
        wr16(0, 0xCE, VBIOS);
    }
    cb_seg[t] = cb_ofs[t] = 0;
    cb_in[t] = 0;
    vdd_event(t, VDD_EV_PROG, 0);
}

unsigned __far v86_exc(struct v86_frame *f)
{
    int t = current;
    unsigned rc, i;

    if (t >= 0)
        vme_in(&f->r, t);
    if (t >= 0 && sti_step[t] && f->vec == 1) { /* Befehl nach STI fertig */
        irq_off();
        inject(&f->r, t);
        vme_out(&f->r, t);
        from_tick = 0;
        return 0;
    }
    if (t >= 0 && sti_step[t]) {        /* anderer Grund (Befehl trappt  */
        sti_step[t] = 0;                /* selbst): TF weg, bevor PUSHF  */
        f->r.eflags &= ~(unsigned long)FL_TF;   /* o. Ae. es sieht       */
    }
    trace((unsigned)f->vec, (unsigned)f->r.cs, (unsigned)f->r.eip, (unsigned)f->eax);
    enable_ints();                      /* Kernel laeuft mit Interrupts  */
    rc = monitor(f, t);
    if (rc == 1) {                      /* Abbruch: Vektoren fuer die Spur */
        trc_ivt[0] = ((unsigned long)rd16(0, 0x22) << 16) | rd16(0, 0x20);
        trc_ivt[1] = ((unsigned long)rd16(0, 0x26) << 16) | rd16(0, 0x24);
        trc_ivt[2] = ((unsigned long)rd16(0, 0x72) << 16) | rd16(0, 0x70);
        for (i = 0; i < 8; i++)
            trc_code[i] = rd8((unsigned)f->r.cs, (unsigned)f->r.eip + i);
    }
    if (rc == 0) {
        unsigned long ocs = f->r.cs, oip = f->r.eip;
        irq_off();                      /* bis zum IRETD                 */
        inject(&f->r, t);
        if (f->r.cs == ocs && f->r.eip == oip)
            cb_v86(f, t, 0);            /* Rueckruf eines Moduls (Maus) */
        if (sti_sh && (vt_pend[t] || virq[t] || kq_h[t] != kq_t[t])) {
            sti_step[t] = 1;            /* nach dem naechsten Befehl     */
            f->r.eflags |= FL_TF;
        }
        sti_sh = 0;
        vme_out(&f->r, t);
    } else if (rc == 2) {
        irq_off();                      /* DPMI: weiter im PM            */
        pvi_out(&f->r, t);
    }
    from_tick = 0;
    return rc;
}
