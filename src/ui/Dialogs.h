#pragma once

#include "core/Job.h"

#include <QDialog>

#include <optional>

namespace tandem {

// F5/F6 target prompt. Returns an empty string when cancelled.
// With startNow given, Shift+Enter accepts and sets it: run right away
// instead of waiting behind other operations on the same drive.
QString askDestination(QWidget* parent, const QString& title, const QString& label, const QString& initial,
                       bool* startNow = nullptr);

// Alt+F5: where to pack, in which format, with which password.
struct PackChoice {
    QString target;    // full path with the format's extension
    QString password;  // empty: none
    bool startNow = false;
};
// base: the target without extension. Relative names are left to the caller.
std::optional<PackChoice> askPack(QWidget* parent, const QString& what, const QString& base);

// Conflict / error prompt for a running job. Finishes with done(int(Answer)).
class PromptDialog : public QDialog {
    Q_OBJECT
public:
    PromptDialog(const PromptRequest& req, QWidget* parent);
};

}  // namespace tandem
