#pragma once

#include "ui/FuzzyPopup.h"

#include <QFrame>
#include <QPointer>

#include <functional>

class QGridLayout;
class QLabel;
class QListView;

namespace tandem {

// Assign mode (Ctrl+Shift+D): type a key or chain for a folder, laid out
// like the Ctrl+D list; Enter saves, Tab takes a free key, Delete removes the
// bookmark. Choices mode: a few numbered things (shelves), picked with their
// digit, Shift+digit or Enter.
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

    void openAssign(const QString& path);
    void openChoices(const Choices& choices);
    bool isChoosing() const { return isVisible() && m_mode == Mode::Choices; }

    // 1–9 from the digit row, also with Shift held (which types "!" or "§")
    // or on layouts whose digits need Shift; 0 for other keys.
    static int digitOf(const QKeyEvent* e);
    QString typed() const { return m_typed; }

signals:
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

    enum class Mode { Assign, Choices } m_mode = Mode::Assign;
    Choices m_choices;
    QString m_typed;
    QString m_assignPath;
    QStringList m_free;  // keys the folder could have, best first (Tab takes one)
    QPointer<QWidget> m_returnFocus;
    QLabel* m_title;
    QLabel* m_field;      // Assign: the key typed so far, drawn like the palette's input
    QListView* m_list;  // Assign: the bookmarks whose keys it touches, as Ctrl+D shows them
    PopupModel* m_model;
    QList<PopupItem> m_rows;
    QWidget* m_grid;
    QGridLayout* m_gridLayout;
    QLabel* m_hint;
};

}  // namespace tandem
