/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VSB.SYS - Ton in der DOS-Box (6.3, Stufe 2 nach docs/VDD.md; vorher in
 * kernel/v86.c): virtuelle Sound Blaster bis SB16 (DSP 4.05, 220h, DMA 1
 * und 5, IRQ 5 oder 7, Mixer 224h/225h), OPL (AdLib) an 388h-38Bh und
 * 220h-223h/228h/229h, MPU-401 an 330h/331h, EMU8000 der AWE32/64.
 *
 * Ausgabe ueber die Audio-Schnittstelle des Kernels (SB16.SYS) im Namen
 * der Box (VS_AUD), Box-Speicher ueber VS_RD/VS_WR. Der OPL und der
 * EMU8000 gehoeren der Box im Vordergrund direkt (OPL ueber die I/O-
 * Bitmap, VS_IOPERM; EMU als Wortzugriff durch den Monitor), hinten sind
 * sie stumm (VDD_EV_BG). Die Leitung meldet das Modul dem Kernel (VS_IRQ),
 * der stellt sie zu wie seine eigenen - auch an PM-Handler (DPMI).
 * Ohne dieses Modul sieht die Box keine Sound Blaster.
 */
#include "drvlib.h"
#include "netlib.h"                     /* fent(), inw/outw              */
#include "../include/vdd.h"
#include "../include/fapi_abi.h"

#define NT VDD_NTASK

static struct vdd_svc sv;
static struct vdd_reg reg;

static unsigned svc(unsigned fn, unsigned t, unsigned a1, unsigned a2, unsigned a3)
{
    sv.fn = fn;
    sv.task = t;
    sv.a1 = a1;
    sv.a2 = a2;
    sv.a3 = a3;
    return devhlp(DH_VDDSVC, 0, (unsigned)&sv, 0);
}

/* Audio-FAPI im Namen der Box (a = Puffer im DS des Moduls) */
static unsigned aud(unsigned t, unsigned fn, unsigned a, unsigned b)
{
    sv.ffn = fn;
    return svc(VS_AUD, t, a, b, 0);
}

/* Box-Speicher <-> Puffer */
static void boxmem(unsigned t, unsigned long lin, void *buf, unsigned n, int rd)
{
    sv.lin = lin;
    sv.buf = (unsigned)buf;
    sv.len = n;
    svc(rd ? VS_RD : VS_WR, t, 0, 0, 0);
}

static unsigned div3216(unsigned long n, unsigned d);
#pragma aux div3216 = "div bx" __parm [__dx __ax] [__bx] __value [__ax] __modify [__dx];

struct vsb {
    unsigned char on, rst, cmd, need, nargs, args[3];
    unsigned char out[4], on_, oh;      /* Ausgabe an 22Ah (Ring)        */
    unsigned char spk, act, ai, aud, irql, dsp4;
    unsigned char b16, st, sgn, ch;     /* laufende Ausgabe: 16 Bit, stereo,
                                           mit Vorzeichen, DMA 1 (0) / 5 (1) */
    unsigned char conv, ist;            /* auf 8 Bit mono gerechnet; Mixer 82h */
    unsigned char och, obits;           /* geoeffnetes Format            */
    unsigned char mix_i, mix[24];       /* Mixer: Index, Register 30h-47h */
    unsigned rate, orate, blk8, frac;   /* Rate, Blocklaenge 48h         */
    unsigned long blk, pos;             /* Block in Bytes, Stand         */
    unsigned last;                      /* Kerneltakt der letzten Abrechnung */
    unsigned dbase[2], dcnt[2], dcur[2], dleft[2];  /* DMA 1 / 5: Basis,
                                           Laenge-1, aktuell, Rest (Einheiten) */
    unsigned char dpage[2], dff[2];
    unsigned char e2v, e2c;             /* DSP E2h: Wert, Zaehler        */
    unsigned char quick;                /* kurzer Block: sofort fertig   */
    unsigned char rec;                  /* laufender Block ist Aufnahme  */
    unsigned char m0e;                  /* SB-Pro-Mixer 0Eh (Bit 1 stereo) */
    unsigned char muart;                /* DSP-MIDI im UART-Betrieb (34h-37h) */
    unsigned prate;                     /* Abtastrate je Kanal beim Start */
};
static struct vsb sb[NT];
static unsigned char mpu_st[NT];        /* MPU-401: Bit 0 UART, 1 offen, 2 FEh wartet */
static unsigned char opl_open[NT];      /* 388h-38Bh frei in der Bitmap  */
static unsigned char opl_trk[NT];       /* Bit 0 mitschreiben, 1 Kopie gilt, 2 Bank 1 benutzt */
static unsigned char opl_idx[NT][2];
static unsigned char opl_sh[NT][512];   /* Registerkopie je Box          */
static unsigned char emu_owner;         /* EMU8000: Task + 1, 0 = keiner */
static unsigned emu_base;               /* 620h, 640h, 660h, 680h        */
static char sb_buf[1024];

/* Box bekommt Hardware (vorn): beim Wechsel nach hinten kommt VDD_EV_BG */
static void fg_take(unsigned t)
{
    svc(VS_FGRES, t, 1, 0, 0);
}

/* ---- OPL (AdLib, FM-Musik) an 388h-38Bh ----------------------------------
   Im Vordergrund direkt an den Chip (DOOM/DMX, Spiele mit AdLib-Musik),
   hinten gesperrt, alle Stimmen aus. */
