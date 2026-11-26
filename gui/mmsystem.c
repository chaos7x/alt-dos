/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - MMSYSTEM (Windows 3.1): Ton fuer Win16-Programme.
 *
 * sndPlaySound (Datei, WIN.INI [sounds], Speicher, Schleife), waveOut*,
 * die Zeitfunktionen und die Wiedergabe fuer MCI waveaudio (mci.c). Ausgegeben wird ueber die Audio-FAPI
 * (FAPI_AUDIO, Treiber SB16.SYS). Der Kernel kennt je AT-DOS-Task einen
 * Strom - die GUI ist ein Task, also gibt es ein Wave-Geraet, das
 * sndPlaySound und waveOut sich teilen (wie bei Windows mit einer SB).
 *
 * Nachfuettern: die Hauptschleife ruft mm_pump(). AUD_WRITE wartet, bis
 * die Daten im Puffer der Karte liegen; die GUI schreibt deshalb nur so
 * viel, wie nach der Uhr (DosQTicks, 100 Hz) seither abgespielt sein kann
 * plus ein Vorrat von drei Abschnitten - laeuft die Karte langsamer als
 * die Uhr, wartet der Aufruf kurz und gleicht sich selbst aus. Ein Puffer
 * gilt als fertig (MM_WOM_DONE), wenn er nach dieser Schaetzung gespielt
 * ist, nicht schon beim Schreiben - sonst schnitte waveOutClose das Ende ab.
 *
 * Rueckrufe: Fenster (post_msg) und Task gleich aus der Hauptschleife,
 * Funktionen erst im Zusammenhang des Programms (mm_task in
 * GetMessage), auf dessen Stapel mit dessen DS - wie ein Zeitgeber.
 *
 * Zustand in einem eigenen Segment (DGROUP der GUI ist knapp).
 */
#include "mm.h"

WORD mm_sel;

short API GetProfileString(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR buf, short n);
int  path_find(LPCSTR name, LPSTR out, WORD n);

int mm_init(void)
{
    if (!mm_sel) {
        mm_sel = seg_new(sizeof(struct mmst));
        if (!mm_sel)
            return 0;
        fmemset(S, 0, sizeof(struct mmst));
    }
    return 1;
}

WORD mm_qticks(void) { return fapi_call(FAPI_QTICKS, 0, 0, 0); }
WORD mm_caps(void)
{
    WORD c = fapi_call(FAPI_AUDIO, AUD_CAPS, 0, 0);
    return FAPI_FAILED(c) ? 0 : c;
}

/* ---- Strom zur Karte ------------------------------------------------- */
static WORD fmt_ok(const PCMWAVEFORMAT FAR *f)
{
    if (f->wFormatTag != 1 || (f->nChannels != 1 && f->nChannels != 2) ||
        (f->wBitsPerSample != 8 && f->wBitsPerSample != 16) ||
        f->nSamplesPerSec < 4000 || f->nSamplesPerSec > 48000)
        return WAVERR_BADFORMAT;
    return 0;
}
static int st_open(const PCMWAVEFORMAT FAR *f, BYTE who)
{
    static struct fapi_audfmt af;
    WORD seg;
    if (fmt_ok(f))
        return WAVERR_BADFORMAT;
    af.rate = (WORD)f->nSamplesPerSec;
    af.channels = (BYTE)f->nChannels;
    af.bits = (BYTE)f->wBitsPerSample | AUD_SMALL;
    if (FAPI_FAILED(fapi_call(FAPI_AUDIO, AUD_OPEN, (WORD)&af, 0)))
        return WAVERR_BADFORMAT;
    S->owner = who;
    S->rate = af.rate;
    S->ch = af.channels;
    S->bits = (BYTE)f->wBitsPerSample;
    S->align = S->ch * (S->bits >> 3);
    S->bps = (DWORD)S->rate * S->align;
    /* SB16.SYS: Abschnitt rate/32 Werte (512..8192 Byte), vier davon */
    seg = (WORD)(S->bps >> 5);
    if (seg < 512) seg = 512;
    if (seg > 8192) seg = 8192;
    S->cap = (DWORD)seg * 3;
    S->backlog = S->rem = 0;
    S->last = mm_qticks();
    S->written = 0;
    return 0;
}
static void st_close(void)
{
    if (S->owner)
        fapi_call(FAPI_AUDIO, AUD_CLOSE, 0, 0);
    S->owner = 0;
}
/* Uhr nachfuehren: was seit dem letzten Mal gespielt sein kann */
static void st_clock(void)
{
    WORD now = mm_qticks(), dt = now - S->last;
    DWORD p;
    S->last = now;
    if (dt > 100)
        dt = 100;
    p = S->bps * dt + S->rem;            /* Byte * 100 */
    S->rem = p % 100;
    p /= 100;
    S->backlog = p >= S->backlog ? 0 : S->backlog - p;
}
/* bis zu n Byte von sel:off schreiben, soweit der Vorrat es zulaesst */
static WORD st_write(WORD sel, WORD off, WORD n)
{
    DWORD room = S->cap > S->backlog ? S->cap - S->backlog : 0;
    if (room < n)
        n = (WORD)room;
    n -= n % S->align;
    if (!n)
        return 0;
    fapi_sel(FAPI_AUDIO, sel, AUD_WRITE, off, n);
    S->backlog += n;
    S->written += n;
    return n;
}
/* Takte, bis der Vorrat gespielt ist */
static WORD st_lag(void)
{
    return S->bps ? (WORD)(S->backlog * 100 / S->bps) + 1 : 0;
}

