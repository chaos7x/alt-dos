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
 * .KBL-Dateien (KbdSetLayout). Version 1 (CP437, drei Ebenen) und 2
 * (Unicode, vier Ebenen mit Umschalt+AltGr): der Kernel haelt die Belegung
 * als Unicode-Vorlage und leitet daraus die Zeichen der aktiven Codepage
 * ab (kbd_derive, auch nach CHCP). Beides liegt in einem Kernelsegment.
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
#define KBL_N     0x5A
#define KB_HDR    8                     /* "KBL" Version, Kennung      */
#define KB_UNI    (KB_HDR + 4 * KBL_N)  /* Vorlage: 4 Ebenen in Unicode */
#define KB_SZ     (KB_UNI + 8 * KBL_N)
unsigned kbd_sel;                       /* Kernelsegment der Belegung  */
#define KB        ((unsigned char __far *)MK_FP(kbd_sel, 0))
#define KBU       ((unsigned short __far *)MK_FP(kbd_sel, KB_UNI))
#define NORM(sc)  KB[KB_HDR + (sc)]
#define SHFT(sc)  KB[KB_HDR + KBL_N + (sc)]
#define ALTG(sc)  KB[KB_HDR + 2 * KBL_N + (sc)]
#define SALT(sc)  KB[KB_HDR + 3 * KBL_N + (sc)]

static void put(unsigned short k)
{
    unsigned char n = (unsigned char)((head + 1) % NBUF);
    if (n != tail) {                /* voll: Taste verwerfen            */
        buf[head] = k;
        head = n;
    }
}

/* Zeichen der aktiven Codepage aus der Vorlage; -> Anzahl Zeichen, die
   es dort nicht gibt (Taste liefert dann nur den Scancode) */
int kbd_derive(void)
{
    unsigned i, u, c;
    int miss = 0;
    if (!kbd_sel)
        return 0;
    for (i = 0; i < 4 * KBL_N; i++) {
        u = KBU[i];
        c = u < 0x80 ? u : cp_from_uni(u);
        if (!c && (u == 0xA7 || u == 0xB6))
            c = u == 0xA7 ? 0x15 : 0x14;    /* § und ¶ wie CP437 (unter 20h) */
        if (u && !c)
            miss++;
        KB[KB_HDR + i] = (unsigned char)c;
    }
    return miss;
}

/* neue Belegung aus sel:off, sel 0 = eingebaute; -> fehlende Zeichen
   (0 = alle da), -1 = keine Belegung */
int kbd_set_layout(unsigned sel, unsigned off)
{
    const unsigned char __far *p;
    unsigned i;

    if (sel == 0) {
        sel = KDATA;
        off = (unsigned)kbl_gr;
    }
    p = MK_FP(sel, off);
    if (p[0] != 'K' || p[1] != 'B' || p[2] != 'L' || (p[3] != 1 && p[3] != 2) ||
        (!kbd_sel && !(kbd_sel = kseg_alloc(KB_SZ))))
        return -1;
    for (i = 0; i < KB_HDR; i++)
        KB[i] = p[i];
    if (p[3] == 1)                      /* CP437, ohne Umschalt+AltGr  */
        for (i = 0; i < 4 * KBL_N; i++)
            KBU[i] = i < 3 * KBL_N ? cp437_uni(p[KB_HDR + i]) : 0;
    else
        for (i = 0; i < 4 * KBL_N; i++)
            KBU[i] = p[KB_HDR + 2 * i] | ((unsigned)p[KB_HDR + 2 * i + 1] << 8);
    return kbd_derive();
}

/* Grossbuchstabe zu einem Kleinbuchstaben (Feststelltaste) in Unicode:
   lateinisch, Latin-1, Latin Extended-A, griechisch, kyrillisch */
static unsigned up_uni(unsigned u)
{
    if ((u >= 'a' && u <= 'z') || (u >= 0xE0 && u <= 0xFE && u != 0xF7) ||
        (u >= 0x3B1 && u <= 0x3C9 && u != 0x3C2) || (u >= 0x430 && u <= 0x44F))
        return u - 32;
    if (u >= 0x450 && u <= 0x45F)
        return u - 80;
    if ((((u >= 0x101 && u <= 0x137) || (u >= 0x14B && u <= 0x177)) && (u & 1) &&
         u != 0x131) ||
        (((u >= 0x13A && u <= 0x148) || (u >= 0x17A && u <= 0x17E)) && !(u & 1)))
        return u - 1;                   /* Latin Extended-A: Paare      */
    return 0;
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

    if (!kbd_sel || code >= KBL_N || (alt && !altgr)) {
        c = 0;                      /* Alt+Taste: nur Scancode (BIOS)   */
    } else if (altgr) {                 /* Umschalt+AltGr: 4. Ebene,   */
        c = (shift != (caps && up_uni(KBU[2 * KBL_N + code]) != 0)) ?
            SALT(code) : 0;                 /* Feststelltaste wie Umschalt */
        if (!c)
            c = ALTG(code);
    } else {
        base = NORM(code);
        c = shift ? SHFT(code) : base;
        if (caps && up_uni(KBU[code]))      /* Buchstabe: Feststelltaste */
            c = shift ? base : SHFT(code);
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
