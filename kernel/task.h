/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS - Taskverwaltung: struct task und LDT-Zugriff, gemeinsam fuer
 * kernel.c und codeswap.c (KTEXT2). Vorher nur in kernel.c.
 */
#ifndef TASK_H
#define TASK_H

#define T_FREE  0
#define T_READY 1           /* laeuft oder schlaeft bis 'wake'           */
#define T_WAIT  2           /* wartet auf das Ende eines Kindprogramms   */
#define T_BLOCK 3           /* schlaeft in einem Treiber (DevHlp ProcBlock) */
struct task {
    unsigned char state;
    unsigned long wake;
    int           session;
    int           parent;   /* -1: Sitzungsinhaber                       */
    char          name[13];                       /* fuer die Titelzeile   */
    char          args[FAPI_ARGLEN];
    unsigned char drive;                          /* aktuelles Laufwerk    */
    char          cwd[IFS_PATHLEN];               /* "" = Stamm, "\DOS"   */
    pg_t          img_unit, code_unit;      /* Seiten: Daten (V1: alles), */
    unsigned      img_cnt, code_cnt;        /* V2: Code, geteilt         */
    unsigned long code_key, code_fsize;     /* gleiches Programm?        */
    unsigned      sig_tramp, sig_mask;          /* DosSetSigHandler      */
    unsigned char std[3];                       /* Eintraege fuer Handle 0-2 */
    unsigned char bg;                           /* EXEC_BG: weckt/schliesst nichts */
    unsigned char nocon;                        /* EXEC_DETACH: ohne Konsole (erbt) */
    unsigned char lfn;                          /* FX_FLAG_LONGNAMES         */
    unsigned char sig_pending, in_sig;
    unsigned short sig_save[15];                /* Rahmen + AX waehrend des Handlers */
    unsigned char rc;                           /* Rueckgabewert (DosExit)  */
    unsigned char child_rc;                     /* des zuletzt beendeten Kindes */
    unsigned char hold;                         /* haelt SHUTDOWN auf (fragt) */
    unsigned char want_lock;                    /* braucht die Kernelsperre zurueck */
    unsigned char lock_depth;                   /* ihre Tiefe beim Abgeben  */
    volatile unsigned char block_rc;            /* ProcBlock: 0 geweckt, 1 Frist */
    unsigned long block_id;                     /* ProcBlock: Ereignis       */
    unsigned long block_until;                  /* ProcBlock: Frist, 0 = keine */
    unsigned char v86;                          /* DOS-Box (v86.c)          */
    /* Phase 6, Stufe 3: Sperren je Laufwerk/Treiber */
    unsigned char vkey;                         /* gehaltene Laufwerkssperre + 1 */
    unsigned char eakey;                        /* haelt die EA-Sperre (VK_EA)  */
    unsigned char wait_k;                       /* wartet: Laufwerk + 1, 80h|Treiber */
    unsigned char may_rel;                      /* Treiberauftrag mit eigenem DMABUF:
                                                   ProcBlock gibt die Kernelsperre ab */
    unsigned char kill_req;                     /* beenden, sobald die Operation fertig */
    unsigned char lk_quiet;                     /* Sperre freiwillig abgegeben (EXEC,
                                                   Schlafen): kein Plattenzustand offen */
    unsigned char code_gone;                    /* Code verworfen (codeswap.c) */
    unsigned long cpu;                          /* Rechenzeit in Takten (PS/TOP) */
    unsigned long born;                         /* Startzeit in Takten      */
};

/* Tabelle der offenen Dateien (kernel.c; procfs.c liest mit) */
struct ofile {
    signed char     owner;  /* Task, die das Handle hat, oder -1         */
    unsigned char   refs;   /* Verweise (Handle + Standard-Handles), 0 = frei */
    unsigned char   kind;   /* OF_*                                      */
    unsigned char   pipe;   /* OF_PRD/OF_PWR: Nummer der Pipe            */
    struct ifs_file f;
    unsigned long   pos;
    char            path[IFS_PATHLEN];  /* kanonisch, fuer "in Benutzung" */
};
#define OF_FILE 0
#define OF_PRD  1           /* Leseende einer Pipe                      */
#define OF_PWR  2           /* Schreibende einer Pipe                   */
#define OF_CON  3           /* Konsole (Tastatur/Bildschirm der Sitzung) */
#define DASD_LBA  0x80      /* OF_DASD: f.flags, pos in Sektoren (DRV_OPEN_LBA) */
#define DASD_DISK 1         /* OF_DASD mit f.attr = DASD_DISK: ganze Platte,
                               f.drive = Blockgeraet (DosOpenDisk)      */
#define OF_DASD 5           /* Laufwerk als Ganzes (f.drive), pipe = gesperrt;
                               4 markiert eine schon geschlossene Datei      */
#define OF_LPT  6           /* Drucker PRN/LPT1-3 (f.drive = 0-2)        */
#define OF_NUL  7           /* NUL                                      */
#define OF_COM  8           /* COM1-4/AUX (f.drive = 0-3)               */
#define OF_SOCK 9           /* Socket (TCPIP.SYS), f.first = Nummer im Stapel */
#define HBASE   3           /* Handles 0-2: Standard, ab 3: eigene Dateien */
#define CONS    0           /* Eintrag 0 ist die Konsole                */
extern struct ofile __far *files;
extern unsigned nfiles;

/* geladene Treiber (kernel.c) */
struct drvslot {
    unsigned char used;
    unsigned      entry;
    unsigned      unit, cnt;
    unsigned      maxcnt;               /* Bloecke je READ (INIT, >= 1) */
    char          name[13];
};
extern struct drvslot drv[NDRV];

extern struct task task[NTASK];
extern unsigned nldt;
extern unsigned image_units;            /* Seiten der Programmabbilder (MEM) */
#ifndef NLDT
#define NLDT        nldt
#endif
#define D_ALIAS     0x0010          /* AVL-Bit (Byte 6): Eintrag ist Alias  */
#define LDT(t, i)   ((struct desc __far *)MK_FP(LDTD, ((unsigned)(t) * NLDT + (i)) << 3))

#endif
