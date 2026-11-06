#include "ui/FileView.h"

#include "ui/ThumbGrid.h"

#include "core/Sorting.h"
#include "core/Util.h"
#include "ui/FileColors.h"
#include "ui/FileModel.h"
#include "ui/Keymap.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDrag>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>

namespace tandem {

// ------------------------------------------------------------ FileDelegate

FileDelegate::FileDelegate(QTreeView* view) : QStyledItemDelegate(view), m_view(view) {}

QSize FileDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const
{
    const int h = qMax(QFontMetrics(FileView::zoomed(theme().listFont)).height() + 12,
                       qRound(28 * FileView::listZoom() / 100.0 * interfaceScale()));
    return {40, h};
}

void FileDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    const auto* model = static_cast<const FileModel*>(idx.model());
    const FileEntry* e = model->entry(idx.row());
    if (!e)
        return;
    const Theme& t = theme();
    const int col = idx.column();

    QColor fg = t.text;
    if (e->marked)
        fg = t.mark;
    else if (!e->isParent && e->name.startsWith(QLatin1Char('.')))
        fg = t.dimText;
    else if (e->brokenLink)
        fg = t.error;
    else if (col == FileModel::ColName || col == FileModel::ColExt)
        if (const auto c = fileColor(*e))
            fg = *c;

    // The panel that isn't active fades into its background, if wished.
    const auto* view = qobject_cast<const FileView*>(opt.widget);
    const double fade = view ? view->fade() : 0.0;
    p->save();
    if (fade > 0)
        p->setOpacity(1.0 - fade);

    // Text and icons sit inside the rounded row bar (see drawRow).
    const bool lastColumn = opt.rect.right() >= m_view->viewport()->width() - 2;
    QRect r = opt.rect.adjusted(col == FileModel::ColName ? FileView::kRowInset + 10 : 6, 0,
                                lastColumn ? -(FileView::kRowInset + 10) : -6, 0);
    if (col == FileModel::ColName) {
        const int iconSize = qRound(16 * FileView::listZoom() / 100.0 * interfaceScale());
        const QIcon ic = idx.data(Qt::DecorationRole).value<QIcon>();
        const QRect iconRect(r.left(), r.top() + (r.height() - iconSize) / 2, iconSize, iconSize);
        ic.paint(p, iconRect);
        if (e->isLink && !e->isParent) {
            // Small arrow badge for symlinks.
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            const QRect b(iconRect.right() - 6, iconRect.bottom() - 6, 8, 8);
            p->setPen(Qt::NoPen);
            p->setBrush(t.base);
            p->drawEllipse(b);
            p->setPen(QPen(t.accent, 1.4));
            p->drawLine(b.left() + 2, b.bottom() - 2, b.right() - 2, b.top() + 2);
            p->restore();
        }
        r.setLeft(iconRect.right() + 7);
        // Git badge at the right end of the name.
        const char git = model->gitStatus(e->name);
        if (git == '!' && !e->marked) {
            fg = t.dimText;
        } else if (git) {
            QColor c = git == 'U' ? t.error : git == 'M' ? t.gitModified : git == 'A' ? t.gitAdded : t.gitUntracked;
            const int s = opt.fontMetrics.height() - 2;
            const QRect badge(r.right() - s, r.top() + (r.height() - s) / 2, s, s);
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            QColor bg = c;
            bg.setAlpha(45);
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(badge, 3, 3);
            QFont bf = FileView::zoomed(t.listFont);
            bf.setPointSizeF(bf.pointSizeF() * 0.78);
            bf.setWeight(QFont::Bold);
            p->setFont(bf);
            p->setPen(c);
            p->drawText(badge, Qt::AlignCenter, QString(QChar(git == 'A' ? '+' : git)));
            p->restore();
            r.setRight(badge.left() - 4);
        }
    }

    const bool number = col == FileModel::ColSize || col == FileModel::ColTime || col == FileModel::ColOctal ||
                        col == FileModel::ColCreated || col == FileModel::ColAccessed;
    QFont f = FileView::zoomed(number ? t.numberFont : t.listFont);
    if (e->isDir && col == FileModel::ColName)
        f.setWeight(QFont::DemiBold);
    if (e->marked)
        f.setWeight(QFont::Bold);
    p->setFont(f);
    // Secondary columns are dimmed so names stand out; marks keep their color.
    const bool secondary = col != FileModel::ColName && col != FileModel::ColExt;
    if (secondary && !e->marked)
        fg = t.dimText;
    p->setPen(fg);

