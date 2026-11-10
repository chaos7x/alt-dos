/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FAPI - Family API, Aufrufschnittstelle (AT-DOS 5.0, alternative Zeitlinie)
 *
 * Anwendungen laufen in Ring 3 und haben keinen Zugriff auf Hardware,
 * I/O-Ports oder fremden Speicher. Der einzige Weg nach unten ist das
 * FAPI-Call-Gate im GDT:
 *
 *     call far FAPI_GATE:0000
 *
 *     AX = Funktionsnummer, BX/CX/DX = Argumente, Rueckgabe in AX
 *     Zeiger-Argumente sind near und beziehen sich auf das DS des Aufrufers.
 */
#ifndef FAPI_ABI_H
#define FAPI_ABI_H

#define FAPI_GATE           0x23    /* GDT-Eintrag 4, RPL 3            */

#define FAPI_GETVERSION     0x00    /* -> AX = 0x0500 (Major<<8|Minor)  */
#define FAPI_VIOWRTSTR      0x01    /* BX = String, CX = Zeile<<8|Spalte */
#define FAPI_YIELD          0x02    /* CPU kooperativ abgeben           */
#define FAPI_EXIT           0x03    /* Task beenden, BX = Rueckgabewert (0-255) */
#define FAPI_SLEEP          0x04    /* BX = Millisekunden               */
#define FAPI_MEMINFO        0x05    /* BX = Puffer fuer struct fapi_meminfo */
#define FAPI_ALLOCSEG       0x06    /* BX = Groesse (0 = 64 KB) -> Selektor */
#define FAPI_FREESEG        0x07    /* BX = Selektor                    */
#define FAPI_KBDCHARIN      0x08    /* -> AX = Zeichen | Scancode << 8  */
#define FAPI_KBDPEEK        0x09    /* wie KBDCHARIN, wartet nicht, 0 = nichts */
#define FAPI_VIOWRTTTY      0x0A    /* BX = Text, CX = Laenge           */
#define FAPI_VIOCLS         0x0B    /* Bildschirm der Sitzung loeschen  */
#define FAPI_EXECPGM        0x0C    /* BX = Befehlszeile, CX = EXEC_*   */
#define FAPI_GETARGS        0x0D    /* BX = Puffer (FAPI_ARGLEN Bytes)  */
#define FAPI_OPEN           0x0E    /* BX = Dateiname, CX = OPEN_* -> Handle */
#define OPEN_READ           0       /* nur lesen (alle alten Programme) */
#define OPEN_RW             2       /* lesen und schreiben, Datei muss da sein,
                                       allein offen; Position 0, nicht gekuerzt */
#define FAPI_READ           0x0F    /* BX = Handle, CX = Puffer, DX = Laenge -> Bytes */
#define FAPI_CLOSE          0x10    /* BX = Handle                      */
#define FAPI_FINDENTRY      0x11    /* BX = struct fapi_dirent, CX = Nummer,
                                       DX = Verzeichnis (0 = aktuelles)  */
#define FAPI_KBDSETLAYOUT   0x12    /* BX = .KBL-Daten (278 Bytes), 0 = eingebaut */
#define FAPI_KBDGETLAYOUT   0x13    /* BX = Puffer (4 Bytes) fuer die Kennung;
                                       CX >= KBL_BYTES: die ganze aktive Belegung
                                       im .KBL-Format (5.1, fuer die Box) */
#define KBL_BYTES           278     /* "KBL" 01h, Kennung, 3 x 90 Zeichen */
#define KBL_SIZE_BYTES      278
#define FAPI_CHDIR          0x14    /* BX = Pfad                        */
#define FAPI_QCURDIR        0x15    /* BX = Puffer -> "C:\DOS"; 130 Bytes, mit
                                       FX_FLAG_LONGNAMES bis 263 (FAPI_PATHLEN+2);
                                       CX = Laufwerk + 1 (0 = aktuelles): wie
                                       MS-DOS merkt sich jeder Task das Ver-
                                       zeichnis von bis zu 4 weiteren Laufwerken
                                       (DosChDir "A:\X" von C: aus) */
