/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MSD.EXE - Systemdiagnose (Aufbau wie MSD von MS-DOS 6):
 *
 *   MSD [/S]
 *
 * Vollbild mit Knoepfen (Computer, Speicher, Video, Betriebssystem, Maus,
 * Laufwerke, LPT, COM, IRQ-Status, Treiber), daneben eine Kurzangabe;
 * Buchstabe, Pfeiltasten + Eingabe oder Mausklick oeffnen ein Fenster mit
 * Einzelheiten. /S schreibt alles als Bericht auf die Standardausgabe
 * (umleitbar: MSD /S > BERICHT.TXT).
 * Angaben kommen nur ueber das FAPI: DosSysInfo (Prozessor, BIOS,
 * Anschluesse, Treiber, IRQs), DosMemInfo, DosQFSInfo, DosDiskInfo, MouOpen.
 */
#include "fapi.h"
#include "util.h"

#define NB      10                      /* Knoepfe                        */
#define LW      66                      /* Zeilenbreite im Fenster        */
#define NL      24

static struct fapi_sysinfo si;
static struct fapi_meminfo mi;
static struct fapi_fsinfo fi;
static struct fapi_diskinfo di;
static struct fapi_mouevent mev;
static struct fapi_pcidev pd;
static struct fapi_res rs;
static char args[FAPI_ARGLEN];
static char lines[NL][LW + 1], sum[NB][34], row[81];
static unsigned nl, nbtn, rows;

static const char *const bname[NB] = {
    "Computer...", "Speicher...", "Video...", "Betriebssystem...", "Maus...",
    "Laufwerke...", "LPT-Anschl\x81sse...", "COM-Anschl\x81sse...", "IRQ-Status...",
    "Treiber..." };
static const char bkey[NB] = { 'C', 'S', 'V', 'B', 'M', 'L', 'P', 'O', 'Q', 'T' };

/* ---- Text bauen ------------------------------------------------------- */
static char *ap(char *d, const char *s)    /* Zeilen hoechstens LW   */
{
    char *e = d + LW;
    while (*d)
        d++;
    while (*s && d < e)
        *d++ = *s++;
    *d = 0;
    return d;
}

/* v / 10 ohne 32-Bit-Hilfsroutine, Rest in *r */
static unsigned long div10(unsigned long v, unsigned *r)
{
    unsigned long q = 0, m = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        m = (m << 1) | ((v >> i) & 1);
        if (m >= 10) {
            m -= 10;
            q |= 1UL << i;
        }
    }
    *r = (unsigned)m;
    return q;
}

static void apn(char *d, unsigned long v)
{
    static char b[12];
    unsigned i = 11, r;
    b[i] = 0;
    do {
        v = div10(v, &r);
        b[--i] = (char)('0' + r);
    } while (v);
    ap(d, b + i);
}

static void aphex(char *d, unsigned v)
{
    static char b[5];
    int i;
    for (i = 3; i >= 0; i--, v >>= 4)
        b[i] = "0123456789ABCDEF"[v & 15];
    b[4] = 0;
    ap(d, b);
}

static void aphex2(char *d, unsigned v)
{
    static char b[3];
    b[0] = "0123456789ABCDEF"[(v >> 4) & 15];
    b[1] = "0123456789ABCDEF"[v & 15];
    b[2] = 0;
    ap(d, b);
}

/* bekannte PCI-Hersteller und -Klassen (Auswahl) */
static const struct { unsigned id; const char *n; } vendors[] = {
    { 0x8086, "Intel" }, { 0x1000, "NCR/Symbios" }, { 0x1022, "AMD" },
    { 0x1106, "VIA" }, { 0x1039, "SiS" }, { 0x10B9, "ALi" }, { 0x1013, "Cirrus" },
    { 0x5333, "S3" }, { 0x1002, "ATI" }, { 0x10DE, "nVidia" }, { 0x102B, "Matrox" },
    { 0x121A, "3dfx" }, { 0x100C, "Tseng" }, { 0x1023, "Trident" }, { 0x10EC, "Realtek" },
    { 0x1011, "DEC" }, { 0x10B7, "3Com" }, { 0x9004, "Adaptec" }, { 0x104B, "BusLogic" },
    { 0x1274, "Ensoniq" }, { 0x1102, "Creative" }, { 0x125D, "ESS" }, { 0x1234, "Bochs/QEMU" },
    { 0x1AF4, "virtio" }, { 0x80EE, "VirtualBox" }, { 0x15AD, "VMware" }
};

