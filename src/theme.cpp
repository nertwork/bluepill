#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QStyleHints>
#include <QTextStream>

namespace {
QString effectiveThemeHomePath() {
#if defined(Q_OS_WIN) && defined(IMSG_UNIT_TEST)
    const QString homeOverride = qEnvironmentVariable("HOME").trimmed();
    if (!homeOverride.isEmpty()) {
        return QDir::fromNativeSeparators(homeOverride);
    }
#endif
    return QDir::homePath();
}

QString extractTomlMode(const QString &tomlContent) {
    QRegularExpression re(QStringLiteral(R"(^\s*mode\s*=\s*\"([^\"]+)\")"),
                          QRegularExpression::MultilineOption);
    auto m = re.match(tomlContent);
    if (m.hasMatch()) {
        return m.captured(1).trimmed().toLower();
    }
    return QString();
}
} // namespace

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
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::instance()) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
            if (m_themeMode == QStringLiteral("system")) {
                reload();
            }
        });
    }
#endif
}

QString Theme::normalizeThemeMode(const QString &mode) {
    const QString norm = mode.trimmed().toLower();
    if (norm == QStringLiteral("dark") || norm == QStringLiteral("light") || norm == QStringLiteral("system")) {
        return norm;
    }
    return QStringLiteral("system");
}

void Theme::setThemeMode(const QString &mode) {
    const QString norm = normalizeThemeMode(mode);
    const bool modeChanged = (m_themeMode != norm);
    m_themeMode = norm;
    reload();
    if (modeChanged) {
        emit themeModeChanged();
    }
}

void Theme::setSystemColorSchemeForTest(bool isLight) {
    m_hasSystemColorSchemeOverrideForTest = true;
    m_systemColorSchemeOverrideIsLightForTest = isLight;
    if (m_themeMode == QStringLiteral("system")) {
        reload();
    }
}

void Theme::clearSystemColorSchemeOverrideForTest() {
    m_hasSystemColorSchemeOverrideForTest = false;
    m_systemColorSchemeOverrideIsLightForTest = false;
    if (m_themeMode == QStringLiteral("system")) {
        reload();
    }
}

bool Theme::detectSystemIsLight() const {
    if (m_hasSystemColorSchemeOverrideForTest) {
        return m_systemColorSchemeOverrideIsLightForTest;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::instance()) {
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        if (scheme == Qt::ColorScheme::Light) {
            return true;
        }
        if (scheme == Qt::ColorScheme::Dark) {
            return false;
        }
    }
#endif
    const QString home = effectiveThemeHomePath();
    const QStringList candidates = {
        home + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml"),
        home + QStringLiteral("/.config/omarchy/current/theme/colors.toml")
    };
    for (const QString &path : candidates) {
        QFile f(path);
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString tomlMode = extractTomlMode(QString::fromUtf8(f.readAll()));
            if (!tomlMode.isEmpty()) {
                return tomlMode == QStringLiteral("light");
            }
            break;
        }
    }
    return false;
}

void Theme::applySemanticPaletteForMode(bool isLight) {
    if (isLight) {
        m_onAccent = QColor(QStringLiteral("#ffffff"));
        m_incomingBubble = QColor(QStringLiteral("#e2e8f0"));
        m_incomingBubbleBorder = QColor(QStringLiteral("#cbd5e1"));
        m_outgoingBlueBubble = QColor(QStringLiteral("#0A84FF"));
        m_outgoingBlueBorder = QColor(QStringLiteral("#0070e0"));
        m_outgoingGreenBubble = QColor(QStringLiteral("#30D158"));
        m_outgoingGreenBorder = QColor(QStringLiteral("#24b247"));
        m_subtleSurface = QColor(15, 23, 42, 13);
        m_subtleBorder = QColor(15, 23, 42, 30);
        m_overlayBackground = QColor(248, 250, 252, 235);
        m_modalScrim = QColor(15, 23, 42, 110);
        m_invisibleInkIncoming = QColor(QStringLiteral("#cbd5e1"));
    } else {
        m_onAccent = m_darkerBackground;
        m_incomingBubble = QColor(QStringLiteral("#1e2430"));
        m_incomingBubbleBorder = QColor(QStringLiteral("#2c3545"));
        m_outgoingBlueBubble = QColor(QStringLiteral("#0A84FF"));
        m_outgoingBlueBorder = QColor(QStringLiteral("#0066CC"));
        m_outgoingGreenBubble = QColor(QStringLiteral("#1B6E38"));
        m_outgoingGreenBorder = QColor(QStringLiteral("#268C48"));
        m_subtleSurface = QColor(255, 255, 255, 13);
        m_subtleBorder = QColor(255, 255, 255, 26);
        m_overlayBackground = QColor(QStringLiteral("#cc131620"));
        m_modalScrim = QColor(QStringLiteral("#aa0e0e14"));
        m_invisibleInkIncoming = QColor(QStringLiteral("#252d3d"));
    }
}

