#include "ui/TrashDialog.h"

#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <sys/stat.h>

namespace tandem {

namespace {

enum Col { Name, From, Deleted, Size };

// Sorts by the real value behind dates and sizes.
class Row : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& o) const override
    {
        const int c = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (c == Deleted || c == Size)
            return data(c, Qt::UserRole).toLongLong() < o.data(c, Qt::UserRole).toLongLong();
        return text(c).compare(o.text(c), Qt::CaseInsensitive) < 0;
    }
};

QString whenText(int64_t secs)
{
    if (!secs)
        return QObject::tr("unknown");
    const QDateTime t = QDateTime::fromSecsSinceEpoch(secs);
    const qint64 days = t.date().daysTo(QDate::currentDate());
    if (days == 0)
        return QObject::tr("today %1").arg(t.toString(QStringLiteral("HH:mm")));
    if (days == 1)
        return QObject::tr("yesterday %1").arg(t.toString(QStringLiteral("HH:mm")));
    if (days < 7)
        return plural(QObject::tr("%n day(s) ago", nullptr, int(days)), int(days));
    return t.toString(QStringLiteral("yyyy-MM-dd"));
}

}  // namespace

TrashDialog::TrashDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Trash"));
    resize(980, 620);
    const Theme& t = theme();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto* top = new QHBoxLayout;
    m_summary = new QLabel(this);
    QFont f = m_summary->font();
    f.setBold(true);
    m_summary->setFont(f);
    top->addWidget(m_summary, 1);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Find by name or where it came from"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMinimumWidth(320);
    top->addWidget(m_filter);
    layout->addLayout(top);

    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({tr("Name"), tr("Came from"), tr("Deleted"), tr("Size")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setSortingEnabled(true);
    m_list->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    m_list->header()->resizeSection(From, 330);
    m_list->header()->resizeSection(Deleted, 130);
    m_list->header()->resizeSection(Size, 90);
    m_list->setStyleSheet(QStringLiteral("QTreeWidget::item { padding: 3px 2px; }"));
    m_list->installEventFilter(this);
    m_list->sortByColumn(Deleted, Qt::DescendingOrder);  // the latest first
    m_list->headerItem()->setTextAlignment(Size, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_list, 1);

    auto* bottom = new QHBoxLayout;
    m_empty = new QPushButton(tr("Empty trash…"), this);
    m_empty->setStyleSheet(t.css(QStringLiteral("QPushButton { color: @error; } QPushButton:disabled { color: @dimText; }")));
    m_empty->setAutoDefault(false);
    bottom->addWidget(m_empty);
    bottom->addWidget(new QLabel(QStringLiteral("  "), this));
    auto* keys = new QLabel(tr("Enter restores · Del deletes for good"), this);
    keys->setStyleSheet(t.css(QStringLiteral("color: @dimText;")));
    bottom->addWidget(keys, 1);
    m_delete = new QPushButton(tr("Delete permanently"), this);
    m_delete->setAutoDefault(false);
    m_restore = new QPushButton(tr("Restore"), this);
    m_restore->setDefault(true);
    auto* close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    bottom->addWidget(close);
    bottom->addWidget(m_delete);
    bottom->addWidget(m_restore);
    layout->addLayout(bottom);

    connect(m_filter, &QLineEdit::textChanged, this, &TrashDialog::fill);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &TrashDialog::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &TrashDialog::restoreSelected);
    connect(m_restore, &QPushButton::clicked, this, &TrashDialog::restoreSelected);
    connect(m_delete, &QPushButton::clicked, this, [this] { deleteSelected(true); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_empty, &QPushButton::clicked, this, [this] {
        if (QMessageBox::warning(this, tr("Empty trash"),
                                 plural(tr("Delete all %n item(s) in the trash for good? This cannot be undone.", nullptr,
                                           int(m_items.size())),
                                        int(m_items.size())),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        const QList<TrashItem> all = m_items;
        run([all] { return deleteTrashItems(all); }, tr("The trash is empty"));
    });
    reload();
}

void TrashDialog::reload()
{
    m_loading = true;
    m_summary->setText(tr("Reading the trash …"));
    updateButtons();
    QPointer<TrashDialog> self(this);
    ioPool()->start([self] {
        auto items = std::make_shared<QList<TrashItem>>(listTrash());
        postToUi([self, items] {
            if (!self)
                return;
            self->m_items = *items;
            self->m_loading = false;
            self->fill();
        });
    });
}

void TrashDialog::fill()
{
    const QString q = m_filter->text().trimmed();
    m_list->setSortingEnabled(false);
    m_list->clear();
    int64_t total = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        const TrashItem& it = m_items[i];
        total += it.size;
        const QString from = it.originalPath.isEmpty() ? tr("(unknown)") : parentPath(it.originalPath);
        const QString name = it.originalPath.isEmpty() ? it.name : baseName(it.originalPath);
        if (!q.isEmpty() && !name.contains(q, Qt::CaseInsensitive) && !from.contains(q, Qt::CaseInsensitive))
            continue;
        auto* row = new Row(m_list, {name, from, whenText(it.deletedAt), formatSize(it.size)});
        FileEntry e;
        e.name = name;
        e.isDir = it.isDir;
        e.mode = it.isDir ? S_IFDIR : S_IFREG;
        row->setIcon(Name, IconCache::forEntry(e));
        row->setData(Name, Qt::UserRole, i);
        row->setData(Deleted, Qt::UserRole, qlonglong(it.deletedAt));
        row->setData(Size, Qt::UserRole, qlonglong(it.size));
        row->setForeground(From, theme().dimText);
        row->setToolTip(From, it.originalPath);
        row->setTextAlignment(Size, Qt::AlignRight | Qt::AlignVCenter);
    }
    m_list->setSortingEnabled(true);
    if (m_list->topLevelItemCount() && m_list->selectedItems().isEmpty()) {
        m_list->setCurrentItem(m_list->topLevelItem(0));  // Enter and Del work right away
        m_list->setFocus();
    }
    m_summary->setText(m_items.isEmpty() ? tr("The trash is empty")
                                         : plural(tr("%n item(s)", nullptr, int(m_items.size())), int(m_items.size())) +
                                               QStringLiteral("  ·  ") + formatSize(total));
    updateButtons();
}

QList<TrashItem> TrashDialog::selectedItems() const
{
    QList<TrashItem> out;
    for (QTreeWidgetItem* r : m_list->selectedItems())
        out << m_items.value(r->data(Name, Qt::UserRole).toInt());
    return out;
}

void TrashDialog::updateButtons()
{
    const int n = int(m_list->selectedItems().size());
    m_restore->setEnabled(n > 0 && !m_loading);
    m_delete->setEnabled(n > 0 && !m_loading);
    m_empty->setEnabled(!m_items.isEmpty() && !m_loading);
    m_restore->setText(n > 1 ? plural(tr("Restore %n", nullptr, n), n) : tr("Restore"));
}

void TrashDialog::run(std::function<QStringList()> work, const QString& done)
{
    m_loading = true;
    updateButtons();
    QPointer<TrashDialog> self(this);
    ioPool()->start([self, work, done] {
        auto errors = std::make_shared<QStringList>(work());
        postToUi([self, errors, done] {
            if (!self)
                return;
            emit self->changed();
            self->reload();
            if (!errors->isEmpty())
                QMessageBox::warning(self, tr("Trash"), errors->join(QLatin1Char('\n')));
        });
    });
}

void TrashDialog::restoreSelected()
{
    const QList<TrashItem> items = selectedItems();
    if (items.isEmpty() || m_loading)
        return;
    run([items] { return restoreTrashItems(items); }, {});
}

void TrashDialog::deleteSelected(bool confirm)
{
    const QList<TrashItem> items = selectedItems();
    if (items.isEmpty() || m_loading)
        return;
    if (confirm &&
        QMessageBox::warning(this, tr("Delete permanently"),
                             plural(tr("Delete %n item(s) for good? This cannot be undone.", nullptr, int(items.size())),
                                    int(items.size())),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    run([items] { return deleteTrashItems(items); }, {});
}

bool TrashDialog::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_list && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Delete) {
            deleteSelected(true);
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            restoreSelected();
            return true;
        }
    }
    return QDialog::eventFilter(obj, e);
}

}  // namespace tandem
