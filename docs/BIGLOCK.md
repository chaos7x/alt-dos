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
| 6.5 | IPC wie OS/2 1.x (docs/FAPI-IPC.md) | begonnen, siehe „Stand 6.5“ – der Rest aus der Zeile „6.3“ (dort liegen geblieben: 6.3/6.4 wurden Box-Module und eigener DOS-Kern) – Entscheidung des Nutzers, 27.11.2026: nach Semaphoren/gemeinsamen Segmenten/Named Pipes (die ohne `klock` entstehen), vor den Threads |
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

## Stand 6.5 (28.11.2026): Bestandsaufnahme und Diskette

Vor dem Umbau nachgesehen, was `klock` heute noch tut. Seit Phase 6 gibt
ein Task sie an fast jedem Schlafpunkt ab (`proc_block` mit `may_rel`,
Warten auf Laufwerks-/Treibersperren, freiwilliges Warten) und bekommt
danach seinen Kontext zurück (`ctx_take`). Sie ist damit vor allem die
**Marke, wessen Stand gerade in den statischen Puffern steht** (Pfade,
Sammler von fat.c, Redirector, DEM) – und nur dort eine echte Sperre, wo
sie über ein Warten hinweg gehalten wird. Das waren noch vier Stellen:

| Stelle | hielt `klock` über | Stand |
| --- | --- | --- |
| Diskette (floppy.c) | jeden Zugriff: Motoranlauf, Suchen, Spur lesen/schreiben | **erledigt**, eigene Sperre (s. u.) |
| Auslagerungsdatei (`kf_swap_rw`) | Lesen/Schreiben einer Seite; gemeinsamer Puffer `sw_buf` | offen: eigene Sperre für `sw_buf` |
| Platten-Cache ohne eigene Ablage (`bc_wb`, Zweig ohne `ctx_room`) | Schreiben direkt aus dem Cache-Fenster | bleibt (nur bei Speichermangel oder vor dem Start der Tasks) |
| Hausarbeit (`bkl_hold`: Herunterfahren, Treiber laden/entladen, Beenden fremder Tasks) | die ganze Arbeit | bleibt bewusst: „alle anhalten“ |

Diskette: Controller und Spurpuffer gehören jetzt dem Task, der die
Sperre der Diskette hält (`kf_fd_lock`/`kf_fd_unlock` in kernel.c, Platz
`dl_owner[NDRV]`, seit 6.2 frei); alle Einsprünge von floppy.c (`fd_read`,
`fd_write`, `fd_sync`, `fd_check`, `fd_format`) nehmen sie, innen rufen
sie die Fassungen ohne Sperre (`*_i`). Solange ein Task sie hält, gibt
`proc_block` die Kernelsperre ab wie bei einem Treiberauftrag. Neu dabei:
Geändertes im Spurpuffer hat einen Besitzer (`tb_own`). Am Ende eines
FAPI-Aufrufs schreibt nur er es weg (`fd_sync_own`) – sonst schriebe jeder
Task, der gerade irgendeinen Aufruf beendet, fremde Daten und bekäme
deren Fehler (schreibgeschützt). Muss ein anderer den Puffer vorher
räumen (andere Spur, `blk_flush`) und scheitert das, merkt floppy.c den
Fehler (`tb_lost`) und meldet ihn dem Besitzer bei dessen `fd_sync_own`.

Geprüft: QEMU (XCOPY von A: und nach A: losgelöst, daneben XCOPY/COPY auf
C:, CHKDSK A:, Inhalte gleich, fsck beider sauber; mit einem Zähler über
Port E9h: vorher wartete ein Task auf die Kernelsperre, während der andere
in der Diskette schlief, jetzt nie – QEMUs Diskette antwortet aber fast
sofort) und 86Box 386DX/40 mit AHA-154x und Diskettenlaufwerk ohne
„Turbo“ (derselbe Ablauf, Inhalte gleich, fsck sauber).

### Auslagerungsdatei: bleibt vorerst unter `klock` (Befund)

Nachgesehen, ob eine eigene Sperre für den Puffer `sw_buf` reicht. Sie
reicht nicht – an `klock` hängt dort mehr:

