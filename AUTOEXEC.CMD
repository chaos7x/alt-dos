@ECHO OFF
REM AT-DOS 5.0 - Startdatei des Befehlsinterpreters (CMD.EXE /P).
REM AUTOEXEC.BAT bleibt der DOS-Box vorbehalten (ab AT-DOS 6.0).
REM Weitere Programme in eigenen Sitzungen: START Programm
REM Temporaere Dateien: TEMP und TMP stellt der Kernel auf X:\TEMP
REM (Startlaufwerk, von Diskette C:); anderer Ort z. B.:
REM SET TEMP=D:\TEMP
REM SET TMP=D:\TEMP
REM DOSKEY-Makros, z. B.:
REM DOSKEY ll=DIR /W /O $*
REM DOSKEY /MACROFILE=C:\DOS\MAKROS.TXT
PROMPT $P$G
