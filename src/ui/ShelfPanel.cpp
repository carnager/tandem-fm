#include "ui/ShelfPanel.h"

#include "core/DirLister.h"
#include "core/Backend.h"
#include "core/Shelves.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace tandem {

namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kFolderRole = Qt::UserRole + 1;
constexpr int kGoneRole = Qt::UserRole + 2;

// The name, then its folder dimmed; a gone one struck through.
class ItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const override
    {
        return {200, opt.fontMetrics.height() + 12};
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        const Theme& t = theme();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect.adjusted(2, 1, -2, -1);
        if (opt.state & QStyle::State_Selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(t.cursorActive);
            p->drawRoundedRect(r, 6, 6);
        }
        const int icon = opt.fontMetrics.height();
        const QIcon ic = idx.data(Qt::DecorationRole).value<QIcon>();
        ic.paint(p, QRect(r.left() + 8, r.center().y() - icon / 2, icon, icon));
        QRect text = r.adjusted(16 + icon, 0, -8, 0);
        const bool gone = idx.data(kGoneRole).toBool();
        QFont f = opt.font;
        f.setStrikeOut(gone);
        p->setFont(f);
        const QFontMetrics fm(f);
        const QString name = fm.elidedText(idx.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, text.width() * 3 / 5);
        p->setPen(gone ? t.dimText : t.text);
        p->drawText(text, Qt::AlignVCenter | Qt::AlignLeft, name);
        text.setLeft(text.left() + fm.horizontalAdvance(name) + 10);
        p->setPen(t.dimText);
        p->drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                    fm.elidedText(idx.data(kFolderRole).toString(), Qt::ElideMiddle, text.width()));
        p->restore();
    }
};

// Drags out as files; files dropped in go onto the shelf.
class ShelfList : public QListWidget {
public:
    explicit ShelfList(ShelfPanel* panel) : QListWidget(panel), m_panel(panel)
    {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::CopyAction);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
    }

protected:
    QStringList mimeTypes() const override { return {QStringLiteral("text/uri-list")}; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        QList<QUrl> urls;
        for (QListWidgetItem* it : items) {
            const QString path = it->data(kPathRole).toString();
            if (!isRemoteLocation(path) && !it->data(kGoneRole).toBool())
                urls.append(QUrl::fromLocalFile(path));
        }
        if (urls.isEmpty())
            return nullptr;
        auto* data = new QMimeData;
        data->setUrls(urls);
        return data;
    }
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction; }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->source() == this || !e->mimeData()->hasUrls())
            return;  // within the list: nothing to do
        e->setDropAction(Qt::LinkAction);
        e->accept();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (e->source() == this)
            return;
        e->setDropAction(Qt::LinkAction);  // the shelf only points at them
        e->accept();
    }
    void dropEvent(QDropEvent* e) override
    {
        if (e->source() == this)
            return;
        e->setDropAction(Qt::LinkAction);
        e->accept();
        emit m_panel->dropped(m_panel->shelf(), e->mimeData());
    }

private:
    ShelfPanel* m_panel;
};

}  // namespace

