#include "ui/viewer/ViewerWindow.h"

#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"
#include "ui/viewer/BlockCache.h"
#include "ui/viewer/ByteViews.h"
#include "ui/viewer/ImageView.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef TANDEM_HAVE_KSYNTAXHIGHLIGHTING
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

namespace {

struct Head {
    int error = 0;
    qint64 size = 0;
    QByteArray bytes;  // first 64 KiB
    QString mimeName;
    QString mimeComment;
    bool isDir = false;
    bool isSpecial = false;
};

Head readHead(const QString& path)
{
    Head h;
    const QByteArray np = toNative(path);
    // O_NONBLOCK: opening a FIFO for reading would otherwise wait for a writer.
    const int fd = ::open(np.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        h.error = errno;
        return h;
    }
    struct stat st;
    if (::fstat(fd, &st) == 0) {
        h.size = st.st_size;
        h.isDir = S_ISDIR(st.st_mode);
        h.isSpecial = !S_ISREG(st.st_mode) && !h.isDir;
    }
    if (h.isSpecial) {
        ::close(fd);
        return h;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    if (!h.isDir) {
        h.bytes.resize(64 * 1024);
        const ssize_t n = ::read(fd, h.bytes.data(), size_t(h.bytes.size()));
        h.bytes.resize(n > 0 ? n : 0);
    }
    ::close(fd);
    QMimeDatabase db;
    const QMimeType mt = db.mimeTypeForFileNameAndData(path, h.bytes);
    h.mimeName = mt.name();
    h.mimeComment = mt.comment();
    return h;
}

bool looksBinary(const QByteArray& head)
{
    // NUL bytes are the classic tell; UTF-16 text is rare enough to ignore.
    return head.contains('\0');
}

#ifdef TANDEM_HAVE_KSYNTAXHIGHLIGHTING
KSyntaxHighlighting::Repository& repository()
{
    static KSyntaxHighlighting::Repository repo;
    return repo;
}
#endif

}  // namespace

ViewerWindow::ViewerWindow(const QStringList& files, int index, QWidget* parent, bool embedded)
    : QWidget(parent, embedded ? Qt::Widget : Qt::Window), m_embedded(embedded), m_files(files)
{
    if (!embedded)
        setAttribute(Qt::WA_DeleteOnClose);
    const Theme& t = theme();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* top = new QWidget(this);
    top->setObjectName(QStringLiteral("viewerTop"));
    top->setAttribute(Qt::WA_StyledBackground);
    top->setStyleSheet(t.css(QStringLiteral(
        "#viewerTop { background: @header; border-bottom: 1px solid @border; }"
        "QToolButton { color: @dimText; border: none; padding: 4px 10px; border-radius: 6px; }"
        "QToolButton:checked { color: @text; background: @headerActive; }"
        "QToolButton:hover { color: @text; }")));
    auto* tl = new QHBoxLayout(top);
    tl->setContentsMargins(10, 4, 10, 4);
    tl->setSpacing(4);
    m_title = new QLabel(top);
    QFont bold = m_title->font();
    bold.setWeight(QFont::DemiBold);
    m_title->setFont(bold);
    m_title->setTextFormat(Qt::PlainText);
    tl->addWidget(m_title, 1);

    auto makeButton = [&](const QString& text, const QString& tip) {
        auto* b = new QToolButton(top);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        tl->addWidget(b);
        return b;
    };
    m_textButton = makeButton(tr("Text"), tr("Text (1)"));
    m_hexButton = makeButton(tr("Hex"), tr("Hex (3)"));
    m_imageButton = makeButton(tr("Image"), tr("Image (4)"));
    tl->addSpacing(8);
    m_wrapButton = makeButton(tr("Wrap"), tr("Wrap long lines (W)"));
    connect(m_textButton, &QToolButton::clicked, this, [this] { setMode(Mode::Text); });
    connect(m_hexButton, &QToolButton::clicked, this, [this] { setMode(Mode::Hex); });
    connect(m_imageButton, &QToolButton::clicked, this, [this] { setMode(Mode::Image); });
    connect(m_wrapButton, &QToolButton::toggled, this, [this](bool on) {
        m_text->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        m_big->setWrap(on);
        QSettings().setValue(QStringLiteral("viewer/wrap"), on);
    });
    layout->addWidget(top);

    m_stack = new QStackedWidget(this);
    const QFont mono = monoFont();
    m_text = new QPlainTextEdit(m_stack);
    m_text->setReadOnly(true);
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->setFont(mono);
    m_text->setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(QLatin1Char(' ')) * 8);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_big = new TextScrollView(m_stack);
    m_hex = new HexView(m_stack);
    m_image = new ImageView(m_stack);
    m_message = new QLabel(m_stack);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setStyleSheet(t.css(QStringLiteral("color: @dimText;")));
    for (QWidget* w : {static_cast<QWidget*>(m_text), static_cast<QWidget*>(m_big), static_cast<QWidget*>(m_hex),
                       static_cast<QWidget*>(m_image), static_cast<QWidget*>(m_message)})
        m_stack->addWidget(w);
    layout->addWidget(m_stack, 1);

