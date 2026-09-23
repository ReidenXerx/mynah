#include "settings_window.h"

#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "downloads.hpp"
#include "engine_bridge.h"
#include "models/table.hpp"
#include "mynah/mynah.h"
#include "shortcut.h"
#include "stt/stt.hpp"

namespace {

struct Offered {
    const mynah::models::ModelInfo* info;
    const char* title;
    const char* detail;
};

// The Linux tiers (docs/ENGINE-MIGRATION.md, "Linux model tiers").
const Offered kModels[] = {
    {&mynah::models::kTurbo, "Large v3 Turbo (recommended)",
     "Best accuracy, Russian included. Fast on a GPU, slow on a CPU."},
    {&mynah::models::kSmall, "Small", "Good accuracy, fast enough on a CPU."},
    {&mynah::models::kBase, "Base", "The fastest and least accurate; for slow machines."},
};

std::filesystem::path model_path(const mynah::models::ModelInfo* info) {
    return std::filesystem::path(mynah::models::download_dir()) / std::string(info->filename);
}

QVariantList languages() {
    QVariantList list;
    const int count = mynah_language_count();
    std::vector<std::pair<QString, QString>> named;
    for (int id = 0; id < count; ++id) {
        const char* code = mynah_language_code(id);
        const char* name = mynah_language_name(id);
        if (!code || !name) continue;
        QString title = QString::fromUtf8(name);
        if (!title.isEmpty()) title[0] = title[0].toUpper();
        named.emplace_back(QStringLiteral("%1 (%2)").arg(title, QString::fromUtf8(code)),
                           QString::fromUtf8(code));
    }
    std::sort(named.begin(), named.end());
    list << QVariantMap{{QStringLiteral("name"), QStringLiteral("Auto-detect")},
                        {QStringLiteral("code"), QStringLiteral("auto")}};
    for (const auto& [name, code] : named)
        list << QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("code"), code}};
    return list;
}

} // namespace

// --- ModelManager ---------------------------------------------------------------------

ModelManager::ModelManager(EngineBridge* bridge, QObject* parent)
    : QObject(parent), bridge_(bridge) {
    connect(bridge_, &EngineBridge::configChanged, this, &ModelManager::changed);
}

ModelManager::~ModelManager() {
    if (cancel_) cancel_->store(true);
}

QVariantList ModelManager::models() const {
    // Which file the engine would load for the configured `model`.
    const QString configured = bridge_->config().value(QStringLiteral("model")).toString();
    char* found = mynah_find_model(configured.toUtf8().constData());
    const QString in_use = found ? QFileInfo(QString::fromUtf8(found)).fileName() : QString();
    std::free(found);

    QVariantList list;
    for (const Offered& offered : kModels) {
        const QString filename = QString::fromUtf8(offered.info->filename.data(),
                                                   qsizetype(offered.info->filename.size()));
        list << QVariantMap{
            {QStringLiteral("alias"), QString::fromUtf8(offered.info->alias.data(),
                                                        qsizetype(offered.info->alias.size()))},
            {QStringLiteral("title"), QString::fromUtf8(offered.title)},
            {QStringLiteral("detail"), QString::fromUtf8(offered.detail)},
            {QStringLiteral("filename"), filename},
            {QStringLiteral("bytes"), double(offered.info->approximate_bytes)},
            {QStringLiteral("onDisk"), std::filesystem::exists(model_path(offered.info))},
            {QStringLiteral("inUse"), filename == in_use},
        };
    }
    return list;
}

bool ModelManager::vadOnDisk() const {
    char* found = mynah_find_vad();
    const bool on_disk = found != nullptr;
    std::free(found);
    return on_disk;
}

QString ModelManager::device() const {
    const bool discrete = bridge_->config().value(QStringLiteral("gpu"), true).toBool();
    if (auto gpu = mynah::stt::pick_gpu(discrete)) return QString::fromStdString(gpu->name);
    return QStringLiteral("the CPU");
}

QString ModelManager::formatBytes(double bytes) {
    return QLocale().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeSIFormat);
}

void ModelManager::download(const QString& alias) {
    if (!downloading_.isEmpty()) return;
    const mynah::models::ModelInfo* info = alias == QLatin1String("vad")
                                               ? &mynah::models::kSileroVad
                                               : mynah::models::find(alias.toStdString());
    if (!info) return;
    downloading_ = alias;
    received_ = 0;
    total_ = double(info->approximate_bytes);
    error_.clear();
    emit progressChanged();

    cancel_ = std::make_shared<std::atomic<bool>>(false);
    QPointer<ModelManager> self(this);
    std::thread([self, info, cancel = cancel_] {
        auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        mynah::download::Result result = mynah::download::fetch(
            std::string(info->url), mynah::models::download_dir(), std::string(info->filename),
            std::string(info->sha256),
            [self, cancel, &last](std::uint64_t received, std::uint64_t total) {
                // ~10 updates a second is plenty for a progress bar.
                const auto now = std::chrono::steady_clock::now();
                if (now - last > std::chrono::milliseconds(100)) {
                    last = now;
                    QMetaObject::invokeMethod(
                        self.data(),
                        [self, received, total] {
                            if (!self) return;
                            self->received_ = double(received);
                            if (total) self->total_ = double(total);
                            emit self->progressChanged();
                        },
                        Qt::QueuedConnection);
                }
                return !cancel->load();
            });
        QMetaObject::invokeMethod(
            self.data(),
            [self, result, cancelled = cancel->load()] {
                if (!self) return;
                self->downloading_.clear();
                self->error_ = result.ok || cancelled ? QString()
                                                      : QString::fromStdString(result.error);
                emit self->progressChanged();
                emit self->changed();
            },
            Qt::QueuedConnection);
    }).detach();
}

void ModelManager::cancel() {
    if (cancel_) cancel_->store(true);
}

// --- SettingsWindow ---------------------------------------------------------------------

SettingsWindow::SettingsWindow(EngineBridge* bridge, Shortcut* shortcut, QObject* parent)
    : QObject(parent), bridge_(bridge), shortcut_(shortcut),
      models_(new ModelManager(bridge, this)) {}

SettingsWindow::~SettingsWindow() { delete engine_; }

void SettingsWindow::show() {
    if (!engine_) {
        engine_ = new QQmlApplicationEngine();
        engine_->rootContext()->setContextProperty(QStringLiteral("Mynah"), bridge_);
        engine_->rootContext()->setContextProperty(QStringLiteral("shortcut"), shortcut_);
        engine_->rootContext()->setContextProperty(QStringLiteral("models"), models_);
        engine_->rootContext()->setContextProperty(QStringLiteral("languages"), languages());
        engine_->load(QUrl(QStringLiteral("qrc:/qml/Settings.qml")));
    }
    if (engine_->rootObjects().isEmpty()) return;
    if (auto* window = qobject_cast<QQuickWindow*>(engine_->rootObjects().first())) {
        emit models_->changed(); // what is on disk may have changed meanwhile
        window->show();
        window->raise();
        window->requestActivate();
    }
}
