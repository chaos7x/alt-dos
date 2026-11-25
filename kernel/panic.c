/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Absturzprotokoll (KTEXT4)
 *
 * panic() (kernel.c) legt Meldung, Version, Task und das Ende des
 * DMESG-Rings bei physisch PN_LIN (120000h) ab: "ATPN", Laenge, Summe,
 * Text. Zwei Wege zum naechsten Start:
 *
 * - RAM: QEMU/SeaBIOS und BIOSe, die beim Warmstart (1234h) den Speicher
 *   nicht testen, lassen die Seite stehen. pn_check findet sie gleich
 *   nach mem_init, die Seite bleibt bis pn_boot reserviert.
 * - Platte, wie das Absturzabbild von NT: \DOS\SYSTEM\PANIC.DAT (4 KB,
 *   versteckt+System) wird beim Start angelegt, ihre Sektoren gemerkt und
 *   Sektor 0 mit einer Kennung je Start beschrieben. pn_disk schreibt das
 *   Protokoll direkt ueber den Treiber dorthin (ohne Cache, ohne
 *   Dateisystem) - nur wenn gerade niemand im Treiber steckt und die
 *   Kennung noch dort steht (die Datei bleibt geschlossen, CHKDSK und
 *   DEFRAG duerfen sie verschieben; dann wird nichts geschrieben). Noetig,
 *   weil manche BIOSe (AMI auf dem 386 in 86Box) beim Neustart den ganzen
 *   Speicher loeschen.
 *
 * pn_boot haengt das Protokoll an \DOS\SYSTEM\PANIC.LOG des
 * Startlaufwerks (fehlt das Verzeichnis: \PANIC.LOG; ab 32 KB neu
 * begonnen); die erste Zeile erscheint unter den Startmeldungen.
 */
#include "kern.h"
#include "../include/fapi_abi.h"
#include "mem.h"
#include "ifs.h"
#include "task.h"
#include "../build/version.h"
#include "k2.h"

struct ifs_file __far * __far kf_kopen(int d, const char *canon, int create);
void __far kf_kclose(struct ifs_file __far *f);
int __far kf_pn_io(int dev, unsigned long lba, unsigned n, unsigned long phys,
                   unsigned sel, int wr);
extern int current;
void __far kf_blk_flush(void);

#define PN_TXT  8u
#define PN_MAX  (0x1000u - PN_TXT)
#define PN_KEEP 32768UL                 /* groesser: Datei neu beginnen   */
#define PN_TMP  0x7000UL                /* Lesepuffer in der Panik (Real-
                                           Mode-Puffer, frei)            */
#define NPX     4                       /* Bereiche von PANIC.DAT         */

static const char pd_name[] = "\\DOS\\SYSTEM\\PANIC.DAT";
static char pn_buf[256];
static int  pd_dev = -1;                /* Blockgeraet von PANIC.DAT      */
static unsigned long pd_lba[NPX];       /* Sektor je Bereich              */
static unsigned char pd_n[NPX];         /* Sektoren je Bereich            */
static unsigned long pd_id[2];          /* Kennung dieses Starts          */

#pragma code_seg("ITEXT", "CODE")   /* nur beim Start (INIT-SEGMENT.md) */
static const char __based(__segname("IDATA")) kp_2[] = "----- Absturz, gemeldet beim Start am ";
static const char __based(__segname("IDATA")) kp_3[] = " -----\r\n";
static const char __based(__segname("IDATA")) kp_4[] = "\xFE Letzter Lauf endete mit: ";
static const char __based(__segname("IDATA")) kp_5[] = "  Protokoll: ";
static const char __based(__segname("IDATA")) kp_6[] = "  Protokoll konnte nicht geschrieben werden.\n";
static unsigned pn_dec(char *b, unsigned j, unsigned v, int w)
{
    char t[6];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n < w)
        t[n++] = '0';
    while (n)
        b[j++] = t[--n];
    return j;
}
#pragma code_seg("KTEXT4", "CODE")

#pragma code_seg("ITEXT", "CODE")   /* nur beim Start (INIT-SEGMENT.md) */
static unsigned pn_cat(char *b, unsigned j, const char __far *s)
{
    while (*s)
        b[j++] = *s++;
    return j;
}
#pragma code_seg("KTEXT4", "CODE")

