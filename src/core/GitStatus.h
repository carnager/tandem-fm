#pragma once

#include <QHash>
#include <QString>

namespace tandem {

// Git state of one listed folder. Status codes per child name:
//   'U' conflict, 'M' modified/deleted/renamed, 'A' staged new,
//   '?' untracked, '!' ignored. Folders get the "strongest" code found inside.
struct GitInfo {
    bool inRepo = false;
    QString root;
    QString branch;
    QHash<QString, char> status;
};

// Blocking: walks up for .git, then runs `git status`. Call from a worker.
GitInfo gitStatusFor(const QString& dir);

// Parses `git status --porcelain=v1 -z --branch` output (exposed for tests).
GitInfo parseGitStatus(const QByteArray& output, const QString& root, const QString& dir);

}  // namespace tandem
