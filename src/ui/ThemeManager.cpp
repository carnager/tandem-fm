#include "ui/ThemeManager.h"
#include "ui/ThemeProfiles.h"
#include "ui/FileColorsPage.h"
#include "core/Config.h"
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QVBoxLayout>

namespace tandem {
namespace {
QIcon swatch(const QColor& color)
{
    QPixmap pixmap(24, 20); pixmap.fill(Qt::white);
    QPainter painter(&pixmap);
    painter.fillRect(0, 0, 12, 10, QColor("#999999")); painter.fillRect(12, 10, 12, 10, QColor("#999999"));
    painter.fillRect(pixmap.rect(), color);
    return QIcon(pixmap);
}
class ThemePreview : public QWidget {
public:
    Theme palette;
    explicit ThemePreview(QWidget* parent) : QWidget(parent) { setMinimumHeight(132); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this); p.fillRect(rect(), palette.base);
        const QStringList names{QStringLiteral("Documents"), QStringLiteral("photo.jpg"), QStringLiteral("report.pdf"), QStringLiteral("notes.txt")};
        const int h = height() / 4;
        for (int i = 0; i < 4; ++i) {
            const QRect row(0, i * h, width(), h);
            if (palette.alternatingRows && i % 2) p.fillRect(row, palette.altBase);
            if (i == 2) p.fillRect(row, palette.markBg);
            if (i == 3) p.fillRect(row.adjusted(2, 1, -2, -1), palette.cursorActive);
            p.setPen(i == 2 ? palette.mark : i == 3 && palette.selectionTextEnabled ? palette.selectionText : palette.text);
            p.drawText(row.adjusted(10, 0, -10, 0), Qt::AlignVCenter, names[i]);
            p.setPen(i == 2 ? palette.mark : i == 3 && palette.selectionTextEnabled ? palette.selectionText : palette.extensionText);
            p.drawText(row.adjusted(width() * 2 / 3, 0, -10, 0), Qt::AlignVCenter, i == 0 ? QString() : names[i].section('.', -1));
        }
    }
};
bool editTheme(QWidget* parent, ThemeProfile profile)
{
    QDialog dialog(parent); dialog.setObjectName(QStringLiteral("themeEditor"));
    dialog.setWindowTitle(QObject::tr("Edit custom theme")); dialog.resize(650, 760);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(profile.name, &dialog); name->setObjectName(QStringLiteral("themeName")); name->setMaxLength(80);
    form->addRow(QObject::tr("Name"), name);
    auto* dark = new QCheckBox(QObject::tr("Use dark-mode file color rules"), &dialog);
    dark->setObjectName(QStringLiteral("themeDark")); dark->setChecked(profile.palette.dark);
    form->addRow(QString(), dark);
    auto* alternate = new QCheckBox(QObject::tr("Alternate row backgrounds"), &dialog);
    alternate->setObjectName(QStringLiteral("themeAlternatingRows")); alternate->setChecked(profile.palette.alternatingRows);
    form->addRow(QString(), alternate);
    auto* selectedText = new QCheckBox(QObject::tr("Use a separate selection text color"), &dialog);
    selectedText->setObjectName(QStringLiteral("themeSelectionTextEnabled")); selectedText->setChecked(profile.palette.selectionTextEnabled);
    form->addRow(QString(), selectedText);
    layout->addLayout(form);
    auto* preview = new ThemePreview(&dialog); preview->setObjectName(QStringLiteral("themePreview"));
    layout->addWidget(preview);
    auto* scroll = new QScrollArea(&dialog); scroll->setWidgetResizable(true);
    auto* colors = new QWidget(scroll); auto* colorForm = new QFormLayout(colors);
    QMap<QString, QLineEdit*> inputs;
    for (const auto& role : themeColorRoles()) {
        auto* row = new QWidget(colors); auto* line = new QHBoxLayout(row); line->setContentsMargins(0, 0, 0, 0);
        const QColor initial = profile.palette.*role.member;
        auto* input = new QLineEdit(initial.name(role.alpha ? QColor::HexArgb : QColor::HexRgb), row);
        input->setObjectName(QStringLiteral("themeColor_") + QLatin1String(role.key));
        input->setToolTip(role.alpha ? QObject::tr("#RRGGBB or #AARRGGBB; AA is opacity (00–FF).") : QObject::tr("Opaque color: #RRGGBB."));
        input->setMaxLength(9); inputs.insert(QLatin1String(role.key), input);
        auto* button = new QPushButton(swatch(initial), QObject::tr("Choose…"), row);
        button->setObjectName(input->objectName() + QStringLiteral("Picker"));
        line->addWidget(input, 1); line->addWidget(button);
        colorForm->addRow(QObject::tr(role.label), row);
        QObject::connect(button, &QPushButton::clicked, &dialog, [input, role, &dialog] {
            QColorDialog picker(QColor(input->text()), &dialog);
            picker.setOption(QColorDialog::ShowAlphaChannel, role.alpha);
            picker.setWindowTitle(QObject::tr(role.label));
            if (picker.exec() == QDialog::Accepted)
                input->setText(picker.selectedColor().name(role.alpha ? QColor::HexArgb : QColor::HexRgb));
        });
        QObject::connect(input, &QLineEdit::textChanged, button, [button](const QString& value) {
            const QColor color(value); button->setIcon(color.isValid() ? swatch(color) : QIcon());
        });
    }
    scroll->setWidget(colors); layout->addWidget(scroll, 1);
    auto* note = new QLabel(QObject::tr("File color rules take priority over default extension text. Mark and error colors take priority over selection text. The preview changes here; Save and use applies the theme to Tandem."), &dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto* error = new QLabel(&dialog); error->setObjectName(QStringLiteral("themeError")); error->setWordWrap(true); layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QObject::tr("Save and use")); layout->addWidget(buttons);
    auto current = [&] {
        ThemeProfile result = profile; result.name = name->text().trimmed();
        result.palette.dark = dark->isChecked(); result.palette.alternatingRows = alternate->isChecked();
        result.palette.selectionTextEnabled = selectedText->isChecked();
        for (const auto& role : themeColorRoles()) result.palette.*role.member = QColor(inputs.value(QLatin1String(role.key))->text().trimmed());
        return result;
    };
    auto validate = [&] {
        const auto result = current();
        QString problem = themeProfileError(result);
        // Require the same hexadecimal syntax as imported theme files.
        for (const auto& role : themeColorRoles()) {
            const QString raw = inputs.value(QLatin1String(role.key))->text().trimmed();
            const QColor color(raw);
            const bool hex = raw.startsWith('#') && (raw.size() == 7 || raw.size() == 9);
            if (!hex || !color.isValid() || (!role.alpha && color.alpha() != 255)) {
                problem = QObject::tr("Enter a valid %1 color%2.").arg(QObject::tr(role.label), role.alpha ? QString() : QObject::tr(" without transparency"));
                break;
            }
        }
        if (problem.isEmpty()) {
            preview->palette = result.palette; preview->update();
        }
        error->setText(problem);
        buttons->button(QDialogButtonBox::Save)->setEnabled(problem.isEmpty());
    };
    for (auto* input : inputs) QObject::connect(input, &QLineEdit::textChanged, &dialog, validate);
    QObject::connect(name, &QLineEdit::textChanged, &dialog, validate);
    for (auto* box : {dark, alternate, selectedText}) QObject::connect(box, &QCheckBox::toggled, &dialog, validate);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QString problem;
        if (!saveThemeProfile(current(), &problem)) {
            error->setText(problem.isEmpty() ? QObject::tr("The theme could not be saved. Check your configuration folder.") : problem); return;
        }
        selectTheme(profile.id); dialog.accept();
    });
    validate();
    return dialog.exec() == QDialog::Accepted;
}
void showFileColors(QWidget* parent)
{
    QDialog dialog(parent); dialog.setWindowTitle(QObject::tr("File and extension colors")); dialog.resize(800, 530);
    auto* layout = new QVBoxLayout(&dialog); layout->addWidget(new FileColorsPage(&dialog));
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog); layout->addWidget(close);
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); dialog.exec();
}
} // namespace
void showThemeManager(QWidget* parent)
{
    QDialog dialog(parent); dialog.setObjectName(QStringLiteral("themeManager"));
    dialog.setWindowTitle(QObject::tr("Themes")); dialog.resize(650, 480);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(QObject::tr("Choose a theme, or add a custom copy to edit its colors. Custom themes are saved with your configuration and can be imported or exported as Tandem theme JSON files."), &dialog);
    help->setWordWrap(true); layout->addWidget(help);
    auto* list = new QListWidget(&dialog); list->setObjectName(QStringLiteral("themeList")); layout->addWidget(list, 1);
    auto* row = new QHBoxLayout;
    auto make = [&](const QString& label, const QString& objectName) {
        auto* button = new QPushButton(label, &dialog); button->setObjectName(objectName); row->addWidget(button); return button;
    };
    auto* use = make(QObject::tr("Use theme"), "useTheme");
    auto* add = make(QObject::tr("Add custom…"), "addTheme");
    auto* edit = make(QObject::tr("Edit…"), "editTheme");
    auto* remove = make(QObject::tr("Remove"), "removeTheme"); layout->addLayout(row);
    auto* extras = new QHBoxLayout;
    auto* import = new QPushButton(QObject::tr("Import…"), &dialog); import->setObjectName("importTheme");
    auto* exportButton = new QPushButton(QObject::tr("Export…"), &dialog); exportButton->setObjectName("exportTheme");
    auto* fileColors = new QPushButton(QObject::tr("File and extension colors…"), &dialog);
    extras->addWidget(import); extras->addWidget(exportButton); extras->addStretch(); extras->addWidget(fileColors); layout->addLayout(extras);
    auto selected = [&]() -> std::optional<ThemeProfile> {
        return list->currentItem() ? findThemeProfile(list->currentItem()->data(Qt::UserRole).toString()) : std::nullopt;
    };
    auto fill = [&](QString id = QString()) {
        if (id.isEmpty()) id = Settings().value("view/theme", "system").toString();
        list->clear();
        for (const auto& profile : themeProfiles()) {
            auto* item = new QListWidgetItem(swatch(profile.palette.base), profile.name + (profile.builtIn ? QObject::tr(" (built-in)") : QString()), list);
            item->setData(Qt::UserRole, profile.id);
            if (profile.id == id) list->setCurrentItem(item);
        }
        if (!list->currentItem()) list->setCurrentRow(0);
    };
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&] {
        const auto profile = selected(); use->setEnabled(profile.has_value()); exportButton->setEnabled(profile.has_value());
        edit->setEnabled(profile && !profile->builtIn); remove->setEnabled(profile && !profile->builtIn);
    });
    QObject::connect(use, &QPushButton::clicked, &dialog, [&] { if (auto profile = selected()) selectTheme(profile->id); });
    QObject::connect(list, &QListWidget::itemDoubleClicked, use, [use] { use->click(); });
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        const auto base = selected().value_or(ThemeProfile{"", QObject::tr("Custom theme"), theme(), false});
        auto profile = duplicateTheme(base);
        if (editTheme(&dialog, profile)) fill(profile.id);
    });
    QObject::connect(edit, &QPushButton::clicked, &dialog, [&] { if (auto profile = selected(); profile && !profile->builtIn && editTheme(&dialog, *profile)) fill(profile->id); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        auto profile = selected(); if (!profile || profile->builtIn) return;
        if (QMessageBox::question(&dialog, QObject::tr("Remove custom theme"), QObject::tr("Remove “%1”? This removes it from your synced configuration.").arg(profile->name)) != QMessageBox::Yes) return;
        if (removeThemeProfile(profile->id)) { applyTheme(*qApp); fill(); }
    });
    QObject::connect(import, &QPushButton::clicked, &dialog, [&] {
        const QString path = QFileDialog::getOpenFileName(&dialog, QObject::tr("Import Tandem theme"), {}, QObject::tr("Tandem theme (*.json)"));
        if (path.isEmpty()) return;
        QFile file(path); QString problem;
        std::optional<ThemeProfile> profile;
        if (!file.open(QIODevice::ReadOnly)) problem = file.errorString();
        else profile = importTheme(file.read(256 * 1024 + 1), &problem);
        if (profile && saveThemeProfile(*profile, &problem)) { selectTheme(profile->id); fill(profile->id); return; }
        QMessageBox::warning(&dialog, QObject::tr("Could not import theme"), problem.isEmpty() ? QObject::tr("Check your configuration folder.") : problem);
    });
    QObject::connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        const auto profile = selected(); if (!profile) return;
        const QString path = QFileDialog::getSaveFileName(&dialog, QObject::tr("Export Tandem theme"), QStringLiteral("theme.json"), QObject::tr("Tandem theme (*.json)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto data = exportTheme(*profile);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
            QMessageBox::warning(&dialog, QObject::tr("Could not export theme"), file.errorString());
    });
    QObject::connect(fileColors, &QPushButton::clicked, &dialog, [&] { showFileColors(&dialog); });
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog); layout->addWidget(close);
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    fill(); dialog.exec();
}
void populateThemeMenu(QMenu* menu, QWidget* parent)
{
    menu->clear();
    // The group belongs to the first action, so clear() also releases it.
    auto* system = menu->addAction(QObject::tr("Like the desktop"));
    auto* group = new QActionGroup(system); group->setExclusive(true);
    const QString selected = Settings().value("view/theme", "system").toString();
    auto choice = [&](QAction* action, const QString& id) {
        action->setCheckable(true); group->addAction(action);
        action->setChecked(selected == id || (id == QLatin1String("system") && !findThemeProfile(selected)));
        action->setData(id);
        QObject::connect(action, &QAction::triggered, parent, [id] { selectTheme(id); });
    };
    choice(system, "system");
    for (const auto& profile : themeProfiles()) choice(menu->addAction(profile.name), profile.id);
    menu->addSeparator();
    menu->addAction(QObject::tr("Add / manage themes…"), parent, [parent] { showThemeManager(parent); });
    menu->addAction(QObject::tr("File and extension colors…"), parent, [parent] { showFileColors(parent); });
}
} // namespace tandem
