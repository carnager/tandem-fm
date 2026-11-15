#pragma once

class QString;
class QWidget;

namespace tandem {

// Opens `window` at the size it had when last closed, and keeps that size
// for next time (state.toml, "dialogs/<key>"). Call after setting the
// default size.
void rememberSize(QWidget* window, const QString& key);

}  // namespace tandem
