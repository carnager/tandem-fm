#include "ui/ThumbGrid.h"

#include "ui/FileColors.h"

#include "core/Backend.h"
#include "core/Util.h"
#include "ui/FileModel.h"
#include "ui/FileView.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"
#include "ui/Thumbnails.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>

namespace tandem {

ThumbGrid::ThumbGrid(FileView* view) : QWidget(view->viewport()), m_view(view)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::NoFocus);  // the list keeps the keys
    m_scroll = new QScrollBar(Qt::Vertical, this);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    connect(m_scroll, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    connect(&Thumbnails::instance(), &Thumbnails::ready, this, [this](const QString& path) {
        if (isVisible() && m_onScreen.contains(path))
            update();
    });
    view->viewport()->installEventFilter(this);
}

QSize ThumbGrid::cell() const
{
    const int line = fontMetrics().height();
    return {m_thumb + 16, m_thumb + 14 + 2 * line + 8};
}

int ThumbGrid::spacing() const
{
    return 8;
}

int ThumbGrid::columns() const
{
    const int usable = width() - m_scroll->width() - spacing();
    return qMax(1, usable / (cell().width() + spacing()));
}

int ThumbGrid::visibleRows() const
{
    return qMax(1, height() / (cell().height() + spacing()));
}

namespace {
int g_thumbSize = ThumbGrid::kDefaultSize;
}

void ThumbGrid::setDefaultSize(int thumb)
{
    g_thumbSize = qBound(kMinSize, thumb, kMaxSize);
}

int ThumbGrid::defaultSize()
{
    return g_thumbSize;
}

void ThumbGrid::setCellSize(int thumb)
{
    thumb = qBound(kMinSize, thumb, kMaxSize);
    if (thumb == m_thumb)
        return;
    const int row = m_view->currentRow();
    m_thumb = thumb;
    m_scaled.clear();
    relayout();
    ensureVisible(row);
    update();
}

QRect ThumbGrid::cellRect(int row) const
{
    const int cols = columns();
    const QSize c = cell();
    // Spread what's left over between the columns, so the grid fills the width.
    const int usable = width() - m_scroll->width();
    const int gap = qMax(spacing(), (usable - cols * c.width()) / (cols + 1));
    const int x = gap + (row % cols) * (c.width() + gap);
    const int y = spacing() + (row / cols) * (c.height() + spacing()) - m_scroll->value();
    return {x, y, c.width(), c.height()};
}

int ThumbGrid::rowAt(const QPoint& pos) const
{
    const FileModel* m = m_view->fileModel();
    if (!m)
        return -1;
    const int cols = columns();
    const int r = (pos.y() + m_scroll->value() - spacing()) / (cell().height() + spacing());
    for (int c = 0; c < cols; ++c) {
        const int row = r * cols + c;
        if (row < m->count() && cellRect(row).contains(pos))
            return row;
    }
    return -1;
}

void ThumbGrid::relayout()
{
    const FileModel* m = m_view->fileModel();
    const int n = m ? m->count() : 0;
    const int rows = (n + columns() - 1) / columns();
    const int content = spacing() + rows * (cell().height() + spacing());
    m_scroll->setGeometry(width() - m_scroll->sizeHint().width(), 0, m_scroll->sizeHint().width(), height());
    m_scroll->setRange(0, qMax(0, content - height()));
    m_scroll->setPageStep(height());
    m_scroll->setSingleStep(cell().height() / 3);
}

void ThumbGrid::ensureVisible(int row)
{
    if (row < 0)
        return;
    relayout();
    const QRect r = cellRect(row).translated(0, m_scroll->value());
    if (r.top() < m_scroll->value() + spacing())
        m_scroll->setValue(r.top() - spacing());
    else if (r.bottom() > m_scroll->value() + height() - spacing())
        m_scroll->setValue(r.bottom() - height() + spacing());
    update();
}

void ThumbGrid::watchModel()
{
    FileModel* m = m_view->fileModel();
    if (m == m_model)
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    m_model = m;
    m_scaled.clear();
    if (!m)
        return;
    auto refresh = [this] {
        relayout();
        update();
    };
    connect(m, &QAbstractItemModel::modelReset, this, refresh);
    connect(m, &QAbstractItemModel::layoutChanged, this, refresh);
    connect(m, &QAbstractItemModel::dataChanged, this, qOverload<>(&QWidget::update));
    connect(m, &QAbstractItemModel::rowsInserted, this, [this] {
        relayout();
        update();
    });
    connect(m, &QAbstractItemModel::rowsRemoved, this, [this] {
        relayout();
        update();
    });
}

bool ThumbGrid::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_view->viewport() && e->type() == QEvent::Resize)
        setGeometry(m_view->viewport()->rect());  // always covers the list
    return QWidget::eventFilter(obj, e);
}

void ThumbGrid::resizeEvent(QResizeEvent*)
{
    relayout();
}

