#!/usr/bin/env python3
"""Run the offscreen snapshot renderer with a private home (md-b9zo).

usage: snapshot_pages.py RENDERER OUTPUT_DIR

The renderer never orders a window on screen and uses fixture workers only.
HOME, CFFIXED_USER_HOME and the XDG directories point at a temporary
directory, so window-frame defaults, relay preferences and accounts written by
the fixtures never touch the user's own files."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

renderer, output = sys.argv[1], Path(sys.argv[2])
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='md-snap-', dir='/private/tmp') as root:
    home = Path(root).resolve()
    env = dict(os.environ, HOME=str(home), CFFIXED_USER_HOME=str(home), XDG_CONFIG_HOME=str(home / 'config'),
               XDG_STATE_HOME=str(home / 'state'), XDG_CACHE_HOME=str(home / 'cache'))
    accounts = home / 'accounts'
    accounts.mkdir(mode=0o700)
    sys.exit(subprocess.run([renderer, str(accounts), str(output)], env=env, timeout=240).returncode)
