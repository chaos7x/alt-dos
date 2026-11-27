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
