#include "ui/FuzzyPopup.h"

#include "core/Fuzzy.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QTextLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <numeric>

namespace tandem {

namespace {
constexpr int kMaxRows = 400;
constexpr int kVisibleRows = 11;
}  // namespace

// ------------------------------------------------------------- PopupModel

void PopupModel::set(const QList<PopupItem>* items, QList<Row> rows)
{
    beginResetModel();
    m_items = items;
    m_rows = std::move(rows);
    endResetModel();
}

int PopupModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant PopupModel::data(const QModelIndex& index, int role) const
{
    const PopupItem* it = item(index.row());
    if (!it)
        return {};
    if (role == Qt::DisplayRole)
        return it->title;
    if (role == Qt::DecorationRole)
        return it->icon;
    return {};
}

const PopupItem* PopupModel::item(int row) const
{
    if (!m_items || row < 0 || row >= m_rows.size())
        return nullptr;
    return &(*m_items)[m_rows[row].item];
}

const QList<int>& PopupModel::positions(int row) const
{
    return m_rows[row].positions;
}

// ---------------------------------------------------------- PopupDelegate

QSize PopupDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    const auto* model = static_cast<const PopupModel*>(idx.model());
    const PopupItem* it = model->item(idx.row());
    const int line = opt.fontMetrics.height();
    const int header = model->header(idx.row()).isEmpty() ? 0 : line + 14;
    (void)it;
    return {200, line + 14 + header};  // one line each: the subtitle sits beside the title
}

static void drawHighlighted(QPainter* p, const QRect& r, const QString& text, const QList<int>& positions,
                            const QFont& font, const QColor& color, const QColor& hl)
{
    const QString shown = QFontMetrics(font).elidedText(text, Qt::ElideMiddle, r.width());
    QTextLayout layout(shown, font);
    if (shown == text && !positions.isEmpty()) {
        QList<QTextLayout::FormatRange> ranges;
        QTextCharFormat f;
        f.setForeground(hl);
        f.setFontWeight(QFont::Bold);
        for (int pos : positions)
            if (pos < shown.size())
                ranges.append({pos, 1, f});
        layout.setFormats(ranges);
    }
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(r.width());
    layout.endLayout();
    p->setPen(color);
    layout.draw(p, QPointF(r.left(), r.top() + (r.height() - line.height()) / 2));
}

namespace {
int badgeHue(const QString& badge)
{
    static const QHash<QString, int> known = {
        {QStringLiteral("SSH"), 210}, {QStringLiteral("SMB"), 28},   {QStringLiteral("DAV"), 280},
        {QStringLiteral("FTP"), 130}, {QStringLiteral("NFS"), 175},  {QStringLiteral("MTP"), 330},
    };
    if (auto h = known.find(badge.toUpper()); h != known.end())
        return *h;
    return int(qHash(badge.toUpper()) % 360);
}
}  // namespace

void PopupDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    const auto* model = static_cast<const PopupModel*>(idx.model());
    const PopupItem* it = model->item(idx.row());
    if (!it)
        return;
    const Theme& t = theme();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QStyleOptionViewItem o = opt;
    if (const QString header = model->header(idx.row()); !header.isEmpty()) {
        const int h = opt.fontMetrics.height() + 14;
        QFont f = opt.font;
        f.setPointSizeF(f.pointSizeF() * 0.85);
        f.setBold(true);
        f.setLetterSpacing(QFont::PercentageSpacing, 108);
        p->setFont(f);
        p->setPen(t.dimText);
        p->drawText(QRect(opt.rect.left() + 14, opt.rect.top(), opt.rect.width() - 28, h - 2),
                    Qt::AlignLeft | Qt::AlignBottom, header.toUpper());
        if (idx.row() > 0) {
            p->setPen(QPen(t.border, 1));
            p->drawLine(opt.rect.left() + 10, opt.rect.top() + 3, opt.rect.right() - 10, opt.rect.top() + 3);
        }
        o.rect.setTop(o.rect.top() + h);
        p->setFont(opt.font);
    }
    const QStyleOptionViewItem& opt2 = o;
    if (opt.state & QStyle::State_Selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(t.cursorActive);
        p->drawRoundedRect(QRectF(opt2.rect).adjusted(4, 1, -4, -1), 6, 6);
    }
    QRect r = opt2.rect.adjusted(14, 6, -14, -6);
    if (!it->icon.isNull()) {
        const int s = 18;
        it->icon.paint(p, QRect(r.left(), r.top() + (r.height() - s) / 2, s, s));
        r.setLeft(r.left() + s + 10);
    }
    // Badge (kind of place) as a tinted tag at the right edge; its hue
    // follows the text, so the same kind always looks the same.
    if (!it->badge.isEmpty()) {
        QFont bf = opt.font;
        bf.setPointSizeF(bf.pointSizeF() * 0.78);
        bf.setBold(true);
        bf.setLetterSpacing(QFont::PercentageSpacing, 106);
        const QFontMetrics bfm(bf);
        const int w = qMax(bfm.horizontalAdvance(it->badge) + 12, bfm.height() * 3);
        const QRect tag(r.right() - w, r.top() + (r.height() - bfm.height() - 4) / 2, w, bfm.height() + 4);
        const int hue = badgeHue(it->badge);
        QColor bg = QColor::fromHsl(hue, 150, t.dark ? 120 : 150);
        bg.setAlpha(t.dark ? 70 : 55);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(QRectF(tag), 4, 4);
        p->setFont(bf);
        p->setPen(QColor::fromHsl(hue, 120, t.dark ? 195 : 60));
        p->drawText(tag, Qt::AlignCenter, it->badge);
        p->setFont(opt.font);
        r.setRight(tag.left() - 10);
    }
    // Hint (shortcut) as a small key cap on the right.
    if (!it->hint.isEmpty() && (!it->hintWhenCurrent || (opt.state & QStyle::State_Selected))) {
        QFont hf = opt.font;
        hf.setPointSizeF(hf.pointSizeF() * 0.9);
        const QFontMetrics hfm(hf);
        const int w = hfm.horizontalAdvance(it->hint) + 14;
        const QRect cap(r.right() - w, r.top() + (r.height() - hfm.height() - 6) / 2, w, hfm.height() + 6);
        p->setPen(QPen(t.border, 1));
        p->setBrush(t.window);
        p->drawRoundedRect(QRectF(cap).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        p->setFont(hf);
        p->setPen(t.dimText);
        p->drawText(cap, Qt::AlignCenter, it->hint);
        r.setRight(cap.left() - 10);
    }
    const QList<int>& pos = model->positions(idx.row());
    const int line = opt.fontMetrics.height();
    if (it->subtitle.isEmpty()) {
        drawHighlighted(p, r, it->title, pos, opt.font, t.text, t.accent);
    } else {
        // Title, then the subtitle (category or path) dimmed beside it.
        QFont sub = opt.font;
        sub.setPointSizeF(sub.pointSizeF() * 0.92);
        const int titleWidth = qMin(opt.fontMetrics.horizontalAdvance(it->title) + 2, r.width() * 4 / 5);
        const QRect first(r.left(), r.top(), titleWidth, r.height());
        const QRect second(r.left() + titleWidth + 12, r.top(), qMax(0, r.width() - titleWidth - 12), r.height());
        drawHighlighted(p, first, it->title, it->matchSubtitle ? QList<int>{} : pos, opt.font, t.text, t.accent);
        drawHighlighted(p, second, it->subtitle, it->matchSubtitle ? pos : QList<int>{}, sub, t.dimText, t.accent);
        (void)line;
    }
    p->restore();
}

// ------------------------------------------------------------- FuzzyPopup

