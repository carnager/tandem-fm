#include "ui/MultiRenameDialog.h"

#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace tandem {

// ------------------------------------------------------- RenamePreviewModel

void RenamePreviewModel::setPlan(const RenamePlan& plan)
{
    beginResetModel();
    m_plan = plan;
    endResetModel();
}

int RenamePreviewModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_plan.rows.size());
}

int RenamePreviewModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 3;
}

QVariant RenamePreviewModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_plan.rows.size())
        return {};
    const RenameRow& r = m_plan.rows[index.row()];
    const Theme& t = theme();
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0:
            return r.oldName;
        case 1:
            return r.newName;
        case 2:
            return r.note;
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (r.status) {
        case RenameRow::Status::Unchanged:
            return t.dimText;
        case RenameRow::Status::Ok:
            return index.column() == 1 ? t.accent : t.text;
        case RenameRow::Status::Conflict:
        case RenameRow::Status::Invalid:
            return t.error;
        }
    }
    if (role == Qt::ToolTipRole && !r.note.isEmpty())
        return r.note;
    return {};
}

QVariant RenamePreviewModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0:
        return tr("Current name");
    case 1:
        return tr("New name");
    case 2:
        return tr("Problem");
    }
    return {};
}

// -------------------------------------------------------- MultiRenameDialog

MultiRenameDialog::MultiRenameDialog(QList<RenameItem> items, QString parentFolderName, QSet<QString> existing,
                                     QWidget* parent)
    : QDialog(parent), m_items(std::move(items)), m_parentName(std::move(parentFolderName)),
      m_existing(std::move(existing))
{
    setWindowTitle(tr("Multi-rename %n item(s)", nullptr, int(m_items.size())));
    resize(900, 620);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(10);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    auto label = [this](const QString& text) {
        auto* l = new QLabel(text, this);
        l->setStyleSheet(theme().css(QStringLiteral("color: @dimText;")));
        return l;
    };

    m_nameMask = new QLineEdit(this);
    m_extMask = new QLineEdit(this);
    m_nameMask->setToolTip(tr("[N] name, [N2-5] part, [C] counter, [P] folder, [YMD] date …"));
    grid->addWidget(label(tr("Name")), 0, 0);
    grid->addWidget(m_nameMask, 0, 1);
    grid->addWidget(insertButton(m_nameMask, false), 0, 2);
    grid->addWidget(label(tr("Extension")), 0, 3);
    grid->addWidget(m_extMask, 0, 4);
    grid->addWidget(insertButton(m_extMask, true), 0, 5);

    m_search = new QLineEdit(this);
    m_replace = new QLineEdit(this);
    m_regex = new QCheckBox(tr("Regex ($1 = group)"), this);
    m_caseSensitive = new QCheckBox(tr("Match case"), this);
    grid->addWidget(label(tr("Search")), 1, 0);
    grid->addWidget(m_search, 1, 1, 1, 2);
    grid->addWidget(label(tr("Replace")), 1, 3);
    grid->addWidget(m_replace, 1, 4, 1, 2);
    auto* flags = new QHBoxLayout;
    flags->addWidget(m_regex);
    flags->addWidget(m_caseSensitive);
    flags->addStretch();

    m_case = new QComboBox(this);
    m_case->addItems({tr("Unchanged"), tr("lowercase"), tr("UPPERCASE"), tr("First letter upper"), tr("Title Case")});
    m_start = new QSpinBox(this);
    m_start->setRange(0, 9999999);
    m_step = new QSpinBox(this);
    m_step->setRange(-1000, 1000);
    m_digits = new QSpinBox(this);
    m_digits->setRange(1, 12);
    auto* counter = new QHBoxLayout;
    counter->addWidget(label(tr("Case")));
    counter->addWidget(m_case);
    counter->addSpacing(16);
    counter->addWidget(label(tr("Counter [C]: start")));
    counter->addWidget(m_start);
    counter->addWidget(label(tr("step")));
    counter->addWidget(m_step);
    counter->addWidget(label(tr("digits")));
    counter->addWidget(m_digits);
    counter->addStretch();
    counter->addLayout(flags);

    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(4, 1);
    layout->addLayout(grid);
    layout->addLayout(counter);

    m_model = new RenamePreviewModel(this);
    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->verticalHeader()->hide();
    m_table->setShowGrid(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setWordWrap(false);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 8);
    layout->addWidget(m_table, 1);

    auto* bottom = new QHBoxLayout;
    m_summary = new QLabel(this);
    m_summary->setTextFormat(Qt::PlainText);
    bottom->addWidget(m_summary, 1);
    auto* reset = new QPushButton(tr("Reset"), this);
    reset->setAutoDefault(false);
    auto* cancel = new QPushButton(tr("Cancel"), this);
    cancel->setAutoDefault(false);
    m_rename = new QPushButton(tr("Rename"), this);
    m_rename->setDefault(true);
    bottom->addWidget(reset);
    bottom->addWidget(cancel);
    bottom->addWidget(m_rename);
    layout->addLayout(bottom);
    connect(reset, &QPushButton::clicked, this, &MultiRenameDialog::resetFields);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_rename, &QPushButton::clicked, this, &QDialog::accept);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(40);
    connect(&m_debounce, &QTimer::timeout, this, &MultiRenameDialog::recompute);
    auto schedule = [this] { m_debounce.start(); };
    for (QLineEdit* e : {m_nameMask, m_extMask, m_search, m_replace})
        connect(e, &QLineEdit::textChanged, this, schedule);
    for (QCheckBox* c : {m_regex, m_caseSensitive})
        connect(c, &QCheckBox::toggled, this, schedule);
    connect(m_case, &QComboBox::currentIndexChanged, this, schedule);
    for (QSpinBox* s : {m_start, m_step, m_digits})
        connect(s, &QSpinBox::valueChanged, this, schedule);

    loadSettings();
    recompute();
    m_nameMask->setFocus();
    m_nameMask->selectAll();
}

