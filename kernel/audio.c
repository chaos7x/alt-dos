/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.x - Ton (FAPI_AUDIO), aus kernel.c nach KTEXT2 verlegt
 * (27.11.2026: das erste Codesegment war voll). Kernelhilfen ueber k2.h.
 */
#include "kern.h"
#include "../include/fapi_abi.h"
#include "mem.h"
#include "ifs.h"
#include "../include/drv.h"
#include "k2.h"                         /* KTEXT2, k2.h zuletzt           */

extern int current;
int __far kf_user_ok(unsigned sel, unsigned off, unsigned len, int write);
void __far kf_aud_drvcall(int d, unsigned q);       /* kernel.c: drv_call  */
int __far kf_task_ses(int t);
#define user_ok(s, o, l, w) kf_user_ok((s), (o), (l), (w))

/* ---- Ton (FAPI_AUDIO) --------------------------------------------------
   Mehrere Programme duerfen das Audiogeraet oeffnen (bis NAUD). Zu hoeren
   ist das Programm der Vordergrund-Sitzung, sonst das zuletzt geoeffnete
   (Musik im Hintergrund, waehrend man in EDIT schreibt). Wer nicht zu
   hoeren ist, laeuft im Echtzeittempo weiter: seine Daten werden nach
   Laenge / Bytes je Takt verworfen - ein Spiel bleibt im Takt und ist
   nach dem Zurueckholen sofort wieder zu hoeren. Der Kernel entscheidet
   bei jedem Schreiben, der Treiber spielt einen Strom (aud_cur). */
#define NAUD 4
static struct {
    signed char   task;                 /* -1 = frei                      */
    unsigned      rate, bpt, seq;       /* bpt: Bytes je Takt (10 ms)     */
    unsigned char ch, bits;
} aud[NAUD] = { { -1 }, { -1 }, { -1 }, { -1 } };
static int aud_drv = -1, aud_cur = -1;
static unsigned aud_seq;
static struct aud_req areq[NAUD + 1];   /* je Programm eine, [NAUD] fuer
                                           START/STOP/MIXER (ein schlafender
                                           Schreiber behaelt seine)      */

static unsigned aud_call(int r, unsigned fn, unsigned a1, unsigned a2, unsigned a3)
{
    struct aud_req *q = &areq[r];
    q->cmd = DRV_AUDIO;
    q->fn = (unsigned char)fn;
    q->status = DRV_ERR_CMD;
    q->a1 = a1;
    q->a2 = a2;
    q->a3 = a3;
    q->result = 0;
    kf_aud_drvcall(aud_drv, (unsigned)q);
    return q->status;
}

static int aud_find(int t)
{
    int i;
    for (i = 0; i < NAUD; i++)
        if (aud[i].task == t)
            return i;
    return -1;
}

/* wer ist zu hoeren? Vordergrund, sonst der zuletzt geoeffnete */
static int aud_pick(void)
{
    int i, best = -1, fg = -1, fgs = vio_fg();
    for (i = 0; i < NAUD; i++) {
        if (aud[i].task < 0)
            continue;
        if (kf_task_ses(aud[i].task) == fgs && (fg < 0 || aud[i].seq > aud[fg].seq))
            fg = i;
        if (best < 0 || aud[i].seq > aud[best].seq)
            best = i;
    }
    return fg >= 0 ? fg : best;
}

static void aud_stop(void)
{
    if (aud_cur >= 0)
        aud_call(NAUD, AR_STOP, 0, 0, 0);
    aud_cur = -1;
}

static void aud_close(int i)
{
    if (aud_cur == i)
        aud_stop();
    aud[i].task = -1;
}

#define NFM 4
struct aclient {
    signed char   task;                 /* -1 = frei                      */
    unsigned      seq;
    unsigned char rs, need, d1;         /* MIDI: laufender Status         */
    unsigned char prog[16], vol[16];
};
static struct aclient fmc[NFM] = { { -1 }, { -1 }, { -1 }, { -1 } },
                      mdc[NFM] = { { -1 }, { -1 }, { -1 }, { -1 } };
static int fm_cur = -1, md_cur = -1;
static unsigned fm_sel;                 /* NFM * 512 Byte Registerkopien  */
static struct fapi_fmreg fm_buf[64];

static int cl_find(struct aclient *c, int t)
{
    int i;
    for (i = 0; i < NFM; i++)
        if (c[i].task == t)
            return i;
    return -1;
}

