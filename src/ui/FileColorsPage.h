#pragma once
#include "ui/FileColors.h"
#include <QWidget>
class QTreeWidget;
class QPushButton;
namespace tandem {
class FileColorsPage : public QWidget {
public:
    explicit FileColorsPage(QWidget* parent = nullptr);
private:
    void fill(int selected = -1);
    void editRule(bool add);
    void updateButtons();
    void save(int selected);
    QList<FileColorRule> m_rules;
    QTreeWidget* m_table;
    QPushButton *m_edit, *m_remove, *m_up, *m_down;
    bool m_loading = false;
};
} // namespace tandem