static const char *pci_kind(unsigned c, unsigned s)
{
    switch (c) {
    case 0x00: return s == 1 ? "VGA (alt)" : "Ger\x84t (alt)";
    case 0x01: return s == 0 ? "SCSI-Controller" : s == 1 ? "IDE-Controller" :
                      s == 2 ? "Diskettencontroller" : "Massenspeicher";
    case 0x02: return "Netzwerk";
    case 0x03: return s == 0 ? "VGA-Grafik" : "Grafik";
    case 0x04: return s == 1 ? "Audio" : "Multimedia";
    case 0x05: return "Speicher";
    case 0x06: return s == 0 ? "Host-Br\x81\x63ke" : s == 1 ? "ISA-Br\x81\x63ke" :
                      s == 4 ? "PCI-PCI-Br\x81\x63ke" : s == 7 ? "CardBus" : "Br\x81\x63ke";
    case 0x07: return "Kommunikation";
    case 0x08: return "Systemger\x84t";
    case 0x0C: return s == 3 ? "USB" : s == 5 ? "SMBus" : "Serieller Bus";
    }
    return "unbekannt";
}

static char *line(const char *a, const char *b)  /* "Name:   Wert"    */
{
    char *l;
    unsigned n;
    if (nl >= NL)
        return lines[NL - 1];
    l = lines[nl++];
    l[0] = 0;
    ap(l, a);
    for (n = slen(l); b && n < 24; n++)
        l[n] = ' ';
    l[n] = 0;
    for (; b && *b && n < LW; n++)          /* auf Fensterbreite kuerzen */
        l[n] = *b++;
    l[n] = 0;
    return l;
}

static const char *fdname(unsigned t)
{
    return t == 1 ? "5,25\" 360 KB" : t == 2 ? "5,25\" 1,2 MB" : t == 3 ? "3,5\" 720 KB" :
           t == 4 ? "3,5\" 1,44 MB" : t == 5 ? "3,5\" 2,88 MB" : "keins";
}

/* ---- Einzelheiten je Knopf -------------------------------------------- */
/* erweiterter Speicher: aus der Speicherkarte (6.0), sonst INT 15h/88h */
static unsigned long ext_kb(void)
{
    return mi.total_kb32 > si.conv_kb ? mi.total_kb32 - si.conv_kb : si.ext_kb;
}

