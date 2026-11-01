/*
 * MOUSE.SYS - Maustreiber
 *
 * Wie MOUSE.COM von Microsoft sucht der Treiber selbst, was da ist: erst
 * eine PS/2-Maus am Hilfsport des Tastaturcontrollers (IRQ 12), dann die
 * serielle Microsoft-Maus (und Logitech mit dritter Taste) an COM1 und
 * COM2. Busmaus folgt. /PS2, /COM1, /COM2 legen fest, was gesucht wird.
 *
 * PS/2: Waehrend der Erkennung maskiert der Kernel die Tastatur (DevHlp
 * KbdLock), sonst holt deren Interruptroutine die Antworten des
 * Controllers aus Port 60h ab. Erst lesen: Status FFh = kein Controller;
 * dann "Hilfsport pruefen" (A9h, Antwort 00h = da), Maus zuruecksetzen
 * (FFh -> FAh AAh 00h), Vorgaben (F6h), Meldungen an (F4h) und im
 * Steuerbyte den Interrupt des Hilfsports einschalten. Pakete (3 Byte):
 *   Byte 1: Yueberl Xueberl Yvorz Xvorz 1 M R L   Byte 2: dx   Byte 3: dy
 * dy ist bei PS/2 positiv nach oben, der Kernel will es nach unten.
 *
 * Erkennung, erst lesen: Ein UART (8250/16450/16550) hat im Interrupt-
 * Identifikationsregister (Basis+2) die Bits 4-5 und im Modem-Steuer-
 * register (Basis+4) die Bits 5-7 immer 0; ein freier Port liefert FFh.
 * Erst dann wird die Schnittstelle auf 1200 Baud, 7N1 gestellt und die
 * Maus per DTR/RTS zurueckgesetzt - sie antwortet mit 'M' (Logitech:
 * "M3"). DEVICE=MOUSE.SYS /COM1 bzw. /COM2 prueft nur diesen Port.
 *
 * Datenformat (3 Byte, 7 Bit): das erste hat Bit 6 gesetzt.
 *   Byte 1: 1 L R Y7 Y6 X7 X6    Byte 2: X5..X0    Byte 3: Y5..Y0
 * Logitech haengt bei gedrueckter oder losgelassener mittlerer Taste ein
 * viertes Byte an (Bit 5 = Mitte). Bewegungen sind vorzeichenbehaftet,
 * Y positiv nach unten - wie der Kernel es erwartet (DevHlp MouEvent).
 */
#include "drvlib.h"
#include "../include/fapi_abi.h"     /* MOU_* */

#define RBR(b)  (b)                     /* Empfangspuffer (DLAB 0)      */
#define DLL(b)  (b)                     /* Teiler unten (DLAB 1)        */
#define IER(b)  ((b) + 1)
#define DLM(b)  ((b) + 1)
#define IIR(b)  ((b) + 2)
#define LCR(b)  ((b) + 3)
#define MCR(b)  ((b) + 4)
#define LSR(b)  ((b) + 5)

#define MCR_DTR  0x01
#define MCR_RTS  0x02
#define MCR_OUT2 0x08                   /* Interrupt zum PIC durchlassen */

static const unsigned com_port[2] = { 0x3F8, 0x2F8 };
static const unsigned com_irq[2]  = { 4, 3 };

#define KBC_DATA    0x60
#define KBC_STAT    0x64                /* lesen: Status, schreiben: Befehl */
#define KS_OBF      0x01                /* Byte fuer uns da             */
#define KS_IBF      0x02                /* Controller noch belegt       */
#define KS_AUX      0x20                /* Byte kommt vom Hilfsport     */

static unsigned base, irq;              /* base 0 und irq 12: PS/2      */
static unsigned char nbtn;
static unsigned char pk[4], pn;         /* Paket im Aufbau              */
static unsigned char btn;               /* Tasten zuletzt (MOU_*)       */
static char line[128];

/* ---- PS/2 ---------------------------------------------------------- */
static int kbc_wait_in(void)            /* bis der Controller annimmt   */
{
    unsigned start = ticks();
    while (inp(KBC_STAT) & KS_IBF)
        if ((unsigned)(ticks() - start) > 10)
            return -1;
    return 0;
}

static int kbc_cmd(unsigned char c)
{
    if (kbc_wait_in())
        return -1;
    outp(KBC_STAT, c);
    return 0;
}

static int kbc_data(unsigned char c)
{
    if (kbc_wait_in())
        return -1;
    outp(KBC_DATA, c);
    return 0;
}

