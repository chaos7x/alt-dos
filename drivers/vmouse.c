/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VMOUSE.SYS - Maustreiber der DOS-Box (INT 33h wie MOUSE.COM von
 * Microsoft, 6.3 Stufe 4 nach docs/VDD.md; wie VMOUSE.SYS bei OS/2 2.x;
 * vorher in kernel/v86.c)
 *
 * Die Maus der Sitzung (MOUSE.SYS) gehoert nach dem Reset der Box
 * (VS_MOUSE VM_OPEN). Bewegung kommt als ungekuerzte Mickeys (VM_DELTA),
 * Tasten als Ereignisse (VM_READ); Position, Grenzen und Zaehler rechnet
 * das Modul je Box. Im Textmodus zeichnet der Kernel den Zeiger an der
 * Stelle der Box (VM_POS); in Grafikmodi gibt es keinen Zeiger (Position
 * und Tasten stimmen). Den Rueckruf (0Ch/14h) stellt der Kernel zu
 * (VS_CBARM): er fragt hier nach (VDD_CBQ), ob ein gewuenschtes Ereignis
 * ansteht, und uebernimmt die Register. Ohne dieses Modul meldet INT 33h
 * "kein Treiber" (AX = 0).
 *
 * Dazu die Zeigegeraete-Schnittstelle des PS/2-BIOS (INT 15h C2xxh), die
 * MOUSE.DRV von Windows 3.1 nimmt (Ausstattungswort 0410h Bit 2): Pakete
 * wie die PS/2-Maus (Status, X, Y, Z), Zustellung an den Rueckruf aus
 * C207h ueber denselben Weg (VS_CBARM a3 = 1: vier Worte auf dem Stapel
 * wie beim BIOS, RETF).
 */
#include "drvlib.h"
#include "netlib.h"                     /* fent()                        */
#include "../include/vdd.h"

#define NT VDD_NTASK

static struct vmou {
    unsigned char on, nbtn, btn;
    signed char show;
    unsigned char rx, ry, cw;           /* Mickeys je 8 Punkte, Zellbreite */
    int x, y, xmin, xmax, ymin, ymax;
    int mcx, mcy, fx, fy;
    unsigned mask, cbofs, cbseg, evp;
    unsigned npr[3], nrl[3];
    int prx[3], pry[3], rlx[3], rly[3];
} mo[NT];

/* PS/2-BIOS (INT 15h C2xxh) je Box */
static struct vps2 {
    unsigned char en, init, rate, res, scal, btn, nq;
    unsigned char q[4];                 /* Tastenstaende, noch nicht gemeldet */
    int dx, dy;                         /* Bewegung, noch nicht gemeldet  */
    unsigned hofs, hseg;                /* Rueckruf (C207h)              */
} ps[NT];

static struct vdd_svc sv;
static struct vdd_reg reg;
static unsigned short ev[4];            /* VM_READ, VM_DELTA             */
static unsigned char bm[2];             /* BDA 0449h (Modus), 0484h (Zeilen-1) */
static unsigned bm_t = 0xFFFF, bm_tk;

static unsigned vs(unsigned fn, unsigned t, unsigned a1, unsigned a2, unsigned a3)
{
    sv.fn = fn;
    sv.task = t;
    sv.a1 = a1;
    sv.a2 = a2;
    sv.a3 = a3;
    sv.buf = (unsigned)ev;
    sv.len = sizeof(ev);
    return devhlp(DH_VDDSVC, 0, (unsigned)&sv, 0);
}

static void rdb(unsigned t, unsigned lin, unsigned char *b)
{
    sv.fn = VS_RD;
    sv.task = t;
    sv.lin = lin;
    sv.buf = (unsigned)b;
    sv.len = 1;
    devhlp(DH_VDDSVC, 0, (unsigned)&sv, 0);
}

/* Videomodus der Box: hoechstens einmal je Takt lesen */
static void bmode(unsigned t)
{
    unsigned now = ticks();
    if (t == bm_t && now == bm_tk)
        return;
    bm_t = t;
    bm_tk = now;
    rdb(t, 0x449, &bm[0]);
    rdb(t, 0x484, &bm[1]);
}

static int m_text(unsigned t)
{
    unsigned m;
    bmode(t);
    m = bm[0] & 0x7F;
    return m <= 3 || m == 7;
}