    QString text = idx.data(Qt::DisplayRole).toString();
    const QFontMetrics fm(f);
    const auto faded = [](QColor c) {
        c.setAlphaF(c.alphaF() * 0.62);
        return c;
    };
    // Without an Ext column the name ends in its extension, a little fainter.
    if (col == FileModel::ColName && !e->isDir && !e->isParent && m_view->isColumnHidden(FileModel::ColExt)) {
        const QString ext = FileModel::displayExt(*e);
        if (!ext.isEmpty()) {
            if (fm.horizontalAdvance(e->name) <= r.width()) {
                p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text);
                QRect rest = r;
                rest.setLeft(r.left() + fm.horizontalAdvance(text));
                p->setPen(faded(fg));
                p->drawText(rest, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, QLatin1Char('.') + ext);
                p->restore();
                return;
            }
            text = e->name;  // too long: shortened as a whole
        }
    }
    // Sizes and counts: the unit fainter than the number.
    if (col == FileModel::ColSize) {
        const int space = int(text.lastIndexOf(QLatin1Char(' ')));
        if (space > 0) {
            const QString unit = text.mid(space);
            const int uw = fm.horizontalAdvance(unit);
            QRect nr = r;
            nr.setRight(r.right() - uw);
            p->drawText(nr, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, text.left(space));
            p->setPen(faded(fg));
            p->drawText(QRect(r.right() - uw, r.top(), uw + 1, r.height()), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                        unit);
            p->restore();
            return;
        }
    }
    const Qt::Alignment align = col == FileModel::ColSize ? (Qt::AlignRight | Qt::AlignVCenter)
                                                          : (Qt::AlignLeft | Qt::AlignVCenter);
    // Names elide in the middle (keeps the end visible); short columns at the end.
    const Qt::TextElideMode elide = col == FileModel::ColName ? Qt::ElideMiddle : Qt::ElideRight;
    const QString shown = QFontMetrics(f).elidedText(text, elide, r.width());
    p->drawText(r, int(align) | Qt::TextSingleLine, shown);
    p->restore();
}

namespace {
double g_inactiveDim = 0.0;
int g_listZoom = 100;
}

void FileView::setListZoom(int percent)
{
    g_listZoom = qBound(kMinZoom, percent, kMaxZoom);
}

int FileView::listZoom()
{
    return g_listZoom;
}

QFont FileView::zoomed(QFont f)
{
    if (g_listZoom != 100 && f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * g_listZoom / 100.0);
    return f;
}

void FileView::zoomChanged()
{
    const int iconSize = qRound(16 * g_listZoom / 100.0 * interfaceScale());
    setIconSize(QSize(iconSize, iconSize));
    // Columns grow and shrink along, keeping widths the user chose.
    if (model() && m_zoomOfColumns != g_listZoom) {
        for (int c = FileModel::ColExt; c < header()->count(); ++c)
            setColumnWidth(c, qRound(columnWidth(c) * double(g_listZoom) / m_zoomOfColumns));
        m_zoomOfColumns = g_listZoom;
        fitColumns();
    }
    scheduleDelayedItemsLayout();  // rows get their new height
    viewport()->update();
}

void FileView::setModel(QAbstractItemModel* model)
{
    setVisual(0);
    if (QAbstractItemModel* old = this->model())
        disconnect(old, &QAbstractItemModel::modelAboutToBeReset, this, nullptr);
    QTreeView::setModel(model);
    // A new listing: another folder ends visual mode, a refresh of this one keeps it.
    if (model)
        connect(model, &QAbstractItemModel::modelReset, this, [this] {
            if (m_visual)
                applyVisual();
        });
}

void FileView::setVisual(int mode)
{
    FileModel* m = fileModel();
    if (mode == m_visual || (mode && !m))
        return;
    m_visual = mode;
    if (mode) {
        const FileEntry* e = m->entry(currentRow());
        m_visualAnchor = e ? e->name : QString();
        m_visualPath = m->path();
        m_visualBase = m->markedSet();
        applyVisual();
    }
    emit modeChanged();
}

// The marks as they were, plus (or minus) everything between the anchor
// and the cursor.
void FileView::applyVisual()
{
    FileModel* m = fileModel();
    const int anchor = m ? m->rowOf(m_visualAnchor) : -1;
    if (!m || anchor < 0 || m->path() != m_visualPath) {  // another folder, or the start is gone
        setVisual(0);
        return;
    }
    QSet<QString> names = m_visualBase;
    const int from = qMin(anchor, currentRow()), to = qMax(anchor, currentRow());
    for (int r = qMax(0, from); r <= to; ++r) {
        const FileEntry* e = m->entry(r);
        if (!e || e->isParent)
            continue;
        if (m_visual > 0)
            names.insert(e->name);
        else
            names.remove(e->name);
    }
    m->setMarkedNames(names);
}

