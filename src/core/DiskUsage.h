#pragma once

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace tandem {

// What a folder tree takes up, like ncdu: counted once, then browsed.
struct UsageNode {
    QString name;
    int64_t size = 0;   // on disk (what deleting would free); whole subtree for folders
    int64_t files = 0;  // files in the subtree (1 for a file)
    bool isDir = false;
    UsageNode* parent = nullptr;
    std::vector<std::unique_ptr<UsageNode>> children;  // biggest first
    QString path() const;  // full path (root's name is its path)
};

// Blocking. Stays on the root's drive (no /proc, no other mounts); hard
// links count once. Servers that can (SFTP) answer with one request.
std::unique_ptr<UsageNode> measureUsage(const QString& root, const std::atomic<bool>* cancel = nullptr,
                                        const std::function<void(int64_t files, int64_t bytes)>& progress = {});

// After something inside was deleted: take it out, fix the sizes above.
void removeUsageNode(UsageNode* node);

}  // namespace tandem
