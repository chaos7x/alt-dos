/*
 * FX - Programmformat fuer FAPI-Anwendungen (DOS 5.0)
 *
 * Aufbau einer .EXE-Datei:
 *
 *   0000h  MZ-Kopf (64 Byte), e_lfanew bei 3Ch zeigt auf den FX-Kopf
 *   0040h  DOS-Stub: gibt unter Real-Mode-DOS eine Meldung aus und endet
 *   e_lfanew  FX-Kopf (struct fx_header)
 *   image_off Programmabbild (Code und Daten, ein Segment, ab Offset 0)
 *
 * Der Lader legt ein Segment der Groesse image + bss + stack an, kopiert
 * das Abbild an den Anfang, nullt den Rest und setzt SP ans Segmentende.
 */
#ifndef FX_H
#define FX_H

#define FX_SIG0     'F'
#define FX_SIG1     'X'
#define FX_VERSION  1

struct fx_header {
    char           sig[2];          /* "FX"                             */
    unsigned short version;         /* FX_VERSION                       */
    unsigned short image_off;       /* Dateioffset des Abbilds          */
    unsigned short image_size;      /* Bytes                            */
    unsigned short bss_size;        /* Bytes, vom Lader genullt         */
    unsigned short stack_size;      /* Bytes                            */
    unsigned short entry;           /* IP beim Start                    */
    unsigned short flags;           /* reserviert, 0                    */
};

#endif