/* Vorgaben nach dem Videomodus der Box (wie der Microsoft-Treiber) */
static void m_mode(unsigned t, struct vmou *o)
{
    unsigned m, rows;
    bm_t = 0xFFFF;                      /* frisch lesen                  */
    bmode(t);
    m = bm[0] & 0x7F;
    rows = bm[1] + 1;
    if (rows < 25 || rows > 50)
        rows = 25;
    o->xmin = o->ymin = 0;
    o->xmax = 639;
    o->cw = m <= 1 ? 16 : 8;
    if (m <= 3 || m == 7)
        o->ymax = rows * 8 - 1;
    else if (m == 0x0F || m == 0x10)
        o->ymax = 349;
    else if (m == 0x11 || m == 0x12 || m > 0x13)
        o->ymax = 479;
    else
        o->ymax = 199;
    o->x = 320;
    o->y = (o->ymax + 1) / 2 & ~7;
}

static void m_clip(struct vmou *o)
{
    if (o->x < o->xmin) o->x = o->xmin;
    if (o->x > o->xmax) o->x = o->xmax;
    if (o->y < o->ymin) o->y = o->ymin;
    if (o->y > o->ymax) o->y = o->ymax;
}

static void m_ptr(unsigned t, struct vmou *o)
{
    if (m_text(t))
        vs(VS_MOUSE, t, VM_POS, o->y >> 3, o->x / o->cw * (o->cw >> 3));
}

static int m_rx(unsigned t, struct vmou *o, int x) { return m_text(t) ? x & ~(o->cw - 1) : x; }
static int m_ry(unsigned t, int y) { return m_text(t) ? y & ~7 : y; }

/* Neues von der Maus holen: Bewegung, dann Tastenereignisse */
static void m_poll(unsigned t, struct vmou *o)
{
    int dx, dy;
    unsigned b, i, ch;
    vs(VS_MOUSE, t, VM_DELTA, 0, 0);
    dx = (int)ev[0];
    dy = (int)ev[1];
    if (dx || dy) {
        o->mcx += dx;
        o->mcy += dy;
        o->fx += dx * 8;
        o->fy += dy * 8;
        o->x += o->fx / (int)o->rx;
        o->fx %= (int)o->rx;
        o->y += o->fy / (int)o->ry;
        o->fy %= (int)o->ry;
        m_clip(o);
        o->evp |= 1;
        m_ptr(t, o);
    }
    while (vs(VS_MOUSE, t, VM_READ, 0, 0) == 1) {
        b = ev[0] & 7;
        ch = b ^ o->btn;
        for (i = 0; i < 3; i++) {
            if (!(ch & (1 << i)))
                continue;
            if (b & (1 << i)) {
                o->npr[i]++;
                o->prx[i] = m_rx(t, o, o->x);
                o->pry[i] = m_ry(t, o->y);
                o->evp |= 2 << (i * 2);
            } else {
                o->nrl[i]++;
                o->rlx[i] = m_rx(t, o, o->x);
                o->rly[i] = m_ry(t, o->y);
                o->evp |= 4 << (i * 2);
            }
        }
        o->btn = (unsigned char)b;
    }
}

static void m_clear(struct vmou *o)
{
    unsigned i;
    for (i = 0; i < sizeof(*o); i++)
        ((unsigned char *)o)[i] = 0;
}

static void m_arm(unsigned t, struct vmou *o)
{
    if (o->mask && (o->cbseg || o->cbofs))
        vs(VS_CBARM, t, o->cbseg, o->cbofs, 0);
    else
        vs(VS_CBARM, t, 0, 0, 0);
}

static void m_reset(unsigned t, struct vmou *o)
{
    m_clear(o);
    vs(VS_CBARM, t, 0, 0, 0);
    o->nbtn = (unsigned char)vs(VS_MOUSE, t, VM_OPEN, 0, 0);
    if (!o->nbtn || o->nbtn == 0xFF)
        return;
    o->on = 1;
    o->show = -1;
    o->rx = 8;
    o->ry = 16;
    m_mode(t, o);
    vs(VS_MOUSE, t, VM_DELTA, 0, 0);    /* Altes verwerfen               */
    vs(VS_MOUSE, t, VM_SHOW, 0, 0);
    m_ptr(t, o);
}

/* ---- PS/2-BIOS ---- */
static void p_clear(struct vps2 *p)
{
    unsigned i;
    for (i = 0; i < sizeof(*p); i++)
        ((unsigned char *)p)[i] = 0;
}

static void p_defaults(struct vps2 *p)
{
    p->en = 0;
    p->rate = 100;
    p->res = 2;
    p->scal = 0;
    p->dx = p->dy = 0;
    p->nq = 0;
}

static void p_arm(unsigned t, struct vps2 *p)
{
    if (p->en && (p->hseg || p->hofs))
        vs(VS_CBARM, t, p->hseg, p->hofs, 1);
    else
        vs(VS_CBARM, t, 0, 0, 0);
}

