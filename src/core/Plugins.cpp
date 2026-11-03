#include "core/Plugins.h"

#include "core/ConfigDir.h"
#include "core/Util.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace tandem {

namespace {

QStringList stringList(const QJsonValue& v)
{
    if (v.isString())  // a command line as one string
        return QProcess::splitCommand(v.toString());
    QStringList out;
    for (const QJsonValue& x : v.toArray())
        out << x.toString();
    return out;
}

QString slug(const QString& s)
{
    QString id = s.toLower();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    return id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
}

QString trustKey(const Plugin& p)
{
    return QStringLiteral("plugins/") + p.id;
}

}  // namespace

QString pluginsFolder()
{
    if (!configDir().isEmpty())
        return QFileInfo(QSettings().fileName()).path() + QStringLiteral("/plugins");
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/tandem-fm/plugins");
}

bool parsePluginManifest(const QByteArray& json, Plugin& out)
{
    if (out.name.isEmpty())
        out.name = out.id;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject()) {
        out.error = QObject::tr("plugin.json: %1").arg(pe.errorString());
        return false;
    }
    const QJsonObject o = doc.object();
    out.name = o.value(QLatin1String("name")).toString(out.id);
    out.description = o.value(QLatin1String("description")).toString();
    if (o.value(QLatin1String("tandem")).toInt(1) > kPluginApi) {
        out.error = QObject::tr("needs a newer Tandem");
        return false;
    }
    out.actions.clear();
    for (const QJsonValue& v : o.value(QLatin1String("actions")).toArray()) {
        const QJsonObject ao = v.toObject();
        PluginAction a;
        a.title = ao.value(QLatin1String("title")).toString();
        a.id = ao.value(QLatin1String("id")).toString(slug(a.title));
        a.run = stringList(ao.value(QLatin1String("run")));
        a.choices = stringList(ao.value(QLatin1String("choices")));
        a.forFolder = ao.value(QLatin1String("on")).toString() == QLatin1String("folder");
        a.localOnly = ao.value(QLatin1String("local")).toBool(true);
        a.types = stringList(ao.value(QLatin1String("types")));
        a.keys = stringList(ao.value(QLatin1String("keys")));
        if (a.title.isEmpty() || a.run.isEmpty() || a.id.isEmpty())
            continue;  // incomplete: skipped, the others still work
        out.actions.append(a);
    }
    if (out.actions.isEmpty()) {
        out.error = QObject::tr("no usable actions");
        return false;
    }
    out.error.clear();
    return true;
}

QList<Plugin> discoverPlugins()
{
    QStringList roots{pluginsFolder()};
    for (const QString& d : QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                                      QStringLiteral("tandem-fm/plugins"), QStandardPaths::LocateDirectory))
        if (!roots.contains(d))
            roots << d;
    QList<Plugin> out;
    QSet<QString> seen;
    for (const QString& root : std::as_const(roots)) {
        // A plugin is a single <name>.json, or a <name>/ folder with a
        // plugin.json and whatever programs it brings along.
        for (const QFileInfo& fi : QDir(root).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
            const bool folder = fi.isDir();
            if (!folder && fi.suffix() != QLatin1String("json"))
                continue;
            Plugin p;
            p.id = folder ? fi.fileName() : fi.completeBaseName();
            if (seen.contains(p.id))
                continue;  // the user's own copy wins over a package's
            seen.insert(p.id);
            p.name = p.id;
            p.dir = folder ? fi.filePath() : root;
            p.system = root != roots.first();
            QFile f(folder ? fi.filePath() + QStringLiteral("/plugin.json") : fi.filePath());
            if (!f.open(QIODevice::ReadOnly)) {
                // Shown in Settings, so a misnamed file is easy to spot.
                const QStringList json = QDir(p.dir).entryList({QStringLiteral("*.json")}, QDir::Files);
                p.error = json.isEmpty() ? QObject::tr("no plugin.json in this folder")
                                         : QObject::tr("no plugin.json in this folder (rename %1?)").arg(json.first());
                out.append(p);
                continue;
            }
            const QByteArray json = f.read(1 << 20);
            p.hash = QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex();
            parsePluginManifest(json, p);
            out.append(p);
        }
    }
    return out;
}

