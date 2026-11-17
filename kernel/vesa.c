/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - VESA (VBE 2.0/3.0) fuer FAPI-Programme (Phase 5, Schritt 2)
 *
 * Umschalten kann nur das Video-BIOS. Wie OS/2 1.x geht der Kernel dafuer
 * kurz in den Real Mode (start.asm rm_int10): Register bei physisch 6F00h,
 * Puffer ab 7000h (VbeInfoBlock 512 Byte, dahinter ModeInfoBlock 256).
 * Nur mit Kernelsperre (FAPI_VESA steht nicht in fapi_nolock): SCRATCH.
 *
 * Laeuft im zweiten Codesegment (KTEXT2).
 */
#include "../include/fapi_abi.h"
#include "k2.h"

struct rmregs { unsigned short ax, bx, cx, dx, si, di, bp, es, ds, flags,
                intno; };              /* 10h Video-BIOS, 15h (APM)     */
#define RM_REGS   0x6F00UL
#define RM_BUF    0x7000UL              /* 7000h: VbeInfoBlock, 7200h: ModeInfo */

unsigned long rm_kcr3;                  /* start.asm: CR3 waehrend des Aufrufs */
unsigned      rm_cs16;                  /* Real-Mode-Segment von CGROUP2   */
void __far rm_int10(void);

static unsigned char rm_apm;            /* naechster rm_call: INT 15h (APM) */

/* INT 10h im Real Mode; r wird mit den Rueckgabewerten ueberschrieben */
static void rm_call(struct rmregs *r)
{
    struct rmregs __far *p;
    unsigned i;
    r->intno = rm_apm ? 0x15 : 0x10;    /* nicht aus r: die meisten Aufrufer */
    rm_apm = 0;                         /* lassen intno ungesetzt (Stack)    */
    rm_kcr3 = paging_cr3();
    rm_cs16 = (unsigned)(KCODE2_LIN >> 4);
    scratch_map(RM_REGS, sizeof(struct rmregs) - 1);
    p = MK_FP(SCRATCH, 0);
    for (i = 0; i < sizeof(struct rmregs) / 2; i++)
        ((unsigned short __far *)p)[i] = ((unsigned short *)r)[i];
    rm_int10();
    scratch_map(RM_REGS, sizeof(struct rmregs) - 1);
    for (i = 0; i < sizeof(struct rmregs) / 2; i++)
        ((unsigned short *)r)[i] = ((unsigned short __far *)p)[i];
}

/* fuer v86.c: VBE-Abfrage (4F00h/4F01h) einer Box ueber den Real Mode -
   im V86-Modus scheitern sie bei BIOSen, die dabei ihre Erweiterungs-
   register lesen (Cirrus GD54xx: die VGA-Ports der Box sind gesperrt).
   sig = erste 4 Byte des Aufrufers ("VBE2"), Ergebnis 512 Byte nach out.
   -> AX */
unsigned __far vesa_boxq(unsigned ax, unsigned cx, unsigned long sig,
                         unsigned char __far *out)
{
    struct rmregs r;
    unsigned i;
    scratch_map(RM_BUF, 511);
    *(unsigned long __far *)MK_FP(SCRATCH, 0) = sig;
    r.ax = ax;
    r.bx = r.dx = r.si = r.bp = r.ds = r.flags = 0;
    r.cx = cx;
    r.es = (unsigned)(RM_BUF >> 4);
    r.di = 0;
    r.intno = 0x10;
    rm_call(&r);
    scratch_map(RM_BUF, 511);
    for (i = 0; i < 512; i++)
        out[i] = *(unsigned char __far *)MK_FP(SCRATCH, i);
    return r.ax;
}

/* 16 x 16 -> 32 Bit ohne Laufzeitbibliothek */
static unsigned long mul16(unsigned a, unsigned b);
#pragma aux mul16 = "mul dx" __parm [__ax] [__dx] __value [__dx __ax] __modify [__ax __dx];

