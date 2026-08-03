#include "SingleInstance.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>

#include <functional>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

// 固定名字（带用户标识，理由见头文件）。改名等于放行一次并存 —— 别为了"品牌统一"去动它。
QString serverName()
{
    QString who;
#ifdef Q_OS_UNIX
    who = QString::number(static_cast<uint>(::getuid()));
#else
    // Windows：命名管道本就按登录会话隔离，这里补一个用户名只为同机多用户切换时更稳。
    who = qEnvironmentVariable("USERNAME");
    if (who.isEmpty())
        who = QStringLiteral("default");
#endif
    return QStringLiteral("coast-singleton-") + who;
}

} // namespace

bool SingleInstance::notifyExistingAndQuit()
{
    QLocalSocket probe;
    probe.connectToServer(serverName());
    // 300ms：连接由内核 accept 队列完成，**不依赖对方事件循环**，所以即使已有实例正忙也够用。
    if (!probe.waitForConnected(300))
        return false;

    probe.write("raise");
    probe.flush();
    probe.waitForBytesWritten(1000);
    probe.disconnectFromServer();
    qInfo("SingleInstance: 已有实例在运行，请求其显示窗口后退出（避免两个实例互相清掉网关规则）");
    return true;
}

SingleInstance::SingleInstance() = default;
SingleInstance::~SingleInstance() = default;

void SingleInstance::listen(std::function<void()> onRaise)
{
    const QString name = serverName();
    // 上次被 kill -9 时 Unix 下会留下陈旧 socket 文件；此时 connect 已失败过，直接覆盖。
    QLocalServer::removeServer(name);

    m_server = std::make_unique<QLocalServer>();
    // 仅同一用户可连：防止本机其他用户/低权限进程投递指令。
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(name)) {
        // 不致命：探测阶段已确认没有活实例，这里失败只意味着丢掉"第二次启动前置窗口"的能力。
        qWarning("SingleInstance: 监听 %s 失败（%s）——二次启动将无法唤起窗口",
                 name.toUtf8().constData(), m_server->errorString().toUtf8().constData());
        m_server.reset();
        return;
    }

    QObject::connect(m_server.get(), &QLocalServer::newConnection, m_server.get(),
                     [this, onRaise = std::move(onRaise)] {
                         while (QLocalSocket *conn = m_server->nextPendingConnection()) {
                             conn->deleteLater();
                             if (onRaise)
                                 onRaise();
                         }
                     });
}
