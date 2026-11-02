#include "ui/SyncDialog.h"

#include "core/Job.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
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
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::TextAlignmentRole)
        return section == Name ? int(Qt::AlignLeft | Qt::AlignVCenter)
                               : section == Action ? int(Qt::AlignCenter)
                                                   : section == LeftSize || section == RightSize
                                                         ? int(Qt::AlignRight | Qt::AlignVCenter)
                                                         : int(Qt::AlignLeft | Qt::AlignVCenter);
    if (role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return tr("Name");
    case LeftSize: return tr("Left size");
    case LeftTime: return tr("Modified");
    case Action: return QString();
    case RightTime: return tr("Modified");
    case RightSize: return tr("Right size");
    }
    return {};
}

static QString sizeText(const CompareSide& s)
{
    if (!s.exists)
        return {};
    if (s.isDir)
        return s.size >= 0 ? formatSize(s.size) : QObject::tr("<DIR>");
    return formatSize(s.size);
}

static QString timeText(const CompareSide& s)
{
    if (!s.exists)
        return {};
    return QDateTime::fromSecsSinceEpoch(s.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QVariant SyncModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};
    const CompareItem& it = item(index.row());
    const SyncAction a = action(index.row());
    const Theme& t = theme();
    const int col = index.column();

    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Name: return it.rel;
        case LeftSize: return sizeText(it.left);
        case LeftTime: return timeText(it.left);
        case RightTime: return timeText(it.right);
        case RightSize: return sizeText(it.right);
        case Action:
            switch (a) {
            case SyncAction::ToRight: return QStringLiteral("→");
            case SyncAction::ToLeft: return QStringLiteral("←");
            case SyncAction::DeleteLeft: return QStringLiteral("✕  ");
            case SyncAction::DeleteRight: return QStringLiteral("  ✕");
            case SyncAction::None:
                return it.diff == Diff::Equal ? QStringLiteral("=")
                       : it.diff == Diff::TypeMismatch ? QStringLiteral("!") : QStringLiteral("≠");
            }
        }
        return {};
    case Qt::DecorationRole:
        if (col == Name) {
            FileEntry e;
            e.name = it.rel.section(QLatin1Char('/'), -1);
            e.isDir = (it.left.exists ? it.left.isDir : it.right.isDir);
            e.mode = e.isDir ? S_IFDIR : S_IFREG;
            return IconCache::forEntry(e);
        }
        return {};
    case Qt::TextAlignmentRole:
        if (col == Action)
            return int(Qt::AlignCenter);
        if (col == LeftSize || col == RightSize)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::FontRole:
        if (col == Action) {
            QFont f = t.listFont;
            f.setBold(true);
            return f;
        }
        if (col != Name)
            return t.numberFont;
        return {};
    case Qt::ForegroundRole:
        if (col == Action) {
            switch (a) {
            case SyncAction::ToRight:
            case SyncAction::ToLeft: return t.accent;
            case SyncAction::DeleteLeft:
            case SyncAction::DeleteRight: return t.error;
            case SyncAction::None: return it.diff == Diff::Equal ? t.dimText : t.gitModified;
            }
        }
        // The side that will be replaced or removed is dimmed.
        if (col == LeftSize || col == LeftTime)
            return a == SyncAction::ToLeft || a == SyncAction::DeleteLeft ? t.dimText : QColor();
        if (col == RightSize || col == RightTime)
            return a == SyncAction::ToRight || a == SyncAction::DeleteRight ? t.dimText : QColor();
        if (col == Name && a == SyncAction::None && it.diff == Diff::Equal)
            return t.dimText;
        return {};
    case Qt::ToolTipRole:
        switch (it.diff) {
        case Diff::Equal: return tr("Same on both sides");
        case Diff::LeftNewer: return tr("Newer on the left");
        case Diff::RightNewer: return tr("Newer on the right");
        case Diff::LeftOnly: return tr("Only on the left");
        case Diff::RightOnly: return tr("Only on the right");
        case Diff::Differs: return tr("Same time, different contents: decide yourself");
        case Diff::TypeMismatch: return tr("A file on one side, a folder on the other: not synchronized");
        }
        return {};
    default:
        return {};
    }
}

