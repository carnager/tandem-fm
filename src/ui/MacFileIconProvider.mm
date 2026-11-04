#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "ui/MacFileIcons.h"

#include "core/Sorting.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QStyle>
#include <QStyleOption>
#include <QThread>
#include <atomic>
#include <sys/stat.h>

namespace tandem {
namespace {

quint64 g_drawGeneration = 0;  // UI thread only; part of every drawn pixmap's key

// Draws `image` at `pixels`. AppKit may render the icon only now, so this is
// UI-thread work for type icons and worker work for path icons.
QImage drawNative(NSImage* image, const QSize& pixels)
{
    @autoreleasepool {
        NSRect rect = NSMakeRect(0, 0, pixels.width(), pixels.height());
        CGImageRef cg = [image CGImageForProposedRect:&rect context:nil hints:nil];
        if (!cg)
            return {};
        QImage result(pixels, QImage::Format_RGBA8888_Premultiplied);
        result.fill(Qt::transparent);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(result.bits(), result.width(), result.height(), 8,
            result.bytesPerLine(), space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(space);
        if (!context)
            return {};
        CGContextDrawImage(context, CGRectMake(0, 0, pixels.width(), pixels.height()), cg);
        CGContextRelease(context);
        return result;
    }
}

// Draws at whatever size and scale is asked for, cached in QPixmapCache.
class DrawnIcon : public QIconEngine {
public:
    DrawnIcon()
    {
        static std::atomic<quint64> next{0};
        m_key = QStringLiteral("tandem-native-%1").arg(++next);
    }
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

protected:
    virtual QImage draw(const QSize& pixels) = 0;

private:
    QPixmap render(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale)
    {
        Q_ASSERT(QThread::currentThread() == qApp->thread());
        if (size.isEmpty() || scale <= 0)
            return {};
        const QSize pixels = size * scale;
        const QString key = m_key + QStringLiteral("-%1-%2-%3-%4-%5-%6").arg(g_drawGeneration)
            .arg(pixels.width()).arg(pixels.height()).arg(scale).arg(mode).arg(state);
        QPixmap pixmap;
        if (QPixmapCache::find(key, &pixmap))
            return pixmap;
        const QImage image = draw(pixels);
        if (image.isNull())
            return {};
        pixmap = QPixmap::fromImage(image);
        pixmap.setDevicePixelRatio(scale);
        if (mode != QIcon::Normal) {
            QStyleOption option;
            option.palette = QApplication::palette();
            pixmap = QApplication::style()->generatedIconPixmap(mode, pixmap, &option);
        }
        QPixmapCache::insert(key, pixmap);
        return pixmap;
    }
    QString m_key;
};

// A type icon: AppKit draws it on the UI thread, in the app's appearance.
class NativeIcon : public DrawnIcon {
public:
    explicit NativeIcon(NSImage* image) : m_image([image copy]) {}
    QIconEngine* clone() const override { return new NativeIcon(*this); }
    bool isNull() override { return m_image == nil; }

protected:
    QImage draw(const QSize& pixels) override
    {
        NSAppearance* appearance = NSApp ? NSApp.effectiveAppearance : nil;
        if (!appearance)
            return drawNative(m_image, pixels);
        NSImage* source = m_image;
        const QSize size = pixels;
        __block QImage image;
        [appearance performAsCurrentDrawingAppearance:^{ image = drawNative(source, size); }];
        return image;
    }

private:
    NSImage* __strong m_image;
};

// A path icon, drawn on the worker: only plain pixels reach the UI thread,
// so painting never waits for AppKit to load or render the icon.
class PixelIcon : public DrawnIcon {
public:
    explicit PixelIcon(QList<QImage> images) : m_images(std::move(images)) {}
    QIconEngine* clone() const override { return new PixelIcon(*this); }
    bool isNull() override { return m_images.isEmpty(); }

protected:
    QImage draw(const QSize& pixels) override
    {
        if (m_images.isEmpty())
            return {};
        const QImage* best = &m_images.last();  // sorted small to large
        for (const QImage& image : m_images) {
            if (image.width() >= pixels.width() && image.height() >= pixels.height()) {
                best = &image;
                break;
            }
        }
        return best->size() == pixels ? *best : best->scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

private:
    QList<QImage> m_images;
};

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
        NSImage* image = [[NSWorkspace sharedWorkspace] iconForContentType:type ?: UTTypeData];
        return image ? QIcon(new NativeIcon(image)) : QIcon();
    }
}

QIcon macPathIcon(const QString& path)
{
    @autoreleasepool {
        NSString* nativePath = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(path.utf16())
                                                           length:path.size()];
        NSImage* image = [[NSWorkspace sharedWorkspace] iconForFile:nativePath];
        if (!image)
            return {};
        // List rows at 1x and 2x, and the grid up to 96 px at 2x.
        QList<QImage> images;
        for (int side : {32, 64, 128, 256}) {
            QImage drawn = drawNative(image, QSize(side, side));
            if (!drawn.isNull())
                images.append(std::move(drawn));
        }
        return images.isEmpty() ? QIcon() : QIcon(new PixelIcon(std::move(images)));
    }
}

void macForgetDrawnIcons()
{
    Q_ASSERT(QThread::currentThread() == qApp->thread());
    ++g_drawGeneration;
}

}  // namespace tandem
