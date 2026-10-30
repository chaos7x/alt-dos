/*
 * DOS 5.0 - Tastaturtreiber (IRQ 1)
 *
 * Der Tastaturcontroller liefert Scancodes (Satz 1). Hier werden daraus
 * Zeichen in Codepage 437. Zwei Tastenkombinationen behandelt der Kernel
 * selbst: Alt+Esc wechselt die Sitzung, Strg+C bricht das Programm im
 * Vordergrund ab.
 *
 * Die deutsche Belegung ist fest eingebaut, andere laedt KEYB.EXE aus
 * .KBL-Dateien (KbdSetLayout).
 */
#include "kern.h"

volatile unsigned char kbd_switch_req, kbd_break_req;

#define NBUF 32
static unsigned short buf[NBUF];
static unsigned char  head, tail;
static unsigned char  shift, ctrl, alt, altgr, caps, e0;

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

    if (sc == 0xE0) {
        e0 = 1;
        return;
    }
    if (e0) {                       /* erweiterte Tasten                */
        e0 = 0;
        if (code == 0x38) altgr = !rel;
        else if (code == 0x1D) ctrl = !rel;
        else if (code == 0x1C && !rel) put(13 | (0x1C << 8));
        return;                     /* Pfeile usw. vorerst ignoriert    */
    }

    switch (code) {
    case 0x2A: case 0x36: shift = !rel; return;
    case 0x1D: ctrl = !rel; return;
    case 0x38: alt = !rel; return;
    case 0x3A: if (!rel) caps = !caps; return;
    }
    if (rel)
        return;

    if (alt && code == 0x01) {      /* Alt+Esc                          */
        kbd_switch_req = 1;
        return;
    }
    if (ctrl && code == 0x2E) {     /* Strg+C                           */
        kbd_break_req = 1;
        return;
    }

    if (code >= KBL_N) {
        c = 0;
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
}

int kbd_get(void)
{
    unsigned f = irq_off();
    int k = -1;
    if (head != tail) {
        k = buf[tail];
        tail = (unsigned char)((tail + 1) % NBUF);
    }
    irq_restore(f);
    return k;
}

int kbd_peek(void)
{
    unsigned f = irq_off();
    int k = head != tail ? buf[tail] : -1;
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
