#include "ui/viewer/ImageView.h"

#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <cmath>

namespace tandem {

ImageView::ImageView(QWidget* parent) : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::OpenHandCursor);
}

void ImageView::setImage(const QImage& image)
{
    m_image = image;
    m_scaled = QPixmap();  // a new picture of the same size must not reuse the old one
    m_live = false;
    m_fit = true;
    m_zoom = 1.0;
    relayout();
}

void ImageView::setFrame(const QImage& frame)
{
    const bool resized = frame.size() != m_image.size() || !m_live;
    m_image = frame;
    m_scaled = QPixmap();
    m_live = true;
    if (resized)
        relayout();
    else
        viewport()->update();
}

QSize ImageView::shownSize() const
{
    return m_image.isNull() ? QSize() : (QSizeF(m_image.size()) * scale()).toSize();
}

double ImageView::scale() const
{
    if (m_image.isNull())
        return 1.0;
    if (!m_fit)
        return m_zoom;
    const double sx = double(viewport()->width() - 2 * m_padding) / m_image.width();
    const double sy = double(viewport()->height() - 2 * m_padding) / m_image.height();
    return std::min({1.0, sx, sy});  // never blow small images up when fitting
}

void ImageView::zoom(int steps)
{
    if (steps == 0) {
        m_fit = true;
    } else {
        const double current = scale();
        m_fit = false;
        // Cap the scaled pixmap at ~100 megapixels to keep memory sane.
        const double maxZoom = std::min(32.0, std::sqrt(1e8 / (double(m_image.width()) * m_image.height())));
        m_zoom = std::clamp(current * std::pow(1.25, steps), 0.02, std::max(0.02, maxZoom));
    }
    relayout();
}

void ImageView::relayout()
{
    const double s = scale();
    const QSize size = shownSize();
    if (!m_live && !m_image.isNull() && m_scaled.size() != size)
        m_scaled = QPixmap::fromImage(m_image.scaled(size, Qt::KeepAspectRatio,
                                                     s < 1.0 ? Qt::SmoothTransformation : Qt::FastTransformation));
    horizontalScrollBar()->setRange(0, qMax(0, size.width() - viewport()->width()));
    verticalScrollBar()->setRange(0, qMax(0, size.height() - viewport()->height()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    verticalScrollBar()->setPageStep(viewport()->height());
    emit scaleChanged();
    viewport()->update();
}

void ImageView::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    relayout();
}

void ImageView::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), m_background.isValid() ? m_background : t.window);
    if (m_live ? m_image.isNull() : m_scaled.isNull())
        return;
    const QSize s = m_live ? shownSize() : m_scaled.size();
    const int x = s.width() < viewport()->width() ? (viewport()->width() - s.width()) / 2 : -horizontalScrollBar()->value();
    const int y = s.height() < viewport()->height() ? (viewport()->height() - s.height()) / 2 : -verticalScrollBar()->value();
    const QRect target(QPoint(x, y), s);
    if (!m_live && m_image.hasAlphaChannel()) {
        // Checkerboard so transparency is visible.
        const int c = 8;
        const QColor a = t.base, b = t.altBase.lighter(t.dark ? 140 : 96);
        p.save();
        p.setClipRect(target);
        for (int yy = target.top(); yy < target.bottom(); yy += c)
            for (int xx = target.left(); xx < target.right(); xx += c)
                p.fillRect(xx, yy, c, c, (((xx - target.left()) / c + (yy - target.top()) / c) % 2) ? a : b);
        p.restore();
    }
    if (m_live) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, m_image);
    } else {
        p.drawPixmap(target.topLeft(), m_scaled);
    }
}

void ImageView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        zoom(e->angleDelta().y() > 0 ? 1 : -1);
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void ImageView::mousePressEvent(QMouseEvent* e)
{
    m_dragFrom = e->position().toPoint();
}

void ImageView::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & Qt::LeftButton))
        return;
    const QPoint d = e->position().toPoint() - m_dragFrom;
    m_dragFrom = e->position().toPoint();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
}

}  // namespace tandem
