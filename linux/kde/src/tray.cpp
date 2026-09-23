#include "tray.h"

#include <KStatusNotifierItem>
#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QSvgRenderer>
#include <QUrl>

#include "autostart.h"
#include "engine_bridge.h"
#include "shortcut.h"

namespace {

QString state_label(const QString& state) {
    if (state == QLatin1String("loading")) return QStringLiteral("Loading…");
    if (state == QLatin1String("listening")) return QStringLiteral("Listening…");
    if (state == QLatin1String("transcribing")) return QStringLiteral("Transcribing…");
    return QStringLiteral("Idle");
}

// Menus reach Plasma over D-Bus as plain labels — no word wrap — so a long
// message becomes several lines, broken at spaces.
QStringList wrapped(const QString& text, int width = 60) {
    QStringList lines;
    for (const QString& paragraph : text.split(QLatin1Char('\n'))) {
        QString line;
        for (const QString& word : paragraph.simplified().split(QLatin1Char(' '))) {
            if (!line.isEmpty() && line.size() + 1 + word.size() > width) {
                lines << line;
                line.clear();
            }
            line += (line.isEmpty() ? QString() : QStringLiteral(" ")) + word;
        }
        if (!line.isEmpty()) lines << line;
    }
    return lines;
}

void add_text(QMenu* menu, const QString& text) {
    for (const QString& line : wrapped(text)) menu->addAction(line)->setEnabled(false);
}

// The bird, in the panel's text colour: one template glyph for every state,
// as on macOS.
QIcon bird_icon() {
    QFile file(QStringLiteral(":/mynah-glyph.svg"));
    if (!file.open(QIODevice::ReadOnly)) return QIcon::fromTheme(QStringLiteral("audio-input-microphone"));
    const QColor ink = QGuiApplication::palette().color(QPalette::WindowText);
    QByteArray svg = file.readAll();
    svg.replace("#14120F", ink.name().toLatin1());
    QSvgRenderer renderer(svg);
    QIcon icon;
    for (int size : {16, 22, 32, 48, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace

Tray::Tray(EngineBridge* bridge, Shortcut* shortcut, QObject* parent)
    : QObject(parent), bridge_(bridge), shortcut_(shortcut),
      item_(new KStatusNotifierItem(QStringLiteral("mynah"), this)), menu_(new QMenu()) {
    item_->setCategory(KStatusNotifierItem::ApplicationStatus);
    item_->setStatus(KStatusNotifierItem::Active);
    item_->setTitle(QStringLiteral("mynah"));
    item_->setStandardActionsEnabled(false); // our own Quit, in the macOS place
    item_->setContextMenu(menu_);
    refreshIcon();

    // Left click: start or stop — the tray as a dictation button.
    connect(item_, &KStatusNotifierItem::activateRequested, this, [this](bool, const QPoint&) {
        if (bridge_->running()) bridge_->toggle();
        else rebuild();
    });
    connect(menu_, &QMenu::aboutToShow, this, &Tray::rebuild);
    connect(bridge_, &EngineBridge::stateChanged, this, [this] {
        item_->setToolTip(QStringLiteral("audio-input-microphone"), QStringLiteral("mynah"),
                          state_label(bridge_->state()));
    });
    qGuiApp->installEventFilter(this); // the palette: a light/dark switch recolours the bird
    rebuild();
}

void Tray::refreshIcon() { item_->setIconByPixmap(bird_icon()); }

bool Tray::eventFilter(QObject* watched, QEvent* event) {
    if (watched == qGuiApp && event->type() == QEvent::ApplicationPaletteChange) refreshIcon();
    return QObject::eventFilter(watched, event);
}

void Tray::rebuild() {
    menu_->clear();
    const QString keys = shortcut_->text();

    if (bridge_->running()) {
        QAction* toggle = menu_->addAction(
            (bridge_->engaged() ? QStringLiteral("Stop Dictation") : QStringLiteral("Start Dictation")) +
            QStringLiteral("\t") + keys);
        connect(toggle, &QAction::triggered, bridge_, &EngineBridge::toggle);
        menu_->addAction(state_label(bridge_->state()))->setEnabled(false);
    } else {
        menu_->addAction(QStringLiteral("mynah is not running"))->setEnabled(false);
    }

    if (!bridge_->lastError().isEmpty()) {
        menu_->addSeparator();
        add_text(menu_, bridge_->lastError());
        if (!bridge_->running()) {
            // The usual reason: the headless service owns the engine socket.
            if (bridge_->lastError().contains(QLatin1String("already running"))) {
                QAction* take = menu_->addAction(QStringLiteral("Stop It and Use This One"));
                connect(take, &QAction::triggered, this, &Tray::takeOver);
            }
            QAction* retry = menu_->addAction(QStringLiteral("Try Again"));
            connect(retry, &QAction::triggered, this, [this] { bridge_->start(); });
        }
    }
    menu_->addSeparator();

    // Typing: KWin's permission is to Linux what Accessibility is to macOS.
    if (bridge_->running()) {
        const QString problem = bridge_->typingProblem();
        if (problem.isEmpty()) {
            menu_->addAction(QStringLiteral("Typing: ready"))->setEnabled(false);
        } else {
            QAction* fix = menu_->addAction(QStringLiteral("Typing: not allowed…"));
            connect(fix, &QAction::triggered, this, [problem] {
                QMessageBox::information(nullptr, QStringLiteral("mynah cannot type yet"), problem);
            });
        }
    }

    QAction* login = menu_->addAction(QStringLiteral("Start at Login"));
    login->setCheckable(true);
    login->setChecked(autostart::enabled());
    connect(login, &QAction::toggled, this, [](bool on) { autostart::setEnabled(on); });

    QAction* settings = menu_->addAction(QStringLiteral("Settings…"));
    connect(settings, &QAction::triggered, this, &Tray::settingsRequested);

    QAction* config = menu_->addAction(QStringLiteral("Open Config File"));
    connect(config, &QAction::triggered, this, [this] {
        QString path = bridge_->configPath();
        if (path.isEmpty()) path = QDir::homePath() + QStringLiteral("/.config/mynah/config.toml");
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    menu_->addSeparator();
    menu_->addAction(QStringLiteral("mynah %1 · %2").arg(bridge_->version(), keys))->setEnabled(false);
    QAction* quit = menu_->addAction(QStringLiteral("Quit mynah"));
    connect(quit, &QAction::triggered, this, &Tray::quitRequested);
}

void Tray::takeOver() {
    // The headless engine, run by its user service: stop it (it stays
    // installed and enabled; this app is simply the one running now).
    QProcess::execute(QStringLiteral("systemctl"),
                      {QStringLiteral("--user"), QStringLiteral("stop"), QStringLiteral("mynah")});
    bridge_->start();
    rebuild();
}