#define FAPI_SELECTDISK     0x16    /* BX = Laufwerk (0 = A:)           */
#define FAPI_QCURDISK       0x17    /* -> AX = Laufwerk (0 = A:)        */
#define FAPI_CREATE         0x18    /* BX = Dateiname, CX = 0 neu/kuerzen, 1 anhaengen -> Handle */
#define FAPI_WRITE          0x19    /* BX = Handle, CX = Puffer, DX = Laenge -> Bytes */
#define FAPI_DELETE         0x1A    /* BX = Dateiname                   */
#define FAPI_MKDIR          0x1B    /* BX = Pfad                        */
#define FAPI_RMDIR          0x1C    /* BX = Pfad (Verzeichnis muss leer sein) */
#define FAPI_MOVE           0x1D    /* BX = alter Pfad, CX = neuer Pfad (gleiches Laufwerk) */
/* Signale wie OS/2 (DosSetSigHandler). Der Kernel stellt ein Signal beim
   naechsten Ruecksprung aus der FAPI zu: Er springt in die Weiche (BX, ein
   Offset im eigenen Codesegment, siehe DosSetSigHandler in apps/fapi.h)
   mit AX = Signal; die Weiche ruft den Handler und endet mit DosSigReturn,
   danach laeuft das Programm weiter, als sei der unterbrochene Aufruf
   normal zurueckgekehrt. Wartende Aufrufe (KbdCharIn, DosSleep) kehren
   dafuer vorzeitig zurueck (KbdCharIn mit 0). */
#define FAPI_SETSIGHANDLER  0x20    /* BX = Weiche (0 = abmelden), CX = Maske SIG_* */
#define FAPI_SIGRETURN      0x21    /* Handler fertig                   */
#define SIG_BREAK           0x01    /* Strg+C                           */
#define SIG_SHUTDOWN        0x02    /* SHUTDOWN / Strg+Alt+Entf: aufraeumen, dann DosExit */
#define FAPI_GETENV         0x22    /* BX = Name, CX = Puffer, DX = Laenge -> Laenge des Werts */
#define FAPI_SETENV         0x23    /* BX = "NAME=WERT" (leerer Wert loescht) */
#define FAPI_ENVENTRY       0x24    /* BX = Nummer, CX = Puffer, DX = Laenge -> n-ter Eintrag */
#define FAPI_GETDATETIME    0x25    /* BX = Puffer (4 Bytes): Datum, Uhrzeit im FAT-Format */
#define FAPI_DUP2           0x26    /* BX = Handle (FFFFh = Konsole), CX = Standard-Handle 0-2 */
#define FAPI_DUP            0x27    /* BX = Handle -> neues Handle auf dasselbe */
#define FAPI_PIPE           0x28    /* BX = Puffer (2 Worte) -> Lese-, Schreib-Handle */
#define FAPI_VIOWRTSTRATT   0x29    /* wie VioWrtStr, DX = Farbattribut */
#define FAPI_QHANDTYPE      0x2A    /* BX = Handle -> HT_*              */
#define HT_FILE             0
#define HT_DRIVE            3       /* DosOpenDrive                     */
#define HT_DEVICE           4       /* PRN, LPT1-3, NUL                 */
/* Laufwerk als Ganzes (wie DASD bei OS/2), fuer CHKDSK, FORMAT, LABEL.
   Lesen/Schreiben mit DosRead/DosWrite in ganzen Sektoren ab der Position
   (DosSeek, Bytes, Vielfaches der Sektorgroesse), Sektor 0 = Bootsektor
   der Partition. Exklusiv geht nur, wenn keine Datei auf dem Laufwerk
   offen ist; solange gesperrt, lehnt das Dateisystem alle Zugriffe ab.
   Beim Schliessen wird das Laufwerk neu eingelesen. */
#define FAPI_OPENDRIVE      0x2B    /* BX = Laufwerk (0 = A:), CX = DRV_OPEN_* -> Handle */
#define DRV_OPEN_READ       0       /* nur lesen, ohne Sperre           */
#define DRV_OPEN_LOCK       1       /* exklusiv, lesen und schreiben    */
/* Ganze Festplatten (FDISK): Nummer 0, 1, ... zaehlt nur Platten mit
   512-Byte-Sektoren (keine Diskette, keine CD) */
