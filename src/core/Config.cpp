#include "core/Config.h"
#include "core/FileOps.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QLockFile>
#include <QUrl>

namespace tandem {
namespace {
QString absoluteFile(const QString& file)
{
    QString path = file;
    if (path == QLatin1String("~")) path = QDir::homePath();
    else if (path.startsWith(QLatin1String("~/"))) path = QDir::homePath() + path.mid(1);
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
QSettings nativeSettings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope,
                     QCoreApplication::organizationName(), QCoreApplication::applicationName());
}
bool readFile(const QString& file, QVariantMap& values, QString* error)
{
    QFile source(file);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = QObject::tr("Cannot read settings %1: %2").arg(file, source.errorString());
        return false;
    }
    // Parse an immutable snapshot. QSettings normally shares its cache for a
    // filename; a fresh temporary file also catches malformed external edits.
    QTemporaryFile snapshot;
    const QByteArray bytes = source.readAll();
    if (source.error() != QFileDevice::NoError || !snapshot.open() || snapshot.write(bytes) != bytes.size() || !snapshot.flush()) {
        if (error) *error = QObject::tr("Cannot read a settings snapshot");
        return false;
    }
    QSettings ini(snapshot.fileName(), QSettings::IniFormat);
    ini.setFallbacksEnabled(false);
    const auto keys = ini.allKeys();
    if (ini.status() != QSettings::NoError) {
        if (error) *error = QObject::tr("Invalid INI settings: %1. Keeping the last valid settings.").arg(file);
        return false;
    }
    values.clear();
    for (const QString& key : keys) values.insert(key, ini.value(key));
    return true;
}
bool serialize(const QVariantMap& values, QByteArray& bytes, QString* error)
{
    QTemporaryFile temporary;
    if (!temporary.open()) {
        if (error) *error = QObject::tr("Cannot create a temporary settings file");
        return false;
    }
    temporary.close();
    {
        QSettings ini(temporary.fileName(), QSettings::IniFormat);
        for (auto it = values.cbegin(); it != values.cend(); ++it) ini.setValue(it.key(), it.value());
        ini.sync();
        if (ini.status() != QSettings::NoError) {
            if (error) *error = QObject::tr("Cannot encode settings");
            return false;
        }
    }
    QFile input(temporary.fileName());
    if (!input.open(QIODevice::ReadOnly)) return false;
    bytes = input.readAll();
    return input.error() == QFileDevice::NoError;
}
bool createBytes(const QString& file, const QByteArray& bytes, QString* error)
{
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) {
        if (error) *error = QObject::tr("Cannot create settings folder: %1").arg(QFileInfo(file).absolutePath());
        return false;
    }
    QTemporaryFile temporary(QFileInfo(file).dir().filePath(QStringLiteral(".tandem-settings-XXXXXX")));
    if (!temporary.open() || temporary.write(bytes) != bytes.size() || !temporary.flush()) {
        if (error) *error = QObject::tr("Cannot write settings: %1").arg(file);
        return false;
    }
    temporary.close();
    const int result = renameNoReplace(temporary.fileName(), file);
    if (result) {
        if (error) *error = QObject::tr("Cannot create %1 (error %2); an existing file was not overwritten").arg(file).arg(result);
        return false;
    }
    return true;
}
bool createFile(const QString& file, const QVariantMap& values, QString* error)
{
    QByteArray bytes;
    return serialize(values, bytes, error) && createBytes(file, bytes, error);
}

QStringList differentKeys(const QVariantMap& a, const QVariantMap& b)
{
    QSet<QString> keys;
    for (auto it = a.cbegin(); it != a.cend(); ++it)
        if (!b.contains(it.key()) || b.value(it.key()) != it.value()) keys.insert(it.key());
    for (auto it = b.cbegin(); it != b.cend(); ++it)
        if (!a.contains(it.key())) keys.insert(it.key());
    return QStringList(keys.begin(), keys.end());
}
QByteArray fileHash(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) : QByteArray();
}
} // namespace

