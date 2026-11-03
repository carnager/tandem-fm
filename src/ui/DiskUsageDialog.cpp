#include "ui/DiskUsageDialog.h"

#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <sys/stat.h>

namespace tandem {

namespace {

enum Col { Name, Size, Share, Files };

// The share of the folder as a bar, the biggest item filling it.
class ShareDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);
        const double share = index.data(Qt::UserRole).toDouble();
        const Theme& t = theme();
        const bool selected = opt.state & QStyle::State_Selected;  // on the accent: light bar
        const QColor fill = selected ? opt.palette.color(QPalette::HighlightedText) : t.accent;
        const QColor label = selected ? opt.palette.color(QPalette::HighlightedText) : t.dimText;
        const QRectF track = QRectF(opt.rect).adjusted(6, opt.rect.height() / 2.0 - 4, -52, -(opt.rect.height() / 2.0 - 4));
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        QColor bg = label;
        bg.setAlpha(selected ? 70 : 40);
        p->setBrush(bg);
        p->drawRoundedRect(track, 4, 4);
        p->setBrush(fill);
        p->drawRoundedRect(QRectF(track.left(), track.top(), qMax(2.0, track.width() * share), track.height()), 4, 4);
        p->setPen(label);
        p->drawText(QRectF(track.right() + 6, opt.rect.top(), 46, opt.rect.height()), Qt::AlignVCenter | Qt::AlignRight,
                    share >= 0.001 ? QStringLiteral("%1%").arg(share * 100, 0, 'f', share < 0.1 ? 1 : 0) : QStringLiteral("<0.1%"));
        p->restore();
    }
};

}  // namespace

DiskUsageDialog::DiskUsageDialog(QString root, bool remote, QWidget* parent)
    : QDialog(parent), m_rootPath(std::move(root)), m_remote(remote)
{
    setWindowTitle(tr("Disk usage"));
    resize(900, 640);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    m_crumbs = new QHBoxLayout;
    m_crumbs->setSpacing(2);
    layout->addLayout(m_crumbs);
    m_summary = new QLabel(this);
    setThemeStyle(m_summary, QStringLiteral("color: @dimText;"));
    layout->addWidget(m_summary);

    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({tr("Name"), tr("Size"), tr("Share"), tr("Files")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setItemDelegateForColumn(Share, new ShareDelegate(m_list));
    m_list->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    m_list->header()->resizeSection(Size, 100);
    m_list->header()->resizeSection(Share, 260);
    m_list->header()->resizeSection(Files, 90);
    m_list->headerItem()->setTextAlignment(Size, Qt::AlignRight | Qt::AlignVCenter);
    m_list->headerItem()->setTextAlignment(Files, Qt::AlignRight | Qt::AlignVCenter);
    m_list->setStyleSheet(QStringLiteral("QTreeWidget::item { padding: 4px 2px; }"));
    m_list->installEventFilter(this);
    layout->addWidget(m_list, 1);

    auto* bottom = new QHBoxLayout;
    auto* keys = new QLabel(m_remote ? tr("Enter goes in · Backspace goes up · Del deletes (no trash on servers)")
                                     : tr("Enter goes in · Backspace goes up · Del moves to the trash"),
                            this);
    setThemeStyle(keys, QStringLiteral("color: @dimText;"));
    bottom->addWidget(keys, 1);
    m_show = new QPushButton(tr("Show in panel"), this);
    m_delete = new QPushButton(m_remote ? tr("Delete") : tr("Move to trash"), this);
    auto* close = new QPushButton(tr("Close"), this);
    for (QPushButton* b : {m_show, m_delete})
        b->setAutoDefault(false);
    close->setDefault(true);
    bottom->addWidget(m_delete);
    bottom->addWidget(m_show);
    bottom->addWidget(close);
    layout->addLayout(bottom);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_show, &QPushButton::clicked, this, [this] {
        if (UsageNode* n = selectedNode()) {
            emit showRequested(n->path());
            accept();
        }
    });
    connect(m_delete, &QPushButton::clicked, this, [this] { deleteSelected(true); });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (UsageNode* n = selectedNode(); n && n->isDir)
            enter(n);
    });

    // Count once, then browse.
    m_counting = true;
    m_summary->setText(tr("Counting …"));
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    QPointer<DiskUsageDialog> self(this);
    auto cancel = m_cancel;
    const QString rootPath = m_rootPath;
    ioPool()->start([self, rootPath, cancel] {
        QElapsedTimer tick;
        tick.start();
        auto progress = [&](int64_t files, int64_t bytes) {
            if (tick.elapsed() < 150)
                return;
            tick.restart();
            postToUi([self, files, bytes] {
                if (self && self->m_counting)
                    self->m_summary->setText(tr("Counting … %1 files, %2").arg(files).arg(formatSize(bytes)));
            });
        };
        auto tree = std::make_shared<std::unique_ptr<UsageNode>>(measureUsage(rootPath, cancel.get(), progress));
        if (cancel->load())
            return;
        postToUi([self, tree] {
            if (!self)
                return;
            self->m_root = std::move(*tree);
            self->m_counting = false;
            self->enter(self->m_root.get());
        });
    });
}

