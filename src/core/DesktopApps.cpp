#include "core/DesktopApps.h"

#include "core/Util.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QLocale>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QStandardPaths>

namespace tandem {

DesktopApps& DesktopApps::instance()
{
    static DesktopApps apps;
    return apps;
}

void DesktopApps::loadAsync()
{
    ioPool()->start([this] { load(); });
}

namespace {

QStringList dataDirs()
{
    QStringList dirs;
    QString home = qEnvironmentVariable("XDG_DATA_HOME");
    if (home.isEmpty())
        home = QDir::homePath() + QStringLiteral("/.local/share");
    dirs << home;
    QString sys = qEnvironmentVariable("XDG_DATA_DIRS");
    if (sys.isEmpty())
        sys = QStringLiteral("/usr/local/share:/usr/share");
    dirs << sys.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return dirs;
}

QStringList configDirs()
{
    QStringList dirs;
    QString home = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (home.isEmpty())
        home = QDir::homePath() + QStringLiteral("/.config");
    dirs << home;
    QString sys = qEnvironmentVariable("XDG_CONFIG_DIRS");
    if (sys.isEmpty())
        sys = QStringLiteral("/etc/xdg");
    dirs << sys.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return dirs;
}

// Minimal [Desktop Entry] parser. Returns false for entries we can't launch.
bool parseDesktopFile(const QString& path, DesktopApp& app)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString lang = QLocale().name();           // de_DE
    const QString langShort = lang.section(QLatin1Char('_'), 0, 0);  // de
    QString localName, shortName;
    bool inEntry = false;
    bool hidden = false;
    QString type;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        if (!inEntry)
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (key == QLatin1String("Name"))
            app.name = value;
        else if (key == QStringLiteral("Name[%1]").arg(lang))
            localName = value;
        else if (key == QStringLiteral("Name[%1]").arg(langShort))
            shortName = value;
        else if (key == QLatin1String("Exec"))
            app.exec = value;
        else if (key == QLatin1String("Icon"))
            app.icon = value;
        else if (key == QLatin1String("MimeType"))
            app.mimeTypes = value.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        else if (key == QLatin1String("Terminal"))
            app.terminal = value == QLatin1String("true");
        else if (key == QLatin1String("Hidden"))
            hidden = value == QLatin1String("true");
        else if (key == QLatin1String("Type"))
            type = value;
    }
    if (!localName.isEmpty())
        app.name = localName;
    else if (!shortName.isEmpty())
        app.name = shortName;
    return !hidden && type == QLatin1String("Application") && !app.exec.isEmpty() && !app.name.isEmpty();
}

void parseMimeApps(const QString& path, QHash<QString, QStringList>& defaults, QHash<QString, QStringList>& added,
                   QHash<QString, QStringList>& removed)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    enum { None, Default, Added, Removed } section = None;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            if (line == QLatin1String("[Default Applications]"))
                section = Default;
            else if (line == QLatin1String("[Added Associations]"))
                section = Added;
            else if (line == QLatin1String("[Removed Associations]"))
                section = Removed;
            else
                section = None;
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (section == None || eq <= 0)
            continue;
        const QString mime = line.left(eq).trimmed();
        const QStringList ids = line.mid(eq + 1).split(QLatin1Char(';'), Qt::SkipEmptyParts);
        auto& target = section == Default ? defaults : (section == Added ? added : removed);
        // Earlier files take precedence: only append ids not yet present.
        QStringList& list = target[mime];
        for (const QString& id : ids)
            if (!list.contains(id))
                list.append(id);
    }
}

}  // namespace

