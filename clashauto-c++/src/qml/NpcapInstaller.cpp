#include "DownloadStats.h"
#include "NpcapInstaller.h"

#include "CoreController.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

#  include "../net/NpcapStatus.h" // installed/adminOnly/processElevated（与二层端点同一套口径）

#  include <wincrypt.h>  // CryptQueryObject / CertGetNameString（取签名者主体名）
#  include <wintrust.h>  // WinVerifyTrust
#  include <softpub.h>   // WINTRUST_ACTION_GENERIC_VERIFY_V2
#  include <shellapi.h>  // ShellExecuteExW（runas 提权跑安装器）
#  include <QWinEventNotifier>
#  include <string>
#  include <vector>
#endif

namespace {

// 下载源 = 官网 https://npcap.com/dist/npcap-<版本>.exe（命名规则稳定）。版本号从 nmap/npcap 的
// GitHub release tag 取 —— 注意那边的 release **不带任何二进制资源**（实测 v1.88 assets 为空），
// GitHub 在这里只当「最新版本号」的目录用，文件始终从官网拿。
// 连 tag 都取不到（离线/限流）时退回下面钉死的版本，保证「立即安装」永远点得动。
constexpr const char *kFallbackVersion = "1.83";
constexpr const char *kHomepage = "https://npcap.com/#download";

QString distUrlFor(const QString &version)
{
    return QStringLiteral("https://npcap.com/dist/npcap-%1.exe").arg(version);
}

#if defined(Q_OS_WIN)
// 允许的签名者（CN）。Npcap 由 Nmap 项目签发，历史上用过 "Insecure.Com LLC" 与 "Nmap Software LLC"。
bool signerAllowed(const QString &subject)
{
    const QString s = subject.toLower();
    return s.contains(QStringLiteral("insecure.com")) || s.contains(QStringLiteral("nmap"));
}

// 取 PE 文件内嵌 Authenticode 签名的「签名者主体名」。取不到返回空串。
QString authenticodeSigner(const QString &path)
{
    const std::wstring w = QDir::toNativeSeparators(path).toStdWString();
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    DWORD enc = 0, ctype = 0, fmt = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, w.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &enc, &ctype, &fmt, &store, &msg,
                          nullptr)) {
        return {};
    }
    QString name;
    DWORD sz = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &sz) && sz > 0) {
        std::vector<BYTE> buf(sz);
        auto *si = reinterpret_cast<CMSG_SIGNER_INFO *>(buf.data());
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si, &sz)) {
            CERT_INFO ci{};
            ci.Issuer = si->Issuer;
            ci.SerialNumber = si->SerialNumber;
            if (auto *ctx = CertFindCertificateInStore(store, enc, 0, CERT_FIND_SUBJECT_CERT, &ci,
                                                       nullptr)) {
                const DWORD n =
                    CertGetNameStringW(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
                if (n > 1) {
                    std::wstring s(n, L'\0');
                    CertGetNameStringW(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, s.data(), n);
                    name = QString::fromWCharArray(s.c_str());
                }
                CertFreeCertificateContext(ctx);
            }
        }
    }
    if (msg) {
        CryptMsgClose(msg);
    }
    if (store) {
        CertCloseStore(store, 0);
    }
    return name;
}

// 验签：证书链可信 + 签名者是 Nmap 项目。安装器要以管理员身份跑，这一步不能省。
bool verifySignature(const QString &path, QString *err)
{
    const std::wstring w = QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = w.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE; // 离线/内网也要能装，不查吊销
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.pFile = &fi;

    const LONG st = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    if (st != ERROR_SUCCESS) {
        if (err) {
            *err = QString::fromUtf8("数字签名无效（WinVerifyTrust 0x%1）")
                       .arg(QString::number(static_cast<quint32>(st), 16));
        }
        return false;
    }
    const QString subject = authenticodeSigner(path);
    if (!signerAllowed(subject)) {
        if (err) {
            *err = QString::fromUtf8("签名者不是 Nmap 项目：%1")
                       .arg(subject.isEmpty() ? QString::fromUtf8("(未取到)") : subject);
        }
        return false;
    }
    return true;
}
#endif // Q_OS_WIN

} // namespace

NpcapInstaller::NpcapInstaller(AppConfig config, CoreController *core, QObject *parent)
    : QObject(parent), m_config(std::move(config)), m_core(core)
{
    detectInstalled();
}