- swap.c schützt damit die Seitentabellen **aller** Boxen, den Kopf der
  Auslagerungsdatei (Uhrzeiger, Bitmap) und den Puffer über das ganze
  Aus-/Einlagern; wer eine Seite braucht, die gerade unterwegs ist, wartet
  auf `klock` und liest danach.
- codeswap.c (Code nachladen, Segmente verschieben und auslagern) braucht
  `klock` ohnehin für den Kontext von fat.c (die EXE wird gelesen) und
  ruft dabei das Seiten-E/A von swap.c.
- Wer eine Box aufbaut (`v86_create`, `load_img`), holt Seiten und füllt
  sie danach über ein Fenster. Gäbe `kf_swap_rw` die Sperre ab, könnte ein
  anderer Task in dieser Zeit genau solche Seiten auslagern.

Ein Umbau hieße: eine Sperre für swap.c **und** codeswap.c, jede Seite,
die ein Erbauer noch in der Hand hat, als „nicht auslagerbar“ markieren,
und alle Aufrufer von `mem_get`/`vp_alloc` darauf prüfen, dass sie einen
neuen Schlafpunkt mit Abgabe vertragen. Der Gewinn: während eine Seite
(4 KB) gelesen oder geschrieben wird, könnten andere in den Kernel –
spürbar nur auf Rechnern unter Speicherdruck. Zurückgestellt, bis es
jemand misst; für Threads und mehr Tasks ist es nicht nötig.

### Durchsicht der Aufrufe mit `klock` (28.11.2026)

Regel für `fapi_nolock`: ein Aufruf darf ohne Sperre laufen, wenn er
**nie schläft** und **keinen Puffer des Kontexts** anfasst (`fi_buf`,
`fi_canon`, `fi_de`, `fi_u`, Zustand von fat.c/Redirector/DEM) – dann ist
er zwischen zwei Schlafpunkten unteilbar und stört keinen Task, der
mitten in einer Operation steht. Oder er schläft nur an Stellen mit
eigenem Schutz und hält nichts im Kontext (Konsole, Pipes, Netz, Ton,
Semaphoren, Named Pipes). Jeder Aufruf mit Sperre kostet sonst, sobald
ein anderer Task mitten in einer Operation steht, zwei Kopien des
Kontexts (einige KB).

| Tabelle | Aufrufe | Befund |
| --- | --- | --- |
| Dateitabelle/Handles | QHANDTYPE, SEEK, DUP | schlafen nie, kein Kontext: **jetzt ohne Sperre** (auch aus der Box über `box_fapi`) |
| | CLOSE, DUP2 | `of_release` -> `ifs_close` schläft und braucht fat.c: mit Marke |
| | OPEN, CREATE, READ/WRITE auf Dateien, NEWSIZE, FINDENTRY, DELETE, MKDIR, RMDIR, MOVE, SETFINFO, LABEL, FREESPACE, QFSINFO, CHDIR, QCURDIR, SELECTDISK | Dateisystem: mit Marke, geschützt durch die Laufwerkssperre |
| Pipes | READ/WRITE auf Pipes | schon ohne Sperre |
| | PIPE | benutzt `fi_u.hh` (zwei Worte) – bleibt, selten |
| Anzeige | CACHEINFO | **jetzt ohne Sperre**; SYSINFO, BUSINFO, QJOIN, QSUBST, SETTITLE, KLOG benutzen `fi_buf` oder Fenster – bleiben, selten |
| Taskverwaltung | EXECPGM, EXIT, PROC (KILL), SHUTDOWN, DRIVER, LOOP, MODULE | laden Dateien bzw. beenden fremde Tasks: mit Marke, EXIT/KILL nur außerhalb einer Operation (`in_op`) |
| Speicher | ALLOCSEG, FREESEG, SHRSEG, SEGCTL | `mem_get` kann auslagern (s. o.); wer ohne Marke im Kernel steht, gilt nicht als „mitten in einer Operation“ und darf verschoben werden (`seg_compact`) – bleibt mit Marke |
| Platten roh | OPENDRIVE, OPENDISK, FMTTRACK, EJECT, DISKINFO | mit Marke, Laufwerks-/Gerätesperre |

