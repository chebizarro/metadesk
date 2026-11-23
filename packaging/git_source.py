#!/usr/bin/env python3
"""Materialize a locked Git source without relying on generated archive bytes."""
import argparse
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile


def git(repo, *args, **kwargs):
    return subprocess.run(
        ['git', '--no-replace-objects', '-C', str(repo), *args], check=True, **kwargs)


def git_fields(pin):
    for field in ('commit', 'tree'):
        if not isinstance(pin.get(field), str) or not re.fullmatch('[0-9a-f]{40}', pin[field]):
            raise ValueError(f'Git source requires a full {field} object ID')
    return pin['commit'], pin['tree']


def verify_git_source(repo, ref, pin):
    commit, tree = git_fields(pin)
    # MSYS2 Git launched by native Windows Python does not preserve ^{...}.
    actual = git(repo, 'rev-parse', '--verify', '--end-of-options', ref,
                 capture_output=True, text=True).stdout.strip()
    kind = git(repo, 'cat-file', '-t', actual, capture_output=True, text=True).stdout.strip()
    if kind != 'commit':
        raise ValueError(f'Git object {actual} is not a commit (got {kind})')
    if actual != commit:
        raise ValueError(f'Git commit mismatch: expected {commit}, got {actual}')
    actual_tree = git(repo, 'show', '-s', '--no-notes', '--no-show-signature', '--format=%T',
                      actual, capture_output=True, text=True).stdout.strip()
    if actual_tree != tree:
        raise ValueError(f'Git tree mismatch: expected {tree}, got {actual_tree}')


def export_git_source(pin, dest, *, local_repo=None):
    commit, _ = git_fields(pin)
    with tempfile.TemporaryDirectory(prefix='git-source-', dir=dest.parent) as tmp:
        remote = pin.get('url')
        if local_repo is not None:
            remote = str(local_repo.resolve(strict=True))
        elif not isinstance(remote, str) or not remote.startswith('https://'):
            raise ValueError('Git source requires an HTTPS repository URL')
        repo = Path(tmp)
        git(repo, 'init', '--quiet', '--template=', '--object-format=sha1')
        git(repo, '-c', 'fetch.fsckObjects=true', 'fetch', '--quiet', '--depth=1',
            '--no-tags', '--no-recurse-submodules', '--', remote, commit)
        verify_git_source(repo, 'FETCH_HEAD', pin)
        # A local archive is only a transport to the clean source directory, not a
        # lock input. No working-tree files, Git metadata or submodule contents.
        with tempfile.TemporaryFile(dir=tmp) as archive:
            git(repo, '-c', 'tar.umask=0022', 'archive', '--format=tar', commit, stdout=archive)
            archive.seek(0)
            with tarfile.open(fileobj=archive) as source:
                dest.mkdir()
                source.extractall(dest, filter='data')
    print(f'{dest.name}: verified Git commit {commit}, tree {pin["tree"]}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dest', type=Path)
    parser.add_argument('url')
    parser.add_argument('commit')
    parser.add_argument('tree')
    args = parser.parse_args()
    export_git_source(vars(args), args.dest)
