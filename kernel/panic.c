/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.5 - Absturzprotokoll (KTEXT2), aus 6.3 zurueckportiert
 *
 * panic() (kernel.c) legt Meldung, Version, Task und das Ende des
 * DMESG-Rings in einen Puffer (2 KB, beim Start angelegt) und schreibt
 * ihn direkt ueber den Treiber nach \DOS\SYSTEM\PANIC.DAT (versteckt +
 * System, wie das Absturzabbild von NT): ohne Cache, ohne Dateisystem,
 * nur wenn gerade niemand im Treiber steckt und die Kennung dieses
 * Starts noch in Sektor 0 der Datei steht (die Datei bleibt geschlossen,
 * CHKDSK und DEFRAG duerfen sie verschieben - dann wird nichts
 * geschrieben). Anders als 6.x gibt es keinen Weg ueber den Speicher:
 * der Kernel liegt hier nicht an fester Stelle (PROTECTONLY=), und die
 * BIOSe der 286er testen beim Neustart den Speicher.
 *
 * pn_boot haengt beim naechsten Start das Protokoll an
 * \DOS\SYSTEM\PANIC.LOG (fehlt das Verzeichnis: \PANIC.LOG; ab 32 KB neu
 * begonnen); die erste Zeile erscheint unter den Startmeldungen.
 */
#include "kern.h"
#include "../include/fapi_abi.h"
#include "ifs.h"
#include "../build/version.h"
#include "k2.h"

int __far kf_pn_io(int dev, unsigned long lba, unsigned long phys,
                   unsigned sel, unsigned off, int wr);
void __far kf_vio_kputs(int s, const char *t);
void __far kf_blk_flush(void);
extern unsigned pn_ksel, pn_klen, pn_khead;     /* DMESG-Ring (kernel.c) */
extern unsigned char pn_kmid;

#define PN_TXT   8u
#define PN_SIZE  2048u                  /* Protokoll: 4 Sektoren          */
#define PN_MAX   (PN_SIZE - PN_TXT)
#define PN_TMP   PN_SIZE                /* dahinter: ein Sektor zum Lesen */
#define PN_KEEP  32768UL                /* groesser: Datei neu beginnen   */
#define NPX      4                      /* Bereiche von PANIC.DAT         */

static const char pd_name[] = "\\DOS\\SYSTEM\\PANIC.DAT";
static char pn_buf[160];
static unsigned pn_sel;                 /* Puffer PN_SIZE + 512           */
static unsigned long pn_phys;
static int  pd_dev = -1;                /* Blockgeraet von PANIC.DAT      */
static unsigned long pd_lba[NPX];       /* Sektor je Bereich              */
static unsigned char pd_n[NPX];         /* Sektoren je Bereich            */
static unsigned long pd_id[2];          /* Kennung dieses Starts          */
static struct ifs_file pf_a, pf_b;
#define FA ((struct ifs_file __far *)&pf_a)
#define FB ((struct ifs_file __far *)&pf_b)

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

static unsigned pn_cat(char *b, unsigned j, const char *s)
{
    while (*s)
        b[j++] = *s++;
    return j;
}

static unsigned pn_put(unsigned n, const char *t)
{
    char __far *p = MK_FP(pn_sel, PN_TXT);
    while (*t && n < PN_MAX)
        p[n++] = *t++;
    return n;
}

/* in der Panik: Meldung, Version, Task, Ende des DMESG-Rings in den
   Puffer und nach PANIC.DAT. Interrupts gehen dafuer kurz an (die Treiber
   warten auf ihren IRQ); kf_pn_io setzt current = -1: der Treiber wartet
   in der HLT-Schleife statt im Scheduler. -> 0 geschrieben */
