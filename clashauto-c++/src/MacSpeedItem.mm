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
    // 变长（按标题宽度自适应）。这是独立于 Qt 托盘图标的另一项——图标仍由 Qt 那项负责。
    g_speedItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    g_speedItem.button.imagePosition = NSNoImage; // 只显示文字
    g_speedItem.visible = NO;                     // 核心未起时不显示
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
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentRight;
    // 紧凑行高：两行各 ~9.5pt 共 ~19pt，塞进 ~22pt 菜单栏而不被裁切（默认行高会太大导致上行被切）。
    para.maximumLineHeight = 9.5;
    para.minimumLineHeight = 9.5;
    // 用菜单栏字体（会随「更大菜单栏」等辅助功能设置走），显式取 9pt 以容纳两行。
    NSFont *font = [NSFont menuBarFontOfSize:9.0];
    NSDictionary *attrs = @{
        NSFontAttributeName : font,
        NSParagraphStyleAttributeName : para,
        NSForegroundColorAttributeName : [NSColor labelColor], // 动态色，随菜单栏深/浅自适应
    };
    NSString *text = [NSString stringWithFormat:@"%@\n%@", up.toNSString(), down.toNSString()];
    g_speedItem.button.attributedTitle = [[NSAttributedString alloc] initWithString:text attributes:attrs];
}

void macSpeedItemRemove()
{
    if (g_speedItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:g_speedItem];
        g_speedItem = nil;
    }
}
