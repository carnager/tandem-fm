#include "ui/BookmarksEditor.h"

#include "core/FolderHistory.h"
#include "core/Util.h"
#include "ui/CardList.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

namespace tandem {

namespace {

constexpr int kKeyRole = Qt::UserRole + 10;
constexpr int kPathRole = Qt::UserRole + 11;

// Local paths: ~ expanded; server addresses as they are.
QString pathOf(const QString& text)
{
    return text.startsWith(QLatin1Char('~')) ? expandUserPath(text) : text;
}

QString nameOf(const QString& path)
{
    const QString name = baseName(path);
    return name.isEmpty() ? path : name;
}

}  // namespace

BookmarksEditor::BookmarksEditor(QWidget* parent) : QWidget(parent)
{
    auto* columns = new QHBoxLayout(this);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(18);

    // Left: the bookmarks, in the order Ctrl+D and Locations show them.
    auto* left = new QVBoxLayout;
    left->setSpacing(4);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("bookmarkList"));
    m_list->setItemDelegate(new CardDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->setFixedWidth(250);  // as the servers: the divider stays put
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->installEventFilter(this);
    left->addWidget(m_list, 1);
    auto* add = new QPushButton(IconCache::themed("list-add", "document-new"), tr("&Add this folder"), this);
    m_remove = new QPushButton(IconCache::themed("list-remove", "edit-delete"), tr("&Remove"), this);
    for (QPushButton* b : {add, m_remove}) {
        b->setObjectName(QStringLiteral("flat"));
        b->setCursor(Qt::PointingHandCursor);
        left->addWidget(b);
    }
    columns->addLayout(left);

    auto* divider = new QFrame(this);
    divider->setObjectName(QStringLiteral("divider"));
    divider->setFixedWidth(1);
    columns->addWidget(divider);

    // Right: the selected one, under a heading with its folder.
    auto* right = new QVBoxLayout;
    right->setSpacing(10);
    m_heading = new QLabel(this);
    QFont hf = m_heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 1.3);
    hf.setWeight(QFont::DemiBold);
    m_heading->setFont(hf);
    m_address = new QLabel(this);
    m_address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setThemeStyle(m_address, QStringLiteral("color: @dimText;"));
    right->addWidget(m_heading);
    right->addWidget(m_address);
    m_empty = new QLabel(tr("No bookmarks yet. Add this folder, or press Ctrl+Shift+D in any folder."), this);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    setThemeStyle(m_empty, QStringLiteral("color: @dimText;"));
    right->addWidget(m_empty, 100);

    m_form = new QWidget(this);
    auto* fields = new QFormLayout(m_form);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setHorizontalSpacing(14);
    fields->setVerticalSpacing(8);
    fields->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    fields->addRow(sectionLabel(tr("Bookmark"), m_form));
    m_key = new QLineEdit(m_form);
    m_key->setObjectName(QStringLiteral("bookmarkKey"));
    m_key->setPlaceholderText(tr("one or more letters, e.g. cp; empty: no key"));
    fields->addRow(tr("Key"), m_key);
    auto* folderRow = new QWidget(m_form);
    auto* folderLayout = new QHBoxLayout(folderRow);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    folderLayout->setSpacing(6);
    m_folder = new QLineEdit(folderRow);
    m_folder->setObjectName(QStringLiteral("bookmarkFolder"));
    m_folder->setPlaceholderText(tr("a folder, or a server address"));
    auto* browse = new QPushButton(tr("Choose…"), folderRow);
    folderLayout->addWidget(m_folder, 1);
    folderLayout->addWidget(browse);
    fields->addRow(tr("Folder"), folderRow);
    auto* how = new QLabel(tr("Ctrl+D and the key go there; with modal keys, g and the key. "
                              "Drag bookmarks in the list to change their order."), m_form);
    how->setWordWrap(true);
    setThemeStyle(how, QStringLiteral("color: @dimText;"));
    fields->addRow(QString(), how);
    right->addWidget(m_form);
    right->addStretch(1);
    columns->addLayout(right, 1);

