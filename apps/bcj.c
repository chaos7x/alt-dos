/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BCJ-x86-Filter fuer 7z (7ZDEC entpacken, 7ZA packen): relative CALL/JMP
 * (E8/E9 mit 32-Bit-Abstand) werden beim Packen in absolute Ziele
 * umgerechnet. Eigene Umsetzung nach der Beschreibung im LZMA-SDK
 * (Bra86.c, gemeinfrei, nur gelesen).
 */
#include "lzma.h"

static const unsigned char m_ok[8] = { 1, 1, 1, 0, 1, 0, 0, 0 };
static const unsigned char m_bit[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
#define MSB(b) ((b) == 0 || (b) == 0xFF)

/* BCJ x86 (enc 1: packen, 0: entpacken); liefert die Zahl der fertigen Bytes (Rest < 5 bleibt
 * fuer den naechsten Aufruf). ip = Stromstelle von buf[0]. */
unsigned bcj_x86(unsigned char *buf, unsigned n, unsigned long ip, unsigned *state, int enc)
{
    unsigned pos = 0, prevpos = 0xFFFF, mask = *state & 7, lim, idx;
    unsigned char b;
    unsigned long src, dst;

    if (n < 5)
        return 0;
    ip += 5;
    lim = n - 4;
    for (;;) {
        while (pos < lim && (buf[pos] & 0xFE) != 0xE8)
            pos++;
        if (pos >= lim)
            break;
        prevpos = pos - prevpos;
        if (prevpos > 3)
            mask = 0;
        else {
            mask = (mask << (prevpos - 1)) & 7;
            if (mask) {
                b = buf[pos + 4 - m_bit[mask]];
                if (!m_ok[mask] || MSB(b)) {
                    prevpos = pos;
                    mask = ((mask << 1) & 7) | 1;
                    pos++;
                    continue;
                }
            }
        }
        prevpos = pos;
        if (MSB(buf[pos + 4])) {
            src = buf[pos + 1] | ((unsigned long)buf[pos + 2] << 8) |
                  ((unsigned long)buf[pos + 3] << 16) | ((unsigned long)buf[pos + 4] << 24);
            for (;;) {
                dst = enc ? src + (ip + pos) : src - (ip + pos);
                if (mask == 0)
                    break;
                idx = m_bit[mask];      /* 1..3: Byte 3, 2, 1 von dst     */
                b = (unsigned char)(idx == 1 ? dst >> 16 : idx == 2 ? dst >> 8 : dst);
                if (!MSB(b))
                    break;
                src = dst ^ (idx == 1 ? 0x00FFFFFFUL : idx == 2 ? 0x0000FFFFUL : 0x000000FFUL);
            }
            buf[pos + 4] = (unsigned char)~(((unsigned)(dst >> 24) & 1) - 1);
            buf[pos + 3] = (unsigned char)(dst >> 16);
            buf[pos + 2] = (unsigned char)(dst >> 8);
            buf[pos + 1] = (unsigned char)dst;
            pos += 5;
        } else {
            mask = ((mask << 1) & 7) | 1;
            pos++;
        }
    }
    prevpos = pos - prevpos;
    *state = prevpos > 3 ? 0 : (mask << (prevpos - 1)) & 7;
    return pos;
}

