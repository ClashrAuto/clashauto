// Qt Quick (QML) 前端入口。复用既有 C++ 后端（AppConfig/CoreController/ClashService/
// SubscriptionStore/TrayController），仅通过 QmlBridge 这层薄胶水暴露给 QML。
// 用 QApplication（而非 QGuiApplication）：TrayController/QSystemTrayIcon 依赖 QtWidgets。
//
// 启动行为：正式版**默认自动拉起核心**（对齐旧 Widgets 版 MainWindow 的 startCore-on-launch）。
// 本地测 UI 时设环境变量 CLASHAUTO_NO_AUTOSTART=1 可跳过自动起核心——本机常已有一个实例在跑，
// 避免端口(9090)/系统代理/TUN 冲突。ClashService::start() 只是只读轮询 REST API，始终安全。
#include "AppConfig.h"
#include "ClashService.h"
#include "CoreController.h"
#include "SubscriptionStore.h"
#include "TrayController.h"
#include "qml/QmlBridge.h"
#include "qml/SubscriptionsController.h"
#include "qml/LogModel.h"
#include "qml/AboutController.h"
#include "qml/SettingsController.h"
#include "qml/UpdateController.h"

#include <QApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // 用可定制的 Basic 样式：macOS 原生 Quick 样式不允许自定义控件 background（会报
    // "current style does not support customization"），本 app 全是自绘控件，必须 Basic。
    QQuickStyle::setStyle("Basic");
    QApplication::setApplicationName("Clash Auto");
    QApplication::setOrganizationName("ClashAuto");
    QApplication::setWindowIcon(QIcon(":/assets/icon.ico"));
    QFontDatabase::addApplicationFont(":/assets/iconfont.ttf"); // family "iconfont"（logo/流量卡图标）

    // 与 Widgets 版 MainWindow 相同的后端装配（AppConfigLoader 负责 sibling Clashr-Auto 发现）。
    AppConfig config = AppConfigLoader::load();
    auto *core = new CoreController(config, &app);
    auto *clash = new ClashService(&app);
    auto *subs = new SubscriptionStore(config, &app);
    // TrayController 只在 m_window 非空时用它做 show/raise；QML 版暂传 nullptr（托盘的
    // 「控制面板」项后续再接上 QML 窗口）。托盘图标 + 流量 + 三个 toggle 信号仍可用。
    auto *tray = new TrayController(nullptr, &app);

    clash->setEndpoint(config.host, config.uiPort);
    clash->setMixedPort(config.mixedPort);
    clash->setSecret(config.secret);
    clash->setClearConnectionsOnSwitch(config.clearConnections);

    QmlBridge bridge(&config, core, clash, subs, &app);

    // 各页面适配器（薄胶水，复用后端；堆分配交给 app 清理，避免栈对象与 QObject 父子重复析构）。
    auto *subsController = new SubscriptionsController(subs, core, &app);
    auto *logModel = new LogModel(core, clash, &app);
    auto *about = new AboutController(&app);
    auto *settingsCtrl = new SettingsController(core, clash, &app);
    auto *updateCtrl = new UpdateController(config, core, &app);

    // 托盘 toggle → 后端（与 Widgets 版契约一致）。核心生命周期仍由用户显式触发。
    QObject::connect(tray, &TrayController::toggleCoreRequested, core, &CoreController::toggleCore);
    QObject::connect(tray, &TrayController::toggleProxyRequested, core, &CoreController::toggleProxy);
    QObject::connect(tray, &TrayController::toggleTunRequested, core, &CoreController::toggleTun);
    QObject::connect(clash, &ClashService::trafficUpdated, tray, &TrayController::setTraffic);
    QObject::connect(core, &CoreController::statusChanged, tray, &TrayController::setStatus);

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

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("ClashAuto", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    clash->start(); // 只读轮询 REST API

    // 正式版：启动即自动拉起核心（复刻旧版 MainWindow：有内核就起，没内核只记日志，
    // 用户到「设置 → 系统」下载）。延时 600ms 让 UI 先就绪，与旧版一致。
    // 本地测 UI 时 CLASHAUTO_NO_AUTOSTART=1 跳过（本机已有实例，避免端口/代理/TUN 冲突）。
    if (qEnvironmentVariableIsEmpty("CLASHAUTO_NO_AUTOSTART")) {
        QTimer::singleShot(600, core, [core] {
            if (core && core->isCoreInstalled() && !core->isRunning())
                core->startCore();
        });
    }
    return app.exec();
}
