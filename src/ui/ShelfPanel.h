#pragma once

#include <QFrame>
#include <QPointer>

class QLabel;
class QListWidget;
class QMimeData;

namespace tandem {

// What's on one shelf, in the bottom right corner. Stays open while you
// work elsewhere, so files can be dragged onto it and out of it; closes with
// its ✕, Esc, or a second click on the shelf's chip. Opened with the keys
// (focus), it takes them: Enter copies here, Shift+Enter moves, 1–4 switch.
class ShelfPanel : public QFrame {
    Q_OBJECT
public:
    explicit ShelfPanel(QWidget* window);
    void showShelf(int shelf, bool focus = false);
    int shelf() const { return m_shelf; }
    void close();
    // Where it sits: the right edge, just above this (the bottom bar).
    void setAbove(QWidget* bar) { m_above = bar; }
    QListWidget* list() const { return m_list; }

signals:
    void showRequested(const QString& path);           // double-click
    void copyRequested(int shelf, bool move);          // into the active panel
    void dropped(int shelf, const QMimeData* data);
    void shownShelfChanged(int shelf);  // 0: closed

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void fill();
    void check();  // which items are still there, and their icons
    void place();
    void removeSelected();
    int m_shelf = 0;
    QWidget* m_above = nullptr;
    QPointer<QWidget> m_returnFocus;
    quint64 m_gen = 0;
    QLabel* m_title;
    QListWidget* m_list;
};

}  // namespace tandem
