#include <QHttpMultiPart>
#include <QMimeData>
#include <QKeyEvent>
#include "backend.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QBuffer>
#include <QImageReader>
#include <QImageWriter>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <tuple>
#include <vector>

static QString cleanIcloudNoteBody(const QString &title, const QString &body) {
    QString cleanTitle = title;
    cleanTitle.remove(QChar(0xFFFC));
    cleanTitle.remove(QLatin1Char('\r'));
    cleanTitle = cleanTitle.trimmed();

    QString cleanBody = body;
    cleanBody.remove(QChar(0xFFFC));
    cleanBody.remove(QLatin1Char('\r'));
    cleanBody = cleanBody.trimmed();
    if (cleanBody.isEmpty()) return QString();

    static const QRegularExpression reCrdtNoise(
        QStringLiteral(R"((?:\n+\s*(?:Wz&M|\}\$\}h|dNP\}M|P\.qFB|sqeL|iL\tH)[\s\S]*)$)")
    );
    cleanBody.remove(reCrdtNoise);
    cleanBody = cleanBody.trimmed();

    if (!cleanTitle.isEmpty()) {
        if (cleanBody.size() > cleanTitle.size() &&
            cleanBody.mid(1).startsWith(cleanTitle) &&
            (cleanBody.size() == cleanTitle.size() + 1 ||
             cleanBody.mid(1 + cleanTitle.size()).startsWith(QLatin1Char('\n')))) {
            cleanBody = cleanBody.mid(1).trimmed();
        }
        if (cleanBody == cleanTitle) {
            return QString();
        }
        if (cleanBody.startsWith(cleanTitle + QLatin1Char('\n'))) {
            cleanBody = cleanBody.mid(cleanTitle.size() + 1).trimmed();
        }
    }
    return cleanBody;
}

static QString stripAttachmentGuidPrefix(const QString &guid) {
    const QString g = guid.trimmed();
    static const QRegularExpression reAtPrefix(
        QStringLiteral(R"(^at_\d+_)"),
        QRegularExpression::CaseInsensitiveOption
    );
    const QRegularExpressionMatch m = reAtPrefix.match(g);
    if (m.hasMatch()) {
        return g.mid(m.capturedLength());
    }
    if (g.startsWith(QStringLiteral("at_"), Qt::CaseInsensitive)) {
        return g.mid(3);
    }
    return g;
}

static bool attachmentGuidsMatch(const QString &g1, const QString &g2) {
    if (g1.isEmpty() || g2.isEmpty()) return false;
    if (g1 == g2) return true;
    if (g1.size() >= 11 && g2.size() >= 11 && g1.left(16) == g2.left(16)) return true;
    const QString b1 = stripAttachmentGuidPrefix(g1);
    const QString b2 = stripAttachmentGuidPrefix(g2);
    if (!b1.isEmpty() && b1 == b2) return true;
    if (b1.size() >= 16 && b2.size() >= 16 && b1.left(16) == b2.left(16)) return true;
    if (b1.size() >= 11 && b2.size() >= 11 && b1.left(11) == b2.left(11)) return true;
    return false;
}

static QString lookupCachedAttachmentPath(const QHash<QString, QString> &cache, const QString &guid) {
    if (guid.isEmpty()) return QString();
    QString p = cache.value(guid);
    if (!p.isEmpty()) return p;
    p = cache.value(guid.left(16));
    if (!p.isEmpty()) return p;
    const QString bare = stripAttachmentGuidPrefix(guid);
    if (!bare.isEmpty()) {
        p = cache.value(bare);
        if (!p.isEmpty()) return p;
        p = cache.value(bare.left(16));
        if (!p.isEmpty()) return p;
        if (bare.size() >= 11) {
            p = cache.value(bare.left(11));
            if (!p.isEmpty()) return p;
        }
    }
    return QString();
}

static void insertCachedAttachmentPath(QHash<QString, QString> &cache, const QString &key, const QString &path) {
    if (key.isEmpty() || path.isEmpty()) return;
    const bool isJpg = path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive) ||
                       path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive);
    auto putKey = [&](const QString &k) {
        if (k.size() < 6) return;
        if (!cache.contains(k) || isJpg) {
            cache.insert(k, path);
        }
    };
    putKey(key);
    putKey(key.left(16));
    const QString bare = stripAttachmentGuidPrefix(key);
    if (!bare.isEmpty()) {
        putKey(bare);
        putKey(bare.left(16));
        if (bare.size() >= 11) {
            putKey(bare.left(11));
        }
    }
}
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTextBoundaryFinder>
#include "emoji_ranges.h"
#include <QHostInfo>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>

static bool writeConfigAtomicallyImpl(const QString &targetPath, const QJsonObject &obj,
                                      bool simulatePrePermFail = false, bool simulatePostPermFail = false) {
    if (targetPath.isEmpty()) return false;
    const QFileInfo targetFi(targetPath);
    if (targetFi.exists() && targetFi.isDir()) return false;
    QDir().mkpath(targetFi.absolutePath());
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    QSaveFile outFile(targetPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QFileDevice::Permissions ownerRw = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    const QFileDevice::Permissions groupOtherMask =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    if (simulatePrePermFail || !outFile.setPermissions(ownerRw)) {
        outFile.cancelWriting();
        return false;
    }
    if (outFile.write(payload) != payload.size() || !outFile.flush()) {
        outFile.cancelWriting();
        return false;
    }
    const QFileDevice::Permissions stagedPerms = outFile.permissions();
    if (simulatePostPermFail ||
        (stagedPerms & groupOtherMask) != 0 ||
        (stagedPerms & ownerRw) != ownerRw) {
        qWarning() << "writeConfigAtomically: failed to enforce 0600 permissions on staged" << targetPath;
        outFile.cancelWriting();
        return false;
    }
    if (!outFile.commit()) {
        return false;
    }
    return true;
}

static bool writeConfigAtomically(const QString &targetPath, const QJsonObject &obj) {
    return writeConfigAtomicallyImpl(targetPath, obj);
}

bool Backend::writeConfigAtomicallyForTest(const QString &targetPath, const QJsonObject &obj,
                                           bool simulatePrePermFail, bool simulatePostPermFail) {
    return writeConfigAtomicallyImpl(targetPath, obj, simulatePrePermFail, simulatePostPermFail);
}

Backend::Backend(bool autoStart, QObject *parent) : QObject(parent), m_autoStart(autoStart) {
    m_startupTimestamp = QDateTime::currentMSecsSinceEpoch();
    // Contacts and chats arrive in either order; queue photos once both are here
    connect(this, &Backend::chatsChanged, this, [this]() {
        if (m_avatarScanPending) return;
        m_avatarScanPending = true;
        QTimer::singleShot(500, this, [this]() {
            m_avatarScanPending = false;
            fetchContactAvatarsForChats();
        });
    });
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        connect(cb, &QClipboard::dataChanged, this, [this]() {
            if (m_clipboardCopySeq) {
                ++(*m_clipboardCopySeq);
            }
        });
    }
    if (autoStart) {
        m_stateFilePath = QDir::homePath() + QStringLiteral("/.config/imsg/state.json");
        loadAppState();
        loadDeletedMessagesFile();
    }
    auto applyEnvBool = [](const char *envName, bool &target) {
        if (!qEnvironmentVariableIsSet(envName)) return;
        const QString v = qEnvironmentVariable(envName).trimmed().toLower();
        if (v == QStringLiteral("0") || v == QStringLiteral("false") || v == QStringLiteral("no") || v == QStringLiteral("off")) {
            target = false;
        } else if (v == QStringLiteral("1") || v == QStringLiteral("true") || v == QStringLiteral("yes") || v == QStringLiteral("on")) {
            target = true;
        }
    };
    applyEnvBool("BLUEPILL_ENABLE_NOTES", m_icloudNotesEnabled);
    applyEnvBool("BLUEPILL_ENABLE_CALENDAR", m_icloudCalendarEnabled);
    applyEnvBool("BLUEPILL_ENABLE_PHOTOS", m_icloudPhotosEnabled);
    loadCredentials();
    refreshAudioDevices();
    loadContactsFromFile(QDir::homePath() + QStringLiteral("/.config/imsg/contacts.json"));
    loadContactsFromFile(QDir::homePath() + QStringLiteral("/.cache/bluebubbles/contacts.json"));
    loadContactsFromFile(QDir::homePath() + QStringLiteral("/.local/share/app.bluebubbles.BlueBubbles/contacts.json"));
    // Pre-populate attachment paths from existing ~/.cache/imsg/attachments BEFORE loading messages cache
    indexCachedAttachmentsOnDisk();

    if (autoStart) {
        loadMessagesCache();
        m_connected = false;
        m_serverStatus = QStringLiteral("Connecting...");
        m_startupHighestRowId = qMax(m_startupHighestRowId, m_highestMessageRowId);
    }

    m_saveCacheTimer.setSingleShot(true);
    m_saveCacheTimer.setInterval(600);
    connect(&m_saveCacheTimer, &QTimer::timeout, this, [this]() {
        flushMessagesCacheNow(true);
    });

    connect(&m_contactsRefreshTimer, &QTimer::timeout, this, &Backend::fetchContactsRemote);
    m_contactsRefreshTimer.setInterval(180000); // 3 minutes

    connect(&m_pollTimer, &QTimer::timeout, this, &Backend::pollDelta);
    m_pollTimer.setInterval(2500);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &Backend::attemptReconnect);

    m_socketReconnectTimer.setSingleShot(true);
    connect(&m_socketReconnectTimer, &QTimer::timeout, this, &Backend::startSocketIoClient);

    m_icloudMutationRetryTimer.setSingleShot(true);
    connect(&m_icloudMutationRetryTimer, &QTimer::timeout, this, [this]() {
        m_icloudMutationRetryScheduled = false;
        m_icloudMutationRetryArmedDeadlineMs = 0;
        replayPendingIcloudMutations();
    });

    connect(&m_relativeTimeTimer, &QTimer::timeout, this, [this]() {
        if (m_demoMode) return;
        bool anyChanged = false;
        for (ChatEntry &c : m_chats) {
            const QString rt = formatRelativeTime(c.lastMessageDate);
            if (c.relativeTime != rt) {
                c.relativeTime = rt;
                anyChanged = true;
            }
        }
        if (anyChanged) {
            emit chatsChanged();
        }
    });
    m_relativeTimeTimer.setInterval(30000);

    m_audioPlaybackTimer.setInterval(100);
    connect(&m_audioPlaybackTimer, &QTimer::timeout, this, [this]() {
        if (!m_audioPlaying) return;
        m_audioPlaybackElapsedMs += 100;
        const int currentPosMs = m_audioSeekOffsetMs + m_audioPlaybackElapsedMs;
        if (currentPosMs >= m_audioPlaybackDurationMs) {
            m_audioProgress = 1.0;
            // Do NOT call stopAudioPlaybackProcess() on estimated duration fallback;
            // let m_audioProcess finished stop real playback, or stop timer when no external process is active.
            if (!m_audioProcess || m_audioProcess->state() == QProcess::NotRunning) {
                m_audioPlaying = false;
                m_audioPlaybackTimer.stop();
            }
        } else if (m_audioPlaybackDurationMs > 0) {
            m_audioProgress = qBound(0.0, double(currentPosMs) / double(m_audioPlaybackDurationMs), 1.0);
        }
        int remSec = qMax(0, (m_audioPlaybackDurationMs - currentPosMs) / 1000);
        m_audioDurationText = formatAudioDuration(remSec);
        emit audioPlaybackChanged();
    });

    m_voiceRecordingTimer.setInterval(1000);
    connect(&m_voiceRecordingTimer, &QTimer::timeout, this, [this]() {
        if (!m_recordingVoice) return;
        m_voiceRecordingSeconds += 1;
        m_voiceRecordingWaveform = computeAudioFileWaveformBars(
            m_currentVoiceRecordingPath, QUuid::createUuid().toString(), 28);
        emit voiceRecordingChanged();
    });


    const QString omarchyNotifPath = QDir::homePath() + QStringLiteral("/.local/state/omarchy/notifications.json");
    const QString omarchyDir = QDir::homePath() + QStringLiteral("/.local/state/omarchy");
    if (QFile::exists(omarchyNotifPath)) {
        m_omarchyWatcher.addPath(omarchyNotifPath);
    } else if (QDir(omarchyDir).exists()) {
        m_omarchyWatcher.addPath(omarchyDir);
    }
    auto onOmarchyChanged = [this]() {
        emit omarchyDndChanged();
        emit notificationsChanged();
    };
    connect(&m_omarchyWatcher, &QFileSystemWatcher::fileChanged, this, [this, onOmarchyChanged](const QString &path) {
        if (!QFile::exists(path)) {
            m_omarchyWatcher.addPath(path);
        }
        onOmarchyChanged();
    });
    connect(&m_omarchyWatcher, &QFileSystemWatcher::directoryChanged, this, [this, onOmarchyChanged](const QString &) {
        const QString notifFile = QDir::homePath() + QStringLiteral("/.local/state/omarchy/notifications.json");
        if (QFile::exists(notifFile) && !m_omarchyWatcher.files().contains(notifFile)) {
            m_omarchyWatcher.addPath(notifFile);
        }
        onOmarchyChanged();
    });

    if (autoStart && !m_serverUrl.isEmpty() && !m_password.isEmpty()) {
        refreshAll();
        m_pollTimer.start();
        m_relativeTimeTimer.start();
        m_contactsRefreshTimer.start();
    } else if (autoStart) {
        m_connected = false;
        m_serverStatus = QStringLiteral("Disconnected · Missing BlueBubbles credentials");
        emit statusChanged();
    }
}

Backend::~Backend() {
    stopAudioPlaybackProcess();
    stopVoiceRecordProcess(false);
    if (!m_currentVoiceRecordingPath.isEmpty()) {
        QFile::remove(m_currentVoiceRecordingPath);
        m_currentVoiceRecordingPath.clear();
    }
    if (!m_recordedVoicePath.isEmpty()) {
        QFile::remove(m_recordedVoicePath);
        m_recordedVoicePath.clear();
    }
    stopSocketIoClient();
    if (m_saveCacheTimer.isActive()) {
        m_saveCacheTimer.stop();
        flushMessagesCacheNow(false);
    }
}

void Backend::setStateFilePath(const QString &path) {
    if (path.trimmed().isEmpty()) return;
    m_stateFilePath = path.trimmed();
    m_notificationsEnabled = true;
    m_notificationBannerEnabled = true;
    m_notificationSoundEnabled = true;
    m_notificationMuteWhenActive = true;
    m_notificationSoundPreset = QStringLiteral("chime");
    m_icloudNotesEnabled = true;
    m_icloudCalendarEnabled = true;
    m_icloudPhotosEnabled = true;
    m_historySyncDays = 60;
    m_showTopBarStatusPill = false;
    m_showPinnedRow = true;
    m_pinnedRowFillRecent = true;
    m_hoverMessageActions = true;
    m_stayOnHomePage = false;
    m_mutedChats.clear();
    m_readWatermarks.clear();
    m_unreadOverrides.clear();
    m_localReadMutationTimeMs.clear();
    m_chats.clear();
    m_chatMessagesCache.clear();
    m_seenMessageGuids.clear();
    m_notifiedMessageGuids.clear();
    m_pendingDeliveryMessages.clear();
    m_pendingDeliveryAddedAtMs.clear();
    m_expiredPendingDeliveryGuids.clear();
    m_inFlightDeliveryCheckGuids.clear();
    m_pendingDeliveryCheckCompletedCountForTest = 0;
    m_messageObservationSeq.clear();
    m_reconcileRemovedSeqByGuid.clear();
    m_lastPendingDeliveryBatchForTest.clear();
    m_pendingDeliveryRotationCursor = 0;
    m_deliveryObservationCounter = 0;
    m_historyReconcileSeq = 0;
    m_lastNotifiedTimestamp = 0;
    m_startupHighestRowId = 0;
    m_deletedMessageGuids.clear();
    m_deletedChatTimestamps.clear();
    m_selectedChatGuid.clear();
    m_messages.clear();
    loadAppState();
    loadDeletedMessagesFile();
    loadMessagesCache();
}

void Backend::loadAppState() {
    if (m_stateFilePath.isEmpty()) return;
    QFile f(m_stateFilePath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;

    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    if (obj.contains(QStringLiteral("notificationsEnabled"))) {
        m_notificationsEnabled = obj.value(QStringLiteral("notificationsEnabled")).toBool(true);
    }
    if (obj.contains(QStringLiteral("notificationBannerEnabled"))) {
        m_notificationBannerEnabled = obj.value(QStringLiteral("notificationBannerEnabled")).toBool(true);
    }
    if (obj.contains(QStringLiteral("notificationSoundEnabled"))) {
        m_notificationSoundEnabled = obj.value(QStringLiteral("notificationSoundEnabled")).toBool(true);
    }
    if (obj.contains(QStringLiteral("notificationMuteWhenActive"))) {
        m_notificationMuteWhenActive = obj.value(QStringLiteral("notificationMuteWhenActive")).toBool(true);
    }
    if (obj.contains(QStringLiteral("notificationSoundPreset"))) {
        const QString preset = obj.value(QStringLiteral("notificationSoundPreset")).toString().trimmed().toLower();
        if (!preset.isEmpty()) {
            m_notificationSoundPreset = preset;
        }
    }
    if (obj.contains(QStringLiteral("lastNotifiedTimestamp"))) {
        m_lastNotifiedTimestamp = obj.value(QStringLiteral("lastNotifiedTimestamp")).toVariant().toLongLong();
    }
    const QJsonArray notifiedArr = obj.value(QStringLiteral("notifiedMessageGuids")).toArray();
    for (const QJsonValue &v : notifiedArr) {
        const QString g = v.toString().trimmed();
        if (!g.isEmpty()) {
            m_notifiedMessageGuids.insert(g);
            m_seenMessageGuids.insert(g);
        }
    }
    if (obj.contains(QStringLiteral("icloudNotesEnabled"))) {
        m_icloudNotesEnabled = obj.value(QStringLiteral("icloudNotesEnabled")).toBool(true);
    }
    if (obj.contains(QStringLiteral("icloudCalendarEnabled"))) {
        m_icloudCalendarEnabled = obj.value(QStringLiteral("icloudCalendarEnabled")).toBool(true);
    }
    if (obj.contains(QStringLiteral("icloudPhotosEnabled"))) {
        m_icloudPhotosEnabled = obj.value(QStringLiteral("icloudPhotosEnabled")).toBool(true);
    }
    if (obj.contains(QStringLiteral("historySyncDays"))) {
        m_historySyncDays = qBound(1, obj.value(QStringLiteral("historySyncDays")).toInt(60), 3650);
    }
    if (obj.contains(QStringLiteral("showTopBarStatusPill"))) {
        m_showTopBarStatusPill = obj.value(QStringLiteral("showTopBarStatusPill")).toBool(false);
    }
    if (obj.contains(QStringLiteral("showPinnedRow"))) {
        m_showPinnedRow = obj.value(QStringLiteral("showPinnedRow")).toBool(true);
    }
    if (obj.contains(QStringLiteral("pinnedRowFillRecent"))) {
        m_pinnedRowFillRecent = obj.value(QStringLiteral("pinnedRowFillRecent")).toBool(true);
    }
    if (obj.contains(QStringLiteral("hoverMessageActions"))) {
        m_hoverMessageActions = obj.value(QStringLiteral("hoverMessageActions")).toBool(true);
    }
    const QString savedStatus = obj.value(QStringLiteral("lastKnownServerStatus")).toString().trimmed();
    if (!savedStatus.isEmpty()) {
        m_lastKnownServerStatus = savedStatus;
    }

    m_mutedChats.clear();
    const QJsonArray mutedArr = obj.value(QStringLiteral("mutedChats")).toArray();
    for (const QJsonValue &v : mutedArr) {
        const QString g = v.toString().trimmed();
        if (!g.isEmpty()) m_mutedChats.insert(g);
    }

    m_pinnedChats.clear();
    const QJsonArray pinnedArr = obj.value(QStringLiteral("pinnedChats")).toArray();
    for (const QJsonValue &v : pinnedArr) {
        const QString g = v.toString().trimmed();
        if (!g.isEmpty() && !m_pinnedChats.contains(g)) m_pinnedChats.append(g);
    }
    m_unpinnedMacChats.clear();
    for (const QJsonValue &v : obj.value(QStringLiteral("unpinnedMacChats")).toArray()) {
        const QString g = v.toString().trimmed();
        if (!g.isEmpty()) m_unpinnedMacChats.insert(g);
    }

    m_readWatermarks.clear();
    const QJsonObject wmObj = obj.value(QStringLiteral("readWatermarks")).toObject();
    for (auto it = wmObj.begin(); it != wmObj.end(); ++it) {
        m_readWatermarks.insert(it.key(), it.value().toVariant().toLongLong());
    }

    m_unreadOverrides.clear();
    const QJsonObject uoObj = obj.value(QStringLiteral("unreadOverrides")).toObject();
    for (auto it = uoObj.begin(); it != uoObj.end(); ++it) {
        m_unreadOverrides.insert(it.key(), it.value().toVariant().toLongLong());
    }

    const QJsonArray delMsgsArr = obj.value(QStringLiteral("deletedMessageGuids")).toArray();
    for (const QJsonValue &v : delMsgsArr) {
        const QString g = v.toString().trimmed();
        if (!g.isEmpty()) m_deletedMessageGuids.insert(g);
    }

    const QJsonObject delChatsObj = obj.value(QStringLiteral("deletedChatTimestamps")).toObject();
    for (auto it = delChatsObj.begin(); it != delChatsObj.end(); ++it) {
        m_deletedChatTimestamps.insert(it.key(), it.value().toVariant().toLongLong());
    }

    auto extractIdGenerationSuffix = [](const QString &id, const QString &prefix) -> quint64 {
        if (!id.startsWith(prefix)) return 0;
        const int lastUnderscore = id.lastIndexOf(QLatin1Char('_'));
        if (lastUnderscore <= prefix.size() - 1) return 0;
        bool ok = false;
        const quint64 seq = id.mid(lastUnderscore + 1).toULongLong(&ok);
        return ok ? seq : 0;
    };

    m_icloudNotesGeneration = qMax(
        m_icloudNotesGeneration,
        obj.value(QStringLiteral("icloudNotesGeneration")).toVariant().toULongLong()
    );
    m_icloudEventsGeneration = qMax(
        m_icloudEventsGeneration,
        obj.value(QStringLiteral("icloudEventsGeneration")).toVariant().toULongLong()
    );

    m_icloudNotes.clear();
    const QJsonArray notesArr = obj.value(QStringLiteral("icloudNotes")).toArray();
    for (const QJsonValue &v : notesArr) {
        if (v.isObject()) {
            QVariantMap m = v.toObject().toVariantMap();
            const QString id = m.value(QStringLiteral("id")).toString();
            const QString title = m.value(QStringLiteral("title")).toString();
            const QString body = m.value(QStringLiteral("body")).toString();
            const QString syncSource = m.value(QStringLiteral("syncSource")).toString();
            if (syncSource == QStringLiteral("apple_notes_ro")) {
                m.insert(QStringLiteral("body"), cleanIcloudNoteBody(title, body));
            }
            m_icloudNotesGeneration = qMax(
                m_icloudNotesGeneration,
                qMax(m.value(QStringLiteral("clientMutationSeq")).toULongLong(),
                     extractIdGenerationSuffix(id, QStringLiteral("note_")))
            );
            m_icloudNotes.append(m);
        }
    }

    m_icloudEvents.clear();
    const QJsonArray eventsArr = obj.value(QStringLiteral("icloudEvents")).toArray();
    for (const QJsonValue &v : eventsArr) {
        if (v.isObject()) {
            const QVariantMap m = v.toObject().toVariantMap();
            const QString id = m.value(QStringLiteral("id")).toString();
            m_icloudEventsGeneration = qMax(
                m_icloudEventsGeneration,
                qMax(m.value(QStringLiteral("clientMutationSeq")).toULongLong(),
                     extractIdGenerationSuffix(id, QStringLiteral("evt_")))
            );
            m_icloudEvents.append(m);
        }
    }

    m_pendingIcloudNoteSaves.clear();
    m_icloudMutationPausedKeys.clear();
    const QJsonArray pendingNoteSavesArr = obj.value(QStringLiteral("pendingIcloudNoteSaves")).toArray();
    for (const QJsonValue &v : pendingNoteSavesArr) {
        if (v.isObject()) {
            QVariantMap m = v.toObject().toVariantMap();
            const QString id = m.value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                m.insert(QStringLiteral("neverDispatchedLocalCreate"), false);
                m.insert(QStringLiteral("dispatchAttempted"), true);
                m.insert(QStringLiteral("statePersisted"), true);
                m_icloudNotesGeneration = qMax(
                    m_icloudNotesGeneration,
                    qMax(m.value(QStringLiteral("clientMutationSeq")).toULongLong(),
                         extractIdGenerationSuffix(id, QStringLiteral("note_")))
                );
                if (m.value(QStringLiteral("hasServerConflict")).toBool() ||
                    m.value(QStringLiteral("conflictServerRevision")).toLongLong() >
                        m.value(QStringLiteral("baseRevision")).toLongLong()) {
                    m_icloudMutationPausedKeys.insert(QStringLiteral("note_save:") + id);
                }
                m_pendingIcloudNoteSaves.insert(id, m);
            }
        }
    }

    m_pendingIcloudNoteDeletes.clear();
    const QJsonArray pendingNoteDeletesArr = obj.value(QStringLiteral("pendingIcloudNoteDeletes")).toArray();
    for (const QJsonValue &v : pendingNoteDeletesArr) {
        const QString id = v.toString().trimmed();
        if (!id.isEmpty()) {
            m_icloudNotesGeneration = qMax(
                m_icloudNotesGeneration,
                extractIdGenerationSuffix(id, QStringLiteral("note_"))
            );
            m_pendingIcloudNoteDeletes.insert(id);
        }
    }

    m_pendingIcloudEventSaves.clear();
    const QJsonArray pendingEventSavesArr = obj.value(QStringLiteral("pendingIcloudEventSaves")).toArray();
    for (const QJsonValue &v : pendingEventSavesArr) {
        if (v.isObject()) {
            QVariantMap m = v.toObject().toVariantMap();
            const QString id = m.value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                m.insert(QStringLiteral("neverDispatchedLocalCreate"), false);
                m.insert(QStringLiteral("dispatchAttempted"), true);
                m_icloudEventsGeneration = qMax(
                    m_icloudEventsGeneration,
                    qMax(m.value(QStringLiteral("clientMutationSeq")).toULongLong(),
                         extractIdGenerationSuffix(id, QStringLiteral("evt_")))
                );
                m_pendingIcloudEventSaves.insert(id, m);
            }
        }
    }

    m_pendingIcloudEventDeletes.clear();
    const QJsonArray pendingEventDeletesArr = obj.value(QStringLiteral("pendingIcloudEventDeletes")).toArray();
    for (const QJsonValue &v : pendingEventDeletesArr) {
        const QString id = v.toString().trimmed();
        if (!id.isEmpty()) {
            m_icloudEventsGeneration = qMax(
                m_icloudEventsGeneration,
                extractIdGenerationSuffix(id, QStringLiteral("evt_"))
            );
            m_pendingIcloudEventDeletes.insert(id);
        }
    }

    emit icloudNotesChanged();
    emit icloudEventsChanged();

    emit notificationsChanged();
    emit historySyncDaysChanged();
    emit uiPreferencesChanged();
    emit chatsChanged();
    emit selectedChatChanged();
}

bool Backend::saveAppState() const {
    if (m_demoMode || m_stateFilePath.isEmpty()) return true;
    QFileInfo fi(m_stateFilePath);
    QDir().mkpath(fi.absolutePath());

    QJsonArray mutedArr;
    QStringList sortedMuted = m_mutedChats.values();
    sortedMuted.sort();
    for (const QString &g : sortedMuted) {
        mutedArr.append(g);
    }

    QJsonObject wmObj;
    for (auto it = m_readWatermarks.begin(); it != m_readWatermarks.end(); ++it) {
        wmObj.insert(it.key(), it.value());
    }

    QJsonObject uoObj;
    for (auto it = m_unreadOverrides.begin(); it != m_unreadOverrides.end(); ++it) {
        uoObj.insert(it.key(), it.value());
    }

    QJsonArray delMsgsArr;
    QStringList sortedDelMsgs = m_deletedMessageGuids.values();
    sortedDelMsgs.sort();
    for (const QString &g : sortedDelMsgs) {
        delMsgsArr.append(g);
    }

    QJsonObject delChatsObj;
    for (auto it = m_deletedChatTimestamps.begin(); it != m_deletedChatTimestamps.end(); ++it) {
        delChatsObj.insert(it.key(), it.value());
    }

    QJsonObject root;
    root.insert(QStringLiteral("notificationsEnabled"), m_notificationsEnabled);
    root.insert(QStringLiteral("notificationBannerEnabled"), m_notificationBannerEnabled);
    root.insert(QStringLiteral("notificationSoundEnabled"), m_notificationSoundEnabled);
    root.insert(QStringLiteral("notificationMuteWhenActive"), m_notificationMuteWhenActive);
    root.insert(QStringLiteral("notificationSoundPreset"), m_notificationSoundPreset);
    root.insert(QStringLiteral("lastNotifiedTimestamp"), m_lastNotifiedTimestamp);
    QJsonArray notifiedOut;
    QStringList sortedNotified = m_notifiedMessageGuids.values();
    sortedNotified.sort();
    const int startNotifiedIdx = qMax(0, int(sortedNotified.size()) - 500);
    for (int i = startNotifiedIdx; i < sortedNotified.size(); ++i) {
        notifiedOut.append(sortedNotified.at(i));
    }
    root.insert(QStringLiteral("notifiedMessageGuids"), notifiedOut);
    root.insert(QStringLiteral("icloudNotesEnabled"), m_icloudNotesEnabled);
    root.insert(QStringLiteral("icloudCalendarEnabled"), m_icloudCalendarEnabled);
    root.insert(QStringLiteral("icloudPhotosEnabled"), m_icloudPhotosEnabled);
    root.insert(QStringLiteral("historySyncDays"), m_historySyncDays);
    root.insert(QStringLiteral("showTopBarStatusPill"), m_showTopBarStatusPill);
    root.insert(QStringLiteral("showPinnedRow"), m_showPinnedRow);
    root.insert(QStringLiteral("pinnedRowFillRecent"), m_pinnedRowFillRecent);
    root.insert(QStringLiteral("hoverMessageActions"), m_hoverMessageActions);
    root.insert(QStringLiteral("lastKnownServerStatus"), m_lastKnownServerStatus);
    QJsonArray pinnedArr;
    for (const QString &g : m_pinnedChats) {
        if (!g.isEmpty()) pinnedArr.append(g);
    }
    root.insert(QStringLiteral("mutedChats"), mutedArr);
    root.insert(QStringLiteral("pinnedChats"), pinnedArr);
    QJsonArray unpinnedArr;
    for (const QString &g : m_unpinnedMacChats) unpinnedArr.append(g);
    root.insert(QStringLiteral("unpinnedMacChats"), unpinnedArr);
    root.insert(QStringLiteral("readWatermarks"), wmObj);
    root.insert(QStringLiteral("unreadOverrides"), uoObj);
    root.insert(QStringLiteral("deletedMessageGuids"), delMsgsArr);
    root.insert(QStringLiteral("deletedChatTimestamps"), delChatsObj);

    QJsonArray notesOut;
    for (const QVariant &v : m_icloudNotes) {
        notesOut.append(QJsonObject::fromVariantMap(v.toMap()));
    }
    root.insert(QStringLiteral("icloudNotes"), notesOut);

    QJsonArray eventsOut;
    for (const QVariant &v : m_icloudEvents) {
        eventsOut.append(QJsonObject::fromVariantMap(v.toMap()));
    }
    root.insert(QStringLiteral("icloudEvents"), eventsOut);

    QJsonArray pendingNoteSavesOut;
    for (auto it = m_pendingIcloudNoteSaves.begin(); it != m_pendingIcloudNoteSaves.end(); ++it) {
        QJsonObject noteObj = QJsonObject::fromVariantMap(it.value());
        noteObj.remove(QStringLiteral("neverDispatchedLocalCreate"));
        pendingNoteSavesOut.append(noteObj);
    }
    root.insert(QStringLiteral("pendingIcloudNoteSaves"), pendingNoteSavesOut);

    QJsonArray pendingNoteDeletesOut;
    for (const QString &id : m_pendingIcloudNoteDeletes) {
        pendingNoteDeletesOut.append(id);
    }
    root.insert(QStringLiteral("pendingIcloudNoteDeletes"), pendingNoteDeletesOut);

    QJsonArray pendingEventSavesOut;
    for (auto it = m_pendingIcloudEventSaves.begin(); it != m_pendingIcloudEventSaves.end(); ++it) {
        QJsonObject evtObj = QJsonObject::fromVariantMap(it.value());
        evtObj.remove(QStringLiteral("neverDispatchedLocalCreate"));
        pendingEventSavesOut.append(evtObj);
    }
    root.insert(QStringLiteral("pendingIcloudEventSaves"), pendingEventSavesOut);

    QJsonArray pendingEventDeletesOut;
    for (const QString &id : m_pendingIcloudEventDeletes) {
        pendingEventDeletesOut.append(id);
    }
    root.insert(QStringLiteral("icloudNotesGeneration"), static_cast<qint64>(m_icloudNotesGeneration));
    root.insert(QStringLiteral("icloudEventsGeneration"), static_cast<qint64>(m_icloudEventsGeneration));
    root.insert(QStringLiteral("pendingIcloudEventDeletes"), pendingEventDeletesOut);

    bool committed = false;
    QSaveFile f(m_stateFilePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (f.write(payload) == payload.size()) {
            committed = f.commit();
        }
    }
    saveDeletedMessagesFile();
    return committed;
}

QString Backend::deletedMessagesFilePath() const {
    if (!m_stateFilePath.isEmpty()) {
        const QFileInfo fi(m_stateFilePath);
        return fi.absoluteDir().filePath(QStringLiteral("deleted_messages.json"));
    }
    return QDir::homePath() + QStringLiteral("/.config/imsg/deleted_messages.json");
}

void Backend::loadDeletedMessagesFile() {
    const QString delPath = deletedMessagesFilePath();
    if (delPath.isEmpty()) return;
    QFile f(delPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        const QJsonArray arr = obj.value(QStringLiteral("deletedMessageGuids")).toArray();
        for (const QJsonValue &v : arr) {
            const QString g = v.toString().trimmed();
            if (!g.isEmpty()) m_deletedMessageGuids.insert(g);
        }
        const QJsonObject chatsObj = obj.value(QStringLiteral("deletedChatTimestamps")).toObject();
        for (auto it = chatsObj.begin(); it != chatsObj.end(); ++it) {
            m_deletedChatTimestamps.insert(it.key(), it.value().toVariant().toLongLong());
        }
    } else if (doc.isArray()) {
        for (const QJsonValue &v : doc.array()) {
            const QString g = v.toString().trimmed();
            if (!g.isEmpty()) m_deletedMessageGuids.insert(g);
        }
    }
}

void Backend::saveDeletedMessagesFile() const {
    if (m_demoMode && m_stateFilePath.isEmpty()) return;
    const QString delPath = deletedMessagesFilePath();
    if (delPath.isEmpty()) return;
    QFileInfo fi(delPath);
    QDir().mkpath(fi.absolutePath());

    QJsonArray delMsgsArr;
    QStringList sortedDelMsgs = m_deletedMessageGuids.values();
    sortedDelMsgs.sort();
    for (const QString &g : sortedDelMsgs) {
        delMsgsArr.append(g);
    }

    QJsonObject delChatsObj;
    for (auto it = m_deletedChatTimestamps.begin(); it != m_deletedChatTimestamps.end(); ++it) {
        delChatsObj.insert(it.key(), it.value());
    }

    QJsonObject root;
    root.insert(QStringLiteral("deletedMessageGuids"), delMsgsArr);
    root.insert(QStringLiteral("deletedChatTimestamps"), delChatsObj);
    root.insert(QStringLiteral("updatedAt"), QDateTime::currentMSecsSinceEpoch());

    QFile f(delPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        f.close();
    }
}

QString Backend::messagesCacheFilePath() const {
    if (m_stateFilePath.isEmpty()) return QString();
    const QFileInfo fi(m_stateFilePath);
    return fi.absoluteDir().filePath(QStringLiteral("messages_cache.json"));
}

void Backend::indexCachedAttachmentsOnDisk() {
    const QString cacheDir = QDir::homePath() + QStringLiteral("/.cache/imsg/attachments");
    QDir attDir(cacheDir);
    if (!attDir.exists()) return;

    const QFileInfoList entries = attDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : entries) {
        if (fi.size() < 100) continue;
        const QString fname = fi.fileName();
        if (fname.endsWith(QStringLiteral(".tmp"), Qt::CaseInsensitive) ||
            fname.endsWith(QStringLiteral(".conv.jpg"), Qt::CaseInsensitive)) {
            continue;
        }
        QString path = fi.absoluteFilePath();
        const QString lower = path.toLower();

        QByteArray hdr;
        QFile hdrFile(path);
        if (hdrFile.open(QIODevice::ReadOnly)) {
            hdr = hdrFile.read(16);
            hdrFile.close();
        }
        const bool isHeifMagic = (hdr.size() >= 12 && hdr.mid(4, 4) == QByteArrayLiteral("ftyp"));
        const bool isNativeQtRaster = hdr.startsWith("\xFF\xD8\xFF") || hdr.startsWith("\x89PNG") ||
                                      hdr.startsWith("GIF8") || (hdr.size() >= 12 && hdr.startsWith("RIFF") && hdr.mid(8, 4) == QByteArrayLiteral("WEBP"));
        const bool isNonNativeExt = lower.endsWith(QStringLiteral(".heic")) || lower.endsWith(QStringLiteral(".heif")) ||
                                    lower.endsWith(QStringLiteral(".heics")) || lower.endsWith(QStringLiteral(".avif")) ||
                                    lower.endsWith(QStringLiteral(".tiff")) || lower.endsWith(QStringLiteral(".tif")) ||
                                    lower.endsWith(QStringLiteral(".bmp"));

        if (isHeifMagic || isNonNativeExt) {
            if (lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg"))) {
                // Disguised HEIF file with .heic.jpeg extension — do not index raw ftyp bytes;
                // downloadAttachment -> resolveOrConvertAsync will convert it in-place to JPEG.
                continue;
            }
            const QString jpgPath = path + QStringLiteral(".jpg");
            bool validJpgSidecar = false;
            if (QFileInfo::exists(jpgPath) && QFileInfo(jpgPath).size() >= 100) {
                QFile jf(jpgPath);
                if (jf.open(QIODevice::ReadOnly)) {
                    const QByteArray jhdr = jf.read(12);
                    jf.close();
                    validJpgSidecar = !(jhdr.size() >= 12 && jhdr.mid(4, 4) == QByteArrayLiteral("ftyp"));
                }
            }
            if (validJpgSidecar) {
                path = jpgPath;
            } else if (isNativeQtRaster) {
                QFile::remove(jpgPath);
                if (QFile::copy(path, jpgPath)) {
                    path = jpgPath;
                }
            } else {
                continue;
            }
        }
        if ((lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg")) ||
             lower.endsWith(QStringLiteral(".png")) || lower.endsWith(QStringLiteral(".gif")) ||
             lower.endsWith(QStringLiteral(".webp"))) && !isNativeQtRaster) {
            continue;
        }
        if (fname.size() > 17 && fname.at(16) == QLatin1Char('_')) {
            const QString prefix16 = fname.left(16);
            insertCachedAttachmentPath(m_attachmentPaths, prefix16, path);
        } else {
            const int underIdx = fname.indexOf(QLatin1Char('_'));
            if (underIdx >= 6) {
                const QString prefix = fname.left(underIdx);
                insertCachedAttachmentPath(m_attachmentPaths, prefix, path);
            }
        }
    }
}

void Backend::loadMessagesCache() {
    indexCachedAttachmentsOnDisk();
    const QString cachePath = messagesCacheFilePath();
    if (cachePath.isEmpty()) return;
    QFile f(cachePath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;

    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QJsonArray messagesArr = root.value(QStringLiteral("messages")).toArray();
    if (!messagesArr.isEmpty()) {
        QJsonArray cleanArr;
        for (const QJsonValue &v : messagesArr) {
            const QJsonObject m = v.toObject();
            const QString g = m.value(QStringLiteral("guid")).toString().trimmed();
            if (g.isEmpty() || g.startsWith(QStringLiteral("temp-")) || g.startsWith(QStringLiteral("bp-"))) {
                continue;
            }
            const QString cacheRoom = m.value(QStringLiteral("cacheRoomnames")).toString().trimmed();
            const QString grpTitle = m.value(QStringLiteral("groupTitle")).toString().trimmed();
            const QJsonArray cArr = m.value(QStringLiteral("chats")).toArray();
            if (!cArr.isEmpty()) {
                const QString firstCg = cArr.first().toObject().value(QStringLiteral("guid")).toString();
                if (firstCg.contains(QStringLiteral(";-;")) && (!cacheRoom.isEmpty() || !grpTitle.isEmpty() || g == QStringLiteral("3A1763E9-2610-4F96-800E-7862A7B01D0A"))) {
                    continue;
                }
            }
            cleanArr.append(m);
        }
        if (!cleanArr.isEmpty()) {
            processIncomingMessages(cleanArr, true);
        }
    }
}

void Backend::saveMessagesCache() const {
    if (m_demoMode || m_stateFilePath.isEmpty()) return;
    m_saveCacheTimer.start(1500);
}

void Backend::flushMessagesCacheNow(bool async) const {
    if (m_demoMode || m_stateFilePath.isEmpty()) return;
    if (m_saveCacheTimer.isActive()) {
        m_saveCacheTimer.stop();
    }
    const QString cachePath = messagesCacheFilePath();
    if (cachePath.isEmpty()) return;

    const auto cacheSnapshot = m_chatMessagesCache;
    const auto chatsSnapshot = m_chats;
    const int historySyncDays = m_historySyncDays;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 cutoffMs = nowMs - (static_cast<qint64>(historySyncDays) * 86400LL * 1000LL);

    auto buildAndWriteFn = [cachePath, cacheSnapshot, chatsSnapshot, historySyncDays, nowMs, cutoffMs]() {
        QHash<QString, QJsonObject> chatMetaMap;
        chatMetaMap.reserve(chatsSnapshot.size());
        for (const ChatEntry &ce : chatsSnapshot) {
            if (ce.guid.isEmpty()) continue;
            QJsonObject chatMetaObj;
            chatMetaObj.insert(QStringLiteral("guid"), ce.guid);
            chatMetaObj.insert(QStringLiteral("chatIdentifier"), ce.chatIdentifier);
            chatMetaObj.insert(QStringLiteral("displayName"), ce.displayName);
            chatMetaObj.insert(QStringLiteral("style"), ce.isGroup ? 43 : 45);
            if (!ce.originalGroupId.isEmpty()) {
                chatMetaObj.insert(QStringLiteral("originalGroupId"), ce.originalGroupId);
            }
            if (!ce.groupId.isEmpty()) {
                chatMetaObj.insert(QStringLiteral("groupId"), ce.groupId);
            }
            if (!ce.siblingGuids.isEmpty()) {
                QJsonArray sibArr;
                for (const QString &sg : ce.siblingGuids) {
                    sibArr.append(sg);
                }
                chatMetaObj.insert(QStringLiteral("siblingChatGuids"), sibArr);
            }
            QJsonArray partsArr;
            for (const QString &h : ce.participantHandles) {
                QJsonObject pObj;
                pObj.insert(QStringLiteral("address"), h);
                partsArr.append(pObj);
            }
            if (!partsArr.isEmpty()) {
                chatMetaObj.insert(QStringLiteral("participants"), partsArr);
            }
            chatMetaMap.insert(ce.guid, chatMetaObj);
        }

        QJsonArray allMessages;
        for (auto chatIt = cacheSnapshot.constBegin(); chatIt != cacheSnapshot.constEnd(); ++chatIt) {
            const QString chatGuid = chatIt.key();
            const auto &msgMap = chatIt.value();
            QList<QJsonObject> chatMsgs = msgMap.values();
            std::sort(chatMsgs.begin(), chatMsgs.end(), [](const QJsonObject &a, const QJsonObject &b) {
                return a.value(QStringLiteral("dateCreated")).toVariant().toLongLong() >
                       b.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
            });

            QJsonObject chatMetaObj = chatMetaMap.value(chatGuid);
            if (chatMetaObj.isEmpty()) {
                chatMetaObj.insert(QStringLiteral("guid"), chatGuid);
            }

            int keptForChat = 0;
            for (QJsonObject m : chatMsgs) {
                const QString mg = m.value(QStringLiteral("guid")).toString().trimmed();
                if (mg.isEmpty() || mg.startsWith(QStringLiteral("temp-")) || mg.startsWith(QStringLiteral("bp-"))) {
                    continue;
                }
                const qint64 dc = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                if (keptForChat >= 30 && dc > 0 && dc < cutoffMs) {
                    continue;
                }
                if (keptForChat >= 80) {
                    break;
                }
                if (m.value(QStringLiteral("text")).toString().trimmed().isEmpty() && m.contains(QStringLiteral("attributedBody"))) {
                    const QString extracted = Backend::extractMessageText(m);
                    if (!extracted.isEmpty() && !extracted.startsWith(QStringLiteral("📷 ")) &&
                        !extracted.startsWith(QStringLiteral("🎬 ")) && !extracted.startsWith(QStringLiteral("🎙️ ")) &&
                        !extracted.startsWith(QStringLiteral("📎 "))) {
                        m.insert(QStringLiteral("text"), extracted);
                    }
                }
                m.remove(QStringLiteral("attributedBody"));
                if (!m.contains(QStringLiteral("chats")) || m.value(QStringLiteral("chats")).toArray().isEmpty()) {
                    m.insert(QStringLiteral("chats"), QJsonArray{chatMetaObj});
                } else if (!chatMetaObj.isEmpty()) {
                    QJsonArray chatsArr = m.value(QStringLiteral("chats")).toArray();
                    QJsonObject firstChat = chatsArr.first().toObject();
                    if (chatMetaObj.contains(QStringLiteral("participants"))) {
                        firstChat.insert(QStringLiteral("participants"), chatMetaObj.value(QStringLiteral("participants")));
                    }
                    if (!chatMetaObj.value(QStringLiteral("displayName")).toString().isEmpty()) {
                        firstChat.insert(QStringLiteral("displayName"), chatMetaObj.value(QStringLiteral("displayName")));
                    }
                    if (!chatMetaObj.value(QStringLiteral("chatIdentifier")).toString().isEmpty() &&
                        firstChat.value(QStringLiteral("chatIdentifier")).toString().isEmpty()) {
                        firstChat.insert(QStringLiteral("chatIdentifier"), chatMetaObj.value(QStringLiteral("chatIdentifier")));
                    }
                    if (chatMetaObj.contains(QStringLiteral("style"))) {
                        firstChat.insert(QStringLiteral("style"), chatMetaObj.value(QStringLiteral("style")));
                    }
                    if (!chatMetaObj.value(QStringLiteral("originalGroupId")).toString().isEmpty()) {
                        firstChat.insert(QStringLiteral("originalGroupId"), chatMetaObj.value(QStringLiteral("originalGroupId")));
                    }
                    if (!chatMetaObj.value(QStringLiteral("groupId")).toString().isEmpty()) {
                        firstChat.insert(QStringLiteral("groupId"), chatMetaObj.value(QStringLiteral("groupId")));
                    }
                    if (chatMetaObj.contains(QStringLiteral("siblingChatGuids"))) {
                        firstChat.insert(QStringLiteral("siblingChatGuids"), chatMetaObj.value(QStringLiteral("siblingChatGuids")));
                    }
                    chatsArr[0] = firstChat;
                    m.insert(QStringLiteral("chats"), chatsArr);
                }
                allMessages.append(m);
                ++keptForChat;
            }
        }

        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("historySyncDays"), historySyncDays);
        root.insert(QStringLiteral("savedAt"), nowMs);
        root.insert(QStringLiteral("messages"), allMessages);

        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
        QFileInfo fi(cachePath);
        QDir().mkpath(fi.absolutePath());
        const QString tmpPath = cachePath + QStringLiteral(".tmp");
        QFile f(tmpPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(payload);
            f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            f.close();
            QFile::remove(cachePath);
            QFile::rename(tmpPath, cachePath);
        }
    };

    if (async) {
        QThreadPool::globalInstance()->start(buildAndWriteFn);
    } else {
        buildAndWriteFn();
    }
}

void Backend::setHistorySyncDays(int days) {
    const int clamped = qBound(1, days, 3650);
    if (m_historySyncDays == clamped) return;
    const bool expanded = clamped > m_historySyncDays;
    m_historySyncDays = clamped;
    saveAppState();
    saveMessagesCache();
    emit historySyncDaysChanged();
    emit toastMessage(QStringLiteral("History sync window set to %1 days").arg(m_historySyncDays));
    if (expanded && !m_demoMode && !m_serverUrl.isEmpty() && !m_password.isEmpty()) {
        fetchRecentMessages(0);
    }
}

void Backend::cycleHistorySyncDays() {
    static const QList<int> presets = {7, 14, 30, 60, 90, 180, 365};
    int next = 60;
    for (int i = 0; i < presets.size(); ++i) {
        if (presets[i] > m_historySyncDays) {
            next = presets[i];
            break;
        }
        if (i == presets.size() - 1) {
            next = presets.first();
        }
    }
    setHistorySyncDays(next);
}

void Backend::setShowTopBarStatusPill(bool enabled) {
    if (m_showTopBarStatusPill == enabled) return;
    m_showTopBarStatusPill = enabled;
    saveAppState();
    emit uiPreferencesChanged();
}

void Backend::setShowPinnedRow(bool enabled) {
    if (m_showPinnedRow == enabled) return;
    m_showPinnedRow = enabled;
    saveAppState();
    emit uiPreferencesChanged();
    emit chatsChanged();
}

void Backend::setPinnedRowFillRecent(bool enabled) {
    if (m_pinnedRowFillRecent == enabled) return;
    m_pinnedRowFillRecent = enabled;
    saveAppState();
    emit uiPreferencesChanged();
    emit chatsChanged();
}

void Backend::setHoverMessageActions(bool enabled) {
    if (m_hoverMessageActions == enabled) return;
    m_hoverMessageActions = enabled;
    saveAppState();
    emit uiPreferencesChanged();
}

bool Backend::selectedChatMuted() const {
    return !m_selectedChatGuid.isEmpty() && m_mutedChats.contains(m_selectedChatGuid);
}

void Backend::setNotificationsEnabled(bool enabled) {
    if (m_notificationsEnabled == enabled) return;
    m_notificationsEnabled = enabled;
    saveAppState();
    emit notificationsChanged();
    emit toastMessage(m_notificationsEnabled
                          ? QStringLiteral("🔔 Notifications enabled")
                          : QStringLiteral("🔕 Notifications muted"));
}

void Backend::setNotificationBannerEnabled(bool enabled) {
    if (m_notificationBannerEnabled == enabled) return;
    m_notificationBannerEnabled = enabled;
    saveAppState();
    emit notificationsChanged();
    emit toastMessage(m_notificationBannerEnabled
                          ? QStringLiteral("💬 Desktop banner alerts enabled")
                          : QStringLiteral("💬 Desktop banner alerts hidden"));
}

void Backend::setNotificationSoundEnabled(bool enabled) {
    if (m_notificationSoundEnabled == enabled) return;
    m_notificationSoundEnabled = enabled;
    saveAppState();
    emit notificationsChanged();
    emit toastMessage(m_notificationSoundEnabled
                          ? QStringLiteral("🔊 Audio notification ding enabled")
                          : QStringLiteral("🔇 Audio notification ding muted"));
}

void Backend::setNotificationMuteWhenActive(bool enabled) {
    if (m_notificationMuteWhenActive == enabled) return;
    m_notificationMuteWhenActive = enabled;
    saveAppState();
    emit notificationsChanged();
}

void Backend::setNotificationSoundPreset(const QString &preset) {
    QString norm = preset.trimmed().toLower();
    static const QStringList validPresets = {
        QStringLiteral("chime"),
        QStringLiteral("crystal"),
        QStringLiteral("marimba"),
        QStringLiteral("pop"),
        QStringLiteral("glass"),
        QStringLiteral("system")
    };
    if (!validPresets.contains(norm)) {
        norm = QStringLiteral("chime");
    }
    if (m_notificationSoundPreset == norm) return;
    m_notificationSoundPreset = norm;
    ensureNotificationSoundFile(m_notificationSoundPreset);
    saveAppState();
    emit notificationsChanged();
}

QVariantList Backend::notificationSoundPresets() const {
    return QVariantList{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("chime")}, {QStringLiteral("label"), QStringLiteral("Classic Chime")}, {QStringLiteral("desc"), QStringLiteral("Warm two-tone iMessage chime")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("crystal")}, {QStringLiteral("label"), QStringLiteral("Crystal Ding")}, {QStringLiteral("desc"), QStringLiteral("Bright resonant bell ding")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("marimba")}, {QStringLiteral("label"), QStringLiteral("Tri-Tone Marimba")}, {QStringLiteral("desc"), QStringLiteral("Ascending 3-note chord")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("pop")}, {QStringLiteral("label"), QStringLiteral("Soft Pop")}, {QStringLiteral("desc"), QStringLiteral("Subtle gentle bubble pop")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("glass")}, {QStringLiteral("label"), QStringLiteral("Glass Ping")}, {QStringLiteral("desc"), QStringLiteral("Clean minimal glass ping")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("system")}, {QStringLiteral("label"), QStringLiteral("System Tone")}, {QStringLiteral("desc"), QStringLiteral("Freedesktop message alert")}}
    };
}

void Backend::toggleNotifications() {
    setNotificationsEnabled(!m_notificationsEnabled);
}

void Backend::toggleChatMute(const QString &chatGuid) {
    const QString target = chatGuid.isEmpty() ? m_selectedChatGuid : chatGuid;
    if (target.isEmpty()) return;
    if (m_mutedChats.contains(target)) {
        m_mutedChats.remove(target);
        emit toastMessage(QStringLiteral("🔔 Conversation unmuted"));
    } else {
        m_mutedChats.insert(target);
        emit toastMessage(QStringLiteral("🔕 Conversation muted"));
    }
    saveAppState();
    emit chatsChanged();
    if (target == m_selectedChatGuid) {
        emit selectedChatChanged();
    }
}

bool Backend::isChatMuted(const QString &chatGuid) const {
    return !chatGuid.isEmpty() && m_mutedChats.contains(chatGuid);
}

void Backend::togglePinChat(const QString &chatGuid) {
    const QString target = chatGuid.isEmpty() ? m_selectedChatGuid : chatGuid;
    if (target.isEmpty()) return;
    const bool fromMac = macPinnedChatGuids().contains(target);
    if (isChatPinned(target)) {
        // A Mac pin is hidden here (the Mac keeps it); a local pin is removed.
        m_pinnedChats.removeAll(target);
        if (fromMac) m_unpinnedMacChats.insert(target);
        emit toastMessage(QStringLiteral("📌 Conversation unpinned"));
    } else {
        if (fromMac) {
            // Un-hide every record for this conversation (it may have been hidden under a sibling GUID)
            for (auto it = m_unpinnedMacChats.begin(); it != m_unpinnedMacChats.end();) {
                if (*it == target || resolveCanonicalChatGuid(*it) == resolveCanonicalChatGuid(target) || isSameConversation(*it, target))
                    it = m_unpinnedMacChats.erase(it);
                else
                    ++it;
            }
        } else if (!m_pinnedChats.contains(target)) {
            m_pinnedChats.append(target);
        }
        emit toastMessage(QStringLiteral("📌 Conversation pinned"));
    }
    saveAppState();
    emit chatsChanged();
}

bool Backend::isChatPinned(const QString &chatGuid) const {
    return !chatGuid.isEmpty() && effectivePinnedChatGuids().contains(chatGuid);
}

QStringList Backend::macPinnedChatGuids() const {
    // Resolve the Mac's pins (chat GUIDs or identifiers) to chats in the list
    QStringList out;
    for (const QVariantMap &pin : m_macPins) {
        const QString pg = pin.value(QStringLiteral("guid")).toString().trimmed();
        const QString pid = normalizeHandle(pin.value(QStringLiteral("chatIdentifier")).toString());
        QString found;
        if (!pg.isEmpty()) {
            // Exact GUID first, then the canonical/merged chat; only then the identifier
            const QString canon = resolveCanonicalChatGuid(pg);
            for (const ChatEntry &c : m_chats) if (c.guid == pg) { found = c.guid; break; }
            if (found.isEmpty())
                for (const ChatEntry &c : m_chats) if (c.guid == canon || c.siblingGuids.contains(pg)) { found = c.guid; break; }
            if (found.isEmpty())
                for (const ChatEntry &c : m_chats) if (isSameConversation(c.guid, pg)) { found = c.guid; break; }
        }
        if (found.isEmpty() && !pid.isEmpty()) {
            for (const ChatEntry &c : m_chats)
                if (!c.isGroup && normalizeHandle(c.chatIdentifier) == pid) { found = c.guid; break; }
        }
        if (!found.isEmpty() && !out.contains(found)) out.append(found);
    }
    return out;
}

bool Backend::isMacPinHidden(const QString &chatGuid) const {
    if (m_unpinnedMacChats.contains(chatGuid)) return true;
    // A hide recorded under a GUID that has since merged into this chat still counts
    const QString canon = resolveCanonicalChatGuid(chatGuid);
    for (const QString &h : m_unpinnedMacChats) {
        if (resolveCanonicalChatGuid(h) == canon || isSameConversation(h, chatGuid)) return true;
    }
    return false;
}

QStringList Backend::effectivePinnedChatGuids() const {
    QStringList out;
    for (const QString &g : macPinnedChatGuids()) {
        if (!isMacPinHidden(g)) out.append(g);
    }
    for (const QString &g : m_pinnedChats) {
        if (!out.contains(g)) out.append(g);
    }
    return out;
}

void Backend::fetchMacPins() {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    const quint64 seq = ++m_macPinsRequestSeq;
    const QString relay = m_serverUrl;
    QNetworkReply *reply = m_nam.get(makeRequest(QStringLiteral("/api/v1/pinned")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, seq, relay]() {
        reply->deleteLater();
        // An older or other-relay reply must not replace a newer pin list
        if (seq != m_macPinsRequestSeq || relay != m_serverUrl) return;
        if (reply->error() != QNetworkReply::NoError) return;
        applyMacPinsResponse(reply->readAll());
    });
}

void Backend::applyMacPinsResponse(const QByteArray &body) {
    const QJsonArray arr = QJsonDocument::fromJson(body).object().value(QStringLiteral("data")).toArray();
    QList<QVariantMap> pins;
    for (const QJsonValue &v : arr) {
        const QVariantMap m = v.toObject().toVariantMap();
        // A pin may carry only a chat identifier (no GUID)
        if (!m.value(QStringLiteral("guid")).toString().trimmed().isEmpty() ||
            !normalizeHandle(m.value(QStringLiteral("chatIdentifier")).toString()).isEmpty())
            pins.append(m);
    }
    setMacPins(pins);
}

void Backend::setMacPins(const QList<QVariantMap> &pins) {
    if (pins == m_macPins) return;
    m_macPins = pins;
    emit chatsChanged();
    fetchUnresolvedPinnedChats();
}

void Backend::fetchUnresolvedPinnedChats() {
    // Pinned chats can be older than the first page of chats: load those one by one
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    if (m_chats.isEmpty()) return;  // the chat page comes first; it re-runs this
    const QStringList resolved = macPinnedChatGuids();
    for (const QVariantMap &pin : std::as_const(m_macPins)) {
        const QString explicitGuid = pin.value(QStringLiteral("guid")).toString().trimmed();
        const QString ident = pin.value(QStringLiteral("chatIdentifier")).toString().trimmed();
        const QString guid = (!explicitGuid.isEmpty() || ident.isEmpty()) ? explicitGuid : (QStringLiteral("iMessage;-;") + ident);
        if (guid.isEmpty() || m_requestedPinChatGuids.contains(guid)) continue;
        bool have = false;
        for (const QString &r : resolved) {
            if (r == guid || resolveCanonicalChatGuid(guid) == r || isSameConversation(r, guid)) { have = true; break; }
        }
        if (!have && explicitGuid.isEmpty() && !ident.isEmpty()) {
            const QString pid = normalizeHandle(ident);
            for (const ChatEntry &c : std::as_const(m_chats))
                if (!c.isGroup && normalizeHandle(c.chatIdentifier) == pid) { have = true; break; }
        }
        if (have) continue;
        m_requestedPinChatGuids.insert(guid);
        QJsonObject body{{QStringLiteral("chatGuid"), guid}, {QStringLiteral("limit"), 1},
                         {QStringLiteral("with"), QJsonArray{QStringLiteral("participants")}}};
        QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/chat/query")),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, guid]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                m_requestedPinChatGuids.remove(guid);
                return;
            }
            const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
            if (arr.isEmpty()) {
                m_requestedPinChatGuids.remove(guid);
                return;
            }
            processChatsMetadata(arr);
            emit chatsChanged();
        });
    }
}

bool Backend::omarchyDnd() const {
    const QString path = QDir::homePath() + QStringLiteral("/.local/state/omarchy/notifications.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject()) {
        return doc.object().value(QStringLiteral("dnd")).toBool(false);
    }
    return false;
}

bool Backend::areOmarchyNotificationsEnabled() const {
    if (omarchyDnd()) return false;
    return m_notificationsEnabled;
}

int Backend::computeTextPartIndex(const QJsonObject &msgObj) {
    const QJsonArray atts = msgObj.value(QStringLiteral("attachments")).toArray();
    QString realText = msgObj.value(QStringLiteral("text")).toString().trimmed();
    if (realText.isEmpty()) {
        const QJsonArray attrBody = msgObj.value(QStringLiteral("attributedBody")).toArray();
        if (!attrBody.isEmpty()) {
            realText = attrBody.first().toObject().value(QStringLiteral("string")).toString();
            realText.remove(QChar(0xFFFC));
            realText = realText.trimmed();
        }
    }
    if (!atts.isEmpty() && !realText.isEmpty()) {
        return 1;
    }
    return 0;
}

QString Backend::configFilePath() const {
    if (!m_configFilePath.trimmed().isEmpty()) {
        return m_configFilePath.trimmed();
    }
    const QString bpEnvPath = qEnvironmentVariable("BLUEPILL_CONFIG_PATH").trimmed();
    if (!bpEnvPath.isEmpty()) {
        return bpEnvPath;
    }
    const QString envPath = qEnvironmentVariable("IMSG_CONFIG_PATH").trimmed();
    if (!envPath.isEmpty()) {
        return envPath;
    }
    const QString home = QDir::homePath();
    const QString bpCfg = home + QStringLiteral("/.config/bluepill/config.json");
    const QString legacyCfg = home + QStringLiteral("/.config/imsg/config.json");
    if (!QFileInfo::exists(bpCfg) && QFileInfo::exists(legacyCfg)) {
        QDir().mkpath(QFileInfo(bpCfg).absolutePath());
        QFile::copy(legacyCfg, bpCfg);
        QFile::setPermissions(bpCfg, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    if (QFileInfo::exists(bpCfg)) {
        return bpCfg;
    }
    return legacyCfg;
}

void Backend::setConfigFilePath(const QString &path) {
    m_configFilePath = path.trimmed();
    loadCredentials();
}

void Backend::checkServerAndLoad() {
    refreshAll();
    if (!m_demoMode && !m_serverUrl.isEmpty() && !m_password.isEmpty()) {
        if (!m_pollTimer.isActive()) m_pollTimer.start();
        if (!m_relativeTimeTimer.isActive()) m_relativeTimeTimer.start();
        if (!m_contactsRefreshTimer.isActive()) m_contactsRefreshTimer.start();
    }
}

QString Backend::normalizeServerEndpointUrl(const QString &rawUrl, bool fromUserInput) {
    QString url = rawUrl.trimmed();
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    if (url.isEmpty()) return url;
    if (url.startsWith(QStringLiteral("ssh://"), Qt::CaseInsensitive)) {
        return QStringLiteral("ssh://") + url.mid(6);
    }
    const bool hasHttp = url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive);
    const bool hasHttps = url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive);
    if (!hasHttp && !hasHttps) {
        if (url.contains(QStringLiteral("://"))) return url;  // unknown scheme: leave as typed
        if (isSshStyleServerUrl(url)) {
            return QStringLiteral("ssh://") + url;
        }
        // Settings: the BluePill relay serves plain HTTP unless started with
        // --tls-cert/--tls-key, so a bare host means http://. Saved config and
        // env vars keep the historical https:// so existing setups don't move.
        url = (fromUserInput ? QStringLiteral("http://") : QStringLiteral("https://")) + url;
    }
    // Settings only: no explicit port -> the relay's default port (1235), for
    // http:// and https:// alike. Reverse proxies on 443 need ":443".
    if (!fromUserInput) return url;
    QUrl parsed(url, QUrl::StrictMode);
    if (parsed.isValid() && !parsed.host().isEmpty() && parsed.port() == -1) {
        parsed.setPort(kDefaultRelayPort);
        QString out = parsed.toString();
        while (out.endsWith(QLatin1Char('/'))) out.chop(1);
        return out;
    }
    return url;
}

// "ssh://..." or a scheme-less "user@host" (no path). http(s)://user@host is a
// URL with user info, and "host/path@x" is an HTTP base with "@" in its path.
bool Backend::isSshStyleServerUrl(const QString &url) {
    const QString trimmed = url.trimmed();
    if (trimmed.startsWith(QStringLiteral("ssh://"), Qt::CaseInsensitive)) return true;
    return !trimmed.contains(QStringLiteral("://")) && trimmed.contains(QLatin1Char('@')) &&
           !trimmed.contains(QLatin1Char('/'));
}

static const QRegularExpression &serverUrlPortRegex() {
    // [scheme://] host-or-[ipv6] :port [rest]
    static const QRegularExpression re(
        QStringLiteral(R"(^(\s*(?:[A-Za-z][A-Za-z0-9+.-]*://)?(?:[^/?#@\s]+@)?(?:\[[^\]]+\]|[^/:?#@\s]+)):(\d{1,5})(?=[/?#]|\s*$)(.*)$)"),
        QRegularExpression::DotMatchesEverythingOption);
    return re;
}

QVariantMap Backend::splitServerUrl(const QString &rawUrl, bool hideDefaultPort) {
    QVariantMap out;
    out.insert(QStringLiteral("url"), rawUrl);
    out.insert(QStringLiteral("port"), QString());
    const QString trimmed = rawUrl.trimmed();
    if (isSshStyleServerUrl(trimmed)) {
        return out;
    }
    const QRegularExpressionMatch m = serverUrlPortRegex().match(trimmed);
    if (!m.hasMatch()) return out;
    const int port = m.captured(2).toInt();
    if (port < 1 || port > 65535) return out;
    QString rest = m.captured(3);
    while (rest.endsWith(QLatin1Char('/'))) rest.chop(1);
    out.insert(QStringLiteral("url"), m.captured(1).trimmed() + rest);
    out.insert(QStringLiteral("port"), (hideDefaultPort && port == kDefaultRelayPort) ? QString() : QString::number(port));
    return out;
}

QString Backend::composeServerUrl(const QString &rawUrl, const QString &port) {
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty()) return QString();
    if (isSshStyleServerUrl(trimmed)) {
        return normalizeServerEndpointUrl(trimmed);
    }
    const QVariantMap split = splitServerUrl(trimmed);
    if (!split.value(QStringLiteral("port")).toString().isEmpty()) {
        return normalizeServerEndpointUrl(trimmed);  // inline port wins
    }
    const int p = port.trimmed().toInt();
    if (p < 1 || p > 65535) return normalizeServerEndpointUrl(trimmed);
    // Insert ":port" after the host, before any path.
    QString withScheme = trimmed;
    if (!withScheme.contains(QStringLiteral("://"))) withScheme.prepend(QStringLiteral("http://"));
    QUrl u(withScheme, QUrl::TolerantMode);
    if (!u.isValid() || u.host().isEmpty()) return normalizeServerEndpointUrl(trimmed);
    u.setPort(p);
    return normalizeServerEndpointUrl(u.toString());
}

QString Backend::webUiUrl() const {
    QString base = m_serverUrl.trimmed();
    if (base.isEmpty() || base.startsWith(QStringLiteral("ssh://"), Qt::CaseInsensitive)) {
        base = QStringLiteral("http://127.0.0.1:%1").arg(kDefaultRelayPort);
    }
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return base + QStringLiteral("/web");
}

void Backend::openWebUi() {
    const QString urlStr = webUiUrl();
    QDesktopServices::openUrl(QUrl(urlStr));
    emit toastMessage(QStringLiteral("🌐 Opening BluePill Web UI (%1)").arg(urlStr));
}

// http://host[:port] with IPv6 literals bracketed.
static QString relayHttpUrl(const QString &host, int port = -1) {
    QUrl u;
    u.setScheme(QStringLiteral("http"));
    u.setHost(host);
    if (port > 0) u.setPort(port);
    return u.toString();
}

bool Backend::looksLikeRelayResponse(int httpStatus, const QByteArray &body) {
    // bluepill-server (and BlueBubbles) answer with {"status": <code>, "message": ...};
    // an authorized ping returns data "pong". A bare status code proves nothing.
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (o.isEmpty() || o.value(QStringLiteral("status")).toInt(-1) != httpStatus ||
        !o.value(QStringLiteral("message")).isString()) {
        return false;
    }
    if (httpStatus == 401) return true;
    return httpStatus == 200 && o.value(QStringLiteral("data")).toString() == QStringLiteral("pong");
}

QVariantList Backend::parseTailscaleMacPeers(const QByteArray &statusJson, QString *backendState) {
    QVariantList out;
    const QJsonObject root = QJsonDocument::fromJson(statusJson).object();
    if (backendState) *backendState = root.value(QStringLiteral("BackendState")).toString();
    const QJsonObject peers = root.value(QStringLiteral("Peer")).toObject();
    for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) {
        const QJsonObject peer = it.value().toObject();
        if (peer.value(QStringLiteral("OS")).toString().compare(QStringLiteral("macOS"), Qt::CaseInsensitive) != 0) continue;
        if (!peer.value(QStringLiteral("Online")).toBool()) continue;

        QString dnsName = peer.value(QStringLiteral("DNSName")).toString().trimmed();
        while (dnsName.endsWith(QLatin1Char('.'))) dnsName.chop(1);
        // Prefer IPv4; keep IPv6 for IPv6-only tailnets.
        QString ip;
        QString ip6;
        for (const QJsonValue &v : peer.value(QStringLiteral("TailscaleIPs")).toArray()) {
            const QHostAddress a(v.toString());
            if (a.isNull()) continue;
            if (a.protocol() == QAbstractSocket::IPv4Protocol) { ip = a.toString(); break; }
            if (ip6.isEmpty()) ip6 = a.toString();
        }
        if (ip.isEmpty()) ip = ip6;
        const QString host = !dnsName.isEmpty() ? dnsName : ip;
        if (host.isEmpty()) continue;

        QString name = peer.value(QStringLiteral("HostName")).toString().trimmed();
        if (name.isEmpty()) name = host.section(QLatin1Char('.'), 0, 0);

        QVariantMap entry;
        entry.insert(QStringLiteral("name"), name);
        entry.insert(QStringLiteral("host"), host);
        entry.insert(QStringLiteral("ip"), ip);
        entry.insert(QStringLiteral("url"), relayHttpUrl(host, kDefaultRelayPort));
        entry.insert(QStringLiteral("baseUrl"), relayHttpUrl(host));
        entry.insert(QStringLiteral("key"), host);
        entry.insert(QStringLiteral("probeState"), QStringLiteral("pending"));
        out.append(entry);
    }
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString().compare(
                   b.toMap().value(QStringLiteral("name")).toString(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

static QSet<QString> &verifiedTailnetHostSet() {
    static QSet<QString> hosts;
    return hosts;
}

// ::ffff:a.b.c.d -> a.b.c.d, so mapped answers compare like plain IPv4.
static QHostAddress unmapIPv4(const QHostAddress &raw) {
    if (raw.protocol() == QAbstractSocket::IPv6Protocol) {
        bool isV4 = false;
        const QHostAddress v4(raw.toIPv4Address(&isV4));
        if (isV4) return v4;
    }
    return raw;
}

static bool isTailscaleAddress(const QHostAddress &raw) {
    const QHostAddress addr = unmapIPv4(raw);
    static const auto kCgnat = QHostAddress::parseSubnet(QStringLiteral("100.64.0.0/10"));
    static const auto kTsV6 = QHostAddress::parseSubnet(QStringLiteral("fd7a:115c:a1e0::/48"));
    return addr.isInSubnet(kCgnat) || addr.isInSubnet(kTsV6);
}

QSet<QString> Backend::parseTailscaleNodeIps(const QByteArray &statusJson) {
    QSet<QString> out;
    const QJsonObject root = QJsonDocument::fromJson(statusJson).object();
    auto addNode = [&out](const QJsonObject &node) {
        for (const QJsonValue &v : node.value(QStringLiteral("TailscaleIPs")).toArray()) {
            const QHostAddress a(v.toString());
            if (!a.isNull()) out.insert(unmapIPv4(a).toString());
        }
    };
    addNode(root.value(QStringLiteral("Self")).toObject());
    const QJsonObject peers = root.value(QStringLiteral("Peer")).toObject();
    for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) addNode(it.value().toObject());
    return out;
}

bool Backend::addressesAreTailnetNodes(const QList<QHostAddress> &addresses, const QSet<QString> &nodeIps) {
    if (addresses.isEmpty()) return false;
    for (const QHostAddress &raw : addresses) {
        // 100.64.0.0/10 is also carrier-grade NAT space, so the range alone isn't
        // enough: the address must belong to a node that tailscale reports.
        const QHostAddress a = unmapIPv4(raw);
        if (!isTailscaleAddress(a) || !nodeIps.contains(a.toString())) return false;
    }
    return true;
}

bool Backend::isVerifiedTailnetHost(const QString &host) {
    return verifiedTailnetHostSet().contains(host.trimmed().toLower());
}

void Backend::markVerifiedTailnetHost(const QString &host) {
    const QString h = host.trimmed().toLower();
    if (!h.isEmpty()) verifiedTailnetHostSet().insert(h);
}

QStringList Backend::hostsNeedingTailnetVerification() const {
    QStringList hosts;
    for (const QString &raw : {m_serverUrl, m_secondaryServerUrl}) {
        const QUrl u(raw.trimmed());
        if (u.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0) continue;
        const QString host = u.host().toLower();
        if (host.isEmpty() || !QHostAddress(host).isNull()) continue;  // IPs are judged directly
        if (isTrustedPasswordTransportUrl(u)) continue;                // localhost, *.ts.net, *.local, verified
        if (m_tailnetHostsChecked.contains(host) || hosts.contains(host)) continue;
        hosts.append(host);
    }
    return hosts;
}

void Backend::verifyTailnetHosts(const QStringList &hosts, std::function<void()> done) {
    QStringList todo;
    bool waitForInFlight = false;
    for (const QString &h : hosts) {
        const QString host = h.trimmed().toLower();
        if (host.isEmpty() || isVerifiedTailnetHost(host)) continue;
        if (m_tailnetHostsInFlight.contains(host)) {
            waitForInFlight = true;
            continue;
        }
        todo.append(host);
    }
    if (todo.isEmpty() && waitForInFlight) {
        // Another check for this host is running; answer when it finishes.
        if (done) m_tailnetVerifyWaiters.append(std::move(done));
        return;
    }
    auto finishWithoutCheck = [this, todo, done]() {
        for (const QString &h : todo) m_tailnetHostsChecked.insert(h);
        if (done) done();
        notifyTailnetVerifyWaiters();
    };
    if (todo.isEmpty() || qEnvironmentVariableIntValue("BLUEPILL_DISABLE_TAILSCALE_DISCOVERY") == 1) {
        finishWithoutCheck();
        return;
    }
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("tailscale"));
    if (exe.isEmpty()) {  // can't confirm tailnet membership without tailscale
        finishWithoutCheck();
        return;
    }
    for (const QString &h : todo) m_tailnetHostsInFlight.insert(h);

    auto *proc = new QProcess(this);
    auto *killTimer = new QTimer(proc);
    killTimer->setSingleShot(true);
    connect(killTimer, &QTimer::timeout, proc, [proc]() { proc->kill(); });
    auto finish = [this, todo, done](const QSet<QString> &nodeIps) {
        auto remaining = std::make_shared<int>(todo.size());
        for (const QString &host : todo) {
            QHostInfo::lookupHost(host, this, [this, host, nodeIps, remaining, done](const QHostInfo &info) {
                m_tailnetHostsInFlight.remove(host);
                m_tailnetHostsChecked.insert(host);
                if (info.error() == QHostInfo::NoError && addressesAreTailnetNodes(info.addresses(), nodeIps)) {
                    markVerifiedTailnetHost(host);
                    ++m_verifiedTailnetHostsRevision;
                    emit verifiedTailnetHostsChanged();
                }
                if (--(*remaining) == 0) {
                    if (done) done();
                    notifyTailnetVerifyWaiters();
                }
            });
        }
    };
    connect(proc, &QProcess::errorOccurred, this, [proc, finish](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        proc->deleteLater();
        finish({});
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [proc, finish](int, QProcess::ExitStatus status) {
        proc->deleteLater();
        finish(status == QProcess::NormalExit ? parseTailscaleNodeIps(proc->readAllStandardOutput()) : QSet<QString>{});
    });
    proc->start(exe, {QStringLiteral("status"), QStringLiteral("--json")});
    killTimer->start(5000);
}

void Backend::notifyTailnetVerifyWaiters() {
    if (!m_tailnetHostsInFlight.isEmpty()) return;
    const QList<std::function<void()>> waiters = std::exchange(m_tailnetVerifyWaiters, {});
    for (const auto &w : waiters) w();
    if (m_resumeAfterTailnetCheck) {
        m_resumeAfterTailnetCheck = false;
        QTimer::singleShot(0, this, [this]() {
            fetchMissingGroupParticipants();
            if (!m_selectedChatGuid.isEmpty()) loadChatMessages(m_selectedChatGuid);
        });
    }
}

bool Backend::tailnetCheckPending() const {
    return !m_tailnetHostsInFlight.isEmpty() || !hostsNeedingTailnetVerification().isEmpty();
}

void Backend::verifyTailnetHostForUrl(const QString &url) {
    const QUrl u(url.trimmed());
    if (u.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0) return;
    const QString host = u.host().toLower();
    if (host.isEmpty() || !QHostAddress(host).isNull() || isTrustedPasswordTransportUrl(u)) return;
    verifyTailnetHosts({host}, nullptr);
}

void Backend::setTailscaleScanState(const QVariantList &relays, const QString &status, bool scanning) {
    m_tailscaleRelays = relays;
    m_tailscaleStatus = status;
    m_tailscaleScanning = scanning;
    emit tailscaleRelaysChanged();
}

void Backend::probeTailscaleRelay(const QString &key, const QString &baseUrl, bool allowIpFallback) {
    QNetworkRequest req(QUrl(baseUrl + QStringLiteral("/api/v1/ping")));
    req.setTransferTimeout(3000);
    // Never follow a redirect: the peer itself must answer like a relay.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QSslConfiguration ssl = req.sslConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    req.setSslConfiguration(ssl);
    QNetworkReply *reply = m_nam.get(req);
    // Only the status code matters; never buffer more than a small body, and
    // stop as soon as a peer sends more than that.
    constexpr qint64 kProbeMaxBody = 4096;
    reply->setReadBufferSize(kProbeMaxBody);
    connect(reply, &QNetworkReply::readyRead, reply, [reply]() {
        if (reply->bytesAvailable() >= kProbeMaxBody) reply->abort();
    });
    // The transfer timeout only catches inactivity; also cap total time so a
    // slow trickle can't keep the scan running.
    QTimer::singleShot(5000, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, baseUrl, allowIpFallback]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->read(kProbeMaxBody);
        QString state;
        if (httpStatus > 0 && looksLikeRelayResponse(httpStatus, body)) state = QStringLiteral("relay");
        else if (httpStatus > 0) state = QStringLiteral("other");  // something answers, but not a relay
        else state = QStringLiteral("unreachable");

        QVariantList updated = m_tailscaleRelays;
        for (QVariant &v : updated) {
            QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("key")).toString() != key) continue;
            const QString ip = m.value(QStringLiteral("ip")).toString();
            // MagicDNS names don't resolve when the client runs with
            // --accept-dns=false; retry the peer's Tailscale IP before giving up.
            if (state == QStringLiteral("unreachable") && allowIpFallback && !ip.isEmpty() &&
                m.value(QStringLiteral("host")).toString() != ip) {
                probeTailscaleRelay(key, relayHttpUrl(ip, kDefaultRelayPort), false);
                return;  // still pending; the IP probe settles this peer
            }
            if (state == QStringLiteral("relay") && !allowIpFallback) {
                m.insert(QStringLiteral("host"), ip);
                m.insert(QStringLiteral("url"), baseUrl);
                m.insert(QStringLiteral("baseUrl"), relayHttpUrl(ip));
            }
            m.insert(QStringLiteral("probeState"), state);
            v = m;
        }
        // Relays that answered first, then the rest.
        std::stable_sort(updated.begin(), updated.end(), [](const QVariant &a, const QVariant &b) {
            const bool ar = a.toMap().value(QStringLiteral("probeState")).toString() == QStringLiteral("relay");
            const bool br = b.toMap().value(QStringLiteral("probeState")).toString() == QStringLiteral("relay");
            return ar && !br;
        });
        m_tailscalePendingProbes = qMax(0, m_tailscalePendingProbes - 1);
        const bool done = m_tailscalePendingProbes == 0;
        setTailscaleScanState(updated, done ? QStringLiteral("done") : QStringLiteral("probing"), !done);
    });
}

void Backend::discoverTailscaleRelays() {
    if (m_tailscaleScanning) return;
    // Lets the test suite (and anyone who wants it off) skip running `tailscale`
    // and probing peers when Settings opens.
    if (qEnvironmentVariableIntValue("BLUEPILL_DISABLE_TAILSCALE_DISCOVERY") == 1) return;
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("tailscale"));
    if (exe.isEmpty()) {
        setTailscaleScanState({}, QStringLiteral("not-installed"), false);
        return;
    }
    setTailscaleScanState({}, QStringLiteral("scanning"), true);

    auto *proc = new QProcess(this);
    auto *killTimer = new QTimer(proc);
    killTimer->setSingleShot(true);
    connect(killTimer, &QTimer::timeout, proc, [proc]() { proc->kill(); });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        setTailscaleScanState({}, QStringLiteral("error"), false);
        proc->deleteLater();
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc](int exitCode, QProcess::ExitStatus exitStatus) {
        proc->deleteLater();
        const QByteArray out = proc->readAllStandardOutput();
        QString backendState;
        QVariantList relays = parseTailscaleMacPeers(out, &backendState);
        if (exitStatus != QProcess::NormalExit || (exitCode != 0 && backendState.isEmpty())) {
            setTailscaleScanState({}, QStringLiteral("error"), false);
            return;
        }
        if (backendState != QStringLiteral("Running")) {
            setTailscaleScanState({}, QStringLiteral("stopped"), false);
            return;
        }
        if (relays.isEmpty()) {
            setTailscaleScanState({}, QStringLiteral("no-macs"), false);
            return;
        }
        setTailscaleScanState(relays, QStringLiteral("probing"), true);

        // Probe each Mac's relay port. The relay answers unauthenticated
        // requests with 401 JSON, which is enough to know it's listening.
        m_tailscalePendingProbes = relays.size();
        for (const QVariant &v : relays) {
            const QVariantMap m = v.toMap();
            probeTailscaleRelay(m.value(QStringLiteral("key")).toString(), m.value(QStringLiteral("url")).toString(), true);
        }
    });
    proc->start(exe, {QStringLiteral("status"), QStringLiteral("--json")});
    killTimer->start(5000);
}

bool Backend::isSameServerEndpoint(const QString &a, const QString &b) {
    const QString ta = a.trimmed(), tb = b.trimmed();
    if (ta == tb) return true;  // scheme/host case is handled below; path and user are case-sensitive
    if (ta.isEmpty() || tb.isEmpty()) return false;
    const QUrl ua(ta), ub(tb);
    if (!ua.isValid() || !ub.isValid() || ua.host().isEmpty() || ub.host().isEmpty()) return false;
    auto effectivePort = [](const QUrl &u) {
        const QString scheme = u.scheme().toLower();
        return u.port(scheme == QStringLiteral("https") ? 443 : scheme == QStringLiteral("http") ? 80 : -1);
    };
    auto path = [](const QUrl &u) {
        QString p = u.path(QUrl::FullyEncoded);  // "/a%2Fb" and "/a/b" can route differently
        while (p.endsWith(QLatin1Char('/'))) p.chop(1);
        return p;
    };
    return ua.scheme().compare(ub.scheme(), Qt::CaseInsensitive) == 0 &&
           ua.host().compare(ub.host(), Qt::CaseInsensitive) == 0 &&
           effectivePort(ua) == effectivePort(ub) && path(ua) == path(ub) &&
           ua.userInfo(QUrl::FullyEncoded) == ub.userInfo(QUrl::FullyEncoded);  // user and password
}

// A field the user didn't change keeps the saved URL verbatim, so re-saving
// Settings never moves an existing endpoint (e.g. "https://relay" -> :1235).
static QString resolveSavedOrNewServerUrl(const QString &input, const QString &saved) {
    if (!saved.isEmpty()) {
        if (Backend::isSameServerEndpoint(Backend::normalizeServerEndpointUrl(input, false), saved) ||
            Backend::isSameServerEndpoint(Backend::normalizeServerEndpointUrl(input, true), saved)) {
            return saved;
        }
    }
    return Backend::normalizeServerEndpointUrl(input, true);
}

void Backend::saveServerConfig(const QString &serverUrl, const QString &secondaryUrl, const QString &password) {
    // m_serverUrl may point at the failover after a candidate rotation; the
    // configured primary is tracked in m_lastConfiguredBaseUrl.
    const QString savedPrimary = m_lastConfiguredBaseUrl.isEmpty() ? m_serverUrl : m_lastConfiguredBaseUrl;
    const QString normPrimary = resolveSavedOrNewServerUrl(serverUrl, savedPrimary);
    const QString normSecondary = resolveSavedOrNewServerUrl(secondaryUrl, m_secondaryServerUrl);
    QString resolvedPassword = password.trimmed();
    if (resolvedPassword.isEmpty()) {
        resolvedPassword = m_password;
    }

    const QString cfgPath = configFilePath();
    const QFileInfo fi(cfgPath);
    QDir().mkpath(fi.absolutePath());

    QJsonObject cfgObj;
    QFile existingFile(cfgPath);
    if (existingFile.exists() && existingFile.open(QIODevice::ReadOnly)) {
        cfgObj = QJsonDocument::fromJson(existingFile.readAll()).object();
        existingFile.close();
    }

    cfgObj.insert(QStringLiteral("serverUrl"), normPrimary);
    cfgObj.insert(QStringLiteral("secondaryServerUrl"), normSecondary);
    cfgObj.insert(QStringLiteral("password"), resolvedPassword);
    cfgObj.insert(QStringLiteral("tlsVerified"), true);
    cfgObj.insert(QStringLiteral("authMethod"), QStringLiteral("tls-apikey"));

    if (!writeConfigAtomically(cfgPath, cfgObj)) {
        emit toastMessage(QStringLiteral("⚠ Failed to save server configuration"));
        return;
    }

    const bool endpointReplaced = !m_lastConfiguredBaseUrl.isEmpty() && !normPrimary.isEmpty() &&
                                  !isSameServerEndpoint(m_lastConfiguredBaseUrl, normPrimary);
    if (endpointReplaced) {
        const int discardedCount = m_pendingIcloudNoteSaves.size() + m_pendingIcloudNoteDeletes.size() +
                                   m_pendingIcloudEventSaves.size() + m_pendingIcloudEventDeletes.size();
        ++m_icloudNotesGeneration;
        ++m_icloudNotesSnapshotEpoch;
        ++m_icloudEventsGeneration;
        ++m_icloudEventsSnapshotEpoch;
        const auto noteReplies = std::exchange(m_inFlightIcloudNoteReplies, {});
        const auto eventReplies = std::exchange(m_inFlightIcloudEventReplies, {});
        for (QNetworkReply *reply : noteReplies) {
            if (reply) reply->abort();
        }
        for (QNetworkReply *reply : eventReplies) {
            if (reply) reply->abort();
        }
        m_pendingIcloudNoteSaves.clear();
        m_pendingIcloudNoteDeletes.clear();
        m_pendingIcloudEventSaves.clear();
        m_pendingIcloudEventDeletes.clear();
        m_icloudMutationRetryCounts.clear();
        m_icloudMutationNextRetryMs.clear();
        m_icloudMutationPausedKeys.clear();
        m_icloudMutationRetryTimer.stop();
        m_icloudMutationRetryScheduled = false;
        m_icloudMutationRetryArmedDeadlineMs = 0;
        saveAppState();
        if (discardedCount > 0) {
            emit toastMessage(QStringLiteral("Server endpoint changed; discarded %1 unsynced iCloud Hub change(s).").arg(discardedCount));
        }
    }
    m_lastConfiguredBaseUrl = normPrimary;
    for (const QString &u : {normPrimary, normSecondary}) {
        m_tailnetHostsChecked.remove(QUrl(u).host().toLower());  // re-check edited hosts
    }

    if (m_serverUrl != normPrimary) {
        bumpServerEndpointGeneration();
    }
    m_serverUrl = normPrimary;
    m_secondaryServerUrl = normSecondary;
    m_password = resolvedPassword;

    m_candidateUrls.clear();
    if (!m_serverUrl.isEmpty()) {
        m_candidateUrls.append(m_serverUrl);
    }
    if (!m_secondaryServerUrl.isEmpty() && !m_candidateUrls.contains(m_secondaryServerUrl)) {
        m_candidateUrls.append(m_secondaryServerUrl);
    }
    m_candidateUrlIndex = 0;
    m_consecutiveFailures = 0;

    emit configChanged();
    emit statusChanged();
    if (m_configFilePath.trimmed().isEmpty()) {
        const QString bpCfg = QDir::homePath() + QStringLiteral("/.config/bluepill/config.json");
        if (cfgPath != bpCfg) {
            writeConfigAtomically(bpCfg, cfgObj);
        }
    }
    emit toastMessage(QStringLiteral("✓ Saved BluePill / BlueBubbles server configuration"));

    if (!m_demoMode && !m_serverUrl.isEmpty() && !m_password.isEmpty()) {
        checkServerAndLoad();
        startSocketIoClient();
    }
}

void Backend::loadCredentials() {
    const QString prevServerUrl = m_serverUrl;
    m_serverUrl = normalizeServerEndpointUrl(qEnvironmentVariable("BLUEPILL_SERVER_URL"), false);
    if (m_serverUrl.isEmpty()) {
        m_serverUrl = normalizeServerEndpointUrl(qEnvironmentVariable("IMSG_SERVER_URL"), false);
    }
    if (m_serverUrl.isEmpty()) {
        m_serverUrl = normalizeServerEndpointUrl(qEnvironmentVariable("BLUEBUBBLES_URL"), false);
    }
    m_secondaryServerUrl = normalizeServerEndpointUrl(qEnvironmentVariable("BLUEPILL_SECONDARY_URL"), false);
    if (m_secondaryServerUrl.isEmpty()) {
        m_secondaryServerUrl = normalizeServerEndpointUrl(qEnvironmentVariable("IMSG_SECONDARY_URL"), false);
    }
    if (m_secondaryServerUrl.isEmpty()) {
        m_secondaryServerUrl = normalizeServerEndpointUrl(qEnvironmentVariable("BLUEBUBBLES_SECONDARY_URL"), false);
    }
    m_password = qEnvironmentVariable("BLUEPILL_PASSWORD").trimmed();
    if (m_password.isEmpty()) {
        m_password = qEnvironmentVariable("IMSG_PASSWORD").trimmed();
    }
    if (m_password.isEmpty()) {
        m_password = qEnvironmentVariable("BLUEBUBBLES_PASSWORD").trimmed();
    }

    const QString home = QDir::homePath();
    const QString cfgPath = configFilePath();

    {
        QFile cfgFile(cfgPath);
        if (cfgFile.exists() && cfgFile.open(QIODevice::ReadOnly)) {
            const QJsonObject obj = QJsonDocument::fromJson(cfgFile.readAll()).object();
            cfgFile.close();
            if (m_serverUrl.isEmpty()) {
                m_serverUrl = normalizeServerEndpointUrl(obj.value(QStringLiteral("serverUrl")).toString(), false);
            }
            if (m_secondaryServerUrl.isEmpty()) {
                m_secondaryServerUrl = normalizeServerEndpointUrl(obj.value(QStringLiteral("secondaryServerUrl")).toString(), false);
                if (m_secondaryServerUrl.isEmpty()) {
                    m_secondaryServerUrl = normalizeServerEndpointUrl(obj.value(QStringLiteral("failoverUrl")).toString(), false);
                }
            }
            if (m_password.isEmpty()) {
                m_password = obj.value(QStringLiteral("password")).toString().trimmed();
            }
            if (obj.contains(QStringLiteral("audioInputDevice"))) {
                m_selectedAudioInputDevice = obj.value(QStringLiteral("audioInputDevice")).toString().trimmed();
            }
            if (obj.contains(QStringLiteral("audioOutputDevice"))) {
                m_selectedAudioOutputDevice = obj.value(QStringLiteral("audioOutputDevice")).toString().trimmed();
            }
            QFile::setPermissions(cfgPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }

    // Migrate from Flutter client shared_preferences if needed (when using default config path)
    if (m_configFilePath.isEmpty() && (m_serverUrl.isEmpty() || m_password.isEmpty())) {
        QFile spFile(home + QStringLiteral("/.local/share/app.bluebubbles.BlueBubbles/shared_preferences.json"));
        if (spFile.exists() && spFile.open(QIODevice::ReadOnly)) {
            const QJsonObject obj = QJsonDocument::fromJson(spFile.readAll()).object();
            spFile.close();
            if (m_serverUrl.isEmpty()) {
                QString spUrl = obj.value(QStringLiteral("serverAddress")).toString().trimmed();
                if (spUrl.isEmpty()) {
                    spUrl = obj.value(QStringLiteral("flutter.serverAddress")).toString().trimmed();
                }
                m_serverUrl = normalizeServerEndpointUrl(spUrl, false);
            }
            if (m_password.isEmpty()) {
                m_password = obj.value(QStringLiteral("guidAuthKey")).toString().trimmed();
                if (m_password.isEmpty()) {
                    m_password = obj.value(QStringLiteral("flutter.guidAuthKey")).toString().trimmed();
                }
            }
        }
    }

    // Try reading credentials from Linux Secret Service via secret-tool if password is still empty
    if (m_configFilePath.isEmpty() && m_password.isEmpty()) {
        const QString host = QUrl(m_serverUrl).host().trimmed();
        auto lookupSecret = [](const QString &service, const QString &account) -> QString {
            if (account.isEmpty()) return QString();
            QProcess proc;
            proc.start(QStringLiteral("secret-tool"),
                       {QStringLiteral("lookup"), QStringLiteral("service"), service,
                        QStringLiteral("account"), account});
            if (proc.waitForFinished(1200) && proc.exitCode() == 0) {
                return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
            }
            return QString();
        };
        if (!host.isEmpty()) {
            m_password = lookupSecret(QStringLiteral("bluebubbles"), host);
            if (m_password.isEmpty()) {
                m_password = lookupSecret(QStringLiteral("imsg"), host);
            }
        }
        if (m_password.isEmpty()) {
            m_password = lookupSecret(QStringLiteral("imsg"), QStringLiteral("default"));
        }
    }

    m_candidateUrls.clear();
    if (!m_serverUrl.isEmpty()) {
        m_candidateUrls.append(m_serverUrl);
    }
    if (!m_secondaryServerUrl.isEmpty() && !m_candidateUrls.contains(m_secondaryServerUrl)) {
        m_candidateUrls.append(m_secondaryServerUrl);
    }
    m_candidateUrlIndex = 0;
    m_lastConfiguredBaseUrl = m_serverUrl;
    if (m_serverUrl != prevServerUrl) {
        bumpServerEndpointGeneration();
    }
    emit configChanged();
}

QString Backend::normalizeHandle(const QString &raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) return QString();
    if (trimmed.contains(QLatin1Char('@'))) {
        return trimmed.toLower();
    }
    QString digits;
    digits.reserve(trimmed.size());
    for (QChar ch : trimmed) {
        if (ch.isDigit()) digits.append(ch);
    }
    if (digits.size() > 10) {
        return digits.right(10);
    }
    return digits.isEmpty() ? trimmed.toLower() : digits;
}

void Backend::loadContactsFromFile(const QString &path) {
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    parseContactsJson(f.readAll());
}

void Backend::parseContactsJson(const QByteArray &jsonBytes) {
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        arr = doc.object().value(QStringLiteral("data")).toArray();
    }

    const QString avatarDir = QDir::homePath() + QStringLiteral("/.local/share/app.bluebubbles.BlueBubbles/contact_avatars");

    for (const QJsonValue &v : arr) {
        const QJsonObject c = v.toObject();
        QString display = c.value(QStringLiteral("displayName")).toString().trimmed();
        if (display.isEmpty()) {
            const QString first = c.value(QStringLiteral("firstName")).toString().trimmed();
            const QString last = c.value(QStringLiteral("lastName")).toString().trimmed();
            display = (first + QLatin1Char(' ') + last).trimmed();
        }
        if (display.isEmpty()) {
            display = c.value(QStringLiteral("organization")).toString().trimmed();
        }
        if (display.isEmpty()) {
            display = c.value(QStringLiteral("nickname")).toString().trimmed();
        }
        if (display.isEmpty()) continue;

        QString avatarUrl;
        const QVariant idVar = c.value(QStringLiteral("id")).toVariant();
        if (idVar.isValid() && !idVar.toString().isEmpty()) {
            const QString candidate = avatarDir + QLatin1Char('/') + idVar.toString() + QStringLiteral(".jpg");
            QFileInfo fi(candidate);
            if (fi.exists() && fi.size() > 2500) {
                avatarUrl = QUrl::fromLocalFile(candidate).toString();
            }
        }

        const QVariantMap record = c.toVariantMap();
        // bluepill-server serves AddressBook photos at /api/v1/contact/<id>/avatar
        const QString avatarApiPath = c.value(QStringLiteral("avatar")).toString().startsWith(QStringLiteral("/api/v1/contact/"))
                                          ? c.value(QStringLiteral("avatar")).toString() : QString();
        const QJsonArray phones = c.value(QStringLiteral("phoneNumbers")).toArray();
        for (const QJsonValue &pv : phones) {
            QString addr;
            if (pv.isObject()) {
                addr = pv.toObject().value(QStringLiteral("address")).toString();
            } else if (pv.isString()) {
                addr = pv.toString();
            }
            const QString norm = normalizeHandle(addr);
            if (!norm.isEmpty()) {
                m_contactNames.insert(norm, display);
                m_contactRawAddresses.insert(norm, canonicalizeAddress(addr));
                m_contactRecords.insert(norm, record);
                if (!avatarUrl.isEmpty()) m_contactAvatars.insert(norm, avatarUrl);
                else if (!avatarApiPath.isEmpty()) m_contactAvatarApiPaths.insert(norm, avatarApiPath);
            }
        }

        const QJsonArray emails = c.value(QStringLiteral("emails")).toArray();
        for (const QJsonValue &ev : emails) {
            QString addr;
            if (ev.isObject()) {
                addr = ev.toObject().value(QStringLiteral("address")).toString();
            } else if (ev.isString()) {
                addr = ev.toString();
            }
            const QString norm = normalizeHandle(addr);
            if (!norm.isEmpty()) {
                m_contactNames.insert(norm, display);
                m_contactRawAddresses.insert(norm, canonicalizeAddress(addr));
                m_contactRecords.insert(norm, record);
                if (!avatarUrl.isEmpty()) m_contactAvatars.insert(norm, avatarUrl);
                else if (!avatarApiPath.isEmpty()) m_contactAvatarApiPaths.insert(norm, avatarApiPath);
            }
        }
    }
    if (!m_chats.isEmpty()) {
        for (ChatEntry &c : m_chats) {
            rebuildChatPresentation(c);
        }
        rebuildSelectedChatMessages();
        emit chatsChanged();
        emit selectedChatChanged();
    }
    emit contactsChanged();
    fetchContactAvatarsForChats();
}

QString Backend::avatarFetchKey(const QString &apiPath) const {
    // Two relays can both serve /api/v1/contact/uid-1/avatar for different people
    return QUrl(m_serverUrl).adjusted(QUrl::StripTrailingSlash | QUrl::RemoveUserInfo).toString() + QLatin1Char('|') + apiPath;
}

QString Backend::avatarCacheFile(const QString &apiPath) const {
    return QDir::homePath() + QStringLiteral("/.cache/imsg/avatars/") +
           QString::fromLatin1(QCryptographicHash::hash(avatarFetchKey(apiPath).toUtf8(), QCryptographicHash::Sha1).toHex()) +
           QStringLiteral(".img");
}

void Backend::fetchContactAvatarsForChats() {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Only people you have a conversation with; the address book can be huge.
    for (const ChatEntry &c : std::as_const(m_chats)) {
        const QStringList handles = c.participantHandles.isEmpty() ? QStringList{c.chatIdentifier} : c.participantHandles;
        for (const QString &h : handles) {
            const QString norm = normalizeHandle(h);
            if (norm.isEmpty()) continue;
            const QString apiPath = m_contactAvatarApiPaths.value(norm);
            if (apiPath.isEmpty() || m_contactAvatarQueue.contains(apiPath)) continue;
            const QString key = avatarFetchKey(apiPath);
            if (m_contactAvatarInFlight.contains(key)) continue;
            // Installed (or confirmed 404) and fresh: nothing to do. Stale photos stay visible while they refresh.
            const auto fetched = m_contactAvatarFetchedAtMs.constFind(key);
            if (fetched != m_contactAvatarFetchedAtMs.constEnd() &&
                now - fetched.value() < kAvatarRefreshMs) continue;
            const auto failure = m_contactAvatarFailures.constFind(key);
            if (failure != m_contactAvatarFailures.constEnd() &&
                (failure->attempts >= kAvatarMaxAttempts || now < failure->retryAtMs)) continue;
            m_contactAvatarQueue.append(apiPath);
        }
    }
    pumpContactAvatarQueue();
}

void Backend::pumpContactAvatarQueue() {
    while (m_contactAvatarsInFlight < 4 && !m_contactAvatarQueue.isEmpty()) {
        const QString apiPath = m_contactAvatarQueue.takeFirst();
        const QString key = avatarFetchKey(apiPath);
        if (m_contactAvatarInFlight.contains(key)) continue;
        const QString file = avatarCacheFile(apiPath);
        const QString dir = QFileInfo(file).absolutePath();
        const QFileInfo fi(file);
        const bool haveFetched = m_contactAvatarFetchedAtMs.contains(key);
        if (!haveFetched && fi.exists() && fi.size() > 0 &&
            fi.lastModified().msecsTo(QDateTime::currentDateTime()) < kAvatarRefreshMs) {
            // Fresh copy from this relay on disk: no request
            m_contactAvatarFetchedAtMs.insert(key, fi.lastModified().toMSecsSinceEpoch());
            applyContactAvatar(apiPath, file);
            continue;
        }
        m_contactAvatarInFlight.insert(key);
        ++m_contactAvatarsInFlight;
        const quint64 genAtDispatch = m_serverEndpointGeneration;
        const QString endpointAtDispatch = m_serverUrl;
        QNetworkReply *reply = m_mediaNam.get(makeRequest(apiPath));
        connect(reply, &QNetworkReply::finished, this, [this, reply, apiPath, key, dir, file, genAtDispatch, endpointAtDispatch]() {
            reply->deleteLater();
            --m_contactAvatarsInFlight;
            m_contactAvatarInFlight.remove(key);
            if (genAtDispatch != m_serverEndpointGeneration || endpointAtDispatch != m_serverUrl) {
                pumpContactAvatarQueue();
                return;
            }
            const QByteArray bytes = reply->readAll();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            bool ok = false;
            QImage probe;
            if (reply->error() == QNetworkReply::NoError && probe.loadFromData(bytes)) {
                QDir().mkpath(dir);
                QFile f(file);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size()) {
                    f.close();
                    ok = true;
                }
            }
            if (ok) {
                m_contactAvatarFailures.remove(key);
                m_contactAvatarFetchedAtMs.insert(key, now);
                applyContactAvatar(apiPath, file);
            } else if (status == 404) {
                // No photo on the Mac: don't ask again until the normal refresh
                m_contactAvatarFailures.remove(key);
                m_contactAvatarFetchedAtMs.insert(key, now);
            } else {
                AvatarFailure &fail = m_contactAvatarFailures[key];
                ++fail.attempts;
                const qint64 backoff = m_avatarRetryBaseMs << qMin(fail.attempts - 1, 6);
                fail.retryAtMs = now + backoff;
                if (fail.attempts < kAvatarMaxAttempts) {
                    QTimer::singleShot(backoff, this, [this]() { fetchContactAvatarsForChats(); });
                }
            }
            pumpContactAvatarQueue();
        });
    }
}

void Backend::applyContactAvatar(const QString &apiPath, const QString &file) {
    const QString url = QUrl::fromLocalFile(file).toString();
    for (auto it = m_contactAvatarApiPaths.constBegin(); it != m_contactAvatarApiPaths.constEnd(); ++it) {
        if (it.value() == apiPath) m_contactAvatars.insert(it.key(), url);
    }
    // Coalesce: many photos arrive together; refresh the lists once.
    if (!m_avatarRefreshPending) {
        m_avatarRefreshPending = true;
        QTimer::singleShot(150, this, [this]() {
            m_avatarRefreshPending = false;
            for (ChatEntry &c : m_chats) rebuildChatPresentation(c);
            emit chatsChanged();
            emit selectedChatChanged();
            emit contactsChanged();
        });
    }
}

QString Backend::resolveContactName(const QString &handle) const {
    const QString norm = normalizeHandle(handle);
    if (norm.isEmpty()) return handle;
    return m_contactNames.value(norm, handle);
}

QString Backend::resolveContactAvatar(const QString &handle) const {
    const QString norm = normalizeHandle(handle);
    if (norm.isEmpty()) return QString();
    return m_contactAvatars.value(norm);
}

QString Backend::extractMessageText(const QJsonObject &msgObj) {
    QString text = msgObj.value(QStringLiteral("text")).toString();
    text.remove(QChar(0xFFFC)); // Object replacement character
    text = text.trimmed();

    if (text.isEmpty()) {
        const QJsonArray attrArr = msgObj.value(QStringLiteral("attributedBody")).toArray();
        for (const QJsonValue &v : attrArr) {
            const QJsonObject runObj = v.toObject();
            QString s = runObj.value(QStringLiteral("string")).toString();
            s.remove(QChar(0xFFFC));
            s = s.trimmed();
            if (!s.isEmpty()) {
                text = s;
                break;
            }
            const QJsonObject attrs = runObj.value(QStringLiteral("attributes")).toObject();
            QString link = attrs.value(QStringLiteral("link")).toString();
            if (link.isEmpty()) link = attrs.value(QStringLiteral("NSLink")).toString();
            if (link.isEmpty()) link = attrs.value(QStringLiteral("__kIMLinkAttributeName")).toString();
            if (!link.isEmpty()) {
                text = link.trimmed();
                break;
            }
        }
    }

    if (text.isEmpty() && msgObj.contains(QStringLiteral("url"))) {
        text = msgObj.value(QStringLiteral("url")).toString().trimmed();
    }

    const QString subject = msgObj.value(QStringLiteral("subject")).toString().trimmed();
    if (!subject.isEmpty()) {
        text = text.isEmpty() ? subject : (subject + QStringLiteral("\n") + text);
    }

    if (text.isEmpty()) {
        const QJsonArray atts = deduplicateMessageAttachments(msgObj.value(QStringLiteral("attachments")).toArray());
        if (!atts.isEmpty()) {
            int imageCount = 0;
            QJsonObject firstValid;
            for (const QJsonValue &av : atts) {
                const QJsonObject aObj = av.toObject();
                const QString mime = aObj.value(QStringLiteral("mimeType")).toString();
                const QString name = aObj.value(QStringLiteral("transferName")).toString();
                if (name.endsWith(QStringLiteral(".pluginPayloadAttachment"), Qt::CaseInsensitive)) {
                    continue;
                }
                if (firstValid.isEmpty()) {
                    firstValid = aObj;
                }
                if (isImageMimeOrExt(mime, name)) {
                    ++imageCount;
                }
            }
            if (imageCount > 1) {
                return QStringLiteral("📷 %1 Photos").arg(imageCount);
            } else if (imageCount == 1) {
                return QStringLiteral("📷 Photo");
            }
            const QJsonObject a0 = firstValid.isEmpty() ? atts.first().toObject() : firstValid;
            const QString mime = a0.value(QStringLiteral("mimeType")).toString();
            const QString name = a0.value(QStringLiteral("transferName")).toString();
            if (mime.startsWith(QStringLiteral("video/"))) {
                return QStringLiteral("🎬 Video");
            } else if (mime.startsWith(QStringLiteral("audio/"))) {
                return QStringLiteral("🎙️ Audio Message");
            } else if (!name.isEmpty()) {
                return QStringLiteral("📎 ") + name;
            }
            return QStringLiteral("📎 Attachment");
        }
    }
    return text;
}

QString Backend::formatExpressiveEffectLabel(const QString &styleId) {
    const QString s = styleId.trimmed();
    if (s.isEmpty()) return QString();
    if (s.endsWith(QStringLiteral(".impact"), Qt::CaseInsensitive)) return QStringLiteral("sent with slam");
    if (s.endsWith(QStringLiteral(".loud"), Qt::CaseInsensitive)) return QStringLiteral("sent with loud");
    if (s.endsWith(QStringLiteral(".gentle"), Qt::CaseInsensitive)) return QStringLiteral("sent with gentle");
    if (s.endsWith(QStringLiteral(".invisibleink"), Qt::CaseInsensitive)) return QStringLiteral("sent with invisible ink");
    if (s.contains(QStringLiteral("CKConfettiEffect"), Qt::CaseInsensitive)) return QStringLiteral("sent with confetti");
    if (s.contains(QStringLiteral("CKLasersEffect"), Qt::CaseInsensitive)) return QStringLiteral("sent with lasers");
    if (s.contains(QStringLiteral("CKFireworksEffect"), Qt::CaseInsensitive)) return QStringLiteral("sent with fireworks");
    if (s.contains(QStringLiteral("CKSparklesEffect"), Qt::CaseInsensitive)) return QStringLiteral("sent with celebration");
    if (s.contains(QStringLiteral("CKEchoEffect"), Qt::CaseInsensitive)) return QStringLiteral("sent with echo");
    if (s.contains(QStringLiteral("CKSpotlightEffect"), Qt::CaseInsensitive)) return QStringLiteral("sent with spotlight");
    if (s.contains(QStringLiteral("CKHappyBirthdayEffect"), Qt::CaseInsensitive)) return QStringLiteral("sent with balloons");
    if (s.contains(QStringLiteral("CKHeartEffect"), Qt::CaseInsensitive)) return QStringLiteral("sent with love");
    return QString();
}

static const QRegularExpression &sharedMessageUrlRegex() {
    static const QRegularExpression regex(
        QStringLiteral(R"((?:https?://[^\s<>"']+)|(?<![/@a-zA-Z0-9._-])(?:www\.(?:[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,24}(?:/[^\s<>"']*)?)|(?<![/@a-zA-Z0-9._-])(?:(?:[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,24}/[^\s<>"']+))"),
        QRegularExpression::CaseInsensitiveOption);
    return regex;
}

static QString normalizeExtractedHrefUrl(const QString &rawUrl) {
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty()) return QString();
    if (trimmed.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
        trimmed.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) ||
        trimmed.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive) ||
        trimmed.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
        return trimmed;
    }
    return QStringLiteral("https://") + trimmed;
}

QString Backend::extractFirstUrl(const QString &text) {
    if (text.isEmpty()) return QString();
    const QRegularExpressionMatch match = sharedMessageUrlRegex().match(text);
    if (!match.hasMatch()) return QString();

    return normalizeExtractedHrefUrl(stripTrailingUrlPunctuation(match.captured(0)));
}

QString Backend::stripTrailingUrlPunctuation(QString url) {
    while (!url.isEmpty()) {
        const QChar last = url.back();
        if (last == QLatin1Char('.') || last == QLatin1Char(',') || last == QLatin1Char(')') ||
            last == QLatin1Char(']') || last == QLatin1Char('?') || last == QLatin1Char('!') ||
            last == QLatin1Char(';') || last == QLatin1Char(':') || last == QLatin1Char('"') ||
            last == QLatin1Char('\'') || last == QLatin1Char('>')) {
            url.chop(1);
        } else {
            break;
        }
    }
    return url;
}

bool Backend::isYouTubeUrl(const QString &url) {
    if (url.isEmpty()) return false;
    const QUrl u(normalizeExtractedHrefUrl(url));
    const QString host = u.host().toLower();
    return host == QStringLiteral("youtube.com") || host == QStringLiteral("www.youtube.com") ||
           host == QStringLiteral("m.youtube.com") || host == QStringLiteral("youtu.be");
}

QString Backend::formatMessageTextWithLinks(const QString &raw) {
    if (raw.isEmpty()) return QString();
    auto escapeWithLineBreaks = [](const QString &segment) -> QString {
        QString escaped = segment.toHtmlEscaped();
        escaped.replace(QStringLiteral("\r\n"), QStringLiteral("<br/>"));
        escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
        return escaped;
    };

    QString result;
    int lastPos = 0;
    auto it = sharedMessageUrlRegex().globalMatch(raw);
    while (it.hasNext()) {
        auto m = it.next();
        const int matchStart = m.capturedStart();
        const int matchLength = m.capturedLength();

        const QString leading = raw.mid(lastPos, matchStart - lastPos);
        result.append(escapeWithLineBreaks(leading));

        QString url = m.captured(0);
        QString trailingPunct;
        while (!url.isEmpty()) {
            const QChar last = url.back();
            if (last == QLatin1Char('.') || last == QLatin1Char(',') || last == QLatin1Char(')') ||
                last == QLatin1Char(']') || last == QLatin1Char('?') || last == QLatin1Char('!') ||
                last == QLatin1Char(';') || last == QLatin1Char(':') || last == QLatin1Char('"') ||
                last == QLatin1Char('\'') || last == QLatin1Char('>')) {
                trailingPunct.prepend(last);
                url.chop(1);
            } else {
                break;
            }
        }

        const QString hrefUrl = normalizeExtractedHrefUrl(url);
        result.append(QStringLiteral("<a href=\"%1\" style=\"color: #64B5F6; text-decoration: underline;\">%2</a>")
                          .arg(hrefUrl.toHtmlEscaped(), url.toHtmlEscaped()));
        result.append(escapeWithLineBreaks(trailingPunct));
        lastPos = matchStart + matchLength;
    }
    if (lastPos < raw.length()) {
        result.append(escapeWithLineBreaks(raw.mid(lastPos)));
    }
    return result;
}

QJsonArray Backend::deduplicateMessageAttachments(const QJsonArray &atts) {
    if (atts.isEmpty()) return atts;

    struct AttCandidate {
        QJsonObject obj;
        QString guid;
        QString origGuid;
        QString nameLower;
        int transferState = -1;
        int atIndex = 999999;
        int rawOrder = 0;
    };

    static const QRegularExpression atIdxRe(QStringLiteral(R"(^at_(\d+)_)"), QRegularExpression::CaseInsensitiveOption);
    QList<AttCandidate> list;
    list.reserve(atts.size());

    for (int i = 0; i < atts.size(); ++i) {
        const QJsonObject aObj = atts.at(i).toObject();
        const QString tName = aObj.value(QStringLiteral("transferName")).toString();
        if (tName.endsWith(QStringLiteral(".pluginPayloadAttachment"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString aGuid = aObj.value(QStringLiteral("guid")).toString();
        if (aGuid.isEmpty() && tName.isEmpty()) {
            continue;
        }
        AttCandidate c;
        c.obj = aObj;
        c.guid = aGuid;
        c.origGuid = aObj.value(QStringLiteral("originalGuid")).toString();
        c.nameLower = tName.trimmed().toLower();
        c.transferState = aObj.contains(QStringLiteral("transferState"))
                              ? aObj.value(QStringLiteral("transferState")).toInt(-1)
                              : -1;
        c.rawOrder = i;
        const QRegularExpressionMatch m = atIdxRe.match(aGuid);
        if (m.hasMatch()) {
            c.atIndex = m.captured(1).toInt();
        } else {
            c.atIndex = i;
        }
        list.append(c);
    }

    std::stable_sort(list.begin(), list.end(), [](const AttCandidate &a, const AttCandidate &b) {
        if (a.atIndex != b.atIndex) return a.atIndex < b.atIndex;
        return a.rawOrder < b.rawOrder;
    });

    QList<AttCandidate> deduped;
    QHash<QString, int> nameToIdx;
    for (const AttCandidate &c : list) {
        if (!c.nameLower.isEmpty() && nameToIdx.contains(c.nameLower)) {
            const int existingIdx = nameToIdx.value(c.nameLower);
            const AttCandidate &existing = deduped[existingIdx];
            const bool existingReady = (existing.transferState == 5) ||
                                       (!existing.origGuid.isEmpty() && existing.origGuid != existing.guid);
            const bool currentReady = (c.transferState == 5) ||
                                      (!c.origGuid.isEmpty() && c.origGuid != c.guid);
            if (!existingReady && currentReady) {
                deduped[existingIdx] = c;
            }
            continue;
        }
        if (!c.nameLower.isEmpty()) {
            nameToIdx.insert(c.nameLower, deduped.size());
        }
        deduped.append(c);
    }

    QJsonArray out;
    for (const AttCandidate &c : deduped) {
        out.append(c.obj);
    }
    return out;
}

bool Backend::isImageMimeOrExt(const QString &mime, const QString &fileName) {
    const QString lowerMime = mime.toLower();
    const QString lowerName = fileName.toLower();
    return lowerMime.startsWith(QStringLiteral("image/"))
        || lowerName.endsWith(QStringLiteral(".jpg"))
        || lowerName.endsWith(QStringLiteral(".jpeg"))
        || lowerName.endsWith(QStringLiteral(".png"))
        || lowerName.endsWith(QStringLiteral(".gif"))
        || lowerName.endsWith(QStringLiteral(".webp"))
        || lowerName.endsWith(QStringLiteral(".heic"))
        || lowerName.endsWith(QStringLiteral(".heif"))
        || lowerName.endsWith(QStringLiteral(".heics"))
        || lowerName.endsWith(QStringLiteral(".avif"))
        || lowerName.endsWith(QStringLiteral(".tiff"))
        || lowerName.endsWith(QStringLiteral(".tif"))
        || lowerName.endsWith(QStringLiteral(".bmp"));
}

bool Backend::isAnimatedImageAttachment(const QString &mimeType, const QString &fileName) {
    const QString mime = mimeType.trimmed().toLower();
    if (mime == QStringLiteral("image/gif") || mime == QStringLiteral("image/webp")) return true;
    QString name = fileName.trimmed().toLower();
    const int q = name.indexOf(QLatin1Char('?'));
    if (q >= 0) name.truncate(q);
    return name.endsWith(QStringLiteral(".gif")) || name.endsWith(QStringLiteral(".webp"));
}

namespace {
template <size_t N>
bool inEmojiRanges(const EmojiRange (&ranges)[N], char32_t cp) {
    const EmojiRange *it = std::upper_bound(std::begin(ranges), std::end(ranges), cp,
                                            [](char32_t v, const EmojiRange &r) { return v < r.lo; });
    return it != std::begin(ranges) && cp <= (it - 1)->hi;
}
// Emoji shown as emoji on their own (Unicode Emoji_Presentation).
bool isStrongEmojiCodePoint(char32_t cp) { return inEmojiRanges(kEmojiPresentationRanges, cp); }
// Emoji that are text by default and only count with U+FE0F.
bool isTextDefaultEmojiCodePoint(char32_t cp) { return inEmojiRanges(kTextDefaultEmojiRanges, cp); }
bool isKeycapBase(char32_t cp) {
    return (cp >= U'0' && cp <= U'9') || cp == U'#' || cp == U'*';
}
bool isEmojiJoinerOrModifier(char32_t cp) {
    return cp == 0x200D || cp == 0xFE0F || cp == 0x20E3 || (cp >= 0xE0020 && cp <= 0xE007F);
}
}  // namespace

int Backend::jumboEmojiCount(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return 0;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, trimmed);
    int count = 0;
    qsizetype start = 0;
    while (start < trimmed.size()) {
        qsizetype end = finder.toNextBoundary();
        if (end < 0) end = trimmed.size();
        const QString cluster = trimmed.mid(start, end - start);
        start = end;
        if (cluster.trimmed().isEmpty()) continue;  // spaces between emoji are fine

        bool strong = false, weak = false, keycapBase = false, hasVs16 = false, hasKeycap = false;
        for (const char32_t cp : cluster.toUcs4()) {
            if (cp == 0xFE0F) hasVs16 = true;
            if (cp == 0x20E3) hasKeycap = true;
            if (isStrongEmojiCodePoint(cp)) strong = true;
            else if (isTextDefaultEmojiCodePoint(cp)) weak = true;
            else if (isKeycapBase(cp)) keycapBase = true;
            else if (cp == 0xFE0E) return 0;                  // explicit text presentation
            else if (!isEmojiJoinerOrModifier(cp)) return 0;  // letters, punctuation...
        }
        const bool isEmoji = keycapBase ? (hasKeycap && !strong && !weak) : (strong || (weak && hasVs16));
        if (!isEmoji) return 0;
        if (++count > 3) return 0;  // iOS only enlarges up to three
    }
    return count;
}

bool Backend::isMessageDeletedOrRetracted(const QJsonObject &m) {
    if (m.value(QStringLiteral("isDeleted")).toBool(false)) return true;
    if (m.value(QStringLiteral("dateDeleted")).toVariant().toLongLong() > 0) return true;
    if (m.value(QStringLiteral("dateRetracted")).toVariant().toLongLong() > 0) return true;
    const qint64 dateEdited = m.value(QStringLiteral("dateEdited")).toVariant().toLongLong();
    if (dateEdited > 0) {
        const QString txt = extractMessageText(m);
        const QJsonArray atts = m.value(QStringLiteral("attachments")).toArray();
        if (txt.isEmpty() && atts.isEmpty()) {
            return true;
        }
    }
    return false;
}

QString Backend::normalizeAssociatedGuid(const QString &assocGuid) {
    QString g = assocGuid.trimmed();
    if (g.isEmpty()) return g;
    const int slashIdx = g.indexOf(QLatin1Char('/'));
    if (slashIdx >= 0) {
        g = g.mid(slashIdx + 1);
    } else if (g.startsWith(QStringLiteral("bp:"))) {
        g = g.mid(3);
    }
    return g;
}

QString Backend::mapReactionEmoji(const QString &typeStr, const QString &text, bool *isRemoval) {
    if (isRemoval) *isRemoval = false;
    const QString t = typeStr.trimmed().toLower();
    if (t.isEmpty() || t == QStringLiteral("0")) {
        const QString trText = text.trimmed();
        if (trText.startsWith(QStringLiteral("Loved “")) || trText.startsWith(QStringLiteral("Loved \"")) ||
            trText == QStringLiteral("Loved an image") || trText == QStringLiteral("Loved an attachment")) {
            return QStringLiteral("❤️");
        }
        if (trText.startsWith(QStringLiteral("Liked “")) || trText.startsWith(QStringLiteral("Liked \"")) ||
            trText == QStringLiteral("Liked an image") || trText == QStringLiteral("Liked an attachment")) {
            return QStringLiteral("👍");
        }
        if (trText.startsWith(QStringLiteral("Disliked “")) || trText.startsWith(QStringLiteral("Disliked \""))) {
            return QStringLiteral("👎");
        }
        if (trText.startsWith(QStringLiteral("Laughed at “")) || trText.startsWith(QStringLiteral("Laughed at \""))) {
            return QStringLiteral("😂");
        }
        if (trText.startsWith(QStringLiteral("Emphasized “")) || trText.startsWith(QStringLiteral("Emphasized \""))) {
            return QStringLiteral("‼️");
        }
        if (trText.startsWith(QStringLiteral("Questioned “")) || trText.startsWith(QStringLiteral("Questioned \""))) {
            return QStringLiteral("❓");
        }
        static const QRegularExpression reFallback(QStringLiteral(R"(Reacted\s+(\S+)\s+to)"));
        auto m = reFallback.match(trText);
        if (m.hasMatch()) return m.captured(1);
        return QString();
    }

    if (t.startsWith(QLatin1Char('-')) || (t.size() == 4 && t.startsWith(QLatin1Char('3')))) {
        if (isRemoval) *isRemoval = true;
    }

    if (t == QStringLiteral("2000") || t == QStringLiteral("3000") || t == QStringLiteral("love") || t == QStringLiteral("-love"))
        return QStringLiteral("❤️");
    if (t == QStringLiteral("2001") || t == QStringLiteral("3001") || t == QStringLiteral("like") || t == QStringLiteral("-like"))
        return QStringLiteral("👍");
    if (t == QStringLiteral("2002") || t == QStringLiteral("3002") || t == QStringLiteral("dislike") || t == QStringLiteral("-dislike"))
        return QStringLiteral("👎");
    if (t == QStringLiteral("2003") || t == QStringLiteral("3003") || t == QStringLiteral("laugh") || t == QStringLiteral("-laugh"))
        return QStringLiteral("😂");
    if (t == QStringLiteral("2004") || t == QStringLiteral("3004") || t == QStringLiteral("emphasize") || t == QStringLiteral("-emphasize"))
        return QStringLiteral("‼️");
    if (t == QStringLiteral("2005") || t == QStringLiteral("3005") || t == QStringLiteral("question") || t == QStringLiteral("-question"))
        return QStringLiteral("❓");
    if (t == QStringLiteral("2006") || t == QStringLiteral("3006")) {
        // iOS 18 / macOS Sequoia/Tahoe / RCS custom emoji reaction: "Reacted 🤍 to “...”" or "Removed 🤍 from “...”"
        QRegularExpression re(QStringLiteral(R"((?:Reacted|Removed)\s+(\S+)\s+(?:to|from))"));
        auto m = re.match(text);
        if (m.hasMatch()) return m.captured(1);
        const QString trText = text.trimmed();
        if (!trText.isEmpty() && jumboEmojiCount(trText) == 1) return trText;
        return QStringLiteral("✨");
    }
    const QString rawType = typeStr.trimmed();
    const QString candidateEmoji = rawType.startsWith(QLatin1Char('-')) ? rawType.mid(1).trimmed() : rawType;
    if (!candidateEmoji.isEmpty() && jumboEmojiCount(candidateEmoji) == 1) {
        return candidateEmoji;
    }
    return QString();
}

QString Backend::formatRelativeTime(qint64 timestampMs, qint64 nowMs) {
    if (timestampMs <= 0) return QString();
    if (nowMs <= 0) nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 diffSec = (nowMs - timestampMs) / 1000;
    if (diffSec < 45) return QStringLiteral("now");
    if (diffSec < 3600) return QStringLiteral("%1m").arg(diffSec / 60);
    if (diffSec < 86400) return QStringLiteral("%1h").arg(diffSec / 3600);

    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(timestampMs);
    const QDateTime nowDt = QDateTime::fromMSecsSinceEpoch(nowMs);
    const qint64 days = dt.date().daysTo(nowDt.date());
    if (days == 1) return QStringLiteral("Yesterday");
    if (days < 7) return dt.toString(QStringLiteral("ddd"));
    return dt.toString(QStringLiteral("MMM d"));
}

QString Backend::computeInitials(const QString &title) {
    const QString t = title.trimmed();
    if (t.isEmpty()) return QStringLiteral("?");
    if (t.startsWith(QLatin1Char('+')) || t.front().isDigit()) {
        return QStringLiteral("#");
    }
    const QStringList parts = t.split(QRegularExpression(QStringLiteral(R"([\s,&]+)")), Qt::SkipEmptyParts);
    if (parts.size() >= 2 && parts[0].front().isLetter() && parts[1].front().isLetter()) {
        return (QString(parts[0].front()) + QString(parts[1].front())).toUpper();
    }
    if (parts.size() >= 1 && parts[0].front().isLetter()) {
        return parts[0].left(2).toUpper();
    }
    return QStringLiteral("#");
}

QString Backend::computeAvatarColor(const QString &seed) {
    static const QStringList palette = {
        QStringLiteral("#3d59a1"),
        QStringLiteral("#2ac3de"),
        QStringLiteral("#7dcfff"),
        QStringLiteral("#bb9af7"),
        QStringLiteral("#9ece6a"),
        QStringLiteral("#e0af68"),
        QStringLiteral("#f7768e"),
        QStringLiteral("#ff9e64")
    };
    uint h = qHash(seed);
    return palette.at(h % palette.size());
}

bool Backend::isTrustedPasswordTransportUrl(const QUrl &url) {
    if (!url.isValid() || url.isEmpty()) return false;
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("https") || scheme == QStringLiteral("wss")) {
        return true;
    }
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("ws")) {
        QString host = url.host().trimmed().toLower();
        if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']'))) {
            host = host.mid(1, host.size() - 2);
        }
        if (host == QStringLiteral("localhost") ||
            host == QStringLiteral("127.0.0.1") ||
            host == QStringLiteral("::1")) {
            return true;
        }
        QHostAddress addr;
        if (addr.setAddress(host) && addr.isLoopback()) {
            return true;
        }
        // Tailscale: WireGuard already encrypts the tunnel end to end.
        if (host.endsWith(QStringLiteral(".ts.net"))) return true;
        // A custom-domain name confirmed to resolve only to tailnet nodes.
        if (isVerifiedTailnetHost(host)) return true;
        // Private networks (bluepill-server serves plain HTTP on :1235 by
        // default): RFC 1918, link-local, IPv6 ULA, mDNS names. Public
        // hosts still need https:// for the password.
        if (host.endsWith(QStringLiteral(".local"))) return true;
        if (!addr.isNull()) {
            if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
                bool isV4 = false;
                const QHostAddress v4(addr.toIPv4Address(&isV4));
                if (isV4) addr = v4;
            }
            static const QList<QPair<QHostAddress, int>> kTrustedHttpSubnets = {
                QHostAddress::parseSubnet(QStringLiteral("100.64.0.0/10")),        // Tailscale CGNAT
                QHostAddress::parseSubnet(QStringLiteral("fd7a:115c:a1e0::/48")),  // Tailscale IPv6
                QHostAddress::parseSubnet(QStringLiteral("10.0.0.0/8")),
                QHostAddress::parseSubnet(QStringLiteral("172.16.0.0/12")),
                QHostAddress::parseSubnet(QStringLiteral("192.168.0.0/16")),
                QHostAddress::parseSubnet(QStringLiteral("169.254.0.0/16")),
                QHostAddress::parseSubnet(QStringLiteral("fc00::/7")),
                QHostAddress::parseSubnet(QStringLiteral("fe80::/10")),
            };
            for (const auto &subnet : kTrustedHttpSubnets) {
                if (addr.isInSubnet(subnet)) return true;
            }
        }
    }
    return false;
}

bool Backend::isPasswordTransportAllowed(const QString &url) {
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("ssh://"), Qt::CaseInsensitive)) return true;
    return isTrustedPasswordTransportUrl(QUrl(trimmed));
}

QUrl Backend::buildApiUrl(const QString &apiPath, const QList<QPair<QString, QString>> &queryItems) const {
    QUrl url(m_serverUrl + apiPath);
    if (!isTrustedPasswordTransportUrl(url)) {
        return QUrl();
    }
    QUrlQuery q(url);
    if (!m_password.isEmpty() && !q.hasQueryItem(QStringLiteral("password"))) {
        q.addQueryItem(QStringLiteral("password"), m_password);
    }
    for (const auto &pair : queryItems) {
        q.addQueryItem(pair.first, pair.second);
    }
    url.setQuery(q);
    return url;
}

QNetworkRequest Backend::makeRequest(const QString &pathAndQuery) const {
    QUrl url(m_serverUrl + pathAndQuery);
    if (!isTrustedPasswordTransportUrl(url)) {
        qWarning() << "makeRequest: refusing untrusted/cleartext password transport URL:"
                   << url.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery);
        return QNetworkRequest(QUrl());
    }
    QUrlQuery q(url);
    if (!q.hasQueryItem(QStringLiteral("password"))) {
        q.addQueryItem(QStringLiteral("password"), m_password);
    }
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(20000);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setRawHeader("Connection", "keep-alive");

    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    req.setSslConfiguration(ssl);
    return req;
}

void Backend::attemptReconnect() {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    if (m_lastKnownServerStatus.isEmpty()) {
        fetchServerInfo();
    } else {
        pollDelta();
    }
}

void Backend::handleNetworkFailure(const QString &context) {
    Q_UNUSED(context);
    m_consecutiveFailures++;

    const bool wasConnected = m_connected;
    m_connected = false;

    // Clear connection cache & rotate endpoint after 4 consecutive failures
    if (m_consecutiveFailures >= 4) {
        stopSocketIoClient();
        m_nam.clearConnectionCache();
        m_sendNam.clearConnectionCache();
        m_mediaNam.clearConnectionCache();
        m_socketNam.clearConnectionCache();
        if (m_candidateUrls.size() > 1) {
            m_candidateUrlIndex = (m_candidateUrlIndex + 1) % m_candidateUrls.size();
            const QString nextUrl = m_candidateUrls[m_candidateUrlIndex];
            if (nextUrl != m_serverUrl) {
                m_serverUrl = nextUrl;
                bumpServerEndpointGeneration();
            }
        }
    }

    const QString nextStatus = m_serverUrl.isEmpty()
                                   ? QStringLiteral("Disconnected")
                                   : QStringLiteral("Disconnected · Reconnecting (%1)...").arg(m_serverUrl);
    if (wasConnected || m_serverStatus != nextStatus) {
        m_serverStatus = nextStatus;
        emit statusChanged();
    }

    // Backoff retry: 1.5s -> 3s -> 5s -> 10s max
    int delayMs = 1500;
    if (m_consecutiveFailures == 2) delayMs = 3000;
    else if (m_consecutiveFailures == 3) delayMs = 5000;
    else if (m_consecutiveFailures >= 4) delayMs = 10000;

    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start(delayMs);
    }
}

void Backend::handleNetworkSuccess() {
    const bool wasReconnecting = (m_consecutiveFailures >= 4);
    const bool wasDisconnected = !m_connected;
    m_consecutiveFailures = 0;
    m_reconnectTimer.stop();
    m_connected = true;
    bool statusUpdated = false;
    if (m_serverStatus.isEmpty() ||
        m_serverStatus.startsWith(QStringLiteral("Connecting")) ||
        m_serverStatus.startsWith(QStringLiteral("Reconnecting")) ||
        m_serverStatus.startsWith(QStringLiteral("Disconnected"))) {
        m_serverStatus = m_lastKnownServerStatus.isEmpty()
                             ? QStringLiteral("Connected · Private API")
                             : m_lastKnownServerStatus;
        statusUpdated = true;
    }
    if (wasDisconnected || statusUpdated) {
        emit statusChanged();
    }

    if (wasReconnecting) {
        // Lightweight self-heal on reconnect after outage (do not re-flood with fetchRecentMessages)
        fetchServerInfo();
        fetchChatsMetadata();
        fetchUnreadStateFromServer();
        if (!m_selectedChatGuid.isEmpty()) {
            loadChatMessages(m_selectedChatGuid);
        }
        startSocketIoClient();
        QSet<QString> conflictPausedKeys;
        for (const QString &key : std::as_const(m_icloudMutationPausedKeys)) {
            if (key.startsWith(QStringLiteral("note_save:"))) {
                const QString id = key.mid(10);
                const QVariantMap pend = m_pendingIcloudNoteSaves.value(id);
                if (pend.value(QStringLiteral("hasServerConflict")).toBool() ||
                    pend.value(QStringLiteral("conflictServerRevision")).toLongLong() >
                        pend.value(QStringLiteral("baseRevision")).toLongLong()) {
                    conflictPausedKeys.insert(key);
                }
            } else if (key.startsWith(QStringLiteral("evt_save:"))) {
                const QString id = key.mid(9);
                const QVariantMap pend = m_pendingIcloudEventSaves.value(id);
                if (pend.value(QStringLiteral("conflictServerRevision")).toLongLong() >
                    pend.value(QStringLiteral("baseRevision")).toLongLong()) {
                    conflictPausedKeys.insert(key);
                }
            }
        }
        m_icloudMutationPausedKeys = conflictPausedKeys;
        m_icloudMutationRetryCounts.clear();
        m_icloudMutationNextRetryMs.clear();
        replayPendingIcloudMutations();
    }
}

QNetworkRequest Backend::makeSocketIoRequest(const QList<QPair<QString, QString>> &extraQueryItems, int timeoutMs) const {
    QList<QPair<QString, QString>> queryItems = {
        {QStringLiteral("EIO"), QStringLiteral("4")},
        {QStringLiteral("transport"), QStringLiteral("polling")}
    };
    queryItems.append(extraQueryItems);
    const QUrl url = buildApiUrl(QStringLiteral("/socket.io/"), queryItems);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/plain;charset=UTF-8"));
    req.setTransferTimeout(timeoutMs);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setRawHeader("Connection", "keep-alive");

    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    req.setSslConfiguration(ssl);
    return req;
}

void Backend::startSocketIoClient() {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    if (m_socketConnecting || !m_socketSid.isEmpty() || m_activeSocketPollReply) return;

    m_socketConnecting = true;
    QNetworkRequest req = makeSocketIoRequest({}, 15000);
    QNetworkReply *reply = m_socketNam.get(req);
    m_activeSocketPollReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_activeSocketPollReply == reply) {
            m_activeSocketPollReply = nullptr;
        }
        m_socketConnecting = false;
        reply->deleteLater();
        if (reply->error() == QNetworkReply::OperationCanceledError) return;
        if (reply->error() != QNetworkReply::NoError) {
            m_socketSid.clear();
            if (!m_demoMode && !m_socketReconnectTimer.isActive()) {
                m_socketReconnectTimer.start(3000);
            }
            return;
        }
        handleSocketIoPayload(reply->readAll());
    });
}

void Backend::stopSocketIoClient() {
    m_socketReconnectTimer.stop();
    m_socketConnecting = false;
    m_socketSid.clear();
    if (m_activeSocketPollReply) {
        QNetworkReply *r = m_activeSocketPollReply;
        m_activeSocketPollReply = nullptr;
        r->abort();
    }
}

void Backend::sendSocketIoPacket(const QByteArray &payload, bool startPollingAfter) {
    if (m_demoMode || m_socketSid.isEmpty() || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    const QString sid = m_socketSid;
    QNetworkRequest req = makeSocketIoRequest({{QStringLiteral("sid"), sid}}, 15000);
    QNetworkReply *reply = m_socketNam.post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, sid, startPollingAfter]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::OperationCanceledError) return;
        if (reply->error() != QNetworkReply::NoError) {
            if (m_socketSid == sid) {
                stopSocketIoClient();
                m_socketReconnectTimer.start(2500);
            }
            return;
        }
        if (startPollingAfter && m_socketSid == sid) {
            pollSocketIo();
        }
    });
}

void Backend::pollSocketIo() {
    if (m_demoMode || m_socketSid.isEmpty() || m_activeSocketPollReply || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    const QString sid = m_socketSid;
    QNetworkRequest req = makeSocketIoRequest({{QStringLiteral("sid"), sid}}, 85000);
    QNetworkReply *reply = m_socketNam.get(req);
    m_activeSocketPollReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, sid]() {
        if (m_activeSocketPollReply == reply) {
            m_activeSocketPollReply = nullptr;
        }
        reply->deleteLater();
        if (reply->error() == QNetworkReply::OperationCanceledError) return;
        if (reply->error() != QNetworkReply::NoError) {
            if (m_socketSid == sid) {
                stopSocketIoClient();
                m_socketReconnectTimer.start(2500);
            }
            return;
        }
        const QByteArray raw = reply->readAll();
        handleSocketIoPayload(raw);
        if (!m_demoMode && !m_socketSid.isEmpty() && m_socketSid == sid && !m_activeSocketPollReply) {
            pollSocketIo();
        }
    });
}

void Backend::handleSocketIoPayloadForTest(const QByteArray &rawPayload) {
    handleSocketIoPayload(rawPayload);
}

void Backend::handleSocketIoPayload(const QByteArray &rawPayload) {
    const QString text = QString::fromUtf8(rawPayload);
    if (text.isEmpty()) return;

    const QStringList packets = text.split(QChar(0x1E), Qt::SkipEmptyParts);
    bool shouldTriggerDeltaPoll = false;
    bool shouldTriggerUnreadCheck = false;
    bool shouldTriggerPhotosRefresh = false;

    for (const QString &pkt : packets) {
        const QString trimmed = pkt.trimmed();
        if (trimmed.isEmpty()) continue;

        const QChar eioType = trimmed.at(0);
        if (eioType == QLatin1Char('0')) {
            const QJsonObject openObj = QJsonDocument::fromJson(trimmed.mid(1).toUtf8()).object();
            const QString sid = openObj.value(QStringLiteral("sid")).toString().trimmed();
            if (!sid.isEmpty()) {
                m_socketSid = sid;
                sendSocketIoPacket(QByteArrayLiteral("40"), true);
            }
        } else if (eioType == QLatin1Char('1')) {
            stopSocketIoClient();
            if (!m_demoMode) {
                m_socketReconnectTimer.start(2000);
            }
            return;
        } else if (eioType == QLatin1Char('2')) {
            const QByteArray pong = QByteArrayLiteral("3") + trimmed.mid(1).toUtf8();
            sendSocketIoPacket(pong, false);
        } else if (trimmed.startsWith(QStringLiteral("41"))) {
            stopSocketIoClient();
            if (!m_demoMode) {
                m_socketReconnectTimer.start(2000);
            }
            return;
        } else if (trimmed.startsWith(QStringLiteral("42"))) {
            const QJsonArray eventArr = QJsonDocument::fromJson(trimmed.mid(2).toUtf8()).array();
            if (eventArr.isEmpty()) continue;
            const QString eventName = eventArr.first().toString();
            const QJsonValue eventData = eventArr.size() > 1 ? eventArr.at(1) : QJsonValue();

            if (eventName == QStringLiteral("new-message") || eventName == QStringLiteral("updated-message")) {
                if (eventData.isObject()) {
                    const QJsonObject msgObj = eventData.toObject();
                    if (!msgObj.value(QStringLiteral("guid")).toString().isEmpty() &&
                        (!msgObj.value(QStringLiteral("chats")).toArray().isEmpty() ||
                         !msgObj.value(QStringLiteral("handle")).toObject().value(QStringLiteral("address")).toString().trimmed().isEmpty())) {
                        processIncomingMessages(QJsonArray{msgObj}, false);
                        saveMessagesCache();
                    }
                } else if (eventData.isArray()) {
                    processIncomingMessages(eventData.toArray(), false);
                    saveMessagesCache();
                }
                shouldTriggerDeltaPoll = true;
                shouldTriggerUnreadCheck = true;
            } else if (eventName == QStringLiteral("message-send-error")) {
                if (eventData.isObject()) {
                    QJsonObject errObj = eventData.toObject();
                    if (errObj.value(QStringLiteral("message")).isObject()) {
                        errObj = errObj.value(QStringLiteral("message")).toObject();
                    }
                    if (!errObj.value(QStringLiteral("guid")).toString().isEmpty()) {
                        processIncomingMessages(QJsonArray{errObj}, false);
                        saveMessagesCache();
                    } else {
                        const int ec = eventData.toObject().value(QStringLiteral("error")).toInt(4);
                        emit toastMessage(QStringLiteral("⚠ Message failed to deliver (error %1)").arg(ec));
                    }
                }
                shouldTriggerDeltaPoll = true;
            } else if (eventName == QStringLiteral("chat-read-status-changed") ||
                       eventName == QStringLiteral("group-name-change") ||
                       eventName == QStringLiteral("participant-added") ||
                       eventName == QStringLiteral("participant-removed") ||
                       eventName == QStringLiteral("participant-left")) {
                shouldTriggerUnreadCheck = true;
            } else if (eventName == QStringLiteral("icloud-photos-updated")) {
                if (m_icloudPhotosEnabled && !m_demoMode) {
                    if (eventData.isObject()) {
                        const QJsonValue evTotalVal = eventData.toObject().value(QStringLiteral("total"));
                        if (evTotalVal.isDouble()) {
                            const int evTotal = evTotalVal.toInt(-1);
                            if (evTotal >= 0 && evTotal != m_icloudPhotosTotal) {
                                m_icloudPhotosTotal = evTotal;
                                emit icloudPhotosChanged();
                            }
                        }
                    }
                    shouldTriggerPhotosRefresh = true;
                }
            }
        }
    }

    if (!m_demoMode && !m_serverUrl.isEmpty() && !m_password.isEmpty()) {
        if (shouldTriggerDeltaPoll) {
            QTimer::singleShot(80, this, &Backend::pollDelta);
        }
        if (shouldTriggerUnreadCheck) {
            QTimer::singleShot(150, this, &Backend::fetchUnreadStateFromServer);
        }
        if (shouldTriggerPhotosRefresh) {
            const quint64 navGen = m_icloudPhotosRequestSeq;
            QTimer::singleShot(120, this, [this, navGen]() {
                if (m_icloudPhotosRequestSeq != navGen) return;
                if (m_icloudPhotosPage == 0 && m_icloudPhotos.size() <= std::max(12, m_icloudPhotosPageSize)) {
                    fetchIcloudPhotos(0, false);
                } else {
                    fetchIcloudPhotosTotalOnly();
                }
            });
        }
    }
}

void Backend::refreshAll() {
    if (m_demoMode) return;
    if (m_serverUrl.isEmpty() || m_password.isEmpty()) {
        loadCredentials();
        if (m_serverUrl.isEmpty() || m_password.isEmpty()) {
            m_serverStatus = QStringLiteral("Missing BlueBubbles credentials");
            emit statusChanged();
            return;
        }
    }
    // A custom-domain Tailscale name (e.g. relay.tail.example.com) over http://
    // is only trusted once it's confirmed to resolve to a tailnet node.
    const QStringList unverifiedHosts = hostsNeedingTailnetVerification();
    if (!unverifiedHosts.isEmpty()) {
        m_serverStatus = QStringLiteral("Checking Tailscale address...");
        emit statusChanged();
        // Deferred so a check that's already in flight (or finishes at once)
        // can never re-enter refreshAll() recursively.
        verifyTailnetHosts(unverifiedHosts, [this]() {
            QTimer::singleShot(0, this, &Backend::refreshAll);
        });
        return;
    }
    if (!m_connected && m_chats.isEmpty()) {
        m_serverStatus = QStringLiteral("Connecting...");
    }
    m_loading = true;
    emit loadingChanged();
    emit statusChanged();
    fetchServerInfo();
    fetchChatsMetadata();
    fetchRecentMessages(0);
    fetchUnreadStateFromServer();
    fetchMacPins();
    startSocketIoClient();
    // Defer heavy macOS AddressBook contact export so it never blocks BlueBubbles's
    // single-threaded Node.js event loop during initial startup/sync
    QTimer::singleShot(m_contactNames.isEmpty() ? 2000 : 12000, this, &Backend::fetchContactsRemote);
    QSet<QString> conflictPausedKeys;
    for (const QString &key : std::as_const(m_icloudMutationPausedKeys)) {
        if (key.startsWith(QStringLiteral("note_save:"))) {
            const QString id = key.mid(10);
            const QVariantMap pend = m_pendingIcloudNoteSaves.value(id);
            if (pend.value(QStringLiteral("hasServerConflict")).toBool() ||
                pend.value(QStringLiteral("conflictServerRevision")).toLongLong() >
                    pend.value(QStringLiteral("baseRevision")).toLongLong()) {
                conflictPausedKeys.insert(key);
            }
        } else if (key.startsWith(QStringLiteral("evt_save:"))) {
            const QString id = key.mid(9);
            const QVariantMap pend = m_pendingIcloudEventSaves.value(id);
            if (pend.value(QStringLiteral("conflictServerRevision")).toLongLong() >
                pend.value(QStringLiteral("baseRevision")).toLongLong()) {
                conflictPausedKeys.insert(key);
            }
        }
    }
    for (const QString &key : std::as_const(m_icloudMutationPausedKeys)) {
        if (!conflictPausedKeys.contains(key)) {
            m_icloudMutationRetryCounts.remove(key);
            m_icloudMutationNextRetryMs.remove(key);
        }
    }
    m_icloudMutationPausedKeys = conflictPausedKeys;
    replayPendingIcloudMutations();
    fetchIcloudNotes();
    fetchIcloudEvents();
    fetchIcloudPhotos();
}

void Backend::fetchServerInfo() {
    QNetworkReply *reply = m_nam.get(makeRequest(QStringLiteral("/api/v1/server/info")));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkFailure(QStringLiteral("fetchServerInfo"));
            return;
        }
        handleNetworkSuccess();
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject data = root.value(QStringLiteral("data")).toObject();
        const QString ver = data.value(QStringLiteral("server_version")).toString();
        const QString osVer = data.value(QStringLiteral("os_version")).toString();
        const bool privApi = data.value(QStringLiteral("private_api")).toBool();
        m_serverStatus = QStringLiteral("Connected v%1 · macOS %2%3")
                             .arg(ver.isEmpty() ? QStringLiteral("1.9") : ver,
                                  osVer.isEmpty() ? QStringLiteral("15") : osVer,
                                  privApi ? QStringLiteral(" · Private API") : QString());
        m_lastKnownServerStatus = m_serverStatus;
        saveAppState();
        emit statusChanged();
    });
}

void Backend::fetchContactsRemote() {
    m_lastContactsFetchMs = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply *reply = m_nam.get(makeRequest(QStringLiteral("/api/v1/contact")));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        const QByteArray bytes = reply->readAll();
        parseContactsJson(bytes);
        m_lastContactsFetchMs = QDateTime::currentMSecsSinceEpoch();

        // Cache contacts with strict 0600 permissions
        const QString configPath = QDir::homePath() + QStringLiteral("/.config/imsg/contacts.json");
        QDir().mkpath(QFileInfo(configPath).absolutePath());
        QFile f(configPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(bytes);
            f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            f.close();
        }

        for (ChatEntry &c : m_chats) {
            rebuildChatPresentation(c);
        }
        rebuildSelectedChatMessages();
        saveMessagesCache();
        emit chatsChanged();
        emit selectedChatChanged();
    });
}

qint64 Backend::extractChatReadWatermark(const QJsonObject &chatObj, const QHash<QString, QJsonObject> &chatMessages) {
    qint64 bestTs = chatObj.value(QStringLiteral("lastReadMessageTimestamp")).toVariant().toLongLong();

    const QJsonArray propsArr = chatObj.value(QStringLiteral("properties")).toArray();
    for (const QJsonValue &pv : propsArr) {
        if (!pv.isObject()) continue;
        const QJsonObject propObj = pv.toObject();

        const QString lsmdStr = propObj.value(QStringLiteral("LSMD")).toString().trimmed();
        if (!lsmdStr.isEmpty()) {
            QDateTime dt = QDateTime::fromString(lsmdStr, Qt::ISODateWithMs);
            if (!dt.isValid()) {
                dt = QDateTime::fromString(lsmdStr, Qt::ISODate);
            }
            if (dt.isValid()) {
                bestTs = qMax(bestTs, dt.toMSecsSinceEpoch());
            }
        }

        const QString seenGuid = propObj.value(QStringLiteral("lastSeenMessageGuid")).toString().trimmed();
        if (!seenGuid.isEmpty() && chatMessages.contains(seenGuid)) {
            const qint64 seenMsgCreated = chatMessages.value(seenGuid).value(QStringLiteral("dateCreated")).toVariant().toLongLong();
            bestTs = qMax(bestTs, seenMsgCreated);
        }
    }

    return bestTs;
}

void Backend::ingestChatsMetadataForTest(const QJsonArray &chatsArray) {
    processChatsMetadata(chatsArray);
}

void Backend::fetchChatsMetadata() {
    QJsonObject body;
    body.insert(QStringLiteral("limit"), 250);
    body.insert(QStringLiteral("offset"), 0);
    body.insert(QStringLiteral("sort"), QStringLiteral("lastmessage"));
    body.insert(QStringLiteral("with"), QJsonArray{
        QStringLiteral("participants")
    });

    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/chat/query")),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fetchMissingGroupParticipants();
            return;
        }
        handleNetworkSuccess();
        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
        processChatsMetadata(arr);
        saveMessagesCache();
        fetchMissingGroupParticipants();
        fetchUnresolvedPinnedChats();
    });
}

void Backend::fetchMissingGroupParticipants() {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    if (tailnetCheckPending()) {
        m_resumeAfterTailnetCheck = true;
        return;
    }

    int dispatched = 0;
    for (const ChatEntry &c : m_chats) {
        if (!c.isGroup || !c.displayName.isEmpty()) continue;
        if (m_fetchedChatParticipantGuids.contains(c.guid)) continue;

        m_fetchedChatParticipantGuids.insert(c.guid);
        const QString chatGuid = c.guid;
        const QString encodedGuid = QString::fromUtf8(QUrl::toPercentEncoding(chatGuid));
        const QString path = QStringLiteral("/api/v1/chat/%1?with=participants").arg(encodedGuid);
        QNetworkReply *reply = m_nam.get(makeRequest(path));
        connect(reply, &QNetworkReply::finished, this, [this, reply, chatGuid]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                m_fetchedChatParticipantGuids.remove(chatGuid);
                return;
            }
            const QJsonObject dataObj = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toObject();
            if (!dataObj.value(QStringLiteral("guid")).toString().isEmpty()) {
                processChatsMetadata(QJsonArray{dataObj});
                saveMessagesCache();
            }
        });
        if (++dispatched >= 8) break;
    }
}

QString Backend::normalizeServiceName(const QString &raw) {
    const QString s = raw.trimmed();
    if (s.compare(QStringLiteral("iMessage"), Qt::CaseInsensitive) == 0) return QStringLiteral("iMessage");
    if (s.compare(QStringLiteral("RCS"), Qt::CaseInsensitive) == 0) return QStringLiteral("RCS");
    if (s.compare(QStringLiteral("SMS"), Qt::CaseInsensitive) == 0 ||
        s.compare(QStringLiteral("MMS"), Qt::CaseInsensitive) == 0) return QStringLiteral("SMS");
    return QString();
}

static QString serviceFromMessageJson(const QJsonObject &m) {
    QString svc = Backend::normalizeServiceName(m.value(QStringLiteral("service")).toString());
    if (svc.isEmpty()) {
        svc = Backend::normalizeServiceName(m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("service")).toString());
    }
    return svc;
}

QString Backend::resolveChatService(const QJsonObject &chat, const QJsonObject &message, qint64 *evidenceDate) {
    if (evidenceDate) *evidenceDate = 0;
    // 1. Legacy GUID prefixes ("iMessage;-;", "SMS;+;", "RCS;-;") are authoritative.
    const QString guid = chat.value(QStringLiteral("guid")).toString().trimmed();
    const QString prefix = guid.section(QLatin1Char(';'), 0, 0);
    if (guid.contains(QLatin1Char(';'))) {
        const QString fromPrefix = normalizeServiceName(prefix);
        if (!fromPrefix.isEmpty()) return fromPrefix;
    }
    // 2. Explicit chat-level service, if the server sends one.
    for (const char *key : {"serviceName", "service"}) {
        const QString svc = normalizeServiceName(chat.value(QLatin1String(key)).toString());
        if (!svc.isEmpty()) return svc;
    }
    // 3. The newest message we know of (most reliable for "any;" chats).
    const QJsonObject lastMessage = chat.value(QStringLiteral("lastMessage")).toObject();
    for (const QJsonObject *m : {&message, &lastMessage}) {
        const QString svc = serviceFromMessageJson(*m);
        if (svc.isEmpty()) continue;
        if (evidenceDate) *evidenceDate = m->value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        return svc;
    }
    // 4. Participant handles: one SMS member makes the thread SMS/MMS.
    bool sawIMessage = false, sawRcs = false, sawSms = false;
    for (const QJsonValue &pv : chat.value(QStringLiteral("participants")).toArray()) {
        const QString ps = normalizeServiceName(pv.toObject().value(QStringLiteral("service")).toString());
        if (ps == QStringLiteral("iMessage")) sawIMessage = true;
        else if (ps == QStringLiteral("RCS")) sawRcs = true;
        else if (ps == QStringLiteral("SMS")) sawSms = true;
    }
    if (sawSms) return QStringLiteral("SMS");
    if (sawRcs) return QStringLiteral("RCS");
    if (sawIMessage) return QStringLiteral("iMessage");
    return QString();
}

bool Backend::applyChatServiceEvidence(ChatEntry &entry, const QString &svc, qint64 evidenceDate) {
    if (svc.isEmpty()) return false;
    const bool hasDated = m_chatServiceDates.contains(entry.guid);
    if (evidenceDate > 0) {
        if (hasDated && evidenceDate < m_chatServiceDates.value(entry.guid)) return false;
        m_chatServiceDates.insert(entry.guid, evidenceDate);
    } else if (hasDated) {
        return false;  // participants/prefix never override message evidence
    }
    if (entry.service == svc) return false;
    entry.service = svc;
    return true;
}

void Backend::refineChatServiceFromMessage(const QString &chatGuid, const QJsonObject &msg) {
    const QString svc = serviceFromMessageJson(msg);
    if (svc.isEmpty() || chatGuid.isEmpty()) return;
    // Tapbacks and other associated messages carry the parent's service; fine to use.
    const qint64 dc = msg.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
    for (ChatEntry &entry : m_chats) {
        if (entry.guid != chatGuid) continue;
        if (!applyChatServiceEvidence(entry, svc, qMax<qint64>(dc, 1))) return;
        if (!m_chatServiceEmitPending) {
            m_chatServiceEmitPending = true;
            QTimer::singleShot(0, this, [this]() {
                m_chatServiceEmitPending = false;
                emit chatsChanged();
                emit selectedChatChanged();
            });
        }
        return;
    }
}

void Backend::processChatsMetadata(const QJsonArray &arr) {
    bool chatsAddedOrModified = false;
    bool watermarksChanged = false;
    const QString prevSelectedSig = m_selectedChatGuid + QLatin1Char('|') + selectedChatTitle() + QLatin1Char('|') +
                                    selectedChatSubtitle() + QLatin1Char('|') + QString::number(selectedChatUnreadCount()) +
                                    QLatin1Char('|') + selectedChatParticipants().join(QLatin1Char(',')) +
                                    QLatin1Char('|') + selectedChatService();

    for (const QJsonValue &v : arr) {
        const QJsonObject c = v.toObject();
        const QString rawGuid = c.value(QStringLiteral("guid")).toString().trimmed();
        if (rawGuid.isEmpty()) continue;
        recordChatGroupMetadata(c);
        const QString guid = resolveCanonicalChatGuid(rawGuid);
        if (m_deletedChatTimestamps.contains(guid) || m_deletedChatTimestamps.contains(rawGuid)) {
            const qint64 deletedAt = qMax(m_deletedChatTimestamps.value(guid, 0), m_deletedChatTimestamps.value(rawGuid, 0));
            const qint64 chatWm = extractChatReadWatermark(c, m_chatMessagesCache.value(guid));
            if (chatWm <= deletedAt && !m_chatMessagesCache.contains(guid)) {
                continue;
            }
        }

        const QString disp = c.value(QStringLiteral("displayName")).toString().trimmed();
        if (!disp.isEmpty()) m_chatDisplayNames.insert(guid, disp);
        if (c.contains(QStringLiteral("style"))) {
            const int st = c.value(QStringLiteral("style")).toInt(0);
            if (st > 0) m_chatStyles.insert(guid, st);
        }

        QStringList handles;
        const QJsonArray parts = c.value(QStringLiteral("participants")).toArray();
        for (const QJsonValue &pv : parts) {
            const QString addr = pv.toObject().value(QStringLiteral("address")).toString().trimmed();
            if (!addr.isEmpty()) handles.append(addr);
        }
        if (!handles.isEmpty()) {
            m_chatParticipants.insert(guid, handles);
            m_fetchedChatParticipantGuids.insert(guid);
        }

        const bool recentLocalMutation = (m_localReadMutationTimeMs.value(guid, 0) > QDateTime::currentMSecsSinceEpoch() - 15000);
        const qint64 chatReadWm = extractChatReadWatermark(c, m_chatMessagesCache.value(guid));
        if (chatReadWm > 0) {
            const qint64 overrideTs = m_unreadOverrides.value(guid, 0);
            if (overrideTs == 0 || chatReadWm > overrideTs) {
                if (overrideTs > 0 && chatReadWm > overrideTs) {
                    m_unreadOverrides.remove(guid);
                    watermarksChanged = true;
                }
                if (chatReadWm > m_readWatermarks.value(guid, 0)) {
                    m_readWatermarks[guid] = chatReadWm;
                    watermarksChanged = true;
                }
            }
        }

        int existingIdx = -1;
        for (int i = 0; i < m_chats.size(); ++i) {
            if (m_chats[i].guid == guid || m_chats[i].guid == rawGuid || m_chats[i].siblingGuids.contains(rawGuid)) {
                existingIdx = i;
                break;
            }
        }

        const QString origGrpId = c.value(QStringLiteral("originalGroupId")).toString().trimmed();
        const QString grpId = c.value(QStringLiteral("groupId")).toString().trimmed();
        const qint64 origRowId = c.value(QStringLiteral("originalROWID")).toVariant().toLongLong();
        const bool isArchived = c.value(QStringLiteral("isArchived")).toBool(false);
        const QString activeTarget = c.value(QStringLiteral("activeSendChatGuid")).toString().trimmed();
        if (!activeTarget.isEmpty()) {
            m_chatPreferredSendGuids.insert(guid, activeTarget);
        } else if (!rawGuid.isEmpty()) {
            m_chatPreferredSendGuids.insert(guid, rawGuid);
        }

        if (existingIdx >= 0) {
            ChatEntry &entry = m_chats[existingIdx];
            if (!handles.isEmpty()) entry.participantHandles = handles;
            {
                QJsonObject svcChat = c;
                svcChat.insert(QStringLiteral("guid"), rawGuid);
                qint64 svcDate = 0;
                const QString svc = resolveChatService(svcChat, QJsonObject(), &svcDate);
                if (applyChatServiceEvidence(entry, svc, svcDate)) chatsAddedOrModified = true;
            }
            if (!disp.isEmpty()) entry.displayName = disp;
            if (!origGrpId.isEmpty()) entry.originalGroupId = origGrpId;
            if (!grpId.isEmpty()) entry.groupId = grpId;
            if (origRowId > entry.originalRowId) entry.originalRowId = origRowId;
            entry.isArchived = isArchived;
            if (c.value(QStringLiteral("siblingChatGuids")).isArray()) {
                for (const QJsonValue &sv : c.value(QStringLiteral("siblingChatGuids")).toArray()) {
                    const QString sg = sv.toString().trimmed();
                    if (!sg.isEmpty() && !entry.siblingGuids.contains(sg)) {
                        entry.siblingGuids.append(sg);
                    }
                }
            }
            if ((!recentLocalMutation || m_unreadOverrides.value(guid, 0) == 0) && m_chatMessagesCache.contains(guid)) {
                int count = 0;
                const qint64 wm = m_readWatermarks.value(guid, 0);
                const qint64 uo = m_unreadOverrides.value(guid, 0);
                const auto &chatMap = m_chatMessagesCache.value(guid);
                for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
                    if (isMessageUnread(it.value(), wm, uo)) {
                        count++;
                    }
                }
                if (entry.unreadCount != count) {
                    entry.unreadCount = count;
                    chatsAddedOrModified = true;
                }
            } else if (!recentLocalMutation && chatReadWm > 0 && entry.lastMessageDate > 0 && chatReadWm >= entry.lastMessageDate && entry.unreadCount > 0 && !m_unreadOverrides.contains(guid)) {
                entry.unreadCount = 0;
                chatsAddedOrModified = true;
            }
            rebuildChatPresentation(entry);
            chatsAddedOrModified = true;
        } else {
            ChatEntry entry;
            entry.guid = guid;
            entry.chatIdentifier = c.value(QStringLiteral("chatIdentifier")).toString();
            entry.displayName = disp;
            entry.originalGroupId = origGrpId;
            entry.groupId = grpId;
            entry.originalRowId = origRowId;
            entry.isArchived = isArchived;
            if (c.value(QStringLiteral("siblingChatGuids")).isArray()) {
                for (const QJsonValue &sv : c.value(QStringLiteral("siblingChatGuids")).toArray()) {
                    const QString sg = sv.toString().trimmed();
                    if (!sg.isEmpty() && !entry.siblingGuids.contains(sg)) {
                        entry.siblingGuids.append(sg);
                    }
                }
            }
            if (!entry.siblingGuids.contains(guid)) entry.siblingGuids.append(guid);
            const int style = c.value(QStringLiteral("style")).toInt(m_chatStyles.value(guid, 45));
            entry.isGroup = (style == 43) || entry.chatIdentifier.startsWith(QStringLiteral("chat")) || guid.contains(QStringLiteral(";+;"));
            entry.participantHandles = handles;
            if (entry.participantHandles.isEmpty() && !entry.isGroup && !entry.chatIdentifier.isEmpty()) {
                entry.participantHandles.append(entry.chatIdentifier);
            }
            {
                QJsonObject svcChat = c;
                svcChat.insert(QStringLiteral("guid"), rawGuid);
                qint64 svcDate = 0;
                const QString svc = resolveChatService(svcChat, QJsonObject(), &svcDate);
                applyChatServiceEvidence(entry, svc, svcDate);
            }
            entry.lastMessageDate = 1;
            entry.lastMessagePreview = QStringLiteral("Tap to view conversation history");
            entry.relativeTime = QString();
            rebuildChatPresentation(entry);
            m_chats.append(entry);
            chatsAddedOrModified = true;
        }
    }

    bool selectedMigrated = false;
    bool chatsMerged = false;
    unifySiblingChats(&selectedMigrated, &chatsMerged);
    if (chatsMerged) {
        chatsAddedOrModified = true;
    }

    if (watermarksChanged) {
        saveAppState();
    }

    if (chatsAddedOrModified) {
        sortChats();
        emit chatsChanged();
        const QString nextSelectedSig = m_selectedChatGuid + QLatin1Char('|') + selectedChatTitle() + QLatin1Char('|') +
                                        selectedChatSubtitle() + QLatin1Char('|') + QString::number(selectedChatUnreadCount()) +
                                        QLatin1Char('|') + selectedChatParticipants().join(QLatin1Char(',')) +
                                    QLatin1Char('|') + selectedChatService();
        if (selectedMigrated || nextSelectedSig != prevSelectedSig) {
            emit selectedChatChanged();
        }
        if (selectedMigrated && !m_selectedChatGuid.isEmpty()) {
            rebuildSelectedChatMessages();
        }
    }
}

void Backend::fetchRecentMessages(int offset, int syncGen) {
    if (offset == 0) {
        syncGen = ++m_historySyncGeneration;
        if (!m_syncingHistory) {
            m_syncingHistory = true;
            emit syncingHistoryChanged();
        }
    } else if (syncGen != m_historySyncGeneration) {
        return;
    }

    const int pageSize = 250;
    const qint64 afterMs = QDateTime::currentMSecsSinceEpoch() - (static_cast<qint64>(m_historySyncDays) * 86400LL * 1000LL);
    QJsonObject body;
    body.insert(QStringLiteral("limit"), pageSize);
    body.insert(QStringLiteral("offset"), offset);
    body.insert(QStringLiteral("after"), afterMs);
    body.insert(QStringLiteral("sort"), QStringLiteral("DESC"));
    body.insert(QStringLiteral("with"), QJsonArray{
        QStringLiteral("chat"),
        QStringLiteral("handle"),
        QStringLiteral("attachment"),
        QStringLiteral("attributedbody")
    });

    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, offset, syncGen, pageSize]() {
        reply->deleteLater();
        if (offset == 0) {
            m_loading = false;
            emit loadingChanged();
        }

        if (reply->error() != QNetworkReply::NoError) {
            if (syncGen == m_historySyncGeneration && m_syncingHistory) {
                m_syncingHistory = false;
                emit syncingHistoryChanged();
                saveMessagesCache();
            }
            return;
        }
        handleNetworkSuccess();
        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
        processIncomingMessages(arr, true);
        if (offset == 0) {
            m_initialSyncComplete = true;
            m_startupHighestRowId = qMax(m_startupHighestRowId, m_highestMessageRowId);
            saveMessagesCache();
        }

        if (syncGen != m_historySyncGeneration) return;

        if (arr.size() >= pageSize && (offset + pageSize) < 2500) {
            QTimer::singleShot(120, this, [this, offset, pageSize, syncGen]() {
                fetchRecentMessages(offset + pageSize, syncGen);
            });
        } else {
            if (m_syncingHistory) {
                m_syncingHistory = false;
                emit syncingHistoryChanged();
            }
            saveMessagesCache();
        }
    });
}

void Backend::reconcileUnreadMessagesForTest(const QJsonArray &unreadMessages) {
    reconcileServerUnreadMessages(unreadMessages);
}

void Backend::fetchUnreadStateFromServer() {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;

    const qint64 afterMs = QDateTime::currentMSecsSinceEpoch() - (static_cast<qint64>(qMax(m_historySyncDays, 60)) * 86400LL * 1000LL);
    QJsonObject whereClause;
    whereClause.insert(QStringLiteral("statement"),
                       QStringLiteral("message.is_read = :isRead AND message.is_from_me = :fromMe AND message.item_type = :itemType"));
    QJsonObject argsObj;
    argsObj.insert(QStringLiteral("isRead"), 0);
    argsObj.insert(QStringLiteral("fromMe"), 0);
    argsObj.insert(QStringLiteral("itemType"), 0);
    whereClause.insert(QStringLiteral("args"), argsObj);

    QJsonObject body;
    body.insert(QStringLiteral("limit"), 200);
    body.insert(QStringLiteral("offset"), 0);
    body.insert(QStringLiteral("after"), afterMs);
    body.insert(QStringLiteral("sort"), QStringLiteral("DESC"));
    body.insert(QStringLiteral("where"), QJsonArray{whereClause});
    body.insert(QStringLiteral("with"), QJsonArray{
        QStringLiteral("chat"),
        QStringLiteral("handle"),
        QStringLiteral("attachment"),
        QStringLiteral("attributedbody")
    });

    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        handleNetworkSuccess();
        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
        reconcileServerUnreadMessages(arr);
    });
}

void Backend::reconcileServerUnreadMessages(const QJsonArray &unreadMessages) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, QSet<QString>> serverUnreadByChat;
    QSet<QString> chatsWithNewOrUpdatedMessages;
    bool stateChanged = false;
    bool chatsModified = false;
    bool selectedChatUpdated = false;

    for (const QJsonValue &v : unreadMessages) {
        QJsonObject m = v.toObject();
        const QString msgGuid = m.value(QStringLiteral("guid")).toString();
        if (msgGuid.isEmpty()) continue;
        if (m.value(QStringLiteral("isFromMe")).toBool()) continue;
        if (m.value(QStringLiteral("itemType")).toInt(0) != 0) continue;
        const QString assocType = m.value(QStringLiteral("associatedMessageType")).toVariant().toString();
        if (!assocType.isEmpty() && assocType != QStringLiteral("0")) continue;

        QJsonArray chatsArr = m.value(QStringLiteral("chats")).toArray();
        if (chatsArr.isEmpty()) {
            const QString cacheRoom = m.value(QStringLiteral("cacheRoomnames")).toString().trimmed();
            const QString grpTitle = m.value(QStringLiteral("groupTitle")).toString().trimmed();
            QString svc = m.value(QStringLiteral("service")).toString().trimmed();
            if (svc.isEmpty()) {
                svc = m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("service")).toString().trimmed();
            }
            if (svc.isEmpty()) {
                svc = QStringLiteral("SMS");
            }
            if (!cacheRoom.isEmpty()) {
                QJsonObject synthChat;
                synthChat.insert(QStringLiteral("guid"), QStringLiteral("%1;+;%2").arg(svc, cacheRoom));
                synthChat.insert(QStringLiteral("chatIdentifier"), cacheRoom);
                if (!grpTitle.isEmpty()) synthChat.insert(QStringLiteral("displayName"), grpTitle);
                synthChat.insert(QStringLiteral("style"), 43);
                chatsArr.append(synthChat);
                m.insert(QStringLiteral("chats"), chatsArr);
            } else if (grpTitle.isEmpty()) {
                const QString handleAddr = m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("address")).toString().trimmed();
                if (!handleAddr.isEmpty()) {
                    QJsonObject synthChat;
                    synthChat.insert(QStringLiteral("guid"), QStringLiteral("%1;-;%2").arg(svc, handleAddr));
                    synthChat.insert(QStringLiteral("chatIdentifier"), handleAddr);
                    synthChat.insert(QStringLiteral("style"), 45);
                    chatsArr.append(synthChat);
                    m.insert(QStringLiteral("chats"), chatsArr);
                }
            }
        }
        if (chatsArr.isEmpty()) continue;

        m.insert(QStringLiteral("isRead"), false);
        const qint64 dateCreated = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        const qint64 dateRead = m.value(QStringLiteral("dateRead")).toVariant().toLongLong();
        if (dateCreated > m_lastMessageTimestamp) {
            m_lastMessageTimestamp = dateCreated;
        }
        const qint64 rowId = m.value(QStringLiteral("originalROWID")).toVariant().toLongLong();
        if (rowId > 0 && rowId < 100000000LL && !msgGuid.startsWith(QStringLiteral("temp-")) && !msgGuid.startsWith(QStringLiteral("bp-")) && rowId > m_highestMessageRowId) {
            m_highestMessageRowId = rowId;
        }

        const bool alreadySeen = m_seenMessageGuids.contains(msgGuid);
        m_seenMessageGuids.insert(msgGuid);
        if (m_autoStart && !m_initialSyncComplete) {
            m_notifiedMessageGuids.insert(msgGuid);
        }

        for (const QJsonValue &cv : chatsArr) {
            const QJsonObject cObj = cv.toObject();
            recordChatGroupMetadata(cObj);
            const QString rawCg = cObj.value(QStringLiteral("guid")).toString().trimmed();
            const QString cg = resolveCanonicalChatGuid(rawCg);
            if (cg.isEmpty()) continue;
            const bool recentLocalMut = (m_localReadMutationTimeMs.value(cg, 0) > nowMs - 15000);
            const qint64 chatWm = m_readWatermarks.value(cg, 0);
            if (recentLocalMut && dateCreated <= chatWm) continue;

            const bool wasCached = m_chatMessagesCache.contains(cg) && m_chatMessagesCache.value(cg).contains(msgGuid);
            serverUnreadByChat[cg].insert(msgGuid);
            cacheMessageForChat(cg, m);

            int chatIdx = -1;
            for (int i = 0; i < m_chats.size(); ++i) {
                if (m_chats[i].guid == cg || m_chats[i].guid == rawCg || m_chats[i].siblingGuids.contains(rawCg)) {
                    chatIdx = i;
                    break;
                }
            }
            if (chatIdx < 0) {
                ChatEntry entry;
                entry.guid = cg;
                entry.chatIdentifier = cObj.value(QStringLiteral("chatIdentifier")).toString();
                entry.displayName = cObj.value(QStringLiteral("displayName")).toString().trimmed();
                if (entry.displayName.isEmpty() && m_chatDisplayNames.contains(cg)) {
                    entry.displayName = m_chatDisplayNames.value(cg);
                }
                const int style = cObj.value(QStringLiteral("style")).toInt(m_chatStyles.value(cg, 45));
                entry.isGroup = (style == 43) || entry.chatIdentifier.startsWith(QStringLiteral("chat"));
                entry.participantHandles = m_chatParticipants.value(cg);
                if (entry.participantHandles.isEmpty() && !entry.isGroup && !entry.chatIdentifier.isEmpty()) {
                    entry.participantHandles.append(entry.chatIdentifier);
                }
                {
                    // Use everything the embedded chat carries (serviceName,
                    // lastMessage, participants), not just its GUID.
                    QJsonObject svcChat = cObj;
                    svcChat.insert(QStringLiteral("guid"), cg);
                    qint64 svcDate = 0;
                    const QString svc = resolveChatService(svcChat, m, &svcDate);
                    applyChatServiceEvidence(entry, svc, svcDate);
                }
                entry.lastMessageDate = dateCreated;
                QString preview = extractMessageText(m);
                entry.lastMessagePreview = preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
                entry.relativeTime = formatRelativeTime(dateCreated);
                rebuildChatPresentation(entry);
                m_chats.append(entry);
                chatIdx = m_chats.size() - 1;
                chatsModified = true;
            }

            if (!wasCached || !alreadySeen) {
                chatsWithNewOrUpdatedMessages.insert(cg);
                if (cg == m_selectedChatGuid) {
                    selectedChatUpdated = true;
                }
            }

            if (!alreadySeen) {
                if (dateCreated >= (nowMs - 3600000LL)) {
                    prefetchMessageAttachments(m, false);
                }
                if (!wasCached && shouldNotifyForIncomingMessage(m, cg, dateCreated, rowId)) {
                    QString preview = extractMessageText(m);
                    if (!preview.isEmpty()) {
                        m_notifiedMessageGuids.insert(msgGuid);
                        m_lastNotifiedTimestamp = qMax(m_lastNotifiedTimestamp, dateCreated);
                        stateChanged = true;
                        const QString senderAddr = m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("address")).toString();
                        const QString senderName = senderAddr.isEmpty() ? m_chats[chatIdx].title : resolveContactName(senderAddr);
                        notifyIncomingMessage(cg, senderName, preview);
                    }
                }
            }

            // If a message on the Mac has is_read = 0 AND date_read > 0, it was previously read
            // and then explicitly marked unread on iPhone/Mac ("Mark as Unread").
            // Override any older local readWatermark so the chat shows unread in imsg!
            if (dateRead > 0) {
                const qint64 targetOverride = qMax(dateCreated, dateRead);
                if (m_unreadOverrides.value(cg, 0) < targetOverride) {
                    m_unreadOverrides[cg] = targetOverride;
                    stateChanged = true;
                }
                if (m_readWatermarks.value(cg, 0) >= dateCreated) {
                    m_readWatermarks[cg] = qMax(0LL, dateCreated - 1LL);
                    stateChanged = true;
                }
            }
        }
    }

    for (ChatEntry &c : m_chats) {
        const QSet<QString> unreadGuids = serverUnreadByChat.value(c.guid);
        const bool recentLocalMut = (m_localReadMutationTimeMs.value(c.guid, 0) > nowMs - 15000);
        if (recentLocalMut && unreadGuids.isEmpty()) continue;
        if (unreadGuids.isEmpty()) {
            // Authoritative server check says 0 unread messages in chat.db for this chat
            if (m_unreadOverrides.contains(c.guid)) {
                m_unreadOverrides.remove(c.guid);
                stateChanged = true;
            }
            if (m_chatMessagesCache.contains(c.guid)) {
                auto &chatMap = m_chatMessagesCache[c.guid];
                for (auto it = chatMap.begin(); it != chatMap.end(); ++it) {
                    QJsonObject m = it.value();
                    if (!m.value(QStringLiteral("isRead")).toBool()) {
                        m.insert(QStringLiteral("isRead"), true);
                        it.value() = m;
                    }
                }
            }
            if (c.unreadCount > 0) {
                c.unreadCount = 0;
                m_readWatermarks[c.guid] = qMax(m_readWatermarks.value(c.guid, 0), c.lastMessageDate);
                stateChanged = true;
                chatsModified = true;
            }
        } else {
            if (m_chatMessagesCache.contains(c.guid)) {
                auto &chatMap = m_chatMessagesCache[c.guid];
                for (auto it = chatMap.begin(); it != chatMap.end(); ++it) {
                    QJsonObject m = it.value();
                    const bool shouldBeUnread = unreadGuids.contains(it.key());
                    m.insert(QStringLiteral("isRead"), !shouldBeUnread);
                    it.value() = m;
                }
            }
            int count = 0;
            qint64 bestDate = c.lastMessageDate;
            QString bestPreview = c.lastMessagePreview;
            const qint64 wm = m_readWatermarks.value(c.guid, 0);
            const qint64 uo = m_unreadOverrides.value(c.guid, 0);
            const auto &chatMap = m_chatMessagesCache.value(c.guid);
            for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
                const QJsonObject &mObj = it.value();
                if (m_deletedMessageGuids.contains(it.key()) || isMessageDeletedOrRetracted(mObj)) continue;
                if (unreadGuids.contains(it.key()) && isMessageUnread(mObj, wm, uo)) {
                    count++;
                }
                const qint64 dc = mObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                if (dc >= bestDate) {
                    QString preview = extractMessageText(mObj);
                    const QString assocType = mObj.value(QStringLiteral("associatedMessageType")).toVariant().toString();
                    const QString emoji = mapReactionEmoji(assocType, preview);
                    if (!emoji.isEmpty()) {
                        preview = QStringLiteral("%1 Reacted to a message").arg(emoji);
                    }
                    preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
                    if (!preview.isEmpty()) {
                        bestDate = dc;
                        bestPreview = preview;
                    }
                }
            }
            if (c.lastMessageDate != bestDate || c.lastMessagePreview != bestPreview) {
                c.lastMessageDate = bestDate;
                c.lastMessagePreview = bestPreview;
                c.relativeTime = formatRelativeTime(bestDate);
                chatsModified = true;
            }
            if (c.unreadCount != count) {
                c.unreadCount = count;
                chatsModified = true;
            }
        }
    }

    if (!chatsWithNewOrUpdatedMessages.isEmpty()) {
        saveMessagesCache();
    }
    if (stateChanged) {
        saveAppState();
    }
    if (chatsModified) {
        sortChats();
        emit chatsChanged();
        if (!m_selectedChatGuid.isEmpty()) {
            emit selectedChatChanged();
        }
    }
    if (selectedChatUpdated && !m_selectedChatGuid.isEmpty()) {
        rebuildSelectedChatMessages();
    }
}

bool Backend::isMessageUnread(const QJsonObject &m, qint64 watermark, qint64 unreadOverrideTs) {
    const bool isFromMe = m.value(QStringLiteral("isFromMe")).toBool();
    if (isFromMe) return false;

    const int itemType = m.value(QStringLiteral("itemType")).toInt(0);
    if (itemType != 0) return false;

    const QString assocType = m.value(QStringLiteral("associatedMessageType")).toVariant().toString();
    if (!assocType.isEmpty() && assocType != QStringLiteral("0")) return false;

    const qint64 dateCreated = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
    if (watermark > 0 && dateCreated <= watermark) return false;

    const qint64 dateRead = m.value(QStringLiteral("dateRead")).toVariant().toLongLong();
    if (unreadOverrideTs > 0 && dateCreated > watermark) {
        if (dateRead > unreadOverrideTs) return false;
        return true;
    }

    const bool isRead = m.value(QStringLiteral("isRead")).toBool();
    if (isRead) return false;

    if (dateRead > 0) return false;

    return true;
}

void Backend::pollDelta() {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;

    ++m_pollTickCount;
    if (m_pollTickCount % 4 == 0) {
        fetchUnreadStateFromServer();
    }
    if (m_pollTickCount % 24 == 0) {
        fetchChatsMetadata();
    }
    if (m_pollTickCount % 48 == 0) {
        if (!m_selectedChatGuid.isEmpty() && !m_activeMessagesReply && !m_loadingOlderMessages) {
            loadChatMessages(m_selectedChatGuid);
        }
    }
    if (!m_pendingDeliveryMessages.isEmpty()) {
        checkPendingDeliveryStatuses();
    } else if (!m_selectedChatGuid.isEmpty()) {
        const qint64 nowCheckMs = QDateTime::currentMSecsSinceEpoch();
        bool hasStaleSending = false;
        for (const QVariant &v : m_messages) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("receiptText")).toString() == QStringLiteral("Sending...")) {
                const QString g = m.value(QStringLiteral("guid")).toString();
                const qint64 dc = m.value(QStringLiteral("dateCreated")).toLongLong();
                const qint64 effectiveStartMs = (dc > 0) ? dc : m_pendingDeliveryAddedAtMs.value(g, 0);
                if (!g.startsWith(QStringLiteral("temp-")) && !g.startsWith(QStringLiteral("bp-")) &&
                    (m_expiredPendingDeliveryGuids.contains(g) ||
                     (effectiveStartMs > 0 && (nowCheckMs - effectiveStartMs) > 120000LL))) {
                    hasStaleSending = true;
                    break;
                }
            }
        }
        if (hasStaleSending) {
            rebuildSelectedChatMessages();
        }
    }

    // Use a 1-hour (3,600,000 ms) lookback window relative to min(nowMs, m_lastMessageTimestamp)
    // because Apple chat.db stores sender/iPhone creation timestamp (message.date) rather than
    // Mac insertion time; SMS/iCloud relay messages can arrive minutes after creation.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 baseMs = (m_lastMessageTimestamp > 0)
                              ? qMin(nowMs, m_lastMessageTimestamp)
                              : nowMs;
    const qint64 afterMs = baseMs - 3600000LL;

    QJsonObject body;
    body.insert(QStringLiteral("limit"), 100);
    body.insert(QStringLiteral("offset"), 0);
    body.insert(QStringLiteral("after"), afterMs);
    body.insert(QStringLiteral("sort"), QStringLiteral("DESC"));
    body.insert(QStringLiteral("with"), QJsonArray{
        QStringLiteral("chat"),
        QStringLiteral("handle"),
        QStringLiteral("attachment"),
        QStringLiteral("attributedbody")
    });

    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkFailure(QStringLiteral("pollDelta"));
            return;
        }
        handleNetworkSuccess();
        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
        processIncomingMessages(arr, false);
    });

    // Also poll by SQLite ROWID > m_highestMessageRowId so delayed SMS or CloudKit
    // (Messages in iCloud) backfills with creation timestamps older than 1 hour are
    // ingested immediately without waiting for an app restart.
    if (m_highestMessageRowId > 0 && (m_pollTickCount % 2 == 0)) {
        QJsonObject whereClause;
        whereClause.insert(QStringLiteral("statement"), QStringLiteral("message.ROWID > :minRowId"));
        QJsonObject argsObj;
        argsObj.insert(QStringLiteral("minRowId"), m_highestMessageRowId);
        whereClause.insert(QStringLiteral("args"), argsObj);

        QJsonObject rowBody;
        rowBody.insert(QStringLiteral("limit"), 100);
        rowBody.insert(QStringLiteral("offset"), 0);
        rowBody.insert(QStringLiteral("sort"), QStringLiteral("DESC"));
        rowBody.insert(QStringLiteral("where"), QJsonArray{whereClause});
        rowBody.insert(QStringLiteral("with"), QJsonArray{
            QStringLiteral("chat"),
            QStringLiteral("handle"),
            QStringLiteral("attachment"),
            QStringLiteral("attributedbody")
        });

        QNetworkReply *rowReply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                             QJsonDocument(rowBody).toJson(QJsonDocument::Compact));
        connect(rowReply, &QNetworkReply::finished, this, [this, rowReply]() {
            rowReply->deleteLater();
            if (rowReply->error() != QNetworkReply::NoError) return;
            const QJsonArray arr = QJsonDocument::fromJson(rowReply->readAll()).object().value(QStringLiteral("data")).toArray();
            if (!arr.isEmpty()) {
                processIncomingMessages(arr, false);
                saveMessagesCache();
            }
        });
    }
}

void Backend::ingestMessagesForTest(const QJsonArray &messagesArray, bool isInitialLoad) {
    processIncomingMessages(messagesArray, isInitialLoad);
    if (!m_stateFilePath.isEmpty()) {
        flushMessagesCacheNow(false);
    }
}

bool Backend::isSameConversation(const QString &guidA, const QString &guidB) const {
    const QString a = guidA.trimmed();
    const QString b = guidB.trimmed();
    if (a.isEmpty() || b.isEmpty()) return false;
    if (a == b) return true;

    const QString canonA = resolveCanonicalChatGuid(a);
    const QString canonB = resolveCanonicalChatGuid(b);
    if (canonA == canonB) return true;

    if (m_chatSiblingGuids.value(canonA).contains(b) || m_chatSiblingGuids.value(canonA).contains(canonB) ||
        m_chatSiblingGuids.value(canonB).contains(a) || m_chatSiblingGuids.value(canonB).contains(canonA)) {
        return true;
    }

    const QString ogA = m_chatOriginalGroupIds.value(canonA, m_chatOriginalGroupIds.value(a));
    const QString ogB = m_chatOriginalGroupIds.value(canonB, m_chatOriginalGroupIds.value(b));
    if (!ogA.isEmpty() && ogA == ogB) return true;

    const QString gA = m_chatGroupIds.value(canonA, m_chatGroupIds.value(a));
    const QString gB = m_chatGroupIds.value(canonB, m_chatGroupIds.value(b));
    if (!gA.isEmpty() && gA == gB) return true;

    return false;
}

QString Backend::resolveCanonicalChatGuid(const QString &chatGuid) const {
    QString cur = chatGuid.trimmed();
    if (cur.isEmpty()) return cur;
    for (int hop = 0; hop < 8; ++hop) {
        const QString next = m_canonicalChatAliases.value(cur);
        if (next.isEmpty() || next == cur) {
            break;
        }
        cur = next;
    }
    return cur;
}

void Backend::recordChatGroupMetadata(const QJsonObject &chatObj) {
    const QString rawGuid = chatObj.value(QStringLiteral("guid")).toString().trimmed();
    if (rawGuid.isEmpty()) return;

    const QString disp = chatObj.value(QStringLiteral("displayName")).toString().trimmed();
    if (!disp.isEmpty()) {
        m_chatDisplayNames.insert(rawGuid, disp);
    }
    if (chatObj.contains(QStringLiteral("style"))) {
        const int st = chatObj.value(QStringLiteral("style")).toInt(0);
        if (st > 0) m_chatStyles.insert(rawGuid, st);
    }

    QString origGrpId = chatObj.value(QStringLiteral("originalGroupId")).toString().trimmed();
    if (origGrpId.isEmpty()) {
        origGrpId = chatObj.value(QStringLiteral("original_group_id")).toString().trimmed();
    }
    if (!origGrpId.isEmpty()) {
        m_chatOriginalGroupIds.insert(rawGuid, origGrpId);
    }

    QString grpId = chatObj.value(QStringLiteral("groupId")).toString().trimmed();
    if (grpId.isEmpty()) {
        grpId = chatObj.value(QStringLiteral("group_id")).toString().trimmed();
    }
    if (!grpId.isEmpty()) {
        m_chatGroupIds.insert(rawGuid, grpId);
    }

    const QJsonArray parts = chatObj.value(QStringLiteral("participants")).toArray();
    if (!parts.isEmpty()) {
        QStringList handles;
        for (const QJsonValue &pv : parts) {
            const QString addr = pv.toObject().value(QStringLiteral("address")).toString().trimmed();
            if (!addr.isEmpty()) handles.append(addr);
        }
        if (!handles.isEmpty()) {
            m_chatParticipants.insert(rawGuid, handles);
        }
    }

    const QString activeTarget = chatObj.value(QStringLiteral("activeSendChatGuid")).toString().trimmed();
    if (!activeTarget.isEmpty()) {
        m_chatPreferredSendGuids.insert(rawGuid, activeTarget);
    }

    QStringList sibs = m_chatSiblingGuids.value(rawGuid);
    if (!sibs.contains(rawGuid)) sibs.append(rawGuid);
    const QJsonArray sibArr = chatObj.value(QStringLiteral("siblingChatGuids")).toArray();
    for (const QJsonValue &sv : sibArr) {
        const QString sg = sv.toString().trimmed();
        if (!sg.isEmpty() && !sibs.contains(sg)) {
            sibs.append(sg);
            if (!origGrpId.isEmpty() && m_chatOriginalGroupIds.value(sg).isEmpty()) {
                m_chatOriginalGroupIds.insert(sg, origGrpId);
            }
            if (!grpId.isEmpty() && m_chatGroupIds.value(sg).isEmpty()) {
                m_chatGroupIds.insert(sg, grpId);
            }
        }
    }
    m_chatSiblingGuids.insert(rawGuid, sibs);
}

QString Backend::computeGroupMergeKey(const ChatEntry &entry) const {
    const QString g = entry.guid.trimmed();
    if (g.isEmpty()) return QString();
    const bool isGrp = entry.isGroup
                       || g.contains(QStringLiteral(";+;"))
                       || (m_chatStyles.value(g, 45) == 43)
                       || entry.chatIdentifier.startsWith(QStringLiteral("chat"));
    if (!isGrp) {
        return QString();
    }

    QString origGrpId = entry.originalGroupId.trimmed();
    if (origGrpId.isEmpty()) origGrpId = m_chatOriginalGroupIds.value(g).trimmed();
    if (!origGrpId.isEmpty()) {
        return QStringLiteral("ogid:") + origGrpId;
    }

    QString grpId = entry.groupId.trimmed();
    if (grpId.isEmpty()) grpId = m_chatGroupIds.value(g).trimmed();
    if (!grpId.isEmpty()) {
        return QStringLiteral("gid:") + grpId;
    }

    return QString();
}

QString Backend::resolveActiveSendChatGuid(const QString &chatGuid) const {
    const QString trimmed = chatGuid.trimmed();
    if (trimmed.isEmpty()) return trimmed;
    const QString canon = resolveCanonicalChatGuid(trimmed);

    QStringList candidates;
    auto addCand = [&candidates](const QString &cand) {
        const QString c = cand.trimmed();
        if (!c.isEmpty() && !candidates.contains(c)) candidates.append(c);
    };

    const QString pref = m_chatPreferredSendGuids.value(canon).trimmed();
    addCand(pref);
    addCand(canon);
    for (const QString &sg : m_chatSiblingGuids.value(canon)) addCand(sg);
    for (const ChatEntry &ce : m_chats) {
        if (ce.guid == canon || ce.guid == trimmed || ce.siblingGuids.contains(canon) || ce.siblingGuids.contains(trimmed)) {
            addCand(ce.guid);
            for (const QString &sg : ce.siblingGuids) addCand(sg);
            break;
        }
    }
    if (candidates.size() <= 1) {
        return pref.isEmpty() ? canon : pref;
    }

    struct CandStats {
        qint64 latestAnyTs = 0;
        qint64 latestOkTs = 0;
        qint64 latestErrTs = 0;
        int svcRank = 1;
        bool isPref = false;
    };
    QHash<QString, CandStats> stats;
    for (const QString &cand : candidates) {
        CandStats cs;
        cs.svcRank = cand.startsWith(QStringLiteral("RCS;")) ? 3 : (cand.startsWith(QStringLiteral("iMessage;")) ? 2 : 1);
        cs.isPref = (cand == pref);
        stats.insert(cand, cs);
    }

    auto inspectCache = [&](const QHash<QString, QJsonObject> &msgMap) {
        for (auto it = msgMap.constBegin(); it != msgMap.constEnd(); ++it) {
            const QJsonObject &m = it.value();
            QString origCg = m.value(QStringLiteral("originalChatGuid")).toString().trimmed();
            if (origCg.isEmpty()) {
                const QJsonArray cArr = m.value(QStringLiteral("chats")).toArray();
                if (!cArr.isEmpty()) origCg = cArr.first().toObject().value(QStringLiteral("guid")).toString().trimmed();
            }
            if (origCg.isEmpty() || !stats.contains(origCg)) continue;
            CandStats &cs = stats[origCg];
            const qint64 dc = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
            const bool fromMe = m.value(QStringLiteral("isFromMe")).toBool();
            const int err = m.value(QStringLiteral("error")).toInt(0);
            cs.latestAnyTs = qMax(cs.latestAnyTs, dc);
            if (fromMe && err != 0) {
                cs.latestErrTs = qMax(cs.latestErrTs, dc);
            } else {
                cs.latestOkTs = qMax(cs.latestOkTs, dc);
            }
        }
    };
    for (const QString &cand : candidates) {
        if (m_chatMessagesCache.contains(cand)) {
            inspectCache(m_chatMessagesCache.value(cand));
        }
    }

    QString bestCand = candidates.first();
    auto betterThan = [&](const QString &a, const QString &b) -> bool {
        const CandStats &sa = stats[a];
        const CandStats &sb = stats[b];
        const bool aHealthy = (sa.latestErrTs == 0) || (sa.latestOkTs >= sa.latestErrTs);
        const bool bHealthy = (sb.latestErrTs == 0) || (sb.latestOkTs >= sb.latestErrTs);
        if (aHealthy != bHealthy) return aHealthy;
        if (sa.isPref != sb.isPref && sa.isPref && (sb.latestOkTs <= sa.latestAnyTs || sa.latestAnyTs == 0)) return true;
        if (sa.isPref != sb.isPref && sb.isPref && (sa.latestOkTs <= sb.latestAnyTs || sb.latestAnyTs == 0)) return false;
        if (sa.latestOkTs != sb.latestOkTs) return sa.latestOkTs > sb.latestOkTs;
        if (sa.svcRank != sb.svcRank) return sa.svcRank > sb.svcRank;
        return sa.latestAnyTs > sb.latestAnyTs;
    };
    for (int i = 1; i < candidates.size(); ++i) {
        if (betterThan(candidates[i], bestCand)) {
            bestCand = candidates[i];
        }
    }
    return bestCand;
}

void Backend::unifySiblingChats(bool *selectedMigrated, bool *chatsMerged) {
    if (selectedMigrated) *selectedMigrated = false;
    if (chatsMerged) *chatsMerged = false;
    const int n = static_cast<int>(m_chats.size());
    if (n == 0) return;

    QList<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    auto findRoot = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        const int ra = findRoot(a);
        const int rb = findRoot(b);
        if (ra != rb) parent[rb] = ra;
    };

    QHash<QString, int> keyOwner;
    for (int i = 0; i < n; ++i) {
        const QString mk = computeGroupMergeKey(m_chats[i]);
        if (!mk.isEmpty()) {
            if (keyOwner.contains(mk)) {
                unite(keyOwner.value(mk), i);
            } else {
                keyOwner.insert(mk, i);
            }
        }
        const QString gi = m_chats[i].guid;
        for (int j = i + 1; j < n; ++j) {
            const QString gj = m_chats[j].guid;
            if (m_chats[i].siblingGuids.contains(gj) || m_chats[j].siblingGuids.contains(gi) ||
                m_chatSiblingGuids.value(gi).contains(gj) || m_chatSiblingGuids.value(gj).contains(gi)) {
                unite(i, j);
            }
        }
    }

    // Fallback: unify unkeyed RCS/SMS group chats sharing non-empty displayName and exact normalized participant set
    std::vector<QSet<QString>> normSets(n);
    for (int i = 0; i < n; ++i) {
        QSet<QString> s;
        for (const QString &h : m_chats[i].participantHandles) {
            const QString nh = normalizeHandle(h);
            if (!nh.isEmpty()) s.insert(nh);
        }
        normSets[i] = s;
    }
    for (int i = 0; i < n; ++i) {
        const QString dispI = m_chats[i].displayName.trimmed();
        const bool isRcsSmsI = m_chats[i].guid.startsWith(QStringLiteral("RCS;+;")) || m_chats[i].guid.startsWith(QStringLiteral("SMS;+;"));
        if (!isRcsSmsI || dispI.isEmpty() || normSets[i].size() < 2) continue;
        if (!computeGroupMergeKey(m_chats[i]).isEmpty()) continue;
        for (int j = i + 1; j < n; ++j) {
            if (findRoot(i) == findRoot(j)) continue;
            const QString dispJ = m_chats[j].displayName.trimmed();
            const bool isRcsSmsJ = m_chats[j].guid.startsWith(QStringLiteral("RCS;+;")) || m_chats[j].guid.startsWith(QStringLiteral("SMS;+;"));
            if (!isRcsSmsJ || dispI.compare(dispJ, Qt::CaseInsensitive) != 0 || normSets[j].size() < 2) continue;
            if (!computeGroupMergeKey(m_chats[j]).isEmpty()) continue;
            if (normSets[i] == normSets[j]) {
                unite(i, j);
            }
        }
    }

    QHash<int, QList<int>> buckets;
    QList<int> bucketOrder;
    for (int i = 0; i < n; ++i) {
        const int r = findRoot(i);
        if (!buckets.contains(r)) bucketOrder.append(r);
        buckets[r].append(i);
    }

    auto chatScore = [this](const ChatEntry &c) -> std::tuple<int, int, qint64, qint64, int> {
        const int archivedScore = c.isArchived ? 0 : 1;
        const int svcScore = c.guid.startsWith(QStringLiteral("RCS;")) ? 3 : (c.guid.startsWith(QStringLiteral("iMessage;")) ? 2 : 1);
        const qint64 dateScore = c.lastMessageDate;
        const qint64 rowScore = c.originalRowId;
        const int selectedTie = (c.guid == m_selectedChatGuid) ? 1 : 0;
        return std::make_tuple(archivedScore, svcScore, dateScore, rowScore, selectedTie);
    };

    QList<ChatEntry> mergedList;
    mergedList.reserve(bucketOrder.size());

    for (int root : bucketOrder) {
        const QList<int> &idxs = buckets[root];
        int bestPos = idxs.first();
        for (int idx : idxs) {
            if (chatScore(m_chats[idx]) > chatScore(m_chats[bestPos])) {
                bestPos = idx;
            }
        }

        ChatEntry winner = m_chats[bestPos];
        QStringList allSibs = winner.siblingGuids;
        if (!allSibs.contains(winner.guid)) allSibs.append(winner.guid);
        for (const QString &sg : m_chatSiblingGuids.value(winner.guid)) {
            if (!sg.isEmpty() && !allSibs.contains(sg)) allSibs.append(sg);
        }

        QSet<QString> seenNormHandles;
        QStringList mergedHandles = winner.participantHandles;
        for (const QString &h : mergedHandles) {
            seenNormHandles.insert(normalizeHandle(h));
        }

        bool bucketHadPinned = m_pinnedChats.contains(winner.guid);
        bool bucketHadMuted = m_mutedChats.contains(winner.guid);
        bool anyCacheMoved = false;

        for (int idx : idxs) {
            const ChatEntry &other = m_chats[idx];
            if (!other.guid.isEmpty() && !allSibs.contains(other.guid)) allSibs.append(other.guid);
            for (const QString &sg : other.siblingGuids) {
                if (!sg.isEmpty() && !allSibs.contains(sg)) allSibs.append(sg);
            }
            for (const QString &sg : m_chatSiblingGuids.value(other.guid)) {
                if (!sg.isEmpty() && !allSibs.contains(sg)) allSibs.append(sg);
            }
            if (winner.originalGroupId.isEmpty() && !other.originalGroupId.isEmpty()) winner.originalGroupId = other.originalGroupId;
            if (winner.groupId.isEmpty() && !other.groupId.isEmpty()) winner.groupId = other.groupId;
            if (winner.displayName.isEmpty() && !other.displayName.isEmpty()) winner.displayName = other.displayName;
            if (other.originalRowId > winner.originalRowId) winner.originalRowId = other.originalRowId;
            if (!other.isArchived) winner.isArchived = false;
            if (m_pinnedChats.contains(other.guid)) bucketHadPinned = true;
            if (m_mutedChats.contains(other.guid)) bucketHadMuted = true;
            for (const QString &h : other.participantHandles) {
                const QString nh = normalizeHandle(h);
                if (!nh.isEmpty() && !seenNormHandles.contains(nh)) {
                    seenNormHandles.insert(nh);
                    mergedHandles.append(h);
                }
            }
            if (other.lastMessageDate > winner.lastMessageDate && !other.lastMessagePreview.isEmpty()) {
                winner.lastMessageDate = other.lastMessageDate;
                winner.lastMessagePreview = other.lastMessagePreview;
                winner.relativeTime = other.relativeTime;
            }
        }

        if (idxs.size() > 1 && chatsMerged) {
            *chatsMerged = true;
        }
        winner.siblingGuids = allSibs;
        winner.participantHandles = mergedHandles;
        if (!winner.originalGroupId.isEmpty()) m_chatOriginalGroupIds.insert(winner.guid, winner.originalGroupId);
        if (!winner.groupId.isEmpty()) m_chatGroupIds.insert(winner.guid, winner.groupId);
        if (!winner.displayName.isEmpty()) m_chatDisplayNames.insert(winner.guid, winner.displayName);
        if (!mergedHandles.isEmpty()) m_chatParticipants.insert(winner.guid, mergedHandles);
        m_chatSiblingGuids.insert(winner.guid, allSibs);

        if (bucketHadPinned && !m_pinnedChats.contains(winner.guid)) m_pinnedChats.append(winner.guid);
        if (bucketHadMuted) m_mutedChats.insert(winner.guid);

        for (const QString &sib : allSibs) {
            if (sib.isEmpty() || sib == winner.guid) continue;
            m_canonicalChatAliases.insert(sib, winner.guid);
            if (m_readWatermarks.contains(sib)) {
                const qint64 sibWm = m_readWatermarks.take(sib);
                m_readWatermarks[winner.guid] = qMax(m_readWatermarks.value(winner.guid, 0), sibWm);
            }
            if (m_unreadOverrides.contains(sib)) {
                const qint64 sibUnread = m_unreadOverrides.take(sib);
                m_unreadOverrides[winner.guid] = qMax(m_unreadOverrides.value(winner.guid, 0), sibUnread);
            }
            if (m_localReadMutationTimeMs.contains(sib)) {
                const qint64 sibMut = m_localReadMutationTimeMs.take(sib);
                m_localReadMutationTimeMs[winner.guid] = qMax(m_localReadMutationTimeMs.value(winner.guid, 0), sibMut);
            }
            if (m_chatMessagesCache.contains(sib)) {
                const auto sibMsgs = m_chatMessagesCache.take(sib);
                for (auto it = sibMsgs.constBegin(); it != sibMsgs.constEnd(); ++it) {
                    QJsonObject moved = it.value();
                    if (moved.value(QStringLiteral("originalChatGuid")).toString().isEmpty()) {
                        moved.insert(QStringLiteral("originalChatGuid"), sib);
                    }
                    cacheMessageForChat(winner.guid, moved);
                }
                anyCacheMoved = true;
            }
            if (sib == m_selectedChatGuid) {
                m_selectedChatGuid = winner.guid;
                if (selectedMigrated) *selectedMigrated = true;
            }
        }

        // Moving sibling messages through cacheMessageForChat() refined the
        // service on the m_chats entry, not on this copy; carry it over.
        winner.service = m_chats[bestPos].service;

        if (anyCacheMoved || idxs.size() > 1) {
            qint64 bestDate = winner.lastMessageDate;
            QString bestPreview = winner.lastMessagePreview;
            int unreadCount = 0;
            const qint64 wm = m_readWatermarks.value(winner.guid, 0);
            const qint64 uo = m_unreadOverrides.value(winner.guid, 0);
            if (m_chatMessagesCache.contains(winner.guid)) {
                const auto &chatMap = m_chatMessagesCache.value(winner.guid);
                for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
                    const QJsonObject &m = it.value();
                    if (m_deletedMessageGuids.contains(it.key()) || isMessageDeletedOrRetracted(m)) continue;
                    if (isMessageUnread(m, wm, uo)) ++unreadCount;
                    const qint64 dc = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                    QString prev = extractMessageText(m);
                    const QString emoji = mapReactionEmoji(m.value(QStringLiteral("associatedMessageType")).toVariant().toString(), prev);
                    if (!emoji.isEmpty()) prev = QStringLiteral("%1 Reacted to a message").arg(emoji);
                    prev.replace(QLatin1Char('\n'), QLatin1Char(' '));
                    if (!prev.isEmpty() && dc >= bestDate) {
                        bestDate = dc;
                        bestPreview = prev;
                    }
                }
            }
            winner.unreadCount = unreadCount;
            if (bestDate > 1 && !bestPreview.isEmpty()) {
                winner.lastMessageDate = bestDate;
                winner.lastMessagePreview = bestPreview;
                winner.relativeTime = formatRelativeTime(bestDate);
            }
        }

        rebuildChatPresentation(winner);
        mergedList.append(winner);
    }

    if (chatsMerged && *chatsMerged) {
        m_chats = mergedList;
    } else if (mergedList.size() == m_chats.size()) {
        m_chats = mergedList;
    }
}

void Backend::addPendingDeliveryForTest(const QString &msgGuid, const QString &chatGuid, qint64 addedAtMs) {
    if (msgGuid.isEmpty()) return;
    const qint64 ts = (addedAtMs > 0) ? addedAtMs : QDateTime::currentMSecsSinceEpoch();
    m_pendingDeliveryMessages.insert(msgGuid, chatGuid);
    m_pendingDeliveryAddedAtMs.insert(msgGuid, ts);
    m_expiredPendingDeliveryGuids.remove(msgGuid);
}

void Backend::agePendingDeliveryForTest(const QString &msgGuid, qint64 ageMs) {
    if (msgGuid.isEmpty()) return;
    const qint64 agedTs = QDateTime::currentMSecsSinceEpoch() - ageMs;
    if (m_pendingDeliveryAddedAtMs.contains(msgGuid)) {
        m_pendingDeliveryAddedAtMs.insert(msgGuid, agedTs);
    }
    const QString chatGuid = resolveCanonicalChatGuid(m_pendingDeliveryMessages.value(msgGuid));
    if (!chatGuid.isEmpty() && m_chatMessagesCache.contains(chatGuid) &&
        m_chatMessagesCache[chatGuid].contains(msgGuid)) {
        m_chatMessagesCache[chatGuid][msgGuid].insert(QStringLiteral("dateCreated"), agedTs);
    }
}

void Backend::clearPendingDeliveryTrackingForGuid(const QString &msgGuid, bool clearExpired) {
    if (msgGuid.isEmpty()) return;
    m_pendingDeliveryMessages.remove(msgGuid);
    m_pendingDeliveryAddedAtMs.remove(msgGuid);
    m_inFlightDeliveryCheckGuids.remove(msgGuid);
    m_messageObservationSeq.remove(msgGuid);
    if (clearExpired) {
        m_expiredPendingDeliveryGuids.remove(msgGuid);
    }
}

void Backend::bumpServerEndpointGeneration() {
    ++m_serverEndpointGeneration;
    m_supportsBatchMessageGuidInQuery = true;
    m_inFlightDeliveryCheckGuids.clear();
}

static bool isTapbackOrAssociatedMessage(const QJsonObject &m) {
    const QString assocType = m.value(QStringLiteral("associatedMessageType")).toVariant().toString();
    const int assocTypeInt = m.value(QStringLiteral("associatedMessageType")).toInt(0);
    const QString assocGuid = m.value(QStringLiteral("associatedMessageGuid")).toString().trimmed();
    const QString assocEmoji = m.value(QStringLiteral("associatedMessageEmoji")).toString().trimmed();
    if (assocTypeInt >= 2000 || !assocGuid.isEmpty() || !assocEmoji.isEmpty()) {
        return true;
    }
    return !Backend::mapReactionEmoji(assocType, Backend::extractMessageText(m)).isEmpty();
}

static bool isHiddenTimelinePlaceholder(const QJsonObject &m) {
    if (!m.value(QStringLiteral("attachments")).toArray().isEmpty()) {
        return false;
    }
    const QString trimmedText = Backend::extractMessageText(m).trimmed();
    return trimmedText.isEmpty() || trimmedText == QStringLiteral("Replied to a message:");
}

static bool hasEligibleLaterIncomingReply(const QHash<QString, QJsonObject> &chatMap, qint64 outgoingDateCreated) {
    if (outgoingDateCreated <= 0) return false;
    for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
        const QJsonObject &other = it.value();
        if (other.value(QStringLiteral("isFromMe")).toBool()) continue;
        if (Backend::isMessageDeletedOrRetracted(other)) continue;
        if (isTapbackOrAssociatedMessage(other)) continue;
        if (isHiddenTimelinePlaceholder(other)) continue;
        if (other.value(QStringLiteral("dateCreated")).toVariant().toLongLong() > outgoingDateCreated) {
            return true;
        }
    }
    return false;
}

bool Backend::filterPendingDeliveryResponseItems(
    const QJsonArray &arr,
    const QSet<QString> &batchSet,
    const QHash<QString, quint64> &observedSeqAtDispatch,
    quint64 reconcileSeqAtDispatch,
    QJsonArray *outNonStale,
    int *outMatchedCount) const {
    if (outMatchedCount) *outMatchedCount = 0;
    if (!outNonStale) return false;

    for (const QJsonValue &v : arr) {
        QJsonObject obj = v.toObject();
        const QString g = obj.value(QStringLiteral("guid")).toString();
        if (g.isEmpty() || !batchSet.contains(g)) {
            continue;
        }
        if (outMatchedCount) {
            ++(*outMatchedCount);
        }
        if (m_reconcileRemovedSeqByGuid.value(g, 0) > reconcileSeqAtDispatch) {
            continue;
        }
        const qint64 batchDelivTs = obj.value(QStringLiteral("dateDelivered")).toVariant().toLongLong();
        const qint64 batchReadTs = obj.value(QStringLiteral("dateRead")).toVariant().toLongLong();
        const bool batchHasError = (obj.value(QStringLiteral("error")).toInt(0) != 0);
        const bool batchDeliveredOrRead =
            obj.value(QStringLiteral("isDelivered")).toBool(false) ||
            (batchDelivTs > 0) ||
            (batchReadTs > 0);
        const bool batchHasTerminalState = batchDeliveredOrRead || batchHasError;
        QString knownCg = m_pendingDeliveryMessages.value(g);
        QJsonObject cachedEntry;
        const QString resolvedKnownChat = resolveCanonicalChatGuid(knownCg);
        if (!resolvedKnownChat.isEmpty() && m_chatMessagesCache.contains(resolvedKnownChat) &&
            m_chatMessagesCache.value(resolvedKnownChat).contains(g)) {
            knownCg = resolvedKnownChat;
            cachedEntry = m_chatMessagesCache.value(resolvedKnownChat).value(g);
        } else {
            for (auto chatIt = m_chatMessagesCache.constBegin(); chatIt != m_chatMessagesCache.constEnd(); ++chatIt) {
                if (chatIt.value().contains(g)) {
                    if (knownCg.isEmpty()) {
                        knownCg = chatIt.key();
                    }
                    cachedEntry = chatIt.value().value(g);
                    break;
                }
            }
        }
        if (observedSeqAtDispatch.contains(g) &&
            m_messageObservationSeq.value(g, 0) > observedSeqAtDispatch.value(g, 0)) {
            const qint64 cachedDelivTs = cachedEntry.value(QStringLiteral("dateDelivered")).toVariant().toLongLong();
            const qint64 cachedReadTs = cachedEntry.value(QStringLiteral("dateRead")).toVariant().toLongLong();
            const bool cachedDeliveredOrRead =
                cachedEntry.value(QStringLiteral("isDelivered")).toBool(false) ||
                (cachedDelivTs > 0) ||
                (cachedReadTs > 0);
            const bool cachedHasError = (cachedEntry.value(QStringLiteral("error")).toInt(0) != 0);
            const bool batchHasNewerRead = (batchReadTs > cachedReadTs);
            const bool batchHasNewerDelivery = (cachedDelivTs > 0) && (batchDelivTs > cachedDelivTs);
            const bool batchAdvancesOverCachedError = batchHasNewerRead || batchHasNewerDelivery;
            if (!batchHasTerminalState ||
                (cachedDeliveredOrRead && !cachedHasError && (batchHasError || !batchDeliveredOrRead)) ||
                (cachedHasError && !batchHasError && !batchAdvancesOverCachedError)) {
                // Newer state was already observed while in flight; never let stale error or older positive GUID-query receipt overwrite newer state
                continue;
            }
        }
        if (obj.value(QStringLiteral("chats")).toArray().isEmpty() && !knownCg.isEmpty()) {
            obj.insert(QStringLiteral("chats"), QJsonArray{QJsonObject{{QStringLiteral("guid"), knownCg}}});
        }
        outNonStale->append(obj);
    }
    return true;
}

void Backend::dispatchPendingDeliverySingleQueries(
    const QStringList &guids,
    quint64 genAtDispatch,
    const QString &endpointAtDispatch,
    const QHash<QString, quint64> &observedSeqAtDispatch,
    quint64 reconcileSeqAtDispatch) {
    if (guids.isEmpty()) return;
    auto remaining = std::make_shared<int>(guids.size());
    for (const QString &singleGuid : guids) {
        m_inFlightDeliveryCheckGuids.insert(singleGuid);
        QJsonObject whereClause;
        whereClause.insert(QStringLiteral("statement"), QStringLiteral("message.guid = :g0"));
        whereClause.insert(QStringLiteral("args"), QJsonObject{{QStringLiteral("g0"), singleGuid}});

        QJsonObject body;
        body.insert(QStringLiteral("limit"), 1);
        body.insert(QStringLiteral("offset"), 0);
        body.insert(QStringLiteral("where"), QJsonArray{whereClause});
        body.insert(QStringLiteral("with"), QJsonArray{QStringLiteral("chat"), QStringLiteral("handle")});

        QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, singleGuid, genAtDispatch, endpointAtDispatch, observedSeqAtDispatch, reconcileSeqAtDispatch, remaining]() {
            reply->deleteLater();
            if (genAtDispatch != m_serverEndpointGeneration || endpointAtDispatch != m_serverUrl) {
                return;
            }
            m_inFlightDeliveryCheckGuids.remove(singleGuid);
            if (reply->error() == QNetworkReply::NoError) {
                const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                if (doc.isObject() && doc.object().value(QStringLiteral("data")).isArray()) {
                    const QJsonArray arr = doc.object().value(QStringLiteral("data")).toArray();
                    QJsonArray nonStaleArr;
                    int matchedCount = 0;
                    filterPendingDeliveryResponseItems(arr, QSet<QString>{singleGuid}, observedSeqAtDispatch,
                                                       reconcileSeqAtDispatch, &nonStaleArr, &matchedCount);
                    if (!nonStaleArr.isEmpty()) {
                        processIncomingMessages(nonStaleArr, false);
                        saveMessagesCache();
                    }
                }
            }
            if (--(*remaining) == 0) {
                ++m_pendingDeliveryCheckCompletedCountForTest;
                emit pendingDeliveryCheckFinishedForTest();
            }
        });
    }
}

void Backend::checkPendingDeliveryStatuses() {
    if (m_pendingDeliveryMessages.isEmpty()) {
        return;
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool selectedNeedsRebuild = false;
    const QStringList initialGuids = m_pendingDeliveryMessages.keys();
    for (const QString &msgGuid : initialGuids) {
        if (msgGuid.isEmpty()) {
            m_pendingDeliveryMessages.remove(msgGuid);
            m_pendingDeliveryAddedAtMs.remove(msgGuid);
            m_inFlightDeliveryCheckGuids.remove(msgGuid);
            continue;
        }
        const qint64 addedAtMs = m_pendingDeliveryAddedAtMs.value(msgGuid, nowMs);
        if (!m_pendingDeliveryAddedAtMs.contains(msgGuid)) {
            m_pendingDeliveryAddedAtMs.insert(msgGuid, addedAtMs);
        }
        const QString chatGuid = resolveCanonicalChatGuid(m_pendingDeliveryMessages.value(msgGuid));
        if (chatGuid.isEmpty() || !m_chatMessagesCache.contains(chatGuid) || !m_chatMessagesCache.value(chatGuid).contains(msgGuid)) {
            if ((nowMs - addedAtMs) > 120000LL) {
                clearPendingDeliveryTrackingForGuid(msgGuid, true);
            }
            continue;
        }
        const auto &chatMap = m_chatMessagesCache.value(chatGuid);
        const QJsonObject cachedMsg = chatMap.value(msgGuid);
        const qint64 dc = cachedMsg.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        const int errCode = cachedMsg.value(QStringLiteral("error")).toInt(0);
        const bool isFromMe = cachedMsg.value(QStringLiteral("isFromMe")).toBool();
        const bool isDeliv = cachedMsg.value(QStringLiteral("isDelivered")).toBool(false) ||
                             (cachedMsg.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0);
        const bool isRead = cachedMsg.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0;
        const bool hasLaterIncoming = hasEligibleLaterIncomingReply(chatMap, dc);
        const qint64 effectiveStartMs = (dc > 0) ? dc : addedAtMs;
        const bool expired = (effectiveStartMs > 0 && (nowMs - effectiveStartMs) > 120000LL);
        const bool isSentConfirmed = cachedMsg.contains(QStringLiteral("isSent")) &&
                                     cachedMsg.value(QStringLiteral("isSent")).toBool();
        if (errCode != 0 || isDeliv || isRead || hasLaterIncoming || isSentConfirmed) {
            m_expiredPendingDeliveryGuids.remove(msgGuid);
        }
        if (errCode != 0 || isDeliv || isRead || hasLaterIncoming || expired) {
            m_pendingDeliveryMessages.remove(msgGuid);
            m_pendingDeliveryAddedAtMs.remove(msgGuid);
            m_inFlightDeliveryCheckGuids.remove(msgGuid);
            if (expired) {
                m_messageObservationSeq.remove(msgGuid);
                if (isFromMe && errCode == 0 && !isDeliv && !isRead && !hasLaterIncoming && !isSentConfirmed) {
                    m_expiredPendingDeliveryGuids.insert(msgGuid);
                }
            }
            if (!m_selectedChatGuid.isEmpty() && isSameConversation(chatGuid, m_selectedChatGuid)) {
                selectedNeedsRebuild = true;
            }
        }
    }
    if (selectedNeedsRebuild) {
        rebuildSelectedChatMessages();
    }
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty() || m_pendingDeliveryMessages.isEmpty()) {
        return;
    }

    QStringList sortedCandidates = m_pendingDeliveryMessages.keys();
    sortedCandidates.removeAll(QString());
    std::sort(sortedCandidates.begin(), sortedCandidates.end());
    if (sortedCandidates.isEmpty()) return;

    const int totalCandidates = sortedCandidates.size();
    const int startIdx = (m_pendingDeliveryRotationCursor >= 0)
                             ? (m_pendingDeliveryRotationCursor % totalCandidates)
                             : 0;
    const int maxBatch = m_supportsBatchMessageGuidInQuery ? 20 : 10;
    QStringList batchGuids;
    batchGuids.reserve(qMin(maxBatch, totalCandidates));
    int steps = 0;
    for (; steps < totalCandidates && batchGuids.size() < maxBatch; ++steps) {
        const QString &candidateGuid = sortedCandidates.at((startIdx + steps) % totalCandidates);
        if (candidateGuid.startsWith(QStringLiteral("temp-")) ||
            candidateGuid.startsWith(QStringLiteral("bp-")) ||
            m_inFlightDeliveryCheckGuids.contains(candidateGuid)) {
            continue;
        }
        batchGuids.append(candidateGuid);
    }
    if (totalCandidates > 0) {
        m_pendingDeliveryRotationCursor = (startIdx + steps) % totalCandidates;
    }
    if (batchGuids.isEmpty()) return;

    m_lastPendingDeliveryBatchForTest = batchGuids;
    const quint64 genAtDispatch = m_serverEndpointGeneration;
    const QString endpointAtDispatch = m_serverUrl;
    const quint64 reconcileSeqAtDispatch = m_historyReconcileSeq;
    QHash<QString, quint64> observedSeqAtDispatch;
    observedSeqAtDispatch.reserve(batchGuids.size());
    for (const QString &bg : batchGuids) {
        observedSeqAtDispatch.insert(bg, m_messageObservationSeq.value(bg, 0));
    }

    if (!m_supportsBatchMessageGuidInQuery || batchGuids.size() == 1) {
        dispatchPendingDeliverySingleQueries(batchGuids, genAtDispatch, endpointAtDispatch, observedSeqAtDispatch, reconcileSeqAtDispatch);
        return;
    }

    for (const QString &bg : batchGuids) {
        m_inFlightDeliveryCheckGuids.insert(bg);
    }
    const QSet<QString> batchSet(batchGuids.begin(), batchGuids.end());

    QStringList placeholders;
    QJsonObject argsObj;
    placeholders.reserve(batchGuids.size());
    for (int i = 0; i < batchGuids.size(); ++i) {
        const QString key = QStringLiteral("g%1").arg(i);
        placeholders.append(QLatin1Char(':') + key);
        argsObj.insert(key, batchGuids.at(i));
    }

    QJsonObject whereClause;
    whereClause.insert(QStringLiteral("statement"),
                       QStringLiteral("message.guid IN (%1)").arg(placeholders.join(QLatin1Char(','))));
    whereClause.insert(QStringLiteral("args"), argsObj);

    QJsonObject body;
    body.insert(QStringLiteral("limit"), batchGuids.size());
    body.insert(QStringLiteral("offset"), 0);
    body.insert(QStringLiteral("where"), QJsonArray{whereClause});
    body.insert(QStringLiteral("with"), QJsonArray{QStringLiteral("chat"), QStringLiteral("handle")});

    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, batchGuids, batchSet, genAtDispatch, endpointAtDispatch, observedSeqAtDispatch, reconcileSeqAtDispatch]() {
        reply->deleteLater();
        if (genAtDispatch != m_serverEndpointGeneration || endpointAtDispatch != m_serverUrl) {
            return;
        }
        for (const QString &bg : batchGuids) {
            m_inFlightDeliveryCheckGuids.remove(bg);
        }
        bool needFallback = false;
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray respBytes = reply->readAll();
            QJsonParseError parseErr{};
            const QJsonDocument doc = QJsonDocument::fromJson(respBytes, &parseErr);
            if (parseErr.error == QJsonParseError::NoError && doc.isObject() &&
                doc.object().value(QStringLiteral("data")).isArray()) {
                const QJsonArray arr = doc.object().value(QStringLiteral("data")).toArray();
                QJsonArray nonStaleArr;
                int matchedCount = 0;
                filterPendingDeliveryResponseItems(arr, batchSet, observedSeqAtDispatch, reconcileSeqAtDispatch,
                                                   &nonStaleArr, &matchedCount);
                if (!arr.isEmpty() && matchedCount == 0) {
                    needFallback = true;
                } else {
                    if (!nonStaleArr.isEmpty()) {
                        processIncomingMessages(nonStaleArr, false);
                        saveMessagesCache();
                    }
                    ++m_pendingDeliveryCheckCompletedCountForTest;
                    emit pendingDeliveryCheckFinishedForTest();
                    return;
                }
            }
        } else if (httpStatus == 400 || httpStatus == 404 || httpStatus == 422) {
            QJsonParseError errParse{};
            const QJsonDocument errDoc = QJsonDocument::fromJson(reply->readAll(), &errParse);
            if (errParse.error == QJsonParseError::NoError && errDoc.isObject()) {
                needFallback = true;
            }
        }
        if (needFallback && m_supportsBatchMessageGuidInQuery && batchGuids.size() > 1) {
            m_supportsBatchMessageGuidInQuery = false;
            const QStringList fallbackGuids = batchGuids.mid(0, qMin(10, batchGuids.size()));
            if (!m_demoMode && !m_serverUrl.isEmpty() && !m_password.isEmpty() && !fallbackGuids.isEmpty()) {
                dispatchPendingDeliverySingleQueries(fallbackGuids, genAtDispatch, endpointAtDispatch,
                                                     observedSeqAtDispatch, reconcileSeqAtDispatch);
                return;
            }
        }
        ++m_pendingDeliveryCheckCompletedCountForTest;
        emit pendingDeliveryCheckFinishedForTest();
    });
}

void Backend::processIncomingMessages(const QJsonArray &messagesArray, bool isInitialLoad) {
    bool chatsModified = false;
    bool selectedChatUpdated = false;
    QSet<QString> affectedChatGuids;
    QList<QJsonObject> newIncomingMessages;
    const QString prevSelectedSig = m_selectedChatGuid + QLatin1Char('|') + selectedChatTitle() + QLatin1Char('|') +
                                    selectedChatSubtitle() + QLatin1Char('|') + QString::number(selectedChatUnreadCount()) +
                                    QLatin1Char('|') + selectedChatParticipants().join(QLatin1Char(',')) +
                                    QLatin1Char('|') + selectedChatService();

    for (const QJsonValue &v : messagesArray) {
        QJsonObject m = v.toObject();
        const QString msgGuid = m.value(QStringLiteral("guid")).toString();
        const qint64 dateCreated = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();

        if (!msgGuid.isEmpty() && (m_deletedMessageGuids.contains(msgGuid) || isMessageDeletedOrRetracted(m))) {
            if (isMessageDeletedOrRetracted(m)) {
                m_deletedMessageGuids.insert(msgGuid);
            }
            const QJsonArray cArr = m.value(QStringLiteral("chats")).toArray();
            for (const QJsonValue &cv : cArr) {
                const QString rawCg = cv.toObject().value(QStringLiteral("guid")).toString();
                const QString cg = resolveCanonicalChatGuid(rawCg);
                if (!cg.isEmpty() && m_chatMessagesCache.contains(cg) && m_chatMessagesCache[cg].contains(msgGuid)) {
                    m_chatMessagesCache[cg].remove(msgGuid);
                    refreshChatPreviewFromCache(cg);
                    chatsModified = true;
                    if (cg == m_selectedChatGuid) {
                        selectedChatUpdated = true;
                    }
                }
            }
            clearPendingDeliveryTrackingForGuid(msgGuid, true);
            continue;
        }

        QJsonArray chatsArr = m.value(QStringLiteral("chats")).toArray();
        if (chatsArr.isEmpty()) {
            const QString cacheRoom = m.value(QStringLiteral("cacheRoomnames")).toString().trimmed();
            const QString grpTitle = m.value(QStringLiteral("groupTitle")).toString().trimmed();
            QString svc = m.value(QStringLiteral("service")).toString().trimmed();
            if (svc.isEmpty()) {
                svc = m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("service")).toString().trimmed();
            }
            if (svc.isEmpty()) {
                svc = QStringLiteral("SMS");
            }
            if (!cacheRoom.isEmpty()) {
                QJsonObject synthChat;
                synthChat.insert(QStringLiteral("guid"), QStringLiteral("%1;+;%2").arg(svc, cacheRoom));
                synthChat.insert(QStringLiteral("chatIdentifier"), cacheRoom);
                if (!grpTitle.isEmpty()) synthChat.insert(QStringLiteral("displayName"), grpTitle);
                synthChat.insert(QStringLiteral("style"), 43);
                chatsArr.append(synthChat);
                m.insert(QStringLiteral("chats"), chatsArr);
            } else if (grpTitle.isEmpty()) {
                const QString handleAddr = m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("address")).toString().trimmed();
                if (!handleAddr.isEmpty()) {
                    QJsonObject synthChat;
                    synthChat.insert(QStringLiteral("guid"), QStringLiteral("%1;-;%2").arg(svc, handleAddr));
                    synthChat.insert(QStringLiteral("chatIdentifier"), handleAddr);
                    synthChat.insert(QStringLiteral("style"), 45);
                    chatsArr.append(synthChat);
                    m.insert(QStringLiteral("chats"), chatsArr);
                }
            }
        }
        if (chatsArr.isEmpty()) continue;

        for (const QJsonValue &cv : chatsArr) {
            recordChatGroupMetadata(cv.toObject());
        }

        const qint64 rowId = m.value(QStringLiteral("originalROWID")).toVariant().toLongLong();
        if (rowId > 0 && rowId < 100000000LL && !msgGuid.startsWith(QStringLiteral("temp-")) && !msgGuid.startsWith(QStringLiteral("bp-")) && rowId > m_highestMessageRowId) {
            m_highestMessageRowId = rowId;
        }

        if (dateCreated > m_lastMessageTimestamp) {
            m_lastMessageTimestamp = dateCreated;
        }

        const bool alreadySeen = m_seenMessageGuids.contains(msgGuid);
        if (!msgGuid.isEmpty()) {
            m_seenMessageGuids.insert(msgGuid);
            if (isInitialLoad || (m_autoStart && !m_initialSyncComplete)) {
                m_notifiedMessageGuids.insert(msgGuid);
            }
        }
        if (isInitialLoad && rowId > 0 && rowId < 100000000LL && !msgGuid.startsWith(QStringLiteral("temp-")) && !msgGuid.startsWith(QStringLiteral("bp-"))) {
            m_startupHighestRowId = qMax(m_startupHighestRowId, rowId);
        }

        const int incomingErr = m.value(QStringLiteral("error")).toInt(0);
        const bool incomingDelivered = m.value(QStringLiteral("isDelivered")).toBool(false) || (m.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0);
        const bool incomingFromMe = m.value(QStringLiteral("isFromMe")).toBool(false);
        bool notifiedSendError = false;

        for (const QJsonValue &cv : chatsArr) {
            const QString rawCg = cv.toObject().value(QStringLiteral("guid")).toString().trimmed();
            const QString cg = resolveCanonicalChatGuid(rawCg);
            if (!cg.isEmpty()) {
                const auto beforeChatIt = m_chatMessagesCache.constFind(cg);
                const bool existedBefore = (beforeChatIt != m_chatMessagesCache.constEnd() && beforeChatIt->contains(msgGuid));
                const QJsonObject beforeCached = existedBefore ? beforeChatIt->value(msgGuid) : QJsonObject();
                const int oldErr = existedBefore ? beforeCached.value(QStringLiteral("error")).toInt(0) : 0;
                if (!isInitialLoad && incomingFromMe && incomingErr != 0 && (oldErr == 0 || !alreadySeen) && !notifiedSendError) {
                    notifiedSendError = true;
                    emit toastMessage(QStringLiteral("⚠ Message failed to deliver (error %1)").arg(incomingErr));
                }
                QJsonObject enrichedMsg = m;
                if (!rawCg.isEmpty() && enrichedMsg.value(QStringLiteral("originalChatGuid")).toString().isEmpty()) {
                    enrichedMsg.insert(QStringLiteral("originalChatGuid"), rawCg);
                }
                cacheMessageForChat(cg, enrichedMsg);
                const auto afterChatIt = m_chatMessagesCache.constFind(cg);
                const bool existsAfter = (afterChatIt != m_chatMessagesCache.constEnd() && afterChatIt->contains(msgGuid));
                const bool msgChanged = !existedBefore || !existsAfter || (beforeCached != afterChatIt->value(msgGuid));
                affectedChatGuids.insert(cg);
                if ((cg == m_selectedChatGuid || rawCg == m_selectedChatGuid) && msgChanged) {
                    selectedChatUpdated = true;
                }
            }
        }

        if (!msgGuid.isEmpty()) {
            const qint64 nowPollMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 addedAtMs = m_pendingDeliveryAddedAtMs.value(msgGuid, 0);
            const qint64 effectiveStartMs = (dateCreated > 0) ? dateCreated : addedAtMs;
            const bool wasPending = m_pendingDeliveryMessages.contains(msgGuid);
            const bool timedOut = m_expiredPendingDeliveryGuids.contains(msgGuid) ||
                                  (effectiveStartMs > 0 && (nowPollMs - effectiveStartMs) > 120000LL);
            const bool incomingSent = m.contains(QStringLiteral("isSent")) && m.value(QStringLiteral("isSent")).toBool();
            const bool positiveConfirmed = (incomingErr != 0 || incomingDelivered ||
                                            m.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0 ||
                                            incomingSent);
            if (positiveConfirmed) {
                m_expiredPendingDeliveryGuids.remove(msgGuid);
            }
            if (incomingErr != 0 || incomingDelivered || m.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0 || timedOut) {
                const QString removedCg = m_pendingDeliveryMessages.take(msgGuid);
                m_pendingDeliveryAddedAtMs.remove(msgGuid);
                m_inFlightDeliveryCheckGuids.remove(msgGuid);
                if (timedOut && !positiveConfirmed && incomingFromMe && wasPending) {
                    m_expiredPendingDeliveryGuids.insert(msgGuid);
                }
                if (timedOut && !removedCg.isEmpty() && !m_selectedChatGuid.isEmpty() && isSameConversation(removedCg, m_selectedChatGuid)) {
                    selectedChatUpdated = true;
                }
            } else if (incomingFromMe && m.contains(QStringLiteral("isSent")) && !m.value(QStringLiteral("isSent")).toBool() && !chatsArr.isEmpty()) {
                const QString pendingCg = resolveCanonicalChatGuid(chatsArr.first().toObject().value(QStringLiteral("guid")).toString().trimmed());
                if (!pendingCg.isEmpty()) {
                    m_pendingDeliveryMessages.insert(msgGuid, pendingCg);
                    if (!m_pendingDeliveryAddedAtMs.contains(msgGuid)) {
                        m_pendingDeliveryAddedAtMs.insert(msgGuid, nowPollMs);
                    }
                }
            }
        }

        const QJsonObject chatObj = chatsArr.first().toObject();
        const QString rawChatGuid = chatObj.value(QStringLiteral("guid")).toString().trimmed();
        const QString chatGuid = resolveCanonicalChatGuid(rawChatGuid);
        if (chatGuid.isEmpty()) continue;

        const QString embeddedDisp = chatObj.value(QStringLiteral("displayName")).toString().trimmed();
        if (!embeddedDisp.isEmpty()) {
            m_chatDisplayNames.insert(chatGuid, embeddedDisp);
        }
        if (chatObj.contains(QStringLiteral("style"))) {
            const int st = chatObj.value(QStringLiteral("style")).toInt(0);
            if (st > 0) m_chatStyles.insert(chatGuid, st);
        }
        const QJsonArray embeddedParts = chatObj.value(QStringLiteral("participants")).toArray();
        if (!embeddedParts.isEmpty() && m_chatParticipants.value(chatGuid).isEmpty()) {
            QStringList handles;
            for (const QJsonValue &pv : embeddedParts) {
                const QString addr = pv.toObject().value(QStringLiteral("address")).toString().trimmed();
                if (!addr.isEmpty()) handles.append(addr);
            }
            if (!handles.isEmpty()) {
                m_chatParticipants.insert(chatGuid, handles);
            }
        }

        if (m_deletedChatTimestamps.contains(chatGuid)) {
            const qint64 deletedAt = m_deletedChatTimestamps.value(chatGuid, 0);
            if (dateCreated <= deletedAt) {
                m_chatMessagesCache.remove(chatGuid);
                affectedChatGuids.remove(chatGuid);
                continue;
            } else {
                m_deletedChatTimestamps.remove(chatGuid);
                saveAppState();
            }
        }

        const bool recentLocalReadMut = (m_localReadMutationTimeMs.value(chatGuid, 0) > QDateTime::currentMSecsSinceEpoch() - 15000);
        // Also check chat-level read watermark metadata if embedded in the message's chat object
        const qint64 embeddedChatWm = extractChatReadWatermark(chatObj, m_chatMessagesCache.value(chatGuid));
        if (embeddedChatWm > 0 && !recentLocalReadMut) {
            const qint64 overrideTs = m_unreadOverrides.value(chatGuid, 0);
            if (overrideTs == 0 || embeddedChatWm > overrideTs) {
                if (overrideTs > 0 && embeddedChatWm > overrideTs) {
                    m_unreadOverrides.remove(chatGuid);
                }
                m_readWatermarks[chatGuid] = qMax(m_readWatermarks.value(chatGuid, 0), embeddedChatWm);
            }
        }

        // If this message was sent by me (e.g. replied from iPhone/Mac) or read on server, advance watermark
        // unless suppressed by an active local markChatUnread override older than/equal to this read timestamp.
        const bool isFromMeMsg = m.value(QStringLiteral("isFromMe")).toBool();
        const qint64 msgDateRead = m.value(QStringLiteral("dateRead")).toVariant().toLongLong();
        const bool msgIsRead = m.value(QStringLiteral("isRead")).toBool() || (msgDateRead > 0);
        const qint64 overrideTs = m_unreadOverrides.value(chatGuid, 0);

        if (!recentLocalReadMut) {
            if (isFromMeMsg) {
                if (overrideTs == 0 || dateCreated > overrideTs) {
                    if (overrideTs > 0 && dateCreated > overrideTs) {
                        m_unreadOverrides.remove(chatGuid);
                    }
                    m_readWatermarks[chatGuid] = qMax(m_readWatermarks.value(chatGuid, 0), dateCreated);
                }
            } else if (msgIsRead) {
                const qint64 effectiveReadTs = qMax(dateCreated, msgDateRead);
                if (overrideTs == 0 || effectiveReadTs > overrideTs) {
                    if (overrideTs > 0 && effectiveReadTs > overrideTs) {
                        m_unreadOverrides.remove(chatGuid);
                    }
                    m_readWatermarks[chatGuid] = qMax(m_readWatermarks.value(chatGuid, 0), dateCreated);
                }
            }
        }

        QString preview = extractMessageText(m);
        const QString assocType = m.value(QStringLiteral("associatedMessageType")).toVariant().toString();
        const QString emoji = mapReactionEmoji(assocType, preview);
        if (!emoji.isEmpty()) {
            preview = QStringLiteral("%1 Reacted to a message").arg(emoji);
        }
        if (preview.isEmpty()) continue;

        int existingIdx = -1;
        for (int i = 0; i < m_chats.size(); ++i) {
            if (m_chats[i].guid == chatGuid || m_chats[i].guid == rawChatGuid || m_chats[i].siblingGuids.contains(rawChatGuid)) {
                existingIdx = i;
                break;
            }
        }

        if (existingIdx < 0) {
            ChatEntry entry;
            entry.guid = chatGuid;
            entry.chatIdentifier = chatObj.value(QStringLiteral("chatIdentifier")).toString();
            entry.displayName = chatObj.value(QStringLiteral("displayName")).toString().trimmed();
            if (entry.displayName.isEmpty() && m_chatDisplayNames.contains(chatGuid)) {
                entry.displayName = m_chatDisplayNames.value(chatGuid);
            }
            entry.originalGroupId = m_chatOriginalGroupIds.value(chatGuid, m_chatOriginalGroupIds.value(rawChatGuid));
            entry.groupId = m_chatGroupIds.value(chatGuid, m_chatGroupIds.value(rawChatGuid));
            entry.siblingGuids = m_chatSiblingGuids.value(chatGuid);
            if (!entry.siblingGuids.contains(chatGuid)) entry.siblingGuids.append(chatGuid);
            if (!rawChatGuid.isEmpty() && !entry.siblingGuids.contains(rawChatGuid)) entry.siblingGuids.append(rawChatGuid);
            const int style = chatObj.value(QStringLiteral("style")).toInt(m_chatStyles.value(chatGuid, 45));
            entry.isGroup = (style == 43) || entry.chatIdentifier.startsWith(QStringLiteral("chat")) || chatGuid.contains(QStringLiteral(";+;"));
            entry.participantHandles = m_chatParticipants.value(chatGuid);
            if (entry.participantHandles.isEmpty() && !entry.isGroup && !entry.chatIdentifier.isEmpty()) {
                entry.participantHandles.append(entry.chatIdentifier);
            }
            {
                QJsonObject svcChat = chatObj;
                svcChat.insert(QStringLiteral("guid"), chatGuid);
                qint64 svcDate = 0;
                const QString svc = resolveChatService(svcChat, m, &svcDate);
                applyChatServiceEvidence(entry, svc, svcDate);
            }
            entry.lastMessageDate = dateCreated;
            entry.lastMessagePreview = preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
            entry.relativeTime = formatRelativeTime(dateCreated);
            rebuildChatPresentation(entry);
            m_chats.append(entry);
            existingIdx = m_chats.size() - 1;
            chatsModified = true;
        } else if (dateCreated > m_chats[existingIdx].lastMessageDate || (dateCreated == m_chats[existingIdx].lastMessageDate && m_chats[existingIdx].lastMessagePreview != preview.replace(QLatin1Char('\n'), QLatin1Char(' ')))) {
            m_chats[existingIdx].lastMessageDate = dateCreated;
            m_chats[existingIdx].lastMessagePreview = preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
            m_chats[existingIdx].relativeTime = formatRelativeTime(dateCreated);
            chatsModified = true;
        }

        // Notification for brand new incoming message
        if (!isInitialLoad && !alreadySeen) {
            newIncomingMessages.append(m);
            if (!msgIsRead && shouldNotifyForIncomingMessage(m, chatGuid, dateCreated, rowId)) {
                m_notifiedMessageGuids.insert(msgGuid);
                m_lastNotifiedTimestamp = qMax(m_lastNotifiedTimestamp, dateCreated);
                saveAppState();
                const QString senderAddr = m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("address")).toString();
                const QString senderName = senderAddr.isEmpty() ? m_chats[existingIdx].title : resolveContactName(senderAddr);
                notifyIncomingMessage(chatGuid, senderName, preview);
            }
        }
    }

    // Accurately reconcile unreadCount for affected chats directly from cached messages and watermark
    for (const QString &rawCg : affectedChatGuids) {
        const QString cg = resolveCanonicalChatGuid(rawCg);
        for (int i = 0; i < m_chats.size(); ++i) {
            if (m_chats[i].guid == cg) {
                int count = 0;
                const qint64 watermark = m_readWatermarks.value(cg, 0);
                const qint64 unreadOverrideTs = m_unreadOverrides.value(cg, 0);
                if (m_chatMessagesCache.contains(cg)) {
                    const auto &chatMap = m_chatMessagesCache.value(cg);
                    for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
                        if (isMessageUnread(it.value(), watermark, unreadOverrideTs)) {
                            count++;
                        }
                    }
                }
                const bool recentLocalMut = (m_localReadMutationTimeMs.value(cg, 0) > QDateTime::currentMSecsSinceEpoch() - 15000);
                // If active chat is currently focused, auto-read immediately unless explicitly marked unread
                if (cg == m_selectedChatGuid && m_windowActive && unreadOverrideTs == 0 && !recentLocalMut) {
                    if (count > 0) {
                        markChatRead(cg);
                    }
                    count = 0;
                }
                if (m_chats[i].unreadCount != count) {
                    m_chats[i].unreadCount = count;
                    chatsModified = true;
                }
                if (m_chats[i].displayName.isEmpty() && m_chatDisplayNames.contains(cg)) {
                    m_chats[i].displayName = m_chatDisplayNames.value(cg);
                    rebuildChatPresentation(m_chats[i]);
                    chatsModified = true;
                }
                const bool isGroupChat = m_chats[i].isGroup
                                         || (m_chatStyles.value(cg, 45) == 43)
                                         || cg.contains(QStringLiteral(";+;"))
                                         || m_chats[i].chatIdentifier.startsWith(QStringLiteral("chat"));
                if (isGroupChat) {
                    m_chats[i].isGroup = true;
                    QStringList merged = m_chatParticipants.value(cg);
                    QSet<QString> seenNorm;
                    for (const QString &h : merged) {
                        seenNorm.insert(normalizeHandle(h));
                    }
                    const QStringList inferred = inferGroupParticipantsFromMessages(cg);
                    for (const QString &h : inferred) {
                        const QString nh = normalizeHandle(h);
                        if (!nh.isEmpty() && !seenNorm.contains(nh)) {
                            seenNorm.insert(nh);
                            merged.append(h);
                        }
                    }
                    const QString prevTitle = m_chats[i].title;
                    if (m_chats[i].participantHandles != merged ||
                        prevTitle.startsWith(QStringLiteral("chat")) ||
                        prevTitle == QStringLiteral("Group Conversation")) {
                        m_chats[i].participantHandles = merged;
                        if (!merged.isEmpty() && m_chatParticipants.value(cg).isEmpty()) {
                            m_chatParticipants.insert(cg, merged);
                        }
                        rebuildChatPresentation(m_chats[i]);
                        if (m_chats[i].title != prevTitle || m_chats[i].participantHandles != merged) {
                            chatsModified = true;
                        }
                    }
                }
                break;
            }
        }
    }

    bool selectedMigrated = false;
    bool chatsMerged = false;
    unifySiblingChats(&selectedMigrated, &chatsMerged);
    if (chatsMerged) {
        chatsModified = true;
        if (selectedMigrated) {
            selectedChatUpdated = true;
        }
    }

    if (chatsModified) {
        sortChats();
        emit chatsChanged();
        const QString nextSelectedSig = m_selectedChatGuid + QLatin1Char('|') + selectedChatTitle() + QLatin1Char('|') +
                                        selectedChatSubtitle() + QLatin1Char('|') + QString::number(selectedChatUnreadCount()) +
                                        QLatin1Char('|') + selectedChatParticipants().join(QLatin1Char(',')) +
                                    QLatin1Char('|') + selectedChatService();
        if (selectedMigrated || nextSelectedSig != prevSelectedSig) {
            emit selectedChatChanged();
        }
    }

    fetchMissingGroupParticipants();

    if (!isInitialLoad) {
        for (const QJsonObject &m : newIncomingMessages) {
            const QJsonArray atts = m.value(QStringLiteral("attachments")).toArray();
            for (const QJsonValue &av : atts) {
                const QString ag = av.toObject().value(QStringLiteral("guid")).toString();
                if (!ag.isEmpty()) {
                    m_liveIncomingAttachments.insert(ag);
                }
            }
            prefetchMessageAttachments(m, true);
        }
    }

    if (isInitialLoad && !m_stayOnHomePage && !m_chats.isEmpty()) {
        QString targetGuid;
        for (const ChatEntry &c : m_chats) {
            if (c.lastMessageDate > 1) {
                targetGuid = c.guid;
                break;
            }
        }
        if (targetGuid.isEmpty()) {
            targetGuid = m_chats.first().guid;
        }

        bool needSelect = m_selectedChatGuid.isEmpty();
        if (!needSelect) {
            for (const ChatEntry &c : m_chats) {
                if (c.guid == m_selectedChatGuid && c.lastMessageDate <= 1) {
                    needSelect = true;
                    break;
                }
            }
        }
        if (needSelect) {
            selectChat(targetGuid);
        } else if (selectedChatUpdated) {
            rebuildSelectedChatMessages();
        }
    } else if (selectedChatUpdated && !m_selectedChatGuid.isEmpty()) {
        rebuildSelectedChatMessages();
    }

    if (isInitialLoad) {
        prefetchRecentChatAttachments();
    }
}

QStringList Backend::inferGroupParticipantsFromMessages(const QString &chatGuid) const {
    if (chatGuid.isEmpty() || !m_chatMessagesCache.contains(chatGuid)) {
        return {};
    }

    struct HandleOrder {
        int rowId = 999999;
        QString address;
    };
    QHash<QString, HandleOrder> byNorm;

    const auto &chatMap = m_chatMessagesCache.value(chatGuid);
    for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
        const QJsonObject &m = it.value();

        const QJsonArray chatsArr = m.value(QStringLiteral("chats")).toArray();
        if (!chatsArr.isEmpty()) {
            const QJsonArray partsArr = chatsArr.first().toObject().value(QStringLiteral("participants")).toArray();
            for (int idx = 0; idx < partsArr.size(); ++idx) {
                const QJsonObject pObj = partsArr[idx].toObject();
                const QString addr = pObj.value(QStringLiteral("address")).toString().trimmed();
                if (addr.isEmpty()) continue;
                const QString nh = normalizeHandle(addr);
                if (nh.isEmpty()) continue;
                int rid = pObj.value(QStringLiteral("originalROWID")).toInt(0);
                if (rid <= 0) rid = idx + 1;
                if (!byNorm.contains(nh) || rid < byNorm[nh].rowId) {
                    byNorm[nh] = HandleOrder{rid, addr};
                }
            }
        }

        if (!m.value(QStringLiteral("isFromMe")).toBool()) {
            const QJsonObject hObj = m.value(QStringLiteral("handle")).toObject();
            const QString addr = hObj.value(QStringLiteral("address")).toString().trimmed();
            if (!addr.isEmpty()) {
                const QString nh = normalizeHandle(addr);
                if (!nh.isEmpty()) {
                    int rid = hObj.value(QStringLiteral("originalROWID")).toInt(0);
                    if (rid <= 0) rid = 999999;
                    if (!byNorm.contains(nh) || rid < byNorm[nh].rowId) {
                        byNorm[nh] = HandleOrder{rid, addr};
                    }
                }
            }
        }
    }

    QList<HandleOrder> ordered = byNorm.values();
    std::sort(ordered.begin(), ordered.end(), [](const HandleOrder &a, const HandleOrder &b) {
        if (a.rowId != b.rowId) return a.rowId < b.rowId;
        return a.address < b.address;
    });

    QStringList result;
    for (const HandleOrder &ho : ordered) {
        result.append(ho.address);
    }
    return result;
}

QString Backend::formatGroupChatTitle(const QStringList &participantHandles) const {
    if (participantHandles.isEmpty()) {
        return QStringLiteral("Group Conversation");
    }
    if (participantHandles.size() == 1) {
        return resolveContactName(participantHandles.first());
    }

    QStringList fullNames;
    QStringList firstNames;
    QHash<QString, int> firstNameCounts;

    for (const QString &h : participantHandles) {
        const QString full = resolveContactName(h).trimmed();
        fullNames.append(full);
        const QString nh = normalizeHandle(h);
        const bool isResolved = m_contactNames.contains(nh) &&
                                !full.isEmpty() &&
                                !full.startsWith(QLatin1Char('+')) &&
                                !full.front().isDigit() &&
                                !full.contains(QLatin1Char('@'));
        if (isResolved && full.contains(QLatin1Char(' '))) {
            const QString first = full.section(QLatin1Char(' '), 0, 0).trimmed();
            firstNames.append(first);
            firstNameCounts[first.toLower()]++;
        } else {
            firstNames.append(full);
            firstNameCounts[full.toLower()]++;
        }
    }

    QStringList displayNames;
    for (int i = 0; i < firstNames.size(); ++i) {
        const QString &first = firstNames[i];
        if (firstNameCounts.value(first.toLower(), 0) > 1) {
            displayNames.append(fullNames[i]);
        } else {
            displayNames.append(first);
        }
    }

    if (displayNames.size() == 2) {
        return displayNames[0] + QStringLiteral(" & ") + displayNames[1];
    }
    return displayNames.mid(0, displayNames.size() - 1).join(QStringLiteral(", ")) +
           QStringLiteral(" & ") + displayNames.last();
}

void Backend::rebuildChatPresentation(ChatEntry &entry) const {
    entry.isGroup = (m_chatStyles.value(entry.guid, entry.isGroup ? 43 : 45) == 43)
                    || entry.guid.contains(QStringLiteral(";+;"))
                    || entry.chatIdentifier.startsWith(QStringLiteral("chat"))
                    || entry.participantHandles.size() > 1;

    if (entry.isGroup && entry.participantHandles.isEmpty()) {
        entry.participantHandles = m_chatParticipants.value(entry.guid);
        if (entry.participantHandles.isEmpty()) {
            entry.participantHandles = inferGroupParticipantsFromMessages(entry.guid);
        }
    }

    if (!entry.displayName.isEmpty()) {
        entry.title = entry.displayName;
    } else if (!entry.participantHandles.isEmpty()) {
        if (entry.isGroup) {
            entry.title = formatGroupChatTitle(entry.participantHandles);
        } else {
            entry.title = resolveContactName(entry.participantHandles.first());
        }
    } else if (entry.isGroup || entry.chatIdentifier.startsWith(QStringLiteral("chat"))) {
        entry.title = QStringLiteral("Group Conversation");
    } else {
        entry.title = resolveContactName(entry.chatIdentifier);
    }

    if (entry.isGroup) {
        entry.subtitle = entry.participantHandles.isEmpty()
                             ? QStringLiteral("Group Conversation")
                             : QStringLiteral("%1 participants").arg(entry.participantHandles.size());
    } else {
        const QString h = entry.participantHandles.isEmpty() ? entry.chatIdentifier : entry.participantHandles.first();
        entry.subtitle = h;
        entry.avatarUrl = resolveContactAvatar(h);
    }

    entry.initials = computeInitials(entry.title);
    entry.avatarColor = computeAvatarColor(entry.guid);
}

void Backend::sortChats() {
    std::sort(m_chats.begin(), m_chats.end(), [](const ChatEntry &a, const ChatEntry &b) {
        return a.lastMessageDate > b.lastMessageDate;
    });
}

QVariantList Backend::chats() const {
    QVariantList result;
    const QString q = m_searchQuery.trimmed().toLower();

    for (const ChatEntry &c : m_chats) {
        if (m_filterMode == QStringLiteral("unread") && c.unreadCount <= 0) continue;
        if (m_filterMode == QStringLiteral("direct") && c.isGroup) continue;
        if (m_filterMode == QStringLiteral("groups") && !c.isGroup) continue;
        if (m_filterMode == QStringLiteral("muted") && !m_mutedChats.contains(c.guid)) continue;

        if (!q.isEmpty()) {
            const bool matchTitle = c.title.toLower().contains(q);
            const bool matchSub = c.subtitle.toLower().contains(q);
            const bool matchPrev = c.lastMessagePreview.toLower().contains(q);
            bool matchParticipant = false;
            if (!matchTitle && !matchSub && !matchPrev) {
                for (const QString &ph : c.participantHandles) {
                    if (ph.toLower().contains(q) || resolveContactName(ph).toLower().contains(q)) {
                        matchParticipant = true;
                        break;
                    }
                }
            }
            if (!matchTitle && !matchSub && !matchPrev && !matchParticipant) continue;
        }

        QVariantMap map;
        map.insert(QStringLiteral("guid"), c.guid);
        map.insert(QStringLiteral("title"), c.title);
        map.insert(QStringLiteral("subtitle"), c.subtitle);
        map.insert(QStringLiteral("service"), c.service);
        map.insert(QStringLiteral("avatarUrl"), c.avatarUrl);
        map.insert(QStringLiteral("initials"), c.initials);
        map.insert(QStringLiteral("avatarColor"), c.avatarColor);
        map.insert(QStringLiteral("lastMessagePreview"), c.lastMessagePreview);
        map.insert(QStringLiteral("relativeTime"), c.relativeTime);
        map.insert(QStringLiteral("unreadCount"), c.unreadCount);
        QString addr = c.chatIdentifier;
        if (!c.isGroup && !c.participantHandles.isEmpty()) {
            addr = c.participantHandles.first();
        }
        map.insert(QStringLiteral("address"), addr);
        map.insert(QStringLiteral("isGroup"), c.isGroup);
        map.insert(QStringLiteral("muted"), m_mutedChats.contains(c.guid));
        map.insert(QStringLiteral("isMuted"), m_mutedChats.contains(c.guid));
        map.insert(QStringLiteral("pinned"), m_pinnedChats.contains(c.guid));
        result.append(map);
    }
    return result;
}

QVariantList Backend::pinnedChats() const {
    QVariantList result;
    QSet<QString> seenGuids;
    auto appendEntry = [&](const ChatEntry &c, bool isExplicitPin) {
        if (c.guid.isEmpty() || seenGuids.contains(c.guid)) return;
        seenGuids.insert(c.guid);
        QVariantMap map;
        map.insert(QStringLiteral("guid"), c.guid);
        map.insert(QStringLiteral("title"), c.title);
        QString shortTitle = c.title.trimmed();
        if (!c.isGroup && shortTitle.contains(QLatin1Char(' '))) {
            shortTitle = shortTitle.section(QLatin1Char(' '), 0, 0);
        } else if (shortTitle.size() > 12) {
            shortTitle = shortTitle.left(11) + QStringLiteral("…");
        }
        map.insert(QStringLiteral("shortTitle"), shortTitle);
        map.insert(QStringLiteral("initials"), c.initials);
        map.insert(QStringLiteral("avatarColor"), c.avatarColor);
        map.insert(QStringLiteral("avatarUrl"), c.avatarUrl);
        map.insert(QStringLiteral("unreadCount"), c.unreadCount);
        map.insert(QStringLiteral("isGroup"), c.isGroup);
        map.insert(QStringLiteral("service"), c.service);
        map.insert(QStringLiteral("pinned"), isExplicitPin);
        map.insert(QStringLiteral("isPinned"), isExplicitPin);
        // Same fields the chat list gives the context menu
        map.insert(QStringLiteral("address"), (!c.isGroup && !c.participantHandles.isEmpty()) ? c.participantHandles.first() : c.chatIdentifier);
        map.insert(QStringLiteral("isMuted"), m_mutedChats.contains(c.guid));
        map.insert(QStringLiteral("muted"), m_mutedChats.contains(c.guid));
        result.append(map);
    };
    // The Mac's pins (in its order), minus any unpinned here, then pins added here.
    for (const QString &pinGuid : effectivePinnedChatGuids()) {
        for (const ChatEntry &c : m_chats) {
            if (c.guid == pinGuid) {
                appendEntry(c, true);
                break;
            }
        }
    }
    if (m_pinnedRowFillRecent) {
        for (const ChatEntry &c : m_chats) {
            if (result.size() >= 4) break;
            if (!seenGuids.contains(c.guid)) {
                appendEntry(c, false);
            }
        }
    }
    return result;
}

int Backend::totalUnread() const {
    int sum = 0;
    for (const ChatEntry &c : m_chats) {
        sum += c.unreadCount;
    }
    return sum;
}

bool Backend::hasOlderMessages() const {
    if (m_selectedChatGuid.isEmpty()) return false;
    return m_chatHasMoreMessages.value(m_selectedChatGuid, false);
}

void Backend::setWindowActive(bool active) {
    if (m_windowActive == active) return;
    m_windowActive = active;
    emit windowActiveChanged();
    if (m_windowActive && !m_selectedChatGuid.isEmpty()) {
        for (const ChatEntry &c : m_chats) {
            if (c.guid == m_selectedChatGuid && c.unreadCount > 0) {
                markChatRead(m_selectedChatGuid);
                break;
            }
        }
    }
}

void Backend::setSearchQuery(const QString &query) {
    if (m_searchQuery == query) return;
    m_searchQuery = query;
    emit filterChanged();
    emit chatsChanged();
}

void Backend::setFilterMode(const QString &mode) {
    if (m_filterMode == mode) return;
    m_filterMode = mode;
    emit filterChanged();
    emit chatsChanged();
}

void Backend::clearSelectedChat() {
    m_stayOnHomePage = true;
    ++m_messageRequestSeq;
    if (m_activeMessagesReply) {
        m_activeMessagesReply->abort();
        m_activeMessagesReply = nullptr;
    }
    if (m_activeEnrichReply) {
        m_activeEnrichReply->abort();
        m_activeEnrichReply = nullptr;
    }
    m_selectedChatGuid.clear();
    m_loadingOlderMessages = false;
    m_messages.clear();
    emit selectedChatChanged();
    emit chatsChanged();
    emit messagesChanged();
    emit olderMessagesChanged();
}

void Backend::goHome() {
    clearSelectedChat();
}

void Backend::selectChat(const QString &chatGuid) {
    if (chatGuid.isEmpty()) {
        clearSelectedChat();
        return;
    }
    const QString resolvedGuid = resolveCanonicalChatGuid(chatGuid);
    m_stayOnHomePage = false;
    m_selectedChatGuid = resolvedGuid;
    m_unreadOverrides.remove(resolvedGuid);

    for (ChatEntry &c : m_chats) {
        if (c.guid == resolvedGuid) {
            m_readWatermarks[resolvedGuid] = qMax(m_readWatermarks.value(resolvedGuid, 0), c.lastMessageDate);
            saveAppState();
            if (c.unreadCount > 0) {
                c.unreadCount = 0;
                markChatRead(resolvedGuid);
            }
            break;
        }
    }

    ++m_messageRequestSeq;
    if (m_activeMessagesReply) {
        m_activeMessagesReply->abort();
        m_activeMessagesReply = nullptr;
    }
    if (m_activeEnrichReply) {
        m_activeEnrichReply->abort();
        m_activeEnrichReply = nullptr;
    }

    m_loadingOlderMessages = false;
    if (m_chatMessagesCache.contains(resolvedGuid)) {
        m_attachmentDownloadQueue.clear();
    }
    emit selectedChatChanged();

    if (!m_demoMode || m_chatMessagesCache.contains(resolvedGuid)) {
        rebuildSelectedChatMessages();
        prefetchRecentChatAttachments();
    } else if (m_demoMode && m_demoChatMessages.contains(resolvedGuid)) {
        emit aboutToChangeMessages();
        m_messages = m_demoChatMessages.value(resolvedGuid);
        emit messagesChanged();
    }
    emit chatsChanged();
    emit olderMessagesChanged();

    if (!m_demoMode) {
        loadChatMessages(resolvedGuid);
    }
}

void Backend::selectNextChat() {
    const QVariantList visible = chats();
    if (visible.isEmpty()) return;
    int idx = -1;
    for (int i = 0; i < visible.size(); ++i) {
        if (visible[i].toMap().value(QStringLiteral("guid")).toString() == m_selectedChatGuid) {
            idx = i;
            break;
        }
    }
    const int nextIdx = (idx + 1) % visible.size();
    selectChat(visible[nextIdx].toMap().value(QStringLiteral("guid")).toString());
}

void Backend::selectPrevChat() {
    const QVariantList visible = chats();
    if (visible.isEmpty()) return;
    int idx = 0;
    for (int i = 0; i < visible.size(); ++i) {
        if (visible[i].toMap().value(QStringLiteral("guid")).toString() == m_selectedChatGuid) {
            idx = i;
            break;
        }
    }
    const int prevIdx = (idx - 1 + visible.size()) % visible.size();
    selectChat(visible[prevIdx].toMap().value(QStringLiteral("guid")).toString());
}

QString Backend::selectedChatTitle() const {
    if (m_selectedChatGuid.isEmpty()) return QString();
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) return c.title;
    }
    return QString();
}

QString Backend::selectedChatSubtitle() const {
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) return c.subtitle;
    }
    return QString();
}

QString Backend::selectedChatService() const {
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) return c.service;
    }
    return QStringLiteral("iMessage");
}

QString Backend::selectedChatAvatar() const {
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) return c.avatarUrl;
    }
    return QString();
}

QString Backend::selectedChatInitials() const {
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) return c.initials;
    }
    return QStringLiteral("iM");
}

QString Backend::selectedChatColor() const {
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) return c.avatarColor;
    }
    return QStringLiteral("#3d59a1");
}

bool Backend::selectedChatIsGroup() const {
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) return c.isGroup;
    }
    return false;
}

QString Backend::selectedChatAddress() const {
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) {
            if (!c.participantHandles.isEmpty()) {
                return c.participantHandles.first();
            }
            if (!c.chatIdentifier.isEmpty()) {
                return c.chatIdentifier;
            }
        }
    }
    if (m_selectedChatGuid.contains(QLatin1Char(';'))) {
        const QStringList parts = m_selectedChatGuid.split(QLatin1Char(';'));
        if (parts.size() >= 3) {
            return parts.last();
        }
    }
    return QString();
}

bool Backend::selectedChatHasContact() const {
    const QString addr = selectedChatAddress();
    if (addr.isEmpty()) return false;
    const QString norm = normalizeHandle(addr);
    return m_contactNames.contains(norm);
}

int Backend::selectedChatUnreadCount() const {
    if (m_selectedChatGuid.isEmpty()) return 0;
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) {
            return c.unreadCount;
        }
    }
    return 0;
}

void Backend::saveContact(const QString &firstName, const QString &lastName, const QString &phone, const QString &email) {
    const QString fn = firstName.trimmed();
    const QString ln = lastName.trimmed();
    const QString ph = phone.trimmed();
    const QString em = email.trimmed();

    QString display = fn;
    if (!ln.isEmpty()) {
        display = display.isEmpty() ? ln : (display + QLatin1Char(' ') + ln);
    }
    if (display.isEmpty()) {
        display = !ph.isEmpty() ? ph : em;
    }
    if (display.isEmpty()) return;

    // Update in-memory contact names
    if (!ph.isEmpty()) {
        const QString normPh = normalizeHandle(ph);
        m_contactNames.insert(normPh, display);
        m_contactRawAddresses.insert(normPh, canonicalizeAddress(ph));
    }
    if (!em.isEmpty()) {
        const QString normEm = normalizeHandle(em);
        m_contactNames.insert(normEm, display);
        m_contactRawAddresses.insert(normEm, canonicalizeAddress(em));
    }
    // Keep the Mac record's other fields, but show the saved name in the info card right away
    for (const QString &addr : {ph, em}) {
        if (addr.isEmpty()) continue;
        const QString norm = normalizeHandle(addr);
        auto recIt = m_contactRecords.find(norm);
        if (recIt == m_contactRecords.end()) continue;
        recIt.value().insert(QStringLiteral("firstName"), fn);
        recIt.value().insert(QStringLiteral("lastName"), ln);
        recIt.value().insert(QStringLiteral("displayName"), display);
    }
    emit contactsChanged();

    QJsonObject contactObj;
    contactObj.insert(QStringLiteral("firstName"), fn);
    contactObj.insert(QStringLiteral("lastName"), ln);
    contactObj.insert(QStringLiteral("displayName"), display);
    QJsonArray phonesArr;
    if (!ph.isEmpty()) phonesArr.append(ph);
    contactObj.insert(QStringLiteral("phoneNumbers"), phonesArr);
    QJsonArray emailsArr;
    if (!em.isEmpty()) emailsArr.append(em);
    contactObj.insert(QStringLiteral("emails"), emailsArr);

    // Persist to local config cache ~/.config/imsg/contacts.json (only in live mode)
    if (!m_demoMode) {
        const QString cfgPath = QDir::homePath() + QStringLiteral("/.config/imsg/contacts.json");
        QJsonArray existingContacts;
        QFile rf(cfgPath);
        if (rf.open(QIODevice::ReadOnly)) {
            existingContacts = QJsonDocument::fromJson(rf.readAll()).array();
            rf.close();
        }
        existingContacts.append(contactObj);

        QDir().mkpath(QFileInfo(cfgPath).absolutePath());
        QFile wf(cfgPath);
        if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            wf.write(QJsonDocument(existingContacts).toJson(QJsonDocument::Indented));
            wf.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            wf.close();
        }
    }

    // Rebuild chats and messages immediately
    for (ChatEntry &c : m_chats) {
        rebuildChatPresentation(c);
    }
    emit chatsChanged();
    emit selectedChatChanged();
    rebuildSelectedChatMessages();

    emit toastMessage(QStringLiteral("✓ Contact saved: %1").arg(display));

    // Post to BlueBubbles server API
    if (!m_demoMode && !m_serverUrl.isEmpty() && !m_password.isEmpty()) {
        QJsonArray payloadArray;
        payloadArray.append(contactObj);
        QNetworkRequest req = makeRequest(QStringLiteral("/api/v1/contact"));
        req.setPriority(QNetworkRequest::HighPriority);
        QNetworkReply *reply = m_sendNam.post(req, QJsonDocument(payloadArray).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            fetchContactsRemote();
        });
    }
}

QVariantMap Backend::conversationMedia(const QString &chatGuid) const {
    // Photos, links and files shared in a conversation (newest first), for the
    // contact info view's tabs. Uses the merged canonical chat and its siblings.
    const QString canon = resolveCanonicalChatGuid(chatGuid.isEmpty() ? m_selectedChatGuid : chatGuid);
    QList<QJsonObject> msgs;
    for (auto it = m_chatMessagesCache.constBegin(); it != m_chatMessagesCache.constEnd(); ++it) {
        if (it.key() == canon || isSameConversation(it.key(), canon)) {
            for (const QJsonObject &m : it.value()) msgs.append(m);
        }
    }
    std::sort(msgs.begin(), msgs.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("dateCreated")).toVariant().toLongLong() >
               b.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
    });
    QVariantList photos, links, files;
    QSet<QString> seenLinks, seenAtts;
    for (const QJsonObject &m : msgs) {
        const QString guid = m.value(QStringLiteral("guid")).toString();
        if (m_deletedMessageGuids.contains(guid) || isMessageDeletedOrRetracted(m)) continue;
        const qint64 date = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        const bool fromMe = m.value(QStringLiteral("isFromMe")).toBool();
        // Tapbacks carry no media of their own
        if (!normalizeAssociatedGuid(m.value(QStringLiteral("associatedMessageGuid")).toString()).isEmpty()) continue;
        // Same cleanup as the timeline: one row per attachment, no plugin payloads
        for (const QJsonValue &av : deduplicateMessageAttachments(m.value(QStringLiteral("attachments")).toArray())) {
            const QJsonObject a = av.toObject();
            const QString ag = a.value(QStringLiteral("guid")).toString();
            if (ag.isEmpty() || seenAtts.contains(ag) || a.value(QStringLiteral("hideAttachment")).toBool()) continue;
            seenAtts.insert(ag);
            const QString name = a.value(QStringLiteral("transferName")).toString();
            const QString mime = a.value(QStringLiteral("mimeType")).toString();
            const QString path = lookupCachedAttachmentPath(m_attachmentPaths, ag);
            QVariantMap item{{QStringLiteral("guid"), ag}, {QStringLiteral("messageGuid"), guid},
                             {QStringLiteral("name"), name}, {QStringLiteral("mimeType"), mime},
                             {QStringLiteral("date"), date}, {QStringLiteral("fromMe"), fromMe},
                             {QStringLiteral("localUrl"), (!path.isEmpty() && QFileInfo::exists(path)) ? QUrl::fromLocalFile(path).toString() : QString()}};
            if (isImageMimeOrExt(mime, name)) photos.append(item);
            else if (!mime.startsWith(QStringLiteral("audio/")) || name.isEmpty() || !name.endsWith(QStringLiteral(".caf"), Qt::CaseInsensitive)) files.append(item);
        }
        auto urlIt = sharedMessageUrlRegex().globalMatch(extractMessageText(m));
        while (urlIt.hasNext()) {
            const QString url = normalizeExtractedHrefUrl(stripTrailingUrlPunctuation(urlIt.next().captured(0)));
            if (url.isEmpty() || seenLinks.contains(url)) continue;
            seenLinks.insert(url);
            const QVariantMap preview = m_linkPreviewCache.value(url);
            links.append(QVariantMap{{QStringLiteral("url"), url}, {QStringLiteral("messageGuid"), guid},
                                     {QStringLiteral("title"), preview.value(QStringLiteral("title")).toString()},
                                     {QStringLiteral("host"), QUrl(url).host()},
                                     {QStringLiteral("date"), date}, {QStringLiteral("fromMe"), fromMe}});
        }
    }
    return QVariantMap{{QStringLiteral("photos"), photos}, {QStringLiteral("links"), links}, {QStringLiteral("files"), files}};
}

QVariantMap Backend::contactDetails(const QString &address) const {
    QVariantMap out;
    const QVariantMap rec = m_contactRecords.value(normalizeHandle(address));
    out.insert(QStringLiteral("found"), !rec.isEmpty());
    if (rec.isEmpty()) return out;
    auto str = [&rec](const char *key) { return rec.value(QString::fromLatin1(key)).toString().trimmed(); };
    out.insert(QStringLiteral("firstName"), str("firstName"));
    out.insert(QStringLiteral("lastName"), str("lastName"));

    // Every populated field, in Contacts' order, for the "On your Mac" list
    QVariantList rows;
    auto addRow = [&rows](const QString &label, const QString &value) {
        if (value.trimmed().isEmpty()) return;
        rows.append(QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("value"), value.trimmed()}});
    };
    addRow(QStringLiteral("Middle name"), str("middleName"));
    addRow(QStringLiteral("Nickname"), str("nickname"));
    addRow(QStringLiteral("Company"), str("organization"));
    addRow(QStringLiteral("Job title"), str("jobTitle"));
    addRow(QStringLiteral("Department"), str("department"));
    auto addList = [&](const char *key, const QString &kind, QString *firstValue) {
        for (const QVariant &v : rec.value(QString::fromLatin1(key)).toList()) {
            const QVariantMap m = v.toMap();
            const QString value = v.typeId() == QMetaType::QString ? v.toString() : m.value(QStringLiteral("address")).toString();
            const QString label = m.value(QStringLiteral("label")).toString().trimmed();
            if (firstValue && firstValue->isEmpty()) *firstValue = value.trimmed();
            addRow(label.isEmpty() ? kind : QStringLiteral("%1 (%2)").arg(kind, label), value);
        }
    };
    QString firstPhone, firstEmail;
    addList("phoneNumbers", QStringLiteral("Phone"), &firstPhone);
    addList("emails", QStringLiteral("Email"), &firstEmail);
    addList("addresses", QStringLiteral("Address"), nullptr);
    addList("urls", QStringLiteral("URL"), nullptr);
    const QString birthday = str("birthday");
    if (!birthday.isEmpty()) {
        const QDate d = QDate::fromString(birthday, Qt::ISODate);
        if (d.isValid()) addRow(QStringLiteral("Birthday"), QLocale().toString(d, QLocale::LongFormat));
        else if (birthday.startsWith(QStringLiteral("--"))) {
            const QDate md = QDate::fromString(QStringLiteral("2000") + birthday.mid(1), Qt::ISODate);
            addRow(QStringLiteral("Birthday"), md.isValid() ? QLocale().toString(md, QStringLiteral("MMMM d")) : birthday);
        } else addRow(QStringLiteral("Birthday"), birthday);
    }
    addRow(QStringLiteral("Note"), str("note"));
    out.insert(QStringLiteral("rows"), rows);

    // Prefill the editable phone/email with the chat's own address when it's one of them
    const QString canon = canonicalizeAddress(address);
    const bool isEmail = address.contains(QLatin1Char('@'));
    out.insert(QStringLiteral("phone"), isEmail ? firstPhone : (canon.isEmpty() ? firstPhone : canon));
    out.insert(QStringLiteral("email"), isEmail ? address.trimmed() : firstEmail);
    return out;
}

void Backend::syncContactsNow() {
    emit toastMessage(QStringLiteral("↻ Syncing contacts..."));
    fetchContactsRemote();
}

QString Backend::computeOutgoingReceiptText(
    const QJsonObject &msgObj,
    bool hasLaterIncomingReply,
    qint64 nowMs,
    qint64 addedAtMs,
    bool forceExpired) {
    const int err = msgObj.value(QStringLiteral("error")).toInt(0);
    if (err != 0) {
        return QStringLiteral("⚠ Not Delivered");
    }
    const qint64 dateRead = msgObj.value(QStringLiteral("dateRead")).toVariant().toLongLong();
    if (dateRead > 0) {
        return QStringLiteral("Read ") + QDateTime::fromMSecsSinceEpoch(dateRead).toString(QStringLiteral("h:mm AP"));
    }
    const qint64 dateDelivered = msgObj.value(QStringLiteral("dateDelivered")).toVariant().toLongLong();
    const bool isDeliveredFlag = msgObj.value(QStringLiteral("isDelivered")).toBool(false);
    const bool hasIsSent = msgObj.contains(QStringLiteral("isSent"));
    const bool hasIsDelivered = msgObj.contains(QStringLiteral("isDelivered"));
    const bool isSentFlag = msgObj.value(QStringLiteral("isSent")).toBool(true);
    if (dateDelivered > 0 || isDeliveredFlag) {
        return QStringLiteral("Delivered");
    }
    const bool isPending = msgObj.value(QStringLiteral("pending")).toBool(false);
    if (!isPending && !hasIsSent && !hasIsDelivered) {
        return QStringLiteral("Delivered");
    }
    if ((hasIsSent && !isSentFlag) || (isPending && !hasIsSent)) {
        if (hasLaterIncomingReply) {
            return QStringLiteral("Sent");
        }
        const qint64 dateCreated = msgObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        const qint64 effectiveStartMs = (dateCreated > 0) ? dateCreated : addedAtMs;
        const qint64 effectiveNow = (nowMs > 0) ? nowMs : QDateTime::currentMSecsSinceEpoch();
        const bool expired = forceExpired ||
                             (effectiveStartMs > 0 && (effectiveNow - effectiveStartMs) > 120000LL);
        if (expired) {
            return QStringLiteral("Unconfirmed");
        }
        return QStringLiteral("Sending...");
    }
    return QStringLiteral("Sent");
}

QVariantList Backend::buildMessageListFromApi(
    const QJsonArray &rawMessagesDesc,
    const QHash<QString, QString> &contactNames,
    const QHash<QString, QString> &attachmentCache,
    QStringList *missingAttachmentsOut,
    const QHash<QString, QVariantMap> &previewCache,
    QStringList *missingPreviewsOut,
    const QSet<QString> &deletedMessageGuids,
    const QHash<QString, QString> &recentOutboundEffects,
    const QHash<QString, qint64> &pendingDeliveryAddedAtMs,
    const QSet<QString> &expiredPendingDeliveryGuids) {

    // Collect chronological (oldest -> newest), skipping deleted/retracted messages
    QList<QJsonObject> chronological;
    chronological.reserve(rawMessagesDesc.size());
    for (int i = rawMessagesDesc.size() - 1; i >= 0; --i) {
        const QJsonObject mObj = rawMessagesDesc.at(i).toObject();
        const QString g = mObj.value(QStringLiteral("guid")).toString();
        if (!g.isEmpty() && deletedMessageGuids.contains(g)) continue;
        if (isMessageDeletedOrRetracted(mObj)) continue;
        chronological.append(mObj);
    }

    auto formatFallbackHandle = [](const QString &raw) -> QString {
        QString digits;
        for (const QChar &ch : raw) {
            if (ch.isDigit()) digits.append(ch);
        }
        if (digits.size() == 11 && digits.startsWith(QLatin1Char('1'))) {
            digits = digits.mid(1);
        }
        if (digits.size() == 10 && !raw.contains(QLatin1Char('@'))) {
            return QStringLiteral("+1 (%1) %2-%3")
                .arg(digits.left(3), digits.mid(3, 3), digits.mid(6, 4));
        }
        return raw;
    };

    auto resolveName = [&](const QString &handle) -> QString {
        if (handle.isEmpty()) return QStringLiteral("Unknown");
        const QString norm = normalizeHandle(handle);
        if (contactNames.contains(norm) && !contactNames.value(norm).trimmed().isEmpty()) {
            return contactNames.value(norm);
        }
        if (contactNames.contains(handle) && !contactNames.value(handle).trimmed().isEmpty()) {
            return contactNames.value(handle);
        }
        return formatFallbackHandle(handle);
    };

    auto resolveSenderForMessage = [&](const QJsonObject &m) -> QString {
        if (m.value(QStringLiteral("isFromMe")).toBool() || m.value(QStringLiteral("is_from_me")).toBool() || (m.value(QStringLiteral("is_from_me")).toInt(0) == 1)) {
            return QStringLiteral("You");
        }
        QString handleAddr = m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("address")).toString().trimmed();
        if (handleAddr.isEmpty()) {
            handleAddr = m.value(QStringLiteral("sender")).toString().trimmed();
        }
        if (!handleAddr.isEmpty()) {
            return resolveName(handleAddr);
        }
        const QJsonArray chatsArr = m.value(QStringLiteral("chats")).toArray();
        if (!chatsArr.isEmpty()) {
            const QJsonObject cObj = chatsArr.first().toObject();
            const int style = cObj.value(QStringLiteral("style")).toInt(0);
            const QString chatId = cObj.value(QStringLiteral("chatIdentifier")).toString().trimmed();
            if (style != 43 && !chatId.isEmpty() && !chatId.startsWith(QStringLiteral("chat"), Qt::CaseInsensitive)) {
                return resolveName(chatId);
            }
        }
        return QStringLiteral("Unknown");
    };

    // Pass 1: collect reactions by normalized parent GUID
    struct ReactionEntry {
        QString emoji;
        QString sender;
        bool fromMe = false;
    };
    QHash<QString, QList<ReactionEntry>> reactionsByParent;
    QHash<QString, QString> messageTextByGuid;
    QHash<QString, QString> messageSenderByGuid;
    QHash<QString, QString> resolvedReactionTargetByGuid;

    struct SeenMsgMeta {
        QString guid;
        QString normalizedText;
        bool hasAttachments = false;
    };
    QList<SeenMsgMeta> seenMessages;

    auto collapseWhitespace = [](const QString &s) -> QString {
        QString out = s;
        out.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral(" "));
        return out.simplified();
    };

    for (const QJsonObject &m : chronological) {
        const QString guid = m.value(QStringLiteral("guid")).toString();
        const QString normGuid = normalizeAssociatedGuid(guid);
        QString assocGuid = normalizeAssociatedGuid(m.value(QStringLiteral("associatedMessageGuid")).toString());
        const QString assocType = m.value(QStringLiteral("associatedMessageType")).toVariant().toString();
        const QString assocEmoji = m.value(QStringLiteral("associatedMessageEmoji")).toString().trimmed();
        const QString rawText = extractMessageText(m);
        bool isRemoval = false;
        QString emoji = mapReactionEmoji(assocType, rawText, &isRemoval);
        if ((emoji.isEmpty() || emoji == QStringLiteral("✨")) && !assocEmoji.isEmpty()) {
            emoji = assocEmoji;
            if (assocType == QStringLiteral("3006") || assocType.startsWith(QLatin1Char('-'))) {
                isRemoval = true;
            }
        }

        const bool isFromMe = m.value(QStringLiteral("isFromMe")).toBool();
        const QString senderName = resolveSenderForMessage(m);

        if (assocGuid.isEmpty() && !emoji.isEmpty()) {
            const QString threadOrig = normalizeAssociatedGuid(m.value(QStringLiteral("threadOriginatorGuid")).toString());
            if (!threadOrig.isEmpty() && messageTextByGuid.contains(threadOrig)) {
                assocGuid = threadOrig;
            } else {
                const QString tr = rawText.trimmed();
                const bool isMediaTapback = tr.endsWith(QStringLiteral("an image"))
                    || tr.endsWith(QStringLiteral("an attachment"))
                    || tr.endsWith(QStringLiteral("a photo"))
                    || tr.endsWith(QStringLiteral("a movie"));
                if (isMediaTapback) {
                    for (int i = seenMessages.size() - 1; i >= 0; --i) {
                        if (seenMessages[i].hasAttachments) {
                            assocGuid = seenMessages[i].guid;
                            break;
                        }
                    }
                } else {
                    int qStart = tr.indexOf(QChar(0x201C)); // “
                    int qEnd = tr.lastIndexOf(QChar(0x201D)); // ”
                    if (qStart < 0 || qEnd <= qStart) {
                        qStart = tr.indexOf(QLatin1Char('"'));
                        qEnd = tr.lastIndexOf(QLatin1Char('"'));
                    }
                    if (qStart >= 0 && qEnd > qStart) {
                        QString quoted = collapseWhitespace(tr.mid(qStart + 1, qEnd - qStart - 1));
                        if (quoted.endsWith(QChar(0x2026))) quoted.chop(1);
                        else if (quoted.endsWith(QStringLiteral("..."))) quoted.chop(3);
                        quoted = quoted.trimmed();
                        if (!quoted.isEmpty()) {
                            for (int i = seenMessages.size() - 1; i >= 0; --i) {
                                const QString &cand = seenMessages[i].normalizedText;
                                if (!cand.isEmpty() && (cand == quoted || cand.startsWith(quoted) || quoted.startsWith(cand))) {
                                    assocGuid = seenMessages[i].guid;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!assocGuid.isEmpty() && !emoji.isEmpty()) {
            if (!guid.isEmpty()) {
                resolvedReactionTargetByGuid.insert(guid, assocGuid);
            }
            auto &list = reactionsByParent[assocGuid];
            if (isRemoval) {
                for (int i = list.size() - 1; i >= 0; --i) {
                    if (list[i].emoji == emoji && list[i].sender == senderName) {
                        list.removeAt(i);
                        break;
                    }
                }
            } else {
                list.append({emoji, senderName, isFromMe});
            }
        } else if (!guid.isEmpty()) {
            const bool hasAtts = !m.value(QStringLiteral("attachments")).toArray().isEmpty();
            const QString previewText = (!rawText.trimmed().isEmpty())
                ? collapseWhitespace(rawText)
                : (hasAtts ? QStringLiteral("📷 Attachment") : QString());
            messageTextByGuid.insert(guid, previewText);
            messageSenderByGuid.insert(guid, senderName);
            if (!normGuid.isEmpty() && normGuid != guid) {
                messageTextByGuid.insert(normGuid, previewText);
                messageSenderByGuid.insert(normGuid, senderName);
            }
            seenMessages.append({normGuid.isEmpty() ? guid : normGuid, collapseWhitespace(rawText), hasAtts});
        }
    }

    // Find latest outgoing message index for delivery/read receipt
    int latestOutgoingIdx = -1;
    QString latestOutgoingGuid;
    for (int i = chronological.size() - 1; i >= 0; --i) {
        const QJsonObject &m = chronological[i];
        if (isTapbackOrAssociatedMessage(m) || isHiddenTimelinePlaceholder(m)) continue;
        if (m.value(QStringLiteral("isFromMe")).toBool()) {
            latestOutgoingIdx = i;
            latestOutgoingGuid = m.value(QStringLiteral("guid")).toString();
            break;
        }
    }
    bool hasLaterIncomingReply = false;
    if (latestOutgoingIdx >= 0) {
        for (int i = latestOutgoingIdx + 1; i < chronological.size(); ++i) {
            const QJsonObject &m = chronological[i];
            if (isTapbackOrAssociatedMessage(m) || isHiddenTimelinePlaceholder(m)) continue;
            if (!m.value(QStringLiteral("isFromMe")).toBool()) {
                hasLaterIncomingReply = true;
                break;
            }
        }
    }

    QVariantList out;
    qint64 prevTimestamp = 0;
    const QDate today = QDate::currentDate();

    for (const QJsonObject &m : chronological) {
        const QString guid = m.value(QStringLiteral("guid")).toString();
        const QString assocGuid = normalizeAssociatedGuid(m.value(QStringLiteral("associatedMessageGuid")).toString());
        const QString assocType = m.value(QStringLiteral("associatedMessageType")).toVariant().toString();
        const QString text = extractMessageText(m);

        // Skip tapback messages from main timeline if they belong to a parent
        const QString effectiveAssocGuid = !assocGuid.isEmpty() ? assocGuid : resolvedReactionTargetByGuid.value(guid);
        const QString assocEmoji = m.value(QStringLiteral("associatedMessageEmoji")).toString().trimmed();
        if (!effectiveAssocGuid.isEmpty() && (!mapReactionEmoji(assocType, text).isEmpty() || !assocEmoji.isEmpty())) {
            continue;
        }

        if (isHiddenTimelinePlaceholder(m)) continue;
        const QJsonArray atts = m.value(QStringLiteral("attachments")).toArray();

        const qint64 dateCreated = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        const bool isFromMe = m.value(QStringLiteral("isFromMe")).toBool();
        const QString handleAddr = m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("address")).toString();
        const QString senderName = isFromMe ? QStringLiteral("You") : resolveName(handleAddr);

        bool showDateHeader = false;
        QString dateHeaderText;
        if (dateCreated > 0) {
            const QDateTime dt = QDateTime::fromMSecsSinceEpoch(dateCreated);
            const QDateTime prevDt = QDateTime::fromMSecsSinceEpoch(prevTimestamp);
            if (prevTimestamp == 0 || dt.date() != prevDt.date() || (dateCreated - prevTimestamp) > 2 * 3600 * 1000) {
                showDateHeader = true;
                const qint64 daysToToday = dt.date().daysTo(today);
                if (daysToToday == 0) {
                    dateHeaderText = QStringLiteral("Today · ") + dt.toString(QStringLiteral("h:mm AP"));
                } else if (daysToToday == 1) {
                    dateHeaderText = QStringLiteral("Yesterday · ") + dt.toString(QStringLiteral("h:mm AP"));
                } else {
                    dateHeaderText = dt.toString(QStringLiteral("ddd, MMM d · h:mm AP"));
                }
            }
            prevTimestamp = dateCreated;
        }

        // Group reactions by emoji
        QVariantList reactionPills;
        const QString lookupReactionGuid = reactionsByParent.contains(guid) ? guid : normalizeAssociatedGuid(guid);
        if (reactionsByParent.contains(lookupReactionGuid)) {
            QHash<QString, int> counts;
            QHash<QString, bool> fromMeMap;
            QHash<QString, QStringList> sendersMap;
            QStringList order;
            for (const ReactionEntry &re : reactionsByParent.value(lookupReactionGuid)) {
                if (!counts.contains(re.emoji)) order.append(re.emoji);
                if (re.fromMe) fromMeMap[re.emoji] = true;
                const QString effectiveSender = re.sender.trimmed().isEmpty() ? QStringLiteral("Unknown") : re.sender.trimmed();
                if (effectiveSender != QStringLiteral("Unknown") && sendersMap[re.emoji].contains(effectiveSender)) {
                    continue;
                }
                sendersMap[re.emoji].append(effectiveSender);
                counts[re.emoji] += 1;
            }
            for (const QString &em : order) {
                const QStringList sList = sendersMap.value(em);
                const QString joinedSenders = sList.join(QStringLiteral(", "));
                QVariantMap pill;
                pill.insert(QStringLiteral("emoji"), em);
                pill.insert(QStringLiteral("count"), counts.value(em));
                pill.insert(QStringLiteral("fromMe"), fromMeMap.value(em, false));
                pill.insert(QStringLiteral("senders"), joinedSenders);
                pill.insert(QStringLiteral("senderList"), QVariant::fromValue(sList));
                pill.insert(QStringLiteral("tooltipText"), joinedSenders.isEmpty() ? em : (em + QStringLiteral(" ") + joinedSenders));
                reactionPills.append(pill);
            }
        }

        // Attachment details (supports single and multi-attachment messages, deduplicating RCS/CloudKit ghost rows)
        bool hasImageAttachment = false;
        bool hasFileAttachment = false;
        QString attachmentGuid;
        QString attachmentName;
        QString attachmentLocalUrl;
        QVariantList imageAttachments;
        QVariantList fileAttachments;
        QVariantList allAttachments;
        const QJsonArray cleanAtts = deduplicateMessageAttachments(atts);
        if (!cleanAtts.isEmpty()) {
            for (const QJsonValue &av : cleanAtts) {
                const QJsonObject aObj = av.toObject();
                const QString mime = aObj.value(QStringLiteral("mimeType")).toString();
                const QString tName = aObj.value(QStringLiteral("transferName")).toString();
                if (tName.endsWith(QStringLiteral(".pluginPayloadAttachment"), Qt::CaseInsensitive)) {
                    continue;
                }
                const QString aGuid = aObj.value(QStringLiteral("guid")).toString();
                if (aGuid.isEmpty() && tName.isEmpty()) {
                    continue;
                }
                const bool isImg = isImageMimeOrExt(mime, tName);
                const bool isAudio = isAudioAttachment(mime, tName);
                const double rawDurationSec = aObj.value(QStringLiteral("durationSeconds")).toDouble(0.0);
                int durationSeconds = (rawDurationSec > 0.0) ? qMax(1, qRound(rawDurationSec)) : 0;
                if (durationSeconds <= 0) {
                    const double fallbackDurSec = aObj.value(QStringLiteral("duration")).toDouble(0.0);
                    durationSeconds = (fallbackDurSec > 0.0) ? qMax(1, qRound(fallbackDurSec)) : 0;
                }
                QVariantList waveformBars;
                if (aObj.contains(QStringLiteral("waveform")) && aObj.value(QStringLiteral("waveform")).isArray()) {
                    const QJsonArray wfArr = aObj.value(QStringLiteral("waveform")).toArray();
                    for (const QJsonValue &v : wfArr) {
                        waveformBars.append(qBound(0.15, v.toDouble(0.15), 1.0));
                    }
                }
                if (isAudio && waveformBars.isEmpty()) {
                    waveformBars = computeWaveformBars(aGuid.isEmpty() ? tName : aGuid, 28);
                }
                const QString origGuid = aObj.value(QStringLiteral("originalGuid")).toString();
                QString cachedPath = lookupCachedAttachmentPath(attachmentCache, aGuid);
                if (cachedPath.isEmpty() && !origGuid.isEmpty()) {
                    cachedPath = lookupCachedAttachmentPath(attachmentCache, origGuid);
                }
                QString localUrl;
                if (!cachedPath.isEmpty()) {
                    localUrl = QUrl::fromLocalFile(cachedPath).toString();
                } else if (isImg && missingAttachmentsOut && !aGuid.isEmpty()) {
                    missingAttachmentsOut->append(aGuid + QStringLiteral("|") + tName);
                }

                QVariantMap attEntry;
                attEntry.insert(QStringLiteral("guid"), aGuid);
                attEntry.insert(QStringLiteral("name"), tName);
                attEntry.insert(QStringLiteral("mimeType"), mime);
                attEntry.insert(QStringLiteral("localUrl"), localUrl);
                attEntry.insert(QStringLiteral("isImage"), isImg);
                attEntry.insert(QStringLiteral("isAudio"), isAudio);
                attEntry.insert(QStringLiteral("durationSeconds"), durationSeconds);
                attEntry.insert(QStringLiteral("waveformBars"), waveformBars);
                attEntry.insert(QStringLiteral("sourceUrl"), localUrl.isEmpty() ? (aGuid.isEmpty() ? QString() : QStringLiteral("/api/v1/attachment/") + aGuid + QStringLiteral("/download")) : localUrl);
                allAttachments.append(attEntry);

                if (isImg) {
                    imageAttachments.append(attEntry);
                    if (!hasImageAttachment) {
                        hasImageAttachment = true;
                        attachmentGuid = aGuid;
                        attachmentName = tName;
                        attachmentLocalUrl = localUrl;
                    }
                } else if (!aGuid.isEmpty()) {
                    fileAttachments.append(attEntry);
                    hasFileAttachment = true;
                    if (attachmentGuid.isEmpty()) {
                        attachmentGuid = aGuid;
                        attachmentName = tName;
                        attachmentLocalUrl = localUrl;
                    }
                }
            }
        }

        // Receipt text
        QString receiptText;
        if (isFromMe && guid == latestOutgoingGuid) {
            receiptText = computeOutgoingReceiptText(
                m,
                hasLaterIncomingReply,
                0,
                pendingDeliveryAddedAtMs.value(guid, 0),
                expiredPendingDeliveryGuids.contains(guid));
        }

        // Thread reply originator
        const QString rawReplyGuid = m.value(QStringLiteral("threadOriginatorGuid")).toString().trimmed();
        const QString replyGuid = normalizeAssociatedGuid(rawReplyGuid);
        QString replyToText;
        QString replyToSender;
        if (!replyGuid.isEmpty() && messageTextByGuid.contains(replyGuid)) {
            replyToText = collapseWhitespace(messageTextByGuid.value(replyGuid));
            replyToSender = messageSenderByGuid.value(replyGuid);
        } else if (!rawReplyGuid.isEmpty() && messageTextByGuid.contains(rawReplyGuid)) {
            replyToText = collapseWhitespace(messageTextByGuid.value(rawReplyGuid));
            replyToSender = messageSenderByGuid.value(rawReplyGuid);
        }

        QString service = normalizeServiceName(m.value(QStringLiteral("service")).toString());
        if (service.isEmpty()) {
            service = normalizeServiceName(m.value(QStringLiteral("handle")).toObject().value(QStringLiteral("service")).toString());
        }
        if (service.isEmpty()) {
            if (guid.startsWith(QStringLiteral("iMessage"))) service = QStringLiteral("iMessage");
            else if (guid.startsWith(QStringLiteral("RCS"))) service = QStringLiteral("RCS");
            else if (guid.startsWith(QStringLiteral("SMS"))) service = QStringLiteral("SMS");
        }

        const bool isIMessage = (service == QStringLiteral("iMessage")) || service.isEmpty();

        const qint64 dateEdited = qMax(
            m.value(QStringLiteral("dateEdited")).toVariant().toLongLong(),
            m.value(QStringLiteral("date_edited")).toVariant().toLongLong());
        const bool isEdited = (dateEdited > 0);
        QString rawEffect = m.value(QStringLiteral("expressiveSendStyleId")).toString().trimmed();
        if (rawEffect.isEmpty()) {
            rawEffect = m.value(QStringLiteral("expressive_send_style_id")).toString().trimmed();
        }
        if (rawEffect.isEmpty() && isFromMe && !recentOutboundEffects.isEmpty()) {
            const QString normTxt = text.trimmed();
            if (recentOutboundEffects.contains(normTxt)) {
                rawEffect = recentOutboundEffects.value(normTxt);
            }
        }
        const QString sendEffectLabel = formatExpressiveEffectLabel(rawEffect);
        QString expressiveEffectName;
        if (rawEffect.endsWith(QStringLiteral(".invisibleink"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("🕵️ Invisible Ink");
        else if (rawEffect.endsWith(QStringLiteral(".impact"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("💥 Slam");
        else if (rawEffect.endsWith(QStringLiteral(".loud"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("📢 Loud");
        else if (rawEffect.endsWith(QStringLiteral(".gentle"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("🪶 Gentle");
        else if (rawEffect.contains(QStringLiteral("CKConfettiEffect"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("🎉 Confetti");
        else if (rawEffect.contains(QStringLiteral("CKLasersEffect"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("✨ Lasers");
        else if (rawEffect.contains(QStringLiteral("CKFireworksEffect"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("🎆 Fireworks");
        else expressiveEffectName = sendEffectLabel;
        const int errorCode = m.value(QStringLiteral("error")).toInt(0);
        const bool isFailed = isFromMe && (errorCode != 0);
        if (isFailed) {
            receiptText = QStringLiteral("⚠ Not Delivered");
        }
        const QString baseTimeText = dateCreated > 0 ? QDateTime::fromMSecsSinceEpoch(dateCreated).toString(QStringLiteral("h:mm AP")) : QString();
        QStringList metaParts;
        if (!baseTimeText.isEmpty()) metaParts.append(baseTimeText);
        if (isEdited) metaParts.append(QStringLiteral("Edited"));
        if (!sendEffectLabel.isEmpty()) metaParts.append(sendEffectLabel);
        if (isFailed) metaParts.append(QStringLiteral("⚠ Not Delivered"));
        const QString metaBadgeText = metaParts.join(QStringLiteral(" · "));
        const QString otpCode = extractVerificationCode(text);

        QString itemChatGuid = m.value(QStringLiteral("chatGuid")).toString().trimmed();
        if (itemChatGuid.isEmpty()) {
            const QJsonArray itemChats = m.value(QStringLiteral("chats")).toArray();
            if (!itemChats.isEmpty()) {
                itemChatGuid = itemChats.first().toObject().value(QStringLiteral("guid")).toString().trimmed();
            }
        }
        QVariantMap item;
        item.insert(QStringLiteral("guid"), guid);
        item.insert(QStringLiteral("chatGuid"), itemChatGuid);
        item.insert(QStringLiteral("text"), text);
        item.insert(QStringLiteral("formattedText"), formatMessageTextWithLinks(text));
        item.insert(QStringLiteral("verificationCode"), otpCode);
        item.insert(QStringLiteral("otpCode"), otpCode);
        item.insert(QStringLiteral("isEdited"), isEdited);
        item.insert(QStringLiteral("sendEffectLabel"), sendEffectLabel);
        item.insert(QStringLiteral("expressiveEffect"), expressiveEffectName);
        item.insert(QStringLiteral("expressiveSendStyleId"), rawEffect);
        item.insert(QStringLiteral("errorCode"), errorCode);
        item.insert(QStringLiteral("isFailed"), isFailed);
        item.insert(QStringLiteral("metaBadgeText"), metaBadgeText);
        item.insert(QStringLiteral("isFromMe"), isFromMe);
        item.insert(QStringLiteral("isIMessage"), isIMessage);
        item.insert(QStringLiteral("dateCreated"), dateCreated);
        item.insert(QStringLiteral("service"), service);
        item.insert(QStringLiteral("senderName"), senderName);
        item.insert(QStringLiteral("senderColor"), computeAvatarColor(handleAddr.isEmpty() ? senderName : handleAddr));
        item.insert(QStringLiteral("timeText"), baseTimeText);
        item.insert(QStringLiteral("showDateHeader"), showDateHeader);
        item.insert(QStringLiteral("dateHeaderText"), dateHeaderText);
        item.insert(QStringLiteral("reactions"), reactionPills);
        item.insert(QStringLiteral("hasImageAttachment"), hasImageAttachment);
        item.insert(QStringLiteral("hasFileAttachment"), hasFileAttachment);
        item.insert(QStringLiteral("attachmentGuid"), attachmentGuid);
        item.insert(QStringLiteral("attachmentName"), attachmentName);
        item.insert(QStringLiteral("attachmentLocalUrl"), attachmentLocalUrl);
        item.insert(QStringLiteral("imageAttachments"), imageAttachments);
        item.insert(QStringLiteral("fileAttachments"), fileAttachments);
        item.insert(QStringLiteral("attachments"), allAttachments);
        item.insert(QStringLiteral("imageAttachmentCount"), imageAttachments.size());
        item.insert(QStringLiteral("receiptText"), receiptText);
        item.insert(QStringLiteral("replyToGuid"), replyGuid);
        item.insert(QStringLiteral("replyToText"), replyToText);
        item.insert(QStringLiteral("replyToSender"), replyToSender);

        const QString firstUrl = extractFirstUrl(text);
        bool hasLinkPreview = false;
        QVariantMap linkPreview;
        if (!firstUrl.isEmpty()) {
            if (previewCache.contains(firstUrl)) {
                hasLinkPreview = true;
                linkPreview = previewCache.value(firstUrl);
            } else if (missingPreviewsOut) {
                missingPreviewsOut->append(guid + QStringLiteral("|") + firstUrl);
            }
        }
        item.insert(QStringLiteral("hasLinkPreview"), hasLinkPreview);
        item.insert(QStringLiteral("linkPreview"), linkPreview);
        item.insert(QStringLiteral("linkUrl"), firstUrl);
        out.append(item);
    }

    // "N Replies" under messages that started a thread (iOS-style)
    QHash<QString, int> replyCounts;
    for (const QVariant &v : std::as_const(out)) {
        const QString r = v.toMap().value(QStringLiteral("replyToGuid")).toString();
        if (!r.isEmpty()) ++replyCounts[r];
    }
    if (!replyCounts.isEmpty()) {
        for (int i = 0; i < out.size(); ++i) {
            QVariantMap m = out[i].toMap();
            const int n = replyCounts.value(m.value(QStringLiteral("guid")).toString(), 0);
            if (n > 0) {
                m.insert(QStringLiteral("replyCount"), n);
                out[i] = m;
            }
        }
    }
    return out;
}

QVariantList Backend::threadMessages(const QString &rootGuid) const {
    QVariantList thread;
    const QString root = normalizeAssociatedGuid(rootGuid);
    if (root.isEmpty()) return thread;
    for (const QVariant &v : m_messages) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("guid")).toString() == root ||
            m.value(QStringLiteral("replyToGuid")).toString() == root) {
            thread.append(m);
        }
    }
    return thread;
}

int Backend::messageIndexForGuid(const QString &guid) const {
    const QString g = normalizeAssociatedGuid(guid);
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].toMap().value(QStringLiteral("guid")).toString() == g) return i;
    }
    return -1;
}

static void normalizeOutgoingReceipts(QVariantList &msgs) {
    int latestOutgoingIdx = -1;
    for (int i = msgs.size() - 1; i >= 0; --i) {
        if (msgs[i].toMap().value(QStringLiteral("isFromMe")).toBool()) {
            latestOutgoingIdx = i;
            break;
        }
    }
    for (int i = 0; i < msgs.size(); ++i) {
        QVariantMap vm = msgs[i].toMap();
        if (!vm.value(QStringLiteral("isFromMe")).toBool()) continue;
        if (i != latestOutgoingIdx) {
            const bool isFailed = vm.value(QStringLiteral("isFailed")).toBool() ||
                                  (vm.value(QStringLiteral("errorCode")).toInt(0) != 0);
            const QString curReceipt = vm.value(QStringLiteral("receiptText")).toString();
            if (!isFailed && curReceipt != QStringLiteral("⚠ Not Delivered") && !curReceipt.isEmpty()) {
                vm.insert(QStringLiteral("receiptText"), QString());
                msgs[i] = vm;
            }
        }
    }
}

void Backend::enforceLatestOutgoingReceiptsInTimeline() {
    normalizeOutgoingReceipts(m_messages);
}

static bool hasRealNonSyntheticAttachment(const QJsonArray &atts) {
    for (const QJsonValue &av : atts) {
        const QString ag = av.toObject().value(QStringLiteral("guid")).toString().trimmed();
        if (!ag.isEmpty() && !ag.startsWith(QStringLiteral("att-")) && !ag.startsWith(QStringLiteral("temp-"))) {
            return true;
        }
    }
    return false;
}

static QJsonObject mergeCachedMessageObject(const QJsonObject &existing, const QJsonObject &incoming,
                                            bool isFromMe, bool recentLocalReadMut) {
    QJsonObject merged = existing;
    const qint64 existingRowId = existing.value(QStringLiteral("originalROWID")).toVariant().toLongLong();
    const bool existingIsPending = existing.value(QStringLiteral("pending")).toBool(false);
    const bool existingIsDbBacked = (existingRowId > 0) ||
                                    (!existingIsPending &&
                                     !existing.value(QStringLiteral("guid")).toString().startsWith(QStringLiteral("temp-")) &&
                                     !existing.value(QStringLiteral("guid")).toString().startsWith(QStringLiteral("bp-")) &&
                                     (existing.value(QStringLiteral("isDelivered")).toBool() ||
                                      existing.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0 ||
                                      hasRealNonSyntheticAttachment(existing.value(QStringLiteral("attachments")).toArray())));
    const qint64 incomingRowId = incoming.value(QStringLiteral("originalROWID")).toVariant().toLongLong();
    const bool incomingIsPending = incoming.value(QStringLiteral("pending")).toBool(false);
    const bool incomingIsSyntheticOrPending = incomingIsPending || (existingIsDbBacked && incomingRowId <= 0);
    const int existingErr = existing.value(QStringLiteral("error")).toInt(0);
    const bool existingHasDelivFlag = existing.value(QStringLiteral("isDelivered")).toBool();
    const qint64 existingDelivTs = existing.value(QStringLiteral("dateDelivered")).toVariant().toLongLong();
    const qint64 existingReadTs = existing.value(QStringLiteral("dateRead")).toVariant().toLongLong();
    const bool existingHadPositiveReceipt = existingHasDelivFlag || (existingDelivTs > 0) || (existingReadTs > 0);
    const qint64 incomingDelivTs = incoming.value(QStringLiteral("dateDelivered")).toVariant().toLongLong();
    const qint64 incomingReadTs = incoming.value(QStringLiteral("dateRead")).toVariant().toLongLong();
    const bool incomingHasPositiveReceipt =
        incoming.value(QStringLiteral("isDelivered")).toBool() ||
        (incomingDelivTs > 0) ||
        (incomingReadTs > 0);
    const int incomingErr = incoming.value(QStringLiteral("error")).toInt(0);
    const bool incomingExplicitNewError = (incomingErr != 0 && !incomingIsPending && !incomingHasPositiveReceipt);
    const bool incomingAdvancesReceipt =
        (incomingReadTs > existingReadTs) ||
        (incomingDelivTs > existingDelivTs) ||
        (!existingHadPositiveReceipt && incomingHasPositiveReceipt);
    const bool shouldClearExistingError =
        !incomingExplicitNewError &&
        ((existingErr == 0) ? incomingHasPositiveReceipt : incomingAdvancesReceipt);

    for (auto it = incoming.begin(); it != incoming.end(); ++it) {
        const QString &key = it.key();
        if (key == QStringLiteral("text")) {
            if (!it.value().toString().trimmed().isEmpty()) {
                merged.insert(key, it.value());
            }
        } else if (key == QStringLiteral("handle")) {
            if (it.value().isObject() && !it.value().toObject().value(QStringLiteral("address")).toString().trimmed().isEmpty()) {
                merged.insert(key, it.value());
            }
        } else if (key == QStringLiteral("attributedBody")) {
            if (!it.value().toArray().isEmpty() && !(existingIsDbBacked && incomingIsPending)) {
                merged.insert(key, it.value());
            }
        } else if (key == QStringLiteral("attachments")) {
            const QJsonArray incAtts = it.value().toArray();
            if (!incAtts.isEmpty()) {
                const QJsonArray curAtts = existing.value(QStringLiteral("attachments")).toArray();
                const bool curHasReal = hasRealNonSyntheticAttachment(curAtts);
                const bool incHasReal = hasRealNonSyntheticAttachment(incAtts);
                if (curHasReal && (!incHasReal || incomingIsPending)) {
                    continue;
                }
                if (existingIsDbBacked && incomingIsSyntheticOrPending && !curAtts.isEmpty()) {
                    continue;
                }
                merged.insert(key, it.value());
            }
        } else if (key == QStringLiteral("originalROWID")) {
            const qint64 newRowId = it.value().toVariant().toLongLong();
            if (newRowId > 0 || existingRowId <= 0) {
                merged.insert(key, it.value());
            }
        } else if (key == QStringLiteral("dateCreated")) {
            const qint64 newDc = it.value().toVariant().toLongLong();
            const qint64 oldDc = existing.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
            if (oldDc > 0 && (newDc <= 0 || (existingIsDbBacked && incomingIsSyntheticOrPending))) {
                continue;
            }
            merged.insert(key, it.value());
        } else if (key == QStringLiteral("pending")) {
            if (!it.value().toBool() || !existingIsDbBacked) {
                if (!existing.contains(QStringLiteral("pending")) || !it.value().toBool() || existingIsPending) {
                    merged.insert(key, it.value());
                }
            }
        } else if (recentLocalReadMut && !isFromMe && (key == QStringLiteral("isRead") || key == QStringLiteral("dateRead"))) {
            // Preserve local read/unread state on incoming messages during recent local mutation window
            continue;
        } else if (isFromMe && key == QStringLiteral("isDelivered")) {
            if (it.value().toBool() || !merged.value(QStringLiteral("isDelivered")).toBool()) {
                merged.insert(key, it.value());
            }
        } else if (isFromMe && key == QStringLiteral("isSent")) {
            if (it.value().toBool() || !merged.value(QStringLiteral("isSent")).toBool()) {
                merged.insert(key, it.value());
            }
        } else if (isFromMe && (key == QStringLiteral("dateDelivered") || key == QStringLiteral("dateRead"))) {
            const qint64 newTs = it.value().toVariant().toLongLong();
            const qint64 oldTs = merged.value(key).toVariant().toLongLong();
            if (newTs >= oldTs) {
                merged.insert(key, it.value());
            }
        } else if (isFromMe && key == QStringLiteral("error")) {
            const int newErr = it.value().toInt(0);
            const int oldErr = merged.value(QStringLiteral("error")).toInt(0);
            if (shouldClearExistingError) {
                merged.insert(key, 0);
            } else if (newErr != 0) {
                if (!incomingIsPending) {
                    merged.insert(key, it.value());
                }
            } else if (oldErr == 0) {
                merged.insert(key, it.value());
            }
        } else {
            merged.insert(key, it.value());
        }
    }
    if (isFromMe && shouldClearExistingError) {
        merged.insert(QStringLiteral("error"), 0);
    }
    return merged;
}

void Backend::cacheMessageForChat(const QString &chatGuid, const QJsonObject &rawMsg) {
    if (chatGuid.isEmpty()) return;
    const QString targetGuid = resolveCanonicalChatGuid(chatGuid);
    const QString msgGuid = rawMsg.value(QStringLiteral("guid")).toString();
    if (msgGuid.isEmpty()) return;
    {
        const auto existingChatIt = m_chatMessagesCache.constFind(targetGuid);
        if (existingChatIt == m_chatMessagesCache.constEnd() || !existingChatIt->contains(msgGuid)) {
            m_messageObservationSeq.insert(msgGuid, ++m_deliveryObservationCounter);
        } else {
            const QJsonObject prev = existingChatIt->value(msgGuid);
            if (prev.value(QStringLiteral("isSent")).toBool() != rawMsg.value(QStringLiteral("isSent")).toBool() ||
                prev.value(QStringLiteral("isDelivered")).toBool() != rawMsg.value(QStringLiteral("isDelivered")).toBool() ||
                prev.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() != rawMsg.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() ||
                prev.value(QStringLiteral("dateRead")).toVariant().toLongLong() != rawMsg.value(QStringLiteral("dateRead")).toVariant().toLongLong() ||
                prev.value(QStringLiteral("error")).toInt(0) != rawMsg.value(QStringLiteral("error")).toInt(0)) {
                m_messageObservationSeq.insert(msgGuid, ++m_deliveryObservationCounter);
            }
        }
    }
    const QJsonArray msgChats = rawMsg.value(QStringLiteral("chats")).toArray();
    QString rawOrigChatGuid = rawMsg.value(QStringLiteral("originalChatGuid")).toString().trimmed();
    if (!msgChats.isEmpty()) {
        bool matchesTargetChat = false;
        bool hasAnyChatGuid = false;
        for (const QJsonValue &cv : msgChats) {
            const QString cg = cv.toObject().value(QStringLiteral("guid")).toString().trimmed();
            if (!cg.isEmpty()) {
                hasAnyChatGuid = true;
                if (rawOrigChatGuid.isEmpty()) rawOrigChatGuid = cg;
                if (cg == targetGuid || cg == chatGuid || resolveCanonicalChatGuid(cg) == targetGuid) {
                    matchesTargetChat = true;
                    break;
                }
            }
        }
        if (hasAnyChatGuid && !matchesTargetChat) {
            return;
        }
    }
    QJsonObject msg = rawMsg;
    if (!rawOrigChatGuid.isEmpty()) {
        msg.insert(QStringLiteral("originalChatGuid"), rawOrigChatGuid);
    }
    msg.insert(QStringLiteral("chatGuid"), targetGuid);
    refineChatServiceFromMessage(targetGuid, msg);
    if (m_deletedMessageGuids.contains(msgGuid) || isMessageDeletedOrRetracted(msg)) {
        if (isMessageDeletedOrRetracted(msg)) {
            m_deletedMessageGuids.insert(msgGuid);
        }
        if (m_chatMessagesCache.contains(targetGuid)) {
            m_chatMessagesCache[targetGuid].remove(msgGuid);
        }
        clearPendingDeliveryTrackingForGuid(msgGuid, true);
        return;
    }

    const QJsonArray incomingAtts = msg.value(QStringLiteral("attachments")).toArray();
    for (const QJsonValue &av : incomingAtts) {
        const QJsonObject aObj = av.toObject();
        const QString ag = aObj.value(QStringLiteral("guid")).toString();
        if (!ag.isEmpty() && !m_attachmentPaths.contains(ag) && !m_attachmentPaths.contains(ag.left(16)) &&
            !m_downloadingAttachments.contains(ag)) {
            if (aObj.value(QStringLiteral("transferState")).toInt(0) == 5) {
                m_attemptedAttachments.remove(ag);
                m_missingAttachmentCooldownUntilMs.remove(ag);
            }
        }
    }

    auto &chatMap = m_chatMessagesCache[targetGuid];
    const bool isTempGuid = msgGuid.startsWith(QStringLiteral("temp-")) || msgGuid.startsWith(QStringLiteral("bp-"));
    const QString explicitTempGuid = msg.value(QStringLiteral("tempGuid")).toString().trimmed();
    const QString explicitUploadUuid = msg.value(QStringLiteral("uploadUuid")).toString().trimmed();
    const bool isFromMe = msg.value(QStringLiteral("isFromMe")).toBool();
    const QString incomingText = extractMessageText(msg).trimmed();
    const qint64 incomingDc = msg.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
    const bool incomingHasAtt = !incomingAtts.isEmpty();

    if (isFromMe) {
        if (!isTempGuid) {
            // Real server message arrived: evict matching optimistic temp-*/bp-* entry from chatMap
            QStringList tempKeysToRemove;
            if (!explicitTempGuid.isEmpty() && explicitTempGuid != msgGuid) {
                tempKeysToRemove.append(explicitTempGuid);
            }
            if (!explicitUploadUuid.isEmpty() && explicitUploadUuid != msgGuid && !tempKeysToRemove.contains(explicitUploadUuid)) {
                tempKeysToRemove.append(explicitUploadUuid);
            }
            for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
                const QString k = it.key();
                if (k == msgGuid || tempKeysToRemove.contains(k)) continue;
                const bool kIsTemp = k.startsWith(QStringLiteral("temp-")) || k.startsWith(QStringLiteral("bp-"));
                const QString kTempGuid = it.value().value(QStringLiteral("tempGuid")).toString().trimmed();
                const QString kUploadUuid = it.value().value(QStringLiteral("uploadUuid")).toString().trimmed();
                if (!explicitTempGuid.isEmpty() || !explicitUploadUuid.isEmpty()) {
                    const bool tempMatches = !explicitTempGuid.isEmpty() &&
                                             (k == explicitTempGuid || kTempGuid == explicitTempGuid || kUploadUuid == explicitTempGuid);
                    const bool uploadMatches = !explicitUploadUuid.isEmpty() &&
                                               (k == explicitUploadUuid || kTempGuid == explicitUploadUuid || kUploadUuid == explicitUploadUuid);
                    if (tempMatches || uploadMatches) {
                        tempKeysToRemove.append(k);
                    }
                    continue;
                }
                if (!kIsTemp) continue;
                if (!it.value().value(QStringLiteral("isFromMe")).toBool()) continue;
                const QString kText = extractMessageText(it.value()).trimmed();
                const qint64 kDc = it.value().value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                const bool kHasAtt = !it.value().value(QStringLiteral("attachments")).toArray().isEmpty();
                const bool closeInTime = (incomingDc == 0 || kDc == 0 || qAbs(incomingDc - kDc) <= 120000LL);
                if (!closeInTime) continue;
                if (!incomingHasAtt && !kHasAtt && !incomingText.isEmpty() && kText == incomingText) {
                    tempKeysToRemove.append(k);
                }
            }
            for (const QString &tk : tempKeysToRemove) {
                chatMap.remove(tk);
                m_seenMessageGuids.remove(tk);
                clearPendingDeliveryTrackingForGuid(tk, true);
            }

            // Monotonically merge into chatMap first so timeline updates compare against merged state
            QJsonObject mergedMsg;
            if (!chatMap.contains(msgGuid)) {
                chatMap.insert(msgGuid, msg);
                mergedMsg = msg;
            } else {
                const bool recentLocalReadMut = (m_localReadMutationTimeMs.value(targetGuid, 0) > QDateTime::currentMSecsSinceEpoch() - 15000);
                mergedMsg = mergeCachedMessageObject(chatMap.value(msgGuid), msg, isFromMe, recentLocalReadMut);
                chatMap.insert(msgGuid, mergedMsg);
            }

            if (targetGuid == m_selectedChatGuid) {
                const int msgErr = mergedMsg.value(QStringLiteral("error")).toInt(0);
                const qint64 msgDateCreated = mergedMsg.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                const bool hasLaterIncoming = hasEligibleLaterIncomingReply(chatMap, msgDateCreated);
                const QString computedReceipt = computeOutgoingReceiptText(
                    mergedMsg,
                    hasLaterIncoming,
                    0,
                    m_pendingDeliveryAddedAtMs.value(msgGuid, 0),
                    m_expiredPendingDeliveryGuids.contains(msgGuid));
                QVariantList nextTimeline = m_messages;
                int existingRealIdx = -1;
                int firstUnguidedTextMatchIdx = -1;
                QList<int> matchingTempIndices;
                for (int i = 0; i < nextTimeline.size(); ++i) {
                    const QVariantMap vm = nextTimeline[i].toMap();
                    const QString vg = vm.value(QStringLiteral("guid")).toString();
                    if (vg == msgGuid) {
                        if (existingRealIdx < 0) existingRealIdx = i;
                        else matchingTempIndices.append(i);
                    } else if (!explicitTempGuid.isEmpty()) {
                        if (vg == explicitTempGuid || tempKeysToRemove.contains(vg)) {
                            matchingTempIndices.append(i);
                        }
                    } else if (tempKeysToRemove.contains(vg)) {
                        matchingTempIndices.append(i);
                    } else if (firstUnguidedTextMatchIdx < 0 &&
                               (vg.startsWith(QStringLiteral("temp-")) || vg.startsWith(QStringLiteral("bp-"))) &&
                               vm.value(QStringLiteral("isFromMe")).toBool()) {
                        const bool vmHasAtt = vm.value(QStringLiteral("hasImageAttachment")).toBool() ||
                                              vm.value(QStringLiteral("hasFileAttachment")).toBool();
                        if (!incomingHasAtt && !vmHasAtt && !incomingText.isEmpty() &&
                            vm.value(QStringLiteral("text")).toString().trimmed() == incomingText) {
                            firstUnguidedTextMatchIdx = i;
                        }
                    }
                }
                if (existingRealIdx < 0 && explicitTempGuid.isEmpty() && tempKeysToRemove.isEmpty() &&
                    firstUnguidedTextMatchIdx >= 0) {
                    matchingTempIndices.append(firstUnguidedTextMatchIdx);
                }
                auto applyMeta = [&](QVariantMap &vm, bool promoteGuid, bool isLatestOutgoingRow) {
                    if (promoteGuid) vm.insert(QStringLiteral("guid"), msgGuid);
                    if (msgErr != 0 || computedReceipt == QStringLiteral("⚠ Not Delivered")) {
                        vm.insert(QStringLiteral("receiptText"), QStringLiteral("⚠ Not Delivered"));
                    } else if (isLatestOutgoingRow) {
                        vm.insert(QStringLiteral("receiptText"), computedReceipt);
                    } else {
                        vm.insert(QStringLiteral("receiptText"), QString());
                    }
                    vm.insert(QStringLiteral("isFailed"), msgErr != 0);
                    vm.insert(QStringLiteral("errorCode"), msgErr);
                    if (vm.value(QStringLiteral("expressiveSendStyleId")).toString().isEmpty() && m_recentOutboundEffects.contains(incomingText)) {
                        const QString effId = m_recentOutboundEffects.value(incomingText);
                        vm.insert(QStringLiteral("expressiveSendStyleId"), effId);
                        vm.insert(QStringLiteral("sendEffectLabel"), formatExpressiveEffectLabel(effId));
                    }
                };
                if (existingRealIdx >= 0) {
                    for (int k = matchingTempIndices.size() - 1; k >= 0; --k) {
                        if (matchingTempIndices[k] < existingRealIdx) --existingRealIdx;
                        nextTimeline.removeAt(matchingTempIndices[k]);
                    }
                    int latestOutgoingIdx = -1;
                    for (int i = nextTimeline.size() - 1; i >= 0; --i) {
                        if (nextTimeline[i].toMap().value(QStringLiteral("isFromMe")).toBool()) {
                            latestOutgoingIdx = i;
                            break;
                        }
                    }
                    QVariantMap vm = nextTimeline[existingRealIdx].toMap();
                    applyMeta(vm, false, existingRealIdx == latestOutgoingIdx);
                    nextTimeline[existingRealIdx] = vm;
                    normalizeOutgoingReceipts(nextTimeline);
                } else if (!matchingTempIndices.isEmpty()) {
                    const int keepIdx = matchingTempIndices.first();
                    for (int k = matchingTempIndices.size() - 1; k >= 1; --k) {
                        nextTimeline.removeAt(matchingTempIndices[k]);
                    }
                    int latestOutgoingIdx = -1;
                    for (int i = nextTimeline.size() - 1; i >= 0; --i) {
                        if (nextTimeline[i].toMap().value(QStringLiteral("isFromMe")).toBool()) {
                            latestOutgoingIdx = i;
                            break;
                        }
                    }
                    QVariantMap vm = nextTimeline[keepIdx].toMap();
                    applyMeta(vm, true, keepIdx == latestOutgoingIdx);
                    nextTimeline[keepIdx] = vm;
                    normalizeOutgoingReceipts(nextTimeline);
                }
                if (nextTimeline != m_messages) {
                    emit aboutToChangeMessages();
                    m_messages = nextTimeline;
                    emit messagesChanged();
                }
            }
            return;
        } else {
            // Incoming message has a synthetic temp-*/bp-* GUID: skip caching if the real server message is already cached
            for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
                const QString k = it.key();
                if (k.startsWith(QStringLiteral("temp-")) || k.startsWith(QStringLiteral("bp-"))) continue;
                if (!it.value().value(QStringLiteral("isFromMe")).toBool()) continue;
                const QString kTempGuid = it.value().value(QStringLiteral("tempGuid")).toString().trimmed();
                const QString kUploadUuid = it.value().value(QStringLiteral("uploadUuid")).toString().trimmed();
                if ((!kTempGuid.isEmpty() && (kTempGuid == msgGuid || (!explicitTempGuid.isEmpty() && kTempGuid == explicitTempGuid))) ||
                    (!kUploadUuid.isEmpty() && (kUploadUuid == msgGuid || (!explicitUploadUuid.isEmpty() && kUploadUuid == explicitUploadUuid)))) {
                    return;
                }
                const QString kText = extractMessageText(it.value()).trimmed();
                const qint64 kDc = it.value().value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                const bool kHasAtt = !it.value().value(QStringLiteral("attachments")).toArray().isEmpty();
                const bool closeInTime = (incomingDc == 0 || kDc == 0 || qAbs(incomingDc - kDc) <= 120000LL);
                if (closeInTime && !incomingHasAtt && !kHasAtt && !incomingText.isEmpty() && kText == incomingText) {
                    return;
                }
            }
        }
    }

    if (!chatMap.contains(msgGuid)) {
        chatMap.insert(msgGuid, msg);
        return;
    }

    const bool recentLocalReadMut = (m_localReadMutationTimeMs.value(targetGuid, 0) > QDateTime::currentMSecsSinceEpoch() - 15000);
    const QJsonObject merged = mergeCachedMessageObject(chatMap.value(msgGuid), msg, isFromMe, recentLocalReadMut);
    chatMap.insert(msgGuid, merged);
}

void Backend::rebuildSelectedChatMessages() {
    if (m_selectedChatGuid.isEmpty()) {
        if (!m_messages.isEmpty()) {
            emit aboutToChangeMessages();
            m_messages.clear();
            emit messagesChanged();
        }
        return;
    }

    const QHash<QString, QJsonObject> chatMap = m_chatMessagesCache.value(m_selectedChatGuid);
    QVariantList nextMessages;
    QStringList missingAtts;
    QStringList missingPreviews;
    if (!chatMap.isEmpty()) {
        QList<QJsonObject> objs = chatMap.values();
        std::sort(objs.begin(), objs.end(), [](const QJsonObject &a, const QJsonObject &b) {
            const qint64 da = a.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
            const qint64 db = b.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
            if (da != db) return da > db;
            return a.value(QStringLiteral("guid")).toString() > b.value(QStringLiteral("guid")).toString();
        });

        QJsonArray rawDesc;
        for (const QJsonObject &o : objs) {
            rawDesc.append(o);
        }

        nextMessages = buildMessageListFromApi(
            rawDesc,
            m_contactNames,
            m_attachmentPaths,
            &missingAtts,
            m_linkPreviewCache,
            &missingPreviews,
            m_deletedMessageGuids,
            m_recentOutboundEffects,
            m_pendingDeliveryAddedAtMs,
            m_expiredPendingDeliveryGuids);
    }

    // Retain any pending optimistic messages (temp-*) from m_messages if their text/attachment is not yet in nextMessages
    const qint64 nowRebuildMs = QDateTime::currentMSecsSinceEpoch();
    QSet<QString> alreadyPresentRealGuids;
    QHash<QString, int> prevTimelineOrder;
    prevTimelineOrder.reserve(m_messages.size());
    for (int i = 0; i < m_messages.size(); ++i) {
        const QString cg = m_messages.at(i).toMap().value(QStringLiteral("guid")).toString();
        if (!cg.isEmpty()) {
            prevTimelineOrder.insert(cg, i);
            if (!cg.startsWith(QStringLiteral("temp-")) && !cg.startsWith(QStringLiteral("bp-"))) {
                alreadyPresentRealGuids.insert(cg);
            }
        }
    }
    QVariantList retainedOptimistic;
    QSet<QString> consumedNextGuids;
    for (const QVariant &curr : m_messages) {
        const QVariantMap cm = curr.toMap();
        const QString g = cm.value(QStringLiteral("guid")).toString();
        if (m_deletedMessageGuids.contains(g)) {
            continue;
        }
        if (g.startsWith(QStringLiteral("temp-")) || g.startsWith(QStringLiteral("bp-"))) {
            const QString optChatGuid = cm.value(QStringLiteral("chatGuid")).toString();
            if (optChatGuid.isEmpty() || optChatGuid != m_selectedChatGuid) {
                continue;
            }
            const qint64 optTs = cm.value(QStringLiteral("optimisticCreatedAtMs")).toLongLong();
            if (optTs <= 0 || (nowRebuildMs - optTs) > 45000LL) {
                continue;
            }
            const QString txt = cm.value(QStringLiteral("text")).toString().trimmed();
            const bool cmHasAtt = cm.value(QStringLiteral("hasImageAttachment")).toBool() ||
                                  cm.value(QStringLiteral("hasFileAttachment")).toBool();
            bool echoed = false;
            for (const QVariant &nxt : std::as_const(nextMessages)) {
                const QVariantMap nm = nxt.toMap();
                const QString nGuid = nm.value(QStringLiteral("guid")).toString();
                if (consumedNextGuids.contains(nGuid)) {
                    continue;
                }
                if (nGuid == g) {
                    echoed = true;
                    consumedNextGuids.insert(nGuid);
                    break;
                }
                const QString nExplicitTemp = chatMap.value(nGuid).value(QStringLiteral("tempGuid")).toString().trimmed();
                const QString nExplicitUpload = chatMap.value(nGuid).value(QStringLiteral("uploadUuid")).toString().trimmed();
                if (!nExplicitTemp.isEmpty() || !nExplicitUpload.isEmpty()) {
                    if ((!nExplicitTemp.isEmpty() && nExplicitTemp == g) ||
                        (!nExplicitUpload.isEmpty() && nExplicitUpload == g)) {
                        echoed = true;
                        consumedNextGuids.insert(nGuid);
                        break;
                    }
                    continue;
                }
                if (alreadyPresentRealGuids.contains(nGuid)) {
                    continue;
                }
                if (nm.value(QStringLiteral("isFromMe")).toBool()) {
                    const QString nTxt = nm.value(QStringLiteral("text")).toString().trimmed();
                    const bool nHasAtt = nm.value(QStringLiteral("hasImageAttachment")).toBool() ||
                                         nm.value(QStringLiteral("hasFileAttachment")).toBool();
                    if (!cmHasAtt && !nHasAtt && !txt.isEmpty() && nTxt == txt) {
                        echoed = true;
                        consumedNextGuids.insert(nGuid);
                        break;
                    }
                }
            }
            if (!echoed) {
                retainedOptimistic.append(curr);
            }
        }
    }
    auto effectiveMsgTs = [](const QVariantMap &vm) -> qint64 {
        const qint64 dc = vm.value(QStringLiteral("dateCreated")).toLongLong();
        return (dc > 0) ? dc : vm.value(QStringLiteral("optimisticCreatedAtMs")).toLongLong();
    };
    for (const QVariant &curr : std::as_const(retainedOptimistic)) {
        const QVariantMap cm = curr.toMap();
        const QString g = cm.value(QStringLiteral("guid")).toString();
        const qint64 optTs = effectiveMsgTs(cm);
        const int optPrevIdx = prevTimelineOrder.value(g, std::numeric_limits<int>::max());

        int groupEnd = nextMessages.size();
        int lastPredIdx = -1;
        for (int insIdx = 0; insIdx < nextMessages.size(); ++insIdx) {
            const QVariantMap nm = nextMessages.at(insIdx).toMap();
            const qint64 nDate = effectiveMsgTs(nm);
            if (nDate > optTs) {
                groupEnd = insIdx;
                break;
            }
            if (nDate == optTs) {
                const QString nGuid = nm.value(QStringLiteral("guid")).toString();
                const int nPrevIdx = prevTimelineOrder.value(nGuid, std::numeric_limits<int>::max());
                if (nPrevIdx != std::numeric_limits<int>::max() && nPrevIdx < optPrevIdx) {
                    lastPredIdx = insIdx;
                }
            }
        }

        int insertAt = groupEnd;
        for (int insIdx = lastPredIdx + 1; insIdx < groupEnd; ++insIdx) {
            const QVariantMap nm = nextMessages.at(insIdx).toMap();
            const qint64 nDate = effectiveMsgTs(nm);
            if (nDate == optTs) {
                const QString nGuid = nm.value(QStringLiteral("guid")).toString();
                const int nPrevIdx = prevTimelineOrder.value(nGuid, std::numeric_limits<int>::max());
                if (optPrevIdx < nPrevIdx) {
                    insertAt = insIdx;
                    break;
                }
            }
        }
        nextMessages.insert(insertAt, curr);
    }
    normalizeOutgoingReceipts(nextMessages);

    if (nextMessages != m_messages) {
        emit aboutToChangeMessages();
        m_messages = nextMessages;
        emit messagesChanged();
    }

    if (!m_demoMode) {
        if (!missingAtts.isEmpty()) {
            int cursor = static_cast<int>(missingAtts.size()) - 1;
            while (cursor >= 0 && m_downloadingAttachments.size() < 2) {
                const QString &spec = missingAtts[cursor--];
                const int sep = spec.indexOf(QLatin1Char('|'));
                if (sep > 0) {
                    downloadAttachment(spec.left(sep), spec.mid(sep + 1), true);
                }
            }
            // Queue remaining attachments in ascending order so prepend(item) leaves newest at the front of the queue
            for (int i = 0; i <= cursor; ++i) {
                const QString &spec = missingAtts[i];
                const int sep = spec.indexOf(QLatin1Char('|'));
                if (sep > 0) {
                    downloadAttachment(spec.left(sep), spec.mid(sep + 1), true);
                }
            }
        }
        for (const QString &spec : missingPreviews) {
            const int sep = spec.indexOf(QLatin1Char('|'));
            if (sep > 0) {
                fetchLinkPreview(spec.left(sep), spec.mid(sep + 1));
            }
        }
    }
}

void Backend::loadChatMessages(const QString &chatGuid) {
    if (chatGuid.isEmpty() || m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    if (tailnetCheckPending()) {  // the password can't be sent yet; load once the check ends
        m_resumeAfterTailnetCheck = true;
        return;
    }

    const quint64 reqSeq = m_messageRequestSeq;
    QJsonObject enrichBody;
    enrichBody.insert(QStringLiteral("chatGuid"), chatGuid);
    enrichBody.insert(QStringLiteral("limit"), 150);
    enrichBody.insert(QStringLiteral("offset"), 0);
    enrichBody.insert(QStringLiteral("sort"), QStringLiteral("DESC"));
    enrichBody.insert(QStringLiteral("with"), QJsonArray{
        QStringLiteral("handle"),
        QStringLiteral("attachment"),
        QStringLiteral("attributedbody")
    });

    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                      QJsonDocument(enrichBody).toJson(QJsonDocument::Compact));
    if (m_selectedChatGuid == chatGuid) {
        m_activeMessagesReply = reply;
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, chatGuid, reqSeq]() {
        reply->deleteLater();
        if (m_activeMessagesReply == reply) m_activeMessagesReply = nullptr;
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            // Fallback to GET /api/v1/chat/:guid/message if POST query fails
            const QString encodedGuid = QString::fromUtf8(QUrl::toPercentEncoding(chatGuid));
            const QString path = QStringLiteral("/api/v1/chat/%1/message?limit=150&sort=DESC&with=attachment,handle,attributedbody").arg(encodedGuid);
            QNetworkReply *fallbackReply = m_nam.get(makeRequest(path));
            if (m_selectedChatGuid == chatGuid) {
                m_activeMessagesReply = fallbackReply;
            }
            connect(fallbackReply, &QNetworkReply::finished, this, [this, fallbackReply, chatGuid, reqSeq]() {
                fallbackReply->deleteLater();
                if (m_activeMessagesReply == fallbackReply) m_activeMessagesReply = nullptr;
                if (fallbackReply->error() == QNetworkReply::OperationCanceledError) {
                    return;
                }
                if (fallbackReply->error() != QNetworkReply::NoError) {
                    handleNetworkFailure(QStringLiteral("loadChatMessages"));
                    QTimer::singleShot(1500, this, [this, chatGuid, reqSeq]() {
                        if (reqSeq == m_messageRequestSeq && m_selectedChatGuid == chatGuid && !m_activeMessagesReply) {
                            loadChatMessages(chatGuid);
                        }
                    });
                    return;
                }
                handleNetworkSuccess();
                const QJsonArray arr = QJsonDocument::fromJson(fallbackReply->readAll()).object().value(QStringLiteral("data")).toArray();
                reconcileChatMessages(chatGuid, arr, true);
                m_chatMessageOffset[chatGuid] = arr.size();
                m_chatHasMoreMessages[chatGuid] = (arr.size() >= 150);
                saveMessagesCache();
                if (reqSeq != m_messageRequestSeq || m_selectedChatGuid != chatGuid) return;
                emit olderMessagesChanged();
                rebuildSelectedChatMessages();
            });
            return;
        }
        handleNetworkSuccess();
        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
        reconcileChatMessages(chatGuid, arr, true);
        m_chatMessageOffset[chatGuid] = arr.size();
        m_chatHasMoreMessages[chatGuid] = (arr.size() >= 150);
        saveMessagesCache();
        if (reqSeq != m_messageRequestSeq || m_selectedChatGuid != chatGuid) return;
        emit olderMessagesChanged();
        rebuildSelectedChatMessages();
    });
}

void Backend::reconcileChatMessagesForTest(const QString &chatGuid, const QJsonArray &serverMessages) {
    reconcileChatMessages(chatGuid, serverMessages, true);
    if (!m_stateFilePath.isEmpty()) {
        flushMessagesCacheNow(false);
    }
    if (chatGuid == m_selectedChatGuid) {
        rebuildSelectedChatMessages();
    }
}

void Backend::reconcileChatMessages(const QString &chatGuid, const QJsonArray &serverMessages, bool isFullWindow) {
    if (chatGuid.isEmpty()) return;

    QSet<QString> validServerGuids;
    qint64 oldestServerTs = 0;
    bool deletionsDetected = false;

    for (const QJsonValue &v : serverMessages) {
        const QJsonObject msgObj = v.toObject();
        const QString g = msgObj.value(QStringLiteral("guid")).toString();
        if (g.isEmpty()) continue;

        const qint64 dc = msgObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        if (dc > 0 && (oldestServerTs == 0 || dc < oldestServerTs)) {
            oldestServerTs = dc;
        }

        if (m_deletedMessageGuids.contains(g) || isMessageDeletedOrRetracted(msgObj)) {
            if (isMessageDeletedOrRetracted(msgObj) && !m_deletedMessageGuids.contains(g)) {
                m_deletedMessageGuids.insert(g);
                deletionsDetected = true;
            }
            if (m_chatMessagesCache.contains(chatGuid) && m_chatMessagesCache[chatGuid].contains(g)) {
                m_chatMessagesCache[chatGuid].remove(g);
                deletionsDetected = true;
            }
            clearPendingDeliveryTrackingForGuid(g, true);
            m_reconcileRemovedSeqByGuid.insert(g, ++m_historyReconcileSeq);
            continue;
        }

        validServerGuids.insert(g);
        m_seenMessageGuids.insert(g);
        cacheMessageForChat(chatGuid, msgObj);
        prefetchMessageAttachments(msgObj, chatGuid == m_selectedChatGuid);
    }

    // Reconcile messages that were deleted on iPhone/Mac/iCloud and are no longer in chat.db
    if (m_chatMessagesCache.contains(chatGuid)) {
        auto &chatMap = m_chatMessagesCache[chatGuid];
        const bool serverReturnedCompleteHistory = isFullWindow && (serverMessages.size() < 150);
        const qint64 nowRecMs = QDateTime::currentMSecsSinceEpoch();
        QStringList toRemove;
        for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
            const QString cachedGuid = it.key();
            if (cachedGuid.startsWith(QStringLiteral("temp-")) || cachedGuid.startsWith(QStringLiteral("bp-"))) {
                const qint64 cachedTs = it.value().value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                const QString cachedTxt = extractMessageText(it.value()).trimmed();
                bool matchedReal = false;
                for (const QJsonValue &sv : serverMessages) {
                    const QJsonObject sObj = sv.toObject();
                    if (!sObj.value(QStringLiteral("isFromMe")).toBool()) continue;
                    const QString sTemp = sObj.value(QStringLiteral("tempGuid")).toString().trimmed();
                    const QString sUpload = sObj.value(QStringLiteral("uploadUuid")).toString().trimmed();
                    if (sTemp == cachedGuid || (!sUpload.isEmpty() && sUpload == cachedGuid) ||
                        (!cachedTxt.isEmpty() && extractMessageText(sObj).trimmed() == cachedTxt)) {
                        matchedReal = true;
                        break;
                    }
                }
                if (matchedReal || (cachedTs > 0 && (nowRecMs - cachedTs) > 45000LL)) {
                    toRemove.append(cachedGuid);
                }
                continue;
            }
            if (m_deletedMessageGuids.contains(cachedGuid) || isMessageDeletedOrRetracted(it.value())) {
                toRemove.append(cachedGuid);
                continue;
            }
            if (!validServerGuids.contains(cachedGuid)) {
                const bool isFromMe = it.value().value(QStringLiteral("isFromMe")).toBool();
                const qint64 cachedTs = it.value().value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                if (isFromMe && (m_pendingDeliveryMessages.contains(cachedGuid) ||
                                 (cachedTs > 0 && (nowRecMs - cachedTs) < 60000LL))) {
                    continue;
                }
                if (serverReturnedCompleteHistory || (oldestServerTs > 0 && cachedTs >= oldestServerTs)) {
                    toRemove.append(cachedGuid);
                }
            }
        }
        if (!toRemove.isEmpty()) {
            ++m_historyReconcileSeq;
            for (const QString &remGuid : toRemove) {
                chatMap.remove(remGuid);
                clearPendingDeliveryTrackingForGuid(remGuid, true);
                m_reconcileRemovedSeqByGuid.insert(remGuid, m_historyReconcileSeq);
            }
            deletionsDetected = true;
        }
    }

    if (deletionsDetected) {
        saveAppState();
        refreshChatPreviewFromCache(chatGuid);
    }
}

void Backend::refreshChatPreviewFromCache(const QString &chatGuid) {
    if (chatGuid.isEmpty()) return;

    int chatIdx = -1;
    for (int i = 0; i < m_chats.size(); ++i) {
        if (m_chats[i].guid == chatGuid) {
            chatIdx = i;
            break;
        }
    }
    if (chatIdx < 0) return;

    qint64 bestDate = 0;
    QString bestPreview;
    int unreadCount = 0;
    const qint64 wm = m_readWatermarks.value(chatGuid, 0);
    const qint64 uo = m_unreadOverrides.value(chatGuid, 0);

    if (m_chatMessagesCache.contains(chatGuid)) {
        const auto &chatMap = m_chatMessagesCache.value(chatGuid);
        for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
            const QJsonObject &m = it.value();
            const QString g = it.key();
            if (m_deletedMessageGuids.contains(g) || isMessageDeletedOrRetracted(m)) continue;

            if (isMessageUnread(m, wm, uo)) {
                unreadCount++;
            }

            const qint64 dc = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
            QString preview = extractMessageText(m);
            const QString assocType = m.value(QStringLiteral("associatedMessageType")).toVariant().toString();
            const QString emoji = mapReactionEmoji(assocType, preview);
            if (!emoji.isEmpty()) {
                preview = QStringLiteral("%1 Reacted to a message").arg(emoji);
            }
            if (preview.isEmpty()) continue;

            if (dc >= bestDate) {
                bestDate = dc;
                bestPreview = preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
            }
        }
    }

    ChatEntry &entry = m_chats[chatIdx];
    entry.unreadCount = unreadCount;
    if (bestDate > 0 && !bestPreview.isEmpty()) {
        entry.lastMessageDate = bestDate;
        entry.lastMessagePreview = bestPreview;
        entry.relativeTime = formatRelativeTime(bestDate);
    } else {
        entry.lastMessageDate = 1;
        entry.lastMessagePreview = QStringLiteral("No messages");
        entry.relativeTime = QString();
    }

    sortChats();
    emit chatsChanged();
    if (chatGuid == m_selectedChatGuid || resolveCanonicalChatGuid(chatGuid) == m_selectedChatGuid) {
        emit selectedChatChanged();
    }
}

void Backend::loadOlderMessages() {
    if (m_selectedChatGuid.isEmpty() || m_loadingOlderMessages || !hasOlderMessages() || m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;

    const QString chatGuid = m_selectedChatGuid;
    const int currentOffset = m_chatMessageOffset.value(chatGuid, 0);
    m_loadingOlderMessages = true;
    emit olderMessagesChanged();

    const quint64 reqSeq = m_messageRequestSeq;
    QJsonObject enrichBody;
    enrichBody.insert(QStringLiteral("chatGuid"), chatGuid);
    enrichBody.insert(QStringLiteral("limit"), 500);
    enrichBody.insert(QStringLiteral("offset"), currentOffset);
    enrichBody.insert(QStringLiteral("sort"), QStringLiteral("DESC"));
    enrichBody.insert(QStringLiteral("with"), QJsonArray{
        QStringLiteral("handle"),
        QStringLiteral("attachment"),
        QStringLiteral("attributedbody")
    });

    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                      QJsonDocument(enrichBody).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, chatGuid, reqSeq, currentOffset]() {
        reply->deleteLater();
        m_loadingOlderMessages = false;
        emit olderMessagesChanged();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkFailure(QStringLiteral("loadOlderMessages"));
            return;
        }
        handleNetworkSuccess();
        if (reqSeq != m_messageRequestSeq || m_selectedChatGuid != chatGuid) return;

        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
        for (const QJsonValue &v : arr) {
            const QJsonObject msgObj = v.toObject();
            const QString g = msgObj.value(QStringLiteral("guid")).toString();
            if (!g.isEmpty()) m_seenMessageGuids.insert(g);
            cacheMessageForChat(chatGuid, msgObj);
            prefetchMessageAttachments(msgObj, true);
        }
        m_chatMessageOffset[chatGuid] = currentOffset + arr.size();
        m_chatHasMoreMessages[chatGuid] = (arr.size() >= 500);
        emit olderMessagesChanged();
        rebuildSelectedChatMessages();
    });
}

void Backend::prefetchMessageAttachments(const QJsonObject &msgObj, bool highPriority) {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    const QString msgGuid = msgObj.value(QStringLiteral("guid")).toString();
    if (!msgGuid.isEmpty() && (m_deletedMessageGuids.contains(msgGuid) || isMessageDeletedOrRetracted(msgObj))) {
        return;
    }
    const QString assocGuid = normalizeAssociatedGuid(msgObj.value(QStringLiteral("associatedMessageGuid")).toString());
    const QString assocType = msgObj.value(QStringLiteral("associatedMessageType")).toVariant().toString();
    const QString text = extractMessageText(msgObj);
    if (!assocGuid.isEmpty() && !mapReactionEmoji(assocType, text).isEmpty()) {
        return;
    }
    const QJsonArray atts = deduplicateMessageAttachments(msgObj.value(QStringLiteral("attachments")).toArray());
    for (const QJsonValue &av : atts) {
        const QJsonObject aObj = av.toObject();
        const QString attGuid = aObj.value(QStringLiteral("guid")).toString();
        const QString tName = aObj.value(QStringLiteral("transferName")).toString();
        const QString mime = aObj.value(QStringLiteral("mimeType")).toString();
        if (!attGuid.isEmpty() && isImageMimeOrExt(mime, tName)) {
            downloadAttachment(attGuid, tName, highPriority);
        }
    }
}

void Backend::prefetchRecentChatAttachments() {
    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;
    QList<QList<QJsonObject>> sortedMsgsPerChat;
    int chatsChecked = 0;
    for (const ChatEntry &c : m_chats) {
        if (c.guid.isEmpty()) continue;
        if (c.guid == m_selectedChatGuid) continue;
        if (!m_chatMessagesCache.contains(c.guid)) continue;
        if (++chatsChecked > 25) break;

        QList<QJsonObject> msgs = m_chatMessagesCache.value(c.guid).values();
        std::sort(msgs.begin(), msgs.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("dateCreated")).toVariant().toLongLong() >
                   b.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        });
        sortedMsgsPerChat.append(msgs);
    }

    // Round-robin by depth (newest messages across all recent chats first)
    for (int depth = 0; depth < 10; ++depth) {
        for (const QList<QJsonObject> &msgs : sortedMsgsPerChat) {
            if (depth < msgs.size()) {
                prefetchMessageAttachments(msgs[depth], false);
            }
        }
    }
}

void Backend::pumpAttachmentDownloadQueue() {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    while (m_downloadingAttachments.size() < 2 && !m_attachmentDownloadQueue.isEmpty()) {
        const QPair<QString, QString> next = m_attachmentDownloadQueue.takeFirst();
        if (!next.first.isEmpty() &&
            !m_downloadingAttachments.contains(next.first) &&
            !m_attemptedAttachments.contains(next.first) &&
            m_missingAttachmentCooldownUntilMs.value(next.first, 0) <= nowMs) {
            downloadAttachment(next.first, next.second, false);
        }
    }
}

QString Backend::cachedAttachmentUrl(const QString &attachmentGuid) const {
    if (attachmentGuid.isEmpty()) return QString();
    const QString path = lookupCachedAttachmentPath(m_attachmentPaths, attachmentGuid);
    if (path.isEmpty() || !QFileInfo::exists(path)) return QString();
    return QUrl::fromLocalFile(path).toString();
}

void Backend::downloadAttachment(const QString &attachmentGuid, const QString &transferName, bool highPriority) {
    if (attachmentGuid.isEmpty() ||
        m_downloadingAttachments.contains(attachmentGuid) ||
        m_attemptedAttachments.contains(attachmentGuid)) {
        return;
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_missingAttachmentCooldownUntilMs.value(attachmentGuid, 0) > nowMs) {
        return;
    }
    const QString existingCached = lookupCachedAttachmentPath(m_attachmentPaths, attachmentGuid);
    if (!existingCached.isEmpty() && QFileInfo::exists(existingCached) && QFileInfo(existingCached).size() >= 100) {
        return;
    }

    const QString cacheDir = QDir::homePath() + QStringLiteral("/.cache/imsg/attachments");
    QDir().mkpath(cacheDir);
    QString safeName = transferName;
    safeName.replace(QRegularExpression(QStringLiteral(R"([/\\:])")), QStringLiteral("_"));
    if (safeName.isEmpty()) safeName = QStringLiteral("attachment.jpg");

    auto notifyAttachmentResolved = [this](const QString &attGuid, const QString &effectivePath) {
        m_attemptedAttachments.remove(attGuid);
        m_missingAttachmentCooldownUntilMs.remove(attGuid);
        m_attachmentRetryCount.remove(attGuid);
        insertCachedAttachmentPath(m_attachmentPaths, attGuid, effectivePath);
        const QString localUrl = QUrl::fromLocalFile(effectivePath).toString();
        for (int i = 0; i < m_messages.size(); ++i) {
            QVariantMap map = m_messages[i].toMap();
            bool msgModified = false;
            const QString msgAttGuid = map.value(QStringLiteral("attachmentGuid")).toString();
            if (attachmentGuidsMatch(msgAttGuid, attGuid)) {
                if (map.value(QStringLiteral("attachmentLocalUrl")).toString() != localUrl) {
                    map.insert(QStringLiteral("attachmentLocalUrl"), localUrl);
                    msgModified = true;
                }
            }
            auto updateAttList = [&](const QString &listKey) {
                QVariantList list = map.value(listKey).toList();
                bool listChanged = false;
                for (int j = 0; j < list.size(); ++j) {
                    QVariantMap aMap = list[j].toMap();
                    const QString g = aMap.value(QStringLiteral("guid")).toString();
                    if (attachmentGuidsMatch(g, attGuid)) {
                        if (aMap.value(QStringLiteral("localUrl")).toString() != localUrl) {
                            aMap.insert(QStringLiteral("localUrl"), localUrl);
                            list[j] = aMap;
                            listChanged = true;
                        }
                    }
                }
                if (listChanged) {
                    map.insert(listKey, list);
                    msgModified = true;
                }
            };
            updateAttList(QStringLiteral("imageAttachments"));
            updateAttList(QStringLiteral("attachments"));
            // Updated in place without messagesChanged(): the timeline's model is
            // a plain list, so that signal makes ListView destroy and rebuild every
            // delegate (one full rebuild per image while a chat opens). Visible
            // delegates pick the file up from attachmentReady below; delegates
            // created later ask cachedAttachmentUrl().
            if (msgModified) {
                m_messages[i] = map;
            }
        }
        bool photosEvictionResolved = false;
        for (int i = 0; i < m_icloudPhotos.size(); ++i) {
            QVariantMap pMap = m_icloudPhotos.at(i).toMap();
            const QString pg = pMap.value(QStringLiteral("guid")).toString();
            if (attachmentGuidsMatch(pg, attGuid)) {
                const bool wasEvicted = pMap.value(QStringLiteral("isCloudEvicted")).toBool();
                if (pMap.value(QStringLiteral("localUrl")).toString() != localUrl ||
                    pMap.value(QStringLiteral("previewUrl")).toString() != localUrl ||
                    wasEvicted) {
                    pMap.insert(QStringLiteral("localUrl"), localUrl);
                    pMap.insert(QStringLiteral("previewUrl"), localUrl);
                    pMap.insert(QStringLiteral("isCloudEvicted"), false);
                    m_icloudPhotos[i] = pMap;
                    if (wasEvicted) {
                        photosEvictionResolved = true;
                    }
                }
            }
        }
        if (photosEvictionResolved) {
            emit icloudPhotosChanged();
        }
        emit attachmentReady(attGuid, localUrl);
        if (!m_pendingClipboardCopyGuid.isEmpty() && attachmentGuidsMatch(attGuid, m_pendingClipboardCopyGuid)) {
            m_pendingClipboardCopyGuid.clear();
            copyImageToClipboard(effectivePath, attGuid);
        }
    };

    auto resolveOrConvertAsync = [this, notifyAttachmentResolved](const QString &attGuid, const QString &srcPath) {
        const QString lower = srcPath.toLower();
        QByteArray hdr;
        QFile hdrFile(srcPath);
        if (hdrFile.open(QIODevice::ReadOnly)) {
            hdr = hdrFile.read(16);
            hdrFile.close();
        }
        const bool isHeifMagic = (hdr.size() >= 12 && hdr.mid(4, 4) == QByteArrayLiteral("ftyp"));
        const bool isNativeQtRaster = hdr.startsWith("\xFF\xD8\xFF") || hdr.startsWith("\x89PNG") ||
                                      hdr.startsWith("GIF8") || (hdr.size() >= 12 && hdr.startsWith("RIFF") && hdr.mid(8, 4) == QByteArrayLiteral("WEBP"));
        const bool isNonNativeExt = lower.endsWith(QStringLiteral(".heic")) || lower.endsWith(QStringLiteral(".heif")) ||
                                    lower.endsWith(QStringLiteral(".heics")) || lower.endsWith(QStringLiteral(".avif")) ||
                                    lower.endsWith(QStringLiteral(".tiff")) || lower.endsWith(QStringLiteral(".tif")) ||
                                    lower.endsWith(QStringLiteral(".bmp"));

        if (isHeifMagic || isNonNativeExt) {
            const bool inPlaceJpg = lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg"));
            const QString outTarget = inPlaceJpg
                                          ? (srcPath + QStringLiteral(".conv.jpg"))
                                          : (srcPath + QStringLiteral(".jpg"));
            if (!inPlaceJpg && QFileInfo::exists(outTarget) && QFileInfo(outTarget).size() >= 100) {
                QFile jf(outTarget);
                if (jf.open(QIODevice::ReadOnly)) {
                    const QByteArray jhdr = jf.read(12);
                    jf.close();
                    if (!(jhdr.size() >= 12 && jhdr.mid(4, 4) == QByteArrayLiteral("ftyp"))) {
                        notifyAttachmentResolved(attGuid, outTarget);
                        return;
                    }
                }
            }
            if (!inPlaceJpg && isNativeQtRaster) {
                QFile::remove(outTarget);
                if (QFile::copy(srcPath, outTarget)) {
                    notifyAttachmentResolved(attGuid, outTarget);
                    return;
                }
            }
            // Asynchronous non-blocking HEIF/HEIC/HEICS/AVIF/TIFF conversion on background process
            QProcess *proc = new QProcess(this);
            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [proc, attGuid, srcPath, outTarget, inPlaceJpg, notifyAttachmentResolved](int, QProcess::ExitStatus) {
                        proc->deleteLater();
                        if (QFileInfo::exists(outTarget) && QFileInfo(outTarget).size() >= 100) {
                            if (inPlaceJpg) {
                                QFile::remove(srcPath);
                                QFile::rename(outTarget, srcPath);
                                notifyAttachmentResolved(attGuid, srcPath);
                            } else {
                                notifyAttachmentResolved(attGuid, outTarget);
                            }
                        } else if (!inPlaceJpg) {
                            notifyAttachmentResolved(attGuid, srcPath);
                        }
                    });
            proc->start(QStringLiteral("sh"), QStringList{
                QStringLiteral("-c"),
                QStringLiteral("heif-convert -q 90 \"$1\" \"$2\" >/dev/null 2>&1 || magick \"$1[0]\" -auto-orient -quality 90 \"$2\" >/dev/null 2>&1 || ffmpeg -y -loglevel error -i \"$1\" -frames:v 1 -update 1 -f image2 -c:v mjpeg \"$2\" >/dev/null 2>&1"),
                QStringLiteral("--"),
                srcPath,
                outTarget
            });
            return;
        }
        notifyAttachmentResolved(attGuid, srcPath);
    };

    const QString destPath = cacheDir + QLatin1Char('/') + attachmentGuid.left(16) + QLatin1Char('_') + safeName;
    if (QFileInfo::exists(destPath) && QFileInfo(destPath).size() >= 100) {
        m_attemptedAttachments.insert(attachmentGuid);
        resolveOrConvertAsync(attachmentGuid, destPath);
        return;
    }

    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) {
        return;
    }

    if (m_downloadingAttachments.size() >= 2) {
        const QPair<QString, QString> item(attachmentGuid, transferName);
        m_attachmentDownloadQueue.removeAll(item);
        if (highPriority) {
            m_attachmentDownloadQueue.prepend(item);
        } else {
            m_attachmentDownloadQueue.append(item);
        }
        return;
    }

    m_downloadingAttachments.insert(attachmentGuid);
    const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(attachmentGuid));
    // Request /api/v1/attachment/%1/live so macOS bluepill-server converts .heic/.tiff to cached JPEG via sips
    QNetworkRequest req = makeRequest(QStringLiteral("/api/v1/attachment/%1/download?force=false&original=true").arg(encoded));
    req.setPriority(highPriority ? QNetworkRequest::HighPriority : QNetworkRequest::LowPriority);
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_mediaNam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, attachmentGuid, transferName, highPriority, destPath, resolveOrConvertAsync]() {
        reply->deleteLater();
        m_downloadingAttachments.remove(attachmentGuid);
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray bytes = reply->isOpen() ? reply->readAll() : QByteArray();
        if (reply->error() != QNetworkReply::NoError || (httpStatus != 0 && httpStatus != 200) || bytes.size() < 100) {
            const qint64 finishedMs = QDateTime::currentMSecsSinceEpoch();
            const bool isLiveIncoming = m_liveIncomingAttachments.contains(attachmentGuid);
            const bool permanentlyMissingOnDisk = bytes.contains("does not exist") && !isLiveIncoming;
            const int retries = m_attachmentRetryCount.value(attachmentGuid, 0);
            const bool isPermanentMissing = (httpStatus == 410 || permanentlyMissingOnDisk ||
                                             (httpStatus == 404 && (!isLiveIncoming || retries >= 6)));
            if (!isPermanentMissing && retries < 6) {
                m_attachmentRetryCount.insert(attachmentGuid, retries + 1);
                m_attemptedAttachments.insert(attachmentGuid);
                const int delayMs = (retries == 0) ? 500 : ((retries == 1) ? 1000 : ((retries == 2) ? 2000 : ((retries == 3) ? 3500 : 6000)));
                QTimer::singleShot(delayMs, this, [this, attachmentGuid, transferName, highPriority]() {
                    m_attemptedAttachments.remove(attachmentGuid);
                    downloadAttachment(attachmentGuid, transferName, highPriority);
                });
            } else {
                m_missingAttachmentCooldownUntilMs.insert(
                    attachmentGuid,
                    finishedMs + (isPermanentMissing ? 1800000LL : 300000LL));
            }
            pumpAttachmentDownloadQueue();
            return;
        }

        const QString tmpDest = destPath + QStringLiteral(".tmp");
        QFile f(tmpDest);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(bytes);
            f.close();
            QFile::remove(destPath);
            QFile::rename(tmpDest, destPath);
            resolveOrConvertAsync(attachmentGuid, destPath);
        }
        pumpAttachmentDownloadQueue();
    });
}

bool Backend::isDecodableImageData(const QByteArray &bytes) {
    // og:image / oEmbed thumbnails sometimes answer with an HTML error or
    // consent page; saving that as *_thumb.jpg left a blank card and a
    // "Unsupported image format" warning on every repaint.
    // Decode fully: a valid header with corrupt pixel data passes canRead().
    if (bytes.isEmpty()) return false;
    QBuffer buf;
    buf.setData(bytes);
    if (!buf.open(QIODevice::ReadOnly)) return false;
    QImageReader reader(&buf);
    if (!reader.canRead()) return false;
    const QImage img = reader.read();
    return !img.isNull() && reader.error() == QImageReader::UnknownError;
}

bool Backend::isDecodableImageFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return isDecodableImageData(f.readAll());
}

void Backend::fetchLinkPreview(const QString &msgGuid, const QString &url) {
    if (url.isEmpty()) return;

    const QString previewDir = QDir::homePath() + QStringLiteral("/.cache/imsg/previews");
    QDir().mkpath(previewDir);

    const QString urlHash = QString::fromUtf8(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString jsonPath = previewDir + QLatin1Char('/') + urlHash + QStringLiteral(".json");
    const QString thumbPath = previewDir + QLatin1Char('/') + urlHash + QStringLiteral("_thumb.jpg");

    auto applyPreview = [this, msgGuid, url](const QVariantMap &previewData) {
        m_linkPreviewCache.insert(url, previewData);
        for (int i = 0; i < m_messages.size(); ++i) {
            QVariantMap map = m_messages[i].toMap();
            if (map.value(QStringLiteral("guid")).toString() == msgGuid || map.value(QStringLiteral("linkUrl")).toString() == url) {
                map.insert(QStringLiteral("hasLinkPreview"), true);
                map.insert(QStringLiteral("linkPreview"), previewData);
                m_messages[i] = map;
            }
        }
        emit linkPreviewUpdated(msgGuid, previewData);
    };

    if (QFileInfo::exists(jsonPath)) {
        QFile jf(jsonPath);
        if (jf.open(QIODevice::ReadOnly)) {
            QJsonObject obj = QJsonDocument::fromJson(jf.readAll()).object();
            jf.close();
            QVariantMap previewData = obj.toVariantMap();
            if (isDecodableImageFile(thumbPath)) {
                previewData.insert(QStringLiteral("thumbnail"), QUrl::fromLocalFile(thumbPath).toString());
            }
            applyPreview(previewData);
            return;
        }
    }

    if (m_attemptedLinkPreviews.contains(url)) return;
    m_attemptedLinkPreviews.insert(url);

    const QUrl parsedUrl(url);
    const QString host = parsedUrl.host().toLower();
    const bool isYt = isYouTubeUrl(url);

    if (isYt) {
        const QString oembedUrl = QStringLiteral("https://www.youtube.com/oembed?url=%1&format=json").arg(QString::fromUtf8(QUrl::toPercentEncoding(url)));
        QNetworkRequest req((QUrl(oembedUrl)));
        req.setTransferTimeout(8000);
        QNetworkReply *reply = m_mediaNam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, msgGuid, url, host, jsonPath, thumbPath, applyPreview]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;
            const QJsonObject oembed = QJsonDocument::fromJson(reply->readAll()).object();
            const QString title = oembed.value(QStringLiteral("title")).toString();
            const QString author = oembed.value(QStringLiteral("author_name")).toString();
            const QString remoteThumb = oembed.value(QStringLiteral("thumbnail_url")).toString();

            QJsonObject cacheObj;
            cacheObj.insert(QStringLiteral("url"), url);
            cacheObj.insert(QStringLiteral("title"), title);
            cacheObj.insert(QStringLiteral("description"), author);
            cacheObj.insert(QStringLiteral("domain"), host.isEmpty() ? QStringLiteral("youtube.com") : host);
            cacheObj.insert(QStringLiteral("isYouTube"), true);

            QVariantMap previewData = cacheObj.toVariantMap();

            if (!remoteThumb.isEmpty()) {
                QNetworkRequest thumbReq((QUrl(remoteThumb)));
                thumbReq.setTransferTimeout(8000);
                QNetworkReply *thumbReply = m_mediaNam.get(thumbReq);
                connect(thumbReply, &QNetworkReply::finished, this, [this, thumbReply, jsonPath, thumbPath, cacheObj, previewData, applyPreview]() mutable {
                    thumbReply->deleteLater();
                    if (thumbReply->error() == QNetworkReply::NoError) {
                        const QByteArray tb = thumbReply->readAll();
                        if (isDecodableImageData(tb)) {
                            QFile tf(thumbPath);
                            if (tf.open(QIODevice::WriteOnly)) {
                                tf.write(tb);
                                tf.close();
                                previewData.insert(QStringLiteral("thumbnail"), QUrl::fromLocalFile(thumbPath).toString());
                            }
                        }
                    }
                    QFile jf(jsonPath);
                    if (jf.open(QIODevice::WriteOnly)) {
                        jf.write(QJsonDocument(cacheObj).toJson(QJsonDocument::Compact));
                        jf.close();
                    }
                    applyPreview(previewData);
                });
            } else {
                QFile jf(jsonPath);
                if (jf.open(QIODevice::WriteOnly)) {
                    jf.write(QJsonDocument(cacheObj).toJson(QJsonDocument::Compact));
                    jf.close();
                }
                applyPreview(previewData);
            }
        });
    } else {
        QNetworkRequest req(parsedUrl);
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (compatible; bluepill/1.1.0; +https://github.com/nertwork/bluepill)"));
        req.setTransferTimeout(8000);
        QNetworkReply *reply = m_mediaNam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, msgGuid, url, host, jsonPath, thumbPath, applyPreview]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;
            const QString html = QString::fromUtf8(reply->read(262144));

            auto extractMeta = [&html](const QString &property) -> QString {
                QRegularExpression re(QStringLiteral(R"(<meta\s+[^>]*(?:property|name)=["']%1["'][^>]*content=["']([^"']*)["'])").arg(QRegularExpression::escape(property)), QRegularExpression::CaseInsensitiveOption);
                auto m = re.match(html);
                if (m.hasMatch()) return m.captured(1).trimmed();
                QRegularExpression re2(QStringLiteral(R"(<meta\s+[^>]*content=["']([^"']*)["'][^>]*(?:property|name)=["']%1["'])").arg(QRegularExpression::escape(property)), QRegularExpression::CaseInsensitiveOption);
                auto m2 = re2.match(html);
                if (m2.hasMatch()) return m2.captured(1).trimmed();
                return QString();
            };

            QString title = extractMeta(QStringLiteral("og:title"));
            if (title.isEmpty()) title = extractMeta(QStringLiteral("twitter:title"));
            if (title.isEmpty()) {
                QRegularExpression titleTag(QStringLiteral(R"(<title[^>]*>([^<]*)</title>)"), QRegularExpression::CaseInsensitiveOption);
                auto tm = titleTag.match(html);
                if (tm.hasMatch()) title = tm.captured(1).trimmed();
            }

            QString desc = extractMeta(QStringLiteral("og:description"));
            if (desc.isEmpty()) desc = extractMeta(QStringLiteral("description"));
            if (desc.isEmpty()) desc = extractMeta(QStringLiteral("twitter:description"));

            QString siteName = extractMeta(QStringLiteral("og:site_name"));
            if (siteName.isEmpty()) siteName = host;

            QString ogImage = extractMeta(QStringLiteral("og:image"));
            if (ogImage.isEmpty()) ogImage = extractMeta(QStringLiteral("twitter:image"));

            if (title.isEmpty() && desc.isEmpty()) return;

            QJsonObject cacheObj;
            cacheObj.insert(QStringLiteral("url"), url);
            cacheObj.insert(QStringLiteral("title"), title);
            cacheObj.insert(QStringLiteral("description"), desc);
            cacheObj.insert(QStringLiteral("domain"), siteName);
            cacheObj.insert(QStringLiteral("isYouTube"), false);

            QVariantMap previewData = cacheObj.toVariantMap();

            if (!ogImage.isEmpty()) {
                QUrl imgUrl(ogImage);
                if (imgUrl.isRelative()) imgUrl = QUrl(url).resolved(imgUrl);
                QNetworkRequest imgReq(imgUrl);
                imgReq.setTransferTimeout(8000);
                QNetworkReply *imgReply = m_mediaNam.get(imgReq);
                connect(imgReply, &QNetworkReply::finished, this, [this, imgReply, jsonPath, thumbPath, cacheObj, previewData, applyPreview]() mutable {
                    imgReply->deleteLater();
                    if (imgReply->error() == QNetworkReply::NoError) {
                        const QByteArray tb = imgReply->readAll();
                        if (isDecodableImageData(tb)) {
                            QFile tf(thumbPath);
                            if (tf.open(QIODevice::WriteOnly)) {
                                tf.write(tb);
                                tf.close();
                                previewData.insert(QStringLiteral("thumbnail"), QUrl::fromLocalFile(thumbPath).toString());
                            }
                        }
                    }
                    QFile jf(jsonPath);
                    if (jf.open(QIODevice::WriteOnly)) {
                        jf.write(QJsonDocument(cacheObj).toJson(QJsonDocument::Compact));
                        jf.close();
                    }
                    applyPreview(previewData);
                });
            } else {
                QFile jf(jsonPath);
                if (jf.open(QIODevice::WriteOnly)) {
                    jf.write(QJsonDocument(cacheObj).toJson(QJsonDocument::Compact));
                    jf.close();
                }
                applyPreview(previewData);
            }
        });
    }
}

void Backend::markChatRead(const QString &chatGuid) {
    if (chatGuid.isEmpty()) return;
    const QString resolvedGuid = resolveCanonicalChatGuid(chatGuid);
    m_localReadMutationTimeMs[resolvedGuid] = QDateTime::currentMSecsSinceEpoch();
    m_unreadOverrides.remove(resolvedGuid);
    qint64 maxMsgDate = 0;
    if (m_chatMessagesCache.contains(resolvedGuid)) {
        const auto &chatMap = m_chatMessagesCache.value(resolvedGuid);
        for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
            maxMsgDate = qMax(maxMsgDate, it.value().value(QStringLiteral("dateCreated")).toVariant().toLongLong());
        }
    }
    for (ChatEntry &c : m_chats) {
        if (c.guid == resolvedGuid) {
            c.unreadCount = 0;
            m_readWatermarks[resolvedGuid] = qMax(m_readWatermarks.value(resolvedGuid, 0), qMax(c.lastMessageDate, maxMsgDate));
            break;
        }
    }
    if (!m_chats.isEmpty()) {
        saveAppState();
    }
    // Only mark incoming (!isFromMe) cached messages in m_chatMessagesCache[resolvedGuid] as isRead = true;
    // outgoing dateRead belongs strictly to recipient read receipts from the relay.
    if (m_chatMessagesCache.contains(resolvedGuid)) {
        auto &chatMap = m_chatMessagesCache[resolvedGuid];
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (auto it = chatMap.begin(); it != chatMap.end(); ++it) {
            QJsonObject m = it.value();
            if (m.value(QStringLiteral("isFromMe")).toBool()) {
                continue;
            }
            m.insert(QStringLiteral("isRead"), true);
            m.insert(QStringLiteral("dateRead"), nowMs);
            it.value() = m;
        }
        saveMessagesCache();
    }
    emit chatsChanged();
    if (resolvedGuid == m_selectedChatGuid) {
        emit selectedChatChanged();
    }

    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;

    const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(resolvedGuid));
    QNetworkRequest req = makeRequest(QStringLiteral("/api/v1/chat/%1/read").arg(encoded));
    req.setPriority(QNetworkRequest::HighPriority);
    QNetworkReply *reply = m_sendNam.post(req, QByteArray("{}"));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void Backend::markChatUnread(const QString &chatGuid) {
    if (chatGuid.isEmpty()) return;
    const QString resolvedGuid = resolveCanonicalChatGuid(chatGuid);
    m_localReadMutationTimeMs[resolvedGuid] = QDateTime::currentMSecsSinceEpoch();
    qint64 latestIncomingDate = 0;
    qint64 latestIncomingReadDate = 0;
    QString latestMsgGuid;

    if (m_chatMessagesCache.contains(resolvedGuid)) {
        auto &chatMap = m_chatMessagesCache[resolvedGuid];
        for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
            const QJsonObject &m = it.value();
            if (!m.value(QStringLiteral("isFromMe")).toBool() && m.value(QStringLiteral("itemType")).toInt(0) == 0) {
                const QString assocType = m.value(QStringLiteral("associatedMessageType")).toVariant().toString();
                if (!assocType.isEmpty() && assocType != QStringLiteral("0")) continue;
                const qint64 d = m.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
                if (d > latestIncomingDate) {
                    latestIncomingDate = d;
                    latestIncomingReadDate = m.value(QStringLiteral("dateRead")).toVariant().toLongLong();
                    latestMsgGuid = it.key();
                }
            }
        }
        if (!latestMsgGuid.isEmpty() && chatMap.contains(latestMsgGuid)) {
            QJsonObject m = chatMap.value(latestMsgGuid);
            m.insert(QStringLiteral("isRead"), false);
            chatMap.insert(latestMsgGuid, m);
            saveMessagesCache();
        }
    }

    for (ChatEntry &c : m_chats) {
        if (c.guid == resolvedGuid) {
            c.unreadCount = 1;
            const qint64 targetUnreadDate = (latestIncomingDate > 0) ? latestIncomingDate : c.lastMessageDate;
            m_readWatermarks[resolvedGuid] = qMax(0LL, targetUnreadDate - 1LL);
            m_unreadOverrides[resolvedGuid] = qMax(targetUnreadDate, latestIncomingReadDate);
            saveAppState();
            break;
        }
    }
    emit chatsChanged();
    if (resolvedGuid == m_selectedChatGuid) {
        emit selectedChatChanged();
    }

    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;

    const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(resolvedGuid));
    QNetworkRequest req = makeRequest(QStringLiteral("/api/v1/chat/%1/unread").arg(encoded));
    req.setPriority(QNetworkRequest::HighPriority);
    QNetworkReply *reply = m_sendNam.post(req, QByteArray("{}"));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void Backend::markAllChatsRead() {
    for (const ChatEntry &c : m_chats) {
        if (c.unreadCount > 0) {
            markChatRead(c.guid);
        }
    }
    emit chatsChanged();
    emit selectedChatChanged();
}

void Backend::toggleChatReadUnread(const QString &chatGuid) {
    if (chatGuid.isEmpty()) return;
    for (const ChatEntry &c : m_chats) {
        if (c.guid == chatGuid) {
            if (c.unreadCount > 0) {
                markChatRead(chatGuid);
            } else {
                markChatUnread(chatGuid);
            }
            return;
        }
    }
}

QString Backend::resolveMergedOutgoingReceiptAndPending(
    const QString &canonicalGuid,
    const QString &realGuid,
    const QJsonObject &fallbackObj,
    int *outErrCode) {
    const QJsonObject mergedObj = (!realGuid.isEmpty() &&
                                   m_chatMessagesCache.contains(canonicalGuid) &&
                                   m_chatMessagesCache.value(canonicalGuid).contains(realGuid))
                                      ? m_chatMessagesCache.value(canonicalGuid).value(realGuid)
                                      : fallbackObj;
    const qint64 mergedDc = mergedObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
    const bool hasLaterIncoming = (mergedDc > 0 && m_chatMessagesCache.contains(canonicalGuid))
                                      ? hasEligibleLaterIncomingReply(m_chatMessagesCache.value(canonicalGuid), mergedDc)
                                      : false;
    const int mergedErrCode = mergedObj.value(QStringLiteral("error")).toInt(0);
    if (outErrCode) {
        *outErrCode = mergedErrCode;
    }
    const bool mergedHasIsSent = mergedObj.contains(QStringLiteral("isSent"));
    const bool mergedHasIsDelivered = mergedObj.contains(QStringLiteral("isDelivered"));
    const bool mergedIsDelivered = mergedObj.value(QStringLiteral("isDelivered")).toBool(false) ||
                                   (mergedObj.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0) ||
                                   (mergedObj.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0);
    const QString finalReceipt = computeOutgoingReceiptText(
        mergedObj,
        hasLaterIncoming,
        0,
        realGuid.isEmpty() ? 0 : m_pendingDeliveryAddedAtMs.value(realGuid, 0),
        !realGuid.isEmpty() && m_expiredPendingDeliveryGuids.contains(realGuid));
    if (!realGuid.isEmpty()) {
        if (mergedIsDelivered || mergedErrCode != 0 ||
            (mergedHasIsSent && mergedObj.value(QStringLiteral("isSent")).toBool())) {
            m_expiredPendingDeliveryGuids.remove(realGuid);
        }
        if (mergedErrCode == 0 && !mergedIsDelivered && (mergedHasIsSent || mergedHasIsDelivered) &&
            finalReceipt != QStringLiteral("⚠ Not Delivered") &&
            !m_expiredPendingDeliveryGuids.contains(realGuid)) {
            m_pendingDeliveryMessages.insert(realGuid, canonicalGuid);
            if (!m_pendingDeliveryAddedAtMs.contains(realGuid)) {
                m_pendingDeliveryAddedAtMs.insert(realGuid, QDateTime::currentMSecsSinceEpoch());
            }
        }
    }
    return finalReceipt;
}

void Backend::sendMessage(const QString &chatGuid, const QString &text, const QString &replyToGuid) {
    const QString trimmed = replaceEmojiShortcodes(text.trimmed());
    if (chatGuid.isEmpty() || trimmed.isEmpty()) return;

    const QString canonicalGuid = resolveCanonicalChatGuid(chatGuid);
    const QString sendChatGuid = resolveActiveSendChatGuid(canonicalGuid);
    const QString tempGuid = QStringLiteral("temp-") + QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Optimistic UI append (scoped to canonicalGuid so background sends never bleed into another selected chat's timeline)
    QVariantMap optimistic;
    optimistic.insert(QStringLiteral("guid"), tempGuid);
    optimistic.insert(QStringLiteral("chatGuid"), canonicalGuid);
    optimistic.insert(QStringLiteral("text"), trimmed);
    optimistic.insert(QStringLiteral("formattedText"), formatMessageTextWithLinks(trimmed));
    optimistic.insert(QStringLiteral("verificationCode"), extractVerificationCode(trimmed));
    optimistic.insert(QStringLiteral("isFromMe"), true);
    optimistic.insert(QStringLiteral("senderName"), QStringLiteral("You"));
    optimistic.insert(QStringLiteral("senderColor"), QStringLiteral("#7aa2f7"));
    optimistic.insert(QStringLiteral("timeText"), QDateTime::currentDateTime().toString(QStringLiteral("h:mm AP")));
    optimistic.insert(QStringLiteral("showDateHeader"), false);
    optimistic.insert(QStringLiteral("dateHeaderText"), QString());
    optimistic.insert(QStringLiteral("reactions"), QVariantList());
    optimistic.insert(QStringLiteral("hasImageAttachment"), false);
    optimistic.insert(QStringLiteral("hasFileAttachment"), false);
    optimistic.insert(QStringLiteral("receiptText"), m_demoMode ? QStringLiteral("Delivered") : QStringLiteral("Sending..."));
    const qint64 optNowMs = QDateTime::currentMSecsSinceEpoch();
    optimistic.insert(QStringLiteral("replyToGuid"), replyToGuid);
    optimistic.insert(QStringLiteral("dateCreated"), optNowMs);
    optimistic.insert(QStringLiteral("optimisticCreatedAtMs"), optNowMs);
    if (m_demoMode && m_demoChatMessages.contains(canonicalGuid)) {
        m_demoChatMessages[canonicalGuid].append(optimistic);
    }
    if (canonicalGuid == m_selectedChatGuid || chatGuid == m_selectedChatGuid) {
        m_messages.append(optimistic);
        enforceLatestOutgoingReceiptsInTimeline();
        emit messagesChanged();
    }

    m_unreadOverrides.remove(canonicalGuid);
    for (ChatEntry &c : m_chats) {
        if (c.guid == canonicalGuid) {
            c.lastMessagePreview = trimmed;
            c.lastMessageDate = QDateTime::currentMSecsSinceEpoch();
            c.relativeTime = QStringLiteral("now");
            c.unreadCount = 0;
            m_readWatermarks[canonicalGuid] = qMax(m_readWatermarks.value(canonicalGuid, 0), c.lastMessageDate);
            break;
        }
    }
    sortChats();
    emit chatsChanged();

    if (m_demoMode) return;

    QJsonObject payload;
    payload.insert(QStringLiteral("chatGuid"), sendChatGuid);
    payload.insert(QStringLiteral("tempGuid"), tempGuid);
    payload.insert(QStringLiteral("message"), trimmed);
    payload.insert(QStringLiteral("method"), QStringLiteral("private-api"));
    if (!replyToGuid.isEmpty()) {
        payload.insert(QStringLiteral("selectedMessageGuid"), replyToGuid);
        int partIndex = 0;
        if (m_chatMessagesCache.contains(canonicalGuid)) {
            partIndex = computeTextPartIndex(m_chatMessagesCache.value(canonicalGuid).value(replyToGuid));
        }
        payload.insert(QStringLiteral("partIndex"), partIndex);
    }

    auto finalizeSentMessage = [this, canonicalGuid, tempGuid, trimmed, optNowMs](const QByteArray &bytes, const QString &sentTempGuid) -> bool {
        QJsonParseError parseErr{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            return false;
        }
        const QJsonValue dataVal = doc.object().value(QStringLiteral("data"));
        if (!dataVal.isObject()) {
            return false;
        }
        QJsonObject dataObj = dataVal.toObject();
        if (dataObj.value(QStringLiteral("messages")).isArray()) {
            const QJsonArray msgs = dataObj.value(QStringLiteral("messages")).toArray();
            if (!msgs.isEmpty()) {
                dataObj = msgs.first().toObject();
            }
        }
        if (dataObj.isEmpty()) {
            return false;
        }
        const QString realGuid = dataObj.value(QStringLiteral("guid")).toString().trimmed();
        const QString ackTempGuid = dataObj.value(QStringLiteral("tempGuid")).toString().trimmed();
        const qint64 dateCreated = dataObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        const int sendErrCode = dataObj.value(QStringLiteral("error")).toInt(0);
        const bool looksLikeSyntheticChatGuid = realGuid.startsWith(QStringLiteral("iMessage;")) ||
                                                realGuid.startsWith(QStringLiteral("SMS;")) ||
                                                realGuid.startsWith(QStringLiteral("RCS;"));
        const bool tempGuidMatchesIfPresent = ackTempGuid.isEmpty() || sentTempGuid.isEmpty() || (ackTempGuid == sentTempGuid);
        const bool isPendingAck = dataObj.value(QStringLiteral("pending")).isBool() &&
                                  dataObj.value(QStringLiteral("pending")).toBool(false);
        const bool hasSendResult =
            (sendErrCode != 0) ||
            dataObj.value(QStringLiteral("isSent")).isBool() ||
            dataObj.value(QStringLiteral("isDelivered")).isBool() ||
            (dataObj.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0) ||
            (dataObj.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0) ||
            (dataObj.value(QStringLiteral("originalROWID")).toVariant().toLongLong() > 0) ||
            (dateCreated > 0 && dataObj.contains(QStringLiteral("isFromMe")));
        if (!tempGuidMatchesIfPresent || (!isPendingAck && !hasSendResult)) {
            return false;
        }

        if (sendErrCode != 0) {
            if (!realGuid.isEmpty() && !looksLikeSyntheticChatGuid) {
                m_seenMessageGuids.insert(realGuid);
                cacheMessageForChat(canonicalGuid, dataObj);
                saveMessagesCache();
                if (canonicalGuid == m_selectedChatGuid) {
                    rebuildSelectedChatMessages();
                }
            }
            return false;
        }

        if (realGuid.isEmpty() && isPendingAck && !tempGuid.isEmpty()) {
            QJsonObject pendingObj = dataObj;
            pendingObj.insert(QStringLiteral("guid"), tempGuid);
            pendingObj.insert(QStringLiteral("tempGuid"),
                              !ackTempGuid.isEmpty() ? ackTempGuid : (!sentTempGuid.isEmpty() ? sentTempGuid : tempGuid));
            if (!pendingObj.contains(QStringLiteral("isFromMe"))) {
                pendingObj.insert(QStringLiteral("isFromMe"), true);
            }
            if (extractMessageText(pendingObj).trimmed().isEmpty() && !trimmed.isEmpty()) {
                pendingObj.insert(QStringLiteral("text"), trimmed);
            }
            if (pendingObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong() <= 0) {
                pendingObj.insert(QStringLiteral("dateCreated"), optNowMs);
            }
            const bool hasPositiveDelivery =
                pendingObj.value(QStringLiteral("isDelivered")).toBool(false) ||
                (pendingObj.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0) ||
                (pendingObj.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0);
            if (!hasPositiveDelivery) {
                if (!pendingObj.value(QStringLiteral("isSent")).isBool()) {
                    pendingObj.insert(QStringLiteral("isSent"), false);
                }
                if (!pendingObj.value(QStringLiteral("isDelivered")).isBool()) {
                    pendingObj.insert(QStringLiteral("isDelivered"), false);
                }
                pendingObj.insert(QStringLiteral("pending"), true);
            }
            cacheMessageForChat(canonicalGuid, pendingObj);
            saveMessagesCache();
            return true;
        }

        if (!realGuid.isEmpty() && !looksLikeSyntheticChatGuid) {
            QJsonObject normalizedObj = dataObj;
            if (!normalizedObj.contains(QStringLiteral("isFromMe"))) {
                normalizedObj.insert(QStringLiteral("isFromMe"), true);
            }
            if (extractMessageText(normalizedObj).trimmed().isEmpty() && !trimmed.isEmpty()) {
                normalizedObj.insert(QStringLiteral("text"), trimmed);
            }
            if (normalizedObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong() <= 0) {
                normalizedObj.insert(QStringLiteral("dateCreated"), optNowMs);
            }
            const bool hasPositiveDelivery =
                normalizedObj.value(QStringLiteral("isDelivered")).toBool(false) ||
                (normalizedObj.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0) ||
                (normalizedObj.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0);
            if (isPendingAck && !hasPositiveDelivery) {
                if (!normalizedObj.value(QStringLiteral("isSent")).isBool()) {
                    normalizedObj.insert(QStringLiteral("isSent"), false);
                }
                if (!normalizedObj.value(QStringLiteral("isDelivered")).isBool()) {
                    normalizedObj.insert(QStringLiteral("isDelivered"), false);
                }
            }
            m_seenMessageGuids.insert(realGuid);
            if (!tempGuid.isEmpty() && tempGuid != realGuid) {
                m_seenMessageGuids.remove(tempGuid);
                m_pendingDeliveryMessages.remove(tempGuid);
                m_pendingDeliveryAddedAtMs.remove(tempGuid);
                m_expiredPendingDeliveryGuids.remove(tempGuid);
                m_inFlightDeliveryCheckGuids.remove(tempGuid);
                m_messageObservationSeq.remove(tempGuid);
                if (m_chatMessagesCache.contains(canonicalGuid)) {
                    m_chatMessagesCache[canonicalGuid].remove(tempGuid);
                }
            }
            if (dateCreated > m_lastMessageTimestamp) {
                m_lastMessageTimestamp = dateCreated;
            }
            cacheMessageForChat(canonicalGuid, normalizedObj);

            int mergedErrCode = 0;
            const QString finalReceipt = resolveMergedOutgoingReceiptAndPending(
                canonicalGuid, realGuid, normalizedObj, &mergedErrCode);

            if (canonicalGuid == m_selectedChatGuid) {
                int realIdx = -1;
                QList<int> dupOrTempIndices;
                for (int i = 0; i < m_messages.size(); ++i) {
                    const QString curG = m_messages[i].toMap().value(QStringLiteral("guid")).toString();
                    if (curG == realGuid) {
                        if (realIdx < 0) realIdx = i;
                        else dupOrTempIndices.append(i);
                    } else if (!tempGuid.isEmpty() && curG == tempGuid) {
                        dupOrTempIndices.append(i);
                    }
                }
                bool replaced = false;
                if (realIdx >= 0) {
                    QVariantMap m = m_messages[realIdx].toMap();
                    m.insert(QStringLiteral("receiptText"), finalReceipt);
                    m.insert(QStringLiteral("isFailed"), mergedErrCode != 0);
                    m.insert(QStringLiteral("errorCode"), mergedErrCode);
                    m_messages[realIdx] = m;
                    for (int k = dupOrTempIndices.size() - 1; k >= 0; --k) {
                        m_messages.removeAt(dupOrTempIndices[k]);
                    }
                    enforceLatestOutgoingReceiptsInTimeline();
                    replaced = true;
                } else if (!dupOrTempIndices.isEmpty()) {
                    const int keepIdx = dupOrTempIndices.first();
                    QVariantMap m = m_messages[keepIdx].toMap();
                    m.insert(QStringLiteral("guid"), realGuid);
                    m.insert(QStringLiteral("receiptText"), finalReceipt);
                    m.insert(QStringLiteral("isFailed"), mergedErrCode != 0);
                    m.insert(QStringLiteral("errorCode"), mergedErrCode);
                    m_messages[keepIdx] = m;
                    for (int k = dupOrTempIndices.size() - 1; k >= 1; --k) {
                        m_messages.removeAt(dupOrTempIndices[k]);
                    }
                    enforceLatestOutgoingReceiptsInTimeline();
                    replaced = true;
                }
                if (replaced) {
                    emit messagesChanged();
                } else {
                    rebuildSelectedChatMessages();
                }
            }
            saveMessagesCache();
            return true;
        }
        return false;
    };

    auto finishSendTrack = [this]() {
        if (m_activeSendCount > 0) {
            --m_activeSendCount;
            emit sendingMessageChanged();
        }
    };

    auto markOptimisticFailed = [this, canonicalGuid, tempGuid]() {
        if (m_chatMessagesCache.contains(canonicalGuid)) {
            m_chatMessagesCache[canonicalGuid].remove(tempGuid);
        }
        if (canonicalGuid == m_selectedChatGuid) {
            bool updated = false;
            for (int i = 0; i < m_messages.size(); ++i) {
                QVariantMap m = m_messages[i].toMap();
                if (m.value(QStringLiteral("guid")).toString() == tempGuid) {
                    m.insert(QStringLiteral("receiptText"), QStringLiteral("⚠ Not Delivered"));
                    m.insert(QStringLiteral("isFailed"), true);
                    m.insert(QStringLiteral("errorCode"), 4);
                    m_messages[i] = m;
                    updated = true;
                    break;
                }
            }
            if (updated) {
                emit messagesChanged();
            }
        }
        emit toastMessage(QStringLiteral("⚠ Message failed to send"));
    };

    auto sendViaNewChat = [this, canonicalGuid, sendChatGuid, trimmed, finalizeSentMessage, markOptimisticFailed, finishSendTrack]() {
        const QString addr = sendChatGuid.section(QStringLiteral(";-;"), 1);
        if (addr.isEmpty()) {
            markOptimisticFailed();
            loadChatMessages(canonicalGuid);
            finishSendTrack();
            return;
        }
        QJsonObject newChatPayload;
        newChatPayload.insert(QStringLiteral("addresses"), QJsonArray{addr});
        newChatPayload.insert(QStringLiteral("message"), trimmed);
        newChatPayload.insert(QStringLiteral("method"), QStringLiteral("private-api"));
        QNetworkRequest req3 = makeRequest(QStringLiteral("/api/v1/chat/new"));
        req3.setPriority(QNetworkRequest::HighPriority);
        req3.setTransferTimeout(45000);
        QNetworkReply *r3 = m_sendNam.post(req3, QJsonDocument(newChatPayload).toJson(QJsonDocument::Compact));
        connect(r3, &QNetworkReply::finished, this, [this, r3, canonicalGuid, finalizeSentMessage, markOptimisticFailed, finishSendTrack]() {
            r3->deleteLater();
            const bool resolved = (r3->error() == QNetworkReply::NoError) && finalizeSentMessage(r3->readAll(), QString());
            fetchChatsMetadata();
            if (!resolved) {
                markOptimisticFailed();
                loadChatMessages(canonicalGuid);
            }
            finishSendTrack();
        });
    };

    ++m_activeSendCount;
    emit sendingMessageChanged();
    QNetworkRequest sendReq = makeRequest(QStringLiteral("/api/v1/message/text"));
    sendReq.setPriority(QNetworkRequest::HighPriority);
    sendReq.setTransferTimeout(45000);
    QNetworkReply *reply = m_sendNam.post(sendReq, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, canonicalGuid, sendChatGuid, trimmed, tempGuid, finalizeSentMessage, markOptimisticFailed, sendViaNewChat, finishSendTrack]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QByteArray errBytes = reply->isOpen() ? reply->readAll() : QByteArray();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool chatMissing = (status == 404 || errBytes.contains("Chat does not exist") || errBytes.contains("chat does not exist"));
            if (chatMissing && sendChatGuid.contains(QStringLiteral(";-;"))) {
                sendViaNewChat();
                return;
            }
            if (errBytes.contains("Message send failed") || reply->error() == QNetworkReply::TimeoutError) {
                markOptimisticFailed();
                loadChatMessages(canonicalGuid);
                finishSendTrack();
                return;
            }

            // Fallback to apple-script method, and if that fails on a brand-new 1:1 chat, create via /api/v1/chat/new
            const QString fallbackTempGuid = tempGuid + QStringLiteral("-as");
            QJsonObject fallback;
            fallback.insert(QStringLiteral("chatGuid"), sendChatGuid);
            fallback.insert(QStringLiteral("tempGuid"), fallbackTempGuid);
            fallback.insert(QStringLiteral("message"), trimmed);
            fallback.insert(QStringLiteral("method"), QStringLiteral("apple-script"));
            QNetworkRequest req2 = makeRequest(QStringLiteral("/api/v1/message/text"));
            req2.setPriority(QNetworkRequest::HighPriority);
            req2.setTransferTimeout(45000);
            QNetworkReply *r2 = m_sendNam.post(req2, QJsonDocument(fallback).toJson(QJsonDocument::Compact));
            connect(r2, &QNetworkReply::finished, this, [this, r2, canonicalGuid, sendChatGuid, fallbackTempGuid, finalizeSentMessage, markOptimisticFailed, sendViaNewChat, finishSendTrack]() {
                r2->deleteLater();
                if (r2->error() != QNetworkReply::NoError) {
                    const QByteArray r2Err = r2->isOpen() ? r2->readAll() : QByteArray();
                    if (!r2Err.contains("Message send failed") && sendChatGuid.contains(QStringLiteral(";-;"))) {
                        sendViaNewChat();
                        return;
                    }
                    markOptimisticFailed();
                    loadChatMessages(canonicalGuid);
                    finishSendTrack();
                    return;
                }
                if (!finalizeSentMessage(r2->readAll(), fallbackTempGuid)) {
                    markOptimisticFailed();
                    loadChatMessages(canonicalGuid);
                }
                finishSendTrack();
            });
            return;
        }

        if (!finalizeSentMessage(reply->readAll(), tempGuid)) {
            markOptimisticFailed();
            loadChatMessages(canonicalGuid);
        }
        finishSendTrack();
    });
}

void Backend::sendReaction(const QString &chatGuid, const QString &messageGuid, const QString &reaction) {
    if (chatGuid.isEmpty() || messageGuid.isEmpty() || reaction.isEmpty()) return;
    const QString canonicalGuid = resolveCanonicalChatGuid(chatGuid);
    const QString sendChatGuid = resolveActiveSendChatGuid(canonicalGuid);

    const QString trimmedReaction = reaction.trimmed();
    QString reactionKey = trimmedReaction.toLower();
    if (trimmedReaction == QStringLiteral("❤️")) reactionKey = QStringLiteral("love");
    else if (trimmedReaction == QStringLiteral("👍")) reactionKey = QStringLiteral("like");
    else if (trimmedReaction == QStringLiteral("👎")) reactionKey = QStringLiteral("dislike");
    else if (trimmedReaction == QStringLiteral("😂")) reactionKey = QStringLiteral("laugh");
    else if (trimmedReaction == QStringLiteral("‼️")) reactionKey = QStringLiteral("emphasize");
    else if (trimmedReaction == QStringLiteral("❓")) reactionKey = QStringLiteral("question");
    else if (trimmedReaction == QStringLiteral("-❤️")) reactionKey = QStringLiteral("-love");
    else if (trimmedReaction == QStringLiteral("-👍")) reactionKey = QStringLiteral("-like");
    else if (trimmedReaction == QStringLiteral("-👎")) reactionKey = QStringLiteral("-dislike");
    else if (trimmedReaction == QStringLiteral("-😂")) reactionKey = QStringLiteral("-laugh");
    else if (trimmedReaction == QStringLiteral("-‼️")) reactionKey = QStringLiteral("-emphasize");
    else if (trimmedReaction == QStringLiteral("-❓")) reactionKey = QStringLiteral("-question");
    else if (mapReactionEmoji(reactionKey).isEmpty()) {
        reactionKey = trimmedReaction;
    }

    bool isRemoval = trimmedReaction.startsWith(QLatin1Char('-')) && trimmedReaction.size() > 1;
    const QString mapped = mapReactionEmoji(reactionKey, QString(), &isRemoval);
    const QString emoji = mapped.isEmpty()
        ? (trimmedReaction.startsWith(QLatin1Char('-')) ? trimmedReaction.mid(1).trimmed() : trimmedReaction)
        : mapped;

    // Immediate optimistic update on m_messages
    for (int i = 0; i < m_messages.size(); ++i) {
        QVariantMap m = m_messages[i].toMap();
        if (m.value(QStringLiteral("guid")).toString() == messageGuid) {
            QVariantList pills = m.value(QStringLiteral("reactions")).toList();
            bool updatedExisting = false;
            for (int j = 0; j < pills.size(); ++j) {
                QVariantMap p = pills[j].toMap();
                if (p.value(QStringLiteral("emoji")).toString() == emoji) {
                    QStringList sList = p.value(QStringLiteral("senderList")).toStringList();
                    if (sList.isEmpty()) {
                        const QString existingSenders = p.value(QStringLiteral("senders")).toString();
                        for (const QString &part : existingSenders.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                            const QString trimmed = part.trimmed();
                            if (!trimmed.isEmpty()) sList.append(trimmed);
                        }
                    }
                    if (isRemoval) {
                        sList.removeAll(QStringLiteral("You"));
                        const int nextCount = qMax(0, p.value(QStringLiteral("count")).toInt() - 1);
                        if (sList.isEmpty() || nextCount <= 0) {
                            pills.removeAt(j);
                        } else {
                            p.insert(QStringLiteral("fromMe"), false);
                            p.insert(QStringLiteral("count"), qMax(nextCount, static_cast<int>(sList.size())));
                            const QString joined = sList.join(QStringLiteral(", "));
                            p.insert(QStringLiteral("senders"), joined);
                            p.insert(QStringLiteral("senderList"), QVariant::fromValue(sList));
                            p.insert(QStringLiteral("tooltipText"), emoji + QStringLiteral(" ") + joined);
                            pills[j] = p;
                        }
                    } else {
                        p.insert(QStringLiteral("fromMe"), true);
                        if (!sList.contains(QStringLiteral("You"))) {
                            sList.append(QStringLiteral("You"));
                            p.insert(QStringLiteral("count"), qMax(p.value(QStringLiteral("count")).toInt() + 1, static_cast<int>(sList.size())));
                        }
                        const QString joined = sList.join(QStringLiteral(", "));
                        p.insert(QStringLiteral("senders"), joined);
                        p.insert(QStringLiteral("senderList"), QVariant::fromValue(sList));
                        p.insert(QStringLiteral("tooltipText"), emoji + QStringLiteral(" ") + joined);
                        pills[j] = p;
                    }
                    updatedExisting = true;
                    break;
                }
            }
            if (!updatedExisting && !isRemoval) {
                const QStringList sList{QStringLiteral("You")};
                QVariantMap newPill;
                newPill.insert(QStringLiteral("emoji"), emoji);
                newPill.insert(QStringLiteral("count"), 1);
                newPill.insert(QStringLiteral("fromMe"), true);
                newPill.insert(QStringLiteral("senders"), QStringLiteral("You"));
                newPill.insert(QStringLiteral("senderList"), QVariant::fromValue(sList));
                newPill.insert(QStringLiteral("tooltipText"), emoji + QStringLiteral(" You"));
                pills.append(newPill);
            }
            m.insert(QStringLiteral("reactions"), pills);
            emit aboutToChangeMessages();
            m_messages[i] = m;
            emit messagesChanged();
            break;
        }
    }

    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) return;

    int partIndex = 0;
    if (m_chatMessagesCache.contains(canonicalGuid)) {
        partIndex = computeTextPartIndex(m_chatMessagesCache.value(canonicalGuid).value(messageGuid));
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("chatGuid"), sendChatGuid);
    payload.insert(QStringLiteral("selectedMessageGuid"), messageGuid);
    payload.insert(QStringLiteral("reaction"), reactionKey);
    payload.insert(QStringLiteral("partIndex"), partIndex);

    QNetworkRequest reactReq = makeRequest(QStringLiteral("/api/v1/message/react"));
    reactReq.setPriority(QNetworkRequest::HighPriority);
    QNetworkReply *reply = m_sendNam.post(reactReq, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, canonicalGuid]() {
        const QByteArray body = reply->readAll();
        const bool ok = (reply->error() == QNetworkReply::NoError);
        reply->deleteLater();
        if (ok && !body.isEmpty()) {
            const QJsonObject resp = QJsonDocument::fromJson(body).object();
            const QJsonObject dataObj = resp.value(QStringLiteral("data")).toObject();
            const QString rGuid = dataObj.value(QStringLiteral("guid")).toString().trimmed();
            if (!rGuid.isEmpty()) {
                m_chatMessagesCache[canonicalGuid].insert(rGuid, dataObj);
            }
        }
        loadChatMessages(canonicalGuid);
        QTimer::singleShot(1200, this, [this, canonicalGuid]() {
            if (m_selectedChatGuid == canonicalGuid) {
                loadChatMessages(canonicalGuid);
            }
        });
    });
}

void Backend::deleteMessage(const QString &messageGuid, const QString &chatGuid) {
    QString resolvedMsgGuid = messageGuid.trimmed();
    QString resolvedChatGuid = chatGuid.trimmed();

    // Support both deleteMessage(msgGuid), deleteMessage(msgGuid, chatGuid), and deleteMessage(chatGuid, msgGuid)
    if (!resolvedChatGuid.isEmpty()) {
        const bool firstLooksLikeChat = resolvedMsgGuid.startsWith(QStringLiteral("iMessage;")) ||
                                        resolvedMsgGuid.startsWith(QStringLiteral("SMS;")) ||
                                        resolvedMsgGuid.startsWith(QStringLiteral("RCS;")) ||
                                        m_chatMessagesCache.contains(resolvedMsgGuid);
        const bool secondLooksLikeChat = resolvedChatGuid.startsWith(QStringLiteral("iMessage;")) ||
                                         resolvedChatGuid.startsWith(QStringLiteral("SMS;")) ||
                                         resolvedChatGuid.startsWith(QStringLiteral("RCS;")) ||
                                         m_chatMessagesCache.contains(resolvedChatGuid);
        if (firstLooksLikeChat && !secondLooksLikeChat) {
            std::swap(resolvedMsgGuid, resolvedChatGuid);
        }
    }

    if (resolvedMsgGuid.isEmpty()) return;
    if (resolvedChatGuid.isEmpty()) {
        resolvedChatGuid = m_selectedChatGuid;
    }
    if (resolvedChatGuid.isEmpty()) {
        for (auto it = m_chatMessagesCache.constBegin(); it != m_chatMessagesCache.constEnd(); ++it) {
            if (it.value().contains(resolvedMsgGuid)) {
                resolvedChatGuid = it.key();
                break;
            }
        }
    }

    // Check if the message is a recent outgoing iMessage (eligible for native Undo Send / unsend on all devices)
    bool isRecentOutgoingIMessage = false;
    int partIndex = 0;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (!resolvedChatGuid.isEmpty() && m_chatMessagesCache.contains(resolvedChatGuid) &&
        m_chatMessagesCache.value(resolvedChatGuid).contains(resolvedMsgGuid)) {
        const QJsonObject cachedMsg = m_chatMessagesCache.value(resolvedChatGuid).value(resolvedMsgGuid);
        partIndex = computeTextPartIndex(cachedMsg);
        const bool fromMe = cachedMsg.value(QStringLiteral("isFromMe")).toBool();
        const QString srv = cachedMsg.value(QStringLiteral("service")).toString(QStringLiteral("iMessage"));
        const qint64 dc = cachedMsg.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        if (fromMe && (srv == QStringLiteral("iMessage") || resolvedChatGuid.startsWith(QStringLiteral("iMessage"))) &&
            dc > 0 && (nowMs - dc) <= 120000LL) {
            isRecentOutgoingIMessage = true;
        }
    }

    // 1. Record in persistent deleted set
    m_deletedMessageGuids.insert(resolvedMsgGuid);
    m_pendingDeliveryMessages.remove(resolvedMsgGuid);
    m_pendingDeliveryAddedAtMs.remove(resolvedMsgGuid);
    m_expiredPendingDeliveryGuids.remove(resolvedMsgGuid);
    m_inFlightDeliveryCheckGuids.remove(resolvedMsgGuid);
    m_messageObservationSeq.remove(resolvedMsgGuid);

    // 2. Remove from m_chatMessagesCache (including any tapback children associated with this message)
    for (auto chatIt = m_chatMessagesCache.begin(); chatIt != m_chatMessagesCache.end(); ++chatIt) {
        auto &chatMap = chatIt.value();
        chatMap.remove(resolvedMsgGuid);
        QStringList childReactionGuids;
        for (auto mIt = chatMap.constBegin(); mIt != chatMap.constEnd(); ++mIt) {
            const QString assoc = normalizeAssociatedGuid(mIt.value().value(QStringLiteral("associatedMessageGuid")).toString());
            if (assoc == resolvedMsgGuid) {
                childReactionGuids.append(mIt.key());
            }
        }
        for (const QString &cg : childReactionGuids) {
            chatMap.remove(cg);
            m_deletedMessageGuids.insert(cg);
            m_pendingDeliveryMessages.remove(cg);
            m_pendingDeliveryAddedAtMs.remove(cg);
            m_expiredPendingDeliveryGuids.remove(cg);
            m_inFlightDeliveryCheckGuids.remove(cg);
            m_messageObservationSeq.remove(cg);
        }
    }

    // 3. Remove from UI timeline immediately and restore preceding receipt when active chat is affected
    const QString canonicalChat = resolveCanonicalChatGuid(resolvedChatGuid);
    const bool isActiveSelectedChat = !m_selectedChatGuid.isEmpty() &&
                                      !canonicalChat.isEmpty() &&
                                      (canonicalChat == m_selectedChatGuid ||
                                       isSameConversation(canonicalChat, m_selectedChatGuid));
    if (isActiveSelectedChat && (!m_demoMode || m_chatMessagesCache.contains(m_selectedChatGuid))) {
        rebuildSelectedChatMessages();
    } else {
        bool removedFromUi = false;
        for (int i = m_messages.size() - 1; i >= 0; --i) {
            if (m_messages[i].toMap().value(QStringLiteral("guid")).toString() == resolvedMsgGuid) {
                if (!removedFromUi) {
                    emit aboutToChangeMessages();
                }
                m_messages.removeAt(i);
                removedFromUi = true;
            }
        }
        if (removedFromUi) {
            emit messagesChanged();
        }
    }

    // 4. Update chat preview & persist state
    if (!resolvedChatGuid.isEmpty()) {
        refreshChatPreviewFromCache(resolvedChatGuid);
    }
    saveAppState();
    saveMessagesCache();
    emit toastMessage(QStringLiteral("🗑 Message deleted"));

    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty() || resolvedMsgGuid.startsWith(QStringLiteral("temp-"))) {
        return;
    }

    // 5. Call BlueBubbles Private API on macOS Relay to delete from Messages.app / chat.db / Messages in iCloud
    const QString encodedMsgGuid = QString::fromUtf8(QUrl::toPercentEncoding(resolvedMsgGuid));
    if (isRecentOutgoingIMessage) {
        QJsonObject unsendBody;
        unsendBody.insert(QStringLiteral("partIndex"), partIndex);
        QNetworkRequest unsendReq = makeRequest(QStringLiteral("/api/v1/message/%1/unsend").arg(encodedMsgGuid));
        unsendReq.setPriority(QNetworkRequest::HighPriority);
        QNetworkReply *unsendReply = m_sendNam.post(
            unsendReq,
            QJsonDocument(unsendBody).toJson(QJsonDocument::Compact));
        connect(unsendReply, &QNetworkReply::finished, unsendReply, &QNetworkReply::deleteLater);
    }

    if (!resolvedChatGuid.isEmpty()) {
        const QString encodedChatGuid = QString::fromUtf8(QUrl::toPercentEncoding(resolvedChatGuid));
        const QString delPath = QStringLiteral("/api/v1/chat/%1/%2").arg(encodedChatGuid, encodedMsgGuid);
        QNetworkRequest delReq = makeRequest(delPath);
        delReq.setPriority(QNetworkRequest::HighPriority);
        QNetworkReply *delReply = m_sendNam.deleteResource(delReq);
        connect(delReply, &QNetworkReply::finished, this, [this, delReply, resolvedChatGuid]() {
            delReply->deleteLater();
            QTimer::singleShot(500, this, [this, resolvedChatGuid]() {
                if (m_selectedChatGuid == resolvedChatGuid) {
                    loadChatMessages(resolvedChatGuid);
                }
            });
        });
    }
}

void Backend::unsendMessage(const QString &messageGuid, const QString &chatGuid) {
    QString resolvedMsgGuid = messageGuid.trimmed();
    QString resolvedChatGuid = chatGuid.trimmed();

    if (!resolvedChatGuid.isEmpty()) {
        const bool firstLooksLikeChat = resolvedMsgGuid.startsWith(QStringLiteral("iMessage;")) ||
                                        resolvedMsgGuid.startsWith(QStringLiteral("SMS;")) ||
                                        resolvedMsgGuid.startsWith(QStringLiteral("RCS;")) ||
                                        m_chatMessagesCache.contains(resolvedMsgGuid);
        const bool secondLooksLikeChat = resolvedChatGuid.startsWith(QStringLiteral("iMessage;")) ||
                                         resolvedChatGuid.startsWith(QStringLiteral("SMS;")) ||
                                         resolvedChatGuid.startsWith(QStringLiteral("RCS;")) ||
                                         m_chatMessagesCache.contains(resolvedChatGuid);
        if (firstLooksLikeChat && !secondLooksLikeChat) {
            std::swap(resolvedMsgGuid, resolvedChatGuid);
        }
    }

    if (resolvedMsgGuid.isEmpty()) return;
    if (resolvedChatGuid.isEmpty()) {
        resolvedChatGuid = m_selectedChatGuid;
    }
    if (resolvedChatGuid.isEmpty()) {
        for (auto it = m_chatMessagesCache.constBegin(); it != m_chatMessagesCache.constEnd(); ++it) {
            if (it.value().contains(resolvedMsgGuid)) {
                resolvedChatGuid = it.key();
                break;
            }
        }
    }

    int partIndex = 0;
    if (!resolvedChatGuid.isEmpty() && m_chatMessagesCache.contains(resolvedChatGuid)) {
        partIndex = computeTextPartIndex(m_chatMessagesCache.value(resolvedChatGuid).value(resolvedMsgGuid));
    }

    const QJsonObject msgObj = findCachedMessage(resolvedMsgGuid);
    const bool offline = m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty() ||
                         resolvedMsgGuid.startsWith(QStringLiteral("temp-"));
    if (offline) {
        applyLocalUnsend(resolvedMsgGuid, resolvedChatGuid);
        emit toastMessage(QStringLiteral("↩ Message unsent"));
        return;
    }
    const qint64 sentAt = msgObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
    if (sentAt > 0 && QDateTime::currentMSecsSinceEpoch() - sentAt > kMessageEditWindowMs) {
        emit toastMessage(QStringLiteral("⚠ Messages can only be unsent within 2 minutes"));
        return;
    }

    // The Mac's helper needs the chat the message is actually in (a merged
    // sibling chat may have a different GUID than the one shown).
    const QString sendChatGuid = messageOwnChatGuid(msgObj, resolvedChatGuid);
    const QString encodedMsgGuid = QString::fromUtf8(QUrl::toPercentEncoding(resolvedMsgGuid));
    QJsonObject body;
    body.insert(QStringLiteral("chatGuid"), sendChatGuid);
    body.insert(QStringLiteral("partIndex"), partIndex);

    QNetworkRequest unsendReq = makeRequest(QStringLiteral("/api/v1/message/%1/unsend").arg(encodedMsgGuid));
    unsendReq.setPriority(QNetworkRequest::HighPriority);
    QNetworkReply *reply = m_sendNam.post(unsendReq, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, resolvedMsgGuid, resolvedChatGuid, sendChatGuid, encodedMsgGuid]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject root;
        const bool jsonOk = parseJsonReplyObject(reply->readAll(), &root);
        if (reply->error() != QNetworkReply::NoError || httpStatus != 200 || !jsonOk) {
            // Keep the message: it's still on everyone's devices.
            QString why = root.value(QStringLiteral("message")).toString().trimmed();
            if (why.isEmpty()) why = (reply->error() == QNetworkReply::NoError && !jsonOk)
                                         ? QStringLiteral("unexpected reply from the server")
                                         : reply->errorString();
            emit toastMessage(QStringLiteral("⚠ Couldn't unsend: %1").arg(why));
            return;
        }
        applyLocalUnsend(resolvedMsgGuid, resolvedChatGuid);
        emit toastMessage(QStringLiteral("↩ Message unsent across iCloud devices"));
        if (!sendChatGuid.isEmpty()) {
            const QString encodedChatGuid = QString::fromUtf8(QUrl::toPercentEncoding(sendChatGuid));
            QNetworkRequest delReq = makeRequest(QStringLiteral("/api/v1/chat/%1/%2").arg(encodedChatGuid, encodedMsgGuid));
            delReq.setPriority(QNetworkRequest::HighPriority);
            QNetworkReply *delReply = m_sendNam.deleteResource(delReq);
            connect(delReply, &QNetworkReply::finished, delReply, &QNetworkReply::deleteLater);
        }
    });
}

bool Backend::parseJsonReplyObject(const QByteArray &bytes, QJsonObject *out) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    if (out) *out = doc.object();
    return true;
}

QJsonObject Backend::findCachedMessage(const QString &messageGuid, QString *chatKey) const {
    for (auto it = m_chatMessagesCache.constBegin(); it != m_chatMessagesCache.constEnd(); ++it) {
        const auto found = it.value().constFind(messageGuid);
        if (found != it.value().constEnd()) {
            if (chatKey) *chatKey = it.key();
            return found.value();
        }
    }
    if (chatKey) chatKey->clear();
    return QJsonObject();
}

QString Backend::messageOwnChatGuid(const QJsonObject &msgObj, const QString &fallbackChatGuid) {
    const QString original = msgObj.value(QStringLiteral("originalChatGuid")).toString().trimmed();
    if (!original.isEmpty()) return original;
    const QJsonArray chats = msgObj.value(QStringLiteral("chats")).toArray();
    for (const QJsonValue &v : chats) {
        const QString g = v.toObject().value(QStringLiteral("guid")).toString().trimmed();
        if (!g.isEmpty()) return g;
    }
    const QString direct = msgObj.value(QStringLiteral("chatGuid")).toString().trimmed();
    return direct.isEmpty() ? fallbackChatGuid : direct;
}

void Backend::applyLocalUnsend(const QString &resolvedMsgGuid, const QString &resolvedChatGuid) {
    m_deletedMessageGuids.insert(resolvedMsgGuid);
    m_pendingDeliveryMessages.remove(resolvedMsgGuid);
    m_pendingDeliveryAddedAtMs.remove(resolvedMsgGuid);
    m_expiredPendingDeliveryGuids.remove(resolvedMsgGuid);
    m_inFlightDeliveryCheckGuids.remove(resolvedMsgGuid);
    m_messageObservationSeq.remove(resolvedMsgGuid);

    for (auto chatIt = m_chatMessagesCache.begin(); chatIt != m_chatMessagesCache.end(); ++chatIt) {
        chatIt.value().remove(resolvedMsgGuid);
    }

    const QString canonicalChat = resolveCanonicalChatGuid(resolvedChatGuid);
    const bool isActiveSelectedChat = !m_selectedChatGuid.isEmpty() &&
                                      !canonicalChat.isEmpty() &&
                                      (canonicalChat == m_selectedChatGuid ||
                                       isSameConversation(canonicalChat, m_selectedChatGuid));
    if (isActiveSelectedChat && (!m_demoMode || m_chatMessagesCache.contains(m_selectedChatGuid))) {
        rebuildSelectedChatMessages();
    } else {
        bool removedFromUi = false;
        for (int i = m_messages.size() - 1; i >= 0; --i) {
            if (m_messages[i].toMap().value(QStringLiteral("guid")).toString() == resolvedMsgGuid) {
                if (!removedFromUi) {
                    emit aboutToChangeMessages();
                }
                m_messages.removeAt(i);
                removedFromUi = true;
            }
        }
        if (removedFromUi) {
            emit messagesChanged();
        }
    }

    if (!resolvedChatGuid.isEmpty()) {
        refreshChatPreviewFromCache(resolvedChatGuid);
    }
    saveAppState();
    saveMessagesCache();
}

void Backend::editMessage(const QString &messageGuid, const QString &newText) {
    const QString msgGuid = messageGuid.trimmed();
    const QString text = newText.trimmed();
    if (msgGuid.isEmpty() || text.isEmpty()) return;
    QString chatKey;
    QJsonObject msgObj = findCachedMessage(msgGuid, &chatKey);
    if (msgObj.isEmpty() || !msgObj.value(QStringLiteral("isFromMe")).toBool()) return;
    const QString oldText = msgObj.value(QStringLiteral("text")).toString();
    if (extractMessageText(msgObj) == text) return;
    const qint64 sentAt = msgObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
    if (sentAt > 0 && QDateTime::currentMSecsSinceEpoch() - sentAt > kMessageEditWindowMs) {
        emit toastMessage(QStringLiteral("⚠ Messages can only be edited within 2 minutes"));
        return;
    }
    const QJsonValue oldDateEdited = msgObj.value(QStringLiteral("dateEdited"));
    const QJsonValue oldAttributed = msgObj.value(QStringLiteral("attributedBody"));
    const bool online = !(m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty() || msgGuid.startsWith(QStringLiteral("temp-")));
    // Show the edit right away (the old attributed body would override the new text)
    applyEditedText(chatKey, msgGuid, text, QJsonValue(QDateTime::currentMSecsSinceEpoch()), QJsonValue(QJsonValue::Undefined));
    if (!online) return;

    EditTracking &track = m_editTracking[msgGuid];
    if (!track.inFlight && !track.hasQueued) {
        track.confirmed = EditSnapshot{oldText, oldDateEdited, oldAttributed};
        track.chatKey = chatKey;
        track.sendChatGuid = messageOwnChatGuid(msgObj, chatKey);
        track.partIndex = computeTextPartIndex(msgObj);
    }
    track.shownText = text;
    if (track.inFlight) {
        // Sent after the one in flight finishes, so the Mac applies them in order
        track.hasQueued = true;
        track.queuedText = text;
        return;
    }
    sendEditRequest(msgGuid, text);
}

void Backend::applyEditedText(const QString &chatKey, const QString &msgGuid, const QString &t,
                              const QJsonValue &dateEdited, const QJsonValue &attributed) {
    auto chatIt = m_chatMessagesCache.find(chatKey);
    if (chatIt == m_chatMessagesCache.end()) return;
    auto msgIt = chatIt.value().find(msgGuid);
    if (msgIt == chatIt.value().end()) return;
    QJsonObject m = msgIt.value();
    m.insert(QStringLiteral("text"), t);
    if (dateEdited.isUndefined()) m.remove(QStringLiteral("dateEdited"));
    else m.insert(QStringLiteral("dateEdited"), dateEdited);
    if (attributed.isUndefined()) m.remove(QStringLiteral("attributedBody"));
    else m.insert(QStringLiteral("attributedBody"), attributed);
    msgIt.value() = m;
    if (!m_selectedChatGuid.isEmpty() &&
        (chatKey == m_selectedChatGuid || isSameConversation(chatKey, m_selectedChatGuid))) {
        rebuildSelectedChatMessages();
    }
    refreshChatPreviewFromCache(chatKey);
    saveMessagesCache();
}

void Backend::sendEditRequest(const QString &msgGuid, const QString &text) {
    auto trackIt = m_editTracking.find(msgGuid);
    if (trackIt == m_editTracking.end()) return;
    trackIt->inFlight = true;
    QJsonObject body;
    body.insert(QStringLiteral("chatGuid"), trackIt->sendChatGuid);
    body.insert(QStringLiteral("editedMessage"), text);
    body.insert(QStringLiteral("backwardsCompatibilityMessage"), QStringLiteral("Edited to “%1”").arg(text));
    body.insert(QStringLiteral("partIndex"), trackIt->partIndex);
    const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(msgGuid));
    QNetworkRequest req = makeRequest(QStringLiteral("/api/v1/message/%1/edit").arg(encoded));
    req.setPriority(QNetworkRequest::HighPriority);
    QNetworkReply *reply = m_sendNam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, msgGuid, text]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject root;
        const bool jsonOk = parseJsonReplyObject(reply->readAll(), &root);
        const bool ok = reply->error() == QNetworkReply::NoError && httpStatus == 200 && jsonOk;
        ++m_editRepliesProcessed;
        auto trackIt = m_editTracking.find(msgGuid);
        if (trackIt == m_editTracking.end()) return;
        EditTracking &track = trackIt.value();
        track.inFlight = false;
        // Requests are sequential, so a success is the Mac's current text
        if (ok) track.confirmed = EditSnapshot{text, QJsonValue(QDateTime::currentMSecsSinceEpoch()), QJsonValue(QJsonValue::Undefined)};
        QString why;
        if (!ok) {
            why = root.value(QStringLiteral("message")).toString().trimmed();
            if (why.isEmpty()) why = (reply->error() == QNetworkReply::NoError && !jsonOk)
                                         ? QStringLiteral("unexpected reply from the server")
                                         : reply->errorString();
        }
        if (track.hasQueued) {
            track.hasQueued = false;
            const QString next = track.queuedText;
            if (!ok) emit toastMessage(QStringLiteral("⚠ An earlier edit didn't go through: %1").arg(why));
            sendEditRequest(msgGuid, next);
            return;
        }
        const EditTracking settled = track;
        m_editTracking.erase(trackIt);
        if (ok) return;
        // Roll back only if the message still shows our edit (a sync from another
        // device may have replaced it with something newer)
        const QString nowShown = findCachedMessage(msgGuid).value(QStringLiteral("text")).toString();
        if (nowShown == settled.shownText) {
            applyEditedText(settled.chatKey, msgGuid, settled.confirmed.text, settled.confirmed.dateEdited, settled.confirmed.attributed);
        }
        emit toastMessage(QStringLiteral("⚠ Couldn't edit: %1").arg(why));
    });
}

void Backend::deleteChat(const QString &chatGuid) {
    const QString targetGuid = chatGuid.trimmed().isEmpty() ? m_selectedChatGuid : chatGuid.trimmed();
    if (targetGuid.isEmpty()) return;

    qint64 maxTs = QDateTime::currentMSecsSinceEpoch();
    for (const ChatEntry &c : m_chats) {
        if (c.guid == targetGuid && c.lastMessageDate > maxTs) {
            maxTs = c.lastMessageDate;
        }
    }
    m_deletedChatTimestamps.insert(targetGuid, maxTs);

    QStringList recentMsgGuidsToDelete;
    if (m_chatMessagesCache.contains(targetGuid)) {
        const auto &chatMap = m_chatMessagesCache.value(targetGuid);
        QList<QJsonObject> cachedObjs = chatMap.values();
        std::sort(cachedObjs.begin(), cachedObjs.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("dateCreated")).toVariant().toLongLong() >
                   b.value(QStringLiteral("dateCreated")).toVariant().toLongLong();
        });
        for (const QJsonObject &mObj : cachedObjs) {
            const QString mg = mObj.value(QStringLiteral("guid")).toString();
            if (!mg.isEmpty() && !mg.startsWith(QStringLiteral("temp-")) && recentMsgGuidsToDelete.size() < 25) {
                recentMsgGuidsToDelete.append(mg);
            }
        }
        for (auto it = chatMap.constBegin(); it != chatMap.constEnd(); ++it) {
            const QString delKey = it.key();
            m_deletedMessageGuids.insert(delKey);
            clearPendingDeliveryTrackingForGuid(delKey, true);
        }
        m_chatMessagesCache.remove(targetGuid);
    }
    for (const QString &pendKey : m_pendingDeliveryMessages.keys()) {
        const QString pendCg = m_pendingDeliveryMessages.value(pendKey);
        if (pendCg == targetGuid || isSameConversation(pendCg, targetGuid)) {
            clearPendingDeliveryTrackingForGuid(pendKey, true);
        }
    }
    m_chatMessageOffset.remove(targetGuid);
    m_chatHasMoreMessages.remove(targetGuid);
    m_unreadOverrides.remove(targetGuid);

    int removedIdx = -1;
    for (int i = 0; i < m_chats.size(); ++i) {
        if (m_chats[i].guid == targetGuid) {
            removedIdx = i;
            m_chats.removeAt(i);
            break;
        }
    }

    const bool wasSelected = (m_selectedChatGuid == targetGuid);
    if (wasSelected) {
        if (!m_chats.isEmpty()) {
            const int nextIdx = qBound(0, removedIdx, m_chats.size() - 1);
            const QString nextGuid = m_chats[nextIdx].guid;
            selectChat(nextGuid);
        } else {
            clearSelectedChat();
        }
    }

    saveAppState();
    saveMessagesCache();
    emit chatsChanged();
    emit selectedChatChanged();
    emit toastMessage(QStringLiteral("🗑 Conversation deleted"));

    if (m_demoMode || m_serverUrl.isEmpty() || m_password.isEmpty()) {
        return;
    }

    const QString encodedChatGuid = QString::fromUtf8(QUrl::toPercentEncoding(targetGuid));
    for (const QString &msgGuid : recentMsgGuidsToDelete) {
        const QString encodedMsgGuid = QString::fromUtf8(QUrl::toPercentEncoding(msgGuid));
        QNetworkRequest msgDelReq = makeRequest(QStringLiteral("/api/v1/chat/%1/%2").arg(encodedChatGuid, encodedMsgGuid));
        msgDelReq.setPriority(QNetworkRequest::HighPriority);
        msgDelReq.setTransferTimeout(15000);
        QNetworkReply *msgReply = m_sendNam.deleteResource(msgDelReq);
        connect(msgReply, &QNetworkReply::finished, msgReply, &QNetworkReply::deleteLater);
    }
    const QString delPath = QStringLiteral("/api/v1/chat/%1").arg(encodedChatGuid);
    QNetworkRequest chatDelReq = makeRequest(delPath);
    chatDelReq.setPriority(QNetworkRequest::HighPriority);
    chatDelReq.setTransferTimeout(35000);
    QNetworkReply *reply = m_sendNam.deleteResource(chatDelReq);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

QString Backend::extractVerificationCode(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return QString();

    // 1. Standalone 4-8 digit verification code message
    static const QRegularExpression reStandalone(QStringLiteral(R"(^\d{4,8}$)"));
    if (reStandalone.match(trimmed).hasMatch()) {
        return trimmed;
    }

    // Strip URLs and phone numbers so ports/paths/phone numbers are never misidentified as OTP codes
    QString cleaned = trimmed;
    static const QRegularExpression reUrl(QStringLiteral(R"(https?://\S+)"), QRegularExpression::CaseInsensitiveOption);
    cleaned.replace(reUrl, QStringLiteral(" "));
    static const QRegularExpression rePhone(QStringLiteral(R"(\+?\d[\d\s().\-]{8,}\d)"));
    cleaned.replace(rePhone, QStringLiteral(" "));

    // 2. Explicit G-XXXXXX Google verification code format
    static const QRegularExpression reGoogle(QStringLiteral(R"(\bG-(\d{4,8})\b)"), QRegularExpression::CaseInsensitiveOption);
    auto mGoogle = reGoogle.match(cleaned);
    if (mGoogle.hasMatch()) {
        return mGoogle.captured(1);
    }

    // 3. Check for OTP / 2FA / verification context keywords
    static const QRegularExpression reKeyword(
        QStringLiteral(R"((\b(?:code|verification|verify|otp|passcode|pin|security|2fa|two-factor|one-time|authentication|auth|login|sign-?in|confirm|confirmation|token)\b|never\s+share|do\s+not\s+share))"),
        QRegularExpression::CaseInsensitiveOption);
    if (!reKeyword.match(cleaned).hasMatch()) {
        return QString();
    }

    // 4. High-confidence patterns near keywords
    // e.g. "verification code is: 849201", "code: 194-820", "OTP is 5829"
    static const QRegularExpression reAfterKw(
        QStringLiteral(R"(\b(?:code|otp|passcode|pin|token)\b(?:\s+for\s+[\w.\-]+)?\s*(?:is|:|\-)?\s*\b(\d{3}-\d{3,4}|\d{4,8})\b)"),
        QRegularExpression::CaseInsensitiveOption);
    auto mAfter = reAfterKw.match(cleaned);
    if (mAfter.hasMatch()) {
        QString c = mAfter.captured(1);
        c.remove(QLatin1Char('-'));
        return c;
    }

    // e.g. "849201 is your verification code" or "194-820 is your Instagram code"
    static const QRegularExpression reBeforeIsYour(
        QStringLiteral(R"(\b(\d{3}-\d{3,4}|\d{4,8})\s+is\s+your\b)"),
        QRegularExpression::CaseInsensitiveOption);
    auto mBefore = reBeforeIsYour.match(cleaned);
    if (mBefore.hasMatch()) {
        QString c = mBefore.captured(1);
        c.remove(QLatin1Char('-'));
        return c;
    }

    // e.g. "Your security code: A9F4K2" (uppercase alphanumeric with at least one letter and one digit)
    static const QRegularExpression reAlphaNum(
        QStringLiteral(R"((?:[Cc]ode|[Oo][Tt][Pp]|[Pp]asscode|[Tt]oken)\s*(?:is)?\s*[:\-]\s*\b([A-Z0-9]{5,8})\b)"));
    auto mAlpha = reAlphaNum.match(cleaned);
    if (mAlpha.hasMatch()) {
        const QString cand = mAlpha.captured(1);
        bool hasDigit = false;
        bool hasUpper = false;
        for (const QChar &ch : cand) {
            if (ch.isDigit()) hasDigit = true;
            if (ch.isUpper()) hasUpper = true;
        }
        if (hasDigit && hasUpper) {
            return cand;
        }
    }

    // 5. Fallback when keyword is present: find 4-8 digit (or XXX-XXX) number not attached to currency or time units
    static const QRegularExpression reDigits(QStringLiteral(R"((?<![$\d])(\d{3}-\d{3,4}|\d{4,8})\b(?!\s*(?:am|pm|min|mins|minutes|sec|seconds|hr|hrs|hours|days)\b))"),
                                             QRegularExpression::CaseInsensitiveOption);
    auto it = reDigits.globalMatch(cleaned);
    while (it.hasNext()) {
        auto m = it.next();
        QString cand = m.captured(1);
        cand.remove(QLatin1Char('-'));
        if (cand.size() == 4) {
            const int yr = cand.toInt();
            if (yr >= 1990 && yr <= 2039) continue;
        }
        return cand;
    }

    return QString();
}

void Backend::copyToClipboard(const QString &text, const QString &label) {
    if (text.isEmpty()) return;

    const QString trimmed = text.trimmed();
    if (trimmed == QStringLiteral("📷 Photo") ||
        QRegularExpression(QStringLiteral(R"(^📷\s+\d+\s+Photos$)")).match(trimmed).hasMatch()) {
        if (!m_lastCopiedImagePath.isEmpty() && QFileInfo::exists(m_lastCopiedImagePath)) {
            copyImageToClipboard(m_lastCopiedImagePath);
            return;
        }
        for (int i = m_messages.size() - 1; i >= 0; --i) {
            const QVariantMap map = m_messages.at(i).toMap();
            if (map.value(QStringLiteral("hasImageAttachment")).toBool()) {
                const QString attGuid = map.value(QStringLiteral("attachmentGuid")).toString();
                const QString attUrl = map.value(QStringLiteral("attachmentLocalUrl")).toString();
                if (!attUrl.isEmpty() || !attGuid.isEmpty()) {
                    copyImageToClipboard(attUrl, attGuid);
                    return;
                }
            }
        }
    }

    m_lastCopiedImagePath.clear();
    m_lastCopiedImageTimeMs = 0;

    if (QClipboard *cb = QGuiApplication::clipboard()) {
        cb->setText(text, QClipboard::Clipboard);
        if (cb->supportsSelection()) {
            cb->setText(text, QClipboard::Selection);
        }
    }

    // Also pipe to wl-copy on Wayland so clipboard persists across all Wayland/XWayland surfaces
    if (QGuiApplication::platformName() != QStringLiteral("offscreen") &&
        QFileInfo::exists(QStringLiteral("/usr/bin/wl-copy"))) {
        QProcess *proc = new QProcess(this);
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                proc, &QObject::deleteLater);
        proc->start(QStringLiteral("/usr/bin/wl-copy"), QStringList{});
        proc->write(text.toUtf8());
        proc->closeWriteChannel();
    }

    if (label.isEmpty()) {
        emit toastMessage(QStringLiteral("✓ Copied to clipboard"));
    } else {
        emit toastMessage(QStringLiteral("✓ Copied %1").arg(label));
    }
}

bool Backend::copyImageToClipboard(const QString &localUrlOrPath, const QString &attachmentGuid) {
    QString localPath;
    const QString raw = localUrlOrPath.trimmed();
    if (raw.startsWith(QStringLiteral("file://"))) {
        localPath = QUrl(raw).toLocalFile();
    } else if (!raw.isEmpty() && !raw.startsWith(QStringLiteral("http://")) && !raw.startsWith(QStringLiteral("https://"))) {
        localPath = raw;
    }

    if ((localPath.isEmpty() || !QFileInfo::exists(localPath)) && !attachmentGuid.isEmpty()) {
        const QString cached = lookupCachedAttachmentPath(m_attachmentPaths, attachmentGuid);
        if (!cached.isEmpty() && QFileInfo::exists(cached)) {
            localPath = cached;
        }
    }

    if ((localPath.isEmpty() || !QFileInfo::exists(localPath)) && !attachmentGuid.isEmpty()) {
        for (const QVariant &v : m_messages) {
            const QVariantMap map = v.toMap();
            if (attachmentGuidsMatch(map.value(QStringLiteral("attachmentGuid")).toString(), attachmentGuid)) {
                const QString u = map.value(QStringLiteral("attachmentLocalUrl")).toString();
                const QString p = u.startsWith(QStringLiteral("file://")) ? QUrl(u).toLocalFile() : u;
                if (!p.isEmpty() && QFileInfo::exists(p)) {
                    localPath = p;
                    break;
                }
            }
            const QVariantList imgList = map.value(QStringLiteral("imageAttachments")).toList();
            for (const QVariant &iv : imgList) {
                const QVariantMap imap = iv.toMap();
                if (attachmentGuidsMatch(imap.value(QStringLiteral("guid")).toString(), attachmentGuid)) {
                    const QString u = imap.value(QStringLiteral("localUrl")).toString();
                    const QString p = u.startsWith(QStringLiteral("file://")) ? QUrl(u).toLocalFile() : u;
                    if (!p.isEmpty() && QFileInfo::exists(p)) {
                        localPath = p;
                        break;
                    }
                }
            }
            if (!localPath.isEmpty()) break;
        }
    }

    if (localPath.isEmpty() || !QFileInfo::exists(localPath)) {
        if (!attachmentGuid.isEmpty()) {
            m_pendingClipboardCopyGuid = attachmentGuid;
            downloadAttachment(attachmentGuid, QStringLiteral("copied-image.jpg"), true);
            emit toastMessage(QStringLiteral("⬇ Downloading image to copy..."));
        } else {
            emit toastMessage(QStringLiteral("⚠ Image not downloaded yet"));
        }
        return false;
    }

    // Header-only validation (<0.1ms; does NOT decode pixels on the GUI thread)
    QImageReader headerReader(localPath);
    if (!headerReader.canRead() && !isImageMimeOrExt(QString(), localPath)) {
        emit toastMessage(QStringLiteral("⚠ Could not decode image for clipboard"));
        return false;
    }

    ++(*m_clipboardCopySeq);
    m_lastCopiedImagePath = localPath;
    m_lastCopiedImageTimeMs = QDateTime::currentMSecsSinceEpoch();

    // Instant non-blocking clipboard URL registration (<0.1ms on GUI thread)
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        auto *mime = new QMimeData();
        mime->setUrls({QUrl::fromLocalFile(localPath)});
        if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
            const QSize sz = headerReader.size();
            if (sz.isValid() && sz.width() * sz.height() <= 256 * 256) {
                const QImage tinyImg = headerReader.read();
                if (!tinyImg.isNull()) {
                    mime->setImageData(tinyImg);
                }
            }
        }
        cb->setMimeData(mime, QClipboard::Clipboard);
    }

    const uint64_t copySeq = m_clipboardCopySeq->load();

    // Offload PNG encoding and Wayland wl-copy piping to background worker thread so GUI never freezes
    if (QGuiApplication::platformName() != QStringLiteral("offscreen") &&
        QFileInfo::exists(QStringLiteral("/usr/bin/wl-copy"))) {
        auto seqPtr = m_clipboardCopySeq;
        QThreadPool::globalInstance()->start([localPath, copySeq, seqPtr]() {
            if (seqPtr->load() != copySeq) return;
            QByteArray pngBytes;
            if (localPath.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
                QFile rawPng(localPath);
                if (rawPng.open(QIODevice::ReadOnly)) {
                    pngBytes = rawPng.readAll();
                    rawPng.close();
                }
            } else {
                QImageReader bgReader(localPath);
                bgReader.setAutoTransform(true);
                QImage img = bgReader.read();
                if (img.isNull()) {
                    for (const QByteArray &fmt : {QByteArrayLiteral("JPEG"), QByteArrayLiteral("PNG"), QByteArrayLiteral("WEBP"), QByteArrayLiteral("GIF")}) {
                        QImageReader fb(localPath, fmt);
                        fb.setAutoTransform(true);
                        img = fb.read();
                        if (!img.isNull()) break;
                    }
                }
                if (!img.isNull()) {
                    const QString clipPngPath = QDir::tempPath() + QStringLiteral("/bluepill-clipboard-image.png");
                    QImageWriter writer(clipPngPath, QByteArrayLiteral("png"));
                    writer.setCompression(1); // Fast zlib level 1 (~6x faster than default)
                    if (writer.write(img)) {
                        QFile::setPermissions(clipPngPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                        QFile pngFile(clipPngPath);
                        if (pngFile.open(QIODevice::ReadOnly)) {
                            pngBytes = pngFile.readAll();
                            pngFile.close();
                        }
                    }
                }
            }
            if (seqPtr->load() != copySeq) return;
            if (!pngBytes.isEmpty()) {
                QProcess proc;
                proc.start(QStringLiteral("/usr/bin/wl-copy"), QStringList{QStringLiteral("--type"), QStringLiteral("image/png")});
                if (proc.waitForStarted(1000)) {
                    proc.write(pngBytes);
                    proc.closeWriteChannel();
                    proc.waitForFinished(3000);
                }
            }
        });
    }

    emit toastMessage(QStringLiteral("✓ Copied image to clipboard"));
    return true;
}

QString Backend::pasteClipboardImageToTempFile() {
    QClipboard *cb = QGuiApplication::clipboard();
    const QMimeData *mime = cb ? cb->mimeData() : nullptr;

    // Priority 1 (0ms Fast-Path): Use in-app copied image path directly without decoding or re-encoding
    if (!m_lastCopiedImagePath.isEmpty() && QFileInfo::exists(m_lastCopiedImagePath)) {
        const QString clipText = (mime && mime->hasText()) ? mime->text().trimmed() : QString();
        if (clipText.isEmpty() ||
            clipText == QStringLiteral("📷 Photo") ||
            clipText.startsWith(QStringLiteral("📷 ")) ||
            clipText.startsWith(QStringLiteral("file://")) ||
            clipText == m_lastCopiedImagePath) {
            return m_lastCopiedImagePath;
        }
    }

    // Priority 2 (0ms Fast-Path): Local image file URL in clipboard
    if (mime && mime->hasUrls()) {
        for (const QUrl &u : mime->urls()) {
            if (u.isLocalFile()) {
                const QString localPath = u.toLocalFile();
                if (isImageMimeOrExt(QString(), localPath) && QFileInfo::exists(localPath)) {
                    return localPath;
                }
            }
        }
    }

    // Priority 3 (0ms Fast-Path): Local file:// or absolute image path in clipboard text
    if (mime && mime->hasText()) {
        const QString txt = mime->text().trimmed();
        if (txt.startsWith(QStringLiteral("file://")) || txt.startsWith(QStringLiteral("/"))) {
            const QString candidate = txt.startsWith(QStringLiteral("file://")) ? QUrl(txt).toLocalFile() : txt;
            if (isImageMimeOrExt(QString(), candidate) && QFileInfo::exists(candidate)) {
                return candidate;
            }
        }
    }

    // Priority 4: External bitmap image in clipboard (e.g. screenshot tool / browser Copy Image)
    if (mime && mime->hasImage()) {
        const QImage img = cb->image();
        if (!img.isNull()) {
            const QString outPath = QDir::tempPath() + QStringLiteral("/bluepill-paste-%1.png").arg(QDateTime::currentMSecsSinceEpoch());
            QImageWriter writer(outPath, QByteArrayLiteral("png"));
            writer.setCompression(1);
            if (writer.write(img)) {
                QFile::setPermissions(outPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                return outPath;
            }
        }
    }

    // Priority 5: Wayland wl-paste raw byte stream fallback (no QImage decode/re-encode)
    if (QGuiApplication::platformName() != QStringLiteral("offscreen") &&
        QFileInfo::exists(QStringLiteral("/usr/bin/wl-paste"))) {
        const QString clipText = (mime && mime->hasText()) ? mime->text().trimmed() : QString();
        if (clipText.isEmpty() || clipText == QStringLiteral("📷 Photo") || clipText.startsWith(QStringLiteral("📷 "))) {
            QProcess listProc;
            listProc.start(QStringLiteral("/usr/bin/wl-paste"), QStringList{QStringLiteral("--list-types")});
            if (listProc.waitForFinished(250)) {
                const QString typesOut = QString::fromUtf8(listProc.readAllStandardOutput());
                QString targetMime;
                QString ext;
                if (typesOut.contains(QStringLiteral("image/png"))) {
                    targetMime = QStringLiteral("image/png");
                    ext = QStringLiteral(".png");
                } else if (typesOut.contains(QStringLiteral("image/jpeg"))) {
                    targetMime = QStringLiteral("image/jpeg");
                    ext = QStringLiteral(".jpg");
                } else if (typesOut.contains(QStringLiteral("image/webp"))) {
                    targetMime = QStringLiteral("image/webp");
                    ext = QStringLiteral(".webp");
                }
                if (!targetMime.isEmpty()) {
                    QProcess pasteProc;
                    pasteProc.start(QStringLiteral("/usr/bin/wl-paste"), QStringList{QStringLiteral("--type"), targetMime});
                    if (pasteProc.waitForFinished(450)) {
                        const QByteArray rawBytes = pasteProc.readAllStandardOutput();
                        if (rawBytes.size() >= 64) {
                            const QString outPath = QDir::tempPath() + QStringLiteral("/bluepill-paste-%1%2")
                                                                           .arg(QDateTime::currentMSecsSinceEpoch())
                                                                           .arg(ext);
                            QFile outFile(outPath);
                            if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                                outFile.write(rawBytes);
                                outFile.close();
                                QFile::setPermissions(outPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                                return outPath;
                            }
                        }
                    }
                }
            }
        }
    }
    return QString();
}

void Backend::sendAttachment(const QString &chatGuid, const QString &filePath, const QString &caption) {
    sendAttachmentInternal(chatGuid, filePath, caption, false);
}

void Backend::sendAttachmentInternal(const QString &chatGuid, const QString &filePath, const QString &caption, bool ownedVoiceRecording) {
    if (chatGuid.isEmpty() || filePath.trimmed().isEmpty()) return;
    const QString canonicalGuid = resolveCanonicalChatGuid(chatGuid);
    const QString sendChatGuid = resolveActiveSendChatGuid(canonicalGuid);
    QFileInfo fi(filePath.trimmed());
    if (!fi.exists() || !fi.isFile()) return;
    const QString tempGuid = QStringLiteral("temp-att-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString absLocalPath = fi.absoluteFilePath();
    const QString localUrl = QUrl::fromLocalFile(absLocalPath).toString();
    const bool isImg = isImageMimeOrExt(QString(), fi.fileName());
    const bool isAud = !isImg && isAudioAttachment(QString(), fi.fileName());
    const QString extLower = fi.suffix().toLower();
    QString mimeType = QStringLiteral("application/octet-stream");
    if (isImg) {
        if (extLower == QStringLiteral("jpg") || extLower == QStringLiteral("jpeg")) mimeType = QStringLiteral("image/jpeg");
        else if (extLower == QStringLiteral("gif")) mimeType = QStringLiteral("image/gif");
        else if (extLower == QStringLiteral("webp")) mimeType = QStringLiteral("image/webp");
        else if (extLower == QStringLiteral("heic") || extLower == QStringLiteral("heif")) mimeType = QStringLiteral("image/heic");
        else mimeType = QStringLiteral("image/png");
    } else if (isAud) {
        if (extLower == QStringLiteral("caf")) mimeType = QStringLiteral("audio/x-caf");
        else if (extLower == QStringLiteral("m4a")) mimeType = QStringLiteral("audio/mp4");
        else if (extLower == QStringLiteral("mp3")) mimeType = QStringLiteral("audio/mpeg");
        else if (extLower == QStringLiteral("ogg")) mimeType = QStringLiteral("audio/ogg");
        else if (extLower == QStringLiteral("opus")) mimeType = QStringLiteral("audio/opus");
        else if (extLower == QStringLiteral("aac")) mimeType = QStringLiteral("audio/aac");
        else if (extLower == QStringLiteral("flac")) mimeType = QStringLiteral("audio/flac");
        else if (extLower == QStringLiteral("amr")) mimeType = QStringLiteral("audio/amr");
        else mimeType = QStringLiteral("audio/wav");
    }
    int durationSeconds = isAud ? wavDurationSeconds(absLocalPath) : 0;
    if (isAud && durationSeconds <= 0 && m_voiceRecordingSeconds > 0) {
        durationSeconds = m_voiceRecordingSeconds;
    }
    const QVariantList waveformBars = isAud ? computeAudioFileWaveformBars(absLocalPath, fi.fileName(), 28) : QVariantList();
    insertCachedAttachmentPath(m_attachmentPaths, tempGuid, absLocalPath);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QVariantMap attMap{
        {QStringLiteral("guid"), tempGuid},
        {QStringLiteral("name"), fi.fileName()},
        {QStringLiteral("mimeType"), mimeType},
        {QStringLiteral("localUrl"), localUrl},
        {QStringLiteral("sourceUrl"), localUrl},
        {QStringLiteral("isImage"), isImg},
        {QStringLiteral("isAudio"), isAud},
        {QStringLiteral("durationSeconds"), durationSeconds},
        {QStringLiteral("waveformBars"), waveformBars}
    };
    QVariantMap optimistic;
    optimistic.insert(QStringLiteral("guid"), tempGuid);
    optimistic.insert(QStringLiteral("chatGuid"), canonicalGuid);
    optimistic.insert(QStringLiteral("text"), caption.trimmed());
    optimistic.insert(QStringLiteral("formattedText"), formatMessageTextWithLinks(caption.trimmed()));
    optimistic.insert(QStringLiteral("isFromMe"), true);
    optimistic.insert(QStringLiteral("isIMessage"), true);
    optimistic.insert(QStringLiteral("senderName"), QStringLiteral("You"));
    optimistic.insert(QStringLiteral("senderColor"), QStringLiteral("#7aa2f7"));
    optimistic.insert(QStringLiteral("timeText"), QDateTime::currentDateTime().toString(QStringLiteral("h:mm AP")));
    optimistic.insert(QStringLiteral("metaBadgeText"), QDateTime::currentDateTime().toString(QStringLiteral("h:mm AP")));
    optimistic.insert(QStringLiteral("hasImageAttachment"), isImg);
    optimistic.insert(QStringLiteral("hasFileAttachment"), !isImg);
    optimistic.insert(QStringLiteral("attachmentGuid"), tempGuid);
    optimistic.insert(QStringLiteral("attachmentName"), fi.fileName());
    optimistic.insert(QStringLiteral("attachmentLocalUrl"), localUrl);
    optimistic.insert(QStringLiteral("attachments"), QVariantList{attMap});
    if (isImg) {
        optimistic.insert(QStringLiteral("imageAttachments"), QVariantList{attMap});
        optimistic.insert(QStringLiteral("imageAttachmentCount"), 1);
    } else {
        optimistic.insert(QStringLiteral("fileAttachments"), QVariantList{attMap});
        optimistic.insert(QStringLiteral("imageAttachmentCount"), 0);
    }
    optimistic.insert(QStringLiteral("receiptText"), m_demoMode ? QStringLiteral("Delivered") : QStringLiteral("Sending..."));
    optimistic.insert(QStringLiteral("dateCreated"), nowMs);
    optimistic.insert(QStringLiteral("optimisticCreatedAtMs"), nowMs);
    if (canonicalGuid == m_selectedChatGuid || chatGuid == m_selectedChatGuid) {
        emit aboutToChangeMessages();
        m_messages.append(optimistic);
        enforceLatestOutgoingReceiptsInTimeline();
        emit messagesChanged();
    }

    m_unreadOverrides.remove(canonicalGuid);
    for (ChatEntry &c : m_chats) {
        if (c.guid == canonicalGuid) {
            c.lastMessagePreview = caption.trimmed().isEmpty()
                                       ? (isImg ? QStringLiteral("📷 Photo") : (isAud ? QStringLiteral("🎤 Voice Memo") : (QStringLiteral("📎 ") + fi.fileName())))
                                       : caption.trimmed();
            c.lastMessageDate = nowMs;
            c.relativeTime = QStringLiteral("now");
            c.unreadCount = 0;
            m_readWatermarks[canonicalGuid] = qMax(m_readWatermarks.value(canonicalGuid, 0), nowMs);
            break;
        }
    }
    sortChats();
    emit chatsChanged();
    emit toastMessage(isAud ? QStringLiteral("🎤 Sending voice memo...") : QStringLiteral("📎 Sending %1...").arg(fi.fileName()));
    if (m_demoMode || m_serverUrl.isEmpty()) {
        if (m_demoMode && m_demoChatMessages.contains(canonicalGuid)) {
            m_demoChatMessages[canonicalGuid].append(optimistic);
        }
        return;
    }

    ++m_activeSendCount;
    emit sendingMessageChanged();

    QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    auto addTextPart = [multiPart](const QByteArray &name, const QString &val) {
        QHttpPart p;
        p.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"%1\"").arg(QString::fromLatin1(name))));
        p.setBody(val.toUtf8());
        multiPart->append(p);
    };
    addTextPart("chatGuid", sendChatGuid);
    addTextPart("tempGuid", tempGuid);
    addTextPart("name", fi.fileName());
    addTextPart("method", QStringLiteral("private-api"));
    if (!caption.trimmed().isEmpty()) addTextPart("message", caption.trimmed());
    QFile *file = new QFile(absLocalPath, multiPart);
    if (!file->open(QIODevice::ReadOnly)) {
        delete multiPart;
        if (m_activeSendCount > 0) {
            --m_activeSendCount;
            emit sendingMessageChanged();
        }
        return;
    }
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QVariant(QStringLiteral("form-data; name=\"attachment\"; filename=\"%1\"").arg(fi.fileName())));
    filePart.setBodyDevice(file);
    multiPart->append(filePart);
    QNetworkRequest req(buildApiUrl(QStringLiteral("/api/v1/message/attachment")));
    req.setPriority(QNetworkRequest::HighPriority);
    req.setTransferTimeout(45000);
    const QString trimmedCaption = caption.trimmed();
    QNetworkReply *reply = m_sendNam.post(req, multiPart);
    multiPart->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, canonicalGuid, tempGuid, nowMs, absLocalPath, mimeType, durationSeconds, waveformBars, trimmedCaption, isAud, ownedVoiceRecording]() {
        reply->deleteLater();
        if (m_activeSendCount > 0) {
            --m_activeSendCount;
            emit sendingMessageChanged();
        }
        auto markAttachmentFailed = [this, canonicalGuid, tempGuid]() {
            if (canonicalGuid == m_selectedChatGuid) {
                for (int i = 0; i < m_messages.size(); ++i) {
                    QVariantMap m = m_messages[i].toMap();
                    if (m.value(QStringLiteral("guid")).toString() == tempGuid) {
                        m.insert(QStringLiteral("receiptText"), QStringLiteral("⚠ Not Delivered"));
                        m.insert(QStringLiteral("isFailed"), true);
                        m.insert(QStringLiteral("errorCode"), 4);
                        m_messages[i] = m;
                        emit messagesChanged();
                        break;
                    }
                }
            }
            emit toastMessage(QStringLiteral("⚠ Failed to send attachment"));
        };
        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray respBytes = reply->readAll();
            QJsonParseError parseErr{};
            const QJsonDocument doc = QJsonDocument::fromJson(respBytes, &parseErr);
            const QJsonObject rootObj = (parseErr.error == QJsonParseError::NoError && doc.isObject())
                                            ? doc.object()
                                            : QJsonObject();
            const QJsonValue dataVal = rootObj.value(QStringLiteral("data"));
            const QJsonObject dataObj = dataVal.isObject() ? dataVal.toObject() : QJsonObject();
            const QString realGuidRaw = dataObj.value(QStringLiteral("guid")).toString().trimmed();
            const QString ackTempGuid = dataObj.value(QStringLiteral("tempGuid")).toString().trimmed();
            const QString ackUploadUuid = dataObj.value(QStringLiteral("uploadUuid")).toString().trimmed();
            const bool looksLikeSyntheticChatGuid =
                realGuidRaw.startsWith(QStringLiteral("iMessage;")) ||
                realGuidRaw.startsWith(QStringLiteral("SMS;")) ||
                realGuidRaw.startsWith(QStringLiteral("RCS;"));
            const QString realGuid = looksLikeSyntheticChatGuid ? QString() : realGuidRaw;

            const bool tempGuidMatchesIfPresent =
                ackTempGuid.isEmpty() || tempGuid.isEmpty() || (ackTempGuid == tempGuid);
            const bool hasCorrelatedIdentity =
                tempGuidMatchesIfPresent &&
                (!realGuid.isEmpty() || !ackTempGuid.isEmpty() || !ackUploadUuid.isEmpty());
            const bool isExplicitPendingAck =
                dataObj.value(QStringLiteral("pending")).isBool() &&
                dataObj.value(QStringLiteral("pending")).toBool(false) &&
                hasCorrelatedIdentity;

            const int sendErrCode = dataObj.value(QStringLiteral("error")).toInt(0);
            const bool hasExplicitError =
                dataObj.value(QStringLiteral("error")).isDouble() && (sendErrCode != 0);
            const bool hasSendBool =
                dataObj.value(QStringLiteral("isSent")).isBool() ||
                dataObj.value(QStringLiteral("isDelivered")).isBool();
            const bool hasPositiveReceiptTimestamp =
                (dataObj.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0) ||
                (dataObj.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0);
            const bool hasConfirmedDbRow =
                (!realGuid.isEmpty()) &&
                ((dataObj.value(QStringLiteral("originalROWID")).toVariant().toLongLong() > 0) ||
                 !dataObj.value(QStringLiteral("attachments")).toArray().isEmpty());
            const bool hasSendResult =
                tempGuidMatchesIfPresent &&
                (hasExplicitError ||
                 ((hasSendBool || hasPositiveReceiptTimestamp || hasConfirmedDbRow) && hasCorrelatedIdentity));

            const bool validUploadAck =
                (parseErr.error == QJsonParseError::NoError) &&
                doc.isObject() &&
                dataVal.isObject() &&
                !dataObj.isEmpty() &&
                tempGuidMatchesIfPresent &&
                (isExplicitPendingAck || hasSendResult);
            if (!validUploadAck) {
                markAttachmentFailed();
                return;
            }
            int mergedErrCode = sendErrCode;
            QString finalReceipt;
            QJsonObject effectiveObj = dataObj;
            auto makeFallbackAttachment = [&](const QString &attGuid) {
                QJsonObject optAtt{
                    {QStringLiteral("guid"), attGuid},
                    {QStringLiteral("transferName"), QFileInfo(absLocalPath).fileName()},
                    {QStringLiteral("mimeType"), mimeType}
                };
                if (durationSeconds > 0) {
                    optAtt.insert(QStringLiteral("durationSeconds"), durationSeconds);
                }
                if (!waveformBars.isEmpty()) {
                    QJsonArray wfArr;
                    for (const QVariant &v : waveformBars) {
                        wfArr.append(v.toDouble());
                    }
                    optAtt.insert(QStringLiteral("waveform"), wfArr);
                }
                return optAtt;
            };

            if (!realGuid.isEmpty()) {
                m_seenMessageGuids.insert(realGuid);
                if (!tempGuid.isEmpty() && tempGuid != realGuid) {
                    m_seenMessageGuids.remove(tempGuid);
                    clearPendingDeliveryTrackingForGuid(tempGuid, true);
                    if (m_chatMessagesCache.contains(canonicalGuid)) {
                        m_chatMessagesCache[canonicalGuid].remove(tempGuid);
                    }
                }
                QJsonObject uploadObj = dataObj;
                if (uploadObj.value(QStringLiteral("tempGuid")).toString().trimmed().isEmpty() && !tempGuid.isEmpty()) {
                    uploadObj.insert(QStringLiteral("tempGuid"), tempGuid);
                }
                if (!uploadObj.contains(QStringLiteral("isFromMe"))) {
                    uploadObj.insert(QStringLiteral("isFromMe"), true);
                }
                const bool uploadHasPositiveDelivery =
                    uploadObj.value(QStringLiteral("isDelivered")).toBool(false) ||
                    (uploadObj.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0) ||
                    (uploadObj.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0);
                if (!uploadHasPositiveDelivery && sendErrCode == 0) {
                    if (uploadObj.value(QStringLiteral("pending")).toBool(false)) {
                        if (!uploadObj.value(QStringLiteral("isSent")).isBool()) {
                            uploadObj.insert(QStringLiteral("isSent"), false);
                        }
                        if (!uploadObj.value(QStringLiteral("isDelivered")).isBool()) {
                            uploadObj.insert(QStringLiteral("isDelivered"), false);
                        }
                    } else if (!uploadObj.contains(QStringLiteral("isSent")) &&
                               !uploadObj.contains(QStringLiteral("isDelivered"))) {
                        uploadObj.insert(QStringLiteral("isSent"), true);
                        uploadObj.insert(QStringLiteral("isDelivered"), false);
                    }
                }
                if (uploadObj.value(QStringLiteral("pending")).toBool(false) || (sendErrCode != 0)) {
                    if (extractMessageText(uploadObj).trimmed().isEmpty() && !trimmedCaption.isEmpty()) {
                        uploadObj.insert(QStringLiteral("text"), trimmedCaption);
                    }
                    if (uploadObj.value(QStringLiteral("dateCreated")).toVariant().toLongLong() <= 0) {
                        uploadObj.insert(QStringLiteral("dateCreated"), nowMs);
                    }
                    if (uploadObj.value(QStringLiteral("attachments")).toArray().isEmpty()) {
                        uploadObj.insert(QStringLiteral("attachments"), QJsonArray{makeFallbackAttachment(tempGuid)});
                    }
                }
                cacheMessageForChat(canonicalGuid, uploadObj);
                if (m_chatMessagesCache.contains(canonicalGuid) &&
                    m_chatMessagesCache.value(canonicalGuid).contains(realGuid)) {
                    effectiveObj = m_chatMessagesCache.value(canonicalGuid).value(realGuid);
                } else {
                    effectiveObj = uploadObj;
                }
                finalReceipt = resolveMergedOutgoingReceiptAndPending(
                    canonicalGuid, realGuid, uploadObj, &mergedErrCode);
            } else {
                const QString dataTempGuid = dataObj.value(QStringLiteral("tempGuid")).toString().trimmed();
                const QString dataUploadUuid = dataObj.value(QStringLiteral("uploadUuid")).toString().trimmed();
                const QString stableTempKey = !tempGuid.isEmpty() ? tempGuid : dataTempGuid;
                const QString stableUploadUuid = !dataUploadUuid.isEmpty()
                                                     ? dataUploadUuid
                                                     : (!dataTempGuid.isEmpty() ? dataTempGuid : stableTempKey);
                QJsonObject pendingAck = dataObj;
                pendingAck.insert(QStringLiteral("guid"), stableTempKey);
                pendingAck.insert(QStringLiteral("tempGuid"), stableTempKey);
                if (!stableUploadUuid.isEmpty()) {
                    pendingAck.insert(QStringLiteral("uploadUuid"), stableUploadUuid);
                }
                if (!pendingAck.contains(QStringLiteral("isFromMe"))) {
                    pendingAck.insert(QStringLiteral("isFromMe"), true);
                }
                if (!pendingAck.contains(QStringLiteral("pending"))) {
                    pendingAck.insert(QStringLiteral("pending"), true);
                }
                const bool hasPositiveDeliveryEvidence =
                    pendingAck.value(QStringLiteral("isDelivered")).toBool(false) ||
                    (pendingAck.value(QStringLiteral("dateDelivered")).toVariant().toLongLong() > 0) ||
                    (pendingAck.value(QStringLiteral("dateRead")).toVariant().toLongLong() > 0);
                if (!hasPositiveDeliveryEvidence && pendingAck.value(QStringLiteral("error")).toInt(0) == 0) {
                    pendingAck.insert(QStringLiteral("isSent"), false);
                    pendingAck.insert(QStringLiteral("isDelivered"), false);
                    pendingAck.insert(QStringLiteral("pending"), true);
                }
                if (extractMessageText(pendingAck).trimmed().isEmpty() && !trimmedCaption.isEmpty()) {
                    pendingAck.insert(QStringLiteral("text"), trimmedCaption);
                }
                if (pendingAck.value(QStringLiteral("dateCreated")).toVariant().toLongLong() <= 0) {
                    pendingAck.insert(QStringLiteral("dateCreated"), nowMs);
                }
                if (pendingAck.value(QStringLiteral("attachments")).toArray().isEmpty()) {
                    pendingAck.insert(QStringLiteral("attachments"), QJsonArray{makeFallbackAttachment(stableTempKey)});
                }
                cacheMessageForChat(canonicalGuid, pendingAck);
                if (m_chatMessagesCache.contains(canonicalGuid) &&
                    m_chatMessagesCache.value(canonicalGuid).contains(stableTempKey)) {
                    effectiveObj = m_chatMessagesCache.value(canonicalGuid).value(stableTempKey);
                } else {
                    effectiveObj = pendingAck;
                }
                mergedErrCode = effectiveObj.value(QStringLiteral("error")).toInt(sendErrCode);
                finalReceipt = computeOutgoingReceiptText(effectiveObj);
            }

            QJsonArray atts = effectiveObj.value(QStringLiteral("attachments")).toArray();
            if (atts.isEmpty()) {
                atts = dataObj.value(QStringLiteral("attachments")).toArray();
            }
            QString firstAttGuid;
            QString persistedLocalPath = absLocalPath;
            int serverDurationSec = 0;
            QVariantList serverWaveformBars;
            bool copiedVoiceMemoToPersistentCache = false;
            for (const QJsonValue &av : atts) {
                const QJsonObject aObj = av.toObject();
                const QString ag = aObj.value(QStringLiteral("guid")).toString();
                if (!ag.isEmpty() && QFileInfo::exists(absLocalPath)) {
                    QString cachePathForGuid = absLocalPath;
                    if (isAud && !absLocalPath.contains(QStringLiteral("/.cache/imsg/attachments/"))) {
                        const QString cacheDir = QDir::homePath() + QStringLiteral("/.cache/imsg/attachments");
                        QDir().mkpath(cacheDir);
                        QString safeName = QFileInfo(absLocalPath).fileName();
                        safeName.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9._-]")), QStringLiteral("_"));
                        const QString persistentTarget = cacheDir + QLatin1Char('/') + ag.left(16) + QLatin1Char('_') + safeName;
                        if (QFile::exists(persistentTarget) || QFile::copy(absLocalPath, persistentTarget)) {
                            cachePathForGuid = persistentTarget;
                            if (persistentTarget != absLocalPath) {
                                copiedVoiceMemoToPersistentCache = true;
                            }
                        }
                    }
                    insertCachedAttachmentPath(m_attachmentPaths, ag, cachePathForGuid);
                    if (firstAttGuid.isEmpty()) {
                        firstAttGuid = ag;
                        persistedLocalPath = cachePathForGuid;
                        const double rawServerDur = aObj.value(QStringLiteral("durationSeconds")).toDouble(0.0);
                        serverDurationSec = (rawServerDur > 0.0) ? qMax(1, qRound(rawServerDur)) : 0;
                        if (serverDurationSec <= 0) {
                            const double fallbackDur = aObj.value(QStringLiteral("duration")).toDouble(0.0);
                            serverDurationSec = (fallbackDur > 0.0) ? qMax(1, qRound(fallbackDur)) : 0;
                        }
                        if (aObj.value(QStringLiteral("waveform")).isArray()) {
                            for (const QJsonValue &wv : aObj.value(QStringLiteral("waveform")).toArray()) {
                                serverWaveformBars.append(qBound(0.15, wv.toDouble(0.15), 1.0));
                            }
                        }
                    }
                }
            }
            if (ownedVoiceRecording && copiedVoiceMemoToPersistentCache && persistedLocalPath != absLocalPath &&
                QFileInfo::exists(persistedLocalPath)) {
                insertCachedAttachmentPath(m_attachmentPaths, tempGuid, persistedLocalPath);
                QFile::remove(absLocalPath);
            }
            if (mergedErrCode != 0) {
                emit toastMessage(QStringLiteral("⚠ Failed to send attachment (error %1)").arg(mergedErrCode));
            }
            if (!firstAttGuid.isEmpty() && m_playingAudioGuid == tempGuid) {
                m_playingAudioGuid = firstAttGuid;
                if (!persistedLocalPath.isEmpty()) {
                    m_currentAudioLocalFile = persistedLocalPath;
                }
                emit audioPlaybackChanged();
            }
            const QString persistedLocalUrl = QFileInfo::exists(persistedLocalPath)
                                                  ? QUrl::fromLocalFile(persistedLocalPath).toString()
                                                  : QString();
            auto updateAttList = [&](QVariantMap &msgMap, const QString &listKey) {
                QVariantList lst = msgMap.value(listKey).toList();
                if (lst.isEmpty()) return;
                QVariantMap first = lst[0].toMap();
                if (!firstAttGuid.isEmpty()) first.insert(QStringLiteral("guid"), firstAttGuid);
                if (!persistedLocalUrl.isEmpty()) {
                    first.insert(QStringLiteral("localUrl"), persistedLocalUrl);
                    first.insert(QStringLiteral("sourceUrl"), persistedLocalUrl);
                }
                if (serverDurationSec > 0) first.insert(QStringLiteral("durationSeconds"), serverDurationSec);
                if (!serverWaveformBars.isEmpty()) first.insert(QStringLiteral("waveformBars"), serverWaveformBars);
                lst[0] = first;
                msgMap.insert(listKey, lst);
            };
            if (canonicalGuid == m_selectedChatGuid) {
                int realIdx = -1;
                QList<int> dupOrTempIndices;
                for (int i = 0; i < m_messages.size(); ++i) {
                    const QString curG = m_messages[i].toMap().value(QStringLiteral("guid")).toString();
                    if (!realGuid.isEmpty() && curG == realGuid) {
                        if (realIdx < 0) realIdx = i;
                        else dupOrTempIndices.append(i);
                    } else if (curG == tempGuid) {
                        dupOrTempIndices.append(i);
                    }
                }
                auto applyOpt = [&](QVariantMap &m, bool setRealGuid) {
                    if (setRealGuid && !realGuid.isEmpty()) m.insert(QStringLiteral("guid"), realGuid);
                    if (!firstAttGuid.isEmpty()) m.insert(QStringLiteral("attachmentGuid"), firstAttGuid);
                    if (!persistedLocalUrl.isEmpty()) m.insert(QStringLiteral("attachmentLocalUrl"), persistedLocalUrl);
                    updateAttList(m, QStringLiteral("attachments"));
                    updateAttList(m, QStringLiteral("imageAttachments"));
                    updateAttList(m, QStringLiteral("fileAttachments"));
                    m.insert(QStringLiteral("receiptText"), finalReceipt);
                    m.insert(QStringLiteral("isFailed"), mergedErrCode != 0);
                    m.insert(QStringLiteral("errorCode"), mergedErrCode);
                };
                bool updatedOpt = false;
                if (realIdx >= 0) {
                    QVariantMap m = m_messages[realIdx].toMap();
                    applyOpt(m, false);
                    m_messages[realIdx] = m;
                    for (int k = dupOrTempIndices.size() - 1; k >= 0; --k) {
                        m_messages.removeAt(dupOrTempIndices[k]);
                    }
                    enforceLatestOutgoingReceiptsInTimeline();
                    updatedOpt = true;
                } else if (!dupOrTempIndices.isEmpty()) {
                    const int keepIdx = dupOrTempIndices.first();
                    QVariantMap m = m_messages[keepIdx].toMap();
                    applyOpt(m, true);
                    m_messages[keepIdx] = m;
                    for (int k = dupOrTempIndices.size() - 1; k >= 1; --k) {
                        m_messages.removeAt(dupOrTempIndices[k]);
                    }
                    enforceLatestOutgoingReceiptsInTimeline();
                    updatedOpt = true;
                }
                if (updatedOpt) {
                    emit messagesChanged();
                } else {
                    rebuildSelectedChatMessages();
                }
            }
            saveMessagesCache();
            if (realGuid.isEmpty()) {
                loadChatMessages(canonicalGuid);
            }
        } else {
            markAttachmentFailed();
        }
    });
}

QString Backend::canonicalizeAddress(const QString &raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty()) return QString();
    if (s.contains(QLatin1Char('@'))) {
        return s.toLower();
    }
    QString digits;
    for (const QChar &ch : s) {
        if (ch.isDigit()) digits.append(ch);
    }
    if (digits.size() == 10) {
        return QStringLiteral("+1") + digits;
    }
    if (digits.size() == 11 && digits.startsWith(QLatin1Char('1'))) {
        return QLatin1Char('+') + digits;
    }
    if (!digits.isEmpty() && s.startsWith(QLatin1Char('+'))) {
        return QLatin1Char('+') + digits;
    }
    if (digits.size() >= 7) {
        return QLatin1Char('+') + digits;
    }
    return s;
}

QString Backend::formatHandleForDisplay(const QString &raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty() || s.contains(QLatin1Char('@'))) return s;
    QString digits;
    for (const QChar &ch : s) {
        if (ch.isDigit()) digits.append(ch);
    }
    if (digits.size() == 11 && digits.startsWith(QLatin1Char('1'))) {
        digits = digits.mid(1);
    }
    if (digits.size() == 10) {
        return QStringLiteral("+1 (%1) %2-%3")
            .arg(digits.mid(0, 3), digits.mid(3, 3), digits.mid(6, 4));
    }
    return s;
}

QVariantList Backend::searchContacts(const QString &query, int limit) const {
    if (!m_demoMode && !m_serverUrl.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastContactsFetchMs == 0 || (nowMs - m_lastContactsFetchMs) > 60000) {
            const_cast<Backend*>(this)->m_lastContactsFetchMs = nowMs;
            QTimer::singleShot(0, const_cast<Backend*>(this), &Backend::fetchContactsRemote);
        }
    }
    const QString q = query.trimmed().toLower();
    QString qDigits;
    for (const QChar &ch : q) {
        if (ch.isDigit()) qDigits.append(ch);
    }
    const QStringList qTokens = q.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);

    struct Candidate {
        QString displayName;
        QString address;
        QString formattedAddress;
        QString normalizedHandle;
        QString avatarUrl;
        QString chatGuid;
        QString service = QStringLiteral("iMessage");
        bool hasExistingChat = false;
        qint64 lastMessageDate = 0;
        int score = 0;
    };

    QHash<QString, Candidate> byNorm;

    // 1. Index existing 1:1 chats first so we know which contacts already have active conversations
    for (const ChatEntry &c : m_chats) {
        if (c.isGroup) continue;
        QString rawAddr = !c.participantHandles.isEmpty() ? c.participantHandles.first() : c.chatIdentifier;
        const QString norm = normalizeHandle(rawAddr);
        if (norm.isEmpty()) continue;

        Candidate cand;
        cand.normalizedHandle = norm;
        cand.address = canonicalizeAddress(rawAddr);
        cand.formattedAddress = formatHandleForDisplay(cand.address);
        cand.displayName = m_contactNames.value(norm, c.title);
        cand.avatarUrl = m_contactAvatars.value(norm, c.avatarUrl);
        cand.chatGuid = c.guid;
        cand.service = c.service.isEmpty() ? QStringLiteral("iMessage") : c.service;
        cand.hasExistingChat = true;
        cand.lastMessageDate = c.lastMessageDate;
        byNorm.insert(norm, cand);
    }

    // 2. Merge all contacts from m_contactNames
    for (auto it = m_contactNames.constBegin(); it != m_contactNames.constEnd(); ++it) {
        const QString norm = it.key();
        const QString name = it.value().trimmed();
        if (norm.isEmpty() || name.isEmpty()) continue;

        if (byNorm.contains(norm)) {
            Candidate &existing = byNorm[norm];
            if (existing.displayName.isEmpty() || normalizeHandle(existing.displayName) == norm) {
                existing.displayName = name;
            }
            if (existing.avatarUrl.isEmpty() && m_contactAvatars.contains(norm)) {
                existing.avatarUrl = m_contactAvatars.value(norm);
            }
            continue;
        }

        Candidate cand;
        cand.normalizedHandle = norm;
        cand.displayName = name;
        const QString rawAddr = m_contactRawAddresses.value(norm,
            norm.contains(QLatin1Char('@')) ? norm : (norm.size() == 10 ? (QStringLiteral("+1") + norm) : norm));
        cand.address = canonicalizeAddress(rawAddr);
        cand.formattedAddress = formatHandleForDisplay(cand.address);
        cand.avatarUrl = m_contactAvatars.value(norm);
        cand.hasExistingChat = false;
        byNorm.insert(norm, cand);
    }

    QList<Candidate> matches;
    for (auto it = byNorm.constBegin(); it != byNorm.constEnd(); ++it) {
        Candidate cand = it.value();
        const QString nameLower = cand.displayName.toLower();
        const QString addrLower = cand.address.toLower();
        const QString fmtLower = cand.formattedAddress.toLower();
        const QString normLower = cand.normalizedHandle.toLower();
        const QStringList nameWords = nameLower.split(QRegularExpression(QStringLiteral(R"([\s\-_.,()]+)")), Qt::SkipEmptyParts);

        if (q.isEmpty()) {
            cand.score = cand.hasExistingChat ? 500 : 100;
            matches.append(cand);
            continue;
        }

        // Multi-token check: every token in query must match either name, address, or digits
        bool allTokensMatch = true;
        for (const QString &tok : qTokens) {
            if (!nameLower.contains(tok) && !addrLower.contains(tok) && !fmtLower.contains(tok) && !normLower.contains(tok)) {
                allTokensMatch = false;
                break;
            }
        }
        bool digitMatch = (!qDigits.isEmpty() && qDigits.size() >= 3 && normLower.contains(qDigits));
        if (!allTokensMatch && !digitMatch) {
            continue;
        }

        int score = 0;
        if (nameLower == q || addrLower == q || normLower == q) {
            score = 1000;
        } else if (!nameWords.isEmpty() && nameWords.first() == q) {
            // Exact first name match (e.g., query "alex" matches "Alex Rivera" above "Alexa")
            score = 950;
        } else if (nameLower.startsWith(q + QLatin1Char(' '))) {
            score = 940;
        } else if (nameLower.startsWith(q)) {
            score = 850;
        } else {
            bool wordExact = false;
            bool wordPrefix = false;
            for (const QString &w : nameWords) {
                if (w == q) { wordExact = true; break; }
                if (w.startsWith(q)) { wordPrefix = true; }
            }
            if (wordExact) score = 820;
            else if (wordPrefix) score = 780;
            else if (normLower.startsWith(q) || (!qDigits.isEmpty() && normLower.startsWith(qDigits))) score = 720;
            else if (nameLower.contains(q)) score = 600;
            else score = 500;
        }

        if (cand.hasExistingChat) {
            score += 25;
        }
        cand.score = score;
        matches.append(cand);
    }

    std::sort(matches.begin(), matches.end(), [](const Candidate &a, const Candidate &b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.hasExistingChat != b.hasExistingChat) return a.hasExistingChat > b.hasExistingChat;
        if (a.lastMessageDate != b.lastMessageDate) return a.lastMessageDate > b.lastMessageDate;
        const int cmp = QString::compare(a.displayName, b.displayName, Qt::CaseInsensitive);
        if (cmp != 0) return cmp < 0;
        return a.address < b.address;
    });

    QVariantList result;
    const int maxItems = (limit > 0) ? limit : 15;
    for (const Candidate &cand : matches) {
        if (result.size() >= maxItems) break;
        QVariantMap map;
        map.insert(QStringLiteral("name"), cand.displayName);
        map.insert(QStringLiteral("displayName"), cand.displayName);
        map.insert(QStringLiteral("handle"), cand.address);
        map.insert(QStringLiteral("address"), cand.address);
        map.insert(QStringLiteral("displayHandle"), cand.formattedAddress);
        map.insert(QStringLiteral("formattedAddress"), cand.formattedAddress);
        map.insert(QStringLiteral("normalizedHandle"), cand.normalizedHandle);
        map.insert(QStringLiteral("initials"), computeInitials(cand.displayName));
        map.insert(QStringLiteral("avatarColor"), computeAvatarColor(cand.displayName));
        map.insert(QStringLiteral("avatarUrl"), cand.avatarUrl);
        map.insert(QStringLiteral("hasExistingChat"), cand.hasExistingChat);
        map.insert(QStringLiteral("chatGuid"), cand.chatGuid);
        map.insert(QStringLiteral("service"), cand.service);
        result.append(map);
    }

    // If the user typed a raw phone number (>= 7 digits) or email address not already in results, offer a direct "New recipient" item
    const bool looksLikeEmail = q.contains(QLatin1Char('@')) && q.contains(QLatin1Char('.'));
    const bool looksLikePhone = (qDigits.size() >= 7 && qDigits.size() <= 15);
    if ((looksLikeEmail || looksLikePhone) && !byNorm.contains(normalizeHandle(query))) {
        const QString canon = canonicalizeAddress(query);
        const QString fmt = formatHandleForDisplay(canon);
        QVariantMap directMap;
        directMap.insert(QStringLiteral("name"), fmt);
        directMap.insert(QStringLiteral("displayName"), fmt);
        directMap.insert(QStringLiteral("handle"), canon);
        directMap.insert(QStringLiteral("address"), canon);
        directMap.insert(QStringLiteral("displayHandle"), QStringLiteral("New recipient"));
        directMap.insert(QStringLiteral("formattedAddress"), QStringLiteral("New recipient"));
        directMap.insert(QStringLiteral("normalizedHandle"), normalizeHandle(canon));
        directMap.insert(QStringLiteral("initials"), computeInitials(fmt));
        directMap.insert(QStringLiteral("avatarColor"), computeAvatarColor(canon));
        directMap.insert(QStringLiteral("avatarUrl"), QString());
        directMap.insert(QStringLiteral("hasExistingChat"), false);
        directMap.insert(QStringLiteral("chatGuid"), QString());
        directMap.insert(QStringLiteral("service"), QStringLiteral("iMessage"));
        result.append(directMap);
    }

    return result;
}

void Backend::startNewChat(const QString &address, const QString &text) {
    QString rawInput = address.trimmed();
    const QString msg = replaceEmojiShortcodes(text.trimmed());
    if (rawInput.isEmpty()) return;

    if (!m_searchQuery.isEmpty()) {
        m_searchQuery.clear();
        emit filterChanged();
        emit chatsChanged();
    }

    // Support "Display Name <handle>" format if passed from UI
    static const QRegularExpression reAngle(QStringLiteral(R"(<([^>]+)>)"));
    auto mAngle = reAngle.match(rawInput);
    if (mAngle.hasMatch()) {
        rawInput = mAngle.captured(1).trimmed();
    }

    QString resolvedAddr = rawInput;
    QString resolvedChatGuid;

    // 1. Check if rawInput directly matches an existing 1:1 chat by handle or title
    const QString normInput = normalizeHandle(rawInput);
    for (const ChatEntry &c : m_chats) {
        if (!c.isGroup) {
            if ((!normInput.isEmpty() && normalizeHandle(c.chatIdentifier) == normInput) ||
                c.title.compare(rawInput, Qt::CaseInsensitive) == 0) {
                selectChat(c.guid);
                if (!msg.isEmpty()) {
                    sendMessage(c.guid, msg);
                }
                return;
            }
            for (const QString &ph : c.participantHandles) {
                if (!normInput.isEmpty() && normalizeHandle(ph) == normInput) {
                    selectChat(c.guid);
                    if (!msg.isEmpty()) {
                        sendMessage(c.guid, msg);
                    }
                    return;
                }
            }
        }
    }

    // 2. If rawInput is a contact name/prefix (e.g. "alex" or "Alex Rivera"), resolve via searchContacts
    bool hasDigitsOrAt = rawInput.contains(QLatin1Char('@'));
    if (!hasDigitsOrAt) {
        int digitCount = 0;
        for (const QChar &ch : rawInput) {
            if (ch.isDigit()) ++digitCount;
        }
        hasDigitsOrAt = (digitCount >= 7);
    }

    if (!hasDigitsOrAt || !m_contactNames.contains(normInput)) {
        const QVariantList matches = searchContacts(rawInput, 1);
        if (!matches.isEmpty()) {
            const QVariantMap best = matches.first().toMap();
            const QString candAddr = best.value(QStringLiteral("address")).toString();
            const QString candGuid = best.value(QStringLiteral("chatGuid")).toString();
            if (!candGuid.isEmpty()) {
                resolvedChatGuid = candGuid;
            }
            if (!candAddr.isEmpty()) {
                resolvedAddr = candAddr;
            }
        }
    }

    // 3. Re-check if resolvedChatGuid or resolvedAddr matches an existing 1:1 chat in m_chats
    const QString normTarget = normalizeHandle(resolvedAddr);
    for (const ChatEntry &c : m_chats) {
        if (!c.isGroup) {
            if ((!resolvedChatGuid.isEmpty() && c.guid == resolvedChatGuid) ||
                (!normTarget.isEmpty() && normalizeHandle(c.chatIdentifier) == normTarget)) {
                selectChat(c.guid);
                if (!msg.isEmpty()) {
                    sendMessage(c.guid, msg);
                }
                return;
            }
            for (const QString &ph : c.participantHandles) {
                if (!normTarget.isEmpty() && normalizeHandle(ph) == normTarget) {
                    selectChat(c.guid);
                    if (!msg.isEmpty()) {
                        sendMessage(c.guid, msg);
                    }
                    return;
                }
            }
        }
    }

    // 4. Create a new local 1:1 ChatEntry immediately so the user can view/type in the conversation right away
    const QString canonicalAddr = canonicalizeAddress(resolvedAddr);
    if (canonicalAddr.isEmpty()) return;

    const QString newChatGuid = QStringLiteral("iMessage;-;") + canonicalAddr;
    m_chatParticipants.insert(newChatGuid, QStringList{canonicalAddr});

    ChatEntry entry;
    entry.guid = newChatGuid;
    entry.chatIdentifier = canonicalAddr;
    entry.participantHandles = QStringList{canonicalAddr};
    entry.isGroup = false;
    entry.service = QStringLiteral("iMessage");
    entry.lastMessageDate = QDateTime::currentMSecsSinceEpoch();
    entry.lastMessagePreview = msg.isEmpty() ? QStringLiteral("New conversation") : msg;
    entry.relativeTime = QStringLiteral("now");
    entry.unreadCount = 0;
    rebuildChatPresentation(entry);

    m_chats.prepend(entry);
    sortChats();
    selectChat(newChatGuid);

    if (!msg.isEmpty()) {
        sendMessage(newChatGuid, msg);
    }
}

void Backend::registerWindow(QObject *window) {
    if (!window) return;
    if (m_rootWindow && m_rootWindow != window) {
        m_rootWindow->removeEventFilter(this);
    }
    m_rootWindow = window;
    m_rootWindow->installEventFilter(this);
    if (auto *app = QCoreApplication::instance()) {
        app->removeEventFilter(this);
        app->installEventFilter(this);
    }
}

bool Backend::eventFilter(QObject *watched, QEvent *event) {
    if (m_rootWindow && watched == m_rootWindow && event) {
        const QEvent::Type et = event->type();
        if (et == QEvent::ShortcutOverride || et == QEvent::KeyPress) {
            if (m_rootWindow->property("showPhotoViewer").toBool()) {
                auto *ke = static_cast<QKeyEvent *>(event);
                const int k = ke->key();
                const QString t = ke->text();
                const bool isNavPrev = (k == Qt::Key_Left || k == Qt::Key_Up || k == Qt::Key_Backtab);
                const bool isNavNext = (k == Qt::Key_Right || k == Qt::Key_Down || k == Qt::Key_Tab || k == Qt::Key_Space);
                const bool isZoomIn = (k == Qt::Key_Plus || k == Qt::Key_Equal || t == QStringLiteral("+") || t == QStringLiteral("="));
                const bool isZoomOut = (k == Qt::Key_Minus || k == Qt::Key_Underscore || t == QStringLiteral("-") || t == QStringLiteral("_"));
                const bool isZoomReset = (k == Qt::Key_0 || t == QStringLiteral("0"));
                const bool isClose = (k == Qt::Key_Escape);

                if (isNavPrev || isNavNext || isZoomIn || isZoomOut || isZoomReset || isClose) {
                    if (et == QEvent::ShortcutOverride) {
                        event->accept();
                        return true;
                    }
                    if (isNavPrev) {
                        QMetaObject::invokeMethod(m_rootWindow, "prevPhotoInViewer");
                    } else if (isNavNext) {
                        QMetaObject::invokeMethod(m_rootWindow, "nextPhotoInViewer");
                    } else if (isZoomIn) {
                        QMetaObject::invokeMethod(m_rootWindow, "zoomInPhotoViewer");
                    } else if (isZoomOut) {
                        QMetaObject::invokeMethod(m_rootWindow, "zoomOutPhotoViewer");
                    } else if (isZoomReset) {
                        QMetaObject::invokeMethod(m_rootWindow, "resetZoomPhotoViewer");
                    } else if (isClose) {
                        QMetaObject::invokeMethod(m_rootWindow, "closePhotoViewer");
                    }
                    event->accept();
                    return true;
                }
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

void Backend::openUrl(const QString &url) {
    const QString normalized = normalizeExtractedHrefUrl(url);
    if (normalized.isEmpty()) return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (normalized == m_lastOpenedUrl && (nowMs - m_lastOpenedUrlTimeMs) < 500) {
        return;
    }
    m_lastOpenedUrl = normalized;
    m_lastOpenedUrlTimeMs = nowMs;

    const bool opened = QDesktopServices::openUrl(QUrl(normalized));
    if (!opened &&
        QGuiApplication::platformName() != QStringLiteral("offscreen") &&
        QFileInfo::exists(QStringLiteral("/usr/bin/xdg-open"))) {
        QProcess::startDetached(QStringLiteral("/usr/bin/xdg-open"), QStringList{normalized});
    }
}

void Backend::openAttachment(const QString &guid, const QString &transferName) {
    const QString filePath = lookupCachedAttachmentPath(m_attachmentPaths, guid);
    if (!filePath.isEmpty() && QFileInfo::exists(filePath)) {
        const QString lower = filePath.toLower();
        if (isImageMimeOrExt(QString(), transferName) ||
            lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg")) ||
            lower.endsWith(QStringLiteral(".png")) || lower.endsWith(QStringLiteral(".gif")) ||
            lower.endsWith(QStringLiteral(".webp")) || lower.endsWith(QStringLiteral(".bmp"))) {
            emit attachmentReady(guid, QUrl::fromLocalFile(filePath).toString());
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
        return;
    }
    downloadAttachment(guid, transferName, true);
}

bool Backend::shouldNotifyForIncomingMessage(const QJsonObject &m, const QString &chatGuid, qint64 dateCreated, qint64 rowId) const {
    if (!areOmarchyNotificationsEnabled()) return false;
    if (chatGuid.isEmpty() || m_mutedChats.contains(chatGuid)) return false;
    if (m_autoStart && !m_initialSyncComplete) return false;

    const QString msgGuid = m.value(QStringLiteral("guid")).toString().trimmed();
    if (msgGuid.isEmpty() || msgGuid.startsWith(QStringLiteral("temp-")) || msgGuid.startsWith(QStringLiteral("bp-"))) {
        return false;
    }
    if (m_notifiedMessageGuids.contains(msgGuid) || m_deletedMessageGuids.contains(msgGuid)) {
        return false;
    }
    if (isMessageDeletedOrRetracted(m)) {
        return false;
    }
    const bool isFromMe = m.value(QStringLiteral("isFromMe")).toBool()
                          || m.value(QStringLiteral("is_from_me")).toBool()
                          || (m.value(QStringLiteral("is_from_me")).toInt(0) == 1);
    if (isFromMe) {
        return false;
    }
    if (!isMessageUnread(m, m_readWatermarks.value(chatGuid, 0), m_unreadOverrides.value(chatGuid, 0))) {
        return false;
    }
    // Never notify for messages created before BluePill started or older than our last notified message
    if (dateCreated < m_startupTimestamp) {
        return false;
    }
    if (m_startupHighestRowId > 0 && rowId > 0 && rowId <= m_startupHighestRowId) {
        return false;
    }
    return true;
}

QString Backend::ensureNotificationSoundFile(const QString &preset) {
    QString norm = preset.trimmed().toLower();
    if (norm.isEmpty()) norm = QStringLiteral("chime");

    if (norm == QStringLiteral("chime")) {
        if (QFileInfo::exists(QStringLiteral("/usr/share/imsg/imessage-notify.wav"))) {
            return QStringLiteral("/usr/share/imsg/imessage-notify.wav");
        }
        const QString localPath = QCoreApplication::applicationDirPath() + QStringLiteral("/../resources/imessage-notify.wav");
        if (QFileInfo::exists(localPath)) {
            return localPath;
        }
    } else if (norm == QStringLiteral("system")) {
        if (QFileInfo::exists(QStringLiteral("/usr/share/sounds/freedesktop/stereo/message-new-instant.oga"))) {
            return QStringLiteral("/usr/share/sounds/freedesktop/stereo/message-new-instant.oga");
        }
    }

    const QString soundsDir = QDir::homePath() + QStringLiteral("/.cache/bluepill/sounds");
    QDir().mkpath(soundsDir);
    const QString wavPath = soundsDir + QStringLiteral("/notify-%1.wav").arg(norm);
    if (QFileInfo::exists(wavPath) && QFileInfo(wavPath).size() > 1024) {
        return wavPath;
    }

    constexpr int sampleRate = 44100;
    int durationMs = 420;
    if (norm == QStringLiteral("marimba")) durationMs = 460;
    else if (norm == QStringLiteral("pop")) durationMs = 180;
    else if (norm == QStringLiteral("glass")) durationMs = 340;

    const int numSamples = (sampleRate * durationMs) / 1000;
    QByteArray pcm(numSamples * int(sizeof(int16_t)), 0);
    auto *samples = reinterpret_cast<int16_t *>(pcm.data());
    constexpr double twoPi = 6.28318530717958647692;

    for (int i = 0; i < numSamples; ++i) {
        const double t = double(i) / double(sampleRate);
        double env = 0.0;
        double sig = 0.0;

        if (norm == QStringLiteral("crystal")) {
            env = std::min(1.0, t / 0.004) * std::exp(-t * 7.5);
            sig = 0.65 * std::sin(twoPi * 1318.51 * t)
                + 0.25 * std::sin(twoPi * 2637.02 * t)
                + 0.10 * std::sin(twoPi * 3639.0 * t);
        } else if (norm == QStringLiteral("marimba")) {
            double freq = 987.77; // B5
            double localT = t;
            if (t >= 0.19) {
                freq = 1661.22; // G#6
                localT = t - 0.19;
            } else if (t >= 0.095) {
                freq = 1318.51; // E6
                localT = t - 0.095;
            }
            env = std::min(1.0, localT / 0.003) * std::exp(-localT * 12.0);
            sig = 0.78 * std::sin(twoPi * freq * localT)
                + 0.22 * std::sin(twoPi * (freq * 4.0) * localT) * std::exp(-localT * 28.0);
        } else if (norm == QStringLiteral("pop")) {
            if (t < 0.095) {
                const double sweepPhase = twoPi * (360.0 * t + 3800.0 * t * t);
                env = std::sin((t / 0.095) * 3.14159265358979323846);
                sig = 0.85 * std::sin(sweepPhase);
            }
        } else if (norm == QStringLiteral("glass")) {
            env = std::min(1.0, t / 0.002) * std::exp(-t * 10.5);
            sig = 0.72 * std::sin(twoPi * 1760.0 * t)
                + 0.28 * std::sin(twoPi * 2637.02 * t);
        } else {
            // Default two-tone chime (C6 -> G6)
            const double freq = (t < 0.11) ? 1046.50 : 1567.98;
            const double localT = (t < 0.11) ? t : (t - 0.11);
            env = std::min(1.0, localT / 0.004) * std::exp(-localT * 8.5);
            sig = 0.75 * std::sin(twoPi * freq * localT)
                + 0.25 * std::sin(twoPi * (freq * 2.0) * localT);
        }

        const double clamped = std::max(-1.0, std::min(1.0, sig * env * 0.55));
        samples[i] = static_cast<int16_t>(clamped * 32767.0);
    }

    QByteArray wav;
    wav.reserve(44 + pcm.size());
    auto appendU32 = [&wav](uint32_t v) {
        char b[4] = {
            static_cast<char>(v & 0xFF),
            static_cast<char>((v >> 8) & 0xFF),
            static_cast<char>((v >> 16) & 0xFF),
            static_cast<char>((v >> 24) & 0xFF)
        };
        wav.append(b, 4);
    };
    auto appendU16 = [&wav](uint16_t v) {
        char b[2] = {
            static_cast<char>(v & 0xFF),
            static_cast<char>((v >> 8) & 0xFF)
        };
        wav.append(b, 2);
    };

    const uint32_t dataSize = static_cast<uint32_t>(pcm.size());
    wav.append("RIFF", 4);
    appendU32(36 + dataSize);
    wav.append("WAVEfmt ", 8);
    appendU32(16); // PCM chunk size
    appendU16(1);  // AudioFormat = 1 (PCM)
    appendU16(1);  // NumChannels = 1 (Mono)
    appendU32(sampleRate);
    appendU32(sampleRate * 2);
    appendU16(2);  // BlockAlign
    appendU16(16); // BitsPerSample
    wav.append("data", 4);
    appendU32(dataSize);
    wav.append(pcm);

    QFile out(wavPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(wav);
        out.close();
    }
    return wavPath;
}

void Backend::previewNotificationSound(const QString &preset) {
    const QString chosen = preset.trimmed().isEmpty() ? m_notificationSoundPreset : preset.trimmed().toLower();
    playNotificationSound(chosen, true);
}

void Backend::sendTestNotification() {
    m_notificationCountForTest++;
    const bool isOffscreen = (QGuiApplication::platformName() == QStringLiteral("offscreen"));
    if (!isOffscreen && !m_demoMode && m_notificationBannerEnabled) {
        QProcess::startDetached(QStringLiteral("notify-send"),
                                {QStringLiteral("-a"), QStringLiteral("BluePill"),
                                 QStringLiteral("-i"), QStringLiteral("messaging-im"),
                                 QStringLiteral("-u"), QStringLiteral("normal"),
                                 QStringLiteral("BluePill Test Notification"),
                                 QStringLiteral("Audio preset: %1").arg(m_notificationSoundPreset)});
    }
    playNotificationSound(m_notificationSoundPreset, true);
    emit toastMessage(QStringLiteral("🔔 Sent test notification (%1)").arg(m_notificationSoundPreset));
}

void Backend::notifyIncomingMessage(const QString &chatGuid, const QString &sender, const QString &preview) {
    if (!areOmarchyNotificationsEnabled() || m_mutedChats.contains(chatGuid)) return;
    if (m_notificationMuteWhenActive && m_windowActive && !m_selectedChatGuid.isEmpty() && chatGuid == m_selectedChatGuid) {
        return;
    }
    m_notificationCountForTest++;
    const bool isOffscreen = (QGuiApplication::platformName() == QStringLiteral("offscreen"));
    if (!isOffscreen && !m_demoMode && m_notificationBannerEnabled) {
        QProcess::startDetached(QStringLiteral("notify-send"),
                                {QStringLiteral("-a"), QStringLiteral("BluePill"),
                                 QStringLiteral("-i"), QStringLiteral("messaging-im"),
                                 QStringLiteral("-u"), QStringLiteral("normal"),
                                 sender, preview});
    }
    if (m_notificationSoundEnabled) {
        playNotificationSound();
    }
}

void Backend::playNotificationSound(const QString &presetOverride, bool forcePreview) {
    if (!forcePreview && (!areOmarchyNotificationsEnabled() || !m_notificationSoundEnabled)) return;
    const QString chosenPreset = presetOverride.trimmed().isEmpty()
                                     ? m_notificationSoundPreset
                                     : presetOverride.trimmed().toLower();
    const QString soundPath = ensureNotificationSoundFile(chosenPreset);
    m_soundPlayCountForTest++;
    m_lastSoundPresetPlayed = chosenPreset;

    const bool isOffscreen = (QGuiApplication::platformName() == QStringLiteral("offscreen"));
    if (isOffscreen || m_demoMode) {
        return;
    }

    if (!soundPath.isEmpty()) {
        const QString effectiveOut = effectiveAudioOutputDevice();
        QStringList pwArgs;
        if (!effectiveOut.isEmpty()) {
            pwArgs << QStringLiteral("--target") << effectiveOut;
        }
        pwArgs << soundPath;
        if (!QProcess::startDetached(QStringLiteral("pw-play"), pwArgs)) {
            QStringList paArgs;
            if (!effectiveOut.isEmpty()) {
                paArgs << (QStringLiteral("--device=") + effectiveOut);
            }
            paArgs << soundPath;
            QProcess::startDetached(QStringLiteral("paplay"), paArgs);
        }
    }
}

QVariantList Backend::parsePactlDeviceList(const QByteArray &rawOutput, bool filterMonitorSources) {
    QVariantList result;
    const QList<QByteArray> lines = rawOutput.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty()) continue;
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;
        const QString devName = parts.at(1).trimmed();
        if (devName.isEmpty()) continue;
        if (filterMonitorSources && devName.endsWith(QStringLiteral(".monitor"), Qt::CaseInsensitive)) {
            continue;
        }
        QString label = devName;
        if (label.startsWith(QStringLiteral("alsa_input."))) {
            label = label.mid(11);
        } else if (label.startsWith(QStringLiteral("alsa_output."))) {
            label = label.mid(12);
        }
        label.replace(QLatin1Char('_'), QLatin1Char(' '));
        label.replace(QLatin1Char('.'), QStringLiteral(" · "));
        result.append(QVariantMap{
            {QStringLiteral("id"), devName},
            {QStringLiteral("label"), label}
        });
    }
    return result;
}

void Backend::refreshAudioDevices() {
    QVariantList inputs;
    QVariantList outputs;
    inputs.append(QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("label"), QStringLiteral("System Default")}});
    outputs.append(QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("label"), QStringLiteral("System Default")}});
    bool inputDiscoveryOk = false;
    bool outputDiscoveryOk = false;

#ifndef IMSG_UNIT_TEST
    if (QGuiApplication::platformName() != QStringLiteral("offscreen") &&
        !QStandardPaths::findExecutable(QStringLiteral("pactl")).isEmpty()) {
        QProcess inProc;
        inProc.start(QStringLiteral("pactl"), {QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sources")});
        if (inProc.waitForFinished(1000) && inProc.exitStatus() == QProcess::NormalExit && inProc.exitCode() == 0) {
            inputDiscoveryOk = true;
            const QVariantList parsedIn = parsePactlDeviceList(inProc.readAllStandardOutput(), true);
            for (const QVariant &v : parsedIn) inputs.append(v);
        }
        QProcess outProc;
        outProc.start(QStringLiteral("pactl"), {QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")});
        if (outProc.waitForFinished(1000) && outProc.exitStatus() == QProcess::NormalExit && outProc.exitCode() == 0) {
            outputDiscoveryOk = true;
            const QVariantList parsedOut = parsePactlDeviceList(outProc.readAllStandardOutput(), false);
            for (const QVariant &v : parsedOut) outputs.append(v);
        }
    }
#endif

    m_audioInputDevices = inputs;
    m_audioOutputDevices = outputs;
    m_audioInputDiscoverySucceeded = inputDiscoveryOk;
    m_audioOutputDiscoverySucceeded = outputDiscoveryOk;
    emit audioDevicesChanged();
}

QString Backend::effectiveAudioInputDevice() const {
    if (m_selectedAudioInputDevice.isEmpty() || !m_audioInputDiscoverySucceeded) {
        return QString();
    }
    for (const QVariant &v : m_audioInputDevices) {
        if (v.toMap().value(QStringLiteral("id")).toString() == m_selectedAudioInputDevice) {
            return m_selectedAudioInputDevice;
        }
    }
    return QString();
}

QString Backend::effectiveAudioOutputDevice() const {
    if (m_selectedAudioOutputDevice.isEmpty() || !m_audioOutputDiscoverySucceeded) {
        return QString();
    }
    for (const QVariant &v : m_audioOutputDevices) {
        if (v.toMap().value(QStringLiteral("id")).toString() == m_selectedAudioOutputDevice) {
            return m_selectedAudioOutputDevice;
        }
    }
    return QString();
}

void Backend::setSelectedAudioInputDevice(const QString &deviceId) {
    const QString norm = deviceId.trimmed();
    if (m_selectedAudioInputDevice == norm) return;
    m_selectedAudioInputDevice = norm;
    saveAudioDeviceConfig();
    emit audioDevicesChanged();
}

void Backend::setSelectedAudioOutputDevice(const QString &deviceId) {
    const QString norm = deviceId.trimmed();
    if (m_selectedAudioOutputDevice == norm) return;
    m_selectedAudioOutputDevice = norm;
    saveAudioDeviceConfig();
    emit audioDevicesChanged();
}

void Backend::saveAudioDeviceConfig() {
    const QString cfgPath = configFilePath();
    if (cfgPath.isEmpty()) return;
    QDir().mkpath(QFileInfo(cfgPath).absolutePath());
    QJsonObject cfgObj;
    QFile existingFile(cfgPath);
    if (existingFile.exists() && existingFile.open(QIODevice::ReadOnly)) {
        cfgObj = QJsonDocument::fromJson(existingFile.readAll()).object();
        existingFile.close();
    }
    cfgObj.insert(QStringLiteral("audioInputDevice"), m_selectedAudioInputDevice);
    cfgObj.insert(QStringLiteral("audioOutputDevice"), m_selectedAudioOutputDevice);
    if (!writeConfigAtomically(cfgPath, cfgObj)) {
        emit toastMessage(QStringLiteral("⚠ Failed to save audio device configuration"));
    }
}

void Backend::loadDemoData() {
    m_demoMode = true;
    m_pollTimer.stop();
    stopSocketIoClient();
    m_connected = true;
    m_loading = false;
    if (m_serverUrl != QStringLiteral("https://bluepill.example.com")) {
        m_serverUrl = QStringLiteral("https://bluepill.example.com");
        bumpServerEndpointGeneration();
    }
    m_secondaryServerUrl = QStringLiteral("https://failover.example.com");
    m_serverStatus = QStringLiteral("BluePill Relay v1.1.0 · macOS 15.7.7 · Private API");

    // Generate 3 synthetic scenic photos in /tmp (mountain lake, alpine ridge, portrait forest trail)
    const QString lakeImgPath = QDir::tempPath() + QStringLiteral("/imsg_demo_mountain_lake.png");
    {
        QImage img(480, 225, QImage::Format_ARGB32);
        QLinearGradient sky(0, 0, 0, 140);
        sky.setColorAt(0.0, QColor(QStringLiteral("#0f172a")));
        sky.setColorAt(0.6, QColor(QStringLiteral("#1e3a5f")));
        sky.setColorAt(1.0, QColor(QStringLiteral("#38bdf8")));
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(img.rect(), sky);

        // Mountain peaks
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(QStringLiteral("#334155")));
        QPolygon farPeaks;
        farPeaks << QPoint(0, 140) << QPoint(90, 58) << QPoint(185, 118) << QPoint(290, 42)
                 << QPoint(390, 105) << QPoint(450, 68) << QPoint(480, 140);
        p.drawPolygon(farPeaks);

        // Snow caps
        p.setBrush(QColor(QStringLiteral("#e2e8f0")));
        QPolygon cap1;
        cap1 << QPoint(290, 42) << QPoint(262, 74) << QPoint(282, 68) << QPoint(298, 76) << QPoint(322, 72);
        p.drawPolygon(cap1);

        // Emerald alpine lake
        QLinearGradient lake(0, 140, 0, 225);
        lake.setColorAt(0.0, QColor(QStringLiteral("#0284c7")));
        lake.setColorAt(0.5, QColor(QStringLiteral("#0f766e")));
        lake.setColorAt(1.0, QColor(QStringLiteral("#0f172a")));
        p.fillRect(0, 140, 480, 85, lake);

        // Caption pill
        p.setBrush(QColor(15, 23, 42, 195));
        p.setPen(QPen(QColor(QStringLiteral("#7aa2f7")), 1));
        p.drawRoundedRect(14, 182, 236, 28, 7, 7);
        p.setPen(QColor(QStringLiteral("#f8fafc")));
        QFont f(QStringLiteral("Sans Serif"), 9, QFont::Bold);
        p.setFont(f);
        p.drawText(26, 200, QStringLiteral("Sierra High Camp · Alpine Lake"));
        p.end();
        img.save(lakeImgPath);
    }

    const QString ridgeImgPath = QDir::tempPath() + QStringLiteral("/imsg_demo_alpine_ridge.png");
    {
        QImage img(480, 225, QImage::Format_ARGB32);
        QLinearGradient sunset(0, 0, 0, 225);
        sunset.setColorAt(0.0, QColor(QStringLiteral("#1e1b4b")));
        sunset.setColorAt(0.45, QColor(QStringLiteral("#7c2d12")));
        sunset.setColorAt(0.75, QColor(QStringLiteral("#f59e0b")));
        sunset.setColorAt(1.0, QColor(QStringLiteral("#111827")));
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(img.rect(), sunset);

        // Glowing sun disc
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(254, 240, 138, 210));
        p.drawEllipse(QPoint(340, 92), 32, 32);

        // Layered granite ridges
        p.setBrush(QColor(QStringLiteral("#1f2937")));
        QPolygon ridge1;
        ridge1 << QPoint(0, 225) << QPoint(0, 145) << QPoint(110, 95) << QPoint(230, 138)
               << QPoint(350, 82) << QPoint(480, 132) << QPoint(480, 225);
        p.drawPolygon(ridge1);

        p.setBrush(QColor(15, 23, 42, 195));
        p.setPen(QPen(QColor(QStringLiteral("#e0af68")), 1));
        p.drawRoundedRect(14, 182, 236, 28, 7, 7);
        p.setPen(QColor(QStringLiteral("#f8fafc")));
        QFont f(QStringLiteral("Sans Serif"), 9, QFont::Bold);
        p.setFont(f);
        p.drawText(26, 200, QStringLiteral("Granite Ridge · Golden Hour"));
        p.end();
        img.save(ridgeImgPath);
    }

    const QString trailImgPath = QDir::tempPath() + QStringLiteral("/imsg_demo_forest_trail.png");
    {
        // Portrait aspect ratio (height > width) to exercise portrait card sizing
        QImage img(240, 242, QImage::Format_ARGB32);
        QLinearGradient nightSky(0, 0, 0, 340);
        nightSky.setColorAt(0.0, QColor(QStringLiteral("#090d16")));
        nightSky.setColorAt(0.55, QColor(QStringLiteral("#1e293b")));
        nightSky.setColorAt(1.0, QColor(QStringLiteral("#064e3b")));
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(img.rect(), nightSky);

        // Pine silhouettes
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(QStringLiteral("#065f46")));
        for (int x = 25; x < 300; x += 55) {
            QPolygon tree;
            tree << QPoint(x, 110) << QPoint(x - 28, 260) << QPoint(x + 28, 260);
            p.drawPolygon(tree);
        }

        // Winding trail
        p.setBrush(QColor(QStringLiteral("#334155")));
        QPolygon trail;
        trail << QPoint(145, 210) << QPoint(175, 210) << QPoint(230, 340) << QPoint(90, 340);
        p.drawPolygon(trail);

        p.setBrush(QColor(15, 23, 42, 195));
        p.setPen(QPen(QColor(QStringLiteral("#9ece6a")), 1));
        p.drawRoundedRect(12, 202, 216, 28, 7, 7);
        p.setPen(QColor(QStringLiteral("#f8fafc")));
        QFont f(QStringLiteral("Sans Serif"), 9, QFont::Bold);
        p.setFont(f);
        p.drawText(22, 220, QStringLiteral("Pine Creek Ascent · Portrait"));
        p.end();
        img.save(trailImgPath);
    }

    m_attachmentPaths.insert(QStringLiteral("demo-att-1"), lakeImgPath);
    m_attachmentPaths.insert(QStringLiteral("demo-att-2"), ridgeImgPath);
    m_attachmentPaths.insert(QStringLiteral("demo-att-3"), trailImgPath);

    // Seed fictional demo contacts for autocomplete & handle resolution
    auto seedDemoContact = [this](const QString &name, const QString &addr) {
        const QString norm = normalizeHandle(addr);
        if (!norm.isEmpty()) {
            m_contactNames.insert(norm, name);
            m_contactRawAddresses.insert(norm, canonicalizeAddress(addr));
        }
    };
    seedDemoContact(QStringLiteral("Alex Rivera"), QStringLiteral("+14155550142"));
    seedDemoContact(QStringLiteral("Alex Mercer"), QStringLiteral("alex.mercer@example.com"));
    seedDemoContact(QStringLiteral("Elena Vance"), QStringLiteral("+14155550188"));
    seedDemoContact(QStringLiteral("Elena Vance"), QStringLiteral("elena.vance@example.com"));
    seedDemoContact(QStringLiteral("Marcus Chen"), QStringLiteral("+14155550199"));
    seedDemoContact(QStringLiteral("Jordan Taylor"), QStringLiteral("+14155550101"));
    seedDemoContact(QStringLiteral("Sam Patel"), QStringLiteral("+14155550102"));
    emit contactsChanged();

    m_chats.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    ChatEntry c1;
    c1.guid = QStringLiteral("iMessage;-;+14155550142");
    c1.chatIdentifier = QStringLiteral("+14155550142");
    c1.title = QStringLiteral("Alex Rivera");
    c1.subtitle = QStringLiteral("+1 (415) 555-0142");
    c1.service = QStringLiteral("iMessage");
    c1.initials = QStringLiteral("AR");
    c1.avatarColor = QStringLiteral("#7aa2f7");
    c1.lastMessagePreview = QStringLiteral("BluePill looks super clean on Hyprland! Your code is G-482910.");
    c1.relativeTime = QStringLiteral("now");
    c1.lastMessageDate = now - 30000;
    c1.unreadCount = 0;
    c1.isGroup = false;
    m_chats.append(c1);

    ChatEntry c2;
    c2.guid = QStringLiteral("iMessage;+;chat555000111222333444");
    c2.chatIdentifier = QStringLiteral("chat555000111222333444");
    c2.title = QStringLiteral("Alpine Trail Crew");
    c2.subtitle = QStringLiteral("3 participants");
    c2.service = QStringLiteral("iMessage");
    c2.initials = QStringLiteral("AT");
    c2.avatarColor = QStringLiteral("#bb9af7");
    c2.lastMessagePreview = QStringLiteral("Marcus: Trailhead weather looks clear for Saturday morning");
    c2.relativeTime = QStringLiteral("4m");
    c2.lastMessageDate = now - 240000;
    c2.unreadCount = 2;
    c2.isGroup = true;
    c2.participantHandles = QStringList{
        QStringLiteral("+14155550142"),
        QStringLiteral("+14155550188"),
        QStringLiteral("+14155550199")
    };
    m_chats.append(c2);

    ChatEntry c3;
    c3.guid = QStringLiteral("iMessage;-;elena.vance@example.com");
    c3.chatIdentifier = QStringLiteral("elena.vance@example.com");
    c3.title = QStringLiteral("Elena Vance");
    c3.subtitle = QStringLiteral("elena.vance@example.com");
    c3.service = QStringLiteral("iMessage");
    c3.initials = QStringLiteral("EV");
    c3.avatarColor = QStringLiteral("#e0af68");
    c3.lastMessagePreview = QStringLiteral("Shared a 4K Sierra timelapse video on YouTube");
    c3.relativeTime = QStringLiteral("28m");
    c3.lastMessageDate = now - 1680000;
    c3.unreadCount = 0;
    c3.isGroup = false;
    m_chats.append(c3);

    ChatEntry c4;
    c4.guid = QStringLiteral("SMS;-;22000");
    c4.chatIdentifier = QStringLiteral("22000");
    c4.title = QStringLiteral("GitHub");
    c4.subtitle = QStringLiteral("22000 · Short Code SMS");
    c4.service = QStringLiteral("SMS");
    c4.initials = QStringLiteral("GH");
    c4.avatarColor = QStringLiteral("#9ece6a");
    c4.lastMessagePreview = QStringLiteral("G-482910 is your verification code. Don't share this code.");
    c4.relativeTime = QStringLiteral("1h");
    c4.lastMessageDate = now - 3600000;
    c4.unreadCount = 1;
    c4.isGroup = false;
    m_chats.append(c4);

    m_selectedChatGuid = c1.guid;

    QVariantList demoMsgs;
    {
        QVariantMap m1;
        m1.insert(QStringLiteral("guid"), QStringLiteral("msg-1"));
        m1.insert(QStringLiteral("text"), QStringLiteral("Just got back from Sierra High Camp! Here are 3 shots from the lake, ridge, and pine ascent:"));
        m1.insert(QStringLiteral("isFromMe"), false);
        m1.insert(QStringLiteral("isIMessage"), true);
        m1.insert(QStringLiteral("service"), QStringLiteral("iMessage"));
        m1.insert(QStringLiteral("senderName"), QStringLiteral("Alex Rivera"));
        m1.insert(QStringLiteral("senderColor"), QStringLiteral("#7aa2f7"));
        m1.insert(QStringLiteral("timeText"), QStringLiteral("2:14 PM"));
        m1.insert(QStringLiteral("showDateHeader"), true);
        m1.insert(QStringLiteral("dateHeaderText"), QStringLiteral("Today · 2:14 PM"));
        m1.insert(QStringLiteral("reactions"), QVariantList{
            QVariantMap{
                {QStringLiteral("emoji"), QStringLiteral("❤️")},
                {QStringLiteral("count"), 3},
                {QStringLiteral("fromMe"), true},
                {QStringLiteral("senders"), QStringLiteral("Alex Rivera, Elena Vance, You")},
                {QStringLiteral("tooltipText"), QStringLiteral("❤️ Alex Rivera, Elena Vance, You")}
            },
            QVariantMap{
                {QStringLiteral("emoji"), QStringLiteral("👍")},
                {QStringLiteral("count"), 2},
                {QStringLiteral("fromMe"), true},
                {QStringLiteral("senders"), QStringLiteral("Alex Rivera, You")},
                {QStringLiteral("tooltipText"), QStringLiteral("👍 Alex Rivera, You")}
            }
        });
        m1.insert(QStringLiteral("hasImageAttachment"), true);
        m1.insert(QStringLiteral("hasFileAttachment"), false);
        m1.insert(QStringLiteral("attachmentGuid"), QStringLiteral("demo-att-1"));
        m1.insert(QStringLiteral("attachmentName"), QStringLiteral("sierra_mountain_lake.png"));
        m1.insert(QStringLiteral("attachmentLocalUrl"), QUrl::fromLocalFile(lakeImgPath).toString());
        m1.insert(QStringLiteral("imageAttachments"), QVariantList{
            QVariantMap{
                {QStringLiteral("guid"), QStringLiteral("demo-att-1")},
                {QStringLiteral("name"), QStringLiteral("sierra_mountain_lake.png")},
                {QStringLiteral("mimeType"), QStringLiteral("image/png")},
                {QStringLiteral("localUrl"), QUrl::fromLocalFile(lakeImgPath).toString()},
                {QStringLiteral("isImage"), true}
            },
            QVariantMap{
                {QStringLiteral("guid"), QStringLiteral("demo-att-2")},
                {QStringLiteral("name"), QStringLiteral("granite_alpine_ridge.png")},
                {QStringLiteral("mimeType"), QStringLiteral("image/png")},
                {QStringLiteral("localUrl"), QUrl::fromLocalFile(ridgeImgPath).toString()},
                {QStringLiteral("isImage"), true}
            },
            QVariantMap{
                {QStringLiteral("guid"), QStringLiteral("demo-att-3")},
                {QStringLiteral("name"), QStringLiteral("pine_creek_portrait.png")},
                {QStringLiteral("mimeType"), QStringLiteral("image/png")},
                {QStringLiteral("localUrl"), QUrl::fromLocalFile(trailImgPath).toString()},
                {QStringLiteral("isImage"), true}
            }
        });
        m1.insert(QStringLiteral("imageAttachmentCount"), 3);
        m1.insert(QStringLiteral("receiptText"), QString());
        demoMsgs.append(m1);

        QVariantMap m2;
        m2.insert(QStringLiteral("guid"), QStringLiteral("msg-2"));
        m2.insert(QStringLiteral("text"), QStringLiteral("Incredible shots! The multi-photo gallery and EXIF portrait rendering work seamlessly in https://github.com/nertwork/bluepill"));
        m2.insert(QStringLiteral("formattedText"), Backend::formatMessageTextWithLinks(
            QStringLiteral("Incredible shots! The multi-photo gallery and EXIF portrait rendering work seamlessly in https://github.com/nertwork/bluepill")));
        m2.insert(QStringLiteral("isEdited"), true);
        m2.insert(QStringLiteral("metaBadgeText"), QStringLiteral("2:15 PM · Edited"));
        m2.insert(QStringLiteral("isFromMe"), true);
        m2.insert(QStringLiteral("isIMessage"), true);
        m2.insert(QStringLiteral("service"), QStringLiteral("iMessage"));
        m2.insert(QStringLiteral("senderName"), QStringLiteral("You"));
        m2.insert(QStringLiteral("senderColor"), QStringLiteral("#7aa2f7"));
        m2.insert(QStringLiteral("timeText"), QStringLiteral("2:15 PM"));
        m2.insert(QStringLiteral("showDateHeader"), false);
        m2.insert(QStringLiteral("reactions"), QVariantList());
        m2.insert(QStringLiteral("hasImageAttachment"), false);
        m2.insert(QStringLiteral("hasFileAttachment"), false);
        m2.insert(QStringLiteral("hasLinkPreview"), true);
        m2.insert(QStringLiteral("linkPreview"), QVariantMap{
            {QStringLiteral("url"), QStringLiteral("https://github.com/nertwork/bluepill")},
            {QStringLiteral("domain"), QStringLiteral("github.com")},
            {QStringLiteral("title"), QStringLiteral("nertwork/bluepill — BluePill Native Linux Qt6/QML iMessage, RCS & SMS Client & macOS Relay")},
            {QStringLiteral("description"), QStringLiteral("Fast C++20 and Qt6/QML desktop & CLI client backed by BlueBubbles Private API and Socket.IO v4 push.")},
            {QStringLiteral("thumbnail"), QString()},
            {QStringLiteral("isYouTube"), false}
        });
        m2.insert(QStringLiteral("receiptText"), QString());
        demoMsgs.append(m2);

        QVariantMap m3;
        m3.insert(QStringLiteral("guid"), QStringLiteral("msg-3"));
        m3.insert(QStringLiteral("text"), QStringLiteral("BluePill looks super clean on Hyprland! Your verification code is G-482910."));
        m3.insert(QStringLiteral("verificationCode"), QStringLiteral("482910"));
        m3.insert(QStringLiteral("otpCode"), QStringLiteral("482910"));
        m3.insert(QStringLiteral("sendEffectLabel"), QStringLiteral("sent with confetti"));
        m3.insert(QStringLiteral("metaBadgeText"), QStringLiteral("2:16 PM · sent with confetti"));
        m3.insert(QStringLiteral("isFromMe"), false);
        m3.insert(QStringLiteral("isIMessage"), true);
        m3.insert(QStringLiteral("service"), QStringLiteral("iMessage"));
        m3.insert(QStringLiteral("senderName"), QStringLiteral("Alex Rivera"));
        m3.insert(QStringLiteral("senderColor"), QStringLiteral("#7aa2f7"));
        m3.insert(QStringLiteral("timeText"), QStringLiteral("2:16 PM"));
        m3.insert(QStringLiteral("showDateHeader"), false);
        m3.insert(QStringLiteral("reactions"), QVariantList{
            QVariantMap{
                {QStringLiteral("emoji"), QStringLiteral("😂")},
                {QStringLiteral("count"), 1},
                {QStringLiteral("fromMe"), true},
                {QStringLiteral("senders"), QStringLiteral("You")},
                {QStringLiteral("tooltipText"), QStringLiteral("😂 You")}
            }
        });
        m3.insert(QStringLiteral("hasImageAttachment"), false);
        m3.insert(QStringLiteral("hasFileAttachment"), false);
        m3.insert(QStringLiteral("receiptText"), QString());
        m3.insert(QStringLiteral("replyToGuid"), QStringLiteral("msg-2"));
        m3.insert(QStringLiteral("replyToSender"), QStringLiteral("You"));
        m3.insert(QStringLiteral("replyToText"), QStringLiteral("Incredible shots! The multi-photo gallery and EXIF portrait rendering..."));
        demoMsgs.append(m3);

        QVariantMap m4;
        m4.insert(QStringLiteral("guid"), QStringLiteral("msg-4"));
        m4.insert(QStringLiteral("text"), QStringLiteral("Sub-10ms Socket.IO push, 0ms local cache startup, and live Omarchy theme hot-reload!"));
        m4.insert(QStringLiteral("isFromMe"), true);
        m4.insert(QStringLiteral("isIMessage"), true);
        m4.insert(QStringLiteral("service"), QStringLiteral("iMessage"));
        m4.insert(QStringLiteral("senderName"), QStringLiteral("You"));
        m4.insert(QStringLiteral("senderColor"), QStringLiteral("#7aa2f7"));
        m4.insert(QStringLiteral("timeText"), QStringLiteral("2:17 PM"));
        m4.insert(QStringLiteral("showDateHeader"), false);
        m4.insert(QStringLiteral("reactions"), QVariantList());
        m4.insert(QStringLiteral("hasImageAttachment"), false);
        m4.insert(QStringLiteral("hasFileAttachment"), false);
        m4.insert(QStringLiteral("receiptText"), QStringLiteral("Read 2:17 PM"));
        demoMsgs.append(m4);
    }
    m_demoChatMessages.clear();
    m_demoChatMessages.insert(c1.guid, demoMsgs);
    m_messages = demoMsgs;
    // Demo "Mac" pins so the pinned row isn't empty
    m_macPins.clear();
    for (int i = 0; i < qMin<int>(3, m_chats.size()); ++i) {
        m_macPins.append(QVariantMap{{QStringLiteral("guid"), m_chats.at(i).guid}, {QStringLiteral("pinIndex"), i}});
    }

    emit configChanged();
    emit statusChanged();
    emit chatsChanged();
    emit selectedChatChanged();
    emit messagesChanged();
}

int Backend::runCliStatus(bool asJson) {
    if (m_serverUrl.isEmpty() || m_password.isEmpty()) {
        std::cerr << "{\"error\":\"Missing BlueBubbles credentials\"}\n";
        return 1;
    }
    QEventLoop loop;
    QNetworkReply *reply = m_nam.get(makeRequest(QStringLiteral("/api/v1/server/info")));
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        QJsonObject err{{QStringLiteral("status"), QStringLiteral("error")},
                        {QStringLiteral("message"), reply->errorString()}};
        std::cout << QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString() << "\n";
        reply->deleteLater();
        return 1;
    }
    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    reply->deleteLater();

    if (asJson) {
        QJsonObject out;
        out.insert(QStringLiteral("status"), QStringLiteral("connected"));
        out.insert(QStringLiteral("serverUrl"), m_serverUrl);
        out.insert(QStringLiteral("authMethod"), QStringLiteral("tls-apikey"));
        out.insert(QStringLiteral("tlsVerified"), true);
        out.insert(QStringLiteral("serverVersion"), data.value(QStringLiteral("server_version")));
        out.insert(QStringLiteral("osVersion"), data.value(QStringLiteral("os_version")));
        out.insert(QStringLiteral("privateApi"), data.value(QStringLiteral("private_api")));
        out.insert(QStringLiteral("helperConnected"), data.value(QStringLiteral("helper_connected")));
        out.insert(QStringLiteral("detectedImessage"), data.value(QStringLiteral("detected_imessage")));
        std::cout << QJsonDocument(out).toJson(QJsonDocument::Compact).toStdString() << "\n";
    } else {
        std::cout << "Connected to " << m_serverUrl.toStdString()
                  << " (TLS verified, v" << data.value(QStringLiteral("server_version")).toString().toStdString()
                  << ", macOS " << data.value(QStringLiteral("os_version")).toString().toStdString() << ")\n";
    }
    return 0;
}

int Backend::runCliRecent(int limit, bool asJson) {
    loadContactsFromFile(QDir::homePath() + QStringLiteral("/.config/imsg/contacts.json"));
    loadContactsFromFile(QDir::homePath() + QStringLiteral("/.cache/bluebubbles/contacts.json"));
    loadContactsFromFile(QDir::homePath() + QStringLiteral("/.local/share/app.bluebubbles.BlueBubbles/contacts.json"));

    const qint64 afterMs = QDateTime::currentMSecsSinceEpoch() - (14LL * 86400LL * 1000LL);
    QJsonObject body;
    body.insert(QStringLiteral("limit"), std::max(limit * 4, 40));
    body.insert(QStringLiteral("offset"), 0);
    body.insert(QStringLiteral("after"), afterMs);
    body.insert(QStringLiteral("sort"), QStringLiteral("DESC"));
    body.insert(QStringLiteral("with"), QJsonArray{
        QStringLiteral("chat"),
        QStringLiteral("handle"),
        QStringLiteral("attachment"),
        QStringLiteral("attributedbody")
    });

    QEventLoop loop;
    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/query")),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        std::cerr << "{\"error\":\"" << reply->errorString().toStdString() << "\"}\n";
        reply->deleteLater();
        return 1;
    }
    const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
    reply->deleteLater();
    processIncomingMessages(arr, true);

    QJsonArray outArr;
    const int count = std::min<int>(limit, m_chats.size());
    for (int i = 0; i < count; ++i) {
        const ChatEntry &c = m_chats[i];
        QJsonObject o;
        o.insert(QStringLiteral("guid"), c.guid);
        o.insert(QStringLiteral("title"), c.title);
        o.insert(QStringLiteral("handle"), c.subtitle);
        o.insert(QStringLiteral("service"), c.service);
        o.insert(QStringLiteral("lastMessage"), c.lastMessagePreview);
        o.insert(QStringLiteral("relativeTime"), c.relativeTime);
        outArr.append(o);
    }

    if (asJson) {
        std::cout << QJsonDocument(outArr).toJson(QJsonDocument::Compact).toStdString() << "\n";
    } else {
        for (const QJsonValue &v : outArr) {
            const QJsonObject o = v.toObject();
            std::cout << "[" << o.value(QStringLiteral("relativeTime")).toString().toStdString() << "] "
                      << o.value(QStringLiteral("title")).toString().toStdString() << " ("
                      << o.value(QStringLiteral("guid")).toString().toStdString() << "): "
                      << o.value(QStringLiteral("lastMessage")).toString().toStdString() << "\n";
        }
    }
    return 0;
}

int Backend::runCliSend(const QString &target, const QString &message, bool asJson) {
    QString chatGuid = target.trimmed();
    if (!chatGuid.contains(QStringLiteral(";-;")) && !chatGuid.contains(QStringLiteral(";+;"))) {
        chatGuid = QStringLiteral("iMessage;-;") + chatGuid;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("chatGuid"), chatGuid);
    payload.insert(QStringLiteral("tempGuid"), QStringLiteral("temp-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    payload.insert(QStringLiteral("message"), message);
    payload.insert(QStringLiteral("method"), QStringLiteral("private-api"));

    QEventLoop loop;
    QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/message/text")),
                                      QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray respBytes = reply->readAll();
    reply->deleteLater();

    if (asJson) {
        std::cout << respBytes.toStdString() << "\n";
    } else {
        std::cout << "HTTP " << status << ": " << respBytes.toStdString() << "\n";
    }
    return (status >= 200 && status < 300) ? 0 : 1;
}

namespace {
const QList<EmojiSpec> &emojiCatalog() {
    static const QList<EmojiSpec> catalog = {
        // Smileys
        {QStringLiteral("smile"), QStringLiteral("😄"), QStringLiteral("Smileys"), QStringLiteral("happy joy grin smile")},
        {QStringLiteral("grinning"), QStringLiteral("😀"), QStringLiteral("Smileys"), QStringLiteral("happy smile face")},
        {QStringLiteral("laughing"), QStringLiteral("😆"), QStringLiteral("Smileys"), QStringLiteral("happy lol laugh")},
        {QStringLiteral("joy"), QStringLiteral("😂"), QStringLiteral("Smileys"), QStringLiteral("tears laugh lol cry")},
        {QStringLiteral("rofl"), QStringLiteral("🤣"), QStringLiteral("Smileys"), QStringLiteral("rolling floor laughing lol")},
        {QStringLiteral("smiley"), QStringLiteral("😃"), QStringLiteral("Smileys"), QStringLiteral("happy smile")},
        {QStringLiteral("sweat_smile"), QStringLiteral("😅"), QStringLiteral("Smileys"), QStringLiteral("relief laugh sweat")},
        {QStringLiteral("wink"), QStringLiteral("😉"), QStringLiteral("Smileys"), QStringLiteral("wink flirt")},
        {QStringLiteral("blush"), QStringLiteral("😊"), QStringLiteral("Smileys"), QStringLiteral("smile happy warm")},
        {QStringLiteral("innocent"), QStringLiteral("😇"), QStringLiteral("Smileys"), QStringLiteral("angel halo")},
        {QStringLiteral("slightly_smiling_face"), QStringLiteral("🙂"), QStringLiteral("Smileys"), QStringLiteral("smile")},
        {QStringLiteral("upside_down_face"), QStringLiteral("🙃"), QStringLiteral("Smileys"), QStringLiteral("silly sarcasm")},
        {QStringLiteral("relaxed"), QStringLiteral("☺️"), QStringLiteral("Smileys"), QStringLiteral("blush cozy")},
        {QStringLiteral("yum"), QStringLiteral("😋"), QStringLiteral("Smileys"), QStringLiteral("delicious food tongue")},
        {QStringLiteral("relieved"), QStringLiteral("😌"), QStringLiteral("Smileys"), QStringLiteral("calm peace")},
        {QStringLiteral("heart_eyes"), QStringLiteral("😍"), QStringLiteral("Smileys"), QStringLiteral("love crush adore")},
        {QStringLiteral("smiling_face_with_three_hearts"), QStringLiteral("🥰"), QStringLiteral("Smileys"), QStringLiteral("love hearts affectionate")},
        {QStringLiteral("kissing_heart"), QStringLiteral("😘"), QStringLiteral("Smileys"), QStringLiteral("kiss love")},
        {QStringLiteral("stuck_out_tongue_winking_eye"), QStringLiteral("😜"), QStringLiteral("Smileys"), QStringLiteral("playful joke wink")},
        {QStringLiteral("zany_face"), QStringLiteral("🤪"), QStringLiteral("Smileys"), QStringLiteral("crazy goofy")},
        {QStringLiteral("sunglasses"), QStringLiteral("😎"), QStringLiteral("Smileys"), QStringLiteral("cool shades")},
        {QStringLiteral("star_struck"), QStringLiteral("🤩"), QStringLiteral("Smileys"), QStringLiteral("excited amazed stars")},
        {QStringLiteral("partying_face"), QStringLiteral("🥳"), QStringLiteral("Smileys"), QStringLiteral("party celebrate birthday")},
        {QStringLiteral("smirk"), QStringLiteral("😏"), QStringLiteral("Smileys"), QStringLiteral("sly smug")},
        {QStringLiteral("unamused"), QStringLiteral("😒"), QStringLiteral("Smileys"), QStringLiteral("meh annoyed")},
        {QStringLiteral("thinking"), QStringLiteral("🤔"), QStringLiteral("Smileys"), QStringLiteral("hmm ponder think")},
        {QStringLiteral("shushing_face"), QStringLiteral("🤫"), QStringLiteral("Smileys"), QStringLiteral("quiet secret shh")},
        {QStringLiteral("hand_over_mouth"), QStringLiteral("🤭"), QStringLiteral("Smileys"), QStringLiteral("oops giggle")},
        {QStringLiteral("rolling_eyes"), QStringLiteral("🙄"), QStringLiteral("Smileys"), QStringLiteral("eyeroll whatever")},
        {QStringLiteral("grimacing"), QStringLiteral("😬"), QStringLiteral("Smileys"), QStringLiteral("awkward yikes")},
        {QStringLiteral("pensive"), QStringLiteral("😔"), QStringLiteral("Smileys"), QStringLiteral("sad down")},
        {QStringLiteral("sleepy"), QStringLiteral("😪"), QStringLiteral("Smileys"), QStringLiteral("tired")},
        {QStringLiteral("sleeping"), QStringLiteral("😴"), QStringLiteral("Smileys"), QStringLiteral("zzz sleep tired")},
        {QStringLiteral("mask"), QStringLiteral("😷"), QStringLiteral("Smileys"), QStringLiteral("sick doctor")},
        {QStringLiteral("mind_blown"), QStringLiteral("🤯"), QStringLiteral("Smileys"), QStringLiteral("exploded head shocked")},
        {QStringLiteral("cowboy_hat_face"), QStringLiteral("🤠"), QStringLiteral("Smileys"), QStringLiteral("yeehaw cowboy")},
        {QStringLiteral("nerd_face"), QStringLiteral("🤓"), QStringLiteral("Smileys"), QStringLiteral("geek glasses")},
        {QStringLiteral("confused"), QStringLiteral("😕"), QStringLiteral("Smileys"), QStringLiteral("unsure puzzled")},
        {QStringLiteral("worried"), QStringLiteral("😟"), QStringLiteral("Smileys"), QStringLiteral("concerned")},
        {QStringLiteral("open_mouth"), QStringLiteral("😮"), QStringLiteral("Smileys"), QStringLiteral("surprised wow")},
        {QStringLiteral("flushed"), QStringLiteral("😳"), QStringLiteral("Smileys"), QStringLiteral("embarrassed shocked")},
        {QStringLiteral("pleading_face"), QStringLiteral("🥺"), QStringLiteral("Smileys"), QStringLiteral("puppy eyes please")},
        {QStringLiteral("cry"), QStringLiteral("😢"), QStringLiteral("Smileys"), QStringLiteral("sad tear")},
        {QStringLiteral("sob"), QStringLiteral("😭"), QStringLiteral("Smileys"), QStringLiteral("crying bawling sad")},
        {QStringLiteral("scream"), QStringLiteral("😱"), QStringLiteral("Smileys"), QStringLiteral("shocked scared omg")},
        {QStringLiteral("sweat"), QStringLiteral("😓"), QStringLiteral("Smileys"), QStringLiteral("stressed")},
        {QStringLiteral("angry"), QStringLiteral("😠"), QStringLiteral("Smileys"), QStringLiteral("mad annoyed")},
        {QStringLiteral("rage"), QStringLiteral("😡"), QStringLiteral("Smileys"), QStringLiteral("furious angry red")},
        {QStringLiteral("cursing_face"), QStringLiteral("🤬"), QStringLiteral("Smileys"), QStringLiteral("swearing mad")},
        {QStringLiteral("skull"), QStringLiteral("💀"), QStringLiteral("Smileys"), QStringLiteral("dead lol skeleton")},
        {QStringLiteral("poop"), QStringLiteral("💩"), QStringLiteral("Smileys"), QStringLiteral("hankey poop")},
        {QStringLiteral("clown_face"), QStringLiteral("🤡"), QStringLiteral("Smileys"), QStringLiteral("clown silly")},
        {QStringLiteral("ghost"), QStringLiteral("👻"), QStringLiteral("Smileys"), QStringLiteral("spooky halloween")},
        {QStringLiteral("alien"), QStringLiteral("👽"), QStringLiteral("Smileys"), QStringLiteral("ufo space")},
        {QStringLiteral("robot"), QStringLiteral("🤖"), QStringLiteral("Smileys"), QStringLiteral("bot ai")},

        // Gestures
        {QStringLiteral("thumbsup"), QStringLiteral("👍"), QStringLiteral("Gestures"), QStringLiteral("yes ok like approve +1")},
        {QStringLiteral("+1"), QStringLiteral("👍"), QStringLiteral("Gestures"), QStringLiteral("thumbsup like yes")},
        {QStringLiteral("thumbsdown"), QStringLiteral("👎"), QStringLiteral("Gestures"), QStringLiteral("no dislike -1")},
        {QStringLiteral("-1"), QStringLiteral("👎"), QStringLiteral("Gestures"), QStringLiteral("thumbsdown dislike no")},
        {QStringLiteral("clap"), QStringLiteral("👏"), QStringLiteral("Gestures"), QStringLiteral("applause bravo")},
        {QStringLiteral("raised_hands"), QStringLiteral("🙌"), QStringLiteral("Gestures"), QStringLiteral("hooray celebrate praise")},
        {QStringLiteral("pray"), QStringLiteral("🙏"), QStringLiteral("Gestures"), QStringLiteral("please thank you namaste")},
        {QStringLiteral("handshake"), QStringLiteral("🤝"), QStringLiteral("Gestures"), QStringLiteral("deal agree")},
        {QStringLiteral("wave"), QStringLiteral("👋"), QStringLiteral("Gestures"), QStringLiteral("hello hi bye")},
        {QStringLiteral("ok_hand"), QStringLiteral("👌"), QStringLiteral("Gestures"), QStringLiteral("okay perfect")},
        {QStringLiteral("v"), QStringLiteral("✌️"), QStringLiteral("Gestures"), QStringLiteral("peace victory")},
        {QStringLiteral("crossed_fingers"), QStringLiteral("🤞"), QStringLiteral("Gestures"), QStringLiteral("luck hope")},
        {QStringLiteral("love_you_gesture"), QStringLiteral("🤟"), QStringLiteral("Gestures"), QStringLiteral("ily rock")},
        {QStringLiteral("metal"), QStringLiteral("🤘"), QStringLiteral("Gestures"), QStringLiteral("rock on")},
        {QStringLiteral("call_me_hand"), QStringLiteral("🤙"), QStringLiteral("Gestures"), QStringLiteral("shaka call")},
        {QStringLiteral("point_up"), QStringLiteral("☝️"), QStringLiteral("Gestures"), QStringLiteral("this up")},
        {QStringLiteral("point_down"), QStringLiteral("👇"), QStringLiteral("Gestures"), QStringLiteral("down below")},
        {QStringLiteral("point_left"), QStringLiteral("👈"), QStringLiteral("Gestures"), QStringLiteral("left")},
        {QStringLiteral("point_right"), QStringLiteral("👉"), QStringLiteral("Gestures"), QStringLiteral("right")},
        {QStringLiteral("Open_hands"), QStringLiteral("👐"), QStringLiteral("Gestures"), QStringLiteral("hug open")},
        {QStringLiteral("muscle"), QStringLiteral("💪"), QStringLiteral("Gestures"), QStringLiteral("strong flex bicep")},
        {QStringLiteral("salute"), QStringLiteral("🫡"), QStringLiteral("Gestures"), QStringLiteral("respect yes sir")},
        {QStringLiteral("facepalm"), QStringLiteral("🤦"), QStringLiteral("Gestures"), QStringLiteral("doh sigh")},
        {QStringLiteral("shrug"), QStringLiteral("🤷"), QStringLiteral("Gestures"), QStringLiteral("idk whatever")},
        {QStringLiteral("eyes"), QStringLiteral("👀"), QStringLiteral("Gestures"), QStringLiteral("look watch see")},
        {QStringLiteral("brain"), QStringLiteral("🧠"), QStringLiteral("Gestures"), QStringLiteral("smart mind")},

        // Hearts
        {QStringLiteral("heart"), QStringLiteral("❤️"), QStringLiteral("Hearts"), QStringLiteral("love red heart")},
        {QStringLiteral("red_heart"), QStringLiteral("❤️"), QStringLiteral("Hearts"), QStringLiteral("love red")},
        {QStringLiteral("orange_heart"), QStringLiteral("🧡"), QStringLiteral("Hearts"), QStringLiteral("love orange")},
        {QStringLiteral("yellow_heart"), QStringLiteral("💛"), QStringLiteral("Hearts"), QStringLiteral("love yellow")},
        {QStringLiteral("green_heart"), QStringLiteral("💚"), QStringLiteral("Hearts"), QStringLiteral("love green")},
        {QStringLiteral("blue_heart"), QStringLiteral("💙"), QStringLiteral("Hearts"), QStringLiteral("love blue")},
        {QStringLiteral("purple_heart"), QStringLiteral("💜"), QStringLiteral("Hearts"), QStringLiteral("love purple")},
        {QStringLiteral("black_heart"), QStringLiteral("🖤"), QStringLiteral("Hearts"), QStringLiteral("love black")},
        {QStringLiteral("white_heart"), QStringLiteral("🤍"), QStringLiteral("Hearts"), QStringLiteral("love white")},
        {QStringLiteral("brown_heart"), QStringLiteral("🤎"), QStringLiteral("Hearts"), QStringLiteral("love brown")},
        {QStringLiteral("broken_heart"), QStringLiteral("💔"), QStringLiteral("Hearts"), QStringLiteral("sad heartbreak")},
        {QStringLiteral("two_hearts"), QStringLiteral("💕"), QStringLiteral("Hearts"), QStringLiteral("love hearts")},
        {QStringLiteral("sparkling_heart"), QStringLiteral("💖"), QStringLiteral("Hearts"), QStringLiteral("love sparkle")},
        {QStringLiteral("heartpulse"), QStringLiteral("💗"), QStringLiteral("Hearts"), QStringLiteral("growing heart")},
        {QStringLiteral("heartbeat"), QStringLiteral("💓"), QStringLiteral("Hearts"), QStringLiteral("beating heart")},
        {QStringLiteral("revolving_hearts"), QStringLiteral("💞"), QStringLiteral("Hearts"), QStringLiteral("love")},
        {QStringLiteral("cupid"), QStringLiteral("💘"), QStringLiteral("Hearts"), QStringLiteral("arrow love")},
        {QStringLiteral("kiss"), QStringLiteral("💋"), QStringLiteral("Hearts"), QStringLiteral("lips kiss")},

        // Activities
        {QStringLiteral("tada"), QStringLiteral("🎉"), QStringLiteral("Activities"), QStringLiteral("party popper congrats celebrate")},
        {QStringLiteral("party"), QStringLiteral("🎉"), QStringLiteral("Activities"), QStringLiteral("celebrate tada")},
        {QStringLiteral("confetti_ball"), QStringLiteral("🎊"), QStringLiteral("Activities"), QStringLiteral("party celebrate")},
        {QStringLiteral("balloon"), QStringLiteral("🎈"), QStringLiteral("Activities"), QStringLiteral("birthday party")},
        {QStringLiteral("birthday"), QStringLiteral("🎂"), QStringLiteral("Activities"), QStringLiteral("cake celebration")},
        {QStringLiteral("trophy"), QStringLiteral("🏆"), QStringLiteral("Activities"), QStringLiteral("winner award champion")},
        {QStringLiteral("medal"), QStringLiteral("🥇"), QStringLiteral("Activities"), QStringLiteral("first place gold")},
        {QStringLiteral("fire"), QStringLiteral("🔥"), QStringLiteral("Activities"), QStringLiteral("lit hot flame")},
        {QStringLiteral("sparkles"), QStringLiteral("✨"), QStringLiteral("Activities"), QStringLiteral("magic shine stars")},
        {QStringLiteral("star"), QStringLiteral("⭐"), QStringLiteral("Activities"), QStringLiteral("gold star favorite")},
        {QStringLiteral("star2"), QStringLiteral("🌟"), QStringLiteral("Activities"), QStringLiteral("glowing star")},
        {QStringLiteral("dizzy"), QStringLiteral("💫"), QStringLiteral("Activities"), QStringLiteral("shooting star")},
        {QStringLiteral("boom"), QStringLiteral("💥"), QStringLiteral("Activities"), QStringLiteral("explosion collision")},
        {QStringLiteral("rocket"), QStringLiteral("🚀"), QStringLiteral("Activities"), QStringLiteral("launch ship fast space")},

        // Food
        {QStringLiteral("coffee"), QStringLiteral("☕"), QStringLiteral("Food"), QStringLiteral("cafe espresso morning")},
        {QStringLiteral("tea"), QStringLiteral("🍵"), QStringLiteral("Food"), QStringLiteral("matcha green tea")},
        {QStringLiteral("beer"), QStringLiteral("🍺"), QStringLiteral("Food"), QStringLiteral("drink pub")},
        {QStringLiteral("cheers"), QStringLiteral("🍻"), QStringLiteral("Food"), QStringLiteral("beers toast drink")},
        {QStringLiteral("wine_glass"), QStringLiteral("🍷"), QStringLiteral("Food"), QStringLiteral("vino drink")},
        {QStringLiteral("cocktail"), QStringLiteral("🍸"), QStringLiteral("Food"), QStringLiteral("martini drink")},
        {QStringLiteral("pizza"), QStringLiteral("🍕"), QStringLiteral("Food"), QStringLiteral("food slice")},
        {QStringLiteral("hamburger"), QStringLiteral("🍔"), QStringLiteral("Food"), QStringLiteral("burger food")},
        {QStringLiteral("fries"), QStringLiteral("🍟"), QStringLiteral("Food"), QStringLiteral("food snack")},
        {QStringLiteral("taco"), QStringLiteral("🌮"), QStringLiteral("Food"), QStringLiteral("mexican food")},
        {QStringLiteral("burrito"), QStringLiteral("🌯"), QStringLiteral("Food"), QStringLiteral("food wrap")},
        {QStringLiteral("ramen"), QStringLiteral("🍜"), QStringLiteral("Food"), QStringLiteral("noodles soup")},
        {QStringLiteral("sushi"), QStringLiteral("🍣"), QStringLiteral("Food"), QStringLiteral("japanese food")},
        {QStringLiteral("avocado"), QStringLiteral("🥑"), QStringLiteral("Food"), QStringLiteral("guac fruit")},
        {QStringLiteral("apple"), QStringLiteral("🍎"), QStringLiteral("Food"), QStringLiteral("fruit mac")},

        // Objects
        {QStringLiteral("gift"), QStringLiteral("🎁"), QStringLiteral("Objects"), QStringLiteral("present box")},
        {QStringLiteral("crown"), QStringLiteral("👑"), QStringLiteral("Objects"), QStringLiteral("king queen royal")},
        {QStringLiteral("gem"), QStringLiteral("💎"), QStringLiteral("Objects"), QStringLiteral("diamond jewel")},
        {QStringLiteral("laptop"), QStringLiteral("💻"), QStringLiteral("Objects"), QStringLiteral("computer code dev")},
        {QStringLiteral("iphone"), QStringLiteral("📱"), QStringLiteral("Objects"), QStringLiteral("phone mobile")},
        {QStringLiteral("camera"), QStringLiteral("📷"), QStringLiteral("Objects"), QStringLiteral("photo picture")},
        {QStringLiteral("clapper"), QStringLiteral("🎬"), QStringLiteral("Objects"), QStringLiteral("movie film video")},
        {QStringLiteral("headphones"), QStringLiteral("🎧"), QStringLiteral("Objects"), QStringLiteral("music audio")},
        {QStringLiteral("musical_note"), QStringLiteral("🎵"), QStringLiteral("Objects"), QStringLiteral("music song")},
        {QStringLiteral("moneybag"), QStringLiteral("💰"), QStringLiteral("Objects"), QStringLiteral("cash dollar rich")},
        {QStringLiteral("chart_with_upwards_trend"), QStringLiteral("📈"), QStringLiteral("Objects"), QStringLiteral("stonks growth graph")},
        {QStringLiteral("wrench"), QStringLiteral("🔧"), QStringLiteral("Objects"), QStringLiteral("tool fix")},
        {QStringLiteral("gear"), QStringLiteral("⚙️"), QStringLiteral("Objects"), QStringLiteral("settings config")},
        {QStringLiteral("hourglass"), QStringLiteral("⏳"), QStringLiteral("Objects"), QStringLiteral("time wait")},
        {QStringLiteral("alarm_clock"), QStringLiteral("⏰"), QStringLiteral("Objects"), QStringLiteral("time wake")},
        {QStringLiteral("pin"), QStringLiteral("📌"), QStringLiteral("Objects"), QStringLiteral("pushpin location")},
        {QStringLiteral("paperclip"), QStringLiteral("📎"), QStringLiteral("Objects"), QStringLiteral("attachment file")},
        {QStringLiteral("bulb"), QStringLiteral("💡"), QStringLiteral("Objects"), QStringLiteral("idea light")},
        {QStringLiteral("lock"), QStringLiteral("🔒"), QStringLiteral("Objects"), QStringLiteral("secure private")},
        {QStringLiteral("key"), QStringLiteral("🔑"), QStringLiteral("Objects"), QStringLiteral("password auth")},
        {QStringLiteral("bell"), QStringLiteral("🔔"), QStringLiteral("Objects"), QStringLiteral("notification alert")},
        {QStringLiteral("no_bell"), QStringLiteral("🔕"), QStringLiteral("Objects"), QStringLiteral("mute silent")},

        // Symbols
        {QStringLiteral("check"), QStringLiteral("✅"), QStringLiteral("Symbols"), QStringLiteral("done green check mark")},
        {QStringLiteral("white_check_mark"), QStringLiteral("✅"), QStringLiteral("Symbols"), QStringLiteral("done verified check")},
        {QStringLiteral("x"), QStringLiteral("❌"), QStringLiteral("Symbols"), QStringLiteral("no cancel cross fail")},
        {QStringLiteral("warning"), QStringLiteral("⚠️"), QStringLiteral("Symbols"), QStringLiteral("alert caution")},
        {QStringLiteral("question"), QStringLiteral("❓"), QStringLiteral("Symbols"), QStringLiteral("help ask")},
        {QStringLiteral("exclamation"), QStringLiteral("❗"), QStringLiteral("Symbols"), QStringLiteral("important bang")},
        {QStringLiteral("100"), QStringLiteral("💯"), QStringLiteral("Symbols"), QStringLiteral("perfect score hundred")},
        {QStringLiteral("zap"), QStringLiteral("⚡"), QStringLiteral("Symbols"), QStringLiteral("lightning bolt fast power")},
        {QStringLiteral("sun"), QStringLiteral("☀️"), QStringLiteral("Symbols"), QStringLiteral("sunny weather bright")},
        {QStringLiteral("moon"), QStringLiteral("🌙"), QStringLiteral("Symbols"), QStringLiteral("night crescent")},
        {QStringLiteral("rainbow"), QStringLiteral("🌈"), QStringLiteral("Symbols"), QStringLiteral("colorful weather")},
        {QStringLiteral("snowflake"), QStringLiteral("❄️"), QStringLiteral("Symbols"), QStringLiteral("cold winter snow")}
    };
    return catalog;
}
} // namespace

QString Backend::replaceEmojiShortcodes(const QString &text) {
    QString working = text;

    // 1. Process :shortcode: replacements
    if (working.contains(QLatin1Char(':'))) {
        static QHash<QString, QString> lookup;
        if (lookup.isEmpty()) {
            for (const EmojiSpec &spec : emojiCatalog()) {
                lookup.insert(spec.shortcode.toLower(), spec.emoji);
            }
        }

        static const QRegularExpression re(QStringLiteral(R"(:([a-zA-Z0-9_+\-]+):)"));
        QString out;
        out.reserve(working.size());
        int lastIdx = 0;
        auto it = re.globalMatch(working);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out.append(working.mid(lastIdx, m.capturedStart() - lastIdx));
            const QString code = m.captured(1).toLower();
            if (lookup.contains(code)) {
                out.append(lookup.value(code));
            } else {
                out.append(m.captured(0));
            }
            lastIdx = m.capturedEnd();
        }
        out.append(working.mid(lastIdx));
        working = out;
    }

    // 2. Process standalone ASCII emoticons (preserving URLs untouched)
    static const auto getEmoticonEmoji = [](const QString &tok) -> QString {
        const QString lower = tok.trimmed().toLower();
        if (lower == QStringLiteral(":-)") || lower == QStringLiteral(":)")) return QStringLiteral("😊");
        if (lower == QStringLiteral(":-(") || lower == QStringLiteral(":(")) return QStringLiteral("☹️");
        if (lower == QStringLiteral(":-d") || lower == QStringLiteral(":d") || lower == QStringLiteral("=d")) return QStringLiteral("😄");
        if (lower == QStringLiteral(";-)") || lower == QStringLiteral(";)")) return QStringLiteral("😉");
        if (lower == QStringLiteral(":-p") || lower == QStringLiteral(":p")) return QStringLiteral("😛");
        if (lower == QStringLiteral("<3")) return QStringLiteral("❤️");
        if (lower == QStringLiteral("</3")) return QStringLiteral("💔");
        if (lower == QStringLiteral(":-o") || lower == QStringLiteral(":o")) return QStringLiteral("😮");
        if (lower == QStringLiteral(":-/") || lower == QStringLiteral(":/")) return QStringLiteral("😕");
        if (lower == QStringLiteral(":'(") || lower == QStringLiteral(":'-(")) return QStringLiteral("😢");
        if (lower == QStringLiteral("b-)") || lower == QStringLiteral("b)") || lower == QStringLiteral("8-)") || lower == QStringLiteral("8)")) return QStringLiteral("😎");
        if (lower == QStringLiteral(":-*") || lower == QStringLiteral(":*")) return QStringLiteral("😘");
        if (lower == QStringLiteral("xd")) return QStringLiteral("😆");
        if (lower == QStringLiteral("-_-")) return QStringLiteral("😑");
        return QString();
    };

    static const QRegularExpression urlRe(QStringLiteral(R"((https?://\S+))"));
    static const QRegularExpression asciiRe(
        QStringLiteral(R"((?:^|(?<=\s))(:-?\)|:-?\(|:-?[dD]|=D|;-?\)|:-?[pP]|:P|:p|<3|<\/3|:-?[oO]|:O|:o|:-?\/|:'-?\(|[bB8]-?\)|[bB8]\)|:-?\*|:\*|[xX][dD]|-_-)(?=$|\s|[.,!?]))")
    );

    auto replaceAsciiInSegment = [](const QString &seg) -> QString {
        QString res;
        res.reserve(seg.size());
        int lastIdx = 0;
        auto it = asciiRe.globalMatch(seg);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            res.append(seg.mid(lastIdx, m.capturedStart() - lastIdx));
            const QString token = m.captured(1);
            const QString rep = getEmoticonEmoji(token);
            if (!rep.isEmpty()) {
                res.append(rep);
            } else {
                res.append(token);
            }
            lastIdx = m.capturedEnd();
        }
        res.append(seg.mid(lastIdx));
        return res;
    };

    if (!working.contains(QStringLiteral("http://")) && !working.contains(QStringLiteral("https://"))) {
        return replaceAsciiInSegment(working);
    }

    QString finalOut;
    finalOut.reserve(working.size());
    int lastIdx = 0;
    auto urlIt = urlRe.globalMatch(working);
    while (urlIt.hasNext()) {
        const QRegularExpressionMatch m = urlIt.next();
        const QString nonUrl = working.mid(lastIdx, m.capturedStart() - lastIdx);
        finalOut.append(replaceAsciiInSegment(nonUrl));
        finalOut.append(m.captured(1));
        lastIdx = m.capturedEnd();
    }
    finalOut.append(replaceAsciiInSegment(working.mid(lastIdx)));
    return finalOut;
}

QVariantList Backend::searchEmojis(const QString &query, const QString &category, int limit) const {
    const QString qRaw = query.trimmed();
    const QString qLower = qRaw.toLower();
    const QString cat = category.trimmed();
    const int maxItems = limit > 0 ? limit : 60;

    // Check ASCII emoticon table for exact or case-insensitive match
    struct AsciiCand {
        const char *token;
        std::initializer_list<const char *> list;
    };
    static const std::vector<AsciiCand> asciiMap = {
        {":-)", {"😊", "🙂", "😄", "😁"}},
        {":)", {"😊", "🙂", "😄", "😁"}},
        {":-(", {"😢", "☹️", "😞", "😔"}},
        {":(", {"😢", "☹️", "😞", "😔"}},
        {":-D", {"😄", "😁", "😆"}},
        {":D", {"😄", "😁", "😆"}},
        {"=D", {"😄", "😁", "😆"}},
        {";-)", {"😉", "😜"}},
        {";)", {"😉", "😜"}},
        {":-P", {"😛", "😜", "😝"}},
        {":-p", {"😛", "😜", "😝"}},
        {":P", {"😛", "😜", "😝"}},
        {":p", {"😛", "😜", "😝"}},
        {"<3", {"❤️", "🧡", "💛", "💚", "💙", "💜", "🤍"}},
        {"</3", {"💔"}},
        {":-O", {"😮", "😲", "😱"}},
        {":-o", {"😮", "😲", "😱"}},
        {":O", {"😮", "😲", "😱"}},
        {":o", {"😮", "😲", "😱"}},
        {":-/", {"😕", "😐", "😑"}},
        {":/", {"😕", "😐", "😑"}},
        {":'(", {"😢", "😭"}},
        {":'-(", {"😢", "😭"}},
        {"B-)", {"😎", "🤓"}},
        {"B)", {"😎", "🤓"}},
        {"8-)", {"😎", "🤓"}},
        {"8)", {"😎", "🤓"}},
        {":-*", {"😘", "😚", "😙"}},
        {":*", {"😘", "😚", "😙"}},
        {"XD", {"😆", "🤣"}},
        {"xd", {"😆", "🤣"}},
        {"-_-", {"😑"}}
    };

    if (!qRaw.isEmpty()) {
        for (const auto &ac : asciiMap) {
            if (qRaw == QString::fromUtf8(ac.token) || qLower == QString::fromUtf8(ac.token).toLower()) {
                QVariantList res;
                for (const char *cand : ac.list) {
                    QVariantMap item;
                    item.insert(QStringLiteral("shortcode"), QString::fromUtf8(ac.token));
                    item.insert(QStringLiteral("code"), QString::fromUtf8(ac.token));
                    item.insert(QStringLiteral("emoji"), QString::fromUtf8(cand));
                    item.insert(QStringLiteral("category"), QStringLiteral("Emoticons"));
                    item.insert(QStringLiteral("keywords"), QString::fromUtf8(ac.token));
                    res.append(item);
                    if (res.size() >= maxItems) break;
                }
                return res;
            }
        }
    }

    QVariantList prefixMatches;
    QVariantList keywordMatches;
    QSet<QString> seenEmojis;

    for (const EmojiSpec &spec : emojiCatalog()) {
        if (!cat.isEmpty() && cat.compare(QStringLiteral("All"), Qt::CaseInsensitive) != 0) {
            bool catMatch = (spec.category.compare(cat, Qt::CaseInsensitive) == 0);
            if (!catMatch && cat.compare(QStringLiteral("Activities"), Qt::CaseInsensitive) == 0) {
                catMatch = (spec.category.compare(QStringLiteral("Celebration"), Qt::CaseInsensitive) == 0);
            }
            if (!catMatch) continue;
        }
        const QString codeLower = spec.shortcode.toLower();
        bool isPrefix = false;
        bool isMatch = false;
        if (qLower.isEmpty()) {
            isPrefix = true;
            isMatch = true;
        } else if (codeLower.startsWith(qLower)) {
            isPrefix = true;
            isMatch = true;
        } else if (codeLower.contains(qLower) || spec.keywords.toLower().contains(qLower)) {
            isMatch = true;
        }

        if (!isMatch) continue;
        if (qLower.isEmpty() && seenEmojis.contains(spec.emoji)) continue;
        seenEmojis.insert(spec.emoji);

        QVariantMap item;
        item.insert(QStringLiteral("shortcode"), QStringLiteral(":") + spec.shortcode + QStringLiteral(":"));
        item.insert(QStringLiteral("code"), spec.shortcode);
        item.insert(QStringLiteral("emoji"), spec.emoji);
        item.insert(QStringLiteral("category"), spec.category);
        item.insert(QStringLiteral("keywords"), spec.keywords);

        if (isPrefix) {
            prefixMatches.append(item);
        } else {
            keywordMatches.append(item);
        }
        if (prefixMatches.size() >= maxItems) break;
    }

    QVariantList result = prefixMatches;
    for (const QVariant &v : keywordMatches) {
        if (result.size() >= maxItems) break;
        result.append(v);
    }
    return result;
}


QStringList Backend::selectedChatParticipants() const {
    if (m_chatParticipants.contains(m_selectedChatGuid)) {
        return m_chatParticipants.value(m_selectedChatGuid);
    }
    for (const ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) return c.participantHandles;
    }
    return QStringList();
}

bool Backend::isAudioAttachment(const QString &mimeType, const QString &fileName) {
    const QString m = mimeType.toLower().trimmed();
    const QString fn = fileName.toLower().trimmed();
    if (m.startsWith(QStringLiteral("audio/"))) return true;
    if (fn.endsWith(QStringLiteral(".caf")) ||
        fn.endsWith(QStringLiteral(".m4a")) ||
        fn.endsWith(QStringLiteral(".mp3")) ||
        fn.endsWith(QStringLiteral(".wav")) ||
        fn.endsWith(QStringLiteral(".aac")) ||
        fn.endsWith(QStringLiteral(".ogg")) ||
        fn.endsWith(QStringLiteral(".flac")) ||
        fn.endsWith(QStringLiteral(".opus")) ||
        fn.endsWith(QStringLiteral(".amr"))) {
        return true;
    }
    return false;
}

QVariantList Backend::computeWaveformBars(const QString &seed, int count) {
    if (count <= 0) count = 28;
    QVariantList bars;
    bars.reserve(count);
    quint32 h = qHash(seed.isEmpty() ? QStringLiteral("waveform-default") : seed);
    for (int i = 0; i < count; ++i) {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        double val = 0.15 + (double(h % 1000) / 1000.0) * 0.85;
        bars.append(val);
    }
    return bars;
}

QString Backend::formatAudioDuration(int totalSeconds) {
    const int sec = qMax(0, totalSeconds);
    return QStringLiteral("%1:%2").arg(sec / 60).arg(sec % 60, 2, 10, QLatin1Char('0'));
}

bool Backend::hasNonEmptyWavDataChunk(const QString &filePath) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(12);
    if (head.size() < 12 || !head.startsWith("RIFF") || head.mid(8, 4) != "WAVE") {
        return false;
    }
    bool hasFmt = false;
    while (!f.atEnd()) {
        const QByteArray chunkHdr = f.read(8);
        if (chunkHdr.size() < 8) break;
        const QByteArray chunkId = chunkHdr.left(4);
        const quint32 chunkSize = static_cast<quint8>(chunkHdr[4])
                                | (static_cast<quint8>(chunkHdr[5]) << 8)
                                | (static_cast<quint8>(chunkHdr[6]) << 16)
                                | (static_cast<quint8>(chunkHdr[7]) << 24);
        if (chunkId == "fmt ") {
            if (chunkSize < 16) return false;
            const QByteArray fmt = f.read(chunkSize);
            if (fmt.size() < 16) return false;
            hasFmt = true;
            if (chunkSize & 1) f.seek(f.pos() + 1);
        } else if (chunkId == "data") {
            const qint64 remainingBytes = qMax<qint64>(0, f.size() - f.pos());
            const qint64 effectiveBytes = (chunkSize == 0 || chunkSize == 0xFFFFFFFFU)
                                              ? remainingBytes
                                              : qMin<qint64>(chunkSize, remainingBytes);
            return hasFmt && effectiveBytes >= 2;
        } else {
            const qint64 skip = qint64(chunkSize) + (chunkSize & 1);
            if (skip < 0 || f.pos() + skip > f.size()) break;
            f.seek(f.pos() + skip);
        }
    }
    return false;
}

int Backend::wavDurationSeconds(const QString &filePath) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray head = f.read(12);
    if (head.size() < 12 || !head.startsWith("RIFF") || head.mid(8, 4) != "WAVE") {
        return 0;
    }
    quint32 byteRate = 0;
    qint64 dataSize = 0;
    while (!f.atEnd()) {
        const QByteArray chunkHdr = f.read(8);
        if (chunkHdr.size() < 8) break;
        const QByteArray chunkId = chunkHdr.left(4);
        const quint32 chunkSize = static_cast<quint8>(chunkHdr[4])
                                | (static_cast<quint8>(chunkHdr[5]) << 8)
                                | (static_cast<quint8>(chunkHdr[6]) << 16)
                                | (static_cast<quint8>(chunkHdr[7]) << 24);
        if (chunkId == "fmt " && chunkSize >= 16) {
            const QByteArray fmt = f.read(chunkSize);
            if (fmt.size() >= 16) {
                const quint16 numCh = static_cast<quint8>(fmt[2]) | (static_cast<quint8>(fmt[3]) << 8);
                const quint32 sampleRate = static_cast<quint8>(fmt[4])
                                         | (static_cast<quint8>(fmt[5]) << 8)
                                         | (static_cast<quint8>(fmt[6]) << 16)
                                         | (static_cast<quint8>(fmt[7]) << 24);
                byteRate = static_cast<quint8>(fmt[8])
                         | (static_cast<quint8>(fmt[9]) << 8)
                         | (static_cast<quint8>(fmt[10]) << 16)
                         | (static_cast<quint8>(fmt[11]) << 24);
                const quint16 bitsPerSample = static_cast<quint8>(fmt[14]) | (static_cast<quint8>(fmt[15]) << 8);
                if (byteRate == 0 && sampleRate > 0 && numCh > 0 && bitsPerSample > 0) {
                    byteRate = sampleRate * numCh * (bitsPerSample / 8);
                }
            }
            if (chunkSize & 1) f.seek(f.pos() + 1);
        } else if (chunkId == "data") {
            const qint64 remainingBytes = qMax<qint64>(0, f.size() - f.pos());
            dataSize = (chunkSize == 0 || chunkSize == 0xFFFFFFFFU)
                           ? remainingBytes
                           : qMin<qint64>(chunkSize, remainingBytes);
            break;
        } else {
            const qint64 skip = qint64(chunkSize) + (chunkSize & 1);
            if (skip < 0 || f.pos() + skip > f.size()) break;
            f.seek(f.pos() + skip);
        }
    }
    if (byteRate == 0 || dataSize <= 0) return 0;
    return qMax(1, int(qRound(double(dataSize) / double(byteRate))));
}

QVariantList Backend::computeAudioFileWaveformBars(const QString &filePath, const QString &seed, int count) {
    if (count <= 0) count = 28;
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return computeWaveformBars(seed, count);
    const QByteArray head = f.read(12);
    if (head.size() < 12 || !head.startsWith("RIFF") || head.mid(8, 4) != "WAVE") {
        return computeWaveformBars(seed, count);
    }
    quint16 audioFormat = 0;
    quint16 bitsPerSample = 0;
    qint64 dataOffset = -1;
    qint64 dataBytes = 0;
    while (!f.atEnd()) {
        const QByteArray chunkHdr = f.read(8);
        if (chunkHdr.size() < 8) break;
        const QByteArray chunkId = chunkHdr.left(4);
        const quint32 chunkSize = static_cast<quint8>(chunkHdr[4])
                                | (static_cast<quint8>(chunkHdr[5]) << 8)
                                | (static_cast<quint8>(chunkHdr[6]) << 16)
                                | (static_cast<quint8>(chunkHdr[7]) << 24);
        if (chunkId == "fmt " && chunkSize >= 16) {
            const QByteArray fmt = f.read(chunkSize);
            if (fmt.size() >= 16) {
                audioFormat = static_cast<quint8>(fmt[0]) | (static_cast<quint8>(fmt[1]) << 8);
                bitsPerSample = static_cast<quint8>(fmt[14]) | (static_cast<quint8>(fmt[15]) << 8);
            }
            if (chunkSize & 1) f.seek(f.pos() + 1);
        } else if (chunkId == "data") {
            dataOffset = f.pos();
            const qint64 remainingBytes = qMax<qint64>(0, f.size() - dataOffset);
            dataBytes = (chunkSize == 0 || chunkSize == 0xFFFFFFFFU)
                            ? remainingBytes
                            : qMin<qint64>(chunkSize, remainingBytes);
            break;
        } else {
            const qint64 skip = qint64(chunkSize) + (chunkSize & 1);
            if (skip < 0 || f.pos() + skip > f.size()) break;
            f.seek(f.pos() + skip);
        }
    }
    if (audioFormat != 1 || bitsPerSample != 16 || dataOffset < 0 || dataBytes < 4) {
        return computeWaveformBars(seed, count);
    }
    const qint64 totalSamples = dataBytes / 2;
    QList<double> rawPeaks;
    rawPeaks.reserve(count);
    double maxPeak = 0.0;
    constexpr qint64 kMaxSamplesPerBarRead = 128;
    for (int i = 0; i < count; ++i) {
        const qint64 startIdx = (i * totalSamples) / count;
        const qint64 endIdx = qMax(startIdx + 1, ((i + 1) * totalSamples) / count);
        const qint64 windowSamples = qMin<qint64>(endIdx - startIdx, kMaxSamplesPerBarRead);
        if (!f.seek(dataOffset + startIdx * 2)) break;
        const QByteArray slice = f.read(windowSamples * 2);
        const qint64 sliceSamples = slice.size() / 2;
        const char *raw = slice.constData();
        double sumSq = 0.0;
        for (qint64 s = 0; s < sliceSamples; ++s) {
            const qint16 sample = static_cast<qint16>(
                static_cast<quint8>(raw[s * 2]) | (static_cast<quint8>(raw[s * 2 + 1]) << 8));
            const double norm = double(sample) / 32768.0;
            sumSq += norm * norm;
        }
        const double rms = (sliceSamples > 0) ? std::sqrt(sumSq / double(sliceSamples)) : 0.0;
        rawPeaks.append(rms);
        if (rms > maxPeak) maxPeak = rms;
    }
    if (rawPeaks.size() < count || maxPeak <= 0.0005) {
        return computeWaveformBars(seed, count);
    }
    QVariantList bars;
    bars.reserve(count);
    for (double p : rawPeaks) {
        bars.append(qBound(0.15, 0.15 + (p / maxPeak) * 0.85, 1.0));
    }
    return bars;
}

void Backend::stopAudioPlaybackProcess() {
    if (!m_audioProcess) return;
    QProcess *proc = m_audioProcess;
    m_audioProcess = nullptr;
    proc->disconnect(this);
    if (proc->state() != QProcess::NotRunning) {
        proc->terminate();
        if (!proc->waitForFinished(250)) {
            proc->kill();
            proc->waitForFinished(200);
        }
    }
    proc->deleteLater();
}

bool Backend::canSeekLocalAudioPlayback() const {
#ifdef IMSG_UNIT_TEST
    return true;
#else
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        return true;
    }
    return !QStandardPaths::findExecutable(QStringLiteral("ffplay")).isEmpty();
#endif
}

bool Backend::startLocalAudioProcess(const QString &localFile, quint64 playbackGen) {
    if (localFile.isEmpty() || !QFileInfo::exists(localFile)) {
        m_audioPlaying = false;
        m_audioPlaybackTimer.stop();
        emit audioPlaybackChanged();
        return false;
    }
#ifdef IMSG_UNIT_TEST
    Q_UNUSED(playbackGen);
    m_audioPlaybackElapsedMs = 0;
    m_audioPlaying = true;
    m_audioPlaybackTimer.start();
    return true;
#else
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        Q_UNUSED(playbackGen);
        m_audioPlaybackElapsedMs = 0;
        m_audioPlaying = true;
        m_audioPlaybackTimer.start();
        return true;
    }
    stopAudioPlaybackProcess();

    QString bin;
    QStringList args;
    const QString effectiveOut = effectiveAudioOutputDevice();
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!effectiveOut.isEmpty()) {
        env.insert(QStringLiteral("PULSE_SINK"), effectiveOut);
    }

    if (!QStandardPaths::findExecutable(QStringLiteral("ffplay")).isEmpty()) {
        bin = QStringLiteral("ffplay");
        args << QStringLiteral("-nodisp") << QStringLiteral("-autoexit") << QStringLiteral("-loglevel") << QStringLiteral("error");
        if (m_audioSeekOffsetMs > 250) {
            args << QStringLiteral("-ss") << QString::number(double(m_audioSeekOffsetMs) / 1000.0, 'f', 2);
        }
        args << localFile;
    } else if (!QStandardPaths::findExecutable(QStringLiteral("pw-play")).isEmpty()) {
        bin = QStringLiteral("pw-play");
        m_audioSeekOffsetMs = 0;
        m_audioProgress = 0.0;
        if (!effectiveOut.isEmpty()) {
            args << QStringLiteral("--target") << effectiveOut;
        }
        args << localFile;
    } else if (!QStandardPaths::findExecutable(QStringLiteral("paplay")).isEmpty()) {
        bin = QStringLiteral("paplay");
        m_audioSeekOffsetMs = 0;
        m_audioProgress = 0.0;
        if (!effectiveOut.isEmpty()) {
            args << (QStringLiteral("--device=") + effectiveOut);
        }
        args << localFile;
    } else {
        m_audioPlaying = false;
        m_audioPlaybackTimer.stop();
        m_audioProgress = 0.0;
        m_audioPlaybackElapsedMs = 0;
        m_audioSeekOffsetMs = 0;
        emit audioPlaybackChanged();
        emit toastMessage(QStringLiteral("⚠ No audio player found (ffplay/pw-play/paplay)"));
        return false;
    }

    QProcess *proc = new QProcess(this);
    proc->setProcessEnvironment(env);
    m_audioProcess = proc;
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, playbackGen](QProcess::ProcessError) {
                if (m_audioProcess == proc) {
                    m_audioProcess = nullptr;
                }
                proc->deleteLater();
                if (m_audioPlaybackGen != playbackGen) return;
                m_audioPlaying = false;
                m_audioPlaybackTimer.stop();
                m_audioProgress = 0.0;
                m_audioPlaybackElapsedMs = 0;
                m_audioSeekOffsetMs = 0;
                m_audioDurationText = formatAudioDuration(m_audioPlaybackDurationMs / 1000);
                emit audioPlaybackChanged();
                emit toastMessage(QStringLiteral("⚠ Audio playback failed"));
            });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, playbackGen](int, QProcess::ExitStatus) {
                if (m_audioProcess == proc) {
                    m_audioProcess = nullptr;
                }
                proc->deleteLater();
                if (m_audioPlaybackGen != playbackGen) return;
                m_audioPlaying = false;
                m_audioPlaybackTimer.stop();
                m_audioProgress = 0.0;
                m_audioPlaybackElapsedMs = 0;
                m_audioSeekOffsetMs = 0;
                m_audioDurationText = formatAudioDuration(m_audioPlaybackDurationMs / 1000);
                emit audioPlaybackChanged();
            });
    m_audioPlaybackElapsedMs = 0;
    m_audioPlaying = true;
    m_audioPlaybackTimer.start();
    proc->start(bin, args);
    return true;
#endif
}

void Backend::stopVoiceRecordProcess(bool waitForFlush) {
    if (!m_voiceRecordProcess) return;
    QProcess *proc = m_voiceRecordProcess;
    m_voiceRecordProcess = nullptr;
    proc->disconnect(this);
    if (proc->state() != QProcess::NotRunning) {
        proc->terminate();
        if (!proc->waitForFinished(waitForFlush ? 1500 : 300)) {
            proc->kill();
            proc->waitForFinished(250);
        }
    }
    proc->deleteLater();
}

void Backend::toggleAudioPlayback(const QString &attachmentGuid, const QString &sourceUrl) {
    if (attachmentGuid.isEmpty()) return;

    if (m_playingAudioGuid == attachmentGuid) {
        if (m_audioPlaying) {
            ++m_audioPlaybackGen;
            if (canSeekLocalAudioPlayback()) {
                m_audioSeekOffsetMs = qBound(0, m_audioSeekOffsetMs + m_audioPlaybackElapsedMs, m_audioPlaybackDurationMs);
            } else {
                m_audioSeekOffsetMs = 0;
                m_audioProgress = 0.0;
            }
            m_audioPlaybackElapsedMs = 0;
            m_audioPlaying = false;
            m_audioPlaybackTimer.stop();
            stopAudioPlaybackProcess();
            emit audioPlaybackChanged();
            return;
        }
        if (m_audioDownloadReply && !m_audioDownloadReply->isFinished()) {
            // User toggled while uncached audio download was still in flight: pause/abort download
            ++m_audioPlaybackGen;
            QNetworkReply *inFlight = m_audioDownloadReply;
            m_audioDownloadReply = nullptr;
            inFlight->abort();
            m_audioPlaying = false;
            m_audioPlaybackTimer.stop();
            emit audioPlaybackChanged();
            return;
        }
        if (!m_currentAudioLocalFile.isEmpty() && QFileInfo::exists(m_currentAudioLocalFile)) {
            const quint64 gen = ++m_audioPlaybackGen;
            if (m_audioProgress >= 1.0 || !canSeekLocalAudioPlayback()) {
                m_audioProgress = 0.0;
                m_audioSeekOffsetMs = 0;
            }
            m_audioPlaybackElapsedMs = 0;
            startLocalAudioProcess(m_currentAudioLocalFile, gen);
            emit audioPlaybackChanged();
            return;
        }
        // Otherwise, previous uncached download was paused/aborted before finishing: fall through to restart download!
    }

    const QString effectiveSourceUrl = !sourceUrl.isEmpty()
                                           ? sourceUrl
                                           : ((m_playingAudioGuid == attachmentGuid) ? m_currentAudioSourceUrl : QString());
    const quint64 gen = ++m_audioPlaybackGen;
    if (m_audioDownloadReply) {
        QNetworkReply *oldReply = m_audioDownloadReply;
        m_audioDownloadReply = nullptr;
        oldReply->abort();
    }
    stopAudioPlaybackProcess();
    m_audioPlaybackTimer.stop();
    m_audioPlaying = false;
    m_currentAudioLocalFile.clear();
    m_currentAudioSourceUrl = effectiveSourceUrl;

    QString localFile = lookupCachedAttachmentPath(m_attachmentPaths, attachmentGuid);
    if (localFile.isEmpty() && effectiveSourceUrl.startsWith(QStringLiteral("file://"))) {
        localFile = QUrl(effectiveSourceUrl).toLocalFile();
    } else if (localFile.isEmpty() && !effectiveSourceUrl.isEmpty() &&
               !effectiveSourceUrl.startsWith(QStringLiteral("/api/")) &&
               !effectiveSourceUrl.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
               !effectiveSourceUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        localFile = effectiveSourceUrl;
    }
    if (!localFile.isEmpty() && !QFileInfo::exists(localFile)) {
        localFile.clear();
    }

    int durSec = !localFile.isEmpty() ? wavDurationSeconds(localFile) : 0;
    if (durSec <= 0) {
        for (const QVariant &mv : m_messages) {
            const QVariantMap msgMap = mv.toMap();
            for (const QVariant &av : msgMap.value(QStringLiteral("attachments")).toList()) {
                const QVariantMap aMap = av.toMap();
                if (aMap.value(QStringLiteral("guid")).toString() == attachmentGuid) {
                    const double rawDur = aMap.value(QStringLiteral("durationSeconds")).toDouble();
                    durSec = (rawDur > 0.0) ? qMax(1, qRound(rawDur)) : 0;
                    break;
                }
            }
            if (durSec > 0) break;
        }
    }
    if (durSec <= 0) durSec = 15;

    m_playingAudioGuid = attachmentGuid;
    m_audioProgress = 0.0;
    m_audioPlaybackElapsedMs = 0;
    m_audioSeekOffsetMs = 0;
    m_audioPlaybackDurationMs = durSec * 1000;
    m_audioDurationText = formatAudioDuration(durSec);

    if (!localFile.isEmpty()) {
        m_currentAudioLocalFile = localFile;
        startLocalAudioProcess(localFile, gen);
        emit audioPlaybackChanged();
        return;
    }

    QUrl downloadUrl;
    if (effectiveSourceUrl.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
        effectiveSourceUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        const QUrl candidateUrl(effectiveSourceUrl);
        if (!isTrustedPasswordTransportUrl(candidateUrl)) {
            emit audioPlaybackChanged();
            emit toastMessage(QStringLiteral("⚠ Refusing insecure HTTP audio download for remote host"));
            return;
        }
        downloadUrl = candidateUrl;
        if (!m_password.isEmpty()) {
            QUrlQuery q(downloadUrl);
            if (!q.hasQueryItem(QStringLiteral("password"))) {
                q.addQueryItem(QStringLiteral("password"), m_password);
                downloadUrl.setQuery(q);
            }
        }
    } else if (!m_serverUrl.isEmpty()) {
        const QString apiPath = effectiveSourceUrl.startsWith(QStringLiteral("/api/"))
                                    ? effectiveSourceUrl
                                    : (QStringLiteral("/api/v1/attachment/") + attachmentGuid + QStringLiteral("/download"));
        downloadUrl = buildApiUrl(apiPath);
        if (!downloadUrl.isValid() || downloadUrl.isEmpty()) {
            emit audioPlaybackChanged();
            emit toastMessage(QStringLiteral("⚠ Refusing insecure HTTP audio download for remote host"));
            return;
        }
    }

    // Do NOT set m_audioPlaying = true or start m_audioPlaybackTimer while downloading!
    // In offline / demo mode with no configured server or remote source URL, run simulated timer state.
    if (effectiveSourceUrl.isEmpty() && (m_serverUrl.isEmpty() || (!m_autoStart && m_configFilePath.isEmpty()))) {
        m_audioPlaying = true;
        m_audioPlaybackTimer.start();
        emit audioPlaybackChanged();
        return;
    }
    emit audioPlaybackChanged();
    if (!downloadUrl.isValid() || downloadUrl.isEmpty()) {
        return;
    }

    QNetworkRequest req(downloadUrl);
    req.setTransferTimeout(20000);
    QNetworkReply *reply = m_mediaNam.get(req);
    m_audioDownloadReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, attachmentGuid, effectiveSourceUrl, gen]() {
        if (m_audioDownloadReply == reply) {
            m_audioDownloadReply = nullptr;
        }
        reply->deleteLater();
        if (m_audioPlaybackGen != gen || m_playingAudioGuid != attachmentGuid) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            m_audioPlaying = false;
            m_audioPlaybackTimer.stop();
            emit audioPlaybackChanged();
            emit toastMessage(QStringLiteral("⚠ Failed to download voice memo"));
            return;
        }
        const QByteArray bytes = reply->readAll();
        if (bytes.isEmpty()) {
            m_audioPlaying = false;
            m_audioPlaybackTimer.stop();
            emit audioPlaybackChanged();
            emit toastMessage(QStringLiteral("⚠ Empty voice memo audio"));
            return;
        }
        const QString ctype = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        QString ext = QStringLiteral(".wav");
        if (ctype.contains(QStringLiteral("mp4")) || ctype.contains(QStringLiteral("m4a")) || effectiveSourceUrl.endsWith(QStringLiteral(".m4a"), Qt::CaseInsensitive)) ext = QStringLiteral(".m4a");
        else if (ctype.contains(QStringLiteral("caf")) || effectiveSourceUrl.endsWith(QStringLiteral(".caf"), Qt::CaseInsensitive)) ext = QStringLiteral(".caf");
        else if (ctype.contains(QStringLiteral("mpeg")) || effectiveSourceUrl.endsWith(QStringLiteral(".mp3"), Qt::CaseInsensitive)) ext = QStringLiteral(".mp3");
        else if (ctype.contains(QStringLiteral("ogg")) || ctype.contains(QStringLiteral("opus"))) ext = QStringLiteral(".ogg");

        const QString cacheDir = QDir::homePath() + QStringLiteral("/.cache/imsg/attachments");
        QDir().mkpath(cacheDir);
        const QString destPath = cacheDir + QLatin1Char('/') + attachmentGuid.left(16) + QStringLiteral("_voice") + ext;
        QFile f(destPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(bytes);
            f.close();
            insertCachedAttachmentPath(m_attachmentPaths, attachmentGuid, destPath);
            m_currentAudioLocalFile = destPath;
            const int wavSec = wavDurationSeconds(destPath);
            if (wavSec > 0) {
                m_audioPlaybackDurationMs = wavSec * 1000;
                const int currentPosMs = m_audioSeekOffsetMs + m_audioPlaybackElapsedMs;
                const int remSec = qMax(0, (m_audioPlaybackDurationMs - currentPosMs) / 1000);
                m_audioDurationText = formatAudioDuration(remSec);
            }
            startLocalAudioProcess(destPath, gen);
            emit audioPlaybackChanged();
        }
    });
}

void Backend::seekAudioPlayback(const QString &attachmentGuid, double ratio) {
    if (!m_currentAudioLocalFile.isEmpty() && !canSeekLocalAudioPlayback()) {
        return;
    }
    if (!attachmentGuid.isEmpty() && m_playingAudioGuid != attachmentGuid) {
        toggleAudioPlayback(attachmentGuid, QString());
    }
    m_audioProgress = qBound(0.0, ratio, 1.0);
    m_audioSeekOffsetMs = int(m_audioProgress * m_audioPlaybackDurationMs);
    m_audioPlaybackElapsedMs = 0;
    const int remSec = qMax(0, (m_audioPlaybackDurationMs - m_audioSeekOffsetMs) / 1000);
    m_audioDurationText = formatAudioDuration(remSec);
    if (m_audioPlaying && !m_currentAudioLocalFile.isEmpty() && QFileInfo::exists(m_currentAudioLocalFile)) {
        startLocalAudioProcess(m_currentAudioLocalFile, ++m_audioPlaybackGen);
    }
    emit audioPlaybackChanged();
}

void Backend::clearStagedVoiceMemo(bool removeFile) {
    if (m_playingAudioGuid == QStringLiteral("voice-memo-preview")) {
        ++m_audioPlaybackGen;
        m_audioPlaying = false;
        m_audioProgress = 0.0;
        m_audioPlaybackElapsedMs = 0;
        m_audioSeekOffsetMs = 0;
        m_audioPlaybackTimer.stop();
        stopAudioPlaybackProcess();
        m_playingAudioGuid.clear();
        m_currentAudioLocalFile.clear();
        m_currentAudioSourceUrl.clear();
        emit audioPlaybackChanged();
    }
    if (removeFile && !m_recordedVoicePath.isEmpty()) {
        QFile::remove(m_recordedVoicePath);
    }
    m_hasRecordedVoiceMemo = false;
    m_recordedVoiceDurationSeconds = 0;
    m_recordedVoiceWaveform.clear();
    m_recordedVoicePath.clear();
    m_recordedVoiceChatGuid.clear();
}

QString Backend::finalizeCapturedVoiceWav(const QString &recPath) {
    if (recPath.isEmpty()) return QString();

#ifdef IMSG_UNIT_TEST
    const bool synthesizeForTest = true;
#else
    const bool synthesizeForTest = (QGuiApplication::platformName() == QStringLiteral("offscreen"));
#endif

    if (synthesizeForTest && !QFile::exists(recPath)) {
        QFile wavFile(recPath);
        if (wavFile.open(QIODevice::WriteOnly)) {
            const quint32 sampleRate = 16000;
            const quint32 numSamples = sampleRate; // 1 second test tone
            const quint32 dataSize = numSamples * 2;
            const quint32 fileSize = 36 + dataSize;
            QByteArray wav;
            wav.reserve(int(44 + dataSize));
            auto appendU32 = [&wav](quint32 v) {
                char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
                wav.append(b, 4);
            };
            auto appendU16 = [&wav](quint16 v) {
                char b[2] = {char(v & 0xFF), char((v >> 8) & 0xFF)};
                wav.append(b, 2);
            };
            wav.append("RIFF", 4);
            appendU32(fileSize);
            wav.append("WAVEfmt ", 8);
            appendU32(16);
            appendU16(1);
            appendU16(1);
            appendU32(sampleRate);
            appendU32(sampleRate * 2);
            appendU16(2);
            appendU16(16);
            wav.append("data", 4);
            appendU32(dataSize);
            for (quint32 i = 0; i < numSamples; ++i) {
                const double s = std::sin(2.0 * 3.14159265358979323846 * 440.0 * double(i) / double(sampleRate));
                const qint16 pcm = static_cast<qint16>(s * 12000.0);
                appendU16(static_cast<quint16>(pcm));
            }
            wavFile.write(wav);
            wavFile.close();
        }
    }

    const QFileInfo fi(recPath);
    if (!fi.exists() || fi.size() <= 44 || !hasNonEmptyWavDataChunk(recPath)) {
        QFile::remove(recPath);
        emit toastMessage(QStringLiteral("⚠ No microphone audio captured"));
        return QString();
    }
    return recPath;
}

void Backend::startVoiceRecording() {
    if (m_recordingVoice) return;
    clearStagedVoiceMemo(true);
    stopVoiceRecordProcess(false);
    if (!m_currentVoiceRecordingPath.isEmpty()) {
        QFile::remove(m_currentVoiceRecordingPath);
        m_currentVoiceRecordingPath.clear();
    }

    QString recDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (recDir.isEmpty()) {
        recDir = QDir::tempPath();
    } else {
        recDir += QStringLiteral("/voice_memos");
    }
    QDir().mkpath(recDir);
    const QString targetPath = recDir + QStringLiteral("/voice_memo_%1.wav").arg(QDateTime::currentMSecsSinceEpoch());

#ifndef IMSG_UNIT_TEST
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        QString bin;
        QStringList args;
        const QString effectiveIn = effectiveAudioInputDevice();
        if (!QStandardPaths::findExecutable(QStringLiteral("pw-record")).isEmpty()) {
            bin = QStringLiteral("pw-record");
            args << QStringLiteral("--format=s16")
                 << QStringLiteral("--rate=16000")
                 << QStringLiteral("--channels=1");
            if (!effectiveIn.isEmpty()) {
                args << QStringLiteral("--target") << effectiveIn;
            }
            args << targetPath;
        } else if (!QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            bin = QStringLiteral("ffmpeg");
            args << QStringLiteral("-y") << QStringLiteral("-loglevel") << QStringLiteral("error")
                 << QStringLiteral("-f") << QStringLiteral("pulse")
                 << QStringLiteral("-i") << (effectiveIn.isEmpty() ? QStringLiteral("default") : effectiveIn)
                 << QStringLiteral("-ac") << QStringLiteral("1")
                 << QStringLiteral("-ar") << QStringLiteral("16000")
                 << QStringLiteral("-sample_fmt") << QStringLiteral("s16")
                 << targetPath;
        } else if (!QStandardPaths::findExecutable(QStringLiteral("arecord")).isEmpty()) {
            bin = QStringLiteral("arecord");
            args << QStringLiteral("-q")
                 << QStringLiteral("-f") << QStringLiteral("S16_LE")
                 << QStringLiteral("-r") << QStringLiteral("16000")
                 << QStringLiteral("-c") << QStringLiteral("1")
                 << targetPath;
        }
        if (bin.isEmpty()) {
            emit toastMessage(QStringLiteral("⚠ No microphone capture tool found (pw-record/ffmpeg/arecord)"));
            return;
        }
        QProcess *proc = new QProcess(this);
        if (!effectiveIn.isEmpty()) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("PULSE_SOURCE"), effectiveIn);
            proc->setProcessEnvironment(env);
        }
        proc->start(bin, args);
        if (!proc->waitForStarted(600)) {
            proc->deleteLater();
            emit toastMessage(QStringLiteral("⚠ Failed to start microphone recording"));
            return;
        }
        m_voiceRecordProcess = proc;
        connect(proc, &QProcess::errorOccurred, this,
                [this, proc](QProcess::ProcessError) {
                    if (m_voiceRecordProcess != proc) return;
                    m_voiceRecordProcess = nullptr;
                    proc->disconnect(this);
                    proc->deleteLater();
                    if (!m_recordingVoice) return;
                    m_recordingVoice = false;
                    m_voiceRecordingTimer.stop();
                    const QString pendingPath = m_currentVoiceRecordingPath;
                    m_currentVoiceRecordingPath.clear();
                    const QString salvaged = finalizeCapturedVoiceWav(pendingPath);
                    if (!salvaged.isEmpty()) {
                        int dur = wavDurationSeconds(salvaged);
                        if (dur <= 0) dur = qMax(1, m_voiceRecordingSeconds);
                        m_recordedVoicePath = salvaged;
                        m_recordedVoiceDurationSeconds = dur;
                        m_recordedVoiceWaveform = computeAudioFileWaveformBars(salvaged, salvaged, 28);
                        m_hasRecordedVoiceMemo = true;
                    } else {
                        clearStagedVoiceMemo(true);
                    }
                    emit voiceRecordingChanged();
                    emit toastMessage(QStringLiteral("⚠ Microphone recording stopped unexpectedly"));
                });
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, proc](int, QProcess::ExitStatus) {
                    if (m_voiceRecordProcess != proc) return;
                    m_voiceRecordProcess = nullptr;
                    proc->disconnect(this);
                    proc->deleteLater();
                    if (!m_recordingVoice) return;
                    m_recordingVoice = false;
                    m_voiceRecordingTimer.stop();
                    const QString pendingPath = m_currentVoiceRecordingPath;
                    m_currentVoiceRecordingPath.clear();
                    const QString salvaged = finalizeCapturedVoiceWav(pendingPath);
                    if (!salvaged.isEmpty()) {
                        int dur = wavDurationSeconds(salvaged);
                        if (dur <= 0) dur = qMax(1, m_voiceRecordingSeconds);
                        m_recordedVoicePath = salvaged;
                        m_recordedVoiceDurationSeconds = dur;
                        m_recordedVoiceWaveform = computeAudioFileWaveformBars(salvaged, salvaged, 28);
                        m_hasRecordedVoiceMemo = true;
                    } else {
                        clearStagedVoiceMemo(true);
                    }
                    emit voiceRecordingChanged();
                    emit toastMessage(QStringLiteral("⚠ Microphone recording stopped unexpectedly"));
                });
    }
#endif

    m_recordingVoice = true;
    m_voiceRecordingSeconds = 0;
    m_voiceRecordingWaveform = computeWaveformBars(QUuid::createUuid().toString(), 28);
    m_currentVoiceRecordingPath = targetPath;
    m_recordedVoiceChatGuid = m_selectedChatGuid;
    m_voiceRecordingTimer.start();
    emit voiceRecordingChanged();
}

void Backend::stopVoiceRecordingForPreview() {
    if (!m_recordingVoice) return;
    m_recordingVoice = false;
    m_voiceRecordingTimer.stop();
    stopVoiceRecordProcess(true);

    const QString recPath = finalizeCapturedVoiceWav(m_currentVoiceRecordingPath);
    m_currentVoiceRecordingPath.clear();
    if (!recPath.isEmpty()) {
        int dur = wavDurationSeconds(recPath);
        if (dur <= 0) dur = qMax(1, m_voiceRecordingSeconds);
        m_recordedVoicePath = recPath;
        m_recordedVoiceDurationSeconds = dur;
        m_recordedVoiceWaveform = computeAudioFileWaveformBars(recPath, recPath, 28);
        m_hasRecordedVoiceMemo = true;
    } else {
        clearStagedVoiceMemo(true);
    }
    emit voiceRecordingChanged();
}

void Backend::togglePreviewRecordedVoiceMemo() {
    if (!m_hasRecordedVoiceMemo || m_recordedVoicePath.isEmpty() || !QFileInfo::exists(m_recordedVoicePath)) {
        return;
    }
    toggleAudioPlayback(QStringLiteral("voice-memo-preview"), m_recordedVoicePath);
}

bool Backend::canSendRecordedVoiceMemoToSelectedChat() const {
    if (m_selectedChatGuid.isEmpty()) return false;
    if (!m_recordedVoiceChatGuid.isEmpty()) {
        return isSameConversation(m_recordedVoiceChatGuid, m_selectedChatGuid);
    }
    return true;
}

void Backend::sendRecordedVoiceMemo() {
    if (!m_hasRecordedVoiceMemo || m_recordedVoicePath.isEmpty()) return;
    const QString recPath = m_recordedVoicePath;
    const QFileInfo fi(recPath);
    if (!fi.exists() || fi.size() <= 44 || !hasNonEmptyWavDataChunk(recPath)) {
        clearStagedVoiceMemo(true);
        emit voiceRecordingChanged();
        emit toastMessage(QStringLiteral("⚠ No microphone audio captured"));
        return;
    }
    if (m_selectedChatGuid.isEmpty()) {
        emit toastMessage(QStringLiteral("⚠ Select a conversation before sending voice memo"));
        return;
    }
    if (!m_recordedVoiceChatGuid.isEmpty() && !isSameConversation(m_recordedVoiceChatGuid, m_selectedChatGuid)) {
        emit toastMessage(QStringLiteral("⚠ Voice memo belongs to another conversation; switch back or discard it"));
        return;
    }
    if (!m_demoMode && (m_serverUrl.isEmpty() || m_password.isEmpty() || !buildApiUrl(QStringLiteral("/api/v1/message/attachment")).isValid())) {
        emit toastMessage(QStringLiteral("⚠ Relay server not ready; voice memo kept as draft"));
        return;
    }
    clearStagedVoiceMemo(false);
    emit voiceRecordingChanged();
    sendAttachmentInternal(m_selectedChatGuid, recPath, QString(), true);
}

void Backend::cancelVoiceRecording() {
    m_recordingVoice = false;
    m_voiceRecordingSeconds = 0;
    m_voiceRecordingTimer.stop();
    stopVoiceRecordProcess(false);
    if (!m_currentVoiceRecordingPath.isEmpty()) {
        QFile::remove(m_currentVoiceRecordingPath);
        m_currentVoiceRecordingPath.clear();
    }
    clearStagedVoiceMemo(true);
    emit voiceRecordingChanged();
}

void Backend::stopAndSendVoiceRecording() {
    if (!m_recordingVoice && m_hasRecordedVoiceMemo) {
        sendRecordedVoiceMemo();
        return;
    }
    m_recordingVoice = false;
    m_voiceRecordingTimer.stop();
    stopVoiceRecordProcess(true);

    const QString recPath = finalizeCapturedVoiceWav(m_currentVoiceRecordingPath);
    m_currentVoiceRecordingPath.clear();
    if (recPath.isEmpty()) {
        clearStagedVoiceMemo(true);
        emit voiceRecordingChanged();
        return;
    }
    int dur = wavDurationSeconds(recPath);
    if (dur <= 0) dur = qMax(1, m_voiceRecordingSeconds);
    m_recordedVoicePath = recPath;
    m_recordedVoiceDurationSeconds = dur;
    m_recordedVoiceWaveform = computeAudioFileWaveformBars(recPath, recPath, 28);
    m_hasRecordedVoiceMemo = true;
    emit voiceRecordingChanged();
    sendRecordedVoiceMemo();
}

void Backend::sendMessageWithEffect(const QString &text, const QString &effectId) {
    const QString trimmed = replaceEmojiShortcodes(text.trimmed());
    if (m_selectedChatGuid.isEmpty() || trimmed.isEmpty()) return;

    m_recentOutboundEffects.insert(trimmed, effectId);

    const QString tempGuid = QStringLiteral("temp-") + QUuid::createUuid().toString(QUuid::WithoutBraces);

    QVariantMap optimistic;
    optimistic.insert(QStringLiteral("guid"), tempGuid);
    optimistic.insert(QStringLiteral("chatGuid"), m_selectedChatGuid);
    optimistic.insert(QStringLiteral("text"), trimmed);
    optimistic.insert(QStringLiteral("formattedText"), formatMessageTextWithLinks(trimmed));
    optimistic.insert(QStringLiteral("verificationCode"), extractVerificationCode(trimmed));
    optimistic.insert(QStringLiteral("isFromMe"), true);
    optimistic.insert(QStringLiteral("senderName"), QStringLiteral("You"));
    optimistic.insert(QStringLiteral("senderColor"), QStringLiteral("#7aa2f7"));
    optimistic.insert(QStringLiteral("timeText"), QDateTime::currentDateTime().toString(QStringLiteral("h:mm AP")));
    optimistic.insert(QStringLiteral("showDateHeader"), false);
    optimistic.insert(QStringLiteral("dateHeaderText"), QString());
    optimistic.insert(QStringLiteral("reactions"), QVariantList());
    optimistic.insert(QStringLiteral("hasImageAttachment"), false);
    optimistic.insert(QStringLiteral("hasFileAttachment"), false);
    optimistic.insert(QStringLiteral("receiptText"), m_demoMode ? QStringLiteral("Delivered") : QStringLiteral("Sending..."));
    optimistic.insert(QStringLiteral("optimisticCreatedAtMs"), QDateTime::currentMSecsSinceEpoch());
    optimistic.insert(QStringLiteral("expressiveSendStyleId"), effectId);

    QString expressiveEffectName;
    if (effectId.endsWith(QStringLiteral(".invisibleink"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("🕵️ Invisible Ink");
    else if (effectId.endsWith(QStringLiteral(".impact"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("💥 Slam");
    else if (effectId.endsWith(QStringLiteral(".loud"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("📢 Loud");
    else if (effectId.endsWith(QStringLiteral(".gentle"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("🪶 Gentle");
    else if (effectId.contains(QStringLiteral("CKConfettiEffect"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("🎉 Confetti");
    else if (effectId.contains(QStringLiteral("CKLasersEffect"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("✨ Lasers");
    else if (effectId.contains(QStringLiteral("CKFireworksEffect"), Qt::CaseInsensitive)) expressiveEffectName = QStringLiteral("🎆 Fireworks");
    else expressiveEffectName = formatExpressiveEffectLabel(effectId);

    optimistic.insert(QStringLiteral("expressiveEffect"), expressiveEffectName);
    optimistic.insert(QStringLiteral("sendEffectLabel"), formatExpressiveEffectLabel(effectId));

    m_messages.append(optimistic);
    emit messagesChanged();

    m_unreadOverrides.remove(m_selectedChatGuid);
    for (ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) {
            c.lastMessagePreview = trimmed;
            c.lastMessageDate = QDateTime::currentMSecsSinceEpoch();
            c.relativeTime = QStringLiteral("now");
            c.unreadCount = 0;
            m_readWatermarks[m_selectedChatGuid] = qMax(m_readWatermarks.value(m_selectedChatGuid, 0), c.lastMessageDate);
            break;
        }
    }
    sortChats();
    emit chatsChanged();

    if (m_demoMode || m_serverUrl.isEmpty()) return;

    QJsonObject payload;
    payload.insert(QStringLiteral("chatGuid"), m_selectedChatGuid);
    payload.insert(QStringLiteral("tempGuid"), tempGuid);
    payload.insert(QStringLiteral("message"), trimmed);
    payload.insert(QStringLiteral("method"), QStringLiteral("private-api"));
    payload.insert(QStringLiteral("effectId"), effectId);

    QUrl url(m_serverUrl + QStringLiteral("/api/v1/message/text"));
    if (!m_password.isEmpty()) {
        QUrlQuery query(url);
        query.addQueryItem(QStringLiteral("password"), m_password);
        url.setQuery(query);
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(45000);
    QNetworkReply *effReply = m_sendNam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(effReply, &QNetworkReply::finished, effReply, &QNetworkReply::deleteLater);
}

void Backend::scheduleMessage(const QString &text, qint64 sendAtMs) {
    if (text.trimmed().isEmpty() || m_selectedChatGuid.isEmpty()) return;
    const QString scheduleId = QStringLiteral("sched-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap item;
    item.insert(QStringLiteral("id"), scheduleId);
    item.insert(QStringLiteral("scheduleId"), scheduleId);
    item.insert(QStringLiteral("chatGuid"), m_selectedChatGuid);
    item.insert(QStringLiteral("text"), text.trimmed());
    item.insert(QStringLiteral("sendAtMs"), sendAtMs);
    item.insert(QStringLiteral("sendAtDateText"), QDateTime::fromMSecsSinceEpoch(sendAtMs).toString(QStringLiteral("M/d h:mm AP")));
    m_scheduledMessages.append(item);
    emit scheduledMessagesChanged();

    if (!m_demoMode && !m_serverUrl.isEmpty()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("chatGuid"), m_selectedChatGuid);
        payload.insert(QStringLiteral("message"), text.trimmed());
        payload.insert(QStringLiteral("sendAt"), sendAtMs);
        payload.insert(QStringLiteral("scheduleId"), scheduleId);
        QUrl url(m_serverUrl + QStringLiteral("/api/v1/message/schedule"));
        if (!m_password.isEmpty()) {
            QUrlQuery query(url);
            query.addQueryItem(QStringLiteral("password"), m_password);
            url.setQuery(query);
        }
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        m_sendNam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }
}

void Backend::cancelScheduledMessage(const QString &scheduleId) {
    for (int i = 0; i < m_scheduledMessages.size(); ++i) {
        if (m_scheduledMessages[i].toMap().value(QStringLiteral("scheduleId")).toString() == scheduleId ||
            m_scheduledMessages[i].toMap().value(QStringLiteral("id")).toString() == scheduleId) {
            m_scheduledMessages.removeAt(i);
            emit scheduledMessagesChanged();
            break;
        }
    }

    if (!m_demoMode && !m_serverUrl.isEmpty()) {
        QUrl url(m_serverUrl + QStringLiteral("/api/v1/message/schedule/") + scheduleId);
        if (!m_password.isEmpty()) {
            QUrlQuery query(url);
            query.addQueryItem(QStringLiteral("password"), m_password);
            url.setQuery(query);
        }
        QNetworkRequest req(url);
        m_sendNam.deleteResource(req);
    }
}

void Backend::addParticipantToActiveChat(const QString &address) {
    const QString cleanAddr = address.trimmed();
    if (m_selectedChatGuid.isEmpty() || cleanAddr.isEmpty()) return;

    QStringList parts = m_chatParticipants.value(m_selectedChatGuid);
    if (!parts.contains(cleanAddr)) {
        parts.append(cleanAddr);
        m_chatParticipants[m_selectedChatGuid] = parts;
    }
    for (ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) {
            if (!c.participantHandles.contains(cleanAddr)) {
                c.participantHandles.append(cleanAddr);
            }
            break;
        }
    }
    emit chatsChanged();
    emit selectedChatChanged();

    if (!m_demoMode && !m_serverUrl.isEmpty()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("address"), cleanAddr);
        QUrl url(m_serverUrl + QStringLiteral("/api/v1/chat/") + m_selectedChatGuid + QStringLiteral("/participant/add"));
        if (!m_password.isEmpty()) {
            QUrlQuery query(url);
            query.addQueryItem(QStringLiteral("password"), m_password);
            url.setQuery(query);
        }
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        m_sendNam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }
}

void Backend::removeParticipantFromActiveChat(const QString &address) {
    const QString cleanAddr = address.trimmed();
    if (m_selectedChatGuid.isEmpty() || cleanAddr.isEmpty()) return;

    QStringList parts = m_chatParticipants.value(m_selectedChatGuid);
    parts.removeAll(cleanAddr);
    m_chatParticipants[m_selectedChatGuid] = parts;

    for (ChatEntry &c : m_chats) {
        if (c.guid == m_selectedChatGuid) {
            c.participantHandles.removeAll(cleanAddr);
            break;
        }
    }
    emit chatsChanged();
    emit selectedChatChanged();

    if (!m_demoMode && !m_serverUrl.isEmpty()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("address"), cleanAddr);
        QUrl url(m_serverUrl + QStringLiteral("/api/v1/chat/") + m_selectedChatGuid + QStringLiteral("/participant/remove"));
        if (!m_password.isEmpty()) {
            QUrlQuery query(url);
            query.addQueryItem(QStringLiteral("password"), m_password);
            url.setQuery(query);
        }
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        m_sendNam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }
}

void Backend::syncChatStateToServer() {
    saveServerConfig(m_serverUrl, m_secondaryServerUrl, m_password);
}


// -----------------------------------------------------------------------------
// BluePill iCloud Hub (Notes, Calendar, Photos & Attachment Unpurging)
// -----------------------------------------------------------------------------

bool Backend::isSecureOrLocalIcloudEndpoint() const {
    if (m_serverUrl.isEmpty() || m_password.isEmpty()) return false;
    const QUrl url(m_serverUrl);
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("https")) return true;
    if (scheme == QStringLiteral("http")) {
        const QString host = url.host().toLower();
        if (host == QStringLiteral("127.0.0.1") ||
            host == QStringLiteral("localhost") ||
            host == QStringLiteral("::1") ||
            host == QStringLiteral("[::1]")) {
            return true;
        }
    }
    return false;
}

QString Backend::attachmentDownloadUrl(const QString &guid) const {
    if (guid.isEmpty()) return QString();
    const QString cached = lookupCachedAttachmentPath(m_attachmentPaths, guid);
    if (!cached.isEmpty() && QFileInfo::exists(cached) && QFileInfo(cached).size() >= 100) {
        return QUrl::fromLocalFile(cached).toString();
    }
    if (!isSecureOrLocalIcloudEndpoint()) return QString();
    return m_serverUrl + QStringLiteral("/api/v1/attachment/") + QString::fromUtf8(QUrl::toPercentEncoding(guid)) + QStringLiteral("/live?password=") + QString::fromUtf8(QUrl::toPercentEncoding(m_password));
}

void Backend::applyRemoteIcloudNotes(const QJsonArray &data, const QString &acknowledgedNoteId, quint64 acknowledgedMutationSeq) {
    QVariantList merged;
    QSet<QString> seenIds;
    QHash<QString, QVariantMap> prevById;
    for (const QVariant &existingVar : m_icloudNotes) {
        const QVariantMap em = existingVar.toMap();
        const QString eid = em.value(QStringLiteral("id")).toString();
        if (!eid.isEmpty()) prevById.insert(eid, em);
    }

    // Preserve unsynced local note saves ahead of or overriding stale remote versions
    for (auto it = m_pendingIcloudNoteSaves.begin(); it != m_pendingIcloudNoteSaves.end(); ++it) {
        const QString id = it.key();
        if (!id.isEmpty() && !m_pendingIcloudNoteDeletes.contains(id)) {
            merged.append(it.value());
            seenIds.insert(id);
        }
    }

    for (const QJsonValue &v : data) {
        if (!v.isObject()) continue;
        QVariantMap m = v.toObject().toVariantMap();
        const QString id = m.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        if (m_pendingIcloudNoteDeletes.contains(id)) continue;
        if (seenIds.contains(id)) continue;
        const QString syncSource = m.value(QStringLiteral("syncSource")).toString();
        if (syncSource == QStringLiteral("apple_notes_ro")) {
            m.insert(
                QStringLiteral("body"),
                cleanIcloudNoteBody(
                    m.value(QStringLiteral("title")).toString(),
                    m.value(QStringLiteral("body")).toString()
                )
            );
        }
        const qint64 remoteRev = m.value(QStringLiteral("revision")).toLongLong();
        if (remoteRev > 0 && !m.contains(QStringLiteral("baseRevision"))) {
            m.insert(QStringLiteral("baseRevision"), remoteRev);
        }
        if (!acknowledgedNoteId.isEmpty() && id == acknowledgedNoteId && acknowledgedMutationSeq > 0) {
            m.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(acknowledgedMutationSeq));
        } else if (prevById.contains(id)) {
            const QVariantMap prevMap = prevById.value(id);
            const quint64 prevSeq = prevMap.value(QStringLiteral("clientMutationSeq")).toULongLong();
            const qint64 prevRev = qMax(prevMap.value(QStringLiteral("revision")).toLongLong(),
                                        prevMap.value(QStringLiteral("baseRevision")).toLongLong());
            if (prevSeq > 0 && (remoteRev == prevRev || prevRev <= 0)) {
                m.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(prevSeq));
            }
        }
        merged.append(m);
        seenIds.insert(id);
    }

    m_icloudNotes = merged;
    emit icloudNotesChanged();
    saveAppState();
}

void Backend::applyRemoteIcloudEvents(const QJsonArray &data, const QString &acknowledgedEventId) {
    QVariantList merged;
    QSet<QString> seenIds;

    for (auto it = m_pendingIcloudEventSaves.begin(); it != m_pendingIcloudEventSaves.end(); ++it) {
        const QString id = it.key();
        if (!id.isEmpty() && !m_pendingIcloudEventDeletes.contains(id)) {
            QVariantMap m = it.value();
            const QString calTitle = m.value(QStringLiteral("calendarTitle")).toString().trimmed();
            const QString calName = m.value(QStringLiteral("calendarName")).toString().trimmed();
            const QString normCal = !calTitle.isEmpty() ? calTitle : (!calName.isEmpty() ? calName : QStringLiteral("Home"));
            m.insert(QStringLiteral("calendarTitle"), normCal);
            m.insert(QStringLiteral("calendarName"), normCal);
            merged.append(m);
            seenIds.insert(id);
        }
    }

    qint64 minRemoteStartMs = std::numeric_limits<qint64>::max();
    qint64 maxRemoteStartMs = std::numeric_limits<qint64>::min();
    int inWindowCount = 0;

    for (const QJsonValue &v : data) {
        if (!v.isObject()) continue;
        QVariantMap m = v.toObject().toVariantMap();
        const QString id = m.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        const bool isOutOfWindowAck = m.value(QStringLiteral("outsideWindow")).toBool() ||
                                      (!acknowledgedEventId.isEmpty() && id == acknowledgedEventId);
        m.remove(QStringLiteral("outsideWindow"));
        const qint64 sMs = m.value(QStringLiteral("startMs")).toLongLong();
        if (!isOutOfWindowAck) {
            if (sMs < minRemoteStartMs) minRemoteStartMs = sMs;
            if (sMs > maxRemoteStartMs) maxRemoteStartMs = sMs;
            ++inWindowCount;
        }
        if (m_pendingIcloudEventDeletes.contains(id)) continue;
        if (seenIds.contains(id)) continue;
        const QString calTitle = m.value(QStringLiteral("calendarTitle")).toString().trimmed();
        const QString calName = m.value(QStringLiteral("calendarName")).toString().trimmed();
        const QString normCal = !calTitle.isEmpty() ? calTitle : (!calName.isEmpty() ? calName : QStringLiteral("Home"));
        m.insert(QStringLiteral("calendarTitle"), normCal);
        m.insert(QStringLiteral("calendarName"), normCal);
        merged.append(m);
        seenIds.insert(id);
    }

    // If the relay response was capped at its 250-event window, preserve locally known events
    // whose startMs falls strictly outside the returned [minRemoteStartMs, maxRemoteStartMs] window.
    if ((data.size() >= 250 || inWindowCount >= 249) && minRemoteStartMs <= maxRemoteStartMs) {
        for (const QVariant &existingVar : m_icloudEvents) {
            const QVariantMap existingMap = existingVar.toMap();
            const QString existingId = existingMap.value(QStringLiteral("id")).toString();
            if (existingId.isEmpty() || seenIds.contains(existingId) || m_pendingIcloudEventDeletes.contains(existingId)) {
                continue;
            }
            const qint64 existingStartMs = existingMap.value(QStringLiteral("startMs")).toLongLong();
            if (existingStartMs < minRemoteStartMs || existingStartMs > maxRemoteStartMs) {
                merged.append(existingMap);
                seenIds.insert(existingId);
            }
        }
    }

    m_icloudEvents = merged;
    emit icloudEventsChanged();
    saveAppState();
}

bool Backend::isIcloudMutationInBackoffForTest(const QString &mutationKey) const {
    if (m_icloudMutationPausedKeys.contains(mutationKey)) return true;
    const qint64 nextMs = m_icloudMutationNextRetryMs.value(mutationKey, 0);
    return nextMs > QDateTime::currentMSecsSinceEpoch();
}

void Backend::clearIcloudMutationRetryState(const QString &mutationKey) {
    m_icloudMutationRetryCounts.remove(mutationKey);
    m_icloudMutationNextRetryMs.remove(mutationKey);
    m_icloudMutationPausedKeys.remove(mutationKey);
}

void Backend::recordIcloudMutationFailure(const QString &mutationKey) {
    const int count = m_icloudMutationRetryCounts.value(mutationKey, 0);
    if (count >= 5) {
        m_icloudMutationPausedKeys.insert(mutationKey);
        m_icloudMutationNextRetryMs.remove(mutationKey);
        return;
    }
    const int delayMs = qMin(30000, 1000 * (1 << count));
    m_icloudMutationRetryCounts.insert(mutationKey, count + 1);
    m_icloudMutationNextRetryMs.insert(mutationKey, QDateTime::currentMSecsSinceEpoch() + delayMs);
    scheduleIcloudMutationRetry(false);
}

void Backend::scheduleIcloudMutationRetry(bool immediate) {
    if (immediate) {
        replayPendingIcloudMutations();
        return;
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    qint64 earliestNextMs = std::numeric_limits<qint64>::max();
    for (auto it = m_icloudMutationNextRetryMs.begin(); it != m_icloudMutationNextRetryMs.end(); ++it) {
        if (m_icloudMutationPausedKeys.contains(it.key())) continue;
        if (it.value() <= nowMs) continue; // already due: dispatched, in flight, or gated
        if (it.value() < earliestNextMs) {
            earliestNextMs = it.value();
        }
    }
    if (earliestNextMs == std::numeric_limits<qint64>::max()) {
        return;
    }
    const int delayMs = static_cast<int>(qBound(qint64(50), earliestNextMs - nowMs, qint64(30000)));
    const qint64 targetDeadlineMs = nowMs + delayMs;
    if (m_icloudMutationRetryScheduled && m_icloudMutationRetryArmedDeadlineMs > 0 &&
        m_icloudMutationRetryArmedDeadlineMs <= targetDeadlineMs) {
        return;
    }
    m_icloudMutationRetryScheduled = true;
    m_icloudMutationRetryArmedDeadlineMs = targetDeadlineMs;
    m_icloudMutationRetryTimer.start(delayMs);
}

void Backend::replayPendingIcloudMutations() {
    if (!isSecureOrLocalIcloudEndpoint()) return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    auto extractServerItemRevision = [](const QJsonArray &arr, const QString &itemId) -> qint64 {
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("id")).toString() == itemId) {
                return o.value(QStringLiteral("revision")).toVariant().toLongLong();
            }
        }
        return 0;
    };

    auto isNoteReplayAlreadyApplied = [](const QJsonArray &arr, const QString &itemId, const QVariantMap &pendingMap) -> bool {
        const QString wantTitle = pendingMap.value(QStringLiteral("title")).toString().trimmed();
        const QString wantBody = pendingMap.value(QStringLiteral("body")).toString().trimmed();
        const QString wantFolder = pendingMap.value(QStringLiteral("folder"), QStringLiteral("Notes")).toString().trimmed();
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("id")).toString() != itemId) continue;
            const QString srvTitle = o.value(QStringLiteral("title")).toString().trimmed();
            const QString srvBody = o.value(QStringLiteral("body")).toString().trimmed();
            const QString srvFolder = o.value(QStringLiteral("folder")).toString().trimmed();
            return srvTitle == wantTitle && srvBody == wantBody &&
                   (srvFolder == wantFolder || (srvFolder.isEmpty() && wantFolder == QStringLiteral("Notes")));
        }
        return false;
    };

    auto isEventReplayAlreadyApplied = [](const QJsonArray &arr, const QString &itemId, const QVariantMap &pendingMap) -> bool {
        const QString wantTitle = pendingMap.value(QStringLiteral("title")).toString().trimmed();
        const qint64 wantStart = pendingMap.value(QStringLiteral("startMs")).toLongLong();
        const qint64 wantEnd = pendingMap.value(QStringLiteral("endMs")).toLongLong();
        const QString wantCal = pendingMap.value(
            QStringLiteral("calendarTitle"),
            pendingMap.value(QStringLiteral("calendarName"), QStringLiteral("Home"))
        ).toString().trimmed();
        const QString wantNotes = pendingMap.value(QStringLiteral("notes")).toString().trimmed();
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("id")).toString() != itemId) continue;
            const QString srvTitle = o.value(QStringLiteral("title")).toString().trimmed();
            const qint64 srvStart = o.value(QStringLiteral("startMs")).toVariant().toLongLong();
            const qint64 srvEnd = o.value(QStringLiteral("endMs")).toVariant().toLongLong();
            const QString srvCal = (o.value(QStringLiteral("calendarTitle")).toString().trimmed().isEmpty()
                                        ? o.value(QStringLiteral("calendarName")).toString().trimmed()
                                        : o.value(QStringLiteral("calendarTitle")).toString().trimmed());
            const QString srvNotes = o.value(QStringLiteral("notes")).toString().trimmed();
            return srvTitle == wantTitle && srvStart == wantStart && srvEnd == wantEnd &&
                   srvCal == wantCal && srvNotes == wantNotes;
        }
        return false;
    };

    const auto noteSaves = m_pendingIcloudNoteSaves;
    for (auto it = noteSaves.begin(); it != noteSaves.end(); ++it) {
        const QString noteId = it.key();
        if (m_pendingIcloudNoteDeletes.contains(noteId)) continue;
        if (m_inFlightIcloudNoteReplies.value(noteId)) continue;
        const QString mutKey = QStringLiteral("note_save:") + noteId;
        if (m_icloudMutationPausedKeys.contains(mutKey)) continue;
        const QVariantMap noteMap = it.value();
        if (noteMap.value(QStringLiteral("hasServerConflict")).toBool() ||
            noteMap.value(QStringLiteral("conflictServerRevision")).toLongLong() >
                noteMap.value(QStringLiteral("baseRevision")).toLongLong()) {
            continue;
        }
        if (m_icloudMutationNextRetryMs.value(mutKey, 0) > nowMs) {
            scheduleIcloudMutationRetry(false);
            continue;
        }
        m_icloudMutationNextRetryMs.remove(mutKey);
        QVariantMap dispatchedNoteMap = noteMap;
        dispatchedNoteMap.insert(QStringLiteral("neverDispatchedLocalCreate"), false);
        dispatchedNoteMap.insert(QStringLiteral("dispatchAttempted"), true);
        m_pendingIcloudNoteSaves.insert(noteId, dispatchedNoteMap);
        if (!m_stateFilePath.isEmpty() && !saveAppState()) {
            recordIcloudMutationFailure(mutKey);
            continue;
        }
        const quint64 sentSeq = dispatchedNoteMap.value(QStringLiteral("clientMutationSeq")).toULongLong();
        const qint64 sentUpdatedAt = dispatchedNoteMap.value(QStringLiteral("updatedAtMs")).toLongLong();
        QJsonObject payload = QJsonObject::fromVariantMap(dispatchedNoteMap);
        payload.remove(QStringLiteral("neverDispatchedLocalCreate"));
        payload.remove(QStringLiteral("dispatchAttempted"));
        const quint64 gen = ++m_icloudNotesGeneration;
        QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/icloud/notes")), QJsonDocument(payload).toJson(QJsonDocument::Compact));
        m_inFlightIcloudNoteReplies.insert(noteId, reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, noteId, mutKey, sentSeq, sentUpdatedAt, gen, extractServerItemRevision, isNoteReplayAlreadyApplied]() {
            if (m_inFlightIcloudNoteReplies.value(noteId) == reply) {
                m_inFlightIcloudNoteReplies.remove(noteId);
            }
            reply->deleteLater();
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (m_pendingIcloudNoteSaves.contains(noteId)) {
                    recordIcloudMutationFailure(mutKey);
                } else if (m_pendingIcloudNoteDeletes.contains(noteId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QVariantMap curPending = m_pendingIcloudNoteSaves.value(noteId);
            const quint64 curSeq = curPending.value(QStringLiteral("clientMutationSeq")).toULongLong();
            const bool sameMutation = (sentSeq > 0 || curSeq > 0)
                                          ? (sentSeq > 0 && curSeq == sentSeq)
                                          : (curPending.value(QStringLiteral("updatedAtMs")).toLongLong() == sentUpdatedAt);
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && !m_pendingIcloudNoteDeletes.contains(noteId)) {
                if (doc.value(QStringLiteral("status")).toInt() == 200 && doc.value(QStringLiteral("data")).isArray()) {
                    const QJsonArray dataArr = doc.value(QStringLiteral("data")).toArray();
                    ++m_icloudNotesSnapshotEpoch;
                    clearIcloudMutationRetryState(mutKey);
                    const qint64 ackRev = extractServerItemRevision(dataArr, noteId);
                    if (sameMutation) {
                        m_pendingIcloudNoteSaves.remove(noteId);
                    } else if (m_pendingIcloudNoteSaves.contains(noteId) && ackRev > 0) {
                        QVariantMap nextMap = m_pendingIcloudNoteSaves.value(noteId);
                        nextMap.insert(QStringLiteral("baseRevision"), ackRev);
                        nextMap.insert(QStringLiteral("revision"), ackRev);
                        m_pendingIcloudNoteSaves.insert(noteId, nextMap);
                    }
                    if (ackRev > 0) {
                        for (int i = 0; i < m_icloudNotes.size(); ++i) {
                            QVariantMap uiMap = m_icloudNotes.at(i).toMap();
                            if (uiMap.value(QStringLiteral("id")).toString() == noteId) {
                                uiMap.insert(QStringLiteral("baseRevision"), ackRev);
                                uiMap.insert(QStringLiteral("revision"), ackRev);
                                if (sameMutation && sentSeq > 0) {
                                    uiMap.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(sentSeq));
                                }
                                m_icloudNotes[i] = uiMap;
                                break;
                            }
                        }
                    }
                    if (gen == m_icloudNotesGeneration) {
                        applyRemoteIcloudNotes(dataArr, noteId, sameMutation ? sentSeq : 0);
                    } else {
                        emit icloudNotesChanged();
                        saveAppState();
                    }
                    scheduleIcloudMutationRetry(true);
                    return;
                }
            }
            if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                ++m_icloudNotesSnapshotEpoch;
                if (sameMutation && m_pendingIcloudNoteSaves.contains(noteId)) {
                    if (httpStatus == 409 && doc.value(QStringLiteral("data")).isArray() &&
                        isNoteReplayAlreadyApplied(doc.value(QStringLiteral("data")).toArray(), noteId, curPending)) {
                        const QJsonArray dataArr = doc.value(QStringLiteral("data")).toArray();
                        const qint64 ackRev = extractServerItemRevision(dataArr, noteId);
                        clearIcloudMutationRetryState(mutKey);
                        m_pendingIcloudNoteSaves.remove(noteId);
                        if (ackRev > 0) {
                            for (int i = 0; i < m_icloudNotes.size(); ++i) {
                                QVariantMap uiMap = m_icloudNotes.at(i).toMap();
                                if (uiMap.value(QStringLiteral("id")).toString() == noteId) {
                                    uiMap.insert(QStringLiteral("baseRevision"), ackRev);
                                    uiMap.insert(QStringLiteral("revision"), ackRev);
                                    if (sentSeq > 0) {
                                        uiMap.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(sentSeq));
                                    }
                                    m_icloudNotes[i] = uiMap;
                                    break;
                                }
                            }
                        }
                        if (gen == m_icloudNotesGeneration) {
                            applyRemoteIcloudNotes(dataArr, noteId, sentSeq);
                        } else {
                            emit icloudNotesChanged();
                            saveAppState();
                        }
                        scheduleIcloudMutationRetry(true);
                        return;
                    }
                    m_icloudMutationPausedKeys.insert(mutKey);
                    m_icloudMutationNextRetryMs.remove(mutKey);
                    if (httpStatus == 409 && doc.value(QStringLiteral("data")).isArray()) {
                        const QJsonArray conflictArr = doc.value(QStringLiteral("data")).toArray();
                        const qint64 srvRev = extractServerItemRevision(conflictArr, noteId);
                        QString srvTitle;
                        QString srvBody;
                        QString srvFolder = QStringLiteral("Notes");
                        for (const QJsonValue &cv : conflictArr) {
                            if (cv.isObject() && cv.toObject().value(QStringLiteral("id")).toString() == noteId) {
                                const QJsonObject co = cv.toObject();
                                srvTitle = co.value(QStringLiteral("title")).toString();
                                srvBody = co.value(QStringLiteral("body")).toString();
                                srvFolder = co.value(QStringLiteral("folder")).toString(QStringLiteral("Notes"));
                                break;
                            }
                        }
                        QVariantMap pend = m_pendingIcloudNoteSaves.value(noteId);
                        pend.insert(QStringLiteral("conflictServerRevision"), srvRev);
                        pend.insert(QStringLiteral("conflictServerTitle"), srvTitle);
                        pend.insert(QStringLiteral("conflictServerBody"), srvBody);
                        pend.insert(QStringLiteral("conflictServerFolder"), srvFolder);
                        pend.insert(QStringLiteral("hasServerConflict"), true);
                        m_pendingIcloudNoteSaves.insert(noteId, pend);
                        for (int i = 0; i < m_icloudNotes.size(); ++i) {
                            QVariantMap uiMap = m_icloudNotes.at(i).toMap();
                            if (uiMap.value(QStringLiteral("id")).toString() == noteId) {
                                uiMap.insert(QStringLiteral("conflictServerRevision"), srvRev);
                                uiMap.insert(QStringLiteral("conflictServerTitle"), srvTitle);
                                uiMap.insert(QStringLiteral("conflictServerBody"), srvBody);
                                uiMap.insert(QStringLiteral("conflictServerFolder"), srvFolder);
                                uiMap.insert(QStringLiteral("hasServerConflict"), true);
                                m_icloudNotes[i] = uiMap;
                                break;
                            }
                        }
                        emit icloudNotesChanged();
                        emit toastMessage(QStringLiteral("Note revision conflict on server; your local draft is preserved."));
                    } else if (httpStatus == 401 || httpStatus == 403) {
                        emit toastMessage(QStringLiteral("iCloud Hub authentication required; local note draft preserved."));
                    }
                    saveAppState();
                }
                if ((!sameMutation && m_pendingIcloudNoteSaves.contains(noteId)) ||
                    m_pendingIcloudNoteDeletes.contains(noteId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && m_pendingIcloudNoteDeletes.contains(noteId)) {
                ++m_icloudNotesSnapshotEpoch;
                clearIcloudMutationRetryState(mutKey);
                scheduleIcloudMutationRetry(true);
                return;
            }
            recordIcloudMutationFailure(mutKey);
        });
    }

    const QSet<QString> noteDeletes = m_pendingIcloudNoteDeletes;
    for (const QString &noteId : noteDeletes) {
        if (m_inFlightIcloudNoteReplies.value(noteId)) continue;
        const QString mutKey = QStringLiteral("note_del:") + noteId;
        if (m_icloudMutationPausedKeys.contains(mutKey)) continue;
        if (m_icloudMutationNextRetryMs.value(mutKey, 0) > nowMs) {
            scheduleIcloudMutationRetry(false);
            continue;
        }
        m_icloudMutationNextRetryMs.remove(mutKey);
        const quint64 gen = ++m_icloudNotesGeneration;
        const QString encodedId = QString::fromUtf8(QUrl::toPercentEncoding(noteId));
        QNetworkReply *reply = m_nam.deleteResource(makeRequest(QStringLiteral("/api/v1/icloud/notes/") + encodedId));
        m_inFlightIcloudNoteReplies.insert(noteId, reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, noteId, mutKey, gen]() {
            if (m_inFlightIcloudNoteReplies.value(noteId) == reply) {
                m_inFlightIcloudNoteReplies.remove(noteId);
            }
            reply->deleteLater();
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (m_pendingIcloudNoteDeletes.contains(noteId)) {
                    recordIcloudMutationFailure(mutKey);
                } else if (m_pendingIcloudNoteSaves.contains(noteId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && !m_pendingIcloudNoteSaves.contains(noteId)) {
                if (doc.value(QStringLiteral("status")).toInt() == 200 && doc.value(QStringLiteral("data")).isArray()) {
                    ++m_icloudNotesSnapshotEpoch;
                    clearIcloudMutationRetryState(mutKey);
                    m_pendingIcloudNoteDeletes.remove(noteId);
                    if (gen == m_icloudNotesGeneration) {
                        applyRemoteIcloudNotes(doc.value(QStringLiteral("data")).toArray());
                    } else {
                        saveAppState();
                    }
                    scheduleIcloudMutationRetry(true);
                    return;
                }
            }
            if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                ++m_icloudNotesSnapshotEpoch;
                const bool isAuthFailure = (httpStatus == 401) || (httpStatus == 403 && !doc.value(QStringLiteral("data")).isArray());
                if (isAuthFailure) {
                    m_icloudMutationPausedKeys.insert(mutKey);
                    m_icloudMutationNextRetryMs.remove(mutKey);
                    emit toastMessage(QStringLiteral("iCloud Hub authentication required; pending note deletion preserved."));
                    saveAppState();
                } else {
                    clearIcloudMutationRetryState(mutKey);
                    m_pendingIcloudNoteDeletes.remove(noteId);
                    if (doc.value(QStringLiteral("data")).isArray() && gen == m_icloudNotesGeneration) {
                        applyRemoteIcloudNotes(doc.value(QStringLiteral("data")).toArray());
                    } else {
                        saveAppState();
                    }
                }
                if (m_pendingIcloudNoteSaves.contains(noteId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && m_pendingIcloudNoteSaves.contains(noteId)) {
                ++m_icloudNotesSnapshotEpoch;
                clearIcloudMutationRetryState(mutKey);
                scheduleIcloudMutationRetry(true);
                return;
            }
            recordIcloudMutationFailure(mutKey);
        });
    }

    const auto eventSaves = m_pendingIcloudEventSaves;
    for (auto it = eventSaves.begin(); it != eventSaves.end(); ++it) {
        const QString evtId = it.key();
        if (m_pendingIcloudEventDeletes.contains(evtId)) continue;
        if (m_inFlightIcloudEventReplies.value(evtId)) continue;
        const QString mutKey = QStringLiteral("evt_save:") + evtId;
        if (m_icloudMutationPausedKeys.contains(mutKey)) continue;
        const QVariantMap evtMap = it.value();
        if (evtMap.value(QStringLiteral("conflictServerRevision")).toLongLong() >
            evtMap.value(QStringLiteral("baseRevision")).toLongLong()) {
            continue;
        }
        if (m_icloudMutationNextRetryMs.value(mutKey, 0) > nowMs) {
            scheduleIcloudMutationRetry(false);
            continue;
        }
        m_icloudMutationNextRetryMs.remove(mutKey);
        QVariantMap dispatchedEvtMap = evtMap;
        dispatchedEvtMap.insert(QStringLiteral("neverDispatchedLocalCreate"), false);
        dispatchedEvtMap.insert(QStringLiteral("dispatchAttempted"), true);
        m_pendingIcloudEventSaves.insert(evtId, dispatchedEvtMap);
        if (!m_stateFilePath.isEmpty() && !saveAppState()) {
            recordIcloudMutationFailure(mutKey);
            continue;
        }
        const quint64 sentSeq = dispatchedEvtMap.value(QStringLiteral("clientMutationSeq")).toULongLong();
        const qint64 sentUpdatedAt = dispatchedEvtMap.value(QStringLiteral("updatedAtMs")).toLongLong();
        QJsonObject payload = QJsonObject::fromVariantMap(dispatchedEvtMap);
        payload.remove(QStringLiteral("neverDispatchedLocalCreate"));
        payload.remove(QStringLiteral("dispatchAttempted"));
        payload.insert(QStringLiteral("calendarName"), dispatchedEvtMap.value(QStringLiteral("calendarTitle")).toString());
        const quint64 gen = ++m_icloudEventsGeneration;
        QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/icloud/events")), QJsonDocument(payload).toJson(QJsonDocument::Compact));
        m_inFlightIcloudEventReplies.insert(evtId, reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, evtId, mutKey, sentSeq, sentUpdatedAt, gen, extractServerItemRevision, isEventReplayAlreadyApplied]() {
            if (m_inFlightIcloudEventReplies.value(evtId) == reply) {
                m_inFlightIcloudEventReplies.remove(evtId);
            }
            reply->deleteLater();
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (m_pendingIcloudEventSaves.contains(evtId)) {
                    recordIcloudMutationFailure(mutKey);
                } else if (m_pendingIcloudEventDeletes.contains(evtId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QVariantMap curPending = m_pendingIcloudEventSaves.value(evtId);
            const quint64 curSeq = curPending.value(QStringLiteral("clientMutationSeq")).toULongLong();
            const bool sameMutation = (sentSeq > 0 || curSeq > 0)
                                          ? (sentSeq > 0 && curSeq == sentSeq)
                                          : (curPending.value(QStringLiteral("updatedAtMs")).toLongLong() == sentUpdatedAt);
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && !m_pendingIcloudEventDeletes.contains(evtId)) {
                if (doc.value(QStringLiteral("status")).toInt() == 200 && doc.value(QStringLiteral("data")).isArray()) {
                    const QJsonArray dataArr = doc.value(QStringLiteral("data")).toArray();
                    ++m_icloudEventsSnapshotEpoch;
                    clearIcloudMutationRetryState(mutKey);
                    const qint64 ackRev = extractServerItemRevision(dataArr, evtId);
                    if (sameMutation) {
                        m_pendingIcloudEventSaves.remove(evtId);
                    } else if (m_pendingIcloudEventSaves.contains(evtId) && ackRev > 0) {
                        QVariantMap nextMap = m_pendingIcloudEventSaves.value(evtId);
                        nextMap.insert(QStringLiteral("baseRevision"), ackRev);
                        nextMap.insert(QStringLiteral("revision"), ackRev);
                        m_pendingIcloudEventSaves.insert(evtId, nextMap);
                    }
                    if (ackRev > 0) {
                        for (int i = 0; i < m_icloudEvents.size(); ++i) {
                            QVariantMap uiMap = m_icloudEvents.at(i).toMap();
                            if (uiMap.value(QStringLiteral("id")).toString() == evtId) {
                                uiMap.insert(QStringLiteral("baseRevision"), ackRev);
                                uiMap.insert(QStringLiteral("revision"), ackRev);
                                m_icloudEvents[i] = uiMap;
                                break;
                            }
                        }
                    }
                    if (gen == m_icloudEventsGeneration) {
                        applyRemoteIcloudEvents(dataArr, evtId);
                    } else {
                        saveAppState();
                    }
                    scheduleIcloudMutationRetry(true);
                    return;
                }
            }
            if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                ++m_icloudEventsSnapshotEpoch;
                if (sameMutation && m_pendingIcloudEventSaves.contains(evtId)) {
                    if (httpStatus == 409 && doc.value(QStringLiteral("data")).isArray() &&
                        isEventReplayAlreadyApplied(doc.value(QStringLiteral("data")).toArray(), evtId, curPending)) {
                        clearIcloudMutationRetryState(mutKey);
                        m_pendingIcloudEventSaves.remove(evtId);
                        if (gen == m_icloudEventsGeneration) {
                            applyRemoteIcloudEvents(doc.value(QStringLiteral("data")).toArray(), evtId);
                        } else {
                            saveAppState();
                        }
                        scheduleIcloudMutationRetry(true);
                        return;
                    }
                    m_icloudMutationPausedKeys.insert(mutKey);
                    m_icloudMutationNextRetryMs.remove(mutKey);
                    if (httpStatus == 409 && doc.value(QStringLiteral("data")).isArray()) {
                        const qint64 srvRev = extractServerItemRevision(doc.value(QStringLiteral("data")).toArray(), evtId);
                        if (srvRev > 0) {
                            QVariantMap pend = m_pendingIcloudEventSaves.value(evtId);
                            pend.insert(QStringLiteral("conflictServerRevision"), srvRev);
                            m_pendingIcloudEventSaves.insert(evtId, pend);
                        }
                        emit toastMessage(QStringLiteral("Event revision conflict on server; your local draft is preserved."));
                    } else if (httpStatus == 401 || httpStatus == 403) {
                        emit toastMessage(QStringLiteral("iCloud Hub authentication required; local event draft preserved."));
                    }
                    saveAppState();
                }
                if ((!sameMutation && m_pendingIcloudEventSaves.contains(evtId)) ||
                    m_pendingIcloudEventDeletes.contains(evtId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && m_pendingIcloudEventDeletes.contains(evtId)) {
                ++m_icloudEventsSnapshotEpoch;
                clearIcloudMutationRetryState(mutKey);
                scheduleIcloudMutationRetry(true);
                return;
            }
            recordIcloudMutationFailure(mutKey);
        });
    }

    const QSet<QString> eventDeletes = m_pendingIcloudEventDeletes;
    for (const QString &evtId : eventDeletes) {
        if (m_inFlightIcloudEventReplies.value(evtId)) continue;
        const QString mutKey = QStringLiteral("evt_del:") + evtId;
        if (m_icloudMutationPausedKeys.contains(mutKey)) continue;
        if (m_icloudMutationNextRetryMs.value(mutKey, 0) > nowMs) {
            scheduleIcloudMutationRetry(false);
            continue;
        }
        m_icloudMutationNextRetryMs.remove(mutKey);
        const quint64 gen = ++m_icloudEventsGeneration;
        const QString encodedId = QString::fromUtf8(QUrl::toPercentEncoding(evtId));
        QNetworkReply *reply = m_nam.deleteResource(makeRequest(QStringLiteral("/api/v1/icloud/events/") + encodedId));
        m_inFlightIcloudEventReplies.insert(evtId, reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, evtId, mutKey, gen]() {
            if (m_inFlightIcloudEventReplies.value(evtId) == reply) {
                m_inFlightIcloudEventReplies.remove(evtId);
            }
            reply->deleteLater();
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (m_pendingIcloudEventDeletes.contains(evtId)) {
                    recordIcloudMutationFailure(mutKey);
                } else if (m_pendingIcloudEventSaves.contains(evtId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && !m_pendingIcloudEventSaves.contains(evtId)) {
                if (doc.value(QStringLiteral("status")).toInt() == 200 && doc.value(QStringLiteral("data")).isArray()) {
                    ++m_icloudEventsSnapshotEpoch;
                    clearIcloudMutationRetryState(mutKey);
                    m_pendingIcloudEventDeletes.remove(evtId);
                    if (gen == m_icloudEventsGeneration) {
                        applyRemoteIcloudEvents(doc.value(QStringLiteral("data")).toArray());
                    } else {
                        saveAppState();
                    }
                    scheduleIcloudMutationRetry(true);
                    return;
                }
            }
            if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                ++m_icloudEventsSnapshotEpoch;
                const bool isAuthFailure = (httpStatus == 401) || (httpStatus == 403 && !doc.value(QStringLiteral("data")).isArray());
                if (isAuthFailure) {
                    m_icloudMutationPausedKeys.insert(mutKey);
                    m_icloudMutationNextRetryMs.remove(mutKey);
                    emit toastMessage(QStringLiteral("iCloud Hub authentication required; pending event deletion preserved."));
                    saveAppState();
                } else {
                    clearIcloudMutationRetryState(mutKey);
                    m_pendingIcloudEventDeletes.remove(evtId);
                    if (doc.value(QStringLiteral("data")).isArray() && gen == m_icloudEventsGeneration) {
                        applyRemoteIcloudEvents(doc.value(QStringLiteral("data")).toArray());
                    } else {
                        saveAppState();
                    }
                }
                if (m_pendingIcloudEventSaves.contains(evtId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && m_pendingIcloudEventSaves.contains(evtId)) {
                ++m_icloudEventsSnapshotEpoch;
                clearIcloudMutationRetryState(mutKey);
                scheduleIcloudMutationRetry(true);
                return;
            }
            recordIcloudMutationFailure(mutKey);
        });
    }
}

void Backend::setIcloudNotesEnabled(bool enabled) {
    if (m_icloudNotesEnabled == enabled) return;
    m_icloudNotesEnabled = enabled;
    emit icloudHubModulesChanged();
    saveAppState();
    if (m_icloudNotesEnabled && !m_serverUrl.isEmpty()) {
        fetchIcloudNotes();
    }
}

void Backend::setIcloudCalendarEnabled(bool enabled) {
    if (m_icloudCalendarEnabled == enabled) return;
    m_icloudCalendarEnabled = enabled;
    emit icloudHubModulesChanged();
    saveAppState();
    if (m_icloudCalendarEnabled && !m_serverUrl.isEmpty()) {
        fetchIcloudEvents();
    }
}

void Backend::setIcloudPhotosEnabled(bool enabled) {
    if (m_icloudPhotosEnabled == enabled) return;
    m_icloudPhotosEnabled = enabled;
    emit icloudHubModulesChanged();
    saveAppState();
    if (m_icloudPhotosEnabled && !m_serverUrl.isEmpty()) {
        fetchIcloudPhotos();
    }
}

void Backend::fetchIcloudNotes() {
    if (!m_icloudNotesEnabled) return;
    if (!isSecureOrLocalIcloudEndpoint()) return;
    const quint64 gen = m_icloudNotesGeneration;
    const quint64 snapEpoch = m_icloudNotesSnapshotEpoch;
    QNetworkReply *reply = m_nam.get(makeRequest(QStringLiteral("/api/v1/icloud/notes")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, snapEpoch]() {
        reply->deleteLater();
        if (gen != m_icloudNotesGeneration || snapEpoch != m_icloudNotesSnapshotEpoch) return;
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonValue dataVal = doc.value(QStringLiteral("data"));
            if (doc.value(QStringLiteral("status")).toInt() == 200 && dataVal.isArray()) {
                applyRemoteIcloudNotes(dataVal.toArray());
            }
        }
    });
}

QString Backend::saveIcloudNote(const QString &id, const QString &title, const QString &body, const QString &folder, qint64 baseRevision, bool allowRecreate) {
    QString actualId = id.trimmed();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const quint64 gen = ++m_icloudNotesGeneration;
    if (actualId.isEmpty()) {
        actualId = QStringLiteral("note_%1_%2").arg(now).arg(gen);
    }

    qint64 baseRev = 0;
    if (baseRevision >= 0) {
        baseRev = baseRevision;
    } else {
        if (m_pendingIcloudNoteSaves.contains(actualId)) {
            const QVariantMap pendMap = m_pendingIcloudNoteSaves.value(actualId);
            baseRev = pendMap.value(QStringLiteral("baseRevision")).toLongLong();
        }
        if (baseRev <= 0) {
            for (int i = 0; i < m_icloudNotes.size(); ++i) {
                const QVariantMap existingMap = m_icloudNotes.at(i).toMap();
                if (existingMap.value(QStringLiteral("id")).toString() == actualId) {
                    baseRev = qMax(
                        existingMap.value(QStringLiteral("baseRevision")).toLongLong(),
                        existingMap.value(QStringLiteral("revision")).toLongLong()
                    );
                    break;
                }
            }
        }
    }

    const QVariantMap prevPendNote = m_pendingIcloudNoteSaves.value(actualId);
    const qint64 conflictSrvRev = prevPendNote.value(QStringLiteral("conflictServerRevision")).toLongLong();
    const bool prevHasConflict = prevPendNote.value(QStringLiteral("hasServerConflict")).toBool();
    if (!id.trimmed().isEmpty() &&
        ((conflictSrvRev > 0 && baseRev < conflictSrvRev) ||
         (prevHasConflict && conflictSrvRev <= 0 && !allowRecreate))) {
        QVariantMap updatedPend = prevPendNote;
        updatedPend.insert(QStringLiteral("title"), title);
        updatedPend.insert(QStringLiteral("body"), body);
        updatedPend.insert(QStringLiteral("folder"), folder.isEmpty() ? QStringLiteral("Notes") : folder);
        updatedPend.insert(QStringLiteral("updatedAtMs"), now);
        m_pendingIcloudNoteSaves.insert(actualId, updatedPend);
        for (int i = 0; i < m_icloudNotes.size(); ++i) {
            if (m_icloudNotes.at(i).toMap().value(QStringLiteral("id")).toString() == actualId) {
                m_icloudNotes[i] = updatedPend;
                break;
            }
        }
        const QString pausedKey = QStringLiteral("note_save:") + actualId;
        m_icloudMutationPausedKeys.insert(pausedKey);
        m_icloudMutationNextRetryMs.remove(pausedKey);
        const bool conflictStateSaved = m_stateFilePath.isEmpty() || saveAppState();
        updatedPend.insert(QStringLiteral("statePersisted"), conflictStateSaved);
        m_pendingIcloudNoteSaves.insert(actualId, updatedPend);
        for (int i = 0; i < m_icloudNotes.size(); ++i) {
            if (m_icloudNotes.at(i).toMap().value(QStringLiteral("id")).toString() == actualId) {
                m_icloudNotes[i] = updatedPend;
                break;
            }
        }
        emit icloudNotesChanged();
        emit toastMessage(QStringLiteral("Note conflict is paused. Choose Reload Server Version, Save as Copy, or Overwrite Server."));
        return actualId;
    }
    const bool alreadyDispatched = prevPendNote.value(QStringLiteral("dispatchAttempted")).toBool();
    const bool wasNeverDispatchedCreate = id.trimmed().isEmpty()
                                              ? true
                                              : (m_pendingIcloudNoteSaves.contains(actualId) &&
                                                 prevPendNote.value(QStringLiteral("neverDispatchedLocalCreate")).toBool() &&
                                                 !alreadyDispatched);

    QVariantMap noteMap;
    noteMap.insert(QStringLiteral("id"), actualId);
    noteMap.insert(QStringLiteral("title"), title);
    noteMap.insert(QStringLiteral("body"), body);
    noteMap.insert(QStringLiteral("folder"), folder.isEmpty() ? QStringLiteral("Notes") : folder);
    noteMap.insert(QStringLiteral("syncSource"), QStringLiteral("local_hub"));
    noteMap.insert(QStringLiteral("updatedAtMs"), now);
    noteMap.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(gen));
    noteMap.insert(QStringLiteral("neverDispatchedLocalCreate"), wasNeverDispatchedCreate && !alreadyDispatched && baseRev <= 0);
    if (alreadyDispatched) {
        noteMap.insert(QStringLiteral("dispatchAttempted"), true);
    }
    if (baseRev > 0) {
        noteMap.insert(QStringLiteral("baseRevision"), baseRev);
        noteMap.insert(QStringLiteral("revision"), baseRev);
    }
    if (allowRecreate) {
        noteMap.insert(QStringLiteral("allowRecreate"), true);
    }

    const QString mutKey = QStringLiteral("note_save:") + actualId;
    clearIcloudMutationRetryState(mutKey);
    clearIcloudMutationRetryState(QStringLiteral("note_del:") + actualId);
    m_pendingIcloudNoteDeletes.remove(actualId);
    m_pendingIcloudNoteSaves.insert(actualId, noteMap);

    bool updated = false;
    for (int i = 0; i < m_icloudNotes.size(); ++i) {
        if (m_icloudNotes.at(i).toMap().value(QStringLiteral("id")).toString() == actualId) {
            m_icloudNotes[i] = noteMap;
            updated = true;
            break;
        }
    }
    if (!updated) {
        m_icloudNotes.prepend(noteMap);
    }
    emit icloudNotesChanged();
    bool initialNoteStateSaved = m_stateFilePath.isEmpty() || saveAppState();
    noteMap.insert(QStringLiteral("statePersisted"), initialNoteStateSaved);
    if (!initialNoteStateSaved) {
        noteMap.insert(QStringLiteral("neverDispatchedLocalCreate"), false);
        noteMap.insert(QStringLiteral("dispatchAttempted"), true);
    }
    m_pendingIcloudNoteSaves.insert(actualId, noteMap);
    for (int i = 0; i < m_icloudNotes.size(); ++i) {
        if (m_icloudNotes.at(i).toMap().value(QStringLiteral("id")).toString() == actualId) {
            m_icloudNotes[i] = noteMap;
            break;
        }
    }

    // Queue behind any already in-flight HTTP request for this noteId instead of dispatching concurrently
    if (isSecureOrLocalIcloudEndpoint() && !m_inFlightIcloudNoteReplies.value(actualId)) {
        noteMap.insert(QStringLiteral("neverDispatchedLocalCreate"), false);
        noteMap.insert(QStringLiteral("dispatchAttempted"), true);
        m_pendingIcloudNoteSaves.insert(actualId, noteMap);
        if (!m_stateFilePath.isEmpty() && !saveAppState()) {
            noteMap.insert(QStringLiteral("statePersisted"), false);
            m_pendingIcloudNoteSaves.insert(actualId, noteMap);
            for (int i = 0; i < m_icloudNotes.size(); ++i) {
                if (m_icloudNotes.at(i).toMap().value(QStringLiteral("id")).toString() == actualId) {
                    m_icloudNotes[i] = noteMap;
                    break;
                }
            }
            recordIcloudMutationFailure(mutKey);
            return actualId;
        }
        noteMap.insert(QStringLiteral("statePersisted"), true);
        m_pendingIcloudNoteSaves.insert(actualId, noteMap);
        for (int i = 0; i < m_icloudNotes.size(); ++i) {
            if (m_icloudNotes.at(i).toMap().value(QStringLiteral("id")).toString() == actualId) {
                m_icloudNotes[i] = noteMap;
                break;
            }
        }

        QJsonObject payload;
        payload.insert(QStringLiteral("id"), actualId);
        payload.insert(QStringLiteral("title"), title);
        payload.insert(QStringLiteral("body"), body);
        payload.insert(QStringLiteral("folder"), folder.isEmpty() ? QStringLiteral("Notes") : folder);
        payload.insert(QStringLiteral("updatedAtMs"), now);
        if (baseRev > 0) {
            payload.insert(QStringLiteral("baseRevision"), baseRev);
        }
        if (allowRecreate) {
            payload.insert(QStringLiteral("allowRecreate"), true);
        }

        QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/icloud/notes")), QJsonDocument(payload).toJson(QJsonDocument::Compact));
        m_inFlightIcloudNoteReplies.insert(actualId, reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, actualId, mutKey, gen]() {
            if (m_inFlightIcloudNoteReplies.value(actualId) == reply) {
                m_inFlightIcloudNoteReplies.remove(actualId);
            }
            reply->deleteLater();
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (m_pendingIcloudNoteSaves.contains(actualId)) {
                    recordIcloudMutationFailure(mutKey);
                } else if (m_pendingIcloudNoteDeletes.contains(actualId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonValue dataVal = doc.value(QStringLiteral("data"));
            const bool sameMutation = m_pendingIcloudNoteSaves.value(actualId).value(QStringLiteral("clientMutationSeq")).toULongLong() == gen;
            auto extractRev = [&actualId](const QJsonArray &arr) -> qint64 {
                for (const QJsonValue &v : arr) {
                    if (v.isObject() && v.toObject().value(QStringLiteral("id")).toString() == actualId) {
                        return v.toObject().value(QStringLiteral("revision")).toVariant().toLongLong();
                    }
                }
                return 0;
            };
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && !m_pendingIcloudNoteDeletes.contains(actualId)) {
                if (doc.value(QStringLiteral("status")).toInt() == 200 && dataVal.isArray()) {
                    const QJsonArray dataArr = dataVal.toArray();
                    ++m_icloudNotesSnapshotEpoch;
                    clearIcloudMutationRetryState(mutKey);
                    const qint64 ackRev = extractRev(dataArr);
                    if (sameMutation) {
                        m_pendingIcloudNoteSaves.remove(actualId);
                    } else if (m_pendingIcloudNoteSaves.contains(actualId) && ackRev > 0) {
                        QVariantMap nextMap = m_pendingIcloudNoteSaves.value(actualId);
                        nextMap.insert(QStringLiteral("baseRevision"), ackRev);
                        nextMap.insert(QStringLiteral("revision"), ackRev);
                        m_pendingIcloudNoteSaves.insert(actualId, nextMap);
                    }
                    if (ackRev > 0) {
                        for (int i = 0; i < m_icloudNotes.size(); ++i) {
                            QVariantMap uiMap = m_icloudNotes.at(i).toMap();
                            if (uiMap.value(QStringLiteral("id")).toString() == actualId) {
                                uiMap.insert(QStringLiteral("baseRevision"), ackRev);
                                uiMap.insert(QStringLiteral("revision"), ackRev);
                                if (sameMutation) {
                                    uiMap.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(gen));
                                }
                                m_icloudNotes[i] = uiMap;
                                break;
                            }
                        }
                    }
                    if (gen == m_icloudNotesGeneration) {
                        applyRemoteIcloudNotes(dataArr, actualId, sameMutation ? gen : 0);
                    } else {
                        emit icloudNotesChanged();
                        saveAppState();
                    }
                    scheduleIcloudMutationRetry(true);
                    return;
                }
            }
            if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                ++m_icloudNotesSnapshotEpoch;
                if (sameMutation && m_pendingIcloudNoteSaves.contains(actualId)) {
                    if (httpStatus == 409 && dataVal.isArray()) {
                        const QVariantMap curPend = m_pendingIcloudNoteSaves.value(actualId);
                        const QString wantTitle = curPend.value(QStringLiteral("title")).toString().trimmed();
                        const QString wantBody = curPend.value(QStringLiteral("body")).toString().trimmed();
                        const QString wantFolder = curPend.value(QStringLiteral("folder"), QStringLiteral("Notes")).toString().trimmed();
                        bool alreadyApplied = false;
                        for (const QJsonValue &v : dataVal.toArray()) {
                            if (!v.isObject()) continue;
                            const QJsonObject o = v.toObject();
                            if (o.value(QStringLiteral("id")).toString() != actualId) continue;
                            const QString srvTitle = o.value(QStringLiteral("title")).toString().trimmed();
                            const QString srvBody = o.value(QStringLiteral("body")).toString().trimmed();
                            const QString srvFolder = o.value(QStringLiteral("folder")).toString().trimmed();
                            alreadyApplied = (srvTitle == wantTitle && srvBody == wantBody &&
                                              (srvFolder == wantFolder || (srvFolder.isEmpty() && wantFolder == QStringLiteral("Notes"))));
                            break;
                        }
                        if (alreadyApplied) {
                            const QJsonArray dataArr = dataVal.toArray();
                            const qint64 ackRev = extractRev(dataArr);
                            clearIcloudMutationRetryState(mutKey);
                            m_pendingIcloudNoteSaves.remove(actualId);
                            if (ackRev > 0) {
                                for (int i = 0; i < m_icloudNotes.size(); ++i) {
                                    QVariantMap uiMap = m_icloudNotes.at(i).toMap();
                                    if (uiMap.value(QStringLiteral("id")).toString() == actualId) {
                                        uiMap.insert(QStringLiteral("baseRevision"), ackRev);
                                        uiMap.insert(QStringLiteral("revision"), ackRev);
                                        uiMap.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(gen));
                                        m_icloudNotes[i] = uiMap;
                                        break;
                                    }
                                }
                            }
                            if (gen == m_icloudNotesGeneration) {
                                applyRemoteIcloudNotes(dataArr, actualId, gen);
                            } else {
                                emit icloudNotesChanged();
                                saveAppState();
                            }
                            scheduleIcloudMutationRetry(true);
                            return;
                        }
                    }
                    m_icloudMutationPausedKeys.insert(mutKey);
                    m_icloudMutationNextRetryMs.remove(mutKey);
                    if (httpStatus == 409 && dataVal.isArray()) {
                        const QJsonArray conflictArr = dataVal.toArray();
                        const qint64 srvRev = extractRev(conflictArr);
                        QString srvTitle;
                        QString srvBody;
                        QString srvFolder = QStringLiteral("Notes");
                        for (const QJsonValue &cv : conflictArr) {
                            if (cv.isObject() && cv.toObject().value(QStringLiteral("id")).toString() == actualId) {
                                const QJsonObject co = cv.toObject();
                                srvTitle = co.value(QStringLiteral("title")).toString();
                                srvBody = co.value(QStringLiteral("body")).toString();
                                srvFolder = co.value(QStringLiteral("folder")).toString(QStringLiteral("Notes"));
                                break;
                            }
                        }
                        QVariantMap pend = m_pendingIcloudNoteSaves.value(actualId);
                        pend.insert(QStringLiteral("conflictServerRevision"), srvRev);
                        pend.insert(QStringLiteral("conflictServerTitle"), srvTitle);
                        pend.insert(QStringLiteral("conflictServerBody"), srvBody);
                        pend.insert(QStringLiteral("conflictServerFolder"), srvFolder);
                        pend.insert(QStringLiteral("hasServerConflict"), true);
                        m_pendingIcloudNoteSaves.insert(actualId, pend);
                        for (int i = 0; i < m_icloudNotes.size(); ++i) {
                            QVariantMap uiMap = m_icloudNotes.at(i).toMap();
                            if (uiMap.value(QStringLiteral("id")).toString() == actualId) {
                                uiMap.insert(QStringLiteral("conflictServerRevision"), srvRev);
                                uiMap.insert(QStringLiteral("conflictServerTitle"), srvTitle);
                                uiMap.insert(QStringLiteral("conflictServerBody"), srvBody);
                                uiMap.insert(QStringLiteral("conflictServerFolder"), srvFolder);
                                uiMap.insert(QStringLiteral("hasServerConflict"), true);
                                m_icloudNotes[i] = uiMap;
                                break;
                            }
                        }
                        emit icloudNotesChanged();
                        emit toastMessage(QStringLiteral("Note revision conflict on server; your local draft is preserved."));
                    } else if (httpStatus == 401 || httpStatus == 403) {
                        emit toastMessage(QStringLiteral("iCloud Hub authentication required; local note draft preserved."));
                    }
                    saveAppState();
                }
                if ((!sameMutation && m_pendingIcloudNoteSaves.contains(actualId)) ||
                    m_pendingIcloudNoteDeletes.contains(actualId)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && m_pendingIcloudNoteDeletes.contains(actualId)) {
                ++m_icloudNotesSnapshotEpoch;
                clearIcloudMutationRetryState(mutKey);
                scheduleIcloudMutationRetry(true);
                return;
            }
            recordIcloudMutationFailure(mutKey);
        });
    }
    return actualId;
}

bool Backend::reloadConflictedIcloudNoteServerVersion(const QString &id) {
    const QString cleanId = id.trimmed();
    if (cleanId.isEmpty()) return false;
    const QVariantMap pend = m_pendingIcloudNoteSaves.value(cleanId);
    const qint64 srvRev = pend.value(QStringLiteral("conflictServerRevision")).toLongLong();
    const bool hadServerConflict = pend.value(QStringLiteral("hasServerConflict")).toBool();
    clearIcloudMutationRetryState(QStringLiteral("note_save:") + cleanId);
    m_pendingIcloudNoteSaves.remove(cleanId);
    const quint64 gen = ++m_icloudNotesGeneration;
    if (srvRev > 0) {
        for (int i = 0; i < m_icloudNotes.size(); ++i) {
            QVariantMap uiMap = m_icloudNotes.at(i).toMap();
            if (uiMap.value(QStringLiteral("id")).toString() == cleanId) {
                uiMap.insert(QStringLiteral("title"), pend.value(QStringLiteral("conflictServerTitle")).toString());
                uiMap.insert(QStringLiteral("body"), pend.value(QStringLiteral("conflictServerBody")).toString());
                uiMap.insert(QStringLiteral("folder"), pend.value(QStringLiteral("conflictServerFolder"), QStringLiteral("Notes")).toString());
                uiMap.insert(QStringLiteral("revision"), srvRev);
                uiMap.insert(QStringLiteral("baseRevision"), srvRev);
                uiMap.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(gen));
                uiMap.remove(QStringLiteral("conflictServerRevision"));
                uiMap.remove(QStringLiteral("conflictServerTitle"));
                uiMap.remove(QStringLiteral("conflictServerBody"));
                uiMap.remove(QStringLiteral("conflictServerFolder"));
                uiMap.remove(QStringLiteral("hasServerConflict"));
                m_icloudNotes[i] = uiMap;
                break;
            }
        }
    } else if (hadServerConflict) {
        for (int i = m_icloudNotes.size() - 1; i >= 0; --i) {
            if (m_icloudNotes.at(i).toMap().value(QStringLiteral("id")).toString() == cleanId) {
                m_icloudNotes.removeAt(i);
            }
        }
    }
    emit icloudNotesChanged();
    saveAppState();
    fetchIcloudNotes();
    return true;
}

bool Backend::isIcloudNoteDurablyPersisted(const QString &id) const {
    const QString cleanId = id.trimmed();
    if (cleanId.isEmpty()) return false;
    if (m_pendingIcloudNoteSaves.contains(cleanId)) {
        const QVariantMap pend = m_pendingIcloudNoteSaves.value(cleanId);
        if (pend.contains(QStringLiteral("statePersisted"))) {
            return pend.value(QStringLiteral("statePersisted")).toBool();
        }
        return m_stateFilePath.isEmpty() || QFileInfo::exists(m_stateFilePath);
    }
    for (const QVariant &v : m_icloudNotes) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toString() == cleanId) {
            if (m.contains(QStringLiteral("statePersisted"))) {
                return m.value(QStringLiteral("statePersisted")).toBool();
            }
            return true;
        }
    }
    return false;
}

void Backend::deleteIcloudNote(const QString &id) {
    if (id.isEmpty()) return;
    const QString mutKey = QStringLiteral("note_del:") + id;
    const QString saveKey = QStringLiteral("note_save:") + id;
    const QVariantMap pendSave = m_pendingIcloudNoteSaves.value(id);
    const bool neverSentLocalCreate = m_pendingIcloudNoteSaves.contains(id) &&
                                      pendSave.value(QStringLiteral("neverDispatchedLocalCreate")).toBool() &&
                                      !pendSave.value(QStringLiteral("dispatchAttempted")).toBool() &&
                                      pendSave.value(QStringLiteral("baseRevision")).toLongLong() <= 0 &&
                                      pendSave.value(QStringLiteral("revision")).toLongLong() <= 0 &&
                                      !m_inFlightIcloudNoteReplies.value(id) &&
                                      m_icloudMutationRetryCounts.value(saveKey, 0) == 0 &&
                                      !m_icloudMutationNextRetryMs.contains(saveKey) &&
                                      !m_icloudMutationPausedKeys.contains(saveKey);
    clearIcloudMutationRetryState(mutKey);
    clearIcloudMutationRetryState(saveKey);
    m_pendingIcloudNoteSaves.remove(id);
    if (neverSentLocalCreate) {
        m_pendingIcloudNoteDeletes.remove(id);
    } else {
        m_pendingIcloudNoteDeletes.insert(id);
    }
    const quint64 gen = ++m_icloudNotesGeneration;

    for (int i = 0; i < m_icloudNotes.size(); ++i) {
        if (m_icloudNotes.at(i).toMap().value(QStringLiteral("id")).toString() == id) {
            m_icloudNotes.removeAt(i);
            break;
        }
    }
    emit icloudNotesChanged();
    saveAppState();
    if (neverSentLocalCreate) return;

    // Queue behind any already in-flight HTTP request for this noteId instead of dispatching concurrently
    if (isSecureOrLocalIcloudEndpoint() && !m_inFlightIcloudNoteReplies.value(id)) {
        const QString encodedId = QString::fromUtf8(QUrl::toPercentEncoding(id));
        QNetworkReply *reply = m_nam.deleteResource(makeRequest(QStringLiteral("/api/v1/icloud/notes/") + encodedId));
        m_inFlightIcloudNoteReplies.insert(id, reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, id, mutKey, gen]() {
            if (m_inFlightIcloudNoteReplies.value(id) == reply) {
                m_inFlightIcloudNoteReplies.remove(id);
            }
            reply->deleteLater();
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (m_pendingIcloudNoteDeletes.contains(id)) {
                    recordIcloudMutationFailure(mutKey);
                } else if (m_pendingIcloudNoteSaves.contains(id)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonValue dataVal = doc.value(QStringLiteral("data"));
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && !m_pendingIcloudNoteSaves.contains(id)) {
                if (doc.value(QStringLiteral("status")).toInt() == 200 && dataVal.isArray()) {
                    ++m_icloudNotesSnapshotEpoch;
                    clearIcloudMutationRetryState(mutKey);
                    m_pendingIcloudNoteDeletes.remove(id);
                    if (gen == m_icloudNotesGeneration) {
                        applyRemoteIcloudNotes(dataVal.toArray());
                    } else {
                        saveAppState();
                    }
                    scheduleIcloudMutationRetry(true);
                    return;
                }
            }
            if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                ++m_icloudNotesSnapshotEpoch;
                const bool isAuthFailure = (httpStatus == 401) || (httpStatus == 403 && !dataVal.isArray());
                if (isAuthFailure) {
                    m_icloudMutationPausedKeys.insert(mutKey);
                    m_icloudMutationNextRetryMs.remove(mutKey);
                    emit toastMessage(QStringLiteral("iCloud Hub authentication required; pending note deletion preserved."));
                    saveAppState();
                } else {
                    clearIcloudMutationRetryState(mutKey);
                    m_pendingIcloudNoteDeletes.remove(id);
                    if (dataVal.isArray() && gen == m_icloudNotesGeneration) {
                        applyRemoteIcloudNotes(dataVal.toArray());
                    } else {
                        saveAppState();
                    }
                }
                if (m_pendingIcloudNoteSaves.contains(id)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && m_pendingIcloudNoteSaves.contains(id)) {
                ++m_icloudNotesSnapshotEpoch;
                clearIcloudMutationRetryState(mutKey);
                scheduleIcloudMutationRetry(true);
                return;
            }
            recordIcloudMutationFailure(mutKey);
        });
    }
}

void Backend::fetchIcloudEvents() {
    if (!m_icloudCalendarEnabled) return;
    if (!isSecureOrLocalIcloudEndpoint()) return;
    const quint64 gen = m_icloudEventsGeneration;
    const quint64 snapEpoch = m_icloudEventsSnapshotEpoch;
    QNetworkReply *reply = m_nam.get(makeRequest(QStringLiteral("/api/v1/icloud/events?limit=500")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, snapEpoch]() {
        reply->deleteLater();
        if (gen != m_icloudEventsGeneration || snapEpoch != m_icloudEventsSnapshotEpoch) return;
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonValue dataVal = doc.value(QStringLiteral("data"));
            if (doc.value(QStringLiteral("status")).toInt() == 200 && dataVal.isArray()) {
                applyRemoteIcloudEvents(dataVal.toArray());
            }
        }
    });

    QNetworkReply *calReply = m_nam.get(makeRequest(QStringLiteral("/api/v1/icloud/calendars")));
    connect(calReply, &QNetworkReply::finished, this, [this, calReply]() {
        calReply->deleteLater();
        if (calReply->error() == QNetworkReply::NoError) {
            const QJsonObject doc = QJsonDocument::fromJson(calReply->readAll()).object();
            const QJsonValue dataVal = doc.value(QStringLiteral("data"));
            if (doc.value(QStringLiteral("status")).toInt() == 200 && dataVal.isArray()) {
                const QJsonArray data = dataVal.toArray();
                m_icloudCalendars.clear();
                for (const QJsonValue &v : data) {
                    if (v.isObject()) m_icloudCalendars.append(v.toObject().toVariantMap());
                }
                emit icloudEventsChanged();
            }
        }
    });
}

QString Backend::saveIcloudEvent(const QString &title, qint64 startMs, qint64 endMs, const QString &calendarName, const QString &notes) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const quint64 gen = ++m_icloudEventsGeneration;
    const QString id = QStringLiteral("evt_%1_%2").arg(now).arg(gen);
    const QString calLabel = calendarName.isEmpty() ? QStringLiteral("Home") : calendarName;
    QVariantMap evtMap;
    evtMap.insert(QStringLiteral("id"), id);
    evtMap.insert(QStringLiteral("title"), title);
    evtMap.insert(QStringLiteral("calendarTitle"), calLabel);
    evtMap.insert(QStringLiteral("calendarName"), calLabel);
    evtMap.insert(QStringLiteral("startMs"), startMs);
    evtMap.insert(QStringLiteral("endMs"), endMs);
    evtMap.insert(QStringLiteral("notes"), notes);
    evtMap.insert(QStringLiteral("updatedAtMs"), now);
    evtMap.insert(QStringLiteral("clientMutationSeq"), QVariant::fromValue(gen));
    evtMap.insert(QStringLiteral("neverDispatchedLocalCreate"), true);

    const QString mutKey = QStringLiteral("evt_save:") + id;
    clearIcloudMutationRetryState(mutKey);
    clearIcloudMutationRetryState(QStringLiteral("evt_del:") + id);
    m_pendingIcloudEventDeletes.remove(id);
    m_pendingIcloudEventSaves.insert(id, evtMap);

    m_icloudEvents.prepend(evtMap);
    emit icloudEventsChanged();
    const bool initialEvtStateSaved = m_stateFilePath.isEmpty() || saveAppState();
    if (!initialEvtStateSaved) {
        evtMap.insert(QStringLiteral("neverDispatchedLocalCreate"), false);
        evtMap.insert(QStringLiteral("dispatchAttempted"), true);
        m_pendingIcloudEventSaves.insert(id, evtMap);
    }

    // Queue behind any already in-flight HTTP request for this evtId instead of dispatching concurrently
    if (isSecureOrLocalIcloudEndpoint() && !m_inFlightIcloudEventReplies.value(id)) {
        evtMap.insert(QStringLiteral("neverDispatchedLocalCreate"), false);
        evtMap.insert(QStringLiteral("dispatchAttempted"), true);
        m_pendingIcloudEventSaves.insert(id, evtMap);
        if (!m_stateFilePath.isEmpty() && !saveAppState()) {
            recordIcloudMutationFailure(mutKey);
            return id;
        }

        QJsonObject payload;
        payload.insert(QStringLiteral("id"), id);
        payload.insert(QStringLiteral("title"), title);
        payload.insert(QStringLiteral("calendarName"), calLabel);
        payload.insert(QStringLiteral("calendarTitle"), calLabel);
        payload.insert(QStringLiteral("startMs"), startMs);
        payload.insert(QStringLiteral("endMs"), endMs);
        payload.insert(QStringLiteral("notes"), notes);
        payload.insert(QStringLiteral("updatedAtMs"), now);

        QNetworkReply *reply = m_nam.post(makeRequest(QStringLiteral("/api/v1/icloud/events")), QJsonDocument(payload).toJson(QJsonDocument::Compact));
        m_inFlightIcloudEventReplies.insert(id, reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, id, mutKey, gen]() {
            if (m_inFlightIcloudEventReplies.value(id) == reply) {
                m_inFlightIcloudEventReplies.remove(id);
            }
            reply->deleteLater();
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (m_pendingIcloudEventSaves.contains(id)) {
                    recordIcloudMutationFailure(mutKey);
                } else if (m_pendingIcloudEventDeletes.contains(id)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonValue dataVal = doc.value(QStringLiteral("data"));
            const bool sameMutation = m_pendingIcloudEventSaves.value(id).value(QStringLiteral("clientMutationSeq")).toULongLong() == gen;
            auto extractRev = [&id](const QJsonArray &arr) -> qint64 {
                for (const QJsonValue &v : arr) {
                    if (v.isObject() && v.toObject().value(QStringLiteral("id")).toString() == id) {
                        return v.toObject().value(QStringLiteral("revision")).toVariant().toLongLong();
                    }
                }
                return 0;
            };
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && !m_pendingIcloudEventDeletes.contains(id)) {
                if (doc.value(QStringLiteral("status")).toInt() == 200 && dataVal.isArray()) {
                    const QJsonArray dataArr = dataVal.toArray();
                    ++m_icloudEventsSnapshotEpoch;
                    clearIcloudMutationRetryState(mutKey);
                    const qint64 ackRev = extractRev(dataArr);
                    if (sameMutation) {
                        m_pendingIcloudEventSaves.remove(id);
                    } else if (m_pendingIcloudEventSaves.contains(id) && ackRev > 0) {
                        QVariantMap nextMap = m_pendingIcloudEventSaves.value(id);
                        nextMap.insert(QStringLiteral("baseRevision"), ackRev);
                        nextMap.insert(QStringLiteral("revision"), ackRev);
                        m_pendingIcloudEventSaves.insert(id, nextMap);
                    }
                    if (ackRev > 0) {
                        for (int i = 0; i < m_icloudEvents.size(); ++i) {
                            QVariantMap uiMap = m_icloudEvents.at(i).toMap();
                            if (uiMap.value(QStringLiteral("id")).toString() == id) {
                                uiMap.insert(QStringLiteral("baseRevision"), ackRev);
                                uiMap.insert(QStringLiteral("revision"), ackRev);
                                m_icloudEvents[i] = uiMap;
                                break;
                            }
                        }
                    }
                    if (gen == m_icloudEventsGeneration) {
                        applyRemoteIcloudEvents(dataArr, id);
                    } else {
                        saveAppState();
                    }
                    scheduleIcloudMutationRetry(true);
                    return;
                }
            }
            if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                ++m_icloudEventsSnapshotEpoch;
                if (sameMutation && m_pendingIcloudEventSaves.contains(id)) {
                    if (httpStatus == 409 && dataVal.isArray()) {
                        const QVariantMap curPend = m_pendingIcloudEventSaves.value(id);
                        const QString wantTitle = curPend.value(QStringLiteral("title")).toString().trimmed();
                        const qint64 wantStart = curPend.value(QStringLiteral("startMs")).toLongLong();
                        const qint64 wantEnd = curPend.value(QStringLiteral("endMs")).toLongLong();
                        const QString wantCal = curPend.value(
                            QStringLiteral("calendarTitle"),
                            curPend.value(QStringLiteral("calendarName"), QStringLiteral("Home"))
                        ).toString().trimmed();
                        const QString wantNotes = curPend.value(QStringLiteral("notes")).toString().trimmed();
                        bool alreadyApplied = false;
                        for (const QJsonValue &v : dataVal.toArray()) {
                            if (!v.isObject()) continue;
                            const QJsonObject o = v.toObject();
                            if (o.value(QStringLiteral("id")).toString() != id) continue;
                            const QString srvTitle = o.value(QStringLiteral("title")).toString().trimmed();
                            const qint64 srvStart = o.value(QStringLiteral("startMs")).toVariant().toLongLong();
                            const qint64 srvEnd = o.value(QStringLiteral("endMs")).toVariant().toLongLong();
                            const QString srvCal = (o.value(QStringLiteral("calendarTitle")).toString().trimmed().isEmpty()
                                                        ? o.value(QStringLiteral("calendarName")).toString().trimmed()
                                                        : o.value(QStringLiteral("calendarTitle")).toString().trimmed());
                            const QString srvNotes = o.value(QStringLiteral("notes")).toString().trimmed();
                            alreadyApplied = (srvTitle == wantTitle && srvStart == wantStart && srvEnd == wantEnd &&
                                              srvCal == wantCal && srvNotes == wantNotes);
                            break;
                        }
                        if (alreadyApplied) {
                            clearIcloudMutationRetryState(mutKey);
                            m_pendingIcloudEventSaves.remove(id);
                            if (gen == m_icloudEventsGeneration) {
                                applyRemoteIcloudEvents(dataVal.toArray(), id);
                            } else {
                                saveAppState();
                            }
                            scheduleIcloudMutationRetry(true);
                            return;
                        }
                    }
                    m_icloudMutationPausedKeys.insert(mutKey);
                    m_icloudMutationNextRetryMs.remove(mutKey);
                    if (httpStatus == 409 && dataVal.isArray()) {
                        const qint64 srvRev = extractRev(dataVal.toArray());
                        if (srvRev > 0) {
                            QVariantMap pend = m_pendingIcloudEventSaves.value(id);
                            pend.insert(QStringLiteral("conflictServerRevision"), srvRev);
                            m_pendingIcloudEventSaves.insert(id, pend);
                        }
                        emit toastMessage(QStringLiteral("Event revision conflict on server; your local draft is preserved."));
                    } else if (httpStatus == 401 || httpStatus == 403) {
                        emit toastMessage(QStringLiteral("iCloud Hub authentication required; local event draft preserved."));
                    }
                    saveAppState();
                }
                if ((!sameMutation && m_pendingIcloudEventSaves.contains(id)) ||
                    m_pendingIcloudEventDeletes.contains(id)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && m_pendingIcloudEventDeletes.contains(id)) {
                ++m_icloudEventsSnapshotEpoch;
                clearIcloudMutationRetryState(mutKey);
                scheduleIcloudMutationRetry(true);
                return;
            }
            recordIcloudMutationFailure(mutKey);
        });
    }
    return id;
}

void Backend::deleteIcloudEvent(const QString &id) {
    if (id.isEmpty()) return;
    const QString mutKey = QStringLiteral("evt_del:") + id;
    const QString saveKey = QStringLiteral("evt_save:") + id;
    const QVariantMap pendSave = m_pendingIcloudEventSaves.value(id);
    const bool neverSentLocalCreate = m_pendingIcloudEventSaves.contains(id) &&
                                      pendSave.value(QStringLiteral("neverDispatchedLocalCreate")).toBool() &&
                                      !pendSave.value(QStringLiteral("dispatchAttempted")).toBool() &&
                                      pendSave.value(QStringLiteral("baseRevision")).toLongLong() <= 0 &&
                                      pendSave.value(QStringLiteral("revision")).toLongLong() <= 0 &&
                                      !m_inFlightIcloudEventReplies.value(id) &&
                                      m_icloudMutationRetryCounts.value(saveKey, 0) == 0 &&
                                      !m_icloudMutationNextRetryMs.contains(saveKey) &&
                                      !m_icloudMutationPausedKeys.contains(saveKey);
    clearIcloudMutationRetryState(mutKey);
    clearIcloudMutationRetryState(saveKey);
    m_pendingIcloudEventSaves.remove(id);
    if (neverSentLocalCreate) {
        m_pendingIcloudEventDeletes.remove(id);
    } else {
        m_pendingIcloudEventDeletes.insert(id);
    }
    const quint64 gen = ++m_icloudEventsGeneration;

    for (int i = 0; i < m_icloudEvents.size(); ++i) {
        if (m_icloudEvents.at(i).toMap().value(QStringLiteral("id")).toString() == id) {
            m_icloudEvents.removeAt(i);
            break;
        }
    }
    emit icloudEventsChanged();
    saveAppState();
    if (neverSentLocalCreate) return;

    // Queue behind any already in-flight HTTP request for this evtId instead of dispatching concurrently
    if (isSecureOrLocalIcloudEndpoint() && !m_inFlightIcloudEventReplies.value(id)) {
        const QString encodedId = QString::fromUtf8(QUrl::toPercentEncoding(id));
        QNetworkReply *reply = m_nam.deleteResource(makeRequest(QStringLiteral("/api/v1/icloud/events/") + encodedId));
        m_inFlightIcloudEventReplies.insert(id, reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, id, mutKey, gen]() {
            if (m_inFlightIcloudEventReplies.value(id) == reply) {
                m_inFlightIcloudEventReplies.remove(id);
            }
            reply->deleteLater();
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (m_pendingIcloudEventDeletes.contains(id)) {
                    recordIcloudMutationFailure(mutKey);
                } else if (m_pendingIcloudEventSaves.contains(id)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonValue dataVal = doc.value(QStringLiteral("data"));
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && !m_pendingIcloudEventSaves.contains(id)) {
                if (doc.value(QStringLiteral("status")).toInt() == 200 && dataVal.isArray()) {
                    ++m_icloudEventsSnapshotEpoch;
                    clearIcloudMutationRetryState(mutKey);
                    m_pendingIcloudEventDeletes.remove(id);
                    if (gen == m_icloudEventsGeneration) {
                        applyRemoteIcloudEvents(dataVal.toArray());
                    } else {
                        saveAppState();
                    }
                    scheduleIcloudMutationRetry(true);
                    return;
                }
            }
            if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                ++m_icloudEventsSnapshotEpoch;
                const bool isAuthFailure = (httpStatus == 401) || (httpStatus == 403 && !dataVal.isArray());
                if (isAuthFailure) {
                    m_icloudMutationPausedKeys.insert(mutKey);
                    m_icloudMutationNextRetryMs.remove(mutKey);
                    emit toastMessage(QStringLiteral("iCloud Hub authentication required; pending event deletion preserved."));
                    saveAppState();
                } else {
                    clearIcloudMutationRetryState(mutKey);
                    m_pendingIcloudEventDeletes.remove(id);
                    if (dataVal.isArray() && gen == m_icloudEventsGeneration) {
                        applyRemoteIcloudEvents(dataVal.toArray());
                    } else {
                        saveAppState();
                    }
                }
                if (m_pendingIcloudEventSaves.contains(id)) {
                    scheduleIcloudMutationRetry(true);
                }
                return;
            }
            if (reply->error() == QNetworkReply::NoError && httpStatus == 200 && m_pendingIcloudEventSaves.contains(id)) {
                ++m_icloudEventsSnapshotEpoch;
                clearIcloudMutationRetryState(mutKey);
                scheduleIcloudMutationRetry(true);
                return;
            }
            recordIcloudMutationFailure(mutKey);
        });
    }
}

void Backend::setIcloudPhotosPageSize(int size) {
    const int clamped = std::clamp(size, 12, 240);
    if (m_icloudPhotosPageSize == clamped) return;
    m_icloudPhotosPageSize = clamped;
    m_icloudPhotosPage = 0;
    emit icloudPhotosChanged();
    if (m_icloudPhotosEnabled) {
        fetchIcloudPhotos(0, false);
    }
}

void Backend::nextIcloudPhotosPage() {
    if (!m_icloudPhotosHasMore && (m_icloudPhotosPage + 1) >= icloudPhotosPageCount()) return;
    fetchIcloudPhotos(m_icloudPhotosPage + 1, false);
}

void Backend::prevIcloudPhotosPage() {
    if (m_icloudPhotosPage <= 0) return;
    fetchIcloudPhotos(m_icloudPhotosPage - 1, false);
}

void Backend::loadMoreIcloudPhotos() {
    if (m_icloudPhotosLoading || !m_icloudPhotosHasMore) return;
    fetchIcloudPhotos(m_icloudPhotosPage, true);
}

void Backend::fetchIcloudPhotos(int page, bool append, bool forceNudge) {
    if (!m_icloudPhotosEnabled) return;
    if (!isSecureOrLocalIcloudEndpoint()) return;
    const int pageSize = std::max(12, m_icloudPhotosPageSize);
    const int targetPage = (page >= 0) ? std::max(0, page) : m_icloudPhotosPage;
    const int offset = append
                           ? (m_icloudPhotosPage * pageSize + int(m_icloudPhotos.size()))
                           : (targetPage * pageSize);
    const quint64 reqSeq = ++m_icloudPhotosRequestSeq;
    if (!m_icloudPhotosLoading) {
        m_icloudPhotosLoading = true;
        emit icloudPhotosLoadingChanged();
    }
    QString path = QStringLiteral("/api/v1/icloud/photos?source=photos_library&limit=%1&offset=%2")
                       .arg(pageSize)
                       .arg(offset);
    if (forceNudge && targetPage == 0 && !append) {
        path += QStringLiteral("&refresh=1");
    }
    QNetworkReply *reply = m_nam.get(makeRequest(path));
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqSeq, append, offset, pageSize, targetPage]() {
        reply->deleteLater();
        if (reqSeq != m_icloudPhotosRequestSeq) return;
        if (m_icloudPhotosLoading) {
            m_icloudPhotosLoading = false;
            emit icloudPhotosLoadingChanged();
        }
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonValue dataVal = doc.value(QStringLiteral("data"));
            if (doc.value(QStringLiteral("status")).toInt() == 200 && dataVal.isArray()) {
                const QJsonArray data = dataVal.toArray();
                QSet<QString> seenGuids;
                if (!append) {
                    m_icloudPhotosPage = targetPage;
                    m_icloudPhotos.clear();
                } else {
                    for (const QVariant &existing : m_icloudPhotos) {
                        const QString eg = existing.toMap().value(QStringLiteral("guid")).toString().trimmed();
                        if (!eg.isEmpty()) seenGuids.insert(eg);
                    }
                }
                for (const QJsonValue &v : data) {
                    if (v.isObject()) {
                        QVariantMap m = v.toObject().toVariantMap();
                        const QString guid = m.value(QStringLiteral("guid")).toString().trimmed();
                        if (!guid.isEmpty()) {
                            if (seenGuids.contains(guid)) continue;
                            seenGuids.insert(guid);
                        }
                        if (!guid.isEmpty()) {
                            const bool isVideo = m.value(QStringLiteral("isVideo")).toBool() ||
                                                 (m.value(QStringLiteral("mediaType")).toString() == QStringLiteral("video"));
                            const QString srvPreview = m.value(QStringLiteral("previewUrl")).toString();
                            const bool wantPreviewParam = isVideo && srvPreview.contains(QStringLiteral("preview=1"));
                            const QString baseLiveUrl = attachmentDownloadUrl(guid);
                            const QString serverLiveUrl = isSecureOrLocalIcloudEndpoint()
                                ? (m_serverUrl + QStringLiteral("/api/v1/attachment/")
                                   + QString::fromUtf8(QUrl::toPercentEncoding(guid))
                                   + QStringLiteral("/live?password=")
                                   + QString::fromUtf8(QUrl::toPercentEncoding(m_password)))
                                : QString();
                            QString thumbUrl;
                            if (isVideo) {
                                if (wantPreviewParam && !serverLiveUrl.isEmpty()) {
                                    thumbUrl = serverLiveUrl + QStringLiteral("&height=360&preview=1");
                                }
                            } else {
                                thumbUrl = baseLiveUrl;
                                if (!thumbUrl.isEmpty() && !thumbUrl.startsWith(QStringLiteral("file://"))) {
                                    const QLatin1Char sep = thumbUrl.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?');
                                    thumbUrl += sep + QStringLiteral("height=360");
                                }
                            }
                            m.insert(QStringLiteral("previewUrl"), thumbUrl);
                            if (!m.value(QStringLiteral("isCloudEvicted")).toBool() && !baseLiveUrl.isEmpty()) {
                                m.insert(QStringLiteral("streamUrl"), baseLiveUrl);
                            }
                            const QString cached = lookupCachedAttachmentPath(m_attachmentPaths, guid);
                            if (!cached.isEmpty() && QFileInfo::exists(cached) && QFileInfo(cached).size() >= 100) {
                                const QString localUrl = QUrl::fromLocalFile(cached).toString();
                                m.insert(QStringLiteral("localUrl"), localUrl);
                                if (!isVideo) {
                                    m.insert(QStringLiteral("previewUrl"), localUrl);
                                }
                                m.insert(QStringLiteral("streamUrl"), localUrl);
                                m.insert(QStringLiteral("isCloudEvicted"), false);
                            }
                        }
                        m_icloudPhotos.append(m);
                    }
                }
                const int srvTotal = doc.value(QStringLiteral("total")).toInt(0);
                m_icloudPhotosTotal = std::max(srvTotal, offset + int(data.size()));
                if (doc.contains(QStringLiteral("hasMore"))) {
                    m_icloudPhotosHasMore = doc.value(QStringLiteral("hasMore")).toBool();
                } else {
                    m_icloudPhotosHasMore = (int(data.size()) >= pageSize) && ((offset + int(data.size())) < m_icloudPhotosTotal);
                }
                emit icloudPhotosChanged();
            }
        }
    });
}

void Backend::fetchIcloudPhotosTotalOnly() {
    if (!m_icloudPhotosEnabled) return;
    if (!isSecureOrLocalIcloudEndpoint()) return;
    const quint64 navGen = m_icloudPhotosRequestSeq;
    const quint64 totalSeq = ++m_icloudPhotosTotalRequestSeq;
    const QString path = QStringLiteral("/api/v1/icloud/photos?source=photos_library&limit=1&offset=0");
    QNetworkReply *reply = m_nam.get(makeRequest(path));
    connect(reply, &QNetworkReply::finished, this, [this, reply, navGen, totalSeq]() {
        reply->deleteLater();
        if (m_icloudPhotosRequestSeq != navGen || m_icloudPhotosTotalRequestSeq != totalSeq) return;
        if (reply->error() != QNetworkReply::NoError) return;
        const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
        if (doc.value(QStringLiteral("status")).toInt() != 200) return;
        const int srvTotal = doc.value(QStringLiteral("total")).toInt(-1);
        if (srvTotal < 0) return;
        const int pageSize = std::max(12, m_icloudPhotosPageSize);
        const int loadedEnd = (m_icloudPhotosPage * pageSize) + int(m_icloudPhotos.size());
        const bool newHasMore = (loadedEnd < srvTotal);
        if (m_icloudPhotosTotal != srvTotal || m_icloudPhotosHasMore != newHasMore) {
            m_icloudPhotosTotal = srvTotal;
            m_icloudPhotosHasMore = newHasMore;
            emit icloudPhotosChanged();
        }
    });
}

void Backend::syncIcloudPhotos() {
    if (!isSecureOrLocalIcloudEndpoint()) return;
    if (m_icloudPhotosSyncInFlight) return;
    m_icloudPhotosSyncInFlight = true;
    emit icloudPhotosSyncingChanged();
    syncIcloudPhotosPage(0, 0);
}

void Backend::syncIcloudPhotosPage(int offset, int accumulatedSynced, const QString &jobId) {
    if (!isSecureOrLocalIcloudEndpoint()) {
        m_icloudPhotosSyncInFlight = false;
        emit icloudPhotosSyncingChanged();
        return;
    }
    QNetworkReply *reply = nullptr;
    if (!jobId.isEmpty()) {
        const QString encodedJob = QString::fromUtf8(QUrl::toPercentEncoding(jobId));
        reply = m_mediaNam.get(makeRequest(QStringLiteral("/api/v1/icloud/photos/sync?jobId=") + encodedJob));
    } else {
        QJsonObject payload;
        payload.insert(QStringLiteral("limit"), 250);
        payload.insert(QStringLiteral("offset"), offset);
        payload.insert(QStringLiteral("syncAll"), true);
        reply = m_mediaNam.post(makeRequest(QStringLiteral("/api/v1/icloud/photos/sync")), QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, offset, accumulatedSynced]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_icloudPhotosSyncInFlight = false;
            emit icloudPhotosSyncingChanged();
            emit toastMessage(QStringLiteral("iCloud Photos sync failed: ") + reply->errorString());
            return;
        }
        const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject data = doc.value(QStringLiteral("data")).toObject();
        const QString state = data.value(QStringLiteral("state")).toString();
        const QString statusStr = data.value(QStringLiteral("status")).toString();
        const QString activeJobId = data.value(QStringLiteral("jobId")).toString();
        if (state == QStringLiteral("running") && !activeJobId.isEmpty()) {
            QTimer::singleShot(350, this, [this, offset, accumulatedSynced, activeJobId]() {
                syncIcloudPhotosPage(offset, accumulatedSynced, activeJobId);
            });
            return;
        }
        const int batchSynced = data.value(QStringLiteral("synced")).toInt();
        const int totalSynced = activeJobId.isEmpty() ? (accumulatedSynced + batchSynced) : batchSynced;
        const int failedCount = data.value(QStringLiteral("failed")).toInt();
        const bool hasMore = data.value(QStringLiteral("hasMore")).toBool();
        const int nextOffset = data.value(QStringLiteral("nextOffset")).toInt(-1);
        if (activeJobId.isEmpty() && hasMore && nextOffset > offset) {
            syncIcloudPhotosPage(nextOffset, totalSynced);
            return;
        }
        m_icloudPhotosSyncInFlight = false;
        emit icloudPhotosSyncingChanged();
        if (state == QStringLiteral("failed") || statusStr == QStringLiteral("error") || statusStr == QStringLiteral("partial") || failedCount > 0) {
            QString errMsg = data.value(QStringLiteral("error")).toString().trimmed();
            if (errMsg.isEmpty()) {
                errMsg = doc.value(QStringLiteral("message")).toString().trimmed();
            }
            if (errMsg.isEmpty() && failedCount > 0) {
                errMsg = QString::number(failedCount) + QStringLiteral(" photo(s) failed to download");
            }
            if (errMsg.isEmpty()) {
                errMsg = QStringLiteral("Photo sync job failed");
            }
            emit toastMessage(QStringLiteral("iCloud Photos sync failed: ") + errMsg);
        } else {
            emit toastMessage(QStringLiteral("iCloud Photos sync complete: ") + QString::number(totalSynced) + QStringLiteral(" processed"));
        }
        fetchIcloudPhotos();
    });
}

void Backend::downloadCloudAttachment(const QString &attachmentGuid) {
    if (attachmentGuid.isEmpty()) return;
    if (isSecureOrLocalIcloudEndpoint()) {
        const QString encodedGuid = QString::fromUtf8(QUrl::toPercentEncoding(attachmentGuid));
        QNetworkReply *reply = m_mediaNam.post(makeRequest(QStringLiteral("/api/v1/attachment/") + encodedGuid + QStringLiteral("/download")), QByteArray());
        connect(reply, &QNetworkReply::finished, this, [this, reply, attachmentGuid]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit toastMessage(QStringLiteral("Cloud attachment download failed: ") + reply->errorString());
                return;
            }
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonObject dataObj = doc.value(QStringLiteral("data")).toObject();
            if (dataObj.value(QStringLiteral("downloaded")).toBool()) {
                m_attemptedAttachments.remove(attachmentGuid);
                m_missingAttachmentCooldownUntilMs.remove(attachmentGuid);
                m_attachmentRetryCount.remove(attachmentGuid);
                QString photoName = dataObj.value(QStringLiteral("transferName")).toString().trimmed();
                if (photoName.isEmpty()) {
                    for (const QVariant &v : m_icloudPhotos) {
                        const QVariantMap m = v.toMap();
                        if (m.value(QStringLiteral("guid")).toString() == attachmentGuid) {
                            photoName = m.value(QStringLiteral("name")).toString();
                            break;
                        }
                    }
                }
                fetchIcloudPhotos();
                downloadAttachment(attachmentGuid, photoName, true);
            } else {
                QString msg = dataObj.value(QStringLiteral("error")).toString().trimmed();
                if (msg.isEmpty()) {
                    msg = dataObj.value(QStringLiteral("message")).toString().trimmed();
                }
                if (msg.isEmpty()) {
                    msg = QStringLiteral("Attachment still downloading from iCloud");
                }
                emit toastMessage(msg);
            }
        });
    }
}

void Backend::shareIcloudPhotoToChat(const QString &chatGuid, const QString &attachmentGuid, const QString &fileName) {
    if (chatGuid.trimmed().isEmpty() || attachmentGuid.trimmed().isEmpty()) return;
    const QString cachedUrl = m_attachmentPaths.value(attachmentGuid);
    if (!cachedUrl.isEmpty()) {
        const QString localPath = QUrl(cachedUrl).isLocalFile() ? QUrl(cachedUrl).toLocalFile() : cachedUrl;
        if (QFileInfo::exists(localPath) && (m_demoMode || isSecureOrLocalIcloudEndpoint())) {
            sendAttachment(chatGuid, localPath);
            return;
        }
    }
    if (!isSecureOrLocalIcloudEndpoint()) {
        emit toastMessage(QStringLiteral("Sharing iCloud photos requires HTTPS or a local loopback tunnel"));
        return;
    }
    const QString encodedGuid = QString::fromUtf8(QUrl::toPercentEncoding(attachmentGuid));
    QNetworkReply *reply = m_mediaNam.get(makeRequest(QStringLiteral("/api/v1/attachment/") + encodedGuid + QStringLiteral("/download")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, chatGuid, attachmentGuid, fileName]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit toastMessage(QStringLiteral("Failed to fetch iCloud photo for sharing"));
            return;
        }
        const QByteArray raw = reply->readAll();
        if (raw.isEmpty()) {
            emit toastMessage(QStringLiteral("Downloaded iCloud photo is empty"));
            return;
        }
        const QString cacheDir = QDir::homePath() + QStringLiteral("/.cache/imsg/attachments");
        QDir().mkpath(cacheDir);
        QString safeName = QFileInfo(fileName.isEmpty() ? QStringLiteral("photo.jpg") : fileName).fileName();
        if (safeName.isEmpty()) safeName = QStringLiteral("photo.jpg");
        QString safeGuid = attachmentGuid;
        safeGuid.replace(QLatin1Char('/'), QLatin1Char('_'));
        safeGuid.replace(QLatin1Char(':'), QLatin1Char('_'));
        const QString destPath = cacheDir + QLatin1Char('/') + safeGuid + QLatin1Char('_') + safeName;
        QFile out(destPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            out.write(raw);
            out.close();

            const QString lower = destPath.toLower();
            const QByteArray hdr = raw.left(16);
            const bool isHeifMagic = (hdr.size() >= 12 && hdr.mid(4, 4) == QByteArrayLiteral("ftyp"));
            const bool isNonNativeExt = lower.endsWith(QStringLiteral(".heic")) || lower.endsWith(QStringLiteral(".heif")) ||
                                        lower.endsWith(QStringLiteral(".heics")) || lower.endsWith(QStringLiteral(".avif")) ||
                                        lower.endsWith(QStringLiteral(".tiff")) || lower.endsWith(QStringLiteral(".tif")) ||
                                        lower.endsWith(QStringLiteral(".bmp"));
            if (isHeifMagic || isNonNativeExt) {
                const bool inPlaceJpg = lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg"));
                const QString outTarget = inPlaceJpg
                                              ? (destPath + QStringLiteral(".conv.jpg"))
                                              : (destPath + QStringLiteral(".jpg"));
                QProcess *proc = new QProcess(this);
                connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                        [this, proc, chatGuid, attachmentGuid, destPath, outTarget, inPlaceJpg](int, QProcess::ExitStatus) {
                            proc->deleteLater();
                            QString finalPath = destPath;
                            if (QFileInfo::exists(outTarget) && QFileInfo(outTarget).size() >= 100) {
                                QFile::setPermissions(outTarget, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                                if (inPlaceJpg) {
                                    QFile::remove(destPath);
                                    QFile::rename(outTarget, destPath);
                                    finalPath = destPath;
                                } else {
                                    finalPath = outTarget;
                                }
                            }
                            QFile::setPermissions(finalPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                            m_attachmentPaths.insert(attachmentGuid, finalPath);
                            if (!m_demoMode && !isSecureOrLocalIcloudEndpoint()) {
                                return;
                            }
                            sendAttachment(chatGuid, finalPath);
                        });
                proc->start(QStringLiteral("sh"), QStringList{
                    QStringLiteral("-c"),
                    QStringLiteral("umask 077; heif-convert -q 90 \"$1\" \"$2\" >/dev/null 2>&1 || magick \"$1[0]\" -auto-orient -quality 90 \"$2\" >/dev/null 2>&1 || ffmpeg -y -loglevel error -i \"$1\" -frames:v 1 -update 1 -f image2 -c:v mjpeg \"$2\" >/dev/null 2>&1"),
                    QStringLiteral("--"),
                    destPath,
                    outTarget
                });
                return;
            }

            m_attachmentPaths.insert(attachmentGuid, destPath);
            if (!m_demoMode && !isSecureOrLocalIcloudEndpoint()) {
                return;
            }
            sendAttachment(chatGuid, destPath);
        }
    });
}