static unsigned rd16(unsigned long lin)
{
    scratch_map(lin, 1);
    return *(unsigned short __far *)MK_FP(SCRATCH, 0);
}

static unsigned char rd8(unsigned long lin)
{
    scratch_map(lin, 0);
    return *(unsigned char __far *)MK_FP(SCRATCH, 0);
}

static unsigned vb_ver;                 /* VBE-Version (aus vbe_info)    */
static unsigned long vb_phys;           /* vbe_mode: linearer Bildspeicher */
static unsigned vb_gmul;                /* ... sonst Fenster: Schritte je 64 KB */

/* VbeInfoBlock holen: -> Version, 0 = kein VBE */
static unsigned vbe_info(unsigned long *list, unsigned *mem_kb)
{
    struct rmregs r;
    scratch_map(RM_BUF, 3);
    *(unsigned long __far *)MK_FP(SCRATCH, 0) = 0x32454256UL;   /* "VBE2" */
    r.ax = 0x4F00;
    r.es = (unsigned)(RM_BUF >> 4);
    r.di = 0;
    rm_call(&r);
    if (r.ax != 0x004F || rd16(RM_BUF) != 0x4556)               /* "VE" */
        return 0;
    *list = ((unsigned long)rd16(RM_BUF + 16) << 4) + rd16(RM_BUF + 14);
    *mem_kb = rd16(RM_BUF + 18) << 6;                           /* 64-KB-Bloecke */
    return vb_ver = rd16(RM_BUF + 4);
}

/* ModeInfoBlock nach RM_BUF + 200h; -> 1, wenn brauchbar (Grafik,
   Palette oder direkte Farben, 8/15/16/24/32 Bit). Linearer Bildspeicher
   (VBE 2.0+) -> vb_phys; sonst (VBE 1.2, ET4000/Cirrus/S3 der 386-Zeit)
   Fenster A bei A000h, 64 KB, lesbar und beschreibbar (oder wie bei den
   Tseng-BIOS A nur schreiben + B nur lesen, beide A000h: VB_SPLIT,
   beide Fenster werden gleich gestellt) -> vb_gmul, ohne
   24 Bit (Punkte laegen ueber der Fenstergrenze). model Bit 7 = Fenster */
static int vbe_mode(unsigned m, struct fapi_vesamode __far *o)
{
    struct rmregs r;
    unsigned attr;
    unsigned char bpp, model;
    r.ax = 0x4F01;
    r.cx = m;
    r.es = (unsigned)(RM_BUF >> 4);
    r.di = 0x200;
    rm_call(&r);
    if (r.ax != 0x004F)
        return 0;
    attr = rd16(RM_BUF + 0x200);
    bpp = rd8(RM_BUF + 0x200 + 25);
    model = rd8(RM_BUF + 0x200 + 27);
    if ((attr & 0x1B) != 0x1B ||                /* da, Angaben, Farbe, Grafik */
        (model != 4 && model != 6) ||
        (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32))
        return 0;
    vb_phys = 0;
    vb_gmul = 0;
    if ((attr & 0x80) && vb_ver >= 0x200)
        vb_phys = ((unsigned long)rd16(RM_BUF + 0x200 + 42) << 16) | rd16(RM_BUF + 0x200 + 40);
    if (!vb_phys) {                             /* nur ueber das Fenster */
        unsigned g = rd16(RM_BUF + 0x200 + 4);  /* Granularitaet in KB   */
        unsigned char wa = rd8(RM_BUF + 0x200 + 2) & 7, wb = rd8(RM_BUF + 0x200 + 3) & 7;
        int split = 0;                          /* A schreiben, B lesen  */
        if (wa == 5 && wb == 3 && rd16(RM_BUF + 0x200 + 10) == 0xA000)
            split = 1;                          /* so die Tseng-BIOS     */
        else if (wa != 7)
            return 0;
        if ((attr & 0x40) || bpp == 24 ||
            rd16(RM_BUF + 0x200 + 6) != 64 || rd16(RM_BUF + 0x200 + 8) != 0xA000 ||
            !g || g > 64 || 64 % g)
            return 0;
        vb_gmul = 64 / g | (split ? VB_SPLIT : 0);
        model |= 0x80;
    }
    o->mode = m;
    o->width = rd16(RM_BUF + 0x200 + 18);
    o->height = rd16(RM_BUF + 0x200 + 20);
    o->pitch = vb_phys && vb_ver >= 0x300 ? rd16(RM_BUF + 0x200 + 50) : 0;
                                                /* LinBytesPerScanLine (3.0) */
    if (!o->pitch)
        o->pitch = rd16(RM_BUF + 0x200 + 16);
    o->bpp = bpp;
    o->model = model;
    return 1;
}

