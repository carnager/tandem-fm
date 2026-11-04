#pragma once

#include "ui/Theme.h"

#include <QAbstractButton>
#include <QPainter>

namespace tandem {

// Bottom bar button: the key as a key cap, the command, and where it goes
// ("F5  Copy 4  → Projects").
class KeyButton : public QAbstractButton {
public:
    KeyButton(const QString& keys, const QString& label, QWidget* parent)
        : QAbstractButton(parent), m_keys(keys.split(QLatin1Char('+'))), m_label(label)
    {
        setObjectName(QStringLiteral("fkey-") + keys);
        setAccessibleName(label);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    // In the selection bar: a framed pill; primary = the accent.
    void setBoxed(bool boxed, bool primary = false)
    {
        m_boxed = boxed;
        m_primary = primary;
        update();
    }
    void setKeys(const QString& keys)
    {
        const QStringList k = keys.split(QLatin1Char('+'));
        if (k == m_keys)
            return;
        m_keys = k;
        updateGeometry();
        update();
    }
    void setText(const QString& label, const QString& detail = {})
    {
        if (label == m_label && detail == m_detail)
            return;
        m_label = label;
        m_detail = detail;
        setAccessibleName(text());  // what screen readers (and tests) read
        updateGeometry();
        update();
    }
    QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        return {capsWidth() + 8 + fm.horizontalAdvance(text()) + 24, fm.height() + 16};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const Theme& t = theme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (m_boxed) {
            QColor bg = m_primary ? t.cursorActive : t.base;
            if (underMouse() || isDown())
                bg = m_primary ? t.dropTarget : t.cursorInactive;
            p.setPen(QPen(m_primary ? t.accent : t.control, 1));
            p.setBrush(bg);
            p.drawRoundedRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), 8, 8);
        } else if (isDown() || underMouse()) {
            p.setPen(QPen(t.border, 1));
            p.setBrush(isDown() ? t.cursorActive : t.header);
            p.drawRoundedRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), 8, 8);
        }
        const QFontMetrics fm(font());
        QFont kf = font();
        kf.setPointSizeF(kf.pointSizeF() * 0.82);
        kf.setWeight(QFont::DemiBold);
        const QFontMetrics kfm(kf);
        int x = 12;
        const int capH = kfm.height() + 4;
        const int capY = (height() - capH) / 2;
        p.setFont(kf);
        for (const QString& k : m_keys) {  // key caps, like the keys on the keyboard
            if (k.isEmpty())
                continue;
            const int w = qMax(capH, kfm.horizontalAdvance(k) + 10);
            const QRectF cap(x, capY, w, capH);
            p.setPen(QPen(t.control, 1));
            p.setBrush(t.header);
            p.drawRoundedRect(cap.adjusted(0.5, 0.5, -0.5, -0.5), 4.5, 4.5);
            p.setPen(t.text);
            p.drawText(cap, Qt::AlignCenter, k);
            x += w + 3;
        }
        if (!m_keys.join(QString()).isEmpty())
            x += 5;
        p.setFont(font());
        p.setPen(t.text);
        p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignLeft | Qt::AlignVCenter, m_label);
        if (!m_detail.isEmpty()) {
            x += fm.horizontalAdvance(m_label + QLatin1Char(' '));
            p.setPen(t.dimText);
            p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignLeft | Qt::AlignVCenter, m_detail);
        }
    }

private:
    QString text() const { return m_detail.isEmpty() ? m_label : m_label + QLatin1Char(' ') + m_detail; }
    int capsWidth() const
    {
        QFont kf = font();
        kf.setPointSizeF(kf.pointSizeF() * 0.82);
        const QFontMetrics kfm(kf);
        int w = 0;
        for (const QString& k : m_keys)
            if (!k.isEmpty())
                w += qMax(kfm.height() + 4, kfm.horizontalAdvance(k) + 10) + 3;
        return w;
    }

    QStringList m_keys;
    QString m_label, m_detail;
    bool m_boxed = false, m_primary = false;
};

}  // namespace tandem
