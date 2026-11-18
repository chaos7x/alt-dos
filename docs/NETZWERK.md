<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Netzwerk (AT-DOS 6.2) – Entwurf

Stand 18.11.2026, abgestimmt mit dem Nutzer: TCP/IP als **Ring-0-Modul**
(nicht im Kernel – erstes Codesegment und DGROUP sind voll –, nicht als
Ring-3-Dienst – Taskwechsel je Paket kosten auf dem 386 zu viel), Karten-
treiber über eine **eigene schlanke Schnittstelle** statt NDIS 2 (ein
Stapel, also kein Protocol Manager; je Richtung genau eine Kopie).
Vorbild für die Aufteilung: OS/2 TCP/IP 1.x (INET.SYS + Socket-Aufrufe).
Noch kein Code.

Zukunftsfähig gebaut (Wunsch des Nutzers): die Treiberanmeldung trägt
Version und Merkmale (später DMA direkt in die Puffer des Stapels für
Gigabit), und **IPv6 wird von Anfang an mitgedacht** – Adressen,
Sockets, Verteiler, TCP/UDP sind familienunabhängig; umgesetzt wird
zuerst IPv4, IPv6 als eigene Stufe ohne neues API und ohne neue Treiber.

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
  -> AX = Schnittstelle 0..3 (`NNETIF` 4) bzw. Fehler. Inhalt:
  `version` (1), `size` (Strukturgröße – spätere Felder hängen hinten
  an, der Kernel liest nur, was er kennt), MAC 6 Byte, MTU,
  Sendeeinsprung und Steuereinsprung im Treiber-CS, `features`:
  - `NF_MCAST`: Mehrfachempfang mit Adressliste (IPv6 braucht ihn von
    Anfang an: Neighbor Discovery schickt an 33:33:FF:xx:xx:xx),
    sonst `NF_ALLMULTI` (alles Mehrfache annehmen) – eins von beiden
    ist Pflicht;
  - `NF_RXDMA` (später): der Treiber nimmt physische Adressen von
    Empfangsplätzen vorab (`DH_NETRXPOST`) und lässt die Karte direkt
    hineinschreiben – keine Kopie mehr (Gigabit, e1000 in Proxmox);
  - `NF_TXSG` (später): Senden aus bis zu vier Stücken (Kopf + Daten
    getrennt, kein Zusammenkopieren);
  - `NF_CSUM4`/`NF_CSUM6` (später): Karte rechnet Prüfsummen.
  Heute meldet RTL8139 nur `NF_MCAST`; der Kernel wählt je Merkmal den
  schnelleren Weg, alte Treiber laufen unverändert.
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
- Steuereinsprung `net_ctl`: Verbindung (Kabel, Geschwindigkeit),
  Zähler, Liste der Mehrfachadressen (IPv4 224.0.0.1, IPv6 ff02::1 und
  die Solicited-Node-Adressen), Promiscuous für die DOS-Boxen.
- Der Kernel verteilt empfangene Rahmen nach Ethertyp: 0800h (IPv4),
  0806h (ARP), 86DDh (IPv6) an TCPIP.SYS, der Rest später an Packet-
  Driver-Boxen.

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
- Aufrufe wie BSD (OS/2 TCP/IP, Winsock dachten genauso): SOCKET
  (Familie AF_INET/AF_INET6, TCP/UDP), BIND, CONNECT, LISTEN, ACCEPT, SEND/RECV, SENDTO/RECVFROM,
  SHUTDOWN, SELECT (bis 8 Handles, Frist in ms; umgesetzt 18.11.2026), GETOPT/SETOPT
  (nicht blockierend, Fristen, Puffergrößen), GETNAME/GETPEER.
  Signale (Strg+C) brechen Warten ab.
- Adressen wie BSD/RFC 3493: `struct fapi_sockaddr` mit Länge und
  Familie vorn, Platz für 28 Byte (`sockaddr_in6`: Port, Flowinfo,
  16 Byte Adresse, Scope); `sockaddr_in` (16 Byte) ist der Anfang davon.
  Jeder Aufruf bekommt Zeiger + Länge, nie eine feste Struktur – IPv6
  braucht kein neues API. Ein AF_INET6-Socket nimmt auch IPv4 als
  ::ffff:a.b.c.d (IPV6_V6ONLY schaltet es ab). Programme schreiben
  Adressen nur über Hilfen in `apps/net.h` (`net_pton`/`net_ntop`,
  RFC 5952 mit `::`), nie selbst.
