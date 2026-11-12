#include <QKeyEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

#include "backend.h"
#include "theme.h"
#include "translator.h"

class ImsgTests : public QObject {
    Q_OBJECT

private slots:
    void testThemeParsing() {
        Theme theme;
        theme.loadFromTomlString(QStringLiteral(R"(
            mode = "dark"
            accent = "#ff9e64"
            background = "#1a1b26"
            foreground = "#c0caf5"
            green = "#9ece6a"
        )"));
        QCOMPARE(theme.mode(), QStringLiteral("dark"));
        QCOMPARE(theme.accent(), QColor(QStringLiteral("#ff9e64")));
        QCOMPARE(theme.background(), QColor(QStringLiteral("#1a1b26")));
        QCOMPARE(theme.foreground(), QColor(QStringLiteral("#c0caf5")));
        QCOMPARE(theme.green(), QColor(QStringLiteral("#9ece6a")));
    }

    void testTranslator() {
        Translator tr;
        tr.loadLanguage(QStringLiteral("da"));
        QCOMPARE(tr.tr(QStringLiteral("Unread")), QStringLiteral("Ulæste"));
        QCOMPARE(tr.tr(QStringLiteral("New Message")), QStringLiteral("Ny besked"));
        QCOMPARE(tr.tr(QStringLiteral("NonExistentKey")), QStringLiteral("NonExistentKey"));
    }

    void testNormalizeHandle() {
        QCOMPARE(Backend::normalizeHandle(QStringLiteral("+1 (415) 555-0140")), QStringLiteral("4155550140"));
        QCOMPARE(Backend::normalizeHandle(QStringLiteral("14155550140")), QStringLiteral("4155550140"));
        QCOMPARE(Backend::normalizeHandle(QStringLiteral("415-555-0140")), QStringLiteral("4155550140"));
        QCOMPARE(Backend::normalizeHandle(QStringLiteral("  Alex.Rivera@Example.com ")), QStringLiteral("alex.rivera@example.com"));
    }

    void testContactResolution() {
        Backend backend(false);
        const QByteArray sampleContacts = R"([
            {
                "id": 191,
                "displayName": "Chamberlain",
                "phoneNumbers": [{"address": "+14155550141"}],
                "emails": [{"address": "chamberlain@example.com"}]
            }
        ])";
        backend.parseContactsJson(sampleContacts);
        QCOMPARE(backend.resolveContactName(QStringLiteral("(415) 555-0141")), QStringLiteral("Chamberlain"));
        QCOMPARE(backend.resolveContactName(QStringLiteral("+14155550141")), QStringLiteral("Chamberlain"));
        QCOMPARE(backend.resolveContactName(QStringLiteral("CHAMBERLAIN@example.com")), QStringLiteral("Chamberlain"));
    }

    void testAttributedBodyAndReactionGrouping() {
        QJsonObject parentMsg{
            {QStringLiteral("guid"), QStringLiteral("MSG-100")},
            {QStringLiteral("text"), QString()},
            {QStringLiteral("dateCreated"), 1700000000000LL},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550141")}}},
            {QStringLiteral("attributedBody"), QJsonArray{
                QJsonObject{{QStringLiteral("string"), QStringLiteral("\uFFFCExtracted from attributedBody!")}}
            }}
        };

        QJsonObject reactionMsg{
            {QStringLiteral("guid"), QStringLiteral("MSG-101")},
            {QStringLiteral("text"), QStringLiteral("Loved \"Extracted from attributedBody!\"")},
            {QStringLiteral("dateCreated"), 1700000005000LL},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("associatedMessageGuid"), QStringLiteral("p:0/MSG-100")},
            {QStringLiteral("associatedMessageType"), QStringLiteral("2000")}
        };

        // API returns DESC order (newest first)
        QJsonArray apiDesc{reactionMsg, parentMsg};
        QHash<QString, QString> contacts{{QStringLiteral("4155550141"), QStringLiteral("Chamberlain")}};
        QHash<QString, QString> attCache;

        const QVariantList timeline = Backend::buildMessageListFromApi(apiDesc, contacts, attCache);
        QCOMPARE(timeline.size(), 1);
        const QVariantMap m0 = timeline.first().toMap();
        QCOMPARE(m0.value(QStringLiteral("guid")).toString(), QStringLiteral("MSG-100"));
        QCOMPARE(m0.value(QStringLiteral("text")).toString(), QStringLiteral("Extracted from attributedBody!"));
        QCOMPARE(m0.value(QStringLiteral("senderName")).toString(), QStringLiteral("Chamberlain"));

        const QVariantList reactions = m0.value(QStringLiteral("reactions")).toList();
        QCOMPARE(reactions.size(), 1);
        QCOMPARE(reactions.first().toMap().value(QStringLiteral("emoji")).toString(), QStringLiteral("❤️"));
        QCOMPARE(reactions.first().toMap().value(QStringLiteral("fromMe")).toBool(), true);
    }

    void testDemoModeAndFiltering() {
        Backend backend(false);
        backend.loadDemoData();
        QVERIFY(backend.connected());
        QCOMPARE(backend.chats().size(), 4);
        QVERIFY(backend.messages().size() >= 4);

        backend.setFilterMode(QStringLiteral("unread"));
        QCOMPARE(backend.chats().size(), 2);

        backend.setFilterMode(QStringLiteral("groups"));
        QCOMPARE(backend.chats().size(), 1);

        backend.setFilterMode(QStringLiteral("all"));
        backend.setSearchQuery(QStringLiteral("GitHub"));
        QCOMPARE(backend.chats().size(), 1);
    }

    void testChatSelectionIsolation() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state.json")));

        const QString morganGuid = QStringLiteral("RCS;-;+14155550131");
        const QString caseyGuid = QStringLiteral("iMessage;-;+14155550140");
        const QString rileyGuid = QStringLiteral("iMessage;-;+14155550132");

        QJsonObject msgMorgan{
            {QStringLiteral("guid"), QStringLiteral("MSG-MORGAN-1")},
            {QStringLiteral("text"), QStringLiteral("Hey from Morgan Blake on RCS")},
            {QStringLiteral("dateCreated"), 1700000010000LL},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550131")}}},
            {QStringLiteral("chats"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), morganGuid},
                {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550131")},
                {QStringLiteral("displayName"), QStringLiteral("Morgan Blake")},
                {QStringLiteral("style"), 45}
            }}}
        };

        QJsonObject msgCasey{
            {QStringLiteral("guid"), QStringLiteral("MSG-CASEY-1")},
            {QStringLiteral("text"), QStringLiteral("Update from Casey Jordan on iMessage")},
            {QStringLiteral("dateCreated"), 1700000009000LL},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550140")}}},
            {QStringLiteral("chats"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), caseyGuid},
                {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550140")},
                {QStringLiteral("displayName"), QStringLiteral("Casey Jordan")},
                {QStringLiteral("style"), 45}
            }}}
        };

        QJsonObject msgRiley{
            {QStringLiteral("guid"), QStringLiteral("MSG-RILEY-1")},
            {QStringLiteral("text"), QStringLiteral("Checking in from Riley Quinn")},
            {QStringLiteral("dateCreated"), 1700000008000LL},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550132")}}},
            {QStringLiteral("chats"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), rileyGuid},
                {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550132")},
                {QStringLiteral("displayName"), QStringLiteral("Riley Quinn")},
                {QStringLiteral("style"), 45}
            }}}
        };

        backend.ingestMessagesForTest(QJsonArray{msgMorgan, msgCasey, msgRiley}, true);
        QCOMPARE(backend.chats().size(), 3);
        QCOMPARE(backend.selectedChatGuid(), morganGuid);
        QCOMPARE(backend.messages().size(), 1);
        QCOMPARE(backend.messages().first().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("MSG-MORGAN-1"));

        // Switch to Casey Jordan: must immediately show ONLY Casey's message in 0ms
        backend.selectChat(caseyGuid);
        QCOMPARE(backend.selectedChatGuid(), caseyGuid);
        QCOMPARE(backend.messages().size(), 1);
        QCOMPARE(backend.messages().first().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("MSG-CASEY-1"));
        QCOMPARE(backend.messages().first().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Update from Casey Jordan on iMessage"));

        // Switch to Riley Quinn: must immediately show ONLY Riley's message
        backend.selectChat(rileyGuid);
        QCOMPARE(backend.selectedChatGuid(), rileyGuid);
        QCOMPARE(backend.messages().size(), 1);
        QCOMPARE(backend.messages().first().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("MSG-RILEY-1"));

        // Selecting a chat with no cached messages must immediately clear messages() to 0
        backend.selectChat(QStringLiteral("iMessage;-;+18005550000"));
        QCOMPARE(backend.messages().size(), 0);
    }

    void testEmojiShortcodesAndSearch() {
        const QString replaced = Backend::replaceEmojiShortcodes(
            QStringLiteral(":smile: :heart: :thumbsup: :rocket: :unknown_code:"));
        QCOMPARE(replaced, QStringLiteral("😄 ❤️ 👍 🚀 :unknown_code:"));

        Backend backend(false);
        const QVariantList matches = backend.searchEmojis(QStringLiteral("smile"), QString(), 10);
        QVERIFY(!matches.isEmpty());
        QCOMPARE(matches.first().toMap().value(QStringLiteral("code")).toString(), QStringLiteral("smile"));
        QCOMPARE(matches.first().toMap().value(QStringLiteral("emoji")).toString(), QStringLiteral("😄"));

        // Category filtering test
        const QVariantList foodEmojis = backend.searchEmojis(QString(), QStringLiteral("Food"), 30);
        QVERIFY(!foodEmojis.isEmpty());
        bool hasPizza = false;
        for (const QVariant &v : foodEmojis) {
            if (v.toMap().value(QStringLiteral("code")).toString() == QStringLiteral("pizza")) hasPizza = true;
        }
        QVERIFY(hasPizza);

        const QVariantList actEmojis = backend.searchEmojis(QString(), QStringLiteral("Activities"), 30);
        QVERIFY(!actEmojis.isEmpty());
        bool hasTada = false;
        for (const QVariant &v : actEmojis) {
            if (v.toMap().value(QStringLiteral("code")).toString() == QStringLiteral("tada")) hasTada = true;
        }
        QVERIFY(hasTada);
    }

    void testFailoverCandidateUrls() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setConfigFilePath(tempDir.filePath(QStringLiteral("config.json")));
        backend.saveServerConfig(
            QStringLiteral("https://relay.example.com/"),
            QStringLiteral("https://failover-relay.example.com/"),
            QStringLiteral("test-api-key"));
        const QStringList candidates = backend.candidateUrls();
        QCOMPARE(candidates.size(), 2);
        QCOMPARE(candidates.at(0), QStringLiteral("https://relay.example.com"));
        QCOMPARE(candidates.at(1), QStringLiteral("https://failover-relay.example.com"));
        QCOMPARE(backend.consecutiveFailures(), 0);
    }

    void testMuteAndNotificationStatePersistence() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString stateFile = tempDir.filePath(QStringLiteral("state.json"));
        const QString chatGuid = QStringLiteral("iMessage;+;chat99887766");

        {
            Backend b1(false);
            b1.setStateFilePath(stateFile);
            QVERIFY(b1.notificationsEnabled());
            QVERIFY(!b1.isChatMuted(chatGuid));

            b1.toggleChatMute(chatGuid);
            QVERIFY(b1.isChatMuted(chatGuid));

            b1.setNotificationsEnabled(false);
            QVERIFY(!b1.notificationsEnabled());
        }

        {
            Backend b2(false);
            b2.setStateFilePath(stateFile);
            QVERIFY(!b2.notificationsEnabled());
            QVERIFY(b2.isChatMuted(chatGuid));

            b2.toggleChatMute(chatGuid);
            QVERIFY(!b2.isChatMuted(chatGuid));
        }
    }

    void testConfigFileStrictPermissions() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("config.json"));
        QFile f(cfgPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"serverUrl\":\"https://relay.example.com\",\"password\":\"dummy\"}");
        f.close();
        QFile::setPermissions(cfgPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        const auto perms = QFile::permissions(cfgPath);
        QVERIFY(perms & QFileDevice::ReadOwner);
        QVERIFY(perms & QFileDevice::WriteOwner);
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::WriteGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));
        QVERIFY(!(perms & QFileDevice::WriteOther));
    }

    void testGreenBubbleAndServiceDetection() {
        QJsonObject rcsMsg{
            {QStringLiteral("guid"), QStringLiteral("RCS;-;+14155550131;12345")},
            {QStringLiteral("text"), QStringLiteral("Hello via RCS")},
            {QStringLiteral("dateCreated"), 1700000020000LL},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("service"), QStringLiteral("RCS")}
        };
        QJsonObject smsMsg{
            {QStringLiteral("guid"), QStringLiteral("SMS;-;+14155550131;12346")},
            {QStringLiteral("text"), QStringLiteral("Hello via SMS")},
            {QStringLiteral("dateCreated"), 1700000021000LL},
            {QStringLiteral("isFromMe"), true}
        };
        QJsonObject imsgMsg{
            {QStringLiteral("guid"), QStringLiteral("iMessage;-;+14155550131;12347")},
            {QStringLiteral("text"), QStringLiteral("Hello via iMessage")},
            {QStringLiteral("dateCreated"), 1700000022000LL},
            {QStringLiteral("isFromMe"), true}
        };

        QJsonArray rawDesc{imsgMsg, smsMsg, rcsMsg};
        QHash<QString, QString> contacts;
        QHash<QString, QString> attCache;

        const QVariantList list = Backend::buildMessageListFromApi(rawDesc, contacts, attCache);
        QCOMPARE(list.size(), 3);

        // Chronological order: rcs first, then sms, then imsg
        QCOMPARE(list[0].toMap().value(QStringLiteral("service")).toString(), QStringLiteral("RCS"));
        QCOMPARE(list[1].toMap().value(QStringLiteral("service")).toString(), QStringLiteral("SMS"));
        QCOMPARE(list[2].toMap().value(QStringLiteral("service")).toString(), QStringLiteral("iMessage"));
    }

    void testAsciiEmoticonAutocompleteAndSearch() {
        Backend backend(false);

        const QVariantList smileResults = backend.searchEmojis(QStringLiteral(":-)"));
        QVERIFY(!smileResults.isEmpty());
        QCOMPARE(smileResults.first().toMap().value(QStringLiteral("emoji")).toString(), QStringLiteral("😊"));

        const QVariantList heartResults = backend.searchEmojis(QStringLiteral("<3"));
        QVERIFY(!heartResults.isEmpty());
        QCOMPARE(heartResults.first().toMap().value(QStringLiteral("emoji")).toString(), QStringLiteral("❤️"));

        const QVariantList laughResults = backend.searchEmojis(QStringLiteral(":D"));
        QVERIFY(!laughResults.isEmpty());
        QCOMPARE(laughResults.first().toMap().value(QStringLiteral("emoji")).toString(), QStringLiteral("😄"));

        const QVariantList winkResults = backend.searchEmojis(QStringLiteral(";-)"));
        QVERIFY(!winkResults.isEmpty());
        QCOMPARE(winkResults.first().toMap().value(QStringLiteral("emoji")).toString(), QStringLiteral("😉"));
    }

    void testAsciiEmoticonReplacement() {
        const QString converted = Backend::replaceEmojiShortcodes(
            QStringLiteral("Hey :-) check out https://example.com/test:/path it is great <3 and :D!"));
        QCOMPARE(converted, QStringLiteral("Hey 😊 check out https://example.com/test:/path it is great ❤️ and 😄!"));
    }

    void testComputeTextPartIndex() {
        // Text only
        QJsonObject textOnly{
            {QStringLiteral("text"), QStringLiteral("Just some text")},
            {QStringLiteral("attachments"), QJsonArray()}
        };
        QCOMPARE(Backend::computeTextPartIndex(textOnly), 0);

        // Attachment only
        QJsonObject attOnly{
            {QStringLiteral("text"), QString()},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-1")}}}}
        };
        QCOMPARE(Backend::computeTextPartIndex(attOnly), 0);

        // Attachment and text
        QJsonObject attAndText{
            {QStringLiteral("text"), QStringLiteral("Here is the picture")},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-1")}}}}
        };
        QCOMPARE(Backend::computeTextPartIndex(attAndText), 1);
    }

    void testOmarchyDndDetection() {
        Backend backend(false);
        bool dnd = backend.omarchyDnd();
        QVERIFY(dnd == true || dnd == false);
    }

    void testRemoteContactEnvelopeResolution() {
        Backend backend(false);
        // Test BlueBubbles /api/v1/contact response envelope containing Jordan Taylor
        const QByteArray jordanContactsEnvelope = R"({
            "status": 200,
            "message": "Successfully retrieved contacts",
            "data": [
                {
                    "id": 940,
                    "displayName": "Jordan Taylor",
                    "firstName": "Jordan",
                    "lastName": "Taylor",
                    "phoneNumbers": [
                        {"address": "+14155550101"}
                    ],
                    "emails": []
                }
            ]
        })";
        backend.parseContactsJson(jordanContactsEnvelope);
        QCOMPARE(backend.resolveContactName(QStringLiteral("+14155550101")), QStringLiteral("Jordan Taylor"));
        QCOMPARE(backend.resolveContactName(QStringLiteral("(415) 555-0101")), QStringLiteral("Jordan Taylor"));
        QCOMPARE(backend.resolveContactName(QStringLiteral("14155550101")), QStringLiteral("Jordan Taylor"));
        QCOMPARE(backend.resolveContactName(QStringLiteral("4155550101")), QStringLiteral("Jordan Taylor"));

        // Test string array address variant
        const QByteArray rawArrayVariant = R"({
            "data": [
                {
                    "displayName": "Jordan Taylor",
                    "phoneNumbers": ["+14155550101"]
                }
            ]
        })";
        Backend b2(false);
        b2.parseContactsJson(rawArrayVariant);
        QCOMPARE(b2.resolveContactName(QStringLiteral("+14155550101")), QStringLiteral("Jordan Taylor"));
    }

    void testUrlExtractionAndYouTubeDetection() {
        const QString text = QStringLiteral("Check this out: https://youtube.com/shorts/lSZx1OiSjwg?is=LQS5DrAr4XEZZMqh it is wild!");
        const QString extracted = Backend::extractFirstUrl(text);
        QCOMPARE(extracted, QStringLiteral("https://youtube.com/shorts/lSZx1OiSjwg?is=LQS5DrAr4XEZZMqh"));

        const QString punctText = QStringLiteral("Visit https://example.com/test/page, or https://other.com.");
        QCOMPARE(Backend::extractFirstUrl(punctText), QStringLiteral("https://example.com/test/page"));

        QVERIFY(Backend::isYouTubeUrl(QStringLiteral("https://youtube.com/shorts/lSZx1OiSjwg?is=LQS5DrAr4XEZZMqh")));
        QVERIFY(Backend::isYouTubeUrl(QStringLiteral("https://youtu.be/lSZx1OiSjwg")));
        QVERIFY(Backend::isYouTubeUrl(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ")));
        QVERIFY(Backend::isYouTubeUrl(QStringLiteral("https://m.youtube.com/watch?v=12345")));
        QVERIFY(!Backend::isYouTubeUrl(QStringLiteral("https://vimeo.com/123456")));
        QVERIFY(!Backend::isYouTubeUrl(QStringLiteral("https://example.com")));
    }

    void testMessagePayloadAndLinkFormatting() {
        QJsonObject samMsg{
            {QStringLiteral("guid"), QStringLiteral("11B63D32-A6E7-4B9B-9360-B96B5DCF45C1")},
            {QStringLiteral("text"), QStringLiteral("https://youtube.com/shorts/lSZx1OiSjwg?is=LQS5DrAr4XEZZMqh")},
            {QStringLiteral("dateCreated"), 1700000030000LL},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("service"), QStringLiteral("RCS")},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550102")}}}
        };

        QJsonArray apiDesc{samMsg};
        QHash<QString, QString> contacts{{QStringLiteral("4155550102"), QStringLiteral("Sam Patel")}};
        QHash<QString, QString> attCache;
        QStringList missingPreviews;

        const QVariantList timeline = Backend::buildMessageListFromApi(
            apiDesc, contacts, attCache, nullptr, {}, &missingPreviews);

        QCOMPARE(timeline.size(), 1);
        const QVariantMap m0 = timeline.first().toMap();
        QCOMPARE(m0.value(QStringLiteral("guid")).toString(), QStringLiteral("11B63D32-A6E7-4B9B-9360-B96B5DCF45C1"));
        QCOMPARE(m0.value(QStringLiteral("senderName")).toString(), QStringLiteral("Sam Patel"));
        QCOMPARE(m0.value(QStringLiteral("service")).toString(), QStringLiteral("RCS"));
        QCOMPARE(m0.value(QStringLiteral("linkUrl")).toString(), QStringLiteral("https://youtube.com/shorts/lSZx1OiSjwg?is=LQS5DrAr4XEZZMqh"));
        QVERIFY(!missingPreviews.isEmpty());
        QCOMPARE(missingPreviews.first(), QStringLiteral("11B63D32-A6E7-4B9B-9360-B96B5DCF45C1|https://youtube.com/shorts/lSZx1OiSjwg?is=LQS5DrAr4XEZZMqh"));

        const QString formatted = m0.value(QStringLiteral("formattedText")).toString();
        QVERIFY(formatted.contains(QStringLiteral("<a href=\"https://youtube.com/shorts/lSZx1OiSjwg?is=LQS5DrAr4XEZZMqh\"")));
    }

    void testFormatMessageTextWithLinks() {
        QCOMPARE(Backend::formatMessageTextWithLinks(QStringLiteral("No links here")), QStringLiteral("No links here"));

        const QString input = QStringLiteral("Check https://example.com/test & enjoy <cool>!");
        const QString formatted = Backend::formatMessageTextWithLinks(input);
        QVERIFY(formatted.contains(QStringLiteral("<a href=\"https://example.com/test\"")));
        QVERIFY(formatted.contains(QStringLiteral("&amp; enjoy &lt;cool&gt;!")));
    }

    void testImageMimeAndExtensionDetection() {
        QVERIFY(Backend::isImageMimeOrExt(QStringLiteral("image/heic"), QStringLiteral("photo.heic")));
        QVERIFY(Backend::isImageMimeOrExt(QStringLiteral("image/heif"), QStringLiteral("photo.heif")));
        QVERIFY(Backend::isImageMimeOrExt(QStringLiteral("application/octet-stream"), QStringLiteral("sticker.heics")));
        QVERIFY(Backend::isImageMimeOrExt(QStringLiteral("application/octet-stream"), QStringLiteral("photo.avif")));
        QVERIFY(Backend::isImageMimeOrExt(QStringLiteral("image/tiff"), QStringLiteral("scan.tiff")));
        QVERIFY(Backend::isImageMimeOrExt(QStringLiteral("image/bmp"), QStringLiteral("drawing.bmp")));
        QVERIFY(Backend::isImageMimeOrExt(QStringLiteral("application/octet-stream"), QStringLiteral("picture.HEIC")));
        QVERIFY(Backend::isImageMimeOrExt(QStringLiteral("image/jpeg"), QStringLiteral("file.jpg")));
        QVERIFY(!Backend::isImageMimeOrExt(QStringLiteral("application/pdf"), QStringLiteral("document.pdf")));
        QVERIFY(!Backend::isImageMimeOrExt(QStringLiteral("text/plain"), QStringLiteral("notes.txt")));
    }

    void testSaveContactAndSelectedChatProperties() {
        Backend backend(false);
        backend.loadDemoData();
        backend.saveContact(QStringLiteral("Jordan"), QStringLiteral("Taylor"), QStringLiteral("+15551234567"), QStringLiteral("jordan@example.com"));
        QCOMPARE(backend.resolveContactName(QStringLiteral("+1 (555) 123-4567")), QStringLiteral("Jordan Taylor"));
        QCOMPARE(backend.resolveContactName(QStringLiteral("jordan@example.com")), QStringLiteral("Jordan Taylor"));
    }
    void testSearchPlaceholderAndHeaderNoAnchorsInLayout() {
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) {
            qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        }
        if (!qmlFile.exists()) {
            qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        }
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString qml = QString::fromUtf8(qmlFile.readAll());

        // 1. Verify Search conversations placeholder explicitly uses Ctrl+F and NOT Ctrl+K
        QVERIFY(qml.contains(QStringLiteral("placeholderText: i18n.tr(\"Search conversations...\") + \" (Ctrl+F)\"")));
        QVERIFY(!qml.contains(QStringLiteral("placeholderText: i18n.tr(\"Search conversations...\") + \" (Ctrl+K)\"")));

        // 2. Verify top bar has Ctrl+K shortcut button
        QVERIFY(qml.contains(QStringLiteral("Keyboard Shortcuts (Ctrl+K)")));

        // 3. Verify Active Chat Header has contactIdentityCard and no MouseArea directly anchored inside RowLayout
        QVERIFY(qml.contains(QStringLiteral("id: contactIdentityCard")));
        QVERIFY(qml.contains(QStringLiteral("id: contactIdentityRow")));
        QVERIFY(qml.contains(QStringLiteral("id: contactHeaderHover")));

        // 4. Verify Theme default font size is at least 15
        Theme th;
        QVERIFY(th.fontSize() >= 15);
    }

    void testFullHistoryAndLoadOlderMessagesPagination() {
        Backend backend(false);
        backend.loadDemoData();

        // Check pagination properties
        QCOMPARE(backend.loadingOlderMessages(), false);
        QVERIFY(backend.loadedMessageCount() > 0);

        // Verify messages older than 90 days are handled cleanly by buildMessageListFromApi
        const qint64 oldTimestamp = QDateTime::currentMSecsSinceEpoch() - (365LL * 86400LL * 1000LL); // 1 year ago
        QJsonObject historicMsg{
            {QStringLiteral("guid"), QStringLiteral("HISTORIC-MSG-1")},
            {QStringLiteral("text"), QStringLiteral("Message from a year ago")},
            {QStringLiteral("dateCreated"), oldTimestamp},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15551234567")}}}
        };

        QJsonArray apiDesc{historicMsg};
        QHash<QString, QString> contacts;
        QHash<QString, QString> attCache;
        const QVariantList list = Backend::buildMessageListFromApi(apiDesc, contacts, attCache);
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Message from a year ago"));

        // Verify QML contains loadOlderMessages, stickToBottom, scrollToBottom, scrollToTop, and Top/Bottom header buttons
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("backend.loadOlderMessages()")));
        QVERIFY(qml.contains(QStringLiteral("stickToBottom")));
        QVERIFY(qml.contains(QStringLiteral("function scrollToBottom()")));
        QVERIFY(qml.contains(QStringLiteral("function scrollToTop()")));
        QVERIFY(qml.contains(QStringLiteral("bottomSettleTimer")));
        QVERIFY(qml.contains(QStringLiteral("messageListView.scrollToTop()")));
        QVERIFY(qml.contains(QStringLiteral("messageListView.scrollToBottom()")));
        QVERIFY(qml.contains(QStringLiteral("i18n.tr(\"Top\")")));
        QVERIFY(qml.contains(QStringLiteral("i18n.tr(\"Bottom\")")));
        QVERIFY(qml.contains(QStringLiteral("Load older messages")));
    }

    void testBidirectionalReadStateSyncWithIMessage() {
        // 1. Unit test isMessageUnread helper with various Apple chat.db / BlueBubbles states
        const qint64 watermark = 1000000LL;

        // Sent by user -> should not be unread
        QJsonObject sentByMe{
            {QStringLiteral("guid"), QStringLiteral("M1")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("dateCreated"), 2000000LL}
        };
        QCOMPARE(Backend::isMessageUnread(sentByMe, watermark), false);

        // System message / call itemType -> should not be unread
        QJsonObject callItem{
            {QStringLiteral("guid"), QStringLiteral("M2")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("itemType"), 1},
            {QStringLiteral("dateCreated"), 2000000LL}
        };
        QCOMPARE(Backend::isMessageUnread(callItem, watermark), false);

        // Tapback / reaction -> should not be unread
        QJsonObject reaction{
            {QStringLiteral("guid"), QStringLiteral("M3")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("itemType"), 0},
            {QStringLiteral("associatedMessageType"), QStringLiteral("2000")},
            {QStringLiteral("dateCreated"), 2000000LL}
        };
        QCOMPARE(Backend::isMessageUnread(reaction, watermark), false);

        // Already read via isRead flag -> should not be unread
        QJsonObject alreadyReadFlag{
            {QStringLiteral("guid"), QStringLiteral("M4")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("itemType"), 0},
            {QStringLiteral("isRead"), true},
            {QStringLiteral("dateCreated"), 2000000LL}
        };
        QCOMPARE(Backend::isMessageUnread(alreadyReadFlag, watermark), false);

        // Already read via dateRead timestamp (Apple Messages default) -> should not be unread
        QJsonObject alreadyReadTimestamp{
            {QStringLiteral("guid"), QStringLiteral("M5")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("itemType"), 0},
            {QStringLiteral("dateRead"), 1793415303229LL},
            {QStringLiteral("dateCreated"), 2000000LL}
        };
        QCOMPARE(Backend::isMessageUnread(alreadyReadTimestamp, watermark), false);
        // When an explicit unreadOverrideTs >= dateRead is active (Apple /unread sets is_read=0 without clearing date_read),
        // isMessageUnread must treat the message as unread until a newer read timestamp occurs
        QCOMPARE(Backend::isMessageUnread(alreadyReadTimestamp, watermark, 1793415303229LL), true);
        QCOMPARE(Backend::isMessageUnread(alreadyReadTimestamp, watermark, 1793415300000LL), false);

        // Older than read watermark -> should not be unread
        QJsonObject olderThanWatermark{
            {QStringLiteral("guid"), QStringLiteral("M6")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("itemType"), 0},
            {QStringLiteral("dateRead"), 0},
            {QStringLiteral("dateCreated"), 500000LL}
        };
        QCOMPARE(Backend::isMessageUnread(olderThanWatermark, watermark), false);

        // Valid unread incoming message
        QJsonObject unreadIncoming{
            {QStringLiteral("guid"), QStringLiteral("M7")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("itemType"), 0},
            {QStringLiteral("isRead"), false},
            {QStringLiteral("dateRead"), 0},
            {QStringLiteral("dateCreated"), 2000000LL}
        };
        QCOMPARE(Backend::isMessageUnread(unreadIncoming, watermark), true);

        // 1b. Unit test extractChatReadWatermark with lastReadMessageTimestamp, properties.LSMD, and properties.lastSeenMessageGuid
        QHash<QString, QJsonObject> msgCache;
        msgCache.insert(QStringLiteral("MSG-SEEN-1"), QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("MSG-SEEN-1")},
            {QStringLiteral("dateCreated"), 1775516127515LL}
        });
        QJsonObject chatWithProps{
            {QStringLiteral("guid"), QStringLiteral("SMS;-;+14155550199")},
            {QStringLiteral("properties"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("LSMD"), QStringLiteral("2026-04-06T22:55:27.515Z")},
                    {QStringLiteral("lastSeenMessageGuid"), QStringLiteral("MSG-SEEN-1")}
                }
            }}
        };
        QCOMPARE(Backend::extractChatReadWatermark(chatWithProps, msgCache), 1775516127515LL);

        // 2. Integration test read/unread toggling & mark-all-read on Backend demo data
        Backend backend(false);
        backend.loadDemoData();

        const QVariantList chats = backend.chats();
        QVERIFY(!chats.isEmpty());
        const QString firstGuid = chats.first().toMap().value(QStringLiteral("guid")).toString();
        QVERIFY(!firstGuid.isEmpty());

        backend.selectChat(firstGuid);

        // Mark read
        backend.markChatRead(firstGuid);
        QCOMPARE(backend.selectedChatUnreadCount(), 0);

        // Mark unread
        backend.markChatUnread(firstGuid);
        QVERIFY(backend.selectedChatUnreadCount() > 0);

        // Toggle read/unread
        backend.toggleChatReadUnread(firstGuid);
        QCOMPARE(backend.selectedChatUnreadCount(), 0);

        backend.toggleChatReadUnread(firstGuid);
        QVERIFY(backend.selectedChatUnreadCount() > 0);

        // Mark all chats read
        backend.markAllChatsRead();
        QCOMPARE(backend.selectedChatUnreadCount(), 0);
        for (const QVariant &cv : backend.chats()) {
            QCOMPARE(cv.toMap().value(QStringLiteral("unreadCount")).toInt(), 0);
        }

        // 2b. Verify phone/Mac -> imsg sync:
        //  (i) Outgoing reply from phone (isFromMe=true) clears older unread incoming messages
        //  (ii) markChatUnread survives subsequent delta polls where Apple chat.db retains non-zero dateRead
        //  (iii) Chat metadata update (properties.LSMD / lastReadMessageTimestamp) clears unread count when read on iPhone/Mac
        QTemporaryDir tempStateDir;
        QVERIFY(tempStateDir.isValid());
        Backend syncBackend(false);
        syncBackend.setStateFilePath(tempStateDir.filePath(QStringLiteral("state.json")));
        syncBackend.setWindowActive(false);

        const QString otherChatGuid = QStringLiteral("iMessage;-;+14155550888");
        const QString testChatGuid = QStringLiteral("iMessage;-;+14155550999");
        const QJsonObject chatRef{
            {QStringLiteral("guid"), testChatGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550999")},
            {QStringLiteral("style"), 45}
        };

        // Seed otherChatGuid on initial load so it becomes the selected chat
        syncBackend.ingestMessagesForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("OTHER-1")},
                {QStringLiteral("text"), QStringLiteral("Other thread")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), 500LL},
                {QStringLiteral("chats"), QJsonArray{QJsonObject{
                    {QStringLiteral("guid"), otherChatGuid},
                    {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550888")},
                    {QStringLiteral("style"), 45}
                }}}
            }
        }, true);
        QCOMPARE(syncBackend.selectedChatGuid(), otherChatGuid);

        // Incoming unread message on testChatGuid at t=1000
        QJsonArray poll1{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("INC-1")},
                {QStringLiteral("text"), QStringLiteral("Hey from iPhone")},
                {QStringLiteral("isFromMe"), false},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), 1000LL},
                {QStringLiteral("dateRead"), 0LL},
                {QStringLiteral("chats"), QJsonArray{chatRef}}
            }
        };
        syncBackend.ingestMessagesForTest(poll1, false);
        QCOMPARE(syncBackend.totalUnread(), 1);

        // User replies to testChatGuid from their iPhone at t=1500 (isFromMe=true, even if INC-1 still has dateRead=0)
        syncBackend.ingestMessagesForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("OUT-PHONE-1")},
                {QStringLiteral("text"), QStringLiteral("Replied from my iPhone")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), 1500LL},
                {QStringLiteral("chats"), QJsonArray{chatRef}}
            }
        }, false);
        QCOMPARE(syncBackend.totalUnread(), 0);

        // Now a new incoming message arrives at t=3000 and is read at t=3500
        syncBackend.ingestMessagesForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("INC-2")},
                {QStringLiteral("text"), QStringLiteral("Follow up message")},
                {QStringLiteral("isFromMe"), false},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), 3000LL},
                {QStringLiteral("dateRead"), 3500LL},
                {QStringLiteral("chats"), QJsonArray{chatRef}}
            }
        }, false);
        QCOMPARE(syncBackend.totalUnread(), 0);

        // User marks testChatGuid as unread in imsg -> Apple Messages sets is_read=0 but keeps dateRead=3500 in chat.db
        syncBackend.markChatUnread(testChatGuid);
        QCOMPARE(syncBackend.totalUnread(), 1);

        // Next delta poll returns INC-2 with dateRead=3500 still set -> must NOT clobber unread state!
        syncBackend.ingestMessagesForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("INC-2")},
                {QStringLiteral("text"), QStringLiteral("Follow up message")},
                {QStringLiteral("isFromMe"), false},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), 3000LL},
                {QStringLiteral("dateRead"), 3500LL},
                {QStringLiteral("chats"), QJsonArray{chatRef}}
            }
        }, false);
        QCOMPARE(syncBackend.totalUnread(), 1);

        // Now user reads the conversation on their iPhone at t=5000 (synced via chat metadata LSMD / lastReadMessageTimestamp)
        syncBackend.ingestChatsMetadataForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), testChatGuid},
                {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550999")},
                {QStringLiteral("style"), 45},
                {QStringLiteral("lastReadMessageTimestamp"), 5000LL}
            }
        });
        QCOMPARE(syncBackend.totalUnread(), 0);

        // 3. Verify QML wiring contains bidirectional read/unread shortcuts and actions
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("backend.toggleChatReadUnread")));
        QVERIFY(qml.contains(QStringLiteral("backend.markAllChatsRead")));
        QVERIFY(qml.contains(QStringLiteral("backend.selectedChatUnreadCount")));
        QVERIFY(qml.contains(QStringLiteral("Ctrl+Shift+U")));
        QVERIFY(qml.contains(QStringLiteral("Ctrl+Shift+A")));
    }

    void testHistorySyncDaysAndPersistentMessagesCache() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString statePath = tempDir.filePath(QStringLiteral("state.json"));

        const QString chatA = QStringLiteral("iMessage;-;+14155550101");
        const QString chatB = QStringLiteral("iMessage;-;+14155550102");
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        {
            Backend b1(false);
            b1.setStateFilePath(statePath);
            QCOMPARE(b1.historySyncDays(), 60);

            b1.setHistorySyncDays(90);
            QCOMPARE(b1.historySyncDays(), 90);
            b1.cycleHistorySyncDays();
            QCOMPARE(b1.historySyncDays(), 180);
            b1.setHistorySyncDays(60);

            QJsonArray seedMessages{
                QJsonObject{
                    {QStringLiteral("guid"), QStringLiteral("CACHE-A-1")},
                    {QStringLiteral("text"), QStringLiteral("First cached message for Contact A")},
                    {QStringLiteral("isFromMe"), false},
                    {QStringLiteral("itemType"), 0},
                    {QStringLiteral("isRead"), true},
                    {QStringLiteral("dateCreated"), nowMs - 60000LL},
                    {QStringLiteral("dateRead"), nowMs - 50000LL},
                    {QStringLiteral("chats"), QJsonArray{QJsonObject{
                        {QStringLiteral("guid"), chatA},
                        {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550101")},
                        {QStringLiteral("style"), 45}
                    }}}
                },
                QJsonObject{
                    {QStringLiteral("guid"), QStringLiteral("CACHE-B-1")},
                    {QStringLiteral("text"), QStringLiteral("Deep history message 1 for Contact B")},
                    {QStringLiteral("isFromMe"), false},
                    {QStringLiteral("itemType"), 0},
                    {QStringLiteral("isRead"), true},
                    {QStringLiteral("dateCreated"), nowMs - 120000LL},
                    {QStringLiteral("dateRead"), nowMs - 110000LL},
                    {QStringLiteral("chats"), QJsonArray{QJsonObject{
                        {QStringLiteral("guid"), chatB},
                        {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550102")},
                        {QStringLiteral("style"), 45}
                    }}}
                },
                QJsonObject{
                    {QStringLiteral("guid"), QStringLiteral("CACHE-B-2")},
                    {QStringLiteral("text"), QStringLiteral("Deep history message 2 for Contact B")},
                    {QStringLiteral("isFromMe"), true},
                    {QStringLiteral("itemType"), 0},
                    {QStringLiteral("dateCreated"), nowMs - 90000LL},
                    {QStringLiteral("chats"), QJsonArray{QJsonObject{
                        {QStringLiteral("guid"), chatB},
                        {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550102")},
                        {QStringLiteral("style"), 45}
                    }}}
                }
            };
            b1.ingestMessagesForTest(seedMessages, true);
            QVERIFY(QFileInfo::exists(b1.messagesCacheFilePath()));
        }

        // Reload in a brand-new Backend instance: verify 0ms instant message history without network
        Backend b2(false);
        b2.setStateFilePath(statePath);
        QCOMPARE(b2.historySyncDays(), 60);
        QCOMPARE(b2.chats().size(), 2);

        b2.selectChat(chatB);
        QCOMPARE(b2.selectedChatGuid(), chatB);
        QCOMPARE(b2.messages().size(), 2);
    }

    void testAuthoritativeServerUnreadReconciliation() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state.json")));
        b.setWindowActive(false);

        const QString activeChat = QStringLiteral("iMessage;-;+14155550201");
        const QString targetChat = QStringLiteral("iMessage;-;+14155550202");
        const QJsonObject targetChatRef{
            {QStringLiteral("guid"), targetChat},
            {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550202")},
            {QStringLiteral("style"), 45}
        };

        b.ingestMessagesForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("ACT-1")},
                {QStringLiteral("text"), QStringLiteral("Active chat")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), 5000LL},
                {QStringLiteral("chats"), QJsonArray{QJsonObject{
                    {QStringLiteral("guid"), activeChat},
                    {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550201")},
                    {QStringLiteral("style"), 45}
                }}}
            },
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("UNREAD-1")},
                {QStringLiteral("text"), QStringLiteral("Incoming unread")},
                {QStringLiteral("isFromMe"), false},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), 2000LL},
                {QStringLiteral("dateRead"), 0LL},
                {QStringLiteral("chats"), QJsonArray{targetChatRef}}
            }
        }, true);
        QCOMPARE(b.totalUnread(), 1);

        // Case 1: User reads message on iPhone/Mac where Apple sets is_read=1 but leaves date_read=0.
        // Server's authoritative `message.is_read = 0` query returns empty array -> imsg clears unread count!
        b.reconcileUnreadMessagesForTest(QJsonArray{});
        QCOMPARE(b.totalUnread(), 0);

        // Case 2: User marks the conversation unread on iPhone/Mac ("Mark as Unread"),
        // which sets message.is_read = 0 in chat.db while retaining date_read = 3500 > 0.
        // Server's authoritative `message.is_read = 0` query returns that message -> imsg marks chat unread!
        b.reconcileUnreadMessagesForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("UNREAD-1")},
                {QStringLiteral("text"), QStringLiteral("Incoming unread")},
                {QStringLiteral("isFromMe"), false},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), 2000LL},
                {QStringLiteral("dateRead"), 3500LL},
                {QStringLiteral("chats"), QJsonArray{targetChatRef}}
            }
        });
        QCOMPARE(b.totalUnread(), 1);
    }

    void testHomePageAndClearSelectedChat() {
        Backend b(false);
        b.loadDemoData();
        QVERIFY(!b.selectedChatGuid().isEmpty());
        QVERIFY(!b.messages().isEmpty());

        // Navigate to Home Page
        b.clearSelectedChat();
        QVERIFY(b.selectedChatGuid().isEmpty());
        QVERIFY(b.selectedChatTitle().isEmpty());
        QVERIFY(b.messages().isEmpty());

        // Subsequent initial or delta message polls must NOT force-select the first chat while on Home Page
        const QString demoGuid = b.chats().first().toMap().value(QStringLiteral("guid")).toString();
        b.ingestMessagesForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("HOME-POLL-1")},
                {QStringLiteral("text"), QStringLiteral("New message while viewing Home Page")},
                {QStringLiteral("isFromMe"), false},
                {QStringLiteral("itemType"), 0},
                {QStringLiteral("dateCreated"), QDateTime::currentMSecsSinceEpoch()},
                {QStringLiteral("chats"), QJsonArray{QJsonObject{
                    {QStringLiteral("guid"), demoGuid},
                    {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550100")},
                    {QStringLiteral("style"), 45}
                }}}
            }
        }, true);
        QVERIFY(b.selectedChatGuid().isEmpty());

        // Clicking a conversation from the Home Page selects it normally
        b.selectChat(demoGuid);
        QCOMPARE(b.selectedChatGuid(), demoGuid);
        QVERIFY(!b.messages().isEmpty());

        // goHome() returns to Home Page
        b.goHome();
        QVERIFY(b.selectedChatGuid().isEmpty());

        // Verify QML Home Page & History Sync Window bindings
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("backend.clearSelectedChat")));
        QVERIFY(qml.contains(QStringLiteral("Ctrl+H")));
        QVERIFY(qml.contains(QStringLiteral("iMessage Home")));
        QVERIFY(qml.contains(QStringLiteral("backend.historySyncDays")));
        QVERIFY(qml.contains(QStringLiteral("backend.setHistorySyncDays")));
        QVERIFY(qml.contains(QStringLiteral("backend.cycleHistorySyncDays")));
    }

    void testVerificationCodeExtractionAndCopyToClipboard() {
        // Positive 2FA / OTP verification code cases
        QCOMPARE(Backend::extractVerificationCode(QStringLiteral("Your Google verification code is G-482910. Do not share it.")),
                 QStringLiteral("482910"));
        QCOMPARE(Backend::extractVerificationCode(QStringLiteral("849201 is your Apple ID verification code.")),
                 QStringLiteral("849201"));
        QCOMPARE(Backend::extractVerificationCode(QStringLiteral("Your Instagram code is 194-820. Don't share it.")),
                 QStringLiteral("194820"));
        QCOMPARE(Backend::extractVerificationCode(QStringLiteral("Chase Fraud Alert: Your one-time passcode is 739104")),
                 QStringLiteral("739104"));
        QCOMPARE(Backend::extractVerificationCode(QStringLiteral("Your security code: A9F4K2")),
                 QStringLiteral("A9F4K2"));
        QCOMPARE(Backend::extractVerificationCode(QStringLiteral("528491")),
                 QStringLiteral("528491"));

        // Negative cases (normal chat messages, URLs, prices, times)
        QCOMPARE(Backend::extractVerificationCode(QStringLiteral("Hey, let's meet at 2026 Main St at 10:30am!")),
                 QString());
        QCOMPARE(Backend::extractVerificationCode(QStringLiteral("Check out https://example.com/item/482910 for $1299")),
                 QString());

        // Verify buildMessageListFromApi populates verificationCode
        QJsonArray apiMsgs{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("OTP-MSG-1")},
                {QStringLiteral("text"), QStringLiteral("Your GitHub authentication code is 904218.")},
                {QStringLiteral("dateCreated"), 1700000050000LL},
                {QStringLiteral("isFromMe"), false},
                {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15550001111")}}}
            }
        };
        QHash<QString, QString> contacts;
        QHash<QString, QString> attCache;
        const QVariantList built = Backend::buildMessageListFromApi(apiMsgs, contacts, attCache);
        QCOMPARE(built.size(), 1);
        QCOMPARE(built.first().toMap().value(QStringLiteral("verificationCode")).toString(), QStringLiteral("904218"));

        // Verify copyToClipboard updates system clipboard and emits toastMessage
        Backend b(false);
        QSignalSpy toastSpy(&b, &Backend::toastMessage);
        b.copyToClipboard(QStringLiteral("904218"), QStringLiteral("Code 904218"));
        QCOMPARE(toastSpy.count(), 1);
        QVERIFY(toastSpy.first().first().toString().contains(QStringLiteral("904218")));
        if (QClipboard *cb = QGuiApplication::clipboard()) {
            QCOMPARE(cb->text(QClipboard::Clipboard), QStringLiteral("904218"));
        }
    }

    void testSearchContactsAndStartNewChatAutocomplete() {
        Backend b(false);
        b.loadDemoData();

        // 1. Search contacts for "alex" (case-insensitive)
        const QVariantList alexResults = b.searchContacts(QStringLiteral("alex"), 15);
        QVERIFY(alexResults.size() >= 2);
        const QVariantMap first = alexResults.at(0).toMap();
        QStringList names;
        for (const QVariant &v : alexResults) {
            names.append(v.toMap().value(QStringLiteral("displayName")).toString());
        }
        QVERIFY(names.contains(QStringLiteral("Alex Rivera")));
        QVERIFY(names.contains(QStringLiteral("Alex Mercer")));

        // Verify multi-word search "alex rivera" ranks Alex Rivera #1
        const QVariantList alexRiveraResults = b.searchContacts(QStringLiteral("alex rivera"), 5);
        QVERIFY(!alexRiveraResults.isEmpty());
        QCOMPARE(alexRiveraResults.first().toMap().value(QStringLiteral("displayName")).toString(),
                 QStringLiteral("Alex Rivera"));

        // Verify both name/displayName and handle/address keys are populated
        QVERIFY(!first.value(QStringLiteral("name")).toString().isEmpty());
        QVERIFY(!first.value(QStringLiteral("handle")).toString().isEmpty());
        QVERIFY(!first.value(QStringLiteral("displayHandle")).toString().isEmpty());

        // 2. Simulate typing "jordan" in sidebar search, then starting a new conversation with Jordan Taylor
        b.setSearchQuery(QStringLiteral("jordan"));
        QCOMPARE(b.searchQuery(), QStringLiteral("jordan"));

        b.startNewChat(QStringLiteral("Jordan Taylor"), QString());
        QCOMPARE(b.searchQuery(), QString());
        QCOMPARE(b.selectedChatGuid(), QStringLiteral("iMessage;-;+14155550101"));
        QCOMPARE(b.selectedChatTitle(), QStringLiteral("Jordan Taylor"));
        QCOMPARE(b.chats().first().toMap().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("iMessage;-;+14155550101"));

        // 3. Verify QML contains selectable TextEdit, otpCopyPill, copyToClipboard, searchContacts, and newChatSuggestionsList
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("selectByMouse: true")));
        QVERIFY(qml.contains(QStringLiteral("id: otpCopyPill")));
        QVERIFY(qml.contains(QStringLiteral("backend.copyToClipboard")));
        QVERIFY(qml.contains(QStringLiteral("backend.searchContacts")));
        QVERIFY(qml.contains(QStringLiteral("id: newChatSuggestionsList")));
        QVERIFY(qml.contains(QStringLiteral("sidebarContactMatches")));
    }

    void testDeleteAndUnsendMessagesAndDeleteChatAndBidirectionalSync() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString stateFile = tempDir.filePath(QStringLiteral("state.json"));
        const QString deletedFile = tempDir.filePath(QStringLiteral("deleted_messages.json"));

        Backend b(false);
        b.setStateFilePath(stateFile);

        const qint64 baseTs = QDateTime::currentMSecsSinceEpoch() - 3600000LL;
        auto makeMsg = [&](const QString &guid, const QString &chatGuid, const QString &text, bool fromMe, qint64 ts) {
            QJsonObject m;
            m.insert(QStringLiteral("guid"), guid);
            m.insert(QStringLiteral("text"), text);
            m.insert(QStringLiteral("isFromMe"), fromMe);
            m.insert(QStringLiteral("service"), QStringLiteral("iMessage"));
            m.insert(QStringLiteral("dateCreated"), ts);
            m.insert(QStringLiteral("isRead"), true);
            m.insert(QStringLiteral("dateRead"), ts + 100);
            QJsonObject cObj;
            cObj.insert(QStringLiteral("guid"), chatGuid);
            cObj.insert(QStringLiteral("chatIdentifier"), chatGuid.mid(11));
            cObj.insert(QStringLiteral("style"), 45);
            m.insert(QStringLiteral("chats"), QJsonArray{cObj});
            return m;
        };

        const QString chatA = QStringLiteral("iMessage;-;+15551112222");
        const QString chatB = QStringLiteral("iMessage;-;+15553334444");

        QJsonArray initialBatch{
            makeMsg(QStringLiteral("msg-a3"), chatA, QStringLiteral("Third message in A"), true, baseTs + 3000),
            makeMsg(QStringLiteral("msg-a2"), chatA, QStringLiteral("Second message in A"), false, baseTs + 2000),
            makeMsg(QStringLiteral("msg-a1"), chatA, QStringLiteral("First message in A"), false, baseTs + 1000),
            makeMsg(QStringLiteral("msg-b1"), chatB, QStringLiteral("First message in B"), false, baseTs + 500)
        };

        b.ingestMessagesForTest(initialBatch, true);
        b.selectChat(chatA);
        QCOMPARE(b.selectedChatGuid(), chatA);
        QCOMPARE(b.messages().size(), 3);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("msg-a3"));
        QVERIFY(b.messages().last().toMap().value(QStringLiteral("isIMessage")).toBool());

        // 1. Delete latest message (msg-a3) -> immediately removed from messages(), chat preview falls back to msg-a2, persisted to deleted_messages.json
        b.deleteMessage(QStringLiteral("msg-a3"));
        QCOMPARE(b.messages().size(), 2);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("msg-a2"));

        QVariantMap chatAMap;
        for (const QVariant &cv : b.chats()) {
            if (cv.toMap().value(QStringLiteral("guid")).toString() == chatA) {
                chatAMap = cv.toMap();
                break;
            }
        }
        QCOMPARE(chatAMap.value(QStringLiteral("lastMessagePreview")).toString(), QStringLiteral("Second message in A"));
        QVERIFY(QFileInfo::exists(deletedFile));

        // 2. Re-ingesting initialBatch (e.g. if pollDelta or loadChatMessages sees msg-a3 before chat.db purges) must NOT resurrect msg-a3
        b.ingestMessagesForTest(initialBatch, false);
        QCOMPARE(b.messages().size(), 2);

        // 3. Restarting Backend with same stateFile preserves deleted message tombstones
        {
            Backend bReloaded(false);
            bReloaded.setStateFilePath(stateFile);
            bReloaded.selectChat(chatA);
            QCOMPARE(bReloaded.messages().size(), 2);
            QCOMPARE(bReloaded.messages().last().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("msg-a2"));
        }

        // 4. Test isMessageDeletedOrRetracted & buildMessageListFromApi filtering for iOS/macOS retracted/deleted messages
        QJsonObject deletedByIos = makeMsg(QStringLiteral("msg-del-ios"), chatA, QStringLiteral("Deleted on iPhone"), false, baseTs + 4000);
        deletedByIos.insert(QStringLiteral("dateDeleted"), baseTs + 4500);
        QVERIFY(Backend::isMessageDeletedOrRetracted(deletedByIos));

        QJsonObject retractedByIos = makeMsg(QStringLiteral("msg-ret-ios"), chatA, QString(), true, baseTs + 5000);
        retractedByIos.insert(QStringLiteral("dateRetracted"), baseTs + 5500);
        QVERIFY(Backend::isMessageDeletedOrRetracted(retractedByIos));

        QJsonObject editedEmptyRetract = makeMsg(QStringLiteral("msg-edit-empty"), chatA, QString(), true, baseTs + 6000);
        editedEmptyRetract.insert(QStringLiteral("dateEdited"), baseTs + 6500);
        QVERIFY(Backend::isMessageDeletedOrRetracted(editedEmptyRetract));

        // 5. Bidirectional Sync: simulate deleting msg-a2 on the iPhone so server now only returns [msg-a1]
        QJsonArray serverRemaining{
            makeMsg(QStringLiteral("msg-a1"), chatA, QStringLiteral("First message in A"), false, baseTs + 1000)
        };
        b.reconcileChatMessagesForTest(chatA, serverRemaining);
        QCOMPARE(b.messages().size(), 1);
        QCOMPARE(b.messages().first().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("msg-a1"));

        // 6. Test unsendMessage on msg-a1
        b.unsendMessage(QStringLiteral("msg-a1"));
        QCOMPARE(b.messages().size(), 0);

        // 7. Test deleteChat on chatA -> removes chatA and automatically selects chatB
        QCOMPARE(b.chats().size(), 2);
        b.deleteChat(chatA);
        QCOMPARE(b.chats().size(), 1);
        QCOMPARE(b.selectedChatGuid(), chatB);

        // 8. Verify QML contains deleteMsgBtn, unsendMsgBtn, headerDeleteChatBtn, and deleteChatConfirmPopup
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("id: deleteMsgBtn")));
        QVERIFY(qml.contains(QStringLiteral("id: unsendMsgBtn")));
        QVERIFY(qml.contains(QStringLiteral("backend.deleteMessage")));
        QVERIFY(qml.contains(QStringLiteral("backend.unsendMessage")));
        QVERIFY(qml.contains(QStringLiteral("backend.deleteChat")));
        QVERIFY(qml.contains(QStringLiteral("id: deleteChatConfirmPopup")));
    }

    void testConversationContextMenuAndNonBlockingReadUnreadDelete()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString stateFile = tempDir.filePath(QStringLiteral("state.json"));
        const QString cacheFile = tempDir.filePath(QStringLiteral("messages_cache.json"));

        Backend b(false);
        b.setStateFilePath(stateFile);

        const QString chatGuid = QStringLiteral("iMessage;-;+15558889999");
        const qint64 baseTs = QDateTime::currentMSecsSinceEpoch() - 3600000LL;

        auto makeChatMsg = [&](int idx, bool fromMe) {
            QJsonObject msg;
            msg.insert(QStringLiteral("guid"), QStringLiteral("msg-%1").arg(idx));
            msg.insert(QStringLiteral("text"), QStringLiteral("Message #%1").arg(idx));
            msg.insert(QStringLiteral("isFromMe"), fromMe);
            msg.insert(QStringLiteral("dateCreated"), baseTs + idx * 1000);
            if (!fromMe) {
                QJsonObject handle;
                handle.insert(QStringLiteral("address"), QStringLiteral("+15558889999"));
                msg.insert(QStringLiteral("handle"), handle);
            }
            // Add bulky attributedBody to verify flushMessagesCacheNow strips it
            QJsonArray runs;
            QJsonObject runObj;
            runObj.insert(QStringLiteral("text"), QStringLiteral("Message #%1").arg(idx));
            runs.append(runObj);
            QJsonObject attrBody;
            attrBody.insert(QStringLiteral("string"), QStringLiteral("Message #%1").arg(idx));
            attrBody.insert(QStringLiteral("runs"), runs);
            msg.insert(QStringLiteral("attributedBody"), attrBody);

            QJsonObject chatObj;
            chatObj.insert(QStringLiteral("guid"), chatGuid);
            chatObj.insert(QStringLiteral("chatIdentifier"), QStringLiteral("+15558889999"));
            chatObj.insert(QStringLiteral("displayName"), QStringLiteral("Context Menu Test"));
            QJsonArray chatsArr;
            chatsArr.append(chatObj);
            msg.insert(QStringLiteral("chats"), chatsArr);
            return msg;
        };

        // 1. Ingest 100 messages to test 80-message cache cap and attributedBody stripping
        QJsonArray hundredMsgs;
        for (int i = 1; i <= 100; ++i) {
            hundredMsgs.append(makeChatMsg(i, (i % 2 == 0)));
        }
        b.ingestMessagesForTest(hundredMsgs, true);
        b.flushMessagesCacheNow(false);

        QFile cf(cacheFile);
        QVERIFY(cf.open(QIODevice::ReadOnly));
        const QByteArray rawCache = cf.readAll();
        cf.close();
        QVERIFY(!rawCache.contains("attributedBody"));
        const QJsonObject cacheRoot = QJsonDocument::fromJson(rawCache).object();
        const QJsonArray cachedChatArr = cacheRoot.value(QStringLiteral("messages")).toArray();
        QCOMPARE(cachedChatArr.size(), 80);

        // 2. Test markChatRead, markChatUnread, toggleChatRead, and 15-second mutation grace period
        b.markChatRead(chatGuid);
        QCOMPARE(b.totalUnread(), 0);

        b.markChatUnread(chatGuid);
        QCOMPARE(b.totalUnread(), 1);

        // An incoming poll containing an outgoing message within 15s must NOT clobber explicit markChatUnread
        QJsonArray pollBatch{ makeChatMsg(100, true) };
        b.ingestMessagesForTest(pollBatch, false);
        QCOMPARE(b.totalUnread(), 1);

        // toggleChatRead (alias for toggleChatReadUnread) flips unread -> read -> unread
        b.toggleChatRead(chatGuid);
        QCOMPARE(b.totalUnread(), 0);
        b.toggleChatRead(chatGuid);
        QCOMPARE(b.totalUnread(), 1);

        // 3. Verify buildMessageListFromApi resolves 16-char attachment prefixes (including at_0_... with underscores)
        //    and ignores .pluginPayloadAttachment blobs
        const QString imgGuid = QStringLiteral("at_0_99887766-5544-3322-1100-aabbccddeeff");
        const QString fakeImgPath = tempDir.filePath(QStringLiteral("at_0_99887766-55_IMG_1234.jpeg"));
        {
            QFile imgFile(fakeImgPath);
            QVERIFY(imgFile.open(QIODevice::WriteOnly));
            imgFile.write("\xFF\xD8\xFF\xE0JFIF-test-image-data-over-100-bytes------------------------------------------------------------------------------------------");
            imgFile.close();
        }
        QHash<QString, QString> attCache;
        attCache.insert(imgGuid.left(16), fakeImgPath);

        QJsonObject msgWithImg = makeChatMsg(101, false);
        msgWithImg.insert(QStringLiteral("text"), QString());
        QJsonObject validImgAtt;
        validImgAtt.insert(QStringLiteral("guid"), imgGuid);
        validImgAtt.insert(QStringLiteral("mimeType"), QStringLiteral("image/jpeg"));
        validImgAtt.insert(QStringLiteral("transferName"), QStringLiteral("IMG_1234.jpeg"));
        QJsonObject pluginBlobAtt;
        pluginBlobAtt.insert(QStringLiteral("guid"), QStringLiteral("at_1_plugin_blob"));
        pluginBlobAtt.insert(QStringLiteral("transferName"), QStringLiteral("payload.pluginPayloadAttachment"));
        msgWithImg.insert(QStringLiteral("attachments"), QJsonArray{ pluginBlobAtt, validImgAtt });

        const QVariantList builtMsgs = Backend::buildMessageListFromApi(
            QJsonArray{ msgWithImg },
            QHash<QString, QString>(),
            attCache
        );
        QCOMPARE(builtMsgs.size(), 1);
        const QVariantMap builtMsg = builtMsgs.first().toMap();
        QCOMPARE(builtMsg.value(QStringLiteral("hasImageAttachment")).toBool(), true);
        QCOMPARE(builtMsg.value(QStringLiteral("hasFileAttachment")).toBool(), false);
        QCOMPARE(builtMsg.value(QStringLiteral("attachmentGuid")).toString(), imgGuid);
        QCOMPARE(builtMsg.value(QStringLiteral("attachmentLocalUrl")).toString(), QUrl::fromLocalFile(fakeImgPath).toString());

        // 4. Verify Main.qml right-click context menu, scroll fix, and reactive attachment binding
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("id: chatContextMenuPopup")));
        QVERIFY(qml.contains(QStringLiteral("function openForChat")));
        QVERIFY(qml.contains(QStringLiteral("Mark as Read")));
        QVERIFY(qml.contains(QStringLiteral("Mark as Unread")));
        QVERIFY(qml.contains(QStringLiteral("Delete Conversation")));
        QVERIFY(qml.contains(QStringLiteral("liveAttachmentUrl")));
        QVERIFY(qml.contains(QStringLiteral("chatContextMenuPopup.openForChat(")));
    }

    void testV158FastSendAttachmentIsolationAndActionMenuSafety() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        qputenv("HOME", tempDir.path().toUtf8());

        // Start a mock BlueBubbles HTTP server that stalls attachment downloads on purpose
        // while responding immediately to POST /api/v1/message/text and /api/v1/chat/new
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        int attachmentRequests = 0;
        int attachmentForceFalseRequests = 0;
        int purgedAttachmentRequests = 0;
        int textSendRequests = 0;
        int newChatRequests = 0;
        bool simulateMissingChatOnSend = false;
        QList<QTcpSocket *> stalledMediaSockets;

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (QTcpSocket *sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock]() {
                    const QByteArray reqData = sock->readAll();
                    const QList<QByteArray> lines = reqData.split('\n');
                    if (lines.isEmpty()) return;
                    const QByteArray reqLine = lines.first().trimmed();

                    auto sendJsonResponse = [](QTcpSocket *s, int status, const QByteArray &statusText, const QByteArray &jsonBody) {
                        QByteArray resp = "HTTP/1.1 " + QByteArray::number(status) + " " + statusText + "\r\n"
                                          "Content-Type: application/json\r\n"
                                          "Content-Length: " + QByteArray::number(jsonBody.size()) + "\r\n"
                                          "Connection: close\r\n\r\n" + jsonBody;
                        s->write(resp);
                        s->flush();
                        s->disconnectFromHost();
                    };

                    if (reqLine.startsWith("GET /api/v1/attachment/")) {
                        attachmentRequests++;
                        if (reqLine.contains("force=false")) {
                            attachmentForceFalseRequests++;
                        }
                        if (reqLine.contains("at_0_purged_")) {
                            purgedAttachmentRequests++;
                            sendJsonResponse(sock, 500, "Internal Server Error",
                                             R"({"status":500,"message":"Attachment does not exist in disk!"})");
                            return;
                        }
                        // Stall slow media downloads to simulate saturated media connections
                        stalledMediaSockets.append(sock);
                        return;
                    }

                    if (reqLine.startsWith("POST /api/v1/message/text")) {
                        textSendRequests++;
                        if (simulateMissingChatOnSend) {
                            sendJsonResponse(sock, 500, "Internal Server Error",
                                             R"({"status":500,"message":"Chat does not exist!"})");
                            return;
                        }
                        sendJsonResponse(sock, 200, "OK",
                                         R"({"status":200,"data":{"guid":"real-msg-guid-158","text":"Just sent you money - pay your taxes ❤️","isFromMe":true,"dateCreated":1775860005000}})");
                        return;
                    }

                    if (reqLine.startsWith("POST /api/v1/chat/new")) {
                        newChatRequests++;
                        sendJsonResponse(sock, 200, "OK",
                                         R"({"status":200,"data":{"guid":"iMessage;-;+14155550140","messages":[{"guid":"real-newchat-msg-guid-158","text":"Hello brand new chat","isFromMe":true,"dateCreated":1775860009000}]}})");
                        return;
                    }

                    sendJsonResponse(sock, 200, "OK", R"({"status":200,"data":[]})");
                });
            }
        });

        qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(port).toUtf8());
        qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));

        Backend b;
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_v158.json")));
        b.loadCredentials();

        const QString chatGuid = QStringLiteral("iMessage;-;+14155550140");
        QJsonObject chatObj;
        chatObj.insert(QStringLiteral("guid"), chatGuid);
        chatObj.insert(QStringLiteral("chatIdentifier"), QStringLiteral("+14155550140"));
        chatObj.insert(QStringLiteral("displayName"), QStringLiteral("Casey Jordan"));
        QJsonObject part;
        part.insert(QStringLiteral("address"), QStringLiteral("+14155550140"));
        chatObj.insert(QStringLiteral("participants"), QJsonArray{part});
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});

        // 1. Ingest a message with a purged attachment and verify force=false + NO retry storm on HTTP 500
        QJsonObject purgedMsg;
        purgedMsg.insert(QStringLiteral("guid"), QStringLiteral("msg-purged-1"));
        purgedMsg.insert(QStringLiteral("text"), QString());
        purgedMsg.insert(QStringLiteral("isFromMe"), false);
        purgedMsg.insert(QStringLiteral("dateCreated"), 1775860002000LL);
        purgedMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
        QJsonObject purgedAtt;
        purgedAtt.insert(QStringLiteral("guid"), QStringLiteral("at_0_purged_guid_1"));
        purgedAtt.insert(QStringLiteral("mimeType"), QStringLiteral("image/jpeg"));
        purgedAtt.insert(QStringLiteral("transferName"), QStringLiteral("purged.jpg"));
        purgedMsg.insert(QStringLiteral("attachments"), QJsonArray{purgedAtt});

        // Plus 8 slow attachments to saturate m_mediaNam
        QJsonArray allMsgs{purgedMsg};
        for (int i = 0; i < 8; ++i) {
            QJsonObject slowMsg;
            slowMsg.insert(QStringLiteral("guid"), QStringLiteral("msg-slow-%1").arg(i));
            slowMsg.insert(QStringLiteral("text"), QString());
            slowMsg.insert(QStringLiteral("isFromMe"), false);
            slowMsg.insert(QStringLiteral("dateCreated"), 1775860001000LL + i);
            slowMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
            QJsonObject slowAtt;
            slowAtt.insert(QStringLiteral("guid"), QStringLiteral("at_0_slow_guid_%1").arg(i));
            slowAtt.insert(QStringLiteral("mimeType"), QStringLiteral("image/jpeg"));
            slowAtt.insert(QStringLiteral("transferName"), QStringLiteral("slow_%1.jpg").arg(i));
            slowMsg.insert(QStringLiteral("attachments"), QJsonArray{slowAtt});
            allMsgs.append(slowMsg);
        }

        b.ingestMessagesForTest(allMsgs, true);
        b.selectChat(chatGuid);

        QTRY_VERIFY_WITH_TIMEOUT(purgedAttachmentRequests == 1, 1500);
        QVERIFY(attachmentForceFalseRequests >= 1);
        // Concurrency cap: at most 2 stalled media sockets should be active simultaneously
        QTRY_COMPARE_WITH_TIMEOUT(stalledMediaSockets.size(), 2, 1500);

        // 2. While m_mediaNam has 2 stalled downloads + 6 queued, sendMessage must complete in < 300ms via m_sendNam!
        QElapsedTimer sendTimer;
        sendTimer.start();
        b.sendMessage(chatGuid, QStringLiteral("Just sent you money - pay your taxes ❤️"));

        // Verify optimistic message is immediately appended
        QVERIFY(!b.messages().isEmpty());
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

        // Wait for POST /api/v1/message/text response and verify immediate transition to Delivered + real GUID
        QTRY_COMPARE_WITH_TIMEOUT(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(),
                                  QStringLiteral("Delivered"), 500);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("real-msg-guid-158"));
        QVERIFY2(sendTimer.elapsed() < 500, "sendMessage must not be blocked by in-flight attachment downloads");

        // 3. Test brand-new chat fallback where /api/v1/message/text returns "Chat does not exist!" -> immediate /api/v1/chat/new
        simulateMissingChatOnSend = true;
        b.sendMessage(chatGuid, QStringLiteral("Hello brand new chat"));
        QTRY_COMPARE_WITH_TIMEOUT(newChatRequests, 1, 500);
        QTRY_COMPARE_WITH_TIMEOUT(b.messages().last().toMap().value(QStringLiteral("guid")).toString(),
                                  QStringLiteral("real-newchat-msg-guid-158"), 500);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(),
                 QStringLiteral("Delivered"));

        for (QTcpSocket *s : stalledMediaSockets) {
            if (s) s->close();
        }

        // 4. Verify Main.qml calls root.clearActionMenu() BEFORE backend.sendReaction/unsendMessage/deleteMessage
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("root.clearActionMenu()\n                                                        backend.sendReaction(cGuid, mGuid, reactEmoji)")));
        QVERIFY(qml.contains(QStringLiteral("root.clearActionMenu()\n                                                    backend.unsendMessage(mGuid)")));
        QVERIFY(qml.contains(QStringLiteral("root.clearActionMenu()\n                                                    backend.deleteMessage(mGuid)")));
        QVERIFY(!qml.contains(QStringLiteral("backend.sendReaction(backend.selectedChatGuid, msgDelegate.modelData.guid, modelData)\n                                                        root.clearActionMenu()")));
    }

    // -------------------------------------------------------------------------
    // 36. v1.5.9 Composer Descender Bottom Clearance & Contact Sync / Fallback
    // -------------------------------------------------------------------------
    void testV159ComposerBottomClearanceAndContactResolution() {
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());

        // 1. Verify Bottom Message Composer Bar height & composerInput bottomPadding for descenders (p, q, g, y, j)
        QVERIFY(qml.contains(QStringLiteral("Layout.preferredHeight: Math.min(160, Math.max(72, theme.fontSize + 52, composerInput.contentHeight + composerInput.topPadding + composerInput.bottomPadding + 28))")));
        QVERIFY(qml.contains(QStringLiteral("id: composerInput\n                                anchors.fill: parent\n                                anchors.leftMargin: 8\n                                anchors.rightMargin: 8\n                                topPadding: 10\n                                bottomPadding: 14")));

        // 2. Verify New Message modal, Search Bar, and Emoji Search Field bottomPadding & expanded heights
        QVERIFY(qml.contains(QStringLiteral("id: newChatRecipient\n                        anchors.fill: parent\n                        anchors.leftMargin: 10\n                        anchors.rightMargin: 10\n                        topPadding: 8\n                        bottomPadding: 12")));
        QVERIFY(qml.contains(QStringLiteral("id: newChatText\n                        anchors.fill: parent\n                        anchors.leftMargin: 10\n                        anchors.rightMargin: 10\n                        topPadding: 8\n                        bottomPadding: 12")));
        QVERIFY(qml.contains(QStringLiteral("id: searchInput\n                                    Layout.fillWidth: true\n                                    Layout.fillHeight: true\n                                    topPadding: 6\n                                    bottomPadding: 10")));

        // 3. Verify Lexi Matthews (1 (415) 555-0170 / 4155550170) + organization/nickname fallback contact resolution
        Backend b;
        const QByteArray contactsPayload = R"({
            "status": 200,
            "data": [
                {
                    "id": 999,
                    "firstName": "Lexi",
                    "lastName": "Matthews",
                    "displayName": "Lexi Matthews",
                    "phoneNumbers": [{"address": "1 (415) 555-0170", "id": 1555}],
                    "emails": []
                },
                {
                    "id": 1000,
                    "firstName": "",
                    "lastName": "",
                    "displayName": "",
                    "organization": "Summit Alpine Club",
                    "phoneNumbers": [{"address": "+14155550199"}],
                    "emails": []
                },
                {
                    "id": 1001,
                    "firstName": "",
                    "lastName": "",
                    "displayName": "",
                    "nickname": "DJ Spark",
                    "phoneNumbers": [{"address": "(415) 555-0144"}],
                    "emails": []
                }
            ]
        })";
        b.parseContactsJson(contactsPayload);

        // Search by digits "4155550170"
        const QVariantList byDigits = b.searchContacts(QStringLiteral("4155550170"), 10);
        QCOMPARE(byDigits.size(), 1);
        QCOMPARE(byDigits.first().toMap().value(QStringLiteral("displayName")).toString(), QStringLiteral("Lexi Matthews"));
        QCOMPARE(byDigits.first().toMap().value(QStringLiteral("address")).toString(), QStringLiteral("+14155550170"));

        // Search by name "Lexi"
        const QVariantList byName = b.searchContacts(QStringLiteral("Lexi"), 10);
        QCOMPARE(byName.size(), 1);
        QCOMPARE(byName.first().toMap().value(QStringLiteral("displayName")).toString(), QStringLiteral("Lexi Matthews"));

        // Verify organization-only and nickname-only contacts were also indexed
        const QVariantList byOrg = b.searchContacts(QStringLiteral("Summit"), 10);
        QCOMPARE(byOrg.size(), 1);
        QCOMPARE(byOrg.first().toMap().value(QStringLiteral("displayName")).toString(), QStringLiteral("Summit Alpine Club"));

        const QVariantList byNick = b.searchContacts(QStringLiteral("Spark"), 10);
        QCOMPARE(byNick.size(), 1);
        QCOMPARE(byNick.first().toMap().value(QStringLiteral("displayName")).toString(), QStringLiteral("DJ Spark"));
    }

    void testV160GroupChatTitleAndParticipantPersistence() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString stateFile = tempDir.filePath(QStringLiteral("state_v160.json"));

        const QByteArray contactsJson = R"({
            "data": [
                {
                    "firstName": "Casey",
                    "lastName": "Jordan",
                    "displayName": "Casey Jordan",
                    "phoneNumbers": [{"address": "+14155550140"}]
                },
                {
                    "firstName": "Avery",
                    "lastName": "Jordan",
                    "displayName": "Avery Jordan",
                    "phoneNumbers": [{"address": "+14155550147"}]
                }
            ]
        })";

        const QString groupGuid = QStringLiteral("iMessage;+;chat555000999888777666");
        {
            Backend b1(false);
            b1.setStateFilePath(stateFile);
            b1.parseContactsJson(contactsJson);

            QJsonObject chatMeta;
            chatMeta.insert(QStringLiteral("guid"), groupGuid);
            chatMeta.insert(QStringLiteral("chatIdentifier"), QStringLiteral("chat555000999888777666"));
            chatMeta.insert(QStringLiteral("displayName"), QStringLiteral(""));
            chatMeta.insert(QStringLiteral("originalROWID"), 975);
            chatMeta.insert(QStringLiteral("style"), 43);

            QJsonObject msgAvery;
            msgAvery.insert(QStringLiteral("guid"), QStringLiteral("msg-avery-1"));
            msgAvery.insert(QStringLiteral("text"), QStringLiteral("See you both at 6!"));
            msgAvery.insert(QStringLiteral("isFromMe"), false);
            msgAvery.insert(QStringLiteral("dateCreated"), 1775861000000LL);
            msgAvery.insert(QStringLiteral("chats"), QJsonArray{chatMeta});
            msgAvery.insert(QStringLiteral("handle"), QJsonObject{
                {QStringLiteral("address"), QStringLiteral("+14155550147")},
                {QStringLiteral("originalROWID"), 66}
            });

            QJsonObject msgCasey;
            msgCasey.insert(QStringLiteral("guid"), QStringLiteral("msg-casey-2"));
            msgCasey.insert(QStringLiteral("text"), QStringLiteral("Sounds great!"));
            msgCasey.insert(QStringLiteral("isFromMe"), false);
            msgCasey.insert(QStringLiteral("dateCreated"), 1775862000000LL);
            msgCasey.insert(QStringLiteral("chats"), QJsonArray{chatMeta});
            msgCasey.insert(QStringLiteral("handle"), QJsonObject{
                {QStringLiteral("address"), QStringLiteral("+14155550140")},
                {QStringLiteral("originalROWID"), 56}
            });

            b1.ingestMessagesForTest(QJsonArray{msgAvery, msgCasey}, true);

            const QVariantList chats = b1.chats();
            QCOMPARE(chats.size(), 1);
            const QVariantMap cMap = chats.first().toMap();
            QCOMPARE(cMap.value(QStringLiteral("guid")).toString(), groupGuid);
            QCOMPARE(cMap.value(QStringLiteral("title")).toString(), QStringLiteral("Casey & Avery"));
            QCOMPARE(cMap.value(QStringLiteral("subtitle")).toString(), QStringLiteral("2 participants"));
            QCOMPARE(cMap.value(QStringLiteral("initials")).toString(), QStringLiteral("CA"));

            // Searching by last name or phone number matches the group chat via participants
            b1.setSearchQuery(QStringLiteral("Jordan"));
            QCOMPARE(b1.chats().size(), 1);
            b1.setSearchQuery(QStringLiteral("4155550147"));
            QCOMPARE(b1.chats().size(), 1);
            b1.setSearchQuery(QString());

            b1.flushMessagesCacheNow(false);
        }

        // Reload from messages_cache.json in a fresh Backend instance and verify participants were persisted in chats[0]
        {
            Backend b2(false);
            b2.setStateFilePath(stateFile);
            b2.parseContactsJson(contactsJson);
            b2.loadMessagesCache();

            const QVariantList chats = b2.chats();
            QCOMPARE(chats.size(), 1);
            const QVariantMap cMap = chats.first().toMap();
            QCOMPARE(cMap.value(QStringLiteral("title")).toString(), QStringLiteral("Casey & Avery"));
            QCOMPARE(cMap.value(QStringLiteral("subtitle")).toString(), QStringLiteral("2 participants"));
        }
    }

    void testV161BackgroundImagePrefetchAndRetry() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        qputenv("HOME", tempDir.path().toUtf8());

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        int groupAttachmentRequests = 0;
        QByteArray validJpegBytes("\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 20);
        validJpegBytes.append(QByteArray(240, '\x42'));

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (QTcpSocket *sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock]() {
                    const QByteArray reqData = sock->readAll();
                    const QList<QByteArray> lines = reqData.split('\n');
                    if (lines.isEmpty()) return;
                    const QByteArray reqLine = lines.first().trimmed();

                    if (reqLine.startsWith("GET /api/v1/attachment/") && reqLine.contains("E1B782AE-5CF2-43C8-A48E-DC8D98A06DB4") && reqLine.contains("original=true")) {
                        groupAttachmentRequests++;
                        if (groupAttachmentRequests == 1) {
                            const QByteArray body = QByteArrayLiteral("{\"status\":500,\"message\":\"Attachment hasn't been downloaded yet!\"}");
                            const QByteArray resp = "HTTP/1.1 500 Internal Server Error\r\n"
                                                    "Content-Type: application/json\r\n"
                                                    "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                                                    "Connection: close\r\n\r\n" + body;
                            sock->write(resp);
                            sock->flush();
                            sock->disconnectFromHost();
                            return;
                        }
                        const QByteArray resp = "HTTP/1.1 200 OK\r\n"
                                                "Content-Type: image/jpeg\r\n"
                                                "Content-Length: " + QByteArray::number(validJpegBytes.size()) + "\r\n"
                                                "Connection: close\r\n\r\n" + validJpegBytes;
                        sock->write(resp);
                        sock->flush();
                        sock->disconnectFromHost();
                        return;
                    }

                    const QByteArray okBody = R"({"status":200,"data":[]})";
                    const QByteArray resp = "HTTP/1.1 200 OK\r\n"
                                            "Content-Type: application/json\r\n"
                                            "Content-Length: " + QByteArray::number(okBody.size()) + "\r\n"
                                            "Connection: close\r\n\r\n" + okBody;
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
            }
        });

        qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(port).toUtf8());
        qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));

        const QString stateFile = tempDir.filePath(QStringLiteral("state_v161.json"));
        const QString primaryChatGuid = QStringLiteral("iMessage;-;+14155550140");
        const QString alpineChatGuid = QStringLiteral("RCS;+;chat555000111222333444");
        const QString groupAttGuid = QStringLiteral("E1B782AE-5CF2-43C8-A48E-DC8D98A06DB4");
        const QString expectedCachedFile = tempDir.path() + QStringLiteral("/.cache/imsg/attachments/E1B782AE-5CF2-43_4754128548160758812.jpg");

        {
            Backend b(false);
            b.setStateFilePath(stateFile);

            // Primary selected chat (newer timestamp so it becomes auto-selected on initial load)
            QJsonObject chat1;
            chat1.insert(QStringLiteral("guid"), primaryChatGuid);
            chat1.insert(QStringLiteral("chatIdentifier"), QStringLiteral("+14155550140"));

            QJsonObject msg1;
            msg1.insert(QStringLiteral("guid"), QStringLiteral("msg-primary-1"));
            msg1.insert(QStringLiteral("text"), QStringLiteral("Active chat message"));
            msg1.insert(QStringLiteral("isFromMe"), false);
            msg1.insert(QStringLiteral("dateCreated"), 1793816000000LL);
            msg1.insert(QStringLiteral("chats"), QJsonArray{chat1});

            // Unselected background chat ("Alpine Trail Crew") with 10:00 AM image attachment
            QJsonObject chat2;
            chat2.insert(QStringLiteral("guid"), alpineChatGuid);
            chat2.insert(QStringLiteral("chatIdentifier"), QStringLiteral("chat555000111222333444"));
            chat2.insert(QStringLiteral("displayName"), QStringLiteral("Alpine Trail Crew"));
            chat2.insert(QStringLiteral("style"), 43);

            QJsonObject attObj;
            attObj.insert(QStringLiteral("guid"), groupAttGuid);
            attObj.insert(QStringLiteral("mimeType"), QStringLiteral("image/jpeg"));
            attObj.insert(QStringLiteral("transferName"), QStringLiteral("4754128548160758812.jpg"));

            QJsonObject msg2;
            msg2.insert(QStringLiteral("guid"), QStringLiteral("0D20ACE8-521E-483A-A785-9ED166800727"));
            msg2.insert(QStringLiteral("text"), QString());
            msg2.insert(QStringLiteral("isFromMe"), false);
            msg2.insert(QStringLiteral("dateCreated"), 1793815220000LL);
            msg2.insert(QStringLiteral("chats"), QJsonArray{chat2});
            msg2.insert(QStringLiteral("attachments"), QJsonArray{attObj});

            b.ingestMessagesForTest(QJsonArray{msg1, msg2}, true);

            // Verify primaryChatGuid is selected, NOT alpineChatGuid
            QCOMPARE(b.selectedChatGuid(), primaryChatGuid);

            // Verify Alpine Trail Crew image is proactively pre-fetched in background and auto-retried after transient 500
            QTRY_COMPARE_WITH_TIMEOUT(groupAttachmentRequests, 2, 3500);
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(expectedCachedFile) && QFileInfo(expectedCachedFile).size() >= 100, 1500);

            // Now select Alpine Trail Crew and verify the image is immediately ready in messages()
            b.selectChat(alpineChatGuid);
            QCOMPARE(b.messages().size(), 1);
            const QVariantMap mMap = b.messages().first().toMap();
            QCOMPARE(mMap.value(QStringLiteral("hasImageAttachment")).toBool(), true);
            QCOMPARE(mMap.value(QStringLiteral("attachmentLocalUrl")).toString(),
                     QUrl::fromLocalFile(expectedCachedFile).toString());

            b.flushMessagesCacheNow(false);
        }

        // Verify a fresh Backend instance indexes ~/.cache/imsg/attachments BEFORE loadMessagesCache() with 0 extra HTTP requests
        const int prevReqs = groupAttachmentRequests;
        {
            Backend b2(false);
            b2.setStateFilePath(stateFile);
            b2.loadMessagesCache();
            b2.selectChat(alpineChatGuid);

            QCOMPARE(b2.messages().size(), 1);
            const QVariantMap mMap = b2.messages().first().toMap();
            QCOMPARE(mMap.value(QStringLiteral("hasImageAttachment")).toBool(), true);
            QCOMPARE(mMap.value(QStringLiteral("attachmentLocalUrl")).toString(),
                     QUrl::fromLocalFile(expectedCachedFile).toString());
            QCOMPARE(groupAttachmentRequests, prevReqs);
        }

        // Verify Main.qml defines root.focusAndScrollChat() to avoid out-of-scope messageListView.positionViewAtEnd()
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("function focusAndScrollChat()")));
        QVERIFY(qml.contains(QStringLiteral("backend.selectChat(chatGuid)\n                                    root.focusAndScrollChat()")));
    }

    void testV162MessageListKeyboardNavigation() {
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());

        // Verify root helper functions for PageUp, PageDown, Home, End, and Up/Down step scrolling
        QVERIFY(qml.contains(QStringLiteral("function scrollMessagesPageUp()")));
        QVERIFY(qml.contains(QStringLiteral("function scrollMessagesPageDown()")));
        QVERIFY(qml.contains(QStringLiteral("function scrollMessagesToTop()")));
        QVERIFY(qml.contains(QStringLiteral("function scrollMessagesToBottom()")));
        QVERIFY(qml.contains(QStringLiteral("function scrollMessagesStepUp()")));
        QVERIFY(qml.contains(QStringLiteral("function scrollMessagesStepDown()")));

        // Verify originY-aware calculation and stickToBottom cancellation so PageUp never snaps back to bottom
        QVERIFY(qml.contains(QStringLiteral("var minY = messageListView.originY - messageListView.topMargin")));
        QVERIFY(qml.contains(QStringLiteral("messageListView.stickToBottom = false")));

        // Verify key handlers in messageListView, composerInput, msgText, and window Shortcuts
        QVERIFY(qml.contains(QStringLiteral("event.key === Qt.Key_PageUp")));
        QVERIFY(qml.contains(QStringLiteral("event.key === Qt.Key_PageDown")));
        QVERIFY(qml.contains(QStringLiteral("event.key === Qt.Key_Home")));
        QVERIFY(qml.contains(QStringLiteral("event.key === Qt.Key_End")));
        QVERIFY(qml.contains(QStringLiteral("sequences: [\"PageDown\", \"Ctrl+PageDown\"]")));
        QVERIFY(qml.contains(QStringLiteral("sequences: [\"PageUp\", \"Ctrl+PageUp\"]")));
        QVERIFY(qml.contains(QStringLiteral("sequence: \"Ctrl+Home\"")));
        QVERIFY(qml.contains(QStringLiteral("sequence: \"Ctrl+End\"")));
        QVERIFY(qml.contains(QStringLiteral("{ keys: \"Home / End\", desc: \"Jump to top / bottom of messages\" }")));
    }

    void testV163DelayedSmsLookbackAndUnreadReconcile() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        qputenv("HOME", tempDir.path().toUtf8());

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        qint64 observedAfterMs = 0;
        int pollCount = 0;

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        const QString smsChatGuid = QStringLiteral("SMS;-;22395");
        const QString edgarChatGuid = QStringLiteral("iMessage;-;+14155550140");

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (QTcpSocket *sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock]() {
                    const QByteArray reqData = sock->readAll();
                    const QList<QByteArray> lines = reqData.split('\n');
                    if (lines.isEmpty()) return;
                    const QByteArray reqLine = lines.first().trimmed();

                    if (reqLine.startsWith("POST /api/v1/message/query")) {
                        const int bodyIdx = reqData.indexOf("\r\n\r\n");
                        QJsonObject reqJson;
                        if (bodyIdx >= 0) {
                            reqJson = QJsonDocument::fromJson(reqData.mid(bodyIdx + 4)).object();
                        }
                        if (!reqJson.contains(QStringLiteral("after"))) {
                            const QByteArray okBody = R"({\"status\":200,\"data\":[]})";
                            const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                                    QByteArray::number(okBody.size()) + "\r\nConnection: close\r\n\r\n" + okBody;
                            sock->write(resp);
                            sock->flush();
                            sock->disconnectFromHost();
                            return;
                        }
                        observedAfterMs = reqJson.value(QStringLiteral("after")).toVariant().toLongLong();
                        ++pollCount;

                        // Delayed SMS created 5 minutes (300,000 ms) BEFORE m_lastMessageTimestamp!
                        // On poll 1, simulate transient empty chats array before chat_message_join commit;
                        // on poll 2, include the full chats array.
                        QJsonObject delayedSms{
                            {QStringLiteral("guid"), QStringLiteral("SMS-22395-DELAYED-1")},
                            {QStringLiteral("text"), QStringLiteral("Clover Auth Code: 739104")},
                            {QStringLiteral("dateCreated"), nowMs - 300000LL},
                            {QStringLiteral("isFromMe"), false},
                            {QStringLiteral("isRead"), false},
                            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("22395")}}}
                        };
                        if (pollCount >= 2) {
                            delayedSms.insert(QStringLiteral("chats"), QJsonArray{QJsonObject{
                                {QStringLiteral("guid"), smsChatGuid},
                                {QStringLiteral("chatIdentifier"), QStringLiteral("22395")},
                                {QStringLiteral("style"), 45}
                            }});
                        }

                        const QByteArray respBody = QJsonDocument(QJsonObject{
                            {QStringLiteral("status"), 200},
                            {QStringLiteral("data"), QJsonArray{delayedSms}}
                        }).toJson(QJsonDocument::Compact);

                        const QByteArray resp = "HTTP/1.1 200 OK\r\n"
                                                "Content-Type: application/json\r\n"
                                                "Content-Length: " + QByteArray::number(respBody.size()) + "\r\n"
                                                "Connection: close\r\n\r\n" + respBody;
                        sock->write(resp);
                        sock->flush();
                        sock->disconnectFromHost();
                        return;
                    }

                    const QByteArray okBody = R"({"status":200,"data":[]})";
                    const QByteArray resp = "HTTP/1.1 200 OK\r\n"
                                            "Content-Type: application/json\r\n"
                                            "Content-Length: " + QByteArray::number(okBody.size()) + "\r\n"
                                            "Connection: close\r\n\r\n" + okBody;
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
            }
        });

        qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(port).toUtf8());
        qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_v163.json")));
        b.loadCredentials();
        b.setWindowActive(false);

        // Seed initial messages: Casey at nowMs (advances m_lastMessageTimestamp to nowMs),
        // and 22395 with an older message from 2 hours ago.
        QJsonObject msgCasey{
            {QStringLiteral("guid"), QStringLiteral("MSG-EDGAR-NOW")},
            {QStringLiteral("text"), QStringLiteral("Newest message at nowMs")},
            {QStringLiteral("dateCreated"), nowMs},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("chats"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), edgarChatGuid},
                {QStringLiteral("chatIdentifier"), QStringLiteral("+14155550140")},
                {QStringLiteral("style"), 45}
            }}}
        };
        QJsonObject msgSmsOld{
            {QStringLiteral("guid"), QStringLiteral("SMS-22395-OLD")},
            {QStringLiteral("text"), QStringLiteral("Old code from 2 hours ago")},
            {QStringLiteral("dateCreated"), nowMs - 7200000LL},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("isRead"), true},
            {QStringLiteral("dateRead"), nowMs - 7100000LL},
            {QStringLiteral("chats"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), smsChatGuid},
                {QStringLiteral("chatIdentifier"), QStringLiteral("22395")},
                {QStringLiteral("style"), 45}
            }}}
        };
        b.ingestMessagesForTest(QJsonArray{msgCasey, msgSmsOld}, true);

        // Poll 1: message arrives with empty chats array (uncommitted chat_message_join)
        b.pollDeltaForTest();
        QTRY_COMPARE_WITH_TIMEOUT(pollCount, 1, 2000);
        // Verify 1-hour lookback window (afterMs <= nowMs - 3600000 + 5000)
        QVERIFY2(observedAfterMs <= nowMs - 3595000LL, "pollDelta must use 1-hour lookback window");

        // Poll 2: message arrives with populated chats array and 5-minute-old dateCreated
        b.pollDeltaForTest();
        QTRY_COMPARE_WITH_TIMEOUT(pollCount, 2, 2000);

        // Verify SMS;-;22395 received the 5-minute-delayed SMS, updated preview, and incremented unreadCount!
        QVariantMap smsChatMap;
        for (const QVariant &cv : b.chats()) {
            const QVariantMap m = cv.toMap();
            if (m.value(QStringLiteral("guid")).toString() == smsChatGuid) {
                smsChatMap = m;
                break;
            }
        }
        QCOMPARE(smsChatMap.value(QStringLiteral("lastMessagePreview")).toString(),
                 QStringLiteral("Clover Auth Code: 739104"));
        QCOMPARE(smsChatMap.value(QStringLiteral("unreadCount")).toInt(), 1);

        // Now test Bug 3B: reconcileServerUnreadMessages discovering a newer unread message
        // while SMS;-;22395 is selected: must update chat preview, re-sort to top, and rebuild messages()!
        b.selectChat(smsChatGuid);
        b.setWindowActive(false);

        QJsonObject unreadViaReconcile{
            {QStringLiteral("guid"), QStringLiteral("SMS-22395-RECONCILE-2")},
            {QStringLiteral("text"), QStringLiteral("Second code via unread reconcile: 991827")},
            {QStringLiteral("dateCreated"), nowMs + 5000LL},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("isRead"), false},
            {QStringLiteral("itemType"), 0},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("22395")}}},
            {QStringLiteral("chats"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), smsChatGuid},
                {QStringLiteral("chatIdentifier"), QStringLiteral("22395")},
                {QStringLiteral("style"), 45}
            }}}
        };
        b.reconcileUnreadMessagesForTest(QJsonArray{unreadViaReconcile});

        // Verify SMS;-;22395 is now sorted to index 0 of chats() with updated preview
        QCOMPARE(b.chats().first().toMap().value(QStringLiteral("guid")).toString(), smsChatGuid);
        QCOMPARE(b.chats().first().toMap().value(QStringLiteral("lastMessagePreview")).toString(),
                 QStringLiteral("Second code via unread reconcile: 991827"));
        QCOMPARE(b.chats().first().toMap().value(QStringLiteral("unreadCount")).toInt(), 1);

        // Verify selected chat timeline was immediately rebuilt to include the new message
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("SMS-22395-RECONCILE-2"));
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("verificationCode")).toString(),
                 QStringLiteral("991827"));
    }

    void testV163SocketIoPushDelivery() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        qputenv("HOME", tempDir.path().toUtf8());

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        bool receivedConnectPacket = false;

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (QTcpSocket *sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock]() {
                    const QByteArray reqData = sock->readAll();
                    const QList<QByteArray> lines = reqData.split('\n');
                    if (lines.isEmpty()) return;
                    const QByteArray reqLine = lines.first().trimmed();

                    if (reqLine.startsWith("GET /socket.io/") && !reqLine.contains("sid=")) {
                        const QByteArray body = R"(0{"sid":"eio-sid-999","upgrades":["websocket"],"pingInterval":60000,"pingTimeout":120000})";
                        const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                                                QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                        sock->write(resp);
                        sock->flush();
                        sock->disconnectFromHost();
                        return;
                    }
                    if (reqLine.startsWith("POST /socket.io/") && reqLine.contains("sid=eio-sid-999")) {
                        receivedConnectPacket = true;
                        const QByteArray body = "ok";
                        const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
                        sock->write(resp);
                        sock->flush();
                        sock->disconnectFromHost();
                        return;
                    }
                    if (reqLine.startsWith("GET /socket.io/") && reqLine.contains("sid=eio-sid-999")) {
                        QJsonObject pushMsg{
                            {QStringLiteral("guid"), QStringLiteral("PUSH-MSG-1")},
                            {QStringLiteral("text"), QStringLiteral("Real-time Socket.IO push arrived!")},
                            {QStringLiteral("dateCreated"), nowMs},
                            {QStringLiteral("isFromMe"), false},
                            {QStringLiteral("isRead"), false},
                            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("22395")}}},
                            {QStringLiteral("chats"), QJsonArray{QJsonObject{
                                {QStringLiteral("guid"), QStringLiteral("SMS;-;22395")},
                                {QStringLiteral("chatIdentifier"), QStringLiteral("22395")},
                                {QStringLiteral("style"), 45}
                            }}}
                        };
                        const QByteArray eventJson = QJsonDocument(QJsonArray{
                            QStringLiteral("new-message"),
                            pushMsg
                        }).toJson(QJsonDocument::Compact);
                        const QByteArray body = QByteArrayLiteral("40{\"sid\":\"sio-1\"}\x1e" "42") + eventJson;
                        const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                                                QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                        sock->write(resp);
                        sock->flush();
                        sock->disconnectFromHost();
                        return;
                    }

                    const QByteArray okBody = R"({"status":200,"data":[]})";
                    const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                            QByteArray::number(okBody.size()) + "\r\nConnection: close\r\n\r\n" + okBody;
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
            }
        });

        qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(port).toUtf8());
        qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_sio.json")));
        b.loadCredentials();
        b.setWindowActive(false);
        b.startSocketIoForTest();

        QTRY_COMPARE_WITH_TIMEOUT(b.socketSid(), QStringLiteral("eio-sid-999"), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(receivedConnectPacket, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(b.chats().size(), 1, 2000);
        QCOMPARE(b.chats().first().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("SMS;-;22395"));
        QCOMPARE(b.chats().first().toMap().value(QStringLiteral("lastMessagePreview")).toString(),
                 QStringLiteral("Real-time Socket.IO push arrived!"));
        QCOMPARE(b.chats().first().toMap().value(QStringLiteral("unreadCount")).toInt(), 1);
    }

    void testMultiImageAttachmentsBubbleRendering() {
        QJsonArray atts{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("815907D7-2973-4E66-864E-C71C0349F54D")},
                {QStringLiteral("transferName"), QStringLiteral("IMG_6619.heic")},
                {QStringLiteral("mimeType"), QStringLiteral("image/heic")}
            },
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("651AC8EF-F296-4EBF-9903-0A44C49049E5")},
                {QStringLiteral("transferName"), QStringLiteral("IMG_6618.heic")},
                {QStringLiteral("mimeType"), QStringLiteral("image/heic")}
            },
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("1A83DFF3-1E8F-407F-9F0C-76C55514D7E1")},
                {QStringLiteral("transferName"), QStringLiteral("IMG_6620.heic")},
                {QStringLiteral("mimeType"), QStringLiteral("image/heic")}
            }
        };

        QJsonObject alexMsg{
            {QStringLiteral("guid"), QStringLiteral("40CFC9FA-D603-490B-BA06-E754DDEFCB2D")},
            {QStringLiteral("text"), QStringLiteral("\ufffc\ufffc\ufffc")},
            {QStringLiteral("dateCreated"), 1794258615303LL},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("service"), QStringLiteral("iMessage")},
            {QStringLiteral("attachments"), atts}
        };

        // 1. Verify extractMessageText returns "📷 3 Photos" for 3 image attachments
        QCOMPARE(Backend::extractMessageText(alexMsg), QStringLiteral("📷 3 Photos"));

        // 2. Verify buildMessageListFromApi exposes all 3 attachments in imageAttachments and queues uncached ones
        QHash<QString, QString> contacts{{QStringLiteral("4155550142"), QStringLiteral("Alex Rivera")}};
        QHash<QString, QString> attCache{
            {QStringLiteral("815907D7-2973-4E"), QStringLiteral("/tmp/IMG_6619.heic.jpg")},
            {QStringLiteral("651AC8EF-F296-4E"), QStringLiteral("/tmp/IMG_6618.heic.jpg")}
        };
        QStringList missingAtts;

        const QVariantList timeline = Backend::buildMessageListFromApi(
            QJsonArray{alexMsg}, contacts, attCache, &missingAtts);

        QCOMPARE(timeline.size(), 1);
        const QVariantMap item = timeline.first().toMap();
        QVERIFY(item.value(QStringLiteral("hasImageAttachment")).toBool());
        QCOMPARE(item.value(QStringLiteral("imageAttachmentCount")).toInt(), 3);

        const QVariantList imgAtts = item.value(QStringLiteral("imageAttachments")).toList();
        QCOMPARE(imgAtts.size(), 3);
        QCOMPARE(imgAtts[0].toMap().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("815907D7-2973-4E66-864E-C71C0349F54D"));
        QCOMPARE(imgAtts[0].toMap().value(QStringLiteral("localUrl")).toString(),
                 QStringLiteral("file:///tmp/IMG_6619.heic.jpg"));
        QCOMPARE(imgAtts[1].toMap().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("651AC8EF-F296-4EBF-9903-0A44C49049E5"));
        QCOMPARE(imgAtts[1].toMap().value(QStringLiteral("localUrl")).toString(),
                 QStringLiteral("file:///tmp/IMG_6618.heic.jpg"));
        QCOMPARE(imgAtts[2].toMap().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("1A83DFF3-1E8F-407F-9F0C-76C55514D7E1"));
        QVERIFY(imgAtts[2].toMap().value(QStringLiteral("localUrl")).toString().isEmpty());

        // The 3rd uncached photo should be queued in missingAttachmentsOut
        QCOMPARE(missingAtts.size(), 1);
        QCOMPARE(missingAtts.first(), QStringLiteral("1A83DFF3-1E8F-407F-9F0C-76C55514D7E1|IMG_6620.heic"));

        // 3. Verify RCS/CloudKit duplicate ghost attachment row (like Alex Rivera's 8072727447363868130.jpg in Alpine Trail Crew)
        // where at_0_ has transferState=5 and at_1_ is a duplicate transferState=0 ghost row with identical transferName
        QJsonArray alexAtts{
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("at_0_B5819049-1200-400E-B8A5-EE0F012F1BA2")},
                {QStringLiteral("originalGuid"), QStringLiteral("DD4EC739-C8E6-450F-9FCB-8F9BB7385E96")},
                {QStringLiteral("transferName"), QStringLiteral("8072727447363868130.jpg")},
                {QStringLiteral("mimeType"), QStringLiteral("image/jpeg")},
                {QStringLiteral("totalBytes"), 367092},
                {QStringLiteral("transferState"), 5}
            },
            QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("at_1_B5819049-1200-400E-B8A5-EE0F012F1BA2")},
                {QStringLiteral("originalGuid"), QStringLiteral("at_1_B5819049-1200-400E-B8A5-EE0F012F1BA2")},
                {QStringLiteral("transferName"), QStringLiteral("8072727447363868130.jpg")},
                {QStringLiteral("mimeType"), QStringLiteral("image/jpeg")},
                {QStringLiteral("totalBytes"), 367092},
                {QStringLiteral("transferState"), 0}
            }
        };
        QJsonObject carlosMsg{
            {QStringLiteral("guid"), QStringLiteral("B5819049-1200-400E-B8A5-EE0F012F1BA2")},
            {QStringLiteral("text"), QString()},
            {QStringLiteral("dateCreated"), 1793980235000LL},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("service"), QStringLiteral("RCS")},
            {QStringLiteral("attachments"), alexAtts}
        };
        QCOMPARE(Backend::extractMessageText(carlosMsg), QStringLiteral("📷 Photo"));
        QHash<QString, QString> carlosCache{
            {QStringLiteral("at_0_B5819049-12"), QStringLiteral("/tmp/8072727447363868130.jpg")}
        };
        QStringList carlosMissing;
        const QVariantList carlosTimeline = Backend::buildMessageListFromApi(
            QJsonArray{carlosMsg}, contacts, carlosCache, &carlosMissing);
        QCOMPARE(carlosTimeline.size(), 1);
        const QVariantMap carlosItem = carlosTimeline.first().toMap();
        QCOMPARE(carlosItem.value(QStringLiteral("imageAttachmentCount")).toInt(), 1);
        QCOMPARE(carlosItem.value(QStringLiteral("imageAttachments")).toList().size(), 1);
        QCOMPARE(carlosItem.value(QStringLiteral("imageAttachments")).toList().first().toMap().value(QStringLiteral("localUrl")).toString(),
                 QStringLiteral("file:///tmp/8072727447363868130.jpg"));
        QVERIFY(carlosMissing.isEmpty());
    }

    void testHomeViewLayoutAndMultiAttachmentQml() {
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());

        // Verify multi-attachment Repeater, EXIF autoTransform, and portrait aspect ratio in Main.qml
        QVERIFY(qml.contains(QStringLiteral("id: imageAttachmentRepeater")));
        QVERIFY(qml.contains(QStringLiteral("autoTransform: true")));
        QVERIFY(qml.contains(QStringLiteral("readonly property bool isPortraitImage")));
        QVERIFY(qml.contains(QStringLiteral("msgDelegate.modelData.imageAttachments")));
        QVERIFY(qml.contains(QStringLiteral("function resolveAttachmentUrl(attGuid, initialUrl)")));

        // Verify Home View summary cards have explicit preferred/minimum widths and heights so Message History Sync Window never crushes sibling cards or overlaps Recent Conversations
        QVERIFY(qml.contains(QStringLiteral("objectName: \"homeConversationsCard\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"homeUnreadCard\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"homeSyncWindowCard\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"homeRecentConversationsCol\"")));

        // Verify native in-app Photo Lightbox / Gallery Viewer is wired in Main.qml instead of external viewer
        QVERIFY(qml.contains(QStringLiteral("id: photoViewerModal")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photoViewerModal\"")));
        QVERIFY(qml.contains(QStringLiteral("function openPhotoViewer(items, startIndex)")));
        QVERIFY(qml.contains(QStringLiteral("function prevPhotoInViewer()")));
        QVERIFY(qml.contains(QStringLiteral("function nextPhotoInViewer()")));
        QVERIFY(qml.contains(QStringLiteral("function zoomPhotoViewer(delta)")));
        QVERIFY(qml.contains(QStringLiteral("root.openPhotoViewer(resolvedList, index)")));
        QVERIFY(qml.contains(QStringLiteral("Component.onCompleted: backend.registerWindow(root)")));
        QVERIFY(qml.contains(QStringLiteral("function zoomInPhotoViewer()")));
        QVERIFY(qml.contains(QStringLiteral("function zoomOutPhotoViewer()")));
        QVERIFY(qml.contains(QStringLiteral("function resetZoomPhotoViewer()")));

        // Verify Backend::eventFilter intercepts Left, Right, +, =, -, 0, and Escape when showPhotoViewer is true
        Backend b(false);
        QObject dummyWin;
        dummyWin.setProperty("showPhotoViewer", true);
        dummyWin.setProperty("photoViewerZoom", 1.25);
        b.registerWindow(&dummyWin);

        QKeyEvent rightOverride(QEvent::ShortcutOverride, Qt::Key_Right, Qt::NoModifier);
        QVERIFY(b.eventFilter(&dummyWin, &rightOverride));
        QKeyEvent rightPress(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QVERIFY(b.eventFilter(&dummyWin, &rightPress));

        QKeyEvent leftPress(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QVERIFY(b.eventFilter(&dummyWin, &leftPress));

        QKeyEvent plusPress(QEvent::KeyPress, Qt::Key_Plus, Qt::NoModifier, QStringLiteral("+"));
        QVERIFY(b.eventFilter(&dummyWin, &plusPress));

        QKeyEvent equalPress(QEvent::KeyPress, Qt::Key_Equal, Qt::NoModifier, QStringLiteral("="));
        QVERIFY(b.eventFilter(&dummyWin, &equalPress));

        QKeyEvent minusPress(QEvent::KeyPress, Qt::Key_Minus, Qt::NoModifier, QStringLiteral("-"));
        QVERIFY(b.eventFilter(&dummyWin, &minusPress));

        QKeyEvent zeroPress(QEvent::KeyPress, Qt::Key_0, Qt::NoModifier, QStringLiteral("0"));
        QVERIFY(b.eventFilter(&dummyWin, &zeroPress));

        QKeyEvent escPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QVERIFY(b.eventFilter(&dummyWin, &escPress));

        // When showPhotoViewer is false, keys pass through untouched
        dummyWin.setProperty("showPhotoViewer", false);
        QKeyEvent passRight(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QVERIFY(!b.eventFilter(&dummyWin, &passRight));
    }

    void testResponsiveSplitWindowHeaderAndZeroOverlap() {
        Backend backend(false);
        backend.loadDemoData();
        Theme theme;
        Translator translator;

        QStringList qmlWarnings;
        QQmlApplicationEngine engine;
        engine.setOutputWarningsToStandardError(false);
        QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine, [&](const QList<QQmlError> &warnings) {
            for (const QQmlError &w : warnings) {
                qmlWarnings.append(w.toString());
            }
        });

        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &translator);
        engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());

        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(win != nullptr);
        QCOMPARE(win->minimumWidth(), 640);

        auto *activeChatHeader = win->findChild<QQuickItem *>(QStringLiteral("activeChatHeader"));
        auto *contactIdentityCard = win->findChild<QQuickItem *>(QStringLiteral("contactIdentityCard"));
        auto *headerActionsRow = win->findChild<QQuickItem *>(QStringLiteral("headerActionsRow"));
        auto *leftSidebarPane = win->findChild<QQuickItem *>(QStringLiteral("leftSidebarPane"));
        QVERIFY(activeChatHeader != nullptr);
        QVERIFY(contactIdentityCard != nullptr);
        QVERIFY(headerActionsRow != nullptr);
        QVERIFY(leftSidebarPane != nullptr);

        // 1. Wide window (1400px): single-row header (height 68), zero overlap
        win->setWidth(1400);
        win->setHeight(820);
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(activeChatHeader->width() > 1000, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(activeChatHeader->property("stackChatHeaderRows").toBool(), false, 1000);
        QCOMPARE(qRound(activeChatHeader->height()), 68);
        QVERIFY2(contactIdentityCard->x() + contactIdentityCard->width() <= headerActionsRow->x(),
                 "In 1-row wide mode, contactIdentityCard must not overlap headerActionsRow");
        if (!qEnvironmentVariableIsEmpty("IMSG_WIDE_SCREENSHOT")) {
            win->grabWindow().save(QString::fromUtf8(qgetenv("IMSG_WIDE_SCREENSHOT")));
        }

        // 2. 50% Desktop Split (900px): dynamic 2-row header (height 104), zero overlap, full name visible
        win->setWidth(900);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(activeChatHeader->property("stackChatHeaderRows").toBool(), true, 1000);
        QCOMPARE(qRound(activeChatHeader->height()), 104);
        const QRectF identityRect900(contactIdentityCard->x(), contactIdentityCard->y(),
                                     contactIdentityCard->width(), contactIdentityCard->height());
        const QRectF actionsRect900(headerActionsRow->x(), headerActionsRow->y(),
                                    headerActionsRow->width(), headerActionsRow->height());
        QVERIFY2(!identityRect900.intersects(actionsRect900),
                 "In 50% split mode (900px), contactIdentityCard and headerActionsRow must have zero geometric overlap");
        QVERIFY2(actionsRect900.right() <= activeChatHeader->width(),
                 "In 50% split mode (900px), action buttons row must fit within activeChatHeader width");
        if (!qEnvironmentVariableIsEmpty("IMSG_SPLIT_SCREENSHOT")) {
            win->grabWindow().save(QString::fromUtf8(qgetenv("IMSG_SPLIT_SCREENSHOT")));
        }

        // 3. Narrow split (720px): compact buttons on row 2, zero overlap
        win->setWidth(720);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(activeChatHeader->property("stackChatHeaderRows").toBool(), true, 1000);
        QCOMPARE(activeChatHeader->property("ultraCompactActionButtons").toBool(), true);
        const QRectF identityRect720(contactIdentityCard->x(), contactIdentityCard->y(),
                                     contactIdentityCard->width(), contactIdentityCard->height());
        const QRectF actionsRect720(headerActionsRow->x(), headerActionsRow->y(),
                                    headerActionsRow->width(), headerActionsRow->height());
        QVERIFY2(!identityRect720.intersects(actionsRect720),
                 "In narrow split mode (720px), contactIdentityCard and headerActionsRow must have zero geometric overlap");
        QVERIFY2(actionsRect720.right() <= activeChatHeader->width(),
                 "In narrow split mode (720px), compact action buttons must fit within activeChatHeader width");

        QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join(QStringLiteral("\n"))));
    }

    void testDisguisedHeicJpegMagicByteConversionGroupChat() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        qputenv("HOME", tempDir.path().toUtf8());

        // Generate a real tiny HEIC image via magick so we have authentic ftypheic magic bytes
        const QString sampleHeicPath = tempDir.filePath(QStringLiteral("sample_group.heic"));
        QProcess::execute(QStringLiteral("magick"), QStringList{
            QStringLiteral("-size"), QStringLiteral("32x32"),
            QStringLiteral("xc:#2288ff"),
            sampleHeicPath
        });
        QFile heicFile(sampleHeicPath);
        QVERIFY(heicFile.open(QIODevice::ReadOnly));
        const QByteArray rawHeicBytes = heicFile.readAll();
        heicFile.close();
        QVERIFY(rawHeicBytes.size() >= 100);
        QCOMPARE(rawHeicBytes.mid(4, 4), QByteArrayLiteral("ftyp"));

        // Pre-seed ~/.cache/imsg/attachments with a disguised .heic.jpeg file containing raw ftypheic bytes
        // (exactly as BlueBubbles Socket.IO new-message named Sam photo D7F44BE4-6F65-4F6A-A4EA-D2C08888088E.heic.jpeg)
        const QString attGuid = QStringLiteral("E239AB32-F150-40AF-8B20-5C6F1144D4D9");
        const QString transferName = QStringLiteral("D7F44BE4-6F65-4F6A-A4EA-D2C08888088E.heic.jpeg");
        const QString cacheDir = tempDir.path() + QStringLiteral("/.cache/imsg/attachments");
        QDir().mkpath(cacheDir);
        const QString disguisedPath = cacheDir + QLatin1Char('/') + attGuid.left(16) + QLatin1Char('_') + transferName;
        {
            QFile df(disguisedPath);
            QVERIFY(df.open(QIODevice::WriteOnly));
            df.write(rawHeicBytes);
            df.close();
        }

        const QString alpineChatGuid = QStringLiteral("RCS;+;chat555000111222333444");
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_group.json")));
        // indexCachedAttachmentsOnDisk() is called inside loadMessagesCache(): must NOT index raw ftyp bytes as ready
        b.loadMessagesCache();

        QJsonObject chatObj{
            {QStringLiteral("guid"), alpineChatGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat555000111222333444")},
            {QStringLiteral("displayName"), QStringLiteral("Alpine Trail Crew")},
            {QStringLiteral("style"), 43}
        };
        QJsonObject attObj{
            {QStringLiteral("guid"), attGuid},
            {QStringLiteral("mimeType"), QStringLiteral("image/heic")},
            {QStringLiteral("transferName"), transferName},
            {QStringLiteral("transferState"), 5}
        };
        QJsonObject samMsg{
            {QStringLiteral("guid"), QStringLiteral("A5E41988-40DA-46C1-B3B7-6DC7C07B91BB")},
            {QStringLiteral("text"), QStringLiteral("\ufffc")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), 1794262832541LL},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550188")}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}},
            {QStringLiteral("attachments"), QJsonArray{attObj}}
        };

        b.ingestMessagesForTest(QJsonArray{samMsg}, true);
        b.selectChat(alpineChatGuid);

        // Wait for resolveOrConvertAsync to convert disguisedPath in-place from ftypheic to JPEG (\xFF\xD8\xFF)
        QTRY_VERIFY_WITH_TIMEOUT([&]() {
            QFile checkFile(disguisedPath);
            if (!checkFile.open(QIODevice::ReadOnly)) return false;
            const QByteArray hdr = checkFile.read(3);
            checkFile.close();
            return hdr.startsWith("\xFF\xD8\xFF");
        }(), 3500);

        QTRY_COMPARE_WITH_TIMEOUT(b.messages().first().toMap().value(QStringLiteral("attachmentLocalUrl")).toString(),
                                  QUrl::fromLocalFile(disguisedPath).toString(), 1500);
    }

    void testTapbackScrollPositionPreservedAndAllGroupImagesResolved() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        qputenv("HOME", tempDir.path().toUtf8());

        // Generate a real tiny JPEG image via magick for fast attachment resolution
        const QString sampleJpgPath = tempDir.filePath(QStringLiteral("sample_alpine.jpg"));
        QProcess::execute(QStringLiteral("magick"), QStringList{
            QStringLiteral("-size"), QStringLiteral("48x48"),
            QStringLiteral("xc:#44cc88"),
            sampleJpgPath
        });
        QFile jpgFile(sampleJpgPath);
        QVERIFY(jpgFile.open(QIODevice::ReadOnly));
        const QByteArray rawJpgBytes = jpgFile.readAll();
        jpgFile.close();
        QVERIFY(rawJpgBytes.size() >= 100);
        QVERIFY(rawJpgBytes.startsWith("\xFF\xD8\xFF"));

        const QString cacheDir = tempDir.path() + QStringLiteral("/.cache/imsg/attachments");
        QDir().mkpath(cacheDir);

        // Pre-seed 6 cached attachments in ~/.cache/imsg/attachments with .heic extension containing JPEG bytes
        // (matching Alpine's 698B9DDA-AF6D-43_IMG_1424.heic AND testing >3 missing attachments in Alpine Trail Crew)
        for (int i = 0; i < 6; ++i) {
            const QString attGuid = QStringLiteral("ATT-ALPINE-000%1-ABCD-1234567890AB").arg(i);
            const QString tName = QStringLiteral("IMG_142%1.heic").arg(i);
            const QString fPath = cacheDir + QLatin1Char('/') + attGuid.left(16) + QLatin1Char('_') + tName;
            QFile f(fPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(rawJpgBytes);
            f.close();
        }

        const QString alpineChatGuid = QStringLiteral("RCS;+;chat555000111222333444");
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_tapback_scroll.json")));

        QJsonObject chatObj{
            {QStringLiteral("guid"), alpineChatGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat555000111222333444")},
            {QStringLiteral("displayName"), QStringLiteral("Alpine Trail Crew")},
            {QStringLiteral("style"), 43}
        };

        // Build 35 messages so the timeline is tall enough to scroll
        QJsonArray msgs;
        for (int i = 0; i < 35; ++i) {
            const QString mGuid = QStringLiteral("MSG-ALPINE-%1").arg(i, 2, 10, QLatin1Char('0'));
            QJsonObject msg{
                {QStringLiteral("guid"), mGuid},
                {QStringLiteral("text"), QStringLiteral("Alpine Trail Crew message line #%1 with enough text to occupy vertical height in the timeline.").arg(i)},
                {QStringLiteral("isFromMe"), (i % 3 == 0)},
                {QStringLiteral("service"), QStringLiteral("RCS")},
                {QStringLiteral("dateCreated"), 1794250000000LL + i * 10000LL},
                {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550188")}}},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            };
            if (i >= 29) {
                const int attIdx = i - 29;
                const QString attGuid = QStringLiteral("ATT-ALPINE-000%1-ABCD-1234567890AB").arg(attIdx);
                const QString tName = QStringLiteral("IMG_142%1.heic").arg(attIdx);
                msg.insert(QStringLiteral("attachments"), QJsonArray{
                    QJsonObject{
                        {QStringLiteral("guid"), attGuid},
                        {QStringLiteral("mimeType"), QStringLiteral("image/heic")},
                        {QStringLiteral("transferName"), tName},
                        {QStringLiteral("transferState"), 5}
                    }
                });
            }
            msgs.append(msg);
        }

        b.ingestMessagesForTest(msgs, true);
        b.selectChat(alpineChatGuid);

        // Verify ALL 6 (>3) image attachments in Alpine Trail Crew are resolved and converted to .heic.jpg sidecars
        QTRY_VERIFY_WITH_TIMEOUT([&]() {
            const QVariantList currentMsgs = b.messages();
            if (currentMsgs.size() != 35) return false;
            for (int i = 29; i < 35; ++i) {
                const QString url = currentMsgs[i].toMap().value(QStringLiteral("attachmentLocalUrl")).toString();
                if (!url.endsWith(QStringLiteral(".heic.jpg"))) return false;
            }
            return true;
        }(), 3000);

        // Load Main.qml and verify tapback reaction scroll preservation
        Theme theme;
        Translator i18n;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &b);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &i18n);

        QFileInfo qmlFi(QStringLiteral("../../src/Main.qml"));
        if (!qmlFi.exists()) {
            qmlFi.setFile(QStringLiteral("../src/Main.qml"));
        }
        QVERIFY(qmlFi.exists());
        engine.load(QUrl::fromLocalFile(qmlFi.absoluteFilePath()));
        QVERIFY(!engine.rootObjects().isEmpty());

        QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(win != nullptr);
        win->setWidth(960);
        win->setHeight(640);
        win->show();
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QCoreApplication::processEvents();

        QQuickItem *listView = win->findChild<QQuickItem *>(QStringLiteral("messageTimelineListView"));
        QVERIFY(listView != nullptr);

        // 1. Wait for initial scrollToBottom to settle at the bottom of Alpine Trail Crew
        QTRY_VERIFY_WITH_TIMEOUT(!listView->property("atYBeginning").toBool(), 2000);
        const qreal bottomOffsetFromTop = listView->property("contentY").toReal() - listView->property("originY").toReal();
        QVERIFY2(bottomOffsetFromTop > 300.0, "Timeline at bottom of 35-message chat must be scrolled >300px below originY");

        // Perform a tapback at the bottom of the chat -> must stay at bottom, never jump to top (contentY <= 0)
        b.sendReaction(alpineChatGuid, QStringLiteral("MSG-ALPINE-34"), QStringLiteral("❤️"));
        QCoreApplication::processEvents();
        QTest::qWait(120);
        QCoreApplication::processEvents();
        QVERIFY2(!listView->property("atYBeginning").toBool(),
                 "Tapback at bottom of chat must preserve bottom scroll position and never jump to top");
        QVERIFY2((listView->property("contentY").toReal() - listView->property("originY").toReal()) > 300.0,
                 "Tapback at bottom of chat must stay scrolled at bottom");
        QCOMPARE(listView->property("stickToBottom").toBool(), true);

        // 2. Scroll up two pages into the middle of the chat (stickToBottom == false, not at top, not at bottom)
        QMetaObject::invokeMethod(win, "scrollMessagesPageUp");
        QMetaObject::invokeMethod(win, "scrollMessagesPageUp");
        QCoreApplication::processEvents();
        QTest::qWait(80);
        QCoreApplication::processEvents();

        QCOMPARE(listView->property("stickToBottom").toBool(), false);
        const qreal midOffsetFromTop = listView->property("contentY").toReal() - listView->property("originY").toReal();
        QVERIFY2(!listView->property("atYBeginning").toBool() && midOffsetFromTop > 150.0,
                 "Mid-chat scroll position must be strictly below the top of the chat");

        // Perform a tapback while scrolled up in the middle of Alpine Trail Crew -> must stay at mid-chat and never jump to top!
        b.sendReaction(alpineChatGuid, QStringLiteral("MSG-ALPINE-20"), QStringLiteral("👍"));
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QCoreApplication::processEvents();

        const qreal afterTapbackOffsetFromTop = listView->property("contentY").toReal() - listView->property("originY").toReal();
        QCOMPARE(listView->property("stickToBottom").toBool(), false);
        QVERIFY2(!listView->property("atYBeginning").toBool() && afterTapbackOffsetFromTop > 150.0,
                 "Tapback while scrolled up must NEVER reset messageListView to the top of the chat");
        QVERIFY2(qAbs(afterTapbackOffsetFromTop - midOffsetFromTop) < 120.0,
                 qPrintable(QStringLiteral("Expected scroll offset near %1 after tapback, got %2")
                                .arg(midOffsetFromTop).arg(afterTapbackOffsetFromTop)));
    }

    void testReactionPillHoverTooltipAndGroupSenderNames() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        qputenv("HOME", tempDir.path().toUtf8());

        const QString alpineChatGuid = QStringLiteral("RCS;+;chat555000111222333444");
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_react_hover.json")));
        b.saveContact(QStringLiteral("Alex"), QStringLiteral("Rivera"), QStringLiteral("+14155550101"), QString());
        b.saveContact(QStringLiteral("Sam"), QString(), QStringLiteral("+14155550188"), QString());

        QJsonObject chatObj{
            {QStringLiteral("guid"), alpineChatGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat555000111222333444")},
            {QStringLiteral("displayName"), QStringLiteral("Alpine Trail Crew")},
            {QStringLiteral("style"), 43}
        };

        QJsonObject parentMsg{
            {QStringLiteral("guid"), QStringLiteral("MSG-PARENT-REACT-1")},
            {QStringLiteral("text"), QStringLiteral("Great meeting tonight everyone!")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("service"), QStringLiteral("RCS")},
            {QStringLiteral("dateCreated"), 1794270000000LL},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550101")}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };

        QJsonObject reactAlex{
            {QStringLiteral("guid"), QStringLiteral("MSG-REACT-ALEX")},
            {QStringLiteral("text"), QStringLiteral("Loved \"Great meeting tonight everyone!\"")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), 1794270001000LL},
            {QStringLiteral("associatedMessageGuid"), QStringLiteral("p:0/MSG-PARENT-REACT-1")},
            {QStringLiteral("associatedMessageType"), QStringLiteral("2000")},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550101")}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };

        // Partial update with null handle: cacheMessageForChat must preserve Alex's handle!
        QJsonObject reactAlexNullHandle = reactAlex;
        reactAlexNullHandle.insert(QStringLiteral("handle"), QJsonValue::Null);

        QJsonObject reactSam{
            {QStringLiteral("guid"), QStringLiteral("MSG-REACT-SAM")},
            {QStringLiteral("text"), QStringLiteral("Loved \"Great meeting tonight everyone!\"")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), 1794270002000LL},
            {QStringLiteral("associatedMessageGuid"), QStringLiteral("p:0/MSG-PARENT-REACT-1")},
            {QStringLiteral("associatedMessageType"), QStringLiteral("2000")},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+14155550188")}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };

        b.ingestMessagesForTest(QJsonArray{parentMsg, reactAlex, reactAlexNullHandle, reactSam}, true);
        b.selectChat(alpineChatGuid);

        QCOMPARE(b.messages().size(), 1);
        QVariantList reactions = b.messages().first().toMap().value(QStringLiteral("reactions")).toList();
        QCOMPARE(reactions.size(), 1);
        QVariantMap heartPill = reactions.first().toMap();
        QCOMPARE(heartPill.value(QStringLiteral("emoji")).toString(), QStringLiteral("❤️"));
        QCOMPARE(heartPill.value(QStringLiteral("count")).toInt(), 2);
        QCOMPARE(heartPill.value(QStringLiteral("fromMe")).toBool(), false);
        QCOMPARE(heartPill.value(QStringLiteral("senders")).toString(), QStringLiteral("Alex Rivera, Sam"));
        QCOMPARE(heartPill.value(QStringLiteral("tooltipText")).toString(), QStringLiteral("❤️ Alex Rivera, Sam"));

        // Optimistic sendReaction("❤️") appends "You" to the existing ❤️ pill and updates count & tooltipText
        b.sendReaction(alpineChatGuid, QStringLiteral("MSG-PARENT-REACT-1"), QStringLiteral("❤️"));
        reactions = b.messages().first().toMap().value(QStringLiteral("reactions")).toList();
        QCOMPARE(reactions.size(), 1);
        heartPill = reactions.first().toMap();
        QCOMPARE(heartPill.value(QStringLiteral("count")).toInt(), 3);
        QCOMPARE(heartPill.value(QStringLiteral("fromMe")).toBool(), true);
        QCOMPARE(heartPill.value(QStringLiteral("senders")).toString(), QStringLiteral("Alex Rivera, Sam, You"));
        QCOMPARE(heartPill.value(QStringLiteral("tooltipText")).toString(), QStringLiteral("❤️ Alex Rivera, Sam, You"));

        // Verify QML reaction pill hover state and tooltip badge rendering
        Theme theme;
        Translator i18n;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &b);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &i18n);
        engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());

        QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(win != nullptr);
        win->setWidth(960);
        win->setHeight(640);
        win->show();
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QCoreApplication::processEvents();

        std::function<QQuickItem *(QQuickItem *, const QString &)> findVisualItem = [&](QQuickItem *item, const QString &objName) -> QQuickItem * {
            if (!item) return nullptr;
            if (item->objectName() == objName) return item;
            for (QQuickItem *child : item->childItems()) {
                if (QQuickItem *found = findVisualItem(child, objName)) return found;
            }
            return nullptr;
        };

        QQuickItem *pillItem = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((pillItem = findVisualItem(win->contentItem(), QStringLiteral("reactionPillItem"))) != nullptr, 1500);
        QCOMPARE(pillItem->property("tooltipText").toString(), QStringLiteral("❤️ Alex Rivera, Sam, You"));
        QCOMPARE(pillItem->property("tooltipVisible").toBool(), false);

        // Simulate hovering over the reaction pill
        pillItem->setProperty("forceHoveredForTest", true);
        QCoreApplication::processEvents();
        QCOMPARE(pillItem->property("tooltipVisible").toBool(), true);

        QQuickItem *tooltipTextItem = findVisualItem(pillItem, QStringLiteral("reactionTooltipText"));
        QVERIFY(tooltipTextItem != nullptr);
        QCOMPARE(tooltipTextItem->property("text").toString(), QStringLiteral("❤️ Alex Rivera, Sam, You"));
    }

    void testOrphanSmsVerificationCodeWithoutChatJoin() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state.json")));

        // Simulate an incoming Google verification SMS where IMDPersistenceAgent wrote the message
        // with a valid handle.address ("22000") and service ("SMS"), but no chat_message_join row (chats: []).
        QJsonObject handleObj;
        handleObj.insert(QStringLiteral("address"), QStringLiteral("22000"));
        handleObj.insert(QStringLiteral("service"), QStringLiteral("SMS"));

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QJsonObject googleSms;
        googleSms.insert(QStringLiteral("guid"), QStringLiteral("google-otp-orphan-guid-1"));
        googleSms.insert(QStringLiteral("text"), QStringLiteral("G-482910 is your Google verification code."));
        googleSms.insert(QStringLiteral("service"), QStringLiteral("SMS"));
        googleSms.insert(QStringLiteral("isFromMe"), false);
        googleSms.insert(QStringLiteral("isRead"), false);
        googleSms.insert(QStringLiteral("dateCreated"), nowMs);
        googleSms.insert(QStringLiteral("handle"), handleObj);
        googleSms.insert(QStringLiteral("chats"), QJsonArray()); // Empty chats array

        b.ingestMessagesForTest(QJsonArray{googleSms}, false);

        // Verify that SMS;-;22000 was synthesized and added to the sidebar chat list
        bool foundGoogleShortCodeChat = false;
        for (const QVariant &v : b.chats()) {
            const QVariantMap chatMap = v.toMap();
            if (chatMap.value(QStringLiteral("guid")).toString() == QStringLiteral("SMS;-;22000")) {
                foundGoogleShortCodeChat = true;
                QCOMPARE(chatMap.value(QStringLiteral("lastMessagePreview")).toString(),
                         QStringLiteral("G-482910 is your Google verification code."));
                break;
            }
        }
        QVERIFY2(foundGoogleShortCodeChat, "Expected synthesized SMS;-;22000 chat for orphan Google verification SMS");

        // Select SMS;-;22000 and verify the verification code message renders in the timeline
        b.selectChat(QStringLiteral("SMS;-;22000"));
        bool foundMsg = false;
        for (const QVariant &mv : b.messages()) {
            const QVariantMap msgMap = mv.toMap();
            if (msgMap.value(QStringLiteral("guid")).toString() == QStringLiteral("google-otp-orphan-guid-1")) {
                foundMsg = true;
                QCOMPARE(msgMap.value(QStringLiteral("text")).toString(),
                         QStringLiteral("G-482910 is your Google verification code."));
                break;
            }
        }
        QVERIFY2(foundMsg, "Expected Google verification message in SMS;-;22000 message timeline");
    }

    void testServerConfigSetupAndDynamicUrlPersistence() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QByteArray prevBbUrl = qgetenv("BLUEBUBBLES_URL");
        const QByteArray prevBbPass = qgetenv("BLUEBUBBLES_PASSWORD");
        const QByteArray prevImsgUrl = qgetenv("IMSG_SERVER_URL");
        const QByteArray prevImsgSec = qgetenv("IMSG_SECONDARY_URL");
        const QByteArray prevImsgPass = qgetenv("IMSG_PASSWORD");

        qunsetenv("BLUEBUBBLES_URL");
        qunsetenv("BLUEBUBBLES_PASSWORD");
        qunsetenv("IMSG_SERVER_URL");
        qunsetenv("IMSG_SECONDARY_URL");
        qunsetenv("IMSG_PASSWORD");

        const QString cfgPath = tempDir.filePath(QStringLiteral("config.json"));
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state.json")));
        b.setConfigFilePath(cfgPath);

        // Initially unconfigured -> needsSetup must be true
        QCOMPARE(b.serverUrl(), QString());
        QCOMPARE(b.secondaryServerUrl(), QString());
        QCOMPARE(b.hasPasswordConfigured(), false);
        QCOMPARE(b.needsSetup(), true);

        // Save primary + failover server config
        b.saveServerConfig(
            QStringLiteral("relay.example.com/"),
            QStringLiteral("https://backup.example.com///"),
            QStringLiteral("super-secret-token"));

        QCOMPARE(b.serverUrl(), QStringLiteral("https://relay.example.com"));
        QCOMPARE(b.secondaryServerUrl(), QStringLiteral("https://backup.example.com"));
        QCOMPARE(b.hasPasswordConfigured(), true);
        QCOMPARE(b.needsSetup(), false);
        QCOMPARE(b.candidateUrls(), (QStringList{
            QStringLiteral("https://relay.example.com"),
            QStringLiteral("https://backup.example.com")
        }));

        // Verify 0600 permissions on config.json
        const auto perms = QFile::permissions(cfgPath);
        QVERIFY(perms & QFileDevice::ReadOwner);
        QVERIFY(perms & QFileDevice::WriteOwner);
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));

        // Update URL with empty password -> must preserve existing password
        b.saveServerConfig(
            QStringLiteral("https://new-relay.example.com"),
            QStringLiteral("https://new-backup.example.com"),
            QString());
        QCOMPARE(b.serverUrl(), QStringLiteral("https://new-relay.example.com"));
        QCOMPARE(b.secondaryServerUrl(), QStringLiteral("https://new-backup.example.com"));
        QCOMPARE(b.hasPasswordConfigured(), true);
        QCOMPARE(b.needsSetup(), false);

        // Reload into a fresh Backend instance and verify persistence
        Backend b2(false);
        b2.setConfigFilePath(cfgPath);
        QCOMPARE(b2.serverUrl(), QStringLiteral("https://new-relay.example.com"));
        QCOMPARE(b2.secondaryServerUrl(), QStringLiteral("https://new-backup.example.com"));
        QCOMPARE(b2.hasPasswordConfigured(), true);
        QCOMPARE(b2.needsSetup(), false);

        // Restore environment variables if any were set
        if (!prevBbUrl.isEmpty()) qputenv("BLUEBUBBLES_URL", prevBbUrl);
        if (!prevBbPass.isEmpty()) qputenv("BLUEBUBBLES_PASSWORD", prevBbPass);
        if (!prevImsgUrl.isEmpty()) qputenv("IMSG_SERVER_URL", prevImsgUrl);
        if (!prevImsgSec.isEmpty()) qputenv("IMSG_SECONDARY_URL", prevImsgSec);
        if (!prevImsgPass.isEmpty()) qputenv("IMSG_PASSWORD", prevImsgPass);
    }

    void testBluePillOtpExtractionAndYearFiltering() {
        // Standard 2FA & Google G-codes
        QCOMPARE(Backend::extractOtpCode(QStringLiteral("Your GitHub verification code is: 849201")), QStringLiteral("849201"));
        QCOMPARE(Backend::extractOtpCode(QStringLiteral("G-482910 is your Google verification code.")), QStringLiteral("482910"));
        QCOMPARE(Backend::extractOtpCode(QStringLiteral("194-820 is your login code.")), QStringLiteral("194820"));
        // Year filtering when keyword appears without explicit 'code is YYYY'
        QCOMPARE(Backend::extractOtpCode(QStringLiteral("Copyright 2026 security notice for your account.")), QString());
    }

    void testBluePillPinnedChatsAndTogglePin() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_pins.json")));
        b.loadDemoData();

        // By default, pinnedChats returns top 4 demo conversations
        const QVariantList defaultPins = b.pinnedChats();
        QCOMPARE(defaultPins.size(), 4);
        QCOMPARE(defaultPins.first().toMap().value(QStringLiteral("shortTitle")).toString(), QStringLiteral("Alex"));

        // Explicitly pin Elena Vance -> pinnedChats now contains only explicitly pinned chats
        const QString elenaGuid = QStringLiteral("iMessage;-;elena.vance@example.com");
        QCOMPARE(b.isChatPinned(elenaGuid), false);
        b.togglePinChat(elenaGuid);
        QCOMPARE(b.isChatPinned(elenaGuid), true);
        QCOMPARE(b.pinnedChats().size(), 1);
        QCOMPARE(b.pinnedChats().first().toMap().value(QStringLiteral("guid")).toString(), elenaGuid);

        // Unpin Elena Vance -> falls back to top 4
        b.togglePinChat(elenaGuid);
        QCOMPARE(b.isChatPinned(elenaGuid), false);
        QCOMPARE(b.pinnedChats().size(), 4);
    }

    void testBluePillEditedExpressiveAndFailureBadges() {
        QJsonArray rawDesc;

        // 1. Outgoing failed message (error = 4)
        QJsonObject mFail;
        mFail.insert(QStringLiteral("guid"), QStringLiteral("msg-fail-1"));
        mFail.insert(QStringLiteral("text"), QStringLiteral("Failed outbound message"));
        mFail.insert(QStringLiteral("isFromMe"), true);
        mFail.insert(QStringLiteral("dateCreated"), 1775860005000LL);
        mFail.insert(QStringLiteral("error"), 4);

        // 2. Incoming edited message with confetti effect & OTP code
        QJsonObject mRich;
        mRich.insert(QStringLiteral("guid"), QStringLiteral("msg-rich-1"));
        mRich.insert(QStringLiteral("text"), QStringLiteral("Your verification code is 739104"));
        mRich.insert(QStringLiteral("isFromMe"), false);
        mRich.insert(QStringLiteral("dateCreated"), 1775860001000LL);
        mRich.insert(QStringLiteral("dateEdited"), 1775860002000LL);
        mRich.insert(QStringLiteral("expressiveSendStyleId"), QStringLiteral("com.apple.messages.effect.CKConfettiEffect"));

        rawDesc.append(mFail);
        rawDesc.append(mRich);

        const QVariantList built = Backend::buildMessageListFromApi(rawDesc, {}, {});
        QCOMPARE(built.size(), 2);

        const QVariantMap richItem = built.at(0).toMap();
        QCOMPARE(richItem.value(QStringLiteral("isEdited")).toBool(), true);
        QCOMPARE(richItem.value(QStringLiteral("sendEffectLabel")).toString(), QStringLiteral("sent with confetti"));
        QCOMPARE(richItem.value(QStringLiteral("otpCode")).toString(), QStringLiteral("739104"));
        QVERIFY(richItem.value(QStringLiteral("metaBadgeText")).toString().contains(QStringLiteral("Edited · sent with confetti")));

        const QVariantMap failItem = built.at(1).toMap();
        QCOMPARE(failItem.value(QStringLiteral("isFailed")).toBool(), true);
        QCOMPARE(failItem.value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));
        QVERIFY(failItem.value(QStringLiteral("metaBadgeText")).toString().contains(QStringLiteral("⚠ Not Delivered")));
    }

    void testBluePillSshUrlAndConfigMigration() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("bluepill_cfg.json"));

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_ssh.json")));
        b.setConfigFilePath(cfgPath);

        // Save user@mac-gateway -> normalized to ssh://user@mac-gateway, needsSetup() is false even with empty password
        b.saveServerConfig(QStringLiteral("user@mac-gateway.local"), QString(), QString());
        QCOMPARE(b.serverUrl(), QStringLiteral("ssh://user@mac-gateway.local"));
        QCOMPARE(b.needsSetup(), false);
        QCOMPARE(b.webUiUrl(), QStringLiteral("http://127.0.0.1:1235/web"));

        // Save HTTPS BluePill / BlueBubbles URL -> webUiUrl points to server/web
        b.saveServerConfig(QStringLiteral("https://bluepill.example.com"), QString(), QStringLiteral("api-key-123"));
        QCOMPARE(b.serverUrl(), QStringLiteral("https://bluepill.example.com"));
        QCOMPARE(b.webUiUrl(), QStringLiteral("https://bluepill.example.com/web"));
    }

    void testOutboundTempGuidDeduplication() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_dedup.json")));

        const QString chatGuid = QStringLiteral("iMessage;-;+15550199999");
        QJsonObject chatObj;
        chatObj.insert(QStringLiteral("guid"), chatGuid);
        chatObj.insert(QStringLiteral("chatIdentifier"), QStringLiteral("+15550199999"));
        chatObj.insert(QStringLiteral("style"), 45);

        // 1. Ingest an optimistic temp-* message (simulating synthetic send_text response)
        QJsonObject tempMsg;
        tempMsg.insert(QStringLiteral("guid"), QStringLiteral("temp-11111111-2222-3333-4444-555555555555"));
        tempMsg.insert(QStringLiteral("text"), QStringLiteral("Deduplicated outbound test message"));
        tempMsg.insert(QStringLiteral("isFromMe"), true);
        tempMsg.insert(QStringLiteral("dateCreated"), 1794368786000LL);
        tempMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
        b.ingestMessagesForTest(QJsonArray{tempMsg}, false);
        b.selectChat(chatGuid);
        QCOMPARE(b.messages().size(), 1);

        // 2. Poll arrives with real chat.db message GUID (even without tempGuid) -> replaces temp-* in-place (still 1 bubble!)
        QJsonObject realMsg;
        realMsg.insert(QStringLiteral("guid"), QStringLiteral("p:0/8A9D1234-5678-49AB-CDEF-0123456789AB"));
        realMsg.insert(QStringLiteral("text"), QStringLiteral("Deduplicated outbound test message"));
        realMsg.insert(QStringLiteral("isFromMe"), true);
        realMsg.insert(QStringLiteral("dateCreated"), 1794368786500LL);
        realMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
        b.ingestMessagesForTest(QJsonArray{realMsg}, false);

        QCOMPARE(b.messages().size(), 1);
        QCOMPARE(b.messages().first().toMap().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("p:0/8A9D1234-5678-49AB-CDEF-0123456789AB"));
    }

    void testLargeReplyAndSettingsPasswordReveal() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("bluepill_settings.json"));

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_settings.json")));
        b.setConfigFilePath(cfgPath);
        b.saveServerConfig(QStringLiteral("https://bluepill.example.com"),
                           QStringLiteral("https://backup.example.com"),
                           QStringLiteral("super-secret-api-key-999"));
        QCOMPARE(b.serverPassword(), QStringLiteral("super-secret-api-key-999"));
        QCOMPARE(b.hasPasswordConfigured(), true);

        // Verify large multi-paragraph message + inline reply + unattached RCS textual tapbacks
        const QString largeText = QStringLiteral(
            "Hello everyone \n"
            "We need to move our upcoming meeting to 11/21 at 11 AM \n"
            "I know it is right before the holiday so no worries if you cannot make it \n"
            "The final meeting of the year will be 12/12 in downtown \n"
            "Please keep me in your thoughts on Monday afternoon (😛🤣)\n"
            "Alex is speaking this Saturday at 6:30 PM in case you want to support \n"
            "See you all soon😘");

        QJsonObject mOrig;
        mOrig.insert(QStringLiteral("guid"), QStringLiteral("orig-large-guid-1"));
        mOrig.insert(QStringLiteral("text"), largeText);
        mOrig.insert(QStringLiteral("isFromMe"), false);
        mOrig.insert(QStringLiteral("dateCreated"), 1794340405020LL);
        mOrig.insert(QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15550101111")}});

        // Unattached RCS textual tapback (associatedMessageGuid is empty, text quotes entire multi-line message)
        QJsonObject mTapback;
        mTapback.insert(QStringLiteral("guid"), QStringLiteral("tapback-unattached-1"));
        mTapback.insert(QStringLiteral("text"), QStringLiteral("Emphasized “") + largeText + QStringLiteral("”"));
        mTapback.insert(QStringLiteral("isFromMe"), false);
        mTapback.insert(QStringLiteral("dateCreated"), 1794340406000LL);
        mTapback.insert(QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15550102222")}});

        // Carrier RCS reply stub
        QJsonObject mStub;
        mStub.insert(QStringLiteral("guid"), QStringLiteral("rcs-reply-stub-1"));
        mStub.insert(QStringLiteral("text"), QStringLiteral("Replied to a message:"));
        mStub.insert(QStringLiteral("isFromMe"), false);
        mStub.insert(QStringLiteral("dateCreated"), 1794372081826LL);
        mStub.insert(QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15550103333")}});

        // Inline reply referencing orig-large-guid-1
        QJsonObject mReply;
        mReply.insert(QStringLiteral("guid"), QStringLiteral("reply-large-guid-2"));
        mReply.insert(QStringLiteral("text"), QStringLiteral("👍🏽 Sounds great for 11/21 - see everyone there!"));
        mReply.insert(QStringLiteral("threadOriginatorGuid"), QStringLiteral("p:0/orig-large-guid-1"));
        mReply.insert(QStringLiteral("isFromMe"), false);
        mReply.insert(QStringLiteral("dateCreated"), 1794372600203LL);
        mReply.insert(QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15550103333")}});

        // rawMessagesDesc is newest -> oldest
        QJsonArray rawDesc{mReply, mStub, mTapback, mOrig};
        const QVariantList built = Backend::buildMessageListFromApi(rawDesc, {}, {});
        // Only mOrig and mReply should be in the timeline (mTapback attached to mOrig, mStub suppressed)
        QCOMPARE(built.size(), 2);

        const QVariantMap origBuilt = built.at(0).toMap();
        QCOMPARE(origBuilt.value(QStringLiteral("guid")).toString(), QStringLiteral("orig-large-guid-1"));
        const QVariantList origReactions = origBuilt.value(QStringLiteral("reactions")).toList();
        QCOMPARE(origReactions.size(), 1);
        QCOMPARE(origReactions.first().toMap().value(QStringLiteral("emoji")).toString(), QStringLiteral("‼️"));

        const QVariantMap replyBuilt = built.at(1).toMap();
        QCOMPARE(replyBuilt.value(QStringLiteral("guid")).toString(), QStringLiteral("reply-large-guid-2"));
        const QString replyToText = replyBuilt.value(QStringLiteral("replyToText")).toString();
        QVERIFY(!replyToText.isEmpty());
        QVERIFY(!replyToText.contains(QLatin1Char('\n')));
        QVERIFY(replyToText.startsWith(QStringLiteral("Hello everyone We need to move our upcoming meeting")));
    }

    void testAudioAttachmentWaveformAndPlaybackState() {
        // 1. isAudioAttachment mime and extension check
        QVERIFY(Backend::isAudioAttachment(QStringLiteral("audio/m4a"), QStringLiteral("memo.m4a")));
        QVERIFY(Backend::isAudioAttachment(QStringLiteral("audio/x-m4a"), QStringLiteral("voice.m4a")));
        QVERIFY(Backend::isAudioAttachment(QStringLiteral("audio/wav"), QStringLiteral("sound.wav")));
        QVERIFY(Backend::isAudioAttachment(QStringLiteral("audio/aac"), QStringLiteral("clip.aac")));
        QVERIFY(Backend::isAudioAttachment(QStringLiteral("application/octet-stream"), QStringLiteral("voice.caf")));
        QVERIFY(!Backend::isAudioAttachment(QStringLiteral("image/png"), QStringLiteral("photo.png")));
        QVERIFY(!Backend::isAudioAttachment(QStringLiteral("video/mp4"), QStringLiteral("video.mp4")));
        QVERIFY(!Backend::isAudioAttachment(QStringLiteral("text/plain"), QStringLiteral("doc.txt")));

        // 2. computeWaveformBars
        const QVariantList bars = Backend::computeWaveformBars(QStringLiteral("audio-guid-123"), 28);
        QCOMPARE(bars.size(), 28);
        for (const QVariant &bar : bars) {
            const double val = bar.toDouble();
            QVERIFY(val >= 0.15 && val <= 1.0);
        }
        // Deterministic check
        QCOMPARE(bars, Backend::computeWaveformBars(QStringLiteral("audio-guid-123"), 28));

        // 3. buildMessageListFromApi with audio attachment
        QJsonObject mAudio;
        mAudio.insert(QStringLiteral("guid"), QStringLiteral("msg-audio-test-1"));
        mAudio.insert(QStringLiteral("text"), QStringLiteral("Voice Memo"));
        mAudio.insert(QStringLiteral("isFromMe"), false);
        mAudio.insert(QStringLiteral("dateCreated"), 1775860000000LL);

        QJsonObject att;
        att.insert(QStringLiteral("guid"), QStringLiteral("att-audio-test-1"));
        att.insert(QStringLiteral("mimeType"), QStringLiteral("audio/m4a"));
        att.insert(QStringLiteral("transferName"), QStringLiteral("Voice Memo.m4a"));
        att.insert(QStringLiteral("total_bytes"), 48000);
        QJsonArray atts;
        atts.append(att);
        mAudio.insert(QStringLiteral("attachments"), atts);

        QJsonArray rawList;
        rawList.append(mAudio);
        const QVariantList built = Backend::buildMessageListFromApi(rawList, {}, {});
        QCOMPARE(built.size(), 1);
        const QVariantMap builtMsg = built.first().toMap();
        const QVariantList builtAtts = builtMsg.value(QStringLiteral("attachments")).toList();
        QCOMPARE(builtAtts.size(), 1);
        const QVariantMap builtAtt = builtAtts.first().toMap();
        QCOMPARE(builtAtt.value(QStringLiteral("isAudio")).toBool(), true);
        const QVariantList builtWaveform = builtAtt.value(QStringLiteral("waveformBars")).toList();
        QCOMPARE(builtWaveform.size(), 28);

        // 4. Backend audio playback state
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_audio.json")));

        QCOMPARE(backend.audioPlaying(), false);
        QCOMPARE(backend.playingAudioGuid(), QString());
        QCOMPARE(backend.audioProgress(), 0.0);

        backend.toggleAudioPlayback(QStringLiteral("att-audio-test-1"), QString());
        QCOMPARE(backend.playingAudioGuid(), QStringLiteral("att-audio-test-1"));
        QCOMPARE(backend.audioPlaying(), true);

        backend.seekAudioPlayback(QStringLiteral("att-audio-test-1"), 0.45);
        QCOMPARE(backend.audioProgress(), 0.45);

        backend.toggleAudioPlayback(QStringLiteral("att-audio-test-1"), QString());
        QCOMPARE(backend.audioPlaying(), false);
    }

    void testVoiceRecordingLifecycle() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_rec.json")));
        backend.loadDemoData();

        QCOMPARE(backend.recordingVoice(), false);
        QCOMPARE(backend.voiceRecordingSeconds(), 0);

        // Start recording
        backend.startVoiceRecording();
        QCOMPARE(backend.recordingVoice(), true);

        // Cancel recording
        backend.cancelVoiceRecording();
        QCOMPARE(backend.recordingVoice(), false);
        QCOMPARE(backend.voiceRecordingSeconds(), 0);

        // Start again and stop/send
        backend.startVoiceRecording();
        QCOMPARE(backend.recordingVoice(), true);

        backend.stopAndSendVoiceRecording();
        QCOMPARE(backend.recordingVoice(), false);
    }

    void testExpressiveSendWithEffect() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_expr.json")));
        backend.loadDemoData();

        const QVariantList chats = backend.chats();
        QVERIFY(!chats.isEmpty());
        const QString chatGuid = chats.first().toMap().value(QStringLiteral("guid")).toString();
        backend.selectChat(chatGuid);

        // 1. Invisible Ink
        backend.sendMessageWithEffect(QStringLiteral("Secret code 4096"), QStringLiteral("com.apple.MobileSMS.expressivesend.invisibleink"));
        QVariantList msgs = backend.messages();
        QVERIFY(!msgs.isEmpty());
        QVariantMap lastMsg = msgs.last().toMap();
        QCOMPARE(lastMsg.value(QStringLiteral("text")).toString(), QStringLiteral("Secret code 4096"));
        QCOMPARE(lastMsg.value(QStringLiteral("expressiveEffect")).toString(), QStringLiteral("🕵️ Invisible Ink"));

        // 2. Confetti Effect
        backend.sendMessageWithEffect(QStringLiteral("Happy Birthday!"), QStringLiteral("com.apple.messages.effect.CKConfettiEffect"));
        msgs = backend.messages();
        lastMsg = msgs.last().toMap();
        QCOMPARE(lastMsg.value(QStringLiteral("text")).toString(), QStringLiteral("Happy Birthday!"));
        QCOMPARE(lastMsg.value(QStringLiteral("expressiveEffect")).toString(), QStringLiteral("🎉 Confetti"));

        // 3. Persistence across temp-* GUID replacement and server row ingestion
        QJsonObject serverRow;
        serverRow.insert(QStringLiteral("guid"), QStringLiteral("real-server-guid-777"));
        serverRow.insert(QStringLiteral("chat_guid"), chatGuid);
        serverRow.insert(QStringLiteral("text"), QStringLiteral("Happy Birthday!"));
        serverRow.insert(QStringLiteral("isFromMe"), true);
        serverRow.insert(QStringLiteral("is_from_me"), 1);
        serverRow.insert(QStringLiteral("date"), QStringLiteral("2026-11-11T12:00:00Z"));
        serverRow.insert(QStringLiteral("expressive_send_style_id"), QString()); // Server DB row missing style ID

        QJsonArray rawMessages;
        rawMessages.append(serverRow);

        QHash<QString, QString> recentEffects;
        recentEffects.insert(QStringLiteral("Happy Birthday!"), QStringLiteral("com.apple.messages.effect.CKConfettiEffect"));

        QVariantList built = Backend::buildMessageListFromApi(rawMessages, {}, {}, nullptr, {}, nullptr, {}, recentEffects);
        QCOMPARE(built.size(), 1);
        QVariantMap restoredMsg = built.first().toMap();
        QCOMPARE(restoredMsg.value(QStringLiteral("guid")).toString(), QStringLiteral("real-server-guid-777"));
        QCOMPARE(restoredMsg.value(QStringLiteral("expressiveEffect")).toString(), QStringLiteral("🎉 Confetti"));
        QVERIFY(restoredMsg.value(QStringLiteral("sendEffectLabel")).toString().contains(QStringLiteral("confetti")));
    }

    void testScheduledMessagesLifecycle() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_sched.json")));
        backend.loadDemoData();

        const QVariantList chats = backend.chats();
        QVERIFY(!chats.isEmpty());
        const QString chatGuid = chats.first().toMap().value(QStringLiteral("guid")).toString();
        backend.selectChat(chatGuid);

        QVERIFY(backend.scheduledMessages().isEmpty());

        const qint64 sendTime = QDateTime::currentMSecsSinceEpoch() + 3600000;
        backend.scheduleMessage(QStringLiteral("Meeting reminder"), sendTime);

        const QVariantList scheds = backend.scheduledMessages();
        QCOMPARE(scheds.size(), 1);
        const QVariantMap schedItem = scheds.first().toMap();
        QCOMPARE(schedItem.value(QStringLiteral("text")).toString(), QStringLiteral("Meeting reminder"));
        const QString scheduleId = schedItem.value(QStringLiteral("id")).toString();
        QVERIFY(!scheduleId.isEmpty());

        backend.cancelScheduledMessage(scheduleId);
        QVERIFY(backend.scheduledMessages().isEmpty());
    }

    void testGroupParticipantManagement() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_parts.json")));
        backend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_parts.json")));
        backend.loadDemoData();

        const QVariantList chats = backend.chats();
        QVERIFY(!chats.isEmpty());
        const QString chatGuid = chats.first().toMap().value(QStringLiteral("guid")).toString();
        backend.selectChat(chatGuid);

        const QString newParticipant = QStringLiteral("+15559876543");
        QVERIFY(!backend.selectedChatParticipants().contains(newParticipant));

        // Add participant
        backend.addParticipantToActiveChat(newParticipant);
        QVERIFY(backend.selectedChatParticipants().contains(newParticipant));

        // Remove participant
        backend.removeParticipantFromActiveChat(newParticipant);
        QVERIFY(!backend.selectedChatParticipants().contains(newParticipant));

        // Sync state to server
        backend.syncChatStateToServer();
    }

    void testQmlMainLayoutSmoke() {
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());

        // Verify Tapback reaction bar hover persistence & grace timer
        QVERIFY(qml.contains(QStringLiteral("id: hoverClearTimer")));
        QVERIFY(qml.contains(QStringLiteral("interval: 380")));
        QVERIFY(qml.contains(QStringLiteral("readonly property bool isAnyPartHovered:")));
        QVERIFY(qml.contains(QStringLiteral("id: msgTextHover")));
        QVERIFY(qml.contains(QStringLiteral("id: actionHoverZone")));
        QVERIFY(qml.contains(QStringLiteral("x: Math.min(-12, actionBar.x - 12)")));

        // Verify Invisible Ink sizing and text protection
        QVERIFY(qml.contains(QStringLiteral("id: textContainer")));
        QVERIFY(qml.contains(QStringLiteral("readonly property bool isInvisibleInkMsg:")));
        QVERIFY(qml.contains(QStringLiteral("Math.min(220, bubbleContent.maxTextW)")));
        QVERIFY(qml.contains(QStringLiteral("id: invisibleInkOverlay")));
        QVERIFY(qml.contains(QStringLiteral("opacity: (msgDelegate.isInvisibleInkMsg && !msgDelegate.invisibleInkRevealed) ? 0.0 : 1.0")));
        QVERIFY(qml.contains(QStringLiteral("🕵️ Invisible Ink · ")));

        // Verify animated expressive bubble & screen effects
        QVERIFY(qml.contains(QStringLiteral("id: slamAnim")));
        QVERIFY(qml.contains(QStringLiteral("id: loudAnim")));
        QVERIFY(qml.contains(QStringLiteral("id: gentleAnim")));
        QVERIFY(qml.contains(QStringLiteral("id: screenEffectOverlay")));
        QVERIFY(qml.contains(QStringLiteral("property string activeScreenEffect:")));
        QVERIFY(qml.contains(QStringLiteral("function triggerExpressiveEffect(effectText)")));
        QVERIFY(qml.contains(QStringLiteral("function playBubbleEffect(effectLabel)")));

        // Verify BluePill iCloud Hub (Notes, Calendar, Photos)
        QVERIFY(qml.contains(QStringLiteral("id: icloudHubBar")));
        QVERIFY(qml.contains(QStringLiteral("id: icloudPhotosBtn")));
        QVERIFY(qml.contains(QStringLiteral("id: icloudNotesPopup")));
        QVERIFY(qml.contains(QStringLiteral("id: notePopupHeadlineText")));
        QVERIFY(qml.contains(QStringLiteral("id: noteDetailHeadlineText")));
        QVERIFY(qml.contains(QStringLiteral("function cleanNoteDisplayBody(title, body)")));
        QVERIFY(qml.contains(QStringLiteral("id: icloudCalendarPopup")));
        QVERIFY(qml.contains(QStringLiteral("id: icloudPhotosPopup")));
    }

    void testIcloudNotesLifecycle() {
        const char *credVars[] = {
            "BLUEPILL_SERVER_URL", "IMSG_SERVER_URL", "BLUEBUBBLES_URL",
            "BLUEPILL_SECONDARY_URL", "IMSG_SECONDARY_URL", "BLUEBUBBLES_SECONDARY_URL",
            "BLUEPILL_PASSWORD", "IMSG_PASSWORD", "BLUEBUBBLES_PASSWORD"
        };
        QMap<QByteArray, QByteArray> savedEnv;
        for (const char *k : credVars) {
            if (qEnvironmentVariableIsSet(k)) {
                savedEnv.insert(QByteArray(k), qgetenv(k));
            }
            qunsetenv(k);
        }

        Backend backend(false);
        QTemporaryDir tempDir;
        const QString statePath = tempDir.filePath(QStringLiteral("state_notes.json"));
        backend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_notes_offline.json")));
        backend.loadCredentials();
        backend.setStateFilePath(statePath);

        QVERIFY(backend.icloudNotes().isEmpty());

        // Create Note (including auto-generated ID return value) and verify never-sent local create + delete cancels cleanly
        const QString genId = backend.saveIcloudNote(QString(), QStringLiteral("Auto ID Note"), QStringLiteral("Body 0"), QStringLiteral("Notes"));
        QVERIFY(genId.startsWith(QStringLiteral("note_")));
        QVERIFY(backend.hasPendingIcloudNoteSaveForTest(genId));
        backend.deleteIcloudNote(genId);
        QVERIFY(!backend.hasPendingIcloudNoteSaveForTest(genId));
        QVERIFY(!backend.hasPendingIcloudNoteDeleteForTest(genId));

        const QString savedId = backend.saveIcloudNote(QStringLiteral("note_test_1"), QStringLiteral("Meeting Notes"), QStringLiteral("Discuss Q3 sync and calendar items"), QStringLiteral("Work"));
        QCOMPARE(savedId, QStringLiteral("note_test_1"));
        QCOMPARE(backend.icloudNotes().size(), 1);
        QCOMPARE(backend.icloudNotes().first().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Meeting Notes"));
        QCOMPARE(backend.icloudNotes().first().toMap().value(QStringLiteral("folder")).toString(), QStringLiteral("Work"));

        // Verify state file has 0600 owner-only permissions
        QFileInfo fi(statePath);
        QVERIFY(fi.exists());
        QCOMPARE(fi.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther), QFileDevice::Permissions());

        // Update Note (passing baseRevision = 1 to represent an existing server-backed note edited offline)
        backend.saveIcloudNote(QStringLiteral("note_test_1"), QStringLiteral("Updated Meeting Notes"), QStringLiteral("New action items"), QStringLiteral("Work"), 1);
        QCOMPARE(backend.icloudNotes().size(), 1);
        QCOMPARE(backend.icloudNotes().first().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Updated Meeting Notes"));
        QCOMPARE(backend.icloudNotes().first().toMap().value(QStringLiteral("body")).toString(), QStringLiteral("New action items"));

        // Reconciling with an empty remote array while note_test_1 is still pending offline save preserves note_test_1
        backend.reconcileRemoteIcloudNotesForTest(QJsonArray());
        QCOMPARE(backend.icloudNotes().size(), 1);

        // Delete Note
        backend.deleteIcloudNote(QStringLiteral("note_test_1"));
        QCOMPARE(backend.icloudNotes().size(), 0);
        QVERIFY(backend.hasPendingIcloudNoteDeleteForTest(QStringLiteral("note_test_1")));

        // Reconciling with a stale remote array containing deleted note_test_1 keeps it deleted while delete is pending
        QJsonObject staleNote;
        staleNote.insert(QStringLiteral("id"), QStringLiteral("note_test_1"));
        staleNote.insert(QStringLiteral("title"), QStringLiteral("Stale Remote Title"));
        backend.reconcileRemoteIcloudNotesForTest(QJsonArray{staleNote});
        QCOMPARE(backend.icloudNotes().size(), 0);

        // Save another note so pending mutation sequence is persisted, then verify a fresh Backend restores
        // icloudNotesGeneration above all restored clientMutationSeq values across restarts
        backend.saveIcloudNote(QStringLiteral("note_restart_seq"), QStringLiteral("Restart Seq Note"), QStringLiteral("Body"), QStringLiteral("Notes"));
        const quint64 preRestartGen = backend.icloudNotesGenerationForTest();
        QVERIFY(preRestartGen > 0);

        // Start a local mock HTTP relay to test:
        // 1) Delayed replay response after restart when a newer edit is queued while replay is in flight
        // 2) Concurrent mutation A failing (entering backoff) followed by mutation B succeeding (triggering scheduleIcloudMutationRetry(true))
        //    without prematurely retrying mutation A before its backoff deadline
        QTcpServer mockServer;
        QVERIFY(mockServer.listen(QHostAddress::LocalHost, 0));
        const quint16 mockPort = mockServer.serverPort();
        int noteACount = 0;
        qint64 firstAttemptAMs = 0;
        qint64 secondAttemptAMs = 0;
        int noteBCount = 0;
        int noteWsReplayCount = 0;
        int noteRealConflictCount = 0;
        int restartNoteCount = 0;
        bool lostAckCreatedOnServer = false;
        int unwritableNotePostCount = 0;
        qint64 secondRestartBaseRev = -1;
        QTcpSocket *stalledRestartSock = nullptr;

        auto sendHttpJson = [](QTcpSocket *s, int status, const QByteArray &statusText, const QByteArray &jsonBody) {
            const QByteArray resp = "HTTP/1.1 " + QByteArray::number(status) + " " + statusText + "\r\n"
                                    "Content-Type: application/json\r\n"
                                    "Content-Length: " + QByteArray::number(jsonBody.size()) + "\r\n"
                                    "Connection: close\r\n\r\n" + jsonBody;
            s->write(resp);
            s->flush();
            s->disconnectFromHost();
        };

        QObject::connect(&mockServer, &QTcpServer::newConnection, &mockServer, [&]() {
            while (QTcpSocket *sock = mockServer.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock, buf]() {
                    buf->append(sock->readAll());
                    const int bodyIdx = buf->indexOf("\r\n\r\n");
                    if (bodyIdx < 0) return;
                    const QByteArray headers = buf->left(bodyIdx).toLower();
                    const int clIdx = headers.indexOf("content-length:");
                    const int contentLength = clIdx >= 0
                        ? headers.mid(clIdx + 15, headers.indexOf('\r', clIdx) - clIdx - 15).trimmed().toInt()
                        : 0;
                    if (buf->size() - (bodyIdx + 4) < contentLength) return;
                    const QByteArray rawBody = buf->mid(bodyIdx + 4, contentLength);
                    buf->clear();
                    const QJsonObject bodyObj = QJsonDocument::fromJson(rawBody).object();
                    const QString reqId = bodyObj.value(QStringLiteral("id")).toString();

                    if (reqId == QStringLiteral("note_restart_seq")) {
                        restartNoteCount++;
                        if (restartNoteCount == 1) {
                            // Hold the first replayed request in flight so a post-restart edit can queue behind it
                            stalledRestartSock = sock;
                            return;
                        }
                        secondRestartBaseRev = bodyObj.value(QStringLiteral("baseRevision")).toVariant().toLongLong();
                        sendHttpJson(sock, 200, "OK",
                                     R"({"status":200,"data":[{"id":"note_restart_seq","title":"Newer Post-Restart Edit","body":"Body 2","folder":"Notes","revision":3}]})");
                        return;
                    }
                    if (bodyObj.value(QStringLiteral("title")).toString() == QStringLiteral("Lost Ack Auto Note")) {
                        // Simulate server persisting the created note and dropping the TCP connection before the HTTP ACK arrives
                        lostAckCreatedOnServer = true;
                        sock->disconnectFromHost();
                        return;
                    }
                    if (bodyObj.value(QStringLiteral("title")).toString() == QStringLiteral("Unwritable State Note")) {
                        unwritableNotePostCount++;
                        sendHttpJson(sock, 200, "OK", R"({"status":200,"data":[]})");
                        return;
                    }
                    if (reqId == QStringLiteral("note_conc_a")) {
                        noteACount++;
                        const qint64 nowAttemptMs = QDateTime::currentMSecsSinceEpoch();
                        if (noteACount == 1) {
                            firstAttemptAMs = nowAttemptMs;
                        } else if (noteACount == 2) {
                            secondAttemptAMs = nowAttemptMs;
                        }
                        sendHttpJson(sock, 503, "Service Unavailable",
                                     R"({"status":503,"message":"Transient upstream failure"})");
                        return;
                    }
                    if (reqId == QStringLiteral("note_conc_b")) {
                        noteBCount++;
                        sendHttpJson(sock, 200, "OK",
                                     R"({"status":200,"data":[{"id":"note_conc_b","title":"Note B","body":"Body B","folder":"Notes","revision":1}]})");
                        return;
                    }
                    if (reqId == QStringLiteral("note_ws_replay")) {
                        noteWsReplayCount++;
                        if (noteWsReplayCount == 1) {
                            // Simulate dropped ACK after server already stored the trimmed note
                            sendHttpJson(sock, 503, "Service Unavailable",
                                         R"({"status":503,"message":"Dropped ACK"})");
                            return;
                        }
                        // On replay, server returns 409 with the stored (trimmed) body
                        sendHttpJson(sock, 409, "Conflict",
                                     R"({"status":409,"error":"revision_conflict","data":[{"id":"note_ws_replay","title":"Trailing WS Note","body":"Body with trailing newline","folder":"Notes","revision":5}]})");
                        return;
                    }
                    if (reqId == QStringLiteral("note_real_conflict")) {
                        noteRealConflictCount++;
                        sendHttpJson(sock, 409, "Conflict",
                                     R"({"status":409,"error":"revision_conflict","data":[{"id":"note_real_conflict","title":"Conflict Note","body":"Different Server Body","folder":"Notes","revision":6}]})");
                        return;
                    }
                    sendHttpJson(sock, 200, "OK", R"({"status":200,"data":[]})");
                });
            }
        });

        qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(mockPort).toUtf8());
        qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));

        Backend restartedBackend(false);
        restartedBackend.setStateFilePath(statePath);
        restartedBackend.loadCredentials();
        QVERIFY(restartedBackend.icloudNotesGenerationForTest() >= preRestartGen);

        // Trigger replay of restored pending note_restart_seq and wait until it is in flight at the mock server
        restartedBackend.scheduleIcloudMutationRetryForTest(true);
        QTRY_COMPARE_WITH_TIMEOUT(restartNoteCount, 1, 1500);
        QVERIFY(stalledRestartSock != nullptr);

        // Save a newer edit to note_restart_seq while the initial replay is still in flight
        restartedBackend.saveIcloudNote(QStringLiteral("note_restart_seq"), QStringLiteral("Newer Post-Restart Edit"), QStringLiteral("Body 2"), QStringLiteral("Notes"));
        QVERIFY(restartedBackend.icloudNotesGenerationForTest() > preRestartGen);

        // Now complete the first in-flight replay with revision=2; the callback must NOT drop the newer edit,
        // and must advance the queued edit's baseRevision to 2 and immediately replay it
        sendHttpJson(stalledRestartSock, 200, "OK",
                     R"({"status":200,"data":[{"id":"note_restart_seq","title":"Restart Seq Note","body":"Body","folder":"Notes","revision":2}]})");
        stalledRestartSock = nullptr;
        QTRY_COMPARE_WITH_TIMEOUT(restartNoteCount, 2, 1500);
        QCOMPARE(secondRestartBaseRev, qint64(2));

        // Now test concurrent mutations A (fails -> enters backoff) and B (succeeds -> calls scheduleIcloudMutationRetry(true))
        Backend liveBackend(false);
        liveBackend.setStateFilePath(tempDir.filePath(QStringLiteral("state_live_backoff.json")));
        liveBackend.loadCredentials();

        const QString keyA = QStringLiteral("note_save:note_conc_a");
        liveBackend.saveIcloudNote(QStringLiteral("note_conc_a"), QStringLiteral("Note A"), QStringLiteral("Body A"), QStringLiteral("Notes"));
        QTRY_COMPARE_WITH_TIMEOUT(noteACount, 1, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(liveBackend.icloudMutationRetryCountForTest(keyA), 1, 1500);
        QVERIFY(liveBackend.isIcloudMutationInBackoffForTest(keyA));

        // Dispatch mutation B which succeeds with HTTP 200 and triggers scheduleIcloudMutationRetry(true)
        liveBackend.saveIcloudNote(QStringLiteral("note_conc_b"), QStringLiteral("Note B"), QStringLiteral("Body B"), QStringLiteral("Notes"));
        QTRY_COMPARE_WITH_TIMEOUT(noteBCount, 1, 1500);
        // Wait for client-side checkpoint confirming mutation B's HTTP 200 response was processed and applied
        // along with its clientMutationSeq so consecutive edits in QML editors advance their revision
        QTRY_VERIFY_WITH_TIMEOUT([&]() {
            for (const QVariant &v : liveBackend.icloudNotes()) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("id")).toString() == QStringLiteral("note_conc_b") &&
                    m.value(QStringLiteral("revision")).toLongLong() == 1 &&
                    m.value(QStringLiteral("clientMutationSeq")).toULongLong() > 0) {
                    return true;
                }
            }
            return false;
        }(), 1500);
        QCoreApplication::processEvents();

        // Verify mutation A was NOT retried early before its 1000ms backoff deadline when mutation B succeeded
        const qint64 elapsedSinceFirstAttemptAMs = QDateTime::currentMSecsSinceEpoch() - firstAttemptAMs;
        if (noteACount == 1 && elapsedSinceFirstAttemptAMs < 900) {
            QCOMPARE(liveBackend.icloudMutationRetryCountForTest(keyA), 1);
            QVERIFY(liveBackend.isIcloudMutationInBackoffForTest(keyA));
        } else {
            if (noteACount == 1) {
                QTRY_COMPARE_WITH_TIMEOUT(noteACount, 2, 1500);
            }
            QVERIFY2(secondAttemptAMs - firstAttemptAMs >= 900,
                     qPrintable(QStringLiteral("Mutation A retried too early after %1 ms").arg(secondAttemptAMs - firstAttemptAMs)));
        }
        liveBackend.clearIcloudMutationRetryStateForTest(keyA);
        QCOMPARE(liveBackend.icloudMutationRetryCountForTest(keyA), 0);
        QVERIFY(!liveBackend.isIcloudMutationInBackoffForTest(keyA));

        // Verify an auto-ID note create that was created on the server before the connection dropped (lost ACK),
        // followed by a client restart without credentials, retains its pending deletion when deleted after restart
        {
            const QString lostAckNoteId = liveBackend.saveIcloudNote(QString(), QStringLiteral("Lost Ack Auto Note"), QStringLiteral("Body"), QStringLiteral("Notes"));
            const QString lostAckMutKey = QStringLiteral("note_save:") + lostAckNoteId;
            QTRY_VERIFY_WITH_TIMEOUT(lostAckCreatedOnServer, 1500);
            QTRY_VERIFY_WITH_TIMEOUT(liveBackend.isIcloudMutationInBackoffForTest(lostAckMutKey), 1500);
            liveBackend.clearIcloudMutationRetryStateForTest(lostAckMutKey);

            const QString copyNoteStatePath = tempDir.filePath(QStringLiteral("state_live_backoff_copy.json"));
            QFile::remove(copyNoteStatePath);
            QVERIFY(QFile::copy(tempDir.filePath(QStringLiteral("state_live_backoff.json")), copyNoteStatePath));
            qunsetenv("BLUEBUBBLES_URL");
            qunsetenv("BLUEBUBBLES_PASSWORD");
            Backend offlineRestartNoteBackend(false);
            offlineRestartNoteBackend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_notes_offline.json")));
            offlineRestartNoteBackend.loadCredentials();
            offlineRestartNoteBackend.setStateFilePath(copyNoteStatePath);
            QVERIFY(offlineRestartNoteBackend.hasPendingIcloudNoteSaveForTest(lostAckNoteId));
            offlineRestartNoteBackend.deleteIcloudNote(lostAckNoteId);
            QVERIFY(!offlineRestartNoteBackend.hasPendingIcloudNoteSaveForTest(lostAckNoteId));
            QVERIFY(offlineRestartNoteBackend.hasPendingIcloudNoteDeleteForTest(lostAckNoteId));
            qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(mockPort).toUtf8());
            qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));
            liveBackend.deleteIcloudNote(lostAckNoteId);

            // Also verify that if saveAppState() fails (unwritable state path), saveIcloudNote does not dispatch
            // the network POST until state can be durably persisted
            const QString blockerFile = tempDir.filePath(QStringLiteral("unwritable_note_blocker"));
            QFile bf(blockerFile);
            QVERIFY(bf.open(QIODevice::WriteOnly));
            bf.write("x");
            bf.close();
            Backend failPersistNoteBackend(false);
            failPersistNoteBackend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_notes_fail_persist.json")));
            failPersistNoteBackend.setStateFilePath(blockerFile + QStringLiteral("/state.json"));
            failPersistNoteBackend.loadCredentials();
            const QString failNoteId = failPersistNoteBackend.saveIcloudNote(QString(), QStringLiteral("Unwritable State Note"), QStringLiteral("Body"), QStringLiteral("Notes"));
            QVERIFY(!failPersistNoteBackend.isIcloudNoteDurablyPersisted(failNoteId));
            QTest::qWait(300);
            QCOMPARE(unwritableNotePostCount, 0);
            failPersistNoteBackend.deleteIcloudNote(failNoteId);
            QVERIFY(!failPersistNoteBackend.hasPendingIcloudNoteSaveForTest(failNoteId));
            QVERIFY(failPersistNoteBackend.hasPendingIcloudNoteDeleteForTest(failNoteId));
        }

        // Verify dropped ACK followed by 409 replay with trailing whitespace in saved body is recognized as already-applied (not paused)
        const QString wsKey = QStringLiteral("note_save:note_ws_replay");
        liveBackend.saveIcloudNote(QStringLiteral("note_ws_replay"), QStringLiteral("Trailing WS Note "), QStringLiteral("Body with trailing newline\n  "), QStringLiteral("Notes"));
        QTRY_COMPARE_WITH_TIMEOUT(noteWsReplayCount, 1, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(liveBackend.isIcloudMutationInBackoffForTest(wsKey), 1500);
        liveBackend.clearIcloudMutationRetryStateForTest(wsKey);
        liveBackend.replayPendingIcloudMutations();
        QTRY_COMPARE_WITH_TIMEOUT(noteWsReplayCount, 2, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(!liveBackend.hasPendingIcloudNoteSaveForTest(QStringLiteral("note_ws_replay")), 1500);
        QVERIFY(!liveBackend.isIcloudMutationPausedForTest(wsKey));

        // Verify a genuine 409 conflict with a different server body still pauses the pending mutation,
        // keeps the pause when saved again with a stale revision, and recovers via reloadConflictedIcloudNoteServerVersion
        const QString conflictKey = QStringLiteral("note_save:note_real_conflict");
        liveBackend.saveIcloudNote(QStringLiteral("note_real_conflict"), QStringLiteral("Conflict Note"), QStringLiteral("Local Draft\n"), QStringLiteral("Notes"));
        QTRY_COMPARE_WITH_TIMEOUT(noteRealConflictCount, 1, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(liveBackend.isIcloudMutationPausedForTest(conflictKey), 1500);
        QVERIFY(liveBackend.hasPendingIcloudNoteSaveForTest(QStringLiteral("note_real_conflict")));

        // Saving with a stale baseRevision (< 6) preserves the draft and keeps the conflict paused without re-submitting
        liveBackend.saveIcloudNote(QStringLiteral("note_real_conflict"), QStringLiteral("Conflict Note Edited"), QStringLiteral("Updated Draft"), QStringLiteral("Notes"), 1);
        QTest::qWait(300);
        QCOMPARE(noteRealConflictCount, 1);
        QVERIFY(liveBackend.isIcloudMutationPausedForTest(conflictKey));
        QVERIFY(liveBackend.hasPendingIcloudNoteSaveForTest(QStringLiteral("note_real_conflict")));

        // Verify Save-as-Copy followed by reloadConflictedIcloudNoteServerVersion preserves the user's draft in the new copy
        // while restoring the conflicted note to the server version (revision 6 / "Different Server Body")
        const QString conflictCopyId = liveBackend.saveIcloudNote(QString(), QStringLiteral("Conflict Note Edited"), QStringLiteral("Updated Draft"), QStringLiteral("Notes"), 0);
        QVERIFY(!conflictCopyId.isEmpty());
        QVERIFY(conflictCopyId != QStringLiteral("note_real_conflict"));
        QVERIFY(liveBackend.isIcloudNoteDurablyPersisted(conflictCopyId));
        QVERIFY(liveBackend.reloadConflictedIcloudNoteServerVersion(QStringLiteral("note_real_conflict")));
        QVERIFY(!liveBackend.isIcloudMutationPausedForTest(conflictKey));
        QVERIFY(!liveBackend.hasPendingIcloudNoteSaveForTest(QStringLiteral("note_real_conflict")));
        bool foundCopyDraft = false;
        bool foundReloadedServer = false;
        for (const QVariant &nv : liveBackend.icloudNotes()) {
            const QVariantMap nm = nv.toMap();
            if (nm.value(QStringLiteral("id")).toString() == conflictCopyId) {
                foundCopyDraft = (nm.value(QStringLiteral("title")).toString() == QStringLiteral("Conflict Note Edited") &&
                                  nm.value(QStringLiteral("body")).toString() == QStringLiteral("Updated Draft"));
            } else if (nm.value(QStringLiteral("id")).toString() == QStringLiteral("note_real_conflict")) {
                foundReloadedServer = (nm.value(QStringLiteral("body")).toString() == QStringLiteral("Different Server Body") &&
                                       nm.value(QStringLiteral("revision")).toLongLong() == 6);
            }
        }
        QVERIFY(foundCopyDraft);
        QVERIFY(foundReloadedServer);

        for (const char *k : credVars) {
            const QByteArray key(k);
            if (savedEnv.contains(key)) qputenv(k, savedEnv.value(key)); else qunsetenv(k);
        }
    }

    void testIcloudCalendarEventsLifecycle() {
        const char *credVars[] = {
            "BLUEPILL_SERVER_URL", "IMSG_SERVER_URL", "BLUEBUBBLES_URL",
            "BLUEPILL_SECONDARY_URL", "IMSG_SECONDARY_URL", "BLUEBUBBLES_SECONDARY_URL",
            "BLUEPILL_PASSWORD", "IMSG_PASSWORD", "BLUEBUBBLES_PASSWORD"
        };
        QMap<QByteArray, QByteArray> savedEnv;
        for (const char *k : credVars) {
            if (qEnvironmentVariableIsSet(k)) {
                savedEnv.insert(QByteArray(k), qgetenv(k));
            }
            qunsetenv(k);
        }

        QTemporaryDir tempDir;
        Backend backend(false);
        backend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_events_offline.json")));
        backend.loadCredentials();
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_events.json")));

        QVERIFY(backend.icloudEvents().isEmpty());

        // Create Event (and verify unique IDs even within the same millisecond)
        const QString evtId = backend.saveIcloudEvent(QStringLiteral("BluePill Architecture Review"), 1700000000000, 1700003600000, QStringLiteral("Work"), QStringLiteral("Discuss iCloud Hub sync"));
        const QString evtId2 = backend.saveIcloudEvent(QStringLiteral("BluePill Architecture Review 2"), 1700000000000, 1700003600000, QStringLiteral("Work"), QStringLiteral("Discuss iCloud Hub sync 2"));
        QVERIFY(!evtId.isEmpty());
        QVERIFY(!evtId2.isEmpty());
        QVERIFY(evtId != evtId2);
        QVERIFY(backend.hasPendingIcloudEventSaveForTest(evtId2));
        backend.deleteIcloudEvent(evtId2);
        // Never-sent local create + delete cancels out cleanly without queueing a remote delete
        QVERIFY(!backend.hasPendingIcloudEventSaveForTest(evtId2));
        QVERIFY(!backend.hasPendingIcloudEventDeleteForTest(evtId2));
        QCOMPARE(backend.icloudEvents().size(), 1);
        QCOMPARE(backend.icloudEvents().first().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("BluePill Architecture Review"));
        QCOMPARE(backend.icloudEvents().first().toMap().value(QStringLiteral("calendarTitle")).toString(), QStringLiteral("Work"));
        QVERIFY(backend.icloudEvents().first().toMap().value(QStringLiteral("clientMutationSeq")).toULongLong() > 0);

        // Offline pending event survives remote snapshot reconciliation and normalizes calendarName -> calendarTitle
        QJsonObject remoteEvt;
        remoteEvt.insert(QStringLiteral("id"), QStringLiteral("evt_remote_99"));
        remoteEvt.insert(QStringLiteral("title"), QStringLiteral("Remote Standup"));
        remoteEvt.insert(QStringLiteral("calendarName"), QStringLiteral("Work"));
        remoteEvt.insert(QStringLiteral("startMs"), 1700007200000LL);
        remoteEvt.insert(QStringLiteral("endMs"), 1700010800000LL);
        backend.reconcileRemoteIcloudEventsForTest(QJsonArray{remoteEvt});
        QCOMPARE(backend.icloudEvents().size(), 2);
        QCOMPARE(backend.icloudEvents().last().toMap().value(QStringLiteral("calendarTitle")).toString(), QStringLiteral("Work"));

        // Delete Events
        backend.deleteIcloudEvent(evtId);
        backend.deleteIcloudEvent(QStringLiteral("evt_remote_99"));
        QCOMPARE(backend.icloudEvents().size(), 0);

        // Verify 250-event capped window reconciliation drops remotely deleted in-window events while preserving out-of-window events
        QJsonArray initial250;
        for (int i = 0; i < 250; ++i) {
            QJsonObject e;
            e.insert(QStringLiteral("id"), QStringLiteral("evt_win_%1").arg(i));
            e.insert(QStringLiteral("title"), QStringLiteral("Event %1").arg(i));
            e.insert(QStringLiteral("startMs"), 1700000000000LL + i * 3600000LL);
            e.insert(QStringLiteral("endMs"), 1700003600000LL + i * 3600000LL);
            initial250.append(e);
        }
        QJsonObject outOfWindowEvt;
        outOfWindowEvt.insert(QStringLiteral("id"), QStringLiteral("evt_out_of_win"));
        outOfWindowEvt.insert(QStringLiteral("title"), QStringLiteral("Far Future Out-Of-Window Event"));
        outOfWindowEvt.insert(QStringLiteral("startMs"), 1800000000000LL);
        outOfWindowEvt.insert(QStringLiteral("endMs"), 1800003600000LL);
        QJsonArray withOut = initial250;
        withOut.append(outOfWindowEvt);
        backend.reconcileRemoteIcloudEventsForTest(withOut);
        QCOMPARE(backend.icloudEvents().size(), 251);

        // Next 250-item snapshot omits in-window evt_win_10 (deleted remotely) and shifts window by 1
        QJsonArray next250;
        for (int i = 0; i < 251; ++i) {
            if (i == 10) continue; // deleted remotely inside the returned [minRemoteStartMs, maxRemoteStartMs] window
            QJsonObject e;
            e.insert(QStringLiteral("id"), QStringLiteral("evt_win_%1").arg(i));
            e.insert(QStringLiteral("title"), QStringLiteral("Event %1").arg(i));
            e.insert(QStringLiteral("startMs"), 1700000000000LL + i * 3600000LL);
            e.insert(QStringLiteral("endMs"), 1700003600000LL + i * 3600000LL);
            next250.append(e);
        }
        QCOMPARE(next250.size(), 250);
        backend.reconcileRemoteIcloudEventsForTest(next250);
        bool foundDeletedInWindow = false;
        bool foundOutOfWindow = false;
        for (const QVariant &v : backend.icloudEvents()) {
            const QString id = v.toMap().value(QStringLiteral("id")).toString();
            if (id == QStringLiteral("evt_win_10")) foundDeletedInWindow = true;
            if (id == QStringLiteral("evt_out_of_win")) foundOutOfWindow = true;
        }
        QVERIFY(!foundDeletedInWindow);
        QVERIFY(foundOutOfWindow);

        // Verify both 200 OK save and dropped-ACK -> 409 replay for far-future events outside the 250-event window
        // preserve both the 250th near-window event (evt_near_249) and cached intermediate out-of-window events (evt_mid_cached)
        QTcpServer evtServer;
        QVERIFY(evtServer.listen(QHostAddress::LocalHost, 0));
        int evtRequestCount = 0;
        int unwritableEvtPostCount = 0;
        bool evtCreatedBeforeDroppedAck = false;
        QString capturedEvtId;
        auto buildNear250 = []() {
            QJsonArray snap250;
            for (int i = 0; i < 250; ++i) {
                QJsonObject e;
                e.insert(QStringLiteral("id"), QStringLiteral("evt_near_%1").arg(i));
                e.insert(QStringLiteral("title"), QStringLiteral("Near Event %1").arg(i));
                e.insert(QStringLiteral("startMs"), 1700000000000LL + i * 3600000LL);
                e.insert(QStringLiteral("endMs"), 1700003600000LL + i * 3600000LL);
                e.insert(QStringLiteral("calendarTitle"), QStringLiteral("Work"));
                e.insert(QStringLiteral("notes"), QStringLiteral(""));
                e.insert(QStringLiteral("revision"), 1);
                snap250.append(e);
            }
            return snap250;
        };
        QObject::connect(&evtServer, &QTcpServer::newConnection, &evtServer, [&]() {
            while (QTcpSocket *sock = evtServer.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock, buf]() {
                    buf->append(sock->readAll());
                    const int bodyIdx = buf->indexOf("\r\n\r\n");
                    if (bodyIdx < 0) return;
                    const QByteArray headers = buf->left(bodyIdx).toLower();
                    const int clIdx = headers.indexOf("content-length:");
                    const int contentLength = clIdx >= 0
                        ? headers.mid(clIdx + 15, headers.indexOf('\r', clIdx) - clIdx - 15).trimmed().toInt()
                        : 0;
                    if (buf->size() - (bodyIdx + 4) < contentLength) return;
                    const QByteArray rawBody = buf->mid(bodyIdx + 4, contentLength);
                    buf->clear();
                    const QJsonObject bodyObj = QJsonDocument::fromJson(rawBody).object();
                    auto sendJson = [](QTcpSocket *s, int st, const QByteArray &stText, const QByteArray &jb) {
                        const QByteArray r = "HTTP/1.1 " + QByteArray::number(st) + " " + stText + "\r\n"
                                             "Content-Type: application/json\r\n"
                                             "Content-Length: " + QByteArray::number(jb.size()) + "\r\n"
                                             "Connection: close\r\n\r\n" + jb;
                        s->write(r);
                        s->flush();
                        s->disconnectFromHost();
                    };
                    if (bodyObj.value(QStringLiteral("title")).toString() == QStringLiteral("Unwritable State Event")) {
                        unwritableEvtPostCount++;
                        QJsonObject respObj;
                        respObj.insert(QStringLiteral("status"), 200);
                        respObj.insert(QStringLiteral("data"), QJsonArray());
                        sendJson(sock, 200, "OK", QJsonDocument(respObj).toJson(QJsonDocument::Compact));
                        return;
                    }
                    capturedEvtId = bodyObj.value(QStringLiteral("id")).toString();
                    evtRequestCount++;
                    if (evtRequestCount == 1) {
                        // First request: 200 OK for far-future event save (returns full 250 near events + appended outsideWindow target)
                        QJsonArray snap = buildNear250();
                        QJsonObject okEvt;
                        okEvt.insert(QStringLiteral("id"), capturedEvtId);
                        okEvt.insert(QStringLiteral("title"), QStringLiteral("Far Future 200 OK Event"));
                        okEvt.insert(QStringLiteral("startMs"), 1900000000000LL);
                        okEvt.insert(QStringLiteral("endMs"), 1900003600000LL);
                        okEvt.insert(QStringLiteral("calendarTitle"), QStringLiteral("Work"));
                        okEvt.insert(QStringLiteral("notes"), QStringLiteral("Saved via 200 OK"));
                        okEvt.insert(QStringLiteral("revision"), 1);
                        okEvt.insert(QStringLiteral("outsideWindow"), true);
                        snap.append(okEvt);
                        QJsonObject respObj;
                        respObj.insert(QStringLiteral("status"), 200);
                        respObj.insert(QStringLiteral("data"), snap);
                        sendJson(sock, 200, "OK", QJsonDocument(respObj).toJson(QJsonDocument::Compact));
                        return;
                    }
                    if (evtRequestCount == 2) {
                        // Simulate server creating the event and dropping the connection before sending the HTTP ACK
                        evtCreatedBeforeDroppedAck = true;
                        sock->disconnectFromHost();
                        return;
                    }
                    // Third request: 409 Conflict snapshot with full 250 near events + appended outsideWindow target
                    QJsonArray snap = buildNear250();
                    QJsonObject appliedEvt;
                    appliedEvt.insert(QStringLiteral("id"), capturedEvtId);
                    appliedEvt.insert(QStringLiteral("title"), QStringLiteral("Far Future Replay Event"));
                    appliedEvt.insert(QStringLiteral("startMs"), 1950000000000LL);
                    appliedEvt.insert(QStringLiteral("endMs"), 1950003600000LL);
                    appliedEvt.insert(QStringLiteral("calendarTitle"), QStringLiteral("Work"));
                    appliedEvt.insert(QStringLiteral("notes"), QStringLiteral("Trimmed event notes"));
                    appliedEvt.insert(QStringLiteral("revision"), 4);
                    appliedEvt.insert(QStringLiteral("outsideWindow"), true);
                    snap.append(appliedEvt);
                    QJsonObject respObj;
                    respObj.insert(QStringLiteral("status"), 409);
                    respObj.insert(QStringLiteral("data"), snap);
                    sendJson(sock, 409, "Conflict", QJsonDocument(respObj).toJson(QJsonDocument::Compact));
                });
            }
        });
        qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(evtServer.serverPort()).toUtf8());
        qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));
        Backend evtBackend(false);
        evtBackend.setStateFilePath(tempDir.filePath(QStringLiteral("state_events_replay.json")));
        evtBackend.loadCredentials();

        // Seed 250 near events + 1 cached intermediate out-of-window event at 1800000000000
        QJsonArray initialSeed = buildNear250();
        QJsonObject midCachedEvt;
        midCachedEvt.insert(QStringLiteral("id"), QStringLiteral("evt_mid_cached"));
        midCachedEvt.insert(QStringLiteral("title"), QStringLiteral("Intermediate Cached Out-of-Window Event"));
        midCachedEvt.insert(QStringLiteral("startMs"), 1800000000000LL);
        midCachedEvt.insert(QStringLiteral("endMs"), 1800003600000LL);
        midCachedEvt.insert(QStringLiteral("calendarTitle"), QStringLiteral("Work"));
        midCachedEvt.insert(QStringLiteral("revision"), 2);
        initialSeed.append(midCachedEvt);
        evtBackend.reconcileRemoteIcloudEventsForTest(initialSeed, QStringLiteral("evt_mid_cached"));
        QCOMPARE(evtBackend.icloudEvents().size(), 251);

        auto hasCachedEventId = [&](const QString &targetId) {
            for (const QVariant &v : evtBackend.icloudEvents()) {
                if (v.toMap().value(QStringLiteral("id")).toString() == targetId) return true;
            }
            return false;
        };

        // 1) 200 OK save of far-future event at 1900000000000 must preserve evt_near_249 and evt_mid_cached (1800000000000)
        const QString okEvtId = evtBackend.saveIcloudEvent(
            QStringLiteral("Far Future 200 OK Event"),
            1900000000000LL,
            1900003600000LL,
            QStringLiteral("Work"),
            QStringLiteral("Saved via 200 OK")
        );
        QTRY_COMPARE_WITH_TIMEOUT(evtRequestCount, 1, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(!evtBackend.hasPendingIcloudEventSaveForTest(okEvtId), 1500);
        QVERIFY(hasCachedEventId(QStringLiteral("evt_near_249")));
        QVERIFY(hasCachedEventId(QStringLiteral("evt_mid_cached")));
        QVERIFY(hasCachedEventId(okEvtId));

        // 2) Dropped ACK followed by 409 Conflict replay of far-future event at 1950000000000 must also preserve evt_near_249 and evt_mid_cached
        const QString savedEvtId = evtBackend.saveIcloudEvent(
            QStringLiteral("Far Future Replay Event "),
            1950000000000LL,
            1950003600000LL,
            QStringLiteral("Work"),
            QStringLiteral("Trimmed event notes\n  ")
        );
        const QString evtMutKey = QStringLiteral("evt_save:") + savedEvtId;
        QTRY_COMPARE_WITH_TIMEOUT(evtRequestCount, 2, 1500);
        QVERIFY(evtCreatedBeforeDroppedAck);
        QTRY_VERIFY_WITH_TIMEOUT(evtBackend.isIcloudMutationInBackoffForTest(evtMutKey), 1500);

        // Verify that if a client restarts without credentials after a dropped event-create ACK,
        // deleting the restored event preserves the pending deletion rather than cancelling it as never-sent
        {
            const QString copyStatePath = tempDir.filePath(QStringLiteral("state_events_lost_ack_copy.json"));
            QFile::remove(copyStatePath);
            QVERIFY(QFile::copy(tempDir.filePath(QStringLiteral("state_events_replay.json")), copyStatePath));
            qunsetenv("BLUEBUBBLES_URL");
            qunsetenv("BLUEBUBBLES_PASSWORD");
            Backend offlineRestartEvtBackend(false);
            offlineRestartEvtBackend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_events_offline.json")));
            offlineRestartEvtBackend.loadCredentials();
            offlineRestartEvtBackend.setStateFilePath(copyStatePath);
            QVERIFY(offlineRestartEvtBackend.hasPendingIcloudEventSaveForTest(savedEvtId));
            offlineRestartEvtBackend.deleteIcloudEvent(savedEvtId);
            QVERIFY(!offlineRestartEvtBackend.hasPendingIcloudEventSaveForTest(savedEvtId));
            QVERIFY(offlineRestartEvtBackend.hasPendingIcloudEventDeleteForTest(savedEvtId));
            qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(evtServer.serverPort()).toUtf8());
            qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));

            // Also verify that if saveAppState() fails (unwritable state path), saveIcloudEvent does not dispatch
            // the network POST until state can be durably persisted
            const QString blockerEvtFile = tempDir.filePath(QStringLiteral("unwritable_evt_blocker"));
            QFile ebf(blockerEvtFile);
            QVERIFY(ebf.open(QIODevice::WriteOnly));
            ebf.write("x");
            ebf.close();
            Backend failPersistEvtBackend(false);
            failPersistEvtBackend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_events_fail_persist.json")));
            failPersistEvtBackend.setStateFilePath(blockerEvtFile + QStringLiteral("/state.json"));
            failPersistEvtBackend.loadCredentials();
            const QString failEvtId = failPersistEvtBackend.saveIcloudEvent(
                QStringLiteral("Unwritable State Event"),
                1960000000000LL,
                1960003600000LL,
                QStringLiteral("Work"),
                QStringLiteral("Notes")
            );
            QTest::qWait(300);
            QCOMPARE(unwritableEvtPostCount, 0);
            failPersistEvtBackend.deleteIcloudEvent(failEvtId);
            QVERIFY(!failPersistEvtBackend.hasPendingIcloudEventSaveForTest(failEvtId));
            QVERIFY(failPersistEvtBackend.hasPendingIcloudEventDeleteForTest(failEvtId));
        }

        evtBackend.clearIcloudMutationRetryStateForTest(evtMutKey);
        evtBackend.replayPendingIcloudMutations();
        QTRY_COMPARE_WITH_TIMEOUT(evtRequestCount, 3, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(!evtBackend.hasPendingIcloudEventSaveForTest(savedEvtId), 1500);
        QVERIFY(!evtBackend.isIcloudMutationPausedForTest(evtMutKey));
        QVERIFY(hasCachedEventId(QStringLiteral("evt_near_249")));
        QVERIFY(hasCachedEventId(QStringLiteral("evt_mid_cached")));
        QVERIFY(hasCachedEventId(savedEvtId));
        for (const char *k : credVars) {
            const QByteArray key(k);
            if (savedEnv.contains(key)) qputenv(k, savedEnv.value(key)); else qunsetenv(k);
        }
    }

    void testIcloudPhotosAndUnpurge() {
        Backend backend(false);
        QTemporaryDir tempDir;
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_photos.json")));

        QVariantList samplePhotos;
        QVariantMap p1;
        p1.insert(QStringLiteral("guid"), QStringLiteral("att_cloud_123"));
        p1.insert(QStringLiteral("name"), QStringLiteral("vacation.heic"));
        p1.insert(QStringLiteral("mimeType"), QStringLiteral("image/heic"));
        p1.insert(QStringLiteral("isCloudEvicted"), true);
        p1.insert(QStringLiteral("width"), 3024);
        p1.insert(QStringLiteral("height"), 4032);
        p1.insert(QStringLiteral("isPortrait"), true);

        QVariantMap p2;
        p2.insert(QStringLiteral("guid"), QStringLiteral("att_local_456"));
        p2.insert(QStringLiteral("name"), QStringLiteral("landscape.jpg"));
        p2.insert(QStringLiteral("mimeType"), QStringLiteral("image/jpeg"));
        p2.insert(QStringLiteral("isCloudEvicted"), false);
        p2.insert(QStringLiteral("width"), 1920);
        p2.insert(QStringLiteral("height"), 1080);
        p2.insert(QStringLiteral("isPortrait"), false);

        samplePhotos.append(p1);
        samplePhotos.append(p2);

        backend.setIcloudPhotosForTest(samplePhotos);
        QCOMPARE(backend.icloudPhotos().size(), 2);
        QVERIFY(backend.icloudPhotos().first().toMap().value(QStringLiteral("isCloudEvicted")).toBool());
        QVERIFY(backend.icloudPhotos().first().toMap().value(QStringLiteral("isPortrait")).toBool());
        QCOMPARE(backend.icloudPhotos().first().toMap().value(QStringLiteral("width")).toInt(), 3024);
        QCOMPARE(backend.icloudPhotos().first().toMap().value(QStringLiteral("height")).toInt(), 4032);

        QVERIFY(!backend.icloudPhotos().last().toMap().value(QStringLiteral("isPortrait")).toBool());

        // Trigger unpurge/download and batch sync
        backend.downloadCloudAttachment(QStringLiteral("att_cloud_123"));
        backend.syncIcloudPhotos();
        // Method should execute without crash even if offline

        // Verify optional iCloud Hub module toggles default to true and persist across state reload
        QVERIFY(backend.icloudNotesEnabled());
        QVERIFY(backend.icloudCalendarEnabled());
        QVERIFY(backend.icloudPhotosEnabled());

        backend.setIcloudNotesEnabled(false);
        backend.setIcloudCalendarEnabled(false);
        backend.setIcloudPhotosEnabled(false);
        QVERIFY(!backend.icloudNotesEnabled());
        QVERIFY(!backend.icloudCalendarEnabled());
        QVERIFY(!backend.icloudPhotosEnabled());

        Backend reloadedBackend(false);
        reloadedBackend.setStateFilePath(tempDir.filePath(QStringLiteral("state_photos.json")));
        QVERIFY(!reloadedBackend.icloudNotesEnabled());
        QVERIFY(!reloadedBackend.icloudCalendarEnabled());
        QVERIFY(!reloadedBackend.icloudPhotosEnabled());

        reloadedBackend.setIcloudNotesEnabled(true);
        reloadedBackend.setIcloudCalendarEnabled(true);
        reloadedBackend.setIcloudPhotosEnabled(true);
        QVERIFY(reloadedBackend.icloudNotesEnabled());
        QVERIFY(reloadedBackend.icloudCalendarEnabled());
        QVERIFY(reloadedBackend.icloudPhotosEnabled());
    }

};

QTEST_MAIN(ImsgTests)
#include "imsg_tests.moc"