Config& Config::instance() { static Config config; return config; }
Config::Config() = default;
QString Config::defaultFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
}
QString Config::localLockFile(const QString& file)
{
    const QFileInfo info(file);
    const QString parent = info.dir().canonicalPath();
    const QString identity = (parent.isEmpty() ? info.absolutePath() : parent) + QLatin1Char('/') + info.fileName();
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/configuration-locks");
    QDir().mkpath(directory);
    return directory + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".lock");
}
QString Config::historyFileFor(const QString& file)
{
    const QFileInfo info(file);
    return info.dir().filePath(info.completeBaseName() == QLatin1String("settings") ? QStringLiteral("folders.tsv")
                               : info.completeBaseName() + QStringLiteral(".folders.tsv"));
}
QString Config::fileName() const
{
    QMutexLocker lock(&m_mutex);
    return m_file.isEmpty() ? defaultFile() : m_file;
}
QString Config::historyFile() const { return historyFileFor(fileName()); }
bool Config::isCustom() const { return fileName() != absoluteFile(defaultFile()); }
void Config::ensureLoaded()
{
    if (!m_file.isEmpty()) return;
    m_file = absoluteFile(defaultFile());
    if (QFileInfo::exists(m_file)) {
        readFile(m_file, m_values, nullptr);
        m_allowCreate = false;
    }
}
QVariantMap Config::snapshot()
{
    QMutexLocker lock(&m_mutex);
    ensureLoaded();
    return m_values;
}
bool Config::initialize(const QString& requestedFile, QString* error)
{
    auto native = nativeSettings();
    QString selected = requestedFile;
    bool explicitlySelected = !selected.isEmpty();
    if (selected.isEmpty() && !qEnvironmentVariableIsEmpty("TANDEM_CONFIG_FILE")) {
        selected = qEnvironmentVariable("TANDEM_CONFIG_FILE"); explicitlySelected = true;
    } else if (selected.isEmpty() && !qEnvironmentVariableIsEmpty("TANDEM_CONFIG_DIR")) {
        selected = QDir(qEnvironmentVariable("TANDEM_CONFIG_DIR")).filePath(QStringLiteral("settings.ini")); explicitlySelected = true;
    }
    if (selected.isEmpty()) selected = native.value(QStringLiteral("configuration/file")).toString();
    if (!selected.isEmpty() && !explicitlySelected && !QFileInfo::exists(selected)) {
        if (error) *error = tr("The configured settings file is unavailable: %1. Restore the folder or start with --config-dir to choose another location.").arg(selected);
        return false;
    }
    {
        QMutexLocker lock(&m_mutex);
        ensureLoaded();
        if (!QFileInfo::exists(m_file))
            for (const QString& key : native.allKeys())
                if (!key.startsWith(QLatin1String("configuration/"))) m_values.insert(key, native.value(key));
    }
    return useFile(selected, error, false);
}
bool Config::useFile(const QString& file, QString* error, bool remember)
{
    const QString target = absoluteFile(file.isEmpty() ? defaultFile() : file);
    const QString oldFile = fileName();
    QVariantMap previous = snapshot();
    {
        QMutexLocker lock(&m_mutex);
        if (!m_pending.isEmpty()) {
            if (error) *error = tr("Settings have unsaved changes. Restore the current settings file before switching locations.");
            return false;
        }
    }
    if (previous.contains(QStringLiteral("network/recent"))) {
        QStringList servers;
        for (const QString& server : previous.value(QStringLiteral("network/recent")).toStringList()) {
            QUrl uri(server); uri.setPassword(QString()); servers.append(uri.toString());
        }
        previous.insert(QStringLiteral("network/recent"), servers);
    }
    if (!QFileInfo::exists(target) && !createFile(target, previous, error)) return false;
    QVariantMap incoming;
    if (!readFile(target, incoming, error)) return false;
    QString oldHistory = historyFileFor(oldFile);
    if (!QFileInfo::exists(oldHistory))
        oldHistory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/folders.tsv");
    const QString newHistory = historyFileFor(target);
    if (oldHistory != newHistory && QFileInfo::exists(oldHistory) && !QFileInfo::exists(newHistory)) {
        QFile history(oldHistory);
        if (!history.open(QIODevice::ReadOnly)) {
            if (error) *error = tr("Cannot read bookmarks: %1").arg(oldHistory);
            return false;
        }
        const QByteArray bytes = history.readAll();
        if (history.error() != QFileDevice::NoError || !createBytes(newHistory, bytes, error)) return false;
    }
    if (remember) {
        auto native = nativeSettings();
        native.setValue(QStringLiteral("configuration/file"), target == absoluteFile(defaultFile()) ? QString() : target);
        native.sync();
        if (native.status() != QSettings::NoError) {
            if (error) *error = tr("Cannot save the settings location on this computer");
            return false;
        }
    }
    {
        QMutexLocker lock(&m_mutex);
        m_file = target; m_values = incoming; m_pending.clear(); m_allowCreate = false;
    }
    m_historyHash = fileHash(newHistory);
    startWatching();
    emit locationChanged();
    emit changed(differentKeys(previous, incoming));
    emit historyChanged();
    return true;
}
bool Config::writePending(QString* error)
{
    if (m_pending.isEmpty()) return true;
    // Locks belong to this machine, never to the synced folder. A cloud copy
    // of another machine's transient lock must not block local writes.
    QLockFile lock(localLockFile(m_file));
    if (!lock.tryLock(1000)) {
        if (error) *error = tr("Settings are busy; changes will be retried: %1").arg(m_file);
        return false;
    }
    QVariantMap disk;
    if (!QFileInfo::exists(m_file) && m_allowCreate) {
        if (!createFile(m_file, {}, error)) return false;
        m_allowCreate = false;
    }
    if (!readFile(m_file, disk, error)) return false;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it.value()) disk.insert(it.key(), *it.value()); else disk.remove(it.key());
    }
    QByteArray bytes;
    if (!serialize(disk, bytes, error)) return false;
    QSaveFile output(m_file);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        if (error) *error = tr("Cannot save settings to %1. Changes are kept in memory and will be retried.").arg(m_file);
        return false;
    }
    m_values = disk; m_pending.clear();
    return true;
}
bool Config::commit(const QMap<QString, std::optional<QVariant>>& changes)
{
    if (changes.isEmpty()) return true;
    QString error;
    bool ok;
    QStringList incoming;
    {
        QMutexLocker lock(&m_mutex);
        ensureLoaded();
        const auto before = m_values;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            m_pending.insert(it.key(), it.value());
            if (it.value()) m_values.insert(it.key(), *it.value()); else m_values.remove(it.key());
        }
        ok = writePending(&error);
        incoming = differentKeys(before, m_values);
        for (const QString& key : changes.keys()) incoming.removeAll(key);
    }
    if (!incoming.isEmpty())
        QMetaObject::invokeMethod(this, [this, incoming] { emit changed(incoming); }, Qt::QueuedConnection);
    if (!ok) reportError(error);
    return ok;
}
void Config::reportError(const QString& message)
{
    if (message.isEmpty()) return;
    QMetaObject::invokeMethod(this, [this, message] {
        if (m_lastError != message) { m_lastError = message; emit storageError(message); }
    }, Qt::QueuedConnection);
}
void Config::startWatching()
{
    if (!m_watch) {
        m_watch = new QFileSystemWatcher(this);
        m_timer = new QTimer(this);
        m_timer->setInterval(1000);
        connect(m_timer, &QTimer::timeout, this, &Config::checkForChanges);
        auto later = [this] { QTimer::singleShot(200, this, &Config::checkForChanges); };
        connect(m_watch, &QFileSystemWatcher::fileChanged, this, later);
        m_timer->start();
    }
    watchPaths();
}
void Config::watchPaths()
{
    if (!m_watch) return;
    const QStringList desired{fileName(), historyFile()};
    const auto watched = m_watch->files() + m_watch->directories();
    for (const QString& path : watched) if (!desired.contains(path)) m_watch->removePath(path);
    for (const QString& path : desired)
        if (QFileInfo::exists(path) && !watched.contains(path)) m_watch->addPath(path);
}
void Config::checkForChanges()
{
    QStringList keys;
    QString error;
    {
        QMutexLocker lock(&m_mutex);
        ensureLoaded();
        const auto before = m_values;
        QVariantMap disk;
        if (readFile(m_file, disk, &error)) {
            for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it)
                if (it.value()) disk.insert(it.key(), *it.value()); else disk.remove(it.key());
            m_values = disk;
            writePending(&error);
            keys = differentKeys(before, m_values);
        }
    }
    if (!error.isEmpty()) reportError(error); else m_lastError.clear();
    watchPaths(); // cloud clients/editors replace files; re-arm their watches
    if (!keys.isEmpty()) emit changed(keys);
    const auto hash = fileHash(historyFile());
    if (!hash.isEmpty() && hash != m_historyHash) { m_historyHash = hash; emit historyChanged(); }
}

