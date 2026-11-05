#pragma once

#include <QAbstractListModel>
#include <QFrame>
#include <QPointer>
#include <QIcon>
#include <QStyledItemDelegate>

#include <functional>
#include <optional>

class QLineEdit;
class QListView;
class QLabel;

namespace tandem {

struct PopupItem {
    QString title;
    QString subtitle;
    QString hint;            // right-aligned, e.g. a shortcut
    bool hintWhenCurrent = false;  // the hint shows on the current row only
    QString badge;           // a small tag at the right edge, e.g. "SSH"
    QIcon icon;
    bool matchSubtitle = false;  // match (and highlight) the subtitle instead of the title
    int bonus = 0;               // added to the fuzzy score (frecency, pins…)
    QString key;                 // caller's id (e.g. a path)
    QString section;             // a header above the first item of each section (unfiltered lists)
    std::function<void()> run;
};

class PopupModel : public QAbstractListModel {
    Q_OBJECT
public:
    struct Row {
        int item;
        QList<int> positions;
        QString header;  // drawn above this row
    };
    using QAbstractListModel::QAbstractListModel;
    void set(const QList<PopupItem>* items, QList<Row> rows);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    const PopupItem* item(int row) const;
    const QList<int>& positions(int row) const;
    QString header(int row) const { return row >= 0 && row < m_rows.size() ? m_rows[row].header : QString(); }
    // Items whose key is a local path can be dragged out as files.
    bool dragPaths = false;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private:
    const QList<PopupItem>* m_items = nullptr;
    QList<Row> m_rows;
};

class PopupDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

// VS Code style quick picker floating over the window: type to fuzzy-filter,
// Enter runs, Esc closes. Used by the command palette and folder jump.
class FuzzyPopup : public QFrame {
    Q_OBJECT
public:
    explicit FuzzyPopup(QWidget* window);

    void open(const QString& placeholder, const QList<PopupItem>& items, const QString& footer = {});
    // Same, as a drop-down under a widget (a button) instead of centered.
    void openUnder(QWidget* anchor, const QString& placeholder, const QList<PopupItem>& items, const QString& footer = {});
    void setItems(const QList<PopupItem>& items);  // keeps the query (after pin/forget)
    const PopupItem* currentItem() const;
    QString query() const;
    void setQuery(const QString& text);
    // Rows drag out as the files their keys name (after open(), which resets it).
    void setDraggable(bool on);

    // Extra keys (e.g. Ctrl+P to pin). Return true when handled.
    std::function<bool(QKeyEvent*, const PopupItem*)> keyHandler;
    // An extra first row made from what was typed (e.g. "Go to /some/path");
    // with a negative bonus it comes last, as a fallback.
    std::function<std::optional<PopupItem>(const QString& query)> queryItem;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void refilter();
    void place();
    void activate();
    void close();  // hand focus back first, so the right panel stays active

    QLineEdit* m_edit;
    QListView* m_list;
    QLabel* m_footer;
    PopupModel* m_model;
    QList<PopupItem> m_items;
    QList<PopupItem> m_shown;  // m_items, plus the queryItem row
    QPointer<QWidget> m_returnFocus;
    QPointer<QWidget> m_anchor;
};

}  // namespace tandem
