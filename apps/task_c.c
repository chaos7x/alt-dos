/* TASKC: versucht, an der FAPI vorbei direkt in den VGA-Speicher zu schreiben. */
#include "fapi.h"

/* mov ax,0B800h / mov es,ax / mov word ptr es:[0],4F21h */
void poke_vga(void);
#pragma aux poke_vga = \
    0xB8 0x00 0xB8              \
    0x8E 0xC0                   \
    0x26 0xC7 0x06 0x00 0x00 0x21 0x4F \
    __modify [__ax __es];

void main(void)
{
    puts("TASKC: schreibt gleich direkt nach B800:0000, wie unter Real-Mode-DOS.\n"
         "mov ax,0B800h / mov es,ax ...\n");
    DosSleep(1000);
    poke_vga();
    puts("Das hier d\x81rfte nie erscheinen!\n");
}
