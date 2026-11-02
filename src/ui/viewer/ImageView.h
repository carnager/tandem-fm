#pragma once

#include <QAbstractScrollArea>
#include <QImage>
#include <QPixmap>

namespace tandem {

// Shows an already decoded image: fit to window by default, zoom with
// +/- or Ctrl+wheel, drag or arrow keys to pan.
class ImageView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit ImageView(QWidget* parent = nullptr);
    void setImage(const QImage& image);
    const QImage& image() const { return m_image; }
    void zoom(int steps);  // 0 = back to fit
    void setBackground(const QColor& c) { m_background = c; viewport()->update(); }
    void setPadding(int px) { m_padding = px; relayout(); }
    double scale() const;

signals:
    void scaleChanged();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void scrollContentsBy(int, int) override { viewport()->update(); }

private:
    void relayout();

    QImage m_image;
    QPixmap m_scaled;
    double m_zoom = 1.0;
    bool m_fit = true;
    QPoint m_dragFrom;
    QColor m_background;
    int m_padding = 0;
};

}  // namespace tandem
