#include "spellchecker.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextCharFormat>
#include <QUrl>
#include <QUrlQuery>

#ifdef Q_OS_WIN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <spellcheck.h>
#endif

static QString spellEffectiveHomePath() {
#if defined(Q_OS_WIN) && defined(IMSG_UNIT_TEST)
    const QString homeOverride = qEnvironmentVariable("HOME").trimmed();
    if (!homeOverride.isEmpty()) {
        return QDir::fromNativeSeparators(homeOverride);
    }
#endif
    return QDir::homePath();
}

SpellHighlighter::SpellHighlighter(SpellChecker *checker, QTextDocument *parent)
    : QSyntaxHighlighter(parent), m_checker(checker) {
    if (m_checker) {
        connect(m_checker, &SpellChecker::spellCheckEnabledChanged, this, &QSyntaxHighlighter::rehighlight);
        connect(m_checker, &SpellChecker::dictionaryChanged, this, &QSyntaxHighlighter::rehighlight);
    }
}

void SpellHighlighter::highlightBlock(const QString &text) {
    if (!m_checker || !m_checker->spellCheckEnabled() || !m_checker->isAvailable() || text.isEmpty()) {
        return;
    }

    // Collect URL and email spans so we never flag domains/paths as typos
    QList<QPair<int, int>> skipSpans;
    static const QRegularExpression reUrlOrEmail(
        QStringLiteral(R"((?:https?://\S+|www\.\S+|[\w.+-]+@[\w-]+\.[\w.-]+))"),
        QRegularExpression::CaseInsensitiveOption);
    auto urlIt = reUrlOrEmail.globalMatch(text);
    while (urlIt.hasNext()) {
        const QRegularExpressionMatch m = urlIt.next();
        skipSpans.append({int(m.capturedStart()), int(m.capturedEnd())});
    }

    static const QRegularExpression reWord(
        QStringLiteral(R"((?<![\p{L}\p{N}_])[\p{L}](?:[\p{L}'’]*[\p{L}])?(?![\p{L}\p{N}_]))"));
    auto it = reWord.globalMatch(text);
    QTextCharFormat fmt;
    fmt.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    fmt.setUnderlineColor(QColor(QStringLiteral("#EF4444")));

    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int start = int(m.capturedStart());
        const int len = int(m.capturedLength());
        if (len <= 1) continue;

        bool insideSkip = false;
        for (const auto &sp : skipSpans) {
            if (start < sp.second && (start + len) > sp.first) {
                insideSkip = true;
                break;
            }
        }
        if (insideSkip) continue;

        const QString word = m.captured();
        // Skip all-uppercase acronyms (e.g. HTTP, USA, API)
        if (word == word.toUpper() && word != word.toLower()) continue;

        if (m_checker->isWordMisspelled(word)) {
            setFormat(start, len, fmt);
        }
    }
}

SpellChecker::SpellChecker(QObject *parent) : QObject(parent) {
    m_userDictPath = defaultUserDictionaryPath();
    m_settingsPath = defaultSettingsFilePath();

    m_assistDebounceTimer.setSingleShot(true);
    m_assistDebounceTimer.setInterval(280);
    connect(&m_assistDebounceTimer, &QTimer::timeout, this, &SpellChecker::dispatchAssistRequest);

    loadSettings();
    loadUserDictionary();
    initNativeSpellChecker();
}

