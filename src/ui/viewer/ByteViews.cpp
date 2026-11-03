#include "ui/viewer/ByteViews.h"

#include "ui/Theme.h"
#include "ui/viewer/BlockCache.h"

#include <QFontDatabase>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <climits>

namespace tandem {

namespace {
constexpr qint64 kMaxLine = 16 * 1024;
constexpr int kMargin = 8;
constexpr int kTab = 8;
}  // namespace

// ------------------------------------------------------------ ByteViewBase

ByteViewBase::ByteViewBase(QWidget* parent) : QAbstractScrollArea(parent)
{
    m_font = monoFont();
    m_baseSize = m_font.pointSizeF() > 0 ? m_font.pointSizeF() : 10;
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    updateMetrics();
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_syncing)
            onScrollBarMoved(qint64(v) * m_scale);
    });
}

void ByteViewBase::changeEvent(QEvent* event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        m_font = monoFont();
        m_baseSize = m_font.pointSizeF() > 0 ? m_font.pointSizeF() : 10;
        updateMetrics();
        relayout();
    }
}

void ByteViewBase::setCache(BlockCache* cache)
{
    if (m_cache)
        disconnect(m_cache, nullptr, this, nullptr);
    m_cache = cache;
    m_hlStart = -1;
    if (m_cache)
        connect(m_cache, &BlockCache::loaded, this, [this] { onLoaded(); });
    cacheChanged();
    viewport()->update();
}

void ByteViewBase::zoom(int steps)
{
    m_zoom = steps == 0 ? 0 : qBound(-6, m_zoom + steps, 20);
    updateMetrics();
    relayout();
}

void ByteViewBase::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    relayout();
}

void ByteViewBase::updateMetrics()
{
    m_font.setPointSizeF(qMax<qreal>(5, m_baseSize + m_zoom));
    const QFontMetrics fm(m_font);
    m_charW = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_lineH = fm.lineSpacing() + 1;
    m_ascent = fm.ascent();
}

double ByteViewBase::position() const
{
    if (!m_cache || m_cache->size() == 0)
        return 0;
    return double(topOffset()) / double(m_cache->size());
}

void ByteViewBase::setScrollRange(qint64 max, qint64 value, qint64 page)
{
    m_scale = max / INT_MAX + 1;
    m_syncing = true;
    verticalScrollBar()->setRange(0, int(max / m_scale));
    verticalScrollBar()->setPageStep(int(qMax<qint64>(1, page / m_scale)));
    verticalScrollBar()->setValue(int(value / m_scale));
    m_syncing = false;
    emit positionChanged();
}

void ByteViewBase::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        zoom(e->angleDelta().y() > 0 ? 1 : -1);
        return;
    }
    const int steps = -e->angleDelta().y() / 40;  // 3 rows per notch
    if (steps)
        scrollRows(steps);
}

// ---------------------------------------------------------- TextScrollView

TextScrollView::TextScrollView(QWidget* parent) : ByteViewBase(parent) {}

void TextScrollView::cacheChanged()
{
    m_top = 0;
    m_hcol = 0;
    m_pendingAlign = -1;
    m_pendingEnd = false;
    m_pendingBack = 0;
    m_pendingForward = 0;
    setTop(0);
}

void TextScrollView::setWrap(bool on)
{
    m_wrap = on;
    m_hcol = 0;
    viewport()->update();
}

QList<TextScrollView::Line> TextScrollView::linesFrom(qint64 offset, int count, bool* complete) const
{
    QList<Line> out;
    bool ok = true;
    if (!m_cache) {
        *complete = true;
        return out;
    }
    const qint64 size = m_cache->size();
    qint64 pos = offset;
    while (out.size() < count && pos < size) {
        bool c = false;
        const QByteArray d = m_cache->read(pos, kMaxLine + 1, &c);
        const qsizetype nl = d.indexOf('\n');
        if (nl >= 0) {
            qint64 end = pos + nl;
            if (nl > 0 && d[nl - 1] == '\r')
                --end;
            out.append({pos, end, pos + nl + 1});
            pos += nl + 1;
        } else if (d.size() > kMaxLine) {
            out.append({pos, pos + kMaxLine, pos + kMaxLine});
            pos += kMaxLine;
        } else if (c) {
            out.append({pos, pos + d.size(), pos + d.size()});  // last line, no newline
            pos += d.size();
        } else {
            ok = false;
            break;
        }
    }
    *complete = ok;
    return out;
}

