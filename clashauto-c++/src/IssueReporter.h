#pragma once

// IssueReporter —— 把程序运行期间的报错汇总、脱敏，提交到本项目的 GitHub issue 区。
//
// 汇聚口径（"所有报错"）：
//   · Qt 消息处理器：qWarning/qCritical/qFatal，以及 QML 运行时报错（Qt 都走这条）；
//   · 后端既有的报错信号：核心/应用日志里被判为 error 的行、网关报错、更新失败、订阅失败…
//     （具体接线在 main_qml.cpp，本类只提供 report() 这一个入口）。
//
// 两条硬约束决定了它的形态，改动前务必先读：
//
// 1) **客户端里不能有 GitHub token。** 建 issue 需要仓库写权限，而 Coast 是可自由下载的开源
//    桌面程序——任何打进二进制的凭据都等同于公开，谁都能拿去刷爆 issue 区。所以默认路径
//    **零密钥**：把标题/正文拼进 issues/new 的 URL，用系统浏览器打开，由用户自己点 Submit
//    （用的是用户自己的 GitHub 身份）。只有当用户在设置里**自愿填入**自己的 PAT 时，才走
//    REST API 真正全自动提交。绝不要为了"更自动"而内置一个共用 token。
//
// 2) **报错正文常带隐私。** 订阅 URL、节点名、局域网 IP/MAC、网卡 GUID、用户名与主目录路径
//    都可能出现在报错里，而 issue 区是公开的。redact() 负责在**入队之前**就把这些抹掉——
//    脱敏发生在源头，之后无论走哪条提交路径都不会再漏。
//
// 另外两个不做不行的工程约束：
//   · 去重：同一个报错在一次会话里可能每秒重复一次（例如每轮扫描都失败的网关）。按指纹去重，
//     并把已上报过的指纹**持久化**到 configDir/issue-reported.json —— 否则每次启动都会把同样
//     的问题再提一遍，issue 区会被自家用户淹掉。
//   · 限流：单次提交合并一批；每次提交之间至少隔 kMinSubmitIntervalMs。
#include "AppConfig.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class CoreController;
class QNetworkAccessManager;
class QTimer;

class IssueReporter final : public QObject
{
    Q_OBJECT
    // 自动上报总开关。默认**关**：把用户机器上的报错发到公开 issue 区必须是知情且自愿的。
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // 是否已经就"要不要开启上报"询问过用户（用于只弹一次首启询问）。
    Q_PROPERTY(bool asked READ asked NOTIFY askedChanged)
    // 待上报条数（已脱敏、已去重）。
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingChanged)
    // 本会话累计捕获 / 已上报条数（设置页展示，让用户对"发出去了多少"有数）。
    Q_PROPERTY(int capturedCount READ capturedCount NOTIFY pendingChanged)
    Q_PROPERTY(int submittedCount READ submittedCount NOTIFY pendingChanged)
    // 用户是否自备了 PAT（只报有无，不回显 token 本身）。
    Q_PROPERTY(bool hasToken READ hasToken NOTIFY tokenChanged)
    Q_PROPERTY(bool submitting READ submitting NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString repo READ repo CONSTANT)

public:
    explicit IssueReporter(AppConfig config, CoreController *core, QObject *parent = nullptr);
    ~IssueReporter() override;

    // 安装全局 Qt 消息处理器（qWarning/qCritical/QML 报错 → report("qt", …)）。
    // 只该在 main 里调用一次；会链式调用之前的处理器，控制台输出不受影响。
    void installMessageHandler();

    // 本类要先于 CoreController 构造（消息处理器越早装越好），核心因此后置注入：
    // 只用来判断"核心在跑吗"，好让提交请求走本地混合端口（GitHub 被墙时仍能发出去）。
    void setCore(CoreController *core) { m_core = core; }

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    bool asked() const { return m_asked; }
    int pendingCount() const { return m_pending.size(); }
    int capturedCount() const { return m_captured; }
    int submittedCount() const { return m_submitted; }
    bool hasToken() const { return !m_token.isEmpty(); }
    bool submitting() const { return m_submitting; }
    QString status() const { return m_status; }
    QString repo() const;

