/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test von kernel/pnp.c auf dem Entwicklungsrechner: bildet zwei ISA-PnP-
 * Karten nach der Spezifikation nach (SB16 PnP mit Gameport, Modem).
 *   gcc -DPNP_HOST -Itools/pnptest -o /tmp/pnpsim tools/pnptest/sim.c kernel/pnp.c
 *   /tmp/pnpsim 0   Normalfall
 *   /tmp/pnpsim 1   220h von einer alten Karte belegt
 *   /tmp/pnpsim 2   SB16 schon vom PnP-BIOS eingeschaltet (IRQ 10)
 */
#include "pnp_host.h"
#include <string.h>
#include <stdlib.h>
unsigned short bios_ports[7] = { 0x3F8, 0, 0, 0, 0x378, 0, 0 };
int pci_info(unsigned i, struct fapi_pcidev *p) { if (i) return 1; p->irq = 11; return 0; }
enum { WFK, SLEEP, ISO, CFG };
struct card {
    int state, csn, ldn, nld;
    unsigned char rom[512]; int romlen, rpos;
    unsigned char regs[8][256];
    int isobit, isophase;
    int bios_act;
};
static struct card cards[4]; static int ncards;
static unsigned addr, rdp; static int keypos; static unsigned keyseq[32];
static unsigned char occupied[0x10000];
static void mkkey(void){unsigned c=0x6A;for(int i=0;i<32;i++){keyseq[i]=c;c=(c>>1)|(((c^(c>>1))&1)<<7);}}
static void addb(struct card*c,int b){c->rom[c->romlen++]=b;}
static void mkid(struct card*c,unsigned char v0,unsigned char v1,unsigned char p0,unsigned char p1,unsigned serial){
    unsigned char id[8]={v0,v1,p0,p1,serial,serial>>8,serial>>16,serial>>24};
    unsigned cs=0x6A; for(int i=0;i<64;i++){int bit=(id[i>>3]>>(i&7))&1; cs=(cs>>1)|((((cs^(cs>>1))&1)^bit)<<7);}
    for(int i=0;i<8;i++)addb(c,id[i]); addb(c,cs);
}
static void io(struct card*c,unsigned min,unsigned max,unsigned al,unsigned len){addb(c,0x47);addb(c,1);addb(c,min);addb(c,min>>8);addb(c,max);addb(c,max>>8);addb(c,al);addb(c,len);}
static void irq(struct card*c,unsigned m){addb(c,0x22);addb(c,m);addb(c,m>>8);}
static void dma(struct card*c,unsigned m){addb(c,0x2A);addb(c,m);addb(c,0x08);}
static void ld(struct card*c,unsigned char a,unsigned char b,unsigned char p0,unsigned char p1){addb(c,0x15);addb(c,a);addb(c,b);addb(c,p0);addb(c,p1);addb(c,0);c->nld++;}
static void sdep(struct card*c){addb(c,0x30);} static void edep(struct card*c){addb(c,0x38);}
static void end(struct card*c){addb(c,0x79);addb(c,0);}
/* serial isolation: card drives id bits */
static int iso_cards(void){int n=0;for(int i=0;i<ncards;i++)if(cards[i].state==ISO)n++;return n;}
unsigned inb(unsigned port){
    if (port==rdp && rdp) {
        if (addr==0x01) { /* isolation read */
            int any=0; unsigned v=0xFF; int b;
            /* bit index shared: use lowest-index card still isolating; cards whose bit is 0 while another drives 1 drop to sleep */
            for(int i=0;i<ncards;i++){struct card*c=&cards[i]; if(c->state!=ISO)continue;}
            /* determine bit each card wants */
            int bitpos=-1; int wants1=0;
            for(int i=0;i<ncards;i++){struct card*c=&cards[i]; if(c->state!=ISO)continue; bitpos=c->isobit; b=(c->rom[c->isobit>>3]>>(c->isobit&7))&1; if(b)wants1=1; any=1;}
            if(!any) return 0xFF;
            int phase=0; for(int i=0;i<ncards;i++)if(cards[i].state==ISO){phase=cards[i].isophase;break;}
            if(wants1) v = phase==0?0x55:0xAA;
            for(int i=0;i<ncards;i++){struct card*c=&cards[i]; if(c->state!=ISO)continue;
                b=(c->rom[c->isobit>>3]>>(c->isobit&7))&1;
                if(c->isophase==1){ if(!b&&wants1) c->state=SLEEP; else c->isobit++; c->isophase=0; } else c->isophase=1; }
            return v;
        }
        struct card*c=0; for(int i=0;i<ncards;i++)if(cards[i].state==CFG)c=&cards[i];
        if(!c) return 0xFF;
        if(addr==0x05) return 1;
        if(addr==0x04){ return c->rpos<c->romlen?c->rom[c->rpos++]:0; }
        if(addr==0x06) return c->csn;
        if(addr==0x07) return c->ldn;
        if(addr>=0x30) return c->regs[c->ldn][addr];
        return 0;
    }
    return occupied[port&0xFFFF]?0x12:0xFF;
}
void outb(unsigned port, unsigned v){
    if(port==0x80) return;
    if(port==0x279){
        addr=v;
        int anywfk=0; for(int i=0;i<ncards;i++)if(cards[i].state==WFK)anywfk=1;
        if(anywfk){ if(v==keyseq[keypos]) keypos++; else keypos=(v==keyseq[0]); if(keypos==32){ for(int i=0;i<ncards;i++)if(cards[i].state==WFK)cards[i].state=SLEEP; keypos=0; } }
        return;
    }
    if(port==0xA79){
        if(addr==0x00){rdp=(v<<2)|3;return;}
        if(addr==0x02){ for(int i=0;i<ncards;i++){ if(v&4)cards[i].csn=0; if(v&2)cards[i].state=WFK; } return; }
        if(addr==0x03){ for(int i=0;i<ncards;i++){struct card*c=&cards[i]; if(c->state==WFK)continue;
                if(c->csn==(int)v){ if(v==0){c->state=ISO;c->isobit=0;c->isophase=0;} else {c->state=CFG;c->rpos=0;} }
                else if(c->state!=WFK) c->state=SLEEP; } return; }
        if(addr==0x06){ for(int i=0;i<ncards;i++)if(cards[i].state==ISO&&cards[i].isobit>=72){cards[i].csn=v;cards[i].state=CFG;cards[i].rpos=0;} return; }
        struct card*c=0; for(int i=0;i<ncards;i++)if(cards[i].state==CFG)c=&cards[i];
        if(!c) return;
        if(addr==0x07){c->ldn=v;return;}
        c->regs[c->ldn][addr]=v; return;
    }
    if(port>=0x200 && port<0x400) return;
    printf("FEHLER: Schreiben auf Port %X = %X\n",port,v); exit(1);
}
unsigned pnp_init(void); int pnp_info(unsigned i, struct fapi_pnpdev *p); void pnp_line(unsigned i, char *b);
int main(int argc,char**argv){
    mkkey();
    int mode = argc>1?atoi(argv[1]):0;
    struct card*c=&cards[ncards++];
    /* SB16 PnP: CTL0031 Audio (3 deps), Gameport */
    mkid(c,0x0E,0x8C,0x00,0x24,0x1234);        /* CTL0024 */
    addb(c,0x0A);addb(c,0x10);addb(c,0x10);     /* PnP version */
    ld(c,0x0E,0x8C,0x00,0x31);
    sdep(c); irq(c,1<<5); dma(c,1<<1); dma(c,1<<5); io(c,0x220,0x220,1,16); io(c,0x330,0x330,1,2); io(c,0x388,0x388,1,4);
    sdep(c); irq(c,(1<<5)|(1<<7)|(1<<9)|(1<<10)); dma(c,0xB); dma(c,0xE0); io(c,0x220,0x280,0x20,16); io(c,0x300,0x330,0x30,2); io(c,0x388,0x388,1,4);
    sdep(c); irq(c,(1<<5)|(1<<7)); dma(c,0xB); dma(c,0); io(c,0x220,0x280,0x20,16); io(c,0,0,1,0); io(c,0x388,0x388,1,4);
    edep(c);
    ld(c,0x0E,0x8C,0x70,0x01); /* gameport */
    sdep(c); io(c,0x200,0x200,1,8); sdep(c); io(c,0x200,0x208,8,8); edep(c);
    end(c);
    /* second card: modem with IRQ list */
    c=&cards[ncards++];
    mkid(c,0x41,0xD0,0x55,0x01,0x99);
    ld(c,0x41,0xD0,0x55,0x01);
    sdep(c); irq(c,1<<4); io(c,0x3F8,0x3F8,1,8);
    sdep(c); irq(c,(1<<3)|(1<<4)|(1<<10)|(1<<11)); io(c,0x2E8,0x3F8,0x10,8);
    edep(c); end(c);
    occupied[0x3F8]=occupied[0x2F8]=1;
    if(mode==1){ /* 220h occupied by legacy card -> SB must take 240h */ occupied[0x220]=1; }
    if(mode==2){ struct card*k=&cards[0]; k->regs[0][0x30]=1; k->regs[0][0x60]=2; k->regs[0][0x61]=0x60; k->regs[0][0x70]=10; k->regs[0][0x74]=3; k->regs[0][0x75]=6; }
    unsigned n=pnp_init();
    printf("%u Geraete\n",n);
    for(unsigned i=0;i<n;i++){struct fapi_pnpdev p; pnp_info(i,&p);
        {char m[80];pnp_line(i,m);printf("  %s\n",m);} printf("csn %d ld %d act %d id %02X%02X%02X%02X io %X %X %X %X irq %d %d dma %d %d\n",p.csn,p.ldn,p.act,p.lid[0],p.lid[1],p.lid[2],p.lid[3],p.io[0],p.io[1],p.io[2],p.io[3],p.irq[0],p.irq[1],p.dma[0],p.dma[1]);}
    for(int i=0;i<ncards;i++){printf("Karte %d: state %d csn %d act0 %d io0 %X irq %d\n",i,cards[i].state,cards[i].csn,cards[i].regs[0][0x30],cards[i].regs[0][0x60]<<8|cards[i].regs[0][0x61],cards[i].regs[0][0x70]);}
    return 0;
}
