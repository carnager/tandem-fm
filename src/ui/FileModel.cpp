#include "ui/FileModel.h"

#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QLocale>
#include <QPointer>
#include <QTimer>
#include <QRegularExpression>

#include <algorithm>
#include <dirent.h>
#include <memory>
#include <numeric>

namespace tandem {

FileModel::FileModel(QObject* parent) : QAbstractTableModel(parent) {}

int FileModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

int FileModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColCount;
}

const FileEntry* FileModel::entry(int row) const
{
    if (row < 0 || row >= int(m_entries.size()))
        return nullptr;
    return &m_entries[size_t(row)];
}

int FileModel::rowOf(const QString& name) const
{
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].name == name)
            return int(i);
    return -1;
}

QString FileModel::filePath(int row) const
{
    const FileEntry* e = entry(row);
    if (!e)
        return {};
    if (e->isParent)
        return parentPath(m_path);
    return joinPath(m_path, e->name);
}

QString FileModel::displayName(const FileEntry& e)
{
    if (e.isDir || e.isParent)
        return e.name;
    const QStringView ext = extensionOf(e.name);
    return ext.isEmpty() ? e.name : e.name.left(e.name.size() - ext.size() - 1);
}

QString FileModel::displayExt(const FileEntry& e)
{
    if (e.isDir || e.isParent)
        return {};
    return extensionOf(e.name).toString();
}

QVariant FileModel::data(const QModelIndex& index, int role) const
{
    const FileEntry* e = entry(index.row());
    if (!e)
        return {};
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case ColName:
            return displayName(*e);
        case ColExt:
            return displayExt(*e);
        case ColSize:
            if (e->isParent)
                return QString();
            if (e->isDir) {
                if (e->size >= 0)  // computed (Space)
                    return formatSize(e->size);
                const std::optional<int> n = itemCount(*e);
                return n ? plural(tr("%n item(s)", nullptr, *n), *n) : QString();
            }
            return formatSize(e->size);
        case ColTime:
            if (e->isParent || e->mtime == 0)
                return QString();
            if (relativeDates())
                return relativeDate(e->mtime, QDateTime::currentDateTime());
            return QDateTime::fromSecsSinceEpoch(e->mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        case ColAttr:
            return e->isParent ? QString() : modeString(e->mode);
        }
        break;
    case Qt::EditRole:
        if (index.column() == ColName)
            return e->name;
        break;
    case Qt::DecorationRole:
        if (index.column() == ColName)
            return IconCache::forEntry(*e);
        break;
    case Qt::TextAlignmentRole:
        if (index.column() == ColSize)
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ToolTipRole:
        if (index.column() == ColName && !e->isParent)
            return e->name;
        if (index.column() == ColTime && !e->isParent && e->mtime)
            return QLocale().toString(QDateTime::fromSecsSinceEpoch(e->mtime), QLocale::LongFormat);
        break;
    }
    return {};
}

// ------------------------------------------------------- item counts

void FileModel::setCountable(bool on, bool hidden)
{
    if (hidden != m_countHidden)
        m_counts.clear();
    m_countable = on;
    m_countHidden = hidden;
}

std::optional<int> FileModel::itemCount(const FileEntry& e) const
{
    if (!m_countable || !e.isDir || e.isParent)
        return std::nullopt;
    const auto it = m_counts.constFind(e.name);
    if (it != m_counts.cend())
        return *it < 0 ? std::nullopt : std::optional<int>(*it);
    m_counts.insert(e.name, -1);  // asked for: counted once
    m_countQueue.insert(e.name);
    if (!m_countScheduled) {
        m_countScheduled = true;
        QTimer::singleShot(0, const_cast<FileModel*>(this), [this] { runCounts(); });
    }
    return std::nullopt;
}

void FileModel::runCounts() const
{
    m_countScheduled = false;
    if (m_countQueue.isEmpty())
        return;
    const QStringList names(m_countQueue.cbegin(), m_countQueue.cend());
    m_countQueue.clear();
    QPointer<FileModel> self(const_cast<FileModel*>(this));
    const QString dir = m_path;
    const bool hidden = m_countHidden;
    const quint64 gen = m_countGen;
    ioPool()->start([self, dir, names, hidden, gen] {
        auto counts = std::make_shared<QHash<QString, int>>();
        for (const QString& name : names) {
            DIR* d = ::opendir(toNative(joinPath(dir, name)).constData());
            int n = 0;
            if (d) {
                while (dirent* de = ::readdir(d)) {
                    const char* s = de->d_name;
                    if (s[0] == '.' && (s[1] == 0 || (s[1] == '.' && s[2] == 0) || !hidden))
                        continue;
                    if (++n >= 99999)
                        break;
                }
                ::closedir(d);
            }
            counts->insert(name, d ? n : -2);  // -2: can't be read, don't ask again
        }
        postToUi([self, counts, gen] {
            if (!self || gen != self->m_countGen)
                return;  // the folder changed meanwhile
            for (auto it = counts->cbegin(); it != counts->cend(); ++it) {
                self->m_counts.insert(it.key(), it.value());
                const int row = self->rowOf(it.key());
                if (row >= 0)
                    emit self->dataChanged(self->index(row, ColSize), self->index(row, ColSize));
            }
        });
    });
}

