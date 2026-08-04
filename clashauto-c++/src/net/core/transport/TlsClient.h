#pragma once

// TLS 客户端薄封装 —— QSslSocket 的一层「只暴露协议出站真正要用到的东西」的皮。
//
// ★ 谁用它：所有「叠 TLS」的协议出站（Trojan 恒 TLS；VMess/VLESS 的 tls:true；ws 传输的 wss 形态）。
//   它们把 TlsClient 当一根**已加密的双向字节管道**：connectToHost() 起 TCP+TLS 握手，encrypted 即
//   connected()，之后 write()/readyRead()/readAll() 就是明文字节进出（加解密对上层透明）。
//
// ★ 为什么单独封而不直接用 QSslSocket：
//   · 统一「connected() = TLS 已建立」的语义（QSslSocket 有 connected(TCP 通) 和 encrypted(TLS 通)
//     两个信号，协议侧只关心后者，直接用容易连错到 TCP connected 而在握手完成前就发数据）。
//   · 把 SNI / ALPN / skipVerify 这三件每个 TLS 协议都要设的事收敛成一个 connectToHost 入参，
//     避免每个协议各写一遍 QSslConfiguration 的样板（还容易漏设 NoProxy 被系统代理绕回去）。
//   · 暴露 socket() 供 WsClient 注入 —— wss = 「WsClient 跑在 TlsClient 的 QSslSocket 之上」。
//
// 跨平台：只用 Qt Network。QSslSocket 的底层 TLS 后端由 Qt 提供（Windows Schannel / 其它 OpenSSL），
// 与本文件的 AEAD(libcrypto) 是两回事，互不影响。
#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class QSslSocket;

namespace coastcore {

class TlsClient : public QObject
{
    Q_OBJECT
public:
    explicit TlsClient(QObject *parent = nullptr);
    ~TlsClient() override;

    // 发起 TCP 连接并在其上做 TLS 握手。
    //   host       —— 实际连接/解析的主机（域名或 IP）。
    //   port       —— 端口。
    //   sni        —— TLS ServerName（证书校验名 + SNI 扩展）。空则回落用 host。
    //   alpnList   —— ALPN 协议名（如 {"h2","http/1.1"}）；空则不设 ALPN。
    //   skipVerify —— true 时 VerifyNone（自签/跳过校验场景），并对 sslErrors 一律 ignore。
    // 握手成功 → connected()；失败 → errorOccurred()。
    void connectToHost(const QString &host, quint16 port, const QString &sni,
                       const QStringList &alpnList, bool skipVerify);

    // —— 已建立后的字节管道（语义同 QIODevice/QAbstractSocket）——
    qint64 write(const QByteArray &data);
    QByteArray readAll();
    qint64 bytesAvailable() const;
    qint64 bytesToWrite() const;
    void flush();
    void setReadBufferSize(qint64 size);
    void abort();
    void close();

    bool isEncrypted() const; // TLS 是否已建立（= connected() 已发过）

    // 暴露底层 QSslSocket（QAbstractSocket*），供 WsClient 注入以实现 wss。所有权仍属本对象，
    // 调用方**不得** delete；且不应在其上直接读写（会与本封装的缓冲语义打架）——仅 WsClient
    // 这类需要接管字节流的组件才拿它当传输。connectToHost 之前为 nullptr。
    QAbstractSocket *socket() const;

signals:
    void connected();    // TLS 握手完成（QSslSocket::encrypted），双向明文管道可用
    void readyRead();    // 有可读明文
    void disconnected(); // 对端关闭
    void errorOccurred(const QString &reason); // TCP/TLS 出错（含校验失败，除非 skipVerify）

private:
    QSslSocket *m_sock = nullptr;
    bool m_skipVerify = false;
    bool m_encrypted = false;
};

} // namespace coastcore
