#include "ui/SyncDialog.h"

#include "core/Job.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QPainter>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <mutex>

#include <sys/stat.h>

namespace tandem {

// ------------------------------------------------------------------ model

int SyncModel::groupOf(size_t i) const
{
    switch (m_actions[i]) {
    case SyncAction::ToRight: return GroupToRight;
    case SyncAction::ToLeft: return GroupToLeft;
    case SyncAction::DeleteLeft:
    case SyncAction::DeleteRight: return GroupDelete;
    case SyncAction::None: break;
    }
    return m_items[i].diff == Diff::Equal ? GroupEqual : GroupDiffers;
}

int SyncModel::groupCount(int group) const
{
    int n = 0;
    for (size_t i = 0; i < m_items.size(); ++i)
        n += groupOf(i) == group;
    return n;
}

void SyncModel::setItems(std::vector<CompareItem> items, SyncMode mode)
{
    beginResetModel();
    m_items = std::move(items);
    m_actions.clear();
    for (const CompareItem& it : m_items)
        m_actions.push_back(defaultAction(it.diff, mode));
    m_visible.clear();
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_shown[groupOf(i)])
            m_visible.push_back(i);
    endResetModel();
}

void SyncModel::applyMode(SyncMode mode)
{
    for (size_t i = 0; i < m_items.size(); ++i)
        m_actions[i] = defaultAction(m_items[i].diff, mode);
    rebuildVisible();
}

void SyncModel::setShown(int group, bool on)
{
    m_shown[group] = on;
    rebuildVisible();
}

void SyncModel::rebuildVisible()
{
    beginResetModel();
    m_visible.clear();
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_shown[groupOf(i)])
            m_visible.push_back(i);
    endResetModel();
}

int SyncModel::rowOf(const QString& rel) const
{
    for (size_t r = 0; r < m_visible.size(); ++r)
        if (m_items[m_visible[r]].rel == rel)
            return int(r);
    return -1;
}

QList<SyncAction> SyncModel::possibleActions(int row) const
{
    const CompareItem& it = item(row);
    QList<SyncAction> out;
    const bool clash = it.left.exists && it.right.exists && it.left.isDir != it.right.isDir;
    if (it.left.exists && !clash)
        out << SyncAction::ToRight;
    if (it.right.exists && !clash)
        out << SyncAction::ToLeft;
    out << SyncAction::None;
    if (it.left.exists && !it.right.exists)
        out << SyncAction::DeleteLeft;
    if (it.right.exists && !it.left.exists)
        out << SyncAction::DeleteRight;
    return out;
}

bool SyncModel::setAction(int row, SyncAction a)
{
    if (row < 0 || row >= rowCount() || !possibleActions(row).contains(a))
        return false;
    m_actions[m_visible[size_t(row)]] = a;
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
    return true;
}

QVariant SyncModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Action: return tr("What happens");
    case Item: return tr("Item");
    case Why: return tr("Why");
    case Size: return tr("Size");
    }
    return {};
}

static QString ago(int64_t secs)
{
    secs = qAbs(secs);
    if (secs < 120)
        return QObject::tr("%1 s").arg(secs);
    if (secs < 7200)
        return QObject::tr("%1 min").arg(secs / 60);
    if (secs < 2 * 86400)
        return QObject::tr("%1 h").arg(secs / 3600);
    return QObject::tr("%1 days").arg(secs / 86400);
}

// Why an item is listed, in words.
static QString reason(const CompareItem& it)
{
    switch (it.diff) {
    case Diff::Equal: return QObject::tr("identical");
    case Diff::LeftNewer: return QObject::tr("newer on the left (by %1)").arg(ago(it.left.mtime - it.right.mtime));
    case Diff::RightNewer: return QObject::tr("newer on the right (by %1)").arg(ago(it.right.mtime - it.left.mtime));
    case Diff::LeftOnly: return QObject::tr("only on the left");
    case Diff::RightOnly: return QObject::tr("only on the right");
    case Diff::Differs: return QObject::tr("same time, different contents: choose a side");
    case Diff::TypeMismatch: return QObject::tr("a file on one side, a folder on the other");
    }
    return {};
}