// -------------------------------------------------------------- dates

namespace {
bool g_relativeDates = true;
}

void FileModel::setRelativeDates(bool on)
{
    g_relativeDates = on;
}

bool FileModel::relativeDates()
{
    return g_relativeDates;
}

QString FileModel::relativeDate(qint64 secs, const QDateTime& now)
{
    const QDateTime t = QDateTime::fromSecsSinceEpoch(secs);
    const qint64 ago = t.secsTo(now);
    const QLocale loc;
    if (ago >= 0 && ago < 60)
        return tr("just now");
    if (ago >= 0 && ago < 3600)
        return tr("%n min ago", nullptr, int(ago / 60));
    const QDate d = t.date(), today = now.date();
    const QString time = loc.toString(t.time(), QLocale::ShortFormat);
    if (d == today)
        return tr("Today %1").arg(time);
    if (d == today.addDays(-1))
        return tr("Yesterday %1").arg(time);
    if (d < today && d > today.addDays(-7))
        return loc.toString(d, QStringLiteral("ddd")) + QLatin1Char(' ') + time;
    if (d.year() == today.year())
        return loc.toString(d, tr("d MMM"));
    return loc.toString(d, tr("d MMM yyyy"));
}

QVariant FileModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case ColName: return tr("Name");
        case ColExt: return tr("Ext");
        case ColSize: return tr("Size");
        case ColTime: return tr("Modified");
        case ColAttr: return tr("Attributes");
        }
    }
    if (role == Qt::TextAlignmentRole)
        return QVariant::fromValue((section == ColSize ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    return {};
}

Qt::ItemFlags FileModel::flags(const QModelIndex& index) const
{
    const FileEntry* e = entry(index.row());
    if (!e)
        return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;
    if (index.column() == ColName && !e->isParent)
        f |= Qt::ItemIsEditable;
    return f;
}

bool FileModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    const FileEntry* e = entry(index.row());
    if (!e || role != Qt::EditRole || index.column() != ColName || e->isParent)
        return false;
    const QString newName = value.toString();
    if (newName.isEmpty() || newName == e->name)
        return false;
    // The rename runs off-thread; the watcher brings the new name in.
    emit renameRequested(e->name, newName);
    return false;
}

void FileModel::account(const FileEntry& e, int sign)
{
    if (e.isParent)
        return;
    if (e.isDir) {
        m_stats.dirs += sign;
        if (e.size > 0)
            m_stats.bytes += sign * e.size;
    } else {
        m_stats.files += sign;
        m_stats.bytes += sign * std::max<int64_t>(e.size, 0);
    }
    if (e.marked)
        accountMark(e, sign);
}

void FileModel::accountMark(const FileEntry& e, int sign)
{
    if (e.isDir) {
        m_stats.markedDirs += sign;
        if (e.size > 0)
            m_stats.markedBytes += sign * e.size;
    } else {
        m_stats.markedFiles += sign;
        m_stats.markedBytes += sign * std::max<int64_t>(e.size, 0);
    }
}

bool FileModel::matchesFilter(const FileEntry& e) const
{
    if (e.isParent || m_filterTerms.isEmpty())
        return true;
    for (const QRegularExpression& rx : m_filterTerms)
        if (!rx.match(e.name).hasMatch())
            return false;
    return true;
}

// Splits `all` (sorted, without "..") into visible and hidden.
void FileModel::partition(std::vector<FileEntry>& all)
{
    m_entries.clear();
    m_hidden.clear();
    m_entries.reserve(all.size() + 1);
    if (!m_path.isEmpty() && m_path != QLatin1String("/")) {
        FileEntry up;
        up.name = QStringLiteral("..");
        up.isDir = true;
        up.isParent = true;
        m_entries.push_back(std::move(up));
    }
    for (auto& e : all)
        (matchesFilter(e) ? m_entries : m_hidden).push_back(std::move(e));
}

