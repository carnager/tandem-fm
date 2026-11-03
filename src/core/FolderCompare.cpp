#include "core/FolderCompare.h"

#include "core/Backend.h"
#include "core/DirLister.h"
#include "core/FileOps.h"
#include "core/Remote.h"
#include "core/Util.h"

#include <QFile>
#include <QMap>

#include <cstdlib>
#include <cstring>
#include <thread>

namespace tandem {

namespace {

CompareSide sideOf(const FileEntry& e)
{
    CompareSide s;
    s.exists = true;
    s.isDir = e.isDir && !e.isLink;
    s.size = s.isDir ? -1 : e.size;
    s.mtime = e.mtime;
    return s;
}

struct Walker {
    QString leftRoot, rightRoot;
    const CompareOptions& opt;
    const std::atomic<bool>* cancel;
    const std::function<void(const QString&)>& progress;
    CompareResult& out;

    std::optional<Backend::Tree> leftTree, rightTree;  // whole trees, when a server hands them over

    bool cancelled() const { return cancel && cancel->load(); }
    // Folder sizes are only worth it where listing is cheap.
    bool cheap(const QString& root) const
    {
        if (root == leftRoot && leftTree)
            return true;
        if (root == rightRoot && rightTree)
            return true;
        return !isRemoteLocation(root) && !isRemotePath(root);
    }
    ListResult listSide(const QString& root, const std::optional<Backend::Tree>& tree, const QString& rel) const
    {
        if (!tree) {
            ListOptions lo;
            lo.showHidden = opt.hidden;
            return listLocation(rel.isEmpty() ? root : joinPath(root, rel), lo);
        }
        ListResult r;
        r.entries = tree->value(rel);
        return r;
    }
    int64_t sizeOf(const QString& root, const QString& rel) const
    {
        const std::optional<Backend::Tree>& tree = root == leftRoot ? leftTree : rightTree;
        if (!tree)
            return directorySizeAt(joinPath(root, rel), cancel);
        int64_t total = 0;
        for (const FileEntry& e : tree->value(rel))
            total += e.isDir && !e.isLink ? qMax<int64_t>(0, sizeOf(root, joinPath(rel, e.name)))
                                          : (e.isLink ? 0 : qMax<int64_t>(0, e.size));
        return total;
    }
    // A server that can list a whole tree in one request does (one "find"
    // instead of a round trip per folder).
    void prefetch()
    {
        if (!opt.recursive)
            return;
        auto fetch = [this](const QString& root, std::optional<Backend::Tree>& into) {
            Backend* b = backendFor(root);
            Backend::Tree t;
            if (b && b->listTree(root, opt.hidden, t, cancel))
                into = std::move(t);
        };
        std::thread other([&] { fetch(rightRoot, rightTree); });
        fetch(leftRoot, leftTree);
        other.join();
    }

    struct Listed {
        ListResult left, right;
    };

    // One level at a time, its folders listed in parallel: on a server each
    // listing is a few round trips, so waiting for them one by one is what
    // makes comparing slow (4000 folders at 20 ms ping: minutes vs seconds).
    void run()
    {
        prefetch();
        const bool remote = !cheap(leftRoot) || !cheap(rightRoot);
        QStringList level{QString()};
        while (!level.isEmpty() && !cancelled()) {
            std::vector<Listed> listed(size_t(level.size()));
            std::atomic<int> next{0};
            auto work = [&] {
                for (int k; (k = next.fetch_add(1)) < level.size() && !cancelled();) {
                    const QString& rel = level[k];
                    listed[size_t(k)].left = listSide(leftRoot, leftTree, rel);
                    listed[size_t(k)].right = listSide(rightRoot, rightTree, rel);
                    if (progress)
                        progress(rel);
                }
            };
            const int threads = remote ? qMin(6, int(level.size())) : 1;
            std::vector<std::thread> pool;
            for (int t = 1; t < threads; ++t)
                pool.emplace_back(work);
            work();
            for (std::thread& t : pool)
                t.join();
            if (cancelled())
                return;
            QStringList deeper;
            for (int k = 0; k < level.size(); ++k)
                if (!compareLevel(level[k], listed[size_t(k)], deeper))
                    return;
            level = deeper;
        }
        // Folder by folder, names in order (the walk went level by level).
        auto key = [](const QString& rel) { return rel.toCaseFolded(); };
        std::stable_sort(out.items.begin(), out.items.end(),
                         [&](const CompareItem& a, const CompareItem& b) { return key(a.rel) < key(b.rel); });
    }

    bool compareLevel(const QString& rel, const Listed& ls, QStringList& subdirs)
    {
        const ListResult& l = ls.left;
        const ListResult& r = ls.right;
        if (rel.isEmpty() && (l.error || r.error)) {
            out.error = errnoString(l.error ? l.error : r.error);
            return false;
        }
        // Case-insensitive natural-ish order, but exact names decide identity.
        QMap<QString, std::pair<const FileEntry*, const FileEntry*>> names;
        auto keyOf = [](const QString& n) { return n.toCaseFolded() + QLatin1Char('\0') + n; };
        for (const FileEntry& e : l.entries)
            names[keyOf(e.name)].first = &e;
        for (const FileEntry& e : r.entries)
            names[keyOf(e.name)].second = &e;

        for (auto it = names.cbegin(); it != names.cend(); ++it) {
            if (cancelled())
                return false;
            const FileEntry* a = it->first;
            const FileEntry* b = it->second;
            CompareItem item;
            item.rel = rel.isEmpty() ? (a ? a->name : b->name) : rel + QLatin1Char('/') + (a ? a->name : b->name);
            if (a)
                item.left = sideOf(*a);
            if (b)
                item.right = sideOf(*b);
            if (!b) {
                item.diff = Diff::LeftOnly;
                if (item.left.isDir && cheap(leftRoot))  // what copying it over would cost
                    item.left.size = sizeOf(leftRoot, item.rel);
            } else if (!a) {
                item.diff = Diff::RightOnly;
                if (item.right.isDir && cheap(rightRoot))
                    item.right.size = sizeOf(rightRoot, item.rel);
            } else if (item.left.isDir != item.right.isDir) {
                item.diff = Diff::TypeMismatch;
            } else if (item.left.isDir) {
                if (opt.recursive)
                    subdirs << item.rel;
                continue;  // both folders: their contents tell
            } else {
                item.diff = compareFiles(item);
            }
            out.items.push_back(std::move(item));
        }
        return true;
    }