static void opl_wr(unsigned base, unsigned reg, unsigned val)
{
    unsigned i;
    outp(base, (unsigned char)reg);
    for (i = 0; i < 6; i++)
        inp(base);
    outp(base + 1, (unsigned char)val);
    for (i = 0; i < 35; i++)
        inp(base);
}

/* Registerkopie der Box (AdLib teilen): die Box schreibt sonst direkt,
   und hat inzwischen ein FAPI-Programm gespielt, fehlen ihr beim
   Zurueckkommen die Instrumente. Fallen auf 388h sind teuer (Treiber
   lesen nach jedem Register 30-40-mal den Status am selben Port) - daher
   nur, wenn es noetig ist: ein FAPI-Programm hat FM offen, wenn die Box
   den Chip nimmt, oder DOSBOX.INI ADLIB_COPY=ON. Dann bleiben die Ports
   gesperrt, opl_out merkt Index und Wert und reicht sie durch; nimmt die
   Box den Chip wieder, laedt opl_restore ihre Kopie (Tasten los). */

/* gibt es das OPL-Register? (Operatoren 0-15h je 20h-Block, Kanaele 0-8,
   BDh, kein D0h-DFh) */
#define OPL_REG(r)  ((r) >= 0xE0 || ((r) < 0xA0 ? ((r) & 0x1F) <= 0x15 : \
                     (r) == 0xBD || ((r) < 0xD0 && ((r) & 0x0F) <= 8)))

static void opl_restore(unsigned t)
{
    unsigned char *c = opl_sh[t];
    unsigned r, b, v;
    static const unsigned first[3] = { 0x105, 0x104, 0x008 };
    for (r = 0; r < 3; r++)                     /* Bank 1 nur, wenn die Box */
        if (!(first[r] & 0x100) || (opl_trk[t] & 4))    /* sie benutzt hat: */
            opl_wr(first[r] & 0x100 ? 0x38A : 0x388, first[r] & 0xFF, c[first[r]]);
    opl_wr(0x388, 0x01, 0x20);                  /* Wellenformen erst mit WSE an */
    for (b = 0; b < 1 + ((opl_trk[t] >> 2) & 1); b++)   /* (OPL2: 38Ah spiegelt 388h) */
        for (r = 0x20; r <= 0xF5; r++) {
            if (!OPL_REG(r))
                continue;                       /* Luecken (D0h-D8h waere */
            v = c[b * 256 + r];                 /* in QEMU C0h-C8h!)     */
            if (r >= 0xB0 && r <= 0xB8)
                v &= ~0x20;                     /* Tasten los            */
            else if (r == 0xBD)
                v &= 0xE0;                      /* Rhythmus-Tasten los   */
            opl_wr(b ? 0x38A : 0x388, r, v);
        }
    opl_wr(0x388, 0x01, c[0x01]);               /* (sonst bleibt die alte) */
}

static void opl_ports(unsigned t, int on)
{
    unsigned c;
    if (on == opl_open[t])
        return;
    opl_open[t] = (unsigned char)on;
    aud(t, 0xFF, on, 0);                        /* FM-Programme warten   */
    if (on) {
        fg_take(t);
        if (opl_trk[t] & 2)
            opl_restore(t);                     /* eigene Instrumente    */
        if ((svc(VS_HK, t, 0, 0, 0) & VDD_HK_ADLIB) || (opl_trk[t] & 2) ||
            aud(t, 0xFE, 0, 0)) {
            opl_trk[t] |= 1;                    /* gesperrt: mitschreiben */
            return;
        }
        opl_trk[t] &= ~1;
        svc(VS_IOPERM, t, 0x388, 4, 1);         /* 388h-38Bh frei        */
    } else {
        svc(VS_IOPERM, t, 0x388, 4, 0);
        for (c = 0; c < 9; c++) {               /* Tasten los            */
            opl_wr(0x388, 0xB0 + c, 0);
            opl_wr(0x38A, 0xB0 + c, 0);
        }
        opl_wr(0x388, 0xBD, 0);                 /* Rhythmus aus          */
    }
}

static void opl_out(unsigned t, unsigned p, unsigned char v)
{
    unsigned bk = (p >> 1) & 1;
    opl_ports(t, 1);                            /* (FM-Programme erst still) */
    if (opl_trk[t] & 1) {                       /* Kopie mitschreiben    */
        if (!(p & 1))
            opl_idx[t][bk] = v;
        else {
            opl_sh[t][bk * 256 + opl_idx[t][bk]] = v;
            opl_trk[t] |= (unsigned char)(2 | bk << 2);   /* Bit 2: Bank 1 */
        }
    }
    outp(p, v);
}

/* OPL der Sound Blaster: 220h-223h (SB Pro/SB16, beide Baenke) und
   228h/229h (OPL2-Sicht) sind derselbe Chip wie 388h-38Bh.  DOTT sucht
   den OPL bei Basis+8 und gibt ohne ihn auch den Digitalton auf.     */
static unsigned sb_opl(unsigned p)
{
    if (p >= 0x220 && p <= 0x223)
        return p + (0x388 - 0x220);
    if (p == 0x228 || p == 0x229)
        return p + (0x388 - 0x228);
    return p;
}

