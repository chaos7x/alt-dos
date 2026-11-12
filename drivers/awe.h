/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * awe.h - EMU8000 (Sound Blaster AWE32/AWE64): Erkennung, Grundzustand,
 * Stimmen. Wird von sb16.c eingebunden (ein Treibersegment).
 *
 * Register: drei Datenports und ein Zeiger, je 16 Bit, nur als Wort
 * ansprechen (der Chip vertraegt keine Bytezugriffe):
 *   DATA0 = Basis, DATA1 = Basis + 400h, DATA2 = Basis + 402h,
 *   DATA3 = Basis + 800h, PTR = Basis + 802h (Register << 5 | Stimme).
 * 32-Bit-Register: erst das niedrige, dann das hohe Wort (Port + 2).
 *
 * Herkunft: Ablauf der Initialisierung und die Tabellen init1-init4
 * (urspruenglich aus Creatives AWE32 Developer Information Pack, ADIP)
 * aus Linux sound/isa/sb/emu8000.c (Copyright Jaroslav Kysela, Takashi
 * Iwai, Steve Ratcliffe; GPL-2.0-or-later), auf 16 Bit umgeschrieben.
 * Verhalten der Register zusaetzlich in 86Box src/sound/snd_emu8k.c
 * nachgelesen. Der Rest ist eigener Code.
 */

#define NAWEV   30              /* Stimmen fuer Musik (30, 31: DRAM-Refresh) */

static unsigned ebase;          /* 620h, 640h, 660h, 680h; 0 = keiner */
static unsigned edram_kb;       /* RAM fuer eigene Samples            */

void aw_outw(unsigned port, unsigned v);
#pragma aux aw_outw = "out dx,ax" __parm [__dx] [__ax];
unsigned aw_inw(unsigned port);
#pragma aux aw_inw = "in ax,dx" __parm [__dx] __value [__ax];

#define E_D0    (ebase)
#define E_D1    (ebase + 0x400)
#define E_D2    (ebase + 0x402)
#define E_D3    (ebase + 0x800)
#define E_PTR   (ebase + 0x802)
#define ECMD(r, c) (((r) << 5) | (c))

static void ew(unsigned port, unsigned reg, unsigned v)
{
    aw_outw(E_PTR, reg);
    aw_outw(port, v);
}

static void ewd(unsigned port, unsigned reg, unsigned long v)
{
    aw_outw(E_PTR, reg);
    aw_outw(port, (unsigned)v);
    aw_outw(port + 2, (unsigned)(v >> 16));
}

static unsigned er(unsigned port, unsigned reg)
{
    aw_outw(E_PTR, reg);
    return aw_inw(port);
}

static unsigned long erd(unsigned port, unsigned reg)
{
    unsigned lo;
    aw_outw(E_PTR, reg);
    lo = aw_inw(port);
    return lo | ((unsigned long)aw_inw(port + 2) << 16);
}

