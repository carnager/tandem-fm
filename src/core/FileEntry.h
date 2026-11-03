#pragma once

#include <QCollatorSortKey>
#include <QString>

#include <cstdint>
#include <optional>

namespace tandem {

// One row in a directory listing. Filled by the lister in a worker thread.
struct FileEntry {
    QString name;
    std::optional<QCollatorSortKey> key;  // locale-aware natural sort key
    int64_t size = -1;    // -1 = unknown (directories until computed)
    int64_t mtime = 0;    // seconds since epoch
    std::optional<int64_t> createdTime; // filesystem birth time; absent when unsupported
    std::optional<int64_t> addedTime;   // macOS date added to this folder
    uint32_t mode = 0;    // st_mode
    uint32_t uid = 0;
    uint32_t gid = 0;
    bool isDir = false;   // directory, or symlink pointing to one
    bool isLink = false;
    bool brokenLink = false;
    bool isParent = false;  // the ".." entry
    bool marked = false;
};

}  // namespace tandem
