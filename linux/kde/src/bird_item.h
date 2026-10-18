// BirdItem — the pill's animated bird, as QML sees it (`Bird` in
// `import org.mynah`).
//
// The core renders the bird and decides how it moves (mynah_bird_*, the
// same frames and behaviour as the macOS pill); this item only hands it the
// engine's state, its model status, the voice and a clock, and shows the
// bitmap: the bird alone, or with `waves` the whole pill picture — the bird
// in the left height×height square and the voice streaming out of it as
// waves (mynah_bird_render_pill). It renders on the GUI thread — the
// bird's one thread — and paint() just draws the last image.
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
    // EngineBridge::level and ::bands: the voice the waves follow.
    Q_PROPERTY(qreal level READ level WRITE setLevel NOTIFY voiceChanged)
    Q_PROPERTY(QList<qreal> bands READ bands WRITE setBands NOTIFY voiceChanged)
    // Draw the pill picture (bird and waves) instead of the bird alone.
    Q_PROPERTY(bool waves READ waves WRITE setWaves NOTIFY wavesChanged)

public:
    explicit BirdItem(QQuickItem* parent = nullptr);
    ~BirdItem() override;

    QString engineState() const { return engine_state_; }
    void setEngineState(const QString& state);
    QString modelStatus() const { return model_status_; }
    void setModelStatus(const QString& status);
    qreal level() const { return level_; }
    void setLevel(qreal level);
    QList<qreal> bands() const { return bands_; }
    void setBands(const QList<qreal>& bands);
    bool waves() const { return waves_; }
    void setWaves(bool waves);

    void paint(QPainter* painter) override;

signals:
    void engineStateChanged();
    void modelStatusChanged();
    void voiceChanged();
    void wavesChanged();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    // Render the bird as it is now and schedule a repaint; keeps a ~60 Hz
    // timer running while the bird moves, and stops it once it rests.
    void tick();
    void readReducedMotion();
    double now() const { return double(clock_.nsecsElapsed()) / 1e9; }
    void passVoice(); // level_ and bands_ to the core

    mynah_bird* bird_ = nullptr;
    QString engine_state_ = QStringLiteral("idle");
    QString model_status_ = QStringLiteral("unloaded"); // as the core's bird starts
    qreal level_ = 0.0;
    QList<qreal> bands_;
    bool waves_ = false;
    QElapsedTimer clock_;
    QTimer timer_;
    QImage image_;
    KConfigWatcher::Ptr kdeglobals_;
};
