<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Plan: Big Lock abbauen – über die Versionen

Stand: 15.11.2026, Wunsch des Nutzers: die eine Kernelsperre `klock` ist
weg, bevor 7.0 auf 32 Bit umstellt. Nur Plan – noch kein Code. Anlass:
Loop-Laufwerke, siehe [LOOP-DBLSPACE.md](LOOP-DBLSPACE.md).

## Warum es geht

- Ein Prozessor: immer nur ein Task im Kernel (anders als Linux, das
  seine große Sperre wegen SMP erst 2011 ganz los wurde).
- Der Kernel wird nie unterbrochen (Präemption nur aus Ring 3). Das
  bleibt so. Zu schützen ist nur, was über einen **Schlafpunkt** lebt:
  ProcBlock im Treiber, `yield`, `sleep_ticks`.

„Big Lock weg“ heißt nicht „Kernel unterbrechbar“, sondern: an jedem
Schlafpunkt gibt der Task die globale Sperre ab, und alles, was er dort
hält, ist seins (Kontext je Task) oder durch eine feine Sperre
geschützt. Phase 6 hat das für Treiberaufrufe schon gemacht (Laufwerks-,
Treiber-, Gerätesperren, Kontext je Task).

Kein Konzeptionsfehler: 5.0 war kooperativ auf dem 286, globaler Zustand
war dort das Einfachste und Sparsamste; zur Last wurde er erst mit
Präemption, Boxen und parallelen Plattenzugriffen. So fingen auch Unix,
OS/2 1.x und Linux 2.0 an.

## Zwischenschritt: Verschachtelungstiefe 2

Ein Task darf genau einen geschachtelten Dateisystemaufruf machen –
Loop-Gerät auf seinen Träger. Kontext von fat.c, DMABUF und FAT-Cache je
Task zweimal (zweite Seite nur, wenn ein Loop eingehängt ist). Auf
Ebene 2 hält der Task zusätzlich die Laufwerkssperre des Trägers;
Reihenfolge immer Loop -> Träger, daher keine Verklemmung. Damit kommt
das Loop-Backend „durchs Dateisystem“.

## Plan je Version (Vorschlag)

| Version | Thema | Big Lock |
| --- | --- | --- |
| 6.1 | Loop, DriveSpace | fat.c: globale Variablen in eine Struktur je Operation; danach Tiefe 2 + Loop-Backend durchs Dateisystem |
| 6.2 | Netzwerk | Sperren für Objekte statt Laufwerke (FAT-, Verzeichnis-, EA-Cache einzeln); ISO9660.IFS und Redirector wie fat.c; Netzstapel von Anfang an ohne `klock` – erledigt, siehe „Stand 6.2“ |
| 6.3 | VirtIO, SMB/Domäne | übriger Kernel: Dateitabelle/Handles, Pipes, Taskverwaltung, Speicher prüfen und markieren; Diskette mit eigener Sperre; `klock` entfällt, alle FAPI-Aufrufe wie heute `fapi_nolock` |
| 6.x danach | nach Bedarf | mehrere Aufträge je Treiber (AHA154X mehrere CCBs, NCR mit Trennen) |
| 7.0 | 32 Bit | Kernel von der ersten Zeile an wiedereintrittsfest: Zustand je Aufruf, Aufträge durch Schichten (wie NT-IRPs), Arbeitsfäden; Loop durchs Dateisystem, Extents nur für die Auslagerungsdatei |

## Regeln

- Grenzen bei 16 Bit: DGROUP ist voll – neuer Zustand in die Seite des
  Tasks (`dmab_pg`/Kontext) oder in Kernelsegmente; Kernelstapel 768
  Worte, keine großen Puffer auf den Stapel.
- Pflicht nach jedem Schritt: zwei XCOPY auf verschiedenen Laufwerken
  gleichzeitig, DOS-Box daneben, CHKDSK und fsck sauber, Test auf dem
  386 mit 4 MB (Untergrenze) und in 86Box beim Nutzer.
- Schnittstellen nach außen (FAPI, Treiber-DevHlp) ändern sich dadurch
  nicht.

## Stand 6.2 (19.11.2026)

Bestandsaufnahme vor dem Umbau: der grösste Teil der Zeile „6.2“ war mit
Phase 6 schon erledigt – Laufwerkssperre je Laufwerk (auch FAT12/16),
EA-Namenscache mit eigener Sperre (`VK_EA`), FAT-Cache je Task, Kontext je
Task für fat.c, kernel.c und den Redirector (redir.c), Netzstapel ohne
`klock` (FAPI_NET ist `fapi_nolock`, eigene Sperren im Modul).

Übrig war die Brücke zu den IFS-Modulen: ein gemeinsamer Auftrag `ireq`
und eine Sperre `MOD_DL` für alle Module und alle CD-Laufwerke, dazu der
Zustand von ISO9660.IFS in globalen Variablen. Jetzt:

- `ireq` liegt im Kontext je Task (`kern_ctx`), `MOD_DL` entfällt – die
  Laufwerkssperre reicht (Reihenfolge Laufwerk -> Treiber).
- ISO9660.IFS hält alles, was über einen Lesezugriff lebt (Einhängedaten,
  Eintrag, UCS-2-Name, Rock-Ridge-Fortsetzungen, Puffer von fill_alias/
  namepath), in einer Struktur je Gerät; vier Plätze, bei Bedarf einem
  Gerät zugeordnet (frei, sonst der älteste, der gerade nicht arbeitet –
  dessen nächster Auftrag hängt neu ein). Der Zeiger auf den Platz wird
  nach jedem Lesen (Schlafpunkt) neu gesetzt. Modul 16 KB statt 9 KB.

Gemessen (QEMU, zwei gedrosselte CDs an IDE und NCR): jede allein 62 s,
beide gleichzeitig 61 s – mit dem alten wie mit dem neuen Stand. Die alte
Sperre fiel in QEMU nicht auf (die Zeit liegt im Treiber, unter der Sperre
je Gerätegruppe). Der Gewinn ist die fehlende gemeinsame Sperre, nicht
Tempo. Geprüft: beide CDs gleichzeitig nach C: (Inhalte gleich), fünf
Geräte reihum (CD + vier ISO-Loops, verdrängt Plätze) mit langen Namen,
Kopien bytegleich, DOS-Box auf ein Loop-Laufwerk, 16 und 4 MB, fsck sauber.

Für 6.3 bleibt (Tabelle): Dateitabelle/Handles, Pipes, Taskverwaltung,
Speicher, Diskette mit eigener Sperre, dann `klock` weg.

## Offene Punkte (Durchsicht 15.11.2026)

- Tiefe 2 geht nicht mit dem heutigen Kontexttausch (`ctx_take` tauscht
  nur zwischen Tasks – ein verschachtelter Aufruf desselben Tasks
  überschriebe seinen eigenen Stand). Deshalb zuerst „fat.c-Zustand je
  Operation“, dann Tiefe 2 – wie in der Tabelle.
- Die GUI (GUI-WIN16.md) braucht unabhängig davon ihre eigene Sperre für
  die Win16-Programme (Win16Mutex im GUI-Prozess).
- Gemeinsamer Fahrplan der Pläne: CLAUDE.md, „Fahrplan nach 6.0“.
