#include "ui/TextEditor.h"

#include "core/Util.h"
#include "ui/Theme.h"
#include "ui/DialogSize.h"

#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QStyle>
#include <QStringDecoder>
#include <QVBoxLayout>

#ifdef TANDEM_HAVE_KSYNTAXHIGHLIGHTING
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#endif

namespace tandem {

namespace {

QHash<QString, QPointer<TextEditor>>& openEditors()
{
    static QHash<QString, QPointer<TextEditor>> editors;
    return editors;
}

#ifdef TANDEM_HAVE_KSYNTAXHIGHLIGHTING
KSyntaxHighlighting::Repository& repository()
{
    static KSyntaxHighlighting::Repository repo;
    return repo;
}
#endif

constexpr char kBom[] = "\xEF\xBB\xBF";

}  // namespace

TextEditor* TextEditor::open(const QString& path, QWidget* parent)
{
    if (TextEditor* open = openEditors().value(path)) {
        open->raise();
        open->activateWindow();
        return open;
    }
    auto* e = new TextEditor(path, parent);
    openEditors().insert(path, e);
    e->show();
    return e;
}

TextEditor::TextEditor(const QString& path, QWidget* parent) : QWidget(parent, Qt::Window), m_path(path)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 700);
    rememberSize(this, QStringLiteral("textEditor"));
    setThemeStyle(this, QStringLiteral(
        "TextEditor { background: @window; }"
        "QPlainTextEdit { background: @base; color: @text; border: none; selection-background-color: @accent;"
        "  selection-color: @accentText; }"
        "QLineEdit#find { background: @base; color: @text; border: 1px solid @control; border-radius: 6px;"
        "  padding: 3px 8px; }"
        "QLabel#status { color: @dimText; }"
        "QLabel#note { color: @text; background: @header; padding: 6px 12px; }"
        "QLabel#note[error=\"true\"] { color: @error; }"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("note"));
    m_note->setWordWrap(true);
    m_note->hide();
    layout->addWidget(m_note);

    m_edit = new QPlainTextEdit(this);
    const QFont mono = monoFont();
    m_edit->setFont(mono);
    m_edit->setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(QLatin1Char(' ')) * 4);
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_edit->setReadOnly(true);  // until it is loaded
    m_edit->document()->setDocumentMargin(10);
    layout->addWidget(m_edit, 1);

    auto* bottom = new QWidget(this);
    auto* bl = new QHBoxLayout(bottom);
    bl->setContentsMargins(12, 6, 12, 6);
    bl->setSpacing(10);
    m_find = new QLineEdit(bottom);
    m_find->setObjectName(QStringLiteral("find"));
    m_find->setPlaceholderText(tr("Find (Enter: next, Shift+Enter: previous)"));
    m_find->setMinimumWidth(280);
    m_find->hide();
    bl->addWidget(m_find);
    m_status = new QLabel(bottom);
    m_status->setObjectName(QStringLiteral("status"));
    bl->addWidget(m_status, 1);
    layout->addWidget(bottom);

    connect(m_edit->document(), &QTextDocument::modificationChanged, this, &TextEditor::updateTitle);
    connect(m_find, &QLineEdit::returnPressed, this, [this] {
        find(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier);
    });

    auto key = [this](const QKeySequence& seq, auto fn) {
        auto* sc = new QShortcut(seq, this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };
    key(QKeySequence::Save, [this] { save(); });
    key(QKeySequence::Find, [this] { showFind(); });
    key(Qt::Key_F7, [this] { showFind(); });
    key(Qt::Key_F3, [this] { find(false); });
    key(QKeySequence(Qt::SHIFT | Qt::Key_F3), [this] { find(true); });
    key(QKeySequence::Close, [this] { close(); });
    key(Qt::Key_Escape, [this] {
        if (m_find->isVisible()) {
            m_find->hide();
            m_edit->setFocus();
        } else {
            close();
        }
    });

    updateTitle();
    load();
}

TextEditor::~TextEditor()
{
    if (openEditors().value(m_path) == this)
        openEditors().remove(m_path);
}

bool TextEditor::isModified() const
{
    return m_edit->document()->isModified();
}

void TextEditor::setNote(const QString& text, bool error)
{
    m_note->setText(text);
    m_note->setProperty("error", error);
    m_note->style()->unpolish(m_note);
    m_note->style()->polish(m_note);
    m_note->setVisible(!text.isEmpty());
}

