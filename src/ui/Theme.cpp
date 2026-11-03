#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QSettings>
#include <QProxyStyle>
#include <QStyleOption>
#include <QStyleFactory>
#include <QStyleHints>
#include <QScopedValueRollback>
#include <QWidget>

namespace tandem {

static Theme g_theme;

const Theme& theme() { return g_theme; }

QFont monoFont()
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!QFontInfo(f).fixedPitch()) {
        f.setFamily(QStringLiteral("monospace"));
        f.setStyleHint(QFont::Monospace);
        f.setFixedPitch(true);
    }
    return f;
}

static QString rgba(const QColor& c)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

QString Theme::css(const QString& tmpl) const
{
    QString s = tmpl;
    const std::pair<const char*, QColor> vars[] = {
        {"@window", window},   {"@base", base},         {"@altBase", altBase},     {"@text", text},
        {"@dimText", dimText}, {"@accentText", accentText}, {"@accent", accent},   {"@border", border},
        {"@headerActive", headerActive}, {"@header", header}, {"@error", error},    {"@success", success},
        {"@mark", mark},       {"@control", control},
    };
    for (const auto& [name, color] : vars)
        s.replace(QLatin1String(name), rgba(color));
    return s;
}

void setThemeStyle(QWidget* widget, const QString& templateCss)
{
    widget->setProperty("tandemThemeCss", templateCss);
    widget->setStyleSheet(theme().css(templateCss));
}

static bool systemPrefersDark(const QApplication& app)
{
    QByteArray forced = qgetenv("TANDEM_THEME");
    if (forced.isEmpty())  // the setting (Settings › Appearance)
        forced = QSettings().value(QStringLiteral("view/theme")).toString().toUtf8();
    if (forced == "light")
        return false;
    if (forced == "dark")
        return true;
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Light)
        return false;
    if (scheme == Qt::ColorScheme::Dark)
        return true;
    return app.palette().color(QPalette::Window).lightness() < 128;
}

