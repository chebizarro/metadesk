import os
import subprocess
import sys
import tempfile

# An owner's workstation may prohibit even disposable Keychain items.
# The hosted release suite leaves this unset and runs the real fixture.
if os.environ.get('MD_TEST_NO_KEYCHAIN') == '1':
    print('SKIP: local verification prohibits creating Keychain items')
    sys.exit(77)

with tempfile.TemporaryDirectory(prefix='md-keychain-home-', dir='/private/tmp') as home:
    env = dict(os.environ, HOME=home, CFFIXED_USER_HOME=home)
    sys.exit(subprocess.run([sys.argv[1], '--keychain'], env=env, timeout=25).returncode)
