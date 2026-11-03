#include "ui/FileView.h"

#include "ui/ThumbGrid.h"

#include "core/Sorting.h"
#include "ui/FileColors.h"
#include "ui/FileModel.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDateTime>
#include <QDrag>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>

namespace tandem {

// ------------------------------------------------------------ FileDelegate

FileDelegate::FileDelegate(QTreeView* view) : QStyledItemDelegate(view), m_view(view) {}

QSize FileDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const
{
    const int h = qMax(QFontMetrics(FileView::zoomed(theme().listFont)).height() + 8,
                       22 * FileView::listZoom() / 100);
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

    QRect r = opt.rect.adjusted(6, 0, -6, 0);
    if (col == FileModel::ColName) {
        const int iconSize = 16 * FileView::listZoom() / 100;
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

    QFont f = FileView::zoomed((col == FileModel::ColSize || col == FileModel::ColTime) ? t.numberFont : t.listFont);
    if (e->isDir && col == FileModel::ColName)
        f.setWeight(QFont::DemiBold);
    if (e->marked)
        f.setWeight(QFont::Bold);
    p->setFont(f);
    // Secondary columns are dimmed so names stand out; marks keep their color.
    const bool secondary = col == FileModel::ColTime || col == FileModel::ColAttr ||
                           (col == FileModel::ColSize && e->isDir);
    if (secondary && !e->marked)
        fg = t.dimText;
    p->setPen(fg);

    const QString text = idx.data(Qt::DisplayRole).toString();
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
    const int iconSize = 16 * g_listZoom / 100;
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
        scheduleDelayedItemsLayout();  // a new font: rows of another height
    });
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
        "QHeaderView::section { background: @base; color: @dimText; border: none; padding: 4px 6px; }"
        "QHeaderView::section:hover { color: @text; }"));
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

