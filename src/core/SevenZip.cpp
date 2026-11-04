#include "core/SevenZip.h"

#include <QDateTime>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <sys/stat.h>

namespace tandem {

namespace {

// Asks for a password on stdin only when it needs one: give it either way.
void startWithPassword(QProcess& p, const QStringList& args, const QByteArray& password)
{
    p.setProgram(sevenZipProgram());
    p.setArguments(args);
    p.start();
    if (p.waitForStarted()) {
        p.write(password + '\n');
        p.closeWriteChannel();
    }
}

bool mentionsPassword(const QByteArray& err)
{
    const QByteArray e = err.toLower();
    return e.contains("password") || e.contains("encrypted");
}

SevenZipStatus finish(QProcess& p, QString* error)
{
    p.waitForFinished(-1);
    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
        return SevenZipStatus::Ok;
    const QByteArray err = p.readAllStandardError();
    if (mentionsPassword(err))
        return SevenZipStatus::WrongPassword;
    if (error)
        *error = QString::fromUtf8(err).trimmed().section(QLatin1Char('\n'), 0, 0);
    if (error && error->isEmpty())
        *error = p.errorString();
    return SevenZipStatus::Failed;
}

uint32_t parseMode(const QString& s, bool dir)
{
    if (s.size() != 10)
        return dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    uint32_t m = s[0] == QLatin1Char('d') ? S_IFDIR : s[0] == QLatin1Char('l') ? S_IFLNK : S_IFREG;
    static const uint32_t bits[9] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
    for (int i = 0; i < 9; ++i)
        if (s[i + 1] != QLatin1Char('-'))
            m |= bits[i];
    return m;
}

}  // namespace

QString sevenZipProgram()
{
    static const QString found = [] {
        for (const char* name : {"7zz", "7z", "7za"})
            if (QString p = QStandardPaths::findExecutable(QString::fromLatin1(name)); !p.isEmpty())
                return p;
        return QString();
    }();
    return found;
}

SevenZipStatus sevenZipList(const QString& archive, const QByteArray& password, QList<SevenZipItem>* out,
                            QString* error)
{
    if (sevenZipProgram().isEmpty()) {
        *error = QObject::tr("7-Zip is not installed");
        return SevenZipStatus::Failed;
    }
    QProcess p;
    startWithPassword(p, {QStringLiteral("l"), QStringLiteral("-slt"), QStringLiteral("-sccUTF-8"),
                          QStringLiteral("--"), archive},
                      password);
    p.waitForFinished(-1);
    const QByteArray text = p.readAllStandardOutput();
    if (const SevenZipStatus s = finish(p, error); s != SevenZipStatus::Ok)
        return s;

    out->clear();
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'));
    bool started = false;
    SevenZipItem item;
    QString attributes;
    bool have = false;
    auto flush = [&] {
        if (!have)
            return;
        const QString unixMode = attributes.section(QLatin1Char(' '), 1, 1);
        item.isDir = attributes.section(QLatin1Char(' '), 0, 0).contains(QLatin1Char('D')) ||
                     unixMode.startsWith(QLatin1Char('d'));
        item.mode = parseMode(unixMode, item.isDir);
        item.isLink = (item.mode & S_IFMT) == S_IFLNK;
        out->append(item);
        item = {};
        attributes.clear();
        have = false;
    };
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (!started) {
            started = line.startsWith(QLatin1String("----------"));
            continue;
        }
        if (line.isEmpty()) {
            flush();
            continue;
        }
        const int eq = int(line.indexOf(QLatin1String(" = ")));
        const QString key = eq < 0 ? line.chopped(line.endsWith(QLatin1String(" =")) ? 2 : 0) : line.left(eq);
        const QString value = eq < 0 ? QString() : line.mid(eq + 3);
        if (key == QLatin1String("Path")) {
            flush();
            item.path = value;
            have = true;
        } else if (key == QLatin1String("Size")) {
            item.size = value.toLongLong();
        } else if (key == QLatin1String("Modified")) {
            item.mtime = QDateTime::fromString(value.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss")).toSecsSinceEpoch();
        } else if (key == QLatin1String("Attributes")) {
            attributes = value;
        } else if (key == QLatin1String("Encrypted")) {
            item.encrypted = value == QLatin1String("+");
        } else if (key == QLatin1String("Folder")) {
            if (value == QLatin1String("+"))
                attributes = QStringLiteral("D ") + attributes.section(QLatin1Char(' '), 1);
        }
    }
    flush();
    return SevenZipStatus::Ok;
}

SevenZipStatus sevenZipTest(const QString& archive, const QString& member, const QByteArray& password,
                            QString* error)
{
    QProcess p;
    // -spd: the member is a name, not a wildcard pattern.
    startWithPassword(p, {QStringLiteral("t"), QStringLiteral("-sccUTF-8"), QStringLiteral("-spd"),
                          QStringLiteral("--"), archive, member},
                      password);
    return finish(p, error);
}

bool sevenZipCreate(const QString& target, const QStringList& sources, const QByteArray& password,
                    const std::function<bool(int)>& progress, QString* error)
{
    if (sevenZipProgram().isEmpty()) {
        *error = QObject::tr("7-Zip is not installed");
        return false;
    }
    // -p alone: ask for the password (on stdin); -mhe: hide the names too.
    QStringList args{QStringLiteral("a"),    QStringLiteral("-t7z"),     QStringLiteral("-mhe=on"),
                     QStringLiteral("-p"),   QStringLiteral("-snl"),     QStringLiteral("-sccUTF-8"),
                     QStringLiteral("-spd"), QStringLiteral("-bsp1"),    QStringLiteral("-bso0"),
                     QStringLiteral("--"),   target};
    args += sources;
    QProcess p;
    startWithPassword(p, args, password);
    static const QRegularExpression percent(QStringLiteral("(\\d+)%"));
    while (p.state() != QProcess::NotRunning) {
        p.waitForReadyRead(200);
        const QString chunk = QString::fromUtf8(p.readAllStandardOutput());
        int last = -1;
        for (auto it = percent.globalMatch(chunk); it.hasNext();)
            last = it.next().captured(1).toInt();
        if (!progress(last)) {
            p.kill();
            p.waitForFinished(-1);
            *error = QObject::tr("Cancelled");
            return false;
        }
    }
    return finish(p, error) == SevenZipStatus::Ok;
}

SevenZipStream::SevenZipStream() = default;

SevenZipStream::~SevenZipStream()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(-1);
    }
}

bool SevenZipStream::start(const QString& archive, const QByteArray& password, QString* error)
{
    if (sevenZipProgram().isEmpty()) {
        *error = QObject::tr("7-Zip is not installed");
        return false;
    }
    m_proc = std::make_unique<QProcess>();
    startWithPassword(*m_proc, {QStringLiteral("x"), QStringLiteral("-so"), QStringLiteral("-sccUTF-8"),
                                QStringLiteral("--"), archive},
                      password);
    if (m_proc->state() == QProcess::NotRunning) {
        *error = m_proc->errorString();
        return false;
    }
    return true;
}

qint64 SevenZipStream::read(char* buf, qint64 n)
{
    for (;;) {
        if (const qint64 got = m_proc->read(buf, n); got > 0)
            return got;
        if (m_proc->state() == QProcess::NotRunning) {
            if (m_proc->bytesAvailable() > 0)
                continue;
            QString why;
            const SevenZipStatus s = finish(*m_proc, &why);
            if (s == SevenZipStatus::Ok)
                return 0;
            m_error = s == SevenZipStatus::WrongPassword ? QObject::tr("wrong or no password") : why;
            return -1;
        }
        m_proc->waitForReadyRead(1000);
    }
}

}  // namespace tandem
