import CoastKit
import SwiftUI

/// 状态页的「延迟」卡。严格对齐 Qt `StatusPage.qml` 的同名卡：
/// 图标 + 「延迟 · 直连」标题 + 直连大数 + 刷新按钮（探测中持续旋转），
/// 分隔线，再列「到路由 / DNS / 代理」三行，每行右侧是数值、下方是副标题。
struct LatencyCard: View {
    @Environment(Theme.self) private var theme

    let monitor: LatencyMonitor

    /// 数值的三态文案，与 Qt 的 `msText` 一致。
    ///
    /// 「—」和「…」必须分开：前者是**测过但没测到**（超时/不可达），后者是**还没测**。
    /// 混成一个的话，刚打开界面的那几秒会显示成「测不到」，用户以为网络坏了。
    private func text(_ ms: Int) -> String {
        if ms < 0 { return "—" }
        if ms == 0 { return "…" }
        return "\(ms) ms"
    }

    private var reading: LatencyProbe.Reading { monitor.reading }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .center, spacing: 12) {
                Image(systemName: "timer")
                    .font(.system(size: 28))
                    .foregroundStyle(theme.cardIcon)
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        // 标题是白色、不是主题色 —— 主题色留给数值旁边那些「可点」的东西。
                        Text("延迟".t)
                            .lineLimit(1)
                            .font(.system(size: 13))
                            .foregroundStyle(theme.textPrimary)
                        Text("直连".t)
                            .lineLimit(1)
                            .font(.system(size: 10))
                            .foregroundStyle(theme.textMuted)
                    }
                    Text(text(reading.directMs))
                        .font(.system(size: 24))
                        .foregroundStyle(theme.latencyColor(reading.directMs))
                }
                Spacer()
                // 手动重测：四项一起再来一轮（平时 10s 自动一轮）
                Button {
                    Task { await monitor.probe() }
                } label: {
                    Image(systemName: "arrow.clockwise")
                        .font(.system(size: 12))
                        .foregroundStyle(theme.accent)
                        .padding(6)
                        .rotationEffect(.degrees(monitor.probing ? 360 : 0))
                        .animation(monitor.probing
                                   ? .linear(duration: 0.9).repeatForever(autoreverses: false)
                                   : .default,
                                   value: monitor.probing)
                }
                .buttonStyle(.plain)
                .glassCapsule()
                .disabled(monitor.probing)
                .help("重新测一轮".t)
            }

            Divider().overlay(theme.divider)

            row("到路由".t, reading.routerMs, sub: monitor.gatewayIP)
            row("DNS", reading.dnsMs, sub: "")
            row("代理".t, reading.proxyMs, sub: reading.proxyName)
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
    }

    private func row(_ label: String, _ ms: Int, sub: String) -> some View {
        HStack(spacing: 6) {
            // 副标题跟在标题**右侧同一行**（Qt：「代理  Auto - US」），不是第二行 ——
            // 放第二行会让三行的高度参差，右侧数值也跟着错位。
            Text(label)
                .font(.system(size: 12))
                .foregroundStyle(theme.textSecondary)
            if !sub.isEmpty {
                Text(sub)
                    .font(.system(size: 10))
                    .foregroundStyle(theme.textMuted)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            Text(text(ms))
                .font(.system(size: 12))
                .foregroundStyle(theme.latencyColor(ms))
        }
    }
}
