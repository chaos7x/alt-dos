/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SB16.SYS - Sound Blaster (Wiedergabe und Mixer) fuer FAPI_AUDIO
 *
 * CONFIG.SYS:  DEVICE=SB16.SYS [/A:220] [/I:5] [/D:1] [/H:5]
 *
 * Ohne Angabe sucht der Treiber an 220h, 240h, 260h, 280h (erst lesen:
 * liefert die Adresse nur FFh, sitzt dort nichts) und setzt den DSP
 * zurueck (Antwort AAh). Beim SB16 (DSP 4.xx) stehen IRQ und DMA in den
 * Mixer-Registern 80h/81h; aeltere Karten (DSP 2.xx/3.xx) nehmen /I und
 * /D oder 5 und 1.
 *
 * Wiedergabe ueber Auto-Init-DMA in einem 16-KB-Puffer aus zwei Haelften:
 * der Schreiber fuellt die Haelfte, die gerade nicht spielt; ist sie beim
 * Interrupt noch nicht voll, wird der Rest mit Stille aufgefuellt (kein
 * Knacken durch alte Daten). SB16: 8 und 16 Bit, mono und stereo, 5000-
 * 44100 Hz (16 Bit ueber den 16-Bit-DMA-Kanal); DSP 2/3: 8 Bit mono bis
 * 22050 Hz ueber die Zeitkonstante. Mixer: SB16 (30h-3Ah) und SB Pro.
 *
 * Gebaut mit -zu wie alle Treiber: SS ist der Kernelstack, also keine
 * Adressen lokaler Variablen weitergeben.
 */
#include "drvlib.h"
#include "../include/fapi_abi.h"

#define BUFSZ   16384u
#define HALF    half            /* Haelfte des gespielten Puffers */
#define EV_AUD  1

#define AREQ ((struct aud_req __far *)MK_FP(req_sel, req_off))

void far_cpy(void __far *dst, const void __far *src, unsigned n);
#pragma aux far_cpy = "push ds" "mov ds,dx" "rep movsb" "pop ds" \
    __parm [__es __di] [__dx __si] [__cx] __modify [__di __si __cx];
void far_set(void __far *dst, unsigned char v, unsigned n);
#pragma aux far_set = "rep stosb" __parm [__es __di] [__al] [__cx] __modify [__di __cx];
/* 32 / 16 Bit -> 16 Bit (Quotient muss passen) */
unsigned div32(unsigned long a, unsigned b);
#pragma aux div32 = "div bx" __parm [__dx __ax] [__bx] __value [__ax] __modify [__dx];

static unsigned base, irq = 5, dma8 = 1, dma16 = 5, dspv;
static unsigned caps;                   /* AUDCAP_*                      */
static void opl_w(unsigned reg, unsigned char v);
static void single_block(void);
static void opl_reset(void);
static void midi_w(unsigned char v);
static unsigned bsel;
static unsigned long bphys;
static char line[128], name[64];

/* Wiedergabe */
static unsigned rate, chans, bits16;
static unsigned half = BUFSZ / 2;      /* AUD_SMALL: kurz (DOS-Boxen)   */
static volatile unsigned char playing, fill, fill_full, cur_dma16;
static volatile unsigned fill_off, irq_count, gen;
static unsigned char silence;

/* ---- DSP und Mixer -------------------------------------------------- */
static int dsp_w(unsigned char v)
{
    unsigned n;
    for (n = 0; n < 60000u; n++)
        if (!(inp(base + 0x0C) & 0x80)) {
            outp(base + 0x0C, v);
            return 0;
        }
    return -1;
}

static int dsp_r(void)
{
    unsigned n;
    for (n = 0; n < 60000u; n++)
        if (inp(base + 0x0E) & 0x80)
            return inp(base + 0x0A);
    return -1;
}

static int dsp_reset(unsigned b)
{
    unsigned t, k;
    base = b;
    outp(b + 6, 1);
    for (k = 0; k < 16; k++)            /* mindestens 3 us              */
        inp(b + 6);
    outp(b + 6, 0);
    t = devhlp(DH_TICKS, 0, 0, 0);
    while ((unsigned)(devhlp(DH_TICKS, 0, 0, 0) - t) < 3) {
        if ((inp(b + 0x0E) & 0x80) && inp(b + 0x0A) == 0xAA)
            return 0;
    }
    return -1;
}

