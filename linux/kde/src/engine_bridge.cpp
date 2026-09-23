#include "engine_bridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>

#include <cstdlib>

#include "mynah/mynah.h"
#include "runtime.hpp"

namespace {

QString take_c_string(char* text) {
    QString result = text ? QString::fromUtf8(text) : QString();
    std::free(text);
    return result;
}

} // namespace

EngineBridge::EngineBridge(QObject* parent) : QObject(parent) {}

EngineBridge::~EngineBridge() { shutdown(); }

QString EngineBridge::version() const { return QString::fromUtf8(mynah_version()); }

bool EngineBridge::start() {
    if (running_) return true;
    // Engine threads call these; everything is re-posted to this object's
    // thread, and dropped if it is gone by then.
    QPointer<EngineBridge> self(this);
    auto post = [self](auto&& fn) {
        if (self) QMetaObject::invokeMethod(self.data(), std::forward<decltype(fn)>(fn),
                                            Qt::QueuedConnection);
    };
    mynah::runtime::Listener listener;
    listener.state = [post, self](mynah_state state) {
        const QString word = QString::fromLatin1(mynah::runtime::state_word(state));
        post([self, word] { if (self) self->setState(word); });
    };
    listener.level = [post, self](float level, const float*) {
        // The macOS pill reads the level alone (five bars from one value);
        // the twelve bands go to the socket for the Omarchy pill.
        post([self, level] {
            if (!self) return;
            self->level_ = double(level);
            emit self->levelChanged();
        });
    };
    listener.problem = [post, self](const std::string& code, const std::string& message) {
        const QString text = QString::fromStdString(message);
        const bool fatal = code == "no_model" || code == "model_load_failed";
        post([self, text, fatal] {
            if (!self) return;
            self->setLastError(text);
            if (fatal) self->setState(QStringLiteral("idle"));
        });
    };
    listener.model = [post, self](mynah_model_status status, const std::string&) {
        if (status != MYNAH_MODEL_LOADED) return;
        // As on macOS: a model that loaded answers any model complaint.
        post([self] {
            if (self && self->last_error_.contains(QLatin1String("model"), Qt::CaseInsensitive))
                self->setLastError(QString());
        });
    };
    listener.quit = [post, self] {
        post([self] { if (self) emit self->quitRequested(); });
    };

    runtime_ = std::make_unique<mynah::runtime::Runtime>(std::move(listener));
    std::string error;
    if (!runtime_->start(error)) {
        runtime_.reset();
        setLastError(QString::fromStdString(error));
        return false;
    }
    running_ = true;
    emit runningChanged();
    setLastError(QString());
    reloadConfig();
    return true;
}

void EngineBridge::shutdown() {
    if (!runtime_) return;
    runtime_->stop();
    runtime_.reset();
    running_ = false;
    emit runningChanged();
    setState(QStringLiteral("idle"));
}

void EngineBridge::toggle() {
    if (runtime_) mynah_toggle(runtime_->engine());
}
void EngineBridge::startDictation() {
    if (runtime_) mynah_start(runtime_->engine());
}
void EngineBridge::stopDictation() {
    if (runtime_) mynah_stop(runtime_->engine());
}

bool EngineBridge::setConfig(const QString& key, const QVariant& value) {
    if (!runtime_) return false;
    // The engine takes the JSON encoding of one value: serialise it as the
    // only element of an array and strip the brackets.
    QByteArray json = QJsonDocument(QJsonArray{QJsonValue::fromVariant(value)})
                          .toJson(QJsonDocument::Compact);
    json = json.mid(1, json.size() - 2);
    const int result = mynah_config_set(runtime_->engine(), key.toUtf8().constData(),
                                        json.constData());
    reloadConfig(); // what the engine accepted, not what was asked
    return result == 0;
}

QString EngineBridge::configPath() const {
    if (!runtime_) return QString();
    return take_c_string(mynah_config_path(runtime_->engine()));
}

QString EngineBridge::typingProblem() const {
    if (!runtime_) return QString();
    const auto [ok, remedy] = runtime_->typing_check();
    return ok ? QString() : QString::fromStdString(remedy);
}

void EngineBridge::setState(const QString& state) {
    if (state == state_) return;
    const bool session_starts = state_ == QLatin1String("idle");
    state_ = state;
    if (state_ == QLatin1String("idle")) {
        level_ = 0.0;
        emit levelChanged();
    } else if (session_starts) {
        setLastError(QString()); // a new session starts clean, as on macOS
    }
    emit stateChanged();
}

void EngineBridge::setLastError(const QString& error) {
    if (error == last_error_) return;
    last_error_ = error;
    emit lastErrorChanged();
}

void EngineBridge::reloadConfig() {
    if (!runtime_) return;
    const QString json = take_c_string(mynah_config_json(runtime_->engine()));
    config_ = QJsonDocument::fromJson(json.toUtf8()).object().toVariantMap();
    emit configChanged();
}
