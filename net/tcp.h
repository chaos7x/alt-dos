/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TCPIP.SYS, Stufe 2b: TCP nach RFC 793/1122/5681/6298 (eingebunden in
 * tcpip.c hinter sock.h). Eigene Umsetzung; Nachschlagewerke RFCs, Comer
 * „Internetworking with TCP/IP II“, lwIP (BSD) nur gelesen.
 *
 * Je Verbindung ein Empfangsring (4 KB = Fenster) und ein Sendering
 * (4 KB) aus dem Pufferpool. Sendepuffer enthaelt die Daten ab tx_seq
 * (SYN/FIN belegen nur Folgenummern). Wiederholung Go-back-N ab snd_una,
 * RTO nach Jacobson/Karels in Takten, Langsamer Start und Ueberlast-
 * vermeidung (cwnd), schnelle Wiederholung nach 3 doppelten ACKs, Nagle,
 * verzoegertes ACK (200 ms bzw. jedes zweite Segment), TIME_WAIT 2 s.
 * Keine Fensterskalierung, kein SACK, keine Zeitstempel (Fenster 4 KB).
 * Die Firewall: Verbindungen von aussen nur mit /NOFW (Regeln folgen).
 */

#define TS_CLOSED   0
#define TS_LISTEN   1
#define TS_SYNSENT  2
#define TS_SYNRCVD  3
#define TS_EST      4
#define TS_FINW1    5
#define TS_FINW2    6
#define TS_CLOSEWT  7
#define TS_CLOSING  8
#define TS_LASTACK  9
#define TS_TIMEWT   10

#define TF_FIN  0x01
#define TF_SYN  0x02
#define TF_RST  0x04
#define TF_PSH  0x08
#define TF_ACK  0x10

#define SEQ_LT(a, b)    ((long)((a) - (b)) < 0)
#define SEQ_LE(a, b)    ((long)((a) - (b)) <= 0)
#define SEQ_GT(a, b)    ((long)((a) - (b)) > 0)
#define SEQ_GE(a, b)    ((long)((a) - (b)) >= 0)
#define G32(p)  ((unsigned long)G16(p) << 16 | G16((p) + 2))
#define P32(p, v)   (P16((p), (unsigned)((v) >> 16)), P16((p) + 2, (unsigned)(v)))

#define TMSS    1460                    /* eigene MSS (MTU 1500)           */
#define RTO_MIN 20                      /* 200 ms                          */
#define RTO_MAX 6000                    /* 60 s                            */

/* Startnummer wie RFC 6528: ISN = M + F(Adressen, Ports, Geheimnis),
   M = Takt in 4-us-Schritten (10 ms = 2500 = 2048+256+128+64+4, ohne
   Multiplikation). Das Geheimnis entsteht bei der ersten Verbindung aus
   dem gesammelten Zufall. */
#define HALFMAX 4                       /* halboffene Verbindungen, alle   */
static unsigned long syn_drop;          /* deshalb verworfene SYN          */
static unsigned long isn_key;

static unsigned long isn_new(struct sock *s)
{
    unsigned long h, t = now;
    unsigned k;
    if (!isn_key)
        isn_key = rnd32() | 1;
    h = isn_key ^ ((unsigned long)s->lport << 16 | s->rport);
    for (k = 12; k < 16; k++)
        h = rotl7(h ^ s->laddr[k]) + 0x7F4A7C15UL;
    for (k = 12; k < 16; k++)
        h = rotl7(h ^ s->raddr[k]) + 0x9E3779B9UL;
    return (t << 11) + (t << 8) + (t << 7) + (t << 6) + (t << 2) + h;
}

static unsigned rwnd(struct sock *s)    /* eigenes Fenster                 */
{
    return BLK - s->ru;
}

