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
  Je Karte eine globale und eine lokale Adresse (ULA fc00::/7) neben
  der link-lokalen: kündigt der Router beide Präfixe an (Fritz!Box:
  öffentlich + fd00::/64), bekommt jedes Ziel den passenden Absender –
  ULA-Ziele die ULA (direkt im LAN), alles andere die globale, ohne
  globale die ULA. Vorher nahm TCPIP.SYS nur das zuletzt genannte Präfix
  (oft die ULA, kein IPv6 ins Internet); der erste Fix (nur die
  öffentliche) machte ULA-Rechner im LAN unerreichbar (beides vom Nutzer
  an seinem LAN gemeldet, 20.11.2026). Eine ULA zählt beim Auflösen von
  Namen nicht als globale Adresse (AAAA nicht zuerst).
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
`TFTP`, `FTP`, `TELNET`. Stufe 3: `WGET`/`CURL` (HTTP/1.1, FTP; WGET,
FTP und TELNET erledigt 19.11.2026), danach
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
   ne2k_pci (86Box SCAT/486). Erledigt: NE2000 (18.11.2026), PCNET.SYS
   und E1000.SYS (19.11.2026; E1000 nur in QEMU geprüft, 86Box hat keine).

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

## TLS (6.3, Stufe 1: Verschlüsselung)

Eigener TLS-1.2-Client für FAPI-Programme (Ring 3, 286-Code wie alle
Programme): `apps/crypto.c` und `apps/tls.c`, benutzt von WGET.

- Kryptografie (`crypto.h`): SHA-256, HMAC-SHA256, TLS-PRF, AES-128 (nur
  Verschlüsseln), GCM (GHASH bitweise), X25519 (16 Worte zu 16 Bit,
  2^256 = 38 mod p, Multiplikation nur 16 × 16 -> 32 Bit über `mul16`,
  keine Watcom-Hilfen). Auf dem Rechner mit gcc übersetzbar
  (`CRYPTO_HOST`): `tools/cryptotest.c` + `tools/cryptotest.py` rechnen
  je Lauf 300 Fälle (Zufallslängen bis 2.900 Byte, Zufallsschlüssel)
  gegen Python `hashlib`/`cryptography` nach – alle gleich.
- Ablauf (`tls.c`): ClientHello mit ECDHE_ECDSA/ECDHE_RSA_WITH_AES_128_
  GCM_SHA256, SNI (nicht bei Zahlenadressen), supported_groups X25519 und
  secp256r1 (ohne die Kurve des Zertifikats nimmt OpenSSL sein ECDSA-
  Zertifikat nicht – RFC 8422 5.1; ECDHE nur mit X25519), signature_
  algorithms, Extended Master Secret (RFC 7627), renegotiation_info.
  Handshake-Strom über Datensätze hinweg, Nachrichten bis 2 KB im Puffer,
  größere (Certificate) nur in den Verlauf gerechnet. Datensätze bis 2^14
  + 2048 Byte, AES-GCM mit expliziter Nonce = Folgenummer. Meldungen:
  close_notify = Ende, Warnungen übergangen, HelloRequest mit
  no_renegotiation beantwortet. Alles statisch (~22 KB DGROUP).
- Zufall: FAPI 5Ch `DosRandom` – Pool im Kernel, den jeder Hardware-
  Interrupt außer dem Takt mit dem laufenden PIT-Zähler mischt (start.asm
  `ENTMIX` in irq1 und irq_common; alle anderen Leser von Kanal 0 latchen
  bei gesperrten Interrupts). `tls_random` hasht ihn mit Datum/Zeit und
  dem Zittern einer Zählschleife je Takt, danach Zähler + Pool je Block.
