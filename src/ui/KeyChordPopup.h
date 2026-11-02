#pragma once

#include <QFrame>
#include <QPointer>

class QGridLayout;
class QLabel;

namespace tandem {

// yazi-style which-key panel for bookmarks. Jump mode: type a key chain
// ("h", "cp"); the list narrows and the jump happens as soon as the chain is
// unique. Assign mode: type a chain for the given folder, Enter to save,
// Delete to remove the bookmark.
class KeyChordPopup : public QFrame {
    Q_OBJECT
public:
    explicit KeyChordPopup(QWidget* window);

    void openJump(const QString& initialKeys = {});
    void openAssign(const QString& path);
    QString typed() const { return m_typed; }

signals:
    void jumpRequested(const QString& path);
    void fuzzyRequested();
    void message(const QString& text, bool error);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    bool event(QEvent* e) override;

private:
    void type(const QString& keys);
    void close();  // hand focus back first, so the right panel stays active
    void refresh();
    void place();

    enum class Mode { Jump, Assign } m_mode = Mode::Jump;
    QString m_typed;
    QString m_assignPath;
    QPointer<QWidget> m_returnFocus;
    QLabel* m_title;
    QWidget* m_grid;
    QGridLayout* m_gridLayout;
    QLabel* m_hint;
};

}  // namespace tandem
