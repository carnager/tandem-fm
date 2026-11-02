#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace tandem {

// What GIO knows about: network mounts (GVfs) and drives that aren't mounted
// yet. Kernel mounts are listed by listMounts() instead.
struct Place {
    QString name;
    QString path;      // local path (GVfs FUSE folder for network mounts); empty if not mounted
    QString volumeId;  // set for a volume that can be mounted
    QString iconName;
    bool network = false;
};

// Mounting through GIO/GVfs: sftp, smb, dav(s), ftp, nfs, mtp, … Password and
// host-key questions are shown as dialogs over `parent`; passwords can be
// kept in the keyring. All calls are asynchronous (GLib main loop = Qt's).
class Network : public QObject {
    Q_OBJECT
public:
    static Network& instance();
    static bool available();  // built with GIO

    using Done = std::function<void(const QString& path, const QString& error)>;
    void mount(const QString& uri, QWidget* parent, Done done);
    void mountVolume(const QString& volumeId, QWidget* parent, Done done);
    // Unmounts (or ejects) whatever GIO mount contains `path`.
    void unmount(const QString& path, QWidget* parent, std::function<void(const QString& error)> done);
    bool canUnmount(const QString& path) const;

    QList<Place> places() const;

    QStringList recentServers() const;
    void rememberServer(const QString& uri);
    void forgetServer(const QString& uri);

signals:
    void placesChanged();

private:
    Network();
};

}  // namespace tandem