/* Modus setzen (mode = 3: Text, sonst VBE-Nummer mit Bit 14 = LFB):
   -> 0 = ok */
int __far vesa_bios(unsigned mode)
{
    struct rmregs r;
    if (mode < 0x100) {                 /* VGA-/Chip-Modus: INT 10h 00h */
        r.ax = mode & 0xFF;
        rm_call(&r);
        return 0;
    }
    r.ax = 0x4F02;
    r.bx = mode;
    rm_call(&r);
    return r.ax != 0x004F;
}

/* Fenster A auf Stelle n (in Granularitaets-Schritten) */
void __far vesa_bank(unsigned n, int both)
{
    struct rmregs r;
    r.ax = 0x4F05;
    r.bx = 0;
    r.dx = n;
    rm_call(&r);
    if (both) {                             /* Fenster B (Lesen) mit     */
        r.ax = 0x4F05;
        r.bx = 1;
        r.dx = n;
        rm_call(&r);
    }
}

/* Stellung von Fenster w (0 = A, 1 = B) in Granularitaets-Schritten */
unsigned __far vesa_getbank(unsigned w)
{
    struct rmregs r;
    r.ax = 0x4F05;
    r.bx = 0x100 | w;
    r.dx = 0;
    rm_call(&r);
    return r.ax == 0x004F ? r.dx : 0;
}

void __far vesa_setwin(unsigned w, unsigned n)
{
    struct rmregs r;
    r.ax = 0x4F05;
    r.bx = w;
    r.dx = n;
    rm_call(&r);
}

unsigned __far vesa_fapi(int s, int t, unsigned sub, unsigned n,
                         struct fapi_vesamode __far *o)
{
    unsigned long list;
    unsigned mem_kb, ver, m, k = 0, i, pages;
    if (sub == VESA_SET) {
        if (!vbe_info(&list, &mem_kb) || !vbe_mode(n, o))
            return FAPI_ERROR;
        pages = (unsigned)((mul16(o->pitch, o->height) + 4095) >> 12);
        if (pages > 880 || vio_setvesa(s, t, o, pages, vb_phys, vb_gmul))
            return FAPI_ERROR;
        o->mem_kb = mem_kb;
        o->version = 0;
        return 0;
    }
    if (sub != VESA_MODE || !(ver = vbe_info(&list, &mem_kb)))
        return FAPI_ERROR;
    for (i = 0; i < 256; i++) {
        m = rd16(list + i * 2);
        if (m == 0xFFFF)
            break;
        if (vbe_mode(m, o) && ((mul16(o->pitch, o->height) + 4095) >> 12) <= 880 &&
            k++ == n) {
            o->mem_kb = mem_kb;
            o->version = ver;
            return 0;
        }
    }
    return FAPI_ERROR;
}

/* ---- SVGA-Zustand der DOS-Boxen (vgagfx.c raw_out/raw_in) -------------
   Chipunabhaengig ueber VBE: 4F04h sichert/laedt den Zustand der Karte
   (Register, BIOS-Daten, DAC, SVGA-Register samt Bank), den Bildspeicher
   des Modus kopiert vgagfx.c selbst (linear oder Fenster fuer Fenster). */

