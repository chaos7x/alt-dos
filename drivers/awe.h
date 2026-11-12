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

/* ==== Klangbank und MIDI-Synthesizer ==================================
   Die Bank (Zonen schon in EMU8000-Werten, AWELOAD) liegt in einem
   eigenen Segment: vorn bis 256 Presets, dahinter die Zonen. Berechnung
   von Tonhoehe, Lautstaerke und Pan wie im Linux-Treiber (sound/synth/
   emux/emux_synth.c, emu8000_callback.c; GPL-2.0-or-later; die Tabellen
   voltab1/voltab2/expressiontab/pan_volumes von dort). */

#define AW_NPRE     256
#define AW_ZOFF     (AW_NPRE * sizeof(struct fapi_awepre))
#define AW_BYTES    61440u
#define AW_NZ1      ((AW_BYTES - AW_ZOFF) / sizeof(struct fapi_awezone))
#define AW_NZ2      (AW_BYTES / sizeof(struct fapi_awezone))
#define AW_NZONE    (AW_NZ1 + AW_NZ2)  /* zweites Segment erst bei Bedarf */
#define AWZ(i)      ((i) < AW_NZ1 ? \
    (struct fapi_awezone __far *)MK_FP(zsel, AW_ZOFF + (i) * sizeof(struct fapi_awezone)) : \
    (struct fapi_awezone __far *)MK_FP(zsel2, ((i) - AW_NZ1) * sizeof(struct fapi_awezone)))
#define AWP(i)      ((struct fapi_awepre __far *)MK_FP(zsel, (i) * sizeof(struct fapi_awepre)))

static unsigned zsel;                   /* Segment der Bank (0 = keins) */
static unsigned zsel2;                  /* zweites (grosse Baenke)      */
static unsigned nzone, npre;
static unsigned char awe_on;            /* Bank fertig: MIDI hierher    */
static unsigned char use_mpu;           /* /MPU: MIDI an den MPU-401    */

/* a * b / c mit 32-Bit-Zwischenergebnis (Ergebnis passt in 16 Bit) */
int aw_muldiv(int a, int b, int c);
#pragma aux aw_muldiv = "imul dx" "idiv bx" __parm [__ax] [__dx] [__bx] __value [__ax] __modify [__dx];
unsigned aw_muldivu(unsigned a, unsigned b, unsigned c);
#pragma aux aw_muldivu = "mul dx" "div bx" __parm [__ax] [__dx] [__bx] __value [__ax] __modify [__dx];