void Theme::applyDefaultDarkPalette() {
    m_mode = QStringLiteral("dark");
    m_accent = QColor(QStringLiteral("#7aa2f7"));
    m_selection = QColor(QStringLiteral("#292e42"));
    m_muted = QColor(QStringLiteral("#414868"));
    m_background = QColor(QStringLiteral("#1a1b26"));
    m_darkBackground = QColor(QStringLiteral("#13141c"));
    m_darkerBackground = QColor(QStringLiteral("#0e0e14"));
    m_lighterBackground = QColor(QStringLiteral("#24283b"));
    m_foreground = QColor(QStringLiteral("#a9b1d6"));
    m_darkForeground = QColor(QStringLiteral("#565f89"));
    m_lightForeground = QColor(QStringLiteral("#b4bee6"));
    m_brightForeground = QColor(QStringLiteral("#c0caf5"));
    m_red = QColor(QStringLiteral("#f7768e"));
    m_yellow = QColor(QStringLiteral("#e0af68"));
    m_orange = QColor(QStringLiteral("#eb927b"));
    m_green = QColor(QStringLiteral("#9ece6a"));
    m_cyan = QColor(QStringLiteral("#449dab"));
    m_blue = QColor(QStringLiteral("#7aa2f7"));
    m_magenta = QColor(QStringLiteral("#ad8ee6"));
    applySemanticPaletteForMode(false);
}

void Theme::applyDefaultLightPalette() {
    m_mode = QStringLiteral("light");
    m_accent = QColor(QStringLiteral("#0A84FF"));
    m_selection = QColor(QStringLiteral("#dbeafe"));
    m_muted = QColor(QStringLiteral("#cbd5e1"));
    m_background = QColor(QStringLiteral("#f5f7fb"));
    m_darkBackground = QColor(QStringLiteral("#ffffff"));
    m_darkerBackground = QColor(QStringLiteral("#ffffff"));
    m_lighterBackground = QColor(QStringLiteral("#eef2f7"));
    m_foreground = QColor(QStringLiteral("#1e293b"));
    m_darkForeground = QColor(QStringLiteral("#475569"));
    m_lightForeground = QColor(QStringLiteral("#334155"));
    m_brightForeground = QColor(QStringLiteral("#0f172a"));
    m_red = QColor(QStringLiteral("#dc2626"));
    m_yellow = QColor(QStringLiteral("#d97706"));
    m_orange = QColor(QStringLiteral("#ea580c"));
    m_green = QColor(QStringLiteral("#16a34a"));
    m_cyan = QColor(QStringLiteral("#0284c7"));
    m_blue = QColor(QStringLiteral("#0A84FF"));
    m_magenta = QColor(QStringLiteral("#7c3aed"));
    applySemanticPaletteForMode(true);
}

void Theme::setupWatcher() {
    const QString home = effectiveThemeHomePath();
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
    bool wantLight = false;
    if (m_themeMode == QStringLiteral("light")) {
        wantLight = true;
    } else if (m_themeMode == QStringLiteral("dark")) {
        wantLight = false;
    } else {
        wantLight = detectSystemIsLight();
    }

    if (wantLight) {
        applyDefaultLightPalette();
    } else {
        applyDefaultDarkPalette();
    }

    const QString home = effectiveThemeHomePath();
    const QStringList candidates = {
        home + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml"),
        home + QStringLiteral("/.config/omarchy/current/theme/colors.toml")
    };

    for (const QString &path : candidates) {
        QFile f(path);
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(f.readAll());
            const QString tomlMode = extractTomlMode(content);
            const bool tomlIsLight = (tomlMode == QStringLiteral("light"));
            // Preserve live Omarchy theme integration whenever the Omarchy theme polarity
            // matches the active light/dark mode (so explicit Light mode stays Light even on dark Omarchy).
            if (tomlIsLight == wantLight) {
                loadFromTomlString(content);
            }
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
            m_mode = val.trimmed().toLower();
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
    applySemanticPaletteForMode(!isDark());
    emit changed();
}
