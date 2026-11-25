/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - OLECLI (OLE 1.0, Client-Seite) und die Registrierungs-
 * datenbank von SHELL (Windows 3.1, REG.DAT).
 *
 * Es gibt keine OLE-Server: Dokumente lassen sich anmelden (Write, Paint-
 * brush und Cardfile tun das bei jedem Oeffnen und Speichern), Objekte
 * aber weder anlegen noch laden. Was ein Objekt braucht, meldet
 * OLE_ERROR_GENERIC; die Programme zeigen dann einen leeren Platzhalter
 * bzw. lassen den Menuepunkt aus. Die Zwischenablage hat nie ein Objekt.
 * Die Registrierungsdatenbank ist leer (ERROR_BADKEY) - Write sucht dort
 * nur die OLE-Server.
 *
 * Parameterlisten nach der OLE-1.0-Schnittstelle (OLE.H des Windows 3.1
 * SDK): bei Pascal raeumt die Funktion den Stapel ab, die Zahl und Groesse
 * der Argumente muss stimmen, auch wenn sie nicht benutzt werden.
 */
#include "gui.h"

typedef DWORD LPOBJ;                    /* LPOLEOBJECT, LPOLECLIENT ...   */
#define OLE_OK              0
#define OLE_ERROR_FORMAT    12
#define OLE_ERROR_OBJECT    13
#define OLE_ERROR_GENERIC   19
#define OLE_ERROR_NAME      24          /* (nur der Vollstaendigkeit)     */

/* ---- Dokumente ---- */
static DWORD ndoc;
WORD API OleRegisterClientDoc(LPCSTR cls, LPCSTR doc, DWORD rsv, DWORD FAR *lh)
{
    (void)cls; (void)doc; (void)rsv;
    if (lh)
        *lh = ++ndoc;
    return OLE_OK;
}
WORD API OleRevokeClientDoc(DWORD lh) { (void)lh; return OLE_OK; }
WORD API OleRenameClientDoc(DWORD lh, LPCSTR name) { (void)lh; (void)name; return OLE_OK; }
WORD API OleRevertClientDoc(DWORD lh) { (void)lh; return OLE_OK; }
WORD API OleSavedClientDoc(DWORD lh) { (void)lh; return OLE_OK; }
WORD API OleEnumObjects(DWORD lh, LPOBJ FAR *o)
{
    (void)lh;
    if (o)
        *o = 0;                         /* keine (weiteren) Objekte      */
    return OLE_OK;
}

/* ---- Zwischenablage: nie ein Objekt ---- */
WORD API OleQueryLinkFromClip(LPCSTR prot, WORD render, WORD fmt)
{
    (void)prot; (void)render; (void)fmt;
    return OLE_ERROR_FORMAT;
}
WORD API OleQueryCreateFromClip(LPCSTR prot, WORD render, WORD fmt)
{
    (void)prot; (void)render; (void)fmt;
    return OLE_ERROR_FORMAT;
}
WORD API OleCreateLinkFromClip(LPCSTR prot, LPOBJ cl, DWORD lh, LPCSTR name,
                               LPOBJ FAR *o, WORD render, WORD fmt)
{
    (void)prot; (void)cl; (void)lh; (void)name; (void)render; (void)fmt;
    if (o) *o = 0;
    return OLE_ERROR_FORMAT;
}
WORD API OleCreateFromClip(LPCSTR prot, LPOBJ cl, DWORD lh, LPCSTR name,
                           LPOBJ FAR *o, WORD render, WORD fmt)
{
    (void)prot; (void)cl; (void)lh; (void)name; (void)render; (void)fmt;
    if (o) *o = 0;
    return OLE_ERROR_FORMAT;
}

