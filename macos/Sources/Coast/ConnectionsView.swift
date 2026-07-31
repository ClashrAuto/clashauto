import CoastKit
import SwiftUI

/// 全部连接。**逐元素对齐** `qml/ConnectionsWindow.qml`：
/// 720×480、顶栏 Online(N)/Offline(N) 分段 + Search、卡片列表
/// （● 圆点 + `[type] host` + 进程/出口链/下载/上传四枚徽标 + ✕ 删除）。
///
/// Qt 那边是独立顶层窗；这里沿用本项目既有做法以 sheet 呈现（状态页连接卡点开）。
struct ConnectionsView: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme
    @Environment(\.dismiss) private var dismiss

    @State private var query = ""
    @State private var showOnline = true
    @State private var showOffline = true
    /// 右键「添加规则」用本行地址预填 value —— 与 Qt 的 `openForValue` 同义。
    @State private var addingRule: RuleDraft?

    /// 顶栏统一高度：两颗分段按钮与搜索框一致。
    private let toolbarHeight: CGFloat = 26

    private var rows: [ConnectionLedger.Entry] {
        state.connectionLedger.filtered(online: showOnline, offline: showOffline, query: query)
    }

    var body: some View {
        VStack(spacing: 5) {
            toolbar
                .padding(.top, 5)
                .padding(.horizontal, 5)

            ScrollView {
                LazyVStack(spacing: 1) {
                    ForEach(rows) { entry in
                        row(entry)
                    }
                }
            }
            .padding(.horizontal, 5)
            .padding(.bottom, 5)
        }
        .frame(width: 720, height: 480)
        .background(theme.card)
        // 每次打开清空账本，避免上次会话的离线连接残留（与 Qt 的 onVisibleChanged 一致）。
        .task { state.resetConnectionLedger() }
        .sheet(item: $addingRule) { draft in
            RuleEditorSheet(draft: draft) { saved in save(rule: saved) }
                .environment(state).environment(theme)
        }
    }

    // MARK: 顶栏

    private var toolbar: some View {
        HStack(spacing: 10) {
            segmentedToggles
            searchBox
        }
    }

    /// 分段按钮组：离线段左端**塞到在线段底下 3px**，中间无缝、只外侧圆角。
    /// 两段各自是独立开关（可以同时开），不是二选一。
    ///
    /// ★ 用 `HStack(spacing: -3)` 让两段**自己量自己**，不要去算文字宽度。
    ///   先前的写法是用 `NSString.size(withAttributes:)` 量一遍再把和式写死给 `ZStack`，
    ///   结果量出来比 SwiftUI 实际排版**偏窄** —— 截图上「Offline (0)」的计数被裁掉了，
    ///   只剩「Offline」。负间距 + `zIndex` 就能同时拿到「重叠 3px」和「在线段盖在上面」，
    ///   一个数都不用量。
    private var segmentedToggles: some View {
        HStack(spacing: -3) {
            segment(title: "Online (\(state.connectionLedger.onlineCount))",
                    on: showOnline, extraWidth: 24)
                .zIndex(1)                       // 在上：盖住离线段的左端圆角
                .onTapGesture { showOnline.toggle() }

            segment(title: "Offline (\(state.connectionLedger.offlineCount))",
                    on: showOffline, extraWidth: 27)
                .zIndex(0)
                .onTapGesture { showOffline.toggle() }
        }
        .fixedSize()
    }

    /// 一段的样子。开 = 品牌蓝，关 = 灰；两个颜色都是 QML 里的字面量，不走主题令牌
    /// （那边也是写死的）。
    private func segment(title: String, on: Bool, extraWidth: CGFloat) -> some View {
        Text(title)
            .font(.system(size: 12))
            .foregroundStyle(.white)
            .fixedSize()
            .padding(.horizontal, extraWidth / 2)
            .frame(height: toolbarHeight)
            .background {
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .fill(on ? Color(hex: 0x48_98_F8) : Color(hex: 0x90_93_99))
            }
            .contentShape(Rectangle())
    }

    /// Search：整块圆角，左侧「Search」前缀标签 + 一条 1px 竖线 + 输入框。
    private var searchBox: some View {
        HStack(spacing: 0) {
            Text("Search")
                .font(.system(size: 12))
                .foregroundStyle(theme.dark ? .white : Color(hex: 0x33_33_33))
                .padding(.horizontal, 10)
                .frame(maxHeight: .infinity)

            Rectangle()
                .fill(theme.dark ? Color(hex: 0x33_33_33) : Color(hex: 0xCC_CC_CC))
                .frame(width: 1)

            TextField("", text: $query)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .foregroundStyle(theme.dark ? .white : Color(hex: 0x33_33_33))
                .padding(.leading, 8)
        }
        .frame(height: toolbarHeight)
        .background {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .fill(theme.dark ? Color(hex: 0x44_44_44) : Color(hex: 0xEA_EA_EA))
        }
        .overlay {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .stroke(theme.dark ? Color(hex: 0x33_33_33) : Color(hex: 0xCC_CC_CC), lineWidth: 1)
        }
    }

    // MARK: 连接行

    /// 行高 42、半径 5、左右内距 10、元素间距 10。
    private func row(_ entry: ConnectionLedger.Entry) -> some View {
        let conn = entry.row
        let offline = entry.offline
        return HStack(spacing: 10) {
            Text("●")
                .font(.system(size: 10))
                .foregroundStyle(offline ? Color(hex: 0x99_99_99) : Color(hex: 0x67_C2_3A))

            Text("[\(conn.type)] \(conn.host)")
                .font(.system(size: 14))
                .foregroundStyle(offline ? Color(hex: 0x99_99_99)
                                 : (theme.dark ? Color(hex: 0xEE_EE_EE) : Color(hex: 0x33_33_33)))
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)

            // 发起连接的进程（核心的 find-process-mode 填的）。只有本机连接查得到，
            // 局域网设备的进程在别人机器上 —— 那种情况留空，不占位。
            if !conn.process.isEmpty {
                ConnBadge(symbol: nil, label: conn.process,
                          background: Color(red: 70 / 255, green: 110 / 255, blue: 168 / 255, opacity: 0.55),
                          foreground: .white)
            }
            ConnBadge(symbol: "arrow.triangle.branch", label: conn.chain,
                      background: Color.black.opacity(0.35), foreground: .white)
            ConnBadge(symbol: "arrow.down", label: conn.download > 0 ? Self.speed(conn.download) : "-",
                      background: Color.green.opacity(0.5), foreground: Color(hex: 0x33_33_33))
            ConnBadge(symbol: "arrow.up", label: conn.upload > 0 ? Self.speed(conn.upload) : "-",
                      background: Color.red.opacity(0.5), foreground: Color(hex: 0x33_33_33))

            CloseDot(disabled: offline) { state.closeConnection(id: conn.id) }
        }
        .padding(.horizontal, 10)
        .frame(height: 42)
        .background(theme.dark ? Color(hex: 0x22_22_22) : Color(hex: 0xEE_EE_EE))
        .clipShape(RoundedRectangle(cornerRadius: 5, style: .continuous))
        .contextMenu {
            Button("添加规则".t) {
                addingRule = RuleDraft(index: nil,
                                       rule: RulesStore.Rule(type: "DOMAIN-SUFFIX",
                                                             node: "DIRECT",
                                                             value: conn.host))
            }
        }
    }

    private func save(rule draft: RuleDraft) {
        let store = RulesStore()
        var loaded = store.load()
        loaded.rules.insert(draft.rule, at: 0)   // 新增前插，与设置页一致
        _ = store.save(rules: loaded.rules, areas: loaded.areas)
        Task { await state.controller.rebuildConfig() }
    }

    // MARK: 小工具

    /// 速率**不带空格**（`1.50KB`）—— 与 QML 的 `spd()` 一致。
    /// 行里四枚徽标横排，多一个空格就多挤掉一截 host。
    static func speed(_ value: Int64) -> String {
        var n = Double(max(0, value))
        let units = ["B", "KB", "MB", "GB", "TB"]
        var index = 0
        while n >= 1024, index < units.count - 1 {
            n /= 1024
            index += 1
        }
        return String(format: "%.2f%@", n, units[index])
    }

}

