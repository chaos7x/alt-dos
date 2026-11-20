/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - SSH-2-Client (apps/ssh.c) fuer SSH.EXE und SFTP.EXE
 *
 * Eigene Umsetzung nach RFC 4251-4254 (Architektur, Transport, Anmeldung,
 * Verbindung), RFC 8731 (curve25519-sha256), RFC 5647/OpenSSH PROTOCOL
 * (aes-gcm@openssh.com), RFC 4344 (aes-ctr), RFC 6668 (hmac-sha2-256),
 * RFC 8332 (rsa-sha2-*), RFC 5656 (ecdsa-sha2-nistp*), RFC 8709
 * (ssh-ed25519), OpenSSH PROTOCOL (etm-MACs, strenger Schluesseltausch
 * gegen "Terrapin"). Eine Verbindung, ein Kanal zur Zeit.
 */
#ifndef SSH_H
#define SSH_H

#define SSH_MAXDATA     8192            /* Kanal: groesstes Datenpaket   */

/* Ereignisse von ssh_poll */
#define SSH_EV_NONE     0
#define SSH_EV_EOF      1               /* Gegenseite sendet nichts mehr */
#define SSH_EV_CLOSE    2               /* Kanal geschlossen             */

extern unsigned ssh_fam;                /* 0, AF_INET, AF_INET6           */
extern int      ssh_exit;               /* exit-status, sonst -1          */
extern int      ssh_verbose;            /* -v: Verfahren anzeigen          */
/* empfangene Kanaldaten (ext = 1: Fehlerausgabe); Zeiger gilt nur im Aufruf */
extern void   (*ssh_data)(int ext, const unsigned char *d, unsigned n);

/* Verbinden, Schluesseltausch, Hostschluessel pruefen -> 0 ok (Fehler
   werden gemeldet) */
int  ssh_connect(const char *host, unsigned port);
/* Anmelden (Kennwort bzw. keyboard-interactive) -> 0 ok */
int  ssh_login(const char *user);
/* Sitzungskanal oeffnen -> 0 ok */
int  ssh_session(void);
/* Kanalanfrage (pty-req, shell, exec, subsystem ...), x = Daten dahinter;
   want: auf Antwort warten -> 0 angenommen */
int  ssh_request(const char *type, const unsigned char *x, unsigned n, int want);
/* Daten senden (wartet auf Fenster) -> 0 ok */
int  ssh_write(const unsigned char *d, unsigned n);
int  ssh_send_eof(void);
/* Pakete verarbeiten, hoechstens ms warten -> SSH_EV_*, < 0 Fehler */
int  ssh_poll(unsigned ms);
void ssh_disconnect(void);

/* Hilfen fuer Aufrufer (Big Endian) */
unsigned char *ssh_put32(unsigned char *p, unsigned long v);
unsigned char *ssh_putstr(unsigned char *p, const void *s, unsigned n);
unsigned long  ssh_get32(const unsigned char *p);
void ssh_out(const char *s);            /* auf den Bildschirm             */

#endif
