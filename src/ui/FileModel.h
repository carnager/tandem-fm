#pragma once

#include <optional>

#include <QDateTime>

#include "core/DirLister.h"
#include "core/FileEntry.h"
#include "core/Sorting.h"

#include <QAbstractTableModel>
#include <QRegularExpression>
#include <QIcon>
#include <QSet>

#include <vector>

namespace tandem {

// Flat, sorted listing of one directory. Marks (TC-style selection) live in
// the entries, independent of the cursor (the view's current index).
class FileModel : public QAbstractTableModel {
    Q_OBJECT
public:
    // In the order of SortColumn. New ones go at the end, hidden at first.
    enum Column {
        ColName, ColExt, ColSize, ColTime, ColAttr,
        ColOwner, ColGroup, ColOctal, ColCreated, ColAccessed, ColLinkTarget,
        ColCount
    };
    // Stable names for saving the column layout ("size", "owner"); -1 if unknown.
    static QString columnId(int column);
    static int columnOf(const QString& id);

    // What the owner and group columns can show: names from this system's
    // user list (local folders), the plain ids (servers), or nothing (archives).
    enum class Owners { Names, Numbers, None };

    struct Stats {
        int files = 0, dirs = 0;
        int64_t bytes = 0;
        int markedFiles = 0, markedDirs = 0;
        int64_t markedBytes = 0;
    };

    explicit FileModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    void sort(int column, Qt::SortOrder order) override;

    void setListing(const QString& path, std::vector<FileEntry>&& entries);
    void applyUpdates(std::vector<EntryUpdate>&& updates);
    // Search results arriving: appended as they come (sorted at the end).
    void appendEntries(std::vector<FileEntry>&& entries);
    void setDirSize(const QString& name, int64_t size);

    // Folders show how many items they hold, counted on a worker for the
    // rows that are painted; only where that is cheap (local folders).
    void setCountable(bool on, bool hidden);
#ifdef Q_OS_MACOS
    void setLocalIconPaths(bool on) { m_localIconPaths = on; }
#endif
    std::optional<int> itemCount(const FileEntry& e) const;  // asks for it when unknown
    void setOwners(Owners owners) { m_owners = owners; }

    // Dates as "12 min ago", "Yesterday 14:20", "3 Nov"; or exact.
    static QString relativeDate(qint64 secs, const QDateTime& now);
    static void setRelativeDates(bool on);
    static bool relativeDates();

    QString path() const { return m_path; }
    const FileEntry* entry(int row) const;
    int rowOf(const QString& name) const;
    QString filePath(int row) const;
    QIcon fileIcon(int row) const;
    int count() const { return int(m_entries.size()); }

    SortSpec sortSpec() const { return m_sort; }
    void setSortSpec(const SortSpec& spec);  // re-sorts

    void setMarked(int row, bool on);
    void toggleMarked(int row) { if (auto* e = entry(row)) setMarked(row, !e->marked); }
    void setMarkedRange(int first, int last, bool on);  // both included, either order
    void markAll(bool on);
    void invertMarks();
    int markByPattern(const QString& patterns, bool on);
    // Marks exactly these names (everything else unmarked).
    void setMarkedNames(const QSet<QString>& names);
    QStringList markedNames() const;
    QSet<QString> markedSet() const;
    void restoreMarks(const QSet<QString>& names);
    Stats stats() const { return m_stats; }

    // Filter as you type: entries not matching are set aside (they keep
    // their marks and still get live updates). Space-separated terms must
    // all be in the name; * and ? work as wildcards.
    void setFilter(const QString& text);
    QString filter() const { return m_filter; }
    int hiddenByFilter() const { return int(m_hidden.size()); }
    const std::vector<FileEntry>& filteredOut() const { return m_hidden; }

    void setGitStatus(const QHash<QString, char>& status);
    char gitStatus(const QString& name) const { return m_git.value(name, 0); }

    static QString displayName(const FileEntry& e);
    static QString displayExt(const FileEntry& e);

signals:
    void renameRequested(const QString& from, const QString& to);
    void statsChanged();

private:
    void account(const FileEntry& e, int sign);
    void accountMark(const FileEntry& e, int sign);
    int insertPosition(const FileEntry& e) const;
    void insertEntry(FileEntry&& e);
    void removeRow(int row);

    bool matchesFilter(const FileEntry& e) const;
    void partition(std::vector<FileEntry>& all);

    void runCounts() const;
    QString ownerText(uint32_t id, bool group) const;  // asks for the name when unknown
    void runNames() const;

    QString m_path;
    std::vector<FileEntry> m_entries;  // ".." first when not at "/"
    std::vector<FileEntry> m_hidden;   // filtered out
    QString m_filter;
    QHash<QString, char> m_git;
    QList<QRegularExpression> m_filterTerms;
    SortSpec m_sort;
    Stats m_stats;
    bool m_countable = false, m_countHidden = false;
    quint64 m_countGen = 0;
    mutable QHash<QString, int> m_counts;
    mutable QSet<QString> m_countQueue;
    mutable bool m_countScheduled = false;
    Owners m_owners = Owners::None;
    mutable QSet<uint32_t> m_userQueue, m_groupQueue;
    mutable bool m_namesScheduled = false;
#ifdef Q_OS_MACOS
    bool m_localIconPaths = false;
#endif
};

}  // namespace tandem
