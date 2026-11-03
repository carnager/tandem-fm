#include "core/Remote.h"

#include "core/Backend.h"

#include <QHash>
#include <QRegularExpression>
#include <QUrl>

#include <unistd.h>

namespace tandem {

QString gvfsDirectory()
{
    static const QString dir = QStringLiteral("/run/user/%1/gvfs").arg(::getuid());
    return dir;
}

RemoteRoot describeGvfsName(const QString& name)
{
    RemoteRoot r;
    const int colon = int(name.indexOf(QLatin1Char(':')));
    QString scheme = colon > 0 ? name.left(colon) : name;
    QHash<QString, QString> kv;
    for (const QString& part : name.mid(colon + 1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int eq = int(part.indexOf(QLatin1Char('=')));
        if (eq > 0)
            kv.insert(part.left(eq), QUrl::fromPercentEncoding(part.mid(eq + 1).toUtf8()));
    }
    QString host = kv.value(QStringLiteral("host"), kv.value(QStringLiteral("server")));
    QString path;
    if (scheme == QLatin1String("smb-share")) {
        scheme = QStringLiteral("smb");
        path = QLatin1Char('/') + kv.value(QStringLiteral("share"));
    } else if (scheme == QLatin1String("smb-server")) {
        scheme = QStringLiteral("smb");
    } else if (scheme == QLatin1String("dav") && kv.value(QStringLiteral("ssl")) == QLatin1String("true")) {
        scheme = QStringLiteral("davs");
    }
    const QString prefix = kv.value(QStringLiteral("prefix"));
    if (!prefix.isEmpty() && prefix != QLatin1String("/"))
        path = prefix + path;
    r.scheme = scheme;
    if (host.isEmpty()) {
        r.label = scheme;  // localtest, … (mtp and gphoto2 have a host)
        return r;
    }
    QString who = kv.value(QStringLiteral("user"));
    if (!who.isEmpty())
        who += QLatin1Char('@');
    QString port = kv.value(QStringLiteral("port"));
    r.label = QStringLiteral("%1://%2%3%4%5").arg(scheme, who, host, port.isEmpty() ? QString() : QLatin1Char(':') + port, path);
    return r;
}

std::optional<RemoteRoot> remoteRootOf(const QString& path)
{
    const QString base = gvfsDirectory() + QLatin1Char('/');
    if (!path.startsWith(base))
        return std::nullopt;
    const QString name = path.mid(base.size()).section(QLatin1Char('/'), 0, 0);
    if (name.isEmpty())
        return std::nullopt;
    RemoteRoot r = describeGvfsName(name);
    r.root = base + name;
    return r;
}

bool isRemotePath(const QString& path)
{
    return path.startsWith(gvfsDirectory() + QLatin1Char('/'));
}

QString normalizeServerUri(const QString& input)
{
    const QString text = input.trimmed();
    if (text.isEmpty() || text.startsWith(QLatin1Char('/')) || text.startsWith(QLatin1Char('~')))
        return {};
    static const QRegularExpression scheme(QStringLiteral("^([a-zA-Z][a-zA-Z0-9+.-]*)://"));
    if (const auto m = scheme.match(text); m.hasMatch()) {
        const QString s = m.captured(1).toLower();
        if (s == QLatin1String("file"))
            return {};
        if (s == QLatin1String("ssh") || s == QLatin1String("scp"))
            return QStringLiteral("sftp") + text.mid(s.size());
        if (s == QLatin1String("webdav"))
            return QStringLiteral("dav") + text.mid(s.size());
        if (s == QLatin1String("webdavs") || s == QLatin1String("https"))
            return QStringLiteral("davs") + text.mid(s.size());
        if (s == QLatin1String("http"))
            return QStringLiteral("dav") + text.mid(s.size());
        return text;
    }
    // scp style: user@host:path  → sftp://user@host/path (path relative to home)
    static const QRegularExpression scp(QStringLiteral("^([^\\s/@:]+@)?([A-Za-z0-9._-]+|\\[[0-9a-fA-F:]+\\])(:(.*))?$"));
    if (const auto m = scp.match(text); m.hasMatch() && (!m.captured(1).isEmpty() || !m.captured(3).isEmpty())) {
        QString user = m.captured(1);
        user.chop(user.isEmpty() ? 0 : 1);
        if (user.isEmpty())
            user = QString::fromLocal8Bit(qgetenv("USER"));
        QString p = m.captured(4);
        if (!m.captured(3).isEmpty() && !p.startsWith(QLatin1Char('/'))) {
            // scp style: relative paths (and ~) start at the home folder
            if (p.startsWith(QLatin1Char('~')))
                p = p.mid(1);
            while (p.startsWith(QLatin1Char('/')))
                p = p.mid(1);
            p = QStringLiteral("/home/") + user + (p.isEmpty() ? QString() : QLatin1Char('/') + p);
        }
        return QStringLiteral("sftp://%1%2%3").arg(m.captured(1), m.captured(2), p);
    }
    return {};
}

QString nativeLocationFor(const QString& path)
{
    const auto r = remoteRootOf(path);
    if (!r || (r->scheme != QLatin1String("sftp") && r->scheme != QLatin1String("smb")) || !hasBackend(r->scheme) ||
        !r->label.contains(QLatin1String("://")))
        return {};
    return normalizeLocation(r->label + path.mid(r->root.size()));
}

}  // namespace tandem