/* ---- Objekte anlegen/laden: geht nicht ---- */
WORD API OleLoadFromStream(LPOBJ st, LPCSTR prot, LPOBJ cl, DWORD lh,
                           LPCSTR name, LPOBJ FAR *o)
{
    (void)st; (void)prot; (void)cl; (void)lh; (void)name;
    if (o) *o = 0;
    glog("OLE: eingebettetes Objekt nicht ladbar (kein OLE-Server)");
    return OLE_ERROR_GENERIC;
}
WORD API OleClone(LPOBJ ob, LPOBJ cl, DWORD lh, LPCSTR name, LPOBJ FAR *o)
{
    (void)ob; (void)cl; (void)lh; (void)name;
    if (o) *o = 0;
    return OLE_ERROR_GENERIC;
}
WORD API OleCopyFromLink(LPOBJ ob, LPCSTR prot, LPOBJ cl, DWORD lh, LPCSTR name,
                         LPOBJ FAR *o)
{
    (void)ob; (void)prot; (void)cl; (void)lh; (void)name;
    if (o) *o = 0;
    return OLE_ERROR_GENERIC;
}
WORD API OleObjectConvert(LPOBJ ob, LPCSTR prot, LPOBJ cl, DWORD lh, LPCSTR name,
                          LPOBJ FAR *o)
{
    (void)ob; (void)prot; (void)cl; (void)lh; (void)name;
    if (o) *o = 0;
    return OLE_ERROR_GENERIC;
}
WORD API OleCreate(LPCSTR prot, LPOBJ cl, LPCSTR cls, DWORD lh, LPCSTR name,
                   LPOBJ FAR *o, WORD render, WORD fmt)
{
    (void)prot; (void)cl; (void)cls; (void)lh; (void)name; (void)render; (void)fmt;
    if (o) *o = 0;
    return OLE_ERROR_GENERIC;
}
WORD API OleCreateFromTemplate(LPCSTR prot, LPOBJ cl, LPCSTR tpl, DWORD lh,
                               LPCSTR name, LPOBJ FAR *o, WORD render, WORD fmt)
{
    (void)prot; (void)cl; (void)tpl; (void)lh; (void)name; (void)render; (void)fmt;
    if (o) *o = 0;
    return OLE_ERROR_GENERIC;
}
WORD API OleCreateFromFile(LPCSTR prot, LPOBJ cl, LPCSTR cls, LPCSTR file,
                           DWORD lh, LPCSTR name, LPOBJ FAR *o, WORD render, WORD fmt)
{
    (void)prot; (void)cl; (void)cls; (void)file; (void)lh; (void)name;
    (void)render; (void)fmt;
    if (o) *o = 0;
    return OLE_ERROR_GENERIC;
}
WORD API OleCreateLinkFromFile(LPCSTR prot, LPOBJ cl, LPCSTR cls, LPCSTR file,
                               LPCSTR item, DWORD lh, LPCSTR name, LPOBJ FAR *o,
                               WORD render, WORD fmt)
{
    (void)prot; (void)cl; (void)cls; (void)file; (void)item; (void)lh; (void)name;
    (void)render; (void)fmt;
    if (o) *o = 0;
    return OLE_ERROR_GENERIC;
}