void FileView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        if (const int dy = e->angleDelta().y())
            emit zoomStepRequested(dy > 0 ? 1 : -1);
        return;
    }
    QTreeView::wheelEvent(e);
}

void FileView::setInactiveDim(double amount)
{
    g_inactiveDim = qBound(0.0, amount, 0.8);
}

double FileView::inactiveDim()
{
    return g_inactiveDim;
}

QWidget* FileDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const
{
    auto* ed = new QLineEdit(parent);
    ed->setFrame(false);
    ed->setFont(FileView::zoomed(theme().listFont));
    setThemeStyle(ed, QStringLiteral(
        "QLineEdit { background: @base; color: @text; border: 1px solid @accent; border-radius: 4px; padding: 0 4px; }"));
    return ed;
}

void FileDelegate::setEditorData(QWidget* editor, const QModelIndex& idx) const
{
    auto* ed = static_cast<QLineEdit*>(editor);
    const QString name = idx.data(Qt::EditRole).toString();
    ed->setText(name);
    // Select the name without extension, like every good file manager.
    const auto* model = static_cast<const FileModel*>(idx.model());
    const FileEntry* e = model->entry(idx.row());
    const int extLen = (e && !e->isDir) ? int(extensionOf(name).size()) : 0;
    const int dot = extLen > 0 ? int(name.size()) - extLen - 1 : -1;
    if (dot > 0)
        ed->setSelection(0, dot);
    else
        ed->selectAll();
}

void FileDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt, const QModelIndex&) const
{
    QRect r = opt.rect;
    r.setLeft(r.left() + 26);
    r.setRight(r.right() + m_view->columnWidth(FileModel::ColExt));
    editor->setGeometry(r.adjusted(0, 1, 0, -1));
}

// --------------------------------------------------------------- FileView

FileView::FileView(QWidget* parent) : QTreeView(parent)
{
    setItemDelegate(new FileDelegate(this));
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, [this] {
        zoomChanged();  // a new font or interface size: rows of another height
    });
    // "5 min ago" becomes "6 min ago".
    auto* minute = new QTimer(this);
    minute->setInterval(60 * 1000);
    connect(minute, &QTimer::timeout, this, [this] {
        if (FileModel::relativeDates() && isVisible())
            viewport()->update();
    });
    minute->start();
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setUniformRowHeights(true);  // essential for huge directories
    setAllColumnsShowFocus(true);
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setTextElideMode(Qt::ElideMiddle);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDropIndicatorShown(false);
    setMouseTracking(false);
    viewport()->setAcceptDrops(true);

    QHeaderView* h = header();
    h->setSectionsClickable(true);
    h->setSortIndicatorShown(true);
    h->setHighlightSections(false);
    h->setStretchLastSection(false);
    h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    h->setMinimumSectionSize(30);
    h->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(h, &QHeaderView::customContextMenuRequested, this, &FileView::showHeaderMenu);
    setThemeStyle(h, QStringLiteral(
        "QHeaderView { background: @base; border: none; border-bottom: 1px solid @border; }"
        "QHeaderView::section { background: @base; color: @dimText; border: none; padding: 5px 6px 6px 6px; }"
        "QHeaderView::section:first { padding-left: 16px; }"
        "QHeaderView::section:last { padding-right: 16px; }"
        "QHeaderView::section:hover { color: @text; }"));
    QFont small = h->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    h->setFont(small);
}

FileModel* FileView::fileModel() const
{
    return static_cast<FileModel*>(model());
}

void FileView::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    viewport()->update();
}

void FileView::setTypingFor(bool on)
{
    if (m_typingFor == on)
        return;
    m_typingFor = on;
    viewport()->update();
}

void FileView::setCurrentRow(int row, bool center)
{
    if (!model() || model()->rowCount() == 0)
        return;
    row = std::clamp(row, 0, model()->rowCount() - 1);
    const QModelIndex idx = model()->index(row, 0);
    setCurrentIndex(idx);
    scrollTo(idx, center ? QAbstractItemView::PositionAtCenter : QAbstractItemView::EnsureVisible);
}

void FileView::setThumbnails(bool on)
{
    if (on == thumbnails())
        return;
    if (on && !m_grid)
        m_grid = new ThumbGrid(this);
    if (m_grid) {
        m_grid->setGeometry(viewport()->rect());
        m_grid->setVisible(on);
        if (on)
            m_grid->ensureVisible(currentRow());
    }
    setHeaderHidden(on);  // columns mean nothing in a grid
    setVerticalScrollBarPolicy(on ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);  // the grid has its own
}

bool FileView::thumbnails() const
{
    return m_grid && m_grid->isVisible();
}

void FileView::moveCursorBy(int delta)
{
    setCurrentRow(currentRow() + delta);
}

