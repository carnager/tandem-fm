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
#include <QHeaderView>
#include <QJsonDocument>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTreeWidget>
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
    qint64 mtime = 0;
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
        h.mtime = st.st_mtime;
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

// Markdown without fetching images: loading them would read files on the
// UI thread (and remote ones not at all). Alt text shows instead.
class DocBrowser : public QTextBrowser {
public:
    using QTextBrowser::QTextBrowser;
    QVariant loadResource(int type, const QUrl& name) override
    {
        if (type == QTextDocument::ImageResource)
            return {};
        return QTextBrowser::loadResource(type, name);
    }
};

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
    m_imageButton = makeButton(tr("Image"), tr("Image, page, frame, contents… (4)"));
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
    m_mediaPage = new QWidget(m_stack);
    auto* ml = new QVBoxLayout(m_mediaPage);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(0);
    m_image = new ImageView(m_mediaPage);
    m_info = new QLabel(m_mediaPage);
    m_info->setWordWrap(true);
    m_info->setTextFormat(Qt::PlainText);
    m_info->setContentsMargins(12, 8, 12, 8);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_info->setStyleSheet(t.css(QStringLiteral("color: @text; background: @header; border-top: 1px solid @border;")));
    ml->addWidget(m_image, 1);
    ml->addWidget(m_info);
    m_doc = new DocBrowser(m_stack);
    m_doc->setFrameShape(QFrame::NoFrame);
    m_doc->setOpenExternalLinks(true);
    m_doc->document()->setDocumentMargin(16);
    m_list = new QTreeWidget(m_stack);
    m_list->setColumnCount(2);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_message = new QLabel(m_stack);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setStyleSheet(t.css(QStringLiteral("color: @dimText;")));
    for (QWidget* w : {static_cast<QWidget*>(m_text), static_cast<QWidget*>(m_big), static_cast<QWidget*>(m_hex),
                       m_mediaPage, static_cast<QWidget*>(m_doc), static_cast<QWidget*>(m_list),
                       static_cast<QWidget*>(m_message)})
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
            w->m_kind = classifyPreview(w->m_path, head->mimeName, head->isDir, head->isSpecial, head->bytes);
            if (w->m_kind == PreviewKind::Special) {
                w->m_loading = false;
                w->showMessage(tr("This is a device, pipe or socket."));
                emit w->loaded();
                return;
            }
            w->m_size = head->size;
            w->m_mtime = head->mtime;
            w->m_mimeComment = head->isDir ? tr("Folder") : head->mimeComment;
            w->m_binary = w->m_kind == PreviewKind::Binary;
            if (w->m_kind != PreviewKind::Folder) {
                w->m_cache = new BlockCache(w->m_path, w->m_size, w);
                connect(w->m_cache, &BlockCache::failed, w,
                        [w](const QString& err) { w->showMessage(tr("Read error: %1").arg(err)); });
            }
            w->setMode(hasRichView(w->m_kind) ? Mode::Image : (w->m_binary ? Mode::Hex : Mode::Text));
        });
    });
}

