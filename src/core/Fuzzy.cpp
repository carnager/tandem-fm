#include "core/Fuzzy.h"

#include <QStringList>

namespace tandem {

namespace {

bool isBoundary(QStringView text, int i)
{
    if (i == 0)
        return true;
    const QChar prev = text[i - 1];
    const QChar cur = text[i];
    if (prev == QLatin1Char(' ') || prev == QLatin1Char('/') || prev == QLatin1Char('-') || prev == QLatin1Char('_') ||
        prev == QLatin1Char('.'))
        return true;
    return prev.isLower() && cur.isUpper();  // camelCase
}

// Best-effort score of one term starting the search at `from`.
// Tries every possible first-character position and keeps the best.
int scoreTerm(QStringView term, QStringView text, int from, QList<int>* positions, int* end)
{
    int best = -1;
    QList<int> bestPos;
    int bestEnd = from;
    for (int start = from; start < text.size(); ++start) {
        if (text[start].toLower() != term[0].toLower())
            continue;
        QList<int> pos{start};
        int score = isBoundary(text, start) ? 30 : 0;
        if (start == from)
            score += 5;
        int ti = 1;
        int last = start;
        for (int i = start + 1; i < text.size() && ti < term.size(); ++i) {
            if (text[i].toLower() != term[ti].toLower())
                continue;
            if (i == last + 1)
                score += 15;  // consecutive run
            else
                score -= qMin(10, i - last - 1);  // gap penalty
            if (isBoundary(text, i))
                score += 20;
            pos.append(i);
            last = i;
            ++ti;
        }
        if (ti < term.size())
            break;  // later starts can't match either
        score += 10 * int(term.size());
        if (score > best) {
            best = score;
            bestPos = pos;
            bestEnd = last + 1;
        }
    }
    if (best >= 0) {
        if (positions)
            positions->append(bestPos);
        *end = bestEnd;
    }
    return best;
}

}  // namespace

int fuzzyScore(QStringView pattern, QStringView text, QList<int>* positions)
{
    const QList<QStringView> terms = pattern.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (terms.isEmpty())
        return 0;
    int total = 0;
    int from = 0;
    for (QStringView term : terms) {
        int end = from;
        const int s = scoreTerm(term, text, from, positions, &end);
        if (s < 0)
            return -1;
        total += s;
        from = end;
    }
    // Shorter texts win ties: "Copy" beats "Copy full paths as text" for "copy".
    return total * 100 / (100 + int(text.size()));
}

}  // namespace tandem