- Stufe 1 prüft den Server NICHT (Zertifikatskette, Signatur des
  ServerKeyExchange, Name, Datum): ohne `TLS_NOVERIFY` liefert tls_open
  TLSE_NOVERIFY, WGET verlangt `--no-check-certificate` (ERRORLEVEL 5).
  Stufe 2: X.509 lesen, RSA (PKCS#1 v1.5, PSS) und ECDSA P-256/P-384,
  SHA-384, CA-Liste, Name (SAN) und Gültigkeit.
- Geprüft in QEMU gegen `openssl s_server -WWW -tls1_2` mit RSA-2048- und
  ECDSA-P-256-Zertifikat: je 1 MB byte-gleich, ohne Schalter ERRORLEVEL 5;
  86Box 386DX/40 mit NE2000 (Entropie im Interruptpfad): beide 1 MB
  byte-gleich.

## TLS Stufe 2: Prüfung des Servers (20.11.2026)

- `apps/pki.c` (Schnittstelle `pki.h`): SHA-384/512 (64-Bit-Worte als
  Paare, Konstanten aus Python nachgerechnet), Großzahlen in 16-Bit-Worten
  mit Montgomery-Multiplikation (CIOS, `mul16`, R² mod m durch Verdoppeln,
  Inverses von m[0] nach Newton) bis 4096 Bit; RSA PKCS#1 v1.5 (DigestInfo
  SHA-256/384/512) und PSS (MGF1, Salz beliebiger Länge); ECDSA P-256 und
  P-384 (Jacobi-Koordinaten mit a = -3, dbl-2001-b/add-2007-bl, Shamir für
  u1·G + u2·Q, Inverse nach Fermat); DER-Leser und X.509 (tbs, Aussteller/
  Subjekt roh für den Vergleich, Gültigkeit, Schlüssel RSA/EC, Signatur-
  verfahren, subjectAltName, basicConstraints, CN). RSA und EC teilen sich
  die Montgomery-Arbeitsbereiche (DGROUP von WGET 60,7 KB).
- `tls.c`: Certificate-Nachricht (bis 40 KB) in einem DosAllocSeg-Segment,
  bis 8 Zertifikate; Signatur des ServerKeyExchange über client_random +
  server_random + Parameter mit dem Schlüssel des ersten Zertifikats
  (0401/0501/0601, 0804/0805/0806, 0403/0503); Name des ersten
  Zertifikats (dNSName, `*.` für genau ein Etikett, IP-Adresse, ohne
  dNSName der CN); Gültigkeit jedes Glieds auf den Tag mit einem Tag
  Spielraum (AT-DOS-Uhr ist Ortszeit); Kette: Aussteller zuerst in der CA-
  Liste suchen (PEM Zeile für Zeile, Base64 in das Segment, Subjekt
  vergleichen, Signatur prüfen – gleichnamige Querzertifikate werden der
  Reihe nach versucht), sonst in der gesendeten Kette (muss CA sein).
  Meldungen mit Einzelheit (`tls_why`): Name, unbekannter Aussteller von
  CN, Gültig bis. Fehler beantwortet der Client mit dem passenden Alert.
- CA-Liste `certs/cacert.pem` -> `\DOS\SYSTEM\CACERT.PEM` (Diskette 1 und
  Platte; Herkunft und Lizenz in `certs/README.txt`).
- Geprüft: auf dem Rechner `tools/pkitest.*` (469 Fälle: SHA-384/512, RSA
  2048/3072/4096 PKCS#1/PSS mit SHA-256/384/512 echt und verfälscht, ECDSA
  P-256/P-384 mit SHA-256/384 echt und verfälscht, alle selbstsignierten
  Wurzeln der Mozilla-Liste mit dem eigenen Schlüssel, Namensregeln). QEMU
  gegen `openssl s_server` mit einer Test-CA (Wurzel RSA-3072 an die Liste
  angehängt): RSA-Blatt (PSS im ServerKeyExchange) und Kette RSA-Wurzel ->
  P-384-Zwischenzertifikat -> P-256-Blatt (ecdsa-with-SHA384) je 1 MB
  byte-gleich; falscher Name, selbstsigniert und abgelaufen jeweils mit dem
  richtigen Grund abgelehnt (ERRORLEVEL 5), mit --no-check-certificate
  angenommen. 86Box 386DX/40 mit NE2000: beide Ketten angenommen.
- Offen: SHA-1-Signaturen (alte Zwischenzertifikate), Sperrlisten/OCSP,
  ECDHE über P-256 (nur X25519), TLS 1.3.

## SMB Stufe 1: SMBCLI (20.11.2026)

