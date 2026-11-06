#pragma once

#include <QIcon>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace tandem {

// "Open with" on macOS: the apps Launch Services offers, as in Finder.
struct MacApp {
    QString path;  // the .app bundle
    QString name;
    bool isDefault = false;
};

// The apps that can open every one of paths; the default for the first one first.
QList<MacApp> macAppsForAll(const QStringList& paths);
QIcon macAppIcon(const QString& appPath);  // UI thread; kept once drawn
// Opens paths in the app. done gets an error message, empty on success, on the UI thread.
void macOpenWith(const QString& appPath, const QStringList& paths, std::function<void(const QString& error)> done);

}  // namespace tandem
