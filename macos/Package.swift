// swift-tools-version: 6.0
import PackageDescription

// Coast for macOS —— Qt/QML 端的 Swift 重写。
//
// 两个 target：
//   • CoastKit —— 与 UI 无关的后端（配置、订阅、核心进程、REST 客户端、存储）。
//                 对齐 clashauto-c++/src 下同名的 C++ 类，行为与磁盘格式保持一致。
//   • Coast    —— SwiftUI 可执行程序（窗口/托盘/页面）。
//
// 资源不进 SPM bundle：种子 yaml、Country.mmdb、字体、i18n 仍然只有一份，放在
// clashauto-c++/assets 下由 scripts/make_app.sh 拷进 .app/Contents/Resources；
// 开发期（swift run）由 Resources.swift 回退到仓库相对路径。见该文件注释。
let package = Package(
    name: "Coast",
    // macOS 26 起步：整套界面建立在 Liquid Glass 上（`VisualEffect.swift` 的
    // `.glassEffect` / `.buttonStyle(.glass)`），26 以下只能跑回落观感，与设计不是一回事。
    // 需要在老系统上用的走 **Qt 版 mac 包**（release 里的 `…-macos-universal-qt.dmg`，
    // 部署目标 13.0、Intel + Apple Silicon 胖包）——两条线并存，各自独立更新。
    //
    // 写字符串而不是 `.macOS(.v26)`：那个枚举成员要 swift-tools-version 6.2+ 才可见，
    // 而字符串重载从 5.0 就有 —— 用它就不必为了一个版本号把整份 manifest 的工具链下限抬上去。
    platforms: [.macOS("26.0")],
    products: [
        .executable(name: "Coast", targets: ["Coast"]),
        .executable(name: "com.yuehongsun.coast.helper", targets: ["CoastHelper"]),
        .library(name: "CoastKit", targets: ["CoastKit"]),
    ],
    targets: [
        // 应用与 helper 共享的 XPC 契约。单独成 target 是为了让 **以 root 运行的 helper**
        // 只链接这一小块，而不是整个 CoastKit —— root 进程里的每一行代码都是攻击面。
        .target(
            name: "CoastHelperProtocol",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .target(
            name: "CoastKit",
            dependencies: ["CoastHelperProtocol"],
            swiftSettings: [.swiftLanguageMode(.v5)],
            // 历史库直接用系统自带的 sqlite3（macOS 一直有），不引第三方 SQLite 包。
            linkerSettings: [.linkedLibrary("sqlite3")]
        ),
        // BPF ioctl 常量的 C shim（BIOCSETIF 等是 _IOW 宏，Swift 导入不进来）。
        .target(name: "CBPF", path: "Sources/CBPF", publicHeadersPath: "include"),
        // 特权 helper（root daemon）。产物名 = mach service 名 = launchd Label = codesign -i，
        // 四者必须一致，见 HelperConstants。
        .executableTarget(
            name: "CoastHelper",
            dependencies: ["CoastHelperProtocol", "CBPF"],
            swiftSettings: [.swiftLanguageMode(.v5)],
            // helper 是裸 Mach-O（不是 bundle），Info.plist 只能嵌进 __TEXT,__info_plist 段。
            linkerSettings: [.unsafeFlags([
                "-Xlinker", "-sectcreate",
                "-Xlinker", "__TEXT", "-Xlinker", "__info_plist",
                "-Xlinker", "Resources/HelperInfo.plist",
            ])]
        ),
        .executableTarget(
            name: "Coast",
            dependencies: ["CoastKit"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "CoastKitTests",
            dependencies: ["CoastKit", "CoastHelperProtocol"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
    ]
)