static void mix_w(unsigned char reg, unsigned char v)
{
    outp(base + 4, reg);
    outp(base + 5, v);
}

static unsigned char mix_r(unsigned char reg)
{
    outp(base + 4, reg);
    return inp(base + 5);
}

/* ---- DMA (8237) ----------------------------------------------------- */
static const unsigned char page8[4] = { 0x87, 0x83, 0x81, 0x82 };
static const unsigned char page16[4] = { 0x8F, 0x8B, 0x89, 0x8A };

static void dma_start(void)
{
    unsigned c;
    if (cur_dma16) {                    /* Kanaele 5-7: Worte, 128 KB    */
        unsigned long w = bphys >> 1;
        c = dma16 & 3;
        outp(0xD4, (unsigned char)(4 | c));     /* maskieren             */
        outp(0xD8, 0);                          /* Flipflop              */
        outp(0xD6, (unsigned char)(0x58 | c));  /* Auto-Init, Speicher->Karte */
        outp(0xC0 + c * 4, (unsigned char)w);
        outp(0xC0 + c * 4, (unsigned char)(w >> 8));
        outp(page16[c], (unsigned char)((bphys >> 16) & 0xFE));
        outp(0xC2 + c * 4, (unsigned char)((HALF - 1) & 0xFF));
        outp(0xC2 + c * 4, (unsigned char)((HALF - 1) >> 8));
        outp(0xD4, (unsigned char)c);           /* frei                  */
    } else {
        c = dma8 & 3;
        outp(0x0A, (unsigned char)(4 | c));
        outp(0x0C, 0);
        outp(0x0B, (unsigned char)(0x58 | c));
        outp(c * 2, (unsigned char)bphys);
        outp(c * 2, (unsigned char)(bphys >> 8));
        outp(page8[c], (unsigned char)(bphys >> 16));
        outp(c * 2 + 1, (unsigned char)((2 * HALF - 1) & 0xFF));
        outp(c * 2 + 1, (unsigned char)((2 * HALF - 1) >> 8));
        outp(0x0A, (unsigned char)c);
    }
}

static void dma_stop(void)
{
    if (cur_dma16)
        outp(0xD4, (unsigned char)(4 | (dma16 & 3)));
    else
        outp(0x0A, (unsigned char)(4 | (dma8 & 3)));
}

/* ---- Wiedergabe ----------------------------------------------------- */
/* DSP 1.xx (Sound Blaster 1.0): eine Haelfte als Einzelblock; der 8237
   laeuft im Auto-Init weiter, der Interrupt startet den naechsten Block */
static void single_block(void)
{
    dsp_w(0x14);
    dsp_w((unsigned char)((HALF - 1) & 0xFF));
    dsp_w((unsigned char)((HALF - 1) >> 8));
}

static void dsp_start(void)
{
    unsigned n;
    dsp_w(0xD1);                        /* Lautsprecher an (aeltere DSP) */
    if (dspv >= 0x400) {
        n = (bits16 ? HALF / 2 : HALF) - 1;     /* Werte je Haelfte - 1  */
        dsp_w(0x41);
        dsp_w((unsigned char)(rate >> 8));
        dsp_w((unsigned char)rate);
        dsp_w(bits16 ? 0xB6 : 0xC6);    /* Ausgabe, Auto-Init, FIFO      */
        dsp_w((unsigned char)((chans == 2 ? 0x20 : 0) | (bits16 ? 0x10 : 0)));
        dsp_w((unsigned char)n);
        dsp_w((unsigned char)(n >> 8));
    } else {
        dsp_w(0x40);                    /* Zeitkonstante                 */
        dsp_w((unsigned char)(256 - div32(1000000UL, rate)));
        if (dspv < 0x200)               /* DSP 1.xx: kein Auto-Init -    */
            single_block();             /* Block fuer Block (14h)        */
        else {
            dsp_w(0x48);                /* Blockgroesse                  */
            dsp_w((unsigned char)((HALF - 1) & 0xFF));
            dsp_w((unsigned char)((HALF - 1) >> 8));
            dsp_w(0x1C);                /* 8 Bit, Auto-Init              */
        }
    }
}

