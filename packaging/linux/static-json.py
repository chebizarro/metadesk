#!/usr/bin/env python3
"""Make a private nostrc build overlay with static JSON from CMake's own objects.

Upstream cfcb86296068 forces nostr_json SHARED even with BUILD_SHARED_LIBS=OFF.
Use its trusted link manifest, not a wildcard/stale-object scan. No upstream edits.
"""
from pathlib import Path
import argparse
import shlex
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
source, out = a.build.resolve(), a.output.resolve()
if out.exists() or source in out.parents:
    p.error('output must be new and outside build')
link = source / 'libjson/CMakeFiles/nostr_json.dir/link.txt'
objects = [source / 'libjson' / word for word in shlex.split(link.read_text()) if word.endswith('.o')]
if not objects or not all(o.is_file() for o in objects):
    p.error('requires built Unix Makefiles nostr_json target')
shutil.copytree(source, out, symlinks=True)
lib = out / 'libjson'
for f in lib.glob('libnostr-json.so*'):
    f.unlink()
(lib / 'libnostr-json.a').unlink(missing_ok=True)
subprocess.run(['ar', 'crs', str(lib / 'libnostr-json.a'), *map(str, objects)], check=True)
print(out)
