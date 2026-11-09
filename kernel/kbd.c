/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - Tastaturtreiber (IRQ 1)
 *
 * Der Tastaturcontroller liefert Scancodes (Satz 1). Hier werden daraus
 * Zeichen in Codepage 437. Zwei Tastenkombinationen behandelt der Kernel
 * selbst: Alt+Esc wechselt die Sitzung, Strg+C bricht das Programm im
 * Vordergrund ab, Strg+Alt+Entf startet den Rechner neu - direkt hier im
 * Interrupt, damit es auch bei einem haengenden Programm wirkt.
 *
 * Die deutsche Belegung ist fest eingebaut, andere laedt KEYB.EXE aus
 * .KBL-Dateien (KbdSetLayout).
 */
#include "kern.h"
#include "../include/fapi_abi.h"

volatile unsigned char kbd_switch_req, kbd_break_req, kbd_tm_req;

#define NBUF 32
static unsigned short buf[NBUF];
static unsigned char  head, tail;
static unsigned char  shift, ctrl, alt, altgr, caps, e0, alt_only;

/* Belegung: Kopf "KBL" 01h, Kennung, dann je 90 Zeichen ohne Umschalt,
   mit Umschalt und mit AltGr (Format siehe tools/mkkbl.py) */
#include "../build/kbd_gr.h"
#define KBL_SIZE  sizeof(kbl_gr)
#define KBL_N     0x5A
static unsigned char kbl[KBL_SIZE];
#define NORM(sc)  kbl[8 + (sc)]
#define SHFT(sc)  kbl[8 + KBL_N + (sc)]
#define ALTG(sc)  kbl[8 + 2 * KBL_N + (sc)]

static void put(unsigned short k)
{
    unsigned char n = (unsigned char)((head + 1) % NBUF);
    if (n != tail) {                /* voll: Taste verwerfen            */
        buf[head] = k;
        head = n;
    }
}

/* neue Belegung aus sel:off (KBL_SIZE Bytes), 0 = ok. sel 0 = eingebaute */
int kbd_set_layout(unsigned sel, unsigned off)
{
    const unsigned char __far *p;
    unsigned f, i;

    if (sel == 0) {
        sel = KDATA;
        off = (unsigned)kbl_gr;
    }
    p = MK_FP(sel, off);
    if (p[0] != 'K' || p[1] != 'B' || p[2] != 'L' || p[3] != 1)
        return -1;
    f = irq_off();
    for (i = 0; i < KBL_SIZE; i++)
        kbl[i] = p[i];
    irq_restore(f);
    return 0;
}

const char *kbd_layout_id(void)
{
    return (const char *)kbl + 4;
}

void kbd_irq(unsigned sc)
{
    unsigned char rel = (unsigned char)(sc & 0x80), c, base;
    unsigned code = sc & 0x7F;

    if (!(alt && code == 0x01) &&   /* Alt+Esc (Sitzungswechsel) und     */
        !(ctrl && shift && code == 0x01))   /* Strg+Umschalt+Esc (Task- */
        v86_key(sc);                /* manager) gehoeren dem Kernel, sonst
                                       DOS-Box: Port 60h */
    if (sc == 0xE0) {
        e0 = 1;
        return;
    }
    if (!rel && code != 0x38)
        alt_only = 0;               /* Alt nicht mehr allein gedrueckt  */
    if (e0) {                       /* erweiterte Tasten                */
        e0 = 0;
        if (code == 0x38) altgr = !rel;
        else if (code == 0x1D) ctrl = !rel;
        else if (code == 0x1C && !rel) put(13 | (0x1C << 8));
        else if (code == 0x53 && !rel && ctrl && (alt || altgr))
            reboot_request();               /* Strg+Alt+Entf (Entf-Taste)       */
        else if (code == 0x4F && !rel && ctrl && (alt || altgr) && v86_fgbox())
            kbd_break_req = 1;              /* Strg+Alt+Ende: DOS-Box beenden   */
        else if (!rel && code >= 0x47 && code <= 0x53)
            put(0xE0 | (code << 8));        /* graue Pfeile, Bild, Pos1, Ende,
                                               Einfg, Entf: wie das AT-BIOS  */
        return;
    }

    switch (code) {
    case 0x2A: case 0x36: shift = !rel; return;
    case 0x1D: ctrl = !rel; return;
    case 0x38:
        if (!rel && !alt)
            alt_only = 1;
        else if (rel && alt_only) {
            alt_only = 0;
            put(KEY_ALT);           /* Alt allein: Menue (EDIT)         */
        }
        alt = !rel;
        return;
    case 0x3A: if (!rel) caps = !caps; return;
    }
    if (rel)
        return;

    if (ctrl && (alt || altgr) && code == 0x53)
        reboot_request();                   /* Strg+Alt+Entf (Ziffernblock)     */
    if (ctrl && shift && !alt && !altgr && code == 0x01) {  /* Strg+Umschalt+ */
        kbd_tm_req = 1;             /* Esc: Taskmanager (wie Windows NT) */
        return;
    }
    if (alt && code == 0x01) {      /* Alt+Esc                          */
        kbd_switch_req = 1;
        return;
    }
    if (ctrl && alt && code == 0x4F && v86_fgbox()) {  /* Strg+Alt+Ende:  */
        kbd_break_req = 1;          /* haengende DOS-Box abschiessen    */
        return;
    }
    if (ctrl && code == 0x2E && !v86_fgbox()) {   /* Strg+C (in einer Box */
        kbd_break_req = 1;          /* mit DOS gehoert es dem DOS)      */
        return;
    }

    if (code >= KBL_N || (alt && !altgr)) {
        c = 0;                      /* Alt+Taste: nur Scancode (BIOS)   */
    } else if (altgr) {
        c = ALTG(code);
    } else {
        base = NORM(code);
        c = shift ? SHFT(code) : base;
        if (caps && base >= 'a' && base <= 'z')
            c = shift ? base : (unsigned char)(base - 32);
        if (ctrl && base >= 'a' && base <= 'z')
            c = (unsigned char)(base - 'a' + 1);
    }
    if (c)
        put(c | (code << 8));
    else if (code && code < 0x59)
        put(code << 8);             /* F-Tasten u. ae.: Zeichen 0, Scancode */
}

int kbd_get(void)
{
    unsigned f;
    int k = vio_paste(1);           /* Einfuegen mit der Maus zuerst    */
    if (k >= 0)
        return k;
    f = irq_off();
    if (head != tail) {
        k = buf[tail];
        tail = (unsigned char)((tail + 1) % NBUF);
    }
    irq_restore(f);
    return k;
}

unsigned kbd_shift(void)
{
    return (shift ? KBDSHIFT_SHIFT : 0) | (ctrl ? KBDSHIFT_CTRL : 0) |
           (alt ? KBDSHIFT_ALT : 0) | (altgr ? KBDSHIFT_ALTGR : 0);
}

int kbd_peek(void)
{
    unsigned f;
    int k = vio_paste(0);
    if (k >= 0)
        return k;
    f = irq_off();
    k = head != tail ? buf[tail] : -1;
    irq_restore(f);
    return k;
}

void kbd_flush(void)
{
    unsigned f = irq_off();
    head = tail = 0;
    irq_restore(f);
}

void kbd_init(void)
{
    unsigned i;
    /* Reste im Ausgabepuffer des Controllers abholen (nur lesen) */
    for (i = 0; i < 16 && (inb(0x64) & 1); i++)
        inb(0x60);
    kbd_set_layout(0, 0);           /* eingebaut: deutsch               */
    kbd_flush();
}