    m_searchBar = new QWidget(this);
    m_searchBar->setObjectName(QStringLiteral("searchBar"));
    m_searchBar->setAttribute(Qt::WA_StyledBackground);
    m_searchBar->setStyleSheet(t.css(QStringLiteral(
        "#searchBar { background: @header; border-top: 1px solid @border; }"
        "QLineEdit { background: @base; color: @text; border: 1px solid @border; border-radius: 6px; padding: 3px 8px; }"
        "QLineEdit:focus { border-color: @accent; }"
        "QLabel { color: @dimText; }")));
    auto* sl = new QHBoxLayout(m_searchBar);
    sl->setContentsMargins(10, 5, 10, 5);
    m_searchEdit = new QLineEdit(m_searchBar);
    m_searchEdit->setPlaceholderText(tr("Find (Enter / F3: next, Shift+F3: previous, Esc: close)"));
    m_searchInfo = new QLabel(m_searchBar);
    sl->addWidget(m_searchEdit, 1);
    sl->addWidget(m_searchInfo);
    m_searchBar->hide();
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] { find(false); });
    connect(m_searchEdit, &QLineEdit::textEdited, this, [this] { m_matchStart = -1; });
    layout->addWidget(m_searchBar);

    m_status = new QLabel(this);
    m_status->setContentsMargins(10, 3, 10, 4);
    m_status->setStyleSheet(t.css(QStringLiteral("color: @dimText; background: @window;")));
    layout->addWidget(m_status);

    connect(m_big, &ByteViewBase::positionChanged, this, &ViewerWindow::updateStatus);
    connect(m_hex, &ByteViewBase::positionChanged, this, &ViewerWindow::updateStatus);
    connect(m_image, &ImageView::scaleChanged, this, &ViewerWindow::updateStatus);

    QSettings s;
    m_wrapButton->setChecked(s.value(QStringLiteral("viewer/wrap"), false).toBool());
    m_text->setLineWrapMode(m_wrapButton->isChecked() ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    m_big->setWrap(m_wrapButton->isChecked());
    if (!embedded) {
        if (!restoreGeometry(s.value(QStringLiteral("viewer/geometry")).toByteArray()))
            resize(1000, 720);
        setupShortcuts();
    }
    openIndex(index);
}

ViewerWindow::~ViewerWindow()
{
    if (m_searchCancel)
        m_searchCancel->store(true);
}

void ViewerWindow::closeEvent(QCloseEvent* e)
{
    if (!m_embedded)
        QSettings().setValue(QStringLiteral("viewer/geometry"), saveGeometry());
    QWidget::closeEvent(e);
}

