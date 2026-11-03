#include "ui/Theme.h"

#include "core/ConfigDir.h"
#include "ui/IconCache.h"

#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFile>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <QFontDatabase>
#include <QFontInfo>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QProxyStyle>
#include <QStyleOption>
#include <QStyleFactory>
#include <QStyleHints>

#include <algorithm>
#include <optional>

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

// ------------------------------------------------------------------ themes

namespace {

Theme builtinDark()
{
    Theme t;
    t.id = QStringLiteral("tandem-dark");
    t.name = QStringLiteral("Tandem Dark");
    t.dark = true;
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
    return t;
}

Theme builtinLight()
{
    Theme t;
    t.id = QStringLiteral("tandem-light");
    t.name = QStringLiteral("Tandem Light");
    t.dark = false;
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
    return t;
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

QColor mix(const QColor& a, const QColor& b, double f)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f, a.greenF() + (b.greenF() - a.greenF()) * f,
                            a.blueF() + (b.blueF() - a.blueF()) * f);
}

// "#rgb", "#rrggbb" or "#rrggbbaa" (CSS order), or a colour name.
std::optional<QColor> parseColor(const QString& s)
{
    QString v = s.trimmed();
    int alpha = 255;
    if (v.size() == 9 && v.startsWith(QLatin1Char('#'))) {
        bool ok = false;
        alpha = v.right(2).toInt(&ok, 16);
        if (!ok)
            return std::nullopt;
        v.chop(2);
    }
    QColor c = QColor::fromString(v);
    if (!c.isValid())
        return std::nullopt;
    c.setAlpha(alpha);
    return c;
}

struct Role {
    const char* key;
    QColor Theme::*color;
};
const Role kRoles[] = {
    {"window", &Theme::window},           {"base", &Theme::base},
    {"altBase", &Theme::altBase},         {"text", &Theme::text},
    {"dimText", &Theme::dimText},         {"accent", &Theme::accent},
    {"accentText", &Theme::accentText},   {"mark", &Theme::mark},
    {"markBg", &Theme::markBg},           {"cursorActive", &Theme::cursorActive},
    {"cursorInactive", &Theme::cursorInactive}, {"dropTarget", &Theme::dropTarget},
    {"border", &Theme::border},           {"control", &Theme::control},
    {"header", &Theme::header},           {"headerActive", &Theme::headerActive},
    {"error", &Theme::error},             {"success", &Theme::success},
    {"gitModified", &Theme::gitModified}, {"gitAdded", &Theme::gitAdded},
    {"gitUntracked", &Theme::gitUntracked},
};

QString builtinFolder() { return QStringLiteral(":/themes"); }

// The file for a theme id: the user's own first, then the shipped one.
QString themeFile(const QString& id)
{
    for (const QString& dir : {themesFolder(), builtinFolder()}) {
        const QString f = dir + QLatin1Char('/') + id + QStringLiteral(".json");
        if (QFileInfo::exists(f))
            return f;
    }
    return {};
}

}  // namespace

QString themesFolder()
{
    if (!configDir().isEmpty())
        return QFileInfo(QSettings().fileName()).path() + QStringLiteral("/themes");
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/themes");
}

