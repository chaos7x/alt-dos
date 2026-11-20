/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Kerberos v5 fuer Programme (KINIT, KLIST, SMBCLI)
 *
 * Eigene Umsetzung nach RFC 4120 (Nachrichten), RFC 3961/3962 (aes256-
 * cts-hmac-sha1-96, krbcrypt.c), RFC 4121 (GSS-Huelle des AP-REQ), nur
 * gelesen. Nur aes256 (Etype 18), KDC ueber TCP Port 88. Die Uhr kommt
 * aus der ersten Antwort des KDC (KRB-ERROR mit stime), damit Zeitzone
 * und Uhrzeit von AT-DOS keine Rolle spielen (krb_off).
 *
 * Ablage der Tickets (wie die Datei-Ablage von MIT): %KRB5CCNAME%, sonst
 * %TEMP%\KRB5CC.DAT - Kopf + Eintraege struct kcred. Wer die Datei lesen
 * kann, kann die Tickets benutzen (wie unter Unix /tmp/krb5cc_uid).
 */
#ifndef KRB5_H
#define KRB5_H
#include "krbcrypt.h"

#define KTKT    2560                    /* Ticket (mit PAC) hoechstens    */
#define KNCRED  4                       /* Eintraege in der Ablage        */

struct kcred {
    char realm[64];                     /* Realm des Clients              */
    char cname[64];                     /* Name des Clients               */
    char sname[96];                     /* "krbtgt/REALM", "cifs/host"    */
    u8   key[32];                       /* Sitzungsschluessel (aes256)    */
    u32  auth, end, renew;              /* Unix-Zeit UTC                  */
    u16  etype, tlen;
    u8   tkt[KTKT];                     /* Ticket roh (DER)               */
};

/* Kopf der Ablage, dahinter n Eintraege struct kcred (der erste ist das
   Ticket fuer krbtgt) */
struct kchdr {
    char magic[8];                      /* "ATKRB5" 0 1                   */
    long off;                           /* UTC = Ortszeit + off           */
    u8   kdc[16];                       /* Adresse des KDC (IPv6/abgeb.)  */
    u16  n;
};

extern long krb_off;                    /* UTC - Ortszeit (Sekunden)      */
extern u8   krb_kdc[16];

/* Arbeitspuffer (je KBUF Byte) stellt der Aufrufer: SMBCLI nimmt dafuer
   seine Sendepuffer, damit DGROUP reicht */
#define KBUF    4096
void krb_bufs(u8 *rq, u8 *rp);

u32  krb_now(void);                     /* Unix-Zeit UTC                  */
void krb_tstr(u32 t, char out[20]);     /* "20.11.2026 11:03:20" (Ortszeit) */

/* Anmeldung: Ticket fuer krbtgt/REALM. 0 = ok, sonst Meldung */
const char *krb_kinit(const char *user, const char *realm, const char *pw,
                      struct kcred *tgt);
/* Dienstticket svc/host (z. B. "cifs", "dc1.atdos.test") */
const char *krb_tgs(const struct kcred *tgt, const char *svc, const char *host,
                    struct kcred *out);
/* GSS-Token (RFC 4121: OID + 01 00 + AP-REQ) fuer den Dienst -> Laenge */
unsigned krb_gss(const struct kcred *c, u8 *out, unsigned max);

/* Ablage: krb_cc_open -> Zahl der Eintraege (stellt krb_off, krb_kdc)
   oder -1; krb_cc_get Eintrag i; krb_cc_find nach sname -> Index oder -1;
   krb_cc_init legt sie mit dem TGT neu an; krb_cc_put ersetzt den Eintrag
   mit gleichem sname oder haengt an (0 = ok) */
void krb_cc_path(char *p, unsigned max);
int  krb_cc_open(void);
int  krb_cc_get(unsigned i, struct kcred *c);
int  krb_cc_find(const char *sname, struct kcred *c);
int  krb_cc_init(const struct kcred *tgt);
int  krb_cc_put(const struct kcred *c);

#endif
