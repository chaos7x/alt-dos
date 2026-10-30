/*
 * DOS 5.0 - Tastaturtreiber (IRQ 1), deutsche Belegung
 *
 * Der Tastaturcontroller liefert Scancodes (Satz 1). Hier werden daraus
 * Zeichen in Codepage 437. Zwei Tastenkombinationen behandelt der Kernel
 * selbst: Alt+Esc wechselt die Sitzung, Strg+C bricht das Programm im
 * Vordergrund ab.
 */
#include "kern.h"

volatile unsigned char kbd_switch_req, kbd_break_req;

#define NBUF 32
static unsigned short buf[NBUF];
static unsigned char  head, tail;
static unsigned char  shift, ctrl, alt, altgr, caps, e0;

/* Scancode 0x00-0x39 -> Zeichen; 0 = kein Zeichen */
static const unsigned char norm[0x3A] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0', 0xE1, '\'', 8, 9,
    'q','w','e','r','t','z','u','i','o','p', 0x81, '+', 13, 0,
    'a','s','d','f','g','h','j','k','l', 0x94, 0x84, '^', 0, '#',
    'y','x','c','v','b','n','m', ',', '.', '-', 0, '*', 0, ' '
};
static const unsigned char shft[0x3A] = {
    0, 27, '!','"', 0x15,'$','%','&','/','(',')','=', '?', '`', 8, 9,
    'Q','W','E','R','T','Z','U','I','O','P', 0x9A, '*', 13, 0,
    'A','S','D','F','G','H','J','K','L', 0x99, 0x8E, 0xF8, 0, '\'',
    'Y','X','C','V','B','N','M', ';', ':', '_', 0, '*', 0, ' '
};

static void put(unsigned short k)
{
    unsigned char n = (unsigned char)((head + 1) % NBUF);
    if (n != tail) {                /* voll: Taste verwerfen            */
        buf[head] = k;
        head = n;
    }
}

static unsigned char altgr_char(unsigned sc)
{
    switch (sc) {
    case 0x03: return 0xFD;         /* AltGr+2  -> hoch 2               */
    case 0x08: return '{';
    case 0x09: return '[';
    case 0x0A: return ']';
    case 0x0B: return '}';
    case 0x0C: return '\\';         /* AltGr+sz                         */
    case 0x10: return '@';          /* AltGr+Q                          */
    case 0x1B: return '~';          /* AltGr++                          */
    case 0x56: return '|';          /* AltGr+<                          */
    }
    return 0;
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

    if (altgr) {
        c = altgr_char(code);
    } else if (code == 0x56) {      /* Taste < > neben links Shift      */
        c = shift ? '>' : '<';
    } else if (code < 0x3A) {
        base = norm[code];
        c = shift ? shft[code] : base;
        if (caps && base >= 'a' && base <= 'z')
            c = shift ? base : (unsigned char)(base - 32);
        if (ctrl && base >= 'a' && base <= 'z')
            c = (unsigned char)(base - 'a' + 1);
    } else {
        c = 0;
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
    kbd_flush();
}