/* Neues von der Maus: Bewegung aufsummieren, Tastenstaende merken */
static void p_poll(unsigned t, struct vps2 *p)
{
    vs(VS_MOUSE, t, VM_DELTA, 0, 0);
    p->dx += (int)ev[0];
    p->dy += (int)ev[1];
    while (p->nq < sizeof(p->q) && vs(VS_MOUSE, t, VM_READ, 0, 0) == 1)
        p->q[p->nq++] = (unsigned char)(ev[0] & 7);
}

/* Ein Paket: c->ax Status, bx X, cx Y (nach oben positiv), dx Z */
static void p_packet(struct vdd_call __far *c, struct vps2 *p)
{
    int x = p->dx, y = -p->dy;
    unsigned st, i;
    if (x > 255) x = 255;
    if (x < -255) x = -255;
    if (y > 255) y = 255;
    if (y < -255) y = -255;
    p->dx -= x;
    p->dy += y;
    if (p->nq) {
        p->btn = p->q[0];
        for (i = 1; i < p->nq; i++)
            p->q[i - 1] = p->q[i];
        p->nq--;
    }
    st = 0x08 | (p->btn & 1) | ((p->btn & 2) ? 2 : 0) | ((p->btn & 4) ? 4 : 0);
    if (x < 0) st |= 0x10;
    if (y < 0) st |= 0x20;
    c->ax = st;
    c->bx = (unsigned)x & 0xFF;
    c->cx = (unsigned)y & 0xFF;
    c->dx = 0;
}

/* INT 15h C2xxh -> CF (flags Bit 0) und AH */
static void ps2(struct vdd_call __far *c, unsigned t, struct vps2 *p)
{
    unsigned char al = (unsigned char)c->ax, bh = (unsigned char)(c->bx >> 8);
    unsigned err = 0;
    switch (al) {
    case 0x00:                          /* ein/aus                       */
        if (bh > 1)
            err = 1;
        else if (bh && !p->hseg && !p->hofs)
            err = 5;                    /* kein Rueckruf                 */
        else if (bh && !p->en) {
            unsigned char nb = (unsigned char)vs(VS_MOUSE, t, VM_OPEN, 0, 0);
            if (!nb || nb == 0xFF) {
                err = 3;                /* keine Maus in der Sitzung      */
                break;
            }
            vs(VS_MOUSE, t, VM_SHOW, 0, 0);
            vs(VS_MOUSE, t, VM_DELTA, 0, 0);    /* Altes verwerfen       */
            while (vs(VS_MOUSE, t, VM_READ, 0, 0) == 1)
                ;
            p->dx = p->dy = 0;
            p->nq = 0;
            p->en = 1;
            p_arm(t, p);
        } else if (!bh && p->en) {
            p->en = 0;
            p_arm(t, p);
        }
        break;
    case 0x01:                          /* zuruecksetzen                 */
        p_defaults(p);
        p_arm(t, p);
        c->bx = 0x00AA;                 /* BH Kennung 0, BL AAh          */
        break;
    case 0x02:                          /* Abtastrate                    */
        if (bh > 6)
            err = 2;
        else
            p->rate = bh;
        break;
    case 0x03:                          /* Aufloesung                    */
        if (bh > 3)
            err = 2;
        else
            p->res = bh;
        break;
    case 0x04:                          /* Geraetekennung                */
        c->bx &= 0x00FF;
        break;
    case 0x05:                          /* einrichten (Paketgroesse BH)  */
        if (bh < 1 || bh > 8)
            err = 2;
        else {
            p_defaults(p);
            p->init = 1;
            p_arm(t, p);
        }
        break;
    case 0x06:                          /* erweitert                     */
        if (bh == 0) {                  /* Status                        */
            c->bx = (c->bx & 0xFF00) | (p->en ? 0x20 : 0) |
                    (p->scal ? 0x10 : 0) | (p->btn & 1 ? 4 : 0) |
                    (p->btn & 2 ? 1 : 0) | (p->btn & 4 ? 2 : 0);
            c->cx = (c->cx & 0xFF00) | p->res;
            c->dx = (c->dx & 0xFF00) | 100;
        } else if (bh <= 2)
            p->scal = (unsigned char)(bh - 1);
        else
            err = 1;
        break;
    case 0x07:                          /* Rueckruf ES:BX                */
        if (c->val & 1) {               /* aus dem PM ohne Uebersetzung  */
            err = 3;
            break;
        }
        p->hseg = c->es;
        p->hofs = c->bx;
        p_arm(t, p);
        break;
    default:
        err = 1;
    }
    c->ax = (c->ax & 0x00FF) | (err << 8);
    if (err)
        c->flags |= 1;
    else
        c->flags &= ~1u;
}

