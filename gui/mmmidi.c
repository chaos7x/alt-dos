/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - MMSYSTEM: MIDI-Ausgabe (midiOut*) und der Sequenzer fuer
 * MCI (Standard-MIDI-Dateien, mci.c).
 *
 * Geraete wie Windows 3.1 mit einer Sound Blaster: ein MIDI-Anschluss
 * (MPU-401 bzw. die Wavetable der AWE, AUD_MIDI* - der Treiber entscheidet)
 * und die FM-Synthese (OPL, AUD_FM*). Fuer FM spielt die GUI selbst: die
 * Klangfamilien, das Schlagzeug und die Stimmenverwaltung stammen aus
 * PLAY.EXE (apps/midiplay.h), hier mit dem Zustand in einem eigenen
 * Segment. Der MIDI-Mapper (Geraet -1) nimmt das erste vorhandene.
 *
 * Der Kernel gibt je AT-DOS-Task einen MIDI- und einen FM-Platz her - die
 * GUI ist ein Task: ein Programm (oder der Sequenzer) zur Zeit.
 *
 * Sequenzer: Datei bis 48 KB in einem Segment, Spuren zusammengefuehrt,
 * Tempo-Wechsel, Zeit in Mikrosekunden. Gespielt wird aus mm_pump nach
 * timeGetTime; Springen spielt die Datei stumm bis zur Stelle durch
 * (Programme und Regler gehen dabei hinaus, Noten nicht).
 */
#include "mm.h"

#define HMO     0x0D01
#define MHDR_DONE       0x01
#define MHDR_PREPARED   0x02
#define MHDR_INQUEUE    0x04
#define MIDIERR_NOTREADY 67

typedef struct midihdr {
    char FAR *lpData;
    DWORD dwBufferLength, dwBytesRecorded, dwUser, dwFlags;
    struct midihdr FAR *lpNext;
    DWORD reserved;
} MIDIHDR;

