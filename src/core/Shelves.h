#pragma once

#include <QObject>
#include <QStringList>

namespace tandem {

// Shelves 1–9: files and folders put aside from anywhere, to copy or move
// somewhere later. Kept in state.toml, so they last until cleared.
class Shelves : public QObject {
    Q_OBJECT
public:
    static constexpr int kCount = 9;
    static Shelves& instance();

    QStringList items(int shelf) const;
    QList<int> used() const;  // the shelves with something on them
    // Adds what isn't there yet, at the end. Returns how many were new.
    int add(int shelf, const QStringList& paths);
    void remove(int shelf, const QStringList& paths);
    // After a move: the same items, now at their new paths.
    void replace(int shelf, const QStringList& from, const QStringList& to);
    void clear(int shelf);
    int lastUsed() const { return m_last; }  // where Enter puts things

signals:
    void changed();

private:
    Shelves();
    void store(int shelf, const QStringList& items);
    QList<QStringList> m_items;
    int m_last = 1;
};

}  // namespace tandem
