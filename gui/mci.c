/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - MMSYSTEM: Media Control Interface (MCI) wie Windows 3.1.
 *
 * Zwei Geraetearten, beide in der GUI selbst statt als MCI-Treiber:
 * waveaudio (WAV-Dateien ueber die Wiedergabe in mmsystem.c) und
 * sequencer (MIDI-Dateien ueber den Sequenzer in mmmidi.c). Je Art ist
 * eine Datei zur Zeit offen. mciSendCommand mit den Befehlen, die
 * Medienwiedergabe und eigene Programme brauchen (OPEN, CLOSE, PLAY, STOP,
 * PAUSE, RESUME, SEEK, STATUS, SET, GETDEVCAPS, INFO, SYSINFO, SOUND),
 * mciSendString mit derselben Befehlssprache als Text, Benachrichtigung
 * ueber MM_MCINOTIFY, MCI_WAIT. Aufnahme, Speichern und CD-Audio fehlen.
 *
 * Texte und Schluesselwoerter stehen in fernen Tabellen (DGROUP knapp).
 */
#include "mm.h"

#define MCI_OPEN        0x0803
#define MCI_CLOSE       0x0804
#define MCI_PLAY        0x0806
#define MCI_SEEK        0x0807
#define MCI_STOP        0x0808
#define MCI_PAUSE       0x0809
#define MCI_INFO        0x080A
#define MCI_GETDEVCAPS  0x080B
#define MCI_SET         0x080D
#define MCI_STATUS      0x0814
#define MCI_SYSINFO     0x0810
#define MCI_BREAK       0x0811
#define MCI_SOUND       0x0812
#define MCI_RESUME      0x0855

#define MCI_NOTIFY      0x00000001UL
#define MCI_WAIT        0x00000002UL
#define MCI_FROM        0x00000004UL
#define MCI_TO          0x00000008UL
#define MCI_TRACK       0x00000010UL
#define MCI_OPEN_SHAREABLE 0x00000100UL
#define MCI_OPEN_ELEMENT   0x00000200UL
#define MCI_OPEN_ALIAS     0x00000400UL
#define MCI_OPEN_ELEMENT_ID 0x00000800UL
#define MCI_OPEN_TYPE_ID   0x00001000UL
#define MCI_OPEN_TYPE      0x00002000UL
#define MCI_SEEK_TO_START  0x00000100UL
#define MCI_SEEK_TO_END    0x00000200UL
#define MCI_STATUS_ITEM    0x00000100UL
#define MCI_STATUS_START   0x00000200UL
#define MCI_GETDEVCAPS_ITEM 0x00000100UL
#define MCI_INFO_PRODUCT   0x00000100UL
#define MCI_INFO_FILE      0x00000200UL
#define MCI_SYSINFO_QUANTITY 0x00000100UL
#define MCI_SYSINFO_OPEN     0x00000200UL
#define MCI_SYSINFO_NAME     0x00000400UL
#define MCI_SYSINFO_INSTALLNAME 0x00000800UL
#define MCI_SET_TIME_FORMAT  0x00000400UL
#define MCI_SOUND_NAME       0x00000100UL

#define MCI_ALL_DEVICE_ID  0xFFFF
#define MCI_NOTIFY_SUCCESSFUL 1
#define MCI_NOTIFY_SUPERSEDED 2
#define MCI_NOTIFY_ABORTED    4
#define MCI_NOTIFY_FAILURE    8

#define MCI_MODE_NOT_READY 524
#define MCI_MODE_STOP      525
#define MCI_MODE_PLAY      526
#define MCI_MODE_PAUSE     529
#define MCI_MODE_OPEN      530
#define MCI_DEVTYPE_WAVEFORM_AUDIO 522
#define MCI_DEVTYPE_SEQUENCER      523
#define MCI_FORMAT_MILLISECONDS 0
#define MCI_FORMAT_BYTES        8
#define MCI_FORMAT_SAMPLES      9

#define NMCI    4
#define T_WAVE  1
#define T_SEQ   2
struct mdev {
    BYTE  used, type, mode, autoclose;  /* mode: 0 Halt, 1 Spielt, 2 Pause */
    BYTE  loaded;
    int   task;
    WORD  tfmt;
    WORD  nwnd;                         /* Fenster fuer Benachrichtigung  */
    BYTE  pend;                         /* PLAY wartet auf Benachrichtigung */
    DWORD to;                           /* Ende (intern)                 */
    char  alias[32];
    char  elem[128];
};
struct mcist {
    struct mdev d[NMCI];
    char  cmd[256];                     /* mciSendString: Arbeitskopie   */
    char  tok[16][128];
    WORD  ntok;
    char  out[128];
    char  tmp[160];
    DWORD parms[8];
};
static WORD mci_sel;
#define C ((struct mcist FAR *)MKFP(mci_sel, 0))
#define DEV(id) (&C->d[(id) - 1])

short API GetProfileString(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR buf, short n);
BOOL  API sndPlaySound(LPCSTR name, WORD fl);

/* ---- ferne Texte ---------------------------------------------------------- */
static const char __far s_types[] = "waveaudio\0sequencer\0";
static const char __far s_inst[] = "WaveAudio\0Sequencer\0";
static const char __far s_prod[] = "Klang (Wave)\0MIDI-Sequenzer\0";
static const char __far s_cmds[] =
    "open\0close\0play\0stop\0pause\0resume\0seek\0status\0set\0capability\0"
    "info\0sysinfo\0break\0sound\0";