QString actionLabel(SyncAction a, Diff d)
{
    switch (a) {
    case SyncAction::ToRight: return QObject::tr("copy →");
    case SyncAction::ToLeft: return QObject::tr("← copy");
    case SyncAction::DeleteLeft: return QObject::tr("delete left");
    case SyncAction::DeleteRight: return QObject::tr("delete right");
    case SyncAction::None:
        return d == Diff::Equal ? QObject::tr("same")
               : d == Diff::Differs || d == Diff::TypeMismatch ? QObject::tr("decide") : QObject::tr("skip");
    }
    return {};
}

QVariant SyncModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};
    const CompareItem& it = item(index.row());
    const SyncAction a = action(index.row());
    const Theme& t = theme();
    const int col = index.column();
    // What a copy carries: the source side's size.
    const CompareSide& from = a == SyncAction::ToLeft || (a == SyncAction::None && !it.left.exists) ||
                                      a == SyncAction::DeleteRight
                                  ? it.right
                                  : it.left;
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Action: return actionLabel(a, it.diff);
        case Item: return it.rel;
        case Why: return reason(it);
        case Size:
            if (!from.exists || (from.isDir && from.size < 0))
                return QString();
            return formatSize(from.size);
        }
        return {};
    case Qt::DecorationRole:
        if (col == Item) {
            FileEntry e;
            e.name = it.rel.section(QLatin1Char('/'), -1);
            e.isDir = (it.left.exists ? it.left.isDir : it.right.isDir);
            e.mode = e.isDir ? S_IFDIR : S_IFREG;
            return IconCache::forEntry(e);
        }
        return {};
    case Qt::TextAlignmentRole:
        return col == Size ? int(Qt::AlignRight | Qt::AlignVCenter) : int(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::FontRole:
        return col == Size ? QVariant(t.numberFont) : QVariant();
    case Qt::ForegroundRole:
        if (col == Why || col == Size)
            return t.dimText;
        return {};
    case Qt::ToolTipRole:
        if (col == Action)
            return tr("Click to change · → ← direction · Space skip · Del delete");
        return QStringLiteral("%1\n%2").arg(it.rel, reason(it));
    default:
        return {};
    }
}

void SyncDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    if (index.column() != SyncModel::Action && index.column() != SyncModel::Item) {
        QStyledItemDelegate::paint(p, option, index);
        return;
    }
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QString text = opt.text;
    opt.text.clear();
    if (index.column() == SyncModel::Action)
        opt.icon = QIcon();
    opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);
    const Theme& t = theme();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (index.column() == SyncModel::Action) {
        const auto* m = static_cast<const SyncModel*>(index.model());
        const SyncAction a = m->action(index.row());
        const Diff d = m->item(index.row()).diff;
        QColor c = t.dimText;
        bool strong = false;
        if (a == SyncAction::ToRight || a == SyncAction::ToLeft) {
            c = t.accent;
            strong = true;
        } else if (a == SyncAction::DeleteLeft || a == SyncAction::DeleteRight) {
            c = t.error;
            strong = true;
        } else if (d == Diff::Differs || d == Diff::TypeMismatch) {
            c = t.gitModified;
            strong = true;
        }
        if (opt.state & QStyle::State_Selected)
            c = opt.palette.color(QPalette::HighlightedText);  // the row is already accent-colored
        QFont f = opt.font;
        f.setBold(strong);
        p->setFont(f);
        const QFontMetrics fm(f);
        const int ph = fm.height() + 6;
        const QRect pill(opt.rect.left() + 6, opt.rect.top() + (opt.rect.height() - ph) / 2, fm.horizontalAdvance(text) + 20, ph);
        QColor bg = c;
        bg.setAlpha(strong ? (t.dark ? 55 : 35) : 0);
        p->setBrush(bg);
        QColor border = c;
        border.setAlpha(strong ? 140 : 70);
        p->setPen(QPen(border, 1));
        p->drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), pill.height() / 2.0, pill.height() / 2.0);
        p->setPen(strong || (opt.state & QStyle::State_Selected) ? c : t.dimText);
        p->drawText(pill, Qt::AlignCenter, text);
    } else {
        // Icon, then "folder/" dimmed and the name in full strength.
        const int iconSize = opt.decorationSize.width();
        QRect r = opt.rect.adjusted(6, 0, -4, 0);
        if (!opt.icon.isNull()) {
            opt.icon.paint(p, QRect(r.left(), r.top() + (r.height() - iconSize) / 2, iconSize, iconSize));
            r.setLeft(r.left() + iconSize + 6);
        }
        const int slash = int(text.lastIndexOf(QLatin1Char('/')));
        const QString dir = slash >= 0 ? text.left(slash + 1) : QString();
        const QString name = text.mid(slash + 1);
        const QFontMetrics fm(opt.font);
        p->setFont(opt.font);
        const bool selected = opt.state & QStyle::State_Selected;
        const QString dirShown = fm.elidedText(dir, Qt::ElideMiddle, qMax(0, r.width() - fm.horizontalAdvance(name)));
        p->setPen(selected ? opt.palette.color(QPalette::HighlightedText) : t.dimText);
        p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, dirShown);
        p->setPen(selected ? opt.palette.color(QPalette::HighlightedText) : t.text);
        p->drawText(r.adjusted(fm.horizontalAdvance(dirShown), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                    fm.elidedText(name, Qt::ElideRight, r.width() - fm.horizontalAdvance(dirShown)));
    }
    p->restore();
}