/* Modus m (wie die Box ihn per 4F02h verlangt hat) beschreiben; 0 = ok */
int __far vesa_boxinfo(unsigned m, struct vbox __far *o)
{
    struct rmregs r;
    unsigned attr, wa, wb, g;
    unsigned char model;
    unsigned long list;
    unsigned mem_kb;
    o->bytes = 0;
    if (!vbe_info(&list, &mem_kb))
        return -1;
    r.ax = 0x4F01;
    r.cx = m & 0x1FF;
    r.es = (unsigned)(RM_BUF >> 4);
    r.di = 0x200;
    rm_call(&r);
    if (r.ax != 0x004F)
        return -1;
    attr = rd16(RM_BUF + 0x200);
    model = rd8(RM_BUF + 0x200 + 27);
    if (!(attr & 0x10) || (model != 4 && model != 6))
        return -1;                      /* Text, planar: wie VGA         */
    o->bytes = mul16(rd16(RM_BUF + 0x200 + 16), rd16(RM_BUF + 0x200 + 20));
    if (vb_ver >= 0x300 && (m & 0x4000))
        o->bytes = mul16(rd16(RM_BUF + 0x200 + 50), rd16(RM_BUF + 0x200 + 20));
    o->phys = (m & 0x4000) && (attr & 0x80) && vb_ver >= 0x200 ?
              ((unsigned long)rd16(RM_BUF + 0x200 + 42) << 16) | rd16(RM_BUF + 0x200 + 40) : 0;
    wa = rd8(RM_BUF + 0x200 + 2) & 7;
    wb = rd8(RM_BUF + 0x200 + 3) & 7;
    g = rd16(RM_BUF + 0x200 + 4);
    o->split = (unsigned char)(wa == 5 && wb == 3);
    o->gmul = g && g <= 64 && !(64 % g) ? 64 / g : 0;
    if (!o->phys && (!o->gmul || (attr & 0x40) || rd16(RM_BUF + 0x200 + 8) != 0xA000 ||
                     (wa != 7 && !o->split)))
        return -1;                      /* weder linear noch Fenster     */
    return 0;
}

/* 4F04h: op 0 = Groesse in Byte (0 = nicht da), 1 = sichern nach lin,
   2 = aus lin laden. Puffer im ersten MB bei RM_BUF (hoechstens 3 KB). */
unsigned __far vesa_state(unsigned op, unsigned long lin)
{
    struct rmregs r;
    unsigned n;
    r.ax = 0x4F04;
    r.dx = 0;
    r.cx = 0x000F;
    rm_call(&r);
    if (r.ax != 0x004F || !r.bx || r.bx > 48)
        return 0;
    n = r.bx << 6;
    if (op == 2) {
        scratch_map(RM_BUF, n - 1);
        scratch2_map(lin, n - 1);
        far_copy(SCRATCH, 0, SCRATCH2, 0, n);
    }
    if (op) {
        r.ax = 0x4F04;
        r.dx = op;                      /* 1 sichern, 2 laden            */
        r.cx = 0x000F;
        r.es = (unsigned)(RM_BUF >> 4);
        r.bx = 0;
        rm_call(&r);
        if (r.ax != 0x004F)
            return 0;
    }
    if (op == 1) {
        scratch_map(RM_BUF, n - 1);
        scratch2_map(lin, n - 1);
        far_copy(SCRATCH2, 0, SCRATCH, 0, n);
    }
    return n;
}

/* ---- APM (Ausschalten) -------------------------------------------------
   Wie DOS/Windows mit POWER.EXE: ueber die Real-Mode-Schnittstelle des
   APM-BIOS (INT 15h 53xxh) - derselbe kurze Ausflug in den Real Mode wie
   fuer das Video-BIOS. Erst pruefen (5300h, BX = 'PM'), verbinden (5301h,
   "schon verbunden" ist kein Fehler), Treiberversion melden (530Eh, 1.2
   bzw. 1.1), Energieverwaltung an (5308h), dann alle Geraete aus (5307h
   BX = 1, CX = 3). Kehrt nur zurueck, wenn es nicht geklappt hat:
   -1 kein APM, -2 BIOS lehnt ab. Nur in system_down, nach allem
   Schreiben. */