static const WORD __far cmd_msg[] = {
    MCI_OPEN, MCI_CLOSE, MCI_PLAY, MCI_STOP, MCI_PAUSE, MCI_RESUME, MCI_SEEK,
    MCI_STATUS, MCI_SET, MCI_GETDEVCAPS, MCI_INFO, MCI_SYSINFO, MCI_BREAK, MCI_SOUND
};
/* Status-Woerter (auch zweiteilig) -> Nummer */
static const char __far s_items[] =
    "length\0position\0number of tracks\0mode\0media present\0time format\0"
    "ready\0current track\0start position\0";
static const char __far s_caps[] =
    "can record\0has audio\0has video\0device type\0uses files\0compound device\0"
    "can eject\0can play\0can save\0";
static const char __far s_modes[] = "not ready\0stopped\0playing\0recording\0seeking\0paused\0open\0";
static const char __far s_fmts[] = "milliseconds\0ms\0bytes\0samples\0";
static const char __far s_bool[] = "false\0true\0";
static const char __far s_kw[] = "notify\0wait\0quantity\0open\0name\0installname\0type\0alias\0shareable\0from\0to\0start\0end\0time\0format\0audio\0file\0product\0all\0";
enum { K_NOTIFY, K_WAIT, K_QUANTITY, K_OPEN, K_NAME, K_INSTALLNAME, K_TYPE, K_ALIAS, K_SHAREABLE, K_FROM, K_TO, K_START, K_END, K_TIME, K_FORMAT, K_AUDIO, K_FILE, K_PRODUCT, K_ALL };
static const char __far s_mciext[] = "mci extensions";
static const char __far s_wav[] = "wav";
static const char __far s_mid[] = "mid";
static const char __far s_rmi[] = "rmi";
static const char __far s_mci[] = "MCI";
static const char __far s_sysdef[] = "SystemDefault";
static const char __far s_empty[] = "";


struct etext { WORD e; const char __far *t; };
static const char __far e00[] = "Ung\xFCltige Ger\xE4tekennung.";
static const char __far e01[] = "Unbekanntes Schl\xFCsselwort.";
static const char __far e02[] = "Unbekannter Befehl.";
static const char __far e03[] = "Fehler im Ger\xE4t oder Treiber.";
static const char __far e04[] = "Ung\xFCltiger Ger\xE4tename.";
static const char __far e05[] = "Nicht gen\xFCgend Arbeitsspeicher.";
static const char __far e06[] = "Das Ger\xE4t ist bereits ge\xF6" "ffnet.";
static const char __far e07[] = "Es fehlt ein Wert.";
static const char __far e08[] = "Die Funktion wird nicht unterst\xFCtzt.";
static const char __far e09[] = "Die Datei wurde nicht gefunden.";
static const char __far e10[] = "Ung\xFCltige oder besch\xE4" "digte Datei.";
static const char __far e11[] = "Die Dateierweiterung ist keinem Ger\xE4t zugeordnet.";
static const char __far e12[] = "Ger\xE4tetyp angeben.";
static const char __far e13[] = "Der Wert liegt au\xDF" "erhalb des Bereichs.";
static const char __far e14[] = "Ung\xFCltiges Zeitformat.";
static const char __far e15[] = "Der Ger\xE4tename fehlt.";
static const char __far e16[] = "Der Wave-Ausgang wird schon benutzt.";
static const char __far e17[] = "Der MIDI-Ausgang wird schon benutzt.";
static const char __far e18[] = "Kein MIDI-Ausgang vorhanden.";
static const char __far e19[] = "Der Alias wird schon benutzt.";
static const char __far e20[] = "Eine Zahl wurde erwartet.";
static const char __far e21[] = "Das Ger\xE4t ist nicht installiert.";
static const char __far e22[] = "Unbekannter MCI-Fehler.";
static const struct etext __far etab[] = {
    { MCIERR_INVALID_DEVICE_ID, e00 }, { MCIERR_UNRECOGNIZED_KEYWORD, e01 },
    { MCIERR_UNRECOGNIZED_COMMAND, e02 }, { MCIERR_HARDWARE, e03 },
    { MCIERR_INVALID_DEVICE_NAME, e04 }, { MCIERR_OUT_OF_MEMORY, e05 },
    { MCIERR_DEVICE_OPEN, e06 }, { MCIERR_MISSING_PARAMETER, e07 },
    { MCIERR_UNSUPPORTED_FUNCTION, e08 }, { MCIERR_FILE_NOT_FOUND, e09 },
    { MCIERR_INVALID_FILE, e10 }, { MCIERR_EXTENSION_NOT_FOUND, e11 },
    { MCIERR_DEVICE_TYPE_REQUIRED, e12 }, { MCIERR_OUTOFRANGE, e13 },
    { MCIERR_BAD_TIME_FORMAT, e14 }, { MCIERR_MISSING_DEVICE_NAME, e15 },
    { MCIERR_WAVE_OUTPUTSINUSE, e16 }, { MCIERR_SEQ_PORT_INUSE, e17 },
    { MCIERR_SEQ_PORT_NONEXISTENT, e18 }, { MCIERR_DUPLICATE_ALIAS, e19 },
    { MCIERR_BAD_INTEGER, e20 }, { MCIERR_DEVICE_NOT_INSTALLED, e21 },
    { 0, e22 }
};

