#include "ui/viewer/ViewerWindow.h"

#include "core/Backend.h"

#include "core/FileOps.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"
#include "ui/viewer/BlockCache.h"
#include "ui/viewer/FileInfo.h"
#include "ui/viewer/ByteViews.h"
#include "ui/viewer/ImageView.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QGridLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QScrollArea>
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
    if (isRemoteLocation(path)) {  // only folders come here from servers (files are fetched first)
        FileStat st;
        h.error = statAt(path, st);
        if (!h.error && !st.isDir)
            h.error = ENOTSUP;
        h.isDir = true;
        h.mtime = st.mtime;
        return h;
    }
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
    // Embedded (preview column): an "inspector", deliberately unlike the file
    // panels: no card or header strip, content on an inset sheet.
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* top = new QWidget(this);
    top->setObjectName(QStringLiteral("viewerTop"));
    top->setAttribute(Qt::WA_StyledBackground);
    setThemeStyle(top, QStringLiteral(
        "#viewerTop { background: %1; %2 }"
        "QToolButton { color: @dimText; border: none; padding: 2px 8px; border-radius: 9px; }"
        "QToolButton:checked { color: @text; background: @border; }"
        "QToolButton:hover:!checked { color: @text; }")
                                .arg(embedded ? QStringLiteral("transparent") : QStringLiteral("@header"),
                                     embedded ? QString() : QStringLiteral("border-bottom: 1px solid @border;")));
    auto* tl = new QHBoxLayout(top);
    tl->setContentsMargins(10, 5, 8, 5);
    tl->setSpacing(4);
    m_titleIcon = new QLabel(top);
    m_titleIcon->setFixedSize(18, 18);
    m_titleIcon->hide();
    tl->addWidget(m_titleIcon);
    tl->addSpacing(2);
    m_title = new QLabel(top);
    QFont bold = m_title->font();
    bold.setWeight(QFont::DemiBold);
    m_title->setFont(bold);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // elided in resizeEvent
    if (embedded) {
        // A caption over the name, so it reads as a label, not a path bar.
        auto* titles = new QVBoxLayout;
        titles->setSpacing(0);
        auto* caption = new QLabel(tr("PREVIEW"), top);
        QFont cf = caption->font();
        cf.setPointSizeF(cf.pointSizeF() * 0.78);
        cf.setWeight(QFont::DemiBold);
        cf.setLetterSpacing(QFont::PercentageSpacing, 112);
        caption->setFont(cf);
        setThemeStyle(caption, QStringLiteral("color: @dimText;"));
        titles->addWidget(caption);
        titles->addWidget(m_title);
        tl->addLayout(titles, 1);
        tl->setContentsMargins(14, 8, 10, 4);
    } else {
        tl->addWidget(m_title, 1);
    }

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
    m_infoButton = makeButton(tr("Info"), tr("Details: type, dates, permissions, owner, checksum… (I)"));
    // Wrapping is a text option, not a view: a checkbox by the status line.
    m_wrapButton = new QCheckBox(tr("Wrap lines"), this);
    m_wrapButton->setToolTip(tr("Wrap long lines (W)"));
    m_wrapButton->setFocusPolicy(Qt::NoFocus);
    connect(m_textButton, &QToolButton::clicked, this, [this] { setMode(Mode::Text); });
    connect(m_hexButton, &QToolButton::clicked, this, [this] { setMode(Mode::Hex); });
    connect(m_imageButton, &QToolButton::clicked, this, [this] {
        setMode(Mode::Image);
        if (m_embedded)
            QSettings().setValue(QStringLiteral("preview/info"), false);
    });
    connect(m_infoButton, &QToolButton::clicked, this, [this] {
        setMode(Mode::Info);
        if (m_embedded)
            QSettings().setValue(QStringLiteral("preview/info"), true);
    });
    for (QToolButton* b : {m_textButton, m_hexButton})
        connect(b, &QToolButton::clicked, this, [this] {
            if (m_embedded)
                QSettings().setValue(QStringLiteral("preview/info"), false);
        });
    connect(m_wrapButton, &QCheckBox::toggled, this, [this](bool on) {
        m_text->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        m_big->setWrap(on);
        if (!m_embedded)
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
    setThemeStyle(m_info, QStringLiteral("color: @text; background: @header; border-top: 1px solid @border;"));
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
    auto* infoScroll = new QScrollArea(m_stack);
    infoScroll->setFrameShape(QFrame::NoFrame);
    infoScroll->setWidgetResizable(true);
    infoScroll->viewport()->setAutoFillBackground(false);
    auto* infoBody = new QWidget(infoScroll);
    infoBody->setAutoFillBackground(false);
    m_infoGrid = new QGridLayout(infoBody);
    m_infoGrid->setContentsMargins(14, 10, 14, 14);
    m_infoGrid->setHorizontalSpacing(16);
    m_infoGrid->setVerticalSpacing(5);
    m_infoGrid->setColumnStretch(1, 1);
    infoScroll->setWidget(infoBody);
    m_infoPage = infoScroll;
    m_noteBox = new QWidget(m_stack);
    auto* nl = new QVBoxLayout(m_noteBox);
    nl->addStretch(1);
    m_noteIcon = new QLabel(m_noteBox);
    m_noteIcon->setAlignment(Qt::AlignCenter);
    {
        // A big, dimmed icon for the empty state.
        QPixmap pm = IconCache::themed("view-preview", "document-preview").pixmap(64, 64);
        QPixmap faded(pm.size());
        faded.fill(Qt::transparent);
        QPainter fp(&faded);
        fp.setOpacity(0.35);
        fp.drawPixmap(0, 0, pm);
        fp.end();
        m_noteIcon->setPixmap(faded);
    }
    m_noteIcon->hide();
    m_message = new QLabel(m_noteBox);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    setThemeStyle(m_message, QStringLiteral("color: @dimText; padding: 8px 24px;"));
    nl->addWidget(m_noteIcon);
    nl->addWidget(m_message);
    nl->addStretch(1);
    for (QWidget* w : {static_cast<QWidget*>(m_text), static_cast<QWidget*>(m_big), static_cast<QWidget*>(m_hex),
                       m_mediaPage, static_cast<QWidget*>(m_doc), static_cast<QWidget*>(m_list), m_infoPage,
                       m_noteBox})
        m_stack->addWidget(w);
    if (embedded) {
        m_sheet = new QFrame(this);
        m_sheet->setObjectName(QStringLiteral("sheet"));
        auto* sl = new QVBoxLayout(m_sheet);
        sl->setContentsMargins(8, 8, 8, 8);
        sl->addWidget(m_stack);
        auto* inset = new QVBoxLayout;
        inset->setContentsMargins(10, 4, 10, 0);
        inset->addWidget(m_sheet);
        layout->addLayout(inset, 1);
    } else {
        layout->addWidget(m_stack, 1);
    }

    m_searchBar = new QWidget(this);
    m_searchBar->setObjectName(QStringLiteral("searchBar"));
    m_searchBar->setAttribute(Qt::WA_StyledBackground);
    setThemeStyle(m_searchBar, QStringLiteral(
        "#searchBar { background: @header; border-top: 1px solid @border; }"
        "QLineEdit { background: @base; color: @text; border: 1px solid @border; border-radius: 6px; padding: 3px 8px; }"
        "QLineEdit:focus { border-color: @accent; }"
        "QLabel { color: @dimText; }"));
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
    m_status->setContentsMargins(10, 4, 10, 5);
    setThemeStyle(m_status, QStringLiteral("color: @dimText; background: @window;"));
    if (embedded) {
        m_status->setContentsMargins(16, 6, 16, 8);
        m_status->setWordWrap(true);
    }
    auto* statusRow = new QWidget(this);
    setThemeStyle(statusRow, QStringLiteral("background: @window; color: @dimText;"));
    auto* sr = new QHBoxLayout(statusRow);
    sr->setContentsMargins(0, 0, embedded ? 14 : 10, 0);
    sr->setSpacing(6);
    sr->addWidget(m_status, 1);
    sr->addWidget(m_wrapButton, 0, Qt::AlignVCenter);
    layout->addWidget(statusRow);
#ifdef TANDEM_HAVE_KSYNTAXHIGHLIGHTING
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, [this] {
        if (!m_highlighter)
            return;
        m_highlighter->setTheme(repository().defaultTheme(theme().dark ? KSyntaxHighlighting::Repository::DarkTheme
                                                                        : KSyntaxHighlighting::Repository::LightTheme));
        m_highlighter->rehighlight();
    });
#endif
    if (embedded) {
        m_image->setBackground(t.window);  // pictures float on the background
        connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this,
                [this] { m_image->setBackground(theme().window); });
        m_image->setPadding(4);
        // The column is narrow: wrap long lines rather than scroll sideways.
        m_text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        m_big->setWrap(true);
        setThemeStyle(m_info, QStringLiteral("color: @text; background: @window;"));
        m_info->setContentsMargins(4, 10, 4, 0);
        m_doc->viewport()->setAutoFillBackground(false);
        m_list->viewport()->setAutoFillBackground(false);
    }

    connect(m_big, &ByteViewBase::positionChanged, this, &ViewerWindow::updateStatus);
    connect(m_hex, &ByteViewBase::positionChanged, this, &ViewerWindow::updateStatus);
    connect(m_image, &ImageView::scaleChanged, this, &ViewerWindow::updateStatus);

    QSettings s;
    m_wrapButton->setChecked(embedded || s.value(QStringLiteral("viewer/wrap"), false).toBool());
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

