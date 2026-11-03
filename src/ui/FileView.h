#pragma once

#include <QStyledItemDelegate>
#include <QTreeView>
#include <QUrl>

namespace tandem {

class FileModel;
class ThumbGrid;

class FileDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit FileDelegate(QTreeView* view);
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    void setEditorData(QWidget* editor, const QModelIndex& idx) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;

private:
    QTreeView* m_view;
};

class FileView : public QTreeView {
    Q_OBJECT
public:
    explicit FileView(QWidget* parent = nullptr);

    FileModel* fileModel() const;
    void setActive(bool active);
    bool isActive() const { return m_active; }
    int currentRow() const { return currentIndex().row(); }
    void setVimKeys(bool on) { m_vim = on; }
    // Header state as the user configured it (ignores columns hidden for width).
    QByteArray userHeaderState();
    void fitColumns();
    void setCurrentRow(int row, bool center = false);
    // Thumbnails in a grid instead of the detailed list.
    void setThumbnails(bool on);
    bool thumbnails() const;
    ThumbGrid* thumbGrid() const { return m_grid; }

signals:
    void openRequested();               // Enter, double click
    void parentRequested();             // Backspace
    void switchPanelRequested();        // Tab
    void escapePressed();
    void quickSearchRequested(const QString& firstText);
    void spaceMarkRequested(int row);   // Space: mark + folder size
    void markGroupRequested(int mode);  // Num+ (1), Num- (-1), Num* (0)
    void historyRequested(int step);    // mouse back/forward buttons
    void focused();
    void cursorMoved();
    void searchNextRequested(int direction);  // vim n / N
    void paletteRequested();                  // vim :
    void bookmarkJumpRequested(const QString& keys);  // vim ' and g<key>
    void bookmarkAssignRequested();                   // vim m
    void commandRequested();                          // vim !
    void dropRequested(const QList<QUrl>& urls, const QString& targetDir, Qt::DropAction action);
    void dragPathsRequested(QStringList& out);  // panel fills marked/current paths
    void contextMenuRequested(const QPoint& globalPos, int row);  // row -1: empty area

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyboardSearch(const QString&) override {}
    void focusInEvent(QFocusEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void drawRow(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    friend class ThumbGrid;
    void moveCursorBy(int delta);
    QString dropTargetFor(const QPoint& pos, int* row) const;
    void startFileDrag();
    void showHeaderMenu(const QPoint& pos);

    bool m_active = false;
    int m_dropRow = -1;
    QPoint m_pressPos;
    bool m_maybeDrag = false;
    bool m_vim = false;
    QList<int> m_autoHidden;  // columns hidden because the panel is narrow
    qint64 m_lastG = 0;  // for "gg"
    ThumbGrid* m_grid = nullptr;
};

}  // namespace tandem