void FileView::markTo(int target, bool includeTarget)
{
    FileModel* m = fileModel();
    if (!m || !model() || model()->rowCount() == 0)
        return;
    const int from = currentRow();
    target = std::clamp(target, 0, model()->rowCount() - 1);
    if (target == from) {
        if (includeTarget)
            m->toggleMarked(from);
        return;
    }
    const FileEntry* e = m->entry(from);
    const bool on = !(e && e->marked);
    // Like Shift+Down held: the file the cursor lands on stays as it was,
    // except at the very top or bottom, where there is nothing further.
    const int last = includeTarget ? target : target + (target > from ? -1 : 1);
    m->setMarkedRange(from, last, on);
    setCurrentRow(target);
}

bool FileView::event(QEvent* e)
{
    if (m_vim && e->type() == QEvent::ShortcutOverride) {
        // Modal keys win over window shortcuts (Ctrl+D scrolls, not bookmarks).
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
            QStringList seq = m_pending;
            seq << Keymap::tokenFor(ke);
            for (const Keymap::Binding& b : Keymap::instance().modalBindings())
                if (b.sequence.mid(0, seq.size()) == seq) {
                    e->accept();
                    return true;
                }
        }
    }
    if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) &&
            !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            emit switchPanelRequested();
            return true;
        }
    }
    return QTreeView::event(e);
}

void FileView::keyPressEvent(QKeyEvent* e)
{
    const bool keypad = e->modifiers() & Qt::KeypadModifier;
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    FileModel* m = fileModel();

    if (thumbnails() && !(mods & (Qt::ControlModifier | Qt::AltModifier))) {
        // In the grid the arrows go sideways too; up and down a whole row.
        const int cols = m_grid->columns();
        int step = 0;
        switch (e->key()) {
        case Qt::Key_Left: step = -1; break;
        case Qt::Key_Right: step = 1; break;
        case Qt::Key_Up: step = -cols; break;
        case Qt::Key_Down: step = cols; break;
        case Qt::Key_PageUp: step = -cols * m_grid->visibleRows(); break;
        case Qt::Key_PageDown: step = cols * m_grid->visibleRows(); break;
        case Qt::Key_Home:
        case Qt::Key_End: {
            const int target = e->key() == Qt::Key_Home ? 0 : (model() ? model()->rowCount() - 1 : 0);
            if (mods & Qt::ShiftModifier)
                markTo(target, true);
            else
                setCurrentRow(target);
            return;
        }
        default: break;
        }
        if (step && (mods & Qt::ShiftModifier) && (e->key() == Qt::Key_PageUp || e->key() == Qt::Key_PageDown)) {
            markTo(currentRow() + step, false);
            return;
        }
        if (step) {
            if ((mods & Qt::ShiftModifier) && m)
                m->toggleMarked(currentRow());
            moveCursorBy(step);
            return;
        }
    }

    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // The rename editor passes its Enter on: that confirms the name, it
        // doesn't open the file.
        if (state() == QAbstractItemView::EditingState) {
            e->accept();
            return;
        }
        if (mods == Qt::NoModifier) {
            emit openRequested();
            return;
        }
        break;
    case Qt::Key_Backspace:
        if (mods == Qt::NoModifier) {
            emit parentRequested();
            return;
        }
        break;
    case Qt::Key_Escape:
        if (m_visual || !m_pending.isEmpty()) {  // leaves visual mode, keeping its marks
            m_pending.clear();
            emit hintRequested(QString());
            setVisual(0);
            return;
        }
        emit escapePressed();
        return;
    case Qt::Key_F10:
        if (mods == Qt::ShiftModifier) {
            const QRect r = visualRect(currentIndex());
            emit contextMenuRequested(viewport()->mapToGlobal(QPoint(r.left() + 40, r.bottom())), currentRow());
            return;
        }
        break;
    case Qt::Key_Insert:
        if (mods == Qt::NoModifier && m) {
            m->toggleMarked(currentRow());
            moveCursorBy(1);
            return;
        }
        break;
    case Qt::Key_Space:
        if (mods == Qt::NoModifier) {
            emit spaceMarkRequested(currentRow());
            moveCursorBy(1);
            return;
        }
        break;
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (mods == Qt::ShiftModifier && m) {
            m->toggleMarked(currentRow());
            moveCursorBy(e->key() == Qt::Key_Down ? 1 : -1);
            return;
        }
        break;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        if (mods == Qt::ShiftModifier && m) {
            const CursorAction action = e->key() == Qt::Key_PageUp     ? MovePageUp
                                        : e->key() == Qt::Key_PageDown ? MovePageDown
                                        : e->key() == Qt::Key_Home     ? MoveHome
                                                                       : MoveEnd;
            const QModelIndex to = moveCursor(action, Qt::NoModifier);
            if (to.isValid())
                markTo(to.row(), e->key() == Qt::Key_Home || e->key() == Qt::Key_End);
            return;
        }
        break;
    case Qt::Key_Plus:
    case Qt::Key_Minus:
    case Qt::Key_Asterisk:
        if (keypad && mods == Qt::NoModifier) {
            emit markGroupRequested(e->key() == Qt::Key_Plus ? 1 : (e->key() == Qt::Key_Minus ? -1 : 0));
            return;
        }
        break;
    case Qt::Key_Left:
    case Qt::Key_Right:
        if (mods == Qt::NoModifier) {
            // No tree to expand and no sideways scrolling: walk folders instead.
            if (m_arrowFolders) {
                if (e->key() == Qt::Key_Left)
                    emit parentRequested();
                else
                    emit intoRequested();
            }
            return;
        }
        break;
    default:
        break;
    }

    if (m_vim && modalKey(e))
        return;

    const QString text = e->text();
    if (!text.isEmpty() && text.at(0).isPrint() && !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        emit quickSearchRequested(text);
        return;
    }
    QTreeView::keyPressEvent(e);
}

