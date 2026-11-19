/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VCOM.VDD - serielle und parallele Ports in der DOS-Box (6.3, erstes
 * Modul nach docs/VDD.md; vorher in kernel/v86.c)
 *
 * Ports der Box (Basen aus ihrem BIOS-Datenbereich 0400h): COM1-4 als
 * einfacher 8250 ohne FIFO und ohne Interrupt (Teiler, LCR, MCR, IER, SCR
 * behalten ihren Wert, Loopback spiegelt THR nach RBR), LPT1-3 mit Daten-
 * latch, Status "bereit" und Steuerregister - die fallende Flanke von
 * STROBE druckt das Datenbyte. INT 14h (senden) und INT 17h (drucken)
 * dazu. Gesendet wird ueber die Geraete des Kernels (COMn, LPTn) im Namen
 * der Box; der Handle bleibt offen bis zum Ende der Box (task_end
 * schliesst ihn). Ohne dieses Modul liest die Box dort FFh und INT 17h
 * meldet "kein Drucker".
 */
#include "drvlib.h"
#include "netlib.h"                     /* fent(): ferne Einspruenge      */
#include "../include/vdd.h"
#include "../include/fapi_abi.h"

#define NT VDD_NTASK

static unsigned char lpt_d[NT][3], lpt_cr[NT][3];  /* Portregister      */
static struct vuart {
    unsigned char dll, dlm, ier, lcr, mcr, scr, rbr, dr;
} vu[NT][4];
static unsigned lpt_h[NT];              /* Handle + 1                    */
static unsigned char lpt_n[NT];         /* fuer welchen Port             */
static unsigned short bda[8];           /* 0400h-040Fh der Box           */
static struct vdd_svc sv;
static struct vdd_reg reg;

static void bda_get(unsigned t)
{
    sv.fn = VS_RD;
    sv.task = t;
    sv.lin = 0x400;
    sv.buf = (unsigned)bda;
    sv.len = sizeof(bda);
    devhlp(DH_VDDSVC, 0, (unsigned)&sv, 0);
}

static unsigned fapi(unsigned t, unsigned fn, unsigned a1, unsigned a2, unsigned a3,
                     unsigned pa, void *buf, unsigned len)
{
    sv.fn = VS_FAPI;
    sv.task = t;
    sv.ffn = fn;
    sv.a1 = a1;
    sv.a2 = a2;
    sv.a3 = a3;
    sv.pa = pa;
    sv.buf = (unsigned)buf;
    sv.len = len;
    return devhlp(DH_VDDSVC, 0, (unsigned)&sv, 0);
}

/* Zeichen an LPT1-3 (port 0-2) bzw. COM1-4 (4-7) -> 0 gesendet */
static int lpt_out(unsigned t, unsigned port, unsigned char c)
{
    static char name[5];
    static unsigned char ch;            /* im DS (SS ist der Kernelstapel) */
    unsigned r;
    if (lpt_h[t] && lpt_n[t] != port) {
        fapi(t, FAPI_CLOSE, lpt_h[t] - 1, 0, 0, 0, 0, 0);
        lpt_h[t] = 0;
    }
    if (!lpt_h[t]) {
        name[0] = port >= 4 ? 'C' : 'L';
        name[1] = port >= 4 ? 'O' : 'P';
        name[2] = port >= 4 ? 'M' : 'T';
        name[3] = (char)('1' + (port & 3));
        name[4] = 0;
        r = fapi(t, FAPI_OPEN, 0, OPEN_RW, 0, 1, name, 5);
        if (r < FAPI_ERR_FIRST) {
            lpt_h[t] = r + 1;
            lpt_n[t] = (unsigned char)port;
        }
    }
    ch = c;
    return !lpt_h[t] ||
           fapi(t, FAPI_WRITE, lpt_h[t] - 1, 0, 1, 2, &ch, 1) != 1;
}

static int com_reg(unsigned p)          /* -> Port * 8 + Register, -1    */
{
    unsigned i, b;
    for (i = 0; i < 4; i++) {
        b = bda[i];
        if (b && p >= b && p <= b + 7)
            return (int)(i * 8 + p - b);
    }
    return -1;
}

static int lpt_reg(unsigned p)          /* -> Port * 4 + Register, -1    */
{
    unsigned i, b;
    for (i = 0; i < 3; i++) {
        b = bda[4 + i];
        if (b && p >= b && p <= b + 2)
            return (int)(i * 4 + p - b);
    }
    return -1;
}

static unsigned char com_in(unsigned t, int k)
{
    struct vuart *u = &vu[t][k >> 3];
    unsigned char m;
    switch (k & 7) {
    case 0:
        if (u->lcr & 0x80)
            return u->dll;
        u->dr = 0;
        return u->rbr;
    case 1:
        return u->lcr & 0x80 ? u->dlm : (unsigned char)(u->ier & 0x0F);
    case 2:
        return 0x01;                            /* kein Interrupt        */
    case 3:
        return u->lcr;
    case 4:
        return (unsigned char)(u->mcr & 0x1F);
    case 5:
        return (unsigned char)(0x60 | u->dr);   /* Sender leer           */
    case 6:
        if (!(u->mcr & 0x10))
            return 0xB0;                        /* DCD, DSR, CTS         */
        m = u->mcr;
        return (unsigned char)(((m & 1) << 5) | ((m & 2) << 3) |
                               ((m & 4) << 4) | ((m & 8) << 4));
    }
    return u->scr;
}

