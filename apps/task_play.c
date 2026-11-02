/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PLAY.EXE - WAV- und VOC-Dateien abspielen (ueber FAPI_AUDIO, z. B. SB16.SYS)
 *
 *   PLAY [/Q] Datei...
 *
 * WAV: unkomprimiertes PCM, 8 Bit (ohne Vorzeichen) oder 16 Bit (mit),
 * mono oder stereo. VOC (Creative): Bloecke 1 (8 Bit, Zeitkonstante),
 * 2 (Fortsetzung) und 9 (Rate, Bits, Kanaele). Esc bricht ab, /Q
 * zeigt nichts an. Zu hoeren ist das Programm der Vordergrund-Sitzung;
 * im Hintergrund spielt PLAY weiter, solange vorn niemand Ton hat.
 */
#include "fapi.h"
#include "util.h"

/* 32 / 16 Bit -> 16 Bit (Quotient muss passen), ohne Laufzeitbibliothek */
unsigned div32(unsigned long a, unsigned b);
#pragma aux div32 = "div bx" __parm [__dx __ax] [__bx] __value [__ax] __modify [__dx];

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN], info[64];
static unsigned char buf[4096];
static struct fapi_audfmt fmt;
static int quiet, opened, stop;
static unsigned h;
static unsigned long zero;

static void err(const char *a, const char *b)
{
    puts("PLAY: ");
    puts(a);
    puts(b);
    puts("\r\n");
}

static int rd(void *p, unsigned n)
{
    return DosRead(h, p, n) == n;
}

static unsigned long le32(const unsigned char *p)
{
    return (p[0] | ((unsigned)p[1] << 8)) |
           ((unsigned long)(p[2] | ((unsigned)p[3] << 8)) << 16);
}

static void skipn(unsigned long n)
{
    unsigned k;
    while (n) {
        k = n > sizeof(buf) ? sizeof(buf) : (unsigned)n;
        if (DosRead(h, buf, k) != k)
            return;
        n -= k;
    }
}

static int open_fmt(unsigned rate, unsigned ch, unsigned bits)
{
    unsigned r;
    if (opened && fmt.rate == rate && fmt.channels == ch && fmt.bits == bits)
        return 0;
    if (opened) {
        AudDrain();
        AudClose();
        opened = 0;
    }
    fmt.rate = rate;
    fmt.channels = (unsigned char)ch;
    fmt.bits = (unsigned char)bits;
    r = AudOpen(&fmt);
    if (FAPI_FAILED(r)) {
        err(r == FAPI_ERR_INUSE ? "Audioger\x84t belegt" :
            "Format nicht m\x94glich - ", "");
        return -1;
    }
    opened = 1;
    if (!quiet) {
        puts("  ");
        put_num(rate, 0, 1);
        puts(" Hz, ");
        put_num(bits, 0, 0);
        puts(ch == 2 ? " Bit, stereo" : " Bit, mono");
        puts("\r\n");
    }
    return 0;
}

/* n Bytes aus der Datei abspielen */
static void play(unsigned long n)
{
    unsigned k;
    while (n && !stop) {
        k = n > sizeof(buf) ? sizeof(buf) : (unsigned)n;
        k = DosRead(h, buf, k);
        if (FAPI_FAILED(k) || k == 0)
            break;
        AudWrite(buf, k);
        n -= k;
        if ((KbdPeek() & 0xFF) == 0x1B) {   /* nur Esc: andere Tasten gehoeren
                                               z. B. COMMAND (DETACH PLAY) */
            KbdCharIn();
            stop = 1;
        }
    }
}

