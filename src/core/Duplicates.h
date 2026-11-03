#pragma once

#include <QList>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>

namespace tandem {

struct DuplicateFile {
    QString path;
    int64_t mtime = 0;
};

struct DuplicateGroup {
    int64_t size = 0;            // of each copy
    QList<DuplicateFile> files;  // two or more with the same contents
};

struct DuplicateOptions {
    int64_t minSize = 1;  // empty files are all "the same": left out
    bool hidden = false;
};

// Files with the same contents under the roots: grouped by size, then a
// quick look at the start and end, then a full hash. Hard links to one file
// count once. Symlinks are ignored. Blocking; progress gets (files looked
// at, bytes hashed).
QList<DuplicateGroup> findDuplicates(const QStringList& roots, const DuplicateOptions& options,
                                     const std::atomic<bool>* cancel = nullptr,
                                     const std::function<void(int64_t files, int64_t bytes)>& progress = {});

}  // namespace tandem
