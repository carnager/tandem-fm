#include "ui/ThemeProfiles.h"
#include "core/Config.h"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace tandem {
const QList<ThemeColorRole>& themeColorRoles()
{
    static const QList<ThemeColorRole> roles = {
        {"base", QT_TR_NOOP("Row background"), &Theme::base},
        {"altBase", QT_TR_NOOP("Alternate row background"), &Theme::altBase},
        {"text", QT_TR_NOOP("Row text"), &Theme::text},
        {"extensionText", QT_TR_NOOP("Default extension text"), &Theme::extensionText},
        {"dimText", QT_TR_NOOP("Secondary / hidden text"), &Theme::dimText},
        {"cursorActive", QT_TR_NOOP("Selection background"), &Theme::cursorActive, true},
        {"selectionText", QT_TR_NOOP("Selection text"), &Theme::selectionText},
        {"cursorInactive", QT_TR_NOOP("Inactive selection"), &Theme::cursorInactive, true},
        {"mark", QT_TR_NOOP("Marked file text"), &Theme::mark},
        {"markBg", QT_TR_NOOP("Marked row background"), &Theme::markBg, true},
        {"accent", QT_TR_NOOP("Accent / highlight"), &Theme::accent},
        {"accentText", QT_TR_NOOP("Text on highlight"), &Theme::accentText},
        {"dropTarget", QT_TR_NOOP("Drop target highlight"), &Theme::dropTarget, true},
        {"window", QT_TR_NOOP("Window background"), &Theme::window},
        {"header", QT_TR_NOOP("Header background"), &Theme::header},
        {"headerActive", QT_TR_NOOP("Active panel header"), &Theme::headerActive},
        {"border", QT_TR_NOOP("Borders"), &Theme::border},
        {"control", QT_TR_NOOP("Control outlines"), &Theme::control},
        {"error", QT_TR_NOOP("Error / broken link"), &Theme::error},
        {"success", QT_TR_NOOP("Success"), &Theme::success},
        {"gitModified", QT_TR_NOOP("Git modified"), &Theme::gitModified},
        {"gitAdded", QT_TR_NOOP("Git added"), &Theme::gitAdded},
        {"gitUntracked", QT_TR_NOOP("Git untracked"), &Theme::gitUntracked},
    };
    return roles;
}
namespace {
QString customId() { return QStringLiteral("custom-") + QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validId(const QString& id)
{
    static const QRegularExpression pattern(QStringLiteral("^custom-[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$"));
    return pattern.match(id).hasMatch();
}
QString validate(const ThemeProfile& profile)
{
    if (profile.name.trimmed().isEmpty() || profile.name.size() > 80)
        return QObject::tr("Enter a theme name of 1–80 characters.");
    for (QChar c : profile.name)
        if (!c.isPrint()) return QObject::tr("Theme names cannot contain control characters.");
    for (const auto& role : themeColorRoles()) {
        const auto& color = profile.palette.*role.member;
        if (!color.isValid() || (!role.alpha && color.alpha() != 255))
            return QObject::tr("Choose a valid %1 color%2.").arg(QObject::tr(role.label), role.alpha ? QString() : QObject::tr(" without transparency"));
    }
    return {};
}
Theme nord()
{
    // Nord palette: https://www.nordtheme.com/docs/colors-and-palettes/
    Theme t = defaultTheme(true);
    t.window = t.base = QColor("#2e3440");
    t.altBase = t.header = QColor("#3b4252");
    t.text = t.selectionText = QColor("#eceff4");
    t.extensionText = QColor("#8fbcbb");
    t.dimText = QColor("#a5afc2");
    t.accent = QColor("#88c0d0"); t.accentText = QColor("#2e3440");
    t.cursorActive = t.headerActive = QColor("#434c5e");
    t.cursorInactive = t.control = QColor("#4c566a");
    t.border = QColor("#3b4252");
    t.mark = t.gitModified = QColor("#ebcb8b");
    t.markBg = QColor("#26ebcb8b");
    t.dropTarget = QColor("#805e81ac");
    t.error = QColor("#bf616a");
    t.success = t.gitAdded = QColor("#a3be8c");
    t.gitUntracked = QColor("#b48ead");
    return t;
}
std::optional<ThemeProfile> decode(const QByteArray& data, QString* error)
{
    auto fail = [&](const QString& message) -> std::optional<ThemeProfile> { if (error) *error = message; return {}; };
    if (error) error->clear();
    if (data.size() > 256 * 1024) return fail(QObject::tr("Theme files must be smaller than 256 KB."));
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QObject::tr("Choose a valid Tandem theme JSON file."));
    const auto obj = doc.object();
    if (obj.value("format").toString() != QLatin1String("tandem-theme") || obj.value("version").toDouble() != 1 ||
        !obj.value("name").isString() || !obj.value("dark").isBool() || !obj.value("colors").isObject())
        return fail(QObject::tr("Unsupported theme format or version."));
    ThemeProfile profile{customId(), obj.value("name").toString().trimmed(), defaultTheme(obj.value("dark").toBool()), false};
    for (const auto& key : {QStringLiteral("alternatingRows"), QStringLiteral("selectionTextEnabled")})
        if (obj.contains(key) && !obj.value(key).isBool()) return fail(QObject::tr("Theme options must be true or false."));
    profile.palette.alternatingRows = obj.value("alternatingRows").toBool(true);
    profile.palette.selectionTextEnabled = obj.value("selectionTextEnabled").toBool(false);
    const auto colors = obj.value("colors").toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        bool found = false;
        for (const auto& role : themeColorRoles())
            if (it.key() == QLatin1String(role.key)) {
                found = true;
                if (!it.value().isString()) return fail(QObject::tr("Theme colors must be hexadecimal strings."));
                static const QRegularExpression hex(QStringLiteral("^#(?:[a-fA-F0-9]{6}|[a-fA-F0-9]{8})$"));
                if (!hex.match(it.value().toString()).hasMatch()) return fail(QObject::tr("Use #RRGGBB or #AARRGGBB colors."));
                profile.palette.*role.member = QColor(it.value().toString());
                break;
            }
        if (!found) return fail(QObject::tr("Unknown theme color: %1").arg(it.key()));
    }
    const auto problem = validate(profile);
    if (!problem.isEmpty()) return fail(problem);
    return profile;
}
QString uniqueName(QString name)
{
    const QString original = name.left(70);
    const auto profiles = themeProfiles();
    auto exists = [&](const QString& n) {
        for (const auto& p : profiles) if (p.name.compare(n, Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    for (int i = 2; exists(name); ++i) name = original + QStringLiteral(" (%1)").arg(i);
    return name;
}
} // namespace
QList<ThemeProfile> themeProfiles()
{
    QList<ThemeProfile> result = {{"dark", QObject::tr("Dark"), defaultTheme(true), true},
                                 {"light", QObject::tr("Light"), defaultTheme(false), true},
                                 {"nord", QStringLiteral("Nord"), nord(), true}};
    Settings settings;
    for (const auto& key : settings.allKeys()) {
        if (!key.startsWith(QLatin1String("themes/"))) continue;
        const QString id = key.mid(7);
        if (!validId(id)) continue;
        auto profile = decode(settings.value(key).toString().toUtf8(), nullptr);
        if (!profile) continue;
        profile->id = id;
        result.append(*profile);
    }
    return result;
}
std::optional<ThemeProfile> findThemeProfile(const QString& id)
{
    if (id == QLatin1String("system")) return {};
    for (const auto& profile : themeProfiles()) if (profile.id == id) return profile;
    return {};
}
ThemeProfile duplicateTheme(const ThemeProfile& source)
{
    ThemeProfile profile = source;
    profile.id = customId(); profile.builtIn = false;
    profile.name = uniqueName(source.name + QObject::tr(" copy"));
    return profile;
}
QString themeProfileError(const ThemeProfile& profile) { return validate(profile); }
QByteArray exportTheme(const ThemeProfile& profile)
{
    QJsonObject colors;
    for (const auto& role : themeColorRoles()) colors.insert(QLatin1String(role.key), (profile.palette.*role.member).name(QColor::HexArgb));
    return QJsonDocument(QJsonObject{{"format", "tandem-theme"}, {"version", 1}, {"name", profile.name},
                                    {"dark", profile.palette.dark}, {"alternatingRows", profile.palette.alternatingRows},
                                    {"selectionTextEnabled", profile.palette.selectionTextEnabled}, {"colors", colors}}).toJson(QJsonDocument::Indented);
}
std::optional<ThemeProfile> importTheme(const QByteArray& data, QString* error)
{
    auto profile = decode(data, error);
    if (profile) profile->name = uniqueName(profile->name);
    return profile;
}
bool saveThemeProfile(const ThemeProfile& profile, QString* error)
{
    QString problem = validate(profile);
    if (!validId(profile.id) || profile.builtIn) problem = QObject::tr("Duplicate a built-in theme before editing it.");
    for (const auto& other : themeProfiles())
        if (other.id != profile.id && other.name.compare(profile.name.trimmed(), Qt::CaseInsensitive) == 0)
            problem = QObject::tr("A theme with this name already exists.");
    if (error) *error = problem;
    if (!problem.isEmpty()) return false;
    ThemeProfile clean = profile; clean.name = clean.name.trimmed();
    return Config::instance().commit({{QStringLiteral("themes/") + profile.id, QString::fromUtf8(exportTheme(clean))}});
}
bool removeThemeProfile(const QString& id)
{
    if (!validId(id)) return false;
    QMap<QString, std::optional<QVariant>> changes{{QStringLiteral("themes/") + id, std::nullopt}};
    if (Settings().value(QStringLiteral("view/theme")).toString() == id) changes.insert(QStringLiteral("view/theme"), QStringLiteral("system"));
    return Config::instance().commit(changes);
}
void selectTheme(const QString& id)
{
    Settings().setValue(QStringLiteral("view/theme"), id == QLatin1String("system") || findThemeProfile(id) ? id : QStringLiteral("system"));
    applyTheme(*qApp);
}
} // namespace tandem
