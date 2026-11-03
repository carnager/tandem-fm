#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>

namespace tandem {

// The freedesktop trash: the home trash and the per-drive ones
// (.Trash/<uid>, .Trash-<uid>), as other apps use them too. Blocking.
struct TrashItem {
    QString trashRoot;     // .../Trash
    QString name;          // under files/ (and info/<name>.trashinfo)
    QString originalPath;  // where it came from; empty if unknown
    int64_t deletedAt = 0; // seconds since the epoch; 0 if unknown
    int64_t size = 0;      // folders: everything in them
    bool isDir = false;
    QString filePath() const { return trashRoot + QStringLiteral("/files/") + name; }
    QString infoPath() const { return trashRoot + QStringLiteral("/info/") + name + QStringLiteral(".trashinfo"); }
};

QStringList trashRoots();
QList<TrashItem> listTrash(const std::atomic<bool>* cancel = nullptr);
// Back where they came from (folders made as needed). An item whose place
// is taken again gets " (restored)" added to its name. Returns errors.
QStringList restoreTrashItems(const QList<TrashItem>& items);
QStringList deleteTrashItems(const QList<TrashItem>& items);

}  // namespace tandem