/* ---- EMU8000 (Wavetable der AWE32/64) -----------------------------------
   Spiele mit eigenem AWE-Treiber (DOOM, Duke 3D ...) sprechen den Chip
   direkt an: Datenregister 620h/A20h/E20h (+2: A22h, E22h = Zeiger) bei
   EMU-Basis 620h (SB 220h), 640h ... - als 16-Bit-Ports, die man nicht
   in Bytes zerlegen darf (der Monitor reicht Wortzugriffe ueber 3FFh als
   VDD_INW/VDD_OUTW durch). Die Box im Vordergrund greift direkt durch,
   hinten ist der Chip stumm. Die I/O-Bitmap reicht nur bis 3DFh, also
   bleibt jeder Zugriff eine Falle. */
static int emu_port(unsigned p)
{
    unsigned q = p & 0x3FF;
    return p >= 0x400 && p < 0x1000 && q >= 0x220 && q <= 0x283 && !(q & 0x1C);
}

static void emu_take(unsigned t, unsigned p)
{
    if (emu_owner != t + 1)
        fg_take(t);
    emu_owner = (unsigned char)(t + 1);
    emu_base = 0x400 | (p & 0x3E0);
}

/* hinten, Parken, Ende: alle 32 Stimmen in die Ausklingphase */
static void emu_off(unsigned t)
{
    unsigned ch;
    if (emu_owner != t + 1)
        return;
    cli();
    for (ch = 0; ch < 32; ch++) {
        outw(emu_base + 0x802, (5 << 5) | ch);  /* DCYSUSV (Data1)       */
        outw(emu_base + 0x400, 0x807F);
    }
    sti();
    emu_owner = 0;
}

/* ---- Virtuelle Sound Blaster (DSP 2.01 bis 4.05, 220h, DMA 1/5, IRQ 5/7)
   DSP-Ports 226h/22Ah/22Ch/22Eh/22Fh, Mixer 224h/225h, 8237-Kanal 1 und 5
   nur virtuell. Laeuft eine DMA-Ausgabe, holt der Takt (sb_pump) die
   faelligen Bytes aus dem Speicher der Box und gibt sie ueber die Audio-
   Schnittstelle des Kernels aus; am Blockende IRQ 5 bzw. 7 - welcher,
   entscheidet der eingehaengte Vektor (0Dh/0Fh). */

/* Leitung der virtuellen Karte: die das Programm zuletzt per DPMI 0205h
   eingehaengt hat (DOS/4GW haengt beim Start alle ein, DMX danach nur
   seine), sonst im Real Mode umgebogenes 0Fh -> 7, sonst 5. Ruht die
   Leitung (kein Handler), verwirft der Kernel den IRQ (VS_IRQ). */
static unsigned sb_line(unsigned t)
{
    unsigned d;
    if (sb[t].irql)
        return sb[t].irql;
    d = svc(VS_VECDEF, t, 0, 0, 0);
    return !(d & 0x80) && (d & 0x20) ? 7 : 5;
}

static void sb_irq(unsigned t, unsigned char bit)
{
    sb[t].ist |= bit;
    svc(VS_IRQ, t, sb_line(t), 0, 0);
}

static void sb_put(unsigned t, unsigned char v)
{
    if (sb[t].on_ < 4) {
        sb[t].out[(sb[t].oh + sb[t].on_) & 3] = v;
        sb[t].on_++;
    }
}

/* Grundzustand (Box angelegt, DSP-Reset); Mixer nur beim Anlegen und
   bei Mixer-Reset (Register 0) */
static void sb_mixreset(struct vsb *b)
{
    unsigned i;
    for (i = 0; i < sizeof(b->mix); i++)
        b->mix[i] = 0;
    for (i = 0; i < 6; i++)                     /* 30h-35h Lautstaerken  */
        b->mix[i] = 0xC0;
    b->mix[0x3C - 0x30] = 0x1F;                 /* Ausgaenge an          */
    b->mix[0x46 - 0x30] = b->mix[0x47 - 0x30] = 0x80;
}

static void sb_reset(unsigned t)
{
    struct vsb *b = &sb[t];
    unsigned char *p = (unsigned char *)b;
    unsigned i;
    for (i = 0; i < sizeof(*b); i++)
        p[i] = 0;
    b->rate = 11025;
    b->e2v = 0xAA;
    sb_mixreset(b);
}

static void sb_stop(unsigned t)
{
    sb[t].act = 0;
    svc(VS_TICK, t, 0, 0, 0);
    if (sb[t].aud) {                    /* der Kernel ordnet Ton dem Task */
        aud(t, AUD_CLOSE, 0, 0);        /* zu (nicht current: task_end    */
        sb[t].aud = 0;                  /* raeumt auf)                    */
    }
}

/* ---- virtueller MPU-401 (330h/331h) ---------------------------------------
   Die meisten Spiele kennen nur "General MIDI ueber MPU-401" im UART-
   Betrieb. Die Box schickt ihre MIDI-Bytes ueber die Audio-FAPI an den
   Kernel - der gibt sie an den echten MPU-401 der Karte (oder den AWE),
   der Vordergrund hat Vorrang. Intelligenter Betrieb: nur bestaetigt
   (FEh), nicht nachgebildet. */
static unsigned char mpu_b;

/* ein MIDI-Byte der Box (MPU-401 oder DSP-MIDI) an den Kernel */
static void midi_send(unsigned t, unsigned char v)
{
    if (!(mpu_st[t] & 2)) {                     /* erstes Byte: oeffnen  */
        if (aud(t, AUD_MIDIOPEN, 0, 0))
            return;                             /* kein MIDI-Ausgang da   */
        mpu_st[t] |= 2;
    }
    mpu_b = v;
    aud(t, AUD_MIDIWRITE, (unsigned)&mpu_b, 1);
}

