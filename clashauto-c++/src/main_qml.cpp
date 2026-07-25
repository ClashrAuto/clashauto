// Qt Quick (QML) 前端入口。复用既有 C++ 后端（AppConfig/CoreController/ClashService/
// SubscriptionStore/TrayController），仅通过 QmlBridge 这层薄胶水暴露给 QML。
// 用 QApplication（而非 QGuiApplication）：TrayController/QSystemTrayIcon 依赖 QtWidgets。
//
// 启动行为：正式版**默认自动拉起核心**（对齐旧 Widgets 版 MainWindow 的 startCore-on-launch）。
// 本地测 UI 时设环境变量 COAST_NO_AUTOSTART=1 可跳过自动起核心——本机常已有一个实例在跑，
// 避免端口(9090)/系统代理/TUN 冲突。ClashService::start() 只是只读轮询 REST API，始终安全。
#include "AppConfig.h"
#include "ClashService.h"
#include "CoreController.h"
#include "DeviceStore.h"
#include "SubscriptionStore.h"
#include "TrayController.h"
#include "qml/QmlBridge.h"
#include "qml/I18n.h"
#include "qml/SubscriptionsController.h"
#include "qml/LogModel.h"
#include "qml/AboutController.h"
#include "qml/SettingsController.h"
#include "qml/UpdateController.h"
#include "qml/DevicesController.h"
#include "net/LanGateway.h"
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include "net/GatewaySelfTest.h"
#endif

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // 渲染后端：走 Qt 默认的 RHI（Windows=D3D11 / macOS=Metal / Linux=OpenGL），**不再强制软件后端**。
    //
    // 曾经强制 QSGRendererInterface::Software（理由：UI 纯 2D 无 ShaderEffect、无 GPU 机器更轻、
    // 打包能省 opengl32sw/d3dcompiler ~24MB）。但软件后端只能走 QPainter 光栅化，代价是：
    //  - 圆角/圆点/开关这类小圆弧边缘只有 1 像素硬跳变（实测覆盖率 4→实心），肉眼就是毛边/锯齿；
    //    RHI 下同一个圆点是 2 像素渐变（2→7→实心）。且 antialiasing/layer.smooth/layer.textureSize
    //    超采样在软件后端全部无效（实测逐像素完全相同），QML 层面无解。
    //  - 文字只能走原生渲染，拿不到距离场渲染那种平滑字形。
    // 无 GPU 的机器不用担心：Windows 的 D3D11 有系统自带 WARP 兜底（本项目的无显卡 QEMU 虚机实测
    // 可跑），Linux 侧 .deb 已 Depends libgl1/libopengl0/libegl1（Mesa llvmpipe 软件 GL）。
    // 真遇到起不来的环境，仍可用环境变量退回：QT_QUICK_BACKEND=software（不再被代码覆盖）。
    //
    // 透明网关 headless 自测（Linux + COAST_GATEWAY_SELFTEST）：不建 GUI，跑 TAP+NetStack+假SOCKS
    // 后退出（配合 validate/gateway_selftest.sh）。用 offscreen 平台即可（无显示环境）。
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    if (qEnvironmentVariableIsSet("COAST_GATEWAY_SELFTEST"))
        return runGatewaySelfTest();
#endif

    // 用可定制的 Basic 样式：macOS 原生 Quick 样式不允许自定义控件 background（会报
    // "current style does not support customization"），本 app 全是自绘控件，必须 Basic。
    QQuickStyle::setStyle("Basic");
    QApplication::setApplicationName("Coast");
    QApplication::setOrganizationName("Coast");
#ifndef Q_OS_MACOS
    // mac 的 Dock/程序坞图标由 .app 包内的 icon.icns 提供（含 macOS 规范留白，尺寸与其它 App 一致）；
    // 若在此 setWindowIcon(全出血的 icon.ico) 会覆盖 Dock 图标、显得比别的图标大一圈，故 mac 不设。
    QApplication::setWindowIcon(QIcon(":/assets/icon.ico"));
