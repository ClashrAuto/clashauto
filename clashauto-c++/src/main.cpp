#include "MainWindow.h"
#include "AppConfig.h"
#include "ConfigBuilder.h"
#include "SubscriptionStore.h"
#if defined(Q_OS_MACOS)
#include "MacHelperClient.h" // 特权 helper 的注册/ping（--mac-helper-* 子命令，真机验证用）
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTextStream>
#include <QThread>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#endif

int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN)
    // GUI 子系统：双击不弹控制台；但从终端启动时附着父控制台，让 --build-config 等 CLI 子命令能打印
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
#if defined(_MSC_VER)
        FILE *f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
#else
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
#endif
    }
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName("Clash Auto");
    QApplication::setOrganizationName("ClashAuto");
    QApplication::setWindowIcon(QIcon(":/assets/icon.ico")); // 窗口/任务栏图标（exe 图标另见 assets/app.rc）
    // 载入旧项目 iconfont（family "iconfont"），供 logo / 流量卡 / 页脚等图标复用
    QFontDatabase::addApplicationFont(":/assets/iconfont.ttf");

    if (app.arguments().contains("--build-config")) {
        const AppConfig config = AppConfigLoader::load();
        ConfigBuilder builder(config);
        const QString path = builder.ensureFullConfig(config.tun);
        QTextStream(stdout) << path << Qt::endl;
        return 0;
    }

#if defined(Q_OS_MACOS)
    // 特权 helper 的 CLI 验证入口（真机上从已签名 bundle 内运行才有效）：
    //   .../Clash Auto.app/Contents/MacOS/Clash Auto --mac-helper-register|status|ping|unregister
    {
        const QStringList args = app.arguments();
        auto statusName = [](MacHelper::RegStatus s) -> QString {
            switch (s) {
                case MacHelper::RegStatus::Enabled:          return QStringLiteral("enabled");
                case MacHelper::RegStatus::RequiresApproval: return QStringLiteral("requires-approval");
                case MacHelper::RegStatus::NotRegistered:    return QStringLiteral("not-registered");
                case MacHelper::RegStatus::NotFound:         return QStringLiteral("not-found");
                default:                                     return QStringLiteral("unknown");
            }
        };
        if (args.contains("--mac-helper-status")) {
            QTextStream(stdout) << statusName(MacHelper::status()) << Qt::endl;
            return 0;
        }
        if (args.contains("--mac-helper-register")) {
            QString err;
            const MacHelper::RegStatus s = MacHelper::registerDaemon(&err);
            QTextStream out(stdout); // 具名 lvalue：临时 QTextStream 上直接 << Qt::endl 不合法
            out << "status=" << statusName(s);
            if (!err.isEmpty()) out << " error=" << err;
            out << Qt::endl;
            return s == MacHelper::RegStatus::Enabled ? 0 : 1;
        }
        if (args.contains("--mac-helper-unregister")) {
            QString err;
            const bool ok = MacHelper::unregisterDaemon(&err);
            QTextStream(stdout) << (ok ? QStringLiteral("unregistered") : (QStringLiteral("failed: ") + err)) << Qt::endl;
            return ok ? 0 : 1;
        }
        if (args.contains("--mac-helper-ping")) {
            QString err;
            const QString v = MacHelper::pingVersion(&err);
            if (!v.isEmpty()) { QTextStream(stdout) << "helper-version=" << v << Qt::endl; return 0; }
            QTextStream(stdout) << "ping failed: " << err << Qt::endl;
            return 1;
        }
    }
