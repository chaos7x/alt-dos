/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Exporttabellen der eingebauten Win16-Module (KERNEL, USER,
 * GDI, KEYBOARD, COMMDLG, SHELL) nach Ordinalzahl. Die Nummern sind die der
 * Windows-3.1-Schnittstelle (gui/ordinals.txt); die Tabellen erzeugt
 * tools/mkexp.py (exp_all.h im Bauverzeichnis). Die Tabellen liegen in
 * einem eigenen fernen Datensegment - in DGROUP fehlte der Platz.
 */
#include "gui.h"

#define X(o, f)  { o, (void FAR *)f }
struct ex { WORD ord; void FAR *fn; };

/* Prototypen der Exporte (API, aus den Quelldateien der GUI) */
#include "expproto.h"

void __cdecl InitTask(void);
void __cdecl Dos3Call(void);

#include "exp_all.h"

/* eingebautes Modul? (fuer LoadLibrary/GetModuleHandle) */
int exp_builtin(LPCSTR mod)
{
    short i;
    for (i = 0; modtab[i].name; i++)
        if (!fstricmp(mod, modtab[i].name))
            return i + 1;
    return 0;
}

void FAR *exp_find(LPCSTR mod, WORD ord, WORD FAR *isconst)
{
    const struct ex __far *t;
    short i;
    *isconst = 0;
    if (!fstricmp(mod, "KERNEL")) {
        if (ord == 114) { *isconst = 1; return MKFP(0, 8); }        /* __AHINCR  */
        if (ord == 113) { *isconst = 1; return MKFP(0, 3); }        /* __AHSHIFT */
        if (ord == 178) { *isconst = 1; return MKFP(0, 0x0413); }   /* __WINFLAGS */
    }
    i = exp_builtin(mod);
    if (!i)
        return 0;
    for (t = modtab[i - 1].t; t->ord; t++)
        if (t->ord == ord)
            return t->fn;
    return 0;
}

/* Eintrag nach Modulnummer (GetProcAddress mit Modulhandle 100h + i) */
void FAR *exp_ord(WORD i, WORD ord, WORD FAR *isconst)
{
    short k;
    for (k = 0; modtab[k].name; k++)
        if (k == (short)i)
            return exp_find(modtab[k].name, ord, isconst);
    *isconst = 0;
    return 0;
}
