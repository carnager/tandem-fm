#include "core/FolderHistory.h"
#include "core/Config.h"

#include "core/Util.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSaveFile>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>

namespace tandem {

namespace {
constexpr double kMaxTotal = 10000;  // like zoxide: age everything once the sum gets large
}

FolderHistory& FolderHistory::instance()
{
    static FolderHistory h;
    return h;
}

QString FolderHistory::storagePath() const
{
    QMutexLocker lock(&m_mutex);
    if (!m_storage.isEmpty())
        return m_storage;
    return Config::instance().historyFile();
}

void FolderHistory::setStoragePath(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    m_storage = path;
    m_entries.clear();
    m_pinned.clear();
    m_keys.clear();
    m_baseline = {};
    m_loaded = false;
}

double FolderHistory::frecency(double count, qint64 lastVisit, qint64 now)
{
    const qint64 age = now - lastVisit;
    if (age < 3600)
        return count * 4;
    if (age < 86400)
        return count * 2;
    if (age < 7 * 86400)
        return count / 2;
    return count / 4;
}

void FolderHistory::loadAsync()
{
    ioPool()->start([this] { load(); });
}

void FolderHistory::load()
{
    const QString path = storagePath();
    QMutexLocker lock(&m_mutex);
    const QString current = m_storage.isEmpty() ? Config::instance().historyFile() : m_storage;
    if (path != current) return;
    Snapshot disk;
    const bool exists = QFileInfo::exists(path);
    if (exists && !readSnapshot(path, disk)) return;
    if (!exists) {
        const QString home = QDir::homePath();
        for (const auto& [key, folder] : {std::pair{QStringLiteral("h"), home},
                                             std::pair{QStringLiteral("d"), home + QStringLiteral("/Downloads")},
                                             std::pair{QStringLiteral("c"), home + QStringLiteral("/.config")},
                                             std::pair{QStringLiteral("/"), QStringLiteral("/")}}) {
            if (!m_pinned.contains(folder)) m_pinned.append(folder);
            if (!m_keys.contains(folder) && pathForKeyLocked(key).isEmpty()) m_keys.insert(folder, key);
        }
    }
    const Snapshot merged = mergeDiskLocked(disk);
    m_entries = merged.entries; m_pinned = merged.pinned; m_keys = merged.keys;
    m_baseline = disk;
    m_loaded = true;
}

bool FolderHistory::readSnapshot(const QString& path, Snapshot& out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    bool v2 = false;
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine());
        if (line.endsWith(QLatin1Char('\n'))) line.chop(1);
        if (line.startsWith(QLatin1String("#tandem-folders"))) { v2 = true; continue; }
        if (line.isEmpty()) continue;
        const auto parts = line.split(QLatin1Char('\t'));
        const int field = v2 ? 4 : 3;
        if (parts.size() <= field) return false; // keep a malformed/incomplete cloud file intact
        const QString path = parts.mid(field).join(QLatin1Char('\t'));
        bool countOk = false, timeOk = false;
        const double count = parts[0].toDouble(&countOk);
        const qint64 time = parts[1].toLongLong(&timeOk);
        if (!countOk || !timeOk) return false;
        out.entries.insert(path, {count, time});
        if (parts[2] == QLatin1String("1")) out.pinned.append(path);
        if (v2 && !parts[3].isEmpty()) out.keys.insert(path, parts[3]);
    }
    return file.error() == QFileDevice::NoError;
}

