#pragma once

#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QWidget>

class QScrollBar;

namespace tandem {

class FileModel;
class FileView;

// The thumbnail view: a grid drawn over the file list, sharing its model,
// cursor and marks. The list keeps the keyboard (Enter, Insert, F5 …);
// the grid draws, scrolls and takes the mouse.
class ThumbGrid : public QWidget {
    Q_OBJECT
public:
    explicit ThumbGrid(FileView* view);

    int columns() const;
    int visibleRows() const;
    void ensureVisible(int row);
    void setCellSize(int thumb);  // 96 … 256
    int cellSize() const { return m_thumb; }
    int rowAt(const QPoint& pos) const;
    QRect cellRect(int row) const;  // in widget coordinates

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    QSize cell() const;
    int spacing() const;
    void relayout();
    void watchModel();

    FileView* m_view;
    QScrollBar* m_scroll;
    QPointer<FileModel> m_model;
    int m_thumb = 150;
    QHash<QString, QPixmap> m_scaled;  // path|mtime -> thumbnail at the current size
    QSet<QString> m_onScreen;
    QPoint m_pressPos;
    bool m_maybeDrag = false;
};

}  // namespace tandem
