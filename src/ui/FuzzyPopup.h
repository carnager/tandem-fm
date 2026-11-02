#pragma once

#include <QAbstractListModel>
#include <QFrame>
#include <QIcon>
#include <QStyledItemDelegate>

#include <functional>

class QLineEdit;
class QListView;
class QLabel;

namespace tandem {

struct PopupItem {
    QString title;
    QString subtitle;
    QString hint;            // right-aligned, e.g. a shortcut
    QIcon icon;
    bool matchSubtitle = false;  // match (and highlight) the subtitle instead of the title
    int bonus = 0;               // added to the fuzzy score (frecency, pins…)
    QString key;                 // caller's id (e.g. a path)
    std::function<void()> run;
};

class PopupModel : public QAbstractListModel {
    Q_OBJECT
public:
    struct Row {
        int item;
        QList<int> positions;
    };
    using QAbstractListModel::QAbstractListModel;
    void set(const QList<PopupItem>* items, QList<Row> rows);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    const PopupItem* item(int row) const;
    const QList<int>& positions(int row) const;

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
    void setItems(const QList<PopupItem>& items);  // keeps the query (after pin/forget)
    const PopupItem* currentItem() const;
    QString query() const;

    // Extra keys (e.g. Ctrl+P to pin). Return true when handled.
    std::function<bool(QKeyEvent*, const PopupItem*)> keyHandler;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void refilter();
    void place();
    void activate();

    QLineEdit* m_edit;
    QListView* m_list;
    QLabel* m_footer;
    PopupModel* m_model;
    QList<PopupItem> m_items;
};

}  // namespace tandem