int __far pn_save(const char *msg, const char *tname)
{
    char __far *p;
    const char __far *r;
    unsigned long __far *m;
    unsigned n, k, st, sum, i, s = 0;
    int rc = -1;

    if (!pn_sel || pd_dev < 0 || !pd_n[0])
        return -1;
    p = MK_FP(pn_sel, 0);
    n = pn_put(0, msg);
    n = pn_put(n, "\r\nAT-DOS 5.5 Build " ATDOS_BUILD " (" ATDOS_REV ")\r\n");
    if (tname) {
        n = pn_put(n, "Laufender Task: ");
        n = pn_put(n, tname);
        n = pn_put(n, "\r\n");
    }
    if (pn_ksel && pn_klen) {
        n = pn_put(n, "Letzte Kernelmeldungen:\r\n");
        k = pn_klen < PN_MAX - n ? pn_klen : PN_MAX - n;
        st = (pn_khead - k) & (KLOG_SIZE - 1);
        r = MK_FP(pn_ksel, KLOG_OFF);
        for (i = 0; i < k; i++)
            p[PN_TXT + n++] = r[(st + i) & (KLOG_SIZE - 1)];
        if (pn_kmid && n + 2 <= PN_MAX) {
            p[PN_TXT + n++] = '\r';
            p[PN_TXT + n++] = '\n';
        }
    }
    for (sum = 0, i = 0; i < n; i++)
        sum += (unsigned char)p[PN_TXT + i];
    p[0] = 'A'; p[1] = 'T'; p[2] = 'P'; p[3] = 'N';
    *(unsigned __far *)(p + 4) = n;
    *(unsigned __far *)(p + 6) = sum;

    outb(0xA0, 0x20);                   /* falls die Panik aus einer      */
    outb(0x20, 0x20);                   /* Interruptroutine kam           */
    enable_ints();
    if (kf_pn_io(pd_dev, pd_lba[0], pn_phys + PN_TMP, pn_sel, PN_TMP, 0) == 0) {
        m = MK_FP(pn_sel, PN_TMP);      /* Kennung noch an ihrem Platz?   */
        if (m[0] == 0x4E505441UL && m[1] == 0x31544144UL &&   /* ATPNDAT1 */
            m[2] == pd_id[0] && m[3] == pd_id[1]) {
            rc = 0;
            for (i = 0; i < NPX && pd_n[i] && !rc; i++)
                for (k = 0; k < pd_n[i] && s < PN_SIZE / 512 && !rc; k++, s++)
                    rc = kf_pn_io(pd_dev, pd_lba[i] + k, pn_phys + (s << 9),
                                  pn_sel, s << 9, 1);
        }
    }
    irq_off();
    return rc;
}

/* PANIC.DAT (offen in FA): gueltiges Protokoll darin? -> Laenge */
static unsigned pd_check(void)
{
    unsigned n, sum, i, k, j;
    if (FA->size < PN_SIZE || ifs_read(FA, 0, KDATA, (unsigned)pn_buf, 8) != 8 ||
        pn_buf[0] != 'A' || pn_buf[1] != 'T' || pn_buf[2] != 'P' || pn_buf[3] != 'N')
        return 0;
    n = *(unsigned *)(pn_buf + 4);
    sum = *(unsigned *)(pn_buf + 6);
    if (!n || n > PN_MAX)
        return 0;
    for (i = 0; i < n; i += k) {
        k = n - i > sizeof pn_buf ? sizeof pn_buf : n - i;
        if (ifs_read(FA, PN_TXT + i, KDATA, (unsigned)pn_buf, k) != (int)k)
            return 0;
        for (j = 0; j < k; j++)
            sum -= (unsigned char)pn_buf[j];
    }
    return sum ? 0 : n;
}

/* PANIC.DAT fuer den naechsten Absturz: 2 KB, Kennung in Sektor 0,
   Sektoren merken. Nur auf Platten mit eigenem Treiber (512 Byte). */
static void pd_prepare(int d, int dev)
{
    static unsigned long ext[2 * NPX];
    int i, n;

    pd_dev = -1;
    if (!pn_sel && !(pn_sel = kseg_dma(PN_SIZE + 512, &pn_phys)))
        return;
    if (ifs_open(d, pd_name, FA) && ifs_create(d, pd_name, FA))
        return;
    FA->flags |= IFS_F_WRITE;
    if (FA->size != PN_SIZE && ifs_setsize(FA, PN_SIZE)) {
        ifs_close(FA);
        return;
    }
    pd_id[0] = rtc_stamp();
    pd_id[1] = get_ticks();
    *(unsigned long *)pn_buf = 0x4E505441UL;        /* "ATPNDAT1"        */
    *(unsigned long *)(pn_buf + 4) = 0x31544144UL;
    *(unsigned long *)(pn_buf + 8) = pd_id[0];
    *(unsigned long *)(pn_buf + 12) = pd_id[1];
    ifs_write(FA, 0, KDATA, (unsigned)pn_buf, 16);
    n = ifs_bmap(FA, KDATA, (unsigned)ext, NPX);
    ifs_close(FA);
    ifs_setinfo(d, pd_name, 1, 0x06, 0, 0);         /* versteckt + System */
    kf_blk_flush();                     /* Kennung jetzt auf die Platte   */
    if (n <= 0 || n > NPX)
        return;                         /* zu zerstueckelt                */
    for (i = 0; i < n; i++) {
        pd_lba[i] = ext[2 * i + 1];
        pd_n[i] = (unsigned char)((i + 1 < n ? ext[2 * i + 2] : PN_SIZE / 512)
                                  - ext[2 * i]);
    }
    if (n < NPX)
        pd_n[n] = 0;
    pd_dev = dev;
}