// Modal keys from keys.toml: single keys and sequences ("g g", ", n").
// While a sequence is open, a hint shows what can follow.
bool FileView::modalKey(QKeyEvent* e)
{
    const QString token = Keymap::tokenFor(e);
    if (token.isEmpty() || token == QLatin1String("Shift") || token == QLatin1String("Ctrl") ||
        token == QLatin1String("Alt") || token == QLatin1String("Meta"))
        return false;
    const QList<Keymap::Binding>& bindings = Keymap::instance().modalBindings();
    QStringList prefix;  // bookmark-prefix: then a free key is a bookmark
    for (const Keymap::Binding& b : bindings)
        if (b.action == QLatin1String("bookmark-prefix"))
            prefix = b.sequence;
    QStringList seq = m_pending;
    seq << token;
    const Keymap::Binding* exact = nullptr;
    QList<const Keymap::Binding*> longer;
    for (const Keymap::Binding& b : bindings) {
        if (b.action == QLatin1String("bookmark-prefix"))
            continue;
        if (b.sequence == seq)
            exact = &b;
        else if (b.sequence.size() > seq.size() && b.sequence.mid(0, seq.size()) == seq)
            longer.append(&b);
    }
    if (!longer.isEmpty() || (seq == prefix && !exact)) {
        m_pending = seq;  // wait for the rest
        QStringList next;
        for (const Keymap::Binding* b : std::as_const(longer)) {
            const QString title = Keymap::actionTitle ? Keymap::actionTitle(b->action) : b->action;
            next << QStringLiteral("%1 %2").arg(b->sequence.mid(seq.size()).join(QLatin1Char(' ')), title);
        }
        if (seq == prefix)
            next << tr("any other key: that bookmark");
        emit hintRequested(seq.join(QLatin1Char(' ')) + QStringLiteral("  →  ") + next.join(QStringLiteral("  ·  ")));
        return true;
    }
    const bool hadPending = !m_pending.isEmpty();
    m_pending.clear();
    if (hadPending)
        emit hintRequested(QString());
    if (exact) {
        runModalAction(exact->action);
        return true;
    }
    if (hadPending && seq.mid(0, seq.size() - 1) == prefix && token.size() == 1 && token.at(0).isPrint()) {
        emit bookmarkJumpRequested(token);
        return true;
    }
    if (hadPending)  // the sequence went nowhere: the key on its own
        return modalKey(e);
    // Letters are commands in modal mode, never quick search.
    const QString t = e->text();
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    return !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !t.isEmpty() &&
           t.at(0).isPrint() && t != QLatin1String(" ");
}

