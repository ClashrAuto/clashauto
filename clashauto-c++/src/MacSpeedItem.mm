#include "MacSpeedItem.h"

#import <AppKit/AppKit.h>
#include <QString>

// 菜单/点击的 ObjC 目标：转发到 C++ 回调。
@interface ClashTrayTarget : NSObject
@property(nonatomic, assign) MacTrayHandlers handlers;
@end

@implementation ClashTrayTarget
- (void)openWindow:(id)s { if (_handlers.openWindow) _handlers.openWindow(_handlers.ctx); }
- (void)toggleCore:(id)s { if (_handlers.toggleCore) _handlers.toggleCore(_handlers.ctx); }
- (void)toggleProxy:(id)s { if (_handlers.toggleProxy) _handlers.toggleProxy(_handlers.ctx); }
- (void)toggleTun:(id)s { if (_handlers.toggleTun) _handlers.toggleTun(_handlers.ctx); }
- (void)quit:(id)s { if (_handlers.quit) _handlers.quit(_handlers.ctx); }
@end

static NSStatusItem *g_item = nil;
static ClashTrayTarget *g_target = nil;
static NSMenuItem *g_upItem = nil, *g_downItem = nil;
static NSMenuItem *g_coreItem = nil, *g_proxyItem = nil, *g_tunItem = nil;
static bool g_core = false;
static NSString *g_upText = @"0 B/s";
static NSString *g_downText = @"0 B/s";
static NSImage *g_icon = nil; // 由 Qt 传入的托盘图标（:/assets/icon.ico 转 PNG）

// 取托盘图标：优先 Qt 传入的，其次 App 图标，都没有则返回 nil（redraw 会画灰块占位）。
static NSImage *trayIcon()
{
    if (g_icon && g_icon.size.width > 0) {
        return g_icon;
    }
    NSImage *appIcon = [NSApp applicationIconImage];
    return (appIcon && appIcon.size.width > 0) ? appIcon : nil;
}

