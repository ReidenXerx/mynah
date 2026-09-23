// Pill — the floating indicator, as the macOS app draws it (IndicatorPanel).
//
// A 168×44 capsule, 80 px above the bottom edge of the screen, centred; a
// layer-shell overlay, so it floats over everything, takes no focus and
// lets clicks through. The bird and five bars in the state's tint; blur
// behind. Fades in and out over 0.18 s. Shown when `show_indicator` is on:
// always with `idle_visible`, otherwise while a session is engaged — both
// read once at launch, as on macOS ("takes effect after you reopen").

#pragma once

#include <QObject>

class QQuickView;
class QTimer;
class EngineBridge;

class Pill : public QObject {
    Q_OBJECT
    // Drives the QML fade; the window itself is unmapped once faded out.
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)

public:
    Pill(EngineBridge* bridge, QObject* parent = nullptr);
    ~Pill() override;

    bool shown() const { return shown_; }

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