static void apm(struct rmregs *r, unsigned ax, unsigned bx, unsigned cx)
{
    unsigned i;
    for (i = 0; i < sizeof(*r); i++)
        ((unsigned char *)r)[i] = 0;
    r->ax = ax;
    r->bx = bx;
    r->cx = cx;
    rm_apm = 1;
    rm_call(r);
}

/* APM-Version fuer MSD (5300h, BX = 'PM') -> 12h fuer 1.2, 0 = keins */
unsigned __far apm_ver(void)
{
    static struct rmregs r;
    apm(&r, 0x5300, 0, 0);
    if ((r.flags & 1) || r.bx != 0x504D)
        return 0;
    return ((r.ax >> 4) & 0xF0) | (r.ax & 0x0F);
}

int __far apm_off(void)
{
    static struct rmregs r;
    unsigned ver;
    apm(&r, 0x5300, 0, 0);
    if ((r.flags & 1) || r.bx != 0x504D)        /* 'PM'                  */
        return -1;
    ver = r.ax;
    if (ver < 0x0101)                           /* 1.0: ohne 530Eh      */
        ver = 0x0100;
    apm(&r, 0x5301, 0, 0);                      /* verbinden            */
    if (ver >= 0x0101)
        apm(&r, 0x530E, 0, ver >= 0x0102 ? 0x0102 : 0x0101);
    apm(&r, 0x5308, 1, 1);                      /* Energieverwaltung an */
    apm(&r, 0x5307, 1, 3);                      /* alles aus            */
    return -2;
}

/* ---- Ausschalten ueber ACPI (S5) --------------------------------------
   Wie Linux, ohne den Rest von ACPI: RSDP im EBDA (erstes KB) oder in
   E0000h-FFFFFh suchen, ueber die RSDT die FADT ("FACP"), daraus
   PM1a/PM1b_CNT (bei 0 die erweiterte Adresse X_PM1a_CNT, nur I/O),
   SMI_CMD/ACPI_ENABLE und die DSDT; in der DSDT das Paket \_S5 (SLP_TYPa/
   SLP_TYPb). Ist SCI_EN aus, erst ACPI_ENABLE an SMI_CMD. Dann SLP_TYP <<
   10 | SLP_EN (2000h) an PM1a/PM1b. Kehrt zurueck, wenn es kein ACPI gibt
   oder der Rechner danach noch laeuft (dann APM). Anlass: Proxmox mit
   q35/KVM – das APM von SeaBIOS blieb dort haengen. Nur in system_down. */
unsigned k2_inw(unsigned port);
#pragma aux k2_inw = "in ax,dx" __parm [__dx] __value [__ax];
void k2_outw(unsigned port, unsigned v);
#pragma aux k2_outw = "out dx,ax" __parm [__dx] [__ax];

static unsigned long aw_base = 0xFFFFFFFFUL;

static unsigned char pb(unsigned long a)
{
    if (aw_base == 0xFFFFFFFFUL || a < aw_base || a - aw_base >= 0xF000UL) {
        aw_base = a;
        scratch_map(a, 0xFFFF);
        if (a >= 0x100000UL)            /* Tabellen liegen hinter dem RAM, */
            set_desc(SCRATCH >> 3,      /* dort reicht der Alias nicht:   */
                     pg_win(SCRATCH, a, 0xFFFF), 0xFFFF, ACC_KDATA);
    }
    return *(unsigned char __far *)MK_FP(SCRATCH, (unsigned)(a - aw_base));
}

static unsigned long p32(unsigned long a)
{
    return (unsigned long)pb(a) | ((unsigned long)pb(a + 1) << 8) |
           ((unsigned long)pb(a + 2) << 16) | ((unsigned long)pb(a + 3) << 24);
}

