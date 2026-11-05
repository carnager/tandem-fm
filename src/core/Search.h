#pragma once

#include <QList>
#include <QRegularExpression>
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

// Lists every file and folder below root (no hidden or .gitignore'd ones
// unless `all`), in batches, like runSearch.
QString listTree(const QString& root, bool all, const std::atomic<bool>* cancel, int limit,
                 const std::function<void(const QStringList& batch)>& onBatch);

// The live filter's matching, on paths relative to the searched folder:
// every space-separated word must be in the path ("ph 24 raw" finds
// Photos/2024/x.raw); one with * or ? is a pattern for the name itself.
// Ignores case unless a word has capitals.
class PathMatcher {
public:
    explicit PathMatcher(const QString& query);
    bool isEmpty() const { return m_words.isEmpty() && m_patterns.isEmpty(); }
    bool matches(const QString& path) const;

private:
    QStringList m_words;
    QList<Qt::CaseSensitivity> m_wordCase;
    QList<QRegularExpression> m_patterns;
};

}  // namespace tandem