void FileModel::setFilter(const QString& text)
{
    if (text == m_filter)
        return;
    m_filter = text;
    m_filterTerms.clear();
    for (const QString& term : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const bool wild = term.contains(QLatin1Char('*')) || term.contains(QLatin1Char('?'));
        const QString pattern = wild ? QRegularExpression::wildcardToRegularExpression(term)
                                     : QRegularExpression::escape(term);
        m_filterTerms.append(QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption));
    }
    beginResetModel();
    std::vector<FileEntry> all;
    all.reserve(m_entries.size() + m_hidden.size());
    for (auto& e : m_entries)
        if (!e.isParent)
            all.push_back(std::move(e));
    for (auto& e : m_hidden)
        all.push_back(std::move(e));
    sortEntries(all, m_sort);
    partition(all);
    m_stats = {};
    for (const auto& e : m_entries)
        account(e, +1);
    endResetModel();
    emit statsChanged();
}

void FileModel::setListing(const QString& path, std::vector<FileEntry>&& entries)
{
    beginResetModel();
    if (path != m_path) {
        m_git.clear();
        m_counts.clear();  // another folder: counts from here on belong to it
        m_countQueue.clear();
        ++m_countGen;
    }
    m_path = path;
    partition(entries);
    m_stats = {};
    for (const auto& e : m_entries)
        account(e, +1);
    endResetModel();
    emit statsChanged();
}

int FileModel::insertPosition(const FileEntry& e) const
{
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), e,
                               [this](const FileEntry& a, const FileEntry& b) { return entryLess(a, b, m_sort); });
    return int(it - m_entries.begin());
}

void FileModel::insertEntry(FileEntry&& e)
{
    const int pos = insertPosition(e);
    beginInsertRows({}, pos, pos);
    account(e, +1);
    m_entries.insert(m_entries.begin() + pos, std::move(e));
    endInsertRows();
}

void FileModel::removeRow(int row)
{
    beginRemoveRows({}, row, row);
    account(m_entries[size_t(row)], -1);
    m_entries.erase(m_entries.begin() + row);
    endRemoveRows();
}

void FileModel::applyUpdates(std::vector<EntryUpdate>&& updates)
{
    for (auto& [name, maybe] : updates) {
        const int row = rowOf(name);
        if (row < 0) {
            // Maybe it's set aside by the filter.
            auto hid = std::find_if(m_hidden.begin(), m_hidden.end(), [&](const FileEntry& h) { return h.name == name; });
            const bool marked = hid != m_hidden.end() && hid->marked;
            if (hid != m_hidden.end())
                m_hidden.erase(hid);
            if (!maybe)
                continue;
            FileEntry e = std::move(*maybe);
            e.marked = marked;
            if (matchesFilter(e))
                insertEntry(std::move(e));
            else
                m_hidden.push_back(std::move(e));
            continue;
        }
        if (!maybe) {
            removeRow(row);
            continue;
        }
        FileEntry e = std::move(*maybe);
        FileEntry& old = m_entries[size_t(row)];
        e.marked = old.marked;
        if (e.isDir && old.isDir && e.size < 0)
            e.size = old.size;  // keep a computed folder size
        const bool before = row == 0 || !entryLess(e, m_entries[size_t(row - 1)], m_sort);
        const bool after = row + 1 >= int(m_entries.size()) || !entryLess(m_entries[size_t(row + 1)], e, m_sort);
        if (before && after) {
            account(old, -1);
            account(e, +1);
            old = std::move(e);
            emit dataChanged(index(row, 0), index(row, ColCount - 1));
        } else {
            removeRow(row);
            insertEntry(std::move(e));
        }
    }
    emit statsChanged();
}

void FileModel::setGitStatus(const QHash<QString, char>& status)
{
    if (status == m_git)
        return;
    m_git = status;
    if (!m_entries.empty())
        emit dataChanged(index(0, ColName), index(int(m_entries.size()) - 1, ColName));
}

void FileModel::appendEntries(std::vector<FileEntry>&& add)
{
    if (add.empty())
        return;
    const int first = int(m_entries.size());
    beginInsertRows({}, first, first + int(add.size()) - 1);
    for (auto& e : add) {
        account(e, +1);
        m_entries.push_back(std::move(e));
    }
    endInsertRows();
    emit statsChanged();
}

void FileModel::setDirSize(const QString& name, int64_t size)
{
    const int row = rowOf(name);
    if (row < 0)
        return;
    FileEntry& e = m_entries[size_t(row)];
    account(e, -1);
    e.size = size;
    account(e, +1);
    if (m_sort.column == SortColumn::Size) {
        FileEntry copy = std::move(e);
        removeRow(row);
        insertEntry(std::move(copy));
    } else {
        emit dataChanged(index(row, ColSize), index(row, ColSize));
    }
    emit statsChanged();
}

void FileModel::sort(int column, Qt::SortOrder order)
{
    SortSpec s;
    s.column = SortColumn(std::clamp(column, 0, int(ColCount) - 1));
    s.descending = order == Qt::DescendingOrder;
    setSortSpec(s);
}

