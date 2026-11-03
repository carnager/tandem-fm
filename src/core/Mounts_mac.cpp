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
