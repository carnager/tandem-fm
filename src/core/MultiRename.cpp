#include "core/MultiRename.h"

#include "core/Backend.h"

#include "core/FileOps.h"
#include "core/Sorting.h"
#include "core/Util.h"

#include <QDateTime>
#include <QHash>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <cerrno>
#include <cstdio>

namespace tandem {

namespace {

struct Context {
    QString file, name, ext, folder;
    int64_t mtime = 0, taken = 0;
    int index = 0;
};

// Python-style slice "[a:b]" / "[a]" of s; ok=false when malformed.
QString slice(const QString& s, const QString& spec, bool* ok)
{
    static const QRegularExpression rx(QStringLiteral("^(-?\\d*)(?::(-?\\d*))?$"));
    const auto m = rx.match(spec);
    if (!m.hasMatch() || spec.isEmpty()) {
        *ok = false;
        return {};
    }
    const int n = int(s.size());
    auto clamp = [n](int v) { return qBound(0, v < 0 ? n + v : v, n); };
    if (!m.hasCaptured(2)) {  // single index
        const int i = m.captured(1).toInt();
        const int at = i < 0 ? n + i : i;
        return at >= 0 && at < n ? s.mid(at, 1) : QString();
    }
    const int from = m.captured(1).isEmpty() ? 0 : clamp(m.captured(1).toInt());
    const int to = m.captured(2).isEmpty() ? n : clamp(m.captured(2).toInt());
    return to > from ? s.mid(from, to - from) : QString();
}

QString titleCase(const QString& s)
{
    QString out = s.toLower();
    bool start = true;
    for (QChar& ch : out) {
        if (start && ch.isLetter())
            ch = ch.toUpper();
        start = !ch.isLetterOrNumber() && ch != QLatin1Char('\'');
    }
    return out;
}

// One {field[slice]:arg|filter|…}; error set on unknown parts.
QString expandField(const QString& body, const Context& c, QString* error)
{
    static const QRegularExpression rx(QStringLiteral("^([a-z]+)(?:\\[([^\\]]*)\\])?(?::([^|]*))?((?:\\|[^|]*)*)$"));
    const auto m = rx.match(body.trimmed());
    if (!m.hasMatch()) {
        *error = QObject::tr("Can't read {%1}").arg(body);
        return {};
    }
    const QString key = m.captured(1), arg = m.captured(3);
    QString v;
    if (key == QLatin1String("name"))
        v = c.name;
    else if (key == QLatin1String("ext"))
        v = c.ext;
    else if (key == QLatin1String("file"))
        v = c.file;
    else if (key == QLatin1String("folder"))
        v = c.folder;
    else if (key == QLatin1String("date") || key == QLatin1String("taken")) {
        const int64_t t = key == QLatin1String("taken") && c.taken ? c.taken : c.mtime;
        v = QDateTime::fromSecsSinceEpoch(t).toString(arg.isEmpty() ? QStringLiteral("yyyy-MM-dd") : arg);
    } else if (key == QLatin1String("n")) {
        // "{n:007+2}": start 7, 3 digits, step 2 — written like the first number.
        static const QRegularExpression nr(QStringLiteral("^(\\d*)(?:\\+(-?\\d+))?$"));
        const auto nm = nr.match(arg);
        if (!nm.hasMatch()) {
            *error = QObject::tr("Write the counter like the first number: {n:001}, {n:10+5}");
            return {};
        }
        const QString first = nm.captured(1);
        const qint64 start = first.isEmpty() ? 1 : first.toLongLong();
        const qint64 step = nm.captured(2).isEmpty() ? 1 : nm.captured(2).toLongLong();
        v = QStringLiteral("%1").arg(start + c.index * step, qMax(1, int(first.size())), 10, QLatin1Char('0'));
    } else {
        *error = QObject::tr("Unknown field {%1}").arg(key);
        return {};
    }
    if (m.hasCaptured(2)) {
        bool ok = true;
        v = slice(v, m.captured(2), &ok);
        if (!ok) {
            *error = QObject::tr("Write parts like {name[0:4]} or {name[-3:]}");
            return {};
        }
    }
    for (const QString& f : m.captured(4).split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
        const QString filter = f.trimmed();
        if (filter == QLatin1String("lower"))
            v = v.toLower();
        else if (filter == QLatin1String("upper"))
            v = v.toUpper();
        else if (filter == QLatin1String("title"))
            v = titleCase(v);
        else if (filter == QLatin1String("trim"))
            v = v.trimmed();
        else {
            *error = QObject::tr("Unknown filter |%1 (lower, upper, title, trim)").arg(filter);
            return {};
        }
    }
    return v;
}

QString expandTemplate(const QString& t, const Context& c, QString* error)
{
    QString out;
    for (int i = 0; i < t.size(); ++i) {
        const QChar ch = t[i];
        if ((ch == QLatin1Char('{') || ch == QLatin1Char('}')) && i + 1 < t.size() && t[i + 1] == ch) {
            out += ch;  // {{ and }}
            ++i;
            continue;
        }
        if (ch != QLatin1Char('{')) {
            out += ch;
            continue;
        }
        const int close = int(t.indexOf(QLatin1Char('}'), i + 1));
        if (close < 0) {
            *error = QObject::tr("A { is not closed");
            return {};
        }
        out += expandField(t.mid(i + 1, close - i - 1), c, error);
        if (!error->isEmpty())
            return {};
        i = close;
    }
    return out;
}

QString applyCase(const QString& s, RenameOptions::Case mode)
{
    switch (mode) {
    case RenameOptions::Case::Keep:
        return s;
    case RenameOptions::Case::Lower:
        return s.toLower();
    case RenameOptions::Case::Upper:
        return s.toUpper();
    case RenameOptions::Case::Sentence:
        return s.isEmpty() ? s : s.left(1).toUpper() + s.mid(1).toLower();
    case RenameOptions::Case::Title:
        return titleCase(s);
    }
    return s;
}

void validatePlan(RenamePlan& plan, const QSet<QString>& existing)
{
    // Validate: names, duplicates, collisions with files outside the set.
    QHash<QString, int> targetCount;
    QSet<QString> leaving;  // old names that will be free afterwards
    for (const RenameRow& r : std::as_const(plan.rows)) {
        targetCount[r.newName] += 1;
        if (r.newName != r.oldName)
            leaving.insert(r.oldName);
    }
    for (RenameRow& r : plan.rows) {
        if (r.newName == r.oldName) {
            r.status = RenameRow::Status::Unchanged;
            continue;
        }
        if (r.newName.isEmpty() || r.newName == QLatin1String(".") || r.newName == QLatin1String("..") ||
            r.newName.contains(QLatin1Char('/')) || r.newName.contains(QChar(0))) {
            r.status = RenameRow::Status::Invalid;
            r.note = QObject::tr("not a valid file name");
        } else if (targetCount.value(r.newName) > 1) {
            r.status = RenameRow::Status::Conflict;
            r.note = QObject::tr("same new name as another file");
        } else if (existing.contains(r.newName) && !leaving.contains(r.newName)) {
            r.status = RenameRow::Status::Conflict;
            r.note = QObject::tr("a file with this name exists");
        } else {
            r.status = RenameRow::Status::Ok;
        }
        if (r.status == RenameRow::Status::Ok)
            ++plan.changes;
        else
            ++plan.problems;
    }
}

}  // namespace

RenamePlan buildRenamePlan(const QList<RenameItem>& items, const QString& parentFolderName,
                           const RenameOptions& opt, const QSet<QString>& existing)
{
    RenamePlan plan;
    QRegularExpression rx;
    QString replacement = opt.replace;
    if (opt.regex) {
        // Groups are written $1 (like most tools); Qt wants \1.
        QString converted;
        for (int i = 0; i < replacement.size(); ++i) {
            if (replacement[i] == QLatin1Char('$') && i + 1 < replacement.size() && replacement[i + 1].isDigit())
                converted += QLatin1Char('\\');
            else
                converted += replacement[i];
        }
        replacement = converted;
    }
    if (!opt.find.isEmpty()) {
        const QString pattern = opt.regex ? opt.find : QRegularExpression::escape(opt.find);
        rx = QRegularExpression(pattern, opt.matchCase ? QRegularExpression::NoPatternOption
                                                       : QRegularExpression::CaseInsensitiveOption);
        if (!rx.isValid()) {
            plan.error = QObject::tr("Invalid regular expression: %1").arg(rx.errorString());
            return plan;
        }
    }

    for (int i = 0; i < items.size(); ++i) {
        const RenameItem& it = items[i];
        Context c;
        c.file = it.name;
        const QString e = it.isDir ? QString() : extensionOf(it.name).toString();
        c.ext = e.isEmpty() ? QString() : QLatin1Char('.') + e;
        c.name = it.name.left(it.name.size() - c.ext.size());
        c.folder = parentFolderName;
        c.mtime = it.mtime;
        c.taken = it.taken;
        c.index = i;

        QString error;
        QString full = expandTemplate(opt.pattern, c, &error);
        if (!error.isEmpty()) {
            plan.error = error;
            plan.rows.clear();
            return plan;
        }
        if (!opt.find.isEmpty()) {
            if (opt.regex)
                full.replace(rx, replacement);
            else
                full.replace(opt.find, opt.replace, opt.matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive);
        }
        if (opt.caseMode != RenameOptions::Case::Keep) {
            // The style is for the name; ".Jpg" or ".JPG" helps nobody.
            const QString x = it.isDir ? QString() : extensionOf(full).toString();
            const QString stem = x.isEmpty() ? full : full.left(full.size() - x.size() - 1);
            full = applyCase(stem, opt.caseMode) + (x.isEmpty() ? QString() : QLatin1Char('.') + x);
        }

        RenameRow row;
        row.oldName = it.name;
        row.newName = full;
        plan.rows.append(row);
    }

    validatePlan(plan, existing);
    return plan;
}

RenamePlan buildRenamePlanFromNames(const QList<RenameItem>& items, const QStringList& newNames,
                                    const QSet<QString>& existing)
{
    RenamePlan plan;
    for (int i = 0; i < items.size() && i < newNames.size(); ++i) {
        RenameRow row;
        row.oldName = items[i].name;
        row.newName = newNames[i];
        plan.rows.append(row);
    }
    validatePlan(plan, existing);
    return plan;
}

QList<QPair<QString, QString>> executeRenamePlan(const QString& dir, const RenamePlan& plan, QStringList* errors)
{
    QList<QPair<QString, QString>> done;
    QSet<QString> oldNames;
    for (const RenameRow& r : plan.rows)
        if (r.status == RenameRow::Status::Ok)
            oldNames.insert(r.oldName);

    // Phase 1: anything whose target is still occupied by another member of
    // the set moves to a temporary name first.
    struct Step {
        QString from, to, original;
    };
    QList<Step> direct, viaTemp;
    for (const RenameRow& r : plan.rows) {
        if (r.status != RenameRow::Status::Ok)
            continue;
        if (oldNames.contains(r.newName))
            viaTemp.append({r.oldName, r.newName, r.oldName});
        else
            direct.append({r.oldName, r.newName, r.oldName});
    }
    const QString tag = QString::number(QRandomGenerator::global()->generate(), 36);
    for (int i = 0; i < viaTemp.size(); ++i) {
        Step& s = viaTemp[i];
        const QString temp = QStringLiteral(".tandem-rename-%1-%2").arg(tag).arg(i);
        if (const int e = renameNoReplaceAt(joinPath(dir, s.from), joinPath(dir, temp))) {
            errors->append(QObject::tr("%1: %2").arg(s.from, errnoString(e)));
            s.to.clear();  // skip
            continue;
        }
        s.from = temp;
    }
    auto finish = [&](const Step& s) {
        if (s.to.isEmpty())
            return;
        if (const int e = renameNoReplaceAt(joinPath(dir, s.from), joinPath(dir, s.to))) {
            errors->append(QObject::tr("%1 → %2: %3").arg(s.original, s.to, errnoString(e)));
            if (s.from != s.original)  // put a temp-named file back
                renameNoReplaceAt(joinPath(dir, s.from), joinPath(dir, s.original));
            return;
        }
        done.append({s.to, s.original});
    };
    for (const Step& s : std::as_const(direct))
        finish(s);
    for (const Step& s : std::as_const(viaTemp))
        finish(s);
    return done;
}

}  // namespace tandem
