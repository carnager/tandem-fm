#pragma once

#include <QFrame>
#include <QPointer>

#include <functional>

class QGridLayout;
class QLabel;

namespace tandem {

// yazi-style which-key panel for bookmarks. Jump mode: type a key chain
// ("h", "cp"); the list narrows and the jump happens as soon as the chain is
// unique. Assign mode: type a chain for the given folder, Enter to save,
// Delete to remove the bookmark. Choices mode: a few numbered things
// (shelves), picked with their digit, Shift+digit or Enter.
class KeyChordPopup : public QFrame {
    Q_OBJECT
public:
    explicit KeyChordPopup(QWidget* window);

    struct Choice {
        int number = 0;     // its key, 1–9
        QString text;       // "Shelf 2 · 3 items: a.txt, b.txt"
        QString unavailable;  // when set: shown dim, picking it says this
    };
    struct Choices {
        QString title, hint;
        QList<Choice> items;
        int enterPicks = 0;  // the number Enter picks; 0: none
        std::function<void(int number, bool shift)> pick;  // runs after the popup closed
        std::function<void()> space;  // optional
    };

    void openJump(const QString& initialKeys = {});
    void openAssign(const QString& path);
    void openChoices(const Choices& choices);
    bool isChoosing() const { return isVisible() && m_mode == Mode::Choices; }

    // 1–9 from the digit row, also with Shift held (which types "!" or "§")
    // or on layouts whose digits need Shift; 0 for other keys.
    static int digitOf(const QKeyEvent* e);
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
    void refreshChoices();
    void pickChoice(int number, bool shift);

    enum class Mode { Jump, Assign, Choices } m_mode = Mode::Jump;
    Choices m_choices;
    QString m_typed;
    QString m_assignPath;
    QPointer<QWidget> m_returnFocus;
    QLabel* m_title;
    QWidget* m_grid;
    QGridLayout* m_gridLayout;
    QLabel* m_hint;
};

}  // namespace tandem