Settings::Settings() : m_values(Config::instance().snapshot()), m_file(Config::instance().fileName()) {}
Settings::~Settings() { sync(); }
QString Settings::fullKey(const QString& key) const
{
    const QString group = m_groups.join(QLatin1Char('/'));
    return group.isEmpty() ? key : key.isEmpty() ? group : group + QLatin1Char('/') + key;
}
QVariant Settings::value(const QString& key, const QVariant& fallback) const { return m_values.value(fullKey(key), fallback); }
bool Settings::contains(const QString& key) const { return m_values.contains(fullKey(key)); }
void Settings::setValue(const QString& key, const QVariant& value)
{
    const QString name = fullKey(key);
    if (m_values.contains(name) && m_values.value(name) == value) return;
    m_values.insert(name, value); m_changes.insert(name, value);
}
void Settings::remove(const QString& key)
{
    const QString name = fullKey(key);
    for (const QString& stored : m_values.keys())
        if (name.isEmpty() || stored == name || stored.startsWith(name + QLatin1Char('/'))) {
            m_values.remove(stored); m_changes.insert(stored, std::nullopt);
        }
}
void Settings::clear() { const auto groups = m_groups; m_groups.clear(); remove({}); m_groups = groups; }
void Settings::beginGroup(const QString& group) { m_groups.append(group); }
void Settings::endGroup() { if (!m_groups.isEmpty()) m_groups.removeLast(); }
QStringList Settings::allKeys() const
{
    QStringList keys;
    const QString prefix = fullKey({});
    for (const QString& key : m_values.keys())
        if (prefix.isEmpty()) keys.append(key);
        else if (key.startsWith(prefix + QLatin1Char('/'))) keys.append(key.mid(prefix.size() + 1));
    return keys;
}
void Settings::sync()
{
    // An object opened before a location switch must never write to the new
    // profile with stale data. GUI switches end all their Settings scopes first.
    if (!m_changes.isEmpty() && m_file == Config::instance().fileName() && Config::instance().commit(m_changes))
        m_changes.clear();
}
QString Settings::fileName() const { return m_file; }
} // namespace tandem