static void start_play(void)
{
    cur_dma16 = (unsigned char)(bits16 && dspv >= 0x400);
    dma_start();
    dsp_start();
    playing = 1;
    fill = 1;
    fill_off = 0;
    fill_full = 0;
}

static void stop_play(void)
{
    if (playing) {
        if (dspv >= 0x400) {
            dsp_w(cur_dma16 ? 0xD9 : 0xDA);     /* Auto-Init beenden     */
            dsp_w(cur_dma16 ? 0xD5 : 0xD0);     /* anhalten              */
        } else {
            /* vor dem SB16: DAh (Auto-Init beenden) gibt es erst ab DSP
               2.01, D0h haelt Auto-Init bei DSP 2.00 nicht an (Sound
               Blaster 1.5 spielte in 86Box endlos weiter) - sicher ist
               nur ein Reset des DSP (Creative-Handbuch) */
            dsp_w(0xD0);
            dsp_reset(base);
        }
        dma_stop();
    }
    playing = 0;
    fill = 0;
    fill_off = 0;
    fill_full = 0;
    gen++;
    proc_run(EV_AUD);
}

static void sb_irq(unsigned n)
{
    unsigned char st;
    (void)n;
    if (dspv >= 0x400) {
        st = mix_r(0x82);               /* welcher Interrupt?            */
        if (st & 1)
            inp(base + 0x0E);
        if (st & 2)
            inp(base + 0x0F);
    } else
        inp(base + 0x0E);
    if (!playing)
        return;
    if (dspv < 0x200)
        single_block();                 /* DSP 1.xx: naechster Block     */
    /* eine Haelfte ist fertig, die DMA spielt jetzt "fill": nicht voll
       geworden -> Rest Stille (sonst klingen alte Daten nach) */
    if (!fill_full)
        far_set(MK_FP(bsel, fill * HALF + fill_off), silence, HALF - fill_off);
    fill ^= 1;
    fill_off = 0;
    fill_full = 0;
    irq_count++;
    proc_run(EV_AUD);
}

static void aud_write(struct aud_req __far *q)
{
    unsigned done = 0, n, my = gen;
    unsigned char __far *src = MK_FP(q->sel, q->off);

    while (done < q->len) {
        if (gen != my)
            break;                      /* anderer Strom ist dran        */
        cli();
        if (fill_full && playing) {     /* warten, bis eine Haelfte frei ist */
            if (proc_block(EV_AUD, 100))
                if (fill_full && gen == my)
                    break;              /* Karte steht: aufgeben         */
            continue;
        }
        sti();
        n = HALF - fill_off;
        if (n > q->len - done)
            n = q->len - done;
        far_cpy(MK_FP(bsel, fill * HALF + fill_off), src + done, n);
        done += n;
        cli();
        fill_off += n;
        if (fill_off == HALF) {
            if (!playing)
                start_play();           /* erste Haelfte voll: los       */
            else
                fill_full = 1;
        }
        sti();
    }
    sti();
    q->result = done;
}

static void aud_drain(void)
{
    unsigned my = gen, want, t;
    cli();
    if (!playing) {
        if (fill_off == 0) {
            sti();
            return;
        }
        far_set(MK_FP(bsel, fill_off), silence, HALF - fill_off);
        start_play();
        want = irq_count + 1;
    } else if (fill_off > 0 || fill_full) {
        if (!fill_full)
            far_set(MK_FP(bsel, fill * HALF + fill_off), silence, HALF - fill_off);
        fill_full = 1;
        want = irq_count + 2;
    } else
        want = irq_count + 1;
    sti();
    t = devhlp(DH_TICKS, 0, 0, 0);
    for (;;) {
        cli();
        if ((int)(irq_count - want) >= 0 || gen != my)
            break;
        if ((unsigned)(devhlp(DH_TICKS, 0, 0, 0) - t) > 300)
            break;                      /* hoechstens 3 s                */
        proc_block(EV_AUD, 50);
    }
    sti();
    if (gen == my)
        stop_play();
}