static const unsigned char voltab1[128] = {
    0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x2b, 0x29, 0x28, 0x27, 0x26,
    0x25, 0x24, 0x23, 0x22, 0x21, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a, 0x19, 0x19,
    0x18, 0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x11, 0x10, 0x10, 0x10, 0x0f, 0x0f, 0x0f, 0x0e, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c,
    0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b, 0x0a, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x09,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char voltab2[128] = {
    0x32, 0x31, 0x30, 0x2f, 0x2e, 0x2d, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x27, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x21, 0x21, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1c, 0x1c, 0x1b, 0x1a, 0x1a, 0x19,
    0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x11,
    0x11, 0x10, 0x10, 0x10, 0x0f, 0x0f, 0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c,
    0x0b, 0x0b, 0x0b, 0x0b, 0x0a, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x07, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char exprtab[128] = {
    0x7f, 0x6c, 0x62, 0x5a, 0x54, 0x50, 0x4b, 0x48, 0x45, 0x42, 0x40, 0x3d, 0x3b, 0x39, 0x38, 0x36,
    0x34, 0x33, 0x31, 0x30, 0x2f, 0x2d, 0x2c, 0x2b, 0x2a, 0x29, 0x28, 0x27, 0x26, 0x25, 0x24, 0x24,
    0x23, 0x22, 0x21, 0x21, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19,
    0x18, 0x18, 0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x15, 0x14, 0x14, 0x13, 0x13, 0x12, 0x12, 0x11,
    0x11, 0x11, 0x10, 0x10, 0x0f, 0x0f, 0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c,
    0x0b, 0x0b, 0x0b, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 0x07, 0x07, 0x07,
    0x07, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03,
    0x03, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char panvol[256] = {
    0x00,0x03,0x06,0x09,0x0c,0x0f,0x12,0x14,0x17,0x1a,0x1d,0x20,0x22,0x25,0x28,0x2a,
    0x2d,0x30,0x32,0x35,0x37,0x3a,0x3c,0x3f,0x41,0x44,0x46,0x49,0x4b,0x4d,0x50,0x52,
    0x54,0x57,0x59,0x5b,0x5d,0x60,0x62,0x64,0x66,0x68,0x6a,0x6c,0x6f,0x71,0x73,0x75,
    0x77,0x79,0x7b,0x7c,0x7e,0x80,0x82,0x84,0x86,0x88,0x89,0x8b,0x8d,0x8f,0x90,0x92,
    0x94,0x96,0x97,0x99,0x9a,0x9c,0x9e,0x9f,0xa1,0xa2,0xa4,0xa5,0xa7,0xa8,0xaa,0xab,
    0xad,0xae,0xaf,0xb1,0xb2,0xb3,0xb5,0xb6,0xb7,0xb9,0xba,0xbb,0xbc,0xbe,0xbf,0xc0,
    0xc1,0xc2,0xc3,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,0xd0,0xd1,
    0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd7,0xd8,0xd9,0xda,0xdb,0xdc,0xdc,0xdd,0xde,0xdf,
    0xdf,0xe0,0xe1,0xe2,0xe2,0xe3,0xe4,0xe4,0xe5,0xe6,0xe6,0xe7,0xe8,0xe8,0xe9,0xe9,
    0xea,0xeb,0xeb,0xec,0xec,0xed,0xed,0xee,0xee,0xef,0xef,0xf0,0xf0,0xf1,0xf1,0xf1,
    0xf2,0xf2,0xf3,0xf3,0xf3,0xf4,0xf4,0xf5,0xf5,0xf5,0xf6,0xf6,0xf6,0xf7,0xf7,0xf7,
    0xf7,0xf8,0xf8,0xf8,0xf9,0xf9,0xf9,0xf9,0xf9,0xfa,0xfa,0xfa,0xfa,0xfb,0xfb,0xfb,
    0xfb,0xfb,0xfc,0xfc,0xfc,0xfc,0xfc,0xfc,0xfc,0xfd,0xfd,0xfd,0xfd,0xfd,0xfd,0xfd,
    0xfd,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
};

/* MIDI-Kanaele und Stimmen */
struct awch {
    unsigned char prog, bank, vol, expr, pan, sus, rpn_l, rpn_h, bendr;
    int bend;                           /* -8192 .. 8191                */
};
static struct awch chn[16];
#define VS_FREE 0
#define VS_ON   1
#define VS_SUS  2                       /* losgelassen, Pedal haelt     */
#define VS_REL  3                       /* klingt aus                   */
struct awv {
    unsigned char ch, key, vel, state, excl;
    unsigned zone, age;
};
static struct awv vo[NAWEV];
static unsigned vage;

static void ch_reset(unsigned c)
{
    struct awch *h = &chn[c];
    h->bank = 0;
    h->vol = 100;
    h->expr = 127;
    h->pan = 64;
    h->sus = 0;
    h->rpn_l = h->rpn_h = 127;
    h->bendr = 2;
    h->bend = 0;
}

static void awe_reset_all(void)
{
    unsigned c;
    for (c = 0; c < NAWEV; c++) {
        aw_off(c);
        vo[c].state = VS_FREE;
    }
    for (c = 0; c < 16; c++) {
        ch_reset(c);
        chn[c].prog = 0;
    }
}

static int find_pre(unsigned bank, unsigned prog)
{
    unsigned i;
    for (i = 0; i < npre; i++)
        if (AWP(i)->bank == bank && AWP(i)->prog == prog)
            return (int)i;
    return -1;
}

/* Tonhoehe der Stimme (IP-Register) */
static unsigned v_pitch(struct awv *v, struct fapi_awezone __far *z)
{
    struct awch *h = &chn[v->ch];
    int d = (int)(z->fixkey != 0xFF ? z->fixkey : v->key) - (int)z->root;
    long off;
    d *= z->scale;                      /* Cent                          */
    if (d > 9500) d = 9500;
    if (d < -9500) d = -9500;
    off = aw_muldiv(d, 4096, 1200);
    if (h->bend)
        off += aw_muldiv(h->bend, h->bendr, 24);
    off += 0xE000L + z->rate;
    if (off < 0) off = 0;
    if (off > 0xFFFFL) off = 0xFFFFL;
    return (unsigned)off;
}

/* Daempfung (0 laut - 255 still) */
static unsigned v_vol(struct awv *v, struct fapi_awezone __far *z)
{
    struct awch *h = &chn[v->ch];
    int vol = voltab1[h->vol & 0x7F] + voltab2[v->vel & 0x7F];
    vol = vol * 8 / 3;
    vol += z->att;
    vol += ((0x100 - vol) * exprtab[h->expr & 0x7F]) / 128;
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    return (unsigned)vol;
}

static unsigned v_pan(struct awv *v, struct fapi_awezone __far *z)
{
    int pan = (int)chn[v->ch].pan - 64;
    if (z->pan != 0xFF)
        pan += (int)z->pan - 64;
    pan = 127 - pan * 2;
    if (pan < 0) pan = 0;
    if (pan > 255) pan = 255;
    return (unsigned)pan;
}

/* Tonhoehenziel (PTRX/CPF) aus IP, wie die Hardware es erwartet */
static unsigned v_ptarget(unsigned ip)
{
    unsigned long t;
    if (ip == 0xFFFF)
        return 0xFFFF;
    t = 1UL << (ip >> 12);
    if (t > 0xFFFF)
        return 0xFFFF;
    if (ip & 0x800) t += aw_muldivu((unsigned)t, 0x102E, 0x2710);
    if (ip & 0x400) t += aw_muldivu((unsigned)t, 0x764, 0x2710);
    if (ip & 0x200) t += aw_muldivu((unsigned)t, 0x389, 0x2710);
    t += t >> 1;
    return t > 0xFFFF ? 0xFFFF : (unsigned)t;
}

static unsigned v_alloc(void)
{
    unsigned c, best = 0, pri, bp = 0xFFFF, bage = 0;
    for (c = 0; c < NAWEV; c++) {
        /* frei vor ausklingend vor gehalten vor klingend, je die aelteste */
        pri = vo[c].state == VS_FREE ? 0 : vo[c].state == VS_REL ? 1 :
              vo[c].state == VS_SUS ? 2 : 3;
        if (pri < bp || (pri == bp && (unsigned)(vage - vo[c].age) > bage)) {
            bp = pri;
            bage = vage - vo[c].age;
            best = c;
        }
    }
    return best;
}

static void v_start(unsigned ch, unsigned key, unsigned vel, unsigned zi)
{
    struct fapi_awezone __far *z = AWZ(zi);
    struct awv *v;
    unsigned c, ip, vol, pan, cut, pt;
    unsigned char drum = (unsigned char)(ch == 9);

    if (z->excl)                        /* exklusive Klasse: andere aus */
        for (c = 0; c < NAWEV; c++)
            if (vo[c].state != VS_FREE && vo[c].ch == ch && vo[c].excl == z->excl) {
                DCYSUSV(c, 0x807F);
                vo[c].state = VS_FREE;
            }
    c = v_alloc();
    v = &vo[c];
    v->ch = (unsigned char)ch;
    v->key = (unsigned char)key;
    v->vel = (unsigned char)(z->fixvel != 0xFF ? z->fixvel : vel);
    v->excl = z->excl;
    v->zone = zi;
    v->age = ++vage;
    v->state = VS_ON;

    ip = v_pitch(v, z);
    vol = v_vol(v, z);
    pan = v_pan(v, z);
    if (!drum && (z->volatkhld & 0xFF) < 0x7D)  /* leiser = dumpfer */
        cut = ((v->vel < 70 ? 70 : v->vel) * z->cutoff + 0xA0) >> 7;
    else
        cut = z->cutoff;
    pt = v_ptarget(ip);

    DCYSUSV(c, 0x0080);                 /* still und angehalten          */
    VTFT(c, 0x0000FFFFUL);
    CVCF(c, 0x0000FFFFUL);
    PTRX(c, 0);
    CPF(c, 0);
    IP(c, ip);
    ENVVAL(c, z->moddelay);
    ATKHLD(c, z->modatkhld);
    DCYSUS(c, z->moddcysus);
    ENVVOL(c, z->voldelay);
    ATKHLDV(c, z->volatkhld);
    IFATN(c, (cut << 8) | vol);
    PEFE(c, z->pefe);
    LFO1VAL(c, z->lfo1delay);
    LFO2VAL(c, z->lfo2delay);
    FMMOD(c, z->fmmod);
    TREMFRQ(c, z->tremfrq);
    FM2FRQ2(c, z->fm2frq2);
    PSST(c, ((unsigned long)panvol[pan] << 24) | (z->lstart - 1));
    CSL(c, ((unsigned long)z->chorus << 24) | (z->lend - 1));
    CCCA(c, ((unsigned long)(z->filterq & 0x0F) << 28) | (z->start - 1));
    R00A0(c, 0);
    R0080(c, 0);
    VTFT(c, (unsigned long)cut << 8);   /* Ziele: Lautstaerke regelt die  */
    CVCF(c, 0x0000FF00UL);              /* Hullkurve, Filter = Grenze     */
    PTRX(c, ((unsigned long)pt << 16) | ((unsigned)z->reverb << 8) | panvol[255 - pan]);
    CPF(c, (unsigned long)pt << 16);
    DCYSUSV(c, z->voldcysus);           /* Hullkurve los                 */
}

static void v_release(unsigned c)
{
    struct fapi_awezone __far *z = AWZ(vo[c].zone);
    DCYSUS(c, 0x8000 | (z->modrelease & 0xFF));
    DCYSUSV(c, 0x8000 | (z->volrelease & 0xFF));
    vo[c].state = VS_REL;
}

static void note_on(unsigned ch, unsigned key, unsigned vel)
{
    int p;
    unsigned i;
    struct fapi_awepre __far *pp;
    if (ch == 9) {                      /* Schlagzeug: Bank 128          */
        p = find_pre(128, chn[ch].prog);
        if (p < 0)
            p = find_pre(128, 0);
    } else {
        p = find_pre(chn[ch].bank, chn[ch].prog);
        if (p < 0)
            p = find_pre(0, chn[ch].prog);
    }
    if (p < 0)
        return;
    pp = AWP(p);
    for (i = pp->first; i < pp->first + pp->n && i < nzone; i++) {
        struct fapi_awezone __far *z = AWZ(i);
        if (key >= z->klo && key <= z->khi && vel >= z->vlo && vel <= z->vhi)
            v_start(ch, key, vel, i);
    }
}

static void note_off(unsigned ch, unsigned key)
{
    unsigned c;
    for (c = 0; c < NAWEV; c++)
        if (vo[c].state == VS_ON && vo[c].ch == ch && vo[c].key == key) {
            if (chn[ch].sus)
                vo[c].state = VS_SUS;
            else
                v_release(c);
        }
}

/* Lautstaerke/Pan/Tonhoehe klingender Stimmen eines Kanals neu */
static void ch_update(unsigned ch, int what)
{
    unsigned c;
    for (c = 0; c < NAWEV; c++) {
        struct fapi_awezone __far *z;
        if (vo[c].state == VS_FREE || vo[c].ch != ch)
            continue;
        z = AWZ(vo[c].zone);
        if (what == 0)
            IFATN(c, (er(E_D3, ECMD(1, c)) & 0xFF00) | v_vol(&vo[c], z));
        else if (what == 1)
            PSST(c, ((unsigned long)panvol[v_pan(&vo[c], z)] << 24) | (z->lstart - 1));
        else
            IP(c, v_pitch(&vo[c], z));
    }
}

static void ch_alloff(unsigned ch, int now)
{
    unsigned c;
    for (c = 0; c < NAWEV; c++)
        if (vo[c].state != VS_FREE && vo[c].ch == ch) {
            if (now) {
                DCYSUSV(c, 0x807F);
                vo[c].state = VS_FREE;
            } else if (vo[c].state != VS_REL)
                v_release(c);
        }
}

static void control(unsigned ch, unsigned cc, unsigned v)
{
    struct awch *h = &chn[ch];
    switch (cc) {
    case 0: h->bank = (unsigned char)v; break;
    case 7: h->vol = (unsigned char)v; ch_update(ch, 0); break;
    case 11: h->expr = (unsigned char)v; ch_update(ch, 0); break;
    case 10: h->pan = (unsigned char)v; ch_update(ch, 1); break;
    case 64:
        h->sus = (unsigned char)(v >= 64);
        if (!h->sus) {
            unsigned c;
            for (c = 0; c < NAWEV; c++)
                if (vo[c].state == VS_SUS && vo[c].ch == ch)
                    v_release(c);
        }
        break;
    case 100: h->rpn_l = (unsigned char)v; break;
    case 101: h->rpn_h = (unsigned char)v; break;
    case 6:                             /* RPN 0: Bereich Pitch Bend    */
        if (h->rpn_h == 0 && h->rpn_l == 0)
            h->bendr = (unsigned char)(v > 24 ? 24 : v);
        break;
    case 120: ch_alloff(ch, 1); break;  /* All Sound Off                */
    case 123: ch_alloff(ch, 0); break;  /* All Notes Off                */
    case 121: {                         /* Controller zurueck           */
        unsigned char b = h->bank;
        ch_reset(ch);
        h->bank = b;
        ch_update(ch, 0);
        ch_update(ch, 2);
        break;
    }
    }
}

/* MIDI-Bytes (laufender Status, SysEx und Echtzeit uebergangen) */
static unsigned char m_st, m_d[2], m_n, m_sysex;

static void awe_midi(unsigned char b)
{
    unsigned ch, need;
    if (b >= 0xF8)
        return;                         /* Echtzeit                      */
    if (b == 0xF0) {
        m_sysex = 1;
        return;
    }
    if (b == 0xF7) {
        m_sysex = 0;
        return;
    }
    if (m_sysex)
        return;
    if (b & 0x80) {
        m_st = b;
        m_n = 0;
        return;
    }
    if (m_st < 0x80 || m_st >= 0xF0)
        return;
    m_d[m_n++] = b;
    need = (m_st & 0xE0) == 0xC0 ? 1 : 2;   /* Cx, Dx: ein Datenbyte  */
    if (m_n < need)
        return;
    m_n = 0;
    ch = m_st & 0x0F;
    switch (m_st & 0xF0) {
    case 0x80: note_off(ch, m_d[0]); break;
    case 0x90:
        if (m_d[1])
            note_on(ch, m_d[0], m_d[1]);
        else
            note_off(ch, m_d[0]);
        break;
    case 0xB0: control(ch, m_d[0], m_d[1]); break;
    case 0xC0: chn[ch].prog = m_d[0]; break;
    case 0xE0:
        chn[ch].bend = (int)((m_d[1] << 7) | m_d[0]) - 8192;
        ch_update(ch, 2);
        break;
    }
}

/* AR_AWE: Bank laden */
static int awe_cmd(struct aud_req __far *q)
{
    struct fapi_awecmd __far *k = MK_FP(q->sel, q->off);
    unsigned char __far *d = (unsigned char __far *)(k + 1);
    unsigned i, n = k->n, room = q->len - sizeof(*k);
    if (!ebase || !zsel)
        return -1;
    switch (k->op) {
    case AWE_INFO:
        k->addr = (unsigned long)edram_kb << 9;
        k->n = AW_NZONE;
        return 0;
    case AWE_RESET:
        awe_on = 0;
        nzone = npre = 0;
        awe_reset_all();
        return 0;
    case AWE_ZONES:
        if (n * sizeof(struct fapi_awezone) > room || nzone + n > AW_NZONE)
            return -1;
        if (nzone + n > AW_NZ1 && !zsel2) {
            static unsigned long zlin2;
            zsel2 = devhlp(DH_DMABUF, 0, AW_BYTES, (unsigned)&zlin2);
            if (!zsel2)
                return -1;
        }
        for (i = 0; i < n; i++, nzone++)    /* einzeln: Segmentgrenze */
            far_cpy(AWZ(nzone), d + i * sizeof(struct fapi_awezone),
                    sizeof(struct fapi_awezone));
        return 0;
    case AWE_PRESETS:
        if (n * sizeof(struct fapi_awepre) > room || n > AW_NPRE)
            return -1;
        far_cpy(AWP(0), d, n * sizeof(struct fapi_awepre));
        npre = n;
        return 0;
    case AWE_SAMPLE: {
        unsigned short __far *w = (unsigned short __far *)d;
        if (n * 2 > room || awe_on ||
            k->addr + n > ((unsigned long)edram_kb << 9))
            return -1;
        aw_dma(0, 1);
        aw_wwait();
        ewd(E_D1, SMALW, DRAM_OFS + k->addr);
        for (i = 0; i < n; i++)
            ew(E_D1, SMLD, w[i]);
        aw_wwait();
        aw_dmaclose(0);
        return 0;
    }
    case AWE_DONE:
        awe_reset_all();
        awe_on = (unsigned char)(npre && nzone);
        return 0;
    }
    return -1;
}
