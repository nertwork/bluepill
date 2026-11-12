#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QProcess>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

struct ChatEntry {
    QString guid;
    QString chatIdentifier;
    QString displayName;
    QString title;
    QString subtitle;
    QString service;
    QString avatarUrl;
    QString initials;
    QString avatarColor;
    QString lastMessagePreview;
    QString relativeTime;
    qint64 lastMessageDate = 0;
    int unreadCount = 0;
    bool isGroup = false;
    QStringList participantHandles;
};

struct EmojiSpec {
    QString shortcode;
    QString emoji;
    QString category;
    QString keywords;
};


struct AttachmentInfo {
    QString guid;
    QString transferName;
    QString mimeType;
    QString localUrl;
    bool isImage = false;
    bool isAudio = false;
    int durationSeconds = 0;
    QVariantList waveformBars;
};

struct MessageModel {
    enum Roles {
        AttachmentsRole = Qt::UserRole + 100
    };
};

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString serverStatus READ serverStatus NOTIFY statusChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY configChanged)
    Q_PROPERTY(QString secondaryServerUrl READ secondaryServerUrl NOTIFY configChanged)
    Q_PROPERTY(QString serverPassword READ serverPassword NOTIFY configChanged)
    Q_PROPERTY(bool hasPasswordConfigured READ hasPasswordConfigured NOTIFY configChanged)
    Q_PROPERTY(bool needsSetup READ needsSetup NOTIFY configChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY filterChanged)
    Q_PROPERTY(QString filterMode READ filterMode WRITE setFilterMode NOTIFY filterChanged)
    Q_PROPERTY(QString selectedChatGuid READ selectedChatGuid NOTIFY selectedChatChanged)
    Q_PROPERTY(QString selectedChatTitle READ selectedChatTitle NOTIFY selectedChatChanged)
    Q_PROPERTY(QString selectedChatSubtitle READ selectedChatSubtitle NOTIFY selectedChatChanged)
    Q_PROPERTY(QString selectedChatService READ selectedChatService NOTIFY selectedChatChanged)
    Q_PROPERTY(QString selectedChatAvatar READ selectedChatAvatar NOTIFY selectedChatChanged)
    Q_PROPERTY(QString selectedChatInitials READ selectedChatInitials NOTIFY selectedChatChanged)
    Q_PROPERTY(QString selectedChatColor READ selectedChatColor NOTIFY selectedChatChanged)
    Q_PROPERTY(bool selectedChatIsGroup READ selectedChatIsGroup NOTIFY selectedChatChanged)
    Q_PROPERTY(bool selectedChatMuted READ selectedChatMuted NOTIFY selectedChatChanged)
    Q_PROPERTY(QString selectedChatAddress READ selectedChatAddress NOTIFY selectedChatChanged)
    Q_PROPERTY(bool selectedChatHasContact READ selectedChatHasContact NOTIFY selectedChatChanged)
    Q_PROPERTY(int selectedChatUnreadCount READ selectedChatUnreadCount NOTIFY selectedChatChanged)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsChanged)
    Q_PROPERTY(bool omarchyDnd READ omarchyDnd NOTIFY omarchyDndChanged)
    Q_PROPERTY(QVariantList chats READ chats NOTIFY chatsChanged)
    Q_PROPERTY(QVariantList pinnedChats READ pinnedChats NOTIFY chatsChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(int totalUnread READ totalUnread NOTIFY chatsChanged)
    Q_PROPERTY(bool windowActive READ windowActive WRITE setWindowActive NOTIFY windowActiveChanged)
    Q_PROPERTY(bool hasOlderMessages READ hasOlderMessages NOTIFY olderMessagesChanged)
    Q_PROPERTY(bool loadingOlderMessages READ loadingOlderMessages NOTIFY olderMessagesChanged)
    Q_PROPERTY(int loadedMessageCount READ loadedMessageCount NOTIFY messagesChanged)
    Q_PROPERTY(int historySyncDays READ historySyncDays WRITE setHistorySyncDays NOTIFY historySyncDaysChanged)
    Q_PROPERTY(bool syncingHistory READ syncingHistory NOTIFY syncingHistoryChanged)
    Q_PROPERTY(QString playingAudioGuid READ playingAudioGuid NOTIFY audioPlaybackChanged)
    Q_PROPERTY(bool audioPlaying READ audioPlaying NOTIFY audioPlaybackChanged)
    Q_PROPERTY(double audioProgress READ audioProgress NOTIFY audioPlaybackChanged)
    Q_PROPERTY(QString audioDurationText READ audioDurationText NOTIFY audioPlaybackChanged)
    Q_PROPERTY(bool recordingVoice READ recordingVoice NOTIFY voiceRecordingChanged)
    Q_PROPERTY(int voiceRecordingSeconds READ voiceRecordingSeconds NOTIFY voiceRecordingChanged)
    Q_PROPERTY(QVariantList voiceRecordingWaveform READ voiceRecordingWaveform NOTIFY voiceRecordingChanged)
    Q_PROPERTY(QVariantList scheduledMessages READ scheduledMessages NOTIFY scheduledMessagesChanged)
    Q_PROPERTY(QStringList selectedChatParticipants READ selectedChatParticipants NOTIFY selectedChatChanged)
    Q_PROPERTY(QVariantList icloudNotes READ icloudNotes NOTIFY icloudNotesChanged)
    Q_PROPERTY(QVariantList icloudEvents READ icloudEvents NOTIFY icloudEventsChanged)
    Q_PROPERTY(QVariantList icloudCalendars READ icloudCalendars NOTIFY icloudEventsChanged)
    Q_PROPERTY(QVariantList icloudPhotos READ icloudPhotos NOTIFY icloudPhotosChanged)
    Q_PROPERTY(bool icloudPhotosSyncing READ icloudPhotosSyncing NOTIFY icloudPhotosSyncingChanged)
    Q_PROPERTY(bool sendingMessage READ sendingMessage NOTIFY sendingMessageChanged)


public:
    explicit Backend(bool autoStart = true, QObject *parent = nullptr);
    ~Backend() override;

    bool connected() const { return m_connected; }
    bool loading() const { return m_loading; }
    QString serverStatus() const { return m_serverStatus; }
    QString serverUrl() const { return m_serverUrl; }
    QString secondaryServerUrl() const { return m_secondaryServerUrl; }
    QString serverPassword() const { return m_password; }
    bool hasPasswordConfigured() const { return !m_password.isEmpty(); }
    bool needsSetup() const { return !m_demoMode && (m_serverUrl.isEmpty() || (!m_serverUrl.startsWith(QStringLiteral("ssh://"), Qt::CaseInsensitive) && m_password.isEmpty())); }
    QString searchQuery() const { return m_searchQuery; }
    QString filterMode() const { return m_filterMode; }
    QString selectedChatGuid() const { return m_selectedChatGuid; }
    QString selectedChatTitle() const;
    QString selectedChatSubtitle() const;
    QString selectedChatService() const;
    QString selectedChatAvatar() const;
    QString selectedChatInitials() const;
    QString selectedChatColor() const;
    bool selectedChatIsGroup() const;
    bool selectedChatMuted() const;
    QString selectedChatAddress() const;
    bool selectedChatHasContact() const;
    int selectedChatUnreadCount() const;
    bool notificationsEnabled() const { return m_notificationsEnabled; }
    bool omarchyDnd() const;
    bool areOmarchyNotificationsEnabled() const;
    QVariantList chats() const;
    QVariantList pinnedChats() const;
    QVariantList messages() const { return m_messages; }
    int totalUnread() const;
    bool windowActive() const { return m_windowActive; }
    bool hasOlderMessages() const;
    bool loadingOlderMessages() const { return m_loadingOlderMessages; }
    int loadedMessageCount() const { return m_messages.size(); }
    int historySyncDays() const { return m_historySyncDays; }
    bool syncingHistory() const { return m_syncingHistory; }
    QString playingAudioGuid() const { return m_playingAudioGuid; }
    bool audioPlaying() const { return m_audioPlaying; }
    double audioProgress() const { return m_audioProgress; }
    QString audioDurationText() const { return m_audioDurationText; }
    bool recordingVoice() const { return m_recordingVoice; }
    int voiceRecordingSeconds() const { return m_voiceRecordingSeconds; }
    QVariantList voiceRecordingWaveform() const { return m_voiceRecordingWaveform; }
    QVariantList scheduledMessages() const { return m_scheduledMessages; }
    QStringList selectedChatParticipants() const;
    QVariantList icloudNotes() const { return m_icloudNotes; }
    QVariantList icloudEvents() const { return m_icloudEvents; }
    QVariantList icloudCalendars() const { return m_icloudCalendars; }
    QVariantList icloudPhotos() const { return m_icloudPhotos; }
    bool icloudPhotosSyncing() const { return m_icloudPhotosSyncInFlight; }
    bool sendingMessage() const { return m_activeSendCount > 0; }
    void setIcloudPhotosForTest(const QVariantList &list) { m_icloudPhotos = list; emit icloudPhotosChanged(); }


    void setWindowActive(bool active);
    void setNotificationsEnabled(bool enabled);
    void setStateFilePath(const QString &path);
    void setConfigFilePath(const QString &path);
    QString configFilePath() const;
    void checkServerAndLoad();
    Q_INVOKABLE void saveServerConfig(const QString &serverUrl, const QString &secondaryUrl, const QString &password);
    Q_INVOKABLE QString webUiUrl() const;
    Q_INVOKABLE void openWebUi();
    Q_INVOKABLE void setHistorySyncDays(int days);
    Q_INVOKABLE void cycleHistorySyncDays();
    Q_INVOKABLE void clearSelectedChat();
    Q_INVOKABLE void goHome();

    Q_INVOKABLE void toggleNotifications();
    Q_INVOKABLE void toggleChatMute(const QString &chatGuid);
    Q_INVOKABLE bool isChatMuted(const QString &chatGuid) const;
    Q_INVOKABLE void togglePinChat(const QString &chatGuid);
    Q_INVOKABLE bool isChatPinned(const QString &chatGuid) const;

    Q_INVOKABLE void setSearchQuery(const QString &query);
    Q_INVOKABLE void setFilterMode(const QString &mode);
    Q_INVOKABLE void refreshAll();
    Q_INVOKABLE void selectChat(const QString &chatGuid);
    Q_INVOKABLE void selectNextChat();
    Q_INVOKABLE void selectPrevChat();
    Q_INVOKABLE void markChatRead(const QString &chatGuid);
    Q_INVOKABLE void markChatUnread(const QString &chatGuid);
    Q_INVOKABLE void markAllChatsRead();
    Q_INVOKABLE void toggleChatReadUnread(const QString &chatGuid);
    Q_INVOKABLE void toggleChatRead(const QString &chatGuid) { toggleChatReadUnread(chatGuid); }
    Q_INVOKABLE void sendMessage(const QString &chatGuid, const QString &text, const QString &replyToGuid = QString());
    Q_INVOKABLE void sendReaction(const QString &chatGuid, const QString &messageGuid, const QString &reaction);
    Q_INVOKABLE void deleteMessage(const QString &messageGuid, const QString &chatGuid = QString());
    Q_INVOKABLE void unsendMessage(const QString &messageGuid, const QString &chatGuid = QString());
    Q_INVOKABLE void deleteChat(const QString &chatGuid = QString());
    Q_INVOKABLE void startNewChat(const QString &address, const QString &text = QString());
    Q_INVOKABLE void openAttachment(const QString &guid, const QString &transferName);
    Q_INVOKABLE void registerWindow(QObject *window);
    bool eventFilter(QObject *watched, QEvent *event) override;
    Q_INVOKABLE void loadDemoData();
    Q_INVOKABLE void seedDemoData() { loadDemoData(); }
    Q_INVOKABLE void saveContact(const QString &firstName, const QString &lastName, const QString &phone, const QString &email);
    Q_INVOKABLE void syncContactsNow();
    Q_INVOKABLE void loadOlderMessages();

    // Emoji shortcode & picker helpers
    Q_INVOKABLE static QString replaceEmojiShortcodes(const QString &text);
    Q_INVOKABLE QVariantList searchEmojis(const QString &query = QString(),
                                          const QString &category = QString(),
                                          int limit = 60) const;

    // Clipboard & Verification/OTP code extraction
    Q_INVOKABLE void copyToClipboard(const QString &text, const QString &label = QString());
    Q_INVOKABLE void copyTextToClipboard(const QString &text) { copyToClipboard(text); }
    Q_INVOKABLE static QString extractVerificationCode(const QString &text);
    Q_INVOKABLE static QString extractOtpCode(const QString &text) { return extractVerificationCode(text); }
    Q_INVOKABLE QString pasteClipboardImageToTempFile();
    Q_INVOKABLE void sendAttachment(const QString &chatGuid, const QString &filePath, const QString &caption = QString());
    // Audio, Voice, Effects & Scheduling
    Q_INVOKABLE static bool isAudioAttachment(const QString &mimeType, const QString &fileName);
    Q_INVOKABLE static QVariantList computeWaveformBars(const QString &seed, int count = 28);
    Q_INVOKABLE void toggleAudioPlayback(const QString &attachmentGuid, const QString &sourceUrl = QString());
    Q_INVOKABLE void seekAudioPlayback(const QString &attachmentGuid, double ratio);
    Q_INVOKABLE void startVoiceRecording();
    Q_INVOKABLE void stopAndSendVoiceRecording();
    Q_INVOKABLE void cancelVoiceRecording();
    Q_INVOKABLE void sendMessageWithEffect(const QString &text, const QString &effectId);
    Q_INVOKABLE void scheduleMessage(const QString &text, qint64 sendAtMs);
    Q_INVOKABLE void cancelScheduledMessage(const QString &scheduleId);
    Q_INVOKABLE void addParticipantToActiveChat(const QString &address);
    Q_INVOKABLE void removeParticipantFromActiveChat(const QString &address);
    Q_INVOKABLE void syncChatStateToServer();


    // Contact & recipient autocomplete
    Q_INVOKABLE QVariantList searchContacts(const QString &query = QString(), int limit = 15) const;
    Q_INVOKABLE static QString canonicalizeAddress(const QString &raw);
    Q_INVOKABLE static QString formatHandleForDisplay(const QString &raw);
    Q_INVOKABLE void fetchIcloudNotes();
    Q_INVOKABLE QString saveIcloudNote(const QString &id, const QString &title, const QString &body, const QString &folder = QStringLiteral("Notes"));
    Q_INVOKABLE void deleteIcloudNote(const QString &id);
    Q_INVOKABLE void fetchIcloudEvents();
    Q_INVOKABLE QString saveIcloudEvent(const QString &title, qint64 startMs, qint64 endMs, const QString &calendarName = QStringLiteral("Home"), const QString &notes = QString());
    Q_INVOKABLE void deleteIcloudEvent(const QString &id);
    Q_INVOKABLE void fetchIcloudPhotos();
    Q_INVOKABLE void downloadCloudAttachment(const QString &attachmentGuid);
    Q_INVOKABLE void syncIcloudPhotos();
    Q_INVOKABLE void shareIcloudPhotoToChat(const QString &chatGuid, const QString &attachmentGuid, const QString &fileName = QString());
    Q_INVOKABLE QString attachmentDownloadUrl(const QString &guid) const;
    bool isSecureOrLocalIcloudEndpoint() const;
    void replayPendingIcloudMutations();
    void reconcileRemoteIcloudNotesForTest(const QJsonArray &data) { applyRemoteIcloudNotes(data); }
    void reconcileRemoteIcloudEventsForTest(const QJsonArray &data) { applyRemoteIcloudEvents(data); }
    quint64 icloudNotesGenerationForTest() const { return m_icloudNotesGeneration; }
    quint64 icloudEventsGenerationForTest() const { return m_icloudEventsGeneration; }
    void recordIcloudMutationFailureForTest(const QString &mutationKey) { recordIcloudMutationFailure(mutationKey); }
    void clearIcloudMutationRetryStateForTest(const QString &mutationKey) { clearIcloudMutationRetryState(mutationKey); }
    bool isIcloudMutationInBackoffForTest(const QString &mutationKey) const;
    bool isIcloudMutationPausedForTest(const QString &mutationKey) const { return m_icloudMutationPausedKeys.contains(mutationKey); }
    bool hasPendingIcloudNoteSaveForTest(const QString &noteId) const { return m_pendingIcloudNoteSaves.contains(noteId); }
    bool hasPendingIcloudEventSaveForTest(const QString &evtId) const { return m_pendingIcloudEventSaves.contains(evtId); }
    int icloudMutationRetryCountForTest(const QString &mutationKey) const { return m_icloudMutationRetryCounts.value(mutationKey, 0); }
    void scheduleIcloudMutationRetryForTest(bool immediate) { scheduleIcloudMutationRetry(immediate); }

    // Network & Failover state
    QString currentServerUrl() const { return m_serverUrl; }
    QStringList candidateUrls() const { return m_candidateUrls; }
    int consecutiveFailures() const { return m_consecutiveFailures; }
    void attemptReconnect();

    // Pure testable helpers
    static QString normalizeHandle(const QString &raw);
    static bool isMessageUnread(const QJsonObject &m, qint64 watermark = 0, qint64 unreadOverrideTs = 0);
    static qint64 extractChatReadWatermark(const QJsonObject &chatObj, const QHash<QString, QJsonObject> &chatMessages = {});
    static QString extractMessageText(const QJsonObject &msgObj);
    static QString mapReactionEmoji(const QString &typeStr, const QString &text = QString(), bool *isRemoval = nullptr);
    static QString normalizeAssociatedGuid(const QString &assocGuid);
    static QString formatRelativeTime(qint64 timestampMs, qint64 nowMs = 0);
    static QString computeInitials(const QString &title);
    static QString computeAvatarColor(const QString &seed);
    static int computeTextPartIndex(const QJsonObject &msgObj);
    static QString formatExpressiveEffectLabel(const QString &styleId);
    static QString extractFirstUrl(const QString &text);
    static bool isYouTubeUrl(const QString &url);
    static QString formatMessageTextWithLinks(const QString &raw);
    static bool isImageMimeOrExt(const QString &mime, const QString &fileName);
    static QJsonArray deduplicateMessageAttachments(const QJsonArray &atts);
    static bool isMessageDeletedOrRetracted(const QJsonObject &m);
    static QVariantList buildMessageListFromApi(
        const QJsonArray &rawMessagesDesc,
        const QHash<QString, QString> &contactNames,
        const QHash<QString, QString> &attachmentCache,
        QStringList *missingAttachmentsOut = nullptr,
        const QHash<QString, QVariantMap> &previewCache = {},
        QStringList *missingPreviewsOut = nullptr,
        const QSet<QString> &deletedMessageGuids = {},
        const QHash<QString, QString> &recentOutboundEffects = {});

    // Contact resolution & state
    void loadCredentials();
    void loadConfig() { loadCredentials(); }
    void loadAppState();
    void saveAppState() const;
    QString messagesCacheFilePath() const;
    void loadMessagesCache();
    void saveMessagesCache() const;
    void flushMessagesCacheNow(bool async = false) const;
    void loadContactsFromFile(const QString &path);
    void parseContactsJson(const QByteArray &jsonBytes);
    QString resolveContactName(const QString &handle) const;
    QString resolveContactAvatar(const QString &handle) const;

    // Test / diagnostic injection helper
    void ingestMessagesForTest(const QJsonArray &messagesArray, bool isInitialLoad = true);
    void ingestChatsMetadataForTest(const QJsonArray &chatsArray);
    void reconcileUnreadMessagesForTest(const QJsonArray &unreadMessages);
    void reconcileChatMessagesForTest(const QString &chatGuid, const QJsonArray &serverMessages);
    void handleSocketIoPayloadForTest(const QByteArray &rawPayload);
    void pollDeltaForTest() { pollDelta(); }
    void startSocketIoForTest() { startSocketIoClient(); }
    QString socketSid() const { return m_socketSid; }

    // Headless CLI entrypoints
    int runCliStatus(bool asJson);
    int runCliRecent(int limit, bool asJson);
    int runCliSend(const QString &target, const QString &message, bool asJson);

signals:
    void icloudNotesChanged();
    void icloudEventsChanged();
    void icloudPhotosChanged();
    void icloudPhotosSyncingChanged();
    void sendingMessageChanged();
    void statusChanged();
    void configChanged();
    void loadingChanged();
    void filterChanged();
    void selectedChatChanged();
    void notificationsChanged();
    void omarchyDndChanged();
    void chatsChanged();
    void contactsChanged();
    void aboutToChangeMessages();
    void messagesChanged();
    void olderMessagesChanged();
    void windowActiveChanged();
    void historySyncDaysChanged();
    void syncingHistoryChanged();
    void audioPlaybackChanged();
    void voiceRecordingChanged();
    void scheduledMessagesChanged();

    void toastMessage(const QString &text);
    void attachmentReady(const QString &attachmentGuid, const QString &localUrl);
    void linkPreviewUpdated(const QString &messageGuid, const QVariantMap &previewData);

private:
    QNetworkRequest makeRequest(const QString &pathAndQuery) const;
    QNetworkRequest makeSocketIoRequest(const QList<QPair<QString, QString>> &extraQueryItems = {}, int timeoutMs = 85000) const;
    QUrl buildApiUrl(const QString &apiPath, const QList<QPair<QString, QString>> &queryItems = {}) const;
    void startSocketIoClient();
    void stopSocketIoClient();
    void pollSocketIo();
    void sendSocketIoPacket(const QByteArray &payload, bool startPollingAfter = false);
    void handleSocketIoPayload(const QByteArray &rawPayload);
    void fetchServerInfo();
    void fetchContactsRemote();
    void fetchChatsMetadata();
    void fetchMissingGroupParticipants();
    void processChatsMetadata(const QJsonArray &arr);
    QStringList inferGroupParticipantsFromMessages(const QString &chatGuid) const;
    QString formatGroupChatTitle(const QStringList &participantHandles) const;
    void fetchRecentMessages(int offset = 0, int syncGen = -1);
    void fetchUnreadStateFromServer();
    void reconcileServerUnreadMessages(const QJsonArray &unreadMessages);
    void pollDelta();
    void loadChatMessages(const QString &chatGuid);
    void reconcileChatMessages(const QString &chatGuid, const QJsonArray &serverMessages, bool isFullWindow = true);
    void refreshChatPreviewFromCache(const QString &chatGuid);
    QString deletedMessagesFilePath() const;
    void loadDeletedMessagesFile();
    void saveDeletedMessagesFile() const;
    void cacheMessageForChat(const QString &chatGuid, const QJsonObject &msg);
    void indexCachedAttachmentsOnDisk();
    void prefetchMessageAttachments(const QJsonObject &msgObj, bool highPriority = false);
    void prefetchRecentChatAttachments();
    void rebuildSelectedChatMessages();
    void downloadAttachment(const QString &attachmentGuid, const QString &transferName, bool highPriority = false);
    void pumpAttachmentDownloadQueue();
    void fetchLinkPreview(const QString &msgGuid, const QString &url);
    void processIncomingMessages(const QJsonArray &messagesArray, bool isInitialLoad);
    void rebuildChatPresentation(ChatEntry &entry) const;
    void sortChats();
    void notifyIncomingMessage(const QString &chatGuid, const QString &sender, const QString &preview);
    void playNotificationSound();
    void handleNetworkFailure(const QString &context);
    void handleNetworkSuccess();
    void applyRemoteIcloudNotes(const QJsonArray &data);
    void applyRemoteIcloudEvents(const QJsonArray &data);
    void scheduleIcloudMutationRetry(bool immediate);
    void recordIcloudMutationFailure(const QString &mutationKey);
    void clearIcloudMutationRetryState(const QString &mutationKey);
    void syncIcloudPhotosPage(int offset, int accumulatedSynced, const QString &jobId = QString());

    QNetworkAccessManager m_nam;
    QNetworkAccessManager m_sendNam;
    QNetworkAccessManager m_mediaNam;
    QNetworkAccessManager m_socketNam;
    QPointer<QNetworkReply> m_activeMessagesReply;
    QPointer<QNetworkReply> m_activeEnrichReply;
    QPointer<QNetworkReply> m_activeSocketPollReply;
    quint64 m_messageRequestSeq = 0;
    QTimer m_pollTimer;
    QTimer m_relativeTimeTimer;
    QTimer m_reconnectTimer;
    QTimer m_socketReconnectTimer;
    QTimer m_contactsRefreshTimer;
    QString m_socketSid;
    bool m_socketConnecting = false;
    mutable QTimer m_saveCacheTimer;
    QFileSystemWatcher m_omarchyWatcher;

    QString m_serverUrl;
    QString m_secondaryServerUrl;
    QStringList m_candidateUrls;
    int m_candidateUrlIndex = 0;
    int m_consecutiveFailures = 0;
    QString m_password;
    QString m_stateFilePath;
    QString m_configFilePath;
    bool m_autoStart = true;
    bool m_connected = false;
    bool m_loading = false;
    bool m_demoMode = false;
    bool m_windowActive = true;
    QPointer<QObject> m_rootWindow;
    bool m_notificationsEnabled = true;
    QString m_serverStatus = QStringLiteral("Connecting...");
    QString m_lastKnownServerStatus = QStringLiteral("Connected · Private API");
    int m_historySyncDays = 60;
    bool m_syncingHistory = false;
    int m_historySyncGeneration = 0;
    bool m_stayOnHomePage = false;

    QString m_searchQuery;
    QString m_filterMode = QStringLiteral("all");
    QString m_selectedChatGuid;

    QHash<QString, QString> m_contactNames;
    QHash<QString, QString> m_contactRawAddresses;
    QHash<QString, QString> m_contactAvatars;
    QHash<QString, QStringList> m_chatParticipants;
    QSet<QString> m_fetchedChatParticipantGuids;
    QHash<QString, QString> m_chatDisplayNames;
    QHash<QString, int> m_chatStyles;
    QHash<QString, QString> m_attachmentPaths;
    QHash<QString, QString> m_recentOutboundEffects;
    QHash<QString, QHash<QString, QJsonObject>> m_chatMessagesCache;
    QSet<QString> m_mutedChats;
    QStringList m_pinnedChats;
    QHash<QString, qint64> m_readWatermarks;
    QHash<QString, qint64> m_unreadOverrides;
    QHash<QString, qint64> m_localReadMutationTimeMs;
    int m_pollTickCount = 0;
    QSet<QString> m_downloadingAttachments;
    QSet<QString> m_attemptedAttachments;
    QSet<QString> m_missingAttachments;
    QHash<QString, qint64> m_missingAttachmentCooldownUntilMs;
    QSet<QString> m_liveIncomingAttachments;
    QHash<QString, int> m_attachmentRetryCount;
    QList<QPair<QString, QString>> m_attachmentDownloadQueue;
    QSet<QString> m_seenMessageGuids;
    QSet<QString> m_deletedMessageGuids;
    QHash<QString, qint64> m_deletedChatTimestamps;
    QHash<QString, QVariantMap> m_linkPreviewCache;
    QSet<QString> m_attemptedLinkPreviews;

    QHash<QString, int> m_chatMessageOffset;
    QHash<QString, bool> m_chatHasMoreMessages;
    bool m_loadingOlderMessages = false;

    QList<ChatEntry> m_chats;
    QVariantList m_messages;
    QHash<QString, QVariantList> m_demoChatMessages;
    qint64 m_lastMessageTimestamp = 0;
    qint64 m_highestMessageRowId = 0;
    qint64 m_startupTimestamp = 0;
    qint64 m_lastContactsFetchMs = 0;
    QString m_playingAudioGuid;
    bool m_audioPlaying = false;
    double m_audioProgress = 0.0;
    QString m_audioDurationText = QStringLiteral("0:00");
    QTimer m_audioPlaybackTimer;
    int m_audioPlaybackElapsedMs = 0;
    int m_audioPlaybackDurationMs = 15000;
    QPointer<QProcess> m_audioProcess;

    bool m_recordingVoice = false;
    int m_voiceRecordingSeconds = 0;
    QVariantList m_voiceRecordingWaveform;
    QTimer m_voiceRecordingTimer;
    QString m_currentVoiceRecordingPath;

    QVariantList m_scheduledMessages;
    QVariantList m_icloudNotes;
    QVariantList m_icloudEvents;
    QVariantList m_icloudCalendars;
    QVariantList m_icloudPhotos;
    quint64 m_icloudNotesGeneration = 0;
    quint64 m_icloudEventsGeneration = 0;
    quint64 m_icloudNotesSnapshotEpoch = 0;
    quint64 m_icloudEventsSnapshotEpoch = 0;
    QHash<QString, QVariantMap> m_pendingIcloudNoteSaves;
    QSet<QString> m_pendingIcloudNoteDeletes;
    QHash<QString, QVariantMap> m_pendingIcloudEventSaves;
    QSet<QString> m_pendingIcloudEventDeletes;
    QHash<QString, QPointer<QNetworkReply>> m_inFlightIcloudNoteReplies;
    QHash<QString, QPointer<QNetworkReply>> m_inFlightIcloudEventReplies;
    QHash<QString, int> m_icloudMutationRetryCounts;
    QHash<QString, qint64> m_icloudMutationNextRetryMs;
    QSet<QString> m_icloudMutationPausedKeys;
    QTimer m_icloudMutationRetryTimer;
    qint64 m_icloudMutationRetryArmedDeadlineMs = 0;
    bool m_icloudMutationRetryScheduled = false;
    bool m_icloudPhotosSyncInFlight = false;
    int m_activeSendCount = 0;
};
