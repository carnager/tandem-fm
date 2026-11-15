#include "ui/KeyChordPopup.h"

#include "core/FolderHistory.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Keymap.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QVBoxLayout>

namespace tandem {

KeyChordPopup::KeyChordPopup(QWidget* window) : QFrame(window)
{
    const Theme& t = theme();
    setObjectName(QStringLiteral("chordPopup"));
    setFocusPolicy(Qt::StrongFocus);
    setThemeStyle(this, QStringLiteral(
        "#chordPopup { background: @header; border: 1px solid @border; border-radius: 12px; }"
        "QLabel { color: @text; }"
        "QLabel#dim { color: @dimText; }"
        "QLabel#cap { color: @accent; background: @window; border: 1px solid @border; border-radius: 4px;"
        "  padding: 1px 6px; font-weight: 600; }"
        "QLabel#capTyped { color: @accentText; background: @accent; border-radius: 4px; padding: 1px 6px;"
        "  font-weight: 600; }"
        "QLabel#keyField { background: @base; border: 1px solid @accent; border-radius: 8px; padding: 7px 10px; }"
        "QListView { background: transparent; border: none; outline: none; }"));
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, t.dark ? 160 : 70));
    setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 10);
    layout->setSpacing(10);
    m_title = new QLabel(this);
    QFont f = m_title->font();
    f.setWeight(QFont::DemiBold);
    m_title->setFont(f);
    layout->addWidget(m_title);
    m_field = new QLabel(this);
    m_field->setObjectName(QStringLiteral("keyField"));
    m_field->setTextFormat(Qt::RichText);
    QFont ff = m_field->font();  // a little larger, like the palette's input
    if (ff.pointSizeF() > 0)
        ff.setPointSizeF(ff.pointSizeF() * 1.1);
    m_field->setFont(ff);
    layout->addWidget(m_field);
    m_model = new PopupModel(this);
    m_list = new QListView(this);
    m_list->setModel(m_model);
    m_list->setItemDelegate(new PopupDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_list);
    m_grid = new QWidget(this);
    m_gridLayout = new QGridLayout(m_grid);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    m_gridLayout->setHorizontalSpacing(10);
    m_gridLayout->setVerticalSpacing(6);
    for (int c = 0; c < 6; ++c)
        m_gridLayout->setColumnStretch(c, c % 2 ? 1 : 0);  // key caps tight, paths take the room
    layout->addWidget(m_grid);
    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("dim"));
    m_hint->setWordWrap(true);  // never cut off
    layout->addWidget(m_hint);
    hide();
}

void KeyChordPopup::close()
{
    if (m_returnFocus)
        m_returnFocus->setFocus();
    hide();
}

void KeyChordPopup::openAssign(const QString& path, QWidget* anchor)
{
    m_anchor = anchor && anchor->isVisible() ? anchor : nullptr;
    if (!isVisible())
        m_returnFocus = QApplication::focusWidget();
    m_mode = Mode::Assign;
    m_assignPath = path;
    m_typed = FolderHistory::instance().bookmarkKey(path);
    show();
    raise();
    setFocus();
    refresh();
}

void KeyChordPopup::openChoices(const Choices& choices)
{
    if (!isVisible())
        m_returnFocus = QApplication::focusWidget();
    m_mode = Mode::Choices;
    m_choices = choices;
    m_typed.clear();
    show();
    raise();
    setFocus();
    refresh();
}

int KeyChordPopup::digitOf(const QKeyEvent* e)
{
    if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9)
        return e->key() - Qt::Key_0;
#ifdef Q_OS_MACOS
    static constexpr quint32 kMac[] = {0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19};  // kVK_ANSI_1…9
    for (int i = 0; i < 9; ++i)
        if (e->nativeVirtualKey() == kMac[i])
            return i + 1;
