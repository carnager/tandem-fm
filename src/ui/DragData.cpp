#include "ui/DragData.h"

#include "core/Backend.h"

#include <QDir>
#include <QMimeData>

namespace tandem {

const QString kLocationsMime = QStringLiteral("application/x-tandem-locations");

QUrl locationUrl(const QString& location)
{
    if (!isRemoteLocation(location))
        return QUrl::fromLocalFile(location);
    QUrl u(locationRoot(location), QUrl::TolerantMode);
    u.setPath(locationPath(location));
    return u;
}

QMimeData* locationsMimeData(const QStringList& locations)
{
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString& l : locations)
        urls.append(locationUrl(l));
    mime->setUrls(urls);
    mime->setData(kLocationsMime, locations.join(QChar(0)).toUtf8());
    return mime;
}

QStringList locationsOf(const QMimeData* mime)
{
    if (!mime)
        return {};
    // Our own list first: it keeps server locations, which URLs from other
    // programs can't be trusted to.
    if (mime->hasFormat(kLocationsMime))
        return QString::fromUtf8(mime->data(kLocationsMime)).split(QChar(0), Qt::SkipEmptyParts);
    QStringList out;
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile())
            out.append(QDir::cleanPath(u.toLocalFile()));
    return out;
}

}  // namespace tandem
