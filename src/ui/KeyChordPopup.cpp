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

namespace {
QString pretty(const QString& path)
{
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}
}  // namespace

KeyChordPopup::KeyChordPopup(QWidget* window) : QFrame(window)
{
    const Theme& t = theme();
    setObjectName(QStringLiteral("chordPopup"));
    setFocusPolicy(Qt::StrongFocus);
    setThemeStyle(this, (QStringLiteral(
        "#chordPopup { background: @header; border: 1px solid @border; border-radius: 12px; }"
        "QLabel { color: @text; }"
        "QLabel#dim { color: @dimText; }"
        "QLabel#cap { color: @accent; background: @window; border: 1px solid @border; border-radius: 4px;"
        "  padding: 1px 6px; font-weight: 600; }"
        "QLabel#capTyped { color: @accentText; background: @accent; border-radius: 4px; padding: 1px 6px;"
        "  font-weight: 600; }")));
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

void KeyChordPopup::openJump(const QString& initialKeys)
{
    if (!isVisible())
        m_returnFocus = QApplication::focusWidget();
    m_mode = Mode::Jump;
    m_typed.clear();
    show();
    raise();
    setFocus();
    refresh();
    if (!initialKeys.isEmpty())
        type(initialKeys);
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
    if (m_mode == Mode::Assign) {
        refresh();
        return;
    }
    const auto all = FolderHistory::instance().bookmarks();
    QList<FolderHistory::Bookmark> matches;
    for (const auto& b : all)
        if (b.key.startsWith(m_typed))
            matches.append(b);
    if (matches.isEmpty()) {
        emit message(tr("No bookmark “%1”").arg(m_typed), true);
        m_typed.chop(keys.size());
        refresh();
        return;
    }
    // Unique and complete: go. (A chain that is also the start of a longer
    // one waits; Enter takes the exact one.)
    if (matches.size() == 1 && matches.first().key == m_typed) {
        const QString path = matches.first().path;
        close();
        emit jumpRequested(path);
        return;
    }
    refresh();
}

void KeyChordPopup::keyPressEvent(QKeyEvent* e)
{
    const QString text = e->text();
    switch (e->key()) {
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Backspace:
        if (m_typed.isEmpty() && m_mode == Mode::Jump) {
            close();
            return;
        }
        m_typed.chop(1);
        refresh();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_mode == Mode::Assign) {
            FolderHistory::instance().setBookmark(m_assignPath, m_typed);
            emit message(m_typed.isEmpty() ? tr("Pinned %1").arg(pretty(m_assignPath))
                                           : tr("Bookmarked %1 as “%2”").arg(pretty(m_assignPath), m_typed),
                         false);
            close();
        } else if (const QString p = FolderHistory::instance().pathForKey(m_typed); !p.isEmpty()) {
            close();
            emit jumpRequested(p);
        }
        return;
    case Qt::Key_Delete:
        if (m_mode == Mode::Assign) {
            FolderHistory::instance().setPinned(m_assignPath, false);
            emit message(tr("Removed bookmark for %1").arg(pretty(m_assignPath)), false);
            close();
            return;
        }
        break;
    case Qt::Key_Space:
        if (m_mode == Mode::Jump) {
            close();
            emit fuzzyRequested();
            return;
        }
        break;
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
    const auto all = FolderHistory::instance().bookmarks();
    if (m_mode == Mode::Jump) {
        m_title->setText(m_typed.isEmpty() ? tr("Bookmarks") : tr("Bookmarks  ·  %1…").arg(m_typed));
        m_hint->setText(tr("Type a key   ·   Space: search all folders   ·   Ctrl+Shift+D: bookmark this folder   ·   Esc"));
    } else {
        const QString owner = m_typed.isEmpty() ? QString() : FolderHistory::instance().pathForKey(m_typed);
        m_title->setText(tr("Bookmark key for %1:  %2").arg(pretty(m_assignPath), m_typed.isEmpty() ? QStringLiteral("_")
                                                                                                      : m_typed));
        m_hint->setText(!owner.isEmpty() && owner != m_assignPath
                            ? tr("“%1” is used by %2 and will move here   ·   Enter: save   ·   Esc").arg(m_typed, pretty(owner))
                            : tr("Type a key or chain (e.g. cp)   ·   Enter: save   ·   Delete: remove   ·   Esc"));
    }
    int row = 0, col = 0;
    const int columns = 3;
    for (const auto& b : all) {
        const bool match = m_mode == Mode::Assign || b.key.startsWith(m_typed);
        if (!match)
            continue;
        auto* cap = new QLabel(m_grid);
        if (m_mode == Mode::Jump && !m_typed.isEmpty()) {
            // Typed part highlighted, the rest still to press.
            cap->setTextFormat(Qt::RichText);
            cap->setText(QStringLiteral("<span style='opacity:0.5'>%1</span><b>%2</b>")
                             .arg(m_typed.toHtmlEscaped(), b.key.mid(m_typed.size()).toHtmlEscaped()));
        } else {
            cap->setText(b.key);
        }
        cap->setObjectName(QStringLiteral("cap"));
        auto* path = new QLabel(pretty(b.path), m_grid);
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

void KeyChordPopup::place()
{
    QWidget* w = parentWidget();
    const int width = qMin(860, w->width() - 40);
    setFixedWidth(width);
    adjustSize();
    move((w->width() - width) / 2, w->height() - height() - 64);
}

}  // namespace tandem
