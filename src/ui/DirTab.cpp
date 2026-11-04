#include "ui/DirTab.h"

#include "core/Backend.h"
#include "core/Mounts.h"
#include "core/Remote.h"

#include "core/DirWatcher.h"
#include "core/FileOps.h"
#include "core/FolderHistory.h"
#include "core/GitStatus.h"
#include "core/Util.h"
#include "ui/FileModel.h"

#include <QDir>
#include <QPointer>
#include <QTimer>

#include <sys/stat.h>

namespace tandem {

namespace {
constexpr int kMaxIncrementalUpdates = 400;  // above this a re-list is cheaper
}

namespace {
bool g_gitEnabled = true;
}

void DirTab::setGitEnabled(bool on)
{
    g_gitEnabled = on;
}

DirTab::DirTab(QObject* parent) : QObject(parent), m_model(new FileModel(this))
{
    m_gitTimer = new QTimer(this);
    m_gitTimer->setSingleShot(true);
    m_gitTimer->setInterval(600);  // changes come in bursts; one git run per burst
    connect(m_gitTimer, &QTimer::timeout, this, &DirTab::refreshGit);
    auto* w = DirWatcher::instance();
    connect(w, &DirWatcher::changed, this, &DirTab::onWatchChanged);
    connect(w, &DirWatcher::gone, this, &DirTab::onDirectoryGone);
    connect(w, &DirWatcher::overflow, this, &DirTab::refresh);
}

DirTab::~DirTab()
{
    if (m_cancel)
        m_cancel->store(true);
    if (m_sizeCancel)
        m_sizeCancel->store(true);
    setWatch(-1);
}

QString DirTab::path() const
{
    return m_model->path().isEmpty() ? m_loadingPath : m_model->path();
}

QString DirTab::title() const
{
    if (m_inSearch)
        return QStringLiteral("⌕ %1").arg(m_search.query);
    const QString p = path();
    if (const auto r = remoteRootOf(p); r && r->root == p)
        return r->label.section(QStringLiteral("://"), -1);
    return baseName(p);
}

void DirTab::setWatch(int wd)
{
    if (m_watch == wd)
        return;
    DirWatcher::instance()->removeWatch(m_watch);
    m_watch = wd;
}

void DirTab::navigate(const QString& path, const QString& focusName)
{
    startListing(path, focusName, History::Push, false);
}

void DirTab::refresh()
{
    if (path().isEmpty())
        return;
    if (m_inSearch) {
        // Re-check the results (after a delete/move) instead of searching again.
        QStringList names;
        for (int r = 0; r < m_model->count(); ++r)
            if (!m_model->entry(r)->isParent)
                names << m_model->entry(r)->name;
        QPointer<DirTab> self(this);
        const QString root = m_search.root;
        ioPool()->start([self, root, names] {
            auto updates = std::make_shared<std::vector<EntryUpdate>>(statNames(root, names, true));
            postToUi([self, updates] {
                if (self && self->m_inSearch)
                    self->m_model->applyUpdates(std::move(*updates));
            });
        });
        return;
    }
    if (m_loading) {
        // A navigation is under way: refreshing the old folder now would win
        // over it. Refresh whatever it lands on instead.
        m_refreshAfterLoad = true;
        return;
    }
    startListing(path(), cursorName(), History::None, true);
}

namespace {
// What a listing looks like, to notice changes cheaply. Folder sizes are left
// out (computed ones would differ from a fresh listing).
size_t listingSignature(const std::vector<const FileEntry*>& entries)
{
    QStringList parts;
    parts.reserve(qsizetype(entries.size()));
    for (const FileEntry* e : entries)
        parts << QStringLiteral("%1\t%2\t%3\t%4").arg(e->name).arg(e->isDir).arg(e->isDir ? -1 : e->size).arg(e->mtime);
    parts.sort();
    return qHash(parts.join(QLatin1Char('\n')));
}
}  // namespace

void DirTab::poll()
{
    if (m_polling || m_loading || !needsPolling() || path().isEmpty())
        return;
    std::vector<const FileEntry*> shown;
    for (int r = 0; r < m_model->count(); ++r)
        if (!m_model->entry(r)->isParent)
            shown.push_back(m_model->entry(r));
    for (const FileEntry& e : m_model->filteredOut())  // a filter hides, it doesn't remove
        shown.push_back(&e);
    const size_t before = listingSignature(shown);
    m_polling = true;
    const QString dir = path();
    ListOptions opt;
    opt.showHidden = m_showHidden;
    QPointer<DirTab> self(this);
    ioPool()->start([self, dir, opt, before] {
        const ListResult l = listLocation(dir, opt);
        std::vector<const FileEntry*> now;
        for (const FileEntry& e : l.entries)
            now.push_back(&e);
        const bool changed = !l.error && listingSignature(now) != before;
        postToUi([self, dir, changed] {
            if (!self)
                return;
            self->m_polling = false;
            if (changed && !self->m_loading && self->path() == dir)
                self->refresh();
        });
    });
}

void DirTab::goUp()
{
    if (m_inSearch) {  // leave the results: back to where the search started
        startListing(m_search.root, {}, History::None, false);
        return;
    }
    const QString p = path();
    if (p == QLatin1String("/") || p.isEmpty())
        return;
    startListing(parentPath(p), baseName(p), History::Push, false);
}

void DirTab::back()
{
    if (m_inSearch) {
        goUp();
        return;
    }
    if (m_back.isEmpty())
        return;
    startListing(m_back.last(), {}, History::Back, false);
}

void DirTab::forward()
{
    if (m_forward.isEmpty())
        return;
    startListing(m_forward.last(), {}, History::Forward, false);
}

void DirTab::cancelLoading()
{
    if (!m_loading)
        return;
    if (m_cancel)
        m_cancel->store(true);
    ++m_gen;  // drop whatever comes back
    m_loading = false;
    m_loadingPath.clear();
    emit loadingChanged(false);
}

void DirTab::setShowHidden(bool on)
{
    if (m_showHidden == on)
        return;
    m_showHidden = on;
    refresh();
}

void DirTab::startListing(const QString& pathIn, const QString& focus, History history, bool keepMarks)
{
    // GVfs's FUSE folders are slow for sftp and smb; Tandem talks to those itself.
    const QString native = nativeLocationFor(pathIn);
    const QString& path = native.isEmpty() ? pathIn : native;
    if (m_inSearch) {
        cancelSearch();
        m_inSearch = false;
        m_model->setListing(QString(), {});  // forget the results; the real listing follows
        emit searchChanged();
    }
    if (m_cancel)
        m_cancel->store(true);
    if (m_sizeCancel && path != this->path())
        m_sizeCancel->store(true);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const quint64 gen = ++m_gen;
    const bool wasLoading = m_loading;
    m_loading = true;
    m_loadingPath = path;
    if (!wasLoading)
        emit loadingChanged(true);

    const QSet<QString> marks = keepMarks ? m_model->markedSet() : QSet<QString>{};
    ListOptions opt;
    opt.showHidden = m_showHidden;
    opt.watch = true;
    opt.sort = m_model->sortSpec();

    QPointer<DirTab> self(this);
    auto cancel = m_cancel;
    ioPool()->start([self, gen, path, focus, history, marks, opt, cancel, keepMarks] {
        auto result = std::make_shared<ListResult>(listLocation(path, opt, cancel.get()));
        const bool networkFs = !result->error && !result->remote && !result->inArchive && isNetworkFilesystem(path);
        postToUi([self, gen, focus, history, marks, result, networkFs, keepMarks] {
            if (!self || gen != self->m_gen) {
                DirWatcher::instance()->removeWatch(result->watch);
                return;
            }
            DirTab* t = self.data();
            if (t->m_refreshAfterLoad) {  // something changed while this was loading
                t->m_refreshAfterLoad = false;
                QMetaObject::invokeMethod(t, &DirTab::refresh, Qt::QueuedConnection);
            }
            t->m_loading = false;
            t->m_loadingPath.clear();
            emit t->loadingChanged(false);
            if (result->error) {
                DirWatcher::instance()->removeWatch(result->watch);
                if (result->error == ENOENT)
                    FolderHistory::instance().forget(result->path);
                if (result->error != ECANCELED)
                    emit t->listingFailed(result->path, result->errorText.isEmpty() ? errnoString(result->error)
                                                                                    : result->errorText);
                return;
            }
            const QString old = t->m_model->path();
            if (old != result->path)
                FolderHistory::instance().visit(result->path);
            if (!old.isEmpty() && old != result->path) {
                switch (history) {
                case History::Push:
                    t->m_back.append(old);
                    t->m_forward.clear();
                    break;
                case History::Back:
                    t->m_back.removeLast();
                    t->m_forward.append(old);
                    break;
                case History::Forward:
                    t->m_forward.removeLast();
                    t->m_back.append(old);
                    break;
                case History::None:
                    break;
                }
                if (t->m_back.size() > 100)
                    t->m_back.removeFirst();
            }
            // A background refresh must not undo a cursor move or selection
            // made while the worker was reading the directory.
            QString focusName = keepMarks && old == result->path ? t->cursorName() : focus;
            QSet<QString> restore = keepMarks && old == result->path ? t->m_model->markedSet() : marks;
            if (!old.isEmpty() && old != result->path) {
                t->remember(old);
                const auto mem = t->m_memory.constFind(result->path);
                if (mem != t->m_memory.cend()) {
                    if (focusName.isEmpty())
                        focusName = mem->cursor;
                    restore = mem->marks;
                }
            }
            t->setWatch(result->watch);
            t->m_inArchive = result->inArchive;
            t->m_remote = result->remote;
            t->m_networkFs = networkFs;
            t->m_archivePath = result->archivePath;
            t->m_free = result->freeBytes;
            t->m_total = result->totalBytes;
            t->m_pendingNames.clear();
            if (old != result->path && !t->m_model->filter().isEmpty())
                t->m_model->setFilter({});  // a new folder starts unfiltered
            if (result->sortedBy != t->m_model->sortSpec())  // sort changed while loading
                sortEntries(result->entries, t->m_model->sortSpec());
            // Item counts of subfolders: only where reading them is cheap.
            t->m_model->setCountable(!result->remote && !result->inArchive && !networkFs, t->m_showHidden);
            t->m_model->setListing(result->path, std::move(result->entries));
            t->m_model->restoreMarks(restore);
            if (old != result->path)
                t->m_gitBranch.clear();
            t->refreshGit();
            if (!t->m_pendingFocus.isEmpty() && t->m_model->rowOf(t->m_pendingFocus) >= 0) {
                focusName = t->m_pendingFocus;
                t->m_pendingFocus.clear();
            }
            emit t->listingReady(focusName);
            emit t->infoChanged();
        });
    });
}

void DirTab::remember(const QString& dir)
{
    m_memory.insert(dir, {cursorName(), m_model->markedSet()});
    m_memoryOrder.removeOne(dir);
    m_memoryOrder.append(dir);
    while (m_memoryOrder.size() > 200)
        m_memory.remove(m_memoryOrder.takeFirst());
}

void DirTab::onWatchChanged(int wd, const QStringList& names)
{
    if (wd != m_watch || m_watch < 0 || m_loading)
        return;
    if (names.isEmpty()) {
        refresh(); // kqueue reports a changed directory without individual names
        return;
    }
    for (const QString& n : names)
        m_pendingNames.insert(n);
    if (!m_statInFlight)
        flushUpdates();
}

void DirTab::flushUpdates()
{
    if (m_pendingNames.isEmpty())
        return;
    if (m_pendingNames.size() > kMaxIncrementalUpdates) {
        m_pendingNames.clear();
        refresh();
        return;
    }
    const QStringList names(m_pendingNames.cbegin(), m_pendingNames.cend());
    m_pendingNames.clear();
    m_statInFlight = true;

    QPointer<DirTab> self(this);
    const QString dir = path();
    const bool hidden = m_showHidden;
    const quint64 gen = m_gen;
    ioPool()->start([self, dir, names, hidden, gen] {
        auto updates = std::make_shared<std::vector<EntryUpdate>>(statNames(dir, names, hidden));
        postToUi([self, updates, gen] {
            if (!self)
                return;
            DirTab* t = self.data();
            t->m_statInFlight = false;
            if (gen != t->m_gen)
                return;  // a new listing replaced this directory
            t->m_model->applyUpdates(std::move(*updates));
            t->m_gitTimer->start();
            if (!t->m_pendingFocus.isEmpty() && t->m_model->rowOf(t->m_pendingFocus) >= 0) {
                emit t->focusRequested(t->m_pendingFocus);
                t->m_pendingFocus.clear();
            }
            if (!t->m_pendingNames.isEmpty())
                t->flushUpdates();
        });
    });
}

void DirTab::refreshGit()
{
    const QString dir = m_model->path();
    const quint64 gen = ++m_gitGen;
    if (!g_gitEnabled || m_inArchive || m_inSearch || dir.isEmpty() || isRemotePath(dir) || isRemoteLocation(dir)) {
        m_model->setGitStatus({});
        if (!m_gitBranch.isEmpty()) {
            m_gitBranch.clear();
            emit infoChanged();
        }
        return;
    }
    QPointer<DirTab> self(this);
    ioPool()->start([self, dir, gen] {
        auto info = std::make_shared<GitInfo>(gitStatusFor(dir));
        postToUi([self, dir, gen, info] {
            if (!self || gen != self->m_gitGen || self->m_model->path() != dir)
                return;
            self->m_model->setGitStatus(info->status);
            if (self->m_gitBranch != info->branch) {
                self->m_gitBranch = info->branch;
                emit self->infoChanged();
            }
        });
    });
}

void DirTab::cancelSearch()
{
    if (m_searchCancel)
        m_searchCancel->store(true);
    if (m_searchRunning) {
        m_searchRunning = false;
        emit searchChanged();
    }
}

void DirTab::startSearch(const SearchSpec& spec)
{
    if (m_cancel)
        m_cancel->store(true);
    cancelSearch();
    ++m_gen;  // results of an older listing are ignored
    if (m_loading) {
        m_loading = false;
        emit loadingChanged(false);
    }
    setWatch(-1);
    m_search = spec;
    m_inSearch = true;
    m_model->setCountable(false, false);  // results are paths below the folder
    m_searchRunning = true;
    m_searchFound = 0;
    m_searchError.clear();
    m_model->setGitStatus({});
    if (!m_model->filter().isEmpty())
        m_model->setFilter({});
    m_model->setListing(spec.root, {});
    emit listingReady(QString());
    emit searchChanged();

    m_searchCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_searchCancel;
    const quint64 gen = m_gen;
    QPointer<DirTab> self(this);
    ioPool()->start([self, spec, cancel, gen] {
        const QString error = runSearch(spec, cancel.get(), 20000, [&](const QStringList& batch) {
            auto found = std::make_shared<std::vector<EntryUpdate>>(statNames(spec.root, batch, true));
            postToUi([self, gen, found] {
                if (!self || gen != self->m_gen || !self->m_inSearch)
                    return;
                std::vector<FileEntry> entries;
                for (auto& [name, e] : *found)
                    if (e)
                        entries.push_back(std::move(*e));
                self->m_searchFound += int(entries.size());
                self->m_model->appendEntries(std::move(entries));
                emit self->searchChanged();
            });
        });
        postToUi([self, gen, error, cancel] {
            if (!self || gen != self->m_gen || cancel->load())
                return;
            self->m_searchRunning = false;
            self->m_searchError = error;
            self->m_model->setSortSpec(self->m_model->sortSpec());  // results arrive unsorted
            emit self->searchChanged();
        });
    });
}

void DirTab::onDirectoryGone(int wd)
{
    if (wd != m_watch)
        return;
    setWatch(-1);
    // Walk up to the nearest directory that still exists, off the UI thread.
    QPointer<DirTab> self(this);
    const QString start = path();
    ioPool()->start([self, start] {
        QString p = start;
        struct stat st;
        while (p != QLatin1String("/") && !(::stat(toNative(p).constData(), &st) == 0 && S_ISDIR(st.st_mode)))
            p = parentPath(p);
        postToUi([self, p] {
            if (self)
                self->startListing(p, {}, History::None, false);
        });
    });
}

void DirTab::expectName(const QString& name)
{
    if (m_model->rowOf(name) >= 0) {
        emit focusRequested(name);
        return;
    }
    m_pendingFocus = name;
    if (m_watch < 0)  // no inotify here (e.g. some FUSE mounts): re-list instead
        QTimer::singleShot(150, this, [this] { refresh(); });
}

// Folder sizes can take minutes on big trees; they get their own small pool
// so they never delay directory listings in the shared I/O pool.
static QThreadPool* sizePool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(2);
        return p;
    }();
    return pool;
}

void DirTab::computeSize(const QString& name)
{
    computeSizes({name});
}

void DirTab::computeAllSizes()
{
    QStringList names;
    for (int r = 0; r < m_model->count(); ++r) {
        const FileEntry* e = m_model->entry(r);
        if (e && e->isDir && !e->isParent && !e->isLink)
            names.append(e->name);
    }
    computeSizes(names);
}

void DirTab::computeSizes(const QStringList& names)
{
    if (names.isEmpty() || m_inArchive)  // archive folders come with their sizes
        return;
    const QString dir = path();
    QPointer<DirTab> self(this);
    if (!m_sizeCancel || m_sizeCancel->load())
        m_sizeCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_sizeCancel;
    sizePool()->start([self, dir, names, cancel] {
        for (const QString& name : names) {
            if (cancel->load())
                return;
            const int64_t size = directorySizeAt(joinPath(dir, name), cancel.get());
            if (size < 0)
                continue;
            postToUi([self, dir, name, size] {
                if (self && self->path() == dir)
                    self->m_model->setDirSize(name, size);
            });
        }
    });
}

}  // namespace tandem
