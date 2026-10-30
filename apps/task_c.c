/* Task C: versucht, an der FAPI vorbei direkt in den VGA-Speicher zu schreiben. */
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
    VioWrtStr(0, 2, "Ich warte 3 Sekunden und schreibe dann direkt nach B800:0000 ...");
    DosSleep(3000);
    VioWrtStr(1, 2, "Jetzt: mov ax,0B800h / mov es,ax  (Real-Mode-Gewohnheit)");
    DosSleep(1000);
    poke_vga();
    VioWrtStr(2, 2, "Das hier d\x81rfte nie erscheinen!");
}
