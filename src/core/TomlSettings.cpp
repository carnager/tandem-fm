#include "core/TomlSettings.h"

#include "core/ConfigDir.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#include <sstream>

namespace tandem {

namespace {

QMutex g_errorMutex;
QString g_error;

void setError(const QString& e)
{
    QMutexLocker lock(&g_errorMutex);
    g_error = e;
}

// A bare TOML key, or a quoted one when it has other characters.
QString keyText(const QString& k)
{
    static const QRegularExpression bare(QStringLiteral("^[A-Za-z0-9_-]+$"));
    if (bare.match(k).hasMatch())
        return k;
    return tomlValueText(k);
}

QString tableText(const QString& table)  // "a/b" -> a.b
{
    QStringList parts;
    for (const QString& p : table.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        parts << keyText(p);
    return parts.join(QLatin1Char('.'));
}

QVariant fromNode(const toml::node& n)
{
    if (auto v = n.as_boolean())
        return v->get();
    if (auto v = n.as_integer()) {
        const int64_t i = v->get();
        return i >= INT_MIN && i <= INT_MAX ? QVariant(int(i)) : QVariant(qlonglong(i));
    }
    if (auto v = n.as_floating_point())
        return v->get();
    if (auto v = n.as_string()) {
        const QString s = QString::fromStdString(v->get());
        if (s.startsWith(QLatin1String("base64:")))  // window state Qt keeps as bytes
            return QByteArray::fromBase64(s.mid(7).toLatin1());
        return s;
    }
    if (auto a = n.as_array()) {
        bool strings = true;
        QVariantList list;
        for (const toml::node& e : *a) {
            strings = strings && e.is_string();
            list.append(fromNode(e));
        }
        if (strings && !list.isEmpty()) {
            QStringList sl;
            for (const QVariant& v : list)
                sl << v.toString();
            return sl;
        }
        return list;
    }
    std::ostringstream os;  // dates and times: as written
    n.visit([&os](auto&& x) { os << x; });
    return QString::fromStdString(os.str());
}

void flatten(const toml::table& t, const QString& prefix, QVariantMap* out)
{
    for (auto&& [k, v] : t) {
        const QString key = prefix + QString::fromUtf8(k.str().data(), qsizetype(k.str().size()));
        if (auto sub = v.as_table())
            flatten(*sub, key + QLatin1Char('/'), out);
        else
            out->insert(key, fromNode(v));
    }
}

QString readText(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

bool writeText(const QString& path, const QString& text)
{
    if (readText(path) == text)
        return true;
    QDir().mkpath(QFileInfo(path).path());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(text.toUtf8());
    return f.commit();
}

// The whole map as TOML: root keys, then a table per group. For state.toml,
// which only Tandem writes.
QString generate(const QVariantMap& map, const QString& header)
{
    QMap<QString, QStringList> tables;  // table -> "key = value" lines
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        const int slash = int(it.key().lastIndexOf(QLatin1Char('/')));
        const QString table = slash < 0 ? QString() : it.key().left(slash);
        const QString key = it.key().mid(slash + 1);
        const QString value = tomlValueText(it.value());
        if (!value.isEmpty())
            tables[table] << keyText(key) + QStringLiteral(" = ") + value;
    }
    QString out = header;
    for (auto it = tables.cbegin(); it != tables.cend(); ++it) {
        if (!it.key().isEmpty())
            out += QStringLiteral("\n[%1]\n").arg(tableText(it.key()));
        out += it.value().join(QLatin1Char('\n')) + QLatin1Char('\n');
    }
    return out;
}

const QString kSettingsHeader = QStringLiteral(
    "# Tandem settings. The settings dialog changes single lines here and\n"
    "# keeps everything else, comments too. Key bindings: keys.toml.\n");
const QString kStateHeader = QStringLiteral(
    "# Tandem's own state: window size, columns, open tabs, recent choices.\n"
    "# Rewritten on every change; settings are in tandem-fm.toml.\n");

bool readToml(QIODevice& device, QSettings::SettingsMap& map)
{
    QString error;
    QVariantMap settings;
    if (!parseToml(QString::fromUtf8(device.readAll()), &settings, &error)) {
        setError(QStringLiteral("tandem-fm.toml, %1").arg(error));
        return false;
    }
    QVariantMap state;
    if (!parseToml(readText(stateFilePath()), &state, &error))
        state.clear();  // Tandem's own file: start over rather than refuse
    setError(QString());
    for (auto it = settings.cbegin(); it != settings.cend(); ++it)
        if (!isStateKey(it.key()))
            map.insert(it.key(), it.value());
    for (auto it = state.cbegin(); it != state.cend(); ++it)
        if (isStateKey(it.key()))
            map.insert(it.key(), it.value());
    return true;
}

bool writeToml(QIODevice& device, const QSettings::SettingsMap& map)
{
    QVariantMap settings, state;
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        (isStateKey(it.key()) ? state : settings).insert(it.key(), it.value());
    writeText(stateFilePath(), generate(state, kStateHeader));

    // The settings file: change only the lines whose value differs.
    auto* file = qobject_cast<QFileDevice*>(&device);  // QSettings hands a QSaveFile
    QString text = file ? readText(file->fileName()) : QString();
    QVariantMap before;
    if (!parseToml(text, &before)) {
        // Hand edits that don't parse: never overwrite them.
        return false;
    }
    if (text.isEmpty())
        text = kSettingsHeader;
    auto split = [](const QString& k) {
        const int slash = int(k.lastIndexOf(QLatin1Char('/')));
        return std::pair{slash < 0 ? QString() : k.left(slash), k.mid(slash + 1)};
    };
    for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
        const QString value = tomlValueText(it.value());
        if (value.isEmpty())
            continue;
        if (before.contains(it.key()) && tomlValueText(before.value(it.key())) == value)
            continue;
        const auto [table, key] = split(it.key());
        text = setTomlLine(text, table, key, value);
    }
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (settings.contains(it.key()) || isStateKey(it.key()))
            continue;
        const auto [table, key] = split(it.key());
        text = setTomlLine(text, table, key, std::nullopt);
    }
    // State that was in the settings file (moved there by hand) leaves it.
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (!isStateKey(it.key()))
            continue;
        const auto [table, key] = split(it.key());
        text = setTomlLine(text, table, key, std::nullopt);
    }
    return device.write(text.toUtf8()) >= 0;
}

}  // namespace

