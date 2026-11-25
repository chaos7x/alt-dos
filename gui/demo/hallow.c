/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Win16-Beispiel HALLOW.EXE (Fenster, Text, Rechteck,
 * MessageBox bei Klick). Uebersetzt mit Open Watcom fuer Windows 3.x,
 * ohne Laufzeitbibliothek (w16crt.asm, Importe aus gui/ordinals.txt).
 */
#include <windows.h>
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
    h = CreateWindow("Hallo", "Hallo Win16", WS_OVERLAPPEDWINDOW, 40, 40, 400, 200, NULL, NULL, hi, NULL);
    ShowWindow(h, show); UpdateWindow(h);
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return msg.wParam;
}
