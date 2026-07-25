#pragma once

// NpcapInstaller —— 「安装 Npcap」独立窗口的控制器（Windows 专用；其它平台编译为空壳）。
//
// 背景：设备页的透明网关在 Windows 上走 Npcap（二层收发帧）。程序对 wpcap.dll/Packet.dll 是
// **延迟加载**（见 CMakeLists 的 /DELAYLOAD），所以没装 Npcap 时 Coast 照常启动、只是网关不可用，
// 用户完全看不出缺了什么。本类负责补上这条引导链：
//   检测是否已装 → 取最新版下载地址 → 带进度条下载 → 验签 → 提权静默安装(/S) → 复检。
//
// 与 UpdateController 的下载/进度契约保持一致（progress / downloadSpeed / downloadedText /
// totalText / cancel），QML 那边的进度条可以照抄同一套写法。
//
// 两个「无法回避」的事实，UI 上必须诚实告知：
//   1) 安装驱动必须管理员权限 → 一定会弹一次 UAC。所谓「静默」= 不用点安装向导的下一步，
//      不等于不弹 UAC。
//   2) Npcap 免费版的许可只允许 **OEM 版**静默安装，免费版安装器可能拒绝 /S。故 install()
//      在静默失败且仍未装上时，会自动改用「可见向导」再跑一次（仍是提权的），让用户点完。
#include "AppConfig.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class CoreController;

class NpcapInstaller final : public QObject
{
    Q_OBJECT
    // 本平台是否需要/支持 Npcap（仅 Windows true）。QML 用它决定要不要显示整条引导。
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool installed READ installed NOTIFY installedChanged)
    Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY installedChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY latestChanged)
    // busy = 下载中或安装中（按钮置灰）；downloading 单指下载阶段（进度条只在这时显示）。
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY busyChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString downloadSpeed READ downloadSpeed NOTIFY downloadStatsChanged)
    Q_PROPERTY(QString downloadedText READ downloadedText NOTIFY downloadStatsChanged)
    Q_PROPERTY(QString totalText READ totalText NOTIFY downloadStatsChanged)

public:
    explicit NpcapInstaller(AppConfig config, CoreController *core, QObject *parent = nullptr);

    bool supported() const;
    bool installed() const { return m_installed; }
    QString installedVersion() const { return m_installedVersion; }
    QString latestVersion() const { return m_latestVersion; }
    bool busy() const { return m_downloading || m_installing; }
    bool downloading() const { return m_downloading; }
    int progress() const { return m_progress; }
    QString status() const { return m_status; }
    QString downloadSpeed() const { return m_speedText; }
    QString downloadedText() const { return m_downloadedText; }
    QString totalText() const { return m_totalText; }

    // 重新检测本机是否已装 Npcap（打开窗口 / 安装结束后调用）。顺带异步查一次最新版本号。
    Q_INVOKABLE void refresh();
    // 一键：下载 → 验签 → 提权静默安装。
    Q_INVOKABLE void install();
    // 取消下载（安装阶段已交给系统安装器，无法取消）。
    Q_INVOKABLE void cancel();
    // 打开官网下载页（自动流程失败时的兜底）。
    Q_INVOKABLE void openHomepage();

signals:
    void installedChanged();
    void latestChanged();
    void busyChanged();
    void progressChanged();
    void statusChanged();
    void downloadStatsChanged();
    // 整个流程结束：ok=true 表示复检确认已装上（main 据此让设备页重新配置网关）。
    void finished(bool ok);

private:
    void detectInstalled();                 // 查文件 + 注册表，写 m_installed/m_installedVersion
    void fetchLatest();                     // 查最新版本号（GitHub tag）→ 拼官网下载直链
    void startDownload(const QString &url, const QString &name);
    void setStatus(const QString &s);
    void setProgress(int v);
    void setDownloading(bool v);
    void setInstalling(bool v);
    void applyDownloadProxy(QNetworkAccessManager *nam) const;
#if defined(Q_OS_WIN)
    // 提权运行安装器；silent=true 传 /S。返回 false = 没起来（含用户拒绝 UAC）。
    bool runInstaller(const QString &path, bool silent);
    void onInstallerExited(const QString &path, bool wasSilent);
#endif

    AppConfig m_config;
    CoreController *m_core = nullptr;
    QNetworkAccessManager *m_nam = nullptr;

    bool m_installed = false;
    QString m_installedVersion;
    QString m_latestVersion;
    QString m_latestUrl;   // 下载直链（GitHub asset 或官网兜底）
    QString m_latestName;  // 文件名 npcap-x.yz.exe

    bool m_downloading = false;
    bool m_installing = false;
    int m_progress = 0;
    QString m_status;
    QString m_speedText, m_downloadedText, m_totalText;
    QNetworkReply *m_dlReply = nullptr;
    QElapsedTimer m_dlTimer;
    qint64 m_lastBytes = 0;
    qint64 m_lastMs = 0;
    bool m_cancelled = false;
};
