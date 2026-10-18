// BirdItem — the pill's animated bird, as QML sees it (`Bird` in
// `import org.mynah`).
//
// The core renders the bird and decides how it moves (mynah_bird_*, the
// same frames and behaviour as the macOS pill); this item only hands it the
// engine's state, its model status and a clock, and shows the bitmap. It
// renders on the GUI thread — the bird's one thread — and paint() just
// draws the last image.
//
// Plasma's "Animation speed" set to Instant (AnimationDurationFactor=0 in
// kdeglobals) is reduce-motion: the bird jumps to its end pose.

#pragma once

#include <KConfigWatcher>
#include <QElapsedTimer>
#include <QImage>
#include <QQuickPaintedItem>
#include <QTimer>

struct mynah_bird;

class BirdItem : public QQuickPaintedItem {
    Q_OBJECT
    // The engine's state as the protocol words it — EngineBridge::state:
    // "idle", "loading", "listening", "transcribing". (Not `state`: every
    // QQuickItem already has one, for QML States.)
    Q_PROPERTY(QString engineState READ engineState WRITE setEngineState NOTIFY engineStateChanged)
    // EngineBridge::modelStatus: "unloaded" (the bird sleeps), "loading",
    // "loaded".
    Q_PROPERTY(QString modelStatus READ modelStatus WRITE setModelStatus NOTIFY modelStatusChanged)

public:
    explicit BirdItem(QQuickItem* parent = nullptr);
    ~BirdItem() override;

    QString engineState() const { return engine_state_; }
    void setEngineState(const QString& state);
    QString modelStatus() const { return model_status_; }
    void setModelStatus(const QString& status);

    void paint(QPainter* painter) override;

signals:
    void engineStateChanged();
    void modelStatusChanged();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    // Render the bird as it is now and schedule a repaint; keeps a ~60 Hz
    // timer running while the bird moves, and stops it once it rests.
    void tick();
    void readReducedMotion();
    double now() const { return double(clock_.nsecsElapsed()) / 1e9; }

    mynah_bird* bird_ = nullptr;
    QString engine_state_ = QStringLiteral("idle");
    QString model_status_ = QStringLiteral("unloaded"); // as the core's bird starts
    QElapsedTimer clock_;
    QTimer timer_;
    QImage image_;
    KConfigWatcher::Ptr kdeglobals_;
};
