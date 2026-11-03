#!/usr/bin/env python3
"""Reject a macOS bundle with missing or non-system external libraries."""
import pathlib
import subprocess
import sys

app = pathlib.Path(sys.argv[1]).resolve()
frameworks = app / 'Contents/Frameworks'
executable_dir = app / 'Contents/MacOS'
magic_numbers = {bytes.fromhex(h) for h in ('cffaedfe', 'cefaedfe', 'cafebabe', 'bebafeca', 'cafebabf')}
issues = []
count = 0
for binary in app.rglob('*'):
    if binary.is_symlink() or not binary.is_file():
        continue
    with binary.open('rb') as stream:
        if stream.read(4) not in magic_numbers:
            continue
    count += 1
    identities = subprocess.check_output(['otool', '-D', str(binary)], text=True).splitlines()[1:]
    identities = {line.strip() for line in identities}
    lines = subprocess.check_output(['otool', '-L', str(binary)], text=True).splitlines()[1:]
    for line in lines:
        dependency = line.strip().split(' (')[0]
        if not dependency or dependency in identities:
            continue
        if dependency.startswith(('/System/Library/', '/usr/lib/')):
            continue
        if dependency.startswith('@executable_path/'):
            resolved = executable_dir / dependency.removeprefix('@executable_path/')
        elif dependency.startswith('@loader_path/'):
            resolved = binary.parent / dependency.removeprefix('@loader_path/')
        elif dependency.startswith('@rpath/'):
            resolved = frameworks / dependency.removeprefix('@rpath/')
        else:
            issues.append(f'{binary.relative_to(app)}: external dependency {dependency}')
            continue
        resolved = resolved.resolve()
        if not resolved.is_relative_to(app) or not resolved.exists():
            issues.append(f'{binary.relative_to(app)}: missing bundled dependency {dependency}')
if issues:
    raise SystemExit('\n'.join(issues))
print(f'Bundle verified: {count} Mach-O files, no missing or external non-system libraries')
