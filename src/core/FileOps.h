#pragma once

#include "core/Job.h"

#include <QByteArray>
#include <QStringList>

#include <atomic>
#include <optional>
#include <vector>

struct stat;

namespace tandem {

class Backend;
struct FileStat;

// F5 / F6. Destination semantics follow Total Commander:
//  - existing directory          -> items are placed inside it
//  - single source, other path   -> copied/moved *as* that path (rename)
//  - several sources, new path   -> directory is created, items go inside
class CopyJob : public Job {
public:
    enum class Mode { Copy, Move };
    CopyJob(Mode mode, QStringList sources, QString destination);
    // Explicit (source, target path) pairs, e.g. from a folder sync. Missing
    // parent folders of targets are created.
    explicit CopyJob(QList<QPair<QString, QString>> pairs);

    Mode mode() const { return m_mode; }
    QString destination() const { return m_destination; }

protected:
    void run() override;

private:
    bool processItem(const QByteArray& src, const QByteArray& dst);
    bool copyDirectory(const QByteArray& src, QByteArray dst, const struct stat& st);
    bool copySymlink(const QByteArray& src, QByteArray dst, const struct stat& st);
    bool copyRegular(const QByteArray& src, QByteArray dst, const struct stat& st);
    int copyData(int in, int out, int64_t size);  // 0, errno, or -1 when cancelled
    std::optional<Resolution> resolveConflict(const QByteArray& src, const struct stat& srcSt, QByteArray& dst,
                                              const struct stat& dstSt);
    void scan(const QByteArray& path, int64_t& files, int64_t& bytes);

    // Remote locations (sftp://, …): the same job, done through Backends
    // with plain read/write streams. Implemented in BackendCopy.cpp.
    bool needsBackends() const;
    void runWithBackends();
    bool backendItem(const QString& src, QString dst);
    bool backendFile(Backend* sb, const QString& src, Backend* db, QString dst, const FileStat& st);
    void backendScan(const QString& path, int64_t& files, int64_t& bytes);
    static ConflictSide sideOf(const QString& path, const FileStat& st);

    Mode m_mode;
    QStringList m_sources;
    QString m_destination;
    QList<QPair<QString, QString>> m_pairs;
    bool m_crossDevice = false;
    std::vector<char> m_buffer;
};

// F8: to trash (default) or permanently (Shift+F8).
class DeleteJob : public Job {
public:
    DeleteJob(QStringList paths, bool toTrash);

protected:
    void run() override;

private:
    bool removeTree(const QByteArray& path);
    bool removeTreeAt(const QString& path);  // remote (BackendCopy.cpp)
    void countAt(const QString& path, int64_t& files);
    void count(const QByteArray& path, int64_t& files);

    QStringList m_paths;
    bool m_toTrash;
    bool m_permanentAll = false;
};

// Small blocking helpers for the I/O pool. Return 0 or an errno value.
int makeDirectories(const QString& path);
int renameNoReplace(const QString& from, const QString& to);

// A link at linkPath to target. Relative symlinks point "../x/y" so they
// keep working when both move together; hard links need the same drive.
enum class LinkKind { Symbolic, Relative, Hard };
int makeLink(const QString& target, const QString& linkPath, LinkKind kind);
// Recursive size of regular files below path. -1 if cancelled or unreadable.
int64_t directorySize(const QString& path, const std::atomic<bool>* cancel);
// "name (2).ext" style name that does not exist yet.
QByteArray uniqueName(const QByteArray& path);

}  // namespace tandem