/* ein Segment senden: Daten ab Stelle doff im Sendering (relativ zu ts) */
static void tcp_seg(struct sock *s, unsigned char fl, unsigned long seq,
                    unsigned doff, unsigned dlen)
{
    unsigned char src[16], *h = txb + L4OFF(s->raddr);
    unsigned hl = fl & TF_SYN ? 24 : 20, cs, p, n;
    int i = route_any(s->raddr, src);
    if (i < 0)
        return;
    P16(h, s->lport);
    P16(h + 2, s->rport);
    P32(h + 4, seq);
    P32(h + 8, fl & TF_ACK ? s->rcv_nxt : 0UL);
    h[12] = (unsigned char)(hl << 2);
    h[13] = fl;
    P16(h + 14, rwnd(s));
    P16(h + 16, 0);
    P16(h + 18, 0);
    if (fl & TF_SYN) {
        h[20] = 2;
        h[21] = 4;
        P16(h + 22, is_v4(s->raddr) ? TMSS : TMSS - 20);  /* IPv6: 40 Byte Kopf */
    }
    if (dlen) {                         /* aus dem Ring, ggf. in zwei Teilen */
        p = (s->ts + doff) & (BLK - 1);
        n = BLK - p < dlen ? BLK - p : dlen;
        cpy(h + hl, BP(s->tb) + p, n);
        if (n < dlen)
            cpy(h + hl + n, BP(s->tb), dlen - n);
    }
    cs = l4sum(src, s->raddr, 6, h, hl + dlen);
    P16(h + 16, cs);
    if (fl & TF_ACK)
        s->ackp = 0, s->ack_t = 0;
    ip_send(i, src, s->raddr, 6, hl + dlen, 64);
}

/* RST ohne Socket (Firewall aus): Antwort auf ein fremdes Segment (rx_s) */
static void tcp_rst_to(int i, fp t, unsigned dlen)
{
    unsigned char *h = txb + L4OFF(rx_s), to[16], me[16];
    unsigned char fl = t[13];
    unsigned long seq = G32(t + 4), ack = G32(t + 8);
    if (fl & TF_RST)
        return;
    cpy(to, rx_s, 16);
    cpy(me, rx_d, 16);
    P16(h, G16(t + 2));
    P16(h + 2, G16(t));
    if (fl & TF_ACK) {
        P32(h + 4, ack);
        P32(h + 8, 0UL);
        h[13] = TF_RST;
    } else {
        P32(h + 4, 0UL);
        P32(h + 8, seq + dlen + (fl & TF_SYN ? 1 : 0) + (fl & TF_FIN ? 1 : 0));
        h[13] = TF_RST | TF_ACK;
    }
    h[12] = 5 << 4;
    P16(h + 14, 0);
    P16(h + 16, 0);
    P16(h + 18, 0);
    P16(h + 16, l4sum(me, to, 6, h, 20));
    ip_send(i, me, to, 6, 20, 64);
}

static void tcp_timer_on(struct sock *s)
{
    if (!s->tmr)
        s->tmr = now + s->trto;
}

/* senden, was Fenster und Nagle erlauben; force = Fensterprobe */
static void tcp_output(struct sock *s, int force)
{
    unsigned off, avail, inflight, wnd, n;
    unsigned char fl;

    if (s->st != TS_EST && s->st != TS_CLOSEWT && s->st != TS_FINW1 &&
        s->st != TS_CLOSING && s->st != TS_LASTACK)
        return;
    for (;;) {
        off = (unsigned)(s->snd_nxt - s->tx_seq);
        if (off > s->tq)
            off = s->tq;                /* FIN schon gesendet             */
        avail = s->tq - off;
        inflight = (unsigned)(s->snd_nxt - s->snd_una);
        wnd = s->snd_wnd < s->cwnd ? s->snd_wnd : s->cwnd;
        if (force && !wnd)
            wnd = 1;
        if (avail) {
            n = avail < s->mss ? avail : s->mss;
            if (inflight + n > wnd)
                n = wnd > inflight ? wnd - inflight : 0;
            if (!n) {                   /* Fenster zu: Probe per Zeitgeber */
                tcp_timer_on(s);
                break;
            }
            if (n < s->mss && inflight && !s->finq && n == avail)
                break;                  /* Nagle: auf ACK warten           */
            fl = TF_ACK | (n == avail ? TF_PSH : 0);
            tcp_seg(s, fl, s->snd_nxt, off, n);
            if (!s->rtt_on) {
                s->rtt_on = 1;
                s->rtt_seq = s->snd_nxt;
                s->rtt_t = (unsigned)now;
            }
            s->snd_nxt += n;
            if (SEQ_GT(s->snd_nxt, s->snd_max))
                s->snd_max = s->snd_nxt;
            tcp_timer_on(s);
            force = 0;
            continue;
        }
        if (s->finq && !s->finsent) {
            tcp_seg(s, TF_FIN | TF_ACK, s->snd_nxt, 0, 0);
            s->snd_nxt++;
            if (SEQ_GT(s->snd_nxt, s->snd_max))
                s->snd_max = s->snd_nxt;
            s->finsent = 1;
            tcp_timer_on(s);
        }
        break;
    }
    if (s->ackp == 2)                   /* sofort faelliges ACK            */
        tcp_seg(s, TF_ACK, s->snd_nxt, 0, 0);
}

