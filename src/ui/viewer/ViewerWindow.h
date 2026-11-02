#pragma once

#include "ui/viewer/Previewers.h"

#include <QStringList>
#include <QWidget>

#include <atomic>
#include <memory>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedWidget;
class QTextBrowser;
class QToolButton;
class QTreeWidget;

namespace KSyntaxHighlighting {
class SyntaxHighlighter;
}

namespace tandem {

class BlockCache;
class HexView;
class ImageView;
class TextScrollView;

// F3 lister. Text (highlighted when small), any-size text, hex, images.
// All file access happens on worker threads.
class ViewerWindow : public QWidget {
    Q_OBJECT
public:
    enum class Mode { None, Text, Hex, Image };

    // embedded: a plain child widget (quick view): no own shortcuts, not a window.
    ViewerWindow(const QStringList& files, int index, QWidget* parent = nullptr, bool embedded = false);
    void showFile(const QString& path);
    void showNote(const QString& text);
    ~ViewerWindow() override;

    void openIndex(int index);
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    QString currentPath() const { return m_path; }
    bool isLoading() const { return m_loading; }
    bool usesLargeTextView() const;  // for tests

    PreviewKind kind() const { return m_kind; }
    QString richInfo() const { return m_richInfo; }
    QTreeWidget* contentsList() const { return m_list; }
    QTextBrowser* documentView() const { return m_doc; }
    // Prepare rich previews of these files in the background (preview column).
    void preload(const QStringList& paths);

    QPlainTextEdit* textEdit() const { return m_text; }
    TextScrollView* largeTextView() const { return m_big; }
    HexView* hexView() const { return m_hex; }
    ImageView* imageView() const { return m_image; }

    static constexpr qint64 kSmallText = 2 * 1024 * 1024;  // editor widget up to this size
    static constexpr qint64 kHighlightMax = 1024 * 1024;    // syntax highlighting up to this size

signals:
    void loaded();  // a file finished loading in the current mode (tests)
    void searchFinished(bool found);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void setupShortcuts();
    void loadText();
    void loadRich();
    void showRich(const RichPreview& r);
    QSize richMaxSize() const;
    void showMessage(const QString& text);
    void updateStatus();
    void updateModeButtons();
    void showSearch();
    void find(bool backward);
    void findInBytes(bool backward);
    void zoom(int steps);

    bool m_embedded = false;
    QStringList m_files;
    int m_index = -1;
    QString m_path;
    qint64 m_size = 0;
    QString m_mimeComment;
    PreviewKind m_kind = PreviewKind::Text;
    qint64 m_mtime = 0;
    QString m_richInfo;
    bool m_binary = false;
    bool m_textLoaded = false;
    bool m_imageLoaded = false;
    bool m_loading = false;
    QString m_encoding;
    quint64 m_gen = 0;
    Mode m_mode = Mode::None;
    qint64 m_matchStart = -1;
    std::shared_ptr<std::atomic<bool>> m_searchCancel;

    BlockCache* m_cache = nullptr;
    QStackedWidget* m_stack;
    QPlainTextEdit* m_text;
    TextScrollView* m_big;
    HexView* m_hex;
    ImageView* m_image;
    QWidget* m_mediaPage;
    QLabel* m_info;
    QTextBrowser* m_doc;
    QTreeWidget* m_list;
    QLabel* m_message;
    QLabel* m_title;
    QLabel* m_status;
    QToolButton* m_textButton;
    QToolButton* m_hexButton;
    QToolButton* m_imageButton;
    QToolButton* m_wrapButton;
    QWidget* m_searchBar;
    QLineEdit* m_searchEdit;
    QLabel* m_searchInfo;
    KSyntaxHighlighting::SyntaxHighlighter* m_highlighter = nullptr;
};

}  // namespace tandem
