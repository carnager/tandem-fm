#include "core/Plugins.h"
#include "core/Config.h"
#include "core/FileOps.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {
namespace {
constexpr qsizetype manifestLimit = 256 * 1024;
constexpr qsizetype outputLimit = 1024 * 1024;
constexpr qsizetype inputLimit = 4 * 1024 * 1024;
bool identifier(const QString& value)
{
    static const QRegularExpression re(QStringLiteral("^[a-z][a-z0-9_-]{0,63}$"));
    return re.match(value).hasMatch();
}
bool text(const QJsonValue& value, int limit, bool empty = false)
{
    if (!value.isString() || value.toString().size() > limit || (!empty && value.toString().trimmed().isEmpty())) return false;
    return !value.toString().contains(QChar::Null);
}
bool knownKeys(const QJsonObject& obj, const QStringList& keys, QString* error)
{
    for (const auto& key : obj.keys()) if (!keys.contains(key)) {
        if (error) *error = QObject::tr("Unknown manifest/result key: %1").arg(key); return false;
    }
    return true;
}
bool webUrl(const QString& address)
{
    const QUrl url(address, QUrl::StrictMode);
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty()
           && url.userInfo().isEmpty() && address.size() <= 4096;
}
QString secretFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/plugin-private/secrets.json");
}
bool loadSecrets(QJsonObject& result, QString* error)
{
    QFile file(secretFile());
    if (QFileInfo(QFileInfo(file).path()).isSymLink()) {
        if (error) *error = QObject::tr("The private plugin settings folder cannot be a symbolic link.");
        return false;
    }
    if (!file.exists()) return true;
    if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > outputLimit) {
        if (error) *error = QObject::tr("Cannot read the local plugin secrets file."); return false;
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.read(outputLimit + 1), &parse);
    if (!doc.isObject() || parse.error != QJsonParseError::NoError) {
        if (error) *error = QObject::tr("The local plugin secrets file is invalid; it was not replaced."); return false;
    }
    result = doc.object(); return true;
}
bool writeSecrets(const Plugin& plugin, const QJsonObject& values, QString* error)
{
    const QString parent = QFileInfo(secretFile()).path();
    if (QFileInfo(parent).isSymLink() || !QDir().mkpath(parent) ||
        !QFile::setPermissions(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        if (error) *error = QObject::tr("Cannot create a private plugin settings folder."); return false;
    }
    QLockFile lock(secretFile() + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) { if (error) *error = QObject::tr("Plugin settings are being saved by another instance. Try again."); return false; }
    QJsonObject secrets;
    if (!loadSecrets(secrets, error)) return false;
    for (const auto& setting : plugin.settings) if (setting.kind == "secret") {
        const QString key = plugin.id + QLatin1Char(':') + setting.key;
        if (values.value(setting.key).toString().isEmpty()) secrets.remove(key);
        else secrets.insert(key, values.value(setting.key));
    }
    QSaveFile file(secretFile());
    const QByteArray data = QJsonDocument(secrets).toJson(QJsonDocument::Compact);
    if (data.size() > outputLimit) {
        if (error) *error = QObject::tr("Local plugin secrets exceed 1 MB; existing values were kept.");
        return false;
    }
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(data) != data.size() || !file.commit()) {
        if (error) *error = QObject::tr("Cannot save local plugin secrets: %1").arg(file.errorString()); return false;
    }
    return true;
}
}
std::optional<Plugin> parsePluginManifest(const QByteArray& data, const QString& folder, QString* error)
{
    if (error) error->clear();
    auto fail = [&](const QString& why) -> std::optional<Plugin> { if (error) *error = why; return {}; };
    if (data.size() > manifestLimit) return fail(QObject::tr("Manifest exceeds 256 KB."));
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(data, &parse);
    if (!doc.isObject() || parse.error != QJsonParseError::NoError) return fail(QObject::tr("plugin.json is not valid JSON."));
    const auto root = doc.object();
    if (!knownKeys(root, {"schema", "name", "description", "actions", "settings"}, error)) return {};
    if (root.value("schema").toDouble() != 1) return fail(QObject::tr("Unsupported plugin schema; expected 1."));
    Plugin plugin;
    plugin.folder = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    plugin.id = QFileInfo(plugin.folder).fileName();
    if (!identifier(plugin.id)) return fail(QObject::tr("Plugin folder names use lowercase letters, digits, hyphens and underscores, starting with a letter (max 64)."));
    if (!text(root.value("name"), 128) || (root.contains("description") && !text(root.value("description"), 4096, true)))
        return fail(QObject::tr("Provide a name and an optional text description."));
    plugin.name = root.value("name").toString().trimmed(); plugin.description = root.value("description").toString();
    plugin.manifestHash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    if (!root.value("actions").isArray() || root.value("actions").toArray().isEmpty() || root.value("actions").toArray().size() > 64)
        return fail(QObject::tr("Declare 1–64 actions."));
    QSet<QString> ids;
    for (const auto& value : root.value("actions").toArray()) {
        if (!value.isObject()) return fail(QObject::tr("Each action must be an object."));
        const auto obj = value.toObject();
        if (!knownKeys(obj, {"id", "label", "for", "run", "result", "timeout", "locations"}, error)) return {};
        PluginAction action;
        action.id = obj.value("id").toString(); action.label = obj.value("label").toString();
        if (!identifier(action.id) || ids.contains(action.id) || !text(obj.value("label"), 128))
            return fail(QObject::tr("Actions need unique ids and text labels."));
        ids.insert(action.id);
        if (obj.contains("for")) action.target = obj.value("for").toString();
        if (!QStringList{"selection", "files", "folders", "always"}.contains(action.target)) return fail(QObject::tr("Action 'for' must be selection, files, folders or always."));
        if (obj.contains("result")) action.result = obj.value("result").toString();
        if (!QStringList{"notify", "copy", "notify-and-copy", "list", "none"}.contains(action.result)) return fail(QObject::tr("Unsupported action result type."));
        if (obj.contains("timeout")) {
            const double seconds = obj.value("timeout").toDouble(-1);
            if (seconds < 1 || seconds > 3600 || seconds != int(seconds)) return fail(QObject::tr("Timeout must be a whole number from 1 to 3600 seconds."));
            action.timeout = int(seconds);
        }
        if (!obj.value("run").isArray() || obj.value("run").toArray().isEmpty() || obj.value("run").toArray().size() > 128)
            return fail(QObject::tr("Action 'run' must contain a program and optional arguments."));
        for (const auto& arg : obj.value("run").toArray()) {
            if (!text(arg, 8192, true)) return fail(QObject::tr("Program arguments must be text without NUL characters."));
            action.run << arg.toString();
        }
        const QString program = action.run.first();
        if (program.isEmpty()) return fail(QObject::tr("An action needs a program."));
        if (program.startsWith("./")) {
            if (program.split('/').contains("..")) return fail(QObject::tr("A relative plugin program must stay inside its folder."));
        } else if (!QDir::isAbsolutePath(program) && (program.contains('/') || program.contains('\\')))
            return fail(QObject::tr("Programs use ./ inside the plugin, an absolute path, or a name in PATH."));
        if (obj.contains("locations")) {
            if (!obj.value("locations").isArray() || obj.value("locations").toArray().isEmpty()) return fail(QObject::tr("Locations must be a nonempty list."));
            action.locations.clear();
            for (const auto& location : obj.value("locations").toArray()) {
                if (!location.isString() || !QStringList{"local", "remote", "archive"}.contains(location.toString()))
                    return fail(QObject::tr("Locations may be local, remote or archive."));
                if (!action.locations.contains(location.toString())) action.locations << location.toString();
            }
        }
        plugin.actions << action;
    }
    if (root.contains("settings") && (!root.value("settings").isArray() || root.value("settings").toArray().size() > 64))
        return fail(QObject::tr("Settings must be a list of at most 64 entries."));
    ids.clear(); QSet<QString> toggles;
    for (const auto& value : root.value("settings").toArray()) {
        if (!value.isObject()) return fail(QObject::tr("Each setting must be an object."));
        const auto obj = value.toObject();
        if (!knownKeys(obj, {"key", "label", "kind", "default", "choices", "help", "requires", "link"}, error)) return {};
        PluginSetting setting;
        setting.key = obj.value("key").toString(); setting.label = obj.value("label").toString();
        if (!identifier(setting.key) || ids.contains(setting.key) || !text(obj.value("label"), 128))
            return fail(QObject::tr("Settings need unique keys and labels."));
        ids.insert(setting.key);
        if (obj.contains("kind")) setting.kind = obj.value("kind").toString();
        if (!QStringList{"text", "secret", "toggle", "choice"}.contains(setting.kind)) return fail(QObject::tr("Unsupported setting kind."));
        for (const auto& key : {QStringLiteral("help"), QStringLiteral("requires"), QStringLiteral("link")})
            if (obj.contains(key) && !text(obj.value(key), 4096, key == "help")) return fail(QObject::tr("Setting help, requires and link must be text."));
        setting.help = obj.value("help").toString(); setting.requiresToggle = obj.value("requires").toString(); setting.link = obj.value("link").toString();
        if (!setting.requiresToggle.isEmpty() && !toggles.contains(setting.requiresToggle)) return fail(QObject::tr("'requires' must name an earlier toggle."));
        if (!setting.link.isEmpty() && !webUrl(setting.link)) return fail(QObject::tr("Setting links must use http or https without credentials."));
        if (setting.kind != "choice" && obj.contains("choices")) return fail(QObject::tr("Only choice settings declare choices."));
        if (setting.kind == "choice") {
            if (!obj.value("choices").isArray() || obj.value("choices").toArray().isEmpty() || obj.value("choices").toArray().size() > 64)
                return fail(QObject::tr("A choice needs 1–64 options."));
            for (const auto& option : obj.value("choices").toArray()) {
                if (!text(option, 1024) || setting.choices.contains(option.toString())) return fail(QObject::tr("Choices must be unique text values."));
                setting.choices << option.toString();
            }
            setting.fallback = setting.choices.first();
        } else if (setting.kind == "toggle") setting.fallback = false;
        else setting.fallback = QString();
        if (obj.contains("default")) {
            if (setting.kind == "secret") return fail(QObject::tr("Secrets cannot have manifest defaults."));
            if (setting.kind == "toggle" ? !obj.value("default").isBool() : !text(obj.value("default"), 8192, true))
                return fail(QObject::tr("Setting default has the wrong type."));
            setting.fallback = obj.value("default").toVariant();
        }
        if (setting.kind == "choice" && !setting.choices.contains(setting.fallback.toString())) return fail(QObject::tr("Choice default must be one of its choices."));
        if (setting.kind == "toggle") toggles.insert(setting.key);
        plugin.settings << setting;
    }
    return plugin;
}
QString userPluginFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/tandem/plugins");
}
QStringList pluginFolders()
{
    if (qEnvironmentVariableIsSet("TANDEM_PLUGIN_PATH"))
        return qEnvironmentVariable("TANDEM_PLUGIN_PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    QStringList roots;
    for (const auto& data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) roots << QDir(data).filePath("tandem/plugins");
    const QDir bin(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    roots << bin.filePath("../Resources/plugins");
#endif
    roots << bin.filePath("../share/tandem/plugins") << bin.filePath("plugins");
    roots.removeDuplicates(); return roots;
}
QList<FoundPlugin> discoverPlugins()
{
    QList<FoundPlugin> result; QSet<QString> seen;
    for (const auto& root : pluginFolders()) {
        const QDir dir(root);
        for (const auto& id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
            if (seen.contains(id) || id.startsWith('.')) continue;
            QFile file(dir.filePath(id + "/plugin.json"));
            if (!file.exists()) continue;
            seen.insert(id);
            FoundPlugin found; found.id = id; found.folder = dir.filePath(id);
            if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly)) found.problem = QObject::tr("Cannot read plugin.json (symlinks are not accepted).");
            else found.plugin = parsePluginManifest(file.read(manifestLimit + 1), found.folder, &found.problem);
            result << found;
        }
    }
    return result;
}
bool pluginEnabled(const QString& id) { return identifier(id) && Settings().value("plugins/enabled/" + id, false).toBool(); }
void setPluginEnabled(const QString& id, bool enabled) { if (identifier(id)) Settings().setValue("plugins/enabled/" + id, enabled); }
QString pluginSettingKey(const QString& id, const QString& key) { return "plugins/settings/" + id + '/' + key; }
QVariant pluginSettingValue(const Plugin& plugin, const PluginSetting& setting)
{
    if (setting.kind == "secret") {
        QJsonObject secrets; loadSecrets(secrets, nullptr);
        return secrets.value(plugin.id + ':' + setting.key).toString();
    }
    const auto value = Settings().value(pluginSettingKey(plugin.id, setting.key), setting.fallback);
    if (setting.kind == "choice" && !setting.choices.contains(value.toString())) return setting.fallback;
    if (setting.kind == "toggle") return value.toBool();
    return value.toString().left(8192);
}
QJsonObject pluginSettings(const Plugin& plugin)
{
    QJsonObject values;
    for (const auto& setting : plugin.settings) values.insert(setting.key, QJsonValue::fromVariant(pluginSettingValue(plugin, setting)));
    return values;
}
bool savePluginSettings(const Plugin& plugin, const QJsonObject& values, QString* error)
{
    if (error) error->clear();
    bool secrets = false;
    for (const auto& setting : plugin.settings) {
        const auto v = values.value(setting.key);
        if ((setting.kind == "toggle" ? !v.isBool() : !text(v, 8192, true)) ||
            (setting.kind == "choice" && !setting.choices.contains(v.toString()))) {
            if (error) *error = QObject::tr("Invalid value for %1.").arg(setting.label); return false;
        }
        secrets |= setting.kind == "secret";
    }
    if (secrets && !writeSecrets(plugin, values, error)) return false;
    QMap<QString, std::optional<QVariant>> changes;
    for (const auto& setting : plugin.settings) if (setting.kind != "secret") changes.insert(pluginSettingKey(plugin.id, setting.key), values.value(setting.key).toVariant());
    const bool ok = Config::instance().commit(changes);
    if (!ok && error) *error = QObject::tr("Plugin preferences could not be saved to the configuration file.");
    return ok;
}
bool pluginActionAvailable(const PluginAction& action, const PluginContext& context)
{
    if (!context.error.isEmpty() || !action.locations.contains(context.location)) return false;
    if (action.target == "always") return true;
    if (context.selected.isEmpty()) return false;
    for (const auto& value : context.selected) {
        const bool dir = value.toObject().value("is_directory").toBool();
        if ((action.target == "files" && dir) || (action.target == "folders" && !dir)) return false;
    }
    return true;
}
QByteArray pluginInput(const Plugin& plugin, const PluginAction& action, const PluginContext& context)
{
    return QJsonDocument(QJsonObject{{"schema", 1}, {"app", "tandem"}, {"action", action.id},
        {"active_panel", context.active}, {"other_panel", context.other}, {"selection", context.selected},
        {"settings", pluginSettings(plugin)}}).toJson(QJsonDocument::Compact);
}
std::optional<PluginList> parsePluginList(const QByteArray& data, QString* error)
{
    auto fail = [&](const QString& why) -> std::optional<PluginList> { if (error) *error = why; return {}; };
    if (data.size() > outputLimit) return fail(QObject::tr("Plugin result exceeds 1 MB."));
    QJsonParseError parse; const auto doc = QJsonDocument::fromJson(data, &parse);
    if (!doc.isObject() || parse.error != QJsonParseError::NoError) return fail(QObject::tr("Plugin list is not valid JSON."));
    const auto obj = doc.object();
    if (!knownKeys(obj, {"title", "items"}, error)) return {};
    if (!text(obj.value("title"), 256) || !obj.value("items").isArray() || obj.value("items").toArray().size() > 5000)
        return fail(QObject::tr("Plugin list needs a title and at most 5000 items."));
    PluginList list; list.title = obj.value("title").toString();
    for (const auto& value : obj.value("items").toArray()) {
        if (!value.isObject()) return fail(QObject::tr("List items must be objects."));
        const auto item = value.toObject();
        if (!knownKeys(item, {"text", "detail", "url"}, error)) return {};
        if (!text(item.value("text"), 4096) || (item.contains("detail") && !text(item.value("detail"), 8192, true)) ||
            (item.contains("url") && (!item.value("url").isString() || !webUrl(item.value("url").toString()))))
            return fail(QObject::tr("List rows need text; optional links must be http/https without credentials."));
        list.items << PluginListItem{item.value("text").toString(), item.value("detail").toString(), item.value("url").toString()};
    }
    return list;
}
PluginRun::PluginRun(Plugin plugin, PluginAction action, QByteArray input, QObject* parent)
    : QObject(parent), m_plugin(std::move(plugin)), m_action(std::move(action)), m_input(std::move(input)) {}
