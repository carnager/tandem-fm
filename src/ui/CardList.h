#pragma once

#include <QStyledItemDelegate>

class QLabel;
class QWidget;

namespace tandem {

// The Locations window's lists: two lines per row (the text, and
// Qt::UserRole dimmed below), selected like the settings' sidebar.
// kCardBadgeRole, when set, is drawn as a key cap on the right.
constexpr int kCardBadgeRole = Qt::UserRole + 1;

class CardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const override;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

// A small heading inside a form, like the location popup's sections.
QLabel* sectionLabel(const QString& text, QWidget* parent);

}  // namespace tandem
