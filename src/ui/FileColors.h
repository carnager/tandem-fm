#pragma once
#include "core/FileEntry.h"
#include <QColor>
#include <QHash>
#include <QList>
#include <QStringList>

namespace tandem {
struct FileColorRule {
    QString match = QStringLiteral("extension"); // extension, mime, kind
    QString value;
    QColor light = QColor(QStringLiteral("#2f6fed"));
    QColor dark = QColor(QStringLiteral("#6c9cff"));
    bool enabled = true;
    QString error() const;
};

// Filename-only MIME lookup: never opens a file or contacts a server while painting.
class FileColors {
public:
    static FileColors& instance();
    static QList<FileColorRule> rules();
    static void saveRules(const QList<FileColorRule>& rules);
    static void setEnabled(bool on);
    static void refreshViews();
    QColor nameColor(const FileEntry& entry, char gitStatus = 0, bool extension = false);
private:
    void load();
    bool matches(const FileColorRule& rule, const FileEntry& entry);
    QString m_json;
    bool m_loaded = false;
    bool m_enabled = true;
    QList<FileColorRule> m_rules;
    QHash<QString, QStringList> m_mimeNames;
    QHash<QString, QStringList> m_extensions;
};
} // namespace tandem