`apps/task_smbcli.c` nach [MS-SMB2], [MS-NLMP] und RFC 4178 (nur gelesen):
NEGOTIATE mit 2.0.2 und 2.1 (CreditCharge ab 2.1), SESSION_SETUP zweimal
(NTLMSSP NEGOTIATE in SPNEGO NegTokenInit, CHALLENGE aus der Antwort
gesucht, AUTHENTICATE in NegTokenResp), NTLMv2: NTOWFv2 = HMAC-MD5(MD4
(UTF-16 Kennwort), UTF-16(GROSS(Benutzer) + Domäne)), Blob mit
MsvAvTimestamp des Servers, Client-Challenge aus `tls_random` (jetzt in
`apps/rnd.c`), NTProofStr, Sitzungsschlüssel = HMAC-MD5(NTOWFv2,
NTProofStr) ohne KEY_EXCH; Signatur HMAC-SHA256 der Nachricht (16 Byte),
wenn der Server sie verlangt (Gast: nie). TREE_CONNECT, CREATE/CLOSE,
QUERY_DIRECTORY (FileDirectoryInformation, Datum über FILETIME -> FAT wie
7ZDEC), READ/WRITE in 16 KB, Löschen über DELETE_ON_CLOSE, Verzeichnis über
FILE_CREATE + FILE_DIRECTORY_FILE; Zwischenantworten STATUS_PENDING
werden übergangen. `crypto.c` hat dafür MD4, MD5 und HMAC-MD5 (auf dem
Rechner gegen Python geprüft, `tools/cryptotest.*`).

Geprüft in QEMU gegen Samba 4.19 (standalone, `server signing =
mandatory`, Freigabe mit Benutzer): dir, get 1 MB byte-gleich (0,4 s),
put byte-gleich, md/cd/put/dir/del/rd, Ordner „Ordner Ä“ (Umlaut,
Leerzeichen), Befehle aus umgeleiteter Eingabe, falsches Kennwort ->
ERRORLEVEL 4. Reine Programmlogik (kein Kernelteil) – kein 86Box-Lauf.

## SMB Stufe 2: NET USE über einen Dienst in Ring 3 (20.11.2026)

Entscheidung des Nutzers („Ring 3“): kein Redirector als Ring-0-IFS,
sondern ein Dateisystem, das ein Programm bedient – wie FUSE bei Linux.
Der Kernel kennt nur den Weg dorthin (`kernel/ufs.c`, KTEXT4, FAPI 5Dh
`FAPI_UFS`); SMB, Anmeldung, Signatur bleiben in SMBCLI im Ring 3 und
laufen so unter 7.0 unverändert.

- Kernel: bis 4 Dienste (`NUFS`), je Dienst ein Laufwerk (`vol[]` mit
  `ufs_driver`, Name „SMB“), ein Blockgerät ohne Blöcke (`DRV_UFS` -3,
  nur damit Laufwerkssperre und Liste passen) und ein Kernelsegment:
  Auftrag (`struct fapi_ufsreq`, 584 Byte) bei 0, Daten (bis 16 KB) bei
  1024. Ein IFS-Aufruf (open/read/readdir/create/write/close/remove/
  mkdir/rmdir/move/space/setinfo/label/namepath/truncate) füllt den
  Auftrag, weckt den Dienst und schläft (`kf_ufs_block`, gibt die
  Kernelsperre ab wie ein Treiberauftrag; Frist 30 s). Der Dienst holt
  mit `UFS_GET` (wartet ohne Kernelsperre, WRITE-Daten gleich in seinen
  Puffer) und antwortet mit `UFS_PUT` (Folgenummer gegen veraltete
  Antworten). `UFS_REG` meldet ein Laufwerk an, `UFS_DEL` lässt den
  Dienst enden (abgelehnt, solange Dateien offen oder ein Task dort
  steht), `UFS_INFO` nennt Dienst und `\\Rechner\Freigabe`. Endet der
  Dienst (auch abgeschossen), gibt `ufs_task_end` Laufwerk, Segment und
  Gerät frei und weckt Wartende mit Fehler.
