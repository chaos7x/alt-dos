/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Exporttabellen der eingebauten Win16-Module (KERNEL, USER,
 * GDI) nach Ordinalzahl. Die Nummern sind die der Windows-3.1-Schnitt-
 * stelle (Importbibliotheken von Open Watcom, lib286/win/windows.lib).
 */
#include "gui.h"

#define X(o, f)  { o, (void FAR *)f }
struct ex { WORD ord; void FAR *fn; };

/* Prototypen der Exporte (API, aus krnl.c/user.c/gdi.c) */
#include "expproto.h"

void __cdecl InitTask(void);
void __cdecl Dos3Call(void);

static const struct ex kernel_t[] = {
#include "exp_k.h"
    { 0, 0 }
};
static const struct ex user_t[] = {
#include "exp_u.h"
    { 0, 0 }
};
static const struct ex gdi_t[] = {
#include "exp_g.h"
    { 0, 0 }
};

void FAR *exp_find(LPCSTR mod, WORD ord, WORD FAR *isconst)
{
    const struct ex *t;
    *isconst = 0;
    if (!fstricmp(mod, "KERNEL")) {
        if (ord == 114) { *isconst = 1; return MKFP(0, 8); }        /* __AHINCR  */
        if (ord == 113) { *isconst = 1; return MKFP(0, 3); }        /* __AHSHIFT */
        if (ord == 178) { *isconst = 1; return MKFP(0, 0x0413); }   /* __WINFLAGS */
        t = kernel_t;
    } else if (!fstricmp(mod, "USER"))
        t = user_t;
    else if (!fstricmp(mod, "GDI"))
        t = gdi_t;
    else
        return 0;
    for (; t->ord; t++)
        if (t->ord == ord)
            return t->fn;
    return 0;
}