void ViewerWindow::paintEvent(QPaintEvent* e)
{
    if (!m_embedded) {
        QWidget::paintEvent(e);
        return;
    }
    QPainter p(this);
    p.fillRect(rect(), theme().window);
}

// Text, documents and lists sit on a rounded sheet; pictures and notes float.
void ViewerWindow::updateSheet()
{
    if (!m_sheet)
        return;
    QWidget* page = m_stack->currentWidget();
    const bool floating = page == m_mediaPage || page == m_noteBox;
    setThemeStyle(m_sheet, floating ? QStringLiteral("#sheet { background: transparent; }")
                                                : QStringLiteral("#sheet { background: @base; border-radius: 10px; }"));
}

void ViewerWindow::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    setTitle(m_titleText, QIcon());
}

void ViewerWindow::setTitle(const QString& text, const QIcon& icon)
{
    m_titleText = text;
    if (!icon.isNull()) {
        m_titleIcon->setPixmap(icon.pixmap(18, 18));
        m_titleIcon->show();
    } else if (text.isEmpty()) {
        m_titleIcon->hide();
    }
    m_title->setText(m_title->fontMetrics().elidedText(text, Qt::ElideMiddle, qMax(40, m_title->width())));
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
    add(Qt::Key_I, [this] { setMode(m_mode == Mode::Info ? defaultMode() : Mode::Info); });
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
    setTitle(QString(), QIcon());
    m_titleIcon->hide();
    m_title->setToolTip(QString());
    m_status->clear();
    for (QToolButton* b : {m_textButton, m_hexButton, m_imageButton, m_infoButton})
        b->hide();
    m_wrapButton->hide();
    showMessage(text);
    m_noteIcon->show();  // the empty state gets the big icon
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
    m_textLoaded = m_imageLoaded = m_infoLoaded = false;
    if (m_infoCancel)
        m_infoCancel->store(true);
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
    if (m_embedded) {
        FileEntry e;
        e.name = baseName(m_path);
        setTitle(e.name, IconCache::forEntry(e));
        m_title->setToolTip(m_path);
    } else {
        setTitle(m_files.size() == 1 ? m_path
                                     : QStringLiteral("%1   (%2/%3)").arg(m_path).arg(index + 1).arg(m_files.size()),
                 QIcon());
    }
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
            if (w->m_embedded && head->isDir) {
                FileEntry e;
                e.isDir = true;
                w->setTitle(w->m_titleText, IconCache::forEntry(e));
            }
            w->m_binary = w->m_kind == PreviewKind::Binary;
            if (w->m_kind != PreviewKind::Folder) {
                w->m_cache = new BlockCache(w->m_path, w->m_size, w);
                connect(w->m_cache, &BlockCache::failed, w,
                        [w](const QString& err) { w->showMessage(tr("Read error: %1").arg(err)); });
            }
            const bool info = w->m_embedded && QSettings().value(QStringLiteral("preview/info"), false).toBool();
            w->setMode(info ? Mode::Info : w->defaultMode());
        });
    });
}

