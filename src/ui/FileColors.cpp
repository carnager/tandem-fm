#include "ui/FileColors.h"

#include "core/Archive.h"
#include "core/FileEntry.h"
#include "ui/Theme.h"

#include <QHash>
#include <QMimeDatabase>
#include <QRegularExpression>

#include <sys/stat.h>

namespace tandem {

namespace {

bool g_enabled = false;

struct Compiled {
    quint64 serial = 0;  // the theme these were made from
    QHash<QString, QColor> kinds;
    QList<QPair<QRegularExpression, QColor>> names;  // "*.rs"
    QList<QPair<QString, QColor>> mimes;             // "text/x-python", "image/svg+xml"
    QHash<QString, std::optional<QColor>> bySuffix;  // cache: lower-case suffix → colour
};

Compiled& compiled()
{
    static Compiled c;
    const Theme& t = theme();
    if (c.serial != t.serial) {  // a theme was applied since
        c = {};
        c.serial = t.serial;
        const QList<QPair<QString, QColor>> rules = t.files.isEmpty() ? defaultFileColors(t) : t.files;
        for (const auto& [key, color] : rules) {
            if (key.contains(QLatin1Char('*')) || key.contains(QLatin1Char('?')))
                c.names.append({QRegularExpression::fromWildcard(key, Qt::CaseInsensitive), color});
            else if (key.contains(QLatin1Char('/')))
                c.mimes.append({key, color});
            else
                c.kinds.insert(key, color);
        }
    }
    return c;
}

QString kindOf(const QMimeType& mt, const QString& name)
{
    const QString n = mt.name();
    if (isArchiveName(name))
        return QStringLiteral("archive");
    if (n.startsWith(QLatin1String("image/")))
        return QStringLiteral("image");
    if (n.startsWith(QLatin1String("video/")))
        return QStringLiteral("video");
    if (n.startsWith(QLatin1String("audio/")))
        return QStringLiteral("audio");
    if (n == QLatin1String("application/pdf") || n == QLatin1String("application/epub+zip") ||
        n.startsWith(QLatin1String("application/vnd.oasis.opendocument")) ||
        n.startsWith(QLatin1String("application/vnd.openxmlformats")) || n == QLatin1String("application/msword") ||
        n == QLatin1String("text/markdown"))
        return QStringLiteral("document");
    return {};
}

}  // namespace

void setFileColorsEnabled(bool on)
{
    g_enabled = on;
}

bool fileColorsEnabled()
{
    return g_enabled;
}

std::optional<QColor> fileColor(const FileEntry& e)
{
    if (!g_enabled || e.isParent)
        return std::nullopt;
    Compiled& c = compiled();
    if (e.isDir)
        return c.kinds.contains(QStringLiteral("folder")) ? std::optional(c.kinds.value(QStringLiteral("folder")))
                                                          : std::nullopt;
    for (const auto& [rx, color] : std::as_const(c.names))
        if (rx.match(e.name).hasMatch())
            return color;
    const int dot = int(e.name.lastIndexOf(QLatin1Char('.')));
    const QString suffix = dot > 0 ? e.name.mid(dot + 1).toLower() : QString();
    std::optional<QColor> color;
    if (!suffix.isEmpty()) {
        auto it = c.bySuffix.constFind(suffix);
        if (it == c.bySuffix.cend()) {
            static const QMimeDatabase db;
            const QMimeType mt = db.mimeTypeForFile(e.name, QMimeDatabase::MatchExtension);
            std::optional<QColor> found;
            for (const auto& [mime, col] : std::as_const(c.mimes))
                if (mt.inherits(mime)) {
                    found = col;
                    break;
                }
            if (!found) {
                const QString kind = kindOf(mt, e.name);
                if (!kind.isEmpty() && c.kinds.contains(kind))
                    found = c.kinds.value(kind);
            }
            if (c.bySuffix.size() > 4096)
                c.bySuffix.clear();
            it = c.bySuffix.insert(suffix, found);
        }
        color = *it;
    }
    // Programs: only when the name says nothing else (shares often mark
    // every file executable).
    if (!color && (e.mode & 0111) && S_ISREG(e.mode) && c.kinds.contains(QStringLiteral("executable")))
        color = c.kinds.value(QStringLiteral("executable"));
    return color;
}

QList<QPair<QString, QColor>> defaultFileColors(const Theme& t)
{
    const auto mix = [](const QColor& a, const QColor& b) {
        return QColor((a.red() + b.red()) / 2, (a.green() + b.green()) / 2, (a.blue() + b.blue()) / 2);
    };
    return {
        {QStringLiteral("executable"), t.success},       {QStringLiteral("archive"), t.error},
        {QStringLiteral("image"), mix(t.accent, t.error)}, {QStringLiteral("video"), t.accent},
        {QStringLiteral("audio"), t.gitUntracked},       {QStringLiteral("document"), t.gitModified},
    };
}

}  // namespace tandem