void TextEditor::load()
{
    m_status->setText(tr("Loading…"));
    QPointer<TextEditor> self(this);
    const QString path = m_path;
    ioPool()->start([self, path] {
        QFile f(path);
        QString text, error, why;
        bool crlf = false, bom = false, readOnly = false;
        if (!f.open(QIODevice::ReadOnly)) {
            error = f.errorString();
        } else if (f.size() > kMaxSize) {
            readOnly = true;
            why = tr("Too big to edit here (over %1 MB): shown read-only.").arg(kMaxSize / (1024 * 1024));
            text = QString::fromUtf8(f.read(kMaxSize));
        } else {
            QByteArray data = f.readAll();
            bom = data.startsWith(kBom);
            if (bom)
                data.remove(0, 3);
            QStringDecoder utf8(QStringDecoder::Utf8);
            text = utf8.decode(data);
            if (utf8.hasError() || data.contains('\0')) {
                readOnly = true;
                why = tr("Not UTF-8 text: shown read-only, so saving can't change bytes it doesn't understand.");
                text = QString::fromLatin1(data);
            }
            crlf = data.contains("\r\n");
            if (crlf)
                text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        }
        if (!readOnly && error.isEmpty() && !QFileInfo(path).isWritable()) {
            readOnly = true;
            why = tr("You can't write to this file: shown read-only.");
        }
        postToUi([self, text, error, why, crlf, bom, readOnly] {
            if (!self)
                return;
            TextEditor* w = self.data();
            w->m_loading = false;
            if (!error.isEmpty()) {
                w->m_readOnly = true;
                w->setNote(tr("Cannot read: %1").arg(error), true);
                w->updateTitle();
                emit w->loaded();
                return;
            }
            w->m_crlf = crlf;
            w->m_bom = bom;
            w->m_readOnly = readOnly;
            w->setNote(why);
            w->m_edit->setPlainText(text);
            w->m_edit->document()->setModified(false);
            w->m_edit->setReadOnly(readOnly);
#ifdef TANDEM_HAVE_KSYNTAXHIGHLIGHTING
            const auto def = repository().definitionForFileName(w->m_path);
            if (def.isValid() && text.size() <= 1024 * 1024) {
                w->m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(w->m_edit->document());
                w->m_highlighter->setTheme(repository().defaultTheme(
                    theme().dark ? KSyntaxHighlighting::Repository::DarkTheme : KSyntaxHighlighting::Repository::LightTheme));
                w->m_highlighter->setDefinition(def);
            }
#endif
            w->m_edit->setFocus();
            w->updateTitle();
            emit w->loaded();
        });
    });
}

void TextEditor::updateTitle()
{
    const QString name = QFileInfo(m_path).fileName();
    QString title = name;
    if (isModified())
        title.prepend(QStringLiteral("● "));
    if (m_readOnly)
        title += QLatin1Char(' ') + tr("(read-only)");
    setWindowTitle(title + QStringLiteral(" — Tandem"));
    if (m_loading)
        return;
    QStringList parts = {m_path, QStringLiteral("UTF-8") + (m_bom ? QStringLiteral(" BOM") : QString()),
                         m_crlf ? QStringLiteral("CRLF") : QStringLiteral("LF")};
    QString keys = m_readOnly ? tr("Ctrl+F: find   Esc: close") : tr("Ctrl+S: save   Ctrl+F: find   Esc: close");
    m_status->setText(parts.join(QStringLiteral("   ·   ")) + QStringLiteral("        ") + keys);
}

void TextEditor::save()
{
    if (m_loading || m_readOnly || m_saving)
        return;
    QString text = m_edit->toPlainText();
    if (m_crlf)
        text.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    QByteArray data = text.toUtf8();
    if (m_bom)
        data.prepend(kBom);
    m_saving = true;
    const int revision = m_edit->document()->revision();
    QPointer<TextEditor> self(this);
    const QString path = m_path;
    ioPool()->start([self, path, data, revision] {
        // Into the file itself, past a symbolic link, with its permissions.
        const QFileInfo info(path);
        const QString target = info.isSymLink() ? info.canonicalFilePath() : path;
        const QFile::Permissions perms = QFile::permissions(target);
        QSaveFile out(target);
        QString error;
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit())
            error = out.errorString();
        else if (perms)
            QFile::setPermissions(target, perms);
        postToUi([self, error, revision] {
            if (!self)
                return;
            TextEditor* w = self.data();
            w->m_saving = false;
            if (!error.isEmpty()) {
                w->setNote(tr("Couldn't save: %1").arg(error), true);
                w->m_closeAfterSave = false;
                emit w->saved(false);
                return;
            }
            w->setNote({});
            if (w->m_edit->document()->revision() == revision)  // nothing typed meanwhile
                w->m_edit->document()->setModified(false);
            emit w->saved(true);
            if (w->m_closeAfterSave)
                w->close();
        });
    });
}

void TextEditor::closeEvent(QCloseEvent* e)
{
    if (!isModified() || m_closeAfterSave) {
        if (!m_saving) {
            e->accept();
            return;
        }
    }
    if (m_saving) {  // closes when the save is done
        m_closeAfterSave = true;
        e->ignore();
        return;
    }
    QMessageBox box(QMessageBox::Question, tr("Unsaved changes"),
                    tr("Save the changes to %1?").arg(QFileInfo(m_path).fileName()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
    case QMessageBox::Save:
        m_closeAfterSave = true;
        save();
        e->ignore();
        return;
    case QMessageBox::Discard:
        e->accept();
        return;
    default:
        e->ignore();
    }
}

void TextEditor::showFind()
{
    m_find->show();
    m_find->setFocus();
    m_find->selectAll();
}

void TextEditor::find(bool backward)
{
    const QString text = m_find->text();
    if (text.isEmpty())
        return;
    QTextDocument::FindFlags flags;
    if (backward)
        flags |= QTextDocument::FindBackward;
    if (m_edit->find(text, flags))
        return;
    // From the other end, once.
    QTextCursor c = m_edit->textCursor();
    c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
    m_edit->setTextCursor(c);
    if (!m_edit->find(text, flags))
        m_status->setText(tr("Not found: %1").arg(text));
}

}  // namespace tandem