- `SMBCLI … --serve X:` (losgelöst gestartet): nach TREE_CONNECT
  `UFS_REG`, dann Schleife GET/PUT. Bis 32 offene Dateien (FileId je
  Handle), Öffnen erst lesend+schreibend, bei „Zugriff verweigert“ nur
  lesend; Datum/Attribut beim Schließen und SETINFO über SET_INFO
  FileBasicInformation (FAT -> FILETIME über 16-Bit-Multiplikationen),
  Umbenennen FileRenameInformation, Kürzen FileEndOfFileInformation,
  freier Platz QUERY_INFO FileFsSizeInformation (gekappt auf 2 GB).
  Verzeichnislisten bleiben je Verzeichnis in einem 64-KB-Segment
  zwischengespeichert (verworfen bei jeder Änderung), mit 8.3-Ersatz-
  namen wie Windows 95 (`BASIS~k.EXT`) für Programme ohne LFN-Merker
  und die DOS-Boxen; Pfade des Kernels mit `~` werden darüber in die
  echten Namen zurückübersetzt, der Originalname beim Anlegen kommt aus
  `ifs_orig`. Der Stamm hat kein `.`/`..`. Die Handles 0–2 schließt der
  Dienst (sonst hielte er eine Umleitung von NET belegt).
- `NET.EXE`: `NET USE` listet, `NET USE X: \\Rechner\Freigabe
  [Kennwort|*] [/USER:[Domäne\]Name]` legt das Kennwort in eine Datei in
  %TEMP% (nicht in die Befehlszeile – TASKMAN zeigt sie), startet
  `X:\DOS\SMBCLI.EXE … -P Datei --serve X:` mit EXEC_DETACH und wartet,
  bis das Laufwerk da ist oder SMBCLI „!Meldung“ in die Datei schreibt
  (SMBCLI löscht das Kennwort gleich nach dem Lesen). `NET USE X:
  /DELETE` (/D) trennt.

Geprüft in QEMU gegen Samba 4.19 (signing mandatory): falsches Kennwort
-> Meldung, NET USE + Liste, DIR (Umlaute, Leerzeichen, Unterordner), COPY
1 MB vom Laufwerk byte-gleich, MD, XCOPY C:\DOS\*.EXE hinüber (87
Dateien byte-gleich), COPY auf einen langen Namen, REN, TYPE, CD auf das
Laufwerk, `COMMAND /C DIR` in der DOS-Box (8.3-Namen), Trennen, danach
„Ungültiges Laufwerk“. Ebenso in 86Box (386DX/40, 4 MB, NE2000, SLiRP zum
Samba des Rechners): alle Schritte gleich, Kopien byte-gleich, Datum der
Quelle bleibt erhalten.
Wiederverbinden (20.11.2026): bricht die Verbindung ab (Netz weg, Server
neu gestartet – Sende-/Empfangsfehler oder NT-Status SESSION_EXPIRED,
USER_SESSION_DELETED, NETWORK_NAME_DELETED), baut der Dienst beim
nächsten Auftrag Sitzung und Freigabe neu auf (`sconnect`, aus `main`
herausgelöst; das Kennwort bleibt dafür im Speicher des Dienstes), öffnet
alle offenen Dateien wieder (Pfad und Zugriff je Handle in einem eigenen
Segment, neue FileIds) und wiederholt den Auftrag einmal – WRITE-Daten
werden vorher gerettet, weil der Aufbau den Sendepuffer braucht.
Scheitert das Neuverbinden, bekommt nur dieser Auftrag einen Fehler, der
nächste versucht es wieder. Ein CLOSE ohne Server gibt den Handle trotzdem
frei. Eine Datei, die nach dem Neuverbinden nicht mehr zu öffnen ist, gibt
für jeden weiteren Zugriff einen Fehler. Geprüft in QEMU (Samba auf dem
Rechner ganz beendet, 4 s später neu gestartet): DIR, COPY hin und zurück
(1 MB byte-gleich) danach; NC mit Ausgabe auf das Netzlaufwerk, Samba-
Neustart, dann 2.100 Byte Daten – kommen vollständig in der wieder
geöffneten Datei an. Lehre beim Testen: `pkill -x smbd` beendet nur den
Elternprozess, die Verbindung bedient ein Kind („smbd: client …“).

## Domäne Stufe 1: Kerberos (20.11.2026)

Reihenfolge (Entscheidung des Nutzers): Wiederverbinden, Domäne, Sperren,
SMB 3. Alles in Ring 3 (Programme), damit es unter 7.0 unverändert läuft.

