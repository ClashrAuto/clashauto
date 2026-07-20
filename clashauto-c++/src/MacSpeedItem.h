#pragma once

class QString;

// 原生 macOS 托盘：把「App 图标 + 两行速率」合成在**同一个** NSStatusItem 里
//（图标固定在左、文字定宽右对齐，位置不随数字变化抖动），并自带右键菜单。
// 取代 Qt 的 QSystemTrayIcon（Qt 那项无法在图标旁显示清晰的两行文字，且两个独立项
// 无法稳定左右排序）。菜单点击/图标点击通过回调回到 C++（TrayController 转成 Qt 信号）。

typedef void (*MacTrayAction)(void *ctx);

struct MacTrayHandlers {
    void *ctx;
    MacTrayAction openWindow;  // 控制面板 / 点击图标
    MacTrayAction toggleCore;  // 启动/停止核心
    MacTrayAction toggleProxy; // 打开/关闭网页代理
    MacTrayAction toggleTun;   // 打开/关闭增强模式
    MacTrayAction quit;        // 退出程序
};

void macTrayInstall(const MacTrayHandlers &handlers);
void macTraySetStatus(bool tun, bool proxy, bool core);       // 更新菜单项文字 + 核心在跑才显示速率
void macTraySetSpeed(const QString &up, const QString &down); // 更新两行速率（图标右侧）+ 菜单 UP/DOWN 行
void macTrayNotify(const QString &title, const QString &message);
void macTrayRemove();