bool NpcapInstaller::supported() const
{
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

void NpcapInstaller::setStatus(const QString &s)
{
    if (m_status == s) {
        return;
    }
    m_status = s;
    emit statusChanged();
}

void NpcapInstaller::setProgress(int v)
{
    if (m_progress == v) {
        return;
    }
    m_progress = v;
    emit progressChanged();
}

void NpcapInstaller::setDownloading(bool v)
{
    if (m_downloading == v) {
        return;
    }
    m_downloading = v;
    emit busyChanged();
}

void NpcapInstaller::setInstalling(bool v)
{
    if (m_installing == v) {
        return;
    }
    m_installing = v;
    emit busyChanged();
}

void NpcapInstaller::applyDownloadProxy(QNetworkAccessManager *nam) const
{
    // 与 UpdateController 同策略：核心在跑就经混合端口下载（GitHub 被墙也能下到），否则直连。
    if (nam && m_core && m_core->isRunning()) {
        nam->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, m_config.host,
                                    static_cast<quint16>(m_config.mixedPort)));
    }
}

void NpcapInstaller::detectInstalled()
{
#if defined(Q_OS_WIN)
    // 判定口径见 src/net/NpcapStatus.h —— 和二层端点实际加载 wpcap.dll 的目录一致。
    const bool found = npcapstatus::installed();
    // 装了 ≠ 用得了：驱动可能被锁成「仅管理员可访问」，普通用户身份下 pcap_open_live 必然
    // 「拒绝访问」。这才是设备页「总提示打开网卡失败」的实际形态，必须单独暴露给 UI。
    const bool locked = found && npcapstatus::adminOnly() && !npcapstatus::processElevated();
    // 版本号取自卸载项（纯展示用；取不到不影响判定）。
    QString ver;
    QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows"
                                 "\\CurrentVersion\\Uninstall\\NpcapInst"),
                  QSettings::NativeFormat);
    ver = reg.value(QStringLiteral("DisplayVersion")).toString();

    const bool changed =
            (found != m_installed) || (locked != m_restricted) || (ver != m_installedVersion);
    m_installed = found;
    m_restricted = locked;
    m_installedVersion = ver;
    if (changed) {
        emit installedChanged();
    }
#else
    m_installed = false;
    m_restricted = false;
    m_installedVersion.clear();
#endif
}

void NpcapInstaller::refresh()
{
    detectInstalled();
    if (!supported()) {
        setStatus(QString::fromUtf8("当前平台不需要 Npcap（Linux 用 AF_PACKET，macOS 用 BPF）。"));
        return;
    }
    // 下载/安装/修复进行中一律不覆盖状态文字（那几条流程自己在写；窗口重新可见会再调一次 refresh）。
    if (busy()) {
        // 什么也不改
    } else if (m_restricted) {
        setStatus(QString::fromUtf8(
                          "已安装 Npcap%1，但驱动被限制为「仅管理员可访问」—— 这正是设备页"
                          "「打开网卡失败：拒绝访问」的原因。点「修复权限」解除（会弹一次 UAC），"
                          "或以管理员身份运行 Coast。")
                          .arg(m_installedVersion.isEmpty()
                                       ? QString()
                                       : QStringLiteral(" ") + m_installedVersion));
    } else if (m_installed) {
        setStatus(m_installedVersion.isEmpty()
                      ? QString::fromUtf8("已安装 Npcap，透明网关可用。")
                      : QString::fromUtf8("已安装 Npcap %1，透明网关可用。").arg(m_installedVersion));
    } else {
        setStatus(QString::fromUtf8("未安装 Npcap —— 设备页的「代理网络」需要它来接管局域网流量。"));
    }
    fetchLatest();
}