- `apps/krbcrypt.c` (eigene Umsetzung nach den Normen, nur gelesen):
  SHA-1, HMAC-SHA1 (vorbereiteter Schlüsselzustand – PBKDF2 braucht so
  zwei statt vier Blöcke je Runde), PBKDF2-HMAC-SHA1, n-fold, DK,
  Schlüssel aus Kennwort und der Rahmen aes256-cts-hmac-sha1-96 (RFC
  3961/3962: Konfusor, AES-CBC mit Ciphertext Stealing, HMAC-SHA1-96 mit
  Ki/Ke/Kc). `crypto.c` kennt dafür AES-256 und Entschlüsseln (`aes_keyl`,
  `aes_dec`, umgekehrte S-Box einmal aus der S-Box). Auf dem Rechner
  geprüft: `tools/krbtest.c` | `tools/krbtest.py` (Testwerte RFC 3962
  Anhang B, n-fold, 404 Zufallsfälle gegen hashlib/cryptography),
  `tools/cryptotest` danach unverändert.
- `apps/krb5.c` (RFC 4120, RFC 4121): DER schreiben (Kennung + 3 Byte
  Platz für die Länge, beim Schließen kürzeste Form und Inhalt nach
  vorn; Zustand wird für verschachtelte Teile gesichert) und lesen. KDC
  über TCP 88 (4 Byte Länge vorn; UDP lohnt nicht – Tickets mit PAC sind
  größer als ein Datagramm). AS-REQ zuerst ohne Vorauthentisierung: die
  KRB-ERROR-Antwort PREAUTH_REQUIRED liefert Salz und Iterationen
  (PA-ETYPE-INFO2) und die Uhr des KDC (`stime` -> `krb_off`, UTC =
  Ortszeit + krb_off – so stört weder die Zeitzone noch eine falsche Uhr
  von AT-DOS); dann mit PA-ENC-TIMESTAMP. TGS-REQ mit PA-TGS-REQ (AP-REQ
  mit Authenticator, Prüfsumme hmac-sha1-96-aes256 über den Request-Body,
  Verwendung 6/7, Antwort 8). Nur Etype 18 (AES-256). GSS-Hülle RFC 4121
  ohne gegenseitige Anmeldung (Merker Integrität + Vertraulichkeit) –
  kein AP-REP, Sitzungsschlüssel = der des Dienst-Tickets. Arbeitspuffer
  (je 4 KB) stellt der Aufrufer (`krb_bufs`): SMBCLI nimmt seine Sende-/
  Empfangspuffer, sonst passte DGROUP nicht (SMBCLI jetzt 60.618 Byte +
  4 KB Stapel). Ablage `%KRB5CCNAME%`, sonst `%TEMP%\KRB5CC.DAT`: Kopf
  (Uhrversatz, KDC) + bis 4 Einträge (TGT zuerst, dann Diensttickets),
  eintragsweise gelesen/geschrieben.
- `KINIT [Name[@REALM]] [/KDC:Adresse]` (Name/Realm sonst aus %USERNAME%/
  %USERDNSDOMAIN%; KDC sonst der Name der Domäne – im AD hat er die
  Adressen der DCs; Kennwort von der Tastatur ohne Echo, bei umgeleiteter
  Eingabe aus deren erster Zeile), `KLIST`, `KDESTROY` (überschreibt die
  Ablage vor dem Löschen). Meldungen nach KRB-ERROR-Code (Kennwort
  falsch, Benutzer unbekannt, Uhr, gesperrt …).
- `SMBCLI -k`: TGT aus der Ablage, `cifs/rechner` aus der Ablage oder vom
  KDC (und abgelegt), AP-REQ in SPNEGO (Mechanismus krb5), signiert wie
  bisher mit den ersten 16 Byte des Sitzungsschlüssels. `NET USE X:
  \\Rechner\Freigabe` ohne Kennwort und `/USER:` nimmt Kerberos von
  selbst, wenn die Ablage da ist und der Rechner mit Namen angegeben ist
  (wie Windows); sonst NTLMv2 wie bisher.

