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
// folders.tsv beside the selected INI file; saving happens off the UI thread.
class FolderHistory {
public:
    struct Item {
        QString path;
        double frecency = 0;
        bool pinned = false;
        QString key;  // bookmark key chain ("h", "cp"), may be empty for a plain pin
    };
    struct Bookmark {
        QString key;
        QString path;
    };

    static FolderHistory& instance();

    void loadAsync();
    void reloadAsync(); // merge external changes without losing local edits
    void configurationChanged();
    bool isLoaded() const { return m_loaded.load(); }
    bool saveNow();  // blocking, small file; used at exit

    void visit(const QString& path);
    void forget(const QString& path);
    void setPinned(const QString& path, bool pinned);
    bool isPinned(const QString& path) const;
    // Bookmarks are pins with a key. Keys are unique: assigning a used key
    // moves it. An empty key keeps (or makes) a plain pin.
    void setBookmark(const QString& path, const QString& key);
    QString bookmarkKey(const QString& path) const;
    QString pathForKey(const QString& key) const;
    QList<Bookmark> bookmarks() const;  // with keys, sorted by key
    QList<Item> items() const;  // pinned first (in pin order), then by frecency

    static double frecency(double count, qint64 lastVisit, qint64 now);
    QString storagePath() const;
    void setStoragePath(const QString& path);  // tests

private:
    FolderHistory() = default;
    void load();
    void scheduleSave();
    QString pathForKeyLocked(const QString& key) const;
    struct Entry {
        double count = 0;
        qint64 last = 0;
        bool operator==(const Entry&) const = default;
    };
    struct Snapshot {
        QHash<QString, Entry> entries;
        QStringList pinned;
        QHash<QString, QString> keys;
        bool operator==(const Snapshot&) const = default;
    };
    static bool readSnapshot(const QString& path, Snapshot& out);
    Snapshot mergeDiskLocked(const Snapshot& disk) const;
    Snapshot m_baseline;

    mutable QMutex m_mutex;
    QHash<QString, Entry> m_entries;
    QStringList m_pinned;
    QHash<QString, QString> m_keys;  // path -> key
    QString m_storage;
    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_saveScheduled{false};
};

}  // namespace tandem