static void mpu_out(unsigned t, unsigned p, unsigned char v)
{
    if (p == 0x331) {                           /* Befehl                */
        if (v == 0xFF) {                        /* Reset                 */
            if (!(mpu_st[t] & 1))
                mpu_st[t] |= 4;                 /* FEh nur ausserhalb UART */
            mpu_st[t] &= (unsigned char)~1;
            if (mpu_st[t] & 2) {                /* alles verstummt        */
                aud(t, AUD_MIDICLOSE, 0, 0);
                mpu_st[t] &= (unsigned char)~2;
            }
        } else {
            if (v == 0x3F)
                mpu_st[t] |= 1;                 /* UART-Betrieb          */
            mpu_st[t] |= 4;
        }
        return;
    }
    midi_send(t, v);
}

static unsigned char mpu_in(unsigned t, unsigned p)
{
    if (p == 0x331)                             /* Bit 7 = 0: Byte da,   */
        return (unsigned char)((mpu_st[t] & 4) ? 0x3F : 0xBF);  /* 6 = 0: bereit */
    mpu_st[t] &= (unsigned char)~4;
    return 0xFE;                                /* Bestaetigung           */
}

/* Parken, Ende: MIDI-Platz zurueckgeben */
static void mpu_off(unsigned t)
{
    if (mpu_st[t] & 2)
        aud(t, AUD_MIDICLOSE, 0, 0);
    mpu_st[t] = 0;
}

static int sb_open(unsigned t, unsigned rate, unsigned ch, unsigned bits)
{
    static struct fapi_audfmt fmt;
    fmt.rate = rate;
    fmt.channels = (unsigned char)ch;
    fmt.bits = (unsigned char)(bits | AUD_SMALL | AUD_NOWAIT);  /* Spiele: Ton
                                       sofort, die Box schlaeft nie beim Schreiben */
    return aud(t, AUD_OPEN, (unsigned)&fmt, 0) == 0;
}

/* Ausgabe beginnen: len Bytes je Block; Format aus b16/st/sgn/ch.
   Kann die Karte das Format nicht (SB 2.0 und 16 Bit/Stereo), geht der
   Ton heruntergerechnet als 8 Bit mono hinaus (conv) */
static void sb_start(unsigned t, unsigned long len, int ai)
{
    struct vsb *b = &sb[t];
    unsigned ch = b->st ? 2 : 1, bits = b->b16 ? 16 : 8;
    if (!len)
        return;
    if (!b->rate)
        b->rate = 11025;
    /* SB Pro stereo (Mixer 0Eh, 8-Bit-Befehle): die Zeitkonstante gilt fuer
       beide Kanaele zusammen - je Kanal die halbe Rate */
    b->prate = b->st && b->cmd < 0xB0 ? b->rate >> 1 : b->rate;
    if (b->rec)                                 /* Aufnahme: nichts zur  */
        ;                                       /* Karte                 */
    else if (!b->aud || b->orate != b->prate || b->och != ch || b->obits != bits) {
        if (b->aud)
            aud(t, AUD_CLOSE, 0, 0);
        b->conv = 0;
        b->aud = (unsigned char)sb_open(t, b->prate, ch, bits);
        if (!b->aud && (ch != 1 || bits != 8)) {
            b->aud = (unsigned char)sb_open(t, b->prate, 1, 8);
            b->conv = 1;
        }
        b->orate = b->prate;
        b->och = (unsigned char)ch;
        b->obits = (unsigned char)bits;
    }
    b->blk = len;
    b->pos = 0;
    b->frac = 0;
    b->ai = (unsigned char)ai;
    b->act = 1;
    b->last = ticks();
    /* Einzelblock kuerzer als ein Takt (Erkennung: DOTT/CT-VOICE schickt
       1 Byte mit 14h und wartet in einer Zaehlschleife auf den IRQ - auf
       schnellen Rechnern weit unter 10 ms): gleich beim Ruecksprung
       fertig melden statt erst beim naechsten Takt */
    b->quick = (unsigned char)(!ai && len <= (unsigned long)(b->prate / 100) << (b->b16 + b->st));
    svc(VS_TICK, t, b->quick ? 2 : 1, 0, 0);
}

/* Vorzeichen anpassen (Karte: 8 Bit ohne, 16 Bit mit), ggf. auf 8 Bit
   mono herunterrechnen (linker Kanal) -> Bytes zum Ausgeben */
static unsigned sb_conv(struct vsb *b, unsigned n)
{
    unsigned i, o, fs;
    unsigned char x = 0;
    if (b->b16 && !b->sgn)
        x = 0x80;                               /* hohes Byte kippen     */
    else if (!b->b16 && b->sgn)
        x = 0x80;
    if (x)
        for (i = b->b16; i < n; i += 1 + b->b16)
            sb_buf[i] ^= x;
    if (!b->conv)
        return n;
    fs = (1 + b->b16) * (b->st ? 2 : 1);
    for (i = o = 0; i + fs <= n; i += fs)
        sb_buf[o++] = (char)(b->b16 ? sb_buf[i + 1] ^ 0x80 : sb_buf[i]);
    return o;
}

