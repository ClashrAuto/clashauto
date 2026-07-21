#include "WinWindow.h"

#include <QColor>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

// 这些 DWM 属性在旧 SDK 头里可能未定义；显式补齐（值来自 dwmapi.h / Windows 11 SDK）。
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

// 把标题栏背景染成 bg（窗口壳色），标题文字按深浅取白/深灰保证可读，并让标题栏按钮
// 跟随深浅色主题。仅 Win11 22000+ 生效；旧系统上 DwmSetWindowAttribute 返回错误但无副作用。
void setWindowsCaptionColor(WId winId, const QColor &bg, bool dark)
{
    HWND hwnd = reinterpret_cast<HWND>(winId);
    if (!hwnd)
        return;

    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));

    COLORREF caption = RGB(bg.red(), bg.green(), bg.blue());
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));

    COLORREF text = dark ? RGB(255, 255, 255) : RGB(51, 51, 51);
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
}
