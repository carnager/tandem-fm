#include "ui/PluginManager.h"
#include "core/Util.h"
#include "core/Backend.h"
#include "ui/Panel.h"
#include "ui/DirTab.h"
#include "ui/FileModel.h"
#include "core/Config.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace tandem {
namespace {
QString cleanPath(const QString& path)
{
    if (!isRemoteLocation(path)) return path;
    return QUrl(path).toString(QUrl::RemovePassword | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::FullyEncoded);
}
QJsonObject panelInfo(Panel* panel)
{
    if (!panel) return {};
    const QString kind = panel->inArchive() ? QStringLiteral("archive") : panel->isRemote() ? QStringLiteral("remote") : QStringLiteral("local");
    QJsonObject result{{"path", cleanPath(panel->path())}, {"kind", kind}, {"is_local", kind == "local"}};
    if (panel->inArchive()) result.insert("archive", cleanPath(panel->archivePath()));
    return result;
}
void showText(QWidget* parent, const QString& title, const QString& text)
{
    auto* dialog = new QDialog(parent); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(title); dialog->resize(700, 450);
    auto* layout = new QVBoxLayout(dialog);
    auto* output = new QPlainTextEdit(dialog); output->setReadOnly(true); output->setPlainText(text); layout->addWidget(output);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog); layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject); dialog->show();
}
void showList(QWidget* parent, const PluginList& list)
{
    auto* dialog = new QDialog(parent); dialog->setObjectName("pluginResultList"); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(list.title); dialog->resize(800, 500); auto* layout = new QVBoxLayout(dialog);
    auto* table = new QTreeWidget(dialog); table->setRootIsDecorated(false); table->setUniformRowHeights(true);
    table->setHeaderLabels({QObject::tr("Item"), QObject::tr("Detail"), QObject::tr("Link")});
    table->header()->setSectionResizeMode(QHeaderView::ResizeToContents); table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    QStringList copy;
    for (const auto& item : list.items) {
        new QTreeWidgetItem(table, {item.text, item.detail, item.url});
        QString line = item.text; if (!item.detail.isEmpty()) line += '\t' + item.detail; if (!item.url.isEmpty()) line += '\t' + item.url;
        copy << line;
    }
    layout->addWidget(table);
    QObject::connect(table, &QTreeWidget::itemDoubleClicked, dialog, [](QTreeWidgetItem* item) {
        if (!item->text(2).isEmpty()) QDesktopServices::openUrl(QUrl(item->text(2)));
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* button = buttons->addButton(QObject::tr("Copy all"), QDialogButtonBox::ActionRole);
    QObject::connect(button, &QPushButton::clicked, dialog, [copy] { QApplication::clipboard()->setText(copy.join('\n')); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject); layout->addWidget(buttons); dialog->show();
}
}
PluginContext capturePluginContext(Panel* active, Panel* other)
{
    PluginContext context; context.active = panelInfo(active); context.other = panelInfo(other);
    context.location = context.active.value("kind").toString();
    if (!active || !active->tab()) { context.error = QObject::tr("No active file panel."); return context; }
    const QStringList paths = active->selectedPaths();
    if (paths.size() > 5000) { context.error = QObject::tr("Select at most 5000 files for a plugin action."); return context; }
    const FileModel* model = active->tab()->model();
    QHash<QString, const FileEntry*> entries;
    for (int i = 0; i < model->count(); ++i) if (!model->entry(i)->isParent) entries.insert(model->filePath(i), model->entry(i));
    for (const auto& entry : model->filteredOut()) entries.insert(joinPath(model->path(), entry.name), &entry);
    for (const auto& path : paths) {
        const auto* e = entries.value(path, nullptr);
        if (!e || e->isParent) { context.error = QObject::tr("Selection changed; reopen the plugin menu."); return context; }
        const QString cleaned = cleanPath(path);
        QJsonObject item{{"name", e->name}, {"path", cleaned}, {"is_local", context.location == "local"},
            {"is_directory", e->isDir}, {"is_link", e->isLink}, {"broken_link", e->brokenLink}, {"marked", e->marked},
            {"size", e->size < 0 ? QJsonValue() : QJsonValue(qint64(e->size))}, {"mode", int(e->mode)},
            {"modified", e->mtime == 0 ? QJsonValue() : QJsonValue(qint64(e->mtime))},
            {"created", e->createdTime ? QJsonValue(qint64(*e->createdTime)) : QJsonValue()},
            {"added", e->addedTime ? QJsonValue(qint64(*e->addedTime)) : QJsonValue()}};
        if (context.location == "local") item.insert("uri", QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
        else if (context.location == "remote") item.insert("uri", cleaned);
        if (active->inArchive()) item.insert("archive_path", active->innerPath(path));
        context.selected.append(item);
    }
    return context;
}
PluginManagerPage::PluginManagerPage(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    auto* help = new QLabel(tr("Plugins add file actions. Each runs as a separate program with your account's permissions. Enable only plugins you trust. File changes made by plugins are outside Tandem's undo."), this);
    help->setWordWrap(true); layout->addWidget(help);
    m_table = new QTreeWidget(this); m_table->setObjectName("pluginTable"); m_table->setRootIsDecorated(false);
    m_table->setHeaderLabels({tr("On"), tr("Plugin"), tr("Description / problem")});
    m_table->header()->setSectionResizeMode(QHeaderView::ResizeToContents); m_table->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);
    auto* row = new QHBoxLayout;
    auto* install = new QPushButton(tr("Install folder…"), this); install->setObjectName("installPlugin");
    m_settings = new QPushButton(tr("Settings…"), this); m_settings->setObjectName("pluginSettings");
    auto* reloadButton = new QPushButton(tr("Reload"), this); reloadButton->setObjectName("reloadPlugins");
    auto* open = new QPushButton(tr("Open plugins folder"), this);
    row->addWidget(install); row->addWidget(m_settings); row->addStretch(); row->addWidget(reloadButton); row->addWidget(open); layout->addLayout(row);
    auto* location = new QLabel(tr("User plugins: %1\nNew plugins stay off until enabled. Reload after editing a manifest. Disable a plugin to remove its actions.").arg(userPluginFolder()), this);
    location->setTextFormat(Qt::PlainText); location->setWordWrap(true); location->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(location);
    connect(reloadButton, &QPushButton::clicked, this, &PluginManagerPage::reload);
    connect(install, &QPushButton::clicked, this, &PluginManagerPage::install);
    connect(m_settings, &QPushButton::clicked, this, &PluginManagerPage::editSettings);
    connect(open, &QPushButton::clicked, this, [this] {
        if (!QDir().mkpath(userPluginFolder()) || !QDesktopServices::openUrl(QUrl::fromLocalFile(userPluginFolder())))
            QMessageBox::warning(this, tr("Plugins"), tr("Could not open the user plugins folder."));
    });
    connect(m_table, &QTreeWidget::currentItemChanged, this, [this] { updateButtons(); });
    connect(m_table, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_loading || column != 0) return;
        const int row = m_table->indexOfTopLevelItem(item);
        if (row >= 0 && row < m_plugins.size() && m_plugins[row].plugin) setPluginEnabled(m_plugins[row].id, item->checkState(0) == Qt::Checked);
    });
    reload();
}
void PluginManagerPage::reload()
{
    const auto current = selected();
    m_loading = true; m_table->clear(); m_plugins = discoverPlugins();
    for (const auto& found : m_plugins) {
        auto* item = new QTreeWidgetItem(m_table, {QString(), found.plugin ? found.plugin->name : found.id,
            found.plugin ? found.plugin->description : found.problem});
        item->setToolTip(1, found.folder); item->setToolTip(2, found.problem);
        if (found.plugin) { item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(0, pluginEnabled(found.id) ? Qt::Checked : Qt::Unchecked); }
        if (current && current->id == found.id) m_table->setCurrentItem(item);
    }
    if (!m_table->currentItem() && m_table->topLevelItemCount()) m_table->setCurrentItem(m_table->topLevelItem(0));
    m_loading = false; updateButtons();
}
std::optional<Plugin> PluginManagerPage::selected() const
{
    const int row = m_table->indexOfTopLevelItem(m_table->currentItem());
    return row < 0 || row >= m_plugins.size() ? std::nullopt : m_plugins[row].plugin;
}
void PluginManagerPage::updateButtons() { const auto plugin = selected(); m_settings->setEnabled(plugin && !plugin->settings.isEmpty()); }
void PluginManagerPage::editSettings()
{
    const auto plugin = selected(); if (!plugin) return;
    QDialog dialog(this); dialog.setObjectName("pluginSettingsDialog"); dialog.setWindowTitle(tr("%1 settings").arg(plugin->name)); dialog.resize(550, 420);
    auto* layout = new QVBoxLayout(&dialog); auto* scroll = new QScrollArea(&dialog); scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll); auto* form = new QFormLayout(content);
    QMap<QString, QWidget*> inputs;
    for (const auto& setting : plugin->settings) {
        QWidget* input;
        const auto value = pluginSettingValue(*plugin, setting);
        if (setting.kind == "toggle") {
            auto* box = new QCheckBox(content); box->setChecked(value.toBool()); input = box;
        } else if (setting.kind == "choice") {
            auto* choice = new QComboBox(content); choice->addItems(setting.choices); choice->setCurrentText(value.toString()); input = choice;
        } else {
            auto* edit = new QLineEdit(value.toString(), content); edit->setMaxLength(8192);
            if (setting.kind == "secret") edit->setEchoMode(QLineEdit::Password);
            input = edit;
        }
        input->setObjectName("pluginSetting_" + setting.key); input->setToolTip(setting.help); inputs.insert(setting.key, input);
        auto* group = new QWidget(content); auto* stack = new QVBoxLayout(group); stack->setContentsMargins(setting.requiresToggle.isEmpty() ? 0 : 16, 0, 0, 0); stack->addWidget(input);
        if (!setting.help.isEmpty()) { auto* help = new QLabel(setting.help, group); help->setTextFormat(Qt::PlainText); help->setWordWrap(true); stack->addWidget(help); }
        if (!setting.link.isEmpty()) {
            auto* link = new QPushButton(tr("Open setup page"), group); stack->addWidget(link);
            connect(link, &QPushButton::clicked, &dialog, [url = setting.link] { QDesktopServices::openUrl(QUrl(url)); });
        }
        form->addRow(setting.label, group);
        if (!setting.requiresToggle.isEmpty()) {
            auto* toggle = qobject_cast<QCheckBox*>(inputs.value(setting.requiresToggle));
            if (toggle) { group->setEnabled(toggle->isChecked()); connect(toggle, &QCheckBox::toggled, group, &QWidget::setEnabled); }
        }
    }
    scroll->setWidget(content); layout->addWidget(scroll, 1);
    auto* note = new QLabel(tr("Preferences use your selected configuration. Secret values stay on this device in a private file."), &dialog); note->setWordWrap(true); layout->addWidget(note);
    auto* error = new QLabel(&dialog); error->setWordWrap(true); error->setTextFormat(Qt::PlainText); layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QJsonObject values;
        for (const auto& setting : plugin->settings) {
            QWidget* input = inputs.value(setting.key);
            if (auto* box = qobject_cast<QCheckBox*>(input)) values.insert(setting.key, box->isChecked());
            else if (auto* choice = qobject_cast<QComboBox*>(input)) values.insert(setting.key, choice->currentText());
            else values.insert(setting.key, qobject_cast<QLineEdit*>(input)->text());
        }
        QString problem;
        if (!savePluginSettings(*plugin, values, &problem)) { error->setText(problem); return; }
        dialog.accept();
    });
    dialog.exec();
}
void PluginManagerPage::install()
{
    const QString source = QFileDialog::getExistingDirectory(this, tr("Choose a plugin folder containing plugin.json"));
    if (source.isEmpty()) return;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto* progress = new QProgressDialog(tr("Installing plugin…"), tr("Cancel"), 0, 0, this); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->show();
    connect(progress, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });
    connect(this, &QObject::destroyed, [cancel] { cancel->store(true); });
    QPointer<PluginManagerPage> self(this); QPointer<QProgressDialog> guardedProgress(progress);
    const QString destination = userPluginFolder();
    ioPool()->start([source, destination, cancel, self, guardedProgress] {
        const auto result = installPluginFolder(source, destination, cancel.get());
        postToUi([self, guardedProgress, result] {
            if (guardedProgress) { guardedProgress->close(); guardedProgress->deleteLater(); }
            if (result.error.isEmpty()) setPluginEnabled(result.id, false);
            if (!self) return;
            if (result.error.isEmpty()) self->reload();
            else QMessageBox::warning(self, QObject::tr("Plugin installation"), result.error);
        });
    });
}
PluginController::PluginController(QWidget* parent) : QObject(parent), m_window(parent) {}
void PluginController::showManager()
{
    QDialog dialog(m_window); dialog.setObjectName("pluginManager"); dialog.setWindowTitle(tr("Plugins")); dialog.resize(850, 500);
    auto* layout = new QVBoxLayout(&dialog); layout->addWidget(new PluginManagerPage(&dialog));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); dialog.exec();
}
void PluginController::populateMenu(QMenu* menu, Panel* active, Panel* other, bool management)
{
    menu->clear(); const auto context = capturePluginContext(active, other);
    bool any = false;
    for (const auto& found : discoverPlugins()) {
        if (!found.plugin || !pluginEnabled(found.id)) continue;
        const auto plugin = *found.plugin;
        auto* group = menu->addMenu(plugin.name);
        for (const auto& action : plugin.actions) {
            any = true;
            auto* item = group->addAction(action.label);
            item->setEnabled(pluginActionAvailable(action, context));
            item->setToolTip(context.error.isEmpty() ? tr("Run %1").arg(plugin.name) : context.error);
            connect(item, &QAction::triggered, this, [this, plugin, action, context, panel = QPointer<Panel>(active)] { if (panel) invoke(plugin, action, context, panel); });
        }
    }
    if (!any && management) menu->addAction(tr("No enabled plugins"))->setEnabled(false);
    if (management) {
        menu->addSeparator();
        menu->addAction(tr("Manage plugins…"), this, &PluginController::showManager);
        auto* cancel = menu->addAction(tr("Cancel running plugins (%1)").arg(m_runs.size()), this, &PluginController::cancelAll); cancel->setEnabled(!m_runs.isEmpty());
    }
}
void PluginController::cancelAll() { const auto runs = m_runs; for (auto* run : runs) run->cancel(); }
void PluginController::invoke(const Plugin& plugin, const PluginAction& action, const PluginContext& context, Panel* panel)
{
    if (!pluginActionAvailable(action, context)) { if (panel) panel->showMessage(tr("Plugin action does not support this selection/location."), true); return; }
    if (m_runs.size() >= 4) { if (panel) panel->showMessage(tr("Four plugins are already running. Wait or cancel one."), true); return; }
    auto* run = new PluginRun(plugin, action, pluginInput(plugin, action, context), this); m_runs.insert(run);
    auto* progress = new QProgressDialog(tr("%1 — %2").arg(plugin.name, action.label), tr("Cancel"), 0, 0, m_window);
    progress->setWindowModality(Qt::NonModal); progress->setAutoClose(false); progress->setMinimumDuration(0); progress->show();
    connect(progress, &QProgressDialog::canceled, run, &PluginRun::cancel);
    connect(run, &PluginRun::finished, this, [this, run, progress, action, name = plugin.name, panel = QPointer<Panel>(panel)] {
        m_runs.remove(run); progress->close(); progress->deleteLater();
        const auto output = run->text();
        if (!run->succeeded()) {
            if (panel) panel->showMessage(tr("%1: %2").arg(name, output.left(1024)), true);
            if (output.size() > 1024) showText(m_window, tr("%1 failed").arg(name), output);
        } else if (action.result == "list") {
            QString problem;
            if (auto list = parsePluginList(output.toUtf8(), &problem)) showList(m_window, *list);
            else if (panel) panel->showMessage(tr("%1: %2").arg(name, problem), true);
        } else {
            if (action.result == "copy" || action.result == "notify-and-copy") QApplication::clipboard()->setText(output);
            if (panel) panel->showMessage(action.result == "notify" || action.result == "notify-and-copy" ? output.left(1024) : tr("%1 completed.").arg(name));
            if ((action.result == "notify" || action.result == "notify-and-copy") && output.size() > 1024) showText(m_window, name, output);
        }
        run->deleteLater();
    });
    run->start();
}
} // namespace tandem
