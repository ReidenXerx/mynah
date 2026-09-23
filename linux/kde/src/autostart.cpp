#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>

namespace autostart {

namespace {

QString entry_path() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/autostart/mynah-kde.desktop");
}

} // namespace

bool enabled() { return QFile::exists(entry_path()); }

bool setEnabled(bool on) {
    const QString path = entry_path();
    if (!on) return !QFile::exists(path) || QFile::remove(path);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QString exe = QCoreApplication::applicationFilePath();
    file.write(QStringLiteral("[Desktop Entry]\n"
                              "Type=Application\n"
                              "Name=mynah\n"
                              "Comment=Dictation: say it, and it types where you are\n"
                              "Exec=%1\n"
                              "Icon=audio-input-microphone\n"
                              "NoDisplay=true\n"
                              "X-KDE-autostart-phase=2\n")
                   .arg(exe)
                   .toUtf8());
    return file.error() == QFile::NoError;
}

} // namespace autostart
