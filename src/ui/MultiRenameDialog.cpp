#include "ui/MultiRenameDialog.h"

#include "core/ExifDate.h"
#include "core/Util.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QTableView>
#include <QThreadPool>
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
    return parent.isValid() ? 0 : 2;
}

QVariant RenamePreviewModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_plan.rows.size())
        return {};
    const RenameRow& r = m_plan.rows[index.row()];
    if (role == Qt::DisplayRole)
        return index.column() == 0 ? r.oldName : r.newName;
    if (role == Qt::ToolTipRole)
        return r.note.isEmpty() ? (index.column() == 0 ? r.oldName : r.newName) : r.note;
    return {};
}

QVariant RenamePreviewModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return section == 0 ? tr("Now") : tr("After");
}

// ------------------------------------------------------- RenameDiffDelegate

void RenameDiffDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);

    const auto* model = qobject_cast<const RenamePreviewModel*>(index.model());
    if (!model || index.row() >= model->plan().rows.size())
        return;
    const RenameRow& r = model->plan().rows[index.row()];
    const Theme& t = theme();
    const QRect box = opt.rect.adjusted(8, 0, -8, 0);
    const QFontMetrics fm(opt.font);
    const int y = box.top() + (box.height() + fm.ascent() - fm.descent()) / 2;
    p->save();
    p->setClipRect(box);
    p->setFont(opt.font);
    const bool unchanged = r.status == RenameRow::Status::Unchanged;
    const bool bad = r.status == RenameRow::Status::Conflict || r.status == RenameRow::Status::Invalid;

    if (!(opt.state & QStyle::State_Enabled)) {  // a stale preview while the template is unfinished
        p->setPen(t.dimText);
        p->drawText(box.left(), y, index.column() == 0 ? r.oldName : r.newName);
        p->restore();
        return;
    }
    if (index.column() == 0 || unchanged) {
        p->setPen(unchanged ? t.dimText : t.text);
        p->drawText(box.left(), y, index.column() == 0 ? r.oldName : r.newName);
        p->restore();
        return;
    }
    // What changed: everything between the common start and the common end.
    const QString& a = r.oldName;
    const QString& b = r.newName;
    int pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre])
        ++pre;
    int suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf])
        ++suf;
    const QString head = b.left(pre), mid = b.mid(pre, b.size() - pre - suf), tail = b.right(suf);
    int x = box.left();
    p->setPen(bad ? t.error : t.text);
    p->drawText(x, y, head);
    x += fm.horizontalAdvance(head);
    if (!mid.isEmpty()) {
        const int w = fm.horizontalAdvance(mid);
        QColor bg = bad ? t.error : t.accent;
        bg.setAlpha(t.dark ? 70 : 45);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(QRectF(x - 1, y - fm.ascent() - 1, w + 2, fm.height() + 2), 3, 3);
        p->setPen(bad ? t.error : (t.dark ? t.accent.lighter(130) : t.accent.darker(120)));
        p->drawText(x, y, mid);
        x += w;
    }
    p->setPen(bad ? t.error : t.text);
    p->drawText(x, y, tail);
    x += fm.horizontalAdvance(tail);
    if (bad && !r.note.isEmpty()) {
        p->setPen(t.error);
        QFont f = opt.font;
        f.setItalic(true);
        p->setFont(f);
        p->drawText(x + 12, y, QStringLiteral("— %1").arg(r.note));
    }
    p->restore();
}

QTableView* makeRenamePreview(RenamePreviewModel* model, QWidget* parent)
{
    auto* table = new QTableView(parent);
    table->setModel(model);
    table->setItemDelegate(new RenameDiffDelegate(table));
    table->verticalHeader()->hide();
    table->setShowGrid(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setWordWrap(false);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 10);
    return table;
}

// ------------------------------------------------------ RenameConfirmDialog

