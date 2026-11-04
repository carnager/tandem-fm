// Copy, move and delete when a remote location is involved. Same job classes
// and prompts as for local files, but every access goes through a Backend
// (the local side through localBackend()), with plain read/write streams.

#include "core/Backend.h"
#include "core/FileOps.h"
#include "core/Util.h"

#include <QRandomGenerator>

#include <sys/stat.h>

namespace tandem {

namespace {

bool sameServer(const QString& a, const QString& b)
{
    return locationAuthority(a) == locationAuthority(b);  // "" == "" for two local paths
}

}  // namespace

void CopyJob::runWithBackends()
{
    // Destination: a folder to copy into, or (one source) the new name.
    QList<QPair<QString, QString>> items = m_pairs;
    if (m_pairs.isEmpty()) {
        Backend* db = anyBackendFor(m_destination);
        if (!db) {
            addError(tr("Tandem can't open %1").arg(m_destination));
            setState(State::Failed);
            return;
        }
        FileStat ds;
        bool destIsDir = db->stat(m_destination, ds) == 0 && ds.isDir;
        if (!destIsDir && m_sources.size() > 1) {
            for (;;) {
                const int e = makeDirectoriesAt(m_destination);
                if (e == 0 || e == EEXIST)
                    break;
                if (askError(tr("Cannot create folder"), m_destination, e) != Answer::Retry) {
                    setState(State::Failed);
                    return;
                }
            }
            destIsDir = true;
        } else if (!destIsDir) {
            makeDirectoriesAt(parentPath(m_destination));
        }
        for (const QString& s : std::as_const(m_sources)) {
            const QString dst = destIsDir ? joinPath(m_destination, baseName(s)) : m_destination;
            if (sameServer(s, dst) && (dst == s || dst.startsWith(s + QLatin1Char('/')))) {
                addError(tr("Cannot copy a folder into itself: %1").arg(s));
                continue;
            }
            items.append({s, dst});
        }
    } else {
        for (const auto& [from, to] : std::as_const(m_pairs)) {
            const QString parent = parentPath(to);
            for (int e; (e = makeDirectoriesAt(parent)) != 0 && e != EEXIST;) {
                if (askError(tr("Cannot create folder"), parent, e) != Answer::Retry) {
                    setState(State::Failed);
                    return;
                }
            }
        }
    }

    // Moving within one server: a rename, no data transfer.
    if (m_mode == Mode::Move) {
        QList<QPair<QString, QString>> rest;
        for (const auto& it : std::as_const(items)) {
            if (checkpoint())
                return;
            if (sameServer(it.first, it.second) && renameNoReplaceAt(it.first, it.second) == 0) {
                m_filesDone += 1;
                if (!isRemoteLocation(it.second))
                    recordUndo(it.second, it.first);
                continue;
            }
            rest.append(it);
        }
        items = rest;
    }
    if (items.isEmpty())
        return;

    {
        QStringList involved;
        for (const auto& it : std::as_const(items))
            involved << it.first << parentPath(it.second);
        if (!waitForTurn(involved))
            return;
    }
    int64_t files = 0, bytes = 0;
    for (const auto& it : std::as_const(items)) {
        backendScan(it.first, files, bytes);
        if (isCancelled())
            return;
    }
    m_filesTotal = m_filesDone + files;
    m_bytesTotal = bytes;
    setState(State::Running);

    for (const auto& it : std::as_const(items)) {
        // Undo only covers local things that didn't exist before.
        FileStat before;
        const bool localTarget = !isRemoteLocation(it.second);
        const bool existed = localTarget && statAt(it.second, before) == 0;
        forgetRenames();
        const bool ok = backendItem(it.first, it.second);
        const QString made = fromNative(renamedTarget(toNative(it.second)));
        FileStat after;
        if (localTarget && (made != it.second || !existed) && statAt(made, after) == 0) {
            if (m_mode == Mode::Copy)
                recordUndo(made);
            else if (ok && !isRemoteLocation(it.first))
                recordUndo(made, it.first);
        }
        if (isCancelled())
            return;
    }
    m_bytesDone = m_bytesTotal.load();
    m_filesDone = m_filesTotal.load();
}

void CopyJob::backendScan(const QString& path, int64_t& files, int64_t& bytes)
{
    if (isCancelled())
        return;
    Backend* b = anyBackendFor(path);
    FileStat st;
    if (!b || b->stat(path, st) != 0)
        return;
    files += 1;
    if (st.isDir && !st.isLink) {
        ListOptions opt;
        opt.showHidden = true;
        const ListResult l = b->list(path, opt, nullptr);
        for (const FileEntry& e : l.entries)
            backendScan(joinPath(path, e.name), files, bytes);
    } else if (!st.isLink) {
        bytes += st.size;
    }
}

Job::ConflictSide CopyJob::sideOf(const QString& path, const FileStat& st)
{
    return ConflictSide{path, st.isDir ? -1 : st.size, st.mtime};
}

bool CopyJob::backendItem(const QString& src, QString dst)
{
    if (checkpoint())
        return false;
    Backend* sb = anyBackendFor(src);
    Backend* db = anyBackendFor(dst);
    if (!sb || !db) {
        addError(tr("Tandem can't open %1").arg(sb ? dst : src));
        return false;
    }
    FileStat st;
    for (int e; (e = sb->stat(src, st)) != 0;)
        if (askError(tr("Cannot read"), src, e) != Answer::Retry)
            return false;
    setCurrent(src);

    if (st.isLink) {
        QString target;
        int e = sb->readLink(src, target);
        FileStat existing;
        if (e == 0 && db->stat(dst, existing) == 0) {
            QByteArray renamed = toNative(dst);
            const auto r = Job::resolveConflict(sideOf(src, st), renamed, sideOf(dst, existing));
            if (!r || *r == Resolution::Skip) {
                m_filesDone += 1;
                return false;
            }
            if (*r == Resolution::Rename)
                dst = fromNative(renamed);
            else
                e = existing.isDir ? EISDIR : db->removeFile(dst);
        }
        if (e == 0)
            e = db->symlink(target, dst);
        m_filesDone += 1;
        if (e != 0) {
            // Servers often can't make links: say so, don't stop the job.
            addError(tr("Skipped link %1 (%2)").arg(src, errnoString(e)));
            return false;
        }
        if (m_mode == Mode::Move)
            sb->removeFile(src);
        return true;
    }

    if (st.isDir) {
        FileStat existing;
        bool exists = db->stat(dst, existing) == 0;
        if (exists && !existing.isDir) {
            QByteArray target = toNative(dst);
            const auto r = Job::resolveConflict(sideOf(src, st), target, sideOf(dst, existing));
            if (!r || *r == Resolution::Skip)
                return false;
            if (*r == Resolution::Rename) {
                dst = fromNative(target);
            } else if (const int e = db->removeFile(dst)) {
                askError(tr("Cannot replace"), dst, e);
                return false;
            }
            exists = false;
        }
        if (!exists) {
            for (int e; (e = db->mkdir(dst, 0700)) != 0;)
                if (askError(tr("Cannot create folder"), dst, e) != Answer::Retry)
                    return false;
        }
        ListOptions opt;
        opt.showHidden = true;
        ListResult l;
        for (;;) {
            l = sb->list(src, opt, nullptr);
            if (!l.error)
                break;
            if (askError(tr("Cannot read folder"), src, l.error) != Answer::Retry)
                return false;
        }
        bool ok = true;
        for (const FileEntry& e : l.entries) {
            if (!backendItem(joinPath(src, e.name), joinPath(dst, e.name)))
                ok = false;
            if (isCancelled())
                return false;
        }
        db->chmod(dst, st.mode);
        m_filesDone += 1;
        if (ok && m_mode == Mode::Move) {
            if (const int e = sb->removeDir(src)) {
                addError(tr("Cannot remove source folder: %1 (%2)").arg(src, errnoString(e)));
                ok = false;
            }
        }
        return ok;
    }

    if (!S_ISREG(st.mode) && st.mode != 0) {
        addError(tr("Skipped special file: %1").arg(src));
        m_filesDone += 1;
        return false;
    }
    return backendFile(sb, src, db, dst, st);
}

bool CopyJob::backendFile(Backend* sb, const QString& src, Backend* db, QString dst, const FileStat& st)
{
    // Target exists: the usual question (overwrite / skip / rename / all).
    bool replacing = false;
    FileStat existing;
    if (db->stat(dst, existing) == 0) {
        QByteArray target = toNative(dst);
        const bool same = sameServer(src, dst) && src == dst;
        const auto r = Job::resolveConflict(sideOf(src, st), target, sideOf(dst, existing), same);
        if (!r || *r == Resolution::Skip) {
            m_filesDone += 1;
            m_bytesDone += st.size;
            return false;
        }
        if (*r == Resolution::Rename) {
            dst = fromNative(target);
        } else if (existing.isDir) {
            addError(tr("Cannot replace a folder with a file: %1").arg(dst));
            return false;
        } else {
            replacing = true;
        }
    }

    int err = 0;
    std::unique_ptr<Reader> in;
    while (!(in = sb->openRead(src, err)))
        if (askError(tr("Cannot open"), src, err) != Answer::Retry)
            return false;
    // Replacing: write a hidden file beside the old one and swap them at the
    // end, so a dropped connection or a cancel leaves the old file as it was.
    QString target = dst;
    std::unique_ptr<Writer> out;
    if (replacing) {
        const int slash = dst.lastIndexOf(QLatin1Char('/'));
        for (int attempt = 0; attempt < 3 && !out; ++attempt) {
            target = dst.left(slash + 1) + QLatin1Char('.') + dst.mid(slash + 1) + QStringLiteral(".tandem-") +
                     QString::number(QRandomGenerator::global()->generate() % 0x1000000, 36);
            out = db->openWrite(target, true, st.mode & 07777, err);
            if (!out && err != EEXIST)
                break;
        }
        if (!out)
            target = dst;  // no files beside it: overwrite in place, as before
    }
    while (!out && !(out = db->openWrite(target, false, st.mode & 07777, err)))
        if (askError(tr("Cannot create"), target, err) != Answer::Retry)
            return false;

    const int64_t before = m_bytesDone.load();
    if (m_buffer.size() < (4u << 20))
        m_buffer.resize(4u << 20);
    auto fail = [&](const QString& what, const QString& path, int e) {
        out.reset();
        db->removeFile(target);  // no half-written files left behind; a replaced file stays
        m_bytesDone = before + st.size;
        m_filesDone += 1;
        if (e != ECANCELED)
            askError(what, path, e);
        return false;
    };
    for (;;) {
        if (checkpoint())
            return fail(QString(), QString(), ECANCELED);
        const int64_t n = in->read(m_buffer.data(), int64_t(m_buffer.size()));
        if (n < 0)
            return fail(tr("Cannot read"), src, int(-n));
        if (n == 0)
            break;
        if (const int64_t w = out->write(m_buffer.data(), n); w < 0)
            return fail(tr("Cannot write"), dst, int(-w));
        m_bytesDone += n;
    }
    in.reset();
    if (const int e = out->finish(st.mtime))
        return fail(tr("Cannot write"), dst, e);
    out.reset();
    if (target != dst) {
        if (const int e = db->replace(target, dst)) {
            addError(tr("Cannot replace %1 (%2); the copy is in %3").arg(dst, errnoString(e), target));
            m_filesDone += 1;
            return false;
        }
    }
    m_filesDone += 1;
    if (m_mode == Mode::Move) {
        if (const int e = sb->removeFile(src)) {
            addError(tr("Cannot remove source: %1 (%2)").arg(src, errnoString(e)));
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- delete

bool DeleteJob::removeTreeAt(const QString& path)
{
    if (checkpoint())
        return false;
    Backend* b = backendFor(path);
    FileStat st;
    if (!b)
        return false;
    if (const int e = b->stat(path, st))
        return e == ENOENT;
    bool ok = true;
    if (st.isDir && !st.isLink) {
        ListOptions opt;
        opt.showHidden = true;
        ListResult l;
        for (;;) {
            l = b->list(path, opt, nullptr);
            if (!l.error)
                break;
            if (askError(tr("Cannot read folder"), path, l.error) != Answer::Retry)
                return false;
        }
        for (const FileEntry& e : l.entries) {
            if (!removeTreeAt(joinPath(path, e.name)))
                ok = false;
            if (isCancelled())
                return false;
        }
        if (ok) {
            for (int e; (e = b->removeDir(path)) != 0;) {
                if (askError(tr("Cannot delete folder"), path, e) != Answer::Retry) {
                    ok = false;
                    break;
                }
            }
        }
    } else {
        setCurrent(path);
        for (int e; (e = b->removeFile(path)) != 0;) {
            if (askError(tr("Cannot delete"), path, e) != Answer::Retry) {
                ok = false;
                break;
            }
        }
    }
    m_filesDone += 1;
    return ok;
}

void DeleteJob::countAt(const QString& path, int64_t& files)
{
    Backend* b = backendFor(path);
    FileStat st;
    if (!b || b->stat(path, st) != 0)
        return;
    files += 1;
    if (st.isDir && !st.isLink) {
        ListOptions opt;
        opt.showHidden = true;
        for (const FileEntry& e : b->list(path, opt, nullptr).entries)
            countAt(joinPath(path, e.name), files);
    }
}

}  // namespace tandem
