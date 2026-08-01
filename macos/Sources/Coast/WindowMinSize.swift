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

    final class Coordinator {
        /// 「顶回下限」只做一次。
        ///
        /// ★ 第一版是每次 `updateNSView` 都查一遍并 `setContentSize`，结果**把窗口的高度
        ///   钉死在了下限上**：拖到 1000×700，`set size` 之后量出来仍是高 430 的内容。
        ///   原因是 `updateNSView` 一秒能跑很多次，只要有**任何一拍**量到的高度偏小
        ///   （SwiftUI 布局过程中很常见），就会把窗口按下去；之后每一拍都等于下限、
        ///   再也涨不回来。下限只是**下限**，不该在窗口活着的时候反复去动它。
        var didClamp = false
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        // 窗口在下一轮 runloop 才挂上来，这里当场取是 nil。
        DispatchQueue.main.async { apply(to: view, context.coordinator) }
        return view
    }

    // **不在 `updateNSView` 里动窗口**。窗口的尺寸约束只该设一次；
    // 每次布局都去写它，等于和 SwiftUI 自己的窗口尺寸推断来回打架。
    func updateNSView(_ nsView: NSView, context: Context) {}

    private func apply(to view: NSView, _ coordinator: Coordinator) {
        guard let window = view.window else { return }
        guard !coordinator.didClamp else { return }
        window.contentMinSize = NSSize(width: width, height: height)
        coordinator.didClamp = true
        // 上次退出时存下来的尺寸可能比下限还小（我自己的 `WindowRestore` 就恢复过一个
        // 460 宽的主窗帧），开窗那一刻顶回去 —— 只这一次。
        let size = window.contentRect(forFrameRect: window.frame).size
        if size.width < width || size.height < height {
            window.setContentSize(NSSize(width: max(size.width, width),
                                         height: max(size.height, height)))
        }
    }
}

/// 让承载这个视图的窗口**不参与 macOS 的窗口恢复**。
///
/// 更新 / 设备详情 / 连接三个窗都是「点出来的」——它们的内容来自当下的选中项或一次检查结果。
/// 系统恢复不管这些，进程上次是被杀掉的话，下次启动就把空壳摆出来（实测：启动后只剩一个
/// 空白的「设备详情」，主界面根本没建）。主窗不用它：主窗本来就该每次都在。
struct NonRestorableWindow: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async { view.window?.isRestorable = false }
        return view
    }
    func updateNSView(_ nsView: NSView, context: Context) {}
}

extension View {
    /// 对齐 Qt 的 `minimumWidth` / `minimumHeight`。
    func windowMinSize(width: CGFloat, height: CGFloat) -> some View {
        background(WindowMinSize(width: width, height: height))
    }

    func nonRestorableWindow() -> some View {
        background(NonRestorableWindow())
    }
}
