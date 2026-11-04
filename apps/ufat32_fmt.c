/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UFAT32.EXE /FORMAT - FAT32 anlegen (eingebunden in task_ufat32.c)
 *
 *   FORMAT X: /FS:FAT32 [/Q] [/V:Bezeichnung]  ->  UFAT32 X: /FORMAT ...
 *
 * Aufbau wie Windows 95 OSR2/98: 32 reservierte Sektoren (FSInfo bei 1,
 * Kopie des Bootsektors bei 6 und des FSInfo bei 7), zwei FATs, Stamm im
 * Cluster 2. Clustergroesse nach der Microsoft-Tabelle (bis 8 GB 4 KB,
 * bis 16 GB 8 KB, bis 32 GB 16 KB, darueber 32 KB; unter 260 MB 512 Byte),
 * kleiner, wenn sonst keine 65.525 Cluster zusammenkommen. Sektoren je
 * FAT nach der Formel der FAT-Spezifikation. Ohne /Q wird danach jeder
 * Cluster gelesen und Unlesbares als fehlerhaft gesperrt. Startcode: nur
 * eine Meldung (Start von FAT32 kommt mit SYS).
 */

static struct fapi_fsinfo ffi;

/* a / b ohne 32-Bit-Hilfsroutine */
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

static unsigned char zb[4096];

/* n Sektoren ab lba mit Nullen */
static int zero_secs(unsigned long lba, unsigned long n)
{
    static unsigned long pos;
    unsigned k;
    pos = lba;
    if (DosSeek(hd, &pos))
        return -1;
    while (n) {
        k = n > 8 ? 8 : (unsigned)n;
        if (DosWrite(hd, zb, k << 9) != (k << 9))
            return -1;
        n -= k;
    }
    return 0;
}

/* Startcode ohne System: Meldung, Taste, INT 19h */
static const unsigned char nosys[] = {
    0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0x8E, 0xD0, 0xBC, 0x00, 0x7C, 0xFB,   /* cli; ds=ss=0; sp */
    0xBE, 0x80, 0x7C,                   /* mov si, 7C80h (Text bei 80h)   */
    0xAC, 0x08, 0xC0, 0x74, 0x09,       /* lodsb; or al,al; jz +9         */
    0xB4, 0x0E, 0xBB, 0x07, 0x00, 0xCD, 0x10, /* int 10h/0Eh            */
    0xEB, 0xF2,                         /* jmp lodsb                      */
    0x31, 0xC0, 0xCD, 0x16, 0xCD, 0x19  /* Taste, int 19h                 */
};
static const char nosys_msg[] = "\r\nKein Systemdatentr\x84ger - Taste dr\x81" "cken\r\n";

