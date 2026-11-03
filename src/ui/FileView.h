#pragma once

#include <QStyledItemDelegate>
#include <QSet>
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
    void setVimKeys(bool on) { m_vim = on; if (!on) setVisual(0); }
    // Visual mode (modal keys): 1 marks, -1 unmarks the range the cursor
    // moves over; 0 ends it, keeping the marks.
    void setVisual(int mode);
    int visualMode() const { return m_visual; }
    void setModel(QAbstractItemModel* model) override;
    // How much the panel that isn't active fades: 0 (not at all) to 1.
    QString dropText() const { return m_dropText; }  // for tests
    static void setInactiveDim(double amount);
    static double inactiveDim();
    // Zoom of the file lists, in percent (both panels alike).
    static constexpr int kMinZoom = 80, kMaxZoom = 180, kZoomStep = 10;
    static constexpr int kRowInset = 6;      // rows are bars this far in from the edges
    static constexpr double kRowRadius = 7;
    static void setListZoom(int percent);
    static int listZoom();
    static QFont zoomed(QFont f);  // a list font at the current zoom
    void zoomChanged();            // re-measures the rows
    void columnsMadeForZoom() { m_zoomOfColumns = listZoom(); }  // after setting or restoring widths
    double fade() const { return m_active ? 0.0 : inactiveDim(); }
    void setArrowFolders(bool on) { m_arrowFolders = on; }  // Left/Right leave and enter folders
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
    void intoRequested();               // Right: enter a folder, never launch a file
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
    void zoomStepRequested(int step);  // Ctrl+wheel: +1 bigger, -1 smaller
    void commandIdRequested(const QString& id);  // modal keys: run this command
    void hintRequested(const QString& text);     // a short note for the status line ("" clears)
    void modeChanged();                          // visual mode began or ended

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyboardSearch(const QString&) override {}
    void focusInEvent(QFocusEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void drawRow(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
    void applyVisual();
    void resizeEvent(QResizeEvent* e) override;

private:
    friend class ThumbGrid;
    void moveCursorBy(int delta);
    QString dropTargetFor(const QPoint& pos, int* row) const;
    void startFileDrag();
    void showHeaderMenu(const QPoint& pos);

    bool m_active = false;
    int m_dropRow = -1;
    QString m_dropText;  // while something is dragged over: what dropping would do
    void updateDropText(const QMimeData* data, Qt::DropAction action, const QPoint& pos);
    QPoint m_pressPos;
    bool m_maybeDrag = false;
    bool m_vim = false;
    int m_visual = 0;
    QString m_visualAnchor;  // where visual mode began, by name
    QString m_visualPath;
    QSet<QString> m_visualBase;  // marks before visual mode began
    bool m_pendingSort = false;  // ',' waits for the sort key
    bool m_arrowFolders = true;
    int m_zoomOfColumns = 100;  // the zoom the column widths were made for
    QList<int> m_autoHidden;  // columns hidden because the panel is narrow
    qint64 m_lastG = 0;  // for "gg"
    ThumbGrid* m_grid = nullptr;
};

}  // namespace tandem