/* Register je Stimme */
#define CPF(c, v)       ewd(E_D0, ECMD(0, c), v)
#define PTRX(c, v)      ewd(E_D0, ECMD(1, c), v)
#define CVCF(c, v)      ewd(E_D0, ECMD(2, c), v)
#define VTFT(c, v)      ewd(E_D0, ECMD(3, c), v)
#define R0080(c, v)     ewd(E_D0, ECMD(4, c), v)
#define R00A0(c, v)     ewd(E_D0, ECMD(5, c), v)
#define PSST(c, v)      ewd(E_D0, ECMD(6, c), v)
#define CSL(c, v)       ewd(E_D0, ECMD(7, c), v)
#define CCCA(c, v)      ewd(E_D1, ECMD(0, c), v)
#define ENVVOL(c, v)    ew(E_D1, ECMD(4, c), v)
#define DCYSUSV(c, v)   ew(E_D1, ECMD(5, c), v)
#define ENVVAL(c, v)    ew(E_D1, ECMD(6, c), v)
#define DCYSUS(c, v)    ew(E_D1, ECMD(7, c), v)
#define ATKHLDV(c, v)   ew(E_D2, ECMD(4, c), v)
#define LFO1VAL(c, v)   ew(E_D2, ECMD(5, c), v)
#define ATKHLD(c, v)    ew(E_D2, ECMD(6, c), v)
#define LFO2VAL(c, v)   ew(E_D2, ECMD(7, c), v)
#define IP(c, v)        ew(E_D3, ECMD(0, c), v)
#define IFATN(c, v)     ew(E_D3, ECMD(1, c), v)
#define PEFE(c, v)      ew(E_D3, ECMD(2, c), v)
#define FMMOD(c, v)     ew(E_D3, ECMD(3, c), v)
#define TREMFRQ(c, v)   ew(E_D3, ECMD(4, c), v)
#define FM2FRQ2(c, v)   ew(E_D3, ECMD(5, c), v)
/* Steuerregister (DATA1, Register 1) */
#define HWCF1           ECMD(1, 29)
#define HWCF2           ECMD(1, 30)
#define HWCF3           ECMD(1, 31)
#define HWCF4           ECMD(1, 9)
#define HWCF5           ECMD(1, 10)
#define HWCF6           ECMD(1, 13)
#define SMALR           ECMD(1, 20)
#define SMARR           ECMD(1, 21)
#define SMALW           ECMD(1, 22)
#define SMARW           ECMD(1, 23)
#define SMLD            ECMD(1, 26)

#define DRAM_OFS        0x200000UL      /* Wortadresse des Karten-RAM */

static const unsigned short init1[128] = {
    0x03ff, 0x0030, 0x07ff, 0x0130, 0x0bff, 0x0230, 0x0fff, 0x0330,
    0x13ff, 0x0430, 0x17ff, 0x0530, 0x1bff, 0x0630, 0x1fff, 0x0730,
    0x23ff, 0x0830, 0x27ff, 0x0930, 0x2bff, 0x0a30, 0x2fff, 0x0b30,
    0x33ff, 0x0c30, 0x37ff, 0x0d30, 0x3bff, 0x0e30, 0x3fff, 0x0f30,
    0x43ff, 0x0030, 0x47ff, 0x0130, 0x4bff, 0x0230, 0x4fff, 0x0330,
    0x53ff, 0x0430, 0x57ff, 0x0530, 0x5bff, 0x0630, 0x5fff, 0x0730,
    0x63ff, 0x0830, 0x67ff, 0x0930, 0x6bff, 0x0a30, 0x6fff, 0x0b30,
    0x73ff, 0x0c30, 0x77ff, 0x0d30, 0x7bff, 0x0e30, 0x7fff, 0x0f30,
    0x83ff, 0x0030, 0x87ff, 0x0130, 0x8bff, 0x0230, 0x8fff, 0x0330,
    0x93ff, 0x0430, 0x97ff, 0x0530, 0x9bff, 0x0630, 0x9fff, 0x0730,
    0xa3ff, 0x0830, 0xa7ff, 0x0930, 0xabff, 0x0a30, 0xafff, 0x0b30,
    0xb3ff, 0x0c30, 0xb7ff, 0x0d30, 0xbbff, 0x0e30, 0xbfff, 0x0f30,
    0xc3ff, 0x0030, 0xc7ff, 0x0130, 0xcbff, 0x0230, 0xcfff, 0x0330,
    0xd3ff, 0x0430, 0xd7ff, 0x0530, 0xdbff, 0x0630, 0xdfff, 0x0730,
    0xe3ff, 0x0830, 0xe7ff, 0x0930, 0xebff, 0x0a30, 0xefff, 0x0b30,
    0xf3ff, 0x0c30, 0xf7ff, 0x0d30, 0xfbff, 0x0e30, 0xffff, 0x0f30,
};