#define FAPI_DISKINFO       0x43    /* BX = Platte, CX = struct fapi_diskinfo
                                       -> 0, FAPI_ERROR = keine solche Platte */
#define FAPI_OPENDISK       0x44    /* BX = Platte, CX = DRV_OPEN_* -> Handle;
                                       Sektoren ab 0 (MBR), mit Sperre nur,
                                       wenn auf keinem ihrer Laufwerke etwas
                                       offen ist; alle bleiben bis zum
                                       Schliessen gesperrt */
struct fapi_diskinfo {
    unsigned long  sectors;
    unsigned short bsize;
    char           driver[13];      /* "IDE.SYS"                        */
    unsigned char  unit;            /* Einheit des Treibers             */
    unsigned char  nvol;            /* Laufwerksbuchstaben darauf       */
    unsigned char  rsv;
    unsigned short heads, spt;      /* Geometrie des BIOS (INT 13h/08h),
                                       0 = unbekannt (Platte ohne BIOS)  */
};
/* Diskettenspur formatieren (FORMAT, DISKCOPY): BX = Handle aus
   DosOpenDrive mit DRV_OPEN_LOCK auf einem Diskettenlaufwerk, CX = struct
   fapi_fmttrack. blocks waehlt das Format (720 = 360 KB, 1440 = 720 KB,
   2400 = 1,2 MB, 2880 = 1,44 MB, 5760 = 2,88 MB), es muss zum Laufwerk
   passen; danach gilt es fuer das Laufwerk (DosQFSInfo meldet es).
   -> 0, FAPI_ERR_WP, FAPI_ERROR (Format passt nicht / Fehler) */
#define FAPI_FMTTRACK       0x46
/* SUBST: BX = Laufwerk (0 = A:), CX = Pfad (bestehendes Verzeichnis) oder
   0 = aufheben -> 0, FAPI_ERROR, FAPI_ERR_INUSE (aktuelles Laufwerk eines
   Programms). DosQSubst: BX = Laufwerk, CX = Puffer (130 Byte) -> "C:\DOS" */
/* Schnittstellen (MODE): BX = COM 0-3 oder 10h|LPT 0-2, CX = struct
   fapi_comcfg, DX = 1 einstellen / 0 abfragen -> 0, FAPI_ERROR (kein
   Anschluss), FAPI_ERR_INUSE (Port gehoert einem Treiber, z. B. Maus) */
#define FAPI_COMCTL         0x4A
struct fapi_comcfg {
    unsigned short divisor;         /* COM: 115200 / Baud               */
    unsigned char  lcr;             /* COM: Datenbits-5 | Stopp<<2 | Paritaet (UART-LCR) */
    unsigned char  retry;           /* 1 = endlos warten (MODE ...,P)    */
    unsigned char  lsr, msr;        /* Status (LPT: Druckerstatus in lsr) */
    unsigned short base;            /* Portadresse                      */
};
/* Systemangaben fuer MSD: BX = struct fapi_sysinfo */
#define FAPI_SYSINFO        0x49
#define FAPI_RBOX           0x55    /* 5.1, Kompatibilitaetsbox: BX = RB_*;
                                       RB_LOAD: CX = Selektor, DX = Laenge eines
                                       .COM (bis FE00h) -> 0, FAPI_ERR_INUSE
                                       (Speicher unter 640 KB belegt), FAPI_ERROR;
                                       RB_BOOT: CX = Groesse eines Disketten-
                                       abbilds in KB (bis 2880) -> 0, Fehler wie
                                       RB_LOAD; RB_IMG: CX = Puffer, DX = Laenge
                                       (Vielfaches von 512) haengt an das Abbild an,
                                       das erste Stueck enthaelt den Bootsektor;
                                       RB_RUN: wartet, bis die eigene Sitzung vorn
                                       ist, laeuft im Real Mode -> RB_SWITCH (Alt+Esc),
                                       RB_EXIT (Programmende, Strg+Alt+Entf, INT E0h
                                       AH = 01h), RB_CALL (INT E0h, AH ungleich 01h:
                                       RBOX erledigt ihn ueber RB_REGS/RB_MEM, dann
                                       wieder RB_RUN); RB_FREE */
