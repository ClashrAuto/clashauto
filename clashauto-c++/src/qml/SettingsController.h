#pragma once

// SettingsController —— 设置页（QML）与既有后端之间的适配层。
// 设计同 QmlBridge（见 qml/ARCHITECTURE.md）：后端类原样复用、不改；本类只：
//   1) 把 config.yaml / userDir/default.yaml 的读写「翻译」成 QML 友好的属性 / QVariantList；
//   2) 复刻 Widgets 版 MainWindow::buildSettingsPage() 的保存/应用副作用（写盘 + 生效）；
//   3) 复刻规则/区域表（loadRuleSection/saveRuleSection/openRuleEditor/openAreaEditor）；
//   4) 复刻内核更新（updateMihomoCore）、GeoIP 更新、开机自启，以及 macOS 免密助手。
// 后端对象生命周期由 main_qml.cpp 拥有；本类只持裸指针、不接管所有权。
//
// 与 Widgets 版一一对应，但无 QWidget：进度/状态经 Q_PROPERTY(NOTIFY) 回报给 QML。

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonObject>

#include "DownloadStats.h"

class CoreController;
class ClashService;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class SettingsController final : public QObject
{
    Q_OBJECT

    // —— 初始值（构建表单时读一次；随后表单本地持有编辑态）——
    Q_PROPERTY(QString host READ host CONSTANT)
    Q_PROPERTY(int uiPort READ uiPort CONSTANT)
    Q_PROPERTY(int mixedPort READ mixedPort CONSTANT)
    Q_PROPERTY(bool webProxy READ webProxy CONSTANT)
    Q_PROPERTY(bool nodeOnlyAvailable READ nodeOnlyAvailable CONSTANT)
    Q_PROPERTY(bool clearConnections READ clearConnections CONSTANT)
    Q_PROPERTY(bool increment READ increment CONSTANT)
    Q_PROPERTY(bool closeToTray READ closeToTray CONSTANT)
    Q_PROPERTY(bool autoStart READ autoStart CONSTANT)
    Q_PROPERTY(bool nodeSwitchNote READ nodeSwitchNote CONSTANT)
    Q_PROPERTY(int geoipUpdateDays READ geoipUpdateDays NOTIFY geoipUpdateDaysChanged)
    // 本地 Country.mmdb 的**构建日期**（metadata 里的 build_epoch，已格成 yyyy-MM-dd）。
    // 读不出来就是空串。随 GeoIP 更新成功刷新。
    Q_PROPERTY(QString geoipBuildDate READ geoipBuildDate NOTIFY geoipBuildDateChanged)
    Q_PROPERTY(bool receiveBeta READ receiveBeta NOTIFY receiveBetaChanged)
    Q_PROPERTY(bool ipv6 READ ipv6 NOTIFY ipv6Changed)
    Q_PROPERTY(int autoUpdateMinutes READ autoUpdateMinutes CONSTANT)
    Q_PROPERTY(bool themeLight READ themeLight CONSTANT)
    Q_PROPERTY(bool autoTheme READ autoTheme CONSTANT)
    Q_PROPERTY(bool autoLanguage READ autoLanguage CONSTANT)
    Q_PROPERTY(QString language READ language CONSTANT)
    Q_PROPERTY(bool allowRuleEnabled READ allowRuleEnabled CONSTANT)
    Q_PROPERTY(QString allowRule READ allowRule CONSTANT)
    Q_PROPERTY(bool noAllowRuleEnabled READ noAllowRuleEnabled CONSTANT)
    Q_PROPERTY(QString noAllowRule READ noAllowRule CONSTANT)

    // 预置正则（过滤 tab 的可编辑下拉候选，对齐 Widgets）
    Q_PROPERTY(QStringList allowRulePresets READ allowRulePresets CONSTANT)
    Q_PROPERTY(QStringList noAllowRulePresets READ noAllowRulePresets CONSTANT)

    // —— 规则 / 区域 表 ——
    Q_PROPERTY(QVariantList ruleRows READ ruleRows NOTIFY rulesChanged)
    Q_PROPERTY(int ruleTotal READ ruleTotal NOTIFY rulesChanged)
    Q_PROPERTY(int ruleShown READ ruleShown NOTIFY rulesChanged)
    Q_PROPERTY(QVariantList areaRows READ areaRows NOTIFY areasChanged)
    Q_PROPERTY(int areaTotal READ areaTotal NOTIFY areasChanged)

