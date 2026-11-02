#pragma once

#include "core/FileEntry.h"

#include <QStringView>

#include <vector>

namespace tandem {

enum class SortColumn : uint8_t { Name = 0, Ext = 1, Size = 2, Time = 3, Attr = 4 };

struct SortSpec {
    SortColumn column = SortColumn::Name;
    bool descending = false;
    bool operator==(const SortSpec&) const = default;
};

// Locale-aware, case-insensitive, numeric-aware ("file2" < "file10") key.
// Uses a per-thread collator, so it is safe to call from workers.
QCollatorSortKey makeSortKey(const QString& name);

// Extension without the dot; empty for dirs-by-convention and dotfiles like ".bashrc".
QStringView extensionOf(const QString& name);

// Directories (and "..") always come first; ".." is always the very first row.
bool entryLess(const FileEntry& a, const FileEntry& b, const SortSpec& spec);
void sortEntries(std::vector<FileEntry>& entries, const SortSpec& spec);

}  // namespace tandem
