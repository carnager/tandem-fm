#pragma once

#include <QWidget>

class QHBoxLayout;
class QMimeData;
class QToolButton;

namespace tandem {

// The shelves at the bottom of the window: a chip per shelf with something
// on it ("▤ 1 · 12"), nothing otherwise. Files dropped on a chip go onto
// that shelf; dropped anywhere else here, onto the first free one.
class ShelfBar : public QWidget {
    Q_OBJECT
public:
    explicit ShelfBar(QWidget* parent);
    QToolButton* chip(int shelf) const;  // null when that shelf is empty
    void setOpenShelf(int shelf);        // its chip looks pressed

signals:
    void openRequested(int shelf);
    void dropped(int shelf, const QMimeData* data);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void rebuild();
    int shelfAt(const QPoint& pos) const;  // 0: not on a chip (a new shelf)
    void setDragging(bool on);
    QHBoxLayout* m_layout;
    QList<QToolButton*> m_chips;
    QToolButton* m_new;
    int m_open = 0;  // "＋ shelf", while something is dragged over
};

}  // namespace tandem
