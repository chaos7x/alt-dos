/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MIDI- und IMF-Wiedergabe fuer PLAY.EXE (wird von task_play.c eingebunden)
 *
 * MIDI (Standard-MIDI-Datei, Format 0/1): ueber den MPU-401, wenn da
 * (und nicht /FM), sonst ueber den OPL mit eigenen FM-Instrumenten -
 * 16 Klangfamilien nach General MIDI (Programm / 8) und ein kleines
 * Schlagzeug auf Kanal 10. Stimmen: 9 (OPL2) oder 18 (OPL3), die aelteste
 * wird abgeloest. IMF (AdLib-Musik aus Spielen): Registerpaare mit
 * Wartezeit in 1/560 s (/R:700 fuer 700 Hz).
 *
 * Zeit: Systemtakt 100 Hz (DosQTicks); Wartezeiten werden in 10 ms
 * gerechnet, der Rest in Mikrosekunden mitgefuehrt - ohne 32-Bit-
 * Multiplikation/Division aus der Laufzeitbibliothek (mul16/div32).
 */

unsigned long mul16(unsigned a, unsigned b);
#pragma aux mul16 = "mul dx" __parm [__ax] [__dx] __value [__dx __ax];

/* a / b, auf 65535 begrenzt (div32 darf nicht ueberlaufen) */
static unsigned sdiv(unsigned long a, unsigned b)
{
    if (!b || (a >> 16) >= b)
        return 0xFFFF;
    return div32(a, b);
}

/* ---- Zeit ----------------------------------------------------------- */
static unsigned t_start, t_now10;       /* Startzeit, Soll-Zeit in 10 ms  */
static unsigned long t_us;              /* Rest unter 10 ms               */

static void time_add_us(unsigned long us)
{
    unsigned q;
    t_us += us;
    if (t_us >= 10000UL) {
        q = div32(t_us > 600000000UL ? 600000000UL : t_us, 10000);
        t_now10 += q;
        t_us -= mul16(q, 10000);
    }
}

/* bis zur Soll-Zeit warten; 1 = Esc gedrueckt */
static int wait_until(void)
{
    for (;;) {
        if ((KbdPeek() & 0xFF) == 0x1B) {
            KbdCharIn();
            return 1;
        }
        if ((int)(t_now10 - (unsigned)(DosQTicks() - t_start)) <= 0)
            return 0;
        DosSleep(10);
    }
}

/* ---- OPL ------------------------------------------------------------ */
static struct fapi_fmreg fmq[128];
static unsigned fmn, opl3;

static void fm(unsigned reg, unsigned val)
{
    fmq[fmn].reg = reg;
    fmq[fmn].val = (unsigned char)val;
    if (++fmn == 128) {
        FmWrite(fmq, fmn);
        fmn = 0;
    }
}

static void fm_flush(void)
{
    if (fmn)
        FmWrite(fmq, fmn);
    fmn = 0;
}