/* ---- Rueckrufe -------------------------------------------------------- */
void mm_cbfire(DWORD cb, DWORD flags, int t, WORD h, WORD msg, DWORD inst, DWORD p1)
{
    switch (flags & CALLBACK_TYPEMASK) {
    case CALLBACK_WINDOW:
        if (LOWORD(cb))
            post_msg(LOWORD(cb), msg, h, p1);
        break;
    case CALLBACK_TASK: {
        int k = (int)LOWORD(cb) - 1;
        struct wtask *w;
        if (k < 0 || k >= NWT || !wt[k].used)
            break;
        w = &wt[k];
        if ((WORD)(w->qt + 1) % NQ == w->qh)
            break;
        TQ(w)[w->qt].hwnd = 0;
        TQ(w)[w->qt].message = msg;
        TQ(w)[w->qt].wParam = h;
        TQ(w)[w->qt].lParam = p1;
        TQ(w)[w->qt].time = 0;
        w->qt = (w->qt + 1) % NQ;
        break;
    }
    case CALLBACK_FUNCTION:
        if (cb && S->qn < NCB && t >= 0) {
            struct mmcb FAR *c = &S->q[S->qn++];
            c->t = (BYTE)t; c->fn = cb; c->h = h; c->msg = msg;
            c->inst = inst; c->p1 = p1; c->p2 = 0;
        }
        break;
    }
}
static WORD cbw[8];
void mm_cbcall(DWORD fn, WORD h, WORD msg, DWORD inst, DWORD p1, DWORD p2, int t)
{
    cbw[0] = h; cbw[1] = msg;
    cbw[2] = HIWORD(inst); cbw[3] = LOWORD(inst);
    cbw[4] = HIWORD(p1); cbw[5] = LOWORD(p1);
    cbw[6] = HIWORD(p2); cbw[7] = LOWORD(p2);
    call16((void FAR *)fn, wt[t].hinst, 8, cbw);
}
/* aus GetMessage (Zusammenhang des Programms t): Funktionsrueckrufe */
void mm_task(int t)
{
    static struct mmcb c;
    WORD i;
    if (!mm_sel)
        return;
    for (i = 0; i < S->qn; ) {
        if (S->q[i].t != t) {
            i++;
            continue;
        }
        fmemcpy(&c, &S->q[i], sizeof(c));
        fmemmove(&S->q[i], &S->q[i + 1], (S->qn - i - 1) * sizeof(c));
        S->qn--;
        mm_cbcall(c.fn, c.h, c.msg, c.inst, c.p1, c.p2, t);
    }
}
int mm_has_work(int t)
{
    WORD i;
    if (!mm_sel)
        return 0;
    for (i = 0; i < S->qn; i++)
        if (S->q[i].t == t)
            return 1;
    return 0;
}