QSize SyncDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QSize sz = QStyledItemDelegate::sizeHint(option, index);
    sz.setHeight(qMax(sz.height(), option.fontMetrics.height() + 14));  // room for the pills
    return sz;
}

// ----------------------------------------------------------------- dialog

SyncDialog::SyncDialog(const QString& left, const QString& right, Runner runner, QWidget* parent)
    : QDialog(parent), m_left(left), m_right(right), m_runner(std::move(runner))
{
    const Theme& t = theme();
    setWindowTitle(tr("Synchronize folders"));
    resize(1040, 680);
    setStyleSheet(t.css(QStringLiteral(
        "QToolButton#segment { padding: 5px 14px; border: 1px solid @border; background: @base; }"
        "QToolButton#segment:checked { background: @accent; color: @accentText; border-color: @accent; }"
        "QToolButton#show { color: @dimText; border: 1px solid transparent; border-radius: 12px; padding: 3px 12px; }"
        "QToolButton#show:checked { color: @text; background: @altBase; border-color: @border; }"
        "QToolButton#show:hover { border-color: @accent; }"
        "QLabel#paths { font-weight: 600; font-size: 11pt; }"
        "QLabel#status, QLabel#keys { color: @dimText; }")));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    // Top: the two folders, the direction, the rest tucked into a menu.
    auto* top = new QHBoxLayout;
    top->setSpacing(10);
    m_paths = new QLabel(this);
    m_paths->setObjectName(QStringLiteral("paths"));
    m_paths->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_paths->setToolTip(QStringLiteral("%1\n%2").arg(left, right));
    top->addWidget(m_paths, 1);

    QSettings s;
    m_mode = new QButtonGroup(this);
    m_mode->setExclusive(true);
    auto* segments = new QHBoxLayout;
    segments->setSpacing(0);
    const QList<std::tuple<SyncMode, QString, QString>> modes = {
        {SyncMode::TwoWay, tr("⇄  both ways"), tr("Newer files win on both sides; nothing is deleted")},
        {SyncMode::MirrorToRight, tr("→  make right like left"),
         tr("The right side becomes a copy of the left: newer files there are overwritten, extra ones go to the trash")},
        {SyncMode::MirrorToLeft, tr("←  make left like right"),
         tr("The left side becomes a copy of the right: newer files there are overwritten, extra ones go to the trash")}};
    for (const auto& [m, label, tip] : modes) {
        auto* b = new QToolButton(this);
        b->setObjectName(QStringLiteral("segment"));
        b->setText(label);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        m_mode->addButton(b, int(m));
        segments->addWidget(b);
    }
    if (QAbstractButton* b = m_mode->button(qBound(0, s.value(QStringLiteral("sync/mode"), 0).toInt(), 2)))
        b->setChecked(true);
    top->addLayout(segments);

    auto* more = new QToolButton(this);
    more->setText(QStringLiteral("⋯"));
    more->setToolTip(tr("Options"));
    more->setPopupMode(QToolButton::InstantPopup);
    more->setFocusPolicy(Qt::NoFocus);
    auto* menu = new QMenu(more);
    auto option = [&](const QString& text, const QString& key, bool def, const QString& tip = {}) {
        QAction* a = menu->addAction(text);
        a->setCheckable(true);
        a->setChecked(s.value(key, def).toBool());
        a->setToolTip(tip);
        return a;
    };
    m_recursive = option(tr("Include subfolders"), QStringLiteral("sync/recursive"), true);
    m_hidden = option(tr("Include hidden files"), QStringLiteral("sync/hidden"), true);
    m_contents = option(tr("Compare contents (slower, exact)"), QStringLiteral("sync/contents"), false,
                        tr("Read files with equal sizes byte by byte instead of trusting their times"));
    menu->setToolTipsVisible(true);
    menu->addSeparator();
    menu->addAction(tr("Compare again"), QKeySequence(Qt::Key_F5), this, &SyncDialog::compare);
    more->setMenu(menu);
    top->addWidget(more);
    layout->addLayout(top);

    // The plan in numbers; each one also shows or hides its lines.
    auto* filters = new QHBoxLayout;
    filters->setSpacing(4);
    m_model = new SyncModel(this);
    for (int g = 0; g < SyncModel::GroupCount; ++g) {
        m_show[g] = new QToolButton(this);
        m_show[g]->setObjectName(QStringLiteral("show"));
        m_show[g]->setCheckable(true);
        m_show[g]->setChecked(m_model->isShown(g));
        m_show[g]->setFocusPolicy(Qt::NoFocus);
        m_show[g]->setToolTip(tr("Show or hide these lines"));
        filters->addWidget(m_show[g]);
        connect(m_show[g], &QToolButton::toggled, this, [this, g](bool on) {
            m_model->setShown(g, on);
            updateSummary();
        });
    }
    filters->addStretch(1);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("status"));
    filters->addWidget(m_status);
    layout->addLayout(filters);

    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setItemDelegate(new SyncDelegate(m_view));
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setFont(t.listFont);
    m_view->installEventFilter(this);
    QHeaderView* h = m_view->header();
    h->setStretchLastSection(false);
    h->setSectionResizeMode(SyncModel::Item, QHeaderView::Stretch);
    h->resizeSection(SyncModel::Action, 130);
    h->resizeSection(SyncModel::Why, 330);
    h->resizeSection(SyncModel::Size, 90);
    layout->addWidget(m_view, 1);

    auto* bottom = new QHBoxLayout;
    auto* keys = new QLabel(tr("→ ← direction · Space skip · Del delete · Enter show differences · Ctrl+Enter sync"), this);
    keys->setObjectName(QStringLiteral("keys"));
    m_syncButton = new QPushButton(tr("Sync"), this);
    m_syncButton->setDefault(true);
    m_syncButton->setMinimumWidth(140);
    auto* close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    bottom->addWidget(keys, 1);
    bottom->addWidget(close);
    bottom->addWidget(m_syncButton);
    layout->addLayout(bottom);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_syncButton, &QPushButton::clicked, this, &SyncDialog::synchronize);
    for (QAction* a : {m_recursive, m_contents, m_hidden})
        connect(a, &QAction::toggled, this, [this] {
            saveOptions();
            compare();
        });
    connect(m_mode, &QButtonGroup::idToggled, this, [this](int, bool on) {
        if (!on)
            return;
        saveOptions();
        m_model->applyMode(mode());
        updateSummary();
    });
    connect(m_view, &QTreeView::clicked, this, [this](const QModelIndex& i) {
        if (i.column() == SyncModel::Action)
            cycleAction(i.row());
    });
    connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex& i) {
        if (i.column() != SyncModel::Action)
            compareFiles(i.row());
    });
    connect(m_view, &QTreeView::customContextMenuRequested, this, &SyncDialog::showContextMenu);

    compare();
}

