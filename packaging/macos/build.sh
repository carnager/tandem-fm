#!/bin/bash
# Reproducible local macOS build. Dependencies are installed separately.
set -euo pipefail
cd "$(dirname "$0")/../.."
qt_prefix="$(brew --prefix qt)"
archive_prefix="$(brew --prefix libarchive)"
ssh_prefix="$(brew --prefix libssh)"
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$qt_prefix;$archive_prefix;$ssh_prefix" \
  -DLibArchive_ROOT="$archive_prefix" \
  -DLibArchive_LIBRARY="$archive_prefix/lib/libarchive.dylib"
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 300
mkdir -p dist
# Refuse to overwrite a previous package; keep locally tested builds available.
package_dir="dist/macos-$(date +%Y%m%d-%H%M%S)"
mkdir "$package_dir"
cp -R build/tandem-fm.app "$package_dir/Tandem.app"
"$qt_prefix/bin/macdeployqt" "$package_dir/Tandem.app" -always-overwrite -no-codesign \
  -libpath="$qt_prefix/lib" -libpath="$(brew --prefix)/lib" > "$package_dir/deployment.log" 2>&1
python3 packaging/macos/finish_bundle.py "$package_dir/Tandem.app" "$qt_prefix" "$(brew --prefix)"
python3 packaging/macos/verify_bundle.py "$package_dir/Tandem.app"
cp LICENSE "$package_dir/LICENSE"
cp packaging/macos/README.md packaging/macos/VALIDATION.md packaging/macos/CONFIGURATION.md "$package_dir/"
codesign --force --deep --sign - "$package_dir/Tandem.app"
codesign --verify --deep --strict "$package_dir/Tandem.app"
ditto -c -k --sequesterRsrc --keepParent "$package_dir/Tandem.app" "$package_dir/Tandem-macOS-0.1.4-$(uname -m).zip"
printf 'App: %s/Tandem.app\n' "$PWD/$package_dir"
