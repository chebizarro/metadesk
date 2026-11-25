#!/usr/bin/env python3
"""Same byte fixtures for each frontend; optionally exchange real writer output."""
import fcntl
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

PREFIX = b'[relays]\nbootstrap='
MAX_URL = b'wss://r/' + b'a' * (2048 - len(b'wss://r/'))
VECTORS = [
    (None, []),  # only absence is equivalent to a saved empty list
    (b'', None), (b'\n# comment\n', None), (b'[relays]\n', None),
    (PREFIX, []), (PREFIX + b'\n', []),
    (PREFIX + b'WSS://RELAY:443;ws://r:00080;ws://r:42/a\\;b%3b;\n',
     ['wss://relay/', 'ws://r/', 'ws://r:42/a;b%3B']),
    (b' # comment\r\n  [relays] \r\n bootstrap  = \tws://r/a//b/../%2e/%41%2f\r\n',
     ['ws://r/a//A%2F']),
    (PREFIX + 'WSS://BÜCHER.Example/café;ws://[FE80::1%25EN0]:80;'.encode(),
     ['wss://xn--bcher-kva.example/caf%C3%A9', 'ws://[fe80::1%25en0]/']),
    (PREFIX + b'ws://Relay;ws://relay;', ['ws://relay/', 'ws://relay/']),
    (PREFIX + b'ws://[FE80::1%25z!%3a%40]/', ['ws://[fe80::1%25z%21:%40]/']),
    (PREFIX + 'wss://\u200b.a'.encode(), ['wss://.a/']),
    (PREFIX + b'bad\n[unrelated]\nk[fr]=value\n[relays]\nbootstrap=ws://r', ['ws://r/']),
    (PREFIX + b'ws://r\n[other]\nmalformed', None),
    (PREFIX + b'ws://r\nEncoding=ISO-8859-1', None),
    (PREFIX + b'ws://r\n[other]\n[relays]\nEncoding=ISO-8859-1', None),
    (PREFIX + b'ws://r\n[other]\f\n', None),
    (PREFIX + b'ws://r\n[other]\nkey[~en]=bad-locale', None),
    (PREFIX + MAX_URL, [MAX_URL.decode()]), (PREFIX + MAX_URL + b'a', None),
] + [(PREFIX + bad, None) for bad in (
    b';', b'ws://r;;', b'ws://r; ', b'ws://r\\s', b'ws://r\\x', b'ws://r\\',
    b'ws://r/\\\\', b'ws://r/\xff', b'https://r', b'ws:///r', b'ws://u@r',
    b'ws://r?', b'ws://r#', b'ws://r:0', b'ws://r:+1', b'ws://r:65536',
    b'ws://[not-ipv6]', b'ws://r/%2', b'ws://r/a\x7fb',
)]


def run(binary, *args, expected=0, env=None):
    result = subprocess.run([binary, *args], capture_output=True, text=True, env=env, timeout=5)
    assert result.returncode == expected, (binary, args, result)
    return result.stdout.splitlines()


def load(binary, directory, expected):
    assert run(binary, 'load', directory, expected=int(expected is None)) == (expected or []), (binary, expected)


def main(binaries):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        directory = root / 'config' / 'metadesk'
        directory.mkdir(parents=True, mode=0o700)
        config = directory / 'manager-relays.ini'
        for reader in binaries:
            env = dict(os.environ, HOME=str(root), CFFIXED_USER_HOME=str(root), XDG_CONFIG_HOME='')
            assert run(reader, 'directory', env=env) == [str(root / '.config/metadesk')]
            env['XDG_CONFIG_HOME'] = str(directory.parent)
            assert run(reader, 'directory', env=env) == [str(directory)]
            for content, expected in VECTORS:
                config.unlink(missing_ok=True)
                if content is not None:
                    config.write_bytes(content)
                try:
                    load(reader, directory, expected)
                except AssertionError as error:
                    raise AssertionError(f'input={content!r}') from error
            config.unlink()
            config.mkdir()  # a non-regular/unreadable file, even when running as root
            load(reader, directory, None)
            config.rmdir()
            if os.geteuid() != 0:
                config.write_bytes(PREFIX + b'ws://r;\n')
                config.chmod(0)
                load(reader, directory, None)
                config.unlink()
        for writer in binaries:
            config.write_bytes(PREFIX + b'ws://old;\n')
            config.chmod(0o600)  # existing GTK writes preserve the old file mode
            with config.open('rb') as old:
                run(writer, 'save', directory, 'WSS://RELAY:443', 'wss://relay/', 'ws://r/a;b')
                assert old.read() == PREFIX + b'ws://old;\n'  # replacement, not truncation
            assert stat.S_IMODE(config.stat().st_mode) == 0o600
            for reader in binaries:
                load(reader, directory, ['wss://relay/', 'ws://r/a;b'])
            saved = config.read_bytes()
            run(writer, 'save', directory, 'ws://valid', 'ws://bad?query', expected=1)
            with (directory / '.management.lock').open('rb') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                run(writer, 'save', directory, 'ws://other', expected=1)
            assert config.read_bytes() == saved
            run(writer, 'save', directory)
            for reader in binaries:
                load(reader, directory, [])


if __name__ == '__main__':
    main(sys.argv[1:])
