// SettingsWindow — the macOS app's Settings (SettingsView.swift), in QML:
// General, Recognition, Sensitivity; every edit goes to the engine one key
// at a time (mynah_config_set) and the window shows what it accepted.
//
// ModelManager is the Recognition tab's model section: the Linux models
// (turbo for a GPU, small and base for a CPU, Silero for the VAD), which
// one is in use, where speech runs, and verified downloads with progress.

#pragma once

#include <QObject>
#include <QVariantList>

#include <atomic>
#include <memory>

class QQmlApplicationEngine;
class EngineBridge;
class Shortcut;

class ModelManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList models READ models NOTIFY changed)
    Q_PROPERTY(bool vadOnDisk READ vadOnDisk NOTIFY changed)
    Q_PROPERTY(QString device READ device NOTIFY changed)
    Q_PROPERTY(QString downloading READ downloading NOTIFY progressChanged)
    Q_PROPERTY(double received READ received NOTIFY progressChanged)
    Q_PROPERTY(double total READ total NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY progressChanged)

public:
    explicit ModelManager(EngineBridge* bridge, QObject* parent = nullptr);
    ~ModelManager() override;

    QVariantList models() const;
    bool vadOnDisk() const;
    QString device() const;
    QString downloading() const { return downloading_; }
    double received() const { return received_; }
    double total() const { return total_; }
    QString error() const { return error_; }

    // "large-v3-turbo", "small", "base" or "vad"; one at a time.
    Q_INVOKABLE void download(const QString& alias);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE static QString formatBytes(double bytes);

signals:
    void changed();
    void progressChanged();

private:
    EngineBridge* bridge_;
    QString downloading_;
    double received_ = 0;
    double total_ = 0;
    QString error_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};

class SettingsWindow : public QObject {
    Q_OBJECT
public:
    SettingsWindow(EngineBridge* bridge, Shortcut* shortcut, QObject* parent = nullptr);
    ~SettingsWindow() override;

    void show();

private:
    EngineBridge* bridge_;
    Shortcut* shortcut_;
    ModelManager* models_;
    QQmlApplicationEngine* engine_ = nullptr;
};
