/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AT-DOS 6.3 - LDAP-Client (apps/ldapc.c) fuer DOMJOIN und LOGON */
#ifndef LDAPC_H
#define LDAPC_H
#include "krb5.h"

#define LBUF    8192

/* X:\DOS\SYSTEM\DOMAIN.KEY (DOMJOIN schreibt, LOGON liest) */
struct djfile {
    char magic[8];                      /* "ATDOMJ1"                     */
    char realm[64], domain[64], name[16], dc[96], dn[192], pw[48];
    u8   key[32];                       /* aes256 des Rechners           */
    u32  joined;                        /* Unix-Zeit                     */
};

extern char base[160], dchost[96], diag[160];   /* aus rootdse/result  */
extern struct kcred tgt, svc;           /* TGT (vom Programm), ldap/<dc>  */
extern u8 sb[], tb[], rb[];             /* je LBUF + 64                   */
extern unsigned sh, msgid, sealed;
extern struct tlv creds;

void fail(const char *m, unsigned rc);  /* stellt das Programm            */
int  cr_cmp(const void *a, const void *b, unsigned n);
char *utoa(unsigned v, char *z);
void lower(char *d, const char *s, unsigned max);

void lconnect(void);                    /* TCP 389 zur Adresse krb_kdc    */
void lsend(unsigned n);                 /* sb[0..n)                       */
void lrecv(struct tlv *t);
void oct(const char *s);
void msg_beg(void);
unsigned msg_end(void);
u32  result(u8 op);
void ldap_fail(const char *was, u32 code);
void rootdse(void);                     /* -> base, dchost                */
void sasl_bind(void);                   /* GSSAPI, danach sealed          */
#endif
