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
| 6.2 | Netzwerk | Sperren für Objekte statt Laufwerke (FAT-, Verzeichnis-, EA-Cache einzeln); ISO9660.IFS und Redirector wie fat.c; Netzstapel von Anfang an ohne `klock` |
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