    // —— 内核 / GeoIP 更新进度 ——
    //
    // 与程序更新（`UpdateController`）**同一套读数**：百分比 + 速度 + 已下载/总量，
    // 外加一个能中止的入口。更新窗三个页签共用一份版式（进度条 + ✕），缺哪一样
    // 内核/GeoIP 那两页就只能退回一行「下载中 45%」的文字，和程序页对不上。
    Q_PROPERTY(bool coreUpdating READ coreUpdating NOTIFY coreUpdatingChanged)
    Q_PROPERTY(QString coreUpdateStatus READ coreUpdateStatus NOTIFY coreUpdateStatusChanged)
    Q_PROPERTY(int coreProgress READ coreProgress NOTIFY coreProgressChanged)
    Q_PROPERTY(QString coreSpeedText READ coreSpeedText NOTIFY coreProgressChanged)
    Q_PROPERTY(QString coreDownloadedText READ coreDownloadedText NOTIFY coreProgressChanged)
    Q_PROPERTY(QString coreTotalText READ coreTotalText NOTIFY coreProgressChanged)
    Q_PROPERTY(bool geoipUpdating READ geoipUpdating NOTIFY geoipUpdatingChanged)
    Q_PROPERTY(QString geoipStatus READ geoipStatus NOTIFY geoipStatusChanged)
    Q_PROPERTY(int geoipProgress READ geoipProgress NOTIFY geoipProgressChanged)
    Q_PROPERTY(QString geoipSpeedText READ geoipSpeedText NOTIFY geoipProgressChanged)
    Q_PROPERTY(QString geoipDownloadedText READ geoipDownloadedText NOTIFY geoipProgressChanged)
    Q_PROPERTY(QString geoipTotalText READ geoipTotalText NOTIFY geoipProgressChanged)

    // —— macOS 免密助手 ——
    Q_PROPERTY(bool macHelperSupported READ macHelperSupported CONSTANT)
    Q_PROPERTY(QString macHelperStatus READ macHelperStatus NOTIFY macHelperStatusChanged)

    // —— 通用反馈（保存成功等），供页面顶部小提示 ——
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY messageChanged)