Test-DC (nur im Scratchpad, nicht im Repository): Samba 4.19 als AD-DC
„ATDOS.TEST“ (`samba-tool domain provision`, Python 3.12 – Ubuntu startet
3.13 –, Samba nur an 127.0.0.1, Container ohne IPv6, winbind
nachinstalliert, Freigabe außerhalb eines 700-Verzeichnisses), DNS-Einträge
der Domäne und von `dc1` auf 10.0.2.2 (aus QEMU über SLiRP), Gast mit
`TCPIP.SYS /DNS:10.0.2.2`. Geprüft in QEMU: KINIT mit falschem Kennwort
(„Kennwort falsch“), unbekanntem Benutzer, richtig (AES-256-Ticket bis 10
Std.), KLIST, KDESTROY; SMBCLI -k dir/get/put signiert, beim zweiten Mal
mit dem abgelegten Dienstticket; NET USE mit Kerberos, DIR, COPY + FC /B
gleich, Trennen; ohne Tickets „Anmeldung abgelehnt“, mit `/USER:` NTLMv2;
Verbindung auf dem Server abgeschossen (`smbd: client`), danach DIR und
COPY über das Wiederverbinden mit Kerberos. Reine Programmlogik – kein
86Box.

## Domäne Stufe 2: DOMJOIN (20.11.2026)

`DOMJOIN Name [/OU:DN]`, `DOMJOIN /LEAVE`, `DOMJOIN` (Stand) – Ring 3 wie
Stufe 1, vorher `KINIT` mit einem Konto, das Rechner anlegen darf.

- LDAP (RFC 4511) über TCP 389 zum KDC aus der Ticket-Ablage; BER mit den
  DER-Hilfen aus krb5.c (`der_*`, jetzt öffentlich). rootDSE anonym:
  `defaultNamingContext`, `dnsHostName`.
- Anmeldung SASL GSSAPI (RFC 4752): AP-REQ für `ldap/<dnsHostName>` (aus
  der Ablage oder vom KDC), dann das Angebot der Sicherheitsstufen
  (Wrap-Token, 4 Byte) prüfen und Stufe 4 (Vertraulichkeit) wählen – AD
  setzt `unicodePwd` nur über eine verschlüsselte Verbindung. Danach
  jede LDAP-Nachricht als 4 Byte Länge + Wrap-Token (RFC 4121, versiegelt,
  Verwendung 24 senden/22 empfangen; `krb_wrap`/`krb_unwrap` in krb5.c,
  auch ungesiegelte Token mit Prüfsumme und RRC werden gelesen).
- addRequest `CN=Name,CN=Computers,<Basis>` (oder `/OU:`): objectClass
  computer, sAMAccountName `Name$`, userAccountControl 4096,
  dNSHostName, SPN `HOST/Name` und `HOST/name.domäne`, unicodePwd
  (`"Kennwort"` als UTF-16LE, 28 Zufallszeichen aus `tls_random`). Gibt es
  das Konto schon (68), wird nur das Kennwort ersetzt (modify replace).
  50 = keine Berechtigung, 32 = Konto an anderer Stelle.
- Probe: `krb_kinit("Name$")` mit dem neuen Kennwort. Ablage
  `X:\DOS\SYSTEM\DOMAIN.KEY` (Startlaufwerk, versteckt+System): Realm,
  Domäne, Name, DC, DN, Kennwort, AES-256-Schlüssel (Salz REALM + "host" +
  name.domäne, 4096 Runden) – Grundlage für die Anmeldung beim Start und
  den Wechsel des Maschinenkennworts. `/LEAVE` löscht das Konto per LDAP
  delRequest und überschreibt die Datei vor dem Löschen.

TCPIP.SYS dabei: eine Verbindung im TIME_WAIT ohne Handle gibt ihre zwei
Pufferblöcke sofort ab (`tw_release`) – KINIT + zwei DOMJOIN hintereinander
hielten sonst 2 s lang alle 16 Blöcke („kein Socket frei“).

