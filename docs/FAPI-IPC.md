# FAPI-IPC: Module, Semaphoren, gemeinsame Segmente, Named Pipes, Threads

Entwurf (27.11.2026). Anlass: Vergleich mit OS/2 1.x (Hinweis über den
Nutzer) – AT-DOS fehlen Threads, Semaphoren, gemeinsamer Speicher, Named
Pipes, DosLoadModule; die Task-Grenze liegt bei 8. Entscheidung des
Nutzers: Umsetzung auf `main`, erscheint mit **AT-DOS 6.5**; 6.4 bekommt
bis zur Freigabe nur Fehlerbehebungen.

Vorbild ist die Schnittstelle von OS/2 1.x (Dos*-Aufrufe), damit
Programme sich ähnlich anfühlen. Nachgebaut wird das Verhalten, nicht der
Code (OS/2-Quellen sind nicht frei). Programme bleiben 286-Code (FAPI wie
bisher), der Kernel 16 Bit.

## Bestand

- Tasks: `NTASK` 8 (`kernel/kern.h`), zählt FAPI-Programme, DOS-Boxen und
  Sitzungen. Je Task ein 386-TSS und ein Kernelstapel von 768 Worten in
  DGROUP (gut 60 KB belegt).
- Warten/Wecken: `DH_PROCBLOCK`/`DH_PROCRUN` (Ereignis = 32 Bit), Frist,
  Signale (`sig_deliver`, Rückkehr wartender Aufrufe bei SIG_BREAK).
- Pipes: nur anonym (`FAPI_PIPE` 28h), Handles `OF_PRD`/`OF_PWR`.
- Segmente: `DosAllocSeg` je Task in der eigenen LDT; gemeinsame
  Segmente nur im NE-Lader (DLL-Code, gemeinsame DLL-Daten, Plätze von
  oben in der LDT, in allen Tasks gleich, `ne_sh`).
- Module: NE-Lader (`kernel/neload.c`) lädt DLLs nur beim Programmstart;
  `FAPI_GETMOD` (65h), `FAPI_GETRES` (64h).

## Funktionsnummern

5.x belegt bis 5Eh, `main` bis 66h. Neu (Unterfunktion in BX wie bei
`FAPI_LOOP`/`FAPI_NET`, Nummern bleiben über beide Linien eindeutig):

| Nr. | Name | Unterfunktionen |
|-----|------|-----------------|
| 67h | `FAPI_MODULE` | LOAD, FREE, PROCADDR, NAME |
| 68h | `FAPI_SEM` | CREATE, OPEN, CLOSE, SET, CLEAR, WAIT, SETWAIT, REQUEST, MUXWAIT |
| 69h | `FAPI_SHRSEG` | ALLOC, GET, GIVE, GETGIVEN |
| 6Ah | `FAPI_NPIPE` | MAKE, CONNECT, DISCONNECT, PEEK, QSTATE, WAIT, CALL, TRANSACT |
| 6Bh | `FAPI_THREAD` | CREATE, EXIT, GETINFO, SUSPEND, RESUME |
| 6Ch | `FAPI_QUEUE` | CREATE, OPEN, WRITE, READ, PEEK, PURGE, CLOSE |
| 6Dh | `FAPI_PRTY` | SET, GET (Klasse + Stufe) |
| 6Eh | `FAPI_TIMER` | ASYNC, START, STOP (Semaphore wird gesetzt) |

Namen wie OS/2: `\SEM\…`, `\SHAREMEM\…`, `\PIPE\…`, `\QUEUES\…` – bis
260 Zeichen wie alle FAPI-Pfade, im Kernel ohne Groß/klein verglichen.
Neue Nummern in `include/fapi_abi.h` und im README eintragen, Stub
(stub/fapidos.c) liefert unter MS-DOS „nicht unterstützt“, außer wo es
sinnvoll nachzubilden ist (DosLoadModule nein, RAM-Semaphoren ja).

## Stufe 1: DosLoadModule (risikoarm)

