#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <atomic>

namespace tandem {

struct DesktopApp {
    QString id;     // e.g. "org.kde.kate.desktop"
    QString name;
    QString icon;
    QString exec;
    QStringList mimeTypes;
    bool terminal = false;
};

// Installed applications (.desktop files) and the user's mimeapps.list,
// loaded once in the background. Used for "Open with".
class DesktopApps {
public:
    static DesktopApps& instance();

    void loadAsync();
    bool isLoaded() const { return m_loaded.load(); }

    // Apps that handle any type in mimeChain (most specific first), with the
    // user's default application first.
    QList<DesktopApp> appsFor(const QStringList& mimeChain) const;
    // The app a double-click should use: the user's default, else the best
    // registered match. Empty id if nothing handles the type.
    DesktopApp defaultFor(const QStringList& mimeChain) const;
    QString defaultTerminal() const;

    // Makes appId the default for mime: updates [Default Applications] in
    // $XDG_CONFIG_HOME/mimeapps.list, keeping everything else in the file.
    // Blocking (file I/O): call from a worker. Returns false and sets error on failure.
    bool setDefault(const QString& mime, const QString& appId, QString* error = nullptr);

    // Expands Exec field codes (%f %F %u %U ...) and starts the app detached.
    static bool launch(const DesktopApp& app, const QStringList& paths, const QString& workDir,
                       const QString& terminal);
    // Builds argv lists for launching; one entry per process to start.
    static QList<QStringList> commandLines(const QString& exec, const QStringList& paths);

private:
    DesktopApps() = default;
    void load();

    mutable QMutex m_mutex;
    std::atomic<bool> m_loaded{false};
    QHash<QString, DesktopApp> m_apps;              // by id
    QHash<QString, QStringList> m_byMime;           // mime -> ids (from .desktop files)
    QHash<QString, QStringList> m_defaults;         // mime -> ids (mimeapps.list)
    QHash<QString, QStringList> m_added;            // mime -> ids (mimeapps.list)
    QHash<QString, QStringList> m_removed;          // mime -> ids (mimeapps.list)
    QString m_terminal;
};

}  // namespace tandem
