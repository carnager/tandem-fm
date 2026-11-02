#pragma once

#include "core/Job.h"

#include <QDialog>

namespace tandem {

// F5/F6 target prompt. Returns an empty string when cancelled.
QString askDestination(QWidget* parent, const QString& title, const QString& label, const QString& initial);

// Conflict / error prompt for a running job. Finishes with done(int(Answer)).
class PromptDialog : public QDialog {
    Q_OBJECT
public:
    PromptDialog(const PromptRequest& req, QWidget* parent);
};

}  // namespace tandem