#else
    const quint32 code = e->nativeScanCode();  // evdev code + 8, on X11 and Wayland
    if (code >= 10 && code <= 18)
        return int(code) - 9;
#endif
    return 0;
}

void KeyChordPopup::pickChoice(int number, bool shift)
{
    for (const Choice& c : std::as_const(m_choices.items)) {
        if (c.number != number)
            continue;
        if (!c.unavailable.isEmpty()) {
            emit message(c.unavailable, true);
            return;
        }
        const auto pick = m_choices.pick;
        close();
        if (pick)
            pick(number, shift);
        return;
    }
}

bool KeyChordPopup::event(QEvent* e)
{
    // While open, every key belongs to the popup (no window shortcuts).
    if (e->type() == QEvent::ShortcutOverride) {
        e->accept();
        return true;
    }
    // Tab takes the first free key the hint offers (instead of moving the focus).
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Tab && m_mode == Mode::Assign) {
        if (!m_free.isEmpty()) {
            m_typed = m_free.first();
            refresh();
        }
        return true;
    }
    return QFrame::event(e);
}

void KeyChordPopup::focusOutEvent(QFocusEvent* e)
{
    QFrame::focusOutEvent(e);
    hide();
}

void KeyChordPopup::type(const QString& keys)
{
    m_typed += keys;
    refresh();
}

void KeyChordPopup::keyPressEvent(QKeyEvent* e)
{
    const QString text = e->text();
    if (m_mode == Mode::Choices) {
        const Qt::KeyboardModifiers mods = e->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        if (e->key() == Qt::Key_Escape) {
            close();
        } else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && m_choices.enterPicks) {
            pickChoice(m_choices.enterPicks, shift);
        } else if (e->key() == Qt::Key_Space && m_choices.space && !mods) {
            const auto space = m_choices.space;
            close();
            space();
        } else if (const int n = digitOf(e); n && !mods) {
            pickChoice(n, shift);
        }
        return;
    }
    switch (e->key()) {
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Backspace:
        m_typed.chop(1);
        refresh();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        FolderHistory::instance().setBookmark(m_assignPath, m_typed);
        emit message(m_typed.isEmpty() ? tr("Pinned %1").arg(m_assignPath)
                                       : tr("Bookmarked %1 as “%2”").arg(m_assignPath, m_typed),
                     false);
        close();
        return;
    case Qt::Key_Delete:
        FolderHistory::instance().setPinned(m_assignPath, false);
        emit message(tr("Removed bookmark for %1").arg(m_assignPath), false);
        close();
        return;
    default:
        break;
    }
    if (!text.isEmpty() && text.at(0).isPrint() && text != QLatin1String(" "))
        type(text);
}