qint64 TextScrollView::lineStartBefore(qint64 lineStart, bool* complete) const
{
    *complete = true;
    if (lineStart <= 0 || !m_cache)
        return 0;
    // Bytes before the break that ended the previous line.
    const qint64 to = lineStart - 1;
    const qint64 from = qMax<qint64>(0, to - kMaxLine);
    bool c = false;
    const QByteArray d = m_cache->read(from, to - from, &c);
    if (!c) {
        *complete = false;
        return lineStart;
    }
    const qsizetype nl = d.lastIndexOf('\n');
    if (nl >= 0)
        return from + nl + 1;
    return from == 0 ? 0 : to - kMaxLine + 1;
}

qint64 TextScrollView::alignToLine(qint64 offset, bool* complete) const
{
    *complete = true;
    if (offset <= 0 || !m_cache)
        return 0;
    const qint64 from = qMax<qint64>(0, offset - kMaxLine);
    bool c = false;
    const QByteArray d = m_cache->read(from, offset - from, &c);
    if (!c) {
        *complete = false;
        return offset;
    }
    const qsizetype nl = d.lastIndexOf('\n');
    if (nl >= 0)
        return from + nl + 1;
    return from == 0 ? 0 : offset;
}

QString TextScrollView::expand(const QByteArray& bytes, int startCol) const
{
    const QString s = QString::fromUtf8(bytes);
    QString out;
    out.reserve(s.size() + 8);
    int col = startCol;
    for (QChar ch : s) {
        if (ch == QLatin1Char('\t')) {
            const int n = kTab - (col % kTab);
            out.append(QString(n, QLatin1Char(' ')));
            col += n;
        } else if (ch.unicode() < 0x20) {
            out.append(QChar(0x00B7));  // visible placeholder for control bytes
            ++col;
        } else {
            out.append(ch);
            ++col;
        }
    }
    return out;
}

void TextScrollView::setTop(qint64 top)
{
    m_top = qBound<qint64>(0, top, m_cache ? m_cache->size() : 0);
    setScrollRange(m_cache ? m_cache->size() : 0, m_top, m_visibleBytes);
    viewport()->update();
}

void TextScrollView::scrollRows(int rows)
{
    if (!m_cache || rows == 0)
        return;
    if (rows > 0) {
        bool c = false;
        const int visible = rowsVisible();
        const QList<Line> lines = linesFrom(m_top, rows + visible, &c);
        if (!c && lines.size() <= rows) {
            m_pendingForward += rows;  // not loaded yet: scroll once it is
            return;
        }
        // Don't scroll past the point where the last page is full.
        const int maxStep = c ? qMax(0, int(lines.size()) - visible) : int(lines.size()) - 1;
        const int step = qMin(rows, maxStep);
        if (step > 0)
            setTop(lines[step].start);
        return;
    }
    qint64 t = m_top;
    for (int i = 0; i < -rows && t > 0; ++i) {
        bool c = false;
        const qint64 prev = lineStartBefore(t, &c);
        if (!c) {
            m_pendingBack = -rows - i;
            break;
        }
        t = prev;
    }
    setTop(t);
}

void TextScrollView::goEnd()
{
    if (!m_cache)
        return;
    bool c = false;
    qint64 t = alignToLine(m_cache->size(), &c);
    for (int i = 0; c && i < rowsVisible() - 1 && t > 0; ++i)
        t = lineStartBefore(t, &c);
    m_pendingEnd = !c;
    setTop(t);
}

void TextScrollView::onScrollBarMoved(qint64 value)
{
    bool c = false;
    const qint64 t = alignToLine(value, &c);
    m_pendingAlign = c ? -1 : value;
    m_top = t;
    emit positionChanged();
    viewport()->update();
}