/* wer ist zu hoeren? Vordergrund, sonst der zuletzt geoeffnete */
static int cl_pick(struct aclient *c)
{
    int i, best = -1, fg = -1, fgs = vio_fg();
    for (i = 0; i < NFM; i++) {
        if (c[i].task < 0)
            continue;
        if (kf_task_ses(c[i].task) == fgs && (fg < 0 || c[i].seq > c[fg].seq))
            fg = i;
        if (best < 0 || c[i].seq > c[best].seq)
            best = i;
    }
    return fg >= 0 ? fg : best;
}

void __far aud_task_end(int t)
{
    int i = aud_find(t);
    if (i >= 0)
        aud_close(i);
    if (aud_drv < 0)
        return;
    if ((i = cl_find(fmc, t)) >= 0) {
        if (fm_cur == i) {
            aud_call(NAUD, AR_FMRESET, 0, 0, 0);
            fm_cur = -1;
        }
        fmc[i].task = -1;
    }
    if ((i = cl_find(mdc, t)) >= 0) {
        if (md_cur == i) {
            aud_call(NAUD, AR_MIDIRESET, 0, 0, 0);
            md_cur = -1;
        }
        mdc[i].task = -1;
    }
}

/* FM und MIDI: dieselbe Vorrangregel. FM: der Kernel haelt je Programm
   alle 512 OPL-Register (Kernelsegment), beim Wechsel laedt er die des
   neuen Programms (Tasten los). MIDI: je Kanal Programm und Lautstaerke,
   beim Wechsel alle Noten aus und den Stand des neuen Programms. */
static unsigned char __far *fm_shadow(int i)
{
    return MK_FP(fm_sel, (unsigned)i * 512u);
}

/* Register des Programms i in den OPL laden (B0h-B8h ohne Taste) */
static void fm_load(int i)
{
    unsigned char __far *sh = fm_shadow(i);
    struct aud_req *q = &areq[NAUD];
    unsigned r, n = 0;
    static const unsigned first[4] = { 0x105, 0x104, 0x001, 0x008 };

    aud_call(NAUD, AR_FMRESET, 0, 0, 0);
    for (r = 0; r < 4 + 2 * 0xD6; r++) {
        unsigned reg = r < 4 ? first[r] : ((r - 4) < 0xD6 ? 0x20 + (r - 4) : 0x120 + (r - 4 - 0xD6));
        unsigned char v = sh[reg], lo = (unsigned char)reg;
        if (lo >= 0xB0 && lo <= 0xB8)
            v &= ~0x20;
        if (lo >= 0xD0 && lo < 0xE0)
            v = sh[reg - 0x10];         /* gibt es nicht - QEMU nimmt es als
                                           C0h-C8h: denselben Wert schreiben */
        fm_buf[n].reg = reg;
        fm_buf[n].val = v;
        if (++n == 64 || r == 4 + 2 * 0xD6 - 1) {
            q->sel = KDATA;
            q->off = (unsigned)fm_buf;
            q->len = n;
            aud_call(NAUD, AR_FMWRITE, 0, 0, 0);
            n = 0;
        }
    }
}

/* MIDI-Strom mitlesen: Programm (Cn) und Lautstaerke (Bn 07) je Kanal */
static void md_track(int i, unsigned char c)
{
    if (c >= 0xF8)
        return;                         /* Echtzeit                      */
    if (c & 0x80) {
        mdc[i].rs = c >= 0xF0 ? 0 : c;
        mdc[i].need = (unsigned char)(((c & 0xF0) == 0xC0 || (c & 0xF0) == 0xD0) ? 1 : 2);
        mdc[i].d1 = 0xFF;
        return;
    }
    if (!mdc[i].rs)
        return;
    if (mdc[i].need == 1) {
        if ((mdc[i].rs & 0xF0) == 0xC0)
            mdc[i].prog[mdc[i].rs & 15] = c;
        return;
    }
    if (mdc[i].d1 == 0xFF) {
        mdc[i].d1 = c;
        return;
    }
    if ((mdc[i].rs & 0xF0) == 0xB0 && mdc[i].d1 == 7)
        mdc[i].vol[mdc[i].rs & 15] = c;
    mdc[i].d1 = 0xFF;                   /* naechstes Paar (laufender Status) */
}