#define RB_LOAD             0
#define RB_RUN              1
#define RB_FREE             2
#define RB_BOOT             3
#define RB_IMG              4
#define RB_REGS             5       /* CX = struct fapi_rbregs, DX = 0 holen,
                                       1 setzen (AX..ES, FLAGS ohne IF/TF) */
#define RB_MEM              6       /* CX = struct fapi_rbmem: Speicher der
                                       Box lesen (write = 0) oder schreiben */
struct fapi_rbregs {                /* Zustand der Box bei RB_CALL      */
    unsigned short ax, bx, cx, dx, si, di, bp, ds, es, sp, ss, ip, cs, flags;
};
struct fapi_rbmem {
    unsigned long  lin;             /* linear in der Box (< 640 KB)     */
    unsigned short buf, len;        /* Puffer im eigenen DS             */
    unsigned short write;
};
#define RB_SWITCH           1
#define RB_EXIT             2
#define RB_CALL             3
#define FAPI_PROC           0x54    /* BX = PROC_*, CX = Tasknummer (0..), DX =
                                       struct fapi_proc (Taskmanager, wie 6.0) */
#define PROC_INFO           0       /* -> 0, FAPI_ERROR hinter dem letzten Platz */
#define PROC_KILL           1       /* Task beenden (wie Strg+C, aber ohne
                                       Rueckfrage an das Programm) -> 0,
                                       FAPI_ERR_DENIED (eigener Task, ein
                                       Aufrufer davon, Shell der ersten Sitzung),
                                       FAPI_ERROR (Platz frei) */
struct fapi_proc {
    unsigned char  state;           /* 0 frei, 1 laeuft, 2 wartet auf Kind,
                                       3 im Treiber                     */
    unsigned char  kind;            /* 0 FAPI-Programm, 1 DOS-Box, 2 geparkte
                                       DOS-Box (nur 6.0)                */
    unsigned char  session;         /* Sitzung (0 = erste)              */
    unsigned char  parent;          /* Aufrufer, FFh = keiner           */
    unsigned char  flags;           /* PROC_SELF, PROC_PROT             */
    unsigned char  rsv;
    unsigned short kb;              /* Programmabbild (Code + Daten) in KB */
    char           name[13];
    char           args[41];        /* Anfang der Befehlszeile          */
};
#define PROC_SELF           1       /* der aufrufende Task              */
#define PROC_PROT           2       /* nicht beendbar                   */
struct fapi_sysinfo {
    unsigned char  cpu;             /* 2 = 80286, 3 = 80386 oder neuer  */
    unsigned char  fpu;             /* Koprozessor: 0 keiner, 1 287,
                                       2 387-Kern (287XL, 387)          */
    unsigned char  vga;             /* VGA erkannt                      */
    unsigned char  model;           /* BIOS-Modellbyte F000:FFFE        */
    char           bios_date[9];    /* F000:FFF5, "MM/TT/JJ"            */
    unsigned char  fdtype[2];       /* Diskettenlaufwerke (CMOS-Typ)    */
    unsigned char  rsv;
    unsigned short equip;           /* INT 11h                          */
    unsigned short com[4], lpt[3];  /* Portadressen, 0 = keiner         */
    unsigned short conv_kb, ext_kb; /* Speicher laut BIOS               */
    unsigned short kernel_kb;       /* Kernel (Code + Daten)            */
    unsigned short irq_used;        /* Bit n: IRQ n freigegeben         */
    unsigned short uptime_s;
    char           bios_name[64];   /* laengster Text im System-BIOS    */
    char           vbios_name[64];  /* ... im Video-BIOS (C000:0000)    */
    char           drivers[6][13];  /* geladene Treiber (DEVICE=)       */
};
/* JOIN: BX = Laufwerk, CX = Pfad (leeres Verzeichnis, fehlt es, wird es
   angelegt; nicht der Stamm) oder 0 = aufheben -> 0, FAPI_ERROR,
   FAPI_ERR_INUSE (aktuelles Laufwerk eines Programms, offene Dateien,
   Ziel eines SUBST/JOIN), FAPI_ERR_DENIED (Verzeichnis nicht leer).
   DosQJoin: BX = Laufwerk, CX = Puffer (FAPI_CURDIRLEN) -> "C:\CDROM" */