/* F-Nummer | Block << 10 je MIDI-Note */
static const unsigned fnum[128] = {
    0x00AC, 0x00B7, 0x00C2, 0x00CD, 0x00D9, 0x00E6, 0x00F4, 0x0102,
    0x0112, 0x0122, 0x0133, 0x0146, 0x0159, 0x016D, 0x0183, 0x019A,
    0x01B3, 0x01CC, 0x01E8, 0x0205, 0x0223, 0x0244, 0x0267, 0x028B,
    0x02B2, 0x02DB, 0x0306, 0x0334, 0x0365, 0x0399, 0x03CF, 0x0605,
    0x0623, 0x0644, 0x0667, 0x068B, 0x06B2, 0x06DB, 0x0706, 0x0734,
    0x0765, 0x0799, 0x07CF, 0x0A05, 0x0A23, 0x0A44, 0x0A67, 0x0A8B,
    0x0AB2, 0x0ADB, 0x0B06, 0x0B34, 0x0B65, 0x0B99, 0x0BCF, 0x0E05,
    0x0E23, 0x0E44, 0x0E67, 0x0E8B, 0x0EB2, 0x0EDB, 0x0F06, 0x0F34,
    0x0F65, 0x0F99, 0x0FCF, 0x1205, 0x1223, 0x1244, 0x1267, 0x128B,
    0x12B2, 0x12DB, 0x1306, 0x1334, 0x1365, 0x1399, 0x13CF, 0x1605,
    0x1623, 0x1644, 0x1667, 0x168B, 0x16B2, 0x16DB, 0x1706, 0x1734,
    0x1765, 0x1799, 0x17CF, 0x1A05, 0x1A23, 0x1A44, 0x1A67, 0x1A8B,
    0x1AB2, 0x1ADB, 0x1B06, 0x1B34, 0x1B65, 0x1B99, 0x1BCF, 0x1E05,
    0x1E23, 0x1E44, 0x1E67, 0x1E8B, 0x1EB2, 0x1EDB, 0x1F06, 0x1F34,
    0x1F65, 0x1F99, 0x1FCF, 0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF,
    0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF
};

/* eigene FM-Instrumente (2 Operatoren): Modulator/Traeger fuer 20h
   (AM/VIB/EG/KSR/MULT), 40h (KSL/TL), 60h (AR/DR), 80h (SL/RR), E0h
   (Welle), dazu C0h (Rueckkopplung << 1 | Verbindung) */
struct patch {
    unsigned char m20, c20, m40, c40, m60, c60, m80, c80, me0, ce0, c0;
};
static const struct patch fam[16] = {
    { 0x01, 0x01, 0x1C, 0x00, 0xF3, 0xF2, 0x34, 0x45, 0, 0, 0x0A },   /* Klavier    */
    { 0x07, 0x01, 0x22, 0x00, 0xF5, 0xF5, 0x25, 0x35, 0, 0, 0x06 },   /* Glocken    */
    { 0x21, 0x21, 0x14, 0x00, 0xF0, 0xF0, 0x06, 0x06, 0, 0, 0x01 },   /* Orgel      */
    { 0x01, 0x01, 0x1A, 0x00, 0xF4, 0xF3, 0x55, 0x46, 1, 0, 0x0C },   /* Gitarre    */
    { 0x01, 0x01, 0x14, 0x00, 0xF5, 0xF4, 0x45, 0x65, 0, 0, 0x0A },   /* Bass       */
    { 0x21, 0x21, 0x1E, 0x00, 0x71, 0x61, 0x14, 0x15, 0, 0, 0x0E },   /* Streicher  */
    { 0x22, 0x21, 0x1C, 0x00, 0x61, 0x52, 0x14, 0x16, 0, 0, 0x0C },   /* Ensemble   */
    { 0x21, 0x21, 0x16, 0x00, 0x91, 0x81, 0x13, 0x16, 0, 0, 0x0C },   /* Blech      */
    { 0x22, 0x21, 0x18, 0x00, 0x82, 0x71, 0x15, 0x16, 1, 0, 0x0E },   /* Rohrblatt  */
    { 0x22, 0x21, 0x2A, 0x00, 0x82, 0x71, 0x08, 0x07, 0, 0, 0x0E },   /* Floete     */
    { 0x21, 0x21, 0x10, 0x00, 0xF1, 0xF1, 0x05, 0x07, 2, 0, 0x0E },   /* Synth-Lead */
    { 0x21, 0x22, 0x1C, 0x00, 0x41, 0x31, 0x23, 0x24, 0, 0, 0x0C },   /* Synth-Pad  */
    { 0x23, 0x21, 0x1A, 0x00, 0x51, 0x41, 0x25, 0x26, 1, 1, 0x0E },   /* Effekte    */
    { 0x01, 0x03, 0x18, 0x00, 0xF4, 0xF3, 0x45, 0x56, 1, 0, 0x0C },   /* Ethno      */
    { 0x00, 0x01, 0x10, 0x00, 0xF6, 0xF5, 0x58, 0x47, 0, 0, 0x08 },   /* Pauke usw. */
    { 0x0E, 0x01, 0x10, 0x00, 0xF4, 0xF4, 0x36, 0x47, 3, 0, 0x0E }    /* Geraeusche */
};
static const struct patch drum[5] = {
    { 0x00, 0x00, 0x0B, 0x00, 0xF8, 0xF6, 0x67, 0x67, 0, 0, 0x08 },   /* Bassdrum   */
    { 0x0F, 0x01, 0x00, 0x00, 0xF8, 0xF8, 0x87, 0x78, 0, 0, 0x0E },   /* Snare      */
    { 0x0F, 0x0F, 0x00, 0x08, 0xFA, 0xFA, 0xA9, 0xAA, 0, 0, 0x0E },   /* Hi-Hat     */
    { 0x01, 0x01, 0x0E, 0x00, 0xF8, 0xF6, 0x56, 0x56, 0, 0, 0x08 },   /* Tom        */
    { 0x0E, 0x0F, 0x00, 0x04, 0xF2, 0xF2, 0x63, 0x64, 0, 0, 0x0E }    /* Becken     */
};

