/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - MMSYSTEM: gemeinsame Teile von mmsystem.c (Wave, sndPlaySound),
 * mmio.c, mmmidi.c (midiOut, FM, Sequenzer) und mci.c.
 */
#ifndef MM_H
#define MM_H
#include "gui.h"


#define MMSYSERR_NOERROR        0
#define MMSYSERR_ERROR          1
#define MMSYSERR_BADDEVICEID    2
#define MMSYSERR_ALLOCATED      4
#define MMSYSERR_INVALHANDLE    5
#define MMSYSERR_NODRIVER       6
#define MMSYSERR_NOMEM          7
#define MMSYSERR_NOTSUPPORTED   8
#define MMSYSERR_INVALFLAG      10
#define MMSYSERR_INVALPARAM     11
#define WAVERR_BADFORMAT        32
#define WAVERR_STILLPLAYING     33
#define WAVERR_UNPREPARED       34

#define WHDR_DONE       0x01
#define WHDR_PREPARED   0x02
#define WHDR_BEGINLOOP  0x04
#define WHDR_ENDLOOP    0x08
#define WHDR_INQUEUE    0x10

#define WAVE_FORMAT_QUERY   0x0001
#define CALLBACK_TYPEMASK   0x00070000UL
#define CALLBACK_WINDOW     0x00010000UL
#define CALLBACK_TASK       0x00020000UL
#define CALLBACK_FUNCTION   0x00030000UL

#define MM_WOM_OPEN     0x3BB
#define MM_WOM_CLOSE    0x3BC
#define MM_WOM_DONE     0x3BD

#define SND_ASYNC       0x0001
#define SND_NODEFAULT   0x0002
#define SND_MEMORY      0x0004
#define SND_LOOP        0x0008
#define SND_NOSTOP      0x0010

#define HWO             0x0B01          /* das eine Wave-Geraet          */
#define NCB             8
#define SBUF            4096            /* Lesepuffer fuer sndPlaySound  */

typedef struct wavehdr {
    char FAR *lpData;
    DWORD dwBufferLength, dwBytesRecorded, dwUser, dwFlags, dwLoops;
    struct wavehdr FAR *lpNext;
    DWORD reserved;                     /* hier: Takt, ab dem er fertig ist */
} WAVEHDR;
typedef WAVEHDR FAR *LPWAVEHDR;
typedef struct {
    WORD  wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD  nBlockAlign, wBitsPerSample;
} PCMWAVEFORMAT;

struct mmcb { BYTE t; DWORD fn; WORD h, msg; DWORD inst, p1, p2; };
struct mmst {
    /* Strom zur Karte: 0 frei, 1 waveOut, 2 sndPlaySound */
    BYTE  owner;
    WORD  rate;
    BYTE  ch, bits;
    WORD  align;
    DWORD bps;                          /* Bytes je Sekunde              */
    DWORD cap;                          /* Vorrat im Treiber             */
    DWORD backlog;                      /* geschaetzt noch nicht gespielt */
    DWORD rem;                          /* Rest unter einem Takt (Byte*100) */
    WORD  last;                         /* DosQTicks beim letzten Pumpen */
    /* waveOut */
    BYTE  wo_open, paused;
    int   wo_task;
    DWORD cb, inst, woflags;
    LPWAVEHDR head, tail, wr;           /* Schlange, gerade geschrieben  */
    DWORD wpos, loops;
    LPWAVEHDR loopstart;
    DWORD written;
    /* sndPlaySound */
    BYTE  snd, sloop, smem;
    WORD  fh;
    DWORD dstart, dlen, dleft;
    const BYTE FAR *mp;                 /* SND_MEMORY: Daten             */
    WORD  bpos, blen;
    DWORD seekpos;
    char  path[128];
    char  prof[128];
    /* MCI waveaudio (Besitzer 3): geladene Datei, Stellen in Byte der Daten */
    BYTE  mw_done;                      /* Wiedergabe am Ende angekommen  */
    WORD  mw_fh;
    PCMWAVEFORMAT mw_f;
    DWORD mw_dstart, mw_dlen;
    DWORD mw_pos;                       /* Stelle, wenn angehalten        */
    DWORD mw_base, mw_to;               /* Start und Ende der Wiedergabe  */
    struct mmcb q[NCB];
    BYTE  qn;
    BYTE  buf[SBUF];
};
extern WORD mm_sel;
#define S ((struct mmst FAR *)MKFP(mm_sel, 0))


#define MM_MOM_OPEN     0x3C7
#define MM_MOM_CLOSE    0x3C8
#define MM_MOM_DONE     0x3C9
#define MM_MCINOTIFY    0x3B9

