#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class Translator : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)

public:
    explicit Translator(QObject *parent = nullptr);

    QString language() const { return m_language; }
    Q_INVOKABLE QString tr(const QString &key) const;
    Q_INVOKABLE void loadLanguage(const QString &langCode);

signals:
    void languageChanged();

private:
    QString m_language = QStringLiteral("en");
    QHash<QString, QString> m_strings;
};
