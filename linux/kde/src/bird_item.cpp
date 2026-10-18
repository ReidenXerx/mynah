#include "bird_item.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QPainter>
#include <QQuickWindow>
#include <QtMath>

#include "mynah/mynah.h"

namespace {

mynah_state engine_state(const QString& state) {
    if (state == QLatin1String("loading")) return MYNAH_LOADING;
    if (state == QLatin1String("listening")) return MYNAH_LISTENING;
    if (state == QLatin1String("transcribing")) return MYNAH_TRANSCRIBING;
    return MYNAH_IDLE;
}

mynah_model_status model_status(const QString& status) {
    if (status == QLatin1String("loading")) return MYNAH_MODEL_LOADING;
    if (status == QLatin1String("loaded")) return MYNAH_MODEL_LOADED;
    return MYNAH_MODEL_UNLOADED;
}

} // namespace

BirdItem::BirdItem(QQuickItem* parent) : QQuickPaintedItem(parent), bird_(mynah_bird_create()) {
    if (!bird_) qWarning("mynah: the pill's bird could not be rendered");
    clock_.start();
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, &BirdItem::tick);

    // Live: System Settings announces the change, the watcher re-reads it.
    kdeglobals_ = KConfigWatcher::create(KSharedConfig::openConfig(QStringLiteral("kdeglobals")));
    connect(kdeglobals_.data(), &KConfigWatcher::configChanged, this,
            [this](const KConfigGroup& group, const QByteArrayList& names) {
                if (group.name() == QLatin1String("KDE") && names.contains("AnimationDurationFactor"))
                    readReducedMotion();
            });
    readReducedMotion();
}

BirdItem::~BirdItem() { mynah_bird_destroy(bird_); }

void BirdItem::setEngineState(const QString& state) {
    if (state == engine_state_) return;
    engine_state_ = state;
    if (bird_) mynah_bird_set_state(bird_, engine_state(state), now());
    emit engineStateChanged();
    tick();
}

void BirdItem::setModelStatus(const QString& status) {
    if (status == model_status_) return;
    model_status_ = status;
    if (bird_) mynah_bird_set_model(bird_, model_status(status), now());
    emit modelStatusChanged();
    tick();
}

void BirdItem::readReducedMotion() {
    const KConfigGroup group = kdeglobals_->config()->group(QStringLiteral("KDE"));
    const double factor = group.readEntry("AnimationDurationFactor", 1.0);
    if (bird_) mynah_bird_set_reduced_motion(bird_, factor <= 0.0 ? 1 : 0);
    tick();
}

void BirdItem::tick() {
    if (!bird_ || !window() || width() <= 0 || height() <= 0) {
        timer_.stop();
        return;
    }
    // Device pixels, so the bird is sharp at any scale factor.
    const qreal ratio = window()->effectiveDevicePixelRatio();
    const QSize size(qCeil(width() * ratio), qCeil(height() * ratio));
    if (image_.size() != size) image_ = QImage(size, QImage::Format_ARGB32_Premultiplied);
    // ARGB32 rows are exactly width * 4 bytes, and Qt keeps the pixels as
    // 0xAARRGGBB words — the layout mynah_bird_render writes.
    const int moving = mynah_bird_render(bird_, now(), reinterpret_cast<uint32_t*>(image_.bits()),
                                         size.width(), size.height());
    if (moving < 0) image_.fill(Qt::transparent);
    image_.setDevicePixelRatio(ratio);
    update();
    if (moving == 1) {
        if (!timer_.isActive()) timer_.start();
    } else {
        timer_.stop();
    }
}

void BirdItem::paint(QPainter* painter) {
    if (image_.isNull()) return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(QRectF(0, 0, width(), height()), image_);
}

void BirdItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) tick();
}

void BirdItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickPaintedItem::itemChange(change, value);
    // Into a window, or onto a screen with another scale: render for it.
    if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged) tick();
}