PluginTrust pluginTrust(const Plugin& p)
{
    const QByteArray trusted = QSettings().value(trustKey(p)).toByteArray();
    if (trusted.isEmpty())
        return PluginTrust::Off;
    return trusted == p.hash ? PluginTrust::On : PluginTrust::Changed;
}

void setPluginEnabled(const Plugin& p, bool on)
{
    if (on)
        QSettings().setValue(trustKey(p), p.hash);
    else
        QSettings().remove(trustKey(p));
}

bool pluginApplies(const PluginAction& a, const PluginContext& ctx)
{
    if (a.localOnly && !ctx.local)
        return false;
    if (a.forFolder)
        return !ctx.dir.isEmpty();
    if (ctx.selection.isEmpty())
        return false;
    if (a.types.isEmpty())
        return true;
    static const QMimeDatabase db;
    for (const QString& path : ctx.selection) {
        const QString name = path.section(QLatin1Char('/'), -1);
        bool ok = false;
        for (const QString& t : a.types) {
            if (t.contains(QLatin1Char('/'))) {  // a MIME type, told by the name: no file reads
                const QMimeType mt = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
                ok = QRegularExpression::fromWildcard(t, Qt::CaseInsensitive).match(mt.name()).hasMatch() ||
                     mt.inherits(t);
            } else {
                ok = QRegularExpression::fromWildcard(t, Qt::CaseInsensitive).match(name).hasMatch();
            }
            if (ok)
                break;
        }
        if (!ok)
            return false;
    }
    return true;
}

QStringList expandPluginArgs(const Plugin& p, const QStringList& argv, const PluginContext& ctx, const QString& choice)
{
    QStringList out;
    for (int n = 0; n < argv.size(); ++n) {
        const QString& arg = argv[n];
        if (arg == QLatin1String("%s")) {
            out << ctx.selection;
            continue;
        }
        QString s;
        for (int i = 0; i < arg.size(); ++i) {
            if (arg[i] != QLatin1Char('%') || i + 1 >= arg.size()) {
                s += arg[i];
                continue;
            }
            switch (arg[++i].unicode()) {
            case 'f': s += ctx.current; break;
            case 'n': s += ctx.current.section(QLatin1Char('/'), -1); break;
            case 'd': s += ctx.dir; break;
            case 'D': s += ctx.otherDir; break;
            case 'c': s += choice; break;
            case '%': s += QLatin1Char('%'); break;
            default: s += QLatin1Char('%'); s += arg[i];
            }
        }
        if (n == 0 && s.startsWith(QLatin1String("./")))
            s = p.dir + s.mid(1);
        out << s;
    }
    return out;
}

bool parsePluginRequest(const QByteArray& line, PluginRequest& out)
{
    if (!line.trimmed().startsWith('{'))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    out = {};
    out.message = o.value(QLatin1String("message")).toString();
    out.error = o.value(QLatin1String("error")).toString();
    out.cd = o.value(QLatin1String("cd")).toString();
    for (const QJsonValue& v : o.value(QLatin1String("select")).toArray())
        out.select << v.toString();
    out.refresh = o.value(QLatin1String("refresh")).toBool();
    return true;
}

// ------------------------------------------------------------------ runs

