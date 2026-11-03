#include "ui/DuplicatesDialog.h"

#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QThreadPool>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <sys/stat.h>

namespace tandem {

DuplicatesDialog::DuplicatesDialog(QString here, QString other, QWidget* parent)
    : QDialog(parent), m_here(std::move(here)), m_other(std::move(other))
{
    setWindowTitle(tr("Duplicate files"));
    resize(1000, 660);
    setThemeStyle(this, (QStringLiteral(
        "QToolButton#segment { padding: 5px 12px; border: 1px solid @control; background: @base; }"
        "QToolButton#segment:checked { background: @accent; color: @accentText; border-color: @accent; }"
        "QTreeWidget::item { padding: 3px 2px; }")));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    // Where and what.
    auto* where = new QHBoxLayout;
    where->setSpacing(14);
    auto* in = new QLabel(tr("In %1").arg(m_here), this);
    in->setToolTip(m_here);
    where->addWidget(in);
    m_both = new QCheckBox(tr("and %1").arg(m_other), this);
    m_both->setVisible(!m_other.isEmpty() && m_other != m_here);
    where->addWidget(m_both);
    where->addStretch(1);
    layout->addLayout(where);
    auto* what = new QHBoxLayout;
    what->setSpacing(0);
    m_min = new QButtonGroup(this);
    const QList<QPair<qint64, QString>> sizes = {{1, tr("Any size")}, {1 << 20, tr("From 1 MB")}, {100 << 20, tr("From 100 MB")}};
    for (int i = 0; i < sizes.size(); ++i) {
        auto* b = new QToolButton(this);
        b->setObjectName(QStringLiteral("segment"));
        b->setText(sizes[i].second);
        b->setCheckable(true);
        b->setProperty("minSize", sizes[i].first);
        m_min->addButton(b, i);
        what->addWidget(b);
    }
    m_min->button(0)->setChecked(true);
    what->addSpacing(16);
    m_hidden = new QCheckBox(tr("Hidden files too"), this);
    what->addWidget(m_hidden);
    what->addStretch(1);
    m_find = new QPushButton(tr("Find duplicates"), this);
    what->addWidget(m_find);
    layout->addLayout(what);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({tr("File"), tr("Folder"), tr("Modified")});
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->resizeSection(0, 330);
    m_tree->header()->resizeSection(2, 140);
    layout->addWidget(m_tree, 1);

    auto* bottom = new QHBoxLayout;
    m_summary = new QLabel(this);
    setThemeStyle(m_summary, (QStringLiteral("color: @dimText;")));
    bottom->addWidget(m_summary, 1);
    auto* newest = new QPushButton(tr("Keep the newest"), this);
    newest->setToolTip(tr("Tick every copy but the newest in each group"));
    auto* oldest = new QPushButton(tr("Keep the oldest"), this);
    oldest->setToolTip(tr("Tick every copy but the oldest (the original, usually)"));
    auto* close = new QPushButton(tr("Close"), this);
    m_trash = new QPushButton(tr("Move to trash"), this);
    for (QPushButton* b : {newest, oldest, close})
        b->setAutoDefault(false);
    bottom->addWidget(newest);
    bottom->addWidget(oldest);
    bottom->addSpacing(12);
    bottom->addWidget(close);
    bottom->addWidget(m_trash);
    layout->addLayout(bottom);

    connect(m_find, &QPushButton::clicked, this, &DuplicatesDialog::search);
    connect(newest, &QPushButton::clicked, this, [this] { keep(true); });
    connect(oldest, &QPushButton::clicked, this, [this] { keep(false); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_trash, &QPushButton::clicked, this, [this] {
        const QStringList paths = ticked();
        if (paths.isEmpty())
            return;
        emit trashRequested(paths);
        // Out of the results; groups with one file left aren't duplicates anymore.
        for (DuplicateGroup& g : m_groups)
            g.files.removeIf([&](const DuplicateFile& f) { return paths.contains(f.path); });
        m_groups.removeIf([](const DuplicateGroup& g) { return g.files.size() < 2; });
        fill();
    });
    connect(m_tree, &QTreeWidget::itemChanged, this, &DuplicatesDialog::updateSummary);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        const QString path = it->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            emit showRequested(path);
    });
    m_summary->setText(tr("Same contents, found by size, then a quick look, then a full comparison."));
    m_trash->setEnabled(false);
    m_find->setDefault(true);
}

DuplicatesDialog::~DuplicatesDialog()
{
    if (m_cancel)
        m_cancel->store(true);
}