QSettings::Format tomlFormat()
{
    static const QSettings::Format f = QSettings::registerFormat(QStringLiteral("toml"), readToml, writeToml);
    return f;
}

QString stateFilePath()
{
    const QString dir = configDir();
    if (!dir.isEmpty()) {
        // Next to the settings, keys and themes: <dir>/<organization>/. Worked
        // out here, not with QSettings: this runs inside QSettings' own reading.
        QString org = QCoreApplication::organizationName();
        if (org.isEmpty())
            org = QStringLiteral("Unknown Organization");
        return dir + QLatin1Char('/') + org + QStringLiteral("/state.toml");
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/tandem-fm/state.toml");
}

QString keysFilePath()
{
    return QFileInfo(QSettings().fileName()).path() + QStringLiteral("/keys.toml");
}

bool isStateKey(const QString& key)
{
    static const QStringList exact = {
        QStringLiteral("window/geometry"),     QStringLiteral("window/panelSizes"),
        QStringLiteral("window/active"),       QStringLiteral("viewer/geometry"),
        QStringLiteral("view/previewHeight"),  QStringLiteral("view/previewHeightEach"),
        QStringLiteral("view/previewWidth"),   QStringLiteral("view/rows2"),
        QStringLiteral("network/recent"),      QStringLiteral("commands/history"),
        QStringLiteral("checksums/algo"),      QStringLiteral("links/kind"),
        QStringLiteral("window/splitter"),     QStringLiteral("palette/recent"),
    };
    static const QStringList groups = {
        QStringLiteral("left/"), QStringLiteral("right/"), QStringLiteral("plugins/"),
        QStringLiteral("multiRename/"), QStringLiteral("sync/"), QStringLiteral("shelves/"),
    };
    if (exact.contains(key))
        return true;
    for (const QString& g : groups)
        if (key.startsWith(g))
            return true;
    return false;
}

QString tomlSettingsError()
{
    QMutexLocker lock(&g_errorMutex);
    return g_error;
}

// Old INI files hold every value as text: make them proper values again.
static QVariant typed(const QVariant& v)
{
    if (v.typeId() == QMetaType::QStringList) {
        QVariantList out;
        bool changed = false;
        for (const QString& s : v.toStringList()) {
            const QVariant t = typed(s);
            changed = changed || t.typeId() != QMetaType::QString;
            out.append(t);
        }
        if (!changed)
            return v;
        for (const QVariant& t : out)  // all of one kind, or leave it as text
            if (t.typeId() != out.first().typeId())
                return v;
        return out;
    }
    if (v.typeId() != QMetaType::QString)
        return v;
    const QString s = v.toString();
    if (s == QLatin1String("true") || s == QLatin1String("false"))
        return s == QLatin1String("true");
    static const QRegularExpression number(QStringLiteral("^-?(0|[1-9][0-9]{0,8})$"));
    if (number.match(s).hasMatch())
        return s.toInt();
    return v;
}

void migrateOldSettings(QSettings::Format oldFormat, const QString& oldPath, QVariantMap* keysOut)
{
    QSettings now;
    if (QFile::exists(now.fileName()))
        return;
    std::unique_ptr<QSettings> old = oldPath.isEmpty()
        ? std::make_unique<QSettings>(oldFormat, QSettings::UserScope, QCoreApplication::organizationName(),
                                      QCoreApplication::applicationName())
        : std::make_unique<QSettings>(oldPath, oldFormat);
    const QStringList keys = old->allKeys();
    if (keys.isEmpty())
        return;
    for (const QString& k : keys) {
        if (k.startsWith(QLatin1String("keys/"))) {
            if (keysOut)
                keysOut->insert(k, old->value(k));
            continue;
        }
        now.setValue(k, typed(old->value(k)));
    }
    now.sync();
    const QString file = old->fileName();
    old.reset();
    if (file.endsWith(QLatin1String(".conf")) || file.endsWith(QLatin1String(".ini")))
        QFile::rename(file, file + QStringLiteral(".bak"));  // kept, out of the way
}

// ------------------------------------------------------------------ text

QString tomlValueText(const QVariant& v)
{
    switch (v.typeId()) {
    case QMetaType::Bool:
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
        return QString::number(v.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float: {
        QString s = QString::number(v.toDouble(), 'g', 15);
        if (!s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e')) && !s.contains(QLatin1String("inf")) &&
            !s.contains(QLatin1String("nan")))
            s += QStringLiteral(".0");
        return s;
    }
    case QMetaType::QByteArray:
        return tomlValueText(QStringLiteral("base64:") + QString::fromLatin1(v.toByteArray().toBase64()));
    case QMetaType::QStringList:
    case QMetaType::QVariantList: {
        QStringList parts;
        for (const QVariant& e : v.toList())
            parts << tomlValueText(e);
        return QLatin1Char('[') + parts.join(QStringLiteral(", ")) + QLatin1Char(']');
    }
    default:
        break;
    }
    if (!v.isValid())
        return {};
    QString s = v.toString();
    QString out = QStringLiteral("\"");
    for (QChar c : s) {
        switch (c.unicode()) {
        case '"': out += QStringLiteral("\\\""); break;
        case '\\': out += QStringLiteral("\\\\"); break;
        case '\n': out += QStringLiteral("\\n"); break;
        case '\t': out += QStringLiteral("\\t"); break;
        case '\r': out += QStringLiteral("\\r"); break;
        default:
            if (c.unicode() < 0x20)
                out += QStringLiteral("\\u%1").arg(int(c.unicode()), 4, 16, QLatin1Char('0'));
            else
                out += c;
        }
    }
    return out + QLatin1Char('"');
}

namespace {

// "[a.b]" or [a."b c"] -> "a/b"; empty when the line is no table header.
std::optional<QString> headerTable(const QString& line)
{
    const QString t = line.trimmed();
    if (!t.startsWith(QLatin1Char('[')) || t.startsWith(QLatin1String("[[")))
        return std::nullopt;
    const int end = int(t.lastIndexOf(QLatin1Char(']')));
    if (end < 0)
        return std::nullopt;
    QStringList parts;
    QString cur;
    bool quoted = false;
    for (QChar c : t.mid(1, end - 1)) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (c == QLatin1Char('.') && !quoted) {
            parts << cur.trimmed();
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts << cur.trimmed();
    return parts.join(QLatin1Char('/'));
}

// Does the line set key (also when commented out with "#")?
bool setsKey(const QString& line, const QString& key, bool commented)
{
    QString t = line.trimmed();
    if (commented) {
        if (!t.startsWith(QLatin1Char('#')))
            return false;
        t = t.mid(1).trimmed();
    }
    const QString k = keyText(key);
    if (!t.startsWith(k))
        return false;
    const QString rest = t.mid(k.size()).trimmed();
    return rest.startsWith(QLatin1Char('='));
}

}  // namespace

QString setTomlLine(const QString& text, const QString& table, const QString& key,
                    const std::optional<QString>& valueText, const QString& commentedDefault)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    const bool endsWithNewline = text.endsWith(QLatin1Char('\n'));
    if (endsWithNewline)
        lines.removeLast();
    // The table's lines: [start, end).
    int start = -1, end = -1;
    if (table.isEmpty()) {
        start = 0;
        end = int(lines.size());
        for (int i = 0; i < lines.size(); ++i)
            if (headerTable(lines[i])) {
                end = i;
                break;
            }
    } else {
        for (int i = 0; i < lines.size(); ++i) {
            const auto h = headerTable(lines[i]);
            if (start < 0) {
                if (h && *h == table)
                    start = i + 1;
            } else if (h) {
                end = i;
                break;
            }
        }
        if (start >= 0 && end < 0)
            end = int(lines.size());
    }
    const QString newLine = valueText ? keyText(key) + QStringLiteral(" = ") + *valueText : QString();
    int active = -1, commented = -1;
    for (int i = qMax(0, start); start >= 0 && i < end; ++i) {
        if (active < 0 && setsKey(lines[i], key, false))
            active = i;
        else if (commented < 0 && setsKey(lines[i], key, true))
            commented = i;
    }
    if (valueText) {
        if (active >= 0) {
            lines[active] = newLine;
        } else if (commented >= 0) {
            lines[commented] = newLine;
        } else if (start >= 0) {
            int at = end;  // after the table's last line that isn't blank
            while (at > start && lines[at - 1].trimmed().isEmpty())
                --at;
            lines.insert(at, newLine);
        } else {
            while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
                lines.removeLast();
            lines << QString() << QStringLiteral("[%1]").arg(tableText(table)) << newLine;
        }
    } else if (active >= 0) {
        if (!commentedDefault.isEmpty() && commented < 0)
            lines[active] = QStringLiteral("# ") + keyText(key) + QStringLiteral(" = ") + commentedDefault;
        else
            lines.removeAt(active);
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool parseToml(const QString& text, QVariantMap* out, QString* error)
{
    const QByteArray utf8 = text.toUtf8();
    toml::parse_result r = toml::parse(std::string_view(utf8.constData(), size_t(utf8.size())));
    if (!r) {
        if (error) {
            const toml::parse_error& e = r.error();
            *error = QObject::tr("line %1: %2")
                         .arg(e.source().begin.line)
                         .arg(QString::fromUtf8(e.description().data(), qsizetype(e.description().size())));
        }
        return false;
    }
    flatten(r.table(), QString(), out);
    return true;
}

}  // namespace tandem