/* mmsystem.c */
int   mm_init(void);
WORD  mm_qticks(void);
WORD  mm_caps(void);
WORD  mm_errtext(WORD e, LPSTR buf, WORD n);
void  mm_cbfire(DWORD cb, DWORD flags, int t, WORD h, WORD msg, DWORD inst, DWORD p1);
void  mm_cbcall(DWORD fn, WORD h, WORD msg, DWORD inst, DWORD p1, DWORD p2, int t);
void  mm_cbopen(DWORD cb, DWORD flags, WORD h, WORD msg, DWORD inst);
WORD  mw_load(LPCSTR path);
void  mw_unload(void);
WORD  mw_play(DWORD from, DWORD to);
void  mw_halt(void);
DWORD mw_where(void);
void  mw_seek(DWORD b);
DWORD API timeGetTime(void);
int   mw_playing(void);
/* mmmidi.c */
WORD  md_ndev(void);
WORD  md_open(WORD dev, BYTE who);
void  md_close(void);
void  md_short(DWORD m);
void  md_reset(void);
WORD  sq_load(LPCSTR path);
void  sq_unload(void);
DWORD sq_length(void);
WORD  sq_play(DWORD from, DWORD to);
void  sq_halt(void);
DWORD sq_where(void);
void  sq_seek(DWORD ms);
int   sq_playing(void);
int   sq_pump(void);                    /* 1 = gerade am Ende angekommen */
int   sq_ended(void);                   /* seit der letzten Frage am Ende */
void  md_task_end(int t);
/* mci.c */
void  mci_poll(void);
void  mci_task_end(int t);
/* mmio.c */
void  mmio_task_end(int t);

#define MCIERR_BASE                 256
#define MCIERR_INVALID_DEVICE_ID    (MCIERR_BASE + 1)
#define MCIERR_UNRECOGNIZED_KEYWORD (MCIERR_BASE + 3)
#define MCIERR_UNRECOGNIZED_COMMAND (MCIERR_BASE + 5)
#define MCIERR_HARDWARE             (MCIERR_BASE + 6)
#define MCIERR_INVALID_DEVICE_NAME  (MCIERR_BASE + 7)
#define MCIERR_OUT_OF_MEMORY        (MCIERR_BASE + 8)
#define MCIERR_DEVICE_OPEN          (MCIERR_BASE + 9)
#define MCIERR_MISSING_COMMAND_STRING (MCIERR_BASE + 11)
#define MCIERR_PARAM_OVERFLOW       (MCIERR_BASE + 12)
#define MCIERR_MISSING_STRING_ARGUMENT (MCIERR_BASE + 13)
#define MCIERR_BAD_INTEGER          (MCIERR_BASE + 14)
#define MCIERR_MISSING_PARAMETER    (MCIERR_BASE + 17)
#define MCIERR_UNSUPPORTED_FUNCTION (MCIERR_BASE + 18)
#define MCIERR_FILE_NOT_FOUND       (MCIERR_BASE + 19)
#define MCIERR_DEVICE_NOT_READY     (MCIERR_BASE + 20)
#define MCIERR_INTERNAL             (MCIERR_BASE + 21)
#define MCIERR_CANNOT_USE_ALL       (MCIERR_BASE + 23)
#define MCIERR_EXTENSION_NOT_FOUND  (MCIERR_BASE + 25)
#define MCIERR_OUTOFRANGE           (MCIERR_BASE + 26)
#define MCIERR_FLAGS_NOT_COMPATIBLE (MCIERR_BASE + 28)
#define MCIERR_DEVICE_TYPE_REQUIRED (MCIERR_BASE + 31)
#define MCIERR_DUPLICATE_ALIAS      (MCIERR_BASE + 33)
#define MCIERR_BAD_CONSTANT         (MCIERR_BASE + 34)
#define MCIERR_MISSING_DEVICE_NAME  (MCIERR_BASE + 36)
#define MCIERR_BAD_TIME_FORMAT      (MCIERR_BASE + 37)
#define MCIERR_NO_CLOSING_QUOTE     (MCIERR_BASE + 38)
#define MCIERR_INVALID_FILE         (MCIERR_BASE + 40)
#define MCIERR_NULL_PARAMETER_BLOCK (MCIERR_BASE + 41)
#define MCIERR_NONAPPLICABLE_FUNCTION (MCIERR_BASE + 46)
#define MCIERR_FILENAME_REQUIRED    (MCIERR_BASE + 48)
#define MCIERR_EXTRA_CHARACTERS     (MCIERR_BASE + 49)
#define MCIERR_DEVICE_NOT_INSTALLED (MCIERR_BASE + 50)
#define MCIERR_NO_INTEGER           (MCIERR_BASE + 56)
#define MCIERR_WAVE_OUTPUTSINUSE    (MCIERR_BASE + 64)
#define MCIERR_SEQ_PORT_INUSE       (MCIERR_BASE + 80)
#define MCIERR_SEQ_PORT_NONEXISTENT (MCIERR_BASE + 82)
#endif
