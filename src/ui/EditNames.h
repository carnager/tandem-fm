#pragma once

#include "core/MultiRename.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <memory>

class QProcess;
class QTemporaryDir;
class QWidget;

namespace tandem {

// vidir-style renaming: names go into a temp file, one per line, the user's
// editor opens it, and the changed lines become a rename plan that is shown
// for confirmation. Lines must not be added or removed.
//
// Editor: config tools/nameEditor if set (a command that waits until the file
// is closed, e.g. "kate -b"), else $VISUAL / $EDITOR in the terminal, else Kate.
class EditNamesSession : public QObject {
    Q_OBJECT
public:
    EditNamesSession(QList<RenameItem> items, QSet<QString> existing, QWidget* dialogParent);
    ~EditNamesSession() override;

    void start();
    static QStringList editorCommand(const QString& file, QString* error);

signals:
    void confirmed(const RenamePlan& plan);
    void message(const QString& text, bool error);
    void done();  // the session is over either way; safe to deleteLater

private:
    void openEditor();
    void editorFinished();
    void finish();

    QList<RenameItem> m_items;
    QSet<QString> m_existing;
    QPointer<QWidget> m_dialogParent;
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_file;
    QProcess* m_process = nullptr;
    QElapsedTimer m_clock;
};

}  // namespace tandem