public slots:
    // 唯一入口。source 是粗粒度来源（"core"/"app"/"gateway"/"update"/"npcap"/"qt"…），
    // 用于 issue 标题分组；message 原样传入，脱敏与去重由本类负责。
    void report(const QString &source, const QString &message);

    // 记下"已经问过用户了"（无论用户选开还是不开），首启询问只出现一次。
    Q_INVOKABLE void markAsked();
    // 立即提交待报队列（有 token 走 API，否则开浏览器预填 issue）。
    Q_INVOKABLE void submit();
    // 丢弃待报队列（并把这些指纹记为已处理，不再反复冒出来）。
    Q_INVOKABLE void discard();
    // 提交前给用户看的正文全文（设置页"预览"按钮）。
    Q_INVOKABLE QString preview() const;
    // 用户自备 PAT：留空即清除，改回浏览器预填路径。
    Q_INVOKABLE void setToken(const QString &token);

signals:
    void enabledChanged();
    void askedChanged();
    void pendingChanged();
    void tokenChanged();
    void statusChanged();
    // 有新的待报报错（QML 据此显示提示条 / 首次出错时询问是否开启上报）。
    void pendingErrorArrived(const QString &summary);

private:
    struct Entry {
        QDateTime when;
        QString source;
        QString message;     // 已脱敏
        QString fingerprint; // 归一化后的指纹（去重 + 在 issue 里搜同类）
        int repeats = 1;     // 本会话内重复次数（只提交一条，附上次数）
    };

    // —— 文本处理 ——
    // 抹掉 URL(白名单外的 host)、token/密码、IP、MAC、网卡 GUID、用户名与主目录路径。
    QString redact(const QString &text) const;
    // 归一化（数字/十六进制串 → #，压空白）后取 sha1 前 12 位，用来判"同一个报错"。
    static QString fingerprintOf(const QString &source, const QString &redacted);
    // 运行环境块（版本/系统/架构/语言/Qt），是 issue 里最有用的部分之一。
    QString environmentBlock() const;
    QString buildTitle() const;
    QString buildBody() const;

    // —— 持久化的已报指纹 ——
    QString seenPath() const;
    void loadSeen();
    void saveSeen();

    void setStatus(const QString &s);
    void scheduleAutoSubmit();

    // —— 提交路径 ——
    void submitViaBrowser();                 // 零密钥：打开 issues/new?title=&body=
    void submitViaApi();                     // 用户自备 PAT：搜同类 → 有则评论、无则新建
    void apiCreateIssue();
    void apiCommentOn(int issueNumber);
    void onSubmitted(bool ok, const QString &detail);
    QNetworkAccessManager *nam();

    AppConfig m_config;
    CoreController *m_core = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_autoTimer = nullptr;

    bool m_enabled = false;
    bool m_asked = false;
    bool m_submitting = false;
    QString m_token;
    QString m_status;
    QVector<Entry> m_pending;
    QSet<QString> m_seen;   // 已上报（或已被用户丢弃）过的指纹，跨会话持久
    int m_captured = 0;
    int m_submitted = 0;
    QElapsedTimer m_sinceSubmit;

    // 单次提交最多带这么多条；再多说明是同一批雪崩，留在本地由用户手动再提。
    static constexpr int kMaxPending = 20;
    // 两次自动提交之间的最小间隔（10 分钟）——别把 issue 区当日志服务器。
    static constexpr qint64 kMinSubmitIntervalMs = 10 * 60 * 1000;
    // 收到第一条报错后攒多久再提交：同一次故障往往连着抛好几条，攒一下能合成一个 issue。
    static constexpr int kBatchDelayMs = 30 * 1000;
};
