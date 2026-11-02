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
    QString name, ext, parent;
    QDateTime time;
    int index = 0;
    const RenameOptions* opt = nullptr;
};

// Substring by TC rules: 1-based, negative = counted from the end.
QString part(const QString& s, const QString& spec, bool* ok)
{
    *ok = true;
    if (spec.isEmpty())
        return s;
    const int n = int(s.size());
    auto pos = [n](int v) { return v < 0 ? n + v + 1 : v; };  // -1 = last char
    static const QRegularExpression range(QStringLiteral("^(-?\\d+)(?:(-)(-?\\d+)?|,(\\d+))?$"));
    const auto m = range.match(spec);
    if (!m.hasMatch()) {
        *ok = false;
        return {};
    }
    int from = pos(m.captured(1).toInt());
    int to;
    if (!m.captured(4).isEmpty())
        to = from + m.captured(4).toInt() - 1;   // [N2,3]
    else if (!m.captured(3).isEmpty())
        to = pos(m.captured(3).toInt());         // [N2-5]
    else if (!m.captured(2).isEmpty())
        to = n;                                  // [N2-]
    else
        to = from;                               // [N3]
    from = qMax(1, from);
    to = qMin(n, to);
    if (to < from)
        return {};
    return s.mid(from - 1, to - from + 1);
}

QString expandMask(const QString& mask, const Context& c, QString* error)
{
    QString out;
    for (int i = 0; i < mask.size(); ++i) {
        const QChar ch = mask[i];
        if (ch != QLatin1Char('[')) {
            out += ch;
            continue;
        }
        const int close = int(mask.indexOf(QLatin1Char(']'), i + 1));
        if (close < 0) {
            *error = QObject::tr("Missing ] in the mask");
            return {};
        }
        // "[[]" and "[]]" are literal brackets.
        if (close == i + 1 && i + 2 < mask.size() && mask[i + 2] == QLatin1Char(']')) {
            out += QLatin1Char(']');
            i += 2;
            continue;
        }
        const QString tok = mask.mid(i + 1, close - i - 1);
        i = close;
        if (tok == QLatin1String("[")) {
            out += QLatin1Char('[');
            continue;
        }
        bool ok = true;
        const QDateTime& t = c.time;
        if (tok == QLatin1String("YMD"))
            out += t.toString(QStringLiteral("yyyyMMdd"));
        else if (tok == QLatin1String("hms"))
            out += t.toString(QStringLiteral("HHmmss"));
        else if (tok == QLatin1String("Y"))
            out += t.toString(QStringLiteral("yyyy"));
        else if (tok == QLatin1String("M"))
            out += t.toString(QStringLiteral("MM"));
        else if (tok == QLatin1String("D"))
            out += t.toString(QStringLiteral("dd"));
        else if (tok == QLatin1String("h"))
            out += t.toString(QStringLiteral("HH"));
        else if (tok == QLatin1String("m"))
            out += t.toString(QStringLiteral("mm"));
        else if (tok == QLatin1String("s"))
            out += t.toString(QStringLiteral("ss"));
        else if (tok == QLatin1String("P"))
            out += c.parent;
        else if (tok.startsWith(QLatin1Char('N')))
            out += part(c.name, tok.mid(1), &ok);
        else if (tok.startsWith(QLatin1Char('E')))
            out += part(c.ext, tok.mid(1), &ok);
        else if (tok.startsWith(QLatin1Char('C'))) {
            int start = c.opt->counterStart, step = c.opt->counterStep, digits = c.opt->counterDigits;
            if (tok.size() > 1) {  // [C10+5:3]
                static const QRegularExpression cr(QStringLiteral("^C(\\d+)?(?:\\+(-?\\d+))?(?::(\\d+))?$"));
                const auto m = cr.match(tok);
                if (!m.hasMatch()) {
                    ok = false;
                } else {
                    if (!m.captured(1).isEmpty())
                        start = m.captured(1).toInt();
                    if (!m.captured(2).isEmpty())
                        step = m.captured(2).toInt();
                    if (!m.captured(3).isEmpty())
                        digits = m.captured(3).toInt();
                }
            }
            out += QStringLiteral("%1").arg(start + c.index * step, qBound(1, digits, 12), 10, QLatin1Char('0'));
        } else {
            ok = false;
        }
        if (!ok) {
            *error = QObject::tr("Unknown placeholder [%1]").arg(tok);
            return {};
        }
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
    case RenameOptions::Case::FirstUpper:
        return s.isEmpty() ? s : s.left(1).toUpper() + s.mid(1).toLower();
    case RenameOptions::Case::Title: {
        QString out = s.toLower();
        bool start = true;
        for (QChar& ch : out) {
            if (start && ch.isLetter())
                ch = ch.toUpper();
            start = !ch.isLetterOrNumber() && ch != QLatin1Char('\'');
        }
        return out;
    }
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
        // TC and most tools write groups as $1; Qt wants \1.
        QString converted;
        for (int i = 0; i < replacement.size(); ++i) {
            if (replacement[i] == QLatin1Char('$') && i + 1 < replacement.size() && replacement[i + 1].isDigit())
                converted += QLatin1Char('\\');
            else
                converted += replacement[i];
        }
        replacement = converted;
    }
    if (!opt.search.isEmpty()) {
        const QString pattern = opt.regex ? opt.search : QRegularExpression::escape(opt.search);
        rx = QRegularExpression(pattern, opt.caseSensitive ? QRegularExpression::NoPatternOption
                                                           : QRegularExpression::CaseInsensitiveOption);
        if (!rx.isValid()) {
            plan.error = QObject::tr("Invalid regular expression: %1").arg(rx.errorString());
            return plan;
        }
    }

    for (int i = 0; i < items.size(); ++i) {
        const RenameItem& it = items[i];
        Context c;
        c.ext = it.isDir ? QString() : extensionOf(it.name).toString();
        c.name = c.ext.isEmpty() ? it.name : it.name.left(it.name.size() - c.ext.size() - 1);
        c.parent = parentFolderName;
        c.time = QDateTime::fromSecsSinceEpoch(it.mtime);
        c.index = i;
        c.opt = &opt;

        QString error;
        const QString name = expandMask(opt.nameMask, c, &error);
        const QString ext = error.isEmpty() ? expandMask(opt.extMask, c, &error) : QString();
        if (!error.isEmpty()) {
            plan.error = error;
            plan.rows.clear();
            return plan;
        }
        QString full = ext.isEmpty() ? name : name + QLatin1Char('.') + ext;
        if (!opt.search.isEmpty()) {
            if (opt.regex)
                full.replace(rx, replacement);
            else
                full.replace(opt.search, opt.replace, opt.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
        }
        if (opt.caseMode == RenameOptions::Case::FirstUpper || opt.caseMode == RenameOptions::Case::Title) {
            // Word-style casing is for the name; ".Jpg" helps nobody.
            const QString e = it.isDir ? QString() : extensionOf(full).toString();
            const QString stem = e.isEmpty() ? full : full.left(full.size() - e.size() - 1);
            full = applyCase(stem, opt.caseMode) + (e.isEmpty() ? QString() : QLatin1Char('.') + e);
        } else {
            full = applyCase(full, opt.caseMode);
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