static int fmt_ok(unsigned r, unsigned c, unsigned b)
{
    b &= ~AUD_SMALL;
    if (dspv >= 0x400)
        return r >= 5000 && r <= 44100 && (c == 1 || c == 2) &&
               (b == 8 || (b == 16 && dma16 >= 5));
    return r >= 4000 && r <= 22050 && c == 1 && b == 8;
}

/* ---- Mixer ---------------------------------------------------------- */
static const unsigned char sb16reg[6] = { 0x30, 0x32, 0x34, 0x36, 0x38, 0x3A };
static const unsigned char sbproreg[6] = { 0x22, 0x04, 0x26, 0x28, 0x2E, 0x00 };

static int mixer(unsigned ch, unsigned v, unsigned *res)
{
    unsigned char r, x;
    if (ch > 5)
        return -1;
    if (dspv >= 0x400) {
        r = sb16reg[ch];
        if (v != 0xFFFF) {
            x = (unsigned char)(((v > 100 ? 100 : v) * 31 + 50) / 100 << 3);
            mix_w(r, x);
            if (ch != AUDMIX_MIC)
                mix_w((unsigned char)(r + 1), x);
        }
        *res = ((mix_r(r) >> 3) * 100 + 15) / 31;
        return 0;
    }
    if (dspv >= 0x300 && sbproreg[ch]) {    /* SB Pro: 4 Bit je Seite    */
        r = sbproreg[ch];
        if (v != 0xFFFF) {
            x = (unsigned char)(((v > 100 ? 100 : v) * 15 + 50) / 100);
            mix_w(r, (unsigned char)((x << 4) | x));
        }
        *res = ((mix_r(r) >> 4) * 100 + 7) / 15;
        return 0;
    }
    return -1;
}

static unsigned mix_res;

static void audio(void)
{
    struct aud_req __far *q = AREQ;
    unsigned k;

    q->status = DRV_OK;
    if (!(caps & AUDCAP_PCM) && q->fn <= AR_STOP) {
        q->status = DRV_ERR_CMD;
        return;
    }
    switch (q->fn) {
    case AR_CHECK:
        if (!fmt_ok(q->a1, q->a2, q->a3))
            q->status = DRV_ERR_CMD;
        break;
    case AR_START:
        stop_play();
        if (!fmt_ok(q->a1, q->a2, q->a3)) {
            q->status = DRV_ERR_CMD;
            break;
        }
        rate = q->a1;
        chans = q->a2;
        bits16 = (q->a3 & ~AUD_SMALL) == 16;
        half = BUFSZ / 2;
        if (q->a3 & AUD_SMALL) {        /* etwa 1/16 s je Haelfte       */
            half = (rate >> 4) * chans << bits16;
            half = (half + 15) & ~15u;
            if (half < 512)
                half = 512;
            if (half > BUFSZ / 2)
                half = BUFSZ / 2;
        }
        silence = bits16 ? 0 : 0x80;
        break;
    case AR_WRITE:
        aud_write(q);
        break;
    case AR_DRAIN:
        aud_drain();
        break;
    case AR_STOP:
        stop_play();
        break;
    case AR_MIXER:
        cli();                          /* Indexregister teilt sich der IRQ */
        if (mixer(q->a1, q->a2, &mix_res))
            q->status = DRV_ERR_CMD;
        sti();
        q->result = mix_res;
        break;
    case AR_CAPS:
        q->result = caps;
        break;
    case AR_FMWRITE: {
        struct fapi_fmreg __far *f = MK_FP(q->sel, q->off);
        if (!(caps & (AUDCAP_OPL2 | AUDCAP_OPL3))) {
            q->status = DRV_ERR_CMD;
            break;
        }
        for (k = 0; k < q->len; k++)
            if (f[k].reg < 0x100 || (caps & AUDCAP_OPL3))
                opl_w(f[k].reg & 0x1FF, f[k].val);
        break;
    }
    case AR_FMRESET:
        if (caps & (AUDCAP_OPL2 | AUDCAP_OPL3))
            opl_reset();
        break;
    case AR_MIDIWRITE: {
        unsigned char __far *m = MK_FP(q->sel, q->off);
        if (!(caps & AUDCAP_MPU401)) {
            q->status = DRV_ERR_CMD;
            break;
        }
        for (k = 0; k < q->len; k++)
            midi_w(m[k]);
        break;
    }
    case AR_MIDIRESET:
        if (caps & AUDCAP_MPU401)
            for (k = 0; k < 16; k++) {
                midi_w((unsigned char)(0xB0 | k));
                midi_w(123);            /* alle Noten aus                */
                midi_w(0);
                midi_w(121);            /* Controller zurueck            */
                midi_w(0);
            }
        break;
    case AR_INFO: {
        char __far *d = MK_FP(q->sel, q->off);
        for (k = 0; name[k] && k < 63; k++)
            d[k] = name[k];
        d[k] = 0;
        break;
    }
    default:
        q->status = DRV_ERR_CMD;
    }
}