RenameConfirmDialog::RenameConfirmDialog(const RenamePlan& plan, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Rename from editor"));
    resize(820, 560);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    auto* model = new RenamePreviewModel(this);
    model->setPlan(plan);
    layout->addWidget(makeRenamePreview(model, this), 1);

    const Theme& t = theme();
    auto* bottom = new QHBoxLayout;
    auto* summary = new QLabel(this);
    if (plan.problems)
        summary->setText(plural(tr("%n name(s) can't be used. Edit again to fix them.", nullptr, plan.problems), plan.problems));
    else
        summary->setText(plural(tr("%n item(s) will be renamed", nullptr, plan.changes), plan.changes));
    QPalette pal = summary->palette();
    pal.setColor(QPalette::WindowText, plan.problems ? t.error : t.text);
    summary->setPalette(pal);
    bottom->addWidget(summary, 1);
    auto* again = new QPushButton(tr("Edit again"), this);
    auto* cancel = new QPushButton(tr("Cancel"), this);
    m_rename = new QPushButton(plural(tr("Rename %n", nullptr, plan.changes), plan.changes), this);
    again->setAutoDefault(false);
    cancel->setAutoDefault(false);
    m_rename->setDefault(true);
    m_rename->setEnabled(plan.canRun());
    if (!plan.canRun())
        again->setDefault(true);
    connect(again, &QPushButton::clicked, this, [this] { done(EditAgain); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_rename, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(again);
    bottom->addWidget(cancel);
    bottom->addWidget(m_rename);
    layout->addLayout(bottom);
}

// -------------------------------------------------------- MultiRenameDialog

namespace {

struct Suggestion {
    QString body;  // the whole {…} content it completes to
    QString label;
    QString hint;
};

// What fits where the cursor is, given the text since the last "{".
QList<Suggestion> suggestionsFor(const QString& body)
{
    QList<Suggestion> out;
    static const QRegularExpression fieldRx(QStringLiteral("^([a-z]*)$"));
    static const QRegularExpression formatRx(QStringLiteral("^((?:date|taken)(?:\\[[^\\]]*\\])?):([^|]*)$"));
    static const QRegularExpression counterRx(QStringLiteral("^n:([\\d+]*)$"));
    static const QRegularExpression filterRx(QStringLiteral("^(.*\\|)([a-z]*)$"));
    if (const auto m = fieldRx.match(body); m.hasMatch()) {
        const QList<Suggestion> fields = {
            {QStringLiteral("name"), QStringLiteral("name"), QObject::tr("name without extension")},
            {QStringLiteral("ext"), QStringLiteral("ext"), QObject::tr("extension, with its dot")},
            {QStringLiteral("n"), QStringLiteral("n"), QObject::tr("counter — {n:001} pads, {n:10+5} starts at 10, steps 5")},
            {QStringLiteral("taken"), QStringLiteral("taken"), QObject::tr("date the photo was taken (else modified)")},
            {QStringLiteral("date"), QStringLiteral("date"), QObject::tr("date modified — {date:yyyyMMdd} for other formats")},
            {QStringLiteral("folder"), QStringLiteral("folder"), QObject::tr("name of the folder")},
            {QStringLiteral("file"), QStringLiteral("file"), QObject::tr("the whole current name")},
        };
        for (const Suggestion& s : fields)
            if (s.body.startsWith(m.captured(1)) && s.body != m.captured(1))
                out << s;
    } else if (const auto f = formatRx.match(body); f.hasMatch()) {
        for (const char* fmt : {"yyyy-MM-dd", "yyyyMMdd", "yyyy-MM-dd HH.mm", "yyyy-MM-dd_HHmmss", "yyyy-MM", "yyyy", "dd.MM.yyyy"})
            if (QLatin1String(fmt).startsWith(f.captured(2)))
                out << Suggestion{f.captured(1) + QLatin1Char(':') + QLatin1String(fmt), QLatin1String(fmt),
                                  QDateTime::currentDateTime().toString(QLatin1String(fmt))};
    } else if (const auto c = counterRx.match(body); c.hasMatch()) {
        const QList<QPair<QString, QString>> counters = {{QStringLiteral("01"), QObject::tr("01, 02, 03 …")},
                                                         {QStringLiteral("001"), QObject::tr("001, 002, 003 …")},
                                                         {QStringLiteral("0001"), QObject::tr("0001, 0002 …")},
                                                         {QStringLiteral("10+10"), QObject::tr("10, 20, 30 …")}};
        for (const auto& [v, hint] : counters)
            if (v.startsWith(c.captured(1)) && v != c.captured(1))
                out << Suggestion{QStringLiteral("n:") + v, v, hint};
    } else if (const auto fl = filterRx.match(body); fl.hasMatch()) {
        const QList<QPair<QString, QString>> filters = {{QStringLiteral("lower"), QObject::tr("lowercase")},
                                                        {QStringLiteral("upper"), QObject::tr("UPPERCASE")},
                                                        {QStringLiteral("title"), QObject::tr("Title Case")},
                                                        {QStringLiteral("trim"), QObject::tr("no spaces at the ends")}};
        for (const auto& [v, hint] : filters)
            if (v.startsWith(fl.captured(2)) && v != fl.captured(2))
                out << Suggestion{fl.captured(1) + v, v, hint};
    }
    return out;
}

// Where the {…} being typed starts (index after "{"), or -1.
int openBrace(const QString& text, int cursor)
{
    const int open = int(text.lastIndexOf(QLatin1Char('{'), cursor - 1));
    if (open < 0 || (open > 0 && text[open - 1] == QLatin1Char('{')))
        return -1;
    const int close = int(text.indexOf(QLatin1Char('}'), open));
    return close >= 0 && close < cursor ? -1 : open + 1;
}

}  // namespace

MultiRenameDialog::MultiRenameDialog(QList<RenameItem> items, QString folder, QSet<QString> existing, QWidget* parent)
    : QDialog(parent), m_items(std::move(items)), m_folder(std::move(folder)), m_existing(std::move(existing))
{
    setWindowTitle(plural(tr("Rename %n item(s)", nullptr, int(m_items.size())), int(m_items.size())));
    resize(940, 640);
    const Theme& t = theme();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    auto dim = [this, &t](const QString& text) {
        auto* l = new QLabel(text, this);
        setThemeStyle(l, QStringLiteral("color: @dimText;"));
        return l;
    };

    // The template: the one thing most renames need.
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    m_template = new QLineEdit(this);
    m_template->setFont(monoFont());
    m_template->setPlaceholderText(QStringLiteral("{name}{ext}"));
    m_template->setClearButtonEnabled(true);
    m_template->setMinimumHeight(m_template->sizeHint().height() + 6);
    m_recent = new QToolButton(this);
    m_recent->setText(tr("recent"));
    m_recent->setToolTip(tr("Recent templates"));
    m_recent->setPopupMode(QToolButton::InstantPopup);
    m_recent->setFocusPolicy(Qt::NoFocus);
    auto* recentMenu = new QMenu(m_recent);
    m_recent->setMenu(recentMenu);
    connect(recentMenu, &QMenu::aboutToShow, this, [this, recentMenu] {
        recentMenu->clear();
        const QStringList recent = QSettings().value(QStringLiteral("multiRename/recent")).toStringList();
        for (const QString& r : recent)
            recentMenu->addAction(r, this, [this, r] { m_template->setText(r); });
        if (recent.isEmpty())
            recentMenu->addAction(tr("Templates you use show up here"))->setEnabled(false);
    });
    auto* templateRow = new QHBoxLayout;
    templateRow->setSpacing(4);
    templateRow->addWidget(m_template, 1);
    templateRow->addWidget(m_recent);
    grid->addWidget(dim(tr("New name")), 0, 0);
    grid->addLayout(templateRow, 0, 1);

    // Clickable fields: what the template can use, without reading docs.
    auto* chips = new QHBoxLayout;
    chips->setSpacing(6);
    chips->addWidget(chip(tr("name"), QStringLiteral("{name}"), tr("Name without extension")));
    chips->addWidget(chip(tr("extension"), QStringLiteral("{ext}"), tr("Extension with its dot, e.g. .jpg")));
    chips->addWidget(chip(tr("number"), QStringLiteral("{n:01}"), tr("Counter: {n:001} has 3 digits, {n:10+5} counts 10, 15, 20 …")));
    chips->addWidget(chip(tr("date taken"), QStringLiteral("{taken}"), tr("When a photo was taken (from the camera), else the modified date")));
    chips->addWidget(chip(tr("date modified"), QStringLiteral("{date}"), tr("Modified date; {date:yyyyMMdd_HHmm} for another format")));
    chips->addWidget(chip(tr("folder"), QStringLiteral("{folder}"), tr("Name of the folder the items are in")));
    auto* help = dim(tr("type { for more · {name[0:4]} first 4 letters · {name|lower}"));
    chips->addWidget(help);
    chips->addStretch();
    grid->addLayout(chips, 1, 1);

    // Find and replace, with the usual two toggles.
    m_find = new QLineEdit(this);
    m_find->setPlaceholderText(tr("find"));
    m_replace = new QLineEdit(this);
    m_replace->setPlaceholderText(tr("replace with"));
    auto toggle = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFont(monoFont());
        return b;
    };
    m_regex = toggle(QStringLiteral(".*"), tr("Regular expression ($1 is the first group)"));
    m_matchCase = toggle(QStringLiteral("Aa"), tr("Match case"));
    auto* findRow = new QHBoxLayout;
    findRow->setSpacing(6);
    findRow->addWidget(m_find, 1);
    findRow->addWidget(m_regex);
    findRow->addWidget(m_matchCase);
    findRow->addSpacing(8);
    findRow->addWidget(dim(QStringLiteral("→")));
    findRow->addSpacing(8);
    findRow->addWidget(m_replace, 1);
    grid->addWidget(dim(tr("Replace")), 2, 0);
    grid->addLayout(findRow, 2, 1);

    // Case style: one row of buttons, the current one lit.
    m_case = new QButtonGroup(this);
    m_case->setExclusive(true);
    auto* caseRow = new QHBoxLayout;
    caseRow->setSpacing(0);
    const QList<QPair<RenameOptions::Case, QString>> cases = {
        {RenameOptions::Case::Keep, tr("as is")},     {RenameOptions::Case::Lower, tr("lowercase")},
        {RenameOptions::Case::Upper, tr("UPPERCASE")}, {RenameOptions::Case::Title, tr("Title Case")},
        {RenameOptions::Case::Sentence, tr("Sentence case")}};
    for (const auto& [mode, label] : cases) {
        auto* b = new QToolButton(this);
        b->setText(label);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setObjectName(QStringLiteral("segment"));
        m_case->addButton(b, int(mode));
        caseRow->addWidget(b);
    }
    m_case->button(0)->setChecked(true);
    caseRow->addStretch();
    grid->addWidget(dim(tr("Case")), 3, 0);
    grid->addLayout(caseRow, 3, 1);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
    setThemeStyle(this, QStringLiteral(
        "QToolButton#segment { padding: 4px 12px; border: 1px solid @border; background: @base; }"
        "QToolButton#segment:checked { background: @accent; color: @accentText; border-color: @accent; }"
        "QToolButton#chip { padding: 2px 10px; border: 1px solid @border; border-radius: 10px; background: @altBase; }"
        "QToolButton#chip:hover { border-color: @accent; }"));

    m_model = new RenamePreviewModel(this);
    m_table = makeRenamePreview(m_model, this);
    layout->addWidget(m_table, 1);

    auto* bottom = new QHBoxLayout;
    m_summary = new QLabel(this);
    m_summary->setTextFormat(Qt::PlainText);
    bottom->addWidget(m_summary, 1);
    auto* inEditor = new QPushButton(tr("Edit names in editor…"), this);
    inEditor->setAutoDefault(false);
    inEditor->setToolTip(tr("Rename with your text editor, one name per line (Ctrl+E)"));
    connect(inEditor, &QPushButton::clicked, this, [this] { done(EditInEditor); });
    auto* toEditor = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_E), this);
    connect(toEditor, &QShortcut::activated, this, [this] { done(EditInEditor); });
    auto* cancel = new QPushButton(tr("Cancel"), this);
    cancel->setAutoDefault(false);
    m_rename = new QPushButton(tr("Rename"), this);
    m_rename->setDefault(true);
    bottom->addWidget(inEditor);
    bottom->addWidget(cancel);
    bottom->addWidget(m_rename);
    layout->addLayout(bottom);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_rename, &QPushButton::clicked, this, &QDialog::accept);

    // Completion popup: floats under the template while typing a {field}.
    m_completions = new QListWidget(this);
    m_completions->setObjectName(QStringLiteral("renameCompletions"));
    m_completions->setFocusPolicy(Qt::NoFocus);
    m_completions->setUniformItemSizes(true);
    setThemeStyle(m_completions, QStringLiteral(
        "QListWidget { border: 1px solid @accent; border-radius: 6px; background: @base; padding: 3px; }"
        "QListWidget::item { padding: 4px 8px; border-radius: 4px; }"
        "QListWidget::item:selected { background: @accent; color: @accentText; }"));
    m_completions->hide();
    connect(m_completions, &QListWidget::itemClicked, this, [this] { acceptCompletion(); });
    m_template->installEventFilter(this);
    connect(m_template, &QLineEdit::cursorPositionChanged, this, &MultiRenameDialog::updateCompletions);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(40);
    connect(&m_debounce, &QTimer::timeout, this, &MultiRenameDialog::recompute);
    auto schedule = [this] { m_debounce.start(); };
    for (QLineEdit* e : {m_template, m_find, m_replace})
        connect(e, &QLineEdit::textChanged, this, schedule);
    connect(m_template, &QLineEdit::textChanged, this, &MultiRenameDialog::updateCompletions);
    for (QToolButton* b : {m_regex, m_matchCase})
        connect(b, &QToolButton::toggled, this, schedule);
    connect(m_case, &QButtonGroup::idToggled, this, [schedule](int, bool on) {
        if (on)
            schedule();
    });

    loadSettings();
    recompute();
    m_template->setFocus();
    m_template->selectAll();
}

