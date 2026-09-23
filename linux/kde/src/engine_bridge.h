// EngineBridge — the running engine, as the UI sees it.
//
// Owns the shared Runtime (engine, typing, socket, microphone) and turns its
// events — delivered on engine threads — into Qt properties and signals on
// the GUI thread (queued: the runtime's callbacks must never block). The
// tray, the pill and the settings window all read from here; QML gets it
// as the `Mynah` context property.

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <memory>

namespace mynah::runtime {
class Runtime;
}

class EngineBridge : public QObject {
    Q_OBJECT
    // "idle", "loading", "listening", "transcribing" — as the protocol says.
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool engaged READ engaged NOTIFY stateChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double level READ level NOTIFY levelChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariantMap config READ config NOTIFY configChanged)
    Q_PROPERTY(QString version READ version CONSTANT)

public:
    explicit EngineBridge(QObject* parent = nullptr);
    ~EngineBridge() override;

    // Starts the runtime; false with lastError set (another mynah owns the
    // socket, no microphone, …). May be called again after a failure.
    bool start();
    void shutdown();

    QString state() const { return state_; }
    bool engaged() const { return state_ != QLatin1String("idle"); }
    bool running() const { return running_; }
    double level() const { return level_; }
    QString lastError() const { return last_error_; }
    QVariantMap config() const { return config_; }
    QString version() const;

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void startDictation();
    Q_INVOKABLE void stopDictation();
    // Set one setting through the engine (validated, saved read-modify-
    // write, reloaded); false when the engine refused it.
    Q_INVOKABLE bool setConfig(const QString& key, const QVariant& value);
    Q_INVOKABLE QString configPath() const;
    // Why typing cannot work right now (KWin's permission, a missing tool),
    // or empty when it can.
    Q_INVOKABLE QString typingProblem() const;

signals:
    void stateChanged();
    void runningChanged();
    void levelChanged();
    void lastErrorChanged();
    void configChanged();
    void quitRequested(); // `mynah quit` on the socket

private:
    void setState(const QString& state);
    void setLastError(const QString& error);
    void reloadConfig();

    std::unique_ptr<mynah::runtime::Runtime> runtime_;
    QString state_ = QStringLiteral("idle");
    bool running_ = false;
    double level_ = 0.0;
    QString last_error_;
    QVariantMap config_;
};
