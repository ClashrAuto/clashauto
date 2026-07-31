import CoastKit
import SwiftUI

/// 关于页：版本信息、路径、更新检查。对齐 `qml/AboutPage.qml` + `UpdateWindow.qml` 的核心功能。
struct AboutPage: View {
    @Environment(AppState.self) private var state
    @Environment(Theme.self) private var theme

    @State private var appRelease: UpdateChecker.Release?
    @State private var coreTag: String?
    @State private var checking = false
    @State private var message = ""

    private var appUpdateAvailable: Bool {
        guard let appRelease else { return false }
        return UpdateChecker.isNewer(remote: appRelease.tag, than: AppInfo.version)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                HStack(spacing: 12) {
                    Image(systemName: "globe")
                        .font(.system(size: 42))
                        .foregroundStyle(theme.accent)
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Coast").font(.system(size: 20, weight: .medium))
                            .foregroundStyle(theme.textPrimary)
                        Text("Ver: \(AppInfo.version)")
                            .font(.system(size: 12)).foregroundStyle(theme.textMuted)
                    }
                    Spacer()
                }

                group("更新".t) {
                    HStack(spacing: 8) {
                        Button(checking ? "检查中…".t : "检查更新".t) { Task { await check() } }
                            .disabled(checking)
                        if appUpdateAvailable, let release = appRelease {
                            UpdateBadge(text: "new")
                            Text("有新版 \(release.tag)")
                                .font(.system(size: 12)).foregroundStyle(theme.danger)
                            Button("前往下载".t) { openReleasePage() }
                        }
                        Spacer()
                    }
                    if let coreTag {
                        Text("内核最新版：\(coreTag)（在「设置 → 更新」里下载）")
                            .font(.system(size: 11)).foregroundStyle(theme.textMuted)
                    }
                    if state.config.receiveBeta {
                        Text("已开启「接收测试版」，检查范围包含 prerelease".t)
                            .font(.system(size: 10)).foregroundStyle(theme.textMuted)
                    }
                    if !message.isEmpty {
                        Text(message).font(.system(size: 11)).foregroundStyle(theme.textMuted)
                    }
                }

                if let release = appRelease, !release.notes.isEmpty {
                    group("更新说明（\(release.tag)）") {
                        Text(release.notes)
                            .font(.system(size: 11))
                            .foregroundStyle(theme.textSecondary)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }

                group("路径".t) {
                    pathRow("数据目录".t, AppPaths.userDir.path)
                    pathRow("配置目录".t, AppPaths.configDir.path)
                    pathRow("内核".t, AppPaths.coreExecutable.path)
                }
            }
            .padding(14)
        }
        .task { await check() }
    }

    private func group<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.system(size: 12, weight: .medium)).foregroundStyle(theme.textMuted)
            content()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(theme.metricBg)
        .clipShape(RoundedRectangle(cornerRadius: theme.radius, style: .continuous))
    }

    private func pathRow(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(label)
                .font(.system(size: 11)).foregroundStyle(theme.textMuted)
                .frame(width: 64, alignment: .leading)
            Text(value)
                .font(.system(size: 11))
                .foregroundStyle(theme.textSecondary)
                .textSelection(.enabled)
                .lineLimit(1).truncationMode(.middle)
            Button {
                NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: value)
            } label: {
                Image(systemName: "folder")
            }
            .buttonStyle(.borderless)
        }
    }

    private func check() async {
        checking = true
        defer { checking = false }
        let checker = UpdateChecker(includePrerelease: state.config.receiveBeta,
                                    proxyPort: state.controller.isCoreRunning ? state.config.mixedPort : nil)
        do {
            appRelease = try await checker.latestAppRelease()
            coreTag = try await checker.latestCoreTag()
            message = appUpdateAvailable ? "" : "已是最新版本".t
        } catch {
            message = "检查更新失败：\(error.localizedDescription)"
        }
    }

    /// 只打开发布页让用户自己下，**不做自动下载安装**。
    /// 自动替换 .app 需要处理签名、公证与「正在运行的自己」，风险远大于省下的那两步点击。
    private func openReleasePage() {
        guard let release = appRelease,
              let url = URL(string: "https://github.com/ClashrAuto/clashauto/releases/tag/\(release.tag)")
        else { return }
        NSWorkspace.shared.open(url)
    }
}
