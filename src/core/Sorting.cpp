#include "core/Sorting.h"

#include <QCollator>
#include <QLocale>

#include <algorithm>

namespace tandem {

// Collation follows POSIX precedence (LC_ALL > LC_COLLATE > LANG). In the "C"
// locale QCollator silently drops numeric and case-insensitive ordering, so
// fall back to English rules there.
static QLocale collationLocale()
{
    static const QLocale loc = [] {
        for (const char* var : {"LC_ALL", "LC_COLLATE", "LANG"}) {
            QString v = qEnvironmentVariable(var);
            v = v.section(QLatin1Char('.'), 0, 0).section(QLatin1Char('@'), 0, 0);
            if (v.isEmpty())
                continue;
            QLocale l(v);
            if (l.language() != QLocale::C)
                return l;
            break;  // explicitly C/POSIX
        }
        QLocale sys;
        return sys.language() == QLocale::C ? QLocale(QLocale::English, QLocale::UnitedStates) : sys;
    }();
    return loc;
}

static QCollator& threadCollator()
{
    thread_local QCollator c = [] {
        QCollator col{collationLocale()};
        col.setNumericMode(true);
        col.setCaseSensitivity(Qt::CaseInsensitive);
        return col;
    }();
    return c;
}

QCollatorSortKey makeSortKey(const QString& name)
{
    return threadCollator().sortKey(name);
}

QStringView extensionOf(const QString& name)
{
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return {};
    // Compressed tarballs: "tar.gz" is one extension.
    const qsizetype prev = name.lastIndexOf(QLatin1Char('.'), dot - 1);
    if (prev > 0 && QStringView(name).mid(prev + 1, dot - prev - 1).compare(QLatin1String("tar"), Qt::CaseInsensitive) == 0)
        return QStringView(name).mid(prev + 1);
    return QStringView(name).mid(dot + 1);
}

template <typename T>
static int cmp(T a, T b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int compareNames(const FileEntry& a, const FileEntry& b)
{
    if (a.key && b.key) {
        const int c = a.key->compare(*b.key);
        if (c)
            return c;
    } else {
        const int c = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        if (c)
            return c;
    }
    return QString::compare(a.name, b.name, Qt::CaseSensitive);  // total order
}

bool entryLess(const FileEntry& a, const FileEntry& b, const SortSpec& spec)
{
    if (a.isParent != b.isParent)
        return a.isParent;
    if (a.isDir != b.isDir)
        return a.isDir;

    int c = 0;
    switch (spec.column) {
    case SortColumn::Name:
        break;
    case SortColumn::Ext:
        if (!a.isDir)
            c = extensionOf(a.name).compare(extensionOf(b.name), Qt::CaseInsensitive);
        break;
    case SortColumn::Size:
        c = cmp(a.size, b.size);
        break;
    case SortColumn::Time:
        c = cmp(a.mtime, b.mtime);
        break;
    case SortColumn::Created:
    case SortColumn::Added: {
        const auto& x = spec.column == SortColumn::Created ? a.createdTime : a.addedTime;
        const auto& y = spec.column == SortColumn::Created ? b.createdTime : b.addedTime;
        // Unknown dates remain last in either direction, within files/directories.
        if (x.has_value() != y.has_value())
            return x.has_value();
        if (x) c = cmp(*x, *y);
        break;
    }
    case SortColumn::Attr:
        c = cmp(a.mode, b.mode);
        break;
    }
    if (c == 0)
        c = compareNames(a, b);
    return spec.descending ? c > 0 : c < 0;
}

void sortEntries(std::vector<FileEntry>& entries, const SortSpec& spec)
{
    std::sort(entries.begin(), entries.end(),
              [&spec](const FileEntry& a, const FileEntry& b) { return entryLess(a, b, spec); });
}

}  // namespace tandem
