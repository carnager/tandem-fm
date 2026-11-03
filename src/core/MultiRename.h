#pragma once

#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace tandem {

// Multi-rename. Pure logic: build a plan, check it, then execute it
// (blocking; call from a worker).
//
// The template describes the whole new name; {field} parts are filled in:
//   {name} name without extension   {ext} extension with its dot (".jpg")
//   {file} the full old name        {folder} the parent folder's name
//   {n} counter: {n:01} pads to 2 digits, {n:100} starts at 100, {n:1+10} steps by 10
//   {date} modified, {date:yyyy-MM-dd HH.mm} in any format
//   {taken} when a photo was taken (EXIF; else modified), formats as {date}
// Parts of a field: {name[0:4]} first four characters, {name[-3:]} last
// three. Filters: {name|lower} upper title trim. {{ and }} are braces.
// Then find/replace runs on the result, then the case style.
struct RenameOptions {
    QString pattern = QStringLiteral("{name}{ext}");
    QString find;
    QString replace;
    bool regex = false;
    bool matchCase = false;
    enum class Case { Keep, Lower, Upper, Title, Sentence } caseMode = Case::Keep;
};

struct RenameItem {
    QString name;  // current file name
    bool isDir = false;
    int64_t mtime = 0;
    int64_t taken = 0;  // photo date (EXIF), 0 if unknown
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