void DuplicatesDialog::search()
{
    if (m_cancel)
        m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_searching = true;
    m_find->setEnabled(false);
    m_tree->clear();
    QStringList roots{m_here};
    if (m_both->isVisible() && m_both->isChecked())
        roots << m_other;
    DuplicateOptions opt;
    opt.minSize = m_min->checkedButton()->property("minSize").toLongLong();
    opt.hidden = m_hidden->isChecked();
    m_summary->setText(tr("Looking …"));
    QPointer<DuplicatesDialog> self(this);
    auto cancel = m_cancel;
    ioPool()->start([self, roots, opt, cancel] {
        QElapsedTimer tick;
        tick.start();
        auto progress = [&](int64_t files, int64_t bytes) {
            if (tick.elapsed() < 150)
                return;
            tick.restart();
            postToUi([self, files, bytes] {
                if (self && self->m_searching)
                    self->m_summary->setText(tr("Looking … %1 files, %2 compared").arg(files).arg(formatSize(bytes)));
            });
        };
        auto groups = std::make_shared<QList<DuplicateGroup>>(findDuplicates(roots, opt, cancel.get(), progress));
        if (cancel->load())
            return;
        postToUi([self, groups] {
            if (!self)
                return;
            self->m_searching = false;
            self->m_find->setEnabled(true);
            self->m_groups = *groups;
            self->fill();
        });
    });
}

void DuplicatesDialog::fill()
{
    const QSignalBlocker block(m_tree);
    m_tree->clear();
    const Theme& t = theme();
    for (int g = 0; g < m_groups.size(); ++g) {
        const DuplicateGroup& group = m_groups[g];
        auto* head = new QTreeWidgetItem(m_tree);
        head->setText(0, tr("%1 copies · %2 each").arg(group.files.size()).arg(formatSize(group.size)));
        QFont f = head->font(0);
        f.setBold(true);
        head->setFont(0, f);
        head->setFirstColumnSpanned(true);
        head->setFlags(Qt::ItemIsEnabled);
        for (const DuplicateFile& file : group.files) {
            auto* it = new QTreeWidgetItem(head, {baseName(file.path), parentPath(file.path),
                                                  QDateTime::fromSecsSinceEpoch(file.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"))});
            FileEntry e;
            e.name = baseName(file.path);
            e.mode = S_IFREG;
            it->setIcon(0, IconCache::forEntry(e));
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            it->setCheckState(0, Qt::Unchecked);
            it->setData(0, Qt::UserRole, file.path);
            it->setData(0, Qt::UserRole + 1, qlonglong(file.mtime));
            it->setForeground(1, t.dimText);
            it->setToolTip(0, file.path);
        }
        head->setExpanded(true);
    }
    updateSummary();
}

void DuplicatesDialog::keep(bool newest)
{
    const QSignalBlocker block(m_tree);
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem* head = m_tree->topLevelItem(g);
        int keepAt = 0;
        for (int i = 1; i < head->childCount(); ++i) {
            const qint64 a = head->child(i)->data(0, Qt::UserRole + 1).toLongLong();
            const qint64 b = head->child(keepAt)->data(0, Qt::UserRole + 1).toLongLong();
            if (newest ? a > b : a < b)
                keepAt = i;
        }
        for (int i = 0; i < head->childCount(); ++i)
            head->child(i)->setCheckState(0, i == keepAt ? Qt::Unchecked : Qt::Checked);
    }
    updateSummary();
}

QStringList DuplicatesDialog::ticked() const
{
    QStringList out;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g)
        for (int i = 0; i < m_tree->topLevelItem(g)->childCount(); ++i)
            if (m_tree->topLevelItem(g)->child(i)->checkState(0) == Qt::Checked)
                out << m_tree->topLevelItem(g)->child(i)->data(0, Qt::UserRole).toString();
    return out;
}

void DuplicatesDialog::updateSummary()
{
    if (m_searching)
        return;
    int64_t waste = 0, freed = 0;
    for (const DuplicateGroup& g : std::as_const(m_groups))
        waste += g.size * (g.files.size() - 1);
    int n = 0;
    bool wholeGroupTicked = false;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem* head = m_tree->topLevelItem(g);
        int ticked = 0;
        for (int i = 0; i < head->childCount(); ++i)
            if (head->child(i)->checkState(0) == Qt::Checked) {
                ++ticked;
                freed += m_groups.value(g).size;
            }
        n += ticked;
        wholeGroupTicked |= ticked && ticked == head->childCount();
    }
    QString text = m_groups.isEmpty() ? tr("No duplicates.")
                                      : tr("%1 groups · %2 could be freed").arg(m_groups.size()).arg(formatSize(waste));
    if (n)
        text += tr("  ·  %1 ticked (%2)").arg(n).arg(formatSize(freed));
    if (wholeGroupTicked)
        text += tr("  ·  careful: every copy of something is ticked");
    m_summary->setText(text);
    setThemeStyle(m_summary, (wholeGroupTicked ? QStringLiteral("color: @error;") : QStringLiteral("color: @dimText;")));
    m_trash->setEnabled(n > 0);
    m_trash->setText(n ? plural(tr("Move %n to the trash", nullptr, n), n) : tr("Move to trash"));
    m_trash->setDefault(n > 0);
}

}  // namespace tandem
