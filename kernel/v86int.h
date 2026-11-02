/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AT-DOS 6.0 - gemeinsame Typen von v86.c und redir.c (DOS-Boxen) */
#ifndef V86INT_H
#define V86INT_H

/* CPU-Rahmen beim Eintritt aus V86 (32-Bit-Gate), ab EIP */
struct v86_iret {
    unsigned long eip, cs, eflags, esp, ss, es, ds, fs, gs;
};
struct v86_frame {
    unsigned short es, ds;              /* vom Handler (genullt)         */
    unsigned long  edi, esi, ebp, esp_k, ebx, edx, ecx, eax;   /* PUSHAD */
    unsigned short vec;
    unsigned long  err;
    struct v86_iret r;
};

/* redir.c: INT 2Fh 11xxh (Redirector) der Box -> AT-DOS-Dateien.
   -> 0 erledigt, > 0 DOS-Fehlercode (CF), -1 nicht zustaendig */
int __far redir(struct v86_frame *f, int t);
void __far redir_sda(int t, unsigned seg, unsigned off);
#endif