void FileView::runModalAction(const QString& action)
{
    const int page = qMax(1, viewport()->height() / qMax(1, sizeHintForRow(0)));
    static const QHash<QString, int> builtins = {
        {QStringLiteral("down"), 1},          {QStringLiteral("up"), 2},
        {QStringLiteral("parent"), 3},        {QStringLiteral("open"), 4},
        {QStringLiteral("top"), 5},           {QStringLiteral("bottom"), 6},
        {QStringLiteral("half-page-down"), 7}, {QStringLiteral("half-page-up"), 8},
        {QStringLiteral("visual"), 9},        {QStringLiteral("visual-unmark"), 10},
        {QStringLiteral("find"), 11},         {QStringLiteral("find-next"), 12},
        {QStringLiteral("find-previous"), 13}, {QStringLiteral("palette"), 14},
        {QStringLiteral("command-line"), 15}, {QStringLiteral("bookmark-jump"), 16},
        {QStringLiteral("bookmark-set"), 17},
    };
    switch (builtins.value(action)) {
    case 1: moveCursorBy(1); return;
    case 2: moveCursorBy(-1); return;
    case 3: emit parentRequested(); return;
    case 4: emit openRequested(); return;
    case 5: setCurrentRow(0); return;
    case 6: setCurrentRow(model() ? model()->rowCount() - 1 : 0); return;
    case 7: moveCursorBy(page / 2); return;
    case 8: moveCursorBy(-page / 2); return;
    case 9: setVisual(m_visual == 1 ? 0 : 1); return;
    case 10: setVisual(m_visual == -1 ? 0 : -1); return;
    case 11: emit quickSearchRequested(QString()); return;
    case 12: emit searchNextRequested(1); return;
    case 13: emit searchNextRequested(-1); return;
    case 14: emit paletteRequested(); return;
    case 15: emit commandRequested(); return;
    case 16: emit bookmarkJumpRequested(QString()); return;
    case 17: emit bookmarkAssignRequested(); return;
    default: break;
    }
    setVisual(0);  // the marks it made stay for the command
    emit commandIdRequested(action);
}

void FileView::focusInEvent(QFocusEvent* e)
{
    QTreeView::focusInEvent(e);
    emit focused();
}

void FileView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::BackButton || e->button() == Qt::ForwardButton) {
        emit historyRequested(e->button() == Qt::BackButton ? -1 : 1);
        return;
    }
    const QModelIndex idx = indexAt(e->position().toPoint());
    FileModel* m = fileModel();
    if (e->button() == Qt::LeftButton && idx.isValid() && m) {
        if (e->modifiers() & Qt::ControlModifier) {
            m->toggleMarked(idx.row());
            setCurrentRow(idx.row());
            setFocus();
            return;
        }
        if (e->modifiers() & Qt::ShiftModifier) {
            const int from = qMin(currentRow(), idx.row());
            const int to = qMax(currentRow(), idx.row());
            for (int r = from; r <= to; ++r)
                m->setMarked(r, true);
            setCurrentRow(idx.row());
            setFocus();
            return;
        }
        m_pressPos = e->position().toPoint();
        m_maybeDrag = true;
    }
    QTreeView::mousePressEvent(e);
}

void FileView::mouseMoveEvent(QMouseEvent* e)
{
    if (m_maybeDrag && (e->buttons() & Qt::LeftButton) &&
        (e->position().toPoint() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_maybeDrag = false;
        startFileDrag();
        return;
    }
    // Swallow plain moves: the base class would start rubber-band selection.
    if (e->buttons() & Qt::LeftButton)
        return;
    QTreeView::mouseMoveEvent(e);
}

void FileView::mouseReleaseEvent(QMouseEvent* e)
{
    m_maybeDrag = false;
    QTreeView::mouseReleaseEvent(e);
}

void FileView::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && indexAt(e->position().toPoint()).isValid()) {
        emit openRequested();
        return;
    }
    QTreeView::mouseDoubleClickEvent(e);
}

void FileView::drawRow(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    const Theme& t = theme();
    const FileModel* m = fileModel();
    const FileEntry* e = m ? m->entry(idx.row()) : nullptr;
    // Rows are rounded bars a little in from the edges: zebra, marks, the
    // drop target and the cursor alike.
    const QRectF r = QRectF(kRowInset, opt.rect.top(), viewport()->width() - 2 * kRowInset, opt.rect.height())
                         .adjusted(0.5, 0.5, -0.5, -0.5);
    auto withAlpha = [](QColor c, double a) {
        c.setAlphaF(a);
        return c;
    };
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    if (idx.row() % 2) {
        p->setBrush(t.altBase);
        p->drawRoundedRect(r, kRowRadius, kRowRadius);
    }
    if (e && e->marked) {
        p->setBrush(t.markBg);
        p->drawRoundedRect(r, kRowRadius, kRowRadius);
    }
    if (idx.row() == m_dropRow) {
        p->setBrush(t.dropTarget);
        p->drawRoundedRect(r, kRowRadius, kRowRadius);
    }
    if (idx.row() == currentIndex().row()) {
        // The panel's edge already says where the keys go: the row is only tinted.
        if (m_active && (hasFocus() || m_typingFor)) {
            p->setPen(Qt::NoPen);
            p->setBrush(withAlpha(t.accent, t.dark ? 0.22 : 0.16));
        } else if (m_active) {
            p->setPen(Qt::NoPen);
            p->setBrush(withAlpha(t.accent, 0.10));
        } else {
            p->setPen(QPen(t.cursorInactive, 1));
            p->setBrush(Qt::NoBrush);
        }
        p->drawRoundedRect(r, kRowRadius, kRowRadius);
    }
    p->restore();

    // The cells, by hand: QTreeView::drawRow would let the desktop style paint
    // its own focus and hover bars (full width, under the rounded cursor).
    QStyleOptionViewItem o = opt;
    o.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus | QStyle::State_MouseOver);
    o.showDecorationSelected = false;
    const QHeaderView* h = header();
    for (int section = 0; section < h->count(); ++section) {
        const int col = h->logicalIndex(section);
        if (h->isSectionHidden(col))
            continue;
        const int x = h->sectionViewportPosition(col);
        const int w = h->sectionSize(col);
        if (x + w < 0 || x > viewport()->width())
            continue;
        const QModelIndex cell = idx.sibling(idx.row(), col);
        QStyleOptionViewItem c = o;
        c.rect = QRect(x, opt.rect.top(), w, opt.rect.height());
        c.index = cell;
        itemDelegateForIndex(cell)->paint(p, c, cell);
    }
}

