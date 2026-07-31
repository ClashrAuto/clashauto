import CoastKit
import SwiftUI

/// 局域网设备**浏览**页（只读）。
///
/// ★ 这里刻意**没有**「代理这台设备」开关。Qt 版有，但那个开关要生效，前提是把设备的流量
///   经二层投毒引到本机（`src/net/**` 那套网关），而它是否移植还没定
///   （见 `docs/gateway-evaluation.md`）。放一个点了没反应的开关，比不放更糟 ——
///   用户会以为自己配对了、然后困惑于为什么没生效。所以这里把话直接写在页面上。
struct DevicesPage: View {
    @Environment(Theme.self) private var theme

    @State private var devices: [LanBrowser.Device] = []
    @State private var scanning = false
    @State private var lastScan: Date?

    private let browser = LanBrowser()

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider().overlay(theme.divider)

            if devices.isEmpty {
                emptyState
            } else {
                List(devices) { device in
                    DeviceRow(device: device)
                        .listRowBackground(Color.clear)
                        .listRowSeparator(.hidden)
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
            }

            Divider().overlay(theme.divider)
            footnote
        }
        .task { await scan() }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Button {
                Task { await scan() }
            } label: {
                Label(scanning ? "扫描中…".t : "重新扫描".t, systemImage: "arrow.clockwise")
            }
            .disabled(scanning)

            if let lastScan {
                Text(lastScan, format: .dateTime.hour().minute().second())
                    .font(.system(size: 10)).foregroundStyle(theme.textMuted)
            }
            Spacer()
            Text("\(devices.count) 台")
                .font(.system(size: 11)).foregroundStyle(theme.textMuted)
        }
        .padding(10)
    }

    private var emptyState: some View {
        VStack(spacing: 6) {
            Image(systemName: "wifi.slash")
                .font(.system(size: 26)).foregroundStyle(theme.textMuted)
            Text(scanning ? "扫描中…".t : "邻居表里还没有设备".t)
                .foregroundStyle(theme.textMuted)
            Text("只读取系统已有的邻居表，所以只看得到最近通信过的设备".t)
                .font(.system(size: 11)).foregroundStyle(theme.textMuted)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    /// 把「为什么没有代理开关」明说出来，而不是留一片空白让人猜。
    private var footnote: some View {
        HStack(spacing: 6) {
            Image(systemName: "info.circle")
                .font(.system(size: 10)).foregroundStyle(theme.textMuted)
            Text("本页仅浏览。「代理局域网设备」需要二层网关支持，macOS 版尚未提供".t)
                .font(.system(size: 10)).foregroundStyle(theme.textMuted)
            Spacer()
        }
        .padding(.horizontal, 10)
        .frame(height: 26)
    }

    private func scan() async {
        scanning = true
        defer { scanning = false }
        devices = await browser.scan()
        lastScan = Date()
    }
}

private struct DeviceRow: View {
    @Environment(Theme.self) private var theme
    let device: LanBrowser.Device

    var body: some View {
        HStack(spacing: 10) {
            // 类型底色 + 白图标，与 Qt 版 Theme.deviceColor/deviceGlyph 同一套配色
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(theme.deviceColor(device.typeKey))
                .frame(width: 30, height: 30)
                .overlay {
                    Image(systemName: theme.deviceSymbol(device.typeKey))
                        .font(.system(size: 14))
                        .foregroundStyle(.white)
                }

            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 5) {
                    Text(device.displayName)
                        .font(.system(size: 13)).foregroundStyle(theme.textPrimary)
                        .lineLimit(1)
                    if device.isGateway {
                        Text("网关".t)
                            .font(.system(size: 9))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 4).padding(.vertical, 1)
                            .background(Capsule().fill(theme.accent))
                    }
                }
                Text("\(device.ip) · \(device.mac)\(device.interface.isEmpty ? "" : " · " + device.interface)")
                    .font(.system(size: 10).monospacedDigit())
                    .foregroundStyle(theme.textMuted)
                    .lineLimit(1)
            }

            Spacer(minLength: 8)

            if !device.vendor.isEmpty {
                Text(device.vendor)
                    .font(.system(size: 10)).foregroundStyle(theme.textMuted)
                    .lineLimit(1).truncationMode(.tail)
                    .frame(maxWidth: 180, alignment: .trailing)
            }
        }
        .padding(.horizontal, 10)
        .frame(height: 46)
        .background(theme.nodeRowBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
        .textSelection(.enabled)
    }
}