/* ANSI-Folgen in der TTY-Ausgabe der Sitzung (wie OS/2 VioSetAnsi):
   BX = 1 an, 0 aus, 2 nur abfragen -> bisheriger Zustand (0/1) */
#define FAPI_VIOSETANSI     0x4E
/* Ton (Treiber z. B. SB16.SYS): BX = AUD_*. Mehrere Programme duerfen
   oeffnen; zu hoeren ist das Programm der Vordergrund-Sitzung, sonst das
   zuletzt geoeffnete. Die anderen laufen im Echtzeittempo weiter, ihre
   Daten werden verworfen. */
#define FAPI_AUDIO          0x4F
#define FAPI_QTICKS         0x50    /* -> Systemtakt (100 Hz), untere 16 Bit:
                                       Zeitmessung fuer Spieler, Spiele */
#define FAPI_NEWSIZE        0x52    /* BX = Handle (schreibend), CX = Zeiger auf neue
                                       Groesse (4 Bytes): kuerzen oder mit Nullen
                                       verlaengern (wie DosNewSize bei OS/2) */
#define AUD_OPEN            0       /* CX = struct fapi_audfmt -> 0, FAPI_ERROR
                                       (kein Geraet, Format geht nicht),
                                       FAPI_ERR_INUSE (alle Plaetze belegt) */
#define AUD_WRITE           1       /* CX = Puffer, DX = Laenge -> Bytes (wartet,
                                       bis sie im Puffer der Karte sind) */
#define AUD_DRAIN           2       /* warten, bis alles gespielt ist    */
#define AUD_CLOSE           3
#define AUD_MIXER           4       /* CX = AUDMIX_*, DX = 0-100 setzen,
                                       FFFFh nur abfragen -> Wert       */
#define AUD_INFO            5       /* CX = Puffer (64 Byte) <- Name der Karte */
#define AUD_CAPS            6       /* -> AUDCAP_* (was der Treiber kann)  */
/* FM-Synthese (OPL2/OPL3, AdLib-kompatibel): Register direkt, wie die
   Programme es von der AdLib kennen. Der Kernel merkt sich je Programm alle
   Register; wechselt der Vorrang, laedt er die des neuen Programms. */
#define AUD_FMOPEN          7       /* -> 0, FAPI_ERROR (kein OPL), FAPI_ERR_INUSE */
#define AUD_FMWRITE         8       /* CX = struct fapi_fmreg[], DX = Anzahl
                                       (hoechstens 512) -> 0               */
#define AUD_FMCLOSE         9
/* MIDI (MPU-401, UART-Betrieb): rohe MIDI-Bytes. Beim Wechsel des
   Vorrangs: alle Noten aus, Programm und Lautstaerke je Kanal neu. */
#define AUD_MIDIOPEN        10
#define AUD_MIDIWRITE       11      /* CX = Bytes, DX = Anzahl -> 0      */
#define AUD_MIDICLOSE       12
#define AUDCAP_PCM          0x01    /* digitaler Ton (AUD_OPEN)          */
#define AUDCAP_OPL2         0x02
#define AUDCAP_OPL3         0x04    /* 18 Stimmen, Register 100h-1FFh    */
#define AUDCAP_MPU401       0x08
struct fapi_fmreg {
    unsigned short reg;             /* 000h-0F5h, OPL3 auch 100h-1F5h    */
    unsigned char  val, rsv;
};
#define AUDMIX_MASTER       0
#define AUDMIX_WAVE         1
#define AUDMIX_FM           2
#define AUDMIX_CD           3
#define AUDMIX_LINE         4
#define AUDMIX_MIC          5
struct fapi_audfmt {
    unsigned short rate;            /* Abtastrate, z. B. 22050           */
    unsigned char  channels;        /* 1 mono, 2 stereo                  */
    unsigned char  bits;            /* 8 (ohne Vorzeichen), 16 (mit)     */
};
/* Platten-Cache (MEM): BX = struct fapi_cacheinfo, CX = Groesse (0 = 5.0) -> 0 */
#define FAPI_CACHEINFO      0x4D
struct fapi_cacheinfo {
    unsigned short kb;              /* Groesse, 0 = kein Cache          */
    unsigned short delay_s;         /* verzoegertes Schreiben, 0 = sofort */
    unsigned long  hits, misses;    /* Lesezugriffe aus dem Cache / von der Platte */
    unsigned short dirty;           /* Bloecke mit noch nicht geschriebenen Daten */
    unsigned short min_kb, max_kb;  /* CX = Groesse: dynamischer Cache (wie 6.0) */
};
#define CACHEINFO_V50 14                /* Groesse in 5.0 (CX = 0)          */
#define FAPI_JOIN           0x4B
#define FAPI_QJOIN          0x4C
#define FAPI_SUBST          0x47
#define FAPI_QSUBST         0x48
struct fapi_fmttrack {
    unsigned short cyl, head, blocks;
};
#define FAPI_SEEK           0x2C    /* BX = Handle, CX = Zeiger auf Position (4 Bytes) */
#define FAPI_QCHILDRC       0x2E    /* -> Rueckgabewert des zuletzt beendeten Kindes
                                       (ERRORLEVEL; 255 = abgebrochen) */
