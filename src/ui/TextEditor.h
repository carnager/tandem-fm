#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace KSyntaxHighlighting {
class SyntaxHighlighter;
}

namespace tandem {

// Tandem's own text editor (F4 when chosen in Settings › Tools): the file in
// an editable view, highlighted like the viewer, saved with Ctrl+S. UTF-8
// only; other files, and very big ones, open read-only. Line endings, a
// byte-order mark and the file's permissions are kept. Reading and saving
// happen on worker threads.
class TextEditor : public QWidget {
    Q_OBJECT
public:
    // The window for path: the one already open, else a new one.
    static TextEditor* open(const QString& path, QWidget* parent);
    ~TextEditor() override;

    QString path() const { return m_path; }
    QPlainTextEdit* editor() const { return m_edit; }
    bool isReadOnly() const { return m_readOnly; }
    bool isModified() const;
    void save();

    static constexpr qint64 kMaxSize = 20 * 1024 * 1024;

signals:
    void loaded();
    void saved(bool ok);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    TextEditor(const QString& path, QWidget* parent);
    void load();
    void updateTitle();
    void showFind();
    void find(bool backward);
    void setNote(const QString& text, bool error = false);

    QString m_path;
    QPlainTextEdit* m_edit;
    QLineEdit* m_find;
    QLabel* m_status;
    QLabel* m_note;
    KSyntaxHighlighting::SyntaxHighlighter* m_highlighter = nullptr;
    bool m_loading = true;
    bool m_readOnly = false;
    bool m_crlf = false;
    bool m_bom = false;
    bool m_saving = false;
    bool m_closeAfterSave = false;
};

}  // namespace tandem
