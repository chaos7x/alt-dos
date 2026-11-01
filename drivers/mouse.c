/*
 * MOUSE.SYS - Maustreiber
 *
 * Wie MOUSE.COM von Microsoft sucht der Treiber selbst, was da ist. Diese
 * Fassung kennt die serielle Microsoft-Maus (und Logitech mit dritter
 * Taste) an COM1 und COM2; PS/2 und Busmaus folgen.
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

static unsigned base, irq;
static unsigned char nbtn;
static unsigned char pk[4], pn;         /* Paket im Aufbau              */
static unsigned char btn;               /* Tasten zuletzt (MOU_*)       */
static char line[128];

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

    (void)n;
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
    base = 0;
    for (i = 0; i < 2; i++) {
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
