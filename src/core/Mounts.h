#pragma once

#include <QString>
#include <QList>

namespace tandem {

struct MountPoint {
    QString path;
    QString device;
    QString fsType;
    bool isNetwork = false;
    bool isRemovable = false;  // under /run/media or /media
};

// Reads the platform mount table without network round trips and filters
// system mounts. macOS uses caller-owned getfsstat storage for thread safety.
QList<MountPoint> listMounts();

// NFS, SMB/CIFS, FUSE (sshfs, rclone, GVfs), Ceph, 9p: changes made by other
// machines don't reach inotify there. Blocking (statfs); call from a worker.
bool isNetworkFilesystem(const QString& path);

}  // namespace tandem