QWidget* MultiRenameDialog::insertButton(QLineEdit* target, bool forExtension)
{
    auto* b = new QToolButton(this);
    b->setText(QStringLiteral("[ ]"));
    b->setToolTip(tr("Insert a placeholder"));
    b->setFocusPolicy(Qt::NoFocus);
    b->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(b);
    const QList<QPair<QString, QString>> entries = {
        {QStringLiteral("[N]"), tr("Name")},
        {QStringLiteral("[N1-8]"), tr("Characters 1 to 8 of the name")},
        {QStringLiteral("[N2,3]"), tr("3 characters from the 2nd")},
        {QStringLiteral("[N-3-]"), tr("Last 3 characters")},
        {QStringLiteral("[E]"), tr("Extension")},
        {QStringLiteral("[C]"), tr("Counter")},
        {QStringLiteral("[P]"), tr("Parent folder name")},
        {QStringLiteral("[YMD]"), tr("Date modified (yyyymmdd)")},
        {QStringLiteral("[hms]"), tr("Time modified (hhmmss)")},
        {QStringLiteral("[Y]-[M]-[D]"), tr("Date with dashes")},
    };
    for (const auto& [code, text] : entries) {
        if (forExtension && code.startsWith(QLatin1String("[Y]")))
            continue;
        menu->addAction(QStringLiteral("%1   %2").arg(code, text), this, [target, code] {
            target->insert(code);
            target->setFocus();
        });
    }
    b->setMenu(menu);
    return b;
}

RenameOptions MultiRenameDialog::options() const
{
    RenameOptions o;
    o.nameMask = m_nameMask->text();
    o.extMask = m_extMask->text();
    o.search = m_search->text();
    o.replace = m_replace->text();
    o.regex = m_regex->isChecked();
    o.caseSensitive = m_caseSensitive->isChecked();
    o.caseMode = RenameOptions::Case(m_case->currentIndex());
    o.counterStart = m_start->value();
    o.counterStep = m_step->value();
    o.counterDigits = m_digits->value();
    return o;
}

