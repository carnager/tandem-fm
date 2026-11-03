#include "core/DiskUsage.h"

#include "core/Backend.h"
#include "core/Util.h"

#include <QSet>

#include <algorithm>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

QString UsageNode::path() const
{
    return parent ? joinPath(parent->path(), name) : name;
}

namespace {

void sortTree(UsageNode* n)
{
    std::sort(n->children.begin(), n->children.end(),
              [](const auto& a, const auto& b) { return a->size > b->size; });
    for (auto& c : n->children)
        if (c->isDir)
            sortTree(c.get());
}

struct LocalWalk {
    dev_t dev;
    const std::atomic<bool>* cancel;
    const std::function<void(int64_t, int64_t)>& progress;
    QSet<QPair<quint64, quint64>> seen;  // hard links: once
    int64_t files = 0, bytes = 0;

    // Fills dir (an open directory fd) into node; openat keeps it fast.
    void walk(int fd, UsageNode* node)
    {
        DIR* d = ::fdopendir(fd);
        if (!d) {
            ::close(fd);
            return;
        }
        while (struct dirent* de = ::readdir(d)) {
            if (cancel && cancel->load())
                break;
            const char* n = de->d_name;
            if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)))
                continue;
            struct stat st;
            if (::fstatat(fd, n, &st, AT_SYMLINK_NOFOLLOW) != 0)
                continue;
            auto child = std::make_unique<UsageNode>();
            child->name = fromNative(QByteArray(n));
            child->parent = node;
            if (S_ISDIR(st.st_mode)) {
                child->isDir = true;
                child->size = int64_t(st.st_blocks) * 512;
                if (st.st_dev == dev) {  // other drives are left out
                    const int sub = ::openat(fd, n, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                    if (sub >= 0)
                        walk(sub, child.get());
                }
            } else {
                child->files = 1;
                const QPair<quint64, quint64> inode{quint64(st.st_dev), quint64(st.st_ino)};
                if (st.st_nlink > 1 && seen.contains(inode)) {
                    child->size = 0;  // another name of a file already counted
                } else {
                    if (st.st_nlink > 1)
                        seen.insert(inode);
                    child->size = int64_t(st.st_blocks) * 512;
                }
                bytes += child->size;
                if (++files % 2000 == 0 && progress)
                    progress(files, bytes);
            }
            node->size += child->size;
            node->files += child->files;
            node->children.push_back(std::move(child));
        }
        ::closedir(d);
    }
};

// Folders' sizes from their children (after a server's whole-tree listing).
void sumUp(UsageNode* n)
{
    if (!n->isDir)
        return;
    for (auto& c : n->children) {
        sumUp(c.get());
        n->size += c->size;
        n->files += c->files;
    }
}

}  // namespace

std::unique_ptr<UsageNode> measureUsage(const QString& root, const std::atomic<bool>* cancel,
                                        const std::function<void(int64_t, int64_t)>& progress)
{
    auto top = std::make_unique<UsageNode>();
    top->name = root;
    top->isDir = true;
    if (Backend* b = backendFor(root)) {
        // A server: everything in one listing when it can, else folder by folder.
        Backend::Tree tree;
        if (!b->listTree(root, true, tree, cancel)) {
            std::function<void(const QString&)> fetch = [&](const QString& rel) {
                if (cancel && cancel->load())
                    return;
                ListOptions o;
                o.showHidden = true;
                const ListResult l = b->list(rel.isEmpty() ? root : joinPath(root, rel), o, cancel);
                tree[rel] = l.entries;
                for (const FileEntry& e : l.entries)
                    if (e.isDir && !e.isLink)
                        fetch(rel.isEmpty() ? e.name : rel + QLatin1Char('/') + e.name);
            };
            fetch(QString());
        }
        std::function<void(UsageNode*, const QString&)> build = [&](UsageNode* node, const QString& rel) {
            for (const FileEntry& e : tree.value(rel)) {
                auto child = std::make_unique<UsageNode>();
                child->name = e.name;
                child->parent = node;
                child->isDir = e.isDir && !e.isLink;
                if (child->isDir)
                    build(child.get(), rel.isEmpty() ? e.name : rel + QLatin1Char('/') + e.name);
                else {
                    child->size = e.isLink ? 0 : qMax<int64_t>(0, e.size);
                    child->files = 1;
                }
                node->children.push_back(std::move(child));
            }
        };
        build(top.get(), QString());
        sumUp(top.get());
    } else {
        struct stat st;
        const int fd = ::open(toNative(root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0 || ::fstat(fd, &st) != 0) {
            if (fd >= 0)
                ::close(fd);
            return top;
        }
        LocalWalk w{st.st_dev, cancel, progress, {}};
        w.walk(fd, top.get());
    }
    sortTree(top.get());
    return top;
}

void removeUsageNode(UsageNode* node)
{
    UsageNode* parent = node->parent;
    if (!parent)
        return;
    for (UsageNode* p = parent; p; p = p->parent) {
        p->size -= node->size;
        p->files -= node->files;
    }
    auto& kids = parent->children;
    kids.erase(std::remove_if(kids.begin(), kids.end(), [node](const auto& c) { return c.get() == node; }), kids.end());
}

}  // namespace tandem
