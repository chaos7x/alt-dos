/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FPUTEST.EXE - prueft, ob jeder Task seinen eigenen 287-Zustand hat:
 *
 *   FPUTEST [Startwert [Schritte]] [/W]
 *
 * /W wartet am Ende auf eine Taste (fuer START in einer eigenen Sitzung).
 * /E loest einen 287-Fehler aus (Division durch Null, Ausnahme nicht
 *    maskiert): der Kernel muss das Programm beenden (IRQ 13), das System
 *    und der 287 laufen weiter.
 *
 * Laedt den Startwert in den 287, zaehlt ihn dort (Vorgabe) 2000-mal abwechselnd um 1 hoch und
 * wieder herunter und
 * gibt nach jedem Schritt die Zeitscheibe ab (DosYield). Laufen zwei
 * Instanzen mit verschiedenen Startwerten gleichzeitig (START FPUTEST 5000),
 * sieht jede nur ihre eigenen Werte - sonst meldet sie FEHLER.
 */
#include "fapi.h"
#include "util.h"

/* 287-Befehle als Bytes (unabhaengig von den Compiler-Schaltern) */
void f_init(void);
#pragma aux f_init = 0x9B 0xDB 0xE3;            /* FINIT                 */
void f_ild(int *p);
#pragma aux f_ild = 0xDF 0x07 parm [bx];        /* FILD word [bx]        */
void f_inc(void);
#pragma aux f_inc = 0xD9 0xE8 0xDE 0xC1;        /* FLD1, FADDP st1,st    */
void f_dec(void);
#pragma aux f_dec = 0xD9 0xE8 0xDE 0xE9;        /* FLD1, FSUBP st1,st    */
void f_divzero(unsigned *cw);
#pragma aux f_divzero = 0xD9 0x2F 0xD9 0xE8 0xD9 0xEE 0xDE 0xF9 0x9B 0xD9 0xE8 0x9B parm [bx];
                        /* FLDCW [bx], FLD1, FLDZ, FDIVP, FWAIT, FLD1, FWAIT */
void f_ist(int *p);
#pragma aux f_ist = 0xDF 0x17 0x9B parm [bx];   /* FIST word [bx], FWAIT */

static char args[FAPI_ARGLEN], nb[8];
static int start, x, wait_key;
static unsigned long steps;
static unsigned cw = 0x037B;           /* alles maskiert ausser ZE */
static struct fapi_sysinfo si;

static void lnum(unsigned long v)
{
    static const unsigned long pw[10] = { 1000000000UL, 100000000UL, 10000000UL,
        1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    static char b[12];
    unsigned i, n = 0, d;
    for (i = 0; i < 10; i++) {
        for (d = 0; v >= pw[i]; d++)
            v -= pw[i];
        if (d || n || i == 9)
            b[n++] = (char)('0' + d);
    }
    b[n] = 0;
    puts(b);
}

static void pnum(int v)
{
    int i = 7;
    unsigned u = v < 0 ? -v : v;
    nb[i] = 0;
    do {
        nb[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u && i > 1);
    if (v < 0)
        nb[--i] = '-';
    puts(nb + i);
}

void main(void)
{
    char *a;
    unsigned long i;

    DosGetArgs(args);
    a = skip(args);
    while (*a >= '0' && *a <= '9')
        start = start * 10 + (*a++ - '0');
    a = skip(a);
    while (*a >= '0' && *a <= '9')
        steps = (steps << 3) + (steps << 1) + (unsigned)(*a++ - '0');
    for (a = skip(a); a[0] == '/'; a = skip(a + 2)) {
        if (a[1] == 'w' || a[1] == 'W')
            wait_key = 1;
        if (a[1] == 'e' || a[1] == 'E') {
            DosSysInfo(&si);
            if (si.fpu) {
                puts("FPUTEST: 1/0 mit unmaskierter Ausnahme ...\r\n");
                f_init();
                f_divzero(&cw);
                puts("FPUTEST: FEHLER - kein Abbruch durch den Kernel\r\n");
            }
            DosExit(1);
        }
    }
    if (!steps)
        steps = 2000;
    DosSysInfo(&si);
    if (!si.fpu) {
        puts("FPUTEST: kein Koprozessor vorhanden\r\n");
        DosExit(1);
    }
    f_init();
    f_ild(&start);
    for (i = 1; i <= steps; i++) {
        DosYield();
        if (i & 1)
            f_inc();
        else
            f_dec();
        f_ist(&x);
        if (x != start + (int)(i & 1)) {
            puts("FPUTEST ");
            pnum(start);
            puts(": FEHLER bei Schritt ");
            lnum(i);
            puts(", 287 enth\x84lt ");
            pnum(x);
            puts("\r\n");
            if (wait_key)
                KbdCharIn();
            DosExit(1);
        }
    }
    puts("FPUTEST ");
    pnum(start);
    puts(": OK, ");
    lnum(steps);
    puts(" Schritte, Ergebnis ");
    pnum(x);
    puts("\r\n");
    if (wait_key)
        KbdCharIn();
}
