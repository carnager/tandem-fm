#pragma once

#include <QAbstractScrollArea>
#include <QFont>
#include <QList>

namespace tandem {

class BlockCache;

// Shared base for views that render straight from a BlockCache: font,
// zoom, scrollbar mapping for files far larger than an int.
class ByteViewBase : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit ByteViewBase(QWidget* parent = nullptr);
    void setCache(BlockCache* cache);
    void zoom(int steps);  // 0 resets
    double position() const;
    virtual qint64 topOffset() const = 0;
    virtual void showOffset(qint64 offset, qint64 len) = 0;

signals:
    void positionChanged();

protected:
    void scrollContentsBy(int, int) override {}
    void wheelEvent(QWheelEvent* e) override;
    virtual void scrollRows(int rows) = 0;
    virtual void onLoaded() { viewport()->update(); }
    virtual void onScrollBarMoved(qint64 value) = 0;
    virtual void cacheChanged() {}
    virtual void relayout() { viewport()->update(); }  // font or size changed
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;
    void updateMetrics();
    void setScrollRange(qint64 max, qint64 value, qint64 page);
    int rowsVisible() const { return qMax(1, viewport()->height() / m_lineH); }

    BlockCache* m_cache = nullptr;
    QFont m_font;
    qreal m_baseSize = 10;
    int m_zoom = 0;
    int m_charW = 8;
    int m_lineH = 16;
    int m_ascent = 12;
    qint64 m_scale = 1;
    bool m_syncing = false;
    qint64 m_hlStart = -1, m_hlLen = 0;
};

// Text of any size, rendered from the visible window only. Lines longer than
// 16 KiB are split, so even minified multi-MB one-liners stay navigable.
class TextScrollView : public ByteViewBase {
    Q_OBJECT
public:
    explicit TextScrollView(QWidget* parent = nullptr);
    void setWrap(bool on);
    bool wrap() const { return m_wrap; }
    qint64 topOffset() const override { return m_top; }
    void showOffset(qint64 offset, qint64 len) override;

protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void scrollRows(int rows) override;
    void onLoaded() override;
    void onScrollBarMoved(qint64 value) override;
    void cacheChanged() override;
    void relayout() override { setTop(m_top); }

private:
    struct Line {
        qint64 start, end, next;  // end excludes the line break
    };
    QList<Line> linesFrom(qint64 offset, int count, bool* complete) const;
    qint64 lineStartBefore(qint64 lineStart, bool* complete) const;
    qint64 alignToLine(qint64 offset, bool* complete) const;
    QString expand(const QByteArray& bytes, int startCol = 0) const;
    void setTop(qint64 top);
    void goEnd();
    void finishPending();

    qint64 m_top = 0;
    qint64 m_pendingAlign = -1;
    bool m_pendingEnd = false;
    int m_pendingBack = 0;  // lines to step back once data arrives (search context)
    int m_pendingForward = 0;  // lines to step forward once data arrives
    bool m_wrap = false;
    int m_hcol = 0;
    qint64 m_visibleBytes = 4096;
};

class HexView : public ByteViewBase {
    Q_OBJECT
public:
    explicit HexView(QWidget* parent = nullptr);
    qint64 topOffset() const override { return m_topRow * 16; }
    void showOffset(qint64 offset, qint64 len) override;

protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void scrollRows(int rows) override;
    void onScrollBarMoved(qint64 value) override;
    void cacheChanged() override;
    void relayout() override { setTopRow(m_topRow); }

private:
    qint64 rowCount() const;
    void setTopRow(qint64 row);
    qint64 m_topRow = 0;
};

}  // namespace tandem