void NpcapInstaller::fetchLatest()
{
    if (!supported()) {
        return;
    }
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    applyDownloadProxy(m_nam);

    QNetworkRequest req{QUrl(QStringLiteral("https://api.github.com/repos/nmap/npcap/releases/latest"))};
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setTransferTimeout(15000);

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();

        QString ver;
        if (ok) {
            const QJsonObject r = QJsonDocument::fromJson(data).object();
            ver = r.value(QStringLiteral("tag_name")).toString();
            ver.remove(QRegularExpression(QStringLiteral("^[vV]")));
            // 只收 "1.88" 这种纯版本号；tag 变了格式就当没取到，走兜底而不是拼出个 404。
            if (!QRegularExpression(QStringLiteral("^[0-9]+(\\.[0-9]+)+$")).match(ver).hasMatch()) {
                ver.clear();
            }
        }
        if (ver.isEmpty()) {
            ver = QString::fromLatin1(kFallbackVersion);
        }
        m_latestUrl = distUrlFor(ver);
        m_latestName = QUrl(m_latestUrl).fileName();
        if (m_latestVersion != ver) {
            m_latestVersion = ver;
            emit latestChanged();
        }
    });
}

void NpcapInstaller::install()
{
    if (!supported() || busy()) {
        return;
    }
    QString url = m_latestUrl;
    QString name = m_latestName;
    if (url.isEmpty()) {
        // 版本还没查回来（网络慢 / 刚开窗就点）→ 直接用钉死的版本，别让用户干等。
        url = distUrlFor(QString::fromLatin1(kFallbackVersion));
        name = QUrl(url).fileName();
    }
    startDownload(url, name);
}

void NpcapInstaller::startDownload(const QString &url, const QString &name)
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    // 下载源是 npcap.com（不是 GitHub），所以没有 ghfast.top 这类镜像可用；核心在跑时
    // applyDownloadProxy 会让请求走本地混合端口，这就是唯一也够用的加速/绕行路径。
    applyDownloadProxy(m_nam);
    QNetworkAccessManager *dlNam = m_nam;
    const QString downloadUrl = url;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    const QString savePath = QDir(dir).filePath(name);

    QNetworkRequest req{QUrl(downloadUrl)};
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setTransferTimeout(30000);

    QNetworkReply *reply = dlNam->get(req);
    m_dlReply = reply;
    m_cancelled = false;
    m_dlTimer.start();
    m_lastMs = 0;
    m_lastBytes = 0;
    m_speedText.clear();
    m_downloadedText.clear();
    m_totalText.clear();
    emit downloadStatsChanged();

    auto *out = new QFile(savePath);
    out->setParent(reply); // 随 reply 一起销毁：取消/关窗不漏句柄
    if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort();
        reply->deleteLater();
        m_dlReply = nullptr;
        setStatus(QString::fromUtf8("无法写入临时文件：%1").arg(savePath));
        emit finished(false);
        return;
    }

    setDownloading(true);
    setProgress(0);
    setStatus(QString::fromUtf8("正在从 npcap.com 下载 %1").arg(name));

    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            setProgress(static_cast<int>(received * 100 / total));
        }
        m_downloadedText = DownloadStats::fmtBytes(received);
        m_totalText = total > 0 ? DownloadStats::fmtBytes(total) : QString::fromUtf8("?");
        const qint64 nowMs = m_dlTimer.elapsed();
        const qint64 dt = nowMs - m_lastMs;
        if (dt >= 500) { // 每 ≥500ms 采一次速，避免数字乱跳
            const double bps = (received - m_lastBytes) * 1000.0 / static_cast<double>(dt);
            m_speedText = DownloadStats::fmtBytes(static_cast<qint64>(bps)) + QStringLiteral("/s");
            m_lastMs = nowMs;
            m_lastBytes = received;
        }
        emit downloadStatsChanged();
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply, out] { out->write(reply->readAll()); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, out, savePath, name] {
        out->write(reply->readAll());
        out->close();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString err = reply->errorString();
        const bool cancelled = m_cancelled;
        m_dlReply = nullptr;
        reply->deleteLater();

        setDownloading(false);
        if (cancelled) {
            QFile::remove(savePath);
            m_cancelled = false;
            setProgress(0);
            setStatus(QString::fromUtf8("已取消下载"));
            emit finished(false);
            return;
        }
        if (!ok) {
            QFile::remove(savePath);
            setStatus(QString::fromUtf8("下载失败：%1\n可先启动核心（下载会走本地代理）再试，"
                                        "或点「官网下载」手动安装。")
                          .arg(err));
            emit finished(false);
            return;
        }
        setProgress(100);

#if defined(Q_OS_WIN)
        // 验签：这份 exe 马上要以管理员身份运行，签名不对一律不碰。
        setStatus(QString::fromUtf8("正在校验安装包数字签名..."));
        QString verr;
        if (!verifySignature(savePath, &verr)) {
            QFile::remove(savePath);
            setStatus(QString::fromUtf8("安装包校验失败，已删除：%1").arg(verr));
            emit finished(false);
            return;
        }
        setInstalling(true);
        setStatus(QString::fromUtf8("签名校验通过，正在静默安装 %1 —— 请在弹出的 UAC 窗口点「是」。")
                      .arg(name));
        if (!runInstaller(savePath, true)) {
            setInstalling(false);
            emit finished(false);
        }
#else
        Q_UNUSED(name);
        emit finished(false);
#endif
    });
}