ShelfPanel::ShelfPanel(QWidget* window) : QFrame(window)
{
    const Theme& t = theme();
    setObjectName(QStringLiteral("shelfPanel"));
    setThemeStyle(this, QStringLiteral(
        "#shelfPanel { background: @header; border: 1px solid @border; border-radius: 12px; }"
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QLabel { color: @dimText; }"
        "QLabel#shelfTitle { color: @text; font-weight: 600; }"
        "QToolButton { color: @text; background: @base; border: 1px solid @border; border-radius: 8px; padding: 3px 10px; }"
        "QToolButton:hover { border-color: @accent; }"
        "QToolButton#shelfClose { background: transparent; border: none; color: @dimText; }"));
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, t.dark ? 150 : 60));
    setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 8, 8);
    layout->setSpacing(6);
    auto* head = new QHBoxLayout;
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("shelfTitle"));
    auto* closeButton = new QToolButton(this);
    closeButton->setObjectName(QStringLiteral("shelfClose"));
    closeButton->setText(QStringLiteral("✕"));
    closeButton->setToolTip(tr("Close (Esc)"));
    closeButton->setFocusPolicy(Qt::NoFocus);
    connect(closeButton, &QToolButton::clicked, this, &ShelfPanel::close);
    head->addWidget(m_title, 1);
    head->addWidget(closeButton);
    layout->addLayout(head);

    m_list = new ShelfList(this);
    m_list->setItemDelegate(new ItemDelegate(m_list));
    m_list->setFocusPolicy(Qt::ClickFocus);  // opening it leaves the keys with the panels
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->installEventFilter(this);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        emit showRequested(it->data(kPathRole).toString());
    });
    layout->addWidget(m_list, 1);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(4);
    auto button = [&](const QString& text, const QString& tip, const QString& name, auto fn) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(name);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QToolButton::clicked, this, fn);
        buttons->addWidget(b);
    };
    button(tr("Copy here"), tr("Copy everything on the shelf into the active panel's folder"), QStringLiteral("shelfCopy"),
           [this] { emit copyRequested(m_shelf, false); });
    button(tr("Move here"), tr("Move everything on the shelf into the active panel's folder"), QStringLiteral("shelfMove"),
           [this] { emit copyRequested(m_shelf, true); });
    buttons->addStretch(1);
    button(tr("Clear"), tr("Take everything off this shelf (the files stay)"), QStringLiteral("shelfClear"),
           [this] { Shelves::instance().clear(m_shelf); });
    layout->addLayout(buttons);
    auto* hint = new QLabel(tr("Drag files in or out  ·  Enter copies here, Shift+Enter moves  ·  "
                               "1–%1 another shelf  ·  Del takes off").arg(Shelves::kCount), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    connect(&Shelves::instance(), &Shelves::changed, this, [this] {
        if (!isVisible())
            return;
        if (Shelves::instance().items(m_shelf).isEmpty())
            close();
        else
            fill();
    });
    window->installEventFilter(this);  // follows the window's size
    hide();
}

void ShelfPanel::showShelf(int shelf, bool focus)
{
    if (Shelves::instance().items(shelf).isEmpty())
        return;
    if (focus && !m_list->hasFocus())
        m_returnFocus = QApplication::focusWidget();
    const bool other = shelf != m_shelf;
    m_shelf = shelf;
    fill();
    place();
    show();
    raise();
    if (focus) {
        m_list->setFocus();
        if (other || !m_list->currentItem())
            m_list->setCurrentRow(0);
    }
    emit shownShelfChanged(shelf);
}

void ShelfPanel::close()
{
    if (m_list->hasFocus()) {
        if (m_returnFocus)
            m_returnFocus->setFocus();  // back to the panel the keys came from
        else
            m_list->clearFocus();
    }
    m_returnFocus = nullptr;
    hide();
    m_shelf = 0;
    emit shownShelfChanged(0);
}

void ShelfPanel::place()
{
    QWidget* w = parentWidget();
    const int width = qMin(440, w->width() - 20);
    const int rows = qBound(1, m_list->count(), 10);
    const int rowHeight = m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0) : fontMetrics().height() + 12;
    m_list->setFixedHeight(rows * rowHeight + 4);
    adjustSize();
    const int h = sizeHint().height();
    const int bottom = m_above && m_above->isVisible() ? m_above->mapTo(w, QPoint(0, 0)).y() : w->height() - 4;
    setGeometry(w->width() - width - 10, qMax(10, bottom - h - 6), width, h);
}

