/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - MMSYSTEM: Datei-Ein-/Ausgabe mmio* (RIFF-Dateien, WAV).
 *
 * DOS-Dateien ueber die KERNEL-Funktionen der GUI (_lopen & Co.) und
 * Speicherdateien (FOURCC_MEM). Ungepuffert, ausser mit MMIO_ALLOCBUF bzw.
 * mmioSetBuffer; dann gilt MMIOINFO wie unter Windows: lBufOffset =
 * Stelle des Puffers in der Datei, pchNext die aktuelle Stelle,
 * pchEndRead das Ende der gueltigen Daten, MMIO_DIRTY = Puffer geaendert.
 * Eigene E/A-Prozeduren (mmioInstallIOProc) gibt es nicht.
 */
#include "mm.h"

#define MMIO_RWMODE     0x00000003UL
#define MMIO_WRITE      0x00000001UL
#define MMIO_READWRITE  0x00000002UL
#define MMIO_CREATE     0x00001000UL
#define MMIO_PARSE      0x00000100UL
#define MMIO_DELETE     0x00000200UL
#define MMIO_EXIST      0x00004000UL
#define MMIO_ALLOCBUF   0x00010000UL
#define MMIO_GETTEMP    0x00020000UL
#define MMIO_DIRTY      0x10000000UL
#define MMIO_TOUPPER    0x0010
#define MMIO_FINDCHUNK  0x0010
#define MMIO_FINDRIFF   0x0020
#define MMIO_FINDLIST   0x0040
#define MMIO_CREATERIFF 0x0020
#define MMIO_CREATELIST 0x0040
#define MMIO_EMPTYBUF   0x0010

#define MMIOERR_FILENOTFOUND    257
#define MMIOERR_OUTOFMEMORY     258
#define MMIOERR_CANNOTOPEN      259
#define MMIOERR_CANNOTREAD      261
#define MMIOERR_CANNOTWRITE     262
#define MMIOERR_CANNOTSEEK      263
#define MMIOERR_CANNOTEXPAND    264
#define MMIOERR_CHUNKNOTFOUND   265
#define MMIOERR_UNBUFFERED      266

#define FOURCC_RIFF     0x46464952UL
#define FOURCC_LIST     0x5453494CUL
#define FOURCC_MEM      0x204D454DUL
#define FOURCC_DOS      0x20534F44UL

typedef char FAR *PB;
struct minfo {                          /* MMIOINFO (Win16), 66 Byte     */
    DWORD dwFlags, fccIOProc, pIOProc;
    WORD  wErrorRet, htask;
    long  cchBuffer;
    PB    pchBuffer;
    PB    pchNext;
    PB    pchEndRead;
    PB    pchEndWrite;
    long  lBufOffset, lDiskOffset;
    DWORD adwInfo[3], r1, r2;
    WORD  hmmio;
};
struct mmck { DWORD ckid, cksize, fccType, dwDataOffset, dwFlags; };

#define NMIO    8
struct miohd {                          /* vorn im Segment: Arbeitsplatz */
    char  mp[FAPI_PATHLEN];
    char  nb[FAPI_PATHLEN];
    struct mmck ck0;
    char  zero;
};
struct mio {
    BYTE  used, mem, own;
    int   task;
    WORD  fh;
    WORD  bsel;                         /* eigener Puffer                */
    struct minfo i;
};
static WORD mio_sel;
#define H       ((struct miohd FAR *)MKFP(mio_sel, 0))
#define MIO(k)  ((struct mio FAR *)MKFP(mio_sel, sizeof(struct miohd) + (k) * sizeof(struct mio)))
#define SEGSZ   (sizeof(struct miohd) + NMIO * sizeof(struct mio))

static int mio_init(void)
{
    if (!mio_sel) {
        mio_sel = seg_new(SEGSZ);
        if (!mio_sel)
            return 0;
        fmemset(MKFP(mio_sel, 0), 0, SEGSZ);
    }
    return 1;
}
#define HBASE   0x0C01