bool loadThemeFile(const QString& path, Theme& out, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(f.errorString());
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.read(256 * 1024), &pe);
    if (!doc.isObject())
        return fail(pe.errorString());
    const QJsonObject o = doc.object();
    const QJsonObject colors = o.value(QLatin1String("colors")).toObject();
    if (colors.isEmpty())
        return fail(QObject::tr("no \"colors\""));

    const bool dark = o.contains(QLatin1String("dark")) ? o.value(QLatin1String("dark")).toBool()
                                                        : parseColor(colors.value(QLatin1String("base")).toString())
                                                              .value_or(QColor(Qt::black))
                                                              .lightness() < 128;
    Theme t = dark ? builtinDark() : builtinLight();
    t.id = QFileInfo(path).completeBaseName();
    t.name = o.value(QLatin1String("name")).toString(t.id);
    t.dark = dark;
    QSet<QString> given;
    for (const Role& r : kRoles) {
        const QString key = QLatin1String(r.key);
        if (const auto c = parseColor(colors.value(key).toString())) {
            t.*r.color = *c;
            given.insert(key);
        }
    }
    // Shades the file leaves out follow its own colours, not Tandem's.
    auto derive = [&](const char* key, const QColor& value) {
        if (!given.contains(QLatin1String(key)))
            for (const Role& r : kRoles)
                if (qstrcmp(r.key, key) == 0)
                    t.*r.color = value;
    };
    if (given.contains(QLatin1String("base")))
        derive("altBase", mix(t.base, t.text, dark ? 0.03 : 0.025));
    if (given.contains(QLatin1String("window")))
        derive("header", mix(t.window, t.text, dark ? 0.04 : 0.05));
    if (given.contains(QLatin1String("text"))) {
        derive("dimText", mix(t.text, t.base, 0.42));
        derive("cursorInactive", withAlpha(t.text, dark ? 46 : 40));
    }
    if (given.contains(QLatin1String("window")) || given.contains(QLatin1String("text"))) {
        derive("border", mix(t.window, t.text, dark ? 0.1 : 0.12));
        derive("control", mix(t.window, t.text, dark ? 0.27 : 0.32));
    }
    derive("markBg", withAlpha(t.mark, dark ? 26 : 20));
    derive("cursorActive", withAlpha(t.accent, dark ? 72 : 50));
    derive("dropTarget", withAlpha(t.accent, dark ? 110 : 90));
    derive("headerActive", mix(t.header, t.accent, dark ? 0.16 : 0.12));
    out = t;
    return true;
}

QByteArray themeJson(const Theme& t)
{
    QJsonObject colors;
    for (const Role& r : kRoles) {
        const QColor c = t.*r.color;
        QString v = c.name(QColor::HexRgb);
        if (c.alpha() != 255)
            v += QStringLiteral("%1").arg(c.alpha(), 2, 16, QLatin1Char('0'));
        colors.insert(QLatin1String(r.key), v);
    }
    QJsonObject o;
    o.insert(QStringLiteral("name"), t.name);
    o.insert(QStringLiteral("dark"), t.dark);
    o.insert(QStringLiteral("colors"), colors);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QList<ThemeInfo> availableThemes()
{
    QList<ThemeInfo> out{{QStringLiteral("tandem-dark"), QStringLiteral("Tandem Dark"), true, true},
                         {QStringLiteral("tandem-light"), QStringLiteral("Tandem Light"), false, true}};
    QSet<QString> seen{out[0].id, out[1].id};
    QList<ThemeInfo> more;
    for (const QString& dir : {themesFolder(), builtinFolder()}) {
        const bool builtIn = dir == builtinFolder();
        for (const QFileInfo& fi : QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
            Theme t;
            if (seen.contains(fi.completeBaseName()) || !loadThemeFile(fi.filePath(), t))
                continue;
            seen.insert(t.id);
            more.append({t.id, t.name, t.dark, builtIn});
        }
    }
    std::sort(more.begin(), more.end(),
              [](const ThemeInfo& a, const ThemeInfo& b) { return a.name.localeAwareCompare(b.name) < 0; });
    return out + more;
}

ThemeNotifier& ThemeNotifier::instance()
{
    static ThemeNotifier n;
    return n;
}

void setThemeStyle(QWidget* w, const QString& tmpl)
{
    w->setProperty("tandemStyle", tmpl);
    w->setStyleSheet(theme().css(tmpl));
}

namespace {

// Light or dark, as the desktop says. Asked before Tandem's own palette is
// set, where the platform can't tell.
bool desktopIsDark(QApplication& app)
{
    static const bool paletteDark = app.palette().color(QPalette::Window).lightness() < 128;
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Light)
        return false;
    if (scheme == Qt::ColorScheme::Dark)
        return true;
    return paletteDark;
}

Theme chosenTheme(QApplication& app, bool* followsDesktop)
{
    QSettings s;
    QString id = qEnvironmentVariable("TANDEM_THEME");
    if (id.isEmpty())
        id = s.value(QStringLiteral("view/theme"), QStringLiteral("system")).toString();
    if (id == QLatin1String("dark") || id == QLatin1String("light"))  // older settings
        id = QStringLiteral("tandem-") + id;
    *followsDesktop = id == QLatin1String("system") || id.isEmpty();
    if (*followsDesktop)
        id = desktopIsDark(app) ? s.value(QStringLiteral("view/themeDark"), QStringLiteral("tandem-dark")).toString()
                                : s.value(QStringLiteral("view/themeLight"), QStringLiteral("tandem-light")).toString();
    if (id == QLatin1String("tandem-dark"))
        return builtinDark();
    if (id == QLatin1String("tandem-light"))
        return builtinLight();
    Theme t;
    const QString file = themeFile(id);
    if (!file.isEmpty() && loadThemeFile(file, t))
        return t;
    return desktopIsDark(app) ? builtinDark() : builtinLight();  // missing or broken file
}

// Theme files being edited are applied as soon as they're saved.
void watchThemes(QApplication& app)
{
    static QFileSystemWatcher* watcher = nullptr;
    static QTimer* later = nullptr;
    if (!watcher) {
        watcher = new QFileSystemWatcher(&app);
        later = new QTimer(&app);
        later->setSingleShot(true);
        later->setInterval(150);
        QObject::connect(later, &QTimer::timeout, &app, [&app] { applyTheme(app); });
        QObject::connect(watcher, &QFileSystemWatcher::fileChanged, later, qOverload<>(&QTimer::start));
        QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, later, qOverload<>(&QTimer::start));
    }
    if (!watcher->files().isEmpty())
        watcher->removePaths(watcher->files());
    // The folder, or, until it exists, the one it will be made in.
    const QString dir = themesFolder();
    const QString watched = QFileInfo::exists(dir) ? dir : QFileInfo(dir).path();
    for (const QString& d : watcher->directories())
        if (d != watched)
            watcher->removePath(d);
    if (QFileInfo::exists(watched) && !watcher->directories().contains(watched))
        watcher->addPath(watched);
    const QString own = dir + QLatin1Char('/') + g_theme.id + QStringLiteral(".json");
    if (QFileInfo::exists(own))
        watcher->addPath(own);  // editors that write in place don't touch the folder
}

}  // namespace

