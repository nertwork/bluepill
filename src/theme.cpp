#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

Theme::Theme(QObject *parent) : QObject(parent) {
    reload();
    setupWatcher();
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &) {
        reload();
        setupWatcher();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) {
        reload();
        setupWatcher();
    });
}

void Theme::setupWatcher() {
    const QString home = QDir::homePath();
    const QStringList paths = {
        home + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml"),
        home + QStringLiteral("/.local/state/omarchy/current/theme.generation"),
        home + QStringLiteral("/.local/state/omarchy/current"),
        home + QStringLiteral("/.config/omarchy/current/theme/colors.toml"),
        home + QStringLiteral("/.config/omarchy/shell.toml")
    };
    for (const QString &p : paths) {
        if (QFileInfo::exists(p) && !m_watcher.files().contains(p) && !m_watcher.directories().contains(p)) {
            m_watcher.addPath(p);
        }
    }
}

void Theme::reload() {
    const QString home = QDir::homePath();
    QStringList candidates = {
        home + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml"),
        home + QStringLiteral("/.config/omarchy/current/theme/colors.toml")
    };

    for (const QString &path : candidates) {
        QFile f(path);
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            loadFromTomlString(QString::fromUtf8(f.readAll()));
            break;
        }
    }

    QFile shellFile(home + QStringLiteral("/.config/omarchy/shell.toml"));
    if (shellFile.exists() && shellFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString content = QString::fromUtf8(shellFile.readAll());
        QRegularExpression re(QStringLiteral(R"(base-size\s*=\s*(\d+))"));
        auto m = re.match(content);
        if (m.hasMatch()) {
            bool ok = false;
            int sz = m.captured(1).toInt(&ok);
            if (ok && sz >= 15 && sz <= 28) {
                m_fontSize = sz;
            }
        }
    }

    emit changed();
}

void Theme::loadFromTomlString(const QString &tomlContent) {
    QRegularExpression lineRe(QStringLiteral(R"(^\s*([a-zA-Z0-9_]+)\s*=\s*\"([^\"]+)\")"),
                              QRegularExpression::MultilineOption);
    auto it = lineRe.globalMatch(tomlContent);
    while (it.hasNext()) {
        auto match = it.next();
        const QString key = match.captured(1);
        const QString val = match.captured(2);

        if (key == QStringLiteral("mode")) {
            m_mode = val;
            continue;
        }
        QColor c(val);
        if (!c.isValid()) continue;

        if (key == QStringLiteral("accent")) m_accent = c;
        else if (key == QStringLiteral("selection")) m_selection = c;
        else if (key == QStringLiteral("muted")) m_muted = c;
        else if (key == QStringLiteral("background")) m_background = c;
        else if (key == QStringLiteral("dark_background")) m_darkBackground = c;
        else if (key == QStringLiteral("darker_background")) m_darkerBackground = c;
        else if (key == QStringLiteral("lighter_background")) m_lighterBackground = c;
        else if (key == QStringLiteral("foreground")) m_foreground = c;
        else if (key == QStringLiteral("dark_foreground")) m_darkForeground = c;
        else if (key == QStringLiteral("light_foreground")) m_lightForeground = c;
        else if (key == QStringLiteral("bright_foreground")) m_brightForeground = c;
        else if (key == QStringLiteral("red")) m_red = c;
        else if (key == QStringLiteral("yellow")) m_yellow = c;
        else if (key == QStringLiteral("orange")) m_orange = c;
        else if (key == QStringLiteral("green")) m_green = c;
        else if (key == QStringLiteral("cyan")) m_cyan = c;
        else if (key == QStringLiteral("blue")) m_blue = c;
        else if (key == QStringLiteral("magenta")) m_magenta = c;
    }
    emit changed();
}