SpellChecker::~SpellChecker() {
    m_assistDebounceTimer.stop();
    if (QNetworkReply *reply = m_assistReply.data()) {
        m_assistReply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    cleanupNativeSpellChecker();
}

QString SpellChecker::defaultUserDictionaryPath() {
#ifdef Q_OS_WIN
    QString appData = qEnvironmentVariable("APPDATA").trimmed();
    if (appData.isEmpty()) {
        appData = spellEffectiveHomePath() + QStringLiteral("/AppData/Roaming");
    }
    return QDir::cleanPath(appData + QStringLiteral("/bluepill/dictionary.txt"));
#else
    return spellEffectiveHomePath() + QStringLiteral("/.config/bluepill/dictionary.txt");
#endif
}

QString SpellChecker::defaultSettingsFilePath() {
#ifdef Q_OS_WIN
    QString appData = qEnvironmentVariable("APPDATA").trimmed();
    if (appData.isEmpty()) {
        appData = spellEffectiveHomePath() + QStringLiteral("/AppData/Roaming");
    }
    return QDir::cleanPath(appData + QStringLiteral("/bluepill/spellcheck.json"));
#else
    return spellEffectiveHomePath() + QStringLiteral("/.config/bluepill/spellcheck.json");
#endif
}

QString SpellChecker::normalizeWord(const QString &word) {
    QString w = word.trimmed();
    w.replace(QChar(0x2019), QLatin1Char('\''));
    w.replace(QChar(0x2018), QLatin1Char('\''));
    return w;
}

void SpellChecker::setAssistDebounceMsForTest(int ms) {
    m_assistDebounceTimer.setInterval(qMax(1, ms));
}

void SpellChecker::setUserDictionaryPathForTest(const QString &path) {
    m_userDictPath = path;
    loadUserDictionary();
    emit dictionaryChanged();
}

void SpellChecker::setSettingsFilePathForTest(const QString &path) {
    m_settingsPath = path;
    loadSettings();
}

void SpellChecker::reloadForTest() {
    m_userDictPath = defaultUserDictionaryPath();
    m_settingsPath = defaultSettingsFilePath();
    loadSettings();
    loadUserDictionary();
    emit dictionaryChanged();
}

void SpellChecker::loadSettings() {
    const bool prevSpell = m_spellCheckEnabled;
    const bool prevAuto = m_autoSuggestEnabled;
    m_spellCheckEnabled = true;
    m_autoSuggestEnabled = true;

    if (!m_settingsPath.isEmpty()) {
        QFile f(m_settingsPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            if (obj.contains(QStringLiteral("spellCheckEnabled"))) {
                m_spellCheckEnabled = obj.value(QStringLiteral("spellCheckEnabled")).toBool(true);
            }
            if (obj.contains(QStringLiteral("autoSuggestEnabled"))) {
                m_autoSuggestEnabled = obj.value(QStringLiteral("autoSuggestEnabled")).toBool(true);
            }
        }
    }

    if (prevSpell != m_spellCheckEnabled) emit spellCheckEnabledChanged();
    if (prevAuto != m_autoSuggestEnabled) emit autoSuggestEnabledChanged();
}

void SpellChecker::saveSettings() const {
    if (m_settingsPath.isEmpty()) return;
    QDir().mkpath(QFileInfo(m_settingsPath).absolutePath());
    QJsonObject obj;
    obj.insert(QStringLiteral("spellCheckEnabled"), m_spellCheckEnabled);
    obj.insert(QStringLiteral("autoSuggestEnabled"), m_autoSuggestEnabled);
    QSaveFile f(m_settingsPath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

void SpellChecker::loadUserDictionary() {
    m_userWords.clear();
    m_userWordsLower.clear();
    if (m_userDictPath.isEmpty()) return;

    QFile f(m_userDictPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    while (!f.atEnd()) {
        const QString line = normalizeWord(QString::fromUtf8(f.readLine()));
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        m_userWords.insert(line);
        m_userWordsLower.insert(line.toLower());
#ifndef Q_OS_WIN
        if (m_hunspell.handle && m_hunspell.add) {
            m_hunspell.add(m_hunspell.handle, line.toUtf8().constData());
        }
#else
        if (m_winSpellChecker) {
            auto *sc = static_cast<ISpellChecker *>(m_winSpellChecker);
            sc->Add(reinterpret_cast<LPCWSTR>(line.utf16()));
        }
#endif
    }
}

bool SpellChecker::saveUserDictionaryWord(const QString &word) const {
    if (m_userDictPath.isEmpty() || word.isEmpty()) return false;
    QDir().mkpath(QFileInfo(m_userDictPath).absolutePath());
    QSaveFile f(m_userDictPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QStringList sorted = m_userWords.values();
    sorted.sort(Qt::CaseInsensitive);
    for (const QString &w : sorted) {
        f.write(w.toUtf8());
        f.write("\n");
    }
    return f.commit();
}

void SpellChecker::setSpellCheckEnabled(bool enabled) {
    if (m_spellCheckEnabled == enabled) return;
    m_spellCheckEnabled = enabled;
    saveSettings();
    emit spellCheckEnabledChanged();
}

void SpellChecker::setAutoSuggestEnabled(bool enabled) {
    if (m_autoSuggestEnabled == enabled) return;
    m_autoSuggestEnabled = enabled;
    saveSettings();
    emit autoSuggestEnabledChanged();
    if (!m_autoSuggestEnabled) {
        clearAssist();
    }
}

void SpellChecker::attachToDocument(QQuickTextDocument *doc) {
    if (!doc) return;
    attachToTextDocument(doc->textDocument());
}

void SpellChecker::attachToTextDocument(QTextDocument *doc) {
    if (!doc) return;
    if (doc->findChild<SpellHighlighter *>(QString(), Qt::FindDirectChildrenOnly)) {
        return;
    }
    new SpellHighlighter(this, doc);
}

#ifndef Q_OS_WIN
bool SpellChecker::ensureHunspellLibraryLoaded() {
    if (m_hunspell.loaded) return true;
    const QStringList candidates = {
        QStringLiteral("hunspell-1.7"),
        QStringLiteral("hunspell-1.6"),
        QStringLiteral("hunspell-1.5"),
        QStringLiteral("hunspell"),
    };
    for (const QString &name : candidates) {
        m_hunspell.lib.setFileName(name);
        if (!m_hunspell.lib.load()) continue;
        m_hunspell.create = reinterpret_cast<HunspellApi::CreateFn>(m_hunspell.lib.resolve("Hunspell_create"));
        m_hunspell.destroy = reinterpret_cast<HunspellApi::DestroyFn>(m_hunspell.lib.resolve("Hunspell_destroy"));
        m_hunspell.spell = reinterpret_cast<HunspellApi::SpellFn>(m_hunspell.lib.resolve("Hunspell_spell"));
        m_hunspell.suggest = reinterpret_cast<HunspellApi::SuggestFn>(m_hunspell.lib.resolve("Hunspell_suggest"));
        m_hunspell.freeList = reinterpret_cast<HunspellApi::FreeListFn>(m_hunspell.lib.resolve("Hunspell_free_list"));
        m_hunspell.add = reinterpret_cast<HunspellApi::AddFn>(m_hunspell.lib.resolve("Hunspell_add"));
        if (m_hunspell.create && m_hunspell.destroy && m_hunspell.spell &&
            m_hunspell.suggest && m_hunspell.freeList) {
            m_hunspell.loaded = true;
            return true;
        }
        m_hunspell.lib.unload();
    }
    return false;
}
#endif

bool SpellChecker::loadHunspellDictionary(const QString &affPath, const QString &dicPath) {
#ifdef Q_OS_WIN
    Q_UNUSED(affPath);
    Q_UNUSED(dicPath);
    return false;
#else
    if (!QFileInfo::exists(affPath) || !QFileInfo::exists(dicPath)) {
        return false;
    }
    if (!ensureHunspellLibraryLoaded()) {
        return false;
    }
    if (m_hunspell.handle && m_hunspell.destroy) {
        m_hunspell.destroy(m_hunspell.handle);
        m_hunspell.handle = nullptr;
    }
    m_hunspell.handle = m_hunspell.create(QFile::encodeName(affPath).constData(),
                                          QFile::encodeName(dicPath).constData());
    if (!m_hunspell.handle) {
        return false;
    }
    if (m_hunspell.add) {
        for (const QString &w : std::as_const(m_userWords)) {
            m_hunspell.add(m_hunspell.handle, w.toUtf8().constData());
        }
    }
    emit dictionaryChanged();
    return true;
#endif
}

void SpellChecker::initNativeSpellChecker() {
#ifdef Q_OS_WIN
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        m_winComInitialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return;
    }
    ISpellCheckerFactory *factory = nullptr;
    hr = CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return;
    }
    m_winSpellFactory = factory;

    QStringList langCandidates;
    const QString sysLocale = QLocale::system().bcp47Name();
    if (!sysLocale.isEmpty()) langCandidates.append(sysLocale);
    langCandidates.append(QStringLiteral("en-US"));
    langCandidates.append(QStringLiteral("en-GB"));
    langCandidates.append(QStringLiteral("en"));

    ISpellChecker *checker = nullptr;
    for (const QString &lang : langCandidates) {
        BOOL supported = FALSE;
        if (SUCCEEDED(factory->IsSupported(reinterpret_cast<LPCWSTR>(lang.utf16()), &supported)) && supported) {
            if (SUCCEEDED(factory->CreateSpellChecker(reinterpret_cast<LPCWSTR>(lang.utf16()), &checker)) && checker) {
                break;
            }
        }
    }
    if (checker) {
        m_winSpellChecker = checker;
        for (const QString &w : std::as_const(m_userWords)) {
            checker->Add(reinterpret_cast<LPCWSTR>(w.utf16()));
        }
        emit dictionaryChanged();
    }
#else
    if (!ensureHunspellLibraryLoaded()) {
        return;
    }
    const QStringList searchDirs = {
        QStringLiteral("/usr/share/hunspell"),
        QStringLiteral("/usr/share/myspell"),
        QStringLiteral("/usr/share/myspell/dicts"),
        QStringLiteral("/usr/local/share/hunspell"),
    };
    const QString localeName = QLocale::system().name();
    QStringList localePrefs;
    if (!localeName.isEmpty() && localeName != QStringLiteral("C")) {
        localePrefs << localeName << localeName.section(QLatin1Char('_'), 0, 0);
    }
    localePrefs << QStringLiteral("en_US") << QStringLiteral("en_GB") << QStringLiteral("en");

    for (const QString &dirPath : searchDirs) {
        QDir dir(dirPath);
        if (!dir.exists()) continue;
        for (const QString &pref : localePrefs) {
            const QString aff = dir.filePath(pref + QStringLiteral(".aff"));
            const QString dic = dir.filePath(pref + QStringLiteral(".dic"));
            if (QFileInfo::exists(aff) && QFileInfo::exists(dic)) {
                if (loadHunspellDictionary(aff, dic)) return;
            }
        }
        const QStringList dics = dir.entryList({QStringLiteral("*.dic")}, QDir::Files);
        for (const QString &dicName : dics) {
            const QString base = dicName.left(dicName.size() - 4);
            const QString aff = dir.filePath(base + QStringLiteral(".aff"));
            const QString dic = dir.filePath(dicName);
            if (QFileInfo::exists(aff) && loadHunspellDictionary(aff, dic)) {
                return;
            }
        }
    }
#endif
}

void SpellChecker::cleanupNativeSpellChecker() {
#ifdef Q_OS_WIN
    if (m_winSpellChecker) {
        static_cast<ISpellChecker *>(m_winSpellChecker)->Release();
        m_winSpellChecker = nullptr;
    }
    if (m_winSpellFactory) {
        static_cast<ISpellCheckerFactory *>(m_winSpellFactory)->Release();
        m_winSpellFactory = nullptr;
    }
    if (m_winComInitialized) {
        CoUninitialize();
        m_winComInitialized = false;
    }
#else
    if (m_hunspell.handle && m_hunspell.destroy) {
        m_hunspell.destroy(m_hunspell.handle);
        m_hunspell.handle = nullptr;
    }
    if (m_hunspell.lib.isLoaded()) {
        m_hunspell.lib.unload();
    }
    m_hunspell.loaded = false;
#endif
}

bool SpellChecker::isAvailable() const {
#ifdef Q_OS_WIN
    return m_winSpellChecker != nullptr;
#else
    return m_hunspell.handle != nullptr;
#endif
}

bool SpellChecker::isWordMisspelled(const QString &word) const {
    if (!m_spellCheckEnabled || !isAvailable()) return false;
    const QString norm = normalizeWord(word);
    if (norm.size() <= 1) return false;
    if (m_userWords.contains(norm) || m_userWordsLower.contains(norm.toLower())) {
        return false;
    }
#ifdef Q_OS_WIN
    auto *sc = static_cast<ISpellChecker *>(m_winSpellChecker);
    if (!sc) return false;
    IEnumSpellingError *errors = nullptr;
    HRESULT hr = sc->Check(reinterpret_cast<LPCWSTR>(norm.utf16()), &errors);
    if (FAILED(hr) || !errors) return false;
    ISpellingError *err = nullptr;
    const bool misspelled = (errors->Next(&err) == S_OK && err != nullptr);
    if (err) err->Release();
    errors->Release();
    return misspelled;
#else
    if (!m_hunspell.handle || !m_hunspell.spell) return false;
    const QByteArray utf8 = norm.toUtf8();
    return m_hunspell.spell(m_hunspell.handle, utf8.constData()) == 0;
#endif
}

QStringList SpellChecker::spellingSuggestions(const QString &word) const {
    QStringList out;
    if (!isAvailable()) return out;
    const QString norm = normalizeWord(word);
    if (norm.isEmpty()) return out;
#ifdef Q_OS_WIN
    auto *sc = static_cast<ISpellChecker *>(m_winSpellChecker);
    if (!sc) return out;
    IEnumString *suggestions = nullptr;
    if (SUCCEEDED(sc->Suggest(reinterpret_cast<LPCWSTR>(norm.utf16()), &suggestions)) && suggestions) {
        LPOLESTR item = nullptr;
        while (out.size() < 8 && suggestions->Next(1, &item, nullptr) == S_OK && item) {
            const QString s = QString::fromWCharArray(item);
            if (!s.isEmpty() && !out.contains(s)) {
                out.append(s);
            }
            CoTaskMemFree(item);
            item = nullptr;
        }
        suggestions->Release();
    }
#else
    if (!m_hunspell.handle || !m_hunspell.suggest || !m_hunspell.freeList) return out;
    char **slst = nullptr;
    const int count = m_hunspell.suggest(m_hunspell.handle, &slst, norm.toUtf8().constData());
    if (count > 0 && slst) {
        const int cap = qMin(count, 8);
        for (int i = 0; i < cap; ++i) {
            if (slst[i]) {
                const QString s = QString::fromUtf8(slst[i]);
                if (!s.isEmpty() && !out.contains(s)) {
                    out.append(s);
                }
            }
        }
        m_hunspell.freeList(m_hunspell.handle, &slst, count);
    }
#endif
    return out;
}

void SpellChecker::addWordToDictionary(const QString &word) {
    const QString norm = normalizeWord(word);
    if (norm.isEmpty()) return;
    m_userWords.insert(norm);
    m_userWordsLower.insert(norm.toLower());
#ifdef Q_OS_WIN
    if (m_winSpellChecker) {
        static_cast<ISpellChecker *>(m_winSpellChecker)->Add(reinterpret_cast<LPCWSTR>(norm.utf16()));
    }
#else
    if (m_hunspell.handle && m_hunspell.add) {
        m_hunspell.add(m_hunspell.handle, norm.toUtf8().constData());
    }
#endif
    saveUserDictionaryWord(norm);
    emit dictionaryChanged();
}

void SpellChecker::setRelayEndpoint(const QString &serverUrl, const QString &password) {
    m_relayUrl = serverUrl.trimmed();
    while (m_relayUrl.endsWith(QLatin1Char('/'))) {
        m_relayUrl.chop(1);
    }
    m_relayPassword = password;
}

void SpellChecker::clearAssist() {
    m_assistDebounceTimer.stop();
    m_pendingAssistText.clear();
    ++m_assistSeq;
    if (QNetworkReply *reply = m_assistReply.data()) {
        m_assistReply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (!m_inlineSuggestion.isEmpty() || !m_grammarHint.isEmpty() || !m_grammarHints.isEmpty()) {
        m_inlineSuggestion.clear();
        m_grammarHint.clear();
        m_grammarHints.clear();
        emit assistChanged();
    }
}

void SpellChecker::requestAssist(const QString &text) {
    if (!m_autoSuggestEnabled || text.trimmed().isEmpty()) {
        clearAssist();
        return;
    }
    m_pendingAssistText = text;
    m_assistDebounceTimer.start();
}

void SpellChecker::dispatchAssistRequest() {
    if (!m_autoSuggestEnabled || m_pendingAssistText.trimmed().isEmpty()) {
        return;
    }
    if (m_relayUrl.isEmpty() || m_relayUrl.startsWith(QStringLiteral("ssh://"), Qt::CaseInsensitive)) {
        return;
    }

    QUrl u(m_relayUrl + QStringLiteral("/api/v1/text/check"));
    if (!u.isValid()) return;
    const QString scheme = u.scheme().toLower();
    const QString host = u.host().toLower();
    const bool isLoopback = (host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost") || host == QStringLiteral("::1"));
    if (scheme != QStringLiteral("https") && !(scheme == QStringLiteral("http") && isLoopback)) {
        return;
    }

    if (!m_relayPassword.isEmpty()) {
        QUrlQuery q(u);
        q.addQueryItem(QStringLiteral("password"), m_relayPassword);
        u.setQuery(q);
    }

    if (QNetworkReply *oldReply = m_assistReply.data()) {
        m_assistReply.clear();
        oldReply->disconnect(this);
        oldReply->abort();
        oldReply->deleteLater();
    }

    const quint64 seq = ++m_assistSeq;
    QNetworkRequest req(u);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(5000);
    if (!m_relayPassword.isEmpty()) {
        req.setRawHeader("X-BlueBubbles-Password", m_relayPassword.toUtf8());
    }

    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("text"), m_pendingAssistText}
    }).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = m_nam.post(req, body);
    m_assistReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, seq]() {
        if (m_assistReply == reply) {
            m_assistReply.clear();
        }
        reply->deleteLater();
        if (seq != m_assistSeq || !m_autoSuggestEnabled || reply->error() != QNetworkReply::NoError) {
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject data = root.contains(QStringLiteral("data"))
                                     ? root.value(QStringLiteral("data")).toObject()
                                     : root;

        QString nextSuggestion;
        const QJsonArray completions = data.value(QStringLiteral("completions")).toArray();
        if (!completions.isEmpty()) {
            const QJsonValue first = completions.first();
            if (first.isString()) {
                nextSuggestion = first.toString().trimmed();
            } else if (first.isObject()) {
                nextSuggestion = first.toObject().value(QStringLiteral("text")).toString().trimmed();
            }
        }

        QVariantList nextHints;
        QString nextGrammarHint;
        const QJsonArray grammarArr = data.value(QStringLiteral("grammar")).toArray();
        for (const QJsonValue &v : grammarArr) {
            if (v.isString()) {
                const QString s = v.toString().trimmed();
                if (!s.isEmpty()) {
                    nextHints.append(QVariantMap{{QStringLiteral("message"), s}});
                    if (nextGrammarHint.isEmpty()) nextGrammarHint = s;
                }
            } else if (v.isObject()) {
                const QJsonObject go = v.toObject();
                nextHints.append(go.toVariantMap());
                if (nextGrammarHint.isEmpty()) {
                    nextGrammarHint = go.value(QStringLiteral("message")).toString().trimmed();
                    if (nextGrammarHint.isEmpty()) {
                        nextGrammarHint = go.value(QStringLiteral("description")).toString().trimmed();
                    }
                    const QString repl = go.value(QStringLiteral("replacement")).toString().trimmed();
                    if (!repl.isEmpty()) {
                        nextGrammarHint = nextGrammarHint.isEmpty()
                                              ? repl
                                              : QStringLiteral("%1 (%2)").arg(nextGrammarHint, repl);
                    }
                }
            }
        }

        if (nextSuggestion != m_inlineSuggestion ||
            nextGrammarHint != m_grammarHint ||
            nextHints != m_grammarHints) {
            m_inlineSuggestion = nextSuggestion;
            m_grammarHint = nextGrammarHint;
            m_grammarHints = nextHints;
            emit assistChanged();
        }
    });
}
