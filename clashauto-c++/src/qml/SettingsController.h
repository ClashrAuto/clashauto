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
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonObject>

class CoreController;
class ClashService;
class QNetworkAccessManager;
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
    Q_PROPERTY(bool mirror READ mirror NOTIFY mirrorChanged)
    Q_PROPERTY(int autoUpdateMinutes READ autoUpdateMinutes CONSTANT)
    Q_PROPERTY(bool themeLight READ themeLight CONSTANT)
    Q_PROPERTY(bool autoTheme READ autoTheme CONSTANT)
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
    Q_PROPERTY(bool coreUpdating READ coreUpdating NOTIFY coreUpdatingChanged)
    Q_PROPERTY(QString coreUpdateStatus READ coreUpdateStatus NOTIFY coreUpdateStatusChanged)
    Q_PROPERTY(bool geoipUpdating READ geoipUpdating NOTIFY geoipUpdatingChanged)
    Q_PROPERTY(QString geoipStatus READ geoipStatus NOTIFY geoipStatusChanged)

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
    bool mirror() const { return m_mirror; }
    int autoUpdateMinutes() const { return m_autoUpdate; }
    bool themeLight() const { return m_themeLight; }
    bool autoTheme() const { return m_autoTheme; }
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
    bool geoipUpdating() const { return m_geoipUpdating; }
    QString geoipStatus() const { return m_geoipStatus; }

    bool macHelperSupported() const;
    QString macHelperStatus() const { return m_macHelperStatus; }
    QString lastMessage() const { return m_lastMessage; }

    // —— 系统 tab：整表保存 + 应用副作用（对齐 Widgets 保存 lambda）——
    // themeLight 由 QML 依当前 Theme.dark 传入；主题即时换肤在 QML 侧完成。
    Q_INVOKABLE void apply(const QString &host, int uiPort, int mixedPort, bool webProxy,
                           bool nodeOnly, bool clearConnections, bool increment, bool closeToTray,
                           bool autoStart, bool nodeNote, bool mirror, int autoUpdate,
                           bool themeLight, bool autoTheme, const QString &language,
                           const QString &allowRule, bool allowUse,
                           const QString &noAllowRule, bool noAllowUse);

    // 即时落盘（无需点「应用」，对齐 Widgets 的 toggled 即时持久化）
    Q_INVOKABLE void setNodeOnly(bool on);
    Q_INVOKABLE void setMirror(bool on);

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
    Q_INVOKABLE void updateCore();   // 从 GitHub 拉最新 mihomo 替换内核（用当前 m_mirror 偏好）
    Q_INVOKABLE void updateGeoip();  // 下载 country.mmdb 到 userDir + bundle

    // —— macOS 免密助手 ——
    Q_INVOKABLE void refreshMacHelperStatus();
    Q_INVOKABLE void installMacHelper();
    Q_INVOKABLE void uninstallMacHelper();

signals:
    void rulesChanged();
    void areasChanged();
    void mirrorChanged();
    void coreUpdatingChanged();
    void coreUpdateStatusChanged();
    void geoipUpdatingChanged();
    void geoipStatusChanged();
    void macHelperStatusChanged();
    void messageChanged();

private:
    void loadInitialValues();      // 从 AppConfigLoader::load() 填初始值
    QString userConfigPath() const;
    QString defaultConfigPath() const; // userDir/default.yaml（缺失时从 bundle 复制）
    void persistConfigBool(const QString &key, bool value);
    void applyAutoStart(bool enabled);

    QJsonArray loadRuleSection(const QString &section) const;
    bool saveRuleSection(const QString &section, const QJsonArray &array);
    void reloadRules();
    void reloadAreas();

    void applyDownloadProxy(QNetworkAccessManager *nam) const;
    void setMessage(const QString &msg);
    void setCoreStatus(const QString &msg, bool updating);
    void setGeoipStatus(const QString &msg, bool updating);
    void startMacHelperApprovalWatch();

    CoreController *m_core = nullptr;
    ClashService *m_clash = nullptr;

    // 下载走代理用的主机/端口（apply 时随端口变更同步）
    QString m_downloadHost = QStringLiteral("127.0.0.1");
    int m_downloadMixedPort = 7890;

    // 初始/当前值
    QString m_userDir;
    QString m_host = QStringLiteral("127.0.0.1");
    int m_uiPort = 9191;
    int m_mixedPort = 7890;
    bool m_webProxy = true;
    bool m_nodeOnly = true;
    bool m_clearConnections = true;
    bool m_increment = false;
    bool m_closeToTray = true;
    bool m_autoStart = false;
    bool m_nodeNote = true;
    bool m_mirror = false;
    int m_autoUpdate = 0;
    bool m_themeLight = false;
    bool m_autoTheme = false;
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

    // 更新进度
    bool m_coreUpdating = false;
    QString m_coreUpdateStatus;
    bool m_geoipUpdating = false;
    QString m_geoipStatus;

    QString m_macHelperStatus;
    QString m_lastMessage;

    QTimer *m_helperApprovalTimer = nullptr;
    int m_helperApprovalTicks = 0;
};
