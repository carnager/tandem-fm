#pragma once

#include <QDialog>

namespace tandem {

// Help > About Tandem: the mascot, the version, the license and where the code lives.
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};

}  // namespace tandem