void ViewerWindow::setMode(Mode mode)
{
    if (mode == Mode::Image ? !hasRichView(m_kind) : !m_cache)
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
        loadRich();
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
    const bool json = m_kind == PreviewKind::Json;
    QPointer<ViewerWindow> self(this);
    ioPool()->start([self, gen, path, json] {
        QFile f(path);
        QString text, encoding = QStringLiteral("UTF-8");
        QString error;
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray data = f.read(kSmallText + 1);
            if (json && data.size() <= kSmallText) {
                const QJsonDocument doc = QJsonDocument::fromJson(data);
                if (!doc.isNull())
                    data = doc.toJson(QJsonDocument::Indented);  // minified JSON becomes readable
            }
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
                auto def = repository().definitionForFileName(w->m_path);
                if (!def.isValid() && w->m_kind == PreviewKind::Json)
                    def = repository().definitionForName(QStringLiteral("JSON"));
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

QSize ViewerWindow::richMaxSize() const
{
    // The preview column never needs more than this; F3 shows images in full.
    return m_embedded ? QSize(1600, 1600) : QSize();
}

void ViewerWindow::loadRich()
{
    const QString key = PreviewCache::key(m_path, m_mtime, m_size, richMaxSize());
    if (auto cached = PreviewCache::instance().get(key)) {
        showRich(*cached);
        return;
    }
    showMessage(m_kind == PreviewKind::Video || m_kind == PreviewKind::Audio ? tr("Reading media…") : tr("Loading…"));
    m_loading = true;
    const quint64 gen = m_gen;
    const QString path = m_path;
    const PreviewKind kind = m_kind;
    const QSize maxSize = richMaxSize();
    QPointer<ViewerWindow> self(this);
    PreviewCache::instance().beginLoad(key);
    mediaPool()->start([self, gen, path, kind, maxSize, key] {
        auto result = std::make_shared<RichPreview>(produceRichPreview(path, kind, maxSize));
        postToUi([self, gen, key, result] {
            PreviewCache::instance().endLoad(key);
            if (result->error.isEmpty())
                PreviewCache::instance().put(key, result);
            if (self && gen == self->m_gen && self->m_mode == Mode::Image)
                self->showRich(*result);
        });
    });
}

void ViewerWindow::showRich(const RichPreview& r)
{
    m_loading = false;
    m_imageLoaded = true;
    if (!r.error.isEmpty()) {
        showMessage(r.error);
        emit loaded();
        return;
    }
    m_richInfo = r.info;
    QWidget* page = m_mediaPage;
    switch (m_kind) {
    case PreviewKind::Markdown:
        m_doc->setMarkdown(r.markdown);
        page = m_doc;
        break;
    case PreviewKind::Folder:
    case PreviewKind::Archive: {
        m_list->clear();
        QList<QTreeWidgetItem*> items;
        for (const RichPreview::Item& it : r.items) {
            FileEntry e;
            e.name = it.name;
            e.isDir = it.isDir;
            e.mode = it.mode;
            auto* row = new QTreeWidgetItem({it.name, it.isDir ? QString() : formatSize(it.size)});
            row->setIcon(0, IconCache::forEntry(e));
            row->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            items.append(row);
        }
        m_list->addTopLevelItems(items);
        if (r.items.isEmpty())
            m_list->addTopLevelItem(new QTreeWidgetItem({tr("(empty)")}));
        page = m_list;
        break;
    }
    case PreviewKind::Font: {
        QString family;
        m_image->setImage(renderFontSample(r.fontData, &family));
        m_richInfo = family;
        m_info->setText(family);
        m_info->setVisible(!family.isEmpty());
        break;
    }
    default:
        m_image->setImage(r.image);
        m_info->setText(r.info);
        m_info->setVisible(!r.info.isEmpty());
        if (r.image.isNull() && !r.info.isEmpty() && m_kind != PreviewKind::Image)
            m_image->setImage({});  // audio without cover: just the details
        break;
    }
    if (m_mode == Mode::Image) {
        m_stack->setCurrentWidget(page);
        if (!m_embedded)
            page->setFocus();
    }
    updateStatus();
    emit loaded();
}

void ViewerWindow::preload(const QStringList& paths)
{
    const QSize maxSize = richMaxSize();
    for (const QString& path : paths) {
        QPointer<ViewerWindow> self(this);
        mediaPool()->start([self, path, maxSize] {
            const Head head = readHead(path);
            if (head.error || head.isDir)
                return;
            const PreviewKind kind = classifyPreview(path, head.mimeName, false, head.isSpecial, head.bytes);
            // Only what's slow to make is worth preparing ahead.
            if (kind != PreviewKind::Image && kind != PreviewKind::Pdf && kind != PreviewKind::Video &&
                kind != PreviewKind::Audio)
                return;
            const QString key = PreviewCache::key(path, head.mtime, head.size, maxSize);
            if (!PreviewCache::instance().beginLoad(key))
                return;
            auto result = std::make_shared<RichPreview>(produceRichPreview(path, kind, maxSize));
            if (result->error.isEmpty())
                PreviewCache::instance().put(key, result);
            PreviewCache::instance().endLoad(key);
        });
    }
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
    m_imageButton->setVisible(hasRichView(m_kind));
    m_imageButton->setText(richViewLabel(m_kind));
    m_textButton->setVisible(m_cache != nullptr);
    m_hexButton->setVisible(m_cache != nullptr);
    m_wrapButton->setVisible(m_mode == Mode::Text);
}

void ViewerWindow::updateStatus()
{
    QStringList parts;
    if (m_kind != PreviewKind::Folder)
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
        if (m_kind == PreviewKind::Image) {
            if (!m_richInfo.isEmpty())
                parts << m_richInfo;  // original dimensions, even when decoded smaller
            parts << tr("%1%").arg(qRound(m_image->scale() * 100));
        } else if (m_kind == PreviewKind::Folder || m_kind == PreviewKind::Archive) {
            parts << m_richInfo;
        }
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