namespace {

QProcess* pluginProcess(const Plugin& p, const PluginContext& ctx, QObject* parent)
{
    auto* proc = new QProcess(parent);
    proc->setWorkingDirectory(ctx.local && !ctx.dir.isEmpty() ? ctx.dir : QDir::homePath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TANDEM_PLUGIN_DIR"), p.dir);
    proc->setProcessEnvironment(env);
    return proc;
}

}  // namespace

PluginRun::PluginRun(const Plugin& p, const PluginAction& a, const PluginContext& ctx, const QString& choice,
                     QObject* parent)
    : QObject(parent), m_proc(pluginProcess(p, ctx, this)), m_argv(expandPluginArgs(p, a.run, ctx, choice)),
      m_title(a.title)
{
    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this] { readLines(false); });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        emit finished(false, tr("Could not start %1").arg(m_argv.value(0)));
        deleteLater();
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readLines(true);
        const bool ok = status == QProcess::NormalExit && code == 0;
        QString text = m_requestedMessage ? QString() : m_lastLine;
        if (!ok) {
            // The first line says why; usage texts and traces only follow.
            text = QString::fromLocal8Bit(m_proc->readAllStandardError()).trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
            if (text.isEmpty())
                text = tr("%1 failed (exit %2)").arg(m_title).arg(code);
        }
        emit finished(ok, text);
        deleteLater();
    });
}

void PluginRun::start()
{
    const QString program = m_argv.value(0);
    m_proc->start(program, m_argv.mid(1));
}

void PluginRun::readLines(bool all)
{
    m_buffer += m_proc->readAllStandardOutput();
    for (;;) {
        int nl = int(m_buffer.indexOf('\n'));
        if (nl < 0) {
            if (!all || m_buffer.isEmpty())
                return;
            nl = int(m_buffer.size());
        }
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        PluginRequest r;
        if (parsePluginRequest(line, r)) {
            m_requestedMessage = m_requestedMessage || !r.message.isEmpty() || !r.error.isEmpty();
            emit request(r);
        } else if (!line.trimmed().isEmpty()) {
            m_lastLine = QString::fromLocal8Bit(line).trimmed();
        }
    }
}

void listPluginChoices(const Plugin& p, const PluginAction& a, const PluginContext& ctx, QObject* context,
                       std::function<void(const QList<QPair<QString, QString>>&, const QString&)> done)
{
    QProcess* proc = pluginProcess(p, ctx, context);
    const QStringList argv = expandPluginArgs(p, a.choices, ctx);
    auto* timer = new QTimer(proc);
    timer->setSingleShot(true);
    auto finish = [proc, done](const QList<QPair<QString, QString>>& entries, const QString& error) {
        if (proc->property("done").toBool())
            return;
        proc->setProperty("done", true);
        done(entries, error);
        proc->deleteLater();
    };
    QObject::connect(timer, &QTimer::timeout, proc, [proc, finish] {
        proc->kill();
        finish({}, QObject::tr("no answer"));
    });
    QObject::connect(proc, &QProcess::errorOccurred, proc, [argv, finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish({}, QObject::tr("Could not start %1").arg(argv.value(0)));
    });
    const QString command = argv.join(QLatin1Char(' '));
    QObject::connect(proc, &QProcess::finished, proc, [proc, finish, command](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0) {
            const QString err =
                QString::fromLocal8Bit(proc->readAllStandardError()).trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
            finish({}, QStringLiteral("%1: %2").arg(command, err.isEmpty() ? QObject::tr("failed (exit %1)").arg(code) : err));
            return;
        }
        QList<QPair<QString, QString>> entries;
        for (const QString& line : QString::fromUtf8(proc->readAllStandardOutput()).split(QLatin1Char('\n'))) {
            if (line.trimmed().isEmpty())
                continue;
            const int tab = int(line.indexOf(QLatin1Char('\t')));
            entries.append(tab < 0 ? qMakePair(line.trimmed(), line.trimmed())
                                   : qMakePair(line.left(tab).trimmed(), line.mid(tab + 1).trimmed()));
        }
        finish(entries, QString());
    });
    timer->start(5000);
    proc->start(argv.value(0), argv.mid(1));
}

}  // namespace tandem
