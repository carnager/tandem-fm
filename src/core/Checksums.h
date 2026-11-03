#pragma once

#include <QCryptographicHash>
#include <QList>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

namespace tandem {

// Checksum files as sha256sum & co. write them ("<hash>  <name>", also the
// BSD form "SHA256 (name) = hash").
struct ChecksumAlgo {
    QCryptographicHash::Algorithm algo;
    QString name;       // "SHA-256"
    QString extension;  // ".sha256"
    int hexLength;
};
QList<ChecksumAlgo> checksumAlgos();
std::optional<ChecksumAlgo> algoForFile(const QString& checksumFile);  // by extension
std::optional<ChecksumAlgo> algoForHexLength(int length);

// Hex, or empty on read errors / cancel. progress gets bytes read so far.
QString fileChecksum(const QString& path, QCryptographicHash::Algorithm algo, const std::atomic<bool>* cancel = nullptr,
                     const std::function<void(int64_t)>& progress = {});

struct ChecksumLine {
    QString hash;  // lower-case hex
    QString name;  // as written (relative to the checksum file's folder)
};
QList<ChecksumLine> readChecksumFile(const QString& path, QString* algoName = nullptr);
bool isChecksumFile(const QString& name);
// "<hash>  <name>\n" lines.
QString checksumLines(const QList<ChecksumLine>& lines);

}  // namespace tandem
