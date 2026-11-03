#include "core/Checksums.h"

#include <QFile>
#include <QRegularExpression>

namespace tandem {

QList<ChecksumAlgo> checksumAlgos()
{
    return {
        {QCryptographicHash::Sha256, QStringLiteral("SHA-256"), QStringLiteral(".sha256"), 64},
        {QCryptographicHash::Sha1, QStringLiteral("SHA-1"), QStringLiteral(".sha1"), 40},
        {QCryptographicHash::Md5, QStringLiteral("MD5"), QStringLiteral(".md5"), 32},
        {QCryptographicHash::Sha512, QStringLiteral("SHA-512"), QStringLiteral(".sha512"), 128},
        {QCryptographicHash::Blake2b_512, QStringLiteral("BLAKE2"), QStringLiteral(".b2"), 128},
    };
}

std::optional<ChecksumAlgo> algoForFile(const QString& file)
{
    const QString n = file.toLower();
    for (const ChecksumAlgo& a : checksumAlgos())
        if (n.endsWith(a.extension) || n.endsWith(a.extension + QStringLiteral("sum")) ||
            n.endsWith(a.extension.mid(1) + QStringLiteral("sums")))  // SHA256SUMS
            return a;
    return std::nullopt;
}

std::optional<ChecksumAlgo> algoForHexLength(int length)
{
    for (const ChecksumAlgo& a : checksumAlgos())
        if (a.hexLength == length && a.algo != QCryptographicHash::Blake2b_512)  // 128: SHA-512 is likelier
            return a;
    return std::nullopt;
}

bool isChecksumFile(const QString& name)
{
    return algoForFile(name).has_value();
}

QString fileChecksum(const QString& path, QCryptographicHash::Algorithm algo, const std::atomic<bool>* cancel,
                     const std::function<void(int64_t)>& progress)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(algo);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    int64_t done = 0;
    for (;;) {
        if (cancel && cancel->load())
            return {};
        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0)
            return {};
        if (n == 0)
            break;
        h.addData(QByteArrayView(buf.constData(), n));
        done += n;
        if (progress)
            progress(done);
    }
    return QString::fromLatin1(h.result().toHex());
}

QList<ChecksumLine> readChecksumFile(const QString& path, QString* algoName)
{
    QList<ChecksumLine> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    static const QRegularExpression gnu(QStringLiteral("^\\\\?([0-9a-fA-F]{32,128})\\s[ *](.+)$"));
    static const QRegularExpression bsd(QStringLiteral("^([A-Za-z0-9-]+)\\s*\\((.+)\\)\\s*=\\s*([0-9a-fA-F]{32,128})$"));
    for (QByteArray raw : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (const auto m = gnu.match(line); m.hasMatch()) {
            out.append({m.captured(1).toLower(), m.captured(2)});
        } else if (const auto b = bsd.match(line); b.hasMatch()) {
            out.append({b.captured(3).toLower(), b.captured(2)});
            if (algoName)
                *algoName = b.captured(1);
        }
    }
    return out;
}

QString checksumLines(const QList<ChecksumLine>& lines)
{
    QString out;
    for (const ChecksumLine& l : lines)
        out += l.hash + QStringLiteral("  ") + l.name + QLatin1Char('\n');
    return out;
}

}  // namespace tandem