static const unsigned short init2[128] = {
    0x03ff, 0x8030, 0x07ff, 0x8130, 0x0bff, 0x8230, 0x0fff, 0x8330,
    0x13ff, 0x8430, 0x17ff, 0x8530, 0x1bff, 0x8630, 0x1fff, 0x8730,
    0x23ff, 0x8830, 0x27ff, 0x8930, 0x2bff, 0x8a30, 0x2fff, 0x8b30,
    0x33ff, 0x8c30, 0x37ff, 0x8d30, 0x3bff, 0x8e30, 0x3fff, 0x8f30,
    0x43ff, 0x8030, 0x47ff, 0x8130, 0x4bff, 0x8230, 0x4fff, 0x8330,
    0x53ff, 0x8430, 0x57ff, 0x8530, 0x5bff, 0x8630, 0x5fff, 0x8730,
    0x63ff, 0x8830, 0x67ff, 0x8930, 0x6bff, 0x8a30, 0x6fff, 0x8b30,
    0x73ff, 0x8c30, 0x77ff, 0x8d30, 0x7bff, 0x8e30, 0x7fff, 0x8f30,
    0x83ff, 0x8030, 0x87ff, 0x8130, 0x8bff, 0x8230, 0x8fff, 0x8330,
    0x93ff, 0x8430, 0x97ff, 0x8530, 0x9bff, 0x8630, 0x9fff, 0x8730,
    0xa3ff, 0x8830, 0xa7ff, 0x8930, 0xabff, 0x8a30, 0xafff, 0x8b30,
    0xb3ff, 0x8c30, 0xb7ff, 0x8d30, 0xbbff, 0x8e30, 0xbfff, 0x8f30,
    0xc3ff, 0x8030, 0xc7ff, 0x8130, 0xcbff, 0x8230, 0xcfff, 0x8330,
    0xd3ff, 0x8430, 0xd7ff, 0x8530, 0xdbff, 0x8630, 0xdfff, 0x8730,
    0xe3ff, 0x8830, 0xe7ff, 0x8930, 0xebff, 0x8a30, 0xefff, 0x8b30,
    0xf3ff, 0x8c30, 0xf7ff, 0x8d30, 0xfbff, 0x8e30, 0xffff, 0x8f30,
};

static const unsigned short init3[128] = {
    0x0C10, 0x8470, 0x14FE, 0xB488, 0x167F, 0xA470, 0x18E7, 0x84B5,
    0x1B6E, 0x842A, 0x1F1D, 0x852A, 0x0DA3, 0x8F7C, 0x167E, 0xF254,
    0x0000, 0x842A, 0x0001, 0x852A, 0x18E6, 0x8BAA, 0x1B6D, 0xF234,
    0x229F, 0x8429, 0x2746, 0x8529, 0x1F1C, 0x86E7, 0x229E, 0xF224,
    0x0DA4, 0x8429, 0x2C29, 0x8529, 0x2745, 0x87F6, 0x2C28, 0xF254,
    0x383B, 0x8428, 0x320F, 0x8528, 0x320E, 0x8F02, 0x1341, 0xF264,
    0x3EB6, 0x8428, 0x3EB9, 0x8528, 0x383A, 0x8FA9, 0x3EB5, 0xF294,
    0x3EB7, 0x8474, 0x3EBA, 0x8575, 0x3EB8, 0xC4C3, 0x3EBB, 0xC5C3,
    0x0000, 0xA404, 0x0001, 0xA504, 0x141F, 0x8671, 0x14FD, 0x8287,
    0x3EBC, 0xE610, 0x3EC8, 0x8C7B, 0x031A, 0x87E6, 0x3EC8, 0x86F7,
    0x3EC0, 0x821E, 0x3EBE, 0xD208, 0x3EBD, 0x821F, 0x3ECA, 0x8386,
    0x3EC1, 0x8C03, 0x3EC9, 0x831E, 0x3ECA, 0x8C4C, 0x3EBF, 0x8C55,
    0x3EC9, 0xC208, 0x3EC4, 0xBC84, 0x3EC8, 0x8EAD, 0x3EC8, 0xD308,
    0x3EC2, 0x8F7E, 0x3ECB, 0x8219, 0x3ECB, 0xD26E, 0x3EC5, 0x831F,
    0x3EC6, 0xC308, 0x3EC3, 0xB2FF, 0x3EC9, 0x8265, 0x3EC9, 0x8319,
    0x1342, 0xD36E, 0x3EC7, 0xB3FF, 0x0000, 0x8365, 0x1420, 0x9570,
};