FolderHistory::Snapshot FolderHistory::mergeDiskLocked(const Snapshot& disk) const
{
    Snapshot merged = disk;
    QSet<QString> paths;
    for (const auto* entries : {&m_entries, &m_baseline.entries})
        for (const QString& p : entries->keys()) paths.insert(p);
    for (const auto* pins : {&m_pinned, &m_baseline.pinned})
        for (const QString& p : *pins) paths.insert(p);
    for (const auto* keys : {&m_keys, &m_baseline.keys})
        for (const QString& p : keys->keys()) paths.insert(p);
    for (const QString& p : paths) {
        const bool has = m_entries.contains(p), had = m_baseline.entries.contains(p);
        const Entry local = m_entries.value(p), base = m_baseline.entries.value(p);
        if (has != had || (has && local != base)) {
            if (!has) merged.entries.remove(p);
            else {
                Entry e = local;
                if (local.count >= base.count) e.count = merged.entries.value(p).count + local.count - base.count;
                e.last = qMax(e.last, merged.entries.value(p).last);
                merged.entries.insert(p, e);
            }
        }
        if (m_pinned.contains(p) != m_baseline.pinned.contains(p)) {
            merged.pinned.removeAll(p);
            if (m_pinned.contains(p)) merged.pinned.append(p);
        }
        if (m_keys.value(p) != m_baseline.keys.value(p)) {
            merged.keys.remove(p);
            const QString key = m_keys.value(p);
            if (!key.isEmpty()) {
                for (auto it = merged.keys.begin(); it != merged.keys.end();)
                    if (it.value() == key) it = merged.keys.erase(it); else ++it;
                merged.keys.insert(p, key);
            }
        }
    }
    if (m_pinned != m_baseline.pinned)
        for (const QString& path : m_pinned)
            if (merged.pinned.contains(path)) { merged.pinned.removeAll(path); merged.pinned.append(path); }
    return merged;
}

void FolderHistory::configurationChanged()
{
    // Pending saves look up the current path only after acquiring this mutex.
    setStoragePath({});
    loadAsync();
}

void FolderHistory::reloadAsync()
{
    if (!m_loaded.load()) { loadAsync(); return; }
    const QString path = storagePath();
    ioPool()->start([this, path] {
        QMutexLocker lock(&m_mutex);
        const QString current = m_storage.isEmpty() ? Config::instance().historyFile() : m_storage;
        if (path != current || !m_loaded.load()) return;
        Snapshot disk;
        if (!readSnapshot(path, disk)) return;
        const Snapshot merged = mergeDiskLocked(disk);
        m_entries = merged.entries; m_pinned = merged.pinned; m_keys = merged.keys;
        m_baseline = disk; // local changes remain pending until a successful save
    });
}

