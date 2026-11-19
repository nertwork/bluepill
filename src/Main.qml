import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

ApplicationWindow {
    id: root
    // Only known SMS/RCS threads are green; an unresolved service stays blue
    // so iMessage chats don't flash green while metadata loads.
    readonly property bool selectedChatIsGreen: backend.selectedChatService === "SMS" || backend.selectedChatService === "RCS"
    visible: true
    width: 1180
    height: 820
    minimumWidth: 640
    minimumHeight: 540
    title: "BluePill"
    color: theme.background

    readonly property bool isCompactWindow: root.width < 1120
    readonly property bool isNarrowWindow: root.width < 920

    property string replyTargetGuid: ""
    property string replyTargetSender: ""
    property string replyTargetText: ""
    // Composer edit mode: the sent message being edited ("" when not editing)
    property string editTargetGuid: ""
    // Ticks so Edit/Unsend disappear once a message is older than the edit window
    property double nowMs: Date.now()
    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: root.nowMs = Date.now()
    }
    function canEditOrUnsend(m) {
        if (!m || !m.isFromMe || !(m.isIMessage || m.service === "iMessage")) return false
        var g = m.guid || ""
        if (g.length === 0 || g.indexOf("temp-") === 0 || m.isFailed) return false
        var sent = Number(m.dateCreated || 0)
        return sent > 0 && root.nowMs - sent < backend.messageEditWindowMs
    }
    // The unsent draft that was in the composer when editing started
    property string preEditDraft: ""
    function startEditing(guid, text) {
        root.replyTargetGuid = ""
        root.replyTargetSender = ""
        root.replyTargetText = ""
        if (root.editTargetGuid === "") root.preEditDraft = composerInput.text
        effectMenuPopup.visible = false
        scheduleMenuPopup.visible = false
        root.editTargetGuid = guid
        composerInput.text = text || ""
        composerInput.forceActiveFocus()
        composerInput.cursorPosition = composerInput.text.length
    }
    function cancelEditing() {
        if (root.editTargetGuid === "") return
        root.editTargetGuid = ""
        composerInput.text = root.preEditDraft
        root.preEditDraft = ""
    }
    property string stagedAttachmentPath: ""
    property string stagedAttachmentName: ""
    property string stagedAttachmentChatGuid: ""
    property var chatAttachmentDrafts: ({})
    property var chatDrafts: ({})
    property string lastDraftChatGuid: ""
    property string hoveredActionMessageGuid: ""
    property string pinnedActionMessageGuid: ""
    // The photo that was right-clicked, so Copy Image copies that one
    property string pinnedActionAttachmentGuid: ""
    property bool showTapbackEmojiPicker: false
    property string tapbackEmojiTargetGuid: ""
    property string tapbackEmojiQuery: ""
    property string tapbackEmojiCategory: "All"
    property real tapbackEmojiPopupX: 0
    property real tapbackEmojiPopupY: 0
    property Item previousFocusBeforeTapbackPicker: null
    property string settingsActiveTab: "all"
    // Shows on hover when backend.hoverMessageActions is enabled (default true), and always when pinned via right-click.
    readonly property string activeActionMessageGuid: pinnedActionMessageGuid !== ""
                                                      ? pinnedActionMessageGuid
                                                      : (backend.hoverMessageActions ? hoveredActionMessageGuid : "")

    function openTapbackEmojiPicker(msgGuid, xPos, yPos) {
        if (!msgGuid || msgGuid.length === 0) return
        if (!root.showTapbackEmojiPicker && root.activeFocusItem && root.activeFocusItem !== tapbackEmojiSearchField) {
            root.previousFocusBeforeTapbackPicker = root.activeFocusItem
        }
        root.pinnedActionMessageGuid = msgGuid
        root.tapbackEmojiTargetGuid = msgGuid
        root.tapbackEmojiQuery = ""
        root.tapbackEmojiCategory = "All"
        root.tapbackEmojiPopupX = Math.max(12, Math.min(xPos || 120, root.width - 360))
        root.tapbackEmojiPopupY = Math.max(56, Math.min(yPos || 120, root.height - 300))
        root.showTapbackEmojiPicker = true
        Qt.callLater(function() {
            if (root.showTapbackEmojiPicker) {
                tapbackEmojiSearchField.forceActiveFocus()
            }
        })
    }

    function closeTapbackEmojiPicker() {
        var wasOpen = root.showTapbackEmojiPicker
        root.showTapbackEmojiPicker = false
        root.tapbackEmojiTargetGuid = ""
        root.tapbackEmojiQuery = ""
        if (wasOpen && root.previousFocusBeforeTapbackPicker) {
            var prev = root.previousFocusBeforeTapbackPicker
            root.previousFocusBeforeTapbackPicker = null
            Qt.callLater(function() {
                if (prev && prev.visible) {
                    prev.forceActiveFocus()
                }
            })
        } else {
            root.previousFocusBeforeTapbackPicker = null
        }
    }

    function selectTapbackCustomEmoji(emoji) {
        var cGuid = backend.selectedChatGuid
        var mGuid = root.tapbackEmojiTargetGuid || root.activeActionMessageGuid
        root.closeTapbackEmojiPicker()
        root.clearActionMenu()
        if (cGuid.length > 0 && mGuid.length > 0 && emoji && emoji.length > 0) {
            backend.sendReaction(cGuid, mGuid, emoji)
        }
    }

    function clearActionMenu() {
        closeTapbackEmojiPicker()
        pinnedActionMessageGuid = ""
        pinnedActionAttachmentGuid = ""
        hoveredActionMessageGuid = ""
    }

    function isStagedAttachmentForChat(chatGuid) {
        if (!chatGuid || chatGuid.length === 0 || root.stagedAttachmentPath.length === 0) return false
        if (root.stagedAttachmentChatGuid.length === 0) return false
        return root.stagedAttachmentChatGuid === chatGuid || backend.isSameConversation(root.stagedAttachmentChatGuid, chatGuid)
    }

    function stageAttachment(attPath, attName, chatGuid) {
        var targetGuid = (chatGuid && chatGuid.length > 0) ? chatGuid : (backend.selectedChatGuid || "")
        if (!attPath || attPath.length === 0 || targetGuid.length === 0) return
        root.stagedAttachmentPath = attPath
        root.stagedAttachmentName = (attName && attName.length > 0) ? attName : attPath.split("/").pop()
        root.stagedAttachmentChatGuid = targetGuid
        var attDrafts = root.chatAttachmentDrafts || ({})
        var canonGuid = backend.resolveCanonicalChatGuid(targetGuid) || targetGuid
        attDrafts[canonGuid] = {
            path: root.stagedAttachmentPath,
            name: root.stagedAttachmentName,
            chatGuid: targetGuid
        }
        if (canonGuid !== targetGuid && attDrafts[targetGuid] !== undefined) {
            delete attDrafts[targetGuid]
        }
        root.chatAttachmentDrafts = attDrafts
    }

    function clearStagedAttachment() {
        var prevGuid = stagedAttachmentChatGuid
        var curGuid = backend.selectedChatGuid || ""
        stagedAttachmentPath = ""
        stagedAttachmentName = ""
        stagedAttachmentChatGuid = ""
        var attDrafts = root.chatAttachmentDrafts || ({})
        var keys = Object.keys(attDrafts)
        var changed = false
        for (var i = 0; i < keys.length; ++i) {
            var k = keys[i]
            if ((prevGuid.length > 0 && (k === prevGuid || backend.isSameConversation(k, prevGuid))) ||
                (curGuid.length > 0 && (k === curGuid || backend.isSameConversation(k, curGuid)))) {
                delete attDrafts[k]
                changed = true
            }
        }
        if (changed) {
            root.chatAttachmentDrafts = attDrafts
        }
    }

    function isVoiceMemoForSelectedChat() {
        if (!backend.selectedChatGuid || backend.selectedChatGuid.length === 0) return false
        if (!backend.recordingVoice && !backend.hasRecordedVoiceMemo) return false
        var recGuid = backend.recordedVoiceChatGuid || ""
        if (recGuid.length > 0 && !backend.isSameConversation(recGuid, backend.selectedChatGuid)) {
            return false
        }
        return backend.canSendRecordedVoiceMemoToSelectedChat()
    }

    function sendComposerContent() {
        if (backend.selectedChatGuid.length === 0) return
        if (root.editTargetGuid !== "") {
            var editGuid = root.editTargetGuid
            var edited = composerInput.text.trim()
            root.editTargetGuid = ""
            composerInput.text = root.preEditDraft
            root.preEditDraft = ""
            if (edited.length > 0) backend.editMessage(editGuid, edited)
            return
        }
        if (root.stagedAttachmentPath.length > 0 && !root.isStagedAttachmentForChat(backend.selectedChatGuid)) {
            root.stagedAttachmentPath = ""
            root.stagedAttachmentName = ""
            root.stagedAttachmentChatGuid = ""
        }
        if (root.stagedAttachmentPath.length > 0) {
            var attPath = root.stagedAttachmentPath
            var targetChatGuid = backend.selectedChatGuid
            var caption = composerInput.text.trim()
            root.clearStagedAttachment()
            composerInput.text = ""
            root.showEmojiPicker = false
            root.emojiAutocompleteResults = []
            root.replyTargetGuid = ""
            root.replyTargetSender = ""
            root.replyTargetText = ""
            backend.sendAttachment(targetChatGuid, attPath, caption)
            messageListView.scrollToBottom()
        } else if (composerInput.text.trim().length > 0) {
            backend.sendMessage(backend.selectedChatGuid, composerInput.text, root.replyTargetGuid)
            messageListView.scrollToBottom()
            composerInput.text = ""
            root.showEmojiPicker = false
            root.emojiAutocompleteResults = []
            root.replyTargetGuid = ""
            root.replyTargetSender = ""
            root.replyTargetText = ""
        } else if (backend.hasRecordedVoiceMemo && root.isVoiceMemoForSelectedChat()) {
            backend.sendRecordedVoiceMemo()
            messageListView.scrollToBottom()
        }
    }

    property bool _reconcilingAudioDevices: false

    function hasAudioDeviceId(list, targetId) {
        if (!list || list.length === 0) return false
        var needle = targetId || ""
        for (var i = 0; i < list.length; i++) {
            if ((list[i].id || "") === needle) return true
        }
        return false
    }

    function findAudioDeviceIndex(list, targetId) {
        if (!list || list.length === 0) return 0
        var needle = targetId || ""
        for (var i = 0; i < list.length; i++) {
            if ((list[i].id || "") === needle) return i
        }
        for (var j = 0; j < list.length; j++) {
            if ((list[j].id || "") === "") return j
        }
        return 0
    }

    function hasNonFallbackAudioDevice(list) {
        if (!list || list.length === 0) return false
        for (var i = 0; i < list.length; i++) {
            if ((list[i].id || "").length > 0) return true
        }
        return false
    }

    function reconcileAudioDeviceSelections() {
        if (root._reconcilingAudioDevices) return
        root._reconcilingAudioDevices = true
        try {
            var inList = backend.audioInputDevices
            if (backend.audioInputDiscoverySucceeded && root.hasNonFallbackAudioDevice(inList) &&
                backend.selectedAudioInputDevice && backend.selectedAudioInputDevice.length > 0 &&
                !root.hasAudioDeviceId(inList, backend.selectedAudioInputDevice)) {
                backend.selectedAudioInputDevice = root.hasAudioDeviceId(inList, "") ? "" : (inList[0].id || "")
            }
            var outList = backend.audioOutputDevices
            if (backend.audioOutputDiscoverySucceeded && root.hasNonFallbackAudioDevice(outList) &&
                backend.selectedAudioOutputDevice && backend.selectedAudioOutputDevice.length > 0 &&
                !root.hasAudioDeviceId(outList, backend.selectedAudioOutputDevice)) {
                backend.selectedAudioOutputDevice = root.hasAudioDeviceId(outList, "") ? "" : (outList[0].id || "")
            }
        } finally {
            root._reconcilingAudioDevices = false
        }
    }

    property string activeScreenEffect: ""

    Timer {
        id: screenEffectTimer
        interval: 2550
        repeat: false
        onTriggered: root.activeScreenEffect = ""
    }

    function triggerExpressiveEffect(effectText) {
        var eff = (effectText || "").toLowerCase()
        if (eff.indexOf("confetti") !== -1) {
            root.activeScreenEffect = "confetti"
            screenEffectTimer.restart()
            effectAnim.restart()
        } else if (eff.indexOf("firework") !== -1) {
            root.activeScreenEffect = "fireworks"
            screenEffectTimer.restart()
            effectAnim.restart()
        } else if (eff.indexOf("laser") !== -1) {
            root.activeScreenEffect = "lasers"
            screenEffectTimer.restart()
            effectAnim.restart()
        } else if (eff.indexOf("balloon") !== -1) {
            root.activeScreenEffect = "balloons"
            screenEffectTimer.restart()
            effectAnim.restart()
        } else if (eff.indexOf("heart") !== -1 || eff.indexOf("love") !== -1) {
            root.activeScreenEffect = "hearts"
            screenEffectTimer.restart()
            effectAnim.restart()
        }
    }

    function focusAndScrollChat() {
        messageListView.scrollToBottom()
        composerInput.forceActiveFocus()
    }

    function scrollMessagesPageUp() {
        if (!backend.selectedChatGuid || messageListView.count === 0) return
        root.hoveredActionMessageGuid = ""
        messageListView.restoringScrollPosition = false
        messageListView.restorePassesRemaining = 0
        scrollRestoreTimer.stop()
        messageListView.stickToBottom = false
        messageListView.savedStickToBottom = false
        messageListView.settlingToBottom = false
        messageListView.settlePassesRemaining = 0
        bottomSettleTimer.stop()
        if (messageListView.atYBeginning) {
            messageListView.positionViewAtBeginning()
            if (backend.hasOlderMessages && !backend.loadingOlderMessages) {
                messageListView.prevContentHeight = messageListView.contentHeight
                messageListView.prevContentY = messageListView.contentY
                backend.loadOlderMessages()
            }
            return
        }
        messageListView.prevContentHeight = 0
        messageListView.prevContentY = 0
        var step = Math.max(120, Math.round(messageListView.height * 0.85))
        var minY = messageListView.originY - messageListView.topMargin
        var targetY = messageListView.contentY - step
        if (targetY <= minY) {
            messageListView.contentY = minY
            if (messageListView.atYBeginning) {
                messageListView.positionViewAtBeginning()
            }
        } else {
            messageListView.contentY = targetY
            messageListView.returnToBounds()
        }
    }

    function scrollMessagesPageDown() {
        if (!backend.selectedChatGuid || messageListView.count === 0) return
        root.hoveredActionMessageGuid = ""
        if (messageListView.atYEnd) {
            messageListView.scrollToBottom()
            return
        }
        var step = Math.max(120, Math.round(messageListView.height * 0.85))
        var maxY = Math.max(messageListView.originY - messageListView.topMargin,
                            messageListView.originY + messageListView.contentHeight + messageListView.bottomMargin - messageListView.height)
        var targetY = messageListView.contentY + step
        if (targetY >= maxY - 8) {
            messageListView.scrollToBottom()
        } else {
            messageListView.restoringScrollPosition = false
            messageListView.restorePassesRemaining = 0
            scrollRestoreTimer.stop()
            messageListView.stickToBottom = false
            messageListView.savedStickToBottom = false
            messageListView.settlingToBottom = false
            messageListView.settlePassesRemaining = 0
            bottomSettleTimer.stop()
            messageListView.prevContentHeight = 0
            messageListView.prevContentY = 0
            messageListView.contentY = targetY
            messageListView.returnToBounds()
            if (messageListView.atYEnd) {
                messageListView.stickToBottom = true
                messageListView.savedStickToBottom = true
            }
        }
    }

    function scrollMessagesToTop() {
        if (!backend.selectedChatGuid || messageListView.count === 0) return
        root.hoveredActionMessageGuid = ""
        messageListView.scrollToTop()
        Qt.callLater(function() {
            if (messageListView.count > 0 && !messageListView.stickToBottom) {
                messageListView.positionViewAtBeginning()
            }
        })
    }

    function scrollMessagesToBottom() {
        if (!backend.selectedChatGuid || messageListView.count === 0) return
        root.hoveredActionMessageGuid = ""
        messageListView.scrollToBottom()
        Qt.callLater(function() {
            if (messageListView.count > 0 && messageListView.stickToBottom) {
                messageListView.positionViewAtEnd()
            }
        })
    }

    function scrollMessagesStepUp() {
        if (!backend.selectedChatGuid || messageListView.count === 0) return
        root.hoveredActionMessageGuid = ""
        messageListView.restoringScrollPosition = false
        messageListView.restorePassesRemaining = 0
        scrollRestoreTimer.stop()
        messageListView.stickToBottom = false
        messageListView.savedStickToBottom = false
        messageListView.settlingToBottom = false
        messageListView.settlePassesRemaining = 0
        bottomSettleTimer.stop()
        messageListView.prevContentHeight = 0
        messageListView.prevContentY = 0
        var minY = messageListView.originY - messageListView.topMargin
        var targetY = messageListView.contentY - 64
        if (targetY <= minY) {
            messageListView.contentY = minY
            if (messageListView.atYBeginning) {
                messageListView.positionViewAtBeginning()
            }
        } else {
            messageListView.contentY = targetY
            messageListView.returnToBounds()
        }
    }

    function scrollMessagesStepDown() {
        if (!backend.selectedChatGuid || messageListView.count === 0) return
        root.hoveredActionMessageGuid = ""
        if (messageListView.atYEnd) {
            messageListView.scrollToBottom()
            return
        }
        var maxY = Math.max(messageListView.originY - messageListView.topMargin,
                            messageListView.originY + messageListView.contentHeight + messageListView.bottomMargin - messageListView.height)
        var targetY = messageListView.contentY + 64
        if (targetY >= maxY - 4) {
            messageListView.scrollToBottom()
        } else {
            messageListView.restoringScrollPosition = false
            messageListView.restorePassesRemaining = 0
            scrollRestoreTimer.stop()
            messageListView.stickToBottom = false
            messageListView.savedStickToBottom = false
            messageListView.settlingToBottom = false
            messageListView.settlePassesRemaining = 0
            bottomSettleTimer.stop()
            messageListView.prevContentHeight = 0
            messageListView.prevContentY = 0
            messageListView.contentY = targetY
            messageListView.returnToBounds()
        }
    }

    property bool showShortcutsModal: false
    property bool showNewChatModal: false
    property bool showContactModal: false
    // Thread view (opened from a quoted reply or "N Replies"), and the message to flash after a jump
    property string threadRootGuid: ""
    property string threadChatGuid: ""
    property bool showThreadView: false
    function closeThread() {
        root.showThreadView = false
        root.threadRootGuid = ""
        root.threadChatGuid = ""
    }
    // A thread belongs to the conversation it was opened from
    Connections {
        target: backend
        function onSelectedChatChanged() {
            if (root.showThreadView && backend.selectedChatGuid !== root.threadChatGuid) root.closeThread()
        }
    }
    property string highlightGuid: ""
    Timer {
        id: highlightTimer
        interval: 1600
        onTriggered: root.highlightGuid = ""
    }
    function openThread(rootGuid) {
        if (!rootGuid || rootGuid.length === 0) return
        root.clearActionMenu()
        root.threadRootGuid = rootGuid
        root.threadChatGuid = backend.selectedChatGuid
        root.showThreadView = true
        threadReplyInput.text = ""
        threadReplyInput.forceActiveFocus()
    }
    function sendThreadReply() {
        var text = threadReplyInput.text.trim()
        if (text.length === 0 || backend.selectedChatGuid.length === 0 || root.threadRootGuid.length === 0) return
        if (backend.selectedChatGuid !== root.threadChatGuid) { root.closeThread(); return }
        backend.sendMessage(backend.selectedChatGuid, text, root.threadRootGuid)
        threadReplyInput.text = ""
        threadList.positionViewAtEnd()
    }
    function jumpToMessage(guid) {
        var idx = backend.messageIndexForGuid(guid || "")
        if (idx < 0) {
            root.showToast("↑ " + i18n.tr("That message isn't loaded yet; scroll up to load older messages"))
            return
        }
        root.closeThread()
        // Cancel any pending restore/settle pass so it can't move the target back out of view
        messageListView.restoringScrollPosition = false
        messageListView.restorePassesRemaining = 0
        scrollRestoreTimer.stop()
        messageListView.settlingToBottom = false
        messageListView.settlePassesRemaining = 0
        bottomSettleTimer.stop()
        messageListView.prevContentHeight = 0
        messageListView.prevContentY = 0
        messageListView.stickToBottom = false
        messageListView.savedStickToBottom = false
        messageListView.positionViewAtIndex(idx, ListView.Center)
        messageListView.snapshotScrollState()
        root.highlightGuid = backend.messages[idx].guid || ""
        highlightTimer.restart()
    }
    property bool showDeleteChatModal: false
    property bool showServerConfigModal: false
    property bool showServerPassword: false

    onShowServerConfigModalChanged: {
        if (showServerConfigModal) {
            root.reconcileAudioDeviceSelections()
            root.loadPrimaryServerFields(backend.configuredServerUrl)
            secondaryServerUrlInput.text = backend.secondaryServerUrl
            serverPasswordInput.text = backend.serverPassword
            root.showServerPassword = false
        }
    }

    // True once the user types in the primary URL/Port fields (blocks discovery auto-fill).
    property bool primaryServerFieldsEdited: false
    property bool serverPortInvalid: false

    // One save path for every Save/Return action: an invalid non-empty port keeps
    // Settings open instead of silently falling back to 1235.
    function savePrimaryServerSettings() {
        var portText = serverPortInput.text.trim()
        if (portText.length > 0 && !primaryServerFields.isSshUrl) {
            var p = Number(portText)
            if (!/^\d+$/.test(portText) || p < 1 || p > 65535) {
                root.serverPortInvalid = true
                serverPortInput.forceActiveFocus()
                return false
            }
        }
        root.serverPortInvalid = false
        backend.saveServerConfig(root.composedPrimaryServerUrl(), secondaryServerUrlInput.text, serverPasswordInput.text)
        root.showServerConfigModal = false
        return true
    }

    // Primary server URL is edited as two fields: URL (no port) + Port.
    function loadPrimaryServerFields(url) {
        var raw = url || ""
        root.primaryServerFieldsEdited = false
        root.serverPortInvalid = false
        backend.verifyTailnetHostForUrl(raw)
        var parts = backend.splitServerUrl(raw, true)
        var explicit = backend.splitServerUrl(raw, false).port
        serverUrlInput.text = parts.url
        serverPortInput.text = parts.port
        // A saved URL with no port has been using the scheme's port (older
        // configs, reverse proxies). Show it so re-saving doesn't move it to 1235.
        if (!explicit) {
            if (/^https:\/\//i.test(raw)) serverPortInput.text = "443"
            else if (/^http:\/\//i.test(raw)) serverPortInput.text = "80"
        }
    }
    // Move a port typed inline ("host:8080") into the Port field.
    function carryOverPrimaryServerPort() {
        var parts = backend.splitServerUrl(serverUrlInput.text, false)
        if (parts.port && parts.port.length > 0) {
            serverUrlInput.text = parts.url
            serverPortInput.text = parts.port
        }
    }
    function composedPrimaryServerUrl() {
        return backend.composeServerUrl(serverUrlInput.text, serverPortInput.text)
    }

    function openServerConfigModal() {
        root.clearActionMenu()
        root.showShortcutsModal = false
        root.showContactModal = false
        root.showNewChatModal = false
        root.showEmojiPicker = false
        root.reconcileAudioDeviceSelections()
        root.loadPrimaryServerFields(backend.configuredServerUrl)
        secondaryServerUrlInput.text = backend.secondaryServerUrl
        serverPasswordInput.text = backend.serverPassword
        root.showServerPassword = false
        root.showServerConfigModal = true
        serverUrlInput.forceActiveFocus()
        backend.discoverTailscaleRelays()
    }

    Timer {
        id: initialSetupCheckTimer
        interval: 50
        running: true
        repeat: false
        onTriggered: {
            if (backend.needsSetup) {
                root.openServerConfigModal()
            }
        }
    }
    property bool showPhotoViewer: false
    // MultiEffect mask for rounded image corners. Loaded from its own file so a
    // missing QtQuick.Effects module (packaged separately on some distros) only
    // means square corners, not a broken window.
    readonly property Component roundedCornerMaskComponent: {
        var c = Qt.createComponent("qrc:/RoundedCornerMask.qml")
        return c.status === Component.Ready ? c : null
    }
    property bool photoViewerFullScreen: false
    property var photoViewerItems: []
    property int photoViewerIndex: 0
    property real photoViewerZoom: 1.0
    property real photoViewerPanX: 0
    property real photoViewerPanY: 0

    function cleanNoteDisplayBody(title, body, syncSource) {
        if (syncSource === "local_hub") {
            return String(body || "").trim()
        }
        var t = String(title || "").replace(/\ufffc/g, "").replace(/\r/g, "").trim()
        var b = String(body || "").replace(/\ufffc/g, "").replace(/\r/g, "").trim()
        if (!b) return ""
        b = b.replace(/(?:\n+\s*(?:Wz&M|\}\$\}h|dNP\}M|P\.qFB|sqeL|iL\tH)[\s\S]*)$/, "").trim()
        if (t.length > 0) {
            if (b.length > t.length && b.slice(1).indexOf(t) === 0 &&
                (b.length === t.length + 1 || b.charAt(1 + t.length) === "\n")) {
                b = b.slice(1).trim()
            }
            if (b === t) return ""
            if (b.indexOf(t + "\n") === 0) {
                b = b.slice(t.length + 1).replace(/^\n+/, "").trim()
            }
        }
        return b
    }

    function openPhotoViewer(items, startIndex) {
        var list = []
        if (items && items.length !== undefined) {
            for (var i = 0; i < items.length; ++i) {
                var it = items[i]
                if (!it) continue
                list.push({
                    guid: it.guid || "",
                    name: it.name || ("Photo " + (i + 1)),
                    mimeType: it.mimeType || "",
                    localUrl: it.localUrl || "",
                    previewUrl: it.previewUrl || "",
                    sourceUrl: it.sourceUrl || "",
                    isCloudEvicted: !!it.isCloudEvicted,
                    canUnpurge: it.canUnpurge !== false,
                    canRequestAttachment: !it.isCloudEvicted || it.canUnpurge !== false
                })
            }
        }
        if (list.length === 0) return
        var idx = Math.max(0, Math.min(startIndex || 0, list.length - 1))
        root.photoViewerItems = list
        root.photoViewerIndex = idx
        root.photoViewerZoom = 1.0
        root.photoViewerPanX = 0
        root.photoViewerPanY = 0
        root.showPhotoViewer = true
        photoViewerModal.forceActiveFocus()
        Qt.callLater(function() {
            if (root.showPhotoViewer) {
                photoViewerModal.forceActiveFocus()
            }
        })
        var cur = list[idx]
        if (cur && (!cur.localUrl || cur.localUrl.length === 0) && cur.guid && cur.guid.length > 0 && cur.canRequestAttachment) {
            if (cur.isCloudEvicted) {
                backend.downloadCloudAttachment(cur.guid)
            } else {
                backend.openAttachment(cur.guid, cur.name || "")
            }
        }
    }

    function closePhotoViewer() {
        root.showPhotoViewer = false
        root.photoViewerZoom = 1.0
        root.photoViewerPanX = 0
        root.photoViewerPanY = 0
        root.focusAfterOverlay()
    }

    function handlePhotoViewerKey(event) {
        if (!root.showPhotoViewer) return false
        var k = event.key
        var t = event.text || ""
        if (k === Qt.Key_Left || k === Qt.Key_Up || k === Qt.Key_Backtab) {
            root.prevPhotoInViewer()
            event.accepted = true
            return true
        }
        if (k === Qt.Key_Right || k === Qt.Key_Down || k === Qt.Key_Tab || k === Qt.Key_Space) {
            root.nextPhotoInViewer()
            event.accepted = true
            return true
        }
        if (k === Qt.Key_Plus || k === Qt.Key_Equal || t === "+" || t === "=") {
            root.zoomPhotoViewer(0.25)
            event.accepted = true
            return true
        }
        if (k === Qt.Key_Minus || k === Qt.Key_Underscore || t === "-" || t === "_") {
            root.zoomPhotoViewer(-0.25)
            event.accepted = true
            return true
        }
        if (k === Qt.Key_0 || t === "0") {
            root.photoViewerZoom = 1.0
            root.photoViewerPanX = 0
            root.photoViewerPanY = 0
            event.accepted = true
            return true
        }
        if ((event.modifiers & Qt.ControlModifier) && k === Qt.Key_C) {
            var cp = photoViewerModal.currentPhoto
            if (cp) {
                backend.copyImageToClipboard(cp.localUrl || "", cp.guid || "")
            }
            event.accepted = true
            return true
        }
        if (k === Qt.Key_Escape) {
            root.closePhotoViewer()
            event.accepted = true
            return true
        }
        return false
    }

    function prevPhotoInViewer() {
        if (!root.photoViewerItems || root.photoViewerItems.length <= 1) return
        root.photoViewerIndex = (root.photoViewerIndex - 1 + root.photoViewerItems.length) % root.photoViewerItems.length
        root.photoViewerZoom = 1.0
        root.photoViewerPanX = 0
        root.photoViewerPanY = 0
        var cur = root.photoViewerItems[root.photoViewerIndex]
        if (cur && (!cur.localUrl || cur.localUrl.length === 0) && cur.guid && cur.guid.length > 0 && cur.canRequestAttachment) {
            if (cur.isCloudEvicted) {
                backend.downloadCloudAttachment(cur.guid)
            } else {
                backend.openAttachment(cur.guid, cur.name || "")
            }
        }
    }

    function nextPhotoInViewer() {
        if (!root.photoViewerItems || root.photoViewerItems.length <= 1) return
        root.photoViewerIndex = (root.photoViewerIndex + 1) % root.photoViewerItems.length
        root.photoViewerZoom = 1.0
        root.photoViewerPanX = 0
        root.photoViewerPanY = 0
        var cur = root.photoViewerItems[root.photoViewerIndex]
        if (cur && (!cur.localUrl || cur.localUrl.length === 0) && cur.guid && cur.guid.length > 0 && cur.canRequestAttachment) {
            if (cur.isCloudEvicted) {
                backend.downloadCloudAttachment(cur.guid)
            } else {
                backend.openAttachment(cur.guid, cur.name || "")
            }
        }
    }

    function zoomPhotoViewer(delta) {
        var nextZoom = Math.max(0.5, Math.min(4.0, Math.round((root.photoViewerZoom + delta) * 100) / 100))
        root.photoViewerZoom = nextZoom
        if (nextZoom <= 1.0) {
            root.photoViewerPanX = 0
            root.photoViewerPanY = 0
        }
    }

    function zoomInPhotoViewer() {
        root.zoomPhotoViewer(0.25)
    }

    function zoomOutPhotoViewer() {
        root.zoomPhotoViewer(-0.25)
    }

    function resetZoomPhotoViewer() {
        root.photoViewerZoom = 1.0
        root.photoViewerPanX = 0
        root.photoViewerPanY = 0
    }
    property string pendingDeleteChatGuid: ""
    property string pendingDeleteChatTitle: "" 
    property string contactModalFirstName: ""
    property string contactModalLastName: ""
    property string contactModalPhone: ""
    property string contactModalEmail: ""
    // Populated fields of the Mac contact ({label, value}), shown read-only in the dialog
    property var contactModalRows: []
    // Contact / conversation info (read-only), opened from the header name
    property bool showContactInfo: false
    property int contactInfoTab: 0          // 0 Details, 1 Photos, 2 Links, 3 Files
    property var contactInfoDetails: ({})
    property var contactInfoMedia: ({ photos: [], links: [], files: [] })
    function refreshContactInfo() {
        root.contactInfoDetails = backend.selectedChatIsGroup ? ({ found: false }) : backend.contactDetails(backend.selectedChatAddress)
        root.contactInfoMedia = backend.conversationMedia(backend.selectedChatGuid)
    }
    function openContactInfo(tab) {
        if (backend.selectedChatGuid.length === 0) return
        root.refreshContactInfo()
        root.contactInfoTab = tab || 0
        root.showContactInfo = true
        // Keys go to the card, not the (covered) composer with its draft
        contactInfoModal.forceActiveFocus()
    }
    function closeContactInfo() {
        if (!root.showContactInfo) return
        root.showContactInfo = false
        if (backend.selectedChatGuid.length > 0) composerInput.forceActiveFocus()
    }
    // Where focus goes when the editor or photo viewer closes: back to the card if it's open
    function focusAfterOverlay() {
        if (root.showContactInfo) contactInfoModal.forceActiveFocus()
        else if (backend.selectedChatGuid.length > 0) composerInput.forceActiveFocus()
    }
    // Closing the contact editor backs out to the card, with the saved details
    onShowContactModalChanged: {
        if (!showContactModal && showContactInfo) {
            root.refreshContactInfo()
            root.focusAfterOverlay()
        }
    }
    function openContactModal(address, title) {
        var d = backend.contactDetails(address || "")
        if (d.found) {
            // Fill from the Mac contact instead of guessing from the chat title
            root.contactModalFirstName = d.firstName || ""
            root.contactModalLastName = d.lastName || ""
            root.contactModalPhone = d.phone || ""
            root.contactModalEmail = d.email || ""
            root.contactModalRows = d.rows || []
        } else {
            var parts = (title || "").split(" ")
            var isEmail = (address || "").indexOf("@") >= 0
            root.contactModalFirstName = parts[0] || ""
            root.contactModalLastName = parts.slice(1).join(" ") || ""
            root.contactModalPhone = isEmail ? "" : (address || "")
            root.contactModalEmail = isEmail ? address : ""
            root.contactModalRows = []
        }
        root.showContactModal = true
    }
    property bool showEmojiPicker: false
    property string emojiPickerCategory: "All"
    property string emojiPickerQuery: ""
    property string emojiShortcodePrefix: ""
    property int emojiShortcodeStart: -1
    property var emojiAutocompleteResults: []
    property int selectedEmojiIndex: 0
    property bool isEmojiCycling: false
    property int emojiCycleStartPos: -1
    property int emojiCycleLength: 0
    property string activeToastText: ""

    // Format 10/11-digit US phone numbers in subtitles (+14155550142 -> +1 (415) 555-0142)
    function formatPhoneNumber(num) {
        if (!num) return ""
        var clean = ("" + num).replace(/[^0-9+]/g, "")
        if (clean.length === 12 && clean.indexOf("+1") === 0) {
            return "+1 (" + clean.substring(2, 5) + ") " + clean.substring(5, 8) + "-" + clean.substring(8, 12)
        }
        if (clean.length === 11 && clean.indexOf("1") === 0) {
            return "+1 (" + clean.substring(1, 4) + ") " + clean.substring(4, 7) + "-" + clean.substring(7, 11)
        }
        if (clean.length === 10) {
            return "(" + clean.substring(0, 3) + ") " + clean.substring(3, 6) + "-" + clean.substring(6, 10)
        }
        return num
    }

    property var newChatSuggestions: []
    property int newChatSelectedIndex: 0
    property string newChatSelectedHandle: ""
    property var sidebarContactMatches: []

    function refreshNewChatSuggestions() {
        root.newChatSuggestions = backend.searchContacts(newChatRecipient.text, 8)
        if (root.newChatSelectedIndex >= root.newChatSuggestions.length) {
            root.newChatSelectedIndex = 0
        }
    }

    function refreshSidebarContacts() {
        var q = searchInput.text.trim()
        if (q.length === 0) {
            root.sidebarContactMatches = []
        } else {
            root.sidebarContactMatches = backend.searchContacts(q, 10)
        }
    }

    function submitNewChat(overrideAddress) {
        var targetAddr = overrideAddress || ""
        if (targetAddr.length === 0) {
            if (root.newChatSelectedHandle.length > 0) {
                targetAddr = root.newChatSelectedHandle
            } else if (root.newChatSuggestions.length > 0) {
                var sel = root.newChatSuggestions[root.newChatSelectedIndex] || root.newChatSuggestions[0]
                targetAddr = sel.address || sel.handle || newChatRecipient.text.trim()
            } else {
                targetAddr = newChatRecipient.text.trim()
            }
        }
        if (targetAddr.length === 0) return
        var initialMsg = newChatText.text.trim()
        backend.startNewChat(targetAddr, initialMsg)
        newChatRecipient.text = ""
        newChatText.text = ""
        root.newChatSelectedHandle = ""
        root.newChatSelectedIndex = 0
        root.showNewChatModal = false
        searchInput.text = ""
        messageListView.scrollToBottom()
        composerInput.forceActiveFocus()
    }

    onShowNewChatModalChanged: {
        if (root.showNewChatModal) {
            if (root.showThreadView) root.closeThread()
            root.newChatSelectedHandle = ""
            root.newChatSelectedIndex = 0
            root.refreshNewChatSuggestions()
            newChatRecipient.forceActiveFocus()
        }
    }

    function resolveSavedDraftForChat(chatGuid) {
        if (!chatGuid || chatGuid.length === 0) return ""
        var drafts = root.chatDrafts || ({})
        if (drafts[chatGuid] !== undefined && drafts[chatGuid].length > 0) {
            return drafts[chatGuid]
        }
        var keys = Object.keys(drafts)
        for (var i = 0; i < keys.length; ++i) {
            var k = keys[i]
            if (k !== "" && k !== chatGuid && drafts[k] !== undefined && drafts[k].length > 0 && backend.isSameConversation(k, chatGuid)) {
                var val = drafts[k]
                drafts[chatGuid] = val
                delete drafts[k]
                root.chatDrafts = drafts
                return val
            }
        }
        return (drafts[chatGuid] !== undefined) ? drafts[chatGuid] : ""
    }

    function resolveSavedAttachmentForChat(chatGuid) {
        if (!chatGuid || chatGuid.length === 0) return null
        var attDrafts = root.chatAttachmentDrafts || ({})
        if (attDrafts[chatGuid] !== undefined && attDrafts[chatGuid] && attDrafts[chatGuid].path) {
            return attDrafts[chatGuid]
        }
        var keys = Object.keys(attDrafts)
        for (var i = 0; i < keys.length; ++i) {
            var k = keys[i]
            var entry = attDrafts[k]
            if (k !== "" && k !== chatGuid && entry && entry.path && backend.isSameConversation(k, chatGuid)) {
                var migrated = {
                    path: entry.path,
                    name: entry.name || entry.path.split("/").pop(),
                    chatGuid: chatGuid
                }
                attDrafts[chatGuid] = migrated
                delete attDrafts[k]
                root.chatAttachmentDrafts = attDrafts
                return migrated
            }
        }
        return null
    }

    Component.onCompleted: {
        var initialGuid = backend.selectedChatGuid || ""
        if (initialGuid.length > 0) {
            root.lastDraftChatGuid = initialGuid
        }
        backend.registerWindow(root)
        root.reconcileAudioDeviceSelections()
        var currentGuid = backend.selectedChatGuid || ""
        if (currentGuid.length > 0) {
            root.lastDraftChatGuid = currentGuid
            var savedDraft = root.resolveSavedDraftForChat(currentGuid)
            if (savedDraft.length > 0 && composerInput.text !== savedDraft) {
                composerInput.text = savedDraft
            }
        }
    }
    onActiveChanged: backend.windowActive = root.active

    Connections {
        target: backend
        function onAudioDevicesChanged() {
            root.reconcileAudioDeviceSelections()
        }
        function onToastMessage(text) {
            root.activeToastText = text
            toastTimer.restart()
        }
        function onSelectedChatChanged() {
            var newGuid = backend.selectedChatGuid || ""
            var sameConv = (root.lastDraftChatGuid === newGuid) || (root.lastDraftChatGuid !== "" && newGuid !== "" && backend.isSameConversation(root.lastDraftChatGuid, newGuid))
            if (!sameConv) {
                root.clearActionMenu()
                // The info card follows the conversation
                if (root.showContactInfo) {
                    if (newGuid.length > 0) root.refreshContactInfo(); else root.closeContactInfo()
                }
                root.cancelEditing()
                root.replyTargetGuid = ""
                root.replyTargetSender = ""
                root.replyTargetText = ""
            }
            if (root.lastDraftChatGuid !== "" && root.lastDraftChatGuid !== newGuid) {
                var drafts = root.chatDrafts || ({})
                var canonPrev = backend.resolveCanonicalChatGuid(root.lastDraftChatGuid) || root.lastDraftChatGuid
                if (sameConv && composerInput.text !== "") {
                    drafts[newGuid] = composerInput.text
                    if (drafts[root.lastDraftChatGuid] !== undefined) delete drafts[root.lastDraftChatGuid]
                    if (canonPrev !== newGuid && drafts[canonPrev] !== undefined) delete drafts[canonPrev]
                } else {
                    drafts[canonPrev] = composerInput.text
                    if (canonPrev !== root.lastDraftChatGuid && drafts[root.lastDraftChatGuid] !== undefined) delete drafts[root.lastDraftChatGuid]
                }
                root.chatDrafts = drafts

                var attDrafts = root.chatAttachmentDrafts || ({})
                if (root.stagedAttachmentPath.length > 0 && root.isStagedAttachmentForChat(root.lastDraftChatGuid)) {
                    var targetKey = (sameConv && newGuid.length > 0) ? newGuid : canonPrev
                    attDrafts[targetKey] = {
                        path: root.stagedAttachmentPath,
                        name: root.stagedAttachmentName,
                        chatGuid: targetKey
                    }
                    if (targetKey !== root.lastDraftChatGuid && attDrafts[root.lastDraftChatGuid] !== undefined) delete attDrafts[root.lastDraftChatGuid]
                    if (targetKey !== canonPrev && attDrafts[canonPrev] !== undefined) delete attDrafts[canonPrev]
                    root.chatAttachmentDrafts = attDrafts
                }
            }
            if (root.lastDraftChatGuid !== newGuid) {
                root.lastDraftChatGuid = newGuid
                var savedDraft = root.resolveSavedDraftForChat(newGuid)
                if (composerInput.text !== savedDraft) {
                    composerInput.text = savedDraft
                }
                if (sameConv && newGuid.length > 0 && root.stagedAttachmentPath.length > 0 && root.isStagedAttachmentForChat(newGuid)) {
                    root.stagedAttachmentChatGuid = newGuid
                } else {
                    var savedAtt = root.resolveSavedAttachmentForChat(newGuid)
                    if (savedAtt && savedAtt.path && savedAtt.path.length > 0) {
                        root.stagedAttachmentPath = savedAtt.path
                        root.stagedAttachmentName = savedAtt.name || savedAtt.path.split("/").pop()
                        root.stagedAttachmentChatGuid = newGuid
                    } else {
                        root.stagedAttachmentPath = ""
                        root.stagedAttachmentName = ""
                        root.stagedAttachmentChatGuid = ""
                    }
                }
            }
        }
        function onContactsChanged() {
            if (root.showContactInfo) root.contactInfoDetails = backend.selectedChatIsGroup ? ({ found: false }) : backend.contactDetails(backend.selectedChatAddress)
            if (root.showNewChatModal) root.refreshNewChatSuggestions()
            if (searchInput.text.trim().length > 0) root.refreshSidebarContacts()
        }
        function onAttachmentReady(guid, localUrl) {
            if (!root.photoViewerItems || root.photoViewerItems.length === 0) return
            var changed = false
            var updated = []
            for (var i = 0; i < root.photoViewerItems.length; ++i) {
                var item = root.photoViewerItems[i]
                var ig = item.guid || ""
                if (ig === guid || (ig.length >= 16 && guid.length >= 16 && ig.substring(0, 16) === guid.substring(0, 16))) {
                    updated.push({
                        guid: item.guid,
                        name: item.name,
                        mimeType: item.mimeType || "",
                        localUrl: localUrl,
                        previewUrl: item.previewUrl || "",
                        sourceUrl: item.sourceUrl || "",
                        isCloudEvicted: false,
                        canUnpurge: item.canUnpurge !== false,
                        canRequestAttachment: true
                    })
                    changed = true
                } else {
                    updated.push(item)
                }
            }
            if (changed) {
                root.photoViewerItems = updated
            }
        }
        function onIcloudPhotosChanged() {
            if (!root.photoViewerItems || root.photoViewerItems.length === 0) return
            var photos = backend.icloudPhotos
            if (!photos || photos.length === 0) return
            var changed = false
            var updated = []
            var pendingOpenGuid = ""
            var pendingOpenName = ""
            for (var i = 0; i < root.photoViewerItems.length; ++i) {
                var item = root.photoViewerItems[i]
                var ig = item.guid || ""
                var matched = null
                for (var j = 0; j < photos.length; ++j) {
                    var p = photos[j]
                    if (p && p.guid === ig) {
                        matched = p
                        break
                    }
                }
                if (matched) {
                    var nextUrl = (matched.localUrl && matched.localUrl.length > 0) ? matched.localUrl : (item.localUrl || "")
                    var nextEvicted = !!matched.isCloudEvicted && nextUrl.length === 0
                    var nextCanUnpurge = matched.canUnpurge !== false
                    var nextCanReq = !nextEvicted || nextCanUnpurge
                    if (nextUrl !== (item.localUrl || "") || nextEvicted !== !!item.isCloudEvicted || nextCanReq !== !!item.canRequestAttachment) {
                        changed = true
                    }
                    updated.push({
                        guid: item.guid,
                        name: item.name,
                        mimeType: item.mimeType || "",
                        localUrl: nextUrl,
                        previewUrl: item.previewUrl || "",
                        sourceUrl: item.sourceUrl || "",
                        isCloudEvicted: nextEvicted,
                        canUnpurge: nextCanUnpurge,
                        canRequestAttachment: nextCanReq
                    })
                    if (i === root.photoViewerIndex && root.showPhotoViewer && nextUrl.length === 0 && !nextEvicted && ig.length > 0) {
                        pendingOpenGuid = ig
                        pendingOpenName = item.name || ""
                    }
                } else {
                    updated.push(item)
                }
            }
            if (changed) {
                root.photoViewerItems = updated
            }
            if (pendingOpenGuid.length > 0) {
                backend.openAttachment(pendingOpenGuid, pendingOpenName)
            }
        }
    }

    Timer {
        id: toastTimer
        interval: 2200
        onTriggered: root.activeToastText = ""
    }

    function showToast(text) {
        root.activeToastText = text
        toastTimer.restart()
    }

    function updateEmojiAutocomplete() {
        if (root.isEmojiCycling) return
        var pos = composerInput.cursorPosition
        var before = composerInput.text.slice(0, pos)

        // 1. Check for :shortcode
        var scMatch = before.match(/(?:^|\s):([a-zA-Z0-9_+\-]{2,})$/)
        if (scMatch && scMatch[1]) {
            root.emojiShortcodePrefix = scMatch[1]
            root.emojiShortcodeStart = pos - scMatch[1].length - 1
            root.emojiAutocompleteResults = backend.searchEmojis(scMatch[1], "", 8)
            root.selectedEmojiIndex = 0
            return
        }

        // 2. Check for ASCII emoticons
        var asciiMatch = before.match(/(?:^|\s)(:-?\)|:-?\(|:-?[dD]|=D|;-?\)|:-?[pP]|:P|:p|<3|<\/3|:-?[oO]|:O|:o|:-?\/|:'-?\(|[bB8]-?\)|[bB8]\)|:-?\*|:\*|[xX][dD]|-_-)$/)
        if (asciiMatch && asciiMatch[1]) {
            var token = asciiMatch[1]
            root.emojiShortcodePrefix = token
            root.emojiShortcodeStart = pos - token.length
            root.emojiAutocompleteResults = backend.searchEmojis(token, "", 8)
            root.selectedEmojiIndex = 0
            return
        }

        root.emojiShortcodePrefix = ""
        root.emojiShortcodeStart = -1
        root.emojiAutocompleteResults = []
        root.selectedEmojiIndex = 0
    }

    function applyEmojiAutocomplete(emojiChar, keepForCycling) {
        if (root.emojiShortcodeStart < 0) return
        var pos = composerInput.cursorPosition
        var full = composerInput.text
        var replaced = full.slice(0, root.emojiShortcodeStart) + emojiChar + " " + full.slice(pos)
        var newPos = root.emojiShortcodeStart + emojiChar.length + 1
        composerInput.text = replaced
        composerInput.cursorPosition = newPos

        if (keepForCycling) {
            root.isEmojiCycling = true
            root.emojiCycleStartPos = root.emojiShortcodeStart
            root.emojiCycleLength = emojiChar.length + 1
        } else {
            root.isEmojiCycling = false
            root.emojiShortcodePrefix = ""
            root.emojiShortcodeStart = -1
            root.emojiAutocompleteResults = []
            root.selectedEmojiIndex = 0
        }
        composerInput.forceActiveFocus()
    }

    function cycleEmoji(step) {
        if (!root.emojiAutocompleteResults || root.emojiAutocompleteResults.length === 0) return
        var count = root.emojiAutocompleteResults.length
        root.selectedEmojiIndex = (root.selectedEmojiIndex + step + count) % count
        var nextEmoji = root.emojiAutocompleteResults[root.selectedEmojiIndex].emoji

        if (root.isEmojiCycling) {
            var full = composerInput.text
            var before = full.slice(0, root.emojiCycleStartPos)
            var after = full.slice(root.emojiCycleStartPos + root.emojiCycleLength)
            var replacement = nextEmoji + " "
            composerInput.text = before + replacement + after
            root.emojiCycleLength = replacement.length
            composerInput.cursorPosition = root.emojiCycleStartPos + replacement.length
        }
    }

    function insertEmojiAtCursor(emojiChar) {
        var pos = composerInput.cursorPosition
        var full = composerInput.text
        composerInput.text = full.slice(0, pos) + emojiChar + full.slice(pos)
        composerInput.cursorPosition = pos + emojiChar.length
        composerInput.forceActiveFocus()
    }

    function escapeHtml(text) {
        if (!text) return ""
        return text.replace(/&/g, "&amp;")
                   .replace(/</g, "&lt;")
                   .replace(/>/g, "&gt;")
                   .replace(/"/g, "&quot;")
                   .replace(/'/g, "&#039;")
    }

    function linkifyText(text) {
        if (!text) return ""
        var urlRegex = /((?:https?:\/\/[^\s<>"']+)|(?:www\.(?:[a-zA-Z0-9-]+\.)+[a-zA-Z]{2,24}(?:\/[^\s<>"']*)?)|(?:(?:[a-zA-Z0-9-]+\.)+[a-zA-Z]{2,24}\/[^\s<>"']+))/gi
        var parts = text.split(urlRegex)
        var res = ""
        for (var i = 0; i < parts.length; ++i) {
            var part = parts[i]
            if (!part) continue
            if (i % 2 === 1) {
                var cleanUrl = part.replace(/[.,)\]?!;:"'>]+$/, "")
                var trailing = part.substring(cleanUrl.length)
                var href = /^https?:\/\//i.test(cleanUrl) ? cleanUrl : ("https://" + cleanUrl)
                res += "<a href=\"" + escapeHtml(href) + "\" style=\"color: #64B5F6; text-decoration: underline;\">" + escapeHtml(cleanUrl) + "</a>" + escapeHtml(trailing).replace(/\r?\n/g, "<br/>")
            } else {
                res += escapeHtml(part).replace(/\r?\n/g, "<br/>")
            }
        }
        return res
    }

    // Keyboard shortcuts
    Shortcut {
        sequences: ["Ctrl+K", "F1"]
        onActivated: {
            root.showShortcutsModal = !root.showShortcutsModal
            if (root.showShortcutsModal) {
                root.showNewChatModal = false
                root.showContactModal = false
                root.showEmojiPicker = false
            }
        }
    }
    Shortcut {
        sequences: ["Ctrl+F", "/"]
        onActivated: searchInput.forceActiveFocus()
    }
    Shortcut {
        sequences: ["Alt+Down", "Ctrl+J", "Ctrl+Tab"]
        onActivated: backend.selectNextChat()
    }
    Shortcut {
        sequences: ["Alt+Up", "Ctrl+Shift+Tab"]
        onActivated: backend.selectPrevChat()
    }
    Shortcut {
        sequence: "Ctrl+N"
        onActivated: {
            root.showNewChatModal = true
            newChatRecipient.forceActiveFocus()
        }
    }
    Shortcut {
        sequences: ["Ctrl+Shift+C", "Alt+C"]
        onActivated: {
            if (backend.selectedChatGuid.length > 0) root.openContactModal(backend.selectedChatAddress, "")
        }
    }
    Shortcut {
        sequence: "Ctrl+E"
        onActivated: root.showEmojiPicker = !root.showEmojiPicker
    }
    Shortcut {
        sequence: "Ctrl+U"
        onActivated: backend.setFilterMode(backend.filterMode === "unread" ? "all" : "unread")
    }
    Shortcut {
        sequences: ["Ctrl+Shift+U", "Alt+U"]
        onActivated: {
            if (backend.selectedChatGuid) {
                backend.toggleChatReadUnread(backend.selectedChatGuid)
                if (backend.selectedChatUnreadCount > 0) {
                    root.showToast("● " + i18n.tr("Marked chat as unread in iMessage"))
                } else {
                    root.showToast("✓ " + i18n.tr("Marked chat as read in iMessage"))
                }
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+Shift+A"
        onActivated: {
            backend.markAllChatsRead()
            root.showToast("✓ " + i18n.tr("Marked all chats as read in iMessage"))
        }
    }
    Shortcut {
        sequences: ["Ctrl+R", "F5"]
        onActivated: backend.refreshAll()
    }
    Shortcut {
        sequence: "Ctrl+Shift+R"
        onActivated: {
            backend.markChatRead(backend.selectedChatGuid)
            root.showToast("✓ " + i18n.tr("Marked chat as read in iMessage"))
        }
    }
    Shortcut {
        sequence: "Ctrl+M"
        onActivated: backend.toggleChatMute(backend.selectedChatGuid)
    }
    Shortcut {
        sequences: ["PageDown", "Ctrl+PageDown"]
        enabled: !root.showNewChatModal && !root.showContactModal && !root.showShortcutsModal && backend.selectedChatGuid.length > 0
        onActivated: root.scrollMessagesPageDown()
    }
    Shortcut {
        sequences: ["PageUp", "Ctrl+PageUp"]
        enabled: !root.showNewChatModal && !root.showContactModal && !root.showShortcutsModal && backend.selectedChatGuid.length > 0
        onActivated: root.scrollMessagesPageUp()
    }
    Shortcut {
        sequence: "Ctrl+Home"
        enabled: !root.showNewChatModal && !root.showContactModal && !root.showShortcutsModal && backend.selectedChatGuid.length > 0
        onActivated: root.scrollMessagesToTop()
    }
    Shortcut {
        sequence: "Ctrl+End"
        enabled: !root.showNewChatModal && !root.showContactModal && !root.showShortcutsModal && backend.selectedChatGuid.length > 0
        onActivated: root.scrollMessagesToBottom()
    }
    Shortcut {
        sequence: "Home"
        enabled: !root.showNewChatModal && !root.showContactModal && !root.showShortcutsModal && !searchInput.activeFocus && (!composerInput.activeFocus || composerInput.text.length === 0) && backend.selectedChatGuid.length > 0
        onActivated: root.scrollMessagesToTop()
    }
    Shortcut {
        sequence: "End"
        enabled: !root.showNewChatModal && !root.showContactModal && !root.showShortcutsModal && !searchInput.activeFocus && (!composerInput.activeFocus || composerInput.text.length === 0) && backend.selectedChatGuid.length > 0
        onActivated: root.scrollMessagesToBottom()
    }
    Repeater {
        model: [1, 2, 3, 4, 5, 6, 7, 8, 9]
        delegate: Item {
            required property int modelData
            Shortcut {
                sequences: ["Alt+" + modelData, "Ctrl+" + modelData]
                onActivated: {
                    var chs = backend.chats
                    if (modelData - 1 < chs.length) {
                        backend.selectChat(chs[modelData - 1].guid)
                    }
                }
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: root.openServerConfigModal()
    }
    Shortcut {
        sequence: "?"
        enabled: !root.showServerConfigModal && !root.showContactModal && !root.showNewChatModal
        onActivated: root.showShortcutsModal = !root.showShortcutsModal
    }
    Shortcut {
        sequences: ["Ctrl+H", "Alt+Home"]
        onActivated: {
            root.clearActionMenu()
            root.showShortcutsModal = false
            root.showContactModal = false
            root.showNewChatModal = false
            root.showEmojiPicker = false
            backend.clearSelectedChat()
        }
    }
    function handleEscapePostVoiceFallback() {
        // Editing first: the staged attachment is hidden meanwhile and must survive
        if (root.editTargetGuid !== "") {
            root.cancelEditing()
        } else if (root.stagedAttachmentPath !== "" && root.isStagedAttachmentForChat(backend.selectedChatGuid)) {
            root.clearStagedAttachment()
            root.showToast("✕ " + i18n.tr("Attachment cancelled"))
        } else if (root.replyTargetGuid !== "") {
            root.replyTargetGuid = ""
            root.replyTargetSender = ""
            root.replyTargetText = ""
        } else if (searchInput.text !== "") {
            searchInput.text = ""
            backend.setSearchQuery("")
            composerInput.forceActiveFocus()
        } else if (backend.selectedChatGuid.length > 0) {
            backend.clearSelectedChat()
        }
    }
    Shortcut {
        sequence: "Escape"
        onActivated: {
            if (root.showPhotoViewer) {
                root.closePhotoViewer()
            } else if (chatContextMenuPopup.visible) {
                chatContextMenuPopup.visible = false
            } else if (effectMenuPopup.visible) {
                effectMenuPopup.visible = false
            } else if (scheduleMenuPopup.visible) {
                scheduleMenuPopup.visible = false
            } else if (renameChatPopup.visible) {
                renameChatPopup.visible = false
            } else if (icloudNotesPopup.visible) {
                icloudNotesPopup.close()
            } else if (icloudCalendarPopup.visible) {
                icloudCalendarPopup.close()
            } else if (icloudPhotosPopup.visible) {
                icloudPhotosPopup.close()
            } else if (audioInputDeviceCombo.popup.visible) {
                audioInputDeviceCombo.popup.close()
            } else if (audioOutputDeviceCombo.popup.visible) {
                audioOutputDeviceCombo.popup.close()
            } else if (root.showServerConfigModal) {
                root.showServerConfigModal = false
            } else if (root.showDeleteChatModal) {
                root.showDeleteChatModal = false
            } else if (root.showShortcutsModal) {
                root.showShortcutsModal = false
            } else if (root.showContactModal) {
                root.showContactModal = false
            } else if (root.showNewChatModal) {
                root.showNewChatModal = false
            } else if (root.showThreadView) {
                root.showThreadView = false
            } else if (root.showContactInfo) {
                root.closeContactInfo()
            } else if (root.showTapbackEmojiPicker) {
                root.closeTapbackEmojiPicker()
            } else if (root.showEmojiPicker) {
                root.showEmojiPicker = false
            } else if (root.isEmojiCycling || root.emojiAutocompleteResults.length > 0) {
                root.isEmojiCycling = false
                root.emojiAutocompleteResults = []
                root.emojiShortcodePrefix = ""
                root.emojiShortcodeStart = -1
            } else if (root.activeActionMessageGuid !== "") {
                root.clearActionMenu()
            } else if (root.editTargetGuid !== "") {
                root.cancelEditing()
            } else if (backend.recordingVoice || backend.hasRecordedVoiceMemo) {
                if (root.isVoiceMemoForSelectedChat()) {
                    backend.cancelVoiceRecording()
                    root.showToast("✕ " + i18n.tr("Voice memo cancelled"))
                } else {
                    root.handleEscapePostVoiceFallback()
                }
            } else {
                root.handleEscapePostVoiceFallback()
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+Q"
        onActivated: Qt.quit()
    }
    Shortcut {
        sequence: "Ctrl+Shift+Delete"
        enabled: backend.selectedChatGuid.length > 0
        onActivated: backend.deleteChat(backend.selectedChatGuid)
    }
    Shortcut {
        sequence: "Delete"
        enabled: !composerInput.activeFocus && !searchInput.activeFocus && !root.showNewChatModal && !root.showContactModal && backend.selectedChatGuid.length > 0
        onActivated: backend.deleteChat(backend.selectedChatGuid)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Top Omarchy Status & Command Bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: theme.darkBackground

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: root.isNarrowWindow ? 8 : 12

                // App badge + connection dot. The relay status ("Connected v1.2.1 ·
                // macOS …") shows only while hovering the dot.
                RowLayout {
                    spacing: 8
                    Rectangle {
                        id: homeBrandBtn
                        objectName: "homeBrandBtn"
                        height: 30
                        radius: 7
                        implicitWidth: homeBrandRow.implicitWidth + 16
                        color: !backend.connected ? Qt.rgba(theme.red.r, theme.red.g, theme.red.b, 0.14) : "transparent"
                        border.width: !backend.connected ? 1 : 0
                        border.color: theme.red

                        RowLayout {
                            id: homeBrandRow
                            anchors.centerIn: parent
                            spacing: 7
                            Rectangle {
                                id: topConnectionStatusDot
                                objectName: "topConnectionStatusDot"
                                width: 10
                                height: 10
                                radius: 5
                                color: backend.connected ? theme.green : theme.red
                                border.width: 1
                                border.color: backend.connected ? Qt.lighter(theme.green, 1.3) : Qt.lighter(theme.red, 1.3)
                                readonly property string statusText: backend.serverStatus
                                                                     + (backend.syncingHistory ? " · " + i18n.tr("Syncing history…") : "")
                                ToolTip.visible: statusDotMouse.containsMouse
                                ToolTip.text: statusText
                                ToolTip.delay: 150
                                MouseArea {
                                    id: statusDotMouse
                                    objectName: "statusDotMouse"
                                    anchors.centerIn: parent
                                    width: 24
                                    height: 24
                                    hoverEnabled: true
                                }
                            }
                            Image {
                                width: 20
                                height: 20
                                source: "qrc:/bluepill.svg"
                                sourceSize.width: 40
                                sourceSize.height: 40
                                fillMode: Image.PreserveAspectFit
                            }
                            Text {
                                text: "BluePill"
                                font.pixelSize: theme.fontSize + 2
                                font.bold: true
                                color: !backend.connected ? theme.red : theme.brightForeground
                            }
                        }
                    }

                    Rectangle {
                        id: topBarStatusPill
                        objectName: "topBarStatusPill"
                        visible: backend.showTopBarStatusPill
                            height: 26
                            radius: 13
                            implicitWidth: topStatusPillRow.implicitWidth + 18
                            color: topStatusPillMouse.containsMouse
                                   ? theme.selection
                                   : (backend.connected ? Qt.rgba(48/255, 209/255, 88/255, 0.14) : Qt.rgba(theme.red.r, theme.red.g, theme.red.b, 0.14))
                            border.width: 1
                            border.color: backend.connected ? theme.green : theme.red
                            ToolTip.visible: topStatusPillMouse.containsMouse
                            ToolTip.text: backend.serverStatus + (backend.serverUrl.length > 0 ? " · " + backend.serverUrl : "")
                            ToolTip.delay: 250

                            RowLayout {
                                id: topStatusPillRow
                                anchors.centerIn: parent
                                spacing: 6
                                Rectangle {
                                    width: 7
                                    height: 7
                                    radius: 3.5
                                    color: backend.connected ? theme.green : theme.red
                                }
                                Text {
                                    id: topStatusPillText
                                    objectName: "topStatusPillText"
                                    text: {
                                        var statusMatch = (backend.serverStatus || "").match(/v\d+\.\d+(?:\.\d+)?/i)
                                        var ver = statusMatch ? statusMatch[0] : (Qt.application.version ? ("v" + Qt.application.version) : "v1.2.3")
                                        var st = backend.connected ? (i18n.tr("Connected") + " " + ver) : i18n.tr("Disconnected")
                                        var host = (backend.serverUrl || "").replace(/^https?:\/\//i, "").replace(/^ssh:\/\//i, "").replace(/\/.*$/, "")
                                        return host.length > 0 ? (st + " · " + host) : st
                                    }
                                    font.pixelSize: theme.fontSize - 3
                                    font.bold: true
                                    color: backend.connected ? theme.green : theme.red
                                }
                            }
                        MouseArea {
                            id: topStatusPillMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.openServerConfigModal()
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // Filter pills
                RowLayout {
                    spacing: root.isNarrowWindow ? 4 : 6
                    Repeater {
                        model: [
                            { key: "all", label: i18n.tr("All") + (backend.totalChatCount > 0 ? " (" + backend.totalChatCount + ")" : "") },
                            { key: "unread", label: i18n.tr("Unread") + (backend.totalUnread > 0 ? " (" + backend.totalUnread + ")" : "") },
                            { key: "direct", label: i18n.tr("Direct") },
                            { key: "groups", label: i18n.tr("Groups") },
                            { key: "muted", label: i18n.tr("Muted") }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            height: 28
                            radius: 7
                            implicitWidth: pillText.implicitWidth + (root.isNarrowWindow ? 14 : 20)
                            color: backend.filterMode === modelData.key ? theme.accent : theme.lighterBackground
                            border.width: 1
                            border.color: backend.filterMode === modelData.key ? theme.accent : theme.muted

                            Text {
                                id: pillText
                                anchors.centerIn: parent
                                text: modelData.label
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.filterMode === modelData.key
                                color: backend.filterMode === modelData.key ? theme.darkerBackground : theme.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.setFilterMode(modelData.key)
                            }
                        }
                    }
                }

                // Sync (was in the conversation header): conversations and contacts
                Rectangle {
                    id: topSyncBtn
                    objectName: "topSyncBtn"
                    width: 34
                    height: 30
                    radius: 7
                    color: topSyncMouse.containsMouse ? theme.selection : theme.lighterBackground
                    border.width: 1
                    border.color: (topSyncMouse.containsMouse || backend.syncingHistory) ? theme.accent : theme.muted
                    ToolTip.visible: topSyncMouse.containsMouse
                    ToolTip.text: backend.syncingHistory ? i18n.tr("Syncing…") : i18n.tr("Sync conversations and contacts (Ctrl+R)")
                    ToolTip.delay: 400
                    Text {
                        id: topSyncLbl
                        anchors.centerIn: parent
                        text: "↻"
                        font.pixelSize: theme.fontSize + 2
                        // Spins while history syncs
                        RotationAnimation on rotation {
                            running: backend.syncingHistory
                            from: 0; to: 360; duration: 1000; loops: Animation.Infinite
                            onRunningChanged: if (!running) topSyncLbl.rotation = 0
                        }
                        color: backend.syncingHistory ? theme.accent : theme.foreground
                    }
                    MouseArea {
                        id: topSyncMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            backend.refreshAll()
                            backend.syncContactsNow()
                        }
                    }
                }

                // Global Notifications Toggle Button
                Rectangle {
                    width: 32
                    height: 30
                    radius: 7
                    color: (backend.notificationsEnabled && !backend.omarchyDnd) ? theme.lighterBackground : theme.selection
                    border.width: 1
                    border.color: (backend.notificationsEnabled && !backend.omarchyDnd) ? theme.muted : theme.accent

                    Text {
                        anchors.centerIn: parent
                        text: (backend.notificationsEnabled && !backend.omarchyDnd) ? "🔔" : "🔕"
                        font.pixelSize: theme.fontSize
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: backend.toggleNotifications()
                    }
                }

                // New Message: a compose icon (pencil over paper), no label
                Rectangle {
                    id: topComposeBtn
                    objectName: "topComposeBtn"
                    height: 30
                    width: 34
                    radius: 7
                    color: composeMouse.containsMouse ? theme.accent : theme.selection
                    border.width: 1
                    border.color: theme.accent
                    ToolTip.visible: composeMouse.containsMouse
                    ToolTip.text: i18n.tr("New Message (Ctrl+N)")
                    ToolTip.delay: 400

                    // Material Design Icons "square-edit-outline" (Apache-2.0), drawn in the theme color
                    Shape {
                        anchors.centerIn: parent
                        width: 24
                        height: 24
                        scale: 0.8
                        antialiasing: true
                        ShapePath {
                            strokeWidth: -1
                            fillColor: composeMouse.containsMouse ? theme.darkerBackground : theme.accent
                            PathSvg { path: "M5 3c-1.11 0-2 .89-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7h-2v7H5V5h7V3H5m12.78 1a.69.69 0 0 0-.48.2l-1.22 1.21 2.5 2.5L19.8 6.7c.26-.26.26-.7 0-.95L18.25 4.2c-.13-.13-.3-.2-.47-.2m-2.41 2.12L8 13.5V16h2.5l7.37-7.38-2.5-2.5z" }
                        }
                    }
                    MouseArea {
                        id: composeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.showNewChatModal = true
                            newChatRecipient.forceActiveFocus()
                        }
                    }
                }

                // Settings Button (Ctrl+,)
                Rectangle {
                    id: topServerConfigBtn
                    objectName: "topServerConfigBtn"
                    height: 30
                    radius: 7
                    implicitWidth: 34
                    color: root.showServerConfigModal ? theme.accent : theme.lighterBackground
                    border.width: 1
                    border.color: (root.showServerConfigModal || backend.needsSetup) ? theme.accent : theme.muted

                    ToolTip.visible: serverTopMouse.containsMouse
                    ToolTip.text: i18n.tr("Settings (Ctrl+,)")
                    ToolTip.delay: 400

                    RowLayout {
                        id: serverTopLbl
                        anchors.centerIn: parent
                        spacing: 5
                        Text {
                            text: "⚙"
                            font.pixelSize: theme.fontSize + 2
                            color: root.showServerConfigModal ? theme.darkerBackground : theme.foreground
                        }
                        Text {
                            // Icon only (the tooltip says "Settings"); kept for tests/accessibility
                            id: serverTopBtnText
                            objectName: "serverTopBtnText"
                            visible: false
                            text: i18n.tr("Settings")
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: root.showServerConfigModal ? theme.darkerBackground : theme.foreground
                        }
                    }
                    MouseArea {
                        id: serverTopMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.openServerConfigModal()
                    }
                }

                // Keyboard Shortcuts Button (Ctrl+K)
                Rectangle {
                    height: 30
                    radius: 7
                    implicitWidth: shortcutsTopLbl.implicitWidth + 16
                    color: root.showShortcutsModal ? theme.accent : theme.lighterBackground
                    border.width: 1
                    border.color: root.showShortcutsModal ? theme.accent : theme.muted

                    ToolTip.visible: shortcutsTopMouse.containsMouse
                    ToolTip.text: "Keyboard Shortcuts (Ctrl+K)"
                    ToolTip.delay: 400

                    RowLayout {
                        id: shortcutsTopLbl
                        anchors.centerIn: parent
                        spacing: 5
                        Text {
                            text: "⌨"
                            font.pixelSize: theme.fontSize - 1
                            color: root.showShortcutsModal ? theme.darkerBackground : theme.foreground
                        }
                        Text {
                            visible: root.width >= 820
                            text: "Ctrl+K"
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: root.showShortcutsModal ? theme.darkerBackground : theme.foreground
                        }
                    }
                    MouseArea {
                        id: shortcutsTopMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.showShortcutsModal = !root.showShortcutsModal
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: theme.lighterBackground
            }
        }

        // Main Split Body
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Left Pane: Search + Conversation List (responsive 250-330px)
            Rectangle {
                id: leftSidebarPane
                objectName: "leftSidebarPane"
                Layout.preferredWidth: root.width < 920 ? 250 : (root.width < 1120 ? 285 : 330)
                Layout.minimumWidth: 230
                Layout.maximumWidth: 330
                Layout.fillHeight: true
                color: theme.darkBackground

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // Search Bar
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(60, theme.fontSize + 40)
                        color: "transparent"

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 10
                            radius: 8
                            color: theme.background
                            border.width: 1
                            border.color: searchInput.activeFocus ? theme.accent : theme.muted

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8

                                Text {
                                    text: "⌕"
                                    font.pixelSize: theme.fontSize + 1
                                    color: theme.darkForeground
                                }

                                TextField {
                                    id: searchInput
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    topPadding: 6
                                    bottomPadding: 10
                                    verticalAlignment: TextInput.AlignVCenter
                                    placeholderText: i18n.tr("Search conversations...") + " (Ctrl+F)"
                                    placeholderTextColor: theme.darkForeground
                                    color: theme.brightForeground
                                    font.pixelSize: theme.fontSize - 1
                                    background: Item {}
                                    onTextChanged: {
                                        backend.setSearchQuery(text)
                                        root.refreshSidebarContacts()
                                    }
                                    onAccepted: {
                                        var win = root
                                        if (backend.chats.length > 0) {
                                            var topGuid = backend.chats[0].guid
                                            searchInput.text = ""
                                            backend.setSearchQuery("")
                                            backend.selectChat(topGuid)
                                            if (win) win.focusAndScrollChat()
                                        } else if (win && win.sidebarContactMatches.length > 0) {
                                            var c = win.sidebarContactMatches[0]
                                            searchInput.text = ""
                                            backend.setSearchQuery("")
                                            backend.startNewChat(c.address, "")
                                            if (win) win.focusAndScrollChat()
                                        }
                                    }
                                }

                                Text {
                                    visible: searchInput.text.length > 0
                                    text: "✕"
                                    font.pixelSize: theme.fontSize - 2
                                    color: theme.darkForeground
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            searchInput.text = ""
                                            backend.setSearchQuery("")
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // iCloud Hub Quick Navigation Bar (Notes, Calendar, Photos)
                    Rectangle {
                        id: icloudHubBar
                        objectName: "icloudHubBar"
                        visible: backend.icloudNotesEnabled || backend.icloudCalendarEnabled || backend.icloudPhotosEnabled
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        Layout.leftMargin: 12
                        Layout.rightMargin: 12
                        Layout.bottomMargin: 4
                        color: "transparent"

                        RowLayout {
                            anchors.fill: parent
                            spacing: 6

                            Shortcut {
                                sequence: "Ctrl+Shift+N"
                                onActivated: {
                                    if (icloudHubBar.visible && backend.icloudNotesEnabled) {
                                        backend.fetchIcloudNotes()
                                        icloudNotesPopup.open()
                                    }
                                }
                            }
                            Shortcut {
                                sequence: "Ctrl+Shift+E"
                                onActivated: {
                                    if (icloudHubBar.visible && backend.icloudCalendarEnabled) {
                                        backend.fetchIcloudEvents()
                                        icloudCalendarPopup.open()
                                    }
                                }
                            }
                            Shortcut {
                                sequence: "Ctrl+Shift+P"
                                onActivated: {
                                    if (icloudHubBar.visible && backend.icloudPhotosEnabled) {
                                        backend.fetchIcloudPhotos()
                                        icloudPhotosPopup.open()
                                    }
                                }
                            }

                            Rectangle {
                                id: notesHubTile
                                objectName: "notesHubTile"
                                visible: backend.icloudNotesEnabled
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 6
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: "Open iCloud Notes"
                                color: (notesHover.containsMouse || activeFocus) ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18) : Qt.rgba(1, 1, 1, 0.05)
                                border.color: (notesHover.containsMouse || activeFocus) ? theme.accent : Qt.rgba(1, 1, 1, 0.1)
                                border.width: activeFocus ? 2 : 1
                                Keys.onReturnPressed: {
                                    backend.fetchIcloudNotes()
                                    icloudNotesPopup.open()
                                }
                                Keys.onEnterPressed: {
                                    backend.fetchIcloudNotes()
                                    icloudNotesPopup.open()
                                }
                                Keys.onSpacePressed: {
                                    backend.fetchIcloudNotes()
                                    icloudNotesPopup.open()
                                }

                                RowLayout {
                                    anchors.centerIn: parent
                                    spacing: 4
                                    Text {
                                        text: "📝"
                                        font.pixelSize: theme.fontSize - 1
                                    }
                                    Text {
                                        text: "Notes"
                                        font.pixelSize: theme.fontSize - 2
                                        font.bold: true
                                        color: theme.foreground
                                    }
                                }

                                MouseArea {
                                    id: notesHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        notesHubTile.forceActiveFocus()
                                        backend.fetchIcloudNotes()
                                        icloudNotesPopup.open()
                                    }
                                }
                            }

                            Rectangle {
                                id: calHubTile
                                objectName: "calHubTile"
                                visible: backend.icloudCalendarEnabled
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 6
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: "Open iCloud Calendar"
                                color: (calHover.containsMouse || activeFocus) ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18) : Qt.rgba(1, 1, 1, 0.05)
                                border.color: (calHover.containsMouse || activeFocus) ? theme.accent : Qt.rgba(1, 1, 1, 0.1)
                                border.width: activeFocus ? 2 : 1
                                Keys.onReturnPressed: {
                                    backend.fetchIcloudEvents()
                                    icloudCalendarPopup.open()
                                }
                                Keys.onEnterPressed: {
                                    backend.fetchIcloudEvents()
                                    icloudCalendarPopup.open()
                                }
                                Keys.onSpacePressed: {
                                    backend.fetchIcloudEvents()
                                    icloudCalendarPopup.open()
                                }

                                RowLayout {
                                    anchors.centerIn: parent
                                    spacing: 4
                                    Text {
                                        text: "📅"
                                        font.pixelSize: theme.fontSize - 1
                                    }
                                    Text {
                                        text: "Cal"
                                        font.pixelSize: theme.fontSize - 2
                                        font.bold: true
                                        color: theme.foreground
                                    }
                                }

                                MouseArea {
                                    id: calHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        calHubTile.forceActiveFocus()
                                        backend.fetchIcloudEvents()
                                        icloudCalendarPopup.open()
                                    }
                                }
                            }

                            Rectangle {
                                id: photosHubTile
                                objectName: "photosHubTile"
                                visible: backend.icloudPhotosEnabled
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 6
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: "Open iCloud Photos"
                                color: (photosHover.containsMouse || activeFocus) ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18) : Qt.rgba(1, 1, 1, 0.05)
                                border.color: (photosHover.containsMouse || activeFocus) ? theme.accent : Qt.rgba(1, 1, 1, 0.1)
                                border.width: activeFocus ? 2 : 1
                                Keys.onReturnPressed: {
                                    backend.fetchIcloudPhotos()
                                    icloudPhotosPopup.open()
                                }
                                Keys.onEnterPressed: {
                                    backend.fetchIcloudPhotos()
                                    icloudPhotosPopup.open()
                                }
                                Keys.onSpacePressed: {
                                    backend.fetchIcloudPhotos()
                                    icloudPhotosPopup.open()
                                }

                                RowLayout {
                                    anchors.centerIn: parent
                                    spacing: 4
                                    Text {
                                        text: "☁️"
                                        font.pixelSize: theme.fontSize - 1
                                    }
                                    Text {
                                        text: "Photos"
                                        font.pixelSize: theme.fontSize - 2
                                        font.bold: true
                                        color: theme.foreground
                                    }
                                }

                                MouseArea {
                                    id: photosHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        photosHubTile.forceActiveFocus()
                                        backend.fetchIcloudPhotos()
                                        icloudPhotosPopup.open()
                                    }
                                }
                            }
                        }
                    }

                    // Pinned Conversations Bar (synced from macOS pinning.plist / user pins)
                    Rectangle {
                        id: pinnedChatsBar
                        objectName: "pinnedChatsBar"
                        visible: backend.showPinnedRow && backend.pinnedChats.length > 0 && backend.searchQuery.length === 0
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? 76 : 0
                        color: "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 6

                            Repeater {
                                model: backend.pinnedChats
                                delegate: Item {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 68

                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 4

                                        Rectangle {
                                            Layout.alignment: Qt.AlignHCenter
                                            width: 42
                                            height: 42
                                            radius: 21
                                            color: modelData.avatarColor || theme.accent
                                            border.width: modelData.guid === backend.selectedChatGuid ? 2 : (pinTileMouse.containsMouse ? 1 : 0)
                                            border.color: modelData.guid === backend.selectedChatGuid ? theme.brightForeground : theme.accent

                                            RoundAvatar {
                                                anchors.fill: parent
                                                color: "transparent"
                                                source: modelData.avatarUrl || ""
                                                initials: modelData.initials || "#"
                                                fontPixelSize: theme.fontSize - 1
                                            }

                                            // Unread badge dot on pinned avatar
                                            Rectangle {
                                                visible: (modelData.unreadCount || 0) > 0
                                                anchors.right: parent.right
                                                anchors.top: parent.top
                                                anchors.rightMargin: -2
                                                anchors.topMargin: -2
                                                width: 16
                                                height: 16
                                                radius: 8
                                                color: theme.accent
                                                border.width: 2
                                                border.color: theme.darkBackground
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: modelData.unreadCount > 9 ? "9+" : modelData.unreadCount
                                                    font.pixelSize: 9
                                                    font.bold: true
                                                    color: "#ffffff"
                                                }
                                            }
                               
                                            // Subtle pin badge when explicitly pinned (vs recent backfill)
                                            Rectangle {
                                                objectName: "pinnedTilePinBadge"
                                                visible: Boolean(modelData.isPinned)
                                                anchors.right: parent.right
                                                anchors.bottom: parent.bottom
                                                anchors.rightMargin: -2
                                                anchors.bottomMargin: -2
                                                width: 15
                                                height: 15
                                                radius: 7.5
                                                color: theme.darkBackground
                                                border.width: 1
                                                border.color: theme.accent
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: "📌"
                                                    font.pixelSize: 8
                                                }
                                            }
                                        }

                                        Text {
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.maximumWidth: 64
                                            elide: Text.ElideRight
                                            horizontalAlignment: Text.AlignHCenter
                                            text: modelData.shortTitle || modelData.title || ""
                                            font.pixelSize: theme.fontSize - 4
                                            font.bold: modelData.guid === backend.selectedChatGuid || (modelData.unreadCount || 0) > 0
                                            color: modelData.guid === backend.selectedChatGuid ? theme.accent : theme.foreground
                                        }
                                    }

                                    MouseArea {
                                        id: pinTileMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: function(mouse) {
                                            if (mouse.button === Qt.RightButton) {
                                                // Same menu as the chat list (with Unpin)
                                                var p = pinTileMouse.mapToItem(root.contentItem, mouse.x, mouse.y)
                                                chatContextMenuPopup.openForChat(modelData, p.x, p.y)
                                            } else {
                                                var win = root
                                                var pinGuid = modelData.guid
                                                backend.selectChat(pinGuid)
                                                if (win) Qt.callLater(function() { win.focusAndScrollChat() })
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Conversation ListView
                    ListView {
                        id: chatListView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: backend.chats
                        spacing: 2
                        cacheBuffer: 2000

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                        }

                        // With the Unread filter on, offer "Mark all read" (was a Home page button)
                        header: Item {
                            width: chatListView.width
                            height: markAllReadRow.visible ? 38 : 0
                            Rectangle {
                                id: markAllReadRow
                                objectName: "markAllReadRow"
                                visible: backend.filterMode === "unread" && backend.totalUnread > 0
                                anchors.fill: parent
                                anchors.margins: 4
                                radius: 6
                                color: markAllReadMouse.containsMouse ? theme.selection : theme.lighterBackground
                                border.width: 1
                                border.color: markAllReadMouse.containsMouse ? theme.accent : theme.muted
                                Text {
                                    anchors.centerIn: parent
                                    text: "✓ " + i18n.tr("Mark all read") + "  ·  Ctrl+Shift+A"
                                    font.pixelSize: theme.fontSize - 2
                                    font.bold: true
                                    color: theme.accent
                                }
                                MouseArea {
                                    id: markAllReadMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        backend.markAllChatsRead()
                                        root.showToast("✓ " + i18n.tr("Marked all chats as read in iMessage"))
                                    }
                                }
                            }
                        }

                        delegate: Rectangle {
                            required property var modelData
                            width: chatListView.width - 12
                            x: 6
                            height: 72
                            radius: 8
                            color: modelData.guid === backend.selectedChatGuid
                                   ? theme.selection
                                   : (chatHover.containsMouse ? theme.lighterBackground : "transparent")
                            border.width: modelData.guid === backend.selectedChatGuid ? 1 : 0
                            border.color: theme.accent

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 10

                                // Avatar Circle (contact photo, or initials)
                                RoundAvatar {
                                    Layout.preferredWidth: 44
                                    Layout.preferredHeight: 44
                                    color: modelData.avatarColor
                                    source: modelData.avatarUrl || ""
                                    initials: modelData.initials
                                    fontPixelSize: theme.fontSize
                                }

                                // Title, Service badge, Preview
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6

                                        Text {
                                            Layout.fillWidth: true
                                            text: (modelData.muted ? "🔕 " : "") + modelData.title
                                            font.pixelSize: theme.fontSize + 1
                                            font.weight: modelData.unreadCount > 0 ? Font.Bold : Font.DemiBold
                                            color: theme.brightForeground
                                            elide: Text.ElideRight
                                        }

                                        Rectangle {
                                            height: 16
                                            radius: 4
                                            implicitWidth: srvLabel.implicitWidth + 8
                                            visible: Boolean(modelData.service)
                                            color: (modelData.service === "SMS" || modelData.service === "RCS") ? "#1f4d36" : "#1e3a6e"
                                            Text {
                                                id: srvLabel
                                                anchors.centerIn: parent
                                                text: modelData.service
                                                font.pixelSize: theme.fontSize - 3
                                                font.bold: true
                                                color: (modelData.service === "SMS" || modelData.service === "RCS") ? theme.green : theme.blue
                                            }
                                        }

                                        Text {
                                            text: modelData.relativeTime
                                            font.pixelSize: theme.fontSize - 2
                                            color: modelData.unreadCount > 0 ? theme.accent : theme.foreground
                                            opacity: modelData.unreadCount > 0 ? 1.0 : 0.75
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6

                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.lastMessagePreview
                                            font.pixelSize: theme.fontSize - 1
                                            color: modelData.unreadCount > 0 ? theme.brightForeground : theme.foreground
                                            opacity: modelData.unreadCount > 0 ? 0.95 : 0.8
                                            elide: Text.ElideRight
                                            maximumLineCount: 1
                                        }

                                        // 1. Pin / Unpin conversation toggle
                                        Rectangle {
                                            id: rowPinChatBtn
                                            objectName: "rowPinChatBtn"
                                            readonly property bool chatIsPinned: (void(backend.pinnedChats), Boolean(modelData.pinned) || backend.isChatPinned(modelData.guid))
                                            visible: chatHover.containsMouse || rowPinMouse.containsMouse || rowReadMouse.containsMouse || rowMuteMouse.containsMouse || rowDeleteMouse.containsMouse
                                            width: 22
                                            height: 20
                                            radius: 5
                                            z: 3
                                            color: rowPinMouse.containsMouse
                                                   ? theme.accent
                                                   : (chatIsPinned ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.background)
                                            border.width: 1
                                            border.color: (rowPinMouse.containsMouse || chatIsPinned) ? theme.accent : theme.muted
                                            ToolTip.visible: rowPinMouse.containsMouse
                                            ToolTip.text: chatIsPinned ? i18n.tr("Unpin conversation") : i18n.tr("Pin conversation")
                                            ToolTip.delay: 400
                                            Text {
                                                anchors.centerIn: parent
                                                text: "📌"
                                                font.pixelSize: theme.fontSize - 3
                                            }
                                            MouseArea {
                                                id: rowPinMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: backend.togglePinChat(modelData.guid)
                                            }
                                        }

                                        // 2. Read/unread toggle (was the header's Read button)
                                        Rectangle {
                                            id: rowReadToggleBtn
                                            objectName: "rowReadToggleBtn"
                                            visible: chatHover.containsMouse || rowPinMouse.containsMouse || rowReadMouse.containsMouse || rowMuteMouse.containsMouse || rowDeleteMouse.containsMouse
                                            width: 22
                                            height: 20
                                            radius: 5
                                            z: 3
                                            color: rowReadMouse.containsMouse ? theme.accent : theme.background
                                            border.width: 1
                                            border.color: theme.muted
                                            ToolTip.visible: rowReadMouse.containsMouse
                                            ToolTip.text: modelData.unreadCount > 0 ? i18n.tr("Mark as read") : i18n.tr("Mark as unread")
                                            ToolTip.delay: 400
                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.unreadCount > 0 ? "✓" : "●"
                                                font.pixelSize: theme.fontSize - 3
                                                color: rowReadMouse.containsMouse ? theme.darkerBackground : theme.foreground
                                            }
                                            MouseArea {
                                                id: rowReadMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: backend.toggleChatReadUnread(modelData.guid)
                                            }
                                        }

                                        // 3. Silence / Unmute notifications toggle
                                        Rectangle {
                                            id: rowMuteChatBtn
                                            objectName: "rowMuteChatBtn"
                                            visible: chatHover.containsMouse || rowPinMouse.containsMouse || rowReadMouse.containsMouse || rowMuteMouse.containsMouse || rowDeleteMouse.containsMouse
                                            width: 22
                                            height: 20
                                            radius: 5
                                            z: 3
                                            color: rowMuteMouse.containsMouse ? theme.accent : theme.background
                                            border.width: 1
                                            border.color: theme.muted
                                            ToolTip.visible: rowMuteMouse.containsMouse
                                            ToolTip.text: (modelData.isMuted || modelData.muted) ? i18n.tr("Unmute conversation") : i18n.tr("Silence notifications")
                                            ToolTip.delay: 400
                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.muted ? "🔔" : "🔕"
                                                font.pixelSize: theme.fontSize - 3
                                            }
                                            MouseArea {
                                                id: rowMuteMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: backend.toggleChatMute(modelData.guid)
                                            }
                                        }

                                        // 4. Delete conversation button
                                        Rectangle {
                                            id: rowDeleteChatBtn
                                            objectName: "rowDeleteChatBtn"
                                            visible: chatHover.containsMouse || rowPinMouse.containsMouse || rowReadMouse.containsMouse || rowMuteMouse.containsMouse || rowDeleteMouse.containsMouse
                                            width: 22
                                            height: 20
                                            radius: 5
                                            z: 3
                                            color: rowDeleteMouse.containsMouse ? "#3b1d26" : theme.background
                                            border.width: 1
                                            border.color: rowDeleteMouse.containsMouse ? "#f7768e" : theme.muted
                                            ToolTip.visible: rowDeleteMouse.containsMouse
                                            ToolTip.text: i18n.tr("Delete conversation")
                                            ToolTip.delay: 400
                                            Text {
                                                anchors.centerIn: parent
                                                text: "󰆴"
                                                font.pixelSize: theme.fontSize - 3
                                                color: rowDeleteMouse.containsMouse ? "#f7768e" : theme.foreground
                                            }
                                            MouseArea {
                                                id: rowDeleteMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var targetGuid = modelData.guid || ""
                                                    if (targetGuid.length > 0) {
                                                        backend.deleteChat(targetGuid)
                                                    }
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: modelData.unreadCount > 0
                                            height: 18
                                            implicitWidth: Math.max(18, unreadTxt.implicitWidth + 8)
                                            radius: 9
                                            color: theme.accent
                                            Text {
                                                id: unreadTxt
                                                anchors.centerIn: parent
                                                text: modelData.unreadCount
                                                font.pixelSize: theme.fontSize - 3
                                                font.bold: true
                                                color: theme.darkerBackground
                                            }
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                id: chatHover
                                anchors.fill: parent
                                z: -1
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: function(mouse) {
                                    var chatGuid = modelData.guid || ""
                                    var chatData = {
                                        guid: chatGuid,
                                        title: modelData.title || "",
                                        address: modelData.address || modelData.subtitle || "",
                                        unreadCount: modelData.unreadCount || 0,
                                        isMuted: (modelData.isMuted !== undefined ? modelData.isMuted : (modelData.muted || false)),
                                        isGroup: modelData.isGroup || false
                                    }
                                    root.clearActionMenu()
                                    if (mouse.button === Qt.RightButton) {
                                        var globalPos = chatHover.mapToItem(root.contentItem, mouse.x, mouse.y)
                                        chatContextMenuPopup.openForChat(chatData, globalPos.x, globalPos.y)
                                        return
                                    }
                                    var win = root
                                    chatContextMenuPopup.visible = false
                                    backend.selectChat(chatGuid)
                                    if (win) Qt.callLater(function() { win.focusAndScrollChat() })
                                }
                            }
                        }

                        footer: Column {
                            width: chatListView.width
                            visible: searchInput.text.trim().length > 0 && root.sidebarContactMatches.length > 0
                            spacing: 4
                            topPadding: 8
                            bottomPadding: 10

                            Text {
                                x: 14
                                text: i18n.tr("CONTACTS — START CONVERSATION")
                                font.pixelSize: theme.fontSize - 4
                                font.bold: true
                                color: theme.accent
                                opacity: 0.9
                            }

                            Repeater {
                                model: (searchInput.text.trim().length > 0) ? root.sidebarContactMatches : []
                                delegate: Rectangle {
                                    required property var modelData
                                    width: chatListView.width - 12
                                    x: 6
                                    height: 58
                                    radius: 8
                                    color: sidebarContactHover.containsMouse ? theme.lighterBackground : "transparent"
                                    border.width: sidebarContactHover.containsMouse ? 1 : 0
                                    border.color: theme.accent

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        spacing: 10

                                        Rectangle {
                                            Layout.preferredWidth: 38
                                            Layout.preferredHeight: 38
                                            radius: 19
                                            color: modelData.avatarColor || theme.accent
                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.initials || "💬"
                                                font.pixelSize: theme.fontSize - 2
                                                font.bold: true
                                                color: "#ffffff"
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2

                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData.displayName || modelData.name || modelData.address
                                                font.pixelSize: theme.fontSize
                                                font.bold: true
                                                color: theme.brightForeground
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData.formattedAddress || modelData.displayHandle || modelData.address
                                                font.pixelSize: theme.fontSize - 3
                                                color: theme.darkForeground
                                                elide: Text.ElideRight
                                            }
                                        }

                                        Rectangle {
                                            height: 22
                                            implicitWidth: contactBadgeText.implicitWidth + 12
                                            radius: 11
                                            color: modelData.hasExistingChat ? theme.selection : Qt.rgba(10/255, 132/255, 255/255, 0.2)
                                            border.width: 1
                                            border.color: theme.accent

                                            Text {
                                                id: contactBadgeText
                                                anchors.centerIn: parent
                                                text: modelData.hasExistingChat ? i18n.tr("Open Chat") : i18n.tr("New Chat")
                                                font.pixelSize: theme.fontSize - 4
                                                font.bold: true
                                                color: theme.accent
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: sidebarContactHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            var win = root
                                            var addr = modelData.address || modelData.handle || ""
                                            searchInput.text = ""
                                            backend.setSearchQuery("")
                                            backend.startNewChat(addr, "")
                                            if (win) win.focusAndScrollChat()
                                        }
                                    }
                                }
                            }
                        }
                    }

                }

                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: theme.lighterBackground
                }
            }

            // Right Pane: Home Page Dashboard OR Active Conversation Header + Message Timeline + Composer
            ColumnLayout {
                id: conversationPane
                objectName: "conversationPane"
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // Shown when no conversation is open. Everything the old Home page
                // offered lives in the top bar, the chat list, or Settings.
                Rectangle {
                    id: homePagePane
                    objectName: "noConversationPane"
                    visible: backend.selectedChatGuid.length === 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: theme.background

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 10
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: i18n.tr("Select a conversation")
                            font.pixelSize: theme.fontSize + 2
                            font.bold: true
                            color: theme.foreground
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: i18n.tr("or press Ctrl+N to start a new message")
                            font.pixelSize: theme.fontSize - 1
                            color: theme.darkForeground
                        }
                    }
                }

                // Active Chat Header (dynamically adapts between 1-row wide layout and 2-row split-screen layout so contact name/metadata and action buttons never overlap or clip)
                Rectangle {
                    id: activeChatHeader
                    objectName: "activeChatHeader"
                    visible: backend.selectedChatGuid.length > 0
                    Layout.fillWidth: true
                    readonly property bool compactActionButtons: activeChatHeader.width > 0 && activeChatHeader.width < 560
                    readonly property bool ultraCompactActionButtons: activeChatHeader.width > 0 && activeChatHeader.width < 500
                    readonly property int actionBtnPad: compactActionButtons ? 14 : 20
                    readonly property real naturalIdentityWidth: 16 + 44 + 12 + Math.max(contactTitleText.implicitWidth + 8 + hdrSrvPill.implicitWidth, contactSubtitleText.visible ? contactSubtitleText.implicitWidth : 0) + 16
                    readonly property real naturalActionsWidth: headerActionsRow.implicitWidth
                    // One row: the actions moved to hover menus, the top bar and the
                    // timeline's floating arrows, so nothing needs a second row.
                    readonly property bool stackChatHeaderRows: false
                    implicitHeight: 60
                    Layout.preferredHeight: implicitHeight
                    // Transparent strip with the contact centered, like Messages
                    color: "transparent"

                    // Left / Top Side: Contact Identity Card
                    Rectangle {
                        id: contactIdentityCard
                        objectName: "contactIdentityCard"
                        readonly property real sideReserve: headerActionsRow.width > 0 ? headerActionsRow.width + 26 : 18
                        x: Math.max(18, Math.round((activeChatHeader.width - width) / 2))
                        y: Math.round((activeChatHeader.height - height) / 2)
                        height: 52
                        readonly property real maxAllowedWidth: Math.max(180, activeChatHeader.width - 2 * sideReserve)
                        width: Math.min(activeChatHeader.naturalIdentityWidth, maxAllowedWidth)
                        radius: 8
                        clip: true
                        color: contactHeaderHover.containsMouse ? theme.lighterBackground : "transparent"

                        RowLayout {
                            id: contactIdentityRow
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            spacing: 12

                            // Circular Avatar
                            Rectangle {
                                Layout.preferredWidth: 44
                                Layout.preferredHeight: 44
                                width: 44
                                height: 44
                                radius: 22
                                color: backend.selectedChatColor
                                border.width: 1.5
                                border.color: Qt.rgba(1, 1, 1, 0.22)

                                RoundAvatar {
                                    anchors.fill: parent
                                    color: "transparent"
                                    source: backend.selectedChatAvatar
                                    initials: backend.selectedChatInitials
                                    fontPixelSize: theme.fontSize + 1
                                }
                            }

                            // Name, Service Pill & Address/Subtitle
                            ColumnLayout {
                                spacing: 3

                                RowLayout {
                                    spacing: 8
                                    Text {
                                        id: contactTitleText
                                        objectName: "contactTitleText"
                                        text: backend.selectedChatTitle
                                        font.pixelSize: theme.fontSize + 2
                                        font.bold: true
                                        color: theme.brightForeground
                                        elide: Text.ElideRight
                                        Layout.maximumWidth: Math.max(80, contactIdentityCard.width - 44 - 12 - hdrSrvPill.implicitWidth - 32)
                                    }
                                    Rectangle {
                                        id: hdrSrvPill
                                        height: 18
                                        radius: 4
                                        implicitWidth: hdrSrv.implicitWidth + 10
                                        visible: backend.selectedChatService.length > 0
                                        color: root.selectedChatIsGreen ? "#1f4d36" : "#1e3a6e"
                                        Text {
                                            id: hdrSrv
                                            anchors.centerIn: parent
                                            text: backend.selectedChatService
                                            font.pixelSize: theme.fontSize - 2
                                            font.bold: true
                                            color: root.selectedChatIsGreen ? theme.green : theme.blue
                                        }
                                    }
                                }

                                Text {
                                    id: contactSubtitleText
                                    objectName: "contactSubtitleText"
                                    property string baseAddr: backend.selectedChatSubtitle.length > 0
                                              ? root.formatPhoneNumber(backend.selectedChatSubtitle)
                                              : (backend.selectedChatAddress.length > 0
                                                     ? (backend.selectedChatHasContact
                                                            ? root.formatPhoneNumber(backend.selectedChatAddress)
                                                            : (root.formatPhoneNumber(backend.selectedChatAddress) + " • Click to add contact"))
                                                     : "")
                                    property string msgCountStr: backend.loadedMessageCount > 0 ? (" · " + backend.loadedMessageCount + " messages") : ""
                                    text: baseAddr.length > 0 ? (baseAddr + msgCountStr) : (backend.loadedMessageCount > 0 ? (backend.loadedMessageCount + " messages") : "")
                                    font.pixelSize: theme.fontSize - 2
                                    color: backend.selectedChatHasContact ? theme.darkForeground : theme.accent
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: Math.max(100, contactIdentityCard.width - 44 - 12 - 24)
                                    visible: text.length > 0
                                }
                            }
                        }

                        MouseArea {
                            id: contactHeaderHover
                            objectName: "contactHeaderNameArea"
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            // The name opens the contact / conversation info (Details, Photos, Links, Files)
                            ToolTip.visible: containsMouse
                            ToolTip.text: i18n.tr("Info")
                            ToolTip.delay: 400
                            onClicked: root.openContactInfo(0)
                        }
                    }

                    // Action Buttons Row: flush right on wide windows, second row on 50% split / narrow windows
                    Row {
                        id: headerActionsRow
                        objectName: "headerActionsRow"
                        x: Math.max(18, activeChatHeader.width - width - 18)
                        y: Math.round((activeChatHeader.height - height) / 2)
                        spacing: activeChatHeader.compactActionButtons ? 5 : 8

                    }
                }

                // Message Timeline ListView
                ListView {
                    id: messageListView
                    objectName: "messageTimelineListView"
                    visible: backend.selectedChatGuid.length > 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: backend.messages
                    spacing: 8
                    topMargin: 42
                    bottomMargin: 46
                    cacheBuffer: 2000

                    property bool stickToBottom: true
                    // Mouse movement / scrolling over the conversation (shows the scroll arrows)
                    property bool pointerActive: false
                    function notePointerActivity() {
                        pointerActive = true
                        pointerIdleTimer.restart()
                    }
                    property point lastPointerScenePos: Qt.point(-1, -1)
                    Timer {
                        id: pointerIdleTimer
                        interval: 1500
                        onTriggered: messageListView.pointerActive = false
                    }
                    HoverHandler {
                        id: timelineHover
                        // Content moving under a still cursor also changes point; only
                        // count real pointer movement (scene position changed).
                        onPointChanged: {
                            const p = point.scenePosition
                            if (p.x !== messageListView.lastPointerScenePos.x || p.y !== messageListView.lastPointerScenePos.y) {
                                messageListView.lastPointerScenePos = p
                                messageListView.notePointerActivity()
                            }
                        }
                    }
                    // The default Flickable wheel step is tiny (3 notches ≈ 18px); scroll a
                    // fixed ~110px per notch instead, speeding up while the wheel spins fast.
                    WheelHandler {
                        id: timelineWheel
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        target: null
                        property double lastWheelMs: 0
                        property real boost: 1
                        onWheel: function(event) {
                            const now = Date.now()
                            boost = (now - lastWheelMs < 120) ? Math.min(2.5, boost + 0.25) : 1
                            lastWheelMs = now
                            const dy = event.pixelDelta.y !== 0 ? event.pixelDelta.y * 1.5
                                                                : (event.angleDelta.y / 120) * 110 * boost
                            messageListView.scrollByWheel(dy)
                        }
                    }
                    function scrollBounds() {
                        const minY = originY - topMargin
                        return { min: minY, max: Math.max(minY, originY + contentHeight - height + bottomMargin) }
                    }
                    function scrollByWheel(dy) {
                        if (dy === 0) return
                        notePointerActivity()
                        smoothScrollAnim.stop()
                        restoringScrollPosition = false
                        scrollRestoreTimer.stop()
                        settlingToBottom = false
                        bottomSettleTimer.stop()
                        const b = scrollBounds()
                        contentY = Math.max(b.min, Math.min(b.max, contentY - dy))
                        stickToBottom = isNearBottom()
                        savedStickToBottom = stickToBottom
                        if (dy > 0 && contentY <= b.min + 1 && backend.hasOlderMessages && !backend.loadingOlderMessages) {
                            prevContentHeight = contentHeight
                            prevContentY = contentY
                            backend.loadOlderMessages()
                        }
                    }
                    // ↑ / ↓ arrows: animate there, then settle exactly (heights are estimates
                    // until delegates exist)
                    NumberAnimation {
                        id: smoothScrollAnim
                        target: messageListView
                        property: "contentY"
                        easing.type: Easing.OutCubic
                        property bool toEnd: false
                        property string chatGuid: ""
                        onFinished: {
                            if (chatGuid !== backend.selectedChatGuid) return
                            toEnd ? messageListView.scrollToBottom() : messageListView.scrollToTop()
                        }
                    }
                    readonly property alias smoothScrolling: smoothScrollAnim.running
                    function smoothScrollTo(toEnd) {
                        smoothScrollAnim.stop()
                        restoringScrollPosition = false
                        scrollRestoreTimer.stop()
                        settlingToBottom = false
                        bottomSettleTimer.stop()
                        stickToBottom = false
                        const b = scrollBounds()
                        const goal = toEnd ? b.max : b.min
                        const dist = Math.abs(goal - contentY)
                        if (dist < 2) {
                            if (toEnd) scrollToBottom(); else scrollToTop()
                            return
                        }
                        smoothScrollAnim.toEnd = toEnd
                        smoothScrollAnim.chatGuid = backend.selectedChatGuid
                        smoothScrollAnim.from = contentY
                        smoothScrollAnim.to = goal
                        smoothScrollAnim.duration = Math.min(700, 220 + dist / 12)
                        smoothScrollAnim.start()
                    }
                    property bool settlingToBottom: false
                    property int settlePassesRemaining: 0
                    property string lastSelectedChatGuid: ""
                    property real prevContentHeight: 0
                    property real prevContentY: 0
                    property bool savedStickToBottom: true
                    property real savedContentY: 0
                    property real savedContentHeight: 0
                    property int savedCount: 0
                    property int savedAnchorIndex: -1
                    property string savedAnchorGuid: ""
                    property real savedAnchorOffset: 0
                    property bool restoringScrollPosition: false
                    property int restorePassesRemaining: 0

                    function isNearBottom() {
                        if (atYEnd || count <= 0) return true
                        var minY = originY - topMargin
                        var maxEnd = Math.max(minY, originY + contentHeight + bottomMargin - height)
                        return contentY >= maxEnd - 24
                    }

                    function snapshotScrollState() {
                        if (backend.selectedChatGuid !== lastSelectedChatGuid || count <= 0) {
                            savedStickToBottom = true
                            savedAnchorIndex = -1
                            savedAnchorGuid = ""
                            savedAnchorOffset = 0
                            return
                        }
                        if (height <= 0 || width <= 0) {
                            return
                        }
                        if (restoringScrollPosition && !stickToBottom && (savedAnchorGuid.length > 0 || savedAnchorIndex >= 0)) {
                            return
                        }
                        savedStickToBottom = stickToBottom
                        savedContentY = contentY - originY
                        savedContentHeight = contentHeight
                        savedCount = count
                        var xFracs = [0.5, 0.25, 0.75, 0.15, 0.85]
                        var yOffsets = [
                            Math.min(height - 8, Math.max(8, topMargin + 8)),
                            Math.min(height - 8, Math.max(8, topMargin + 28)),
                            Math.min(height - 8, Math.max(8, Math.min(80, Math.max(46, height * 0.22)))),
                            Math.min(height - 8, Math.max(8, height * 0.5)),
                            Math.min(height - 8, Math.max(8, height * 0.75))
                        ]
                        var idx = -1
                        for (var yi = 0; yi < yOffsets.length && idx < 0; ++yi) {
                            var py = contentY + yOffsets[yi]
                            for (var xi = 0; xi < xFracs.length; ++xi) {
                                var cand = indexAt(width * xFracs[xi], py)
                                if (cand >= 0) {
                                    idx = cand
                                    break
                                }
                            }
                        }
                        if (idx < 0 && count > 0) {
                            idx = Math.min(count - 1, Math.max(0, Math.floor((contentY - originY) / Math.max(40, contentHeight / count))))
                        }
                        savedAnchorIndex = idx
                        var anchorItem = idx >= 0 ? itemAtIndex(idx) : null
                        savedAnchorOffset = anchorItem ? (anchorItem.y - contentY) : topMargin
                        if (anchorItem && anchorItem.modelData && anchorItem.modelData.guid) {
                            savedAnchorGuid = anchorItem.modelData.guid
                        } else if (idx >= 0 && idx < backend.messages.length && backend.messages[idx] && backend.messages[idx].guid) {
                            savedAnchorGuid = backend.messages[idx].guid
                        } else {
                            savedAnchorGuid = ""
                        }
                    }

                    function restoreSavedScrollPosition() {
                        if (count <= 0) return
                        if (savedStickToBottom || stickToBottom) {
                            scrollToBottom()
                            return
                        }
                        var targetIdx = -1
                        if (savedAnchorGuid.length > 0) {
                            var msgs = backend.messages
                            for (var i = 0; i < msgs.length; ++i) {
                                if (msgs[i] && msgs[i].guid === savedAnchorGuid) {
                                    targetIdx = i
                                    break
                                }
                            }
                        }
                        if (targetIdx < 0 && savedAnchorIndex >= 0) {
                            targetIdx = Math.min(savedAnchorIndex, count - 1)
                        }
                        if (targetIdx >= 0 && targetIdx < count) {
                            var item = itemAtIndex(targetIdx)
                            if (!item) {
                                positionViewAtIndex(targetIdx, ListView.Beginning)
                                item = itemAtIndex(targetIdx)
                            }
                            if (item) {
                                var desiredY = item.y - savedAnchorOffset
                                var minY = originY - topMargin
                                var maxY = Math.max(minY, originY + contentHeight + bottomMargin - height)
                                var clampedY = Math.max(minY, Math.min(maxY, desiredY))
                                if (Math.abs(contentY - clampedY) > 1.0) {
                                    contentY = clampedY
                                }
                                if (restoringScrollPosition && Math.abs(contentY - clampedY) <= 1.0 && restorePassesRemaining < 4) {
                                    restoringScrollPosition = false
                                    restorePassesRemaining = 0
                                    scrollRestoreTimer.stop()
                                }
                            } else if (count === savedCount && savedContentHeight > 0) {
                                contentY = originY + savedContentY
                            }
                        } else if (savedContentHeight > 0) {
                            contentY = originY + savedContentY
                        }
                    }

                    function scrollToBottom() {
                        restoringScrollPosition = false
                        restorePassesRemaining = 0
                        scrollRestoreTimer.stop()
                        stickToBottom = true
                        savedStickToBottom = true
                        settlingToBottom = true
                        settlePassesRemaining = 6
                        prevContentHeight = 0
                        prevContentY = 0
                        if (count > 0) {
                            positionViewAtEnd()
                        }
                        Qt.callLater(function() {
                            if (messageListView.count > 0 && messageListView.stickToBottom) {
                                messageListView.positionViewAtEnd()
                            }
                        })
                        bottomSettleTimer.restart()
                    }

                    activeFocusOnTab: true

                    function scrollToTop() {
                        restoringScrollPosition = false
                        restorePassesRemaining = 0
                        scrollRestoreTimer.stop()
                        stickToBottom = false
                        savedStickToBottom = false
                        settlingToBottom = false
                        settlePassesRemaining = 0
                        prevContentHeight = 0
                        prevContentY = 0
                        bottomSettleTimer.stop()
                        if (count > 0) {
                            positionViewAtBeginning()
                        }
                    }

                    Keys.onPressed: function(event) {
                        if (root.showPhotoViewer && root.handlePhotoViewerKey(event)) {
                            return
                        }
                        if (event.key === Qt.Key_PageUp) {
                            root.scrollMessagesPageUp()
                            event.accepted = true
                        } else if (event.key === Qt.Key_PageDown) {
                            root.scrollMessagesPageDown()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Home) {
                            root.scrollMessagesToTop()
                            event.accepted = true
                        } else if (event.key === Qt.Key_End) {
                            root.scrollMessagesToBottom()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Up) {
                            root.scrollMessagesStepUp()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Down) {
                            root.scrollMessagesStepDown()
                            event.accepted = true
                        }
                    }

                    Timer {
                        id: bottomSettleTimer
                        interval: 35
                        repeat: true
                        onTriggered: {
                            if (!messageListView.stickToBottom) {
                                messageListView.settlingToBottom = false
                                stop()
                                return
                            }
                            if (messageListView.count > 0) {
                                messageListView.positionViewAtEnd()
                            }
                            messageListView.settlePassesRemaining -= 1
                            if (messageListView.settlePassesRemaining <= 0) {
                                messageListView.settlingToBottom = false
                                stop()
                            }
                        }
                    }

                    Timer {
                        id: scrollRestoreTimer
                        interval: 30
                        repeat: true
                        onTriggered: {
                            if (!messageListView.restoringScrollPosition || messageListView.stickToBottom) {
                                messageListView.restoringScrollPosition = false
                                stop()
                                return
                            }
                            if (messageListView.count > 0) {
                                messageListView.restoreSavedScrollPosition()
                            }
                            messageListView.restorePassesRemaining -= 1
                            if (messageListView.restorePassesRemaining <= 0) {
                                messageListView.restoringScrollPosition = false
                                stop()
                            }
                        }
                    }

                    WheelHandler {
                        id: messageWheelHandler
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        blocking: false
                        onWheel: function(event) {
                            messageListView.restoringScrollPosition = false
                            scrollRestoreTimer.stop()
                            if (event.angleDelta.y > 0) {
                                messageListView.stickToBottom = false
                                messageListView.savedStickToBottom = false
                                messageListView.settlingToBottom = false
                                messageListView.settlePassesRemaining = 0
                                bottomSettleTimer.stop()
                            } else if (event.angleDelta.y < 0) {
                                Qt.callLater(function() {
                                    if (messageListView.isNearBottom()) {
                                        messageListView.stickToBottom = true
                                        messageListView.savedStickToBottom = true
                                    }
                                })
                            }
                        }
                    }

                    onMovementEnded: {
                        stickToBottom = isNearBottom()
                        savedStickToBottom = stickToBottom
                    }
                    onFlickEnded: {
                        stickToBottom = isNearBottom()
                        savedStickToBottom = stickToBottom
                    }

                    onAtYBeginningChanged: {
                        if (atYBeginning && !stickToBottom && !settlingToBottom && !restoringScrollPosition && (moving || dragging || flicking) && backend.hasOlderMessages && !backend.loadingOlderMessages) {
                            stickToBottom = false
                            prevContentHeight = contentHeight
                            prevContentY = contentY
                            backend.loadOlderMessages()
                        }
                    }

                    onCountChanged: {
                        if (stickToBottom && !restoringScrollPosition) {
                            scrollToBottom()
                        }
                    }

                    onContentHeightChanged: {
                        if (stickToBottom) {
                            if (contentHeight > 0 && count > 0) {
                                positionViewAtEnd()
                                Qt.callLater(function() {
                                    if (messageListView.stickToBottom && messageListView.count > 0) {
                                        messageListView.positionViewAtEnd()
                                    }
                                })
                            }
                        } else if (restoringScrollPosition) {
                            restoreSavedScrollPosition()
                        } else if (prevContentHeight > 0 && contentHeight > prevContentHeight) {
                            // Maintain scroll position when older messages are prepended at the top
                            var delta = contentHeight - prevContentHeight
                            contentY = prevContentY + delta
                            prevContentHeight = contentHeight
                            prevContentY = contentY
                        }
                    }

                    onMovementStarted: {
                        notePointerActivity()
                        smoothScrollAnim.stop()
                        root.hoveredActionMessageGuid = ""
                        restoringScrollPosition = false
                        scrollRestoreTimer.stop()
                    }

                    Component.onCompleted: {
                        if (backend.selectedChatGuid.length > 0) {
                            lastSelectedChatGuid = backend.selectedChatGuid
                            scrollToBottom()
                        }
                    }

                    Connections {
                        target: backend
                        function onSelectedChatChanged() {
                            if (backend.selectedChatGuid !== messageListView.lastSelectedChatGuid) {
                                smoothScrollAnim.stop()
                                messageListView.lastSelectedChatGuid = backend.selectedChatGuid
                                messageListView.scrollToBottom()
                            }
                        }
                        function onAboutToChangeMessages() {
                            messageListView.snapshotScrollState()
                        }
                        function onMessagesChanged() {
                            if (backend.selectedChatGuid !== messageListView.lastSelectedChatGuid) {
                                return
                            }
                            if (messageListView.savedStickToBottom || messageListView.stickToBottom) {
                                messageListView.restoringScrollPosition = false
                                scrollRestoreTimer.stop()
                                messageListView.scrollToBottom()
                            } else {
                                messageListView.stickToBottom = false
                                messageListView.restoringScrollPosition = true
                                messageListView.restorePassesRemaining = 4
                                messageListView.restoreSavedScrollPosition()
                                Qt.callLater(function() {
                                    if (!messageListView.stickToBottom && messageListView.count > 0) {
                                        messageListView.restoreSavedScrollPosition()
                                    }
                                })
                                scrollRestoreTimer.restart()
                            }
                        }
                    }

                    header: Item {
                        width: messageListView.width
                        height: (backend.hasOlderMessages || backend.loadingOlderMessages) ? 44 : 0
                        visible: backend.hasOlderMessages || backend.loadingOlderMessages

                        Rectangle {
                            anchors.centerIn: parent
                            height: 30
                            width: headerRow.implicitWidth + 24
                            radius: 15
                            color: loadOlderHover.containsMouse ? theme.lighterBackground : theme.darkerBackground
                            border.width: 1
                            border.color: theme.lighterBackground

                            RowLayout {
                                id: headerRow
                                anchors.centerIn: parent
                                spacing: 8

                                Text {
                                    text: backend.loadingOlderMessages ? "↻" : "↑"
                                    font.pixelSize: 13
                                    font.bold: true
                                    color: theme.accent
                                }

                                Text {
                                    text: backend.loadingOlderMessages ? i18n.tr("Loading older messages...") : i18n.tr("Load older messages")
                                    font.pixelSize: 12
                                    color: theme.foreground
                                }
                            }

                            MouseArea {
                                id: loadOlderHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                enabled: !backend.loadingOlderMessages && backend.hasOlderMessages
                                onClicked: {
                                    messageListView.stickToBottom = false
                                    messageListView.prevContentHeight = messageListView.contentHeight
                                    messageListView.prevContentY = messageListView.contentY
                                    backend.loadOlderMessages()
                                }
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        onClicked: {
                            root.clearActionMenu()
                            messageListView.forceActiveFocus()
                        }
                    }

                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                    }

                    // Floating "Top (Ctrl+Home)" arrow (replaces the header's Top button). Parented to
                    // the ListView itself so it stays put while the content scrolls.
                    Rectangle {
                        id: scrollToTopArrow
                        objectName: "scrollToTopArrow"
                        parent: messageListView
                        z: 20
                        width: 28
                        height: 28
                        radius: 14
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.top: parent.top
                        anchors.topMargin: 4
                        // Only while the mouse moves or the list scrolls; fades out when idle
                        readonly property bool needed: messageListView.count > 0 && (messageListView.contentY > messageListView.originY + messageListView.topMargin + 40)
                        readonly property bool shown: needed && (messageListView.pointerActive || scrollToTopArrowMouse.containsMouse)
                        visible: needed && opacity > 0
                        enabled: shown
                        opacity: !shown ? 0 : (scrollToTopArrowMouse.containsMouse ? 0.95 : 0.45)
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                        color: scrollToTopArrowMouse.containsMouse ? theme.accent : theme.lighterBackground
                        border.width: 1
                        border.color: theme.accent
                        ToolTip.visible: scrollToTopArrowMouse.containsMouse
                        ToolTip.text: i18n.tr("Top (Ctrl+Home)")
                        ToolTip.delay: 400
                        Text {
                            anchors.centerIn: parent
                            text: "↑"
                            font.pixelSize: theme.fontSize - 1
                            font.bold: true
                            color: scrollToTopArrowMouse.containsMouse ? theme.darkerBackground : theme.accent
                        }
                        MouseArea {
                            id: scrollToTopArrowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: messageListView.smoothScrollTo(false)
                        }
                    }

                    // Floating "Newest messages (Ctrl+End)" arrow (replaces the header's Bottom button). Parented to
                    // the ListView itself so it stays put while the content scrolls.
                    Rectangle {
                        id: scrollToBottomArrow
                        objectName: "scrollToBottomArrow"
                        parent: messageListView
                        z: 20
                        width: 28
                        height: 28
                        radius: 14
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 8
                        // Only while the mouse moves or the list scrolls; fades out when idle
                        readonly property bool needed: messageListView.count > 0 && (!messageListView.atYEnd && messageListView.contentHeight > messageListView.height)
                        readonly property bool shown: needed && (messageListView.pointerActive || scrollToBottomArrowMouse.containsMouse)
                        visible: needed && opacity > 0
                        enabled: shown
                        opacity: !shown ? 0 : (scrollToBottomArrowMouse.containsMouse ? 0.95 : 0.45)
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                        color: scrollToBottomArrowMouse.containsMouse ? theme.accent : theme.lighterBackground
                        border.width: 1
                        border.color: theme.accent
                        ToolTip.visible: scrollToBottomArrowMouse.containsMouse
                        ToolTip.text: i18n.tr("Newest messages (Ctrl+End)")
                        ToolTip.delay: 400
                        Text {
                            anchors.centerIn: parent
                            text: "↓"
                            font.pixelSize: theme.fontSize - 1
                            font.bold: true
                            color: scrollToBottomArrowMouse.containsMouse ? theme.darkerBackground : theme.accent
                        }
                        MouseArea {
                            id: scrollToBottomArrowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: messageListView.smoothScrollTo(true)
                        }
                    }

                    delegate: ColumnLayout {
                        id: msgDelegate
                        required property var modelData
                        required property int index
                        objectName: "messageDelegate"
                        readonly property string messageGuid: modelData.guid || ""
                        property bool invisibleInkRevealed: false
                        readonly property bool isGreenService: Boolean(modelData.isGreenBubble || modelData.service === "RCS" || modelData.service === "SMS" || (!modelData.service && root.selectedChatIsGreen))
                        readonly property bool isInvisibleInkMsg: (modelData.expressiveEffect === "🕵️ Invisible Ink") || ((modelData.sendEffectLabel || "").toLowerCase().indexOf("invisible ink") !== -1)
                        readonly property bool isAnyPartHovered: (bubbleHover.hovered || (typeof msgTextHover !== "undefined" && msgTextHover.hovered) || actionZoneHover.hovered || actionBarHover.hovered || (typeof invisibleInkMouse !== "undefined" && invisibleInkMouse.containsMouse))
                        width: messageListView.width
                        spacing: 4
                        property string hoveredReactionEmoji: ""
                        z: (root.activeActionMessageGuid === modelData.guid || hoveredReactionEmoji.length > 0) ? 100 : 1

                        Timer {
                            id: hoverClearTimer
                            interval: 380
                            repeat: false
                            onTriggered: {
                                if (!msgDelegate.isAnyPartHovered && root.hoveredActionMessageGuid === modelData.guid) {
                                    root.hoveredActionMessageGuid = ""
                                }
                            }
                        }

                        onIsAnyPartHoveredChanged: {
                            if (isAnyPartHovered) {
                                hoverClearTimer.stop()
                                root.hoveredActionMessageGuid = modelData.guid
                            } else {
                                hoverClearTimer.restart()
                            }
                        }

                        function playBubbleEffect(effectLabel) {
                            var eff = (effectLabel || "").toLowerCase()
                            if (eff.indexOf("slam") !== -1 || eff.indexOf("impact") !== -1) {
                                slamAnim.restart()
                            } else if (eff.indexOf("loud") !== -1) {
                                loudAnim.restart()
                            } else if (eff.indexOf("gentle") !== -1) {
                                gentleAnim.restart()
                            }
                        }

                        Component.onCompleted: {
                            var eff = modelData.expressiveEffect || modelData.sendEffectLabel || ""
                            if (eff.length > 0 && modelData.isFromMe && index === messageListView.count - 1) {
                                playBubbleEffect(eff)
                                root.triggerExpressiveEffect(eff)
                            }
                        }

                        property string downloadedAttachmentUrl: ""
                        property var downloadedAttachmentUrls: ({})
                        readonly property string liveAttachmentUrl: (downloadedAttachmentUrl.length > 0) ? downloadedAttachmentUrl : (modelData.attachmentLocalUrl || "")
                        property var liveLinkPreview: modelData.linkPreview || null
                        property bool liveHasPreview: modelData.hasLinkPreview || (liveLinkPreview !== null && liveLinkPreview !== undefined && (liveLinkPreview.url || "").length > 0)

                        // Like iOS Messages: photos and up to three emoji sent without
                        // text are shown on their own, without a bubble.
                        readonly property bool hasCaptionText: {
                            var t = modelData.text || ""
                            if (t.length === 0) return false
                            return !(modelData.hasImageAttachment && (t === "📷 Photo" || /^📷 \d+ Photos$/.test(t)))
                        }
                        readonly property bool hasNonImageContent: Boolean(modelData.hasFileAttachment || liveHasPreview || isInvisibleInkMsg
                                                                         || (modelData.verificationCode || "").length > 0
                                                                         || (modelData.attachments || []).some(function(a) { return a.isAudio === true }))
                        readonly property bool isBubblelessMedia: Boolean(modelData.hasImageAttachment) && !hasCaptionText && !hasNonImageContent
                        readonly property int jumboEmojiCount: (!modelData.hasImageAttachment && !hasNonImageContent)
                                                               ? backend.jumboEmojiCount(modelData.text || "") : 0
                        readonly property bool isBubbleless: isBubblelessMedia || jumboEmojiCount > 0

                        function resolveAttachmentUrl(attGuid, initialUrl) {
                            if (attGuid && downloadedAttachmentUrls && downloadedAttachmentUrls[attGuid]) {
                                return downloadedAttachmentUrls[attGuid]
                            }
                            if (attGuid && attGuid.length >= 16 && downloadedAttachmentUrls && downloadedAttachmentUrls[attGuid.substring(0, 16)]) {
                                return downloadedAttachmentUrls[attGuid.substring(0, 16)]
                            }
                            if (initialUrl && initialUrl.length > 0) {
                                return initialUrl
                            }
                            var primaryGuid = modelData.attachmentGuid || ""
                            if (attGuid === primaryGuid && downloadedAttachmentUrl.length > 0) {
                                return downloadedAttachmentUrl
                            }
                            // Resolved after this delegate's model snapshot was taken
                            return attGuid ? backend.cachedAttachmentUrl(attGuid) : ""
                        }

                        function copyPrimaryImageAttachment() {
                            var imgList = modelData.imageAttachments || []
                            var firstImg = imgList.length > 0 ? imgList[0] : null
                            // Prefer the photo that was right-clicked
                            if (root.pinnedActionMessageGuid === modelData.guid && root.pinnedActionAttachmentGuid !== "") {
                                for (var ci = 0; ci < imgList.length; ++ci) {
                                    if (imgList[ci] && imgList[ci].guid === root.pinnedActionAttachmentGuid) { firstImg = imgList[ci]; break }
                                }
                            }
                            var guid = (firstImg && firstImg.guid) ? firstImg.guid : (modelData.attachmentGuid || "")
                            var rawUrl = (firstImg && firstImg.localUrl) ? firstImg.localUrl : (msgDelegate.liveAttachmentUrl || modelData.attachmentLocalUrl || "")
                            var resolvedUrl = msgDelegate.resolveAttachmentUrl(guid, rawUrl)
                            return backend.copyImageToClipboard(resolvedUrl, guid)
                        }

                        Connections {
                            target: backend
                            function onAttachmentReady(guid, localUrl) {
                                var attGuid = modelData.attachmentGuid || ""
                                if (attGuid === guid || (attGuid.length >= 16 && guid.length >= 16 && attGuid.substring(0, 16) === guid.substring(0, 16))) {
                                    msgDelegate.downloadedAttachmentUrl = localUrl
                                }
                                if (guid && guid.length > 0) {
                                    var updated = Object.assign({}, msgDelegate.downloadedAttachmentUrls || {})
                                    updated[guid] = localUrl
                                    if (guid.length >= 16) {
                                        updated[guid.substring(0, 16)] = localUrl
                                    }
                                    msgDelegate.downloadedAttachmentUrls = updated
                                }
                            }
                            function onLinkPreviewUpdated(guid, previewData) {
                                if (modelData.guid === guid) {
                                    msgDelegate.liveLinkPreview = previewData
                                    msgDelegate.liveHasPreview = (previewData !== null && previewData !== undefined && (previewData.url || "").length > 0)
                                }
                            }
                        }

                        // Date Header Separator
                        Rectangle {
                            visible: modelData.showDateHeader || false
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 8
                            Layout.bottomMargin: 6
                            height: 22
                            radius: 11
                            color: theme.darkBackground
                            implicitWidth: dateHdrTxt.implicitWidth + 20

                            Text {
                                id: dateHdrTxt
                                anchors.centerIn: parent
                                text: modelData.dateHeaderText || ""
                                font.pixelSize: theme.fontSize - 3
                                color: theme.darkForeground
                            }
                        }

                        // Sender Name (for incoming messages in group chats)
                        Text {
                            visible: !modelData.isFromMe
                            Layout.alignment: Qt.AlignLeft
                            Layout.leftMargin: 24
                            text: modelData.senderName
                            font.pixelSize: theme.fontSize - 3
                            font.bold: true
                            color: modelData.senderColor || theme.accent
                        }

                        // Reply Context Banner above bubble
                        Rectangle {
                            id: replyContextBanner
                            objectName: "replyContextBanner"
                            visible: (modelData.replyToText || "").length > 0
                            Layout.alignment: modelData.isFromMe ? Qt.AlignRight : Qt.AlignLeft
                            Layout.leftMargin: modelData.isFromMe ? 80 : 24
                            Layout.rightMargin: modelData.isFromMe ? 24 : 80
                            readonly property int maxBannerWidth: Math.min(420, Math.max(220, Math.floor((messageListView.width > 0 ? messageListView.width : 640) * 0.65)))
                            Layout.maximumWidth: maxBannerWidth
                            radius: 6
                            clip: true
                            color: replyBannerMouse.containsMouse ? theme.lighterBackground : theme.darkBackground
                            border.width: 1
                            border.color: replyBannerMouse.containsMouse ? theme.accent : theme.muted
                            implicitWidth: Math.min(maxBannerWidth, Math.max(replySenderTxt.implicitWidth, Math.min(maxBannerWidth - 16, replyBodyTxt.implicitWidth)) + 16)
                            implicitHeight: replyPreviewCol.implicitHeight + 12

                            // Click the quote to see the whole thread (and jump to the original)
                            MouseArea {
                                id: replyBannerMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.openThread(modelData.replyToGuid || "")
                            }
                            ToolTip.visible: replyBannerMouse.containsMouse
                            ToolTip.text: i18n.tr("Show thread")
                            ToolTip.delay: 500

                            ColumnLayout {
                                id: replyPreviewCol
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 6
                                spacing: 2
                                Text {
                                    id: replySenderTxt
                                    Layout.fillWidth: true
                                    text: "↳ " + (modelData.replyToSender || "")
                                    font.pixelSize: theme.fontSize - 4
                                    font.bold: true
                                    color: theme.accent
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                }
                                Text {
                                    id: replyBodyTxt
                                    objectName: "replyBodyTxt"
                                    Layout.fillWidth: true
                                    text: (modelData.replyToText || "").replace(/[\r\n]+/g, " ").trim()
                                    font.pixelSize: theme.fontSize - 3
                                    color: theme.darkForeground
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        // Main Bubble Row
                        RowLayout {
                            id: msgRow
                            Layout.fillWidth: true
                            Layout.leftMargin: 20
                            Layout.rightMargin: 20
                            Layout.minimumHeight: 0
                            spacing: 8

                            Item {
                                visible: modelData.isFromMe
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: root.clearActionMenu()
                                }
                            }

                            // Container wrapping Bubble and ActionBar with single unified zero-gap hover
                            Item {
                                id: bubbleContainer
                                implicitWidth: bubbleRect.implicitWidth
                                implicitHeight: bubbleRect.implicitHeight
                                z: (root.activeActionMessageGuid === modelData.guid) ? 50 : 1

                                // Invisible bridge item covering bubbleRect + gap + actionBar beside it
                                Item {
                                    id: actionHoverZone
                                    visible: root.activeActionMessageGuid === modelData.guid
                                    x: Math.min(-12, actionBar.x - 12)
                                    y: Math.min(-8, actionBar.y - 8)
                                    width: Math.max(bubbleRect.width, actionBar.x + actionBar.width) - Math.min(-12, actionBar.x - 12) + 24
                                    height: Math.max(bubbleRect.height, actionBar.y + actionBar.height) - Math.min(-8, actionBar.y - 8) + 16

                                    HoverHandler {
                                        id: actionZoneHover
                                    }
                                }

                                // Tapback & Reply Action Bar (on left side when possible, beside bubble)
                                Rectangle {
                                    id: actionBar
                                    visible: root.activeActionMessageGuid === modelData.guid
                                    x: {
                                        if (modelData.isFromMe) {
                                            return -Math.min(width + 8, Math.max(0, bubbleContainer.x));
                                        } else {
                                            if (bubbleContainer.x >= width + 8) {
                                                return -width - 8;
                                            } else {
                                                return bubbleRect.width + 8;
                                            }
                                        }
                                    }
                                    y: Math.round((bubbleRect.height - height) / 2)
                                    height: 36
                                    implicitWidth: actionBarRow.implicitWidth + 12
                                    radius: 18
                                    color: theme.darkBackground
                                    border.width: 1
                                    border.color: theme.accent
                                    z: 50

                                    HoverHandler {
                                        id: actionBarHover
                                    }

                                    RowLayout {
                                        id: actionBarRow
                                        anchors.centerIn: parent
                                        spacing: 4

                                        Repeater {
                                            model: ["❤️", "👍", "👎", "😂", "‼️", "❓"]
                                            delegate: Rectangle {
                                                required property string modelData
                                                width: 30
                                                height: 30
                                                radius: 15
                                                color: reactBtnMouse.containsMouse ? theme.selection : theme.lighterBackground
                                                border.width: 1
                                                border.color: reactBtnMouse.containsMouse ? theme.accent : theme.muted

                                                Text {
                                                    anchors.centerIn: parent
                                                    text: modelData
                                                    font.pixelSize: theme.fontSize
                                                }

                                                MouseArea {
                                                    id: reactBtnMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        var cGuid = backend.selectedChatGuid
                                                        var mGuid = msgDelegate.modelData.guid
                                                        var reactEmoji = modelData
                                                        root.clearActionMenu()
                                                        backend.sendReaction(cGuid, mGuid, reactEmoji)
                                                    }
                                                }
                                            }
                                        }

                                        Rectangle {
                                            id: customReactionAddBtn
                                            objectName: "customReactionAddBtn"
                                            width: 30
                                            height: 30
                                            radius: 15
                                            color: (customReactBtnMouse.containsMouse || (root.showTapbackEmojiPicker && root.tapbackEmojiTargetGuid === msgDelegate.modelData.guid))
                                                   ? theme.selection
                                                   : theme.lighterBackground
                                            border.width: 1
                                            border.color: (customReactBtnMouse.containsMouse || (root.showTapbackEmojiPicker && root.tapbackEmojiTargetGuid === msgDelegate.modelData.guid))
                                                          ? theme.accent
                                                          : theme.muted
                                            ToolTip.visible: customReactBtnMouse.containsMouse
                                            ToolTip.text: i18n.tr("Add custom emoji reaction")
                                            ToolTip.delay: 300

                                            Text {
                                                anchors.centerIn: parent
                                                text: "➕"
                                                font.pixelSize: theme.fontSize - 2
                                                font.bold: true
                                                color: theme.brightForeground
                                            }

                                            MouseArea {
                                                id: customReactBtnMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var mGuid = msgDelegate.modelData.guid
                                                    if (root.showTapbackEmojiPicker && root.tapbackEmojiTargetGuid === mGuid) {
                                                        root.closeTapbackEmojiPicker()
                                                    } else {
                                                        var pt = customReactionAddBtn.mapToItem(root.contentItem, 0, customReactionAddBtn.height + 6)
                                                        root.openTapbackEmojiPicker(mGuid, pt.x, pt.y)
                                                    }
                                                }
                                            }
                                        }

                                        Rectangle {
                                            height: 30
                                            implicitWidth: replyBtnRow.implicitWidth + 14
                                            radius: 15
                                            color: replyBtnMouse.containsMouse ? theme.selection : theme.lighterBackground
                                            border.width: 1
                                            border.color: replyBtnMouse.containsMouse ? theme.accent : theme.muted

                                            RowLayout {
                                                id: replyBtnRow
                                                anchors.centerIn: parent
                                                spacing: 4
                                                Text {
                                                    text: "↩"
                                                    font.pixelSize: theme.fontSize
                                                    color: theme.accent
                                                }
                                                Text {
                                                    text: i18n.tr("Reply")
                                                    font.pixelSize: theme.fontSize - 3
                                                    font.bold: true
                                                    color: theme.brightForeground
                                                }
                                            }

                                            MouseArea {
                                                id: replyBtnMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    root.cancelEditing()
                                                    root.replyTargetGuid = msgDelegate.modelData.guid
                                                    root.replyTargetSender = msgDelegate.modelData.senderName
                                                    root.replyTargetText = msgDelegate.modelData.text
                                                    root.clearActionMenu()
                                                    composerInput.forceActiveFocus()
                                                }
                                            }
                                        }

                                        Rectangle {
                                            id: editMsgBtn
                                            objectName: "editMsgBtn"
                                            visible: root.canEditOrUnsend(msgDelegate.modelData) && (msgDelegate.modelData.text || "").trim().length > 0
                                            height: 30
                                            implicitWidth: editBtnRow.implicitWidth + 14
                                            radius: 15
                                            color: editBtnMouse.containsMouse ? theme.selection : theme.lighterBackground
                                            border.width: 1
                                            border.color: editBtnMouse.containsMouse ? theme.accent : theme.muted

                                            RowLayout {
                                                id: editBtnRow
                                                anchors.centerIn: parent
                                                spacing: 4
                                                Text {
                                                    text: "✎"
                                                    font.pixelSize: theme.fontSize - 1
                                                    color: theme.accent
                                                }
                                                Text {
                                                    text: i18n.tr("Edit")
                                                    font.pixelSize: theme.fontSize - 3
                                                    font.bold: true
                                                    color: theme.brightForeground
                                                }
                                            }

                                            MouseArea {
                                                id: editBtnMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var mGuid = msgDelegate.modelData.guid
                                                    var mText = msgDelegate.modelData.text || ""
                                                    root.clearActionMenu()
                                                    root.startEditing(mGuid, mText)
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: (msgDelegate.modelData.text || "").length > 0 || Boolean(msgDelegate.modelData.hasImageAttachment)
                                            height: 30
                                            implicitWidth: copyBtnRow.implicitWidth + 14
                                            radius: 15
                                            color: copyBtnMouse.containsMouse ? theme.selection : theme.lighterBackground
                                            border.width: 1
                                            border.color: copyBtnMouse.containsMouse ? theme.accent : theme.muted

                                            RowLayout {
                                                id: copyBtnRow
                                                anchors.centerIn: parent
                                                spacing: 4
                                                Text {
                                                    text: "󰆏"
                                                    font.pixelSize: theme.fontSize - 1
                                                    color: theme.accent
                                                }
                                                Text {
                                                    text: (msgDelegate.modelData.hasImageAttachment && !(msgText.selectedText && msgText.selectedText.length > 0))
                                                          ? i18n.tr("Copy Image")
                                                          : i18n.tr("Copy")
                                                    font.pixelSize: theme.fontSize - 3
                                                    font.bold: true
                                                    color: theme.brightForeground
                                                }
                                            }

                                            MouseArea {
                                                id: copyBtnMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    if (msgText.selectedText && msgText.selectedText.length > 0) {
                                                        backend.copyToClipboard(msgText.selectedText, "selection")
                                                    } else if (msgDelegate.modelData.hasImageAttachment) {
                                                        msgDelegate.copyPrimaryImageAttachment()
                                                    } else {
                                                        backend.copyToClipboard(msgDelegate.modelData.text || "", "message")
                                                    }
                                                    root.clearActionMenu()
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: (msgDelegate.modelData.verificationCode || "").length > 0
                                            height: 30
                                            implicitWidth: codeBtnRow.implicitWidth + 14
                                            radius: 15
                                            color: codeBtnMouse.containsMouse ? theme.selection : theme.lighterBackground
                                            border.width: 1
                                            border.color: codeBtnMouse.containsMouse ? theme.accent : theme.muted

                                            RowLayout {
                                                id: codeBtnRow
                                                anchors.centerIn: parent
                                                spacing: 4
                                                Text {
                                                    text: "󰌆"
                                                    font.pixelSize: theme.fontSize - 1
                                                    color: theme.accent
                                                }
                                                Text {
                                                    text: msgDelegate.modelData.verificationCode || i18n.tr("Code")
                                                    font.pixelSize: theme.fontSize - 3
                                                    font.bold: true
                                                    color: theme.brightForeground
                                                }
                                            }

                                            MouseArea {
                                                id: codeBtnMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var code = msgDelegate.modelData.verificationCode || ""
                                                    backend.copyToClipboard(code, "Code " + code)
                                                    root.clearActionMenu()
                                                }
                                            }
                                        }

                                        Rectangle {
                                            id: unsendMsgBtn
                                            objectName: "unsendMsgBtn"
                                            // iMessage allows unsending for 2 minutes after sending
                                            visible: root.canEditOrUnsend(msgDelegate.modelData)
                                            height: 30
                                            implicitWidth: unsendBtnRow.implicitWidth + 14
                                            radius: 15
                                            color: unsendBtnMouse.containsMouse ? theme.selection : theme.lighterBackground
                                            border.width: 1
                                            border.color: unsendBtnMouse.containsMouse ? theme.accent : theme.muted

                                            RowLayout {
                                                id: unsendBtnRow
                                                anchors.centerIn: parent
                                                spacing: 4
                                                Text {
                                                    text: "󰕌"
                                                    font.pixelSize: theme.fontSize - 1
                                                    color: theme.accent
                                                }
                                                Text {
                                                    text: i18n.tr("Unsend")
                                                    font.pixelSize: theme.fontSize - 3
                                                    font.bold: true
                                                    color: theme.brightForeground
                                                }
                                            }

                                            MouseArea {
                                                id: unsendBtnMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var mGuid = msgDelegate.modelData.guid
                                                    root.clearActionMenu()
                                                    backend.unsendMessage(mGuid)
                                                }
                                            }
                                        }

                                        Rectangle {
                                            id: deleteMsgBtn
                                            height: 30
                                            implicitWidth: deleteBtnRow.implicitWidth + 14
                                            radius: 15
                                            color: deleteBtnMouse.containsMouse ? "#3b1d26" : theme.lighterBackground
                                            border.width: 1
                                            border.color: deleteBtnMouse.containsMouse ? "#f7768e" : theme.muted

                                            RowLayout {
                                                id: deleteBtnRow
                                                anchors.centerIn: parent
                                                spacing: 4
                                                Text {
                                                    text: "󰆴"
                                                    font.pixelSize: theme.fontSize - 1
                                                    color: "#f7768e"
                                                }
                                                Text {
                                                    text: i18n.tr("Delete")
                                                    font.pixelSize: theme.fontSize - 3
                                                    font.bold: true
                                                    color: deleteBtnMouse.containsMouse ? "#f7768e" : theme.brightForeground
                                                }
                                            }

                                            MouseArea {
                                                id: deleteBtnMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var mGuid = msgDelegate.modelData.guid
                                                    root.clearActionMenu()
                                                    backend.deleteMessage(mGuid)
                                                }
                                            }
                                        }
                                    }
                                }

                                // Message Bubble Card
                                Rectangle {
                                    id: bubbleRect
                                    anchors.fill: parent
                                    objectName: "messageBubbleRect"
                                    radius: theme.radius + 2

                                    // Flash after jumping here from a thread
                                    Rectangle {
                                        objectName: "jumpHighlightRing"
                                        anchors.fill: parent
                                        radius: parent.radius
                                        color: "transparent"
                                        border.width: 3
                                        border.color: theme.accent
                                        z: 10
                                        opacity: root.highlightGuid !== "" && root.highlightGuid === modelData.guid ? 1 : 0
                                        Behavior on opacity { NumberAnimation { duration: 250 } }
                                    }
                                    color: msgDelegate.isBubbleless ? "transparent"
                                           : (modelData.isFromMe
                                              ? (msgDelegate.isGreenService ? "#1B6E38" : "#0A84FF")
                                              : "#1e2430")
                                    border.width: msgDelegate.isBubbleless ? 0 : 1  // no outline on bare photos/emoji, even with actions open
                                    border.color: (root.activeActionMessageGuid === modelData.guid)
                                                  ? theme.brightForeground
                                                  : (modelData.isFromMe
                                                     ? (msgDelegate.isGreenService ? "#268C48" : "#0066CC")
                                                     : "#2c3545")
                                    implicitWidth: msgDelegate.isBubbleless ? bubbleContent.width : Math.max(bubbleContent.width + 28, 96)
                                    implicitHeight: msgDelegate.isBubbleless ? bubbleContent.height : bubbleContent.height + 20

                                    transformOrigin: Item.Center

                                    SequentialAnimation {
                                        id: slamAnim
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 1.38; duration: 120; easing.type: Easing.OutQuad }
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 0.90; duration: 100; easing.type: Easing.InOutQuad }
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 1.06; duration: 90; easing.type: Easing.InOutQuad }
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 1.0; duration: 80; easing.type: Easing.OutQuad }
                                    }

                                    SequentialAnimation {
                                        id: loudAnim
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 1.24; duration: 140; easing.type: Easing.OutQuad }
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 0.95; duration: 110; easing.type: Easing.InOutQuad }
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 1.14; duration: 110; easing.type: Easing.InOutQuad }
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 1.0; duration: 100; easing.type: Easing.OutQuad }
                                    }

                                    SequentialAnimation {
                                        id: gentleAnim
                                        PropertyAction { target: bubbleRect; property: "scale"; value: 0.70 }
                                        NumberAnimation { target: bubbleRect; property: "scale"; to: 1.0; duration: 600; easing.type: Easing.OutCubic }
                                    }

                                    HoverHandler {
                                        id: bubbleHover
                                    }

                                    MouseArea {
                                        id: bubbleClickArea
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        onClicked: (mouse) => {
                                            if (mouse.button === Qt.RightButton) {
                                                // Right-click opens (or closes) this message's actions
                                                root.pinnedActionMessageGuid = (root.pinnedActionMessageGuid === modelData.guid) ? "" : modelData.guid
                                                root.pinnedActionAttachmentGuid = ""
                                                return
                                            }
                                            // Left-click just dismisses an open action bar
                                            if (root.pinnedActionMessageGuid !== "") root.clearActionMenu()
                                        }
                                        onDoubleClicked: (mouse) => {
                                            var cGuid = backend.selectedChatGuid
                                            var mGuid = modelData.guid
                                            root.clearActionMenu()
                                            backend.sendReaction(cGuid, mGuid, "❤️")
                                        }
                                    }

                                    Column {
                                        id: bubbleContent
                                        x: msgDelegate.isBubbleless ? 0 : 14
                                        y: msgDelegate.isBubbleless ? 0 : 10
                                        spacing: msgDelegate.isBubbleless ? 2 : 6
                                        readonly property real maxTextW: Math.max(180, Math.floor(messageListView.width * 0.62) - 28)

                                        // Inline Image Attachment Previews (supports 1..N photos per message)
                                        Repeater {
                                            id: imageAttachmentRepeater
                                            model: (msgDelegate.modelData.imageAttachments && msgDelegate.modelData.imageAttachments.length > 0)
                                                   ? msgDelegate.modelData.imageAttachments
                                                   : ((msgDelegate.modelData.hasImageAttachment || false)
                                                      ? [{ guid: msgDelegate.modelData.attachmentGuid || "", name: msgDelegate.modelData.attachmentName || "", localUrl: msgDelegate.liveAttachmentUrl }]
                                                      : [])
                                            delegate: Rectangle {
                                                required property var modelData
                                                required property int index
                                                id: inlineImageCard
                                                objectName: "inlineImageAttachmentCard"
                                                readonly property string resolvedImgUrl: msgDelegate.resolveAttachmentUrl(modelData.guid || "", modelData.localUrl || "")
                                                readonly property bool isAnimated: backend.isAnimatedImageAttachment(modelData.mimeType || "", modelData.name || "")
                                                                                 || backend.isAnimatedImageAttachment("", resolvedImgUrl)
                                                readonly property var media: isAnimated ? inlineGif : inlineImg
                                                readonly property bool mediaReady: media.status === Image.Ready && media.implicitWidth > 0 && media.implicitHeight > 0
                                                // Keep the image's own aspect ratio: scale it down to fit the
                                                // message column (never up), so nothing is cropped.
                                                readonly property real maxImageW: Math.min(msgDelegate.isBubblelessMedia ? 360 : 320, bubbleContent.maxTextW + (msgDelegate.isBubblelessMedia ? 0 : 28))
                                                readonly property real maxImageH: 360
                                                readonly property real fitScale: mediaReady ? Math.min(1, maxImageW / media.implicitWidth, maxImageH / media.implicitHeight) : 1
                                                // Media is drawn at its fitted size (never enlarged); the card
                                                // keeps a 48px minimum only as a click target.
                                                readonly property real fittedW: mediaReady ? Math.max(1, Math.round(media.implicitWidth * fitScale)) : Math.min(240, maxImageW)
                                                readonly property real fittedH: mediaReady ? Math.max(1, Math.round(media.implicitHeight * fitScale)) : 170
                                                width: Math.max(48, fittedW)
                                                height: Math.max(48, fittedH)
                                                radius: msgDelegate.isBubblelessMedia ? 16 : 8
                                                color: mediaReady ? "transparent" : theme.darkBackground
                                                clip: true
                                                // Rounded corners need QtQuick.Effects and a GPU scene graph
                                                // (shader effects draw nothing with the software renderer);
                                                // otherwise the image is shown with square corners.
                                                readonly property Component cornerMask: root.roundedCornerMaskComponent
                                                layer.enabled: mediaReady && cornerMask !== null && GraphicsInfo.api !== GraphicsInfo.Software
                                                layer.effect: cornerMask
                                                // Decode no wider than the widest card (360px at the screen's
                                                // pixel ratio): a 12 MP photo otherwise decodes and uploads ~48 MB
                                                // just to be drawn at 360px. Width only: with both bounds set Qt
                                                // scales to cover the box, so wide images would stay huge.
                                                readonly property real decodeBox: Math.ceil(360 * Math.max(1, Screen.devicePixelRatio))
                                                // GIFs play only on screen: ListView keeps delegates up to its
                                                // cacheBuffer away alive (and "visible") too. Still images skip
                                                // the check so scrolling doesn't re-evaluate it for every photo.
                                                readonly property bool inView: {
                                                    if (!isAnimated) return false
                                                    // This card's own rectangle, re-checked when the list
                                                    // scrolls or the delegate or card moves.
                                                    void (msgDelegate.y + y)  // dependencies for mapToItem()
                                                    const top = mapToItem(messageListView.contentItem, 0, 0).y
                                                    return top + height > messageListView.contentY
                                                        && top < messageListView.contentY + messageListView.height
                                                }

                                                Image {
                                                    id: inlineImg
                                                    objectName: "inlineStillImage"
                                                    anchors.centerIn: parent
                                                    width: parent.fittedW
                                                    height: parent.fittedH
                                                    source: parent.isAnimated ? "" : parent.resolvedImgUrl
                                                    asynchronous: true
                                                    cache: true
                                                    autoTransform: true
                                                    smooth: true
                                                    sourceSize.width: parent.decodeBox
                                                    fillMode: Image.PreserveAspectFit
                                                    visible: !parent.isAnimated && status === Image.Ready
                                                }

                                                AnimatedImage {
                                                    id: inlineGif
                                                    objectName: "inlineAnimatedImage"
                                                    anchors.centerIn: parent
                                                    width: parent.fittedW
                                                    height: parent.fittedH
                                                    source: parent.isAnimated ? parent.resolvedImgUrl : ""
                                                    asynchronous: true
                                                    // Don't keep every decoded frame: a large GIF would hold
                                                    // them all at native size. (The photo viewer still caches.)
                                                    cache: false
                                                    smooth: true
                                                    fillMode: Image.PreserveAspectFit
                                                    // No sourceSize: AnimatedImage scales small GIFs *up* to it.
                                                    playing: visible && parent.inView
                                                    visible: parent.isAnimated && status === Image.Ready
                                                }

                                                // Subtle photo index badge when message has multiple photos (e.g. 1/3, 2/3, 3/3)
                                                Rectangle {
                                                    visible: imageAttachmentRepeater.count > 1
                                                    anchors.top: parent.top
                                                    anchors.right: parent.right
                                                    anchors.margins: 6
                                                    height: 20
                                                    radius: 10
                                                    implicitWidth: photoIdxTxt.implicitWidth + 12
                                                    color: "#B3000000"
                                                    Text {
                                                        id: photoIdxTxt
                                                        anchors.centerIn: parent
                                                        text: (index + 1) + " / " + imageAttachmentRepeater.count
                                                        font.pixelSize: theme.fontSize - 4
                                                        font.bold: true
                                                        color: "#ffffff"
                                                    }
                                                }

                                                Text {
                                                    anchors.centerIn: parent
                                                    visible: !parent.mediaReady
                                                    text: "📷 " + (modelData.name || "Loading image...")
                                                    font.pixelSize: theme.fontSize - 2
                                                    color: theme.foreground
                                                }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: (mouse) => {
                                                        if (mouse.button === Qt.RightButton) {
                                                            // Same as the bubble: right-click opens the message's actions (Copy is there)
                                                            var mg = msgDelegate.modelData.guid
                                                            const reopen = root.pinnedActionMessageGuid !== mg || root.pinnedActionAttachmentGuid !== (modelData.guid || "")
                                                            root.pinnedActionMessageGuid = reopen ? mg : ""
                                                            root.pinnedActionAttachmentGuid = reopen ? (modelData.guid || "") : ""
                                                            return
                                                        }
                                                        var rawList = imageAttachmentRepeater.model || []
                                                        var resolvedList = []
                                                        for (var k = 0; k < rawList.length; ++k) {
                                                            var entry = rawList[k]
                                                            var eg = entry.guid || ""
                                                            resolvedList.push({
                                                                guid: eg,
                                                                name: entry.name || ("Photo " + (k + 1)),
                                                                mimeType: entry.mimeType || "",
                                                                localUrl: msgDelegate.resolveAttachmentUrl(eg, entry.localUrl || ""),
                                                                previewUrl: entry.previewUrl || "",
                                                                sourceUrl: entry.sourceUrl || ""
                                                            })
                                                        }
                                                        root.openPhotoViewer(resolvedList, index)
                                                    }
                                                    onDoubleClicked: {
                                                        var cGuid = backend.selectedChatGuid;
                                                        var mGuid = msgDelegate.modelData.guid;
                                                        root.clearActionMenu();
                                                        backend.sendReaction(cGuid, mGuid, "❤️");
                                                    }
                                                }
                                            }
                                        }


                                        // Inline Voice Memo Waveform Player
                                        Repeater {
                                            model: (msgDelegate.modelData.attachments && msgDelegate.modelData.attachments.length > 0)
                                                   ? msgDelegate.modelData.attachments.filter(function(a) { return a.isAudio === true; })
                                                   : []
                                            delegate: Rectangle {
                                                id: voiceMemoPlayerCard
                                                required property var modelData
                                                width: Math.min(280, bubbleContent.maxTextW + 28)
                                                height: 52
                                                radius: 10
                                                color: msgDelegate.isFromMe ? Qt.rgba(0, 0, 0, 0.22) : theme.darkerBackground
                                                border.color: msgDelegate.isFromMe ? Qt.rgba(255, 255, 255, 0.25) : theme.muted
                                                border.width: 1

                                                readonly property bool isThisPlaying: backend.playingAudioGuid === modelData.guid && backend.audioPlaying
                                                readonly property real currentProgress: backend.playingAudioGuid === modelData.guid ? backend.audioProgress : 0.0

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.margins: 8
                                                    spacing: 8

                                                    // Play / Pause Button
                                                    Rectangle {
                                                        width: 34
                                                        height: 34
                                                        radius: 17
                                                        color: voiceMemoPlayerCard.isThisPlaying ? theme.accent : (msgDelegate.isFromMe ? "#ffffff" : theme.accent)
                                                        Text {
                                                            anchors.centerIn: parent
                                                            text: voiceMemoPlayerCard.isThisPlaying ? "⏸" : "▶"
                                                            font.pixelSize: 14
                                                            color: voiceMemoPlayerCard.isThisPlaying ? "#ffffff" : (msgDelegate.isFromMe ? theme.accent : "#ffffff")
                                                        }
                                                        MouseArea {
                                                            anchors.fill: parent
                                                            cursorShape: Qt.PointingHandCursor
                                                            onClicked: {
                                                                backend.toggleAudioPlayback(modelData.guid || "", modelData.sourceUrl || modelData.localUrl || "");
                                                            }
                                                        }
                                                    }

                                                    // Waveform & Info Column
                                                    ColumnLayout {
                                                        Layout.fillWidth: true
                                                        spacing: 4

                                                        // 28 Waveform Bars with click-to-seek
                                                        Item {
                                                            Layout.fillWidth: true
                                                            height: 20

                                                            Row {
                                                                id: waveRow
                                                                anchors.fill: parent
                                                                spacing: 2

                                                                Repeater {
                                                                    model: modelData.waveformBars && modelData.waveformBars.length > 0
                                                                           ? modelData.waveformBars
                                                                           : backend.computeWaveformBars(modelData.guid || modelData.name || "audio", 28)
                                                                    delegate: Rectangle {
                                                                        required property var modelData
                                                                        required property int index
                                                                        width: Math.max(2, Math.floor((waveRow.width - (27 * 2)) / 28))
                                                                        height: Math.max(4, Math.round(20 * Number(modelData)))
                                                                        anchors.verticalCenter: parent.verticalCenter
                                                                        radius: 1
                                                                        readonly property real barProgressRatio: index / 28.0
                                                                        color: barProgressRatio <= voiceMemoPlayerCard.currentProgress
                                                                               ? (msgDelegate.isFromMe ? "#ffffff" : theme.accent)
                                                                               : (msgDelegate.isFromMe ? Qt.rgba(255, 255, 255, 0.4) : theme.muted)
                                                                    }
                                                                }
                                                            }

                                                            MouseArea {
                                                                anchors.fill: parent
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: {
                                                                    if (width > 0) {
                                                                        var ratio = Math.max(0.0, Math.min(1.0, mouseX / width));
                                                                        backend.seekAudioPlayback(modelData.guid || "", ratio);
                                                                    }
                                                                }
                                                            }
                                                        }

                                                        // Label & Duration
                                                        RowLayout {
                                                            Layout.fillWidth: true
                                                            Text {
                                                                text: "🎙 Voice Memo"
                                                                font.pixelSize: theme.fontSize - 3
                                                                font.bold: true
                                                                color: msgDelegate.isFromMe ? Qt.rgba(255, 255, 255, 0.9) : theme.brightForeground
                                                            }
                                                            Item { Layout.fillWidth: true }
                                                            Text {
                                                                text: backend.playingAudioGuid === modelData.guid
                                                                      ? backend.audioDurationText
                                                                      : (modelData.durationSeconds > 0
                                                                         ? (Math.floor(modelData.durationSeconds / 60) + ":" + (modelData.durationSeconds % 60 < 10 ? "0" : "") + (modelData.durationSeconds % 60))
                                                                         : "0:15")
                                                                font.pixelSize: theme.fontSize - 3
                                                                color: msgDelegate.isFromMe ? Qt.rgba(255, 255, 255, 0.75) : theme.darkForeground
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // Non-image file attachment pills
                                        Repeater {
                                            model: (msgDelegate.modelData.fileAttachments && msgDelegate.modelData.fileAttachments.length > 0)
                                                   ? msgDelegate.modelData.fileAttachments
                                                   : ((msgDelegate.modelData.hasFileAttachment || false)
                                                      ? [{ guid: msgDelegate.modelData.attachmentGuid || "", name: msgDelegate.modelData.attachmentName || "" }]
                                                      : [])
                                            delegate: Rectangle {
                                                required property var modelData
                                                visible: !modelData.isAudio
                                                height: visible ? 34 : 0
                                                radius: 6
                                                color: theme.darkBackground
                                                width: attFileTxt.implicitWidth + 20

                                                Text {
                                                    id: attFileTxt
                                                    anchors.centerIn: parent
                                                    text: "📎 " + (modelData.name || "Attachment")
                                                    font.pixelSize: theme.fontSize - 2
                                                    color: theme.brightForeground
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: backend.openAttachment(modelData.guid || "", modelData.name || "")
                                                    onDoubleClicked: {
                                                        var cGuid = backend.selectedChatGuid;
                                                        var mGuid = msgDelegate.modelData.guid;
                                                        root.clearActionMenu();
                                                        backend.sendReaction(cGuid, mGuid, "❤️");
                                                    }
                                                }
                                            }
                                        }

                                        // Rich Link Preview Card (YouTube & Web OpenGraph)
                                        Rectangle {
                                            id: linkPreviewCard
                                            visible: msgDelegate.liveHasPreview
                                            width: Math.min(320, bubbleContent.maxTextW + 28)
                                            height: (msgDelegate.liveLinkPreview && msgDelegate.liveLinkPreview.thumbnail && msgDelegate.liveLinkPreview.thumbnail.length > 0) ? 200 : 70
                                            radius: 8
                                            color: theme.darkBackground
                                            border.width: 1
                                            border.color: theme.muted
                                            clip: true

                                            ColumnLayout {
                                                anchors.fill: parent
                                                spacing: 0

                                                // Thumbnail area
                                                Item {
                                                    visible: (msgDelegate.liveLinkPreview && msgDelegate.liveLinkPreview.thumbnail && msgDelegate.liveLinkPreview.thumbnail.length > 0) ? true : false
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 130
                                                    clip: true

                                                    Image {
                                                        anchors.fill: parent
                                                        source: (msgDelegate.liveLinkPreview && msgDelegate.liveLinkPreview.thumbnail) ? msgDelegate.liveLinkPreview.thumbnail : ""
                                                        autoTransform: true
                                                        fillMode: Image.PreserveAspectCrop
                                                    }

                                                    // YouTube red play badge
                                                    Rectangle {
                                                        visible: (msgDelegate.liveLinkPreview && msgDelegate.liveLinkPreview.isYouTube) ? true : false
                                                        anchors.centerIn: parent
                                                        width: 44
                                                        height: 30
                                                        radius: 8
                                                        color: "#E62117"
                                                        Text {
                                                            anchors.centerIn: parent
                                                            text: "▶"
                                                            font.pixelSize: 14
                                                            color: "#ffffff"
                                                        }
                                                    }

                                                    // Domain tag / pill bottom-left
                                                    Rectangle {
                                                        anchors.left: parent.left
                                                        anchors.bottom: parent.bottom
                                                        anchors.margins: 6
                                                        height: 18
                                                        radius: 4
                                                        color: "#CC000000"
                                                        implicitWidth: domainTxt.implicitWidth + 8

                                                        Text {
                                                            id: domainTxt
                                                            anchors.centerIn: parent
                                                            text: (msgDelegate.liveLinkPreview && msgDelegate.liveLinkPreview.domain) ? msgDelegate.liveLinkPreview.domain : ""
                                                            font.pixelSize: theme.fontSize - 2
                                                            font.bold: true
                                                            color: "#ffffff"
                                                        }
                                                    }
                                                }

                                                // Title & Description area
                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    Layout.margins: 8
                                                    spacing: 2

                                                    Text {
                                                        Layout.fillWidth: true
                                                        text: (msgDelegate.liveLinkPreview && msgDelegate.liveLinkPreview.title) ? msgDelegate.liveLinkPreview.title : (msgDelegate.liveLinkPreview ? (msgDelegate.liveLinkPreview.url || "") : "")
                                                        font.pixelSize: theme.fontSize
                                                        font.bold: true
                                                        color: theme.brightForeground
                                                        elide: Text.ElideRight
                                                        maximumLineCount: 2
                                                        wrapMode: Text.Wrap
                                                    }

                                                    Text {
                                                        visible: (text || "").length > 0
                                                        Layout.fillWidth: true
                                                        text: (msgDelegate.liveLinkPreview && msgDelegate.liveLinkPreview.description) ? msgDelegate.liveLinkPreview.description : ""
                                                        font.pixelSize: theme.fontSize - 4
                                                        color: theme.darkForeground
                                                        elide: Text.ElideRight
                                                        maximumLineCount: 1
                                                    }
                                                }
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    if (msgDelegate.liveLinkPreview && msgDelegate.liveLinkPreview.url) {
                                                        backend.openUrl(msgDelegate.liveLinkPreview.url);
                                                    }
                                                }
                                                onDoubleClicked: {
                                                    var cGuid = backend.selectedChatGuid;
                                                    var mGuid = modelData.guid;
                                                    root.clearActionMenu();
                                                    backend.sendReaction(cGuid, mGuid, "❤️");
                                                }
                                            }
                                        }

                                        // Message Text Container (Handles Sizing, Selection & Invisible Ink)
                                        Item {
                                            id: textContainer
                                            visible: (modelData.text || "").length > 0 && !(modelData.hasImageAttachment && (modelData.text === "📷 Photo" || /^📷 \d+ Photos$/.test(modelData.text)))
                                            width: Math.max(Math.min(msgText.implicitWidth, bubbleContent.maxTextW), (msgDelegate.isInvisibleInkMsg && !msgDelegate.invisibleInkRevealed) ? Math.min(220, bubbleContent.maxTextW) : 0)
                                            height: Math.max(msgText.implicitHeight, (msgDelegate.isInvisibleInkMsg && !msgDelegate.invisibleInkRevealed) ? 34 : 0)

                                            TextEdit {
                                                id: msgText
                                                anchors.fill: parent
                                                opacity: (msgDelegate.isInvisibleInkMsg && !msgDelegate.invisibleInkRevealed) ? 0.0 : 1.0
                                                Behavior on opacity { NumberAnimation { duration: 200 } }
                                                text: {
                                                    var rawHtml = modelData.formattedText || root.linkifyText(modelData.text || "")
                                                    if (modelData.isFromMe && rawHtml.indexOf("#64B5F6") !== -1) {
                                                        return rawHtml.replace(/color:\s*#64B5F6;/gi, "color: #FFFFFF; font-weight: 600;")
                                                    }
                                                    return rawHtml
                                                }
                                                textFormat: TextEdit.RichText
                                                readOnly: true
                                                selectByMouse: true
                                                selectByKeyboard: true
                                                selectedTextColor: "#ffffff"
                                                selectionColor: modelData.isFromMe ? (msgDelegate.isGreenService ? "#0F4221" : "#004080") : theme.accent
                                                objectName: "messageTextEdit"
                                                font.pixelSize: msgDelegate.jumboEmojiCount === 1 ? Math.round(theme.fontSize * 3.2)
                                                                : (msgDelegate.jumboEmojiCount > 1 ? Math.round(theme.fontSize * 2.5) : theme.fontSize)
                                                color: msgDelegate.isBubbleless ? theme.brightForeground : (modelData.isFromMe ? "#ffffff" : theme.brightForeground)
                                                wrapMode: TextEdit.Wrap
                                                onLinkActivated: (link) => backend.openUrl(link)

                                                HoverHandler {
                                                    id: msgTextHover
                                                    cursorShape: msgText.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
                                                }

                                                TapHandler {
                                                    acceptedButtons: Qt.LeftButton
                                                    onTapped: (eventPoint) => {
                                                        // A left tap in the text closes an open action bar (a drag-select isn't a tap)
                                                        if (root.pinnedActionMessageGuid !== "") root.clearActionMenu()
                                                        var lnk = msgText.linkAt(eventPoint.position.x, eventPoint.position.y)
                                                        if (!lnk || lnk.length === 0) {
                                                            lnk = msgText.hoveredLink
                                                        }
                                                        if (lnk && lnk.length > 0) {
                                                            backend.openUrl(lnk)
                                                        }
                                                    }
                                                }

                                                Keys.onPressed: (event) => {
                                                    if (root.showPhotoViewer && root.handlePhotoViewerKey(event)) {
                                                        return
                                                    }
                                                    if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_C) {
                                                        if (msgText.selectedText.length > 0) {
                                                            backend.copyToClipboard(msgText.selectedText, "selection")
                                                            event.accepted = true
                                                        }
                                                    } else if (event.key === Qt.Key_PageUp) {
                                                        root.scrollMessagesPageUp()
                                                        event.accepted = true
                                                    } else if (event.key === Qt.Key_PageDown) {
                                                        root.scrollMessagesPageDown()
                                                        event.accepted = true
                                                    } else if (event.key === Qt.Key_Home && !(event.modifiers & Qt.ShiftModifier)) {
                                                        root.scrollMessagesToTop()
                                                        event.accepted = true
                                                    } else if (event.key === Qt.Key_End && !(event.modifiers & Qt.ShiftModifier)) {
                                                        root.scrollMessagesToBottom()
                                                        event.accepted = true
                                                    }
                                                }
                                            }

                                            // Invisible Ink Click-To-Reveal Overlay (Sibling to msgText inside textContainer)
                                            Rectangle {
                                                id: invisibleInkOverlay
                                                anchors.fill: parent
                                                visible: textContainer.visible && msgDelegate.isInvisibleInkMsg && !msgDelegate.invisibleInkRevealed
                                                clip: true
                                                radius: 8
                                                color: modelData.isFromMe ? (msgDelegate.isGreenService ? "#15572C" : "#0866c6") : "#252d3d"
                                                border.width: 1
                                                border.color: modelData.isFromMe ? (msgDelegate.isGreenService ? "#34D399" : "#5cb3ff") : theme.accent
                                                z: 10

                                                // Animated shimmering / twinkling dots
                                                Repeater {
                                                    model: 12
                                                    delegate: Rectangle {
                                                        required property int index
                                                        width: (index % 3 === 0) ? 4 : ((index % 3 === 1) ? 3 : 2)
                                                        height: width
                                                        radius: width / 2
                                                        color: "#ffffff"
                                                        x: ((index * 19 + 7) % Math.max(20, Math.round(invisibleInkOverlay.width - 12))) + 6
                                                        y: ((index * 13 + 5) % Math.max(12, Math.round(invisibleInkOverlay.height - 10))) + 5
                                                        opacity: 0.25 + 0.65 * Math.abs(Math.sin((twinkleTimer.tick + index * 0.7)))
                                                    }
                                                }

                                                Timer {
                                                    id: twinkleTimer
                                                    property real tick: 0
                                                    interval: 80
                                                    running: invisibleInkOverlay.visible
                                                    repeat: true
                                                    onTriggered: tick += 0.2
                                                }

                                                RowLayout {
                                                    anchors.centerIn: parent
                                                    spacing: 6
                                                    Text {
                                                        text: "🕵️ Invisible Ink · " + i18n.tr("Click to reveal")
                                                        font.pixelSize: theme.fontSize - 2
                                                        font.bold: true
                                                        color: "#ffffff"
                                                    }
                                                }

                                                MouseArea {
                                                    id: invisibleInkMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        msgDelegate.invisibleInkRevealed = true
                                                    }
                                                }
                                            }
                                        }

                                        // One-Click 2FA / Verification Code Copy Pill
                                        Rectangle {
                                            id: otpCopyPill
                                            property bool justCopied: false
                                            visible: (modelData.verificationCode || "").length > 0
                                            height: 28
                                            implicitWidth: otpCopyRow.implicitWidth + 18
                                            radius: 14
                                            color: otpCopyPill.justCopied
                                                   ? "#2ea043"
                                                   : (otpPillMouse.containsMouse
                                                      ? (modelData.isFromMe ? Qt.rgba(1, 1, 1, 0.28) : theme.selection)
                                                      : (modelData.isFromMe ? Qt.rgba(0, 0, 0, 0.24) : theme.lighterBackground))
                                            border.width: 1
                                            border.color: otpCopyPill.justCopied
                                                          ? "#3fb950"
                                                          : (modelData.isFromMe ? Qt.rgba(1, 1, 1, 0.45) : theme.accent)

                                            Timer {
                                                id: otpResetTimer
                                                interval: 2000
                                                repeat: false
                                                onTriggered: otpCopyPill.justCopied = false
                                            }

                                            RowLayout {
                                                id: otpCopyRow
                                                anchors.centerIn: parent
                                                spacing: 6
                                                Text {
                                                    text: otpCopyPill.justCopied
                                                          ? ("󰄬 " + i18n.tr("Copied") + " " + (modelData.verificationCode || ""))
                                                          : ("󰆏 " + i18n.tr("Copy Code:") + " " + (modelData.verificationCode || ""))
                                                    font.pixelSize: theme.fontSize - 2
                                                    font.bold: true
                                                    color: "#ffffff"
                                                }
                                            }

                                            MouseArea {
                                                id: otpPillMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    backend.copyToClipboard(modelData.verificationCode, "Code " + modelData.verificationCode)
                                                    otpCopyPill.justCopied = true
                                                    otpResetTimer.restart()
                                                }
                                            }
                                        }

                                        // Timestamp & Metadata (Edited / Send Effect / Delivery Error) inside bubble bottom-right
                                        Item {
                                            anchors.right: parent ? parent.right : undefined
                                            implicitWidth: metaBadgeTextItem.implicitWidth
                                            implicitHeight: metaBadgeTextItem.implicitHeight

                                            Text {
                                                id: metaBadgeTextItem
                                                anchors.fill: parent
                                                text: (modelData.metaBadgeText && modelData.metaBadgeText.length > 0)
                                                      ? modelData.metaBadgeText
                                                      : (modelData.timeText || "")
                                                font.pixelSize: theme.fontSize - 3
                                                font.bold: modelData.isFailed || false
                                                font.underline: metaBadgeMouse.containsMouse && (msgDelegate.isInvisibleInkMsg || (modelData.sendEffectLabel || "").length > 0 || (modelData.expressiveEffect || "").length > 0)
                                                color: (modelData.isFailed || false)
                                                       ? theme.red
                                                       : ((modelData.isFromMe && !msgDelegate.isBubbleless)
                                                          ? (msgDelegate.isGreenService ? "#CBEFD8" : "#d0dff7")
                                                          : theme.darkForeground)
                                            }

                                            MouseArea {
                                                id: metaBadgeMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: (msgDelegate.isInvisibleInkMsg || (modelData.sendEffectLabel || "").length > 0 || (modelData.expressiveEffect || "").length > 0)
                                                             ? Qt.PointingHandCursor : Qt.ArrowCursor
                                                onClicked: {
                                                    if (msgDelegate.isInvisibleInkMsg) {
                                                        msgDelegate.invisibleInkRevealed = !msgDelegate.invisibleInkRevealed
                                                    }
                                                    var eff = modelData.expressiveEffect || modelData.sendEffectLabel || ""
                                                    if (eff.length > 0) {
                                                        msgDelegate.playBubbleEffect(eff)
                                                        root.triggerExpressiveEffect(eff)
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            Item {
                                visible: !modelData.isFromMe
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: root.clearActionMenu()
                                }
                            }
                        }

                        // "N Replies" under a message that started a thread
                        Text {
                            objectName: "threadRepliesLink"
                            visible: (modelData.replyCount || 0) > 0
                            Layout.alignment: modelData.isFromMe ? Qt.AlignRight : Qt.AlignLeft
                            Layout.leftMargin: modelData.isFromMe ? 0 : 26
                            Layout.rightMargin: modelData.isFromMe ? 26 : 0
                            text: (modelData.replyCount || 0) === 1 ? i18n.tr("1 Reply") : ((modelData.replyCount || 0) + " " + i18n.tr("Replies"))
                            font.pixelSize: theme.fontSize - 3
                            font.bold: true
                            font.underline: repliesLinkMouse.containsMouse
                            color: theme.accent
                            MouseArea {
                                id: repliesLinkMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.openThread(modelData.guid)
                            }
                        }

                        // Tapback Reaction Pills Row
                        RowLayout {
                            id: reactionPillsRow
                            visible: (modelData.reactions || []).length > 0
                            Layout.alignment: modelData.isFromMe ? Qt.AlignRight : Qt.AlignLeft
                            Layout.leftMargin: 28
                            Layout.rightMargin: 28
                            spacing: 6
                            z: msgDelegate.hoveredReactionEmoji.length > 0 ? 50 : 2

                            Repeater {
                                id: reactionPillsRepeater
                                model: modelData.reactions || []
                                delegate: Rectangle {
                                    id: reactionPillRect
                                    objectName: "reactionPillItem"
                                    required property var modelData
                                    readonly property string senderNamesText: (modelData.senders && modelData.senders.length > 0) ? modelData.senders : "You"
                                    readonly property string pillTooltipText: (modelData.tooltipText && modelData.tooltipText.length > 0)
                                                                              ? modelData.tooltipText
                                                                              : ((modelData.emoji || "") + " " + senderNamesText)
                                    property bool forceHoveredForTest: false
                                    readonly property bool isPillHovered: forceHoveredForTest || reactionPillHover.hovered || reactionPillMouse.containsMouse
                                    readonly property string tooltipText: pillTooltipText
                                    readonly property bool tooltipVisible: reactionTooltipBadge.visible
                                    height: 24
                                    radius: 12
                                    implicitWidth: reactRow.implicitWidth + 14
                                    color: isPillHovered
                                           ? theme.lighterBackground
                                           : (modelData.fromMe ? theme.selection : theme.darkBackground)
                                    border.width: 1
                                    border.color: (isPillHovered || modelData.fromMe) ? theme.accent : theme.muted
                                    z: isPillHovered ? 100 : 1

                                    HoverHandler {
                                        id: reactionPillHover
                                        onHoveredChanged: {
                                            if (hovered) {
                                                msgDelegate.hoveredReactionEmoji = reactionPillRect.modelData.emoji || "hovered"
                                            } else if (msgDelegate.hoveredReactionEmoji === (reactionPillRect.modelData.emoji || "hovered")) {
                                                msgDelegate.hoveredReactionEmoji = ""
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: reactionPillMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        acceptedButtons: Qt.NoButton
                                        cursorShape: Qt.PointingHandCursor
                                    }

                                    RowLayout {
                                        id: reactRow
                                        anchors.centerIn: parent
                                        spacing: 4
                                        Text {
                                            text: modelData.emoji
                                            font.pixelSize: theme.fontSize - 2
                                        }
                                        Text {
                                            visible: modelData.count > 1
                                            text: modelData.count
                                            font.pixelSize: theme.fontSize - 3
                                            font.bold: true
                                            color: theme.brightForeground
                                        }
                                    }

                                    // Floating Reaction Senders Hover Tooltip Badge
                                    Rectangle {
                                        id: reactionTooltipBadge
                                        objectName: "reactionTooltipBadge"
                                        visible: reactionPillRect.isPillHovered && reactionPillRect.senderNamesText.length > 0
                                        anchors.bottom: parent.top
                                        anchors.bottomMargin: 6
                                        anchors.left: msgDelegate.modelData.isFromMe ? undefined : parent.left
                                        anchors.right: msgDelegate.modelData.isFromMe ? parent.right : undefined
                                        width: Math.min(340, reactionTooltipText.implicitWidth + 18)
                                        height: reactionTooltipText.implicitHeight + 10
                                        radius: 8
                                        color: theme.darkerBackground
                                        border.width: 1
                                        border.color: theme.accent
                                        z: 200

                                        Text {
                                            id: reactionTooltipText
                                            objectName: "reactionTooltipText"
                                            anchors.centerIn: parent
                                            width: Math.min(322, implicitWidth)
                                            text: reactionPillRect.pillTooltipText
                                            font.pixelSize: theme.fontSize - 3
                                            font.bold: true
                                            color: theme.brightForeground
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }
                            }
                        }

                        // Delivered / Read Receipt under latest outgoing message
                        Text {
                            visible: (modelData.receiptText || "").length > 0
                            Layout.alignment: Qt.AlignRight
                            Layout.rightMargin: 26
                            text: modelData.receiptText || ""
                            font.pixelSize: theme.fontSize - 2
                            color: theme.foreground
                            opacity: 0.78
                        }
                    }
                }

                // Editing banner above Composer
                Rectangle {
                    objectName: "editingBanner"
                    visible: backend.selectedChatGuid.length > 0 && root.editTargetGuid !== ""
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    color: theme.selection

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 3
                        color: theme.accent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        spacing: 8

                        Text {
                            text: "✎"
                            font.pixelSize: theme.fontSize - 1
                            color: theme.accent
                        }
                        Text {
                            Layout.fillWidth: true
                            text: i18n.tr("Editing message") + " · " + i18n.tr("Enter to save, Esc to cancel")
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: theme.accent
                            elide: Text.ElideRight
                        }
                        Text {
                            text: "✕"
                            font.pixelSize: theme.fontSize - 1
                            color: theme.foreground
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.cancelEditing()
                            }
                        }
                    }
                }

                // Active Reply Context Banner above Composer
                Rectangle {
                    visible: backend.selectedChatGuid.length > 0 && root.replyTargetGuid !== ""
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    color: theme.selection

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 3
                        color: root.selectedChatIsGreen ? "#30D158" : theme.accent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        spacing: 8

                        Text {
                            text: "↩"
                            font.pixelSize: theme.fontSize - 1
                            color: root.selectedChatIsGreen ? "#30D158" : theme.accent
                        }

                        Text {
                            text: i18n.tr("Replying to") + " " + root.replyTargetSender + ":"
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: root.selectedChatIsGreen ? "#30D158" : theme.accent
                        }
                        Text {
                            Layout.fillWidth: true
                            text: (root.replyTargetText || "").replace(/[\r\n]+/g, " ").trim()
                            font.pixelSize: theme.fontSize - 2
                            color: theme.foreground
                            maximumLineCount: 1
                            elide: Text.ElideRight
                        }
                        Text {
                            text: "✕"
                            font.pixelSize: theme.fontSize - 1
                            color: theme.foreground
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.replyTargetGuid = ""
                                    root.replyTargetSender = ""
                                    root.replyTargetText = ""
                                }
                            }
                        }
                    }

                }

                // Emoji :shortcode: Live Autocomplete Bar above Composer
                Rectangle {
                    visible: backend.selectedChatGuid.length > 0 && root.emojiAutocompleteResults.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: 42
                    color: theme.darkBackground

                    Rectangle {
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: theme.accent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 6

                        Text {
                            text: ":" + root.emojiShortcodePrefix
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: theme.accent
                        }

                        Repeater {
                            model: root.emojiAutocompleteResults
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                height: 30
                                radius: 6
                                implicitWidth: emojiAutoRow.implicitWidth + 14
                                color: index === root.selectedEmojiIndex ? theme.selection : (emojiAutoMouse.containsMouse ? theme.lighterBackground : theme.background)
                                border.width: index === root.selectedEmojiIndex ? 2 : 1
                                border.color: index === root.selectedEmojiIndex ? theme.accent : theme.muted

                                RowLayout {
                                    id: emojiAutoRow
                                    anchors.centerIn: parent
                                    spacing: 5
                                    Text {
                                        text: modelData.emoji
                                        font.pixelSize: theme.fontSize
                                    }
                                    Text {
                                        text: modelData.shortcode
                                        font.pixelSize: theme.fontSize - 3
                                        color: index === root.selectedEmojiIndex ? theme.accent : theme.foreground
                                    }
                                }

                                MouseArea {
                                    id: emojiAutoMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.selectedEmojiIndex = index
                                        root.applyEmojiAutocomplete(modelData.emoji, false)
                                    }
                                }
                            }
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: "Tab: cycle • ←/→: select • Enter: insert • Esc: close"
                            font.pixelSize: theme.fontSize - 3
                            color: theme.darkForeground
                        }
                    }
                }

                // Emoji Picker Popover above Composer
                Rectangle {
                    visible: backend.selectedChatGuid.length > 0 && root.showEmojiPicker
                    Layout.fillWidth: true
                    Layout.preferredHeight: 245
                    color: theme.darkBackground

                    Rectangle {
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: theme.accent
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            Rectangle {
                                Layout.preferredWidth: 190
                                height: Math.max(36, theme.fontSize + 16)
                                Layout.preferredHeight: Math.max(36, theme.fontSize + 16)
                                radius: 6
                                color: theme.background
                                border.width: 1
                                border.color: emojiSearchField.activeFocus ? theme.accent : theme.muted

                                TextField {
                                    id: emojiSearchField
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    topPadding: 4
                                    bottomPadding: 8
                                    verticalAlignment: TextInput.AlignVCenter
                                    placeholderText: i18n.tr("Search emojis (:smile:, heart)...")
                                    placeholderTextColor: theme.darkForeground
                                    color: theme.brightForeground
                                    font.pixelSize: theme.fontSize - 2
                                    background: Item {}
                                    onTextChanged: root.emojiPickerQuery = text

                                    Keys.onReturnPressed: function(event) {
                                        event.accepted = true
                                        var results = backend.searchEmojis(root.emojiPickerQuery, root.emojiPickerCategory, 1)
                                        if (results.length > 0) {
                                            root.insertEmojiAtCursor(results[0].emoji)
                                            root.showEmojiPicker = false
                                            composerInput.forceActiveFocus()
                                        }
                                    }

                                    Keys.onEscapePressed: function(event) {
                                        event.accepted = true
                                        root.showEmojiPicker = false
                                        composerInput.forceActiveFocus()
                                    }
                                }
                            }

                            Repeater {
                                model: ["All", "Smileys", "Gestures", "Hearts", "Food", "Activities", "Objects", "Symbols"]
                                delegate: Rectangle {
                                    required property string modelData
                                    height: 26
                                    radius: 6
                                    implicitWidth: catLabel.implicitWidth + 14
                                    color: root.emojiPickerCategory === modelData ? theme.accent : theme.lighterBackground

                                    Text {
                                        id: catLabel
                                        anchors.centerIn: parent
                                        text: modelData
                                        font.pixelSize: theme.fontSize - 3
                                        font.bold: root.emojiPickerCategory === modelData
                                        color: root.emojiPickerCategory === modelData ? theme.darkerBackground : theme.foreground
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.emojiPickerCategory = modelData
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }

                            Text {
                                text: "✕"
                                font.pixelSize: theme.fontSize
                                color: theme.darkForeground
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.showEmojiPicker = false
                                }
                            }
                        }

                        GridView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            cellWidth: 42
                            cellHeight: 38
                            clip: true
                            model: backend.searchEmojis(root.emojiPickerQuery, root.emojiPickerCategory, 120)

                            delegate: Rectangle {
                                required property var modelData
                                width: 38
                                height: 34
                                radius: 6
                                color: emojiCellMouse.containsMouse ? theme.selection : "transparent"

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.emoji
                                    font.pixelSize: theme.fontSize + 5
                                }

                                MouseArea {
                                    id: emojiCellMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.insertEmojiAtCursor(modelData.emoji)
                                }

                                ToolTip.visible: emojiCellMouse.containsMouse
                                ToolTip.text: modelData.shortcode
                                ToolTip.delay: 300
                            }
                        }
                    }
                }

                // Staged Pasted Image / Attachment Bar
                Rectangle {
                    id: stagedAttachmentBar
                    objectName: "stagedAttachmentBar"
                    // Hidden while editing: its Send must never touch the message being edited
                    visible: root.editTargetGuid === "" && backend.selectedChatGuid.length > 0 && root.stagedAttachmentPath.length > 0 && root.isStagedAttachmentForChat(backend.selectedChatGuid)
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    color: theme.darkBackground

                    Rectangle {
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: theme.lighterBackground
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Rectangle {
                            width: 46
                            height: 46
                            radius: 6
                            clip: true
                            color: theme.background
                            border.width: 1
                            border.color: theme.accent

                            Image {
                                anchors.fill: parent
                                fillMode: Image.PreserveAspectCrop
                                source: root.stagedAttachmentPath.length > 0 ? ("file://" + root.stagedAttachmentPath) : ""
                                asynchronous: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: root.stagedAttachmentName.length > 0 ? root.stagedAttachmentName : i18n.tr("Pasted Image")
                                font.pixelSize: theme.fontSize - 1
                                font.bold: true
                                color: theme.brightForeground
                                elide: Text.ElideMiddle
                            }
                            Text {
                                Layout.fillWidth: true
                                text: i18n.tr("Staged attachment · type optional caption or press Enter to send")
                                font.pixelSize: theme.fontSize - 3
                                color: theme.darkForeground
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            objectName: "stagedAttachmentCancelBtn"
                            Layout.preferredWidth: 78
                            Layout.preferredHeight: 32
                            radius: 6
                            color: stagedCancelMouse.containsMouse ? theme.lighterBackground : theme.background
                            border.width: 1
                            border.color: theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: "✕ " + i18n.tr("Cancel")
                                font.pixelSize: theme.fontSize - 2
                                color: theme.foreground
                            }
                            MouseArea {
                                id: stagedCancelMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.clearStagedAttachment()
                            }
                        }

                        Rectangle {
                            objectName: "stagedAttachmentSendBtn"
                            Layout.preferredWidth: 74
                            Layout.preferredHeight: 32
                            radius: 6
                            color: theme.accent
                            Text {
                                anchors.centerIn: parent
                                text: i18n.tr("Send") + " ↑"
                                font.pixelSize: theme.fontSize - 2
                                font.bold: true
                                color: "#ffffff"
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.sendComposerContent()
                            }
                        }
                    }
                }

                // Voice Memo Recording & Pre-Send Playback Preview Bar
                Rectangle {
                    id: voiceMemoBar
                    objectName: "voiceMemoBar"
                    visible: backend.selectedChatGuid.length > 0 && (backend.recordingVoice || backend.hasRecordedVoiceMemo) && root.isVoiceMemoForSelectedChat()
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    color: backend.recordingVoice ? Qt.rgba(0.90, 0.22, 0.27, 0.14) : theme.darkBackground

                    Rectangle {
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: backend.recordingVoice ? "#e63946" : theme.accent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        // Pulsing dot when recording, or Play/Pause button when staged for preview
                        Rectangle {
                            visible: backend.recordingVoice
                            width: 12
                            height: 12
                            radius: 6
                            color: "#e63946"
                            opacity: 1.0
                            SequentialAnimation on opacity {
                                running: backend.recordingVoice
                                loops: Animation.Infinite
                                NumberAnimation { to: 0.25; duration: 550 }
                                NumberAnimation { to: 1.0; duration: 550 }
                            }
                        }

                        Rectangle {
                            objectName: "voiceMemoPreviewPlayBtn"
                            visible: !backend.recordingVoice && backend.hasRecordedVoiceMemo
                            Layout.preferredWidth: 34
                            Layout.preferredHeight: 34
                            radius: 17
                            color: theme.accent
                            readonly property bool previewPlaying: backend.playingAudioGuid === "voice-memo-preview" && backend.audioPlaying
                            Text {
                                anchors.centerIn: parent
                                text: parent.previewPlaying ? "⏸" : "▶"
                                font.pixelSize: theme.fontSize
                                color: "#ffffff"
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.togglePreviewRecordedVoiceMemo()
                            }
                        }

                        Text {
                            text: {
                                var secs = backend.recordingVoice ? backend.voiceRecordingSeconds : backend.recordedVoiceDurationSeconds
                                var timeStr = Math.floor(secs / 60) + ":" + ("0" + (secs % 60)).slice(-2)
                                if (backend.recordingVoice) {
                                    return i18n.tr("Recording") + " " + timeStr
                                }
                                if (backend.playingAudioGuid === "voice-memo-preview") {
                                    return i18n.tr("Preview") + " " + backend.audioDurationText
                                }
                                return i18n.tr("Preview") + " " + timeStr
                            }
                            font.pixelSize: theme.fontSize - 1
                            font.bold: true
                            color: backend.recordingVoice ? "#ff6b6b" : theme.brightForeground
                        }

                        // Live / Recorded Waveform Bars
                        Item {
                            Layout.fillWidth: true
                            height: 24
                            Row {
                                id: composerWaveRow
                                anchors.fill: parent
                                spacing: 2
                                Repeater {
                                    model: backend.recordingVoice ? backend.voiceRecordingWaveform : backend.recordedVoiceWaveform
                                    delegate: Rectangle {
                                        required property var modelData
                                        required property int index
                                        width: Math.max(2, Math.floor((composerWaveRow.width - (27 * 2)) / 28))
                                        height: Math.max(4, Math.round(22 * Number(modelData)))
                                        anchors.verticalCenter: parent.verticalCenter
                                        radius: 1
                                        readonly property bool barActive: !backend.recordingVoice &&
                                                                        backend.playingAudioGuid === "voice-memo-preview" &&
                                                                        (index / 28.0) <= backend.audioProgress
                                        color: backend.recordingVoice
                                               ? "#ff6b6b"
                                               : (barActive ? theme.accent : theme.muted)
                                    }
                                }
                            }
                        }

                        // Cancel / Discard Button (active during both recording and preview)
                        Rectangle {
                            objectName: "voiceMemoCancelBtn"
                            Layout.preferredWidth: 84
                            Layout.preferredHeight: 34
                            radius: 6
                            color: vmCancelMouse.containsMouse ? theme.lighterBackground : theme.background
                            border.width: 1
                            border.color: theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: "✕ " + i18n.tr("Cancel")
                                font.pixelSize: theme.fontSize - 2
                                color: theme.foreground
                            }
                            MouseArea {
                                id: vmCancelMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    backend.cancelVoiceRecording()
                                    root.showToast("✕ " + i18n.tr("Voice memo cancelled"))
                                }
                            }
                        }

                        // Stop & Preview Button (shown while recording)
                        Rectangle {
                            objectName: "voiceMemoStopPreviewBtn"
                            visible: backend.recordingVoice
                            Layout.preferredWidth: 96
                            Layout.preferredHeight: 34
                            radius: 6
                            color: vmStopMouse.containsMouse ? theme.lighterBackground : theme.background
                            border.width: 1
                            border.color: theme.accent
                            Text {
                                anchors.centerIn: parent
                                text: "⏹ " + i18n.tr("Preview")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: true
                                color: theme.brightForeground
                            }
                            MouseArea {
                                id: vmStopMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.stopVoiceRecordingForPreview()
                            }
                        }

                        // Send Voice Memo Button (shown while recording and when staged for preview)
                        Rectangle {
                            objectName: "voiceMemoSendBtn"
                            Layout.preferredWidth: 80
                            Layout.preferredHeight: 34
                            radius: 6
                            color: theme.accent
                            Text {
                                anchors.centerIn: parent
                                text: i18n.tr("Send") + " ↑"
                                font.pixelSize: theme.fontSize - 2
                                font.bold: true
                                color: "#ffffff"
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (!root.isVoiceMemoForSelectedChat()) return
                                    if (backend.recordingVoice) {
                                        backend.stopAndSendVoiceRecording()
                                    } else if (backend.canSendRecordedVoiceMemoToSelectedChat()) {
                                        backend.sendRecordedVoiceMemo()
                                    }
                                    messageListView.scrollToBottom()
                                }
                            }
                        }
                    }

                }

                // Bottom Message Composer Bar
                Rectangle {
                    visible: backend.selectedChatGuid.length > 0
                    Layout.fillWidth: true
                    // Hugs the input: one line is as tall as the buttons, and it grows as text wraps
                    Layout.preferredHeight: Math.min(160, Math.max(64, composerBox.Layout.preferredHeight + 20))
                    color: theme.darkBackground

                    Rectangle {
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: theme.lighterBackground
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 10

                        // Emoji Picker Toggle Button (😊)
                        Rectangle {
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            Layout.alignment: Qt.AlignVCenter
                            radius: theme.radius
                            color: root.showEmojiPicker ? theme.selection : (emojiBtnMouse.containsMouse ? theme.lighterBackground : theme.background)
                            border.width: 1
                            border.color: root.showEmojiPicker ? theme.accent : theme.muted

                            Text {
                                anchors.centerIn: parent
                                text: "😊"
                                font.pixelSize: theme.fontSize + 3
                            }

                            MouseArea {
                                id: emojiBtnMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.showEmojiPicker = !root.showEmojiPicker
                                    if (root.showEmojiPicker) {
                                        emojiSearchField.forceActiveFocus()
                                    } else {
                                        composerInput.forceActiveFocus()
                                    }
                                }
                            }

                            ToolTip.visible: emojiBtnMouse.containsMouse
                            ToolTip.text: i18n.tr("Emoji Picker (or type :shortcode:)")
                            ToolTip.delay: 400
                        }

                        Rectangle {
                            id: composerBox
                            objectName: "composerBox"
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            // No empty space under a single line; up to ~6 lines before scrolling
                            Layout.preferredHeight: Math.min(140, Math.max(44, Math.ceil(composerInput.contentHeight) + composerInput.topPadding + composerInput.bottomPadding))
                            radius: theme.radius
                            color: theme.background
                            border.width: 1
                            border.color: composerInput.activeFocus ? theme.accent : theme.muted

                            TextArea {
                                id: composerInput
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                topPadding: 11
                                bottomPadding: 11
                                objectName: "composerInput"
                                leftPadding: 4
                                rightPadding: 4
                                placeholderText: i18n.tr("Type an iMessage... (:smile: for emoji, Enter to send)")
                                placeholderTextColor: theme.darkForeground
                                color: theme.brightForeground
                                font.pixelSize: theme.fontSize
                                wrapMode: TextArea.Wrap
                                background: Item {}

                                readOnly: root.showPhotoViewer
                                onTextChanged: {
                                    root.updateEmojiAutocomplete()
                                }
                                onCursorPositionChanged: root.updateEmojiAutocomplete()

                                Keys.priority: Keys.BeforeItem
                                Keys.onPressed: function(event) {
                                    if (root.showPhotoViewer && root.handlePhotoViewerKey(event)) {
                                        return
                                    }
                                    if (((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_V) ||
                                        ((event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Insert)) {
                                        var pastedPath = backend.pasteClipboardImageToTempFile()
                                        if (pastedPath && pastedPath.length > 0 && backend.selectedChatGuid.length > 0) {
                                            root.stagedAttachmentPath = pastedPath
                                            root.stagedAttachmentName = pastedPath.split("/").pop()
                                            root.stagedAttachmentChatGuid = backend.selectedChatGuid
                                            root.stageAttachment(pastedPath, root.stagedAttachmentName, backend.selectedChatGuid)
                                            root.showToast("📎 " + i18n.tr("Image staged — press Enter or Send"))
                                            event.accepted = true
                                            return
                                        }
                                    }
                                    if (event.key === Qt.Key_PageUp) {
                                        root.scrollMessagesPageUp()
                                        event.accepted = true
                                        return
                                    }
                                    if (event.key === Qt.Key_PageDown) {
                                        root.scrollMessagesPageDown()
                                        event.accepted = true
                                        return
                                    }
                                    if (event.key === Qt.Key_Home && !(event.modifiers & Qt.ShiftModifier)) {
                                        if (composerInput.text.length === 0 || (event.modifiers & Qt.ControlModifier) ||
                                            (composerInput.cursorPosition === 0 && composerInput.selectedText.length === 0)) {
                                            root.scrollMessagesToTop()
                                            event.accepted = true
                                            return
                                        }
                                    }
                                    if (event.key === Qt.Key_End && !(event.modifiers & Qt.ShiftModifier)) {
                                        if (composerInput.text.length === 0 || (event.modifiers & Qt.ControlModifier) ||
                                            (composerInput.cursorPosition === composerInput.text.length && composerInput.selectedText.length === 0)) {
                                            root.scrollMessagesToBottom()
                                            event.accepted = true
                                            return
                                        }
                                    }
                                    if (root.isEmojiCycling) {
                                        if (event.key !== Qt.Key_Tab && event.key !== Qt.Key_Left && event.key !== Qt.Key_Right &&
                                            event.key !== Qt.Key_Up && event.key !== Qt.Key_Down && event.key !== Qt.Key_Return &&
                                            event.key !== Qt.Key_Enter && event.key !== Qt.Key_Escape && event.key !== Qt.Key_Shift) {
                                            root.isEmojiCycling = false
                                            root.emojiAutocompleteResults = []
                                            root.emojiShortcodePrefix = ""
                                            root.emojiShortcodeStart = -1
                                        }
                                    }
                                }

                                Keys.onTabPressed: function(event) {
                                    if (root.isEmojiCycling) {
                                        event.accepted = true
                                        root.cycleEmoji(1)
                                    } else if (root.emojiAutocompleteResults.length > 0) {
                                        event.accepted = true
                                        root.applyEmojiAutocomplete(root.emojiAutocompleteResults[root.selectedEmojiIndex].emoji, true)
                                    } else {
                                        event.accepted = false
                                    }
                                }

                                Keys.onRightPressed: function(event) {
                                    if (root.showPhotoViewer && root.handlePhotoViewerKey(event)) {
                                        return
                                    }
                                    if (root.emojiAutocompleteResults.length > 0) {
                                        event.accepted = true
                                        if (root.isEmojiCycling) {
                                            root.cycleEmoji(1)
                                        } else {
                                            root.selectedEmojiIndex = (root.selectedEmojiIndex + 1) % root.emojiAutocompleteResults.length
                                        }
                                    } else {
                                        event.accepted = false
                                    }
                                }

                                Keys.onLeftPressed: function(event) {
                                    if (root.showPhotoViewer && root.handlePhotoViewerKey(event)) {
                                        return
                                    }
                                    if (root.emojiAutocompleteResults.length > 0) {
                                        event.accepted = true
                                        if (root.isEmojiCycling) {
                                            root.cycleEmoji(-1)
                                        } else {
                                            root.selectedEmojiIndex = (root.selectedEmojiIndex - 1 + root.emojiAutocompleteResults.length) % root.emojiAutocompleteResults.length
                                        }
                                    } else {
                                        event.accepted = false
                                    }
                                }

                                Keys.onDownPressed: function(event) {
                                    if (root.emojiAutocompleteResults.length > 0) {
                                        event.accepted = true
                                        if (root.isEmojiCycling) {
                                            root.cycleEmoji(1)
                                        } else {
                                            root.selectedEmojiIndex = (root.selectedEmojiIndex + 1) % root.emojiAutocompleteResults.length
                                        }
                                    } else {
                                        event.accepted = false
                                    }
                                }

                                Keys.onUpPressed: function(event) {
                                    if (root.emojiAutocompleteResults.length > 0) {
                                        event.accepted = true
                                        if (root.isEmojiCycling) {
                                            root.cycleEmoji(-1)
                                        } else {
                                            root.selectedEmojiIndex = (root.selectedEmojiIndex - 1 + root.emojiAutocompleteResults.length) % root.emojiAutocompleteResults.length
                                        }
                                    } else {
                                        event.accepted = false
                                    }
                                }

                                Keys.onReturnPressed: function(event) {
                                    if (event.modifiers & Qt.ShiftModifier) {
                                        event.accepted = false
                                    } else if (root.isEmojiCycling) {
                                        event.accepted = true
                                        root.isEmojiCycling = false
                                        root.emojiAutocompleteResults = []
                                        root.emojiShortcodePrefix = ""
                                        root.emojiShortcodeStart = -1
                                    } else if (root.emojiAutocompleteResults.length > 0) {
                                        event.accepted = true
                                        root.applyEmojiAutocomplete(root.emojiAutocompleteResults[root.selectedEmojiIndex].emoji, false)
                                    } else {
                                        event.accepted = true
                                        root.sendComposerContent()
                                    }
                                }
                            }
                        }


                        // Voice Memo Record/Stop Button (🎙)
                        Rectangle {
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            Layout.alignment: Qt.AlignVCenter
                            radius: theme.radius
                            color: backend.recordingVoice ? "#e63946" : (voiceBtnMouse.containsMouse ? theme.lighterBackground : theme.background)
                            border.width: 1
                            border.color: backend.recordingVoice ? "#ff4d6d" : theme.muted

                            Text {
                                anchors.centerIn: parent
                                text: backend.recordingVoice ? "⏹" : "🎙"
                                font.pixelSize: theme.fontSize + 3
                                color: backend.recordingVoice ? "#ffffff" : theme.brightForeground
                            }

                            MouseArea {
                                id: voiceBtnMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (backend.recordingVoice) {
                                        backend.stopVoiceRecordingForPreview();
                                    } else {
                                        backend.startVoiceRecording();
                                    }
                                }
                            }

                            ToolTip.visible: voiceBtnMouse.containsMouse
                            ToolTip.text: backend.recordingVoice ? i18n.tr("Stop & Preview Voice Memo") : i18n.tr("Record Voice Memo")
                            ToolTip.delay: 400
                        }

                        // Expressive Send Effect Menu Button (✨)
                        Rectangle {
                            id: effectBtn
                            objectName: "composerEffectBtn"
                            visible: root.editTargetGuid === ""
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            Layout.alignment: Qt.AlignVCenter
                            radius: theme.radius
                            color: effectMenuPopup.visible ? theme.selection : (effectBtnMouse.containsMouse ? theme.lighterBackground : theme.background)
                            border.width: 1
                            border.color: effectMenuPopup.visible ? theme.accent : theme.muted

                            Text {
                                anchors.centerIn: parent
                                text: "✨"
                                font.pixelSize: theme.fontSize + 3
                            }

                            MouseArea {
                                id: effectBtnMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    effectMenuPopup.visible = !effectMenuPopup.visible;
                                    scheduleMenuPopup.visible = false;
                                }
                            }

                            ToolTip.visible: effectBtnMouse.containsMouse
                            ToolTip.text: i18n.tr("Send with Effect")
                            ToolTip.delay: 400
                        }

                        // Send Later Schedule Button (🕒)
                        Rectangle {
                            id: scheduleBtn
                            objectName: "composerScheduleBtn"
                            visible: root.editTargetGuid === ""
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            Layout.alignment: Qt.AlignVCenter
                            radius: theme.radius
                            color: scheduleMenuPopup.visible ? theme.selection : (scheduleBtnMouse.containsMouse ? theme.lighterBackground : theme.background)
                            border.width: 1
                            border.color: scheduleMenuPopup.visible ? theme.accent : theme.muted

                            Text {
                                anchors.centerIn: parent
                                text: "🕒"
                                font.pixelSize: theme.fontSize + 3
                            }

                            MouseArea {
                                id: scheduleBtnMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    scheduleMenuPopup.visible = !scheduleMenuPopup.visible;
                                    effectMenuPopup.visible = false;
                                }
                            }

                            ToolTip.visible: scheduleBtnMouse.containsMouse
                            ToolTip.text: i18n.tr("Send Later (+15m, +1h, Tomorrow 9 AM)")
                            ToolTip.delay: 400
                        }

                        // iCloud Photos Hub Button
                        Rectangle {
                            id: icloudPhotosBtn
                            objectName: "icloudPhotosBtn"
                            visible: backend.icloudPhotosEnabled && root.width >= 760
                            Layout.preferredWidth: 38
                            Layout.preferredHeight: 38
                            Layout.alignment: Qt.AlignVCenter
                            radius: theme.radius
                            color: icloudPhotosBtnMouse.containsMouse ? theme.lighterBackground : "transparent"

                            Text {
                                anchors.centerIn: parent
                                text: "☁️"
                                font.pixelSize: theme.fontSize + 2
                            }

                            MouseArea {
                                id: icloudPhotosBtnMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    backend.fetchIcloudPhotos()
                                    icloudPhotosPopup.open()
                                }
                            }

                            ToolTip.visible: icloudPhotosBtnMouse.containsMouse
                            ToolTip.text: i18n.tr("iCloud & iMessage Photos")
                            ToolTip.delay: 400
                        }

                        // Send Button
                        Rectangle {
                            Layout.preferredWidth: 82
                            Layout.preferredHeight: 44
                            Layout.alignment: Qt.AlignVCenter
                            radius: theme.radius
                            readonly property bool canSendNow: composerInput.text.trim().length > 0 || (root.stagedAttachmentPath.length > 0 && root.isStagedAttachmentForChat(backend.selectedChatGuid)) || (backend.hasRecordedVoiceMemo && root.isVoiceMemoForSelectedChat())
                            color: canSendNow
                                   ? (root.selectedChatIsGreen ? "#1B6E38" : theme.accent)
                                   : theme.lighterBackground
                            border.width: (canSendNow && root.selectedChatIsGreen) ? 1 : 0
                            border.color: "#268C48"

                            Text {
                                anchors.centerIn: parent
                                text: root.editTargetGuid !== "" ? i18n.tr("Save") : (i18n.tr("Send") + " ↑")
                                font.pixelSize: theme.fontSize - 1
                                font.bold: true
                                color: parent.canSendNow ? "#ffffff" : theme.darkForeground
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.sendComposerContent()
                            }
                        }
                    }
                }
            }
        }
    }

    // Dedicated Note Detail & Editor Window
    Window {
        id: noteDetailWindow
        objectName: "noteDetailWindow"
        transientParent: root
        flags: Qt.Dialog
        title: (noteDetailTitleInput.text.trim().length > 0 ? noteDetailTitleInput.text.trim() : i18n.tr("Note Viewer")) + " - BluePill Notes"
        property bool isFullScreen: false
        width: isFullScreen ? Math.max(640, root.width - 24) : Math.min(680, Math.max(480, Math.round(root.width * 0.65)))
        height: isFullScreen ? Math.max(480, root.height - 24) : Math.min(520, Math.max(380, Math.round(root.height * 0.65)))
        minimumWidth: 440
        minimumHeight: 340
        color: theme.darkBackground

        property string noteId: ""
        property string noteSyncSource: "local_hub"
        property int noteRevision: 0
        property int noteMutationSeq: 0
        property int noteConflictServerRevision: 0
        property bool noteHasServerConflict: false
        property string noteConflictTitle: ""
        property string noteConflictBody: ""
        property string noteConflictFolder: "Notes"
        property bool pendingHubConflictPrompt: false
        property string loadedTitle: ""
        property string loadedFolder: "Notes"
        property string loadedBody: ""
        property bool noteSendInFlight: false
        property string targetChatGuid: ""
        property string targetChatTitle: ""

        Shortcut {
            sequence: "Escape"
            context: Qt.WindowShortcut
            onActivated: noteDetailWindow.close()
        }

        Connections {
            target: backend
            function onSendingMessageChanged() {
                if (!backend.sendingMessage) {
                    noteDetailWindow.noteSendInFlight = false
                }
            }
            function onMessagesChanged() {
                if (!backend.sendingMessage) {
                    noteDetailWindow.noteSendInFlight = false
                }
            }
            function onSelectedChatChanged() {
                if (noteDetailWindow.visible && noteDetailWindow.targetChatGuid === "" && backend.selectedChatGuid !== "") {
                    noteDetailWindow.targetChatGuid = backend.selectedChatGuid
                    noteDetailWindow.targetChatTitle = backend.selectedChatTitle
                }
            }
            function onIcloudNotesChanged() {
                var all = backend.icloudNotes
                for (var i = 0; i < all.length; ++i) {
                    var n = all[i]
                    if (!n || !n.id) continue
                    var nRev = Number(n.revision || n.baseRevision || 0)
                    var nSeq = Number(n.clientMutationSeq || 0)
                    var nConflictRev = Number(n.conflictServerRevision || 0)
                    var nHasConflict = !!n.hasServerConflict || (nConflictRev > nRev)
                    if (n.id === icloudNotesPopup.selectedNoteId) {
                        icloudNotesPopup.selectedNoteConflictServerRevision = nConflictRev
                        icloudNotesPopup.selectedNoteHasServerConflict = nHasConflict
                        icloudNotesPopup.selectedNoteConflictTitle = n.conflictServerTitle || ""
                        icloudNotesPopup.selectedNoteConflictBody = n.conflictServerBody || ""
                        icloudNotesPopup.selectedNoteConflictFolder = n.conflictServerFolder || "Notes"
                        if (nSeq > 0 && nSeq === icloudNotesPopup.selectedNoteMutationSeq && nRev > icloudNotesPopup.selectedNoteRevision) {
                            icloudNotesPopup.selectedNoteRevision = nRev
                        }
                        var popupClean = (noteTitleInput.text === icloudNotesPopup.loadedTitle &&
                                          noteBodyInput.text === icloudNotesPopup.loadedBody &&
                                          noteFolderInput.text === icloudNotesPopup.loadedFolder)
                        var nextPopupTitle = n.title || ""
                        var nextPopupFolder = n.folder || "Notes"
                        var nextPopupBody = root.cleanNoteDisplayBody(n.title, n.body || n.snippet || "", n.syncSource || "local_hub")
                        var popupFieldsChanged = (nextPopupTitle !== icloudNotesPopup.loadedTitle ||
                                                  nextPopupFolder !== icloudNotesPopup.loadedFolder ||
                                                  nextPopupBody !== icloudNotesPopup.loadedBody)
                        if (popupClean && !nHasConflict && (nSeq > icloudNotesPopup.selectedNoteMutationSeq || nRev > icloudNotesPopup.selectedNoteRevision || popupFieldsChanged)) {
                            noteTitleInput.text = nextPopupTitle
                            noteFolderInput.text = nextPopupFolder
                            noteBodyInput.text = nextPopupBody
                            icloudNotesPopup.loadedTitle = noteTitleInput.text
                            icloudNotesPopup.loadedFolder = noteFolderInput.text
                            icloudNotesPopup.loadedBody = noteBodyInput.text
                            icloudNotesPopup.selectedNoteRevision = nRev
                            icloudNotesPopup.selectedNoteMutationSeq = nSeq
                            icloudNotesPopup.pendingDetailConflictPrompt = false
                        }
                    }
                    if (n.id === noteDetailWindow.noteId) {
                        noteDetailWindow.noteConflictServerRevision = nConflictRev
                        noteDetailWindow.noteHasServerConflict = nHasConflict
                        noteDetailWindow.noteConflictTitle = n.conflictServerTitle || ""
                        noteDetailWindow.noteConflictBody = n.conflictServerBody || ""
                        noteDetailWindow.noteConflictFolder = n.conflictServerFolder || "Notes"
                        if (nSeq > 0 && nSeq === noteDetailWindow.noteMutationSeq && nRev > noteDetailWindow.noteRevision) {
                            noteDetailWindow.noteRevision = nRev
                        }
                        var detailClean = (noteDetailTitleInput.text === noteDetailWindow.loadedTitle &&
                                           noteDetailBodyInput.text === noteDetailWindow.loadedBody &&
                                           noteDetailFolderInput.text === noteDetailWindow.loadedFolder)
                        var nextDetailTitle = n.title || ""
                        var nextDetailFolder = n.folder || "Notes"
                        var nextDetailBody = root.cleanNoteDisplayBody(n.title, n.body || n.snippet || "", n.syncSource || "local_hub")
                        var detailFieldsChanged = (nextDetailTitle !== noteDetailWindow.loadedTitle ||
                                                   nextDetailFolder !== noteDetailWindow.loadedFolder ||
                                                   nextDetailBody !== noteDetailWindow.loadedBody)
                        if (detailClean && !nHasConflict && (nSeq > noteDetailWindow.noteMutationSeq || nRev > noteDetailWindow.noteRevision || detailFieldsChanged)) {
                            noteDetailTitleInput.text = nextDetailTitle
                            noteDetailFolderInput.text = nextDetailFolder
                            noteDetailBodyInput.text = nextDetailBody
                            noteDetailWindow.loadedTitle = noteDetailTitleInput.text
                            noteDetailWindow.loadedFolder = noteDetailFolderInput.text
                            noteDetailWindow.loadedBody = noteDetailBodyInput.text
                            noteDetailWindow.noteRevision = nRev
                            noteDetailWindow.noteMutationSeq = nSeq
                            noteDetailWindow.pendingHubConflictPrompt = false
                        }
                    }
                }
                if (icloudNotesPopup.visible && icloudNotesPopup.selectedNoteId === "" &&
                    noteTitleInput.text.length === 0 && noteBodyInput.text.length === 0 &&
                    icloudNotesPopup.filteredNotes.length > 0) {
                    icloudNotesPopup.selectNoteObject(icloudNotesPopup.filteredNotes[0])
                }
            }
        }

        onVisibleChanged: {
            if (visible) {
                if (noteDetailWindow.targetChatGuid === "") {
                    noteDetailWindow.targetChatGuid = backend.selectedChatGuid
                    noteDetailWindow.targetChatTitle = backend.selectedChatTitle
                }
            } else {
                noteDetailWindow.noteSendInFlight = false
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Text {
                    id: noteDetailHeadlineText
                    objectName: "noteDetailHeadlineText"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    Layout.minimumWidth: 80
                    visible: noteDetailTitleInput.text.trim().length > 0
                    text: noteDetailTitleInput.text.trim()
                    font.bold: true
                    font.pixelSize: theme.fontSize + 6
                    color: theme.brightForeground
                    maximumLineCount: 1
                    elide: Text.ElideRight
                }

                Rectangle {
                    height: 28
                    width: syncBadgeText.implicitWidth + 16
                    Layout.preferredWidth: syncBadgeText.implicitWidth + 16
                    Layout.minimumWidth: syncBadgeText.implicitWidth + 16
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignVCenter
                    radius: 14
                    color: noteDetailWindow.noteSyncSource === "apple_notes_ro"
                           ? Qt.rgba(theme.green.r, theme.green.g, theme.green.b, 0.18)
                           : Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18)
                    border.color: noteDetailWindow.noteSyncSource === "apple_notes_ro" ? theme.green : theme.accent
                    border.width: 1
                    Text {
                        id: syncBadgeText
                        anchors.centerIn: parent
                        text: noteDetailWindow.noteSyncSource === "apple_notes_ro" ? "🍏 iCloud Note (R/W)" : "📝 Local Hub Note"
                        font.pixelSize: 11
                        font.bold: true
                        color: noteDetailWindow.noteSyncSource === "apple_notes_ro" ? theme.green : theme.accent
                    }
                }

                Button {
                    id: noteDetailFullScreenBtn
                    objectName: "noteDetailFullScreenBtn"
                    Layout.preferredWidth: implicitWidth
                    Layout.minimumWidth: implicitWidth
                    Layout.alignment: Qt.AlignVCenter
                    text: noteDetailWindow.isFullScreen ? ("🗗 " + i18n.tr("Compact")) : ("⛶ " + i18n.tr("Full Screen"))
                    onClicked: noteDetailWindow.isFullScreen = !noteDetailWindow.isFullScreen
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                TextField {
                    id: noteDetailTitleInput
                    Layout.fillWidth: true
                    placeholderText: i18n.tr("Note Title...")
                    font.bold: true
                    font.pixelSize: theme.fontSize + 3
                    color: theme.brightForeground
                    background: Rectangle {
                        color: theme.background
                        radius: 6
                        border.color: noteDetailTitleInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.1)
                        border.width: 1
                    }
                }

                TextField {
                    id: noteDetailFolderInput
                    Layout.preferredWidth: 140
                    placeholderText: i18n.tr("Folder")
                    font.pixelSize: theme.fontSize
                    color: theme.foreground
                    background: Rectangle {
                        color: theme.background
                        radius: 6
                        border.color: noteDetailFolderInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.1)
                        border.width: 1
                    }
                }
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ScrollBar.vertical.policy: ScrollBar.AlwaysOn
                ScrollBar.vertical.interactive: true

                TextArea {
                    id: noteDetailBodyInput
                    placeholderText: i18n.tr("Note content...")
                    wrapMode: TextArea.Wrap
                    font.pixelSize: theme.fontSize + 3
                    leftPadding: 16
                    rightPadding: 16
                    topPadding: 14
                    bottomPadding: 14
                    color: theme.brightForeground
                    selectByMouse: true
                    background: Rectangle {
                        color: theme.background
                        radius: 8
                        border.color: noteDetailBodyInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.1)
                        border.width: 1
                    }
                }
            }

            RowLayout {
                visible: noteDetailWindow.pendingHubConflictPrompt ||
                         (noteDetailWindow.noteId !== "" &&
                          (noteDetailWindow.noteHasServerConflict || noteDetailWindow.noteConflictServerRevision > noteDetailWindow.noteRevision))
                Layout.fillWidth: true
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    text: noteDetailWindow.noteConflictServerRevision > noteDetailWindow.noteRevision
                          ? i18n.tr("Server conflict (rev %1). Choose how to resolve your draft:").arg(noteDetailWindow.noteConflictServerRevision)
                          : (noteDetailWindow.noteHasServerConflict
                             ? i18n.tr("Note was deleted on server. Choose how to resolve your draft:")
                             : i18n.tr("Updated in Notes Hub. Choose how to resolve your unsaved draft:"))
                    font.pixelSize: theme.fontSize - 2
                    color: "#f7768e"
                    elide: Text.ElideRight
                }

                Button {
                    text: i18n.tr("Reload Latest")
                    onClicked: {
                        if (noteDetailWindow.noteHasServerConflict || noteDetailWindow.noteConflictServerRevision > noteDetailWindow.noteRevision) {
                            var srvTitle = noteDetailWindow.noteConflictTitle
                            var srvFolder = noteDetailWindow.noteConflictFolder || "Notes"
                            var srvBody = noteDetailWindow.noteConflictBody
                            var srvRev = noteDetailWindow.noteConflictServerRevision
                            backend.reloadConflictedIcloudNoteServerVersion(noteDetailWindow.noteId)
                            if (srvRev <= 0) {
                                noteDetailWindow.noteId = ""
                                noteDetailWindow.noteSyncSource = "local_hub"
                                noteDetailWindow.noteRevision = 0
                                noteDetailWindow.noteMutationSeq = 0
                            } else {
                                noteDetailWindow.noteRevision = srvRev
                                var refreshedList = backend.icloudNotes
                                for (var rIdx = 0; rIdx < refreshedList.length; ++rIdx) {
                                    var rItem = refreshedList[rIdx]
                                    if (rItem && rItem.id === noteDetailWindow.noteId) {
                                        noteDetailWindow.noteMutationSeq = Number(rItem.clientMutationSeq || 0)
                                        break
                                    }
                                }
                            }
                            noteDetailTitleInput.text = srvTitle
                            noteDetailFolderInput.text = srvFolder
                            noteDetailBodyInput.text = srvBody
                            noteDetailWindow.loadedTitle = srvTitle
                            noteDetailWindow.loadedFolder = srvFolder
                            noteDetailWindow.loadedBody = srvBody
                            noteDetailWindow.noteConflictServerRevision = 0
                            noteDetailWindow.noteHasServerConflict = false
                            noteDetailWindow.pendingHubConflictPrompt = false
                            return
                        }
                        var existingList = backend.icloudNotes
                        for (var i = 0; i < existingList.length; ++i) {
                            var cur = existingList[i]
                            if (cur && cur.id === noteDetailWindow.noteId) {
                                noteDetailTitleInput.text = cur.title || ""
                                noteDetailFolderInput.text = cur.folder || "Notes"
                                noteDetailBodyInput.text = cur.body || cur.snippet || ""
                                noteDetailWindow.loadedTitle = noteDetailTitleInput.text
                                noteDetailWindow.loadedFolder = noteDetailFolderInput.text
                                noteDetailWindow.loadedBody = noteDetailBodyInput.text
                                noteDetailWindow.noteRevision = Number(cur.revision || cur.baseRevision || 0)
                                noteDetailWindow.noteMutationSeq = Number(cur.clientMutationSeq || 0)
                                noteDetailWindow.pendingHubConflictPrompt = false
                                break
                            }
                        }
                    }
                }

                Button {
                    text: i18n.tr("Save as Copy")
                    onClicked: {
                        var prevId = noteDetailWindow.noteId
                        var hadServerConflict = ((noteDetailWindow.noteHasServerConflict || noteDetailWindow.noteConflictServerRevision > noteDetailWindow.noteRevision) && prevId !== "")
                        var draftTitle = noteDetailTitleInput.text
                        var draftBody = noteDetailBodyInput.text
                        var draftFolder = noteDetailFolderInput.text
                        var copyId = backend.saveIcloudNote("", draftTitle, draftBody, draftFolder, 0)
                        var copyPersisted = (copyId !== "" && backend.isIcloudNoteDurablyPersisted(copyId))
                        noteDetailWindow.noteId = copyId
                        noteDetailWindow.noteSyncSource = "local_hub"
                        noteDetailWindow.noteRevision = 0
                        noteDetailWindow.noteConflictServerRevision = 0
                        noteDetailWindow.noteHasServerConflict = false
                        noteDetailWindow.pendingHubConflictPrompt = false
                        noteDetailTitleInput.text = draftTitle
                        noteDetailFolderInput.text = draftFolder
                        noteDetailBodyInput.text = draftBody
                        noteDetailWindow.loadedTitle = draftTitle
                        noteDetailWindow.loadedFolder = draftFolder
                        noteDetailWindow.loadedBody = draftBody
                        var updatedList = backend.icloudNotes
                        for (var j = 0; j < updatedList.length; ++j) {
                            var saved = updatedList[j]
                            if (saved && saved.id === copyId) {
                                noteDetailWindow.noteMutationSeq = Number(saved.clientMutationSeq || 0)
                                break
                            }
                        }
                        if (hadServerConflict && copyPersisted) {
                            backend.reloadConflictedIcloudNoteServerVersion(prevId)
                        } else if (hadServerConflict && !copyPersisted) {
                            root.showToast(i18n.tr("Copy could not be saved to disk yet; original conflicted draft preserved."))
                        }
                    }
                }

                Button {
                    text: i18n.tr("Overwrite")
                    onClicked: {
                        var deletedOnServer = (noteDetailWindow.noteHasServerConflict && noteDetailWindow.noteConflictServerRevision <= 0)
                        var overwriteRev = deletedOnServer ? 0 : Math.max(noteDetailWindow.noteRevision, noteDetailWindow.noteConflictServerRevision)
                        if (!deletedOnServer) {
                            var existingList = backend.icloudNotes
                            for (var i = 0; i < existingList.length; ++i) {
                                var cur = existingList[i]
                                if (cur && cur.id === noteDetailWindow.noteId) {
                                    overwriteRev = Math.max(overwriteRev, Number(cur.revision || cur.baseRevision || 0))
                                    break
                                }
                            }
                        }
                        noteDetailWindow.noteId = backend.saveIcloudNote(noteDetailWindow.noteId, noteDetailTitleInput.text, noteDetailBodyInput.text, noteDetailFolderInput.text, overwriteRev, deletedOnServer)
                        noteDetailWindow.noteSyncSource = "local_hub"
                        noteDetailWindow.noteRevision = overwriteRev
                        noteDetailWindow.noteConflictServerRevision = 0
                        noteDetailWindow.noteHasServerConflict = false
                        noteDetailWindow.pendingHubConflictPrompt = false
                        noteDetailWindow.loadedTitle = noteDetailTitleInput.text
                        noteDetailWindow.loadedFolder = noteDetailFolderInput.text
                        noteDetailWindow.loadedBody = noteDetailBodyInput.text
                        var updatedList = backend.icloudNotes
                        for (var j = 0; j < updatedList.length; ++j) {
                            var saved = updatedList[j]
                            if (saved && saved.id === noteDetailWindow.noteId) {
                                noteDetailWindow.noteMutationSeq = Number(saved.clientMutationSeq || 0)
                                break
                            }
                        }
                        if (icloudNotesPopup.selectedNoteId === noteDetailWindow.noteId) {
                            var popupCleanOnOverwrite = (noteTitleInput.text === icloudNotesPopup.loadedTitle &&
                                                         noteBodyInput.text === icloudNotesPopup.loadedBody &&
                                                         noteFolderInput.text === icloudNotesPopup.loadedFolder)
                            if (popupCleanOnOverwrite) {
                                noteTitleInput.text = noteDetailTitleInput.text
                                noteBodyInput.text = noteDetailBodyInput.text
                                noteFolderInput.text = noteDetailFolderInput.text
                                icloudNotesPopup.loadedTitle = noteDetailTitleInput.text
                                icloudNotesPopup.loadedBody = noteDetailBodyInput.text
                                icloudNotesPopup.loadedFolder = noteDetailFolderInput.text
                                icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                icloudNotesPopup.selectedNoteRevision = noteDetailWindow.noteRevision
                                icloudNotesPopup.selectedNoteConflictServerRevision = 0
                                icloudNotesPopup.selectedNoteHasServerConflict = false
                                icloudNotesPopup.selectedNoteMutationSeq = noteDetailWindow.noteMutationSeq
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Button {
                    text: "💾 " + i18n.tr("Save Note")
                    onClicked: {
                        var isCopy = false
                        var targetId = noteDetailWindow.noteId
                        if (targetId !== "" && (noteDetailWindow.noteHasServerConflict || noteDetailWindow.noteConflictServerRevision > noteDetailWindow.noteRevision)) {
                            root.showToast(i18n.tr("Server conflict paused (rev %1). Choose Reload Latest, Save as Copy, or Overwrite.").arg(noteDetailWindow.noteConflictServerRevision))
                            return
                        }
                        if (targetId !== "") {
                            var existingList = backend.icloudNotes
                            for (var i = 0; i < existingList.length; ++i) {
                                var cur = existingList[i]
                                if (cur && cur.id === targetId) {
                                    var curSeq = Number(cur.clientMutationSeq || 0)
                                    var curConflictRev = Number(cur.conflictServerRevision || 0)
                                    var curHasConflict = !!cur.hasServerConflict || (curConflictRev > noteDetailWindow.noteRevision)
                                    if (curHasConflict) {
                                        noteDetailWindow.noteConflictServerRevision = curConflictRev
                                        noteDetailWindow.noteHasServerConflict = true
                                        noteDetailWindow.noteConflictTitle = cur.conflictServerTitle || ""
                                        noteDetailWindow.noteConflictBody = cur.conflictServerBody || ""
                                        noteDetailWindow.noteConflictFolder = cur.conflictServerFolder || "Notes"
                                        root.showToast(i18n.tr("Server conflict paused (rev %1). Choose Reload Latest, Save as Copy, or Overwrite.").arg(curConflictRev))
                                        return
                                    }
                                    if (curSeq > noteDetailWindow.noteMutationSeq) {
                                        var detailCleanOnSave = (noteDetailTitleInput.text === noteDetailWindow.loadedTitle &&
                                                                 noteDetailBodyInput.text === noteDetailWindow.loadedBody &&
                                                                 noteDetailFolderInput.text === noteDetailWindow.loadedFolder)
                                        if (detailCleanOnSave) {
                                            noteDetailTitleInput.text = cur.title || ""
                                            noteDetailFolderInput.text = cur.folder || "Notes"
                                            noteDetailBodyInput.text = cur.body || cur.snippet || ""
                                            noteDetailWindow.loadedTitle = noteDetailTitleInput.text
                                            noteDetailWindow.loadedFolder = noteDetailFolderInput.text
                                            noteDetailWindow.loadedBody = noteDetailBodyInput.text
                                            noteDetailWindow.noteRevision = Number(cur.revision || cur.baseRevision || 0)
                                            noteDetailWindow.noteMutationSeq = curSeq
                                            noteDetailWindow.pendingHubConflictPrompt = false
                                            root.showToast(i18n.tr("Note was updated in Notes Hub; loaded latest saved version."))
                                        } else {
                                            noteDetailWindow.pendingHubConflictPrompt = true
                                            root.showToast(i18n.tr("Note was updated in Notes Hub; your draft is preserved. Choose Reload Latest, Save as Copy, or Overwrite."))
                                        }
                                        return
                                    }
                                    break
                                }
                            }
                        }
                        var baseRev = (isCopy || targetId === "") ? 0 : noteDetailWindow.noteRevision
                        noteDetailWindow.noteId = backend.saveIcloudNote(targetId, noteDetailTitleInput.text, noteDetailBodyInput.text, noteDetailFolderInput.text, baseRev)
                        noteDetailWindow.noteSyncSource = "local_hub"
                        noteDetailWindow.pendingHubConflictPrompt = false
                        noteDetailWindow.loadedTitle = noteDetailTitleInput.text
                        noteDetailWindow.loadedFolder = noteDetailFolderInput.text
                        noteDetailWindow.loadedBody = noteDetailBodyInput.text
                        var updatedList = backend.icloudNotes
                        for (var j = 0; j < updatedList.length; ++j) {
                            var saved = updatedList[j]
                            if (saved && saved.id === noteDetailWindow.noteId) {
                                noteDetailWindow.noteMutationSeq = Number(saved.clientMutationSeq || 0)
                                break
                            }
                        }
                        if (icloudNotesPopup.selectedNoteId === noteDetailWindow.noteId) {
                            var popupCleanOnDetailSave = (noteTitleInput.text === icloudNotesPopup.loadedTitle &&
                                                          noteBodyInput.text === icloudNotesPopup.loadedBody &&
                                                          noteFolderInput.text === icloudNotesPopup.loadedFolder)
                            if (popupCleanOnDetailSave) {
                                noteTitleInput.text = noteDetailTitleInput.text
                                noteBodyInput.text = noteDetailBodyInput.text
                                noteFolderInput.text = noteDetailFolderInput.text
                                icloudNotesPopup.loadedTitle = noteDetailTitleInput.text
                                icloudNotesPopup.loadedBody = noteDetailBodyInput.text
                                icloudNotesPopup.loadedFolder = noteDetailFolderInput.text
                                icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                icloudNotesPopup.selectedNoteRevision = noteDetailWindow.noteRevision
                                icloudNotesPopup.selectedNoteMutationSeq = noteDetailWindow.noteMutationSeq
                            } else {
                                root.showToast(i18n.tr("Note saved; unsaved draft in Notes Hub preserved."))
                            }
                        }
                    }
                }

                Button {
                    Layout.maximumWidth: 220
                    text: {
                        var recipient = noteDetailWindow.targetChatTitle.trim()
                        if (recipient.length > 22) {
                            recipient = recipient.substring(0, 21) + "…"
                        }
                        return "📤 " + (recipient.length > 0
                                        ? i18n.tr("Send to %1").arg(recipient)
                                        : i18n.tr("Send to Chat"))
                    }
                    enabled: !noteDetailWindow.noteSendInFlight && !backend.sendingMessage && noteDetailWindow.targetChatGuid !== "" && (noteDetailTitleInput.text.trim().length > 0 || noteDetailBodyInput.text.trim().length > 0)
                    onClicked: {
                        if (noteDetailWindow.noteSendInFlight || backend.sendingMessage || noteDetailWindow.targetChatGuid === "") return
                        var textToSend = noteDetailTitleInput.text.trim()
                        if (noteDetailBodyInput.text.trim().length > 0) {
                            textToSend = (textToSend.length > 0 ? ("* " + textToSend + " *\n\n") : "") + noteDetailBodyInput.text.trim()
                        }
                        noteDetailWindow.noteSendInFlight = true
                        backend.sendMessage(noteDetailWindow.targetChatGuid, textToSend, "")
                        if (!backend.sendingMessage) {
                            noteDetailWindow.noteSendInFlight = false
                        }
                    }
                }

                Button {
                    text: "📋 " + i18n.tr("Copy Text")
                    onClicked: {
                        var textToCopy = noteDetailTitleInput.text.trim()
                        if (noteDetailBodyInput.text.trim().length > 0) {
                            textToCopy = (textToCopy.length > 0 ? (textToCopy + "\n\n") : "") + noteDetailBodyInput.text.trim()
                        }
                        backend.copyToClipboard(textToCopy)
                    }
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: "🗑 " + i18n.tr("Delete")
                    enabled: noteDetailWindow.noteId !== ""
                    onClicked: {
                        if (noteDetailWindow.noteId === "") return
                        var deletedId = noteDetailWindow.noteId
                        backend.deleteIcloudNote(deletedId)
                        noteDetailWindow.noteId = ""
                        noteDetailWindow.noteSyncSource = "local_hub"
                        noteDetailWindow.noteRevision = 0
                        noteDetailWindow.noteMutationSeq = 0
                        noteDetailWindow.noteConflictServerRevision = 0
                        noteDetailWindow.noteHasServerConflict = false
                        noteDetailWindow.pendingHubConflictPrompt = false
                        noteDetailTitleInput.text = ""
                        noteDetailFolderInput.text = "Notes"
                        noteDetailBodyInput.text = ""
                        noteDetailWindow.loadedTitle = ""
                        noteDetailWindow.loadedFolder = "Notes"
                        noteDetailWindow.loadedBody = ""
                        if (icloudNotesPopup.selectedNoteId === deletedId) {
                            icloudNotesPopup.selectedNoteId = ""
                            icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                            icloudNotesPopup.selectedNoteRevision = 0
                            icloudNotesPopup.selectedNoteMutationSeq = 0
                            icloudNotesPopup.selectedNoteConflictServerRevision = 0
                            icloudNotesPopup.selectedNoteHasServerConflict = false
                            icloudNotesPopup.pendingDetailConflictPrompt = false
                            noteTitleInput.text = ""
                            noteFolderInput.text = "Notes"
                            noteBodyInput.text = ""
                            icloudNotesPopup.loadedTitle = ""
                            icloudNotesPopup.loadedFolder = "Notes"
                            icloudNotesPopup.loadedBody = ""
                        }
                        noteDetailWindow.close()
                    }
                }

                Button {
                    text: i18n.tr("Close")
                    onClicked: noteDetailWindow.close()
                }
            }
        }
    }

    // iCloud Notes Popup
    Popup {
        id: icloudNotesPopup
        objectName: "icloudNotesPopup"
        modal: true
        focus: true
        anchors.centerIn: parent
        property bool isFullScreen: false
        width: isFullScreen ? Math.max(520, root.width - 20) : Math.min(960, Math.max(580, root.width - 80))
        height: isFullScreen ? Math.max(400, root.height - 20) : Math.min(580, Math.max(400, root.height - 80))
        readonly property bool compactNotesLayout: width < 760
        property int notesPage: 0
        property int notesPageSize: 20
        readonly property var filteredNotes: {
            var q = noteSearchInput.text.trim().toLowerCase()
            var all = backend.icloudNotes || []
            if (q.length === 0) return all
            return all.filter(function(n) {
                var t = (n.title || "").toLowerCase()
                var b = (n.body || n.snippet || "").toLowerCase()
                return t.indexOf(q) !== -1 || b.indexOf(q) !== -1
            })
        }
        readonly property int notesPageCount: Math.max(1, Math.ceil(filteredNotes.length / Math.max(1, notesPageSize)))
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.darkBackground
            radius: icloudNotesPopup.isFullScreen ? 8 : (theme.radius + 4)
            border.width: 1
            border.color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.4)
        }

        property string selectedNoteId: ""
        property string selectedNoteFolder: "Notes"
        property string selectedNoteSyncSource: "local_hub"
        property int selectedNoteRevision: 0
        property int selectedNoteMutationSeq: 0
        property int selectedNoteConflictServerRevision: 0
        property bool selectedNoteHasServerConflict: false
        property string selectedNoteConflictTitle: ""
        property string selectedNoteConflictBody: ""
        property string selectedNoteConflictFolder: "Notes"
        property bool pendingDetailConflictPrompt: false
        property string loadedTitle: ""
        property string loadedFolder: "Notes"
        property string loadedBody: ""

        function selectNoteObject(n) {
            if (!n) return
            var loadedRev = Number(n.revision || n.baseRevision || 0)
            var loadedSeq = Number(n.clientMutationSeq || 0)
            var conflictRev = Number(n.conflictServerRevision || 0)
            var hasConflict = !!n.hasServerConflict || (conflictRev > loadedRev)
            icloudNotesPopup.selectedNoteId = n.id || ""
            icloudNotesPopup.selectedNoteSyncSource = n.syncSource || "local_hub"
            icloudNotesPopup.selectedNoteRevision = loadedRev
            icloudNotesPopup.selectedNoteMutationSeq = loadedSeq
            icloudNotesPopup.selectedNoteConflictServerRevision = conflictRev
            icloudNotesPopup.selectedNoteHasServerConflict = hasConflict
            icloudNotesPopup.selectedNoteConflictTitle = n.conflictServerTitle || ""
            icloudNotesPopup.selectedNoteConflictBody = n.conflictServerBody || ""
            icloudNotesPopup.selectedNoteConflictFolder = n.conflictServerFolder || "Notes"
            icloudNotesPopup.pendingDetailConflictPrompt = false
            noteTitleInput.text = n.title || ""
            noteFolderInput.text = n.folder || "Notes"
            noteBodyInput.text = root.cleanNoteDisplayBody(n.title, n.body || n.snippet || "", n.syncSource || "local_hub")
            icloudNotesPopup.loadedTitle = noteTitleInput.text
            icloudNotesPopup.loadedFolder = noteFolderInput.text
            icloudNotesPopup.loadedBody = noteBodyInput.text

            var detailHasUnsavedDraft = (noteDetailWindow.noteId !== "" ||
                                         noteDetailTitleInput.text.length > 0 ||
                                         noteDetailBodyInput.text.length > 0) &&
                                        (noteDetailTitleInput.text !== noteDetailWindow.loadedTitle ||
                                         noteDetailBodyInput.text !== noteDetailWindow.loadedBody ||
                                         noteDetailFolderInput.text !== noteDetailWindow.loadedFolder)
            if (detailHasUnsavedDraft) {
                if (noteDetailWindow.noteId === (n.id || "")) {
                    noteDetailWindow.noteConflictServerRevision = conflictRev
                    noteDetailWindow.noteHasServerConflict = hasConflict
                    noteDetailWindow.noteConflictTitle = n.conflictServerTitle || ""
                    noteDetailWindow.noteConflictBody = n.conflictServerBody || ""
                    noteDetailWindow.noteConflictFolder = n.conflictServerFolder || "Notes"
                } else {
                    noteDetailWindow.pendingHubConflictPrompt = true
                    root.showToast(i18n.tr("Unsaved draft in Note Viewer preserved."))
                }
            } else {
                noteDetailWindow.noteId = n.id || ""
                noteDetailWindow.noteSyncSource = n.syncSource || "local_hub"
                noteDetailWindow.noteRevision = loadedRev
                noteDetailWindow.noteMutationSeq = loadedSeq
                noteDetailWindow.noteConflictServerRevision = conflictRev
                noteDetailWindow.noteHasServerConflict = hasConflict
                noteDetailWindow.noteConflictTitle = n.conflictServerTitle || ""
                noteDetailWindow.noteConflictBody = n.conflictServerBody || ""
                noteDetailWindow.noteConflictFolder = n.conflictServerFolder || "Notes"
                noteDetailWindow.pendingHubConflictPrompt = false
                noteDetailWindow.targetChatGuid = backend.selectedChatGuid
                noteDetailWindow.targetChatTitle = backend.selectedChatTitle
                noteDetailTitleInput.text = n.title || ""
                noteDetailFolderInput.text = n.folder || "Notes"
                noteDetailBodyInput.text = root.cleanNoteDisplayBody(n.title, n.body || n.snippet || "", n.syncSource || "local_hub")
                noteDetailWindow.loadedTitle = noteDetailTitleInput.text
                noteDetailWindow.loadedFolder = noteDetailFolderInput.text
                noteDetailWindow.loadedBody = noteDetailBodyInput.text
            }
            if (noteDetailWindow.visible) {
                noteDetailWindow.raise()
                noteDetailWindow.requestActivate()
            }
        }

        function popOutCurrentNote() {
            var detailClean = (noteDetailTitleInput.text === noteDetailWindow.loadedTitle &&
                               noteDetailBodyInput.text === noteDetailWindow.loadedBody &&
                               noteDetailFolderInput.text === noteDetailWindow.loadedFolder);
            if (!detailClean && noteDetailWindow.noteId !== "") {
                root.showToast("⚠️ " + i18n.tr("Pop-out window has unsaved edits"))
                noteDetailWindow.show()
                noteDetailWindow.raise()
                noteDetailWindow.requestActivate()
                return
            }
            noteDetailWindow.noteId = icloudNotesPopup.selectedNoteId
            noteDetailWindow.noteSyncSource = icloudNotesPopup.selectedNoteSyncSource
            noteDetailWindow.noteRevision = icloudNotesPopup.selectedNoteRevision
            noteDetailWindow.noteMutationSeq = icloudNotesPopup.selectedNoteMutationSeq
            noteDetailWindow.noteConflictServerRevision = icloudNotesPopup.selectedNoteConflictServerRevision
            noteDetailWindow.noteHasServerConflict = icloudNotesPopup.selectedNoteHasServerConflict
            noteDetailWindow.noteConflictTitle = icloudNotesPopup.selectedNoteConflictTitle
            noteDetailWindow.noteConflictBody = icloudNotesPopup.selectedNoteConflictBody
            noteDetailWindow.noteConflictFolder = icloudNotesPopup.selectedNoteConflictFolder
            noteDetailWindow.pendingHubConflictPrompt = false
            noteDetailWindow.targetChatGuid = backend.selectedChatGuid
            noteDetailWindow.targetChatTitle = backend.selectedChatTitle
            noteDetailTitleInput.text = noteTitleInput.text
            noteDetailFolderInput.text = noteFolderInput.text
            noteDetailBodyInput.text = noteBodyInput.text
            noteDetailWindow.loadedTitle = icloudNotesPopup.loadedTitle
            noteDetailWindow.loadedFolder = icloudNotesPopup.loadedFolder
            noteDetailWindow.loadedBody = icloudNotesPopup.loadedBody
            noteDetailWindow.show()
            noteDetailWindow.raise()
            noteDetailWindow.requestActivate()
        }

        onOpened: {
            if (icloudNotesPopup.selectedNoteId === "" &&
                noteTitleInput.text.length === 0 &&
                noteBodyInput.text.length === 0 &&
                icloudNotesPopup.filteredNotes.length > 0) {
                icloudNotesPopup.selectNoteObject(icloudNotesPopup.filteredNotes[0])
            }
        }

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    text: "📝 " + i18n.tr("iCloud & Local Hub Notes")
                    font.pixelSize: theme.fontSize + 2
                    font.bold: true
                    color: theme.brightForeground
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                }
                Text {
                    visible: icloudNotesPopup.filteredNotes.length > 0
                    text: "(" + icloudNotesPopup.filteredNotes.length + ")"
                    font.pixelSize: theme.fontSize - 1
                    color: theme.darkForeground
                }
                Text {
                    visible: icloudNotesPopup.width >= 660
                    text: i18n.tr("Per page:")
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                }
                ThemedComboBox {
                    id: notesPageSizeSelector
                    objectName: "notesPageSizeSelector"
                    Layout.preferredWidth: 74
                    model: [10, 20, 50, 100]
                    currentIndex: 1
                    onActivated: {
                        icloudNotesPopup.notesPageSize = Number(model[currentIndex] || 20)
                        icloudNotesPopup.notesPage = 0
                        notesListView.positionViewAtBeginning()
                    }
                }
                Button {
                    text: icloudNotesPopup.width < 660 ? "+" : ("+ " + i18n.tr("New Note"))
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: {
                        icloudNotesPopup.selectedNoteId = ""
                        icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                        icloudNotesPopup.selectedNoteRevision = 0
                        icloudNotesPopup.selectedNoteMutationSeq = 0
                        icloudNotesPopup.selectedNoteConflictServerRevision = 0
                        icloudNotesPopup.selectedNoteHasServerConflict = false
                        icloudNotesPopup.selectedNoteConflictTitle = ""
                        icloudNotesPopup.selectedNoteConflictBody = ""
                        icloudNotesPopup.selectedNoteConflictFolder = "Notes"
                        icloudNotesPopup.pendingDetailConflictPrompt = false
                        noteTitleInput.text = ""
                        noteFolderInput.text = "Notes"
                        noteBodyInput.text = ""
                        icloudNotesPopup.loadedTitle = ""
                        icloudNotesPopup.loadedFolder = "Notes"
                        icloudNotesPopup.loadedBody = ""
                        noteTitleInput.forceActiveFocus()
                    }
                }
                Button {
                    id: notesFullScreenBtn
                    objectName: "notesFullScreenBtn"
                    leftPadding: 8
                    rightPadding: 8
                    text: icloudNotesPopup.width < 660
                          ? (icloudNotesPopup.isFullScreen ? "🗗" : "⛶")
                          : (icloudNotesPopup.isFullScreen ? ("🗗 " + i18n.tr("Compact")) : ("⛶ " + i18n.tr("Full Screen")))
                    onClicked: icloudNotesPopup.isFullScreen = !icloudNotesPopup.isFullScreen
                }
                Button {
                    text: "✕"
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: icloudNotesPopup.close()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 14

                ColumnLayout {
                    Layout.preferredWidth: Math.min(280, Math.max(200, Math.round(icloudNotesPopup.width * 0.34)))
                    Layout.minimumWidth: 200
                    Layout.maximumWidth: 300
                    Layout.fillHeight: true
                    spacing: 8

                    TextField {
                        id: noteSearchInput
                        objectName: "noteSearchInput"
                        Layout.fillWidth: true
                        placeholderText: "🔍 " + i18n.tr("Search notes...")
                        placeholderTextColor: theme.darkForeground
                        font.pixelSize: theme.fontSize
                        color: theme.brightForeground
                        leftPadding: 10
                        rightPadding: 10
                        topPadding: 7
                        bottomPadding: 7
                        background: Rectangle {
                            color: theme.background
                            radius: 6
                            border.color: noteSearchInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.14)
                            border.width: 1
                        }
                        onTextChanged: icloudNotesPopup.notesPage = 0
                    }

                    ListView {
                        id: notesListView
                        objectName: "notesListView"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {
                            id: notesListScrollBar
                            objectName: "notesListScrollBar"
                            policy: ScrollBar.AlwaysOn
                            interactive: true
                            width: 10
                        }
                        model: {
                            var all = icloudNotesPopup.filteredNotes
                            var maxPage = Math.max(0, icloudNotesPopup.notesPageCount - 1)
                            var pg = Math.min(Math.max(0, icloudNotesPopup.notesPage), maxPage)
                            var startIdx = pg * icloudNotesPopup.notesPageSize
                            return all.slice(startIdx, startIdx + icloudNotesPopup.notesPageSize)
                        }
                        spacing: 6

                        delegate: Rectangle {
                            width: Math.max(140, notesListView.width - 16)
                            height: 72
                            radius: 8
                            clip: true
                            color: icloudNotesPopup.selectedNoteId === (modelData.id || "")
                                   ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.24)
                                   : (itemMouse.containsMouse ? theme.lighterBackground : theme.background)
                            border.color: icloudNotesPopup.selectedNoteId === (modelData.id || "") ? theme.accent : Qt.rgba(1, 1, 1, 0.08)
                            border.width: 1

                            ColumnLayout {
                                id: noteCardCol
                                anchors.fill: parent
                                anchors.margins: 9
                                spacing: 3

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Text {
                                        text: modelData.syncSource === "apple_notes_ro" ? "🍏" : "📝"
                                        font.pixelSize: theme.fontSize - 2
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.title || i18n.tr("Untitled Note")
                                        font.bold: true
                                        font.pixelSize: theme.fontSize
                                        color: theme.brightForeground
                                        elide: Text.ElideRight
                                        maximumLineCount: 1
                                    }
                                    Rectangle {
                                        visible: !!(modelData.folder && modelData.folder.length > 0)
                                        height: 16
                                        width: Math.min(72, folderTagTxt.implicitWidth + 10)
                                        radius: 4
                                        color: Qt.rgba(1, 1, 1, 0.06)
                                        Text {
                                            id: folderTagTxt
                                            anchors.centerIn: parent
                                            width: parent.width - 6
                                            horizontalAlignment: Text.AlignHCenter
                                            text: modelData.folder || "Notes"
                                            font.pixelSize: 10
                                            color: theme.darkForeground
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    text: {
                                        var cleaned = root.cleanNoteDisplayBody(modelData.title, modelData.body || modelData.snippet || "", modelData.syncSource || "local_hub").replace(/\r?\n+/g, " · ")
                                        return cleaned.length > 0 ? cleaned : i18n.tr("No additional text")
                                    }
                                    font.pixelSize: theme.fontSize - 2
                                    color: theme.darkForeground
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                }
                            }

                            MouseArea {
                                id: itemMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: icloudNotesPopup.selectNoteObject(modelData)
                                onDoubleClicked: icloudNotesPopup.popOutCurrentNote()
                            }
                        }
                    }
                }

                Rectangle {
                    id: noteSideDetailPane
                    objectName: "noteSideDetailPane"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.fillHeight: true
                    color: theme.background
                    radius: 8
                    border.color: Qt.rgba(1, 1, 1, 0.1)
                    border.width: 1
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            Text {
                                id: notePopupHeadlineText
                                objectName: "notePopupHeadlineText"
                                Layout.fillWidth: true
                                Layout.preferredWidth: 0
                                Layout.minimumWidth: 40
                                text: noteTitleInput.text.trim().length > 0 ? noteTitleInput.text.trim() : i18n.tr("Untitled Note")
                                font.bold: true
                                font.pixelSize: theme.fontSize + 2
                                color: theme.brightForeground
                                maximumLineCount: 1
                                elide: Text.ElideRight
                            }

                            Rectangle {
                                height: 24
                                width: sideSyncBadgeTxt.implicitWidth + 12
                                Layout.preferredWidth: sideSyncBadgeTxt.implicitWidth + 12
                                Layout.minimumWidth: sideSyncBadgeTxt.implicitWidth + 12
                                Layout.preferredHeight: 24
                                Layout.alignment: Qt.AlignVCenter
                                radius: 12
                                color: icloudNotesPopup.selectedNoteSyncSource === "apple_notes_ro"
                                       ? Qt.rgba(theme.green.r, theme.green.g, theme.green.b, 0.18)
                                       : Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18)
                                border.color: icloudNotesPopup.selectedNoteSyncSource === "apple_notes_ro" ? theme.green : theme.accent
                                border.width: 1
                                Text {
                                    id: sideSyncBadgeTxt
                                    anchors.centerIn: parent
                                    text: icloudNotesPopup.selectedNoteSyncSource === "apple_notes_ro" ? "🍏 iCloud (R/W)" : "📝 Local Hub"
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: icloudNotesPopup.selectedNoteSyncSource === "apple_notes_ro" ? theme.green : theme.accent
                                }
                            }

                            Button {
                                id: notePopOutBtn
                                objectName: "notePopOutBtn"
                                Layout.alignment: Qt.AlignVCenter
                                implicitHeight: 30
                                topPadding: 4
                                bottomPadding: 4
                                leftPadding: 10
                                rightPadding: 10
                                font.pixelSize: theme.fontSize - 2
                                text: "⤢ " + i18n.tr("Pop Out")
                                enabled: icloudNotesPopup.selectedNoteId !== "" || noteTitleInput.text.trim().length > 0 || noteBodyInput.text.trim().length > 0
                                onClicked: icloudNotesPopup.popOutCurrentNote()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            TextField {
                                id: noteTitleInput
                                objectName: "noteTitleInput"
                                Layout.fillWidth: true
                                placeholderText: i18n.tr("Note Title")
                                placeholderTextColor: theme.darkForeground
                                font.bold: true
                                font.pixelSize: theme.fontSize + 1
                                color: theme.brightForeground
                                leftPadding: 10
                                rightPadding: 10
                                topPadding: 7
                                bottomPadding: 7
                                background: Rectangle {
                                    color: theme.darkBackground
                                    radius: 6
                                    border.color: noteTitleInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.14)
                                    border.width: 1
                                }
                            }
                            TextField {
                                id: noteFolderInput
                                objectName: "noteFolderInput"
                                Layout.preferredWidth: 120
                                placeholderText: i18n.tr("Folder")
                                placeholderTextColor: theme.darkForeground
                                text: "Notes"
                                font.pixelSize: theme.fontSize
                                color: theme.foreground
                                leftPadding: 10
                                rightPadding: 10
                                topPadding: 7
                                bottomPadding: 7
                                background: Rectangle {
                                    color: theme.darkBackground
                                    radius: 6
                                    border.color: noteFolderInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.14)
                                    border.width: 1
                                }
                            }
                        }

                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            ScrollBar.vertical.policy: ScrollBar.AlwaysOn
                            ScrollBar.vertical.interactive: true
                            TextArea {
                                id: noteBodyInput
                                objectName: "noteBodyInput"
                                placeholderText: i18n.tr("Write note content here...")
                                placeholderTextColor: theme.darkForeground
                                readOnly: false
                                color: theme.brightForeground
                                wrapMode: TextArea.Wrap
                                selectByMouse: true
                                font.pixelSize: theme.fontSize + 1
                                leftPadding: 14
                                rightPadding: 14
                                topPadding: 12
                                bottomPadding: 12
                                background: Rectangle {
                                    color: theme.darkBackground
                                    radius: 8
                                    border.color: noteBodyInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.12)
                                    border.width: 1
                                }
                            }
                        }

                        RowLayout {
                            visible: icloudNotesPopup.pendingDetailConflictPrompt ||
                                     (icloudNotesPopup.selectedNoteId !== "" &&
                                      (icloudNotesPopup.selectedNoteHasServerConflict ||
                                       icloudNotesPopup.selectedNoteConflictServerRevision > icloudNotesPopup.selectedNoteRevision))
                            Layout.fillWidth: true
                            spacing: 6

                            Text {
                                Layout.fillWidth: true
                                text: icloudNotesPopup.selectedNoteConflictServerRevision > icloudNotesPopup.selectedNoteRevision
                                      ? i18n.tr("Server conflict (rev %1). Resolve draft:").arg(icloudNotesPopup.selectedNoteConflictServerRevision)
                                      : (icloudNotesPopup.selectedNoteHasServerConflict
                                         ? i18n.tr("Note deleted on server. Resolve draft:")
                                         : i18n.tr("Updated in detail window. Resolve draft:"))
                                font.pixelSize: theme.fontSize - 2
                                color: "#f7768e"
                                elide: Text.ElideRight
                            }

                            Button {
                                text: i18n.tr("Reload Server")
                                onClicked: {
                                    if (icloudNotesPopup.selectedNoteHasServerConflict ||
                                        icloudNotesPopup.selectedNoteConflictServerRevision > icloudNotesPopup.selectedNoteRevision) {
                                        var srvTitle = icloudNotesPopup.selectedNoteConflictTitle
                                        var srvFolder = icloudNotesPopup.selectedNoteConflictFolder || "Notes"
                                        var srvBody = icloudNotesPopup.selectedNoteConflictBody
                                        var srvRev = icloudNotesPopup.selectedNoteConflictServerRevision
                                        backend.reloadConflictedIcloudNoteServerVersion(icloudNotesPopup.selectedNoteId)
                                        if (srvRev <= 0) {
                                            icloudNotesPopup.selectedNoteId = ""
                                            icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                            icloudNotesPopup.selectedNoteRevision = 0
                                            icloudNotesPopup.selectedNoteMutationSeq = 0
                                        } else {
                                            icloudNotesPopup.selectedNoteRevision = srvRev
                                            var refreshedPopupList = backend.icloudNotes
                                            for (var rpIdx = 0; rpIdx < refreshedPopupList.length; ++rpIdx) {
                                                var rpItem = refreshedPopupList[rpIdx]
                                                if (rpItem && rpItem.id === icloudNotesPopup.selectedNoteId) {
                                                    icloudNotesPopup.selectedNoteMutationSeq = Number(rpItem.clientMutationSeq || 0)
                                                    break
                                                }
                                            }
                                        }
                                        noteTitleInput.text = srvTitle
                                        noteFolderInput.text = srvFolder
                                        noteBodyInput.text = srvBody
                                        icloudNotesPopup.loadedTitle = srvTitle
                                        icloudNotesPopup.loadedFolder = srvFolder
                                        icloudNotesPopup.loadedBody = srvBody
                                        icloudNotesPopup.selectedNoteConflictServerRevision = 0
                                        icloudNotesPopup.selectedNoteHasServerConflict = false
                                        icloudNotesPopup.pendingDetailConflictPrompt = false
                                        return
                                    }
                                    var existingList = backend.icloudNotes
                                    for (var i = 0; i < existingList.length; ++i) {
                                        var cur = existingList[i]
                                        if (cur && cur.id === icloudNotesPopup.selectedNoteId) {
                                            noteTitleInput.text = cur.title || ""
                                            noteFolderInput.text = cur.folder || "Notes"
                                            noteBodyInput.text = cur.body || cur.snippet || ""
                                            icloudNotesPopup.loadedTitle = noteTitleInput.text
                                            icloudNotesPopup.loadedFolder = noteFolderInput.text
                                            icloudNotesPopup.loadedBody = noteBodyInput.text
                                            icloudNotesPopup.selectedNoteRevision = Number(cur.revision || cur.baseRevision || 0)
                                            icloudNotesPopup.selectedNoteMutationSeq = Number(cur.clientMutationSeq || 0)
                                            icloudNotesPopup.pendingDetailConflictPrompt = false
                                            break
                                        }
                                    }
                                }
                            }

                            Button {
                                text: i18n.tr("Save as Copy")
                                onClicked: {
                                    var prevId = icloudNotesPopup.selectedNoteId
                                    var hadServerConflict = ((icloudNotesPopup.selectedNoteHasServerConflict ||
                                                              icloudNotesPopup.selectedNoteConflictServerRevision > icloudNotesPopup.selectedNoteRevision) &&
                                                             prevId !== "")
                                    var draftTitle = noteTitleInput.text
                                    var draftBody = noteBodyInput.text
                                    var draftFolder = noteFolderInput.text
                                    var copyId = backend.saveIcloudNote("", draftTitle, draftBody, draftFolder, 0)
                                    var copyPersisted = (copyId !== "" && backend.isIcloudNoteDurablyPersisted(copyId))
                                    icloudNotesPopup.selectedNoteId = copyId
                                    icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                    icloudNotesPopup.selectedNoteRevision = 0
                                    icloudNotesPopup.selectedNoteConflictServerRevision = 0
                                    icloudNotesPopup.selectedNoteHasServerConflict = false
                                    icloudNotesPopup.pendingDetailConflictPrompt = false
                                    noteTitleInput.text = draftTitle
                                    noteFolderInput.text = draftFolder
                                    noteBodyInput.text = draftBody
                                    icloudNotesPopup.loadedTitle = draftTitle
                                    icloudNotesPopup.loadedFolder = draftFolder
                                    icloudNotesPopup.loadedBody = draftBody
                                    var updatedList = backend.icloudNotes
                                    for (var i = 0; i < updatedList.length; ++i) {
                                        var saved = updatedList[i]
                                        if (saved && saved.id === copyId) {
                                            icloudNotesPopup.selectedNoteMutationSeq = Number(saved.clientMutationSeq || 0)
                                            break
                                        }
                                    }
                                    if (hadServerConflict && copyPersisted) {
                                        backend.reloadConflictedIcloudNoteServerVersion(prevId)
                                    } else if (hadServerConflict && !copyPersisted) {
                                        root.showToast(i18n.tr("Copy could not be saved to disk yet; original conflicted draft preserved."))
                                    }
                                }
                            }

                            Button {
                                text: i18n.tr("Overwrite Server")
                                onClicked: {
                                    var deletedOnServer = (icloudNotesPopup.selectedNoteHasServerConflict && icloudNotesPopup.selectedNoteConflictServerRevision <= 0)
                                    var overwriteRev = deletedOnServer ? 0 : Math.max(icloudNotesPopup.selectedNoteRevision, icloudNotesPopup.selectedNoteConflictServerRevision)
                                    if (!deletedOnServer) {
                                        var existingList = backend.icloudNotes
                                        for (var k = 0; k < existingList.length; ++k) {
                                            var cur = existingList[k]
                                            if (cur && cur.id === icloudNotesPopup.selectedNoteId) {
                                                overwriteRev = Math.max(overwriteRev, Number(cur.revision || cur.baseRevision || 0))
                                                break
                                            }
                                        }
                                    }
                                    icloudNotesPopup.selectedNoteId = backend.saveIcloudNote(icloudNotesPopup.selectedNoteId, noteTitleInput.text, noteBodyInput.text, noteFolderInput.text, overwriteRev, deletedOnServer)
                                    icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                    icloudNotesPopup.selectedNoteRevision = overwriteRev
                                    icloudNotesPopup.selectedNoteConflictServerRevision = 0
                                    icloudNotesPopup.selectedNoteHasServerConflict = false
                                    icloudNotesPopup.pendingDetailConflictPrompt = false
                                    icloudNotesPopup.loadedTitle = noteTitleInput.text
                                    icloudNotesPopup.loadedFolder = noteFolderInput.text
                                    icloudNotesPopup.loadedBody = noteBodyInput.text
                                    var updatedList = backend.icloudNotes
                                    for (var i = 0; i < updatedList.length; ++i) {
                                        var saved = updatedList[i]
                                        if (saved && saved.id === icloudNotesPopup.selectedNoteId) {
                                            icloudNotesPopup.selectedNoteMutationSeq = Number(saved.clientMutationSeq || 0)
                                            break
                                        }
                                    }
                                    if (noteDetailWindow.noteId === icloudNotesPopup.selectedNoteId) {
                                        var detailCleanOnPopupOverwrite = (noteDetailTitleInput.text === noteDetailWindow.loadedTitle &&
                                                                           noteDetailBodyInput.text === noteDetailWindow.loadedBody &&
                                                                           noteDetailFolderInput.text === noteDetailWindow.loadedFolder)
                                        if (detailCleanOnPopupOverwrite) {
                                            noteDetailTitleInput.text = noteTitleInput.text
                                            noteDetailBodyInput.text = noteBodyInput.text
                                            noteDetailFolderInput.text = noteFolderInput.text
                                            noteDetailWindow.loadedTitle = noteTitleInput.text
                                            noteDetailWindow.loadedBody = noteBodyInput.text
                                            noteDetailWindow.loadedFolder = noteFolderInput.text
                                            noteDetailWindow.noteSyncSource = "local_hub"
                                            noteDetailWindow.noteRevision = icloudNotesPopup.selectedNoteRevision
                                            noteDetailWindow.noteConflictServerRevision = 0
                                            noteDetailWindow.noteHasServerConflict = false
                                            noteDetailWindow.noteMutationSeq = icloudNotesPopup.selectedNoteMutationSeq
                                            noteDetailWindow.pendingHubConflictPrompt = false
                                        }
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            Button {
                                implicitHeight: 30
                                topPadding: 4
                                bottomPadding: 4
                                leftPadding: 8
                                rightPadding: 8
                                font.pixelSize: theme.fontSize - 2
                                text: "💾 " + i18n.tr("Save")
                                onClicked: {
                                    var isCopy = false
                                    var targetId = icloudNotesPopup.selectedNoteId
                                    if (targetId !== "" && (icloudNotesPopup.selectedNoteHasServerConflict ||
                                                            icloudNotesPopup.selectedNoteConflictServerRevision > icloudNotesPopup.selectedNoteRevision)) {
                                        root.showToast(i18n.tr("Server conflict paused (rev %1). Choose Reload Server, Save as Copy, or Overwrite Server.").arg(icloudNotesPopup.selectedNoteConflictServerRevision))
                                        return
                                    }
                                    if (targetId !== "") {
                                        var preList = backend.icloudNotes
                                        for (var k = 0; k < preList.length; ++k) {
                                            var curN = preList[k]
                                            if (curN && curN.id === targetId) {
                                                var curConflictRev = Number(curN.conflictServerRevision || 0)
                                                var curHasConflict = !!curN.hasServerConflict || (curConflictRev > icloudNotesPopup.selectedNoteRevision)
                                                if (curHasConflict) {
                                                    icloudNotesPopup.selectedNoteConflictServerRevision = curConflictRev
                                                    icloudNotesPopup.selectedNoteHasServerConflict = true
                                                    icloudNotesPopup.selectedNoteConflictTitle = curN.conflictServerTitle || ""
                                                    icloudNotesPopup.selectedNoteConflictBody = curN.conflictServerBody || ""
                                                    icloudNotesPopup.selectedNoteConflictFolder = curN.conflictServerFolder || "Notes"
                                                    root.showToast(i18n.tr("Server conflict paused (rev %1). Choose Reload Server, Save as Copy, or Overwrite Server.").arg(curConflictRev))
                                                    return
                                                }
                                                var curSeq = Number(curN.clientMutationSeq || 0)
                                                if (curSeq > icloudNotesPopup.selectedNoteMutationSeq) {
                                                    var popupCleanOnSave = (noteTitleInput.text === icloudNotesPopup.loadedTitle &&
                                                                            noteBodyInput.text === icloudNotesPopup.loadedBody &&
                                                                            noteFolderInput.text === icloudNotesPopup.loadedFolder)
                                                    if (popupCleanOnSave) {
                                                        noteTitleInput.text = curN.title || ""
                                                        noteFolderInput.text = curN.folder || "Notes"
                                                        noteBodyInput.text = curN.body || curN.snippet || ""
                                                        icloudNotesPopup.loadedTitle = noteTitleInput.text
                                                        icloudNotesPopup.loadedFolder = noteFolderInput.text
                                                        icloudNotesPopup.loadedBody = noteBodyInput.text
                                                        icloudNotesPopup.selectedNoteRevision = Number(curN.revision || curN.baseRevision || 0)
                                                        icloudNotesPopup.selectedNoteMutationSeq = curSeq
                                                        icloudNotesPopup.pendingDetailConflictPrompt = false
                                                        root.showToast(i18n.tr("Note was updated in the detail window; loaded latest saved version."))
                                                    } else {
                                                        icloudNotesPopup.pendingDetailConflictPrompt = true
                                                        root.showToast(i18n.tr("Note was updated in the detail window; your draft is preserved. Choose Reload Server, Save as Copy, or Overwrite Server."))
                                                    }
                                                    return
                                                }
                                                break
                                            }
                                        }
                                    }
                                    var baseRev = (isCopy || targetId === "") ? 0 : icloudNotesPopup.selectedNoteRevision
                                    icloudNotesPopup.selectedNoteId = backend.saveIcloudNote(targetId, noteTitleInput.text, noteBodyInput.text, noteFolderInput.text, baseRev)
                                    icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                    icloudNotesPopup.pendingDetailConflictPrompt = false
                                    icloudNotesPopup.loadedTitle = noteTitleInput.text
                                    icloudNotesPopup.loadedFolder = noteFolderInput.text
                                    icloudNotesPopup.loadedBody = noteBodyInput.text
                                    var updatedList = backend.icloudNotes
                                    for (var i = 0; i < updatedList.length; ++i) {
                                        var saved = updatedList[i]
                                        if (saved && saved.id === icloudNotesPopup.selectedNoteId) {
                                            icloudNotesPopup.selectedNoteMutationSeq = Number(saved.clientMutationSeq || 0)
                                            break
                                        }
                                    }
                                    if (noteDetailWindow.noteId === icloudNotesPopup.selectedNoteId) {
                                        var detailCleanOnPopupSave = (noteDetailTitleInput.text === noteDetailWindow.loadedTitle &&
                                                                      noteDetailBodyInput.text === noteDetailWindow.loadedBody &&
                                                                      noteDetailFolderInput.text === noteDetailWindow.loadedFolder)
                                        if (detailCleanOnPopupSave) {
                                            noteDetailTitleInput.text = noteTitleInput.text
                                            noteDetailBodyInput.text = noteBodyInput.text
                                            noteDetailFolderInput.text = noteFolderInput.text
                                            noteDetailWindow.loadedTitle = noteTitleInput.text
                                            noteDetailWindow.loadedBody = noteBodyInput.text
                                            noteDetailWindow.loadedFolder = noteFolderInput.text
                                            noteDetailWindow.noteSyncSource = "local_hub"
                                            noteDetailWindow.noteRevision = icloudNotesPopup.selectedNoteRevision
                                            noteDetailWindow.noteMutationSeq = icloudNotesPopup.selectedNoteMutationSeq
                                            noteDetailWindow.pendingHubConflictPrompt = false
                                        } else {
                                            root.showToast(i18n.tr("Note saved in Notes Hub; unsaved draft in detail window preserved."))
                                        }
                                    }
                                }
                            }

                            Button {
                                implicitHeight: 30
                                topPadding: 4
                                bottomPadding: 4
                                leftPadding: 8
                                rightPadding: 8
                                font.pixelSize: theme.fontSize - 2
                                text: noteSideDetailPane.width < 460 ? ("📤 " + i18n.tr("Send")) : ("📤 " + i18n.tr("Send to Chat"))
                                enabled: backend.selectedChatGuid !== "" && (noteTitleInput.text.trim().length > 0 || noteBodyInput.text.trim().length > 0)
                                onClicked: {
                                    var textToSend = noteTitleInput.text.trim()
                                    if (noteBodyInput.text.trim().length > 0) {
                                        textToSend = (textToSend.length > 0 ? ("* " + textToSend + " *\n\n") : "") + noteBodyInput.text.trim()
                                    }
                                    backend.sendMessage(backend.selectedChatGuid, textToSend, "")
                                    icloudNotesPopup.close()
                                }
                            }

                            Button {
                                implicitHeight: 30
                                topPadding: 4
                                bottomPadding: 4
                                leftPadding: 8
                                rightPadding: 8
                                font.pixelSize: theme.fontSize - 2
                                text: "📋 " + i18n.tr("Copy")
                                enabled: noteTitleInput.text.trim().length > 0 || noteBodyInput.text.trim().length > 0
                                onClicked: {
                                    var textToCopy = noteTitleInput.text.trim()
                                    if (noteBodyInput.text.trim().length > 0) {
                                        textToCopy = (textToCopy.length > 0 ? (textToCopy + "\n\n") : "") + noteBodyInput.text.trim()
                                    }
                                    backend.copyToClipboard(textToCopy)
                                }
                            }

                            Item { Layout.fillWidth: true }

                            Button {
                                id: noteDeleteBtn
                                objectName: "noteDeleteBtn"
                                implicitHeight: 30
                                topPadding: 4
                                bottomPadding: 4
                                leftPadding: 8
                                rightPadding: 8
                                font.pixelSize: theme.fontSize - 2
                                text: "🗑 " + i18n.tr("Delete")
                                enabled: icloudNotesPopup.selectedNoteId !== ""
                                onClicked: {
                                    if (icloudNotesPopup.selectedNoteId === "") return
                                    var deletedPopupId = icloudNotesPopup.selectedNoteId
                                    backend.deleteIcloudNote(deletedPopupId)
                                    icloudNotesPopup.selectedNoteId = ""
                                    icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                    icloudNotesPopup.selectedNoteRevision = 0
                                    icloudNotesPopup.selectedNoteMutationSeq = 0
                                    icloudNotesPopup.selectedNoteConflictServerRevision = 0
                                    icloudNotesPopup.selectedNoteHasServerConflict = false
                                    icloudNotesPopup.pendingDetailConflictPrompt = false
                                    noteTitleInput.text = ""
                                    noteFolderInput.text = "Notes"
                                    noteBodyInput.text = ""
                                    icloudNotesPopup.loadedTitle = ""
                                    icloudNotesPopup.loadedFolder = "Notes"
                                    icloudNotesPopup.loadedBody = ""
                                    if (noteDetailWindow.noteId === deletedPopupId) {
                                        noteDetailWindow.noteId = ""
                                        noteDetailWindow.noteSyncSource = "local_hub"
                                        noteDetailWindow.noteRevision = 0
                                        noteDetailWindow.noteMutationSeq = 0
                                        noteDetailWindow.noteConflictServerRevision = 0
                                        noteDetailWindow.noteHasServerConflict = false
                                        noteDetailWindow.pendingHubConflictPrompt = false
                                        noteDetailTitleInput.text = ""
                                        noteDetailFolderInput.text = "Notes"
                                        noteDetailBodyInput.text = ""
                                        noteDetailWindow.loadedTitle = ""
                                        noteDetailWindow.loadedFolder = "Notes"
                                        noteDetailWindow.loadedBody = ""
                                        noteDetailWindow.close()
                                    }
                                }
                            }
                        }
                    }

                }
            }

            RowLayout {
                id: notesPaginationBar
                objectName: "notesPaginationBar"
                Layout.fillWidth: true
                spacing: 8
                Button {
                    text: "‹ Prev"
                    enabled: icloudNotesPopup.notesPage > 0
                    onClicked: {
                        if (icloudNotesPopup.notesPage > 0) {
                            icloudNotesPopup.notesPage -= 1
                            notesListView.positionViewAtBeginning()
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: {
                        var total = icloudNotesPopup.filteredNotes.length
                        var pg = icloudNotesPopup.notesPage + 1
                        var pgCount = icloudNotesPopup.notesPageCount
                        var startNum = total > 0 ? (icloudNotesPopup.notesPage * icloudNotesPopup.notesPageSize + 1) : 0
                        var endNum = Math.min(total, startNum + icloudNotesPopup.notesPageSize - 1)
                        return "Page " + pg + " / " + pgCount + (total > 0 ? (" · Showing " + startNum + "–" + Math.max(startNum, endNum) + " of " + total + " notes") : " (0 notes)")
                    }
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                    elide: Text.ElideRight
                }
                Button {
                    text: "Next ›"
                    enabled: (icloudNotesPopup.notesPage + 1) < icloudNotesPopup.notesPageCount
                    onClicked: {
                        if ((icloudNotesPopup.notesPage + 1) < icloudNotesPopup.notesPageCount) {
                            icloudNotesPopup.notesPage += 1
                            notesListView.positionViewAtBeginning()
                        }
                    }
                }
            }
        }
    }

    // iCloud Calendar Popup
    Popup {
        id: icloudCalendarPopup
        objectName: "icloudCalendarPopup"
        modal: true
        focus: true
        anchors.centerIn: parent
        property bool isFullScreen: false
        width: isFullScreen ? Math.max(500, root.width - 20) : Math.min(840, Math.max(500, root.width - 80))
        height: isFullScreen ? Math.max(400, root.height - 20) : Math.min(560, Math.max(380, root.height - 80))
        property int eventsPage: 0
        property int eventsPageSize: 25
        readonly property int eventsPageCount: Math.max(1, Math.ceil((backend.icloudEvents ? backend.icloudEvents.length : 0) / Math.max(1, eventsPageSize)))
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.darkBackground
            radius: theme.radius + 4
            border.width: 1
            border.color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.4)
        }

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    text: "📅 " + i18n.tr("iCloud & Local Hub Calendar")
                    font.pixelSize: theme.fontSize + 3
                    font.bold: true
                    color: theme.brightForeground
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                }
                Text {
                    visible: icloudCalendarPopup.width >= 640
                    text: i18n.tr("Per page:")
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                }
                ThemedComboBox {
                    id: eventsPageSizeSelector
                    objectName: "eventsPageSizeSelector"
                    Layout.preferredWidth: 74
                    model: [15, 25, 50, 100]
                    currentIndex: 1
                    onActivated: {
                        icloudCalendarPopup.eventsPageSize = Number(model[currentIndex] || 25)
                        icloudCalendarPopup.eventsPage = 0
                        eventsListView.positionViewAtBeginning()
                    }
                }
                Button {
                    id: calendarFullScreenBtn
                    objectName: "calendarFullScreenBtn"
                    leftPadding: 8
                    rightPadding: 8
                    text: icloudCalendarPopup.width < 640
                          ? (icloudCalendarPopup.isFullScreen ? "🗗" : "⛶")
                          : (icloudCalendarPopup.isFullScreen ? ("🗗 " + i18n.tr("Compact")) : ("⛶ " + i18n.tr("Full Screen")))
                    onClicked: icloudCalendarPopup.isFullScreen = !icloudCalendarPopup.isFullScreen
                }
                Button {
                    text: "✕"
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: icloudCalendarPopup.close()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                color: theme.background
                radius: 6
                border.color: Qt.rgba(1, 1, 1, 0.1)
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 8

                    TextField {
                        id: eventTitleInput
                        Layout.fillWidth: true
                        placeholderText: i18n.tr("Event summary / title...")
                        color: theme.brightForeground
                        placeholderTextColor: theme.darkForeground
                        selectedTextColor: theme.brightForeground
                        selectionColor: theme.selection
                        background: Rectangle {
                            color: theme.darkBackground
                            radius: 5
                            border.width: 1
                            border.color: eventTitleInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.14)
                        }
                    }

                    ThemedComboBox {
                        id: eventCalendarSelector
                        Layout.preferredWidth: 120
                        model: {
                            var cals = backend.icloudCalendars || []
                            var names = []
                            for (var i = 0; i < cals.length; ++i) {
                                var t = (cals[i] && cals[i].title) ? String(cals[i].title).trim() : ""
                                if (t.length > 0 && names.indexOf(t) === -1) names.push(t)
                            }
                            return names.length > 0 ? names : ["Home", "Work", "Family", "Calendar"]
                        }
                    }

                    Button {
                        text: "+1h"
                        onClicked: {
                            if (eventTitleInput.text.trim().length === 0) return
                            var now = Date.now()
                            backend.saveIcloudEvent(eventTitleInput.text.trim(), now + 3600000, now + 7200000, eventCalendarSelector.currentText, "")
                            eventTitleInput.text = ""
                        }
                    }

                    Button {
                        text: "+ " + i18n.tr("Add Event")
                        onClicked: {
                            if (eventTitleInput.text.trim().length === 0) return
                            var now = Date.now()
                            backend.saveIcloudEvent(eventTitleInput.text.trim(), now, now + 3600000, eventCalendarSelector.currentText, "")
                            eventTitleInput.text = ""
                        }
                    }
                }
            }

            ListView {
                id: eventsListView
                objectName: "eventsListView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    id: eventsListScrollBar
                    objectName: "eventsListScrollBar"
                    policy: ScrollBar.AlwaysOn
                    interactive: true
                    width: 10
                }
                model: {
                    var all = backend.icloudEvents || []
                    var maxPage = Math.max(0, icloudCalendarPopup.eventsPageCount - 1)
                    var pg = Math.min(Math.max(0, icloudCalendarPopup.eventsPage), maxPage)
                    var startIdx = pg * icloudCalendarPopup.eventsPageSize
                    return all.slice(startIdx, startIdx + icloudCalendarPopup.eventsPageSize)
                }
                spacing: 6

                delegate: Rectangle {
                    width: Math.max(180, eventsListView.width - 14)
                    height: 54
                    radius: 6
                    color: theme.background
                    border.color: Qt.rgba(1, 1, 1, 0.08)
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 4
                            Layout.fillHeight: true
                            radius: 2
                            color: modelData.calendarTitle === "Work" ? "#007AFF" : (modelData.calendarTitle === "Family" ? "#FF9500" : "#30D158")
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: modelData.title || i18n.tr("Untitled Event")
                                font.bold: true
                                font.pixelSize: theme.fontSize
                                color: theme.brightForeground
                                elide: Text.ElideRight
                            }
                            Text {
                                text: {
                                    var d = new Date(modelData.startMs || Date.now())
                                    return (modelData.calendarTitle || "Calendar") + " · " + d.toLocaleString(Qt.locale(), "ddd, MMM d h:mm AP")
                                }
                                font.pixelSize: theme.fontSize - 2
                                color: theme.darkForeground
                            }
                        }

                        Button {
                            text: "📤 " + i18n.tr("Share")
                            enabled: backend.selectedChatGuid !== ""
                            onClicked: {
                                var d = new Date(modelData.startMs || Date.now())
                                var shareText = "📅 " + (modelData.title || "Event") + "\n" + d.toLocaleString(Qt.locale(), "ddd, MMM d, yyyy h:mm AP")
                                backend.sendMessage(backend.selectedChatGuid, shareText, "")
                                icloudCalendarPopup.close()
                            }
                        }

                        Button {
                            text: "🗑"
                            enabled: !!modelData.id && modelData.syncSource !== "apple_calendar_ro"
                            onClicked: {
                                if (modelData.id && modelData.syncSource !== "apple_calendar_ro") backend.deleteIcloudEvent(modelData.id)
                            }
                        }
                    }
                }
            }

            RowLayout {
                id: eventsPaginationBar
                objectName: "eventsPaginationBar"
                Layout.fillWidth: true
                spacing: 8
                Button {
                    text: "‹ Prev"
                    enabled: icloudCalendarPopup.eventsPage > 0
                    onClicked: {
                        if (icloudCalendarPopup.eventsPage > 0) {
                            icloudCalendarPopup.eventsPage -= 1
                            eventsListView.positionViewAtBeginning()
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Page " + (icloudCalendarPopup.eventsPage + 1) + " / " + icloudCalendarPopup.eventsPageCount + " (" + (backend.icloudEvents ? backend.icloudEvents.length : 0) + " events)"
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                    elide: Text.ElideRight
                }
                Button {
                    text: "Next ›"
                    enabled: (icloudCalendarPopup.eventsPage + 1) < icloudCalendarPopup.eventsPageCount
                    onClicked: {
                        if ((icloudCalendarPopup.eventsPage + 1) < icloudCalendarPopup.eventsPageCount) {
                            icloudCalendarPopup.eventsPage += 1
                            eventsListView.positionViewAtBeginning()
                        }
                    }
                }
            }
        }
    }

    // iCloud Photos Popup
    Popup {
        id: icloudPhotosPopup
        objectName: "icloudPhotosPopup"
        modal: true
        focus: true
        anchors.centerIn: parent
        property bool isFullScreen: false
        width: isFullScreen ? Math.max(520, root.width - 20) : Math.min(960, Math.max(560, root.width - 80))
        height: isFullScreen ? Math.max(400, root.height - 20) : Math.min(580, Math.max(400, root.height - 80))
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: {
            if (backend.icloudPhotosEnabled) {
                backend.fetchIcloudPhotos(0, false)
            }
        }
        Timer {
            interval: 20000
            repeat: true
            running: icloudPhotosPopup.visible && backend.icloudPhotosEnabled && !backend.icloudPhotosLoading && !backend.icloudPhotosSyncing && backend.icloudPhotosPage === 0 && backend.icloudPhotos.length <= backend.icloudPhotosPageSize
            onTriggered: backend.fetchIcloudPhotos(0, false)
        }

        function openImageGalleryFromPhotos(clickedIndex) {
            var allItems = backend.icloudPhotos || []
            var imagesOnly = []
            var mappedIndex = 0
            for (var i = 0; i < allItems.length; ++i) {
                var item = allItems[i]
                var itemIsVid = Boolean(item && (item.isVideo || item.mediaType === "video"))
                if (itemIsVid) continue
                if (i === clickedIndex) mappedIndex = imagesOnly.length
                imagesOnly.push(item)
            }
            if (imagesOnly.length === 0) return
            icloudPhotosPopup.close()
            root.openPhotoViewer(imagesOnly, mappedIndex)
        }
        background: Rectangle {
            color: theme.darkBackground
            radius: theme.radius + 4
            border.width: 1
            border.color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.4)
        }

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    text: "☁️ " + i18n.tr("iCloud Photos")
                    font.pixelSize: theme.fontSize + 2
                    font.bold: true
                    color: theme.brightForeground
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                }
                Text {
                    visible: backend.icloudPhotosTotal > 0 || backend.icloudPhotos.length > 0
                    text: "(" + (backend.icloudPhotosTotal > 0 ? backend.icloudPhotosTotal : backend.icloudPhotos.length) + ")"
                    font.pixelSize: theme.fontSize - 1
                    color: theme.darkForeground
                }
                Text {
                    visible: icloudPhotosPopup.width >= 680
                    text: i18n.tr("Per page:")
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                }
                ThemedComboBox {
                    id: photosPageSizeSelector
                    objectName: "photosPageSizeSelector"
                    Layout.preferredWidth: 72
                    model: [24, 48, 60, 120, 240]
                    currentIndex: {
                        var idx = model.indexOf(backend.icloudPhotosPageSize)
                        return idx >= 0 ? idx : 2
                    }
                    onActivated: {
                        var chosen = Number(model[currentIndex] || 60)
                        photosGridView.positionViewAtBeginning()
                        backend.setIcloudPhotosPageSize(chosen)
                    }
                }
                Button {
                    leftPadding: 8
                    rightPadding: 8
                    text: backend.icloudPhotosSyncing
                          ? (icloudPhotosPopup.width < 680 ? "⏳" : ("⏳ " + i18n.tr("Syncing...")))
                          : (icloudPhotosPopup.width < 680 ? ("⬇ " + i18n.tr("Sync")) : ("⬇ " + i18n.tr("Sync All")))
                    enabled: !backend.icloudPhotosSyncing
                    onClicked: {
                        if (backend.icloudPhotosSyncing) return
                        backend.syncIcloudPhotos()
                    }
                }
                Button {
                    leftPadding: 8
                    rightPadding: 8
                    text: icloudPhotosPopup.width < 680 ? "🔄" : ("🔄 " + i18n.tr("Refresh"))
                    onClicked: backend.fetchIcloudPhotos(backend.icloudPhotosPage, false, true)
                }
                Button {
                    id: photosFullScreenBtn
                    objectName: "photosFullScreenBtn"
                    leftPadding: 8
                    rightPadding: 8
                    text: icloudPhotosPopup.width < 680
                          ? (icloudPhotosPopup.isFullScreen ? "🗗" : "⛶")
                          : (icloudPhotosPopup.isFullScreen ? ("🗗 " + i18n.tr("Compact")) : ("⛶ " + i18n.tr("Full Screen")))
                    onClicked: icloudPhotosPopup.isFullScreen = !icloudPhotosPopup.isFullScreen
                }
                Button {
                    leftPadding: 8
                    rightPadding: 8
                    text: "✕"
                    onClicked: icloudPhotosPopup.close()
                }
            }

            GridView {
                id: photosGridView
                objectName: "photosGridView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                rightMargin: 12
                readonly property int gridCols: Math.max(2, Math.floor(Math.max(180, width - 14) / 182))
                cellWidth: Math.max(172, Math.floor(Math.max(180, width - 14) / gridCols))
                cellHeight: 234
                model: backend.icloudPhotos
                ScrollBar.vertical: ScrollBar {
                    id: photosGridScrollBar
                    objectName: "photosGridScrollBar"
                    policy: ScrollBar.AlwaysOn
                    interactive: true
                    width: 10
                }
                onAtYEndChanged: {
                    if (atYEnd && moving && backend.icloudPhotosHasMore && !backend.icloudPhotosLoading) {
                        backend.loadMoreIcloudPhotos()
                    }
                }

                delegate: Rectangle {
                    width: Math.max(162, photosGridView.cellWidth - 10)
                    height: 224
                    clip: true
                    radius: 8
                    color: theme.background
                    border.color: Qt.rgba(1, 1, 1, 0.1)
                    border.width: 1

                    Connections {
                        target: backend
                        function onAttachmentReady(guid, localUrl) {
                            if (modelData && modelData.guid === guid && localUrl && localUrl.length > 0) {
                                thumbImg.source = localUrl
                            }
                        }
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 4

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: modelData.isPortrait ? 124 : 102
                            radius: 6
                            color: theme.darkBackground
                            clip: true

                            Image {
                                id: thumbImg
                                property string baseSourceUrl: modelData.previewUrl || modelData.localUrl || ((modelData.guid && !modelData.isCloudEvicted) ? backend.attachmentDownloadUrl(modelData.guid) : "")
                                property int retryAttempt: 0
                                anchors.fill: parent
                                anchors.margins: 2
                                fillMode: Image.PreserveAspectFit
                                autoTransform: true
                                smooth: true
                                source: {
                                    if (!baseSourceUrl) return ""
                                    if (retryAttempt <= 0) return baseSourceUrl
                                    var sep = baseSourceUrl.indexOf("?") >= 0 ? "&" : "?"
                                    return baseSourceUrl + sep + "retry=" + retryAttempt
                                }
                                asynchronous: true
                                cache: true
                                onBaseSourceUrlChanged: retryAttempt = 0
                                onStatusChanged: {
                                    if (status === Image.Error && retryAttempt < 3 && baseSourceUrl.indexOf("http") === 0) {
                                        thumbRetryTimer.restart()
                                    }
                                }
                            }
                            Timer {
                                id: thumbRetryTimer
                                interval: 2200
                                repeat: false
                                onTriggered: {
                                    if (thumbImg.retryAttempt < 3) {
                                        thumbImg.retryAttempt += 1
                                    }
                                }
                            }

                            Text {
                                anchors.centerIn: parent
                                text: (Boolean(modelData.isVideo) || modelData.mediaType === "video") ? "🎬" : "🖼"
                                font.pixelSize: 24
                                visible: thumbImg.status !== Image.Ready
                            }

                            Rectangle {
                                visible: Boolean(modelData.isVideo) || modelData.mediaType === "video"
                                anchors.left: parent.left
                                anchors.bottom: parent.bottom
                                anchors.margins: 4
                                height: 18
                                width: vidDurationText.implicitWidth + 8
                                radius: 4
                                color: Qt.rgba(0, 0, 0, 0.78)
                                Text {
                                    id: vidDurationText
                                    anchors.centerIn: parent
                                    font.pixelSize: 9
                                    font.bold: true
                                    color: "#ffffff"
                                    text: {
                                        var sec = Math.max(0, Math.round(Number(modelData.duration || 0)))
                                        if (sec <= 0) return "🎬 Video"
                                        var m = Math.floor(sec / 60)
                                        var s = sec % 60
                                        return "🎬 " + m + ":" + (s < 10 ? "0" : "") + s
                                    }
                                }
                            }

                            RowLayout {
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.margins: 4
                                spacing: 4

                                Rectangle {
                                    height: 18
                                    width: orientText.implicitWidth + 8
                                    radius: 4
                                    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.85)
                                    border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.25)
                                    border.width: 1
                                    Text {
                                        id: orientText
                                        anchors.centerIn: parent
                                        text: modelData.isPortrait ? "▯ Portrait" : "▭ Landscape"
                                        font.pixelSize: 9
                                        color: theme.foreground
                                    }
                                }

                                Rectangle {
                                    width: 18
                                    height: 18
                                    radius: 9
                                    color: modelData.isCloudEvicted ? theme.accent : theme.green
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.isCloudEvicted ? "☁" : "✓"
                                        font.pixelSize: 10
                                        color: theme.brightForeground
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    var isVid = Boolean(modelData.isVideo) || modelData.mediaType === "video"
                                    if (modelData.guid && !modelData.isCloudEvicted) {
                                        if (isVid) {
                                            backend.openAttachment(modelData.guid, modelData.name || "video.mov")
                                        } else {
                                            icloudPhotosPopup.openImageGalleryFromPhotos(index)
                                        }
                                    } else if (modelData.guid && modelData.isCloudEvicted && modelData.canUnpurge !== false) {
                                        root.showToast("☁ " + i18n.tr("Requesting iCloud download..."))
                                        backend.downloadCloudAttachment(modelData.guid)
                                    } else if (modelData.isCloudEvicted) {
                                        root.showToast("☁ " + i18n.tr("Stored in iCloud Only — unpurge unavailable on server"))
                                    }
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: modelData.name || "Photo"
                            font.pixelSize: theme.fontSize - 2
                            color: theme.foreground
                            elide: Text.ElideMiddle
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Button {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                implicitHeight: 30
                                topPadding: 4
                                bottomPadding: 4
                                leftPadding: 4
                                rightPadding: 4
                                font.pixelSize: theme.fontSize - 3
                                enabled: !modelData.isCloudEvicted || modelData.canUnpurge !== false
                                text: {
                                    var isVid = Boolean(modelData.isVideo) || modelData.mediaType === "video"
                                    if (modelData.isCloudEvicted) {
                                        if (modelData.canUnpurge === false) {
                                            return isVid ? "🎬 iCloud Only" : "☁ iCloud Only"
                                        }
                                        return "☁ Unpurge"
                                    }
                                    return isVid ? "▶ Video" : "View"
                                }
                                onClicked: {
                                    if (!modelData.guid) return
                                    var isVid = Boolean(modelData.isVideo) || modelData.mediaType === "video"
                                    if (modelData.isCloudEvicted) {
                                        if (modelData.canUnpurge === false) {
                                            root.showToast("☁ " + i18n.tr("Stored in iCloud Only — unpurge unavailable on server"))
                                            return
                                        }
                                        root.showToast("☁ " + i18n.tr("Requesting iCloud download..."))
                                        backend.downloadCloudAttachment(modelData.guid)
                                    } else if (isVid) {
                                        backend.openAttachment(modelData.guid, modelData.name || "video.mov")
                                    } else {
                                        icloudPhotosPopup.openImageGalleryFromPhotos(index)
                                    }
                                }
                            }

                            Button {
                                objectName: "photoCardSendBtn"
                                visible: !modelData.isCloudEvicted
                                implicitHeight: 30
                                topPadding: 4
                                bottomPadding: 4
                                leftPadding: 6
                                rightPadding: 6
                                font.pixelSize: theme.fontSize - 2
                                text: "📤 Send"
                                enabled: backend.selectedChatGuid !== "" && !!modelData.guid && !modelData.isCloudEvicted
                                ToolTip.visible: hovered && backend.selectedChatGuid !== ""
                                ToolTip.text: modelData.isCloudEvicted
                                              ? i18n.tr("Download from iCloud before sending")
                                              : (i18n.tr("Send to ") + (backend.selectedChatTitle || i18n.tr("active chat")))
                                onClicked: {
                                    if (modelData.guid && !modelData.isCloudEvicted) {
                                        backend.shareIcloudPhotoToChat(backend.selectedChatGuid, modelData.guid, modelData.name || "photo.jpg")
                                        icloudPhotosPopup.close()
                                    }
                                }
                            }
                        }
                    }

                }
            }

            RowLayout {
                id: photosPaginationBar
                objectName: "photosPaginationBar"
                Layout.fillWidth: true
                spacing: 8

                Button {
                    text: "‹ Prev"
                    enabled: backend.icloudPhotosPage > 0 && !backend.icloudPhotosLoading
                    onClicked: {
                        photosGridView.positionViewAtBeginning()
                        backend.prevIcloudPhotosPage()
                    }
                }

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: {
                        var total = backend.icloudPhotosTotal > 0 ? backend.icloudPhotosTotal : backend.icloudPhotos.length
                        var pg = backend.icloudPhotosPage + 1
                        var pgCount = Math.max(1, backend.icloudPhotosPageCount)
                        var startNum = total > 0 ? (backend.icloudPhotosPage * backend.icloudPhotosPageSize + 1) : 0
                        var endNum = Math.min(total, startNum + backend.icloudPhotos.length - 1)
                        if (backend.icloudPhotosLoading) {
                            return "Loading page " + pg + " / " + pgCount + "..."
                        }
                        return "Page " + pg + " / " + pgCount + (total > 0 ? (" · Showing " + startNum + "–" + Math.max(startNum, endNum) + " of " + total) : "")
                    }
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                    elide: Text.ElideRight
                }

                Button {
                    text: "+ Load More"
                    visible: backend.icloudPhotosHasMore
                    enabled: backend.icloudPhotosHasMore && !backend.icloudPhotosLoading
                    onClicked: backend.loadMoreIcloudPhotos()
                }

                Button {
                    text: "Next ›"
                    enabled: ((backend.icloudPhotosPage + 1) < backend.icloudPhotosPageCount || backend.icloudPhotosHasMore) && !backend.icloudPhotosLoading
                    onClicked: {
                        photosGridView.positionViewAtBeginning()
                        backend.nextIcloudPhotosPage()
                    }
                }
            }
        }
    }

    // New Message Modal Overlay
    Rectangle {
        id: newChatPopup
        visible: root.showNewChatModal
        anchors.fill: parent
        color: "#aa0e0e14"
        z: 997

        MouseArea {
            anchors.fill: parent
            onClicked: root.showNewChatModal = false
        }

        Rectangle {
            width: 500
            height: root.newChatSuggestions.length > 0 ? 500 : 280
            anchors.centerIn: parent
            radius: theme.radius + 2
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: i18n.tr("New Message")
                        font.pixelSize: theme.fontSize + 2
                        font.bold: true
                        color: theme.brightForeground
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: i18n.tr("Type a name, phone, or email")
                        font.pixelSize: theme.fontSize - 3
                        color: theme.darkForeground
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: Math.max(48, theme.fontSize + 28)
                    Layout.preferredHeight: Math.max(48, theme.fontSize + 28)
                    radius: 8
                    color: theme.background
                    border.width: 1
                    border.color: newChatRecipient.activeFocus ? theme.accent : theme.muted

                    TextField {
                        id: newChatRecipient
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        topPadding: 8
                        bottomPadding: 12
                        verticalAlignment: TextInput.AlignVCenter
                        placeholderText: i18n.tr("To: Type a contact name (e.g. Alex), phone, or email...")
                        placeholderTextColor: theme.darkForeground
                        color: theme.brightForeground
                        font.pixelSize: theme.fontSize
                        background: Item {}
                        onTextChanged: {
                            root.newChatSelectedHandle = ""
                            root.refreshNewChatSuggestions()
                        }
                        Keys.onDownPressed: (event) => {
                            if (root.newChatSuggestions.length > 0) {
                                root.newChatSelectedIndex = Math.min(root.newChatSuggestions.length - 1, root.newChatSelectedIndex + 1)
                                newChatSuggestionsList.positionViewAtIndex(root.newChatSelectedIndex, ListView.Contain)
                                event.accepted = true
                            }
                        }
                        Keys.onUpPressed: (event) => {
                            if (root.newChatSuggestions.length > 0) {
                                root.newChatSelectedIndex = Math.max(0, root.newChatSelectedIndex - 1)
                                newChatSuggestionsList.positionViewAtIndex(root.newChatSelectedIndex, ListView.Contain)
                                event.accepted = true
                            }
                        }
                        Keys.onTabPressed: (event) => {
                            if (root.newChatSuggestions.length > 0) {
                                var item = root.newChatSuggestions[root.newChatSelectedIndex] || root.newChatSuggestions[0]
                                newChatRecipient.text = (item.displayName || item.name) + " <" + (item.address || item.handle) + ">"
                                root.newChatSelectedHandle = item.address || item.handle
                                newChatText.forceActiveFocus()
                                event.accepted = true
                            }
                        }
                        onAccepted: root.submitNewChat("")
                    }
                }

                // Live Contact Autocomplete Suggestions List
                Rectangle {
                    visible: root.newChatSuggestions.length > 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 160
                    radius: 8
                    color: theme.background
                    border.width: 1
                    border.color: theme.muted
                    clip: true

                    ListView {
                        id: newChatSuggestionsList
                        anchors.fill: parent
                        anchors.margins: 4
                        clip: true
                        spacing: 2
                        model: root.newChatSuggestions

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                        }

                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: newChatSuggestionsList.width
                            height: 48
                            radius: 6
                            color: (index === root.newChatSelectedIndex || suggestionMouse.containsMouse)
                                   ? theme.selection
                                   : "transparent"
                            border.width: index === root.newChatSelectedIndex ? 1 : 0
                            border.color: theme.accent

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 32
                                    radius: 16
                                    color: modelData.avatarColor || theme.accent
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.initials || "💬"
                                        font.pixelSize: theme.fontSize - 3
                                        font.bold: true
                                        color: "#ffffff"
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.displayName || modelData.name || modelData.address
                                        font.pixelSize: theme.fontSize - 1
                                        font.bold: true
                                        color: theme.brightForeground
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.formattedAddress || modelData.displayHandle || modelData.address
                                        font.pixelSize: theme.fontSize - 3
                                        color: theme.darkForeground
                                        elide: Text.ElideRight
                                    }
                                }

                                Rectangle {
                                    height: 22
                                    implicitWidth: suggBadgeLbl.implicitWidth + 12
                                    radius: 11
                                    color: modelData.hasExistingChat ? theme.lighterBackground : Qt.rgba(10/255, 132/255, 255/255, 0.2)
                                    border.width: 1
                                    border.color: modelData.hasExistingChat ? theme.muted : theme.accent

                                    Text {
                                        id: suggBadgeLbl
                                        anchors.centerIn: parent
                                        text: modelData.hasExistingChat ? i18n.tr("Open Chat") : i18n.tr("New Chat")
                                        font.pixelSize: theme.fontSize - 4
                                        font.bold: true
                                        color: modelData.hasExistingChat ? theme.foreground : theme.accent
                                    }
                                }
                            }

                            MouseArea {
                                id: suggestionMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: root.newChatSelectedIndex = index
                                onClicked: {
                                    var addr = modelData.address || modelData.handle || ""
                                    root.submitNewChat(addr)
                                }
                            }
                        }
                    }

                }

                Rectangle {
                    Layout.fillWidth: true
                    height: Math.max(50, theme.fontSize + 30)
                    Layout.preferredHeight: Math.max(50, theme.fontSize + 30)
                    radius: 8
                    color: theme.background
                    border.width: 1
                    border.color: newChatText.activeFocus ? theme.accent : theme.muted

                    TextField {
                        id: newChatText
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        topPadding: 8
                        bottomPadding: 12
                        verticalAlignment: TextInput.AlignVCenter
                        placeholderText: i18n.tr("Optional initial message (or leave blank to open chat)...")
                        placeholderTextColor: theme.darkForeground
                        color: theme.brightForeground
                        font.pixelSize: theme.fontSize
                        background: Item {}
                        onAccepted: root.submitNewChat("")
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Item { Layout.fillWidth: true }

                    Rectangle {
                        height: 34
                        implicitWidth: 80
                        radius: 7
                        color: theme.lighterBackground
                        Text {
                            anchors.centerIn: parent
                            text: i18n.tr("Cancel")
                            color: theme.foreground
                            font.pixelSize: theme.fontSize - 1
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.showNewChatModal = false
                        }
                    }

                    Rectangle {
                        height: 34
                        implicitWidth: 156
                        radius: 7
                        color: theme.accent
                        Text {
                            anchors.centerIn: parent
                            text: newChatText.text.trim().length > 0 ? i18n.tr("Send Message") : i18n.tr("Open Conversation")
                            color: theme.darkerBackground
                            font.pixelSize: theme.fontSize - 1
                            font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.submitNewChat("")
                        }
                    }
                }
            }
        }
    }

    // Contact / conversation info: read-only details plus what's been shared
    Rectangle {
        id: contactInfoModal
        objectName: "contactInfoModal"
        visible: root.showContactInfo
        // Holds focus while open so Return/typing never reach the composer underneath
        Keys.onEscapePressed: root.closeContactInfo()
        Keys.onPressed: (event) => { event.accepted = true }
        // Over the open conversation only (not the sidebar)
        readonly property point paneOrigin: {
            void (conversationPane.x + conversationPane.y + conversationPane.width + conversationPane.height + root.width + root.height)
            return conversationPane.mapToItem(contactInfoModal.parent, 0, 0)
        }
        x: paneOrigin.x
        y: paneOrigin.y
        width: conversationPane.width
        height: conversationPane.height
        color: "#aa0e0e14"
        z: 997

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.closeContactInfo()
        }

        Rectangle {
            objectName: "contactInfoCard"
            width: Math.min(560, parent.width - 60)
            height: Math.min(parent.height - 60, 560)
            anchors.centerIn: parent
            radius: theme.radius + 2
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent
            MouseArea { anchors.fill: parent }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 12

                // Who
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    RoundAvatar {
                        Layout.preferredWidth: 56
                        Layout.preferredHeight: 56
                        color: backend.selectedChatColor
                        source: backend.selectedChatAvatar
                        initials: backend.selectedChatInitials
                        fontPixelSize: theme.fontSize + 3
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: backend.selectedChatTitle
                            font.pixelSize: theme.fontSize + 3
                            font.bold: true
                            color: theme.brightForeground
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: backend.selectedChatSubtitle
                            font.pixelSize: theme.fontSize - 2
                            color: theme.darkForeground
                            elide: Text.ElideRight
                        }
                    }
                    Text {
                        text: "✕"
                        font.pixelSize: theme.fontSize + 1
                        color: theme.foreground
                        Layout.alignment: Qt.AlignTop
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.closeContactInfo()
                        }
                    }
                }

                // Tabs
                Row {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: [
                            i18n.tr("Details"),
                            i18n.tr("Photos") + " (" + (root.contactInfoMedia.photos || []).length + ")",
                            i18n.tr("Links") + " (" + (root.contactInfoMedia.links || []).length + ")",
                            i18n.tr("Files") + " (" + (root.contactInfoMedia.files || []).length + ")"
                        ]
                        delegate: Rectangle {
                            required property string modelData
                            required property int index
                            height: 28
                            width: tabLbl.implicitWidth + 22
                            radius: 7
                            color: root.contactInfoTab === index ? theme.accent : (tabMouse.containsMouse ? theme.selection : theme.lighterBackground)
                            border.width: 1
                            border.color: root.contactInfoTab === index ? theme.accent : theme.muted
                            Text {
                                id: tabLbl
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: theme.fontSize - 2
                                font.bold: root.contactInfoTab === index
                                color: root.contactInfoTab === index ? theme.darkerBackground : theme.foreground
                            }
                            MouseArea {
                                id: tabMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.contactInfoTab = index
                            }
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.contactInfoTab

                    // 0. Details (read-only)
                    ColumnLayout {
                        spacing: 8
                        ListView {
                            objectName: "contactInfoDetailRows"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 4
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            model: root.contactInfoDetails.found
                                   ? (root.contactInfoDetails.rows || [])
                                   : (backend.selectedChatIsGroup
                                      ? [{ label: i18n.tr("Group"), value: backend.selectedChatSubtitle }]
                                      : [{ label: i18n.tr("Address"), value: backend.selectedChatAddress }])
                            delegate: RowLayout {
                                required property var modelData
                                width: ListView.view.width
                                spacing: 10
                                Text {
                                    Layout.preferredWidth: 130
                                    Layout.alignment: Qt.AlignTop
                                    text: modelData.label
                                    font.pixelSize: theme.fontSize - 2
                                    color: theme.darkForeground
                                    elide: Text.ElideRight
                                }
                                TextEdit {
                                    Layout.fillWidth: true
                                    text: modelData.value
                                    textFormat: TextEdit.PlainText
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    font.pixelSize: theme.fontSize - 1
                                    color: theme.foreground
                                    selectionColor: theme.selection
                                }
                            }
                        }
                        Rectangle {
                            objectName: "contactInfoEditBtn"
                            visible: !backend.selectedChatIsGroup && backend.selectedChatAddress.length > 0
                            Layout.alignment: Qt.AlignRight
                            height: 32
                            width: editLbl.implicitWidth + 26
                            radius: 8
                            color: editInfoMouse.containsMouse ? theme.accent : theme.lighterBackground
                            border.width: 1
                            border.color: theme.accent
                            Text {
                                id: editLbl
                                anchors.centerIn: parent
                                text: backend.selectedChatHasContact ? ("✎ " + i18n.tr("Edit")) : ("＋ " + i18n.tr("Add Contact"))
                                font.pixelSize: theme.fontSize - 1
                                font.bold: true
                                color: editInfoMouse.containsMouse ? theme.darkerBackground : theme.accent
                            }
                            MouseArea {
                                id: editInfoMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                // The card stays underneath; Save/Cancel back out to it
                                onClicked: root.openContactModal(backend.selectedChatAddress, backend.selectedChatTitle)
                            }
                        }
                    }

                    // 1. Photos
                    Item {
                        GridView {
                            id: infoPhotoGrid
                            objectName: "contactInfoPhotos"
                            anchors.fill: parent
                            clip: true
                            cellWidth: Math.floor(width / Math.max(1, Math.floor(width / 120)))
                            cellHeight: cellWidth
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            model: root.contactInfoMedia.photos || []
                            delegate: Item {
                                required property var modelData
                                required property int index
                                width: infoPhotoGrid.cellWidth
                                height: infoPhotoGrid.cellHeight
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: 3
                                    radius: 6
                                    clip: true
                                    color: theme.lighterBackground
                                    Image {
                                        anchors.fill: parent
                                        source: modelData.localUrl || ""
                                        sourceSize.width: 240
                                        asynchronous: true
                                        autoTransform: true
                                        fillMode: Image.PreserveAspectCrop
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        visible: !(modelData.localUrl || "").length
                                        text: "📷"
                                        font.pixelSize: theme.fontSize + 6
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        // Closing the viewer comes back to the Photos tab
                                        onClicked: root.openPhotoViewer(root.contactInfoMedia.photos, index)
                                    }
                                }
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: infoPhotoGrid.count === 0
                            text: i18n.tr("No photos in this conversation yet")
                            font.pixelSize: theme.fontSize - 1
                            color: theme.darkForeground
                        }
                    }

                    // 2. Links
                    Item {
                        ListView {
                            id: infoLinkList
                            objectName: "contactInfoLinks"
                            anchors.fill: parent
                            clip: true
                            spacing: 4
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            model: root.contactInfoMedia.links || []
                            delegate: Rectangle {
                                required property var modelData
                                width: ListView.view.width
                                height: linkCol.implicitHeight + 12
                                radius: 6
                                color: linkMouse.containsMouse ? theme.lighterBackground : "transparent"
                                ColumnLayout {
                                    id: linkCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.margins: 8
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.title || modelData.host || modelData.url
                                        font.pixelSize: theme.fontSize - 1
                                        font.bold: true
                                        color: theme.brightForeground
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.url + "  ·  " + Qt.formatDate(new Date(Number(modelData.date || 0)), "MMM d, yyyy")
                                        font.pixelSize: theme.fontSize - 3
                                        color: theme.accent
                                        elide: Text.ElideMiddle
                                    }
                                }
                                MouseArea {
                                    id: linkMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.openUrl(modelData.url)
                                }
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: infoLinkList.count === 0
                            text: i18n.tr("No links in this conversation yet")
                            font.pixelSize: theme.fontSize - 1
                            color: theme.darkForeground
                        }
                    }

                    // 3. Files
                    Item {
                        ListView {
                            id: infoFileList
                            objectName: "contactInfoFiles"
                            anchors.fill: parent
                            clip: true
                            spacing: 4
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            model: root.contactInfoMedia.files || []
                            delegate: Rectangle {
                                required property var modelData
                                width: ListView.view.width
                                height: 44
                                radius: 6
                                color: fileMouse.containsMouse ? theme.lighterBackground : "transparent"
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    spacing: 10
                                    Text {
                                        text: (modelData.mimeType || "").indexOf("audio/") === 0 ? "🎵"
                                              : ((modelData.mimeType || "").indexOf("video/") === 0 ? "🎬" : "📎")
                                        font.pixelSize: theme.fontSize + 2
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.name || i18n.tr("Attachment")
                                            font.pixelSize: theme.fontSize - 1
                                            color: theme.brightForeground
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            text: (modelData.fromMe ? i18n.tr("You") : backend.selectedChatTitle) + "  ·  " + Qt.formatDate(new Date(Number(modelData.date || 0)), "MMM d, yyyy")
                                            font.pixelSize: theme.fontSize - 3
                                            color: theme.darkForeground
                                        }
                                    }
                                }
                                MouseArea {
                                    id: fileMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.openAttachment(modelData.guid, modelData.name || "")
                                }
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: infoFileList.count === 0
                            text: i18n.tr("No files in this conversation yet")
                            font.pixelSize: theme.fontSize - 1
                            color: theme.darkForeground
                        }
                    }
                }
            }
        }
    }

    // Thread view: the original message and its replies, like Messages on iOS/macOS
    Rectangle {
        id: threadViewModal
        objectName: "threadViewModal"
        visible: root.showThreadView
        // Covers only the open conversation (not the sidebar), so the thread
        // card is centered over the chat it belongs to.
        readonly property point paneOrigin: {
            void (conversationPane.x + conversationPane.y + conversationPane.width + conversationPane.height + root.width + root.height)
            return conversationPane.mapToItem(threadViewModal.parent, 0, 0)
        }
        x: paneOrigin.x
        y: paneOrigin.y
        width: conversationPane.width
        height: conversationPane.height
        color: "#aa0e0e14"
        z: 998

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true  // takes hover from the timeline underneath (no stray tooltips)
            onClicked: root.showThreadView = false
        }

        Rectangle {
            objectName: "threadViewCard"
            width: Math.min(560, parent.width - 80)
            height: Math.min(parent.height - 80, threadCol.implicitHeight + 44)
            anchors.centerIn: parent
            radius: theme.radius + 2
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent
            MouseArea { anchors.fill: parent }  // keep clicks inside from closing it

            ColumnLayout {
                id: threadCol
                anchors.fill: parent
                anchors.margins: 22
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: i18n.tr("Thread")
                        font.pixelSize: theme.fontSize + 3
                        font.bold: true
                        color: theme.brightForeground
                    }
                    Text {
                        text: "✕"
                        font.pixelSize: theme.fontSize + 1
                        color: theme.foreground
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.showThreadView = false
                        }
                    }
                }

                ListView {
                    id: threadList
                    objectName: "threadList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: contentHeight
                    clip: true
                    spacing: 8
                    boundsBehavior: Flickable.StopAtBounds
                    // Re-read whenever the timeline changes
                    model: root.showThreadView ? (backend.messages, backend.threadMessages(root.threadRootGuid)) : []
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: Rectangle {
                        id: threadItem
                        required property var modelData
                        required property int index
                        readonly property bool isRoot: modelData.guid === root.threadRootGuid
                        width: ListView.view.width
                        height: threadItemCol.implicitHeight + 16
                        radius: 8
                        color: threadItemMouse.containsMouse ? theme.lighterBackground : (isRoot ? theme.selection : "transparent")
                        border.width: isRoot ? 1 : 0
                        border.color: theme.accent

                        ColumnLayout {
                            id: threadItemCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 8
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    text: threadItem.modelData.isFromMe ? i18n.tr("You") : (threadItem.modelData.senderName || "")
                                    font.pixelSize: theme.fontSize - 3
                                    font.bold: true
                                    color: threadItem.modelData.isFromMe ? theme.accent : (threadItem.modelData.senderColor || theme.accent)
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: threadItem.modelData.timeText || ""
                                    font.pixelSize: theme.fontSize - 4
                                    color: theme.darkForeground
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: (threadItem.modelData.text || "").length > 0
                                      ? threadItem.modelData.text
                                      : (threadItem.modelData.hasImageAttachment ? "📷 " + i18n.tr("Photo") : (threadItem.modelData.hasFileAttachment ? "📎 " + i18n.tr("Attachment") : ""))
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                font.pixelSize: theme.fontSize - 1
                                color: theme.foreground
                            }
                            Text {
                                visible: threadItem.isRoot
                                text: "↑ " + i18n.tr("Show in conversation")
                                font.pixelSize: theme.fontSize - 3
                                color: theme.accent
                            }
                        }
                        MouseArea {
                            id: threadItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.jumpToMessage(threadItem.modelData.guid)
                        }
                    }
                }

                // Reply to the thread without leaving it
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(120, Math.max(40, Math.ceil(threadReplyInput.contentHeight) + threadReplyInput.topPadding + threadReplyInput.bottomPadding))
                        radius: theme.radius
                        color: theme.background
                        border.width: 1
                        border.color: threadReplyInput.activeFocus ? theme.accent : theme.muted
                        TextArea {
                            id: threadReplyInput
                            objectName: "threadReplyInput"
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            topPadding: 9
                            bottomPadding: 9
                            placeholderText: i18n.tr("Reply to thread… (Enter to send)")
                            placeholderTextColor: theme.darkForeground
                            color: theme.brightForeground
                            font.pixelSize: theme.fontSize
                            wrapMode: TextArea.Wrap
                            background: Item {}
                            Keys.onReturnPressed: function(event) {
                                if (event.modifiers & Qt.ShiftModifier) { event.accepted = false; return }
                                root.sendThreadReply()
                            }
                            Keys.onEnterPressed: function(event) {
                                if (event.modifiers & Qt.ShiftModifier) { event.accepted = false; return }
                                root.sendThreadReply()
                            }
                            Keys.onEscapePressed: root.closeThread()
                        }
                    }
                    Rectangle {
                        objectName: "threadReplySendBtn"
                        Layout.preferredWidth: 70
                        Layout.preferredHeight: 40
                        radius: theme.radius
                        readonly property bool canSend: threadReplyInput.text.trim().length > 0
                        color: canSend ? theme.accent : theme.lighterBackground
                        Text {
                            anchors.centerIn: parent
                            text: i18n.tr("Reply")
                            font.pixelSize: theme.fontSize - 1
                            font.bold: true
                            color: parent.canSend ? "#ffffff" : theme.darkForeground
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.sendThreadReply()
                        }
                    }
                }
            }
        }
    }

    // Add / Edit Contact Modal Overlay
    Rectangle {
        visible: root.showContactModal
        anchors.fill: parent
        color: "#aa0e0e14"
        z: 998

        MouseArea {
            anchors.fill: parent
            onClicked: root.showContactModal = false
        }

        Rectangle {
            objectName: "contactModalCard"
            width: 440
            height: root.contactModalRows.length > 0 ? Math.min(parent.height - 80, 410 + 34 + root.contactModalRows.length * 40) : 410
            anchors.centerIn: parent
            radius: theme.radius + 2
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 22
                spacing: 12

                Text {
                    text: i18n.tr("Contact Details")
                    font.pixelSize: theme.fontSize + 3
                    font.bold: true
                    color: theme.brightForeground
                }

                Text {
                    text: i18n.tr("Save or edit contact info synced across the fleet and BlueBubbles.")
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        Layout.fillWidth: true
                        height: Math.max(42, theme.fontSize + 22)
                        Layout.preferredHeight: Math.max(42, theme.fontSize + 22)
                        radius: 6
                        color: theme.lighterBackground
                        border.width: 1
                        border.color: contactFnInput.activeFocus ? theme.accent : theme.muted

                        TextInput {
                            id: contactFnInput
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            verticalAlignment: TextInput.AlignVCenter
                            text: root.contactModalFirstName
                            color: theme.brightForeground
                            font.pixelSize: theme.fontSize
                            Text {
                                text: i18n.tr("First Name")
                                color: theme.darkForeground
                                visible: !contactFnInput.text && !contactFnInput.activeFocus
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                font.pixelSize: theme.fontSize
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: Math.max(42, theme.fontSize + 22)
                        Layout.preferredHeight: Math.max(42, theme.fontSize + 22)
                        radius: 6
                        color: theme.lighterBackground
                        border.width: 1
                        border.color: contactLnInput.activeFocus ? theme.accent : theme.muted

                        TextInput {
                            id: contactLnInput
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            verticalAlignment: TextInput.AlignVCenter
                            text: root.contactModalLastName
                            color: theme.brightForeground
                            font.pixelSize: theme.fontSize
                            Text {
                                text: i18n.tr("Last Name")
                                color: theme.darkForeground
                                visible: !contactLnInput.text && !contactLnInput.activeFocus
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                font.pixelSize: theme.fontSize
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: Math.max(42, theme.fontSize + 22)
                    Layout.preferredHeight: Math.max(42, theme.fontSize + 22)
                    radius: 6
                    color: theme.lighterBackground
                    border.width: 1
                    border.color: contactPhInput.activeFocus ? theme.accent : theme.muted

                    TextInput {
                        id: contactPhInput
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        verticalAlignment: TextInput.AlignVCenter
                        text: root.contactModalPhone
                        color: theme.brightForeground
                        font.pixelSize: theme.fontSize
                        Text {
                            text: i18n.tr("Phone Number (+1...)")
                            color: theme.darkForeground
                            visible: !contactPhInput.text && !contactPhInput.activeFocus
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            font.pixelSize: theme.fontSize
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: Math.max(42, theme.fontSize + 22)
                    Layout.preferredHeight: Math.max(42, theme.fontSize + 22)
                    radius: 6
                    color: theme.lighterBackground
                    border.width: 1
                    border.color: contactEmInput.activeFocus ? theme.accent : theme.muted

                    TextInput {
                        id: contactEmInput
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        verticalAlignment: TextInput.AlignVCenter
                        text: root.contactModalEmail
                        color: theme.brightForeground
                        font.pixelSize: theme.fontSize
                        Text {
                            text: i18n.tr("Email Address")
                            color: theme.darkForeground
                            visible: !contactEmInput.text && !contactEmInput.activeFocus
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            font.pixelSize: theme.fontSize
                        }
                    }
                }

                // Everything else the Mac has for this contact (read-only)
                Text {
                    visible: root.contactModalRows.length > 0
                    text: i18n.tr("On your Mac")
                    font.pixelSize: theme.fontSize - 1
                    font.bold: true
                    color: theme.accent
                }
                ListView {
                    objectName: "contactDetailRows"
                    visible: root.contactModalRows.length > 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: root.contactModalRows
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: RowLayout {
                        required property var modelData
                        width: ListView.view.width
                        spacing: 10
                        Text {
                            Layout.preferredWidth: 130
                            Layout.alignment: Qt.AlignTop
                            text: modelData.label
                            font.pixelSize: theme.fontSize - 2
                            color: theme.darkForeground
                            elide: Text.ElideRight
                        }
                        TextEdit {
                            Layout.fillWidth: true
                            text: modelData.value
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            font.pixelSize: theme.fontSize - 1
                            color: theme.foreground
                            selectionColor: theme.selection
                        }
                    }
                }

                Item { Layout.fillHeight: true; visible: root.contactModalRows.length === 0 }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Item { Layout.fillWidth: true }

                    Rectangle {
                        height: 34
                        implicitWidth: 80
                        radius: 7
                        color: theme.lighterBackground
                        Text {
                            anchors.centerIn: parent
                            text: i18n.tr("Cancel")
                            color: theme.foreground
                            font.pixelSize: theme.fontSize - 1
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.showContactModal = false
                        }
                    }

                    Rectangle {
                        height: 34
                        implicitWidth: 120
                        radius: 7
                        color: theme.accent
                        Text {
                            anchors.centerIn: parent
                            text: i18n.tr("Save Contact")
                            color: theme.darkerBackground
                            font.pixelSize: theme.fontSize - 1
                            font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                backend.saveContact(contactFnInput.text, contactLnInput.text, contactPhInput.text, contactEmInput.text)
                                root.showContactModal = false
                            }
                        }
                    }
                }
            }
        }
    }

    // BlueBubbles Server Configuration Modal Overlay (Ctrl+, / First-Run Setup)
    Rectangle {
        id: serverConfigPopup
        objectName: "serverConfigPopup"
        visible: root.showServerConfigModal
        anchors.fill: parent
        color: "#aa0e0e14"
        z: 999

        MouseArea {
            anchors.fill: parent
            onClicked: root.showServerConfigModal = false
        }

        Rectangle {
            id: serverConfigCard
            objectName: "serverConfigCard"
            width: Math.min(580, root.width - 40)
            height: Math.min(root.height - 32, serverConfigCol.implicitHeight + 44)
            anchors.centerIn: parent
            radius: 12
            clip: true
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent

            MouseArea { anchors.fill: parent }

            Flickable {
                id: serverConfigFlickable
                anchors.fill: parent
                anchors.margins: 22
                contentWidth: width
                contentHeight: serverConfigCol.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    policy: serverConfigFlickable.contentHeight > serverConfigFlickable.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                }

                ColumnLayout {
                    id: serverConfigCol
                    width: serverConfigFlickable.width
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Text {
                            id: settingsModalTitle
                            objectName: "settingsModalTitle"
                            Layout.fillWidth: true
                            text: "⚙ " + i18n.tr("Settings")
                            font.pixelSize: theme.fontSize + 2
                            font.bold: true
                            color: theme.brightForeground
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            height: 22
                            radius: 6
                            implicitWidth: cfgStatusBadge.implicitWidth + 14
                            color: backend.connected
                                       ? Qt.rgba(48/255, 209/255, 88/255, 0.16)
                                       : Qt.rgba(theme.red.r, theme.red.g, theme.red.b, 0.16)
                            border.width: 1
                            border.color: backend.connected ? theme.green : theme.red
                            Text {
                                id: cfgStatusBadge
                                objectName: "cfgStatusBadge"
                                anchors.centerIn: parent
                                text: backend.connected ? i18n.tr("Connected") : i18n.tr("Disconnected")
                                font.pixelSize: theme.fontSize - 3
                                font.bold: true
                                color: backend.connected ? theme.green : theme.red
                            }
                        }
                    }

                    // Always-Visible Client & Relay Version / Connection Status Banner
                    Rectangle {
                        id: settingsVersionBanner
                        objectName: "settingsVersionBanner"
                        Layout.fillWidth: true
                        implicitHeight: settingsVersionCol.implicitHeight + 18
                        radius: 8
                        color: theme.lighterBackground
                        border.width: 1
                        border.color: backend.connected ? Qt.rgba(48/255, 209/255, 88/255, 0.45) : theme.muted

                        ColumnLayout {
                            id: settingsVersionCol
                            anchors.fill: parent
                            anchors.margins: 9
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    id: settingsClientVersionText
                                    objectName: "settingsClientVersionText"
                                    text: "BluePill v" + (Qt.application.version || "1.2.3")
                                    font.pixelSize: theme.fontSize - 2
                                    font.bold: true
                                    color: theme.accent
                                }
                                Text {
                                    text: "·"
                                    font.pixelSize: theme.fontSize - 2
                                    color: theme.darkForeground
                                }
                                Text {
                                    id: settingsRelayStatusText
                                    objectName: "settingsRelayStatusText"
                                    Layout.fillWidth: true
                                    text: backend.serverStatus
                                    font.pixelSize: theme.fontSize - 2
                                    font.bold: true
                                    color: backend.connected ? theme.brightForeground : theme.red
                                    elide: Text.ElideRight
                                }
                            }

                            Text {
                                id: settingsEndpointText
                                objectName: "settingsEndpointText"
                                Layout.fillWidth: true
                                text: i18n.tr("Endpoint:") + " " + (backend.serverUrl && backend.serverUrl.length > 0 ? backend.serverUrl : i18n.tr("Not configured"))
                                font.pixelSize: theme.fontSize - 3
                                color: theme.darkForeground
                                elide: Text.ElideMiddle
                            }
                        }
                    }

                    // Settings Section Tabs (All / Server & Sync / UI & Appearance / Notifications & Audio)
                    RowLayout {
                        id: settingsSectionTabsRow
                        objectName: "settingsSectionTabsRow"
                        Layout.fillWidth: true
                        spacing: 6

                        Repeater {
                            model: [
                                { key: "all", label: "☰ " + i18n.tr("All") },
                                { key: "server", label: "🌐 " + i18n.tr("Server & Sync") },
                                { key: "appearance", label: "🎨 " + i18n.tr("UI & Appearance") },
                                { key: "notifications", label: "🔔 " + i18n.tr("Notifications & Audio") }
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                objectName: "settingsTab_" + modelData.key
                                Layout.fillWidth: true
                                height: 28
                                radius: 6
                                readonly property bool isCurrentTab: root.settingsActiveTab === modelData.key
                                color: isCurrentTab ? theme.accent : theme.lighterBackground
                                border.width: 1
                                border.color: isCurrentTab ? theme.accent : theme.muted
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.label
                                    font.pixelSize: theme.fontSize - 3
                                    font.bold: isCurrentTab
                                    color: isCurrentTab ? theme.darkerBackground : theme.foreground
                                    elide: Text.ElideRight
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.settingsActiveTab = modelData.key
                                }
                            }
                        }
                    }

                ColumnLayout {
                    id: serverSyncSection
                    objectName: "serverSyncSection"
                    visible: root.settingsActiveTab === "all" || root.settingsActiveTab === "server"
                    Layout.fillWidth: true
                    spacing: 12

                Text {
                    Layout.fillWidth: true
                    text: i18n.tr("Connect to a BluePill macOS Relay (brew install nertwork/tap/bluepill), a legacy BlueBubbles Server (https://...), or Direct SSH (ssh://user@mac). Without a scheme, http:// is used; if no port is given, 1235 is used. Credentials are saved with 0600 permissions to ~/.config/bluepill/config.json.")
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                    wrapMode: Text.WordWrap
                }

                // Tailscale relay discovery
                ColumnLayout {
                    id: tailscaleDiscoverySection
                    objectName: "tailscaleDiscoverySection"
                    Layout.fillWidth: true
                    spacing: 6
                    visible: backend.tailscaleStatus !== "" && backend.tailscaleStatus !== "not-installed"

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: i18n.tr("Macs on your Tailscale network")
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: theme.foreground
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            id: tailscaleRescanBtn
                            objectName: "tailscaleRescanBtn"
                            height: 24
                            implicitWidth: rescanTxt.implicitWidth + 16
                            radius: 6
                            color: theme.lighterBackground
                            border.width: 1
                            opacity: backend.tailscaleScanning ? 0.5 : 1.0
                            activeFocusOnTab: true
                            border.color: activeFocus ? theme.accent : theme.muted
                            Accessible.role: Accessible.Button
                            Accessible.name: rescanTxt.text
                            Keys.onReturnPressed: if (!backend.tailscaleScanning) backend.discoverTailscaleRelays()
                            Keys.onEnterPressed: if (!backend.tailscaleScanning) backend.discoverTailscaleRelays()
                            Keys.onSpacePressed: if (!backend.tailscaleScanning) backend.discoverTailscaleRelays()
                            Text {
                                id: rescanTxt
                                anchors.centerIn: parent
                                text: backend.tailscaleScanning ? i18n.tr("Scanning…") : i18n.tr("Rescan")
                                font.pixelSize: theme.fontSize - 3
                                color: theme.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                enabled: !backend.tailscaleScanning
                                onClicked: backend.discoverTailscaleRelays()
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: backend.tailscaleStatus === "stopped"
                              ? i18n.tr("Tailscale is installed but not connected. Run `tailscale up`, then Rescan.")
                              : backend.tailscaleStatus === "no-macs"
                                ? i18n.tr("No online Macs found on your tailnet. Make sure the relay Mac is signed in to Tailscale.")
                                : backend.tailscaleStatus === "error"
                                  ? i18n.tr("Couldn't read Tailscale status.")
                                  : backend.tailscaleStatus === "scanning"
                                    ? i18n.tr("Looking for Macs…")
                                    : ""
                        font.pixelSize: theme.fontSize - 3
                        color: theme.darkForeground
                        wrapMode: Text.WordWrap
                    }

                    Connections {
                        target: backend
                        function onTailscaleRelaysChanged() {
                            // First run: a single responding relay on the tailnet is
                            // almost certainly the one to use, so pre-fill it.
                            if (backend.tailscaleScanning || !root.showServerConfigModal) return
                            if (backend.configuredServerUrl.length > 0 || root.primaryServerFieldsEdited) return
                            if (serverUrlInput.text.trim().length > 0) return
                            var found = backend.tailscaleRelays.filter(function(r) { return r.probeState === "relay" })
                            if (found.length === 1) {
                                root.loadPrimaryServerFields(found[0].url)
                                serverPasswordInput.forceActiveFocus()
                            }
                        }
                    }

                    Repeater {
                        model: backend.tailscaleRelays
                        delegate: Rectangle {
                            readonly property bool isRelay: modelData.probeState === "relay"
                            readonly property string relayName: modelData.name
                            readonly property bool isSelected: root.composedPrimaryServerUrl() === modelData.url
                            objectName: "tailscaleRelayRow"
                            Layout.fillWidth: true
                            implicitHeight: relayRowCol.implicitHeight + 14
                            radius: 6
                            color: isSelected ? theme.selection : theme.lighterBackground
                            border.width: activeFocus ? 2 : 1
                            border.color: (isSelected || activeFocus) ? theme.accent : theme.muted
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.name + " " + modelData.url
                            // A pending row's URL may still change (IP fallback), so wait for its result.
                            readonly property bool isSelectable: modelData.probeState !== "pending"
                            opacity: isSelectable ? 1.0 : 0.6
                            function selectRelay() {
                                if (!isSelectable) return
                                root.loadPrimaryServerFields(modelData.url)
                                serverPasswordInput.forceActiveFocus()
                            }
                            Keys.onReturnPressed: selectRelay()
                            Keys.onEnterPressed: selectRelay()
                            Keys.onSpacePressed: selectRelay()
                            ColumnLayout {
                                id: relayRowCol
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 2
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        textFormat: Text.PlainText  // peer HostName is untrusted
                                        font.pixelSize: theme.fontSize - 1
                                        font.bold: true
                                        color: theme.brightForeground
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: modelData.probeState === "relay" ? i18n.tr("● Relay found")
                                              : modelData.probeState === "pending" ? i18n.tr("Checking…")
                                              : modelData.probeState === "other" ? i18n.tr("Port 1235 answers, but not as a BluePill relay")
                                              : i18n.tr("No relay on :1235")
                                        font.pixelSize: theme.fontSize - 3
                                        color: modelData.probeState === "relay" ? theme.green : theme.darkForeground
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.url
                                    textFormat: Text.PlainText
                                    font.pixelSize: theme.fontSize - 3
                                    color: theme.darkForeground
                                    elide: Text.ElideMiddle
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: modelData.probeState === "unreachable"
                                    text: i18n.tr("Is bluepill-server running with --host 0.0.0.0 (or this Mac's Tailscale IP)? By default it only listens on 127.0.0.1.")
                                    font.pixelSize: theme.fontSize - 3
                                    color: theme.darkForeground
                                    wrapMode: Text.WordWrap
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: parent.isSelectable ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: parent.selectRelay()
                            }
                        }
                    }
                }

                // Primary Server URL + Port
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    // ssh://... or scheme-less user@host (same rule as the backend)
                    readonly property bool isSshUrl: backend.isSshStyleServerUrl(serverUrlInput.text)
                    id: primaryServerFields
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: i18n.tr("Primary Server URL")
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: theme.foreground
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.preferredWidth: serverPortBox.width
                            text: i18n.tr("Port")
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: theme.foreground
                            elide: Text.ElideRight
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.fillWidth: true
                            height: Math.max(40, theme.fontSize + 20)
                            Layout.preferredHeight: Math.max(40, theme.fontSize + 20)
                            radius: 6
                            clip: true
                            color: theme.lighterBackground
                            border.width: 1
                            border.color: serverUrlInput.activeFocus ? theme.accent : theme.muted

                            TextInput {
                                id: serverUrlInput
                                objectName: "serverUrlInput"
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                clip: true
                                verticalAlignment: TextInput.AlignVCenter
                                color: theme.brightForeground
                                font.pixelSize: theme.fontSize
                                selectByMouse: true
                                KeyNavigation.tab: serverPortInput
                                onTextEdited: root.primaryServerFieldsEdited = true
                                onEditingFinished: {
                                    root.carryOverPrimaryServerPort()
                                    backend.verifyTailnetHostForUrl(root.composedPrimaryServerUrl())
                                }
                                Keys.onReturnPressed: {
                                    root.savePrimaryServerSettings()
                                }
                                Text {
                                    text: "mac-relay.example.com  (or https://relay.example.com)"
                                    color: theme.darkForeground
                                    visible: !serverUrlInput.text && !serverUrlInput.activeFocus
                                    anchors.verticalCenter: parent.verticalCenter
                                    font.pixelSize: theme.fontSize
                                    elide: Text.ElideRight
                                    width: parent.width
                                }
                            }
                        }
                        Rectangle {
                            id: serverPortBox
                            Layout.preferredWidth: Math.max(84, theme.fontSize * 6)
                            height: Math.max(40, theme.fontSize + 20)
                            Layout.preferredHeight: Math.max(40, theme.fontSize + 20)
                            radius: 6
                            clip: true
                            color: theme.lighterBackground
                            border.width: 1
                            border.color: root.serverPortInvalid ? theme.red : (serverPortInput.activeFocus ? theme.accent : theme.muted)
                            opacity: primaryServerFields.isSshUrl ? 0.4 : 1.0

                            TextInput {
                                id: serverPortInput
                                objectName: "serverPortInput"
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                clip: true
                                enabled: !primaryServerFields.isSshUrl
                                verticalAlignment: TextInput.AlignVCenter
                                color: theme.brightForeground
                                font.pixelSize: theme.fontSize
                                selectByMouse: true
                                inputMethodHints: Qt.ImhDigitsOnly
                                maximumLength: 5
                                validator: IntValidator { bottom: 1; top: 65535 }
                                onTextEdited: { root.primaryServerFieldsEdited = true; root.serverPortInvalid = false }
                                KeyNavigation.tab: secondaryServerUrlInput
                                KeyNavigation.backtab: serverUrlInput
                                Keys.onReturnPressed: {
                                    root.savePrimaryServerSettings()
                                }
                                Text {
                                    // Greyed default shown whenever the field is empty.
                                    text: String(1235)
                                    color: theme.darkForeground
                                    visible: !serverPortInput.text
                                    anchors.verticalCenter: parent.verticalCenter
                                    font.pixelSize: theme.fontSize
                                }
                            }
                        }
                    }
                    Text {
                        objectName: "serverPortInvalidText"
                        Layout.fillWidth: true
                        visible: root.serverPortInvalid
                        text: i18n.tr("Port must be a number from 1 to 65535, or empty for 1235.")
                        font.pixelSize: theme.fontSize - 3
                        color: theme.red
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        objectName: "serverUrlCleartextWarning"
                        Layout.fillWidth: true
                        // Re-evaluated when a custom-domain Tailscale name is confirmed.
                        visible: serverUrlInput.text.trim().length > 0 && !primaryServerFields.isSshUrl
                                 && backend.verifiedTailnetHostsRevision >= 0
                                 && !backend.isPasswordTransportAllowed(root.composedPrimaryServerUrl())
                        text: i18n.tr("Plain http:// only works on localhost, Tailscale (a *.ts.net name, a Tailscale IP, or a name that resolves to a device on your tailnet), or a private LAN address. For a public host, use https:// (a TLS reverse proxy).")
                        font.pixelSize: theme.fontSize - 3
                        color: theme.red
                        wrapMode: Text.WordWrap
                    }
                }

                // Secondary / Failover Server URL
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Text {
                        Layout.fillWidth: true
                        text: i18n.tr("Failover Server URL (Optional)")
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: theme.foreground
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: Math.max(40, theme.fontSize + 20)
                        Layout.preferredHeight: Math.max(40, theme.fontSize + 20)
                        radius: 6
                        clip: true
                        color: theme.lighterBackground
                        border.width: 1
                        border.color: secondaryServerUrlInput.activeFocus ? theme.accent : theme.muted

                        TextInput {
                            id: secondaryServerUrlInput
                            objectName: "secondaryServerUrlInput"
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            clip: true
                            verticalAlignment: TextInput.AlignVCenter
                            text: backend.secondaryServerUrl
                            color: theme.brightForeground
                            font.pixelSize: theme.fontSize
                            selectByMouse: true
                            KeyNavigation.tab: serverPasswordInput
                            Keys.onReturnPressed: {
                                root.savePrimaryServerSettings()
                            }
                            Text {
                                text: "https://backup-relay.example.com"
                                color: theme.darkForeground
                                visible: !secondaryServerUrlInput.text && !secondaryServerUrlInput.activeFocus
                                anchors.verticalCenter: parent.verticalCenter
                                font.pixelSize: theme.fontSize
                                elide: Text.ElideRight
                                width: parent.width
                            }
                        }
                    }
                }

                // API Password / Key
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Text {
                        Layout.fillWidth: true
                        text: i18n.tr("API Password / Key")
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: theme.foreground
                        elide: Text.ElideRight
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.fillWidth: true
                            height: Math.max(40, theme.fontSize + 20)
                            Layout.preferredHeight: Math.max(40, theme.fontSize + 20)
                            radius: 6
                            clip: true
                            color: theme.lighterBackground
                            border.width: 1
                            border.color: serverPasswordInput.activeFocus ? theme.accent : theme.muted

                            TextInput {
                                id: serverPasswordInput
                                objectName: "serverPasswordInput"
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                clip: true
                                verticalAlignment: TextInput.AlignVCenter
                                text: backend.serverPassword
                                echoMode: root.showServerPassword ? TextInput.Normal : TextInput.Password
                                color: theme.brightForeground
                                font.pixelSize: theme.fontSize
                                selectByMouse: true
                                Keys.onReturnPressed: {
                                    root.savePrimaryServerSettings()
                                }
                                Text {
                                    text: backend.hasPasswordConfigured
                                          ? i18n.tr("•••••••• (Configured — leave blank to keep existing)")
                                          : i18n.tr("Enter BlueBubbles Server Password / API Key")
                                    color: theme.darkForeground
                                    visible: !serverPasswordInput.text && !serverPasswordInput.activeFocus
                                    anchors.verticalCenter: parent.verticalCenter
                                    font.pixelSize: theme.fontSize - 1
                                    elide: Text.ElideRight
                                    width: parent.width
                                }
                            }
                        }
                        Rectangle {
                            id: toggleShowPasswordBtn
                            objectName: "toggleShowPasswordBtn"
                            height: Math.max(40, theme.fontSize + 20)
                            implicitWidth: showPwdTxt.implicitWidth + 20
                            radius: 6
                            color: theme.lighterBackground
                            border.width: 1
                            border.color: theme.muted
                            Text {
                                id: showPwdTxt
                                anchors.centerIn: parent
                                text: root.showServerPassword ? i18n.tr("Hide") : i18n.tr("Show")
                                font.pixelSize: theme.fontSize - 2
                                color: theme.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.showServerPassword = !root.showServerPassword
                            }
                        }
                    }
                }

                // Optional iCloud Hub Feature Modules
                ColumnLayout {
                    id: icloudModulesSettingsCol
                    objectName: "icloudModulesSettingsCol"
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        Layout.fillWidth: true
                        text: i18n.tr("iCloud Hub Modules (Optional)")
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: theme.foreground
                        elide: Text.ElideRight
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            id: toggleNotesModuleBtn
                            objectName: "toggleNotesModuleBtn"
                            Layout.fillWidth: true
                            height: 34
                            radius: 6
                            color: backend.icloudNotesEnabled ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.icloudNotesEnabled ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.icloudNotesEnabled ? "✓ " : "○ ") + "📝 " + i18n.tr("Notes")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.icloudNotesEnabled
                                color: backend.icloudNotesEnabled ? theme.brightForeground : theme.darkForeground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.icloudNotesEnabled = !backend.icloudNotesEnabled
                            }
                        }

                        Rectangle {
                            id: toggleCalendarModuleBtn
                            objectName: "toggleCalendarModuleBtn"
                            Layout.fillWidth: true
                            height: 34
                            radius: 6
                            color: backend.icloudCalendarEnabled ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.icloudCalendarEnabled ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.icloudCalendarEnabled ? "✓ " : "○ ") + "📅 " + i18n.tr("Calendar")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.icloudCalendarEnabled
                                color: backend.icloudCalendarEnabled ? theme.brightForeground : theme.darkForeground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.icloudCalendarEnabled = !backend.icloudCalendarEnabled
                            }
                        }

                        Rectangle {
                            id: togglePhotosModuleBtn
                            objectName: "togglePhotosModuleBtn"
                            Layout.fillWidth: true
                            height: 34
                            radius: 6
                            color: backend.icloudPhotosEnabled ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.icloudPhotosEnabled ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.icloudPhotosEnabled ? "✓ " : "○ ") + "☁️ " + i18n.tr("Photos")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.icloudPhotosEnabled
                                color: backend.icloudPhotosEnabled ? theme.brightForeground : theme.darkForeground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.icloudPhotosEnabled = !backend.icloudPhotosEnabled
                            }
                        }
                    }
                }

                // Message history and relay tools (moved here from the old Home page
                // and the top bar's "60d" pill)
                ColumnLayout {
                    id: historySettingsCol
                    objectName: "historySettingsCol"
                    Layout.fillWidth: true
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: "🕒 " + i18n.tr("Message History Sync Window")
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: theme.brightForeground
                        }
                        Repeater {
                            model: [
                                { label: "🌐 " + i18n.tr("Web UI"), name: "settingsWebUiBtn", run: function() { backend.openWebUi() } }
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                objectName: modelData.name
                                height: 26
                                radius: 6
                                implicitWidth: toolLbl.implicitWidth + 18
                                color: toolMouse.containsMouse ? theme.selection : theme.lighterBackground
                                border.width: activeFocus ? 2 : 1
                                border.color: (toolMouse.containsMouse || activeFocus) ? theme.accent : theme.muted
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: modelData.label
                                Accessible.onPressAction: modelData.run()
                                Keys.onReturnPressed: modelData.run()
                                Keys.onEnterPressed: modelData.run()
                                Keys.onSpacePressed: modelData.run()
                                Text {
                                    id: toolLbl
                                    anchors.centerIn: parent
                                    text: modelData.label
                                    font.pixelSize: theme.fontSize - 3
                                    font.bold: true
                                    color: theme.accent
                                }
                                MouseArea {
                                    id: toolMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: modelData.run()
                                }
                            }
                        }
                    }

                    ThemedComboBox {
                        id: historySyncWindowCombo
                        objectName: "historySyncWindowChoices"
                        Layout.preferredWidth: 180
                        Accessible.name: i18n.tr("Message History Sync Window")
                        textRole: "label"
                        valueRole: "days"
                        readonly property var dayChoices: {
                            const presets = [7, 14, 30, 60, 90, 180, 365]
                            const cur = backend.historySyncDays
                            return presets.indexOf(cur) >= 0 || cur <= 0 ? presets
                                 : presets.concat([cur]).sort(function(a, b) { return a - b })
                        }
                        // Presets, plus the current value if it was set to something else
                        model: dayChoices.map(function(d) {
                            return { days: d, label: d === 365 ? i18n.tr("1 year") : i18n.tr("%1 days").arg(d) }
                        })
                        currentIndex: Math.max(0, dayChoices.indexOf(backend.historySyncDays))
                        displayText: {
                            const d = (currentIndex >= 0 && currentIndex < dayChoices.length) ? dayChoices[currentIndex] : backend.historySyncDays
                            return d === 365 ? i18n.tr("1 year") : i18n.tr("%1 days").arg(d > 0 ? d : 60)
                        }
                        onModelChanged: {
                            const idx = dayChoices.indexOf(backend.historySyncDays)
                            if (idx >= 0 && currentIndex !== idx) currentIndex = idx
                        }
                        Connections {
                            target: backend
                            function onHistorySyncDaysChanged() {
                                const idx = historySyncWindowCombo.dayChoices.indexOf(backend.historySyncDays)
                                if (idx >= 0) historySyncWindowCombo.currentIndex = idx
                            }
                        }
                        onActivated: function(index) {
                            if (index >= 0 && index < dayChoices.length) {
                                backend.setHistorySyncDays(dayChoices[index])
                            } else {
                                backend.setHistorySyncDays(currentValue)
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: i18n.tr("How far back conversations are synced from the Mac.")
                        font.pixelSize: theme.fontSize - 3
                        color: theme.darkForeground
                        wrapMode: Text.WordWrap
                    }
                }
                } // end serverSyncSection

                // UI & Appearance Settings Section
                ColumnLayout {
                    id: uiAppearanceSection
                    objectName: "uiAppearanceSection"
                    visible: root.settingsActiveTab === "all" || root.settingsActiveTab === "appearance"
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: "🎨 " + i18n.tr("UI & Appearance")
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: theme.foreground
                        elide: Text.ElideRight
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        rowSpacing: 6
                        columnSpacing: 8

                        Rectangle {
                            id: toggleTopBarStatusPillBtn
                            objectName: "toggleTopBarStatusPillBtn"
                            Layout.fillWidth: true
                            height: 34
                            radius: 6
                            color: backend.showTopBarStatusPill ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.showTopBarStatusPill ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.showTopBarStatusPill ? "✓ " : "○ ") + i18n.tr("Top Bar Status Pill")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.showTopBarStatusPill
                                color: backend.showTopBarStatusPill ? theme.brightForeground : theme.darkForeground
                            }
                            ToolTip.visible: topBarPillToggleMouse.containsMouse
                            ToolTip.text: i18n.tr("Show connection & version status pill in top bar")
                            ToolTip.delay: 300
                            MouseArea {
                                id: topBarPillToggleMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.showTopBarStatusPill = !backend.showTopBarStatusPill
                            }
                        }

                        Rectangle {
                            id: toggleShowPinnedRowBtn
                            objectName: "toggleShowPinnedRowBtn"
                            Layout.fillWidth: true
                            height: 34
                            radius: 6
                            color: backend.showPinnedRow ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.showPinnedRow ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.showPinnedRow ? "✓ 📌 " : "○ 📌 ") + i18n.tr("Show Pinned Row")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.showPinnedRow
                                color: backend.showPinnedRow ? theme.brightForeground : theme.darkForeground
                            }
                            ToolTip.visible: showPinnedRowToggleMouse.containsMouse
                            ToolTip.text: i18n.tr("Show pinned / quick-access row at top of sidebar")
                            ToolTip.delay: 300
                            MouseArea {
                                id: showPinnedRowToggleMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.showPinnedRow = !backend.showPinnedRow
                            }
                        }

                        Rectangle {
                            id: togglePinnedRowFillRecentBtn
                            objectName: "togglePinnedRowFillRecentBtn"
                            Layout.fillWidth: true
                            height: 34
                            radius: 6
                            opacity: backend.showPinnedRow ? 1.0 : 0.5
                            color: backend.pinnedRowFillRecent ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.pinnedRowFillRecent ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.pinnedRowFillRecent ? "✓ " : "○ ") + i18n.tr("Fill 4 Pinned Slots with Recent")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.pinnedRowFillRecent
                                color: backend.pinnedRowFillRecent ? theme.brightForeground : theme.darkForeground
                            }
                            ToolTip.visible: fillRecentToggleMouse.containsMouse
                            ToolTip.text: i18n.tr("Fill empty pinned slots with most recent conversations (up to 4)")
                            ToolTip.delay: 300
                            MouseArea {
                                id: fillRecentToggleMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.pinnedRowFillRecent = !backend.pinnedRowFillRecent
                            }
                        }

                        Rectangle {
                            id: toggleHoverMessageActionsBtn
                            objectName: "toggleHoverMessageActionsBtn"
                            Layout.fillWidth: true
                            height: 34
                            radius: 6
                            color: backend.hoverMessageActions ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.hoverMessageActions ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.hoverMessageActions ? "✓ 💬 " : "○ 💬 ") + i18n.tr("Tapback Actions on Hover")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.hoverMessageActions
                                color: backend.hoverMessageActions ? theme.brightForeground : theme.darkForeground
                            }
                            ToolTip.visible: hoverActionsToggleMouse.containsMouse
                            ToolTip.text: i18n.tr("Show Tapback & message actions on hover (in addition to right-click)")
                            ToolTip.delay: 300
                            MouseArea {
                                id: hoverActionsToggleMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.hoverMessageActions = !backend.hoverMessageActions
                            }
                        }
                    }
                }

                // Customizable Notifications & Audio Alert Settings
                ColumnLayout {
                    id: notificationsSettingsCol
                    objectName: "notificationsSettingsCol"
                    visible: root.settingsActiveTab === "all" || root.settingsActiveTab === "notifications"
                    Layout.fillWidth: true
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: "🔔 " + i18n.tr("Notifications & Audio Alerts")
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: theme.foreground
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            id: previewSoundBtn
                            objectName: "previewSoundBtn"
                            height: 26
                            radius: 6
                            implicitWidth: previewSoundLbl.implicitWidth + 16
                            color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18)
                            border.width: 1
                            border.color: theme.accent
                            Text {
                                id: previewSoundLbl
                                anchors.centerIn: parent
                                text: "🔊 " + i18n.tr("Preview Ding")
                                font.pixelSize: theme.fontSize - 3
                                font.bold: true
                                color: theme.accent
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.previewNotificationSound(backend.notificationSoundPreset)
                            }
                        }
                        Rectangle {
                            id: testNotificationBtn
                            objectName: "testNotificationBtn"
                            height: 26
                            radius: 6
                            implicitWidth: testNotifLbl.implicitWidth + 16
                            color: theme.lighterBackground
                            border.width: 1
                            border.color: theme.muted
                            Text {
                                id: testNotifLbl
                                anchors.centerIn: parent
                                text: "🔔 " + i18n.tr("Test Alert")
                                font.pixelSize: theme.fontSize - 3
                                font.bold: true
                                color: theme.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.sendTestNotification()
                            }
                        }
                    }

                    // Notification Behavior Toggles (2x2 Grid)
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        rowSpacing: 6
                        columnSpacing: 8

                        Rectangle {
                            id: toggleMasterNotificationsBtn
                            objectName: "toggleMasterNotificationsBtn"
                            Layout.fillWidth: true
                            height: 32
                            radius: 6
                            color: backend.notificationsEnabled ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.notificationsEnabled ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.notificationsEnabled ? "✓ 🔔 " : "○ 🔕 ") + i18n.tr("Enable Alerts")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.notificationsEnabled
                                color: backend.notificationsEnabled ? theme.brightForeground : theme.darkForeground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.notificationsEnabled = !backend.notificationsEnabled
                            }
                        }

                        Rectangle {
                            id: toggleBannerNotificationsBtn
                            objectName: "toggleBannerNotificationsBtn"
                            Layout.fillWidth: true
                            height: 32
                            radius: 6
                            color: backend.notificationBannerEnabled ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.notificationBannerEnabled ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.notificationBannerEnabled ? "✓ 💬 " : "○ 💬 ") + i18n.tr("Desktop Banner")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.notificationBannerEnabled
                                color: backend.notificationBannerEnabled ? theme.brightForeground : theme.darkForeground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.notificationBannerEnabled = !backend.notificationBannerEnabled
                            }
                        }

                        Rectangle {
                            id: toggleAudioDingBtn
                            objectName: "toggleAudioDingBtn"
                            Layout.fillWidth: true
                            height: 32
                            radius: 6
                            color: backend.notificationSoundEnabled ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.notificationSoundEnabled ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.notificationSoundEnabled ? "✓ 🔊 " : "○ 🔇 ") + i18n.tr("Audio Ding")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.notificationSoundEnabled
                                color: backend.notificationSoundEnabled ? theme.brightForeground : theme.darkForeground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.notificationSoundEnabled = !backend.notificationSoundEnabled
                            }
                        }

                        Rectangle {
                            id: toggleMuteActiveChatBtn
                            objectName: "toggleMuteActiveChatBtn"
                            Layout.fillWidth: true
                            height: 32
                            radius: 6
                            color: backend.notificationMuteWhenActive ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : theme.lighterBackground
                            border.width: 1
                            border.color: backend.notificationMuteWhenActive ? theme.accent : theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: (backend.notificationMuteWhenActive ? "✓ 👀 " : "○ 👀 ") + i18n.tr("Mute Focused Chat")
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.notificationMuteWhenActive
                                color: backend.notificationMuteWhenActive ? theme.brightForeground : theme.darkForeground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.notificationMuteWhenActive = !backend.notificationMuteWhenActive
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: i18n.tr("Audio Ding Sound Preset (click any preset to select & preview):")
                        font.pixelSize: theme.fontSize - 3
                        color: theme.darkForeground
                        elide: Text.ElideRight
                    }

                    // Audio Ding Presets (3x2 Grid with instant preview)
                    GridLayout {
                        id: soundPresetGrid
                        objectName: "soundPresetGrid"
                        Layout.fillWidth: true
                        columns: 3
                        rowSpacing: 6
                        columnSpacing: 8

                        Repeater {
                            model: backend.notificationSoundPresets
                            delegate: Rectangle {
                                required property var modelData
                                objectName: "soundPresetBtn_" + modelData.id
                                Layout.fillWidth: true
                                height: 34
                                radius: 6
                                readonly property bool isSelected: backend.notificationSoundPreset === modelData.id
                                color: isSelected ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.24) : theme.lighterBackground
                                border.width: 1
                                border.color: isSelected ? theme.accent : theme.muted

                                RowLayout {
                                    anchors.centerIn: parent
                                    spacing: 4
                                    Text {
                                        text: isSelected ? "🔊" : "♪"
                                        font.pixelSize: theme.fontSize - 2
                                        color: isSelected ? theme.accent : theme.darkForeground
                                    }
                                    Text {
                                        text: modelData.label
                                        font.pixelSize: theme.fontSize - 3
                                        font.bold: isSelected
                                        color: isSelected ? theme.brightForeground : theme.foreground
                                        elide: Text.ElideRight
                                    }
                                }

                                ToolTip.visible: presetMouse.containsMouse
                                ToolTip.text: modelData.desc + " — " + i18n.tr("Click to select & preview")
                                ToolTip.delay: 300

                                MouseArea {
                                    id: presetMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        backend.notificationSoundPreset = modelData.id
                                        backend.previewNotificationSound(modelData.id)
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        text: i18n.tr("Audio Devices (Voice Memos & Alerts)")
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: theme.darkForeground
                        Layout.topMargin: 4
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Text {
                                text: i18n.tr("Microphone Input")
                                font.pixelSize: theme.fontSize - 3
                                color: theme.darkForeground
                            }
                            ThemedComboBox {
                                id: audioInputDeviceCombo
                                objectName: "audioInputDeviceCombo"
                                Layout.fillWidth: true
                                model: backend.audioInputDevices
                                textRole: "label"
                                valueRole: "id"
                                currentIndex: root.findAudioDeviceIndex(backend.audioInputDevices, backend.selectedAudioInputDevice)
                                onActivated: {
                                    if (currentIndex >= 0 && currentIndex < backend.audioInputDevices.length) {
                                        backend.selectedAudioInputDevice = backend.audioInputDevices[currentIndex].id || ""
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Text {
                                text: i18n.tr("Speaker / Output")
                                font.pixelSize: theme.fontSize - 3
                                color: theme.darkForeground
                            }
                            ThemedComboBox {
                                id: audioOutputDeviceCombo
                                objectName: "audioOutputDeviceCombo"
                                Layout.fillWidth: true
                                model: backend.audioOutputDevices
                                textRole: "label"
                                valueRole: "id"
                                currentIndex: root.findAudioDeviceIndex(backend.audioOutputDevices, backend.selectedAudioOutputDevice)
                                onActivated: {
                                    if (currentIndex >= 0 && currentIndex < backend.audioOutputDevices.length) {
                                        backend.selectedAudioOutputDevice = backend.audioOutputDevices[currentIndex].id || ""
                                    }
                                }
                            }
                        }

                        Rectangle {
                            objectName: "refreshAudioDevicesBtn"
                            Layout.preferredWidth: 34
                            Layout.preferredHeight: 34
                            Layout.alignment: Qt.AlignBottom
                            radius: 6
                            color: refreshAudioMouse.containsMouse ? theme.selection : theme.lighterBackground
                            border.width: 1
                            border.color: theme.muted
                            Text {
                                anchors.centerIn: parent
                                text: "↻"
                                font.pixelSize: theme.fontSize + 1
                                color: theme.brightForeground
                            }
                            ToolTip.visible: refreshAudioMouse.containsMouse
                            ToolTip.text: i18n.tr("Refresh Audio Devices")
                            ToolTip.delay: 300
                            MouseArea {
                                id: refreshAudioMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    backend.refreshAudioDevices()
                                    root.reconcileAudioDeviceSelections()
                                    root.showToast("↻ " + i18n.tr("Audio devices refreshed"))
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Item { Layout.fillWidth: true }

                    Rectangle {
                        id: cancelServerConfigBtn
                        height: 34
                        implicitWidth: 86
                        radius: 7
                        color: theme.lighterBackground
                        border.width: 1
                        border.color: theme.muted
                        Text {
                            anchors.centerIn: parent
                            text: i18n.tr("Cancel")
                            color: theme.foreground
                            font.pixelSize: theme.fontSize - 1
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.showServerConfigModal = false
                        }
                    }

                    Rectangle {
                        id: saveServerConfigBtn
                        objectName: "saveServerConfigBtn"
                        height: 34
                        implicitWidth: 140
                        radius: 7
                        color: theme.accent
                        Text {
                            anchors.centerIn: parent
                            text: i18n.tr("Save & Connect")
                            color: theme.darkerBackground
                            font.pixelSize: theme.fontSize - 1
                            font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.savePrimaryServerSettings()
                            }
                        }
                    }
                }
                }
            }
        }
    }

    // Keyboard Shortcuts Modal Overlay (Ctrl+K / F1 / ?)
    Rectangle {
        visible: root.showShortcutsModal
        anchors.fill: parent
        color: "#aa0e0e14"
        z: 998

        MouseArea {
            anchors.fill: parent
            onClicked: root.showShortcutsModal = false
        }

        Rectangle {
            width: 500
            height: 520
            anchors.centerIn: parent
            radius: theme.radius + 2
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 22
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: i18n.tr("Keyboard Shortcuts")
                        font.pixelSize: theme.fontSize + 3
                        font.bold: true
                        color: theme.brightForeground
                        Layout.fillWidth: true
                    }
                    Text {
                        text: "Esc to close"
                        font.pixelSize: theme.fontSize - 2
                        color: theme.darkForeground
                    }
                }

                Item { height: 4 }

                Repeater {
                    model: [
                        { keys: "Ctrl+H / Esc", desc: "Close the open conversation" },
                        { keys: "Ctrl+K / F1", desc: "Toggle this keyboard shortcuts dialog" },
                        { keys: "Ctrl+F / /", desc: "Focus conversation search filter" },
                        { keys: "Alt+Down / Ctrl+J", desc: "Next conversation in sidebar" },
                        { keys: "Alt+Up", desc: "Previous conversation in sidebar" },
                        { keys: "Alt+1 .. Alt+9", desc: "Jump directly to conversation 1–9" },
                        { keys: "Ctrl+N", desc: "Start new conversation" },
                        { keys: "Ctrl+Shift+C", desc: "Add / edit active contact" },
                        { keys: "Ctrl+E", desc: "Toggle emoji picker palette" },
                        { keys: "Ctrl+U", desc: "Toggle unread filter" },
                        { keys: "Ctrl+Shift+U / Alt+U", desc: "Toggle read/unread on active chat" },
                        { keys: "Ctrl+Shift+A", desc: "Mark all conversations as read" },
                        { keys: "Ctrl+Shift+R", desc: "Mark active chat as read" },
                        { keys: "Right-click chat", desc: "Toggle read/unread directly" },
                        { keys: "Ctrl+M", desc: "Mute / unmute active chat" },
                        { keys: "Ctrl+R / F5", desc: "Sync contacts and poll recent" },
                        { keys: "PageUp / PageDown", desc: "Scroll message timeline up / down" },
                        { keys: "Home / End", desc: "Jump to top / bottom of messages" },
                        { keys: ":shortcode + Tab", desc: "Complete emoji shortcode or emoticon" },
                        { keys: "Double-click bubble", desc: "Quick reaction (❤️ Love)" },
                        { keys: "Esc", desc: "Dismiss popup, pin, or search" },
                        { keys: "Ctrl+Q", desc: "Quit application" }
                    ]
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Text {
                            text: modelData.keys
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: theme.accent
                            Layout.preferredWidth: 175
                        }
                        Text {
                            text: modelData.desc
                            font.pixelSize: theme.fontSize - 2
                            color: theme.foreground
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }

    // Custom Emoji Tapback Picker Popover (+ button in message action bar)
    Item {
        id: tapbackEmojiPickerPopup
        objectName: "tapbackEmojiPickerPopup"
        visible: root.showTapbackEmojiPicker && root.tapbackEmojiTargetGuid.length > 0
        anchors.fill: parent
        z: 1004

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: root.closeTapbackEmojiPicker()
        }

        Rectangle {
            id: tapbackEmojiCard
            objectName: "tapbackEmojiCard"
            x: root.tapbackEmojiPopupX
            y: root.tapbackEmojiPopupY
            width: 348
            height: 276
            radius: 12
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent

            MouseArea { anchors.fill: parent }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Rectangle {
                        Layout.fillWidth: true
                        height: 32
                        radius: 6
                        color: theme.background
                        border.width: 1
                        border.color: tapbackEmojiSearchField.activeFocus ? theme.accent : theme.muted

                        TextField {
                            id: tapbackEmojiSearchField
                            objectName: "tapbackEmojiSearchField"
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            topPadding: 4
                            bottomPadding: 6
                            verticalAlignment: TextInput.AlignVCenter
                            placeholderText: i18n.tr("Search reaction emoji (:fire:, party)...")
                            placeholderTextColor: theme.darkForeground
                            color: theme.brightForeground
                            font.pixelSize: theme.fontSize - 2
                            background: Item {}
                            text: root.tapbackEmojiQuery
                            onTextChanged: root.tapbackEmojiQuery = text

                            Keys.onReturnPressed: function(event) {
                                event.accepted = true
                                var results = backend.searchEmojis(root.tapbackEmojiQuery, root.tapbackEmojiCategory, 1)
                                if (results.length > 0) {
                                    root.selectTapbackCustomEmoji(results[0].emoji)
                                }
                            }
                            Keys.onEscapePressed: function(event) {
                                event.accepted = true
                                root.closeTapbackEmojiPicker()
                            }
                        }
                    }

                    Text {
                        text: "✕"
                        font.pixelSize: theme.fontSize
                        color: theme.darkForeground
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.closeTapbackEmojiPicker()
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: ["All", "Smileys", "Gestures", "Hearts", "Activities", "Symbols"]
                        delegate: Rectangle {
                            required property string modelData
                            height: 22
                            radius: 5
                            implicitWidth: tbCatLbl.implicitWidth + 10
                            color: root.tapbackEmojiCategory === modelData ? theme.accent : theme.lighterBackground
                            Text {
                                id: tbCatLbl
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: theme.fontSize - 4
                                font.bold: root.tapbackEmojiCategory === modelData
                                color: root.tapbackEmojiCategory === modelData ? theme.darkerBackground : theme.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.tapbackEmojiCategory = modelData
                            }
                        }
                    }
                }

                GridView {
                    id: tapbackEmojiGridView
                    objectName: "tapbackEmojiGridView"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    cellWidth: 40
                    cellHeight: 36
                    clip: true
                    model: backend.searchEmojis(root.tapbackEmojiQuery, root.tapbackEmojiCategory, 120)

                    delegate: Rectangle {
                        required property var modelData
                        width: 36
                        height: 32
                        radius: 6
                        color: tbEmojiCellMouse.containsMouse ? theme.selection : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: modelData.emoji
                            font.pixelSize: theme.fontSize + 4
                        }

                        MouseArea {
                            id: tbEmojiCellMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectTapbackCustomEmoji(modelData.emoji)
                        }

                        ToolTip.visible: tbEmojiCellMouse.containsMouse
                        ToolTip.text: modelData.shortcode
                        ToolTip.delay: 300
                    }
                }
            }
        }
    }

    // Conversation Right-Click Context Menu Popup
    Item {
        id: chatContextMenuPopup
        anchors.fill: parent
        visible: false
        z: 990

        property string targetChatGuid: ""
        property string targetChatTitle: ""
        property string targetAddress: ""
        property int targetUnreadCount: 0
        property bool targetIsMuted: false
        property bool targetIsGroup: false
        // Live: a Mac-pin refresh while the menu is open updates the label it acts on
        property bool targetIsPinned: (void(backend.pinnedChats), backend.isChatPinned(targetChatGuid))
        property real menuX: 100
        property real menuY: 100

        function openForChat(chatObj, gx, gy) {
            targetChatGuid = chatObj.guid || ""
            targetChatTitle = chatObj.title || ""
            targetAddress = chatObj.address || ""
            targetUnreadCount = chatObj.unreadCount || 0
            targetIsMuted = chatObj.isMuted || false
            targetIsGroup = chatObj.isGroup || false
            menuX = Math.min(Math.max(12, gx), root.width - 252)
            menuY = Math.min(Math.max(12, gy), root.height - 266)
            visible = true
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: chatContextMenuPopup.visible = false
        }

        Rectangle {
            id: chatContextMenuCard
            x: chatContextMenuPopup.menuX
            y: chatContextMenuPopup.menuY
            width: 236
            height: contextMenuCol.implicitHeight + 12
            radius: 10
            color: theme.darkerBackground
            border.width: 1
            border.color: theme.muted

            ColumnLayout {
                id: contextMenuCol
                anchors.fill: parent
                anchors.margins: 6
                spacing: 2

                // 1. Mark as Read / Mark as Unread
                Rectangle {
                    Layout.fillWidth: true
                    height: 32
                    radius: 6
                    color: ctxReadMouse.containsMouse ? theme.selection : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        text: chatContextMenuPopup.targetUnreadCount > 0
                              ? ("󰇮  " + i18n.tr("Mark as Read"))
                              : ("󰇰  " + i18n.tr("Mark as Unread"))
                        font.pixelSize: theme.fontSize - 1
                        color: theme.brightForeground
                    }
                    MouseArea {
                        id: ctxReadMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var g = chatContextMenuPopup.targetChatGuid
                            var wasUnread = chatContextMenuPopup.targetUnreadCount > 0
                            chatContextMenuPopup.visible = false
                            if (g.length > 0) {
                                backend.toggleChatReadUnread(g)
                                root.showToast(wasUnread
                                               ? ("✓ " + i18n.tr("Marked conversation as read"))
                                               : ("● " + i18n.tr("Marked conversation as unread")))
                            }
                        }
                    }
                }

                // 2. Mute / Unmute Notifications
                Rectangle {
                    Layout.fillWidth: true
                    height: 32
                    radius: 6
                    color: ctxMuteMouse.containsMouse ? theme.selection : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        text: chatContextMenuPopup.targetIsMuted
                              ? ("󰂚  " + i18n.tr("Unmute Conversation"))
                              : ("󰂛  " + i18n.tr("Mute Notifications"))
                        font.pixelSize: theme.fontSize - 1
                        color: theme.brightForeground
                    }
                    MouseArea {
                        id: ctxMuteMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var g = chatContextMenuPopup.targetChatGuid
                            chatContextMenuPopup.visible = false
                            if (g.length > 0) {
                                backend.toggleChatMute(g)
                            }
                        }
                    }
                }

                // Pin / Unpin (pins from the Mac show first in the sidebar row)
                Rectangle {
                    objectName: "ctxPinItem"
                    Layout.fillWidth: true
                    height: 32
                    radius: 6
                    color: ctxPinMouse.containsMouse ? theme.selection : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        text: chatContextMenuPopup.targetIsPinned
                              ? ("󰐄  " + i18n.tr("Unpin conversation"))
                              : ("󰐃  " + i18n.tr("Pin conversation"))
                        font.pixelSize: theme.fontSize - 1
                        color: theme.brightForeground
                    }
                    MouseArea {
                        id: ctxPinMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var g = chatContextMenuPopup.targetChatGuid
                            chatContextMenuPopup.visible = false
                            if (g.length > 0) backend.togglePinChat(g)
                        }
                    }
                }

                // 3. Contact Info / Save Contact
                Rectangle {
                    Layout.fillWidth: true
                    height: 32
                    radius: 6
                    color: ctxContactMouse.containsMouse ? theme.selection : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        text: "󰋼  " + i18n.tr("Contact Info / Save Contact")
                        font.pixelSize: theme.fontSize - 1
                        color: theme.brightForeground
                    }
                    MouseArea {
                        id: ctxContactMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var g = chatContextMenuPopup.targetChatGuid
                            var t = chatContextMenuPopup.targetChatTitle
                            var addr = chatContextMenuPopup.targetAddress
                            chatContextMenuPopup.visible = false
                            if (g.length > 0) {
                                backend.selectChat(g)
                                root.openContactModal(addr.length > 0 ? addr : backend.selectedChatAddress, t)
                            }
                        }
                    }
                }

                // 4. Copy Phone / Address
                Rectangle {
                    Layout.fillWidth: true
                    height: 32
                    radius: 6
                    color: ctxCopyMouse.containsMouse ? theme.selection : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        text: "󰆏  " + i18n.tr("Copy Phone / Address")
                        font.pixelSize: theme.fontSize - 1
                        color: theme.brightForeground
                    }
                    MouseArea {
                        id: ctxCopyMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var addr = chatContextMenuPopup.targetAddress
                            var title = chatContextMenuPopup.targetChatTitle
                            chatContextMenuPopup.visible = false
                            backend.copyToClipboard(addr.length > 0 ? addr : title, "Address")
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: theme.lighterBackground
                    Layout.topMargin: 2
                    Layout.bottomMargin: 2
                }

                // 5. Delete Conversation (danger red)
                Rectangle {
                    id: ctxDeleteChatItem
                    Layout.fillWidth: true
                    height: 32
                    radius: 6
                    color: ctxDelMouse.containsMouse ? "#3b1d26" : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        text: "󰆴  " + i18n.tr("Delete Conversation")
                        font.pixelSize: theme.fontSize - 1
                        font.bold: true
                        color: "#f7768e"
                    }
                    MouseArea {
                        id: ctxDelMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var g = chatContextMenuPopup.targetChatGuid
                            chatContextMenuPopup.visible = false
                            if (g.length > 0) {
                                backend.deleteChat(g)
                            }
                        }
                    }
                }
            }
        }
    }

    // Delete Conversation Confirmation Modal Overlay
    Rectangle {
        id: deleteChatConfirmPopup
        visible: root.showDeleteChatModal
        anchors.fill: parent
        color: "#88000000"
        z: 995

        MouseArea {
            anchors.fill: parent
            onClicked: root.showDeleteChatModal = false
        }

        Rectangle {
            width: 440
            height: 210
            radius: theme.radius + 4
            color: theme.darkerBackground
            border.width: 1
            border.color: "#f7768e"
            anchors.centerIn: parent

            MouseArea { anchors.fill: parent }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 22
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: "󰆴  " + i18n.tr("Delete Conversation?")
                        font.pixelSize: theme.fontSize + 3
                        font.bold: true
                        color: "#f7768e"
                    }
                    Item { Layout.fillWidth: true }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: i18n.tr("Delete the entire conversation with ") + (root.pendingDeleteChatTitle || root.pendingDeleteChatGuid) + i18n.tr("? This removes the thread from imsg, the macOS Relay, and Messages in iCloud across your iPhone and other Apple devices.")
                    font.pixelSize: theme.fontSize - 1
                    color: theme.foreground
                    opacity: 0.9
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Item { Layout.fillWidth: true }

                    Rectangle {
                        height: 34
                        width: 96
                        radius: 8
                        color: cancelDelChatMouse.containsMouse ? theme.selection : theme.lighterBackground
                        border.width: 1
                        border.color: theme.muted
                        Text {
                            anchors.centerIn: parent
                            text: i18n.tr("Cancel")
                            font.pixelSize: theme.fontSize - 1
                            color: theme.foreground
                        }
                        MouseArea {
                            id: cancelDelChatMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.showDeleteChatModal = false
                        }
                    }

                    Rectangle {
                        id: confirmDeleteChatBtn
                        height: 34
                        width: 164
                        radius: 8
                        color: confirmDelChatMouse.containsMouse ? "#f7768e" : "#c53b53"
                        border.width: 1
                        border.color: "#f7768e"
                        Text {
                            anchors.centerIn: parent
                            text: "󰆴  " + i18n.tr("Delete Thread")
                            font.pixelSize: theme.fontSize - 1
                            font.bold: true
                            color: "#ffffff"
                        }
                        MouseArea {
                            id: confirmDelChatMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                var target = root.pendingDeleteChatGuid
                                root.showDeleteChatModal = false
                                root.pendingDeleteChatGuid = ""
                                root.pendingDeleteChatTitle = ""
                                if (target && target.length > 0) {
                                    backend.deleteChat(target)
                                }
                            }
                        }
                    }

                }
            }
        }
    }

    // In-App Photo Lightbox / Gallery Viewer Overlay
    Rectangle {
        id: photoViewerModal
        objectName: "photoViewerModal"
        visible: root.showPhotoViewer
        focus: root.showPhotoViewer
        activeFocusOnTab: false
        anchors.fill: parent
        color: "#e60b0d13"
        z: 998

        Keys.priority: Keys.BeforeItem
        Keys.onPressed: (event) => {
            root.handlePhotoViewerKey(event)
        }

        readonly property var currentPhoto: (root.photoViewerItems && root.photoViewerIndex >= 0 && root.photoViewerIndex < root.photoViewerItems.length)
                                            ? root.photoViewerItems[root.photoViewerIndex]
                                            : ({ guid: "", name: "", localUrl: "", previewUrl: "", sourceUrl: "", isCloudEvicted: false, canUnpurge: true, canRequestAttachment: true })

        Connections {
            target: backend
            function onAttachmentReady(guid, localUrl) {
                if (!root.showPhotoViewer || !guid || !localUrl) return
                var items = root.photoViewerItems ? root.photoViewerItems.slice() : []
                var changed = false
                for (var i = 0; i < items.length; ++i) {
                    if (items[i] && items[i].guid === guid && items[i].localUrl !== localUrl) {
                        var copy = Object.assign({}, items[i])
                        copy.localUrl = localUrl
                        items[i] = copy
                        changed = true
                    }
                }
                if (changed) {
                    root.photoViewerItems = items
                }
            }
        }

        // Backdrop click to close
        MouseArea {
            anchors.fill: parent
            onClicked: root.closePhotoViewer()
            onWheel: (wheel) => {
                if (wheel.angleDelta.y > 0) {
                    root.zoomPhotoViewer(0.2)
                } else if (wheel.angleDelta.y < 0) {
                    root.zoomPhotoViewer(-0.2)
                }
            }
        }

        // Compact Centered Lightbox Card (or Full Screen when toggled)
        Rectangle {
            id: photoViewerCard
            objectName: "photoViewerCard"
            anchors.centerIn: parent
            width: root.photoViewerFullScreen ? parent.width : Math.min(1040, Math.max(Math.min(360, parent.width - 20), root.width - 80))
            height: root.photoViewerFullScreen ? parent.height : Math.min(720, Math.max(Math.min(340, parent.height - 20), root.height - 80))
            radius: root.photoViewerFullScreen ? 0 : 12
            color: theme.darkBackground
            border.width: root.photoViewerFullScreen ? 0 : 1
            border.color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.45)
            clip: true

            MouseArea { anchors.fill: parent }

        // Top Header Bar (Filename, Index Counter, Zoom Controls, Send/Copy, Close Button)
        Rectangle {
            id: photoViewerTopBar
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 50
            color: "#cc131620"
            z: 5
            readonly property bool compactTopBar: photoViewerCard.width < 720

            MouseArea { anchors.fill: parent }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 6

                Text {
                    text: "📷 " + (photoViewerModal.currentPhoto.name || i18n.tr("Photo"))
                    font.pixelSize: theme.fontSize + 1
                    font.bold: true
                    color: theme.brightForeground
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                }

                Rectangle {
                    visible: root.photoViewerItems.length > 1
                    height: 24
                    implicitWidth: viewerCounterTxt.implicitWidth + 14
                    radius: 12
                    color: theme.selection
                    border.width: 1
                    border.color: theme.accent
                    Text {
                        id: viewerCounterTxt
                        anchors.centerIn: parent
                        text: (root.photoViewerIndex + 1) + " / " + root.photoViewerItems.length
                        font.pixelSize: theme.fontSize - 3
                        font.bold: true
                        color: theme.accent
                    }
                }

                // Zoom Out (-)
                Rectangle {
                    height: 30
                    width: 30
                    radius: 7
                    color: zoomOutMouse.containsMouse ? theme.selection : theme.lighterBackground
                    border.width: 1
                    border.color: theme.muted
                    Text {
                        anchors.centerIn: parent
                        text: "−"
                        font.pixelSize: theme.fontSize + 2
                        font.bold: true
                        color: theme.brightForeground
                    }
                    MouseArea {
                        id: zoomOutMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.zoomPhotoViewer(-0.25)
                    }
                }

                // Zoom Reset Pill (100%)
                Rectangle {
                    height: 30
                    implicitWidth: zoomPctTxt.implicitWidth + 14
                    radius: 7
                    color: zoomResetMouse.containsMouse ? theme.selection : theme.lighterBackground
                    border.width: 1
                    border.color: root.photoViewerZoom !== 1.0 ? theme.accent : theme.muted
                    Text {
                        id: zoomPctTxt
                        anchors.centerIn: parent
                        text: Math.round(root.photoViewerZoom * 100) + "%"
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: root.photoViewerZoom !== 1.0 ? theme.accent : theme.foreground
                    }
                    MouseArea {
                        id: zoomResetMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.photoViewerZoom = 1.0
                            root.photoViewerPanX = 0
                            root.photoViewerPanY = 0
                        }
                    }
                }

                // Zoom In (+)
                Rectangle {
                    height: 30
                    width: 30
                    radius: 7
                    color: zoomInMouse.containsMouse ? theme.selection : theme.lighterBackground
                    border.width: 1
                    border.color: theme.muted
                    Text {
                        anchors.centerIn: parent
                        text: "+"
                        font.pixelSize: theme.fontSize + 2
                        font.bold: true
                        color: theme.brightForeground
                    }
                    MouseArea {
                        id: zoomInMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.zoomPhotoViewer(0.25)
                    }
                }

                // Send to Active Chat Button
                Rectangle {
                    id: photoViewerSendBtn
                    objectName: "photoViewerSendBtn"
                    visible: backend.selectedChatGuid.length > 0 && ((!!photoViewerModal.currentPhoto.guid && !photoViewerModal.currentPhoto.isCloudEvicted) || (!!photoViewerModal.currentPhoto.localUrl && photoViewerModal.currentPhoto.localUrl.length > 0))
                    height: 30
                    implicitWidth: sendViewerTxt.implicitWidth + 16
                    radius: 7
                    color: sendViewerMouse.containsMouse ? theme.accent : theme.selection
                    border.width: 1
                    border.color: theme.accent
                    Text {
                        id: sendViewerTxt
                        anchors.centerIn: parent
                        text: photoViewerTopBar.compactTopBar ? "📤" : ("📤 " + i18n.tr("Send"))
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: theme.brightForeground
                    }
                    ToolTip.visible: sendViewerMouse.containsMouse
                    ToolTip.text: i18n.tr("Send to ") + (backend.selectedChatTitle || i18n.tr("active chat"))
                    MouseArea {
                        id: sendViewerMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var cp = photoViewerModal.currentPhoto
                            if (!cp || backend.selectedChatGuid.length === 0) return
                            if (cp.localUrl && cp.localUrl.length > 0) {
                                var lp = cp.localUrl
                                if (lp.indexOf("file://") === 0) {
                                    lp = decodeURIComponent(lp.slice(7))
                                }
                                if (lp.indexOf("/") === 0) {
                                    backend.sendAttachment(backend.selectedChatGuid, lp)
                                    root.closePhotoViewer()
                                    return
                                }
                            }
                            if (cp.guid && cp.guid.length > 0 && !cp.isCloudEvicted) {
                                backend.shareIcloudPhotoToChat(backend.selectedChatGuid, cp.guid, cp.name || "photo.jpg")
                                root.closePhotoViewer()
                            }
                        }
                    }
                }

                // Copy Image Button (Ctrl+C)
                Rectangle {
                    height: 30
                    implicitWidth: copyViewerTxt.implicitWidth + 16
                    radius: 7
                    color: copyViewerMouse.containsMouse ? theme.selection : theme.lighterBackground
                    border.width: 1
                    border.color: copyViewerMouse.containsMouse ? theme.accent : theme.muted
                    Text {
                        id: copyViewerTxt
                        anchors.centerIn: parent
                        text: photoViewerTopBar.compactTopBar ? "󰆏" : ("󰆏 " + i18n.tr("Copy"))
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: copyViewerMouse.containsMouse ? theme.accent : theme.brightForeground
                    }
                    ToolTip.visible: copyViewerMouse.containsMouse
                    ToolTip.text: i18n.tr("Copy Image (Ctrl+C)")
                    MouseArea {
                        id: copyViewerMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var cp = photoViewerModal.currentPhoto
                            if (cp) {
                                backend.copyImageToClipboard(cp.localUrl || "", cp.guid || "")
                            }
                        }
                    }
                }

                // Full Screen / Compact Toggle Button
                Rectangle {
                    id: photoViewerFullScreenBtn
                    objectName: "photoViewerFullScreenBtn"
                    height: 30
                    implicitWidth: fsViewerTxt.implicitWidth + 16
                    radius: 7
                    color: fsViewerMouse.containsMouse ? theme.selection : theme.lighterBackground
                    border.width: 1
                    border.color: root.photoViewerFullScreen ? theme.accent : theme.muted
                    Text {
                        id: fsViewerTxt
                        anchors.centerIn: parent
                        text: photoViewerTopBar.compactTopBar
                              ? (root.photoViewerFullScreen ? "🗗" : "⛶")
                              : (root.photoViewerFullScreen ? ("🗗 " + i18n.tr("Compact")) : ("⛶ " + i18n.tr("Full Screen")))
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: root.photoViewerFullScreen ? theme.accent : theme.brightForeground
                    }
                    MouseArea {
                        id: fsViewerMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.photoViewerFullScreen = !root.photoViewerFullScreen
                    }
                }

                // Close Button (Esc)
                Rectangle {
                    height: 30
                    implicitWidth: closeViewerTxt.implicitWidth + 16
                    radius: 7
                    color: closeViewerMouse.containsMouse ? "#c53b53" : theme.lighterBackground
                    border.width: 1
                    border.color: closeViewerMouse.containsMouse ? "#f7768e" : theme.muted
                    Text {
                        id: closeViewerTxt
                        anchors.centerIn: parent
                        text: photoViewerTopBar.compactTopBar ? "✕" : ("✕ " + i18n.tr("Close (Esc)"))
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: closeViewerMouse.containsMouse ? "#ffffff" : theme.brightForeground
                    }
                    MouseArea {
                        id: closeViewerMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.closePhotoViewer()
                    }
                }
            }
        }

        // Main Image Canvas Area
        Item {
            id: photoCanvasArea
            anchors.top: photoViewerTopBar.bottom
            anchors.bottom: photoThumbBar.visible ? photoThumbBar.top : parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 8
            clip: true

            MouseArea {
                id: imageCanvasMouse
                anchors.fill: parent
                cursorShape: root.photoViewerZoom > 1.0 ? (dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.PointingHandCursor
                property real lastX: 0
                property real lastY: 0
                property bool dragging: false

                onPressed: (mouse) => {
                    if (root.photoViewerZoom > 1.0) {
                        dragging = true
                        lastX = mouse.x
                        lastY = mouse.y
                    }
                }
                onPositionChanged: (mouse) => {
                    if (dragging && root.photoViewerZoom > 1.0) {
                        var dx = mouse.x - lastX
                        var dy = mouse.y - lastY
                        lastX = mouse.x
                        lastY = mouse.y
                        var maxPanX = Math.max(120, fullPhotoImage.width * root.photoViewerZoom * 0.5)
                        var maxPanY = Math.max(120, fullPhotoImage.height * root.photoViewerZoom * 0.5)
                        root.photoViewerPanX = Math.max(-maxPanX, Math.min(maxPanX, root.photoViewerPanX + dx))
                        root.photoViewerPanY = Math.max(-maxPanY, Math.min(maxPanY, root.photoViewerPanY + dy))
                    }
                }
                onReleased: dragging = false
                onCanceled: dragging = false
                onDoubleClicked: {
                    if (root.photoViewerZoom > 1.0) {
                        root.photoViewerZoom = 1.0
                        root.photoViewerPanX = 0
                        root.photoViewerPanY = 0
                    } else {
                        root.photoViewerZoom = 2.0
                    }
                }
                onWheel: (wheel) => {
                    if (wheel.angleDelta.y > 0) {
                        root.zoomPhotoViewer(0.2)
                    } else if (wheel.angleDelta.y < 0) {
                        root.zoomPhotoViewer(-0.2)
                    }
                }
            }

            readonly property string fullPhotoSource: photoViewerModal.currentPhoto.localUrl || photoViewerModal.currentPhoto.previewUrl || photoViewerModal.currentPhoto.sourceUrl || ((photoViewerModal.currentPhoto.guid && !photoViewerModal.currentPhoto.isCloudEvicted) ? backend.attachmentDownloadUrl(photoViewerModal.currentPhoto.guid) : "")
            readonly property bool fullPhotoIsAnimated: backend.isAnimatedImageAttachment(photoViewerModal.currentPhoto.mimeType || "", photoViewerModal.currentPhoto.name || "")
                                                        || backend.isAnimatedImageAttachment("", photoViewerModal.currentPhoto.localUrl || "")
            readonly property bool fullPhotoReady: fullPhotoIsAnimated ? fullPhotoAnimated.status === Image.Ready : fullPhotoImage.status === Image.Ready

            // GIF / WebP: same geometry as fullPhotoImage, but animated
            AnimatedImage {
                id: fullPhotoAnimated
                objectName: "fullPhotoAnimated"
                anchors.fill: fullPhotoImage
                anchors.centerIn: undefined
                // Cleared on close: with cache on, every decoded frame stays in memory while a source is set
                source: root.showPhotoViewer && parent.fullPhotoIsAnimated ? parent.fullPhotoSource : ""
                asynchronous: true
                cache: true
                smooth: true
                fillMode: Image.PreserveAspectFit
                scale: root.photoViewerZoom
                playing: visible
                visible: parent.fullPhotoIsAnimated && status === Image.Ready
            }

            Image {
                id: fullPhotoImage
                objectName: "fullPhotoImage"
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: root.photoViewerPanX
                anchors.verticalCenterOffset: root.photoViewerPanY
                width: Math.max(80, parent.width - (root.photoViewerItems.length > 1 ? 116 : 16))
                height: Math.max(80, parent.height - 8)
                source: parent.fullPhotoIsAnimated ? "" : parent.fullPhotoSource
                asynchronous: true
                cache: true
                autoTransform: true
                smooth: true
                mipmap: true
                fillMode: Image.PreserveAspectFit
                scale: root.photoViewerZoom
                visible: status === Image.Ready
            }

            // Loading / Downloading fallback state
            Rectangle {
                visible: !parent.fullPhotoReady
                anchors.centerIn: parent
                width: 280
                height: 84
                radius: 10
                color: theme.darkBackground
                border.width: 1
                border.color: theme.lighterBackground
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 6
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: photoViewerModal.currentPhoto.canRequestAttachment === false
                              ? ("☁ " + i18n.tr("iCloud Only") + " — " + (photoViewerModal.currentPhoto.name || i18n.tr("Photo")))
                              : ("📷 " + (photoViewerModal.currentPhoto.name || i18n.tr("Loading photo...")))
                        font.pixelSize: theme.fontSize
                        font.bold: true
                        color: theme.brightForeground
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: photoViewerModal.currentPhoto.canRequestAttachment === false
                              ? i18n.tr("Evicted from local Mac cache (unpurge unavailable)")
                              : i18n.tr("Preparing high-resolution preview...")
                        font.pixelSize: theme.fontSize - 3
                        color: theme.darkForeground
                    }
                }
            }

            // Previous Photo Button (‹)
            Rectangle {
                visible: root.photoViewerItems.length > 1
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 46
                height: 46
                radius: 23
                color: prevPhotoMouse.containsMouse ? theme.selection : "#cc131620"
                border.width: 1
                border.color: prevPhotoMouse.containsMouse ? theme.accent : theme.muted
                z: 4
                Text {
                    anchors.centerIn: parent
                    text: "‹"
                    font.pixelSize: theme.fontSize + 14
                    font.bold: true
                    color: theme.brightForeground
                }
                MouseArea {
                    id: prevPhotoMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.prevPhotoInViewer()
                }
            }

            // Next Photo Button (›)
            Rectangle {
                visible: root.photoViewerItems.length > 1
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 46
                height: 46
                radius: 23
                color: nextPhotoMouse.containsMouse ? theme.selection : "#cc131620"
                border.width: 1
                border.color: nextPhotoMouse.containsMouse ? theme.accent : theme.muted
                z: 4
                Text {
                    anchors.centerIn: parent
                    text: "›"
                    font.pixelSize: theme.fontSize + 14
                    font.bold: true
                    color: theme.brightForeground
                }
                MouseArea {
                    id: nextPhotoMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.nextPhotoInViewer()
                }
            }
        }

        // Bottom Filmstrip Thumbnail Bar (when viewing multi-photo messages)
        Rectangle {
            id: photoThumbBar
            visible: root.photoViewerItems.length > 1
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 86
            color: "#cc131620"
            z: 5

            MouseArea { anchors.fill: parent }

            ListView {
                id: photoThumbListView
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 10
                anchors.bottomMargin: 10
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width - 32, Math.max(66, count * 76 - 10))
                orientation: ListView.Horizontal
                spacing: 10
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.photoViewerItems
                currentIndex: root.photoViewerIndex

                onCurrentIndexChanged: {
                    if (currentIndex >= 0 && currentIndex < count) {
                        positionViewAtIndex(currentIndex, ListView.Contain)
                    }
                }
                onCountChanged: {
                    if (currentIndex >= 0 && currentIndex < count) {
                        Qt.callLater(function() {
                            if (photoThumbListView.currentIndex >= 0 && photoThumbListView.currentIndex < photoThumbListView.count) {
                                photoThumbListView.positionViewAtIndex(photoThumbListView.currentIndex, ListView.Center)
                            }
                        })
                    }
                }

                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: 66
                    height: 66
                    radius: 8
                    color: theme.darkBackground
                    border.width: index === root.photoViewerIndex ? 2 : 1
                    border.color: index === root.photoViewerIndex ? theme.accent : theme.muted
                    clip: true

                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: modelData.localUrl || modelData.previewUrl || modelData.sourceUrl || ((modelData.guid && !modelData.isCloudEvicted) ? backend.attachmentDownloadUrl(modelData.guid) : "")
                        asynchronous: true
                        cache: true
                        autoTransform: true
                        fillMode: Image.PreserveAspectCrop
                        opacity: index === root.photoViewerIndex ? 1.0 : 0.65
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.photoViewerIndex = index
                            root.photoViewerZoom = 1.0
                            root.photoViewerPanX = 0
                            root.photoViewerPanY = 0
                            var cur = root.photoViewerItems[index]
                            if (cur && (!cur.localUrl || cur.localUrl.length === 0) && cur.guid && cur.guid.length > 0 && cur.canRequestAttachment) {
                                if (cur.isCloudEvicted) {
                                    backend.downloadCloudAttachment(cur.guid)
                                } else {
                                    backend.openAttachment(cur.guid, cur.name || "")
                                }
                            }
                        }
                        onWheel: (wheel) => {
                            var delta = wheel.angleDelta.x !== 0 ? wheel.angleDelta.x : wheel.angleDelta.y
                            if (delta !== 0) {
                                var maxContentX = Math.max(0, photoThumbListView.contentWidth - photoThumbListView.width)
                                photoThumbListView.contentX = Math.max(0, Math.min(maxContentX, photoThumbListView.contentX - delta))
                                wheel.accepted = true
                            }
                        }
                    }
                }
            }
        }
        }
    }

    // Floating Toast Notification Pill
    Rectangle {
        visible: root.activeToastText.length > 0
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 74
        anchors.horizontalCenter: parent.horizontalCenter
        height: 34
        radius: 17
        implicitWidth: toastLabel.implicitWidth + 28
        color: theme.darkerBackground
        border.width: 1
        border.color: theme.accent
        z: 999

        Text {
            id: toastLabel
            anchors.centerIn: parent
            text: root.activeToastText
            font.pixelSize: theme.fontSize - 1
            font.bold: true
            color: theme.brightForeground
        }
    }

    // Expressive Effect Menu Popup
    Rectangle {
        id: effectMenuPopup
        visible: false
        z: 995
        width: 240
        height: effectCol.implicitHeight + 16
        radius: 10
        color: theme.darkerBackground
        border.color: theme.muted
        border.width: 1
        x: Math.max(12, Math.min(root.width - width - 12, root.width - 320))
        y: Math.max(12, root.height - height - 80)

        ColumnLayout {
            id: effectCol
            anchors.fill: parent
            anchors.margins: 8
            spacing: 2

            Text {
                text: i18n.tr("Send with Effect")
                font.pixelSize: theme.fontSize - 1
                font.bold: true
                color: theme.brightForeground
                Layout.leftMargin: 6
                Layout.topMargin: 2
            }

            Repeater {
                model: [
                    { name: "💥 " + i18n.tr("Slam"), id: "com.apple.MobileSMS.expressivesend.impact" },
                    { name: "📢 " + i18n.tr("Loud"), id: "com.apple.MobileSMS.expressivesend.loud" },
                    { name: "🪶 " + i18n.tr("Gentle"), id: "com.apple.MobileSMS.expressivesend.gentle" },
                    { name: "🕵️ " + i18n.tr("Invisible Ink"), id: "com.apple.MobileSMS.expressivesend.invisibleink" },
                    { name: "🎉 " + i18n.tr("Confetti"), id: "com.apple.messages.effect.CKConfettiEffect" },
                    { name: "✨ " + i18n.tr("Lasers"), id: "com.apple.messages.effect.CKLasersEffect" },
                    { name: "🎆 " + i18n.tr("Fireworks"), id: "com.apple.messages.effect.CKFireworksEffect" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    height: 28
                    radius: 6
                    color: effMouse.containsMouse ? theme.selection : "transparent"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        text: modelData.name
                        font.pixelSize: theme.fontSize - 1
                        color: theme.brightForeground
                    }

                    MouseArea {
                        id: effMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            effectMenuPopup.visible = false;
                            if (root.editTargetGuid !== "") return;
                            var txt = composerInput.text.trim();
                            if (txt.length === 0) {
                                root.showToast("Type a message to send with effect");
                                return;
                            }
                            backend.sendMessageWithEffect(txt, modelData.id);
                            root.triggerExpressiveEffect(modelData.name);
                            composerInput.text = "";
                            messageListView.scrollToBottom();
                        }
                    }
                }
            }
        }
    }

    // Schedule Message Menu Popup
    Rectangle {
        id: scheduleMenuPopup
        visible: false
        z: 995
        width: 220
        height: schedCol.implicitHeight + 16
        radius: 10
        color: theme.darkerBackground
        border.color: theme.muted
        border.width: 1
        x: Math.max(12, Math.min(root.width - width - 12, root.width - 240))
        y: Math.max(12, root.height - height - 80)

        ColumnLayout {
            id: schedCol
            anchors.fill: parent
            anchors.margins: 8
            spacing: 2

            Text {
                text: i18n.tr("Send Later")
                font.pixelSize: theme.fontSize - 1
                font.bold: true
                color: theme.brightForeground
                Layout.leftMargin: 6
                Layout.topMargin: 2
            }

            Repeater {
                model: [
                    { label: "⏱ +15m", offsetMs: 15 * 60 * 1000 },
                    { label: "⏳ +1h", offsetMs: 60 * 60 * 1000 },
                    { label: "🌅 " + i18n.tr("Tomorrow 9 AM"), special: "tomorrow9am" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    height: 28
                    radius: 6
                    color: schedMouse.containsMouse ? theme.selection : "transparent"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        text: modelData.label
                        font.pixelSize: theme.fontSize - 1
                        color: theme.brightForeground
                    }

                    MouseArea {
                        id: schedMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            scheduleMenuPopup.visible = false;
                            if (root.editTargetGuid !== "") return;
                            var txt = composerInput.text.trim();
                            if (txt.length === 0) {
                                root.showToast("Type a message to schedule");
                                return;
                            }
                            var sendTime = Date.now() + (modelData.offsetMs || 0);
                            if (modelData.special === "tomorrow9am") {
                                var d = new Date();
                                d.setDate(d.getDate() + 1);
                                d.setHours(9, 0, 0, 0);
                                sendTime = d.getTime();
                            }
                            backend.scheduleMessage(txt, sendTime);
                            composerInput.text = "";
                            root.showToast("✓ Scheduled message");
                        }
                    }
                }
            }
        }
    }

    // Group Rename & Participant Management Modal (renameChatPopup)
    Rectangle {
        id: renameChatPopup
        visible: false
        anchors.fill: parent
        color: "#aa0e0e14"
        z: 998

        MouseArea {
            anchors.fill: parent
            onClicked: renameChatPopup.visible = false
        }

        Rectangle {
            anchors.centerIn: parent
            width: 420
            height: renameCol.implicitHeight + 36
            radius: 12
            color: theme.darkerBackground
            border.color: theme.muted
            border.width: 1

            MouseArea {
                anchors.fill: parent
                onClicked: {} // prevent backdrop dismissal
            }

            ColumnLayout {
                id: renameCol
                anchors.fill: parent
                anchors.margins: 18
                spacing: 12

                Text {
                    text: i18n.tr("Manage Group Participants")
                    font.pixelSize: theme.fontSize + 2
                    font.bold: true
                    color: theme.brightForeground
                }

                Text {
                    text: backend.selectedChatTitle
                    font.pixelSize: theme.fontSize - 1
                    color: theme.foreground
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                // Add participant row
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle {
                        Layout.fillWidth: true
                        height: 38
                        radius: 8
                        color: theme.background
                        border.color: addPartInput.activeFocus ? theme.accent : theme.muted
                        border.width: 1

                        TextInput {
                            id: addPartInput
                            anchors.fill: parent
                            anchors.margins: 8
                            color: theme.brightForeground
                            font.pixelSize: theme.fontSize
                            clip: true
                            selectByMouse: true

                            Text {
                                text: i18n.tr("Phone number or email address...")
                                color: theme.darkForeground
                                font.pixelSize: theme.fontSize
                                visible: !addPartInput.text && !addPartInput.activeFocus
                            }

                            Keys.onReturnPressed: {
                                if (addPartInput.text.trim().length > 0) {
                                    backend.addParticipantToActiveChat(addPartInput.text.trim());
                                    addPartInput.text = "";
                                    root.showToast("✓ " + i18n.tr("Added participant"));
                                }
                            }
                        }
                    }

                    Rectangle {
                        width: 80
                        height: 38
                        radius: 8
                        color: theme.accent

                        Text {
                            anchors.centerIn: parent
                            text: "+ " + i18n.tr("Add")
                            font.bold: true
                            color: "#ffffff"
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (addPartInput.text.trim().length > 0) {
                                    backend.addParticipantToActiveChat(addPartInput.text.trim());
                                    addPartInput.text = "";
                                    root.showToast("✓ " + i18n.tr("Added participant"));
                                }
                            }
                        }
                    }

                }

                // Close button
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 80
                        height: 32
                        radius: 6
                        color: theme.lighterBackground
                        border.color: theme.muted
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: i18n.tr("Done")
                            color: theme.brightForeground
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: renameChatPopup.visible = false
                        }
                    }
                }
            }
        }
    }

    // Full-Viewport Expressive Screen Effect Overlay (Confetti, Fireworks, Lasers, Balloons, Hearts)
    Item {
        id: screenEffectOverlay
        anchors.fill: parent
        z: 900
        visible: root.activeScreenEffect.length > 0
        clip: true

        Repeater {
            model: 40
            delegate: Item {
                id: particleItem
                required property int index
                property real pProgress: effectAnim.progress
                property real randSeedX: ((index * 7919 + 1013) % 1000) / 1000.0
                property real randSeedY: ((index * 6271 + 2017) % 1000) / 1000.0
                property real randSpeed: 0.6 + (((index * 3571) % 100) / 100.0) * 0.8
                property string particleChar: {
                    if (root.activeScreenEffect === "confetti") {
                        var confChars = ["🎊", "🎉", "✨", "🔹", "🔸", "⭐", "▫️", "▪️"]
                        return confChars[index % confChars.length]
                    } else if (root.activeScreenEffect === "fireworks") {
                        var fireChars = ["🎆", "🎇", "✨", "💥", "🌟", "✨"]
                        return fireChars[index % fireChars.length]
                    } else if (root.activeScreenEffect === "lasers") {
                        return (index % 2 === 0) ? "⚡" : "✨"
                    } else if (root.activeScreenEffect === "balloons") {
                        var balChars = ["🎈", "🎈", "🎈", "🎈", "✨"]
                        return balChars[index % balChars.length]
                    } else if (root.activeScreenEffect === "hearts") {
                        var heartChars = ["❤️", "💖", "💕", "💓", "✨"]
                        return heartChars[index % heartChars.length]
                    }
                    return "✨"
                }

                x: {
                    if (root.activeScreenEffect === "fireworks") {
                        var cx = (index % 2 === 0) ? (screenEffectOverlay.width * 0.35) : (screenEffectOverlay.width * 0.65)
                        var angle = (index * (360.0 / 20.0)) * (Math.PI / 180.0)
                        var dist = pProgress * (180.0 + (index % 5) * 40.0)
                        return cx + Math.cos(angle) * dist
                    } else if (root.activeScreenEffect === "balloons" || root.activeScreenEffect === "hearts") {
                        return randSeedX * (screenEffectOverlay.width - 60) + Math.sin(pProgress * 6.0 + index) * 30
                    } else if (root.activeScreenEffect === "lasers") {
                        return randSeedX * screenEffectOverlay.width + (pProgress * screenEffectOverlay.width * 0.5 * ((index % 2 === 0) ? 1 : -1))
                    } else {
                        return randSeedX * screenEffectOverlay.width + Math.sin(pProgress * 8.0 + index) * 40
                    }
                }

                y: {
                    if (root.activeScreenEffect === "fireworks") {
                        var cy = (index % 2 === 0) ? (screenEffectOverlay.height * 0.35) : (screenEffectOverlay.height * 0.45)
                        var angle = (index * (360.0 / 20.0)) * (Math.PI / 180.0)
                        var dist = pProgress * (180.0 + (index % 5) * 40.0)
                        return cy + Math.sin(angle) * dist + (pProgress * pProgress * 80)
                    } else if (root.activeScreenEffect === "balloons" || root.activeScreenEffect === "hearts") {
                        return (screenEffectOverlay.height + 40) - (pProgress * (screenEffectOverlay.height + 120)) * randSpeed
                    } else if (root.activeScreenEffect === "lasers") {
                        return (index * (screenEffectOverlay.height / 40.0)) + Math.sin(pProgress * 10.0 + index) * 60
                    } else {
                        return -30 + (pProgress * (screenEffectOverlay.height + 80)) * randSpeed
                    }
                }

                rotation: (pProgress * 360 * ((index % 2 === 0) ? 1 : -1))
                scale: {
                    if (root.activeScreenEffect === "fireworks") {
                        return (pProgress < 0.2) ? (pProgress * 5.0) : Math.max(0.2, (1.0 - (pProgress - 0.2) * 1.25))
                    } else if (root.activeScreenEffect === "lasers") {
                        return 1.4 + 0.6 * Math.sin(pProgress * 12.0 + index)
                    } else {
                        return 0.8 + ((index % 4) * 0.2)
                    }
                }
                opacity: {
                    if (pProgress < 0.1) return pProgress * 10.0
                    if (pProgress > 0.75) return Math.max(0.0, (1.0 - pProgress) / 0.25)
                    return 0.95
                }

                Text {
                    anchors.centerIn: parent
                    text: particleItem.particleChar
                    font.pixelSize: (root.activeScreenEffect === "balloons" || root.activeScreenEffect === "hearts") ? 28 : ((root.activeScreenEffect === "fireworks") ? 22 : 18)
                }
            }
        }

        NumberAnimation {
            id: effectAnim
            target: effectAnim
            property: "progress"
            from: 0.0
            to: 1.0
            duration: 2500
            running: root.activeScreenEffect.length > 0
            property real progress: 0.0
        }
    }

}
