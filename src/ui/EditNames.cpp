#include "ui/EditNames.h"

#include "core/DesktopApps.h"
#include "ui/MultiRenameDialog.h"

#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace tandem {

EditNamesSession::EditNamesSession(QList<RenameItem> items, QSet<QString> existing, QWidget* dialogParent)
    : m_items(std::move(items)), m_existing(std::move(existing)), m_dialogParent(dialogParent)
{
}

EditNamesSession::~EditNamesSession()
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->disconnect(this);
}

QStringList EditNamesSession::editorCommand(const QString& file, QString* error)
{
    const QString custom = QSettings().value(QStringLiteral("tools/nameEditor")).toString();
    if (!custom.isEmpty()) {
        QStringList argv = QProcess::splitCommand(custom);
        if (!argv.contains(QLatin1String("%f")))
            argv.append(QStringLiteral("%f"));
        argv.replaceInStrings(QStringLiteral("%f"), file);
        return argv;
    }
    QString editor = qEnvironmentVariable("VISUAL");
    if (editor.isEmpty())
        editor = qEnvironmentVariable("EDITOR");
    if (!editor.isEmpty()) {
        QString terminal = QSettings().value(QStringLiteral("tools/terminal")).toString();
        if (terminal.isEmpty())
            terminal = DesktopApps::instance().defaultTerminal();
        if (terminal.isEmpty()) {
            *error = QObject::tr("No terminal found for $EDITOR. Set tools/terminal or tools/nameEditor.");
            return {};
        }
        return DesktopApps::terminalCommand(terminal, QProcess::splitCommand(editor) << file);
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("kate")).isEmpty())
        return {QStringLiteral("kate"), QStringLiteral("-b"), file};
    *error = QObject::tr("No editor configured. Set $EDITOR, or tools/nameEditor to a command that waits.");
    return {};
}

void EditNamesSession::start()
{
    for (const RenameItem& it : std::as_const(m_items)) {
        if (it.name.contains(QLatin1Char('\n'))) {
            emit message(tr("“%1” contains a line break and can't be edited as a line").arg(it.name.simplified()),
                         true);
            finish();
            return;
        }
    }
    m_dir = std::make_unique<QTemporaryDir>();
    if (!m_dir->isValid()) {
        emit message(tr("Cannot create a temporary file"), true);
        finish();
        return;
    }
    m_file = m_dir->filePath(QStringLiteral("rename.txt"));
    QFile f(m_file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit message(tr("Cannot create a temporary file"), true);
        finish();
        return;
    }
    for (const RenameItem& it : std::as_const(m_items))
        f.write(it.name.toUtf8() + '\n');
    f.close();
    openEditor();
}

void EditNamesSession::openEditor()
{
    QString error;
    QStringList argv = editorCommand(m_file, &error);
    if (argv.isEmpty()) {
        emit message(error, true);
        finish();
        return;
    }
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::ForwardedChannels);
    connect(m_process, &QProcess::finished, this, &EditNamesSession::editorFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this, argv](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        emit message(tr("Could not start %1").arg(argv.first()), true);
        finish();
    });
    emit message(tr("Editing %n name(s) in %1 — save and quit to continue", nullptr, int(m_items.size()))
                     .arg(QFileInfo(argv.first()).fileName()),
                 false);
    m_clock.start();
    const QString program = argv.takeFirst();
    m_process->start(program, argv);
}

void EditNamesSession::editorFinished()
{
    m_process->deleteLater();
    m_process = nullptr;

    QFile f(m_file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit message(tr("The names file is gone"), true);
        finish();
        return;
    }
    QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();  // editors add a final newline
    for (QString& l : lines)
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);

    QWidget* parent = m_dialogParent.data();
    if (lines.size() != m_items.size()) {
        QMessageBox box(parent);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Rename from editor"));
        box.setText(tr("Expected %1 lines, found %2.").arg(m_items.size()).arg(lines.size()));
        box.setInformativeText(tr("Each line is one file, in the same order. Don't add or remove lines."));
        QPushButton* again = box.addButton(tr("Edit again"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == again)
            openEditor();  // the file still holds the user's edits
        else
            finish();
        return;
    }

    const RenamePlan plan = buildRenamePlanFromNames(m_items, lines, m_existing);
    if (plan.changes == 0 && plan.problems == 0) {
        // An editor that returns at once (GUI app forking away) looks the same.
        if (m_clock.elapsed() < 1500)
            emit message(tr("The editor returned immediately. Set tools/nameEditor to a command that waits "
                            "(e.g. “kate -b”)."),
                         true);
        else
            emit message(tr("No names changed"), false);
        finish();
        return;
    }
    RenameConfirmDialog dlg(plan, parent);
    const int result = dlg.exec();
    if (result == RenameConfirmDialog::EditAgain) {
        openEditor();
        return;
    }
    if (result == QDialog::Accepted)
        emit confirmed(plan);
    finish();
}

void EditNamesSession::finish()
{
    emit done();
}

}  // namespace tandem
