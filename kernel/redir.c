/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - Redirector der DOS-Boxen: AT-DOS-Laufwerke unter DOS
 *
 * Das DOS in der Box (FreeDOS, MKDOSIMG-Abbilder) haelt die Laufwerke ab
 * C: fuer Netzlaufwerke (BOXSHELL traegt sie in die CDS ein, Flags C000h)
 * und fragt fuer jeden Zugriff ueber INT 2Fh 11xxh nach - wie bei MSCDEX
 * oder einem Netzwerk-Redirector. BOXSHELL reicht den Aufruf mit INT E0h
 * 10h an den Monitor weiter.
 *
 * Seit Stufe 1 von docs/DOSKERN.md (21.11.2026) ist das hier nur noch der
 * Eingang ins DEM (kernel/dem.c, Vertrag include/dem.h): Daten aus SDA und
 * SFT in struct dem_req, dem_call, Ergebnis zurueck in SFT, SDA und
 * Register. Die Dateiarbeit selbst (Suche, SHARE, Lesen/Schreiben, ...)
 * steht im DEM, das spaeter auch der eigene Box-Kern ueber eine Falle
 * nimmt.
 *
 * Daten des DOS: SDA (INT 21h 5D06h, Aufbau wie MS-DOS 4+ - FreeDOS haelt
 * die Offsets ein): DTA 0Ch, Dateiname 1 9Eh, Dateiname 2 11Eh, Such-
 * zustand 19Eh (21 Byte), gefundener Eintrag 1B3h, Suchattribut 24Dh,
 * erweitertes Oeffnen 2DDh-2E2h. SFT (ES:DI): Modus 02h, Attribut 04h,
 * Flags 05h (8000h Netz | Laufwerk), Zeit/Datum 0Dh/0Fh, Groesse 11h,
 * Position 15h, Name 20h; das DEM-Handle steht in 0Bh (Handle des Kernels
 * + 1, geteilte Oeffnungen E000h | Platz).
 *
 * Laeuft im Kontext der Box unter der Kernelsperre (BOX_LOCK) - die
 * statischen Puffer stehen im Kontext je Task (redir_ctx).
 */
#include "k2.h"
#include "../include/fapi_abi.h"
#include "../include/dem.h"
#include "v86int.h"
#include "dembox.h"

int  __far kf_box_ctl(unsigned op, unsigned a);
unsigned long __far kf_cd_blocks(int d);
int __far kf_cd_read(int d, unsigned long lba, unsigned sel, unsigned off);
unsigned __far dem_call(int t, struct dem_req *q);
int __far dem_box_init(int t);
int __far dem_ready(void);
unsigned __far dem_xfer(int t, unsigned fn, unsigned h, unsigned long lin, unsigned n,
                        unsigned *err);
unsigned long __far dem_cd_size(int d);

#define AL  (*(unsigned char *)&f->eax)
#define AX  (*(unsigned short *)&f->eax)
#define BX  (*(unsigned short *)&f->ebx)
#define CX  (*(unsigned short *)&f->ecx)
#define DX  (*(unsigned short *)&f->edx)
#define DI  (*(unsigned short *)&f->edi)

static unsigned sda_seg[NTASK], sda_off[NTASK];
static struct dem_req rq;               /* Auftrag ans DEM               */

/* oem: Kennung aus INT 21h 30h (BH) - FFh MS-DOS/PC-DOS, FDh FreeDOS.
   MS-DOS laesst bei einem Redirector einiges dem Redirector (Aktions-
   code von 6Ch); FreeDOS erledigt es selbst. */
static unsigned char ms_dos[NTASK];
void __far redir_sda(int t, unsigned seg, unsigned off, unsigned oem)
{
    sda_seg[t] = seg;
    sda_off[t] = off;
    ms_dos[t] = oem != 0xFD;
    dem_box_init(t);                    /* neue Box: nichts offen         */
}

static unsigned char __far *sda(int t, unsigned x)
{
    return bp(t, sda_seg[t], sda_off[t] + x);
}

/* PSP des laufenden DOS-Programms der Box (SDA+10h, fuer DPMI) */
unsigned __far redir_psp(int t)
{
    unsigned char __far *p;
    if (!sda_seg[t])
        return 0;
    p = sda(t, 0x10);
    return p[0] | (p[1] << 8);
}