Test-DC: Samba hatte die SPN `ldap/dc1…` des DC nicht angelegt
(`samba_spnupdate` scheiterte an einem fehlenden Python-Modul); ein echtes
AD hat sie – im Test von Hand mit `samba-tool spn add` nachgetragen.
Geprüft in QEMU: Beitritt (Konto neu und vorhanden), Stand, zweimal
hintereinander erneut beitreten, Benutzer ohne Recht („Keine
Berechtigung“), `/LEAVE` (Konto auf dem DC weg, DOMAIN.KEY weg),
Probe-Anmeldung als `ATPC1$`. Reine Programmlogik – kein 86Box.

RC4-HMAC als Rückfall (20.11.2026; Anlass: beim Nutzer meldete KINIT
gegen sein Samba 4.22.11 Fehler 14 – das Konto hatte nur einen RC4-
Schlüssel, Kennwort aus der Zeit vor AES): `krbcrypt.c` `krb_key23`
(MD4 des Kennworts in UTF-16LE), `krb_enc23`/`krb_dec23` (RFC 4757:
K1 = HMAC-MD5(Schlüssel, Verwendung), Prüfsumme HMAC-MD5(K1, Konfusor 8 ||
Daten), RC4 mit HMAC-MD5(K1, Prüfsumme); Verwendung 3 gilt als 8),
`krb_cksum23` (hmac-md5, Typ −138). krb5.c bietet 18 und 23 an, nimmt aus
PA-ETYPE-INFO2 AES, wenn da, sonst RC4; Sitzungsschlüssel je Ticket 18
oder 23 (`kcred.etype`), Authenticator und TGS-Prüfsumme danach. DOMJOIN
verlangt für LDAP weiter einen AES-Sitzungsschlüssel (Wrap-Token nach
RFC 4121). Geprüft in QEMU: k2.sh wie bisher (AES) und mit einem Testbau,
der nur 23 anbietet – KINIT, Dienstticket, SMBCLI -k signiert, get/put.
Dazu NET `/USER:\\DOM\Name` (Backslashes vorn wurden Teil des Namens –
Anmeldung als Gast, Freigabe verweigert).

## Domäne Stufe 3: Anmeldung beim Start (21.11.2026)

`LOGON [Name] [/NOHOME] [/Q]` (apps/task_logon.c) – Zeile in
AUTOEXEC.CMD eines beigetretenen Rechners, Ablauf wie die Anmeldung von
Windows NT:

1. `DOMAIN.KEY` lesen (Realm, Domäne, DC, Rechnerschlüssel), KDC = DC aus
   der Datei (sonst der Name der Domäne). Name aus dem Aufruf, sonst
   `%USERNAME%`, sonst Frage; Kennwort ohne Echo. Drei Versuche an der
   Tastatur, bei umgeleiteter Eingabe einer (Name und Kennwort je eine
   Zeile).
2. KDC prüfen wie pam_krb5/sssd („validate“): Dienstticket für den eigenen
   Rechner `host/name.domäne` holen und mit dem Rechnerschlüssel öffnen
   (Etype 18: AES-Schlüssel aus DOMAIN.KEY; 23: MD4 des Rechnerkennworts),
   Verwendung 2; der Sitzungsschlüssel im EncTicketPart muss der sein, den
   der KDC in der TGS-Antwort nannte. Nur der echte DC kennt den
   Rechnerschlüssel – ein falscher KDC, der jedes Kennwort annimmt, wird
   abgewiesen und es wird nichts abgelegt.
3. Tickets ablegen (TGT + host-Ticket), Umgebung setzen: `USERNAME`,
   `USERDOMAIN` (erster Teil der Domäne, groß), `USERDNSDOMAIN`,
   `LOGONSERVER` (`\\DC`). Damit die Werte in CMD ankommen, kennt
   FAPI_SETENV jetzt CX = `ENV_CALLER` (1): Eintrag in der Umgebung des
   Aufrufers (task.parent) – drei Zeilen im Kernel; spätere Programme und
   START-Sitzungen erben sie von CMD.
4. Heimlaufwerk (außer `/NOHOME`): LDAP über `apps/ldapc.c` (aus DOMJOIN
   herausgelöst: Verbindung, rootDSE, SASL GSSAPI mit Vertraulichkeit),
   Suche `(sAMAccountName=Name)` im Baum nach `homeDirectory` und
   `homeDrive`; `\\Rechner\Freigabe` aus homeDirectory wird mit `NET USE`
   verbunden (Kerberos, Vorgabe H:), `HOMEDRIVE`/`HOMESHARE`/`HOMEPATH`
   (Rest des Pfads, Windows bindet das Laufwerk an den tieferen Pfad –
   hier nur die Freigabe).