void KeyChordPopup::refresh()
{
    while (QLayoutItem* item = m_gridLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    const bool assign = m_mode == Mode::Assign;
    m_field->setVisible(assign);
    m_list->setVisible(assign);
    m_grid->setVisible(!assign);
    if (!assign) {
        refreshChoices();
        return;
    }
    const auto all = FolderHistory::instance().bookmarks();
    {
        const QString owner = m_typed.isEmpty() ? QString() : FolderHistory::instance().pathForKey(m_typed);
        m_title->setText(tr("Bookmark key for %1").arg(m_assignPath));
        const Theme& t = theme();
        m_field->setText(m_typed.isEmpty()
                             ? QStringLiteral("<span style='color:%1'>%2</span>").arg(t.dimText.name(), tr("Type a key").toHtmlEscaped())
                             : QStringLiteral("<span style='color:%1'>%2</span><span style='color:%3'>▏</span>")
                                   .arg(t.text.name(), m_typed.toHtmlEscaped(), t.accent.name()));
        QStringList used;
        for (const auto& b : all)
            if (b.path != m_assignPath)
                used << b.key;
        m_free = FolderHistory::suggestKeys(m_assignPath, used + Keymap::instance().keysAfterBookmarkPrefix());
        const QString free = m_free.isEmpty() ? QString()
                                              : tr("free: %1   ·   Tab: %2   ·   ").arg(m_free.join(QStringLiteral(", ")), m_free.first());
        m_hint->setText(!owner.isEmpty() && owner != m_assignPath
                            ? tr("“%1” moves here from %2   ·   %3Enter: save   ·   Esc")
                                  .arg(m_typed, baseName(owner).isEmpty() ? owner : baseName(owner), free)
                            : tr("Type a key or chain (e.g. cp)   ·   %1Enter: save   ·   Delete: remove   ·   Esc").arg(free));
    }
    // The other bookmarks, as Ctrl+D lists them: all of them, or those whose
    // keys clash with what's typed (the same, or one the start of the other).
    m_rows.clear();
    QList<PopupModel::Row> rows;
    for (const auto& b : all) {
        if (b.path == m_assignPath || !(b.key.startsWith(m_typed) || m_typed.startsWith(b.key)))
            continue;
        PopupItem it;
        it.icon = IconCache::themed(b.path.contains(QLatin1String("://")) ? "folder-remote" : "folder", "folder");
        it.title = baseName(b.path).isEmpty() ? b.path : baseName(b.path);
        it.subtitle = it.title == b.path ? QString() : b.path;
        it.hint = b.key;
        rows.append({int(m_rows.size()), {}, {}});
        m_rows << it;
    }
    m_model->set(&m_rows, rows);
    m_list->setVisible(!rows.isEmpty());
    place();
}

void KeyChordPopup::refreshChoices()
{
    m_title->setText(m_choices.title);
    m_hint->setText(m_choices.hint);
    int row = 0;
    for (const Choice& c : std::as_const(m_choices.items)) {
        auto* cap = new QLabel(QString::number(c.number), m_grid);
        cap->setObjectName(c.number == m_choices.enterPicks ? QStringLiteral("capTyped") : QStringLiteral("cap"));
        auto* text = new QLabel(c.text, m_grid);
        text->setTextFormat(Qt::PlainText);
        text->setObjectName(c.unavailable.isEmpty() ? QString() : QStringLiteral("dim"));
        cap->show();
        text->show();
        m_gridLayout->addWidget(cap, row, 0, Qt::AlignLeft);
        m_gridLayout->addWidget(text, row, 1, 1, 5);
        ++row;
    }
    place();
}

void KeyChordPopup::place()
{
    QWidget* w = parentWidget();
    if (m_mode == Mode::Assign) {  // where and as wide as Ctrl+D's list; a dozen rows, the rest scroll
        QStyleOptionViewItem opt;
        opt.initFrom(m_list);
        int listHeight = 0;
        for (int r = 0; r < qMin(11, m_model->rowCount()); ++r)
            listHeight += m_list->itemDelegate()->sizeHint(opt, m_model->index(r)).height();
        m_list->setFixedHeight(listHeight + 4);
        if (m_anchor) {  // under the star, its right edge at the star's
            const int width = qMin(520, w->width() - 20);
            setFixedWidth(width);
            layout()->activate();
            adjustSize();
            const QPoint at = m_anchor->mapTo(w, QPoint(m_anchor->width(), m_anchor->height() + 4));
            move(qBound(10, at.x() - width, w->width() - width - 10), at.y());
            return;
        }
        const int width = qMin(680, w->width() - 40);
        setFixedWidth(width);
        layout()->activate();
        adjustSize();
        move((w->width() - width) / 2, qMin(72, w->height() / 8));
        return;
    }
    const int width = qMin(860, w->width() - 40);
    setFixedWidth(width);
    // The grid was just refilled: let it measure the new rows before the
    // popup takes its height, or the rows are squeezed and cut off.
    m_gridLayout->activate();
    layout()->activate();
    adjustSize();
    move((w->width() - width) / 2, w->height() - height() - 64);
}

}  // namespace tandem
