#pragma once

#include <QWidget>

class QHBoxLayout;
class QLineEdit;
class QStackedLayout;

namespace tandem {

// The panel's location: clickable path segments ("~ › Code › tandem-fm"),
// which turn into a text field on click in empty space or Ctrl+L. Leading
// segments collapse into a "…" menu when the path doesn't fit.
class PathBar : public QWidget {
    Q_OBJECT
public:
    explicit PathBar(QWidget* parent = nullptr);

    void setPath(const QString& path);
    QString path() const { return m_path; }
    void startEditing();
    bool isEditing() const;

signals:
    void pathEntered(const QString& text);
    void segmentClicked(const QString& path);
    void segmentMiddleClicked(const QString& path);
    void focused();

protected:
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void scheduleRebuild();
    void rebuild();
    void stopEditing();

    bool m_rebuildPending = false;

    QString m_path;
    QStackedLayout* m_stack;
    QWidget* m_crumbs;
    QHBoxLayout* m_crumbLayout;
    QLineEdit* m_edit;
};

}  // namespace tandem