void SyncDialog::setMode(SyncMode m)
{
    if (QAbstractButton* b = m_mode->button(int(m)))
        b->setChecked(true);
}

SyncDialog::~SyncDialog()
{
    if (m_cancel)
        m_cancel->store(true);
}

void SyncDialog::resizeEvent(QResizeEvent* e)
{
    QDialog::resizeEvent(e);
    // Long paths: shorten each to half the width, keeping both ends.
    const QFontMetrics fm(m_paths->font());
    const int half = qMax(80, (m_paths->width() - fm.horizontalAdvance(QStringLiteral("   ⇄   "))) / 2);
    auto shortPath = [&](QString p) {
        const QString home = QDir::homePath();
        if (p == home || p.startsWith(home + QLatin1Char('/')))
            p = QLatin1Char('~') + p.mid(home.size());
        return fm.elidedText(p, Qt::ElideMiddle, half);
    };
    m_paths->setText(QStringLiteral("%1   ⇄   %2").arg(shortPath(m_left), shortPath(m_right)));
}

void SyncDialog::done(int r)
{
    if (m_cancel)
        m_cancel->store(true);
    QDialog::done(r);
}

SyncMode SyncDialog::mode() const
{
    return SyncMode(qMax(0, m_mode->checkedId()));
}

void SyncDialog::saveOptions()
{
    QSettings s;
    s.setValue(QStringLiteral("sync/recursive"), m_recursive->isChecked());
    s.setValue(QStringLiteral("sync/contents"), m_contents->isChecked());
    s.setValue(QStringLiteral("sync/hidden"), m_hidden->isChecked());
    s.setValue(QStringLiteral("sync/mode"), m_mode->checkedId());
}