WORD  API _lopen(LPCSTR name, WORD mode);
WORD  API _lcreat(LPCSTR name, WORD attr);
WORD  API _lclose(WORD h);
WORD  API _lread(WORD h, LPVOID p, WORD n);
WORD  API _lwrite(WORD h, LPCSTR p, WORD n);
DWORD API _llseek(WORD h, DWORD pos, short how);

static struct mio FAR *get(WORD h)
{
    WORD k = h - HBASE;
    if (!mio_sel || k >= NMIO || !MIO(k)->used)
        return 0;
    return MIO(k);
}
static int buffered(struct mio FAR *m) { return m->i.pchBuffer != 0; }
static long lpos(struct mio FAR *m)
{
    if (!buffered(m))
        return m->i.lDiskOffset;
    return m->i.lBufOffset + (OFFOF(m->i.pchNext) - OFFOF(m->i.pchBuffer));
}
/* huge-Zeiger weiterzaehlen (__AHINCR = 8) */
static PB hadd(PB p, DWORD n)
{
    DWORD o = (DWORD)OFFOF(p) + n;
    return (PB)MKFP(SELOF(p) + (WORD)(o >> 16) * 8, (WORD)o);
}
/* Datei direkt lesen/schreiben, auch ueber Segmentgrenzen */
static long raw_rw(struct mio FAR *m, int wr, PB p, long n)
{
    long done = 0;
    while (n > 0) {
        WORD k = n > 0x4000 ? 0x4000 : (WORD)n, r;
        if ((DWORD)OFFOF(p) + k > 0x10000UL)
            k = (WORD)(0x10000UL - OFFOF(p));
        r = wr ? _lwrite(m->fh, p, k) : _lread(m->fh, p, k);
        if (r == 0xFFFF)
            return done ? done : -1;
        done += r;
        n -= r;
        p = hadd(p, r);
        if (r < k)
            break;
    }
    m->i.lDiskOffset += done;
    return done;
}
static int disk_seek(struct mio FAR *m, long off)
{
    if (m->i.lDiskOffset == off)
        return 1;
    if (_llseek(m->fh, off, 0) == 0xFFFFFFFFUL)
        return 0;
    m->i.lDiskOffset = off;
    return 1;
}
/* geaenderten Puffer schreiben; danach ist er leer an der aktuellen Stelle */
static int flush(struct mio FAR *m)
{
    WORD n;
    long p;
    if (!buffered(m) || m->mem)
        return 1;
    p = lpos(m);
    if (m->i.dwFlags & MMIO_DIRTY) {
        n = OFFOF(m->i.pchEndRead) > OFFOF(m->i.pchNext) ? OFFOF(m->i.pchEndRead) : OFFOF(m->i.pchNext);
        n -= OFFOF(m->i.pchBuffer);
        if (n && (!disk_seek(m, m->i.lBufOffset) || raw_rw(m, 1, m->i.pchBuffer, n) != n))
            return 0;
        m->i.dwFlags &= ~MMIO_DIRTY;
    }
    m->i.lBufOffset = p;
    m->i.pchNext = m->i.pchEndRead = m->i.pchBuffer;
    return 1;
}
static int fill(struct mio FAR *m)
{
    long n;
    if (!flush(m) || !disk_seek(m, m->i.lBufOffset))
        return 0;
    n = raw_rw(m, 0, m->i.pchBuffer, m->i.cchBuffer);
    if (n < 0)
        return 0;
    m->i.pchEndRead = m->i.pchBuffer + (WORD)n;
    return 1;
}

static WORD set_buf(struct mio FAR *m, PB buf, long cch)
{
    long p = lpos(m);
    if (!flush(m))
        return MMIOERR_CANNOTWRITE;
    if (m->own && m->bsel)
        seg_del(m->bsel);
    m->own = 0;
    m->bsel = 0;
    if (cch > 0xFFF0L)
        cch = 0xFFF0L;
    if (!buf && cch > 0) {
        m->bsel = seg_new((WORD)cch);
        if (!m->bsel)
            return MMIOERR_OUTOFMEMORY;
        m->own = 1;
        buf = (PB)MKFP(m->bsel, 0);
    }
    if (!buf || cch <= 0) {
        m->i.pchBuffer = m->i.pchNext = m->i.pchEndRead = m->i.pchEndWrite = 0;
        m->i.cchBuffer = 0;
        m->i.lDiskOffset = p;
        if (!m->mem)
            _llseek(m->fh, p, 0);
        return 0;
    }
    m->i.pchBuffer = m->i.pchNext = m->i.pchEndRead = buf;
    m->i.pchEndWrite = buf + (WORD)cch;
    m->i.cchBuffer = cch;
    m->i.lBufOffset = p;
    return 0;
}

