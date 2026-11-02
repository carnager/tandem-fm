#pragma once

#include "core/FileEntry.h"
#include "core/Sorting.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <optional>
#include <utility>
#include <vector>

namespace tandem {

struct ListOptions {
    bool showHidden = false;
    bool watch = false;  // register an inotify watch (done here, off the UI thread)
    SortSpec sort;
};

struct ListResult {
    QString path;
    std::vector<FileEntry> entries;  // sorted, without ".."
    int error = 0;                   // errno, 0 on success
    int64_t freeBytes = -1;
    int64_t totalBytes = -1;
    int watch = -1;
    int64_t elapsedMs = 0;
    SortSpec sortedBy;
};

// Blocking. Call from a worker thread only. Returns entries already sorted
// and with sort keys, so the UI thread only has to swap the vector in.
ListResult listDirectory(const QString& path, const ListOptions& opt,
                         const std::atomic<bool>* cancel = nullptr);

using EntryUpdate = std::pair<QString, std::optional<FileEntry>>;  // nullopt = gone

// Blocking. Re-stat the given names inside dir (used for watcher updates).
std::vector<EntryUpdate> statNames(const QString& dir, const QStringList& names, bool showHidden);

}  // namespace tandem
