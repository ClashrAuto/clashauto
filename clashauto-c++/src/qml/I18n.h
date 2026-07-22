#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTranslator>

class QQmlEngine;

// 按「源串(中文) → 目标串」映射翻译，忽略 qsTr 的 context。本项目所有 qsTr 的源文就是中文，
// 用一张扁平的 中文→英文 表即可覆盖全 UI，无需 lupdate/.ts/.qm、也无需逐 QML 文件对齐 context。
class JsonTranslator final : public QTranslator
{
    Q_OBJECT
public:
    using QTranslator::QTranslator;
    // 加载扁平 JSON（{ "状态": "Status", ... }），可为 qrc 路径（":/assets/i18n/en-US.json"）。
    bool loadMap(const QString &jsonPath);
    // 仅按 sourceText 命中；未命中返回空串 → Qt 回落用源串（即中文）。
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override { return m_map.isEmpty(); }

private:
    QHash<QString, QString> m_map;
};

// 语言管理：按语言码安装/卸载翻译器，并让 QML 引擎 retranslate() 以运行时即时切换界面语言。
// zh-CN 为默认（简体中文即源串，不装翻译器）；其余语言码 <code> 加载 :/assets/i18n/<code>.json。
// 各语言翻译器按需懒加载并缓存，切换只换「已装」的那个。
class I18n final : public QObject
{
    Q_OBJECT
public:
    explicit I18n(QQmlEngine *engine, QObject *parent = nullptr);

    // 按系统区域推断界面语言码：中(简/繁)/日/韩/俄/西/法/德/葡/意/土/越 → 对应码，其余 → "en-US"。
    static QString systemLanguage();

public slots:
    // 启动时与设置页切换语言时都调它。运行时调用会 engine->retranslate() 让已加载界面即时刷新。
    void setLanguage(const QString &lang);

private:
    QQmlEngine *m_engine = nullptr;
    QHash<QString, JsonTranslator *> m_translators; // 语言码 → 已加载翻译器（懒加载缓存）
    JsonTranslator *m_active = nullptr;             // 当前已安装的翻译器（zh-CN 时为空）
    QString m_current;                              // 当前语言码，避免重复安装/无谓 retranslate
};