static void tcp_free_tx(struct sock *s)
{
    blk_put(s->tb);
    s->tb = -1;
}

/* TIME_WAIT ohne Handle braucht keine Puffer mehr (nur noch ACK auf ein
   wiederholtes FIN) - sonst halten kurz hintereinander geoeffnete
   Verbindungen (KINIT, DOMJOIN) 2 s lang alle 16 Bloecke fest */
static void tw_release(struct sock *s)
{
    if (!s->orphan || s->st != TS_TIMEWT)
        return;
    s->tmr = 0;
    blk_put(s->rb);
    s->rb = -1;
    blk_put(s->tb);
    s->tb = -1;
}

/* Verbindung zu Ende: Wartende wecken; ohne Handle ganz frei */
static void tcp_closed(int k, unsigned char err)
{
    struct sock *s = &sk[k];
    s->st = TS_CLOSED;
    if (err && !s->err)
        s->err = err;
    s->tmr = 0;
    if (s->parent >= 0) {               /* nie angenommen: ACCEPT nimmt es */
        int p = s->parent;              /* nur mit FIN, sonst bliebe es    */
        if (!s->rxfin)                  /* fuer immer belegt (Leck bei     */
            sk_free(k);                 /* gefaelschten SYN)               */
        wake(p);
        return;
    }
    if (s->orphan)
        sk_free(k);
    else
        wake(k);
}

static void rtt_sample(struct sock *s, unsigned m)
{
    int e;
    if (!m)
        m = 1;
    if (!s->srtt) {
        s->srtt = m << 3;
        s->rttv = m << 1;
    } else {
        e = (int)m - (int)(s->srtt >> 3);
        s->srtt += e;
        if (e < 0)
            e = -e;
        s->rttv += e - (s->rttv >> 2);
    }
    s->trto = (s->srtt >> 3) + s->rttv;
    if (s->trto < RTO_MIN)
        s->trto = RTO_MIN;
    if (s->trto > RTO_MAX)
        s->trto = RTO_MAX;
}

static int sock_new(void)
{
    int i, a, b;
    for (i = 0; i < NSOCK && sk[i].used; i++)
        ;
    if (i == NSOCK || (a = blk_get()) < 0)
        return -1;
    if ((b = blk_get()) < 0) {
        blk_put(a);
        return -1;
    }
    zero(&sk[i], sizeof(sk[i]));
    sk[i].used = 1;
    sk[i].type = SOCK_STREAM;
    sk[i].rb = (signed char)a;
    sk[i].tb = (signed char)b;
    sk[i].parent = -1;
    sk[i].trto = 300;                   /* 3 s bis zur ersten Messung      */
    sk[i].mss = 536;
    sk[i].cwnd = 2 * 536;
    sk[i].ssthresh = 0xFFFF;
    return i;
}

static unsigned opt_mss(fp t, unsigned hl)
{
    unsigned p = 20, k;
    while (p < hl) {
        k = t[p];
        if (k == 0)
            break;
        if (k == 1) {
            p++;
            continue;
        }
        if (p + 1 >= hl || t[p + 1] < 2)
            break;
        if (k == 2 && t[p + 1] == 4 && p + 4 <= hl)
            return G16(t + p + 2) < 64 ? 64 : G16(t + p + 2);  /* winzige MSS:
                                           Flut kleiner Segmente          */
        p += t[p + 1];
    }
    return 536;
}

