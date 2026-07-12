#include "MainWindow.h"
#include "AppConfig.h"
#include "ConfigBuilder.h"
#include "SubscriptionStore.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTextStream>

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
    // 载入旧项目 iconfont（family "iconfont"），供 logo / 流量卡 / 页脚等图标复用
    QFontDatabase::addApplicationFont(":/assets/iconfont.ttf");

    if (app.arguments().contains("--build-config")) {
        const AppConfig config = AppConfigLoader::load();
        ConfigBuilder builder(config);
        const QString path = builder.ensureFullConfig(config.tun);
        QTextStream(stdout) << path << Qt::endl;
        return 0;
    }

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

    // 单实例 + 链接转发：能连上已运行实例就把链接发过去并退出（CLI 子命令不受此限制）
    const QString serverName = "clashauto-cpp-singleton";
    {
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
    server.listen(serverName);

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