static const unsigned short init4[128] = {
    0x0C10, 0x8470, 0x14FE, 0xB488, 0x167F, 0xA470, 0x18E7, 0x84B5,
    0x1B6E, 0x842A, 0x1F1D, 0x852A, 0x0DA3, 0x0F7C, 0x167E, 0x7254,
    0x0000, 0x842A, 0x0001, 0x852A, 0x18E6, 0x0BAA, 0x1B6D, 0x7234,
    0x229F, 0x8429, 0x2746, 0x8529, 0x1F1C, 0x06E7, 0x229E, 0x7224,
    0x0DA4, 0x8429, 0x2C29, 0x8529, 0x2745, 0x07F6, 0x2C28, 0x7254,
    0x383B, 0x8428, 0x320F, 0x8528, 0x320E, 0x0F02, 0x1341, 0x7264,
    0x3EB6, 0x8428, 0x3EB9, 0x8528, 0x383A, 0x0FA9, 0x3EB5, 0x7294,
    0x3EB7, 0x8474, 0x3EBA, 0x8575, 0x3EB8, 0x44C3, 0x3EBB, 0x45C3,
    0x0000, 0xA404, 0x0001, 0xA504, 0x141F, 0x0671, 0x14FD, 0x0287,
    0x3EBC, 0xE610, 0x3EC8, 0x0C7B, 0x031A, 0x07E6, 0x3EC8, 0x86F7,
    0x3EC0, 0x821E, 0x3EBE, 0xD208, 0x3EBD, 0x021F, 0x3ECA, 0x0386,
    0x3EC1, 0x0C03, 0x3EC9, 0x031E, 0x3ECA, 0x8C4C, 0x3EBF, 0x0C55,
    0x3EC9, 0xC208, 0x3EC4, 0xBC84, 0x3EC8, 0x0EAD, 0x3EC8, 0xD308,
    0x3EC2, 0x8F7E, 0x3ECB, 0x0219, 0x3ECB, 0xD26E, 0x3EC5, 0x031F,
    0x3EC6, 0xC308, 0x3EC3, 0x32FF, 0x3EC9, 0x0265, 0x3EC9, 0x8319,
    0x1342, 0xD36E, 0x3EC7, 0x33FF, 0x0000, 0x8365, 0x1420, 0x9570,
};

/* Tabelle in vier Teilen: INIT1/INIT3 ueber DATA1, INIT2/INIT4 ueber
   DATA2, Register 2 bzw. 3, Stimme = Index */
static void aw_send(const unsigned short *p)
{
    unsigned i;
    for (i = 0; i < 32; i++)
        ew(E_D1, ECMD(2, i), *p++);
    for (i = 0; i < 32; i++)
        ew(E_D2, ECMD(2, i), *p++);
    for (i = 0; i < 32; i++)
        ew(E_D1, ECMD(3, i), *p++);
    for (i = 0; i < 32; i++)
        ew(E_D2, ECMD(3, i), *p++);
}

/* Wanduhr (PTR Bit 12, 44,1 kHz / 1024) abwarten, mit Frist */
static void aw_clock(void)
{
    unsigned t = ticks();
    while (!(aw_inw(E_PTR) & 0x1000) && (unsigned)(ticks() - t) < 3)
        ;
    while ((aw_inw(E_PTR) & 0x1000) && (unsigned)(ticks() - t) < 3)
        ;
}

/* Stimmen 30/31: DRAM-Refresh (und FM-Hall); ohne sie verliert das RAM
   der Karte seinen Inhalt */