void TextScrollView::finishPending()
{
    if (m_pendingAlign >= 0) {
        bool c = false;
        const qint64 t = alignToLine(m_pendingAlign, &c);
        if (c) {
            m_pendingAlign = -1;
            m_top = t;
        }
    }
    if (m_pendingEnd)
        goEnd();
    if (m_pendingBack > 0 && m_pendingAlign < 0) {
        const int n = std::exchange(m_pendingBack, 0);
        scrollRows(-n);
    }
    if (m_pendingForward > 0 && m_pendingAlign < 0) {
        const int n = std::exchange(m_pendingForward, 0);
        scrollRows(n);
    }
}

void TextScrollView::onLoaded()
{
    finishPending();
    viewport()->update();
}

void TextScrollView::showOffset(qint64 offset, qint64 len)
{
    m_hlStart = offset;
    m_hlLen = len;
    bool c = false;
    const qint64 t = alignToLine(offset, &c);
    m_pendingAlign = c ? -1 : offset;
    m_top = t;
    m_pendingBack = rowsVisible() / 3;
    if (!m_wrap && c) {
        // Bring the match into view horizontally.
        const int col = int(expand(m_cache->read(t, offset - t)).size());
        const int cols = qMax(1, (viewport()->width() - 2 * kMargin) / m_charW);
        if (col < m_hcol || col > m_hcol + cols - 10)
            m_hcol = qMax(0, col - 10);
    }
    finishPending();
    setTop(m_top);
}

void TextScrollView::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), t.base);
    if (!m_cache)
        return;
    p.setFont(m_font);
    const int cols = qMax(1, (viewport()->width() - 2 * kMargin) / m_charW);
    const int rows = rowsVisible() + 1;
    bool complete = false;
    const QList<Line> lines = linesFrom(m_top, rows, &complete);

    int y = 0;
    qint64 lastShown = m_top;
    for (const Line& line : lines) {
        if (y >= viewport()->height())
            break;
        const QString s = expand(m_cache->read(line.start, line.end - line.start));
        int hlFrom = -1, hlTo = -1;
        if (m_hlStart >= 0 && m_hlStart < line.end + 1 && m_hlStart + m_hlLen > line.start) {
            const qint64 a = qMax(m_hlStart, line.start);
            const qint64 b = qMin(m_hlStart + m_hlLen, line.end);
            hlFrom = int(expand(m_cache->read(line.start, a - line.start)).size());
            hlTo = hlFrom + int(expand(m_cache->read(a, b - a), hlFrom).size());
        }
        auto drawRow = [&](const QString& text, int firstCol) {
            if (hlFrom >= 0) {
                const int a = qMax(hlFrom, firstCol) - firstCol;
                const int b = qMin(hlTo, firstCol + int(text.size())) - firstCol;
                if (b > a)
                    p.fillRect(QRect(kMargin + a * m_charW, y, (b - a) * m_charW, m_lineH), t.mark);
            }
            p.setPen(t.text);
            p.drawText(kMargin, y + m_ascent, text);
            y += m_lineH;
        };
        if (m_wrap) {
            for (int i = 0; i == 0 || i < s.size(); i += cols) {
                drawRow(s.mid(i, cols), i);
                if (y >= viewport()->height())
                    break;
            }
        } else {
            drawRow(s.mid(m_hcol, cols + 1), m_hcol);
        }
        lastShown = line.next;
    }
    if (!complete && y < viewport()->height()) {
        p.setPen(t.dimText);
        p.drawText(kMargin, y + m_ascent, tr("Loading…"));
    }
    if (lastShown > m_top && lastShown - m_top != m_visibleBytes) {
        m_visibleBytes = lastShown - m_top;
        m_syncing = true;
        verticalScrollBar()->setPageStep(int(qMax<qint64>(1, m_visibleBytes / m_scale)));
        m_syncing = false;
    }
}