/* ---- waveOut ---------------------------------------------------------- */
static void wo_done(LPWAVEHDR h)
{
    h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
    mm_cbfire(S->cb, S->woflags, S->wo_task, HWO, MM_WOM_DONE, S->inst, (DWORD)h);
}
/* fertige Puffer vorn aus der Schlange nehmen */
static void wo_retire(int all)
{
    WORD now = mm_qticks();
    while (S->head && (all || (S->head != S->wr && S->head != S->loopstart &&
           (short)(now - (WORD)S->head->reserved) >= 0))) {
        LPWAVEHDR h = S->head;
        S->head = h->lpNext;
        if (!S->head)
            S->tail = 0;
        if (S->wr == h)
            S->wr = S->head;
        wo_done(h);
    }
}
static void wo_pump(void)
{
    WORD n;
    while (S->wr && !S->paused) {
        LPWAVEHDR h = S->wr;
        DWORD left = h->dwBufferLength - S->wpos;
        WORD off = OFFOF(h->lpData) + (WORD)S->wpos;
        if (left > 0x8000UL)
            left = 0x8000UL;
        if ((DWORD)off + left > 0x10000UL)
            left = 0x10000UL - off;
        if (left) {
            n = st_write(SELOF(h->lpData), off, (WORD)left);
            if (!n)
                return;                 /* Vorrat voll */
            S->wpos += n;
            if (S->wpos < h->dwBufferLength)
                continue;
        }
        /* Puffer ganz geschrieben: Schleife? */
        if (h->dwFlags & WHDR_BEGINLOOP) {
            S->loopstart = h;
            if (!S->loops)
                S->loops = h->dwLoops;
        }
        if ((h->dwFlags & WHDR_ENDLOOP) && S->loopstart && S->loops > 1) {
            S->loops--;
            S->wr = S->loopstart;
            S->wpos = 0;
            continue;
        }
        if (h->dwFlags & WHDR_ENDLOOP) {
            S->loopstart = 0;
            S->loops = 0;
        }
        h->reserved = (WORD)(mm_qticks() + st_lag());
        S->wr = h->lpNext;
        S->wpos = 0;
    }
}

WORD API waveOutGetNumDevs(void) { return (mm_caps() & AUDCAP_PCM) ? 1 : 0; }

WORD API waveOutGetDevCaps(WORD dev, void FAR *p, WORD sz)
{
    static BYTE c[52];
    if (dev != 0 && dev != 0xFFFF)
        return MMSYSERR_BADDEVICEID;
    if (!waveOutGetNumDevs())
        return MMSYSERR_NODRIVER;
    fmemset(c, 0, sizeof(c));
    c[0] = 2; c[2] = 1;                         /* wMid, wPid (Creative)   */
    c[4] = 0x0A; c[5] = 3;                      /* Version 3.10            */
    fstrncpy((char FAR *)&c[6], "AT-DOS Wave (FAPI)", 32);
    *(DWORD *)&c[38] = 0x0FFFUL;                /* 11/22/44 kHz, 8/16, mono/stereo */
    *(WORD *)&c[42] = 2;
    *(DWORD *)&c[44] = 0x0004UL;                /* WAVECAPS_VOLUME         */
    fmemcpy(p, c, sz < sizeof(c) ? sz : sizeof(c));
    return 0;
}

WORD API waveOutOpen(WORD FAR *ph, WORD dev, const PCMWAVEFORMAT FAR *f,
                     DWORD cb, DWORD inst, DWORD flags)
{
    WORD r;
    if (!mm_init())
        return MMSYSERR_NOMEM;
    if (dev != 0 && dev != 0xFFFF)
        return MMSYSERR_BADDEVICEID;
    if (!waveOutGetNumDevs())
        return MMSYSERR_NODRIVER;
    if (fmt_ok(f))
        return WAVERR_BADFORMAT;
    if (flags & WAVE_FORMAT_QUERY)
        return 0;
    if (S->wo_open || S->owner == 3)
        return MMSYSERR_ALLOCATED;
    if (S->owner == 2) {                        /* sndPlaySound weicht     */
        S->snd = 0;
        if (S->fh) { DosClose(S->fh); S->fh = 0; }
        st_close();
    }
    if ((r = st_open(f, 1)) != 0)
        return r;
    S->wo_open = 1;
    S->paused = 0;
    S->wo_task = cur_task;
    S->cb = (flags & CALLBACK_TYPEMASK) ? cb : 0;
    S->inst = inst;
    S->woflags = flags;
    S->head = S->tail = S->wr = S->loopstart = 0;
    S->wpos = S->loops = 0;
    if (ph)
        *ph = HWO;
    if ((flags & CALLBACK_TYPEMASK) == CALLBACK_FUNCTION) {
        if (cb && cur_task >= 0)
            mm_cbcall(cb, HWO, MM_WOM_OPEN, inst, 0, 0, cur_task);
    } else
        mm_cbfire(cb, flags, cur_task, HWO, MM_WOM_OPEN, inst, 0);
    return 0;
}

static int wo_ok(WORD h) { return mm_sel && S->wo_open && h == HWO; }

