#pragma once

#include <QChar>

class QAbstractButton;
class QObject;
class QWidget;

namespace tandem {

// macOS ignores the underlined letters on buttons (&Skip): Qt only honours
// them elsewhere. This makes them work in dialogs there too: the letter
// alone presses the button, or Option+letter while typing in a field.
// Returns the filter (deleting it uninstalls it).
QObject* installDialogKeys(QObject* app);

// The button in `dialog` whose underlined letter is `letter`; nullptr when
// none or several have it.
QAbstractButton* mnemonicButton(QWidget* dialog, QChar letter);

}  // namespace tandem