void FileModel::setSortSpec(const SortSpec& spec)
{
    m_sort = spec;
    if (m_entries.size() < 2)
        return;
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);

    std::vector<int> order(m_entries.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        return entryLess(m_entries[size_t(a)], m_entries[size_t(b)], m_sort);
    });
    std::vector<int> newRowOf(order.size());
    std::vector<FileEntry> sorted;
    sorted.reserve(m_entries.size());
    for (size_t i = 0; i < order.size(); ++i) {
        newRowOf[size_t(order[i])] = int(i);
        sorted.push_back(std::move(m_entries[size_t(order[i])]));
    }
    m_entries = std::move(sorted);

    const QModelIndexList from = persistentIndexList();
    QModelIndexList to;
    to.reserve(from.size());
    for (const QModelIndex& idx : from)
        to.append(index(newRowOf[size_t(idx.row())], idx.column()));
    changePersistentIndexList(from, to);
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void FileModel::setMarked(int row, bool on)
{
    if (row < 0 || row >= int(m_entries.size()))
        return;
    FileEntry& e = m_entries[size_t(row)];
    if (e.isParent || e.marked == on)
        return;
    e.marked = on;
    accountMark(e, on ? +1 : -1);
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
    emit statsChanged();
}

void FileModel::setMarkedNames(const QSet<QString>& names)
{
    for (auto& e : m_entries) {
        const bool on = !e.isParent && names.contains(e.name);
        if (e.marked == on)
            continue;
        e.marked = on;
        accountMark(e, on ? +1 : -1);
    }
    if (!m_entries.empty())
        emit dataChanged(index(0, 0), index(int(m_entries.size()) - 1, ColCount - 1));
    emit statsChanged();
}

void FileModel::markAll(bool on)
{
    for (auto& e : m_entries) {
        if (e.isParent || e.marked == on)
            continue;
        e.marked = on;
        accountMark(e, on ? +1 : -1);
    }
    if (!m_entries.empty())
        emit dataChanged(index(0, 0), index(int(m_entries.size()) - 1, ColCount - 1));
    emit statsChanged();
}

void FileModel::invertMarks()
{
    for (auto& e : m_entries) {
        if (e.isParent || e.isDir)
            continue;  // TC inverts files only
        e.marked = !e.marked;
        accountMark(e, e.marked ? +1 : -1);
    }
    if (!m_entries.empty())
        emit dataChanged(index(0, 0), index(int(m_entries.size()) - 1, ColCount - 1));
    emit statsChanged();
}

int FileModel::markByPattern(const QString& patterns, bool on)
{
    // "*.jpg *.png" or "*.jpg;*.png". A trailing "/" targets folders ("build/").
    QList<QRegularExpression> fileRx, dirRx;
    for (QString p : patterns.split(QRegularExpression(QStringLiteral("[;\\s]+")), Qt::SkipEmptyParts)) {
        const bool dirs = p.endsWith(QLatin1Char('/'));
        if (dirs)
            p.chop(1);
        if (p == QLatin1String("*.*"))
            p = QStringLiteral("*");
        const QRegularExpression rx(QRegularExpression::wildcardToRegularExpression(p),
                                    QRegularExpression::CaseInsensitiveOption);
        (dirs ? dirRx : fileRx).append(rx);
    }
    int n = 0;
    for (auto& e : m_entries) {
        if (e.isParent || e.marked == on)
            continue;
        const auto& list = e.isDir ? dirRx : fileRx;
        const bool match = std::any_of(list.begin(), list.end(),
                                       [&](const QRegularExpression& rx) { return rx.match(e.name).hasMatch(); });
        if (match) {
            e.marked = on;
            accountMark(e, on ? +1 : -1);
            ++n;
        }
    }
    if (n && !m_entries.empty())
        emit dataChanged(index(0, 0), index(int(m_entries.size()) - 1, ColCount - 1));
    emit statsChanged();
    return n;
}

QStringList FileModel::markedNames() const
{
    QStringList out;
    for (const auto& e : m_entries)
        if (e.marked)
            out.append(e.name);
    return out;
}

QSet<QString> FileModel::markedSet() const
{
    QSet<QString> out;
    for (const auto& e : m_entries)
        if (e.marked)
            out.insert(e.name);
    return out;
}

void FileModel::restoreMarks(const QSet<QString>& names)
{
    if (names.isEmpty())
        return;
    for (auto& e : m_entries) {
        if (!e.isParent && !e.marked && names.contains(e.name)) {
            e.marked = true;
            accountMark(e, +1);
        }
    }
    if (!m_entries.empty())
        emit dataChanged(index(0, 0), index(int(m_entries.size()) - 1, ColCount - 1));
    emit statsChanged();
}

}  // namespace tandem