- `MODULE_LOAD` (BX), CX:DX = Name (`FOO` sucht `FOO.DLL` wie beim
  Programmstart: Programmverzeichnis, `LIBPATH=` aus der CONFIG.SYS,
  sonst `\DOS\SYSTEM`, PATH) -> AX = Modulkennung wie `FAPI_GETMOD`.
  Der NE-Lader legt die DLL wie beim Start an (Plätze von oben in der
  LDT, MULTIPLEDATA-Kopie, Zähler je Task in `task.nemods`), ruft den
  DLL-Start über `ne_stub` auf.
- `MODULE_FREE`: Zähler herunter; beim letzten Benutzer DLL-Ende (WEP-
  Gegenstück, falls vorhanden) und Segmente frei.
- `MODULE_PROCADDR`: Kennung + Ordinal oder Name -> CS:IP (Export-
  tabelle; Name über die Resident-/Non-Resident-Namen).
- `MODULE_NAME`: Kennung -> voller Pfad.
- Fehlercodes: nicht gefunden, kein NE, Importe fehlen (`EXEC_ERR_DLL`).
- Test: Watcom `format os2` DLL wie in `docs/GUI-WIN16.md` (NE-Lader
  Stufe 1), zur Laufzeit laden, Funktion per Name und Ordinal rufen,
  freigeben, erneut laden, MEM gleich (kein Leck).

### Stand Stufe 1 (27.11.2026, in QEMU geprüft)

`kernel/neload.c` `ne_modload`/`ne_modfree`/`ne_procaddr`/`ne_modname`,
Verteiler in kernel.c (`FAPI_MODULE`), Wrapper in apps/fapi.h
(`DosLoadModule`, `DosFreeModule`, `DosGetProcAddr`, `DosGetModName`).

- Geht auch für FX-Programme: deren LDT-Einträge 0/1 und DosAllocSeg
  kommen den gemeinsamen DLL-Plätzen von oben nicht in die Quere.
