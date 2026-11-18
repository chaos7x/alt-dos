/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TCPIP.SYS - Firewall: Regeln je Programm (in tcpip.c eingebunden, hinter
 * same_net). Entwurf docs/NETZWERK.md, Abschnitt Firewall.
 *
 * Eingehend ist alles gesperrt ausser Antworten (TCP ueber die Verbindung,
 * UDP ueber die Gegenstellen, denen der Socket geschrieben hat) und dem,
 * was eine Regel erlaubt: Programm des lauschenden Sockets ("*" = jedes),
 * Protokoll, Ports, Herkunft. Lauschen allein oeffnet nichts. Ausgehend
 * ist alles frei. PING von aussen: eigenes Netz immer, sonst nur per
 * Regel (Programm "*", Protokoll ICMP). Abgewiesenes zaehlt fw_drop und
 * geht hoechstens einmal je Sekunde ins DMESG (DH_KLOG).
 */
#define NFWR    16

static struct fapi_fwrule fwr[NFWR];
static unsigned char fw_n;              /* Regeln belegt                   */
static unsigned char fw_log = 1;        /* Abgewiesenes ins DMESG          */
static unsigned long fw_drop, fw_pass;
static unsigned long fw_lsec = 0xFFFFFFFFUL;    /* letzte Meldung          */
static unsigned fw_quiet;               /* seitdem verschluckt             */

static int up(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static int prog_eq(const char *r, const char __far *p)
{
    unsigned k;
    if (r[0] == '*' && !r[1])
        return 1;
    for (k = 0; k < 13; k++) {
        if (up(r[k]) != up(p[k]))
            return 0;
        if (!r[k])
            return 1;
    }
    return 1;
}

/* IPv4-Absender a (4 Byte) im Netz der Regel? */
static int net_in(struct fapi_fwrule *r, const unsigned char __far *a, int i)
{
    unsigned b, k;
    if (r->flags & FWR_LOCAL)
        return i >= 0 && same_net(&nif[i], a);
    if (r->plen <= 96)
        return 1;                       /* /0: alle                        */
    b = r->plen - 96;
    for (k = 0; k < 4 && b; k++) {
        unsigned char m = b >= 8 ? 0xFF : (unsigned char)(0xFF << (8 - b));
        if ((a[k] & m) != (r->net[12 + k] & m))
            return 0;
        b = b >= 8 ? b - 8 : 0;
    }
    return 1;
}

static char fw_line[112];

static char *fw_dec(char *p, unsigned v)
{
    char t[6];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    return p;
}

static void fw_note(unsigned char proto, const unsigned char __far *a, unsigned sp,
                    unsigned dp, const char __far *prog)
{
    char *p = fw_line;
    unsigned k;
    fw_drop++;
    if (!fw_log)
        return;
    if (secs == fw_lsec) {              /* gedrosselt                      */
        fw_quiet++;
        return;
    }
    fw_lsec = secs;
    p = scat(p, "Firewall: ");
    p = scat(p, proto == FWP_TCP ? "TCP" : proto == FWP_UDP ? "UDP" : "ICMP");
    p = scat(p, " von ");
    for (k = 0; k < 4; k++) {
        p = fw_dec(p, a[k]);
        *p++ = k < 3 ? '.' : ':';
    }
    if (proto == FWP_ICMP)
        p--;
    else {
        p = fw_dec(p, sp);
        p = scat(p, " an Port ");
        p = fw_dec(p, dp);
    }
    if (prog && prog[0]) {
        p = scat(p, " (");
        for (k = 0; k < 12 && prog[k]; k++)
            *p++ = prog[k];
        *p++ = ')';
    }
    p = scat(p, " abgewiesen");
    if (fw_quiet) {
        p = scat(p, ", ");
        p = fw_dec(p, fw_quiet);
        p = scat(p, " weitere");
        fw_quiet = 0;
    }
    *p++ = '\n';
    devhlp(DH_KLOG, 0, (unsigned)fw_line, p - fw_line);
}

/* Eingehendes ohne Antwortbezug: erlaubt eine Regel es? Zaehlt und meldet,
   wenn nicht. prog = Programm des Sockets ("" ICMP/System). */
static int fw_allow(const char __far *prog, unsigned char proto, unsigned dp,
                    const unsigned char __far *a, unsigned sp, int i)
{
    unsigned k;
    struct fapi_fwrule *r;
    if (!fw_on)
        return 1;
    for (k = 0; k < fw_n; k++) {
        r = &fwr[k];
        if (r->proto && r->proto != proto)
            continue;
        if (proto != FWP_ICMP && (dp < r->lo || dp > r->hi))
            continue;
        if (!prog_eq(r->prog, prog) || !net_in(r, a, i))
            continue;
        r->hits++;
        fw_pass++;
        return 1;
    }
    fw_note(proto, a, sp, dp, prog);
    return 0;
}

static void __far *uptr(unsigned sel, unsigned off, unsigned n);

static unsigned fw_ctl(struct net_call __far *c)
{
    struct fapi_fwctl __far *u = uptr(c->usel, c->p, sizeof(struct fapi_fwctl));
    unsigned k;
    if (!u || c->n < sizeof(struct fapi_fwctl))
        return FAPI_ERROR;
    switch (u->op) {
    case FW_SET:
        fw_on = u->on ? 1 : 0;
        break;
    case FW_LOG:
        fw_log = u->on ? 1 : 0;
        break;
    case FW_ADD:
        if (fw_n == NFWR)
            return FAPI_ERR_FULL;
        cpy(&fwr[fw_n], &u->r, sizeof(struct fapi_fwrule));
        fwr[fw_n].prog[12] = 0;
        fwr[fw_n].hits = 0;
        return fw_n++;
    case FW_DEL:
        if (u->idx >= fw_n)
            return FAPI_ERROR;
        for (k = u->idx; k + 1 < fw_n; k++)
            cpy(&fwr[k], &fwr[k + 1], sizeof(struct fapi_fwrule));
        fw_n--;
        break;
    case FW_CLEAR:
        fw_n = 0;
        break;
    case FW_GET:
        break;
    default:
        return FAPI_ERROR;
    }
    u->on = fw_on;
    u->log = fw_log;
    u->drop = fw_drop;
    u->pass = fw_pass;
    u->nrules = fw_n;
    u->maxrules = NFWR;
    if (u->op == FW_GET && u->idx < fw_n)
        cpy(&u->r, &fwr[u->idx], sizeof(struct fapi_fwrule));
    return 0;
}
