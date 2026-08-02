pragma Singleton
import QtQuick

// 全局设计令牌（单例）。颜色随 `dark` 布尔在深/浅两套之间切换——所有 readonly 属性都是
// 依赖 dark 的绑定，故翻转 Theme.dark 即整窗换肤。取值对齐 Widgets 版 appStyle()/lightStyle()。
// 页面/组件通过 `import ClashAuto` 后以 `Theme.xxx` 访问。
QtObject {
    id: theme

    // 应用主题（真值来源）。启动时由 Main.qml 用 bridge.initialDark 初始化。
    property bool dark: true

    // —— 品牌色 ——
    readonly property color accent: "#4898f8"
    readonly property color accentStrong: dark ? "#83bdff" : "#1f6fd2"
    readonly property color danger: "#ff4d4f"

    // —— 结构面 ——
    readonly property color shell: dark ? "#222222" : "#eeeeee"      // 窗口壳 / 侧栏 / 页脚（mac 上透明露玻璃）
    readonly property color card: dark ? "#000000" : "#ffffff"       // 主内容卡 / 页脚开关底
    readonly property color metricBg: dark ? "#2a2a2a" : "#eeeeee"   // 流量指标卡
    readonly property color nodeRowBg: dark ? "#252525" : "#eeeeee"  // 节点行
    readonly property color nodeRowActive: Qt.rgba(72 / 255, 151 / 255, 248 / 255, 0.69)

    // —— 文本 ——
    readonly property color textPrimary: dark ? "#eeeeee" : "#111111"
    readonly property color textSecondary: dark ? "#cccccc" : "#333333"
    readonly property color textMuted: dark ? "#888888" : "#999999"
    readonly property color versionColor: "#666666"

    // —— 控件 ——
    readonly property color inputBg: dark ? "#444444" : "#eaeaea"
    readonly property color inputBorder: dark ? "#333333" : "#cccccc"
    readonly property color footerComboBg: dark ? "#111111" : "#ffffff"
    readonly property color switchTrackOff: dark ? "#666666" : "#ffffff"
    readonly property color scrollHandle: dark ? "#555555" : "#cccccc"
    readonly property color hover: dark ? "#3e3e3e" : "#d2d2d2"
    readonly property color divider: dark ? "#333333" : "#cccccc"

    // —— 度量（对齐 Widgets 版常量）——
    readonly property int radius: 5
    readonly property int sidebarWidth: 170
    readonly property int footerHeight: 38
    readonly property int inset: 5 // mac 上主内容离窗顶/右 5px，透出玻璃

    // —— 字体 ——
    // uiFont：正文/UI（MiSans）。已在 main_qml 里设为全局默认字体，故**所有** Text 都无需显式指定。
    //         此常量留给不继承应用默认字体的场景（Canvas 的 ctx.font 之类）；目前 QML 里没有这种
    //         用法——BandwidthChart 的文字曾经走 ctx.fillText，因性能问题已全部改成 QML Text，
    //         见该文件头部注释。要往 Canvas 里写字之前，先读那段。
    // iconFont：图标字体（logo/流量卡图标）。全 UI 统一 MiSans，不再用等宽字体。
    readonly property string uiFont: "MiSans"
    readonly property string iconFont: "iconfont"
    // 通用 UI 图标字体：Remix Icon（Apache-2.0，约 3200 图标）。用法：Text{ font.family: Theme.riFont; text: "" }。
    // 图标码点见 assets/remixicon.css（.ri-<名>:before content 即 \uXXXX）；常用：check-line EB7B、eye-line ECB5、
    // edit-line EC86、refresh-line F064、close-line EB99、add-line EA13、delete-bin-line EC2A。
    readonly property string riFont: "remixicon"

    // —— 工具函数（供各页复用，避免每处重复实现字节格式化）——
    // 人类可读字节：B / KB / MB / GB / TB（保留两位）。
    function fmtBytes(v) {
        var n = Number(v) || 0;
        if (n < 1024) return n.toFixed(0) + " B";
        var u = ["KB", "MB", "GB", "TB", "PB"];
        var i = -1;
        do { n /= 1024; ++i; } while (n >= 1024 && i < u.length - 1);
        return n.toFixed(2) + " " + u[i];
    }
    // 速率：字节/秒 → "1.20 MB/s"。
    function fmtRate(v) { return fmtBytes(v) + "/s"; }

    // 设备类型 → 头像底色（无图标字体依赖，纯色区分类型）。
    function deviceColor(typeKey) {
        switch (typeKey) {
        case "phone":    return "#4da13e";
        case "tablet":   return "#3e8fa1";
        case "computer": return "#466ea8";
        case "router":   return "#a86e43";
        case "tvbox":    return "#8a54c6";
        case "speaker":  return "#c65492";
        case "printer":  return "#5a6470";
        case "camera":   return "#a84343";
        case "game":     return "#43a897";
        case "nas":      return "#7a7a3e";
        case "iot":      return "#c69a54";
        default:         return "#888888";
        }
    }
    // 设备类型 → 头像图标（Remix Icon 码点，用 -fill 实心款：白图标压在类型底色上更清楚）。
    // 用法：Text{ font.family: Theme.riFont; text: Theme.deviceGlyph(k) }——务必带 riFont，
    // 正文字体（MiSans）里这些私用区码点是空的。码点由 assets/remixicon.ttf 的 post/cmap 表核对过：
    // smartphone-fill F159 / tablet-fill F1DF / computer-fill EBC9 / router-fill F09C / tv-fill F236 /
    // speaker-fill F172 / printer-fill F028 / camera-fill EB2E / gamepad-fill EDAA /
    // hard-drive-fill EDFA / home-wifi-fill EE30 / device-fill EC2D。
    function deviceGlyph(typeKey) {
        switch (typeKey) {
        case "phone":    return "\uF159"; // smartphone-fill
        case "tablet":   return "\uF1DF"; // tablet-fill
        case "computer": return "\uEBC9"; // computer-fill
        case "router":   return "\uF09C"; // router-fill
        case "tvbox":    return "\uF236"; // tv-fill
        case "speaker":  return "\uF172"; // speaker-fill
        case "printer":  return "\uF028"; // printer-fill
        case "camera":   return "\uEB2E"; // camera-fill
        case "game":     return "\uEDAA"; // gamepad-fill
        case "nas":      return "\uEDFA"; // hard-drive-fill
        case "iot":      return "\uEE30"; // home-wifi-fill（智能家居）
        default:         return "\uEC2D"; // device-fill（未知设备）
        }
    }
}
