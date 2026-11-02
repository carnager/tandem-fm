#include "core/GitStatus.h"

#include "core/Util.h"

#include <QProcess>

#include <sys/stat.h>

namespace tandem {

namespace {

int rank(char c)
{
    switch (c) {
    case 'U':
        return 5;
    case 'M':
        return 4;
    case 'A':
        return 3;
    case '?':
        return 2;
    case '!':
        return 1;
    default:
        return 0;
    }
}

char codeFor(char x, char y)
{
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D'))
        return 'U';
    if (x == '?' && y == '?')
        return '?';
    if (x == '!' && y == '!')
        return '!';
    if (y == 'M' || y == 'D' || x == 'M' || x == 'D' || x == 'R' || x == 'C' || y == 'T' || x == 'T')
        return 'M';
    if (x == 'A')
        return 'A';
    return 0;
}

QString findRoot(const QString& dir)
{
    QString p = dir;
    struct stat st;
    for (;;) {
        if (::stat(toNative(joinPath(p, QStringLiteral(".git"))).constData(), &st) == 0)
            return p;  // .git folder, or a .git file in worktrees/submodules
        if (p == QLatin1String("/") || p.isEmpty())
            return {};
        p = parentPath(p);
    }
}

}  // namespace

GitInfo parseGitStatus(const QByteArray& output, const QString& root, const QString& dir)
{
    GitInfo info;
    info.inRepo = true;
    info.root = root;
    QString rel = dir == root ? QString() : dir.mid(root.size() + 1);
    if (!rel.isEmpty())
        rel += QLatin1Char('/');

    const QList<QByteArray> records = output.split('\0');
    for (int i = 0; i < records.size(); ++i) {
        const QByteArray& r = records[i];
        if (r.size() < 3)
            continue;
        if (r.startsWith("## ")) {
            QByteArray b = r.mid(3);
            if (b.startsWith("No commits yet on "))
                b = b.mid(18);
            const int dots = int(b.indexOf("..."));
            const int space = int(b.indexOf(' '));
            info.branch = QString::fromUtf8(dots >= 0 ? b.left(dots) : (space >= 0 ? b.left(space) : b));
            continue;
        }
        const char x = r[0], y = r[1];
        QString path = QString::fromUtf8(r.mid(3));
        if (x == 'R' || x == 'C')
            ++i;  // the next record is the original name
        if (!path.startsWith(rel))
            continue;
        path = path.mid(rel.size());
        if (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        const QString child = path.section(QLatin1Char('/'), 0, 0);
        const char code = codeFor(x, y);
        if (child.isEmpty() || !code)
            continue;
        // An ignored file inside a folder doesn't make the folder ignored.
        if (code == '!' && path.contains(QLatin1Char('/')))
            continue;
        char& slot = info.status[child];
        if (rank(code) > rank(slot))
            slot = code;
    }
    return info;
}

GitInfo gitStatusFor(const QString& dir)
{
    const QString root = findRoot(dir);
    if (root.isEmpty() || dir.startsWith(joinPath(root, QStringLiteral(".git"))))
        return {};
    QProcess git;
    git.setProgram(QStringLiteral("git"));
    git.setArguments({QStringLiteral("-C"), dir, QStringLiteral("--no-optional-locks"), QStringLiteral("status"),
                      QStringLiteral("--porcelain=v1"), QStringLiteral("-z"), QStringLiteral("--branch"),
                      QStringLiteral("--ignored=matching"), QStringLiteral("--untracked-files=normal"),
                      QStringLiteral("--"), QStringLiteral(".")});
    git.start();
    if (!git.waitForFinished(5000) || git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0) {
        git.kill();
        git.waitForFinished(500);
        return {};
    }
    // git prints paths relative to the repository root with -z.
    return parseGitStatus(git.readAllStandardOutput(), root, dir);
}

}  // namespace tandem
