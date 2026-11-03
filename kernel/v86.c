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
 * Laeuft im zweiten Codesegment (KTEXT2), im Kontext des Box-Tasks: dann
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
#define PTE_OWN 0x200           /* eigene Seite (EMB/UMB): bei Ende frei  */
#define BDA     0x0040
#define VSEG    0xB800
#define ROWS    25
#define COLS    80

#include "v86int.h"
#include "../include/fapi_abi.h"

static pg_t pd_box[NTASK], pt_box[NTASK];
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
static unsigned char cmos_idx[NTASK];    /* Port 70h der Box              */
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
static unsigned long pit_acc[NTASK];    /* Schritte bis zum naechsten INT 8 */
static unsigned long pit_t0[NTASK];     /* Takt der letzten Messung      */
static unsigned pit_c0[NTASK];          /* Schritte seit dem Kerneltakt dabei */
unsigned kpit_div, kpit_acc;            /* start.asm irq0: PIT-Teiler (0 = 11932) */
static unsigned char spk[NTASK];        /* Lautsprecher-Bits 61h (0-1)   */
static unsigned img_spt[NTASK], img_heads[NTASK], img_secs[NTASK];  /* Startabbild */
static unsigned xms_lim[NTASK];         /* XMS-Grenze in Seiten + 1 (0 = Vorgabe) */
static unsigned char idle_n[NTASK];     /* leere Tastaturabfragen hintereinander */

unsigned long v86_pages;                /* Seiten aller Boxen (MEM)      */
static pg_t vp_alloc(void)
{
    pg_t p = mem_alloc(1);
    if (p)
        v86_pages++;
    return p;
}
static void vp_free(pg_t p)
{
    mem_free(p, 1);
    v86_pages--;
}

static void xms_end(int t);
static void vga_freeze(int t);
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
static unsigned long t8_last[NTASK];
/* Hardware-Vektoren beim Programmstart (INT E0h 00h): bricht ein Spiel
   ab (Strg+C), ohne INT 8/9 zurueckzunehmen, zeigen sie in freigegebenen
   Speicher - die offen gehaltene Box stellt sie beim Parken wieder her */
static const unsigned char hwvec[] = { 0x08, 0x09, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x1C,
                                       0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77 };
static unsigned long ivt_save[NTASK][sizeof(hwvec)];    /* Kerneltakt des letzten INT 8 der Box */         /* Zustellung aus irq0 (Takt erlaubt) */
static int pm_hooked(int t, unsigned n);
static void dp_hwint(struct v86_iret *r, int t);
static int box_fg(int t);
static void blink_hw(int off);
static void spk_hw(int t, int on);
static void vga_leave(int t);
static void tlb(int t);
static unsigned char vga_raw[NTASK];
static void ems_end(int t);
static void ems_box(int t);
static void dpmi_end(int t);
static unsigned char dp_inirq[NTASK];   /* PM-Handler laeuft (IRET kommt ohne Trap) */
static unsigned dp_switch(struct v86_frame *f, int t);
static unsigned dp_back(struct v86_frame *f, int t);
unsigned __far redir_psp(int t);
static void ems(struct v86_frame *f, int t);
static void ems_pick_frame(void);
static unsigned char ems_frame;         /* erste EMS-Rahmenseite, 0 = keiner */
static unsigned ems_lim[NTASK];         /* EMS: logische Seiten + 1, 0 = Vorgabe */


/* Thunks (erstes Codesegment) */
unsigned __far kf_box_fapi(unsigned fn, unsigned usel, unsigned a1, unsigned a2, unsigned a3);
int  __far kf_box_ctl(unsigned op, unsigned a);
int  __far kf_vio_fg(void);
int  __far kf_kbd_get(void);
int  __far kf_kbd_peek(void);
unsigned __far kf_kbd_shift(void);
void __far kf_kbd_flush(void);
void __far kf_vio_tty(int s, unsigned sel, unsigned off, unsigned len);
void __far kf_vio_setmode(int s, int full, int owner);
void __far kf_vio_setcur(int s, int row, int col);
void __far kf_vio_dirty(int s);
unsigned long __far kf_vio_buf_lin(int s);

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

static char cap[ROWS * (COLS + 1)];     /* letzter Text einer Box        */
static unsigned cap_len;

/* Text der Box (Zeilen bis zum Cursor) fuer v86_replay merken */
static void capture(int t)
{
    unsigned long e;
    unsigned r, c, n, last, crow;
    unsigned char ch;

    cap_len = 0;
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    e = get32(SCRATCH2, 0xB8);
    scratch_map(get32(SCRATCH2, 0) & ~0xFFFUL, 4095);
    crow = *(unsigned char __far *)MK_FP(SCRATCH, 0x451);
    if (*(unsigned char __far *)MK_FP(SCRATCH, 0x450))
        crow++;                         /* Cursor mitten in der Zeile    */
    if (crow > ROWS)
        crow = ROWS;
    scratch_map(e & ~0xFFFUL, 4095);
    for (r = 0, n = 0; r < crow; r++) {
        for (c = 0, last = 0; c < COLS; c++)
            if (*(unsigned char __far *)MK_FP(SCRATCH, (r * COLS + c) * 2) != ' ')
                last = c + 1;
        for (c = 0; c < last; c++) {
            ch = *(unsigned char __far *)MK_FP(SCRATCH, (r * COLS + c) * 2);
            cap[n++] = ch < ' ' ? ' ' : (char)ch;
        }
        cap[n++] = '\n';
    }
    cap_len = n;
}

void __far v86_destroy(int t)
{
    unsigned i;
    unsigned long e;

    if (!pd_box[t])
        return;
    dpmi_end(t);
    if (box_ses[t] >= 0 && box_fg(t))
        kf_kbd_flush();                 /* wie v86_park                  */
    if (spk[t] && box_fg(t))
        spk_hw(t, 0);
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
                if ((ee & PTE_P) && (ee & PTE_OWN))
                    vp_free(ee >> 12);
            }
            vp_free(e >> 12);
        }
    }
    if (pt_box[t]) {
        scratch_map(PG_LIN(pt_box[t]), 4095);
        for (i = 0; i < 0x100; i++) {
            e = get32(SCRATCH, i);
            if ((e & PTE_P) && (i <= 0xA0 || i == 0xF0 || (e & PTE_OWN)))  /* nicht B8/B9! */
                vp_free(e >> 12);
        }
        vp_free(pt_box[t]);
        pt_box[t] = 0;
    }
    vp_free(pd_box[t]);
    pd_box[t] = 0;
    xms_end(t);
    ems_end(t);
}

