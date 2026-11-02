#include "ui/viewer/Previewers.h"

#include "core/Archive.h"
#include "core/DirLister.h"
#include "core/Util.h"

#include <QDateTime>
#include <QFile>
#include <QFontDatabase>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QPainter>
#include <QPdfDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace tandem {

namespace {

constexpr int kMaxListed = 500;
constexpr int kToolTimeoutMs = 8000;

bool runTool(const QString& program, const QStringList& args, QByteArray* out)
{
    if (QStandardPaths::findExecutable(program).isEmpty())
        return false;
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(kToolTimeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return false;
    }
    *out = p.readAllStandardOutput();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// Case-insensitive tag lookup (containers differ: "title" vs "TITLE").
QString tag(const QJsonObject& tags, const char* name)
{
    for (auto it = tags.begin(); it != tags.end(); ++it)
        if (it.key().compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
            return it.value().toString();
    return {};
}

QString clock(double seconds)
{
    const qint64 s = qint64(seconds + 0.5);
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QJsonObject probe(const QString& path)
{
    QByteArray out;
    if (!runTool(QStringLiteral("ffprobe"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-print_format"),
                                              QStringLiteral("json"), QStringLiteral("-show_format"),
                                              QStringLiteral("-show_streams"), path},
                 &out))
        return {};
    return QJsonDocument::fromJson(out).object();
}

QImage grabFrame(const QString& path, double at, int maxWidth)
{
    QByteArray png;
    const QStringList args = {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-ss"),
                              QString::number(at, 'f', 2), QStringLiteral("-i"), path, QStringLiteral("-frames:v"),
                              QStringLiteral("1"), QStringLiteral("-vf"),
                              QStringLiteral("scale='min(%1,iw)':-2").arg(maxWidth), QStringLiteral("-f"),
                              QStringLiteral("image2pipe"), QStringLiteral("-c:v"), QStringLiteral("png"),
                              QStringLiteral("-")};
    if (!runTool(QStringLiteral("ffmpeg"), args, &png))
        return {};
    return QImage::fromData(png, "PNG");
}

QImage readImage(const QString& path, QSize maxSize, QString* error, QString* info)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);  // EXIF rotation
    reader.setAllocationLimit(1024);
    const QSize full = reader.size();
    if (maxSize.isValid() && full.isValid() &&
        (full.width() > maxSize.width() || full.height() > maxSize.height()))
        reader.setScaledSize(full.scaled(maxSize, Qt::KeepAspectRatio));  // decode small: much faster
    const QImage img = reader.read();
    if (img.isNull())
        *error = reader.errorString();
    if (full.isValid())
        *info = QStringLiteral("%1 × %2").arg(full.width()).arg(full.height());
    return img;
}

}  // namespace

PreviewKind classifyPreview(const QString& path, const QString& mime, bool isDir, bool isSpecial, const QByteArray& head)
{
    if (isDir)
        return PreviewKind::Folder;
    if (isSpecial)
        return PreviewKind::Special;
    if (isArchiveName(baseName(path)))
        return PreviewKind::Archive;
    if (mime == QLatin1String("application/pdf"))
        return PreviewKind::Pdf;
    if (mime.startsWith(QLatin1String("video/")))
        return PreviewKind::Video;
    if (mime.startsWith(QLatin1String("audio/")))
        return PreviewKind::Audio;
    if (mime.startsWith(QLatin1String("font/")) || mime == QLatin1String("application/x-font-ttf") ||
        mime == QLatin1String("application/x-font-otf") || mime == QLatin1String("application/vnd.ms-opentype"))
        return PreviewKind::Font;
    if (QImageReader::supportedMimeTypes().contains(mime.toLatin1()))
        return PreviewKind::Image;
    if (mime == QLatin1String("text/markdown"))
        return PreviewKind::Markdown;
    if (mime == QLatin1String("application/json"))
        return PreviewKind::Json;
    return head.contains('\0') ? PreviewKind::Binary : PreviewKind::Text;
}

bool hasRichView(PreviewKind k)
{
    switch (k) {
    case PreviewKind::Image:
    case PreviewKind::Pdf:
    case PreviewKind::Video:
    case PreviewKind::Audio:
    case PreviewKind::Font:
    case PreviewKind::Markdown:
    case PreviewKind::Folder:
    case PreviewKind::Archive:
        return true;
    default:
        return false;
    }
}

QString richViewLabel(PreviewKind k)
{
    switch (k) {
    case PreviewKind::Image: return QObject::tr("Image");
    case PreviewKind::Pdf: return QObject::tr("Page");
    case PreviewKind::Video: return QObject::tr("Video");
    case PreviewKind::Audio: return QObject::tr("Audio");
    case PreviewKind::Font: return QObject::tr("Font");
    case PreviewKind::Markdown: return QObject::tr("Rendered");
    case PreviewKind::Folder:
    case PreviewKind::Archive: return QObject::tr("Contents");
    default: return QObject::tr("Preview");
    }
}

RichPreview produceRichPreview(const QString& path, PreviewKind kind, QSize maxSize)
{
    RichPreview r;
    const int maxWidth = maxSize.isValid() ? maxSize.width() : 1920;
    switch (kind) {
    case PreviewKind::Image:
        r.image = readImage(path, maxSize, &r.error, &r.info);
        break;

    case PreviewKind::Pdf: {
        QPdfDocument doc;
        if (doc.load(path) != QPdfDocument::Error::None) {
            r.error = QObject::tr("Cannot read this PDF (maybe encrypted)");
            break;
        }
        const QSizeF pt = doc.pagePointSize(0);
        const qreal scale = qMin<qreal>(3.0, maxWidth / qMax<qreal>(1, pt.width()));
        r.image = doc.render(0, (pt * scale).toSize());
        QStringList info;
        info << plural(QObject::tr("%n page(s)", nullptr, doc.pageCount()), doc.pageCount());
        const QString title = doc.metaData(QPdfDocument::MetaDataField::Title).toString();
        const QString author = doc.metaData(QPdfDocument::MetaDataField::Author).toString();
        if (!title.isEmpty())
            info << title;
        if (!author.isEmpty())
            info << author;
        r.info = info.join(QStringLiteral("  ·  "));
        break;
    }

    case PreviewKind::Video: {
        const QJsonObject p = probe(path);
        if (p.isEmpty()) {
            r.error = QObject::tr("Install ffmpeg for video previews");
            break;
        }
        const QJsonObject fmt = p.value(QStringLiteral("format")).toObject();
        const double duration = fmt.value(QStringLiteral("duration")).toString().toDouble();
        QStringList info;
        for (const QJsonValue& v : p.value(QStringLiteral("streams")).toArray()) {
            const QJsonObject s = v.toObject();
            if (s.value(QStringLiteral("codec_type")).toString() == QLatin1String("video") && info.isEmpty())
                info << QStringLiteral("%1 × %2").arg(s.value(QStringLiteral("width")).toInt())
                            .arg(s.value(QStringLiteral("height")).toInt())
                     << s.value(QStringLiteral("codec_name")).toString();
        }
        if (duration > 0)
            info << clock(duration);
        if (const double br = fmt.value(QStringLiteral("bit_rate")).toString().toDouble(); br > 0)
            info << (br >= 1e6 ? QStringLiteral("%1 Mbit/s").arg(br / 1e6, 0, 'f', 1)
                               : QStringLiteral("%1 kbit/s").arg(int(br / 1e3)));
        r.info = info.join(QStringLiteral("  ·  "));
        // A frame a bit into the video says more than the (often black) first one.
        r.image = grabFrame(path, duration > 0 ? qMin(duration * 0.1, 60.0) : 0, maxWidth);
        if (r.image.isNull() && duration > 0)
            r.image = grabFrame(path, 0, maxWidth);
        break;
    }

    case PreviewKind::Audio: {
        const QJsonObject p = probe(path);
        if (p.isEmpty()) {
            r.error = QObject::tr("Install ffmpeg for audio details");
            break;
        }
        const QJsonObject fmt = p.value(QStringLiteral("format")).toObject();
        QJsonObject tags = fmt.value(QStringLiteral("tags")).toObject();
        QStringList lines;
        QString codec, rate;
        for (const QJsonValue& v : p.value(QStringLiteral("streams")).toArray()) {
            const QJsonObject s = v.toObject();
            if (s.value(QStringLiteral("codec_type")).toString() != QLatin1String("audio"))
                continue;
            codec = s.value(QStringLiteral("codec_name")).toString();
            rate = QStringLiteral("%1 kHz").arg(s.value(QStringLiteral("sample_rate")).toString().toDouble() / 1000.0, 0, 'f', 1);
            if (tags.isEmpty())
                tags = s.value(QStringLiteral("tags")).toObject();  // ogg/opus keep tags on the stream
        }
        const QString title = tag(tags, "title"), artist = tag(tags, "artist"), album = tag(tags, "album");
        const QString date = tag(tags, "date").left(4);
        if (!artist.isEmpty() || !title.isEmpty())
            lines << QStringList{artist, title}.filter(QRegularExpression(QStringLiteral("."))).join(QStringLiteral(" — "));
        if (!album.isEmpty())
            lines << (date.isEmpty() ? album : QStringLiteral("%1 (%2)").arg(album, date));
        QStringList tech;
        if (const double d = fmt.value(QStringLiteral("duration")).toString().toDouble(); d > 0)
            tech << clock(d);
        if (!codec.isEmpty())
            tech << codec;
        if (!rate.isEmpty())
            tech << rate;
        if (const double br = fmt.value(QStringLiteral("bit_rate")).toString().toDouble(); br > 0)
            tech << QStringLiteral("%1 kbit/s").arg(int(br / 1000));
        lines << tech.join(QStringLiteral("  ·  "));
        r.info = lines.join(QLatin1Char('\n'));
        // Embedded cover art (attached picture stream), if any.
        QByteArray png;
        if (runTool(QStringLiteral("ffmpeg"),
                    {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path, QStringLiteral("-an"),
                     QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-f"), QStringLiteral("image2pipe"),
                     QStringLiteral("-c:v"), QStringLiteral("png"), QStringLiteral("-")},
                    &png))
            r.image = QImage::fromData(png, "PNG");
        break;
    }

    case PreviewKind::Font: {
        QFile f(path);
        if (f.size() > 64 * 1024 * 1024 || !f.open(QIODevice::ReadOnly))
            r.error = QObject::tr("Cannot read this font");
        else
            r.fontData = f.readAll();
        break;
    }

    case PreviewKind::Markdown: {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            r.error = f.errorString();
        else
            r.markdown = QString::fromUtf8(f.read(2 * 1024 * 1024));
        break;
    }

    case PreviewKind::Folder: {
        ListOptions opt;
        opt.showHidden = false;
        ListResult l = listDirectory(path, opt);
        if (l.error) {
            r.error = errnoString(l.error);
            break;
        }
        int files = 0, dirs = 0;
        qint64 bytes = 0;
        for (const FileEntry& e : l.entries) {
            (e.isDir ? dirs : files) += 1;
            if (!e.isDir)
                bytes += qMax<qint64>(0, e.size);
            if (r.items.size() < kMaxListed)
                r.items.append({e.name, e.isDir, e.size, e.mode});
        }
        r.info = QObject::tr("%1 folders, %2 files  ·  %3").arg(dirs).arg(files).arg(formatSize(bytes));
        break;
    }

    case PreviewKind::Archive: {
        QString error;
        const auto idx = loadArchiveIndex(path, &error);
        if (!idx) {
            r.error = error;
            break;
        }
        qint64 total = 0;
        int files = 0;
        for (const ArchiveEntry& e : idx->entries)
            if (!e.isDir) {
                total += e.size;
                ++files;
            }
        QList<int> top = idx->children.value(QString());
        std::sort(top.begin(), top.end(), [&](int a, int b) {
            const ArchiveEntry& x = idx->entries[size_t(a)];
            const ArchiveEntry& y = idx->entries[size_t(b)];
            if (x.isDir != y.isDir)
                return x.isDir;
            return QString::compare(x.name, y.name, Qt::CaseInsensitive) < 0;
        });
        for (int i : top) {
            const ArchiveEntry& e = idx->entries[size_t(i)];
            if (r.items.size() >= kMaxListed)
                break;
            r.items.append({e.name, e.isDir, e.size, e.mode});
        }
        r.info = plural(QObject::tr("%n file(s)", nullptr, files), files) + QStringLiteral("  ·  ") +
                 QObject::tr("%1 unpacked").arg(formatSize(total));
        break;
    }

    default:
        break;
    }
    return r;
}

QImage renderFontSample(const QByteArray& fontData, QString* familyOut)
{
    const int id = QFontDatabase::addApplicationFontFromData(fontData);
    if (id < 0)
        return {};
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    const QString family = families.value(0);
    *familyOut = family;
    QImage img(1400, 900, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    int y = 20;
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setPen(QColor(0x22, 0x22, 0x22));
        auto line = [&](int px, const QString& text) {
            QFont f(family);
            f.setPixelSize(px);
            p.setFont(f);
            const QFontMetrics fm(f);
            y += fm.ascent();
            p.drawText(30, y, text);
            y += fm.descent() + px / 3;
        };
        line(72, family);
        line(40, QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
        line(40, QStringLiteral("abcdefghijklmnopqrstuvwxyz"));
        line(40, QStringLiteral("0123456789 !?&@#%(){}[]"));
        line(28, QStringLiteral("The quick brown fox jumps over the lazy dog."));
        line(18, QStringLiteral("Zwölf Boxkämpfer jagen Viktor quer über den großen Sylter Deich."));
        line(13, QStringLiteral("Sphinx of black quartz, judge my vow. 0O 1lI"));
    }  // the painter must be done before the image is cropped or the font removed
    QFontDatabase::removeApplicationFont(id);
    return img.copy(0, 0, 1400, qMin(900, y + 20));
}

QThreadPool* mediaPool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(2);
        return p;
    }();
    return pool;
}

