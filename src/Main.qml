import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
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
    property string hoveredActionMessageGuid: ""
    property string pinnedActionMessageGuid: ""
    readonly property string activeActionMessageGuid: pinnedActionMessageGuid !== "" ? pinnedActionMessageGuid : hoveredActionMessageGuid

    function clearActionMenu() {
        pinnedActionMessageGuid = ""
        hoveredActionMessageGuid = ""
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
    property bool showDeleteChatModal: false
    property bool showServerConfigModal: false
    property bool showServerPassword: false

    onShowServerConfigModalChanged: {
        if (showServerConfigModal) {
            serverUrlInput.text = backend.serverUrl
            secondaryServerUrlInput.text = backend.secondaryServerUrl
            serverPasswordInput.text = backend.serverPassword
            root.showServerPassword = false
        }
    }

    function openServerConfigModal() {
        root.clearActionMenu()
        root.showShortcutsModal = false
        root.showContactModal = false
        root.showNewChatModal = false
        root.showEmojiPicker = false
        serverUrlInput.text = backend.serverUrl
        secondaryServerUrlInput.text = backend.secondaryServerUrl
        serverPasswordInput.text = backend.serverPassword
        root.showServerPassword = false
        root.showServerConfigModal = true
        serverUrlInput.forceActiveFocus()
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
    property var photoViewerItems: []
    property int photoViewerIndex: 0
    property real photoViewerZoom: 1.0
    property real photoViewerPanX: 0
    property real photoViewerPanY: 0

    function openPhotoViewer(items, startIndex) {
        var list = []
        if (items && items.length !== undefined) {
            for (var i = 0; i < items.length; ++i) {
                var it = items[i]
                if (!it) continue
                list.push({
                    guid: it.guid || "",
                    name: it.name || ("Photo " + (i + 1)),
                    localUrl: it.localUrl || ""
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
        if (cur && (!cur.localUrl || cur.localUrl.length === 0) && cur.guid && cur.guid.length > 0) {
            backend.openAttachment(cur.guid, cur.name || "")
        }
    }

    function closePhotoViewer() {
        root.showPhotoViewer = false
        root.photoViewerZoom = 1.0
        root.photoViewerPanX = 0
        root.photoViewerPanY = 0
        if (backend.selectedChatGuid.length > 0) {
            composerInput.forceActiveFocus()
        }
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
        if (cur && (!cur.localUrl || cur.localUrl.length === 0) && cur.guid && cur.guid.length > 0) {
            backend.openAttachment(cur.guid, cur.name || "")
        }
    }

    function nextPhotoInViewer() {
        if (!root.photoViewerItems || root.photoViewerItems.length <= 1) return
        root.photoViewerIndex = (root.photoViewerIndex + 1) % root.photoViewerItems.length
        root.photoViewerZoom = 1.0
        root.photoViewerPanX = 0
        root.photoViewerPanY = 0
        var cur = root.photoViewerItems[root.photoViewerIndex]
        if (cur && (!cur.localUrl || cur.localUrl.length === 0) && cur.guid && cur.guid.length > 0) {
            backend.openAttachment(cur.guid, cur.name || "")
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
            root.newChatSelectedHandle = ""
            root.newChatSelectedIndex = 0
            root.refreshNewChatSuggestions()
            newChatRecipient.forceActiveFocus()
        }
    }

    Component.onCompleted: backend.registerWindow(root)
    onActiveChanged: backend.windowActive = root.active

    Connections {
        target: backend
        function onToastMessage(text) {
            root.activeToastText = text
            toastTimer.restart()
        }
        function onSelectedChatChanged() {
            root.clearActionMenu()
        }
        function onContactsChanged() {
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
                        localUrl: localUrl
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
        var urlRegex = /(https?:\/\/[^\s<]+[^<.,:;"')\]\s])/g
        var parts = text.split(urlRegex)
        var res = ""
        for (var i = 0; i < parts.length; ++i) {
            var part = parts[i]
            if (part && part.match(/^https?:\/\//i)) {
                res += "<a href=\"" + part + "\" style=\"color: #64B5F6; text-decoration: underline;\">" + escapeHtml(part) + "</a>"
            } else {
                res += escapeHtml(part)
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
            root.contactModalFirstName = ""
            root.contactModalLastName = ""
            root.contactModalPhone = backend.selectedChatAddress
            root.contactModalEmail = ""
            root.showContactModal = true
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
    Shortcut {
        sequence: "Escape"
        onActivated: {
            if (root.showPhotoViewer) {
                root.closePhotoViewer()
            } else if (chatContextMenuPopup.visible) {
                chatContextMenuPopup.visible = false
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
            } else if (root.showEmojiPicker) {
                root.showEmojiPicker = false
            } else if (root.isEmojiCycling || root.emojiAutocompleteResults.length > 0) {
                root.isEmojiCycling = false
                root.emojiAutocompleteResults = []
                root.emojiShortcodePrefix = ""
                root.emojiShortcodeStart = -1
            } else if (root.activeActionMessageGuid !== "") {
                root.clearActionMenu()
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

                // App badge (Clickable Home Page Button) + Connection indicator
                RowLayout {
                    spacing: 8
                    Rectangle {
                        id: homeBrandBtn
                        height: 30
                        radius: 7
                        implicitWidth: homeBrandRow.implicitWidth + 16
                        color: backend.selectedChatGuid.length === 0
                                   ? theme.selection
                                   : (homeBrandMouse.containsMouse ? theme.lighterBackground : "transparent")
                        border.width: (backend.selectedChatGuid.length === 0 || homeBrandMouse.containsMouse) ? 1 : 0
                        border.color: backend.selectedChatGuid.length === 0 ? theme.accent : theme.muted

                        ToolTip.visible: homeBrandMouse.containsMouse
                        ToolTip.text: i18n.tr("BluePill Home Dashboard (Ctrl+H)")
                        ToolTip.delay: 350

                        RowLayout {
                            id: homeBrandRow
                            anchors.centerIn: parent
                            spacing: 7
                            Rectangle {
                                width: 10
                                height: 10
                                radius: 5
                                color: backend.connected ? theme.green : theme.red
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
                                color: backend.selectedChatGuid.length === 0 ? theme.accent : theme.brightForeground
                            }
                            Text {
                                visible: backend.selectedChatGuid.length > 0
                                text: "⌂"
                                font.pixelSize: theme.fontSize
                                color: homeBrandMouse.containsMouse ? theme.accent : theme.darkForeground
                            }
                        }
                        MouseArea {
                            id: homeBrandMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.clearActionMenu()
                                root.showShortcutsModal = false
                                root.showContactModal = false
                                root.showNewChatModal = false
                                root.showEmojiPicker = false
                                backend.clearSelectedChat()
                            }
                        }
                    }
                    Rectangle {
                        id: topStatusPill
                        objectName: "topStatusPill"
                        visible: root.width >= 900
                        height: 22
                        radius: 6
                        color: theme.lighterBackground
                        implicitWidth: statusLabel.implicitWidth + 16
                        Text {
                            id: statusLabel
                            anchors.centerIn: parent
                            text: root.width < 1160
                                      ? (backend.connected ? i18n.tr("Connected") : i18n.tr("Offline"))
                                      : backend.serverStatus
                            font.pixelSize: theme.fontSize - 3
                            color: theme.foreground
                        }
                    }
                    // History Sync Window Setting Pill
                    Rectangle {
                        height: 24
                        radius: 6
                        implicitWidth: historyPillRow.implicitWidth + 14
                        color: historyPillMouse.containsMouse ? theme.selection : theme.lighterBackground
                        border.width: 1
                        border.color: backend.syncingHistory ? theme.accent : (historyPillMouse.containsMouse ? theme.accent : theme.muted)

                        ToolTip.visible: historyPillMouse.containsMouse
                        ToolTip.text: i18n.tr("History sync window: ") + backend.historySyncDays + i18n.tr(" days (Click to cycle 7d/14d/30d/60d/90d/180d/365d)")
                        ToolTip.delay: 350

                        RowLayout {
                            id: historyPillRow
                            anchors.centerIn: parent
                            spacing: 4
                            Text {
                                text: backend.syncingHistory ? "↻" : "🕒"
                                font.pixelSize: theme.fontSize - 3
                                color: backend.syncingHistory ? theme.accent : theme.foreground
                            }
                            Text {
                                text: backend.historySyncDays + "d"
                                font.pixelSize: theme.fontSize - 3
                                font.bold: true
                                color: historyPillMouse.containsMouse || backend.syncingHistory ? theme.accent : theme.foreground
                            }
                        }
                        MouseArea {
                            id: historyPillMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: backend.cycleHistorySyncDays()
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // Filter pills
                RowLayout {
                    spacing: root.isNarrowWindow ? 4 : 6
                    Repeater {
                        model: [
                            { key: "all", label: i18n.tr("All") },
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

                // New Message Button
                Rectangle {
                    height: 30
                    radius: 7
                    implicitWidth: newMsgText.implicitWidth + (root.isNarrowWindow ? 16 : 22)
                    color: theme.selection
                    border.width: 1
                    border.color: theme.accent

                    Text {
                        id: newMsgText
                        anchors.centerIn: parent
                        text: "+ " + (root.width < 980 ? i18n.tr("New") : i18n.tr("New Message"))
                        font.pixelSize: theme.fontSize - 2
                        font.bold: true
                        color: theme.accent
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.showNewChatModal = true
                            newChatRecipient.forceActiveFocus()
                        }
                    }
                }

                // Server Configuration Button (Ctrl+,)
                Rectangle {
                    id: topServerConfigBtn
                    objectName: "topServerConfigBtn"
                    height: 30
                    radius: 7
                    implicitWidth: serverTopLbl.implicitWidth + 16
                    color: root.showServerConfigModal ? theme.accent : theme.lighterBackground
                    border.width: 1
                    border.color: (root.showServerConfigModal || backend.needsSetup) ? theme.accent : theme.muted

                    ToolTip.visible: serverTopMouse.containsMouse
                    ToolTip.text: i18n.tr("BlueBubbles Server Configuration (Ctrl+,)")
                    ToolTip.delay: 400

                    RowLayout {
                        id: serverTopLbl
                        anchors.centerIn: parent
                        spacing: 5
                        Text {
                            text: "⚙"
                            font.pixelSize: theme.fontSize - 1
                            color: root.showServerConfigModal ? theme.darkerBackground : theme.foreground
                        }
                        Text {
                            visible: root.width >= 900
                            text: i18n.tr("Server")
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
                                    if (icloudHubBar.visible) {
                                        backend.fetchIcloudNotes()
                                        icloudNotesPopup.open()
                                    }
                                }
                            }
                            Shortcut {
                                sequence: "Ctrl+Shift+E"
                                onActivated: {
                                    if (icloudHubBar.visible) {
                                        backend.fetchIcloudEvents()
                                        icloudCalendarPopup.open()
                                    }
                                }
                            }
                            Shortcut {
                                sequence: "Ctrl+Shift+P"
                                onActivated: {
                                    if (icloudHubBar.visible) {
                                        backend.fetchIcloudPhotos()
                                        icloudPhotosPopup.open()
                                    }
                                }
                            }

                            Rectangle {
                                id: notesHubTile
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
                        visible: backend.pinnedChats.length > 0 && backend.searchQuery.length === 0
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

                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.initials || "#"
                                                font.pixelSize: theme.fontSize - 1
                                                font.bold: true
                                                color: "#ffffff"
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
                                                backend.togglePinChat(modelData.guid)
                                            } else {
                                                backend.selectChat(modelData.guid)
                                                root.focusAndScrollChat()
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

                                // Avatar Circle
                                Rectangle {
                                    Layout.preferredWidth: 44
                                    Layout.preferredHeight: 44
                                    radius: 22
                                    color: modelData.avatarColor
                                    clip: true

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.initials
                                        font.pixelSize: theme.fontSize
                                        font.bold: true
                                        color: "#ffffff"
                                        visible: avatarImg.status !== Image.Ready
                                    }

                                    Image {
                                        id: avatarImg
                                        anchors.fill: parent
                                        source: modelData.avatarUrl || ""
                                        autoTransform: true
                                        fillMode: Image.PreserveAspectCrop
                                        visible: status === Image.Ready
                                    }
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
                                            color: modelData.service === "iMessage" ? "#1e3a6e" : "#1f4d36"
                                            Text {
                                                id: srvLabel
                                                anchors.centerIn: parent
                                                text: modelData.service
                                                font.pixelSize: theme.fontSize - 3
                                                font.bold: true
                                                color: modelData.service === "iMessage" ? theme.blue : theme.green
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

                                        Rectangle {
                                            visible: chatHover.containsMouse || rowMuteMouse.containsMouse || rowDeleteMouse.containsMouse
                                            width: 22
                                            height: 20
                                            radius: 5
                                            z: 3
                                            color: rowMuteMouse.containsMouse ? theme.accent : theme.background
                                            border.width: 1
                                            border.color: theme.muted
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

                                        Rectangle {
                                            id: rowDeleteChatBtn
                                            visible: chatHover.containsMouse || rowMuteMouse.containsMouse || rowDeleteMouse.containsMouse
                                            width: 22
                                            height: 20
                                            radius: 5
                                            z: 3
                                            color: rowDeleteMouse.containsMouse ? "#3b1d26" : theme.background
                                            border.width: 1
                                            border.color: rowDeleteMouse.containsMouse ? "#f7768e" : theme.muted
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
                                    root.focusAndScrollChat()
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
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // iMessage Home Page Dashboard (shown when no conversation is selected)
                Rectangle {
                    id: homePagePane
                    visible: backend.selectedChatGuid.length === 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: theme.background

                    Flickable {
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: homeDashboardCol.implicitHeight + 56
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        ColumnLayout {
                            id: homeDashboardCol
                            objectName: "homeDashboardCol"
                            width: Math.min(parent.width - 48, 960)
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 28
                            spacing: 22

                            // Home Hero Banner
                            Rectangle {
                                id: homeHeroBanner
                                objectName: "homeHeroBanner"
                                Layout.fillWidth: true
                                readonly property bool compactHeroActions: homeHeroBanner.width > 0 && homeHeroBanner.width < 680
                                implicitHeight: heroCol.implicitHeight + 36
                                Layout.preferredHeight: implicitHeight
                                radius: 12
                                color: theme.darkBackground
                                border.width: 1
                                border.color: theme.lighterBackground
                                clip: true

                                ColumnLayout {
                                    id: heroCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: 18
                                    spacing: 14

                                    RowLayout {
                                        id: heroTopRow
                                        Layout.fillWidth: true
                                        spacing: 12

                                        Rectangle {
                                            Layout.preferredWidth: 48
                                            Layout.preferredHeight: 48
                                            width: 48
                                            height: 48
                                            radius: 12
                                            color: theme.selection
                                            border.width: 1
                                            border.color: theme.accent
                                            Image {
                                                anchors.fill: parent
                                                anchors.margins: 3
                                                source: "qrc:/bluepill.svg"
                                                sourceSize.width: 96
                                                sourceSize.height: 96
                                                fillMode: Image.PreserveAspectFit
                                            }
                                        }

                                        ColumnLayout {
                                            id: heroTitleCol
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 180
                                            clip: true
                                            spacing: 3
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 8
                                                Text {
                                                    id: homeTitleTxt
                                                    text: i18n.tr("BluePill Home")
                                                    font.pixelSize: theme.fontSize + 6
                                                    font.bold: true
                                                    color: theme.brightForeground
                                                }
                                                Rectangle {
                                                    height: 22
                                                    implicitHeight: 22
                                                    Layout.preferredHeight: 22
                                                    radius: 6
                                                    implicitWidth: homeStatusTxt.implicitWidth + 14
                                                    Layout.preferredWidth: implicitWidth
                                                    Layout.minimumWidth: 60
                                                    Layout.maximumWidth: homeStatusTxt.implicitWidth + 14
                                                    Layout.fillWidth: true
                                                    clip: true
                                                    color: backend.connected ? Qt.rgba(48/255, 209/255, 88/255, 0.16) : Qt.rgba(255/255, 69/255, 58/255, 0.18)
                                                    border.width: 1
                                                    border.color: backend.connected ? theme.green : theme.red
                                                    Text {
                                                        id: homeStatusTxt
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        anchors.left: parent.left
                                                        anchors.right: parent.right
                                                        anchors.leftMargin: 7
                                                        anchors.rightMargin: 7
                                                        horizontalAlignment: Text.AlignHCenter
                                                        elide: Text.ElideRight
                                                        text: backend.serverStatus
                                                        font.pixelSize: theme.fontSize - 3
                                                        font.bold: true
                                                        color: backend.connected ? theme.green : theme.red
                                                    }
                                                }
                                                Item { Layout.fillWidth: true }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                                text: backend.serverUrl.length > 0
                                                          ? (i18n.tr("Relay: ") + backend.serverUrl + " · " + i18n.tr("Instant local cache + Private API sync"))
                                                          : i18n.tr("BluePill & BlueBubbles macOS Relay")
                                                font.pixelSize: theme.fontSize - 2
                                                color: theme.darkForeground
                                            }
                                        }

                                        // Quick Actions in Hero Header
                                        RowLayout {
                                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                            spacing: 8

                                            Rectangle {
                                                height: 34
                                                implicitHeight: 34
                                                Layout.preferredHeight: 34
                                                radius: 8
                                                implicitWidth: homeNewMsgLbl.implicitWidth + 22
                                                Layout.preferredWidth: implicitWidth
                                                color: theme.accent
                                                Text {
                                                    id: homeNewMsgLbl
                                                    anchors.centerIn: parent
                                                    text: "+ " + (homeHeroBanner.compactHeroActions ? i18n.tr("New") : i18n.tr("New Message"))
                                                    font.pixelSize: theme.fontSize - 2
                                                    font.bold: true
                                                    color: theme.darkerBackground
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        root.showNewChatModal = true
                                                        newChatRecipient.forceActiveFocus()
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                id: homeWebUiBtn
                                                objectName: "homeWebUiBtn"
                                                height: 34
                                                implicitHeight: 34
                                                Layout.preferredHeight: 34
                                                radius: 8
                                                implicitWidth: homeWebUiLbl.implicitWidth + 20
                                                Layout.preferredWidth: implicitWidth
                                                color: theme.lighterBackground
                                                border.width: 1
                                                border.color: theme.accent
                                                Text {
                                                    id: homeWebUiLbl
                                                    anchors.centerIn: parent
                                                    text: "🌐 " + (homeHeroBanner.compactHeroActions ? i18n.tr("Web") : i18n.tr("Web UI"))
                                                    font.pixelSize: theme.fontSize - 2
                                                    font.bold: true
                                                    color: theme.brightForeground
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: backend.openWebUi()
                                                }
                                            }

                                            Rectangle {
                                                visible: backend.totalUnread > 0
                                                height: 34
                                                implicitHeight: 34
                                                Layout.preferredHeight: 34
                                                radius: 8
                                                implicitWidth: homeMarkAllLbl.implicitWidth + 20
                                                Layout.preferredWidth: implicitWidth
                                                color: theme.selection
                                                border.width: 1
                                                border.color: theme.accent
                                                Text {
                                                    id: homeMarkAllLbl
                                                    anchors.centerIn: parent
                                                    text: "✓ " + (homeHeroBanner.compactHeroActions ? i18n.tr("Read") : i18n.tr("Mark All Read"))
                                                    font.pixelSize: theme.fontSize - 2
                                                    font.bold: true
                                                    color: theme.accent
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        backend.markAllChatsRead()
                                                        root.showToast("✓ " + i18n.tr("Marked all chats as read in iMessage"))
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                height: 34
                                                implicitHeight: 34
                                                Layout.preferredHeight: 34
                                                radius: 8
                                                implicitWidth: homeSyncLbl.implicitWidth + 20
                                                Layout.preferredWidth: implicitWidth
                                                color: theme.lighterBackground
                                                border.width: 1
                                                border.color: theme.muted
                                                Text {
                                                    id: homeSyncLbl
                                                    anchors.centerIn: parent
                                                    text: backend.syncingHistory ? ("↻ " + i18n.tr("Syncing...")) : ("↻ " + (homeHeroBanner.compactHeroActions ? i18n.tr("Sync") : i18n.tr("Sync Now")))
                                                    font.pixelSize: theme.fontSize - 2
                                                    color: theme.foreground
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: backend.refreshAll()
                                                }
                                            }

                                            Rectangle {
                                                id: homeServerConfigBtn
                                                objectName: "homeServerConfigBtn"
                                                height: 34
                                                implicitHeight: 34
                                                Layout.preferredHeight: 34
                                                radius: 8
                                                implicitWidth: homeServerLbl.implicitWidth + 20
                                                Layout.preferredWidth: implicitWidth
                                                color: backend.needsSetup ? theme.selection : theme.lighterBackground
                                                border.width: 1
                                                border.color: backend.needsSetup ? theme.accent : theme.muted
                                                Text {
                                                    id: homeServerLbl
                                                    anchors.centerIn: parent
                                                    text: "⚙ " + i18n.tr("Server")
                                                    font.pixelSize: theme.fontSize - 2
                                                    font.bold: backend.needsSetup
                                                    color: backend.needsSetup ? theme.accent : theme.foreground
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: root.openServerConfigModal()
                                                }
                                            }
                                        }
                                    }

                                    // 3 Summary Metric Cards Row
                                    RowLayout {
                                        id: homeSummaryRow
                                        objectName: "homeSummaryRow"
                                        Layout.fillWidth: true
                                        spacing: 12

                                        Rectangle {
                                            id: homeConversationsCard
                                            objectName: "homeConversationsCard"
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 160
                                            Layout.minimumWidth: 90
                                            implicitHeight: 72
                                            Layout.preferredHeight: 72
                                            height: 72
                                            radius: 9
                                            color: theme.background
                                            border.width: 1
                                            border.color: theme.lighterBackground
                                            clip: true
                                            ColumnLayout {
                                                anchors.fill: parent
                                                anchors.margins: 12
                                                spacing: 2
                                                Text {
                                                    Layout.fillWidth: true
                                                    elide: Text.ElideRight
                                                    text: i18n.tr("CONVERSATIONS")
                                                    font.pixelSize: theme.fontSize - 4
                                                    font.bold: true
                                                    color: theme.darkForeground
                                                }
                                                Text {
                                                    text: String(backend.chats.length)
                                                    font.pixelSize: theme.fontSize + 5
                                                    font.bold: true
                                                    color: theme.brightForeground
                                                }
                                            }
                                        }

                                        Rectangle {
                                            id: homeUnreadCard
                                            objectName: "homeUnreadCard"
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 160
                                            Layout.minimumWidth: 90
                                            implicitHeight: 72
                                            Layout.preferredHeight: 72
                                            height: 72
                                            radius: 9
                                            color: theme.background
                                            border.width: 1
                                            border.color: backend.totalUnread > 0 ? theme.accent : theme.lighterBackground
                                            clip: true
                                            ColumnLayout {
                                                anchors.fill: parent
                                                anchors.margins: 12
                                                spacing: 2
                                                Text {
                                                    Layout.fillWidth: true
                                                    elide: Text.ElideRight
                                                    text: i18n.tr("UNREAD MESSAGES")
                                                    font.pixelSize: theme.fontSize - 4
                                                    font.bold: true
                                                    color: backend.totalUnread > 0 ? theme.accent : theme.darkForeground
                                                }
                                                Text {
                                                    text: String(backend.totalUnread)
                                                    font.pixelSize: theme.fontSize + 5
                                                    font.bold: true
                                                    color: backend.totalUnread > 0 ? theme.accent : theme.brightForeground
                                                }
                                            }
                                        }

                                        // Configurable History Sync Window Setting Card
                                        Rectangle {
                                            id: homeSyncWindowCard
                                            objectName: "homeSyncWindowCard"
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 320
                                            Layout.minimumWidth: 210
                                            implicitHeight: 72
                                            Layout.preferredHeight: 72
                                            height: 72
                                            radius: 9
                                            color: theme.background
                                            border.width: 1
                                            border.color: backend.syncingHistory ? theme.accent : theme.lighterBackground
                                            clip: true
                                            ColumnLayout {
                                                anchors.fill: parent
                                                anchors.margins: 10
                                                spacing: 6
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Text {
                                                        Layout.fillWidth: true
                                                        elide: Text.ElideRight
                                                        text: i18n.tr("MESSAGE HISTORY SYNC WINDOW")
                                                        font.pixelSize: theme.fontSize - 4
                                                        font.bold: true
                                                        color: theme.darkForeground
                                                    }
                                                    Text {
                                                        visible: backend.syncingHistory
                                                        text: i18n.tr("Syncing...")
                                                        font.pixelSize: theme.fontSize - 4
                                                        color: theme.accent
                                                    }
                                                }
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 4
                                                    Repeater {
                                                        model: [7, 14, 30, 60, 90, 180, 365]
                                                        delegate: Rectangle {
                                                            required property int modelData
                                                            Layout.fillWidth: true
                                                            Layout.minimumWidth: presetDayTxt.implicitWidth + 8
                                                            Layout.preferredWidth: presetDayTxt.implicitWidth + 10
                                                            Layout.preferredHeight: 24
                                                            implicitHeight: 24
                                                            height: 24
                                                            radius: 5
                                                            implicitWidth: presetDayTxt.implicitWidth + 10
                                                            color: backend.historySyncDays === modelData ? theme.accent : theme.lighterBackground
                                                            border.width: 1
                                                            border.color: backend.historySyncDays === modelData ? theme.accent : theme.muted
                                                            Text {
                                                                id: presetDayTxt
                                                                anchors.centerIn: parent
                                                                text: modelData + "d"
                                                                font.pixelSize: theme.fontSize - 3
                                                                font.bold: backend.historySyncDays === modelData
                                                                color: backend.historySyncDays === modelData ? theme.darkerBackground : theme.foreground
                                                            }
                                                            MouseArea {
                                                                anchors.fill: parent
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: backend.setHistorySyncDays(modelData)
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Recent Conversations Grid/List on Home Page
                            ColumnLayout {
                                id: homeRecentConversationsCol
                                objectName: "homeRecentConversationsCol"
                                Layout.fillWidth: true
                                spacing: 10

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        text: backend.filterMode === "unread"
                                                  ? i18n.tr("Unread Conversations")
                                                  : i18n.tr("Recent Conversations")
                                        font.pixelSize: theme.fontSize + 1
                                        font.bold: true
                                        color: theme.brightForeground
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: i18n.tr("Click any conversation to open · Right-click to toggle read/unread")
                                        font.pixelSize: theme.fontSize - 3
                                        color: theme.darkForeground
                                        elide: Text.ElideRight
                                    }
                                }

                                Repeater {
                                    model: backend.chats.slice(0, 12)
                                    delegate: Rectangle {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        implicitHeight: 62
                                        Layout.preferredHeight: 62
                                        height: 62
                                        radius: 10
                                        color: homeCardMouse.containsMouse ? theme.lighterBackground : theme.darkBackground
                                        border.width: 1
                                        border.color: modelData.unreadCount > 0 ? theme.accent : theme.lighterBackground

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 14
                                            anchors.rightMargin: 14
                                            spacing: 12

                                            Rectangle {
                                                Layout.preferredWidth: 40
                                                Layout.preferredHeight: 40
                                                width: 40
                                                height: 40
                                                radius: 20
                                                color: modelData.avatarColor || theme.accent
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: modelData.initials || "?"
                                                    font.pixelSize: theme.fontSize - 1
                                                    font.bold: true
                                                    color: "#ffffff"
                                                }
                                            }

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 3
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 8
                                                    Text {
                                                        text: modelData.title
                                                        font.pixelSize: theme.fontSize
                                                        font.bold: true
                                                        color: theme.brightForeground
                                                        elide: Text.ElideRight
                                                    }
                                                    Rectangle {
                                                        visible: modelData.unreadCount > 0
                                                        height: 18
                                                        radius: 9
                                                        implicitWidth: Math.max(18, homeUnreadCountTxt.implicitWidth + 10)
                                                        color: theme.accent
                                                        Text {
                                                            id: homeUnreadCountTxt
                                                            anchors.centerIn: parent
                                                            text: String(modelData.unreadCount)
                                                            font.pixelSize: theme.fontSize - 4
                                                            font.bold: true
                                                            color: theme.darkerBackground
                                                        }
                                                    }
                                                    Item { Layout.fillWidth: true }
                                                    Text {
                                                        text: modelData.relativeTime
                                                        font.pixelSize: theme.fontSize - 3
                                                        color: modelData.unreadCount > 0 ? theme.accent : theme.darkForeground
                                                    }
                                                }
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: modelData.lastMessagePreview
                                                    font.pixelSize: theme.fontSize - 2
                                                    color: modelData.unreadCount > 0 ? theme.foreground : theme.darkForeground
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }

                                        MouseArea {
                                            id: homeCardMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            onClicked: function(mouse) {
                                                if (mouse.button === Qt.RightButton) {
                                                    backend.toggleChatReadUnread(modelData.guid)
                                                } else {
                                                    var win = root
                                                    backend.selectChat(modelData.guid)
                                                    if (win) win.focusAndScrollChat()
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Keyboard Shortcuts Quick Strip
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 42
                                Layout.preferredHeight: 42
                                height: 42
                                radius: 8
                                color: theme.darkBackground
                                border.width: 1
                                border.color: theme.lighterBackground
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 14
                                    spacing: 14
                                    Text {
                                        text: "⌂ Ctrl+H / Esc: Home   ·   Ctrl+N: New Message   ·   Ctrl+F: Search   ·   Ctrl+Shift+U: Read/Unread   ·   Ctrl+Shift+A: Mark All Read   ·   Ctrl+K: All Shortcuts"
                                        font.pixelSize: theme.fontSize - 3
                                        color: theme.darkForeground
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                }
                            }
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
                    readonly property bool stackChatHeaderRows: (activeChatHeader.width > 0) && (activeChatHeader.width < 900) && ((naturalIdentityWidth + naturalActionsWidth + 44) > activeChatHeader.width || activeChatHeader.width < 700)
                    implicitHeight: stackChatHeaderRows ? 104 : 68
                    Layout.preferredHeight: implicitHeight
                    color: theme.darkBackground

                    // Left / Top Side: Contact Identity Card
                    Rectangle {
                        id: contactIdentityCard
                        objectName: "contactIdentityCard"
                        x: 18
                        y: activeChatHeader.stackChatHeaderRows ? 8 : Math.round((activeChatHeader.height - height) / 2)
                        height: activeChatHeader.stackChatHeaderRows ? 48 : 52
                        readonly property real maxAllowedWidth: activeChatHeader.stackChatHeaderRows
                                                                    ? Math.max(180, activeChatHeader.width - 36)
                                                                    : Math.max(180, activeChatHeader.width - headerActionsRow.width - 44)
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

                                Text {
                                    anchors.centerIn: parent
                                    text: backend.selectedChatInitials
                                    font.pixelSize: theme.fontSize + 1
                                    font.bold: true
                                    color: "#ffffff"
                                    visible: headerAvatar.status !== Image.Ready
                                }
                                Image {
                                    id: headerAvatar
                                    anchors.fill: parent
                                    source: backend.selectedChatAvatar
                                    autoTransform: true
                                    fillMode: Image.PreserveAspectCrop
                                    visible: status === Image.Ready
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
                                        color: backend.selectedChatService === "iMessage" ? "#1e3a6e" : "#1f4d36"
                                        Text {
                                            id: hdrSrv
                                            anchors.centerIn: parent
                                            text: backend.selectedChatService
                                            font.pixelSize: theme.fontSize - 2
                                            font.bold: true
                                            color: backend.selectedChatService === "iMessage" ? theme.blue : theme.green
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
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: (!backend.selectedChatIsGroup && backend.selectedChatAddress.length > 0) ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: {
                                if (!backend.selectedChatIsGroup && backend.selectedChatAddress.length > 0) {
                                    var parts = backend.selectedChatTitle.split(" ")
                                    root.contactModalFirstName = parts[0] || ""
                                    root.contactModalLastName = parts.slice(1).join(" ") || ""
                                    root.contactModalPhone = backend.selectedChatAddress
                                    root.contactModalEmail = ""
                                    root.showContactModal = true
                                }
                            }
                        }
                    }

                    // Action Buttons Row: flush right on wide windows, second row on 50% split / narrow windows
                    Row {
                        id: headerActionsRow
                        objectName: "headerActionsRow"
                        x: activeChatHeader.stackChatHeaderRows
                               ? 18
                               : Math.max(18, activeChatHeader.width - width - 18)
                        y: activeChatHeader.stackChatHeaderRows
                               ? 62
                               : Math.round((activeChatHeader.height - height) / 2)
                        spacing: activeChatHeader.compactActionButtons ? 5 : 8

                        // Add / Edit Contact Button
                        Rectangle {
                            visible: !backend.selectedChatIsGroup && backend.selectedChatAddress.length > 0
                            height: 32
                            radius: 8
                            width: contactBtnLbl.implicitWidth + activeChatHeader.actionBtnPad
                            color: backend.selectedChatHasContact ? theme.lighterBackground : Qt.rgba(10/255, 132/255, 255/255, 0.18)
                            border.width: 1
                            border.color: backend.selectedChatHasContact ? theme.muted : theme.accent

                            Text {
                                id: contactBtnLbl
                                anchors.centerIn: parent
                                text: activeChatHeader.ultraCompactActionButtons
                                          ? (backend.selectedChatHasContact ? ("✎ " + i18n.tr("Contact")) : ("＋ " + i18n.tr("Contact")))
                                          : (backend.selectedChatHasContact ? ("✎ " + i18n.tr("Edit Contact")) : ("＋ " + i18n.tr("Add Contact")))
                                font.pixelSize: theme.fontSize - 2
                                font.bold: !backend.selectedChatHasContact
                                color: backend.selectedChatHasContact ? theme.foreground : theme.accent
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    var parts = backend.selectedChatTitle.split(" ")
                                    root.contactModalFirstName = parts[0] || ""
                                    root.contactModalLastName = parts.slice(1).join(" ") || ""
                                    root.contactModalPhone = backend.selectedChatAddress
                                    root.contactModalEmail = ""
                                    root.showContactModal = true
                                }
                            }
                        }

                        // Scroll to Top Button
                        Rectangle {
                            height: 32
                            radius: 8
                            width: topBtnLbl.implicitWidth + activeChatHeader.actionBtnPad
                            color: topBtnMouse.containsMouse ? theme.selection : theme.lighterBackground
                            border.width: 1
                            border.color: topBtnMouse.containsMouse ? theme.accent : theme.muted

                            Text {
                                id: topBtnLbl
                                anchors.centerIn: parent
                                text: "↑ " + i18n.tr("Top")
                                font.pixelSize: theme.fontSize - 2
                                color: topBtnMouse.containsMouse ? theme.accent : theme.foreground
                            }
                            MouseArea {
                                id: topBtnMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: messageListView.scrollToTop()
                            }
                        }

                        // Scroll to Bottom Button
                        Rectangle {
                            height: 32
                            radius: 8
                            width: bottomBtnLbl.implicitWidth + activeChatHeader.actionBtnPad
                            color: bottomBtnMouse.containsMouse ? theme.selection : theme.lighterBackground
                            border.width: 1
                            border.color: bottomBtnMouse.containsMouse ? theme.accent : theme.muted

                            Text {
                                id: bottomBtnLbl
                                anchors.centerIn: parent
                                text: "↓ " + i18n.tr("Bottom")
                                font.pixelSize: theme.fontSize - 2
                                color: bottomBtnMouse.containsMouse ? theme.accent : theme.foreground
                            }
                            MouseArea {
                                id: bottomBtnMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: messageListView.scrollToBottom()
                            }
                        }

                        // Mute / Unmute Conversation Button
                        Rectangle {
                            height: 32
                            radius: 8
                            width: muteChatLbl.implicitWidth + activeChatHeader.actionBtnPad
                            color: backend.selectedChatMuted ? theme.selection : theme.lighterBackground
                            border.width: 1
                            border.color: backend.selectedChatMuted ? theme.accent : theme.muted

                            Text {
                                id: muteChatLbl
                                anchors.centerIn: parent
                                text: backend.selectedChatMuted ? ("🔔 " + i18n.tr("Muted")) : ("🔕 " + i18n.tr("Mute"))
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.selectedChatMuted
                                color: backend.selectedChatMuted ? theme.accent : theme.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.toggleChatMute(backend.selectedChatGuid)
                            }
                        }

                        // Mark Read Button (Bidirectional iMessage Read/Unread Sync)
                        Rectangle {
                            height: 32
                            radius: 8
                            width: markReadLbl.implicitWidth + activeChatHeader.actionBtnPad
                            color: backend.selectedChatUnreadCount > 0 ? theme.selection : theme.lighterBackground
                            border.width: 1
                            border.color: backend.selectedChatUnreadCount > 0 ? theme.accent : theme.muted

                            Text {
                                id: markReadLbl
                                anchors.centerIn: parent
                                text: backend.selectedChatUnreadCount > 0
                                          ? ("✓ " + (activeChatHeader.ultraCompactActionButtons ? i18n.tr("Read") : i18n.tr("Mark Read")))
                                          : ("✓ " + i18n.tr("Read"))
                                font.pixelSize: theme.fontSize - 2
                                font.bold: backend.selectedChatUnreadCount > 0
                                color: backend.selectedChatUnreadCount > 0 ? theme.accent : theme.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: function(mouse) {
                                    if (mouse.button === Qt.RightButton) {
                                        backend.markChatUnread(backend.selectedChatGuid)
                                        root.showToast("● " + i18n.tr("Marked chat as unread in iMessage"))
                                    } else {
                                        if (backend.selectedChatUnreadCount > 0) {
                                            backend.markChatRead(backend.selectedChatGuid)
                                            root.showToast("✓ " + i18n.tr("Marked chat as read in iMessage"))
                                        } else {
                                            backend.toggleChatReadUnread(backend.selectedChatGuid)
                                            if (backend.selectedChatUnreadCount > 0) {
                                                root.showToast("● " + i18n.tr("Marked chat as unread in iMessage"))
                                            } else {
                                                root.showToast("✓ " + i18n.tr("Marked chat as read in iMessage"))
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Sync Contacts / Refresh Button
                        Rectangle {
                            height: 32
                            radius: 8
                            width: refreshLbl.implicitWidth + activeChatHeader.actionBtnPad
                            color: theme.lighterBackground
                            border.width: 1
                            border.color: theme.muted

                            Text {
                                id: refreshLbl
                                anchors.centerIn: parent
                                text: "↻ " + i18n.tr("Sync")
                                font.pixelSize: theme.fontSize - 2
                                color: theme.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.syncContactsNow()
                            }
                        }

                        // Delete Conversation Button (deletes conversation & syncs across iPhone/iCloud)
                        Rectangle {
                            id: headerDeleteChatBtn
                            height: 32
                            radius: 8
                            width: 34
                            color: headerDeleteChatMouse.containsMouse ? "#3b1d26" : theme.lighterBackground
                            border.width: 1
                            border.color: headerDeleteChatMouse.containsMouse ? "#f7768e" : theme.muted

                            Text {
                                anchors.centerIn: parent
                                text: "󰆴"
                                font.pixelSize: theme.fontSize
                                color: headerDeleteChatMouse.containsMouse ? "#f7768e" : theme.foreground
                            }
                            MouseArea {
                                id: headerDeleteChatMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.pendingDeleteChatGuid = backend.selectedChatGuid
                                    root.pendingDeleteChatTitle = backend.selectedChatTitle
                                    root.showDeleteChatModal = true
                                }
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
                        var maxEnd = originY + contentHeight - height
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
                        if (restoringScrollPosition && !stickToBottom && (savedAnchorGuid.length > 0 || savedAnchorIndex >= 0)) {
                            return
                        }
                        savedStickToBottom = stickToBottom
                        savedContentY = contentY - originY
                        savedContentHeight = contentHeight
                        savedCount = count
                        var probeY = contentY + Math.min(80, Math.max(46, height * 0.22))
                        var idx = indexAt(width * 0.5, probeY)
                        if (idx < 0) {
                            idx = indexAt(width * 0.5, contentY + height * 0.5)
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
                            positionViewAtIndex(targetIdx, ListView.Beginning)
                            var item = itemAtIndex(targetIdx)
                            if (item) {
                                var desiredY = item.y - savedAnchorOffset
                                var minY = originY - topMargin
                                var maxY = Math.max(minY, originY + contentHeight + bottomMargin - height)
                                contentY = Math.max(minY, Math.min(maxY, desiredY))
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
                        if (stickToBottom) {
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

                    delegate: ColumnLayout {
                        id: msgDelegate
                        required property var modelData
                        property bool invisibleInkRevealed: false
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
                            return ""
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
                            color: theme.darkBackground
                            border.width: 1
                            border.color: theme.muted
                            implicitWidth: Math.min(maxBannerWidth, Math.max(replySenderTxt.implicitWidth, Math.min(maxBannerWidth - 16, replyBodyTxt.implicitWidth)) + 16)
                            implicitHeight: replyPreviewCol.implicitHeight + 12

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
                                                    root.replyTargetGuid = msgDelegate.modelData.guid
                                                    root.replyTargetSender = msgDelegate.modelData.senderName
                                                    root.replyTargetText = msgDelegate.modelData.text
                                                    root.clearActionMenu()
                                                    composerInput.forceActiveFocus()
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: (msgDelegate.modelData.text || "").length > 0
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
                                                    text: i18n.tr("Copy")
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
                                                    var toCopy = (msgText.selectedText && msgText.selectedText.length > 0)
                                                                 ? msgText.selectedText
                                                                 : (msgDelegate.modelData.text || "")
                                                    backend.copyToClipboard(toCopy, (msgText.selectedText && msgText.selectedText.length > 0) ? "selection" : "message")
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
                                            visible: Boolean(msgDelegate.modelData.isFromMe && (msgDelegate.modelData.isIMessage || msgDelegate.modelData.service === "iMessage"))
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
                                    radius: theme.radius + 2
                                    color: modelData.isFromMe
                                           ? ((modelData.service === "RCS" || modelData.service === "SMS" || (!modelData.service && backend.selectedChatService !== "iMessage"))
                                              ? "#30D158" : "#0A84FF")
                                           : "#1e2430"
                                    border.width: 1
                                    border.color: (root.activeActionMessageGuid === modelData.guid)
                                                  ? theme.brightForeground
                                                  : (modelData.isFromMe
                                                     ? ((modelData.service === "RCS" || modelData.service === "SMS" || (!modelData.service && backend.selectedChatService !== "iMessage"))
                                                        ? "#248A3D" : "#0066CC")
                                                     : "#2c3545")
                                    implicitWidth: Math.max(bubbleContent.width + 28, 96)
                                    implicitHeight: bubbleContent.height + 20

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
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: (mouse) => {
                                            if (mouse.button === Qt.RightButton) {
                                                var toCopy = (msgText.selectedText && msgText.selectedText.length > 0)
                                                             ? msgText.selectedText
                                                             : (modelData.text || "")
                                                if (toCopy.length > 0) {
                                                    backend.copyToClipboard(toCopy, (msgText.selectedText && msgText.selectedText.length > 0) ? "selection" : "message")
                                                }
                                                return
                                            }
                                            if (root.pinnedActionMessageGuid === modelData.guid) {
                                                root.pinnedActionMessageGuid = ""
                                            } else {
                                                root.pinnedActionMessageGuid = modelData.guid
                                            }
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
                                        x: 14
                                        y: 10
                                        spacing: 6
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
                                                objectName: "inlineImageAttachmentCard"
                                                readonly property bool isPortraitImage: inlineImg.status === Image.Ready && inlineImg.implicitHeight > inlineImg.implicitWidth && inlineImg.implicitWidth > 0
                                                width: isPortraitImage
                                                           ? Math.min(240, bubbleContent.maxTextW + 28)
                                                           : Math.min(320, bubbleContent.maxTextW + 28)
                                                height: (inlineImg.status === Image.Ready && inlineImg.implicitWidth > 0 && inlineImg.implicitHeight > 0)
                                                            ? Math.round(Math.max(150, Math.min(isPortraitImage ? 320 : 240, width * (inlineImg.implicitHeight / inlineImg.implicitWidth))))
                                                            : 170
                                                radius: 8
                                                color: theme.darkBackground
                                                clip: true

                                                readonly property string resolvedImgUrl: msgDelegate.resolveAttachmentUrl(modelData.guid || "", modelData.localUrl || "")

                                                Image {
                                                    id: inlineImg
                                                    anchors.fill: parent
                                                    source: parent.resolvedImgUrl
                                                    asynchronous: true
                                                    cache: true
                                                    autoTransform: true
                                                    fillMode: Image.PreserveAspectCrop
                                                    visible: status === Image.Ready
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
                                                    visible: inlineImg.status !== Image.Ready
                                                    text: "📷 " + (modelData.name || "Loading image...")
                                                    font.pixelSize: theme.fontSize - 2
                                                    color: theme.foreground
                                                }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        var rawList = imageAttachmentRepeater.model || []
                                                        var resolvedList = []
                                                        for (var k = 0; k < rawList.length; ++k) {
                                                            var entry = rawList[k]
                                                            var eg = entry.guid || ""
                                                            resolvedList.push({
                                                                guid: eg,
                                                                name: entry.name || ("Photo " + (k + 1)),
                                                                localUrl: msgDelegate.resolveAttachmentUrl(eg, entry.localUrl || "")
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
                                                        Qt.openUrlExternally(msgDelegate.liveLinkPreview.url);
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
                                                text: modelData.formattedText || root.linkifyText(modelData.text || "")
                                                textFormat: TextEdit.RichText
                                                readOnly: true
                                                selectByMouse: true
                                                selectByKeyboard: true
                                                selectedTextColor: "#ffffff"
                                                selectionColor: modelData.isFromMe ? "#004080" : theme.accent
                                                font.pixelSize: theme.fontSize
                                                color: modelData.isFromMe ? "#ffffff" : theme.brightForeground
                                                wrapMode: TextEdit.Wrap
                                                onLinkActivated: (link) => Qt.openUrlExternally(link)

                                                HoverHandler {
                                                    id: msgTextHover
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
                                                color: modelData.isFromMe ? "#0866c6" : "#252d3d"
                                                border.width: 1
                                                border.color: modelData.isFromMe ? "#5cb3ff" : theme.accent
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
                                                       : (modelData.isFromMe
                                                          ? ((modelData.service === "RCS" || modelData.service === "SMS" || (!modelData.service && backend.selectedChatService !== "iMessage"))
                                                             ? "#e0f8e9" : "#d0dff7")
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
                        color: backend.selectedChatService !== "iMessage" ? "#30D158" : theme.accent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        spacing: 8

                        Text {
                            text: "↩"
                            font.pixelSize: theme.fontSize - 1
                            color: backend.selectedChatService !== "iMessage" ? "#30D158" : theme.accent
                        }

                        Text {
                            text: i18n.tr("Replying to") + " " + root.replyTargetSender + ":"
                            font.pixelSize: theme.fontSize - 2
                            font.bold: true
                            color: backend.selectedChatService !== "iMessage" ? "#30D158" : theme.accent
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

                // Bottom Message Composer Bar
                Rectangle {
                    visible: backend.selectedChatGuid.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(160, Math.max(72, theme.fontSize + 52, composerInput.contentHeight + composerInput.topPadding + composerInput.bottomPadding + 28))
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
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: theme.radius
                            color: theme.background
                            border.width: 1
                            border.color: composerInput.activeFocus ? theme.accent : theme.muted

                            TextArea {
                                id: composerInput
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                topPadding: 10
                                bottomPadding: 14
                                leftPadding: 4
                                rightPadding: 4
                                placeholderText: i18n.tr("Type an iMessage... (:smile: for emoji, Enter to send)")
                                placeholderTextColor: theme.darkForeground
                                color: theme.brightForeground
                                font.pixelSize: theme.fontSize
                                wrapMode: TextArea.Wrap
                                background: Item {}

                                readOnly: root.showPhotoViewer
                                onTextChanged: root.updateEmojiAutocomplete()
                                onCursorPositionChanged: root.updateEmojiAutocomplete()

                                Keys.priority: Keys.BeforeItem
                                Keys.onPressed: function(event) {
                                    if (root.showPhotoViewer && root.handlePhotoViewerKey(event)) {
                                        return
                                    }
                                    if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_V) {
                                        var pastedPath = backend.pasteClipboardImageToTempFile()
                                        if (pastedPath && pastedPath.length > 0 && backend.selectedChatGuid.length > 0) {
                                            backend.sendAttachment(backend.selectedChatGuid, pastedPath, composerInput.text.trim())
                                            composerInput.text = ""
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
                                        if (composerInput.text.trim().length > 0) {
                                            backend.sendMessage(backend.selectedChatGuid, composerInput.text, root.replyTargetGuid)
                                            messageListView.scrollToBottom()
                                            composerInput.text = ""
                                            root.showEmojiPicker = false
                                            root.emojiAutocompleteResults = []
                                            root.replyTargetGuid = ""
                                            root.replyTargetSender = ""
                                            root.replyTargetText = ""
                                        }
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
                                        backend.stopAndSendVoiceRecording();
                                        root.showToast("✓ Sent voice memo");
                                    } else {
                                        backend.startVoiceRecording();
                                    }
                                }
                            }

                            ToolTip.visible: voiceBtnMouse.containsMouse
                            ToolTip.text: backend.recordingVoice ? i18n.tr("Stop & Send Voice Memo") : i18n.tr("Record Voice Memo")
                            ToolTip.delay: 400
                        }

                        // Expressive Send Effect Menu Button (✨)
                        Rectangle {
                            id: effectBtn
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
                            visible: root.width >= 760
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
                            color: composerInput.text.trim().length > 0
                                   ? (backend.selectedChatService !== "iMessage" ? "#30D158" : theme.accent)
                                   : theme.lighterBackground

                            Text {
                                anchors.centerIn: parent
                                text: i18n.tr("Send") + " ↑"
                                font.pixelSize: theme.fontSize - 1
                                font.bold: true
                                color: composerInput.text.trim().length > 0 ? "#ffffff" : theme.darkForeground
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (composerInput.text.trim().length > 0) {
                                        backend.sendMessage(backend.selectedChatGuid, composerInput.text, root.replyTargetGuid)
                                        messageListView.scrollToBottom()
                                        composerInput.text = ""
                                        root.replyTargetGuid = ""
                                        root.replyTargetSender = ""
                                        root.replyTargetText = ""
                                    }
                                }
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
        title: (noteDetailTitleInput.text.trim().length > 0 ? noteDetailTitleInput.text.trim() : i18n.tr("Note Viewer")) + " - BluePill Notes"
        width: 680
        height: 560
        minimumWidth: 480
        minimumHeight: 380
        color: theme.darkBackground

        property string noteId: ""
        property string noteSyncSource: "local_hub"
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
                if (noteDetailWindow.targetChatGuid === "" && backend.selectedChatGuid !== "") {
                    noteDetailWindow.targetChatGuid = backend.selectedChatGuid
                    noteDetailWindow.targetChatTitle = backend.selectedChatTitle
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
                spacing: 10

                TextField {
                    id: noteDetailTitleInput
                    Layout.fillWidth: true
                    placeholderText: i18n.tr("Note Title...")
                    font.bold: true
                    font.pixelSize: theme.fontSize + 4
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
                    font.pixelSize: theme.fontSize - 1
                    color: theme.foreground
                    background: Rectangle {
                        color: theme.background
                        radius: 6
                        border.color: noteDetailFolderInput.activeFocus ? theme.accent : Qt.rgba(1, 1, 1, 0.1)
                        border.width: 1
                    }
                }

                Rectangle {
                    height: 26
                    width: syncBadgeText.implicitWidth + 12
                    radius: 13
                    color: noteDetailWindow.noteSyncSource === "apple_notes_ro"
                           ? Qt.rgba(theme.green.r, theme.green.g, theme.green.b, 0.18)
                           : Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18)
                    border.color: noteDetailWindow.noteSyncSource === "apple_notes_ro" ? theme.green : theme.accent
                    border.width: 1
                    Text {
                        id: syncBadgeText
                        anchors.centerIn: parent
                        text: noteDetailWindow.noteSyncSource === "apple_notes_ro" ? "🍏 Apple Notes (iCloud)" : "📝 Local Hub Note"
                        font.pixelSize: 10
                        font.bold: true
                        color: noteDetailWindow.noteSyncSource === "apple_notes_ro" ? theme.green : theme.accent
                    }
                }
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                TextArea {
                    id: noteDetailBodyInput
                    placeholderText: i18n.tr("Note content...")
                    wrapMode: TextArea.Wrap
                    font.pixelSize: theme.fontSize + 1
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
                Layout.fillWidth: true
                spacing: 8

                Button {
                    text: noteDetailWindow.noteSyncSource === "apple_notes_ro"
                          ? ("📋 " + i18n.tr("Copy to Local Hub"))
                          : ("💾 " + i18n.tr("Save Note"))
                    onClicked: {
                        var targetId = noteDetailWindow.noteSyncSource === "apple_notes_ro" ? "" : noteDetailWindow.noteId
                        noteDetailWindow.noteId = backend.saveIcloudNote(targetId, noteDetailTitleInput.text, noteDetailBodyInput.text, noteDetailFolderInput.text)
                        noteDetailWindow.noteSyncSource = "local_hub"
                        noteTitleInput.text = noteDetailTitleInput.text
                        noteBodyInput.text = noteDetailBodyInput.text
                        noteFolderInput.text = noteDetailFolderInput.text
                        icloudNotesPopup.selectedNoteId = noteDetailWindow.noteId
                        icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                    }
                }

                Button {
                    text: "📤 " + (noteDetailWindow.targetChatTitle.trim().length > 0
                                   ? i18n.tr("Send to %1").arg(noteDetailWindow.targetChatTitle.trim())
                                   : i18n.tr("Send to Chat"))
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
                    enabled: noteDetailWindow.noteId !== "" && noteDetailWindow.noteSyncSource !== "apple_notes_ro"
                    onClicked: {
                        if (noteDetailWindow.noteSyncSource === "apple_notes_ro") return
                        backend.deleteIcloudNote(noteDetailWindow.noteId)
                        noteDetailWindow.noteId = ""
                        noteDetailWindow.noteSyncSource = "local_hub"
                        noteDetailTitleInput.text = ""
                        noteDetailBodyInput.text = ""
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
        width: Math.min(900, root.width - 40)
        height: Math.min(640, root.height - 40)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.darkBackground
            radius: theme.radius + 4
            border.width: 1
            border.color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.4)
        }

        property string selectedNoteId: ""
        property string selectedNoteFolder: "Notes"
        property string selectedNoteSyncSource: "local_hub"

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "📝 " + i18n.tr("iCloud (Read-Only) & Local Hub Notes")
                    font.pixelSize: theme.fontSize + 3
                    font.bold: true
                    color: theme.brightForeground
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "+ " + i18n.tr("New Note")
                    onClicked: {
                        icloudNotesPopup.selectedNoteId = ""
                        icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                        noteTitleInput.text = ""
                        noteFolderInput.text = "Notes"
                        noteBodyInput.text = ""
                        noteTitleInput.forceActiveFocus()
                    }
                }
                Button {
                    text: "✕"
                    onClicked: icloudNotesPopup.close()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 14

                ColumnLayout {
                    Layout.preferredWidth: 280
                    Layout.fillHeight: true
                    spacing: 8

                    TextField {
                        id: noteSearchInput
                        Layout.fillWidth: true
                        placeholderText: i18n.tr("Search notes...")
                        color: theme.foreground
                    }

                    ListView {
                        id: notesListView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: {
                            var q = noteSearchInput.text.trim().toLowerCase()
                            var all = backend.icloudNotes
                            if (q.length === 0) return all
                            return all.filter(function(n) {
                                var t = (n.title || "").toLowerCase()
                                var b = (n.body || n.snippet || "").toLowerCase()
                                return t.indexOf(q) !== -1 || b.indexOf(q) !== -1
                            })
                        }
                        spacing: 4

                        delegate: Rectangle {
                            width: notesListView.width
                            height: 60
                            radius: 6
                            color: icloudNotesPopup.selectedNoteId === (modelData.id || "")
                                   ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.25)
                                   : (itemMouse.containsMouse ? theme.lighterBackground : theme.background)
                            border.color: icloudNotesPopup.selectedNoteId === (modelData.id || "") ? theme.accent : "transparent"
                            border.width: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 2

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.title || i18n.tr("Untitled Note")
                                    font.bold: true
                                    font.pixelSize: theme.fontSize
                                    color: theme.brightForeground
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.snippet || modelData.body || ""
                                    font.pixelSize: theme.fontSize - 2
                                    color: theme.darkForeground
                                    elide: Text.ElideRight
                                }
                            }

                            MouseArea {
                                id: itemMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    icloudNotesPopup.selectedNoteId = modelData.id || ""
                                    icloudNotesPopup.selectedNoteSyncSource = modelData.syncSource || "local_hub"
                                    noteTitleInput.text = modelData.title || ""
                                    noteFolderInput.text = modelData.folder || "Notes"
                                    noteBodyInput.text = modelData.body || modelData.snippet || ""

                                    noteDetailWindow.noteId = modelData.id || ""
                                    noteDetailWindow.noteSyncSource = modelData.syncSource || "local_hub"
                                    noteDetailWindow.targetChatGuid = backend.selectedChatGuid
                                    noteDetailWindow.targetChatTitle = backend.selectedChatTitle
                                    noteDetailTitleInput.text = modelData.title || ""
                                    noteDetailFolderInput.text = modelData.folder || "Notes"
                                    noteDetailBodyInput.text = modelData.body || modelData.snippet || ""
                                    noteDetailWindow.show()
                                    noteDetailWindow.raise()
                                    noteDetailWindow.requestActivate()
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: theme.background
                    radius: 8
                    border.color: Qt.rgba(1, 1, 1, 0.1)
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            TextField {
                                id: noteTitleInput
                                Layout.fillWidth: true
                                placeholderText: i18n.tr("Note Title")
                                font.bold: true
                                font.pixelSize: theme.fontSize + 1
                                color: theme.brightForeground
                            }
                            TextField {
                                id: noteFolderInput
                                Layout.preferredWidth: 120
                                placeholderText: i18n.tr("Folder")
                                text: "Notes"
                                color: theme.foreground
                            }
                        }

                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            TextArea {
                                id: noteBodyInput
                                placeholderText: i18n.tr("Write note content here...")
                                color: theme.foreground
                                wrapMode: TextArea.Wrap
                                font.pixelSize: theme.fontSize
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Button {
                                text: icloudNotesPopup.selectedNoteSyncSource === "apple_notes_ro"
                                      ? ("📋 " + i18n.tr("Copy to Local Hub"))
                                      : ("💾 " + i18n.tr("Save"))
                                onClicked: {
                                    var targetId = icloudNotesPopup.selectedNoteSyncSource === "apple_notes_ro" ? "" : icloudNotesPopup.selectedNoteId
                                    icloudNotesPopup.selectedNoteId = backend.saveIcloudNote(targetId, noteTitleInput.text, noteBodyInput.text, noteFolderInput.text)
                                    icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                }
                            }

                            Button {
                                text: "📤 " + i18n.tr("Send to Chat")
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

                            Item { Layout.fillWidth: true }

                            Button {
                                text: "🗑 " + i18n.tr("Delete")
                                enabled: icloudNotesPopup.selectedNoteId !== "" && icloudNotesPopup.selectedNoteSyncSource !== "apple_notes_ro"
                                onClicked: {
                                    if (icloudNotesPopup.selectedNoteSyncSource === "apple_notes_ro") return
                                    backend.deleteIcloudNote(icloudNotesPopup.selectedNoteId)
                                    icloudNotesPopup.selectedNoteId = ""
                                    icloudNotesPopup.selectedNoteSyncSource = "local_hub"
                                    noteTitleInput.text = ""
                                    noteBodyInput.text = ""
                                }
                            }
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
        width: Math.min(880, root.width - 40)
        height: Math.min(620, root.height - 40)
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
                Text {
                    text: "📅 " + i18n.tr("iCloud (Read-Only) & Local Hub Calendar")
                    font.pixelSize: theme.fontSize + 3
                    font.bold: true
                    color: theme.brightForeground
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "✕"
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
                    }

                    ComboBox {
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
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: backend.icloudEvents
                spacing: 6

                delegate: Rectangle {
                    width: eventsListView.width
                    height: 56
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
        }
    }

    // iCloud Photos Popup
    Popup {
        id: icloudPhotosPopup
        objectName: "icloudPhotosPopup"
        modal: true
        focus: true
        anchors.centerIn: parent
        width: Math.min(880, root.width - 40)
        height: Math.min(620, root.height - 40)
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
                Text {
                    text: "☁️ " + i18n.tr("iCloud & iMessage Photos")
                    font.pixelSize: theme.fontSize + 3
                    font.bold: true
                    color: theme.brightForeground
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: backend.icloudPhotosSyncing
                          ? ("⏳ " + i18n.tr("Syncing Photos..."))
                          : ("⬇ " + i18n.tr("Sync All Photos"))
                    enabled: !backend.icloudPhotosSyncing
                    onClicked: {
                        if (backend.icloudPhotosSyncing) return
                        backend.syncIcloudPhotos()
                    }
                }
                Button {
                    text: "🔄 " + i18n.tr("Refresh")
                    onClicked: backend.fetchIcloudPhotos()
                }
                Button {
                    text: "✕"
                    onClicked: icloudPhotosPopup.close()
                }
            }

            GridView {
                id: photosGridView
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                cellWidth: 170
                cellHeight: 250
                model: backend.icloudPhotos

                delegate: Rectangle {
                    width: 160
                    height: 240
                    radius: 8
                    color: theme.background
                    border.color: Qt.rgba(1, 1, 1, 0.1)
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 4

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: modelData.isPortrait ? 170 : 130
                            radius: 6
                            color: theme.darkBackground
                            clip: true

                            Image {
                                id: thumbImg
                                anchors.fill: parent
                                anchors.margins: 2
                                fillMode: Image.PreserveAspectFit
                                autoTransform: true
                                smooth: true
                                source: (modelData.guid && !modelData.isCloudEvicted) ? backend.attachmentDownloadUrl(modelData.guid) : ""
                                asynchronous: true
                            }

                            Text {
                                anchors.centerIn: parent
                                text: "🖼"
                                font.pixelSize: 24
                                visible: thumbImg.status !== Image.Ready
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
                                    if (modelData.guid && !modelData.isCloudEvicted) {
                                        root.openPhotoViewer(backend.icloudPhotos, index)
                                    } else if (modelData.guid && modelData.isCloudEvicted && modelData.canUnpurge !== false) {
                                        backend.downloadCloudAttachment(modelData.guid)
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
                                enabled: !modelData.isCloudEvicted || modelData.canUnpurge !== false
                                text: modelData.isCloudEvicted ? (modelData.canUnpurge === false ? "☁ iCloud Only" : "☁ Unpurge") : "View"
                                onClicked: {
                                    if (!modelData.guid) return
                                    if (modelData.isCloudEvicted) {
                                        if (modelData.canUnpurge === false) return
                                        backend.downloadCloudAttachment(modelData.guid)
                                    } else {
                                        root.openPhotoViewer(backend.icloudPhotos, index)
                                    }
                                }
                            }

                            Button {
                                text: "📤"
                                enabled: backend.selectedChatGuid !== "" && !!modelData.guid && !modelData.isCloudEvicted
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
            width: 440
            height: 410
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

                Item { Layout.fillHeight: true }

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
            width: Math.min(540, root.width - 40)
            height: Math.min(root.height - 32, serverConfigCol.implicitHeight + 44)
            anchors.centerIn: parent
            radius: 12
            clip: true
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent

            MouseArea { anchors.fill: parent }

            ColumnLayout {
                id: serverConfigCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 22
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        Layout.fillWidth: true
                        text: "⚙ " + i18n.tr("BluePill & BlueBubbles Server Configuration")
                        font.pixelSize: theme.fontSize + 2
                        font.bold: true
                        color: theme.brightForeground
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        visible: backend.hasPasswordConfigured && backend.serverUrl.length > 0
                        height: 22
                        radius: 6
                        implicitWidth: cfgStatusBadge.implicitWidth + 14
                        color: backend.connected ? Qt.rgba(48/255, 209/255, 88/255, 0.16) : theme.lighterBackground
                        border.width: 1
                        border.color: backend.connected ? theme.green : theme.muted
                        Text {
                            id: cfgStatusBadge
                            anchors.centerIn: parent
                            text: backend.connected ? i18n.tr("Connected") : i18n.tr("Configured")
                            font.pixelSize: theme.fontSize - 3
                            font.bold: true
                            color: backend.connected ? theme.green : theme.foreground
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: i18n.tr("Connect to a BluePill macOS Relay (brew install nertwork/tap/bluepill), a legacy BlueBubbles Server (https://...), or Direct SSH (ssh://user@mac). Credentials are saved with 0600 permissions to ~/.config/bluepill/config.json.")
                    font.pixelSize: theme.fontSize - 2
                    color: theme.darkForeground
                    wrapMode: Text.WordWrap
                }

                // Primary Server URL
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Text {
                        Layout.fillWidth: true
                        text: i18n.tr("Primary Server URL")
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
                        border.color: serverUrlInput.activeFocus ? theme.accent : theme.muted

                        TextInput {
                            id: serverUrlInput
                            objectName: "serverUrlInput"
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            clip: true
                            verticalAlignment: TextInput.AlignVCenter
                            text: backend.serverUrl
                            color: theme.brightForeground
                            font.pixelSize: theme.fontSize
                            selectByMouse: true
                            KeyNavigation.tab: secondaryServerUrlInput
                            Keys.onReturnPressed: {
                                backend.saveServerConfig(serverUrlInput.text, secondaryServerUrlInput.text, serverPasswordInput.text)
                                root.showServerConfigModal = false
                            }
                            Text {
                                text: "https://bluebubbles.example.com"
                                color: theme.darkForeground
                                visible: !serverUrlInput.text && !serverUrlInput.activeFocus
                                anchors.verticalCenter: parent.verticalCenter
                                font.pixelSize: theme.fontSize
                                elide: Text.ElideRight
                                width: parent.width
                            }
                        }
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
                                backend.saveServerConfig(serverUrlInput.text, secondaryServerUrlInput.text, serverPasswordInput.text)
                                root.showServerConfigModal = false
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
                                    backend.saveServerConfig(serverUrlInput.text, secondaryServerUrlInput.text, serverPasswordInput.text)
                                    root.showServerConfigModal = false
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
                                backend.saveServerConfig(serverUrlInput.text, secondaryServerUrlInput.text, serverPasswordInput.text)
                                root.showServerConfigModal = false
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
                        { keys: "Ctrl+H / Esc", desc: "Open iMessage Home Page dashboard" },
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
            menuY = Math.min(Math.max(12, gy), root.height - 230)
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
                                var parts = t.split(" ")
                                root.contactModalFirstName = parts[0] || ""
                                root.contactModalLastName = parts.slice(1).join(" ") || ""
                                root.contactModalPhone = addr.length > 0 ? addr : backend.selectedChatAddress
                                root.contactModalEmail = ""
                                root.showContactModal = true
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
                                            : ({ guid: "", name: "", localUrl: "" })

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

        // Top Header Bar (Filename, Index Counter, Zoom Controls, Close Button)
        Rectangle {
            id: photoViewerTopBar
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 56
            color: "#cc131620"
            z: 5

            MouseArea { anchors.fill: parent }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 12

                Text {
                    text: "📷  " + (photoViewerModal.currentPhoto.name || i18n.tr("Photo"))
                    font.pixelSize: theme.fontSize + 1
                    font.bold: true
                    color: theme.brightForeground
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 380
                }

                Rectangle {
                    visible: root.photoViewerItems.length > 1
                    height: 24
                    implicitWidth: viewerCounterTxt.implicitWidth + 16
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

                Item { Layout.fillWidth: true }

                // Zoom Out (-)
                Rectangle {
                    height: 32
                    width: 34
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
                    height: 32
                    implicitWidth: zoomPctTxt.implicitWidth + 18
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
                    height: 32
                    width: 34
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

                // Close Button (Esc)
                Rectangle {
                    height: 32
                    implicitWidth: closeViewerTxt.implicitWidth + 22
                    radius: 7
                    color: closeViewerMouse.containsMouse ? "#c53b53" : theme.lighterBackground
                    border.width: 1
                    border.color: closeViewerMouse.containsMouse ? "#f7768e" : theme.muted
                    Text {
                        id: closeViewerTxt
                        anchors.centerIn: parent
                        text: "✕  " + i18n.tr("Close (Esc)")
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
            anchors.margins: 16
            clip: true

            Image {
                id: fullPhotoImage
                objectName: "fullPhotoImage"
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: root.photoViewerPanX
                anchors.verticalCenterOffset: root.photoViewerPanY
                width: Math.max(120, parent.width - (root.photoViewerItems.length > 1 ? 140 : 32))
                height: Math.max(120, parent.height - 24)
                source: photoViewerModal.currentPhoto.localUrl || ""
                asynchronous: true
                cache: true
                autoTransform: true
                smooth: true
                mipmap: true
                fillMode: Image.PreserveAspectFit
                scale: root.photoViewerZoom
                visible: status === Image.Ready

                MouseArea {
                    id: imageCanvasMouse
                    anchors.fill: parent
                    cursorShape: root.photoViewerZoom > 1.0 ? Qt.OpenHandCursor : Qt.PointingHandCursor
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
                            root.photoViewerPanX += (mouse.x - lastX) * root.photoViewerZoom
                            root.photoViewerPanY += (mouse.y - lastY) * root.photoViewerZoom
                        }
                    }
                    onReleased: dragging = false
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
            }

            // Loading / Downloading fallback state
            Rectangle {
                visible: fullPhotoImage.status !== Image.Ready
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
                        text: "📷 " + (photoViewerModal.currentPhoto.name || i18n.tr("Loading photo..."))
                        font.pixelSize: theme.fontSize
                        font.bold: true
                        color: theme.brightForeground
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: i18n.tr("Preparing high-resolution preview...")
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

            Row {
                anchors.centerIn: parent
                spacing: 10

                Repeater {
                    model: root.photoViewerItems
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
                            source: modelData.localUrl || ""
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
                                if (cur && (!cur.localUrl || cur.localUrl.length === 0) && cur.guid && cur.guid.length > 0) {
                                    backend.openAttachment(cur.guid, cur.name || "")
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
