/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATDOSCM.SYS - DOS-interne Dienste fuer Redirectoren und TSRs
 * (INT 2Fh 12xxh) und die Netzaufrufe 21h 5Eh/5Fh.
 *
 * Vorlage fuer Verhalten und Register: MS-DOS 4.0 (MIT), MACRO.ASM
 * ($UserOper, $AssignOper) und die Liste der 12xxh-Dienste. Ein Netz im
 * Sinne von MS-DOS gibt es in der Box nicht - die AT-DOS-Laufwerke
 * (auch NET USE) erscheinen lokal, also verhalten sich 5Eh 02h-05h und
 * 5Fh 00h-06h wie MS-DOS ohne geladenen Redirector (Fehler 01h).
 *
 * INT 2Fh 12xxh kommt ueber denselben Eingang wie INT 21h (entry.asm
 * i2f12): Rahmen wie SAVE_WORLD auf dem Stapel des Aufrufers.
 */
#include "cm.h"

struct frame __far *user_frame;         /* Rahmen des aeussersten 21h   */

static char  myname[16];                /* 5E00h/5E01h                   */
static u16   mynum;

static u8 up(u8 c)
{
    return c >= 'a' && c <= 'z' ? (u8)(c - 32) : c;
}

/* ---- 21h 5Eh/5Fh -------------------------------------------------------- */
void net_fn(struct frame __far *f)
{
    unsigned i;
    if (AH(f) == 0x5E) {
        switch (AL(f)) {
        case 0x00:                      /* Rechnername: CH = 0 ungesetzt */
            for (i = 0; i < 15; i++)
                B(f->ds, f->dx + i) = (u8)myname[i];
            B(f->ds, f->dx + 15) = 0;
            f->cx = mynum;
            return;
        case 0x01:
            mynum = f->cx;
            for (i = 0; i < 15; i++)
                myname[i] = (char)B(f->ds, f->dx + i);
            return;
        }
        set_err(f, E_FUNC);             /* Drucker: kein Redirector      */
        return;
    }
    if (AL(f) == 0x07 || AL(f) == 0x08) {   /* CDS in Gebrauch an/aus    */
        unsigned d = DL(f);
        if (d >= NDRV || !drive_ok(d)) {
            set_err(f, E_DRIVE);
            return;
        }
        if (AL(f) == 0x07)
            cdstab[d].flags |= 0x4000;
        else
            cdstab[d].flags &= ~0x4000;
        return;
    }
    set_err(f, E_FUNC);                 /* 00h-06h: kein Redirector      */
}

/* ---- INT 2Fh 12xxh ------------------------------------------------------ */
/* Wort auf dem Stapel des Aufrufers (ueber dem IRET-Rahmen) */
#define STK(f)  (((u16 __far *)(f))[12])

static void zf(struct frame __far *f, int set)
{
    if (set)
        f->fl |= FL_ZF;
    else
        f->fl &= ~FL_ZF;
}

static void cf(struct frame __far *f, int set)
{
    if (set)
        f->fl |= FL_CF;
    else
        f->fl &= ~FL_CF;
}

static u16 slen(u16 seg, u16 off)
{
    u16 n = 0;
    while (B(seg, off + n))
        n++;
    return n;
}

/* Dateinamen vergleichen: ohne Gross/klein, / gleich \ */
static int fname_eq(u16 s1, u16 o1, u16 s2, u16 o2)
{
    u8 a, b;
    do {
        a = up(B(s1, o1++));
        b = up(B(s2, o2++));
        if (a == '/')
            a = '\\';
        if (b == '/')
            b = '\\';
        if (a != b)
            return 0;
    } while (a);
    return 1;
}

void datetime_fn(struct frame __far *f);

static void sub21(struct frame __far *f, u16 ax)
{
    f->ax = ax;
    f->fl &= ~FL_CF;
    file_fn(f);
}