/* faellige Bytes ausgeben, Blockende -> IRQ; je Kerneltakt (VS_TICK) */
static void sb_pump(unsigned t, int fgb)
{
    struct vsb *b = &sb[t];
    unsigned long lin, n, room;
    unsigned now, d, fr, u, k, w, j;
    int c;
    if (!b->act) {
        svc(VS_TICK, t, 0, 0, 0);
        return;
    }
    now = ticks();
    d = now - b->last > 20 ? 20 : now - b->last;
    if (b->quick && !d)
        d = 1;
    b->quick = 0;
    if (!d)
        return;
    b->last = now;
    fr = 0;
    while (d--) {                               /* Abtastwerte je 10 ms  */
        fr += b->prate / 100;
        b->frac += b->prate % 100;
        if (b->frac >= 100) {
            b->frac -= 100;
            fr++;
        }
    }
    n = (unsigned long)fr << (b->b16 + b->st);  /* Bytes                */
    c = b->ch;
    u = 1 + c;                                  /* DMA-Einheit in Bytes  */
    while (n && b->act) {
        k = (unsigned)(n > sizeof(sb_buf) ? sizeof(sb_buf) : n);
        if (k > b->blk - b->pos)                /* bis zum Blockende     */
            k = (unsigned)(b->blk - b->pos);
        room = ((unsigned long)b->dleft[c] + 1) << c;
        if (k > room)                           /* bis zum Pufferende    */
            k = (unsigned)room;
        if (c)
            lin = ((unsigned long)(b->dpage[c] & 0xFE) << 16) + ((unsigned long)b->dcur[c] << 1);
        else
            lin = ((unsigned long)b->dpage[c] << 16) + b->dcur[c];
        if (b->rec) {                           /* Aufnahme: Stille in den */
            for (j = 0; j < k; j++)             /* Puffer der Box        */
                sb_buf[j] = (char)(b->b16 ? 0 : b->sgn ? 0 : 0x80);
            boxmem(t, lin, sb_buf, k, 0);
        } else
            boxmem(t, lin, sb_buf, k, 1);
        if (!b->rec && b->aud && (b->spk || b->b16 || b->dsp4) && fgb) {
            w = sb_conv(b, k);
            if (w)
                aud(t, AUD_WRITE, (unsigned)sb_buf, w);
        }
        n -= k;
        b->pos += k;
        w = k / u;
        b->dcur[c] += w;
        if (w > b->dleft[c]) {                  /* Puffer zu Ende: 8237  */
            b->dcur[c] = b->dbase[c];           /* im Auto-Init          */
            b->dleft[c] = b->dcnt[c];
        } else
            b->dleft[c] -= w;
        if (b->pos >= b->blk) {                 /* Block fertig: IRQ     */
            sb_irq(t, (unsigned char)(b->b16 ? 2 : 1));
            b->pos = 0;
            if (!b->ai)
                b->act = 0;
        }
    }
}

/* DSP E2h (DMA-Erkennung, undokumentiert): die DSP schreibt per DMA
   (Kanal 1) ein Byte, das aus dem Argument und den vorigen E2h-Aufrufen
   folgt; Erkennungsroutinen (z. B. CT-VOICE in DOTT) pruefen es und
   nehmen die Karte sonst nicht.  Tabelle wie in DOSBox (GPL, src/hardware/
   sblaster.cpp, E2_incr_table) - Verhalten der echten DSP. */
static const signed char e2_tab[4][9] = {
    {  1,  -2,  -4,  8, -16,  32,  64, -128, -106 },
    { -1,   2,  -4,  8,  16, -32,  64, -128,  -91 },
    { -1,   2,   4, -8,  16, -32, -64, -128,  105 },
    {  1,  -2,   4, -8, -16,  32, -64, -128,   90 }
};
static void sb_e2(unsigned t, unsigned char a)
{
    struct vsb *b = &sb[t];
    const signed char *e = e2_tab[b->e2c & 3];
    unsigned i;
    for (i = 0; i < 8; i++)
        if (a & (1 << i))
            b->e2v = (unsigned char)(b->e2v + e[i]);
    b->e2v = (unsigned char)(b->e2v + e[8]);
    b->e2c++;
    boxmem(t, ((unsigned long)b->dpage[0] << 16) + b->dcur[0], &b->e2v, 1, 0);
    b->dcur[0]++;                               /* 8237 zaehlt weiter    */
    if (b->dleft[0])
        b->dleft[0]--;
    else {
        b->dcur[0] = b->dbase[0];
        b->dleft[0] = b->dcnt[0];
    }
}

/* DSP-Befehle bis DSP 4.05 (SB16). Aufnahme liefert Stille im richtigen
   Takt (mit IRQ und DMA-Zaehler - Programme, die aufnehmen oder damit die
   Karte pruefen, laufen weiter); MIDI ueber die DSP (38h, UART 34h-37h)
   geht wie der MPU-401 an den Kernel. */
