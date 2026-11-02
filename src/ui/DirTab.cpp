#include "ui/DirTab.h"

#include "core/DirWatcher.h"
#include "core/FileOps.h"
#include "core/FolderHistory.h"
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

DirTab::DirTab(QObject* parent) : QObject(parent), m_model(new FileModel(this))
{
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
    const QString p = path();
    if (p == QDir::homePath())
        return QStringLiteral("~");
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
    startListing(path(), cursorName(), History::None, true);
}

void DirTab::goUp()
{
    const QString p = path();
    if (p == QLatin1String("/") || p.isEmpty())
        return;
    startListing(parentPath(p), baseName(p), History::Push, false);
}

void DirTab::back()
{
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

void DirTab::startListing(const QString& path, const QString& focus, History history, bool keepMarks)
{
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
    ioPool()->start([self, gen, path, focus, history, marks, opt, cancel] {
        auto result = std::make_shared<ListResult>(listLocation(path, opt, cancel.get()));
        postToUi([self, gen, focus, history, marks, result] {
            if (!self || gen != self->m_gen) {
                DirWatcher::instance()->removeWatch(result->watch);
                return;
            }
            DirTab* t = self.data();
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
            QString focusName = focus;
            QSet<QString> restore = marks;
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
            t->m_archivePath = result->archivePath;
            t->m_free = result->freeBytes;
            t->m_total = result->totalBytes;
            t->m_pendingNames.clear();
            if (result->sortedBy != t->m_model->sortSpec())  // sort changed while loading
                sortEntries(result->entries, t->m_model->sortSpec());
            t->m_model->setListing(result->path, std::move(result->entries));
            t->m_model->restoreMarks(restore);
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
            if (!t->m_pendingFocus.isEmpty() && t->m_model->rowOf(t->m_pendingFocus) >= 0) {
                emit t->focusRequested(t->m_pendingFocus);
                t->m_pendingFocus.clear();
            }
            if (!t->m_pendingNames.isEmpty())
                t->flushUpdates();
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
            const int64_t size = directorySize(joinPath(dir, name), cancel.get());
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
