/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Seltenes aus kernel.c im zweiten Codesegment (KTEXT2): Text im ROM
   fuer MSD (aus kernel.c verschoben, erstes Codesegment war voll). */
#include "kern.h"
#include "../include/fapi_abi.h"
#include "k2.h"                         /* KTEXT2, k2.h zuletzt           */

/* Text im ROM fuer MSD: der laengste mit "BIOS", "(C)" oder "Copyright",
   sonst der laengste ueberwiegend aus Buchstaben/Ziffern (ohne '%') */
void __far rom_str(unsigned long lin, unsigned len, char __far *out, unsigned max)
{
    unsigned char __far *p = MK_FP(SCRATCH, 0);
    unsigned i, s = 0, n = 0, al = 0, best = 0, bs = 0, c, kw = 0, pc = 0, bkw = 0, sp = 0, ks = 0;
    scratch_map(lin, len - 1);
    for (i = 0; i <= len - 1; i++) {
        c = i < len - 1 ? p[i] : 0;
        if (c >= 0x20 && c < 0x7F) {
            if (!n)
                s = i, al = 0, kw = 0, pc = 0, sp = 0;
            n++;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                al++;
            if (c == '%')
                pc = 1;
            if (c == ' ')
                sp = 1, al++;
            if (n >= 4 && p[i - 3] == 'B' && p[i - 2] == 'I' && p[i - 1] == 'O' && c == 'S' &&
                kw < 1)
                kw = 1;                     /* "BIOS" zaehlt weniger als  */
            if (n >= 4 && ((p[i - 2] == '(' && (p[i - 1] | 0x20) == 'c' && c == ')') ||
                           (p[i - 3] == 'r' && p[i - 2] == 'i' && p[i - 1] == 'g' && c == 'h'))) {
                if (kw < 2)                 /* ab hier zeigen: davor steht */
                    ks = c == ')' ? i - 2 : (i - 7 >= s ? i - 7 : s);  /* oft Muell */
                kw = 2;                     /* "(C)" / "Copyright"        */
            }
            continue;
        }
        /* Zeichensalat aussortieren: mindestens 8 Zeichen, ein Leerzeichen,
           zu drei Vierteln Buchstaben/Ziffern/Leerzeichen */
        if (n >= 8 && sp && al * 4 >= n * 3 && !pc && (kw > bkw || (kw == bkw && n > best))) {
            best = n, bs = s, bkw = kw;
            if (kw == 2 && ks > s) {        /* "(C)"/"Copyright" voran   */
                best -= ks - s;
                bs = ks;
            }
        }
        n = 0;
    }
    if (best >= max)
        best = max - 1;
    for (i = 0; i < best; i++)
        out[i] = p[bs + i];
    out[i] = 0;
}


/* CONFIG.SYS beim Start (aus kernel.c, erstes Codesegment voll):
   F5 wie MS-DOS 6 (docs/NOTSTART.md) = Notstart, nur BASEDEV=-Zeilen,
   CMD.EXE /P ohne AUTOEXEC.CMD, NOTSTART=1 in der Systemumgebung; F8 =
   jede Zeile bestaetigen (boot_f8: 0 aus, 1 fragen, 2 fragen vorbei),
   CMD bekommt /STEP. BASEDEV= wie OS/2: Treiber fuer das Startlaufwerk,
   in einem ersten Durchgang vor allen anderen Zeilen. */
int __far kf_kbd_get(void);
int __far kf_env_set(const char *e);
void __far kf_vio_kputs(int s, const char *t);

static void k2_hlt(void);
#pragma aux k2_hlt = "sti" "hlt";

static char upc(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

/* Eine Sekunde Zeit fuer F5/F8 (wie "MS-DOS wird gestartet ..."); vor
   dem Kernel getippt hat start.asm schon gemerkt */
void __far cfg_wait(int s)
{
    unsigned long t = get_ticks() + 100;
    int k;
    kf_vio_kputs(s, "\xFE F5: Notstart, F8: Startdateien schrittweise");
    while (!boot_f5 && !boot_f8 && get_ticks() < t)
        if ((k = kf_kbd_get()) == 0x4200)
            boot_f8 = 1;
        else if (k == 0x3F00)
            boot_f5 = 1;
        else
            k2_hlt();
    kf_vio_kputs(s, boot_f5 ? "\n\xFE F5: Notstart - ohne CONFIG.SYS und "
                 "AUTOEXEC.CMD (nur BASEDEV=)\n" : boot_f8 ? " - F8\n" : "\n");
    if (boot_f5)
        kf_env_set("NOTSTART=1");
}

static int cfg_ask(const char *l, int s)
{
    int k;
    kf_vio_kputs(s, l);
    kf_vio_kputs(s, " [J,N]? ");
    do {
        while ((k = kf_kbd_get()) < 0)
            k2_hlt();
        k &= 0xFF;
    } while (k != 'j' && k != 'J' && k != 'y' && k != 'Y' && k != 'n' &&
             k != 'N' && k != '\r' && k != 27);
    if (k == 27)
        boot_f8 = 2;
    k = k == 'n' || k == 'N';
    kf_vio_kputs(s, k ? "N\n" : "J\n");
    return !k;
}

/* Zeile in diesem Durchgang uebergehen? Durchgang 0 = nur BASEDEV=,
   1 = alles andere (im Notstart gar nicht); F8 fragt hier */
int __far cfg_skip(const char *l, int pass, int s)
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
    return boot_f8 == 1 && !cfg_ask(l, s);
}

