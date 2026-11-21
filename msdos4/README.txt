COMMAND.COM 4.0 fuer die DOS-Boxen von AT-DOS
=============================================

COMMAND4.COM ist COMMAND.COM aus den Quellen von MS-DOS 4.0, die Microsoft
2024 unter der MIT-Lizenz veroeffentlicht hat (siehe LICENSE, in der Box
COMMAND4.LIC):

  https://github.com/microsoft/ms-dos, Stand 2d04cac, v4.0/src/CMD/COMMAND
  gebaut mit den Werkzeugen aus dem Quellbaum (NMAKE, MASM, LINK, BUILDMSG)
  in dosbox-x

Geaendert (AT-DOS, 21.11.2026): patches/version.patch - die Versionspruefung
in INC/MSGSERV.ASM verlangte genau DOS 4.00, jetzt 4.0 oder neuer. Die Box
meldet sich als 6.22.
Geaendert (AT-DOS, 21.11.2026): patches/german.patch - deutsche Meldungen,
eigene Uebersetzung (MESSAGES/USA-MS.MSG: Abschnitte COMMON, EXTEND, PARSE
und COMMAND, in CP437), dazu die Antwortbuchstaben im residenten Teil
(CMD/COMMAND/RDATA.ASM: (W)iederholen statt Retry, J statt Y).

Neu bauen: tools/msdos4.sh (holt die Quellen per git, braucht dosbox-x).

In der Box liegt es als \DOS\MDOS\COMMAND4.COM (im PATH der Box): COMMAND4
startet es als zweite Shell, EXIT kehrt zurueck (aus CMD:
C:\DOS\MDOS\COMMAND4). Die erste Shell der Box
bleibt FreeCOM (COMMAND.COM).