static void sb_cmd(unsigned t, unsigned char v)
{
    struct vsb *b = &sb[t];
    unsigned d;
    if (b->muart) {                             /* UART: alles ist MIDI  */
        midi_send(t, v);
        return;
    }
    if (b->need) {                              /* Argument              */
        b->args[b->nargs++] = v;
        if (b->nargs < b->need)
            return;
        b->need = 0;
        if (b->cmd >= 0xB0 && b->cmd <= 0xCF) { /* SB16: Bx 16, Cx 8 Bit */
            b->rec = (unsigned char)((b->cmd & 8) != 0);    /* Bit 3: Eingang */
            b->b16 = (unsigned char)(b->cmd < 0xC0);
            b->ch = b->b16;                     /* 16 Bit ueber DMA 5    */
            b->st = (unsigned char)((b->args[0] & 0x20) != 0);
            b->sgn = (unsigned char)((b->args[0] & 0x10) != 0);
            sb_start(t, ((unsigned long)(b->args[1] | (b->args[2] << 8)) + 1) << b->b16,
                     (b->cmd & 4) != 0);
            return;
        }
        switch (b->cmd) {
        case 0x40:
            d = 256 - b->args[0];
            if (d < 16)
                d = 16;
            b->rate = div3216(1000000UL, d);
            break;
        case 0x41: case 0x42:                   /* SB16: Rate direkt     */
            b->rate = (b->args[0] << 8) | b->args[1];
            break;
        case 0x48:
            b->blk8 = (b->args[0] | (b->args[1] << 8)) + 1;
            break;
        case 0x14: case 0x91: case 0x24:       /* 24h: Aufnahme         */
            b->b16 = b->sgn = b->ch = 0;
            b->st = (unsigned char)((b->m0e & 2) != 0);
            b->rec = (unsigned char)(b->cmd == 0x24);
            sb_start(t, (b->args[0] | (b->args[1] << 8)) + 1UL, 0);
            break;
        case 0x38:                              /* MIDI-Byte senden      */
            midi_send(t, b->args[0]);
            break;
        case 0xE0:
            sb_put(t, (unsigned char)~b->args[0]);
            break;
        case 0xE4:
            b->args[2] = b->args[0];            /* Testregister          */
            break;
        case 0xE2:
            sb_e2(t, b->args[0]);
            break;
        }
        return;
    }
    b->cmd = v;
    b->nargs = 0;
    if (v >= 0xB0 && v <= 0xCF) {
        b->need = 3;
        return;
    }
    switch (v) {
    case 0x10: case 0x40: case 0xE0: case 0xE2: case 0xE4: case 0x38:
        b->need = 1;
        break;
    case 0x14: case 0x48: case 0x91: case 0x16: case 0x17: case 0x74: case 0x75:
    case 0x41: case 0x42: case 0x24:
        b->need = 2;
        break;
    case 0x1C: case 0x90:                       /* Auto-Init mit 48h     */
    case 0x2C: case 0x98:                       /* ... Aufnahme          */
        b->b16 = b->sgn = b->ch = 0;
        b->st = (unsigned char)((b->m0e & 2) != 0);
        b->rec = (unsigned char)(v == 0x2C || v == 0x98);
        sb_start(t, b->blk8 ? b->blk8 : 2048, 1);
        break;
    case 0x99:                                  /* Aufnahme einzeln (48h) */
        b->b16 = b->sgn = b->ch = 0;
        b->st = (unsigned char)((b->m0e & 2) != 0);
        b->rec = 1;
        sb_start(t, b->blk8 ? b->blk8 : 2048, 0);
        break;
    case 0x34: case 0x35: case 0x36: case 0x37: /* MIDI UART bis Reset   */
        b->muart = 1;
        break;
    case 0x30: case 0x31:                       /* MIDI lesen: nichts da */
        break;
    case 0xD0: case 0xD5: b->act = 0; break;    /* anhalten              */
    case 0xD4: case 0xD6:                       /* weiter                */
        if (b->blk) {
            b->act = 1;
            b->last = ticks();
            svc(VS_TICK, t, 1, 0, 0);
        }
        break;
    case 0xDA: case 0xD9: b->ai = 0; break;     /* Auto-Init beenden     */
    case 0xD1: b->spk = 1; break;
    case 0xD3: b->spk = 0; break;
    case 0xD8: sb_put(t, b->spk ? 0xFF : 0); break;
    case 0xE1:                                  /* DSP 4.05 (SB16)       */
        sb_put(t, 4); sb_put(t, 5);
        b->dsp4 = 1;
        break;
    case 0xE8: sb_put(t, b->args[2]); break;
    case 0xF2: sb_irq(t, 1); break;             /* IRQ ausloesen          */
    case 0xF3: sb_irq(t, 2); break;
    }
}

/* SB-Pro-Lautstaerken (4 Bit links | 4 Bit rechts) auf die SB16-Register
   (je Kanal 5 Bit oben): 22h Gesamt, 04h Wave, 26h FM, 28h CD, 2Eh Line */
static unsigned char sbp_reg(unsigned char i)
{
    switch (i) {
    case 0x22: return 0x30;
    case 0x04: return 0x32;
    case 0x26: return 0x34;
    case 0x28: return 0x36;
    case 0x2E: return 0x38;
    }
    return 0;
}

static unsigned char sb_mixget(unsigned t, unsigned char i)
{
    struct vsb *b = &sb[t];
    unsigned char l;
    if (i == 0x80)                              /* IRQ-Auswahl           */
        return (unsigned char)(sb_line(t) == 7 ? 4 : 2);
    if (i == 0x81)                              /* DMA 1 und 5           */
        return 0x22;
    if (i == 0x82)                              /* anstehende IRQ        */
        return b->ist;
    if (i >= 0x30 && i < 0x30 + sizeof(b->mix))
        return b->mix[i - 0x30];
    if (i == 0x0E)
        return b->m0e;
    if ((l = sbp_reg(i)) != 0)                  /* SB Pro: links/rechts  */
        return (unsigned char)((b->mix[l - 0x30] & 0xF0) | (b->mix[l + 1 - 0x30] >> 4));
    return 0;
}