/* nach dem Ende einer Box in der Sitzung ihres Aufrufers: ihren Text dort
   ausgeben (s < 0: verwerfen) */
void __far v86_replay(int s)
{
    if (s >= 0 && cap_len)
        kf_vio_tty(s, KDATA, (unsigned)cap, cap_len);
    cap_len = 0;
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
        p = vp_alloc();
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
    visr[t] = vimr[t] = 0;
    kq_h[t] = kq_t[t] = 0;
    pit_rl[t] = 0;
    pit_acc[t] = pit_t0[t] = 0;
    pit_acc_m[t] = 3;
    pit_latd[t] = pit_wlo[t] = pit_ff[t] = 0;
    spk[t] = 0;
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
    put32(SCRATCH, XMS_STUB >> 2, 0x90CBE163UL);    /* 63 E1 CB: XMS     */
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
    put32(SCRATCH, DPT_OFF >> 2, 0x122502AFUL);
    put32(SCRATCH, (DPT_OFF >> 2) + 1, 0xF66CFF1BUL);
    put32(SCRATCH, (DPT_OFF >> 2) + 2, 0x0000080FUL);
    img_spt[t] = img_heads[t] = img_secs[t] = 0;
    idle_n[t] = 0;
    ems_box(t);
    return PG_LIN(pd_box[t]);
}

/* Sitzung s steht fest (task_create): Vollbild, B800 = Sitzungspuffer */
void __far v86_start(int t, int s)
{
    unsigned long b;

    if (!pt_box[t])
        return;
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
    for (t = 0; t < NTASK && (box_ses[t] < 0 || !img_secs[t]); t++)
        ;
    if (t == NTASK)
        return 0;
    s = kf_vio_fg();
    for (t = 0; t < NTASK; t++)
        if (box_ses[t] == s && img_secs[t])
            return 1;
    return 0;
}

/* n Bytes aus dem Kernel (sel:off) an linear lin der Box, Seite fuer Seite
   ueber die physischen Seiten (die Box laeuft dabei nicht) */
void __far v86_copy(int t, unsigned long lin, unsigned sel, unsigned off, unsigned n)
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
        if (!(e & PTE_P))
            return;
        scratch2_map(e & ~0xFFFUL, 4095);
        far_copy(SCRATCH2, o, sel, off, k);
        lin += k;
        off += k;
        n -= k;
    }
}

/* Scheduler: hat eine Box in ihren Bildschirm geschrieben (Dirty-Bit der
   Seiten B8/B9)? Dann Sitzung neu zeichnen. Laeuft ohne Kernelsperre in
   irgendeinem Kontext - SCRATCH sichern. */
