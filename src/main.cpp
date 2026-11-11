#include <QKeyEvent>
#include "backend.h"
#include "theme.h"
#include "translator.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <functional>
#include <iostream>

static QQuickItem *findVisualItemByName(QQuickItem *item, const QString &objName) {
    if (!item) return nullptr;
    if (item->objectName() == objName) return item;
    for (QQuickItem *child : item->childItems()) {
        if (QQuickItem *found = findVisualItemByName(child, objName)) return found;
    }
    return nullptr;
}

int main(int argc, char *argv[]) {
    QStringList rawArgs;
    for (int i = 1; i < argc; ++i) {
        rawArgs.append(QString::fromUtf8(argv[i]));
    }

    if (rawArgs.contains(QStringLiteral("--version")) || rawArgs.contains(QStringLiteral("-v"))) {
        std::cout << "imsg 1.0.0" << std::endl;
        return 0;
    }

    const bool isCliCommand = !rawArgs.isEmpty() && (
        rawArgs.first() == QStringLiteral("status") ||
        rawArgs.first() == QStringLiteral("recent") ||
        rawArgs.first() == QStringLiteral("send")
    );

    if (isCliCommand) {
        QCoreApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("imsg"));
        app.setApplicationVersion(QStringLiteral("1.0.0"));
        Backend backend(false);
        const bool asJson = rawArgs.contains(QStringLiteral("--json"));
        const QString cmd = rawArgs.first();
        if (cmd == QStringLiteral("status")) {
            return backend.runCliStatus(asJson);
        }
        if (cmd == QStringLiteral("recent")) {
            int limit = 15;
            const int idx = rawArgs.indexOf(QStringLiteral("--limit"));
            if (idx >= 0 && idx + 1 < rawArgs.size()) {
                limit = rawArgs.at(idx + 1).toInt();
            }
            return backend.runCliRecent(limit, asJson);
        }
        if (cmd == QStringLiteral("send") && rawArgs.size() >= 3) {
            return backend.runCliSend(rawArgs.at(1), rawArgs.at(2), asJson);
        }
        return 1;
    }

    QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.services.warning=false"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("imsg"));
    app.setApplicationVersion(QStringLiteral("1.0.0"));
    app.setDesktopFileName(QStringLiteral("imsg"));
    app.setOrganizationName(QStringLiteral("Omarchy"));

    QString releaseScreenshotsDir;
    const int relSsIdx = rawArgs.indexOf(QStringLiteral("--generate-release-screenshots"));
    if (relSsIdx >= 0 && relSsIdx + 1 < rawArgs.size()) {
        releaseScreenshotsDir = rawArgs.at(relSsIdx + 1);
    }

    const bool demoMode = rawArgs.contains(QStringLiteral("--demo")) ||
                          rawArgs.contains(QStringLiteral("--demo-screenshot")) ||
                          !releaseScreenshotsDir.isEmpty();
    const bool testStartup = rawArgs.contains(QStringLiteral("--test-startup"));

    QString screenshotPath;
    const int ssIdx = rawArgs.indexOf(QStringLiteral("--demo-screenshot"));
    if (ssIdx >= 0 && ssIdx + 1 < rawArgs.size()) {
        screenshotPath = rawArgs.at(ssIdx + 1);
    }
    QString liveScreenshotPath;
    const int liveSsIdx = rawArgs.indexOf(QStringLiteral("--live-screenshot"));
    if (liveSsIdx >= 0 && liveSsIdx + 1 < rawArgs.size()) {
        liveScreenshotPath = rawArgs.at(liveSsIdx + 1);
    }
    QString homeScreenshotPath;
    const int homeSsIdx = rawArgs.indexOf(QStringLiteral("--screenshot-home"));
    if (homeSsIdx >= 0 && homeSsIdx + 1 < rawArgs.size()) {
        homeScreenshotPath = rawArgs.at(homeSsIdx + 1);
    }
    QString chatScreenshotQuery;
    QString chatScreenshotPath;
    const int chatSsIdx = rawArgs.indexOf(QStringLiteral("--screenshot-chat"));
    if (chatSsIdx >= 0 && chatSsIdx + 2 < rawArgs.size()) {
        chatScreenshotQuery = rawArgs.at(chatSsIdx + 1);
        chatScreenshotPath = rawArgs.at(chatSsIdx + 2);
    }
    QString viewerScreenshotQuery;
    QString viewerScreenshotPath;
    const int viewerSsIdx = rawArgs.indexOf(QStringLiteral("--screenshot-photo-viewer"));
    if (viewerSsIdx >= 0 && viewerSsIdx + 2 < rawArgs.size()) {
        viewerScreenshotQuery = rawArgs.at(viewerSsIdx + 1);
        viewerScreenshotPath = rawArgs.at(viewerSsIdx + 2);
    }

    Theme theme;
    Translator translator;
    Backend backend(!demoMode);
    if (demoMode) {
        backend.loadDemoData();
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("i18n"), &translator);
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);

    const QUrl url(QStringLiteral("qrc:/Main.qml"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &app,
        [url](QObject *obj, const QUrl &objUrl) {
            if (!obj && url == objUrl) QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    engine.load(url);

    if (!releaseScreenshotsDir.isEmpty()) {
        QDir().mkpath(releaseScreenshotsDir);
        const auto roots = engine.rootObjects();
        if (!roots.isEmpty()) {
            if (auto *win = qobject_cast<QQuickWindow *>(roots.first())) {
                win->setWidth(1440);
                win->setHeight(900);
            }
        }
        // 1. Capture active Alex Rivera conversation (3-photo bubble + reaction tooltip + link preview)
        QTimer::singleShot(900, &app, [&engine, &backend, releaseScreenshotsDir]() {
            const auto roots = engine.rootObjects();
            if (roots.isEmpty()) { QCoreApplication::quit(); return; }
            auto *win = qobject_cast<QQuickWindow *>(roots.first());
            if (!win) { QCoreApplication::quit(); return; }

            QObject *rootObj0 = engine.rootObjects().first();
            QMetaObject::invokeMethod(rootObj0, "scrollMessagesToTop");
            QTimer::singleShot(150, QCoreApplication::instance(), [rootObj0, win]() {
                QMetaObject::invokeMethod(rootObj0, "scrollMessagesStepDown");
                QMetaObject::invokeMethod(rootObj0, "scrollMessagesStepDown");
                if (QQuickItem *pill = findVisualItemByName(win->contentItem(), QStringLiteral("reactionPillItem"))) {
                    pill->setProperty("forceHoveredForTest", true);
                }
            });

            QTimer::singleShot(350, QCoreApplication::instance(), [&engine, &backend, win, releaseScreenshotsDir]() {
                const QString chatPng = releaseScreenshotsDir + QStringLiteral("/chat-conversation.png");
                win->grabWindow().save(chatPng);
                QFileInfo dirFi(releaseScreenshotsDir);
                if (dirFi.fileName() == QStringLiteral("screenshots")) {
                    win->grabWindow().save(dirFi.dir().filePath(QStringLiteral("screenshot.png")));
                }

                if (QQuickItem *pill = findVisualItemByName(win->contentItem(), QStringLiteral("reactionPillItem"))) {
                    pill->setProperty("forceHoveredForTest", false);
                }

                // 2. Open built-in Photo Viewer overlay (1 / 3 counter + zoom controls)
                QObject *rootObj = engine.rootObjects().first();
                for (const QVariant &mv : backend.messages()) {
                    const QVariantMap msg = mv.toMap();
                    const QVariantList imgAtts = msg.value(QStringLiteral("imageAttachments")).toList();
                    if (!imgAtts.isEmpty()) {
                        QMetaObject::invokeMethod(rootObj, "openPhotoViewer",
                                                  Q_ARG(QVariant, QVariant::fromValue(imgAtts)),
                                                  Q_ARG(QVariant, QVariant::fromValue(0)));
                        break;
                    }
                }

                QTimer::singleShot(450, QCoreApplication::instance(), [&engine, &backend, win, rootObj, releaseScreenshotsDir]() {
                    win->grabWindow().save(releaseScreenshotsDir + QStringLiteral("/photo-viewer.png"));
                    QMetaObject::invokeMethod(rootObj, "closePhotoViewer");

                    // 3. Navigate to iMessage Home Dashboard (1440x900)
                    backend.clearSelectedChat();
                    QTimer::singleShot(450, QCoreApplication::instance(), [&engine, &backend, win, releaseScreenshotsDir]() {
                        win->grabWindow().save(releaseScreenshotsDir + QStringLiteral("/home-dashboard.png"));

                        // 4. Select Alex Rivera & resize to 920x860 for 50% split-window responsive header
                        backend.selectChat(QStringLiteral("iMessage;-;+14155550142"));
                        win->setWidth(920);
                        win->setHeight(860);
                        QObject *rObj = engine.rootObjects().first();
                        QMetaObject::invokeMethod(rObj, "scrollMessagesToTop");
                        QTimer::singleShot(500, QCoreApplication::instance(), [&engine, win, releaseScreenshotsDir]() {
                            QObject *rObj2 = engine.rootObjects().first();
                            QMetaObject::invokeMethod(rObj2, "scrollMessagesToTop");
                            win->grabWindow().save(releaseScreenshotsDir + QStringLiteral("/split-window-responsive.png"));
                            QCoreApplication::quit();
                        });
                    });
                });
            });
        });
    } else if (!homeScreenshotPath.isEmpty()) {
        backend.clearSelectedChat();
        QTimer::singleShot(1200, &app, [&engine, &backend, homeScreenshotPath]() {
            backend.clearSelectedChat();
            QTimer::singleShot(400, QCoreApplication::instance(), [&engine, homeScreenshotPath]() {
                const auto roots = engine.rootObjects();
                if (!roots.isEmpty()) {
                    if (auto *win = qobject_cast<QQuickWindow *>(roots.first())) {
                        QDir().mkpath(QFileInfo(homeScreenshotPath).absolutePath());
                        win->grabWindow().save(homeScreenshotPath);
                    }
                }
                QCoreApplication::quit();
            });
        });
    } else if (!viewerScreenshotPath.isEmpty()) {
        QTimer::singleShot(800, &app, [&engine, &backend, viewerScreenshotQuery, viewerScreenshotPath]() {
            for (const QVariant &item : backend.chats()) {
                const QVariantMap m = item.toMap();
                if (m.value(QStringLiteral("guid")).toString().contains(viewerScreenshotQuery, Qt::CaseInsensitive) ||
                    m.value(QStringLiteral("title")).toString().contains(viewerScreenshotQuery, Qt::CaseInsensitive)) {
                    backend.selectChat(m.value(QStringLiteral("guid")).toString());
                    break;
                }
            }
            QTimer::singleShot(800, QCoreApplication::instance(), [&engine, &backend, viewerScreenshotPath]() {
                const auto roots = engine.rootObjects();
                if (!roots.isEmpty()) {
                    QObject *rootObj = roots.first();
                    for (int i = backend.messages().size() - 1; i >= 0; --i) {
                        const QVariantMap msg = backend.messages().at(i).toMap();
                        const QVariantList imgAtts = msg.value(QStringLiteral("imageAttachments")).toList();
                        if (!imgAtts.isEmpty()) {
                            QMetaObject::invokeMethod(rootObj, "openPhotoViewer",
                                                      Q_ARG(QVariant, QVariant::fromValue(imgAtts)),
                                                      Q_ARG(QVariant, QVariant::fromValue(0)));
                            break;
                        }
                    }
                }
                QTimer::singleShot(400, QCoreApplication::instance(), [&engine, viewerScreenshotPath]() {
                    const auto roots3 = engine.rootObjects();
                    if (!roots3.isEmpty()) {
                        if (auto *win = qobject_cast<QQuickWindow *>(roots3.first())) {
                            QDir().mkpath(QFileInfo(viewerScreenshotPath).absolutePath());
                            win->grabWindow().save(viewerScreenshotPath);
                        }
                    }
                    QCoreApplication::quit();
                });
            });
        });
    } else if (!chatScreenshotPath.isEmpty()) {
        QTimer::singleShot(800, &app, [&engine, &backend, chatScreenshotQuery, chatScreenshotPath]() {
            for (const QVariant &item : backend.chats()) {
                const QVariantMap m = item.toMap();
                if (m.value(QStringLiteral("guid")).toString().contains(chatScreenshotQuery, Qt::CaseInsensitive) ||
                    m.value(QStringLiteral("title")).toString().contains(chatScreenshotQuery, Qt::CaseInsensitive)) {
                    backend.selectChat(m.value(QStringLiteral("guid")).toString());
                    break;
                }
            }
            QTimer::singleShot(1200, QCoreApplication::instance(), [&engine, chatScreenshotPath]() {
                const auto roots = engine.rootObjects();
                if (!roots.isEmpty()) {
                    if (auto *win = qobject_cast<QQuickWindow *>(roots.first())) {
                        QDir().mkpath(QFileInfo(chatScreenshotPath).absolutePath());
                        win->grabWindow().save(chatScreenshotPath);
                    }
                }
                QCoreApplication::quit();
            });
        });
    } else if (!liveScreenshotPath.isEmpty()) {
        QTimer::singleShot(6000, &app, [&engine, liveScreenshotPath]() {
            const auto roots = engine.rootObjects();
            if (!roots.isEmpty()) {
                if (auto *win = qobject_cast<QQuickWindow *>(roots.first())) {
                    QDir().mkpath(QFileInfo(liveScreenshotPath).absolutePath());
                    win->grabWindow().save(liveScreenshotPath);
                }
            }
            QCoreApplication::quit();
        });
    } else if (!screenshotPath.isEmpty()) {
        QTimer::singleShot(800, &app, [&engine, screenshotPath]() {
            const auto roots = engine.rootObjects();
            if (!roots.isEmpty()) {
                if (auto *win = qobject_cast<QQuickWindow *>(roots.first())) {
                    QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
                    win->grabWindow().save(screenshotPath);
                }
            }
            QCoreApplication::quit();
        });
    } else if (testStartup) {
        QTimer::singleShot(900, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
