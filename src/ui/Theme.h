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
    QFont listFont;     // file list
    QFont numberFont;   // tabular digits for sizes and dates

    QString css(const QString& tmpl) const;  // replaces @window, @accent, ...
};

const Theme& theme();
void applyTheme(QApplication& app);

}  // namespace tandem
