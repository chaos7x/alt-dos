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
#define DosExit()           F2(FAPI_EXIT, 0, 0)
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
#define FAPI_FAILED(r)      ((unsigned short)(r) >= FAPI_ERR_FIRST)
#define KbdCharIn()         F2(FAPI_KBDCHARIN, 0, 0)
#define KbdPeek()           F2(FAPI_KBDPEEK, 0, 0)
#define KbdSetLayout(p)     F2(FAPI_KBDSETLAYOUT, (p), 0)
#define KbdGetLayout(p)     F2(FAPI_KBDGETLAYOUT, (p), 0)
#define VioCls()            F2(FAPI_VIOCLS, 0, 0)
#define VioWrtTTY(s, n)     F2(FAPI_VIOWRTTTY, (s), (n))
#define VioWrtStr(r, c, s)  F2(FAPI_VIOWRTSTR, (s), ((r) << 8) | (c))

/* Zeichenkette mit Nullbyte ausgeben */
void puts(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    VioWrtTTY(s, n);
}

#endif
