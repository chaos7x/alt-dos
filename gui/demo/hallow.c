/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Win16-Beispiel HALLOW.EXE (Fenster, Text, Rechteck,
 * MessageBox bei Klick, Menue, Bildlaufleiste). Uebersetzt mit Open Watcom fuer Windows 3.x,
 * ohne Laufzeitbibliothek (w16crt.asm, Importe aus gui/ordinals.txt).
 */
#include <windows.h>
static int pos;
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    PAINTSTRUCT ps; HDC dc; RECT r;
    switch (m) {
    case WM_PAINT:
        dc = BeginPaint(h, &ps);
        GetClientRect(h, &r);
        TextOut(dc, 10, 10, "Hallo von Win16 auf AT-DOS!", 27);
        TextOut(dc, 10, 130, "Klick ins Fenster: MessageBox, Alt+F4: Ende", 43);
        Rectangle(dc, 10, 40, 200, 120);
        EndPaint(h, &ps);
        return 0;
    case WM_LBUTTONDOWN:
        MessageBox(h, "Klick!", "Hallo", MB_OK);
        return 0;
    case WM_COMMAND:
        if (w == 1)
            MessageBox(h, "Menue gewaehlt", "Info", MB_OK);
        if (w == 2)
            DestroyWindow(h);
        if (w == 3)
            CheckMenuItem(GetMenu(h), 3, (GetMenuState(GetMenu(h), 3, 0) & MF_CHECKED) ? MF_UNCHECKED : MF_CHECKED);
        return 0;
    case WM_VSCROLL:
        if (w == SB_LINEDOWN || w == SB_PAGEDOWN) pos += w == SB_LINEDOWN ? 1 : 10;
        if (w == SB_LINEUP || w == SB_PAGEUP) pos -= w == SB_LINEUP ? 1 : 10;
        if (w == SB_THUMBPOSITION || w == SB_THUMBTRACK) pos = LOWORD(l);
        if (pos < 0) pos = 0;
        if (pos > 100) pos = 100;
        SetScrollPos(h, SB_VERT, pos, TRUE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}
int PASCAL WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    WNDCLASS wc; MSG msg; HWND h;
    (void)cmd;
    if (!hp) {
        wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = WndProc;
        wc.cbClsExtra = wc.cbWndExtra = 0; wc.hInstance = hi;
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION); wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = GetStockObject(WHITE_BRUSH); wc.lpszMenuName = NULL; wc.lpszClassName = "Hallo";
        if (!RegisterClass(&wc)) return 0;
    }
    {
        HMENU m = CreateMenu(), p = CreatePopupMenu();
        AppendMenu(p, MF_STRING, 1, "&Info...");
        AppendMenu(p, MF_STRING | MF_CHECKED, 3, "&Haken");
        AppendMenu(p, MF_SEPARATOR, 0, NULL);
        AppendMenu(p, MF_STRING, 2, "&Beenden\tAlt+F4");
        AppendMenu(m, MF_POPUP, (UINT)p, "&Datei");
        AppendMenu(m, MF_STRING | MF_GRAYED, 9, "&Bearbeiten");
        h = CreateWindow("Hallo", "Hallo Win16", WS_OVERLAPPEDWINDOW | WS_VSCROLL, 40, 40, 400, 220, NULL, m, hi, NULL);
    }
    ShowWindow(h, show); UpdateWindow(h);
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return msg.wParam;
}