/* TCP-Segment t (tl Byte), Adressen in rx_s/rx_d */
static void tcp_in(int i, fp t, unsigned tl)
{
    unsigned thl, dlen, sp, dp, k, n, room, w;
    unsigned long seq, ack;
    unsigned char fl, src[16];
    struct sock *s = 0;
    int si = -1, c;

    if (tl < 20)
        return;
    if (l4sum(rx_s, rx_d, 6, t, tl))
        return;
    thl = (t[12] >> 4) * 4;
    if (thl < 20 || thl > tl)
        return;
    sp = G16(t);
    dp = G16(t + 2);
    seq = G32(t + 4);
    ack = G32(t + 8);
    fl = t[13];
    w = G16(t + 14);
    dlen = tl - thl;
    cpy(src, rx_s, 16);

    for (k = 0; k < NSOCK; k++)         /* Verbindung                      */
        if (sk[k].used && sk[k].type == SOCK_STREAM && sk[k].st > TS_LISTEN &&
            sk[k].lport == dp && sk[k].rport == sp && eq(sk[k].raddr, src, 16)) {
            si = k;
            break;
        }
    if (si < 0 && (fl & (TF_SYN | TF_ACK | TF_RST)) == TF_SYN)
        for (k = 0; k < NSOCK; k++)     /* Listener                        */
            if (sk[k].used && sk[k].type == SOCK_STREAM && sk[k].st == TS_LISTEN &&
                sk[k].lport == dp && fam_ok(&sk[k], src)) {
                si = k;
                break;
            }
    if (si < 0) {                       /* niemand: Firewall schweigt      */
        if (fw_on)
            fw_drop++;
        else
            tcp_rst_to(i, t, dlen);
        return;
    }
    s = &sk[si];

    if (s->st == TS_LISTEN) {           /* neue Verbindung                 */
        if (!fw_allow(s->prog, FWP_TCP, dp, src, sp, i))
            return;                     /* Lauschen allein oeffnet nichts  */
        for (k = n = c = 0; k < NSOCK; k++) {
            if (sk[k].used && sk[k].parent == si)
                n++;
            if (sk[k].used && sk[k].type == SOCK_STREAM && sk[k].st == TS_SYNRCVD)
                c++;
        }
        if (c >= HALFMAX) {             /* halboffen ueber alle: SYN-Flut. */
            unsigned r = (unsigned)rnd32() % c;      /* Einen zufaellig    */
            syn_drop++;                 /* opfern (wie ein SYN-Cache), sonst */
            for (k = 0; k < NSOCK; k++) /* sperren vier gefaelschte SYN    */
                if (sk[k].used && sk[k].type == SOCK_STREAM &&   /* den Port */
                    sk[k].st == TS_SYNRCVD && !r--) {           /* minutenlang */
                    if (sk[k].parent == si)
                        n--;
                    sk_free(k);
                    break;
                }
        }
        if (n >= s->backlog || (c = sock_new()) < 0)
            return;                     /* voll: Gegenseite versucht es neu */
        s = &sk[c];
        s->fam = sk[si].fam;
        s->task = sk[si].task;
        cpy(s->prog, sk[si].prog, 13);
        s->lport = dp;
        cpy(s->laddr, sk[si].laddr, 16);
        cpy(s->raddr, src, 16);
        s->rport = sp;
        s->parent = (signed char)si;
        s->rcv_nxt = seq + 1;
        s->mss = opt_mss(t, thl);
        if (s->mss > (is_v4(src) ? TMSS : TMSS - 20))
            s->mss = is_v4(src) ? TMSS : TMSS - 20;
        s->cwnd = 2 * s->mss;
        s->snd_wnd = w;
        s->iss = isn_new(s);
        s->snd_una = s->iss;
        s->tx_seq = s->iss + 1;
        s->st = TS_SYNRCVD;
        tcp_seg(s, TF_SYN | TF_ACK, s->iss, 0, 0);
        s->snd_nxt = s->snd_max = s->iss + 1;
        tcp_timer_on(s);
        return;
    }

    if (s->st == TS_SYNSENT) {
        if ((fl & TF_ACK) && ack != s->iss + 1) {
            if (!(fl & TF_RST))
                tcp_rst_to(i, t, dlen);
            return;
        }
        if (fl & TF_RST) {
            if (fl & TF_ACK)
                tcp_closed(si, 1);      /* abgelehnt                       */
            return;
        }
        if (!(fl & TF_SYN) || !(fl & TF_ACK))
            return;
        s->rcv_nxt = seq + 1;
        s->snd_una = ack;
        s->snd_wnd = w;
        s->mss = opt_mss(t, thl);
        if (s->mss > (is_v4(src) ? TMSS : TMSS - 20))
            s->mss = is_v4(src) ? TMSS : TMSS - 20;
        s->cwnd = 2 * s->mss;
        s->st = TS_EST;
        s->tmr = 0;
        s->retries = 0;
        if (s->rtt_on) {
            rtt_sample(s, (unsigned)now - s->rtt_t);
            s->rtt_on = 0;
        }
        tcp_seg(s, TF_ACK, s->snd_nxt, 0, 0);
        wake(si);
        tcp_output(s, 0);
        return;
    }

    /* synchronisierte Zustaende */
    if (fl & TF_RST) {                  /* RFC 5961: nur genau an rcv_nxt */
        if (seq == s->rcv_nxt)
            tcp_closed(si, 1);
        else if (SEQ_GE(seq, s->rcv_nxt) && SEQ_LE(seq, s->rcv_nxt + rwnd(s)))
            tcp_seg(s, TF_ACK, s->snd_nxt, 0, 0);       /* Challenge-ACK   */
        return;
    }
    if (fl & TF_SYN) {                  /* SYN mitten drin: nur ACK        */
        tcp_seg(s, TF_ACK, s->snd_nxt, 0, 0);
        return;
    }
    if (!(fl & TF_ACK))
        return;
    if (SEQ_LT(seq, s->rcv_nxt)) {      /* schon Gesehenes vorn abschneiden */
        unsigned long skip = s->rcv_nxt - seq;
        if (skip >= dlen + ((fl & TF_FIN) ? 1 : 0)) {
            s->ackp = 2;                /* Wiederholung: ACK erneut        */
            tcp_output(s, 0);
            return;
        }
        t += (unsigned)skip;
        dlen -= (unsigned)skip;
        seq = s->rcv_nxt;
    }

    if (s->st == TS_SYNRCVD) {
        if (ack != s->iss + 1)
            return;
        s->snd_una = ack;
        s->snd_wnd = w;
        s->st = TS_EST;
        s->tmr = 0;
        s->retries = 0;
        if (s->parent >= 0)
            wake(s->parent);
    }

    /* ACK verarbeiten */
    if (SEQ_GT(ack, s->snd_una) && SEQ_LE(ack, s->snd_max)) {
        unsigned acked = (unsigned)(ack - s->snd_una);
        if (SEQ_GT(ack, s->tx_seq)) {
            unsigned d = (unsigned)(ack - s->tx_seq);
            if (d > s->tq)
                d = s->tq;              /* der Rest ist das FIN            */
            s->ts = (s->ts + d) & (BLK - 1);
            s->tq -= d;
            s->tx_seq += d;
        }
        s->snd_una = ack;
        if (SEQ_LT(s->snd_nxt, s->snd_una))
            s->snd_nxt = s->snd_una;
        if (s->rtt_on && SEQ_GE(ack, s->rtt_seq)) {
            rtt_sample(s, (unsigned)now - s->rtt_t);
            s->rtt_on = 0;
        }
        if (s->cwnd < s->ssthresh)      /* langsamer Start                 */
            s->cwnd += s->mss;
        else if ((s->ca_cnt += acked) >= s->cwnd) {
            s->ca_cnt -= s->cwnd;
            s->cwnd += s->mss;
        }
        if (s->cwnd > 32768u)
            s->cwnd = 32768u;
        s->retries = 0;
        s->dupacks = 0;
        s->tmr = s->snd_una == s->snd_max ? 0 : now + s->trto;
        wake(si);                       /* Platz im Sendepuffer            */
    } else if (ack == s->snd_una && !dlen && s->snd_max != s->snd_una &&
               w == s->snd_wnd && !(fl & TF_FIN)) {
        if (++s->dupacks == 3) {        /* schnelle Wiederholung           */
            n = (unsigned)(s->snd_max - s->snd_una) >> 1;
            s->ssthresh = n > 2 * s->mss ? n : 2 * s->mss;
            s->cwnd = s->ssthresh;
            s->snd_nxt = s->snd_una;
            s->finsent = 0;
            s->rtt_on = 0;
        }
    }
    if (SEQ_GE(ack, s->snd_una))
        s->snd_wnd = w;

    if (s->finsent && s->snd_una == s->snd_max) {   /* unser FIN ist da  */
        if (s->st == TS_FINW1)
            s->st = TS_FINW2, s->tw = s->orphan ? now + 6000 : 0;
        else if (s->st == TS_CLOSING)
            s->st = TS_TIMEWT, s->tw = now + 200, tw_release(s);
        else if (s->st == TS_LASTACK) {
            tcp_closed(si, 0);
            return;
        }
    }

    /* Daten und FIN */
    if ((dlen || (fl & TF_FIN)) &&
        (s->st == TS_EST || s->st == TS_FINW1 || s->st == TS_FINW2)) {
        if (seq != s->rcv_nxt) {        /* Luecke: ACK fuer das Erwartete  */
            s->ackp = 2;
        } else {
            room = rwnd(s);
            n = dlen < room ? dlen : room;
            if (s->orphan)
                n = dlen;               /* niemand liest mehr: verwerfen   */
            else if (n) {
                unsigned p = s->rh, m = BLK - p < n ? BLK - p : n;
                cpy(BP(s->rb) + p, t + thl, m);
                if (m < n)
                    cpy(BP(s->rb), t + thl + m, n - m);
                s->rh = (p + n) & (BLK - 1);
                s->ru += n;
            }
            s->rcv_nxt += n;
            if (n)
                s->ackp = s->ackp ? 2 : 1;  /* jedes zweite sofort        */
            if (!s->ack_t)
                s->ack_t = now + 20;
            if ((fl & TF_FIN) && n == dlen) {
                s->rcv_nxt++;
                s->rxfin = 1;
                s->ackp = 2;
                if (s->st == TS_EST)
                    s->st = TS_CLOSEWT;
                else if (s->st == TS_FINW1)
                    s->st = TS_CLOSING;
                else {
                    s->st = TS_TIMEWT;
                    s->tw = now + 200;
                    tw_release(s);
                }
            }
            if (n < dlen)
                s->ackp = 2;            /* Fenster voll                    */
            wake(si);
        }
    }
    tcp_output(s, 0);
}

