#include "MacWindow.h"

#import <AppKit/AppKit.h>

namespace {

// 幂等标记：毛玻璃层在窗框视图子树中的标识，重复调用只查找复用、不重复插入。
NSString *const kBlurViewId = @"ClashAutoBlurEffectView";

// winId() 在 macOS 上是 NSView*；由它取到承载它的 NSWindow。
// ARC 下禁止整数直接转 Obj-C 指针，先转 void* 再 __bridge（不转移所有权）。
NSWindow *windowForWId(WId winId)
{
    NSView *view = (__bridge NSView *)reinterpret_cast<void *>(static_cast<quintptr>(winId));
    return view.window;
}

} // namespace

void configureMacTitleBar(WId winId)
{
    NSWindow *window = windowForWId(winId);
    if (!window) {
        return; // 窗口尚未实体化（未 show）时无 NSWindow，稍后 show 后会再次调用
    }
    // 标题栏透明 + 隐藏标题文字 + 内容视图铺满整窗（含标题栏区域）：
    // 标题栏区域直接透出下方内容/毛玻璃；内容也随之上移贴顶。
    window.titlebarAppearsTransparent = YES;
    window.titleVisibility = NSWindowTitleHidden;
    window.styleMask |= NSWindowStyleMaskFullSizeContentView;
    // 拖窗改由 Qt 侧 MainWindow::mousePressEvent → startSystemMove() 实现（跨平台且对 Qt 的
    // 单 NSView 架构可靠）；movableByWindowBackground 对 Qt 基本无效，故不用。
    // 注意：这里不再给窗口填不透明底色——那会整面盖住 enableMacBlur() 插入的毛玻璃层。
    // 非不透明 + clear 底色统一由 enableMacBlur() 设置（它与本函数总是被一起调用）。
}

void enableMacBlur(WId winId, bool dark)
{
    NSWindow *window = windowForWId(winId);
    if (!window) {
        return; // 未 show 无 NSWindow；showEvent → applyTitleBarColor 会再次调用
    }

    // 玻璃要透出来的两个硬前提：窗口非不透明 + 底色全透明。
    // （Qt 侧还需 WA_TranslucentBackground，否则 backing store 以纯色清底照样盖住玻璃。）
    window.opaque = NO;
    window.backgroundColor = [NSColor clearColor];

    // 玻璃深浅跟随「应用主题」而非系统外观：应用有自己的黑/白主题设置，
    // 显式钉住窗口外观，避免系统浅色 + 应用深色时玻璃发白（反之亦然）。
    window.appearance =
        [NSAppearance appearanceNamed:(dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua)];

    // Qt 顶层 QNSView 即 NSWindow 的 contentView，无法在 contentView 内部再垫层，
    // 只能把效果视图插到窗框视图（contentView 的父视图）里、contentView 的下层。
    NSView *contentView = window.contentView;
    NSView *frameView = contentView.superview;
    if (!frameView) {
        return;
    }

    // 幂等：已插过就复用，只刷新属性（showEvent / 每次主题切换都会走到这里）。
    NSVisualEffectView *blur = nil;
    for (NSView *sub in frameView.subviews) {
        if ([sub isKindOfClass:[NSVisualEffectView class]]
            && [sub.identifier isEqualToString:kBlurViewId]) {
            blur = (NSVisualEffectView *)sub;
            break;
        }
    }
    if (!blur) {
        blur = [[NSVisualEffectView alloc] initWithFrame:frameView.bounds];
        blur.identifier = kBlurViewId;
        // 随窗框拉伸铺满整窗（含标题栏区域）；窗口圆角由窗框视图裁切，无需自管。
        blur.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [frameView addSubview:blur positioned:NSWindowBelow relativeTo:contentView];
    }
    // BehindWindow：模糊的是窗口后方的桌面/其他窗口（毛玻璃），不是窗口自身内容。
    blur.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    // Sidebar 材质：Finder/系统设置侧栏那种明显的磨砂质感，且是语义材质，
    // 自动随上面钉住的 Aqua/DarkAqua 外观出浅/深两种玻璃。
    blur.material = NSVisualEffectMaterialSidebar;
    // Active：窗口失焦时也保持磨砂，不退化成一块发灰的纯色底。
    blur.state = NSVisualEffectStateActive;
}