QToolButton* MultiRenameDialog::chip(const QString& text, const QString& field, const QString& tip)
{
    auto* b = new QToolButton(this);
    b->setObjectName(QStringLiteral("chip"));
    b->setText(QStringLiteral("+ ") + text);
    b->setToolTip(QStringLiteral("%1\n%2").arg(field, tip));
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    connect(b, &QToolButton::clicked, this, [this, field] { insertField(field); });
    return b;
}

void MultiRenameDialog::insertField(const QString& field)
{
    if (m_template->hasSelectedText() && m_template->selectedText() == m_template->text() &&
        m_template->text() == QLatin1String("{name}{ext}"))
        m_template->setCursorPosition(int(QStringLiteral("{name}").size()));  // fresh: add after the name
    m_template->insert(field);
    m_template->setFocus();
}

void MultiRenameDialog::updateCompletions()
{
    const QString text = m_template->text();
    const int cursor = m_template->cursorPosition();
    const int start = m_template->hasFocus() || !m_completions->isHidden() ? openBrace(text, cursor) : -1;
    const QList<Suggestion> list = start < 0 ? QList<Suggestion>{} : suggestionsFor(text.mid(start, cursor - start));
    if (list.isEmpty()) {
        m_completions->hide();
        return;
    }
    m_completions->clear();
    for (const Suggestion& s : list) {
        auto* item = new QListWidgetItem(QStringLiteral("%1    %2").arg(s.label, s.hint), m_completions);
        item->setData(Qt::UserRole, s.body);
    }
    m_completions->setCurrentRow(0);
    const int rowH = m_completions->sizeHintForRow(0);
    const int w = qMax(360, m_completions->sizeHintForColumn(0) + 30);
    const QPoint at = m_template->mapTo(this, QPoint(m_template->inputMethodQuery(Qt::ImCursorRectangle).toRect().left(), m_template->height() + 2));
    m_completions->setGeometry(qMin(at.x(), width() - w - 8), at.y(), w, rowH * int(list.size()) + 10);
    m_completions->show();
    m_completions->raise();
}