#define FAPI_SETFINFO       0x2F    /* BX = Pfad, CX = struct fapi_finfo */
struct fapi_finfo {
    unsigned short mask;            /* 1: Attribut, 2: Datum/Uhrzeit    */
    unsigned short attr;            /* 01h RO, 02h versteckt, 04h System, 20h Archiv */
    unsigned short date, time;      /* FAT-Format                       */
};
#define FAPI_SETDATETIME    0x30    /* BX = Puffer (4 Bytes): Datum, Uhrzeit (FAT) -> Echtzeituhr */
#define FAPI_LABEL          0x31    /* BX = Laufwerk, CX = struct fapi_label (Ausgabe),
                                       DX = neue Bezeichnung (0 = nur lesen, "" = loeschen) */
struct fapi_label {
    char           label[12];
    unsigned long  serial;          /* 0 = keine                        */
};
#define FAPI_VIOSETMODE     0x32    /* BX = 1 Vollbild (alle Zeilen, ohne Titel- und
                                       Statusleiste), 0 normal (2 weniger) -> Zeilenzahl;
                                       endet das Programm, gilt wieder normal */
#define FAPI_VIOSETCURPOS   0x33    /* BX = Zeile << 8 | Spalte, Zeile FFh = Cursor aus */
#define FAPI_KBDSHIFT       0x34    /* -> AX = KBDSHIFT_* (Zustand jetzt)  */
#define FAPI_VIOSETLINES    0x35    /* BX = Bildschirmzeilen 25/43/50 (nur VGA)
                                       der eigenen Sitzung, 0 = nur fragen;
                                       loescht den Bildschirm -> Zeilen */
#define FAPI_SHUTDOWNHOLD   0x37    /* nach SIG_SHUTDOWN: Herunterfahren aufhalten,
                                       weil das Programm den Benutzer fragt; seine
                                       Sitzung kommt nach vorn, der Kernel wartet
                                       bis zum Programmende (sonst 5 s) */
#define FAPI_VIOGETROWS     0x36    /* -> Zeilen, die das Programm beschreiben
                                       kann (normal Bildschirm - 2, Vollbild alle) */
/* Maus (wie das Mou-API von OS/2): Ereignisse gehen an die Sitzung im
   Vordergrund, wenn dort ein Programm MouOpen gerufen hat. Zeile/Spalte
   sind Textzellen relativ zur Sitzung (Zeile FFh: Titel-/Statusleiste). */
#define FAPI_MOUOPEN        0x38    /* -> Anzahl Tasten, 0 = keine Maus;
                                       Ereignisse fuer diese Sitzung an,
                                       Warteschlange leer, Zeiger aus */
#define FAPI_MOUREADEVENT   0x39    /* BX = struct fapi_mouevent, CX = MOU_WAIT
                                       (warten) | MOU_XY (8 Byte mit x, y)
                                       -> 0 Ereignis, 1 keins (oder Signal) */
#define MOU_WAIT            1
#define MOU_XY              2       /* x/y: Grafik in Bildpunkten (12h
                                       640x480, 13h 320x200), Text im
                                       virtuellen Raster (8 je Spalte) */