/* naechstes Byte, t Takte warten; -1 = keins */
static int kbc_read(unsigned t)
{
    unsigned start = ticks();
    while (!(inp(KBC_STAT) & KS_OBF))
        if ((unsigned)(ticks() - start) > t)
            return -1;
    return inp(KBC_DATA);
}

/* Befehl an die Maus (D4h davor), Bestaetigung FAh abwarten */
static int aux_cmd(unsigned char c)
{
    if (kbc_cmd(0xD4) || kbc_data(c))
        return -1;
    return kbc_read(30) == 0xFA ? 0 : -1;
}

static unsigned ps2_probe(void)
{
    int cb, r;

    if (inp(KBC_STAT) == 0xFF)
        return 0;                       /* kein Controller              */
    while (inp(KBC_STAT) & KS_OBF)      /* Reste abholen                */
        inp(KBC_DATA);
    if (kbc_cmd(0x20) || (cb = kbc_read(10)) < 0)
        return 0;                       /* Steuerbyte lesen             */
    if (kbc_cmd(0xA9) || kbc_read(10) != 0x00)
        return 0;                       /* kein Hilfsport (AT-Controller) */
    if (kbc_cmd(0xA8))                  /* Hilfsport an                 */
        return 0;
    if (aux_cmd(0xFF) ||                /* Reset; Selbsttest bis 1 s    */
        kbc_read(100) != 0xAA) {
        kbc_cmd(0xA7);                  /* keine Maus: Hilfsport aus    */
        return 0;
    }
    kbc_read(10);                       /* Kennung 00h                  */
    if (aux_cmd(0xF6) || aux_cmd(0xF4)) {
        kbc_cmd(0xA7);
        return 0;
    }
    /* Steuerbyte: Interrupt des Hilfsports an (Bit 1), Hilfsport nicht
       gesperrt (Bit 5 aus) */
    if (kbc_cmd(0x60) || kbc_data((unsigned char)((cb | 0x02) & ~0x20)))
        return 0;
    r = 3;                              /* L, R und M im Protokoll      */
    return (unsigned)r;
}

static void ps2_irq(void)
{
    unsigned char st, c;
    int dx, dy;

    while (((st = inp(KBC_STAT)) & (KS_OBF | KS_AUX)) == (KS_OBF | KS_AUX)) {
        c = inp(KBC_DATA);
        if (pn == 0 && !(c & 0x08))
            continue;                   /* Bit 3 im ersten Byte immer 1 */
        pk[pn++] = c;
        if (pn < 3)
            continue;
        pn = 0;
        if (pk[0] & 0xC0)               /* Ueberlauf: Paket verwerfen   */
            continue;
        dx = pk[1] - ((pk[0] & 0x10) ? 256 : 0);
        dy = pk[2] - ((pk[0] & 0x20) ? 256 : 0);
        btn = (unsigned char)(pk[0] & 0x07);    /* L R M = MOU_*        */
        devhlp(DH_MOUEVENT, (unsigned)-dy, btn, (unsigned)dx);
    }
}

/* ---- seriell ------------------------------------------------------- */
/* Lesen: sieht der Port wie ein UART aus? */
static int looks_like_uart(unsigned b)
{
    unsigned char iir = inp(IIR(b)), mcr = inp(MCR(b));
    if (iir == 0xFF || mcr == 0xFF)
        return 0;
    return !(iir & 0x30) && !(mcr & 0xE0);
}

/* Schnittstelle einstellen, Maus zuruecksetzen; -> Tasten (2/3), 0 = keine */
static unsigned probe(unsigned b)
{
    unsigned start, got = 0, three = 0;
    unsigned char c;

    if (!looks_like_uart(b))
        return 0;
    outp(IER(b), 0x00);                 /* keine Interrupts             */
    outp(LCR(b), 0x80);                 /* DLAB                         */
    outp(DLL(b), 96);                   /* 115200 / 1200                */
    outp(DLM(b), 0);
    outp(LCR(b), 0x02);                 /* 7 Bit, keine Paritaet, 1 Stopp */
    outp(MCR(b), 0x00);                 /* DTR/RTS aus: Maus ohne Strom  */
    delay(15);
    while (inp(LSR(b)) & 1)             /* Reste wegwerfen              */
        inp(RBR(b));
    outp(MCR(b), MCR_DTR | MCR_RTS);    /* an: Maus meldet sich         */
    start = ticks();
    while ((unsigned)(ticks() - start) < 30) {
        if (inp(LSR(b)) & 1) {
            c = inp(RBR(b));
            if (c == 'M')
                got = 1;
            else if (got && c == '3')
                three = 1;
        }
    }
    if (!got)
        return 0;
    return three ? 3 : 2;
}

