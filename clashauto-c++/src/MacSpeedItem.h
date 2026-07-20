#pragma once

class QString;

// macOS 专用：独立于 Qt 托盘图标的一个原生 NSStatusItem，用两行 attributedTitle 显示
// 上/下速率（真·文字，清晰、按内容自适应宽度，不占图标位置、也不被整体缩放）。
void macSpeedItemInstall();                                        // 创建（初始隐藏）
void macSpeedItemSetVisible(bool visible);                         // 核心在跑才显示，停了收起
void macSpeedItemSetSpeed(const QString &up, const QString &down); // 更新两行（上=上传、下=下载）
void macSpeedItemRemove();                                         // 移除该菜单栏项
