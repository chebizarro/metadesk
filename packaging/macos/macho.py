#!/usr/bin/env python3
"""Fail if any Mach-O slice in the supplied trees exceeds the macOS floor."""
import re
import subprocess
import sys
from pathlib import Path

MAGICS = {bytes.fromhex(x) for x in ('cffaedfe', 'feedfacf', 'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca')}


def versions(text):
    # otool emits one load-command list per architecture in a universal image.
    result = []
    for block in re.split(r'^Load command \d+', text, flags=re.M):
        if re.search(r'^\s*cmd LC_BUILD_VERSION$', block, re.M):
            field = 'minos'
        elif re.search(r'^\s*cmd LC_VERSION_MIN_MACOSX$', block, re.M):
            field = 'version'
        else:
            continue
        match = re.search(r'^\s*' + field + r'\s+(\d+(?:\.\d+){1,2})\s*$', block, re.M)
        if match:
            result.append(match[1])
    return result



def version_key(value):
    parts = tuple(map(int, value.split('.')))
    return parts + (0,) * (3 - len(parts))


def minimums(path):
    values = versions(subprocess.check_output(['otool', '-arch', 'all', '-l', str(path)], text=True))
    arches = subprocess.check_output(['lipo', '-archs', str(path)], text=True).split()
    if len(values) != len(arches):
        raise SystemExit(f'{path}: expected minimum for each slice {arches}, got {values}')
    return values


def main():
    floor = sys.argv[1]
    count = 0
    failed = False
    for arg in sys.argv[2:]:
        root = Path(arg)
        if not root.exists():
            sys.exit(f'FAIL: missing input {root}')
        for path in sorted(root.rglob('*')) if root.is_dir() else [root]:
            if not path.is_file() or path.is_symlink():
                continue
            with path.open('rb') as f:
                if f.read(4) not in MAGICS:
                    continue
            count += 1
            mins = minimums(path)
            print(f'{path}: minos={",".join(mins) or "MISSING"}')
            if not mins or any(version_key(v) > version_key(floor) for v in mins):
                failed = True
    if count == 0:
        sys.exit('FAIL: no Mach-O images inspected')
    if failed:
        sys.exit(f'FAIL: Mach-O minimum exceeds {floor} or is missing')
    print(f'PASS: {count} Mach-O images, all slices <= macOS {floor}')


if __name__ == '__main__':
    main()
