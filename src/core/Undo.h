#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace tandem {

// One undoable step. pairs are (path now, path before); for renames they
// are plain names inside `dir`.
struct UndoEntry {
    enum class Kind { Rename, Move, Copy, Trash, Create };
    Kind kind = Kind::Rename;
    QString description;
    QString dir;
    QList<QPair<QString, QString>> pairs;
};

// Blocking helpers (call from a worker). They return error messages.
// Puts trashed items back and removes their .trashinfo files.
QStringList restoreFromTrash(const QList<QPair<QString, QString>>& pairs);
// Removes new folders/files only while they're still empty.
QStringList removeIfStillEmpty(const QStringList& paths);

}  // namespace tandem