WORD API mmioOpen(LPSTR name, struct minfo FAR *in, DWORD fl)
{
    WORD k, h;
    struct mio FAR *m;
    if (in)
        in->wErrorRet = 0;
    if (!mio_init()) {
        if (in) in->wErrorRet = MMIOERR_OUTOFMEMORY;
        return 0;
    }
    if (fl & MMIO_PARSE) {
        if (!name)
            return 0;
        full_path(name, H->mp, sizeof(H->mp));
        fstrncpy(name, H->mp, 128);
        return 1;
    }
    if (fl & MMIO_DELETE) {
        if (!name)
            return 0;
        fstrncpy(H->mp, name, sizeof(H->mp));
        return FAPI_FAILED(fapi_sel(FAPI_DELETE, mio_sel, OFFOF(H->mp), 0, 0)) ? 0 : 1;
    }
    if (fl & MMIO_EXIST) {
        if (!name)
            return 0;
        h = _lopen(name, 0);
        if (h == 0xFFFF)
            return 0;
        _lclose(h);
        return 1;
    }
    if (in && (in->pIOProc || (in->fccIOProc && in->fccIOProc != FOURCC_MEM &&
                               in->fccIOProc != FOURCC_DOS))) {
        in->wErrorRet = MMIOERR_CANNOTOPEN;
        return 0;
    }
    for (k = 0; k < NMIO && MIO(k)->used; k++)
        ;
    if (k == NMIO) {
        if (in) in->wErrorRet = MMIOERR_OUTOFMEMORY;
        return 0;
    }
    m = MIO(k);
    fmemset(m, 0, sizeof(*m));
    m->task = cur_task;
    if (in && in->fccIOProc == FOURCC_MEM) {
        /* Speicherdatei: der ganze Inhalt ist der Puffer */
        if (!in->pchBuffer) {
            in->wErrorRet = MMIOERR_OUTOFMEMORY;
            return 0;
        }
        m->mem = 1;
        m->i.fccIOProc = FOURCC_MEM;
        m->i.cchBuffer = in->cchBuffer;
        m->i.pchBuffer = m->i.pchNext = in->pchBuffer;
        m->i.pchEndWrite = in->pchBuffer + (WORD)in->cchBuffer;
        m->i.pchEndRead = (fl & MMIO_CREATE) ? in->pchBuffer : m->i.pchEndWrite;
    } else {
        if (!name) {
            if (in) in->wErrorRet = MMIOERR_FILENOTFOUND;
            return 0;
        }
        if (fl & MMIO_CREATE)
            h = _lcreat(name, 0);
        else
            h = _lopen(name, (WORD)(fl & MMIO_RWMODE));
        if (h == 0xFFFF) {
            if (in) in->wErrorRet = (fl & MMIO_CREATE) ? MMIOERR_CANNOTOPEN : MMIOERR_FILENOTFOUND;
            return 0;
        }
        m->fh = h;
        m->i.fccIOProc = FOURCC_DOS;
        if (in && in->pchBuffer && in->cchBuffer > 0)
            set_buf(m, in->pchBuffer, in->cchBuffer);
        else if (fl & MMIO_ALLOCBUF)
            set_buf(m, 0, in && in->cchBuffer > 0 ? in->cchBuffer : 8192);
    }
    m->i.dwFlags = fl & ~(MMIO_DIRTY | MMIO_ALLOCBUF);
    m->i.htask = cur_task + 1;
    m->i.hmmio = HBASE + k;
    m->used = 1;
    if (in)
        in->hmmio = HBASE + k;
    return HBASE + k;
}