/* ---- Initialisierung ------------------------------------------------ */
static unsigned hexval(const char __far *p)
{
    unsigned v = 0;
    for (;; p++) {
        if (*p >= '0' && *p <= '9') v = v * 16 + (*p - '0');
        else if (*p >= 'a' && *p <= 'f') v = v * 16 + (*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'F') v = v * 16 + (*p - 'A' + 10);
        else return v;
    }
}

static unsigned decval(const char __far *p)
{
    unsigned v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return v;
}

/* Argument /X:wert oder (wie BLASTER) Xwert suchen -> Zeiger auf den Wert */
static const char __far *arg(const char __far *s, char key)
{
    const char __far *a;
    for (a = s; *a; a++)
        if ((a[0] == key || a[0] == key + 32) &&
            (a == s || a[-1] == ' ' || a[-1] == '/' || a[-1] == '\t')) {
            a++;
            if (*a == ':' || *a == '=')
                a++;
            return a;
        }
    return 0;
}

static int init_pcm(struct drv_req __far *r)
{
    static const unsigned ports[4] = { 0x220, 0x240, 0x260, 0x280 };
    const char __far *a;
    unsigned i, b = 0, maj, min;
    unsigned char m;
    char *p;

    if ((a = arg(r->args, 'A')) != 0) {
        b = hexval(a);
        if (dsp_reset(b))
            b = 0;
    } else
        for (i = 0; i < 4 && !b; i++) {
            /* erst lesen: nur FFh an allen Leseports = nichts da */
            if (inp(ports[i] + 0x0A) == 0xFF && inp(ports[i] + 0x0C) == 0xFF &&
                inp(ports[i] + 0x0E) == 0xFF)
                continue;
            if (dsp_reset(ports[i]) == 0)
                b = ports[i];
        }
    if (!b)
        return -1;
    base = b;
    dsp_w(0xE1);                        /* DSP-Version                   */
    maj = dsp_r();
    min = dsp_r();
    dspv = (maj << 8) | min;
    if ((a = arg(r->args, 'V')) != 0 && hexval(a))
        dspv = hexval(a);               /* /V:105 - nur zum Testen: alte
                                           DSP-Version vortaeuschen      */
    if (dspv >= 0x400) {                /* SB16: Einstellung im Mixer    */
        m = mix_r(0x80);
        irq = (m & 1) ? 9 : (m & 2) ? 5 : (m & 4) ? 7 : (m & 8) ? 10 : irq;
        m = mix_r(0x81);
        dma8 = (m & 1) ? 0 : (m & 2) ? 1 : (m & 8) ? 3 : dma8;
        dma16 = (m & 0x20) ? 5 : (m & 0x40) ? 6 : (m & 0x80) ? 7 : 0;
    } else
        dma16 = 0;
    if ((a = arg(r->args, 'I')) != 0)
        irq = decval(a) == 2 ? 9 : decval(a);
    if ((a = arg(r->args, 'D')) != 0)
        dma8 = decval(a);
    if ((a = arg(r->args, 'H')) != 0)
        dma16 = decval(a);
    if (dma8 > 3 || dma8 == 2 || (dma16 && (dma16 < 5 || dma16 > 7))) {
        dputs("SB16.SYS: ung\x81ltiger DMA-Kanal\n");
        return -1;
    }
    bsel = devhlp(DH_DMABUF, 0, BUFSZ, (unsigned)&bphys);
    if (!bsel) {
        dputs("SB16.SYS: kein Speicher f\x81r den DMA-Puffer\n");
        return -1;
    }
    if (set_irq(irq, sb_irq)) {
        dputs("SB16.SYS: IRQ belegt\n");
        return -1;
    }
    claim(RES_IO, base, 16);
    claim(RES_DMA, dma8, 1);
    if (dma16)
        claim(RES_DMA, dma16, 1);

    p = scat(name, dspv >= 0x400 ? "Sound Blaster 16" :
                   dspv >= 0x300 ? "Sound Blaster Pro" : "Sound Blaster");
    p = scat(p, ", DSP ");
    p = udec(p, maj);
    p = scat(p, ".");
    if (min < 10)
        p = scat(p, "0");
    udec(p, min);
    p = scat(line, "  ");
    p = scat(p, name);
    p = scat(p, " an ");
    p = hex(p, base, 3);
    p = scat(p, "h, IRQ ");
    p = udec(p, irq);
    p = scat(p, ", DMA ");
    p = udec(p, dma8);
    if (dma16) {
        p = scat(p, "/");
        p = udec(p, dma16);
    }
    scat(p, "\n");
    dputs(line);
    return 0;
}