void MultiRenameDialog::acceptCompletion()
{
    QListWidgetItem* item = m_completions->currentItem();
    if (!item)
        return;
    const QString text = m_template->text();
    const int cursor = m_template->cursorPosition();
    const int start = openBrace(text, cursor);
    if (start < 0)
        return;
    QString body = item->data(Qt::UserRole).toString();
    const bool closed = cursor < text.size() && text[cursor] == QLatin1Char('}');
    QString rest = text.mid(cursor);
    if (!closed)
        rest.prepend(QLatin1Char('}'));
    m_template->setText(text.left(start) + body + rest);
    m_template->setCursorPosition(int(start + body.size() + 1));
    m_completions->hide();
}

bool MultiRenameDialog::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_template && e->type() == QEvent::KeyPress && m_completions->isVisible()) {
        auto* k = static_cast<QKeyEvent*>(e);
        const int row = m_completions->currentRow();
        switch (k->key()) {
        case Qt::Key_Down:
            m_completions->setCurrentRow(qMin(row + 1, m_completions->count() - 1));
            return true;
        case Qt::Key_Up:
            m_completions->setCurrentRow(qMax(row - 1, 0));
            return true;
        case Qt::Key_Tab:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            acceptCompletion();
            return true;
        case Qt::Key_Escape:
            m_completions->hide();
            return true;
        default:
            break;
        }
    }
    if (obj == m_template && e->type() == QEvent::FocusOut)
        QTimer::singleShot(0, this, [this] {
            if (!m_template->hasFocus())
                m_completions->hide();
        });
    return QDialog::eventFilter(obj, e);
}