struct frame __far *int2f12_c(struct frame __far *f)
{
    unsigned i;
    u16 s, o;
    f->fl &= ~FL_CF;
    switch (AL(f)) {
    case 0x00:                          /* da                            */
        AL(f) = 0xFF;
        break;
    case 0x03:                          /* Datensegment des DOS          */
        f->ds = kseg;
        break;
    case 0x04: {                        /* Trenner normieren             */
        u8 c = (u8)STK(f);
        if (c == '/')
            c = '\\';
        AL(f) = c;
        zf(f, c == '\\');
        break;
    }
    case 0x05:                          /* Zeichen ausgeben              */
        con_out((u8)STK(f));
        break;
    case 0x08: {                        /* SFT-Verweise herunterzaehlen  */
        u16 __far *r = (u16 __far *)MK_FP(f->es, f->di);
        f->ax = *r;
        if (*r > 1)
            (*r)--;
        break;
    }
    case 0x09: case 0x0E: case 0x0F:    /* Puffer: hat der Kernel        */
        break;
    case 0x0D: {                        /* Datum/Zeit gepackt            */
        struct frame t;
        u16 d;
        t.ax = 0x2A00;
        datetime_fn((struct frame __far *)&t);
        d = ((t.cx - 1980) << 9) | ((t.dx >> 8) << 5) | (t.dx & 0xFF);
        t.ax = 0x2C00;
        datetime_fn((struct frame __far *)&t);
        f->dx = ((t.cx >> 8) << 11) | ((t.cx & 0xFF) << 5) | ((t.dx >> 8) >> 1);
        f->ax = d;
        break;
    }
    case 0x11:                          /* Namen normieren DS:SI -> ES:DI */
        s = f->ds, o = f->si;
        for (i = 0; ; i++) {
            u8 c = up(B(s, o + i));
            if (c == '/')
                c = '\\';
            B(f->es, f->di + i) = c;
            if (!c)
                break;
        }
        break;
    case 0x12:                          /* Laenge ES:DI mit Null (MISC2) */
        f->cx = slen(f->es, f->di) + 1;
        break;
    case 0x13:                          /* Zeichen gross                 */
        AL(f) = up((u8)STK(f));
        break;
    case 0x14:                          /* Fernzeiger vergleichen        */
        zf(f, f->ds == f->es && f->si == f->di);
        break;
    case 0x16:                          /* SFT-Eintrag BX -> ES:DI       */
        if (f->bx >= NSFT) {
            cf(f, 1);
            break;
        }
        f->es = kseg;
        f->di = (u16)&sftab[f->bx];
        break;
    case 0x17: {                        /* CDS des Laufwerks -> DS:SI    */
        unsigned d = STK(f);
        if (d >= NDRV || !drive_ok(d)) {
            cf(f, 1);
            break;
        }
        f->ds = kseg;
        f->si = (u16)&cdstab[d];
        AL(f) = (u8)d;
        break;
    }
    case 0x18:                          /* Register des Aufrufers        */
        if (user_frame) {
            f->ds = FP_SEG(user_frame);
            f->si = FP_OFF(user_frame);
        }
        break;
    case 0x1A: {                        /* Laufwerk eines Namens         */
        u8 c0 = B(f->ds, f->si), c1 = B(f->ds, f->si + 1);
        if (c0 && c1 == ':') {
            c0 = up(c0);
            AL(f) = c0 >= 'A' && c0 <= 'Z' ? (u8)(c0 - 'A' + 1) : 0xFF;
            f->si += 2;
        } else
            AL(f) = 0;
        break;
    }
    case 0x1E:                          /* Namen vergleichen             */
        zf(f, fname_eq(f->ds, f->si, f->es, f->di));
        break;
    case 0x20: {                        /* JFT-Eintrag BX -> ES:DI       */
        u16 p = cur_psp();
        if (f->bx >= W(p, 0x32)) {
            f->ax = E_HANDLE;
            cf(f, 1);
            break;
        }
        f->es = W(p, 0x36);
        f->di = W(p, 0x34) + f->bx;
        break;
    }
    case 0x21:                          /* kanonischer Name (wie 60h)    */
        sub21(f, 0x6000);
        break;
    case 0x25:                          /* Laenge DS:SI mit Null         */
        f->cx = slen(f->ds, f->si) + 1;
        break;
    case 0x26:                          /* Oeffnen: CL Modus, DS:DX      */
        sub21(f, 0x3D00 | CL(f));
        break;
    case 0x27:
        sub21(f, 0x3E00);
        break;
    case 0x28: case 0x2B:               /* Seek bzw. IOCTL: AX in BP     */
        sub21(f, f->bp);
        break;
    case 0x29:
        sub21(f, 0x3F00);
        break;
    case 0x2E: {                        /* Meldungstabellen (DOS 4.0):   */
        static u32 msgtab[5];           /* DL gerade holen, ungerade     */
        unsigned k = DL(f) >> 1;        /* setzen: 0 erweitert, 1 Parse, */
        if (k >= 5)                     /* 2 kritisch, 3 Dateisystem,    */
            break;                      /* 4 READ_DISK_PROC - COMMAND.COM */
        if (DL(f) & 1)                  /* 4.0 meldet sie an und holt    */
            msgtab[k] = ((u32)f->es << 16) | f->di;  /* sie im INT-24h- */
        else {                          /* Handler wieder (sonst kreiste */
            f->es = (u16)(msgtab[k] >> 16);          /* SYSGETMSG)      */
            f->di = (u16)msgtab[k];
        }
        break;
    }
    case 0x2C:                          /* Geraetekette hinter NUL       */
        f->bx = (u16)(devs[DV_NUL].next >> 16);
        f->ax = (u16)devs[DV_NUL].next;
        break;
    case 0x2D:                          /* erweiterter Fehler            */
        f->ax = *(u16 *)(sda + 4);
        break;
    default:
        f->ax = E_FUNC;
        cf(f, 1);
    }
    return f;
}