bool FileView::event(QEvent* e)
{
    if (m_vim && e->type() == QEvent::ShortcutOverride) {
        // Vim's Ctrl+D / Ctrl+U scroll instead of running window commands.
        auto* ke = static_cast<QKeyEvent*>(e);
        if ((ke->modifiers() & Qt::ControlModifier) && (ke->key() == Qt::Key_D || ke->key() == Qt::Key_U)) {
            e->accept();
            return true;
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
        case Qt::Key_Home: setCurrentRow(0); return;
        case Qt::Key_End: setCurrentRow(model() ? model()->rowCount() - 1 : 0); return;
        default: break;
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
        if (m_visual || m_pendingSort) {  // leaves visual mode, keeping its marks
            m_pendingSort = false;
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

    if (m_vim) {
        const int page = qMax(1, viewport()->height() / qMax(1, sizeHintForRow(0)));
        if (mods == Qt::ControlModifier && (e->key() == Qt::Key_D || e->key() == Qt::Key_U)) {
            moveCursorBy(e->key() == Qt::Key_D ? page / 2 : -page / 2);
            return;
        }
        if (!(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            const QString t = e->text();
            // ,<key>: sort, like yazi.
            if (m_pendingSort) {
                m_pendingSort = false;
                emit hintRequested(QString());
                static const QHash<QString, QString> sorts = {
                    {QStringLiteral("n"), QStringLiteral("sort-by-name")},
                    {QStringLiteral("e"), QStringLiteral("sort-by-extension")},
                    {QStringLiteral("m"), QStringLiteral("sort-by-date")},
                    {QStringLiteral("s"), QStringLiteral("sort-by-size")},
                };
                if (sorts.contains(t))
                    emit commandIdRequested(sorts.value(t));
                return;
            }
            if (t == QLatin1String(",")) {
                m_pendingSort = true;
                emit hintRequested(tr("Sort by:  n name  ·  e extension  ·  m modified  ·  s size"));
                return;
            }
            // g followed by anything but g: a bookmark chain (gh = home…), like yazi.
            if (m_lastG && QDateTime::currentMSecsSinceEpoch() - m_lastG < 600 && !t.isEmpty() &&
                t != QLatin1String("g") && t.at(0).isPrint()) {
                m_lastG = 0;
                emit bookmarkJumpRequested(t);
                return;
            }
            if (t == QLatin1String("'")) { emit bookmarkJumpRequested(QString()); return; }
            if (t == QLatin1String("!")) { emit commandRequested(); return; }
            if (t == QLatin1String("m")) { emit bookmarkAssignRequested(); return; }
            if (t == QLatin1String("j")) { moveCursorBy(1); return; }
            if (t == QLatin1String("k")) { moveCursorBy(-1); return; }
            if (t == QLatin1String("h")) { emit parentRequested(); return; }
            if (t == QLatin1String("l")) { emit openRequested(); return; }
            if (t == QLatin1String("G")) { setCurrentRow(model() ? model()->rowCount() - 1 : 0); return; }
            if (t == QLatin1String("g")) {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - m_lastG < 600) {
                    setCurrentRow(0);
                    m_lastG = 0;
                } else {
                    m_lastG = now;
                }
                return;
            }
            if (t == QLatin1String("/")) { emit quickSearchRequested(QString()); return; }
            if (t == QLatin1String("n")) { emit searchNextRequested(1); return; }
            if (t == QLatin1String("N")) { emit searchNextRequested(-1); return; }
            if (t == QLatin1String(":")) { emit paletteRequested(); return; }
            // v / V: visual mode, moving marks (or unmarks) a range.
            if (t == QLatin1String("v") || t == QLatin1String("V")) {
                const int mode = t == QLatin1String("v") ? 1 : -1;
                setVisual(m_visual == mode ? 0 : mode);
                return;
            }
            // Commands on the selection and around it, yazi's keys.
            static const QHash<QString, QString> commands = {
                {QStringLiteral("y"), QStringLiteral("copy-to-clipboard")},
                {QStringLiteral("x"), QStringLiteral("cut-to-clipboard")},
                {QStringLiteral("p"), QStringLiteral("paste")},
                {QStringLiteral("d"), QStringLiteral("move-to-trash")},
                {QStringLiteral("D"), QStringLiteral("delete-permanently")},
                {QStringLiteral("r"), QStringLiteral("rename")},
                {QStringLiteral("a"), QStringLiteral("create")},
                {QStringLiteral("."), QStringLiteral("show-hidden-files")},
                {QStringLiteral("f"), QStringLiteral("filter-as-you-type")},
                {QStringLiteral("s"), QStringLiteral("search-files-names-or-contents")},
                {QStringLiteral("z"), QStringLiteral("jump-to-folder-search")},
                {QStringLiteral("t"), QStringLiteral("new-tab")},
                {QStringLiteral("q"), QStringLiteral("close-tab")},
                {QStringLiteral("["), QStringLiteral("previous-tab")},
                {QStringLiteral("]"), QStringLiteral("next-tab")},
                {QStringLiteral("w"), QStringLiteral("jobs")},
                {QStringLiteral("K"), QStringLiteral("preview-up")},
                {QStringLiteral("J"), QStringLiteral("preview-down")},
                {QStringLiteral("~"), QStringLiteral("keyboard-shortcuts")},
            };
            QString id = commands.value(t);
            if (t.size() == 1 && t.at(0) >= QLatin1Char('1') && t.at(0) <= QLatin1Char('9'))
                id = QStringLiteral("tab-") + t;  // tab slots
            if (!id.isEmpty()) {
                setVisual(0);  // the marks it made stay for the command
                emit commandIdRequested(id);
                return;
            }
            if (!t.isEmpty() && t.at(0).isPrint() && t != QLatin1String(" "))
                return;  // in vim mode letters are commands, not quick search
        }
    }

    const QString text = e->text();
    if (!text.isEmpty() && text.at(0).isPrint() && !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        emit quickSearchRequested(text);
        return;
    }
    QTreeView::keyPressEvent(e);
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
    const QRect r(0, opt.rect.top(), viewport()->width(), opt.rect.height());

    if (idx.row() % 2)
        p->fillRect(r, t.altBase);
    if (e && e->marked)
        p->fillRect(r, t.markBg);
    if (idx.row() == m_dropRow)
        p->fillRect(r, t.dropTarget);

    if (idx.row() == currentIndex().row()) {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF cr = QRectF(r).adjusted(2.5, 0.5, -2.5, -0.5);
        if (m_active && hasFocus()) {
            p->setPen(Qt::NoPen);
            p->setBrush(t.cursorActive);
            p->drawRoundedRect(cr, 5, 5);
        } else if (m_active) {
            p->setPen(QPen(t.accent, 1.2));
            p->setBrush(t.cursorActive.lighter(100));
            p->drawRoundedRect(cr, 5, 5);
        } else {
            p->setPen(QPen(t.cursorInactive, 1.2));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(cr, 5, 5);
        }
        p->restore();
    }

    QStyleOptionViewItem o = opt;
    o.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus | QStyle::State_MouseOver);
    QTreeView::drawRow(p, o, idx);
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
}

void FileView::dragLeaveEvent(QDragLeaveEvent* e)
{
    m_dropRow = -1;
    viewport()->update();
    QTreeView::dragLeaveEvent(e);
}

void FileView::dropEvent(QDropEvent* e)
{
    int row = -1;
    const QString target = dropTargetFor(e->position().toPoint(), &row);
    m_dropRow = -1;
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
    for (int col : {int(FileModel::ColAttr), int(FileModel::ColTime)}) {
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