static const unsigned char opoff[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

static struct voice {
    unsigned char on, ch, note;
    unsigned age;
} vc[18];
static unsigned nvc, age_ctr;
static unsigned char prog[16], vol[16];

static void voice_off(unsigned v)
{
    unsigned b = v >= 9 ? 0x100 : 0, c = v % 9;
    unsigned f = fnum[vc[v].note];
    fm(b | (0xB0 + c), (f >> 8) & 0x1F);    /* Taste los, Tonhoehe bleibt */
    vc[v].on = 0;
}

static void note_on(unsigned ch, unsigned note, unsigned vel)
{
    const struct patch *p;
    unsigned v, best = 0, b, c, o, att, f, n = note;

    if (ch == 9) {                      /* Schlagzeug: fester Klang       */
        if (note == 35 || note == 36)       p = &drum[0], n = 36;
        else if (note >= 37 && note <= 40)  p = &drum[1], n = 60;
        else if (note == 42 || note == 44 || note == 46) p = &drum[2], n = 84;
        else if (note == 49 || note == 51 || note == 52 || note == 53 ||
                 note == 55 || note == 57 || note == 59) p = &drum[4], n = 90;
        else                                 p = &drum[3];
    } else
        p = &fam[prog[ch] >> 3];
    for (v = 0; v < nvc; v++)           /* frei, sonst die aelteste       */
        if (!vc[v].on)
            break;
    if (v == nvc) {
        for (v = 1; v < nvc; v++)
            if ((unsigned)(age_ctr - vc[v].age) > (unsigned)(age_ctr - vc[best].age))
                best = v;
        v = best;
        voice_off(v);
    }
    b = v >= 9 ? 0x100 : 0;
    c = v % 9;
    o = opoff[c];
    /* Lautstaerke: Anschlag * Kanal -> Daempfung des Traegers */
    att = (127 - (unsigned)(mul16(vel, vol[ch]) >> 7)) >> 2;
    fm(b | (0x20 + o), p->m20);
    fm(b | (0x23 + o), p->c20);
    fm(b | (0x40 + o), (p->c0 & 1) ? ((p->m40 & 0x3F) + att > 63 ? 63 : p->m40 + att) : p->m40);
    fm(b | (0x43 + o), (p->c40 & 0x3F) + att > 63 ? 63 | (p->c40 & 0xC0) : p->c40 + att);
    fm(b | (0x60 + o), p->m60);
    fm(b | (0x63 + o), p->c60);
    fm(b | (0x80 + o), p->m80);
    fm(b | (0x83 + o), p->c80);
    fm(b | (0xE0 + o), p->me0);
    fm(b | (0xE3 + o), p->ce0);
    fm(b | (0xC0 + c), p->c0 | (opl3 ? 0x30 : 0));
    f = fnum[n & 127];
    fm(b | (0xA0 + c), f & 0xFF);
    fm(b | (0xB0 + c), 0x20 | ((f >> 8) & 0x1F));
    vc[v].on = 1;
    vc[v].ch = (unsigned char)ch;
    vc[v].note = (unsigned char)n;
    vc[v].age = ++age_ctr;
}

static void note_off(unsigned ch, unsigned note)
{
    unsigned v;
    if (ch == 9)
        return;                         /* Schlagzeug klingt aus          */
    for (v = 0; v < nvc; v++)
        if (vc[v].on && vc[v].ch == ch && vc[v].note == note) {
            voice_off(v);
            return;
        }
}

static void all_off(unsigned ch)
{
    unsigned v;
    for (v = 0; v < nvc; v++)
        if (vc[v].on && (ch == 0xFF || vc[v].ch == ch))
            voice_off(v);
}

static int fm_start(void)
{
    unsigned r = FmOpen(), k;
    if (FAPI_FAILED(r)) {
        err(r == FAPI_ERR_INUSE ? "FM-Synthese belegt" : "keine FM-Synthese (OPL)", "");
        return -1;
    }
    opl3 = (AudCaps() & AUDCAP_OPL3) != 0;
    nvc = opl3 ? 18 : 9;
    if (opl3)
        fm(0x105, 1);                   /* OPL3-Betrieb: 18 Stimmen       */
    fm(0x01, 0x20);                     /* Wellenformen erlaubt (OPL2)    */
    for (k = 0; k < 18; k++)
        vc[k].on = 0;
    return 0;
}

/* ---- MIDI-Datei ------------------------------------------------------- */
#define NTRK 32
static unsigned mseg;                   /* Datei in einem eigenen Segment */
static unsigned long msize;
static struct {
    unsigned pos, end;
    unsigned long next;                 /* Zeit des naechsten Ereignisses */
    unsigned char rs, done;
} trk[NTRK];
static unsigned ntrk;
static int use_mpu;
static unsigned char mb[512];
static unsigned mbn;

static unsigned char MB(unsigned o)
{
    return ((unsigned char __far *)MK_FP(mseg, 0))[o];
}

static unsigned long vlq(unsigned *o, unsigned end)
{
    unsigned long v = 0;
    unsigned char c;
    do {
        if (*o >= end)
            return v;
        c = MB((*o)++);
        v = (v << 7) | (c & 0x7F);
    } while (c & 0x80);
    return v;
}

static void mpu_out(unsigned char c)
{
    mb[mbn++] = c;
    if (mbn == sizeof(mb)) {
        MidiWrite(mb, mbn);
        mbn = 0;
    }
}

static void out_flush(void)
{
    if (use_mpu) {
        if (mbn)
            MidiWrite(mb, mbn);
        mbn = 0;
    } else
        fm_flush();
}

static void midi_event(unsigned char st, unsigned char d1, unsigned char d2)
{
    unsigned ch = st & 15;
    if (use_mpu) {
        mpu_out(st);
        mpu_out(d1);
        if ((st & 0xF0) != 0xC0 && (st & 0xF0) != 0xD0)
            mpu_out(d2);
        return;
    }
    switch (st & 0xF0) {
    case 0x90:
        if (d2) {
            note_on(ch, d1, d2);
            break;
        }
        /* Anschlag 0 = loslassen */
    case 0x80:
        note_off(ch, d1);
        break;
    case 0xB0:
        if (d1 == 7)
            vol[ch] = d2;
        else if (d1 == 120 || d1 == 123)
            all_off(ch);
        break;
    case 0xC0:
        prog[ch] = d1;
        break;
    }
}

static void midi_file(void)
{
    unsigned long tempo = 500000UL, n;
    unsigned div, ust, k, o, t, best, i;
    unsigned long cur = 0, d;
    unsigned char c, st, d1, d2, typ;
    unsigned r;

    /* ganze Datei in ein Segment (hoechstens 64 KB); DosRead nur ins DS */
    DosSeek(h, &zero);
    msize = 0;
    mseg = DosAllocSeg(0);
    if (FAPI_FAILED(mseg) || !mseg) {
        err("nicht gen\x81gend Arbeitsspeicher", "");
        return;
    }
    while ((r = DosRead(h, buf, sizeof(buf))) != 0 && !FAPI_FAILED(r) && msize + r <= 0xFFF0UL) {
        unsigned char __far *dst = MK_FP(mseg, (unsigned)msize);
        for (k = 0; k < r; k++)
            dst[k] = buf[k];
        msize += r;
    }
    if (msize < 14 || MB(0) != 'M' || MB(1) != 'T' || MB(2) != 'h' || MB(3) != 'd') {
        err("keine MIDI-Datei - ", name);
        goto out;
    }
    div = (MB(12) << 8) | MB(13);
    if (div & 0x8000 || div == 0) {
        err("SMPTE-Zeitbasis wird nicht unterst\x81tzt", "");
        goto out;
    }
    o = 8 + ((MB(6) << 8) | MB(7));
    for (ntrk = 0; ntrk < NTRK && (unsigned long)o + 8 <= msize; ) {
        n = ((unsigned long)((MB(o + 4) << 8) | MB(o + 5)) << 16) | ((MB(o + 6) << 8) | MB(o + 7));
        if (MB(o) == 'M' && MB(o + 1) == 'T' && MB(o + 2) == 'r' && MB(o + 3) == 'k') {
            trk[ntrk].pos = o + 8;
            trk[ntrk].end = (unsigned long)o + 8 + n > msize ? (unsigned)msize : (unsigned)(o + 8 + n);
            trk[ntrk].rs = 0;
            trk[ntrk].done = 0;
            trk[ntrk].next = vlq(&trk[ntrk].pos, trk[ntrk].end);
            ntrk++;
        }
        if ((unsigned long)o + 8 + n > 0xFFF0UL)
            break;
        o += 8 + (unsigned)n;
    }
    use_mpu = (AudCaps() & AUDCAP_MPU401) && !o_fm;
    if (use_mpu) {
        r = MidiOpen();
        if (FAPI_FAILED(r)) {
            err(r == FAPI_ERR_INUSE ? "MIDI belegt" : "MIDI nicht m\x94glich", "");
            goto out;
        }
    } else if (fm_start())
        goto out;
    for (k = 0; k < 16; k++) {
        prog[k] = 0;
        vol[k] = 100;
    }
    if (!quiet) {
        puts(use_mpu ? "  MIDI \x81" "ber MPU-401, " : (opl3 ? "  MIDI \x81" "ber FM (OPL3), " : "  MIDI \x81" "ber FM (OPL2), "));
        put_num(ntrk, 0, 0);
        puts(" Spur(en)\r\n");
    }
    ust = sdiv(tempo, div);
    t_start = DosQTicks();
    t_now10 = 0;
    t_us = 0;
    for (;;) {
        best = 0xFFFF;                  /* Spur mit dem fruehesten Ereignis */
        for (t = 0; t < ntrk; t++)
            if (!trk[t].done && (best == 0xFFFF || trk[t].next < trk[best].next))
                best = t;
        if (best == 0xFFFF)
            break;
        if (trk[best].next > cur) {     /* Zeit vergeht: erst ausgeben    */
            out_flush();
            d = trk[best].next - cur;
            while (d) {
                k = d > 60000UL ? 60000u : (unsigned)d;
                time_add_us(mul16(k, ust));
                d -= k;
            }
            cur = trk[best].next;
            if (wait_until()) {
                stop = 1;
                break;
            }
        }
        i = best;
        if (trk[i].pos >= trk[i].end) {
            trk[i].done = 1;
            continue;
        }
        c = MB(trk[i].pos);
        if (c & 0x80) {
            st = c;
            trk[i].pos++;
            if (c < 0xF0)
                trk[i].rs = c;
        } else
            st = trk[i].rs;
        if (st == 0xFF) {               /* Meta: Tempo, Spurende          */
            typ = MB(trk[i].pos++);
            n = vlq(&trk[i].pos, trk[i].end);
            if (typ == 0x51 && n >= 3) {
                tempo = ((unsigned long)MB(trk[i].pos) << 16) |
                        ((unsigned)MB(trk[i].pos + 1) << 8) | MB(trk[i].pos + 2);
                ust = sdiv(tempo, div);
            } else if (typ == 0x2F)
                trk[i].done = 1;
            trk[i].pos += (unsigned)n;
        } else if (st == 0xF0 || st == 0xF7) {  /* SysEx                  */
            n = vlq(&trk[i].pos, trk[i].end);
            if (use_mpu) {
                if (st == 0xF0)
                    mpu_out(0xF0);
                for (k = 0; k < (unsigned)n; k++)
                    mpu_out(MB(trk[i].pos + k));
            }
            trk[i].pos += (unsigned)n;
        } else if (st & 0x80) {
            d1 = MB(trk[i].pos++);
            d2 = 0;
            if ((st & 0xF0) != 0xC0 && (st & 0xF0) != 0xD0)
                d2 = MB(trk[i].pos++);
            midi_event(st, d1, d2);
        } else {
            trk[i].done = 1;            /* kaputt: Spur beenden           */
            continue;
        }
        if (!trk[i].done && trk[i].pos < trk[i].end)
            trk[i].next += vlq(&trk[i].pos, trk[i].end);
        else
            trk[i].done = 1;
    }
    out_flush();
    if (!stop) {                        /* ausklingen lassen              */
        t_now10 += 100;
        wait_until();
    }
    if (use_mpu)
        MidiClose();
    else
        FmClose();
out:
    DosFreeSeg(mseg);
}

/* ---- IMF -------------------------------------------------------------- */
static void imf_file(void)
{
    unsigned len, k, n, delay, imf_us;
    unsigned long left;

    DosSeek(h, &zero);
    if (!rd(buf, 2))
        return;
    len = buf[0] | (buf[1] << 8);       /* Typ 1: Laenge vorn, Typ 0: nicht */
    if (len == 0 || (len & 3)) {
        DosSeek(h, &zero);
        left = 0xFFFFFFFFUL;
    } else
        left = len;
    imf_us = sdiv(1000000UL, imf_rate);     /* Mikrosekunden je IMF-Takt */
    if (fm_start())
        return;
    if (!quiet) {
        puts(opl3 ? "  IMF \x81" "ber FM (OPL3), " : "  IMF \x81" "ber FM (OPL2), ");
        put_num(imf_rate, 0, 0);
        puts(" Hz\r\n");
    }
    t_start = DosQTicks();
    t_now10 = 0;
    t_us = 0;
    while (left && !stop) {
        n = DosRead(h, buf, left < sizeof(buf) ? (unsigned)left & ~3u : sizeof(buf));
        if (FAPI_FAILED(n) || n < 4)
            break;
        left -= n;
        for (k = 0; k + 3 < n; k += 4) {
            fm(buf[k], buf[k + 1]);
            delay = buf[k + 2] | (buf[k + 3] << 8);
            if (delay) {
                fm_flush();
                time_add_us(mul16(delay, imf_us));
                if (wait_until()) {
                    stop = 1;
                    break;
                }
            }
        }
    }
    fm_flush();
    FmClose();
}