/* ---- FM (OPL2/OPL3 an 388h) ------------------------------------------ */

static void opl_w(unsigned reg, unsigned char v)
{
    unsigned port = (reg & 0x100) ? 0x38A : 0x388, k;
    outp(port, (unsigned char)reg);
    for (k = 0; k < 6; k++)             /* 3,3 us nach der Adresse (OPL2) */
        inp(0x388);
    outp(port + 1, v);
    for (k = 0; k < 35; k++)            /* 23 us nach dem Wert            */
        inp(0x388);
}

/* Erkennung wie im AdLib-Handbuch: Zeitgeber 1 laufen lassen, Status
   muss danach C0h zeigen. Erst lesen: FFh = nichts an 388h */
static int opl_detect(void)
{
    unsigned char s1, s2;
    unsigned k;
    if (inp(0x388) == 0xFF)
        return 0;
    opl_w(4, 0x60);
    opl_w(4, 0x80);
    s1 = (unsigned char)(inp(0x388) & 0xE0);
    opl_w(2, 0xFF);
    opl_w(4, 0x21);
    for (k = 0; k < 200; k++)           /* mindestens 80 us               */
        inp(0x388);
    s2 = (unsigned char)(inp(0x388) & 0xE0);
    opl_w(4, 0x60);
    opl_w(4, 0x80);
    if (s1 != 0 || s2 != 0xC0)
        return 0;
    if (inp(0x388) & 0x06)
        return AUDCAP_OPL2;
    /* Statusbits 1-2 = 0 sagen OPL3, aber ein OPL2 dekodiert nur A0:
       38Ah/38Bh spiegeln dann 388h/389h (QEMU-AdLib). Zeitgeber ueber
       die zweite Bank starten - laeuft er, ist es ein Spiegel */
    opl_w(0x102, 0xFF);
    opl_w(0x104, 0x21);
    for (k = 0; k < 200; k++)
        inp(0x388);
    s2 = (unsigned char)(inp(0x388) & 0xE0);
    opl_w(0x104, 0);                    /* OPL3: 4-Operator-Bits wieder aus */
    opl_w(4, 0x60);
    opl_w(4, 0x80);
    return s2 == 0xC0 ? AUDCAP_OPL2 : AUDCAP_OPL3;
}

