#include "MacWindow.h"

#import <AppKit/AppKit.h>
#include <QColor>

void configureMacTitleBar(WId winId, const QColor &bg)
{
    // winId() 在 macOS 上是 NSView*；由它取到承载它的 NSWindow。
    // ARC 下禁止整数直接转 Obj-C 指针，先转 void* 再 __bridge（不转移所有权）。
    NSView *view = (__bridge NSView *)reinterpret_cast<void *>(static_cast<quintptr>(winId));
    NSWindow *window = view.window;
    if (!window) {
        return; // 窗口尚未实体化（未 show）时无 NSWindow，稍后 show 后会再次调用
    }
    // 标题栏透明 + 隐藏标题文字 + 内容视图铺满整窗（含标题栏区域）：
    // 于是标题栏背景透出下方 Qt 内容（#222/#eee），与背景同色；内容也随之上移贴顶。
    window.titlebarAppearsTransparent = YES;
    window.titleVisibility = NSWindowTitleHidden;
    window.styleMask |= NSWindowStyleMaskFullSizeContentView;
    // 显式给窗口底色，避免主题切换瞬间标题栏区域闪出系统灰。
    window.backgroundColor = [NSColor colorWithSRGBRed:bg.redF()
                                                 green:bg.greenF()
                                                  blue:bg.blueF()
                                                 alpha:1.0];
}