void ViewerWindow::setupShortcuts()
{
    auto add = [this](const QKeySequence& key, auto fn) {
        auto* sc = new QShortcut(key, this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };
    add(Qt::Key_Escape, [this] {
        if (m_searchBar->isVisible()) {
            m_searchBar->hide();
            m_stack->currentWidget()->setFocus();
        } else {
            close();
        }
    });
    add(Qt::Key_Q, [this] { close(); });
    add(Qt::Key_N, [this] { openIndex(m_index + 1); });
    add(Qt::Key_P, [this] { openIndex(m_index - 1); });
    add(Qt::Key_1, [this] { setMode(Mode::Text); });
    add(Qt::Key_3, [this] { setMode(Mode::Hex); });
    add(Qt::Key_4, [this] { setMode(Mode::Image); });
    add(Qt::Key_W, [this] { m_wrapButton->toggle(); });
    add(Qt::Key_F7, [this] { showSearch(); });
    add(QKeySequence::Find, [this] { showSearch(); });
    add(Qt::Key_F3, [this] { find(false); });
    add(QKeySequence(Qt::SHIFT | Qt::Key_F3), [this] { find(true); });
    add(Qt::Key_Plus, [this] { zoom(1); });
    add(Qt::Key_Equal, [this] { zoom(1); });
    add(Qt::Key_Minus, [this] { zoom(-1); });
    add(Qt::Key_0, [this] { zoom(0); });
    if (QSettings().value(QStringLiteral("input/vimKeys"), false).toBool()) {
        auto sendKey = [this](Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QWidget* w = m_stack->currentWidget();
            QKeyEvent press(QEvent::KeyPress, key, mods);
            QCoreApplication::sendEvent(w, &press);
        };
        add(Qt::Key_J, [sendKey] { sendKey(Qt::Key_Down); });
        add(Qt::Key_K, [sendKey] { sendKey(Qt::Key_Up); });
        add(QKeySequence(Qt::SHIFT | Qt::Key_G), [sendKey] { sendKey(Qt::Key_End, Qt::ControlModifier); });
        add(QKeySequence(Qt::Key_G, Qt::Key_G), [sendKey] { sendKey(Qt::Key_Home, Qt::ControlModifier); });
        add(Qt::Key_Slash, [this] { showSearch(); });
    }
}

void ViewerWindow::showFile(const QString& path)
{
    m_files = {path};
    openIndex(0);
}

void ViewerWindow::showNote(const QString& text)
{
    ++m_gen;
    m_index = -1;
    m_path.clear();
    m_loading = false;
    m_mode = Mode::None;
    m_big->setCache(nullptr);
    m_hex->setCache(nullptr);
    delete m_cache;
    m_cache = nullptr;
    m_title->clear();
    m_status->clear();
    for (QToolButton* b : {m_textButton, m_hexButton, m_imageButton, m_wrapButton})
        b->hide();
    showMessage(text);
}

// ------------------------------------------------------------------ loading

void ViewerWindow::openIndex(int index)
{
    if (index < 0 || index >= m_files.size())
        return;
    m_index = index;
    m_path = m_files[index];
    ++m_gen;
    if (m_searchCancel)
        m_searchCancel->store(true);
    m_textLoaded = m_imageLoaded = false;
    m_matchStart = -1;
    m_mode = Mode::None;
    m_loading = true;
    delete m_highlighter;
    m_highlighter = nullptr;
    m_text->clear();
    m_image->setImage({});
    m_big->setCache(nullptr);
    m_hex->setCache(nullptr);
    delete m_cache;
    m_cache = nullptr;

    setWindowTitle(QStringLiteral("%1 — Tandem").arg(baseName(m_path)));
    m_title->setText(m_files.size() == 1 ? m_path
                                         : QStringLiteral("%1   (%2/%3)").arg(m_path).arg(index + 1).arg(m_files.size()));
    m_textButton->show();
    m_hexButton->show();
    showMessage(tr("Loading…"));

    const quint64 gen = m_gen;
    const QString path = m_path;
    QPointer<ViewerWindow> self(this);
    ioPool()->start([self, gen, path] {
        auto head = std::make_shared<Head>(readHead(path));
        postToUi([self, gen, head] {
            if (!self || gen != self->m_gen)
                return;
            ViewerWindow* w = self.data();
            if (head->error) {
                w->m_loading = false;
                w->showMessage(tr("Cannot open: %1").arg(errnoString(head->error)));
                emit w->loaded();
                return;
            }
            if (head->isDir || head->isSpecial) {
                w->m_loading = false;
                w->showMessage(head->isDir ? tr("This is a folder.") : tr("This is a device, pipe or socket."));
                emit w->loaded();
                return;
            }
            w->m_size = head->size;
            w->m_mimeComment = head->mimeComment;
            w->m_isImage = QImageReader::supportedMimeTypes().contains(head->mimeName.toLatin1());
            w->m_binary = looksBinary(head->bytes);
            w->m_cache = new BlockCache(w->m_path, w->m_size, w);
            connect(w->m_cache, &BlockCache::failed, w,
                    [w](const QString& err) { w->showMessage(tr("Read error: %1").arg(err)); });
            w->setMode(w->m_isImage ? Mode::Image : (w->m_binary ? Mode::Hex : Mode::Text));
        });
    });
}

void ViewerWindow::setMode(Mode mode)
{
    if (!m_cache || (mode == Mode::Image && !m_isImage))
        return;
    m_mode = mode;
    updateModeButtons();
    switch (mode) {
    case Mode::Text:
        if (m_size > kSmallText) {
            m_big->setCache(m_cache);
            m_stack->setCurrentWidget(m_big);
            if (!m_embedded)
                m_big->setFocus();
            m_encoding = QStringLiteral("UTF-8");
            m_loading = false;
            emit loaded();
        } else if (m_textLoaded) {
            m_stack->setCurrentWidget(m_text);
            if (!m_embedded)
                m_text->setFocus();
        } else {
            loadText();
        }
        break;
    case Mode::Hex:
        m_hex->setCache(m_cache);
        m_stack->setCurrentWidget(m_hex);
        if (!m_embedded)
            m_hex->setFocus();
        m_loading = false;
        emit loaded();
        break;
    case Mode::Image:
        if (m_imageLoaded) {
            m_stack->setCurrentWidget(m_image);
            if (!m_embedded)
                m_image->setFocus();
        } else {
            loadImage();
        }
        break;
    case Mode::None:
        break;
    }
    updateStatus();
}

void ViewerWindow::loadText()
{
    showMessage(tr("Loading…"));
    m_loading = true;
    const quint64 gen = m_gen;
    const QString path = m_path;
    QPointer<ViewerWindow> self(this);
    ioPool()->start([self, gen, path] {
        QFile f(path);
        QString text, encoding = QStringLiteral("UTF-8");
        QString error;
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray data = f.read(kSmallText + 1);
            QStringDecoder utf8(QStringDecoder::Utf8);
            text = utf8.decode(data);
            if (utf8.hasError()) {
                text = QString::fromLatin1(data);
                encoding = QStringLiteral("Latin-1");
            }
            // Keep NULs and other control bytes from swallowing the rest of a line.
            for (QChar& ch : text)
                if (ch.unicode() < 0x20 && ch != QLatin1Char('\n') && ch != QLatin1Char('\t') && ch != QLatin1Char('\r'))
                    ch = QChar(0x00B7);
        } else {
            error = f.errorString();
        }
        postToUi([self, gen, text, encoding, error] {
            if (!self || gen != self->m_gen)
                return;
            ViewerWindow* w = self.data();
            w->m_loading = false;
            if (!error.isEmpty()) {
                w->showMessage(tr("Cannot read: %1").arg(error));
                emit w->loaded();
                return;
            }
            w->m_encoding = encoding;
            w->m_text->setPlainText(text);
#ifdef TANDEM_HAVE_KSYNTAXHIGHLIGHTING
            if (w->m_size <= kHighlightMax) {
                const auto def = repository().definitionForFileName(w->m_path);
                if (def.isValid()) {
                    w->m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(w->m_text->document());
                    w->m_highlighter->setTheme(repository().defaultTheme(
                        theme().dark ? KSyntaxHighlighting::Repository::DarkTheme
                                     : KSyntaxHighlighting::Repository::LightTheme));
                    w->m_highlighter->setDefinition(def);
                }
            }
#endif
            w->m_textLoaded = true;
            if (w->m_mode == Mode::Text) {
                w->m_stack->setCurrentWidget(w->m_text);
                if (!w->m_embedded)
                    w->m_text->setFocus();
            }
            w->updateStatus();
            emit w->loaded();
        });
    });
}