static unsigned up(unsigned c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* Dateiname aus der SDA (x = 9Eh/11Eh) -> d (128 Byte) */
static void get_fn(int t, unsigned x, char *d)
{
    unsigned char __far *s = sda(t, x);
    unsigned i, o = 0, k;
    for (i = 0; i < 127 && s[i]; i++) {
        if (s[i] == ' ') {              /* DOS denkt in 8+3-Feldern:      */
            for (k = i; k < 127 && s[k] == ' '; k++)   /* "LW .LLD" ist  */
                ;                       /* LW.LLD (Lotus-Works-INSTALL);  */
            if (k >= 127 || !s[k] || s[k] == '.' || s[k] == '\\') {
                i = k - 1;              /* Leerzeichen vor Punkt, Trenner */
                continue;               /* und Ende fallen weg             */
            }
        }
        d[o++] = s[i];
    }
    if (o > 3 && d[o - 1] == '\\')      /* MS-DOS fragt "C:\T3\" (1105h,  */
        o--;                            /* XCOPY) - wie "C:\T3"           */
    d[o] = 0;
    if (d[0] == '\\' && d[1] == '\\' && d[3] == '.' && d[4] == '\\' &&
        d[6] == '.') {                  /* CD-Laufwerk wie MSCDEX: Stamm   */
        d[1] = ':';                     /* "\\E.\A." -> "E:", Rest dahinter */
        d[0] = d[2];
        for (i = 7, k = 2; d[i]; )
            d[k++] = d[i++];
        if (k == 2)
            d[k++] = '\\';
        d[k] = 0;
    }
}

/* DEM-Handle <-> SFT+0Bh (Handle des Kernels + 1 bzw. E000h | Platz;
   0 = keiner) */
static unsigned long sft2dh(unsigned v) { return (v & 0xFF00) == 0xE000 ? v : (unsigned)(v - 1); }
static unsigned dh2sft(unsigned long h) { return ((unsigned)h & 0xFF00) == 0xE000 ? (unsigned)h : (unsigned)h + 1; }

/* SFT nach dem Oeffnen fuellen (Werte aus rq) */
static void sft_fill(int t, unsigned es, unsigned di, unsigned mode)
{
    unsigned char __far *s = bp(t, es, di);
    const char *n;
    unsigned k;
    w16(s + 0x02, (r16(s + 0x02) & 0x8000) | mode);  /* sf_isFCB (MS-DOS
                                           setzt es vor dem Oeffnen) bleibt */
    s[0x04] = (unsigned char)rq.attr;
    w16(s + 0x05, 0x8040 | (up(rq.path[0]) - 'A'));
    w32(s + 0x07, 0);
    w16(s + 0x0B, dh2sft(rq.handle));
    w16(s + 0x0D, rq.time);
    w16(s + 0x0F, rq.date);
    w32(s + 0x11, rq.size);
    w32(s + 0x15, 0);
    k = redir_psp(t);                   /* sf_PID = aktueller PSP: MS-DOS */
    s = bp(t, es, di);                  /* setzt es bei Netzdateien nicht, */
    w16(s + 0x31, k);                   /* prueft es aber bei FCBs        */
    for (n = rq.path, k = 0; rq.path[k]; k++)       /* Name in FCB-Form   */
        if (rq.path[k] == '\\')
            n = rq.path + k + 1;
    s = bp(t, es, di + 0x20);
    for (k = 0; k < 11; k++)
        s[k] = ' ';
    if (n[0] == '.') {
        s[0] = '.';
        if (n[1] == '.')
            s[1] = '.';
    } else {
        for (k = 0; *n && *n != '.'; n++)
            if (k < 8)
                s[k++] = (unsigned char)up(*n);
        if (*n == '.')
            n++;
        for (k = 8; *n; n++)
            if (k < 11)
                s[k++] = (unsigned char)up(*n);
    }
}

/* Standard-Handle h der Box (umgeleitet aus CMD, ATBOX.SYS-Geraete
   ATIN$/ATOUT$/ATERR$): h = 0 lesen, 1/2 schreiben -> Bytes */
unsigned __far redir_std(int t, unsigned h, unsigned seg, unsigned off, unsigned n)
{
    unsigned err, done = 0, r, idle = 0;
    unsigned long lin = ((unsigned long)seg << 4) + off;
    if (!h)                                 /* lesen: was da ist          */
        return dem_xfer(t, FAPI_READ, 0, lin, n, &err);
    while (done < n) {                      /* schreiben: alles (Pipe voll */
        r = dem_xfer(t, FAPI_WRITE, h, lin + done, n - done, &err);   /* -> warten) */
        done += r;
        if (err)
            break;
        if (r)
            idle = 0;
        else if (++idle > 500)              /* 5 s nichts: aufgeben       */
            break;
        else
            kf_box_ctl(BOX_SLEEP, 1);
    }
    return done;
}

/* gefundenen Eintrag (rq) nach SDA+1B3h */
static void found(int t)
{
    unsigned char __far *s = sda(t, 0x1B3);
    unsigned k;
    for (k = 0; k < 11; k++)
        s[k] = rq.name[k];
    s[11] = (unsigned char)rq.attr;
    for (k = 12; k < 22; k++)
        s[k] = 0;
    w16(s + 22, rq.time);
    w16(s + 24, rq.date);
    w16(s + 26, 0);
    w32(s + 28, rq.size);
}

/* Suchzustand zwischen Box (seg:off) und rq.find */
static void find_io(int t, unsigned seg, unsigned off, int to_box)
{
    unsigned char __far *s = bp(t, seg, off);
    unsigned k;
    for (k = 0; k < 21; k++)
        if (to_box)
            s[k] = rq.find[k];
        else
            rq.find[k] = s[k];
}

/* Aktionscode von 6Ch (1 geoeffnet, 2 angelegt, 3 ersetzt): FreeDOS
   nimmt ihn aus CX, MS-DOS gibt dem Aufrufer bei einem Redirector nur AX
   zurueck - dort steht er in dessen Registerrahmen (user_SS:user_SP aus
   der SDA, +264h bei MS-DOS 6.22, CX bei +4) */
static void ext_act(struct v86_frame *f, int t, unsigned act)
{
    unsigned sp, ss;
    CX = act;
    if (!ms_dos[t])
        return;
    sp = r16(sda(t, 0x264));
    ss = r16(sda(t, 0x266));
    w16(bp(t, ss, sp + 4), act);
    *sda(t, 0x2D6) = 0;                         /* EXTOPEN_ON: MS-DOS     */
}                                               /* loescht es nach dem    */
                                                /* Redirector selbst, PC  */
                                                /* DOS 6.3/2000 nicht und */
                                                /* traegt sonst 1 ein     */

static unsigned dem(int t, unsigned fn)
{
    rq.fn = fn;
    return dem_call(t, &rq);
}

static int op(struct v86_frame *f, int t, unsigned fn)
{
    unsigned char __far *s;
    unsigned es = (unsigned)f->r.es, di = DI, r, k, arg, cnt;

    /* Argument, das DOS vor INT 2Fh auf den Stack legte: [SP] AX von
       BOXSHELL, [SP+2] IP, CS, FLAGS, dann das Argument */
    arg = r16(bp(t, (unsigned)f->r.ss, (unsigned)f->r.esp + 8));

    switch (fn) {
    case 0x01:                                  /* RMDIR                 */
    case 0x03:                                  /* MKDIR                 */
        get_fn(t, 0x9E, rq.path);
        if (fn == 1) {                          /* aktuelles Verzeichnis */
            unsigned char __far *c;             /* des Laufwerks (CDS bei */
            s = sda(t, 0x282);                  /* SDA+282h): 10h wie DOS */
            c = bp(t, r16(s + 2), r16(s));
            for (k = 0; k < 67 && rq.path[k] && up(c[k]) == up(rq.path[k]); k++)
                ;
            if (k < 67 && !rq.path[k] && !c[k])
                return 0x10;
        }
        return dem(t, fn == 1 ? DEM_RMDIR : DEM_MKDIR);
    case 0x05:                                  /* CHDIR: gibt es das?   */
        get_fn(t, 0x9E, rq.path);
        return dem(t, DEM_CHDIR);
    case 0x06:                                  /* CLOSE                 */
        s = bp(t, es, di);
        cnt = r16(s);
        k = r16(s + 0x0B);
        if (cnt)
            w16(s, --cnt);
        if (cnt == 0 && k) {
            rq.handle = sft2dh(k);
            rq.mode = r16(s + 2);
            rq.flags = r16(s + 5) & 0x4000 ? DEM_F_TIME : 0;  /* 5701h   */
            rq.date = r16(s + 0x0F);
            rq.time = r16(s + 0x0D);
            dem(t, DEM_CLOSE);
            w16(bp(t, es, di + 0x0B), 0);
        }
        return 0;
    case 0x0A: {                                /* LOCK/UNLOCK: BL = 1    */
        unsigned char __far *pb;                /* entsperren, DS:DX ->   */
        rq.handle = sft2dh(r16(bp(t, es, di + 0x0B)));  /* Stelle, Laenge */
        pb = bp(t, (unsigned)f->r.ds, DX);
        rq.pos = r32(pb);
        rq.count = r32(pb + 4);
        rq.flags = *(unsigned char *)&f->ebx ? DEM_F_UNLOCK : 0;
        return dem(t, DEM_LOCK);
    }
    case 0x07:                                  /* COMMIT                */
    case 0x0B:                                  /* (FreeDOS: 0Ah + BL)   */
    case 0x1D: case 0x22:                       /* alle zu, Prozessende  */
        return 0;
    case 0x08:                                  /* READ                  */
    case 0x09:                                  /* WRITE                 */
        s = bp(t, es, di);
        rq.handle = sft2dh(r16(s + 0x0B));
        rq.pos = r32(s + 0x15);
        rq.size = r32(s + 0x11);
        rq.count = CX;
        s = sda(t, 0x0C);
        rq.buf = ((unsigned long)r16(s + 2) << 4) + r16(s);
        if ((r = dem(t, fn == 8 ? DEM_READ : DEM_WRITE)) != 0)
            return r;
        s = bp(t, es, di);
        w32(s + 0x15, rq.pos);
        w32(s + 0x11, rq.size);
        CX = (unsigned)rq.count;
        return 0;
    case 0x0C:                                  /* freier Platz          */
        s = bp(t, es, di);
        rq.drive = up(s[0] == '\\' ? s[2] : s[0]) - 'A';  /* CD: "\\X.\A." */
        if ((r = dem(t, DEM_SPACE)) != 0)
            return r;
        AX = (unsigned)rq.count;
        BX = (unsigned)rq.size;
        CX = rq.attr;
        DX = (unsigned)rq.pos;
        return 0;
    case 0x0E:                                  /* Attribut setzen       */
        get_fn(t, 0x9E, rq.path);
        rq.attr = arg;
        return dem(t, DEM_SETATTR);
    case 0x0F:                                  /* Attribut holen        */
        get_fn(t, 0x9E, rq.path);
        if ((r = dem(t, DEM_GETATTR)) != 0)
            return r;
        AX = rq.attr;
        BX = (unsigned)(rq.size >> 16);
        DI = (unsigned)rq.size;
        CX = rq.time;
        DX = rq.date;
        return 0;
    case 0x11:                                  /* RENAME                */
        get_fn(t, 0x9E, rq.path);
        get_fn(t, 0x11E, rq.path2);
        return dem(t, DEM_RENAME);
    case 0x13:                                  /* DELETE (auch *.*)     */
        get_fn(t, 0x9E, rq.path);
        return dem(t, DEM_DELETE);
    case 0x16:                                  /* OPEN                  */
    case 0x17:                                  /* CREATE (kuerzen)      */
    case 0x2E: {                                /* erweitert             */
        unsigned mode = arg & 0xFF;
        get_fn(t, 0x9E, rq.path);
        rq.attr = arg;
        if (fn == 0x2E) {
            rq.act = r16(sda(t, 0x2DD));
            mode = r16(sda(t, 0x2E1)) & 0xFF;
            rq.attr = r16(sda(t, 0x2DF));
        } else if (fn == 0x17)
            rq.act = arg & 0x100 ? DEM_ACT_NEW : DEM_ACT_CREATE;  /* 5Bh */
        else
            rq.act = DEM_ACT_OPEN;
        if (fn == 0x17)
            mode = 2;                           /* arg ist das Attribut;  */
        rq.mode = mode;                         /* DOS prueft den Modus   */
        if ((r = dem(t, DEM_OPEN)) != 0)
            return r;
        sft_fill(t, es, di, mode);
        if (fn == 0x2E)
            ext_act(f, t, rq.act);
        return 0;
    }
    case 0x1B:                                  /* FINDFIRST             */
        get_fn(t, 0x9E, rq.path);
        rq.attr = *sda(t, 0x24D);
        r = dem(t, DEM_FINDFIRST);
        find_io(t, sda_seg[t], sda_off[t] + 0x19E, 1);
        if (!r)
            found(t);
        return r;
    case 0x1C:                                  /* FINDNEXT (ES:DI)      */
        find_io(t, es, di, 0);
        r = dem(t, DEM_FINDNEXT);
        find_io(t, es, di, 1);
        if (!r)
            found(t);
        return r;
    case 0x21: {                                /* SEEK vom Ende         */
        unsigned long off = ((unsigned long)CX << 16) | DX, pos;
        s = bp(t, es, di);
        rq.handle = sft2dh(r16(s + 0x0B));
        rq.size = r32(s + 0x11);
        dem(t, DEM_SIZE);                       /* ein anderer SFT schrieb */
        pos = rq.size + off;
        w32(bp(t, es, di + 0x15), pos);
        DX = (unsigned)(pos >> 16);
        AX = (unsigned)pos;
        return 0;
    }
    }
    return -1;                                  /* nicht unser           */
}

int __far redir(struct v86_frame *f, int t)
{
    int r;
    if (!sda_seg[t] || !dem_ready())        /* (kein Kernelsegment frei) */
        return -1;
    kf_box_ctl(BOX_LOCK, 1);
    r = op(f, t, AL);
    kf_box_ctl(BOX_LOCK, 0);
    return r;
}

/* Auftrag des eigenen Box-Kerns ATDOSCM.SYS (INT E0h 30h, ES:DI = struct
   dem_req in der Box, docs/DOSKERN.md Stufe 2): herein, DEM, zurueck */
void __far redir_cm(struct v86_frame *f, int t)
{
    unsigned char __far *p;
    unsigned char *d = (unsigned char *)&rq;
    unsigned k;
    if (!sda_seg[t] || !dem_ready()) {
        p = bp(t, (unsigned)f->r.es, DI);
        w16(p + 2, E_DENIED);
        return;
    }
    kf_box_ctl(BOX_LOCK, 1);
    p = bp(t, (unsigned)f->r.es, DI);
    for (k = 0; k < sizeof(rq); k++)
        d[k] = p[k];
    dem_call(t, &rq);
    p = bp(t, (unsigned)f->r.es, DI);   /* (DEM kann das Fenster umlegen) */
    for (k = 0; k < sizeof(rq) - sizeof(rq.path) - sizeof(rq.path2); k++)
        p[k] = d[k];
    kf_box_ctl(BOX_LOCK, 0);
}

/* Laufwerksinfo fuer die Laufwerks-IOCTL von BOXSHELL (INT E0h 21h, AL =
   Laufwerk, 32 Byte nach ES:DI, Aufbau bei DEM_DINFO in dem.c) -> 0,
   sonst kein solches Laufwerk */
int __far redir_dinfo(struct v86_frame *f, int t)
{
    unsigned r;
    kf_box_ctl(BOX_LOCK, 1);
    rq.drive = AL;
    rq.buf = ((unsigned long)(unsigned)f->r.es << 4) + DI;
    r = dem(t, DEM_DINFO);
    kf_box_ctl(BOX_LOCK, 0);
    return r != 0;
}

/* Kontext je Task (Phase 6, kernel.c ctx_take) */
const struct ctxblk redir_ctx[] = {
    { &rq, sizeof(rq) }
};
const unsigned redir_nctx = sizeof(redir_ctx) / sizeof(redir_ctx[0]);


/* ---- MSCDEX (INT 2Fh 15xxh) ----------------------------------------------
   Viele CD-Spiele und Installationsprogramme fragen MSCDEX ab, bevor sie
   die CD ueber DOS lesen (Dateien gehen ohnehin ueber den Redirector).
   BOXSHELL reicht AH = 15h mit INT E0h 20h hierher (AL = Unterfunktion).
   Version 2.21 wie MSCDEX von MS-DOS 6.22; CD-Laufwerke sind alle AT-DOS-
   Laufwerke mit 2048-Byte-Bloecken (auch ISO-Loop). Gelesen wird direkt
   ueber das Blockgeraet (kf_cd_read). Keine CD-Audio-Wiedergabe: Abspiel-
   befehle melden "erledigt", Spuren gibt es nur als Datenspur.
   -> 0 erledigt, > 0 Fehler (CF, AX), -1 nicht zustaendig */
#define SI  (*(unsigned short *)&f->esi)
#define E_BADDRV 15

static int cd_first(unsigned *n)
{
    int d, first = -1;
    *n = 0;
    for (d = 0; d < 26; d++)
        if (kf_cd_blocks(d)) {
            if (first < 0)
                first = d;
            ++*n;
        }
    return first;
}

/* count Bloecke ab lba nach seg:off der Box; 0 = ok */
static int cd_rd(int t, int d, unsigned long lba, unsigned count, unsigned seg, unsigned off)
{
    unsigned long lin = ((unsigned long)seg << 4) + off;
    int r;
    while (count--) {
        bp(t, (unsigned)(lin >> 4), 0);
        if ((r = kf_cd_read(d, lba++, RWIN_SEL(t), (unsigned)lin & 15)) != 0)
            return r;
        lin += 2048;
    }
    return 0;
}

/* Geraeteauftrag (1510h) bei ES:BX fuer Laufwerk d */
static void cd_req(int t, int d, unsigned seg, unsigned off)
{
    unsigned char __far *q = bp(t, seg, off);
    unsigned st = 0x0100, cmd = q[2], ts, to, n;
    unsigned long lba, v = 0;
    unsigned char code;
    unsigned char __far *c;

    ts = r16(q + 0x10);                 /* Uebergabeadresse (fern)        */
    to = r16(q + 0x0E);
    if (cmd == 3) {                     /* IOCTL Eingabe                  */
        c = bp(t, ts, to);
        code = c[0];
        switch (code) {
        case 0: w32(c + 1, 0); break;               /* Kopf des Treibers */
        case 1: c[1] = 0; w32(c + 2, 0); break;     /* Lage des Kopfs    */
        case 4: for (n = 1; n < 9; n++) c[n] = (unsigned char)(n & 1 ? n / 2 : 0xFF); break;
        case 6: w32(c + 1, 0x00000202UL); break;    /* entriegelt, Red Book */
        case 7: c[1] = 0; w16(c + 2, 2048); break;  /* Sektorgroesse      */
        case 8: v = dem_cd_size(d); c = bp(t, ts, to); w32(c + 1, v); break;
        case 9: c[1] = 1; break;                    /* nicht gewechselt   */
        case 10: c[1] = 1; c[2] = 1;                /* eine (Daten-)Spur  */
            v = dem_cd_size(d) + 150;                   /* Ende als MSF       */
            c = bp(t, ts, to);
            for (n = 0; v >= 4500 && n < 99; n++)   /* ohne 32-Bit-Division */
                v -= 4500;
            c[5] = (unsigned char)n;
            c[4] = (unsigned char)((unsigned)v / 75);
            c[3] = (unsigned char)((unsigned)v % 75);
            c[6] = 0;
            break;
        case 11: c[2] = 2; c[3] = 0; c[4] = 0; c[5] = 0; c[6] = 0x40; break;  /* Spur 1: 00:02:00, Daten */
        case 12: case 15: for (n = 1; n < 11; n++) c[n] = 0; break;
        default: st = 0x8103;
        }
        q = bp(t, seg, off);
    } else if (cmd == 128) {            /* READ LONG                      */
        lba = r32(q + 0x14);
        if (q[0x0D] == 1)               /* Red Book (MSF) -> Block        */
        {
            unsigned m = (unsigned)(lba >> 16) & 0xFF;
            v = (unsigned)((lba >> 8) & 0xFF) * 75u + ((unsigned)lba & 0xFF);
            while (m--)                 /* ohne 32-Bit-Multiplikation    */
                v += 4500;
            lba = v - 150;
        }
        n = r16(q + 0x12);
        if (q[0x18])                    /* roh (2352 Byte) gibt es nicht  */
            st = 0x8103;
        else if ((v = (unsigned long)cd_rd(t, d, lba, n, ts, to)) != 0)
            st = v == 1 ? 0x8102 : 0x810F;
        q = bp(t, seg, off);
    } else if (cmd != 13 && cmd != 14 && cmd != 7 && cmd != 12 && cmd != 131 &&
               cmd != 132 && cmd != 133 && cmd != 136)
        st = 0x8103;                    /* unbekannter Befehl             */
    w16(q + 3, st);
}

int __far mscdex(struct v86_frame *f, int t)
{
    unsigned char sub = AL;
    unsigned n, i, k, seg = f->r.es;
    int d, r;
    unsigned char __far *p;
    unsigned char __far *s;

    if (sub != 0 && sub != 0x0B && sub != 0x0C && sub != 0x0D &&
        (CX >= 26 || !kf_cd_blocks(CX)))
        return sub <= 0x10 ? (AX = E_BADDRV, 1) : -1;
    kf_box_ctl(BOX_LOCK, 1);
    r = 0;
    switch (sub) {
    case 0x00:                          /* Installation: Zahl, erstes     */
        d = cd_first(&n);
        BX = n;
        CX = d < 0 ? 0 : (unsigned)d;
        break;
    case 0x01:                          /* Treiberliste: Untereinheit +   */
        for (d = 0, k = 0, i = 0xFF; d < 26; d++)    /* Geraetekopf      */
            if (kf_cd_blocks(d)) {
                if (i == 0xFF)
                    i = d;
                p = bp(t, seg, BX + k * 5);
                p[0] = (unsigned char)k;
                w32(p + 1, 0xF0000AE0UL);
                k++;
            }
        /* Kopf eines CD-Treibers bei F000:0AE0 (Stummelseite, hinter der
           XMS-Tabelle): Zeichengeraet C800h, "MSCD001", Laufwerk und
           Einheiten wie MSCDEX sie eintragen; Strategie/Interrupt = RETF.
           Vorher Nullzeiger - CheckIt las dort die Interrupttabelle und
           hielt die CD fuer ein Netzlaufwerk. */
        p = bp(t, 0xF000, 0x0AE0);
        w32(p, 0xFFFFFFFFUL);
        p[4] = 0x00; p[5] = 0xC8;
        p[6] = 0x02; p[7] = 0x05;       /* XMS-Stummel +2: CB (RETF)      */
        p[8] = 0x02; p[9] = 0x05;
        for (n = 0; n < 8; n++)
            p[10 + n] = (unsigned char)"MSCD001 "[n];
        p[18] = p[19] = 0;
        p[20] = (unsigned char)(i == 0xFF ? 0 : i + 1);
        p[21] = (unsigned char)k;
        break;
    case 0x02: case 0x03: case 0x04:    /* Dateinamen aus dem PVD         */
        if (kf_cd_read(CX, 16, 0, 0)) {
            AX = E_NOTREADY;
            r = 1;
            break;
        }
        s = MK_FP(DMABUF, 702 + (sub - 2) * 37);
        p = bp(t, seg, BX);
        for (i = 0; i < 37 && s[i] != ' ' && s[i]; i++)
            p[i] = s[i];
        p[i] = 0;
        break;
    case 0x05:                          /* VTOC lesen (DX = Index)        */
        if ((k = (unsigned)cd_rd(t, CX, 16UL + DX, 1, seg, BX)) != 0) {
            AX = k == 1 ? E_NOTREADY : 0x1F;
            r = 1;
        } else
            AX = bp(t, seg, BX)[0] == 1 ? 1 : bp(t, seg, BX)[0] == 0xFF ? 0xFF : 0;
        break;
    case 0x08:                          /* absolut lesen: SI:DI, DX Stueck */
        if ((k = (unsigned)cd_rd(t, CX, ((unsigned long)SI << 16) | DI, DX, seg, BX)) != 0) {
            AX = k == 1 ? E_NOTREADY : 0x1F;
            r = 1;
        }
        break;
    case 0x09:                          /* schreiben: nein                */
        AX = 0x13;                      /* schreibgeschuetzt              */
        r = 1;
        break;
    case 0x0B:                          /* Laufwerk CX ein CD?            */
        AX = CX < 26 && kf_cd_blocks(CX) ? 0x5AD8 : 0;
        BX = 0xADAD;
        break;
    case 0x0C:
        BX = 0x0215;                    /* Version 2.21                   */
        break;
    case 0x0D:                          /* Buchstaben (0 = A:) nach ES:BX */
        for (d = 0, k = 0; d < 26; d++)
            if (kf_cd_blocks(d))
                bp(t, seg, BX + k++)[0] = (unsigned char)d;
        break;
    case 0x0E: case 0x0F:               /* Vorgaben / Verzeichniseintrag   */
        AX = 1;                         /* nicht unterstuetzt              */
        r = 1;
        break;
    case 0x10:                          /* Geraeteauftrag ES:BX            */
        cd_req(t, CX, seg, BX);
        break;
    default:
        r = -1;
    }
    kf_box_ctl(BOX_LOCK, 0);
    return r;
}
