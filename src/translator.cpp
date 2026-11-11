#include "translator.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

Translator::Translator(QObject *parent) : QObject(parent) {
    QString envLang = qEnvironmentVariable("IMSG_LANG");
    if (envLang.isEmpty()) {
        envLang = QLocale::system().name().section(QLatin1Char('_'), 0, 0);
    }
    loadLanguage(envLang);
}

void Translator::loadLanguage(const QString &langCode) {
    m_strings.clear();
    m_language = langCode.isEmpty() ? QStringLiteral("en") : langCode.toLower();

    QFile file(QStringLiteral(":/i18n/%1.json").arg(m_language));
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                m_strings.insert(it.key(), it.value().toString());
            }
        }
    }
    emit languageChanged();
}

QString Translator::tr(const QString &key) const {
    return m_strings.value(key, key);
}
