/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.1 - Auslagerung Teil 2, Schritt 1: Codesegmente verwerfen
 * (KTEXT2), wie OS/2 1.x "discardable" Segmente.
 *
 * Der Code eines FX-Programms (Version 2: eigenes Codesegment, LDT-Eintrag
 * 0, unter Instanzen geteilt) steht unveraendert in seiner EXE. Wird der
 * Speicher knapp (mem_get, nach Cache und geparkten Boxen), gibt
 * code_drop den Code eines Programms frei, das gerade nicht laeuft und
 * dessen Codesegment seit der letzten Runde nicht benutzt wurde (Accessed-
 * Bit im Deskriptor, Uhr-Verfahren). Der Deskriptor wird "nicht
 * vorhanden"; laedt die CPU den Selektor wieder (meist beim IRET/RETF
 * zurueck ins Programm: Takt, FAPI-Ruecksprung, oder beim Taskwechsel),
 * kommt #NP (Vektor 11, Fehlercode = Selektor) - code_np laedt den Code
 * aus der EXE neu (keine Schreibzugriffe) und der Befehl laeuft noch
 * einmal. Beim Laden gemerkte Pruefsumme schuetzt vor einer inzwischen
 * ueberschriebenen EXE: passt sie nicht, endet das Programm mit Meldung.
 * Pfade fuer das Nachladen in einem Kernelsegment (cs_sel), angelegt beim
 * ersten Programm.
 */
#include "kern.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "../include/fx.h"
#include "k2.h"
#include "task.h"

void __far kf_klock_enter(void);            /* kernel.c                   */
void __far kf_klock_leave(void);
void __far kf_vio_kputs(int s, const char *str);
extern int current;

#define CS_ENT  (IFS_PATHLEN + 3)       /* Laufwerk, Pruefsumme, Pfad      */
static unsigned cs_sel;
#define CSE(t)  ((unsigned char __far *)MK_FP(cs_sel, (t) * CS_ENT))

/* Summe der Worte (Code ist ein Vielfaches von 16 Byte) */
unsigned __far code_sum(unsigned sel, unsigned n)
{
    const unsigned __far *p = MK_FP(sel, 0);
    unsigned s = 0;
    for (n >>= 1; n; n--)
        s += *p++;
    return s;
}

/* beim Laden: Laufwerk, Pfad und Pruefsumme des Codes von Task t */
void __far code_note(int t, int drive, const char *path, unsigned sum)
{
    unsigned char __far *e;
    unsigned i;
    if (!cs_sel && !(cs_sel = kseg_alloc((unsigned long)NTASK * CS_ENT)))
        return;
    e = CSE(t);
    e[0] = (unsigned char)drive;
    e[1] = (unsigned char)sum;
    e[2] = (unsigned char)(sum >> 8);
    for (i = 0; i < IFS_PATHLEN - 1 && path[i]; i++)
        e[3 + i] = path[i];
    e[3 + i] = 0;
}

static int sharer(int k, int t)         /* teilt k den Code von t?       */
{
    return task[k].state != T_FREE && (task[k].code_unit ? task[k].code_unit == task[t].code_unit
                                                         : 0);
}

/* einen Code verwerfen -> 1 */
int __far code_drop(void)
{
    int t, k, pass, busy;
    pg_t u;

    if (!cs_sel)
        return 0;
    for (pass = 0; pass < 2; pass++)
        for (t = 0; t < NTASK; t++) {
            if (task[t].state == T_FREE || !task[t].code_unit || task[t].v86 ||
                CSE(t)[3] == 0)
                continue;
            busy = 0;
            for (k = 0; k < NTASK; k++)
                if (sharer(k, t) && (k == current || CSE(k)[3] == 0 ||
                                     (LDT(k, 0)->access & 1)))
                    busy = 1;
            if (busy) {                 /* zweite Chance: A-Bits weg     */
                for (k = 0; k < NTASK; k++)
                    if (sharer(k, t) && k != current)
                        LDT(k, 0)->access &= (unsigned char)~1;
                continue;
            }
            u = task[t].code_unit;
            for (k = 0; k < NTASK; k++)
                if (k != t && sharer(k, t)) {
                    task[k].code_unit = 0;
                    task[k].code_gone = 1;
                    LDT(k, 0)->access &= (unsigned char)~0x80;
                }
            task[t].code_unit = 0;
            task[t].code_gone = 1;
            LDT(t, 0)->access &= (unsigned char)~0x80;
            mem_free(u, task[t].code_cnt);
            image_units -= task[t].code_cnt;
            return 1;
        }
    return 0;
}

