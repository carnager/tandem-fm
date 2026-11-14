#include "ui/KeyChordPopup.h"

#include "core/FolderHistory.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
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
        "  font-weight: 600; }"));
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
    layout->addWidget(m_hint);
    hide();
}

void KeyChordPopup::close()
{
    if (m_returnFocus)
        m_returnFocus->setFocus();
    hide();
}

void KeyChordPopup::openAssign(const QString& path)
{
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
    if (m_mode == Mode::Choices) {
        refreshChoices();
        return;
    }
    const auto all = FolderHistory::instance().bookmarks();
    {
        const QString owner = m_typed.isEmpty() ? QString() : FolderHistory::instance().pathForKey(m_typed);
        m_title->setText(tr("Bookmark key for %1:  %2").arg(m_assignPath, m_typed.isEmpty() ? QStringLiteral("_")
                                                                                                      : m_typed));
        m_hint->setText(!owner.isEmpty() && owner != m_assignPath
                            ? tr("“%1” is used by %2 and will move here   ·   Enter: save   ·   Esc").arg(m_typed, owner)
                            : tr("Type a key or chain (e.g. cp)   ·   Enter: save   ·   Delete: remove   ·   Esc"));
    }
    int row = 0, col = 0;
    const int columns = 3;
    const int width = qMin(860, parentWidget()->width() - 40);  // as place() makes it
    const int room = qMax(80, (width - 32) / columns - 70);     // a column, less its key
    for (const auto& b : all) {
        auto* cap = new QLabel(b.key, m_grid);
        cap->setObjectName(QStringLiteral("cap"));
        // Home as ~, the middle shortened: the folder's name stays visible.
        QString shown = b.path;
        if (shown.startsWith(QDir::homePath() + QLatin1Char('/')))
            shown = QLatin1Char('~') + shown.mid(QDir::homePath().size());
        auto* path = new QLabel(m_grid);
        path->setText(path->fontMetrics().elidedText(shown, Qt::ElideMiddle, room));
        path->setToolTip(b.path);
        path->setObjectName(b.path == m_assignPath ? QString() : QStringLiteral("dim"));
        path->setTextFormat(Qt::PlainText);
        cap->show();
        path->show();
        m_gridLayout->addWidget(cap, row, col * 2, Qt::AlignLeft);
        m_gridLayout->addWidget(path, row, col * 2 + 1);
        if (++col == columns) {
            col = 0;
            ++row;
        }
    }
    if (all.isEmpty()) {
        auto* none = new QLabel(tr("No bookmarks yet. Ctrl+Shift+D bookmarks the current folder."), m_grid);
        none->setObjectName(QStringLiteral("dim"));
        none->show();
        m_gridLayout->addWidget(none, 0, 0);
    }
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