FuzzyPopup::FuzzyPopup(QWidget* window) : QFrame(window)
{
    const Theme& t = theme();
    setObjectName(QStringLiteral("fuzzyPopup"));
    setThemeStyle(this, QStringLiteral(
        "#fuzzyPopup { background: @header; border: 1px solid @border; border-radius: 12px; }"
        "QLineEdit { background: @base; color: @text; border: 1px solid @border; border-radius: 8px;"
        "  padding: 7px 10px; }"
        "QLineEdit:focus { border-color: @accent; }"
        "QListView { background: transparent; border: none; outline: none; }"
        "QLabel { color: @dimText; }"));
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(32);
    shadow->setOffset(0, 8);
    shadow->setColor(QColor(0, 0, 0, t.dark ? 160 : 70));
    setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 8);
    layout->setSpacing(8);
    m_edit = new QLineEdit(this);
    m_edit->installEventFilter(this);
    // A little larger than the list. Relative, not "11pt" in the style:
    // macOS counts 72 points to the inch, where 11pt is smaller than its 13pt.
    QFont editFont = font();
    if (editFont.pointSizeF() > 0)
        editFont.setPointSizeF(editFont.pointSizeF() * 1.1);
    else
        editFont.setPixelSize(qRound(editFont.pixelSize() * 1.1));
    m_edit->setFont(editFont);
    layout->addWidget(m_edit);
    m_model = new PopupModel(this);
    m_list = new QListView(this);
    m_list->setModel(m_model);
    m_list->setItemDelegate(new PopupDelegate(m_list));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setUniformItemSizes(false);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_list, 1);
    m_footer = new QLabel(this);
    m_footer->hide();
    layout->addWidget(m_footer);

    connect(m_edit, &QLineEdit::textChanged, this, &FuzzyPopup::refilter);
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex& i) {
        m_list->setCurrentIndex(i);
        activate();
    });
    hide();
    window->installEventFilter(this);
}

void FuzzyPopup::close()
{
    if (m_returnFocus)
        m_returnFocus->setFocus();
    hide();
    m_anchor = nullptr;
}

void FuzzyPopup::open(const QString& placeholder, const QList<PopupItem>& items, const QString& footer)
{
    if (!isVisible())
        m_returnFocus = QApplication::focusWidget();
    m_items = items;
    m_edit->setPlaceholderText(placeholder);
    m_footer->setText(footer);
    m_footer->setVisible(!footer.isEmpty());
    m_edit->clear();
    refilter();
    place();
    show();
    raise();
    m_edit->setFocus();
}

void FuzzyPopup::setQuery(const QString& text)
{
    m_edit->setText(text);  // refilters
}

void FuzzyPopup::openUnder(QWidget* anchor, const QString& placeholder, const QList<PopupItem>& items,
                           const QString& footer)
{
    m_anchor = anchor;
    open(placeholder, items, footer);
}

void FuzzyPopup::setItems(const QList<PopupItem>& items)
{
    const int row = m_list->currentIndex().row();
    m_items = items;
    refilter();
    if (row >= 0 && m_model->rowCount() > 0)
        m_list->setCurrentIndex(m_model->index(qMin(row, m_model->rowCount() - 1)));
}

const PopupItem* FuzzyPopup::currentItem() const
{
    return m_model->item(m_list->currentIndex().row());
}

QString FuzzyPopup::query() const
{
    return m_edit->text();
}

void FuzzyPopup::refilter()
{
    const QString q = m_edit->text().trimmed();
    m_shown = m_items;
    std::optional<PopupItem> typed = queryItem && !q.isEmpty() ? queryItem(q) : std::nullopt;
    QList<PopupModel::Row> rows;
    QList<int> scores;
    if (typed)
        m_shown.prepend(*typed);
    for (int i = typed ? 1 : 0; i < m_shown.size(); ++i) {
        const PopupItem& it = m_shown[i];
        QList<int> pos;
        int s = 0;
        if (!q.isEmpty()) {
            s = fuzzyScore(q, it.matchSubtitle ? it.subtitle : it.title, &pos);
            if (s < 0)
                continue;
        }
        rows.append({i, pos});
        scores.append(s + it.bonus);
    }
    if (!q.isEmpty()) {
        QList<int> order(rows.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return scores[a] > scores[b]; });
        QList<PopupModel::Row> sorted;
        for (int i : order)
            sorted.append(rows[i]);
        rows = sorted;
    }
    if (typed && typed->bonus < 0)
        rows.append({0, {}, {}});  // a fallback: after the matches
    else if (typed)
        rows.prepend({0, {}, {}});
    if (rows.size() > kMaxRows)
        rows.resize(kMaxRows);
    if (q.isEmpty()) {  // section headers only make sense in the unsorted list
        QString last;
        for (PopupModel::Row& r : rows) {
            const QString& sec = m_shown[r.item].section;
            if (!sec.isEmpty() && sec != last)
                r.header = sec;
            last = sec;
        }
    }
    m_model->set(&m_shown, rows);
    if (m_model->rowCount() > 0)
        m_list->setCurrentIndex(m_model->index(0));
    place();
}