void NpcapInstaller::cancel()
{
    if (m_dlReply) {
        m_cancelled = true;
        m_dlReply->abort(); // finished 里走「已取消」分支
    }
}

void NpcapInstaller::openHomepage()
{
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kHomepage)));
}

#if defined(Q_OS_WIN)
bool NpcapInstaller::runInstaller(const QString &path, bool silent)
{
    const std::wstring fileW = QDir::toNativeSeparators(path).toStdWString();
    // /S = NSIS 静默安装。注意：Npcap 免费版的许可只允许 OEM 版静默安装，免费版安装器可能
    // 忽略/拒绝 /S；因此静默跑完若仍未装上，onInstallerExited 会改用可见向导再跑一次。
    //
    // admin_only=no 是**必须**带的：Npcap 向导里「Restrict Npcap driver's access to
    // Administrators only」默认勾选，勾上后 Coast 以普通用户身份跑就永远打不开网卡（拒绝访问）。
    // winpcap_mode=yes 顺带装上 WinPcap 兼容名，兼容性更好。这两个选项官方文档只保证静默模式
    // 生效；可见向导里传过去多半被忽略，所以那条路上再用文案提醒用户手动取消勾选，并且装完
    // 还有 fixPermissions() 兜底。
    const std::wstring paramsW = silent ? L"/S /admin_only=no /winpcap_mode=yes"
                                        : L"/admin_only=no /winpcap_mode=yes";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    // 不用 NOASYNC：本进程不退出，要挂在事件循环里等安装器结束（进度条/状态还要更新）。
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // 装驱动必须管理员 → 一定会弹 UAC，这一步无法回避
    sei.lpFile = fileW.c_str();
    sei.lpParameters = paramsW.empty() ? nullptr : paramsW.c_str();
    sei.nShow = silent ? SW_HIDE : SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        setStatus(e == ERROR_CANCELLED
                      ? QString::fromUtf8("已取消：安装 Npcap 驱动需要管理员权限。")
                      : QString::fromUtf8("启动安装器失败（错误码 %1），请点「官网下载」手动安装。")
                            .arg(e));
        return false;
    }

    // 等安装器退出：进程句柄可等待，挂到 Qt 事件循环，不阻塞 UI。
    HANDLE proc = sei.hProcess;
    auto *notifier = new QWinEventNotifier(proc, this);
    connect(notifier, &QWinEventNotifier::activated, this,
            [this, notifier, proc, path, silent](HANDLE) {
                notifier->setEnabled(false); // 句柄保持有信号态，先关掉避免反复触发
                notifier->deleteLater();
                CloseHandle(proc);
                onInstallerExited(path, silent);
            });
    notifier->setEnabled(true);
    return true;
}

void NpcapInstaller::onInstallerExited(const QString &path, bool wasSilent)
{
    detectInstalled();
    if (m_installed) {
        QFile::remove(path);
        setInstalling(false);
        setProgress(0);
        setStatus(m_installedVersion.isEmpty()
                      ? QString::fromUtf8("Npcap 安装完成，透明网关已可用。")
                      : QString::fromUtf8("Npcap %1 安装完成，透明网关已可用。")
                            .arg(m_installedVersion));
        emit finished(true);
        return;
    }
    if (wasSilent) {
        // 免费版安装器拒绝 /S 的典型表现：进程秒退、什么也没装。改用可见向导再来一次。
        setStatus(QString::fromUtf8(
                "静默安装未生效（Npcap 免费版仅 OEM 授权允许静默安装），已打开安装向导，"
                "请按提示点「Install」完成。**记得取消勾选「Restrict Npcap driver's access to "
                "Administrators only」**，否则 Coast 需要管理员权限才能接管设备流量"
                "（装完忘了取消也没关系，回设备页点「修复权限」即可）。"));
        if (!runInstaller(path, false)) {
            setInstalling(false);
            emit finished(false);
        }
        return;
    }
    QFile::remove(path);
    setInstalling(false);
    setProgress(0);
    setStatus(QString::fromUtf8("安装未完成（可能被取消）。可再试一次，或点「官网下载」手动安装。"));
    emit finished(false);
}
#endif // Q_OS_WIN