void ThumbGrid::paintEvent(QPaintEvent*)
{
    watchModel();
    const Theme& t = theme();
    QPainter p(this);
    p.fillRect(rect(), t.base);
    const FileModel* m = m_view->fileModel();
    if (!m)
        return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const int cols = columns();
    const int rowH = cell().height() + spacing();
    const int firstRow = qMax(0, (m_scroll->value() - spacing()) / rowH);
    const int lastRow = (m_scroll->value() + height()) / rowH + 1;
    const int current = m_view->currentRow();
    const bool active = m_view->isActive();
    // Thumbnails of real files only (not inside archives or on servers).
    const bool local = !isRemoteLocation(m->path()) && QFileInfo(m->path()).isDir();
    const QFontMetrics fm = fontMetrics();
    m_onScreen.clear();

    for (int row = firstRow * cols; row < qMin(m->count(), (lastRow + 1) * cols); ++row) {
        const FileEntry* e = m->entry(row);
        if (!e)
            continue;
        const QRect r = cellRect(row);
        p.setOpacity(1.0);
        if (row == current) {
            QColor c = active ? t.cursorActive : t.cursorInactive;
            p.setPen(active ? QPen(t.accent, 1.2) : Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        } else if (e->marked) {
            p.setPen(Qt::NoPen);
            p.setBrush(t.markBg);
            p.drawRoundedRect(QRectF(r), 8, 8);
        }
        p.setOpacity(1.0 - m_view->fade());  // the cursor stays clear, the rest fades
        // The picture: a thumbnail when there is one, else the file's icon.
        const QRect pic(r.left() + 8, r.top() + 8, m_thumb, m_thumb);
        QPixmap px;
        if (local && !e->isDir && !e->isParent && Thumbnails::canThumbnail(e->name)) {
            const QString path = m->filePath(row);
            m_onScreen.insert(path);
            const QString key = path + QLatin1Char('|') + QString::number(e->mtime);
            auto it = m_scaled.find(key);
            if (it != m_scaled.end()) {
                px = *it;
            } else {
                const QImage img = Thumbnails::instance().get(path, e->mtime);
                if (!img.isNull()) {
                    px = QPixmap::fromImage(img.scaled(m_thumb * devicePixelRatio(), m_thumb * devicePixelRatio(),
                                                       Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    px.setDevicePixelRatio(devicePixelRatio());
                    m_scaled.insert(key, px);
                }
            }
        }
        if (!px.isNull()) {
            const QSizeF s = px.deviceIndependentSize();
            const QRectF target(pic.left() + (pic.width() - s.width()) / 2, pic.top() + (pic.height() - s.height()) / 2,
                                s.width(), s.height());
            QPainterPath clip;
            clip.addRoundedRect(target, 5, 5);
            p.save();
            p.setClipPath(clip);
            p.drawPixmap(target.toRect(), px);
            p.restore();
        } else {
            const int is = qMin(96, m_thumb * 2 / 3);
            const QIcon icon = e->isParent ? IconCache::themed("go-up", "go-previous")
                                         : m->data(m->index(row, FileModel::ColName), Qt::DecorationRole).value<QIcon>();
            icon.paint(&p, QRect(pic.center().x() - is / 2, pic.center().y() - is / 2, is, is));
        }
        // The name, on up to two lines.
        const QString name = e->isParent ? QStringLiteral("..") : e->name;
        const QRect text(r.left() + 6, pic.bottom() + 6, r.width() - 12, 2 * fm.height());
        const auto typeColor = e->marked ? std::nullopt : fileColor(*e);
        p.setPen(e->marked ? t.mark : typeColor.value_or(t.text));
        QString first = name, second;
        if (fm.horizontalAdvance(name) > text.width()) {
            int cut = int(name.size());
            while (cut > 1 && fm.horizontalAdvance(name.left(cut)) > text.width())
                --cut;
            first = name.left(cut);
            second = fm.elidedText(name.mid(cut), Qt::ElideMiddle, text.width());
        }
        p.drawText(QRect(text.left(), text.top(), text.width(), fm.height()), Qt::AlignHCenter | Qt::AlignVCenter, first);
        if (!second.isEmpty())
            p.drawText(QRect(text.left(), text.top() + fm.height(), text.width(), fm.height()),
                       Qt::AlignHCenter | Qt::AlignVCenter, second);
    }
    Thumbnails::instance().keepOnly(m_onScreen);
}

void ThumbGrid::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {  // Ctrl+wheel: bigger or smaller, both panels alike
        if (const int dy = e->angleDelta().y())
            emit m_view->zoomStepRequested(dy > 0 ? 1 : -1);
        return;
    }
    m_scroll->setValue(m_scroll->value() - e->angleDelta().y());
}

void ThumbGrid::mousePressEvent(QMouseEvent* e)
{
    m_view->setFocus(Qt::MouseFocusReason);
    if (e->button() == Qt::BackButton || e->button() == Qt::ForwardButton) {
        emit m_view->historyRequested(e->button() == Qt::BackButton ? -1 : 1);
        return;
    }
    const int row = rowAt(e->position().toPoint());
    FileModel* m = m_view->fileModel();
    if (row < 0 || !m || e->button() != Qt::LeftButton)
        return;
    if (e->modifiers() & Qt::ControlModifier) {
        m->toggleMarked(row);
    } else if (e->modifiers() & Qt::ShiftModifier) {
        for (int r = qMin(m_view->currentRow(), row); r <= qMax(m_view->currentRow(), row); ++r)
            m->setMarked(r, true);
    } else {
        m_pressPos = e->position().toPoint();
        m_maybeDrag = true;
    }
    m_view->setCurrentRow(row);
}

void ThumbGrid::mouseMoveEvent(QMouseEvent* e)
{
    if (m_maybeDrag && (e->buttons() & Qt::LeftButton) &&
        (e->position().toPoint() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_maybeDrag = false;
        m_view->startFileDrag();
    }
}

void ThumbGrid::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && rowAt(e->position().toPoint()) >= 0)
        emit m_view->openRequested();
}

void ThumbGrid::contextMenuEvent(QContextMenuEvent* e)
{
    const int row = rowAt(e->pos());
    if (row >= 0)
        m_view->setCurrentRow(row);
    emit m_view->contextMenuRequested(e->globalPos(), row);
}

}  // namespace tandem
