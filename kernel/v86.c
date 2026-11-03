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
static unsigned pit_c[NTASK];
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
static void ems_end(int t);
static void ems_box(int t);
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
    if (box_ses[t] >= 0 && pt_box[t])
        capture(t);
    box_ses[t] = -1;
    /* laeuft gerade das CR3 der Box (sie beendet sich selbst)? Dann auf
       das des Kernels - der Kernel lebt ganz im Alias */
    if (k2_get_cr3() == PG_LIN(pd_box[t]))
        k2_cr3((unsigned)paging_cr3(), (unsigned)(paging_cr3() >> 16));
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
        if (i >= 512) {
            scratch_map(kcr3, 4095);
            e = get32(SCRATCH, i);
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

    for (t = 0; t < NTASK; t++) {
        if (box_ses[t] < 0 || !pt_box[t])
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

/* 100 Hz des Kernels in 18,2 Hz der Box umrechnen; ohne eigene Takt-
   Routine nur den Zaehler im BIOS-Datenbereich weiterzaehlen */
static void account(int t)
{
    unsigned long now = get_ticks(), d = now - vt_last[t];
    vt_last[t] = now;
    if (d > 100)
        d = 100;
    vt_acc[t] += (unsigned)d * 182;
    while (vt_acc[t] >= 1000) {
        vt_acc[t] -= 1000;
        vt_pend[t]++;
    }
    if (vt_pend[t] && vec_def(8) && vec_def(0x1C)) {
        bda_ticks(vt_pend[t]);
        vt_pend[t] = 0;
    } else if (vt_pend[t] > 4)
        vt_pend[t] = 4;                 /* lange gesperrt: nicht nachholen */
}

/* beim Ruecksprung in die Box: ausstehenden Interrupt zustellen */
static void inject(struct v86_iret *r, int t)
{
    account(t);
    if (!(vfl[t] & FL_IF))
        return;
    if (vt_pend[t] && !(visr[t] & 1) && !(vimr[t] & 1)) {
        vt_pend[t]--;
        visr[t] |= 1;
        int_raw(r, t, 8, (unsigned)r->eip);
        return;
    }
    if (kq_h[t] != kq_t[t] && !vec_def(9) && !(visr[t] & 2) && !(vimr[t] & 2)) {
        vcur[t] = (unsigned char)kq_get(t);
        visr[t] |= 2;
        int_raw(r, t, 9, (unsigned)r->eip);
    }
}

/* aus irq0, wenn er die Box unterbrochen hat (nach preempt) */
void __far v86_tick(struct v86_iret *r)
{
    int t = kf_box_ctl(BOX_TASK, 0);
    if (t >= 0 && pt_box[t])
        inject(r, t);
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
            const char *a = (const char *)kf_box_ctl(BOX_ARGS, 0);
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
        } else if (AH == 0x15)                  /* Programm fertig (AL):  */
            kf_box_ctl(BOX_PARK, AL);           /* parken oder beenden    */
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

/* INT n wie im Real Mode, ip = Ruecksprung */
static void do_int(struct v86_frame *f, int t, unsigned n, unsigned ip)
{
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
        if (vec_def(9) && (k = kq_get(t)) >= 0)
            vcur[t] = (unsigned char)k;         /* ohne INT 9: abfragen  */
        return vcur[t];
    case 0x64:
        return (unsigned char)(0x14 | (kq_h[t] != kq_t[t]));
    case 0x61:
        tgl[t] ^= 0x10;                         /* Refresh-Bit kippt     */
        return (unsigned char)(0x20 | (tgl[t] & 0x10));
    case 0x40: case 0x41: case 0x42:
        if (!pit_ff[t]++)
            pit_c[t] -= 0x1F3;
        pit_ff[t] &= 1;
        return (unsigned char)(pit_ff[t] ? pit_c[t] : pit_c[t] >> 8);
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
        break;
    case 0x21:
        vimr[t] = v;
        break;
    case 0x43:
        pit_ff[t] = 0;
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
    d->rsv = 0;
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
        if (cs == VBIOS && ip == XMS_STUB) {     /* Fernaufruf: kein FLAGS */
            xms(f, t);
            return 0;
        }
        /* FLAGS des INT liegen auf dem Stack der Box: dort setzen */
        wr16((unsigned)f->r.ss, (unsigned)f->r.esp + 4,
             run_svc(v, f, t, rd16((unsigned)f->r.ss, (unsigned)f->r.esp + 4)));
        return 0;
    case 13:
        break;
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
        kf_box_ctl(BOX_SLEEP, 1);
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
    }
    return rc;
}