/* Patch aus PLAY: Modulator/Traeger 20h, 40h, 60h, 80h, E0h, dazu C0h */
struct patch {
    BYTE m20, c20, m40, c40, m60, c60, m80, c80, me0, ce0, c0;
};
static const struct patch __far fam[16] = {
    { 0x01, 0x01, 0x1C, 0x00, 0xF3, 0xF2, 0x34, 0x45, 0, 0, 0x0A },
    { 0x07, 0x01, 0x22, 0x00, 0xF5, 0xF5, 0x25, 0x35, 0, 0, 0x06 },
    { 0x21, 0x21, 0x14, 0x00, 0xF0, 0xF0, 0x06, 0x06, 0, 0, 0x01 },
    { 0x01, 0x01, 0x1A, 0x00, 0xF4, 0xF3, 0x55, 0x46, 1, 0, 0x0C },
    { 0x01, 0x01, 0x14, 0x00, 0xF5, 0xF4, 0x45, 0x65, 0, 0, 0x0A },
    { 0x21, 0x21, 0x1E, 0x00, 0x71, 0x61, 0x14, 0x15, 0, 0, 0x0E },
    { 0x22, 0x21, 0x1C, 0x00, 0x61, 0x52, 0x14, 0x16, 0, 0, 0x0C },
    { 0x21, 0x21, 0x16, 0x00, 0x91, 0x81, 0x13, 0x16, 0, 0, 0x0C },
    { 0x22, 0x21, 0x18, 0x00, 0x82, 0x71, 0x15, 0x16, 1, 0, 0x0E },
    { 0x22, 0x21, 0x2A, 0x00, 0x82, 0x71, 0x08, 0x07, 0, 0, 0x0E },
    { 0x21, 0x21, 0x10, 0x00, 0xF1, 0xF1, 0x05, 0x07, 2, 0, 0x0E },
    { 0x21, 0x22, 0x1C, 0x00, 0x41, 0x31, 0x23, 0x24, 0, 0, 0x0C },
    { 0x23, 0x21, 0x1A, 0x00, 0x51, 0x41, 0x25, 0x26, 1, 1, 0x0E },
    { 0x01, 0x03, 0x18, 0x00, 0xF4, 0xF3, 0x45, 0x56, 1, 0, 0x0C },
    { 0x00, 0x01, 0x10, 0x00, 0xF6, 0xF5, 0x58, 0x47, 0, 0, 0x08 },
    { 0x0E, 0x01, 0x10, 0x00, 0xF4, 0xF4, 0x36, 0x47, 3, 0, 0x0E }
};
static const struct patch __far drum[5] = {
    { 0x00, 0x00, 0x0B, 0x00, 0xF8, 0xF6, 0x67, 0x67, 0, 0, 0x08 },
    { 0x0F, 0x01, 0x00, 0x00, 0xF8, 0xF8, 0x87, 0x78, 0, 0, 0x0E },
    { 0x0F, 0x0F, 0x00, 0x08, 0xFA, 0xFA, 0xA9, 0xAA, 0, 0, 0x0E },
    { 0x01, 0x01, 0x0E, 0x00, 0xF8, 0xF6, 0x56, 0x56, 0, 0, 0x08 },
    { 0x0E, 0x0F, 0x00, 0x04, 0xF2, 0xF2, 0x63, 0x64, 0, 0, 0x0E }
};
static const char __far s_busy[] = "Das MIDI-Ger\xE4t ist noch besch\xE4" "ftigt.";
static const char __far s_map[] = "MIDI-Mapper";
static const char __far s_port[] = "AT-DOS MIDI-Anschluss";
static const char __far s_fm[] = "AT-DOS FM-Synthese";
static const BYTE __far opoff[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
/* F-Nummer | Block << 10 je MIDI-Note (wie PLAY) */
static const WORD __far fntab[128] = {
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

#define NTRK    32
struct trk { WORD pos, end; DWORD next; BYTE rs, done; };
struct mdst {
    /* Ausgabe */
    BYTE  who;                          /* 0 zu, 1 Programm, 2 Sequenzer  */
    BYTE  kind;                         /* 1 MIDI-Anschluss, 2 FM         */
    BYTE  opl3, nvc;
    int   task;
    DWORD cb, inst, flags;
    BYTE  rs;                           /* laufender Status (ShortMsg)    */
    struct fapi_fmreg fmq[64];
    WORD  fmn;
    struct { BYTE on, ch, note; WORD age; } vc[18];
    WORD  age;
    BYTE  prog[16], vol[16];
    BYTE  mb[256];
    WORD  mbn;
    /* Sequenzer */
    WORD  fsel;                         /* Datei                          */
    WORD  fsize;
    WORD  div;                          /* Ticks je Viertel               */
    DWORD lenus;                        /* Laenge in Mikrosekunden        */
    BYTE  ntrk, play, ended, mute;
    struct trk t[NTRK];
    DWORD tick, us, tempo;              /* Stand der Auswertung           */
    DWORD t0;                           /* timeGetTime bei Stelle 0       */
    DWORD pos;                          /* Stelle (ms), wenn angehalten   */
    DWORD to;                           /* Ende der Wiedergabe (ms)       */
    char  path[128];
    BYTE  caps[52];
};
static WORD md_sel;
#define M ((struct mdst FAR *)MKFP(md_sel, 0))

static int md_init(void)
{
    if (!md_sel) {
        md_sel = seg_new(sizeof(struct mdst));
        if (!md_sel)
            return 0;
        fmemset(M, 0, sizeof(struct mdst));
    }
    return 1;
}

/* Geraete: 0 = Anschluss (wenn da), dann FM */
static BYTE dev_kind(WORD dev)
{
    WORD c = mm_caps();
    BYTE k[2], n = 0;
    if (c & AUDCAP_MPU401) k[n++] = 1;
    if (c & (AUDCAP_OPL2 | AUDCAP_OPL3)) k[n++] = 2;
    if (dev == 0xFFFF)
        dev = 0;
    return dev < n ? k[dev] : 0;
}
WORD md_ndev(void)
{
    WORD c = mm_caps(), n = 0;
    if (c & AUDCAP_MPU401) n++;
    if (c & (AUDCAP_OPL2 | AUDCAP_OPL3)) n++;
    return n;
}

/* ---- FM ---------------------------------------------------------------- */
static void fm_flush(void)
{
    if (M->fmn)
        fapi_sel(FAPI_AUDIO, md_sel, AUD_FMWRITE, OFFOF(M->fmq), M->fmn);
    M->fmn = 0;
}
static void fm(WORD reg, BYTE val)
{
    M->fmq[M->fmn].reg = reg;
    M->fmq[M->fmn].val = val;
    if (++M->fmn == 64)
        fm_flush();
}
#define fnum(n) fntab[(n) & 127]
static void voice_off(WORD v)
{
    WORD b = v >= 9 ? 0x100 : 0;
    fm(b | (0xB0 + v % 9), (BYTE)((fnum(M->vc[v].note) >> 8) & 0x1F));
    M->vc[v].on = 0;
}
static void fm_noteon(BYTE ch, BYTE note, BYTE vel)
{
    const struct patch __far *p;
    WORD v, best = 0, b, c, o, att, f;
    BYTE n = note;
    if (ch == 9) {
        if (note == 35 || note == 36)       p = &drum[0], n = 36;
        else if (note >= 37 && note <= 40)  p = &drum[1], n = 60;
        else if (note == 42 || note == 44 || note == 46) p = &drum[2], n = 84;
        else if (note == 49 || note == 51 || note == 52 || note == 53 ||
                 note == 55 || note == 57 || note == 59) p = &drum[4], n = 90;
        else                                 p = &drum[3];
    } else
        p = &fam[M->prog[ch] >> 3];
    for (v = 0; v < M->nvc; v++)
        if (!M->vc[v].on)
            break;
    if (v == M->nvc) {
        for (v = 1; v < M->nvc; v++)
            if ((WORD)(M->age - M->vc[v].age) > (WORD)(M->age - M->vc[best].age))
                best = v;
        v = best;
        voice_off(v);
    }
    b = v >= 9 ? 0x100 : 0;
    c = v % 9;
    o = opoff[c];
    att = (127 - (WORD)(((WORD)vel * M->vol[ch]) >> 7)) >> 2;
    fm(b | (0x20 + o), p->m20);
    fm(b | (0x23 + o), p->c20);
    fm(b | (0x40 + o), (p->c0 & 1) ? (BYTE)((p->m40 & 0x3F) + att > 63 ? 63 : p->m40 + att) : p->m40);
    fm(b | (0x43 + o), (BYTE)((p->c40 & 0x3F) + att > 63 ? 63 | (p->c40 & 0xC0) : p->c40 + att));
    fm(b | (0x60 + o), p->m60);
    fm(b | (0x63 + o), p->c60);
    fm(b | (0x80 + o), p->m80);
    fm(b | (0x83 + o), p->c80);
    fm(b | (0xE0 + o), p->me0);
    fm(b | (0xE3 + o), p->ce0);
    fm(b | (0xC0 + c), (BYTE)(p->c0 | (M->opl3 ? 0x30 : 0)));
    f = fnum(n & 127);
    fm(b | (0xA0 + c), (BYTE)f);
    fm(b | (0xB0 + c), (BYTE)(0x20 | ((f >> 8) & 0x1F)));
    M->vc[v].on = 1;
    M->vc[v].ch = ch;
    M->vc[v].note = n;
    M->vc[v].age = ++M->age;
}
static void fm_noteoff(BYTE ch, BYTE note)
{
    WORD v;
    if (ch == 9)
        return;
    for (v = 0; v < M->nvc; v++)
        if (M->vc[v].on && M->vc[v].ch == ch && M->vc[v].note == note) {
            voice_off(v);
            return;
        }
}
static void fm_alloff(BYTE ch)
{
    WORD v;
    for (v = 0; v < M->nvc; v++)
        if (M->vc[v].on && (ch == 0xFF || M->vc[v].ch == ch))
            voice_off(v);
}
static void fm_msg(BYTE st, BYTE d1, BYTE d2)
{
    BYTE ch = st & 15;
    switch (st & 0xF0) {
    case 0x90:
        if (d2) { fm_noteon(ch, d1, d2); break; }
        /* Anschlag 0 = los */
    case 0x80:
        fm_noteoff(ch, d1);
        break;
    case 0xB0:
        if (d1 == 7)
            M->vol[ch] = d2;
        else if (d1 == 121)
            M->vol[ch] = 100;
        else if (d1 >= 120)
            fm_alloff(ch);
        break;
    case 0xC0:
        M->prog[ch] = d1;
        break;
    }
}

/* ---- Ausgabe ------------------------------------------------------------ */
static void mpu_flush(void)
{
    if (M->mbn)
        fapi_sel(FAPI_AUDIO, md_sel, AUD_MIDIWRITE, OFFOF(M->mb), M->mbn);
    M->mbn = 0;
}
static void mpu_byte(BYTE b)
{
    M->mb[M->mbn++] = b;
    if (M->mbn == sizeof(M->mb))
        mpu_flush();
}
static void out_msg(BYTE st, BYTE d1, BYTE d2)
{
    if (M->kind == 1) {
        mpu_byte(st);
        mpu_byte(d1);
        if ((st & 0xE0) != 0xC0)
            mpu_byte(d2);
    } else
        fm_msg(st, d1, d2);
}
static void out_flush(void)
{
    if (M->kind == 1)
        mpu_flush();
    else
        fm_flush();
}

WORD md_open(WORD dev, BYTE who)
{
    WORD k, r, i;
    if (!md_init())
        return MMSYSERR_NOMEM;
    if (M->who)
        return MMSYSERR_ALLOCATED;
    k = dev_kind(dev);
    if (!k)
        return md_ndev() ? MMSYSERR_BADDEVICEID : MMSYSERR_NODRIVER;
    r = fapi_call(FAPI_AUDIO, k == 1 ? AUD_MIDIOPEN : AUD_FMOPEN, 0, 0);
    if (FAPI_FAILED(r))
        return r == FAPI_ERR_INUSE ? MMSYSERR_ALLOCATED : MMSYSERR_NODRIVER;
    M->who = who;
    M->kind = (BYTE)k;
    M->rs = 0;
    M->fmn = M->mbn = 0;
    for (i = 0; i < 16; i++) {
        M->prog[i] = 0;
        M->vol[i] = 100;
    }
    if (k == 2) {
        M->opl3 = (mm_caps() & AUDCAP_OPL3) != 0;
        M->nvc = M->opl3 ? 18 : 9;
        for (i = 0; i < 18; i++)
            M->vc[i].on = 0;
        if (M->opl3)
            fm(0x105, 1);
        fm(0x01, 0x20);
        fm_flush();
    }
    return 0;
}
void md_reset(void)
{
    BYTE c;
    if (!md_sel || !M->who)
        return;
    if (M->kind == 2)
        fm_alloff(0xFF);
    else
        for (c = 0; c < 16; c++) {
            out_msg((BYTE)(0xB0 | c), 123, 0);      /* alle Noten aus    */
            out_msg((BYTE)(0xB0 | c), 121, 0);      /* Regler zurueck    */
        }
    out_flush();
}
void md_close(void)
{
    if (!md_sel || !M->who)
        return;
    md_reset();
    fapi_call(FAPI_AUDIO, M->kind == 1 ? AUD_MIDICLOSE : AUD_FMCLOSE, 0, 0);
    M->who = 0;
}
void md_short(DWORD m)
{
    BYTE st = (BYTE)m, d1 = (BYTE)(m >> 8), d2 = (BYTE)(m >> 16);
    if (!md_sel || !M->who)
        return;
    if (st < 0x80) {                    /* laufender Status            */
        d2 = d1; d1 = st; st = M->rs;
        if (st < 0x80)
            return;
    }
    if (st >= 0xF0) {                   /* System: nur an den Anschluss */
        if (M->kind == 1) {
            mpu_byte(st);
            mpu_flush();
        }
        return;
    }
    M->rs = st;
    out_msg(st, d1, d2);
    out_flush();
}

/* ---- midiOut ------------------------------------------------------------ */
static int mo_ok(WORD h) { return md_sel && M->who == 1 && h == HMO; }

WORD API midiOutGetNumDevs(void) { return md_ndev(); }
WORD API midiOutGetDevCaps(WORD dev, void FAR *p, WORD sz)
{
    BYTE FAR *c;
    BYTE k = dev_kind(dev);
    if (!k)
        return md_ndev() ? MMSYSERR_BADDEVICEID : MMSYSERR_NODRIVER;
    if (!md_init())
        return MMSYSERR_NOMEM;
    c = M->caps;
    fmemset(c, 0, 50);
    c[0] = 2; c[2] = k == 1 ? 3 : 2;            /* wMid, wPid (Creative)  */
    c[4] = 0x0A; c[5] = 3;
    if (dev == 0xFFFF) {
        fstrncpy((char FAR *)&c[6], s_map, 32);
        c[38] = 5;                              /* MOD_MAPPER             */
    } else if (k == 1) {
        fstrncpy((char FAR *)&c[6], s_port, 32);
        c[38] = 1;                              /* MOD_MIDIPORT           */
    } else {
        fstrncpy((char FAR *)&c[6], s_fm, 32);
        c[38] = 4;                              /* MOD_FMSYNTH            */
        c[40] = (mm_caps() & AUDCAP_OPL3) ? 18 : 9;
        c[42] = c[40];
    }
    *(WORD FAR *)&c[44] = 0xFFFF;
    fmemcpy(p, c, sz < 50 ? sz : 50);
    return 0;
}
WORD API midiOutGetErrorText(WORD e, LPSTR buf, WORD n)
{
    if (e == MIDIERR_NOTREADY && buf && n) {
        fstrncpy(buf, s_busy, n);
        return 0;
    }
    return mm_errtext(e, buf, n);
}
WORD API midiOutOpen(WORD FAR *ph, WORD dev, DWORD cb, DWORD inst, DWORD flags)
{
    WORD r;
    if (md_sel && M->who == 2)          /* Sequenzer haelt ihn          */
        sq_halt();
    if ((r = md_open(dev, 1)) != 0)
        return r;
    M->task = cur_task;
    M->cb = (flags & CALLBACK_TYPEMASK) ? cb : 0;
    M->inst = inst;
    M->flags = flags;
    if (ph)
        *ph = HMO;
    mm_cbopen(M->cb, flags, HMO, MM_MOM_OPEN, inst);
    return 0;
}
WORD API midiOutClose(WORD h)
{
    if (!mo_ok(h))
        return MMSYSERR_INVALHANDLE;
    md_close();
    mm_cbopen(M->cb, M->flags, HMO, MM_MOM_CLOSE, M->inst);
    return 0;
}
WORD API midiOutPrepareHeader(WORD h, MIDIHDR FAR *m, WORD sz)
{
    (void)sz;
    if (!mo_ok(h))
        return MMSYSERR_INVALHANDLE;
    m->dwFlags |= MHDR_PREPARED;
    return 0;
}
WORD API midiOutUnprepareHeader(WORD h, MIDIHDR FAR *m, WORD sz)
{
    (void)sz;
    if (!mo_ok(h))
        return MMSYSERR_INVALHANDLE;
    m->dwFlags &= ~MHDR_PREPARED;
    return 0;
}
WORD API midiOutShortMsg(WORD h, DWORD m)
{
    if (!mo_ok(h))
        return MMSYSERR_INVALHANDLE;
    md_short(m);
    return 0;
}
/* System-Exklusiv u. a.: gleich hinaus, dann fertig melden */
WORD API midiOutLongMsg(WORD h, MIDIHDR FAR *m, WORD sz)
{
    DWORD i;
    (void)sz;
    if (!mo_ok(h))
        return MMSYSERR_INVALHANDLE;
    if (!(m->dwFlags & MHDR_PREPARED))
        return 64;                      /* MIDIERR_UNPREPARED          */
    if (M->kind == 1) {
        const BYTE FAR *b = (const BYTE FAR *)m->lpData;
        for (i = 0; i < m->dwBufferLength && OFFOF(b) + (WORD)i >= OFFOF(b); i++)
            mpu_byte(b[(WORD)i]);
        mpu_flush();
    }
    m->dwFlags |= MHDR_DONE;
    mm_cbfire(M->cb, M->flags, M->task, HMO, MM_MOM_DONE, M->inst, (DWORD)m);
    return 0;
}
WORD API midiOutReset(WORD h)
{
    if (!mo_ok(h))
        return MMSYSERR_INVALHANDLE;
    md_reset();
    return 0;
}
WORD API midiOutGetVolume(WORD dev, DWORD FAR *v) { (void)dev; (void)v; return MMSYSERR_NOTSUPPORTED; }
WORD API midiOutSetVolume(WORD dev, DWORD v) { (void)dev; (void)v; return MMSYSERR_NOTSUPPORTED; }
WORD API midiOutCachePatches(WORD h, WORD b, WORD FAR *p, WORD f) { (void)h; (void)b; (void)p; (void)f; return 0; }
WORD API midiOutCacheDrumPatches(WORD h, WORD b, WORD FAR *p, WORD f) { (void)h; (void)b; (void)p; (void)f; return 0; }
WORD API midiOutGetID(WORD h, WORD FAR *id)
{
    if (!mo_ok(h))
        return MMSYSERR_INVALHANDLE;
    *id = M->kind == 1 || md_ndev() == 1 ? 0 : 1;
    return 0;
}
DWORD API midiOutMessage(WORD h, WORD m, DWORD a, DWORD b) { (void)h; (void)m; (void)a; (void)b; return MMSYSERR_NOTSUPPORTED; }

void md_task_end(int t)
{
    if (!md_sel)
        return;
    if (M->who == 1 && M->task == t)
        md_close();
}

/* ---- Sequenzer ----------------------------------------------------------- */
#define FB(o)   (((BYTE FAR *)MKFP(M->fsel, 0))[o])

static DWORD vlq(struct trk FAR *k)
{
    DWORD v = 0;
    BYTE b;
    do {
        if (k->pos >= k->end)
            return v;
        b = FB(k->pos++);
        v = (v << 7) | (b & 0x7F);
    } while (b & 0x80);
    return v;
}
static DWORD be32(WORD o)
{
    return ((DWORD)FB(o) << 24) | ((DWORD)FB(o + 1) << 16) | ((WORD)FB(o + 2) << 8) | FB(o + 3);
}
/* Stand auf den Anfang */
static void sq_rewind(void)
{
    WORD i, o = 14;
    M->ntrk = 0;
    for (i = 0; o + 8 <= M->fsize && M->ntrk < NTRK; i++) {
        DWORD len = be32(o + 4);
        struct trk FAR *k;
        if (o + 8 + len > M->fsize)
            len = M->fsize - o - 8;
        if (be32(o) == 0x4D54726BUL) {          /* "MTrk" */
            k = &M->t[M->ntrk++];
            k->pos = o + 8;
            k->end = (WORD)(o + 8 + len);
            k->rs = 0;
            k->done = 0;
            k->next = vlq(k);
        }
        o += 8 + (WORD)len;
    }
    M->tick = M->us = 0;
    M->tempo = 500000UL;
    M->ended = 0;
}
/* naechstes Ereignis auswerten; -> 0 Ende. out: Noten ausgeben */
static int sq_step(int out)
{
    struct trk FAR *k = 0;
    WORD i;
    BYTE st, d1, d2;
    DWORD dt;
    for (i = 0; i < M->ntrk; i++)
        if (!M->t[i].done && (!k || M->t[i].next < k->next))
            k = &M->t[i];
    if (!k)
        return 0;
    dt = k->next - M->tick;
    M->us += dt * (M->tempo / M->div) + dt * (M->tempo % M->div) / M->div;
    M->tick = k->next;
    if (k->pos >= k->end) {
        k->done = 1;
        return 1;
    }
    st = FB(k->pos);
    if (st & 0x80)
        k->pos++;
    else
        st = k->rs;
    if (st == 0xFF) {                           /* Meta                 */
        BYTE ty = FB(k->pos++);
        DWORD len = vlq(k);
        if (ty == 0x51 && len >= 3)
            M->tempo = ((DWORD)FB(k->pos) << 16) | ((WORD)FB(k->pos + 1) << 8) | FB(k->pos + 2);
        if (ty == 0x2F)
            k->done = 1;
        k->pos += (WORD)len;
    } else if (st == 0xF0 || st == 0xF7) {      /* System-Exklusiv      */
        DWORD len = vlq(k);
        k->pos += (WORD)len;
    } else if (st >= 0x80) {
        k->rs = st;
        d1 = FB(k->pos++) & 0x7F;
        d2 = 0;
        if ((st & 0xE0) != 0xC0)
            d2 = FB(k->pos++) & 0x7F;
        if (out && M->who == 2) {
            if (out == 2 && ((st & 0xF0) == 0x90 || (st & 0xF0) == 0x80))
                ;                               /* springen: keine Noten */
            else
                out_msg(st, d1, d2);
        }
    } else {
        k->done = 1;                            /* kaputt               */
        return 1;
    }
    if (!k->done && k->pos < k->end)
        k->next += vlq(k);
    else
        k->done = 1;
    return 1;
}
/* Zeit des naechsten Ereignisses in us (ohne es auszuwerten) */
static DWORD sq_nextus(void)
{
    DWORD best = 0xFFFFFFFFUL, dt;
    WORD i;
    for (i = 0; i < M->ntrk; i++)
        if (!M->t[i].done && M->t[i].next < best)
            best = M->t[i].next;
    if (best == 0xFFFFFFFFUL)
        return best;
    dt = best - M->tick;
    return M->us + dt * (M->tempo / M->div) + dt * (M->tempo % M->div) / M->div;
}

WORD sq_load(LPCSTR path)
{
    WORD h, n;
    if (!md_init())
        return MCIERR_OUT_OF_MEMORY;
    sq_unload();
    fstrncpy(M->path, path, sizeof(M->path));
    h = fapi_sel(FAPI_OPEN, md_sel, OFFOF(M->path), 0, 0);
    if (FAPI_FAILED(h))
        return MCIERR_FILE_NOT_FOUND;
    M->fsel = seg_new(0);                       /* 64 KB              */
    if (!M->fsel) {
        DosClose(h);
        return MCIERR_OUT_OF_MEMORY;
    }
    for (n = 0; n < 0xC000; ) {                 /* hoechstens 48 KB    */
        WORD r = fapi_sel(FAPI_READ, M->fsel, h, n, 0x4000);
        if (FAPI_FAILED(r) || !r)
            break;
        n += r;
    }
    DosClose(h);
    if (n > 64 && be32(0) == 0x52494646UL) {   /* RIFF RMID: MThd suchen */
        WORD o;
        for (o = 12; o + 4 < 256 && o + 4 < n; o++)
            if (be32(o) == 0x4D546864UL) {
                fmemmove(MKFP(M->fsel, 0), MKFP(M->fsel, o), n - o);
                n -= o;
                break;
            }
    }
    if (FAPI_FAILED(n) || n < 22 || be32(0) != 0x4D546864UL) {   /* "MThd" */
        sq_unload();
        return MCIERR_INVALID_FILE;
    }
    M->fsize = n;
    M->div = ((WORD)FB(12) << 8) | FB(13);
    if (M->div & 0x8000) {                      /* SMPTE: Ticks je s    */
        WORD fps = 256 - (M->div >> 8);
        /* Tempo 1 s je "Viertel": div = Ticks je Sekunde */
        M->div = fps * (M->div & 0xFF);
        M->tempo = 1000000UL;
    }
    if (!M->div)
        M->div = 96;
    /* Laenge: einmal durchrechnen */
    sq_rewind();
    while (sq_step(0))
        ;
    M->lenus = M->us;
    sq_rewind();
    M->pos = 0;
    return 0;
}
void sq_unload(void)
{
    if (!md_sel)
        return;
    sq_halt();
    if (M->fsel)
        seg_del(M->fsel);
    M->fsel = 0;
    M->fsize = 0;
}
DWORD sq_length(void) { return md_sel && M->fsel ? M->lenus / 1000 : 0; }
int sq_playing(void) { return md_sel && M->play; }
WORD sq_play(DWORD from, DWORD to)
{
    WORD r;
    if (!md_sel || !M->fsel)
        return MCIERR_FILE_NOT_FOUND;
    sq_halt();
    if (M->who)
        return MCIERR_SEQ_PORT_INUSE;
    if ((r = md_open(0xFFFF, 2)) != 0)
        return r == MMSYSERR_ALLOCATED ? MCIERR_SEQ_PORT_INUSE : MCIERR_SEQ_PORT_NONEXISTENT;
    if (to > sq_length())
        to = sq_length();
    if (from > to)
        from = to;
    /* bis zur Stelle stumm durchspielen: Programme/Regler gehen hinaus */
    sq_rewind();
    while (sq_nextus() / 1000 < from && sq_step(2))
        ;
    out_flush();
    M->t0 = timeGetTime() - from;
    M->to = to;
    M->play = 1;
    M->ended = 0;
    sq_pump();
    return 0;
}
DWORD sq_where(void)
{
    DWORD p;
    if (!md_sel)
        return 0;
    if (!M->play)
        return M->pos;
    p = timeGetTime() - M->t0;
    return p > M->to ? M->to : p;
}
void sq_halt(void)
{
    if (!md_sel || !M->play)
        return;
    M->pos = sq_where();
    M->play = 0;
    md_close();
}
/* aus mm_pump: faellige Ereignisse spielen; 1 = eben am Ende */
int sq_pump(void)
{
    DWORD now, nx;
    if (!md_sel || !M->play)
        return 0;
    now = timeGetTime() - M->t0;
    while ((nx = sq_nextus()) != 0xFFFFFFFFUL && nx / 1000 <= now && nx / 1000 <= M->to)
        sq_step(1);
    out_flush();
    if (now < M->to)
        return 0;
    M->pos = M->to;
    M->play = 0;
    M->ended = 1;
    md_close();
    return 1;
}
void sq_seek(DWORD ms)
{
    if (!md_sel)
        return;
    sq_halt();
    M->pos = ms > sq_length() ? sq_length() : ms;
}
int sq_ended(void)
{
    int e;
    if (!md_sel)
        return 0;
    e = M->ended;
    M->ended = 0;
    return e;
}
