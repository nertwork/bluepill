/****************************************************************************
** Meta object code from reading C++ file 'backend.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.12.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../src/backend.h"
#include <QtNetwork/QSslError>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'backend.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.12.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN7BackendE_t {};
} // unnamed namespace

template <> constexpr inline auto Backend::qt_create_metaobjectdata<qt_meta_tag_ZN7BackendE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "Backend",
        "statusChanged",
        "",
        "configChanged",
        "loadingChanged",
        "filterChanged",
        "selectedChatChanged",
        "notificationsChanged",
        "omarchyDndChanged",
        "chatsChanged",
        "contactsChanged",
        "aboutToChangeMessages",
        "messagesChanged",
        "olderMessagesChanged",
        "windowActiveChanged",
        "historySyncDaysChanged",
        "syncingHistoryChanged",
        "audioPlaybackChanged",
        "voiceRecordingChanged",
        "scheduledMessagesChanged",
        "toastMessage",
        "text",
        "attachmentReady",
        "attachmentGuid",
        "localUrl",
        "linkPreviewUpdated",
        "messageGuid",
        "QVariantMap",
        "previewData",
        "saveServerConfig",
        "serverUrl",
        "secondaryUrl",
        "password",
        "webUiUrl",
        "openWebUi",
        "setHistorySyncDays",
        "days",
        "cycleHistorySyncDays",
        "clearSelectedChat",
        "goHome",
        "toggleNotifications",
        "toggleChatMute",
        "chatGuid",
        "isChatMuted",
        "togglePinChat",
        "isChatPinned",
        "setSearchQuery",
        "query",
        "setFilterMode",
        "mode",
        "refreshAll",
        "selectChat",
        "selectNextChat",
        "selectPrevChat",
        "markChatRead",
        "markChatUnread",
        "markAllChatsRead",
        "toggleChatReadUnread",
        "toggleChatRead",
        "sendMessage",
        "replyToGuid",
        "sendReaction",
        "reaction",
        "deleteMessage",
        "unsendMessage",
        "deleteChat",
        "startNewChat",
        "address",
        "openAttachment",
        "guid",
        "transferName",
        "registerWindow",
        "window",
        "loadDemoData",
        "seedDemoData",
        "saveContact",
        "firstName",
        "lastName",
        "phone",
        "email",
        "syncContactsNow",
        "loadOlderMessages",
        "replaceEmojiShortcodes",
        "searchEmojis",
        "QVariantList",
        "category",
        "limit",
        "copyToClipboard",
        "label",
        "copyTextToClipboard",
        "extractVerificationCode",
        "extractOtpCode",
        "pasteClipboardImageToTempFile",
        "sendAttachment",
        "filePath",
        "caption",
        "isAudioAttachment",
        "mimeType",
        "fileName",
        "computeWaveformBars",
        "seed",
        "count",
        "toggleAudioPlayback",
        "sourceUrl",
        "seekAudioPlayback",
        "ratio",
        "startVoiceRecording",
        "stopAndSendVoiceRecording",
        "cancelVoiceRecording",
        "sendMessageWithEffect",
        "effectId",
        "scheduleMessage",
        "sendAtMs",
        "cancelScheduledMessage",
        "scheduleId",
        "addParticipantToActiveChat",
        "removeParticipantFromActiveChat",
        "syncChatStateToServer",
        "searchContacts",
        "canonicalizeAddress",
        "raw",
        "formatHandleForDisplay",
        "connected",
        "loading",
        "serverStatus",
        "secondaryServerUrl",
        "serverPassword",
        "hasPasswordConfigured",
        "needsSetup",
        "searchQuery",
        "filterMode",
        "selectedChatGuid",
        "selectedChatTitle",
        "selectedChatSubtitle",
        "selectedChatService",
        "selectedChatAvatar",
        "selectedChatInitials",
        "selectedChatColor",
        "selectedChatIsGroup",
        "selectedChatMuted",
        "selectedChatAddress",
        "selectedChatHasContact",
        "selectedChatUnreadCount",
        "notificationsEnabled",
        "omarchyDnd",
        "chats",
        "pinnedChats",
        "messages",
        "totalUnread",
        "windowActive",
        "hasOlderMessages",
        "loadingOlderMessages",
        "loadedMessageCount",
        "historySyncDays",
        "syncingHistory",
        "playingAudioGuid",
        "audioPlaying",
        "audioProgress",
        "audioDurationText",
        "recordingVoice",
        "voiceRecordingSeconds",
        "voiceRecordingWaveform",
        "scheduledMessages",
        "selectedChatParticipants"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'statusChanged'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'configChanged'
        QtMocHelpers::SignalData<void()>(3, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'loadingChanged'
        QtMocHelpers::SignalData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'filterChanged'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'selectedChatChanged'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'notificationsChanged'
        QtMocHelpers::SignalData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'omarchyDndChanged'
        QtMocHelpers::SignalData<void()>(8, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'chatsChanged'
        QtMocHelpers::SignalData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'contactsChanged'
        QtMocHelpers::SignalData<void()>(10, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'aboutToChangeMessages'
        QtMocHelpers::SignalData<void()>(11, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'messagesChanged'
        QtMocHelpers::SignalData<void()>(12, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'olderMessagesChanged'
        QtMocHelpers::SignalData<void()>(13, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'windowActiveChanged'
        QtMocHelpers::SignalData<void()>(14, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'historySyncDaysChanged'
        QtMocHelpers::SignalData<void()>(15, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'syncingHistoryChanged'
        QtMocHelpers::SignalData<void()>(16, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'audioPlaybackChanged'
        QtMocHelpers::SignalData<void()>(17, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'voiceRecordingChanged'
        QtMocHelpers::SignalData<void()>(18, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'scheduledMessagesChanged'
        QtMocHelpers::SignalData<void()>(19, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'toastMessage'
        QtMocHelpers::SignalData<void(const QString &)>(20, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 21 },
        }}),
        // Signal 'attachmentReady'
        QtMocHelpers::SignalData<void(const QString &, const QString &)>(22, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 23 }, { QMetaType::QString, 24 },
        }}),
        // Signal 'linkPreviewUpdated'
        QtMocHelpers::SignalData<void(const QString &, const QVariantMap &)>(25, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 26 }, { 0x80000000 | 27, 28 },
        }}),
        // Method 'saveServerConfig'
        QtMocHelpers::MethodData<void(const QString &, const QString &, const QString &)>(29, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 30 }, { QMetaType::QString, 31 }, { QMetaType::QString, 32 },
        }}),
        // Method 'webUiUrl'
        QtMocHelpers::MethodData<QString() const>(33, 2, QMC::AccessPublic, QMetaType::QString),
        // Method 'openWebUi'
        QtMocHelpers::MethodData<void()>(34, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'setHistorySyncDays'
        QtMocHelpers::MethodData<void(int)>(35, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 36 },
        }}),
        // Method 'cycleHistorySyncDays'
        QtMocHelpers::MethodData<void()>(37, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'clearSelectedChat'
        QtMocHelpers::MethodData<void()>(38, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'goHome'
        QtMocHelpers::MethodData<void()>(39, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'toggleNotifications'
        QtMocHelpers::MethodData<void()>(40, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'toggleChatMute'
        QtMocHelpers::MethodData<void(const QString &)>(41, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'isChatMuted'
        QtMocHelpers::MethodData<bool(const QString &) const>(43, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'togglePinChat'
        QtMocHelpers::MethodData<void(const QString &)>(44, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'isChatPinned'
        QtMocHelpers::MethodData<bool(const QString &) const>(45, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'setSearchQuery'
        QtMocHelpers::MethodData<void(const QString &)>(46, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 47 },
        }}),
        // Method 'setFilterMode'
        QtMocHelpers::MethodData<void(const QString &)>(48, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 49 },
        }}),
        // Method 'refreshAll'
        QtMocHelpers::MethodData<void()>(50, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'selectChat'
        QtMocHelpers::MethodData<void(const QString &)>(51, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'selectNextChat'
        QtMocHelpers::MethodData<void()>(52, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'selectPrevChat'
        QtMocHelpers::MethodData<void()>(53, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'markChatRead'
        QtMocHelpers::MethodData<void(const QString &)>(54, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'markChatUnread'
        QtMocHelpers::MethodData<void(const QString &)>(55, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'markAllChatsRead'
        QtMocHelpers::MethodData<void()>(56, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'toggleChatReadUnread'
        QtMocHelpers::MethodData<void(const QString &)>(57, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'toggleChatRead'
        QtMocHelpers::MethodData<void(const QString &)>(58, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'sendMessage'
        QtMocHelpers::MethodData<void(const QString &, const QString &, const QString &)>(59, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 }, { QMetaType::QString, 21 }, { QMetaType::QString, 60 },
        }}),
        // Method 'sendMessage'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(59, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Void, {{
            { QMetaType::QString, 42 }, { QMetaType::QString, 21 },
        }}),
        // Method 'sendReaction'
        QtMocHelpers::MethodData<void(const QString &, const QString &, const QString &)>(61, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 }, { QMetaType::QString, 26 }, { QMetaType::QString, 62 },
        }}),
        // Method 'deleteMessage'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(63, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 26 }, { QMetaType::QString, 42 },
        }}),
        // Method 'deleteMessage'
        QtMocHelpers::MethodData<void(const QString &)>(63, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Void, {{
            { QMetaType::QString, 26 },
        }}),
        // Method 'unsendMessage'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(64, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 26 }, { QMetaType::QString, 42 },
        }}),
        // Method 'unsendMessage'
        QtMocHelpers::MethodData<void(const QString &)>(64, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Void, {{
            { QMetaType::QString, 26 },
        }}),
        // Method 'deleteChat'
        QtMocHelpers::MethodData<void(const QString &)>(65, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 },
        }}),
        // Method 'deleteChat'
        QtMocHelpers::MethodData<void()>(65, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Void),
        // Method 'startNewChat'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(66, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 67 }, { QMetaType::QString, 21 },
        }}),
        // Method 'startNewChat'
        QtMocHelpers::MethodData<void(const QString &)>(66, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Void, {{
            { QMetaType::QString, 67 },
        }}),
        // Method 'openAttachment'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(68, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 69 }, { QMetaType::QString, 70 },
        }}),
        // Method 'registerWindow'
        QtMocHelpers::MethodData<void(QObject *)>(71, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QObjectStar, 72 },
        }}),
        // Method 'loadDemoData'
        QtMocHelpers::MethodData<void()>(73, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'seedDemoData'
        QtMocHelpers::MethodData<void()>(74, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'saveContact'
        QtMocHelpers::MethodData<void(const QString &, const QString &, const QString &, const QString &)>(75, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 76 }, { QMetaType::QString, 77 }, { QMetaType::QString, 78 }, { QMetaType::QString, 79 },
        }}),
        // Method 'syncContactsNow'
        QtMocHelpers::MethodData<void()>(80, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'loadOlderMessages'
        QtMocHelpers::MethodData<void()>(81, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'replaceEmojiShortcodes'
        QtMocHelpers::MethodData<QString(const QString &)>(82, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 21 },
        }}),
        // Method 'searchEmojis'
        QtMocHelpers::MethodData<QVariantList(const QString &, const QString &, int) const>(83, 2, QMC::AccessPublic, 0x80000000 | 84, {{
            { QMetaType::QString, 47 }, { QMetaType::QString, 85 }, { QMetaType::Int, 86 },
        }}),
        // Method 'searchEmojis'
        QtMocHelpers::MethodData<QVariantList(const QString &, const QString &) const>(83, 2, QMC::AccessPublic | QMC::MethodCloned, 0x80000000 | 84, {{
            { QMetaType::QString, 47 }, { QMetaType::QString, 85 },
        }}),
        // Method 'searchEmojis'
        QtMocHelpers::MethodData<QVariantList(const QString &) const>(83, 2, QMC::AccessPublic | QMC::MethodCloned, 0x80000000 | 84, {{
            { QMetaType::QString, 47 },
        }}),
        // Method 'searchEmojis'
        QtMocHelpers::MethodData<QVariantList() const>(83, 2, QMC::AccessPublic | QMC::MethodCloned, 0x80000000 | 84),
        // Method 'copyToClipboard'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(87, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 21 }, { QMetaType::QString, 88 },
        }}),
        // Method 'copyToClipboard'
        QtMocHelpers::MethodData<void(const QString &)>(87, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Void, {{
            { QMetaType::QString, 21 },
        }}),
        // Method 'copyTextToClipboard'
        QtMocHelpers::MethodData<void(const QString &)>(89, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 21 },
        }}),
        // Method 'extractVerificationCode'
        QtMocHelpers::MethodData<QString(const QString &)>(90, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 21 },
        }}),
        // Method 'extractOtpCode'
        QtMocHelpers::MethodData<QString(const QString &)>(91, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 21 },
        }}),
        // Method 'pasteClipboardImageToTempFile'
        QtMocHelpers::MethodData<QString()>(92, 2, QMC::AccessPublic, QMetaType::QString),
        // Method 'sendAttachment'
        QtMocHelpers::MethodData<void(const QString &, const QString &, const QString &)>(93, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 42 }, { QMetaType::QString, 94 }, { QMetaType::QString, 95 },
        }}),
        // Method 'sendAttachment'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(93, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Void, {{
            { QMetaType::QString, 42 }, { QMetaType::QString, 94 },
        }}),
        // Method 'isAudioAttachment'
        QtMocHelpers::MethodData<bool(const QString &, const QString &)>(96, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::QString, 97 }, { QMetaType::QString, 98 },
        }}),
        // Method 'computeWaveformBars'
        QtMocHelpers::MethodData<QVariantList(const QString &, int)>(99, 2, QMC::AccessPublic, 0x80000000 | 84, {{
            { QMetaType::QString, 100 }, { QMetaType::Int, 101 },
        }}),
        // Method 'computeWaveformBars'
        QtMocHelpers::MethodData<QVariantList(const QString &)>(99, 2, QMC::AccessPublic | QMC::MethodCloned, 0x80000000 | 84, {{
            { QMetaType::QString, 100 },
        }}),
        // Method 'toggleAudioPlayback'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(102, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 23 }, { QMetaType::QString, 103 },
        }}),
        // Method 'toggleAudioPlayback'
        QtMocHelpers::MethodData<void(const QString &)>(102, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Void, {{
            { QMetaType::QString, 23 },
        }}),
        // Method 'seekAudioPlayback'
        QtMocHelpers::MethodData<void(const QString &, double)>(104, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 23 }, { QMetaType::Double, 105 },
        }}),
        // Method 'startVoiceRecording'
        QtMocHelpers::MethodData<void()>(106, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'stopAndSendVoiceRecording'
        QtMocHelpers::MethodData<void()>(107, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'cancelVoiceRecording'
        QtMocHelpers::MethodData<void()>(108, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'sendMessageWithEffect'
        QtMocHelpers::MethodData<void(const QString &, const QString &)>(109, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 21 }, { QMetaType::QString, 110 },
        }}),
        // Method 'scheduleMessage'
        QtMocHelpers::MethodData<void(const QString &, qint64)>(111, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 21 }, { QMetaType::LongLong, 112 },
        }}),
        // Method 'cancelScheduledMessage'
        QtMocHelpers::MethodData<void(const QString &)>(113, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 114 },
        }}),
        // Method 'addParticipantToActiveChat'
        QtMocHelpers::MethodData<void(const QString &)>(115, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 67 },
        }}),
        // Method 'removeParticipantFromActiveChat'
        QtMocHelpers::MethodData<void(const QString &)>(116, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 67 },
        }}),
        // Method 'syncChatStateToServer'
        QtMocHelpers::MethodData<void()>(117, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'searchContacts'
        QtMocHelpers::MethodData<QVariantList(const QString &, int) const>(118, 2, QMC::AccessPublic, 0x80000000 | 84, {{
            { QMetaType::QString, 47 }, { QMetaType::Int, 86 },
        }}),
        // Method 'searchContacts'
        QtMocHelpers::MethodData<QVariantList(const QString &) const>(118, 2, QMC::AccessPublic | QMC::MethodCloned, 0x80000000 | 84, {{
            { QMetaType::QString, 47 },
        }}),
        // Method 'searchContacts'
        QtMocHelpers::MethodData<QVariantList() const>(118, 2, QMC::AccessPublic | QMC::MethodCloned, 0x80000000 | 84),
        // Method 'canonicalizeAddress'
        QtMocHelpers::MethodData<QString(const QString &)>(119, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 120 },
        }}),
        // Method 'formatHandleForDisplay'
        QtMocHelpers::MethodData<QString(const QString &)>(121, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 120 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
        // property 'connected'
        QtMocHelpers::PropertyData<bool>(122, QMetaType::Bool, QMC::DefaultPropertyFlags, 0),
        // property 'loading'
        QtMocHelpers::PropertyData<bool>(123, QMetaType::Bool, QMC::DefaultPropertyFlags, 2),
        // property 'serverStatus'
        QtMocHelpers::PropertyData<QString>(124, QMetaType::QString, QMC::DefaultPropertyFlags, 0),
        // property 'serverUrl'
        QtMocHelpers::PropertyData<QString>(30, QMetaType::QString, QMC::DefaultPropertyFlags, 1),
        // property 'secondaryServerUrl'
        QtMocHelpers::PropertyData<QString>(125, QMetaType::QString, QMC::DefaultPropertyFlags, 1),
        // property 'serverPassword'
        QtMocHelpers::PropertyData<QString>(126, QMetaType::QString, QMC::DefaultPropertyFlags, 1),
        // property 'hasPasswordConfigured'
        QtMocHelpers::PropertyData<bool>(127, QMetaType::Bool, QMC::DefaultPropertyFlags, 1),
        // property 'needsSetup'
        QtMocHelpers::PropertyData<bool>(128, QMetaType::Bool, QMC::DefaultPropertyFlags, 1),
        // property 'searchQuery'
        QtMocHelpers::PropertyData<QString>(129, QMetaType::QString, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet, 3),
        // property 'filterMode'
        QtMocHelpers::PropertyData<QString>(130, QMetaType::QString, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet, 3),
        // property 'selectedChatGuid'
        QtMocHelpers::PropertyData<QString>(131, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatTitle'
        QtMocHelpers::PropertyData<QString>(132, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatSubtitle'
        QtMocHelpers::PropertyData<QString>(133, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatService'
        QtMocHelpers::PropertyData<QString>(134, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatAvatar'
        QtMocHelpers::PropertyData<QString>(135, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatInitials'
        QtMocHelpers::PropertyData<QString>(136, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatColor'
        QtMocHelpers::PropertyData<QString>(137, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatIsGroup'
        QtMocHelpers::PropertyData<bool>(138, QMetaType::Bool, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatMuted'
        QtMocHelpers::PropertyData<bool>(139, QMetaType::Bool, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatAddress'
        QtMocHelpers::PropertyData<QString>(140, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatHasContact'
        QtMocHelpers::PropertyData<bool>(141, QMetaType::Bool, QMC::DefaultPropertyFlags, 4),
        // property 'selectedChatUnreadCount'
        QtMocHelpers::PropertyData<int>(142, QMetaType::Int, QMC::DefaultPropertyFlags, 4),
        // property 'notificationsEnabled'
        QtMocHelpers::PropertyData<bool>(143, QMetaType::Bool, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet, 5),
        // property 'omarchyDnd'
        QtMocHelpers::PropertyData<bool>(144, QMetaType::Bool, QMC::DefaultPropertyFlags, 6),
        // property 'chats'
        QtMocHelpers::PropertyData<QVariantList>(145, 0x80000000 | 84, QMC::DefaultPropertyFlags | QMC::EnumOrFlag, 7),
        // property 'pinnedChats'
        QtMocHelpers::PropertyData<QVariantList>(146, 0x80000000 | 84, QMC::DefaultPropertyFlags | QMC::EnumOrFlag, 7),
        // property 'messages'
        QtMocHelpers::PropertyData<QVariantList>(147, 0x80000000 | 84, QMC::DefaultPropertyFlags | QMC::EnumOrFlag, 10),
        // property 'totalUnread'
        QtMocHelpers::PropertyData<int>(148, QMetaType::Int, QMC::DefaultPropertyFlags, 7),
        // property 'windowActive'
        QtMocHelpers::PropertyData<bool>(149, QMetaType::Bool, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet, 12),
        // property 'hasOlderMessages'
        QtMocHelpers::PropertyData<bool>(150, QMetaType::Bool, QMC::DefaultPropertyFlags, 11),
        // property 'loadingOlderMessages'
        QtMocHelpers::PropertyData<bool>(151, QMetaType::Bool, QMC::DefaultPropertyFlags, 11),
        // property 'loadedMessageCount'
        QtMocHelpers::PropertyData<int>(152, QMetaType::Int, QMC::DefaultPropertyFlags, 10),
        // property 'historySyncDays'
        QtMocHelpers::PropertyData<int>(153, QMetaType::Int, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet, 13),
        // property 'syncingHistory'
        QtMocHelpers::PropertyData<bool>(154, QMetaType::Bool, QMC::DefaultPropertyFlags, 14),
        // property 'playingAudioGuid'
        QtMocHelpers::PropertyData<QString>(155, QMetaType::QString, QMC::DefaultPropertyFlags, 15),
        // property 'audioPlaying'
        QtMocHelpers::PropertyData<bool>(156, QMetaType::Bool, QMC::DefaultPropertyFlags, 15),
        // property 'audioProgress'
        QtMocHelpers::PropertyData<double>(157, QMetaType::Double, QMC::DefaultPropertyFlags, 15),
        // property 'audioDurationText'
        QtMocHelpers::PropertyData<QString>(158, QMetaType::QString, QMC::DefaultPropertyFlags, 15),
        // property 'recordingVoice'
        QtMocHelpers::PropertyData<bool>(159, QMetaType::Bool, QMC::DefaultPropertyFlags, 16),
        // property 'voiceRecordingSeconds'
        QtMocHelpers::PropertyData<int>(160, QMetaType::Int, QMC::DefaultPropertyFlags, 16),
        // property 'voiceRecordingWaveform'
        QtMocHelpers::PropertyData<QVariantList>(161, 0x80000000 | 84, QMC::DefaultPropertyFlags | QMC::EnumOrFlag, 16),
        // property 'scheduledMessages'
        QtMocHelpers::PropertyData<QVariantList>(162, 0x80000000 | 84, QMC::DefaultPropertyFlags | QMC::EnumOrFlag, 17),
        // property 'selectedChatParticipants'
        QtMocHelpers::PropertyData<QStringList>(163, QMetaType::QStringList, QMC::DefaultPropertyFlags, 4),
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<Backend, qt_meta_tag_ZN7BackendE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject Backend::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7BackendE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7BackendE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7BackendE_t>.metaTypes,
    nullptr
} };

void Backend::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<Backend *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->statusChanged(); break;
        case 1: _t->configChanged(); break;
        case 2: _t->loadingChanged(); break;
        case 3: _t->filterChanged(); break;
        case 4: _t->selectedChatChanged(); break;
        case 5: _t->notificationsChanged(); break;
        case 6: _t->omarchyDndChanged(); break;
        case 7: _t->chatsChanged(); break;
        case 8: _t->contactsChanged(); break;
        case 9: _t->aboutToChangeMessages(); break;
        case 10: _t->messagesChanged(); break;
        case 11: _t->olderMessagesChanged(); break;
        case 12: _t->windowActiveChanged(); break;
        case 13: _t->historySyncDaysChanged(); break;
        case 14: _t->syncingHistoryChanged(); break;
        case 15: _t->audioPlaybackChanged(); break;
        case 16: _t->voiceRecordingChanged(); break;
        case 17: _t->scheduledMessagesChanged(); break;
        case 18: _t->toastMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 19: _t->attachmentReady((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 20: _t->linkPreviewUpdated((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QVariantMap>>(_a[2]))); break;
        case 21: _t->saveServerConfig((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 22: { QString _r = _t->webUiUrl();
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 23: _t->openWebUi(); break;
        case 24: _t->setHistorySyncDays((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 25: _t->cycleHistorySyncDays(); break;
        case 26: _t->clearSelectedChat(); break;
        case 27: _t->goHome(); break;
        case 28: _t->toggleNotifications(); break;
        case 29: _t->toggleChatMute((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 30: { bool _r = _t->isChatMuted((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 31: _t->togglePinChat((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 32: { bool _r = _t->isChatPinned((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 33: _t->setSearchQuery((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 34: _t->setFilterMode((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 35: _t->refreshAll(); break;
        case 36: _t->selectChat((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 37: _t->selectNextChat(); break;
        case 38: _t->selectPrevChat(); break;
        case 39: _t->markChatRead((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 40: _t->markChatUnread((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 41: _t->markAllChatsRead(); break;
        case 42: _t->toggleChatReadUnread((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 43: _t->toggleChatRead((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 44: _t->sendMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 45: _t->sendMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 46: _t->sendReaction((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 47: _t->deleteMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 48: _t->deleteMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 49: _t->unsendMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 50: _t->unsendMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 51: _t->deleteChat((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 52: _t->deleteChat(); break;
        case 53: _t->startNewChat((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 54: _t->startNewChat((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 55: _t->openAttachment((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 56: _t->registerWindow((*reinterpret_cast<std::add_pointer_t<QObject*>>(_a[1]))); break;
        case 57: _t->loadDemoData(); break;
        case 58: _t->seedDemoData(); break;
        case 59: _t->saveContact((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[4]))); break;
        case 60: _t->syncContactsNow(); break;
        case 61: _t->loadOlderMessages(); break;
        case 62: { QString _r = _t->replaceEmojiShortcodes((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 63: { QVariantList _r = _t->searchEmojis((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[3])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 64: { QVariantList _r = _t->searchEmojis((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 65: { QVariantList _r = _t->searchEmojis((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 66: { QVariantList _r = _t->searchEmojis();
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 67: _t->copyToClipboard((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 68: _t->copyToClipboard((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 69: _t->copyTextToClipboard((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 70: { QString _r = _t->extractVerificationCode((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 71: { QString _r = _t->extractOtpCode((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 72: { QString _r = _t->pasteClipboardImageToTempFile();
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 73: _t->sendAttachment((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 74: _t->sendAttachment((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 75: { bool _r = _t->isAudioAttachment((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 76: { QVariantList _r = _t->computeWaveformBars((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 77: { QVariantList _r = _t->computeWaveformBars((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 78: _t->toggleAudioPlayback((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 79: _t->toggleAudioPlayback((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 80: _t->seekAudioPlayback((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2]))); break;
        case 81: _t->startVoiceRecording(); break;
        case 82: _t->stopAndSendVoiceRecording(); break;
        case 83: _t->cancelVoiceRecording(); break;
        case 84: _t->sendMessageWithEffect((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 85: _t->scheduleMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<qint64>>(_a[2]))); break;
        case 86: _t->cancelScheduledMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 87: _t->addParticipantToActiveChat((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 88: _t->removeParticipantFromActiveChat((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 89: _t->syncChatStateToServer(); break;
        case 90: { QVariantList _r = _t->searchContacts((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 91: { QVariantList _r = _t->searchContacts((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 92: { QVariantList _r = _t->searchContacts();
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 93: { QString _r = _t->canonicalizeAddress((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 94: { QString _r = _t->formatHandleForDisplay((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::statusChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::configChanged, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::loadingChanged, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::filterChanged, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::selectedChatChanged, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::notificationsChanged, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::omarchyDndChanged, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::chatsChanged, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::contactsChanged, 8))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::aboutToChangeMessages, 9))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::messagesChanged, 10))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::olderMessagesChanged, 11))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::windowActiveChanged, 12))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::historySyncDaysChanged, 13))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::syncingHistoryChanged, 14))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::audioPlaybackChanged, 15))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::voiceRecordingChanged, 16))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)()>(_a, &Backend::scheduledMessagesChanged, 17))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)(const QString & )>(_a, &Backend::toastMessage, 18))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)(const QString & , const QString & )>(_a, &Backend::attachmentReady, 19))
            return;
        if (QtMocHelpers::indexOfMethod<void (Backend::*)(const QString & , const QVariantMap & )>(_a, &Backend::linkPreviewUpdated, 20))
            return;
    }
    if (_c == QMetaObject::ReadProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast<bool*>(_v) = _t->connected(); break;
        case 1: *reinterpret_cast<bool*>(_v) = _t->loading(); break;
        case 2: *reinterpret_cast<QString*>(_v) = _t->serverStatus(); break;
        case 3: *reinterpret_cast<QString*>(_v) = _t->serverUrl(); break;
        case 4: *reinterpret_cast<QString*>(_v) = _t->secondaryServerUrl(); break;
        case 5: *reinterpret_cast<QString*>(_v) = _t->serverPassword(); break;
        case 6: *reinterpret_cast<bool*>(_v) = _t->hasPasswordConfigured(); break;
        case 7: *reinterpret_cast<bool*>(_v) = _t->needsSetup(); break;
        case 8: *reinterpret_cast<QString*>(_v) = _t->searchQuery(); break;
        case 9: *reinterpret_cast<QString*>(_v) = _t->filterMode(); break;
        case 10: *reinterpret_cast<QString*>(_v) = _t->selectedChatGuid(); break;
        case 11: *reinterpret_cast<QString*>(_v) = _t->selectedChatTitle(); break;
        case 12: *reinterpret_cast<QString*>(_v) = _t->selectedChatSubtitle(); break;
        case 13: *reinterpret_cast<QString*>(_v) = _t->selectedChatService(); break;
        case 14: *reinterpret_cast<QString*>(_v) = _t->selectedChatAvatar(); break;
        case 15: *reinterpret_cast<QString*>(_v) = _t->selectedChatInitials(); break;
        case 16: *reinterpret_cast<QString*>(_v) = _t->selectedChatColor(); break;
        case 17: *reinterpret_cast<bool*>(_v) = _t->selectedChatIsGroup(); break;
        case 18: *reinterpret_cast<bool*>(_v) = _t->selectedChatMuted(); break;
        case 19: *reinterpret_cast<QString*>(_v) = _t->selectedChatAddress(); break;
        case 20: *reinterpret_cast<bool*>(_v) = _t->selectedChatHasContact(); break;
        case 21: *reinterpret_cast<int*>(_v) = _t->selectedChatUnreadCount(); break;
        case 22: *reinterpret_cast<bool*>(_v) = _t->notificationsEnabled(); break;
        case 23: *reinterpret_cast<bool*>(_v) = _t->omarchyDnd(); break;
        case 24: *reinterpret_cast<QVariantList*>(_v) = _t->chats(); break;
        case 25: *reinterpret_cast<QVariantList*>(_v) = _t->pinnedChats(); break;
        case 26: *reinterpret_cast<QVariantList*>(_v) = _t->messages(); break;
        case 27: *reinterpret_cast<int*>(_v) = _t->totalUnread(); break;
        case 28: *reinterpret_cast<bool*>(_v) = _t->windowActive(); break;
        case 29: *reinterpret_cast<bool*>(_v) = _t->hasOlderMessages(); break;
        case 30: *reinterpret_cast<bool*>(_v) = _t->loadingOlderMessages(); break;
        case 31: *reinterpret_cast<int*>(_v) = _t->loadedMessageCount(); break;
        case 32: *reinterpret_cast<int*>(_v) = _t->historySyncDays(); break;
        case 33: *reinterpret_cast<bool*>(_v) = _t->syncingHistory(); break;
        case 34: *reinterpret_cast<QString*>(_v) = _t->playingAudioGuid(); break;
        case 35: *reinterpret_cast<bool*>(_v) = _t->audioPlaying(); break;
        case 36: *reinterpret_cast<double*>(_v) = _t->audioProgress(); break;
        case 37: *reinterpret_cast<QString*>(_v) = _t->audioDurationText(); break;
        case 38: *reinterpret_cast<bool*>(_v) = _t->recordingVoice(); break;
        case 39: *reinterpret_cast<int*>(_v) = _t->voiceRecordingSeconds(); break;
        case 40: *reinterpret_cast<QVariantList*>(_v) = _t->voiceRecordingWaveform(); break;
        case 41: *reinterpret_cast<QVariantList*>(_v) = _t->scheduledMessages(); break;
        case 42: *reinterpret_cast<QStringList*>(_v) = _t->selectedChatParticipants(); break;
        default: break;
        }
    }
    if (_c == QMetaObject::WriteProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 8: _t->setSearchQuery(*reinterpret_cast<QString*>(_v)); break;
        case 9: _t->setFilterMode(*reinterpret_cast<QString*>(_v)); break;
        case 22: _t->setNotificationsEnabled(*reinterpret_cast<bool*>(_v)); break;
        case 28: _t->setWindowActive(*reinterpret_cast<bool*>(_v)); break;
        case 32: _t->setHistorySyncDays(*reinterpret_cast<int*>(_v)); break;
        default: break;
        }
    }
}

const QMetaObject *Backend::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *Backend::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7BackendE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int Backend::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 95)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 95;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 95)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 95;
    }
    if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 43;
    }
    return _id;
}

// SIGNAL 0
void Backend::statusChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void Backend::configChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void Backend::loadingChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void Backend::filterChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void Backend::selectedChatChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void Backend::notificationsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void Backend::omarchyDndChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void Backend::chatsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void Backend::contactsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}

// SIGNAL 9
void Backend::aboutToChangeMessages()
{
    QMetaObject::activate(this, &staticMetaObject, 9, nullptr);
}

// SIGNAL 10
void Backend::messagesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void Backend::olderMessagesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 11, nullptr);
}

// SIGNAL 12
void Backend::windowActiveChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 12, nullptr);
}

// SIGNAL 13
void Backend::historySyncDaysChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 13, nullptr);
}

// SIGNAL 14
void Backend::syncingHistoryChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 14, nullptr);
}

// SIGNAL 15
void Backend::audioPlaybackChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 15, nullptr);
}

// SIGNAL 16
void Backend::voiceRecordingChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 16, nullptr);
}

// SIGNAL 17
void Backend::scheduledMessagesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 17, nullptr);
}

// SIGNAL 18
void Backend::toastMessage(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 18, nullptr, _t1);
}

// SIGNAL 19
void Backend::attachmentReady(const QString & _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 19, nullptr, _t1, _t2);
}

// SIGNAL 20
void Backend::linkPreviewUpdated(const QString & _t1, const QVariantMap & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 20, nullptr, _t1, _t2);
}
QT_WARNING_POP
