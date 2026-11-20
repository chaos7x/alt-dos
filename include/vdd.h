/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Module fuer die DOS-Box (nach dem Vorbild der VDDs von
 * OS/2 2.0, Entwurf docs/VDD.md)
 *
 * Ein Modul ist ein gewoehnlicher Ring-0-Treiber (Kopf "DRV1", geladen mit
 * DEVICE= oder DEVLOAD), der sich in INIT mit DH_VDDREG anmeldet. Der
 * Monitor der Box (v86.c) behandelt erst seine eigenen Faelle (Basis wie
 * die Basis-VDDs von OS/2: PIC, Zeitgeber, CMOS, Tastatur, BIOS-Grundlage)
 * und fragt dann die Module. Aufrufe gehen fern an die Einspruenge des
 * Moduls (fent0..4 aus dcrt0.asm) mit DS = Moduldaten, ES:BX = struct
 * vdd_call; Rueckgabe in AX.
 */
#ifndef VDD_H
#define VDD_H

#define DH_VDDREG       32          /* BX = struct vdd_reg (DS) -> 0 ok   */
#define DH_VDDSVC       33          /* BX = struct vdd_svc (DS) -> AX     */

#define VDD_VERSION     1
#define NVDD            4           /* Module gleichzeitig               */
#define VDD_NTASK       8           /* Boxen hoechstens (= NTASK, vdd.c prueft) */
#define VDD_NINT        4           /* Interrupts je Modul               */

/* Anmeldung */
struct vdd_reg {
    unsigned short version;         /* VDD_VERSION                       */
    unsigned short size;            /* sizeof(struct vdd_reg)            */
    char           name[9];         /* "VCOM"                            */
    unsigned char  flags;           /* VDDF_*                            */
    unsigned short io_off;          /* Ports: VDD_IN/VDD_OUT, 0 = keine   */
    unsigned short int_off;         /* Interrupts aus ints[]             */
    unsigned short ev_off;          /* Ereignisse VDD_EV_*               */
    unsigned char  ints[VDD_NINT];  /* Vektoren (0 = frei)               */
    unsigned short tick_off;        /* Takt (VS_TICK), 0 = keiner         */
};
#define VDDF_ANYPORT    0x01        /* io_off fuer jeden Port, den der
                                       Kernel nicht selbst behandelt (das
                                       Modul prueft und meldet VDD_NOTMINE) */

/* Aufruf des Kernels an das Modul */
struct vdd_call {
    unsigned short fn;              /* VDD_IN, VDD_OUT, VDD_INT, VDD_EV_* */
    unsigned short task;            /* Box (Task)                        */
    unsigned short port;            /* IN/OUT: Port; INT: Vektor         */
    unsigned short val;             /* OUT: Wert; Rueckgabe IN: Wert     */
    unsigned short ax, bx, cx, dx;  /* INT: Register der Box (ein/aus)   */
    unsigned short si, di, ds, es;
    unsigned short flags;           /* INT: FLAGS der Box (CF = Bit 0);
                                       IN/OUT: Bit 0 = Box vorn          */
    unsigned short cs, ip, ss, sp;  /* INT: Ruecksprung bzw. Stapel der
                                       Box (ein/aus; EMS 55h/56h)        */
};
#define VDD_IN          1           /* -> AX = Wert, VDD_NOTMINE        */
#define VDD_OUT         2           /* -> 0, VDD_NOTMINE                */
#define VDD_INT         3           /* -> 0 erledigt (Register in der
                                          Struktur), VDD_NOTMINE weiter  */
#define VDD_INW         4           /* Port ueber 3FFh als Wort (EMU8000) */
#define VDD_OUTW        5
#define VDD_TICK        6           /* Takt: mit IF an, im Zusammenhang
                                       der Box (current), vor der Rueckkehr */
#define VDD_EV_DESTROY  0x10        /* Box endet (Handles schliessen)    */
#define VDD_EV_PARK     0x11        /* Box wird geparkt (Programmende)   */
#define VDD_EV_CREATE   0x12        /* Box angelegt (Zustand zuruecksetzen) */
#define VDD_EV_BG       0x13        /* Box nicht mehr vorn: Hardware, die
                                       das Modul ihr gab (VS_FGRES), zurueck */
#define VDD_EV_RESUME   0x14        /* Box war eingefroren und laeuft
                                       weiter (Pause nicht nachholen)    */
