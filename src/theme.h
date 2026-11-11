#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor darkBackground READ darkBackground NOTIFY changed)
    Q_PROPERTY(QColor darkerBackground READ darkerBackground NOTIFY changed)
    Q_PROPERTY(QColor lighterBackground READ lighterBackground NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor darkForeground READ darkForeground NOTIFY changed)
    Q_PROPERTY(QColor lightForeground READ lightForeground NOTIFY changed)
    Q_PROPERTY(QColor brightForeground READ brightForeground NOTIFY changed)
    Q_PROPERTY(QColor red READ red NOTIFY changed)
    Q_PROPERTY(QColor yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QColor orange READ orange NOTIFY changed)
    Q_PROPERTY(QColor green READ green NOTIFY changed)
    Q_PROPERTY(QColor cyan READ cyan NOTIFY changed)
    Q_PROPERTY(QColor blue READ blue NOTIFY changed)
    Q_PROPERTY(QColor magenta READ magenta NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(int fontSize READ fontSize NOTIFY changed)

public:
    explicit Theme(QObject *parent = nullptr);

    QString mode() const { return m_mode; }
    QColor accent() const { return m_accent; }
    QColor selection() const { return m_selection; }
    QColor muted() const { return m_muted; }
    QColor background() const { return m_background; }
    QColor darkBackground() const { return m_darkBackground; }
    QColor darkerBackground() const { return m_darkerBackground; }
    QColor lighterBackground() const { return m_lighterBackground; }
    QColor foreground() const { return m_foreground; }
    QColor darkForeground() const { return m_darkForeground; }
    QColor lightForeground() const { return m_lightForeground; }
    QColor brightForeground() const { return m_brightForeground; }
    QColor red() const { return m_red; }
    QColor yellow() const { return m_yellow; }
    QColor orange() const { return m_orange; }
    QColor green() const { return m_green; }
    QColor cyan() const { return m_cyan; }
    QColor blue() const { return m_blue; }
    QColor magenta() const { return m_magenta; }
    int radius() const { return m_radius; }
    int fontSize() const { return m_fontSize; }

    Q_INVOKABLE void reload();
    void loadFromTomlString(const QString &tomlContent);

signals:
    void changed();

private:
    void setupWatcher();

    QFileSystemWatcher m_watcher;
    QString m_mode = QStringLiteral("dark");
    QColor m_accent = QColor(QStringLiteral("#7aa2f7"));
    QColor m_selection = QColor(QStringLiteral("#292e42"));
    QColor m_muted = QColor(QStringLiteral("#414868"));
    QColor m_background = QColor(QStringLiteral("#1a1b26"));
    QColor m_darkBackground = QColor(QStringLiteral("#13141c"));
    QColor m_darkerBackground = QColor(QStringLiteral("#0e0e14"));
    QColor m_lighterBackground = QColor(QStringLiteral("#24283b"));
    QColor m_foreground = QColor(QStringLiteral("#a9b1d6"));
    QColor m_darkForeground = QColor(QStringLiteral("#565f89"));
    QColor m_lightForeground = QColor(QStringLiteral("#b4bee6"));
    QColor m_brightForeground = QColor(QStringLiteral("#c0caf5"));
    QColor m_red = QColor(QStringLiteral("#f7768e"));
    QColor m_yellow = QColor(QStringLiteral("#e0af68"));
    QColor m_orange = QColor(QStringLiteral("#eb927b"));
    QColor m_green = QColor(QStringLiteral("#9ece6a"));
    QColor m_cyan = QColor(QStringLiteral("#449dab"));
    QColor m_blue = QColor(QStringLiteral("#7aa2f7"));
    QColor m_magenta = QColor(QStringLiteral("#ad8ee6"));
    int m_radius = 10;
    int m_fontSize = 15;
};