namespace {

// Fusion draws checkboxes, radio buttons and arrows in colours that vanish
// on a dark background. These are drawn here instead: clear outlines, the
// accent colour when on.
class TandemStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric m, const QStyleOption* o, const QWidget* w) const override
    {
        if (m == PM_IndicatorWidth || m == PM_IndicatorHeight || m == PM_ExclusiveIndicatorWidth ||
            m == PM_ExclusiveIndicatorHeight)
            return 16;
        return QProxyStyle::pixelMetric(m, o, w);
    }

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* o, QPainter* p, const QWidget* w) const override
    {
        const Theme& t = theme();
        const bool enabled = o->state & State_Enabled;
        const bool hover = o->state & State_MouseOver;
        auto fade = [&](QColor c) {
            if (!enabled)
                c.setAlphaF(c.alphaF() * 0.4);
            return c;
        };
        switch (pe) {
        case PE_IndicatorCheckBox:
        case PE_IndicatorItemViewItemCheck: {
            if (o->rect.width() < 6)  // hidden on purpose (toggle-style check boxes)
                return;
            const QRectF r = QRectF(o->rect).adjusted(1.5, 1.5, -1.5, -1.5);
            const bool on = o->state & State_On;
            const bool partial = o->state & State_NoChange;
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            if (on || partial) {
                QColor fill = t.accent;
                if (partial)
                    fill.setAlphaF(0.55);
                p->setPen(Qt::NoPen);
                p->setBrush(fade(fill));
                p->drawRoundedRect(r, 3.5, 3.5);
                p->setPen(QPen(fade(t.accentText), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->setBrush(Qt::NoBrush);
                if (partial) {
                    p->drawLine(QPointF(r.left() + 3.5, r.center().y()), QPointF(r.right() - 3.5, r.center().y()));
                } else {
                    QPainterPath tick;
                    tick.moveTo(r.left() + r.width() * 0.24, r.top() + r.height() * 0.53);
                    tick.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.72);
                    tick.lineTo(r.left() + r.width() * 0.77, r.top() + r.height() * 0.30);
                    p->drawPath(tick);
                }
            } else {
                p->setPen(QPen(fade(hover && enabled ? t.accent : t.control), 1.3));
                p->setBrush(t.base);
                p->drawRoundedRect(r, 3.5, 3.5);
            }
            p->restore();
            return;
        }
        case PE_IndicatorRadioButton: {
            const QRectF r = QRectF(o->rect).adjusted(1.5, 1.5, -1.5, -1.5);
            const bool on = o->state & State_On;
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(QPen(fade(on ? t.accent : (hover && enabled ? t.accent : t.control)), on ? 1.6 : 1.3));
            p->setBrush(t.base);
            p->drawEllipse(r);
            if (on) {
                p->setPen(Qt::NoPen);
                p->setBrush(fade(t.accent));
                p->drawEllipse(r.center(), r.width() * 0.25, r.height() * 0.25);
            }
            p->restore();
            return;
        }
        case PE_IndicatorArrowDown:
        case PE_IndicatorArrowUp:
        case PE_IndicatorArrowLeft:
        case PE_IndicatorArrowRight: {
            // A thin chevron instead of a filled triangle.
            const QRectF r = QRectF(o->rect);
            const qreal s = qMin(r.width(), r.height()) * 0.28;
            const QPointF c = r.center();
            QPolygonF v;
            if (pe == PE_IndicatorArrowDown)
                v << QPointF(c.x() - s, c.y() - s / 2) << QPointF(c.x(), c.y() + s / 2) << QPointF(c.x() + s, c.y() - s / 2);
            else if (pe == PE_IndicatorArrowUp)
                v << QPointF(c.x() - s, c.y() + s / 2) << QPointF(c.x(), c.y() - s / 2) << QPointF(c.x() + s, c.y() + s / 2);
            else if (pe == PE_IndicatorArrowLeft)
                v << QPointF(c.x() + s / 2, c.y() - s) << QPointF(c.x() - s / 2, c.y()) << QPointF(c.x() + s / 2, c.y() + s);
            else
                v << QPointF(c.x() - s / 2, c.y() - s) << QPointF(c.x() + s / 2, c.y()) << QPointF(c.x() - s / 2, c.y() + s);
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(QPen(fade(t.dimText), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->drawPolyline(v);
            p->restore();
            return;
        }
        default:
            QProxyStyle::drawPrimitive(pe, o, p, w);
        }
    }
};

}  // namespace

void applyTheme(QApplication& app)
{
    static bool applying = false;
    if (applying)
        return;
    QScopedValueRollback guard(applying, true);
    Theme& t = g_theme;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const QString requested = qEnvironmentVariable("TANDEM_THEME", QSettings().value(QStringLiteral("view/theme"), QStringLiteral("system")).toString());
    app.styleHints()->setColorScheme(requested == QLatin1String("dark") ? Qt::ColorScheme::Dark
                                     : requested == QLatin1String("light") ? Qt::ColorScheme::Light
                                                                          : Qt::ColorScheme::Unknown);
#endif
    t.dark = systemPrefersDark(app);
    if (!app.property("tandemThemeConnected").toBool()) {
        app.setProperty("tandemThemeConnected", true);
        QObject::connect(app.styleHints(), &QStyleHints::colorSchemeChanged, &app, [&app] {
            if (qEnvironmentVariableIsEmpty("TANDEM_THEME") &&
                QSettings().value(QStringLiteral("view/theme"), QStringLiteral("system")).toString() == QLatin1String("system"))
                applyTheme(app);
        });
    }

    if (t.dark) {
        t.window = QColor(0x1a, 0x1c, 0x20);
        t.base = QColor(0x1f, 0x21, 0x26);
        t.altBase = QColor(0x23, 0x25, 0x2b);
        t.text = QColor(0xe4, 0xe6, 0xea);
        t.dimText = QColor(0x8a, 0x90, 0x9a);
        t.accent = QColor(0x6c, 0x9c, 0xff);
        t.accentText = QColor(0xff, 0xff, 0xff);
        t.mark = QColor(0xff, 0xb4, 0x54);
        t.markBg = QColor(255, 180, 84, 26);
        t.cursorActive = QColor(108, 156, 255, 72);
        t.cursorInactive = QColor(255, 255, 255, 46);
        t.dropTarget = QColor(108, 156, 255, 110);
        t.border = QColor(0x2d, 0x30, 0x37);
        t.control = QColor(0x4a, 0x4f, 0x5a);
        t.header = QColor(0x22, 0x24, 0x2a);
        t.headerActive = QColor(0x27, 0x2f, 0x45);
        t.error = QColor(0xff, 0x6b, 0x6b);
        t.success = QColor(0x5f, 0xd3, 0x8d);
        t.gitModified = QColor(0xe5, 0xc0, 0x7b);
        t.gitAdded = QColor(0x98, 0xc3, 0x79);
        t.gitUntracked = QColor(0x56, 0xb6, 0xc2);
    } else {
        t.window = QColor(0xee, 0xf0, 0xf3);
        t.base = QColor(0xff, 0xff, 0xff);
        t.altBase = QColor(0xf6, 0xf7, 0xf9);
        t.text = QColor(0x1d, 0x21, 0x27);
        t.dimText = QColor(0x6b, 0x72, 0x80);
        t.accent = QColor(0x2f, 0x6f, 0xed);
        t.accentText = QColor(0xff, 0xff, 0xff);
        t.mark = QColor(0xc2, 0x41, 0x0c);
        t.markBg = QColor(194, 65, 12, 20);
        t.cursorActive = QColor(47, 111, 237, 50);
        t.cursorInactive = QColor(0, 0, 0, 40);
        t.dropTarget = QColor(47, 111, 237, 90);
        t.border = QColor(0xd9, 0xdc, 0xe1);
        t.control = QColor(0xa9, 0xaf, 0xb9);
        t.header = QColor(0xe6, 0xe9, 0xee);
        t.headerActive = QColor(0xdc, 0xe6, 0xfb);
        t.error = QColor(0xd9, 0x2d, 0x20);
        t.success = QColor(0x16, 0x8a, 0x4a);
        t.gitModified = QColor(0xb0, 0x6d, 0x00);
        t.gitAdded = QColor(0x2e, 0x7d, 0x32);
        t.gitUntracked = QColor(0x00, 0x7c, 0x89);
    }

    // Fusion + our palette: consistent, fast to paint, no theme engine quirks.
    if (!app.property("tandemStyleInstalled").toBool()) {
        app.setStyle(new TandemStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
        app.setProperty("tandemStyleInstalled", true);
    }
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.base);
    p.setColor(QPalette::AlternateBase, t.altBase);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::PlaceholderText, t.dimText);
    p.setColor(QPalette::Button, t.header);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.accentText);
    p.setColor(QPalette::ToolTipBase, t.header);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::Mid, t.border);
    p.setColor(QPalette::Dark, t.border.darker(120));
    p.setColor(QPalette::Light, t.header.lighter(110));
    p.setColor(QPalette::Disabled, QPalette::Text, t.dimText);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.dimText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.dimText);
    app.setPalette(p);

    // Style sheets can't draw a drop-down's arrow themselves: a small chevron file.
    const QString chevron = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                            QStringLiteral("/chevron-%1.svg").arg(t.dimText.name().mid(1));
    if (!QFileInfo::exists(chevron)) {
        QDir().mkpath(QFileInfo(chevron).path());
        QFile f(chevron);
        if (f.open(QIODevice::WriteOnly))
            f.write(QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'>"
                                   "<path d='M2.5 4.5 6 8l3.5-3.5' fill='none' stroke='%1' stroke-width='1.6' "
                                   "stroke-linecap='round' stroke-linejoin='round'/></svg>")
                        .arg(t.dimText.name())
                        .toUtf8());
    }
    // Inputs, drop-downs and buttons: visible edges, the accent when focused.
    // Widgets with their own style sheets (popups, bars) still win.
    app.setStyleSheet(t.css(QStringLiteral(
        "QLineEdit, QComboBox, QSpinBox, QPlainTextEdit[frameShape=\"6\"] {"
        "  background: @base; color: @text; border: 1px solid @control; border-radius: 6px;"
        "  padding: 4px 8px; selection-background-color: @accent; selection-color: @accentText; }"
        "QLineEdit:hover, QComboBox:hover, QSpinBox:hover { border-color: @dimText; }"
        "QLineEdit:focus, QComboBox:focus, QComboBox:on, QSpinBox:focus { border-color: @accent; }"
        "QLineEdit:disabled, QComboBox:disabled { color: @dimText; border-color: @border; }"
        "QComboBox { padding-right: 26px; }"
        "QComboBox::drop-down { border: none; width: 24px; subcontrol-origin: padding; subcontrol-position: center right; }"
        "QComboBox::down-arrow { image: url(CHEVRON); width: 12px; height: 12px; }"
        "QComboBox QAbstractItemView { background: @base; border: 1px solid @control; border-radius: 6px;"
        "  padding: 4px; selection-background-color: @accent; selection-color: @accentText; outline: none; }"
        "QPushButton { background: @header; color: @text; border: 1px solid @control; border-radius: 6px;"
        "  padding: 5px 16px; min-height: 18px; }"
        "QPushButton:hover { border-color: @dimText; }"
        "QPushButton:pressed { background: @base; }"
        "QPushButton:default { background: @accent; color: @accentText; border-color: @accent; }"
        "QPushButton:default:hover { border-color: @text; }"
        "QPushButton:disabled { color: @dimText; border-color: @border; background: @header; }"
        "QToolTip { background: @header; color: @text; border: 1px solid @control; border-radius: 4px; padding: 4px 6px; }"))
                          .replace(QStringLiteral("CHEVRON"), chevron));

    // Icons: keep the user's theme; pick a sensible one if none is configured.
    const QString current = QIcon::themeName();
    if (current.isEmpty() || current == QLatin1String("hicolor")) {
        const QStringList candidates = t.dark ? QStringList{"Papirus-Dark", "breeze-dark", "Adwaita", "breeze"}
                                              : QStringList{"Papirus", "breeze", "Adwaita"};
        for (const QString& name : candidates) {
            if (QDir(QStringLiteral("/usr/share/icons/") + name).exists()) {
                QIcon::setThemeName(name);
                break;
            }
        }
    }

    for (QWidget* widget : QApplication::allWidgets()) {
        const QVariant css = widget->property("tandemThemeCss");
        if (css.isValid())
            widget->setStyleSheet(t.css(css.toString()));
        widget->update();
    }
    t.listFont = app.font();
    t.numberFont = t.listFont;
    t.numberFont.setFeature(QFont::Tag("tnum"), 1);
}

}  // namespace tandem
