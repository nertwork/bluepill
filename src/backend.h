#include <atomic>
#include <functional>
#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QHostAddress>
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
    QString originalGroupId;
    QString groupId;
    QStringList siblingGuids;
    QString title;
    QString subtitle;
    QString service;
    QString avatarUrl;
    QString initials;
    QString avatarColor;
    QString lastMessagePreview;
    QString relativeTime;
    qint64 lastMessageDate = 0;
    qint64 originalRowId = 0;
    int unreadCount = 0;
    bool isGroup = false;
    bool isArchived = false;
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
    // How long after sending a message it can still be edited or unsent.
    Q_PROPERTY(int messageEditWindowMs READ messageEditWindowMs CONSTANT)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString serverStatus READ serverStatus NOTIFY statusChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY configChanged)
    // The configured primary (serverUrl may be the failover after a rotation).
    Q_PROPERTY(QString configuredServerUrl READ configuredServerUrl NOTIFY configChanged)
    Q_PROPERTY(QString secondaryServerUrl READ secondaryServerUrl NOTIFY configChanged)
    Q_PROPERTY(QString serverPassword READ serverPassword NOTIFY configChanged)
    Q_PROPERTY(bool hasPasswordConfigured READ hasPasswordConfigured NOTIFY configChanged)
    Q_PROPERTY(bool needsSetup READ needsSetup NOTIFY configChanged)
    Q_PROPERTY(QVariantList tailscaleRelays READ tailscaleRelays NOTIFY tailscaleRelaysChanged)
    Q_PROPERTY(QString tailscaleStatus READ tailscaleStatus NOTIFY tailscaleRelaysChanged)
    Q_PROPERTY(bool tailscaleScanning READ tailscaleScanning NOTIFY tailscaleRelaysChanged)
    // Bumped when a host name is confirmed as a tailnet node (see verifyTailnetHosts).
    Q_PROPERTY(int verifiedTailnetHostsRevision READ verifiedTailnetHostsRevision NOTIFY verifiedTailnetHostsChanged)
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
    Q_PROPERTY(bool notificationBannerEnabled READ notificationBannerEnabled WRITE setNotificationBannerEnabled NOTIFY notificationsChanged)
    Q_PROPERTY(bool notificationSoundEnabled READ notificationSoundEnabled WRITE setNotificationSoundEnabled NOTIFY notificationsChanged)
    Q_PROPERTY(bool notificationMuteWhenActive READ notificationMuteWhenActive WRITE setNotificationMuteWhenActive NOTIFY notificationsChanged)
    Q_PROPERTY(QString notificationSoundPreset READ notificationSoundPreset WRITE setNotificationSoundPreset NOTIFY notificationsChanged)
    Q_PROPERTY(QVariantList notificationSoundPresets READ notificationSoundPresets CONSTANT)
    Q_PROPERTY(bool omarchyDnd READ omarchyDnd NOTIFY omarchyDndChanged)
    Q_PROPERTY(QVariantList chats READ chats NOTIFY chatsChanged)
    Q_PROPERTY(QVariantList pinnedChats READ pinnedChats NOTIFY chatsChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(int totalUnread READ totalUnread NOTIFY chatsChanged)
    // Number of conversations, ignoring the filter and search (shown as "All (N)")
    Q_PROPERTY(int totalChatCount READ totalChatCount NOTIFY chatsChanged)
    Q_PROPERTY(bool windowActive READ windowActive WRITE setWindowActive NOTIFY windowActiveChanged)
    Q_PROPERTY(bool hasOlderMessages READ hasOlderMessages NOTIFY olderMessagesChanged)
    Q_PROPERTY(bool loadingOlderMessages READ loadingOlderMessages NOTIFY olderMessagesChanged)
    Q_PROPERTY(int loadedMessageCount READ loadedMessageCount NOTIFY messagesChanged)
    Q_PROPERTY(int historySyncDays READ historySyncDays WRITE setHistorySyncDays NOTIFY historySyncDaysChanged)
    Q_PROPERTY(bool showTopBarStatusPill READ showTopBarStatusPill WRITE setShowTopBarStatusPill NOTIFY uiPreferencesChanged)
    Q_PROPERTY(bool showPinnedRow READ showPinnedRow WRITE setShowPinnedRow NOTIFY uiPreferencesChanged)
    Q_PROPERTY(bool pinnedRowFillRecent READ pinnedRowFillRecent WRITE setPinnedRowFillRecent NOTIFY uiPreferencesChanged)
    Q_PROPERTY(bool hoverMessageActions READ hoverMessageActions WRITE setHoverMessageActions NOTIFY uiPreferencesChanged)
    Q_PROPERTY(bool syncingHistory READ syncingHistory NOTIFY syncingHistoryChanged)
    Q_PROPERTY(QString playingAudioGuid READ playingAudioGuid NOTIFY audioPlaybackChanged)
    Q_PROPERTY(bool audioPlaying READ audioPlaying NOTIFY audioPlaybackChanged)
    Q_PROPERTY(double audioProgress READ audioProgress NOTIFY audioPlaybackChanged)
    Q_PROPERTY(QString audioDurationText READ audioDurationText NOTIFY audioPlaybackChanged)
    Q_PROPERTY(bool recordingVoice READ recordingVoice NOTIFY voiceRecordingChanged)
    Q_PROPERTY(int voiceRecordingSeconds READ voiceRecordingSeconds NOTIFY voiceRecordingChanged)
    Q_PROPERTY(QVariantList voiceRecordingWaveform READ voiceRecordingWaveform NOTIFY voiceRecordingChanged)
    Q_PROPERTY(bool hasRecordedVoiceMemo READ hasRecordedVoiceMemo NOTIFY voiceRecordingChanged)
    Q_PROPERTY(int recordedVoiceDurationSeconds READ recordedVoiceDurationSeconds NOTIFY voiceRecordingChanged)
    Q_PROPERTY(QVariantList recordedVoiceWaveform READ recordedVoiceWaveform NOTIFY voiceRecordingChanged)
    Q_PROPERTY(QString recordedVoicePath READ recordedVoicePath NOTIFY voiceRecordingChanged)
    Q_PROPERTY(QString recordedVoiceChatGuid READ recordedVoiceChatGuid NOTIFY voiceRecordingChanged)
    Q_PROPERTY(QVariantList audioInputDevices READ audioInputDevices NOTIFY audioDevicesChanged)
    Q_PROPERTY(QVariantList audioOutputDevices READ audioOutputDevices NOTIFY audioDevicesChanged)
    Q_PROPERTY(bool audioInputDiscoverySucceeded READ audioInputDiscoverySucceeded NOTIFY audioDevicesChanged)
    Q_PROPERTY(bool audioOutputDiscoverySucceeded READ audioOutputDiscoverySucceeded NOTIFY audioDevicesChanged)
    Q_PROPERTY(QString selectedAudioInputDevice READ selectedAudioInputDevice WRITE setSelectedAudioInputDevice NOTIFY audioDevicesChanged)
    Q_PROPERTY(QString selectedAudioOutputDevice READ selectedAudioOutputDevice WRITE setSelectedAudioOutputDevice NOTIFY audioDevicesChanged)
    Q_PROPERTY(QVariantList scheduledMessages READ scheduledMessages NOTIFY scheduledMessagesChanged)
    Q_PROPERTY(QStringList selectedChatParticipants READ selectedChatParticipants NOTIFY selectedChatChanged)
    Q_PROPERTY(QVariantList icloudNotes READ icloudNotes NOTIFY icloudNotesChanged)
    Q_PROPERTY(QVariantList icloudEvents READ icloudEvents NOTIFY icloudEventsChanged)
    Q_PROPERTY(QVariantList icloudCalendars READ icloudCalendars NOTIFY icloudEventsChanged)
    Q_PROPERTY(QVariantList icloudPhotos READ icloudPhotos NOTIFY icloudPhotosChanged)
    Q_PROPERTY(int icloudPhotosTotal READ icloudPhotosTotal NOTIFY icloudPhotosChanged)
    Q_PROPERTY(int icloudPhotosPage READ icloudPhotosPage NOTIFY icloudPhotosChanged)
    Q_PROPERTY(int icloudPhotosPageSize READ icloudPhotosPageSize WRITE setIcloudPhotosPageSize NOTIFY icloudPhotosChanged)
    Q_PROPERTY(int icloudPhotosPageCount READ icloudPhotosPageCount NOTIFY icloudPhotosChanged)
    Q_PROPERTY(bool icloudPhotosHasMore READ icloudPhotosHasMore NOTIFY icloudPhotosChanged)
    Q_PROPERTY(bool icloudPhotosLoading READ icloudPhotosLoading NOTIFY icloudPhotosLoadingChanged)
    Q_PROPERTY(bool icloudPhotosSyncing READ icloudPhotosSyncing NOTIFY icloudPhotosSyncingChanged)
    Q_PROPERTY(bool icloudNotesEnabled READ icloudNotesEnabled WRITE setIcloudNotesEnabled NOTIFY icloudHubModulesChanged)
    Q_PROPERTY(bool icloudCalendarEnabled READ icloudCalendarEnabled WRITE setIcloudCalendarEnabled NOTIFY icloudHubModulesChanged)
    Q_PROPERTY(bool icloudPhotosEnabled READ icloudPhotosEnabled WRITE setIcloudPhotosEnabled NOTIFY icloudHubModulesChanged)
    Q_PROPERTY(bool sendingMessage READ sendingMessage NOTIFY sendingMessageChanged)


public:
    explicit Backend(bool autoStart = true, QObject *parent = nullptr);
    ~Backend() override;

    bool connected() const { return m_connected; }
    bool loading() const { return m_loading; }
    QString serverStatus() const { return m_serverStatus; }
    QString serverUrl() const { return m_serverUrl; }
    QString configuredServerUrl() const { return m_lastConfiguredBaseUrl.isEmpty() ? m_serverUrl : m_lastConfiguredBaseUrl; }
    QString secondaryServerUrl() const { return m_secondaryServerUrl; }
    QString serverPassword() const { return m_password; }
    bool hasPasswordConfigured() const { return !m_password.isEmpty(); }
    bool needsSetup() const { return !m_demoMode && (m_serverUrl.isEmpty() || (!m_serverUrl.startsWith(QStringLiteral("ssh://"), Qt::CaseInsensitive) && m_password.isEmpty())); }
    const QVariantList &tailscaleRelays() const { return m_tailscaleRelays; }
    const QString &tailscaleStatus() const { return m_tailscaleStatus; }
    bool tailscaleScanning() const { return m_tailscaleScanning; }
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
    bool notificationBannerEnabled() const { return m_notificationBannerEnabled; }
    bool notificationSoundEnabled() const { return m_notificationSoundEnabled; }
    bool notificationMuteWhenActive() const { return m_notificationMuteWhenActive; }
    QString notificationSoundPreset() const { return m_notificationSoundPreset; }
    QVariantList notificationSoundPresets() const;
    bool omarchyDnd() const;
    bool areOmarchyNotificationsEnabled() const;
    QVariantList chats() const;
    QVariantList pinnedChats() const;
    QVariantList messages() const { return m_messages; }
    int totalUnread() const;
    int totalChatCount() const { return static_cast<int>(m_chats.size()); }
    bool windowActive() const { return m_windowActive; }
    bool hasOlderMessages() const;
    bool loadingOlderMessages() const { return m_loadingOlderMessages; }
    int loadedMessageCount() const { return m_messages.size(); }
    int historySyncDays() const { return m_historySyncDays; }
    bool showTopBarStatusPill() const { return m_showTopBarStatusPill; }
    void setShowTopBarStatusPill(bool enabled);
    bool showPinnedRow() const { return m_showPinnedRow; }
    void setShowPinnedRow(bool enabled);
    bool pinnedRowFillRecent() const { return m_pinnedRowFillRecent; }
    void setPinnedRowFillRecent(bool enabled);
    bool hoverMessageActions() const { return m_hoverMessageActions; }
    void setHoverMessageActions(bool enabled);
    bool syncingHistory() const { return m_syncingHistory; }
    QString playingAudioGuid() const { return m_playingAudioGuid; }
    bool audioPlaying() const { return m_audioPlaying; }
    double audioProgress() const { return m_audioProgress; }
    QString audioDurationText() const { return m_audioDurationText; }
    bool recordingVoice() const { return m_recordingVoice; }
    int voiceRecordingSeconds() const { return m_voiceRecordingSeconds; }
    QVariantList voiceRecordingWaveform() const { return m_voiceRecordingWaveform; }
    bool hasRecordedVoiceMemo() const { return m_hasRecordedVoiceMemo; }
    int recordedVoiceDurationSeconds() const { return m_recordedVoiceDurationSeconds; }
    QVariantList recordedVoiceWaveform() const { return m_recordedVoiceWaveform; }
    QString recordedVoicePath() const { return m_recordedVoicePath; }
    QString recordedVoiceChatGuid() const { return m_recordedVoiceChatGuid; }
    QVariantList audioInputDevices() const { return m_audioInputDevices; }
    QVariantList audioOutputDevices() const { return m_audioOutputDevices; }
    bool audioInputDiscoverySucceeded() const { return m_audioInputDiscoverySucceeded; }
    bool audioOutputDiscoverySucceeded() const { return m_audioOutputDiscoverySucceeded; }
    QString selectedAudioInputDevice() const { return m_selectedAudioInputDevice; }
    QString selectedAudioOutputDevice() const { return m_selectedAudioOutputDevice; }
    void setSelectedAudioInputDevice(const QString &deviceId);
    void setSelectedAudioOutputDevice(const QString &deviceId);
    QVariantList scheduledMessages() const { return m_scheduledMessages; }
    QStringList selectedChatParticipants() const;
    QVariantList icloudNotes() const { return m_icloudNotes; }
    QVariantList icloudEvents() const { return m_icloudEvents; }
    QVariantList icloudCalendars() const { return m_icloudCalendars; }
    QVariantList icloudPhotos() const { return m_icloudPhotos; }
    int icloudPhotosTotal() const { return std::max(int(m_icloudPhotos.size()), m_icloudPhotosTotal); }
    int icloudPhotosPage() const { return m_icloudPhotosPage; }
    int icloudPhotosPageSize() const { return m_icloudPhotosPageSize; }
    void setIcloudPhotosPageSize(int size);
    int icloudPhotosPageCount() const {
        const int ps = std::max(1, m_icloudPhotosPageSize);
        const int tot = icloudPhotosTotal();
        return std::max(1, (tot + ps - 1) / ps);
    }
    bool icloudPhotosHasMore() const { return m_icloudPhotosHasMore; }
    bool icloudPhotosLoading() const { return m_icloudPhotosLoading; }
    bool icloudPhotosSyncing() const { return m_icloudPhotosSyncInFlight; }
    bool icloudNotesEnabled() const { return m_icloudNotesEnabled; }
    bool icloudCalendarEnabled() const { return m_icloudCalendarEnabled; }
    bool icloudPhotosEnabled() const { return m_icloudPhotosEnabled; }
    void setIcloudNotesEnabled(bool enabled);
    void setIcloudCalendarEnabled(bool enabled);
    void setIcloudPhotosEnabled(bool enabled);
    bool sendingMessage() const { return m_activeSendCount > 0; }
    void setIcloudPhotosForTest(const QVariantList &list) {
        m_icloudPhotos = list;
        m_icloudPhotosTotal = list.size();
        emit icloudPhotosChanged();
    }
    void cacheAttachmentPathForTest(const QString &guid, const QString &filePath) {
        m_attachmentPaths.insert(guid, filePath);
    }


    void setWindowActive(bool active);
    void setNotificationsEnabled(bool enabled);
    void setNotificationBannerEnabled(bool enabled);
    void setNotificationSoundEnabled(bool enabled);
    void setNotificationMuteWhenActive(bool enabled);
    void setNotificationSoundPreset(const QString &preset);
    Q_INVOKABLE void previewNotificationSound(const QString &preset = QString());
    Q_INVOKABLE void sendTestNotification();
    static QString ensureNotificationSoundFile(const QString &preset);
    void setStateFilePath(const QString &path);
    void setConfigFilePath(const QString &path);
    QString configFilePath() const;
    void checkServerAndLoad();
    Q_INVOKABLE void saveServerConfig(const QString &serverUrl, const QString &secondaryUrl, const QString &password);
    Q_INVOKABLE QString webUiUrl() const;
    // Default relay port used when a scheme-less or http:// URL omits one.
    static constexpr int kDefaultRelayPort = 1235;
    // fromUserInput=true (Settings): bare host -> http://, no port -> :1235.
    // fromUserInput=false (config.json / env vars / keyring): keep the legacy
    // behaviour exactly (bare host -> https://, no port added) so existing
    // endpoints are never rewritten.
    static QString normalizeServerEndpointUrl(const QString &rawUrl, bool fromUserInput = true);
    // True when two server URLs reach the same endpoint (scheme, host,
    // effective port, path), e.g. "https://relay" and "https://relay:443".
    static bool isSameServerEndpoint(const QString &a, const QString &b);
    // Splits "scheme://host:port/path" into {url: text without ":port", port}.
    // Keeps the typed text otherwise unchanged (no scheme is added). With
    // hideDefaultPort, a port equal to kDefaultRelayPort comes back as "".
    Q_INVOKABLE static QVariantMap splitServerUrl(const QString &rawUrl, bool hideDefaultPort = false);
    // Joins the URL and Port fields; a port typed inline in the URL wins.
    Q_INVOKABLE static QString composeServerUrl(const QString &rawUrl, const QString &port);
    // Parses `tailscale status --json` into online macOS peers:
    // [{name, host, ip, url}] (url = http://<MagicDNS name>:1235).
    static QVariantList parseTailscaleMacPeers(const QByteArray &statusJson, QString *backendState = nullptr);
    // True only for a bluepill-server/BlueBubbles-style JSON answer to /api/v1/ping.
    static bool looksLikeRelayResponse(int httpStatus, const QByteArray &body);
    // ssh://... or scheme-less user@host (no path).
    Q_INVOKABLE static bool isSshStyleServerUrl(const QString &url);
    // Every Tailscale IP of this node and its peers in `tailscale status --json`.
    static QSet<QString> parseTailscaleNodeIps(const QByteArray &statusJson);
    // True when every address is a Tailscale address that belongs to a tailnet node.
    static bool addressesAreTailnetNodes(const QList<QHostAddress> &addresses, const QSet<QString> &nodeIps);
    // Host names (e.g. a custom-domain Tailscale FQDN) confirmed to resolve only to tailnet nodes.
    static bool isVerifiedTailnetHost(const QString &host);
    static void markVerifiedTailnetHost(const QString &host);
    int verifiedTailnetHostsRevision() const { return m_verifiedTailnetHostsRevision; }
    // Settings: check a typed http:// host name in the background.
    Q_INVOKABLE void verifyTailnetHostForUrl(const QString &url);
    Q_INVOKABLE void discoverTailscaleRelays();
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
    // Pins: the Mac's (Messages' pinned conversations), minus ones unpinned here, plus ones pinned here
    QStringList macPinnedChatGuids() const;
    QStringList effectivePinnedChatGuids() const;
    void setMacPins(const QList<QVariantMap> &pins);
    void applyMacPinsResponseForTest(const QByteArray &body) { applyMacPinsResponse(body); }

    Q_INVOKABLE void setSearchQuery(const QString &query);
    Q_INVOKABLE void setFilterMode(const QString &mode);
    Q_INVOKABLE void refreshAll();
    // file:// URL of an attachment already on disk, or "".
    Q_INVOKABLE QString cachedAttachmentUrl(const QString &attachmentGuid) const;
    Q_INVOKABLE void selectChat(const QString &chatGuid);
    Q_INVOKABLE bool isSameConversation(const QString &guidA, const QString &guidB) const;
    Q_INVOKABLE QString resolveCanonicalChatGuid(const QString &chatGuid) const;
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
    // A thread: the original message plus every loaded reply to it, in timeline order.
    Q_INVOKABLE QVariantList threadMessages(const QString &rootGuid) const;
    // Index of a message in messages(), or -1 when it isn't loaded.
    Q_INVOKABLE int messageIndexForGuid(const QString &guid) const;
    Q_INVOKABLE void editMessage(const QString &messageGuid, const QString &newText);
    static constexpr int kMessageEditWindowMs = 2 * 60 * 1000;
    int editRepliesProcessedForTest() const { return m_editRepliesProcessed; }
    int messageEditWindowMs() const { return kMessageEditWindowMs; }
    Q_INVOKABLE void deleteChat(const QString &chatGuid = QString());
    Q_INVOKABLE void startNewChat(const QString &address, const QString &text = QString());
    Q_INVOKABLE void openAttachment(const QString &guid, const QString &transferName);
    Q_INVOKABLE void openUrl(const QString &url);
    Q_INVOKABLE void registerWindow(QObject *window);
    bool eventFilter(QObject *watched, QEvent *event) override;
    Q_INVOKABLE void loadDemoData();
    Q_INVOKABLE void seedDemoData() { loadDemoData(); }
    Q_INVOKABLE void saveContact(const QString &firstName, const QString &lastName, const QString &phone, const QString &email);
    Q_INVOKABLE void syncContactsNow();
    // A Mac contact's fields for Edit Contact: firstName, lastName, phone, email, and
    // rows ({label, value}) for every populated field. found=false when unknown.
    Q_INVOKABLE QVariantMap contactDetails(const QString &address) const;
    // Photos, links and files shared in a conversation (newest first): {photos, links, files}
    Q_INVOKABLE QVariantMap conversationMedia(const QString &chatGuid) const;
    Q_INVOKABLE void loadOlderMessages();

    // Emoji shortcode & picker helpers
    Q_INVOKABLE static QString replaceEmojiShortcodes(const QString &text);
    Q_INVOKABLE QVariantList searchEmojis(const QString &query = QString(),
                                          const QString &category = QString(),
                                          int limit = 60) const;

    // Clipboard & Verification/OTP code extraction
    Q_INVOKABLE void copyToClipboard(const QString &text, const QString &label = QString());
    Q_INVOKABLE void copyTextToClipboard(const QString &text) { copyToClipboard(text); }
    Q_INVOKABLE bool copyImageToClipboard(const QString &localUrlOrPath, const QString &attachmentGuid = QString());
    Q_INVOKABLE static QString extractVerificationCode(const QString &text);
    Q_INVOKABLE static QString extractOtpCode(const QString &text) { return extractVerificationCode(text); }
    Q_INVOKABLE QString pasteClipboardImageToTempFile();
    Q_INVOKABLE void sendAttachment(const QString &chatGuid, const QString &filePath, const QString &caption = QString());
    // Audio, Voice, Effects & Scheduling
    Q_INVOKABLE static bool isAudioAttachment(const QString &mimeType, const QString &fileName);
    // GIF / WebP attachments play inline with AnimatedImage.
    Q_INVOKABLE static bool isAnimatedImageAttachment(const QString &mimeType, const QString &fileName);
    // 1-3 when the text is only emoji (shown large without a bubble), else 0.
    Q_INVOKABLE static int jumboEmojiCount(const QString &text);
    Q_INVOKABLE static QVariantList computeWaveformBars(const QString &seed, int count = 28);
    Q_INVOKABLE void toggleAudioPlayback(const QString &attachmentGuid, const QString &sourceUrl = QString());
    Q_INVOKABLE void seekAudioPlayback(const QString &attachmentGuid, double ratio);
    Q_INVOKABLE void startVoiceRecording();
    Q_INVOKABLE void stopVoiceRecordingForPreview();
    Q_INVOKABLE void togglePreviewRecordedVoiceMemo();
    Q_INVOKABLE void sendRecordedVoiceMemo();
    Q_INVOKABLE void stopAndSendVoiceRecording();
    Q_INVOKABLE void cancelVoiceRecording();
    Q_INVOKABLE bool canSendRecordedVoiceMemoToSelectedChat() const;
    Q_INVOKABLE void refreshAudioDevices();
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
    Q_INVOKABLE QString saveIcloudNote(const QString &id, const QString &title, const QString &body, const QString &folder = QStringLiteral("Notes"), qint64 baseRevision = -1, bool allowRecreate = false);
    Q_INVOKABLE bool reloadConflictedIcloudNoteServerVersion(const QString &id);
    Q_INVOKABLE bool isIcloudNoteDurablyPersisted(const QString &id) const;
    Q_INVOKABLE void deleteIcloudNote(const QString &id);
    Q_INVOKABLE void fetchIcloudEvents();
    Q_INVOKABLE QString saveIcloudEvent(const QString &title, qint64 startMs, qint64 endMs, const QString &calendarName = QStringLiteral("Home"), const QString &notes = QString());
    Q_INVOKABLE void deleteIcloudEvent(const QString &id);
    Q_INVOKABLE void fetchIcloudPhotos(int page = -1, bool append = false, bool forceNudge = false);
    Q_INVOKABLE void fetchIcloudPhotosTotalOnly();
    Q_INVOKABLE void nextIcloudPhotosPage();
    Q_INVOKABLE void prevIcloudPhotosPage();
    Q_INVOKABLE void loadMoreIcloudPhotos();
    Q_INVOKABLE void downloadCloudAttachment(const QString &attachmentGuid);
    Q_INVOKABLE void syncIcloudPhotos();
    Q_INVOKABLE void shareIcloudPhotoToChat(const QString &chatGuid, const QString &attachmentGuid, const QString &fileName = QString());
    Q_INVOKABLE QString attachmentDownloadUrl(const QString &guid) const;
    bool isSecureOrLocalIcloudEndpoint() const;
    void replayPendingIcloudMutations();
    void reconcileRemoteIcloudNotesForTest(const QJsonArray &data, const QString &acknowledgedNoteId = QString(), quint64 acknowledgedMutationSeq = 0) { applyRemoteIcloudNotes(data, acknowledgedNoteId, acknowledgedMutationSeq); }
    void reconcileRemoteIcloudEventsForTest(const QJsonArray &data, const QString &acknowledgedEventId = QString()) { applyRemoteIcloudEvents(data, acknowledgedEventId); }
    quint64 icloudNotesGenerationForTest() const { return m_icloudNotesGeneration; }
    quint64 icloudEventsGenerationForTest() const { return m_icloudEventsGeneration; }
    void recordIcloudMutationFailureForTest(const QString &mutationKey) { recordIcloudMutationFailure(mutationKey); }
    void clearIcloudMutationRetryStateForTest(const QString &mutationKey) { clearIcloudMutationRetryState(mutationKey); }
    bool isIcloudMutationInBackoffForTest(const QString &mutationKey) const;
    bool isIcloudMutationPausedForTest(const QString &mutationKey) const { return m_icloudMutationPausedKeys.contains(mutationKey); }
    bool hasPendingIcloudNoteSaveForTest(const QString &noteId) const { return m_pendingIcloudNoteSaves.contains(noteId); }
    bool hasPendingIcloudNoteDeleteForTest(const QString &noteId) const { return m_pendingIcloudNoteDeletes.contains(noteId); }
    bool hasPendingIcloudEventSaveForTest(const QString &evtId) const { return m_pendingIcloudEventSaves.contains(evtId); }
    bool hasPendingIcloudEventDeleteForTest(const QString &evtId) const { return m_pendingIcloudEventDeletes.contains(evtId); }
    int icloudMutationRetryCountForTest(const QString &mutationKey) const { return m_icloudMutationRetryCounts.value(mutationKey, 0); }
    void scheduleIcloudMutationRetryForTest(bool immediate) { scheduleIcloudMutationRetry(immediate); }

    // Network & Failover state
    QString currentServerUrl() const { return m_serverUrl; }
    QStringList candidateUrls() const { return m_candidateUrls; }
    int consecutiveFailures() const { return m_consecutiveFailures; }
    void attemptReconnect();
    void simulateNetworkFailureForTest() { handleNetworkFailure(QStringLiteral("test")); }
    void simulateNetworkSuccessForTest() { handleNetworkSuccess(); }
    int notificationCountForTest() const { return m_notificationCountForTest; }
    int soundPlayCountForTest() const { return m_soundPlayCountForTest; }
    QString lastSoundPresetPlayedForTest() const { return m_lastSoundPresetPlayed; }
    void resetNotificationCountersForTest() {
        m_notificationCountForTest = 0;
        m_soundPlayCountForTest = 0;
        m_lastSoundPresetPlayed.clear();
    }
    void rebuildSelectedChatMessagesForTest() { rebuildSelectedChatMessages(); }
    QString contactAvatarApiPathForTest(const QString &handle) const { return m_contactAvatarApiPaths.value(normalizeHandle(handle)); }
    QString contactAvatarCacheFileForTest(const QString &apiPath) const { return avatarCacheFile(apiPath); }
    void setAvatarRetryBaseMsForTest(qint64 ms) { m_avatarRetryBaseMs = ms; }
    void expireContactAvatarsForTest() { for (auto it = m_contactAvatarFetchedAtMs.begin(); it != m_contactAvatarFetchedAtMs.end(); ++it) it.value() -= kAvatarRefreshMs + 1; }
    void fetchContactAvatarsForChatsForTest() { fetchContactAvatarsForChats(); }
    void applyContactAvatarForTest(const QString &apiPath, const QString &file) { applyContactAvatar(apiPath, file); }
    void downloadAttachmentForTest(const QString &guid, const QString &transferName) { downloadAttachment(guid, transferName, false); }
    bool tailnetCheckPendingForTest() const { return tailnetCheckPending(); }

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
    // True when the bytes are an image Qt can decode (not, say, an HTML error page).
    static bool isDecodableImageData(const QByteArray &bytes);
    static bool isDecodableImageFile(const QString &path);
    static QString formatExpressiveEffectLabel(const QString &styleId);
    static QString extractFirstUrl(const QString &text);
    static QString stripTrailingUrlPunctuation(QString url);
    static bool isYouTubeUrl(const QString &url);
    static QString formatMessageTextWithLinks(const QString &raw);
    static bool isImageMimeOrExt(const QString &mime, const QString &fileName);
    static QJsonArray deduplicateMessageAttachments(const QJsonArray &atts);
    static bool isMessageDeletedOrRetracted(const QJsonObject &m);
    static QString computeOutgoingReceiptText(
        const QJsonObject &msgObj,
        bool hasLaterIncomingReply = false,
        qint64 nowMs = 0,
        qint64 addedAtMs = 0,
        bool forceExpired = false);
    static int wavDurationSeconds(const QString &filePath);
    static QVariantList computeAudioFileWaveformBars(const QString &filePath, const QString &seed, int count = 28);
    static QVariantList parsePactlDeviceList(const QByteArray &rawOutput, bool filterMonitorSources);
    static QVariantList buildMessageListFromApi(
        const QJsonArray &rawMessagesDesc,
        const QHash<QString, QString> &contactNames,
        const QHash<QString, QString> &attachmentCache,
        QStringList *missingAttachmentsOut = nullptr,
        const QHash<QString, QVariantMap> &previewCache = {},
        QStringList *missingPreviewsOut = nullptr,
        const QSet<QString> &deletedMessageGuids = {},
        const QHash<QString, QString> &recentOutboundEffects = {},
        const QHash<QString, qint64> &pendingDeliveryAddedAtMs = {},
        const QSet<QString> &expiredPendingDeliveryGuids = {});

    // Contact resolution & state
    void loadCredentials();
    void loadConfig() { loadCredentials(); }
    void loadAppState();
    bool saveAppState() const;
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
    // Resolves "iMessage" / "RCS" / "SMS" (or "" if unknown) for a chat.
    // macOS 26+ writes chat GUIDs as "any;-;<handle>", so the prefix alone
    // is not enough: fall back to the chat's last message, then participants.
    // evidenceDate (optional) receives the dateCreated of the message that
    // decided the service, or 0 when it came from undated evidence
    // (GUID prefix, chat-level field, participants).
    static QString resolveChatService(const QJsonObject &chat, const QJsonObject &message = QJsonObject(),
                                      qint64 *evidenceDate = nullptr);
    static QString normalizeServiceName(const QString &raw);
    void reconcileUnreadMessagesForTest(const QJsonArray &unreadMessages);
    void reconcileChatMessagesForTest(const QString &chatGuid, const QJsonArray &serverMessages);
    void handleSocketIoPayloadForTest(const QByteArray &rawPayload);
    void pollDeltaForTest() { pollDelta(); }
    void startSocketIoForTest() { startSocketIoClient(); }
    QString socketSid() const { return m_socketSid; }
    QString resolveCanonicalChatGuidForTest(const QString &chatGuid) const { return resolveCanonicalChatGuid(chatGuid); }
    QString resolveActiveSendChatGuidForTest(const QString &chatGuid) const { return resolveActiveSendChatGuid(chatGuid); }
    QString currentVoiceRecordingPathForTest() const { return m_currentVoiceRecordingPath; }
    int pendingDeliveryCountForTest() const { return m_pendingDeliveryMessages.size(); }
    int pendingDeliveryInFlightCountForTest() const { return m_inFlightDeliveryCheckGuids.size(); }
    quint64 pendingDeliveryCheckCompletedCountForTest() const { return m_pendingDeliveryCheckCompletedCountForTest; }
    void checkPendingDeliveryStatusesForTest() { checkPendingDeliveryStatuses(); }
    QStringList lastPendingDeliveryBatchForTest() const { return m_lastPendingDeliveryBatchForTest; }
    void addPendingDeliveryForTest(const QString &msgGuid, const QString &chatGuid, qint64 addedAtMs = 0);
    void agePendingDeliveryForTest(const QString &msgGuid, qint64 ageMs);
    void setMessagesForTest(const QVariantList &messages) {
        m_messages = messages;
        emit messagesChanged();
    }
    void markLiveIncomingAttachmentForTest(const QString &attachmentGuid) {
        if (!attachmentGuid.isEmpty()) m_liveIncomingAttachments.insert(attachmentGuid);
    }
    qint64 attachmentCooldownUntilMsForTest(const QString &attachmentGuid) const {
        return m_missingAttachmentCooldownUntilMs.value(attachmentGuid, 0);
    }
    static bool hasNonEmptyWavDataChunk(const QString &filePath);
    static bool isTrustedPasswordTransportUrl(const QUrl &url);
    // QML: false when Settings would save an http:// URL the guard above refuses.
    Q_INVOKABLE static bool isPasswordTransportAllowed(const QString &url);
    QNetworkRequest makeRequestForTest(const QString &pathAndQuery) const { return makeRequest(pathAndQuery); }
    void setTailscaleRelaysForTest(const QVariantList &relays) { setTailscaleScanState(relays, QStringLiteral("done"), false); }
    quint64 serverEndpointGenerationForTest() const { return m_serverEndpointGeneration; }
    bool supportsBatchMessageGuidInQueryForTest() const { return m_supportsBatchMessageGuidInQuery; }
    bool isExpiredPendingDeliveryForTest(const QString &msgGuid) const { return m_expiredPendingDeliveryGuids.contains(msgGuid); }
    QJsonObject cachedMessageForTest(const QString &chatGuid, const QString &msgGuid) const {
        return m_chatMessagesCache.value(chatGuid).value(msgGuid);
    }
    void setAudioDevicesForTest(const QVariantList &inputs, const QVariantList &outputs,
                                bool inputDiscoverySucceeded = true, bool outputDiscoverySucceeded = true) {
        m_audioInputDevices = inputs;
        m_audioOutputDevices = outputs;
        m_audioInputDiscoverySucceeded = inputDiscoverySucceeded;
        m_audioOutputDiscoverySucceeded = outputDiscoverySucceeded;
        emit audioDevicesChanged();
    }
    QString effectiveAudioInputDeviceForTest() const { return effectiveAudioInputDevice(); }
    QString effectiveAudioOutputDeviceForTest() const { return effectiveAudioOutputDevice(); }
    static bool writeConfigAtomicallyForTest(const QString &targetPath, const QJsonObject &obj,
                                             bool simulatePrePermFail = false, bool simulatePostPermFail = false);

    // Headless CLI entrypoints
    int runCliStatus(bool asJson);
    int runCliRecent(int limit, bool asJson);
    int runCliSend(const QString &target, const QString &message, bool asJson);

signals:
    void icloudNotesChanged();
    void icloudEventsChanged();
    void icloudPhotosChanged();
    void icloudPhotosLoadingChanged();
    void icloudPhotosSyncingChanged();
    void icloudHubModulesChanged();
    void sendingMessageChanged();
    void statusChanged();
    void configChanged();
    void tailscaleRelaysChanged();
    void verifiedTailnetHostsChanged();
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
    void uiPreferencesChanged();
    void syncingHistoryChanged();
    void audioPlaybackChanged();
    void voiceRecordingChanged();
    void audioDevicesChanged();
    void scheduledMessagesChanged();

    void toastMessage(const QString &text);
    void attachmentReady(const QString &attachmentGuid, const QString &localUrl);
    void linkPreviewUpdated(const QString &messageGuid, const QVariantMap &previewData);
    void pendingDeliveryCheckFinishedForTest();

private:
    // Finds a cached message; chatKey gets the m_chatMessagesCache key it's under.
    QJsonObject findCachedMessage(const QString &messageGuid, QString *chatKey = nullptr) const;
    // The chat a message actually belongs to on the Mac (not the merged canonical chat).
    static QString messageOwnChatGuid(const QJsonObject &msgObj, const QString &fallbackChatGuid);
    void applyLocalUnsend(const QString &messageGuid, const QString &chatGuid);
    // Edits to one message go to the Mac one at a time, in order: while one is
    // in flight only the newest further edit waits behind it. A failure rolls
    // back to the last text the Mac confirmed, unless a sync changed it meanwhile.
    struct EditSnapshot { QString text; QJsonValue dateEdited; QJsonValue attributed; };
    struct EditTracking {
        bool inFlight = false;
        bool hasQueued = false;
        QString queuedText;
        QString shownText;      // what this client last put on screen
        QString chatKey;
        QString sendChatGuid;
        int partIndex = 0;
        EditSnapshot confirmed;
    };
    QHash<QString, EditTracking> m_editTracking;
    int m_editRepliesProcessed = 0;
    void sendEditRequest(const QString &msgGuid, const QString &text);
    void applyEditedText(const QString &chatKey, const QString &msgGuid, const QString &text,
                         const QJsonValue &dateEdited, const QJsonValue &attributed);
    static bool parseJsonReplyObject(const QByteArray &bytes, QJsonObject *out);
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
    void sendAttachmentInternal(const QString &chatGuid, const QString &filePath, const QString &caption, bool ownedVoiceRecording);
    QString resolveMergedOutgoingReceiptAndPending(const QString &canonicalGuid, const QString &realGuid, const QJsonObject &fallbackObj, int *outErrCode = nullptr);
    void fetchLinkPreview(const QString &msgGuid, const QString &url);
    void processIncomingMessages(const QJsonArray &messagesArray, bool isInitialLoad);
    void recordChatGroupMetadata(const QJsonObject &chatObj);
    QString resolveActiveSendChatGuid(const QString &chatGuid) const;
    QString computeGroupMergeKey(const ChatEntry &entry) const;
    void unifySiblingChats(bool *selectedMigrated = nullptr, bool *chatsMerged = nullptr);
    void checkPendingDeliveryStatuses();
    void clearPendingDeliveryTrackingForGuid(const QString &msgGuid, bool clearExpired = true);
    void bumpServerEndpointGeneration();
    void dispatchPendingDeliverySingleQueries(const QStringList &guids, quint64 genAtDispatch,
                                              const QString &endpointAtDispatch,
                                              const QHash<QString, quint64> &observedSeqAtDispatch,
                                              quint64 reconcileSeqAtDispatch);
    bool filterPendingDeliveryResponseItems(const QJsonArray &arr, const QSet<QString> &batchSet,
                                            const QHash<QString, quint64> &observedSeqAtDispatch,
                                            quint64 reconcileSeqAtDispatch,
                                            QJsonArray *outNonStale, int *outMatchedCount) const;
    void stopAudioPlaybackProcess();
    bool startLocalAudioProcess(const QString &localFile, quint64 playbackGen);
    bool canSeekLocalAudioPlayback() const;
    void enforceLatestOutgoingReceiptsInTimeline();
    void stopVoiceRecordProcess(bool waitForFlush);
    QString finalizeCapturedVoiceWav(const QString &recPath);
    void clearStagedVoiceMemo(bool removeFile);
    void saveAudioDeviceConfig();
    QString effectiveAudioInputDevice() const;
    QString effectiveAudioOutputDevice() const;
    static QString formatAudioDuration(int totalSeconds);
    void rebuildChatPresentation(ChatEntry &entry) const;
    void sortChats();
    bool shouldNotifyForIncomingMessage(const QJsonObject &m, const QString &chatGuid, qint64 dateCreated, qint64 rowId) const;
    void notifyIncomingMessage(const QString &chatGuid, const QString &sender, const QString &preview);
    void playNotificationSound(const QString &presetOverride = QString(), bool forcePreview = false);
    void handleNetworkFailure(const QString &context);
    void handleNetworkSuccess();
    void applyRemoteIcloudNotes(const QJsonArray &data, const QString &acknowledgedNoteId = QString(), quint64 acknowledgedMutationSeq = 0);
    void applyRemoteIcloudEvents(const QJsonArray &data, const QString &acknowledgedEventId = QString());
    void scheduleIcloudMutationRetry(bool immediate);
    void recordIcloudMutationFailure(const QString &mutationKey);
    void clearIcloudMutationRetryState(const QString &mutationKey);
    void syncIcloudPhotosPage(int offset, int accumulatedSynced, const QString &jobId = QString());

    QNetworkAccessManager m_nam;
    QNetworkAccessManager m_sendNam;
    QNetworkAccessManager m_mediaNam;
    QNetworkAccessManager m_socketNam;
    QVariantList m_tailscaleRelays;
    QString m_tailscaleStatus;
    bool m_tailscaleScanning = false;
    int m_tailscalePendingProbes = 0;
    void setTailscaleScanState(const QVariantList &relays, const QString &status, bool scanning);
    // http:// host names that need a tailnet check before the password may be sent.
    QStringList hostsNeedingTailnetVerification() const;
    void verifyTailnetHosts(const QStringList &hosts, std::function<void()> done);
    void notifyTailnetVerifyWaiters();
    // True while an http:// host still needs (or is getting) its tailnet check.
    bool tailnetCheckPending() const;
    QList<std::function<void()>> m_tailnetVerifyWaiters;
    bool m_resumeAfterTailnetCheck = false;
    QSet<QString> m_tailnetHostsChecked;      // checked this session (verified or not)
    QSet<QString> m_tailnetHostsInFlight;
    int m_verifiedTailnetHostsRevision = 0;
    void probeTailscaleRelay(const QString &key, const QString &baseUrl, bool allowIpFallback);
    QHash<QString, qint64> m_chatServiceDates;  // chat guid -> date of message that set entry.service
    bool m_chatServiceEmitPending = false;
    void refineChatServiceFromMessage(const QString &chatGuid, const QJsonObject &msg);
    // Dated (message) evidence wins if it is at least as new as what we have;
    // undated evidence only fills in a chat that has no message evidence yet.
    bool applyChatServiceEvidence(ChatEntry &entry, const QString &svc, qint64 evidenceDate);
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
    QString m_lastConfiguredBaseUrl;
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
    bool m_notificationBannerEnabled = true;
    bool m_notificationSoundEnabled = true;
    bool m_notificationMuteWhenActive = true;
    QString m_notificationSoundPreset = QStringLiteral("chime");
    int m_notificationCountForTest = 0;
    int m_soundPlayCountForTest = 0;
    QString m_lastSoundPresetPlayed;
    QString m_serverStatus = QStringLiteral("Connecting...");
    QString m_lastKnownServerStatus = QStringLiteral("Connected · Private API");
    int m_historySyncDays = 60;
    bool m_showTopBarStatusPill = false;
    bool m_showPinnedRow = true;
    bool m_pinnedRowFillRecent = true;
    bool m_hoverMessageActions = true;
    bool m_syncingHistory = false;
    int m_historySyncGeneration = 0;
    bool m_stayOnHomePage = false;
    QList<QVariantMap> m_macPins;          // from GET /api/v1/pinned
    QSet<QString> m_unpinnedMacChats;      // Mac pins hidden here
    void fetchMacPins();
    quint64 m_macPinsRequestSeq = 0;       // only the newest /pinned reply is applied
    QSet<QString> m_requestedPinChatGuids; // pinned chats fetched individually (outside the chat page)
    void fetchUnresolvedPinnedChats();
    bool isMacPinHidden(const QString &chatGuid) const;
    void applyMacPinsResponse(const QByteArray &body);

    QString m_searchQuery;
    QString m_filterMode = QStringLiteral("all");
    QString m_selectedChatGuid;

    QHash<QString, QString> m_contactNames;
    QHash<QString, QVariantMap> m_contactRecords;  // full Mac contact, by normalized handle
    // Contact photos served by bluepill-server, fetched for chat participants
    QHash<QString, QString> m_contactAvatarApiPaths;  // normalized handle -> /api/v1/contact/<id>/avatar
    QStringList m_contactAvatarQueue;
    // Fetch state, keyed by relay + avatar path (avatarFetchKey)
    QSet<QString> m_contactAvatarInFlight;
    QHash<QString, qint64> m_contactAvatarFetchedAtMs;              // last success (or fresh disk copy)
    struct AvatarFailure { int attempts = 0; qint64 retryAtMs = 0; };
    QHash<QString, AvatarFailure> m_contactAvatarFailures;
    int m_contactAvatarsInFlight = 0;
    bool m_avatarScanPending = false;
    qint64 m_avatarRetryBaseMs = 30000;
    static constexpr qint64 kAvatarRefreshMs = 24LL * 60 * 60 * 1000;
    static constexpr int kAvatarMaxAttempts = 5;
    QString avatarFetchKey(const QString &apiPath) const;
    QString avatarCacheFile(const QString &apiPath) const;
    bool m_avatarRefreshPending = false;
    void fetchContactAvatarsForChats();
    void pumpContactAvatarQueue();
    void applyContactAvatar(const QString &apiPath, const QString &file);
    QHash<QString, QString> m_contactRawAddresses;
    QHash<QString, QString> m_contactAvatars;
    QHash<QString, QStringList> m_chatParticipants;
    QSet<QString> m_fetchedChatParticipantGuids;
    QHash<QString, QString> m_chatDisplayNames;
    QHash<QString, int> m_chatStyles;
    QHash<QString, QString> m_chatOriginalGroupIds;
    QHash<QString, QString> m_chatGroupIds;
    QHash<QString, QString> m_canonicalChatAliases;
    QHash<QString, QStringList> m_chatSiblingGuids;
    QHash<QString, QString> m_chatPreferredSendGuids;
    QHash<QString, QString> m_pendingDeliveryMessages;
    QHash<QString, qint64> m_pendingDeliveryAddedAtMs;
    QSet<QString> m_expiredPendingDeliveryGuids;
    QSet<QString> m_inFlightDeliveryCheckGuids;
    quint64 m_pendingDeliveryCheckCompletedCountForTest = 0;
    quint64 m_deliveryObservationCounter = 0;
    quint64 m_historyReconcileSeq = 0;
    quint64 m_serverEndpointGeneration = 1;
    bool m_supportsBatchMessageGuidInQuery = true;
    QHash<QString, quint64> m_messageObservationSeq;
    QHash<QString, quint64> m_reconcileRemovedSeqByGuid;
    int m_pendingDeliveryRotationCursor = 0;
    QStringList m_lastPendingDeliveryBatchForTest;
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
    QSet<QString> m_notifiedMessageGuids;
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
    qint64 m_lastNotifiedTimestamp = 0;
    qint64 m_highestMessageRowId = 0;
    qint64 m_startupHighestRowId = 0;
    qint64 m_startupTimestamp = 0;
    bool m_initialSyncComplete = false;
    qint64 m_lastContactsFetchMs = 0;
    QString m_playingAudioGuid;
    bool m_audioPlaying = false;
    double m_audioProgress = 0.0;
    QString m_audioDurationText = QStringLiteral("0:00");
    QTimer m_audioPlaybackTimer;
    int m_audioPlaybackElapsedMs = 0;
    int m_audioSeekOffsetMs = 0;
    int m_audioPlaybackDurationMs = 15000;
    quint64 m_audioPlaybackGen = 0;
    QString m_currentAudioLocalFile;
    QString m_currentAudioSourceUrl;
    QPointer<QNetworkReply> m_audioDownloadReply;
    QPointer<QProcess> m_audioProcess;

    bool m_recordingVoice = false;
    int m_voiceRecordingSeconds = 0;
    QVariantList m_voiceRecordingWaveform;
    QTimer m_voiceRecordingTimer;
    QString m_currentVoiceRecordingPath;
    bool m_hasRecordedVoiceMemo = false;
    int m_recordedVoiceDurationSeconds = 0;
    QVariantList m_recordedVoiceWaveform;
    QString m_recordedVoicePath;
    QString m_recordedVoiceChatGuid;
    QPointer<QProcess> m_voiceRecordProcess;
    QVariantList m_audioInputDevices;
    QVariantList m_audioOutputDevices;
    bool m_audioInputDiscoverySucceeded = false;
    bool m_audioOutputDiscoverySucceeded = false;
    QString m_selectedAudioInputDevice;
    QString m_selectedAudioOutputDevice;

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
    int m_icloudPhotosTotal = 0;
    int m_icloudPhotosPage = 0;
    int m_icloudPhotosPageSize = 60;
    bool m_icloudPhotosHasMore = false;
    bool m_icloudPhotosLoading = false;
    quint64 m_icloudPhotosRequestSeq = 0;
    quint64 m_icloudPhotosTotalRequestSeq = 0;
    bool m_icloudNotesEnabled = true;
    bool m_icloudCalendarEnabled = true;
    bool m_icloudPhotosEnabled = true;
    int m_activeSendCount = 0;
    QString m_lastCopiedImagePath;
    qint64 m_lastCopiedImageTimeMs = 0;
    std::shared_ptr<std::atomic<uint64_t>> m_clipboardCopySeq = std::make_shared<std::atomic<uint64_t>>(0);
    QString m_pendingClipboardCopyGuid;
    QString m_lastOpenedUrl;
    qint64 m_lastOpenedUrlTimeMs = 0;
};