static void mou33(struct vdd_call __far *c, unsigned t, struct vmou *o)
{
    unsigned i, a = c->ax;
    if (a == 0x0000 || a == 0x0021) {
        m_reset(t, o);
        c->ax = o->on ? 0xFFFF : 0;
        c->bx = o->nbtn == 3 ? 3 : 2;
        return;
    }
    if (!o->on)                         /* kein Treiber                  */
        return;
    m_poll(t, o);
    switch (a) {
    case 0x01:                          /* Zeiger zeigen                 */
        if (o->show < 0 && ++o->show == 0)
            vs(VS_MOUSE, t, VM_SHOW, 1, 0);
        break;
    case 0x02:                          /* Zeiger verbergen              */
        if (o->show-- == 0)
            vs(VS_MOUSE, t, VM_SHOW, 0, 0);
        break;
    case 0x03:
        c->bx = o->btn;
        c->cx = (unsigned)m_rx(t, o, o->x);
        c->dx = (unsigned)m_ry(t, o->y);
        break;
    case 0x04:                          /* Position setzen               */
        o->x = (int)c->cx;
        o->y = (int)c->dx;
        m_clip(o);
        m_ptr(t, o);
        break;
    case 0x05: case 0x06:               /* Druecken/Loslassen            */
        i = c->bx < 3 ? c->bx : 0;
        c->ax = o->btn;
        if (a == 5) {
            c->bx = o->npr[i];
            c->cx = (unsigned)o->prx[i];
            c->dx = (unsigned)o->pry[i];
            o->npr[i] = 0;
        } else {
            c->bx = o->nrl[i];
            c->cx = (unsigned)o->rlx[i];
            c->dx = (unsigned)o->rly[i];
            o->nrl[i] = 0;
        }
        break;
    case 0x07: case 0x08: {             /* Grenzen                       */
        int lo = (int)c->cx, hi = (int)c->dx;
        if (lo > hi) { int k = lo; lo = hi; hi = k; }
        if (a == 7) { o->xmin = lo; o->xmax = hi; }
        else { o->ymin = lo; o->ymax = hi; }
        m_clip(o);
        m_ptr(t, o);
        break;
    }
    case 0x0B:                          /* Mickeys seit dem letzten Mal  */
        c->cx = (unsigned)o->mcx;
        c->dx = (unsigned)o->mcy;
        o->mcx = o->mcy = 0;
        break;
    case 0x0C:                          /* Rueckruf setzen (aus dem PM   */
        o->mask = (c->val & 1) ? 0 : c->cx;     /* ohne Uebersetzung ist  */
        o->cbofs = c->dx;               /* ES:DX keine Real-Mode-Adresse) */
        o->cbseg = c->es;
        o->evp = 0;
        m_arm(t, o);
        break;
    case 0x14:                          /* Rueckruf tauschen             */
        i = o->mask;
        o->mask = (c->val & 1) ? 0 : c->cx;
        c->cx = i;
        i = o->cbofs;
        o->cbofs = c->dx;
        c->dx = i;
        i = o->cbseg;
        o->cbseg = c->es;
        c->es = i;
        o->evp = 0;
        m_arm(t, o);
        break;
    case 0x0F:                          /* Mickeys je 8 Punkte           */
        if (c->cx) o->rx = (unsigned char)(c->cx > 255 ? 255 : c->cx);
        if (c->dx) o->ry = (unsigned char)(c->dx > 255 ? 255 : c->dx);
        break;
    case 0x15:                          /* Zustand: Puffergroesse        */
        c->bx = 0;
        break;
    case 0x1B:                          /* Empfindlichkeit               */
        c->bx = c->cx = 50;
        c->dx = 0;
        break;
    case 0x1D: case 0x1E:               /* Bildschirmseite               */
        c->bx = 0;
        break;
    case 0x1F:                          /* Treiber aus                   */
        c->ax = 0xFFFF;
        break;
    case 0x24:                          /* Version, Typ, IRQ der echten  */
        c->bx = 0x0805;                 /* Maus (PS/2: IRQ 0 wie bei     */
        i = vs(VS_MOUSE, t, VM_INFO, 0, 0);     /* Microsoft)            */
        c->cx = i && i != 0xFFFF ? i : 0x0400;
        break;
    case 0x26:                          /* groesste Koordinaten          */
        c->bx = 0;
        c->cx = (unsigned)o->xmax;
        c->dx = (unsigned)o->ymax;
        break;
    case 0x2A:                          /* Zaehler, heisser Punkt        */
        c->ax = (unsigned)o->show;
        c->bx = c->cx = 0;
        c->dx = 4;
        break;
    }
}

