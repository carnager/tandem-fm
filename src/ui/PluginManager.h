#pragma once
#include "core/Plugins.h"
#include <QWidget>
#include <QSet>
class QMenu;
class QTreeWidget;
class QPushButton;
namespace tandem {
class Panel;
PluginContext capturePluginContext(Panel* active, Panel* other);
class PluginManagerPage : public QWidget {
    Q_OBJECT
public:
    explicit PluginManagerPage(QWidget* parent = nullptr);
    void reload();
private:
    void editSettings();
    void install();
    void updateButtons();
    std::optional<Plugin> selected() const;
    QTreeWidget* m_table;
    QPushButton* m_settings;
    QList<FoundPlugin> m_plugins;
    bool m_loading = false;
};
class PluginController : public QObject {
    Q_OBJECT
public:
    explicit PluginController(QWidget* parent);
    void populateMenu(QMenu* menu, Panel* active, Panel* other, bool management = true);
    void showManager();
    void cancelAll();
    int runningCount() const { return m_runs.size(); }
    void invoke(const Plugin& plugin, const PluginAction& action, const PluginContext& context, Panel* panel);
private:
    QWidget* m_window;
    QSet<PluginRun*> m_runs;
};
} // namespace tandem