#define FAPI_MOUSHOWPTR     0x3A    /* BX = 1 Zeiger zeigen, 0 verbergen */
/* Zwischenablage des Kernels, fuer alle Sitzungen gemeinsam (auch die
   Konsolenmaus legt markierten Text hier ab). Zeilenende CR LF oder LF. */
#define FAPI_CLIPPUT        0x3B    /* BX = Selektor (0 = eigenes Datensegment),
                                       CX = Offset, DX = Laenge -> gespeicherte
                                       Laenge (hoechstens CLIP_MAX) */
#define FAPI_CLIPGET        0x3C    /* BX = Selektor (0 = eigenes Datensegment),
                                       CX = Offset, DX = Platz -> Laenge des
                                       Inhalts (kopiert hoechstens DX Bytes) */
#define CLIP_MAX            16384
/* Logischer Bildschirmpuffer (wie VioGetBuf/VioShowBuf bei OS/2 1.x):
   Selektor auf den Puffer der eigenen Sitzung, Zelle = Zeichen | Attribut
   << 8, Zeile r ab Offset r * 160 (Zeilen wie VioGetRows). Schreiben
   allein zeigt nichts; VioShowBuf meldet fertige Zeilen. */
#define FAPI_VIOGETBUF      0x3D    /* -> Selektor (FAPI_ERROR: LDT voll) */
#define FAPI_VIOSHOWBUF     0x3E    /* BX = erste, CX = letzte Zeile zeigen */
/* Grafik (nur VGA): die Sitzung schaltet in einen Grafikmodus; Text-
   ausgaben laufen weiter in den Textpuffer und erscheinen nach der Rueck-
   kehr. Beim Sitzungswechsel sichert der Kernel Bild und Palette und biegt
   den Selektor aus VioGetPhysBuf um - das Programm zeichnet weiter. */
#define FAPI_VIOSETGFX      0x3F    /* BX = 13h (320x200x256), 12h (640x480x16),
                                       VIO_MODEX (320x240x256, 2 Seiten),
                                       0 = Text */
#define VIO_MODEX           0x58
#define FAPI_VIOGETPHYSBUF  0x40    /* -> Selektor auf das Bild. 13h: 64 KB,
                                       Punkt (x,y) bei y * 320 + x. 12h: eine
                                       Ebene (VioSetPlane), 38400 Byte, Bit 7
                                       von Byte y * 80 + x / 8 = Punkt x */
#define FAPI_VIOSETPAL      0x41    /* BX = erster Eintrag, CX = Anzahl,
                                       DX = Puffer (je R, G, B, 0-63) */
#define FAPI_VIOSETPLANE    0x42    /* 12h: BX = Ebene 0-3 (Farbbit) zum
                                       Schreiben und Lesen; Mode X: Ebene
                                       x & 3, Punkt (x,y) bei Seite +
                                       y * 80 + x / 4, Seite 0 bei 0,
                                       Seite 1 bei 19200 */
#define FAPI_VIOSETPAGE     0x45    /* Mode X: BX = sichtbare Seite 0/1;
                                       wartet vorn auf den Strahlruecklauf */
struct fapi_mouevent {
    unsigned char  buttons;         /* MOU_* gedrueckt                  */
    unsigned char  row, col;        /* Zelle, row FFh = ausserhalb      */
    unsigned char  rsv;
    unsigned short x, y;            /* nur mit MOU_XY                   */
};
#define MOU_LEFT            0x01
#define MOU_RIGHT           0x02
#define MOU_MIDDLE          0x04
#define KBDSHIFT_SHIFT      0x01
#define KBDSHIFT_CTRL       0x02
#define KBDSHIFT_ALT        0x04    /* linke Alt-Taste                  */
#define KBDSHIFT_ALTGR      0x08
/* Tastencodes (KbdCharIn): Alt+Taste liefert wie das BIOS Zeichen 0 und den
   Scancode; Alt allein gedrueckt und losgelassen liefert KEY_ALT. */
