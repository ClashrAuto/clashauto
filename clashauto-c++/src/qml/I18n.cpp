#include "I18n.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QQmlEngine>

QString I18n::systemLanguage()
{
    const QLocale loc = QLocale::system();
    switch (loc.language()) {
    case QLocale::Chinese:
        // 简/繁：繁体脚本或台港澳地区 → zh-TW，其余中文 → zh-CN。
        if (loc.script() == QLocale::TraditionalHanScript || loc.territory() == QLocale::Taiwan
            || loc.territory() == QLocale::HongKong || loc.territory() == QLocale::Macao)
            return QStringLiteral("zh-TW");
        return QStringLiteral("zh-CN");
    case QLocale::Japanese:   return QStringLiteral("ja");
    case QLocale::Korean:     return QStringLiteral("ko");
    case QLocale::Russian:    return QStringLiteral("ru");
    case QLocale::Spanish:    return QStringLiteral("es");
    case QLocale::French:     return QStringLiteral("fr");
    case QLocale::German:     return QStringLiteral("de");
    case QLocale::Portuguese: return QStringLiteral("pt-BR");
    case QLocale::Italian:    return QStringLiteral("it");
    case QLocale::Turkish:    return QStringLiteral("tr");
    case QLocale::Vietnamese: return QStringLiteral("vi");
    default:                  return QStringLiteral("en-US"); // 未覆盖语言一律英文
    }
}

bool JsonTranslator::loadMap(const QString &jsonPath)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return false;
    const QJsonObject obj = doc.object();
    m_map.clear();
    m_map.reserve(obj.size());
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
        m_map.insert(it.key(), it.value().toString());
    return !m_map.isEmpty();
}

QString JsonTranslator::translate(const char *context, const char *sourceText,
                                  const char *disambiguation, int n) const
{
    Q_UNUSED(context);
    Q_UNUSED(disambiguation);
    Q_UNUSED(n);
    if (!sourceText)
        return QString();
    const auto it = m_map.constFind(QString::fromUtf8(sourceText));
    if (it != m_map.constEnd() && !it.value().isEmpty())
        return it.value();
    return QString(); // 未命中：返回空 → Qt 用源串（中文）
}

I18n::I18n(QQmlEngine *engine, QObject *parent)
    : QObject(parent), m_engine(engine)
{
}

void I18n::setLanguage(const QString &lang)
{
    if (lang == m_current)
        return;

    // 先卸掉当前翻译器。卸载后若不再装 → 源串(简体中文)生效。
    if (m_active) {
        QCoreApplication::removeTranslator(m_active);
        m_active = nullptr;
    }

    // zh-CN（及空）= 源语言，不装翻译器；其余按 <lang>.json 懒加载并缓存后安装。
    if (!lang.isEmpty() && lang.compare(QStringLiteral("zh-CN"), Qt::CaseInsensitive) != 0) {
        JsonTranslator *t = m_translators.value(lang, nullptr);
        if (!t) {
            t = new JsonTranslator(this);
            if (t->loadMap(QStringLiteral(":/assets/i18n/%1.json").arg(lang)) && !t->isEmpty()) {
                m_translators.insert(lang, t); // 加载成功才缓存
            } else {
                delete t; // 无此语言表：回落源串(中文)
                t = nullptr;
            }
        }
        if (t) {
            QCoreApplication::installTranslator(t);
            m_active = t;
        }
    }

    m_current = lang;
    // 让已加载的 QML 重新求值所有 qsTr 绑定（运行时切换即时生效）；启动前调用时引擎尚空，安全 no-op。
    if (m_engine)
        m_engine->retranslate();
}