void applyTheme(QApplication& app)
{
    static bool first = true;
    bool followsDesktop = false;
    const QFont listFont = g_theme.listFont;
    g_theme = chosenTheme(app, &followsDesktop);
    Theme& t = g_theme;

    if (first) {
        // Fusion + our palette: consistent, fast to paint, no theme engine quirks.
        app.setStyle(new TandemStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, [&app] {
            bool follows = false;
            const QString before = g_theme.id;
            if (chosenTheme(app, &follows).id != before)
                applyTheme(app);
        });
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

    // Icons: the desktop's icon theme in its light or dark variant, to match
    // (Papirus and Papirus-Dark, breeze and breeze-dark), unless one is chosen
    // in Settings. No icon theme at all: a common one.
    if (QSettings().value(QStringLiteral("view/iconTheme")).toString().isEmpty()) {
        const QString original = IconCache::systemTheme();  // remembered before we change it
        auto installed = [](const QString& name) {
            for (const QString& dir : QIcon::themeSearchPaths())
                if (QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral("/index.theme")))
                    return true;
            return false;
        };
        QStringList candidates;
        if (original.isEmpty() || original == QLatin1String("hicolor")) {
            candidates = t.dark ? QStringList{"Papirus-Dark", "breeze-dark", "Adwaita", "breeze"}
                                : QStringList{"Papirus", "breeze", "Adwaita"};
        } else {
            static const QRegularExpression variant(QStringLiteral("-(dark|light)$"),
                                                    QRegularExpression::CaseInsensitiveOption);
            const QString stem = QString(original).remove(variant);
            candidates = t.dark ? QStringList{stem + QStringLiteral("-Dark"), stem + QStringLiteral("-dark"), original}
                                : QStringList{stem, original};
        }
        for (const QString& name : std::as_const(candidates)) {
            if (!installed(name))
                continue;
            if (name != QIcon::themeName()) {
                QIcon::setThemeName(name);
                if (!first)
                    IconCache::clear();
            }
            break;
        }
    }

    t.listFont = first ? app.font() : listFont;
    t.numberFont = t.listFont;
    t.numberFont.setFeature(QFont::Tag("tnum"), 1);

    if (!first) {
        // Everything already shown takes the new colours.
        for (QWidget* w : QApplication::allWidgets()) {
            const QVariant tmpl = w->property("tandemStyle");
            if (tmpl.isValid())
                w->setStyleSheet(t.css(tmpl.toString()));
        }
        emit ThemeNotifier::instance().changed();
        for (QWidget* w : QApplication::topLevelWidgets())
            w->update();
    }
    watchThemes(app);
    first = false;
}

}  // namespace tandem
