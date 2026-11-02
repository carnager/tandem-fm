// Benchmarks.
//   tandem-bench [--files N] [--dir PATH] [--runs R]
//       Directory listing: tandem's lister vs QDir. Without --dir, a test
//       directory with N files is created once in /tmp.
//   tandem-bench --copy-latency SRC_FILE DEST_DIR [--dir LISTDIR]
//       Copies SRC_FILE into DEST_DIR as a background job while measuring how
//       long the main thread is ever blocked (it should not be) and how fast
//       listings run during the transfer.

#include "core/DirLister.h"
#include "core/FileOps.h"
#include "core/Util.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTextStream>
#include <QTimer>

#include <atomic>

#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

using namespace tandem;

static void createFiles(const QString& dir, int n)
{
    QDir().mkpath(dir);
    static const char* words[] = {"report", "IMG", "Übersicht", "notes", "track", "backup", "Zebra", "äpfel", "data"};
    for (int i = 0; i < n; ++i) {
        const QString name = QStringLiteral("%1_%2.%3").arg(QLatin1String(words[i % 9])).arg(i).arg(i % 3 ? "txt" : "jpg");
        const QByteArray p = QFile::encodeName(dir + QLatin1Char('/') + name);
        const int fd = ::open(p.constData(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0)
            ::close(fd);
    }
    for (int i = 0; i < n / 100; ++i)
        QDir(dir).mkdir(QStringLiteral("dir_%1").arg(i));
}

static int copyLatency(QCoreApplication& app, const QString& src, const QString& destDir, const QString& listDir)
{
    QTextStream out(stdout);
    CopyJob job(CopyJob::Mode::Copy, {src}, destDir);
    job.setSyncPromptHandler([](const PromptRequest&) { return Answer::Overwrite; });

    QElapsedTimer clock;
    clock.start();
    qint64 last = 0, maxGap = 0, ticks = 0;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        const qint64 now = clock.nsecsElapsed() / 1000;
        if (last)
            maxGap = std::max(maxGap, now - last);
        last = now;
        ++ticks;
    });
    timer.start(2);

    std::atomic<int> listings{0};
    std::atomic<qint64> listMsTotal{0};
    std::atomic<bool> stop{false};
    if (!listDir.isEmpty()) {
        ioPool()->start([&] {
            while (!stop.load()) {
                QElapsedTimer t;
                t.start();
                listDirectory(listDir, ListOptions{});
                listMsTotal += t.elapsed();
                ++listings;
            }
        });
    }

    QObject::connect(&job, &Job::finished, &app, [&] { app.quit(); });
    job.start();
    app.exec();
    stop.store(true);
    ioPool()->waitForDone();

    const double secs = clock.elapsed() / 1000.0;
    const auto snap = job.snapshot();
    out << "Copied " << formatSize(snap.bytesDone) << " in " << QString::number(secs, 'f', 2) << " s ("
        << formatSpeed(snap.bytesDone / std::max(secs, 0.001)) << ")\n";
    out << "  main thread: " << ticks << " timer ticks, longest gap " << QString::number(maxGap / 1000.0, 'f', 2)
        << " ms (2 ms timer)\n";
    if (listings)
        out << "  listings of " << listDir << " during the copy: " << listings << ", avg "
            << listMsTotal / listings << " ms\n";
    if (!job.errors().isEmpty())
        out << "  errors: " << job.errors().join("; ") << "\n";
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    {
        const QStringList a = app.arguments();
        const int i = a.indexOf(QStringLiteral("--copy-latency"));
        if (i >= 0 && i + 2 < a.size()) {
            const int d = a.indexOf(QStringLiteral("--dir"));
            return copyLatency(app, a[i + 1], a[i + 2], d >= 0 && d + 1 < a.size() ? a[d + 1] : QString());
        }
    }
    int files = 200000;
    int runs = 5;
    QString dir;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--files") && i + 1 < args.size())
            files = args[++i].toInt();
        else if (args[i] == QLatin1String("--dir") && i + 1 < args.size())
            dir = args[++i];
        else if (args[i] == QLatin1String("--runs") && i + 1 < args.size())
            runs = args[++i].toInt();
    }
    if (dir.isEmpty()) {
        dir = QDir::tempPath() + QStringLiteral("/tandem-bench-%1").arg(files);
        if (!QDir(dir).exists()) {
            out << "Creating " << files << " files in " << dir << " ...\n";
            out.flush();
            createFiles(dir, files);
        }
    }

    auto median = [](std::vector<qint64> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };

    std::vector<qint64> tandemMs, qdirMs;
    size_t count = 0;
    for (int r = 0; r < runs; ++r) {
        QElapsedTimer t;
        t.start();
        ListResult res = listDirectory(dir, ListOptions{});
        tandemMs.push_back(t.elapsed());
        count = res.entries.size();
        if (res.error) {
            out << "error: " << res.error << "\n";
            return 1;
        }

        t.restart();
        const QFileInfoList l = QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                        QDir::Name | QDir::DirsFirst | QDir::IgnoreCase);
        // Touch size/time like a file manager has to.
        qint64 sum = 0;
        for (const QFileInfo& fi : l)
            sum += fi.size() + fi.lastModified().toSecsSinceEpoch();
        qdirMs.push_back(t.elapsed());
        Q_UNUSED(sum);
    }

    out << "Directory: " << dir << " (" << count << " entries, " << runs << " runs, warm cache)\n";
    out << "  tandem listDirectory (stat + natural sort keys + sort): " << median(tandemMs) << " ms median\n";
    out << "  QDir::entryInfoList + size/mtime:                        " << median(qdirMs) << " ms median\n";
    return 0;
}