static void aw_initfm(void)
{
    DCYSUSV(30, 0x80);
    PSST(30, 0xFFFFFFE0UL);
    CSL(30, 0x00FFFFE8UL);
    PTRX(30, 0);
    CPF(30, 0);
    CCCA(30, 0x00FFFFE3UL);
    DCYSUSV(31, 0x80);
    PSST(31, 0x00FFFFF0UL);
    CSL(31, 0x00FFFFF8UL);
    PTRX(31, 0);
    CPF(31, 0x8000);
    CCCA(31, 0x00FFFFF3UL);
    ew(E_D0, ECMD(1, 30), 0);
    aw_clock();
    ew(E_D0, ECMD(1, 30), 0x4828);
    outp(E_PTR, 0x3C);                  /* so im ADIP                   */
    outp(E_D1, 0);
    VTFT(30, 0x8000FFFFUL);
    VTFT(31, 0x8000FFFFUL);
}

/* Stimme still und leer */
static void aw_off(unsigned c)
{
    DCYSUSV(c, 0x807F);
}

/* Karten-RAM: Stimme 0 schreibt, 1 liest (DMA-Betrieb der Stimmen) */
static void aw_dma(unsigned c, int wr)
{
    DCYSUSV(c, 0x80);
    VTFT(c, 0);
    CVCF(c, 0);
    PTRX(c, 0x40000000UL);
    CPF(c, 0x40000000UL);
    PSST(c, 0);
    CSL(c, 0);
    CCCA(c, wr ? 0x06000000UL : 0x04000000UL);
}

static void aw_dmaclose(unsigned c)
{
    CCCA(c, 0);
    DCYSUSV(c, 0x807F);
}

static void aw_wwait(void)
{
    unsigned t = ticks();
    while ((erd(E_D1, SMALW) & 0x80000000UL) && (unsigned)(ticks() - t) < 10)
        ;
}

/* Groesse des Karten-RAM in Schritten zu 512 KB (bis 28 MB) */
static unsigned aw_dramsize(void)
{
    unsigned kb = 0;
    aw_dma(0, 1);
    aw_dma(1, 0);
    ewd(E_D1, SMALW, DRAM_OFS);
    ew(E_D1, SMLD, 0xA5B9);
    aw_initfm();
    aw_wwait();
    ewd(E_D1, SMALR, DRAM_OFS);
    er(E_D1, SMLD);                     /* veraltetes Wort verwerfen    */
    if (er(E_D1, SMLD) == 0xA5B9)
        for (kb = 512; kb < 28u * 1024; kb += 512) {
            unsigned long a = DRAM_OFS + ((unsigned long)kb << 9);  /* Worte */
            ewd(E_D1, SMALW, a);
            ew(E_D1, SMLD, 0x9D53);
            aw_wwait();
            ewd(E_D1, SMALR, a);
            er(E_D1, SMLD);
            if (er(E_D1, SMLD) != 0x9D53)
                break;
            ewd(E_D1, SMALR, DRAM_OFS);
            er(E_D1, SMLD);
            if (er(E_D1, SMLD) != 0xA5B9)
                break;                  /* Adresse lief um               */
        }
    aw_wwait();
    aw_dmaclose(0);
    aw_dmaclose(1);
    return kb;
}

/* EMU8000 suchen: erst lesen (nichts da = FFFFh an Zeiger und DATA1),
   dann die Konfigurationsregister schreiben und zurueckpruefen */
static int aw_detect(unsigned b)
{
    ebase = b;
    if (aw_inw(E_PTR) == 0xFFFF && aw_inw(E_D1) == 0xFFFF) {
        ebase = 0;
        return 0;
    }
    ew(E_D1, HWCF1, 0x0059);
    ew(E_D1, HWCF2, 0x0020);
    ew(E_D1, HWCF3, 0x0000);
    if ((er(E_D1, HWCF1) & 0x7E) != 0x58 || (er(E_D1, HWCF2) & 3) != 3) {
        ebase = 0;
        return 0;
    }
    return 1;
}