/* Interrupt: Bytes abholen, Pakete zusammensetzen */
static void mou_irq(unsigned n)
{
    unsigned char c, b;
    int dx, dy;

    if (n == 12) {
        ps2_irq();
        return;
    }
    while (inp(LSR(base)) & 1) {
        c = inp(RBR(base));
        if (c & 0x40) {                 /* Anfang eines Pakets          */
            pk[0] = c;
            pn = 1;
            continue;
        }
        if (pn == 0)
            continue;                   /* noch nicht synchron          */
        if (pn == 3) {                  /* Logitech: mittlere Taste     */
            b = (unsigned char)((btn & ~MOU_MIDDLE) | ((c & 0x20) ? MOU_MIDDLE : 0));
            if (b != btn) {
                btn = b;
                devhlp(DH_MOUEVENT, 0, btn, 0);
            }
            pn = 0;
            continue;
        }
        pk[pn++] = c;
        if (pn < 3)
            continue;
        dx = (signed char)(((pk[0] & 0x03) << 6) | (pk[1] & 0x3F));
        dy = (signed char)(((pk[0] & 0x0C) << 4) | (pk[2] & 0x3F));
        btn = (unsigned char)((btn & MOU_MIDDLE) |
                              ((pk[0] & 0x20) ? MOU_LEFT : 0) |
                              ((pk[0] & 0x10) ? MOU_RIGHT : 0));
        devhlp(DH_MOUEVENT, (unsigned)dy, btn, (unsigned)dx);
        if (nbtn < 3)
            pn = 0;                     /* kein viertes Byte erwartet   */
    }
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    int i, only = -1;
    char *p;

    r->units = 0;
    if (has_opt(r->args, "/COM1"))
        only = 0;
    else if (has_opt(r->args, "/COM2"))
        only = 1;
    else if (has_opt(r->args, "/PS2"))
        only = 2;
    base = 0;
    pn = 0;
    btn = 0;
    if (only < 0 || only == 2) {        /* zuerst PS/2                  */
        devhlp(DH_KBDLOCK, 0, 1, 0);
        nbtn = (unsigned char)ps2_probe();
        if (nbtn && set_irq(12, mou_irq) == 0) {
            devhlp(DH_KBDLOCK, 0, 2, 0);    /* Tastatur frei, AUX an    */
            dputs("  PS/2-Maus am Tastaturcontroller, IRQ 12\n");
            devhlp(DH_MOUREG, 0, nbtn, 0);
            irq = 12;
            r->status = DRV_OK;
            return;
        }
        if (nbtn) {                     /* IRQ 12 belegt: Hilfsport aus */
            kbc_cmd(0xA7);
            dputs("  MOUSE.SYS: PS/2-Maus gefunden, aber IRQ 12 belegt\n");
        }
        devhlp(DH_KBDLOCK, 0, 0, 0);
        nbtn = 0;
    }
    for (i = 0; i < 2 && only != 2; i++) {
        if (only >= 0 && i != only)
            continue;
        nbtn = (unsigned char)probe(com_port[i]);
        if (nbtn) {
            base = com_port[i];
            irq = com_irq[i];
            break;
        }
    }
    if (!base) {
        dputs("  MOUSE.SYS: keine Maus gefunden\n");
        r->status = DRV_ERR_NODEV;
        return;
    }
    p = scat(line, nbtn == 3 ? "  Serielle Maus (Logitech, 3 Tasten) an COM" :
                               "  Serielle Maus (Microsoft, 2 Tasten) an COM");
    *p++ = (char)('1' + i);
    p = scat(p, ", IRQ ");
    p = udec(p, irq);
    if (set_irq(irq, mou_irq)) {
        scat(p, " belegt\n");
        dputs(line);
        r->status = DRV_ERR_NODEV;
        return;
    }
    scat(p, "\n");
    dputs(line);
    pn = 0;
    btn = 0;
    while (inp(LSR(base)) & 1)          /* 'M' usw. wegwerfen           */
        inp(RBR(base));
    outp(MCR(base), MCR_DTR | MCR_RTS | MCR_OUT2);
    outp(IER(base), 0x01);              /* Interrupt bei empfangenem Byte */
    devhlp(DH_MOUREG, 0, nbtn, 0);
    r->status = DRV_OK;
}

void strategy(void)
{
    struct drv_req __far *r = REQ;

    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
