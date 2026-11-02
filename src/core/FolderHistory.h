#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <atomic>

namespace tandem {

// Folders you visit, ranked by "frecency" (zoxide-style: visit count weighted
// by how recently), plus pinned folders (the hotlist). Thread-safe. Stored in
// $XDG_DATA_HOME/tandem-fm/folders.tsv; saving happens off the UI thread.
class FolderHistory {
public:
    struct Item {
        QString path;
        double frecency = 0;
        bool pinned = false;
    };

    static FolderHistory& instance();

    void loadAsync();
    bool isLoaded() const { return m_loaded.load(); }
    void saveNow();  // blocking, small file; used at exit

    void visit(const QString& path);
    void forget(const QString& path);
    void setPinned(const QString& path, bool pinned);
    bool isPinned(const QString& path) const;
    QList<Item> items() const;  // pinned first (in pin order), then by frecency

    static double frecency(double count, qint64 lastVisit, qint64 now);
    QString storagePath() const;
    void setStoragePath(const QString& path);  // tests

private:
    FolderHistory() = default;
    void load();
    void scheduleSave();
    struct Entry {
        double count = 0;
        qint64 last = 0;
    };

    mutable QMutex m_mutex;
    QHash<QString, Entry> m_entries;
    QStringList m_pinned;
    QString m_storage;
    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_saveScheduled{false};
};

}  // namespace tandem
