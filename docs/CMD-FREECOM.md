# Befehle aus FreeCOM für CMD

Stand: gebaut (Wunsch des Nutzers, 25.11.2026: „die fehlenden Befehle aus
FreeCOM nach CMD portieren“; Auftrag „1, 9 und 7 machen“). Stufen 1–5
erledigt, DESCRIPT.ION nicht (nach Absprache). Einzelheiten unter „Stand“
am Ende.

Grundlage ist ein Vergleich der internen Befehle von CMD.EXE
(`apps/task_cmd.c`, Tabelle `builtin[]` und Verteiler `execute()`) mit
FreeCOM d52ba90 (`freedos/COMMAND.COM`; Quelltext `shell/cmdtable.c`,
`config.h`, `cmd/*.c`). FreeCOM ist GPL wie AT-DOS, trotzdem wird nichts
übernommen: CMD ist FAPI-Code mit eigenen Hilfen (util.c, walk_tree,
country.h), FreeCOM ist DOS-Code mit INT 21h. Vorbild ist nur das
Verhalten, bei NT-Befehlen das Verhalten von CMD.EXE unter NT.

## Platz

CMD stand schon bei der Stapelsprache knapp an 64 KB (CLAUDE.md
„Stapelsprache“, `cbuf` auf 4 KB verkleinert). Vor jeder Stufe Code- und
Datengröße aus der Map messen. Größere Puffer (Verzeichnisstapel, Liste
für DEL /P) gehören per DosAllocSeg in eigene Segmente, erst bei Bedarf
geholt (wie die DOSKEY-Makros).

## Stufe 1: neue Befehle

| Befehl | Plan | Bemerkung |
|---|---|---|
| PUSHD, POPD, DIRS | Stapel von Laufwerk + Verzeichnis, 8 Plätze, Segment erst beim ersten PUSHD. PUSHD wechselt auch das Laufwerk (wie NT). DIRS zeigt den Stapel. | Je Sitzung wie die Makros. Ob eine mit START gestartete CMD-Sitzung den Stapel erbt: zu klären (Vorschlag: nein). |
| HISTORY | zeigt die gespeicherten Zeilen (heute `NHIST` 16 in DGROUP). `HISTORY n` ändert die Größe, dann in einem eigenen Segment. | DOSKEY /HISTORY als zweiter Name, wie MS-DOS. |
| ALIAS | zweiter Name für die eingebauten DOSKEY-Makros: `ALIAS` listet, `ALIAS name=text` setzt, `ALIAS name=` löscht. | Gleicher Speicher wie DOSKEY, kein zweiter Mechanismus. |
| WHICH | intern, zeigt, was CMD für ein Wort ausführen würde: Makro, interner Befehl oder Programm mit vollem Pfad (dieselbe Suche wie `run_program`, also auch .CMD/.BAT/.COM und die DOS-Box). | Mehrwert gegenüber WHERE.EXE, das nur Dateien sucht. Das Makro `which=WHERE $*` in LINUX.MAC fällt dann weg. |
| CDD | wie `CD /D` von NT: Laufwerk und Verzeichnis zusammen wechseln. Dafür auch `CD /D` einbauen. | Klein. |
| `?` | wie HELP/HILFE ohne Argument. | Schon fast da (`?` geht im Verteiler); nur prüfen, ob es überall greift. |
| BEEP | Ton über den PC-Lautsprecher. | Klein, braucht einen Weg zum Lautsprecher: FAPI-Aufruf oder Ausgabe von 07h über die Konsole (vio piept heute vermutlich nicht – prüfen). |
| TITLE | Titel der Sitzung in der Titelzeile (Zeile 0) setzen, wie NT. | Braucht einen neuen FAPI-Aufruf für den Sitzungstitel (gibt es heute nicht). In FreeCOM ohne Wirkung – hier mit Wirkung oder gar nicht. |

## Stufe 2: Schalter bei vorhandenen Befehlen

| Befehl | Plan |
|---|---|
| COPY | `/Y`, `/-Y` und Umgebungsvariable `COPYCMD` wie MS-DOS 6.2: Rückfrage vor dem Überschreiben (heute überschreibt COPY still). In Stapeldateien ohne Rückfrage, wie MS-DOS. `/V` liest nach dem Schreiben zurück und vergleicht. `/A` und `/B` mit Textmodus (bis ^Z lesen, ^Z anhängen). |
| DEL/ERASE | `/P` fragt je Datei (Treffer erst sammeln wie REN). `/Q` und `/S` wie NT gleich mit (walk_tree). |
| SET | `/P Name=Text` liest eine Zeile von der Tastatur (wichtig für Stapeldateien). `/E Name=Befehl` legt die erste Zeile der Ausgabe ab (FreeCOM; über eine Pipe, die der Kernel schon hat). `/A` (Rechnen wie NT) bauen, obwohl FreeCOM es nicht hat – ohne 32-Bit-Division, 16 Bit reicht zuerst. `/C`, `/U`, `/I` von FreeCOM nicht: sie regeln Groß/klein und Platz der DOS-Umgebung, die es in CMD so nicht gibt. |
| IF | `/I` (Vergleich ohne Groß/klein), dazu `DEFINED` und Vergleiche `EQU NEQ LSS LEQ GTR GEQ` wie NT. |
| FOR | `/D` (nur Verzeichnisse), `/R [Pfad]` (rekursiv über walk_tree), `/L (Start,Schritt,Ende)` wie NT. FOR schachtelbar machen (heute nicht). `/F` als eigener Schritt danach (Zeilen einer Datei oder eines Befehls zerlegen, `tokens=`, `delims=`, `skip=`, `eol=`) – der größte Brocken. |
| CALL | `/Y` (Stapeldatei schrittweise wie F8, über das vorhandene `step`). `/S` und `/N` nicht: sie betreffen das Auslagern von FreeCOM selbst. |
| DATE, TIME | `/T` gibt nur aus, ohne Eingabe (NT, FreeCOM). `/D` von FreeCOM nicht nötig. |
| DIR | `/L` (Namen klein). `/4` und `/Y` von FreeCOM nicht: CMD zeigt das Jahr ohnehin vierstellig. |

