#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

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
        {"@mark", mark},
    };
    for (const auto& [name, color] : vars)
        s.replace(QLatin1String(name), rgba(color));
    return s;
}

static bool systemPrefersDark(const QApplication& app)
{
    const QByteArray forced = qgetenv("TANDEM_THEME");
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

void applyTheme(QApplication& app)
{
    Theme& t = g_theme;
    t.dark = systemPrefersDark(app);

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
        t.header = QColor(0xe6, 0xe9, 0xee);
        t.headerActive = QColor(0xdc, 0xe6, 0xfb);
        t.error = QColor(0xd9, 0x2d, 0x20);
        t.success = QColor(0x16, 0x8a, 0x4a);
        t.gitModified = QColor(0xb0, 0x6d, 0x00);
        t.gitAdded = QColor(0x2e, 0x7d, 0x32);
        t.gitUntracked = QColor(0x00, 0x7c, 0x89);
    }

    // Fusion + our palette: consistent, fast to paint, no theme engine quirks.
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
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

    t.listFont = app.font();
    t.numberFont = t.listFont;
    t.numberFont.setFeature(QFont::Tag("tnum"), 1);
}

}  // namespace tandem