ViewerWindow::Mode ViewerWindow::defaultMode() const
{
    return hasRichView(m_kind) ? Mode::Image : (m_binary ? Mode::Hex : Mode::Text);
}

void ViewerWindow::setShowInfo(bool on)
{
    QSettings().setValue(QStringLiteral("preview/info"), on);
    if (m_path.isEmpty() || m_kind == PreviewKind::Special)
        return;
    setMode(on ? Mode::Info : defaultMode());
}

void ViewerWindow::setMode(Mode mode)
{
    if (mode == Mode::Info) {
        if (m_path.isEmpty())
            return;
    } else if (mode == Mode::Image ? !hasRichView(m_kind) : !m_cache) {
        return;
    }
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
    case Mode::Info:
        if (m_infoLoaded) {
            m_stack->setCurrentWidget(m_infoPage);
        } else {
            loadInfo();
        }
        m_loading = false;
        break;
    case Mode::None:
        break;
    }
    updateSheet();
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
                w->updateSheet();
                if (!w->m_embedded)
                    w->m_text->setFocus();
            }
            w->updateStatus();
            emit w->loaded();
        });
    });
}

void ViewerWindow::loadInfo()
{
    const quint64 gen = m_gen;
    const QString path = m_path;
    const char git = m_gitHint;
    QPointer<ViewerWindow> self(this);
    ioPool()->start([self, gen, path, git] {
        auto info = std::make_shared<FileInfo>(gatherFileInfo(path, git));
        postToUi([self, gen, info] {
            if (!self || gen != self->m_gen)
                return;
            self->fillInfo(*info);
        });
    });
}