void SyncDialog::compare()
{
    if (m_cancel)
        m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_comparing = true;
    m_syncButton->setEnabled(false);
    m_status->setText(tr("Comparing …"));
    for (QToolButton* b : m_show)  // nothing to count yet
        b->hide();
    CompareOptions opt;
    opt.recursive = m_recursive->isChecked();
    opt.contents = m_contents->isChecked();
    opt.hidden = m_hidden->isChecked();
    const quint64 gen = ++m_gen;
    QPointer<SyncDialog> self(this);
    auto cancel = m_cancel;
    const QString left = m_left, right = m_right;
    ioPool()->start([self, gen, cancel, left, right, opt] {
        QElapsedTimer tick;
        tick.start();
        std::mutex lock;  // folders are listed on several threads
        int folders = 0;
        auto progress = [&](const QString&) {
            std::lock_guard<std::mutex> g(lock);
            ++folders;
            if (tick.elapsed() < 150)
                return;
            tick.restart();
            postToUi([self, gen, n = folders] {
                if (self && gen == self->m_gen)
                    self->m_status->setText(plural(tr("Comparing … %n folder(s)", nullptr, n), n));
            });
        };
        auto result = std::make_shared<CompareResult>(compareFolders(left, right, opt, cancel.get(), progress));
        if (cancel->load())
            return;
        postToUi([self, gen, result] {
            if (!self || gen != self->m_gen)
                return;
            self->m_comparing = false;
            if (!result->error.isEmpty()) {
                self->m_model->setItems({}, self->mode());
                self->m_status->setText(tr("Cannot compare: %1").arg(result->error));
                return;
            }
            self->m_model->setItems(std::move(result->items), self->mode());
            self->updateSummary();
            if (self->m_model->rowCount() > 0) {
                self->m_view->setCurrentIndex(self->m_model->index(0, 0));
                self->m_view->setFocus();
            }
        });
    });
}

