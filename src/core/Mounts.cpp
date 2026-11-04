#include "core/Mounts.h"

#include <sys/vfs.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
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

QString usbNodeFromMtpError(const QString& error)
{
    static const QRegularExpression re(QStringLiteral("MTP.*\\b(\\d{3}),(\\d{3})\\b"));
    const auto m = re.match(error);
    return m.hasMatch() ? QStringLiteral("/dev/bus/usb/%1/%2").arg(m.captured(1), m.captured(2)) : QString();
}

QStringList usbDeviceHolders(const QString& node, const QString& procRoot)
{
    QStringList out;
    const QByteArray target = QFile::encodeName(node);
    const QString self = QString::number(QCoreApplication::applicationPid());
    const QStringList pids = QDir(procRoot).entryList({QStringLiteral("[0-9]*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& pid : pids) {
        if (pid == self)
            continue;
        const QString fdDir = procRoot + QLatin1Char('/') + pid + QStringLiteral("/fd");
        bool holds = false;
        for (const QString& fd : QDir(fdDir).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
            char buf[256];
            const ssize_t n = ::readlink(QFile::encodeName(fdDir + QLatin1Char('/') + fd).constData(), buf, sizeof buf);
            if (n > 0 && QByteArray(buf, int(n)) == target) {
                holds = true;
                break;
            }
        }
        if (!holds)
            continue;
        QFile comm(procRoot + QLatin1Char('/') + pid + QStringLiteral("/comm"));
        const QString name = comm.open(QIODevice::ReadOnly) ? QString::fromUtf8(comm.readAll()).trimmed() : pid;
        if (name == QLatin1String("adb") || name.startsWith(QLatin1String("gvfs")))
            continue;
        const QString shown = name.startsWith(QLatin1String("kio"))
                                  ? QStringLiteral("Dolphin (%1)").arg(name)
                                  : name;
        if (!out.contains(shown))
            out << shown;
    }
    return out;
}

}  // namespace tandem