/* INT 33h und die Frage nach dem Rueckruf */
static unsigned int_fn(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task, e;
    struct vmou *o;
    if (t >= NT)
        return VDD_NOTMINE;
    o = &mo[t];
    if (c->fn == VDD_INT) {
        if (c->port == 0x15) {
            if ((c->ax >> 8) != 0xC2)
                return VDD_NOTMINE;
            ps2(c, t, &ps[t]);
        } else
            mou33(c, t, o);
        return 0;
    }
    if (c->fn == VDD_CBQ && ps[t].en) { /* PS/2-Rueckruf                 */
        struct vps2 *p = &ps[t];
        p_poll(t, p);
        if (!p->dx && !p->dy && !p->nq)
            return VDD_NOTMINE;
        if (c->val)
            p_packet(c, p);
        return 0;
    }
    if (c->fn != VDD_CBQ || !o->on || !o->mask)
        return VDD_NOTMINE;
    m_poll(t, o);
    e = o->evp & o->mask;
    if (!e)
        return VDD_NOTMINE;
    if (c->val) {                       /* nehmen                        */
        o->evp = 0;
        c->ax = e;
        c->bx = o->btn;
        c->cx = (unsigned)m_rx(t, o, o->x);
        c->dx = (unsigned)m_ry(t, o->y);
        c->si = (unsigned)o->mcx;
        c->di = (unsigned)o->mcy;
    }
    return 0;
}

static unsigned event(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task;
    if (t >= NT)
        return 0;
    switch (c->fn) {
    case VDD_EV_PMEND:                  /* DPMI zu Ende: ein PS/2-Rueck- */
        if (ps[t].hseg == 0xF000 &&     /* ruf ueber DPMI 0303h (Windows, */
            ps[t].hofs >= 0x700 && ps[t].hofs < 0x800) {  /* F000:07xx) */
            ps[t].en = 0;               /* gilt nicht mehr               */
            ps[t].hseg = ps[t].hofs = 0;
            vs(VS_CBARM, t, 0, 0, 0);
        }
        break;
    case VDD_EV_PROG:                   /* neues Programm: Treiber aus,  */
        vs(VS_MOUSE, t, VM_CLOSE, 0, 0);    /* Maus der Sitzung frei     */
        {   static unsigned char e;     /* Zeigegeraet im Ausstattungs-  */
            rdb(t, 0x410, &e);          /* wort (INT 11h): PS/2-BIOS     */
            if (!(e & 4)) {             /* (INT 15h C2xxh) gibt es hier  */
                e |= 4;
                sv.fn = VS_WR;
                sv.task = t;
                sv.lin = 0x410;
                sv.buf = (unsigned)&e;
                sv.len = 1;
                devhlp(DH_VDDSVC, 0, (unsigned)&sv, 0);
            }
        }
        /* fallthrough */
    case VDD_EV_CREATE:
    case VDD_EV_PARK:
    case VDD_EV_DESTROY:                /* (Kernel gibt die Maus frei)   */
        m_clear(&mo[t]);
        p_clear(&ps[t]);
        break;
    }
    return 0;
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    reg.version = VDD_VERSION;
    reg.size = sizeof(reg);
    scat(reg.name, "VMOUSE");
    reg.int_off = fent(0, int_fn);
    reg.ev_off = fent(1, event);
    reg.ints[0] = 0x33;
    reg.ints[1] = 0x15;                 /* nur C2xxh (Kernel fragt)      */
    if (devhlp(DH_VDDREG, 0, (unsigned)&reg, 0)) {
        dputs("VMOUSE.SYS: Anmeldung gescheitert\n");
        r->status = DRV_ERR_NODEV;
        return;
    }
    dputs("  Maus der DOS-Boxen (INT 33h, PS/2-BIOS)\n");
    r->units = 0;
    r->status = DRV_OK;
}

void strategy(void)
{
    struct drv_req __far *r = REQ;
    unsigned t;
    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    case DRV_DEINIT:                    /* DEVLOAD /U: Maus der Boxen frei */
        for (t = 0; t < NT; t++)
            if (mo[t].on) {
                vs(VS_MOUSE, t, VM_CLOSE, 0, 0);
                vs(VS_CBARM, t, 0, 0, 0);
                m_clear(&mo[t]);
            }
        for (t = 0; t < NT; t++)
            if (ps[t].en) {
                vs(VS_MOUSE, t, VM_CLOSE, 0, 0);
                vs(VS_CBARM, t, 0, 0, 0);
                p_clear(&ps[t]);
            }
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