void TextScrollView::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Down:
        scrollRows(1);
        return;
    case Qt::Key_Up:
        scrollRows(-1);
        return;
    case Qt::Key_PageDown:
        scrollRows(rowsVisible() - 1);
        return;
    case Qt::Key_PageUp:
        scrollRows(-(rowsVisible() - 1));
        return;
    case Qt::Key_Home:
        setTop(0);
        return;
    case Qt::Key_End:
        goEnd();
        return;
    case Qt::Key_Right:
        if (!m_wrap) {
            m_hcol += 8;
            viewport()->update();
        }
        return;
    case Qt::Key_Left:
        if (!m_wrap) {
            m_hcol = qMax(0, m_hcol - 8);
            viewport()->update();
        }
        return;
    default:
        ByteViewBase::keyPressEvent(e);
    }
}

// ---------------------------------------------------------------- HexView

HexView::HexView(QWidget* parent) : ByteViewBase(parent) {}

qint64 HexView::rowCount() const
{
    return m_cache ? (m_cache->size() + 15) / 16 : 0;
}

void HexView::cacheChanged()
{
    setTopRow(0);
}

void HexView::setTopRow(qint64 row)
{
    const qint64 maxTop = qMax<qint64>(0, rowCount() - rowsVisible());
    m_topRow = qBound<qint64>(0, row, maxTop);
    setScrollRange(maxTop, m_topRow, rowsVisible());
    viewport()->update();
}

void HexView::scrollRows(int rows)
{
    setTopRow(m_topRow + rows);
}

void HexView::onScrollBarMoved(qint64 value)
{
    m_topRow = value;
    emit positionChanged();
    viewport()->update();
}

void HexView::showOffset(qint64 offset, qint64 len)
{
    m_hlStart = offset;
    m_hlLen = len;
    setTopRow(offset / 16 - rowsVisible() / 3);
}

void HexView::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), t.base);
    if (!m_cache)
        return;
    p.setFont(m_font);
    int digits = 8;
    while (digits < 16 && (m_cache->size() >> (digits * 4)) > 0)
        ++digits;
    const int hexX = kMargin + (digits + 2) * m_charW;
    const int asciiX = hexX + (16 * 3 + 2) * m_charW;
    const int rows = rowsVisible() + 1;

    bool complete = false;
    const qint64 start = m_topRow * 16;
    const QByteArray data = m_cache->read(start, qint64(rows) * 16, &complete);
    int y = 0;
    for (int r = 0; r < rows; ++r, y += m_lineH) {
        const qint64 off = start + qint64(r) * 16;
        if (off >= m_cache->size())
            break;
        p.setPen(t.dimText);
        p.drawText(kMargin, y + m_ascent, QStringLiteral("%1").arg(off, digits, 16, QLatin1Char('0')));
        const int have = int(qMin<qint64>(16, data.size() - qint64(r) * 16));
        if (have <= 0) {
            p.drawText(hexX, y + m_ascent, tr("Loading…"));
            break;
        }
        for (int i = 0; i < have; ++i) {
            const qint64 abs = off + i;
            const auto byte = uchar(data[r * 16 + i]);
            const int hx = hexX + (i * 3 + (i >= 8 ? 1 : 0)) * m_charW;
            const int ax = asciiX + i * m_charW;
            if (m_hlStart >= 0 && abs >= m_hlStart && abs < m_hlStart + m_hlLen) {
                p.fillRect(QRect(hx, y, 2 * m_charW, m_lineH), t.mark);
                p.fillRect(QRect(ax, y, m_charW, m_lineH), t.mark);
            }
            p.setPen(byte == 0 ? t.dimText : t.text);
            p.drawText(hx, y + m_ascent, QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')));
            p.setPen(byte >= 0x20 && byte < 0x7f ? t.text : t.dimText);
            p.drawText(ax, y + m_ascent, QString(byte >= 0x20 && byte < 0x7f ? QChar(byte) : QChar(0x00B7)));
        }
    }
}

void HexView::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Down:
        scrollRows(1);
        return;
    case Qt::Key_Up:
        scrollRows(-1);
        return;
    case Qt::Key_PageDown:
        scrollRows(rowsVisible() - 1);
        return;
    case Qt::Key_PageUp:
        scrollRows(-(rowsVisible() - 1));
        return;
    case Qt::Key_Home:
        setTopRow(0);
        return;
    case Qt::Key_End:
        setTopRow(rowCount());
        return;
    default:
        ByteViewBase::keyPressEvent(e);
    }
}

}  // namespace tandem