/* je Takt: Wiederholung, verzoegertes ACK, Zeitgrenzen */
static void tcp_tick(void)
{
    int k;
    struct sock *s;
    unsigned n;
    for (k = 0; k < NSOCK; k++) {
        s = &sk[k];
        if (!s->used || s->type != SOCK_STREAM || s->st <= TS_LISTEN)
            continue;
        if ((s->st == TS_TIMEWT || s->st == TS_FINW2) && s->tw && now >= s->tw) {
            tcp_closed(k, 0);
            continue;
        }
        if (s->ackp && s->ack_t && now >= s->ack_t) {
            s->ack_t = 0;
            tcp_seg(s, TF_ACK, s->snd_nxt, 0, 0);
        }
        if (!s->tmr || now < s->tmr)
            continue;
        s->tmr = 0;
        if (++s->retries > (s->st == TS_SYNRCVD ? 3 : s->st < TS_EST ? 6 : 10)) {
            if (s->st >= TS_EST)
                tcp_seg(s, TF_RST | TF_ACK, s->snd_nxt, 0, 0);
            tcp_closed(k, 2);           /* Gegenseite antwortet nicht      */
            continue;
        }
        s->trto = s->trto < RTO_MAX / 2 ? s->trto << 1 : RTO_MAX;
        s->rtt_on = 0;
        if (s->st == TS_SYNSENT) {
            tcp_seg(s, TF_SYN, s->iss, 0, 0);
            tcp_timer_on(s);
            continue;
        }
        if (s->st == TS_SYNRCVD) {
            tcp_seg(s, TF_SYN | TF_ACK, s->iss, 0, 0);
            tcp_timer_on(s);
            continue;
        }
        n = (unsigned)(s->snd_max - s->snd_una) >> 1;
        s->ssthresh = n > 2 * s->mss ? n : 2 * s->mss;
        s->cwnd = s->mss;
        s->snd_nxt = s->snd_una;        /* Go-back-N                       */
        if (s->finsent && SEQ_LE(s->snd_nxt, s->tx_seq + s->tq))
            s->finsent = 0;
        tcp_output(s, 1);
        tcp_timer_on(s);
    }
}

