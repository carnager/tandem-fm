#pragma once

#include <QObject>
#include <QStringList>

namespace tandem {

// Shelves 1–4: files and folders put aside from anywhere, to copy or move
// somewhere later. Kept in state.toml, so they last until cleared.
class Shelves : public QObject {
    Q_OBJECT
public:
    static constexpr int kCount = 4;
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
    // Reads them from the settings again. Items on shelves 5–9 (there used
    // to be nine) move onto an empty shelf, or onto the last one.
    void reload();

signals:
    void changed();

private:
    Shelves();
    void store(int shelf, const QStringList& items);
    QList<QStringList> m_items;
    int m_last = 1;
};

}  // namespace tandem