## Nicht übernehmen

- **LFNFOR**: schaltet in FreeCOM lange Namen für FOR ein. CMD liefert
  lange Namen immer (LFN-Merker); FOR muss sie einfach mitnehmen.
- **MEMORY**: zeigt den Speicher von FreeCOM selbst – ohne Sinn in CMD
  (MEM zeigt alles).
- **LOADFIX**: Programm oberhalb 64 KB laden, nur für DOS-Programme.
  Gehört, wenn überhaupt, in die DOS-Box (dort läuft LOADFIX von MS-DOS
  bzw. FreeDOS ohnehin).
- **LH/LOADHIGH echt**: bleibt angenommen ohne Wirkung – FX-Programme
  haben keinen oberen Speicher.
- **Installierbare Befehle (INT 2Fh AE00h)**: DOS-Schnittstelle, gilt in
  der Box schon für FreeCOM und COMMAND.COM 4.0.

## Später, eigener Entwurf

- **DESCRIPT.ION** (Beschreibungen je Datei, wie 4DOS/FreeCOM): DIR
  zeigt sie, COPY/MOVE/REN/DEL pflegen sie mit. Betrifft mehrere Befehle
  und FM; mit dem Nutzer klären, ob das gewünscht ist. Lange Namen
  decken einen Teil des Zwecks ab.

## Reihenfolge (Vorschlag)

1. PUSHD/POPD/DIRS, CDD + `CD /D`, HISTORY, ALIAS, WHICH (reine
   Programmlogik, QEMU reicht).
2. SET /P, IF /I, COPY /Y + COPYCMD, DEL /P, DATE/TIME /T, DIR /L.
3. FOR /D /R /L und Schachteln, SET /A, CALL /Y, COPY /V /A /B.
4. FOR /F.
5. BEEP und TITLE (brauchen Kernelteile), DESCRIPT.ION nach Absprache.

Jede Stufe: README („Was läuft“) und `HELP`-Texte (`/?`) im selben
Commit nachtragen; Diskette 1 auf Platz prüfen (CMD.EXE liegt dort).

## Stand (25.11.2026)

Gebaut in `apps/task_cmd.c`, alles eigener Code:

- Stufe 1: PUSHD/POPD/DIRS (8 Plätze, Segment beim ersten PUSHD; START
  erbt den Stapel nicht), `CD /D`, CDD, HISTORY [n] (n = nur die letzten n
  – die Größe bleibt 16), ALIAS, WHICH. Das Makro `which=WHERE $*` in
  LINUX.MAC ist weg (es verdeckte den Befehl).
- Stufe 2/3: SET /P, SET /E (erste nicht leere Zeile), SET /A (32 Bit
  über eigene Mul/Div, `umul32`/`udiv32`), IF /I/DEFINED/EQU…, COPY
  /Y /-Y /V /A /B + COPYCMD (Rückfrage nur an der Eingabe, wie MS-DOS
  6.2), DEL /P /Q /S (Baumgang `tree_walk`, eigener Stapel je Aufruf),
  DATE/TIME /T, DIR /L, FOR /D /R /L und Schachteln (3 Ebenen, Puffer je
  Ebene in `flv[]`), CALL /Y (`step_lvl`).
- Stufe 4: FOR /F mit Dateien (bis 64 KB je Datei), "Text" und 'Befehl'
  (Ausgabe über eine Datei in %TEMP%, `capture`), `usebackq`.
- Stufe 5: BEEP über FAPI 61h `FAPI_BEEP` (Kernel stellt PIT-Kanal 2,
  wie OS/2 DosBeep), TITLE über FAPI 62h `FAPI_SETTITLE`.
- Nebenbei: `ECHO x> datei` schrieb Leerzeichen an das Zeilenende (der
  Platz der Umleitung) – parse_line kürzt sie.

Nicht gebaut: `CALL :Marke` (NT), `%~`-Modifizierer, DESCRIPT.ION.
Geprüft: QEMU (Stapeldatei mit allen Befehlen, Rückfragen von COPY/DEL,
SET /P, CALL /Y, TITLE), 86Box 386DX/40 bzw. 486 (BEEP läuft – hörbar
nicht geprüft, 86Box hier ohne Ton).
Platz: CMD.EXE Code 36 KB, Daten 61 KB; Diskette 1 wurde dafür voll –
DOMJOIN.EXE liegt jetzt auf Diskette 3.
