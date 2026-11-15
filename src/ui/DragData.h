#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

class QMimeData;

namespace tandem {

// Tandem's own clipboard and drag format: locations as Tandem writes them,
// server ones included, separated by NUL. Other programs get the usual URL
// list.
extern const QString kLocationsMime;

QUrl locationUrl(const QString& location);  // sftp://… stays a server URL
// URLs for other programs, our list for Tandem (the caller owns it).
QMimeData* locationsMimeData(const QStringList& locations);
// Our list when it's there, else the local files among the URLs.
QStringList locationsOf(const QMimeData* mime);

}  // namespace tandem
