#!/usr/bin/env python3
"""Bind each producer manifest to the actual files on a mounted release DMG."""
import hashlib
import json
from pathlib import Path
import plistlib
import sys

from macho import MAGICS

PRODUCTS = {
    'Metadesk.app': ('metadesk-client', 'metadesk-host'),
    'Metadesk Manager.app': ('metadesk-macos-manager', 'metadesk-client'),
}


def measure(app, executables):
    contents = app / 'Contents'
    plist = plistlib.loads((contents / 'Info.plist').read_bytes())
    resources = contents / 'Resources'
    record = json.loads((resources / 'metadesk-bundle.json').read_bytes())
    bins = {path.name for path in (contents / 'MacOS').iterdir()}
    if plist['CFBundleExecutable'] != executables[0] or bins != set(executables):
        raise ValueError(f'{app}: unexpected executable layout')
    libraries = {path.name: path for path in (contents / 'Frameworks').iterdir()}
    if set(libraries) != set(record['private_libraries']):
        raise ValueError(f'{app}: declared libraries do not match DMG payload')
    images = set()
    for path in contents.rglob('*'):
        if path.is_symlink():
            raise ValueError(f'{app}: unexpected symlink {path.relative_to(app)}')
        if path.is_file():
            with path.open('rb') as stream:
                if stream.read(4) in MAGICS:
                    images.add(str(path.relative_to(app)))
    expected = {f'Contents/MacOS/{name}' for name in executables} | {
        f'Contents/Frameworks/{name}' for name in libraries}
    if images != expected or images != set(record['image_minos']):
        raise ValueError(f'{app}: Mach-O images do not match manifest and layout')
    for name, path in libraries.items():
        with path.open('rb') as stream:
            record['private_libraries'][name] = {
                'origin': record['private_libraries'][name],
                'sha256': hashlib.file_digest(stream, 'sha256').hexdigest(),
            }
    record['bundle_identifier'] = plist['CFBundleIdentifier']
    record['main_executable'] = plist['CFBundleExecutable']
    return record


def inventory(mountpoint, pins):
    if {path.name for path in mountpoint.glob('*.app')} != set(PRODUCTS):
        raise ValueError(f'{mountpoint}: DMG must contain both release apps')
    return {'pins': json.loads(pins.read_bytes()), 'bundles': {
        name: measure(mountpoint / name, executables) for name, executables in PRODUCTS.items()
    }}


if __name__ == '__main__':
    print(json.dumps(inventory(*(Path(arg) for arg in sys.argv[1:])), indent=2))
