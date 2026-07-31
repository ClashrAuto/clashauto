import AppKit
import SwiftUI

/// 给承载这个视图的 `NSWindow` 设**内容最小尺寸**。
///
/// ★ SwiftUI 的 `.frame(minWidth:minHeight:)` **不构成窗口的最小尺寸** —— 它只约束内容的
///   布局，窗口照样能被拖得更小，内容被裁掉。实测：更新窗上写着 `minWidth: 460`，
///   一句 `set size to {300, 300}` 就把它缩到了 300×300。
///
///   Qt 的每个窗都显式写了 `minimumWidth` / `minimumHeight`（主窗 640×430、更新窗 460×420、
///   设备详情 420×420、连接窗 480×320），macOS 上的对应物是 `NSWindow.contentMinSize`。
struct WindowMinSize: NSViewRepresentable {
    let width: CGFloat
    let height: CGFloat

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        // 窗口在下一轮 runloop 才挂上来，这里当场取是 nil。
        DispatchQueue.main.async { apply(to: view) }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) { apply(to: nsView) }

    private func apply(to view: NSView) {
        guard let window = view.window else { return }
        window.contentMinSize = NSSize(width: width, height: height)
        // 已经比下限还小的窗（例如上次退出时存下来的），当场顶回去。
        let size = window.contentLayoutRect.size
        if size.width < width || size.height < height {
            window.setContentSize(NSSize(width: max(size.width, width),
                                         height: max(size.height, height)))
        }
    }
}

extension View {
    /// 对齐 Qt 的 `minimumWidth` / `minimumHeight`。
    func windowMinSize(width: CGFloat, height: CGFloat) -> some View {
        background(WindowMinSize(width: width, height: height))
    }
}
