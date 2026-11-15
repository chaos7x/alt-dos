/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - Seltenes aus kernel.c im zweiten Codesegment (KTEXT2):
 * Echtzeituhr (CMOS) und die Suche nach dem BIOS-Namen fuer MSD.
 */
#include "../include/fapi_abi.h"
#include "k2.h"
#include "ifs.h"

void __far kf_vio_kputs(int s, const char *str);
const char * __far kf_drv_name(int d);

static void udec(unsigned v, char *b)          /* ohne fuehrende Nullen */
{
    char t[6];
    int i = 0, j = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i)
        b[j++] = t[--i];
    b[j] = 0;
}

static void scopy(char *d, const char *s, int max)
{
    int i;
    for (i = 0; i < max - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* Laufwerke mit ihren endgueltigen Buchstaben anzeigen (nach vol_move;
   aus kernel.c, dort war das erste Codesegment voll) */

extern unsigned short mem_conv_kb, mem_ext_kb;   /* start.asm, Real Mode  */
extern unsigned char cpu_386, bios_date[9], bios_model, vga_ok;
extern unsigned short fpu_ok, bios_equip, bios_ports[7];

/* DosSysInfo ohne die Treiberliste (die steht in kernel.c) */
void __far sys_info2(struct fapi_sysinfo __far *si, unsigned irq_used)
{
    unsigned i;
    si->cpu = cpu_386 ? 3 : 2;
    si->fpu = (unsigned char)fpu_ok;
    si->vga = vga_ok;
    si->model = bios_model;
    for (i = 0; i < 8; i++)
        si->bios_date[i] = bios_date[i];
    si->bios_date[8] = 0;
    si->fdtype[0] = (unsigned char)fd_type(0);
    si->fdtype[1] = (unsigned char)fd_type(1);
    si->rsv = (unsigned char)apm_ver();     /* APM-Version, Bit 7 kernel.c */
    si->equip = bios_equip;
    for (i = 0; i < 4; i++)
        si->com[i] = bios_ports[i];
    for (i = 0; i < 3; i++)
        si->lpt[i] = bios_ports[4 + i];
    si->conv_kb = mem_conv_kb;
    si->ext_kb = mem_ext_kb;
    si->kernel_kb = (unsigned)((KDATA_LIN + kdata_size - KCODE_LIN + 1023) >> 10);
    si->irq_used = irq_used;
    si->uptime_s = uptime_s;
    rom_str(0xF0000UL, 0xFFFF, si->bios_name, 64);
    rom_str(0xC0000UL, 0x0800, si->vbios_name, 64);
}

/* ---- Echtzeituhr (CMOS) ------------------------------------------------ */
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


static const char *fd_name(unsigned blocks)
{
    return blocks == 720 ? "360 KB" : blocks == 1440 ? "720 KB" : blocks == 2400 ? "1,2 MB" :
           blocks == 5760 ? "2,88 MB" : "1,44 MB";
}

void __far list_drives(int fd_ok, int boot_session)
{
    static char msg[80];
    static const unsigned tsize[6] = { 0, 720, 2400, 1440, 2880, 5760 };
    int d, n;
    struct blkdev *b;

    for (d = (boot_drive & 0x80) ? 0 : 1; d < 2; d++)
        if (fd_ok && fd_type(d)) {          /* Diskettenlaufwerke         */
            kf_vio_kputs(boot_session, d ? "\xFE Laufwerk B: Diskette " : "\xFE Laufwerk A: Diskette ");
            kf_vio_kputs(boot_session, fd_name(tsize[fd_type(d)]));
            kf_vio_kputs(boot_session, "\n");
        }

    for (d = 2; d < NDRIVE; d++) {
        if (!(vol[d].used || vol[d].present) || d == PRE_DRIVE)
            continue;
        b = &blk[vol[d].blk];
        msg[0] = (char)('A' + d);
        scopy(msg + 1, ": ", sizeof(msg) - 1);
        n = 3;
        if (vol[d].drv == &mod_driver) {
            scopy(msg + n, "CD-ROM", sizeof(msg) - n);
        } else if (b->rem && !vol[d].used) {    /* eingehaengt bei Bedarf */
            scopy(msg + n, b->blocks ? "Wechselplatte, " : "Wechselplatte, kein Medium",
                  sizeof(msg) - n);
            if (b->blocks) {
                while (msg[n])
                    n++;
                udec((unsigned)(b->blocks >> 11), msg + n);
                while (msg[n])
                    n++;
                scopy(msg + n, " MB", sizeof(msg) - n);
            }
        } else {
            if (vol[d].part >= 5) {     /* in der erweiterten Partition */
                scopy(msg + n, "logisch ", sizeof(msg) - n);
                n += 8;
                udec(vol[d].part - 4, msg + n);
                while (msg[n])
                    n++;
            } else {
                scopy(msg + n, "Partition ", sizeof(msg) - n);
                n += 10;
                msg[n++] = (char)('0' + vol[d].part);
            }
            scopy(msg + n, !vol[d].used ? ", nicht formatiert, " :
                           vol[d].fat16 == 2 ? ", FAT32, " : vol[d].fat16 ? ", FAT16, " : ", FAT12, ", sizeof(msg) - n);
            while (msg[n])
                n++;
            udec((unsigned)(vol[d].psize >> 11), msg + n);
            while (msg[n])
                n++;
            scopy(msg + n, " MB", sizeof(msg) - n);
        }
        while (msg[n])
            n++;
        if (b->drv >= 0) {
            scopy(msg + n, "  (", sizeof(msg) - n);
            n += 3;
            scopy(msg + n, kf_drv_name(b->drv), sizeof(msg) - n);
            while (msg[n])
                n++;
            scopy(msg + n, ", Einheit ", sizeof(msg) - n);
            n += 10;
            msg[n++] = (char)('0' + b->unit);
            msg[n++] = ')';
        }
        msg[n++] = '\n';
        msg[n] = 0;
        kf_vio_kputs(boot_session, "\xFE Laufwerk ");
        kf_vio_kputs(boot_session, msg);
    }
}

