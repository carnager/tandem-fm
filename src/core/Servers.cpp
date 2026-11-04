#include "core/Servers.h"

#include "core/Backend.h"
#include "core/TomlSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

namespace tandem {

namespace {

int defaultPort(const QString& type)
{
    if (type == QLatin1String("sftp"))
        return 22;
    if (type == QLatin1String("smb"))
        return 445;
    return 0;
}

int effectivePort(const QString& type, int port)
{
    return port > 0 ? port : defaultPort(type);
}

QString readText(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

const QString kHeader = QStringLiteral(
    "# Servers Tandem knows: \"Servers…\" in the command palette edits this file,\n"
    "# and saved changes apply right away. Passwords are kept in the keyring.\n"
    "#\n"
    "# [[server]]\n"
    "# name = \"NAS\"\n"
    "# type = \"smb\"            # or \"sftp\"\n"
    "# host = \"nas.lan\"\n"
    "# port = 445              # optional\n"
    "# user = \"bob\"            # optional\n"
    "# domain = \"WORKGROUP\"    # smb only\n"
    "# key-file = \"~/.ssh/id\"  # sftp only\n"
    "# folder = \"/media\"       # where to start; empty: the home folder\n");

}  // namespace

QString SavedServer::authority() const
{
    QUrl u;
    u.setScheme(type);
    u.setHost(host);
    const QString who = domain.isEmpty() || user.isEmpty() ? user : domain + QLatin1Char(';') + user;
    if (!who.isEmpty())
        u.setUserName(who);
    if (port > 0 && port != defaultPort(type))
        u.setPort(port);
    return u.toString();
}

QString SavedServer::uri() const
{
    QString path = folder.trimmed();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (!path.isEmpty() && !path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    return authority() + path;
}

SavedServer serverFromUri(const QString& uri)
{
    const QUrl u(uri);
    SavedServer s;
    s.type = u.scheme().toLower();
    s.host = u.host();
    s.name = u.host();
    s.port = u.port(0) == defaultPort(s.type) ? 0 : u.port(0);
    s.user = u.userName();
    for (QChar sep : {QLatin1Char(';'), QLatin1Char('\\')}) {
        if (const int i = int(s.user.indexOf(sep)); i > 0) {
            s.domain = s.user.left(i);
            s.user = s.user.mid(i + 1);
        }
    }
    if (u.path() != QLatin1String("/"))
        s.folder = u.path();
    return s;
}

QString serversToml(const QList<SavedServer>& servers)
{
    QString text = kHeader;
    for (const SavedServer& s : servers) {
        text += QStringLiteral("\n[[server]]\n");
        auto line = [&text](const char* key, const QVariant& v) {
            text += QLatin1String(key) + QStringLiteral(" = ") + tomlValueText(v) + QLatin1Char('\n');
        };
        line("name", s.name);
        line("type", s.type);
        line("host", s.host);
        if (s.port > 0)
            line("port", s.port);
        if (!s.user.isEmpty())
            line("user", s.user);
        if (!s.domain.isEmpty())
            line("domain", s.domain);
        if (!s.keyFile.isEmpty())
            line("key-file", s.keyFile);
        if (!s.folder.isEmpty())
            line("folder", s.folder);
    }
    return text;
}

bool parseServersToml(const QString& text, QList<SavedServer>* out, QString* error)
{
    const QByteArray utf8 = text.toUtf8();
    toml::parse_result r = toml::parse(std::string_view(utf8.constData(), size_t(utf8.size())));
    if (!r) {
        if (error) {
            const toml::parse_error& e = r.error();
            *error = QObject::tr("line %1: %2")
                         .arg(e.source().begin.line)
                         .arg(QString::fromUtf8(e.description().data(), qsizetype(e.description().size())));
        }
        return false;
    }
    QList<SavedServer> list;
    if (const toml::array* arr = r.table()["server"].as_array()) {
        for (const toml::node& n : *arr) {
            const toml::table* t = n.as_table();
            if (!t)
                continue;
            auto str = [t](const char* key) {
                const auto v = (*t)[key].value<std::string>();
                return v ? QString::fromStdString(*v).trimmed() : QString();
            };
            SavedServer s;
            s.host = str("host");
            if (s.host.isEmpty())
                continue;  // nothing to connect to
            const QString type = str("type").toLower();
            if (!type.isEmpty())
                s.type = type;
            s.name = str("name");
            if (s.name.isEmpty())
                s.name = s.host;
            s.port = int((*t)["port"].value<int64_t>().value_or(0));
            s.user = str("user");
            s.domain = str("domain");
            s.keyFile = str("key-file");
            s.folder = str("folder");
            list.append(s);
        }
    }
    *out = list;
    return true;
}

// ------------------------------------------------------------ ~/.ssh/config

namespace {

QString expandSshPath(QString p)
{
    if (p == QLatin1String("~") || p.startsWith(QLatin1String("~/")))
        p = QDir::homePath() + p.mid(1);
    p.replace(QLatin1String("%d"), QDir::homePath());
    return p;
}

// "a b", "a=b", quoted values.
bool splitLine(const QString& raw, QString* key, QString* value)
{
    const QString line = raw.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
        return false;
    static const QRegularExpression rx(QStringLiteral("^(\\S+?)\\s*(?:=|\\s)\\s*(.*)$"));
    const auto m = rx.match(line);
    if (!m.hasMatch())
        return false;
    *key = m.captured(1).toLower();
    *value = m.captured(2).trimmed();
    return true;
}

QStringList words(const QString& value)
{
    QStringList out;
    static const QRegularExpression rx(QStringLiteral("\"([^\"]*)\"|(\\S+)"));
    for (auto it = rx.globalMatch(value); it.hasNext();) {
        const auto m = it.next();
        out << (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1));
    }
    return out;
}

void readSshFile(const QString& path, int depth, QList<SavedServer>* hosts, QHash<QString, int>* index)
{
    if (depth > 8)
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QList<int> current;  // the hosts the lines below belong to
    bool skipping = false;  // in a Match block or one of only patterns
    const QString sshDir = QDir::homePath() + QStringLiteral("/.ssh");
    for (const QString& raw : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
        QString key, value;
        if (!splitLine(raw, &key, &value))
            continue;
        if (key == QLatin1String("include")) {
            for (QString pattern : words(value)) {
                pattern = expandSshPath(pattern);
                if (!pattern.startsWith(QLatin1Char('/')))
                    pattern = sshDir + QLatin1Char('/') + pattern;
                const QFileInfo fi(pattern);
                QDir dir(fi.path());
                QStringList files = dir.entryList({fi.fileName()}, QDir::Files, QDir::Name);
                for (const QString& name : files)
                    readSshFile(dir.filePath(name), depth + 1, hosts, index);
            }
            continue;
        }
        if (key == QLatin1String("match")) {
            current.clear();
            skipping = true;
            continue;
        }
        if (key == QLatin1String("host")) {
            current.clear();
            for (const QString& pattern : words(value)) {
                static const QRegularExpression wild(QStringLiteral("[*?!]"));
                if (pattern.contains(wild))
                    continue;
                if (!index->contains(pattern)) {
                    SavedServer s;
                    s.name = pattern;
                    s.type = QStringLiteral("sftp");
                    index->insert(pattern, int(hosts->size()));
                    hosts->append(s);
                }
                current << index->value(pattern);
            }
            skipping = current.isEmpty();
            continue;
        }
        if (skipping || current.isEmpty())
            continue;
        const QStringList w = words(value);
        if (w.isEmpty())
            continue;
        // Like ssh: the first value given for a host wins.
        for (int i : std::as_const(current)) {
            SavedServer& s = (*hosts)[i];
            if (key == QLatin1String("hostname") && s.host.isEmpty())
                s.host = w.first();
            else if (key == QLatin1String("user") && s.user.isEmpty())
                s.user = w.first();
            else if (key == QLatin1String("port") && s.port == 0)
                s.port = w.first().toInt();
            else if (key == QLatin1String("identityfile") && s.keyFile.isEmpty())
                s.keyFile = w.first();
        }
    }
}

}  // namespace

QList<SavedServer> readSshConfig(const QString& pathIn)
{
    const QString path = pathIn.isEmpty() ? QDir::homePath() + QStringLiteral("/.ssh/config") : pathIn;
    QList<SavedServer> hosts;
    QHash<QString, int> index;
    readSshFile(path, 0, &hosts, &index);
    for (SavedServer& s : hosts) {
        if (s.host.isEmpty())
            s.host = s.name;
        s.host.replace(QLatin1String("%h"), s.name);
        if (s.port == 22)
            s.port = 0;
    }
    return hosts;
}

// ------------------------------------------------------------ ServerList

ServerList& ServerList::instance()
{
    static ServerList list;
    return list;
}

ServerList::ServerList()
{
    m_reload = new QTimer(this);
    m_reload->setSingleShot(true);
    m_reload->setInterval(150);  // editors write in steps
    connect(m_reload, &QTimer::timeout, this, [this] {
        if (readText(path()) == m_written)
            return;
        load();
        emit changed();
    });
}

QString ServerList::path() const
{
    return QFileInfo(QSettings().fileName()).path() + QStringLiteral("/servers.toml");
}

bool ServerList::load()
{
    watch();
    const QString text = readText(path());
    m_written = text;
    QList<SavedServer> list;
    QString error;
    if (!parseServersToml(text, &list, &error)) {
        QMutexLocker lock(&m_mutex);
        m_error = QStringLiteral("servers.toml, %1").arg(error);
        return false;
    }
    QMutexLocker lock(&m_mutex);
    m_error.clear();
    m_servers = list;
    return true;
}

QString ServerList::error() const
{
    QMutexLocker lock(&m_mutex);
    return m_error;
}

QList<SavedServer> ServerList::servers() const
{
    QMutexLocker lock(&m_mutex);
    return m_servers;
}

bool ServerList::save(const QList<SavedServer>& servers)
{
    const QString text = serversToml(servers);
    QDir().mkpath(QFileInfo(path()).path());
    QSaveFile f(path());
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(text.toUtf8());
    if (!f.commit())
        return false;
    m_written = text;
    {
        QMutexLocker lock(&m_mutex);
        m_servers = servers;
        m_error.clear();
    }
    watch();
    emit changed();
    return true;
}

std::optional<SavedServer> ServerList::find(const QString& location) const
{
    if (!isRemoteLocation(location))
        return std::nullopt;
    const QUrl u(locationRoot(location));
    const QString type = u.scheme().toLower();
    const int port = effectivePort(type, u.port(0));
    QMutexLocker lock(&m_mutex);
    for (const SavedServer& s : m_servers) {
        if (s.type != type || s.host.compare(u.host(), Qt::CaseInsensitive) != 0 ||
            effectivePort(s.type, s.port) != port)
            continue;
        if (QUrl(s.authority() + QLatin1Char('/')).userName() == u.userName())
            return s;
    }
    return std::nullopt;
}

void ServerList::watch()
{
    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::fileChanged, m_reload, qOverload<>(&QTimer::start));
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
            // Editors save by replacing the file: watch the new one.
            if (QFile::exists(path()) && !m_watcher->files().contains(path()))
                m_watcher->addPath(path());
            m_reload->start();
        });
    }
    const QString dir = QFileInfo(path()).path();
    if (!m_watcher->directories().isEmpty() && m_watcher->directories() != QStringList{dir})
        m_watcher->removePaths(m_watcher->directories() + m_watcher->files());
    if (QDir(dir).exists() && !m_watcher->directories().contains(dir))
        m_watcher->addPath(dir);
    if (QFile::exists(path()) && !m_watcher->files().contains(path()))
        m_watcher->addPath(path());
}

}  // namespace tandem