/* ---- Aufrufe der Programme -------------------------------------------- */
static unsigned tcp_connect(int k, unsigned ms)
{
    struct sock *s = &sk[k];
    unsigned t0 = ticks(), r;
    if (s->st != TS_CLOSED)
        return FAPI_ERR_INUSE;
    {
        unsigned char src[16];
        if (route_any(s->raddr, src) < 0)
            return FAPI_ERR_NONET;
    }
    if (!s->lport && !(s->lport = port_new(SOCK_STREAM)))
        return FAPI_ERR_FULL;
    s->iss = isn_new(s);
    s->snd_una = s->iss;
    s->snd_nxt = s->snd_max = s->iss + 1;
    s->tx_seq = s->iss + 1;
    s->snd_wnd = 1;
    s->st = TS_SYNSENT;
    s->rtt_on = 1;
    s->rtt_t = (unsigned)ticks();
    tcp_seg(s, TF_SYN, s->iss, 0, 0);
    tcp_timer_on(s);
    if (s->flags & SF_NB)
        return FAPI_ERR_AGAIN;
    if (!ms)
        ms = 30000;
    for (;;) {
        if (s->st == TS_EST || s->st == TS_CLOSEWT)
            return 0;
        if (s->st == TS_CLOSED)
            return s->err == 2 ? FAPI_ERR_TIMEOUT : FAPI_ERR_RESET;
        cli();
        if (s->st == TS_SYNSENT) {
            if ((r = sk_wait(k, ms, t0)) != 0) {
                sti();
                s->st = TS_CLOSED;
                s->tmr = 0;
                return r;
            }
        } else
            sti();
    }
}