#endif

    const int urlArg = app.arguments().indexOf("--print-effective-url");
    if (urlArg >= 0 && app.arguments().size() > urlArg + 1) {
        const AppConfig config = AppConfigLoader::load();
        SubscriptionStore store(config);
        QTextStream(stdout) << store.effectiveUrlForTest(app.arguments().at(urlArg + 1).toInt()) << Qt::endl;
        return 0;
    }

    if (app.arguments().contains("--print-subscription-path")) {
        const AppConfig config = AppConfigLoader::load();
        SubscriptionStore store(config);
        QTextStream(stdout) << store.path() << Qt::endl;
        return 0;
    }

    if (app.arguments().contains("--list-subscriptions")) {
        const AppConfig config = AppConfigLoader::load();
        SubscriptionStore store(config);
        const QVector<SubscriptionSummary> summaries = store.load();
        QTextStream out(stdout);
        for (int i = 0; i < summaries.size(); ++i) {
            out << i << "\t" << summaries[i].type << "\t" << summaries[i].name << "\t" << summaries[i].url << Qt::endl;
        }
        return 0;
    }

    const int listNodesArg = app.arguments().indexOf("--list-nodes");
    if (listNodesArg >= 0 && app.arguments().size() > listNodesArg + 1) {
        const AppConfig config = AppConfigLoader::load();
        SubscriptionStore store(config);
        const QVector<SubscriptionNodeSummary> nodes = store.nodes(app.arguments().at(listNodesArg + 1).toInt());
        QTextStream out(stdout);
        for (int i = 0; i < nodes.size(); ++i) {
            out << i << "\t" << (nodes[i].use ? "true" : "false") << "\t" << nodes[i].name << "\t"
                << nodes[i].server << ":" << nodes[i].port << Qt::endl;
        }
        return 0;
    }

    const int setNodeArg = app.arguments().indexOf("--set-node-use");
    if (setNodeArg >= 0 && app.arguments().size() > setNodeArg + 3) {
        const AppConfig config = AppConfigLoader::load();
        SubscriptionStore store(config);
        const bool enabled = app.arguments().at(setNodeArg + 3).toLower() == "true";
        const bool ok = store.setNodeEnabled(app.arguments().at(setNodeArg + 1).toInt(), app.arguments().at(setNodeArg + 2).toInt(), enabled);
        QTextStream(ok ? stdout : stderr) << (ok ? "ok" : "failed") << Qt::endl;
        return ok ? 0 : 4;
    }

    const int removeArg = app.arguments().indexOf("--remove-subscription");
    if (removeArg >= 0 && app.arguments().size() > removeArg + 1) {
        const AppConfig config = AppConfigLoader::load();
        SubscriptionStore store(config);
        const bool ok = store.removeSubscription(app.arguments().at(removeArg + 1).toInt());
        QTextStream(ok ? stdout : stderr) << (ok ? "ok" : "failed") << Qt::endl;
        return ok ? 0 : 5;
    }

    const int editArg = app.arguments().indexOf("--edit-subscription");
    if (editArg >= 0 && app.arguments().size() > editArg + 4) {
        const AppConfig config = AppConfigLoader::load();
        SubscriptionStore store(config);
        const bool ok = store.editSubscription(app.arguments().at(editArg + 1).toInt(),
                                               app.arguments().at(editArg + 2),
                                               app.arguments().at(editArg + 3),
                                               app.arguments().at(editArg + 4));
        QTextStream(ok ? stdout : stderr) << (ok ? "ok" : "failed") << Qt::endl;
        return ok ? 0 : 6;
    }

    const int updateArg = app.arguments().indexOf("--update-subscription");
    if (updateArg >= 0 && app.arguments().size() > updateArg + 2) {
        const int index = app.arguments().at(updateArg + 1).toInt();
        QFile file(app.arguments().at(updateArg + 2));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream(stderr) << "Cannot read subscription source" << Qt::endl;
            return 2;
        }
        const AppConfig config = AppConfigLoader::load();
        SubscriptionStore store(config);
        QString message;
        const bool ok = store.updateSubscriptionFromTextForTest(index, QString::fromUtf8(file.readAll()), &message);
        QTextStream(ok ? stdout : stderr) << message << Qt::endl;
        return ok ? 0 : 3;
    }

    // 协议启动：从参数里取出 clash-auto:// 链接（若有）
    QString protocolUrl;
    for (const QString &arg : app.arguments()) {
        if (arg.startsWith("clash-auto://", Qt::CaseInsensitive)) {
            protocolUrl = arg;
            break;
        }
    }

    // 提权重启（--tun-elevated）：跳过单实例转发探测，否则会连到正在退出的旧(非提权)实例
    // 并把自己当成二次启动直接退出，导致提权实例起不来。
    const bool elevatedRestart = app.arguments().contains("--tun-elevated");

    // 单实例 + 链接转发：能连上已运行实例就把链接发过去并退出（CLI 子命令不受此限制）
    const QString serverName = "clashauto-cpp-singleton";
    if (!elevatedRestart) {
        QLocalSocket probe;
        probe.connectToServer(serverName);
        if (probe.waitForConnected(300)) {
            probe.write(protocolUrl.toUtf8()); // 可能为空 → 仅请求前置窗口
            probe.flush();
            probe.waitForBytesWritten(1000);
            probe.disconnectFromServer();
            return 0;
        }
    }

    // 本进程为主实例：监听命名管道，接收后续实例转发的链接
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    // 仅同一用户可连接该命名管道：防止本地其他用户/低权限进程投递「导入订阅」等指令
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.listen(serverName);
    // 提权重启时旧(非提权)实例可能还没退出、仍占用命名管道 → listen 失败；短暂重试直到它释放，
    // 保证提权实例也能建起单实例服务器（否则后续链接转发/防重复启动会失效）。
    if (elevatedRestart) {
        for (int i = 0; i < 10 && !server.isListening(); ++i) {
            QThread::msleep(150);
            QLocalServer::removeServer(serverName);
            server.listen(serverName);
        }
    }

    QFont font("Microsoft YaHei UI");
    app.setFont(font);

    MainWindow window;

    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window] {
        QLocalSocket *conn = server.nextPendingConnection();
        if (!conn) {
            return;
        }
        window.showNormal();
        window.raise();
        window.activateWindow();
        if (conn->bytesAvailable() > 0 || conn->waitForReadyRead(1000)) {
            const QString url = QString::fromUtf8(conn->readAll()).trimmed();
            if (!url.isEmpty()) {
                window.handleProtocolUrl(url);
            }
        }
        conn->disconnectFromServer();
        conn->deleteLater();
    });

    window.show();
    if (!protocolUrl.isEmpty()) {
        window.handleProtocolUrl(protocolUrl);
    }

    return app.exec();
}
