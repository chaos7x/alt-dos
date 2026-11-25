/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - gemeinsame Kopfdatei des GUI-Prozesses (docs/GUI-WIN16.md).
 *
 * Aufbau wie Windows 3.x im Standard-Modus: ein AT-DOS-Task (GUI.EXE)
 * mit einer LDT, darin alle Win16-Programme als kooperative Tasks (Wechsel
 * nur in GetMessage/PeekMessage/Yield). KERNEL, USER und GDI sind Code
 * dieses Prozesses; Win16-Programme rufen sie ueber Importe, die der Lader
 * (w16ldr.c) gegen die Tabellen in exports.c aufloest.
 *
 * Uebersetzt mit -mm -zu: Code fern, Daten nah in DGROUP, SS != DS (die
 * Exporte laufen auf dem Stapel des Programms). Adressen lokaler Variablen
 * nie als nahe Zeiger weitergeben - Warnung W112 ist ein Fehler.
 */
#ifndef GUI_H
#define GUI_H

#ifndef GUI_MAIN
#define FAPI_NO_IMPL
#endif
#include "../apps/fapi.h"

#define FAR     __far
#define PASCAL  __pascal
#define API     __far __pascal __loadds            /* von Win16 gerufen */
#define APIC    __far __cdecl __loadds             /* ... mit cdecl (wsprintf) */
typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef unsigned long  DWORD;
typedef short          BOOL;
typedef WORD           HANDLE;
typedef WORD           HWND, HDC, HINSTANCE, HGDIOBJ, HMENU, HBRUSH, HPEN, HFONT, HBITMAP;
typedef long           LONG;
typedef char FAR      *LPSTR;
typedef const char FAR *LPCSTR;
typedef void FAR      *LPVOID;
typedef struct { short left, top, right, bottom; } RECT;
typedef RECT FAR      *LPRECT;
typedef struct { short x, y; } POINT;
typedef struct { HWND hwnd; WORD message, wParam; DWORD lParam; DWORD time; POINT pt; } MSG;
typedef MSG FAR       *LPMSG;
typedef long (FAR PASCAL *WNDPROC)(HWND, WORD, WORD, DWORD);

#define MKFP(s, o)      ((void FAR *)(((WORD)(s)) :> ((void __near *)(WORD)(o))))  /* wie MK_FP; (long)-Weg wurde bei s = 0 zu DS:o */
#define SELOF(p)        ((WORD)((unsigned long)(void FAR *)(p) >> 16))
#define OFFOF(p)        ((WORD)(unsigned long)(void FAR *)(p))
#define LOWORD(l)       ((WORD)(l))
#define HIWORD(l)       ((WORD)((DWORD)(l) >> 16))
#define MAKELONG(a, b)  ((DWORD)(WORD)(a) | ((DWORD)(WORD)(b) << 16))

/* ---- gcrt.asm ---- */
void __cdecl tsw(WORD FAR *save, WORD ss, WORD sp);
void __cdecl tstart(void);
void __cdecl int_entry(void);
void __cdecl w16_stub(void);
long __cdecl call16(void FAR *fn, WORD ds, WORD nw, WORD *w);
WORD __cdecl fapi_sel(WORD fn, WORD sel, WORD a1, WORD a2, WORD a3);
extern WORD ts_ax, ts_bx, ts_cx, ts_dx, ts_si, ts_di, ts_es, ts_ds, ts_ip, ts_cs;

/* ---- Hilfen (main.c) ---- */
void  fmemcpy(void FAR *d, const void FAR *s, WORD n);
void  fmemmove(void FAR *d, const void FAR *s, WORD n);
void  fmemset(void FAR *d, BYTE v, WORD n);
WORD  fstrlen(LPCSTR s);
void  fstrncpy(char FAR *d, LPCSTR s, WORD n);  /* mit 0, hoechstens n-1 */
int   fstricmp(LPCSTR a, LPCSTR b);
int   fstrcmp(LPCSTR a, LPCSTR b);
void  glog(const char *s);                       /* GUI.LOG */
void  glogn(const char *s, WORD v);              /* s + hex */
void  glogs(const char *s, LPCSTR t);
WORD  seg_new(WORD bytes);                       /* DosAllocSeg, 0 = Fehler */
void  seg_del(WORD sel);
extern WORD gui_ds;

