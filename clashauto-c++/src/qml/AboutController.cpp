#include "AboutController.h"

#include "Version.h" // APP_VERSION（由 CMake configure_file 生成，QmlBridge.cpp 同款包含）

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVector>

namespace {
// 与 Widgets 版 MainWindow.cpp 的 versionNewer() 同款：去掉非数字/点，逐段比较。
bool versionNewer(const QString &remote, const QString &local)
{
    auto parse = [](QString s) {
        s.remove(QRegularExpression(QStringLiteral("[^0-9.]")));
        QVector<int> v;
        for (const QString &p : s.split('.', Qt::SkipEmptyParts)) {
            v << p.toInt();
        }
        return v;
    };
    const QVector<int> a = parse(remote);
    const QVector<int> b = parse(local);
    for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a.at(i) : 0;
        const int y = i < b.size() ? b.at(i) : 0;
        if (x != y) {
            return x > y;
        }
    }
    return false;
}
} // namespace

AboutController::AboutController(QObject *parent) : QObject(parent) {}

QString AboutController::currentVersion() const
{
    return QString::fromUtf8(APP_VERSION);
}

void AboutController::setChecking(bool v)
{
    if (m_checking == v) {
        return;
    }
    m_checking = v;
    emit checkingChanged();
}

void AboutController::check()
{
    if (m_checking) {
        return; // 检查中不重复发起
    }
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    setChecking(true);

    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/ClashrAuto/clashauto/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    // 组织/仓库改名会以 301 跳转——必须跟随，否则拿到空响应而「查不到版本」。
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(10000);
#endif

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        setChecking(false);

        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            return;
        }
        const QJsonObject r = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = r.value(QStringLiteral("tag_name")).toString();
        const QString local = QString::fromUtf8(APP_VERSION);
        if (tag.isEmpty()) {
            const QString apiMsg = r.value(QStringLiteral("message")).toString();
            emit checkFailed(apiMsg.isEmpty() ? QStringLiteral("未取到版本号") : apiMsg);
            return;
        }

        if (m_latestVersion != tag) {
            m_latestVersion = tag;
            emit latestVersionChanged();
        }
        const bool newer = versionNewer(tag, local);
        if (m_updateAvailable != newer) {
            m_updateAvailable = newer;
            emit updateAvailableChanged();
        }
        if (!newer) {
            emit upToDate();
        }
    });
}
