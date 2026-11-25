#pragma once

#include <QLibrary>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTextDocument>
#include <QTimer>
#include <QVariantList>

class SpellChecker;

class SpellHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit SpellHighlighter(SpellChecker *checker, QTextDocument *parent = nullptr);

protected:
    void highlightBlock(const QString &text) override;

private:
    QPointer<SpellChecker> m_checker;
};

class SpellChecker : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool spellCheckEnabled READ spellCheckEnabled WRITE setSpellCheckEnabled NOTIFY spellCheckEnabledChanged)
    Q_PROPERTY(bool autoSuggestEnabled READ autoSuggestEnabled WRITE setAutoSuggestEnabled NOTIFY autoSuggestEnabledChanged)
    Q_PROPERTY(QString inlineSuggestion READ inlineSuggestion NOTIFY assistChanged)
    Q_PROPERTY(QString grammarHint READ grammarHint NOTIFY assistChanged)
    Q_PROPERTY(QVariantList grammarHints READ grammarHints NOTIFY assistChanged)
    Q_PROPERTY(bool available READ isAvailable NOTIFY dictionaryChanged)

public:
    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    bool spellCheckEnabled() const { return m_spellCheckEnabled; }
    void setSpellCheckEnabled(bool enabled);

    bool autoSuggestEnabled() const { return m_autoSuggestEnabled; }
    void setAutoSuggestEnabled(bool enabled);

    QString inlineSuggestion() const { return m_inlineSuggestion; }
    QString grammarHint() const { return m_grammarHint; }
    QVariantList grammarHints() const { return m_grammarHints; }
    bool isAvailable() const;

    Q_INVOKABLE void attachToDocument(QQuickTextDocument *doc);
    void attachToTextDocument(QTextDocument *doc);

    Q_INVOKABLE bool isWordMisspelled(const QString &word) const;
    Q_INVOKABLE QStringList spellingSuggestions(const QString &word) const;
    Q_INVOKABLE void addWordToDictionary(const QString &word);
    Q_INVOKABLE void requestAssist(const QString &text);
    Q_INVOKABLE void clearAssist();

    void setRelayEndpoint(const QString &serverUrl, const QString &password);
    void setAllowLocalhostRelayForTest(bool allow) { m_allowLocalhostRelayForTest = allow; }
    void setAssistDebounceMsForTest(int ms);
    void setUserDictionaryPathForTest(const QString &path);
    void setSettingsFilePathForTest(const QString &path);
    bool loadHunspellDictionary(const QString &affPath, const QString &dicPath);
    void reloadForTest();

signals:
    void spellCheckEnabledChanged();
    void autoSuggestEnabledChanged();
    void assistChanged();
    void dictionaryChanged();

private:
    void initNativeSpellChecker();
    void cleanupNativeSpellChecker();
    void loadSettings();
    void saveSettings() const;
    void loadUserDictionary();
    bool saveUserDictionaryWord(const QString &word) const;
    void dispatchAssistRequest();
    static QString defaultUserDictionaryPath();
    static QString defaultSettingsFilePath();
    static QString normalizeWord(const QString &word);

    bool m_spellCheckEnabled = true;
    bool m_autoSuggestEnabled = true;
    QString m_inlineSuggestion;
    QString m_grammarHint;
    QVariantList m_grammarHints;

    QString m_userDictPath;
    QString m_settingsPath;
    QSet<QString> m_userWords;
    QSet<QString> m_userWordsLower;

    QString m_relayUrl;
    QString m_relayPassword;
    bool m_allowLocalhostRelayForTest = false;
    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_assistReply;
    QTimer m_assistDebounceTimer;
    QString m_pendingAssistText;
    quint64 m_assistSeq = 0;

#ifdef Q_OS_WIN
    void *m_winSpellFactory = nullptr;
    void *m_winSpellChecker = nullptr;
    bool m_winComInitialized = false;
#else
    struct HunspellApi {
        QLibrary lib;
        using CreateFn = void *(*)(const char *, const char *);
        using DestroyFn = void (*)(void *);
        using SpellFn = int (*)(void *, const char *);
        using SuggestFn = int (*)(void *, char ***, const char *);
        using FreeListFn = void (*)(void *, char ***, int);
        using AddFn = int (*)(void *, const char *);
        CreateFn create = nullptr;
        DestroyFn destroy = nullptr;
        SpellFn spell = nullptr;
        SuggestFn suggest = nullptr;
        FreeListFn freeList = nullptr;
        AddFn add = nullptr;
        void *handle = nullptr;
        bool loaded = false;
    };
    HunspellApi m_hunspell;
    bool ensureHunspellLibraryLoaded();
#endif
};