RenameOptions MultiRenameDialog::options() const
{
    RenameOptions o;
    o.pattern = m_template->text().isEmpty() ? QStringLiteral("{name}{ext}") : m_template->text();
    o.find = m_find->text();
    o.replace = m_replace->text();
    o.regex = m_regex->isChecked();
    o.matchCase = m_matchCase->isChecked();
    o.caseMode = RenameOptions::Case(qMax(0, m_case->checkedId()));
    return o;
}

void MultiRenameDialog::readPhotoDates()
{
    m_readingDates = true;
    QPointer<MultiRenameDialog> self(this);
    QStringList paths;
    for (const RenameItem& it : std::as_const(m_items))
        paths << (it.isDir ? QString() : joinPath(m_folder, it.name));
    ioPool()->start([self, paths] {
        auto dates = std::make_shared<QList<int64_t>>();
        for (const QString& p : paths)
            dates->append(p.isEmpty() ? 0 : exifTakenTime(p));
        postToUi([self, dates] {
            if (!self)
                return;
            for (int i = 0; i < self->m_items.size() && i < dates->size(); ++i)
                self->m_items[i].taken = dates->at(i);
            self->m_readingDates = false;
            self->m_datesRead = true;
            self->recompute();
        });
    });
}

void MultiRenameDialog::recompute()
{
    const RenameOptions o = options();
    if (o.pattern.contains(QLatin1String("{taken")) && !m_datesRead && !m_readingDates)
        readPhotoDates();
    m_plan = buildRenamePlan(m_items, baseName(m_folder), o, m_existing);
    // Half-typed templates are errors: keep showing the last good preview, dimmed.
    if (m_plan.error.isEmpty())
        m_model->setPlan(m_plan);
    m_table->setEnabled(m_plan.error.isEmpty());
    const Theme& t = theme();
    QString text;
    QColor color = t.dimText;
    if (!m_plan.error.isEmpty()) {
        text = m_plan.error;
        color = t.error;
    } else if (m_plan.problems) {
        text = plural(tr("%n name(s) can't be used", nullptr, m_plan.problems), m_plan.problems);
        color = t.error;
    } else if (m_readingDates) {
        text = tr("Reading photo dates …");
    } else if (m_plan.changes == 0) {
        text = tr("Nothing changes yet");
    } else {
        text = tr("%1 of %2 change").arg(m_plan.changes).arg(m_plan.rows.size());
        color = t.text;
    }
    m_summary->setText(text);
    QPalette pal = m_summary->palette();
    pal.setColor(QPalette::WindowText, color);
    m_summary->setPalette(pal);
    const bool ready = m_plan.canRun() && !m_readingDates;
    m_rename->setEnabled(ready);
    m_rename->setText(ready ? plural(tr("Rename %n", nullptr, m_plan.changes), m_plan.changes) : tr("Rename"));
}

