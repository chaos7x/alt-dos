/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Gemeinsame Hilfen fuer die Werkzeuge (util.c wird zu jedem Programm
 * gelinkt). Kein C-Laufzeitsystem - nur das, was die Werkzeuge brauchen.
 */
#ifndef UTIL_H
#define UTIL_H

char     up(char c);
unsigned slen(const char *s);
void     scopy(char *d, const char *s, unsigned max);   /* max inkl. 0 */
void     scat(char *d, const char *s, unsigned max);
int      same(const char *a, const char *b);            /* ohne Gross/klein */
void     putc(char c);
void     put_num(unsigned long v, int width, int dots); /* rechtsbuendig */
char    *skip(char *p);                                  /* Leerzeichen   */
char    *next_word(char *p);
char    *word(char *p, char *out, unsigned max);         /* -> Rest       */
int      has_wild(const char *s);
int      match(const char *name, const char *pat);       /* NAME.EXT, * ? */
char    *base_name(char *path);                          /* nach \ oder : */
void     join(char *out, const char *dir, const char *name, unsigned max);
int      is_dir(const char *p);
int      ask_yn(void);                                    /* J/N -> 1/0    */
int      copy_file(const char *from, const char *to);    /* 0 = ok; -1 Quelle, -2 Ziel,
                         -3 Lesen, -4 Schreiben, -5 Schliessen, -6 Quelle in Benutzung,
                         -7 Datentraeger voll, -8 abgebrochen (copy_abort) */
extern int (*copy_abort)(void);         /* != 0: zwischen den Bloecken fragen */
#define  XFER 4096

/* Verzeichnisbaum ablaufen (ohne Rekursion im C-Stack, App-Stack 2 KB).
   path: absoluter Pfad eines Verzeichnisses, Puffer FAPI_PATHLEN, wird
   unterwegs verlaengert. Der Rueckruf bekommt den vollen Pfad des
   Eintrags; e == 0 und what == WT_LEAVE: Verzeichnis path ist fertig.
   Rueckgabe des Rueckrufs: WT_DESCEND (in ein Verzeichnis hinein),
   WT_REMOVED (Eintrag ist weg, Zaehlung nicht weiterruecken), WT_STOP. */
#define WT_FILE     0
#define WT_DIR      1
#define WT_LEAVE    2
#define WT_DESCEND  1
#define WT_REMOVED  2
#define WT_STOP     4
typedef int (*walk_fn)(char *path, struct fapi_dirent *e, int what, int depth);
int      walk_tree(char *path, walk_fn fn);              /* WT_STOP -> 1 */
int      abs_path(const char *in, char *out);            /* -> 0 = ok   */

#endif