static unsigned tcp_accept(int k, unsigned ms, struct fapi_sockaddr __far *from)
{
    unsigned t0 = ticks(), r;
    int c;
    if (sk[k].st != TS_LISTEN)
        return FAPI_ERROR;
    if (!ms)
        ms = sk[k].rto;
    for (;;) {
        if (!sk[k].used || sk[k].st != TS_LISTEN)
            return FAPI_ERR_RESET;
        for (c = 0; c < NSOCK; c++)
            if (sk[c].used && sk[c].parent == k &&
                (sk[c].st >= TS_EST || (sk[c].st == TS_CLOSED && sk[c].rxfin)))
                break;
        if (c < NSOCK)
            break;
        if (sk[k].flags & SF_NB)
            return FAPI_ERR_AGAIN;
        cli();
        if ((r = sk_wait(k, ms, t0)) != 0) {
            sti();
            return r;
        }
    }
    sk[c].parent = -1;
    if (from)
        sa_put(from, sk[c].fam, sk[c].raddr, sk[c].rport);
    return c;
}

static unsigned tcp_send(int k, unsigned usel, unsigned buf, unsigned len)
{
    struct sock *s = &sk[k];
    const unsigned char __far *src;
    unsigned done = 0, n, p, m, t0 = ticks(), r;
    if (!len)
        return 0;
    if (!(src = uptr(usel, buf, len)))
        return FAPI_ERROR;
    while (done < len) {
        if (s->st != TS_EST && s->st != TS_CLOSEWT)
            return done ? done : (s->err == 2 ? FAPI_ERR_TIMEOUT : FAPI_ERR_RESET);
        if (s->finq || (s->flags & SF_NOWR))
            return done ? done : FAPI_ERR_RESET;
        n = BLK - s->tq;
        if (!n) {
            if (s->flags & SF_NB)
                return done ? done : FAPI_ERR_AGAIN;
            cli();
            if (s->tq == BLK) {
                if ((r = sk_wait(k, 0, t0)) != 0) {
                    sti();
                    return done ? done : r;
                }
            } else
                sti();
            continue;
        }
        if (n > len - done)
            n = len - done;
        p = (s->ts + s->tq) & (BLK - 1);
        m = BLK - p < n ? BLK - p : n;
        cpy(BP(s->tb) + p, src + done, m);
        if (m < n)
            cpy(BP(s->tb), src + done + m, n - m);
        s->tq += n;
        done += n;
        tcp_output(s, 0);
    }
    return done;
}

