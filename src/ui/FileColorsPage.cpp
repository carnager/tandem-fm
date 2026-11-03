#include "ui/FileColorsPage.h"
#include "core/Config.h"
#include "ui/Theme.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace tandem {
namespace {
QIcon swatch(const QColor& color)
{
    QPixmap pixmap(18, 18);
    pixmap.fill(color);
    return QIcon(pixmap);
}
QString matchLabel(const QString& match)
{
    return match == QLatin1String("extension") ? QObject::tr("Extension")
           : match == QLatin1String("mime") ? QObject::tr("MIME type") : QObject::tr("Kind");
}
bool editColorRule(QWidget* parent, FileColorRule& rule, bool adding)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("fileColorRuleDialog"));
    dialog.setWindowTitle(adding ? QObject::tr("Add file color rule") : QObject::tr("Edit file color rule"));
    dialog.resize(540, 330);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* match = new QComboBox(&dialog);
    match->setObjectName(QStringLiteral("fileColorMatch"));
    match->addItem(QObject::tr("Extension"), QStringLiteral("extension"));
    match->addItem(QObject::tr("File type (MIME)"), QStringLiteral("mime"));
    match->addItem(QObject::tr("File kind"), QStringLiteral("kind"));
    match->setCurrentIndex(qMax(0, match->findData(rule.match)));
    form->addRow(QObject::tr("Match by"), match);
    auto* targets = new QStackedWidget(&dialog);
    auto* extension = new QLineEdit(targets);
    extension->setObjectName(QStringLiteral("fileColorExtensions"));
    extension->setPlaceholderText(QStringLiteral("pdf; epub; tar.gz"));
    extension->setText(rule.match == QLatin1String("extension") ? rule.value : QString());
    auto* mime = new QComboBox(targets);
    mime->setObjectName(QStringLiteral("fileColorMime"));
    mime->setEditable(true);
    mime->addItems({QStringLiteral("image/*"), QStringLiteral("audio/*"), QStringLiteral("video/*"),
                    QStringLiteral("text/*"), QStringLiteral("application/pdf"), QStringLiteral("application/zip"),
                    QStringLiteral("application/json"), QStringLiteral("application/octet-stream")});
    mime->setCurrentText(rule.match == QLatin1String("mime") ? rule.value : QStringLiteral("image/*"));
    auto* kind = new QComboBox(targets);
    kind->setObjectName(QStringLiteral("fileColorKind"));
    kind->addItem(QObject::tr("Folders"), QStringLiteral("folder"));
    kind->addItem(QObject::tr("Symbolic links"), QStringLiteral("link"));
    kind->addItem(QObject::tr("Executable files"), QStringLiteral("executable"));
    kind->addItem(QObject::tr("Files without an extension"), QStringLiteral("no-extension"));
    kind->addItem(QObject::tr("All files"), QStringLiteral("file"));
    kind->setCurrentIndex(qMax(0, kind->findData(rule.value)));
    targets->addWidget(extension); targets->addWidget(mime); targets->addWidget(kind);
    targets->setCurrentIndex(match->currentIndex());
    auto* targetLabel = new QLabel(&dialog);
    form->addRow(targetLabel, targets);
    auto colorRow = [&](const QString& name, const QColor& initial) {
        auto* row = new QWidget(&dialog);
        auto* line = new QHBoxLayout(row); line->setContentsMargins(0, 0, 0, 0);
        auto* input = new QLineEdit(initial.name(QColor::HexRgb), row);
        input->setObjectName(name);
        input->setPlaceholderText(QStringLiteral("#3366cc"));
        auto* button = new QPushButton(swatch(initial), QObject::tr("Choose…"), row);
        button->setObjectName(name + QStringLiteral("Picker"));
        line->addWidget(input, 1); line->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, &dialog, [input, &dialog] {
            const QColor color = QColorDialog::getColor(QColor(input->text().trimmed()), &dialog, QObject::tr("Choose file text color"));
            if (color.isValid()) input->setText(color.name(QColor::HexRgb));
        });
        QObject::connect(input, &QLineEdit::textChanged, button, [button](const QString& text) {
            const QColor color(text.trimmed());
            button->setIcon(color.isValid() ? swatch(color) : QIcon());
        });
        return qMakePair(row, input);
    };
    const auto light = colorRow(QStringLiteral("fileColorLight"), rule.light);
    const auto dark = colorRow(QStringLiteral("fileColorDark"), rule.dark);
    form->addRow(QObject::tr("Light theme"), light.first);
    form->addRow(QObject::tr("Dark theme"), dark.first);
    layout->addLayout(form);
    auto* note = new QLabel(&dialog); note->setWordWrap(true);
    note->setObjectName(QStringLiteral("fileColorRuleNote"));
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    auto current = [&] {
        FileColorRule result = rule;
        result.match = match->currentData().toString();
        result.value = match->currentIndex() == 0 ? extension->text().trimmed()
                     : match->currentIndex() == 1 ? mime->currentText().trimmed() : kind->currentData().toString();
        result.light = QColor(light.second->text().trimmed()); result.dark = QColor(dark.second->text().trimmed());
        return result;
    };
    auto validate = [&] {
        targets->setCurrentIndex(match->currentIndex());
        targetLabel->setText(match->currentIndex() == 0 ? QObject::tr("Extensions")
                            : match->currentIndex() == 1 ? QObject::tr("MIME type") : QObject::tr("Kind"));
        const QString error = current().error();
        note->setText(error.isEmpty() ? QObject::tr("Extensions ignore case. MIME types are determined from the filename, without reading the file. Colors apply to name and extension text in both panels and thumbnails.") : error);
        buttons->button(QDialogButtonBox::Save)->setEnabled(error.isEmpty());
    };
    QObject::connect(match, &QComboBox::currentIndexChanged, &dialog, validate);
    QObject::connect(extension, &QLineEdit::textChanged, &dialog, validate);
    QObject::connect(mime, &QComboBox::currentTextChanged, &dialog, validate);
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, validate);
    QObject::connect(light.second, &QLineEdit::textChanged, &dialog, validate);
    QObject::connect(dark.second, &QLineEdit::textChanged, &dialog, validate);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validate();
    if (dialog.exec() != QDialog::Accepted) return false;
    rule = current();
    return rule.error().isEmpty();
}
} // namespace

