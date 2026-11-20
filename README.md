# alt-dos – AT-DOS 6.2 (Zweig `6.2`, Release `v6.2`; freigegeben: 6.1 auf `6.1`, 6.0 auf `6.0`, 286-Linie 5.4 auf `5.4`, 5.5 im Test auf `5.5`): Netzwerk mit TCP/IP und IPv6, Firewall, WGET, FTP, TELNET, SNTP, Karten NE2000/RTL8139/PCnet/e1000, Sperren je Laufwerk auch für CD-Dateisysteme, acht Loop-Laufwerke

**AT-DOS** ist ein Hobby-Betriebssystem aus einer alternativen Zeitlinie.
1986 zeigte Microsoft mit dem „Multitasking-DOS 4.0“ einen eigenen Weg zu
mehreren gleichzeitig laufenden Programmen, verfolgte ihn dann aber nicht
weiter und setzte zusammen mit IBM auf OS/2. AT-DOS fragt: Was wäre, wenn
diese Linie weitergeführt worden wäre?

- **AT-DOS 5.0/5.1** läuft auf dem **286** im Protected Mode. Es bietet
  mehrere Sitzungen, eine eigene Programmschnittstelle (FAPI, die einzige
  Brücke zur Hardware), nachladbare Treiber und Dateisysteme, lange
  Dateinamen und einen Platten-Cache im Kernel. Dazu kommen Werkzeuge im
  Stil von MS-DOS 6 (EDIT, FDISK, FORMAT, CHKDSK, XCOPY, …) und GNU (LESS,
  GREP, CUT, TAIL, …), ein Dateimanager, VGA-Grafik und Ton über die Sound
  Blaster. Version 5.1 bringt eine Kompatibilitätsbox wie OS/2 1.x:
  FreeDOS im echten Real Mode, mit Zugriff auf die AT-DOS-Laufwerke.
- **AT-DOS 6.0** nutzt den **386**: Paging, präemptives Multitasking und
  mehrere DOS-Boxen im V86-Modus. Jede Box hat XMS, EMS, DPMI, eine
  virtuelle VGA und eine virtuelle Sound Blaster, darin laufen Spiele wie
  DOOM. Dazu kommen FAT32, VESA-Grafik, PCI-SCSI (NCR 53C8xx),
  IDE-Busmaster, APM und Speicher bis knapp 4 GB. Der Kernel bleibt dabei
  bewusst 16 Bit und schlank; Programme bleiben 16 Bit und laufen auf
  beiden Linien. 6.1 brachte Setup-Disketten, Auslagerung wie OS/2 1.x,
  Loop-Laufwerke und DoubleSpace/DriveSpace, 6.2 das Netzwerk (TCP/IP
  mit IPv6 und Firewall, WGET, FTP, TELNET, SNTP).

Getestet wird in QEMU, Bochs und vor allem in **86Box**, auf
nachgebildeten Maschinen vom 286 mit SCAT-Chipsatz bis zum ASUS P3B-F.
Geschrieben in C (Open Watcom) und Assembler (NASM), Lizenz
**GPL-2.0-or-later**. „AT“ steht doppelt: für die Rechnerklasse, die es
voraussetzt, und für die Alternative – wie bei DR-DOS und PC-DOS.

Zweige: `main` = Weiterarbeit (6.3), `6.2` = Stand zur Prüfung vor der
Freigabe, `6.1`/`6.0` = freigegebene 386-Fassungen, `5.x` = Weiterarbeit
der 286-Linie (freigegeben: `5.0` bis `5.4`, `5.5` zur Prüfung).

## Was läuft

- Standard-FAT12-Diskette: Der Kernel ist die Datei `ATDOS.SYS` (versteckt,
  System), die wie einst `IO.SYS` als erste Datei zusammenhängend liegt
- Diskettentreiber im Kernel (Controller 765/82077, DMA-Kanal 2, IRQ 6) –
  im Protected Mode gibt es kein BIOS mehr. Laufwerke A: und B: mit Typ
  aus dem CMOS, Formate 360 KB, 720 KB, 1,2 MB, 1,44 MB und 2,88 MB (auch
  360 KB im 1,2-MB-Laufwerk mit Doppelschritt); das Format erkennt der
  Treiber beim Einlegen über die Datenrate, den Wechsel über die
  Wechselleitung. Spurpuffer: gelesen wird spurweise, geschrieben am Ende
  jedes Aufrufs in zusammenhängenden Stücken. Spuren formatieren (FAPI 70)
- IFS-Schicht; FAT12 und FAT16 stecken wie bei OS/2 fest im Kernel, mit
  Unterverzeichnissen, Partitionstabelle und 32-Bit-Sektornummern
- Schreiben auf FAT12/16 (Diskette, IDE, SCSI): anlegen, schreiben,
  löschen, Verzeichnisse anlegen/entfernen, umbenennen und verschieben.
  Daten gehen sofort auf den Datenträger, die FAT am Ende jeder Operation in
  beide Kopien; Zeitstempel aus der CMOS-Uhr. Gewechselte Disketten
  werden neu eingelesen, Schreibschutz wird erkannt
- Parallele Plattenzugriffe (6.0, Phase 6): Sperren je Laufwerk und je
  Treiber statt einer Kernelsperre – Kopieren auf verschiedene Laufwerke
  läuft gleichzeitig, siehe „Treiber mit Interrupt“
- FAT32 (6.0): Partitionen vom Typ 0Bh/0Ch werden eingehängt, gelesen
  und beschrieben, lange Namen dort immer im Windows-95-Format (VFAT);
  Sauber-Bit (FAT[1] Bit 27) und FSInfo wie Windows 95/98. CHKDSK nennt
  das Dateisystem und prüft FAT32 mit `UFAT32.EXE` (Ketten, Größen,
  Querverbindungen, verlorene Ketten -> FILEnnnn.CHK, FAT-Kopien; die FAT
  bleibt auf der Platte, Bitmaps in eigenen Segmenten). UNDELETE,
  DEFRAG (auch `/U`) und der Oberflächentest `CHKDSK /R` (bzw. SCANDISK)
  gehen auf FAT32 ebenso über `UFAT32.EXE`. `FORMAT X:
  /FS:FAT32 [/Q] [/V:Name]` legt FAT32 an (Clustergröße wie Windows 98,
  ohne /Q mit Oberflächentest). FDISK fragt wie Windows 98 nach
  „Unterstützung für große Festplatten“ und legt dann Partitionen ab
  512 MB als FAT32 an (0Bh, über 8 GB 0Ch), FORMAT nimmt dort FAT32 von
  selbst. `SYS` macht FAT32-Partitionen startfähig (Bootsektor mit
  zweitem Teil in Sektor 2, LBA-Erweiterungen des BIOS, sonst CHS).
  `CONVERT X: /FS:FAT32 [/V]` wandelt eine FAT16-Partition an Ort und
  Stelle in FAT32 um, ohne Daten zu löschen (wie CVT.EXE von Windows 98,
  auch das Startlaufwerk im Betrieb): erst CHKDSK, dann `UFAT32
  /CONVERT` – kleinere Zuordnungseinheiten (Tabelle wie FORMAT), die
  Daten bleiben liegen, nur der künftige FAT-Bereich und der feste Stamm
  werden in freie Cluster kopiert; Partitionstyp 06h/04h -> 0Bh,
  0Eh -> 0Ch, Startcode von AT-DOS wird zum FAT32-Startcode. Bis zur
  Rückfrage ändert es nichts; lange Namen im EA-Format werden dabei in
  VFAT-Einträge übertragen (wie Windows 95/98 sie liest) und `EA DATA. SF`
  gelöscht – mit `LONGNAMES=EA` in der CONFIG.SYS bleiben sie in der
  EA-Datei, und FAT32 benutzt dann ebenfalls das EA-Format (bei AUTO,
  wenn im Stamm eine `EA DATA. SF` liegt). `CONVERT X: /FS:FAT16 [/V]`
  wandelt umgekehrt eine FAT32-Partition bis etwa 2 GB zurück in FAT16
  (größere Zuordnungseinheiten; erst werden Dateien so geordnet, dass je
  neue Zuordnungseinheit nur eine Datei liegt – unterbrechbar, das
  Laufwerk bleibt dabei FAT32 –, dann FAT, Stamm mit 512 Einträgen,
  Bootsektor und Partitionstyp 0Bh -> 06h, 0Ch -> 0Eh; lange Namen im
  EA-Format bleiben, VFAT-Namen ebenso; `/LFN:VFAT` bzw. `/LFN:EA`
  zusammen mit `/FS:` legt die Form der langen Namen danach fest, in
  beide Richtungen; den geänderten Partitionstyp liest der Kernel gleich
  neu ein, hin und zurück geht ohne Neustart; ist das Laufwerk belegt –
  etwa das Startlaufwerk mit Auslagerungsdatei –, merkt CONVERT es wie NT
  für den nächsten Start vor, wandelt dann um und startet neu). `CONVERT X: /LFN:VFAT` bzw.
  `/LFN:EA` wandelt die langen Namen eines FAT-Laufwerks (FAT12/16/32,
  auch Disketten) nachträglich in die eine oder andere Form (hin und
  zurück; Zeichen außerhalb CP437 werden beim Weg nach EA zu `_`; der
  feste Stamm von FAT12/16 wächst nicht – passt etwas nicht, bleibt alles,
  wie es war; `/LFN:EA` ohne VFAT-Namen legt eine leere `EA DATA. SF`
  als Merker an, damit neue lange Namen auch bei `LONGNAMES=AUTO` und
  FAT32 ins EA-Format kommen). Umleitungen nehmen Namen in Anführungszeichen
  (`echo x > "Brief an Hans.txt"`)
- Gerätetreiber als Dateien: `CONFIG.SYS` lädt per `DEVICE=` Ring-0-Treiber,
  die über DevHlp mit dem Kernel sprechen. Dabei: `AHA154X.SYS` (Adaptec
  AHA-1540/1542, SCSI, Busmaster mit Mailboxen; läuft auch mit dem
  kompatiblen BusLogic BT-545C) und `IDE.SYS` (bis zu
  vier IDE-Platten an primärem und sekundärem Kanal, PIO, LBA oder CHS für
  Platten ohne LBA; ATAPI-CD-ROMs als Einheit; dazu MFM/RLL-Platten am
  WD1003 und ESDI am WD1007, Geometrie aus der Tabelle des BIOS) – beide lesen und schreiben,
  CD-ROMs (2048-Byte-Blöcke) nur lesen
- Die mitgelieferte `CONFIG.SYS` nennt alle Einstellungen mit Erklärung
  (Treiberschalter, Cache, lange Namen, Land/Codepage, Boxen); was nicht
  gebraucht wird, steht als `REM`-Beispiel darin.
- F8 beim Start wie bei MS-DOS 6 (auch schon während der BIOS-Meldungen
  getippt; danach wartet der Kernel eine Sekunde): jede Zeile der
  `CONFIG.SYS` wird mit `[J,N]?` gefragt, dann „AUTOEXEC.CMD ausführen“
  und jede ihrer Zeilen (`REM` nicht); Esc führt den Rest ohne Fragen
  aus. CMD bekommt dafür vom Kernel `/STEP`. Eine `CONFIG.SYS` über
  8 KB wird mit Meldung übergangen (vorher still).
- `CONFIG.SYS` wie MS-DOS: `DEVICE=`, `DEVICEHIGH=` (auch mit `/L:`),
  `IFS=`, `FILES=` (Tabelle offener Dateien, 8–255, Vorgabe 20),
  `CACHE=kb[,s[,max]]` (Platten-Cache wie SMARTDRV, aber im Kernel wie
  bei OS/2: Festplatten schreiben s Sekunden verzögert, Vorgabe 5, `,0`
  sofort. Dynamisch wie bei Windows NT (6.0): kb ist die Mindestgröße
  (ohne Angabe nach Speicher, z. B. 128 KB bei 2 MB), darüber wächst der
  Cache in freien Speicher bis max (ohne Angabe halber Speicher,
  höchstens 8 MB; `CACHE=1024,5,1024` = feste Größe), lässt immer 1/8
  des Speichers (512 KB–4 MB) frei und gibt saubere 16-KB-Stücke ab,
  sobald ein Programm oder eine DOS-Box Speicher braucht; DPMI/XMS der
  Boxen zählen den abgebbaren Teil als frei. MEM zeigt Größe, Spanne,
  „verfügbar“ und Trefferquote; `BUFFERS=` gilt nur noch als
  Untergrenze), `SWAPPATH=[X:]\Verzeichnis [KB]` (6.1, wie OS/2:
  Auslagerungsdatei `SWAPPER.DAT` für die DOS-Boxen; ohne Angabe auf
  Rechnern bis 16 MB in `\SWAP` des Startlaufwerks (eigenes Verzeichnis:
  nicht im Stamm, nicht bei den Systemdateien), doppelte
  RAM-Größe, 2–8 MB; `SWAPPATH=OFF` schaltet ab. Reicht der Speicher
  nicht – auch nicht, nachdem der Platten-Cache abgegeben hat –, gehen
  selten benutzte Seiten der Boxen in die Datei und kommen beim nächsten
  Zugriff zurück; so laufen z. B. sechs Boxen auf einem 4-MB-Rechner,
  drei auf 2 MB. MEM zeigt Größe und Belegung. Wie bei OS/2 1.x wird
  außerdem der Code ruhender Programme verworfen und bei Bedarf aus der
  EXE nachgeladen, Box-Seiten und Datensegmente von Programmen ziehen um,
  damit Programme ihre Segmente am Stück bekommen, und Datensegmente
  ruhender Programme gehen ebenfalls in die Datei – sie kommen zurück,
  sobald das Programm sie anfasst), `LONGNAMES=AUTO|VFAT|EA`, `DOSSTART=SESSION`, `DOS=HIGH,UMB`, `RMSIZE=` und `EMS_FRAME_LOCATION=NONE` (für die DOS-Boxen, wie bei OS/2); `STACKS=`, `FCBS=`, `BREAK=`, `LASTDRIVE=`, `DOS=`, `SHELL=`,
  `COUNTRY=nnn` (Land wie MS-DOS: Datum, Uhrzeit, Tausendertrenner in CMD
  und FM, auch in der DOS-Box; je Sitzung mit `SET COUNTRY=nnn` änderbar),
  `CODEPAGE=850,437` (wie OS/2: vorbereitete Codepages, die erste gilt ab
  dem Start; `CHCP 437`/`CHCP 850` schaltet um – Zeichensatz auf dem
  Bildschirm, Umsetzung langer Dateinamen und die Codepage der DOS-Box;
  vorhanden: 437, 850, 852, 857, 858, 860, 863, 865, 866, 737,
  Zeichensätze aus FreeDOS cpidos in `\DOS\SYSTEM\CPnnn.FNT`),
  `SET=` und die Menübefehle von MS-DOS 6 werden angenommen bzw.
  übergangen, Zeilen mit `;` sind Kommentare
- Nachladbare Dateisysteme: `IFS=` in `CONFIG.SYS`. Erstes Modul ist
  `ISO9660.IFS` für CD-ROMs an IDE (ATAPI) und am Adaptec; jedes CD-Laufwerk
  bekommt einen Buchstaben, ein CD-Wechsel wird bei jedem Zugriff erkannt.
  Joliet-CDs (Windows 95) zeigen lange Namen in Originalschreibung (auch
  Umlaute), ebenso CDs nach ISO 9660:1999 (Namen bis 207 Zeichen) und
  Unix-CDs mit Rock Ridge (Namen bis 255 Zeichen, UTF-8, auch tiefe
  Verzeichnisse); gibt es mehrere, gilt Joliet; ältere Programme bekommen 8.3-Ersatznamen wie Windows
  (`BRIEFA~1.TXT`), beide Namen gehen in Pfaden. `VOL` zeigt die
  Bezeichnung der CD
- Unicode-Dateinamen verlustfrei (Stufe 1): Namen mit Zeichen außerhalb
  von CP437 (Japanisch, Kyrillisch, Griechisch …) sehen Programme mit `_`
  an diesen Stellen, Kopieren (COPY, XCOPY, FM), Verschieben und der
  Weg von einer Joliet-CD auf FAT behalten aber das Original – der Kernel
  merkt es sich je Task, wenn ein Programm den Namen liest, und schreibt
  es beim Anlegen unter genau diesem Namen. Auf EA-Laufwerken stehen
  solche Namen als UTF-16 in `EA DATA. SF`; `CONVERT /LFN` überträgt sie
  in beide Richtungen ohne Verlust. Alle Zeichen von CP437 (auch α, ░,
  ∞) werden jetzt richtig nach Unicode umgesetzt (vorher nur Latin-1)
- Lange Namen, Stufe 1: VFAT-Namen (Windows 95) werden gelesen – in
  Verzeichnislisten, als Pfadteil (`CD "Eigene Dateien"`, Anführungszeichen
  halten Namen mit Leerzeichen zusammen) und beim Löschen/Umbenennen mit
  aufgeräumt; Pfade bis 260 Zeichen. Wie bei OS/2 1.2 sehen nur Programme
  mit dem Merker „kann lange Namen“ (`FX_FLAG_LONGNAMES`, bisher COMMAND)
  lange Namen, alle anderen 8.3-Kurznamen (`EIGENE~1`).
- Lange Namen, Stufe 2: auf VFAT-Laufwerken (`LONGNAMES=VFAT` in
  CONFIG.SYS oder `AUTO`, wenn im Stamm schon lange Einträge stehen)
  legen `MD`, `COPY`, `REN` lange Namen wie Windows 95 an (Kurzname
  `NAME~1.EXT`); kleingeschriebene 8.3-Namen bekommen wie bei Windows NT
  nur das Kleinschreibungsbit
- Lange Namen, Stufe 3: auf allen anderen FAT-Laufwerken (`LONGNAMES=EA`
  oder `AUTO`) eigenes Format wie bei IBM/OS/2, aber FAT32-fest: die Datei
  `EA DATA. SF` im Stamm hält zu jeder Datei (Verzeichniscluster + 8.3-Name
  als Rückverweis) den langen Namen; im Verzeichniseintrag steht nichts,
  MS-DOS sieht nur Kurznamen und eine versteckte Datei. XCOPY und MOVE
  kopieren lange Namen mit; Befehlszeilen bis 127 Zeichen; `DIR /B`, `/W`
- `AIC7XXX.SYS`: Adaptec AHA-2940 / 2940 Ultra (UW) / 2944UW (PCI,
  AIC-7870/7880) – Platten, Wechselplatten, CD, mit PCI-Interrupt
  (`/NOIRQ` = Abfragebetrieb); Wide-Karten (2940W/UW, 2944UW) mit
  IDs 0–15 und 16 Bit je Gerät (WDTR), `/NOWIDE` bleibt schmal. Die
  Geräte dürfen sich während eines Befehls vom Bus trennen (IDENTIFY mit
  Trennerlaubnis, Wiederauswahl per Interrupt): eine CD, die gerade sucht,
  hält die Platte am selben Adaptec nicht mehr auf – jedes Gerät hat
  seine eigene Sperre im Kernel. `/NODISC` schaltet das ab