- Namen wie `getaddrinfo`: RESOLVE liefert eine Liste von Adressen mit
  Familie (A und AAAA), Reihenfolge nach RFC 6724 vereinfacht (IPv6 vor
  IPv4, wenn eine globale IPv6-Adresse da ist).
- Schnittstellen und Namen: IFINFO (je Schnittstelle eine Liste von
  Adressen beider Familien mit Präfixlänge und Herkunft – fest, DHCP,
  SLAAC, link-lokal –, Gateways, DNS, MAC, Zähler, Leases), IFSET
  (feste Adresse), RESOLVE (DNS-Auflöser im Modul, kleiner Cache), PING (ICMP-Echo direkt,
  spart einen Roh-Socket).
- Unter MS-DOS (gebundener Stub) gibt es kein Netz -> Fehler; Packet
  Driver dort erst, wenn es jemand braucht.

## TCPIP.SYS

- Ein Codesegment (Ziel unter 48 KB), Daten im DS (Tabellen) plus
  Kernelsegmente über `DH_KSEG` für Puffer: Sendeschlange, je TCP-
  Verbindung Empfangs- und Sendefenster (Vorgabe je 8 KB, `/W:`),
  höchstens 32 Sockets (`/S:`). Speicher wird erst bei SOCKET geholt.
- **Familienunabhängig innen:** jede Adresse 16 Byte (IPv4 als
  ::ffff:a.b.c.d, RFC 4291), Socket-/TCP-/UDP-Blöcke, Routing-Tabelle
  (Präfix + Länge, längster Treffer), Nachbarn-Tabelle und Prüfsumme
  (Pseudokopf mit 4 oder 16 Byte) gibt es nur einmal. Nur die
  Netzwerkschicht ist doppelt: `ip4_in/out` und `ip6_in/out`.
  Nachbarn: ein Cache für ARP und Neighbor Discovery (Zustände wie
  RFC 4861: unvollständig, erreichbar, veraltet, prüfen).
- ARP (Cache 16 Einträge, Warteschlange je Ziel 1 Rahmen), IPv4 ohne
  Optionen, Zerlegen/Zusammensetzen (Zusammensetzen bis 8 KB, ein
  Datagramm gleichzeitig), ICMP Echo/Unerreichbar, UDP, TCP nach RFC 793/
  1122: MSS, Fenster, Wiederholung mit RTT-Schätzung (Jacobson/Karels),
  verzögertes ACK, Nagle, TIME_WAIT gekürzt; kein SACK, keine Fenster-
  skalierung (16-Bit-Fenster reicht hier).
- IPv6 umgesetzt 18.11.2026 (`net/ip6.h`), abweichend vom Entwurf
  unten: Fragmente werden verworfen, kein MLD, keine Pfad-MTU, kein
  DHCPv6 (DNS nur aus RDNSS), keine Datenschutzadressen; Kennung EUI-64
  statt RFC 7217. Ein Nachbarcache für ARP und ND (16-Byte-Schlüssel),
  eigene gespiegelte Rahmen (gleiche Absender-MAC) werden verworfen.
- IPv6 (eigene Stufe): Kopf + Erweiterungsköpfe überspringen (Hop-by-
  Hop, Routing Typ 0 verwerfen, Fragment – Zusammensetzen wie IPv4),
  ICMPv6 (Echo, Fehler, Packet Too Big -> Pfad-MTU je Ziel), Neighbor
  Discovery (NS/NA/RS/RA, DAD), SLAAC mit EUI-64 bzw. stabiler Kennung
  (RFC 7217), link-lokale Adresse immer, MLDv2 nur senden (Melden der
  Solicited-Node-Gruppen), RDNSS aus RA (RFC 8106), DHCPv6 zustandslos
  für DNS. Mindest-MTU 1280, zerlegt wird nur beim Absender.
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

Stufe 1: `PING` (wie Windows: -n -t -l -w, -4/-6, Name oder Adresse),
`IPCONFIG` (/ALL, /RELEASE, /RENEW; zeigt beide Familien). Stufe 2: `NETSTAT`, `NSLOOKUP`,
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
5. IPv6: ND, SLAAC, ICMPv6, RDNSS/DHCPv6 – TCP/UDP und Sockets
   bleiben, wie sie sind. (Erledigt 18.11.2026 ohne DHCPv6.) Test in QEMU (`ipv6=on`, fec0::/64) und am
   LAN des Nutzers.