WORD API mmioClose(WORD h, WORD fl)
{
    struct mio FAR *m = get(h);
    WORD r = 0;
    (void)fl;
    if (!m)
        return MMSYSERR_INVALHANDLE;
    if (!flush(m))
        r = MMIOERR_CANNOTWRITE;
    if (!m->mem)
        _lclose(m->fh);
    if (m->own && m->bsel)
        seg_del(m->bsel);
    m->used = 0;
    return r;
}

long API mmioRead(WORD h, PB p, long n)
{
    struct mio FAR *m = get(h);
    long done = 0;
    if (!m)
        return -1;
    if (!buffered(m))
        return raw_rw(m, 0, p, n);
    while (n > 0) {
        WORD av = OFFOF(m->i.pchEndRead) > OFFOF(m->i.pchNext) ?
                  OFFOF(m->i.pchEndRead) - OFFOF(m->i.pchNext) : 0, k;
        if (!av) {
            if (m->mem || !fill(m))
                break;
            av = OFFOF(m->i.pchEndRead) - OFFOF(m->i.pchNext);
            if (!av)
                break;
        }
        k = n < (long)av ? (WORD)n : av;
        if ((DWORD)OFFOF(p) + k > 0x10000UL)
            k = (WORD)(0x10000UL - OFFOF(p));
        fmemcpy(p, m->i.pchNext, k);
        m->i.pchNext += k;
        p = hadd(p, k);
        done += k;
        n -= k;
    }
    return done;
}

long API mmioWrite(WORD h, const char FAR *src, long n)
{
    struct mio FAR *m = get(h);
    PB p = (PB)src;
    long done = 0;
    if (!m)
        return -1;
    if (!buffered(m))
        return raw_rw(m, 1, p, n);
    while (n > 0) {
        WORD room = OFFOF(m->i.pchEndWrite) - OFFOF(m->i.pchNext), k;
        if (!room) {
            if (m->mem || !flush(m))
                break;
            room = OFFOF(m->i.pchEndWrite) - OFFOF(m->i.pchNext);
        }
        k = n < (long)room ? (WORD)n : room;
        if ((DWORD)OFFOF(p) + k > 0x10000UL)
            k = (WORD)(0x10000UL - OFFOF(p));
        fmemcpy(m->i.pchNext, p, k);
        m->i.pchNext += k;
        if (OFFOF(m->i.pchNext) > OFFOF(m->i.pchEndRead))
            m->i.pchEndRead = m->i.pchNext;
        m->i.dwFlags |= MMIO_DIRTY;
        p = hadd(p, k);
        done += k;
        n -= k;
    }
    return done;
}

long API mmioSeek(WORD h, long off, short how)
{
    struct mio FAR *m = get(h);
    long t, end;
    if (!m)
        return -1;
    if (m->mem) {
        end = OFFOF(m->i.pchEndRead) - OFFOF(m->i.pchBuffer);
        t = how == 1 ? lpos(m) + off : how == 2 ? end + off : off;
        if (t < 0 || t > m->i.cchBuffer)
            return -1;
        m->i.pchNext = m->i.pchBuffer + (WORD)t;
        return t;
    }
    if (!buffered(m)) {
        if (how == 1)
            off += m->i.lDiskOffset;
        t = _llseek(m->fh, off, how == 1 ? 0 : how);
        if (t == -1L)
            return -1;
        m->i.lDiskOffset = t;
        return t;
    }
    end = _llseek(m->fh, 0, 2);
    m->i.lDiskOffset = end;
    if (m->i.lBufOffset + (long)(OFFOF(m->i.pchEndRead) - OFFOF(m->i.pchBuffer)) > end)
        end = m->i.lBufOffset + (OFFOF(m->i.pchEndRead) - OFFOF(m->i.pchBuffer));
    t = how == 1 ? lpos(m) + off : how == 2 ? end + off : off;
    if (t < 0)
        return -1;
    /* im Puffer: nur den Zeiger setzen */
    if (t >= m->i.lBufOffset &&
        t <= m->i.lBufOffset + (long)(OFFOF(m->i.pchEndRead) - OFFOF(m->i.pchBuffer))) {
        m->i.pchNext = m->i.pchBuffer + (WORD)(t - m->i.lBufOffset);
        return t;
    }
    if (!flush(m))
        return -1;
    m->i.lBufOffset = t;
    return t;
}

