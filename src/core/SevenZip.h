#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QProcess;

namespace tandem {

// The 7-Zip program (7zz, 7z or 7za), for what libarchive can't do:
// encrypted 7z (and RAR) archives. Passwords go to it on stdin, never on
// the command line. All calls block: use them from a worker.

QString sevenZipProgram();  // empty when 7-Zip isn't installed

struct SevenZipItem {
    QString path;
    int64_t size = 0;
    int64_t mtime = 0;
    uint32_t mode = 0;  // type and permission bits
    bool isDir = false;
    bool isLink = false;
    bool encrypted = false;
};

enum class SevenZipStatus { Ok, WrongPassword, Failed };

// Members in archive order (the order their data comes out of a stream).
// password may be empty; WrongPassword then means one is needed.
SevenZipStatus sevenZipList(const QString& archive, const QByteArray& password, QList<SevenZipItem>* out,
                            QString* error);
// Whether the password opens this member (decrypts it, writes nothing).
SevenZipStatus sevenZipTest(const QString& archive, const QString& member, const QByteArray& password,
                            QString* error);
// A new 7z archive with names and data encrypted. progress gets 0–100 and
// returns false to cancel.
bool sevenZipCreate(const QString& target, const QStringList& sources, const QByteArray& password,
                    const std::function<bool(int percent)>& progress, QString* error);

// The data of every file member, one after the other in archive order.
class SevenZipStream {
public:
    SevenZipStream();
    ~SevenZipStream();
    bool start(const QString& archive, const QByteArray& password, QString* error);
    // Blocks for up to n bytes; 0 at the end, -1 on errors (see error()).
    qint64 read(char* buf, qint64 n);
    QString error() const { return m_error; }

private:
    std::unique_ptr<QProcess> m_proc;
    QString m_error;
};

}  // namespace tandem
