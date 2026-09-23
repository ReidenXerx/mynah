// mynah-kde — dictation for Plasma: a tray, a global shortcut, the pill,
// a settings window, over the engine the headless `mynah` runs.
//
// One instance per session (KDBusService): launching it again opens
// Settings. It serves the engine socket like `mynah` does, so `mynah
// toggle`, `mynah watch` and scripts work against it; when the headless
// service already owns that socket, the tray offers to take over.

#include <KAboutData>
#include <KDBusService>
#include <KSignalHandler>
#include <QApplication>
#include <QQuickStyle>

#include <csignal>

#include "engine_bridge.h"
#include "pill.h"
#include "settings_window.h"
#include "shortcut.h"
#include "tray.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false); // it lives in the tray
    app.setDesktopFileName(QStringLiteral("mynah-kde"));

    KAboutData about(QStringLiteral("mynah"), QStringLiteral("mynah"),
                     QStringLiteral(MYNAH_VERSION),
                     QStringLiteral("Say it, and it types where you are"),
                     KAboutLicense::MIT);
    about.setHomepage(QStringLiteral("https://duduphudu.app/mynah/"));
    KAboutData::setApplicationData(about); // also KGlobalAccel's component name

    // Plasma's own look for the settings window, unless one was asked for.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));

    KDBusService service(KDBusService::Unique);

    EngineBridge bridge;
    bridge.start(); // a failure is shown in the tray, which can retry
    Shortcut shortcut(&bridge);
    Pill pill(&bridge); // reads show_indicator/idle_visible once, as on macOS
    SettingsWindow settings(&bridge, &shortcut);
    Tray tray(&bridge, &shortcut);

    QObject::connect(&tray, &Tray::settingsRequested, &settings, &SettingsWindow::show);
    QObject::connect(&service, &KDBusService::activateRequested, &settings,
                     [&settings] { settings.show(); });
    QObject::connect(&tray, &Tray::quitRequested, &app, &QApplication::quit);
    QObject::connect(&bridge, &EngineBridge::quitRequested, &app, &QApplication::quit);
    QObject::connect(&app, &QApplication::aboutToQuit, &bridge, &EngineBridge::shutdown);

    // Logout and `kill` send SIGTERM: quit properly — the engine destroyed,
    // what is queued typed, the socket removed — instead of dying mid-way.
    KSignalHandler::self()->watchSignal(SIGTERM);
    KSignalHandler::self()->watchSignal(SIGINT);
    QObject::connect(KSignalHandler::self(), &KSignalHandler::signalReceived, &app,
                     [](int) { QApplication::quit(); });

    return app.exec();
}