WORD API waveOutClose(WORD h)
{
    DWORD fl;
    if (!wo_ok(h))
        return MMSYSERR_INVALHANDLE;
    if (S->head)
        return WAVERR_STILLPLAYING;
    fl = S->woflags;
    st_close();
    S->wo_open = 0;
    if ((fl & CALLBACK_TYPEMASK) == CALLBACK_FUNCTION) {
        if (S->cb && cur_task >= 0)
            mm_cbcall(S->cb, HWO, MM_WOM_CLOSE, S->inst, 0, 0, cur_task);
    } else
        mm_cbfire(S->cb, fl, cur_task, HWO, MM_WOM_CLOSE, S->inst, 0);
    return 0;
}
WORD API waveOutPrepareHeader(WORD h, LPWAVEHDR w, WORD sz)
{
    (void)sz;
    if (!wo_ok(h))
        return MMSYSERR_INVALHANDLE;
    w->dwFlags |= WHDR_PREPARED;
    return 0;
}
WORD API waveOutUnprepareHeader(WORD h, LPWAVEHDR w, WORD sz)
{
    (void)sz;
    if (!wo_ok(h))
        return MMSYSERR_INVALHANDLE;
    if (w->dwFlags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;
    w->dwFlags &= ~WHDR_PREPARED;
    return 0;
}
WORD API waveOutWrite(WORD h, LPWAVEHDR w, WORD sz)
{
    (void)sz;
    if (!wo_ok(h))
        return MMSYSERR_INVALHANDLE;
    if (!(w->dwFlags & WHDR_PREPARED))
        return WAVERR_UNPREPARED;
    w->dwFlags = (w->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
    w->lpNext = 0;
    w->reserved = 0;
    if (S->tail)
        S->tail->lpNext = w;
    else
        S->head = w;
    S->tail = w;
    if (!S->wr) {
        S->wr = w;
        S->wpos = 0;
    }
    st_clock();
    wo_pump();
    return 0;
}
WORD API waveOutPause(WORD h)   { if (!wo_ok(h)) return MMSYSERR_INVALHANDLE; S->paused = 1; return 0; }
WORD API waveOutRestart(WORD h) { if (!wo_ok(h)) return MMSYSERR_INVALHANDLE; S->paused = 0; return 0; }
WORD API waveOutReset(WORD h)
{
    static PCMWAVEFORMAT f;
    if (!wo_ok(h))
        return MMSYSERR_INVALHANDLE;
    /* Strom neu oeffnen: der Vorrat in der Karte verstummt */
    f.wFormatTag = 1; f.nChannels = S->ch; f.nSamplesPerSec = S->rate;
    f.wBitsPerSample = S->bits;
    fapi_call(FAPI_AUDIO, AUD_CLOSE, 0, 0);
    S->owner = 0;
    st_open(&f, 1);
    S->wr = 0;
    S->loopstart = 0;
    S->loops = 0;
    wo_retire(1);
    S->paused = 0;
    return 0;
}
WORD API waveOutBreakLoop(WORD h)
{
    if (!wo_ok(h))
        return MMSYSERR_INVALHANDLE;
    S->loops = 1;
    return 0;
}
WORD API waveOutGetPosition(WORD h, void FAR *mt, WORD sz)
{
    WORD FAR *t = mt;
    DWORD b;
    (void)sz;
    if (!wo_ok(h))
        return MMSYSERR_INVALHANDLE;
    st_clock();
    b = S->written - S->backlog;
    if (t[0] == 1) {                            /* TIME_MS */
        *(DWORD FAR *)(t + 1) = S->bps ? b * 10 / (S->bps / 100) : 0;
    } else if (t[0] == 2) {                     /* TIME_SAMPLES */
        *(DWORD FAR *)(t + 1) = b / S->align;
    } else {
        t[0] = 4;                               /* TIME_BYTES */
        *(DWORD FAR *)(t + 1) = b;
    }
    return 0;
}
WORD API waveOutGetVolume(WORD dev, DWORD FAR *v)
{
    WORD x = fapi_call(FAPI_AUDIO, AUD_MIXER, AUDMIX_WAVE, 0xFFFF);
    (void)dev;
    if (FAPI_FAILED(x))
        x = 100;
    x = (WORD)((DWORD)x * 0xFFFF / 100);
    *v = MAKELONG(x, x);
    return 0;
}
WORD API waveOutSetVolume(WORD dev, DWORD v)
{
    WORD l = LOWORD(v), r = HIWORD(v);
    (void)dev;
    fapi_call(FAPI_AUDIO, AUD_MIXER, AUDMIX_WAVE, (WORD)(((DWORD)(l > r ? l : r) * 100) / 0xFFFF));
    return 0;
}
WORD API waveOutGetID(WORD h, WORD FAR *id)
{
    if (!wo_ok(h))
        return MMSYSERR_INVALHANDLE;
    *id = 0;
    return 0;
}
WORD API waveOutGetPitch(WORD h, DWORD FAR *p) { (void)h; (void)p; return MMSYSERR_NOTSUPPORTED; }
WORD API waveOutSetPitch(WORD h, DWORD p) { (void)h; (void)p; return MMSYSERR_NOTSUPPORTED; }
WORD API waveOutGetPlaybackRate(WORD h, DWORD FAR *p) { (void)h; (void)p; return MMSYSERR_NOTSUPPORTED; }
WORD API waveOutSetPlaybackRate(WORD h, DWORD p) { (void)h; (void)p; return MMSYSERR_NOTSUPPORTED; }
DWORD API waveOutMessage(WORD h, WORD m, DWORD a, DWORD b) { (void)h; (void)m; (void)a; (void)b; return MMSYSERR_NOTSUPPORTED; }

static const char *const mmerr[] = {
    "Kein Fehler.", "Unbekannter Fehler.", "Ung\xFCltige Ger\xE4tekennung.",
    "Treiber nicht aktiviert.", "Ger\xE4t ist bereits belegt.",
    "Ung\xFCltige Zugriffsnummer.", "Kein Treiber installiert.",
    "Nicht gen\xFCgend Arbeitsspeicher.", "Funktion wird nicht unterst\xFCtzt.",
    "Fehlernummer au\xDF" "erhalb des Bereichs.", "Ung\xFCltiges Flag.",
    "Ung\xFCltiger Parameter."
};
WORD mm_errtext(WORD e, LPSTR buf, WORD n)
{
    const char *s;
    if (!buf || !n)
        return MMSYSERR_INVALPARAM;
    if (e < sizeof(mmerr) / sizeof(mmerr[0]))
        s = mmerr[e];
    else if (e == WAVERR_BADFORMAT)
        s = "Das Format wird nicht unterst\xFCtzt.";
    else if (e == WAVERR_STILLPLAYING)
        s = "Es wird noch abgespielt.";
    else if (e == WAVERR_UNPREPARED)
        s = "Der Puffer ist nicht vorbereitet.";
    else {
        buf[0] = 0;
        return 9;                               /* MMSYSERR_BADERRNUM */
    }
    fstrncpy(buf, s, n);
    return 0;
}
WORD API waveOutGetErrorText(WORD e, LPSTR buf, WORD n) { return mm_errtext(e, buf, n); }
WORD API waveInGetErrorText(WORD e, LPSTR buf, WORD n) { return mm_errtext(e, buf, n); }
WORD API waveInGetNumDevs(void) { return 0; }
WORD API waveInGetDevCaps(WORD d, void FAR *p, WORD n) { (void)d; (void)p; (void)n; return MMSYSERR_NODRIVER; }
WORD API waveInOpen(WORD FAR *ph, WORD d, const void FAR *f, DWORD cb, DWORD i, DWORD fl)
{ (void)ph; (void)d; (void)f; (void)cb; (void)i; (void)fl; return MMSYSERR_NODRIVER; }
/* Aufnahme: der Kernel hat keinen Eingabeweg - nie ein offenes Geraet */
WORD API waveInClose(WORD h) { (void)h; return MMSYSERR_INVALHANDLE; }
WORD API waveInPrepareHeader(WORD h, LPWAVEHDR w, WORD s) { (void)h; (void)w; (void)s; return MMSYSERR_INVALHANDLE; }
WORD API waveInUnprepareHeader(WORD h, LPWAVEHDR w, WORD s) { (void)h; (void)w; (void)s; return MMSYSERR_INVALHANDLE; }
WORD API waveInAddBuffer(WORD h, LPWAVEHDR w, WORD s) { (void)h; (void)w; (void)s; return MMSYSERR_INVALHANDLE; }
WORD API waveInStart(WORD h) { (void)h; return MMSYSERR_INVALHANDLE; }
WORD API waveInStop(WORD h) { (void)h; return MMSYSERR_INVALHANDLE; }
WORD API waveInReset(WORD h) { (void)h; return MMSYSERR_INVALHANDLE; }
WORD API waveInGetPosition(WORD h, void FAR *t, WORD s) { (void)h; (void)t; (void)s; return MMSYSERR_INVALHANDLE; }
WORD API waveInGetID(WORD h, WORD FAR *id) { (void)h; (void)id; return MMSYSERR_INVALHANDLE; }
DWORD API waveInMessage(WORD h, WORD m, DWORD a, DWORD b) { (void)h; (void)m; (void)a; (void)b; return MMSYSERR_NOTSUPPORTED; }

/* ---- sndPlaySound ----------------------------------------------------- */
static void snd_stop(void)
{
    if (!mm_sel)
        return;
    if (S->owner == 2)
        st_close();
    if (S->fh) {
        DosClose(S->fh);
        S->fh = 0;
    }
    S->snd = 0;
}
/* RIFF/WAVE im Puffer bzw. ueber die Datei: fmt und data finden */
static int snd_parse(WORD fh, int mem, PCMWAVEFORMAT FAR *f, DWORD FAR *ds, DWORD FAR *dl)
{
    static BYTE hd[8];
    DWORD p = 12, sz, end;
    const BYTE FAR *m = S->mp;
    if (mem) {
        if (*(DWORD FAR *)m != 0x46464952UL || *(DWORD FAR *)(m + 8) != 0x45564157UL)
            return 0;
        end = *(DWORD FAR *)(m + 4) + 8;
    } else {
        if (fapi_sel(FAPI_READ, mm_sel, fh, OFFOF(S->buf), 12) != 12)
            return 0;
        if (*(DWORD FAR *)S->buf != 0x46464952UL || *(DWORD FAR *)(S->buf + 8) != 0x45564157UL)
            return 0;
        end = *(DWORD FAR *)(S->buf + 4) + 8;
    }
    fmemset(f, 0, sizeof(*f));
    while (p + 8 <= end) {
        if (mem)
            fmemcpy(hd, m + (WORD)p, 8);
        else {
            S->seekpos = p;
            fapi_sel(FAPI_SEEK, mm_sel, fh, OFFOF(&S->seekpos), 0);
            if (fapi_sel(FAPI_READ, mm_sel, fh, OFFOF(S->buf), 8) != 8)
                return 0;
            fmemcpy(hd, S->buf, 8);
        }
        sz = *(DWORD *)(hd + 4);
        if (*(DWORD *)hd == 0x20746D66UL) {     /* "fmt " */
            if (mem)
                fmemcpy(f, m + (WORD)p + 8, sz < sizeof(*f) ? (WORD)sz : sizeof(*f));
            else
                fapi_sel(FAPI_READ, mm_sel, fh, OFFOF(S->buf),
                         sz < sizeof(*f) ? (WORD)sz : sizeof(*f)),
                fmemcpy(f, S->buf, sz < sizeof(*f) ? (WORD)sz : sizeof(*f));
        } else if (*(DWORD *)hd == 0x61746164UL) {  /* "data" */
            *ds = p + 8;
            *dl = sz;
            if (*ds + sz > end)
                *dl = end > *ds ? end - *ds : 0;
            return f->wFormatTag != 0;
        }
        p += 8 + sz + (sz & 1);
    }
    return 0;
}
static void snd_rewind(void)
{
    S->dleft = S->dlen;
    S->bpos = S->blen = 0;
    if (!S->smem) {
        S->seekpos = S->dstart;
        fapi_sel(FAPI_SEEK, mm_sel, S->fh, OFFOF(&S->seekpos), 0);
    }
}
/* Daten weiterreichen; -> 0 fertig */
static int snd_pump(WORD fh, int mem, int loop)
{
    for (;;) {
        WORD n, w;
        if (S->bpos == S->blen) {
            if (!S->dleft) {
                if (!loop)
                    return 0;
                snd_rewind();
                continue;
            }
            n = S->dleft > SBUF ? SBUF : (WORD)S->dleft;
            if (mem) {
                DWORD o = S->dstart + (S->dlen - S->dleft);
                if (o + n > 0x10000UL)          /* nur ein Segment         */
                    return 0;
                fmemcpy(S->buf, S->mp + (WORD)o, n);
            } else {
                n = fapi_sel(FAPI_READ, mm_sel, fh, OFFOF(S->buf), n);
                if (FAPI_FAILED(n) || !n)
                    return 0;
            }
            S->dleft -= n;
            S->bpos = 0;
            S->blen = n;
        }
        w = st_write(mm_sel, OFFOF(S->buf) + S->bpos, S->blen - S->bpos);
        if (!w)
            return 1;
        S->bpos += w;
    }
}

/* Name -> Datei: WIN.INI [sounds] (Eintrag vor dem Komma), sonst Pfad */
static int snd_find(LPCSTR name, WORD fl)
{
    WORD i;
    GetProfileString("sounds", name, "", S->prof, sizeof(S->prof));
    for (i = 0; S->prof[i] && S->prof[i] != ','; i++)
        ;
    S->prof[i] = 0;
    if (S->prof[0] && fstricmp(S->prof, "none") && path_find(S->prof, S->path, sizeof(S->path)))
        return 1;
    if (path_find(name, S->path, sizeof(S->path)))
        return 1;
    if (fl & SND_NODEFAULT)
        return 0;
    GetProfileString("sounds", "SystemDefault", "", S->prof, sizeof(S->prof));
    for (i = 0; S->prof[i] && S->prof[i] != ','; i++)
        ;
    S->prof[i] = 0;
    return S->prof[0] && path_find(S->prof, S->path, sizeof(S->path));
}

BOOL API sndPlaySound(LPCSTR name, WORD fl)
{
    static PCMWAVEFORMAT f;
    if (!mm_init())
        return 0;
    if (!name) {
        snd_stop();
        return 1;
    }
    if (S->wo_open || S->owner == 3)    /* Geraet gehoert waveOut/MCI    */
        return 0;
    if (S->snd && (fl & SND_NOSTOP))
        return 0;
    snd_stop();
    S->smem = (fl & SND_MEMORY) != 0;
    S->sloop = (fl & SND_LOOP) && (fl & SND_ASYNC);
    if (S->smem)
        S->mp = (const BYTE FAR *)name;
    else {
        WORD h;
        if (!snd_find(name, fl))
            return 0;
        h = fapi_sel(FAPI_OPEN, mm_sel, OFFOF(S->path), 0, 0);
        if (FAPI_FAILED(h))
            return 0;
        S->fh = h;
    }
    if (!snd_parse(S->fh, S->smem, &f, &S->dstart, &S->dlen) || st_open(&f, 2)) {
        snd_stop();
        return 0;
    }
    S->snd = 1;
    snd_rewind();
    if (fl & SND_ASYNC)
        return 1;
    /* synchron: hier abspielen, dann warten, bis es verklungen ist */
    while (snd_pump(S->fh, S->smem, S->sloop)) {
        DosSleep(10);
        st_clock();
    }
    while (S->backlog) {
        DosSleep(10);
        st_clock();
    }
    snd_stop();
    return 1;
}


/* ---- MCI waveaudio: eine geladene Datei ------------------------------- */
WORD mw_load(LPCSTR path)
{
    WORD h;
    if (!mm_init())
        return MCIERR_OUT_OF_MEMORY;
    mw_unload();
    fstrncpy(S->prof, path, sizeof(S->prof));
    h = fapi_sel(FAPI_OPEN, mm_sel, OFFOF(S->prof), 0, 0);
    if (FAPI_FAILED(h))
        return MCIERR_FILE_NOT_FOUND;
    if (!snd_parse(h, 0, &S->mw_f, &S->mw_dstart, &S->mw_dlen) || fmt_ok(&S->mw_f)) {
        DosClose(h);
        return MCIERR_INVALID_FILE;
    }
    S->mw_fh = h;
    S->mw_pos = 0;
    S->mw_done = 0;
    return 0;
}
void mw_unload(void)
{
    if (!mm_sel)
        return;
    mw_halt();
    if (S->mw_fh)
        DosClose(S->mw_fh);
    S->mw_fh = 0;
    S->mw_dlen = 0;
}
int mw_playing(void) { return mm_sel && S->owner == 3; }
/* from/to in Byte der Daten */
WORD mw_play(DWORD from, DWORD to)
{
    if (!mm_sel || !S->mw_fh)
        return MCIERR_FILE_NOT_FOUND;
    if (S->wo_open)
        return MCIERR_WAVE_OUTPUTSINUSE;
    mw_halt();
    if (S->owner == 2)
        snd_stop();
    if (to > S->mw_dlen)
        to = S->mw_dlen;
    from -= from % S->mw_f.nBlockAlign;
    if (from > to)
        from = to;
    if (st_open(&S->mw_f, 3))
        return MCIERR_WAVE_OUTPUTSINUSE;
    S->mw_base = from;
    S->mw_to = to;
    S->mw_done = 0;
    S->dleft = to - from;
    S->bpos = S->blen = 0;
    S->seekpos = S->mw_dstart + from;
    fapi_sel(FAPI_SEEK, mm_sel, S->mw_fh, OFFOF(&S->seekpos), 0);
    snd_pump(S->mw_fh, 0, 0);
    return 0;
}
DWORD mw_where(void)
{
    DWORD p;
    if (!mm_sel)
        return 0;
    if (S->owner != 3)
        return S->mw_pos;
    st_clock();
    p = S->mw_base + (S->written - S->backlog);
    return p > S->mw_to ? S->mw_to : p;
}
void mw_halt(void)
{
    if (!mm_sel || S->owner != 3)
        return;
    S->mw_pos = mw_where();
    st_close();
}
void mw_seek(DWORD b)
{
    if (!mm_sel)
        return;
    mw_halt();
    S->mw_pos = b > S->mw_dlen ? S->mw_dlen : b - b % S->mw_f.nBlockAlign;
}
void mm_cbopen(DWORD cb, DWORD flags, WORD h, WORD msg, DWORD inst)
{
    if ((flags & CALLBACK_TYPEMASK) == CALLBACK_FUNCTION) {
        if (cb && cur_task >= 0)
            mm_cbcall(cb, h, msg, inst, 0, 0, cur_task);
    } else
        mm_cbfire(cb, flags, cur_task, h, msg, inst, 0);
}

/* Hauptschleife */
void mm_pump(void)
{
    if (!mm_sel)
        return;
    sq_pump();
    mci_poll();
    if (!S->owner)
        return;
    st_clock();
    if (S->owner == 1) {
        wo_pump();
        wo_retire(0);
    } else if (S->owner == 2 && S->snd) {
        if (!snd_pump(S->fh, S->smem, S->sloop) && !S->backlog)
            snd_stop();
    } else if (S->owner == 3) {
        if (!snd_pump(S->mw_fh, 0, 0) && !S->backlog) {
            S->mw_pos = S->mw_to;
            st_close();
            S->mw_done = 1;
        }
    }
}
/* Task endet: was ihm gehoert, schliessen */
void mm_task_end(int t)
{
    WORD i;
    if (!mm_sel)
        return;
    if (S->wo_open && S->wo_task == t) {
        S->head = S->tail = S->wr = 0;
        st_close();
        S->wo_open = 0;
    }
    md_task_end(t);
    mci_task_end(t);
    mmio_task_end(t);
    for (i = 0; i < S->qn; )
        if (S->q[i].t == t) {
            fmemmove(&S->q[i], &S->q[i + 1], (S->qn - i - 1) * sizeof(struct mmcb));
            S->qn--;
        } else
            i++;
}

/* MessageBeep: Klang aus WIN.INI, sonst Piepser */
void mm_beep(WORD type)
{
    const char *k = "SystemDefault";
    if (type == 0x10) k = "SystemHand";
    else if (type == 0x20) k = "SystemQuestion";
    else if (type == 0x30) k = "SystemExclamation";
    else if (type == 0x40) k = "SystemAsterisk";
    if (type != 0xFFFF && waveOutGetNumDevs() && mm_init() && !S->wo_open &&
        sndPlaySound(k, SND_ASYNC | SND_NODEFAULT))
        return;
    fapi_call(FAPI_BEEP, 880, 80, 0);
}

/* ---- Zeit, Version, Kleinkram ----------------------------------------- */
WORD  API mmsystemGetVersion(void) { return 0x030A; }
DWORD API timeGetTime(void) { return (DWORD)mm_qticks() * 10; }
WORD API timeGetSystemTime(WORD FAR *mt, WORD sz)
{
    (void)sz;
    mt[0] = 1;
    *(DWORD FAR *)(mt + 1) = timeGetTime();
    return 0;
}
WORD API timeGetDevCaps(WORD FAR *tc, WORD sz)
{
    (void)sz;
    tc[0] = 10;                                 /* wPeriodMin (ms) */
    tc[1] = 65535;
    return 0;
}
WORD API timeBeginPeriod(WORD p) { (void)p; return 0; }
WORD API timeEndPeriod(WORD p) { (void)p; return 0; }
void API OutputDebugStr(LPCSTR s) { glogs("OutputDebugStr: ", s); }
WORD API auxGetNumDevs(void) { return 0; }
WORD API joyGetNumDevs(void) { return 0; }
WORD API midiInGetNumDevs(void) { return 0; }
