#include "core/Shelves.h"

#include <QSettings>

#include <algorithm>

namespace tandem {

namespace {
QString key(int shelf)
{
    return QStringLiteral("shelves/%1").arg(shelf);
}
}  // namespace

Shelves& Shelves::instance()
{
    static Shelves s;
    return s;
}

Shelves::Shelves()
{
    QSettings s;
    for (int n = 1; n <= kCount; ++n)
        m_items.append(s.value(key(n)).toStringList());
    m_last = std::clamp(s.value(QStringLiteral("shelves/last"), 1).toInt(), 1, kCount);
}

QStringList Shelves::items(int shelf) const
{
    return shelf >= 1 && shelf <= kCount ? m_items[shelf - 1] : QStringList();
}

QList<int> Shelves::used() const
{
    QList<int> out;
    for (int n = 1; n <= kCount; ++n)
        if (!m_items[n - 1].isEmpty())
            out.append(n);
    return out;
}

void Shelves::store(int shelf, const QStringList& items)
{
    m_items[shelf - 1] = items;
    QSettings s;
    if (items.isEmpty())
        s.remove(key(shelf));
    else
        s.setValue(key(shelf), items);
    emit changed();
}

int Shelves::add(int shelf, const QStringList& paths)
{
    if (shelf < 1 || shelf > kCount)
        return 0;
    QStringList items = m_items[shelf - 1];
    int added = 0;
    for (const QString& p : paths)
        if (!p.isEmpty() && !items.contains(p)) {
            items.append(p);
            ++added;
        }
    m_last = shelf;
    QSettings().setValue(QStringLiteral("shelves/last"), shelf);
    if (added)
        store(shelf, items);
    return added;
}

void Shelves::remove(int shelf, const QStringList& paths)
{
    if (shelf < 1 || shelf > kCount)
        return;
    QStringList items = m_items[shelf - 1];
    for (const QString& p : paths)
        items.removeAll(p);
    if (items != m_items[shelf - 1])
        store(shelf, items);
}

void Shelves::replace(int shelf, const QStringList& from, const QStringList& to)
{
    if (shelf < 1 || shelf > kCount)
        return;
    QStringList items = m_items[shelf - 1];
    for (int i = 0; i < from.size() && i < to.size(); ++i) {
        const qsizetype at = items.indexOf(from[i]);
        if (at >= 0)
            items[at] = to[i];
    }
    if (items != m_items[shelf - 1])
        store(shelf, items);
}

void Shelves::clear(int shelf)
{
    if (shelf >= 1 && shelf <= kCount && !m_items[shelf - 1].isEmpty())
        store(shelf, {});
}

}  // namespace tandem