static void com_out(unsigned t, int k, unsigned char v)
{
    struct vuart *u = &vu[t][k >> 3];
    switch (k & 7) {
    case 0:
        if (u->lcr & 0x80)
            u->dll = v;
        else if (u->mcr & 0x10) {
            u->rbr = v;
            u->dr = 1;
        } else
            lpt_out(t, 4 + (k >> 3), v);
        break;
    case 1:
        if (u->lcr & 0x80) u->dlm = v; else u->ier = v;
        break;
    case 3: u->lcr = v; break;
    case 4: u->mcr = v; break;
    case 7: u->scr = v; break;
    }
}

static unsigned io(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task;
    int k;
    if (t >= NT || c->port < 0x278 || c->port > 0x3FF)
        return VDD_NOTMINE;             /* schnell: Joystick 201h u. a.,
                                           ohne den BDA zu lesen (Basen
                                           der Karten: 278h-3FFh)        */
    bda_get(t);
    if (c->fn == VDD_IN) {
        if ((k = com_reg(c->port)) >= 0)
            c->val = com_in(t, k);
        else if ((k = lpt_reg(c->port)) >= 0) {
            if ((k & 3) == 0)                   /* Eingaberichtung (PS/2) */
                c->val = lpt_cr[t][k >> 2] & 0x20 ? 0xFF : lpt_d[t][k >> 2];
            else if ((k & 3) == 1)
                c->val = 0xDF;                  /* bereit, gewaehlt      */
            else
                c->val = lpt_cr[t][k >> 2] | 0xE0;
        } else
            return VDD_NOTMINE;
        return 0;
    }
    if ((k = com_reg(c->port)) >= 0)
        com_out(t, k, (unsigned char)c->val);
    else if ((k = lpt_reg(c->port)) >= 0) {
        if ((k & 3) == 0)
            lpt_d[t][k >> 2] = (unsigned char)c->val;
        else if ((k & 3) == 2) {
            if ((lpt_cr[t][k >> 2] & 1) && !(c->val & 1))
                lpt_out(t, k >> 2, lpt_d[t][k >> 2]);
            lpt_cr[t][k >> 2] = (unsigned char)(c->val & 0x3F);
        }
    } else
        return VDD_NOTMINE;
    return 0;
}

#define AH (c->ax >> 8)
#define SETAH(v) (c->ax = (c->ax & 0xFF) | ((unsigned)(v) << 8))
static unsigned int_fn(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task, dx = c->dx;
    if (t >= NT)
        return VDD_NOTMINE;
    bda_get(t);
    if (c->port == 0x14) {                      /* seriell: keine Daten,  */
        if (dx > 3 || !bda[dx])                 /* Senden laeuft ab       */
            c->ax = 0x8000;
        else if (AH == 0x01)                    /* senden ueber COMn      */
            SETAH(lpt_out(t, 4 + dx, (unsigned char)c->ax) ? 0x80 : 0x60);
        else if (AH == 0x00 || AH == 0x03)
            c->ax = 0x6000;                     /* Sendepuffer leer      */
        else if (AH <= 0x02)
            SETAH(0x80);
        return 0;
    }
    if (c->port == 0x17) {                      /* Drucker                */
        if (AH > 0x02 || dx > 2)
            return 0;
        if (!bda[4 + dx])
            SETAH(0x30);                        /* kein Port              */
        else if (AH == 0x00 && lpt_out(t, dx, (unsigned char)c->ax))
            SETAH(0x09);                        /* Zeitueberschreitung   */
        else
            SETAH(0x90);
        return 0;
    }
    return VDD_NOTMINE;
}

static unsigned event(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task, i;
    if (t >= NT)
        return 0;
    if (c->fn == VDD_EV_CREATE || c->fn == VDD_EV_DESTROY) {
        lpt_h[t] = 0;                   /* Handle schliesst task_end      */
        lpt_cr[t][0] = lpt_cr[t][1] = lpt_cr[t][2] = 0;
        for (i = 0; i < 4; i++) {       /* 2400 Baud, 8N1 wie nach dem BIOS */
            vu[t][i].dll = 0x30;
            vu[t][i].dlm = vu[t][i].ier = vu[t][i].mcr = vu[t][i].scr = 0;
            vu[t][i].lcr = 3;
            vu[t][i].dr = 0;
        }
    }
    return 0;
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    reg.version = VDD_VERSION;
    reg.size = sizeof(reg);
    scat(reg.name, "VCOM");
    reg.flags = VDDF_ANYPORT;
    reg.io_off = fent(0, io);
    reg.int_off = fent(1, int_fn);
    reg.ev_off = fent(2, event);
    reg.ints[0] = 0x14;
    reg.ints[1] = 0x17;
    if (devhlp(DH_VDDREG, 0, (unsigned)&reg, 0)) {
        dputs("VCOM.VDD: Anmeldung beim Kernel gescheitert\n");
        r->status = DRV_ERR_NODEV;
        return;
    }
    dputs("  COM1-4 und LPT1-3 der DOS-Boxen (Ausgabe \x81" "ber COMn/LPTn)\n");
    r->units = 0;
    r->status = DRV_OK;
}

void strategy(void)
{
    struct drv_req __far *r = REQ;
    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    case DRV_DEINIT:                    /* DEVLOAD /U: nichts anzuhalten  */
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