WORD API mmioGetInfo(WORD h, struct minfo FAR *out, WORD fl)
{
    struct mio FAR *m = get(h);
    (void)fl;
    if (!m)
        return MMSYSERR_INVALHANDLE;
    fmemcpy(out, &m->i, sizeof(struct minfo));
    out->hmmio = h;
    return 0;
}
WORD API mmioSetInfo(WORD h, const struct minfo FAR *in, WORD fl)
{
    struct mio FAR *m = get(h);
    (void)fl;
    if (!m)
        return MMSYSERR_INVALHANDLE;
    if (buffered(m)) {
        m->i.pchNext = in->pchNext;
        if (OFFOF(in->pchEndRead) >= OFFOF(m->i.pchEndRead))
            m->i.pchEndRead = in->pchEndRead;
        if (OFFOF(m->i.pchNext) > OFFOF(m->i.pchEndRead))
            m->i.pchEndRead = m->i.pchNext;
        m->i.dwFlags = (m->i.dwFlags & ~MMIO_DIRTY) | (in->dwFlags & MMIO_DIRTY);
    }
    m->i.adwInfo[0] = in->adwInfo[0];
    m->i.adwInfo[1] = in->adwInfo[1];
    m->i.adwInfo[2] = in->adwInfo[2];
    return 0;
}
WORD API mmioSetBuffer(WORD h, PB buf, long cch, WORD fl)
{
    struct mio FAR *m = get(h);
    (void)fl;
    if (!m)
        return MMSYSERR_INVALHANDLE;
    if (m->mem)
        return MMIOERR_CANNOTEXPAND;
    return set_buf(m, buf, cch);
}
WORD API mmioFlush(WORD h, WORD fl)
{
    struct mio FAR *m = get(h);
    (void)fl;
    if (!m)
        return MMSYSERR_INVALHANDLE;
    return flush(m) ? 0 : MMIOERR_CANNOTWRITE;
}
/* Puffer weiterschieben: schreiben, dann (beim Lesen) neu fuellen */
WORD API mmioAdvance(WORD h, struct minfo FAR *in, WORD fl)
{
    struct mio FAR *m = get(h);
    if (!m)
        return MMSYSERR_INVALHANDLE;
    if (!buffered(m))
        return MMIOERR_UNBUFFERED;
    if (in)
        mmioSetInfo(h, in, 0);
    if (m->mem) {
        if (fl & 1)
            return MMIOERR_CANNOTEXPAND;
    } else if (fl & 1) {                        /* MMIO_WRITE          */
        if (!flush(m))
            return MMIOERR_CANNOTWRITE;
    } else {
        if (!fill(m))
            return MMIOERR_CANNOTREAD;
    }
    if (in)
        mmioGetInfo(h, in, 0);
    return 0;
}
DWORD API mmioStringToFOURCC(LPCSTR s, WORD fl)
{
    BYTE c[4];
    WORD i;
    for (i = 0; i < 4; i++) {
        char ch = s && *s ? *s++ : ' ';
        if ((fl & MMIO_TOUPPER) && ch >= 'a' && ch <= 'z')
            ch -= 32;
        c[i] = ch;
    }
    return *(DWORD *)c;
}
DWORD API mmioInstallIOProc(DWORD fcc, DWORD proc, DWORD fl) { (void)fcc; (void)proc; (void)fl; return 0; }
long API mmioSendMessage(WORD h, WORD msg, long a, long b) { (void)h; (void)msg; (void)a; (void)b; return 0; }
WORD API mmioRename(LPCSTR a, LPCSTR b, const void FAR *i, DWORD fl)
{
    (void)i; (void)fl;
    if (!mio_init())
        return MMIOERR_OUTOFMEMORY;
    fstrncpy(H->mp, a, sizeof(H->mp));
    fstrncpy(H->nb, b, sizeof(H->nb));
    return FAPI_FAILED(fapi_sel(FAPI_MOVE, mio_sel, OFFOF(H->mp), OFFOF(H->nb), 0)) ? MMIOERR_FILENOTFOUND : 0;
}