- `AHCI.SYS`: SATA-Platten und -CD/DVD am AHCI-Controller (Intel ab
  ICH6, QEMU `-M q35` bzw. `-device ahci`, Proxmox/VirtualBox/VMware mit
  SATA) – LBA48, Wechselplatten, EJECT, DMA über den Controller, PCI-
  Interrupt (`/NOIRQ` = Abfragebetrieb), jeder Port mit eigener Sperre
  (zwei Platten arbeiten gleichzeitig). Start von einer SATA-Platte geht
  über das BIOS (SeaBIOS kann es); entladbar mit `DEVLOAD /U AHCI`
- VirtIO (6.3, QEMU/Proxmox mit i440fx; Legacy-Schnittstelle der
  Spezifikation 1.0, `drivers/virtio.h`): `VIOBLK.SYS` für virtio-blk-
  Platten (je Platte eigene Sperre, Lesen bis 16 Sektoren am Stück, FLUSH,
  schreibgeschützte Platten, PCI-Interrupt oder `/NOIRQ`; Start von der
  VirtIO-Platte über das INT 13h von SeaBIOS) und `VIONET.SYS` für
  virtio-net (16 Empfangs-, 8 Sendepuffer, MAC und Verbindung aus der
  Gerätekonfiguration). Reine VirtIO-1.0-Geräte ohne Legacy-Teil (q35 mit
  PCIe-Ports) melden sich beim Start mit einem Hinweis; beide entladbar
  mit `DEVLOAD /U`
- Netzwerk (6.2, erste Stufe; Entwurf `docs/NETZWERK.md`):
  `RTL8139.SYS` für Realtek RTL8139/8139C/8139C+ (PCI; QEMU
  `-device rtl8139`, 86Box, Proxmox) mit IRQ und Busmaster,
  `NE2000.SYS` für NE1000/NE2000 und Nachbauten (ISA, RTL8019AS, PCI-
  Klone wie RTL8029; sucht 300h, 280h, 320h, 340h, 360h, 240h und den
  IRQ selbst, `/P:300 /I:3` legt fest, `/NOIRQ` = Abfragebetrieb),
  `PCNET.SYS` für AMD PCnet-PCI (Am79C970/970A/971/972/973/975 – die
  Karte von VMware, VirtualBox, QEMU `-device pcnet` und 86Box) und
  `E1000.SYS` für Intel PRO/1000 (8254x, z. B. 82540EM/82545EM – QEMU/
  Proxmox `e1000`, VirtualBox, VMware; Register über den Speicher-BAR),
  beide mit IRQ und Busmaster, `/NOIRQ`, `/PROMISC`, entladbar; darüber
  `TCPIP.SYS` mit ARP, IPv4, ICMP und DHCP-Client –
  `DEVICE=TCPIP.SYS [/IP:a.b.c.d/n /GW:x /DNS:x[,y]] [/HOST:name]`
  (ohne /IP holt er die Adresse per DHCP; /DNS gilt auch dann). `IPCONFIG [/ALL] [/RELEASE]
  [/RENEW]` und `PING [-t] [-n n] [-l Größe] [-w ms] [-i TTL] Ziel` wie
  bei Windows; AT-DOS antwortet selbst auf ARP und PING. Beide lassen
  sich mit DEVLOAD laden und (erst TCPIP, dann die Karte) entladen.
  Stufe 2a: Sockets für Programme (Handles wie Dateien – DosRead/
  DosWrite/DosClose gehen, am Programmende schließt der Kernel sie), UDP,
  DNS-Auflöser mit Cache und Suffix aus DHCP (`NSLOOKUP Name`, `PING
  Name`), `NC -u Rechner Port [Text]` / `NC -u -l Port` zum Testen.
  Firewall ab Werk an (`DEVICE=TCPIP.SYS /NOFW` schaltet sie ab):
  eingehend nur Antworten auf eigene Anfragen, PING nur aus dem eigenen
  Netz, geschlossene Ports schweigen. Stufe 2b: TCP (Verbinden,
  Lauschen/Annehmen, Slow Start, schnelles Wiederholen, Nagle,
  verzögertes ACK, RTO nach Jacobson) – `NC Rechner Port [Text]`,
  `NC -l Port`, `NC r p < DATEI` / `> DATEI`. Firewall-Regeln je
  Programm: `FIREWALL ALLOW NC.EXE TCP 5556 FROM LOCAL` (Protokoll TCP/
  UDP/ICMP/ANY, Ports von-bis, Herkunft Netz/n, LOCAL oder ANY; Lauschen
  allein öffnet nichts), `FIREWALL` zeigt Zustand, Zähler und Regeln,
  `FIREWALL ON|OFF`, `LOG ON|OFF`, `DELETE n`, `CLEAR`; Regeln stehen in
  `\DOS\SYSTEM\FIREWALL.INI` (AUTOEXEC.CMD lädt sie mit `FIREWALL /LOAD
  /Q`), Abgewiesenes gedrosselt im DMESG. `NETSTAT [-a]` zeigt
  Verbindungen und offene Ports mit Programm, `IPCONFIG /WAIT[:s]` wartet
  in Stapeldateien auf die DHCP-Adresse. Härtung: Anfangsfolgenummern
  und Absenderports zufällig (Zufall aus Takt und Netzverkehr), höchstens
  4 halboffene Verbindungen (gegen SYN-Fluten), RST nur mit genau
  passender Folgenummer (sonst ACK zur Prüfung, RFC 5961), keine IP-
  Optionen, Absender 0/127/Multicast/eigene Adresse verworfen, kein
  Echo auf Rundruf; DHCP- und DNS-Antworten nur auf eigene Anfrage
  (Kennung und Port zufällig), bei SYN-Fluten wird ein zufälliger
  halboffener Eintrag geopfert. Geprüft mit dem Störsender
  `tools/netfuzz.py` (über 30.000 verstümmelte Rahmen in QEMU). `SELECT` wartet auf bis zu 8 Handles zugleich –
  `NC -l -k Port` nimmt damit bis zu 7 Verbindungen gleichzeitig an
  (Eingegangenes mit Absender, getippte Zeilen an alle). Geprüft in QEMU und 86Box
  (386DX/40 mit NE2000, P3B-F mit RTL8139). IPv6 (neben IPv4, ab Werk
  an, `DEVICE=TCPIP.SYS /NOIPV6` schaltet ab): link-lokale Adresse aus
  der MAC (EUI-64) mit Prüfung auf Doppelte (DAD), Router-Anfrage und
  -Werbung, globale Adresse per SLAAC, Standard-Router und DNS-Server
  aus der Werbung (RDNSS), Neighbor Discovery, PING beantworten und
  senden, UDP, TCP und DNS (AAAA) über IPv6; Programme sehen dieselben
  Sockets – ein `AF_INET6`-Socket nimmt auch IPv4 (`::ffff:a.b.c.d`).
  IPCONFIG zeigt die IPv6-Adressen, PING/NC/NSLOOKUP nehmen IPv6-
  Adressen und Namen (`-4`/`-6` wählt die Familie), FIREWALL-Regeln
  nehmen IPv6-Netze (`FROM fd00::/8`, `LOCAL` = link-lokal oder eigenes
  Präfix). Noch nicht: DHCPv6, MLD, Fragmente, Datenschutzadressen
- `WGET` holt Dateien über HTTP/1.1 (Bedienung wie GNU wget): `-O Datei`
  (`-O -` = Standardausgabe), `-P Verzeichnis`, `-c` setzt eine
  abgebrochene Datei fort (Range), `-q`, `-S` (Kopfzeilen), `-T s`,
  `-4`/`-6`; Content-Length oder chunked, bis zu 5 Weiterleitungen,
  Fortschritt mit KB/s, Dateiname aus der URL (auch lang, `%20` wird
  Leerzeichen); vorhandene Dateien nur mit `-O`/`-c`. ERRORLEVEL wie
  GNU wget (3 Datei, 4 Netz, 5 TLS, 8 Fehlerantwort). https:// (6.3)
  über den eigenen TLS-1.2-Client `apps/tls.c` (ECDHE mit X25519,
  AES-128-GCM, Extended Master Secret, SNI; Zertifikate mit RSA oder
  ECDSA P-256/P-384), der den Server prüft wie GNU wget: Signatur des
  Schlüsselaustauschs (RSA PKCS#1 v1.5 und PSS, ECDSA), Kette bis zu einer
  Wurzel aus `\DOS\SYSTEM\CACERT.PEM` (Liste von Mozilla über curl,
  MPL-2.0, `certs/`), Name (subjectAltName, sonst CN, auch `*.`) und
  Gültigkeit; `--ca-certificate=Datei` nimmt eine andere Liste,
  `--no-check-certificate` lässt die Prüfung weg
- `SMBCLI` (6.3) holt und legt Dateien auf Windows-/Samba-Freigaben ab
  (Bedienung angelehnt an smbclient): `SMBCLI \\Rechner\Freigabe
  [-U Benutzer[%Kennwort]] [-W Domäne] [-k] [-c "Befehl; Befehl"]
  [-p Port] [-4|-6]`, Befehle dir/ls, cd, pwd, get, put, del, md, rd, lcd, help,
  quit (auch aus einer umgeleiteten Eingabe). SMB 2.0.2/2.1, Anmeldung
  NTLMv2 in SPNEGO (ohne `-U` als Gast) oder mit `-k` per Kerberos
  (Ticket von KINIT), signiert mit HMAC-SHA256, wenn
  der Server es verlangt; Namen CP437 <-> UTF-16, Dateien bis 4 GB in
  Stücken zu 16 KB. ERRORLEVEL 0 ok, 1 Befehl gescheitert, 2 Aufruf,
  4 Netz/Anmeldung
- `NET USE X: \\Rechner\Freigabe [Kennwort|*] [/USER:[Domäne\]Name]`
  (6.3) verbindet eine Freigabe als Laufwerk, `NET USE` zeigt die
  Verbindungen, `NET USE X: /DELETE` trennt. Bedient wird das Laufwerk
  von einem losgelösten SMBCLI im Ring 3 (FAPI 93 DosUfs, wie FUSE); alle
  Programme und die DOS-Boxen sehen es wie eine Platte (lange Namen,
  8.3-Ersatznamen für alte Programme); bricht die Verbindung ab, verbindet
  der Dienst beim nächsten Zugriff selbst neu, offene Dateien bleiben offen;
  ohne Kennwort und `/USER:` mit Kerberos, wenn KINIT Tickets abgelegt hat)
- `KINIT [Name[@REALM]] [/KDC:Adresse]`, `KLIST`, `KDESTROY` (6.3):
  Anmeldung an einer Domäne (Active Directory, Samba-AD, MIT) mit
  Kerberos v5, aes256-cts-hmac-sha1-96, KDC über TCP 88; Tickets in
  `%TEMP%\KRB5CC.DAT` (oder `%KRB5CCNAME%`), die Uhr kommt vom KDC
- `FTP` wie das ftp von MS-DOS/Windows: `FTP [-n] [-v] [-i] [-d] [-A]
  [-4|-6] [-s:Datei] [Rechner [Port]]`, Befehle open, close, user, ls,
  dir, cd, cdup, lcd, pwd, get/recv, put/send, mget, mput, delete,
  mdelete, mkdir, rmdir, rename, binary, ascii, type, size, prompt, hash,
  passive, verbose, debug, status, quote/literal, bye/quit, help
  (eindeutig abkürzbar). Passiv als Vorgabe (PASV, bei IPv6 EPSV),
  `passive` schaltet auf aktiv (PORT/EPRT – braucht eine Firewall-Regel
  für FTP.EXE). Platzhalter bei mget/mdelete wertet FTP selbst aus (wie
  Unix: `d1*` trifft `d1.dat`). Ausgaben gehen nach STDOUT (umleitbar),
  `-s:Datei` oder umgeleitete Eingabe für Skripte
- `TELNET [-4|-6] [-b] [-e] Rechner [Port]`: Terminal mit ANSI-Farben,
  Optionen ECHO, SUPPRESS-GO-AHEAD, Terminaltyp „ANSI“, Fenstergröße
  (NAWS); ohne ECHO des Servers Zeilenbetrieb mit eigenem Echo (SMTP,
  POP3 …). Pfeil-, Bild-, Pos1/Ende- und F1–F4-Tasten als VT100-Folgen,
  `-b` sendet ^H statt DEL. Strg+] beendet
- `SSH [-p Port] [-l Benutzer] [-4|-6] [-v] [-t] [Benutzer@]Rechner
  [Befehl]` (6.3): Anmeldung über SSH-2 – curve25519-sha256 (strenger
  Schlüsseltausch gegen „Terrapin“), Hostschlüssel ssh-ed25519,
  ecdsa-sha2-nistp256/384, rsa-sha2-512/256, Verschlüsselung aes128/256-gcm
  oder aes128/256-ctr mit hmac-sha2-256(-etm), Anmeldung mit Kennwort oder
  keyboard-interactive. Ohne Befehl ein Terminal wie xterm im Vollbild
  (Farben, Rollbereiche, Liniengrafik, UTF-8 -> CP437), mit Befehl geht
  die Ausgabe auf STDOUT (umleitbar) und der Rückgabewert wird ERRORLEVEL.
  `~.` am Zeilenanfang trennt. Bekannte Rechner in
  `\DOS\SYSTEM\SSHHOSTS.TXT` (oder `%SSH_HOSTS%`, Fingerabdruck SHA256
  wie OpenSSH), beim ersten Mal wird nachgefragt
- `SFTP [-P Port] [-l Benutzer] [-b Datei] [Benutzer@]Rechner` (6.3):
  Dateien über SSH (SFTP 3): ls/dir, cd, pwd, lcd, lpwd, get, put, mget,
  mput, rm, mkdir, rmdir, rename, chmod, `!Befehl`, bye
- `SNTP [Server] [/N] [/Q] [/W:s] [-4|-6]` stellt die Uhr aus dem Netz
  (SNTP, Server aus `%NTPSERVER%`, sonst pool.ntp.org; `/N` nur
  anzeigen). Zeitzone über `TZ` im POSIX-Format wie Unix/DJGPP, z. B.
  `SET TZ=EST5EDT,M3.2.0,M11.1.0`; ohne TZ Mitteleuropa mit Sommerzeit
  (`CET-1CEST,M3.5.0,M10.5.0/3`). Die Uhr hält wie bei DOS Ortszeit,
  gestellt wird auf die volle Sekunde; Beispiel in AUTOEXEC.CMD
- `DIR /O` sortiert wie MS-DOS (`N`, `E`, `S`, `D`, `G`, `-` kehrt um; `/O`
  allein = `GN`), `DIRCMD` gilt auch in CMD, `DIR C*` heißt `C*.*`,
  unbekannte Schalter werden gemeldet, `DIR /?` zeigt die Schalter;
  `DIR /S` zeigt auch alle Unterverzeichnisse (je Verzeichnis mit
  Treffern Kopf und Summe, am Ende „Anzahl angezeigter Dateien“; mit
  `/B` die vollen Pfade)
- Umleitung und Pipes auch für DOS-Programme aus CMD: `prog > datei`,
  `>>`, `prog < datei` und `a | prog | b` – die Box bekommt dafür statt
  CON die Geräte `ATIN$`/`ATOUT$`/`ATERR$` (ATBOX.SYS), die über den
  Kernel in die Handles des Aufrufers lesen und schreiben. Ein DOS-
  Programm, das als Teil einer Pipe im Hintergrund läuft, übernimmt den
  Bildschirm nicht mehr. Programme, die direkt in den Bildschirmspeicher
  oder über das BIOS schreiben, lassen sich wie unter MS-DOS nicht umleiten
- Lange Namen, Stufe 4: CHKDSK prüft VFAT-Ketten und die Sätze in
  `EA DATA. SF` und räumt mit `/F` verwaiste lange Namen auf (z. B. nach
  Umbenennen mit einem alten DOS-Werkzeug). EDIT zeigt lange Namen in der
  Dateiliste, die Tab-Ergänzung setzt Anführungszeichen wie bash
- Laufwerke `A:`, `B:` (Disketten), `C:`, `D:` … für jede FAT-Partition
- Start von Diskette oder von einer FAT16-Partition (IDE oder SCSI), siehe
  „Start von Festplatte“
- Programmlader für das eigene FX-Format. Beim Start läuft der
  Befehlsinterpreter aus `PROTSHELL=` (Vorgabe `CMD.EXE /P`), der
  `AUTOEXEC.CMD` abarbeitet. Wie bei OS/2 gehören `.CMD`-Dateien der nativen
  Seite, `AUTOEXEC.BAT`, `.BAT` und `SHELL=` gehören der DOS-Box
- Der Befehlsinterpreter heißt `CMD.EXE` (bis 6.0 `COMMAND.EXE`), wie bei
  OS/2 und NT – `COMMAND.COM` gehört der DOS-Box. Eine alte CONFIG.SYS mit
  `PROTSHELL=COMMAND.EXE` startet trotzdem (der Kernel nimmt dann `CMD.EXE /P`)
