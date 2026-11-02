#pragma once

#include <QFrame>
#include <QPointer>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace tandem {

// Expands %s %f %n %d %D in a command line; every substituted value is
// shell-quoted. Without any placeholder the command runs as typed.
struct CommandContext {
    QStringList selection;  // %s
    QString current;        // %f
    QString dir;            // %d
    QString otherDir;       // %D
};
QString expandCommand(const QString& command, const CommandContext& ctx);
QString shellQuote(const QString& s);

// Bottom bar for running a shell command on the selection (yazi's ";").
class CommandBar : public QFrame {
    Q_OBJECT
public:
    explicit CommandBar(QWidget* window);
    void open(const QString& hint);

signals:
    void runRequested(const QString& command, bool inTerminal);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void close();
    void place();

    QLineEdit* m_edit;
    QCheckBox* m_terminal;
    QLabel* m_hint;
    QStringList m_history;
    int m_historyPos = -1;
    QString m_draft;
    QPointer<QWidget> m_returnFocus;
};

}  // namespace tandem
