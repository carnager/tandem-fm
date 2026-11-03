#include "ui/viewer/FileInfo.h"

#include "core/DirLister.h"
#include "core/Mounts.h"
#include "core/Util.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>
#include <QProcess>
#include <QStandardPaths>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

namespace {

// The viewer needs seconds, while platform stat structures differ.
struct InfoStat {
    uint32_t mode = 0, uid = 0, gid = 0;
    uint64_t size = 0, ino = 0, nlink = 0, blocks = 0;
    int64_t mtime = 0, atime = 0, ctime = 0, birthtime = 0;
    bool hasBirthTime = false;
};

bool readInfoStat(const QByteArray& path, int flags, InfoStat& out)
{
#ifdef __APPLE__
    struct stat st;
    if (::fstatat(AT_FDCWD, path.constData(), &st, flags) != 0)
        return false;
    out = {uint32_t(st.st_mode), st.st_uid, st.st_gid, uint64_t(st.st_size), st.st_ino, st.st_nlink,
           uint64_t(st.st_blocks), st.st_mtime, st.st_atime, st.st_ctime, st.st_birthtimespec.tv_sec, true};
#else
    struct statx st;
    if (::statx(AT_FDCWD, path.constData(), flags, STATX_BASIC_STATS | STATX_BTIME, &st) != 0)
        return false;
    out = {st.stx_mode, st.stx_uid, st.stx_gid, st.stx_size, st.stx_ino, st.stx_nlink, st.stx_blocks,
           st.stx_mtime.tv_sec, st.stx_atime.tv_sec, st.stx_ctime.tv_sec, st.stx_btime.tv_sec,
           bool(st.stx_mask & STATX_BTIME)};
#endif
    return true;
}

QString when(int64_t seconds)
{
    return seconds == 0 ? QString() : QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss"));
}

QString userName(uint32_t uid)
{
    struct passwd pw, *res = nullptr;
    char buf[4096];
    if (getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res)
        return QString::fromLocal8Bit(res->pw_name);
    return QString::number(uid);
}

QString groupName(uint32_t gid)
{
    struct group gr, *res = nullptr;
    char buf[4096];
    if (getgrgid_r(gid, &gr, buf, sizeof buf, &res) == 0 && res)
        return QString::fromLocal8Bit(res->gr_name);
    return QString::number(gid);
}

QString exactSize(qint64 bytes)
{
    const QString human = formatSize(bytes);
    if (bytes < 1024)
        return human;
    return QStringLiteral("%1  (%2 bytes)").arg(human, QLocale().toString(bytes));
}

QString gitText(char c)
{
    switch (c) {
    case 'M': return QObject::tr("modified");
    case 'A': return QObject::tr("added (staged)");
    case '?': return QObject::tr("untracked");
    case '!': return QObject::tr("ignored");
    case 'U': return QObject::tr("conflict");
    default: return {};
    }
}

// Camera data via exiv2 or exiftool, whichever is installed.
QList<FileInfoRow> exifRows(const QString& path)
{
    struct Field {
        const char* exiv2;
        const char* exiftool;
        const char* label;
    };
    static const Field fields[] = {
        {"Exif.Image.Make", "Make", QT_TRANSLATE_NOOP("FileInfo", "Camera make")},
        {"Exif.Image.Model", "Model", QT_TRANSLATE_NOOP("FileInfo", "Camera")},
        {"Exif.Photo.LensModel", "LensModel", QT_TRANSLATE_NOOP("FileInfo", "Lens")},
        {"Exif.Photo.DateTimeOriginal", "DateTimeOriginal", QT_TRANSLATE_NOOP("FileInfo", "Taken")},
        {"Exif.Photo.ExposureTime", "ExposureTime", QT_TRANSLATE_NOOP("FileInfo", "Exposure")},
        {"Exif.Photo.FNumber", "FNumber", QT_TRANSLATE_NOOP("FileInfo", "Aperture")},
        {"Exif.Photo.ISOSpeedRatings", "ISO", QT_TRANSLATE_NOOP("FileInfo", "ISO")},
        {"Exif.Photo.FocalLength", "FocalLength", QT_TRANSLATE_NOOP("FileInfo", "Focal length")},
    };
    QList<FileInfoRow> out;
    QProcess p;
    if (!QStandardPaths::findExecutable(QStringLiteral("exiv2")).isEmpty()) {
        QStringList args{QStringLiteral("-q"), QStringLiteral("-Pkt")};
        for (const Field& f : fields)
            args << QStringLiteral("-K") << QLatin1String(f.exiv2);
        args << path;
        p.start(QStringLiteral("exiv2"), args);
        if (!p.waitForFinished(4000)) {
            p.kill();
            return out;
        }
        const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'));
        for (const Field& f : fields) {
            for (const QString& line : lines) {
                if (!line.startsWith(QLatin1String(f.exiv2) + QLatin1Char(' ')))
                    continue;
                const QString value = line.mid(int(qstrlen(f.exiv2))).trimmed();
                if (!value.isEmpty())
                    out.append({QCoreApplication::translate("FileInfo", f.label), value});
            }
        }
    } else if (!QStandardPaths::findExecutable(QStringLiteral("exiftool")).isEmpty()) {
        QStringList args{QStringLiteral("-s"), QStringLiteral("-s"), QStringLiteral("-s")};
        for (const Field& f : fields)
            args << QStringLiteral("-") + QLatin1String(f.exiftool);
        args << path;
        p.start(QStringLiteral("exiftool"), args);
        if (!p.waitForFinished(4000)) {
            p.kill();
            return out;
        }
        // -s -s -s prints bare values in the order asked, blank when missing.
        const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'));
        int i = 0;
        for (const Field& f : fields)
            if (i < lines.size() && !lines[i++].trimmed().isEmpty())
                out.append({QCoreApplication::translate("FileInfo", f.label), lines[i - 1].trimmed()});
    }
    return out;
}

}  // namespace