6. Weitere Karten: PCnet (VMware, VirtualBox), e1000, NE2000 ISA/
   ne2k_pci (86Box SCAT/486).

## Firewall (abgestimmt 18.11.2026, Regeln je Programm umgesetzt 18.11.2026)

- In TCPIP.SYS: je ein Prüfpunkt am Eingang und Ausgang der IP-Schicht,
  ein Code für IPv4 und IPv6, Regelliste linear (auch auf dem 286 billig).
- Zustandsbehaftet, Vorgabe wie Windows: eingehend gesperrt außer
  Antworten (TCP über die Verbindungstabelle, UDP-Anfragen 30–60 s
  gemerkt), ausgehend frei; ARP, DHCP-Antworten und später ND immer.
  Geschlossene Ports schweigen (kein RST, kein „Port unerreichbar“).
- Entscheidung des Nutzers: **ab Werk an** (`FIREWALL OFF` schaltet ab);
  **PING von außen nur aus dem eigenen Netz** (Präfix der Schnittstelle),
  sonst still.
- Regeln je Programm (der Stapel kennt den Besitzer jedes Sockets):
  „FTPD.EXE darf auf 21 lauschen, nur aus 192.168.1.0/24“ – Lauschen
  allein öffnet nichts.
- `\DOS\SYSTEM\FIREWALL.INI` (CONFIG.SYS ist voll) und `FIREWALL.EXE`
  (Regeln/Zähler zeigen, zur Laufzeit ändern, an/aus); Abgewiesenes
  gedrosselt in DMESG.
- Härtung: zufällige ISN und Absenderports, keine IP-Optionen/Source-
  Routing, ICMP-Redirects ignorieren, kein Echo auf Rundruf, Grenze für
  halboffene Verbindungen, strenge Längen-/Prüfsummenprüfung.
  Umgesetzt 18.11.2026: ISN = Zeitanteil (4 µs-Schritte wie RFC 6528)
  + Hash aus geheimem Schlüssel und Adressen/Ports, Absenderports
  zufällig ab 49152, Zufall aus Takt und Ankunftszeiten der Rahmen;
  höchstens 4 halboffene Verbindungen über alle Listener; RST nur bei
  genau passender Folgenummer, sonst ACK (RFC 5961); IP-Kopf mit
  Optionen und Absender 0/127/224+/eigene Adresse verworfen; ICMP kennt
  nur Echo (Redirects fallen weg), kein Echo auf 255.255.255.255.
  Geprüft mit dem Störsender `tools/netfuzz.py` (über 30.000 verstümmelte
  Rahmen, 18.11.2026); dabei behoben: RTL8139 nach Kurzrahmen taub,
  Leck je abgelaufener halboffener Verbindung, SYN-Flut sperrte den Port
  (jetzt zufälliger halboffener geopfert), verlorene Takte unter Last,
  DHCP-ACK ohne Anfrage, DNS-Antworten von Fremden, IPv6-Absender
  ::ffff:a.b.c.d/::1/::a.b.c.d, MSS unter 64.
- Reihenfolge: Prüfpunkte und Vorgabe zusammen mit TCP (Stufe 2), Regeln
  je Programm, INI und Werkzeug direkt danach – bevor ein Programm lauscht.
- Packet-Driver-Boxen laufen später durch denselben Filter.

## Offene Punkte

- Platz im Kernel für `net.c`: KTEXT4 hat ~45 KB frei; DGROUP nur ein
  paar Byte (Tabellen der Schnittstellen in ein Kernelsegment).
- Die Kernelsegmente (`NKSEG` 20) werden mit Empfangsringen und Modul-
  puffern knapp – evtl. anheben.
- Speicher: 16-Byte-Adressen kosten je TCP-Block ~24 Byte mehr – bei
  32 Sockets unter 1 KB, dafür kein zweiter Code für IPv6 in TCP/UDP.
- IPv6-Datenschutzadressen (RFC 4941), DHCPv6 mit Adressvergabe,
  Mobile IPv6: später nach Bedarf.
