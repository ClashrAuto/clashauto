#include "I18n.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>

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

    // 先卸掉英文翻译器（切到中文或换语言时）。移除后未装任何翻译器 → 源串(中文)生效。
    if (m_en)
        QCoreApplication::removeTranslator(m_en);

    // en-US / en*：装英文翻译器（懒加载一次）。其它一律回落中文源串。
    if (lang.compare(QStringLiteral("en-US"), Qt::CaseInsensitive) == 0
        || lang.startsWith(QStringLiteral("en"), Qt::CaseInsensitive)) {
        if (!m_en) {
            m_en = new JsonTranslator(this);
            m_en->loadMap(QStringLiteral(":/assets/i18n/en-US.json"));
        }
        if (m_en && !m_en->isEmpty())
            QCoreApplication::installTranslator(m_en);
    }

    m_current = lang;
    // 让已加载的 QML 重新求值所有 qsTr 绑定（运行时切换即时生效）；启动前调用时引擎尚空，安全 no-op。
    if (m_engine)
        m_engine->retranslate();
}
