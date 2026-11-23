#!/usr/bin/env python3
"""Fetch object- or hash-locked sources into a NEW directory (no package installs)."""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import shutil
import hashlib
import json
from pathlib import Path
import sys
import tarfile
import urllib.request


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from git_source import export_git_source


def fetch(output, cache=None):
    output.mkdir(parents=True, exist_ok=False)
    lockfile = Path(__file__).with_name('sources.lock.json')
    lock = json.loads(lockfile.read_text())
    for name, spec in lock.items():
        if spec['type'] == 'git':
            export_git_source(spec, output / name)
            continue
        if spec['type'] == 'git-blobs':
            def get_file(item):
                relative, expected = item
                with urllib.request.urlopen(spec['base_url'] + relative + spec.get('url_suffix', '')) as src:
                    data = src.read()
                if spec.get('base64'):
                    data = base64.b64decode(data)
                actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
                if actual != expected:
                    raise ValueError('Git blob hash mismatch: ' + relative)
                dest = output / name / relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
            with ThreadPoolExecutor(max_workers=8) as workers:
                list(workers.map(get_file, spec['git_blob_sha1'].items()))
            print(name + ': pinned Git blob hashes verified')
            continue
        if spec['type'] != 'archive':
            raise ValueError(f"{name}: unknown source type {spec['type']}")
        archive = output / (name + '.tar.gz')
        cached = cache / archive.name if cache else None
        if cached and cached.is_file():
            shutil.copyfile(cached, archive)
        else:
            with urllib.request.urlopen(spec['url']) as src, archive.open('wb') as dest:
                while data := src.read(1024 * 1024):
                    dest.write(data)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != spec['sha256']:
            raise ValueError('source hash mismatch: ' + name)
        dest = output / name
        dest.mkdir()
        with tarfile.open(archive) as tar:
            # Upstream archives have one top-level directory. Use Python >=3.12's
            # data filter to reject absolute paths, escaping links and devices.
            tar.extractall(dest, filter='data')
        children = list(dest.iterdir())
        if len(children) != 1 or not children[0].is_dir():
            raise ValueError('unexpected archive layout: ' + name)
        top = children[0]
        for entry in top.iterdir():
            entry.rename(dest / entry.name)
        top.rmdir()
        print(name + ': SHA256 verified')
    (output / 'sources.lock.json').write_bytes(lockfile.read_bytes())


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--cache', type=Path, help='Optional source archive cache; hashes still verified')
    a = p.parse_args()
    fetch(a.output, a.cache)