void DesktopApps::load()
{
    QHash<QString, DesktopApp> apps;
    QHash<QString, QStringList> byMime, defaults, added, removed;

    for (const QString& base : dataDirs()) {
        const QString dir = base + QStringLiteral("/applications");
        QDirIterator it(dir, {QStringLiteral("*.desktop")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QString id = path.mid(dir.size() + 1);
            id.replace(QLatin1Char('/'), QLatin1Char('-'));
            if (apps.contains(id))
                continue;  // a higher-priority dir already defined it
            DesktopApp app;
            app.id = id;
            if (!parseDesktopFile(path, app)) {
                apps.insert(id, DesktopApp{});  // shadow lower-priority copies
                continue;
            }
            for (const QString& m : std::as_const(app.mimeTypes))
                byMime[m].append(id);
            apps.insert(id, app);
        }
    }
    for (const QString& dir : configDirs())
        parseMimeApps(dir + QStringLiteral("/mimeapps.list"), defaults, added, removed);
    for (const QString& base : dataDirs())
        parseMimeApps(base + QStringLiteral("/applications/mimeapps.list"), defaults, added, removed);

    QString terminal = qEnvironmentVariable("TERMINAL");
    if (terminal.isEmpty() || QStandardPaths::findExecutable(terminal.section(QLatin1Char(' '), 0, 0)).isEmpty()) {
        terminal.clear();
        for (const char* t : {"foot", "kitty", "alacritty", "wezterm", "ghostty", "konsole", "gnome-terminal",
                              "xfce4-terminal", "xterm"}) {
            if (!QStandardPaths::findExecutable(QLatin1String(t)).isEmpty()) {
                terminal = QLatin1String(t);
                break;
            }
        }
    }

    QMutexLocker lock(&m_mutex);
    m_apps = std::move(apps);
    m_byMime = std::move(byMime);
    m_defaults = std::move(defaults);
    m_added = std::move(added);
    m_removed = std::move(removed);
    m_terminal = terminal;
    m_loaded.store(true);
}

QList<DesktopApp> DesktopApps::appsFor(const QStringList& mimeChain) const
{
    QMutexLocker lock(&m_mutex);
    QList<DesktopApp> out;
    QSet<QString> seen;
    auto take = [&](const QStringList& ids) {
        for (const QString& id : ids) {
            if (seen.contains(id))
                continue;
            const auto it = m_apps.constFind(id);
            if (it == m_apps.cend() || it->exec.isEmpty())
                continue;
            seen.insert(id);
            out.append(*it);
        }
    };
    for (const QString& m : mimeChain)
        take(m_defaults.value(m));
    for (const QString& m : mimeChain) {
        take(m_added.value(m));
        QStringList ids = m_byMime.value(m);
        for (const QString& r : m_removed.value(m))
            ids.removeAll(r);
        take(ids);
    }
    return out;
}

DesktopApp DesktopApps::defaultFor(const QStringList& mimeChain) const
{
    const QList<DesktopApp> apps = appsFor(mimeChain);
    return apps.isEmpty() ? DesktopApp{} : apps.first();
}

bool DesktopApps::setDefault(const QString& mime, const QString& appId, QString* error)
{
    QString path = configDirs().first() + QStringLiteral("/mimeapps.list");
    const QFileInfo fi(path);
    if (fi.isSymLink() && !fi.canonicalFilePath().isEmpty())
        path = fi.canonicalFilePath();  // keep dotfile symlinks intact

    QStringList lines;
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
            lines.removeLast();
    }

    const QString entry = mime + QLatin1Char('=') + appId + QLatin1Char(';');
    int section = -1;
    for (int i = 0; i < lines.size(); ++i)
        if (lines[i].trimmed() == QLatin1String("[Default Applications]"))
            section = i;
    if (section < 0) {
        if (!lines.isEmpty())
            lines << QString();
        lines << QStringLiteral("[Default Applications]") << entry;
    } else {
        int insertAt = section + 1;
        bool replaced = false;
        for (int i = section + 1; i < lines.size(); ++i) {
            const QString t = lines[i].trimmed();
            if (t.startsWith(QLatin1Char('[')))
                break;
            if (!t.isEmpty())
                insertAt = i + 1;
            if (t.section(QLatin1Char('='), 0, 0).trimmed() == mime) {
                lines[i] = entry;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            lines.insert(insertAt, entry);
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = out.errorString();
        return false;
    }
    out.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
    if (!out.commit()) {
        if (error)
            *error = out.errorString();
        return false;
    }

    QMutexLocker lock(&m_mutex);
    QStringList& ids = m_defaults[mime];
    ids.removeAll(appId);
    ids.prepend(appId);
    return true;
}

QString DesktopApps::defaultTerminal() const
{
    QMutexLocker lock(&m_mutex);
    return m_terminal;
}

QList<QStringList> DesktopApps::commandLines(const QString& exec, const QStringList& paths)
{
    const QStringList tokens = QProcess::splitCommand(exec);
    bool multi = false, single = false;
    for (const QString& t : tokens) {
        if (t == QLatin1String("%F") || t == QLatin1String("%U"))
            multi = true;
        else if (t == QLatin1String("%f") || t == QLatin1String("%u"))
            single = true;
    }

    auto build = [&](const QStringList& files) {
        QStringList argv;
        bool used = false;
        for (const QString& t : tokens) {
            if (t == QLatin1String("%F") || t == QLatin1String("%U") || t == QLatin1String("%f") ||
                t == QLatin1String("%u")) {
                const bool url = t.endsWith(QLatin1Char('u')) || t.endsWith(QLatin1Char('U'));
                for (const QString& f : files)
                    argv << (url ? QUrl::fromLocalFile(f).toString(QUrl::FullyEncoded) : f);
                used = true;
            } else if (t == QLatin1String("%i") || t == QLatin1String("%c") || t == QLatin1String("%k")) {
                continue;  // icon, caption, location: not needed
            } else {
                QString a = t;
                a.replace(QLatin1String("%%"), QLatin1String("%"));
                argv << a;
            }
        }
        if (!used)
            argv << files;  // no field code: pass files anyway, like other file managers
        return argv;
    };

    QList<QStringList> out;
    if (single && !multi && paths.size() > 1) {
        for (const QString& p : paths)
            out.append(build({p}));
    } else {
        out.append(build(paths));
    }
    return out;
}

bool DesktopApps::launch(const DesktopApp& app, const QStringList& paths, const QString& workDir,
                         const QString& terminal)
{
    bool ok = true;
    for (QStringList argv : commandLines(app.exec, paths)) {
        if (argv.isEmpty())
            return false;
        if (app.terminal && !terminal.isEmpty())
            argv = QStringList{terminal, QStringLiteral("-e")} + argv;
        const QString program = argv.takeFirst();
        ok = QProcess::startDetached(program, argv, workDir) && ok;
    }
    return ok;
}

}  // namespace tandem