/* ---- Win16-Tasks (krnl.c) ---- */
#define NWT     12
#define NQ      48              /* Nachrichtenschlange je Task (Ring) */
struct wtask {
    BYTE  used, state;          /* WT_* */
    WORD  ss, sp;               /* gesichert, solange er nicht laeuft */
    WORD  hinst;                /* = Autodata-Selektor (DGROUP)       */
    WORD  hmod;                 /* Modul (w16ldr.c), 1..             */
    WORD  psp;                  /* Selektor des PSP                  */
    WORD  env;                  /* Umgebung (PSP:2Ch)                */
    WORD  dta_s, dta_o;         /* INT 21h 1Ah                        */
    WORD  qh, qt;               /* Nachrichtenschlange (Ring)         */
    MSG   q[NQ];
    BYTE  quit;                 /* WM_QUIT steht an                   */
    WORD  quitcode;
    BYTE  waiting;              /* wartet auf Nachrichten             */
    /* gesendete Nachricht (SendMessage aus einem anderen Task)      */
    BYTE  sent;                 /* 1 steht an, 2 erledigt            */
    WORD  s_hwnd, s_msg, s_wp, s_from;
    DWORD s_lp, s_res;
    BYTE  wait_reply;
    char  name[9];
};
#define WT_RUN  1               /* bereit                             */
#define WT_DEAD 2
extern struct wtask wt[NWT];
extern int  cur_task;           /* -1 = GUI selbst (Hauptschleife)    */
extern WORD main_ss, main_sp;
void task_yield_to_main(void);  /* aus einem Task zur Hauptschleife   */
void task_run(int t);           /* aus der Hauptschleife in Task t    */
int  task_start(LPCSTR path, LPCSTR args, WORD show);   /* -> 0 ok */
void task_exit(int t, WORD code);
void task_reap(int t);
int  task_of_inst(WORD hinst);
void dos_int(void FAR *r);
void krnl_init(void);

/* ---- Lader (w16ldr.c) ---- */
#define NMOD    8
struct w16seg { WORD sel, flags, minalloc; };
struct w16mod {
    BYTE  used, refs;
    char  name[9];
    char  path[80];
    WORD  nseg, autodata, entcs, entip, ss, sp, heap, stack, flags;
    struct w16seg seg[24];
    WORD  ent_sel;              /* Kopie der Eintragstabelle (Segment)  */
    WORD  ent_len;
    WORD  res_sel, res_len;     /* Ressourcentabelle (Segment)          */
    WORD  rsh;                  /* Ausrichtung der Ressourcen           */
};
extern struct w16mod wm[NMOD];
int   w16_load(LPCSTR path);                     /* -> Modul 0.. oder -1 */
void  w16_free(int m);
DWORD w16_res_find(int m, LPCSTR type, LPCSTR name);  /* Lage<<?: s.u. */
WORD  w16_res_load(int m, DWORD id);             /* -> Selektor */
WORD  w16_res_size(DWORD id);
void  w16_missing(void);

/* ---- Exporte (exports.c) ---- */
void FAR *exp_find(LPCSTR mod, WORD ord, WORD FAR *isconst);
int   exp_builtin(LPCSTR mod);                    /* 1.. oder 0 */
void FAR *exp_ord(WORD i, WORD ord, WORD FAR *isconst);
/* lokale Halde eines Autodata (krnl.c), Handle = Stelle */
BOOL  lh_init(WORD ds, WORD start, WORD end);
WORD  lh_alloc(WORD ds, WORD n, WORD zero);
WORD  lh_ptr(WORD ds, WORD x);
WORD  lh_hnew(WORD ds, WORD n, WORD zero);
WORD  lh_realloc(WORD ds, WORD h, WORD n);
WORD  lh_size(WORD ds, WORD a);
void  lh_free(WORD ds, WORD a);
void  full_path(LPCSTR n, LPSTR out, WORD max);