void FuzzyPopup::place()
{
    QWidget* w = parentWidget();
    const int width = qMin(680, w->width() - 40);
    int listHeight = 0;
    QStyleOptionViewItem opt;
    opt.initFrom(m_list);
    for (int r = 0; r < qMin(kVisibleRows, m_model->rowCount()); ++r)
        listHeight += m_list->itemDelegate()->sizeHint(opt, m_model->index(r)).height();
    if (m_anchor) {  // a drop-down under the field: a handful of rows, the rest scroll
        QPoint at = m_anchor->mapTo(w, QPoint(0, m_anchor->height() + 4));
        const int top = m_anchor->mapTo(w, QPoint(0, 0)).y() - 4;
        const bool above = w->height() - at.y() < top;  // near the bottom (the selection bar): open upwards
        const int maxList = (above ? top : w->height() - at.y()) - 110;
        m_list->setFixedHeight(qBound(40, listHeight + 4, qMax(120, maxList)));
        adjustSize();
        const int dw = qMin(480, w->width() - 20);
        const int h = sizeHint().height();
        if (above)
            at.setY(qMax(4, top - h));
        setGeometry(qBound(10, at.x() - 6, w->width() - dw - 10), at.y(), dw, h);
        return;
    }
    m_list->setFixedHeight(qMax(listHeight + 4, 40));
    adjustSize();
    setGeometry((w->width() - width) / 2, qMin(72, w->height() / 8), width, sizeHint().height());
}

void FuzzyPopup::activate()
{
    const PopupItem* it = currentItem();
    auto run = it ? it->run : std::function<void()>{};
    close();
    if (run)
        run();
}

bool FuzzyPopup::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == parentWidget()) {
        if (e->type() == QEvent::Resize && isVisible())
            place();
        return false;
    }
    if (obj != m_edit)
        return false;
    if (e->type() == QEvent::FocusOut) {
        if (!m_list->underMouse())
            hide();
        return false;
    }
    if (e->type() == QEvent::ShortcutOverride) {
        // Ctrl+keys belong to the popup while it's open, not to window shortcuts.
        // So do Enter and Tab with any modifier (Shift+Enter: start right away).
        const auto* ke = static_cast<QKeyEvent*>(e);
        if ((ke->modifiers() & Qt::ControlModifier) || ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter
            || ke->key() == Qt::Key_Tab) {
            e->accept();
            return true;
        }
        return false;
    }
    if (e->type() != QEvent::KeyPress)
        return false;
    auto* ke = static_cast<QKeyEvent*>(e);
    if (keyHandler && keyHandler(ke, currentItem()))
        return true;
    const int rows = m_model->rowCount();
    auto move = [&](int delta) {
        if (rows == 0)
            return;
        const int r = std::clamp(m_list->currentIndex().row() + delta, 0, rows - 1);
        m_list->setCurrentIndex(m_model->index(r));
    };
    const bool ctrl = ke->modifiers() & Qt::ControlModifier;
    switch (ke->key()) {
    case Qt::Key_Escape:
        close();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        activate();
        return true;
    case Qt::Key_Down:
        move(1);
        return true;
    case Qt::Key_Up:
        move(-1);
        return true;
    case Qt::Key_PageDown:
        move(kVisibleRows - 1);
        return true;
    case Qt::Key_PageUp:
        move(-(kVisibleRows - 1));
        return true;
    case Qt::Key_N:
    case Qt::Key_J:
        if (ctrl) {
            move(1);
            return true;
        }
        break;
    case Qt::Key_K:
        if (ctrl) {
            move(-1);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

}  // namespace tandem