/* ---- RIFF-Bloecke --------------------------------------------------------- */
#define ck0 (H->ck0)
WORD API mmioDescend(WORD h, struct mmck FAR *ck, const struct mmck FAR *par, WORD fl)
{
    long start, lim = 0x7FFFFFFFL;
    DWORD want = 0, wtype = 0;
    if (!get(h))
        return MMSYSERR_INVALHANDLE;
    if (par)
        lim = par->dwDataOffset + par->cksize;
    if (fl & MMIO_FINDCHUNK)
        want = ck->ckid;
    else if (fl & MMIO_FINDRIFF)
        want = FOURCC_RIFF, wtype = ck->fccType;
    else if (fl & MMIO_FINDLIST)
        want = FOURCC_LIST, wtype = ck->fccType;
    for (;;) {
        start = mmioSeek(h, 0, 1);
        if (start < 0 || start + 8 > lim || mmioRead(h, (PB)&ck0, 8) != 8)
            return MMIOERR_CHUNKNOTFOUND;
        ck0.dwDataOffset = start + 8;
        ck0.fccType = 0;
        ck0.dwFlags = 0;
        if (ck0.ckid == FOURCC_RIFF || ck0.ckid == FOURCC_LIST)
            if (mmioRead(h, (PB)&ck0.fccType, 4) != 4)
                return MMIOERR_CHUNKNOTFOUND;
        if (!want || (ck0.ckid == want && (!wtype || ck0.fccType == wtype)))
            break;
        if (mmioSeek(h, ck0.dwDataOffset + ck0.cksize + (ck0.cksize & 1), 0) < 0)
            return MMIOERR_CHUNKNOTFOUND;
    }
    fmemcpy(ck, &ck0, sizeof(ck0));
    return 0;
}
WORD API mmioAscend(WORD h, struct mmck FAR *ck, WORD fl)
{
    long p, end;
    (void)fl;
    if (!get(h))
        return MMSYSERR_INVALHANDLE;
    if (ck->dwFlags & MMIO_DIRTY) {
        p = mmioSeek(h, 0, 1);
        if (p < 0)
            return MMIOERR_CANNOTSEEK;
        if ((DWORD)(p - ck->dwDataOffset) != ck->cksize) {
            ck->cksize = p - ck->dwDataOffset;
            if (mmioSeek(h, ck->dwDataOffset - 4, 0) < 0 ||
                mmioWrite(h, (const char FAR *)&ck->cksize, 4) != 4)
                return MMIOERR_CANNOTWRITE;
        }
        ck->dwFlags &= ~MMIO_DIRTY;
        end = ck->dwDataOffset + ck->cksize;
        if (mmioSeek(h, end, 0) < 0)
            return MMIOERR_CANNOTSEEK;
        if (ck->cksize & 1) {
            H->zero = 0;
            if (mmioWrite(h, &H->zero, 1) != 1)
                return MMIOERR_CANNOTWRITE;
        }
        return 0;
    }
    end = ck->dwDataOffset + ck->cksize + (ck->cksize & 1);
    return mmioSeek(h, end, 0) < 0 ? MMIOERR_CANNOTSEEK : 0;
}
WORD API mmioCreateChunk(WORD h, struct mmck FAR *ck, WORD fl)
{
    long p;
    WORD n = 8;
    if (!get(h))
        return MMSYSERR_INVALHANDLE;
    p = mmioSeek(h, 0, 1);
    if (p < 0)
        return MMIOERR_CANNOTSEEK;
    if (fl & MMIO_CREATERIFF)
        ck->ckid = FOURCC_RIFF;
    else if (fl & MMIO_CREATELIST)
        ck->ckid = FOURCC_LIST;
    if (fl & (MMIO_CREATERIFF | MMIO_CREATELIST))
        n = 12;
    ck->dwDataOffset = p + 8;
    ck->dwFlags = MMIO_DIRTY;
    if (mmioWrite(h, (const char FAR *)ck, n) != n)
        return MMIOERR_CANNOTWRITE;
    return 0;
}

void mmio_task_end(int t)
{
    WORD k;
    if (!mio_sel)
        return;
    for (k = 0; k < NMIO; k++)
        if (MIO(k)->used && MIO(k)->task == t)
            mmioClose(HBASE + k, 0);
}