void ViewerWindow::loadImage()
{
    showMessage(tr("Loading…"));
    m_loading = true;
    const quint64 gen = m_gen;
    const QString path = m_path;
    QPointer<ViewerWindow> self(this);
    ioPool()->start([self, gen, path] {
        QImageReader reader(path);
        reader.setAutoTransform(true);  // EXIF rotation
        reader.setAllocationLimit(1024);  // MiB
        const QImage img = reader.read();
        const QString error = img.isNull() ? reader.errorString() : QString();
        postToUi([self, gen, img, error] {
            if (!self || gen != self->m_gen)
                return;
            ViewerWindow* w = self.data();
            w->m_loading = false;
            if (img.isNull()) {
                w->showMessage(tr("Cannot show image: %1").arg(error));
                emit w->loaded();
                return;
            }
            w->m_imageLoaded = true;
            w->m_image->setImage(img);
            if (w->m_mode == Mode::Image) {
                w->m_stack->setCurrentWidget(w->m_image);
                if (!w->m_embedded)
                    w->m_image->setFocus();
            }
            w->updateStatus();
            emit w->loaded();
        });
    });
}

void ViewerWindow::showMessage(const QString& text)
{
    m_message->setText(text);
    m_stack->setCurrentWidget(m_message);
}

bool ViewerWindow::usesLargeTextView() const
{
    return m_stack->currentWidget() == m_big;
}