    Diff compareFiles(const CompareItem& item) const
    {
        const bool sameTime = std::llabs(item.left.mtime - item.right.mtime) <= opt.tolerance;
        const bool sameSize = item.left.size == item.right.size;
        if (opt.contents && sameSize) {
            const auto same = sameContents(joinPath(leftRoot, item.rel), joinPath(rightRoot, item.rel), cancel);
            if (same && *same)
                return Diff::Equal;
            if (sameTime)
                return Diff::Differs;
        } else if (sameTime) {
            return sameSize ? Diff::Equal : Diff::Differs;
        }
        return item.left.mtime > item.right.mtime ? Diff::LeftNewer : Diff::RightNewer;
    }
};

}  // namespace

CompareResult compareFolders(const QString& left, const QString& right, const CompareOptions& options,
                             const std::atomic<bool>* cancel, const std::function<void(const QString&)>& progress)
{
    CompareResult out;
    Walker w{left, right, options, cancel, progress, out, {}, {}};
    w.run();
    return out;
}

std::optional<bool> sameContents(const QString& a, const QString& b, const std::atomic<bool>* cancel)
{
    Backend* ba = anyBackendFor(a);
    Backend* bb = anyBackendFor(b);
    if (!ba || !bb)
        return std::nullopt;
    FileStat sa, sb;
    if (ba->stat(a, sa) || bb->stat(b, sb))
        return std::nullopt;
    if (sa.size != sb.size)
        return false;
    int err = 0;
    auto ra = ba->openRead(a, err);
    auto rb = ra ? bb->openRead(b, err) : nullptr;
    if (!ra || !rb)
        return std::nullopt;
    // Reads can come back short (network), so each side keeps its own window.
    std::vector<char> bufA(1 << 20), bufB(1 << 20);
    int64_t offA = 0, lenA = 0, offB = 0, lenB = 0;
    for (;;) {
        if (cancel && cancel->load())
            return std::nullopt;
        if (offA == lenA) {
            offA = 0;
            lenA = ra->read(bufA.data(), int64_t(bufA.size()));
            if (lenA < 0)
                return std::nullopt;
        }
        if (offB == lenB) {
            offB = 0;
            lenB = rb->read(bufB.data(), int64_t(bufB.size()));
            if (lenB < 0)
                return std::nullopt;
        }
        const int64_t n = qMin(lenA - offA, lenB - offB);
        if (n == 0)
            return lenA - offA == lenB - offB;  // both at the end
        if (memcmp(bufA.data() + offA, bufB.data() + offB, size_t(n)) != 0)
            return false;
        offA += n;
        offB += n;
    }
}

SyncAction defaultAction(Diff diff, SyncMode mode)
{
    switch (mode) {
    case SyncMode::TwoWay:
        switch (diff) {
        case Diff::LeftOnly:
        case Diff::LeftNewer:
            return SyncAction::ToRight;
        case Diff::RightOnly:
        case Diff::RightNewer:
            return SyncAction::ToLeft;
        default:
            return SyncAction::None;  // equal, or unclear: the user decides
        }
    case SyncMode::MirrorToRight:
        switch (diff) {
        case Diff::LeftOnly:
        case Diff::LeftNewer:
        case Diff::RightNewer:
        case Diff::Differs:
            return SyncAction::ToRight;
        case Diff::RightOnly:
            return SyncAction::DeleteRight;
        default:
            return SyncAction::None;
        }
    case SyncMode::MirrorToLeft:
        switch (diff) {
        case Diff::RightOnly:
        case Diff::RightNewer:
        case Diff::LeftNewer:
        case Diff::Differs:
            return SyncAction::ToLeft;
        case Diff::LeftOnly:
            return SyncAction::DeleteLeft;
        default:
            return SyncAction::None;
        }
    }
    return SyncAction::None;
}

SyncPlan buildSyncPlan(const QString& left, const QString& right, const std::vector<CompareItem>& items,
                       const std::vector<SyncAction>& actions)
{
    SyncPlan plan;
    for (size_t i = 0; i < items.size() && i < actions.size(); ++i) {
        const CompareItem& it = items[i];
        const QString l = joinPath(left, it.rel), r = joinPath(right, it.rel);
        switch (actions[i]) {
        case SyncAction::None:
            break;
        case SyncAction::ToRight:
            if (it.left.exists && !(it.right.exists && it.right.isDir != it.left.isDir)) {
                plan.copies.append({l, r});
                plan.bytes += qMax<int64_t>(0, it.left.size);
            }
            break;
        case SyncAction::ToLeft:
            if (it.right.exists && !(it.left.exists && it.left.isDir != it.right.isDir)) {
                plan.copies.append({r, l});
                plan.bytes += qMax<int64_t>(0, it.right.size);
            }
            break;
        case SyncAction::DeleteLeft:
            if (it.left.exists)
                plan.deletions << l;
            break;
        case SyncAction::DeleteRight:
            if (it.right.exists)
                plan.deletions << r;
            break;
        }
    }
    return plan;
}

}  // namespace tandem
