#include "ui/CommandBar.h"

#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QVBoxLayout>

namespace tandem {

QString shellQuote(const QString& s)
{
    QString out = s;
    out.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

QString expandCommand(const QString& command, const CommandContext& ctx)
{
    QString out;
    for (int i = 0; i < command.size(); ++i) {
        const QChar c = command[i];
        if (c != QLatin1Char('%') || i + 1 >= command.size()) {
            out += c;
            continue;
        }
        const QChar k = command[++i];
        if (k == QLatin1Char('s')) {
            QStringList quoted;
            for (const QString& p : ctx.selection)
                quoted << shellQuote(p);
            out += quoted.join(QLatin1Char(' '));
        } else if (k == QLatin1Char('f')) {
            out += shellQuote(ctx.current);
        } else if (k == QLatin1Char('n')) {
            out += shellQuote(ctx.current.section(QLatin1Char('/'), -1));
        } else if (k == QLatin1Char('d')) {
            out += shellQuote(ctx.dir);
        } else if (k == QLatin1Char('D')) {
            out += shellQuote(ctx.otherDir);
        } else if (k == QLatin1Char('%')) {
            out += QLatin1Char('%');
        } else {
            out += c;
            out += k;
        }
    }
    return out;
}

CommandBar::CommandBar(QWidget* window) : QFrame(window)
{
    const Theme& t = theme();
    setObjectName(QStringLiteral("commandBar"));
    setThemeStyle(this, QStringLiteral(
        "#commandBar { background: @header; border: 1px solid @border; border-radius: 12px; }"
        "QLineEdit { background: @base; color: @text; border: 1px solid @accent; border-radius: 8px; padding: 6px 10px;"
        "  font-family: monospace; }"
        "QLabel, QCheckBox { color: @dimText; }"));
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, t.dark ? 160 : 70));
    setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 8);
    layout->setSpacing(6);
    auto* row = new QHBoxLayout;
    auto* prompt = new QLabel(QStringLiteral("$"), this);
    QFont pf = prompt->font();
    pf.setWeight(QFont::Bold);
    prompt->setFont(pf);
    m_edit = new QLineEdit(this);
    m_edit->installEventFilter(this);
    m_terminal = new QCheckBox(tr("in terminal"), this);
    m_terminal->setFocusPolicy(Qt::NoFocus);
    m_terminal->setToolTip(tr("Run in a terminal window, for interactive commands (Ctrl+Enter)"));
    row->addWidget(prompt);
    row->addWidget(m_edit, 1);
    row->addWidget(m_terminal);
    layout->addLayout(row);
    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);
    hide();
}

void CommandBar::open(const QString& hint)
{
    if (!isVisible())
        m_returnFocus = QApplication::focusWidget();
    m_history = QSettings().value(QStringLiteral("commands/history")).toStringList();
    m_historyPos = -1;
    m_hint->setText(hint + QStringLiteral("\n") +
                    tr("%s marked files · %f file · %n name · %d this folder · %D other panel   ·   "
                       "Enter: run in background · Ctrl+Enter: in terminal · ↑↓ history"));
    m_edit->clear();
    place();
    show();
    raise();
    m_edit->setFocus();
}

void CommandBar::close()
{
    if (m_returnFocus)
        m_returnFocus->setFocus();
    hide();
}

void CommandBar::place()
{
    QWidget* w = parentWidget();
    const int width = qMin(900, w->width() - 40);
    setFixedWidth(width);
    adjustSize();
    move((w->width() - width) / 2, w->height() - height() - 64);
}

bool CommandBar::eventFilter(QObject* obj, QEvent* e)
{
    if (obj != m_edit)
        return false;
    if (e->type() == QEvent::ShortcutOverride) {
        e->accept();  // every key belongs to the bar while it's open
        return true;
    }
    if (e->type() == QEvent::FocusOut) {
        hide();
        return false;
    }
    if (e->type() != QEvent::KeyPress)
        return false;
    auto* ke = static_cast<QKeyEvent*>(e);
    switch (ke->key()) {
    case Qt::Key_Escape:
        close();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        const QString cmd = m_edit->text().trimmed();
        if (cmd.isEmpty())
            return true;
        m_history.removeAll(cmd);
        m_history.prepend(cmd);
        while (m_history.size() > 50)
            m_history.removeLast();
        QSettings().setValue(QStringLiteral("commands/history"), m_history);
        const bool terminal = m_terminal->isChecked() || (ke->modifiers() & Qt::ControlModifier);
        close();
        emit runRequested(cmd, terminal);
        return true;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (m_history.isEmpty())
            return true;
        if (m_historyPos < 0)
            m_draft = m_edit->text();
        m_historyPos += ke->key() == Qt::Key_Up ? 1 : -1;
        m_historyPos = qBound(-1, m_historyPos, int(m_history.size()) - 1);
        m_edit->setText(m_historyPos < 0 ? m_draft : m_history[m_historyPos]);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace tandem