static int lc(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(const char FAR *a, const char FAR *b)
{
    while (*a && lc(*a) == lc(*b))
        a++, b++;
    return !*a && !*b;
}
/* Index von w in einer Liste "a\0b\0\0" -> -1 */
static int kwfind(const char __far *t, const char FAR *w)
{
    int i = 0;
    while (*t) {
        if (ieq(t, w))
            return i;
        while (*t)
            t++;
        t++;
        i++;
    }
    return -1;
}
static const char __far *kwget(const char __far *t, int i)
{
    while (i-- > 0) {
        while (*t)
            t++;
        t++;
    }
    return t;
}

static int mci_init(void)
{
    if (!mci_sel) {
        if (!mm_init())
            return 0;
        mci_sel = seg_new(sizeof(struct mcist));
        if (!mci_sel)
            return 0;
        fmemset(C, 0, sizeof(struct mcist));
    }
    return 1;
}
static void notify(WORD id, WORD wnd, WORD code)
{
    if (wnd)
        post_msg(wnd, MM_MCINOTIFY, code, id);
}

/* ---- Zeit: intern Wave = Byte der Daten, Sequenzer = ms ----------------- */
static DWORD bps(void) { return (DWORD)S->mw_f.nSamplesPerSec * S->mw_f.nBlockAlign; }
static DWORD to_int(struct mdev FAR *d, DWORD v)
{
    DWORD b = bps();
    if (d->type != T_WAVE)
        return v;
    if (d->tfmt == MCI_FORMAT_BYTES)
        return v;
    if (d->tfmt == MCI_FORMAT_SAMPLES)
        return v * S->mw_f.nBlockAlign;
    return (v / 1000) * b + (v % 1000) * b / 1000;
}
static DWORD from_int(struct mdev FAR *d, DWORD v)
{
    DWORD b = bps();
    if (d->type != T_WAVE)
        return v;
    if (d->tfmt == MCI_FORMAT_BYTES)
        return v;
    if (d->tfmt == MCI_FORMAT_SAMPLES)
        return S->mw_f.nBlockAlign ? v / S->mw_f.nBlockAlign : 0;
    return b ? (v / b) * 1000 + (v % b) * 1000 / b : 0;
}
static DWORD d_len(struct mdev FAR *d)
{
    if (!d->loaded)
        return 0;
    return d->type == T_WAVE ? S->mw_dlen : sq_length();
}
static DWORD d_pos(struct mdev FAR *d)
{
    if (!d->loaded)
        return 0;
    return d->type == T_WAVE ? mw_where() : sq_where();
}
static void d_halt(struct mdev FAR *d)
{
    if (d->type == T_WAVE)
        mw_halt();
    else
        sq_halt();
}
static void d_abort(WORD id, WORD code)
{
    struct mdev FAR *d = DEV(id);
    if (d->pend) {
        d->pend = 0;
        notify(id, d->nwnd, code);
    }
}
static void d_close(WORD id)
{
    struct mdev FAR *d = DEV(id);
    d_halt(d);
    d_abort(id, MCI_NOTIFY_ABORTED);
    if (d->loaded) {
        if (d->type == T_WAVE)
            mw_unload();
        else
            sq_unload();
    }
    d->used = 0;
}

/* Typ aus der Endung (WIN.INI [mci extensions], sonst wav/mid/rmi) */
static int type_of_file(LPCSTR f)
{
    const char FAR *e = 0, FAR *p;
    for (p = f; *p; p++)
        if (*p == '.')
            e = p + 1;
        else if (*p == '\\')
            e = 0;
    if (!e)
        return 0;
    fstrncpy(C->tmp, e, 8);
    GetProfileString(s_mciext, C->tmp, s_empty, C->out, sizeof(C->out));
    if (C->out[0])
        return kwfind(s_types, C->out) + 1;
    if (ieq(e, s_wav))
        return T_WAVE;
    if (ieq(e, s_mid) || ieq(e, s_rmi))
        return T_SEQ;
    return 0;
}

static DWORD m_open(DWORD fl, DWORD FAR *p)
{
    WORD k, type = 0;
    DWORD r;
    struct mdev FAR *d;
    LPCSTR ty = (LPCSTR)p[2], el = (LPCSTR)p[3], al = (LPCSTR)p[4];
    if (fl & MCI_OPEN_ELEMENT_ID)
        return MCIERR_UNSUPPORTED_FUNCTION;
    if (fl & MCI_OPEN_TYPE_ID) {
        WORD t = LOWORD((DWORD)ty);
        type = t == MCI_DEVTYPE_WAVEFORM_AUDIO ? T_WAVE : t == MCI_DEVTYPE_SEQUENCER ? T_SEQ : 0;
        if (!type)
            return MCIERR_DEVICE_NOT_INSTALLED;
    } else if (fl & MCI_OPEN_TYPE) {
        int i = ty ? kwfind(s_types, ty) : -1;
        if (i < 0) {
            /* ohne Typ-Namen: vielleicht eine Datei */
            if (ty && !(fl & MCI_OPEN_ELEMENT) && (type = type_of_file(ty)) != 0) {
                el = ty;
                fl |= MCI_OPEN_ELEMENT;
            } else
                return MCIERR_INVALID_DEVICE_NAME;
        } else
            type = i + 1;
    } else if ((fl & MCI_OPEN_ELEMENT) && el) {
        type = type_of_file(el);
        if (!type)
            return MCIERR_EXTENSION_NOT_FOUND;
    } else
        return MCIERR_MISSING_PARAMETER;
    if ((fl & MCI_OPEN_ALIAS) && al)
        for (k = 0; k < NMCI; k++)
            if (C->d[k].used && ieq(C->d[k].alias, al))
                return MCIERR_DUPLICATE_ALIAS;
    for (k = 0; k < NMCI; k++)                  /* je Art eine Datei     */
        if (C->d[k].used && C->d[k].type == type && C->d[k].loaded && (fl & MCI_OPEN_ELEMENT))
            return MCIERR_DEVICE_OPEN;
    for (k = 0; k < NMCI && C->d[k].used; k++)
        ;
    if (k == NMCI)
        return MCIERR_OUT_OF_MEMORY;
    d = &C->d[k];
    fmemset(d, 0, sizeof(*d));
    d->type = (BYTE)type;
    d->task = cur_task;
    if ((fl & MCI_OPEN_ELEMENT) && el && *el) {
        full_path(el, d->elem, sizeof(d->elem));
        r = type == T_WAVE ? mw_load(d->elem) : sq_load(d->elem);
        if (r)
            return r;
        d->loaded = 1;
    }
    if ((fl & MCI_OPEN_ALIAS) && al)
        fstrncpy(d->alias, al, sizeof(d->alias));
    else if ((fl & MCI_OPEN_ELEMENT) && el)
        fstrncpy(d->alias, el, sizeof(d->alias));
    else
        fstrncpy(d->alias, kwget(s_types, type - 1), sizeof(d->alias));
    d->used = 1;
    *((WORD FAR *)&p[1]) = k + 1;               /* wDeviceID            */
    return 0;
}

static DWORD m_play(WORD id, DWORD fl, DWORD FAR *p)
{
    struct mdev FAR *d = DEV(id);
    DWORD from, to, len = d_len(d);
    WORD r;
    if (!d->loaded)
        return MCIERR_FILENAME_REQUIRED;
    from = (fl & MCI_FROM) ? to_int(d, p[1]) : d_pos(d);
    to = (fl & MCI_TO) ? to_int(d, p[2]) : len;
    if (from > len || to > len)
        return MCIERR_OUTOFRANGE;
    if (from >= len && !(fl & MCI_FROM))
        from = 0;                               /* am Ende: von vorn     */
    d_abort(id, MCI_NOTIFY_SUPERSEDED);
    r = d->type == T_WAVE ? mw_play(from, to) : sq_play(from, to);
    if (r)
        return r;
    d->mode = 1;
    d->to = to;
    if (fl & MCI_NOTIFY) {
        d->pend = 1;
        d->nwnd = LOWORD(p[0]);
    }
    if (fl & MCI_WAIT) {
        while (d->mode == 1 && (d->type == T_WAVE ? mw_playing() : sq_playing())) {
            mm_pump();
            DosSleep(10);
        }
        mci_poll();
    }
    return 0;
}

static DWORD m_status(WORD id, DWORD fl, DWORD FAR *p)
{
    struct mdev FAR *d = DEV(id);
    DWORD v;
    if (!(fl & MCI_STATUS_ITEM))
        return MCIERR_MISSING_PARAMETER;
    switch ((WORD)p[2]) {
    case 1: v = from_int(d, d_len(d)); break;               /* LENGTH     */
    case 2: v = (fl & MCI_STATUS_START) ? 0 : from_int(d, d_pos(d)); break;
    case 3: v = 1; break;                                   /* Spuren     */
    case 4: v = !d->loaded ? (d->type == T_WAVE ? MCI_MODE_STOP : MCI_MODE_NOT_READY) :
                d->mode == 1 ? MCI_MODE_PLAY : d->mode == 2 ? MCI_MODE_PAUSE : MCI_MODE_STOP;
            break;
    case 5: v = d->loaded; break;                           /* Medium da  */
    case 6: v = d->tfmt; break;
    case 7: v = 1; break;                                   /* bereit     */
    case 8: v = 1; break;                                   /* Spur       */
    case 0x4001: v = d->type == T_WAVE ? S->mw_f.wFormatTag : 0; break;
    case 0x4002: v = d->type == T_WAVE ? S->mw_f.nChannels : 0; break;
    case 0x4003: v = d->type == T_WAVE ? S->mw_f.nSamplesPerSec : 0; break;
    case 0x4004: v = d->type == T_WAVE ? S->mw_f.nAvgBytesPerSec : 0; break;
    case 0x4005: v = d->type == T_WAVE ? S->mw_f.nBlockAlign : 0; break;
    case 0x4006: v = d->type == T_WAVE ? S->mw_f.wBitsPerSample : 0; break;
    default: v = 0; break;
    }
    p[1] = v;
    return 0;
}

static DWORD m_caps(WORD id, DWORD fl, DWORD FAR *p)
{
    struct mdev FAR *d = DEV(id);
    DWORD v = 0;
    if (!(fl & MCI_GETDEVCAPS_ITEM))
        return MCIERR_MISSING_PARAMETER;
    switch ((WORD)p[2]) {
    case 2: case 5: case 6: case 8: v = 1; break;
    case 4: v = d->type == T_WAVE ? MCI_DEVTYPE_WAVEFORM_AUDIO : MCI_DEVTYPE_SEQUENCER; break;
    case 0x4002: v = d->type == T_WAVE ? 1 : 0; break;      /* Ausgaenge  */
    }
    p[1] = v;
    return 0;
}

static DWORD m_info(WORD id, DWORD fl, DWORD FAR *p)
{
    struct mdev FAR *d = DEV(id);
    LPSTR r = (LPSTR)p[1];
    WORD n = (WORD)p[2];
    if (!r || !n)
        return MCIERR_PARAM_OVERFLOW;
    if (fl & MCI_INFO_FILE)
        fstrncpy(r, d->elem, n);
    else
        fstrncpy(r, kwget(s_prod, d->type - 1), n);
    return 0;
}

static DWORD m_sysinfo(WORD id, DWORD fl, DWORD FAR *p)
{
    LPSTR r = (LPSTR)p[1];
    WORD n = (WORD)p[2], k, cnt = 0, want = (WORD)p[3];
    WORD ty = (WORD)p[4];
    int t = ty == MCI_DEVTYPE_WAVEFORM_AUDIO ? T_WAVE : ty == MCI_DEVTYPE_SEQUENCER ? T_SEQ : 0;
    if (!r)
        return MCIERR_PARAM_OVERFLOW;
    /* Name in [mci] eines offenen Geraets */
    if ((fl & MCI_SYSINFO_INSTALLNAME) && id && id <= NMCI && DEV(id)->used) {
        fstrncpy(r, kwget(s_inst, DEV(id)->type - 1), n);
        return 0;
    }
    if (fl & MCI_SYSINFO_QUANTITY) {
        if (fl & MCI_SYSINFO_OPEN) {
            for (k = 0; k < NMCI; k++)
                if (C->d[k].used && (!t || C->d[k].type == t))
                    cnt++;
        } else
            cnt = t ? 1 : 2;
        *(DWORD FAR *)r = cnt;
        return 0;
    }
    if (fl & (MCI_SYSINFO_NAME | MCI_SYSINFO_INSTALLNAME)) {
        if (fl & MCI_SYSINFO_OPEN) {
            for (k = 0; k < NMCI; k++)
                if (C->d[k].used && (!t || C->d[k].type == t) && ++cnt == want) {
                    fstrncpy(r, C->d[k].alias, n);
                    return 0;
                }
            return MCIERR_OUTOFRANGE;
        }
        if (t && want <= 1) {
            fstrncpy(r, kwget((fl & MCI_SYSINFO_INSTALLNAME) ? s_inst : s_types, t - 1), n);
            return 0;
        }
        if (!t && (want == 1 || want == 2)) {
            fstrncpy(r, kwget((fl & MCI_SYSINFO_INSTALLNAME) ? s_inst : s_types, want - 1), n);
            return 0;
        }
        return MCIERR_OUTOFRANGE;
    }
    return MCIERR_MISSING_PARAMETER;
}

static DWORD mci_cmd(WORD id, WORD msg, DWORD fl, DWORD FAR *p)
{
    struct mdev FAR *d;
    DWORD r = 0, cb = p ? p[0] : 0;
    if (!mci_init())
        return MCIERR_OUT_OF_MEMORY;
    if (msg == MCI_OPEN) {
        if (!p)
            return MCIERR_NULL_PARAMETER_BLOCK;
        r = m_open(fl, p);
        if (!r && (fl & MCI_NOTIFY))
            notify(*((WORD FAR *)&p[1]), LOWORD(cb), MCI_NOTIFY_SUCCESSFUL);
        return r;
    }
    if (msg == MCI_SYSINFO)
        return p ? m_sysinfo(id, fl, p) : MCIERR_NULL_PARAMETER_BLOCK;
    if (msg == MCI_SOUND) {
        sndPlaySound((fl & MCI_SOUND_NAME) && p ? (LPCSTR)p[1] : s_sysdef,
                     (fl & MCI_WAIT) ? 0 : 1);
        return 0;
    }
    if (id == MCI_ALL_DEVICE_ID) {
        WORD k;
        if (msg != MCI_CLOSE)
            return MCIERR_CANNOT_USE_ALL;
        for (k = 0; k < NMCI; k++)
            if (C->d[k].used && C->d[k].task == cur_task)
                d_close(k + 1);
        return 0;
    }
    if (!id || id > NMCI || !DEV(id)->used)
        return MCIERR_INVALID_DEVICE_ID;
    d = DEV(id);
    if (!p && msg != MCI_CLOSE && msg != MCI_STOP && msg != MCI_PAUSE &&
        msg != MCI_RESUME && msg != MCI_PLAY && msg != MCI_BREAK)
        return MCIERR_NULL_PARAMETER_BLOCK;
    switch (msg) {
    case MCI_CLOSE:
        d_close(id);
        break;
    case MCI_PLAY:
        if (!p) {
            fmemset(C->parms, 0, sizeof(C->parms));
            p = C->parms;
        }
        return m_play(id, fl, p);
    case MCI_STOP:
        d_halt(d);
        d->mode = 0;
        d_abort(id, MCI_NOTIFY_ABORTED);
        break;
    case MCI_PAUSE:
        d_halt(d);
        if (d->mode == 1)
            d->mode = 2;
        break;
    case MCI_RESUME:
        if (d->mode == 2) {
            r = d->type == T_WAVE ? mw_play(mw_where(), d->to) : sq_play(sq_where(), d->to);
            if (!r)
                d->mode = 1;
        }
        break;
    case MCI_SEEK: {
        DWORD to;
        if (!d->loaded)
            return MCIERR_FILENAME_REQUIRED;
        to = (fl & MCI_SEEK_TO_START) ? 0 : (fl & MCI_SEEK_TO_END) ? d_len(d) :
             (fl & MCI_TO) ? to_int(d, p[1]) : d_pos(d);
        if (to > d_len(d))
            return MCIERR_OUTOFRANGE;
        d_abort(id, MCI_NOTIFY_ABORTED);
        if (d->type == T_WAVE)
            mw_seek(to);
        else
            sq_seek(to);
        d->mode = 0;
        break;
    }
    case MCI_STATUS:
        r = m_status(id, fl, p);
        break;
    case MCI_SET:
        if (fl & MCI_SET_TIME_FORMAT) {
            WORD f = (WORD)p[1];
            if (f != MCI_FORMAT_MILLISECONDS &&
                !(d->type == T_WAVE && (f == MCI_FORMAT_BYTES || f == MCI_FORMAT_SAMPLES)))
                return MCIERR_BAD_TIME_FORMAT;
            d->tfmt = f;
        }
        break;
    case MCI_GETDEVCAPS:
        r = m_caps(id, fl, p);
        break;
    case MCI_INFO:
        r = m_info(id, fl, p);
        break;
    case MCI_BREAK:
        break;
    default:
        return MCIERR_UNSUPPORTED_FUNCTION;
    }
    if (!r && (fl & MCI_NOTIFY) && p && msg != MCI_PLAY)
        notify(id, LOWORD(cb), MCI_NOTIFY_SUCCESSFUL);
    return r;
}

DWORD API mciSendCommand(WORD id, WORD msg, DWORD fl, DWORD parm)
{
    return mci_cmd(id, msg, fl, (DWORD FAR *)parm);
}

/* aus mm_pump: zu Ende gespielt? */
void mci_poll(void)
{
    WORD k;
    int sqe;
    if (!mci_sel)
        return;
    sqe = sq_ended();
    for (k = 0; k < NMCI; k++) {
        struct mdev FAR *d = &C->d[k];
        if (!d->used || d->mode != 1)
            continue;
        if (d->type == T_WAVE ? (mw_playing() || !S->mw_done) : !sqe)
            continue;
        if (d->type == T_WAVE)
            S->mw_done = 0;
        d->mode = 0;
        if (d->pend) {
            d->pend = 0;
            notify(k + 1, d->nwnd, MCI_NOTIFY_SUCCESSFUL);
        }
        if (d->autoclose)
            d_close(k + 1);
    }
}
void mci_task_end(int t)
{
    WORD k;
    if (!mci_sel)
        return;
    for (k = 0; k < NMCI; k++)
        if (C->d[k].used && C->d[k].task == t) {
            C->d[k].pend = 0;
            d_close(k + 1);
        }
}

WORD API mciGetDeviceID(LPCSTR name)
{
    WORD k;
    if (!mci_sel || !name)
        return 0;
    if (ieq(name, kwget(s_kw, K_ALL)))
        return MCI_ALL_DEVICE_ID;
    for (k = 0; k < NMCI; k++)
        if (C->d[k].used && (ieq(C->d[k].alias, name) || ieq(C->d[k].elem, name)))
            return k + 1;
    return 0;
}

BOOL API mciGetErrorString(DWORD err, LPSTR buf, WORD n)
{
    WORD i;
    if (!buf || !n)
        return 0;
    if ((WORD)err < MCIERR_BASE)
        return !mm_errtext((WORD)err, buf, n);
    for (i = 0; etab[i].e && etab[i].e != (WORD)err; i++)
        ;
    fstrncpy(buf, etab[i].t, n);
    return etab[i].e != 0;
}

/* ---- mciSendString --------------------------------------------------------- */
static void split(void)
{
    char FAR *s = C->cmd;
    C->ntok = 0;
    for (;;) {
        WORD n = 0;
        char q = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || C->ntok == 16)
            break;
        if (*s == '"')
            q = *s++;
        while (*s && (q ? *s != q : (*s != ' ' && *s != '\t'))) {
            if (n < 127)
                C->tok[C->ntok][n++] = *s;
            s++;
        }
        if (q && *s)
            s++;
        C->tok[C->ntok++][n] = 0;
    }
}
static int tokis(WORD i, int k) { return i < C->ntok && ieq(C->tok[i], kwget(s_kw, k)); }
static int num(WORD i, DWORD FAR *v)
{
    const char FAR *s;
    DWORD x = 0;
    if (i >= C->ntok)
        return 0;
    s = C->tok[i];
    if (!*s)
        return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 0;
        x = x * 10 + (*s - '0');
    }
    *v = x;
    return 1;
}
static void put_num(LPSTR r, WORD n, DWORD v)
{
    char b[12];
    int i = 11;
    b[11] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i);
    fstrncpy(r, b + i, n);
}
/* Woerter ab i zu einem Ausdruck zusammenfassen (bis 3) und in t suchen */
static int phrase(const char __far *t, WORD i)
{
    WORD w, k;
    for (w = 3; w >= 1; w--) {
        if (i + w > C->ntok)
            continue;
        C->tmp[0] = 0;
        for (k = 0; k < w; k++) {
            WORD l = fstrlen(C->tmp);
            if (k)
                C->tmp[l++] = ' ';
            fstrncpy(C->tmp + l, C->tok[i + k], sizeof(C->tmp) - l);
        }
        if ((k = kwfind(t, C->tmp)) != (WORD)-1)
            return k;
    }
    return -1;
}

