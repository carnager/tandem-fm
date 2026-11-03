#!/usr/bin/env python3
"""Complete Homebrew Qt deployment when macdeployqt misses plugin rpaths.

macdeployqt handles Qt resources and plugin selection. This pass copies missing
linked frameworks/libraries and rewrites every non-system load reference to the
bundle, without changing the Homebrew installation.
"""
import pathlib
import shutil
import subprocess
import sys

app = pathlib.Path(sys.argv[1]).resolve()
qt_lib = pathlib.Path(sys.argv[2]) / 'lib'
brew_lib = pathlib.Path(sys.argv[3]) / 'lib'
frameworks = app / 'Contents/Frameworks'
main = app / 'Contents/MacOS/tandem-fm'
magic = {bytes.fromhex(h) for h in ('cffaedfe', 'cefaedfe', 'cafebabe', 'bebafeca', 'cafebabf')}


def run(*args):
    return subprocess.check_output(args, text=True)


def binaries():
    for file in app.rglob('*'):
        if file.is_symlink() or not file.is_file():
            continue
        with file.open('rb') as stream:
            if stream.read(4) in magic:
                yield file


def relative_name(dependency):
    parts = pathlib.PurePosixPath(dependency).parts
    for i, part in enumerate(parts):
        if part.endswith('.framework'):
            return pathlib.Path(*parts[i:])
    return pathlib.Path(parts[-1])


processed = set()
while True:
    pending = [file for file in binaries() if file not in processed]
    if not pending:
        break
    for binary in pending:
        processed.add(binary)
        identities = {line.strip() for line in run('otool', '-D', str(binary)).splitlines()[1:]}
        dependencies = run('otool', '-L', str(binary)).splitlines()[1:]
        for line in dependencies:
            dependency = line.strip().split(' (')[0]
            if not dependency or dependency in identities or dependency.startswith(('/usr/lib/', '/System/Library/')):
                continue
            relative = relative_name(dependency)
            bundled = frameworks / relative
            if not bundled.exists():
                candidates = [qt_lib / relative, brew_lib / relative]
                if dependency.startswith('/'):
                    candidates.insert(0, pathlib.Path(dependency))
                source = next((file for file in candidates if file.exists()), None)
                if source is None:
                    raise SystemExit(f'Cannot bundle {dependency} required by {binary}')
                if relative.parts[0].endswith('.framework'):
                    source_root = source
                    for _ in range(len(relative.parts) - 1):
                        source_root = source_root.parent
                    shutil.copytree(source_root.resolve(), frameworks / relative.parts[0], symlinks=True)
                else:
                    shutil.copy2(source.resolve(), bundled)
            new_dependency = '@executable_path/../Frameworks/' + str(relative)
            if dependency != new_dependency:
                subprocess.run(['install_name_tool', '-change', dependency, new_dependency, str(binary)], check=True)
        if identities and binary.is_relative_to(frameworks):
            identity = '@rpath/' + str(binary.relative_to(frameworks))
            subprocess.run(['install_name_tool', '-id', identity, str(binary)], check=True)
        commands = run('otool', '-l', str(binary)).splitlines()
        for index, line in enumerate(commands):
            if line.strip() == 'cmd LC_RPATH':
                path = commands[index + 2].strip().removeprefix('path ').split(' (offset')[0]
                if path.startswith(('/opt/', '/usr/local/')):
                    subprocess.run(['install_name_tool', '-delete_rpath', path, str(binary)], check=True)
# Every linked reference above is explicit; this also serves dynamically loaded
# Qt components that use @rpath IDs.
commands = run('otool', '-l', str(main))
if 'path @executable_path/../Frameworks ' not in commands:
    subprocess.run(['install_name_tool', '-add_rpath', '@executable_path/../Frameworks', str(main)], check=True)
print(f'Completed dependency relocation for {len(processed)} Mach-O files')