static int same_name(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void detail(unsigned k)
{
    static char v[LW + 1];
    unsigned i, n, d;
    char *l;

    nl = 0;
    switch (k) {
    case 0:
        line("Prozessor:", si.cpu == 3 ? "80386 oder neuer" : "80286");
        line("Koprozessor:", !si.fpu ? "keiner" : si.fpu == 1 ? "80287" :
                             si.cpu == 3 ? "80387" : "80287XL (387-Kern)");
        l = line("BIOS-Datum:", si.bios_date);
        v[0] = 0;
        aphex(v, si.model);
        l = line("BIOS-Modellbyte:", v + 2);
        ap(l, "h");
        line("BIOS:", si.bios_name[0] ? si.bios_name : "(kein Text gefunden)");
        v[0] = 0;
        apn(v, si.uptime_s / 60);
        ap(v, " min");
        line("Laufzeit:", v);
        if (DosBusInfo(BUS_PCI, 0, &pd) != 0) {
            line("Bus:", "ISA (AT)");
            break;
        }
        line("Bus:", "ISA und PCI");
        for (i = 0; DosBusInfo(BUS_PCI, i, &pd) == 0 && nl < NL; i++) {
            v[0] = 0;
            ap(v, "  ");
            aphex2(v, pd.loc >> 8);
            ap(v, ":");
            aphex2(v, (pd.loc >> 3) & 31);
            ap(v, ".");
            apn(v, pd.loc & 7);
            ap(v, "  ");
            aphex(v, pd.vendor);
            ap(v, ":");
            aphex(v, pd.device);
            ap(v, "  ");
            for (n = 0; n < sizeof(vendors) / sizeof(vendors[0]); n++)
                if (vendors[n].id == pd.vendor) {
                    ap(v, vendors[n].n);
                    ap(v, " ");
                    break;
                }
            ap(v, pci_kind(pd.cls, pd.sub));
            if (pd.irq && pd.irq < 16) {
                ap(v, ", IRQ ");
                apn(v, pd.irq);
            }
            line(v, 0);
        }
        break;
    case 1:
        v[0] = 0;
        apn(v, si.conv_kb);
        ap(v, " KB");
        line("Konventionell:", v);
        v[0] = 0;
        apn(v, ext_kb());
        ap(v, " KB");
        line("Erweitert (ab 1 MB):", v);
        v[0] = 0;
        apn(v, mi.total_kb32 ? mi.total_kb32 : mi.total_kb);
        ap(v, " KB");
        line("Verwaltet:", v);
        v[0] = 0;
        apn(v, mi.total_kb32 ? mi.free_kb32 : mi.free_kb);
        ap(v, " KB");
        line("Frei:", v);
        v[0] = 0;
        apn(v, si.kernel_kb);
        ap(v, " KB");
        line("Kernel:", v);
        v[0] = 0;
        apn(v, mi.apps_kb + mi.alloc_kb);
        ap(v, " KB");
        line("Programme:", v);
        line("EMS/XMS:", "nicht n\x94tig (Protected Mode)");
        break;
    case 2:
        line("Grafikkarte:", si.vga ? "VGA" : "keine VGA (EGA/CGA/MDA)");
        line("Video-BIOS:", si.vbios_name[0] ? si.vbios_name : "(kein Text gefunden)");
        v[0] = 0;
        apn(v, rows);
        line("Textzeilen:", v);
        line("Modi (FAPI):", si.vga ? "Text 25/43/50, 13h, 12h, Mode X" : "Text 25");
        break;
    case 3:
        v[0] = 0;
        n = DosGetVersion();
        apn(v, n >> 8);
        ap(v, ".");
        if ((n & 0xFF) < 10)
            ap(v, "0");
        apn(v, n & 0xFF);
        line("Betriebssystem:", "AT-DOS (alternative Zeitlinie)");
        line("FAPI-Version:", v);
        line("Betriebsart:", "16-Bit Protected Mode, kooperatives Multitasking");
        if (!FAPI_FAILED(DosGetEnv("COMSPEC", v, sizeof(v))))
            line("COMSPEC:", v);
        if (!FAPI_FAILED(DosGetEnv("PATH", v, sizeof(v))))
            line("PATH:", v);
        break;
    case 4:
        if (!nbtn) {
            line("Maus:", "keine (DEVICE=MOUSE.SYS in der CONFIG.SYS?)");
            break;
        }
        v[0] = 0;
        apn(v, nbtn);
        line("Tasten:", v);
        line("Treiber:", "MOUSE.SYS (PS/2, seriell oder Busmaus)");
        break;
    case 5:
        for (d = 0; d < 26; d++) {
            if (DosQFSInfo(d, &fi))
                continue;
            v[0] = (char)('A' + d);
            v[1] = ':';
            v[2] = 0;
            l = line(v, 0);
            ap(l, "  ");
            if (d < 2 && (fi.flags & FSI_REMOVABLE))
                ap(l, "Diskette ");
            else if (fi.flags & FSI_READONLY)
                ap(l, "CD-ROM ");
            else
                ap(l, "Festplatte ");
            if ((fi.flags & FSI_NOMEDIA) ||
                ((fi.sectors == 0 || fi.sectors > 0x7FFFFFFFUL) && fi.fsname[0] == 'R' &&
                 fi.fsname[1] == 'A' && fi.fsname[2] == 'W')) {
                ap(l, "(kein Datentr\x84ger)");
                continue;
            }
            ap(l, fi.fsname);
            if (fi.sectors == 0 || fi.sectors > 0x7FFFFFFFUL)
                continue;                   /* Groesse unbekannt (CD)    */
            ap(l, ", ");
            apn(l, fi.bsize == 2048 ? fi.sectors << 1 : fi.sectors >> 1);
            ap(l, " KB");
        }
        for (d = 0; d < 2; d++)
            if (si.fdtype[d]) {
                v[0] = 0;
                ap(v, d ? "Laufwerk B: " : "Laufwerk A: ");
                ap(v, fdname(si.fdtype[d]));
                line(v, 0);
            }
        for (d = 0; d < 8 && DosDiskInfo(d, &di) == 0; d++) {
            v[0] = 0;
            ap(v, "Platte ");
            apn(v, d + 1);
            ap(v, ": ");
            apn(v, di.sectors >> 11);
            ap(v, " MB, ");
            ap(v, di.driver);
            if (di.heads) {
                ap(v, ", BIOS ");
                apn(v, di.heads);
                ap(v, "/");
                apn(v, di.spt);
            }
            line(v, 0);
        }
        break;
    case 6:
    case 7:
        n = k == 6 ? 3 : 4;
        for (i = 0; i < n; i++) {
            v[0] = 0;
            ap(v, k == 6 ? "LPT" : "COM");
            apn(v, i + 1);
            ap(v, ":");
            l = line(v, 0);
            d = k == 6 ? si.lpt[i] : si.com[i];
            if (d) {
                ap(l, "  ");
                aphex(l, d);
                ap(l, "h");
            } else
                ap(l, "  nicht vorhanden");
        }
        line(k == 6 ? "Drucken im Hintergrund: PRINT Datei" :
                      "Serielle Maus: MOUSE.SYS /COM1 bzw. /COM2", 0);
        break;
    case 8:
        for (i = 0; i < 16; i++) {
            static const char *const use[16] = {
                "Zeitgeber (Kernel)", "Tastatur (Kernel)", "Kaskade zum 2. PIC",
                "COM2/COM4", "COM1/COM3", "LPT2 / Soundkarte", "Diskette (Kernel)",
                "LPT1", "Echtzeituhr", "umgeleitet IRQ 2", "frei/Karte",
                "frei/Karte (Adaptec)", "PS/2-Maus", "Koprozessor", "IDE prim\x84r",
                "IDE sekund\x84r" };
            v[0] = 0;
            ap(v, "IRQ ");
            if (i < 10)
                ap(v, " ");
            apn(v, i);
            l = line(v, 0);
            ap(l, "  ");
            ap(l, (si.irq_used >> i) & 1 || i == 2 || (i == 13 && si.fpu) ? "belegt  " : "frei    ");
            for (n = 0; DosBusInfo(BUS_RES, n, &rs) == 0; n++)
                if (rs.type == 3 && rs.start == i)
                    break;
            ap(l, DosBusInfo(BUS_RES, n, &rs) == 0 ? rs.owner : use[i]);
        }
        break;
    case 9:
        for (i = 0, n = 0; i < 6; i++)
            if (si.drivers[i][0]) {
            {
                unsigned e = slen(si.drivers[i]);
                line(si.drivers[i], e > 4 && si.drivers[i][e - 3] == 'I' && si.drivers[i][e - 2] == 'F' ?
                     "Dateisystem (IFS=)" : "Ger\x84tetreiber (DEVICE=)");
                n++;
                v[0] = 0;
                for (d = 0; DosBusInfo(BUS_RES, d, &rs) == 0; d++) {
                    if (!same_name(rs.owner, si.drivers[i]))
                        continue;
                    ap(v, v[0] ? ", " : "  ");
                    if (rs.type == 1) {
                        aphex(v, rs.start);
                        if (rs.len > 1) {
                            ap(v, "-");
                            aphex(v, rs.start + rs.len - 1);
                        }
                        ap(v, "h");
                    } else {
                        ap(v, rs.type == 2 ? "DMA " : "IRQ ");
                        apn(v, rs.start);
                    }
                }
                if (v[0])
                    line(v, 0);
            }
            }
        if (!n)
            line("Keine Treiber geladen.", 0);
        line("Fest im Kernel:", "Diskette, Tastatur, Bildschirm, FAT12/16");
        break;
    }
}

/* Kurzangabe neben dem Knopf */
static void summary(void)
{
    unsigned i, d;
    for (i = 0; i < NB; i++)
        sum[i][0] = 0;
    ap(sum[0], si.cpu == 3 ? "80386+" : "80286");
    if (si.fpu)
        ap(sum[0], si.fpu == 1 ? ", 80287" : si.cpu == 3 ? ", 80387" : ", 80287XL");
    apn(sum[1], si.conv_kb);
    ap(sum[1], "K + ");
    apn(sum[1], ext_kb());
    ap(sum[1], "K");
    ap(sum[2], si.vga ? "VGA" : "keine VGA");
    if (si.vbios_name[0]) {
        ap(sum[2], ", ");
        for (i = 0, d = slen(sum[2]); si.vbios_name[i] && d < 18; i++, d++)
            sum[2][d] = si.vbios_name[i];
        sum[2][d] = 0;
    }
    ap(sum[3], "AT-DOS 6.00");
    if (nbtn) {
        apn(sum[4], nbtn);
        ap(sum[4], " Tasten");
    } else
        ap(sum[4], "keine");
    for (d = 0; d < 26 && slen(sum[5]) < 16; d++)
        if (DosQFSInfo(d, &fi) == 0) {
            char x[4];
            x[0] = (char)('A' + d);
            x[1] = ':';
            x[2] = ' ';
            x[3] = 0;
            ap(sum[5], x);
        }
    for (i = 0, d = 0; i < 3; i++)
        d += si.lpt[i] != 0;
    apn(sum[6], d);
    for (i = 0, d = 0; i < 4; i++)
        d += si.com[i] != 0;
    apn(sum[7], d);
    for (i = 0, d = 0; i < 16; i++)
        d += (si.irq_used >> i) & 1;
    apn(sum[8], d);
    ap(sum[8], " freigegeben");
    for (i = 0, d = 0; i < 6; i++)
        d += si.drivers[i][0] != 0;
    apn(sum[9], d);
}

/* ---- Bildschirm ------------------------------------------------------- */
#define A_BG    0x17                    /* weiss auf blau                 */
#define A_BTN   0x70
#define A_SEL   0x1F
#define A_KEY   0x74

static void fill(unsigned r, unsigned a)
{
    unsigned i;
    for (i = 0; i < 80; i++)
        row[i] = ' ';
    row[80] = 0;
    VioWrtStrAtt(r, 0, row, a);
}

static void put_at(unsigned r, unsigned c, const char *s, unsigned a)
{
    VioWrtStrAtt(r, c, (char *)s, a);
}

static unsigned brow(unsigned i) { return 3 + (i % 5) * 4; }
static unsigned bcol(unsigned i) { return i < 5 ? 2 : 42; }

static void draw_main(int sel)
{
    unsigned i, r, c;
    char k[2];
    for (r = 0; r < rows; r++)
        fill(r, A_BG);
    fill(0, 0x70);
    put_at(0, 26, "MSD - AT-DOS Systemdiagnose", 0x70);
    for (i = 0; i < NB; i++) {
        r = brow(i);
        c = bcol(i);
        for (k[1] = 0; k[1] < 19; k[1]++)
            row[(unsigned)k[1]] = ' ';
        row[19] = 0;
        put_at(r, c, row, i == (unsigned)sel ? A_SEL : A_BTN);
        put_at(r, c + 1, bname[i], i == (unsigned)sel ? A_SEL : A_BTN);
        k[0] = bkey[i];
        k[1] = 0;
        for (c = 0; bname[i][c] && up(bname[i][c]) != bkey[i]; c++)
            ;
        put_at(r, bcol(i) + 1 + c, k, i == (unsigned)sel ? 0x1E : A_KEY);
        sum[i][18] = 0;                     /* passt vor die 2. Spalte  */
        put_at(r, bcol(i) + 20, sum[i], A_BG);
    }
    fill(rows - 1, 0x70);
    put_at(rows - 1, 1, "Buchstabe, Pfeile + Eingabe oder Maus w\x84hlen  -  ESC oder F3 beendet", 0x70);
}

static void draw_box(unsigned k)
{
    unsigned top, h, i, n;
    h = nl + 4;
    top = (rows - h) / 2;
    for (i = 0; i < h; i++) {
        for (n = 0; n < LW + 4; n++)
            row[n] = ' ';
        row[LW + 4] = 0;
        if (i == 0 || i == h - 1)
            for (n = 0; n < LW + 4; n++)
                row[n] = '\xC4';
        row[0] = i == 0 ? '\xDA' : i == h - 1 ? '\xC0' : '\xB3';
        row[LW + 3] = i == 0 ? '\xBF' : i == h - 1 ? '\xD9' : '\xB3';
        put_at(top + i, 5, row, 0x70);
    }
    put_at(top, 7, " ", 0x70);
    put_at(top, 8, bname[k], 0x70);
    for (i = 0; i < nl; i++)
        put_at(top + 2 + i, 7, lines[i], 0x70);
    put_at(top + h - 1, 5 + LW / 2 - 2, " OK ", 0x0F);
}

/* Taste oder Mausklick -> Knopf (0..NB-1), -2 = Ende, -1 = nichts */
static int input(int *sel)
{
    unsigned k, c, i;
    for (;;) {
        if (KbdPeek()) {
            k = KbdCharIn();
            c = (unsigned)up((char)(k & 0xFF));
            if (c == 27 || (k >> 8) == 0x3D)        /* ESC, F3          */
                return -2;
            if (c == 13)
                return *sel;
            for (i = 0; i < NB; i++)
                if (c == (unsigned)bkey[i])
                    return (int)i;
            if ((k & 0xFF) == 0 || (k & 0xFF) == 0xE0) {
                c = k >> 8;
                if (c == 0x48 && *sel % 5) (*sel)--;          /* hoch    */
                else if (c == 0x50 && *sel % 5 < 4) (*sel)++;  /* runter  */
                else if (c == 0x4B && *sel >= 5) *sel -= 5;    /* links   */
                else if (c == 0x4D && *sel < 5) *sel += 5;     /* rechts  */
                return -1;
            }
            continue;
        }
        if (nbtn && MouReadEvent(&mev, 0) == 0) {
            if (!(mev.buttons & MOU_LEFT))
                continue;
            for (i = 0; i < NB; i++)
                if (mev.row == brow(i) && mev.col >= bcol(i) && mev.col < bcol(i) + 19) {
                    *sel = (int)i;
                    return (int)i;
                }
            return -3;                              /* Klick daneben   */
        }
        DosSleep(30);
    }
}

static void report(void)
{
    unsigned k, i;
    puts("MSD - AT-DOS Systemdiagnose\r\n");
    for (k = 0; k < NB; k++) {
        puts("\r\n---- ");
        puts(bname[k]);
        puts(" ----\r\n");
        detail(k);
        for (i = 0; i < nl; i++) {
            puts(lines[i]);
            puts("\r\n");
        }
    }
}

void main(void)
{
    int sel = 0, r;

    DosGetArgs(args);
    if (args[0] && (skip(args)[0] == '/') && skip(args)[1] == '?') {
        puts("Zeigt Angaben \x81" "ber den Rechner und das System.\r\n\r\n"
             "MSD [/S]\r\n\r\n  /S  Bericht auf die Standardausgabe\r\n");
        return;
    }
    if (DosSysInfo(&si)) {
        puts("DosSysInfo fehlt im Kernel.\r\n");
        DosExit(1);
    }
    DosMemInfo(&mi);
    rows = VioGetRows();
    if (skip(args)[0] == '/' && up(skip(args)[1]) == 'S') {
        report();
        return;
    }
    nbtn = MouOpen();
    summary();
    rows = VioSetMode(1);
    if (FAPI_FAILED(rows) || rows < 25)
        rows = 25;
    if (nbtn)
        MouShowPtr(1);
    VioSetCurPos(0xFF, 0);                  /* Cursor aus                */
    for (;;) {
        draw_main(sel);
        r = input(&sel);
        if (r == -2)
            break;
        if (r < 0)
            continue;
        detail((unsigned)r);
        draw_box((unsigned)r);
        do                                  /* Taste oder Klick schliesst */
            r = input(&sel);
        while (r == -1);
        if (r == -2)
            r = 0;                          /* ESC nur Fenster zu        */
    }
    if (nbtn)
        MouShowPtr(0);
    VioSetMode(0);
}