/* F8 noch an: CMD fragt auch AUTOEXEC.CMD zeilenweise (nur CMD kennt
   /STEP - eine andere PROTSHELL= bekommt es nicht) */
void __far cfg_step(char *p)
{
    static const char st[] = " /STEP";
    unsigned i = 0, k;
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

/* ---- Echtzeituhr (CMOS), aus kernel.c (27.11.2026, Platz im ersten
   Codesegment) ------------------------------------------------------------ */
static unsigned cmos(unsigned r)
{
    outb(0x70, (unsigned char)r);
    return inb(0x71);
}

static unsigned from_bcd(unsigned v, unsigned bin)
{
    return bin ? v : (v >> 4) * 10 + (v & 15);
}

/* jetzt, als FAT-Datum << 16 | FAT-Uhrzeit */
unsigned long __far rtc_stamp(void)
{
    unsigned s, m, h, dd, mm, yy, cc, bin, n;

    for (n = 0; n < 2000 && (cmos(0x0A) & 0x80); n++)
        ;                                       /* Uhr zaehlt gerade     */
    bin = cmos(0x0B) & 4;
    s  = from_bcd(cmos(0x00), bin);
    m  = from_bcd(cmos(0x02), bin);
    h  = from_bcd(cmos(0x04) & 0x7F, bin);
    dd = from_bcd(cmos(0x07), bin);
    mm = from_bcd(cmos(0x08), bin);
    yy = from_bcd(cmos(0x09), bin);
    cc = from_bcd(cmos(0x32), bin);
    if (cc != 19 && cc != 20)
        cc = yy < 80 ? 20 : 19;
    yy += cc == 20 ? 100 : 0;                   /* Jahre seit 1900       */
    yy = yy < 80 ? 0 : yy - 80;                 /* FAT zaehlt ab 1980    */
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || h > 23 || m > 59 || s > 59)
        return (1UL << 5 | 1) << 16;            /* Uhr kaputt: 1.1.1980  */
    return ((unsigned long)((yy << 9) | (mm << 5) | dd) << 16) |
           ((h << 11) | (m << 5) | (s >> 1));
}

static unsigned to_bcd(unsigned v, unsigned bin)
{
    return bin ? v : ((v / 10) << 4) | (v % 10);
}

/* Echtzeituhr stellen (FAT-Format); 0 = ok */
int __far rtc_set(unsigned date, unsigned time)
{
    unsigned y = 1980 + (date >> 9), mo = (date >> 5) & 15, dd = date & 31;
    unsigned h = time >> 11, mi = (time >> 5) & 63, sc = (time & 31) << 1;
    unsigned bin = cmos(0x0B) & 4, b = cmos(0x0B), f;

    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || h > 23 || mi > 59 || sc > 59)
        return -1;
    f = irq_off();
    outb(0x70, 0x0B); outb(0x71, b | 0x80);     /* Uhr anhalten          */
    outb(0x70, 0x00); outb(0x71, to_bcd(sc, bin));
    outb(0x70, 0x02); outb(0x71, to_bcd(mi, bin));
    outb(0x70, 0x04); outb(0x71, to_bcd(h, bin));    /* 24-Stunden-Modus */
    outb(0x70, 0x07); outb(0x71, to_bcd(dd, bin));
    outb(0x70, 0x08); outb(0x71, to_bcd(mo, bin));
    outb(0x70, 0x09); outb(0x71, to_bcd(y % 100, bin));
    outb(0x70, 0x32); outb(0x71, to_bcd(y / 100, bin));
    outb(0x70, 0x0B); outb(0x71, b & 0x7F);     /* weiterlaufen          */
    irq_restore(f);
    return 0;
}

