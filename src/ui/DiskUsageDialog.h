#pragma once

#include "core/DiskUsage.h"

#include <QDialog>

#include <atomic>
#include <memory>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace tandem {

// Ctrl+Shift+U: what takes the space, biggest first, with bars; Enter goes
// in, Backspace out, Del trashes (sizes update).
class DiskUsageDialog : public QDialog {
    Q_OBJECT
public:
    DiskUsageDialog(QString root, bool remote, QWidget* parent = nullptr);
    ~DiskUsageDialog() override;

    bool isCounting() const { return m_counting; }
    QTreeWidget* list() const { return m_list; }
    UsageNode* current() const { return m_current; }
    void enter(UsageNode* folder);
    void up();
    void deleteSelected(bool confirm = true);

signals:
    void showRequested(const QString& path);
    void deleteRequested(const QStringList& paths, bool toTrash);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void fill();
    UsageNode* selectedNode() const;

    QString m_rootPath;
    bool m_remote;
    std::unique_ptr<UsageNode> m_root;
    UsageNode* m_current = nullptr;
    QHBoxLayout* m_crumbs;
    QLabel* m_summary;
    QTreeWidget* m_list;
    QPushButton* m_show;
    QPushButton* m_delete;
    bool m_counting = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

}  // namespace tandem
