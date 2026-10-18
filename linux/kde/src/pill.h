// Pill — the floating indicator. Its concept (2026-10-19): the bird in a
// dark circle whose rim pulses with the voice — three wavy rings, one per
// band group; the macOS IndicatorPanel still draws the earlier capsule with
// bars.
//
// The circle as tall as the macOS pill times kScale (pill.cpp), its bottom
// 80 px above the screen's edge, centred, in a square window with room for
// the rings; a layer-shell overlay, so it floats over everything, takes no
// focus and lets clicks through. The core draws the bird and the rings
// (BirdItem, mynah_bird_render_pill); the circle is the only blurred, dark
// part. Fades in and out over 0.18 s.
// Shown when `show_indicator` is on: always with `idle_visible`, otherwise
// while a session is engaged — both read once at launch, as on macOS
// ("takes effect after you reopen").

#pragma once

#include <QObject>

class QQuickView;
class QTimer;
class EngineBridge;

class Pill : public QObject {
    Q_OBJECT
    // Drives the QML fade; the window itself is unmapped once faded out.
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    // The size factor over the macOS pill's numbers (kScale in pill.cpp),
    // the window's side and the circle's diameter, in px.
    Q_PROPERTY(qreal scale READ scale CONSTANT)
    Q_PROPERTY(int side READ side CONSTANT)
    Q_PROPERTY(int circle READ circle CONSTANT)

public:
    Pill(EngineBridge* bridge, QObject* parent = nullptr);
    ~Pill() override;

    bool shown() const { return shown_; }
    qreal scale() const;
    int side() const;
    int circle() const;

signals:
    void shownChanged();

private:
    void update();

    EngineBridge* bridge_;
    QQuickView* view_ = nullptr;
    QTimer* hide_timer_ = nullptr;
    bool enabled_ = true;      // show_indicator, at launch
    bool idle_visible_ = false; // idle_visible, at launch
    bool shown_ = false;
};
