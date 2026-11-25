/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - CONFIG.SYS beim Start: F5/F8, BASEDEV=, ISA-PnP (KTEXT4).
 *
 * Aus kernel.c herausgeloest (erstes Codesegment voll). Was hier steht,
 * laeuft nur einmal beim Start.
 *
 * F5 wie MS-DOS 6 (docs/NOTSTART.md): Notstart ohne CONFIG.SYS und
 * AUTOEXEC.CMD - nur die BASEDEV=-Zeilen (Treiber fuer das Startlaufwerk)
 * gelten, kein ISA-PnP, keine Auslagerungsdatei, CMD.EXE /P statt
 * PROTSHELL=, NOTSTART=1 in der Systemumgebung (CMD laesst dann die
 * AUTOEXEC.CMD aus, die Box liest DOSBOX.INI, ihre CONFIG.SYS und die
 * AUTOEXEC.BAT nicht).
 *
 * BASEDEV= wie OS/2: Treiber, die das System zum Starten braucht. Sie
 * kommen in einem ersten Durchgang vor allen anderen Zeilen dran, im
 * Notstart nur sie.
 */
#include "kern.h"
#include "mem.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "task.h"
#include "k2.h"                         /* KTEXT4, k2.h zuletzt           */
#pragma code_seg("ITEXT", "CODE")   /* ganze Datei nur beim Start (INIT-SEGMENT.md) */
/* Texte des Startcodes liegen in IDATA (nach dem Start frei): in
   kleinen Stuecken ueber den Stapel ausgeben (Startstapel knapp) */
static const char __based(__segname("IDATA")) kc_1[] = "\xFE F5: Notstart, F8: Startdateien schrittweise";
static const char __based(__segname("IDATA")) kc_2[] = " [J,N]? ";
static const char __based(__segname("IDATA")) kc_3[] = "\xFE ISA-PnP: ";
static const char __based(__segname("IDATA")) kc_4[] = "\n";
static void kc_i(int s, const char __far *t)
{
    char b[33];
    unsigned i;
    while (*t) {
        for (i = 0; i < 32 && t[i]; i++)
            b[i] = t[i];
        b[i] = 0;
        vio_kputs(s, b);
        t += i;
    }
}


int __far kf_kbd_get(void);
int __far kf_env_set(const char *e);
extern unsigned char boot_f5;
extern int boot_session;
extern char protshell[FAPI_ARGLEN];
extern unsigned cfg_sel;

void k4_hlt(void);
#pragma aux k4_hlt = "sti" "hlt";

static char upc(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

/* Eine Sekunde Zeit fuer F5/F8 (wie "MS-DOS wird gestartet ..."); vor
   dem Kernel getippt hat start.asm schon gemerkt */
void __far cfg_wait(void)
{
    unsigned long t = get_ticks() + 100;
    int k;
    kc_i(boot_session, kc_1);
    while (!boot_f5 && !boot_f8 && get_ticks() < t)
        if ((k = kf_kbd_get()) == 0x4200)
            boot_f8 = 1;
        else if (k == 0x3F00)
            boot_f5 = 1;
        else
            k4_hlt();
    vio_kputs(boot_session, boot_f5 ? "\n\xFE F5: Notstart - ohne CONFIG.SYS und "
              "AUTOEXEC.CMD (nur BASEDEV=)\n" : boot_f8 ? " - F8\n" : "\n");
    if (boot_f5)
        kf_env_set("NOTSTART=1");
}

/* F8 (wie MS-DOS 6): jede Zeile bestaetigen. J/Eingabe = ausfuehren,
   N = uebergehen, Esc = Rest ohne Fragen. boot_f8: 0 aus, 1 fragen,
   2 fragen vorbei (Esc), CMD bekommt /STEP */
static int cfg_ask(const char *l)
{
    int k;
    vio_kputs(boot_session, l);
    kc_i(boot_session, kc_2);
    do {
        while ((k = kf_kbd_get()) < 0)
            k4_hlt();
        k &= 0xFF;
    } while (k != 'j' && k != 'J' && k != 'y' && k != 'Y' && k != 'n' &&
             k != 'N' && k != '\r' && k != 27);
    if (k == 27)
        boot_f8 = 2;
    k = k == 'n' || k == 'N';
    vio_kputs(boot_session, k ? "N\n" : "J\n");
    return !k;
}

/* Zeile in diesem Durchgang uebergehen? Durchgang 0 = nur BASEDEV=,
   1 = alles andere (im Notstart gar nicht); F8 fragt hier */
int __far cfg_skip(const char *l, int pass)
{
    static const char bd[] = "BASEDEV";
    unsigned i;
    int b;
    for (i = 0; i < 7 && upc(l[i]) == bd[i]; i++)
        ;
    while (i == 7 && (l[i] == ' ' || l[i] == '\t'))
        i++;
    b = i >= 7 && l[i] == '=';
    if (b != !pass || (pass && boot_f5))
        return 1;
    return boot_f8 == 1 && !cfg_ask(l);
}

/* F8 noch an: CMD fragt auch AUTOEXEC.CMD zeilenweise (nur CMD kennt
   /STEP - eine andere PROTSHELL= bekommt es nicht) */
void __far cfg_step(void)
{
    static const char st[] = " /STEP";
    unsigned i = 0, k;
    char *p = protshell;
    while (p[i] && p[i] != ' ')
        i++;
    if (i >= 4 && upc(p[i - 1]) == 'E' && p[i - 4] == '.')
        i -= 4;
    if (boot_f8 != 1 || i < 3 || upc(p[i - 3]) != 'C' ||
        upc(p[i - 2]) != 'M' || upc(p[i - 1]) != 'D' ||
        (i > 3 && p[i - 4] != '\\' && p[i - 4] != ':'))
        return;
    while (p[i])
        i++;
    if (i + 7 < FAPI_ARGLEN)
        for (k = 0; k < sizeof(st); k++)
            p[i + k] = st[k];
}

/* ISA-PnP vor den Treibern; PNP=OFF in CONFIG.SYS (vorab gesucht, die
   Zeile darf irgendwo stehen) schaltet es ab, ebenso der Notstart */
void __far cfg_pnp(int size)
{
    char m[80];                         /* Stapel: DGROUP ist knapp      */
    const char __far *c = MK_FP(cfg_sel, 0);
    unsigned p = 0, n, i;
    if (boot_f5)
        return;
    while (size > 0 && p + 7 <= (unsigned)size) {
        if ((p == 0 || c[p - 1] == '\n') && (c[p] | 32) == 'p' &&
            (c[p + 1] | 32) == 'n' && (c[p + 2] | 32) == 'p') {
            for (i = p + 3; i < (unsigned)size && c[i] == ' '; i++)
                ;
            if (i + 2 < (unsigned)size && c[i] == '=') {
                while (++i < (unsigned)size && c[i] == ' ')
                    ;
                if (i + 1 < (unsigned)size && (c[i] | 32) == 'o' && (c[i + 1] | 32) == 'f')
                    return;
            }
        }
        p++;
    }
    n = pnp_init();
    for (i = 0; i < n; i++) {
        pnp_line(i, m);
        kc_i(boot_session, kc_3);
        vio_kputs(boot_session, m);
        kc_i(boot_session, kc_4);
    }
}
