import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='md-keychain-home-', dir='/private/tmp') as home:
    env = dict(os.environ, HOME=home, CFFIXED_USER_HOME=home)
    sys.exit(subprocess.run([sys.argv[1], '--keychain'], env=env, timeout=25).returncode)