static unsigned tcp_recv(int k, unsigned usel, unsigned buf, unsigned len, unsigned flags,
                         unsigned ms)
{
    struct sock *s = &sk[k];
    unsigned char __far *dst;
    unsigned n, m, t0 = ticks(), r, old;
    if (!len)
        return 0;
    if (!(dst = uptr(usel, buf, len)))
        return FAPI_ERROR;
    if (!ms)
        ms = s->rto;
    for (;;) {
        if (s->ru)
            break;
        if (s->rxfin || (s->flags & SF_NORD))
            return 0;                   /* Gegenseite hat geschlossen      */
        if (s->st == TS_CLOSED || s->st == TS_SYNSENT || s->st == TS_LISTEN)
            return s->err == 2 ? FAPI_ERR_TIMEOUT : s->err ? FAPI_ERR_RESET : FAPI_ERROR;
        if (s->flags & SF_NB)
            return FAPI_ERR_AGAIN;
        cli();
        if (!s->ru && !s->rxfin && s->st != TS_CLOSED) {
            if ((r = sk_wait(k, ms, t0)) != 0) {
                sti();
                return r;
            }
        } else
            sti();
    }
    old = rwnd(s);
    n = s->ru < len ? s->ru : len;
    m = BLK - s->rt < n ? BLK - s->rt : n;
    cpy(dst, BP(s->rb) + s->rt, m);
    if (m < n)
        cpy(dst + m, BP(s->rb), n - m);
    if (!(flags & MSG_PEEK)) {
        s->rt = (s->rt + n) & (BLK - 1);
        s->ru -= n;
        if (old < s->mss && rwnd(s) >= s->mss && s->st >= TS_EST) {
            s->ackp = 2;                /* Fenster wieder offen melden     */
            tcp_output(s, 0);
        }
    }
    return n;
}

/* Handle geschlossen oder SHUTDOWN schreiben: FIN hinter die Daten */
static void tcp_fin(struct sock *s)
{
    if (s->st == TS_EST)
        s->st = TS_FINW1;
    else if (s->st == TS_CLOSEWT)
        s->st = TS_LASTACK;
    else
        return;
    s->finq = 1;
    tcp_output(s, 0);
}

static void tcp_close(int k)
{
    struct sock *s = &sk[k];
    int c;
    if (s->st == TS_LISTEN) {           /* nicht angenommene mit RST weg   */
        for (c = 0; c < NSOCK; c++)
            if (sk[c].used && sk[c].parent == k) {
                if (sk[c].st >= TS_SYNRCVD)
                    tcp_seg(&sk[c], TF_RST | TF_ACK, sk[c].snd_nxt, 0, 0);
                sk_free(c);
            }
        sk_free(k);
        return;
    }
    if (s->st == TS_CLOSED || s->st == TS_SYNSENT || s->st == TS_SYNRCVD) {
        if (s->st == TS_SYNRCVD)
            tcp_seg(s, TF_RST | TF_ACK, s->snd_nxt, 0, 0);
        sk_free(k);
        return;
    }
    s->orphan = 1;
    if (s->ru && (s->st == TS_EST || s->st == TS_CLOSEWT)) {
        tcp_seg(s, TF_RST | TF_ACK, s->snd_nxt, 0, 0);  /* Ungelesenes: Abbruch */
        sk_free(k);
        return;
    }
    if (s->st == TS_EST || s->st == TS_CLOSEWT)
        tcp_fin(s);
    if (s->st == TS_FINW2 || s->st == TS_FINW1)
        s->tw = now + 6000;
    tw_release(s);
}