// ----------------------------------------------------------------- dialog

SyncDialog::SyncDialog(const QString& left, const QString& right, Runner runner, QWidget* parent)
    : QDialog(parent), m_left(left), m_right(right), m_runner(std::move(runner))
{
    const Theme& t = theme();
    setWindowTitle(tr("Synchronize folders"));
    resize(1100, 680);
    setStyleSheet(t.css(QStringLiteral(
        "QToolButton#show { color: @dimText; border: 1px solid @border; border-radius: 10px; padding: 2px 10px; }"
        "QToolButton#show:checked { color: @text; background: @border; }"
        "QLabel#paths { font-weight: 600; }"
        "QLabel#summary { color: @dimText; }")));

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    m_paths = new QLabel(this);
    m_paths->setObjectName(QStringLiteral("paths"));
    m_paths->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_paths->setToolTip(QStringLiteral("%1\n%2").arg(left, right));
    layout->addWidget(m_paths);

    QSettings s;
    auto* options = new QHBoxLayout;
    m_recursive = new QCheckBox(tr("Subfolders"), this);
    m_recursive->setChecked(s.value(QStringLiteral("sync/recursive"), true).toBool());
    m_contents = new QCheckBox(tr("Compare contents"), this);
    m_contents->setToolTip(tr("Read files with equal sizes byte by byte instead of trusting their times (slower)"));
    m_contents->setChecked(s.value(QStringLiteral("sync/contents"), false).toBool());
    m_hidden = new QCheckBox(tr("Hidden files"), this);
    m_hidden->setChecked(s.value(QStringLiteral("sync/hidden"), true).toBool());
    m_mode = new QComboBox(this);
    m_mode->addItem(tr("Both ways (newer wins)"));
    m_mode->addItem(tr("Mirror left → right"));
    m_mode->addItem(tr("Mirror right → left"));
    m_mode->setToolTip(tr("Mirroring makes the target like the source: it also overwrites newer files and deletes extra ones (to the trash)"));
    m_mode->setCurrentIndex(qBound(0, s.value(QStringLiteral("sync/mode"), 0).toInt(), 2));
    m_compareButton = new QPushButton(tr("Compare again"), this);
    m_compareButton->setAutoDefault(false);
    options->addWidget(m_recursive);
    options->addWidget(m_contents);
    options->addWidget(m_hidden);
    options->addSpacing(12);
    options->addWidget(m_mode);
    options->addStretch(1);
    options->addWidget(m_compareButton);
    layout->addLayout(options);

    auto* filters = new QHBoxLayout;
    filters->setSpacing(6);
    filters->addWidget(new QLabel(tr("Show:"), this));
    m_model = new SyncModel(this);
    for (int g = 0; g < SyncModel::GroupCount; ++g) {
        m_show[g] = new QToolButton(this);
        m_show[g]->setObjectName(QStringLiteral("show"));
        m_show[g]->setCheckable(true);
        m_show[g]->setChecked(m_model->isShown(g));
        m_show[g]->setFocusPolicy(Qt::NoFocus);
        filters->addWidget(m_show[g]);
        connect(m_show[g], &QToolButton::toggled, this, [this, g](bool on) {
            m_model->setShown(g, on);
            updateSummary();
        });
    }
    m_show[SyncModel::GroupToRight]->setToolTip(tr("Copied to the right"));
    m_show[SyncModel::GroupToLeft]->setToolTip(tr("Copied to the left"));
    m_show[SyncModel::GroupDiffers]->setToolTip(tr("Different, but left alone"));
    m_show[SyncModel::GroupEqual]->setToolTip(tr("Identical"));
    m_show[SyncModel::GroupDelete]->setToolTip(tr("Deleted (to the trash)"));
    filters->addStretch(1);
    auto* keys = new QLabel(tr("← → direction · Space: leave alone · Del: delete · Enter: diff · Ctrl+Enter: synchronize"), this);
    keys->setObjectName(QStringLiteral("summary"));
    filters->addWidget(keys);
    layout->addLayout(filters);

    m_view = new QTreeView(this);
    m_view->setModel(m_model);
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
    h->setSectionResizeMode(SyncModel::Name, QHeaderView::Stretch);
    h->setSectionResizeMode(SyncModel::Action, QHeaderView::Fixed);
    h->resizeSection(SyncModel::Action, 56);
    for (int c : {SyncModel::LeftSize, SyncModel::RightSize})
        h->resizeSection(c, 100);
    for (int c : {SyncModel::LeftTime, SyncModel::RightTime})
        h->resizeSection(c, 150);
    layout->addWidget(m_view, 1);

    auto* bottom = new QHBoxLayout;
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("summary"));
    m_syncButton = new QPushButton(tr("Synchronize"), this);
    m_syncButton->setDefault(true);
    auto* close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    bottom->addWidget(m_summary, 1);
    bottom->addWidget(m_syncButton);
    bottom->addWidget(close);
    layout->addLayout(bottom);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_syncButton, &QPushButton::clicked, this, &SyncDialog::synchronize);
    connect(m_compareButton, &QPushButton::clicked, this, &SyncDialog::compare);
    for (QCheckBox* c : {m_recursive, m_contents, m_hidden})
        connect(c, &QCheckBox::toggled, this, [this] {
            saveOptions();
            compare();
        });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] {
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
    return SyncMode(m_mode->currentIndex());
}