void ShelfPanel::fill()
{
    const QStringList paths = Shelves::instance().items(m_shelf);
    QSet<QString> selected;
    for (QListWidgetItem* it : m_list->selectedItems())
        selected.insert(it->data(kPathRole).toString());
    QHash<QString, QListWidgetItem> old;
    for (int i = 0; i < m_list->count(); ++i)
        old.insert(m_list->item(i)->data(kPathRole).toString(), *m_list->item(i));
    m_list->clear();
    const QString home = QDir::homePath();
    for (const QString& path : paths) {
        auto* it = new QListWidgetItem(m_list);
        if (const auto o = old.constFind(path); o != old.cend()) {
            *it = *o;  // icon and gone from the last look
        } else {
            it->setText(baseName(path));
            it->setIcon(IconCache::themed("text-x-generic"));
        }
        QString dir = parentPath(path);
        if (dir == home || dir.startsWith(home + QLatin1Char('/')))
            dir = QLatin1Char('~') + dir.mid(home.size());
        it->setData(kPathRole, path);
        it->setData(kFolderRole, dir);
        it->setToolTip(path);
        it->setSelected(selected.contains(path));
    }
    m_title->setText(plural(tr("Shelf %1 · %n item(s)", nullptr, int(paths.size())), paths.size()).arg(m_shelf));
    if (isVisible())
        place();
    check();
}

void ShelfPanel::check()
{
    const QStringList paths = Shelves::instance().items(m_shelf);
    const quint64 gen = ++m_gen;
    QPointer<ShelfPanel> self(this);
    ioPool()->start([self, gen, paths] {
        QHash<QString, QStringList> byDir;
        for (const QString& p : paths)
            if (!isRemoteLocation(p))
                byDir[parentPath(p)] << baseName(p);
        auto found = std::make_shared<QHash<QString, FileEntry>>();
        for (auto it = byDir.cbegin(); it != byDir.cend(); ++it)
            for (auto& [name, e] : statNames(it.key(), it.value(), true))
                if (e)
                    found->insert(joinPath(it.key(), name), *e);
        postToUi([self, gen, found] {
            if (!self || gen != self->m_gen)
                return;
            for (int i = 0; i < self->m_list->count(); ++i) {
                QListWidgetItem* it = self->m_list->item(i);
                const QString path = it->data(kPathRole).toString();
                if (isRemoteLocation(path))
                    continue;
                const auto e = found->constFind(path);
                it->setData(kGoneRole, e == found->cend());
                if (e != found->cend())
                    it->setIcon(IconCache::forEntry(*e));
            }
        });
    });
}

void ShelfPanel::removeSelected()
{
    QStringList paths;
    for (QListWidgetItem* it : m_list->selectedItems())
        paths << it->data(kPathRole).toString();
    if (paths.isEmpty() && m_list->currentItem())
        paths << m_list->currentItem()->data(kPathRole).toString();
    const int row = m_list->currentRow();
    Shelves::instance().remove(m_shelf, paths);  // refills (or closes)
    if (isVisible() && m_list->count() > 0)
        m_list->setCurrentRow(qMin(row, m_list->count() - 1));
}

bool ShelfPanel::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == parentWidget() && e->type() == QEvent::Resize && isVisible())
        place();
    // Del here takes off the shelf: the window's Del (to the trash) mustn't see it.
    if (obj == m_list && e->type() == QEvent::ShortcutOverride) {
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace || key == Qt::Key_Escape ||
            key == Qt::Key_Return || key == Qt::Key_Enter || (key >= Qt::Key_1 && key < Qt::Key_1 + Shelves::kCount)) {
            e->accept();
            return true;
        }
    }
    if (obj == m_list && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) {
            removeSelected();
            return true;
        }
        if (ke->key() == Qt::Key_Escape) {
            close();
            return true;
        }
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            const int shelf = m_shelf;
            const bool move = ke->modifiers() & Qt::ShiftModifier;
            close();  // done with it: the keys go back to the panel
            emit copyRequested(shelf, move);
            return true;
        }
        if (ke->key() >= Qt::Key_1 && ke->key() < Qt::Key_1 + Shelves::kCount && !(ke->modifiers() & ~Qt::KeypadModifier)) {
            const int n = ke->key() - Qt::Key_0;
            if (!Shelves::instance().items(n).isEmpty())
                showShelf(n, true);
            return true;
        }
    }
    return QFrame::eventFilter(obj, e);
}

}  // namespace tandem
