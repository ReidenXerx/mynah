#include "pill.h"

#include <KWindowEffects>
#include <LayerShellQt/Window>
#include <QQmlContext>
#include <QQuickView>
#include <QRegion>
#include <QTimer>

#include "engine_bridge.h"

namespace {

constexpr int kWidth = 168;
constexpr int kHeight = 44;
constexpr int kBottomMargin = 80;
constexpr int kFadeMs = 180;

// The capsule, for the blur: a rectangle between two circles.
QRegion capsule() {
    const int r = kHeight / 2;
    return QRegion(r, 0, kWidth - 2 * r, kHeight) +
           QRegion(0, 0, kHeight, kHeight, QRegion::Ellipse) +
           QRegion(kWidth - kHeight, 0, kHeight, kHeight, QRegion::Ellipse);
}

} // namespace

Pill::Pill(EngineBridge* bridge, QObject* parent) : QObject(parent), bridge_(bridge) {
    const QVariantMap config = bridge_->config();
    enabled_ = config.value(QStringLiteral("show_indicator"), true).toBool();
    idle_visible_ = config.value(QStringLiteral("idle_visible"), false).toBool();
    if (!enabled_) return;

    view_ = new QQuickView();
    view_->setColor(Qt::transparent);
    view_->setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                    Qt::WindowDoesNotAcceptFocus);
    view_->setResizeMode(QQuickView::SizeRootObjectToView);
    view_->resize(kWidth, kHeight);
    view_->rootContext()->setContextProperty(QStringLiteral("Mynah"), bridge_);
    view_->rootContext()->setContextProperty(QStringLiteral("pill"), this);
    view_->setSource(QUrl(QStringLiteral("qrc:/qml/Pill.qml")));

    // Before the window is first shown: the role is fixed at map time.
    if (auto* layer = LayerShellQt::Window::get(view_)) {
        layer->setScope(QStringLiteral("mynah-pill"));
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::AnchorBottom);
        layer->setMargins(QMargins(0, 0, 0, kBottomMargin));
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        // From the screen's edge, not above the panel: the macOS pill sits
        // 80 pt above the screen's bottom whatever the Dock does.
        layer->setExclusiveZone(-1);
    }

    hide_timer_ = new QTimer(this);
    hide_timer_->setSingleShot(true);
    hide_timer_->setInterval(kFadeMs + 20);
    connect(hide_timer_, &QTimer::timeout, this, [this] {
        if (!shown_) view_->hide();
    });

    connect(bridge_, &EngineBridge::stateChanged, this, &Pill::update);
    update();
}

Pill::~Pill() { delete view_; }

void Pill::update() {
    if (!view_) return;
    const bool wanted = idle_visible_ || bridge_->engaged();
    if (wanted == shown_) return;
    shown_ = wanted;
    if (wanted) {
        hide_timer_->stop();
        if (!view_->isVisible()) {
            view_->show();
            KWindowEffects::enableBlurBehind(view_, true, capsule());
        }
    } else {
        hide_timer_->start(); // after the fade
    }
    emit shownChanged();
}