static void aw_init(void)
{
    unsigned c;
    ew(E_D1, HWCF1, 0x0059);
    ew(E_D1, HWCF2, 0x0020);
    ew(E_D1, HWCF3, 0);                 /* Ausgabe aus (weniger Knacken) */
    for (c = 0; c < 32; c++)
        DCYSUSV(c, 0x80);
    for (c = 0; c < 32; c++) {
        ENVVOL(c, 0); ENVVAL(c, 0); DCYSUS(c, 0); ATKHLDV(c, 0);
        LFO1VAL(c, 0); ATKHLD(c, 0); LFO2VAL(c, 0); IP(c, 0);
        IFATN(c, 0); PEFE(c, 0); FMMOD(c, 0); TREMFRQ(c, 0);
        FM2FRQ2(c, 0); PTRX(c, 0); VTFT(c, 0); PSST(c, 0);
        CSL(c, 0); CCCA(c, 0);
    }
    for (c = 0; c < 32; c++) {
        CPF(c, 0);
        CVCF(c, 0);
    }
    ewd(E_D1, SMALR, 0);
    ewd(E_D1, SMARR, 0);
    ewd(E_D1, SMALW, 0);
    ewd(E_D1, SMARW, 0);
    aw_send(init1);
    delay(3);                           /* 1024 Takte des Chips          */
    aw_send(init2);
    aw_send(init3);
    ewd(E_D1, HWCF4, 0);
    ewd(E_D1, HWCF5, 0x83);
    ewd(E_D1, HWCF6, 0x8000);
    aw_send(init4);
    aw_initfm();
    for (c = 0; c < NAWEV; c++)
        aw_off(c);
    edram_kb = aw_dramsize();
    ew(E_D1, HWCF3, 4);                 /* Ausgabe an                    */
}

/* Stimme c mit einem Sample starten (Wortadressen, Ende/Schleife wie im
   SoundFont); ip: Tonhoehe (E000h = 44,1 kHz ohne Verschiebung, 1000h je
   Oktave), att: Daempfung 0-255, pan 0 links - 255 rechts */
static void aw_start(unsigned c, unsigned long start, unsigned long lstart,
                     unsigned long lend, unsigned ip, unsigned char att,
                     unsigned char pan)
{
    DCYSUSV(c, 0x0080);                 /* still                         */
    VTFT(c, 0x0000FFFFUL);
    CVCF(c, 0x0000FFFFUL);
    PTRX(c, 0);
    CPF(c, 0);
    IP(c, ip);
    ENVVAL(c, 0x8000);                  /* Hullkurven: ohne Verzoegerung */
    ATKHLD(c, 0x7F7F);
    DCYSUS(c, 0x7F7F);
    ENVVOL(c, 0x8000);
    ATKHLDV(c, 0x7F7F);
    IFATN(c, 0xFF00 | att);             /* Filter offen                  */
    PEFE(c, 0);
    LFO1VAL(c, 0x8000);
    LFO2VAL(c, 0x8000);
    FMMOD(c, 0);
    TREMFRQ(c, 0x0010);
    FM2FRQ2(c, 0x0010);
    PSST(c, ((unsigned long)(unsigned char)(255 - pan) << 24) | (lstart - 1));
    CSL(c, lend - 1);
    CCCA(c, start - 1);
    R00A0(c, 0);
    R0080(c, 0);
    VTFT(c, 0xFFFF0000UL | 0xFFFF);
    CVCF(c, 0xFFFF0000UL | 0xFF00);
    PTRX(c, 0x40000000UL);
    CPF(c, 0x40000000UL);
    DCYSUSV(c, 0x7F7F);                 /* Hullkurve los                 */
}

static void aw_release(unsigned c)
{
    DCYSUS(c, 0x8000 | 0x40);
    DCYSUSV(c, 0x8000 | 0x40);
}
