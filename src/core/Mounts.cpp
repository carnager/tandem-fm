#ifdef __APPLE__
#include "core/Mounts.h"
#include <QFile>
#include <sys/mount.h>
#include <vector>

namespace tandem {
QList<MountPoint> listMounts()
{
    QList<MountPoint> out;
    const int count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
    if (count <= 0) return out;
    std::vector<struct statfs> mounts(size_t(count) + 16);
    const int filled = ::getfsstat(mounts.data(), int(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
    for (int i = 0; i < qMin(filled, int(mounts.size())); ++i) {
        MountPoint m;
        m.path = QFile::decodeName(mounts[i].f_mntonname);
        if (m.path != QLatin1String("/") && !m.path.startsWith(QLatin1String("/Volumes/")))
            continue;
        m.device = QFile::decodeName(mounts[i].f_mntfromname);
        m.fsType = QString::fromLocal8Bit(mounts[i].f_fstypename);
        m.isNetwork = !(mounts[i].f_flags & MNT_LOCAL);
        m.isRemovable = !m.isNetwork && m.path.startsWith(QLatin1String("/Volumes/"));
        out.append(m);
    }
    return out;
}
bool isNetworkFilesystem(const QString& path)
{
    struct statfs st;
    return ::statfs(QFile::encodeName(path).constData(), &st) == 0 && !(st.f_flags & MNT_LOCAL);
}
} // namespace tandem
#else
#include "core/Mounts.h"

#include <sys/vfs.h>

#include <QFile>
#include <QSet>

namespace tandem {

static QString unescapeMount(const QByteArray& s)
{
    // /proc/mounts escapes space, tab, newline, backslash as \ooo octal.
    QByteArray out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            bool ok = false;
            const int v = s.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out.append(char(v));
                i += 3;
                continue;
            }
        }
        out.append(s[i]);
    }
    return QFile::decodeName(out);
}

QList<MountPoint> listMounts()
{
    static const QSet<QString> pseudo = {
        QStringLiteral("proc"),       QStringLiteral("sysfs"),      QStringLiteral("devtmpfs"),
        QStringLiteral("devpts"),     QStringLiteral("cgroup"),     QStringLiteral("cgroup2"),
        QStringLiteral("securityfs"), QStringLiteral("pstore"),     QStringLiteral("efivarfs"),
        QStringLiteral("bpf"),        QStringLiteral("tracefs"),    QStringLiteral("debugfs"),
        QStringLiteral("mqueue"),     QStringLiteral("hugetlbfs"),  QStringLiteral("configfs"),
        QStringLiteral("fusectl"),    QStringLiteral("autofs"),     QStringLiteral("binfmt_misc"),
        QStringLiteral("rpc_pipefs"), QStringLiteral("nsfs"),       QStringLiteral("ramfs"),
        QStringLiteral("fuse.portal"), QStringLiteral("fuse.gvfsd-fuse"), QStringLiteral("overlay"),
        QStringLiteral("squashfs"),
    };
    static const QSet<QString> network = {
        QStringLiteral("nfs"),        QStringLiteral("nfs4"),       QStringLiteral("cifs"),
        QStringLiteral("smb3"),       QStringLiteral("smbfs"),      QStringLiteral("fuse.sshfs"),
        QStringLiteral("fuse.rclone"), QStringLiteral("9p"),        QStringLiteral("ceph"),
        QStringLiteral("glusterfs"),  QStringLiteral("davfs"),      QStringLiteral("fuse.s3fs"),
    };

    QList<MountPoint> out;
    QFile f(QStringLiteral("/proc/self/mounts"));
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QSet<QString> seen;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (const QByteArray& line : lines) {
        const QList<QByteArray> parts = line.split(' ');
        if (parts.size() < 3)
            continue;
        MountPoint m;
        m.device = unescapeMount(parts[0]);
        m.path = unescapeMount(parts[1]);
        m.fsType = QString::fromLatin1(parts[2]);
        if (pseudo.contains(m.fsType))
            continue;
        if (m.fsType == QLatin1String("tmpfs") && m.path != QLatin1String("/tmp"))
            continue;
        const bool systemDir = m.path.startsWith(QLatin1String("/proc")) || m.path.startsWith(QLatin1String("/sys")) ||
                               m.path.startsWith(QLatin1String("/dev")) || m.path.startsWith(QLatin1String("/snap")) ||
                               m.path.startsWith(QLatin1String("/var/lib/docker")) || m.path == QLatin1String("/boot") ||
                               m.path == QLatin1String("/efi") ||
                               (m.path.startsWith(QLatin1String("/run")) && !m.path.startsWith(QLatin1String("/run/media")));
        if (systemDir)
            continue;
        if (seen.contains(m.path))
            continue;
        seen.insert(m.path);
        m.isNetwork = network.contains(m.fsType);
        m.isRemovable = m.path.startsWith(QLatin1String("/run/media")) || m.path.startsWith(QLatin1String("/media"));
        out.append(m);
    }
    return out;
}

bool isNetworkFilesystem(const QString& path)
{
    struct statfs sf;
    if (::statfs(QFile::encodeName(path).constData(), &sf) != 0)
        return false;
    switch (uint32_t(sf.f_type)) {
    case 0x6969:      // NFS
    case 0x517B:      // SMB
    case 0xFF534D42:  // CIFS
    case 0xFE534D42:  // SMB2
    case 0x65735546:  // FUSE: sshfs, rclone, GVfs …
    case 0x00C36400:  // Ceph
    case 0x01021997:  // 9p
    case 0x5346414F:  // AFS
        return true;
    default:
        return false;
    }
}

}  // namespace tandem

#endif
