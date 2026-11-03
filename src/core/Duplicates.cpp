#include "core/Duplicates.h"

#include "core/Util.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <map>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

namespace {

struct Found {
    QString path;
    int64_t size, mtime;
    dev_t dev;
    ino_t ino;
};

bool cancelled(const std::atomic<bool>* c)
{
    return c && c->load();
}

// A quick look: the first and last 64 KiB. Different here → different files.
QByteArray quickHash(const QString& path, int64_t size)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(f.read(64 * 1024));
    if (size > 128 * 1024 && f.seek(size - 64 * 1024))
        h.addData(f.read(64 * 1024));
    return h.result();
}

QByteArray fullHash(const QString& path, const std::atomic<bool>* cancel, int64_t* bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    for (;;) {
        if (cancelled(cancel))
            return {};
        const qint64 n = f.read(buf.data(), buf.size());
        if (n <= 0)
            break;
        h.addData(QByteArrayView(buf.constData(), n));
        *bytes += n;
    }
    return h.result();
}

}  // namespace

QList<DuplicateGroup> findDuplicates(const QStringList& roots, const DuplicateOptions& opt,
                                     const std::atomic<bool>* cancel,
                                     const std::function<void(int64_t, int64_t)>& progress)
{
    // 1. Every file, by size (one entry per inode: hard links are one file).
    std::map<int64_t, std::vector<Found>> bySize;
    QSet<QPair<quint64, quint64>> seenInodes;
    int64_t files = 0, hashed = 0;
    std::function<void(const QString&)> walk = [&](const QString& dir) {
        if (cancelled(cancel))
            return;
        QDir d(dir);
        QDir::Filters f = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
        if (opt.hidden)
            f |= QDir::Hidden;
        for (const QString& name : d.entryList(f)) {
            const QString path = joinPath(dir, name);
            struct stat st;
            if (::lstat(toNative(path).constData(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode)) {
                walk(path);
            } else if (S_ISREG(st.st_mode) && st.st_size >= qMax<int64_t>(1, opt.minSize)) {
                if (seenInodes.contains({quint64(st.st_dev), quint64(st.st_ino)}))
                    continue;
                seenInodes.insert({quint64(st.st_dev), quint64(st.st_ino)});
                bySize[st.st_size].push_back({path, st.st_size, st.st_mtime, st.st_dev, st.st_ino});
                if (++files % 500 == 0 && progress)
                    progress(files, hashed);
            }
        }
    };
    QStringList unique;
    for (const QString& r : roots)  // a root inside another would count twice
        if (std::none_of(unique.begin(), unique.end(),
                         [&](const QString& u) { return r == u || r.startsWith(u + QLatin1Char('/')); }))
            unique << r;
    for (const QString& r : std::as_const(unique))
        walk(r);

    // 2. Same size → same start and end → same hash.
    QList<DuplicateGroup> out;
    for (auto& [size, same] : bySize) {
        if (same.size() < 2 || cancelled(cancel))
            continue;
        QHash<QByteArray, std::vector<const Found*>> byQuick;
        for (const Found& f : same)
            if (const QByteArray q = quickHash(f.path, size); !q.isEmpty())
                byQuick[q].push_back(&f);
        for (const auto& candidates : std::as_const(byQuick)) {
            if (candidates.size() < 2)
                continue;
            QHash<QByteArray, QList<DuplicateFile>> byFull;
            for (const Found* f : candidates) {
                // Files up to 64 KiB were read whole by the quick look already.
                const QByteArray h = size <= 64 * 1024 ? quickHash(f->path, size) : fullHash(f->path, cancel, &hashed);
                if (!h.isEmpty())
                    byFull[h].append({f->path, f->mtime});
                if (progress)
                    progress(files, hashed);
            }
            for (auto& group : byFull) {
                if (group.size() < 2)
                    continue;
                std::sort(group.begin(), group.end(), [](const DuplicateFile& a, const DuplicateFile& b) { return a.path < b.path; });
                out.append({size, group});
            }
        }
    }
    // Biggest waste first.
    std::sort(out.begin(), out.end(), [](const DuplicateGroup& a, const DuplicateGroup& b) {
        return a.size * (a.files.size() - 1) > b.size * (b.files.size() - 1);
    });
    return out;
}

}  // namespace tandem