// ------------------------------------------------------------ presentation

void ViewerWindow::updateModeButtons()
{
    m_textButton->setChecked(m_mode == Mode::Text);
    m_hexButton->setChecked(m_mode == Mode::Hex);
    m_imageButton->setChecked(m_mode == Mode::Image);
    m_imageButton->setVisible(m_isImage);
    m_wrapButton->setVisible(m_mode == Mode::Text);
}

void ViewerWindow::updateStatus()
{
    QStringList parts;
    parts << formatSize(m_size);
    if (!m_mimeComment.isEmpty())
        parts << m_mimeComment;
    switch (m_mode) {
    case Mode::Text:
        parts << m_encoding;
        if (usesLargeTextView())
            parts << tr("%1%").arg(int(m_big->position() * 100));
        else if (m_highlighter)
            parts << tr("highlighted");
        break;
    case Mode::Hex:
        parts << tr("offset %1").arg(m_hex->topOffset(), 0, 16) << tr("%1%").arg(int(m_hex->position() * 100));
        break;
    case Mode::Image:
        if (!m_image->image().isNull())
            parts << QStringLiteral("%1 × %2").arg(m_image->image().width()).arg(m_image->image().height())
                  << tr("%1%").arg(qRound(m_image->scale() * 100));
        break;
    case Mode::None:
        break;
    }
    m_status->setText(parts.join(QStringLiteral("   ·   ")) +
                      (m_embedded ? QString()
                                  : tr("        N / P: next / previous file   1 / 3 / 4: text / hex / image   F7: find")));
}

void ViewerWindow::zoom(int steps)
{
    switch (m_mode) {
    case Mode::Image:
        m_image->zoom(steps);
        break;
    case Mode::Hex:
        m_hex->zoom(steps);
        break;
    case Mode::Text:
        if (usesLargeTextView()) {
            m_big->zoom(steps);
        } else {
            QFont f = m_text->font();
            const qreal base = monoFont().pointSizeF();
            f.setPointSizeF(steps == 0 ? base : qBound<qreal>(5, f.pointSizeF() + steps, 48));
            m_text->setFont(f);
        }
        break;
    case Mode::None:
        break;
    }
}

// ------------------------------------------------------------------ search

void ViewerWindow::showSearch()
{
    m_searchBar->show();
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
}