// 把「图标 + 两行速率」画进一张恰好等于菜单栏厚度的图。图标固定在最左、离文字 3 个字符，
// 文字区定宽右对齐——数字长短变化只在定宽区内右对齐，不推动图标，故不抖动。
static void redrawTray()
{
    if (!g_item || !g_item.button) {
        return;
    }
    NSStatusBarButton *button = g_item.button;
    const CGFloat thickness = [[NSStatusBar systemStatusBar] thickness];
    const CGFloat iconSide = floor(thickness) - 3.0; // 图标略小于厚度，上下留边
    NSImage *appIcon = trayIcon();

    // 画图标（有效就画，否则画个灰块占位——保证这一项永远可见，绝不透明消失）。
    void (^drawIcon)(void) = ^{
        const NSRect ir = NSMakeRect(2.0, (thickness - iconSide) / 2.0, iconSide, iconSide);
        if (appIcon) {
            [appIcon drawInRect:ir];
        } else {
            [[NSColor systemGrayColor] set];
            [[NSBezierPath bezierPathWithRoundedRect:ir xRadius:3.0 yRadius:3.0] fill];
        }
    };

    if (!g_core) {
        // 核心未跑：只画图标（不占速率区，宽度收窄）。
        const CGFloat w = iconSide + 4.0;
        NSImage *img = [NSImage imageWithSize:NSMakeSize(w, thickness)
                                      flipped:NO
                               drawingHandler:^BOOL(NSRect /*r*/) {
            drawIcon();
            return YES;
        }];
        button.image = img;
        button.imagePosition = NSImageOnly;
        g_item.length = w;
        return;
    }

    NSFont *font = [NSFont menuBarFontOfSize:8.5]; // 两行自然行高 ~20pt，塞进 ~22pt 不裁切
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentRight;
    // 手绘图非 template（图标是彩色的），文字颜色需按菜单栏深/浅自己取。
    NSAppearanceName ap = [button.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    NSColor *textColor = [ap isEqualToString:NSAppearanceNameDarkAqua] ? [NSColor whiteColor]
                                                                       : [NSColor blackColor];
    NSDictionary *attrs = @{
        NSFontAttributeName : font,
        NSParagraphStyleAttributeName : para,
        NSForegroundColorAttributeName : textColor,
    };

    // 定宽文字区：按最宽模板值算宽度，右对齐 → 不随数字长短抖动。图标离文字 3 个字符。
    const CGFloat charW = ceil([@"0" sizeWithAttributes:attrs].width);
    const CGFloat gap = 3.0 * charW;
    const CGFloat textW = ceil([@"888.8 MB/s" sizeWithAttributes:attrs].width);
    NSString *text = [NSString stringWithFormat:@"%@\n%@", g_upText, g_downText];
    NSAttributedString *att = [[NSAttributedString alloc] initWithString:text attributes:attrs];
    const CGFloat totalW = iconSide + gap + textW + 4.0;

    NSImage *img = [NSImage imageWithSize:NSMakeSize(totalW, thickness)
                                  flipped:NO
                           drawingHandler:^BOOL(NSRect /*r*/) {
        drawIcon();
        const NSRect tr = NSMakeRect(iconSide + gap, (thickness - att.size.height) / 2.0, textW, att.size.height);
        [att drawInRect:tr];
        return YES;
    }];
    button.image = img;
    button.imagePosition = NSImageOnly;
    g_item.length = totalW;
}

void macTrayInstall(const MacTrayHandlers &handlers)
{
    if (g_item) {
        return;
    }
    g_target = [[ClashTrayTarget alloc] init];
    g_target.handlers = handlers;

    g_item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];

    NSMenu *menu = [[NSMenu alloc] init];
    menu.autoenablesItems = NO; // 自己控制启用态：UP/DOWN 行常灰，其余可点

    NSMenuItem *panel = [menu addItemWithTitle:@"控制面板" action:@selector(openWindow:) keyEquivalent:@""];
    panel.target = g_target;
    [menu addItem:[NSMenuItem separatorItem]];
    g_upItem = [menu addItemWithTitle:@"UP: 0 B/s" action:nil keyEquivalent:@""];
    g_upItem.enabled = NO;
    g_downItem = [menu addItemWithTitle:@"DOWN: 0 B/s" action:nil keyEquivalent:@""];
    g_downItem.enabled = NO;
    [menu addItem:[NSMenuItem separatorItem]];
    g_coreItem = [menu addItemWithTitle:@"启动核心" action:@selector(toggleCore:) keyEquivalent:@""];
    g_coreItem.target = g_target;
    g_proxyItem = [menu addItemWithTitle:@"打开网页代理" action:@selector(toggleProxy:) keyEquivalent:@""];
    g_proxyItem.target = g_target;
    g_tunItem = [menu addItemWithTitle:@"打开增强模式" action:@selector(toggleTun:) keyEquivalent:@""];
    g_tunItem.target = g_target;
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit = [menu addItemWithTitle:@"退出程序" action:@selector(quit:) keyEquivalent:@""];
    quit.target = g_target;

    // 设了 menu：点击图标即弹菜单，「控制面板」为首项负责打开主界面。
    g_item.menu = menu;

    redrawTray(); // 初始只画图标（核心未跑）
}

void macTraySetIconPng(const void *pngData, long len)
{
    if (!pngData || len <= 0) {
        return;
    }
    NSData *d = [NSData dataWithBytes:pngData length:(NSUInteger)len];
    NSImage *img = [[NSImage alloc] initWithData:d];
    if (img && img.size.width > 0) {
        g_icon = img;
        redrawTray();
    }
}

void macTraySetStatus(bool tun, bool proxy, bool core)
{
    g_core = core;
    if (g_coreItem) g_coreItem.title = core ? @"停止核心" : @"启动核心";
    if (g_proxyItem) g_proxyItem.title = proxy ? @"关闭网页代理" : @"打开网页代理";
    if (g_tunItem) g_tunItem.title = tun ? @"关闭增强模式" : @"打开增强模式";
    redrawTray(); // 核心开/停会切换「图标only ↔ 图标+速率」布局
}

void macTraySetSpeed(const QString &up, const QString &down)
{
    g_upText = up.toNSString();
    g_downText = down.toNSString();
    if (g_upItem) g_upItem.title = [NSString stringWithFormat:@"UP: %@", g_upText];
    if (g_downItem) g_downItem.title = [NSString stringWithFormat:@"DOWN: %@", g_downText];
    redrawTray();
}

void macTrayNotify(const QString &title, const QString &message)
{
    // NSUserNotification 虽被标记弃用，但无需额外授权/框架，作气泡提示足够。
    // 显式抑制弃用告警，避免万一开了 -Werror 导致编译失败。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSUserNotification *n = [[NSUserNotification alloc] init];
    n.title = title.toNSString();
    n.informativeText = message.toNSString();
    [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:n];
#pragma clang diagnostic pop
}

void macTrayRemove()
{
    if (g_item) {
        [[NSStatusBar systemStatusBar] removeStatusItem:g_item];
        g_item = nil;
    }
}