static unsigned pn_put(unsigned n, const char __far *t)
{
    char __far *p = MK_FP(SCRATCH, PN_TXT);
    while (*t && n < PN_MAX)
        p[n++] = *t++;
    return n;
}

/* in der Panik: Meldung, Version, Task, Ende des DMESG-Rings nach PN_LIN */
void __far pn_save(const char *msg)
{
    char __far *p = MK_FP(SCRATCH, 0);
    const char __far *r;
    unsigned n, k, st, sum, i;
    scratch_map(PN_LIN, 0xFFF);
    n = pn_put(0, msg);
    n = pn_put(n, "\r\nAT-DOS 6.3 Build " ATDOS_BUILD " (" ATDOS_REV ")\r\n");
    if (current >= 0) {
        n = pn_put(n, "Laufender Task: ");
        n = pn_put(n, task[current].name);
        n = pn_put(n, "\r\n");
    }
    if (subst_sel && klog_len) {
        n = pn_put(n, "Letzte Kernelmeldungen:\r\n");
        k = klog_len < PN_MAX - n ? klog_len : PN_MAX - n;
        st = (klog_head - k) & (KLOG_SIZE - 1);
        r = MK_FP(subst_sel, KLOG_OFF);
        for (i = 0; i < k; i++)
            p[PN_TXT + n++] = r[(st + i) & (KLOG_SIZE - 1)];
        if (klog_mid && n + 2 <= PN_MAX) {
            p[PN_TXT + n++] = '\r';
            p[PN_TXT + n++] = '\n';
        }
    }
    for (sum = 0, i = 0; i < n; i++)
        sum += (unsigned char)p[PN_TXT + i];
    p[0] = 'A'; p[1] = 'T'; p[2] = 'P'; p[3] = 'N';
    *(unsigned __far *)(p + 4) = n;
    *(unsigned __far *)(p + 6) = sum;
}

/* beim Start (gleich nach mem_init): Protokoll im Speicher? -> Laenge */
#pragma code_seg("ITEXT", "CODE")   /* nur beim Start (INIT-SEGMENT.md) */
unsigned __far pn_check(void)
{
    const unsigned char __far *p = MK_FP(SCRATCH, 0);
    unsigned n, sum, i;
    scratch_map(PN_LIN, 0xFFF);
    if (p[0] != 'A' || p[1] != 'T' || p[2] != 'P' || p[3] != 'N')
        return 0;
    n = *(const unsigned __far *)(p + 4);
    if (!n || n > PN_MAX)
        return 0;
    for (sum = 0, i = 0; i < n; i++)
        sum += p[PN_TXT + i];
    return sum == *(const unsigned __far *)(p + 6) ? n : 0;
}
#pragma code_seg("KTEXT4", "CODE")

/* in der Panik: Protokoll (bei PN_LIN) nach PANIC.DAT. Interrupts gehen
   dafuer kurz an (die Treiber warten auf ihren IRQ), current = -1: der
   Treiber wartet in der HLT-Schleife statt im Scheduler. -> 0 geschrieben */
int __far pn_disk(void)
{
    unsigned i, k, s = 0;
    int r = -1;
    unsigned long __far *m;

    if (pd_dev < 0 || !pd_n[0])
        return -1;
    current = -1;
    outb(0xA0, 0x20);                   /* falls die Panik aus einer      */
    outb(0x20, 0x20);                   /* Interruptroutine kam           */
    enable_ints();
    scratch2_map(PN_TMP, 0x1FF);        /* Kennung noch an ihrem Platz?   */
    if (kf_pn_io(pd_dev, pd_lba[0], 1, PN_TMP, SCRATCH2, 0) == 0) {
        m = MK_FP(SCRATCH2, 0);
        if (m[0] == 0x4E505441UL && m[1] == 0x31544144UL &&   /* ATPNDAT1 */
            m[2] == pd_id[0] && m[3] == pd_id[1]) {
            r = 0;
            for (i = 0; i < NPX && pd_n[i] && !r; i++)
                for (k = 0; k < pd_n[i] && s < 8 && !r; k++, s++) {
                    scratch_map(PN_LIN + ((unsigned long)s << 9), 0x1FF);
                    r = kf_pn_io(pd_dev, pd_lba[i] + k, 1,
                                 PN_LIN + ((unsigned long)s << 9), SCRATCH, 1);
                }
        }
    }
    irq_off();
    scratch_map(PN_LIN, 0xFFF);
    return r;
}

