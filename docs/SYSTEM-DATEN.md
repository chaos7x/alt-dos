<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Plan: System und Daten trennen

Stand: 15.11.2026, abgestimmt mit dem Nutzer. Nur Plan – noch kein Code.

Heute: Stamm nur `ATDOS.SYS`, `CMD.EXE`, `CONFIG.SYS`, `AUTOEXEC.CMD`;
System in `\DOS`, `\DOS\SYSTEM`, `\DOS\MDOS`; dazu `\TEMP`.

## Bausteine

1. **Feste Ordner und Variablen** (Grundlage, zuerst): `\DOS` System,
   `\Programme` Installiertes, Daten in `C:\Eigene Dateien` und
   `C:\Desktop` (lange Namen). Orte in Variablen, z. B. `DOCS=`,
   `DESKTOP=` – GUI, Datei-Dialoge und Werkzeuge nehmen nur die
   Variablen, nie feste Pfade.
2. **Einhängen wie /home** (Wunsch des Nutzers): eine Datenpartition
   erscheint als Ordner, unabhängig vom Laufwerksbuchstaben. CONFIG.SYS-
   Zeile z. B. `MOUNT=DATEN,C:\Eigene Dateien` hängt beim Start nach
   Bezeichnung oder Seriennummer ein (Grundlage JOIN). Grenzen wie bei
   JOIN; ein direkt gestartetes MS-DOS sieht weiter D:.
3. **Netzlaufwerke** (mit dem geplanten SMB-IFS): Benutzerordner dürfen
   auf einem Netzlaufwerk liegen, z. B. `DOCS=N:\Eigene Dateien`. Mit dem
   Domänenbeitritt aus dem AD-Konto (homeDrive/homeDirectory): bei der
   Anmeldung N: verbinden und DOCS setzen. Ohne Netz Rückfall auf die
   lokalen Ordner mit Meldung, kein Hängen. MOUNT soll auch Netzziele
   kennen (Zusammenspiel JOIN/SMB-Modul dann prüfen). Desktop bleibt
   vorzugsweise lokal.
4. **System schreibgeschützt** (später): nur `\DOS` und Stamm, nie
   Programmordner (alte Programme schreiben Spielstände/INI dorthin);
   Schalter für Updates.
5. **Benutzerverzeichnisse je Person**: erst mit Anmeldung.

Siehe auch [GUI-WIN16.md](GUI-WIN16.md).

## Offene Punkte (Durchsicht 15.11.2026)

- Die Variablen `DOCS=`/`DESKTOP=` lassen sich sofort einführen (wie
  TEMP im Kernel bzw. `SET` in CONFIG.SYS) und helfen schon CMD, FM und
  EDIT – guter erster Schritt vor jeder GUI.
- MOUNT auf JOIN aufgebaut übernimmt dessen Grenzen (Ziel leer, Laufwerk
  unbenutzt, nicht das Startlaufwerk) – beim Start unkritisch, zur
  Laufzeit prüfen. DOS-Boxen sehen den Ordner über den Redirector mit,
  als 8.3-Name (`EIGENE~1`).
- Datei als Quelle für MOUNT: siehe Loop-Laufwerke in
  [LOOP-DBLSPACE.md](LOOP-DBLSPACE.md).