    for (const FolderHistory::Bookmark& b : FolderHistory::instance().pinnedBookmarks())
        addRow(b.key, b.path);

    connect(m_list, &QListWidget::currentItemChanged, this, &BookmarksEditor::showCurrent);
    connect(m_list, &QListWidget::itemActivated, this, [this] {
        m_key->setFocus();
        m_key->selectAll();
    });
    connect(m_key, &QLineEdit::textChanged, this, &BookmarksEditor::formChanged);
    connect(m_folder, &QLineEdit::textChanged, this, &BookmarksEditor::formChanged);
    connect(m_remove, &QPushButton::clicked, this, &BookmarksEditor::removeCurrent);
    connect(add, &QPushButton::clicked, this, [this] {
        addRow(QString(), m_currentFolder);
        m_list->setCurrentRow(m_list->count() - 1);
        m_key->setFocus();
        emit edited();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString now = pathOf(m_folder->text().trimmed());
        const QString start = now.startsWith(QLatin1Char('/')) ? now : QDir::homePath();
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder"), start);
        if (!dir.isEmpty())
            m_folder->setText(dir);
    });
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    showCurrent();
}

void BookmarksEditor::addRow(const QString& key, const QString& path)
{
    auto* item = new QListWidgetItem(m_list);
    item->setData(kKeyRole, key);
    item->setData(kPathRole, path);
    updateRow(item);
}

void BookmarksEditor::updateRow(QListWidgetItem* item)
{
    const QString path = item->data(kPathRole).toString();
    item->setText(path.isEmpty() ? tr("New bookmark") : nameOf(path));
    item->setData(Qt::UserRole, path);
    item->setData(kCardBadgeRole, item->data(kKeyRole));
    item->setIcon(IconCache::themed(path.contains(QLatin1String("://")) ? "folder-remote" : "folder", "folder"));
    item->setToolTip(path);
}

void BookmarksEditor::removeCurrent()
{
    delete m_list->currentItem();
    showCurrent();
    emit edited();
}

void BookmarksEditor::showCurrent()
{
    QListWidgetItem* item = m_list->currentItem();
    for (QWidget* w : {static_cast<QWidget*>(m_form), static_cast<QWidget*>(m_heading),
                       static_cast<QWidget*>(m_address)})
        w->setVisible(item);
    m_empty->setVisible(!item);
    m_remove->setEnabled(item);
    m_loading = true;
    m_key->setText(item ? item->data(kKeyRole).toString() : QString());
    m_folder->setText(item ? item->data(kPathRole).toString() : QString());
    m_loading = false;
    m_heading->setText(item ? item->text() : QString());
    m_address->setText(item ? item->data(kPathRole).toString() : QString());
}

void BookmarksEditor::formChanged()
{
    QListWidgetItem* item = m_list->currentItem();
    if (m_loading || !item)
        return;
    item->setData(kKeyRole, m_key->text().trimmed());
    item->setData(kPathRole, m_folder->text().trimmed());
    updateRow(item);
    m_heading->setText(item->text());
    m_address->setText(item->data(kPathRole).toString());
    emit edited();
}

bool BookmarksEditor::eventFilter(QObject* obj, QEvent* e)
{
    // Del in the list removes the selected bookmark.
    if (obj == m_list && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Delete) {
        removeCurrent();
        return true;
    }
    return QWidget::eventFilter(obj, e);
}

QString BookmarksEditor::check()
{
    QSet<QString> keys, paths;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* item = m_list->item(i);
        const QString key = item->data(kKeyRole).toString();
        const QString path = item->data(kPathRole).toString();
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
            m_list->setCurrentItem(item);
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
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem* item = m_list->item(i);
        pins.append({item->data(kKeyRole).toString(), pathOf(item->data(kPathRole).toString())});
    }
    if (pins != FolderHistory::instance().pinnedBookmarks())
        FolderHistory::instance().setBookmarks(pins);
}

}  // namespace tandem