void SyncDialog::updateFilters()
{
    for (QToolButton* b : m_show)
        b->setVisible(!m_comparing);
    int64_t bytes[SyncModel::GroupCount] = {};
    int count[SyncModel::GroupCount] = {};
    for (size_t i = 0; i < m_model->items().size(); ++i) {
        const int g = m_model->groupOf(i);
        const CompareItem& it = m_model->items()[i];
        count[g] += 1;
        if (g == SyncModel::GroupToRight)
            bytes[g] += qMax<int64_t>(0, it.left.size);
        else if (g == SyncModel::GroupToLeft)
            bytes[g] += qMax<int64_t>(0, it.right.size);
    }
    m_show[SyncModel::GroupToRight]->setText(
        count[0] ? tr("→ %1 to the right · %2").arg(count[0]).arg(formatSize(bytes[0])) : tr("→ none to the right"));
    m_show[SyncModel::GroupToLeft]->setText(
        count[1] ? tr("← %1 to the left · %2").arg(count[1]).arg(formatSize(bytes[1])) : tr("← none to the left"));
    m_show[SyncModel::GroupDiffers]->setText(tr("%1 skipped or to decide").arg(count[2]));
    m_show[SyncModel::GroupEqual]->setText(tr("= %1 identical").arg(count[3]));
    m_show[SyncModel::GroupDelete]->setText(tr("✕ %1 to the trash").arg(count[4]));
    for (int g : {int(SyncModel::GroupDiffers), int(SyncModel::GroupDelete)})
        m_show[g]->setVisible(count[g] > 0 && !m_comparing);
}

void SyncDialog::updateSummary()
{
    updateFilters();
    if (m_comparing)
        return;
    const SyncPlan plan = buildSyncPlan(m_left, m_right, m_model->items(), m_model->actions());
    const int n = int(plan.copies.size() + plan.deletions.size());
    const bool busy = !m_running.isEmpty();
    if (busy)
        m_status->setText(tr("Synchronizing …"));
    else if (n == 0)
        m_status->setText(m_model->items().empty() || m_model->groupCount(SyncModel::GroupEqual) == int(m_model->items().size())
                              ? tr("✓ The folders are in sync")
                              : tr("Nothing chosen to sync"));
    else
        m_status->clear();
    m_syncButton->setEnabled(!busy && n > 0);
    m_syncButton->setText(n > 0 ? plural(tr("Sync %n item(s)", nullptr, n), n) : tr("Sync"));
}

void SyncDialog::setActionOnSelection(SyncAction a)
{
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        m_model->setAction(m_view->currentIndex().row(), a);
    } else {
        for (const QModelIndex& i : rows)
            m_model->setAction(i.row(), a);
    }
    updateSummary();
}

void SyncDialog::cycleAction(int row)
{
    const QList<SyncAction> options = m_model->possibleActions(row);
    const int at = int(options.indexOf(m_model->action(row)));
    m_model->setAction(row, options[(at + 1) % options.size()]);
    updateSummary();
}