// 解除「仅管理员可访问」：提权把 AdminOnly 置 0，再重启 npcap 驱动服务让它立即生效。
// 这两件事都要管理员，所以整条命令用 ShellExecuteEx("runas") 交给一个隐藏的 cmd 去跑。
void NpcapInstaller::fixPermissions()
{
#if defined(Q_OS_WIN)
    if (busy()) {
        return;
    }
    detectInstalled();
    if (!m_installed) {
        setStatus(QString::fromUtf8("还没装 Npcap —— 请先点「立即安装」。"));
        return;
    }
    if (!m_restricted) {
        setStatus(QString::fromUtf8("Npcap 驱动权限正常，无需修复。"));
        return;
    }

    // 注册表路径里没有空格，所以整条命令不带引号——cmd /c 的引号剥离规则很容易踩坑，能不用就不用。
    // 用 `&&` 串 reg→net stop：改不成注册表就别去动驱动；用 `&` 串 net start：stop 失败（驱动正
    // 被占用）时也要把它重新拉起来，不能把用户的抓包驱动停在停止态。整条命令的退出码 = net start
    // 的退出码，据此区分「已生效」和「要重启电脑才生效」。
    static const wchar_t *kFixCmd =
            L"/c reg add HKLM\\SYSTEM\\CurrentControlSet\\Services\\npcap\\Parameters "
            L"/v AdminOnly /t REG_DWORD /d 0 /f && net stop npcap & net start npcap";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = kFixCmd;
    sei.nShow = SW_HIDE; // 不闪黑框

    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        setStatus(e == ERROR_CANCELLED
                          ? QString::fromUtf8("已取消：解除驱动限制需要管理员权限。")
                          : QString::fromUtf8("提权失败（错误码 %1）—— 也可以直接以管理员身份"
                                              "运行 Coast 来绕过该限制。")
                                    .arg(e));
        return;
    }

    setInstalling(true); // 复用 busy：期间按钮置灰
    setStatus(QString::fromUtf8("正在解除 Npcap 的「仅管理员」限制…"));

    HANDLE proc = sei.hProcess;
    auto *notifier = new QWinEventNotifier(proc, this);
    connect(notifier, &QWinEventNotifier::activated, this, [this, notifier, proc](HANDLE) {
        notifier->setEnabled(false); // 句柄保持有信号态，先关掉避免反复触发
        notifier->deleteLater();
        DWORD code = 1;
        if (!GetExitCodeProcess(proc, &code)) {
            code = 1;
        }
        CloseHandle(proc);
        onFixExited(code == 0);
    });
    notifier->setEnabled(true);
#endif
}

#if defined(Q_OS_WIN)
void NpcapInstaller::onFixExited(bool driverRestarted)
{
    detectInstalled();
    setInstalling(false);
    if (m_restricted) {
        // 注册表都没改成 —— UAC 被拒或 reg 失败。
        setStatus(QString::fromUtf8("未能解除限制（可能取消了 UAC）。可以再试一次，"
                                    "或直接以管理员身份运行 Coast。"));
        emit finished(false);
        return;
    }
    if (!driverRestarted) {
        // AdminOnly 已置 0，但驱动没能重启（多半是正被别的抓包程序占用）。限制要到下次
        // 驱动加载才真正解除，这时候谎报成功只会让用户回设备页继续撞同一堵墙。
        setStatus(QString::fromUtf8("限制已解除，但 npcap 驱动未能重启（可能正被其它抓包程序占用）"
                                    "—— 重启电脑后「代理网络」即可使用。"));
        emit finished(false);
        return;
    }
    setStatus(QString::fromUtf8("已解除限制，透明网关现在可用。"));
    emit finished(true); // main 里连着 → 触发一次重扫，重新 open 二层端点
}
#endif // Q_OS_WIN
