#pragma once

#include "core/DirLister.h"
#include "core/FileEntry.h"
#include "core/Sorting.h"

#include <QAbstractTableModel>
#include <QRegularExpression>
#include <QSet>

#include <vector>

namespace tandem {

// Flat, sorted listing of one directory. Marks (TC-style selection) live in
// the entries, independent of the cursor (the view's current index).
class FileModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { ColName, ColExt, ColSize, ColTime, ColAttr, ColCount };

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

    QString path() const { return m_path; }
    const FileEntry* entry(int row) const;
    int rowOf(const QString& name) const;
    QString filePath(int row) const;
    int count() const { return int(m_entries.size()); }

    SortSpec sortSpec() const { return m_sort; }
    void setSortSpec(const SortSpec& spec);  // re-sorts

    void setMarked(int row, bool on);
    void toggleMarked(int row) { if (auto* e = entry(row)) setMarked(row, !e->marked); }
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

    QString m_path;
    std::vector<FileEntry> m_entries;  // ".." first when not at "/"
    std::vector<FileEntry> m_hidden;   // filtered out
    QString m_filter;
    QHash<QString, char> m_git;
    QList<QRegularExpression> m_filterTerms;
    SortSpec m_sort;
    Stats m_stats;
};

}  // namespace tandem