void SyncDialog::saveOptions()
{
    QSettings s;
    s.setValue(QStringLiteral("sync/recursive"), m_recursive->isChecked());
    s.setValue(QStringLiteral("sync/contents"), m_contents->isChecked());
    s.setValue(QStringLiteral("sync/hidden"), m_hidden->isChecked());
    s.setValue(QStringLiteral("sync/mode"), m_mode->currentIndex());
}

void SyncDialog::compare()
{
    if (m_cancel)
        m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_comparing = true;
    m_syncButton->setEnabled(false);
    m_summary->setText(tr("Comparing…"));
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
        auto progress = [&](const QString& rel) {
            if (tick.elapsed() < 120)
                return;
            tick.restart();
            postToUi([self, gen, rel] {
                if (self && gen == self->m_gen)
                    self->m_summary->setText(tr("Comparing %1 …").arg(rel));
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
                self->m_summary->setText(tr("Cannot compare: %1").arg(result->error));
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
    static const char* const symbols[] = {"→", "←", "≠", "=", "✕"};
    for (int g = 0; g < SyncModel::GroupCount; ++g)
        m_show[g]->setText(QStringLiteral("%1  %2").arg(QString::fromUtf8(symbols[g])).arg(m_model->groupCount(g)));
}

void SyncDialog::updateSummary()
{
    updateFilters();
    if (m_comparing)
        return;
    const SyncPlan plan = buildSyncPlan(m_left, m_right, m_model->items(), m_model->actions());
    int toRight = 0, toLeft = 0;
    int64_t rightBytes = 0, leftBytes = 0;
    for (size_t i = 0; i < m_model->items().size(); ++i) {
        const CompareItem& it = m_model->items()[i];
        if (m_model->actions()[i] == SyncAction::ToRight) {
            ++toRight;
            rightBytes += qMax<int64_t>(0, it.left.size);
        } else if (m_model->actions()[i] == SyncAction::ToLeft) {
            ++toLeft;
            leftBytes += qMax<int64_t>(0, it.right.size);
        }
    }
    QStringList parts;
    if (toRight)
        parts << tr("%1 → right (%2)").arg(toRight).arg(formatSize(rightBytes));
    if (toLeft)
        parts << tr("%1 ← left (%2)").arg(toLeft).arg(formatSize(leftBytes));
    if (!plan.deletions.isEmpty())
        parts << tr("%1 to the trash").arg(plan.deletions.size());
    if (parts.isEmpty())
        parts << (m_model->items().empty() || m_model->groupCount(SyncModel::GroupEqual) == int(m_model->items().size())
                      ? tr("The folders are in sync")
                      : tr("Nothing selected to synchronize"));
    const bool busy = !m_running.isEmpty();
    m_summary->setText(busy ? tr("Synchronizing…") : parts.join(QStringLiteral("   ·   ")));
    m_syncButton->setEnabled(!busy && (!plan.copies.isEmpty() || !plan.deletions.isEmpty()));
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
