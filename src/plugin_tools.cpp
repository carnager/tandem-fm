// Read-only example of Tandem's external JSON action protocol.
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QFile input; if (!input.open(stdin, QIODevice::ReadOnly)) return 1;
    const QByteArray bytes = input.read(4 * 1024 * 1024 + 1);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (bytes.size() > 4 * 1024 * 1024 || !doc.isObject() || error.error != QJsonParseError::NoError) return 2;
    const auto request = doc.object();
    if (request.value("schema").toInt() != 1 || request.value("app").toString() != "tandem" || !request.value("selection").isArray()) return 2;
    const QString action = request.value("action").toString();
    const auto selection = request.value("selection").toArray();
    const auto settings = request.value("settings").toObject();
    QByteArray result;
    if (action == "copy-paths" || action == "copy-names") {
        QStringList paths;
        for (const auto& value : selection) paths << value.toObject().value(action == "copy-paths" ? "path" : "name").toString();
        result = paths.join(settings.value("separator").toString() == "Tabs" ? '\t' : '\n').toUtf8();
    } else if (action == "inspect") {
        QJsonArray items;
        for (const auto& value : selection) {
            const auto file = value.toObject();
            QString detail = file.value("is_directory").toBool() ? QObject::tr("Directory") :
                file.value("size").isNull() ? QObject::tr("Size unavailable") : QObject::tr("%1 bytes").arg(file.value("size").toInteger());
            if (file.value("is_link").toBool()) detail += QObject::tr(" · symbolic link");
            if (settings.value("show-dates").toBool(true) && !file.value("modified").isNull())
                detail += QStringLiteral(" · ") + QDateTime::fromSecsSinceEpoch(file.value("modified").toInteger()).toString(Qt::ISODate);
            detail += QStringLiteral(" · ") + file.value("path").toString();
            items.append(QJsonObject{{"text", file.value("name").toString()}, {"detail", detail}});
        }
        result = QJsonDocument(QJsonObject{{"title", QObject::tr("Selection report — %1 items").arg(selection.size())}, {"items", items}}).toJson(QJsonDocument::Compact);
    } else return 2;
    QFile output; if (!output.open(stdout, QIODevice::WriteOnly)) return 1;
    return output.write(result) == result.size() && output.flush() ? 0 : 1;
}
