/*
 * FAPI fuer Anwendungen - der einzige Weg aus Ring 3 nach unten.
 */
#ifndef FAPI_H
#define FAPI_H

#include "../include/fapi_abi.h"

unsigned short fapi_call(unsigned short fn, unsigned short a1,
                         unsigned short a2, unsigned short a3);
#pragma aux fapi_call = \
    0x9A 0x00 0x00 0x23 0x00    /* call far 0023:0000 (FAPI-Gate) */ \
    __parm [__ax] [__bx] [__cx] [__dx] \
    __value [__ax] \
    __modify [__ax];

#define F2(fn, a, b)        fapi_call((fn), (unsigned short)(a), (unsigned short)(b), 0)

#define DosGetVersion()     F2(FAPI_GETVERSION, 0, 0)
#define DosYield()          F2(FAPI_YIELD, 0, 0)
#define DosSleep(ms)        F2(FAPI_SLEEP, (ms), 0)
#define DosExit(rc)         F2(FAPI_EXIT, (rc), 0)        /* ERRORLEVEL */
#define DosQChildRC()       F2(FAPI_QCHILDRC, 0, 0)
#define DosAllocSeg(size)   F2(FAPI_ALLOCSEG, (size), 0)
#define DosFreeSeg(sel)     F2(FAPI_FREESEG, (sel), 0)
#define DosMemInfo(p)       F2(FAPI_MEMINFO, (p), 0)
#define DosExecPgm(cmd, m)  F2(FAPI_EXECPGM, (cmd), (m))
#define DosGetArgs(buf)     F2(FAPI_GETARGS, (buf), 0)
#define DosOpen(name)       F2(FAPI_OPEN, (name), 0)
#define DosRead(h, buf, n)  fapi_call(FAPI_READ, (h), (unsigned short)(buf), (n))
#define DosClose(h)         F2(FAPI_CLOSE, (h), 0)
#define DosFindEntry(e, n, dir) fapi_call(FAPI_FINDENTRY, (unsigned short)(e), (n), (unsigned short)(dir))
#define DosChDir(p)         F2(FAPI_CHDIR, (p), 0)
#define DosQCurDir(buf)     F2(FAPI_QCURDIR, (buf), 0)
#define DosSelectDisk(d)    F2(FAPI_SELECTDISK, (d), 0)
#define DosQCurDisk()       F2(FAPI_QCURDISK, 0, 0)
#define DosCreate(name)     F2(FAPI_CREATE, (name), 0)
#define DosAppend(name)     F2(FAPI_CREATE, (name), 1)          /* >> */
#define DosDup2(h, std)     F2(FAPI_DUP2, (h), (std))
#define DosDup(h)           F2(FAPI_DUP, (h), 0)
#define DosPipe(hh)         F2(FAPI_PIPE, (hh), 0)
#define DosQHandType(h)     F2(FAPI_QHANDTYPE, (h), 0)
#define DosWrite(h, buf, n) fapi_call(FAPI_WRITE, (h), (unsigned short)(buf), (n))
#define DosDelete(name)     F2(FAPI_DELETE, (name), 0)
#define DosMkDir(p)         F2(FAPI_MKDIR, (p), 0)
#define DosRmDir(p)         F2(FAPI_RMDIR, (p), 0)
#define DosMove(from, to)   F2(FAPI_MOVE, (from), (to))
#define DosShutdown(restart) F2(FAPI_SHUTDOWN, (restart), 0)
#define DosGetEnv(name, buf, n)  fapi_call(FAPI_GETENV, (unsigned short)(name), (unsigned short)(buf), (n))
#define DosSetEnv(entry)         F2(FAPI_SETENV, (entry), 0)
#define DosEnvEntry(i, buf, n)   fapi_call(FAPI_ENVENTRY, (i), (unsigned short)(buf), (n))
#define DosGetDateTime(p)        F2(FAPI_GETDATETIME, (p), 0)
#define DosFreeSpace(d, p)  F2(FAPI_FREESPACE, (d), (p))   /* freie Bytes */
#define DosOpenDrive(d, mode)  F2(FAPI_OPENDRIVE, (d), (mode))  /* DRV_OPEN_* */
#define DosSeek(h, posp)    F2(FAPI_SEEK, (h), (posp))      /* Zeiger auf unsigned long */
#define DosQFSInfo(d, fi)   F2(FAPI_QFSINFO, (d), (fi))     /* struct fapi_fsinfo */
#define DosSetFInfo(p, fi)  F2(FAPI_SETFINFO, (p), (fi))    /* struct fapi_finfo */
#define DosSetDateTime(p)   F2(FAPI_SETDATETIME, (p), 0)    /* Datum, Uhrzeit (FAT) */
#define DosLabel(d, out, nl) fapi_call(FAPI_LABEL, (d), (unsigned short)(out), (unsigned short)(nl))
#define FAPI_FAILED(r)      ((unsigned short)(r) >= FAPI_ERR_FIRST)
#define KbdCharIn()         F2(FAPI_KBDCHARIN, 0, 0)
#define KbdPeek()           F2(FAPI_KBDPEEK, 0, 0)
#define KbdSetLayout(p)     F2(FAPI_KBDSETLAYOUT, (p), 0)
#define KbdGetLayout(p)     F2(FAPI_KBDGETLAYOUT, (p), 0)
#define VioCls()            F2(FAPI_VIOCLS, 0, 0)
#define VioWrtTTY(s, n)     F2(FAPI_VIOWRTTTY, (s), (n))
#define VioWrtStr(r, c, s)  F2(FAPI_VIOWRTSTR, (s), ((r) << 8) | (c))
#define VioSetMode(full)    F2(FAPI_VIOSETMODE, (full), 0)
#define VioSetCurPos(r, c)  F2(FAPI_VIOSETCURPOS, ((r) << 8) | (c), 0)
#define VioSetLines(n)      F2(FAPI_VIOSETLINES, (n), 0)
#define VioGetRows()        F2(FAPI_VIOGETROWS, 0, 0)
#define DosShutdownHold()   F2(FAPI_SHUTDOWNHOLD, 0, 0)
#define KbdShift()          F2(FAPI_KBDSHIFT, 0, 0)
#define MouOpen()           F2(FAPI_MOUOPEN, 0, 0)
#define MouReadEvent(e, w)  F2(FAPI_MOUREADEVENT, (unsigned short)(e), (w))
#define MouShowPtr(on)      F2(FAPI_MOUSHOWPTR, (on), 0)
#define DosClipPut(sel, off, n) fapi_call(FAPI_CLIPPUT, (sel), (unsigned short)(off), (n))
#define DosClipGet(sel, off, n) fapi_call(FAPI_CLIPGET, (sel), (unsigned short)(off), (n))
#define VioGetBuf()         F2(FAPI_VIOGETBUF, 0, 0)
#define VioShowBuf(a, b)    F2(FAPI_VIOSHOWBUF, (a), (b))
#define VioSetGfx(m)        F2(FAPI_VIOSETGFX, (m), 0)
#define VioGetPhysBuf()     F2(FAPI_VIOGETPHYSBUF, 0, 0)
#define VioSetPal(f, n, p)  fapi_call(FAPI_VIOSETPAL, (f), (n), (unsigned short)(p))
#define VioSetPlane(p)      F2(FAPI_VIOSETPLANE, (p), 0)
#define VioWrtStrAtt(r, c, s, a) fapi_call(FAPI_VIOWRTSTRATT, (unsigned short)(s), ((r) << 8) | (c), (a))

#ifndef FAPI_NO_IMPL                    /* util.c: nur Deklarationen  */
/* Signale: Der Kernel springt in sig_tramp (AX = Signal), die ruft den
   Handler und meldet sich mit DosSigReturn zurueck. */
typedef void (*sig_fn)(unsigned sig);
sig_fn sig_user;
void sig_tramp(unsigned sig)
{
    sig_user(sig);
    fapi_call(FAPI_SIGRETURN, 0, 0, 0);
}
#define DosSetSigHandler(fn, mask) \
    (sig_user = (fn), F2(FAPI_SETSIGHANDLER, (unsigned short)sig_tramp, (mask)))

/* Zeichenkette mit Nullbyte auf die Standardausgabe (Handle 1: Konsole,
   Datei oder Pipe - je nach Umleitung) */
void puts(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    fapi_call(FAPI_WRITE, STDOUT, (unsigned short)s, n);
}
#else
void puts(const char *s);
#endif

#endif
