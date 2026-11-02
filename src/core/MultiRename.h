#pragma once

#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace tandem {

// Total Commander style multi-rename. Pure logic: build a plan, check it,
// then execute it (blocking; call from a worker).
//
// Mask placeholders (name and extension masks):
//   [N] name without extension     [E] extension      [P] parent folder name
//   [N3] 3rd character             [N2-5] chars 2..5  [N2,3] 3 chars from 2nd
//   [N-3-] last three chars        [N2-] from 2nd on  (same ranges work for [E])
//   [C] counter (start/step/digits from the options); [C10+5:3] inline override
//   [Y] [M] [D] [h] [m] [s] parts of the modification time, [YMD] [hms] combined
//   [[] and []] for literal brackets
struct RenameOptions {
    QString nameMask = QStringLiteral("[N]");
    QString extMask = QStringLiteral("[E]");
    QString search;
    QString replace;
    bool regex = false;
    bool caseSensitive = false;
    enum class Case { Keep, Lower, Upper, FirstUpper, Title } caseMode = Case::Keep;
    int counterStart = 1;
    int counterStep = 1;
    int counterDigits = 1;
};

struct RenameItem {
    QString name;  // current file name
    bool isDir = false;
    int64_t mtime = 0;
};

struct RenameRow {
    QString oldName;
    QString newName;
    enum class Status { Unchanged, Ok, Conflict, Invalid } status = Status::Unchanged;
    QString note;  // why it can't be renamed
};

struct RenamePlan {
    QList<RenameRow> rows;
    QString error;  // mask syntax error; empty when fine
    int changes = 0;
    int problems = 0;
    bool canRun() const { return error.isEmpty() && problems == 0 && changes > 0; }
};

// existing: all names currently in the folder (to detect collisions with
// files that are not part of the rename).
RenamePlan buildRenamePlan(const QList<RenameItem>& items, const QString& parentFolderName,
                           const RenameOptions& options, const QSet<QString>& existing);

// Plan from names typed elsewhere (e.g. in a text editor), one per item.
RenamePlan buildRenamePlanFromNames(const QList<RenameItem>& items, const QStringList& newNames,
                                    const QSet<QString>& existing);

// Renames in dir. Chains and swaps (a->b, b->a) go through temporary names.
// Returns the (new, old) pairs that were actually done, for undo.
QList<QPair<QString, QString>> executeRenamePlan(const QString& dir, const RenamePlan& plan, QStringList* errors);

}  // namespace tandem
