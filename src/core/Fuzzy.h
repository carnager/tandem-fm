#pragma once

#include <QList>
#include <QString>
#include <QStringView>

namespace tandem {

// Fuzzy match for palettes and jump lists. Every space-separated term of
// `pattern` must appear in `text` as a case-insensitive subsequence, in
// order. Higher scores for matches at word starts and runs of consecutive
// characters. Returns -1 when there is no match; positions (optional) gets
// the matched character indices for highlighting.
int fuzzyScore(QStringView pattern, QStringView text, QList<int>* positions = nullptr);

}  // namespace tandem
