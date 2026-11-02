#pragma once

// A pretend server for tests: "test://srv/x" is the folder root + "/x" on
// disk, but everything goes through the Backend interface, like sftp would.

#include "core/Backend.h"
#include "core/Util.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace tandem {

class TestBackend : public Backend {
public:
    explicit TestBackend(QString root) : m_root(std::move(root)) {}

    std::atomic<int> latencyMs{0};
    std::atomic<int> calls{0};

    QString local(const QString& uri) const
    {
        const QString p = locationPath(uri);
        return p == QLatin1String("/") ? m_root : m_root + p;
    }

    ListResult list(const QString& path, const ListOptions& opt, const std::atomic<bool>* cancel) override
    {
        wait();
        ListOptions o = opt;
        o.watch = false;
        ListResult r = localBackend()->list(local(path), o, cancel);
        r.freeBytes = r.totalBytes = -1;  // servers often don't say
        return r;
    }
    int stat(const QString& path, FileStat& out) override { wait(); return localBackend()->stat(local(path), out); }
    std::unique_ptr<Reader> openRead(const QString& path, int& err) override
    {
        wait();
        return localBackend()->openRead(local(path), err);
    }
    std::unique_ptr<Writer> openWrite(const QString& path, bool exclusive, uint32_t mode, int& err) override
    {
        wait();
        return localBackend()->openWrite(local(path), exclusive, mode, err);
    }
    int mkdir(const QString& path, uint32_t mode) override { wait(); return localBackend()->mkdir(local(path), mode); }
    int rename(const QString& from, const QString& to) override
    {
        wait();
        return localBackend()->rename(local(from), local(to));
    }
    int removeFile(const QString& path) override { wait(); return localBackend()->removeFile(local(path)); }
    int removeDir(const QString& path) override { wait(); return localBackend()->removeDir(local(path)); }
    int readLink(const QString& path, QString& target) override { wait(); return localBackend()->readLink(local(path), target); }
    int symlink(const QString& target, const QString& path) override
    {
        wait();
        return localBackend()->symlink(target, local(path));
    }

private:
    void wait()
    {
        ++calls;
        if (const int ms = latencyMs.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    QString m_root;
};

}  // namespace tandem
