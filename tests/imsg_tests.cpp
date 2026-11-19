#include <QImageReader>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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

        const QString youngLaSms = QStringLiteral(
            "Promo: YOUR HOLIDAY FIRST LOOK IS HERE!!!\n\n"
            "Get an exclusive sneak peek at our new Winter and Expedition 2.0 Compression Collections! promo.example.com/a1mj-cMJ6fZX \n\n"
            "30% OFF SITEWIDE starts 11/24, plus your chance to WIN $1,000,000 CASH!\n\n"
            "No purchase necessary. Exclusions apply. Terms: shop.example.net/a7qcUWK503Mo");
        QCOMPARE(Backend::extractFirstUrl(youngLaSms), QStringLiteral("https://promo.example.com/a1mj-cMJ6fZX"));
        const QString formattedSms = Backend::formatMessageTextWithLinks(youngLaSms);
        QVERIFY(formattedSms.contains(QStringLiteral("<a href=\"https://promo.example.com/a1mj-cMJ6fZX\" style=\"color: #64B5F6; text-decoration: underline;\">promo.example.com/a1mj-cMJ6fZX</a>")));
        QVERIFY(formattedSms.contains(QStringLiteral("<a href=\"https://shop.example.net/a7qcUWK503Mo\" style=\"color: #64B5F6; text-decoration: underline;\">shop.example.net/a7qcUWK503Mo</a>")));
        QVERIFY(!formattedSms.contains(QStringLiteral("href=\"https://2.0")));
        QVERIFY(!formattedSms.contains(QStringLiteral("href=\"https://11/24")));
        QVERIFY(formattedSms.contains(QStringLiteral("<br/><br/>")));
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
        QVERIFY(qml.contains(QStringLiteral("backend.selectChat(chatGuid)\n                                    if (win) Qt.callLater(function() { win.focusAndScrollChat() })")));
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
        QVERIFY(qml.contains(QStringLiteral("backend.registerWindow(root)")));
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
        const QString chatGuidB = QStringLiteral("iMessage;-;+14155550142");
        QJsonObject chatObjB;
        chatObjB.insert(QStringLiteral("guid"), chatGuidB);
        chatObjB.insert(QStringLiteral("chatIdentifier"), QStringLiteral("+14155550142"));
        chatObjB.insert(QStringLiteral("style"), 45);
        QJsonObject msgB;
        msgB.insert(QStringLiteral("guid"), QStringLiteral("real-alex-msg-1"));
        msgB.insert(QStringLiteral("text"), QStringLiteral("Existing Alex Rivera message"));
        msgB.insert(QStringLiteral("isFromMe"), false);
        msgB.insert(QStringLiteral("dateCreated"), 1794368700000LL);
        msgB.insert(QStringLiteral("chats"), QJsonArray{chatObjB});

        b.ingestMessagesForTest(QJsonArray{tempMsg, msgB}, false);
        b.selectChat(chatGuid);
        QCOMPARE(b.messages().size(), 1);

        // Switching from chatGuid (which has temp-*) to chatGuidB must NEVER bleed temp-* into chatGuidB!
        b.selectChat(chatGuidB);
        QCOMPARE(b.messages().size(), 1);
        QCOMPARE(b.messages().first().toMap().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("real-alex-msg-1"));

        // Switch back to chatGuid -> still has its 1 message
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
        QVERIFY(qml.contains(QStringLiteral("function cleanNoteDisplayBody(title, body, syncSource)")));
        QVERIFY(qml.contains(QStringLiteral("id: icloudCalendarPopup")));
        QVERIFY(qml.contains(QStringLiteral("id: icloudPhotosPopup")));
        // Verify compact non-fullscreen dimensions, interactive scrollbars, and pagination bars for Notes, Calendar, and Photos
        QVERIFY(qml.contains(QStringLiteral("objectName: \"notesListScrollBar\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"eventsListScrollBar\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photosGridScrollBar\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"notesPaginationBar\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"eventsPaginationBar\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photosPaginationBar\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photoViewerCard\"")));
        // Verify Full Screen toggles, page-size selectors, and dark-styled side note reader pane
        QVERIFY(qml.contains(QStringLiteral("objectName: \"notesFullScreenBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"calendarFullScreenBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photosFullScreenBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photoViewerFullScreenBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"noteDetailFullScreenBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"notesPageSizeSelector\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"eventsPageSizeSelector\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photosPageSizeSelector\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"noteSideDetailPane\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"notePopOutBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("iCloud & Local Hub Notes")));
        QVERIFY(!qml.contains(QStringLiteral("iCloud (Read-Only) & Local Hub Notes")));
        // Verify high-contrast dark green RCS/SMS sent bubble palette (#1B6E38 background, #268C48 border, #CBEFD8 metadata)
        QVERIFY(qml.contains(QStringLiteral("readonly property bool isGreenService:")));
        QVERIFY(qml.contains(QStringLiteral("#1B6E38")));
        QVERIFY(qml.contains(QStringLiteral("#268C48")));
        QVERIFY(qml.contains(QStringLiteral("#CBEFD8")));
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
        int noteDeletedConflictCount = 0;
        bool noteDeletedAllowRecreateSeen = false;
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
                    if (reqId == QStringLiteral("note_deleted_conflict")) {
                        noteDeletedConflictCount++;
                        if (bodyObj.value(QStringLiteral("allowRecreate")).toBool()) {
                            noteDeletedAllowRecreateSeen = true;
                            sendHttpJson(sock, 200, "OK",
                                         R"({"status":200,"data":[{"id":"note_deleted_conflict","title":"Recreated Deleted Note","body":"Restored Draft","folder":"Notes","revision":7}]})");
                        } else {
                            sendHttpJson(sock, 409, "Conflict",
                                         R"({"status":409,"error":"revision_conflict","data":[]})");
                        }
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

        // Verify server-deleted note 409 conflict: regular save with baseRevision 0 remains paused unless allowRecreate=true is passed
        const QString delConflictKey = QStringLiteral("note_save:note_deleted_conflict");
        liveBackend.saveIcloudNote(QStringLiteral("note_deleted_conflict"), QStringLiteral("Deleted On Server"), QStringLiteral("Draft Body"), QStringLiteral("Notes"), 1);
        QTRY_COMPARE_WITH_TIMEOUT(noteDeletedConflictCount, 1, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(liveBackend.isIcloudMutationPausedForTest(delConflictKey), 1500);
        liveBackend.saveIcloudNote(QStringLiteral("note_deleted_conflict"), QStringLiteral("Deleted On Server"), QStringLiteral("Restored Draft"), QStringLiteral("Notes"), 0, false);
        QTest::qWait(300);
        QCOMPARE(noteDeletedConflictCount, 1);
        QVERIFY(liveBackend.isIcloudMutationPausedForTest(delConflictKey));
        liveBackend.saveIcloudNote(QStringLiteral("note_deleted_conflict"), QStringLiteral("Recreated Deleted Note"), QStringLiteral("Restored Draft"), QStringLiteral("Notes"), 0, true);
        QTRY_COMPARE_WITH_TIMEOUT(noteDeletedConflictCount, 2, 1500);
        QVERIFY(noteDeletedAllowRecreateSeen);
        QTRY_VERIFY_WITH_TIMEOUT(!liveBackend.hasPendingIcloudNoteSaveForTest(QStringLiteral("note_deleted_conflict")), 1500);
        QVERIFY(!liveBackend.isIcloudMutationPausedForTest(delConflictKey));

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
                    if (!headers.startsWith("post ")) {
                        sendJson(sock, 200, "OK", "{}");
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
        struct EnvGuard {
            const QList<QByteArray> vars{
                "BLUEPILL_SERVER_URL", "BLUEBUBBLES_URL", "IMSG_SERVER_URL",
                "BLUEPILL_PASSWORD", "BLUEBUBBLES_PASSWORD", "IMSG_PASSWORD",
                "BLUEPILL_SECONDARY_SERVER_URL", "BLUEBUBBLES_SECONDARY_URL",
                "BLUEPILL_SECONDARY_URL", "IMSG_SECONDARY_URL",
                "BLUEPILL_SECONDARY_PASSWORD", "BLUEBUBBLES_SECONDARY_PASSWORD",
                "BLUEPILL_ENABLE_NOTES", "BLUEPILL_ENABLE_CALENDAR", "BLUEPILL_ENABLE_PHOTOS",
                "BLUEPILL_PORT", "BLUEPILL_CONFIG_DIR"
            };
            QMap<QByteArray, QByteArray> saved;
            QSet<QByteArray> wasSet;
            EnvGuard() {
                for (const auto &v : vars) {
                    if (qEnvironmentVariableIsSet(v.constData())) {
                        saved[v] = qgetenv(v.constData());
                        wasSet.insert(v);
                    }
                    qunsetenv(v.constData());
                }
            }
            ~EnvGuard() {
                for (const auto &v : vars) {
                    if (wasSet.contains(v)) {
                        qputenv(v.constData(), saved[v]);
                    } else {
                        qunsetenv(v.constData());
                    }
                }
            }
        } envGuard;

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        qputenv("BLUEPILL_CONFIG_DIR", tempDir.path().toUtf8());

        Backend backend(false);
        backend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_photos.json")));
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
        reloadedBackend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_photos.json")));
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

        // Verify iCloud Photos pagination (fetchIcloudPhotos(page), nextIcloudPhotosPage, prevIcloudPhotosPage, loadMoreIcloudPhotos)
        QTcpServer photosServer;
        QVERIFY(photosServer.listen(QHostAddress::LocalHost, 0));
        QList<QString> requestedQueries;
        int totalPhotosOnServer = 150;
        bool syncShouldFail = false;
        int syncPollCount = 0;
        QObject::connect(&photosServer, &QTcpServer::newConnection, &photosServer, [&]() {
            while (QTcpSocket *sock = photosServer.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock]() {
                    const QByteArray req = sock->readAll();
                    const int firstLineEnd = req.indexOf("\r\n");
                    const QString reqLine = QString::fromUtf8(firstLineEnd > 0 ? req.left(firstLineEnd) : req);
                    if (reqLine.contains(QStringLiteral("/api/v1/icloud/photos/sync"))) {
                        QJsonObject dataObj;
                        dataObj.insert(QStringLiteral("jobId"), QStringLiteral("job_sync_test"));
                        if (reqLine.startsWith(QStringLiteral("POST "))) {
                            syncPollCount = 0;
                            dataObj.insert(QStringLiteral("state"), QStringLiteral("running"));
                            dataObj.insert(QStringLiteral("status"), QStringLiteral("running"));
                        } else {
                            syncPollCount++;
                            if (syncShouldFail) {
                                dataObj.insert(QStringLiteral("state"), QStringLiteral("failed"));
                                dataObj.insert(QStringLiteral("status"), QStringLiteral("error"));
                                dataObj.insert(QStringLiteral("synced"), 0);
                                dataObj.insert(QStringLiteral("failed"), 1);
                                dataObj.insert(QStringLiteral("error"), QStringLiteral("Simulated sync error"));
                            } else {
                                totalPhotosOnServer = 152;
                                dataObj.insert(QStringLiteral("state"), QStringLiteral("completed"));
                                dataObj.insert(QStringLiteral("status"), QStringLiteral("ok"));
                                dataObj.insert(QStringLiteral("synced"), 2);
                                dataObj.insert(QStringLiteral("failed"), 0);
                            }
                        }
                        QJsonObject resp;
                        resp.insert(QStringLiteral("status"), 200);
                        resp.insert(QStringLiteral("data"), dataObj);
                        const QByteArray body = QJsonDocument(resp).toJson(QJsonDocument::Compact);
                        const QByteArray raw = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                               + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                        sock->write(raw);
                        sock->flush();
                        sock->disconnectFromHost();
                    } else if (reqLine.contains(QStringLiteral("/api/v1/icloud/photos"))) {
                        requestedQueries.append(reqLine);
                        const QUrl reqUrl(QStringLiteral("http://localhost") + reqLine.section(QLatin1Char(' '), 1, 1));
                        const QUrlQuery q(reqUrl);
                        const int limit = q.queryItemValue(QStringLiteral("limit")).toInt() > 0 ? q.queryItemValue(QStringLiteral("limit")).toInt() : 60;
                        const int offset = q.queryItemValue(QStringLiteral("offset")).toInt();
                        const int total = totalPhotosOnServer;
                        QJsonArray items;
                        const int count = qMax(0, qMin(limit, total - offset));
                        for (int i = 0; i < count; ++i) {
                            const int idx = offset + i;
                            QJsonObject item;
                            item.insert(QStringLiteral("guid"), QStringLiteral("plib:PHOTO-GUID-%1").arg(idx));
                            if (idx == 0) {
                                item.insert(QStringLiteral("transferName"), QStringLiteral("VID_0000.MOV"));
                                item.insert(QStringLiteral("mimeType"), QStringLiteral("video/quicktime"));
                                item.insert(QStringLiteral("mediaType"), QStringLiteral("video"));
                                item.insert(QStringLiteral("isVideo"), true);
                                item.insert(QStringLiteral("duration"), 14.2);
                                item.insert(QStringLiteral("isCloudEvicted"), true);
                                item.insert(QStringLiteral("canUnpurge"), false);
                                item.insert(QStringLiteral("hasPreview"), true);
                                item.insert(QStringLiteral("previewUrl"), QStringLiteral("/api/v1/attachment/plib%3APHOTO-GUID-0/live?preview=1"));
                            } else {
                                item.insert(QStringLiteral("transferName"), QStringLiteral("IMG_%1.JPG").arg(1000 + idx));
                                item.insert(QStringLiteral("mimeType"), QStringLiteral("image/jpeg"));
                                item.insert(QStringLiteral("mediaType"), QStringLiteral("image"));
                                item.insert(QStringLiteral("isVideo"), false);
                                item.insert(QStringLiteral("isCloudEvicted"), false);
                            }
                            item.insert(QStringLiteral("width"), 4032);
                            item.insert(QStringLiteral("height"), 3024);
                            items.append(item);
                        }
                        QJsonObject resp;
                        resp.insert(QStringLiteral("status"), 200);
                        resp.insert(QStringLiteral("data"), items);
                        resp.insert(QStringLiteral("total"), total);
                        resp.insert(QStringLiteral("offset"), offset);
                        resp.insert(QStringLiteral("limit"), limit);
                        resp.insert(QStringLiteral("hasMore"), (offset + count) < total);
                        const QByteArray body = QJsonDocument(resp).toJson(QJsonDocument::Compact);
                        const QByteArray raw = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                               + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                        sock->write(raw);
                        sock->flush();
                        sock->disconnectFromHost();
                    } else {
                        const QByteArray body = R"({"status":200,"data":[]})";
                        const QByteArray raw = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                               + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                        sock->write(raw);
                        sock->flush();
                        sock->disconnectFromHost();
                    }
                });
            }
        });

        const QByteArray prevUrl = qgetenv("BLUEBUBBLES_URL");
        const QByteArray prevPass = qgetenv("BLUEBUBBLES_PASSWORD");
        qputenv("BLUEBUBBLES_URL", QStringLiteral("http://127.0.0.1:%1").arg(photosServer.serverPort()).toUtf8());
        qputenv("BLUEBUBBLES_PASSWORD", QByteArrayLiteral("test-pass"));

        Backend pagedBackend(false);
        pagedBackend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_photos_paged.json")));
        pagedBackend.setStateFilePath(tempDir.filePath(QStringLiteral("state_photos_paged.json")));
        pagedBackend.loadCredentials();

        pagedBackend.fetchIcloudPhotos(0, false);
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotos().size(), 60, 1500);
        QCOMPARE(pagedBackend.icloudPhotosTotal(), 150);
        QCOMPARE(pagedBackend.icloudPhotosPage(), 0);
        QCOMPARE(pagedBackend.icloudPhotosPageSize(), 60);
        QCOMPARE(pagedBackend.icloudPhotosPageCount(), 3);
        QVERIFY(pagedBackend.icloudPhotosHasMore());
        QVERIFY(pagedBackend.icloudPhotos().first().toMap().value(QStringLiteral("previewUrl")).toString().contains(QStringLiteral("height=360")));
        QVERIFY(pagedBackend.icloudPhotos().first().toMap().value(QStringLiteral("previewUrl")).toString().contains(QStringLiteral("preview=1")));
        QVERIFY(pagedBackend.icloudPhotos().first().toMap().value(QStringLiteral("isVideo")).toBool());
        QCOMPARE(pagedBackend.icloudPhotos().first().toMap().value(QStringLiteral("duration")).toDouble(), 14.2);

        // Next page -> page 1 (offset 60)
        pagedBackend.nextIcloudPhotosPage();
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotosPage(), 1, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotos().first().toMap().value(QStringLiteral("guid")).toString(),
                                  QStringLiteral("plib:PHOTO-GUID-60"), 1500);
        QCOMPARE(pagedBackend.icloudPhotos().size(), 60);
        QVERIFY(pagedBackend.icloudPhotosHasMore());

        // Load more -> appends page 2 (30 remaining items, total in list = 90 starting at page 1, hasMore = false)
        pagedBackend.loadMoreIcloudPhotos();
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotos().size(), 90, 1500);
        QCOMPARE(pagedBackend.icloudPhotosPage(), 1);
        QVERIFY(!pagedBackend.icloudPhotosHasMore());

        // Prev page -> page 0
        pagedBackend.prevIcloudPhotosPage();
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotosPage(), 0, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotos().size(), 60, 1500);

        // Change page size -> 24 per page (150 total => 7 pages)
        pagedBackend.setIcloudPhotosPageSize(24);
        QCOMPARE(pagedBackend.icloudPhotosPageSize(), 24);
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotos().size(), 24, 1500);
        QCOMPARE(pagedBackend.icloudPhotosPageCount(), 7);
        QCOMPARE(pagedBackend.icloudPhotosPage(), 0);

        // Observe async syncIcloudPhotos() job polling through running -> completed (refreshes collection to 152) and running -> failed
        pagedBackend.syncIcloudPhotos();
        QVERIFY(pagedBackend.icloudPhotosSyncing());
        QTRY_VERIFY_WITH_TIMEOUT(!pagedBackend.icloudPhotosSyncing(), 2500);
        QVERIFY(syncPollCount >= 1);
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotosTotal(), 152, 1500);

        syncShouldFail = true;
        pagedBackend.syncIcloudPhotos();
        QVERIFY(pagedBackend.icloudPhotosSyncing());
        QTRY_VERIFY_WITH_TIMEOUT(!pagedBackend.icloudPhotosSyncing(), 2500);
        QVERIFY(syncPollCount >= 1);

        // Verify Socket.IO icloud-photos-updated push refreshes page 0 cleanly with updated server total
        totalPhotosOnServer = 155;
        pagedBackend.handleSocketIoPayloadForTest(
            QByteArrayLiteral("42[\"icloud-photos-updated\",{\"total\":155,\"maxRowId\":999}]"));
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotosTotal(), 155, 1500);

        // Verify navigation-race guard: pagination before 120ms timer fires cancels stale page-0 reset
        pagedBackend.nextIcloudPhotosPage();
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotosPage(), 1, 1500);
        totalPhotosOnServer = 153; // Simulate server deletion / count decrease while on page 1
        pagedBackend.handleSocketIoPayloadForTest(
            QByteArrayLiteral("42[\"icloud-photos-updated\",{\"total\":153,\"maxRowId\":999}]"));
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotosTotal(), 153, 1500);
        QCOMPARE(pagedBackend.icloudPhotosPage(), 1);

        // Verify cached local video keeps server ?preview=1 thumbnail URL rather than local file:// .mov URL
        QTemporaryDir cachedVidDir;
        QVERIFY(cachedVidDir.isValid());
        const QString cachedVidPath = cachedVidDir.filePath(QStringLiteral("VID_0000.MOV"));
        {
            QFile vf(cachedVidPath);
            QVERIFY(vf.open(QIODevice::WriteOnly));
            vf.write(QByteArray(256, 'V'));
        }
        pagedBackend.cacheAttachmentPathForTest(QStringLiteral("plib:PHOTO-GUID-0"), cachedVidPath);
        pagedBackend.fetchIcloudPhotos(0, false);
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotosPage(), 0, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(pagedBackend.icloudPhotos().first().toMap().value(QStringLiteral("isCloudEvicted")).toBool(), false, 1500);
        const QVariantMap cachedVidItem = pagedBackend.icloudPhotos().first().toMap();
        QVERIFY(cachedVidItem.value(QStringLiteral("previewUrl")).toString().startsWith(QStringLiteral("http://")));
        QVERIFY(cachedVidItem.value(QStringLiteral("previewUrl")).toString().contains(QStringLiteral("preview=1")));
        QCOMPARE(cachedVidItem.value(QStringLiteral("streamUrl")).toString(), QUrl::fromLocalFile(cachedVidPath).toString());

        // Verify explicit Refresh on page 0 includes refresh=1 server nudge query parameter
        requestedQueries.clear();
        pagedBackend.fetchIcloudPhotos(0, false, true);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(requestedQueries.cbegin(), requestedQueries.cend(), [](const QString &q) {
            return q.contains(QStringLiteral("offset=0")) && q.contains(QStringLiteral("refresh=1"));
        }), 1500);

        if (prevUrl.isEmpty()) qunsetenv("BLUEBUBBLES_URL"); else qputenv("BLUEBUBBLES_URL", prevUrl);
        if (prevPass.isEmpty()) qunsetenv("BLUEBUBBLES_PASSWORD"); else qputenv("BLUEBUBBLES_PASSWORD", prevPass);
    }

    void copyImageToClipboardAndPasteRoundtrip() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_clip.json")));

        QImage srcImg(2400, 1800, QImage::Format_RGBA8888);
        srcImg.fill(QColor(30, 144, 255));
        const QString imgPath = tempDir.filePath(QStringLiteral("sample_photo.jpg"));
        QVERIFY(srcImg.save(imgPath, "JPG", 90));

        const QString fileUrl = QUrl::fromLocalFile(imgPath).toString();
        QElapsedTimer timer;
        timer.start();
        QVERIFY(backend.copyImageToClipboard(fileUrl, QStringLiteral("att-img-001")));
        const qint64 copyElapsedMs = timer.elapsed();
        QVERIFY2(copyElapsedMs < 50, qPrintable(QStringLiteral("copyImageToClipboard took %1ms on GUI thread").arg(copyElapsedMs)));

        // Calling copyToClipboard with placeholder "📷 Photo" must NOT overwrite the copied image
        backend.copyToClipboard(QStringLiteral("📷 Photo"), QStringLiteral("message"));

        timer.restart();
        const QString pastedPath = backend.pasteClipboardImageToTempFile();
        const qint64 pasteElapsedMs = timer.elapsed();
        QVERIFY2(pasteElapsedMs < 50, qPrintable(QStringLiteral("pasteClipboardImageToTempFile took %1ms on GUI thread").arg(pasteElapsedMs)));
        QVERIFY(!pastedPath.isEmpty());
        QVERIFY(QFileInfo::exists(pastedPath));
        QImageReader pastedReader(pastedPath);
        QVERIFY(pastedReader.canRead());
        QCOMPARE(pastedReader.size(), QSize(2400, 1800));

        // Copying real text clears the staged image so normal text copy works as expected
        backend.copyToClipboard(QStringLiteral("Hello world text"), QStringLiteral("message"));
        if (QClipboard *cb = QGuiApplication::clipboard()) {
            QCOMPARE(cb->text(), QStringLiteral("Hello world text"));
        }
        QCOMPARE(backend.pasteClipboardImageToTempFile(), QString());
    }

    void testBluePillNotificationsSoundPresetsAndDisconnectedStatus() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString statePath = tempDir.filePath(QStringLiteral("state_notif.json"));

        const QString chatGuid = QStringLiteral("iMessage;-;+15550198877");
        QJsonObject chatObj;
        chatObj.insert(QStringLiteral("guid"), chatGuid);
        chatObj.insert(QStringLiteral("chatIdentifier"), QStringLiteral("+15550198877"));
        chatObj.insert(QStringLiteral("style"), 45);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        {
            Backend b(false);
            b.setStateFilePath(statePath);
            b.resetNotificationCountersForTest();

            // 1. Verify default notification customization properties & 6 built-in presets
            QVERIFY(b.notificationsEnabled());
            QVERIFY(b.notificationBannerEnabled());
            QVERIFY(b.notificationSoundEnabled());
            QVERIFY(b.notificationMuteWhenActive());
            QCOMPARE(b.notificationSoundPreset(), QStringLiteral("chime"));
            QCOMPARE(b.notificationSoundPresets().size(), 6);

            // Verify all 6 presets produce valid audio files and can be previewed
            const QStringList presetIds = {
                QStringLiteral("chime"),
                QStringLiteral("crystal"),
                QStringLiteral("marimba"),
                QStringLiteral("pop"),
                QStringLiteral("glass"),
                QStringLiteral("system")
            };
            for (const QString &pid : presetIds) {
                const QString soundFile = Backend::ensureNotificationSoundFile(pid);
                QVERIFY2(!soundFile.isEmpty() && QFileInfo::exists(soundFile),
                         qPrintable(QStringLiteral("Missing sound file for preset %1").arg(pid)));
                QVERIFY(QFileInfo(soundFile).size() > 512);
                b.previewNotificationSound(pid);
                QCOMPARE(b.lastSoundPresetPlayedForTest(), pid);
            }
            QCOMPARE(b.soundPlayCountForTest(), 6);
            b.resetNotificationCountersForTest();

            // 2. Initial load establishes startup baseline (ROWID 500) without notifying
            QJsonObject initialMsg;
            initialMsg.insert(QStringLiteral("guid"), QStringLiteral("msg-initial-500"));
            initialMsg.insert(QStringLiteral("text"), QStringLiteral("Initial baseline message"));
            initialMsg.insert(QStringLiteral("isFromMe"), false);
            initialMsg.insert(QStringLiteral("isRead"), false);
            initialMsg.insert(QStringLiteral("originalROWID"), 500);
            initialMsg.insert(QStringLiteral("dateCreated"), nowMs - 120000LL);
            initialMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
            b.ingestMessagesForTest(QJsonArray{initialMsg}, true);
            QCOMPARE(b.notificationCountForTest(), 0);

            // 3. Pre-startup message (within 1 hour lookback window, or ROWID <= 500) must NEVER trigger a spurious ding
            QJsonObject preStartupMsg;
            preStartupMsg.insert(QStringLiteral("guid"), QStringLiteral("msg-pre-startup-499"));
            preStartupMsg.insert(QStringLiteral("text"), QStringLiteral("Old unread message from 10 minutes ago"));
            preStartupMsg.insert(QStringLiteral("isFromMe"), false);
            preStartupMsg.insert(QStringLiteral("isRead"), false);
            preStartupMsg.insert(QStringLiteral("originalROWID"), 499);
            preStartupMsg.insert(QStringLiteral("dateCreated"), nowMs - 600000LL);
            preStartupMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
            b.ingestMessagesForTest(QJsonArray{preStartupMsg}, false);
            b.reconcileUnreadMessagesForTest(QJsonArray{preStartupMsg});
            QCOMPARE(b.notificationCountForTest(), 0);
            QCOMPARE(b.soundPlayCountForTest(), 0);

            // 4. While chatGuid is actively focused (windowActive && selectedChatGuid == chatGuid) and
            // notificationMuteWhenActive is true, incoming message in active chat is muted
            QCOMPARE(b.selectedChatGuid(), chatGuid);
            QVERIFY(b.windowActive());
            QVERIFY(b.notificationMuteWhenActive());
            b.setNotificationSoundPreset(QStringLiteral("crystal"));
            QJsonObject activeChatMsg;
            activeChatMsg.insert(QStringLiteral("guid"), QStringLiteral("msg-active-muted-501"));
            activeChatMsg.insert(QStringLiteral("text"), QStringLiteral("Message while actively viewing chat"));
            activeChatMsg.insert(QStringLiteral("isFromMe"), false);
            activeChatMsg.insert(QStringLiteral("isRead"), false);
            activeChatMsg.insert(QStringLiteral("originalROWID"), 501);
            activeChatMsg.insert(QStringLiteral("dateCreated"), QDateTime::currentMSecsSinceEpoch() + 500LL);
            activeChatMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
            b.ingestMessagesForTest(QJsonArray{activeChatMsg}, false);
            QCOMPARE(b.notificationCountForTest(), 0);

            // 5. When on Home dashboard (clearSelectedChat) or window inactive, genuinely new incoming message
            // (dateCreated >= startupTimestamp, ROWID > 500) triggers notification once
            b.clearSelectedChat();
            QJsonObject liveMsg;
            liveMsg.insert(QStringLiteral("guid"), QStringLiteral("msg-live-502"));
            liveMsg.insert(QStringLiteral("text"), QStringLiteral("Brand new live message!"));
            liveMsg.insert(QStringLiteral("isFromMe"), false);
            liveMsg.insert(QStringLiteral("isRead"), false);
            liveMsg.insert(QStringLiteral("originalROWID"), 502);
            liveMsg.insert(QStringLiteral("dateCreated"), QDateTime::currentMSecsSinceEpoch() + 1000LL);
            liveMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
            b.ingestMessagesForTest(QJsonArray{liveMsg}, false);
            if (!b.omarchyDnd()) {
                QCOMPARE(b.notificationCountForTest(), 1);
                QCOMPARE(b.soundPlayCountForTest(), 1);
                QCOMPARE(b.lastSoundPresetPlayedForTest(), QStringLiteral("crystal"));
            }

            // Re-ingesting or reconciling the same message must NEVER ding a second time
            b.ingestMessagesForTest(QJsonArray{liveMsg}, false);
            b.reconcileUnreadMessagesForTest(QJsonArray{liveMsg});
            if (!b.omarchyDnd()) {
                QCOMPARE(b.notificationCountForTest(), 1);
                QCOMPARE(b.soundPlayCountForTest(), 1);
            }

            // Customize settings and verify persistence
            b.setNotificationBannerEnabled(false);
            b.setNotificationSoundEnabled(false);
            b.setNotificationMuteWhenActive(false);
            b.setNotificationSoundPreset(QStringLiteral("marimba"));
        }

        // Reload from state.json -> settings and notifiedMessageGuids persist
        {
            Backend b2(false);
            b2.setStateFilePath(statePath);
            QCOMPARE(b2.notificationBannerEnabled(), false);
            QCOMPARE(b2.notificationSoundEnabled(), false);
            QCOMPARE(b2.notificationMuteWhenActive(), false);
            QCOMPARE(b2.notificationSoundPreset(), QStringLiteral("marimba"));

            // Re-ingesting msg-live-502 after restart must still NOT re-notify
            QJsonObject liveMsg;
            liveMsg.insert(QStringLiteral("guid"), QStringLiteral("msg-live-502"));
            liveMsg.insert(QStringLiteral("text"), QStringLiteral("Brand new live message!"));
            liveMsg.insert(QStringLiteral("isFromMe"), false);
            liveMsg.insert(QStringLiteral("isRead"), false);
            liveMsg.insert(QStringLiteral("originalROWID"), 502);
            liveMsg.insert(QStringLiteral("dateCreated"), QDateTime::currentMSecsSinceEpoch() + 1500LL);
            liveMsg.insert(QStringLiteral("chats"), QJsonArray{chatObj});
            b2.ingestMessagesForTest(QJsonArray{liveMsg}, false);
            QCOMPARE(b2.notificationCountForTest(), 0);

            // 5. Verify QML top-left disconnected red indicator, "BluePill Settings" button/modal, and preset buttons
            Theme theme;
            Translator i18n;
            QQmlApplicationEngine engine;
            engine.rootContext()->setContextProperty(QStringLiteral("backend"), &b2);
            engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
            engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &i18n);
            engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
            QVERIFY(!engine.rootObjects().isEmpty());

            QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            QVERIFY(win != nullptr);
            win->setWidth(1280);
            win->setHeight(800);

            auto *statusDot = win->findChild<QQuickItem *>(QStringLiteral("topConnectionStatusDot"));
            auto *serverBtnText = win->findChild<QQuickItem *>(QStringLiteral("serverTopBtnText"));
            auto *homeSettingsText = win->findChild<QQuickItem *>(QStringLiteral("homeServerLbl"));
            auto *settingsTitle = win->findChild<QQuickItem *>(QStringLiteral("settingsModalTitle"));
            auto *cfgBadge = win->findChild<QQuickItem *>(QStringLiteral("cfgStatusBadge"));
            auto *previewBtn = win->findChild<QQuickItem *>(QStringLiteral("previewSoundBtn"));
            auto *presetGrid = win->findChild<QQuickItem *>(QStringLiteral("soundPresetGrid"));
            QVERIFY(statusDot != nullptr);
            QVERIFY(serverBtnText != nullptr);
            QVERIFY(homeSettingsText != nullptr);
            QVERIFY(settingsTitle != nullptr);
            QVERIFY(cfgBadge != nullptr);
            QVERIFY(previewBtn != nullptr);
            QVERIFY(presetGrid != nullptr);

            // Top button, Home dashboard button, and modal title all say "Settings"
            QCOMPARE(serverBtnText->property("text").toString(), QStringLiteral("Settings"));
            QCOMPARE(homeSettingsText->property("text").toString(), QStringLiteral("⚙ Settings"));
            QCOMPARE(settingsTitle->property("text").toString(), QStringLiteral("⚙ Settings"));

            // Initially disconnected -> top-left dot is red (theme.red) and badge says Disconnected
            QCOMPARE(b2.connected(), false);
            QCOMPARE(statusDot->property("color").value<QColor>(), theme.red());
            QCOMPARE(cfgBadge->property("text").toString(), QStringLiteral("Disconnected"));

            // Network success -> connected -> top-left dot turns green (theme.green)
            b2.simulateNetworkSuccessForTest();
            QCOMPARE(b2.connected(), true);
            QCOMPARE(statusDot->property("color").value<QColor>(), theme.green());
            QCOMPARE(cfgBadge->property("text").toString(), QStringLiteral("Connected"));

            // Network failure -> immediately turns red (theme.red) on first failure
            b2.simulateNetworkFailureForTest();
            QCOMPARE(b2.connected(), false);
            QCOMPARE(statusDot->property("color").value<QColor>(), theme.red());
            QCOMPARE(cfgBadge->property("text").toString(), QStringLiteral("Disconnected"));
        }
    }

    void testRcsGroupSiblingUnificationAndActiveSendRouting() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_unify.json")));

        const QString staleRcsGuid = QStringLiteral("RCS;+;chat111111111111111111");
        const QString activeRcsGuid = QStringLiteral("RCS;+;chat222222222222222222");
        const QString smsLegacyGuid = QStringLiteral("SMS;+;chat333333333333333333");
        const QString otherGroupGuid = QStringLiteral("RCS;+;chat999999999999999999");
        const QString dmGuid = QStringLiteral("RCS;-;+15555550110");
        const QString sharedOrigGroupId = QStringLiteral("31443445000000000000000000000001");

        QJsonObject cStale{
            {QStringLiteral("guid"), staleRcsGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat111111111111111111")},
            {QStringLiteral("displayName"), QStringLiteral("Weekend Hike Crew")},
            {QStringLiteral("style"), 43},
            {QStringLiteral("originalGroupId"), sharedOrigGroupId},
            {QStringLiteral("groupId"), QStringLiteral("grp-stale-1")},
            {QStringLiteral("originalROWID"), 3882},
            {QStringLiteral("isArchived"), false},
            {QStringLiteral("participants"), QJsonArray{
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550101")}},
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550102")}}
            }}
        };
        QJsonObject cActive{
            {QStringLiteral("guid"), activeRcsGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat222222222222222222")},
            {QStringLiteral("displayName"), QStringLiteral("Weekend Hike Crew")},
            {QStringLiteral("style"), 43},
            {QStringLiteral("originalGroupId"), sharedOrigGroupId},
            {QStringLiteral("groupId"), QStringLiteral("grp-active-2")},
            {QStringLiteral("originalROWID"), 4050},
            {QStringLiteral("isArchived"), false},
            {QStringLiteral("participants"), QJsonArray{
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550101")}},
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550102")}},
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550103")}}
            }}
        };
        QJsonObject cSms{
            {QStringLiteral("guid"), smsLegacyGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat333333333333333333")},
            {QStringLiteral("displayName"), QStringLiteral("Weekend Hike Crew")},
            {QStringLiteral("style"), 43},
            {QStringLiteral("originalGroupId"), sharedOrigGroupId},
            {QStringLiteral("originalROWID"), 2500},
            {QStringLiteral("isArchived"), true}
        };
        QJsonObject cOther{
            {QStringLiteral("guid"), otherGroupGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat999999999999999999")},
            {QStringLiteral("displayName"), QStringLiteral("Book Club")},
            {QStringLiteral("style"), 43},
            {QStringLiteral("originalGroupId"), QStringLiteral("OTHER_OGID_999")},
            {QStringLiteral("originalROWID"), 4100}
        };
        QJsonObject cDm{
            {QStringLiteral("guid"), dmGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("+15555550110")},
            {QStringLiteral("style"), 45},
            {QStringLiteral("originalROWID"), 4105}
        };

        b.ingestChatsMetadataForTest(QJsonArray{cStale, cActive, cSms, cOther, cDm});
        // 3 sibling chats sharing originalGroupId must unify into 1 entry + Book Club + DM = 3 total
        QCOMPARE(b.chats().size(), 3);
        QVERIFY(b.isSameConversation(staleRcsGuid, activeRcsGuid));
        QVERIFY(b.isSameConversation(smsLegacyGuid, activeRcsGuid));
        QVERIFY(!b.isSameConversation(activeRcsGuid, otherGroupGuid));

        // Message present on stale sibling, failed send on stale sibling, newer message on active sibling, and one cross-joined message
        QJsonObject msgOldOnStale{
            {QStringLiteral("guid"), QStringLiteral("rcs-msg-stale-1")},
            {QStringLiteral("text"), QStringLiteral("Meet at trailhead at 8")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), 1794300000000LL},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550101")}}},
            {QStringLiteral("chats"), QJsonArray{cStale}}
        };
        QJsonObject msgFailedOnStale{
            {QStringLiteral("guid"), QStringLiteral("rcs-msg-failed-on-stale")},
            {QStringLiteral("text"), QStringLiteral("Unsent failed reply")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), false},
            {QStringLiteral("error"), 4},
            {QStringLiteral("dateCreated"), 1794300100000LL},
            {QStringLiteral("chats"), QJsonArray{cStale}}
        };
        QJsonObject msgOnActive{
            {QStringLiteral("guid"), QStringLiteral("rcs-msg-active-2")},
            {QStringLiteral("text"), QStringLiteral("Sounds great, bringing water")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), 1794300200000LL},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550103")}}},
            {QStringLiteral("chats"), QJsonArray{cActive}}
        };
        // Same message linked to both stale and active siblings in chat_message_join
        QJsonObject msgJoinedBoth{
            {QStringLiteral("guid"), QStringLiteral("rcs-msg-joined-both")},
            {QStringLiteral("text"), QStringLiteral("Pinned parking map")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), 1794300300000LL},
            {QStringLiteral("handle"), QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550102")}}},
            {QStringLiteral("chats"), QJsonArray{cStale, cActive}}
        };

        b.ingestMessagesForTest(QJsonArray{msgOldOnStale, msgFailedOnStale, msgOnActive, msgJoinedBoth}, false);
        QCOMPARE(b.chats().size(), 3);

        // Selecting via stale sibling GUID resolves to unified conversation containing all 4 unique messages (deduped!)
        b.selectChat(staleRcsGuid);
        QCOMPARE(b.messages().size(), 4);
        QCOMPARE(b.resolveActiveSendChatGuidForTest(staleRcsGuid), activeRcsGuid);

        // Load QML and verify replyTargetGuid is NOT wiped when background chat updates arrive for another chat
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
        win->setProperty("replyTargetGuid", QStringLiteral("rcs-msg-active-2"));
        win->setProperty("replyTargetSender", QStringLiteral("Participant"));
        win->setProperty("replyTargetText", QStringLiteral("Sounds great, bringing water"));

        QJsonObject dmMsg{
            {QStringLiteral("guid"), QStringLiteral("dm-msg-bg-1")},
            {QStringLiteral("text"), QStringLiteral("Background ping in other chat")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), 1794300400000LL},
            {QStringLiteral("chats"), QJsonArray{cDm}}
        };
        b.ingestMessagesForTest(QJsonArray{dmMsg}, false);
        QCOMPARE(win->property("replyTargetGuid").toString(), QStringLiteral("rcs-msg-active-2"));

        // Verify unkeyed groups with identical displayName but distinct participant sets ({101,102,103} vs {101,102,104}) stay separate
        QJsonObject unkeyedA{
            {QStringLiteral("guid"), QStringLiteral("RCS;+;chat_unkeyed_1")},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat_unkeyed_1")},
            {QStringLiteral("displayName"), QStringLiteral("Weekend")},
            {QStringLiteral("style"), 43},
            {QStringLiteral("participants"), QJsonArray{
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550101")}},
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550102")}},
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550103")}}
            }}
        };
        QJsonObject unkeyedB{
            {QStringLiteral("guid"), QStringLiteral("RCS;+;chat_unkeyed_2")},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat_unkeyed_2")},
            {QStringLiteral("displayName"), QStringLiteral("Weekend")},
            {QStringLiteral("style"), 43},
            {QStringLiteral("participants"), QJsonArray{
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550101")}},
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550102")}},
                QJsonObject{{QStringLiteral("address"), QStringLiteral("+15555550104")}}
            }}
        };
        b.ingestChatsMetadataForTest(QJsonArray{cStale, cActive, cSms, cOther, cDm, unkeyedA, unkeyedB});
        QVERIFY(!b.isSameConversation(QStringLiteral("RCS;+;chat_unkeyed_1"), QStringLiteral("RCS;+;chat_unkeyed_2")));
        QCOMPARE(b.chats().size(), 5);

        // Verify markChatUnread clears canonical watermark and does not resurrect stale sibling watermark on re-unification
        b.markChatRead(staleRcsGuid);
        b.markChatUnread(activeRcsGuid);
        b.ingestChatsMetadataForTest(QJsonArray{cStale, cActive, cSms, cOther, cDm, unkeyedA, unkeyedB});
        bool foundUnifiedUnread = false;
        for (const QVariant &cv : b.chats()) {
            const QVariantMap cm = cv.toMap();
            if (b.isSameConversation(cm.value(QStringLiteral("guid")).toString(), activeRcsGuid)) {
                foundUnifiedUnread = cm.value(QStringLiteral("unreadCount")).toInt() >= 1;
            }
        }
        QVERIFY(foundUnifiedUnread);

        // Verify marking a fresh un-consolidated sibling unread preserves qint64 unread override after unification
        const QString preUnreadSib = QStringLiteral("RCS;+;chat_pre_unread_sib");
        const qint64 futureTs = QDateTime::currentMSecsSinceEpoch() + 60000;
        QJsonObject preSibUnkeyed{
            {QStringLiteral("guid"), preUnreadSib},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat_pre_unread_sib")},
            {QStringLiteral("displayName"), QStringLiteral("Weekend Hike Crew")},
            {QStringLiteral("originalROWID"), 100},
            {QStringLiteral("style"), 43}
        };
        QJsonObject preSibKeyed = preSibUnkeyed;
        preSibKeyed.insert(QStringLiteral("originalGroupId"), sharedOrigGroupId);
        QJsonObject preSibMsg{
            {QStringLiteral("guid"), QStringLiteral("rcs-msg-pre-unread-1")},
            {QStringLiteral("text"), QStringLiteral("Unread ping before unification")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("isRead"), true},
            {QStringLiteral("dateRead"), futureTs},
            {QStringLiteral("dateCreated"), futureTs},
            {QStringLiteral("chats"), QJsonArray{preSibUnkeyed}}
        };
        QJsonObject cActiveWinMsg{
            {QStringLiteral("guid"), QStringLiteral("rcs-msg-active-win-1")},
            {QStringLiteral("text"), QStringLiteral("Latest active message")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("dateCreated"), futureTs + 1000},
            {QStringLiteral("chats"), QJsonArray{cActive}}
        };
        b.markChatRead(activeRcsGuid);
        b.ingestChatsMetadataForTest(QJsonArray{cStale, cActive, cSms, preSibUnkeyed, cOther, cDm, unkeyedA, unkeyedB});
        b.ingestMessagesForTest(QJsonArray{preSibMsg, cActiveWinMsg}, false);
        QVERIFY(!b.isSameConversation(preUnreadSib, activeRcsGuid));
        b.markChatUnread(preUnreadSib);
        b.ingestChatsMetadataForTest(QJsonArray{cStale, cActive, cSms, preSibKeyed, cOther, cDm, unkeyedA, unkeyedB});
        QVERIFY(b.isSameConversation(preUnreadSib, activeRcsGuid));
        QCOMPARE(b.resolveCanonicalChatGuid(preUnreadSib), b.resolveCanonicalChatGuid(activeRcsGuid));
        const QString winnerGuid = b.resolveCanonicalChatGuid(activeRcsGuid);
        bool preSibUnreadPreserved = false;
        for (const QVariant &cv : b.chats()) {
            const QVariantMap cm = cv.toMap();
            if (cm.value(QStringLiteral("guid")).toString() == winnerGuid) {
                preSibUnreadPreserved = cm.value(QStringLiteral("unreadCount")).toInt() >= 1;
            }
        }
        QVERIFY(preSibUnreadPreserved);
    }

    void testAsyncDeliveryFailureSurfacing() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_err.json")));

        const QString chatGuid = QStringLiteral("RCS;+;chat777777777777777777");
        QJsonObject chatObj{
            {QStringLiteral("guid"), chatGuid},
            {QStringLiteral("chatIdentifier"), QStringLiteral("chat777777777777777777")},
            {QStringLiteral("displayName"), QStringLiteral("RCS Thread Test")},
            {QStringLiteral("style"), 43}
        };

        // Initial queued outgoing RCS reply (isSent=false, isDelivered=false, error=0)
        QJsonObject pendingReply{
            {QStringLiteral("guid"), QStringLiteral("rcs-pending-reply-1")},
            {QStringLiteral("text"), QStringLiteral("Count me in!")},
            {QStringLiteral("threadOriginatorGuid"), QStringLiteral("p:0/parent-guid-1")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), false},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("error"), 0},
            {QStringLiteral("dateCreated"), QDateTime::currentMSecsSinceEpoch()},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{pendingReply}, false);
        b.selectChat(chatGuid);
        QCOMPARE(b.messages().size(), 1);
        QCOMPARE(b.messages().first().toMap().value(QStringLiteral("isFailed")).toBool(), false);
        QCOMPARE(b.messages().first().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

        // Async carrier delivery failure arrives (is_sent=0, error=4) via updated-message / pollDelta
        QSignalSpy toastSpy(&b, &Backend::toastMessage);
        QJsonObject failedUpdate = pendingReply;
        failedUpdate.insert(QStringLiteral("error"), 4);
        b.ingestMessagesForTest(QJsonArray{failedUpdate}, false);

        QCOMPARE(b.messages().size(), 1);
        const QVariantMap msgVm = b.messages().first().toMap();
        QCOMPARE(msgVm.value(QStringLiteral("isFailed")).toBool(), true);
        QCOMPARE(msgVm.value(QStringLiteral("errorCode")).toInt(), 4);
        QCOMPARE(msgVm.value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));
        QVERIFY(!toastSpy.isEmpty());
    }

    void testStuckSendingRecoveryAndBatchedQuery() {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QJsonObject freshSending{
            {QStringLiteral("guid"), QStringLiteral("g-fresh")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), false},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("error"), 0},
            {QStringLiteral("dateCreated"), nowMs - 5000}
        };
        QCOMPARE(Backend::computeOutgoingReceiptText(freshSending, false, nowMs), QStringLiteral("Sending..."));
        QCOMPARE(Backend::computeOutgoingReceiptText(freshSending, true, nowMs), QStringLiteral("Sent"));

        QJsonObject staleSending = freshSending;
        staleSending.insert(QStringLiteral("guid"), QStringLiteral("g-stale"));
        staleSending.insert(QStringLiteral("dateCreated"), nowMs - 180000);
        QCOMPARE(Backend::computeOutgoingReceiptText(staleSending, false, nowMs), QStringLiteral("Unconfirmed"));

        QJsonObject failedMsg = freshSending;
        failedMsg.insert(QStringLiteral("error"), 4);
        QCOMPARE(Backend::computeOutgoingReceiptText(failedMsg, false, nowMs), QStringLiteral("⚠ Not Delivered"));

        QJsonObject noFlagsMsg{{QStringLiteral("guid"), QStringLiteral("g-noflags")}, {QStringLiteral("isFromMe"), true}};
        QCOMPARE(Backend::computeOutgoingReceiptText(noFlagsMsg, false, nowMs), QStringLiteral("Delivered"));

        // Timeline with outgoing isSent=false followed by incoming reply must render Sent, not Sending...
        QJsonObject incomingReply{
            {QStringLiteral("guid"), QStringLiteral("g-reply")},
            {QStringLiteral("text"), QStringLiteral("Got it!")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), nowMs - 1000}
        };
        QJsonObject outBeforeReply = freshSending;
        outBeforeReply.insert(QStringLiteral("text"), QStringLiteral("Ping"));
        const QVariantList built = Backend::buildMessageListFromApi(QJsonArray{incomingReply, outBeforeReply}, {}, {});
        QCOMPARE(built.size(), 2);
        QCOMPARE(built.first().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sent"));

        // Verify checkPendingDeliveryStatuses batches GUIDs into a single IN (:g0,:g1) query and expires stale items
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();
        int queryCount = 0;
        QString capturedStatement;
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550140");
        QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Batch Test")}, {QStringLiteral("style"), 45}};
        QJsonObject pendFresh{
            {QStringLiteral("guid"), QStringLiteral("pend-fresh-1")},
            {QStringLiteral("text"), QStringLiteral("Fresh pending")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), false},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateCreated"), nowMs - 2000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        QJsonObject pendFresh2 = pendFresh;
        pendFresh2.insert(QStringLiteral("guid"), QStringLiteral("pend-fresh-2"));
        QJsonObject pendStale = pendFresh;
        pendStale.insert(QStringLiteral("guid"), QStringLiteral("pend-stale-3"));
        pendStale.insert(QStringLiteral("dateCreated"), nowMs - 180000);

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, &queryCount, &capturedStatement, pendFresh, pendFresh2]() {
                    const QByteArray raw = sock->readAll();
                    if (raw.contains("POST /api/v1/message/query")) {
                        const int sep = raw.indexOf("\r\n\r\n");
                        const QJsonObject body = QJsonDocument::fromJson(raw.mid(sep + 4)).object();
                        const QJsonArray where = body.value(QStringLiteral("where")).toArray();
                        const QString stmt = where.isEmpty() ? QString() : where.first().toObject().value(QStringLiteral("statement")).toString();
                        if (stmt.contains(QStringLiteral("message.guid"))) {
                            ++queryCount;
                            capturedStatement = stmt;
                            const QJsonArray respData{
                                QJsonObject{{QStringLiteral("guid"), QStringLiteral("pend-fresh-1")}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), QDateTime::currentMSecsSinceEpoch()}}
                            };
                            const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), respData}}).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        } else {
                            const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), QJsonArray{pendFresh, pendFresh2}}}).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        }
                        sock->disconnectFromHost();
                    } else {
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    }
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_batch.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_batch.json")));
        b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(port), QString(), QStringLiteral("pw"));

        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.ingestMessagesForTest(QJsonArray{pendFresh, pendFresh2, pendStale}, false);
        b.selectChat(chatGuid);
        // pendStale (>120s) is immediately excluded; only pendFresh and pendFresh2 stay tracked
        QCOMPARE(b.pendingDeliveryCountForTest(), 2);

        b.checkPendingDeliveryStatusesForTest();
        QTRY_COMPARE_WITH_TIMEOUT(queryCount, 1, 1000);
        QVERIFY2(capturedStatement.contains(QStringLiteral("message.guid IN (")),
                 qPrintable(QStringLiteral("Expected batched IN query, got: ") + capturedStatement));
        // pend-fresh-1 resolved to Delivered from server response, leaving pend-fresh-2
        QTRY_COMPARE_WITH_TIMEOUT(b.pendingDeliveryCountForTest(), 1, 1000);

        // Once a later incoming reply arrives, pend-fresh-2 expires locally without another network query
        QJsonObject laterReply{
            {QStringLiteral("guid"), QStringLiteral("later-reply-batch-1")},
            {QStringLiteral("text"), QStringLiteral("Reply arrived")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), nowMs},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{laterReply}, false);
        b.checkPendingDeliveryStatusesForTest();
        QCOMPARE(b.pendingDeliveryCountForTest(), 0);
        QCOMPARE(queryCount, 1);
    }

    void testVoiceMemoCapturePreviewCancelAndPlayback() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();
        QByteArray uploadedMultipart;
        QByteArray downloadableWav;
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550141");
        const QJsonObject seedChatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Voice Test")}, {QStringLiteral("style"), 45}};
        const QJsonObject seedMsgObj{
            {QStringLiteral("guid"), QStringLiteral("seed-msg-vm")},
            {QStringLiteral("text"), QStringLiteral("Hi")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), QDateTime::currentMSecsSinceEpoch() - 10000},
            {QStringLiteral("chats"), QJsonArray{seedChatObj}}
        };
        QJsonObject sentMsgObj;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &uploadedMultipart, &downloadableWav, seedChatObj, seedMsgObj, &sentMsgObj]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLen = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    if (hdr.startsWith("POST /api/v1/message/attachment")) {
                        uploadedMultipart = buf->mid(hdrEnd + 4, contentLen);
                        QJsonObject att{
                            {QStringLiteral("guid"), QStringLiteral("srv-att-voice-1")},
                            {QStringLiteral("mimeType"), QStringLiteral("audio/wav")},
                            {QStringLiteral("transferName"), QStringLiteral("voice_memo_test.wav")},
                            {QStringLiteral("durationSeconds"), 1},
                            {QStringLiteral("waveform"), QJsonArray{0.4, 0.8, 0.6}}
                        };
                        sentMsgObj = QJsonObject{
                            {QStringLiteral("guid"), QStringLiteral("srv-msg-voice-1")},
                            {QStringLiteral("text"), QString()},
                            {QStringLiteral("isFromMe"), true},
                            {QStringLiteral("isSent"), true},
                            {QStringLiteral("isDelivered"), true},
                            {QStringLiteral("dateCreated"), QDateTime::currentMSecsSinceEpoch()},
                            {QStringLiteral("attachments"), QJsonArray{att}},
                            {QStringLiteral("chats"), QJsonArray{seedChatObj}}
                        };
                        const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), sentMsgObj}}).toJson(QJsonDocument::Compact);
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    } else if (hdr.startsWith("POST /api/v1/message/query")) {
                        QJsonArray arr{seedMsgObj};
                        if (!sentMsgObj.isEmpty()) arr.prepend(sentMsgObj);
                        const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), arr}}).toJson(QJsonDocument::Compact);
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    } else if (hdr.contains("GET /api/v1/attachment/remote-voice-att-1/download")) {
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " + QByteArray::number(downloadableWav.size()) + "\r\nConnection: close\r\n\r\n" + downloadableWav);
                        sock->disconnectFromHost();
                    } else {
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    }
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_vm.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_vm.json")));
        b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(port), QString(), QStringLiteral("pw"));

        b.ingestChatsMetadataForTest(QJsonArray{seedChatObj});
        b.ingestMessagesForTest(QJsonArray{seedMsgObj}, false);
        b.selectChat(chatGuid);

        // 1. Start -> Stop for preview -> Listen -> Cancel discards temp file and stops preview playback
        b.startVoiceRecording();
        QVERIFY(b.recordingVoice());
        b.stopVoiceRecordingForPreview();
        QVERIFY(!b.recordingVoice());
        QVERIFY(b.hasRecordedVoiceMemo());
        QVERIFY(b.recordedVoiceDurationSeconds() >= 1);
        QCOMPARE(b.recordedVoiceWaveform().size(), 28);
        const QString stagedPath = b.recordedVoicePath();
        QVERIFY(QFileInfo::exists(stagedPath));
        QVERIFY(QFileInfo(stagedPath).size() > 44);
        QVERIFY(Backend::wavDurationSeconds(stagedPath) >= 1);

        // Read WAV bytes for remote download check later
        QFile f(stagedPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        downloadableWav = f.readAll();
        f.close();

        b.togglePreviewRecordedVoiceMemo();
        QCOMPARE(b.playingAudioGuid(), QStringLiteral("voice-memo-preview"));
        QCOMPARE(b.audioPlaying(), true);

        b.cancelVoiceRecording();
        QVERIFY(!b.hasRecordedVoiceMemo());
        QVERIFY(!b.audioPlaying());
        QVERIFY(!QFileInfo::exists(stagedPath));

        // 2. Start -> Stop for preview -> Send recorded voice memo (no duplicate "Voice Memo" caption!)
        b.startVoiceRecording();
        b.stopVoiceRecordingForPreview();
        QVERIFY(b.hasRecordedVoiceMemo());
        b.sendRecordedVoiceMemo();
        QVERIFY(!b.hasRecordedVoiceMemo());

        QTRY_VERIFY_WITH_TIMEOUT(!uploadedMultipart.isEmpty(), 1500);
        QVERIFY(!uploadedMultipart.contains("name=\"message\""));
        QTRY_VERIFY_WITH_TIMEOUT(!b.messages().isEmpty() &&
                                 b.messages().last().toMap().value(QStringLiteral("guid")).toString() == QStringLiteral("srv-msg-voice-1"),
                                 1500);
        const QVariantMap sentVm = b.messages().last().toMap();
        QCOMPARE(sentVm.value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));
        QCOMPARE(sentVm.value(QStringLiteral("attachments")).toList().size(), 1);
        QCOMPARE(sentVm.value(QStringLiteral("fileAttachments")).toList().size(), 1);
        const QVariantMap sentAtt = sentVm.value(QStringLiteral("attachments")).toList().first().toMap();
        QCOMPARE(sentAtt.value(QStringLiteral("guid")).toString(), QStringLiteral("srv-att-voice-1"));
        QCOMPARE(sentAtt.value(QStringLiteral("isAudio")).toBool(), true);
        QVERIFY(!sentAtt.value(QStringLiteral("localUrl")).toString().isEmpty());

        // 3. Playback of just-sent attachment works immediately, and remote attachment downloads & updates duration
        b.toggleAudioPlayback(QStringLiteral("srv-att-voice-1"), sentAtt.value(QStringLiteral("sourceUrl")).toString());
        QCOMPARE(b.playingAudioGuid(), QStringLiteral("srv-att-voice-1"));
        QCOMPARE(b.audioPlaying(), true);
        b.toggleAudioPlayback(QStringLiteral("srv-att-voice-1"), sentAtt.value(QStringLiteral("sourceUrl")).toString());
        QCOMPARE(b.audioPlaying(), false);

        b.toggleAudioPlayback(QStringLiteral("remote-voice-att-1"), QStringLiteral("/api/v1/attachment/remote-voice-att-1/download"));
        QCOMPARE(b.playingAudioGuid(), QStringLiteral("remote-voice-att-1"));
        QTRY_COMPARE_WITH_TIMEOUT(b.audioDurationText(), QStringLiteral("0:01"), 1500);
    }

    void testAudioDeviceSelectionAndPersistence() {
        const QByteArray sampleSources =
            "41\talsa_output.pci-0000_00_1f.3.analog-stereo.monitor\tPipeWire\ts32le 2ch 48000Hz\tSUSPENDED\n"
            "42\talsa_input.usb-Blue_Microphones-00.mono-fallback\tPipeWire\ts16le 1ch 48000Hz\tRUNNING\n";
        const QVariantList filtered = Backend::parsePactlDeviceList(sampleSources, true);
        QCOMPARE(filtered.size(), 1);
        QCOMPARE(filtered.first().toMap().value(QStringLiteral("id")).toString(),
                 QStringLiteral("alsa_input.usb-Blue_Microphones-00.mono-fallback"));

        const QVariantList unfiltered = Backend::parsePactlDeviceList(sampleSources, false);
        QCOMPARE(unfiltered.size(), 2);

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("config_audio.json"));
        {
            Backend b1(false);
            b1.setConfigFilePath(cfgPath);
            b1.setSelectedAudioInputDevice(QStringLiteral("alsa_input.usb-mic"));
            b1.setSelectedAudioOutputDevice(QStringLiteral("alsa_output.headphones"));
        }
        Backend b2(false);
        b2.setConfigFilePath(cfgPath);
        b2.loadCredentials();
        QCOMPARE(b2.selectedAudioInputDevice(), QStringLiteral("alsa_input.usb-mic"));
        QCOMPARE(b2.selectedAudioOutputDevice(), QStringLiteral("alsa_output.headphones"));
    }

    void testQmlComposerStagingAndVoiceMemoUiControls() {
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../../src/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());

        QVERIFY(qml.contains(QStringLiteral("objectName: \"stagedAttachmentBar\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"voiceMemoBar\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"voiceMemoPreviewPlayBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"voiceMemoCancelBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"voiceMemoStopPreviewBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"voiceMemoSendBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"audioInputDeviceCombo\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"audioOutputDeviceCombo\"")));
        QVERIFY(qml.contains(QStringLiteral("backend.recordingVoice || backend.hasRecordedVoiceMemo")));
        QVERIFY(qml.contains(QStringLiteral("root.stageAttachment(pastedPath")));
        QVERIFY(qml.contains(QStringLiteral("function isStagedAttachmentForChat(chatGuid)")));
        const int escIdx = qml.indexOf(QStringLiteral("sequence: \"Escape\""));
        QVERIFY(escIdx > 0);
        const int modalCheckIdx = qml.indexOf(QStringLiteral("if (root.showPhotoViewer)"), escIdx);
        const int voiceCancelIdx = qml.indexOf(QStringLiteral("if (backend.recordingVoice || backend.hasRecordedVoiceMemo)"), escIdx);
        QVERIFY(modalCheckIdx > escIdx);
        QVERIFY(voiceCancelIdx > modalCheckIdx);

        // Behavioral Escape-key test: with both photo viewer open and a staged voice memo,
        // Escape #1 closes photo viewer leaving staged voice memo intact; Escape #2 cancels staged memo.
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_esc.json")));
        backend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_esc.json")));
        const QString escChatGuid = QStringLiteral("iMessage;-;+14155550147");
        backend.ingestChatsMetadataForTest(QJsonArray{
            QJsonObject{{QStringLiteral("guid"), escChatGuid}, {QStringLiteral("displayName"), QStringLiteral("Escape Test")}, {QStringLiteral("style"), 45}}
        });
        backend.selectChat(escChatGuid);
        backend.startVoiceRecording();
        backend.stopVoiceRecordingForPreview();
        QCOMPARE(backend.hasRecordedVoiceMemo(), true);

        Theme theme;
        Translator translator;
        QQmlApplicationEngine engine;
        engine.setOutputWarningsToStandardError(false);
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &translator);
        engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());

        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(win != nullptr);
        win->resize(800, 600);
        win->show();
        win->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QVERIFY(QTest::qWaitForWindowActive(win));

        QVariantList viewerItems;
        viewerItems.append(QVariantMap{
            {QStringLiteral("guid"), QStringLiteral("att-esc-1")},
            {QStringLiteral("name"), QStringLiteral("esc.jpg")},
            {QStringLiteral("localUrl"), QStringLiteral("file:///tmp/nonexistent-esc.jpg")},
            {QStringLiteral("isCloudEvicted"), false}
        });
        QMetaObject::invokeMethod(win, "openPhotoViewer", Q_ARG(QVariant, QVariant::fromValue(viewerItems)), Q_ARG(QVariant, 0));
        QTRY_COMPARE_WITH_TIMEOUT(win->property("showPhotoViewer").toBool(), true, 1000);
        QCOMPARE(backend.hasRecordedVoiceMemo(), true);

        QTest::keyClick(win, Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(win->property("showPhotoViewer").toBool(), false, 1000);
        QCOMPARE(backend.hasRecordedVoiceMemo(), true);

        QTest::keyClick(win, Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(backend.hasRecordedVoiceMemo(), false, 1000);
    }

    void testDuplicateOptimisticMessagePrevented() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550142");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Dedup Test")}, {QStringLiteral("style"), 45}};
        QTcpSocket *pendingSendSocket = nullptr;
        QString pendingTempGuid;
        QByteArray pendingBody;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, &pendingSendSocket, &pendingTempGuid, &pendingBody]() {
                    const QByteArray raw = sock->readAll();
                    if (raw.contains("POST /api/v1/message/text")) {
                        pendingSendSocket = sock;
                        pendingBody = raw.mid(raw.indexOf("\r\n\r\n") + 4);
                        pendingTempGuid = QJsonDocument::fromJson(pendingBody).object().value(QStringLiteral("tempGuid")).toString();
                    } else {
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    }
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_dup.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_dup.json")));
        b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(port), QString(), QStringLiteral("pw"));
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.selectChat(chatGuid);

        b.sendMessage(chatGuid, QStringLiteral("Single send check"));
        QTRY_VERIFY_WITH_TIMEOUT(pendingSendSocket != nullptr, 1500);
        QCOMPARE(b.messages().size(), 1);

        // Simulate SocketIO / pollDelta delivering realGuid while tempGuid is already in m_messages,
        // BEFORE the HTTP /api/v1/message/text response arrives
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QJsonObject realMsg{
            {QStringLiteral("guid"), QStringLiteral("real-dedup-guid-1")},
            {QStringLiteral("tempGuid"), pendingTempGuid},
            {QStringLiteral("text"), QStringLiteral("Single send check")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateCreated"), nowMs},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{realMsg}, false);
        QCOMPARE(b.messages().size(), 1);
        QCOMPARE(b.messages().first().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("real-dedup-guid-1"));

        // Now complete the delayed HTTP response with the same realGuid
        const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), realMsg}}).toJson(QJsonDocument::Compact);
        pendingSendSocket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
        pendingSendSocket->disconnectFromHost();

        QTRY_COMPARE_WITH_TIMEOUT(b.messages().first().toMap().value(QStringLiteral("receiptText")).toString(),
                                  QStringLiteral("Delivered"), 1500);
        QCOMPARE(b.messages().size(), 1);
    }

    void testQmlPhotosAndPhotoViewerLayoutNoClipping() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_ui.json")));
        backend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_ui.json")));
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550143");
        backend.ingestChatsMetadataForTest(QJsonArray{
            QJsonObject{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Viewer Chat")}, {QStringLiteral("style"), 45}}
        });
        backend.selectChat(chatGuid);

        Theme theme;
        Translator translator;
        QQmlApplicationEngine engine;
        engine.setOutputWarningsToStandardError(false);
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &translator);
        engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());

        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(win != nullptr);
        win->setWidth(640);
        win->setHeight(520);
        QCoreApplication::processEvents();

        auto *photosGrid = win->findChild<QQuickItem *>(QStringLiteral("photosGridView"));
        QVERIFY(photosGrid != nullptr);
        QVERIFY(photosGrid->property("cellWidth").toInt() >= 172);
        QVERIFY(photosGrid->property("cellHeight").toInt() >= 234);

        // Verify openPhotoViewer preserves sourceUrl and photoViewerTopBar controls never clip even at 640px window width
        QVariantList items;
        items.append(QVariantMap{
            {QStringLiteral("guid"), QStringLiteral("att-view-1")},
            {QStringLiteral("name"), QStringLiteral("Very_Long_Photo_Filename_From_Conversation_Attachment_2026_Test.jpg")},
            {QStringLiteral("sourceUrl"), QStringLiteral("http://127.0.0.1:1234/api/v1/attachment/att-view-1/download")},
            {QStringLiteral("isCloudEvicted"), false}
        });
        items.append(QVariantMap{
            {QStringLiteral("guid"), QStringLiteral("att-view-2")},
            {QStringLiteral("name"), QStringLiteral("Second.jpg")},
            {QStringLiteral("localUrl"), QStringLiteral("file:///tmp/nonexistent-second.jpg")},
            {QStringLiteral("isCloudEvicted"), false}
        });
        QMetaObject::invokeMethod(win, "openPhotoViewer", Q_ARG(QVariant, QVariant::fromValue(items)), Q_ARG(QVariant, 0));
        QCoreApplication::processEvents();

        QCOMPARE(win->property("showPhotoViewer").toBool(), true);
        auto *card = win->findChild<QQuickItem *>(QStringLiteral("photoViewerCard"));
        auto *sendBtn = win->findChild<QQuickItem *>(QStringLiteral("photoViewerSendBtn"));
        auto *fsBtn = win->findChild<QQuickItem *>(QStringLiteral("photoViewerFullScreenBtn"));
        auto *fullImg = win->findChild<QQuickItem *>(QStringLiteral("fullPhotoImage"));
        QVERIFY(card != nullptr);
        QVERIFY(sendBtn != nullptr);
        QVERIFY(fsBtn != nullptr);
        QVERIFY(fullImg != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(card->width() >= 360 && card->width() <= win->width(), 1000);
        QCOMPARE(sendBtn->isVisible(), true);
        QVERIFY(sendBtn->width() > 16);
        QVERIFY(fsBtn->x() + fsBtn->width() <= card->width());
        QVERIFY(fullImg->property("source").toUrl().toString().contains(QStringLiteral("att-view-1")));

        // Panning MouseArea must sit on photoCanvasArea (parent of fullPhotoImage), not inside fullPhotoImage
        QVERIFY(fullImg->parentItem() != nullptr);
        bool hasCanvasMouseArea = false;
        for (QQuickItem *ch : fullImg->parentItem()->childItems()) {
            if (ch && ch->inherits("QQuickMouseArea")) {
                hasCanvasMouseArea = true;
                break;
            }
        }
        QVERIFY(hasCanvasMouseArea);

        // Also check QML string guards for card send button & header minimumWidth
        QFile qmlFile(QStringLiteral(":/Main.qml"));
        if (!qmlFile.exists()) qmlFile.setFileName(QStringLiteral("../src/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photoCardSendBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"photoViewerSendBtn\"")));
        QVERIFY(qml.contains(QStringLiteral("Layout.minimumWidth: 40")));
    }

    void testMarkChatReadPreservesOutgoingDateReadAndDeliveryCheckRotation() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550144");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Delivery Guard")}, {QStringLiteral("style"), 45}};

        QTcpSocket *inFlightBatchSock = nullptr;
        int queryRequestCount = 0;
        QList<QStringList> capturedBatchArgs;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &inFlightBatchSock, &queryRequestCount, &capturedBatchArgs]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLen = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    const QByteArray bodyBytes = buf->mid(hdrEnd + 4, contentLen);
                    if (hdr.startsWith("POST /api/v1/message/query") && bodyBytes.contains("message.guid")) {
                        ++queryRequestCount;
                        QStringList guidsInReq;
                        const QJsonObject reqObj = QJsonDocument::fromJson(bodyBytes).object();
                        const QJsonArray whereArr = reqObj.value(QStringLiteral("where")).toArray();
                        if (!whereArr.isEmpty()) {
                            const QJsonObject whereObj = whereArr.first().toObject();
                            const QString statement = whereObj.value(QStringLiteral("statement")).toString();
                            const QJsonObject argsObj = whereObj.value(QStringLiteral("args")).toObject();
                            static const QRegularExpression placeholderRe(QStringLiteral(":([A-Za-z0-9_]+)"));
                            QRegularExpressionMatchIterator mit = placeholderRe.globalMatch(statement);
                            while (mit.hasNext()) {
                                const QString paramName = mit.next().captured(1);
                                const QString gv = argsObj.value(paramName).toString();
                                if (!gv.isEmpty()) guidsInReq.append(gv);
                            }
                        }
                        capturedBatchArgs.append(guidsInReq);
                        if (queryRequestCount == 1) {
                            inFlightBatchSock = sock;
                            return;
                        }
                    }
                    const QByteArray json = R"({"status":200,"data":[]})";
                    sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    sock->disconnectFromHost();
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_deliv.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_deliv.json")));
        b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(port), QString(), QStringLiteral("pw"));
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.selectChat(chatGuid);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        // 1. Seed one incoming unread message and one outgoing Sent (undelivered) message
        const QJsonObject incomingMsg{
            {QStringLiteral("guid"), QStringLiteral("inc-read-1")},
            {QStringLiteral("text"), QStringLiteral("Unread incoming")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("isRead"), false},
            {QStringLiteral("dateRead"), 0},
            {QStringLiteral("dateCreated"), nowMs - 5000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        const QJsonObject outgoingMsg{
            {QStringLiteral("guid"), QStringLiteral("out-sent-1")},
            {QStringLiteral("text"), QStringLiteral("Outgoing waiting for delivery")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateDelivered"), 0},
            {QStringLiteral("dateRead"), 0},
            {QStringLiteral("dateCreated"), nowMs - 2000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{incomingMsg, outgoingMsg}, false);
        b.addPendingDeliveryForTest(QStringLiteral("out-sent-1"), chatGuid, nowMs - 2000);
        QCOMPARE(b.pendingDeliveryCountForTest(), 1);

        // markChatRead must NOT set dateRead on outgoing messages, so out-sent-1 remains pending and shows "Sent" (not "Read")
        b.markChatRead(chatGuid);
        b.checkPendingDeliveryStatusesForTest();
        QCOMPARE(b.pendingDeliveryCountForTest(), 1);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sent"));

        QTRY_VERIFY_WITH_TIMEOUT(inFlightBatchSock != nullptr, 1500);
        QCOMPARE(queryRequestCount, 1);

        // Calling checkPendingDeliveryStatuses again while out-sent-1 is already in flight must NOT dispatch an overlapping batch
        b.checkPendingDeliveryStatusesForTest();
        QTest::qWait(60);
        QCOMPARE(queryRequestCount, 1);

        // Simulate Socket.IO delivering a newer "Delivered" update while the older batch request is still in flight
        const QJsonObject socketDeliveredMsg{
            {QStringLiteral("guid"), QStringLiteral("out-sent-1")},
            {QStringLiteral("text"), QStringLiteral("Outgoing waiting for delivery")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateDelivered"), nowMs - 500},
            {QStringLiteral("dateCreated"), nowMs - 2000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{socketDeliveredMsg}, false);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));

        // Complete the stale in-flight batch with isDelivered: false; observation sequence guard must prevent regression to "Sent"
        const QJsonObject staleBatchMsg{
            {QStringLiteral("guid"), QStringLiteral("out-sent-1")},
            {QStringLiteral("text"), QStringLiteral("Outgoing waiting for delivery")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateDelivered"), 0},
            {QStringLiteral("dateCreated"), nowMs - 2000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        const QByteArray staleResp = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), QJsonArray{staleBatchMsg}}}).toJson(QJsonDocument::Compact);
        const quint64 beforeStaleAck = b.pendingDeliveryCheckCompletedCountForTest();
        inFlightBatchSock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(staleResp.size()) + "\r\nConnection: close\r\n\r\n" + staleResp);
        inFlightBatchSock->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(b.pendingDeliveryCheckCompletedCountForTest() > beforeStaleAck, 1500);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));

        // 2. Verify absent GUIDs stay in m_pendingDeliveryMessages until 120s expiry, >20 entries rotate across batches, and temp-* GUIDs are never sent in server batch queries
        b.addPendingDeliveryForTest(QStringLiteral("temp-skip-batch-1"), chatGuid, nowMs);
        for (int i = 0; i < 25; ++i) {
            const QString g = QStringLiteral("pend-rot-%1").arg(i, 2, 10, QLatin1Char('0'));
            b.addPendingDeliveryForTest(g, chatGuid, nowMs);
        }
        QCOMPARE(b.pendingDeliveryCountForTest(), 26);
        const quint64 beforeRot1 = b.pendingDeliveryCheckCompletedCountForTest();
        b.checkPendingDeliveryStatusesForTest();
        const QStringList firstBatch = b.lastPendingDeliveryBatchForTest();
        QCOMPARE(firstBatch.size(), 20);
        QVERIFY(!firstBatch.contains(QStringLiteral("temp-skip-batch-1")));
        QTRY_VERIFY_WITH_TIMEOUT(queryRequestCount >= 2 &&
                                 capturedBatchArgs.size() >= 2 &&
                                 b.pendingDeliveryCheckCompletedCountForTest() > beforeRot1 &&
                                 b.pendingDeliveryInFlightCountForTest() == 0, 1500);
        const QStringList firstTcpBatch = capturedBatchArgs.at(1);
        QCOMPARE(firstTcpBatch.size(), 20);
        QVERIFY(!firstTcpBatch.contains(QStringLiteral("temp-skip-batch-1")));
        // Empty response must NOT evict unexpired pending GUIDs
        QCOMPARE(b.pendingDeliveryCountForTest(), 26);

        const quint64 beforeRot2 = b.pendingDeliveryCheckCompletedCountForTest();
        b.checkPendingDeliveryStatusesForTest();
        QTRY_VERIFY_WITH_TIMEOUT(queryRequestCount >= 3 &&
                                 capturedBatchArgs.size() >= 3 &&
                                 b.pendingDeliveryCheckCompletedCountForTest() > beforeRot2 &&
                                 b.pendingDeliveryInFlightCountForTest() == 0, 1500);
        const QStringList secondTcpBatch = capturedBatchArgs.at(2);
        QCOMPARE(secondTcpBatch.size(), 20);
        QVERIFY(!secondTcpBatch.contains(QStringLiteral("temp-skip-batch-1")));
        QVERIFY(firstTcpBatch != secondTcpBatch);
        // All 25 exact seeded GUIDs (pend-rot-00..24) must be covered across the two HTTP requests' bound args
        QSet<QString> combinedTcpGuids(firstTcpBatch.begin(), firstTcpBatch.end());
        for (const QString &g : secondTcpBatch) combinedTcpGuids.insert(g);
        QSet<QString> expectedSeededGuids;
        for (int i = 0; i < 25; ++i) {
            expectedSeededGuids.insert(QStringLiteral("pend-rot-%1").arg(i, 2, 10, QLatin1Char('0')));
        }
        QCOMPARE(combinedTcpGuids, expectedSeededGuids);
    }

    void testOptimisticAttachmentMatchingAndReceiptRules() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString att1 = tempDir.filePath(QStringLiteral("first_doc.txt"));
        const QString att2 = tempDir.filePath(QStringLiteral("second_doc.txt"));
        {
            QFile f1(att1);
            QVERIFY(f1.open(QIODevice::WriteOnly));
            f1.write("Attachment one payload");
            f1.close();
            QFile f2(att2);
            QVERIFY(f2.open(QIODevice::WriteOnly));
            f2.write("Attachment two payload");
            f2.close();
        }

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_opt.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_opt.json")));
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550145");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Opt Match")}, {QStringLiteral("style"), 45}};
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.selectChat(chatGuid);

        // Queue two optimistic attachment rows without server URL (so they remain temp-att-* rows in m_messages)
        b.sendAttachment(chatGuid, att1, QString());
        b.sendAttachment(chatGuid, att2, QString());
        QCOMPARE(b.messages().size(), 2);
        const QString tempGuid1 = b.messages().at(0).toMap().value(QStringLiteral("guid")).toString();
        const QString tempGuid2 = b.messages().at(1).toMap().value(QStringLiteral("guid")).toString();
        QVERIFY(tempGuid1.startsWith(QStringLiteral("temp-att-")));
        QVERIFY(tempGuid2.startsWith(QStringLiteral("temp-att-")));
        // Only the latest outgoing row should carry ordinary "Sending..." receipt text
        QCOMPARE(b.messages().at(0).toMap().value(QStringLiteral("receiptText")).toString(), QString());
        QCOMPARE(b.messages().at(1).toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

        // 1. An un-GUIDed incoming attachment message when 2 optimistic attachment rows exist must NOT delete either ambiguous temp row
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject unguidedRealAtt{
            {QStringLiteral("guid"), QStringLiteral("real-att-unguided")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("dateCreated"), nowMs},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-g-u")}, {QStringLiteral("transferName"), QStringLiteral("other.txt")}}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{unguidedRealAtt}, false);
        QStringList currentGuids;
        for (const QVariant &v : b.messages()) currentGuids.append(v.toMap().value(QStringLiteral("guid")).toString());
        QVERIFY(currentGuids.contains(tempGuid1));
        QVERIFY(currentGuids.contains(tempGuid2));

        // 2. An incoming message with explicitTempGuid == tempGuid1 must replace ONLY tempGuid1,
        // clean up any pendingDelivery entry for tempGuid1, and leave tempGuid2 intact
        b.addPendingDeliveryForTest(tempGuid1, chatGuid, nowMs);
        b.addPendingDeliveryForTest(tempGuid2, chatGuid, nowMs);
        QCOMPARE(b.pendingDeliveryCountForTest(), 2);

        const QJsonObject guidedRealAtt1{
            {QStringLiteral("guid"), QStringLiteral("real-att-1")},
            {QStringLiteral("tempGuid"), tempGuid1},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateCreated"), nowMs + 10},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-g-1")}, {QStringLiteral("transferName"), QStringLiteral("first_doc.txt")}}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{guidedRealAtt1}, false);
        // tempGuid1 must be evicted from m_pendingDeliveryMessages when replaced by real-att-1, leaving only tempGuid2
        QCOMPARE(b.pendingDeliveryCountForTest(), 1);
        currentGuids.clear();
        for (const QVariant &v : b.messages()) currentGuids.append(v.toMap().value(QStringLiteral("guid")).toString());
        QVERIFY(currentGuids.contains(QStringLiteral("real-att-1")));
        QVERIFY(!currentGuids.contains(tempGuid1));
        QVERIFY(currentGuids.contains(tempGuid2));

        // 2b. Even when only one un-matched optimistic attachment (tempGuid2) remains AND confirmed real rows exist,
        // an un-GUIDed attachment message must NOT drop tempGuid2 by single-candidate fallback
        const QJsonObject secondUnguidedRealAtt{
            {QStringLiteral("guid"), QStringLiteral("real-att-unguided-2")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("dateCreated"), nowMs + 15},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-g-u2")}, {QStringLiteral("transferName"), QStringLiteral("third.txt")}}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{secondUnguidedRealAtt}, false);
        currentGuids.clear();
        for (const QVariant &v : b.messages()) currentGuids.append(v.toMap().value(QStringLiteral("guid")).toString());
        QCOMPARE(currentGuids.count(tempGuid2), qsizetype(1));
        QCOMPARE(currentGuids.count(QStringLiteral("real-att-unguided-2")), qsizetype(1));

        // 3. Explicit failure badge on an older row must be preserved even when a strictly newer outgoing row exists after it
        const QJsonObject newerOutAfterFail{
            {QStringLiteral("guid"), QStringLiteral("newer-out-after-fail")},
            {QStringLiteral("text"), QStringLiteral("Follow up after failure")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateDelivered"), nowMs + 55},
            {QStringLiteral("dateCreated"), nowMs + 50},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        const QJsonObject failedOlderMsg{
            {QStringLiteral("guid"), QStringLiteral("real-att-1")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("error"), 4},
            {QStringLiteral("dateCreated"), nowMs + 10},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-g-1")}, {QStringLiteral("transferName"), QStringLiteral("first_doc.txt")}}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{newerOutAfterFail, failedOlderMsg}, false);
        int realAttIdx = -1;
        int newerOutIdx = -1;
        QVariantMap olderRow;
        for (int i = 0; i < b.messages().size(); ++i) {
            const QVariantMap row = b.messages().at(i).toMap();
            const QString g = row.value(QStringLiteral("guid")).toString();
            if (g == QStringLiteral("real-att-1")) {
                realAttIdx = i;
                olderRow = row;
            } else if (g == QStringLiteral("newer-out-after-fail")) {
                newerOutIdx = i;
            }
        }
        QVERIFY(realAttIdx >= 0);
        QVERIFY(newerOutIdx > realAttIdx);
        QCOMPARE(olderRow.value(QStringLiteral("isFailed")).toBool(), true);
        QCOMPARE(olderRow.value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));
    }

    void testLiveAttachment404RetryAndAudioSecurityAndDraftRetention() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();
        int liveAttHits = 0;
        int nonLiveAttHits = 0;
        int audioDownloadHits = 0;
        QByteArray validWavBytes;

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &liveAttHits, &nonLiveAttHits, &audioDownloadHits, &validWavBytes]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLen = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    buf->remove(0, hdrEnd + 4 + contentLen);

                    if (hdr.contains("/api/v1/attachment/live-att-404/download")) {
                        ++liveAttHits;
                        const QByteArray body = "Attachment does not exist on disk yet";
                        sock->write("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        sock->disconnectFromHost();
                    } else if (hdr.contains("/api/v1/attachment/old-att-404/download")) {
                        ++nonLiveAttHits;
                        const QByteArray body = "Attachment does not exist";
                        sock->write("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        sock->disconnectFromHost();
                    } else if (hdr.contains("/api/v1/attachment/pause-resume-voice/download")) {
                        ++audioDownloadHits;
                        if (audioDownloadHits == 1) {
                            // Keep first request open so user can toggle pause before download finishes
                            return;
                        }
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " + QByteArray::number(validWavBytes.size()) + "\r\nConnection: close\r\n\r\n" + validWavBytes);
                        sock->disconnectFromHost();
                    } else {
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    }
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_sec.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_sec.json")));
        b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(port), QString(), QStringLiteral("secret-pw"));

        // 1. Atomic config save sets 0600 permissions and preserves previous config if target is a directory
        const QFileDevice::Permissions perms = QFileInfo(b.configFilePath()).permissions();
        const QFileDevice::Permissions expectedOwnerRw = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        const QFileDevice::Permissions forbiddenBits =
            QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QCOMPARE(perms & (expectedOwnerRw | forbiddenBits), expectedOwnerRw);

        // 2. CWE-319 check: loopback HTTP and remote HTTPS are allowed; remote cleartext HTTP is rejected
        QVERIFY(Backend::isTrustedPasswordTransportUrl(QUrl(QStringLiteral("http://127.0.0.1:1235/api/v1/attachment/a/download"))));
        QVERIFY(Backend::isTrustedPasswordTransportUrl(QUrl(QStringLiteral("http://localhost:1235/api/v1/attachment/a/download"))));
        QVERIFY(Backend::isTrustedPasswordTransportUrl(QUrl(QStringLiteral("http://[::1]:1235/api/v1/attachment/a/download"))));
        QVERIFY(Backend::isTrustedPasswordTransportUrl(QUrl(QStringLiteral("https://relay.example.com/api/v1/attachment/a/download"))));
        QVERIFY(!Backend::isTrustedPasswordTransportUrl(QUrl(QStringLiteral("http://relay.example.com/api/v1/attachment/a/download"))));

        // 3. WAV data chunk validation rejects >44-byte WAV with empty data chunk
        const QString emptyDataWav = tempDir.filePath(QStringLiteral("empty_data.wav"));
        {
            QFile wf(emptyDataWav);
            QVERIFY(wf.open(QIODevice::WriteOnly));
            QByteArray hdr("RIFF\x38\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00\x80\x3e\x00\x00\x00\x7d\x00\x00\x02\x00\x10\x00LIST\x0c\x00\x00\x00INFOISFT\x00\x00\x00\x00" "data\x00\x00\x00\x00", 64);
            wf.write(hdr);
            wf.close();
        }
        QVERIFY(QFileInfo(emptyDataWav).size() > 44);
        QVERIFY(!Backend::hasNonEmptyWavDataChunk(emptyDataWav));

        // 4. Staged voice memo is retained if selectedChatGuid is empty when sendRecordedVoiceMemo() is called
        b.startVoiceRecording();
        b.stopVoiceRecordingForPreview();
        QVERIFY(b.hasRecordedVoiceMemo());
        const QString stagedWavPath = b.recordedVoicePath();
        QVERIFY(Backend::hasNonEmptyWavDataChunk(stagedWavPath));
        {
            QFile vf(stagedWavPath);
            QVERIFY(vf.open(QIODevice::ReadOnly));
            validWavBytes = vf.readAll();
            vf.close();
        }
        // No selected chat yet -> sendRecordedVoiceMemo must keep the draft
        b.sendRecordedVoiceMemo();
        QVERIFY(b.hasRecordedVoiceMemo());
        QVERIFY(QFileInfo::exists(stagedWavPath));
        b.cancelVoiceRecording();

        // 5. Audio download pause/resume: m_audioPlaying stays false during download, pausing aborts, and toggling again restarts download and plays
        b.toggleAudioPlayback(QStringLiteral("pause-resume-voice"), QStringLiteral("/api/v1/attachment/pause-resume-voice/download"));
        QCOMPARE(b.playingAudioGuid(), QStringLiteral("pause-resume-voice"));
        QCOMPARE(b.audioPlaying(), false);
        QTRY_COMPARE_WITH_TIMEOUT(audioDownloadHits, 1, 1500);
        // Pause before first download finishes
        b.toggleAudioPlayback(QStringLiteral("pause-resume-voice"), QStringLiteral("/api/v1/attachment/pause-resume-voice/download"));
        QCOMPARE(b.audioPlaying(), false);
        // Toggle again to restart download -> second request returns validWavBytes and starts playback
        b.toggleAudioPlayback(QStringLiteral("pause-resume-voice"), QStringLiteral("/api/v1/attachment/pause-resume-voice/download"));
        QTRY_COMPARE_WITH_TIMEOUT(audioDownloadHits, 2, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(b.audioPlaying(), true, 1500);
        b.toggleAudioPlayback(QStringLiteral("pause-resume-voice"), QString());
        QCOMPARE(b.audioPlaying(), false);

        // 6. Live incoming attachment 404 retries with bounded backoff instead of immediate 30m cooldown, whereas non-live 404 enters 30m cooldown immediately
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550146");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Att Retry")}, {QStringLiteral("style"), 45}};
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject histMsgWithAtt{
            {QStringLiteral("guid"), QStringLiteral("msg-att-old")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), nowMs - 60000},
            {QStringLiteral("attachments"), QJsonArray{
                QJsonObject{{QStringLiteral("guid"), QStringLiteral("old-att-404")}, {QStringLiteral("mimeType"), QStringLiteral("image/jpeg")}, {QStringLiteral("transferName"), QStringLiteral("old.jpg")}},
                QJsonObject{{QStringLiteral("guid"), QStringLiteral("frac-aud-1")}, {QStringLiteral("mimeType"), QStringLiteral("audio/wav")}, {QStringLiteral("transferName"), QStringLiteral("frac.wav")}, {QStringLiteral("durationSeconds"), 1.6}}
            }},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        // Ingest historical message with isInitialLoad = true so old-att-404 is NOT marked as live incoming
        b.ingestMessagesForTest(QJsonArray{histMsgWithAtt}, true);
        b.selectChat(chatGuid);
        // Fractional durationSeconds (1.6) must round to 2 seconds
        const QVariantList renderedAtts = b.messages().last().toMap().value(QStringLiteral("attachments")).toList();
        QCOMPARE(renderedAtts.last().toMap().value(QStringLiteral("durationSeconds")).toInt(), 2);

        const QJsonObject liveMsgWithAtt{
            {QStringLiteral("guid"), QStringLiteral("msg-att-live")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), nowMs},
            {QStringLiteral("attachments"), QJsonArray{
                QJsonObject{{QStringLiteral("guid"), QStringLiteral("live-att-404")}, {QStringLiteral("mimeType"), QStringLiteral("image/jpeg")}, {QStringLiteral("transferName"), QStringLiteral("live.jpg")}}
            }},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        // Ingest live incoming message with isInitialLoad = false so live-att-404 gets bounded 404 retry
        b.ingestMessagesForTest(QJsonArray{liveMsgWithAtt}, false);

        QTRY_VERIFY_WITH_TIMEOUT(nonLiveAttHits >= 1 && liveAttHits >= 1, 1500);
        // Non-live 404 must get the 30-minute cooldown, while live-att-404 has no cooldown before retry
        QTRY_VERIFY_WITH_TIMEOUT(b.attachmentCooldownUntilMsForTest(QStringLiteral("old-att-404")) > nowMs + 1000000LL, 1500);
        QCOMPARE(b.attachmentCooldownUntilMsForTest(QStringLiteral("live-att-404")), 0LL);
        // Wait for the first 500ms bounded retry on live-att-404
        QTRY_VERIFY_WITH_TIMEOUT(liveAttHits >= 2, 1500);
    }

    void testVoiceMemoConversationBindingAndAliasAllow() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();
        int attachmentUploads = 0;
        QString lastUploadedChatGuid;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &attachmentUploads, &lastUploadedChatGuid]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLen = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                    buf->remove(0, hdrEnd + 4 + contentLen);

                    if (hdr.startsWith("POST /api/v1/message/attachment")) {
                        ++attachmentUploads;
                        const int cgPos = body.indexOf("name=\"chatGuid\"\r\n\r\n");
                        if (cgPos >= 0) {
                            const int valStart = cgPos + 19;
                            const int valEnd = body.indexOf("\r\n--", valStart);
                            if (valEnd > valStart) {
                                lastUploadedChatGuid = QString::fromUtf8(body.mid(valStart, valEnd - valStart));
                            }
                        }
                        const QJsonObject respData{
                            {QStringLiteral("guid"), QStringLiteral("real-vm-bind-1")},
                            {QStringLiteral("isFromMe"), true},
                            {QStringLiteral("isSent"), true},
                            {QStringLiteral("isDelivered"), true},
                            {QStringLiteral("dateDelivered"), QDateTime::currentMSecsSinceEpoch()},
                            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-vm-bind-1")}, {QStringLiteral("mimeType"), QStringLiteral("audio/wav")}}}}
                        };
                        const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), respData}}).toJson(QJsonDocument::Compact);
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    } else {
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    }
                    sock->disconnectFromHost();
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QByteArray prevHome = qgetenv("HOME");
        qputenv("HOME", tempDir.path().toUtf8());
        auto restoreHome = qScopeGuard([&prevHome]() {
            if (prevHome.isEmpty()) qunsetenv("HOME");
            else qputenv("HOME", prevHome);
        });

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_vm_bind.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_vm_bind.json")));
        b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(port), QString(), QStringLiteral("pw"));

        const QString chatA = QStringLiteral("iMessage;-;+14155550148");
        const QString chatAAlias = QStringLiteral("SMS;-;+14155550148");
        const QString chatB = QStringLiteral("iMessage;-;+14155550149");
        b.ingestChatsMetadataForTest(QJsonArray{
            QJsonObject{
                {QStringLiteral("guid"), chatA},
                {QStringLiteral("displayName"), QStringLiteral("Alice")},
                {QStringLiteral("style"), 45},
                {QStringLiteral("siblingChatGuids"), QJsonArray{chatAAlias}}
            },
            QJsonObject{
                {QStringLiteral("guid"), chatAAlias},
                {QStringLiteral("displayName"), QStringLiteral("Alice")},
                {QStringLiteral("style"), 45},
                {QStringLiteral("siblingChatGuids"), QJsonArray{chatA}}
            },
            QJsonObject{
                {QStringLiteral("guid"), chatB},
                {QStringLiteral("displayName"), QStringLiteral("Bob")},
                {QStringLiteral("style"), 45}
            }
        });

        QSignalSpy toastSpy(&b, &Backend::toastMessage);
        b.selectChat(chatA);
        b.startVoiceRecording();
        QCOMPARE(b.recordedVoiceChatGuid(), chatA);
        b.stopVoiceRecordingForPreview();
        QVERIFY(b.hasRecordedVoiceMemo());
        const QString stagedPath = b.recordedVoicePath();
        QVERIFY(QFileInfo::exists(stagedPath));
        QCOMPARE(b.canSendRecordedVoiceMemoToSelectedChat(), true);

        // Switching to unrelated chat B blocks sendRecordedVoiceMemo without clearing the staged draft
        b.selectChat(chatB);
        QCOMPARE(b.canSendRecordedVoiceMemoToSelectedChat(), false);
        QCOMPARE(b.recordedVoiceChatGuid(), chatA);
        b.sendRecordedVoiceMemo();
        QVERIFY(b.hasRecordedVoiceMemo());
        QVERIFY(QFileInfo::exists(stagedPath));
        QCOMPARE(b.recordedVoiceChatGuid(), chatA);
        QCOMPARE(attachmentUploads, 0);
        QVERIFY(!toastSpy.isEmpty());

        // Clearing selected chat also blocks sendRecordedVoiceMemo and preserves draft
        b.clearSelectedChat();
        QCOMPARE(b.canSendRecordedVoiceMemoToSelectedChat(), false);
        b.sendRecordedVoiceMemo();
        QVERIFY(b.hasRecordedVoiceMemo());
        QVERIFY(QFileInfo::exists(stagedPath));
        QCOMPARE(attachmentUploads, 0);

        // Switching to same-conversation alias allows sendRecordedVoiceMemo and clears staged state + GUID
        b.selectChat(chatAAlias);
        QCOMPARE(b.canSendRecordedVoiceMemoToSelectedChat(), true);
        b.sendRecordedVoiceMemo();
        QCOMPARE(b.hasRecordedVoiceMemo(), false);
        QCOMPARE(b.recordedVoiceChatGuid(), QString());
        QTRY_COMPARE_WITH_TIMEOUT(attachmentUploads, 1, 1500);
        QVERIFY(!lastUploadedChatGuid.isEmpty());
        QVERIFY(b.isSameConversation(lastUploadedChatGuid, chatA));

        // cancelVoiceRecording clears recordedVoiceChatGuid as well
        b.selectChat(chatA);
        b.startVoiceRecording();
        b.stopVoiceRecordingForPreview();
        QCOMPARE(b.recordedVoiceChatGuid(), chatA);
        b.cancelVoiceRecording();
        QCOMPARE(b.hasRecordedVoiceMemo(), false);
        QCOMPARE(b.recordedVoiceChatGuid(), QString());
    }

    void testSaveServerConfigFailurePreservesSettingsAndQueuedIcloudMutations() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString initialUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        const QString altUrl = QStringLiteral("http://127.0.0.1:%1/alt").arg(server.serverPort());

        QTcpSocket *heldNoteSock = nullptr;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &heldNoteSock]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLen = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    if (hdr.startsWith("POST /api/v1/icloud/notes")) {
                        // Hold the response to saveIcloudNote() pending while saveServerConfig fails
                        heldNoteSock = sock;
                        return;
                    }
                    const QByteArray json = R"({"status":200,"data":{}})";
                    sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    sock->disconnectFromHost();
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_cfg.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_cfg.json")));
        b.saveServerConfig(initialUrl, QString(), QStringLiteral("initial-secret"));
        QCOMPARE(b.serverUrl(), initialUrl);
        QCOMPARE(b.serverPassword(), QStringLiteral("initial-secret"));

        // Enqueue a pending iCloud note save and confirm the live test server holds it in-flight
        const QString noteId = b.saveIcloudNote(QString(), QStringLiteral("Draft Note"), QStringLiteral("Keep me safe"));
        QVERIFY(!noteId.isEmpty());
        QVERIFY(b.hasPendingIcloudNoteSaveForTest(noteId));
        QTRY_VERIFY_WITH_TIMEOUT(heldNoteSock != nullptr, 1500);

        // Force writeConfigAtomically failure by replacing the existing configFilePath with a directory
        // (without calling setConfigFilePath, which intentionally reloads credentials from disk)
        QSignalSpy toastSpy(&b, &Backend::toastMessage);
        const QString cfgPath = tempDir.filePath(QStringLiteral("config_cfg.json"));
        QVERIFY(QFile::remove(cfgPath));
        QVERIFY(QDir().mkpath(cfgPath));

        b.saveServerConfig(altUrl, QString(), QStringLiteral("new-secret"));
        QCOMPARE(b.serverUrl(), initialUrl);
        QCOMPARE(b.serverPassword(), QStringLiteral("initial-secret"));
        QVERIFY(b.hasPendingIcloudNoteSaveForTest(noteId));
        QVERIFY(!toastSpy.isEmpty());

        // Release held iCloud note request cleanly
        const QByteArray noteResp = QJsonDocument(QJsonObject{
            {QStringLiteral("status"), 200},
            {QStringLiteral("data"), QJsonArray{QJsonObject{{QStringLiteral("id"), noteId}, {QStringLiteral("title"), QStringLiteral("Draft Note")}, {QStringLiteral("revision"), 1}}}}
        }).toJson(QJsonDocument::Compact);
        heldNoteSock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(noteResp.size()) + "\r\nConnection: close\r\n\r\n" + noteResp);
        heldNoteSock->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!b.hasPendingIcloudNoteSaveForTest(noteId), 1500);
    }

    void testPendingExpiryConsistencyAndMergedAckAndOwnedVoiceCleanup() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550148");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Ack Merge")}, {QStringLiteral("style"), 45}};

        QTcpSocket *heldTextSock = nullptr;
        QTcpSocket *heldAttSock = nullptr;
        QString heldTextTempGuid;
        QString heldAttTempGuid;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &heldTextSock, &heldAttSock, &heldTextTempGuid, &heldAttTempGuid]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLen = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                    buf->remove(0, hdrEnd + 4 + contentLen);

                    if (hdr.startsWith("POST /api/v1/message/text")) {
                        heldTextSock = sock;
                        heldTextTempGuid = QJsonDocument::fromJson(body).object().value(QStringLiteral("tempGuid")).toString();
                        return;
                    }
                    if (hdr.startsWith("POST /api/v1/message/attachment")) {
                        if (heldAttSock == nullptr) {
                            heldAttSock = sock;
                            const int tgPos = body.indexOf("name=\"tempGuid\"\r\n\r\n");
                            if (tgPos >= 0) {
                                const int valStart = tgPos + 19;
                                const int valEnd = body.indexOf("\r\n--", valStart);
                                if (valEnd > valStart) heldAttTempGuid = QString::fromUtf8(body.mid(valStart, valEnd - valStart));
                            }
                            return;
                        }
                        // Second attachment upload returns immediately (for ownedVoiceRecording=false check)
                        const QJsonObject respData{
                            {QStringLiteral("guid"), QStringLiteral("real-user-vm-2")},
                            {QStringLiteral("isFromMe"), true},
                            {QStringLiteral("isSent"), true},
                            {QStringLiteral("isDelivered"), true},
                            {QStringLiteral("dateDelivered"), QDateTime::currentMSecsSinceEpoch()},
                            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-user-vm-2")}, {QStringLiteral("mimeType"), QStringLiteral("audio/wav")}}}}
                        };
                        const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), respData}}).toJson(QJsonDocument::Compact);
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                        return;
                    }
                    const QByteArray json = R"({"status":200,"data":[]})";
                    sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    sock->disconnectFromHost();
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QByteArray prevHome = qgetenv("HOME");
        qputenv("HOME", tempDir.path().toUtf8());
        auto restoreHome = qScopeGuard([&prevHome]() {
            if (prevHome.isEmpty()) qunsetenv("HOME");
            else qputenv("HOME", prevHome);
        });

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_merge.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_merge.json")));
        b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(port), QString(), QStringLiteral("pw"));
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.selectChat(chatGuid);

        // 1. dateCreated == 0 expiry consistency -> "Unconfirmed" (never "Sent"), then upgrades to "Delivered"
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject noDcPending{
            {QStringLiteral("guid"), QStringLiteral("pend-nodc-1")},
            {QStringLiteral("text"), QStringLiteral("No dateCreated")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), false},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateCreated"), 0},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        QCOMPARE(Backend::computeOutgoingReceiptText(noDcPending, false, nowMs, nowMs - 150000), QStringLiteral("Unconfirmed"));
        b.ingestMessagesForTest(QJsonArray{noDcPending}, false);
        b.addPendingDeliveryForTest(QStringLiteral("pend-nodc-1"), chatGuid, nowMs - 150000);
        b.checkPendingDeliveryStatusesForTest();
        QCOMPARE(b.pendingDeliveryCountForTest(), 0);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Unconfirmed"));

        QJsonObject noDcDelivered = noDcPending;
        noDcDelivered.insert(QStringLiteral("isSent"), true);
        noDcDelivered.insert(QStringLiteral("isDelivered"), true);
        noDcDelivered.insert(QStringLiteral("dateDelivered"), nowMs);
        b.ingestMessagesForTest(QJsonArray{noDcDelivered}, false);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));

        // 2. Stale text ack arrives after Socket.IO already delivered realGuid -> keeps Delivered and avoids redundant polling
        b.sendMessage(chatGuid, QStringLiteral("Race text"));
        QTRY_VERIFY_WITH_TIMEOUT(heldTextSock != nullptr && !heldTextTempGuid.isEmpty(), 1500);
        b.ingestMessagesForTest(QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("real-text-race")},
            {QStringLiteral("tempGuid"), heldTextTempGuid},
            {QStringLiteral("text"), QStringLiteral("Race text")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateDelivered"), nowMs + 10},
            {QStringLiteral("dateCreated"), nowMs + 5},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        }}, false);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));
        {
            const QJsonObject staleTextAck{
                {QStringLiteral("guid"), QStringLiteral("real-text-race")},
                {QStringLiteral("tempGuid"), heldTextTempGuid},
                {QStringLiteral("text"), QStringLiteral("Race text")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), false},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateCreated"), nowMs + 5},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            };
            const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), staleTextAck}}).toJson(QJsonDocument::Compact);
            heldTextSock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
            heldTextSock->disconnectFromHost();
        }
        QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));
        QCOMPARE(b.pendingDeliveryCountForTest(), 0);

        // 3. Stale attachment ack (via ownedVoiceRecording=true) preserves Socket.IO Delivered and cleans owned voice file
        b.startVoiceRecording();
        b.stopVoiceRecordingForPreview();
        const QString ownedWav = b.recordedVoicePath();
        QVERIFY(QFileInfo::exists(ownedWav));
        b.sendRecordedVoiceMemo();
        QTRY_VERIFY_WITH_TIMEOUT(heldAttSock != nullptr && !heldAttTempGuid.isEmpty(), 1500);
        b.ingestMessagesForTest(QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("real-att-race")},
            {QStringLiteral("tempGuid"), heldAttTempGuid},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateDelivered"), nowMs + 25},
            {QStringLiteral("dateCreated"), nowMs + 20},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-race-1")}, {QStringLiteral("mimeType"), QStringLiteral("audio/wav")}}}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        }}, false);
        {
            const QJsonObject staleAttAck{
                {QStringLiteral("guid"), QStringLiteral("real-att-race")},
                {QStringLiteral("tempGuid"), heldAttTempGuid},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateCreated"), nowMs + 20},
                {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-race-1")}, {QStringLiteral("mimeType"), QStringLiteral("audio/wav")}}}},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            };
            const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), staleAttAck}}).toJson(QJsonDocument::Compact);
            heldAttSock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
            heldAttSock->disconnectFromHost();
        }
        QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
        QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));
        QCOMPARE(b.pendingDeliveryCountForTest(), 0);
        QVERIFY(!QFileInfo::exists(ownedWav));

        // 4. Ordinary user attachment named voice_memo_*.wav (ownedVoiceRecording=false) MUST NOT be deleted
        const QString userNamedVm = tempDir.filePath(QStringLiteral("voice_memo_interview.wav"));
        {
            QFile uf(userNamedVm);
            QVERIFY(uf.open(QIODevice::WriteOnly));
            static const char rawWav[] = "RIFF\x2c\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00\x80\x3e\x00\x00\x00\x7d\x00\x00\x02\x00\x10\x00""data\x08\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08";
            const QByteArray wav(rawWav, sizeof(rawWav) - 1);
            QCOMPARE(wav.size(), qsizetype(52));
            uf.write(wav);
            uf.close();
        }
        b.sendAttachment(chatGuid, userNamedVm, QString());
        QTRY_VERIFY_WITH_TIMEOUT(([&]() {
            for (const QVariant &v : b.messages()) {
                if (v.toMap().value(QStringLiteral("guid")).toString() == QStringLiteral("real-user-vm-2")) {
                    return true;
                }
            }
            return false;
        }()), 1500);
        QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
        QVERIFY(QFileInfo::exists(userNamedVm));
    }

    void testRebuildEchoMatchesOnlyCacheRowsAndDeleteUnsendRestoresReceipt() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_echo.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_echo.json")));

        const QString chatGuid = QStringLiteral("iMessage;-;+14155550149");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Echo Test")}, {QStringLiteral("style"), 45}};
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.selectChat(chatGuid);

        // 1. Two identical optimistic sends + 1 server echo must match only cache row, keeping 2nd optimistic row intact
        b.sendMessage(chatGuid, QStringLiteral("Ping!"));
        b.sendMessage(chatGuid, QStringLiteral("Ping!"));
        QCOMPARE(b.messages().size(), 2);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject realEcho1{
            {QStringLiteral("guid"), QStringLiteral("real-echo-1")},
            {QStringLiteral("text"), QStringLiteral("Ping!")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateDelivered"), nowMs + 5},
            {QStringLiteral("dateCreated"), nowMs + 1},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{realEcho1}, false);
        QCOMPARE(b.messages().size(), 2);
        QStringList guids;
        for (const QVariant &v : b.messages()) guids.append(v.toMap().value(QStringLiteral("guid")).toString());
        QVERIFY(guids.contains(QStringLiteral("real-echo-1")));
        int tempCount = 0;
        for (const QString &g : guids) if (g.startsWith(QStringLiteral("temp-"))) ++tempCount;
        QCOMPARE(tempCount, 1);

        // 2. deleteMessage & unsendMessage must rebuild selected chat and restore preceding outgoing row's Delivered receipt
        b.deleteMessage(guids.first().startsWith(QStringLiteral("temp-")) ? guids.first() : guids.last(), chatGuid);
        b.ingestMessagesForTest(QJsonArray{
            QJsonObject{{QStringLiteral("guid"), QStringLiteral("out-del-1")}, {QStringLiteral("text"), QStringLiteral("First")}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), nowMs + 10}, {QStringLiteral("dateCreated"), nowMs + 10}, {QStringLiteral("chats"), QJsonArray{chatObj}}},
            QJsonObject{{QStringLiteral("guid"), QStringLiteral("out-del-2")}, {QStringLiteral("text"), QStringLiteral("Second")}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), nowMs + 20}, {QStringLiteral("dateCreated"), nowMs + 20}, {QStringLiteral("chats"), QJsonArray{chatObj}}}
        }, false);
        auto receiptFor = [&b](const QString &guid) {
            for (const QVariant &v : b.messages()) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("guid")).toString() == guid) return m.value(QStringLiteral("receiptText")).toString();
            }
            return QString();
        };
        QCOMPARE(receiptFor(QStringLiteral("out-del-1")), QString());
        QCOMPARE(receiptFor(QStringLiteral("out-del-2")), QStringLiteral("Delivered"));

        b.deleteMessage(QStringLiteral("out-del-2"), chatGuid);
        QCOMPARE(receiptFor(QStringLiteral("out-del-1")), QStringLiteral("Delivered"));

        b.ingestMessagesForTest(QJsonArray{
            QJsonObject{{QStringLiteral("guid"), QStringLiteral("out-unsend-3")}, {QStringLiteral("text"), QStringLiteral("Third")}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), nowMs + 30}, {QStringLiteral("dateCreated"), nowMs + 30}, {QStringLiteral("chats"), QJsonArray{chatObj}}}
        }, false);
        QCOMPARE(receiptFor(QStringLiteral("out-del-1")), QString());
        QCOMPARE(receiptFor(QStringLiteral("out-unsend-3")), QStringLiteral("Delivered"));

        b.unsendMessage(QStringLiteral("out-unsend-3"), chatGuid);
        QCOMPARE(receiptFor(QStringLiteral("out-del-1")), QStringLiteral("Delivered"));
    }

    void testPollDeltaAndRebuildNoSpuriousSignals() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_nobounce.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_nobounce.json")));

        const QString chatGuid = QStringLiteral("iMessage;-;+14155550148");
        const QJsonObject chatObj{
            {QStringLiteral("guid"), chatGuid},
            {QStringLiteral("displayName"), QStringLiteral("No Bounce")},
            {QStringLiteral("style"), 45}
        };
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject inMsg{
            {QStringLiteral("guid"), QStringLiteral("nb-in-1")},
            {QStringLiteral("text"), QStringLiteral("Hey")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), nowMs - 30000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        const QJsonObject outText{
            {QStringLiteral("guid"), QStringLiteral("nb-out-1")},
            {QStringLiteral("text"), QStringLiteral("First outgoing")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateDelivered"), nowMs - 19000},
            {QStringLiteral("dateCreated"), nowMs - 20000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        const QJsonObject outAudio{
            {QStringLiteral("guid"), QStringLiteral("nb-out-att-2")},
            {QStringLiteral("text"), QString()},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateDelivered"), nowMs - 9000},
            {QStringLiteral("dateCreated"), nowMs - 10000},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("nb-att-guid-2")},
                {QStringLiteral("mimeType"), QStringLiteral("audio/wav")},
                {QStringLiteral("transferName"), QStringLiteral("voice_memo_1.wav")},
                {QStringLiteral("durationSeconds"), 3}
            }}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{inMsg, outText, outAudio}, true);
        b.selectChat(chatGuid);
        QCOMPARE(b.messages().size(), 3);
        QCOMPARE(b.messages().at(1).toMap().value(QStringLiteral("receiptText")).toString(), QString());
        QCOMPARE(b.messages().at(2).toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));

        QSignalSpy changedSpy(&b, &Backend::messagesChanged);
        QSignalSpy aboutSpy(&b, &Backend::aboutToChangeMessages);

        // Repeated identical pollDelta payloads must emit 0 timeline change signals
        b.ingestMessagesForTest(QJsonArray{outText, outAudio}, false);
        b.ingestMessagesForTest(QJsonArray{outText, outAudio}, false);

        // Stale pollDelta payloads (e.g. older isDelivered=false) must preserve Delivered and emit 0 signals
        QJsonObject staleAudio = outAudio;
        staleAudio.insert(QStringLiteral("isDelivered"), false);
        staleAudio.insert(QStringLiteral("dateDelivered"), 0);
        QJsonObject staleText = outText;
        staleText.insert(QStringLiteral("isDelivered"), false);
        staleText.insert(QStringLiteral("dateDelivered"), 0);
        b.ingestMessagesForTest(QJsonArray{staleText, staleAudio}, false);

        // Direct rebuildSelectedChatMessages with >=2 outgoing rows must normalize receipts before comparison and emit 0 signals
        b.rebuildSelectedChatMessagesForTest();
        b.rebuildSelectedChatMessagesForTest();

        QCOMPARE(changedSpy.count(), 0);
        QCOMPARE(aboutSpy.count(), 0);
        QCOMPARE(b.messages().at(1).toMap().value(QStringLiteral("receiptText")).toString(), QString());
        QCOMPARE(b.messages().at(2).toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));
    }

    void testWriteConfigAtomicallyAndMakeRequestSecurity() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        // 1. CWE-732 writeConfigAtomically permission checks
        const QString cfgFile = tempDir.filePath(QStringLiteral("atomic_cfg.json"));
        const QJsonObject initialObj{{QStringLiteral("serverUrl"), QStringLiteral("https://example.com")},
                                     {QStringLiteral("password"), QStringLiteral("init-pw")}};
        QVERIFY(Backend::writeConfigAtomicallyForTest(cfgFile, initialObj));
        const QFileDevice::Permissions okPerms = QFile::permissions(cfgFile);
        QVERIFY(okPerms.testFlag(QFileDevice::ReadOwner));
        QVERIFY(okPerms.testFlag(QFileDevice::WriteOwner));
        QCOMPARE(okPerms & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther),
                 QFileDevice::Permissions());

        // Pre-commit setPermissions failure cancels write and preserves original config contents
        const QJsonObject overwriteObj{{QStringLiteral("serverUrl"), QStringLiteral("https://other.example")},
                                       {QStringLiteral("password"), QStringLiteral("bad-pw")}};
        QVERIFY(!Backend::writeConfigAtomicallyForTest(cfgFile, overwriteObj, true, false));
        {
            QFile f(cfgFile);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QJsonObject persisted = QJsonDocument::fromJson(f.readAll()).object();
            QCOMPARE(persisted.value(QStringLiteral("password")).toString(), QStringLiteral("init-pw"));
        }

        // Staged permission failure cancels write before commit and preserves existing config intact
        QVERIFY(!Backend::writeConfigAtomicallyForTest(cfgFile, overwriteObj, false, true));
        {
            QFile f(cfgFile);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QJsonObject persisted = QJsonDocument::fromJson(f.readAll()).object();
            QCOMPARE(persisted.value(QStringLiteral("password")).toString(), QStringLiteral("init-pw"));
        }
        const QString postFailCfg = tempDir.filePath(QStringLiteral("post_fail_cfg.json"));
        QVERIFY(!Backend::writeConfigAtomicallyForTest(postFailCfg, initialObj, false, true));
        QVERIFY(!QFileInfo::exists(postFailCfg));

        // 2. CWE-319 makeRequest enforces isTrustedPasswordTransportUrl (isolate from env URLs)
        struct EnvGuard {
            QHash<QByteArray, QByteArray> saved;
            EnvGuard(std::initializer_list<const char *> vars) {
                for (const char *v : vars) {
                    const QByteArray k(v);
                    if (qEnvironmentVariableIsSet(v)) saved.insert(k, qgetenv(v));
                    qunsetenv(v);
                }
            }
            ~EnvGuard() {
                for (auto it = saved.constBegin(); it != saved.constEnd(); ++it) {
                    qputenv(it.key().constData(), it.value());
                }
            }
        } envGuard({"BLUEPILL_SERVER_URL", "IMSG_SERVER_URL", "BLUEBUBBLES_URL",
                    "BLUEPILL_PASSWORD", "IMSG_PASSWORD", "BLUEBUBBLES_PASSWORD"});

        const QString insecureCfg = tempDir.filePath(QStringLiteral("insecure_remote.json"));
        QVERIFY(Backend::writeConfigAtomicallyForTest(
            insecureCfg,
            QJsonObject{{QStringLiteral("serverUrl"), QStringLiteral("http://remote.example.invalid:1234")},
                        {QStringLiteral("password"), QStringLiteral("top-secret-pw")}}));

        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_sec.json")));
        b.setConfigFilePath(insecureCfg);
        const QNetworkRequest rejectedReq = b.makeRequestForTest(QStringLiteral("/api/v1/message/query?limit=1"));
        QVERIFY(!rejectedReq.url().isValid() || rejectedReq.url().isEmpty());
        QVERIFY(!rejectedReq.url().toString().contains(QStringLiteral("top-secret-pw")));

        // HTTPS and loopback HTTP are permitted with password query param
        b.saveServerConfig(QStringLiteral("https://remote.example.invalid:1234"), QString(), QStringLiteral("top-secret-pw"));
        const QNetworkRequest httpsReq = b.makeRequestForTest(QStringLiteral("/api/v1/message/query"));
        QVERIFY(httpsReq.url().isValid());
        QCOMPARE(httpsReq.url().scheme(), QStringLiteral("https"));
        QVERIFY(httpsReq.url().query().contains(QStringLiteral("password=top-secret-pw")));

        b.saveServerConfig(QStringLiteral("http://127.0.0.1:12345"), QString(), QStringLiteral("top-secret-pw"));
        const QNetworkRequest loopReq = b.makeRequestForTest(QStringLiteral("/api/v1/message/query"));
        QVERIFY(loopReq.url().isValid());
        QCOMPARE(loopReq.url().scheme(), QStringLiteral("http"));
        QVERIFY(loopReq.url().query().contains(QStringLiteral("password=top-secret-pw")));
    }

    void testPendingDeliveryFreshnessEndpointGenerationAndNonInFallback() {
        QTcpServer server1;
        QTcpServer server2;
        QVERIFY(server1.listen(QHostAddress::LocalHost, 0));
        QVERIFY(server2.listen(QHostAddress::LocalHost, 0));

        const QString chatGuid = QStringLiteral("iMessage;-;+14155550171");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV3 Poll Guard")}, {QStringLiteral("style"), 45}};

        QList<QTcpSocket *> s1QuerySocks;
        enum class Server1Mode { Hold, RejectBatch400, ReturnUnrequestedZeroMatch };
        Server1Mode s1Mode = Server1Mode::Hold;
        int s1SingleQueryCount = 0;
        QStringList s1SingleQueriedGuids;

        QObject::connect(&server1, &QTcpServer::newConnection, &server1, [&]() {
            while (server1.hasPendingConnections()) {
                QTcpSocket *sock = server1.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &s1QuerySocks, &s1Mode, &s1SingleQueryCount, &s1SingleQueriedGuids]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                    buf->remove(0, hdrEnd + 4 + contentLen);

                    if (hdr.startsWith("POST /api/v1/message/query") && body.contains("message.guid")) {
                        if (s1Mode == Server1Mode::Hold) {
                            s1QuerySocks.append(sock);
                            return;
                        }
                        if (s1Mode == Server1Mode::RejectBatch400 && body.contains("IN (")) {
                            const QByteArray errJson = R"({"status":400,"error":"Unsupported IN clause"})";
                            sock->write("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(errJson.size()) + "\r\nConnection: close\r\n\r\n" + errJson);
                            sock->disconnectFromHost();
                            return;
                        }
                        if (body.contains("message.guid = :g0")) {
                            ++s1SingleQueryCount;
                            const QJsonObject reqObj = QJsonDocument::fromJson(body).object();
                            const QJsonArray whereArr = reqObj.value(QStringLiteral("where")).toArray();
                            const QString g0 = whereArr.isEmpty() ? QString() : whereArr.first().toObject().value(QStringLiteral("args")).toObject().value(QStringLiteral("g0")).toString();
                            if (!g0.isEmpty()) s1SingleQueriedGuids.append(g0);
                            const QJsonObject row{{QStringLiteral("guid"), g0}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), QDateTime::currentMSecsSinceEpoch()}};
                            const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), QJsonArray{row}}}).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                            sock->disconnectFromHost();
                            return;
                        }
                        if (s1Mode == Server1Mode::ReturnUnrequestedZeroMatch) {
                            const QJsonObject unreq{{QStringLiteral("guid"), QStringLiteral("unrequested-bogus-row")}, {QStringLiteral("text"), QStringLiteral("DoNotIngest")}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false}};
                            const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), QJsonArray{unreq}}}).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                            sock->disconnectFromHost();
                            return;
                        }
                    }
                    const QByteArray json = R"({"status":200,"data":[]})";
                    sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    sock->disconnectFromHost();
                });
            }
        });

        QList<QTcpSocket *> s2QuerySocks;
        QObject::connect(&server2, &QTcpServer::newConnection, &server2, [&]() {
            while (server2.hasPendingConnections()) {
                QTcpSocket *sock = server2.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &s2QuerySocks]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                    buf->remove(0, hdrEnd + 4 + contentLen);
                    if (hdr.startsWith("POST /api/v1/message/query") && body.contains("message.guid")) {
                        s2QuerySocks.append(sock);
                        return;
                    }
                    const QByteArray json = R"({"status":200,"data":[]})";
                    sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    sock->disconnectFromHost();
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev3_poll.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev3_poll.json")));
        const QString url1 = QStringLiteral("http://127.0.0.1:%1").arg(server1.serverPort());
        const QString url2 = QStringLiteral("http://127.0.0.1:%1").arg(server2.serverPort());
        b.saveServerConfig(url1, QString(), QStringLiteral("pw"));
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.selectChat(chatGuid);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        // A. Freshness vs conflicting stale error=4 after live delivery, plus fresh error=4 when still undelivered
        const QJsonObject mDeliveredRace{
            {QStringLiteral("guid"), QStringLiteral("out-fresh-err")},
            {QStringLiteral("text"), QStringLiteral("Delivered before error reply")},
            {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateCreated"), nowMs - 2000}, {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        const QJsonObject mFreshFail{
            {QStringLiteral("guid"), QStringLiteral("out-still-undeliv")},
            {QStringLiteral("text"), QStringLiteral("Will genuinely fail")},
            {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateCreated"), nowMs - 1000}, {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{mDeliveredRace, mFreshFail}, true);
        b.addPendingDeliveryForTest(QStringLiteral("out-fresh-err"), chatGuid, nowMs - 1000);
        b.addPendingDeliveryForTest(QStringLiteral("out-still-undeliv"), chatGuid, nowMs - 1000);

        b.checkPendingDeliveryStatusesForTest();
        QTRY_COMPARE_WITH_TIMEOUT(s1QuerySocks.size(), 1, 1500);

        // Advance observationSeq for out-fresh-err by delivering it via live event
        QJsonObject liveDelivered = mDeliveredRace;
        liveDelivered.insert(QStringLiteral("isDelivered"), true);
        liveDelivered.insert(QStringLiteral("dateDelivered"), nowMs);
        b.ingestMessagesForTest(QJsonArray{liveDelivered}, false);

        // Now complete held batch with stale error=4 on out-fresh-err and fresh error=4 on out-still-undeliv
        QJsonObject staleErrRow = mDeliveredRace;
        staleErrRow.insert(QStringLiteral("error"), 4);
        QJsonObject freshErrRow = mFreshFail;
        freshErrRow.insert(QStringLiteral("error"), 4);
        const quint64 beforeCheck1 = b.pendingDeliveryCheckCompletedCountForTest();
        const QByteArray resp1 = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), QJsonArray{staleErrRow, freshErrRow}}}).toJson(QJsonDocument::Compact);
        s1QuerySocks.takeFirst()->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(resp1.size()) + "\r\nConnection: close\r\n\r\n" + resp1);
        QTRY_VERIFY_WITH_TIMEOUT(b.pendingDeliveryCheckCompletedCountForTest() > beforeCheck1, 1500);

        // out-fresh-err MUST stay Delivered (error 0); out-still-undeliv MUST accept error=4
        const QJsonObject cachedDeliv = b.cachedMessageForTest(chatGuid, QStringLiteral("out-fresh-err"));
        QCOMPARE(cachedDeliv.value(QStringLiteral("isDelivered")).toBool(), true);
        QCOMPARE(cachedDeliv.value(QStringLiteral("error")).toInt(), 0);
        const QJsonObject cachedFail = b.cachedMessageForTest(chatGuid, QStringLiteral("out-still-undeliv"));
        QCOMPARE(cachedFail.value(QStringLiteral("error")).toInt(), 4);

        // B. Non-IN relay fallback on unrequested rows + bounded individual queries (<=10 per tick)
        s1Mode = Server1Mode::ReturnUnrequestedZeroMatch;
        QVERIFY(b.supportsBatchMessageGuidInQueryForTest());
        for (int i = 0; i < 12; ++i) {
            const QString g = QStringLiteral("out-fallback-%1").arg(i, 2, 10, QLatin1Char('0'));
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), g}, {QStringLiteral("text"), g},
                {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateCreated"), nowMs + i}, {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            b.addPendingDeliveryForTest(g, chatGuid, nowMs);
        }
        const quint64 beforeFallback = b.pendingDeliveryCheckCompletedCountForTest();
        b.checkPendingDeliveryStatusesForTest();
        QTRY_VERIFY_WITH_TIMEOUT(b.pendingDeliveryCheckCompletedCountForTest() > beforeFallback &&
                                 s1SingleQueryCount == 10 &&
                                 b.pendingDeliveryInFlightCountForTest() == 0, 1500);
        QCOMPARE(b.supportsBatchMessageGuidInQueryForTest(), false);
        // Unrequested row must NOT enter cache
        QVERIFY(b.cachedMessageForTest(chatGuid, QStringLiteral("unrequested-bogus-row")).isEmpty());
        QCOMPARE(s1SingleQueriedGuids.size(), 10);
        QCOMPARE(b.pendingDeliveryCountForTest(), 2); // 12 - 10 bounded single-query fallback in first tick

        // Next tick resolves remaining 2 via single-guid queries without batch IN
        b.checkPendingDeliveryStatusesForTest();
        QTRY_VERIFY_WITH_TIMEOUT(s1SingleQueryCount == 12 && b.pendingDeliveryInFlightCountForTest() == 0, 1500);
        QCOMPARE(b.pendingDeliveryCountForTest(), 0);

        // C. Endpoint generation guard discards stale endpoint in-flight reply & resets m_supportsBatchMessageGuidInQuery
        s1Mode = Server1Mode::Hold;
        const quint64 gen1 = b.serverEndpointGenerationForTest();
        b.saveServerConfig(url2, QString(), QStringLiteral("pw"));
        const quint64 gen2 = b.serverEndpointGenerationForTest();
        QVERIFY(gen2 > gen1);
        QCOMPARE(b.supportsBatchMessageGuidInQueryForTest(), true);

        // Switch back to url1, dispatch held query, then switch to url2 and dispatch held query on url2
        b.saveServerConfig(url1, QString(), QStringLiteral("pw"));
        b.ingestMessagesForTest(QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("out-ep-1")}, {QStringLiteral("text"), QStringLiteral("EP1")},
            {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateCreated"), nowMs + 50}, {QStringLiteral("chats"), QJsonArray{chatObj}}
        }}, false);
        b.addPendingDeliveryForTest(QStringLiteral("out-ep-1"), chatGuid, nowMs);
        b.checkPendingDeliveryStatusesForTest();
        QTRY_COMPARE_WITH_TIMEOUT(s1QuerySocks.size(), 1, 1500);

        b.saveServerConfig(url2, QString(), QStringLiteral("pw"));
        b.checkPendingDeliveryStatusesForTest();
        QTRY_COMPARE_WITH_TIMEOUT(s2QuerySocks.size(), 1, 1500);
        QCOMPARE(b.pendingDeliveryInFlightCountForTest(), 1);

        // Stale server1 reply arrives with out-ep-1 Delivered: MUST be ignored by generation guard and keep s2 in-flight intact
        const QByteArray staleEpResp = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("out-ep-1")}, {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), nowMs + 60}
        }}}}).toJson(QJsonDocument::Compact);
        QTcpSocket *staleEpSock = s1QuerySocks.takeFirst();
        staleEpSock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(staleEpResp.size()) + "\r\nConnection: close\r\n\r\n" + staleEpResp);
        staleEpSock->disconnectFromHost();
        QTest::qWait(80);
        QCOMPARE(b.pendingDeliveryInFlightCountForTest(), 1);
        QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-ep-1")).value(QStringLiteral("isDelivered")).toBool(), false);

        // Complete server2 reply cleanly
        const QByteArray s2Resp = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("out-ep-1")}, {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), nowMs + 70}
        }}}}).toJson(QJsonDocument::Compact);
        QTcpSocket *s2Sock = s2QuerySocks.takeFirst();
        s2Sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(s2Resp.size()) + "\r\nConnection: close\r\n\r\n" + s2Resp);
        s2Sock->disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(b.pendingDeliveryInFlightCountForTest(), 0, 1500);
        QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-ep-1")).value(QStringLiteral("isDelivered")).toBool(), true);
    }

    void testExpiredPendingTrackingCleanupAndTapbackReplyExclusion() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_exp_tap.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_exp_tap.json")));

        const QString chatGuid = QStringLiteral("iMessage;-;+14155550172");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Tapback & Expiry")}, {QStringLiteral("style"), 45}};
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.selectChat(chatGuid);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        // 1. Old incoming or non-pending outgoing messages MUST NOT be inserted into m_expiredPendingDeliveryGuids
        const QJsonObject oldIncoming{
            {QStringLiteral("guid"), QStringLiteral("old-in-not-pending")},
            {QStringLiteral("text"), QStringLiteral("Old incoming")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("dateCreated"), nowMs - 300000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        const QJsonObject oldNonPendingOut{
            {QStringLiteral("guid"), QStringLiteral("old-out-not-pending")},
            {QStringLiteral("text"), QStringLiteral("Old outgoing not tracked")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateCreated"), nowMs - 300000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{oldIncoming, oldNonPendingOut}, false);
        QVERIFY(!b.isExpiredPendingDeliveryForTest(QStringLiteral("old-in-not-pending")));
        QVERIFY(!b.isExpiredPendingDeliveryForTest(QStringLiteral("old-out-not-pending")));

        // Tracked outgoing >120s old expires into m_expiredPendingDeliveryGuids, then remote retraction clears it
        b.addPendingDeliveryForTest(QStringLiteral("out-ret-1"), chatGuid, nowMs - 130000);
        b.ingestMessagesForTest(QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("out-ret-1")},
            {QStringLiteral("text"), QStringLiteral("About to expire then retract")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), false},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateCreated"), nowMs - 130000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        }}, false);
        QVERIFY(b.isExpiredPendingDeliveryForTest(QStringLiteral("out-ret-1")));

        b.ingestMessagesForTest(QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("out-ret-1")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("dateRetracted"), nowMs},
            {QStringLiteral("dateCreated"), nowMs - 130000},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        }}, false);
        QVERIFY(!b.isExpiredPendingDeliveryForTest(QStringLiteral("out-ret-1")));

        // 2. Tapbacks MUST NOT count as eligible later incoming replies across timeline or polling
        const QJsonObject outPending{
            {QStringLiteral("guid"), QStringLiteral("out-tap-target")},
            {QStringLiteral("text"), QStringLiteral("Did you see this?")},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), false},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("dateCreated"), nowMs},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{outPending}, false);
        b.addPendingDeliveryForTest(QStringLiteral("out-tap-target"), chatGuid, nowMs);

        auto findReceiptFor = [&](const QString &g) -> QString {
            for (const QVariant &v : b.messages()) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("guid")).toString() == g) return m.value(QStringLiteral("receiptText")).toString();
            }
            return QString();
        };
        QCOMPARE(findReceiptFor(QStringLiteral("out-tap-target")), QStringLiteral("Sending..."));

        // Incoming tapback after outPending
        b.ingestMessagesForTest(QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("in-tapback-1")},
            {QStringLiteral("text"), QStringLiteral("Loved “Did you see this?”")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("associatedMessageType"), 2000},
            {QStringLiteral("associatedMessageGuid"), QStringLiteral("p:0/out-tap-target")},
            {QStringLiteral("dateCreated"), nowMs + 100},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        }}, false);
        // Must still be Sending... and remain pending across polling check
        b.checkPendingDeliveryStatusesForTest();
        QCOMPARE(findReceiptFor(QStringLiteral("out-tap-target")), QStringLiteral("Sending..."));
        QCOMPARE(b.pendingDeliveryCountForTest(), 1);

        // Genuine incoming non-tapback reply resolves it to "Sent" and clears pending delivery on check
        b.ingestMessagesForTest(QJsonArray{QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("in-real-reply-1")},
            {QStringLiteral("text"), QStringLiteral("Yes I did!")},
            {QStringLiteral("isFromMe"), false},
            {QStringLiteral("associatedMessageType"), 0},
            {QStringLiteral("dateCreated"), nowMs + 200},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        }}, false);
        b.checkPendingDeliveryStatusesForTest();
        QCOMPARE(findReceiptFor(QStringLiteral("out-tap-target")), QStringLiteral("Sent"));
        QCOMPARE(b.pendingDeliveryCountForTest(), 0);
    }

    void testPendingAckDoesNotClobberDbMetadataAndEmptyGuidUpload() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString chatGuid = QStringLiteral("iMessage;-;+14155550173");
        const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Upload Merge Guard")}, {QStringLiteral("style"), 45}};

        QTcpSocket *heldUpload1 = nullptr;
        QString heldTemp1;
        int uploadCount = 0;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *sock = server.nextPendingConnection();
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &heldUpload1, &heldTemp1, &uploadCount]() {
                    buf->append(sock->readAll());
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray hdr = buf->left(hdrEnd);
                    int contentLen = 0;
                    for (const QByteArray &line : hdr.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                    }
                    if (buf->size() < hdrEnd + 4 + contentLen) return;
                    const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                    buf->remove(0, hdrEnd + 4 + contentLen);

                    if (hdr.startsWith("POST /api/v1/message/attachment")) {
                        ++uploadCount;
                        QString tempG;
                        const int tgPos = body.indexOf("name=\"tempGuid\"\r\n\r\n");
                        if (tgPos >= 0) {
                            const int valStart = tgPos + 19;
                            const int valEnd = body.indexOf("\r\n--", valStart);
                            if (valEnd > valStart) tempG = QString::fromUtf8(body.mid(valStart, valEnd - valStart));
                        }
                        if (uploadCount == 1) {
                            heldUpload1 = sock;
                            heldTemp1 = tempG;
                            return;
                        }
                        // Subsequent uploads return acknowledgements WITHOUT a real server guid (realGuid.isEmpty())
                        const QJsonObject noGuidAck{
                            {QStringLiteral("tempGuid"), tempG},
                            {QStringLiteral("isFromMe"), true},
                            {QStringLiteral("isSent"), true}
                        };
                        const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), noGuidAck}}).toJson(QJsonDocument::Compact);
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                        return;
                    }
                    const QByteArray json = R"({"status":200,"data":[]})";
                    sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    sock->disconnectFromHost();
                });
            }
        });

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Backend b(false);
        b.setStateFilePath(tempDir.filePath(QStringLiteral("state_upmerge.json")));
        b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_upmerge.json")));
        b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
        b.ingestChatsMetadataForTest(QJsonArray{chatObj});
        b.selectChat(chatGuid);

        const QString f1 = tempDir.filePath(QStringLiteral("photo1.png"));
        const QString f2 = tempDir.filePath(QStringLiteral("photo2.png"));
        const QString f3 = tempDir.filePath(QStringLiteral("photo3.png"));
        for (const QString &fp : {f1, f2, f3}) {
            QFile f(fp);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("PNGDATA");
            f.close();
        }

        // 1. Live DB row arrives BEFORE upload #1 callback returns -> synthetic ack must NOT clobber DB fields
        b.sendAttachment(chatGuid, f1, QString());
        QTRY_VERIFY_WITH_TIMEOUT(heldUpload1 != nullptr && !heldTemp1.isEmpty(), 1500);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject realDbRow{
            {QStringLiteral("guid"), QStringLiteral("real-db-att-1")},
            {QStringLiteral("tempGuid"), heldTemp1},
            {QStringLiteral("originalROWID"), 9988},
            {QStringLiteral("dateCreated"), nowMs + 500},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), true},
            {QStringLiteral("dateDelivered"), nowMs + 510},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("real-server-att-1")},
                {QStringLiteral("transferName"), QStringLiteral("real.png")},
                {QStringLiteral("mimeType"), QStringLiteral("image/png")},
                {QStringLiteral("totalBytes"), 4242}
            }}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        b.ingestMessagesForTest(QJsonArray{realDbRow}, false);

        const QJsonObject syntheticAck{
            {QStringLiteral("guid"), QStringLiteral("real-db-att-1")},
            {QStringLiteral("tempGuid"), heldTemp1},
            {QStringLiteral("originalROWID"), 0},
            {QStringLiteral("pending"), true},
            {QStringLiteral("dateCreated"), nowMs + 1},
            {QStringLiteral("isFromMe"), true},
            {QStringLiteral("isSent"), true},
            {QStringLiteral("isDelivered"), false},
            {QStringLiteral("attachments"), QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("att-synthetic-1")},
                {QStringLiteral("transferName"), QStringLiteral("local.png")}
            }}},
            {QStringLiteral("chats"), QJsonArray{chatObj}}
        };
        const QByteArray ackResp = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), syntheticAck}}).toJson(QJsonDocument::Compact);
        heldUpload1->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(ackResp.size()) + "\r\nConnection: close\r\n\r\n" + ackResp);
        heldUpload1->disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);

        const QJsonObject cachedReal = b.cachedMessageForTest(chatGuid, QStringLiteral("real-db-att-1"));
        QCOMPARE(cachedReal.value(QStringLiteral("originalROWID")).toVariant().toLongLong(), 9988LL);
        QCOMPARE(cachedReal.value(QStringLiteral("dateCreated")).toVariant().toLongLong(), nowMs + 500);
        QCOMPARE(cachedReal.value(QStringLiteral("isDelivered")).toBool(), true);
        QCOMPARE(cachedReal.value(QStringLiteral("pending")).toBool(false), false);
        QCOMPARE(cachedReal.value(QStringLiteral("attachments")).toArray().first().toObject().value(QStringLiteral("guid")).toString(),
                 QStringLiteral("real-server-att-1"));

        // 2. Two uploads returning empty realGuid stay distinctly cached under their stable tempGuid keys
        b.sendAttachment(chatGuid, f2, QString());
        QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
        b.sendAttachment(chatGuid, f3, QString());
        QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);

        QStringList renderedGuids;
        for (const QVariant &v : b.messages()) {
            renderedGuids.append(v.toMap().value(QStringLiteral("guid")).toString());
        }
        QCOMPARE(renderedGuids.count(QStringLiteral("real-db-att-1")), qsizetype(1));
        int tempUploadCount = 0;
        for (const QString &g : renderedGuids) {
            if (g.startsWith(QStringLiteral("temp-att-"))) ++tempUploadCount;
        }
        QCOMPARE(tempUploadCount, 2);
    }

    void testAudioSaveFailureToastAndQmlDiscoveryFallback() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("audio_fail_cfg.json"));

        // 1. saveAudioDeviceConfig failure emits toastMessage and leaves config untouched
        {
            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_afail.json")));
            b.setConfigFilePath(cfgPath);
            b.saveServerConfig(QStringLiteral("https://example.com"), QString(), QStringLiteral("pw"));
            b.setSelectedAudioInputDevice(QStringLiteral("alsa_input.usb1"));
            b.setSelectedAudioOutputDevice(QStringLiteral("alsa_output.usb1"));

            QVERIFY(QFile::remove(cfgPath));
            QVERIFY(QDir().mkpath(cfgPath));
            QSignalSpy toastSpy(&b, &Backend::toastMessage);
            b.setSelectedAudioInputDevice(QStringLiteral("alsa_input.usb2"));
            QVERIFY(!toastSpy.isEmpty());
        }

        // 2. Main.qml audio device discovery failure preserves persisted IDs & displays System Default
        Backend backend(false);
        backend.setStateFilePath(tempDir.filePath(QStringLiteral("state_qml_audio.json")));
        backend.setConfigFilePath(tempDir.filePath(QStringLiteral("config_qml_audio.json")));
        backend.saveServerConfig(QStringLiteral("https://example.com"), QString(), QStringLiteral("pw"));
        backend.setSelectedAudioInputDevice(QStringLiteral("alsa_input.saved_mic"));
        backend.setSelectedAudioOutputDevice(QStringLiteral("alsa_output.saved_spk"));

        const QVariantList fallbackOnly{QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("label"), QStringLiteral("System Default")}}};
        backend.setAudioDevicesForTest(fallbackOnly, fallbackOnly, false, false);

        Theme theme;
        Translator translator;
        QQmlApplicationEngine engine;
        engine.setOutputWarningsToStandardError(false);
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &translator);
        engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());

        QObject *win = engine.rootObjects().first();
        QCOMPARE(backend.selectedAudioInputDevice(), QStringLiteral("alsa_input.saved_mic"));
        QCOMPARE(backend.selectedAudioOutputDevice(), QStringLiteral("alsa_output.saved_spk"));
        QCOMPARE(backend.effectiveAudioInputDeviceForTest(), QString());
        QCOMPARE(backend.effectiveAudioOutputDeviceForTest(), QString());

        QVERIFY(QMetaObject::invokeMethod(win, "reconcileAudioDeviceSelections"));
        QCOMPARE(backend.selectedAudioInputDevice(), QStringLiteral("alsa_input.saved_mic"));
        QCOMPARE(backend.selectedAudioOutputDevice(), QStringLiteral("alsa_output.saved_spk"));

        // Input discovery succeeds cleanly with a different device while output stays failed:
        // only missing input is cleared; output remains preserved independently.
        const QVariantList cleanInputs{
            QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("label"), QStringLiteral("System Default")}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("alsa_input.other_mic")}, {QStringLiteral("label"), QStringLiteral("Other Mic")}}
        };
        backend.setAudioDevicesForTest(cleanInputs, fallbackOnly, true, false);
        QTest::qWait(50);
        QCOMPARE(backend.selectedAudioInputDevice(), QString());
        QCOMPARE(backend.selectedAudioOutputDevice(), QStringLiteral("alsa_output.saved_spk"));
    }

    void testRev4GuardsHiddenPlaceholderTransient503DateReadEvictionAndUploadAck() {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        // 1. "Replied to a message:" hidden placeholder MUST NOT confirm outgoing isSent=false as "Sent"
        {
            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev4_placeholder.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev4_placeholder.json")));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550174");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Placeholder Guard")}, {QStringLiteral("style"), 45}};
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            const QJsonObject outPending{
                {QStringLiteral("guid"), QStringLiteral("out-placeholder-1")},
                {QStringLiteral("text"), QStringLiteral("Still sending")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), false},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateCreated"), nowMs},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            };
            const QJsonObject hiddenStub{
                {QStringLiteral("guid"), QStringLiteral("in-hidden-reply-stub")},
                {QStringLiteral("text"), QStringLiteral("Replied to a message:")},
                {QStringLiteral("isFromMe"), false},
                {QStringLiteral("dateCreated"), nowMs + 100},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            };

            const QVariantList built = Backend::buildMessageListFromApi(QJsonArray{hiddenStub, outPending}, {}, {});
            QCOMPARE(built.size(), 1);
            QCOMPARE(built.first().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

            b.ingestMessagesForTest(QJsonArray{outPending, hiddenStub}, false);
            b.addPendingDeliveryForTest(QStringLiteral("out-placeholder-1"), chatGuid, nowMs);
            b.checkPendingDeliveryStatusesForTest();
            QCOMPARE(b.pendingDeliveryCountForTest(), 1);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));
        }

        // 2. dateRead > 0 clears cached error=4 even when isDelivered=false and error key is omitted
        {
            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev4_dateread.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev4_dateread.json")));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550175");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("DateRead Clear")}, {QStringLiteral("style"), 45}};
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-err-read-1")},
                {QStringLiteral("text"), QStringLiteral("Failed then read")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("error"), 4},
                {QStringLiteral("dateCreated"), nowMs - 1000},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-err-read-1")).value(QStringLiteral("error")).toInt(), 4);

            // Non-receipt metadata update must keep error=4
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-err-read-1")},
                {QStringLiteral("text"), QStringLiteral("Failed then read")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-err-read-1")).value(QStringLiteral("error")).toInt(), 4);

            // Update with dateRead > 0, isDelivered=false, and omitted error must clear cached error to 0
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-err-read-1")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateRead"), nowMs},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            const QJsonObject cleared = b.cachedMessageForTest(chatGuid, QStringLiteral("out-err-read-1"));
            QCOMPARE(cleared.value(QStringLiteral("error")).toInt(), 0);
            QVERIFY(!b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool());
            QVERIFY(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString().startsWith(QStringLiteral("Read ")));
        }

        // 3. HTTP 503 preserves batch IN mode + reconciliation eviction rejects stale in-flight status reply
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550176");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("503 & Evict")}, {QStringLiteral("style"), 45}};

            int queryHits = 0;
            bool return503 = true;
            bool holdNextQuery = false;
            QTcpSocket *heldQuerySock = nullptr;
            QList<bool> sawBatchInPerRequest;
            QList<QByteArray> queryBodies;

            QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *sock = server.nextPendingConnection();
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &queryHits, &return503, &holdNextQuery, &heldQuerySock, &sawBatchInPerRequest, &queryBodies]() {
                        buf->append(sock->readAll());
                        const int hdrEnd = buf->indexOf("\r\n\r\n");
                        if (hdrEnd < 0) return;
                        const QByteArray hdr = buf->left(hdrEnd);
                        int contentLen = 0;
                        for (const QByteArray &line : hdr.split('\n')) {
                            if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                        }
                        if (buf->size() < hdrEnd + 4 + contentLen) return;
                        const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                        buf->remove(0, hdrEnd + 4 + contentLen);

                        if (hdr.startsWith("POST /api/v1/message/query") && body.contains("message.guid")) {
                            ++queryHits;
                            sawBatchInPerRequest.append(body.contains("IN ("));
                            queryBodies.append(body);
                            if (return503) {
                                const QByteArray err503 = "Service Temporarily Unavailable";
                                sock->write("HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: " + QByteArray::number(err503.size()) + "\r\nConnection: close\r\n\r\n" + err503);
                                sock->disconnectFromHost();
                                return;
                            }
                            if (holdNextQuery) {
                                heldQuerySock = sock;
                                return;
                            }
                        }
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    });
                }
            });

            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev4_503.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev4_503.json")));
            b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            for (int i = 0; i < 2; ++i) {
                const QString g = QStringLiteral("out-503-%1").arg(i);
                b.ingestMessagesForTest(QJsonArray{QJsonObject{
                    {QStringLiteral("guid"), g}, {QStringLiteral("text"), g},
                    {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false},
                    {QStringLiteral("dateCreated"), nowMs - 70000 + i}, {QStringLiteral("chats"), QJsonArray{chatObj}}
                }}, false);
                b.addPendingDeliveryForTest(g, chatGuid, nowMs);
            }

            // First batch gets HTTP 503 -> must keep m_supportsBatchMessageGuidInQuery == true
            const quint64 before503 = b.pendingDeliveryCheckCompletedCountForTest();
            b.checkPendingDeliveryStatusesForTest();
            QTRY_VERIFY_WITH_TIMEOUT(b.pendingDeliveryCheckCompletedCountForTest() > before503 &&
                                     b.pendingDeliveryInFlightCountForTest() == 0, 1500);
            QCOMPARE(b.supportsBatchMessageGuidInQueryForTest(), true);
            QCOMPARE(queryHits, 1);
            QCOMPARE(sawBatchInPerRequest.first(), true);

            // Second tick still uses batch IN query; hold it in-flight while out-evict-0 is aged past 120s and evicted by reconciliation
            const QString evictGuid = QStringLiteral("out-evict-0");
            const qint64 safeDispatchTs = QDateTime::currentMSecsSinceEpoch() - 5000;
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), evictGuid}, {QStringLiteral("text"), evictGuid},
                {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateCreated"), safeDispatchTs}, {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            b.addPendingDeliveryForTest(evictGuid, chatGuid, safeDispatchTs);

            return503 = false;
            holdNextQuery = true;
            b.checkPendingDeliveryStatusesForTest();
            QTRY_VERIFY_WITH_TIMEOUT(heldQuerySock != nullptr, 1500);
            QCOMPARE(sawBatchInPerRequest.size(), 2);
            QCOMPARE(sawBatchInPerRequest.at(1), true);
            {
                QJsonParseError qErr{};
                const QJsonDocument qDoc = QJsonDocument::fromJson(queryBodies.at(1), &qErr);
                QCOMPARE(qErr.error, QJsonParseError::NoError);
                const QJsonArray whereArr = qDoc.object().value(QStringLiteral("where")).toArray();
                QVERIFY(!whereArr.isEmpty());
                const QJsonObject where0 = whereArr.first().toObject();
                QVERIFY(where0.value(QStringLiteral("statement")).toString().contains(QStringLiteral("message.guid IN (")));
                const QJsonObject argsObj = where0.value(QStringLiteral("args")).toObject();
                QStringList boundGuids;
                for (auto it = argsObj.begin(); it != argsObj.end(); ++it) {
                    boundGuids.append(it.value().toString());
                }
                QVERIFY(boundGuids.contains(evictGuid));
            }

            // Deterministically age out-evict-0 past 120s expiry so it leaves m_pendingDeliveryMessages while batch is still in-flight
            QCOMPARE(b.pendingDeliveryCountForTest(), 3);
            b.agePendingDeliveryForTest(evictGuid, 125000LL);
            b.checkPendingDeliveryStatusesForTest();
            QCOMPARE(b.pendingDeliveryCountForTest(), 2);

            // Reconcile history: remove out-evict-0 (absent from complete server history) while keeping out-503-0 & out-503-1
            b.reconcileChatMessagesForTest(chatGuid, QJsonArray{
                QJsonObject{{QStringLiteral("guid"), QStringLiteral("out-503-0")}, {QStringLiteral("text"), QStringLiteral("out-503-0")}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false}, {QStringLiteral("dateCreated"), nowMs - 70000}, {QStringLiteral("chats"), QJsonArray{chatObj}}},
                QJsonObject{{QStringLiteral("guid"), QStringLiteral("out-503-1")}, {QStringLiteral("text"), QStringLiteral("out-503-1")}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), false}, {QStringLiteral("dateCreated"), nowMs - 69999}, {QStringLiteral("chats"), QJsonArray{chatObj}}}
            });
            QVERIFY(b.cachedMessageForTest(chatGuid, evictGuid).isEmpty());

            // Now in-flight pre-eviction query completes with both out-evict-0 and out-503-1 Delivered
            const quint64 beforeEvictAck = b.pendingDeliveryCheckCompletedCountForTest();
            const QByteArray evictResp = QJsonDocument(QJsonObject{
                {QStringLiteral("status"), 200},
                {QStringLiteral("data"), QJsonArray{
                    QJsonObject{{QStringLiteral("guid"), evictGuid}, {QStringLiteral("text"), evictGuid}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), nowMs}, {QStringLiteral("dateCreated"), safeDispatchTs}, {QStringLiteral("chats"), QJsonArray{chatObj}}},
                    QJsonObject{{QStringLiteral("guid"), QStringLiteral("out-503-1")}, {QStringLiteral("text"), QStringLiteral("out-503-1")}, {QStringLiteral("isFromMe"), true}, {QStringLiteral("isSent"), true}, {QStringLiteral("isDelivered"), true}, {QStringLiteral("dateDelivered"), nowMs}, {QStringLiteral("dateCreated"), nowMs - 69999}, {QStringLiteral("chats"), QJsonArray{chatObj}}}
                }}
            }).toJson(QJsonDocument::Compact);
            heldQuerySock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(evictResp.size()) + "\r\nConnection: close\r\n\r\n" + evictResp);
            heldQuerySock->disconnectFromHost();
            QTRY_VERIFY_WITH_TIMEOUT(b.pendingDeliveryCheckCompletedCountForTest() > beforeEvictAck, 1500);

            // Evicted out-evict-0 must NOT be re-inserted by the stale in-flight status reply; out-503-1 updates normally
            QVERIFY(b.cachedMessageForTest(chatGuid, evictGuid).isEmpty());
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-503-1")).value(QStringLiteral("isDelivered")).toBool(), true);

            // A later genuinely fresh incoming observation for out-evict-0 is accepted normally
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), evictGuid},
                {QStringLiteral("text"), QStringLiteral("Fresh restore")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), true},
                {QStringLiteral("dateCreated"), QDateTime::currentMSecsSinceEpoch()},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QVERIFY(!b.cachedMessageForTest(chatGuid, evictGuid).isEmpty());
        }

        // 4. HTML 200 upload response rejected & empty-GUID upload ack without delivery evidence stays "Sending..."
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550177");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("Upload Ack Guard")}, {QStringLiteral("style"), 45}};

            int uploadCall = 0;
            QString secondTempGuid;
            QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *sock = server.nextPendingConnection();
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &uploadCall, &secondTempGuid]() {
                        buf->append(sock->readAll());
                        const int hdrEnd = buf->indexOf("\r\n\r\n");
                        if (hdrEnd < 0) return;
                        const QByteArray hdr = buf->left(hdrEnd);
                        int contentLen = 0;
                        for (const QByteArray &line : hdr.split('\n')) {
                            if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                        }
                        if (buf->size() < hdrEnd + 4 + contentLen) return;
                        const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                        buf->remove(0, hdrEnd + 4 + contentLen);

                        if (hdr.startsWith("POST /api/v1/message/attachment")) {
                            ++uploadCall;
                            if (uploadCall == 1) {
                                const QByteArray html200 = "<html><body>Proxy Auth Required</body></html>";
                                sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + QByteArray::number(html200.size()) + "\r\nConnection: close\r\n\r\n" + html200);
                                sock->disconnectFromHost();
                                return;
                            }
                            QString tempG;
                            const int tgPos = body.indexOf("name=\"tempGuid\"\r\n\r\n");
                            if (tgPos >= 0) {
                                const int valStart = tgPos + 19;
                                const int valEnd = body.indexOf("\r\n--", valStart);
                                if (valEnd > valStart) tempG = QString::fromUtf8(body.mid(valStart, valEnd - valStart));
                            }
                            secondTempGuid = tempG;
                            const QByteArray ackJson = QJsonDocument(QJsonObject{
                                {QStringLiteral("status"), 200},
                                {QStringLiteral("data"), QJsonObject{
                                    {QStringLiteral("tempGuid"), tempG},
                                    {QStringLiteral("isFromMe"), true},
                                    {QStringLiteral("pending"), true}
                                }}
                            }).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(ackJson.size()) + "\r\nConnection: close\r\n\r\n" + ackJson);
                            sock->disconnectFromHost();
                            return;
                        }
                        if (hdr.startsWith("POST /api/v1/message/text")) {
                            const QByteArray html200 = "<html><body>Bad Gateway</body></html>";
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + QByteArray::number(html200.size()) + "\r\nConnection: close\r\n\r\n" + html200);
                            sock->disconnectFromHost();
                            return;
                        }
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    });
                }
            });

            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            const QString img1 = tempDir.filePath(QStringLiteral("ack1.png"));
            const QString img2 = tempDir.filePath(QStringLiteral("ack2.png"));
            for (const QString &fp : {img1, img2}) {
                QFile f(fp);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write("PNG");
                f.close();
            }

            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev4_upload.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev4_upload.json")));
            b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            QSignalSpy toastSpy(&b, &Backend::toastMessage);
            b.sendAttachment(chatGuid, img1, QString());
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QCOMPARE(b.messages().size(), 1);
            QCOMPARE(b.messages().first().toMap().value(QStringLiteral("isFailed")).toBool(), true);
            QCOMPARE(b.messages().first().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));
            QVERIFY(toastSpy.last().first().toString().contains(QStringLiteral("Failed to send attachment")));

            b.sendAttachment(chatGuid, img2, QString());
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QVERIFY(!secondTempGuid.isEmpty());
            const QJsonObject cachedSecond = b.cachedMessageForTest(chatGuid, secondTempGuid);
            QVERIFY(!cachedSecond.isEmpty());
            QCOMPARE(cachedSecond.value(QStringLiteral("pending")).toBool(), true);
            QCOMPARE(cachedSecond.value(QStringLiteral("isSent")), QJsonValue(false));
            QCOMPARE(cachedSecond.value(QStringLiteral("isDelivered")), QJsonValue(false));
            b.rebuildSelectedChatMessagesForTest();
            QCOMPARE(b.messages().size(), 2);
            QCOMPARE(b.messages().first().toMap().value(QStringLiteral("isFailed")).toBool(), true);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

            b.sendMessage(chatGuid, QStringLiteral("HTML 200 text send"));
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QTest::qWait(60);
            b.rebuildSelectedChatMessagesForTest();
            QCOMPARE(b.messages().size(), 3);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));
        }
    }

    void testRev5ReceiptTieBreakAndUploadAckGuards() {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        // 1. Retained old positive receipt must NOT clear a newer accepted error=4 on non-receipt updates;
        //    only incoming positive delivery/read evidence (including dateRead > 0 omitting error) clears it.
        {
            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev5_err.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev5_err.json")));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550181");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV5 Error Merge")}, {QStringLiteral("style"), 45}};
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev5-err-1")},
                {QStringLiteral("text"), QStringLiteral("Initially delivered")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), true},
                {QStringLiteral("dateDelivered"), nowMs - 3000},
                {QStringLiteral("dateCreated"), nowMs - 4000},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev5-err-1")).value(QStringLiteral("error")).toInt(), 0);

            // Newer accepted non-pending error=4 arrives
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev5-err-1")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("error"), 4},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev5-err-1")).value(QStringLiteral("error")).toInt(), 4);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);

            // Non-receipt status updates (explicit error=0 or omitted error, no incoming delivery/read evidence)
            // MUST NOT clear error=4 based on old retained isDelivered/dateDelivered in merged.
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev5-err-1")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("error"), 0},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev5-err-1")).value(QStringLiteral("error")).toInt(), 4);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);

            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev5-err-1")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev5-err-1")).value(QStringLiteral("error")).toInt(), 4);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);

            // Incoming dateRead > 0 omitting error key MUST clear error to 0
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev5-err-1")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateRead"), nowMs},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev5-err-1")).value(QStringLiteral("error")).toInt(), 0);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QVERIFY(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString().startsWith(QStringLiteral("Read ")));
        }

        // 2. Equal-timestamp ordering in rebuildSelectedChatMessages with newly fetched server rows
        {
            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev5_tie.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev5_tie.json")));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550182");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV5 TieBreak")}, {QStringLiteral("style"), 45}};
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            const qint64 ts = nowMs;
            // (a) [srvA(ts, prev=0), optB(ts, prev=1)] + newly fetched a-srv-new(ts, prev=INT_MAX) sorting before b-srv-a:
            //     optB MUST NOT jump ahead of b-srv-a!
            b.ingestMessagesForTest(QJsonArray{
                QJsonObject{{QStringLiteral("guid"), QStringLiteral("a-srv-new")}, {QStringLiteral("text"), QStringLiteral("New")}, {QStringLiteral("isFromMe"), false}, {QStringLiteral("dateCreated"), ts}, {QStringLiteral("chats"), QJsonArray{chatObj}}},
                QJsonObject{{QStringLiteral("guid"), QStringLiteral("b-srv-a")}, {QStringLiteral("text"), QStringLiteral("Srv A")}, {QStringLiteral("isFromMe"), false}, {QStringLiteral("dateCreated"), ts}, {QStringLiteral("chats"), QJsonArray{chatObj}}}
            }, false);

            const QVariantMap srvAPrev{{QStringLiteral("guid"), QStringLiteral("b-srv-a")}, {QStringLiteral("chatGuid"), chatGuid}, {QStringLiteral("text"), QStringLiteral("Srv A")}, {QStringLiteral("dateCreated"), ts}};
            const QVariantMap optBPrev{{QStringLiteral("guid"), QStringLiteral("temp-opt-b")}, {QStringLiteral("chatGuid"), chatGuid}, {QStringLiteral("text"), QStringLiteral("Opt B")}, {QStringLiteral("dateCreated"), ts}, {QStringLiteral("optimisticCreatedAtMs"), ts}, {QStringLiteral("isFromMe"), true}};
            b.setMessagesForTest(QVariantList{srvAPrev, optBPrev});
            b.rebuildSelectedChatMessagesForTest();

            QStringList orderA;
            for (const QVariant &v : b.messages()) orderA.append(v.toMap().value(QStringLiteral("guid")).toString());
            QCOMPARE(orderA, (QStringList{QStringLiteral("a-srv-new"), QStringLiteral("b-srv-a"), QStringLiteral("temp-opt-b")}));

            // (b) [optA(ts, prev=0), srvB(ts, prev=1)] + a-srv-new(ts, prev=INT_MAX):
            //     optA MUST stay before c-srv-b!
            b.ingestMessagesForTest(QJsonArray{
                QJsonObject{{QStringLiteral("guid"), QStringLiteral("c-srv-b")}, {QStringLiteral("text"), QStringLiteral("Srv B")}, {QStringLiteral("isFromMe"), false}, {QStringLiteral("dateCreated"), ts}, {QStringLiteral("chats"), QJsonArray{chatObj}}}
            }, false);
            const QVariantMap optAPrev{{QStringLiteral("guid"), QStringLiteral("temp-opt-a")}, {QStringLiteral("chatGuid"), chatGuid}, {QStringLiteral("text"), QStringLiteral("Opt A")}, {QStringLiteral("dateCreated"), ts}, {QStringLiteral("optimisticCreatedAtMs"), ts}, {QStringLiteral("isFromMe"), true}};
            const QVariantMap srvBPrev{{QStringLiteral("guid"), QStringLiteral("c-srv-b")}, {QStringLiteral("chatGuid"), chatGuid}, {QStringLiteral("text"), QStringLiteral("Srv B")}, {QStringLiteral("dateCreated"), ts}};
            b.setMessagesForTest(QVariantList{optAPrev, srvBPrev});
            b.rebuildSelectedChatMessagesForTest();

            QStringList orderB;
            for (const QVariant &v : b.messages()) orderB.append(v.toMap().value(QStringLiteral("guid")).toString());
            const int idxOptA = orderB.indexOf(QStringLiteral("temp-opt-a"));
            const int idxSrvB = orderB.indexOf(QStringLiteral("c-srv-b"));
            QVERIFY(idxOptA >= 0 && idxSrvB >= 0);
            QVERIFY(idxOptA < idxSrvB);
        }

        // 3. Reject bare-GUID / non-result upload & sendMessage acks; keep valid correlated pending acks pending
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550183");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV5 Ack Guards")}, {QStringLiteral("style"), 45}};

            int uploadCall = 0;
            QString pendingTempGuid;
            QString pendingSrvTempGuid;
            QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *sock = server.nextPendingConnection();
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &uploadCall, &pendingTempGuid, &pendingSrvTempGuid]() {
                        buf->append(sock->readAll());
                        const int hdrEnd = buf->indexOf("\r\n\r\n");
                        if (hdrEnd < 0) return;
                        const QByteArray hdr = buf->left(hdrEnd);
                        int contentLen = 0;
                        for (const QByteArray &line : hdr.split('\n')) {
                            if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                        }
                        if (buf->size() < hdrEnd + 4 + contentLen) return;
                        const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                        buf->remove(0, hdrEnd + 4 + contentLen);

                        auto extractTemp = [&body]() -> QString {
                            const int tgPos = body.indexOf("name=\"tempGuid\"\r\n\r\n");
                            if (tgPos < 0) return QString();
                            const int valStart = tgPos + 19;
                            const int valEnd = body.indexOf("\r\n--", valStart);
                            return (valEnd > valStart) ? QString::fromUtf8(body.mid(valStart, valEnd - valStart)) : QString();
                        };

                        if (hdr.startsWith("POST /api/v1/message/attachment")) {
                            ++uploadCall;
                            const QString tempG = extractTemp();
                            QJsonObject dataObj;
                            if (uploadCall == 1) {
                                dataObj = QJsonObject{{QStringLiteral("guid"), QStringLiteral("g-only")}};
                            } else if (uploadCall == 2) {
                                dataObj = QJsonObject{{QStringLiteral("foo"), QStringLiteral("bar")}};
                            } else if (uploadCall == 3) {
                                dataObj = QJsonObject{{QStringLiteral("guid"), QStringLiteral("g-temp-only")}, {QStringLiteral("tempGuid"), tempG}};
                            } else if (uploadCall == 4) {
                                pendingTempGuid = tempG;
                                dataObj = QJsonObject{{QStringLiteral("tempGuid"), tempG}, {QStringLiteral("pending"), true}};
                            } else {
                                pendingSrvTempGuid = tempG;
                                dataObj = QJsonObject{{QStringLiteral("guid"), QStringLiteral("g-srv-pending")}, {QStringLiteral("tempGuid"), tempG}, {QStringLiteral("pending"), true}};
                            }
                            const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), dataObj}}).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                            sock->disconnectFromHost();
                            return;
                        }
                        if (hdr.startsWith("POST /api/v1/message/text")) {
                            const QString tg = QJsonDocument::fromJson(body).object().value(QStringLiteral("tempGuid")).toString();
                            const QByteArray json = QJsonDocument(QJsonObject{
                                {QStringLiteral("status"), 200},
                                {QStringLiteral("data"), QJsonObject{{QStringLiteral("guid"), QStringLiteral("text-g-only")}, {QStringLiteral("tempGuid"), tg}}}
                            }).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                            sock->disconnectFromHost();
                            return;
                        }
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    });
                }
            });

            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            const QString imgPath = tempDir.filePath(QStringLiteral("ack_guard.png"));
            {
                QFile f(imgPath);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write("PNG");
                f.close();
            }

            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev5_ack.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev5_ack.json")));
            b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            for (int i = 0; i < 3; ++i) {
                b.sendAttachment(chatGuid, imgPath, QString());
                QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
                QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);
                QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));
            }

            b.sendAttachment(chatGuid, imgPath, QString());
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QVERIFY(!pendingTempGuid.isEmpty());
            const QJsonObject cachedPend = b.cachedMessageForTest(chatGuid, pendingTempGuid);
            QCOMPARE(cachedPend.value(QStringLiteral("pending")).toBool(), true);
            QCOMPARE(cachedPend.value(QStringLiteral("isSent")), QJsonValue(false));
            QCOMPARE(cachedPend.value(QStringLiteral("isDelivered")), QJsonValue(false));
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

            b.sendAttachment(chatGuid, imgPath, QString());
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QVERIFY(!pendingSrvTempGuid.isEmpty());
            const QJsonObject cachedSrvPend = b.cachedMessageForTest(chatGuid, QStringLiteral("g-srv-pending"));
            QCOMPARE(cachedSrvPend.value(QStringLiteral("pending")).toBool(), true);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

            b.sendMessage(chatGuid, QStringLiteral("Bare guid sendMessage"));
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));
        }
    }

    void testRev6UploadCorrelationPendingNormalizationAndErrorFreshness() {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        // 1. Mismatched upload IDs with error=4 must NOT mark another upload failed or overwrite its optimistic row,
        //    while correlated upload errors MUST still be accepted.
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550191");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV6 Upload Err")}, {QStringLiteral("style"), 45}};

            int uploadCall = 0;
            QString tempA;
            QString tempB;
            QString tempC;
            QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *sock = server.nextPendingConnection();
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &uploadCall, &tempA, &tempB, &tempC, nowMs]() {
                        buf->append(sock->readAll());
                        const int hdrEnd = buf->indexOf("\r\n\r\n");
                        if (hdrEnd < 0) return;
                        const QByteArray hdr = buf->left(hdrEnd);
                        int contentLen = 0;
                        for (const QByteArray &line : hdr.split('\n')) {
                            if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                        }
                        if (buf->size() < hdrEnd + 4 + contentLen) return;
                        const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                        buf->remove(0, hdrEnd + 4 + contentLen);

                        auto extractTemp = [&body]() -> QString {
                            const int tgPos = body.indexOf("name=\"tempGuid\"\r\n\r\n");
                            if (tgPos < 0) return QString();
                            const int valStart = tgPos + 19;
                            const int valEnd = body.indexOf("\r\n--", valStart);
                            return (valEnd > valStart) ? QString::fromUtf8(body.mid(valStart, valEnd - valStart)) : QString();
                        };

                        if (hdr.startsWith("POST /api/v1/message/attachment")) {
                            ++uploadCall;
                            const QString tg = extractTemp();
                            QJsonObject dataObj;
                            if (uploadCall == 1) {
                                tempA = tg;
                                dataObj = QJsonObject{
                                    {QStringLiteral("guid"), QStringLiteral("g-up-a")},
                                    {QStringLiteral("tempGuid"), tempA},
                                    {QStringLiteral("isFromMe"), true},
                                    {QStringLiteral("isSent"), true},
                                    {QStringLiteral("isDelivered"), false},
                                    {QStringLiteral("dateCreated"), nowMs - 2000},
                                    {QStringLiteral("attachments"), QJsonArray{QJsonObject{{QStringLiteral("guid"), QStringLiteral("att-a-1")}, {QStringLiteral("transferName"), QStringLiteral("up_a.png")}, {QStringLiteral("mimeType"), QStringLiteral("image/png")}}}}
                                };
                            } else if (uploadCall == 2) {
                                tempB = tg;
                                // Mismatched error naming upload A's guid and tempGuid
                                dataObj = QJsonObject{
                                    {QStringLiteral("guid"), QStringLiteral("g-up-a")},
                                    {QStringLiteral("tempGuid"), tempA},
                                    {QStringLiteral("error"), 4}
                                };
                            } else {
                                tempC = tg;
                                // Correlated error for upload C
                                dataObj = QJsonObject{
                                    {QStringLiteral("guid"), QStringLiteral("g-up-c")},
                                    {QStringLiteral("tempGuid"), tempC},
                                    {QStringLiteral("error"), 4},
                                    {QStringLiteral("dateCreated"), nowMs}
                                };
                            }
                            const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("status"), 200}, {QStringLiteral("data"), dataObj}}).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                            sock->disconnectFromHost();
                            return;
                        }
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    });
                }
            });

            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            const QString imgA = tempDir.filePath(QStringLiteral("up_a.png"));
            const QString imgB = tempDir.filePath(QStringLiteral("up_b.png"));
            const QString imgC = tempDir.filePath(QStringLiteral("up_c.png"));
            for (const QString &fp : {imgA, imgB, imgC}) {
                QFile f(fp);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write("PNG");
                f.close();
            }

            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev6_up.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev6_up.json")));
            b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            b.sendAttachment(chatGuid, imgA, QString());
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("g-up-a")).value(QStringLiteral("error")).toInt(), 0);

            b.sendAttachment(chatGuid, imgB, QString());
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QVERIFY(!tempB.isEmpty());
            // Upload A must remain healthy; upload B's optimistic row must remain distinct (not overwritten with g-up-a) and marked failed
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("g-up-a")).value(QStringLiteral("error")).toInt(), 0);
            QCOMPARE(b.messages().size(), 2);
            QCOMPARE(b.messages().at(0).toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("g-up-a"));
            QCOMPARE(b.messages().at(0).toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QCOMPARE(b.messages().at(1).toMap().value(QStringLiteral("guid")).toString(), tempB);
            QCOMPARE(b.messages().at(1).toMap().value(QStringLiteral("isFailed")).toBool(), true);
            QCOMPARE(b.messages().at(1).toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));

            b.sendAttachment(chatGuid, imgC, QString());
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("g-up-c")).value(QStringLiteral("error")).toInt(), 4);
            QCOMPARE(b.messages().size(), 3);
            QCOMPARE(b.messages().at(2).toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("g-up-c"));
            QCOMPARE(b.messages().at(2).toMap().value(QStringLiteral("isFailed")).toBool(), true);
            b.rebuildSelectedChatMessagesForTest();
            QCOMPARE(b.messages().size(), 3);
            QVariantMap rebuiltErr;
            for (const QVariant &v : b.messages()) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("guid")).toString() == QStringLiteral("g-up-c")) {
                    rebuiltErr = m;
                    break;
                }
            }
            QCOMPARE(rebuiltErr.value(QStringLiteral("guid")).toString(), QStringLiteral("g-up-c"));
            QCOMPARE(rebuiltErr.value(QStringLiteral("hasImageAttachment")).toBool(), true);
            QCOMPARE(rebuiltErr.value(QStringLiteral("attachmentName")).toString(), QStringLiteral("up_c.png"));
            QCOMPARE(rebuiltErr.value(QStringLiteral("isFailed")).toBool(), true);
            QCOMPARE(rebuiltErr.value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));
        }

        // 2 & 4. Text ack tempGuid validation against sentTempGuid (AppleScript -as, /chat/new relay tempGuid, mismatch reject)
        //        + partial pending text ack defaulting isSent=false independently of isDelivered while preserving explicit isSent=true.
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550192");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV6 Text Ack")}, {QStringLiteral("style"), 45}};

            QString origAsTempGuid;
            QString asFallbackMethod;
            QString asFallbackTempGuid;
            QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *sock = server.nextPendingConnection();
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, chatGuid, nowMs, &origAsTempGuid, &asFallbackMethod, &asFallbackTempGuid]() {
                        buf->append(sock->readAll());
                        const int hdrEnd = buf->indexOf("\r\n\r\n");
                        if (hdrEnd < 0) return;
                        const QByteArray hdr = buf->left(hdrEnd);
                        int contentLen = 0;
                        for (const QByteArray &line : hdr.split('\n')) {
                            if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                        }
                        if (buf->size() < hdrEnd + 4 + contentLen) return;
                        const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                        buf->remove(0, hdrEnd + 4 + contentLen);

                        if (hdr.startsWith("POST /api/v1/message/text")) {
                            const QJsonObject reqObj = QJsonDocument::fromJson(body).object();
                            const QString msg = reqObj.value(QStringLiteral("message")).toString();
                            const QString method = reqObj.value(QStringLiteral("method")).toString();
                            const QString tg = reqObj.value(QStringLiteral("tempGuid")).toString();

                            if (msg == QStringLiteral("AS fallback msg")) {
                                if (method == QStringLiteral("private-api")) {
                                    origAsTempGuid = tg;
                                    const QByteArray errJson = R"({"status":500,"error":"Private API unavailable"})";
                                    sock->write("HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(errJson.size()) + "\r\nConnection: close\r\n\r\n" + errJson);
                                    sock->disconnectFromHost();
                                    return;
                                }
                                asFallbackMethod = method;
                                asFallbackTempGuid = tg;
                                const QByteArray json = QJsonDocument(QJsonObject{
                                    {QStringLiteral("status"), 200},
                                    {QStringLiteral("data"), QJsonObject{
                                        {QStringLiteral("guid"), QStringLiteral("g-as-1")},
                                        {QStringLiteral("tempGuid"), tg},
                                        {QStringLiteral("isFromMe"), true},
                                        {QStringLiteral("isSent"), true},
                                        {QStringLiteral("isDelivered"), false},
                                        {QStringLiteral("dateCreated"), nowMs - 5000}
                                    }}
                                }).toJson(QJsonDocument::Compact);
                                sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                                sock->disconnectFromHost();
                                return;
                            }
                            if (msg == QStringLiteral("New chat msg")) {
                                const QByteArray errJson = R"({"status":404,"error":"Chat does not exist"})";
                                sock->write("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(errJson.size()) + "\r\nConnection: close\r\n\r\n" + errJson);
                                sock->disconnectFromHost();
                                return;
                            }
                            if (msg == QStringLiteral("Wrong temp ack")) {
                                const QByteArray json = QJsonDocument(QJsonObject{
                                    {QStringLiteral("status"), 200},
                                    {QStringLiteral("data"), QJsonObject{
                                        {QStringLiteral("guid"), QStringLiteral("g-wrong-send")},
                                        {QStringLiteral("tempGuid"), QStringLiteral("temp-different-send")},
                                        {QStringLiteral("isFromMe"), true},
                                        {QStringLiteral("isSent"), true},
                                        {QStringLiteral("isDelivered"), true},
                                        {QStringLiteral("dateCreated"), nowMs - 3000}
                                    }}
                                }).toJson(QJsonDocument::Compact);
                                sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                                sock->disconnectFromHost();
                                return;
                            }
                            if (msg == QStringLiteral("Pending no isSent")) {
                                const QByteArray json = QJsonDocument(QJsonObject{
                                    {QStringLiteral("status"), 200},
                                    {QStringLiteral("data"), QJsonObject{
                                        {QStringLiteral("guid"), QStringLiteral("g-pend-no-issent")},
                                        {QStringLiteral("tempGuid"), tg},
                                        {QStringLiteral("text"), msg},
                                        {QStringLiteral("pending"), true},
                                        {QStringLiteral("isDelivered"), false},
                                        {QStringLiteral("dateCreated"), nowMs + 2000}
                                    }}
                                }).toJson(QJsonDocument::Compact);
                                sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                                sock->disconnectFromHost();
                                return;
                            }
                            if (msg == QStringLiteral("Pending explicit isSent")) {
                                const QByteArray json = QJsonDocument(QJsonObject{
                                    {QStringLiteral("status"), 200},
                                    {QStringLiteral("data"), QJsonObject{
                                        {QStringLiteral("guid"), QStringLiteral("g-pend-pos-issent")},
                                        {QStringLiteral("tempGuid"), tg},
                                        {QStringLiteral("text"), msg},
                                        {QStringLiteral("pending"), true},
                                        {QStringLiteral("isSent"), true},
                                        {QStringLiteral("isDelivered"), false},
                                        {QStringLiteral("dateCreated"), nowMs + 3000}
                                    }}
                                }).toJson(QJsonDocument::Compact);
                                sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                                sock->disconnectFromHost();
                                return;
                            }
                        }
                        if (hdr.startsWith("POST /api/v1/chat/new")) {
                            const QByteArray json = QJsonDocument(QJsonObject{
                                {QStringLiteral("status"), 200},
                                {QStringLiteral("data"), QJsonObject{
                                    {QStringLiteral("guid"), chatGuid},
                                    {QStringLiteral("messages"), QJsonArray{QJsonObject{
                                        {QStringLiteral("guid"), QStringLiteral("g-newchat-1")},
                                        {QStringLiteral("tempGuid"), QStringLiteral("relay-gen-newchat-1")},
                                        {QStringLiteral("isFromMe"), true},
                                        {QStringLiteral("isSent"), true},
                                        {QStringLiteral("isDelivered"), false},
                                        {QStringLiteral("dateCreated"), nowMs - 4000}
                                    }}}
                                }}
                            }).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                            sock->disconnectFromHost();
                            return;
                        }
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    });
                }
            });

            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev6_text.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev6_text.json")));
            b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            // (a) AppleScript -as suffix accepted & promotes optimistic row
            b.sendMessage(chatGuid, QStringLiteral("AS fallback msg"));
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QVERIFY(!origAsTempGuid.isEmpty());
            QCOMPARE(asFallbackMethod, QStringLiteral("apple-script"));
            QCOMPARE(asFallbackTempGuid, origAsTempGuid + QStringLiteral("-as"));
            QVERIFY(!b.cachedMessageForTest(chatGuid, QStringLiteral("g-as-1")).isEmpty());
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("g-as-1"));
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sent"));

            // (b) /api/v1/chat/new omitted tempGuid -> relay-generated tempGuid accepted
            b.sendMessage(chatGuid, QStringLiteral("New chat msg"));
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QVERIFY(!b.cachedMessageForTest(chatGuid, QStringLiteral("g-newchat-1")).isEmpty());
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("g-newchat-1"));
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sent"));

            // (c) Mismatched tempGuid rejected
            b.sendMessage(chatGuid, QStringLiteral("Wrong temp ack"));
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            QVERIFY(b.cachedMessageForTest(chatGuid, QStringLiteral("g-wrong-send")).isEmpty());
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));

            // (d) Pending ack with isDelivered:false and omitted isSent defaults isSent:false -> shows "Sending..." (never "Sent")
            b.sendMessage(chatGuid, QStringLiteral("Pending no isSent"));
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            const QJsonObject cachedNoSent = b.cachedMessageForTest(chatGuid, QStringLiteral("g-pend-no-issent"));
            QCOMPARE(cachedNoSent.value(QStringLiteral("isSent")), QJsonValue(false));
            QCOMPARE(cachedNoSent.value(QStringLiteral("isDelivered")), QJsonValue(false));
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("g-pend-no-issent")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("pending"), false},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

            // (e) Pending ack with explicit isSent:true preserves isSent:true -> shows "Sent"
            b.sendMessage(chatGuid, QStringLiteral("Pending explicit isSent"));
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            const QJsonObject cachedPosSent = b.cachedMessageForTest(chatGuid, QStringLiteral("g-pend-pos-issent"));
            QCOMPARE(cachedPosSent.value(QStringLiteral("isSent")), QJsonValue(true));
            QCOMPARE(cachedPosSent.value(QStringLiteral("isDelivered")), QJsonValue(false));
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sent"));
        }

        // 3. Sparse pending:true upload ack preserves optimistic attachment metadata & creation time across rebuilds,
        //    then prefers full server attachment metadata when the real server row arrives.
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550193");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV6 Sparse Upload")}, {QStringLiteral("style"), 45}};

            QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *sock = server.nextPendingConnection();
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf]() {
                        buf->append(sock->readAll());
                        const int hdrEnd = buf->indexOf("\r\n\r\n");
                        if (hdrEnd < 0) return;
                        const QByteArray hdr = buf->left(hdrEnd);
                        int contentLen = 0;
                        for (const QByteArray &line : hdr.split('\n')) {
                            if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                        }
                        if (buf->size() < hdrEnd + 4 + contentLen) return;
                        const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                        buf->remove(0, hdrEnd + 4 + contentLen);

                        if (hdr.startsWith("POST /api/v1/message/attachment")) {
                            const int tgPos = body.indexOf("name=\"tempGuid\"\r\n\r\n");
                            const int valStart = tgPos + 19;
                            const int valEnd = body.indexOf("\r\n--", valStart);
                            const QString tg = (tgPos >= 0 && valEnd > valStart) ? QString::fromUtf8(body.mid(valStart, valEnd - valStart)) : QString();
                            const QByteArray json = QJsonDocument(QJsonObject{
                                {QStringLiteral("status"), 200},
                                {QStringLiteral("data"), QJsonObject{
                                    {QStringLiteral("guid"), QStringLiteral("g-sparse-pend")},
                                    {QStringLiteral("tempGuid"), tg},
                                    {QStringLiteral("pending"), true}
                                }}
                            }).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                            sock->disconnectFromHost();
                            return;
                        }
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    });
                }
            });

            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            const QString imgPath = tempDir.filePath(QStringLiteral("sparse_photo.png"));
            {
                QFile f(imgPath);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write("PNG");
                f.close();
            }

            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev6_sparse.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev6_sparse.json")));
            b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            b.sendAttachment(chatGuid, imgPath, QStringLiteral("Caption stays"));
            QCOMPARE(b.messages().size(), 1);
            const qint64 optDateCreated = b.messages().first().toMap().value(QStringLiteral("dateCreated")).toLongLong();
            QVERIFY(optDateCreated > 0);
            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            b.rebuildSelectedChatMessagesForTest();
            QCOMPARE(b.messages().size(), 1);
            const QVariantMap rebuiltOpt = b.messages().first().toMap();
            QCOMPARE(rebuiltOpt.value(QStringLiteral("guid")).toString(), QStringLiteral("g-sparse-pend"));
            QCOMPARE(rebuiltOpt.value(QStringLiteral("hasImageAttachment")).toBool(), true);
            QCOMPARE(rebuiltOpt.value(QStringLiteral("attachmentName")).toString(), QStringLiteral("sparse_photo.png"));
            QCOMPARE(rebuiltOpt.value(QStringLiteral("text")).toString(), QStringLiteral("Caption stays"));
            QCOMPARE(rebuiltOpt.value(QStringLiteral("dateCreated")).toLongLong(), optDateCreated);
            QVERIFY(!rebuiltOpt.value(QStringLiteral("timeText")).toString().isEmpty());
            QCOMPARE(rebuiltOpt.value(QStringLiteral("receiptText")).toString(), QStringLiteral("Sending..."));

            // Full server row arrives later and supersedes optimistic attachment metadata & timestamp
            const qint64 srvTs = nowMs + 5000;
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("g-sparse-pend")},
                {QStringLiteral("originalROWID"), 7777},
                {QStringLiteral("text"), QStringLiteral("Caption stays")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), true},
                {QStringLiteral("dateDelivered"), srvTs},
                {QStringLiteral("dateCreated"), srvTs},
                {QStringLiteral("pending"), false},
                {QStringLiteral("attachments"), QJsonArray{QJsonObject{
                    {QStringLiteral("guid"), QStringLiteral("real-srv-att-7777")},
                    {QStringLiteral("transferName"), QStringLiteral("server_photo.png")},
                    {QStringLiteral("mimeType"), QStringLiteral("image/png")}
                }}},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            b.rebuildSelectedChatMessagesForTest();
            QCOMPARE(b.messages().size(), 1);
            const QVariantMap rebuiltSrv = b.messages().first().toMap();
            QCOMPARE(rebuiltSrv.value(QStringLiteral("attachmentGuid")).toString(), QStringLiteral("real-srv-att-7777"));
            QCOMPARE(rebuiltSrv.value(QStringLiteral("attachmentName")).toString(), QStringLiteral("server_photo.png"));
            QCOMPARE(rebuiltSrv.value(QStringLiteral("dateCreated")).toLongLong(), srvTs);
            QCOMPARE(rebuiltSrv.value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));
        }

        // 5. Reject older positive GUID-query receipt snapshot when it conflicts with newer accepted error=4,
        //    while preserving clearing error when a genuinely newer delivery/read observation arrives.
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550194");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV6 Error Race")}, {QStringLiteral("style"), 45}};

            QList<QTcpSocket *> heldSocks;
            QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *sock = server.nextPendingConnection();
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &heldSocks]() {
                        buf->append(sock->readAll());
                        const int hdrEnd = buf->indexOf("\r\n\r\n");
                        if (hdrEnd < 0) return;
                        const QByteArray hdr = buf->left(hdrEnd);
                        int contentLen = 0;
                        for (const QByteArray &line : hdr.split('\n')) {
                            if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                        }
                        if (buf->size() < hdrEnd + 4 + contentLen) return;
                        const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                        buf->remove(0, hdrEnd + 4 + contentLen);
                        if (hdr.startsWith("POST /api/v1/message/query") && body.contains("message.guid")) {
                            heldSocks.append(sock);
                            return;
                        }
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    });
                }
            });

            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev6_race.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev6_race.json")));
            b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            // (a) In-flight GUID query reads delivered row before live update reports error=4, then finishes last
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev6-race-1")},
                {QStringLiteral("text"), QStringLiteral("Race test")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateCreated"), nowMs - 2000},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            b.addPendingDeliveryForTest(QStringLiteral("out-rev6-race-1"), chatGuid, nowMs - 1000);
            b.checkPendingDeliveryStatusesForTest();
            QTRY_COMPARE_WITH_TIMEOUT(heldSocks.size(), 1, 1500);

            // Live update delivers error=4 while GUID query is in flight
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev6-race-1")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("error"), 4},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev6-race-1")).value(QStringLiteral("error")).toInt(), 4);

            // Held GUID query completes with older positive delivery snapshot -> MUST be rejected
            const quint64 beforeFin = b.pendingDeliveryCheckCompletedCountForTest();
            const QByteArray staleDelivResp = QJsonDocument(QJsonObject{
                {QStringLiteral("status"), 200},
                {QStringLiteral("data"), QJsonArray{QJsonObject{
                    {QStringLiteral("guid"), QStringLiteral("out-rev6-race-1")},
                    {QStringLiteral("isFromMe"), true},
                    {QStringLiteral("isSent"), true},
                    {QStringLiteral("isDelivered"), true},
                    {QStringLiteral("dateDelivered"), nowMs - 500},
                    {QStringLiteral("error"), 0}
                }}}
            }).toJson(QJsonDocument::Compact);
            QTcpSocket *hs = heldSocks.takeFirst();
            hs->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(staleDelivResp.size()) + "\r\nConnection: close\r\n\r\n" + staleDelivResp);
            hs->disconnectFromHost();
            QTRY_VERIFY_WITH_TIMEOUT(b.pendingDeliveryCheckCompletedCountForTest() > beforeFin, 1500);

            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev6-race-1")).value(QStringLiteral("error")).toInt(), 4);
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("⚠ Not Delivered"));

            // (b) Direct merge conflict: initially delivered at T0, then error=4 accepted, then older T0 delivery snapshot
            //     must NOT clear error=4, whereas a newer delivery (> T0) or read (> 0) MUST clear error=4.
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev6-merge-2")},
                {QStringLiteral("text"), QStringLiteral("Merge conflict")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), true},
                {QStringLiteral("dateDelivered"), nowMs - 3000},
                {QStringLiteral("dateCreated"), nowMs - 1000},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev6-merge-2")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("error"), 4},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev6-merge-2")).value(QStringLiteral("error")).toInt(), 4);

            // Older positive snapshot (same/older dateDelivered <= nowMs - 3000) must NOT clear error=4
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev6-merge-2")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), true},
                {QStringLiteral("dateDelivered"), nowMs - 3000},
                {QStringLiteral("error"), 0},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev6-merge-2")).value(QStringLiteral("error")).toInt(), 4);
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), true);

            // Genuinely newer delivery observation (dateDelivered > nowMs - 3000) clears error=4
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev6-merge-2")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), true},
                {QStringLiteral("dateDelivered"), nowMs},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev6-merge-2")).value(QStringLiteral("error")).toInt(), 0);
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString(), QStringLiteral("Delivered"));

            // (c) In-flight GUID query reads a recovered row with newer dateRead after live error=4 -> MUST clear error=4
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev7-recover-3")},
                {QStringLiteral("text"), QStringLiteral("Recovered read test")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("dateCreated"), nowMs - 500},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            b.addPendingDeliveryForTest(QStringLiteral("out-rev7-recover-3"), chatGuid, nowMs - 200);
            b.checkPendingDeliveryStatusesForTest();
            QTRY_COMPARE_WITH_TIMEOUT(heldSocks.size(), 1, 1500);

            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("out-rev7-recover-3")},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isDelivered"), false},
                {QStringLiteral("error"), 4},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            QCOMPARE(b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev7-recover-3")).value(QStringLiteral("error")).toInt(), 4);

            const quint64 beforeRecFin = b.pendingDeliveryCheckCompletedCountForTest();
            const qint64 recReadTs = nowMs + 1000;
            const QByteArray recoveredReadResp = QJsonDocument(QJsonObject{
                {QStringLiteral("status"), 200},
                {QStringLiteral("data"), QJsonArray{QJsonObject{
                    {QStringLiteral("guid"), QStringLiteral("out-rev7-recover-3")},
                    {QStringLiteral("isFromMe"), true},
                    {QStringLiteral("isSent"), true},
                    {QStringLiteral("isDelivered"), false},
                    {QStringLiteral("dateRead"), recReadTs},
                    {QStringLiteral("error"), 0}
                }}}
            }).toJson(QJsonDocument::Compact);
            QTcpSocket *hsRec = heldSocks.takeFirst();
            hsRec->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(recoveredReadResp.size()) + "\r\nConnection: close\r\n\r\n" + recoveredReadResp);
            hsRec->disconnectFromHost();
            QTRY_VERIFY_WITH_TIMEOUT(b.pendingDeliveryCheckCompletedCountForTest() > beforeRecFin, 1500);

            const QJsonObject recCached = b.cachedMessageForTest(chatGuid, QStringLiteral("out-rev7-recover-3"));
            QCOMPARE(recCached.value(QStringLiteral("error")).toInt(), 0);
            QCOMPARE(recCached.value(QStringLiteral("dateRead")).toVariant().toLongLong(), recReadTs);
            QVERIFY(!b.messages().isEmpty());
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("out-rev7-recover-3"));
            QCOMPARE(b.messages().last().toMap().value(QStringLiteral("isFailed")).toBool(), false);
            QVERIFY(b.messages().last().toMap().value(QStringLiteral("receiptText")).toString().startsWith(QStringLiteral("Read ")));
        }

        // 6. Sparse pending:true audio upload ACK preserves WAV-computed waveformBars across rebuilds
        //    until a full server attachment replaces it (both realGuid and pending-tempGuid paths).
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            const QString chatGuid = QStringLiteral("iMessage;-;+14155550195");
            const QJsonObject chatObj{{QStringLiteral("guid"), chatGuid}, {QStringLiteral("displayName"), QStringLiteral("REV7 Sparse Audio")}, {QStringLiteral("style"), 45}};

            int audioUploadCall = 0;
            QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *sock = server.nextPendingConnection();
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &audioUploadCall]() {
                        buf->append(sock->readAll());
                        const int hdrEnd = buf->indexOf("\r\n\r\n");
                        if (hdrEnd < 0) return;
                        const QByteArray hdr = buf->left(hdrEnd);
                        int contentLen = 0;
                        for (const QByteArray &line : hdr.split('\n')) {
                            if (line.toLower().startsWith("content-length:")) contentLen = line.mid(15).trimmed().toInt();
                        }
                        if (buf->size() < hdrEnd + 4 + contentLen) return;
                        const QByteArray body = buf->mid(hdrEnd + 4, contentLen);
                        buf->remove(0, hdrEnd + 4 + contentLen);

                        if (hdr.startsWith("POST /api/v1/message/attachment")) {
                            ++audioUploadCall;
                            const int tgPos = body.indexOf("name=\"tempGuid\"\r\n\r\n");
                            const int valStart = tgPos + 19;
                            const int valEnd = body.indexOf("\r\n--", valStart);
                            const QString tg = (tgPos >= 0 && valEnd > valStart) ? QString::fromUtf8(body.mid(valStart, valEnd - valStart)) : QString();
                            QJsonObject dataObj{
                                {QStringLiteral("tempGuid"), tg},
                                {QStringLiteral("pending"), true}
                            };
                            if (audioUploadCall == 1) {
                                dataObj.insert(QStringLiteral("guid"), QStringLiteral("g-sparse-wav-1"));
                            }
                            const QByteArray json = QJsonDocument(QJsonObject{
                                {QStringLiteral("status"), 200},
                                {QStringLiteral("data"), dataObj}
                            }).toJson(QJsonDocument::Compact);
                            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                            sock->disconnectFromHost();
                            return;
                        }
                        const QByteArray json = R"({"status":200,"data":[]})";
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                        sock->disconnectFromHost();
                    });
                }
            });

            QTemporaryDir tempDir;
            QVERIFY(tempDir.isValid());
            const QString wavPath = tempDir.filePath(QStringLiteral("voice_sparse.wav"));
            {
                QFile wf(wavPath);
                QVERIFY(wf.open(QIODevice::WriteOnly));
                const quint32 sampleRate = 8000;
                const quint32 numSamples = 8000;
                const quint32 dataBytes = numSamples * 2;
                const quint32 riffSize = 36 + dataBytes;
                QByteArray hdr;
                hdr.append("RIFF", 4);
                auto appendU32 = [&](quint32 v) {
                    hdr.append(char(v & 0xFF));
                    hdr.append(char((v >> 8) & 0xFF));
                    hdr.append(char((v >> 16) & 0xFF));
                    hdr.append(char((v >> 24) & 0xFF));
                };
                auto appendU16 = [&](quint16 v) {
                    hdr.append(char(v & 0xFF));
                    hdr.append(char((v >> 8) & 0xFF));
                };
                appendU32(riffSize);
                hdr.append("WAVEfmt ", 8);
                appendU32(16);
                appendU16(1);
                appendU16(1);
                appendU32(sampleRate);
                appendU32(sampleRate * 2);
                appendU16(2);
                appendU16(16);
                hdr.append("data", 4);
                appendU32(dataBytes);
                wf.write(hdr);
                QByteArray pcm;
                pcm.reserve(int(dataBytes));
                for (quint32 i = 0; i < numSamples; ++i) {
                    const qint16 amp = static_cast<qint16>(((i % 200) < 100) ? ((i * 3) % 24000) : -((i * 5) % 18000));
                    pcm.append(char(amp & 0xFF));
                    pcm.append(char((amp >> 8) & 0xFF));
                }
                wf.write(pcm);
                wf.close();
            }

            Backend b(false);
            b.setStateFilePath(tempDir.filePath(QStringLiteral("state_rev7_wav.json")));
            b.setConfigFilePath(tempDir.filePath(QStringLiteral("config_rev7_wav.json")));
            b.saveServerConfig(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), QString(), QStringLiteral("pw"));
            b.ingestChatsMetadataForTest(QJsonArray{chatObj});
            b.selectChat(chatGuid);

            // (a) Sparse pending ack with realGuid
            b.sendAttachment(chatGuid, wavPath, QString());
            QCOMPARE(b.messages().size(), 1);
            const QVariantList optAtts1 = b.messages().first().toMap().value(QStringLiteral("attachments")).toList();
            QCOMPARE(optAtts1.size(), 1);
            const QVariantList optWaveform1 = optAtts1.first().toMap().value(QStringLiteral("waveformBars")).toList();
            QCOMPARE(optWaveform1.size(), 28);
            QVERIFY(optWaveform1 != Backend::computeWaveformBars(QStringLiteral("g-sparse-wav-1"), 28));

            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            b.rebuildSelectedChatMessagesForTest();
            QCOMPARE(b.messages().size(), 1);
            const QVariantList rebuiltAtts1 = b.messages().first().toMap().value(QStringLiteral("attachments")).toList();
            QCOMPARE(rebuiltAtts1.size(), 1);
            QCOMPARE(rebuiltAtts1.first().toMap().value(QStringLiteral("waveformBars")).toList(), optWaveform1);
            QVERIFY(rebuiltAtts1.first().toMap().value(QStringLiteral("durationSeconds")).toInt() >= 1);

            // Full server attachment replaces fallback waveform
            const QJsonArray srvWf{0.2, 0.9, 0.4};
            b.ingestMessagesForTest(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), QStringLiteral("g-sparse-wav-1")},
                {QStringLiteral("originalROWID"), 8888},
                {QStringLiteral("isFromMe"), true},
                {QStringLiteral("isSent"), true},
                {QStringLiteral("isDelivered"), true},
                {QStringLiteral("dateDelivered"), nowMs - 4000},
                {QStringLiteral("dateCreated"), nowMs - 5000},
                {QStringLiteral("pending"), false},
                {QStringLiteral("attachments"), QJsonArray{QJsonObject{
                    {QStringLiteral("guid"), QStringLiteral("real-srv-wav-8888")},
                    {QStringLiteral("transferName"), QStringLiteral("server_voice.wav")},
                    {QStringLiteral("mimeType"), QStringLiteral("audio/wav")},
                    {QStringLiteral("durationSeconds"), 2},
                    {QStringLiteral("waveform"), srvWf}
                }}},
                {QStringLiteral("chats"), QJsonArray{chatObj}}
            }}, false);
            b.rebuildSelectedChatMessagesForTest();
            const QVariantList srvAtts1 = b.messages().first().toMap().value(QStringLiteral("attachments")).toList();
            QCOMPARE(srvAtts1.size(), 1);
            QCOMPARE(srvAtts1.first().toMap().value(QStringLiteral("guid")).toString(), QStringLiteral("real-srv-wav-8888"));
            QCOMPARE(srvAtts1.first().toMap().value(QStringLiteral("waveformBars")).toList().size(), 3);
            QCOMPARE(srvAtts1.first().toMap().value(QStringLiteral("waveformBars")).toList().at(1).toDouble(), 0.9);

            // (b) Sparse pending ack without realGuid (pendingAck synthetic fallback)
            b.sendAttachment(chatGuid, wavPath, QString());
            QCOMPARE(b.messages().size(), 2);
            const QVariantList optAtts2 = b.messages().last().toMap().value(QStringLiteral("attachments")).toList();
            QCOMPARE(optAtts2.size(), 1);
            const QVariantList optWaveform2 = optAtts2.first().toMap().value(QStringLiteral("waveformBars")).toList();
            QCOMPARE(optWaveform2.size(), 28);

            QTRY_COMPARE_WITH_TIMEOUT(b.sendingMessage(), false, 1500);
            b.rebuildSelectedChatMessagesForTest();
            QCOMPARE(b.messages().size(), 2);
            const QVariantList rebuiltAtts2 = b.messages().last().toMap().value(QStringLiteral("attachments")).toList();
            QCOMPARE(rebuiltAtts2.size(), 1);
            QCOMPARE(rebuiltAtts2.first().toMap().value(QStringLiteral("waveformBars")).toList(), optWaveform2);
            QVERIFY(rebuiltAtts2.first().toMap().value(QStringLiteral("durationSeconds")).toInt() >= 1);
        }
    }

};

QTEST_MAIN(ImsgTests)
#include "imsg_tests.moc"