Nebenbei: `fapi_kcall` (Aufruf aus dem Kernel für die Box, Puffer mit
RPL 0) blieb gesetzt, während der Aufruf im Treiber schlief – ein
anderer Task bekam dann ebenfalls keinen RPL 3 auf seinen Selektor. Nicht
ausnutzbar (der Selektor ist sein eigenes DS), aber falsch; `fapi_inner`
nimmt den Merker jetzt beim Eintritt zurück.

Damit ist der Abbau so weit, wie er im 16-Bit-Kernel sinnvoll geht:
`klock` ist die Kontextmarke. Auf sie gewartet wird nur noch, während
ein anderer Task ein- oder auslagert, ohne eigene Ablage aus dem Cache
schreibt oder die Hausarbeit (`bkl_hold`) läuft. Ganz ohne sie ginge es
erst, wenn der Zustand je Aufruf nicht mehr in DGROUP steht (eigenes
Segment je Task und ferne Zeiger in fat.c – im 16-Bit-Kernel zu teuer;
so steht es für 7.0 in der Tabelle oben).

Was Stufe 5/6 (mehr Tasks, Threads) daraus braucht:

- Kontextablage und DMABUF je **Thread** (wie heute je Task) – jeder
  Thread kann mitten in einer Operation schlafen.
- Handles je **Prozess**: `task.std[]` und der Besitzer in `files[].owner`
  zeigen auf den Prozess; `of_release` beim Prozessende, nicht beim
  Threadende. Zwei Threads auf einem Handle verhalten sich wie heute zwei
  Tasks mit geerbtem Handle (Stelle erst nach der Laufwerkssperre lesen).
- `in_op` je Thread; `seg_compact`/`seg_out` verschieben Segmente eines
  Prozesses nur, wenn **keiner** seiner Threads im Kernel steht.
- LDT je Prozess: `seg_alloc`/`seg_free` bleiben mit Marke (kein Thread
  sieht eine halb eingetragene Zeile, weil dazwischen niemand schläft –
  außer in `mem_get`; dort Eintrag erst nach dem Holen des Speichers
  schreiben, wie heute).

## Start des Taskwechsels (vorgemerkt 24.11.2026)

Der Startpfad in `kmain` läuft ohne Tasks (`current = -1`): `klock_enter`
kehrt sofort zurück, `irq0` ruft `preempt` nur aus Ring 3/V86, Treiber,
Tastatur, CONFIG.SYS, Laufwerke, Auslagerungsdatei und Codepage laufen
also allein. Der Taskwechsel beginnt erst mit `load_tr(TSS_K)` und dem
ersten `schedule_next()` am Ende von `kmain`. Diese Zeilen stehen seit
25.11.2026 in der kernelinternen Funktion `sched_start()` (kernel.c,
kein Systemaufruf, Verhalten unverändert; in QEMU geprüft). Sie ist der
eine Punkt, ab dem Sperren gelten; in 7.0 startet sie zusätzlich die
weiteren CPU-Kerne (vorher läuft nur der Startkern).

Für den F5-Notstart (docs/NOTSTART.md) heißt das: die BASEDEV=-Treiber
und der INT-13h-Rückfall laufen vor `sched_start()`, also ohne
Multitasking und ohne Sperren – der Rückfall (Real-Mode-Ausflug) muss
erst danach auf Sperren achten (Kernelsperre + Gerätesperre).

## Offene Punkte (Durchsicht 15.11.2026)

- Tiefe 2 geht nicht mit dem heutigen Kontexttausch (`ctx_take` tauscht
  nur zwischen Tasks – ein verschachtelter Aufruf desselben Tasks
  überschriebe seinen eigenen Stand). Deshalb zuerst „fat.c-Zustand je
  Operation“, dann Tiefe 2 – wie in der Tabelle.
- Die GUI (GUI-WIN16.md) braucht unabhängig davon ihre eigene Sperre für
  die Win16-Programme (Win16Mutex im GUI-Prozess).
- Gemeinsamer Fahrplan der Pläne: CLAUDE.md, „Fahrplan nach 6.0“.
