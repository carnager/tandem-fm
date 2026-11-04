#pragma once

#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

#include <optional>

class QFileSystemWatcher;
class QTimer;

namespace tandem {

// A server the user saved: servers.toml, next to the settings.
//
//   [[server]]
//   name = "NAS"
//   type = "smb"            # or "sftp"
//   host = "nas.lan"
//   port = 445              # optional
//   user = "bob"            # optional
//   domain = "WORKGROUP"    # smb only
//   key-file = "~/.ssh/id"  # sftp only
//   folder = "/media"       # where to start; empty: the login's home
//
// Passwords live in the keyring, never in the file.
struct SavedServer {
    QString name;
    QString type = QStringLiteral("sftp");
    QString host;
    int port = 0;  // 0: the protocol's default
    QString user;
    QString domain;
    QString keyFile;
    QString folder;

    // "sftp://bob@host:2222/srv", "smb://WORKGROUP;bob@nas/media".
    QString uri() const;
    // The server part only, as locations name it ("sftp://bob@host:2222").
    QString authority() const;
    bool operator==(const SavedServer&) const = default;
};

// A server address ("sftp://bob@host/srv") as a saved server to edit.
SavedServer serverFromUri(const QString& uri);

QString serversToml(const QList<SavedServer>& servers);
bool parseServersToml(const QString& text, QList<SavedServer>* out, QString* error = nullptr);

// Concrete hosts from an OpenSSH config (Include followed; wildcard
// patterns and Match blocks skipped), as saved servers to pick from.
// path empty: ~/.ssh/config.
QList<SavedServer> readSshConfig(const QString& path = {});

// The saved servers, read from the file and watched: saving it in an editor
// applies at once. Lookups may come from any thread.
class ServerList : public QObject {
    Q_OBJECT
public:
    static ServerList& instance();

    QString path() const;
    bool load();  // false with error() on a broken file; the old list stays
    QString error() const;
    QList<SavedServer> servers() const;
    bool save(const QList<SavedServer>& servers);
    // The saved server a location belongs to (same type, host, port, user).
    std::optional<SavedServer> find(const QString& location) const;

signals:
    void changed();

private:
    ServerList();
    void watch();
    mutable QMutex m_mutex;
    QList<SavedServer> m_servers;
    QString m_error;
    QString m_written;
    QFileSystemWatcher* m_watcher = nullptr;
    QTimer* m_reload = nullptr;
};

}  // namespace tandem