static void sb_mixset(unsigned t, unsigned char i, unsigned char v)
{
    struct vsb *b = &sb[t];
    if (i == 0)
        sb_mixreset(b);
    else if (i == 0x80) {                       /* Programm stellt die   */
        if (v & 2)                              /* Leitung ein           */
            b->irql = 5;
        else if (v & 4)
            b->irql = 7;
    } else if (i >= 0x30 && i < 0x30 + sizeof(b->mix))
        b->mix[i - 0x30] = v;
    else if (i == 0x0E)
        b->m0e = v;                             /* Bit 1 stereo, 5 Filter */
    else if ((i = sbp_reg(i)) != 0) {
        b->mix[i - 0x30] = (unsigned char)(v & 0xF0);
        b->mix[i + 1 - 0x30] = (unsigned char)(v << 4);
    }
}

/* 8237-Ports: Kanal 1 (8 Bit) und 5 (16 Bit) virtuell, die anderen
   verpuffen - die echte DMA gehoert den Treibern */
static int sb_dma_in(struct vsb *b, unsigned p, unsigned char *v)
{
    int c;
    unsigned w;
    switch (p) {
    case 0x02: case 0x03: c = 0; break;
    case 0xC4: case 0xC6: c = 1; break;
    case 0x08:
        *v = (unsigned char)(b->act && !b->ch ? 0 : 0x02);  /* TC Kanal 1 */
        return 1;
    case 0xD0:
        *v = (unsigned char)(b->act && b->ch ? 0 : 0x02);   /* TC Kanal 5 */
        return 1;
    case 0x83: *v = b->dpage[0]; return 1;
    case 0x8B: *v = b->dpage[1]; return 1;
    default:
        return 0;
    }
    w = (p == 0x02 || p == 0xC4) ? b->dcur[c] : b->dleft[c];
    *v = (unsigned char)(b->dff[c] ? w >> 8 : w);
    b->dff[c] ^= 1;
    return 1;
}

static int sb_dma_out(struct vsb *b, unsigned p, unsigned char v)
{
    int c = p >= 0xC0;
    switch (p) {
    case 0x02: case 0xC4:                       /* Adresse               */
        if (b->dff[c]) b->dbase[c] = (b->dbase[c] & 0xFF) | (v << 8);
        else b->dbase[c] = (b->dbase[c] & 0xFF00) | v;
        b->dff[c] ^= 1;
        b->dcur[c] = b->dbase[c];
        return 1;
    case 0x03: case 0xC6:                       /* Laenge - 1            */
        if (b->dff[c]) b->dcnt[c] = (b->dcnt[c] & 0xFF) | (v << 8);
        else b->dcnt[c] = (b->dcnt[c] & 0xFF00) | v;
        b->dff[c] ^= 1;
        b->dleft[c] = b->dcnt[c];
        return 1;
    case 0x0C: case 0xD8:
        b->dff[c] = 0;
        return 1;
    case 0x83: b->dpage[0] = v; return 1;
    case 0x8B: b->dpage[1] = v; return 1;
    case 0x00: case 0x01: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0D: case 0x0E: case 0x0F:
    case 0xC0: case 0xC2: case 0xC8: case 0xCA: case 0xCC: case 0xCE:
    case 0xD0: case 0xD2: case 0xD4: case 0xD6: case 0xDA: case 0xDC: case 0xDE:
    case 0x81: case 0x82: case 0x87: case 0x89: case 0x8A: case 0x8F:
        return 1;                               /* Maske/Modus: egal     */
    }
    return 0;
}

static int sb_in(unsigned t, unsigned p, unsigned char *v)
{
    struct vsb *b = &sb[t];
    switch (p) {
    case 0x22A:
        if (b->on_) {
            *v = b->out[b->oh];
            b->oh = (unsigned char)((b->oh + 1) & 3);
            b->on_--;
        } else
            *v = 0xAA;
        return 1;
    case 0x22C:
        *v = 0x7F;                              /* bereit                */
        return 1;
    case 0x22E:                                 /* auch 8-Bit-IRQ quittieren */
        b->ist &= (unsigned char)~1;
        *v = (unsigned char)(b->on_ ? 0xFF : 0x7F);
        return 1;
    case 0x22F:                                 /* 16-Bit-IRQ quittieren */
        b->ist &= (unsigned char)~2;
        *v = 0xFF;
        return 1;
    case 0x224:
        *v = b->mix_i;
        return 1;
    case 0x225:
        *v = sb_mixget(t, b->mix_i);
        return 1;
    case 0x226:
        *v = 0xFF;
        return 1;
    }
    return sb_dma_in(b, p, v);
}

static int sb_out(unsigned t, unsigned p, unsigned char v)
{
    struct vsb *b = &sb[t];
    switch (p) {
    case 0x226:                                 /* Reset                 */
        if (v & 1)
            b->rst = 1;
        else if (b->rst) {
            b->rst = 0;
            sb_stop(t);
            b->need = 0;
            b->muart = 0;                       /* Reset beendet UART-MIDI */
            b->rec = 0;
            b->on_ = b->oh = 0;
            b->spk = 0;
            b->ist = 0;
            b->e2v = 0xAA;
            b->e2c = 0;
            sb_put(t, 0xAA);
        }
        return 1;
    case 0x22C:
        sb_cmd(t, v);
        return 1;
    case 0x224:
        b->mix_i = v;
        return 1;
    case 0x225:
        sb_mixset(t, b->mix_i, v);
        return 1;
    }
    return sb_dma_out(b, p, v);
}