public:
    SettingsController(CoreController *core, ClashService *clash, QObject *parent = nullptr);

    // 初始值 getter
    QString host() const { return m_host; }
    int uiPort() const { return m_uiPort; }
    int mixedPort() const { return m_mixedPort; }
    bool webProxy() const { return m_webProxy; }
    bool nodeOnlyAvailable() const { return m_nodeOnly; }
    bool clearConnections() const { return m_clearConnections; }
    bool increment() const { return m_increment; }
    bool closeToTray() const { return m_closeToTray; }
    bool autoStart() const { return m_autoStart; }
    bool nodeSwitchNote() const { return m_nodeNote; }
    int geoipUpdateDays() const { return m_geoipUpdateDays; }
    QString geoipBuildDate() const { return m_geoipBuildDate; }
    bool receiveBeta() const { return m_receiveBeta; }
    bool ipv6() const { return m_ipv6; }
    int autoUpdateMinutes() const { return m_autoUpdate; }
    bool themeLight() const { return m_themeLight; }
    bool autoTheme() const { return m_autoTheme; }
    bool autoLanguage() const { return m_autoLanguage; }
    QString language() const { return m_language; }
    bool allowRuleEnabled() const { return m_allowUse; }
    QString allowRule() const { return m_allowRule; }
    bool noAllowRuleEnabled() const { return m_noAllowUse; }
    QString noAllowRule() const { return m_noAllowRule; }
    QStringList allowRulePresets() const;
    QStringList noAllowRulePresets() const;

    QVariantList ruleRows() const { return m_ruleRows; }
    int ruleTotal() const { return m_ruleTotal; }
    int ruleShown() const { return m_ruleRows.size(); }
    QVariantList areaRows() const { return m_areaRows; }
    int areaTotal() const { return m_areaRows.size(); }

    bool coreUpdating() const { return m_coreUpdating; }
    QString coreUpdateStatus() const { return m_coreUpdateStatus; }
    int coreProgress() const { return m_coreProgress; }
    QString coreSpeedText() const { return m_coreStats.speedText; }
    QString coreDownloadedText() const { return m_coreStats.downloadedText; }
    QString coreTotalText() const { return m_coreStats.totalText; }
    bool geoipUpdating() const { return m_geoipUpdating; }
    QString geoipStatus() const { return m_geoipStatus; }
    int geoipProgress() const { return m_geoipProgress; }
    QString geoipSpeedText() const { return m_geoipStats.speedText; }
    QString geoipDownloadedText() const { return m_geoipStats.downloadedText; }
    QString geoipTotalText() const { return m_geoipStats.totalText; }

    bool macHelperSupported() const;
    QString macHelperStatus() const { return m_macHelperStatus; }
    QString lastMessage() const { return m_lastMessage; }

    // —— 系统 tab：整表保存 + 应用副作用（对齐 Widgets 保存 lambda）——
    // themeLight 由 QML 依当前 Theme.dark 传入；主题即时换肤在 QML 侧完成。
    Q_INVOKABLE void apply(const QString &host, int uiPort, int mixedPort, bool webProxy,
                           bool nodeOnly, bool clearConnections, bool increment, bool closeToTray,
                           bool autoStart, bool nodeNote, int autoUpdate,
                           bool themeLight, bool autoTheme, const QString &language, bool autoLanguage,
                           const QString &allowRule, bool allowUse,
                           const QString &noAllowRule, bool noAllowUse);

    // 即时落盘（无需点「应用」，对齐 Widgets 的 toggled 即时持久化）
    Q_INVOKABLE void setNodeOnly(bool on);
    Q_INVOKABLE void setGeoipUpdateDays(int days); // geoipUpdate：GeoIP 自动更新周期（天，0=关）
    Q_INVOKABLE void setReceiveBeta(bool on);
    Q_INVOKABLE void setIpv6(bool on);

    // 系统 tab 开关/下拉全部即时落盘+即时生效；「应用」按钮仍整表保存（Host/端口等文本项用）。
    Q_INVOKABLE void setAutoStart(bool on);          // sys：写注册表 Run 键
    Q_INVOKABLE void setCloseToTray(bool on);        // mini（✕ 行为由 QML 侧同步 bridge）
    Q_INVOKABLE void setIncrement(bool on);          // increment
    Q_INVOKABLE void setNodeNote(bool on);           // note（通知开关由 QML 侧同步 bridge）
    Q_INVOKABLE void setWebProxy(bool on);           // web：与当前代理态不一致则立即切换
    Q_INVOKABLE void setClearConnections(bool on);   // clearConnections：同步 ClashService
    Q_INVOKABLE void setAutoTheme(bool on);          // autoTheme（跟随即时换肤由 QML 侧同步 bridge）
    Q_INVOKABLE void setThemeLight(bool light);      // theme: light|black
    Q_INVOKABLE void setAutoUpdateMinutes(int minutes); // autoUpdate
    Q_INVOKABLE void setLanguage(const QString &lang);  // language：生效码变了发 languageChangeRequested
    Q_INVOKABLE void setAutoLanguage(bool on);          // autoLanguage：同上

    // —— 规则 / 区域 ——
    Q_INVOKABLE void setRuleFilter(const QString &text);
    Q_INVOKABLE QVariantMap ruleAt(int index) const;   // 编辑器回填：{type,node,value}
    Q_INVOKABLE QVariantMap areaAt(int index) const;   // 编辑器回填：{name,type,proxiesText,url,interval}
    Q_INVOKABLE void saveRule(int editIndex, const QString &type, const QString &value, const QString &node);
    Q_INVOKABLE void deleteRule(int index);
    Q_INVOKABLE void saveArea(int editIndex, const QString &name, const QString &type,
                              const QString &proxiesText, const QString &url, const QString &interval);
    Q_INVOKABLE QStringList proxyGroupNames() const;   // 规则编辑器「节点/策略组」下拉
    Q_INVOKABLE QStringList processChoices(bool wantPath) const; // PROCESS-NAME/PATH 值下拉

    // —— 更新 ——
    // 从 GitHub 拉最新 mihomo 替换内核。入口只在**更新窗的内核页**（设置页那颗按钮已撤）。
    Q_INVOKABLE void updateCore();
    Q_INVOKABLE void updateGeoip();  // 下载 country.mmdb 到 userDir + bundle
    // 中止在途的下载（更新窗那颗 ✕）。**只结束这次下载，不关窗**：结束后底栏自己回到
    // 「更新 / 获取更新」，用户可以直接再来一次。与 UpdateController::cancelDownload 同义。
    Q_INVOKABLE void cancelCoreUpdate();
    Q_INVOKABLE void cancelGeoipUpdate();
    // 从磁盘上那份 mmdb 重读 build_epoch。设置页进页时叫一次 ——
    // 后台那条静默下载（AboutController::checkGeoip）不经本类，不重读就看不到它拉下来的那份。
    Q_INVOKABLE void refreshGeoipBuildDate();

    // —— macOS 免密助手 ——
    Q_INVOKABLE void refreshMacHelperStatus();
    Q_INVOKABLE void installMacHelper();
    Q_INVOKABLE void uninstallMacHelper();