PluginRun::~PluginRun()
{
    disconnect(&m_process, nullptr, this, nullptr);
    killGroup();
    if (m_process.state() != QProcess::NotRunning) { m_process.kill(); m_process.waitForFinished(1000); }
}
void PluginRun::killGroup()
{
    const qint64 pid = m_pid > 0 ? m_pid : m_process.processId();
    if (pid > 0) ::kill(-pid_t(pid), SIGKILL);
}
void PluginRun::complete(bool success, const QString& text)
{
    if (m_done) return;
    m_done = true; m_succeeded = success; m_text = text;
    emit finished();
}
void PluginRun::readOutput()
{
    m_stdout += m_process.readAllStandardOutput(); m_stderr += m_process.readAllStandardError();
    if (m_stdout.size() + m_stderr.size() > outputLimit) {
        m_stdout.truncate(outputLimit); m_stderr.truncate(outputLimit);
        if (m_failure.isEmpty()) m_failure = tr("Plugin output exceeded 1 MB.");
        killGroup(); m_process.kill();
    }
}
void PluginRun::stop(const QString& error)
{
    if (m_done) return;
    if (m_failure.isEmpty()) m_failure = error;
    killGroup(); m_process.kill();
    if (m_process.state() == QProcess::NotRunning) complete(false, m_failure);
}
void PluginRun::cancel() { stop(tr("Plugin cancelled.")); }
void PluginRun::start()
{
    if (m_started || m_done) return;
    m_started = true;
    if (m_input.size() > inputLimit) { complete(false, tr("Plugin input exceeds 4 MB; select fewer files.")); return; }
    if (!pluginEnabled(m_plugin.id)) { complete(false, tr("Plugin is disabled.")); return; }
    QFile manifest(QDir(m_plugin.folder).filePath("plugin.json"));
    if (QFileInfo(manifest).isSymLink() || !manifest.open(QIODevice::ReadOnly) ||
        QCryptographicHash::hash(manifest.read(manifestLimit + 1), QCryptographicHash::Sha256) != m_plugin.manifestHash) {
        complete(false, tr("Plugin manifest changed; reload plugins before running it.")); return;
    }
    QString program = m_action.run.value(0);
    if (program.startsWith("./")) {
        const QString root = QFileInfo(m_plugin.folder).canonicalFilePath();
        program = QFileInfo(QDir(m_plugin.folder).filePath(program.mid(2))).canonicalFilePath();
        if (root.isEmpty() || program.isEmpty() || !program.startsWith(root + '/')) {
            complete(false, tr("Plugin program is missing or points outside its folder.")); return;
        }
    } else if (!QDir::isAbsolutePath(program)) {
        auto paths = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
        paths.prepend(QCoreApplication::applicationDirPath());
        program = QStandardPaths::findExecutable(program, paths);
    }
    if (program.isEmpty()) { complete(false, tr("Plugin program was not found in PATH.")); return; }
    m_process.setWorkingDirectory(m_plugin.folder); m_process.setProgram(program); m_process.setArguments(m_action.run.mid(1));
    m_process.setChildProcessModifier([] { if (::setpgid(0, 0) != 0) ::_exit(126); });
    connect(&m_process, &QProcess::started, this, [this] {
        m_pid = m_process.processId(); m_process.write(m_input); m_input.clear(); m_process.closeWriteChannel();
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &PluginRun::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, &PluginRun::readOutput);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete(false, tr("Could not start plugin: %1").arg(m_process.errorString()));
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput(); killGroup(); m_pid = 0;
        if (!m_failure.isEmpty()) complete(false, m_failure);
        else if (status == QProcess::NormalExit && code == 0) complete(true, QString::fromUtf8(m_stdout));
        else {
            const QString message = QString::fromUtf8(m_stderr).trimmed();
            complete(false, !message.isEmpty() ? message : status == QProcess::CrashExit ? tr("Plugin crashed.") : tr("Plugin ended with exit code %1.").arg(code));
        }
    });
    auto* timer = new QTimer(this); timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this] { if (!m_done) stop(tr("Plugin timed out after %1 seconds.").arg(m_action.timeout)); });
    timer->start(m_action.timeout * 1000); m_process.start();
}

