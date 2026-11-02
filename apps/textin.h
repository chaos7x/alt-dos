/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Gepuffertes Lesen fuer die Textwerkzeuge (HEAD, TAIL, WC, UNIQ, TR):
 * wird per #include eingebunden, nicht gelinkt.
 */
#ifndef TEXTIN_H
#define TEXTIN_H

static unsigned char ti_buf[2048], to_buf[2048];
static unsigned ti_pos, ti_len, to_pos, ti_h;

static void ti_open(unsigned h)
{
    ti_h = h;
    ti_pos = ti_len = 0;
}

static int ti_get(void)
{
    if (ti_pos >= ti_len) {
        ti_len = DosRead(ti_h, ti_buf, sizeof(ti_buf));
        if (ti_len == FAPI_ERROR || ti_len == 0) {
            ti_len = 0;
            return -1;
        }
        ti_pos = 0;
    }
    return ti_buf[ti_pos++];
}

static void to_flush(void)
{
    if (to_pos)
        DosWrite(STDOUT, to_buf, to_pos);
    to_pos = 0;
}

static void to_put(char c)
{
    to_buf[to_pos++] = (unsigned char)c;
    if (to_pos == sizeof(to_buf))
        to_flush();
}

void to_str(const char *s)      /* nicht static: nicht jeder nutzt es */
{
    while (*s)
        to_put(*s++);
}

#ifdef TI_NUM                   /* nur, wer es braucht (sonst W202) */
/* Zahl aus "123" (16 Bit reicht nicht immer: unsigned long ohne Mult.) */
static unsigned long ti_num(const char *s, int *ok)
{
    unsigned long v = 0;
    *ok = *s >= '0' && *s <= '9';
    while (*s >= '0' && *s <= '9')
        v = (v << 3) + (v << 1) + (unsigned)(*s++ - '0');
    if (*s && *s != ' ')
        *ok = 0;
    return v;
}

#endif

#ifdef TI_FILE
/* Datei oder "-" (Standardeingabe) oeffnen; -> Handle oder FAPI_ERROR */
static unsigned ti_file(const char *name)
{
    if (name[0] == '-' && !name[1])
        return STDIN;
    return DosOpen(name);
}

#endif

#endif
