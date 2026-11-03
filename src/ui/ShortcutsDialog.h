#pragma once

#include <QDialog>

class QLineEdit;
class QGridLayout;
class QWidget;

namespace tandem {

// F1: every key, grouped, in columns, with a filter.
class ShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShortcutsDialog(QWidget* parent = nullptr);
    QLineEdit* filterEdit() const { return m_filter; }
    int visibleEntries() const { return m_visible; }

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void rebuild();

    QLineEdit* m_filter;
    QWidget* m_page;
    QGridLayout* m_grid = nullptr;
    int m_columns = 0;
    int m_visible = 0;
};

}  // namespace tandem