PreviewCache& PreviewCache::instance()
{
    static PreviewCache c;
    return c;
}

QString PreviewCache::key(const QString& path, qint64 mtime, qint64 size, QSize maxSize)
{
    return QStringLiteral("%1|%2|%3|%4x%5").arg(path).arg(mtime).arg(size).arg(maxSize.width()).arg(maxSize.height());
}

std::shared_ptr<const RichPreview> PreviewCache::get(const QString& key)
{
    QMutexLocker lock(&m_mutex);
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].first == key) {
            auto p = m_items[i].second;
            m_items.move(i, m_items.size() - 1);
            return p;
        }
    }
    return nullptr;
}

void PreviewCache::put(const QString& key, std::shared_ptr<const RichPreview> preview)
{
    QMutexLocker lock(&m_mutex);
    m_items.append({key, std::move(preview)});
    while (m_items.size() > 48)
        m_items.removeFirst();
}

bool PreviewCache::beginLoad(const QString& key)
{
    QMutexLocker lock(&m_mutex);
    if (m_inFlight.contains(key))
        return false;
    for (const auto& item : std::as_const(m_items))
        if (item.first == key)
            return false;
    m_inFlight.insert(key);
    return true;
}

void PreviewCache::endLoad(const QString& key)
{
    QMutexLocker lock(&m_mutex);
    m_inFlight.remove(key);
}

}  // namespace tandem
