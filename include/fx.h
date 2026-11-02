/*
 * FX - Programmformat fuer FAPI-Anwendungen (AT-DOS 5.0)
 *
 * Aufbau einer .EXE-Datei:
 *
 *   0000h  MZ-Kopf (64 Byte), e_lfanew bei 3Ch zeigt auf den FX-Kopf
 *   0040h  DOS-Stub: gibt unter Real-Mode-DOS eine Meldung aus und endet;
 *          gebundene Programme haben stattdessen den FAPI-Stub (stub/),
 *          der sie unter MS-DOS ausfuehrt
 *   e_lfanew  FX-Kopf (struct fx_header), Signatur "CH"
 *   image_off Programmabbild (Code und Daten, ein Segment, ab Offset 0)
 *
 * Die Signatur folgt dem Vorbild von MZ (Mark Zbikowski): das Kuerzel des
 * Entwicklers, hier CH fuer chaos7x.
 *
 * Version 1 (Treiber, IFS-Module): der Lader legt ein Segment der Groesse
 * image + bss + stack an, kopiert das Abbild an den Anfang, nullt den Rest
 * und setzt SP ans Segmentende. Version 2 (Programme): Code und Daten
 * getrennt; laeuft dasselbe Programm schon, wird sein Codesegment geteilt
 * (wie bei OS/2), jede Instanz hat ihr eigenes Datensegment.
 */
#ifndef FX_H
#define FX_H

#define FX_SIG0     'C'
#define FX_SIG1     'H'
#define FX_VERSION  1               /* ein Segment (Treiber, IFS)        */
#define FX_VERSION2 2               /* Programme: Code und Daten getrennt,
                                       Code zwischen Instanzen geteilt   */

#define FX_FLAG_DRIVER  0x0001      /* Geraetetreiber (Ring 0), kein Programm */
#define FX_FLAG_LONGNAMES 0x0002    /* kann lange Namen (wie OS/2 1.2): sonst
                                       liefern DosFindEntry/DosQCurDir 8.3 */
#define FX_FLAG_DOS     0x0004      /* gebunden (mkfx --bind): der MZ-Stub
                                       uebersetzt die FAPI unter MS-DOS */

struct fx_header {
    char           sig[2];          /* "CH"                             */
    unsigned short version;         /* FX_VERSION                       */
    unsigned short image_off;       /* Dateioffset des Abbilds          */
    unsigned short image_size;      /* Bytes                            */
    unsigned short bss_size;        /* Bytes, vom Lader genullt         */
    unsigned short stack_size;      /* Bytes                            */
    unsigned short entry;           /* IP beim Start                    */
    unsigned short flags;           /* FX_FLAG_*                        */
    /* ab Version 2: Abbild = Code (code_size Bytes, Vielfaches von 16) +
       initialisierte Daten; Datensegment = Daten + bss_size + stack_size,
       beide Segmente ab Offset 0, entry im Code */
    unsigned short code_size;
};

#endif
