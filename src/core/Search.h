#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

namespace tandem {

struct SearchSpec {
    QString root;          // folder searched (recursively)
    QString query;
    bool contents = false; // false: names (fd), true: file contents (rg)
    bool all = false;      // include hidden and .gitignore'd files
    bool operator==(const SearchSpec&) const = default;
};

// Blocking (worker). Runs fd / rg (falling back to a built-in name search or
// grep) and reports paths relative to spec.root in batches as they arrive.
// Stops after `limit` results or when cancelled. Returns an error text, or
// an empty string on success.
QString runSearch(const SearchSpec& spec, const std::atomic<bool>* cancel, int limit,
                  const std::function<void(const QStringList& batch)>& onBatch);

}  // namespace tandem
