// Shortcut — the global dictation key, through KGlobalAccel.
//
// One action, "toggle-dictation", default Meta+Alt+D (the Omarchy plugin's
// key too), rebindable here or in System Settings → Shortcuts; KDE keeps it
// in kglobalshortcutsrc, not in config.toml (the `hotkey` key there is the
// macOS app's). `trigger` decides what it does: toggle — each press starts
// or ends a session; ptt — hold to talk, press starts and release stops.

#pragma once

#include <QKeySequence>
#include <QObject>

class QAction;
class EngineBridge;

class Shortcut : public QObject {
    Q_OBJECT
    Q_PROPERTY(QKeySequence keySequence READ keySequence NOTIFY keySequenceChanged)
    Q_PROPERTY(QString text READ text NOTIFY keySequenceChanged)

public:
    Shortcut(EngineBridge* bridge, QObject* parent = nullptr);

    QKeySequence keySequence() const;
    QString text() const; // "Meta+Alt+D", or "none"

    // From the settings window's key recorder; an empty sequence unbinds.
    Q_INVOKABLE void setKeySequence(const QKeySequence& sequence);
    Q_INVOKABLE void resetKeySequence();

signals:
    void keySequenceChanged();

private:
    bool pushToTalk() const;

    EngineBridge* bridge_;
    QAction* action_;
};