PluginInstallResult installPluginFolder(const QString& source, const QString& destinationRoot, const std::atomic<bool>* cancel)
{
    PluginInstallResult result;
    auto fail = [&](const QString& reason) { result.error = reason; return result; };
    const QFileInfo info(source);
    if (!info.isDir() || info.isSymLink()) return fail(QObject::tr("Choose a real plugin folder containing plugin.json."));
    QFile manifest(QDir(source).filePath("plugin.json"));
    if (QFileInfo(manifest).isSymLink() || !manifest.open(QIODevice::ReadOnly)) return fail(QObject::tr("Cannot read plugin.json."));
    QString problem;
    auto plugin = parsePluginManifest(manifest.read(manifestLimit + 1), source, &problem);
    if (!plugin) return fail(problem);
    result.id = plugin->id;
    const QString srcRoot = info.canonicalFilePath();
    // Resolve existing ancestors before making anything, including when the
    // destination is absent or spelled through /var's macOS symlink.
    QString ancestor = QFileInfo(destinationRoot).absoluteFilePath();
    QStringList suffix;
    while (!QFileInfo::exists(ancestor)) {
        const QFileInfo part(ancestor);
        suffix.prepend(part.fileName());
        const QString parent = part.dir().absolutePath();
        if (parent == ancestor) return fail(QObject::tr("Cannot resolve the plugin destination."));
        ancestor = parent;
    }
    QString resolvedDestination = QFileInfo(ancestor).canonicalFilePath();
    for (const auto& part : suffix) resolvedDestination = QDir(resolvedDestination).filePath(part);
    if (resolvedDestination == srcRoot || resolvedDestination.startsWith(srcRoot + '/'))
        return fail(QObject::tr("The destination cannot be inside the source plugin."));
    if (!QDir().mkpath(destinationRoot)) return fail(QObject::tr("Cannot create the plugin folder."));
    const QDir root(destinationRoot);
    const QString destination = root.filePath(result.id);
    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) return fail(QObject::tr("A user plugin with this folder name is already installed. It was not overwritten."));
    QTemporaryDir stage(root.filePath(".install-XXXXXX"));
    if (!stage.isValid()) return fail(QObject::tr("Cannot create a plugin staging folder."));
    const QString payload = stage.filePath(result.id);
    if (!QDir().mkdir(payload)) return fail(QObject::tr("Cannot create the staged plugin."));
    QDir sourceDir(srcRoot);
    QDirIterator it(srcRoot, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    qint64 bytes = 0; int entries = 0;
    while (it.hasNext()) {
        if (cancel && cancel->load()) return fail(QObject::tr("Plugin installation cancelled."));
        it.next(); const QFileInfo entry = it.fileInfo();
        if (++entries > 5000) return fail(QObject::tr("Plugin package contains more than 5000 entries."));
        if (entry.isSymLink()) return fail(QObject::tr("Plugin packages cannot contain symbolic links."));
        const QString target = QDir(payload).filePath(sourceDir.relativeFilePath(entry.filePath()));
        if (entry.isDir()) { if (!QDir().mkpath(target)) return fail(QObject::tr("Cannot create a plugin subfolder.")); continue; }
        const QByteArray native = QFile::encodeName(entry.filePath());
        const int fd = ::open(native.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat st{};
        if (fd < 0) return fail(QObject::tr("Cannot open a plugin file."));
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); return fail(QObject::tr("Plugin packages contain only regular files and directories.")); }
        QFile input;
        if (!input.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); return fail(QObject::tr("Cannot read a plugin file.")); }
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly)) return fail(output.errorString());
        for (;;) {
            if (cancel && cancel->load()) return fail(QObject::tr("Plugin installation cancelled."));
            const QByteArray chunk = input.read(64 * 1024);
            if (chunk.isEmpty()) { if (input.error() != QFileDevice::NoError) return fail(input.errorString()); break; }
            bytes += chunk.size(); if (bytes > 100 * 1024 * 1024) return fail(QObject::tr("Plugin package exceeds 100 MB."));
            if (output.write(chunk) != chunk.size()) return fail(output.errorString());
        }
        if (!output.setPermissions(entry.permissions()) || !output.commit()) return fail(QObject::tr("Cannot finish a staged plugin file."));
    }
    QFile stagedManifest(QDir(payload).filePath("plugin.json"));
    if (!stagedManifest.open(QIODevice::ReadOnly) || QCryptographicHash::hash(stagedManifest.read(manifestLimit + 1), QCryptographicHash::Sha256) != plugin->manifestHash)
        return fail(QObject::tr("Plugin manifest changed during installation; retry with an unchanged folder."));
    if (cancel && cancel->load()) return fail(QObject::tr("Plugin installation cancelled."));
    // Use Tandem's exclusive native rename; never replace a raced-in destination.
    if (renameNoReplace(payload, destination) != 0) return fail(QObject::tr("Cannot install the plugin; the destination was not overwritten."));
    return result;
}
} // namespace tandem