static void desc_base_set(struct desc __far *d, unsigned long b)
{
    if (k_alias)
        b += 0x80000000UL;
    d->base_lo = (unsigned short)b;
    d->base_hi = (unsigned char)(b >> 16);
    d->rsv = (unsigned short)((d->rsv & 0x00FF) | ((unsigned)(b >> 24) << 8));
    d->access |= 0x80;
}

/* Code von t aus der EXE neu laden; 0 = ok */
static int code_load(int t)
{
    struct ifs_file f;                  /* Stapel: DGROUP ist voll       */
    struct fx_header h;
    char p[IFS_PATHLEN];
    unsigned char __far *e = CSE(t);
    unsigned i, n = task[t].code_cnt, fx, sum, op = 0;
    unsigned short w[8];
    pg_t u;
    int k, r = -1;

    for (k = 0; k < NTASK; k++)         /* anderer Task hat ihn schon    */
        if (k != t && task[k].state != T_FREE && task[k].code_unit &&
            task[k].code_key == task[t].code_key && task[k].code_fsize == task[t].code_fsize) {
            u = task[k].code_unit;
            goto share;
        }
    for (i = 0; i < IFS_PATHLEN - 1 && e[3 + i]; i++)
        p[i] = (char)e[3 + i];
    p[i] = 0;
    while (!(u = mem_alloc(n)) && (code_drop() || swap_out(n + 16)))
        ;
    if (!u)
        return -1;
    win_save(w);
    if (ifs_open(e[0], p, &f) || (op = 1, f.size != task[t].code_fsize) ||
        ifs_read(&f, 0x3C, KDATA, (unsigned)&fx, 2) != 2 ||
        ifs_read(&f, fx, KDATA, (unsigned)&h, sizeof(h)) != (int)sizeof(h))
        goto out;
    scratch_map(PG_LIN(u), n * MEM_PAGE - 1);
    if (h.code_size == 0 || PAGES(h.code_size) != n ||
        ifs_read(&f, h.image_off, SCRATCH, 0, h.code_size) != (int)h.code_size)
        goto out;
    sum = code_sum(SCRATCH, h.code_size);
    if (sum != (e[1] | ((unsigned)e[2] << 8)))
        goto out;
    r = 0;
out:
    if (op)
        ifs_close(&f);
    win_restore(w);
    if (r) {
        mem_free(u, n);
        return r;
    }
    image_units += n;
share:
    for (k = 0; k < NTASK; k++)
        if (task[k].state != T_FREE && task[k].code_gone &&
            task[k].code_key == task[t].code_key && task[k].code_fsize == task[t].code_fsize) {
            task[k].code_unit = u;
            task[k].code_gone = 0;
            desc_base_set(LDT(k, 0), PG_LIN(u));
        }
    return 0;
}

/* #NP (Vektor 11) mit Fehlercode err: verworfener Code des laufenden
   Tasks? -> 1 nachgeladen (Befehl wiederholen), 0 nicht zustaendig */
int __far code_np(unsigned err)
{
    int t = current, r;
    if (t < 0 || !task[t].code_gone || (err & 0xFFFC) != 4 || !cs_sel)
        return 0;
    kf_klock_enter();
    r = task[t].code_gone ? code_load(t) : 0;   /* vielleicht schon da   */
    kf_klock_leave();
    if (r)
        kf_vio_kputs(task[t].session, "\n\xFE Programmcode nicht nachladbar "
                                      "(EXE ge\x84ndert oder fehlt)\n");
    return r == 0;
}
