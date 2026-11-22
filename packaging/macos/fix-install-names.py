#!/usr/bin/env python3
"""Meson Darwin install hook: keep product references relocatable in DESTDIR."""
import os
from pathlib import Path
import subprocess
import sys


def main():
    if os.environ.get('MESON_INSTALL_DRY_RUN') == '1':
        return
    prefix = Path(os.environ['MESON_INSTALL_DESTDIR_PREFIX'])
    def directory(value):
        p = Path(value)
        return Path(os.environ.get('DESTDIR', '/')) / str(p).lstrip('/') if p.is_absolute() else prefix / p
    lib = directory(sys.argv[1]) / 'libmetadesk.0.dylib'
    old = subprocess.check_output(['otool', '-D', str(lib)], text=True).splitlines()[1].strip()
    new = '@rpath/' + lib.name
    original = str(Path(os.environ['MESON_INSTALL_PREFIX']) / sys.argv[1] / lib.name)
    subprocess.run(['install_name_tool', '-id', new, str(lib)], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', str(lib.resolve())], check=True)
    for name in sys.argv[3:]:
        exe = directory(sys.argv[2]) / name
        for ref in sorted({old, original} - {new}):
            subprocess.run(['install_name_tool', '-change', ref, new, str(exe)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', str(exe)], check=True)


if __name__ == '__main__':
    main()
