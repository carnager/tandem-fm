#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "ui/MacFileIcons.h"

#include <QMimeDatabase>

namespace tandem {

QImage readMacFileIcon(const MacIconRequest& request)
{
    @autoreleasepool {
        NSWorkspace* workspace = [NSWorkspace sharedWorkspace];
        NSImage* icon = nil;
        if (!request.path.isEmpty()) {
            NSString* path = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(request.path.utf16())
                                                         length:request.path.size()];
            icon = [workspace iconForFile:path];
        } else {
            UTType* type = request.folder ? UTTypeFolder : nil;
            if (!type) {
                const QString ext = request.name.section(QLatin1Char('.'), -1);
                if (request.name.contains(QLatin1Char('.')) && !ext.isEmpty()) {
                    NSString* extension = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(ext.utf16())
                                                                       length:ext.size()];
                    type = [UTType typeWithFilenameExtension:extension];
                }
            }
            if (!type && request.executable)
                type = UTTypeUnixExecutable;
            if (!type) {
                QMimeDatabase database;
                const auto types = database.mimeTypesForFileName(request.name);
                if (!types.isEmpty()) {
                    const QByteArray mime = types.first().name().toUtf8();
                    type = [UTType typeWithMIMEType:[NSString stringWithUTF8String:mime.constData()]];
                }
            }
            icon = [workspace iconForContentType:type ?: UTTypeData];
        }
        if (!icon)
            return {};
        icon = [icon copy];  // rasterization must not mutate a shared workspace image
        NSRect rect = NSMakeRect(0, 0, 256, 256);
        CGImageRef image = [icon CGImageForProposedRect:&rect context:nil hints:nil];
        if (!image)
            return {};
        QImage result(256, 256, QImage::Format_RGBA8888_Premultiplied);
        result.fill(Qt::transparent);
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(result.bits(), result.width(), result.height(), 8,
                                                     result.bytesPerLine(), colorSpace,
                                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(colorSpace);
        if (!context)
            return {};
        CGContextDrawImage(context, CGRectMake(0, 0, result.width(), result.height()), image);
        CGContextRelease(context);
        return result;
    }
}

}  // namespace tandem