/* beim Start: Protokoll des letzten Laufs nach PANIC.LOG, PANIC.DAT neu */
void __far pn_boot(int d, int dev, int ses)
{
    static const char name1[] = "\\DOS\\SYSTEM\\PANIC.LOG";
    static const char name2[] = "\\PANIC.LOG";
    const char *nm = name1;
    unsigned long pos, st;
    unsigned i, j, k, n = 0;
    int have = 0, log = 0;

    if (dev < 0 || dev >= NBLK || blk[dev].drv < 0 || blk[dev].bsize != 512 ||
        blk[dev].rem)
        return;
    if (ifs_open(d, pd_name, FA) == 0) {
        have = 1;
        n = pd_check();
    }
    if (n) {
        if (ifs_open(d, name1, FB) == 0 || ifs_create(d, name1, FB) == 0)
            log = 1;
        else {
            nm = name2;
            if (ifs_open(d, name2, FB) == 0 || ifs_create(d, name2, FB) == 0)
                log = 1;
        }
        st = rtc_stamp();               /* Kopfzeile: Zeitpunkt des Starts */
        j = pn_cat(pn_buf, 0, "----- Absturz, gemeldet beim Start am ");
        j = pn_dec(pn_buf, j, (unsigned)(st >> 16) & 31, 2);
        pn_buf[j++] = '.';
        j = pn_dec(pn_buf, j, (unsigned)(st >> 21) & 15, 2);
        pn_buf[j++] = '.';
        j = pn_dec(pn_buf, j, 1980 + ((unsigned)(st >> 25) & 127), 4);
        pn_buf[j++] = ' ';
        j = pn_dec(pn_buf, j, (unsigned)(st >> 11) & 31, 2);
        pn_buf[j++] = ':';
        j = pn_dec(pn_buf, j, (unsigned)(st >> 5) & 63, 2);
        j = pn_cat(pn_buf, j, " -----\r\n");
        if (log) {
            FB->flags |= IFS_F_WRITE;
            pos = FB->size;
            if (pos > PN_KEEP) {
                ifs_setsize(FB, 0);
                pos = 0;
            }
            ifs_write(FB, pos, KDATA, (unsigned)pn_buf, j);
            pos += j;
            for (i = 0; i < n; i += k) {
                k = n - i > sizeof pn_buf ? sizeof pn_buf : n - i;
                if (ifs_read(FA, PN_TXT + i, KDATA, (unsigned)pn_buf, k) != (int)k ||
                    ifs_write(FB, pos, KDATA, (unsigned)pn_buf, k) != (int)k)
                    break;
                pos += k;
            }
            ifs_write(FB, pos, KDATA, (unsigned)"\r\n", 2);
            ifs_close(FB);
        }
        /* erste Zeile (Meldung) unter den Startmeldungen */
        j = pn_cat(pn_buf, 0, "\xFE Letzter Lauf endete mit: ");
        k = n < 100 ? n : 100;
        ifs_read(FA, PN_TXT, KDATA, (unsigned)(pn_buf + j), k);
        for (i = j; i < j + k && pn_buf[i] != '\r'; i++)
            ;
        pn_buf[i++] = '\n';
        pn_buf[i] = 0;
        kf_vio_kputs(ses, pn_buf);
        j = pn_cat(pn_buf, 0, log ? "  Protokoll: "
                   : "  Protokoll konnte nicht geschrieben werden.\n");
        if (log) {
            pn_buf[j++] = (char)('A' + d);
            pn_buf[j++] = ':';
            j = pn_cat(pn_buf, j, nm);
            pn_buf[j++] = '\n';
        }
        pn_buf[j] = 0;
        kf_vio_kputs(ses, pn_buf);
    }
    if (have)
        ifs_close(FA);
    pd_prepare(d, dev);                 /* neue Kennung, alter Kopf weg   */
}