#endif
    // 关闭主窗口不再退出程序：✕ 只隐藏窗口（mac 恒隐藏、留 Dock；Win/Linux 按「关闭到托盘」决定，
    // 见 Main.qml onClosing）。真正退出走托盘/菜单栏「退出程序」或 mac 的 Cmd+Q。
    QApplication::setQuitOnLastWindowClosed(false);
    QFontDatabase::addApplicationFont(":/assets/iconfont.ttf");   // family "iconfont"（logo/流量卡图标）
    QFontDatabase::addApplicationFont(":/assets/remixicon.ttf"); // family "remixicon"（Remix Icon 通用 UI 图标集，Apache-2.0）

    // —— 应用字体 ——
    // 全 UI 统一用 MiSans（不再用等宽字体）。**正文/标题一律不加粗**，层级只靠字号/颜色区分；
    // 唯一例外是「图标内的字母」（侧栏 logo 角标、托盘图标角标）——那里只有 20~30px，
    // 不加粗看不清。Semibold 就是给这些例外用的真实半粗体（不走合成粗体，避免发虚）。
    QFontDatabase::addApplicationFont(":/assets/fonts/MiSans-Regular.ttf");  // family "MiSans"
    QFontDatabase::addApplicationFont(":/assets/fonts/MiSans-Semibold.ttf"); // 归入 "MiSans"（typo family），weight=Semibold
    // 全局默认字体设为 MiSans：所有未显式指定的属性都由 QML Text 通过 QFont resolve 继承下来
    // （family / 字重 / hinting / styleStrategy 都算），字号仍由各 QML 的 font.pixelSize 决定。
    //
    // hinting = PreferNoHinting：**别把字形吸附到像素网格**。Windows 默认是全 hinting——笔画被
    // 阈值化成硬边、中文尤其显得又锐又硬（实测边缘中间调占比仅 78%）。关掉 hinting 后是 94%，
    // 笔画回到字体设计的形状、边缘有正常的灰阶过渡。（原生文字渲染下尤其明显；换 RHI 后端后
    // 文字走距离场渲染，这两项设置一并保留，退回软件后端时依然生效。）
    // styleStrategy = PreferAntialias：补上 NoHinting 单用时笔画偏淡的问题（实心像素 352→766），
    // 小字号也强制抗锯齿，不会在某些字号退化成锯齿硬边。
    {
        QFont uiFont = app.font();
        uiFont.setFamily(QStringLiteral("MiSans"));
        uiFont.setWeight(QFont::Normal);
        uiFont.setHintingPreference(QFont::PreferNoHinting);
        uiFont.setStyleStrategy(QFont::PreferAntialias);
        app.setFont(uiFont);
    }

    // 与 Widgets 版 MainWindow 相同的后端装配（AppConfigLoader 载入配置；资源已内嵌 qrc，不再找 Clashr-Auto）。
    AppConfig config = AppConfigLoader::load();
    auto *core = new CoreController(config, &app);
    auto *clash = new ClashService(&app);
    auto *subs = new SubscriptionStore(config, &app);
    // TrayController 用 MainWindow* 做 show/raise；QML 版无 MainWindow，传 nullptr，改用它的
    // openWindowRequested 信号重开 QML 主窗（见下方连接）。托盘图标 + 流量 + 三个 toggle 信号仍可用。
    auto *tray = new TrayController(nullptr, &app);

    clash->setEndpoint(config.host, config.uiPort);
    clash->setMixedPort(config.mixedPort);
    clash->setSecret(config.secret);
    clash->setClearConnectionsOnSwitch(config.clearConnections);

    QmlBridge bridge(&config, core, clash, subs, &app);

    // 各页面适配器（薄胶水，复用后端；堆分配交给 app 清理，避免栈对象与 QObject 父子重复析构）。
    auto *subsController = new SubscriptionsController(subs, core, &app);
    auto *logModel = new LogModel(core, clash, &app);
    auto *about = new AboutController(config, &app);
    auto *settingsCtrl = new SettingsController(core, clash, &app);
    auto *updateCtrl = new UpdateController(config, core, &app);
    // 设备页：局域网发现台账 + 透明网关 + 控制器（自持连接轮询/热更新；页面显隐时开停）。
    auto *deviceStore = new DeviceStore(config.configDir, &app);
    auto *lanGateway = new LanGateway(&app);
    // 启动即先还原上次异常退出遗留的 ARP 投毒（panic-restore），避免被劫持设备一直断网。
    lanGateway->recoverFromCrash();
    auto *devicesCtrl = new DevicesController(deviceStore, clash, core, lanGateway, &app);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, deviceStore, &DeviceStore::save);
    // 退出必须可靠还原所有被劫持设备的 ARP（否则设备断网）。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, lanGateway, &LanGateway::disableAll);
    // 新设备提醒（蹭网检测）：首轮扫描后发现新设备 → 托盘气泡。
    QObject::connect(devicesCtrl, &DevicesController::newDeviceFound, tray,
                     [tray](const QString &name) {
                         tray->notify(QCoreApplication::translate("Devices", "发现新设备"), name);
                     });

    // 托盘 toggle → 后端（与 Widgets 版契约一致）。核心生命周期仍由用户显式触发。
    // 增强(TUN) 走 bridge.toggleTun（而非直连 core）：Windows 上开启且非提权时先弹 UAC 提权重启，
    // 与页脚「增强」开关同一入口。否则托盘开增强会绕过提权、建不了 wintun 网卡。
    QObject::connect(tray, &TrayController::toggleCoreRequested, core, &CoreController::toggleCore);
    QObject::connect(tray, &TrayController::toggleProxyRequested, core, &CoreController::toggleProxy);
    QObject::connect(tray, &TrayController::toggleTunRequested, &bridge, &QmlBridge::toggleTun);
    QObject::connect(clash, &ClashService::trafficUpdated, tray, &TrayController::setTraffic);
    QObject::connect(core, &CoreController::statusChanged, tray, &TrayController::setStatus);

    // 节点切换通知（config.note「切换通知」开时）：QmlBridge 检出活动节点变化 → 托盘气泡。
    QObject::connect(&bridge, &QmlBridge::notifyRequested, tray, &TrayController::notify);
    // 用户把「切换通知」关→开：重注册系统通知（重显托盘图标），尝试恢复失效的通知注册。
    QObject::connect(&bridge, &QmlBridge::reinitNotificationsRequested, tray,
                     &TrayController::reinitForNotifications);

    // 退出时停核心、还原系统代理（对齐 Widgets 版 aboutToQuit）。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, core, &CoreController::stopCore);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bridge", &bridge);
    engine.rootContext()->setContextProperty("nodeModel", bridge.nodeModel());
    engine.rootContext()->setContextProperty("subs", subsController);
    engine.rootContext()->setContextProperty("logModel", logModel);
    engine.rootContext()->setContextProperty("about", about);
    engine.rootContext()->setContextProperty("settings", settingsCtrl);
    engine.rootContext()->setContextProperty("updater", updateCtrl);
    engine.rootContext()->setContextProperty("devices", devicesCtrl);

    // 界面语言（i18n）：按 config.language 在「加载 QML 前」装好翻译器，首帧即是目标语言（zh-CN 为默认，
    // 不装翻译器 → 用中文源串；en-US 装英文表）。设置页切语言经 languageChangeRequested → 运行时 retranslate。
    auto *i18n = new I18n(&engine, &app);
    // 跟随系统语言(config.autoLanguage)开→按系统区域选；关→用手选 language。
    i18n->setLanguage(config.autoLanguage ? I18n::systemLanguage() : config.language);
    tray->retranslate(); // 托盘菜单在翻译器安装前就已构造（Qt 托盘）；装好后刷成目标语言
    QObject::connect(settingsCtrl, &SettingsController::languageChangeRequested, i18n, &I18n::setLanguage);
    QObject::connect(settingsCtrl, &SettingsController::languageChangeRequested, tray, &TrayController::retranslate);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("ClashAuto", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    // 主窗（ApplicationWindow 根即 QQuickWindow）：托盘/菜单栏「控制面板」据此重开。
    // 关闭主窗只是 hide（见 Main.qml onClosing），这里负责再把它显示出来。mac 上窗口再显示时
    // Main.qml 的 onVisibleChanged 会经 bridge.setMacDockVisible(true) 让 Dock 图标回来。
    auto *rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QObject::connect(tray, &TrayController::openWindowRequested, rootWindow, [rootWindow] {
        if (rootWindow) {
            rootWindow->show();
            rootWindow->raise();
            rootWindow->requestActivate();
        }
    });

    clash->start(); // 只读轮询 REST API

#if defined(Q_OS_WIN)
    // 由页脚/托盘「增强」以管理员身份重启而来（--tun-elevated）：等旧(非提权)实例硬杀核心、
    // 释放 9090/系统代理并退出后，本(提权)实例带 TUN 冷启动核心（对齐 Widgets 版）。
    const bool tunElevated = app.arguments().contains(QStringLiteral("--tun-elevated"));
#else
    const bool tunElevated = false;
#endif

    if (tunElevated) {
        QTimer::singleShot(1000, core, [core] {
            if (core && !core->isRunning()) {
                core->setTunEnabled(true);
                core->startCore();
            }
        });
    } else if (qEnvironmentVariableIsEmpty("COAST_NO_AUTOSTART")) {
        // 正式版：启动即自动拉起核心（复刻旧版 MainWindow：有内核就起）。延时 600ms 让 UI 先就绪。
        // Windows 上若上次开着增强(TUN)而当前非提权，autoStartCore 会先按需提权重启（见 QmlBridge）。
        // 本地测 UI 时 COAST_NO_AUTOSTART=1 跳过（本机已有实例，避免端口/代理/TUN 冲突）。
        QTimer::singleShot(600, &bridge, &QmlBridge::autoStartCore);
    }
    return app.exec();
}