DiskUsageDialog::~DiskUsageDialog()
{
    m_cancel->store(true);
}

void DiskUsageDialog::enter(UsageNode* folder)
{
    UsageNode* from = m_current;
    m_current = folder;
    fill();
    // Coming back up: the cursor on the folder we came from.
    for (int i = 0; from && i < m_list->topLevelItemCount(); ++i)
        if (m_list->topLevelItem(i)->data(Name, Qt::UserRole).value<void*>() == from)
            m_list->setCurrentItem(m_list->topLevelItem(i));
    if (!m_list->currentItem() && m_list->topLevelItemCount())
        m_list->setCurrentItem(m_list->topLevelItem(0));
    m_list->setFocus();
}

void DiskUsageDialog::up()
{
    if (m_current && m_current->parent)
        enter(m_current->parent);
}

void DiskUsageDialog::fill()
{
    // Breadcrumbs from the measured folder down to here.
    while (QLayoutItem* it = m_crumbs->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    QList<UsageNode*> chain;
    for (UsageNode* n = m_current; n; n = n->parent)
        chain.prepend(n);
    for (int i = 0; i < chain.size(); ++i) {
        if (i)
            m_crumbs->addWidget(new QLabel(QStringLiteral("›"), this));
        auto* b = new QToolButton(this);
        b->setText(i == 0 ? chain[i]->name : chain[i]->name);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        if (i == chain.size() - 1) {
            QFont f = b->font();
            f.setBold(true);
            b->setFont(f);
        }
        UsageNode* target = chain[i];
        connect(b, &QToolButton::clicked, this, [this, target] { enter(target); });
        m_crumbs->addWidget(b);
    }
    m_crumbs->addStretch(1);

    m_list->clear();
    if (!m_current)
        return;
    const double total = qMax<int64_t>(1, m_current->size);
    for (const auto& c : m_current->children) {
        auto* row = new QTreeWidgetItem(m_list, {c->name, formatSize(c->size), QString(),
                                                 c->isDir ? QString::number(c->files) : QString()});
        FileEntry e;
        e.name = c->name;
        e.isDir = c->isDir;
        e.mode = c->isDir ? S_IFDIR : S_IFREG;
        row->setIcon(Name, IconCache::forEntry(e));
        row->setData(Name, Qt::UserRole, QVariant::fromValue(static_cast<void*>(c.get())));
        row->setData(Share, Qt::UserRole, double(c->size) / total);
        row->setTextAlignment(Size, Qt::AlignRight | Qt::AlignVCenter);
        row->setTextAlignment(Files, Qt::AlignRight | Qt::AlignVCenter);
        row->setForeground(Files, theme().dimText);
    }
    m_summary->setText(tr("%1 in %2 files").arg(formatSize(m_current->size)).arg(m_current->files));
}

UsageNode* DiskUsageDialog::selectedNode() const
{
    QTreeWidgetItem* it = m_list->currentItem();
    return it ? static_cast<UsageNode*>(it->data(Name, Qt::UserRole).value<void*>()) : nullptr;
}

void DiskUsageDialog::deleteSelected(bool confirm)
{
    QList<UsageNode*> nodes;
    for (QTreeWidgetItem* it : m_list->selectedItems())
        nodes << static_cast<UsageNode*>(it->data(Name, Qt::UserRole).value<void*>());
    if (nodes.isEmpty())
        return;
    int64_t bytes = 0;
    QStringList paths;
    for (UsageNode* n : nodes) {
        bytes += n->size;
        paths << n->path();
    }
    if (confirm) {
        const QString text = m_remote ? plural(tr("Delete %n item(s) (%1) for good? Servers have no trash.", nullptr, int(paths.size())), int(paths.size()))
                                            .arg(formatSize(bytes))
                                      : plural(tr("Move %n item(s) (%1) to the trash?", nullptr, int(paths.size())), int(paths.size()))
                                            .arg(formatSize(bytes));
        if (QMessageBox::question(this, windowTitle(), text) != QMessageBox::Yes)
            return;
    }
    emit deleteRequested(paths, !m_remote);
    const int row = m_list->indexOfTopLevelItem(m_list->currentItem());
    for (UsageNode* n : nodes)
        removeUsageNode(n);
    fill();
    if (m_list->topLevelItemCount())
        m_list->setCurrentItem(m_list->topLevelItem(qMin(row, m_list->topLevelItemCount() - 1)));
}

bool DiskUsageDialog::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_list && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        switch (k->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Right:
            if (UsageNode* n = selectedNode(); n && n->isDir)
                enter(n);
            return true;
        case Qt::Key_Backspace:
        case Qt::Key_Left:
            up();
            return true;
        case Qt::Key_Delete:
            deleteSelected(true);
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(obj, e);
}

}  // namespace tandem
