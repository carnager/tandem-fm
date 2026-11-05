#include "ui/ShelfBar.h"

#include "core/Shelves.h"
#include "core/Util.h"
#include "ui/Theme.h"

#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QMimeData>
#include <QStyle>
#include <QToolButton>

namespace tandem {

ShelfBar::ShelfBar(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("shelfBar"));
    setAcceptDrops(true);
    setThemeStyle(this, QStringLiteral(
        "QToolButton { color: @text; background: @base; border: 1px solid @border; border-radius: 10px;"
        "  padding: 2px 10px; }"
        "QToolButton:hover, QToolButton[target=\"true\"] { border-color: @accent; }"
        "QToolButton[open=\"true\"] { border-color: @accent; background: @altBase; }"
        "QToolButton#shelf-new { color: @dimText; border-style: dashed; }"));
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(8, 0, 8, 0);
    m_layout->setSpacing(4);
    m_layout->addStretch(1);
    m_new = new QToolButton(this);
    m_new->setObjectName(QStringLiteral("shelf-new"));
    m_new->setText(tr("＋ shelf"));
    m_new->setFocusPolicy(Qt::NoFocus);
    m_new->hide();
    m_layout->addWidget(m_new);
    connect(&Shelves::instance(), &Shelves::changed, this, &ShelfBar::rebuild);
    rebuild();
}

QToolButton* ShelfBar::chip(int shelf) const
{
    for (QToolButton* c : m_chips)
        if (c->property("shelf").toInt() == shelf)
            return c;
    return nullptr;
}

void ShelfBar::rebuild()
{
    qDeleteAll(m_chips);
    m_chips.clear();
    int at = 1;  // after the stretch
    for (int n : Shelves::instance().used()) {
        auto* c = new QToolButton(this);
        const int count = int(Shelves::instance().items(n).size());
        c->setObjectName(QStringLiteral("shelf-%1").arg(n));
        c->setProperty("shelf", n);
        c->setProperty("open", n == m_open);
        c->setText(QStringLiteral("▤ %1 · %2").arg(n).arg(count));
        c->setToolTip(plural(tr("Shelf %1: %n item(s), click to look in, drop to add", nullptr, count), count).arg(n));
        c->setFocusPolicy(Qt::NoFocus);
        c->setCursor(Qt::PointingHandCursor);
        connect(c, &QToolButton::clicked, this, [this, n] { emit openRequested(n); });
        m_layout->insertWidget(at++, c);
        m_chips.append(c);
    }
}

void ShelfBar::setOpenShelf(int shelf)
{
    m_open = shelf;
    rebuild();
}

int ShelfBar::shelfAt(const QPoint& pos) const
{
    for (QToolButton* c : m_chips)
        if (c->geometry().contains(pos))
            return c->property("shelf").toInt();
    return 0;
}

void ShelfBar::setDragging(bool on)
{
    m_new->setVisible(on && Shelves::instance().used().size() < Shelves::kCount);
}

void ShelfBar::dragEnterEvent(QDragEnterEvent* e)
{
    if (!e->mimeData()->hasUrls())
        return;
    setDragging(true);
    e->acceptProposedAction();
}

void ShelfBar::dragMoveEvent(QDragMoveEvent* e)
{
    const int over = shelfAt(e->position().toPoint());
    for (QToolButton* c : m_chips) {
        const bool target = c->property("shelf").toInt() == over;
        if (c->property("target").toBool() != target) {
            c->setProperty("target", target);
            c->style()->unpolish(c);
            c->style()->polish(c);
        }
    }
    e->setDropAction(Qt::LinkAction);  // nothing moves: the shelf only points at them
    e->accept();
}

void ShelfBar::dragLeaveEvent(QDragLeaveEvent*)
{
    setDragging(false);
    for (QToolButton* c : m_chips)
        c->setProperty("target", false);
    rebuild();
}

void ShelfBar::dropEvent(QDropEvent* e)
{
    int shelf = shelfAt(e->position().toPoint());
    setDragging(false);
    if (shelf == 0) {  // the first free shelf
        const QList<int> used = Shelves::instance().used();
        for (int n = 1; n <= Shelves::kCount && shelf == 0; ++n)
            if (!used.contains(n))
                shelf = n;
    }
    if (shelf == 0)
        return;
    e->setDropAction(Qt::LinkAction);
    e->accept();
    emit dropped(shelf, e->mimeData());
}

}  // namespace tandem