/* ---- GDI (gdi.c) ---- */
#define SCR_W   640
#define SCR_H   480
void gdi_init(void);
void gdi_done(void);
void gdi_flush(void);                            /* Schatten -> Karte  */
void gdi_cursor(short x, short y);
void gdi_dirty(short x0, short y0, short x1, short y1);
void gfx_fill(short x0, short y0, short x1, short y1, BYTE c);   /* ohne Clip */
void gfx_text(short x, short y, LPCSTR s, WORD n, BYTE fg, BYTE bg, BYTE opaque);
WORD gfx_textw(LPCSTR s, WORD n);
#define FONT_H  16
#define FONT_W  8
BYTE rgb2c(DWORD rgb);                           /* -> VGA-Farbe 0-15 */
extern DWORD vga_rgb[16];
/* Zeichenflaeche eines DC: Ursprung + Clipping-Rechtecke (Bildschirm) */
#define NCLIP   24
struct clip { short n; RECT r[NCLIP]; };
void clip_window(HWND h, int client, struct clip FAR *c);
HDC  dc_get(HWND h, int client, int paint);
void dc_release(HDC dc);

/* ---- MDI (mdi.c) ---- */
LONG FAR PASCAL mdi_client_proc(HWND h, WORD m, WORD wp, DWORD lp);
int  mdi_is_kid(HWND h);
int  mdi_kid_active(HWND h);

/* ---- USER (user.c) ---- */
#define NWND    64
struct wnd {
    BYTE   used, visible, enabled, internal;
    WORD   cls;                  /* Klasse 1..                         */
    HWND   parent, owner;
    DWORD  style, exstyle;
    RECT   wr;                   /* Fenster, Bildschirmkoordinaten     */
    RECT   cr;                   /* Client, Bildschirmkoordinaten      */
    RECT   upd;                  /* ungueltiger Bereich (Client-Koord.) */
    BYTE   need_paint, need_ncpaint, erase;
    int    task;                 /* Besitzer                           */
    WORD   hinst;
    WORD   id;                   /* Kind-Kennung / Menue                */
    WNDPROC proc;
    char   title[64];
    BYTE   extra[16];            /* GetWindowWord/Long ab 0            */
    WORD   z;                    /* Rang (groesser = oben)             */
    HMENU  menu;                 /* Menueleiste (oberste Fenster)      */
    short  sb[2][3];             /* Bildlauf waagerecht/senkrecht: min, max, Stelle */
};
#define CAPH     20              /* Titelleiste                        */
#define MENUH    20              /* Menueleiste                        */
#define SBW      16              /* Bildlaufleiste                     */
#define MT_MOUSE 0x7F00          /* interne Nachrichten beim Menue     */
#define MT_KEY   0x7F01
extern struct wnd ww[NWND];
#define HW(i)    ((HWND)(0x4000u + ((i) << 2)))
#define WI(h)    ((((WORD)(h)) - 0x4000u) >> 2)
struct wnd *wptr(HWND h);
void user_init(void);
void user_input(void);           /* Maus/Tastatur abholen, zustellen   */
void user_paint_pending(void);   /* NC zeichnen, WM_PAINT anstossen     */
int  user_task_has_work(int t);
short wnd_border(struct wnd *w);
BOOL user_get_msg(MSG FAR *m, HWND filt, WORD lo, WORD hi, int remove, int wait);
void dc_rawfill(HDC h, short x0, short y0, short x1, short y1, BYTE c);
void dc_rawtext(HDC h, short x, short y, LPCSTR s, WORD n, BYTE fg);
HWND API CreateWindowEx(DWORD ex, LPCSTR cname, LPCSTR title, DWORD style,
                        short x, short y, short cx, short cy, HWND parent,
                        HMENU menu, WORD hinst, LPVOID param);
