/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - DOS-Boxen (V86), Phase 3 Stufe 1: Grundlage
 *
 * Eine Box ist ein Task, dessen "Ring 3" der V86-Modus des 386 ist. Sie
 * hat ein eigenes Seitenverzeichnis (CR3 im 386-TSS): linear 0-4 MB ist
 * ihr eigenes erstes MB, ab 80000000h liegt wie ueberall der Kernel-Alias
 * (paging.c). Seitentabelle der Box:
 *   0 .. kb/4-1     eigene Seiten (konventioneller Speicher)
 *   A0000-BFFFF     nicht vorhanden (Bildschirm kommt mit Stufe 2)
 *   C0000-EFFFF     ROM 1:1, nur lesen
 *   F0000-F0FFF     eigene Seite: "BIOS" der Box (Dienst-Stummel)
 *   F1000-FFFFF     ROM 1:1, nur lesen
 *   100000-10FFFF   wie 0-FFFF (A20 aus: Umlauf wie beim echten 8086)
 *
 * Monitor: Die Box laeuft mit IOPL 0. CLI, STI, PUSHF, POPF, INT n, IRET,
 * HLT und jeder Portzugriff (kein I/O-Bitmap) loesen #GP aus; v86_exc
 * fuehrt sie nach (virtuelles IF in vfl). INT n springt wie im Real Mode
 * ueber die Vektortabelle der Box. Anfangs zeigt jeder Vektor n auf
 * F000:n*4, dort steht "ARPL n; IRET": ARPL gibt es im V86-Modus nicht
 * (#UD), der Monitor fuehrt Dienst n aus (wie die BIOS-Haken bei
 * Windows 386). Zeigt ein Vektor noch dorthin, wird der Dienst bei INT n
 * gleich ausgefuehrt, ohne Umweg ueber den Stack der Box.
 *
 * Stufe 1 bietet nur wenige Dienste, ueber die Sitzung des Tasks:
 * INT 10h 0Eh/0Fh, INT 16h 00h/01h/02h/10h/11h, INT 20h, INT 21h
 * 01h/02h/06h/07h/08h/09h/0Bh/25h/30h/35h/40h/4Ch.
 *
 * Laeuft im zweiten Codesegment (KTEXT2), im Kontext des Box-Tasks: dann
 * ist das CR3 der Box geladen und ihr Speicher linear 0-10FFEFh ueber das
 * rohe Fenster VWIN erreichbar.
 */
#include "k2.h"

#define PTE_P   1
#define PTE_W   2
#define PTE_U   4
#define PTE_BOX (PTE_P | PTE_W | PTE_U)
#define PTE_ROM (PTE_P | PTE_U)

#define FL_TF   0x0100
#define FL_IF   0x0200
#define FL_V    0x7200          /* IF, IOPL, NT: gehoeren der Box (vfl)  */
#define FL_SET  0x0DD5          /* CF PF AF ZF SF TF DF OF: echt         */

#define VBIOS   0xF000          /* Segment der Dienst-Stummel            */

struct v86_frame {
    unsigned short es, ds;              /* vom Handler (genullt)         */
    unsigned long  edi, esi, ebp, esp_k, ebx, edx, ecx, eax;   /* PUSHAD */
    unsigned short vec;
    unsigned long  err, eip, cs, eflags, esp, ss, ves, vds, vfs, vgs;
};

static pg_t pd_box[NTASK], pt_box[NTASK];
static unsigned short vfl[NTASK];       /* IF/IOPL/NT der Box            */

/* Thunks (erstes Codesegment) */
int  __far kf_box_ctl(unsigned op, unsigned a);
int  __far kf_vio_fg(void);
int  __far kf_kbd_get(void);
int  __far kf_kbd_peek(void);
unsigned __far kf_kbd_shift(void);
void __far kf_vio_tty(int s, unsigned sel, unsigned off, unsigned len);

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

