#include "core/FolderCompare.h"

#include "core/DirLister.h"
#include "core/FileOps.h"
#include "core/Util.h"

#include <QFile>
#include <QMap>

#include <cstdlib>
#include <cstring>

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

    bool cancelled() const { return cancel && cancel->load(); }

    void walk(const QString& rel)
    {
        if (cancelled())
            return;
        if (progress)
            progress(rel);
        ListOptions lo;
        lo.showHidden = opt.hidden;
        const ListResult l = listDirectory(rel.isEmpty() ? leftRoot : joinPath(leftRoot, rel), lo);
        const ListResult r = listDirectory(rel.isEmpty() ? rightRoot : joinPath(rightRoot, rel), lo);
        if (rel.isEmpty() && (l.error || r.error)) {
            out.error = errnoString(l.error ? l.error : r.error);
            return;
        }
        // Case-insensitive natural-ish order, but exact names decide identity.
        QMap<QString, std::pair<const FileEntry*, const FileEntry*>> names;
        auto keyOf = [](const QString& n) { return n.toCaseFolded() + QLatin1Char('\0') + n; };
        for (const FileEntry& e : l.entries)
            names[keyOf(e.name)].first = &e;
        for (const FileEntry& e : r.entries)
            names[keyOf(e.name)].second = &e;

        QStringList subdirs;
        for (auto it = names.cbegin(); it != names.cend(); ++it) {
            if (cancelled())
                return;
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
                if (item.left.isDir)  // what copying it over would cost
                    item.left.size = directorySize(joinPath(leftRoot, item.rel), cancel);
            } else if (!a) {
                item.diff = Diff::RightOnly;
                if (item.right.isDir)
                    item.right.size = directorySize(joinPath(rightRoot, item.rel), cancel);
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
        for (const QString& d : subdirs)
            walk(d);
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
    Walker w{left, right, options, cancel, progress, out};
    w.walk(QString());
    return out;
}

std::optional<bool> sameContents(const QString& a, const QString& b, const std::atomic<bool>* cancel)
{
    QFile fa(a), fb(b);
    if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly))
        return std::nullopt;
    if (fa.size() != fb.size())
        return false;
    QByteArray ba(1 << 20, Qt::Uninitialized), bb(1 << 20, Qt::Uninitialized);
    for (;;) {
        if (cancel && cancel->load())
            return std::nullopt;
        const qint64 na = fa.read(ba.data(), ba.size());
        const qint64 nb = fb.read(bb.data(), bb.size());
        if (na < 0 || nb < 0)
            return std::nullopt;
        if (na != nb || memcmp(ba.constData(), bb.constData(), size_t(na)) != 0)
            return false;
        if (na == 0)
            return true;
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
