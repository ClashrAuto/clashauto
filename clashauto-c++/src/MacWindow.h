#pragma once

#include <qwindowdefs.h> // WId

class QColor;

// macOS 专用：把系统标题栏做成「透明 + 内容延伸到顶」，标题栏区域直接透出窗口背景色，
// 从而与内容同色；同时内容上移，页面顶部贴近窗口上边缘（仅剩各自布局的 ~10px 内边距）。
// 每次主题变化重复调用即可（styleMask 用 OR，幂等；backgroundColor 随主题更新）。
void configureMacTitleBar(WId winId, const QColor &bg);
