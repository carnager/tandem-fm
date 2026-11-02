#pragma once

#include <QColor>
#include <QFont>

class QApplication;

namespace tandem {

struct Theme {
    bool dark = true;
    QColor window, base, altBase, text, dimText, accent, accentText;
    QColor mark, markBg, cursorActive, cursorInactive, dropTarget;
    QColor border, header, headerActive, error, success;
    QColor gitModified, gitAdded, gitUntracked;
    QFont listFont;     // file list
    QFont numberFont;   // tabular digits for sizes and dates

    QString css(const QString& tmpl) const;  // replaces @window, @accent, ...
};

const Theme& theme();
// The configured fixed-width font, guaranteed to really be monospaced.
QFont monoFont();
void applyTheme(QApplication& app);

}  // namespace tandem