signals:
    void rulesChanged();
    void areasChanged();
    void geoipUpdateDaysChanged();
    void geoipBuildDateChanged();
    void receiveBetaChanged();
    void ipv6Changed();
    void coreUpdatingChanged();
    void coreUpdateStatusChanged();
    void coreProgressChanged();
    void geoipUpdatingChanged();
    void geoipStatusChanged();
    void geoipProgressChanged();
    void macHelperStatusChanged();
    void messageChanged();
    // 「应用」时语言码变化：main_qml 连到 I18n::setLanguage 做运行时切换（装/卸翻译器 + retranslate）。
    void languageChangeRequested(const QString &lang);
    // 内核二进制**真的被换掉了**（updateCore 替换成功那一刻）。
    //
    // ★ main_qml 把它连到 `AboutController::checkCore`。不连的话侧栏那颗 "core" 角标
    //   要等下一轮**每小时**的后台 checkAll 才会灭 —— 用户刚在更新窗里看着「内核已更新到
    //   v1.10.xxxx」，转头侧栏还在红着说有新内核，看着像更新根本没成。
    //   角标的判据是「本地 `core -v` 与清单里那版是否不同」，而替换掉二进制正是
    //   唯一能让这个判据翻面的事件 —— 所以信号发在这儿，不是发在「下载完成」。
    void coreReplaced();

private:
    void loadInitialValues();      // 从 AppConfigLoader::load() 填初始值
    QString userConfigPath() const;
    QString defaultConfigPath() const; // userDir/default.yaml（缺失时从 bundle 复制）
    void persistConfigBool(const QString &key, bool value);
    void persistConfigScalar(const QString &key, const QString &value); // 单键改写，保留其余内容
    void applyEffectiveLanguage(); // 依 m_autoLanguage/m_language 重算生效语言，变了发信号
    void applyAutoStart(bool enabled);

    QJsonArray loadRuleSection(const QString &section) const;
    bool saveRuleSection(const QString &section, const QJsonArray &array);
    void reloadRules();
    void reloadAreas();

    void applyDownloadProxy(QNetworkAccessManager *nam) const;
    void setMessage(const QString &msg);
    void setCoreStatus(const QString &msg, bool updating);
    void setGeoipStatus(const QString &msg, bool updating);
    // 把在途 reply 记下来（供 cancel 用）并复位那一组进度读数。传 nullptr = 这一轮结束了。
    void trackCoreReply(QNetworkReply *reply);
    void trackGeoipReply(QNetworkReply *reply);
    void startMacHelperApprovalWatch();

    CoreController *m_core = nullptr;
    ClashService *m_clash = nullptr;

    // 下载走代理用的主机/端口（apply 时随端口变更同步）
    QString m_downloadHost = QStringLiteral("127.0.0.1");
    int m_downloadMixedPort = 7890;

    // 初始/当前值
    QString m_configDir;
    QString m_host = QStringLiteral("127.0.0.1");
    int m_uiPort = 9191;
    int m_mixedPort = 7890;
    bool m_webProxy = true;
    bool m_nodeOnly = true;
    bool m_clearConnections = true;
    bool m_increment = false;
    bool m_closeToTray = false; // 默认关（实际值由 config 覆盖）
    bool m_autoStart = false;
    bool m_nodeNote = true;
    int m_geoipUpdateDays = 1;
    QString m_geoipBuildDate;
    bool m_receiveBeta = false;
    bool m_ipv6 = false;
    int m_autoUpdate = 0;
    bool m_themeLight = false;
    bool m_autoTheme = false;
    bool m_autoLanguage = true; // 默认开，跟随系统语言（实际值由 config 覆盖）
    QString m_effectiveLang = QStringLiteral("zh-CN"); // 当前实际生效的语言码（跟随系统时=系统语言）
    QString m_language = QStringLiteral("zh-CN");
    bool m_allowUse = false;
    QString m_allowRule;
    bool m_noAllowUse = false;
    QString m_noAllowRule;

    // 表
    QString m_ruleFilter;
    QVariantList m_ruleRows;
    int m_ruleTotal = 0;
    QVariantList m_areaRows;

    // 更新进度。`QPointer` 而不是裸指针：reply 走的是 `deleteLater()`，用户点 ✕ 的那一拍
    // 它可能已经被删了，裸指针在这里就是个悬空指针 + 一次 abort() 崩溃。
    bool m_coreUpdating = false;
    QString m_coreUpdateStatus;
    int m_coreProgress = 0;
    DownloadStats m_coreStats;
    QPointer<QNetworkReply> m_coreReply;
    bool m_coreCancelled = false;
    bool m_geoipUpdating = false;
    QString m_geoipStatus;
    int m_geoipProgress = 0;
    DownloadStats m_geoipStats;
    QPointer<QNetworkReply> m_geoipReply;
    bool m_geoipCancelled = false;

    QString m_macHelperStatus;
    QString m_lastMessage;

    QTimer *m_helperApprovalTimer = nullptr;
    int m_helperApprovalTicks = 0;
};
