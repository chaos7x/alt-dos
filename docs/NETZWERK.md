<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Netzwerk (AT-DOS 6.2) – Entwurf

Stand 18.11.2026, abgestimmt mit dem Nutzer: TCP/IP als **Ring-0-Modul**
(nicht im Kernel – erstes Codesegment und DGROUP sind voll –, nicht als
Ring-3-Dienst – Taskwechsel je Paket kosten auf dem 386 zu viel), Karten-
treiber über eine **eigene schlanke Schnittstelle** statt NDIS 2 (ein
Stapel, also kein Protocol Manager; je Richtung genau eine Kopie).
Vorbild für die Aufteilung: OS/2 TCP/IP 1.x (INET.SYS + Socket-Aufrufe).
Noch kein Code.

## Schichten

```
 Programme (Ring 3)      PING  IPCONFIG  WGET ...   (FAPI, 286-Code)
        |  FAPI_NET (Sockets, Schnittstellen, Namen)
 Kernel  -- net.c: Socket-Handles in der Dateitabelle, Weiterleitung,
        |           Netz-Schnittstellen, Weckruf ("Unterbrechungsnachlauf")
 TCPIP.SYS (Ring 0, eigenes Codesegment + Kernelsegmente für Puffer)
        |  ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS-Auflöser
        |  DevHlp DH_NET*: Rahmen senden / Empfangsring
 RTL8139.SYS, PCNET.SYS, E1000.SYS, NE2000.SYS (Ring 0, mit IRQ)
```

## Kartentreiber <-> Kernel

Ein Kartentreiber ist ein gewöhnlicher Treiber (DRV1, `DRV_INIT` meldet
0 Blockgeräte wie MOUSE.SYS, `DRV_DEINIT` hält die Karte an). Neu:

- `DH_NETREG` (23): BX = Zeiger auf `struct net_reg` im Treiber-DS
  (MAC 6 Byte, MTU, Sendeeinsprung im Treiber-CS, Merkmale: Rundruf/
  Mehrfachempfang, Prüfsummen nein) -> AX = Schnittstelle 0..3
  (`NNETIF` 4) bzw. Fehler.
- **Empfang:** der Kernel legt je Schnittstelle einen Empfangsring an
  (Kernelsegment, Plätze zu 1.536 Byte, Vorgabe 16 = 24 KB, unter 3,5 MB
  RAM 8). Der Treiber kopiert im IRQ den Rahmen direkt von der Karte in
  den nächsten freien Platz (`DH_NETRX` 24: BX = Schnittstelle -> AX =
  Offset des Platzes, Selektor fest aus der Anmeldung; danach
  `DH_NETRXDONE` 25 mit CX = Länge). Ring voll -> Rahmen verworfen und
  gezählt (wie eine echte Karte bei Überlauf). Kein Speicher wird im IRQ
  geholt.
- **Senden:** TCPIP.SYS ruft über den Kernel den Sendeeinsprung fern mit
  DS = Treiberdaten, ES:BX = Rahmen, CX = Länge (mit Ethernet-Kopf, ohne
  FCS). Der Treiber kopiert in seinen Sendepuffer (RTL8139: vier Puffer
  zu 2 KB über `DH_DMABUF`, PCnet/e1000: Deskriptorringe) und kehrt sofort
  zurück; alle Puffer belegt -> AX = 1 (später nochmal), TCPIP hält den
  Rahmen in seiner Schlange.
- Verbindung (Kabel) und Zähler über einen zweiten Einsprung `net_ctl`
  (Status, MAC-Filter für Mehrfachempfang).

Ein Packet Driver (INT 60h-Schnittstelle von FTP Software) für die
DOS-Boxen kommt später obendrauf: eine Box bekommt eigene Ethertypen oder
alle Rahmen, die TCPIP.SYS nicht für sich nimmt.

## Wo die Protokolle laufen

Im IRQ nur kopieren und `net_kick` (Merker setzen). Die eigentliche
Arbeit (ARP beantworten, IP zerlegen, TCP-Zustände, Zeitgeber für
Wiederholungen) macht ein **Nachlauf** wie die Softirqs von Linux 2.0:
der Scheduler ruft vor HLT, nach jedem FAPI-Aufruf und aus dem 100-Hz-
Takt (nur aus Ring 3, wie preempt) `net_run()`, wenn Arbeit ansteht –
das ruft `TCPIP.SYS` mit `NETM_RUN`. Der Nachlauf schläft nie, nimmt
nicht `klock`, nur die Sperre des Moduls (`MOD_NET`); ist sie belegt,
erledigt es der Besitzer beim Abgeben. Damit antwortet AT-DOS auf PING
und ARP auch, wenn kein Programm das Netz benutzt.

## Sockets für Programme

`FAPI_NET` (5Bh), BX = Unterfunktion, **ohne Kernelsperre** (BIGLOCK.md:
Netzstapel von Anfang an ohne `klock`), Wartende schlafen per ProcBlock
im Modul und wachen über `net_kick` bzw. Fristen auf.

- Sockets sind **Handles in der Dateitabelle** (`OF_SOCK`): DosRead/
  DosWrite/DosClose gehen (Umleitung, Pipes in CMD), task_end schließt
  sie, Kinder erben sie wie Dateien. Abbildung auf den Socket des Moduls
  über `files[k].first`.