QString FileView::dropTargetFor(const QPoint& pos, int* row) const
{
    const FileModel* m = fileModel();
    if (!m)
        return {};
    const QModelIndex idx = indexAt(pos);
    const FileEntry* e = idx.isValid() ? m->entry(idx.row()) : nullptr;
    if (e && e->isDir) {
        *row = idx.row();
        return m->filePath(idx.row());
    }
    *row = -1;
    return m->path();
}

void FileView::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        e->ignore();
}

void FileView::dragMoveEvent(QDragMoveEvent* e)
{
    if (!e->mimeData()->hasUrls()) {
        e->ignore();
        return;
    }
    int row = -1;
    dropTargetFor(e->position().toPoint(), &row);
    if (row != m_dropRow) {
        m_dropRow = row;
        viewport()->update();
    }
    if (e->modifiers() & Qt::ShiftModifier)
        e->setDropAction(Qt::MoveAction);
    else if (e->modifiers() & Qt::ControlModifier)
        e->setDropAction(Qt::CopyAction);
    else
        e->setDropAction(e->proposedAction());
    e->accept();
    updateDropText(e->mimeData(), e->dropAction(), e->position().toPoint());
}

// "Copy 4 items into Projects": said before the drop, not after.
void FileView::updateDropText(const QMimeData* data, Qt::DropAction action, const QPoint& pos)
{
    int row = -1;
    QString target = dropTargetFor(pos, &row);
    target = baseName(target);
    const QList<QUrl> urls = data ? data->urls() : QList<QUrl>{};
    const QString what = urls.size() == 1 ? urls.first().fileName()
                                          : plural(tr("%n item(s)", nullptr, int(urls.size())), urls.size());
    QString text = action == Qt::MoveAction ? tr("Move %1 into %2") : action == Qt::LinkAction ? tr("Link %1 into %2")
                                                                                              : tr("Copy %1 into %2");
    text = text.arg(what, target);
    if (text != m_dropText) {
        m_dropText = text;
        viewport()->update();
    }
}

void FileView::paintEvent(QPaintEvent* e)
{
    QTreeView::paintEvent(e);
    if (m_dropText.isEmpty())
        return;
    const Theme& t = theme();
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    if (m_dropRow < 0) {  // into the folder itself: the whole list is the target
        QPen dash(t.accent, 2, Qt::DashLine);
        p.setPen(dash);
        QColor fill = t.accent;
        fill.setAlphaF(0.06);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(viewport()->rect()).adjusted(4, 4, -4, -4), 10, 10);
    }
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QString hint = tr("Shift: move · Ctrl: copy");
    const QFontMetrics bfm(bold), fm(font());
    const int w = qMin(viewport()->width() - 24, qMax(bfm.horizontalAdvance(m_dropText), fm.horizontalAdvance(hint)) + 32);
    const int h = bfm.height() + fm.height() + 20;
    const QRectF pill((viewport()->width() - w) / 2.0, viewport()->height() - h - 16, w, h);
    p.setPen(QPen(t.accent, 1));
    p.setBrush(t.header);
    p.drawRoundedRect(pill, 10, 10);
    p.setFont(bold);
    p.setPen(t.text);
    p.drawText(pill.adjusted(12, 8, -12, -(fm.height() + 10)), Qt::AlignCenter,
               bfm.elidedText(m_dropText, Qt::ElideMiddle, int(pill.width()) - 24));
    p.setFont(font());
    p.setPen(t.dimText);
    p.drawText(pill.adjusted(12, bfm.height() + 10, -12, -8), Qt::AlignCenter, hint);
}

void FileView::dragLeaveEvent(QDragLeaveEvent* e)
{
    m_dropRow = -1;
    m_dropText.clear();
    viewport()->update();
    QTreeView::dragLeaveEvent(e);
}