void MultiRenameDialog::recompute()
{
    m_plan = buildRenamePlan(m_items, m_parentName, options(), m_existing);
    m_model->setPlan(m_plan);
    m_table->setColumnHidden(2, m_plan.problems == 0);
    const Theme& t = theme();
    QString text;
    QColor color = t.dimText;
    if (!m_plan.error.isEmpty()) {
        text = m_plan.error;
        color = t.error;
    } else if (m_plan.problems) {
        text = tr("%n name(s) can't be used — fix the mask or search/replace", nullptr, m_plan.problems);
        color = t.error;
    } else if (m_plan.changes == 0) {
        text = tr("Nothing changes yet");
    } else {
        text = tr("%n item(s) will be renamed", nullptr, m_plan.changes);
        color = t.text;
    }
    m_summary->setText(text);
    QPalette pal = m_summary->palette();
    pal.setColor(QPalette::WindowText, color);
    m_summary->setPalette(pal);
    m_rename->setEnabled(m_plan.canRun());
}

void MultiRenameDialog::accept()
{
    m_debounce.stop();
    recompute();  // never act on a stale preview
    if (!m_plan.canRun())
        return;
    saveSettings();
    QDialog::accept();
}

void MultiRenameDialog::resetFields()
{
    const RenameOptions d;
    m_nameMask->setText(d.nameMask);
    m_extMask->setText(d.extMask);
    m_search->clear();
    m_replace->clear();
    m_regex->setChecked(false);
    m_caseSensitive->setChecked(false);
    m_case->setCurrentIndex(0);
    m_start->setValue(d.counterStart);
    m_step->setValue(d.counterStep);
    m_digits->setValue(d.counterDigits);
}

void MultiRenameDialog::loadSettings()
{
    QSettings s;
    s.beginGroup(QStringLiteral("multiRename"));
    const RenameOptions d;
    m_nameMask->setText(s.value(QStringLiteral("nameMask"), d.nameMask).toString());
    m_extMask->setText(s.value(QStringLiteral("extMask"), d.extMask).toString());
    m_search->setText(s.value(QStringLiteral("search")).toString());
    m_replace->setText(s.value(QStringLiteral("replace")).toString());
    m_regex->setChecked(s.value(QStringLiteral("regex"), false).toBool());
    m_caseSensitive->setChecked(s.value(QStringLiteral("caseSensitive"), false).toBool());
    m_case->setCurrentIndex(s.value(QStringLiteral("case"), 0).toInt());
    m_start->setValue(s.value(QStringLiteral("counterStart"), d.counterStart).toInt());
    m_step->setValue(s.value(QStringLiteral("counterStep"), d.counterStep).toInt());
    m_digits->setValue(s.value(QStringLiteral("counterDigits"), d.counterDigits).toInt());
}

void MultiRenameDialog::saveSettings() const
{
    QSettings s;
    s.beginGroup(QStringLiteral("multiRename"));
    const RenameOptions o = options();
    s.setValue(QStringLiteral("nameMask"), o.nameMask);
    s.setValue(QStringLiteral("extMask"), o.extMask);
    s.setValue(QStringLiteral("search"), o.search);
    s.setValue(QStringLiteral("replace"), o.replace);
    s.setValue(QStringLiteral("regex"), o.regex);
    s.setValue(QStringLiteral("caseSensitive"), o.caseSensitive);
    s.setValue(QStringLiteral("case"), int(o.caseMode));
    s.setValue(QStringLiteral("counterStart"), o.counterStart);
    s.setValue(QStringLiteral("counterStep"), o.counterStep);
    s.setValue(QStringLiteral("counterDigits"), o.counterDigits);
}

}  // namespace tandem