- `CMD.EXE`: Zeileneditor und Befehlsspeicher wie DOSKEY eingebaut
  (Pfeil auf/ab, links/rechts, Pos1/Ende, Einfg/Entf, F3, Esc, Bild auf/ab;
  `DOSKEY /HISTORY` listet die letzten 16 Befehle); Makros wie MS-DOS 6: `DOSKEY name=text` mit
  `$1`–`$9`, `$*`, `$T`, `$G`/`$G$G`/`$L`/`$B`, `DOSKEY name=` löscht,
  `/MACROS` zeigt, `/MACROFILE=datei` lädt Zeilen `name=text`; nur für
  getippte Zeilen, je Sitzung, mit START gestartete CMD-Sitzungen erben
  sie; `\DOS\LINUX.MAC` bringt die gewohnten Namen von Linux mit – `ll`,
  `la`, `ls`, `..`, `pwd`, `cp`, `mv`, `rm`, `clear`, `diff`, `man`,
  `free`, `ps`, `kill`, `bg` (= DETACH), `jobs` (= TASKMAN /L), `uname`,
  `history`, `nano`/`vi`, `mc` –, nicht
  aktiv, Beispiel in AUTOEXEC.CMD). **Tab** vervollständigt
  wie bash: erstes Wort aus internen Befehlen und Programmen im PATH, sonst
  Dateien und Verzeichnisse; mehrdeutig ergänzt den gemeinsamen Anfang,
  zweites Tab listet die Möglichkeiten.
  Befehlsinterpreter mit `DIR` (Verzeichnisse zuerst, `/A` alle bzw.
  `/A:HSDRA` mit `-` wie MS-DOS, `/P`, Muster, freier Platz), `CD`,
  Laufwerkswechsel, `TYPE`, `COPY` und `DEL` (auch mit `*` und `?`; COPY
  erhält Datum/Uhrzeit und verkettet mit `A+B [Ziel]`), `HELP [Befehl]`, `MD`,
  `SET`, `PATH`, `PROMPT` (`$P $G $N $D $T $V $_ …`), Stapeldateien (`.BAT`
  mit `@`, `ECHO ON/OFF`, `ECHO.`, `REM`, `PAUSE`, `GOTO`, `IF [NOT]
  EXIST/==/ERRORLEVEL` – Programme liefern Rückgabewerte, `IF ERRORLEVEL n`
  heißt „n oder größer“ wie bei MS-DOS, `CMD /C` reicht den Wert durch –, `CALL`, `SHIFT`, `%0`–`%9`, `%NAME%`), `AUTOEXEC.BAT`
  beim Start (`CMD /P`), `CMD /C`; Programme werden wie bei
  COMMAND.COM erst im aktuellen Verzeichnis, dann über `PATH` gesucht
  (`.EXE`, `.CMD`, `.BAT`; `.COM` wird als MS-DOS-Programm erkannt),
  Umleitung `<`, `>`, `>>` und Pipes `|` (echte Pipes, alle Teile laufen
  gleichzeitig), `RD`, `REN` (auch `*` und `?` wie MS-DOS), `CLS`, `VER` (mit Build-Nummer), `ECHO`, `START`, `SHUTDOWN` (`/H` anhalten, `/NOW` ohne Rückfrage, `/T:n` nach n Sekunden Countdown – Esc bricht ab, `/R`
  neu starten; auch `POWEROFF`, `REBOOT` = `SHUTDOWN /R`: schließt alle Dateien, leert die
  Platten-Caches und schaltet den Rechner aus – über ACPI (S5, Tabellen
  liest der Kernel selbst), sonst über das APM des BIOS, sonst Meldung wie
  bisher; `APM=OFF` in der CONFIG.SYS schaltet das Ausschalten ab,
  `ACPI=OFF` nimmt nur APM), `HILFE`, `EXIT`; andere Eingaben
  starten das gleichnamige Programm, auch mit Pfad.
  Stapelsprache vollständig wie MS-DOS 6.22 und OS/2 CMD: dazu `FOR %%v IN
  (Satz) DO` (Platzhalter werden zu Dateinamen; an der Eingabe `%v`),
  `GOTO :EOF`, `SETLOCAL`/`ENDLOCAL` (Umgebung, Laufwerk und Verzeichnis;
  am Dateiende automatisch), `EXTPROC` in der ersten Zeile, Verketten mit
  `&`, `&&`, `||` und `^` zum Maskieren, Argumente in Anführungszeichen
  bleiben ein Wort; Strg+C fragt „Stapelverarbeitung abbrechen (J/N)?“.
  Außerdem `TRUENAME` (echter Pfad hinter SUBST/JOIN), `BREAK`, `VERIFY`,
  `CHCP`, `CTTY COM1` (Bedienung über die serielle Leitung), `KEYS
  ON|OFF|LIST`, `LH`, `DETACH` (Programm im Hintergrund, ohne Konsole: es nimmt keine Tasten weg, liest von der Tastatur Dateiende wie `< NUL`), `CD A:\X` von C:
  aus (jedes Programm merkt sich das Verzeichnis weiterer Laufwerke),
  `PROMPT $E` für ANSI-Farben; `DIR` zeigt JOIN-Verzeichnisse als
  `<JUNCTION>`
