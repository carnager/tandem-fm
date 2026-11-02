#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace tandem {

struct CompareSide {
    bool exists = false;
    bool isDir = false;
    int64_t size = -1;  // folders: total size when on one side only, else -1
    int64_t mtime = 0;
};

enum class Diff {
    Equal,
    LeftNewer,
    RightNewer,
    LeftOnly,
    RightOnly,
    Differs,       // same time but different size or contents
    TypeMismatch,  // a file on one side, a folder on the other
};

struct CompareItem {
    QString rel;  // path relative to both roots
    CompareSide left, right;
    Diff diff = Diff::Equal;
};

struct CompareOptions {
    bool recursive = true;
    bool contents = false;  // equal sizes: compare bytes instead of trusting times
    bool hidden = true;
    int tolerance = 2;      // seconds (FAT stores times in 2 s steps)
};

struct CompareResult {
    std::vector<CompareItem> items;  // folder order, names sorted
    QString error;
};

// Blocking (worker). Folders on both sides are walked into (when recursive);
// a folder on one side only is one item. Symlinks are compared, not followed.
CompareResult compareFolders(const QString& left, const QString& right, const CompareOptions& options,
                             const std::atomic<bool>* cancel = nullptr,
                             const std::function<void(const QString& rel)>& progress = {});

// Blocking. nullopt when a file can't be read or the check was cancelled.
std::optional<bool> sameContents(const QString& a, const QString& b, const std::atomic<bool>* cancel = nullptr);

enum class SyncAction { None, ToRight, ToLeft, DeleteLeft, DeleteRight };
enum class SyncMode { TwoWay, MirrorToRight, MirrorToLeft };
SyncAction defaultAction(Diff diff, SyncMode mode);

struct SyncPlan {
    QList<QPair<QString, QString>> copies;  // (source, target) full paths
    QStringList deletions;
    int64_t bytes = 0;
};
SyncPlan buildSyncPlan(const QString& left, const QString& right, const std::vector<CompareItem>& items,
                       const std::vector<SyncAction>& actions);

}  // namespace tandem
