/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.2 - Netzwerk: Schnittstelle Kartentreiber <-> Kernel <-> Stapel
 * (Entwurf in docs/NETZWERK.md)
 *
 * Ein Kartentreiber (RTL8139.SYS ...) meldet beim INIT seine Karte mit
 * DH_NETREG an (0 Blockgeraete, DRV_OK). Der Stapel (TCPIP.SYS) meldet
 * sich mit DH_NETPROTO. Der Kernel vermittelt nur:
 *
 *   Empfang (im IRQ des Kartentreibers): DH_NETRX mit struct net_pkt
 *     (ifc, len) -> der Kernel ruft rx_off des Stapels, der setzt sel:off
 *     auf einen freien Platz (0:0 = verwerfen); der Treiber kopiert den
 *     Rahmen dorthin und ruft DH_NETRXDONE -> done_off des Stapels.
 *     Danach ruft der Kernel aus dem Scheduler run_off (Nachlauf).
 *   Senden: der Stapel ruft DH_NETSEND mit struct net_pkt -> send_off des
 *     Kartentreibers, der kopiert in seinen Sendepuffer und kehrt sofort
 *     zurueck (NS_FULL: alle Puffer belegt, spaeter nochmal).
 *
 * Alle Einspruenge sind ferne Funktionen im Treibercode (RETF), gerufen
 * mit DS = Daten des Treibers, ES:BX = Struktur, SS = Kernelstapel;
 * Rueckgabe in AX. rx_off/done_off laufen mit gesperrten Interrupts.
 *
 * Erweiterbar: jede Struktur hat version/size; spaetere Felder kommen
 * hinten dazu, der Kernel liest nur, was size meldet.
 */
#ifndef NET_H
#define NET_H

#define NNETIF          4           /* Karten                           */

/* DevHlp (Nummern in drv.h fortgesetzt) */
#define DH_NETREG       23          /* Karte: BX = struct net_reg -> AX = ifc,
                                       FFFFh = kein Platz/ungueltig        */
#define DH_NETPROTO     24          /* Stapel: BX = struct net_proto -> AX = 0 */
#define DH_NETRX        25          /* Karte (IRQ): BX = struct net_pkt (ifc,
                                       len) -> sel:off gesetzt, AX = 0; AX = 1:
                                       verwerfen (kein Stapel, Ring voll)  */
#define DH_NETRXDONE    26          /* Karte (IRQ): BX = struct net_pkt, Rahmen
                                       liegt in sel:off                     */
#define DH_NETSEND      27          /* Stapel: BX = struct net_pkt -> AX = NS_* */
#define DH_NETCTL       28          /* Stapel: BX = struct net_ctl -> AX = 0 ok */
#define DH_NETINFO      29          /* Stapel: BX = ifc, CX = struct net_reg
                                       (Ziel im DS) -> AX = 0, 1 = keine Karte */
#define DH_NETKICK      30          /* Stapel: Nachlauf bald rufen         */

/* Merkmale einer Karte (net_reg.features) */
#define NF_MCAST        0x0001      /* Mehrfachempfang mit Adressliste    */
#define NF_ALLMULTI     0x0002      /* nur "alles Mehrfache" (ohne Liste)  */
#define NF_RXDMA        0x0010      /* spaeter: Karte schreibt direkt in
                                       Plaetze des Stapels                 */
#define NF_TXSG         0x0020      /* spaeter: Senden aus mehreren Stuecken */
#define NF_CSUM4        0x0040      /* spaeter: Pruefsummen IPv4           */
#define NF_CSUM6        0x0080      /* spaeter: Pruefsummen IPv6           */

#pragma pack(1)
struct net_reg {                    /* Karte -> DH_NETREG                 */
    unsigned short version;         /* 1                                   */
    unsigned short size;            /* sizeof(struct net_reg)              */
    unsigned char  mac[6];
    unsigned short mtu;             /* 1500                                */
    unsigned short features;        /* NF_*                                */
    unsigned short send_off;        /* fern: ES:BX = struct net_pkt -> NS_* */
    unsigned short ctl_off;         /* fern: ES:BX = struct net_ctl -> 0 ok */
    char           name[12];        /* "RTL8139"                           */
    unsigned short irq;             /* zur Anzeige (0 = Abfrage)           */
    unsigned short io;              /* Basisport zur Anzeige               */
};

struct net_pkt {                    /* ein Rahmen (Ethernet, ohne FCS)     */
    unsigned short ifc;
    unsigned short len;
    unsigned short off, sel;        /* Rahmen (Empfang: vom Stapel gesetzt) */
};

#define NS_OK           0
#define NS_FULL         1           /* Sendepuffer belegt, spaeter nochmal */
#define NS_ERR          2           /* Karte/Verbindung gestoert           */

struct net_ctl {                    /* DH_NETCTL -> ctl_off der Karte      */
    unsigned short ifc;
    unsigned short cmd;             /* NC_*                                */
    unsigned short arg;
    unsigned short off, sel;        /* Daten                               */
    unsigned short len;
    unsigned short result;
};
#define NC_LINK         1           /* -> result Bit 0 Verbindung, Bit 1
                                       Vollduplex, Bits 8-15 Mbit/10      */
#define NC_MCAST        2           /* sel:off = len/6 Mehrfachadressen    */
#define NC_PROMISC      3           /* arg = 1 an, 0 aus                   */
#define NC_STATS        4           /* sel:off = struct net_stats          */

struct net_stats {
    unsigned long rx_ok, tx_ok, rx_err, tx_err, rx_drop, tx_coll;
};

struct net_proto {                  /* Stapel -> DH_NETPROTO               */
    unsigned short version;         /* 1                                   */
    unsigned short size;
    unsigned short rx_off;          /* IRQ: ES:BX = net_pkt (ifc, len) ->
                                       sel:off setzen, 0:0 = verwerfen     */
    unsigned short done_off;        /* IRQ: ES:BX = net_pkt, Rahmen fertig */
    unsigned short run_off;         /* Nachlauf: ES:BX = struct net_run ->
                                       AX Bit 0 = bitte je Takt wieder rufen */
    unsigned short call_off;        /* FAPI_NET: ES:BX = struct net_call   */
    unsigned short link_off;        /* neue Karte: ES:BX = net_pkt (ifc)   */
};

struct net_run {
    unsigned long  ticks;           /* Systemtakt (100 Hz)                 */
};

struct net_call {                   /* FAPI_NET eines Programms            */
    unsigned short fn;              /* BX: NET_* (fapi_abi.h)               */
    unsigned short usel;            /* DS des Programms                     */
    unsigned short p;               /* CX: Parameterblock im DS             */
    unsigned short n;               /* DX: dessen Groesse                   */
    unsigned short task;            /* Tasknummer                           */
};
#pragma pack()

#endif
