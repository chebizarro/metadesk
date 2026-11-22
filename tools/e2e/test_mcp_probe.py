#!/usr/bin/env python3
"""Private loopback negative: the MCP probe must not follow bearer redirects."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest


class RedirectTests(unittest.TestCase):
    def test_no_credential_redirect(self):
        received = []

        class Destination(BaseHTTPRequestHandler):
            def do_GET(self):
                received.append(self.headers.get('Authorization'))
                self.send_response(200)
                self.end_headers()
            do_POST = do_GET
            def log_message(self, *_):
                pass

        destination = ThreadingHTTPServer(('127.0.0.1', 0), Destination)
        authenticated = []

        class Source(BaseHTTPRequestHandler):
            def do_POST(self):
                if self.headers.get('Authorization'):
                    authenticated.append(True)
                    self.send_response(302)
                    self.send_header('Location', 'http://127.0.0.1:%d/leak' % destination.server_port)
                else:
                    self.send_response(401)
                self.end_headers()
            def log_message(self, *_):
                pass

        source = ThreadingHTTPServer(('127.0.0.1', 0), Source)
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in (source, destination)]
        for t in threads:
            t.start()
        try:
            with tempfile.TemporaryDirectory(prefix='md-l7-mcp-') as root:
                token = Path(root) / 'token'
                token.write_text('throwaway-loopback-token')
                result = subprocess.run([sys.executable, '-O',
                    str(Path(__file__).with_name('mcp-target.py')), '--url',
                    'http://127.0.0.1:%d/mcp' % source.server_port, '--token', str(token),
                    '--window', 'md-l7 owned test', '--button', 'md-l7 button'],
                    capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('302', result.stderr)
                self.assertEqual(authenticated, [True])
                self.assertEqual(received, [])
        finally:
            for s in (source, destination):
                s.shutdown()
                s.server_close()
            for t in threads:
                t.join()


if __name__ == '__main__':
    unittest.main()