void MultiRenameDialog::accept()
{
    if (m_completions->isVisible())
        return;
    m_debounce.stop();
    recompute();  // never act on a stale preview
    if (!m_plan.canRun() || m_readingDates)
        return;
    saveSettings();
    QDialog::accept();
}

void MultiRenameDialog::loadSettings()
{
    QSettings s;
    s.beginGroup(QStringLiteral("multiRename"));
    m_template->setText(s.value(QStringLiteral("template"), QStringLiteral("{name}{ext}")).toString());
    m_find->setText(s.value(QStringLiteral("find")).toString());
    m_replace->setText(s.value(QStringLiteral("replaceWith")).toString());
    m_regex->setChecked(s.value(QStringLiteral("regex"), false).toBool());
    m_matchCase->setChecked(s.value(QStringLiteral("matchCase"), false).toBool());
    if (QAbstractButton* b = m_case->button(s.value(QStringLiteral("caseStyle"), 0).toInt()))
        b->setChecked(true);
}

void MultiRenameDialog::saveSettings() const
{
    QSettings s;
    s.beginGroup(QStringLiteral("multiRename"));
    const RenameOptions o = options();
    s.setValue(QStringLiteral("template"), o.pattern);
    s.setValue(QStringLiteral("find"), o.find);
    s.setValue(QStringLiteral("replaceWith"), o.replace);
    s.setValue(QStringLiteral("regex"), o.regex);
    s.setValue(QStringLiteral("matchCase"), o.matchCase);
    s.setValue(QStringLiteral("caseStyle"), int(o.caseMode));
    QStringList recent = s.value(QStringLiteral("recent")).toStringList();
    recent.removeAll(o.pattern);
    if (o.pattern != QLatin1String("{name}{ext}"))
        recent.prepend(o.pattern);
    s.setValue(QStringLiteral("recent"), recent.mid(0, 12));
}

}  // namespace tandem
