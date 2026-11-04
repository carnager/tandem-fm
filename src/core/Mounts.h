#pragma once

#include <QString>
#include <QList>
#include <QStringList>

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

// GVfs only says "Unable to open MTP device "001,019"" when another program
// holds the phone. This turns that into /dev/bus/usb/001/019 (empty for
// other errors) ...
QString usbNodeFromMtpError(const QString& error);
// ... and this names the programs that have it open, friendly where known
// ("Dolphin (kiod6)"). adb is left out: it doesn't block MTP. Blocking
// (reads /proc); call from a worker. procRoot is for tests.
QStringList usbDeviceHolders(const QString& node, const QString& procRoot = QStringLiteral("/proc"));

}  // namespace tandem
