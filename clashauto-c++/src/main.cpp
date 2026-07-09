#include "MainWindow.h"
#include "AppConfig.h"
#include "ConfigBuilder.h"
#include "SubscriptionStore.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Clash Auto");
    QApplication::setOrganizationName("ClashAuto");

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

    QFont font("Microsoft YaHei UI");
    app.setFont(font);

    MainWindow window;
    window.show();

    return app.exec();
}