void __far v86_poll(void)
{
    unsigned short w[8];
    unsigned long e8, e9;
    int t, cur = -2;

    if (kpit_div) {                     /* schneller Takt nur fuer die Box vorn */
        int s = kf_vio_fg(), ok = 0;
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
                cur = kf_box_ctl(BOX_TASK, 0);
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
    s = kf_vio_fg();
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

/* INT n ohne Dienst: FLAGS, CS, IP auf den Stack, ueber die IVT */
static void int_raw(struct v86_iret *r, int t, unsigned n, unsigned ip)
{
    push16(r, vflags(r, t));
    push16(r, (unsigned)r->cs);
    push16(r, ip);
    vfl[t] &= ~FL_IF;
    r->eflags &= ~(unsigned long)FL_TF;
    r->cs = rd16(0, n * 4 + 2);
    r->eip = rd16(0, n * 4);
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
static void kpit_set(unsigned dv)
{
    unsigned f;
    if (dv && dv < KPIT_MIN)
        dv = KPIT_MIN;
    if (dv >= KPIT)
        dv = 0;
    if (dv == kpit_div)
        return;
    f = irq_off();
    kpit_div = dv;
    outb(0x43, 0x36);
    outb(0x40, (dv ? dv : KPIT) & 0xFF);
    outb(0x40, (dv ? dv : KPIT) >> 8);
    irq_restore(f);
}

static void kpit_check(int t)
{
    kpit_set(pit_rl[t] && box_fg(t) ? pit_rl[t] : 0);
}

static unsigned long pit_div(int t)
{
    return pit_rl[t] ? pit_rl[t] : 65536UL;
}

/* Zeit fortschreiben: Zaehler und faellige INT 8 */
static void pit_run(int t)
{
    unsigned long el = pit_elapsed(t), dv = pit_div(t), c;
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

static unsigned char pit_in(int t, unsigned p)
{
    unsigned v;
    if (p != 0x40) {                    /* Kanal 1/2: grob laufen lassen */
        pit_c[t] -= 0x1F3;
        pit_ff[t] ^= 1;
        return (unsigned char)(pit_ff[t] ? pit_c[t] : pit_c[t] >> 8);
    }
    pit_run(t);
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
    return box_ses[t] >= 0 && kf_vio_fg() == box_ses[t];
}

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
            pit_run(t);
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
        if (box_fg(t))
            outb(0x42, v);
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
    if (!vec_def(8) || pit_rl[t] || pm_hooked(t, 8)) {  /* eigener Takt/Teiler */
        pit_run(t);                     /* Rate des Programms            */
        if (vt_pend[t] && vec_def(8) && vec_def(0x1C) && !pm_hooked(t, 8)) {
            bda_ticks(vt_pend[t]);      /* (Teiler ohne Routine)         */
            vt_pend[t] = 0;
        }
        vt_last[t] = get_ticks();
        return;
    }
    now = get_ticks();
    d = now - vt_last[t];
    vt_last[t] = now;
    if (d > 100)
        d = 100;
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
/* darf jetzt ein Takt zugestellt werden? Aus irq0 immer, sonst
   hoechstens einer je Kerneltakt (nachgeholte Takte hintereinander ->
   DOS/4GW "transfer stack overflow"; nur aus irq0 -> eine Box, die
   staendig trappt, bekam fast keinen Takt, Digger wartete ewig) */
static int t8_ok(int t)
{
    unsigned long now = get_ticks();
    if (!from_tick && now == t8_last[t])
        return 0;
    t8_last[t] = now;
    return 1;
}

static void inject(struct v86_iret *r, int t)
{
    if (spk[t] && !box_fg(t))
        spk_hw(t, 0);                           /* hinten: stumm         */
    if (pit_rl[t])
        kpit_check(t);                          /* vorn schneller Takt   */
    vga_freeze(t);
    account(t);
    if (!(vfl[t] & FL_IF))
        return;
    if (vt_pend[t] && !(visr[t] & 1) && !(vimr[t] & 1) && t8_ok(t)) {
        vt_pend[t]--;
        visr[t] |= 1;
        int_raw(r, t, 8, (unsigned)r->eip);
        return;
    }
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
    int t = kf_box_ctl(BOX_TASK, 0);
    if (t < 0 || !pt_box[t])
        return;
    from_tick = 1;                              /* je irq0 hoechstens ein */
    if (r->eflags & 0x20000UL)                  /* Takt (sonst schachteln */
        inject(r, t);                           /* nachgeholte Takte, DOS/4GW: */
    else {                                      /* "transfer stack overflow") */
        vga_freeze(t);                          /* DPMI-Programm (Ring 3) */
        if (pit_rl[t])
            kpit_check(t);
        dp_hwint(r, t);
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

static void int10(struct v86_frame *f, int t)
{
    unsigned i, r, c, n;

    cur_get();
    switch (AH) {
    case 0x00:                                  /* Modus                 */
        wr8(BDA, 0x49, AL & 0x7F);
        if ((AL & 0x7F) <= 3 || (AL & 0x7F) == 7) {
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
        }
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
        }
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
    }
    cur_put(t);
}

/* ---- Dienste ------------------------------------------------------------ */

static unsigned char svc_cf, svc_zf;    /* 0xFF = unveraendert          */

/* Taste: warten (wait = 1) oder nachsehen; -> Zeichen | Scancode << 8,
   -1 = keine */
static int key(int t, int wait)
{
    int s = box_ses[t], k;
    for (;;) {
        if (kf_vio_fg() == s) {
            k = wait ? kf_kbd_get() : kf_kbd_peek();
            if (k >= 0)
                return k;
        }
        if (!wait)
            return -1;
        kf_box_ctl(BOX_SLEEP, 1);
        account(t);                     /* Takt im BIOS-Datenbereich     */
    }
}

static unsigned bcd(unsigned v)
{
    return ((v / 10) << 4) | (v % 10);
}

static void out_ch(int t, unsigned char ch)
{
    cur_get();
    vtty(ch);
    cur_put(t);
    (void)t;
}

/* INT 13h: Laufwerk 0 ist das Startabbild (nur lesen), sonst nichts */
static void int13(struct v86_frame *f, int t)
{
    unsigned i, n, c, h, sct, lba, cyl;
    unsigned long lin;

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
        visr[t] &= ~2;
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
        } else if (AH == 0x88) {                /* erweiterter Speicher  */
            AX = 0;
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
            if (i == 0 && AL == 0xE0)
                AL = 0;                         /* graue Tasten wie AT-BIOS 00h */
        } else if (AH == 0x01 || AH == 0x11) {
            k = key(t, 0);
            svc_zf = k < 0;
            if (k >= 0) {
                AX = (unsigned)k;
                idle_n[t] = 0;
            } else if (++idle_n[t] >= 16) {     /* fragt nur ab: abgeben  */
                idle_n[t] = 0;                  /* (wie IDLE_SENSITIVITY  */
                kf_box_ctl(BOX_SLEEP, 1);       /* bei OS/2)              */
            }
        } else if (AH == 0x02 || AH == 0x12)
            AL = (unsigned char)kf_kbd_shift();
        else if (AH == 0x05)
            AL = 1;                             /* Puffer voll           */
        break;
    case 0x18: case 0x19:                       /* kein BASIC, kein Neustart */
    case 0x20:
        kf_box_ctl(BOX_EXIT, 0);
        break;
    case 0x1A:                                  /* Uhr                   */
        st = rtc_stamp();
        if (AH == 0x00) {
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
                k = key(t, 0);
                svc_zf = k < 0;
                AL = k < 0 ? 0 : (unsigned char)key(t, 1);
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
    case 0x33:                                  /* keine Maus (noch)     */
        AX = 0;
        break;
    case 0x28:                                  /* DOS wartet: abgeben   */
        kf_box_ctl(BOX_SLEEP, 1);
        break;
    case 0x67:                                  /* EMS                   */
        ems(f, t);
        break;
    case 0x29:                                  /* schnelle Ausgabe      */
        out_ch(t, AL);
        break;
    case 0x2F:
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
            a = (const char *)kf_box_ctl(BOX_ARGS, 0);
            for (i = 0; a[i] && i < 120; i++)
                wr8((unsigned)f->r.es, DI + i, a[i]);
            CX = i;
        } else if (AH == 0x01)
            kf_box_ctl(BOX_EXIT, AL);
        else if (AH == 0x10) {                  /* Redirector (BOXSHELL) */
            k = redir(f, t);
            svc_zf = k >= 0;                    /* ZF: erledigt          */
            if (k > 0) {
                AX = (unsigned)k;
                svc_cf = 1;
            } else if (k == 0)
                svc_cf = 0;
        } else if (AH == 0x13)                  /* Startlaufwerk (0 = A:) */
            AL = (unsigned char)kf_box_ctl(BOX_SYSDRV, 0);
        else if (AH == 0x17) {                  /* EMS-Grenze (BX KB, DOSBOX.INI) */
            unsigned pg = BX >> 4;
            ems_lim[t] = (pg > 256 ? 256 : pg) + 1;
        } else if (AH == 0x16) {                /* XMS-Grenze (BX KB, DOSBOX.INI) */
            unsigned pg = BX >> 2;
            xms_lim[t] = (pg > 0x400 - 0x110 ? 0x400 - 0x110 : pg) + 1;  /* bis 3008 KB */
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
        else if (AH == 0x11)                    /* SDA bei DS:SI         */
            redir_sda(t, (unsigned)f->r.ds, (unsigned)f->esi);
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

static void vga_ports(int t, int on)
{
    unsigned i;
    for (i = 0x3B0 >> 3; i <= 0x3DF >> 3; i++)
        tss[t].iob[i] = on ? 0 : 0xFF;
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
    vga_raw[t] = 0;
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

/* Hintergrund: Box mit der Karte steht, bis ihre Sitzung vorn ist und
   gfx_in alles zurueckgeladen hat (Interrupts aus, wie beim Aufruf) */
static void vga_freeze(int t)
{
    if (!vga_raw[t] || (kf_vio_fg() == box_ses[t] && hw_gfx == RAWGFX))
        return;
    if (spk[t])
        spk_hw(t, 0);
    kpit_set(0);
    enable_ints();
    while (vga_raw[t] && (kf_vio_fg() != box_ses[t] || hw_gfx != RAWGFX))
        kf_box_ctl(BOX_SLEEP, 2);
    irq_off();
}

/* INT 10h: 1 = an das Video-BIOS geben */
static int vga_want(struct v86_frame *f, int t)
{
    unsigned m = AL & 0x7F;
    int txt;
    txt = (AH == 0x00 && (m <= 3 || m == 7)) ||
              (AX == 0x4F02 && (BX & 0x1FF) < 0x100 && ((BX & 0x7F) <= 3 || (BX & 0x7F) == 7));
    if (vga_raw[t]) {
        if (!txt)
            return 1;
        vga_leave(t);                           /* Nachbildung macht 03h */
        if (AH == 0x4F) {
            AX = 0x0003;
            int10(f, t);
            AX = 0x004F;
            return 2;
        }
        return 0;
    }
    if (AH == 0x00 || AX == 0x4F02)
        return !txt && vga_enter(t) == 0;
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
static void do_int(struct v86_frame *f, int t, unsigned n, unsigned ip)
{
    if (n == 0x2F && AX == 0x1687) {            /* DPMI da?              */
        f->r.eip = ip;
        AX = 0;
        BX = 1;                                 /* 32-Bit-Programme gehen */
        CL = 3;
        DX = 0x005A;                            /* 0.90                  */
        *(unsigned short *)&f->esi = 0x0400 >> 4;     /* Absaetze fuer den Host (DP_RMSTK) */
        f->r.es = VBIOS;
        DI = 0x0510;
        return;
    }
    if (n == 0x2F && (AX == 0x4300 || AX == 0x4310)) {  /* XMS da?      */
        f->r.eip = ip;                          /* (vor dem DOS: FreeDOS */
        if (AL == 0)                            /* reicht 43h nicht weiter) */
            AL = 0x80;
        else {
            f->r.es = VBIOS;
            BX = XMS_STUB;
        }
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
static unsigned char port_in(int t, unsigned p)
{
    int k;
    switch (p) {
    case 0x20:
        return 0;
    case 0x21:
        return vimr[t];
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
        unsigned kb = 64 + ((xms_lim[t] ? xms_lim[t] - 1 : 512) << 2), f;
        unsigned char v;
        if (cmos_idx[t] == 0x17 || cmos_idx[t] == 0x30)
            return (unsigned char)kb;
        if (cmos_idx[t] == 0x18 || cmos_idx[t] == 0x31)
            return (unsigned char)(kb >> 8);
        f = irq_off();
        outb(0x70, cmos_idx[t]);
        v = (unsigned char)inb(0x71);
        irq_restore(f);
        return v;
    }
    case 0x3DA: case 0x3BA:
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
    }
    return 0xFF;
}

static void port_out(int t, unsigned p, unsigned char v)
{
    unsigned o, cur;
    switch (p) {
    case 0x20:
        if (v == 0x20)                          /* EOI: niedrigstes Bit  */
            visr[t] &= (unsigned char)(visr[t] - 1);
        else if ((v & 0xF8) == 0x60)            /* spezifisches EOI (Digger) */
            visr[t] &= (unsigned char)~(1 << (v & 7));
        break;
    case 0x21:
        vimr[t] = v;
        break;
    case 0x40: case 0x41: case 0x42: case 0x43:
        pit_out(t, p, v);
        break;
    case 0x70:
        cmos_idx[t] = (unsigned char)(v & 0x7F);
        break;
    case 0x61:
        spk[t] = (unsigned char)(v & 3);
        if (box_fg(t))
            spk_hw(t, 1);
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
   Box ab 110000h (eine Seitentabelle: bis XMS_KB), Lock liefert diese
   lineare Adresse (fuer DPMI spaeter), Move kopiert ueber zwei Fenster.
   Eigene Seiten tragen PTE_OWN und werden in v86_destroy frei. */
#define rd8_k(i) (*(unsigned char __far *)MK_FP(SCRATCH, (i)))
#define XMS_PG0  0x110                  /* erste EMB-Seite               */
#define XMS_KB   2048                   /* Vorgabe je Box (DOSBOX.INI: XMS_MEMORY_LIMIT) */
#define XMS_MAXPG (0x400 - XMS_PG0)      /* eine Seitentabelle: 3008 KB   */
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

static pg_t own_page(void)              /* genullte eigene Seite         */
{
    pg_t p = vp_alloc();
    if (p) {
        scratch_map(PG_LIN(p), 4095);
        zero_seg(SCRATCH, 2048);
    }
    return p;
}

static int a20_set(int t, int on)
{
    unsigned i;
    if (on && !hma_pg[t][0])
        for (i = 0; i < 16; i++)
            if (!(hma_pg[t][i] = own_page())) {
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
           (!ems_frame || p + n < ems_frame || p + n >= ems_frame + 16u))
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

static int emb_map(int t, unsigned base, unsigned n)
{
    unsigned i;
    pg_t p;
    for (i = 0; i < n; i++) {
        if (!(p = own_page())) {
            while (i--) {
                vp_free(pte_get(t, base + i) >> 12);
                pte_set(t, base + i, 0);
            }
            return 0;
        }
        pte_set(t, base + i, PG_LIN(p) | PTE_BOX | PTE_OWN);
    }
    return 1;
}

static void emb_unmap(int t, unsigned base, unsigned n)
{
    unsigned long e;
    while (n--) {
        e = pte_get(t, base + n);
        if (e & PTE_P)
            vp_free(e >> 12);
        pte_set(t, base + n, 0);
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
        kb = mem_free_kb() >> 2;                /* auch das System zaehlt */
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
            pg_t q = own_page();
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
            vp_free(pte_get(t, p) >> 12);
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

/* ---- EMS 4.0 fuer die Box (wie VEMM bei OS/2, EMM386 im Kernel) -----------
   INT 67h zeigt auf F060:0020 ("ARPL 67h; IRET"), bei F060:000A steht
   "EMMXXXX0" (so pruefen Programme, ob EMS da ist; ATBOX.SYS heisst
   ebenso). Seitenrahmen: die erste freie 64-KB-Luecke ab C8000 (auf 16 KB
   ausgerichtet), einmal fuer alle Boxen gewaehlt, vor den UMBs. Eine
   logische Seite = 4 zusammenhaengende Kernelseiten, eingeblendet wird
   ueber die Seitentabelle der Box; ein leerer Rahmenplatz zeigt auf die
   Ersatzseite. Verwaltung je Box in einer eigenen Seite (ems_tab):
   0 Erstseite je Platz (256 x pg_t), 1024 Besitzer (FFh frei), 1280
   Namen (32 x 8), 1536 gesicherte Zuordnung je Handle (32 x 8). */
#define EMS_SEG   0xF060
#define EMS_OFF   0x0020
#define NEH       32
#define EMS_MAXP  256                   /* 4 MB je Box                    */
#define EMS_KB    2048                  /* Vorgabe (DOSBOX.INI: EMS_MEMORY_LIMIT) */
static pg_t ems_tab[NTASK];
static unsigned char ems_on[NTASK][NEH], ems_np[NTASK][NEH];
static unsigned char ems_mh[NTASK][4], ems_ml[NTASK][4];   /* Rahmen: Handle/Seite */
#define EMS_PGN   (ems_lim[t] ? ems_lim[t] - 1 : EMS_KB / 16)
#define ET(o)     (*(unsigned char __far *)MK_FP(SCRATCH, (o)))

static void ems_tabmap(int t)
{
    scratch_map(PG_LIN(ems_tab[t]), 4095);
}

static void ems_pick_frame(void)        /* einmal, nach umb_scan          */
{
    unsigned p, i;
    for (p = 0xC8; p + 16 <= 0xF0; p += 4) {
        for (i = 0; i < 16 && BIT(umb_free, p + i); i++)
            ;
        if (i == 16) {
            ems_frame = (unsigned char)p;
            return;
        }
    }
}

/* Platz s (Kernelseiten base..base+3) in Rahmenplatz pp, s < 0: leer */
static void ems_show(int t, unsigned pp, int s)
{
    unsigned i;
    unsigned long e, spare = pte_get(t, 0xA0) & ~0xFFFUL;
    pg_t base = 0;
    if (s >= 0) {
        ems_tabmap(t);
        base = *(pg_t __far *)MK_FP(SCRATCH, s * 4);
    }
    for (i = 0; i < 4; i++) {
        e = s >= 0 ? PG_LIN(base + i) : spare;
        pte_set(t, ems_frame + pp * 4 + i, e | PTE_BOX);
    }
}

static int ems_slot(int t, unsigned h, unsigned k)   /* k-te Seite von h  */
{
    unsigned s;
    ems_tabmap(t);
    for (s = 0; s < EMS_MAXP; s++)
        if (ET(1024 + s) == h && k-- == 0)
            return (int)s;
    return -1;
}

static unsigned ems_used(int t)
{
    unsigned h, u = 0;
    for (h = 0; h < NEH; h++)
        u += ems_np[t][h];
    return u;
}

static unsigned ems_free(int t)
{
    unsigned u = ems_used(t), n = EMS_PGN;
    unsigned long sys = mem_free_kb() >> 4;
    n = u >= n ? 0 : n - u;
    return sys < n ? (unsigned)sys : n;
}

static int ems_grow(int t, unsigned h, unsigned n)   /* n Seiten anhaengen */
{
    unsigned s;
    pg_t p;
    while (n) {
        ems_tabmap(t);
        for (s = 0; s < EMS_MAXP && ET(1024 + s) != 0xFF; s++)
            ;
        if (s == EMS_MAXP || !(p = mem_alloc(4)))
            return 0;
        v86_pages += 4;
        ems_tabmap(t);
        *(pg_t __far *)MK_FP(SCRATCH, s * 4) = p;
        ET(1024 + s) = (unsigned char)h;
        ems_np[t][h]++;
        n--;
    }
    return 1;
}

static void ems_shrink(int t, unsigned h, unsigned n) /* auf n Seiten     */
{
    unsigned i;
    int s;
    pg_t p;
    for (i = 0; i < 4; i++)
        if (ems_mh[t][i] == h + 1 && ems_ml[t][i] >= n) {
            ems_show(t, i, -1);
            ems_mh[t][i] = 0;
        }
    while (ems_np[t][h] > n) {
        s = ems_slot(t, h, ems_np[t][h] - 1);
        ems_tabmap(t);
        p = *(pg_t __far *)MK_FP(SCRATCH, s * 4);
        ET(1024 + s) = 0xFF;
        mem_free(p, 4);
        v86_pages -= 4;
        ems_np[t][h]--;
    }
}

static void ems_box(int t)              /* aus v86_create                 */
{
    unsigned i;
    if (!umb_ok)
        umb_scan();
    for (i = 0; i < NEH; i++)
        ems_on[t][i] = ems_np[t][i] = 0;
    ems_on[t][0] = 1;                   /* Handle 0 gehoert dem System    */
    ems_lim[t] = 0;
    ems_tab[t] = 0;
    for (i = 0; i < 4; i++) {
        ems_mh[t][i] = 0;
        if (ems_frame)
            ems_show(t, i, -1);
    }
}

static void ems_end(int t)              /* aus v86_destroy                */
{
    unsigned h;
    if (ems_tab[t]) {
        for (h = 0; h < NEH; h++)
            if (ems_np[t][h])
                ems_shrink(t, h, 0);
        vp_free(ems_tab[t]);
        ems_tab[t] = 0;
    }
}

static int ems_newh(int t)
{
    unsigned h, i;
    if (!ems_tab[t]) {
        if (!(ems_tab[t] = own_page()))
            return -1;
        ems_tabmap(t);
        for (i = 1024; i < 1280; i++)
            ET(i) = 0xFF;
    }
    for (h = 1; h < NEH && ems_on[t][h]; h++)
        ;
    if (h == NEH)
        return -1;
    ems_tabmap(t);
    for (i = 0; i < 8; i++)
        ET(1280 + h * 8 + i) = 0;
    return (int)h;
}

/* 57h: eine Seite des Bereichs -> Fenster. typ 0 = konventionell
   (seg:off, lineare Adresse der Box), 1 = EMS (Handle, logische Seite +
   Versatz, ueber die physischen Seiten). -> Bytes bis zur Grenze, 0 = Fehler */
static unsigned x57_win(int t, unsigned char typ, unsigned h, unsigned off, unsigned sp,
                        unsigned long pos, unsigned widx, unsigned sel)
{
    unsigned long a;
    unsigned lp, o, n;
    int sl;
    if (typ == 0) {
        a = ((unsigned long)sp << 4) + off + pos;
        if (a >= 0x110000UL)
            return 0;
        lwin(widx, a);
        return 0x8000;
    }
    a = (unsigned long)off + pos;               /* ab Anfang der Seite sp */
    lp = sp + (unsigned)(a >> 14);
    o = (unsigned)a & 0x3FFF;
    if (h >= NEH || !ems_on[t][h] || lp >= ems_np[t][h] || (sl = ems_slot(t, h, lp)) < 0)
        return 0;
    ems_tabmap(t);
    a = PG_LIN(*(pg_t __far *)MK_FP(SCRATCH, sl * 4)) + o;
    n = 0x4000 - o;
    if (sel == SCRATCH)
        scratch_map(a, n - 1);
    else
        scratch2_map(a, n - 1);
    return n;
}

#define ERET(c)  do { AH = (c); return; } while (0)

static void ems(struct v86_frame *f, int t)
{
    unsigned h = DX, n, i, k, es = (unsigned)f->r.es, di = DI;
    unsigned ds = (unsigned)f->r.ds, si = (unsigned)f->esi;
    int s;

    if (!ems_frame)
        ERET(0x80);
    switch (AH) {                               /* mit Handle in DX      */
    case 0x44: case 0x45: case 0x47: case 0x48: case 0x4C:
    case 0x50: case 0x51: case 0x53:
        if (h >= NEH || !ems_on[t][h])
            ERET(0x83);
    }
    switch (AH) {
    case 0x40:                                  /* Zustand               */
        ERET(0);
    case 0x41:                                  /* Seitenrahmen          */
        BX = (unsigned)ems_frame << 8;
        ERET(0);
    case 0x42:                                  /* freie/alle Seiten     */
        BX = ems_free(t);
        DX = EMS_PGN;
        ERET(0);
    case 0x43: case 0x5A:                       /* anlegen (5A: auch 0)  */
        n = BX;
        if (AH == 0x43 && n == 0)
            ERET(0x89);
        if (n > EMS_PGN)
            ERET(0x87);
        if (n > ems_free(t))
            ERET(0x88);
        if ((s = ems_newh(t)) < 0)
            ERET(0x85);
        ems_on[t][s] = 1;
        if (!ems_grow(t, (unsigned)s, n)) {
            ems_shrink(t, (unsigned)s, 0);
            ems_on[t][s] = 0;
            ERET(0x88);
        }
        DX = (unsigned)s;
        ERET(0);
    case 0x44:                                  /* einblenden            */
        if (AL > 3)
            ERET(0x8B);
        if (BX == 0xFFFF) {
            ems_show(t, AL, -1);
            ems_mh[t][AL] = 0;
        } else {
            if (BX >= ems_np[t][h])
                ERET(0x8A);
            ems_show(t, AL, ems_slot(t, h, BX));
            ems_mh[t][AL] = (unsigned char)(h + 1);
            ems_ml[t][AL] = (unsigned char)BX;
        }
        tlb(t);
        ERET(0);
    case 0x45:                                  /* freigeben             */
        ems_shrink(t, h, 0);
        tlb(t);
        if (h)
            ems_on[t][h] = 0;
        ERET(0);
    case 0x46:                                  /* Version 4.0           */
        AL = 0x40;
        ERET(0);
    case 0x47: case 0x48:                       /* Zuordnung sichern/zurueck */
        ems_tabmap(t);
        for (i = 0; i < 4; i++)
            if (AH == 0x47) {
                ET(1536 + h * 8 + i * 2) = ems_mh[t][i];
                ET(1536 + h * 8 + i * 2 + 1) = ems_ml[t][i];
            } else {
                ems_mh[t][i] = ET(1536 + h * 8 + i * 2);
                ems_ml[t][i] = ET(1536 + h * 8 + i * 2 + 1);
            }
        if (AH == 0x48) {
            for (i = 0; i < 4; i++)
                ems_show(t, i, ems_mh[t][i] ? ems_slot(t, ems_mh[t][i] - 1, ems_ml[t][i]) : -1);
            tlb(t);
        }
        ERET(0);
    case 0x4B:                                  /* aktive Handles        */
        for (i = 0, n = 0; i < NEH; i++)
            n += ems_on[t][i];
        BX = n;
        ERET(0);
    case 0x4C:                                  /* Seiten eines Handles  */
        BX = ems_np[t][h];
        ERET(0);
    case 0x4D:                                  /* alle Handles -> ES:DI */
        for (i = 0, n = 0; i < NEH; i++)
            if (ems_on[t][i]) {
                wr16(es, di + n * 4, i);
                wr16(es, di + n * 4 + 2, ems_np[t][i]);
                n++;
            }
        BX = n;
        ERET(0);
    case 0x4E:                                  /* ganze Zuordnung (8 Byte) */
        if (AL == 3) {
            AL = 8;
            ERET(0);
        }
        if (AL > 3)
            ERET(0x8F);
        if (AL == 0 || AL == 2)
            for (i = 0; i < 4; i++) {
                wr8(es, di + i * 2, ems_mh[t][i]);
                wr8(es, di + i * 2 + 1, ems_ml[t][i]);
            }
        if (AL == 1 || AL == 2) {
            for (i = 0; i < 4; i++) {
                ems_mh[t][i] = rd8(ds, si + i * 2);
                ems_ml[t][i] = rd8(ds, si + i * 2 + 1);
                k = ems_mh[t][i];
                ems_show(t, i, k && k <= NEH && ems_on[t][k - 1] && ems_ml[t][i] < ems_np[t][k - 1]
                               ? ems_slot(t, k - 1, ems_ml[t][i]) : -1);
            }
            tlb(t);
        }
        ERET(0);
    case 0x50:                                  /* mehrere einblenden    */
        if (AL > 1)
            ERET(0x8F);
        for (i = 0; i < CX; i++) {
            unsigned lg = rd16(ds, si + i * 4), pp = rd16(ds, si + i * 4 + 2);
            if (AL == 1)                        /* Segment statt Nummer  */
                pp = (pp - ((unsigned)ems_frame << 8)) >> 10;
            if (pp > 3)
                ERET(0x8B);
            if (lg == 0xFFFF) {
                ems_show(t, pp, -1);
                ems_mh[t][pp] = 0;
            } else {
                if (lg >= ems_np[t][h])
                    ERET(0x8A);
                ems_show(t, pp, ems_slot(t, h, lg));
                ems_mh[t][pp] = (unsigned char)(h + 1);
                ems_ml[t][pp] = (unsigned char)lg;
            }
        }
        tlb(t);
        ERET(0);
    case 0x51:                                  /* Groesse aendern       */
        n = BX;
        if (n > ems_np[t][h]) {
            if (n - ems_np[t][h] > ems_free(t))
                ERET(0x88);
            if (!ems_grow(t, h, n - ems_np[t][h]))
                ERET(0x88);
        } else
            ems_shrink(t, h, n);
        tlb(t);
        BX = ems_np[t][h];
        ERET(0);
    case 0x53:                                  /* Name holen/setzen     */
        if (!ems_tab[t] && ems_newh(t) < 0)
            ERET(0x80);
        if (AL > 1)
            ERET(0x8F);
        for (i = 0; i < 8; i++) {
            ems_tabmap(t);
            if (AL == 0)
                wr8(es, di + i, ET(1280 + h * 8 + i));
            else {
                k = rd8(ds, si + i);
                ems_tabmap(t);
                ET(1280 + h * 8 + i) = (unsigned char)k;
            }
        }
        ERET(0);
    case 0x54:                                  /* Handle-Verzeichnis    */
        if (AL == 2) {
            BX = NEH;
            ERET(0);
        }
        ERET(AL > 2 ? 0x8F : 0xA0);
    case 0x57: {                                /* Bereich kopieren/tauschen */
        static unsigned char xb[512];
        unsigned long len, pos = 0;
        unsigned char st = rd8(ds, si + 4), dt = rd8(ds, si + 11);
        unsigned sh = rd16(ds, si + 5), so = rd16(ds, si + 7), ssp = rd16(ds, si + 9);
        unsigned dh = rd16(ds, si + 12), dof = rd16(ds, si + 14), dsp = rd16(ds, si + 16);
        unsigned a, b, ss, dsel;
        len = ((unsigned long)rd16(ds, si + 2) << 16) | rd16(ds, si);
        if (AL > 1 || st > 1 || dt > 1)
            ERET(0x8F);
        if (len > 0x100000UL)
            ERET(0x96);
        while (pos < len) {
            ss = st ? SCRATCH : VWIN;
            dsel = dt ? SCRATCH2 : RWIN_SEL(t);
            b = x57_win(t, dt, dh, dof, dsp, pos, RWIN_IDX(t), dsel);  /* Ziel zuerst: */
            a = x57_win(t, st, sh, so, ssp, pos, VWIN_IDX, ss);  /* SCRATCH zuletzt */
            if (!a || !b)
                ERET(0x8A);
            n = a < b ? a : b;
            if (AL == 1 && n > sizeof(xb))
                n = sizeof(xb);
            if (len - pos < n)
                n = (unsigned)(len - pos);
            if (AL == 0)
                far_copy(dsel, 0, ss, 0, n);
            else {                              /* tauschen ueber xb     */
                far_copy(KDATA, (unsigned)xb, dsel, 0, n);
                far_copy(dsel, 0, ss, 0, n);
                far_copy(ss, 0, KDATA, (unsigned)xb, n);
            }
            pos += n;
        }
        ERET(0);
    }
    case 0x58:                                  /* Rahmenadressen        */
        if (AL == 0)
            for (i = 0; i < 4; i++) {
                wr16(es, di + i * 4, ((unsigned)ems_frame << 8) + (i << 10));
                wr16(es, di + i * 4 + 2, i);
            }
        else if (AL != 1)
            ERET(0x8F);
        CX = 4;
        ERET(0);
    case 0x59:                                  /* Hardware: nur Seiten  */
        if (AL != 1)
            ERET(AL ? 0x8F : 0xA4);
        BX = ems_free(t);
        DX = EMS_PGN;
        ERET(0);
    }
    ERET(0x84);                                 /* unbekannt             */
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

static unsigned char dp_on[NTASK], dp_32[NTASK], dp_dep[NTASK];
static unsigned dp_exss[NTASK];         /* Selektor des Ausnahme-Stapels  */
static unsigned dp_cbss[NTASK];         /* Selektor fuer den RM-Stapel (Callback) */
struct dpcb { unsigned short psel, ssel; unsigned long poff, soff; };
#define NCB       16
#define DP_CB     0x0700                /* Callback-Einspruenge (4 Byte) */
#define DP_CBRET  0x0548                /* Ruecksprung aus einem Callback */
#define CB(t, k)  ((struct dpcb __far *)MK_FP(SCRATCH, 1728 + (k) * 12))
#define DP_EXC    0x0600                /* Standard-Ausnahme-Handler (4 Byte) */
#define DP_EXRET  0x0540                /* Ruecksprung aus einem Handler  */
#define EXV(t, n) ((unsigned long __far *)MK_FP(SCRATCH, 1536 + (n) * 6))
static unsigned dp_rmseg[NTASK], dp_stub[NTASK], dp_psp[NTASK];
static pg_t dp_ldt[NTASK], dp_st[NTASK];        /* LDT-Seite, Zustandsseite */
static struct desc dp_oldldt[NTASK];
static unsigned char dp_lmap[NTASK][LDT_N / 8];
static struct { unsigned long lpg, npg; } dp_mem[NTASK][NDPM];

/* Zustandsseite: 0 PM-Vektoren (256 x 6), 1536 Ausnahme-Handler (32 x 6),
   2048 Rahmen (DPCT x 64) */
struct dpctx {
    unsigned short type, cs, ss, ds, es, fs, gs, rsv;
    unsigned long eip, eflags, esp, lin, r[8];  /* r: EDI..EAX       */
};

static unsigned long lrd(unsigned long lin, unsigned n)   /* 1/2/4 Byte */
{
    unsigned long v = 0;
    lwin(VWIN_IDX, lin);
    while (n--)
        v = (v << 8) | *(unsigned char __far *)MK_FP(VWIN, n);
    return v;
}

static void lwr(unsigned long lin, unsigned long v, unsigned n)
{
    unsigned i;
    lwin(VWIN_IDX, lin);
    for (i = 0; i < n; i++, v >>= 8)
        *(unsigned char __far *)MK_FP(VWIN, i) = (unsigned char)v;
}

static unsigned char __far *ldt_e(int t, unsigned i)      /* 8 Byte  */
{
    scratch_map(PG_LIN(dp_ldt[t]), 4095);
    return (unsigned char __far *)MK_FP(SCRATCH, i * 8);
}

static void ldesc(int t, unsigned i, unsigned long base, unsigned long lim,
                  unsigned char acc, unsigned char fl)
{
    unsigned char __far *d = ldt_e(t, i);
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
    if (!(s & 4) || IDX(s) >= LDT_N)
        return 0;
    d = ldt_e(t, IDX(s));
    return d[2] | ((unsigned long)d[3] << 8) | ((unsigned long)d[4] << 16) |
           ((unsigned long)d[7] << 24);
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
        if (e & PTE_P) {
            vp_free(e >> 12);
            lpte_set(t, lp + n, 0);
        }
    }
}

static int dmem_map(int t, unsigned long lp, unsigned long n)
{
    unsigned long i;
    pg_t p;
    for (i = 0; i < n; i++)
        if (!(p = own_page()) || !lpte_set(t, lp + i, PG_LIN(p) | PTE_BOX | PTE_OWN)) {
            if (p)
                vp_free(p);
            dmem_unmap(t, lp, i);
            return 0;
        }
    return 1;
}

/* Luecke fuer n Seiten ab 4 MB (Seite 400h), bis 1 GB */
static unsigned long dmem_gap(int t, unsigned long n)
{
    unsigned long s = 0x400;
    unsigned i;
again:
    for (i = 0; i < NDPM; i++)
        if (dp_mem[t][i].npg && s < dp_mem[t][i].lpg + dp_mem[t][i].npg &&
            s + n > dp_mem[t][i].lpg) {
            s = dp_mem[t][i].lpg + dp_mem[t][i].npg;
            goto again;
        }
    return s + n <= 0x40000UL ? s : 0;
}

static void dpmi_end(int t)             /* Programm fertig / Box zu      */
{
    unsigned i;
    dp_inirq[t] = 0;
    if (!dp_on[t])
        return;
    for (i = 0; i < NDPM; i++)
        if (dp_mem[t][i].npg) {
            dmem_unmap(t, dp_mem[t][i].lpg, dp_mem[t][i].npg);
            dp_mem[t][i].npg = 0;
        }
    gdt[LDT_IDX(t)] = dp_oldldt[t];     /* wieder die Task-LDT          */
    if (kf_box_ctl(BOX_TASK, 0) == t)
        k2_lldt(LDT_SEL(t));
    vp_free(dp_ldt[t]);
    vp_free(dp_st[t]);
    dp_ldt[t] = dp_st[t] = 0;
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
    scratch_map(pte_get(t, 0xF0) & ~0xFFFUL, 4095);
    for (i = 0; i < 32; i++)                    /* Standard: HLT -> Ende  */
        put32(SCRATCH, (DP_EXC >> 2) + i, 0x909090F4UL);
    put32(SCRATCH, DP_EXRET >> 2, 0x909090F4UL);
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
        unsigned long fr = mem_free_kb() >> 2;
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
        if (n > mem_free_kb() >> 2 || !(lin = dmem_gap(t, n)) || !dmem_map(t, lin, n))
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
static void dp_hwint(struct v86_iret *r, int t)
{
    if (!dp_on[t] || (r->cs & 3) != 3)
        return;
    account(t);
    if (!(vfl[t] & FL_IF))
        return;
    if (vt_pend[t] && !(visr[t] & 1) && !(vimr[t] & 1) && pm_hooked(t, 8) && t8_ok(t)) {
        vt_pend[t]--;
        visr[t] |= 1;
        dp_push(r, t, 8);
        return;
    }
    if (pm_hooked(t, 9) && key_ready(t)) {
        key_t0[t] = get_ticks();
        vcur[t] = (unsigned char)kq_get(t);
        visr[t] |= 2;
        dp_push(r, t, 9);
    }
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

static unsigned dp_trap(struct v86_frame *f, int t)
{
    unsigned long lin;
    unsigned char op;
    unsigned n, cs = (unsigned)f->r.cs;

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
        vfl[t] &= ~FL_IF;
        f->r.eip++;
        return 0;
    case 0xFB:
        vfl[t] |= FL_IF;
        f->r.eip++;
        return 0;
    case 0xF4:                                  /* HLT: abgeben          */
        f->r.eip++;
        hlt_wait(t);
        return 0;
    case 0xE4: case 0xE5: case 0xEC: case 0xED: /* IN                    */
        n = op <= 0xE5 ? (unsigned)lrd(lin + 1, 1) : DX;
        if (op & 1)
            AX = port_in(t, n) | (port_in(t, n + 1) << 8);
        else
            AL = port_in(t, n);
        f->r.eip += op <= 0xE5 ? 2 : 1;
        return 0;
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: /* OUT                   */
        n = op <= 0xE7 ? (unsigned)lrd(lin + 1, 1) : DX;
        port_out(t, n, AL);
        if (op & 1)
            port_out(t, n + 1, AH);
        f->r.eip += op <= 0xE7 ? 2 : 1;
        return 0;
    }
    return dp_exc(f, t);
}

unsigned __far dpmi_trap(struct v86_frame *f)
{
    int t = kf_box_ctl(BOX_TASK, 0);
    unsigned rc;
    if (t < 0 || !dp_on[t])
        return 1;
    enable_ints();
    rc = dp_trap(f, t);
    irq_off();
    vga_freeze(t);
    if (rc == 0)
        dp_hwint(&f->r, t);
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
    pg = vp_alloc();
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
            (ems_frame && p >= ems_frame && p < ems_frame + 16)))
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
        return rom_cow(f, t) ? 0 : 1;
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
    case 0xFA:                                  /* CLI                   */
        vfl[t] &= ~FL_IF;
        break;
    case 0xFB:                                  /* STI                   */
        vfl[t] |= FL_IF;
        break;
    case 0x9C:                                  /* PUSHF(D)              */
        if (o32)
            push16(&f->r, (unsigned)(f->r.eflags >> 16) & ~3);  /* ohne VM/RF */
        push16(&f->r, vflags(&f->r, t));
        break;
    case 0x9D:                                  /* POPF(D)               */
        v = pop16(&f->r);
        if (o32)
            pop16(&f->r);
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
        if (op & 1) {
            AX = port_in(t, p) | (port_in(t, p + 1) << 8);
            if (o32)
                f->eax = AX | 0xFFFF0000UL;
        } else
            AL = port_in(t, p);
        break;
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: /* OUT                   */
        p = op <= 0xE7 ? rd8(cs, ip++) : DX;
        port_out(t, p, AL);
        if (op & 1)
            port_out(t, p + 1, AH);
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

unsigned __far v86_exc(struct v86_frame *f)
{
    int t = kf_box_ctl(BOX_TASK, 0);
    unsigned rc;

    enable_ints();                      /* Kernel laeuft mit Interrupts  */
    rc = monitor(f, t);
    if (rc == 0) {
        irq_off();                      /* bis zum IRETD                 */
        inject(&f->r, t);
    } else if (rc == 2)
        irq_off();                      /* DPMI: weiter im PM            */
    from_tick = 0;
    return rc;
}