/* Text des Protokolls (n Byte) an f ab pos haengen: aus dem Speicher
   (src = 0) oder aus PANIC.DAT (src) */
#pragma code_seg("ITEXT", "CODE")   /* nur beim Start (INIT-SEGMENT.md) */
static unsigned long pn_copy(struct ifs_file __far *f, unsigned long pos,
                             struct ifs_file __far *src, unsigned n)
{
    unsigned i, k, j;
    char __far *p;
    for (i = 0; i < n; i += k) {        /* stueckweise ueber pn_buf       */
        k = n - i > sizeof pn_buf ? sizeof pn_buf : n - i;
        if (src)
            ifs_read(src, PN_TXT + i, KDATA, (unsigned)pn_buf, k);
        else {
            scratch_map(PN_LIN, 0xFFF);
            p = MK_FP(SCRATCH, PN_TXT + i);
            for (j = 0; j < k; j++)
                pn_buf[j] = p[j];
        }
        if (ifs_write(f, pos, KDATA, (unsigned)pn_buf, k) != (int)k)
            break;
        pos += k;
    }
    return pos;
}
#pragma code_seg("KTEXT4", "CODE")

/* PANIC.DAT: gueltiges Protokoll darin? -> Laenge */
#pragma code_seg("ITEXT", "CODE")   /* nur beim Start (INIT-SEGMENT.md) */
static unsigned pd_check(struct ifs_file __far *f)
{
    unsigned n, sum, i, k, j;
    if (f->size < 4096 || ifs_read(f, 0, KDATA, (unsigned)pn_buf, 8) != 8 ||
        pn_buf[0] != 'A' || pn_buf[1] != 'T' || pn_buf[2] != 'P' || pn_buf[3] != 'N')
        return 0;
    n = *(unsigned *)(pn_buf + 4);
    sum = *(unsigned *)(pn_buf + 6);
    if (!n || n > PN_MAX)
        return 0;
    for (i = 0; i < n; i += k) {
        k = n - i > sizeof pn_buf ? sizeof pn_buf : n - i;
        if (ifs_read(f, PN_TXT + i, KDATA, (unsigned)pn_buf, k) != (int)k)
            return 0;
        for (j = 0; j < k; j++)
            sum -= (unsigned char)pn_buf[j];
    }
    return sum ? 0 : n;
}
#pragma code_seg("KTEXT4", "CODE")

/* PANIC.DAT fuer den naechsten Absturz: 4 KB, Kennung in Sektor 0,
   Sektoren merken. Nur auf Platten mit eigenem Treiber (512 Byte). */
#pragma code_seg("ITEXT", "CODE")   /* nur beim Start (INIT-SEGMENT.md) */
static void pd_prepare(int d)
{
    static unsigned long ext[2 * NPX];
    struct ifs_file __far *f;
    int i, n, dev = vol[d].blk;

    pd_dev = -1;
    if (dev < 0 || dev >= NBLK || blk[dev].drv < 0 || blk[dev].drv >= NDRV ||
        blk[dev].bsize != 512)
        return;
    f = kf_kopen(d, pd_name, 0);
    if (!f && !(f = kf_kopen(d, pd_name, 1)))
        return;
    if (f->size != 4096 && ifs_setsize(f, 4096)) {
        kf_kclose(f);
        return;
    }
    pd_id[0] = rtc_stamp();
    pd_id[1] = get_ticks();
    *(unsigned long *)pn_buf = 0x4E505441UL;        /* "ATPNDAT1"        */
    *(unsigned long *)(pn_buf + 4) = 0x31544144UL;
    *(unsigned long *)(pn_buf + 8) = pd_id[0];
    *(unsigned long *)(pn_buf + 12) = pd_id[1];
    ifs_write(f, 0, KDATA, (unsigned)pn_buf, 16);
    f->attr = 0x06;                     /* versteckt + System             */
    f->flags |= IFS_F_ATTR;
    n = ifs_bmap(f, KDATA, (unsigned)ext, NPX);
    kf_kclose(f);
    kf_blk_flush();                     /* Kennung jetzt auf die Platte   */
    if (n <= 0 || n > NPX)
        return;                         /* zu zerstueckelt: nur RAM       */
    for (i = 0; i < n; i++) {
        pd_lba[i] = ext[2 * i + 1];
        pd_n[i] = (unsigned char)((i + 1 < n ? ext[2 * i + 2] : 8) - ext[2 * i]);
    }
    if (n < NPX)
        pd_n[n] = 0;
    pd_dev = dev;
}
#pragma code_seg("KTEXT4", "CODE")

