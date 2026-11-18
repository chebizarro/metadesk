"""Offline tests for the qualification client's cryptographic wire format."""
import base64
import ctypes as C
import ctypes.util
import hashlib
import importlib.util
import json
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import tempfile
import subprocess
import sys
import threading
import unittest
import urllib.error
import urllib.request

spec = importlib.util.spec_from_file_location("mesh_peer", Path(__file__).with_name("mesh-peer.py"))
peer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(peer)


@unittest.skipUnless(ctypes.util.find_library("secp256k1"), "distro libsecp256k1 required")
class MeshPeerTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.key = Path(self.work.name) / "key"
        self.key.write_text("00" * 31 + "01\n")
        self.key.chmod(0o600)

    def test_known_public_key_and_npub(self):
        lib, ctx, _, raw = peer.identity(self.key)
        self.addCleanup(lib.secp256k1_context_destroy, ctx)
        self.assertEqual(raw.hex(), "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798")
        self.assertEqual(peer.npub(raw), "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d")

    def test_nip98_signature_payload_url_and_unique_id(self):
        url, body = "http://[fd00::1]:7711/mcp", b'{"method":"initialize"}'
        events = [json.loads(base64.b64decode(peer.authorization(self.key, url, body).split()[1]))
                  for _ in range(2)]
        self.assertNotEqual(events[0]["id"], events[1]["id"])
        lib, ctx, _, _ = peer.identity(self.key)
        self.addCleanup(lib.secp256k1_context_destroy, ctx)
        lib.secp256k1_xonly_pubkey_parse.argtypes = [C.c_void_p] * 3
        lib.secp256k1_schnorrsig_verify.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
        for event in events:
            self.assertEqual(event["kind"], 27235)
            tags = dict(event["tags"])
            self.assertEqual(tags["u"], url)
            self.assertEqual(tags["method"], "POST")
            self.assertEqual(tags["payload"], hashlib.sha256(body).hexdigest())
            digest = hashlib.sha256(peer.compact([0, event["pubkey"], event["created_at"],
                                                   event["kind"], event["tags"], event["content"]])).digest()
            self.assertEqual(digest.hex(), event["id"])
            public = C.create_string_buffer(64)
            self.assertEqual(lib.secp256k1_xonly_pubkey_parse(ctx, public, bytes.fromhex(event["pubkey"])), 1)
            self.assertEqual(lib.secp256k1_schnorrsig_verify(ctx, bytes.fromhex(event["sig"]), digest, 32, public), 1)

    def test_invalid_key(self):
        self.key.write_text("00" * 32)
        with self.assertRaises(ValueError):
            peer.identity(self.key)


@contextmanager
def serve(handler):
    server = HTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        yield "http://127.0.0.1:" + str(server.server_port)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class QuietHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass


class TransportTests(unittest.TestCase):
    def test_redirect_is_not_followed_with_credential(self):
        stolen = []

        class Handler(QuietHandler):
            def do_POST(self):
                self.send_response(302)
                self.send_header("Location", "/stolen")
                self.end_headers()

            def do_GET(self):
                stolen.append(self.headers.get("Authorization"))
                self.send_response(200)
                self.end_headers()

        with serve(Handler) as url:
            req = urllib.request.Request(url, b"{}", {"Authorization": "Bearer test-only"})
            with self.assertRaises(urllib.error.HTTPError) as caught:
                peer.http_opener().open(req, timeout=5)
            self.assertEqual(caught.exception.code, 302)
            caught.exception.close()
        self.assertEqual(stolen, [])

    def test_optimized_cli_rejects_missing_desktop_evidence(self):
        class Handler(QuietHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                if "Authorization" not in self.headers:
                    code, result = 401, None
                elif self.headers.get("Origin") or self.headers.get("Host") == "foreign.invalid":
                    code, result = 403, None
                else:
                    method = json.loads(body)["method"]
                    code = 202 if method == "notifications/initialized" else 200
                    result = {"result": {"tools": []}} if method == "tools/list" else {"result": {}}
                data = json.dumps(result).encode() if result is not None else b""
                self.send_response(code)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        with tempfile.TemporaryDirectory() as work, serve(Handler) as url:
            token = Path(work) / "token"
            token.write_text("test-only")
            result = subprocess.run([sys.executable, "-O", str(Path(__file__).with_name("mesh-peer.py")),
                                     "--url", url + "/mcp", "--token", str(token)],
                                    capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("live Calculator window absent", result.stderr)
        self.assertNotIn('"initialize_tools_tree": "PASS"', result.stdout)


if __name__ == "__main__":
    unittest.main()