static void md_load(int i)
{
    static unsigned char m[16 * 5];
    struct aud_req *q = &areq[NAUD];
    unsigned k, n = 0;
    aud_call(NAUD, AR_MIDIRESET, 0, 0, 0);
    for (k = 0; k < 16; k++) {
        m[n++] = (unsigned char)(0xC0 | k);
        m[n++] = mdc[i].prog[k];
        m[n++] = (unsigned char)(0xB0 | k);
        m[n++] = 7;
        m[n++] = mdc[i].vol[k];
    }
    q->sel = KDATA;
    q->off = (unsigned)m;
    q->len = n;
    aud_call(NAUD, AR_MIDIWRITE, 0, 0, 0);
}

static unsigned aud_caps(void)
{
    aud_call(NAUD, AR_CAPS, 0, 0, 0);
    return areq[NAUD].result;
}

unsigned __far aud_fapi(unsigned usel, unsigned fn, unsigned a, unsigned b)
{
    int i = aud_find(current), w;
    struct aud_req *q;

    if (aud_drv < 0)
        return FAPI_ERROR;
    switch (fn) {
    case AUD_CAPS:
        return aud_caps();
    case AUD_FMOPEN:
    case AUD_MIDIOPEN: {
        int fm = fn == AUD_FMOPEN, k;
        if (!(aud_caps() & (fm ? (AUDCAP_OPL2 | AUDCAP_OPL3) : AUDCAP_MPU401)))
            return FAPI_ERROR;
        if (fm && !fm_sel && !(fm_sel = kseg_alloc(NFM * 512UL)))
            return FAPI_ERR_FULL;       /* kein Kernelsegment frei        */
        i = cl_find(fm ? fmc : mdc, current);
        if (i < 0)
            for (i = 0; i < NFM && (fm ? fmc[i].task : mdc[i].task) >= 0; i++)
                ;
        if (i == NFM)
            return FAPI_ERR_INUSE;
        if (fm) {
            unsigned char __far *sh = fm_shadow(i);
            for (k = 0; k < 512; k++)
                sh[k] = (unsigned char)(((k & 0xFF) >= 0x40 && (k & 0xFF) <= 0x55) ? 0x3F : 0);
            fmc[i].task = (signed char)current;
            fmc[i].seq = ++aud_seq;
            if (fm_cur == i)
                fm_cur = -1;
        } else {
            for (k = 0; k < 16; k++) {
                mdc[i].prog[k] = 0;
                mdc[i].vol[k] = 100;
            }
            mdc[i].rs = 0;
            mdc[i].task = (signed char)current;
            mdc[i].seq = ++aud_seq;
            if (md_cur == i)
                md_cur = -1;
        }
        return 0;
    }
    case AUD_FMWRITE: {
        struct fapi_fmreg __far *f = MK_FP(usel, a);
        unsigned k;
        i = cl_find(fmc, current);
        if (i < 0 || b > 512 || !user_ok(usel, a, b * sizeof(*f), 0))
            return FAPI_ERROR;
        for (k = 0; k < b; k++)
            fm_shadow(i)[f[k].reg & 0x1FF] = f[k].val;
        w = cl_pick(fmc);
        if (w == i && fm_cur != i) {
            fm_load(i);                 /* Stand laden (Tasten los) ...  */
            fm_cur = i;
        }
        if (fm_cur == i) {              /* ... und diesen Auftrag dazu   */
            q = &areq[i];
            q->sel = usel;
            q->off = a;
            q->len = b;
            aud_call(i, AR_FMWRITE, 0, 0, 0);
        }
        return 0;
    }
    case AUD_MIDIWRITE: {
        unsigned char __far *m = MK_FP(usel, a);
        unsigned k;
        i = cl_find(mdc, current);
        if (i < 0 || !user_ok(usel, a, b, 0))
            return FAPI_ERROR;
        for (k = 0; k < b; k++)
            md_track(i, m[k]);
        w = cl_pick(mdc);
        if (w == i && md_cur != i) {
            md_load(i);
            md_cur = i;
        }
        if (md_cur == i) {
            q = &areq[i];
            q->sel = usel;
            q->off = a;
            q->len = b;
            aud_call(i, AR_MIDIWRITE, 0, 0, 0);
        }
        return 0;
    }
    case AUD_FMCLOSE:
    case AUD_MIDICLOSE: {
        int fm = fn == AUD_FMCLOSE;
        i = cl_find(fm ? fmc : mdc, current);
        if (i < 0)
            return FAPI_ERROR;
        if (fm) {
            if (fm_cur == i) {
                aud_call(NAUD, AR_FMRESET, 0, 0, 0);
                fm_cur = -1;
            }
            fmc[i].task = -1;
        } else {
            if (md_cur == i) {
                aud_call(NAUD, AR_MIDIRESET, 0, 0, 0);
                md_cur = -1;
            }
            mdc[i].task = -1;
        }
        return 0;
    }
    case AUD_AWE:                               /* Klangbank (AWELOAD)   */
        if (b < sizeof(struct fapi_awecmd) || !user_ok(usel, a, b, 1))
            return FAPI_ERROR;
        q = &areq[NAUD];
        q->sel = usel;
        q->off = a;
        q->len = b;
        return aud_call(NAUD, AR_AWE, 0, 0, 0) == DRV_OK ? 0 : FAPI_ERROR;
    case AUD_OPEN: {
        struct fapi_audfmt __far *f = MK_FP(usel, a);
        unsigned bytes;
        if (!user_ok(usel, a, sizeof(*f), 0))
            return FAPI_ERROR;
        if (i >= 0)
            aud_close(i);
        for (i = 0; i < NAUD && aud[i].task >= 0; i++)
            ;
        if (i == NAUD)
            return FAPI_ERR_INUSE;
        if (aud_call(NAUD, AR_CHECK, f->rate, f->channels, f->bits) != DRV_OK)
            return FAPI_ERROR;
        bytes = (unsigned)f->channels * (f->bits >> 3);
        aud[i].rate = f->rate;
        aud[i].ch = f->channels;
        aud[i].bits = f->bits;
        aud[i].bpt = (f->rate / 100) * bytes;
        aud[i].seq = ++aud_seq;
        aud[i].task = (signed char)current;
        return 0;
    }
    case AUD_WRITE:
        if (i < 0 || !user_ok(usel, a, b, 0))
            return FAPI_ERROR;
        w = aud_pick();
        if (w == i && aud_cur != i) {           /* wechseln              */
            aud_stop();
            if (aud_call(NAUD, AR_START, aud[i].rate, aud[i].ch, aud[i].bits) == DRV_OK)
                aud_cur = i;
        }
        if (aud_cur == i) {
            q = &areq[i];
            q->sel = usel;
            q->off = a;
            q->len = b;
            aud_call(i, AR_WRITE, 0, 0, 0);
            return b;                           /* Rest ging beim Wechsel unter */
        }
        if (aud[i].bpt)                         /* stumm: Echtzeit abwarten */
            sleep_ticks(b / aud[i].bpt + 1);
        return b;
    case AUD_DRAIN:
        if (i >= 0 && aud_cur == i)
            aud_call(i, AR_DRAIN, 0, 0, 0);
        return i >= 0 ? 0 : FAPI_ERROR;
    case AUD_CLOSE:
        if (i < 0)
            return FAPI_ERROR;
        aud_close(i);
        return 0;
    case AUD_MIXER:
        if (aud_call(NAUD, AR_MIXER, a, b, 0) != DRV_OK)
            return FAPI_ERROR;
        return areq[NAUD].result;
    case AUD_INFO:
        if (!user_ok(usel, a, 64, 1))
            return FAPI_ERROR;
        q = &areq[NAUD];
        q->sel = usel;
        q->off = a;
        return aud_call(NAUD, AR_INFO, 0, 0, 0) == DRV_OK ? 0 : FAPI_ERROR;
    case AUD_BLASTER:
        if (!user_ok(usel, a, 64, 1))
            return FAPI_ERROR;
        q = &areq[NAUD];
        q->sel = usel;
        q->off = a;
        return aud_call(NAUD, AR_BLASTER, 0, 0, 0) == DRV_OK ? 0 : FAPI_ERROR;
    }
    return FAPI_ERROR;
}

/* Treiberlader (kernel.c): Tontreiber anmelden, in Benutzung?, entladen */
int __far aud_reg(int d)
{
    aud_drv = d;
    return d < 0;
}

int __far aud_busy(int d)
{
    int k;
    if (aud_drv != d)
        return 0;
    for (k = 0; k < NFM; k++)           /* NAUD == NFM                   */
        if (aud[k].task >= 0 || fmc[k].task >= 0 || mdc[k].task >= 0)
            return 1;
    return 0;
}

void __far aud_gone(int d)
{
    if (aud_drv == d)
        aud_drv = aud_cur = fm_cur = md_cur = -1;
}