/* ---- INT 25h/26h (absolut lesen/schreiben) ------------------------------ */
static u32 lin(u16 seg, u16 off)
{
    return ((u32)seg << 4) + off;
}

/* Wie MS-DOS 7: AL Laufwerk (0 = A:), CX Sektoren, DX erster Sektor,
   DS:BX Puffer; CX = FFFFh: DS:BX -> Paket (DWORD Sektor, WORD Anzahl,
   DWORD Puffer). Fehler in AX (AH BIOS, AL Treiber), CF. */
static void abs_args(struct frame __far *f)
{
    q.drive = AL(f);
    if (f->cx == 0xFFFF) {
        q.pos = D(f->ds, f->bx);
        q.count = W(f->ds, f->bx + 4);
        q.buf = lin(W(f->ds, f->bx + 8), W(f->ds, f->bx + 6));
        q.flags = 1;
    } else {
        q.pos = f->dx;
        q.count = f->cx;
        q.buf = lin(f->ds, f->bx);
        q.flags = 0;
    }
}

struct frame __far *int25_c(struct frame __far *f)
{
    unsigned e;
    abs_args(f);
    e = drive_ok(q.drive) ? dem(DEM_ABSREAD) : 0x8002;
    if (e) {
        f->ax = e;
        cf(f, 1);
    } else
        cf(f, 0);
    return f;
}

/* Schreiben braucht wie unter MS-DOS 7 eine Laufwerkssperre (440Dh 4Ah),
   sonst meldet der Kernel Schreibschutz (0300h) */
struct frame __far *int26_c(struct frame __far *f)
{
    unsigned e;
    abs_args(f);
    e = drive_ok(q.drive) ? dem(DEM_ABSWRITE) : 0x8002;
    if (e) {
        f->ax = e;
        cf(f, 1);
    } else
        cf(f, 0);
    return f;
}

/* INT 21h 7305h (Windows 95 OSR2): absolut lesen/schreiben, auch FAT32.
   DL Laufwerk (0 = aktuelles), CX = FFFFh, DS:BX Paket wie INT 25h,
   SI Bit 0 = schreiben (Bits 13-15 Art der Daten: ohne Bedeutung) */
void abs7305(struct frame __far *f)
{
    unsigned e;
    if (AL(f) != 0x05 || f->cx != 0xFFFF) {
        set_err(f, AL(f) != 0x05 ? 1 : 0x57);
        return;
    }
    AL(f) = DL(f) ? DL(f) - 1 : cur_drv;
    abs_args(f);
    q.flags = 3;                        /* Paketform, ueber 7305h        */
    e = !drive_ok(q.drive) ? 0x8002 : dem(f->si & 1 ? DEM_ABSWRITE : DEM_ABSREAD);
    if (e) {
        f->ax = e;
        cf(f, 1);
    } else
        cf(f, 0);
}

/* ---- Laufwerkssperre wie MS-DOS 7 (440Dh, CH 08h oder 48h) -------------
   4Ah logisch sperren (BH Stufe 0-3), 6Ah freigeben, 6Ch Zugriffsmerker,
   70h Stand; geschachtelt gezaehlt, der Kernel sperrt beim ersten und
   gibt beim letzten frei (DEM_DLOCK). Endet das Programm, gibt term()
   seine Sperren frei. 4Bh/6Bh (physisch) nur bestaetigt. */
static u8  lk_cnt[NDRV], lk_lvl[NDRV];
static u16 lk_psp[NDRV];

void dlock_fn(struct frame __far *f, unsigned d)
{
    unsigned e;
    switch (CL(f)) {
    case 0x4A:
        if (!lk_cnt[d]) {
            q.drive = d;
            q.flags = 0;
            if ((e = dem(DEM_DLOCK)) != 0) {
                set_err(f, e);
                return;
            }
            lk_psp[d] = cur_psp();
            lk_lvl[d] = 0;
        }
        if (BH(f) > lk_lvl[d])
            lk_lvl[d] = BH(f);
        lk_cnt[d]++;
        f->ax = 0;
        break;
    case 0x6A:
        if (!lk_cnt[d]) {
            set_err(f, 0x01);
            return;
        }
        if (!--lk_cnt[d]) {
            q.drive = d;
            q.flags = 1;
            dem(DEM_DLOCK);
        }
        f->ax = 0;
        break;
    case 0x6C:                          /* Zugriffsmerker: unveraendert  */
        f->ax = 0;
        break;
    default:                            /* 70h: Stufe und Rechte         */
        f->ax = lk_cnt[d] ? lk_lvl[d] : 0xFFFF;
        f->cx = 0;
        break;
    }
}

void dlock_end(u16 psp)
{
    unsigned d;
    for (d = 0; d < NDRV; d++)
        if (lk_cnt[d] && lk_psp[d] == psp) {
            lk_cnt[d] = 0;
            q.drive = d;
            q.flags = 1;
            dem(DEM_DLOCK);
        }
}
