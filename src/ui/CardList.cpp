#include "ui/CardList.h"

#include "ui/Theme.h"

#include <QLabel>
#include <QPainter>

namespace tandem {

QSize CardDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const
{
    return {opt.rect.width(), opt.fontMetrics.height() * 2 + 16};
}

void CardDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    const Theme& t = theme();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF card = QRectF(opt.rect).adjusted(0, 1, 0, -1);
    if (opt.state & QStyle::State_Selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(t.headerActive);
        p->drawRoundedRect(card, 6, 6);
    } else if (opt.state & QStyle::State_MouseOver) {
        p->setPen(Qt::NoPen);
        p->setBrush(t.altBase);
        p->drawRoundedRect(card, 6, 6);
    }
    QRect r = opt.rect.adjusted(10, 0, -10, 0);
    const int is = 22;
    idx.data(Qt::DecorationRole).value<QIcon>().paint(p, QRect(r.left(), r.center().y() - is / 2, is, is));
    r.setLeft(r.left() + is + 10);
    // The key, as a cap on the right.
    const QString badge = idx.data(kCardBadgeRole).toString();
    if (!badge.isEmpty()) {
        const int w = opt.fontMetrics.horizontalAdvance(badge) + 14;
        const QRect cap(r.right() - w, r.center().y() - opt.fontMetrics.height() / 2 - 2, w, opt.fontMetrics.height() + 4);
        p->setPen(t.border);
        p->setBrush(t.window);
        p->drawRoundedRect(QRectF(cap).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        p->setPen(t.accent);
        p->setFont(opt.font);
        p->drawText(cap, Qt::AlignCenter, badge);
        r.setRight(cap.left() - 8);
    }
    const int line = opt.fontMetrics.height();
    const int top = opt.rect.center().y() - line;
    p->setFont(opt.font);
    p->setPen(t.text);
    p->drawText(QRect(r.left(), top, r.width(), line), Qt::AlignLeft | Qt::AlignVCenter,
                opt.fontMetrics.elidedText(idx.data(Qt::DisplayRole).toString(), Qt::ElideRight, r.width()));
    QFont small = opt.font;
    small.setPointSizeF(small.pointSizeF() * 0.9);
    p->setFont(small);
    p->setPen(t.dimText);
    p->drawText(QRect(r.left(), top + line + 1, r.width(), line), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(small).elidedText(idx.data(Qt::UserRole).toString(), Qt::ElideMiddle, r.width()));
    p->restore();
}

QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text.toUpper(), parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    f.setBold(true);
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    l->setFont(f);
    setThemeStyle(l, QStringLiteral("color: @dimText; padding-top: 6px;"));
    return l;
}

}  // namespace tandem