DWORD API mciSendString(LPCSTR cmd, LPSTR ret, WORD rn, WORD wnd)
{
    int c, x;
    WORD id, i = 2, msg;
    DWORD fl = 0, r, v;
    DWORD FAR *p;
    if (ret && rn)
        ret[0] = 0;
    if (!cmd || !mci_init())
        return MCIERR_OUT_OF_MEMORY;
    fstrncpy(C->cmd, cmd, sizeof(C->cmd));
    split();
    if (!C->ntok)
        return MCIERR_MISSING_COMMAND_STRING;
    c = kwfind(s_cmds, C->tok[0]);
    if (c < 0)
        return MCIERR_UNRECOGNIZED_COMMAND;
    msg = cmd_msg[c];
    if (C->ntok < 2)
        return MCIERR_MISSING_DEVICE_NAME;
    p = C->parms;
    fmemset(p, 0, sizeof(C->parms));
    p[0] = wnd;
    /* nachgestellte notify/wait gelten fuer jeden Befehl */
    while (C->ntok > 2 && (tokis(C->ntok - 1, K_NOTIFY) || tokis(C->ntok - 1, K_WAIT))) {
        fl |= tokis(C->ntok - 1, K_NOTIFY) ? MCI_NOTIFY : MCI_WAIT;
        C->ntok--;
    }
    if (msg == MCI_SOUND) {
        p[1] = (DWORD)(LPCSTR)C->tok[1];
        return mci_cmd(0, msg, fl | MCI_SOUND_NAME, p);
    }
    if (msg == MCI_SYSINFO) {
        x = kwfind(s_types, C->tok[1]);
        p[4] = x == 0 ? MCI_DEVTYPE_WAVEFORM_AUDIO : x == 1 ? MCI_DEVTYPE_SEQUENCER : 0;
        if (x < 0 && !tokis(1, K_ALL))
            return MCIERR_INVALID_DEVICE_NAME;
        p[1] = (DWORD)(LPSTR)C->out;
        p[2] = sizeof(C->out);
        for (; i < C->ntok; i++)
            if (tokis(i, K_QUANTITY)) fl |= MCI_SYSINFO_QUANTITY;
            else if (tokis(i, K_OPEN)) fl |= MCI_SYSINFO_OPEN;
            else if (tokis(i, K_NAME) && num(i + 1, &v)) fl |= MCI_SYSINFO_NAME, p[3] = v, i++;
            else if (tokis(i, K_INSTALLNAME)) fl |= MCI_SYSINFO_INSTALLNAME, p[3] = 1;
            else return MCIERR_UNRECOGNIZED_KEYWORD;
        r = mci_cmd(MCI_ALL_DEVICE_ID, msg, fl, p);
        if (!r && ret && rn) {
            if (fl & MCI_SYSINFO_QUANTITY)
                put_num(ret, rn, *(DWORD FAR *)C->out);
            else
                fstrncpy(ret, C->out, rn);
        }
        return r;
    }
    if (msg == MCI_OPEN) {
        /* Geraet: Typ, Datei oder Typ!Datei */
        char FAR *bang = 0, FAR *s;
        for (s = C->tok[1]; *s; s++)
            if (*s == '!')
                bang = s;
        if (bang) {
            *bang = 0;
            p[2] = (DWORD)(LPCSTR)C->tok[1];
            p[3] = (DWORD)(LPCSTR)(bang + 1);
            fl |= MCI_OPEN_TYPE | MCI_OPEN_ELEMENT;
        } else if (kwfind(s_types, C->tok[1]) >= 0) {
            p[2] = (DWORD)(LPCSTR)C->tok[1];
            fl |= MCI_OPEN_TYPE;
        } else {
            p[3] = (DWORD)(LPCSTR)C->tok[1];
            fl |= MCI_OPEN_ELEMENT;
        }
        for (; i < C->ntok; i++)
            if (tokis(i, K_TYPE) && i + 1 < C->ntok) {
                p[2] = (DWORD)(LPCSTR)C->tok[++i];
                fl |= MCI_OPEN_TYPE;
            } else if (tokis(i, K_ALIAS) && i + 1 < C->ntok) {
                p[4] = (DWORD)(LPCSTR)C->tok[++i];
                fl |= MCI_OPEN_ALIAS;
            } else if (tokis(i, K_SHAREABLE))
                fl |= MCI_OPEN_SHAREABLE;
            else
                return MCIERR_UNRECOGNIZED_KEYWORD;
        r = mci_cmd(0, msg, fl, p);
        if (!r && ret && rn)
            put_num(ret, rn, *((WORD FAR *)&p[1]));
        return r;
    }
    /* Geraet suchen, sonst als Datei selbst oeffnen (wird danach zu) */
    id = mciGetDeviceID(C->tok[1]);
    if (!id) {
        int ty = type_of_file(C->tok[1]);
        if (!ty)
            return MCIERR_INVALID_DEVICE_NAME;
        C->parms[3] = (DWORD)(LPCSTR)C->tok[1];
        r = m_open(MCI_OPEN_ELEMENT, C->parms);
        if (r)
            return r;
        id = *((WORD FAR *)&C->parms[1]);
        DEV(id)->autoclose = 1;
        fmemset(p, 0, sizeof(C->parms));
        p[0] = wnd;
    }
    switch (msg) {
    case MCI_PLAY:
        for (; i < C->ntok; i++)
            if (tokis(i, K_FROM) && num(i + 1, &v)) p[1] = v, fl |= MCI_FROM, i++;
            else if (tokis(i, K_TO) && num(i + 1, &v)) p[2] = v, fl |= MCI_TO, i++;
            else return MCIERR_UNRECOGNIZED_KEYWORD;
        break;
    case MCI_SEEK:
        if (tokis(i, K_TO) && tokis(i + 1, K_START)) fl |= MCI_SEEK_TO_START;
        else if (tokis(i, K_TO) && tokis(i + 1, K_END)) fl |= MCI_SEEK_TO_END;
        else if (tokis(i, K_TO) && num(i + 1, &v)) p[1] = v, fl |= MCI_TO;
        else return MCIERR_MISSING_PARAMETER;
        break;
    case MCI_STATUS:
        x = phrase(s_items, i);
        if (x < 0)
            return MCIERR_UNRECOGNIZED_KEYWORD;
        fl |= MCI_STATUS_ITEM;
        p[2] = x == 0 ? 1 : x == 1 ? 2 : x == 2 ? 3 : x == 3 ? 4 : x == 4 ? 5 :
               x == 5 ? 6 : x == 6 ? 7 : x == 7 ? 8 : 2;
        if (x == 8)
            fl |= MCI_STATUS_START;
        break;
    case MCI_GETDEVCAPS:
        x = phrase(s_caps, i);
        if (x < 0)
            return MCIERR_UNRECOGNIZED_KEYWORD;
        fl |= MCI_GETDEVCAPS_ITEM;
        p[2] = x + 1;
        break;
    case MCI_SET:
        if (tokis(i, K_TIME) && tokis(i + 1, K_FORMAT) && i + 2 < C->ntok) {
            x = kwfind(s_fmts, C->tok[i + 2]);
            if (x < 0)
                return MCIERR_BAD_TIME_FORMAT;
            p[1] = x <= 1 ? MCI_FORMAT_MILLISECONDS : x == 2 ? MCI_FORMAT_BYTES : MCI_FORMAT_SAMPLES;
            fl |= MCI_SET_TIME_FORMAT;
        } else if (!tokis(i, K_AUDIO))
            return MCIERR_UNRECOGNIZED_KEYWORD;
        break;
    case MCI_INFO:
        p[1] = (DWORD)(LPSTR)C->out;
        p[2] = sizeof(C->out);
        if (tokis(i, K_FILE)) fl |= MCI_INFO_FILE;
        else if (tokis(i, K_PRODUCT)) fl |= MCI_INFO_PRODUCT;
        else return MCIERR_MISSING_PARAMETER;
        break;
    default:
        if (i < C->ntok)
            return MCIERR_UNRECOGNIZED_KEYWORD;
    }
    r = mci_cmd(id, msg, fl, p);
    if (r || !ret || !rn)
        return r;
    if (msg == MCI_INFO)
        fstrncpy(ret, C->out, rn);
    else if (msg == MCI_STATUS) {
        v = p[1];
        if (p[2] == 4)
            fstrncpy(ret, kwget(s_modes, (WORD)v - MCI_MODE_NOT_READY), rn);
        else if (p[2] == 5 || p[2] == 7)
            fstrncpy(ret, kwget(s_bool, v != 0), rn);
        else if (p[2] == 6)
            fstrncpy(ret, kwget(s_fmts, v == MCI_FORMAT_BYTES ? 2 : v == MCI_FORMAT_SAMPLES ? 3 : 0), rn);
        else
            put_num(ret, rn, v);
    } else if (msg == MCI_GETDEVCAPS) {
        if (p[2] == 4)
            fstrncpy(ret, kwget(s_types, p[1] == MCI_DEVTYPE_SEQUENCER), rn);
        else
            fstrncpy(ret, kwget(s_bool, p[1] != 0), rn);
    }
    return 0;
}

BOOL API mciExecute(LPCSTR cmd)
{
    DWORD r = mciSendString(cmd, 0, 0, 0);
    if (r) {
        mciGetErrorString(r, C->tmp, sizeof(C->tmp));
        MessageBox(0, C->tmp, s_mci, 0x30);
        return 0;
    }
    return 1;
}
BOOL  API mciSetYieldProc(WORD id, DWORD proc, DWORD data) { (void)id; (void)proc; (void)data; return 1; }
DWORD API mciGetYieldProc(WORD id, DWORD FAR *data) { (void)id; if (data) *data = 0; return 0; }
WORD  API mciGetCreatorTask(WORD id)
{
    return mci_sel && id && id <= NMCI && DEV(id)->used ? DEV(id)->task + 1 : 0;
}
