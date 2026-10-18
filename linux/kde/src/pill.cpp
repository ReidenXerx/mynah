#include "pill.h"

#include <KWindowEffects>
#include <LayerShellQt/Window>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QRegion>
#include <QTimer>

#include <cmath>

#include "bird_item.h"
#include "engine_bridge.h"
#include "mynah/mynah.h"

namespace {

// The circle is as tall as the macOS pill (44) times kScale — 2 is an
// experiment (2026-10-18) in making the bird easier to notice. The window
// is the square around it the core draws the rings in (MYNAH_PILL_CIRCLE:
// the circle's share of the side); its margin comes off the distance from
// the screen's edge, so the circle's bottom sits where the pill's was.
constexpr qreal kScale = 2.0;
constexpr int kCircle = int(44 * kScale);
const int kSide = int(std::ceil(kCircle / MYNAH_PILL_CIRCLE));
const int kRingMargin = (kSide - kCircle) / 2;
const int kBottomMargin = 80 - kRingMargin;
constexpr int kFadeMs = 180;

// The circle behind the bird, in the middle of the window: the only blurred
// part — the rings around it float over the desktop.
QRegion blur_region() { return QRegion(kRingMargin, kRingMargin, kCircle, kCircle, QRegion::Ellipse); }

} // namespace

Pill::Pill(EngineBridge* bridge, QObject* parent) : QObject(parent), bridge_(bridge) {
    const QVariantMap config = bridge_->config();
    enabled_ = config.value(QStringLiteral("show_indicator"), true).toBool();
    idle_visible_ = config.value(QStringLiteral("idle_visible"), false).toBool();
    if (!enabled_) return;

    qmlRegisterType<BirdItem>("org.mynah", 1, 0, "Bird");
    view_ = new QQuickView();
    view_->setColor(Qt::transparent);
    view_->setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                    Qt::WindowDoesNotAcceptFocus);
    view_->setResizeMode(QQuickView::SizeRootObjectToView);
    view_->resize(kSide, kSide);
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

qreal Pill::scale() const { return kScale; }
int Pill::side() const { return kSide; }
int Pill::circle() const { return kCircle; }

void Pill::update() {
    if (!view_) return;
    const bool wanted = idle_visible_ || bridge_->engaged();
    if (wanted == shown_) return;
    shown_ = wanted;
    if (wanted) {
        hide_timer_->stop();
        if (!view_->isVisible()) {
            view_->show();
            KWindowEffects::enableBlurBehind(view_, true, blur_region());
        }
    } else {
        hide_timer_->start(); // after the fade
    }
    emit shownChanged();
}