void __far v86_destroy(int t)
{
    unsigned i;
    unsigned long e;

    if (!pd_box[t])
        return;
    /* laeuft gerade das CR3 der Box (sie beendet sich selbst)? Dann auf
       das des Kernels - der Kernel lebt ganz im Alias */
    if (k2_get_cr3() == PG_LIN(pd_box[t]))
        k2_cr3((unsigned)paging_cr3(), (unsigned)(paging_cr3() >> 16));
    if (pt_box[t]) {
        scratch_map(PG_LIN(pt_box[t]), 4095);
        for (i = 0; i < 0x100; i++) {
            e = get32(SCRATCH, i);
            if ((e & PTE_P) && (i < 0xA0 || i == 0xF0))
                mem_free(e >> 12, 1);
        }
        mem_free(pt_box[t], 1);
        pt_box[t] = 0;
    }
    mem_free(pd_box[t], 1);
    pd_box[t] = 0;
}

/* -> CR3 der neuen Box, 0 = kein Speicher. Speicher genullt. */
unsigned long __far v86_create(int t, unsigned kb)
{
    unsigned long kcr3 = paging_cr3(), e;
    unsigned i, n = kb >> 2;
    pg_t p;

    if (!kcr3 || pd_box[t] || n == 0 || n > 0xA0)
        return 0;
    pd_box[t] = mem_alloc(1);
    pt_box[t] = mem_alloc(1);
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
    for (i = 0; i <= n; i++) {                  /* n: dann die BIOS-Seite */
        p = mem_alloc(1);
        if (!p) {
            v86_destroy(t);
            return 0;
        }
        scratch_map(PG_LIN(p), 4095);
        zero_seg(SCRATCH, 2048);
        scratch2_map(PG_LIN(pt_box[t]), 4095);
        if (i == n)
            put32(SCRATCH2, 0xF0, PG_LIN(p) | PTE_ROM);
        else
            put32(SCRATCH2, i, PG_LIN(p) | PTE_BOX);
        if (i < 0x10 && i < n)                  /* HMA: Umlauf           */
            put32(SCRATCH2, 0x100 + i, PG_LIN(p) | PTE_BOX);
    }
    vfl[t] = FL_IF;

    /* Vektortabelle (Seite 0) -> F000:n*4 */
    scratch2_map(PG_LIN(pt_box[t]), 4095);
    scratch_map(get32(SCRATCH2, 0) & ~0xFFFUL, 4095);
    for (i = 0; i < 256; i++)
        put32(SCRATCH, i, ((unsigned long)VBIOS << 16) | (i << 2));
    /* BIOS-Datenbereich: Speichergroesse, 80 Spalten, 25 Zeilen */
    *(unsigned __far *)MK_FP(SCRATCH, 0x413) = kb;
    *(unsigned char __far *)MK_FP(SCRATCH, 0x449) = 3;
    *(unsigned __far *)MK_FP(SCRATCH, 0x44A) = 80;
    *(unsigned char __far *)MK_FP(SCRATCH, 0x484) = 24;
    /* Stummel bei F000:0000: je Vektor "ARPL n; IRET; NOP" */
    scratch_map(get32(SCRATCH2, 0xF0) & ~0xFFFUL, 4095);
    for (i = 0; i < 256; i++)
        put32(SCRATCH, i, 0x90CF0063UL | ((unsigned long)i << 8));
    return PG_LIN(pd_box[t]);
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

static void push16(struct v86_frame *f, unsigned v)
{
    unsigned sp = (unsigned)f->esp - 2;
    wr16((unsigned)f->ss, sp, v);
    f->esp = (f->esp & 0xFFFF0000UL) | sp;
}

static unsigned pop16(struct v86_frame *f)
{
    unsigned sp = (unsigned)f->esp, v = rd16((unsigned)f->ss, sp);
    f->esp = (f->esp & 0xFFFF0000UL) | (unsigned)(sp + 2);
    return v;
}

/* FLAGS, wie die Box sie sieht */
static unsigned vflags(struct v86_frame *f, int t)
{
    return ((unsigned)f->eflags & ~FL_V) | vfl[t] | 0x0002;
}

/* POPF/IRET: Rechenflags echt, IF/IOPL/NT nur virtuell */
static void set_flags(struct v86_frame *f, int t, unsigned v)
{
    vfl[t] = v & FL_V;
    f->eflags = (f->eflags & ~(unsigned long)FL_SET) | (v & FL_SET);
}

static void set_ip(struct v86_frame *f, unsigned ip)
{
    f->eip = ip;
}

/* ---- Dienste ------------------------------------------------------------ */

#define AL  (*(unsigned char *)&f->eax)
#define AH  (*((unsigned char *)&f->eax + 1))
#define AX  (*(unsigned short *)&f->eax)
#define DL  (*(unsigned char *)&f->edx)
#define DX  (*(unsigned short *)&f->edx)
#define BX  (*(unsigned short *)&f->ebx)
#define CX  (*(unsigned short *)&f->ecx)

static unsigned char svc_cf, svc_zf;    /* -1 = unveraendert            */

static void tty_ch(int s, char c)
{
    static char ch[1];
    ch[0] = c;
    kf_vio_tty(s, KDATA, (unsigned)ch, 1);
}

/* Taste: warten (wait = 1) oder nachsehen; -> Zeichen | Scancode << 8,
   -1 = keine */
static int key(int wait)
{
    int s = kf_box_ctl(BOX_SESSION, 0), k;
    for (;;) {
        if (kf_vio_fg() == s) {
            k = wait ? kf_kbd_get() : kf_kbd_peek();
            if (k >= 0)
                return k;
        }
        if (!wait)
            return -1;
        kf_box_ctl(BOX_SLEEP, 1);
    }
}

static void svc(int n, struct v86_frame *f, int t)
{
    int s = kf_box_ctl(BOX_SESSION, 0), k;
    unsigned i, off;

    switch (n) {
    case 0x10:                                  /* Bildschirm           */
        if (AH == 0x0E)
            tty_ch(s, AL);
        else if (AH == 0x0F) {
            AX = 0x5003;                        /* 80 Spalten, Modus 3  */
            BX &= 0x00FF;
        }
        break;
    case 0x16:                                  /* Tastatur             */
        if (AH == 0x00 || AH == 0x10)
            AX = (unsigned)key(1);
        else if (AH == 0x01 || AH == 0x11) {
            k = key(0);
            svc_zf = k < 0;
            if (k >= 0)
                AX = (unsigned)k;
        } else if (AH == 0x02)
            AL = (unsigned char)kf_kbd_shift();
        break;
    case 0x20:
        kf_box_ctl(BOX_EXIT, 0);
        break;
    case 0x21:                                  /* DOS (Stufe 1: Auszug) */
        switch (AH) {
        case 0x01:
            AL = (unsigned char)key(1);
            tty_ch(s, AL);
            break;
        case 0x02:
            tty_ch(s, DL);
            break;
        case 0x06:
            if (DL != 0xFF)
                tty_ch(s, DL);
            else {
                k = key(0);
                svc_zf = k < 0;
                AL = k < 0 ? 0 : (unsigned char)key(1);
            }
            break;
        case 0x07: case 0x08:
            AL = (unsigned char)key(1);
            break;
        case 0x09:                              /* Text bis '$'         */
            off = DX;
            for (i = 0; i < 0xFFFF && rd8((unsigned)f->vds, off + i) != '$'; i++)
                ;
            vwin((unsigned)f->vds);
            kf_vio_tty(s, VWIN, off, i);
            break;
        case 0x0B:
            AL = key(0) < 0 ? 0 : 0xFF;
            break;
        case 0x25:                              /* Vektor setzen         */
            wr16(0, AL * 4, DX);
            wr16(0, AL * 4 + 2, (unsigned)f->vds);
            break;
        case 0x30:                              /* Version 6.00          */
            AX = 0x0006;
            BX = 0;
            CX = 0;
            break;
        case 0x35:                              /* Vektor holen          */
            BX = rd16(0, AL * 4);
            f->ves = rd16(0, AL * 4 + 2);
            break;
        case 0x40:                              /* Schreiben (1/2)       */
            if (BX == 1 || BX == 2) {
                vwin((unsigned)f->vds);
                kf_vio_tty(s, VWIN, DX, CX);
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
    }
    (void)t;
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
    unsigned o = rd16(0, n * 4), sg = rd16(0, n * 4 + 2);

    if (sg == VBIOS && o == n * 4) {            /* Vektor unveraendert:  */
        set_ip(f, ip);                          /* Dienst gleich         */
        set_flags(f, t, run_svc(n, f, t, vflags(f, t)));
        return;
    }
    push16(f, vflags(f, t));
    push16(f, (unsigned)f->cs);
    push16(f, ip);
    vfl[t] &= ~FL_IF;
    f->eflags &= ~(unsigned long)FL_TF;
    f->cs = sg;
    set_ip(f, o);
}

/* ---- Monitor -------------------------------------------------------------
   Aus start.asm exc_common, wenn eine Ausnahme aus der Box kam. -> 0:
   erledigt, weiter in der Box; sonst beendet fault() die Box. */
unsigned __far v86_exc(struct v86_frame *f)
{
    int t = kf_box_ctl(BOX_TASK, 0);
    unsigned cs = (unsigned)f->cs, ip = (unsigned)f->eip, i, v;
    unsigned char op, o32 = 0, rep = 0;

    enable_ints();                      /* Kernel laeuft mit Interrupts  */
    switch (f->vec) {
    case 0: case 1: case 3: case 4: case 5:     /* wie im Real Mode      */
        do_int(f, t, f->vec, ip);
        return 0;
    case 6:                                     /* ARPL n: Dienst n      */
        if (rd8(cs, ip) != 0x63)
            return 1;
        v = rd8(cs, ip + 1);
        set_ip(f, ip + 2);
        /* FLAGS des INT liegen auf dem Stack der Box: dort setzen */
        wr16((unsigned)f->ss, (unsigned)f->esp + 4,
             run_svc(v, f, t, rd16((unsigned)f->ss, (unsigned)f->esp + 4)));
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
            push16(f, (unsigned)(f->eflags >> 16) & ~3);  /* ohne VM/RF  */
        push16(f, vflags(f, t));
        break;
    case 0x9D:                                  /* POPF(D)               */
        v = pop16(f);
        if (o32)
            pop16(f);
        set_flags(f, t, v);
        break;
    case 0xCD:                                  /* INT n                 */
        do_int(f, t, rd8(cs, ip), ip + 1);
        return 0;
    case 0xCC:                                  /* INT 3                 */
        do_int(f, t, 3, ip);
        return 0;
    case 0xCE:                                  /* INTO                  */
        if (f->eflags & 0x800) {
            do_int(f, t, 4, ip);
            return 0;
        }
        break;
    case 0xCF:                                  /* IRET(D)               */
        if (o32) {
            ip = pop16(f);
            pop16(f);
            f->cs = pop16(f);
            pop16(f);
            v = pop16(f);
            pop16(f);
        } else {
            ip = pop16(f);
            f->cs = pop16(f);
            v = pop16(f);
        }
        set_flags(f, t, v);
        break;
    case 0xF4:                                  /* HLT: abgeben          */
        set_ip(f, ip);
        kf_box_ctl(BOX_SLEEP, 1);
        return 0;
    /* Ports gehoeren in Stufe 1 niemandem: Lesen FFh, Schreiben verpufft */
    case 0xE4: case 0xEC:                       /* IN AL                 */
        AL = 0xFF;
        if (op == 0xE4)
            ip++;
        break;
    case 0xE5: case 0xED:                       /* IN AX/EAX             */
        AX = 0xFFFF;
        if (o32)
            f->eax = 0xFFFFFFFFUL;
        if (op == 0xE5)
            ip++;
        break;
    case 0xE6: case 0xE7:                       /* OUT imm               */
        ip++;
        break;
    case 0xEE: case 0xEF:                       /* OUT DX                */
        break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: /* INS/OUTS              */
        if (rep)
            CX = 0;
        break;
    default:
        return 1;                               /* unbekannt: Abbruch    */
    }
    set_ip(f, ip);
    return 0;
}