/* RSDP in [a, a+n) auf 16-Byte-Grenzen */
static unsigned long rsdp_scan(unsigned long a, unsigned long n)
{
    unsigned long e = a + n;
    unsigned i;
    unsigned char s;
    for (; a < e; a += 16) {
        if (pb(a) != 'R' || pb(a + 1) != 'S' || pb(a + 2) != 'D' ||
            pb(a + 3) != ' ' || pb(a + 4) != 'P' || pb(a + 5) != 'T' ||
            pb(a + 6) != 'R' || pb(a + 7) != ' ')
            continue;
        for (s = 0, i = 0; i < 20; i++)
            s += pb(a + i);
        if (!s)
            return a;
    }
    return 0;
}

static unsigned s5_val(unsigned long *q)
{
    unsigned char b = pb(*q);
    if (b == 0x0A) {                            /* BytePrefix            */
        b = pb(*q + 1);
        *q += 2;
        return b;
    }
    *q += 1;                                    /* ZeroOp/OneOp/Wert     */
    return b == 0xFF ? 0 : b;
}

int __far acpi_off(void)
{
    unsigned long r, rsdt, f, dsdt, len, i, q;
    unsigned pm1a, pm1b, smi, en, ta, tb, k;
    unsigned long t0;

    aw_base = 0xFFFFFFFFUL;
    r = 0;
    k = (unsigned)p32(0x40E) & 0xFFFF;          /* EBDA-Segment          */
    if (k >= 0x8000 && k < 0xA000)
        r = rsdp_scan((unsigned long)k << 4, 1024);
    if (!r)
        r = rsdp_scan(0xE0000UL, 0x20000UL);
    if (!r)
        return -1;
    rsdt = p32(r + 16);
    if (!rsdt || p32(rsdt) != 0x54445352UL)     /* "RSDT"                */
        return -1;
    len = p32(rsdt + 4);
    if (len > 4096)
        return -1;
    f = 0;
    for (i = 36; i + 4 <= len; i += 4) {
        q = p32(rsdt + i);
        if (q && p32(q) == 0x50434146UL) {      /* "FACP"                */
            f = q;
            break;
        }
    }
    if (!f)
        return -1;
    len = p32(f + 4);
    dsdt = p32(f + 40);
    smi = (unsigned)p32(f + 48);
    en = pb(f + 52);
    pm1a = (unsigned)p32(f + 64);
    pm1b = (unsigned)p32(f + 68);
    if (!pm1a && len >= 184 && pb(f + 172) == 1) /* X_PM1a_CNT, I/O      */
        pm1a = (unsigned)p32(f + 176);
    if (!pm1a || !dsdt || p32(dsdt) != 0x54445344UL)   /* "DSDT"         */
        return -1;
    len = p32(dsdt + 4);
    if (len > 0x100000UL)
        len = 0x100000UL;
    for (i = 36; i + 8 < len; i++) {
        if (pb(dsdt + i) != '_' || pb(dsdt + i + 1) != 'S' ||
            pb(dsdt + i + 2) != '5' || pb(dsdt + i + 3) != '_')
            continue;
        if (pb(dsdt + i - 1) != 0x08 &&         /* NameOp, auch \_S5_    */
            (pb(dsdt + i - 1) != '\\' || pb(dsdt + i - 2) != 0x08))
            continue;
        q = dsdt + i + 4;
        if (pb(q) != 0x12)                      /* PackageOp             */
            continue;
        q += 1 + ((pb(q + 1) >> 6) & 3) + 1;    /* PkgLength             */
        q++;                                    /* NumElements           */
        ta = s5_val(&q);
        tb = s5_val(&q);
        if (!(k2_inw(pm1a) & 1) && smi && en) { /* SCI_EN aus            */
            outb(smi, en);
            t0 = get_ticks();
            while (!(k2_inw(pm1a) & 1) && get_ticks() - t0 < 300)
                ;
        }
        irq_off();
        k2_outw(pm1a, ((ta & 7) << 10) | 0x2000);
        if (pm1b)
            k2_outw(pm1b, ((tb & 7) << 10) | 0x2000);
        enable_ints();
        t0 = get_ticks();
        while (get_ticks() - t0 < 100)          /* 1 s warten            */
            ;
        return -2;
    }
    return -1;
}