FileInfo gatherFileInfo(const QString& path, char gitStatus)
{
    FileInfo info;
    const QByteArray np = toNative(path);
    InfoStat sx;
    if (!readInfoStat(np, AT_SYMLINK_NOFOLLOW, sx)) {
        info.error = errnoString(errno);
        return info;
    }
    auto& rows = info.rows;
    auto section = [&](const QString& title) { rows.append({QString(), title}); };
    auto row = [&](const QString& k, const QString& v) {
        if (!v.isEmpty())
            rows.append({k, v});
    };

    const bool isLink = S_ISLNK(sx.mode);
    info.isDir = S_ISDIR(sx.mode);
    info.isRegular = S_ISREG(sx.mode);
    InfoStat target = sx;
    QString linkTarget;
    if (isLink) {
        QByteArray buf(4096, '\0');
        const ssize_t n = ::readlink(np.constData(), buf.data(), size_t(buf.size()));
        if (n > 0)
            linkTarget = QFile::decodeName(buf.left(n));
        if (readInfoStat(np, 0, target)) {
            info.isDir = S_ISDIR(target.mode);
            info.isRegular = S_ISREG(target.mode);
        }
    }

    section(QObject::tr("General"));
    row(QObject::tr("Name"), baseName(path));
    row(QObject::tr("Location"), parentPath(path));
    QMimeDatabase db;
    const QMimeType mt = info.isDir ? db.mimeTypeForName(QStringLiteral("inode/directory")) : db.mimeTypeForFile(path);
    row(QObject::tr("Type"), QStringLiteral("%1  (%2)").arg(mt.comment(), mt.name()));
    if (isLink)
        row(QObject::tr("Link to"), linkTarget.isEmpty() ? QObject::tr("(unreadable)") : linkTarget);
    if (!info.isDir)
        row(QObject::tr("Size"), exactSize(qint64(target.size)));
    if (info.isDir) {
        ListOptions opt;
        opt.showHidden = true;
        const ListResult l = listDirectory(path, opt);
        int dirs = 0, files = 0, hidden = 0;
        for (const FileEntry& e : l.entries) {
            (e.isDir ? dirs : files) += 1;
            hidden += e.name.startsWith(QLatin1Char('.'));
        }
        row(QObject::tr("Contains"), l.error ? errnoString(l.error)
                                             : QObject::tr("%1 folders, %2 files (%3 hidden)").arg(dirs).arg(files).arg(hidden));
    }
    row(QObject::tr("Git"), gitText(gitStatus));

    section(QObject::tr("Dates"));
    row(QObject::tr("Modified"), when(target.mtime));
    if (target.hasBirthTime)
        row(QObject::tr("Created"), when(target.birthtime));
    row(QObject::tr("Changed"), when(target.ctime));
    row(QObject::tr("Accessed"), when(target.atime));

    section(QObject::tr("Permissions"));
    row(QObject::tr("Mode"), QStringLiteral("%1   %2").arg(modeString(sx.mode)).arg(sx.mode & 07777, 4, 8, QLatin1Char('0')));
    row(QObject::tr("Owner"), QStringLiteral("%1 : %2").arg(userName(sx.uid), groupName(sx.gid)));
    row(QObject::tr("Readable / writable"),
        QStringLiteral("%1 / %2").arg(::access(np.constData(), R_OK) == 0 ? QObject::tr("yes") : QObject::tr("no"),
                                      ::access(np.constData(), W_OK) == 0 ? QObject::tr("yes") : QObject::tr("no")));

    section(QObject::tr("Storage"));
    QString mountPoint, fsType;
    for (const MountPoint& m : listMounts()) {
        if ((path == m.path || path.startsWith(m.path == QLatin1String("/") ? m.path : m.path + QLatin1Char('/'))) &&
            m.path.size() >= mountPoint.size()) {
            mountPoint = m.path;
            fsType = m.fsType;
        }
    }
    row(QObject::tr("Filesystem"), mountPoint.isEmpty() ? QString() : QStringLiteral("%1  (%2)").arg(mountPoint, fsType));
    row(QObject::tr("Inode / links"), QStringLiteral("%1 / %2").arg(sx.ino).arg(sx.nlink));
    if (info.isRegular && target.blocks * 512 < target.size)
        row(QObject::tr("On disk"), QObject::tr("%1 (sparse or compressed)").arg(formatSize(qint64(target.blocks) * 512)));

    if (info.isRegular && mt.name().startsWith(QLatin1String("image/"))) {
        QImageReader reader(path);
        const QSize sz = reader.size();
        if (sz.isValid()) {
            section(QObject::tr("Image"));
            row(QObject::tr("Dimensions"), QStringLiteral("%1 × %2  (%3 MP)")
                                               .arg(sz.width())
                                               .arg(sz.height())
                                               .arg(double(sz.width()) * sz.height() / 1e6, 0, 'f', 1));
            row(QObject::tr("Format"), QString::fromLatin1(reader.format()).toUpper());
            const QList<FileInfoRow> exif = exifRows(path);
            if (!exif.isEmpty()) {
                section(QObject::tr("Camera"));
                rows.append(exif);
            }
        }
    }
    return info;
}

QString sha256Of(const QString& path, const std::atomic<bool>* cancel)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buf(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (cancel && cancel->load())
            return {};
        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0)
            return {};
        if (n == 0)
            break;
        hash.addData(QByteArrayView(buf.constData(), n));
    }
    return QString::fromLatin1(hash.result().toHex());
}

}  // namespace tandem
