#pragma once

// Windows 原生标题栏染色（Win11 22000+ 的 DWM 属性）：把标题栏背景染成窗口壳色，
// 使标题栏与应用背景融为一体（对齐 mac 的透明标题栏观感）。较旧 Windows 上 DWM 会
// 忽略这些属性 → 标题栏保持系统默认，不崩溃。实现见 WinWindow.cpp（仅 Windows 编译）。
#include <qwindowdefs.h> // WId

class QColor;

void setWindowsCaptionColor(WId winId, const QColor &bg, bool dark);
