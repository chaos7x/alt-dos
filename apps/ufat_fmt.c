/*
 * UFAT.EXE /FORMAT - FAT12/16 anlegen (eingebunden in task_ufat.c)
 *
 *   UFAT X: /FORMAT [/Q] [/V:Bezeichnung] [/V]
 *
 * Aufbau wie MS-DOS FORMAT: Disketten nach ihrer Groesse (1,44 MB, 1,2 MB,
 * 720 KB, 360 KB), Partitionen nach Partitionstyp (01h FAT12, sonst FAT16)
 * mit den Clustergroessen von MS-DOS 6. Ohne /Q wird jeder Sektor gelesen
 * und fehlerhafte Cluster in der FAT gesperrt (keine Low-Level-Formatierung).
 * Der Bootsektor bekommt den AT-DOS-Startcode (SYS macht den Datentraeger
 * startfaehig). FAT[1] mit Sauber-Bit.
 */
#include "../build/bootcode.h"

static struct fapi_fsinfo ffi;

#define VCHUNK 32                       /* Sektoren je Lesen beim Pruefen */
static unsigned char vbuf[VCHUNK * 512];

static int rd_vsecs(unsigned long lba, unsigned n)
{
    static unsigned long pos;
    pos = lba << 9;
    if (DosSeek(hd, &pos))
        return -1;
    return DosRead(hd, vbuf, n << 9) == (n << 9) ? 0 : -1;
}
static char flabel[12];

/* a / b ohne 32-Bit-Hilfsroutine (bitweise) */
static unsigned long udiv(unsigned long a, unsigned long b)
{
    unsigned long q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= 1UL << i;
        }
    }
    return q;
}

/* Sektoren je FAT fuer die Clusterzahl, Schleife bis stabil */
static unsigned calc_spf(unsigned long n, unsigned res, unsigned rsec)
{
    unsigned long data, cl, bytes;
    unsigned s = 1, s2;
    for (;;) {
        data = n - res - rsec - fbase_n(s);
        cl = shr(data, shift);
        bytes = fat16 ? (cl + 2) << 1 : (((cl + 2) << 1) + (cl + 2) + 1) >> 1;
        s2 = (unsigned)((bytes + 511) >> 9);
        if (s2 <= s)
            return s;
        s = s2;
    }
}

