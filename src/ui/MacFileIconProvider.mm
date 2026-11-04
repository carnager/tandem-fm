#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "ui/MacFileIcons.h"

#include <QApplication>
#include <QIconEngine>
#include "core/Sorting.h"
#include <QPainter>
#include <QPixmapCache>
#include <QStyle>
#include <QStyleOption>
#include <QThread>
#include <atomic>
#include <sys/stat.h>

namespace tandem {
namespace {

class NativeIcon : public QIconEngine {
public:
    explicit NativeIcon(NSImage* image) : m_image([image copy])
    {
        static std::atomic<quint64> next{0};
        m_key = QStringLiteral("tandem-native-%1").arg(++next);
    }
    QIconEngine* clone() const override { return new NativeIcon(*this); }
    bool isNull() override { return m_image == nil; }
    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        painter->drawPixmap(rect, render(rect.size(), mode, state, painter->device()->devicePixelRatioF()));
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return render(size, mode, state, 1);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
        return render(size / scale, mode, state, scale);
#else
        return render(size, mode, state, scale);
#endif
    }

private:
    QPixmap render(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale)
    {
        Q_ASSERT(QThread::currentThread() == qApp->thread());
        if (size.isEmpty() || scale <= 0 || !m_image)
            return {};
        const QSize pixels = size * scale;
        const QString key = m_key + QStringLiteral("-%1-%2-%3-%4-%5")
            .arg(pixels.width()).arg(pixels.height()).arg(scale).arg(mode).arg(state);
        QPixmap pixmap;
        if (QPixmapCache::find(key, &pixmap))
            return pixmap;
        @autoreleasepool {
            NSRect rect = NSMakeRect(0, 0, pixels.width(), pixels.height());
            CGImageRef image = [m_image CGImageForProposedRect:&rect context:nil hints:nil];
            if (!image)
                return {};
            QImage result(pixels, QImage::Format_RGBA8888_Premultiplied);
            result.fill(Qt::transparent);
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            CGContextRef context = CGBitmapContextCreate(result.bits(), result.width(), result.height(), 8,
                result.bytesPerLine(), space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(space);
            if (!context)
                return {};
            CGContextDrawImage(context, CGRectMake(0, 0, pixels.width(), pixels.height()), image);
            CGContextRelease(context);
            pixmap = QPixmap::fromImage(result);
            pixmap.setDevicePixelRatio(scale);
            if (mode != QIcon::Normal) {
                QStyleOption option;
                option.palette = QApplication::palette();
                pixmap = QApplication::style()->generatedIconPixmap(mode, pixmap, &option);
            }
        }
        QPixmapCache::insert(key, pixmap);
        return pixmap;
    }
    NSImage* __strong m_image;
    QString m_key;
};

QIcon nativeIcon(NSImage* image)
{
    return image ? QIcon(new NativeIcon(image)) : QIcon();
}

}  // namespace

QIcon macTypeIcon(const FileEntry& entry)
{
    Q_ASSERT(QThread::currentThread() == qApp->thread());
    @autoreleasepool {
        UTType* type = entry.isDir ? UTTypeFolder : nil;
        if (!type) {
            const QString ext = extensionOf(entry.name).toString().section(QLatin1Char('.'), -1);
            if (!ext.isEmpty()) {
                NSString* extension = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(ext.utf16())
                                                                   length:ext.size()];
                type = [UTType typeWithFilenameExtension:extension];
            }
        }
        if (!type && (entry.mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
            type = UTTypeUnixExecutable;
        return nativeIcon([[NSWorkspace sharedWorkspace] iconForContentType:type ?: UTTypeData]);
    }
}

QIcon macPathIcon(const QString& path)
{
    @autoreleasepool {
        NSString* nativePath = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(path.utf16())
                                                           length:path.size()];
        return nativeIcon([[NSWorkspace sharedWorkspace] iconForFile:nativePath]);
    }
}

}  // namespace tandem
