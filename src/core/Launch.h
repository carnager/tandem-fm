#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace tandem {

// Every program Tandem starts for the user (apps, editors, terminals, the
// diff tool) goes through here, and so does every file or URL it hands to
// the desktop. That lets tests make sure nothing real starts on the desktop.
bool launchDetached(const QString& program, const QStringList& args, const QString& workDir = {});

// Wayland: a token from the compositor that lets the started app's window
// take the focus (else it may open unfocused, e.g. on another workspace).
// Asked for on each launch from the UI thread; "" when there is none.
void setActivationTokenProvider(std::function<QString()> provider);

// Tests: a result pretends the launch happened (true) or failed (false);
// nullopt lets it really run. URLs come as program "open-url", the URL in args.
using LaunchGuard = std::function<std::optional<bool>(const QString& program, const QStringList& args)>;
void setLaunchGuardForTests(LaunchGuard guard);
std::optional<bool> guardLaunch(const QString& program, const QStringList& args);

}  // namespace tandem