- Aufrufe wie BSD (OS/2 TCP/IP, Winsock 1.1 dachten genauso): SOCKET
  (TCP/UDP), BIND, CONNECT, LISTEN, ACCEPT, SEND/RECV, SENDTO/RECVFROM,
  SHUTDOWN, SELECT (bis 16 Handles, Frist in ms), GETOPT/SETOPT
  (nicht blockierend, Fristen, Puffergrößen), GETNAME/GETPEER.
  Strukturen fest 16 Bit (`struct fapi_sockaddr`: Familie, Port, IPv4 –
  wie sockaddr_in, Bytefolge Netz). Signale (Strg+C) brechen Warten ab.
- Schnittstellen und Namen: IFINFO (Adresse, Maske, Gateway, DNS, MAC,
  Zähler, DHCP-Lease), IFSET (feste Adresse), RESOLVE (Name -> IPv4 über
  den DNS-Auflöser im Modul, kleiner Cache), PING (ICMP-Echo direkt,
  spart einen Roh-Socket).
- Unter MS-DOS (gebundener Stub) gibt es kein Netz -> Fehler; Packet
  Driver dort erst, wenn es jemand braucht.

## TCPIP.SYS

- Ein Codesegment (Ziel unter 48 KB), Daten im DS (Tabellen) plus
  Kernelsegmente über `DH_KSEG` für Puffer: Sendeschlange, je TCP-
  Verbindung Empfangs- und Sendefenster (Vorgabe je 8 KB, `/W:`),
  höchstens 32 Sockets (`/S:`). Speicher wird erst bei SOCKET geholt.
- ARP (Cache 16 Einträge, Warteschlange je Ziel 1 Rahmen), IPv4 ohne
  Optionen, Zerlegen/Zusammensetzen (Zusammensetzen bis 8 KB, ein
  Datagramm gleichzeitig), ICMP Echo/Unerreichbar, UDP, TCP nach RFC 793/
  1122: MSS, Fenster, Wiederholung mit RTT-Schätzung (Jacobson/Karels),
  verzögertes ACK, Nagle, TIME_WAIT gekürzt; kein SACK, keine Fenster-
  skalierung (16-Bit-Fenster reicht hier).
- Prüfsummen über eine kleine Assembler-Routine (16-Bit-Einerkomplement
  mit ADC, 32-Bit-Register erlaubt – Ring 0, 386-Regeln).
- Zahlen: Sequenznummern 32 Bit nur addieren/vergleichen, keine
  32-Bit-Mul/Div (RTT in 10-ms-Takten, Schieben).
- DHCP-Client im Modul (DISCOVER/OFFER/REQUEST/ACK, Lease erneuern bei
  T1), Vorgabe beim Laden; feste Werte über Schalter:
  `DEVICE=TCPIP.SYS /IP:192.168.1.20/24 /GW:192.168.1.1 /DNS:192.168.1.1`,
  `/HOST:name`. Ergebnis zusätzlich in der Systemumgebung (HOSTNAME,
  DOMAIN), wie LONGNAMES=.
- Laden/Entladen über DEVLOAD (DRV_DEINIT: alle Sockets zurücksetzen,
  abgelehnt solange Programme Sockets offen haben).

## Programme (FAPI, gebunden nicht nötig)

Stufe 1: `PING` (wie Windows: -n -t -l -w, Name oder Adresse),
`IPCONFIG` (/ALL, /RELEASE, /RENEW). Stufe 2: `NETSTAT`, `NSLOOKUP`,
`TFTP`, `FTP`, `TELNET`. Stufe 3: `WGET`/`CURL` (HTTP/1.1, FTP), danach
TLS als eigene Stufe. Zuletzt Kerberos/LDAP/SMB (siehe CLAUDE.md).

## Reihenfolge

1. DevHlp DH_NETREG/NETRX/NETRXDONE, `kernel/net.c` (KTEXT2 oder
   KTEXT4), Empfangsring, Weiterleitung zum Modul.
2. `RTL8139.SYS` (PCI 10ECh:8139h, auch der C+ im klassischen Betrieb):
   Reset, MAC aus IDR0–5, Empfangsring 8 KB + 16 + 1.500 (WRAP), vier
   Sendepuffer, IRQ (ROK/TOK/RER/TER/RXOVW), Abfragebetrieb `/NOIRQ`.
3. `TCPIP.SYS` mit ARP, IPv4, ICMP, UDP, DHCP; `PING`, `IPCONFIG`.
   Test in QEMU (`-netdev user -device rtl8139`: DHCP 10.0.2.15,
   Gateway 10.0.2.2, PING auf das Gateway), danach 86Box P3B-F (RTL8139C+
   über pcap am LAN des Nutzers) und Proxmox.
4. TCP, Sockets komplett, DNS. Test gegen einen Server auf dem Rechner
   (QEMU `guestfwd`/10.0.2.2), Durchsatz messen.
5. Weitere Karten: PCnet (VMware, VirtualBox), e1000, NE2000 ISA/
   ne2k_pci (86Box SCAT/486).

## Offene Punkte

- Platz im Kernel für `net.c`: KTEXT4 hat ~45 KB frei; DGROUP nur ein
  paar Byte (Tabellen der Schnittstellen in ein Kernelsegment).
- Die Kernelsegmente (`NKSEG` 20) werden mit Empfangsringen und Modul-
  puffern knapp – evtl. anheben.
- IPv6: nicht vor 7.0.
