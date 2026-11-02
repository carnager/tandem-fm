#pragma once

#include "core/Job.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <vector>

namespace tandem {

struct ArchiveEntry {
    QString path;  // inside the archive: no leading slash, '/' separated
    QString name;  // last path component
    int64_t size = 0;  // folders: total size of their contents
    int64_t mtime = 0;
    uint32_t mode = 0;  // type and permission bits
    bool isDir = false;
    bool isLink = false;
    bool encrypted = false;
};

// Table of contents of one archive, built once and shared (read-only).
struct ArchiveIndex {
    QString archivePath;
    int64_t archiveSize = 0;
    int64_t archiveMtime = 0;
    std::vector<ArchiveEntry> entries;
    QHash<QString, int> byPath;
    QHash<QString, QList<int>> children;  // folder path -> entry indices; "" is the root

    const ArchiveEntry* find(const QString& path) const;
};

// Archives Enter browses into (by name). Office documents, jars etc. are zip
// files too but open with their app; Ctrl+PgDn browses into anything.
bool isArchiveName(const QString& fileName);
// File name without its archive extension ("photos.tar.gz" -> "photos").
QString archiveStem(const QString& fileName);
// Turns an archive member name into a safe relative path; empty if unsafe.
QString normalizeArchivePath(QString path);

// Blocking (reads the archive): call from a worker. Cached by path, size and
// mtime, so browsing around inside one archive is instant after the first look.
std::shared_ptr<const ArchiveIndex> loadArchiveIndex(const QString& archivePath, QString* error,
                                                     const std::atomic<bool>* cancel = nullptr);

// Extracts one file into the cache folder (for viewing / opening). Blocking.
// Returns the local path, or an empty string with *error set.
QString extractToCache(const QString& archivePath, const QString& innerPath, QString* error,
                       const std::atomic<bool>* cancel = nullptr);
// Removes cached extractions older than a day. Blocking.
void pruneExtractCache();

// Extracts items (folders recursively) into destDir, keeping their names.
// innerPaths may contain "" for the whole archive; with smartFolder the
// whole-archive case goes into destDir/<archive name> unless the archive
// already has a single top-level folder.
class ExtractJob : public Job {
public:
    ExtractJob(QString archivePath, QStringList innerPaths, QString destDir, bool smartFolder = false);
    QString destination() const { return m_destDir; }

protected:
    void run() override;

private:
    QString m_archive;
    QStringList m_inner;
    QString m_destDir;
    bool m_smart;
};

// Alt+F5: creates an archive from local files. The format follows the
// extension (.zip, .7z, .tar, .tar.gz/.tgz, .tar.xz, .tar.zst, .tar.bz2).
class PackJob : public Job {
public:
    PackJob(QStringList sources, QString archivePath);

protected:
    void run() override;

private:
    QStringList m_sources;
    QString m_archive;
};

}  // namespace tandem