static int do_format(int quick)
{
    static unsigned short dt[2];
    unsigned long n, cl, s, step, next;
    unsigned spt = 63, heads = 16, res = 1, rsec, media = 0xF8, i, k, pct = 0;
    unsigned char *b;

    if (DosQFSInfo(drive, &ffi) || (ffi.flags & FSI_READONLY)) {
        puts("Dieses Laufwerk kann nicht formatiert werden.\r\n");
        return 3;
    }
    n = ffi.sectors;
    hd = DosOpenDrive(drive, DRV_OPEN_LOCK);
    if (FAPI_FAILED(hd)) {
        puts("Das Laufwerk kann nicht gesperrt werden (Dateien ge\x94" "ffnet?).\r\n");
        return 3;
    }
    nfats = 2;
    if (ffi.flags & FSI_REMOVABLE) {                /* Disketten          */
        fat16 = 0;
        rootents = 224; spc = 1; media = 0xF0; heads = 2; spt = 18;
        if (n == 2400) { spt = 15; media = 0xF9; }
        else if (n == 1440) { rootents = 112; spc = 2; spt = 9; media = 0xF9; }
        else if (n == 720) { rootents = 112; spc = 2; spt = 9; media = 0xFD; }
        else if (n != 2880) {
            puts("Unbekanntes Diskettenformat.\r\n");
            return 3;
        }
    } else {
        rootents = 512;
        if (rd_secs(0, sec, 1) == 0 && rd16(sec + 11) == 512 &&
            rd16(sec + 24) >= 1 && rd16(sec + 24) <= 63 &&
            rd16(sec + 26) >= 1 && rd16(sec + 26) <= 255) {
            spt = rd16(sec + 24);               /* Geometrie behalten     */
            heads = rd16(sec + 26);
        }
        fat16 = ffi.ptype != 0x01;
        if (fat16)
            spc = n <= 262144UL ? 4 : n <= 524288UL ? 8 : n <= 1048576UL ? 16 :
                  n <= 2097152UL ? 32 : 64;
        else
            spc = 1;
    }
    for (shift = 0; (1U << shift) < spc; shift++)
        ;
    rsec = (rootents * 32 + 511) >> 9;
    for (;;) {                                  /* Clustergroesse pruefen */
        spf = calc_spf(n, res, rsec);
        cl = shr(n - res - rsec - fbase_n(spf), shift);
        if (!fat16 && cl >= 4085 && spc < 64) { spc <<= 1; shift++; continue; }
        if (fat16 && cl > 65524UL && spc < 64) { spc <<= 1; shift++; continue; }
        break;
    }
    if ((!fat16 && cl >= 4085) || (fat16 && (cl > 65524UL || cl < 4085))) {
        puts("Die Partition passt nicht zum Partitionstyp (FDISK).\r\n");
        return 3;
    }
    clusters = (unsigned)cl;
    maxc = clusters + 1;
    cshift = shift + 9;
    fat_lba = res;
    root_lba = res + fbase_n(spf);
    data_lba = root_lba + rsec;

    /* FAT im Speicher: alles frei */
    fseg[0] = DosAllocSeg(0);
    fseg[1] = spf > 128 ? DosAllocSeg(0) : fseg[0];
    if (FAPI_FAILED(fseg[0]) || FAPI_FAILED(fseg[1])) {
        puts("Zu wenig Arbeitsspeicher f\x81r die FAT.\r\n");
        return 3;
    }
    for (s = 0; s < shl(spf, 9); s++)
        *fatb(s) = 0;

    if (!quick) {                               /* Oberflaeche lesen      */
        unsigned long end = data_lba + shl(cl, shift), lba, c0, c1;
        unsigned chunk;
        puts("Datentr\x84ger wird gepr\x81" "ft ...\r\n");
        step = udiv(end - data_lba, 100);
        if (!step)
            step = 1;
        next = data_lba + step;
        for (lba = data_lba; lba < end; lba += chunk) {
            chunk = end - lba > VCHUNK ? VCHUNK : (unsigned)(end - lba);
            if (rd_vsecs(lba, chunk)) {         /* Fehler: clusterweise */
                c0 = shr(lba - data_lba, shift);
                c1 = shr(lba + chunk - 1 - data_lba, shift);
                for (; c0 <= c1; c0++)
                    for (k = 0; k < spc; k++)
                        if (rd_secs(data_lba + shl(c0, shift) + k, sec, 1)) {
                            if (fat_get((unsigned)c0 + 2) != BAD) {
                                fat_put((unsigned)c0 + 2, BAD);
                                nbad++;
                            }
                            break;
                        }
            }
            while (lba + chunk >= next && pct < 100) {
                next += step;
                pct++;
                putc('\r');
                put_num(pct, 3);
                puts(" Prozent gepr\x81" "ft");
            }
        }
        puts("\r\n");
    }
    fat_put(0, 0xFF00 | media);
    fat_put(1, 0xFFFF);                         /* Ende-Marke, sauber     */
    for (i = 0; i < 32; i++)
        fdirty[i] = 0xFF;
    fats_differ = 0;

    if (!flabel[0] && !autom) {                 /* wie MS-DOS: danach fragen */
        static char in[40];
        unsigned m;
        puts("Datentr\x84gerbezeichnung (11 Zeichen, EINGABETASTE f\x81r keine)? ");
        m = DosRead(STDIN, in, sizeof(in) - 1);
        if (FAPI_FAILED(m))
            m = 0;
        in[m] = 0;
        for (m = 0; in[m] && in[m] != '\r' && in[m] != '\n' && m < 11; m++)
            flabel[m] = up(in[m]);
        flabel[m] = 0;
    }

    /* Bootsektor */
    b = sec;
    for (i = 0; i < 512; i++)
        b[i] = (fat16 ? boot_hd : boot_fd)[i];
    for (i = 0; i < 8; i++)
        b[3 + i] = (unsigned char)"ALTDOS50"[i];
    wr16(b + 11, 512);
    b[13] = (unsigned char)spc;
    wr16(b + 14, res);
    b[16] = (unsigned char)nfats;
    wr16(b + 17, rootents);
    wr16(b + 19, n < 65536UL ? (unsigned)n : 0);
    b[21] = (unsigned char)media;
    wr16(b + 22, spf);
    wr16(b + 24, spt);
    wr16(b + 26, heads);
    wr16(b + 28, (unsigned)ffi.hidden);
    wr16(b + 30, (unsigned)(ffi.hidden >> 16));
    wr16(b + 32, n < 65536UL ? 0 : (unsigned)n);
    wr16(b + 34, n < 65536UL ? 0 : (unsigned)(n >> 16));
    b[36] = (unsigned char)((ffi.flags & FSI_REMOVABLE) ? 0 : 0x80);
    b[37] = 0;
    b[38] = 0x29;
    DosGetDateTime(dt);
    wr16(b + 39, dt[1] ^ 0x5A5A);               /* Seriennummer aus der Zeit */
    wr16(b + 41, dt[0]);
    for (i = 0; i < 11; i++)
        b[43 + i] = flabel[0] ? (flabel[i] ? flabel[i] : ' ') : "NO NAME    "[i];
    for (i = 0; i < 8; i++)
        b[54 + i] = (unsigned char)(fat16 ? "FAT16   " : "FAT12   ")[i];
    if (wr_sec(0, b)) {
        puts("Bootsektor nicht schreibbar - Formatierung abgebrochen.\r\n");
        return 3;
    }
    write_fat();
    for (i = 0; i < 512; i++)                   /* Stammverzeichnis leer  */
        dsec[i] = 0;
    for (k = 0; k < rsec; k++) {
        if (k == 0 && flabel[0]) {
            for (i = 0; i < 11; i++)
                dsec[i] = flabel[i] ? flabel[i] : ' ';
            for (; i < 11; i++)
                dsec[i] = ' ';
            dsec[11] = 0x08;
            wr16(dsec + 22, dt[1]);
            wr16(dsec + 24, dt[0]);
        }
        wr_sec(root_lba + k, dsec);
        for (i = 0; i < 32; i++)
            dsec[i] = 0;
    }
    DosClose(hd);                               /* Kernel liest neu ein   */

    puts("Formatierung abgeschlossen.\r\n\r\n");
    line(shl(clusters, cshift), " Byte Speicherplatz insgesamt", 0, 0);
    if (nbad)
        line(shl(nbad, cshift), " Byte in fehlerhaften Sektoren", 0, 0);
    line(shl(clusters - nbad, cshift), " Byte verf\x81gbar", 0, 0);
    puts("\r\n");
    line(shl(1, cshift), " Byte in jeder Zuordnungseinheit", 0, 0);
    line(clusters - nbad, " Zuordnungseinheiten verf\x81gbar", 0, 0);
    puts("\r\nDatentr\x84ger-Seriennummer: ");
    put_hex4(dt[0]);
    putc('-');
    put_hex4(dt[1] ^ 0x5A5A);
    puts("\r\n");
    return 0;
}
