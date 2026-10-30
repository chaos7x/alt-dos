/*
 * FAPI fuer Anwendungen - der einzige Weg aus Ring 3 nach unten.
 */
#ifndef FAPI_H
#define FAPI_H

#include "../include/fapi_abi.h"

unsigned short fapi_call(unsigned short fn, unsigned short a1, unsigned short a2);
#pragma aux fapi_call = \
    0x9A 0x00 0x00 0x23 0x00    /* call far 0023:0000 (FAPI-Gate) */ \
    __parm [__ax] [__bx] [__cx] \
    __value [__ax] \
    __modify [__ax];

#define DosGetVersion()     fapi_call(FAPI_GETVERSION, 0, 0)
#define DosYield()          fapi_call(FAPI_YIELD, 0, 0)
#define DosSleep(ms)        fapi_call(FAPI_SLEEP, (ms), 0)
#define DosExit()           fapi_call(FAPI_EXIT, 0, 0)
#define DosMemInfo(p)       fapi_call(FAPI_MEMINFO, (unsigned short)(p), 0)
#define VioWrtStr(r, c, s)  fapi_call(FAPI_VIOWRTSTR, (unsigned short)(s), \
                                      (unsigned short)(((r) << 8) | (c)))

#endif
