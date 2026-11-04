#include "ui/DialogKeys.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

namespace tandem {

namespace {

// The letter after a single '&' ("&&" is a literal ampersand).
QChar mnemonicOf(const QString& text)
{
    for (int i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != QLatin1Char('&'))
            continue;
        if (text[i + 1] == QLatin1Char('&')) {
            ++i;
            continue;
        }
        return text[i + 1].toLower();
    }
    return {};
}

bool takesText(const QWidget* w)
{
    if (!w)
        return false;
    if (auto* e = qobject_cast<const QLineEdit*>(w))
        return !e->isReadOnly();
    if (auto* c = qobject_cast<const QComboBox*>(w))
        return c->isEditable();
    return qobject_cast<const QTextEdit*>(w) || qobject_cast<const QPlainTextEdit*>(w) ||
           qobject_cast<const QAbstractSpinBox*>(w);
}

class DialogKeys : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (event->type() != QEvent::KeyPress || !obj->isWidgetType())
            return false;
        auto* w = static_cast<QWidget*>(obj);
        // A key is delivered to the focus widget, then its parents: act once.
        QWidget* focus = QApplication::focusWidget();
        if (focus ? w != focus : !qobject_cast<QDialog*>(w))
            return false;
        auto* dialog = qobject_cast<QDialog*>(w->window());
        if (!dialog)
            return false;
        auto* e = static_cast<QKeyEvent*>(event);
        const auto mods = e->modifiers() & ~Qt::KeypadModifier;
        const bool option = mods == Qt::AltModifier;
        if (mods != Qt::NoModifier && !option)
            return false;
        if (!option && takesText(focus))
            return false;  // typing
        const int key = e->key();
        QChar letter;
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            letter = QChar(u'a' + (key - Qt::Key_A));
        else if (key >= Qt::Key_0 && key <= Qt::Key_9)
            letter = QChar(u'0' + (key - Qt::Key_0));
        else
            return false;
        QAbstractButton* b = mnemonicButton(dialog, letter);
        if (!b)
            return false;
        b->setFocus(Qt::ShortcutFocusReason);
        b->animateClick();
        return true;
    }
};

}  // namespace

QAbstractButton* mnemonicButton(QWidget* dialog, QChar letter)
{
    QAbstractButton* found = nullptr;
    letter = letter.toLower();
    for (QAbstractButton* b : dialog->findChildren<QAbstractButton*>()) {
        if (!b->isVisibleTo(dialog) || !b->isEnabled() || b->window() != dialog)
            continue;
        if (mnemonicOf(b->text()) != letter)
            continue;
        if (found)
            return nullptr;  // ambiguous
        found = b;
    }
    return found;
}

QObject* installDialogKeys(QObject* app)
{
    auto* filter = new DialogKeys(app);
    app->installEventFilter(filter);
    return filter;
}

}  // namespace tandem