/* ---- Einspruenge ----------------------------------------------------------- */

/* Ports, die das Modul nachbildet? (schnell: alles andere sofort weiter) */
static int mine(unsigned p)
{
    if (p < 0x10 || (p >= 0x81 && p <= 0x8F) || (p >= 0xC0 && p <= 0xDE))
        return 1;                               /* 8237                  */
    if (p >= 0x220 && p <= 0x22F)
        return 1;
    return p == 0x330 || p == 0x331 || (p >= 0x388 && p <= 0x38B) || emu_port(p);
}

static unsigned io(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task, p = c->port, fgb = c->flags & 1;
    static unsigned char v;             /* im DS (SS ist der Kernelstapel) */
    if (t >= NT || !mine(p))
        return VDD_NOTMINE;
    if (emu_port(p)) {                          /* EMU8000: Bytes/Worte  */
        if (c->fn == VDD_IN || c->fn == VDD_INW) {
            c->val = c->fn == VDD_INW ? 0xFFFF : 0xFF;
            if (fgb) {
                emu_take(t, p);
                c->val = c->fn == VDD_INW ? inw(p) : inp(p);
            }
        } else if (fgb) {
            emu_take(t, p);
            if (c->fn == VDD_OUTW)
                outw(p, c->val);
            else
                outp(p, (unsigned char)c->val);
        }
        return 0;
    }
    if (c->fn == VDD_INW || c->fn == VDD_OUTW)
        return VDD_NOTMINE;                     /* (der Monitor teilt sie) */
    if (c->fn == VDD_IN) {
        if (sb_in(t, p, &v)) {
            c->val = v;
            return 0;
        }
        if (p == 0x330 || p == 0x331) {
            c->val = mpu_in(t, p);
            return 0;
        }
        p = sb_opl(p);
        if (p >= 0x388 && p <= 0x38B) {
            c->val = 0xFF;
            if (fgb) {                          /* OPL: vorn direkt      */
                opl_ports(t, 1);
                c->val = inp(p);
            }
            return 0;
        }
        return VDD_NOTMINE;
    }
    v = (unsigned char)c->val;
    if (sb_out(t, p, v))
        return 0;
    if (p == 0x330 || p == 0x331) {
        mpu_out(t, p, v);
        return 0;
    }
    p = sb_opl(p);
    if (p >= 0x388 && p <= 0x38B) {
        if (fgb)
            opl_out(t, p, v);
        return 0;
    }
    return VDD_NOTMINE;
}

static unsigned tick(void __far *a)
{
    struct vdd_call __far *c = a;
    if (c->task < NT)
        sb_pump(c->task, c->flags & 1);
    return 0;
}

static unsigned event(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task;
    if (t >= NT)
        return 0;
    switch (c->fn) {
    case VDD_EV_CREATE:                 /* (tss_init sperrt alle Ports)  */
        sb_reset(t);
        mpu_st[t] = opl_open[t] = opl_trk[t] = 0;
        if (emu_owner == t + 1)
            emu_owner = 0;
        break;
    case VDD_EV_BG:                     /* hinten: OPL und EMU stumm      */
        opl_ports(t, 0);
        emu_off(t);
        break;
    case VDD_EV_PARK:
    case VDD_EV_DESTROY:
        opl_ports(t, 0);
        emu_off(t);
        sb_stop(t);
        mpu_off(t);
        if (c->fn == VDD_EV_DESTROY)
            opl_trk[t] = 0;             /* Kopie gilt nicht mehr          */
        break;
    case VDD_EV_RESUME:                 /* Pause nicht nachholen          */
        sb[t].last = ticks();
        break;
    case VDD_EV_PMHOOK:                 /* zuletzt eingehaengt: Leitung   */
        if (c->val == 0x0D || c->val == 0x0F)
            sb[t].irql = (unsigned char)(c->val == 0x0D ? 5 : 7);
        break;
    case VDD_EV_PMEND:
        sb[t].irql = 0;
        break;
    }
    return 0;
}

#pragma code_seg("ITEXT", "ICODE")   /* nur beim INIT */
static void init(void)
{
    struct drv_req __far *r = REQ;
    reg.version = VDD_VERSION;
    reg.size = sizeof(reg);
    scat(reg.name, "VSB");
    reg.flags = VDDF_ANYPORT;
    reg.io_off = fent(0, io);
    reg.ev_off = fent(1, event);
    reg.tick_off = fent(2, tick);
    if (devhlp(DH_VDDREG, 0, (unsigned)&reg, 0)) {
        dputs("VSB.SYS: Anmeldung beim Kernel gescheitert\n");
        r->status = DRV_ERR_NODEV;
        return;
    }
    dputs("  Sound Blaster 16 (220h, IRQ 5/7, DMA 1/5), AdLib, MPU-401 und AWE der DOS-Boxen\n");
    r->units = 0;
    r->status = DRV_OK;
}
#pragma code_seg("_TEXT", "CODE")

void strategy(void)
{
    struct drv_req __far *r = REQ;
    unsigned t;
    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    case DRV_DEINIT:                    /* DEVLOAD /U: Boxen verstummen   */
        for (t = 0; t < NT; t++) {
            sb[t].act = 0;
            svc(VS_TICK, t, 0, 0, 0);
            opl_ports(t, 0);
            emu_off(t);
        }
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