#define KEY_ALT             0x3800
#define FAPI_QFSINFO        0x2D    /* BX = Laufwerk, CX = struct fapi_fsinfo */
struct fapi_fsinfo {
    char           fsname[10];      /* "FAT", "ISO9660" -> Pruefprogramm U<name>.EXE;
                                       "RAW" = nicht formatiert */
    unsigned long  sectors;         /* Sektoren des Laufwerks           */
    unsigned short bsize;           /* Bytes je Sektor (512, CD 2048)   */
    unsigned short flags;           /* FSI_*                            */
    unsigned long  hidden;          /* erster Sektor der Partition (BPB) */
    unsigned char  ptype;           /* Partitionstyp im MBR, 0 = keiner  */
    unsigned char  rsv;
};
#define FSI_REMOVABLE       0x01
#define FSI_READONLY        0x02
#define FSI_DIRTY           0x04    /* beim Einhaengen nicht sauber abgemeldet */
#define FSI_NOMEDIA         0x08    /* Disketten- oder CD-Laufwerk leer */
#define HT_PIPE             1
#define HT_CONSOLE          2
#define STDIN               0       /* Standard-Handles, Vorgabe Konsole */
#define STDOUT              1
#define STDERR              2
#define FAPI_ENVSIZE        1024    /* Umgebung je Programm (wie COMMAND /E:1024) */
#define FAPI_SHUTDOWN       0x1F    /* BX = 0 anhalten (ausschalten), 1 Neustart; kehrt nicht zurueck */
#define FAPI_FREESPACE      0x1E    /* BX = Laufwerk (0 = A:), CX = Puffer (4 Bytes) -> freie Bytes */

/* Fehler: alle Werte ab FAPI_ERR_FIRST. FAPI_ERROR heisst "nicht gefunden
   oder unzulaessig", die uebrigen sagen genauer, was schiefging. */
#define FAPI_ERROR          0xFFFF
#define FAPI_ERR_WP         0xFFFE  /* Datentraeger schreibgeschuetzt   */
#define FAPI_ERR_FULL       0xFFFD  /* Datentraeger oder Verzeichnis voll */
#define FAPI_ERR_EXIST      0xFFFC  /* Ziel gibt es schon               */
#define FAPI_ERR_DENIED     0xFFFB  /* Attribut verbietet es / nicht leer */
#define FAPI_ERR_INUSE      0xFFFA  /* Datei ist geoeffnet / Verzeichnis in Benutzung */
#define FAPI_ERR_NOTREADY   0xFFF9  /* Geraet nicht bereit (Drucker aus, Papierende) */
#define FAPI_ERR_FIRST      0xFFF0

#define FAPI_VERSION        0x0500
#define FAPI_ARGLEN         128     /* Argumente bis 127 Zeichen (wie MS-DOS) */
#define FAPI_PATHLEN        261     /* Pfade bis 260 Zeichen + Nullbyte */
#define FAPI_CURDIRLEN      263     /* DosQCurDir-Puffer (FAPI_PATHLEN + 2) */

/* DosExecPgm */
#define EXEC_SYNC           0       /* in dieser Sitzung, Aufrufer wartet */
#define EXEC_ASYNC          1       /* eigene Sitzung, laeuft nebenher    */
#define EXEC_BG             2       /* diese Sitzung, laeuft nebenher (Pipes) */
#define EXEC_ERR_NOTFOUND   1
#define EXEC_ERR_NOMEM      2
#define EXEC_ERR_FORMAT     3
#define EXEC_ERR_NOSLOT     4

/*
 * Speicher aus Sicht der FAPI: ein einziger Arbeitsspeicher. Die Trennung
 * in "konventionell" und "erweitert" ist ein Real-Mode-Artefakt und wird
 * nicht nach oben gereicht; das Loch 640 KB - 1 MB verwaltet der Kernel.
 */
struct fapi_meminfo {               /* alle Angaben in KB               */
    unsigned short total_kb;        /* Arbeitsspeicher insgesamt        */
    unsigned short free_kb;         /* frei                             */
    unsigned short kernel_kb;       /* vom Kernel reserviert            */
    unsigned short apps_kb;         /* fuer Programmabbilder reserviert */
    unsigned short alloc_kb;        /* per DosAllocSeg vergeben         */
};

struct fapi_dirent {                /* DosFindEntry                     */
    char           name[256];       /* Dateiname, auch fuer lange Namen */
    unsigned char  attr;            /* FAT-Attribute (10h = Verzeichnis) */
    unsigned long  size;
    unsigned short date, time;      /* FAT-Format                       */
};

#endif