FileColorsPage::FileColorsPage(QWidget* parent) : QWidget(parent), m_rules(FileColors::rules())
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 14, 10);
    auto* enabled = new QCheckBox(tr("Color file names using these rules"), this);
    enabled->setObjectName(QStringLiteral("fileColorsEnabled"));
    enabled->setChecked(Settings().value(QStringLiteral("view/fileColorsEnabled"), true).toBool());
    connect(enabled, &QCheckBox::toggled, this, [](bool on) { FileColors::setEnabled(on); });
    layout->addWidget(enabled);
    auto* help = new QLabel(tr("First matching enabled rule wins. Choose separate colors for light and dark themes. Marked files and broken links keep their highlight/error colors."), this);
    help->setWordWrap(true);
    setThemeStyle(help, QStringLiteral("color: @dimText;"));
    layout->addWidget(help);
    m_table = new QTreeWidget(this);
    m_table->setObjectName(QStringLiteral("fileColorRules"));
    m_table->setHeaderLabels({tr("On"), tr("Match"), tr("Value"), tr("Light"), tr("Dark")});
    m_table->setRootIsDecorated(false);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSortingEnabled(false);
    m_table->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);
    auto* row = new QHBoxLayout;
    auto make = [&](const QString& text, const QString& name) {
        auto* button = new QPushButton(text, this); button->setObjectName(name); row->addWidget(button); return button;
    };
    auto* add = make(tr("Add…"), QStringLiteral("addFileColorRule"));
    m_edit = make(tr("Edit…"), QStringLiteral("editFileColorRule"));
    m_remove = make(tr("Remove"), QStringLiteral("removeFileColorRule"));
    row->addStretch(1);
    m_up = make(tr("Up"), QStringLiteral("moveFileColorUp"));
    m_down = make(tr("Down"), QStringLiteral("moveFileColorDown"));
    layout->addLayout(row);
    auto* note = new QLabel(tr("Examples: extensions “jpg; png”, MIME type “image/*”, or kind “Folders”. Rules apply immediately and are saved in your selected configuration file."), this);
    note->setWordWrap(true);
    setThemeStyle(note, QStringLiteral("color: @dimText;"));
    layout->addWidget(note);
    connect(add, &QPushButton::clicked, this, [this] { editRule(true); });
    connect(m_edit, &QPushButton::clicked, this, [this] { editRule(false); });
    connect(m_table, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int column) { if (column) editRule(false); });
    connect(m_table, &QTreeWidget::currentItemChanged, this, [this] { updateButtons(); });
    connect(m_table, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_loading || column != 0) return;
        const int row = m_table->indexOfTopLevelItem(item);
        if (row < 0 || row >= m_rules.size()) return;
        m_rules[row].enabled = item->checkState(0) == Qt::Checked;
        FileColors::saveRules(m_rules);
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const int row = m_table->indexOfTopLevelItem(m_table->currentItem());
        if (row >= 0 && row < m_rules.size()) { m_rules.removeAt(row); save(qMin(row, int(m_rules.size()) - 1)); }
    });
    auto move = [this](int delta) {
        const int row = m_table->indexOfTopLevelItem(m_table->currentItem());
        if (row >= 0 && row + delta >= 0 && row + delta < m_rules.size()) { m_rules.swapItemsAt(row, row + delta); save(row + delta); }
    };
    connect(m_up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(m_down, &QPushButton::clicked, this, [move] { move(1); });
    fill();
}
void FileColorsPage::fill(int selected)
{
    m_loading = true;
    m_table->clear();
    for (const auto& rule : m_rules) {
        auto* item = new QTreeWidgetItem(m_table, {QString(), matchLabel(rule.match), rule.value, rule.light.name(), rule.dark.name()});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, rule.enabled ? Qt::Checked : Qt::Unchecked);
        item->setIcon(3, swatch(rule.light)); item->setIcon(4, swatch(rule.dark));
    }
    if (selected >= 0 && selected < m_table->topLevelItemCount()) m_table->setCurrentItem(m_table->topLevelItem(selected));
    m_loading = false;
    updateButtons();
}
void FileColorsPage::updateButtons()
{
    const int row = m_table->indexOfTopLevelItem(m_table->currentItem());
    m_edit->setEnabled(row >= 0); m_remove->setEnabled(row >= 0);
    m_up->setEnabled(row > 0); m_down->setEnabled(row >= 0 && row + 1 < m_rules.size());
}
void FileColorsPage::save(int selected) { FileColors::saveRules(m_rules); fill(selected); }
void FileColorsPage::editRule(bool add)
{
    const int selected = m_table->indexOfTopLevelItem(m_table->currentItem());
    if (!add && (selected < 0 || selected >= m_rules.size())) return;
    FileColorRule rule = add ? FileColorRule{} : m_rules[selected];
    if (!editColorRule(this, rule, add)) return;
    if (add) { m_rules.append(rule); save(int(m_rules.size()) - 1); }
    else { m_rules[selected] = rule; save(selected); }
}
} // namespace tandem
