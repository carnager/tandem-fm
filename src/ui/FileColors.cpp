#include "ui/FileColors.h"
#include "core/Config.h"
#include "ui/FileView.h"
#include "ui/ThumbGrid.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QRegularExpression>
#include <sys/stat.h>

namespace tandem {
namespace {
QStringList extensions(const QString& value)
{
    QStringList out;
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    for (QString part : value.split(separators, Qt::SkipEmptyParts)) {
        if (part.startsWith(QLatin1String("*."))) part.remove(0, 2);
        else if (part.startsWith(QLatin1Char('.'))) part.remove(0, 1);
        out.append(part.toCaseFolded());
    }
    return out;
}
QList<FileColorRule> decode(const QString& json)
{
    QList<FileColorRule> rules;
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isArray()) return rules;
    for (const auto& value : document.array()) {
        if (!value.isObject()) continue;
        const auto object = value.toObject();
        FileColorRule rule;
        rule.match = object.value(QStringLiteral("match")).toString();
        rule.value = object.value(QStringLiteral("value")).toString();
        rule.light = QColor(object.value(QStringLiteral("light")).toString());
        rule.dark = QColor(object.value(QStringLiteral("dark")).toString());
        rule.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        if (rule.error().isEmpty()) rules.append(rule);
    }
    return rules;
}
} // namespace
QString FileColorRule::error() const
{
    if (!light.isValid() || !dark.isValid() || light.alpha() != 255 || dark.alpha() != 255)
        return QObject::tr("Choose an opaque text color for each theme (for example #3366cc).");
    if (match == QLatin1String("extension")) {
        const auto parts = extensions(value);
        if (parts.isEmpty()) return QObject::tr("Enter one or more extensions, for example pdf; epub or tar.gz.");
        static const QRegularExpression valid(QStringLiteral("^[\\p{L}\\p{N}_+-]+(?:\\.[\\p{L}\\p{N}_+-]+)*$"));
        for (const QString& part : parts)
            if (!valid.match(part).hasMatch()) return QObject::tr("Use extensions such as pdf, .jpg or *.tar.gz; separate them with commas or semicolons.");
    } else if (match == QLatin1String("mime")) {
        static const QRegularExpression valid(QStringLiteral("^[a-z0-9!#$&^_.+-]+/(?:[a-z0-9!#$&^_.+-]+|\\*)$"), QRegularExpression::CaseInsensitiveOption);
        if (!valid.match(value.trimmed()).hasMatch()) return QObject::tr("Enter a MIME type such as application/pdf or image/*.");
    } else if (match == QLatin1String("kind")) {
        if (!QStringList{QStringLiteral("folder"), QStringLiteral("link"), QStringLiteral("executable"),
                         QStringLiteral("no-extension"), QStringLiteral("file")}.contains(value))
            return QObject::tr("Choose a file kind.");
    } else {
        return QObject::tr("Choose how to match files.");
    }
    return {};
}
FileColors& FileColors::instance() { static FileColors colors; return colors; }
QList<FileColorRule> FileColors::rules() { return decode(Settings().value(QStringLiteral("view/fileColors")).toString()); }
void FileColors::saveRules(const QList<FileColorRule>& rules)
{
    QJsonArray array;
    for (const auto& rule : rules)
        if (rule.error().isEmpty())
            array.append(QJsonObject{{QStringLiteral("match"), rule.match}, {QStringLiteral("value"), rule.value.trimmed()},
                                     {QStringLiteral("light"), rule.light.name(QColor::HexRgb)},
                                     {QStringLiteral("dark"), rule.dark.name(QColor::HexRgb)}, {QStringLiteral("enabled"), rule.enabled}});
    Settings().setValue(QStringLiteral("view/fileColors"), QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    refreshViews();
}
void FileColors::setEnabled(bool on)
{
    Settings().setValue(QStringLiteral("view/fileColorsEnabled"), on);
    refreshViews();
}
void FileColors::refreshViews()
{
    for (QWidget* widget : QApplication::allWidgets())
        if (auto* view = qobject_cast<FileView*>(widget)) {
            view->viewport()->update();
            if (view->thumbGrid()) view->thumbGrid()->update();
        }
}
void FileColors::load()
{
    Settings settings;
    const QString json = settings.value(QStringLiteral("view/fileColors")).toString();
    m_enabled = settings.value(QStringLiteral("view/fileColorsEnabled"), true).toBool();
    if (m_loaded && json == m_json) return;
    m_loaded = true; m_json = json; m_rules = decode(json);
    m_extensions.clear();
    m_mimeNames.clear();
    for (const auto& rule : m_rules)
        if (rule.match == QLatin1String("extension")) m_extensions.insert(rule.value, extensions(rule.value));
}
bool FileColors::matches(const FileColorRule& rule, const FileEntry& entry)
{
    if (rule.match == QLatin1String("extension")) {
        if (entry.isDir) return false;
        for (const QString& extension : m_extensions.value(rule.value)) {
            const QString suffix = QLatin1Char('.') + extension;
            if (entry.name.size() > suffix.size() && entry.name.endsWith(suffix, Qt::CaseInsensitive)) return true;
        }
        return false;
    }
    if (rule.match == QLatin1String("kind")) {
        if (rule.value == QLatin1String("folder")) return entry.isDir;
        if (rule.value == QLatin1String("link")) return entry.isLink;
        if (entry.isDir) return false;
        if (rule.value == QLatin1String("file")) return true;
        if (rule.value == QLatin1String("executable")) return !entry.isLink && S_ISREG(entry.mode) && (entry.mode & 0111);
        if (rule.value == QLatin1String("no-extension")) return entry.name.lastIndexOf(QLatin1Char('.')) <= 0 || entry.name.endsWith(QLatin1Char('.'));
        return false;
    }
    const QString wanted = rule.value.trimmed().toLower();
    if (entry.isDir) return wanted == QLatin1String("inode/directory") || wanted == QLatin1String("inode/*");
    auto cached = m_mimeNames.constFind(entry.name);
    if (cached == m_mimeNames.cend()) {
        if (m_mimeNames.size() >= 4096) m_mimeNames.clear();
        QStringList names;
        const auto types = QMimeDatabase().mimeTypesForFileName(entry.name);
        for (const auto& type : types) names << type.name() << type.aliases() << type.allAncestors();
        if (names.isEmpty()) names << QStringLiteral("application/octet-stream");
        cached = m_mimeNames.insert(entry.name, names);
    }
    for (const QString& name : *cached)
        if (wanted.endsWith(QLatin1String("/*")) ? name.startsWith(wanted.chopped(1)) : name == wanted) return true;
    return false;
}
QColor FileColors::nameColor(const FileEntry& entry, char gitStatus)
{
    const auto& t = theme();
    if (entry.isParent) return t.text;
    if (entry.marked) return t.mark;
    if (entry.brokenLink) return t.error;
    load();
    if (m_enabled)
        for (const auto& rule : m_rules)
            if (rule.enabled && matches(rule, entry)) return t.dark ? rule.dark : rule.light;
    return entry.name.startsWith(QLatin1Char('.')) || gitStatus == '!' ? t.dimText : t.text;
}
} // namespace tandem
