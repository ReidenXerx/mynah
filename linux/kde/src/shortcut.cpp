#include "shortcut.h"

#include <KGlobalAccel>
#include <QAction>

#include "engine_bridge.h"

namespace {

const QKeySequence kDefault(Qt::META | Qt::ALT | Qt::Key_D);

} // namespace

Shortcut::Shortcut(EngineBridge* bridge, QObject* parent)
    : QObject(parent), bridge_(bridge), action_(new QAction(this)) {
    action_->setObjectName(QStringLiteral("toggle-dictation"));
    action_->setText(QStringLiteral("Start or stop dictation"));
    action_->setProperty("componentName", QStringLiteral("mynah"));
    action_->setProperty("componentDisplayName", QStringLiteral("mynah"));
    KGlobalAccel::self()->setDefaultShortcut(action_, {kDefault});
    KGlobalAccel::self()->setShortcut(action_, {kDefault}); // autoloads the user's own

    connect(action_, &QAction::triggered, this, [this] {
        if (!pushToTalk()) bridge_->toggle();
    });
    // Press and release, for push-to-talk. A key repeat is not a second
    // press here: KGlobalAccel reports the transition once.
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutActiveChanged, this,
            [this](QAction* action, bool active) {
                if (action != action_ || !pushToTalk()) return;
                if (active) bridge_->startDictation();
                else bridge_->stopDictation();
            });
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this,
            [this](QAction* action, const QKeySequence&) {
                if (action == action_) emit keySequenceChanged();
            });
}

bool Shortcut::pushToTalk() const {
    return bridge_->config().value(QStringLiteral("trigger")).toString() == QLatin1String("ptt");
}

QKeySequence Shortcut::keySequence() const {
    const QList<QKeySequence> keys = KGlobalAccel::self()->shortcut(action_);
    return keys.isEmpty() ? QKeySequence() : keys.first();
}

QString Shortcut::text() const {
    const QKeySequence keys = keySequence();
    return keys.isEmpty() ? QStringLiteral("none") : keys.toString(QKeySequence::NativeText);
}

void Shortcut::setKeySequence(const QKeySequence& sequence) {
    KGlobalAccel::self()->setShortcut(action_, {sequence}, KGlobalAccel::NoAutoloading);
    emit keySequenceChanged();
}

void Shortcut::resetKeySequence() { setKeySequence(kDefault); }
