#include "core/Util.h"

#include <QDir>

#include <cstring>
#include <sys/stat.h>

namespace tandem {

QThreadPool* ioPool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(32);
        p->setExpiryTimeout(30000);
        return p;
    }();
    return pool;
}

QString formatSize(int64_t bytes)
{
    if (bytes < 0)
        return {};
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    static const char* units[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = double(bytes);
    int u = -1;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    const int decimals = v < 10 ? 2 : (v < 100 ? 1 : 0);
    return QStringLiteral("%1 %2").arg(v, 0, 'f', decimals).arg(QLatin1String(units[u]));
}

QString formatSpeed(double bytesPerSec)
{
    if (bytesPerSec <= 0)
        return QStringLiteral("–");
    return formatSize(int64_t(bytesPerSec)) + QStringLiteral("/s");
}

QString formatDuration(int64_t s)
{
    if (s < 0)
        return QStringLiteral("–");
    if (s < 60)
        return QStringLiteral("%1s").arg(s);
    if (s < 3600)
        return QStringLiteral("%1m %2s").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1h %2m").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0'));
}

QString errnoString(int err)
{
    char buf[256];
    const char* s = strerror_r(err, buf, sizeof buf);  // GNU variant
    return QString::fromLocal8Bit(s);
}

QString modeString(uint32_t m)
{
    QString s(10, QLatin1Char('-'));
    if (S_ISDIR(m))
        s[0] = QLatin1Char('d');
    else if (S_ISLNK(m))
        s[0] = QLatin1Char('l');
    else if (S_ISCHR(m))
        s[0] = QLatin1Char('c');
    else if (S_ISBLK(m))
        s[0] = QLatin1Char('b');
    else if (S_ISFIFO(m))
        s[0] = QLatin1Char('p');
    else if (S_ISSOCK(m))
        s[0] = QLatin1Char('s');
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        if (m & (1u << (8 - i)))
            s[i + 1] = QLatin1Char(rwx[i]);
    if (m & S_ISUID)
        s[3] = (m & S_IXUSR) ? QLatin1Char('s') : QLatin1Char('S');
    if (m & S_ISGID)
        s[6] = (m & S_IXGRP) ? QLatin1Char('s') : QLatin1Char('S');
    if (m & S_ISVTX)
        s[9] = (m & S_IXOTH) ? QLatin1Char('t') : QLatin1Char('T');
    return s;
}

QString joinPath(const QString& dir, const QString& name)
{
    if (dir.endsWith(QLatin1Char('/')))
        return dir + name;
    return dir + QLatin1Char('/') + name;
}

QByteArray joinPath(const QByteArray& dir, const QByteArray& name)
{
    if (dir.endsWith('/'))
        return dir + name;
    return dir + '/' + name;
}

QString parentPath(const QString& path)
{
    if (path == QLatin1String("/"))
        return path;
    const int i = path.lastIndexOf(QLatin1Char('/'));
    if (i <= 0)
        return QStringLiteral("/");
    return path.left(i);
}

QString baseName(const QString& path)
{
    if (path == QLatin1String("/"))
        return path;
    QString p = path;
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p.mid(p.lastIndexOf(QLatin1Char('/')) + 1);
}

QString expandUserPath(const QString& in)
{
    QString p = in.trimmed();
    if (p == QLatin1String("~"))
        p = QDir::homePath();
    else if (p.startsWith(QLatin1String("~/")))
        p = QDir::homePath() + p.mid(1);
    if (p.startsWith(QLatin1String("file://")))
        p = p.mid(7);
    if (p.isEmpty())
        return p;
    return QDir::cleanPath(p);
}

}  // namespace tandem