void ViewerWindow::fillInfo(const FileInfo& info)
{
    while (QLayoutItem* item = m_infoGrid->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    QWidget* body = m_infoGrid->parentWidget();
    int row = 0;
    auto addRow = [&](const QString& key, QWidget* value) {
        auto* k = new QLabel(key, body);
        setThemeStyle(k, QStringLiteral("color: @dimText;"));
        k->setAlignment(Qt::AlignRight | Qt::AlignTop);
        k->show();
        value->show();
        m_infoGrid->addWidget(k, row, 0);
        m_infoGrid->addWidget(value, row, 1);
        ++row;
    };
    auto valueLabel = [&](const QString& text) {
        auto* v = new QLabel(text, body);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->setWordWrap(true);
        v->setTextFormat(Qt::PlainText);
        return v;
    };
    if (!info.error.isEmpty()) {
        addRow(tr("Error"), valueLabel(info.error));
    }
    for (const FileInfoRow& r : info.rows) {
        if (r.key.isEmpty()) {  // section title
            auto* title = new QLabel(r.value.toUpper(), body);
            QFont f = title->font();
            f.setPointSizeF(f.pointSizeF() * 0.78);
            f.setWeight(QFont::DemiBold);
            f.setLetterSpacing(QFont::PercentageSpacing, 112);
            title->setFont(f);
            setThemeStyle(title, QStringLiteral("color: @accent; padding-top: %1px;").arg(row ? 10 : 0));
            title->show();
            m_infoGrid->addWidget(title, row++, 0, 1, 2);
            continue;
        }
        addRow(r.key, valueLabel(r.value));
    }
    const QString path = m_path;
    QPointer<ViewerWindow> self(this);
    if (m_infoCancel)
        m_infoCancel->store(true);
    m_infoCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_infoCancel;
    if (info.isDir) {
        // Total size can take a while: fill it in when it's known.
        QLabel* total = valueLabel(tr("calculating…"));
        addRow(tr("Total size"), total);
        QPointer<QLabel> target(total);
        mediaPool()->start([path, cancel, target] {
            const int64_t size = directorySize(path, cancel.get());
            postToUi([target, size] {
                if (target)
                    target->setText(size < 0 ? QObject::tr("unknown") : formatSize(size));
            });
        });
    } else if (info.isRegular) {
        auto* compute = new QPushButton(tr("Compute"), body);
        compute->setFlat(true);
        compute->setCursor(Qt::PointingHandCursor);
        setThemeStyle(compute, QStringLiteral("QPushButton { color: @accent; text-align: left; padding: 0; border: none; }"));
        addRow(QStringLiteral("SHA-256"), compute);
        QPointer<QPushButton> button(compute);
        connect(compute, &QPushButton::clicked, this, [path, cancel, button] {
            if (!button)
                return;
            button->setText(QObject::tr("computing…"));
            button->setEnabled(false);
            mediaPool()->start([path, cancel, button] {
                const QString hash = sha256Of(path, cancel.get());
                postToUi([button, hash] {
                    if (!button)
                        return;
                    button->setText(hash.isEmpty() ? QObject::tr("not readable") : hash);
                    button->setEnabled(true);
                    button->setStyleSheet(QStringLiteral("QPushButton { text-align: left; padding: 0; border: none; font-family: monospace; }"));
                });
            });
        });
    }
    m_infoGrid->setRowStretch(row, 1);
    m_infoLoaded = true;
    m_loading = false;
    if (m_mode == Mode::Info) {
        m_stack->setCurrentWidget(m_infoPage);
        updateSheet();
    }
    updateStatus();
    emit loaded();
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
        if (m_kind == PreviewKind::Folder && m_folderTotal.first == m_path)
            m_richInfo = tr("%1  ·  %2 in all").arg(m_richInfo, formatSize(m_folderTotal.second));
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
        // A plain picture's size is already in the status line.
        m_info->setVisible(!r.info.isEmpty() && !(m_embedded && m_kind == PreviewKind::Image));
        if (r.image.isNull() && !r.info.isEmpty() && m_kind != PreviewKind::Image)
            m_image->setImage({});  // audio without cover: just the details
        break;
    }
    if (m_mode == Mode::Image) {
        m_stack->setCurrentWidget(page);
        updateSheet();
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
    m_noteIcon->hide();
    m_stack->setCurrentWidget(m_noteBox);
    updateSheet();
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
    m_infoButton->setChecked(m_mode == Mode::Info);
    m_infoButton->setVisible(!m_path.isEmpty());
    m_imageButton->setVisible(hasRichView(m_kind));
    m_imageButton->setText(richViewLabel(m_kind));
    m_textButton->setVisible(m_cache != nullptr);
    m_hexButton->setVisible(m_cache != nullptr);
    m_wrapButton->setVisible(m_mode == Mode::Text);
}

void ViewerWindow::updateStatus()
{
    if (m_path.isEmpty()) {
        m_status->clear();
        return;
    }
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
    case Mode::Info:
    case Mode::None:
        break;
    }
    m_status->setText(parts.join(QStringLiteral("   ·   ")) +
                      (m_embedded ? QString()
                                  : tr("        N / P: next / previous file   1 / 3 / 4: text / hex / image   F7: find")));
}

void ViewerWindow::setFolderTotal(const QString& path, qint64 bytes)
{
    if (bytes < 0)
        return;
    m_folderTotal = {path, bytes};  // may come before the listing does
    if (path == m_path && m_kind == PreviewKind::Folder && m_imageLoaded && !m_loading) {
        m_richInfo = tr("%1  ·  %2 in all").arg(m_richInfo, formatSize(bytes));
        updateStatus();
    }
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
    case Mode::Info:
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
