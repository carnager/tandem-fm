#pragma once

#include <QString>

#include <optional>

namespace tandem {

// GVfs exposes its mounts as folders under /run/user/<uid>/gvfs/, named like
// "sftp:host=example.com,user=bob". These helpers turn them into readable
// labels and tell remote paths apart (e.g. to skip git there).
QString gvfsDirectory();

struct RemoteRoot {
    QString root;   // the mount folder
    QString label;  // "sftp://bob@example.com"
    QString scheme; // "sftp", "smb", "davs", …
};
std::optional<RemoteRoot> remoteRootOf(const QString& path);
// Same, for a GVfs folder name alone ("sftp:host=x,user=y").
RemoteRoot describeGvfsName(const QString& name);
bool isRemotePath(const QString& path);
// A GVfs sftp/smb folder as the native location Tandem handles itself
// ("/run/user/1000/gvfs/sftp:host=x/srv" → "sftp://x/srv"); empty otherwise.
QString nativeLocationFor(const QString& path);

// "host", "sftp://host/path" or "smb://nas/share" → a URI GIO can mount.
// Plain "user@host[:path]" is taken as SFTP. Empty if it doesn't look like one.
QString normalizeServerUri(const QString& text);

}  // namespace tandem
