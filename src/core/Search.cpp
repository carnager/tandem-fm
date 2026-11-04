#include "core/Search.h"

#include "core/Util.h"

#include <QDirIterator>
#include <algorithm>
#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace tandem {

namespace {

QString findTool(std::initializer_list<const char*> names)
{
    for (const char* n : names) {
        const QString p = QStandardPaths::findExecutable(QLatin1String(n));
        if (!p.isEmpty())
            return p;
    }
    return {};
}

QString clean(QString line)
{
    if (line.startsWith(QLatin1String("./")))
        line = line.mid(2);
    while (line.endsWith(QLatin1Char('/')))
        line.chop(1);
    return line;
}

// Streams the NUL-separated names a process prints into batches. Names may
// contain newlines, so lines won't do.
QString runProcess(const QString& program, const QStringList& args, const QString& cwd,
                   const std::atomic<bool>* cancel, int limit,
                   const std::function<void(const QStringList&)>& onBatch)
{
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(program, args);
    if (!p.waitForStarted(3000))
        return QObject::tr("Could not start %1").arg(program);
    QStringList batch;
    QByteArray pending;
    int total = 0;
    QElapsedTimer sinceFlush;
    sinceFlush.start();
    auto flush = [&] {
        if (!batch.isEmpty())
            onBatch(batch);
        batch.clear();
        sinceFlush.restart();
    };
    for (;;) {
        if (cancel && cancel->load()) {
            p.kill();
            p.waitForFinished(1000);
            return {};
        }
        const bool more = p.waitForReadyRead(100) || p.bytesAvailable() > 0;
        pending += p.readAllStandardOutput();
        qsizetype nl;
        while ((nl = pending.indexOf('\0')) >= 0) {
            const QString line = clean(QFile::decodeName(pending.left(nl)));
            pending.remove(0, nl + 1);
            if (line.isEmpty() || line == QLatin1String("."))
                continue;
            batch.append(line);
            if (++total >= limit) {
                flush();
                p.kill();
                p.waitForFinished(1000);
                return {};
            }
        }
        if (batch.size() >= 400 || sinceFlush.elapsed() > 250)
            flush();
        if (!more && p.state() == QProcess::NotRunning)
            break;
    }
    if (!pending.isEmpty())
        batch.append(clean(QFile::decodeName(pending)));
    flush();
    // rg/grep exit 1 when nothing matched; that's not an error.
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() > 1) {
        const QString err = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
        return err.isEmpty() ? QObject::tr("Search failed") : err.section(QLatin1Char('\n'), 0, 0);
    }
    return {};
}

// No fd: walk the tree ourselves (substring or wildcard match on names).
QString builtinNameSearch(const SearchSpec& spec, const std::atomic<bool>* cancel, int limit,
                          const std::function<void(const QStringList&)>& onBatch)
{
    const bool wild = spec.query.contains(QLatin1Char('*')) || spec.query.contains(QLatin1Char('?'));
    const bool caseSensitive = spec.query != spec.query.toLower();  // smart case
    const QRegularExpression rx(wild ? QRegularExpression::wildcardToRegularExpression(spec.query)
                                     : QRegularExpression::escape(spec.query),
                                caseSensitive ? QRegularExpression::NoPatternOption
                                              : QRegularExpression::CaseInsensitiveOption);
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks;
    if (spec.all)
        filters |= QDir::Hidden | QDir::System;
    QDirIterator it(spec.root, filters, QDirIterator::Subdirectories);
    QStringList batch;
    int total = 0;
    while (it.hasNext()) {
        if (cancel && cancel->load())
            return {};
        const QString path = it.next();
        const QString relative = path.mid(spec.root.size() + (spec.root.endsWith(QLatin1Char('/')) ? 0 : 1));
#ifdef Q_OS_MACOS
        // Qt on macOS doesn't hide dot folders by their name alone.
        if (!spec.all) {
            const auto parts = relative.split(QLatin1Char('/'));
            if (std::any_of(parts.begin(), parts.end(), [](const QString& part) { return part.startsWith(QLatin1Char('.')); }))
                continue;
        }
#endif
        if (!rx.match(it.fileName()).hasMatch())
            continue;
        batch.append(relative);
        if (++total >= limit)
            break;
        if (batch.size() >= 200) {
            onBatch(batch);
            batch.clear();
        }
    }
    onBatch(batch);
    return {};
}

}  // namespace

QString runSearch(const SearchSpec& spec, const std::atomic<bool>* cancel, int limit,
                  const std::function<void(const QStringList&)>& onBatch)
{
    if (spec.query.isEmpty())
        return {};
    if (!spec.contents) {
        const QString fd = findTool({"fd", "fdfind"});
        if (fd.isEmpty())
            return builtinNameSearch(spec, cancel, limit, onBatch);
        QStringList args{QStringLiteral("--color"), QStringLiteral("never"), QStringLiteral("--print0")};
        if (spec.all)
            args << QStringLiteral("--hidden") << QStringLiteral("--no-ignore");
        const bool wild = spec.query.contains(QLatin1Char('*')) || spec.query.contains(QLatin1Char('?'));
        if (wild)
            args << QStringLiteral("--glob");
        else
            args << QStringLiteral("--fixed-strings");
        args << QStringLiteral("--") << spec.query << QStringLiteral(".");
        return runProcess(fd, args, spec.root, cancel, limit, onBatch);
    }
    const QString rg = findTool({"rg"});
    if (!rg.isEmpty()) {
        QStringList args{QStringLiteral("--files-with-matches"), QStringLiteral("--null"), QStringLiteral("--no-messages"),
                         QStringLiteral("--smart-case"), QStringLiteral("--fixed-strings")};
        if (spec.all)
            args << QStringLiteral("--hidden") << QStringLiteral("--no-ignore");
        args << QStringLiteral("-e") << spec.query << QStringLiteral(".");
        return runProcess(rg, args, spec.root, cancel, limit, onBatch);
    }
    const QString grep = findTool({"grep"});
    if (grep.isEmpty())
        return QObject::tr("Install ripgrep (rg) to search file contents");
    QStringList args{QStringLiteral("-rlIF"), QStringLiteral("--null")};
    if (spec.query == spec.query.toLower())
        args << QStringLiteral("-i");
    if (!spec.all)
        args << QStringLiteral("--exclude-dir=.?*");  // not ".*": that also matches the "." we start in
    args << QStringLiteral("-e") << spec.query << QStringLiteral(".");
    return runProcess(grep, args, spec.root, cancel, limit, onBatch);
}

}  // namespace tandem
