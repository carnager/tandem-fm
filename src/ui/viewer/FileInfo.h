#pragma once

#include <QList>
#include <QString>

#include <atomic>

namespace tandem {

// Details for the Info view (yazi's "spot"). Rows are grouped in sections;
// a row with an empty key starts a new section titled by its value.
struct FileInfoRow {
    QString key;
    QString value;
};

struct FileInfo {
    QList<FileInfoRow> rows;
    bool isDir = false;
    bool isRegular = false;
    QString error;
};

// Blocking (worker): everything cheap to find out. Folder totals and
// checksums are separate because they can take long.
FileInfo gatherFileInfo(const QString& path, char gitStatus);
// Blocking: SHA-256 of a regular file; empty if cancelled or unreadable.
QString sha256Of(const QString& path, const std::atomic<bool>* cancel);

}  // namespace tandem
