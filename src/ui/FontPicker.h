#pragma once

#include <QFrame>

#include <functional>

class QLineEdit;
class QListWidget;
class QLabel;
class QButtonGroup;

namespace tandem {

// A popup for choosing a font: type to search, every name in its own font,
// sizes as buttons, a sample line. What is under the cursor is tried out at
// once (changed); Esc goes back to what was there, anything else keeps it.
class FontPicker : public QFrame {
    Q_OBJECT
public:
    // family "": `inherit` (e.g. "Like the desktop"); size 0: its size.
    FontPicker(const QString& family, int size, bool fixedOnly, const QString& inherit, QWidget* parent);
    void popupBelow(QWidget* anchor);

signals:
    void changed(const QString& family, int size);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void filter(const QString& text);
    void tryOut();
    QString currentFamily() const;

    QLineEdit* m_search;
    QListWidget* m_list;
    QButtonGroup* m_sizes;
    QLabel* m_sample;
    QString m_origFamily;
    int m_origSize;
    bool m_reverted = false;
};

}  // namespace tandem