static void opl_reset(void)
{
    unsigned r, banks = (caps & AUDCAP_OPL3) ? 2 : 1, b;
    for (b = 0; b < banks; b++) {
        for (r = 0xB0; r <= 0xB8; r++)  /* zuerst alle Tasten los        */
            opl_w(r | (b << 8), 0);
        for (r = 0x20; r <= 0xF5; r++)
            opl_w(r | (b << 8), (r >= 0x40 && r <= 0x55) ? 0x3F : 0);
    }
    if (caps & AUDCAP_OPL3) {
        opl_w(0x104, 0);
        opl_w(0x105, 0);
    }
    opl_w(0x01, 0);
    opl_w(0x08, 0);
}

/* ---- MIDI (MPU-401 im UART-Betrieb) ---------------------------------- */
static unsigned mpu = 0x330;

static int mpu_cmd(unsigned char c)
{
    unsigned n;
    for (n = 0; n < 30000u && (inp(mpu + 1) & 0x40); n++)
        ;
    outp(mpu + 1, c);
    for (n = 0; n < 30000u; n++)
        if (!(inp(mpu + 1) & 0x80) && inp(mpu) == 0xFE)
            return 0;                   /* Bestaetigung                  */
    return -1;
}

static void midi_w(unsigned char v)
{
    unsigned n;
    for (n = 0; n < 30000u && (inp(mpu + 1) & 0x40); n++)
        if (!(inp(mpu + 1) & 0x80))
            inp(mpu);                   /* Eingang leeren                */
    outp(mpu, v);
}

static int mpu_detect(void)
{
    if (inp(mpu + 1) == 0xFF)           /* erst lesen                    */
        return 0;
    if (mpu_cmd(0xFF) && mpu_cmd(0xFF)) /* Reset (im UART-Betrieb kommt
                                           das erste FFh ohne Antwort an) */
        return 0;
    mpu_cmd(0x3F);                      /* UART-Betrieb                  */
    return AUDCAP_MPU401;
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    const char __far *a;
    char *p;
    unsigned o;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    if (init_pcm(r) == 0)
        caps |= AUDCAP_PCM;
    if (!has_opt(r->args, "NOFM") && (o = opl_detect()) != 0) {
        caps |= o;
        opl_reset();
        if (dspv >= 0x400 && (mix_r(0x34) >> 3) == 0) {
            mix_w(0x34, 0xC0);          /* FM-Regler auf 0: 75 %          */
            mix_w(0x35, 0xC0);
        }
    }
    if ((a = arg(r->args, 'M')) != 0)
        mpu = hexval(a);
    if (!has_opt(r->args, "NOMPU") && mpu_detect())
        caps |= AUDCAP_MPU401;
    if (!caps) {
        dputs("SB16.SYS: keine Sound Blaster, AdLib oder MPU-401 gefunden\n");
        return;
    }
    if (caps & (AUDCAP_OPL2 | AUDCAP_OPL3))
        claim(RES_IO, 0x388, 4);
    if (caps & AUDCAP_MPU401)
        claim(RES_IO, mpu, 2);
    if (caps & (AUDCAP_OPL2 | AUDCAP_OPL3 | AUDCAP_MPU401)) {
        p = scat(line, "  ");
        if (caps & AUDCAP_OPL3)
            p = scat(p, "FM-Synthese OPL3 an 388h");
        else if (caps & AUDCAP_OPL2)
            p = scat(p, "FM-Synthese OPL2 (AdLib) an 388h");
        if (caps & AUDCAP_MPU401) {
            if (caps & (AUDCAP_OPL2 | AUDCAP_OPL3))
                p = scat(p, ", ");
            p = scat(p, "MIDI MPU-401 an ");
            p = hex(p, mpu, 3);
            p = scat(p, "h");
        }
        scat(p, "\n");
        dputs(line);
    }
    if (!(caps & AUDCAP_PCM))
        scat(name, (caps & AUDCAP_OPL3) ? "OPL3" : (caps & AUDCAP_OPL2) ? "AdLib (OPL2)" : "MPU-401");
    devhlp(DH_AUDREG, 0, 0, 0);
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
    case DRV_AUDIO:
        audio();
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
