#pragma once

#include <qwindowdefs.h> // WId

// macOS 专用：把系统标题栏做成「透明 + 内容延伸到顶」，标题栏文字隐藏、内容铺满整窗
// （含标题栏区域）。窗口底色不再填不透明纯色——透明底交给 enableMacBlur() 的毛玻璃层，
// 标题栏区域直接透出玻璃/内容。每次主题变化重复调用即可（styleMask 用 OR，幂等）。
void configureMacTitleBar(WId winId);

// macOS 专用：整窗毛玻璃。在窗框视图（contentView 的父视图）里、Qt 内容视图的下层插入
// 一块铺满整窗的 NSVisualEffectView（blendingMode=BehindWindow，桌面/后方窗口被模糊后
// 透进来），并把 NSWindow 设为非不透明 + clear 底色——Qt 侧配合 WA_TranslucentBackground，
// 凡未被不透明控件（如 #rightPane 内容卡）覆盖的区域都露出玻璃。
// dark 决定玻璃深浅：应用主题独立于系统外观，故通过 NSWindow.appearance 显式指定
// DarkAqua/Aqua，让材质跟随应用主题而非系统设置。幂等：重复调用只更新材质/外观，
// 不会重复插入效果视图（按 identifier 查重）。窗口未实体化（无 NSWindow）时静默返回。
void enableMacBlur(WId winId, bool dark);

// macOS 专用：点击 Dock 图标（尤其当无可见窗口时）重新打开主窗——Qt 默认不处理 reopen，
// 关闭主窗后点 Dock 图标不会有反应。这里把 applicationShouldHandleReopen:hasVisibleWindows:
// 补到 Qt 的 NSApp delegate 上（仅当其未自带该方法时，安全幂等），触发时在主线程回调 callback。
// callback 通常用于 show()/raise() 主窗。应在事件循环启动后调用（Qt 的 delegate 已就位）。
void installMacReopenHandler(void (*callback)(void));
