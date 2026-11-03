#include "ui/FontPicker.h"

#include "ui/Theme.h"

#include <QApplication>
#include <QButtonGroup>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

namespace tandem {

FontPicker::FontPicker(const QString& family, int size, bool fixedOnly, const QString& inherit, QWidget* parent)
    : QFrame(parent, Qt::Popup), m_origFamily(family), m_origSize(size)
{
    setObjectName(QStringLiteral("fontPicker"));
    setThemeStyle(this, QStringLiteral(
        "#fontPicker { background: @header; border: 1px solid @control; border-radius: 10px; }"
        "QListWidget { background: @base; border: 1px solid @border; border-radius: 6px; padding: 4px; outline: none; }"
        "QListWidget::item { padding: 4px 8px; border-radius: 5px; color: @text; }"
        "QListWidget::item:selected { background: @cursorActive; color: @text; }"
        "QToolButton { background: @base; color: @text; border: 1px solid @control; border-radius: 6px; padding: 3px 8px; }"
        "QToolButton:checked { background: @accent; color: @accentText; border-color: @accent; }"
        "QLabel#sample { background: @base; color: @text; border: 1px solid @border; border-radius: 6px; padding: 8px 10px; }"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(8);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search fonts"));
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);  // ↑ ↓ Enter work while typing
    v->addWidget(m_search);

    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(false);
    auto* inheritItem = new QListWidgetItem(inherit, m_list);
    inheritItem->setData(Qt::UserRole, QString());
    QFont italic = font();
    italic.setItalic(true);
    inheritItem->setFont(italic);
    for (const QString& f : QFontDatabase::families()) {
        if (QFontDatabase::isPrivateFamily(f) || (fixedOnly && !QFontDatabase::isFixedPitch(f)))
            continue;
        // Only fonts for text: Latin letters, no symbol and icon fonts.
        const QList<QFontDatabase::WritingSystem> ws = QFontDatabase::writingSystems(f);
        if (!ws.contains(QFontDatabase::Latin) || ws.contains(QFontDatabase::Symbol) ||
            f.contains(QLatin1String("Symbols"), Qt::CaseInsensitive) ||
            f.contains(QLatin1String("Emoji"), Qt::CaseInsensitive) || f.contains(QLatin1String("Icons"), Qt::CaseInsensitive))
            continue;
        auto* it = new QListWidgetItem(f, m_list);
        it->setData(Qt::UserRole, f);
        QFont own(f);
        own.setPointSizeF(font().pointSizeF() * 1.1);
        it->setFont(own);  // each name in its own font
        if (f == family)
            m_list->setCurrentItem(it);
    }
    if (!m_list->currentItem())
        m_list->setCurrentRow(0);
    v->addWidget(m_list, 1);

    auto* sizes = new QHBoxLayout;
    sizes->setSpacing(4);
    m_sizes = new QButtonGroup(this);
    m_sizes->setExclusive(true);
    for (int pt : {0, 9, 10, 11, 12, 13, 14, 16, 18}) {
        auto* b = new QToolButton(this);
        b->setText(pt ? QString::number(pt) : tr("Default"));
        b->setCheckable(true);
        b->setChecked(pt == size);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(pt ? tr("%1 pt").arg(pt) : tr("The size it is based on"));
        m_sizes->addButton(b, pt);
        sizes->addWidget(b);
    }
    if (!m_sizes->checkedButton()) {  // a size set elsewhere: keep it
        auto* b = new QToolButton(this);
        b->setText(QString::number(size));
        b->setCheckable(true);
        b->setChecked(true);
        b->setFocusPolicy(Qt::NoFocus);
        m_sizes->addButton(b, size);
        sizes->addWidget(b);
    }
    sizes->addStretch(1);
    v->addLayout(sizes);

    m_sample = new QLabel(tr("Holiday 2026.jpg   report_final (2).pdf   1.27 KiB   2026-11-03 21:50"), this);
    m_sample->setObjectName(QStringLiteral("sample"));
    v->addWidget(m_sample);

    connect(m_search, &QLineEdit::textChanged, this, &FontPicker::filter);
    connect(m_list, &QListWidget::currentItemChanged, this, &FontPicker::tryOut);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &QWidget::close);
    connect(m_sizes, &QButtonGroup::idClicked, this, &FontPicker::tryOut);
    resize(380, 460);
    tryOut();
}

void FontPicker::popupBelow(QWidget* anchor)
{
    QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height() + 4));
    if (const QScreen* s = anchor->screen()) {
        const QRect avail = s->availableGeometry();
        pos.setY(qMin(pos.y(), avail.bottom() - height()));
        pos.setX(qMin(pos.x(), avail.right() - width()));
    }
    move(pos);
    show();
    m_search->setFocus();
    m_list->scrollToItem(m_list->currentItem(), QAbstractItemView::PositionAtCenter);
}

QString FontPicker::currentFamily() const
{
    const QListWidgetItem* it = m_list->currentItem();
    return it ? it->data(Qt::UserRole).toString() : m_origFamily;
}

void FontPicker::filter(const QString& text)
{
    QListWidgetItem* first = nullptr;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        const bool show = text.isEmpty() || it->text().contains(text, Qt::CaseInsensitive);
        it->setHidden(!show);
        if (show && !first)
            first = it;
    }
    if (first && (!m_list->currentItem() || m_list->currentItem()->isHidden()))
        m_list->setCurrentItem(first);
}

void FontPicker::tryOut()
{
    const QString family = currentFamily();
    const int size = m_sizes->checkedId();
    QFont f = family.isEmpty() ? QApplication::font() : QFont(family);
    if (size > 0)
        f.setPointSize(size);
    else
        f.setPointSizeF(QApplication::font().pointSizeF());
    m_sample->setFont(f);
    emit changed(family, qMax(0, size));
}

bool FontPicker::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_search && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        switch (k->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QApplication::sendEvent(m_list, e);  // move through the list while typing
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            close();
            return true;
        case Qt::Key_Escape:
            m_reverted = true;
            close();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, e);
}

void FontPicker::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape)
        m_reverted = true;
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        close();
        return;
    }
    QFrame::keyPressEvent(e);
}

void FontPicker::hideEvent(QHideEvent* e)
{
    QFrame::hideEvent(e);
    if (m_reverted)
        emit changed(m_origFamily, m_origSize);  // Esc: as it was
    deleteLater();
}

}  // namespace tandem
