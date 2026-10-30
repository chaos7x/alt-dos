#ifndef MEM_H
#define MEM_H

#define MEM_UNIT        256         /* Bytes je Einheit                    */
#define MEM_LOW_START   0x0200      /* 20000h: unterhalb Kernel/Anwendungen */
#define MEM_HIGH_START  0x1000      /* 100000h: 1 MB                       */

void     mem_init(unsigned conv_kb, unsigned ext_kb);
unsigned mem_alloc(unsigned units);             /* -> erste Einheit, 0 = Fehler */
void     mem_free(unsigned start, unsigned units);
unsigned mem_free_kb(void);

#endif
