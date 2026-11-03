#include "ui/ShortcutsDialog.h"

#include "ui/Theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>

namespace tandem {

namespace {

struct Entry {
    const char* section;
    const char* keys;
    const char* text;
};

// Keys stay as written (they're keys); the descriptions are translated.
const Entry kEntries[] = {
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Tab", QT_TRANSLATE_NOOP("Shortcuts", "Switch panel")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Enter", QT_TRANSLATE_NOOP("Shortcuts", "Open")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Backspace", QT_TRANSLATE_NOOP("Shortcuts", "Parent folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "← / →", QT_TRANSLATE_NOOP("Shortcuts", "Parent folder / into folder or archive (can be turned off)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "a…z", QT_TRANSLATE_NOOP("Shortcuts", "Quick search: type letters (↑ ↓ next match)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Alt+←", QT_TRANSLATE_NOOP("Shortcuts", "Back")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Alt+→", QT_TRANSLATE_NOOP("Shortcuts", "Forward")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Ctrl+PgUp", QT_TRANSLATE_NOOP("Shortcuts", "Parent folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Ctrl+\\", QT_TRANSLATE_NOOP("Shortcuts", "Root folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Ctrl+`", QT_TRANSLATE_NOOP("Shortcuts", "Home folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Ctrl+L", QT_TRANSLATE_NOOP("Shortcuts", "Type a path or server address")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Alt+F1 / F2", QT_TRANSLATE_NOOP("Shortcuts", "Places, drives and servers (left / right panel)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Ctrl+J", QT_TRANSLATE_NOOP("Shortcuts", "Jump to any folder you visited (fuzzy)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Ctrl+D", QT_TRANSLATE_NOOP("Shortcuts", "Bookmark keys (chains like cp)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Ctrl+Shift+D", QT_TRANSLATE_NOOP("Shortcuts", "Bookmark this folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Moving around"), "Ctrl+PgDn", QT_TRANSLATE_NOOP("Shortcuts", "Enter a folder, or browse any file as an archive")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Selecting"), "Insert", QT_TRANSLATE_NOOP("Shortcuts", "Mark")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Selecting"), "Space", QT_TRANSLATE_NOOP("Shortcuts", "Mark (and size a folder)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Selecting"), "Shift+↑↓", QT_TRANSLATE_NOOP("Shortcuts", "Mark while moving")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Selecting"), "Num+ / Num-", QT_TRANSLATE_NOOP("Shortcuts", "Mark / unmark a group (*.jpg …)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Selecting"), "Num*", QT_TRANSLATE_NOOP("Shortcuts", "Invert the marks")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Selecting"), "Ctrl+A", QT_TRANSLATE_NOOP("Shortcuts", "Mark all")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Selecting"), "Ctrl+Shift+A", QT_TRANSLATE_NOOP("Shortcuts", "Mark none")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Selecting"), "Alt+Shift+Enter", QT_TRANSLATE_NOOP("Shortcuts", "Calculate all folder sizes")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "F3", QT_TRANSLATE_NOOP("Shortcuts", "View")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "F4", QT_TRANSLATE_NOOP("Shortcuts", "Edit (on servers and in archives: a copy that saves back)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "F5", QT_TRANSLATE_NOOP("Shortcuts", "Copy to the other panel (into archives too)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "F6", QT_TRANSLATE_NOOP("Shortcuts", "Move to the other panel")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "F7", QT_TRANSLATE_NOOP("Shortcuts", "New folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Shift+F4", QT_TRANSLATE_NOOP("Shortcuts", "New file")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "F8", QT_TRANSLATE_NOOP("Shortcuts", "Move to the trash")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Shift+F8", QT_TRANSLATE_NOOP("Shortcuts", "Delete permanently")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Shift+F6", QT_TRANSLATE_NOOP("Shortcuts", "Rename in place")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Ctrl+M", QT_TRANSLATE_NOOP("Shortcuts", "Rename many with a template")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Ctrl+E", QT_TRANSLATE_NOOP("Shortcuts", "Rename in your text editor")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Alt+A", QT_TRANSLATE_NOOP("Shortcuts", "Permissions, extra people and groups (ACLs), flags")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Ctrl+Shift+F5", QT_TRANSLATE_NOOP("Shortcuts", "Create a link (symbolic, relative or hard)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Alt+Shift+C", QT_TRANSLATE_NOOP("Shortcuts", "Checksums: compute and save, or check .sha256/.md5 files")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Ctrl+C / X / V", QT_TRANSLATE_NOOP("Shortcuts", "Copy, cut, paste (works with other apps)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Ctrl+Z", QT_TRANSLATE_NOOP("Shortcuts", "Undo the last move, copy, trash, rename or new folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Alt+F5", QT_TRANSLATE_NOOP("Shortcuts", "Pack into an archive")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Alt+F9", QT_TRANSLATE_NOOP("Shortcuts", "Unpack to other panel")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Alt+Shift+F9", QT_TRANSLATE_NOOP("Shortcuts", "Unpack here (each archive next to itself)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Files"), "Menu / Shift+F10", QT_TRANSLATE_NOOP("Shortcuts", "Context menu")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Tabs and panels"), "Ctrl+T", QT_TRANSLATE_NOOP("Shortcuts", "New tab")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Tabs and panels"), "Ctrl+W", QT_TRANSLATE_NOOP("Shortcuts", "Close tab")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Tabs and panels"), "Ctrl+Tab", QT_TRANSLATE_NOOP("Shortcuts", "Next tab")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Tabs and panels"), "Alt+1 … 9", QT_TRANSLATE_NOOP("Shortcuts", "Tab 1 … 9, created here if it doesn't exist")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Tabs and panels"), "Ctrl+← / →", QT_TRANSLATE_NOOP("Shortcuts", "Show this folder in the left / right panel")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Tabs and panels"), "Ctrl+U", QT_TRANSLATE_NOOP("Shortcuts", "Swap panels")},
    {QT_TRANSLATE_NOOP("Shortcuts", "View"), "Ctrl+Q", QT_TRANSLATE_NOOP("Shortcuts", "Preview (below each panel, below both or on the right)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "View"), "Ctrl+I", QT_TRANSLATE_NOOP("Shortcuts", "Previews show details: type, dates, permissions, checksum")},
    {QT_TRANSLATE_NOOP("Shortcuts", "View"), "Ctrl+H", QT_TRANSLATE_NOOP("Shortcuts", "Show hidden files")},
    {QT_TRANSLATE_NOOP("Shortcuts", "View"), "Ctrl+Shift+F1", QT_TRANSLATE_NOOP("Shortcuts", "Thumbnails instead of the list, in this tab")},
    {QT_TRANSLATE_NOOP("Shortcuts", "View"), "Ctrl+= / Ctrl+−", QT_TRANSLATE_NOOP("Shortcuts", "Zoom in / out (also Ctrl+wheel, or the slider)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "View"), "Ctrl+0", QT_TRANSLATE_NOOP("Shortcuts", "Normal size")},
    {QT_TRANSLATE_NOOP("Shortcuts", "View"), "Ctrl+F3 … F6", QT_TRANSLATE_NOOP("Shortcuts", "Sort by name / extension / date / size")},
    {QT_TRANSLATE_NOOP("Shortcuts", "View"), "Ctrl+Shift+P", QT_TRANSLATE_NOOP("Shortcuts", "Command palette: every command by name")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Search and compare"), "Alt+F7", QT_TRANSLATE_NOOP("Shortcuts", "Search names or contents here and below; the results are a folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Search and compare"), "Alt+Shift+F7", QT_TRANSLATE_NOOP("Shortcuts", "Find duplicate files and keep one of each")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Search and compare"), "Ctrl+Shift+U", QT_TRANSLATE_NOOP("Shortcuts", "Disk usage: what takes the space, biggest first")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Search and compare"), "Ctrl+F", QT_TRANSLATE_NOOP("Shortcuts", "Filter this folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Search and compare"), "Shift+F2", QT_TRANSLATE_NOOP("Shortcuts", "Compare the panels: mark what's newer or missing")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Search and compare"), "Ctrl+Shift+S", QT_TRANSLATE_NOOP("Shortcuts", "Synchronize folders, with a preview of the plan")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Search and compare"), "Esc", QT_TRANSLATE_NOOP("Shortcuts", "Stop a search or load, close the search bar, clear the filter, leave results")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Servers and tools"), "Ctrl+K", QT_TRANSLATE_NOOP("Shortcuts", "Connect to a server: sftp and smb built in, WebDAV and ftp via GVfs")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Servers and tools"), "Ctrl+;", QT_TRANSLATE_NOOP("Shortcuts", "Run a command on the selection (%s files, %f file, %d folder, %D other panel)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Servers and tools"), "Ctrl+Alt+T", QT_TRANSLATE_NOOP("Shortcuts", "Terminal here")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Servers and tools"), "F1", QT_TRANSLATE_NOOP("Shortcuts", "These shortcuts")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Servers and tools"), "Ctrl+,", QT_TRANSLATE_NOOP("Shortcuts", "Settings: look, tools, keys")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Servers and tools"), "Alt+F4", QT_TRANSLATE_NOOP("Shortcuts", "Quit")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "j / k", QT_TRANSLATE_NOOP("Shortcuts", "Down / up")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "h / l", QT_TRANSLATE_NOOP("Shortcuts", "Out of / into a folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "gg / G", QT_TRANSLATE_NOOP("Shortcuts", "Top / bottom")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "Ctrl+D / U", QT_TRANSLATE_NOOP("Shortcuts", "Half a page down / up")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "/ · n · N", QT_TRANSLATE_NOOP("Shortcuts", "Search, next, previous")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "v / V", QT_TRANSLATE_NOOP("Shortcuts", "Visual mode: moving marks (V: unmarks) a range; Esc or v ends it")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "Space", QT_TRANSLATE_NOOP("Shortcuts", "Mark and move down")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "y / x / p", QT_TRANSLATE_NOOP("Shortcuts", "Copy, cut, paste (clipboard)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "d / D", QT_TRANSLATE_NOOP("Shortcuts", "Move to trash / delete permanently")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "r · a", QT_TRANSLATE_NOOP("Shortcuts", "Rename · create (a name ending in / makes a folder)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), ".", QT_TRANSLATE_NOOP("Shortcuts", "Show hidden files")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "f · s · z", QT_TRANSLATE_NOOP("Shortcuts", "Filter · search files · jump to a folder")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "t · q", QT_TRANSLATE_NOOP("Shortcuts", "New tab · close tab")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "[ / ] · 1…9", QT_TRANSLATE_NOOP("Shortcuts", "Previous / next tab · tab number")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), ", n e m s", QT_TRANSLATE_NOOP("Shortcuts", "Sort by name, extension, modified, size")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "w", QT_TRANSLATE_NOOP("Shortcuts", "Background jobs")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "K / J", QT_TRANSLATE_NOOP("Shortcuts", "Scroll the preview")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), ":", QT_TRANSLATE_NOOP("Shortcuts", "Command palette")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "!", QT_TRANSLATE_NOOP("Shortcuts", "Run a command")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "'", QT_TRANSLATE_NOOP("Shortcuts", "Go to a bookmark")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "m", QT_TRANSLATE_NOOP("Shortcuts", "Set a bookmark")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "g<key>", QT_TRANSLATE_NOOP("Shortcuts", "Bookmark key (gh: home)")},
    {QT_TRANSLATE_NOOP("Shortcuts", "Modal keys (Settings › Behaviour)"), "~", QT_TRANSLATE_NOOP("Shortcuts", "These shortcuts")},
};

// The keys as little key caps: "Ctrl+F5 / Shift+F8" → [Ctrl][F5] / [Shift][F8].
class KeyCaps : public QWidget {
public:
    KeyCaps(const QString& keys, QWidget* parent) : QWidget(parent)
    {
        static const QRegularExpression alternatives(QStringLiteral("\\s+(/|·|…)\\s+"));
        int last = 0;
        auto addKeys = [&](const QString& alt) {
            if (alt.size() > 1 && alt.contains(QLatin1Char('+')) && !alt.endsWith(QLatin1Char('+'))) {
                const QStringList parts = alt.split(QLatin1Char('+'));
                for (int i = 0; i < parts.size(); ++i)
                    m_parts.append({parts[i], true});
            } else {
                m_parts.append({alt, true});
            }
        };
        auto it = alternatives.globalMatch(keys);
        while (it.hasNext()) {
            const auto m = it.next();
            addKeys(keys.mid(last, m.capturedStart() - last));
            m_parts.append({m.captured(1), false});
            last = int(m.capturedEnd());
        }
        addKeys(keys.mid(last));
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.88);
        setFont(f);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        int w = 0;
        for (const auto& [text, cap] : m_parts)
            w += cap ? fm.horizontalAdvance(text) + 14 + 3 : fm.horizontalAdvance(text) + 8;
        return {w, fm.height() + 8};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const Theme& t = theme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QFontMetrics fm(font());
        int x = 0;
        const int h = fm.height() + 6;
        for (const auto& [text, cap] : m_parts) {
            if (!cap) {
                p.setPen(t.dimText);
                const int w = fm.horizontalAdvance(text) + 8;
                p.drawText(QRect(x, 0, w, h + 2), Qt::AlignCenter, text);
                x += w;
                continue;
            }
            const int w = fm.horizontalAdvance(text) + 14;
            const QRectF r(x + 0.5, 0.5, w - 1, h);
            p.setPen(QPen(t.control, 1));
            p.setBrush(t.base);
            p.drawRoundedRect(r, 4, 4);
            p.setPen(QPen(t.control, 2));  // a key's bottom edge
            p.drawLine(QPointF(r.left() + 3, r.bottom() + 0.5), QPointF(r.right() - 3, r.bottom() + 0.5));
            p.setPen(t.text);
            p.drawText(r, Qt::AlignCenter, text);
            x += w + 3;
        }
    }

private:
    QList<QPair<QString, bool>> m_parts;  // text, is a key cap
};

}  // namespace

ShortcutsDialog::ShortcutsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Keyboard shortcuts"));
    const QSize avail = parent ? parent->window()->size() : QSize(1300, 860);
    resize(qMin(1240, avail.width() - 60), qMin(820, avail.height() - 60));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 18, 22, 16);
    layout->setSpacing(12);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Find a shortcut: type what you want to do, or a key"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_page = new QWidget(scroll);
    scroll->setWidget(m_page);
    layout->addWidget(scroll, 1);
    auto* note = new QLabel(tr("These are the default keys. Change them in Settings › Keys (Ctrl+,)."), this);
    setThemeStyle(note, QStringLiteral("color: @dimText;"));
    layout->addWidget(note);
    connect(m_filter, &QLineEdit::textChanged, this, &ShortcutsDialog::rebuild);
    rebuild();
}

void ShortcutsDialog::resizeEvent(QResizeEvent* e)
{
    QDialog::resizeEvent(e);
    if (qMax(1, (width() - 44) / 380) != m_columns)
        rebuild();
}

void ShortcutsDialog::rebuild()
{
    const QString q = m_filter->text().trimmed();
    m_columns = qMax(1, (width() - 44) / 380);

    // Sections in order, each with its matching entries.
    QList<QPair<QString, QList<const Entry*>>> sections;
    m_visible = 0;
    for (const Entry& e : kEntries) {
        const QString section = QCoreApplication::translate("Shortcuts", e.section);
        const QString text = QCoreApplication::translate("Shortcuts", e.text);
        if (!q.isEmpty() && !text.contains(q, Qt::CaseInsensitive) && !section.contains(q, Qt::CaseInsensitive) &&
            !QString::fromUtf8(e.keys).contains(q, Qt::CaseInsensitive))
            continue;
        if (sections.isEmpty() || sections.last().first != section)
            sections.append({section, {}});
        sections.last().second.append(&e);
        ++m_visible;
    }

    delete m_page->layout();
    qDeleteAll(m_page->findChildren<QWidget*>(Qt::FindDirectChildrenOnly));
    auto* columns = new QHBoxLayout(m_page);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(36);
    // One grid per column, so keys and descriptions line up across its sections.
    QList<QGridLayout*> cols;
    QList<int> heights, rows;
    for (int c = 0; c < m_columns; ++c) {
        auto* v = new QVBoxLayout;
        auto* g = new QGridLayout;
        g->setHorizontalSpacing(14);
        g->setVerticalSpacing(7);
        g->setColumnStretch(1, 1);
        v->addLayout(g);
        v->addStretch(1);
        columns->addLayout(v, 1);
        cols << g;
        heights << 0;
        rows << 0;
    }
    if (sections.isEmpty()) {
        auto* none = new QLabel(tr("No shortcut matches “%1”").arg(q), m_page);
        setThemeStyle(none, QStringLiteral("color: @dimText;"));
        cols.first()->addWidget(none, 0, 0, 1, 2);
    }
    for (const auto& [name, entries] : sections) {
        // Shortest column next, so the columns end at about the same height.
        const int c = int(std::min_element(heights.begin(), heights.end()) - heights.begin());
        auto* head = new QLabel(name.toUpper(), m_page);
        QFont hf = head->font();
        hf.setPointSizeF(hf.pointSizeF() * 0.82);
        hf.setBold(true);
        hf.setLetterSpacing(QFont::PercentageSpacing, 108);
        head->setFont(hf);
        setThemeStyle(head, QStringLiteral("color: @accent; padding-top: %1px; padding-bottom: 2px;").arg(heights[c] ? 14 : 0));
        cols[c]->addWidget(head, rows[c]++, 0, 1, 2);
        for (const Entry* e : entries) {
            cols[c]->addWidget(new KeyCaps(QString::fromUtf8(e->keys), m_page), rows[c], 0, Qt::AlignLeft | Qt::AlignTop);
            auto* text = new QLabel(QCoreApplication::translate("Shortcuts", e->text), m_page);
            text->setWordWrap(true);
            cols[c]->addWidget(text, rows[c]++, 1, Qt::AlignTop);
        }
        heights[c] += 2 + int(entries.size());
    }
}

}  // namespace tandem