#define VDD_EV_PMHOOK   0x15        /* DPMI 0205h: val = Vektor          */
#define VDD_EV_PMEND    0x16        /* DPMI-Programm zu Ende             */
#define VDD_NOTMINE     0xFFFF

/* Dienste des Kernels fuer das Modul (DH_VDDSVC) */
struct vdd_svc {
    unsigned short fn;              /* VS_*                              */
    unsigned short task;
    unsigned long  lin;             /* VS_RD/VS_WR: Adresse in der Box   */
    unsigned short ffn;             /* VS_FAPI: FAPI-Funktion            */
    unsigned short a1, a2, a3;      /* VS_FAPI: Argumente (BX, CX, DX)   */
    unsigned short buf;             /* Puffer im DS des Moduls           */
    unsigned short len;             /* seine Laenge (VS_FAPI hoechstens  */
                                    /* VDD_FBUF)                         */
    unsigned short pa;              /* VS_FAPI: welches Argument (1-3)
                                       auf den Puffer zeigt, 0 = keins  */
    unsigned long  lin2;            /* VS_COPY: Ziel                     */
    unsigned short ty1, ty2;        /* VS_COPY: Art von lin/lin2 (VA_*)  */
};
#define VA_BOX          0           /* lineare Adresse der Box (< 4 MB)  */
#define VA_PHYS         1           /* physische Adresse (Seiten aus VS_PGALLOC) */
#define VA_MOD          2           /* Offset im DS des Moduls           */
#define VS_RD           1           /* len Byte Box-Speicher -> buf      */
#define VS_WR           2           /* buf -> Box-Speicher               */
#define VS_FAPI         3           /* FAPI ffn(a1..a3) im Namen der Box, mit
                                       Kernelsperre; Zeiger-Argument =
                                       buf (len Byte, kopiert hin und
                                       zurueck) -> AX wie die FAPI      */
#define VDD_FBUF        128
/* nur im Zusammenhang der Box (task = current; Ports, Takt):          */
#define VS_IRQ          4           /* a1 = Leitung 2-7: virtueller IRQ  ->
                                       0, 1 = ruht (kein Handler, verworfen) */
#define VS_VECDEF       5           /* -> Bit n: Vektor von IRQ n (0-7)
                                       steht im Real Mode auf dem Stummel;
                                       Bit 8+n: im PM eingehaengt (DPMI) */
#define VS_TICK         6           /* a1 = 0 aus, 1 je Takt, 2 je Takt und
                                       beim naechsten Ruecksprung        */
#define VS_IOPERM       7           /* Ports a1..a1+a2-1 (unter 3E0h) in der
                                       I/O-Bitmap: a3 = 1 frei, 0 Falle  */
#define VS_FGRES        8           /* Modul gab der Box Hardware: beim
                                       Wechsel nach hinten kommt VDD_EV_BG */
#define VS_AUD          9           /* Audio-FAPI ffn(a1, a2), Puffer im DS
                                       des Moduls (a1 Offset) -> AX; ffn
                                       FFh/FEh: Box nimmt/gibt den OPL,
                                       FAPI-Programme mit FM (auch ohne
                                       current) */
#define VS_HK           10          /* -> Schalter aus DOSBOX.INI (HK_*)  */
/* Speicher (wie VDHAllocPages/VDHMapPages bei OS/2):                  */
#define VS_PGALLOC      11          /* a1 Seiten am Stueck (4 KB) -> lin =
                                       erste Seitennummer, AX 0/FFFFh;
                                       zaehlt als Speicher der Boxen     */
#define VS_PGFREE       12          /* lin = erste Seite, a1 Seiten       */
#define VS_MAP          13          /* Box-Seiten a1..a1+a2-1 (nur C0h-EFh,
                                       nie eigene Seiten der Box) -> Seiten
                                       ab lin, lin = 0: Ersatzseite       */
#define VS_COPY         14          /* len Byte lin (ty1) -> lin2 (ty2)   */
#define VS_INFO         15          /* a1 = VI_*                          */
#define VI_EMSFRAME     0           /* -> erste Seite des EMS-Rahmens, 0 */
#define VI_EMSLIM       1           /* -> EMS_MEMORY_LIMIT (16 KB) + 1, 0 */
#define VI_FREE16       2           /* -> verfuegbarer Speicher in 16 KB  */
#define VDD_HK_ADLIB    16          /* ADLIB_COPY=ON                     */

#endif