BOOL API DestroyWindow(HWND h);
void API InvalidateRect(HWND h, const RECT FAR *r, BOOL erase);
LONG API DefWindowProc(HWND h, WORD m, WORD wp, DWORD lp);
LONG API DispatchMessage(const MSG FAR *m);
void API PostQuitMessage(short code);
HWND API GetParent(HWND h);
HDC  API BeginPaint(HWND h, void FAR *ps);
BOOL API EndPaint(HWND h, void FAR *ps);
/* ---- Menues (menu.c) ---- */
void  menu_free(HMENU h);
void  menu_task_end(int t);
void  menu_bar_paint(HDC dc, struct wnd *w);
void  menu_bar_rect(struct wnd *w, RECT FAR *r);
short menu_bar_hit(struct wnd *w, short x, short y);
short menu_mnemonic(HMENU h, BYTE ch);
void  menu_track(HWND h, short bar, int bykey);
int   menu_input_mouse(short x, short y, BYTE btn);
int   menu_input_key(WORD k);
HMENU API LoadMenu(WORD hinst, LPCSTR name);
LONG FAR PASCAL menu_pop_proc(HWND h, WORD msg, WORD wp, DWORD lp);
LONG FAR PASCAL edit_proc(HWND h, WORD msg, WORD wp, DWORD lp);
/* ---- Dialoge, Knoepfe, Listen (dlg.c) ---- */
LONG FAR PASCAL button_proc(HWND h, WORD msg, WORD wp, DWORD lp);
LONG FAR PASCAL static_proc(HWND h, WORD msg, WORD wp, DWORD lp);
LONG FAR PASCAL dlg_proc(HWND h, WORD msg, WORD wp, DWORD lp);
LONG FAR PASCAL listbox_proc(HWND h, WORD msg, WORD wp, DWORD lp);
void  ui_bevel(HDC dc, short l, short t, short r, short b, int down);
typedef BOOL (*IDLGPROC)(HWND h, WORD m, WORD wp, DWORD lp);
HWND  dlg_new(HWND owner, LPCSTR title, short x, short y, short cx, short cy,
              IDLGPROC proc, WORD hinst);           /* interner Dialog     */
HWND  dlg_item(HWND d, LPCSTR cls, LPCSTR text, DWORD style, short x, short y,
               short cx, short cy, WORD id);
short dlg_run(HWND d, HWND owner);                   /* modal, -> EndDialog */
BOOL API IsDialogMessage(HWND d, MSG FAR *m);
BOOL API EndDialog(HWND d, short r);
HWND API GetDlgItem(HWND p, short id);
BOOL API SetDlgItemText(HWND d, short id, LPCSTR s);
short API GetDlgItemText(HWND d, short id, LPSTR s, short n);
LONG API SendDlgItemMessage(HWND p, short id, WORD m, WORD wp, DWORD lp);
BOOL API EnableWindow(HWND h, BOOL on);
BOOL API SetWindowText(HWND h, LPCSTR s);
short API GetWindowText(HWND h, LPSTR s, short n);
HWND API SetFocus(HWND h);
HWND API GetFocus(void);
int   wmatch(const char *p, const char FAR *n);
void user_task_end(int t);
long send_msg(HWND h, WORD m, WORD wp, DWORD lp);
BOOL post_msg(HWND h, WORD m, WORD wp, DWORD lp);
extern HWND desktop;
DWORD GetSysColor_c(WORD i);
void  MessageBeep_c(void);
int   timer_due_peek(int t);
BOOL  API ShowWindow(HWND h, short cmd);
BOOL  API ExtTextOut(HDC h, short x, short y, WORD opt, LPRECT r, LPCSTR s, WORD n, short FAR *dx);
short API SetROP2(HDC h, short r);
short API SetBkMode(HDC h, short m);
short API MessageBox(HWND owner, LPCSTR text, LPCSTR cap, WORD type);
BYTE  ansi2oem(BYTE c);

#endif
