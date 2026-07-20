#include "MacSpeedItem.h"

#import <AppKit/AppKit.h>
#include <QString>

// 单例：整个进程共用一个速率菜单栏项，进程退出时随之消失。
static NSStatusItem *g_speedItem = nil;

void macSpeedItemInstall()
{
    if (g_speedItem) {
        return;
    }
    // 独立于 Qt 托盘图标的另一项（图标仍由 Qt 那项负责）。安装时机要在 Qt 图标 show() 之后，
    // 这样它比图标「更晚创建」→ 排在图标右侧（菜单栏按创建先后从左到右排）。
    g_speedItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    g_speedItem.visible = NO; // 核心未起时不显示
}

void macSpeedItemSetVisible(bool visible)
{
    if (g_speedItem) {
        g_speedItem.visible = visible ? YES : NO;
    }
}

void macSpeedItemSetSpeed(const QString &up, const QString &down)
{
    if (!g_speedItem) {
        return;
    }
    // 菜单栏可用厚度（~22pt）。两行文字画进一张这么高的图里，自己算垂直居中——不靠按钮的
    // 多行自动居中（那个会把两行顶得偏上甚至裁切）。
    const CGFloat thickness = [[NSStatusBar systemStatusBar] thickness];
    // 8.5pt：两行按自然行高共 ~20pt，塞进 ~22pt 不裁切；不强设行高，避免压行裁掉字。
    NSFont *font = [NSFont menuBarFontOfSize:8.5];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentRight;
    // 画成黑色即可：template 图只看 alpha，系统会按菜单栏深/浅自动上色。
    NSDictionary *attrs = @{
        NSFontAttributeName : font,
        NSParagraphStyleAttributeName : para,
        NSForegroundColorAttributeName : [NSColor blackColor],
    };
    NSString *text = [NSString stringWithFormat:@"%@\n%@", up.toNSString(), down.toNSString()];
    NSAttributedString *att = [[NSAttributedString alloc] initWithString:text attributes:attrs];
    const NSSize ts = att.size;         // 宽=较长一行，高=两行总高
    const CGFloat w = ceil(ts.width) + 4;

    NSImage *img = [NSImage imageWithSize:NSMakeSize(w, thickness)
                                  flipped:NO
                           drawingHandler:^BOOL(NSRect /*dstRect*/) {
        // 文本框垂直居中：底部留 (厚度-文本高)/2；右对齐留 2pt 右边距。
        const NSRect box = NSMakeRect(0, (thickness - ts.height) / 2.0, w - 2, ts.height);
        [att drawInRect:box];
        return YES;
    }];
    [img setTemplate:YES]; // 随菜单栏深/浅自适应上色（.mm 里 template 是 C++ 关键字，用 setter）

    g_speedItem.button.image = img;
    g_speedItem.button.imagePosition = NSImageOnly;
    g_speedItem.length = w;
}

void macSpeedItemRemove()
{
    if (g_speedItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:g_speedItem];
        g_speedItem = nil;
    }
}