void FileView::dropEvent(QDropEvent* e)
{
    int row = -1;
    const QString target = dropTargetFor(e->position().toPoint(), &row);
    m_dropRow = -1;
    m_dropText.clear();
    viewport()->update();
    if (!e->mimeData()->hasUrls() || target.isEmpty()) {
        e->ignore();
        return;
    }
    Qt::DropAction action = e->proposedAction();
    if (e->modifiers() & Qt::ShiftModifier)
        action = Qt::MoveAction;
    else if (e->modifiers() & Qt::ControlModifier)
        action = Qt::CopyAction;
    e->setDropAction(action);
    e->accept();
    emit dropRequested(e->mimeData()->urls(), target, action);
}

void FileView::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    QTreeView::currentChanged(current, previous);
    if (m_grid && m_grid->isVisible())
        m_grid->ensureVisible(current.row());
    if (m_visual)
        applyVisual();
    if (current.row() != previous.row())
        emit cursorMoved();
}

// Narrow panel (e.g. with the preview column open): hide Attributes, then
// Modified, so names stay readable; bring them back when there is room.
void FileView::fitColumns()
{
    QHeaderView* h = header();
    if (!model() || h->count() < FileModel::ColCount)
        return;
    auto nameRoom = [&] {
        int others = 0;
        for (int c = 1; c < h->count(); ++c)
            if (!h->isSectionHidden(c))
                others += h->sectionSize(c);
        return viewport()->width() - others;
    };
    constexpr int kMinName = 210;
    // The extra columns go first, the date last.
    for (int col : {int(FileModel::ColLinkTarget), int(FileModel::ColAccessed), int(FileModel::ColCreated),
                    int(FileModel::ColOctal), int(FileModel::ColGroup), int(FileModel::ColOwner),
                    int(FileModel::ColAttr), int(FileModel::ColTime)}) {
        if (nameRoom() >= kMinName)
            break;
        if (!h->isSectionHidden(col)) {
            h->setSectionHidden(col, true);
            m_autoHidden.append(col);
        }
    }
    while (!m_autoHidden.isEmpty()) {
        const int col = m_autoHidden.last();
        if (nameRoom() - h->sectionSize(col) < kMinName + 40)
            break;
        h->setSectionHidden(col, false);
        m_autoHidden.removeLast();
    }
}

QByteArray FileView::userHeaderState()
{
    QHeaderView* h = header();
    for (int col : std::as_const(m_autoHidden))
        h->setSectionHidden(col, false);
    const QByteArray state = h->saveState();
    for (int col : std::as_const(m_autoHidden))
        h->setSectionHidden(col, true);
    return state;
}

void FileView::resizeEvent(QResizeEvent* e)
{
    QTreeView::resizeEvent(e);
    fitColumns();
}

void FileView::contextMenuEvent(QContextMenuEvent* e)
{
    if (e->reason() == QContextMenuEvent::Keyboard) {
        const QRect r = visualRect(currentIndex());
        emit contextMenuRequested(viewport()->mapToGlobal(QPoint(r.left() + 40, r.bottom())), currentRow());
        return;
    }
    const QModelIndex idx = indexAt(viewport()->mapFromGlobal(e->globalPos()));
    emit contextMenuRequested(e->globalPos(), idx.isValid() ? idx.row() : -1);
}

void FileView::showHeaderMenu(const QPoint& pos)
{
    QMenu menu(this);
    QHeaderView* h = header();
    for (int c = 1; c < h->count(); ++c) {  // the name column always stays
        if (c == FileModel::ColOwner)
            menu.addSeparator();  // the ones after it are for now and then
        QAction* a = menu.addAction(model()->headerData(c, Qt::Horizontal).toString());
        a->setCheckable(true);
        a->setChecked(!h->isSectionHidden(c));
        a->setChecked(!h->isSectionHidden(c) || m_autoHidden.contains(c));
        connect(a, &QAction::toggled, this, [this, h, c](bool on) {
            m_autoHidden.removeAll(c);
            h->setSectionHidden(c, !on);
            fitColumns();
        });
    }
    menu.exec(h->mapToGlobal(pos));
}

void FileView::startFileDrag()
{
    QStringList paths;
    emit dragPathsRequested(paths);
    if (paths.isEmpty())
        return;
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString& p : std::as_const(paths))
        urls.append(QUrl::fromLocalFile(p));
    mime->setUrls(urls);
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    const FileEntry* e = fileModel() ? fileModel()->entry(currentRow()) : nullptr;
    if (e)
        drag->setPixmap(model()->index(currentRow(), 0).data(Qt::DecorationRole).value<QIcon>().pixmap(32));
    drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::CopyAction);
}

}  // namespace tandem
