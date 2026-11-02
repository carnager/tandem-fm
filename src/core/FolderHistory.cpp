#include "core/FolderHistory.h"

#include "core/Util.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSaveFile>
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
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/folders.tsv");
}

void FolderHistory::setStoragePath(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    m_storage = path;
    m_entries.clear();
    m_pinned.clear();
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
    QFile f(storagePath());
    QHash<QString, Entry> entries;
    QStringList pinned;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).chopped(1);
            const QStringList parts = line.split(QLatin1Char('\t'));
            if (parts.size() < 4)
                continue;
            const QString path = parts.mid(3).join(QLatin1Char('\t'));
            entries.insert(path, {parts[0].toDouble(), parts[1].toLongLong()});
            if (parts[2] == QLatin1String("1"))
                pinned.append(path);
        }
    }
    QMutexLocker lock(&m_mutex);
    // Visits made before loading finished win.
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        Entry& e = m_entries[it.key()];
        e.count += it->count;
        e.last = std::max(e.last, it->last);
    }
    for (const QString& p : pinned)
        if (!m_pinned.contains(p))
            m_pinned.append(p);
    m_loaded.store(true);
}

void FolderHistory::saveNow()
{
    QHash<QString, Entry> entries;
    QStringList pinned;
    {
        QMutexLocker lock(&m_mutex);
        entries = m_entries;
        pinned = m_pinned;
    }
    const QString path = storagePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QSet<QString> written;
    auto line = [&](const QString& p, const Entry& e) {
        f.write(QStringLiteral("%1\t%2\t%3\t%4\n")
                    .arg(e.count)
                    .arg(e.last)
                    .arg(pinned.contains(p) ? 1 : 0)
                    .arg(p)
                    .toUtf8());
        written.insert(p);
    };
    for (const QString& p : std::as_const(pinned))
        line(p, entries.value(p));
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        if (!written.contains(it.key()))
            line(it.key(), *it);
    f.commit();
}

void FolderHistory::scheduleSave()
{
    if (m_saveScheduled.exchange(true))
        return;
    ioPool()->start([this] {
        QThread::msleep(2000);  // batch a burst of visits into one write
        m_saveScheduled.store(false);
        if (m_loaded.load())
            saveNow();
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
    }
    scheduleSave();
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
        out.append({p, frecency(e.count, e.last, now), true});
    }
    QList<Item> rest;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        if (!m_pinned.contains(it.key()))
            rest.append({it.key(), frecency(it->count, it->last, now), false});
    std::sort(rest.begin(), rest.end(), [](const Item& a, const Item& b) { return a.frecency > b.frecency; });
    out.append(rest);
    return out;
}

}  // namespace tandem