/// 连接行上的徽标：22 高、半径 5、宽 = 内容 + 12、图标与文字间距 4、字号 12。
private struct ConnBadge: View {
    let symbol: String?
    let label: String
    let background: Color
    let foreground: Color

    var body: some View {
        HStack(spacing: 4) {
            if let symbol {
                Image(systemName: symbol).font(.system(size: 12))
            }
            Text(label).font(.system(size: 12))
        }
        .foregroundStyle(foreground)
        .fixedSize()
        .padding(.horizontal, 6)
        .frame(height: 22)
        .background {
            RoundedRectangle(cornerRadius: 5, style: .continuous).fill(background)
        }
    }
}

/// ✕ 关闭这条连接：30×30、半径 3，悬停转红底白字；离线时置灰不可点
/// （连接已经断了，再发一次关闭没有意义）。
private struct CloseDot: View {
    @Environment(Theme.self) private var theme
    let disabled: Bool
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text("✕")
                .font(.system(size: 12))
                .foregroundStyle(disabled ? (theme.dark ? Color(hex: 0x66_66_66) : Color(hex: 0xCC_CC_CC))
                                 : (hovering ? .white : Color(hex: 0xF5_6C_6C)))
                .frame(width: 30, height: 30)
                .background {
                    RoundedRectangle(cornerRadius: 3, style: .continuous)
                        .fill(hovering && !disabled ? Color(hex: 0xF5_6C_6C)
                              : (theme.dark ? Color(hex: 0x33_33_33) : Color(hex: 0xEE_EE_EE)))
                }
                .overlay {
                    if !theme.dark {
                        RoundedRectangle(cornerRadius: 3, style: .continuous)
                            .stroke(Color(hex: 0xDD_DD_DD), lineWidth: 1)
                    }
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(disabled)
        .onHover { hovering = $0 }
    }
}
