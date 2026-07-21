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
    readonly property int sidebarWidth: 120
    readonly property int footerHeight: 38
    readonly property int inset: 5 // mac 上主内容离窗顶/右 5px，透出玻璃

    // —— 字体 ——
    // uiFont：正文/UI（MiSans）。已在 main_qml 里设为全局默认字体，故绝大多数 Text 无需显式指定；
    //         此常量供个别需强调「就是正文字体」的场景显式引用。
    // monoFont：代码/等宽（Sarasa Mono SC）——日志、连接列表、实时流量数字等技术/表格文本用它，
    //           等宽可避免数字每秒刷新时宽度抖动。iconFont：图标字体（logo/流量卡图标）。
    readonly property string uiFont: "MiSans"
    readonly property string monoFont: "Sarasa Mono SC"
    readonly property string iconFont: "iconfont"
}