bool FolderHistory::saveNow()
{
    QMutexLocker lock(&m_mutex);
    if (!m_loaded.load() && m_storage.isEmpty()) return false;
    const QString path = m_storage.isEmpty() ? Config::instance().historyFile() : m_storage;
    // A missing cloud folder is not an empty profile. Do not recreate it and
    // overwrite later-arriving bookmarks with defaults.
    if (m_storage.isEmpty() && !QFileInfo::exists(Config::instance().fileName())) return false;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QLockFile fileLock(Config::localLockFile(path));
    if (!fileLock.tryLock(1000)) return false;
    Snapshot disk;
    if (QFileInfo::exists(path) && !readSnapshot(path, disk)) return false;
    const Snapshot merged = mergeDiskLocked(disk);
    if (QFileInfo::exists(path) && merged == disk) {
        m_entries = merged.entries; m_pinned = merged.pinned; m_keys = merged.keys; m_baseline = merged;
        return true;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    file.write("#tandem-folders 2\n");
    QSet<QString> written;
    auto line = [&](const QString& p) {
        const Entry e = merged.entries.value(p);
        file.write(QStringLiteral("%1\t%2\t%3\t%4\t%5\n").arg(e.count).arg(e.last)
                   .arg(merged.pinned.contains(p) ? 1 : 0).arg(merged.keys.value(p), p).toUtf8());
        written.insert(p);
    };
    for (const QString& p : merged.pinned) line(p);
    for (const QString& p : merged.entries.keys()) if (!written.contains(p)) line(p);
    if (!file.commit()) return false;
    m_entries = merged.entries; m_pinned = merged.pinned; m_keys = merged.keys;
    m_baseline = merged;
    return true;
}

void FolderHistory::scheduleSave()
{
    if (m_saveScheduled.exchange(true))
        return;
    ioPool()->start([this] {
        QThread::msleep(2000);  // batch a burst of visits into one write
        m_saveScheduled.store(false);
        if (m_loaded.load() && !saveNow()) {
            Config::instance().reportError(QObject::tr("Bookmarks could not be saved. They remain in memory and will be retried when the configuration folder is available."));
            scheduleSave();
        }
    });
}

void FolderHistory::visit(const QString& path)
{
    if (path.isEmpty())
        return;
    {
        QMutexLocker lock(&m_mutex);
        Entry& e = m_entries[path];
        e.count += 1;
        e.last = QDateTime::currentSecsSinceEpoch();
        double total = 0;
        for (const Entry& x : std::as_const(m_entries))
            total += x.count;
        if (total > kMaxTotal) {
            for (auto it = m_entries.begin(); it != m_entries.end();) {
                it->count *= 0.9;
                if (it->count < 1 && !m_pinned.contains(it.key()))
                    it = m_entries.erase(it);
                else
                    ++it;
            }
        }
    }
    scheduleSave();
}

void FolderHistory::forget(const QString& path)
{
    {
        QMutexLocker lock(&m_mutex);
        m_entries.remove(path);
        m_pinned.removeAll(path);
        m_keys.remove(path);
    }
    scheduleSave();
}

void FolderHistory::setPinned(const QString& path, bool pinned)
{
    {
        QMutexLocker lock(&m_mutex);
        m_pinned.removeAll(path);
        if (pinned)
            m_pinned.append(path);
        else
            m_keys.remove(path);
    }
    scheduleSave();
}

QString FolderHistory::pathForKeyLocked(const QString& key) const
{
    for (auto it = m_keys.cbegin(); it != m_keys.cend(); ++it)
        if (it.value() == key)
            return it.key();
    return {};
}

void FolderHistory::setBookmark(const QString& path, const QString& key)
{
    {
        QMutexLocker lock(&m_mutex);
        if (!key.isEmpty()) {
            const QString old = pathForKeyLocked(key);
            if (!old.isEmpty())
                m_keys.remove(old);
            m_keys.insert(path, key);
        } else {
            m_keys.remove(path);
        }
        if (!m_pinned.contains(path))
            m_pinned.append(path);
    }
    scheduleSave();
}

QString FolderHistory::bookmarkKey(const QString& path) const
{
    QMutexLocker lock(&m_mutex);
    return m_keys.value(path);
}

QString FolderHistory::pathForKey(const QString& key) const
{
    QMutexLocker lock(&m_mutex);
    return pathForKeyLocked(key);
}

QList<FolderHistory::Bookmark> FolderHistory::bookmarks() const
{
    QMutexLocker lock(&m_mutex);
    QList<Bookmark> out;
    for (auto it = m_keys.cbegin(); it != m_keys.cend(); ++it)
        out.append({it.value(), it.key()});
    std::sort(out.begin(), out.end(), [](const Bookmark& a, const Bookmark& b) { return a.key < b.key; });
    return out;
}

bool FolderHistory::isPinned(const QString& path) const
{
    QMutexLocker lock(&m_mutex);
    return m_pinned.contains(path);
}

QList<FolderHistory::Item> FolderHistory::items() const
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QMutexLocker lock(&m_mutex);
    QList<Item> out;
    for (const QString& p : m_pinned) {
        const Entry e = m_entries.value(p);
        out.append({p, frecency(e.count, e.last, now), true, m_keys.value(p)});
    }
    QList<Item> rest;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        if (!m_pinned.contains(it.key()))
            rest.append({it.key(), frecency(it->count, it->last, now), false, {}});
    std::sort(rest.begin(), rest.end(), [](const Item& a, const Item& b) { return a.frecency > b.frecency; });
    out.append(rest);
    return out;
}

}  // namespace tandem