static void wav(void)
{
    unsigned long sz;
    unsigned ch = 0, rate = 0, bits = 0, tag = 0;
    if (!rd(buf, 12) || buf[8] != 'W' || buf[9] != 'A' || buf[10] != 'V' || buf[11] != 'E') {
        err("keine WAV-Datei - ", name);
        return;
    }
    while (!stop && rd(buf, 8)) {
        sz = le32(buf + 4);
        if (buf[0] == 'f' && buf[1] == 'm' && buf[2] == 't' && buf[3] == ' ') {
            if (sz < 16 || !rd(buf, 16))
                break;
            tag = buf[0] | (buf[1] << 8);
            ch = buf[2];
            rate = le32(buf + 4) > 0xFFFFUL ? 0 : (unsigned)le32(buf + 4);
            bits = buf[14];
            skipn(sz - 16 + (sz & 1));
        } else if (buf[0] == 'd' && buf[1] == 'a' && buf[2] == 't' && buf[3] == 'a') {
            if (tag != 1) {
                err("nur unkomprimiertes PCM - ", name);
                return;
            }
            if (open_fmt(rate, ch, bits))
                return;
            play(sz);
            return;
        } else
            skipn(sz + (sz & 1));
    }
}

static void voc(void)
{
    unsigned off, len_lo;
    unsigned long n;
    unsigned char t;
    if (!rd(buf + 20, 6))
        return;
    off = buf[20] | (buf[21] << 8);
    skipn(off > 26 ? off - 26 : 0);
    while (!stop && rd(&t, 1) && t != 0) {
        if (!rd(buf, 3))
            break;
        len_lo = buf[0] | (buf[1] << 8);
        n = len_lo | ((unsigned long)buf[2] << 16);
        if (t == 1) {                   /* Zeitkonstante, Packart        */
            if (!rd(buf, 2) || n < 2)
                break;
            if (buf[1] != 0) {
                err("gepackte VOC-Daten werden nicht unterst\x81tzt", "");
                return;
            }
            if (open_fmt(div32(1000000UL, 256 - buf[0]), 1, 8))
                return;
            play(n - 2);
        } else if (t == 2 && opened) {  /* Fortsetzung                   */
            play(n);
        } else if (t == 9) {            /* Rate, Bits, Kanaele, Codec    */
            if (!rd(buf, 12) || n < 12)
                break;
            if ((buf[6] | (buf[7] << 8)) != 0 && (buf[6] | (buf[7] << 8)) != 4) {
                err("VOC-Codec wird nicht unterst\x81tzt", "");
                return;
            }
            if (open_fmt((unsigned)le32(buf), buf[5], buf[4]))
                return;
            play(n - 12);
        } else
            skipn(n);
    }
}

void main(void)
{
    char *a;
    unsigned files = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Spielt WAV- und VOC-Dateien ab.\r\n\r\n"
                 "PLAY [/Q] Datei...\r\n\r\n"
                 "  /Q  keine Anzeige      Esc bricht ab.\r\n"
                 "Lautst\x84rke mit MIXER, Treiber: DEVICE=SB16.SYS\r\n");
            return;
        }
        if (a[0] == '/' && up(a[1]) == 'Q')
            quiet = 1;
    }
    if (FAPI_FAILED(AudInfo(info))) {
        err("keine Soundkarte (DEVICE=SB16.SYS in CONFIG.SYS)", "");
        DosExit(1);
    }
    for (a = skip(args); *a && !stop; a = next_word(a)) {
        if (a[0] == '/')
            continue;
        word(a, name, sizeof(name));
        files++;
        h = DosOpen(name);
        if (FAPI_FAILED(h)) {
            err("Datei nicht gefunden - ", name);
            continue;
        }
        if (!quiet) {
            puts(name);
            puts(" (");
            puts(info);
            puts(")\r\n");
        }
        if (!rd(buf, 20))
            ;
        else if (buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F') {
            DosSeek(h, &zero);
            wav();
        } else if (buf[0] == 'C' && buf[1] == 'r' && buf[9] == 'V')
            voc();
        else
            err("weder WAV noch VOC - ", name);
        DosClose(h);
    }
    if (opened) {
        if (!stop)
            AudDrain();
        AudClose();
    }
    if (!files)
        err("Datei fehlt (PLAY /? zeigt die Hilfe)", "");
}
