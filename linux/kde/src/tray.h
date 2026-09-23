// Tray — the macOS app's menu bar item, as a KStatusNotifierItem.
//
// One monochrome bird for every state (the pill shows the state). Left
// click starts or stops dictation; the menu is rebuilt each time it opens,
// from live state, in the macOS order (MenuBarContent.swift):
//   Start/Stop Dictation · the state · the last error ·
//   Typing (KWin's permission — the Linux twin of Accessibility) ·
//   Start at Login · Settings… · Open Config File ·
//   mynah <version> · <shortcut> · Quit mynah
// When another mynah owns the engine socket (the headless service), the
// menu offers to stop it and take over.

#pragma once

#include <QObject>

class KStatusNotifierItem;
class QMenu;
class EngineBridge;
class Shortcut;

class Tray : public QObject {
    Q_OBJECT
public:
    Tray(EngineBridge* bridge, Shortcut* shortcut, QObject* parent = nullptr);

signals:
    void settingsRequested();
    void quitRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();
    void refreshIcon();
    void takeOver();

    EngineBridge* bridge_;
    Shortcut* shortcut_;
    KStatusNotifierItem* item_;
    QMenu* menu_;
};
