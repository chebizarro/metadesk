"""No internet: observe explicit-only dispatch and license-before-network ordering."""
import os
import socket
import subprocess
import sys
import tempfile

host = sys.argv[1]
with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    env = dict(os.environ, HTTPS_PROXY=f'http://127.0.0.1:{listener.getsockname()[1]}',
               https_proxy=f'http://127.0.0.1:{listener.getsockname()[1]}', NO_PROXY='', no_proxy='')
    for args in [('--help',), ('--fetch-openh264', '--encoder', 'openh264')]:
        result = subprocess.run([host, *args], env=env, capture_output=True, timeout=10)
        assert result.returncode == (0 if args == ('--help',) else 1), result.stderr
    # Completed processes cannot have a future connect: check the queue once.
    listener.setblocking(False)
    try:
        conn, _ = listener.accept()
    except BlockingIOError:
        pass
    else:
        conn.close()
        raise AssertionError('network without explicit standalone action')
    listener.settimeout(10)
    # At the first request, observe bytes already flushed by the child, not
    # buffered pipe bytes read after the request has failed.
    scratch = '/private/tmp' if sys.platform == 'darwin' else os.environ.get('TMPDIR')
    with tempfile.TemporaryFile(dir=scratch) as notice:
        process = subprocess.Popen([host, '--fetch-openh264'], env=env,
                                   stdout=notice, stderr=subprocess.PIPE)
        with listener.accept()[0] as conn:
            conn.settimeout(10)
            request = conn.recv(8192)
            notice.seek(0)
            before_request = notice.read()
            conn.sendall(b'HTTP/1.1 502 Offline fixture\r\nContent-Length: 0\r\n\r\n')
        _, stderr = process.communicate(timeout=15)
        assert process.returncode == 1, stderr
        assert b'Cisco' in before_request and b'BSD' in before_request
        assert b'Verifying pinned Cisco SHA-256:' in before_request
        assert b'ciscobinary.openh264.org:443' in request, request
print('PASS CLI: no implicit network; full license before explicit TLS request; fail closed on proxy rejection')