/* ---- Aufrufe auf ein Objekt: es gibt keine ---- */
WORD API OleDelete(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleRelease(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleCopyToClipboard(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleQueryOpen(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleUpdate(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleReconnect(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleClose(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleQueryOutOfDate(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleQueryReleaseStatus(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleQueryReleaseError(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }
WORD API OleQueryReleaseMethod(LPOBJ ob) { (void)ob; return OLE_ERROR_OBJECT; }

WORD API OleSaveToStream(LPOBJ ob, LPOBJ st) { (void)ob; (void)st; return OLE_ERROR_OBJECT; }
WORD API OleEqual(LPOBJ a, LPOBJ b) { (void)a; (void)b; return OLE_ERROR_OBJECT; }
WORD API OleQueryType(LPOBJ ob, DWORD FAR *t) { (void)ob; (void)t; return OLE_ERROR_OBJECT; }
WORD API OleSetHostNames(LPOBJ ob, LPCSTR c, LPCSTR o)
{
    (void)ob; (void)c; (void)o;
    return OLE_ERROR_OBJECT;
}
WORD API OleSetTargetDevice(LPOBJ ob, WORD hdev) { (void)ob; (void)hdev; return OLE_ERROR_OBJECT; }
WORD API OleSetBounds(LPOBJ ob, const RECT FAR *r) { (void)ob; (void)r; return OLE_ERROR_OBJECT; }
WORD API OleQueryBounds(LPOBJ ob, RECT FAR *r) { (void)ob; (void)r; return OLE_ERROR_OBJECT; }
WORD API OleDraw(LPOBJ ob, HDC dc, const RECT FAR *b, const RECT FAR *w, HDC fmt)
{
    (void)ob; (void)dc; (void)b; (void)w; (void)fmt;
    return OLE_ERROR_OBJECT;
}
WORD API OleActivate(LPOBJ ob, WORD verb, BOOL show, BOOL focus, HWND h,
                     const RECT FAR *r)
{
    (void)ob; (void)verb; (void)show; (void)focus; (void)h; (void)r;
    return OLE_ERROR_OBJECT;
}
WORD API OleGetLinkUpdateOptions(LPOBJ ob, WORD FAR *o) { (void)ob; (void)o; return OLE_ERROR_OBJECT; }
WORD API OleSetLinkUpdateOptions(LPOBJ ob, WORD o) { (void)ob; (void)o; return OLE_ERROR_OBJECT; }
WORD API OleEnumFormats(LPOBJ ob, WORD fmt) { (void)ob; (void)fmt; return 0; }
WORD API OleGetData(LPOBJ ob, WORD fmt, WORD FAR *h) { (void)ob; (void)fmt; (void)h; return OLE_ERROR_OBJECT; }
WORD API OleSetData(LPOBJ ob, WORD fmt, WORD h) { (void)ob; (void)fmt; (void)h; return OLE_ERROR_OBJECT; }
DWORD API OleQueryProtocol(LPOBJ ob, LPCSTR prot) { (void)ob; (void)prot; return 0; }
WORD API OleRename(LPOBJ ob, LPCSTR name) { (void)ob; (void)name; return OLE_ERROR_OBJECT; }
WORD API OleQuerySize(LPOBJ ob, DWORD FAR *n) { (void)ob; if (n) *n = 0; return OLE_ERROR_OBJECT; }
BOOL API OleIsDcMeta(HDC dc) { (void)dc; return 0; }

/* ---- SHELL: Registrierungsdatenbank (leer) ---- */
#define ERROR_BADKEY    2L
LONG API RegOpenKey(DWORD key, LPCSTR sub, DWORD FAR *res)
{
    (void)key; (void)sub;
    if (res) *res = 0;
    return ERROR_BADKEY;
}
LONG API RegCreateKey(DWORD key, LPCSTR sub, DWORD FAR *res)
{
    (void)key; (void)sub;
    if (res) *res = 0;
    return ERROR_BADKEY;
}
LONG API RegCloseKey(DWORD key) { (void)key; return 0; }
LONG API RegDeleteKey(DWORD key, LPCSTR sub) { (void)key; (void)sub; return ERROR_BADKEY; }
LONG API RegSetValue(DWORD key, LPCSTR sub, DWORD type, LPCSTR val, DWORD n)
{
    (void)key; (void)sub; (void)type; (void)val; (void)n;
    return ERROR_BADKEY;
}
LONG API RegQueryValue(DWORD key, LPCSTR sub, LPSTR val, LONG FAR *n)
{
    (void)key; (void)sub; (void)n;
    if (val) val[0] = 0;
    return ERROR_BADKEY;
}
LONG API RegEnumKey(DWORD key, DWORD i, LPSTR buf, DWORD n)
{
    (void)key; (void)i; (void)n;
    if (buf) buf[0] = 0;
    return ERROR_BADKEY;
}