#pragma code_seg("ITEXT", "CODE")   /* nur beim Start (INIT-SEGMENT.md) */
void __far pn_boot(int d, int ses, unsigned n)
{
    static const char name1[] = "\\DOS\\SYSTEM\\PANIC.LOG";
    static const char name2[] = "\\PANIC.LOG";
    struct ifs_file __far *f = 0;
    struct ifs_file __far *src = 0;
    const char *nm = name1;
    unsigned long pos, st;
    unsigned i, j;
    char __far *p;

    if (!n && (src = kf_kopen(d, pd_name, 0)) != 0 && (n = pd_check(src)) == 0) {
        kf_kclose(src);
        src = 0;
    }
    if (n) {
        f = kf_kopen(d, name1, 0);
        if (!f && !(f = kf_kopen(d, name1, 1))) {
            nm = name2;
            if (!(f = kf_kopen(d, name2, 0)))
                f = kf_kopen(d, name2, 1);
        }
        st = rtc_stamp();               /* Kopfzeile: Zeitpunkt des Starts */
        j = pn_cat(pn_buf, 0, (const char __far *)kp_2);
        j = pn_dec(pn_buf, j, (unsigned)(st >> 16) & 31, 2);
        pn_buf[j++] = '.';
        j = pn_dec(pn_buf, j, (unsigned)(st >> 21) & 15, 2);
        pn_buf[j++] = '.';
        j = pn_dec(pn_buf, j, 1980 + ((unsigned)(st >> 25) & 127), 4);
        pn_buf[j++] = ' ';
        j = pn_dec(pn_buf, j, (unsigned)(st >> 11) & 31, 2);
        pn_buf[j++] = ':';
        j = pn_dec(pn_buf, j, (unsigned)(st >> 5) & 63, 2);
        j = pn_cat(pn_buf, j, (const char __far *)kp_3);
        if (f) {
            pos = f->size;
            if (pos > PN_KEEP) {
                ifs_setsize(f, 0);
                pos = 0;
            }
            ifs_write(f, pos, KDATA, (unsigned)pn_buf, j);
            pos = pn_copy(f, pos + j, src, n);
            ifs_write(f, pos, KDATA, (unsigned)"\r\n", 2);
            kf_kclose(f);
        }
        /* erste Zeile (Meldung) unter den Startmeldungen */
        j = pn_cat(pn_buf, 0, (const char __far *)kp_4);
        if (src)
            ifs_read(src, PN_TXT, KDATA, (unsigned)(pn_buf + j), 100);
        else {
            scratch_map(PN_LIN, 0xFFF);
            p = MK_FP(SCRATCH, PN_TXT);
            for (i = 0; i < n && i < 100; i++)
                pn_buf[j + i] = p[i];
        }
        for (i = j; i < j + 100 && pn_buf[i] != '\r'; i++)
            ;
        pn_buf[i++] = '\n';
        pn_buf[i] = 0;
        vio_kputs(ses, pn_buf);
        j = pn_cat(pn_buf, 0, f ? (const char __far *)kp_5 : (const char __far *)kp_6);
        if (f) {
            pn_buf[j++] = (char)('A' + d);
            pn_buf[j++] = ':';
            j = pn_cat(pn_buf, j, nm);
            pn_buf[j++] = '\n';
        }
        pn_buf[j] = 0;
        vio_kputs(ses, pn_buf);
    }
    if (src)
        kf_kclose(src);
    if (pn_check()) {                   /* Speicher: verbraucht           */
        scratch_map(PN_LIN, 0xFFF);
        *(char __far *)MK_FP(SCRATCH, 0) = 0;
        mem_free(PN_LIN >> 12, 1);
    }
    pd_prepare(d);                      /* neue Kennung, alter Kopf weg   */
}
#pragma code_seg("KTEXT4", "CODE")