- Werkzeuge wie MS-DOS 5/6 (Schalter und Meldungen wie dort): `ATTRIB`
  (`+R -A … /S`), `XCOPY` (`/S /E /A /M /P /I /K /Y` – `/I` wie NT: fehlendes Ziel ist ein Verzeichnis, ebenso ein Ziel mit `\` am Ende –, Datum bleibt erhalten, Schreibschutz wird wie bei NT nur mit `/K` übernommen – Kopien von CD sind sonst schreibgeschützt),
  `MOVE`, `DELTREE`, `TREE` (`/F /A`), `LABEL`, `CHOICE` (`/C /N /S /T`,
  ERRORLEVEL), `SORT` (`/R /+n`), `FC` (Text mit Resynchronisation wie
  MS-DOS, `/B /C /N /nnnn`, Platzhalter wie `FC A:\*.* C:\ZIEL` oder
  `*.TXT *.BAK`); in COMMAND dazu `VOL`, `DATE`, `TIME`.
- `FORMAT X: [/V[:Name]] [/Q] [/S] [/F:Größe] [/FS:FAT]` wie MS-DOS, gebaut wie OS/2:
  FORMAT startet `U<Dateisystem>.EXE /FORMAT` aus `\DOS\SYSTEM` (für FAT
  `UFAT.EXE`). Disketten (2,88 MB, 1,44 MB, 1,2 MB, 720 KB, 360 KB; `/F:`
  wie MS-DOS: 360, 720, 1.2, 1.44, 2.88) und Partitionen
  (FAT12 bei Typ 01h, sonst FAT16 mit den Clustergrößen von MS-DOS 6);
  Disketten werden ohne `/Q` Spur für Spur formatiert, danach wird jeder
  Sektor gelesen und Fehlerhaftes gesperrt. Das Startlaufwerk wird nicht formatiert.
  Unformatierte Partitionen bekommen trotzdem einen Buchstaben.
  Wechselplatten (ZIP, MO) bekommen wie unter MS-DOS das Datenträgerbyte F0h.
  Auf Wechselplatten setzt AT-DOS keinen Merker „nicht sauber abgemeldet“ (wie bei Disketten).
  FDISK legt Partitionen wie MS-DOS nur über ganze Zylinder bis zum letzten an, den das BIOS meldet.
- `DISKCOPY [A: [B:]] [/V]` wie MS-DOS: alle Diskettenformate, mit einem
  Laufwerk über Diskettenwechsel, in mehreren Durchgängen, wenn der
  Speicher nicht reicht; unformatierte oder anders formatierte Ziel-
  disketten werden beim Kopieren formatiert, die Kopie bekommt eine neue
  Seriennummer
- `UNDELETE [Pfad][Muster] [/LIST] [/ALL]` wie MS-DOS 5 (ohne Löschschutz):
  Verteiler wie CHKDSK, die Arbeit macht `UFAT /UNDELETE` – Startcluster
  frei, dann Startcluster + nächste freie Cluster; erstes Zeichen wird
  abgefragt (`/ALL`: `#`)
- `CHKDSK /R` (wie Windows NT): Oberflächentest, defekte freie Cluster
  werden gesperrt, belegte verlegt (Lesbares gerettet); `SCANDISK X:` ruft
  `CHKDSK X: /F /R`
- Drucker: Gerätenamen `PRN`, `LPT1`–`LPT3` und `NUL` in jedem Verzeichnis
  (`COPY BRIEF.TXT PRN`, `DIR > LPT1`); der Kernel gibt über den Parallel-
  port aus (Abfragebetrieb, andere Tasks laufen, solange der Drucker
  belegt ist; nach 30 s „nicht bereit“)
- `MODE COMn:b,p,d,s,P` bzw. `MODE COMn BAUD= PARITY= DATA= STOP= RETRY=`,
  `MODE LPTn:80|132,6|8,P` und `MODE` (alle Geräte) wie MS-DOS; `COM1`–`COM4`
  und `AUX` sind Gerätenamen (`COPY DATEI COM1`). Den Port einer seriellen
  Maus gibt der Kernel nicht her
- `PRINT [/D:Gerät] [/T] [Datei [/C] [/P] ...]` wie MS-DOS: Warteschlange
  `\DOS\SYSTEM\PRINT.QUE`, gedruckt wird von einem Hintergrund-Task
  (`PRINT /WORK`), `/T` und `/C` brechen auch den laufenden Druck ab
- `DEFRAG [X:] [/F | /U] [/V]` wie MS-DOS 6: Verteiler, die Arbeit macht
  `UFAT /DEFRAG` – fragmentierte Dateien zusammenlegen, mit `/F` (Vorgabe)
  Dateien in Lücken weiter vorn schieben; je Datei erst kopieren, dann neue
  Kette, dann Verzeichniseintrag, dann alte Kette frei (Absturz kostet
  nichts); Verzeichnisse bleiben an ihrem Platz
- `MSD [/S]` Systemdiagnose wie MSD von MS-DOS 6: Knöpfe mit Kurzangabe
  (Computer, Speicher, Video, Betriebssystem, Maus, Laufwerke, LPT, COM,
  IRQ-Status mit Besitzer, Treiber mit Ports/DMA; PCI-Geräte auf der
  Computer-Seite), Einzelheiten im Fenster, Bedienung mit Buchstaben,
  Pfeilen oder Maus; `/S` als Bericht auf die Standardausgabe
- `TASKMAN [/L | /K Nummer|Name]` Taskmanager: zeigt Programme und
  DOS-Boxen (Sitzung, Zustand, Größe, Befehlszeile), Entf bzw. `/K`
  beendet einen Task samt allem, was er gestartet hat – z. B. eine
  hängende DOS-Box in einer anderen Sitzung; geschützt sind TASKMAN, seine
  Aufrufer und die Shell der ersten Sitzung. **Strg+Umschalt+Esc** (wie
  Windows NT) holt ihn von überall nach vorn, auch über eine hängende Box
- `SUBST [X: [Y:]Pfad] | X: /D` wie MS-DOS: der Kernel setzt X:-Pfade auf
  das Verzeichnis um; Programme sehen X: (aktuelles Verzeichnis, Eingabeaufforderung)
- `JOIN [X: [Y:]Pfad] | X: /D` wie MS-DOS 3.1–5.0: hängt ein Laufwerk in ein
  leeres Verzeichnis ein (Gegenstück zu SUBST, im Kernel statt in COMMAND –
  CHKDSK, FORMAT usw. sehen weiter das echte Laufwerk)
- `DISKCOMP [A: [B:]]` (Disketten Spur für Spur, Seriennummer zählt
  nicht), `COMP [Daten1] [Daten2] [/D /A /L /N=n /C]` (Byte für Byte, bis
  10 Unterschiede, Muster wie `*.BAK`), `REPLACE Quelle [Ziel] [/A /P /R
  /S /U /W]` und `EXPAND Datei … Ziel` (entpackt SZDD-Dateien wie `*.EX_`
  von Microsoft-Disketten, der Name kommt aus dem Kopf zurück) wie MS-DOS;
  dazu `COMPRESS [/R] Quelle [Ziel]` wie aus den Microsoft-Entwickler-
  paketen (packt ins SZDD-Format, `/R` macht `BRIEF.TX_`)
- `CUT -c|-b|-f LISTE [-d X] [-s] [--complement] [--output-delimiter=T]
  [Datei…]` nach dem Vorbild von GNU cut (Zeichen oder Felder je Zeile,
  auch in Pipes: `DIR /B | CUT -c1-8`)
- Packprogramme (eigener Deflate-Kern nach RFC 1951, kein fremder Code;
  auf Setup-Diskette 2, gebunden – laufen auch unter MS-DOS und in den
  Boxen): `GZIP`/`GUNZIP [-d -c -k -f -l -t -v -1..-9] [Datei…]` wie GNU
  gzip (`BRIEF.TXT` -> `BRIEF.TXT.gz`, ohne Datei als Filter, mehrteilige
  `.gz`), `TAR -c|-t|-x [-v -z -k] [-f Archiv] [-C Verz] [Name…]` wie GNU
  tar (`tar xzvf a.tgz`, ustar mit langen Namen über 100 Zeichen, gzip
  wird beim Lesen erkannt), `ZIP [-r -j -0..-9 -q] Archiv Datei…` (ergänzt
  vorhandene Archive) und `UNZIP [-l -v -t -o -n -j -q -d Ziel] Archiv
  [Name…]` wie Info-ZIP (PKZIP 2.x: gespeichert und Deflate)
- Entpacker für ältere DOS-Archive (eigener Kern `lzh.c`): `UNARJ`,
  `UNZOO` und `UNLHA [e|x|l|v|t] [-l -v -t -o -n -j -q -d Ziel] Archiv
  [Ziel\] [Name…]` – jedes erkennt ARJ, ZOO und LHA am Inhalt, auch in
  selbstentpackenden `.EXE`/`.COM`. ARJ Methode 0–4, ZOO gespeichert/
  LZW/lh5, LHA/LHarc/LArc lh0/lh1/lh4–lh7/lz4/lzs/lz5 (Kopfstufen 0–2);
  `unarj e a.arj c:\ziel\` wie das Original
- `LHA` und `ARJ` legen solche Archive auch an (eigener Packer `lzhenc.c`):
  `LHA a|u|f|m|d [-r] [-o5|-o6|-o7|-z] [-h1|-h2] Archiv [Datei…]` und
  `ARJ a|u|f|m|d [-r] [-m0..-m3] Archiv [Datei…]` – hinzufügen, nur
  Neueres, nur Vorhandenes erneuern, verschieben, löschen; `x e l v t`
  wie UNLHA/UNARJ. Was nicht kleiner wird, wird gespeichert. Die
  Archive lesen LHA 2.x/lhasa bzw. ARJ 2.x/3.x
- `ZOO a|u|f|m|d [-r] [-z] Archiv[.ZOO] [Datei…]` legt ZOO-Archive an und
  pflegt sie (immer lh5 wie `zoo ah`, Zusätze am Befehl wie `ah` werden
  angenommen); alte Einträge, auch LZW, bleiben erhalten, gelöschte
  fallen beim Neuaufbau weg; `x e l v t` wie UNZOO
- `7ZDEC [e|x|l|t] [-l -t -o -n -j -q -d Ziel] Archiv[.7Z] [Ziel\]
  [Name…]` entpackt 7z-Archive von 7-Zip (Copy, LZMA, LZMA2, BCJ x86,
  Delta; gepackter Kopf, feste und einzelne Ordner) und `.LZMA`-Dateien
  (eigener LZMA-Kern `lzma.c`). Wörterbuch im Speicher bis 16 MB, wenn
  er reicht; sonst über eine Zwischendatei mit kleinem Ring – so entpackt
  es jedes Archiv. Nicht: BCJ2, PPMd,
  Verschlüsselung
- `7ZA a [-r] [-mx=0..9] [-q] [-y] Archiv[.7Z] [Datei…]` legt 7z-Archive
  an (eigener LZMA-Packer `lzmaenc.c`, Wörterbuch nach Stufe 128 KB
  (1–3), 512 KB (4–5), 1 MB (6), 2 MB (7), 4 MB (8–9), kleiner bei wenig
  Speicher – Bedarf etwa das Dreifache; fester Ordner,
  Namen UTF-16, Datum, Attribute; `-mx=0` speichert); `x e l t` ruft
  7ZDEC. 32-Bit-Programme (PE, LE/LX von DOS/4GW, ELF) kommen in einen
  eigenen Ordner mit BCJ-x86-Filter davor (auch mit DOS/4GW gebundene
  wie DOOM.EXE). 7-Zip liest die Archive. Nur
  neue Archive (`-y` überschreibt), noch kein gepackter Kopf
- Textwerkzeuge nach dem Vorbild von GNU, alle auch in Pipes:
  `HEAD [-n N] [-c N] [-q|-v] [Datei…]` (Anfang, Vorgabe 10 Zeilen),
  `TAIL [-n N|+N] [-c N] [-f] [Datei…]` (Ende; `-f` bleibt dran und zeigt,
  was andere Programme anhängen – Taste beendet), `WC [-l -w -c -m -L]`
  (Zeilen, Wörter, Bytes, Summenzeile), `UNIQ [-c] [-d|-u] [-i] [Ein
  [Aus]]` (gleiche Nachbarzeilen, meist hinter `SORT`) und `TR [-c -d -s]
  Satz1 [Satz2]` (Zeichen ersetzen/löschen, Bereiche `a-z`, `\r`,
  `[:upper:]` …), z. B. `TYPE BRIEF.TXT | TR -d \r` oder
  `SORT < NAMEN.TXT | UNIQ -c`; `CAT [-A -b -e -E -n -s -t -T -v]
  [Datei…]` hängt Dateien aneinander (Platzhalter, `-` = Standardeingabe,
  nummerieren, Leerzeilen zusammenfassen, Steuerzeichen sichtbar);
  `STRINGS [-f] [-n N] [-t d|o|x] [-o] [-w] [-e s|S|l|b] [Datei…]` wie
  GNU strings: lesbare Zeichenketten in Programmen und anderen
  Binärdateien (Mindestlänge, Stelle in der Datei, `-e l` UTF-16);
  `MD5SUM`/`SHA1SUM [-b] [--tag] [Datei…]` bilden Prüfsummen wie GNU,
  `-c Liste [--quiet] [--status] [-w] [--ignore-missing]` prüft sie
  (auch Listen von Linux mit `/`), ERRORLEVEL 1 bei Abweichung;
  `HEXDUMP [-C|-b|-c|-d|-o|-x] [-n n] [-s n] [-v]` wie util-linux und
  `XXD [-a -b -c n -e -g n -i -l n -o n -p -r -s n -u] [Ein [Aus]]` wie
  xxd – `XXD -r` wandelt zurück und überschreibt in einer vorhandenen
  Datei nur die Stellen der Zeilen (Patchen: `XXD -r PATCH.TXT PROG.EXE`);
  `DMESG [-c] [-C] [-n Zeilen]` zeigt die letzten 8 KB der Kernelmeldungen
  mit der Zeit seit dem Start (`[0:00:32]`): Startzeilen, Treiber,
  abgebrochene Programme und DOS-Boxen – `DMESG > LOG.TXT` nach einem
  Fehler
- `FM [Verz1 [Verz2]]`, der AT-DOS-Dateimanager: zwei Verzeichnisfenster
  nebeneinander (Tab wechselt), Einfg/Grau + - * markieren, F3 Ansehen
  im eigenen Betrachter nach dem Vorbild von mcview (beliebig große
  Dateien, F2 Umbruch, F4 Hex, F5 Gehe zu Zeile/Offset, F7 Suchen mit
  Hervorhebung, `n` weiter), F4 Bearbeiten (EDIT), F5 Kopieren, F6 Verschieben/Umbenennen,
  F7 Verzeichnis, F8 Löschen (Verzeichnisse mit Inhalt; Kopieren,
  Verschieben und Löschen lassen sich mit Esc oder dem Knopf abbrechen,
  eine halb kopierte Datei wird entfernt), Alt+F1/F2
  Laufwerk, Strg+O zeigt den Bildschirm der Sitzung, Befehlszeile unten
  (Tippen, Eingabe; Strg+Eingabe übernimmt den Namen), Maus (auch die
  Knöpfe in den Dialogen), lange Namen
- ANSI wie bei OS/2 fest in der Bildschirmausgabe (kein ANSI.SYS nötig):
  Cursor, Löschen, Farben; `ANSI [ON|OFF]` je Sitzung
- Leere Disketten- und CD-Laufwerke melden „Laufwerk nicht bereit - keine
  Diskette/CD eingelegt“ (CD: der Treiber fragt per REQUEST SENSE nach;
  eine anlaufende CD wartet er bis 10 s ab)
- `TEMP` und `TMP` stehen wie bei Windows von Anfang an in der Umgebung
  (`X:\TEMP` auf dem Startlaufwerk, beim Start von Diskette `C:\TEMP`,
  wenn es C: gibt; `SET TEMP=` in CONFIG.SYS geht vor). `CMD /P` legt
  das Verzeichnis beim Start an
- `FDISK [/STATUS] [/MBR]` wie MS-DOS 5/6: primäre und erweiterte
  Partitionen, logische Laufwerke, aktive Partition, löschen, anzeigen;
  `/MBR` erneuert den Startcode (der MBR von AT-DOS lädt über LBA und die
  BIOS-Geometrie). Logische Laufwerke in erweiterten Partitionen hängt der
  Kernel beim Start ein, auch auf Platten von MS-DOS.
- `SYS [Quelle:] Ziel: [/A]` überträgt `ATDOS.SYS`, `CMD.EXE`, `\DOS\SYSTEM`,
  `CHKDSK.EXE` und (falls fehlend) `CONFIG.SYS`/`AUTOEXEC.CMD`, dazu den
  Startcode in den Bootsektor; mit `/A` zusätzlich alle Programme aus
  `\DOS`, die DOS-Box (`\DOS\MDOS`), die Klangbänke
  (`\DOS\SYSTEM\SFBANK`) und `\DEMO` – vollständige Installation. `ATSYS` (unter
  MS-DOS) kopiert dieselben Verzeichnisse; Systemdateien (z. B. eine
  dorthin gelegte `SWAPPER.DAT`) lassen beide aus. Disketten nur leer (`FORMAT A: /S`). Der
  Diskettenbootsektor meldet „Keine Systemdiskette“, wenn `ATDOS.SYS` fehlt.
- `CHKDSK [X:] [/F] [/V]` wie MS-DOS: prüft FAT, Ketten, Dateigrößen,
  Querverbindungen und verlorene Zuordnungseinheiten; `/F` repariert
  (verlorene Ketten als `FILEnnnn.CHK`, Größen, FAT-Kopien). CHKDSK kennt
  selbst kein Dateisystem: Es fragt den Kernel (`DosQFSInfo`) und startet
  `U<Dateisystem>.EXE` aus dem PATH, für FAT also `UFAT.EXE` – wie
  `UFAT.DLL`/`UHPFS.DLL` bei OS/2. Ein neues Dateisystem bringt sein `.IFS`
  und sein `U….EXE` mit. Für `/F` wird das Laufwerk gesperrt; geht das nicht
  (Dateien offen), merkt CHKDSK es wie Windows NT für den nächsten Start vor
  (`\AUTOCHK.LST`), `CMD /P` prüft dann vor `AUTOEXEC.CMD`.
  Querverbindungen behebt `/F` wie ScanDisk: Der gemeinsame Rest wird für
  die zweite Datei in freie Cluster kopiert. ERRORLEVEL wie bei NT: 0 keine
  Fehler, 1 behoben, 3 nicht behoben oder nicht prüfbar. GREP liefert wie
  GNU grep 0/1/2 (Treffer/keiner/Fehler), FIND wie MS-DOS 6 ebenso.
  Wie bei NT und OS/2 merkt sich jede FAT16-Platte, ob sie sauber
  abgemeldet wurde (FAT[1] Bit 15, wie Windows 95/NT; MS-DOS stört es
  nicht): beim ersten Schreiben gelöscht, bei `SHUTDOWN`, `POWEROFF` und
  Strg+Alt+Entf gesetzt. Fehlt es beim Start, prüft `CMD /P` die Platte
  mit `CHKDSK /F`. `AUTOCHECK=CD` in der CONFIG.SYS beschränkt das auf
  bestimmte Laufwerke, `AUTOCHECK=` ohne Buchstaben schaltet es ab.
- Sitzungen wie bei OS/2: jedes per `START` gestartete Programm hat einen
  eigenen Bildschirm, **Alt+Esc** wechselt, **Strg+C** bricht ab,
  **Strg+Alt+Entf** schließt alle Dateien, leert die Platten-Caches und
  startet neu; hängt ein Programm, nach spätestens 5 s trotzdem (zweimal
  drücken: sofort)
- Tastaturtreiber (IRQ 1), deutsche Belegung mit Umlauten und AltGr fest
  eingebaut; `KEYB US`, `GB`, `FR`, `SP`, `IT`, `DK`, `NO`, `SV`, `PL`
  (Polnisch für Programmierer, AltGr + Buchstabe) lädt andere Belegungen aus
  `.KBL`-Dateien. Die Belegungen sind Unicode (vier Ebenen: normal,
  Umschalt, AltGr, Umschalt+AltGr; Feststelltaste auch für Umlaute und
  Akzentbuchstaben); der Kernel leitet daraus die Zeichen der aktiven
  Codepage ab, auch nach `CHCP`. `KEYB DK,865` stellt die Codepage gleich
  mit um; fehlen Zeichen in der Codepage, sagt KEYB wie viele
- A20 über den Tastaturcontroller, GDT/IDT, Wechsel per `LMSW`
- PIC auf 20h/28h umgelegt, Systemtakt 100 Hz
- FAPI-Call-Gate (Selektor 0023h) von Ring 3 nach Ring 0
- je Task ein 286-TSS und eine eigene LDT, Ring 3, IOPL 0
- Speicherverwaltung in 4-KB-Seiten bis knapp 4 GB (6.0; was ein 32-Bit-
  Adressraum ohne PAE erreicht – auf einer VM mit 4 GB meist 3–3,5 GB,
  der Rest liegt oberhalb 4 GB): Speicherkarte des
  BIOS per INT 15h E820h (Lücken werden ausgespart), sonst E801h bzw.
  88h; Segmente auch oberhalb 16 MB, per `DosAllocSeg`/`DosFreeSeg`,
  Aufräumen bei Task-Ende; ISA-DMA-Puffer bleiben unter 16 MB
- VESA für FAPI-Programme (6.0): Modi mit linearem Bildspeicher bis
  1024×768×32 (VBE 2.0/3.0, FAPI VioVesa), auf älteren Karten mit
  VBE 1.2 (ET4000, Cirrus, frühe S3) über das umschaltbare 64-KB-Fenster
  bei A0000h – für Programme gleich (`VESA` zeigt „linear“/„Fenster“);
  bei ET4000(/W32) und Cirrus GD54xx schaltet der Kernel das Fenster
  direkt in der Karte um statt über das BIOS (auch getrennte Lese-/
  Schreibfenster wie bei den Tseng-BIOS). Den Modus stellt der Kernel
  wie OS/2 1.x über einen kurzen Ausflug in den Real Mode mit dem Video-
  BIOS ein; das Programm sieht das Bild über ein 64-KB-Fenster, das
  VioSetPlane weiterschiebt (passt zu 286-Programmen). Sitzungswechsel
  wie bei VGA-Grafik (Bild und Palette gesichert, im Hintergrund wird
  weitergezeichnet). Mauszeiger (MouShowPtr) zeichnet der Kernel wie
  bei VGA in allen Farbtiefen, Koordinaten in Bildpunkten bis zum
  Bildrand. `VESA` listet die Modi, `\DEMO\VDEMO [Breite [Bit]]` zeigt
  einen Modus, linke Maustaste malt
- Paging (6.0): der Kernel bildet beim Start allen RAM 1:1 über
  Seitentabellen ab und schaltet den Seitenmechanismus des 386 ein –
  Grundlage für Speicherschutz je Seite, DOS-Boxen und virtuellen
  Speicher; ein Seitenfehler meldet die Adresse. Der Kernel selbst sieht
  den Speicher ab 2 GB (Alias), so gehört der Bereich 0–4 MB frei der
  jeweiligen DOS-Box. Der Alias reicht bis knapp 2 GB; Speicher darüber
  erreicht der Kernel über umlegbare Seitenfenster und nutzt ihn für
  DOS-Boxen (auch XMS/EMS/DPMI) und den Platten-Cache
- präemptiver Scheduler (6.0) mit Hardware-Taskwechsel (`JMP` auf TSS):
  der Takt-Interrupt unterbricht ein Programm nach seiner Zeitscheibe
  (50 ms), auch wenn es nur rechnet; im Kernel wird nicht unterbrochen,
  gewechselt wird beim Rücksprung ins Programm (wie OS/2 1.x). Test:
  `\DEMO\BUSY [Runden] [/W]` rechnet ohne FAPI-Aufrufe – andere Sitzungen
  bleiben bedienbar, Strg+C bricht ab
- DOS-Boxen (6.0, Phase 3 Stufen 1–2): ein `.COM`-Programm läuft im
  V86-Modus des 386 in einer eigenen virtuellen Maschine – mit eigenem
  erstem MB (640 KB, eigenes Seitenverzeichnis), präemptiv wie jedes
  Programm, geschützt: ein Absturz beendet nur die Box. Der Bildschirm
  der Box (B800) ist der Puffer ihrer Sitzung (Vollbild, 80x25): direktes
  Schreiben kostet nichts, auch im Hintergrund. Virtueller Takt (INT 8/
  1Ch, 18,2 Hz), virtuelle Tastatur (INT 9, Port 60h/64h), virtueller PIC
  (EOI, Maske; nach `STI` kommt ein Interrupt wie bei der CPU erst nach
  dem nächsten Befehl – Digger setzte darauf; stellt ein Programm den
  Zeitgeber schneller, kommt jeder INT 8 von einem echten Takt-Interrupt,
  ohne Zittern – wichtig für Lautsprecher-Ton); Warteschleifen an 3DAh (Strahlrücklauf), 61h und am PIT
  hängen nicht. BIOS: INT 10h (Textmodus, Cursor, Rollen, Zeichen,
  Zeichenketten), 11h, 12h, 15h, 16h, 1Ah (Takt, Uhrzeit, Datum); DOS
  (bis zum FreeDOS-Kern in Stufe 3) nur Zeichen/Text/Handle 1–2/
  Vektoren/Datum/Uhrzeit/Version/Ende. Endet eine Box in der Sitzung von
  COMMAND, bleibt ihr Text stehen. Alt+Esc und Strg+C bleiben beim
  Kernel. Tests: `\DEMO\HALLO [Text] [/W]`, `\DEMO\BOXTEST` (wie ein
  Spiel: B800, INT 1Ch, INT 9, 3DAh; Esc beendet), auch mit `START`
- Vergleichstest der Box (18.11.2026): `\DEMO\BIOSCMP Datei [X:]` (auch auf Diskette 2) ruft
  rund 180 BIOS- und DOS-Dienste auf und schreibt die Ergebnisse in eine
  Datei; `tools/bioscmp.sh` lässt es unter FreeDOS direkt und in der Box
  laufen und vergleicht. Bestandsliste, Funde und erwartete Unterschiede:
  `docs/BOX-BIOS.md`. Behoben dabei: Dateidatum (5701h) und Attribut beim
  Anlegen blieben nicht erhalten, 5Bh überschrieb vorhandene Dateien,
  Fehlercodes 03h/05h/10h wie DOS, Diskettenparameter (INT 1Eh), INT 10h
  1Ch/1007h/1008h/1015h/1017h/101Ah, INT 13h 01h, INT 14h/17h-Status,
  INT 15h C0h/90h/2400h, INT 16h 09h/0Ah
- SHARE in der DOS-Box (beide Linien): der Redirector teilt eine Datei,
  die ein Programm mehrfach öffnet, über einen Handle von AT-DOS (Größe
  stimmt in allen Handles), prüft die Freigabemodi und Bereichssperren
  (5Ch); INT 2Fh 1000h meldet SHARE als geladen
- Die AT-DOS-Laufwerke gelten in der DOS-Box als lokale Laufwerke
  (IOCTL 4408h/4409h/440Dh, Seriennummer 6900h wie MS-DOS); die FCB-Suche
  findet wie MS-DOS keine versteckten Dateien mehr (Korrektur im
  FreeDOS-Kern, `freedos/patches/`)
- CheckIt Pro 1.11 läuft in der DOS-Box (19.11.2026): Prozessor-, Cache-
  und Speichertests mit Privilegbefehlen (CR0 lesen, INVD) wie unter
  EMM386, Schreiben auf ROM verpufft, virtuelle Register für Parallel-
  und serielle Ports (Drucken auch direkt über die Ports), CMOS-Kopie je
  Box, Weckzeit (INT 1Ah 06h), zweiter PIC; Disketten ohne 55AAh im
  Bootsektor werden gelesen (wie MS-DOS). Details: `docs/BOX-BIOS.md`
- Drucken aus der DOS-Box: INT 17h (also auch `PRN`, `COPY datei PRN`
  und das Drucken aus Programmen) geht über das LPT-Gerät von AT-DOS.
  In 86Box ohne Drucker: LPT1 auf „Generic Text Printer“, „Generic ESC/P
  2 Dot-Matrix“ oder „Generic PostScript Printer“ (PDF) stellen
- DOS-Box mit FreeDOS (6.0, Phase 3 Stufe 3, erster Schritt): **DOSBOX**
  öffnet eine Sitzung, in der der FreeDOS-Kern mit FreeCOM (deutsch)
  läuft – gestartet wie auf einem echten PC vom Bootsektor eines
  Diskettenabbilds (`\DOS\MDOS\FREEDOS.IMG`, Laufwerk A: der Box, nur
  lesend). Jedes andere Startabbild (`.IMG`) lässt sich ebenso mit
  `START` starten. EXIT beendet die Box, Strg+C gehört dem DOS darin.
  Eine Box, die nur auf Tasten wartet, gibt die CPU ab. Alle
  AT-DOS-Laufwerke ab C: sind in der Box da (Redirector wie bei einem
  Netzlaufwerk, über die Dateifunktionen des Kernels – auch CD, SUBST,
  JOIN): DIR, CD, MD, RD, COPY, REN, DEL, TYPE, Umleitung, Programme
  von C: starten, Dateien zum Lesen und Schreiben öffnen, kürzen und
  verlängern. CD-Laufwerke (auch ISO-Abbilder über LOOP) meldet die Box
  wie MSCDEX 2.21 (INT 2Fh 15xxh: Laufwerke, Version, VTOC, Sektoren
  lesen, Gerätaufträge wie Status, Größe, READ LONG) – für CD-Spiele und
  Installationsprogramme, die nach MSCDEX fragen; keine CD-Audio-
  Wiedergabe.
- MS-DOS statt FreeDOS in der DOS-Box (wie „OS/2 für Windows“ mit der
  vorhandenen Windows-Kopie; AT-DOS liefert kein Microsoft-DOS mit):
  **MKDOSIMG** `Quelle: [Werkzeuge] [/N:NAME.IMG]` baut unter AT-DOS aus
  der eigenen Kopie ein Startabbild `\DOS\MDOS\NAME.IMG` (so klein wie
  möglich, meist 360 KB). Quelle ist ein
  Laufwerk mit dem DOS: Startdiskette oder Festplatte mit installiertem
  MS-DOS 4.0–6.22 bzw. PC DOS (auch ein Plattenabbild über LOOP). Kopiert
  werden IO.SYS/MSDOS.SYS bzw. IBMBIO.COM/IBMDOS.COM, COMMAND.COM,
  COUNTRY.SYS, dazu BOXSHELL und ATBOX.SYS und eine eigene CONFIG.SYS; der
  Startsektor ist ein eigener (`boot/mdosboot.asm`), der der Quelle wird
  nicht gebraucht. Die Programme des DOS (`Quelle:\DOS` von selbst, sonst
  das angegebene Verzeichnis) landen in `\DOS\MDOS\NAME\`, vorn im PATH
  der Box, dort auch COMMAND.COM (COMSPEC). `DOS_IMAGE=NAME.IMG` in
  `DOSBOX.INI` wählt es für alle Programme (`[*]`), eines (`[PROG.EXE]`)
  oder die Befehlszeile (`[COMMAND.COM]`). Der Redirector kennt die
  Eigenheiten von MS-DOS (Platzhalter bei REN, FCB-Dateien, Rückgabe von
  6Ch); mit MS-DOS 6.22 in QEMU (Abbild aus Disketten und aus einer
  Festplatte mit installiertem DOS, DIR, COPY/XCOPY/REN/DEL auf den
  AT-DOS-Laufwerken, FCB-Programme, BIOSCMP wie unter echtem MS-DOS 6.22)
  und in 86Box (386DX/40, 4 MB, ohne Diskette: Abbild aus einem Platten-
  abbild über LOOP gebaut, Start, DIR, MEM, `COMMAND /C`) geprüft. PC DOS
  hat denselben Kern (IBM-Lizenz des MS-DOS-Quelltexts) und wird gleich
  behandelt. `tools/doscmp.sh` vergleicht jede Version direkt und in der
  Box: MS-DOS 5.0, 6.0, 6.20, 6.22 und PC DOS 5.0, 6.3, 2000 geprüft
  (QEMU), DOS-Teil gleich. Offen: MEM von MS-DOS zeigt auf knappen Maschinen bei
  XMS „Verwendet“ falsch (`docs/BOX-BIOS.md`)
- DOS-Programme direkt aus CMD.EXE wie bei NT: `.COM`, `.BAT` und `.EXE`
  ohne FX-Kopf startet der Kernel in einer FreeDOS-Box in derselben
  Sitzung (Vollbild, im aktuellen Laufwerk und Verzeichnis), CMD wartet,
  danach steht die Ausgabe im Fenster und ERRORLEVEL ist gesetzt. Das gilt
  für jeden Programmstart (auch START, FM, Stapeldateien). `COMMAND` bzw.
  `COMMAND.COM` öffnet die DOS-Befehlszeile in derselben Sitzung (EXIT kehrt
  zurück), `COMMAND /C befehl` führt einen DOS-Befehl aus. Mit
  `DOSSTART=SESSION` in CONFIG.SYS bekommt wie bei OS/2 jedes DOS-Programm
  eine eigene Sitzung. Die Box bleibt danach geparkt offen (wie NTVDM bei
  NT): FreeDOS startet nur beim ersten DOS-Programm eines Befehlsinterpreters,
  jedes weitere läuft ohne neuen Start; endet der Befehlsinterpreter, wird
  ihr Speicher (≈ 660 KB) frei; MEM zeigt ihn als „DOS-Boxen“. In der Box
  sind A:/B: die Disketten von AT-DOS (das Startabbild braucht sie nach dem
  Start nicht mehr), PATH ist `X:\DOS\MDOS;X:\DOS` (X: = Startlaufwerk;
  `\DOS\MDOS` ist wie `\OS2\MDOS` bei OS/2 das Verzeichnis der DOS-Seite,
  dort liegen auch COMMAND.COM und das MEM von FreeDOS, das XMS, EMS und
  UMB der Box zeigt), die interaktive Box führt `X:\AUTOEXEC.BAT`
  aus, wenn es sie gibt. Eine hängende Box beendet **Strg+Alt+Ende**
  (Strg+C gehört dem DOS darin)
- XMS 3.0 in jeder DOS-Box vom Kernel (wie VXMS bei OS/2, kein HIMEM in
  der Box): bis 2 MB Speicherblöcke je Box (Anlegen, Kopieren, Sperren,
  32-Bit-Funktionen 88h/89h/8Eh/8Fh), A20 je Box. `DOS=HIGH,UMB` in der
  CONFIG.SYS von AT-DOS gilt für alle Boxen: FreeDOS lädt sich in die HMA
  (48 KB mehr konventioneller Speicher) und bekommt die freien Lücken
  zwischen C8000 und EFFFF als UMBs (`LH`, `DEVICEHIGH`); `RMSIZE=` setzt
  den konventionellen Speicher je Box (256–640 KB). Eine Box kostet damit
  ≈ 820 KB
- EMS 4.0 in jeder DOS-Box (wie VEMM bei OS/2, kein EMM386; seit 6.3 im
  Modul `VEMS.SYS`, ohne das Modul meldet INT 67h „kein EMS“):
  Seitenrahmen in der ersten freien 64-KB-Lücke ab C8000 (vor den UMBs),
  Vorgabe 2 MB je Box, bis 32 MB (`EMS_MEMORY_LIMIT`), zusammen 32 MB; Erkennung über `EMMXXXX0` am INT-67h-
  Vektor und als Gerät. Speicher wird erst beim Anlegen belegt. Auch
  55h/56h (Seiten einblenden und springen bzw. aufrufen).
  `EMS_FRAME_LOCATION=NONE` in der CONFIG.SYS (wie OS/2): kein EMS, der
  Rahmen bleibt als UMB frei (64 KB mehr)
- DPMI 0.9 in jeder DOS-Box vom Kernel (wie VDPMI bei OS/2), erste Stufe:
  16- und 32-Bit-Programme, Moduswechsel, Deskriptoren (0000h–000Ch),
  DOS-Speicher (0100h/0101h), Vektoren (0200h–0205h), Real Mode aufrufen
  (0300h–0302h), Speicher bis 1 GB Adressraum (0501h–0503h),
  Gerätespeicher einblenden (0800h/0801h, z. B. linearer Bildspeicher
  einer VBE-2.0-Karte, auch oberhalb 2 GB; RAM nie), virtuelles
  IF; andere Interrupts gehen an den Handler des Programms oder an DOS
  im Real Mode, Ende mit INT 21h 4Ch aus dem Protected Mode; dazu
  Ausnahme-Handler (0202h/0203h), Callbacks (0303h/0304h) und Raw Switch
  (0306h). Mit DOS/4G läuft DOOM 1.2 (Shareware) durch die ganze
  Initialisierung; Takt und Tastatur gehen auch an die Handler im
  Protected Mode – **DOOM 1.2 läuft** (Demo, Menü, Spiel)
- Grafik in DOS-Boxen (virtuelle VGA, Stufe 1): ein Grafikmodus (INT 10h
  00h, VESA 4F02h) gibt der Box im Vordergrund die echte Karte – Modus-
  wechsel über das Video-BIOS der Karte, Bildspeicher A0000–BFFFF und
  Ports 3B0h–3DFh direkt (CGA, EGA, VGA, SVGA-Modi des BIOS). Beim
  Sitzungswechsel sichert der Kernel alle Register, Palette und die vier
  Ebenen (256 KB). Stufe 2: Boxen in 320×200×256 (13h) und den CGA-
  Modi 4–6 laufen im Hintergrund weiter (wie VVGA bei OS/2): sie
  schreiben in einen Schatten, Palette, CRTC und die übrigen VGA-Register
  bildet der Kernel nach; beim Zurückholen kommt das Bild mit allem, was
  hinten gezeichnet wurde. Planare Modi (12h, EGA, Mode X) laufen hinten
  weiter, solange das Programm einfach schreibt (eine Ebene über die Map
  Mask – der Kernel blendet die Ebene im Puffer ein). SVGA-Modi (VESA)
  laufen hinten ebenfalls weiter: über das Bank-Fenster (INT 10h 4F05h
  bildet der Kernel hinten nach) oder über den linearen Bildspeicher
  eines DPMI-Programms (DPMI 0800h) schreibt die Box in die Sicherung
  ihres Bildes. Latch-Kopien (Schreibmodus 1, z. B. DOOM in Mode Y),
  Palette und Register im SVGA-Modus und die meisten Aufrufe des Video-
  BIOS halten die Box an, bis sie wieder vorn ist – Modus abfragen und
  Palette/Attribute über INT 10h (0Fh, 1000h–1017h) bildet der Kernel
  hinten nach. Test: `START \DEMO\BGTEST [4|10|12]` (eine Zeile je Takt,
  bei Zeile 100 Farbe auf Rot), wegschalten, nach ein paar
  Sekunden zurück. EGA und CGA (6.3): deren Register kann man nicht
  lesen – mit `DEVICE=VEGA.SYS` bzw. `DEVICE=VCGA.SYS` (wie bei OS/2)
  schreibt ein Grafikmodul jeden Zugriff der Box mit; dann gibt es Box-
  Grafik mit Sitzungswechsel und Hintergrund (EGA planar 0Dh/0Eh/10h,
  CGA-Modi 4–6). Ohne Modul bleibt die Box auf diesen Karten im Text. AWE32/64: Spiele mit eigenem AWE-Treiber sprechen den
Wavetable-Chip (EMU8000, 620h/A20h/E20h) im Vordergrund direkt an,
hinten ist er stumm. Ein virtueller MPU-401 (330h, UART-Betrieb) gibt
die MIDI-Daten der Box an den MPU-401 der Karte weiter; die Box im
Vordergrund hat Vorrang vor MIDI-Programmen. SVGA-Modi (VESA, auch über das Bank-Fenster) sichert
  der Kernel beim Wechsel über VBE 4F04h samt Bildspeicher und stellt sie
  beim Zurückholen wieder her, einschließlich der eingestellten Bank
  (Test: `START \DEMO\BANKT`, VBETEST; hinten weiterzeichnen: `START \DEMO\SVBG`). Virtueller Zeitgeber: Kanal 0 tickt mit der Rate,
  die das Programm einstellt (Spiele stellen oft 100–1000 Hz ein), der
  Zähler läuft nach echter Zeit; Kanal 2 und Port 61h (PC-Lautsprecher)
  gehen im Vordergrund an die echte Hardware, ebenso der AdLib-Chip
  (388h, FM-Musik; FM-Programme wie PLAY pausieren solange und spielen
  danach weiter). Virtuelle Sound Blaster 16 (220h, DMA 1/5, IRQ 5/7, DSP 4.05,
  16 Bit/Stereo, Mixer; seit 6.3 im Modul `VSB.SYS`, mit AdLib, MPU-401 und AWE):
  digitaler Ton der Box geht über SB16.SYS an die echte Karte (DOOM-
  Effekte, Sprache in Day of the Tentacle; OPL auch an 220h/228h, DSP
  E2h), `BLASTER=A220 I5 D1 H5 T6` steht in der Umgebung. Dazu SB-Pro-
  Stereo (Mixer 0Eh, SB-Pro-Lautstärken 04h/22h/26h/28h/2Eh), MIDI über
  die DSP (38h, UART 34h–37h) und Aufnahme (24h/2Ch/98h/99h, SB16 mit
  Eingabe-Bit) – Aufnahme liefert Stille im richtigen Takt mit IRQ. Die Box wartet beim
  Ton nie auf die Karte (`AUD_NOWAIT`); auf CPUs ohne VME/PVI (386/486)
  kosten die Fallen für CLI/STI/Ports weniger als früher – DOOM 1.9 mit
  Sound Blaster auf einem DX4-100 (86Box) braucht für `-timedemo demo1`
  5091 statt 7123 Takte, unter FreeDOS direkt 4686 (im genaueren
  Interpreter-Modus von 86Box: 4452 gegen 4182, also gut 6 % langsamer). Datum und
  Zeit in der Box deutsch (COUNTRY.SYS von FreeDOS). Wie beim echten
  BIOS: Tastaturpuffer im BIOS-Datenbereich (INT 16h AH=05h, Programme
  lesen ihn direkt), INT 10h AH=1Bh, INT 21h 36h mit echter Clusterzahl,
  FINDFIRST ohne Treffer 12h, Suchzustand im DTA wie DOS (verschachtelte
  Suchen) – Lotus Works 3.0 und Word 5.5 installieren sich damit von
  Diskette (Lotus in QEMU und 86Box, Word in QEMU geprüft). Maus in der Box
  (seit 6.3 im Modul `VMOUSE.SYS`, ohne das Modul meldet INT 33h „kein
  Treiber“) über INT 33h wie MOUSE.COM von Microsoft (Reset, Zeiger, Position,
  Tasten, Grenzen, Mickeys, Ereignis-Rückruf 0Ch/14h – auch für DPMI-Programme
  im Protected Mode (DOS-Extender), Version 24h mit Art und IRQ der echten Maus – MSD
  erkennt sie); im Textmodus zeichnet AT-DOS den Zeiger, in Grafikmodi
  fehlt er noch (Position und Tasten stimmen). CMOS (Port 70h/71h) meldet
  der Box ihren eigenen erweiterten Speicher (MEM/MSD von MS-DOS), INT 10h
  1003h (Blinken/helle Hintergründe) wirkt. Ein Textmodus gibt die Karte zurück. Schreibt ein
  BIOS in sein ROM, bekommt die Box eine eigene Kopie der Seite (wie
  Schatten-RAM)
- Konventioneller Speicher der Box: FreeDOS in der HMA, Dateien und
  Laufwerkstabelle in UMBs, BOXSHELL verlegt sich selbst in eine UMB –
  bis 627 KB frei (unter MS-DOS 6.22 mit QEMM ~635 KB)
- `\DOS\MDOS\DOSBOX.INI`: Einstellungen je DOS-Programm, da es keine
  Oberfläche wie das Einstellungs-Notizbuch von OS/2 gibt – `[*]` für alle,
  `[NAME.EXT]` für eines (geht vor); `XMS_MEMORY_LIMIT=KB` (0–63424,
  Vorgabe 2048), `EMS_MEMORY_LIMIT=KB` (0–32768, Vorgabe 2048; 0 oder
  `EMS_FRAME_LOCATION=NONE` beim Anlegen der Box: kein Seitenrahmen,
  64 KB mehr oberer Speicher), je `-1`
  = so viel wie möglich (MS-DOS-MEM zeigt in der Box die XMS-Größe:
  BOXSHELL trägt sie in LoL+45h ein),
  `DOS_VERSION=n.nn` (SETVER.LST geht vor); Hilfen der Nachbildung für
  DPMI-Programme, Vorgabe `ON`, bei einem störrischen Programm `OFF`:
  `POPF_PATCH` (POPF nach CLI wird zur Falle, damit das virtuelle IF
  stimmt), `SB_IRQ_IDLE` (SB-Leitung ohne Handler ruht),
  `IF_WATCHDOG` (IF gilt nach 3 Takten ohne STI wieder als an), `PVI`
  (ab Pentium: CLI/STI ohne Falle – DPMI-Programme über CR4.PVI, sonst
  über CR4.VME wie OS/2 2.11/Windows 95; `OFF` schaltet beides ab),
  `IDLE_SENSITIVITY=n` (0–100 wie OS/2, Vorgabe 75, 100 = aus: wer im
  Takt öfter leer die Tastatur, die Uhr oder den Strahlrücklauf abfragt,
  gibt die Zeitscheibe ab; ohne Angabe nicht bei Spielen mit eigenem
  Takt; wer dabei in den Textbildschirm schreibt – wie der Norton
  Commander, der vor jedem Zeichen auf den Rücklauf wartet –, gilt nicht
  als untätig), `ADLIB_COPY=ON` (Vorgabe aus: der Kernel schreibt die AdLib-
  Register der Box von Anfang an mit, damit ihre Instrumente zurückkommen,
  wenn zwischendurch ein FAPI-Programm wie `PLAY /FM` gespielt hat; ohne
  Angabe nur, wenn beim Zugriff der Box schon eines FM offen hat – das
  Mitschreiben kostet Fallen; Test `\DEMO\OPLTEST`), `HW_TIMER=ON`
  (Vorgabe aus: der echte Zeitgeber läuft vorn mit dem Takt des
  Programms auch unter 100 Hz, jeder Takt-Interrupt kommt von der
  Hardware statt aus dem 100-Hz-Raster – für Spiele mit Ton über den
  PC-Lautsprecher wie Digger). Wird vor jedem
  Programm neu gelesen; was beim Start der Box gilt (`DOS=`, `RMSIZE=`),
  steht in der CONFIG.SYS
- `SETVER [Datei n.nn] [/DELETE] [/QUIET]` wie MS-DOS 5/6: Versionstabelle
  für DOS-Programme in den Boxen (`\DOS\MDOS\SETVER.LST` des
  Startlaufwerks, Text „NAME.EXT n.nn“). Jede neue Box lädt sie, das DOS
  darin meldet dem Programm auf INT 21h 30h die eingetragene Version
  (ohne Eintrag 7.10). Läuft unter AT-DOS und in der Box FreeDOS-Kern und FreeCOM stehen unter der GPL
  und sind aus dem Quelltext gebaut (`freedos/README.txt`,
  `tools/freedos.sh`)
- Ausnahmen in Anwendungen beenden nur den Task, nicht das System
- Koprozessor (287/287XL) je Task: jedes Programm hat seinen eigenen
  287-Zustand, getauscht erst, wenn ein anderes Programm wirklich rechnet
  (#NM nach dem Taskwechsel, wie OS/2); ein 287-Fehler (IRQ 13) beendet
  nur das Programm. Test: `\DEMO\FPUTEST [Start [Schritte]] [/W] [/E]`
  zweimal gleichzeitig

**MODE CON LINES=25|43|50** stellt die Zeilenzahl der eigenen Sitzung um
(wie OS/2: jede Sitzung hat ihre eigene, beim Wechsel stellt der Kernel die
Karte um). 43 und 50 Zeilen nur mit VGA: 350 bzw. 400 Rasterzeilen mit dem
8x8-Zeichensatz aus dem ROM der Karte; `MODE CON` zeigt den Zustand,
`MODE CO80` geht zurück auf 25. EDIT, LESS, MORE und `DIR /P` nutzen alle
Zeilen.

Programme auf der Diskette: **EDIT** ist ein Bildschirm-Editor wie MS-DOS
EDIT (Vollbild, Menüs mit Alt oder F10, Markieren mit Umschalt,
Umschalt+Entf/Strg+Einfg/Umschalt+Einfg, Strg+Y, Suchen/Weitersuchen (F3)/
Ändern, Strg+S; Text bis 512 KB, Tabulatoren werden zu Leerzeichen;
Dialoge mit Tab und Pfeiltasten; Öffnen mit Dateiliste, Verzeichniswechsel
und Mustern wie `*.TXT`; beim Herunterfahren fragt EDIT, ob geänderter Text
gespeichert werden soll).
Alt+Taste liefert wie beim BIOS Zeichen 0 und den Scancode, Alt allein
`KEY_ALT` (3800h). **LESS** zeigt Text an und blättert vor und
zurück nach dem Vorbild von GNU less (Bild/Pfeile, `g G`, `123g`, `/` `?`
`n N` mit Hervorhebung, `-N` Zeilennummern, `-i`, `h` Hilfe; aus Datei oder
Pipe), **GREP** sucht mit regulären Ausdrücken nach dem
Vorbild von GNU grep (`-i -v -n -c -l -h -H -r -s -E`, `. * + ? ^ $ [..] |`,
Platzhalter in Dateinamen, erkennt Binärdateien), **MORE** blättert seitenweise, **FIND** sucht
Text (`/V /C /N /I`, wie MS-DOS), **MEM** zeigt den Arbeitsspeicher als Tabelle wie das MEM von FreeDOS (`/C` ohne Platten-Cache; in der DOS-Box gilt das FreeDOS-MEM aus `\DOS\MDOS`), **TASKA**
zählt und gibt per `DosYield()` ab (schön mit `START TASKA` im Hintergrund),
**TASKB** fordert zehn Segmente zu 64 KB an, prüft sie, gibt die Hälfte frei
und überlässt den Rest dem Kernel, **TASKC** versucht direkt nach B800:0000 zu
schreiben und wird per #GP beendet.

## Programmformat

FAPI-Programme sind `.EXE`-Dateien mit einem MZ-Stub vorn: Unter MS-DOS
gestartet, meldet er nur „Diese FAPI-Anwendung kann nicht im Real Mode
ausgeführt werden.“

**Gebundene Programme laufen auch unter MS-DOS** – wie die Family API von
OS/2 mit `BIND`: Statt der Meldung tragen sie den FAPI-Stub (`stub/`,
knapp 11 KB) mit sich, der unter MS-DOS 3.1 und neuer (ab 80286) das
Programmabbild als 64-KB-Segment zurechtlegt und jeden FAPI-Aufruf in
DOS-, BIOS- und Maustreiber-Aufrufe übersetzt (Dateien, Verzeichnisse,
Umgebung, Programmstart, Tastatur, Bildschirm samt VioGetBuf und
Vollbild, VGA-Grafik 13h/12h/Mode X, Maus über INT 33h, Strg+C als
SIG_BREAK). Dieselbe Datei läuft also unter AT-DOS und unter MS-DOS.
Gebunden sind MORE, FIND, GREP, LESS, SORT, FC, COMP, CUT, HEAD, TAIL,
WC, UNIQ, TR, CAT, STRINGS, MD5SUM, SHA1SUM, HEXDUMP, XXD, EXPAND, COMPRESS, GZIP, GUNZIP, TAR, ZIP, UNZIP, UNARJ, UNZOO, UNLHA, LHA, ARJ, ZOO, 7ZDEC, 7ZA, EDIT, FM, TREE, ATTRIB, XCOPY, MOVE,
DELTREE, CHOICE, REPLACE, LABEL, MSD, SETVER und die Demo GRAFIK
(`BINDAPPS` in build.sh, `mkfx.py --bind`) – sie laufen damit auch in
den DOS-Boxen.
Unter DOS nicht möglich: Sitzungen, Pipes im Kernel, Laufwerke als Ganzes
(CHKDSK, FORMAT, FDISK), SUBST/JOIN, lange Namen. Möglich macht es der
Gate-Zeiger: Programme rufen die FAPI über `call far [2]`, der Zeiger am
Anfang von crt0 zeigt unter AT-DOS auf das Call-Gate 0023:0000, unter DOS
trägt der Stub seine Übersetzung ein.

Dahinter folgt der FX-Kopf mit der Signatur `CH` – nach dem Vorbild von MZ,
den Initialen von Mark Zbikowski (siehe `include/fx.h`), mit
Größe von Code, Daten, BSS und Stack sowie dem Einsprung. Code und Daten
sind getrennte Segmente (je bis 64 KB); läuft ein Programm mehrfach (z. B.
COMMAND in jeder Sitzung), teilen sich alle Instanzen das Codesegment wie
bei OS/2, jede hat nur ihr eigenes Datensegment. `tools/mkfx.py` erzeugt die
Dateien aus dem Watcom-Abbild.

## FAPI (Stand Meilenstein 9)

Dateinamen und Pfade nehmen bis 260 Zeichen an (wie bei OS/2), auch wenn FAT
selbst nur 8.3 kennt.

| AX | Funktion        | Argumente                       |
|----|-----------------|---------------------------------|
| 0  | DosGetVersion   | → AX = 0500h                    |
| 1  | VioWrtStr       | BX = String, CX = Zeile<<8\|Spalte |
| 2  | DosYield        | –                               |
| 3  | DosExit         | BX = Rückgabewert 0–255 (ERRORLEVEL); `main` ohne DosExit liefert 0 |
| 4  | DosSleep        | BX = Millisekunden              |
| 5  | DosMemInfo      | BX = `struct fapi_meminfo` (KB), CX = dessen Größe: ab 6.0 mit 32-Bit-Feldern `total_kb32`/`free_kb32` (CX ≥ 18) und `box_kb` (Speicher der DOS-Boxen, CX ≥ 22) (CX = 0: alte 10 Byte, Werte über 64 MB gekappt) |
| 6  | DosAllocSeg     | BX = Größe, 0 = 64 KB → AX = Selektor |
| 7  | DosFreeSeg      | BX = Selektor                   |
| 8  | KbdCharIn       | → AX = Zeichen \| Scancode<<8   |
| 9  | KbdPeek         | wie 8, wartet nicht, 0 = nichts |
| 10 | VioWrtTTY       | BX = Text, CX = Länge           |
| 11 | VioCls          | –                               |
| 12 | DosExecPgm      | BX = Befehlszeile, CX = 0 warten / 1 eigene Sitzung / 2 nebenher in dieser Sitzung (Teil einer Pipe, behält die Konsole) / 3 `EXEC_DETACH` losgelöst: ohne Konsole – Eingabe von der Konsole wird NUL (Dateiende), KbdCharIn/MouOpen melden `FAPI_ERR_NOCON` (FFF3h), KbdPeek nichts; Kinder in derselben Sitzung erben es |
| 13 | DosGetArgs      | BX = Puffer (81 Bytes)          |
| 14 | DosOpen         | BX = Pfad, CX = 0 lesen / 2 lesen+schreiben (`DosOpenRW`, Datei muss da sein) → Handle |
| 15 | DosRead         | BX = Handle, CX = Puffer, DX = Länge → Bytes |
| 16 | DosClose        | BX = Handle                     |
| 17 | DosFindEntry    | BX = `struct fapi_dirent`, CX = Nummer, DX = Verzeichnis (0 = aktuelles) |
| 18 | KbdSetLayout    | BX = `.KBL`-Daten (Version 1: 278 Bytes CP437, Version 2: 728 Bytes Unicode), 0 = eingebaute → Anzahl Zeichen, die die aktive Codepage nicht hat / FAPI_ERROR |
| 19 | KbdGetLayout    | BX = Puffer (4 Bytes) für die Kennung |
| 20 | DosChDir        | BX = Pfad                       |
| 21 | DosQCurDir      | BX = Puffer (130 Bytes) → z. B. `C:\DOS` |
| 22 | DosSelectDisk   | BX = Laufwerk (0 = A:)          |
| 23 | DosQCurDisk     | → AX = Laufwerk                 |
| 24 | DosCreate       | BX = Pfad → Handle (legt an oder kürzt auf 0) |
| 25 | DosWrite        | BX = Handle, CX = Puffer, DX = Länge → Bytes |
| 26 | DosDelete       | BX = Pfad                       |
| 27 | DosMkDir        | BX = Pfad                       |
| 28 | DosRmDir        | BX = Pfad (muss leer sein)      |
| 29 | DosMove         | BX = alter, CX = neuer Pfad (gleiches Laufwerk) |
| 30 | DosFreeSpace    | BX = Laufwerk, CX = Puffer (4 Bytes) → freie Bytes |
| 31 | DosShutdown     | BX = 0 anhalten, 1 Neustart (kehrt nicht zurück) |
| 32 | DosSetSigHandler | BX = Weiche (0 = abmelden), CX = `SIG_BREAK` \| `SIG_SHUTDOWN` – in C: `DosSetSigHandler(fn, maske)` |
| 33 | DosSigReturn    | Handler fertig (macht die Weiche in `fapi.h`) |
| 34 | DosGetEnv       | BX = Name, CX = Puffer, DX = Länge → Länge des Werts |
| 35 | DosSetEnv       | BX = `NAME=WERT` (leerer Wert löscht) |
| 36 | DosEnvEntry     | BX = Nummer, CX = Puffer, DX = Länge → n-ter Eintrag |
| 37 | DosGetDateTime  | BX = Puffer (4 Bytes): Datum, Uhrzeit im FAT-Format |
| 38 | DosDup2         | BX = Handle (FFFFh = Konsole), CX = Standard-Handle 0–2 |
| 39 | DosDup          | BX = Handle → neues Handle auf dasselbe |
| 40 | DosPipe         | BX = Puffer (2 Worte) → Lese-, Schreib-Handle |
| 41 | VioWrtStrAtt    | wie VioWrtStr, DX = Farbattribut |
| 42 | DosQHandType    | BX = Handle → 0 Datei, 1 Pipe, 2 Konsole, 3 Laufwerk |
| 43 | DosOpenDrive    | BX = Laufwerk, CX = 0 lesen / 1 exklusiv (Sperre) (+ 2 `DRV_OPEN_LBA`: DosSeek-Stelle in Sektoren, für über 4 GB) → Handle; Sektoren mit DosRead/DosWrite |
| 44 | DosSeek         | BX = Handle, CX = Zeiger auf Position (4 Bytes) |
| 47 | DosSetFInfo     | BX = Pfad, CX = struct fapi_finfo: Attribute und/oder Datum/Uhrzeit setzen |
| 48 | DosSetDateTime  | BX = Datum, Uhrzeit (FAT-Format) → Echtzeituhr stellen |
| 49 | DosLabel        | BX = Laufwerk, CX = Ausgabe (Bezeichnung, Seriennummer), DX = neue Bezeichnung oder 0 |
| 46 | DosQChildRC     | → Rückgabewert des zuletzt beendeten Kindes (255 = Ausnahme oder Strg+C) |
| 45 | DosQFSInfo      | BX = Laufwerk, CX = Puffer: Dateisystem-Name, Sektoren, Sektorgröße, Flags |
| 50 | VioSetMode      | BX = 1 Vollbild (alle Zeilen ohne Titel- und Statusleiste), 0 normal → Zeilenzahl; der normale Inhalt wird gesichert und am Ende wiederhergestellt |
| 51 | VioSetCurPos    | BX = Zeile << 8 \| Spalte (Zeile FFh = Cursor aus) |
| 52 | KbdShift        | → Umschalttasten jetzt: 1 Umschalt, 2 Strg, 4 Alt, 8 AltGr |
| 53 | VioSetLines     | BX = Bildschirmzeilen der eigenen Sitzung: 25, 43, 50 (nur VGA), 0 = fragen → Zeilen; löscht den Bildschirm, nicht im Vollbild |
| 54 | VioGetRows      | → Zeilen, die das Programm beschreiben kann (normal Bildschirm − 2, Vollbild alle) |
| 55 | DosShutdownHold | nach SIG_SHUTDOWN: Herunterfahren aufhalten, weil das Programm den Benutzer fragt; Sitzung kommt nach vorn, der Kernel wartet bis zum Programmende (sonst 5 s) |
| 56 | MouOpen         | → Anzahl Maustasten (0 = keine Maus); Ereignisse für die eigene Sitzung an, Zeiger zunächst aus |
| 57 | MouReadEvent    | BX = Puffer (Tasten, Zeile, Spalte; mit MOU_XY zusätzlich x, y), CX = MOU_WAIT (1) warten \| MOU_XY (2) 8 statt 4 Byte → 0 Ereignis, 1 keins; Zeile/Spalte als Textzelle der Sitzung, Zeile FFh = Titel-/Statusleiste; x/y in Grafiksitzungen in Bildpunkten (12h 640×480, 13h 320×200), sonst Mickeys (8 je Spalte, 16 je Zeile) |
| 58 | MouShowPtr      | BX = 1 Mauszeiger zeigen (invertierte Zelle), 0 verbergen |
| 59 | DosClipPut      | BX = Selektor (0 = eigenes Datensegment), CX = Offset, DX = Länge → gespeichert (bis 16 KB); Zwischenablage für alle Sitzungen |
| 60 | DosClipGet      | BX = Selektor (0 = eigenes), CX = Offset, DX = Platz → Länge des Inhalts |
| 61 | VioGetBuf       | → Selektor auf den Bildschirmpuffer der eigenen Sitzung (logischer Puffer wie bei OS/2 1.x; Zelle = Zeichen \| Attribut << 8, Zeile r ab r·160) |
| 62 | VioShowBuf      | BX = erste, CX = letzte Zeile: fertige Zeilen zeigen (Schreiben in den Puffer allein zeigt nichts) |
| 63 | VioSetGfx       | BX = 13h: VGA-Grafik 320×200×256, 12h: 640×480×16 (vier Ebenen), 58h (`VIO_MODEX`): Mode X 320×240×256 unverkettet mit zwei Seiten, 0 = zurück auf Text (nur VGA) |
| 64 | VioGetPhysBuf   | → Selektor auf das Bild (64 KB, Punkt x,y bei y·320+x); beim Sitzungswechsel sichert der Kernel Bild und Palette und biegt den Selektor in den Puffer um – das Programm zeichnet weiter |
| 65 | VioSetPal       | BX = erster Eintrag, CX = Anzahl, DX = Puffer (je R, G, B, 0–63) |
| 67 | DosDiskInfo     | BX = Platte (0 = erste Festplatte), CX = Puffer: Sektoren, Sektorgröße, Treiber, Einheit, Zahl der Laufwerke darauf |
| 70 | DosFmtTrack     | BX = Handle aus DosOpenDrive (Diskette, mit Sperre), CX = struct fapi_fmttrack (Zylinder, Kopf, Format als Sektorzahl 720/1440/2400/2880/5760) → 0, FAPI_ERR_WP, FAPI_ERROR |
| 74 | DosComCtl       | BX = COM 0–3 oder 10h+LPT 0–2, CX = struct fapi_comcfg (Teiler, LCR, Wiederholen, Status), DX = 1 einstellen / 0 abfragen → 0, FAPI_ERROR, FAPI_ERR_INUSE (Port gehört einem Treiber) |
| 73 | DosSysInfo      | BX = struct fapi_sysinfo: Prozessor (286/386+), Koprozessor, VGA, BIOS-Datum/-Modell/-Name, Video-BIOS-Name, COM/LPT-Ports, Speicher, Kernelgröße, freigegebene IRQs, Treiber, Diskettentypen |
| 71 | DosSubst        | BX = Laufwerk, CX = Pfad oder 0 (aufheben) → 0, FAPI_ERROR, FAPI_ERR_INUSE |
| 72 | DosQSubst       | BX = Laufwerk, CX = Puffer (130 Byte) → "C:\DOS" oder FAPI_ERROR |
| 75 | DosJoin         | BX = Laufwerk, CX = leeres Verzeichnis (wird angelegt, wenn es fehlt) oder 0 (aufheben) → 0, FAPI_ERROR, FAPI_ERR_INUSE, FAPI_ERR_DENIED (nicht leer) |
| 76 | DosQJoin        | BX = Laufwerk, CX = Puffer → "C:\CDROM" oder FAPI_ERROR |
| 77 | DosCacheInfo    | BX = struct fapi_cacheinfo, CX = Größe (0 = 5.0) → Größe des Platten-Caches (KB), Frist für verzögertes Schreiben (s), Treffer/Fehltreffer, ungeschriebene Blöcke; ab CX = 18 Mindest-/Höchstgröße des dynamischen Caches |
| 79 | Aud*            | Ton (FAPI_AUDIO), BX = Unterfunktion: AudOpen (CX = struct fapi_audfmt: Rate, Kanäle, Bits; Bits | `AUD_SMALL` = kurzer Puffer, Abschnitte ~1/32 s statt 4 KB, `AUD_NOWAIT` = AudWrite wartet nie, was nicht passt, fällt weg – für DOS-Boxen), AudWrite (CX = Puffer, DX = Länge, wartet auf Platz), AudDrain, AudClose, AudMixer (CX = Regler, DX = 0–100 oder FFFFh), AudInfo (Name der Karte), AudCaps, FmOpen/FmWrite (OPL-Register direkt)/FmClose, MidiOpen/MidiWrite (rohe MIDI-Bytes)/MidiClose, AweCmd (AUD_AWE, CX = struct fapi_awecmd: SoundFont-Bank laden – Info, Zonen, Presets, Samples). Zu hören ist das Programm der Vordergrund-Sitzung, sonst das zuletzt geöffnete; die anderen laufen stumm im Echtzeittempo weiter |
| 80 | DosQTicks       | → Systemtakt (100 Hz), untere 16 Bit – Zeitmessung für Spieler und Spiele |
| 81 | DosBusInfo      | BX = 0 PCI-Gerät / 1 Belegungsliste / 2 ISA-PnP-Gerät, CX = Nummer, DX = struct fapi_pcidev, fapi_res bzw. fapi_pnpdev → 0, FAPI_ERROR am Ende (MSD) |
| 83 | VioVesa         | BX = 0 (`VESA_MODE`): CX = n → n-ter brauchbare VESA-Modus mit linearem Bildspeicher (8/15/16/24/32 Bit, bis 3,4 MB Bild) oder bei VBE 1.2 mit 64-KB-Fenster (ohne 24 Bit; `model` Bit 7 = `VESA_BANKED`) in `struct fapi_vesamode` bei DX (Nummer, Breite, Höhe, Byte je Zeile, Bit, Farbmodell, Bildspeicher der Karte, VBE-Version), Fehler hinter dem letzten; BX = 1 (`VESA_SET`): CX = Modusnummer einstellen (DX wie oben). Danach VioGetPhysBuf = 64-KB-Fenster aufs Bild, VioSetPlane(n) = n-ter 64-KB-Abschnitt, VioSetPal für 8 Bit, VioSetGfx(0) = zurück auf Text. Sitzungswechsel sichert das Bild, im Hintergrund zeichnet das Programm in den Puffer |
| 82 | DosNewSize      | BX = Handle (schreibend), CX = Zeiger auf neue Größe (4 Byte) → 0: kürzen oder mit Nullen verlängern (wie OS/2) |
| 85 | –               | in 6.0 frei (in 5.1/5.2 DosRBox, Kompatibilitätsbox) |
| 86 | DosGetCp / DosSetCp / DosCpList | BX = 0: aktive Codepage → AX; BX = FFFFh, CX = Platz: vorbereitete Codepage aus `CODEPAGE=` (0 = Ende); sonst BX = Codepage einschalten (437 immer, sonst nur vorbereitete; lädt den Zeichensatz, Dateinamen folgen) → 0 / FAPI_ERROR |
| 87 | DosEject        | BX = Laufwerk (0 = A:): Medium auswerfen (CD, ZIP, MO; Treiberbefehl `DRV_EJECT` = START STOP UNIT mit LoEj) → FAPI_ERR_INUSE (Dateien offen), FAPI_ERR_DENIED (kein Wechseldatenträger oder Treiber kann es nicht) |
| 88 | DosLoadDriver / DosUnloadDriver / DosDrvInfo | BX = 0 (`DRVL_LOAD`): CX = „voller Pfad [Argumente]“, DX = 1 (`DRVL_IFS`) für ein IFS-Modul → Treiberplatz; neue Blockgeräte bekommen die nächsten freien Buchstaben, CDs nach einem IFS-Modul; FAPI_ERR_EXIST (schon geladen), FAPI_ERR_FULL (kein Platz), FAPI_ERR_NOTREADY (kein Gerät gefunden), FAPI_ERROR (Datei fehlt/kein Treiber). BX = 1 (`DRVL_INFO`): CX = Platz → `struct fapi_drvinfo` bei DX (Name, Art, Geräte, Laufwerke als Bitmaske), Fehler hinter dem letzten Platz. BX = 2 (`DRVL_UNLOAD`): CX = Name (mit oder ohne Endung) → 0, FAPI_ERR_INUSE (Dateien offen, aktuelles Laufwerk eines Programms, SUBST/JOIN, Ton offen), FAPI_ERR_DENIED (Startlaufwerk oder Treiber kennt `DRV_DEINIT` nicht), FAPI_ERROR (nicht geladen) |
| 89 | DosLoop         | BX = 0 (`LOOP_ATTACH`): CX = Pfad der Datei, DX = Laufwerk (2 = C: … 25, 0 = nächster freier), dazu `LOOP_RO` (100h) = nur lesend → erstes Laufwerk; FAPI_ERR_INUSE (zum Schreiben offen), FAPI_ERR_EXIST (Buchstabe belegt), FAPI_ERR_FULL (kein Platz), FAPI_ERR_FRAG (FFF8h, über 8.188 Stücke), FAPI_ERR_DENIED (nicht auf FAT/CD, dynamische VHD), FAPI_ERR_NOTREADY (kein Dateisystem im Abbild). BX = 1 (`LOOP_DETACH`): CX = Laufwerk → 0, FAPI_ERR_INUSE. BX = 2 (`LOOP_INFO`): CX = Platz 0–7 → `struct fapi_loopinfo` bei DX (Laufwerke, Blöcke, Blockgröße, Stücke, Art – 1 ISO, 2 Abbild, 3 Platte, 4 VHD, 5 `LOOPK_DBL` DoubleSpace/DriveSpace, Bit 7 `LOOPK_RO` = nur lesend –, Laufwerk und Pfad der Datei) |
| 90 | DosKlog         | Kernelmeldungen (`DMESG`): BX = Puffer, CX = Länge, DX = Stelle ab der ältesten → kopierte Bytes; CX = 0 → Länge des Protokolls (bis 8 KB, `KLOG_SIZE`); CX = 0, DX = FFFFh → löschen |
| 91 | DosNet          | Netzwerk (6.2, `TCPIP.SYS`): BX = `NET_*`, CX = Parameterblock, DX = dessen Größe; ohne Stapel FAPI_ERR_NONET (FFF7h). `NET_IFINFO` (1): `struct fapi_netif` mit vorgegebener Karte → Name, MAC, Verbindung, Adressen (immer 16 Byte, IPv4 als ::ffff:a.b.c.d, mit Präfixlänge und Herkunft), Gateways, DNS, DHCP-Server und Lease, Zähler, Rechnername; FAPI_ERROR hinter der letzten Karte. `NET_PING` (2): `struct fapi_ping` (Ziel, Größe, Frist in ms, Folgenummer, TTL) → Laufzeit, TTL, Absender, ICMP-Typ/-Code; FAPI_ERR_TIMEOUT (FFF6h). `NET_DHCP` (3): `struct fapi_netcmd` (Karte, 0 erneuern, 1 freigeben) `NET_RESOLVE` (4): `struct fapi_resolve` (Name → bis 4 Adressen, TTL; Zahlenadressen direkt; Familie 0 = A, mit globaler IPv6-Adresse erst AAAA, `AF_INET`/`AF_INET6` nur A bzw. AAAA) → 0, FAPI_ERROR (unbekannt), FAPI_ERR_TIMEOUT. `NET_FWCTL` (5): `struct fapi_fwctl` – `FW_GET` (Zustand, Zähler, Regel idx), `FW_SET`, `FW_ADD` (`struct fapi_fwrule`: Programm, Protokoll, Ports, Netz/Präfix oder FWR_LOCAL → Nummer), `FW_DEL`, `FW_CLEAR`, `FW_LOG`. `NET_SOCKLIST` (6): `struct fapi_sockinfo` für Platz idx (Art, TCP-Zustand, Adressen, Ports, Programm, Pufferfüllung) → 0, FAPI_ERROR dahinter. `NET_SELECT` (1Eh): `struct fapi_select` (Anzahl bis 8, Frist in ms – 0 nur nachsehen, FFFFh ohne Frist –, je Eintrag Handle und gewünscht SEL_READ/SEL_WRITE) → Zahl der bereiten Einträge (0 = Frist um), in `ready` SEL_READ/SEL_WRITE/SEL_ERR; lauschende Sockets sind lesebereit, wenn eine Verbindung zum Annehmen wartet. Sockets wie BSD, Handles wie Dateien: `NET_SOCKET` (10h, `struct fapi_socket`: Familie AF_INET/AF_INET6, Art SOCK_DGRAM oder SOCK_STREAM) → Handle; `NET_BIND`/`CONNECT`/`LISTEN`/`ACCEPT` (ACCEPT → neues Handle)/`SEND`/`RECV`/`SENDTO`/`RECVFROM`/`SHUTDOWN`/`GETNAME`/`GETPEER`/`SETOPT`/`GETOPT` (11h–1Dh) mit `struct fapi_sockio` (Handle, Puffer, Länge, Merker, Frist in ms, `struct fapi_sockaddr` mit Länge/Familie wie sockaddr_in6, Port in Netzbytefolge); FAPI_ERR_AGAIN (FFF5h) nicht blockierend, FAPI_ERR_RESET (FFF4h). |
| 92 | DosRandom       | Entropie (6.3): BX = Puffer, CX = Länge (bis 34) → Pool des Kernels (8 Doppelworte, gemischt aus dem PIT-Zähler und Takt bei jedem Hardware-Interrupt außer dem Takt – Tastatur, Platten, Netz, Maus –, dahinter die Zahl der Ereignisse) → Bytes; roh, vor Gebrauch hashen (`tls_random` in apps/tls.c) |
| 93 | DosUfs          | Dateisystem in Ring 3 (6.3, NET USE): BX = UFS_REG (CX = Laufwerk + `\\Rechner\Freigabe`), UFS_GET (CX = Auftrag, DX = Datenpuffer 16 KB, wartet), UFS_PUT (Antwort), UFS_DEL (CX = Laufwerk, Dienst beenden), UFS_INFO (CX = Laufwerk, DX = Ziel) |
| 84 | DosProc         | BX = 0 (`PROC_INFO`): CX = Tasknummer → `struct fapi_proc` bei DX (Zustand, Art FAPI/DOS-Box/ruhende Box, Sitzung, Aufrufer, Merker eigener/geschützt, Größe, Name, Anfang der Befehlszeile), Fehler hinter dem letzten Platz; BX = 1 (`PROC_KILL`): Task CX beenden (Rückgabewert 255 wie Strg+C) → FAPI_ERR_DENIED für den eigenen Task, seine Aufrufer und die Shell der ersten Sitzung |
| 78 | VioSetAnsi      | BX = 1 an, 0 aus, 2 abfragen → bisheriger Zustand; ANSI-Folgen (ESC[…H/f/A–D/s/u/J/K/m) in der TTY-Ausgabe der eigenen Sitzung, wie OS/2 |
| 68 | DosOpenDisk     | BX = Platte, CX = DRV_OPEN_* → Handle auf die ganze Platte ab Sektor 0 (für FDISK); mit Sperre nur, wenn darauf nichts offen ist |
| 66 | VioSetPlane     | VESA (VioVesa): BX = 64-KB-Abschnitt des Bildes, den das Fenster aus VioGetPhysBuf zeigt. Modus 12h: BX = Ebene 0–3 (Farbbit) zum Schreiben und Lesen; Mode X: Ebene x & 3, Punkt bei Seite + y·80 + x/4 (Seite 0 bei 0, Seite 1 bei 19200); im Hintergrund zeigt der Selektor auf die Ebene im Puffer |
| 69 | VioSetPage      | Mode X: BX = sichtbare Seite 0/1; im Vordergrund wartet der Aufruf auf den Strahlrücklauf, danach kann das Programm in die andere Seite zeichnen |

Handles 0, 1, 2 sind Standardeingabe, -ausgabe und -fehler (Vorgabe:
Konsole), eigene Dateien beginnen bei 3. Gestartete Programme erben die
Standard-Handles; `DosCreate` mit CX = 1 hängt an (`>>`). `DosExecPgm` mit
CX = 2 startet in derselben Sitzung im Hintergrund (für Pipes). Pipes
puffern im Kernel 2 KB; der Schreiber wartet bei vollem, der Leser bei
leerem Puffer, ein Leser ohne Schreiber bekommt das Dateiende.

Signale wie bei OS/2: Ein Programm kann Strg+C (`SIG_BREAK`) und das
Herunterfahren (`SIG_SHUTDOWN`) abfangen. Der Kernel stellt sie beim
nächsten Rücksprung aus der FAPI zu; wartende Aufrufe wie `KbdCharIn`
kehren dafür vorzeitig zurück. `SHUTDOWN` gibt solchen Programmen bis zu
5 s zum Aufräumen und beendet dann alle Programme regulär. TASKA zeigt es.

Jedes Programm hat eine eigene Umgebung (1 KB) und erbt sie von dem
Programm, das es startet. Die Systemumgebung enthält `COMSPEC`, `PATH`
(`C:\DOS;C:\` mit dem Startlaufwerk) und alles, was die `CONFIG.SYS` mit `SET` festlegt.

Fehler sind Werte ab FFF0h: FFFFh nicht gefunden/unzulässig, FFFEh
schreibgeschützt, FFFDh voll, FFFCh existiert schon, FFFBh Zugriff
verweigert (Attribut, Verzeichnis nicht leer), FFFAh in Benutzung (offene
Datei, aktuelles Verzeichnis eines Programms).

## Gerätetreiber

Dateisystem-Module (`IFS=`) sind gleich aufgebaut, melden bei der
Initialisierung ihre Medienklassen und werden danach mit `struct ifs_req`
aufgerufen (`include/ifsmod.h`); Blöcke lesen sie über DevHlp `DH_BLKREAD`.

Module der DOS-Box (6.3, Entwurf `docs/VDD.md`, wie die VDDs von OS/2
2.0): ein Treiber (`.SYS` wie bei OS/2: `VCOM.SYS`, `VSB.SYS`) meldet sich mit `DH_VDDREG` (`include/vdd.h`)
für Ports und Interrupts der Box an, die der Kernel nicht selbst
nachbildet, und bekommt Ereignisse (Box angelegt, beendet, geparkt);
Dienste über `DH_VDDSVC` (Speicher der Box lesen, FAPI im Namen der
Box). PIC, Zeitgeber, CMOS, Tastatur und BIOS-Grundlage bleiben im
Kernel. Erstes Modul: `VCOM.SYS` – COM1–4 und LPT1–3 der Box (Register,
INT 14h/17h), Ausgabe über COMn/LPTn; `DEVICE=VCOM.SYS`, entladbar.
Zweites: `VSB.SYS` – der Ton der Box (Sound Blaster bis SB16 an 220h mit
DMA 1/5 und IRQ 5/7, Mixer, AdLib/OPL, MPU-401, EMU8000 der AWE),
Ausgabe über SB16.SYS; `DEVICE=VSB.SYS`, ohne das Modul hat die Box keine
Sound Blaster. Dafür meldet ein Modul virtuelle IRQ-Leitungen, einen
Takt-Rückruf, Ports in der I/O-Bitmap und Hardware, die es der Box vorn
gibt (`VS_IRQ`, `VS_TICK`, `VS_IOPERM`, `VS_FGRES`, `VS_AUD`).
Drittes: `VEMS.SYS` – EMS 4.0 der Box (INT 67h, Verwaltung im Modul,
Seiten über `VS_PGALLOC`/`VS_MAP`/`VS_COPY` wie VDHAllocPages/
VDHMapPages bei OS/2); den Seitenrahmen wählt weiter der Kernel.
Viertes: `VMOUSE.SYS` – der Maustreiber der Box (INT 33h, Position,
Grenzen und Zähler im Modul, Maus der Sitzung über `VS_MOUSE`); den
Ereignis-Rückruf stellt der Kernel zu (`VS_CBARM` wie VDHPushFarCall bei
OS/2: das Modul setzt Segment:Offset, der Kernel fragt mit `VDD_CBQ` nach,
ob etwas ansteht – im V86-Modus, aus dem Takt und für DPMI-Programme im
PM). Module gleichzeitig (`NVDD`): 8.
Grafikmodule `VEGA.SYS`/`VCGA.SYS` (nur auf EGA- bzw. CGA-Karten): Ports
der Box mit Rohgrafik bleiben gesperrt und gehen ans Modul, das die
Register mitschreibt (`VDDF_VEGA`/`VDDF_VCGA`, Ereignisse VSAVE/VREST,
`VS_VREGS`). Treiberplätze (`NDRV`) seit 6.3: 12, CONFIG.SYS bis 8 KB.

Befehle: `DRV_INIT`, `DRV_READ`, `DRV_WRITE` (je ein Block über den
DMA-Puffer) und `DRV_FLUSH` (Schreib-Cache der Platten leeren: IDE
`FLUSH CACHE`, SCSI `SYNCHRONIZE CACHE`).

Treiber sind FX-Dateien mit gesetztem Treiber-Flag und laufen in Ring 0 mit
eigenem Code- und Datensegment. Der Kernel ruft ihre Strategieroutine mit
einer Anforderung (`struct drv_req`) auf, Treiber erreichen den Kernel über
DevHlp – angelehnt an die Treiber von OS/2 1.x. Schnittstelle:
`include/drv.h`. Die Hardware-Erkennung liest immer erst: Der Adaptec-Treiber
erkennt die Karte am Bitmuster der Register, bevor er etwas schreibt, und
übergeht so etwa den MPU-401 einer Sound Blaster auf Port 330h.

DevHlp-Funktionen: `DH_PUTS` (Text ins Startprotokoll), `DH_TICKS`
(Systemtakt), `DH_DELAY` (warten), `DH_BLKREAD` (Block eines anderen
Geräts lesen, für IFS-Module), `DH_BIOSDISK` (Plattentabelle des BIOS hinter
INT 41h/46h, vom Kernel beim Start im Real Mode gesichert), `DH_SETIRQ`/
`DH_UNSETIRQ` (IRQ belegen: der Kernel ruft die Routine des Treibers mit
dessen DS und schickt danach das EOI), `DH_PROCBLOCK`/`DH_PROCRUN` (Task
schläft im Treiber bis zum Ereignis oder zur Frist, andere Tasks laufen
weiter; die Interruptroutine weckt ihn), `DH_DMABUF` (Puffer für ISA-DMA),
`DH_KSEG` (gewöhnliches Kernelsegment bis 64 KB, z. B. für Tabellen),
`DH_PHYSMAP` (Geräteregister im Speicher – PCI-MMIO, auch über 2 GB – als
Selektor einblenden, ungecacht; z. B. AHCI), `DH_KLOG` (Text nur ins DMESG, auch aus der Interruptroutine).

Netzwerk (6.2, `include/net.h`): eine Karte meldet sich mit `DH_NETREG`
(MAC, MTU, Merkmale, ferne Einsprünge für Senden und Steuerung), der
Stapel mit `DH_NETPROTO`. Der Kernel kennt keine Protokolle und
vermittelt nur: im IRQ holt die Karte mit `DH_NETRX` einen Platz im
Empfangsring des Stapels, kopiert den Rahmen einmal dorthin und meldet
ihn mit `DH_NETRXDONE`; die Protokollarbeit macht ein Nachlauf, den der
Scheduler ruft (antwortet auch, wenn kein Programm das Netz benutzt).
Gesendet wird über `DH_NETSEND` direkt in den Sendepuffer der Karte.

Treiber mit Interrupt: Während ein Task in einem Treiber auf das Gerät
wartet, gibt er seit 6.0 die Kernelsperre ab (Phase 6): andere Tasks
arbeiten auf anderen Laufwerken weiter – XCOPY auf die SCSI-Platte, eine
CD-Kopie und eine DOS-Box auf der IDE-Platte laufen wirklich
gleichzeitig, CMD bleibt daneben bedienbar. Geschützt wird je Laufwerk
(FAT12/16 teilen sich eine Sperre, FAT32 und CDs haben je eine) und je
Treiber, bei `IDE.SYS` je Kanal (primär und sekundär arbeiten
gleichzeitig); jeder Task hat eigenen DMA-Puffer, FAT-Cache und Schreibpuffer,
und was eine Operation über einen Plattenzugriff hinweg hält, tauscht
der Kernel bei Bedarf aus. Strg+C, der Taskmanager und SHUTDOWN warten,
bis eine laufende Plattenoperation fertig ist. `IDE.SYS` benutzt IRQ 14/15
(Abschalten mit `DEVICE=IDE.SYS /NOIRQ`); bleibt der Interrupt aus, obwohl
das Gerät fertig ist, fragt der Kanal nach drei Fehltreffern wieder ab.
Busmaster-DMA (6.0): sitzt der IDE-Controller am PCI (PIIX3/4 wie im
ASUS P3B-F, QEMU) im Kompatibilitätsmodus und hat das BIOS der Platte
einen DMA-Modus eingestellt, überträgt `IDE.SYS` per READ/WRITE DMA
direkt in den Puffer (PRD-Tabelle, Startmeldung „Busmaster-DMA“);
`/NODMA` schaltet es ab, `/DMA` stellt selbst einen Modus ein, wenn das
BIOS keinen gesetzt hat (SeaBIOS/QEMU): am Intel PIIX4 UDMA bis Modus 2
(Platte und Chipsatz), sonst Multiword-DMA 2; die Startmeldung nennt den
Modus. Nach Fehlern geht die Platte auf PIO zurück. CD-ROMs bleiben bei
PIO. Platten über 128 GB (LBA48) werden ganz genutzt – bis 2 TB, weil
der Kernel Sektoren mit 32 Bit zählt; Startmeldung „… GB (LBA48)“.
MSD zeigt die APM-Version des BIOS.
`EJECT X:` wirft CDs (IDE/ATAPI, SCSI) und SCSI-Wechselplatten (ZIP, MO)
aus, solange dort keine Datei offen ist.
`DEVLOAD Treiber [Argumente]` lädt einen Gerätetreiber (`.SYS`, wie
`DEVICE=`) oder ein Dateisystem (`.IFS`, wie `IFS=`) im laufenden Betrieb
nach – wie `modprobe` unter Linux. Ein Name ohne Pfad wird im aktuellen
Verzeichnis, dann in `\DOS\SYSTEM` und `\DOS` des Startlaufwerks gesucht.
Neue Platten und Partitionen bekommen die nächsten freien Laufwerks-
buchstaben, CD-Laufwerke ebenso, sobald ein CD-Dateisystem geladen ist.
`DEVLOAD /L` zeigt die geladenen Treiber mit ihren Laufwerken,
`DEVLOAD /U Name` entlädt einen (wie `rmmod`) – nur wenn auf seinen
Laufwerken nichts offen ist, kein Programm dort sein aktuelles Laufwerk
hat, kein SUBST/JOIN darauf zeigt und (beim Tontreiber) kein Programm
spielt. Entladen können NCR8XX, AIC7XXX, AHCI, SB16, MOUSE und die IFS-Module;
andere Treiber lehnen ab. Beim nächsten Laden bekommen die Laufwerke
wieder die freien Buchstaben. In `LINUX.MAC` heißen die drei `modprobe`,
`lsmod` und `rmmod`.
`LOOP [X:] Datei [/RO]` hängt eine Datei als Laufwerk ein (6.1) – ein CD-Abbild (`.ISO`, braucht `ISO9660.IFS`, unter AT-DOS ohne
MSCDEX), ein Disketten- oder Partitionsabbild (`.IMG`/`.IMA`), ein
Plattenabbild mit Partitionstabelle (je Partition ein Buchstabe) oder
eine feste VHD (z. B. aus 86Box). Die Datei darf auf FAT12/16/32 oder
auf einer CD liegen und zerstückelt sein (bis 8.188 Stücke); gelesen
und geschrieben wird über den Platten-Cache des Trägers. Disketten-,
Platten- und VHD-Abbilder sind beschreibbar (Stufe 2), außer mit `/RO`,
bei schreibgeschützter Datei oder auf einer CD; ISOs immer nur lesend.
Bei DoubleSpace/DriveSpace-Containern zählt das Attribut R nicht (MS-DOS
und Windows legen sie schreibgeschützt an) – nur `/RO`.
Solange die Datei eingehängt ist, lässt sie sich nicht öffnen zum
Schreiben, löschen oder umbenennen. `LOOP` zeigt die eingehängten
Dateien und ob sie beschreibbar sind, `LOOP X: /D` hängt aus (setzt
vorher das Sauber-Bit). In CONFIG.SYS hängt `LOOP=[X:,]Datei[,/RO]`
(bis zu acht Zeilen) beim Start ein; höchstens acht Abbilder zugleich. Auch komprimierte Laufwerke von
MS-DOS 6.0–6.22 (`DBLSPACE.nnn` von DoubleSpace, `DRVSPACE.nnn` von
DriveSpace) lassen sich so einhängen, lesen und schreiben (`LOOP` zeigt
„gepackt“): neue Cluster werden im Format des Containers gepackt (DS bzw.
JM), gelöschte Dateien geben ihren Platz im Container frei, ein voller
Container meldet „Datenträger voll“ (schon bevor ein neuer Cluster nicht
mehr ungepackt hineinpasst – das Dateisystem innen bleibt stimmig). Der angezeigte freie Platz ist der
des inneren Laufwerks (ohne Schätzung der Packrate). Ebenso DriveSpace 3
(Windows 95 Plus!/98, große Laufwerke bis 2 GB mit 32-KB-Clustern):
gelesen werden alle Arten (JM, SQ, ungepackt, zerstückelte Cluster),
geschrieben wird JM; jeder solche Container braucht 64 KB zusätzlich
(ein unter Windows 98 angelegter Container mit 2 GB innen in 86Box
gelesen, beschrieben und von ScanDisk unter Windows 98 fehlerfrei
geprüft, auch mit UltraPack/HiPack umgepackt).
`PACKDISK /CREATE Datei MB [/DS3|/DBL] [/RATIO:n] [/V:Name]` legt einen
leeren Container an (DriveSpace-6.22-Format bis 512 MB, mit `/DS3`
DriveSpace-3-Format bis 2047 MB, mit `/DBL` DoubleSpace-Format von
MS-DOS 6.0/6.20 – Kennung „MSDSP6.0“, Version 0, gepackt wird im
DS-Verfahren; das innere Laufwerk wird mit der
geschätzten Packrate, Vorgabe 2:1, größer angelegt; Superblock Feld für
Feld wie bei Containern von MS-DOS 6.22 bzw. Windows 98: Kennung
„MSDBL6.0“, FAT 16/32 Sektoren hinter dem Bootblock, Datenbeginn auf
Clustergrenze, FAT immer 256 Sektoren, innen zwei FATs gemeldet, dcluster daraus, Höchstgröße 512 bzw. 2048 MB im Superblock mit BITFAT/MDFAT dafür, Bootblock an derselben Stelle wie bei MS-DOS bzw. Windows 98, Datenträgerbyte hinter dem Bootblock und im MDR-Sektor, mit `/DS3` innen immer 2 GB und BITFAT-Bits hinter dem Datenbereich bis zum Wortende belegt wie Windows 98; `PACKDISK /CMP Vorlage Datei` vergleicht die Verwaltungssektoren zweier Container), `PACKDISK /CHECK
Datei [/F]` prüft einen nicht eingehängten Container (MDFAT gegen BITFAT
und FAT, Überschneidungen, verlorene Cluster), zeigt Packrate und
geschätzten freien Platz und behebt mit `/F` Fehler in BITFAT/MDFAT; `/V` zeigt die ersten Abweichungen mit Cluster und MDFAT-Eintrag
und zählt die Packart (Kopf JM/DS) aller gepackten Cluster, dazu die
ersten 96 Byte des Superblocks, Bootblock und FAT-Anfang. `PACKDISK /COMPRESS X: [/DS3|/DBL] [/RATIO:n] [/RESERVE:MB] [/Y]` zieht ein
Laufwerk (nicht das Startlaufwerk) Datei für Datei in `X:\DRVSPACE.000`
(mit `/DBL` `X:\DBLSPACE.000`) um: der Container beginnt mit dem freien Platz und wächst, sobald die
gelöschten Originale Platz gemacht haben; eine Datei liegt immer ganz auf
einer Seite, ein abgebrochener Lauf setzt mit demselben Aufruf fort;
Systemdateien im Stamm (Startdateien) bleiben auf dem Laufwerk.
Schalter und
Dateiname dürfen in beliebiger Reihenfolge stehen (`PACKDISK C:\X.CVF
/V /CHECK`). Geschrieben
wird in der Packart, die der Container schon enthält (MS-DOS 6.22 legt
auch DriveSpace-Container mit der Kennung „MSDBL6.0“ an). In 86Box
geprüft: unter AT-DOS in einen Container von MS-DOS 6.22 geschrieben,
ScanDisk von MS-DOS meldet danach keine Fehler.
Höchstens 4 zugleich; DOS-Boxen
sehen die Laufwerke über den Redirector, auch zum Schreiben – auch eine
schon laufende Box sieht ein später eingehängtes Laufwerk beim nächsten
Programm. Ein beschreibbar eingehängtes Abbild kann sonst niemand öffnen. In
`LINUX.MAC`: `mount` und `umount`. Auf Setup-Diskette 2.
`NCR5380.SYS` (in Arbeit) spricht SCSI-Karten mit NCR 5380/53C400 an,
zunächst den Trantor T130B (Ports 350h/340h/250h/240h, `/P:340` legt fest),
Byte für Byte per PIO.
Busdienste (6.0): Der Kernel durchsucht beim Start den PCI-Bus
(Mechanismus 1, bei frühen Intel-Boards wie dem ASUS PCI/I-486SP3
auch Mechanismus 2; auch hinter PCI-PCI-Brücken; auf ISA-Rechnern wird
CF8h nur gelesen) und stellt Treibern Suche und Konfigurationszugriff
als DevHlp bereit (`DH_PCIFIND`, `DH_PCICLASS`, `DH_PCIREAD`,
`DH_PCIWRITE`, wie OEMHLP$ bei OS/2). Treiber melden ihre Ports,
DMA-Kanäle und IRQs in einer Belegungsliste an (`DH_CLAIM`); ein
Bereich gehört nur einem Treiber. MSD zeigt PCI-Geräte und Belegung.
ISA Plug and Play (6.0): vor der ersten `DEVICE=`-Zeile isoliert der
Kernel alle PnP-Karten (z. B. Sound Blaster 16/AWE32 PnP), gibt ihnen
eine Kartennummer und schaltet jedes logische Gerät mit der ersten
passenden Konfiguration ein (Ports, die frei sind; IRQ und DMA, die kein
PCI-Gerät, COM-Port oder Systemteil hat). Was ein PnP-BIOS schon
eingeschaltet hat, bleibt so. Danach finden die Treiber die Karte wie
eine feste ISA-Karte. Die Startmeldung und MSD (Computer) zeigen jedes
Gerät mit Kennung und Einstellung; `PNP=OFF` in der CONFIG.SYS schaltet
es ab.
`NCR8XX.SYS` (6.0, braucht einen 386) spricht PCI-SCSI-Controller mit
NCR/Symbios 53C810, 815, 820, 825, 860, 875, 895, 895A und 896 an: findet
die Karte über den PCI-Dienst des Kernels, I/O-Basis aus
BAR 0, und lässt den SCRIPTS-Prozessor der Karte jeden Befehl selbst
abarbeiten (Auswahl, IDENTIFY, Befehl, Daten per Busmaster direkt in den
Kernelpuffer, Status, Trennen und Wiederauswahl). Platten und CD-ROMs
an den IDs 0–6 (`/ID:n` legt die eigene ID fest, Vorgabe 7), mit dem PCI-Interrupt
(`/NOIRQ`: Abfragebetrieb). Wechselplatten (ZIP, JAZ, MO, SyQuest)
bekommen immer einen Laufwerksbuchstaben, auch ohne Medium beim Start;
ein Wechsel wird wie bei der Diskette erkannt (Partition 4 der ZIP-
Disketten und Medien ohne Partitionstabelle gehen beide) – ebenso am
Adaptec/BusLogic (`AHA154X.SYS`) und am Trantor T130B (`NCR5380.SYS`).
`MOUSE.SYS` sucht eine PS/2-Maus (IRQ 12), dann eine serielle (Microsoft,
Logitech mit dritter Taste) an COM1/COM2, zuletzt die Busmaus (Microsoft InPort an 23Ch/238h, Logitech an 23Ch; IRQ wird gesucht, `/IRQ:n` legt ihn fest) (`/PS2`, `/COM1`, `/COM2`, `/BUS` legt fest) und meldet ihre Bewegungen
dem Kernel; Programme lesen sie über MouOpen/MouReadEvent, der Zeiger ist
eine invertierte Textzelle (`\DEMO\MAUSTEST.EXE` zeigt die Ereignisse).
In Sitzungen ohne eigenes Mausprogramm arbeitet die Maus wie gpm unter
Linux: linke Taste ziehen markiert Text, rechte oder mittlere Taste fügt
ihn ein, als wäre er getippt. Die Zwischenablage gilt für alle Sitzungen
und für EDIT (DosClipPut/DosClipGet).
`SB16.SYS` spielt Ton über eine Sound Blaster (SB16: 8/16 Bit, mono/stereo
bis 44,1 kHz; SB/SB Pro: 8 Bit mono bis 22 kHz) per Auto-Init-DMA in vier
Abschnitten (Interrupt je Abschnitt); sucht an
220h–280h, IRQ und DMA liest er beim SB16 aus dem Mixer (`/A:220 /I:5 /D:1
/H:5` legt fest), dazu die FM-Synthese (OPL2/AdLib oder OPL3 an 388h) und
MIDI über einen MPU-401 (330h, `/M:300` legt fest) – auch auf Karten ohne
Sound Blaster. `PLAY Datei` spielt WAV, VOC, MIDI (über den MPU-401, mit
`/FM` oder ohne MPU über eigene FM-Instrumente) und IMF (AdLib-Musik aus
Spielen, `/R:700`) – Beispiele in `\DEMO`: `KLANG.WAV`, `ODE.MID`,
`ODE.IMF` (Ode an die Freude) –, `MIXER [Regler Wert]`
stellt die Lautstärke (MASTER, WAVE, FM, CD, LINE, MIC). AWE32/64: den
Wavetable-Chip (EMU8000) erkennt `SB16.SYS` mit; `AWELOAD [Datei.SBK|.SF2]`
lädt eine SoundFont-Bank (1 oder 2, auch große GS-Bänke für Karten mit
2 MB und mehr) (z. B. `SYNTHGM.SBK` von den Creative-
Treiberdisketten – liegt nicht bei; Ablage `\DOS\SYSTEM\SFBANK`, Vorgabe
`SFBANK\SYNTHGM.SBK`, ein Name ohne Pfad wird auch dort gesucht) in
die Karte, danach spielt MIDI (PLAY und der MPU-401 der DOS-Boxen) über
den AWE als General-MIDI-Synthesizer (mit Hall/Chorus, CC 91/93, und
Vibrato über das Modulationsrad); `AWELOAD /U` gibt die Bank frei
(AWELOAD liegt seit 6.1 nur auf der Festplatte, wie die Bänke),
`SB16.SYS /MPU` schickt MIDI stattdessen an einen externen Synthesizer.
Mehrere Programme
dürfen gleichzeitig Ton ausgeben: zu hören ist die Vordergrund-Sitzung
(Spiele), sonst das zuletzt gestartete (Musik im Hintergrund).
`AHA154X.SYS` liest den IRQ aus der Konfiguration der Karte (Befehl 0Bh,
Anzeige beim Start) und schläft, bis die Karte die Eingangs-Mailbox
beschrieben hat; ebenfalls mit Rückfall und `/NOIRQ`. Beide Treiber melden
gescheiterte Versuche mit Status (IDE) bzw. Host-/Zielstatus (SCSI) in
Sitzung 1.

`IDE.SYS` und ältere Controller: Der WD1003 (MFM/RLL) hat die Register der
IDE-Platten, kennt aber kein IDENTIFY und bricht es ab. Dann nimmt der
Treiber die Geometrie aus der BIOS-Tabelle (nur primärer Kanal, Platte
80h/81h), stellt sie mit INITIALIZE DRIVE PARAMETERS (91h) ein und prüft
sie, indem er den ersten und den letzten Sektor der ersten Spur liest. Der
WD1007 (ESDI) liefert IDENTIFY mit der Rohgeometrie; das BIOS stellt oft
etwas weniger ein. Bei Platten ohne LBA und ohne gültige aktuelle Geometrie
(IDENTIFY-Wort 53) gilt deshalb die des BIOS, wenn sie in die Platte passt.

## Speicher

| linear           | Inhalt                          |
|------------------|---------------------------------|
| 08000h – 087FFh  | DMA-Puffer für Laufwerke (2 KB, ein CD-Block) |
| ab 10000h        | Kernel: Code (bis 64 KB), direkt dahinter Daten samt Stacks (bis 64 KB) |
| Kernelende – 9FFFFh | frei, Speicherverwaltung     |
| A0000h – FFFFFh  | Systembereich (Video, ROMs)     |
| ab 100000h       | frei, Speicherverwaltung        |

## Bauen

Benötigt NASM, mtools und Open Watcom 2.0 (`WATCOM` gesetzt, `wcc`/`wlink`
im PATH):

    ./build.sh      # -> build/atdos-fd.img, build/atdos-fd2.img (je 1,44 MB) und build/atdos-hd.img

Verteilt wird AT-DOS wie MS-DOS 6 auf **zwei Setup-Disketten**:
`atdos-fd.img` (Diskette 1, startfähig: System, alle Werkzeuge in `\DOS`,
Treiber – zugleich Notfalldiskette) und `atdos-fd2.img` (Diskette 2: DOS-Box
`\DOS\MDOS`, Zeichensätze, Packprogramme und GNU-Textwerkzeuge, LOOP,
AWELOAD, `\DEMO`). Beide tragen im Stamm `SETUP.ID` (Nummer, belegte KB
beider Disketten). Startet man von Diskette 1, ruft `AUTOEXEC.CMD`
**SETUP** auf (F3 = Eingabeaufforderung):

1. Ziellaufwerk suchen (bei mehreren wählen); fehlt eine Partition, startet
   SETUP FDISK (danach Neustart, SETUP läuft von selbst weiter), ist sie
   nicht formatiert, `FORMAT /Q`.
2. `CONFIG.SYS` schreiben – `DEVICE=`/`IFS=`-Zeilen von Treibern, die beim
   Start der Diskette kein Gerät fanden, stehen dort als `REM` – und
   `AUTOEXEC.CMD` (ohne den SETUP-Aufruf); danach `SYS`.
3. Diskette 1 kopieren, Diskette 2 anfordern und kopieren (Fortschritts-
   balken, Fehler: Wiederholen/Überspringen), Diskette 1 zurück, Neustart.

Ist schon ein AT-DOS da, wird es aktualisiert: `CONFIG.SYS` und
`AUTOEXEC.CMD` bleiben, die neuen Fassungen heißen `CONFIG.NEW` und
`AUTOEXEC.NEW`. `atdos-hd.img` enthält dasselbe wie beide Disketten.

`CONFIG.SYS` auf der Diskette lädt beide Plattentreiber; was keine Hardware
findet, wird wieder entladen.

Im Stammverzeichnis liegen nur `ATDOS.SYS`, `CMD.EXE`, `CONFIG.SYS`,
`AUTOEXEC.CMD` (und auf Diskette 1 `ATSYS.COM` und `SETUP.ID`). Wie bei OS/2 (`\OS2`,
`\OS2\BOOT`, `\OS2\DLL`) getrennt:

- `\DOS` – was man aufruft: Werkzeuge wie `LESS`, `GREP`, `XCOPY`, `CHKDSK`
- `\DOS\SYSTEM` – was das System nachlädt: Treiber (`.SYS`), Dateisysteme
  (`.IFS`), Prüfprogramme für CHKDSK (`UFAT.EXE`), Tastaturbelegungen (`.KBL`)
- `\DEMO` – die Testprogramme `TASKA`–`TASKC`

Treiber in `DEVICE=`/`IFS=` ohne Pfad sucht der Kernel im Stamm, in
`\DOS\SYSTEM` und (ältere Installationen) in `\DOS`; `KEYB` sucht die
`.KBL` genauso, CHKDSK sein `U*.EXE` zuerst fest in `\DOS\SYSTEM`.

Das Image ist eine normale FAT12-Diskette und lässt sich unter MS-DOS, mit
mtools oder unter jedem anderen System lesen. `ATDOS.SYS` ist versteckt
(`DIR /A` zeigt sie) und muss die erste Datei bleiben; beim Austauschen
also nicht einfach überschreiben, sondern das Image neu bauen.

In 86Box als Diskette A: einlegen, beliebige 286-Maschine mit VGA.

## Start von Festplatte

Der Bootsektor der Partition (`boot/hdboot.asm`) sucht `ATDOS.SYS` im
Stammverzeichnis und folgt der FAT-Kette – die Datei darf also zerstückelt
liegen. Gelesen wird per BIOS (CHS), deshalb muss `ATDOS.SYS` unterhalb von
Zylinder 1024 liegen.

Die Plattentreiber stehen in `CONFIG.SYS` auf eben der Platte, die sie erst
lesbar machen. Wie bei OS/2 liest der Kernel deshalb noch im Real Mode über
das BIOS `CONFIG.SYS` und alle `DEVICE=`-Dateien vor (höchstens 30 KB, aus dem
Stammverzeichnis, `\DOS\SYSTEM` oder `\DOS`). Danach erkennt er die Startpartition an Startsektor
und Seriennummer; sie wird immer `C:` (die übrigen Laufwerke rücken in ihrer Reihenfolge nach) und das erste Laufwerk der Programme. `A:` wird dann
erst beim ersten Zugriff eingehängt.

Zwei Wege auf die Platte:

- **SETUP** von den zwei Setup-Disketten (siehe „Bauen“).
- **Fertige Platte:** `build/atdos-hd.img` (im Release als
  `atdos-hd.zip`), 63 MB, Geometrie 130 Zylinder / 16 Köpfe / 63 Sektoren.
  In 86Box als IDE-Platte einhängen und im BIOS-Setup als Typ „User“ mit
  dieser Geometrie eintragen.
- **Vorhandene FAT16-Partition:** Unter MS-DOS von der AT-DOS-Diskette
  `ATSYS D:` aufrufen. Es kopiert alle Dateien der Diskette (auch die
  versteckte `ATDOS.SYS`) ins Stammverzeichnis, dazu `\DOS`, `\DOS\SYSTEM` und `\DEMO`, und ersetzt
  den Code im Bootsektor; der BPB bleibt. Laufwerke mit `IO.SYS`, `MSDOS.SYS` oder
  `IBMBIO.COM` lehnt es ab. Danach muss die Partition aktiv sein (FDISK) und
  die Platte die erste Festplatte des BIOS.

Achtung: `ATSYS` überschreibt eine vorhandene `CONFIG.SYS` im Stammverzeichnis.

## Lizenz

GPL-2.0-or-later (GNU General Public License, Version 2 oder neuer), wie
der FreeDOS-Kernel – siehe [LICENSE](LICENSE). Jede Quelldatei trägt eine
`SPDX-License-Identifier`-Zeile.