- Suchweg ohne Pfad: aktuelles Verzeichnis (wie `LIBPATH=.`), Verzeichnis
  des NE-Programms, `\DOS\SYSTEM`, `\DOS`; mit `:` oder `\` nur der Pfad.
  `LIBPATH=` aus der CONFIG.SYS fehlt noch.
- Zähler je Task und DLL (`NEX(t)->dl`), Importe der EXE (`NEX(t)->stat`)
  bleiben immer; `ne_detach` hängt ab, was keiner mehr braucht (Abhängig-
  keiten mitgerechnet), Instanzkopien frei, letzter Benutzer gibt die DLL
  frei.
- Start-Routinen im Ring 3 vor der Rückkehr: der Kernel legt die Liste
  wie beim Programmstart auf den Stapel des Aufrufers, dahinter BP, SI,
  DI, BX, CX, DX, ES, DS, IP, CS aus dem FAPI-Rahmen, und biegt den
  Rücksprung auf den zweiten Teil des Stummels (`NE_RSTUB`). Meldet eine
  Start-Routine 0, gibt der Stummel die DLL per MODULE_FREE wieder frei
  und liefert FAPI_ERR_DENIED; die Register des Aufrufers bleiben.
- Nebenbei: die Suche nach nicht residenten Namen (`ord_byname`) schließt
  die DLL-Datei jetzt (vorher blieb sie offen – auf Netzlaufwerken ein
  Leck).
- Noch nicht: Ende-Routine der DLL beim Freigeben (WEP), `LIBPATH=`,
  Fehlername wie OS/2 (pszFailName).

Geprüft mit Testprogrammen im Scratchpad (NETDLL mit Start-Routine und
nicht residenten Namen, NETBAD mit scheiternder Start-Routine): Laden aus
dem aktuellen Verzeichnis und über Pfad, Aufruf über Ordinalzahl und
Namen, doppeltes Laden zählt, Freigeben, Fehlerfälle, Programmende ohne
Freigeben, drei Runden ohne Speicherverlust; NE-Programm mit Importen
unverändert.

## Stufe 2: Semaphoren

- RAM-Semaphoren: ein Doppelwort im Speicher des Programms (wie OS/2:
  0 = frei); Kennung = Selektor:Offset. Warten über ProcBlock mit
  Ereignis aus dem linearen Ort; SET/CLEAR prüfen und wecken. Nur
  innerhalb eines Tasks bzw. in gemeinsamen Segmenten zwischen Tasks.
- Systemsemaphoren `\SEM\name`: Tabelle im Kernelsegment (z. B. 64
  Einträge: Name-Hash + Name in eigenem Segment, Zustand, Besitzer,
  Zähler der Öffnungen, Exklusiv-Merker). REQUEST = besitzen mit
  Rekursionszähler; Ende des Besitzers gibt frei und meldet „Besitzer
  beendet“ (ERROR_SEM_OWNER_DIED) wie OS/2.
- MUXWAIT: bis 16 Semaphoren, Rückgabe des Index.
- Frist in ms wie OS/2 (-1 ewig, 0 nur prüfen).
- Signale: wartende Aufrufe kehren bei Strg+C zurück (wie KbdCharIn).
- Test: zwei Tasks, Erzeuger/Verbraucher; Besitzer per TASKMAN /K
  beenden -> anderer bekommt „Besitzer beendet“; MUXWAIT.

### Stand Stufe 2 (27.11.2026, in QEMU geprüft)

`kernel/sem.c` (KTEXT4, 2,6 KB), Verteiler `FAPI_SEM` in kernel.c, Wrapper
in apps/fapi.h (`DosCreateSem`, `DosOpenSem`, `DosCloseSem`, `DosSemSet`,
`DosSemClear`, `DosSemWait`, `DosSemSetWait`, `DosSemRequest`,
`DosMuxSemWait` – je mit Zeiger auf `struct fapi_sem` bzw.
`struct fapi_muxsem`).

- Läuft **ohne Kernelsperre** (`fapi_nolock`, wie Netz und UFS): der
  Kernel wird nie unterbrochen, zwischen zwei Schlafpunkten ändert sich
  nichts; nach jedem Aufwachen wird die Kennung neu geprüft.
- Kennung 32 Bit: RAM-Semaphore = Fernzeiger auf das Doppelwort (Wort 0:
  0 frei, sonst gesetzt; kein Besitzer, kein Anlegen), Systemsemaphore =
  0000h:(Generation << 8 | Platz + 1) – eine alte Kennung trifft nach dem
  Löschen keinen neuen Eintrag.
- Tabelle: 32 Systemsemaphoren (`NSEM`) in einem Kernelsegment (9 KB),
  erst beim ersten Anlegen geholt; Name bis 260 Zeichen, groß
  gespeichert, muss mit `\SEM\` beginnen. Zähler der Öffnungen je Task,
  der letzte Verweis löscht.
- REQUEST merkt Besitzer und Zähler; CLEAR des Besitzers gibt erst beim
  letzten frei. Endet der Besitzer (`sem_task_end` aus task_end), wird
  sie frei, der nächste REQUEST bekommt sie und das Ergebnis 1 statt 0
  („Vorbesitzer beendet“ – kein Fehlercode, der Aufrufer besitzt sie).
  `SEM_EXCL`: solange sie jemandem gehört, setzt/löscht nur er (FFFBh).
  CLOSE mit eigener Belegung -> FFFAh.
- Warten in Stücken von 200 ms (`SLICE`): danach Signal prüfen
  (`FAPI_ERR_INTR` FFF1h, neu) und bei RAM-Semaphoren die lineare Adresse
  neu bilden (sie ist das Ereignis; ein verschobenes Segment bliebe
  sonst hängen). Geweckt wird sofort (`proc_run`), die Stücke sind nur
  der Rückfall. MUXWAIT wartet auf ein gemeinsames Ereignis, das jedes
  Freigeben auslöst.
- Stub (MS-DOS): RAM-Semaphoren gehen (ein Task – Warten auf eine
  gesetzte meldet sofort „Frist abgelaufen“), Systemsemaphoren nicht.
- Noch nicht: Zeitgeber, die eine Semaphore setzen (Stufe 7);
  Semaphoren aus Interruptroutinen von Treibern (DevHlp SemClear).

Geprüft mit `\DEMO\SEMTEST.EXE` (nur Platte): Selbsttest (26 Fälle, RAM
und System, Fristen, Zähler, Mux, Kennung nach dem Löschen), Diener und
Kunde in zwei Tasks über 500 Runden, Besitzer endet ohne Freigabe ->
wartender REQUEST bekommt 1, Mux mit Freigabe aus einem anderen Task,
Strg+C beendet einen Wartenden (Semaphoren danach wieder anlegbar),
gebunden in der DOS-Box (RAM-Teil). Nicht geprüft: Rückkehr mit
`FAPI_ERR_INTR` bei eigenem Signalhandler.

## Stufe 3: gemeinsame Segmente

- `SHRSEG_ALLOC(Name, Größe)`: Speicher wie DosAllocSeg, dazu ein Platz
  in einer Kernel-Tabelle (Name, Seiten, Zähler). Jeder Task bekommt
  beim GET einen eigenen LDT-Eintrag auf denselben Speicher (Alias wie
  `D_ALIAS`, aber mit Zähler statt „nur Eintrag frei“).
- `GIVE`/`GETGIVEN` (OS/2 DosGiveSeg) ohne Namen: Selektor im Ziel-Task.
- Freigabe: DosFreeSeg/Task-Ende zählen herunter, beim letzten frei.
- Auslagern: Segmente mit mehreren Besitzern nicht verschieben/auslagern
  (`seg_compact`/`seg_out` überspringen sie) – sonst stimmen die anderen
  Deskriptoren nicht mehr.
- Test: zwei Tasks schreiben/lesen, zusammen mit Semaphore aus Stufe 2.

### Stand Stufe 3 (27.11.2026, in QEMU geprüft)

`kernel/shrseg.c` (KTEXT4, 1 KB), Verteiler `FAPI_SHRSEG`, Wrapper
`DosAllocShrSeg(Größe, Name)`, `DosGetShrSeg(Name)`, `DosGiveSeg(Selektor,
Task)` – alle liefern einen Selektor.

- Speicher über `seg_alloc` (mit Nullen), danach trägt der LDT-Eintrag
  `D_ALIAS`; Tabelle (32 Segmente, `NSHR`) im Kernelsegment, erst beim
  ersten Anlegen geholt: erste Seite, Seitenzahl, Grenze, Verweise, Name.
- Jeder Task hat seinen eigenen Selektor (LDT je Task). GIVE legt den
  Eintrag im Ziel-Task an und liefert dessen Selektor – der Geber teilt
  ihn dem anderen mit (z. B. über ein benanntes Segment). Ein eigenes
  GETGIVEN braucht es deshalb nicht. Ohne Namen (`DX` = 0) ist ein Segment
  nur weiterzugeben.
- Freigeben: `seg_free` meldet jeden freigegebenen Alias-Eintrag mit
  seiner Seite an `shr_unref`; der letzte Verweis gibt den Speicher
  zurück (auch beim Programmende, task_end läuft über seg_free).
- Verschieben/Auslagern: `seg_compact`/`seg_out` und SEG_REALLOC
  übergehen Alias-Einträge schon.
- RAM-Semaphoren in gemeinsamen Segmenten gehen zwischen Tasks (Ereignis
  = lineare Adresse, in allen Tasks gleich).
- Läuft **mit** Kernelsperre wie DosAllocSeg – die Speicherverwaltung
  steht noch darunter (Abbau s. u.).
- Nicht: Stub (MS-DOS), DOS-Boxen als Ziel von GIVE, Schutz „nur lesen“.

Geprüft mit `\DEMO\SHRTEST.EXE` (nur Platte): Fehlerfälle in einem Task
(doppelter/falscher Name, zwei Selektoren auf denselben Speicher,
Freigabe in beiden Reihenfolgen, 64 KB ohne Namen), Diener und Kunde in
zwei Tasks (Muster hin und zurück, RAM-Semaphoren im Segment, zweites
Segment per GIVE, Geber gibt zuerst frei), MEM davor/danach gleich.

## Stufe 4: Named Pipes

- `\PIPE\name` als Handle-Art `OF_NPIPE` (wie `OF_SOCK`): Server legt
  mit MAKE bis zu N Instanzen an (Puffer je Richtung, z. B. 4 KB im
  Kernelsegment), CONNECT wartet auf einen Client; Client öffnet über
  normales DosOpen auf `\PIPE\name` (FAPI_OPEN erkennt den Pfad).
- Byte- und Nachrichtenbetrieb, blockierend/nicht blockierend, PEEK,
  QSTATE, TRANSACT/CALL wie OS/2.
- Lesen/Schreiben über die vorhandenen Wege von DosRead/DosWrite (ohne
  Kernelsperre wie bei Pipes), Ende des Gegenübers -> 0 Byte / Fehler.
- Später über das Netz (SMB-Named-Pipes) – nicht in 6.5.
- DOS-Boxen: INT 21h 3Dh auf `\PIPE\…` über den DEM (wie MS-DOS mit LAN
  Manager) – erst nach Bedarf.

### Stand Stufe 4 (27.11.2026, in QEMU geprüft)

`kernel/npipe.c` (KTEXT4, 3 KB), Verteiler `FAPI_NPIPE`, Handle-Art
`OF_NPIPE` (task.h), Wrapper `DosMakeNmPipe`, `DosConnectNmPipe`,
`DosDisConnectNmPipe`, `DosPeekNmPipe`, `DosWaitNmPipe`,
`DosTransactNmPipe`, `DosCallNmPipe`.

- 8 Instanzen im System (`NNP`), je zwei Ringe zu 2 KB (`NPBUF`), alles in
  einem Kernelsegment (35 KB), erst beim ersten MAKE geholt. Mehr erst
  mit Stufe 5 (dann auch Puffergröße je Pipe).
- Kunde: `DosOpen("\\PIPE\\name")` – FAPI_OPEN fragt bei Pfaden, die mit
  `\` beginnen, zuerst `np_open` (unter der Sperre von FAPI_OPEN, schläft
  nicht). Ein echtes Verzeichnis `\PIPE` im Stamm ist damit nur noch mit
  Laufwerksbuchstaben erreichbar (wie bei OS/2). Keine Instanz lauscht ->
  FFFAh (dann DosWaitNmPipe), Name unbekannt -> FFFFh.
- Lesen/Schreiben/Schließen über DosRead/DosWrite/DosClose, **ohne
  Kernelsperre** (`fapi_nolock` wie Pipes und Sockets), Warten in Stücken
  wie bei den Semaphoren (Signal -> FFF1h). DosQHandType meldet HT_PIPE.
- Zustände DISC/LISTEN/CONN; schließt eine Seite, liest die andere den
  Rest und dann 0 Byte, Schreiben scheitert (PEEK meldet NPS_CLOSING).
  DISCONNECT des Dieners hängt den Kunden ab und leert die Puffer; das
  Handle des alten Kunden ist dann tot (Generation im Handle), die
  Instanz gleich wieder verwendbar.
- Nachrichtenbetrieb (`NP_MSG`): Längenwort im Ring, Nachricht bis 2046
  Byte, ein Lesen liefert höchstens eine; kleinerer Puffer -> Rest beim
  nächsten Lesen. TRANSACT/CALL nur dort. CALL hängt den Kunden ohne
  Handle an (`cltask`, `np_task_end` räumt auf, wenn er dabei endet).
- Abweichungen von OS/2: kein QSTATE (PEEK mit Größe 0 liefert Zustand
  und Füllstand), kein nicht blockierender Betrieb, keine getrennten
  Lese-/Schreibrichtungen (immer beide), ein Betrieb für beide Seiten.
- Nicht: Stub (MS-DOS), DOS-Boxen (INT 21h 3Dh auf `\PIPE\`), Netz.

Geprüft mit `\DEMO\NPTEST.EXE` (nur Platte), Diener und Kunde in zwei
Tasks: Bytebetrieb 20 × 1000 Byte hin und zurück, besetzte Instanz,
unbekannter Name, Nachrichten mit 1/100/2046 Byte über TRANSACT, zu große
Nachricht, PEEK, Teil-Lesen einer Nachricht, zweimal CALL, Diener
schließt zuerst (Rest lesen, Dateiende, Schreiben scheitert); anonyme
Pipes (`|`) unverändert.

Platz: KTEXT4 danach 63.058 Byte (2,4 KB frei) – vor dem nächsten Modul
dort ein fünftes Codesegment anlegen oder IPC-Module verschieben.

## Dazwischen: Rest der Kernelsperre (vor Stufe 5/6)

Entscheidung des Nutzers (27.11.2026): der Abbau von `klock`, in
`docs/BIGLOCK.md` für 6.3 vorgesehen und liegen geblieben, kommt in 6.5 –
nach den Stufen 2–4 (die gleich ohne `klock` gebaut werden) und vor den
Threads. Grund: Threads teilen Handles, LDT und Speicher eines Prozesses,
genau die Tabellen, die noch unter `klock` stehen (Dateitabelle/Handles,
Pipes, Taskverwaltung, Speicher; Diskette mit eigener Sperre).

## Stufe 5: mehr Tasks (nach der Freigabe von 6.4)

- Kernelstapel und TSS aus DGROUP in Kernelsegmente (je Task eine Seite
  oder ein gemeinsames Segment); SS = DS-Annahme im Kernel-C prüfen
  (heute „Kernel-C setzt SS = DS voraus“) – deshalb wohl Stapel weiter
  in einem Segment, das als DS taugt, oder ein Umbau der Annahme.
- `NTASK` 8 -> 32 (Tabellen in Kernelsegmente: `task[]`, LDT-Block,
  `fpu_area`, Box-Tabellen je Task).
- Prüfen in 86Box (Scheduler, Boxen): Pflicht.

## Stufe 6: Threads (nach Stufe 5)

- Thread = Task-Eintrag mit eigener TSS und eigenem Kernelstapel, aber
  gemeinsamer LDT und gemeinsamen Handles/Umgebung/Verzeichnis mit dem
  Prozess (`task.proc` zeigt auf den ersten Eintrag).
- `THREAD_CREATE(CS:IP, SS:SP)` wie DosCreateThread (Stapel stellt das
  Programm), `THREAD_EXIT`, Prozessende beendet alle Threads.
- Was heute je Task gilt, muss je Prozess gelten: Handles, LDT-Freigabe
  (`seg_free` erst, wenn kein Thread den Selektor mehr hält – über die
  Prozessgrenze sauber), Signale an Thread 1, `task_end`.
- Kernelsperre/Kontext je Thread (wie je Task), ein Thread im Kernel
  blockiert die anderen nicht.
- Test: Programm mit zwei Threads + Semaphore, Prozessende mitten im
  Warten, Strg+C, TASKMAN zeigt Threads.

## Stufe 7: Warteschlangen, Prioritäten, Zeitgeber

- Warteschlangen (`\QUEUES\…`): Einträge mit Zeiger in gemeinsames
  Segment wie OS/2 (FIFO/LIFO/Priorität), READ blockiert.
- Prioritäten: Klassen idle/normal/zeitkritisch wie OS/2, der Scheduler
  bevorzugt höhere Klassen; DOS-Boxen bleiben „normal“.
- Zeitgeber: DosTimerAsync/Start setzen eine Systemsemaphore.

## Offene Fragen (vor Stufe 5/6 klären)

- Kernelstapel außerhalb von DGROUP vs. „SS = DS“ im Kernel-C.
- Wie viele Tasks auf dem 386 mit 4 MB sinnvoll sind (Speicher je Task:
  LDT 4 KB, DMABUF/Kontext-Seite 4 KB, Kernelstapel).
- Gerätemonitore (DosMon*) für Tastatur/Maus – erst mit Bedarf.
