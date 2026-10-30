/*
 * DOS 5.0 - Treiber fuer den Diskettencontroller (NEC 765 / Intel 82077)
 *
 * Laufwerk A:, 1,44 MB. Daten laufen per ISA-DMA (Kanal 2) in einen festen
 * Puffer bei 8000h, das Ende eines Befehls meldet der Controller ueber IRQ 6.
 * Im Protected Mode gibt es kein BIOS mehr - INT 13h ist Geschichte.
 *
 * Gewartet wird noch synchron (HLT bis zum Interrupt). Andere Tasks laufen
 * waehrenddessen nicht; fuer kooperatives DOS 5.0 vertretbar.
 */
#include "kern.h"

#define DOR     0x3F2           /* Digital Output Register             */
#define MSR     0x3F4           /* Main Status Register                */
#define FIFO    0x3F5
#define CCR     0x3F7           /* Datenrate                           */

#define SPT     18
#define HEADS   2

#define DOR_RUN 0x0C            /* nicht Reset, DMA/IRQ an, Laufwerk 0 */
#define DOR_MOT 0x10            /* Motor A:                            */

static int motor_on;

static int fdc_out(unsigned char b)
{
    unsigned i;
    for (i = 0; i < 60000U; i++) {
        if ((inb(MSR) & 0xC0) == 0x80) {        /* bereit, Richtung CPU->FDC */
            outb(FIFO, b);
            return 0;
        }
    }
    return -1;
}

static int fdc_in(void)
{
    unsigned i;
    for (i = 0; i < 60000U; i++) {
        if ((inb(MSR) & 0xD0) == 0xD0)          /* bereit, FDC->CPU, belegt */
            return inb(FIFO);
    }
    return -1;
}

static int wait_irq(unsigned timeout)
{
    unsigned long until = get_ticks() + timeout;
    while (!fdc_irq) {
        if (get_ticks() > until)
            return -1;
        idle_wait();
    }
    fdc_irq = 0;
    return 0;
}

static void sense_int(int *st0, int *cyl)
{
    fdc_out(0x08);
    *st0 = fdc_in();
    *cyl = fdc_in();
}

static void motor(void)
{
    if (!motor_on) {
        outb(DOR, DOR_RUN | DOR_MOT);
        delay_ticks(30);                        /* 300 ms anlaufen lassen */
        motor_on = 1;
    }
}

void fd_motor_off(void)
{
    outb(DOR, DOR_RUN);
    motor_on = 0;
}

static int recalibrate(void)
{
    int st0, cyl, n;
    motor();
    for (n = 0; n < 2; n++) {                   /* bis zu 80 Spuren: 2x */
        fdc_irq = 0;
        fdc_out(0x07);
        fdc_out(0x00);
        if (wait_irq(300))
            return -1;
        sense_int(&st0, &cyl);
        if (cyl == 0)
            return 0;
    }
    return -1;
}

int fd_init(void)
{
    int st0, cyl, i;

    fdc_irq = 0;
    outb(DOR, 0x00);                            /* Reset                  */
    outb(DOR, DOR_RUN);
    if (wait_irq(100))
        return -1;
    for (i = 0; i < 4; i++)                     /* je Laufwerk quittieren */
        sense_int(&st0, &cyl);

    outb(CCR, 0x00);                            /* 500 kbit/s (1,44 MB)   */
    fdc_out(0x03);                              /* SPECIFY                */
    fdc_out(0xDF);                              /* SRT 3 ms, HUT 240 ms   */
    fdc_out(0x02);                              /* HLT 4 ms, DMA-Modus    */

    motor_on = 0;
    return recalibrate();
}

static void dma_setup(void)
{
    unsigned long a = DMABUF_LIN;
    unsigned n = DMABUF_SIZE - 1;

    outb(0x0A, 0x06);                           /* Kanal 2 maskieren      */
    outb(0x0C, 0x00);                           /* Flip-Flop zuruecksetzen */
    outb(0x0B, 0x46);                           /* Einzel, Schreiben in RAM, Kanal 2 */
    outb(0x04, (unsigned)a & 0xFF);
    outb(0x04, ((unsigned)a >> 8) & 0xFF);
    outb(0x81, (unsigned)(a >> 16) & 0xFF);     /* Seitenregister         */
    outb(0x0C, 0x00);
    outb(0x05, n & 0xFF);
    outb(0x05, n >> 8);
    outb(0x0A, 0x02);                           /* Kanal 2 freigeben      */
}

static int read_once(unsigned cyl, unsigned head, unsigned sec)
{
    int st0, c, r[7], i;

    fdc_irq = 0;                                /* SEEK                   */
    fdc_out(0x0F);
    fdc_out((unsigned char)(head << 2));
    fdc_out((unsigned char)cyl);
    if (wait_irq(300))
        return -1;
    sense_int(&st0, &c);
    if (c != (int)cyl)
        return -1;

    dma_setup();
    fdc_irq = 0;
    fdc_out(0xE6);                              /* READ DATA, MT MFM SK   */
    fdc_out((unsigned char)(head << 2));
    fdc_out((unsigned char)cyl);
    fdc_out((unsigned char)head);
    fdc_out((unsigned char)sec);
    fdc_out(2);                                 /* 512 Bytes              */
    fdc_out(SPT);
    fdc_out(0x1B);                              /* GAP3                   */
    fdc_out(0xFF);
    if (wait_irq(300))
        return -1;
    for (i = 0; i < 7; i++)
        r[i] = fdc_in();
    return (r[0] & 0xC0) ? -1 : 0;
}

int fd_read(unsigned lba)
{
    unsigned cyl = lba / (SPT * HEADS);
    unsigned head = (lba / SPT) % HEADS;
    unsigned sec = lba % SPT + 1;
    int tries;

    motor();
    for (tries = 0; tries < 3; tries++) {
        if (read_once(cyl, head, sec) == 0)
            return 0;
        recalibrate();
    }
    return -1;
}