static int do_format32(int quick)
{
    static unsigned short dt[2];
    unsigned long n, spf32, cl, mb, c, bad = 0, step, next, lba;
    unsigned res = 32, k, i, pct = 0;
    unsigned char *b = sec;

    if (DosQFSInfo(drive, &ffi) || (ffi.flags & (FSI_READONLY | FSI_REMOVABLE))) {
        puts("FAT32 gibt es nur f\x81r Festplatten-Partitionen.\r\n");
        return 3;
    }
    n = ffi.sectors;
    mb = n >> 11;
    spc = mb < 260 ? 1 : mb <= 8192 ? 8 : mb <= 16384 ? 16 : mb <= 32768 ? 32 : 64;
    for (;;) {                          /* mindestens 65.525 Cluster   */
        for (shift = 0; (1U << shift) < spc; shift++)
            ;
        spf32 = udiv(n - res + (128UL * spc + 1) - 1, 128UL * spc + 1);   /* ((256*spc)+2)/2 */
        cl = shr(n - res - spf32 - spf32, shift);
        if (cl >= 65525UL || spc == 1)
            break;
        spc >>= 1;
    }
    if (cl < 65525UL) {
        puts("Das Laufwerk ist f\x81r FAT32 zu klein (mindestens etwa 33 MB).\r\n");
        return 3;
    }
    if (cl > 0x0FFFFFF4UL) {
        puts("Das Laufwerk ist f\x81r FAT32 zu gro\xE1.\r\n");
        return 3;
    }
    hd = DosOpenDrive(drive, DRV_OPEN_LOCK | DRV_OPEN_LBA);
    if (FAPI_FAILED(hd)) {
        puts("Das Laufwerk kann nicht gesperrt werden (Dateien ge\x94" "ffnet?).\r\n");
        return 3;
    }
    cshift = shift + 9;
    clusters = cl;
    maxc = cl + 1;
    spf = spf32;
    nfats = 2;
    fat_lba = res;
    data_lba = res + spf32 + spf32;
    rootc = 2;

    /* reservierte Sektoren, FATs und Stammcluster leeren */
    puts("Lege FAT32 an...\r\n");
    if (zero_secs(0, data_lba + spc)) {
        puts("Schreibfehler.\r\n");
        DosClose(hd);
        return 3;
    }
    /* Bootsektor */
    for (i = 0; i < 512; i++)
        b[i] = 0;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    for (i = 0; i < 8; i++)
        b[3 + i] = (unsigned char)"ALTDOS60"[i];
    wr16(b + 11, 512);
    b[13] = (unsigned char)spc;
    wr16(b + 14, res);
    b[16] = 2;
    b[21] = 0xF8;
    wr16(b + 24, 63);
    wr16(b + 26, 255);
    wr32(b + 28, ffi.hidden);
    wr32(b + 32, n);
    wr32(b + 36, spf32);
    wr32(b + 44, 2);                    /* Stammcluster                  */
    wr16(b + 48, 1);                    /* FSInfo                        */
    wr16(b + 50, 6);                    /* Kopie des Bootsektors         */
    b[64] = 0x80;
    b[66] = 0x29;
    DosGetDateTime(dt);
    wr16(b + 67, dt[1] ^ 0x1234);
    wr16(b + 69, dt[0] + 0x4321);
    for (i = 0; i < 11; i++)
        b[71 + i] = flabel[0] ? (unsigned char)(flabel[i] ? flabel[i] : ' ') :
                                (unsigned char)"NO NAME    "[i];
    if (flabel[0])                      /* nach dem ersten Leerzeichen   */
        for (i = 0, k = 0; i < 11; i++) {
            if (!flabel[i])
                k = 1;
            if (k)
                b[71 + i] = ' ';
        }
    for (i = 0; i < 8; i++)
        b[82 + i] = (unsigned char)"FAT32   "[i];
    for (i = 0; i < sizeof(nosys); i++)
        b[90 + i] = nosys[i];
    for (i = 0; nosys_msg[i]; i++)
        b[0x80 + i] = (unsigned char)nosys_msg[i];
    b[510] = 0x55; b[511] = 0xAA;
    for (i = 0; i < 96; i++)
        boot[i] = b[i];
    if (wr_sec(0, b) || wr_sec(6, b))
        goto werr;
    /* FSInfo */
    for (i = 0; i < 512; i++)
        b[i] = 0;
    wr32(b, 0x41615252UL);
    wr32(b + 484, 0x61417272UL);
    wr32(b + 488, cl - 1);              /* Cluster 2 = Stamm             */
    wr32(b + 492, 3);
    wr32(b + 508, 0xAA550000UL);
    if (wr_sec(1, b) || wr_sec(7, b))
        goto werr;
    /* FAT-Anfang in beiden Kopien: Medium, Sauber-Bit, Stamm-Ende */
    for (i = 0; i < 512; i++)
        b[i] = 0;
    wr32(b, 0x0FFFFFF8UL);
    wr32(b + 4, 0x0FFFFFFFUL);
    wr32(b + 8, 0x0FFFFFFFUL);
    if (wr_sec(res, b) || wr_sec(res + spf32, b))
        goto werr;
    /* Bezeichnung im Stamm */
    if (flabel[0]) {
        for (i = 0; i < 512; i++)
            b[i] = 0;
        for (i = 0, k = 0; i < 11; i++) {
            if (!flabel[i])
                k = 1;
            b[i] = k ? ' ' : (unsigned char)flabel[i];
        }
        b[11] = 0x08;
        wr16(b + 22, dt[1]);
        wr16(b + 24, dt[0]);
        if (wr_sec(data_lba, b))
            goto werr;
    }
    /* ohne /Q: jeden Cluster lesen, Unlesbares sperren */
    if (!quick) {
        fc_init();
        step = shr(cl, 7) + 1;
        next = 0;
        for (c = 3; c <= maxc; c++) {
            if (c - 3 >= next) {
                putc('\r');
                put_num(pct++ * 100 / 128, 3);
                puts(" Prozent gepr\x81" "ft");
                next += step;
            }
            lba = clus_lba(c);
            for (i = 0; i < spc; i += k) {
                k = spc - i > 8 ? 8 : spc - i;
                if (rd_secs(lba + i, zb, k)) {
                    fat_put(c, BAD);
                    bad++;
                    break;
                }
            }
        }
        for (i = 0; i < 4096; i++)
            zb[i] = 0;
        fat_flush();
        puts("\r100 Prozent gepr\x81" "ft\r\n");
        if (bad && rd_secs(1, b, 1) == 0) {
            wr32(b + 488, cl - 1 - bad);
            wr_sec(1, b);
            wr_sec(7, b);
        }
    }
    DosClose(hd);
    puts("Formatieren beendet (FAT32, ");
    put_num(shl(1, cshift), 0);
    puts(" Byte je Zuordnungseinheit).\r\n");
    if (bad) {
        put_num(bad, 0);
        puts(" fehlerhafte Zuordnungseinheiten gesperrt.\r\n");
    }
    return 0;
werr:
    puts("Schreibfehler.\r\n");
    DosClose(hd);
    return 3;
}
