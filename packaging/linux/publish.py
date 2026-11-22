#!/usr/bin/env python3
"""Publish OCI by digest, then sign and verify. Default prints commands ONLY.

--local-demo restricts destination to 127.0.0.1/localhost, disables transparency
upload and requires an explicit throwaway key. Release uses normal TLS + tlog.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('image', help='local image')
p.add_argument('destination', help='registry/repository:tag')
p.add_argument('--key', default='OWNER_KEY_REFERENCE')
p.add_argument('--public-key', default='OWNER_PUBLIC_KEY')
p.add_argument('--execute', action='store_true')
p.add_argument('--local-demo', action='store_true')
a = p.parse_args()
match = re.fullmatch(r'([a-z0-9]+(?:[.-][a-z0-9]+)*(?::[0-9]+)?)/([a-z0-9]+(?:[._-][a-z0-9]+)*(?:/[a-z0-9]+(?:[._-][a-z0-9]+)*)*):([A-Za-z0-9_][A-Za-z0-9_.-]*)', a.destination)
if not match or not ('.' in match[1] or ':' in match[1] or match[1] == 'localhost'):
    p.error('destination must have an explicit registry/repository:tag')
if a.local_demo and not re.match(r'^(127\.0\.0\.1|localhost):[0-9]+/md-w9-', a.destination):
    p.error('demo destination must be loopback/md-w9-*')
if a.execute and (a.key == 'OWNER_KEY_REFERENCE' or a.public_key == 'OWNER_PUBLIC_KEY'):
    p.error('execution requires explicit signing + verification keys')


def command(args):
    print(shlex.join(args), flush=True)
    if a.execute:
        subprocess.run(args, check=True)


# Temporary digest file is created only when execution was explicitly requested.
if not a.execute:
    digest_file = 'DIGEST_RECEIPT'
    reference = a.destination.rsplit(':', 1)[0] + '@sha256:RESOLVED_AFTER_PUSH'
    flags = ['--tls-verify=false'] if a.local_demo else []
    command(['podman', 'push', *flags, '--digestfile', digest_file, a.image, 'docker://' + a.destination])
else:
    with tempfile.TemporaryDirectory(prefix='md-w9-publish-') as tmp:
        digest_file = str(Path(tmp) / 'digest')
        flags = ['--tls-verify=false'] if a.local_demo else []
        command(['podman', 'push', *flags, '--digestfile', digest_file, a.image, 'docker://' + a.destination])
        digest = Path(digest_file).read_text().strip()
        if not re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
            raise ValueError('invalid pushed digest')
        reference = a.destination.rsplit(':', 1)[0] + '@' + digest
extra = ['--allow-insecure-registry', '--use-signing-config=false', '--new-bundle-format=false', '--tlog-upload=false'] if a.local_demo else []
command(['cosign', 'sign', '--yes', '--key', a.key, *extra, reference])
verify_flags = ['--allow-insecure-registry', '--new-bundle-format=false', '--insecure-ignore-tlog=true'] if a.local_demo else []
command(['cosign', 'verify', '--key', a.public_key, *verify_flags, reference])
print(('VERIFIED ' if a.execute else 'DRY RUN; no push, keys or signature access: ') + reference)