void ViewerWindow::find(bool backward)
{
    const QString needle = m_searchEdit->text();
    if (needle.isEmpty()) {
        showSearch();
        return;
    }
    m_searchInfo->clear();
    if (m_mode == Mode::Text && !usesLargeTextView()) {
        QTextDocument::FindFlags flags;
        if (backward)
            flags |= QTextDocument::FindBackward;
        bool found = m_text->find(needle, flags);
        if (!found) {  // wrap around
            QTextCursor c = m_text->textCursor();
            c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
            m_text->setTextCursor(c);
            found = m_text->find(needle, flags);
            if (found)
                m_searchInfo->setText(tr("wrapped"));
        }
        if (!found)
            m_searchInfo->setText(tr("not found"));
        emit searchFinished(found);
        return;
    }
    if (m_mode == Mode::Text || m_mode == Mode::Hex)
        findInBytes(backward);
}

// Case-insensitive (ASCII) byte search through the whole file, off-thread.
void ViewerWindow::findInBytes(bool backward)
{
    if (m_searchCancel)
        m_searchCancel->store(true);
    m_searchCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_searchCancel;
    const QByteArray needle = m_searchEdit->text().toUtf8().toLower();
    ByteViewBase* view = m_mode == Mode::Hex ? static_cast<ByteViewBase*>(m_hex) : m_big;
    const qint64 from = m_matchStart >= 0 ? (backward ? m_matchStart - 1 : m_matchStart + 1)
                                          : (backward ? view->topOffset() - 1 : view->topOffset());
    const QString path = m_path;
    const qint64 size = m_size;
    const quint64 gen = m_gen;
    m_searchInfo->setText(tr("searching…"));
    QPointer<ViewerWindow> self(this);
    ioPool()->start([self, cancel, needle, from, backward, path, size, gen] {
        constexpr qint64 kChunk = 4 * 1024 * 1024;
        const int fd = ::open(toNative(path).constData(), O_RDONLY | O_CLOEXEC);
        qint64 found = -1;
        bool wrapped = false;
        if (fd >= 0 && !needle.isEmpty()) {
            QByteArray buf;
            auto scan = [&](qint64 a, qint64 b) -> qint64 {  // search in [a, b)
                if (backward) {
                    for (qint64 end = b; end > a && !cancel->load();) {
                        const qint64 start = qMax(a, end - kChunk);
                        const qint64 len = qMin(end + needle.size() - 1, size) - start;
                        buf.resize(len);
                        const ssize_t n = ::pread(fd, buf.data(), size_t(len), start);
                        if (n <= 0)
                            return -1;
                        buf.resize(n);
                        const QByteArray lower = buf.toLower();
                        const qsizetype i = lower.lastIndexOf(needle, end - start - 1);
                        if (i >= 0)
                            return start + i;
                        end = start;
                    }
                } else {
                    for (qint64 start = a; start < b && !cancel->load();) {
                        const qint64 len = qMin(kChunk + needle.size() - 1, size - start);
                        buf.resize(len);
                        const ssize_t n = ::pread(fd, buf.data(), size_t(len), start);
                        if (n <= 0)
                            return -1;
                        buf.resize(n);
                        const qsizetype i = buf.toLower().indexOf(needle);
                        if (i >= 0 && start + i < b)
                            return start + i;
                        start += kChunk;
                    }
                }
                return -1;
            };
            const qint64 f = qBound<qint64>(0, from, size);
            found = backward ? scan(0, f + 1) : scan(f, size);
            if (found < 0 && !cancel->load()) {
                found = backward ? scan(f, size) : scan(0, f);
                wrapped = found >= 0;
            }
            ::close(fd);
        }
        if (cancel->load())
            return;
        postToUi([self, gen, found, wrapped, len = needle.size()] {
            if (!self || gen != self->m_gen)
                return;
            ViewerWindow* w = self.data();
            if (found < 0) {
                w->m_searchInfo->setText(tr("not found"));
                emit w->searchFinished(false);
                return;
            }
            w->m_matchStart = found;
            w->m_searchInfo->setText(wrapped ? tr("wrapped") : QString());
            ByteViewBase* v = w->m_mode == Mode::Hex ? static_cast<ByteViewBase*>(w->m_hex) : w->m_big;
            v->showOffset(found, len);
            w->updateStatus();
            emit w->searchFinished(true);
        });
    });
}

}  // namespace tandem