ERRORLEVEL 0 angemeldet, 1 gescheitert, 2 Aufruf, 3 angemeldet, aber
Heimlaufwerk nicht verbunden. DOMJOIN setzt jetzt
`msDS-SupportedEncryptionTypes` = 24 (AES) wie Windows – sonst gab Samba
das host-Ticket mit RC4 aus (beides wird geprüft). KINIT ohne `@REALM`:
`%USERDNSDOMAIN%`, dann DOMAIN.KEY, dann der DNS-Suffix der Karte (DHCP).

Geprüft in QEMU gegen den Samba-Test-DC: Beitritt, KDESTROY, `LOGON test`
mit homeDirectory `\\dc1.atdos.test\daten\test` – Umgebung in CMD,
KLIST mit host-, ldap- und cifs-Ticket, `DIR H:\`, TYPE der Datei im
Heimverzeichnis; falsches Kennwort -> abgewiesen; DOMAIN.KEY mit
verfälschtem Kennwort (RC4-Ticket) und verfälschtem AES-Schlüssel
(Konto mit Typ 24) -> „Der DC konnte sich nicht ausweisen“; KINIT ohne
Realm mit und ohne DOMAIN.KEY (QEMU `domainname=`). Reine Programmlogik
und eine FAPI-Erweiterung ohne Hardware – kein 86Box.

## HOSTS und HOSTNAME (21.11.2026, Anregung des Nutzers)

- `HOSTS` wie `/etc/hosts`: `X:\DOS\SYSTEM\HOSTS` (X = Laufwerk von
  COMSPEC) oder `%ETC%\HOSTS` (Variable wie bei OS/2 TCP/IP). Gelesen in
  Ring 3 von `net_lookup` (apps/net.h) vor dem DNS: jede Zeile `Adresse
  Name [Namen]`, `#` bis Zeilenende Kommentar, Namen ohne Groß/klein,
  Familie wie `-4`/`-6`. Kein Laden, kein Cache – Änderungen gelten sofort,
  TCPIP.SYS bleibt unberührt. NSLOOKUP fragt weiter nur den DNS (wie
  nslookup unter Windows/Linux). Mitgeliefert nur mit Kommentaren (der
  Stapel hat kein Loopback, also kein `localhost`). Puffer klein
  (64 + 160 Byte) – SFTP steht knapp an 64 KB.
- `HOSTNAME.EXE` wie `/etc/hostname`: Datei `HOSTNAME` am selben Ort, eine
  Zeile. `HOSTNAME Name` schreibt sie und setzt den Namen,
  `HOSTNAME /LOAD [/Q]` (AUTOEXEC.CMD, vor `IPCONFIG /WAIT`) setzt ihn aus
  der Datei, `HOSTNAME` zeigt ihn. Gesetzt über FAPI_NET `NET_HOSTNAME`
  (7) in TCPIP.SYS (`host`, DHCP-Option 12; eine gebundene Adresse wird
  gleich erneuert, damit der DHCP-Server den neuen Namen sieht) und als
  `COMPUTERNAME` in der Umgebung von CMD (`ENV_CALLER`). Ohne Datei
  bleibt es bei `/HOST:` bzw. „ATDOS“.

Geprüft in QEMU: PING über drei HOSTS-Namen (Kommentar am Zeilenende,
Tabulator, letzte Zeile ohne Zeilenende, Groß/klein), `PING -4` auf einen
reinen IPv6-Eintrag -> nicht gefunden; HOSTNAME setzen, falsche Namen
abgelehnt, IPCONFIG /ALL zeigt ihn, PING nach dem Erneuern; nach Neustart
`HOSTNAME /LOAD /Q` -> Name und COMPUTERNAME wieder da. LOGON und
HOSTNAME liegen auf Diskette 2 (Diskette 1 war voll).

Weiter (offen): Wechsel des Maschinenkennworts, SRV-Einträge im Auflöser, Ticket erneuern, SMB 3
(AES-CMAC, Verschlüsselung), Sperren (SHARE) über das Netz.