void SyncDialog::showContextMenu(const QPoint& pos)
{
    const QModelIndex at = m_view->indexAt(pos);
    if (!at.isValid())
        return;
    QMenu menu(this);
    const QList<SyncAction> possible = m_model->possibleActions(at.row());
    auto add = [&](const QString& text, SyncAction a) {
        QAction* act = menu.addAction(text, this, [this, a] { setActionOnSelection(a); });
        act->setEnabled(possible.contains(a));
    };
    add(tr("Copy to the right  →"), SyncAction::ToRight);
    add(tr("Copy to the left  ←"), SyncAction::ToLeft);
    add(tr("Leave alone"), SyncAction::None);
    add(tr("Delete on the left"), SyncAction::DeleteLeft);
    add(tr("Delete on the right"), SyncAction::DeleteRight);
    menu.addSeparator();
    QAction* diff = menu.addAction(tr("Compare the two files"), this, [this, row = at.row()] { compareFiles(row); });
    const CompareItem& it = m_model->item(at.row());
    diff->setEnabled(it.left.exists && it.right.exists && !it.left.isDir && !it.right.isDir);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void SyncDialog::compareFiles(int row)
{
    if (row < 0 || row >= m_model->rowCount())
        return;
    const CompareItem& it = m_model->item(row);
    if (!(it.left.exists && it.right.exists) || it.left.isDir || it.right.isDir)
        return;
    const QString a = joinPath(m_left, it.rel), b = joinPath(m_right, it.rel);
    QString tool = QSettings().value(QStringLiteral("tools/diff")).toString();
    if (tool.isEmpty()) {
        for (const char* t : {"meld", "kdiff3", "kompare", "diffuse"}) {
            if (!QStandardPaths::findExecutable(QLatin1String(t)).isEmpty()) {
                tool = QLatin1String(t);
                break;
            }
        }
    }
    if (!tool.isEmpty() && QProcess::startDetached(tool, {a, b}))
        return;
    // No diff tool: show unified diff output.
    QProcess p;
    p.start(QStringLiteral("diff"), {QStringLiteral("-u"), a, b});
    p.waitForFinished(5000);
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    if (out.isEmpty())
        out = tr("The files are identical.");
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(it.rel);
    dlg->resize(860, 560);
    auto* l = new QVBoxLayout(dlg);
    auto* text = new QPlainTextEdit(out, dlg);
    text->setReadOnly(true);
    text->setFont(monoFont());
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    l->addWidget(text);
    dlg->show();
}

void SyncDialog::synchronize()
{
    if (m_comparing || !m_running.isEmpty())
        return;
    const SyncPlan plan = buildSyncPlan(m_left, m_right, m_model->items(), m_model->actions());
    if (plan.copies.isEmpty() && plan.deletions.isEmpty())
        return;
    if (!plan.deletions.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, tr("Synchronize"),
            plural(tr("Move %n item(s) to the trash?", nullptr, int(plan.deletions.size())), plan.deletions.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return;
    }
    for (Job* j : m_runner(plan)) {
        m_running << j;
        auto finished = [this, j] {
            m_running.removeIf([j](const QPointer<Job>& p) { return !p || p == j; });
            if (m_running.isEmpty())
                compare();  // show what's left
        };
        connect(j, &Job::finished, this, finished);
        connect(j, &QObject::destroyed, this, finished);
    }
    updateSummary();
}

bool SyncDialog::eventFilter(QObject* obj, QEvent* e)
{
    if (obj != m_view || e->type() != QEvent::KeyPress)
        return QDialog::eventFilter(obj, e);
    auto* ke = static_cast<QKeyEvent*>(e);
    const QString text = ke->text();
    switch (ke->key()) {
    case Qt::Key_Right:
        setActionOnSelection(SyncAction::ToRight);
        return true;
    case Qt::Key_Left:
        setActionOnSelection(SyncAction::ToLeft);
        return true;
    case Qt::Key_Space:
    case Qt::Key_Backspace:
        setActionOnSelection(SyncAction::None);
        return true;
    case Qt::Key_Delete: {
        const int row = m_view->currentIndex().row();
        if (row >= 0) {
            const CompareItem& it = m_model->item(row);
            setActionOnSelection(it.left.exists ? SyncAction::DeleteLeft : SyncAction::DeleteRight);
        }
        return true;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (ke->modifiers() & Qt::ControlModifier) {
            synchronize();
        } else {
            compareFiles(m_view->currentIndex().row());
        }
        return true;
    default:
        break;
    }
    if (text == QLatin1String(">") || text == QLatin1String("l")) {
        setActionOnSelection(SyncAction::ToRight);
        return true;
    }
    if (text == QLatin1String("<") || text == QLatin1String("h")) {
        setActionOnSelection(SyncAction::ToLeft);
        return true;
    }
    if (text == QLatin1String("=")) {
        setActionOnSelection(SyncAction::None);
        return true;
    }
    if (text == QLatin1String("j") || text == QLatin1String("k")) {
        QKeyEvent move(QEvent::KeyPress, text == QLatin1String("j") ? Qt::Key_Down : Qt::Key_Up, ke->modifiers());
        QCoreApplication::sendEvent(m_view, &move);
        return true;
    }
    return QDialog::eventFilter(obj, e);
}

}  // namespace tandem
