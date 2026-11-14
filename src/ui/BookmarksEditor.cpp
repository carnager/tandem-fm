#include "ui/BookmarksEditor.h"

#include "core/FolderHistory.h"
#include "core/Util.h"
#include "ui/IconCache.h"

#include <QHeaderView>
#include <QKeyEvent>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace tandem {

namespace {

QString pathOf(const QString& text)
{
    return text.startsWith(QLatin1Char('~')) ? expandUserPath(text) : text;
}

// Del removes the selected bookmark, unless a cell is being edited.
class BookmarkTable : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void()> onDelete;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Delete && state() != EditingState && onDelete) {
            onDelete();
            return;
        }
        QTreeWidget::keyPressEvent(e);
    }
};

}  // namespace

BookmarksEditor::BookmarksEditor(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    auto* table = new BookmarkTable(this);
    m_table = table;
    m_table->setObjectName(QStringLiteral("bookmarks"));
    m_table->setColumnCount(2);
    m_table->setHeaderLabels({tr("Key"), tr("Folder")});
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                             QAbstractItemView::AnyKeyPressed);
    m_table->setDragDropMode(QAbstractItemView::InternalMove);  // the order Locations shows
    m_table->header()->setStretchLastSection(true);
    m_table->header()->resizeSection(0, 80);
    v->addWidget(m_table, 1);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(IconCache::themed("list-add", "document-new"), tr("&Add this folder"), this);
    m_remove = new QPushButton(IconCache::themed("list-remove", "edit-delete"), tr("&Remove"), this);
    for (QPushButton* b : {add, m_remove}) {
        b->setObjectName(QStringLiteral("flat"));
        b->setCursor(Qt::PointingHandCursor);
        buttons->addWidget(b);
    }
    buttons->addStretch(1);
    v->addLayout(buttons);

    for (const FolderHistory::Bookmark& b : FolderHistory::instance().pinnedBookmarks())
        addRow(b.key, b.path);

    auto remove = [this] { delete m_table->currentItem(); };
    table->onDelete = remove;
    connect(m_remove, &QPushButton::clicked, this, remove);
    connect(add, &QPushButton::clicked, this, [this] {
        addRow(QString(), m_currentFolder);
        m_table->setCurrentItem(m_table->topLevelItem(m_table->topLevelItemCount() - 1), 0);
        m_table->editItem(m_table->currentItem(), 0);  // its key next
    });
    connect(m_table, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current) { m_remove->setEnabled(current); });
    if (m_table->topLevelItemCount() > 0)
        m_table->setCurrentItem(m_table->topLevelItem(0));
    m_remove->setEnabled(m_table->currentItem());
}

void BookmarksEditor::addRow(const QString& key, const QString& path)
{
    auto* item = new QTreeWidgetItem(m_table, {key, path});
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
    item->setIcon(1, IconCache::themed(path.contains(QLatin1String("://")) ? "folder-remote" : "folder", "folder"));
}

QString BookmarksEditor::check()
{
    QSet<QString> keys, paths;
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_table->topLevelItem(i);
        const QString key = item->text(0).trimmed();
        const QString path = item->text(1).trimmed();
        QString problem;
        if (path.isEmpty())
            problem = tr("A bookmark needs a folder.");
        else if (key.contains(QLatin1Char(' ')))
            problem = tr("Keys have no spaces: “%1”.").arg(key);
        else if (!key.isEmpty() && keys.contains(key))
            problem = tr("The key “%1” is used twice.").arg(key);
        else if (paths.contains(pathOf(path)))
            problem = tr("%1 is bookmarked twice.").arg(path);
        if (!problem.isEmpty()) {
            m_table->setCurrentItem(item);
            return problem;
        }
        keys.insert(key);
        paths.insert(pathOf(path));
    }
    return {};
}

void BookmarksEditor::save()
{
    QList<FolderHistory::Bookmark> pins;
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* item = m_table->topLevelItem(i);
        pins.append({item->text(0).trimmed(), pathOf(item->text(1).trimmed())});
    }
    if (pins != FolderHistory::instance().pinnedBookmarks())
        FolderHistory::instance().setBookmarks(pins);
}

}  // namespace tandem
