#include "TlsClient.h"

#include <QNetworkProxy>
#include <QSslConfiguration>
#include <QSslSocket>

namespace coastcore {

TlsClient::TlsClient(QObject *parent) : QObject(parent) {}

TlsClient::~TlsClient() = default;

void TlsClient::connectToHost(const QString &host, quint16 port, const QString &sni,
                              const QStringList &alpnList, bool skipVerify)
{
    m_skipVerify = skipVerify;
    m_encrypted = false;

    m_sock = new QSslSocket(this);
    // 同 DirectOutbound：必须 NoProxy，否则本机开着系统代理时 QSslSocket 会去读全局 ApplicationProxy，
    // 把这条本应「直连远端代理服务器」的 TLS 又绕回系统代理里。
    m_sock->setProxy(QNetworkProxy::NoProxy);

    QSslConfiguration cfg = m_sock->sslConfiguration();
    if (!alpnList.isEmpty()) {
        QList<QByteArray> protos;
        protos.reserve(alpnList.size());
        for (const QString &a : alpnList)
            protos.append(a.toUtf8());
        cfg.setAllowedNextProtocols(protos);
    }
    if (skipVerify) {
        // 不校验对端证书（自签 / 跳过校验场景）。VerifyNone 下 QSslSocket 不因校验错误阻断握手，
        // 但个别错误仍可能以 sslErrors 冒出来，下面再兜一层 ignoreSslErrors。
        cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    }
    m_sock->setSslConfiguration(cfg);

    // —— 信号接线 ——
    // ★ 只把 encrypted（TLS 建立）当 connected()，不接 QAbstractSocket::connected（那只是 TCP 通、
    //   TLS 还没握手完，此刻发数据会明文外泄/被对端当协议错误）。
    connect(m_sock, &QSslSocket::encrypted, this, [this] {
        m_encrypted = true;
        // 关 Nagle：出站管道每段都是上层已决定要发的字节，攒一手只叠延迟（理由同 Socks5Tcp）。
        // 必须在连接建立后设（Qt6 socketEngine 未起时 setSocketOption 直接 return）。
        m_sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        emit connected();
    });
    connect(m_sock, &QSslSocket::readyRead, this, [this] { emit readyRead(); });
    connect(m_sock, &QSslSocket::disconnected, this, [this] { emit disconnected(); });
    connect(m_sock, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { emit errorOccurred(m_sock->errorString()); });
    // sslErrors：skipVerify 时一律忽略放行；否则报错并中止（默认 QSslSocket 会自己中止握手，
    // 这里显式 emit 让上层拿到原因）。
    connect(m_sock, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errs) {
        if (m_skipVerify) {
            m_sock->ignoreSslErrors();
            return;
        }
        QString reason = QStringLiteral("TLS 校验失败");
        if (!errs.isEmpty())
            reason = errs.first().errorString();
        emit errorOccurred(reason);
    });

    // SNI = sni（空则回落 host）。connectToHostEncrypted 的第三参 sslPeerName 同时用作 SNI 扩展与
    // 证书校验名；setPeerVerifyName 再显式设一遍，语义一致、双保险。
    const QString peerName = sni.isEmpty() ? host : sni;
    if (!peerName.isEmpty())
        m_sock->setPeerVerifyName(peerName);
    m_sock->connectToHostEncrypted(host, port, peerName);
}

qint64 TlsClient::write(const QByteArray &data)
{
    return m_sock ? m_sock->write(data) : -1;
}

QByteArray TlsClient::readAll()
{
    return m_sock ? m_sock->readAll() : QByteArray();
}

qint64 TlsClient::bytesAvailable() const
{
    return m_sock ? m_sock->bytesAvailable() : 0;
}

qint64 TlsClient::bytesToWrite() const
{
    return m_sock ? m_sock->bytesToWrite() : 0;
}

void TlsClient::flush()
{
    if (m_sock)
        m_sock->flush();
}

void TlsClient::setReadBufferSize(qint64 size)
{
    if (m_sock)
        m_sock->setReadBufferSize(size);
}

void TlsClient::abort()
{
    if (m_sock)
        m_sock->abort();
}

void TlsClient::close()
{
    if (m_sock)
        m_sock->close();
}

bool TlsClient::isEncrypted() const
{
    return m_encrypted;
}

QAbstractSocket *TlsClient::socket() const
{
    return m_sock;
}

} // namespace coastcore
