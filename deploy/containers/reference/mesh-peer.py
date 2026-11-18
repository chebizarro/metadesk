#!/usr/bin/env python3
"""Throwaway mesh qualification client; stdlib + distro libsecp256k1.

Run inside the remote peer's namespace, not the MCP host's namespace.
Keys are files, never environment variables; --generate-key refuses overwrite.
This is a test client, not a production signer or MCP transport implementation.
"""
import argparse
import base64
import ctypes as C
import ctypes.util
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.request


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def http_opener():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())


def identity(path):
    secret = bytes.fromhex(Path(path).read_text().strip())
    if len(secret) != 32:
        raise ValueError("expected a 32-byte throwaway key")
    lib = C.CDLL(ctypes.util.find_library("secp256k1"))
    lib.secp256k1_context_create.argtypes = [C.c_uint]
    lib.secp256k1_context_create.restype = C.c_void_p
    lib.secp256k1_context_destroy.argtypes = [C.c_void_p]
    for name, count in (("keypair_create", 3), ("keypair_xonly_pub", 4),
                        ("xonly_pubkey_serialize", 3), ("schnorrsig_sign32", 5)):
        fn = getattr(lib, "secp256k1_" + name)
        fn.argtypes = [C.c_void_p] * count
        fn.restype = C.c_int
    ctx = lib.secp256k1_context_create(1)
    pair, public, raw = C.create_string_buffer(96), C.create_string_buffer(64), C.create_string_buffer(32)
    try:
        if not lib.secp256k1_keypair_create(ctx, pair, secret):
            raise ValueError("invalid secret key")
        require(lib.secp256k1_keypair_xonly_pub(ctx, public, None, pair) == 1, "xonly public key failed")
        require(lib.secp256k1_xonly_pubkey_serialize(ctx, raw, public) == 1, "public key serialization failed")
        return lib, ctx, pair, raw.raw
    except BaseException:
        lib.secp256k1_context_destroy(ctx)
        raise


def npub(raw):
    # NIP-19 Bech32 (original checksum, not Bech32m).
    values, bits, acc = [], 0, 0
    for byte in raw:
        acc = (acc << 8) | byte
        bits += 8
        while bits >= 5:
            bits -= 5
            values.append((acc >> bits) & 31)
    if bits:
        values.append((acc << (5 - bits)) & 31)
    hrp = "npub"
    checksum = 1
    expanded = [ord(c) >> 5 for c in hrp] + [0] + [ord(c) & 31 for c in hrp]
    for value in expanded + values + [0] * 6:
        top = checksum >> 25
        checksum = ((checksum & 0x1ffffff) << 5) ^ value
        for bit, generator in enumerate((0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3)):
            if (top >> bit) & 1:
                checksum ^= generator
    checksum ^= 1
    values += [(checksum >> (5 * (5 - i))) & 31 for i in range(6)]
    alphabet = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
    return hrp + "1" + "".join(alphabet[v] for v in values)


def compact(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()


def authorization(key, url, body, method="POST"):
    lib, ctx, pair, public = identity(key)
    try:
        event = {"pubkey": public.hex(), "created_at": int(time.time()), "kind": 27235,
                 "tags": [["u", url], ["method", method],
                          ["nonce", os.urandom(8).hex()]], "content": ""}
        if method == "POST":
            event["tags"].append(["payload", hashlib.sha256(body).hexdigest()])
        digest = hashlib.sha256(compact([0, event["pubkey"], event["created_at"],
                                          event["kind"], event["tags"], event["content"]])).digest()
        sig = C.create_string_buffer(64)
        require(lib.secp256k1_schnorrsig_sign32(ctx, sig, digest, pair, os.urandom(32)) == 1,
                "Schnorr signing failed")
        event.update(id=digest.hex(), sig=sig.raw.hex())
        return "Nostr " + base64.b64encode(compact(event)).decode()
    finally:
        lib.secp256k1_context_destroy(ctx)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate-key", type=Path)
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--url")
    auth = parser.add_mutually_exclusive_group()
    auth.add_argument("--key", type=Path)
    auth.add_argument("--token", type=Path)
    parser.add_argument("--expect", default="gnome-calculator")
    args = parser.parse_args()
    if args.generate_key:
        fd = os.open(args.generate_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as out:
            out.write(os.urandom(32).hex() + "\n")
        return
    if args.identity:
        lib, ctx, _, raw = identity(args.identity)
        try:
            print(json.dumps({"npub": npub(raw), "pubkey": raw.hex(),
                              "address": str(ipaddress.IPv6Address(b"\xfd" + hashlib.sha256(raw).digest()[:15]))}))
        finally:
            lib.secp256k1_context_destroy(ctx)
        return
    if not args.url or not (args.key or args.token):
        parser.error("--url and --key or --token required")
    opener = http_opener()
    ident = 0

    def send(method, params=None, extra=None, credential=True, expect=200):
        nonlocal ident
        ident += 1
        message = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not method.startswith("notifications/"):
            message["id"] = ident
        body = compact(message)
        headers = {"Content-Type": "application/json"}
        if credential:
            headers["Authorization"] = (authorization(args.key, args.url, body) if args.key
                                         else "Bearer " + args.token.read_text().strip())
        headers.update(extra or {})
        req = urllib.request.Request(args.url, body, headers)
        try:
            response = opener.open(req, timeout=40)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            require(response.status == expect, (method, response.status, expect))
            data = response.read()
        return json.loads(data) if expect == 200 else None

    send("initialize", credential=False, expect=401)
    send("initialize", extra={"Host": "foreign.invalid"}, expect=403)
    send("initialize", extra={"Origin": "http://foreign.invalid"}, expect=403)
    result = send("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                  "clientInfo": {"name": "ah9-5-mesh-peer", "version": "1"}})
    require("result" in result, result)
    send("notifications/initialized", expect=202)
    listing = send("tools/list")
    require("result" in listing, listing)
    tools = listing["result"]["tools"]
    forbidden = {"metadesk_click", "metadesk_dbl_click", "metadesk_right_click", "metadesk_type",
                 "metadesk_key_combo", "metadesk_scroll", "metadesk_set_value", "metadesk_focus",
                 "metadesk_screenshot"}
    require(forbidden.isdisjoint(tool["name"] for tool in tools), tools)
    windows = send("tools/call", {"name": "metadesk_windows", "arguments": {}})
    require("result" in windows and not windows["result"].get("isError"), windows)
    require("Calculator" in json.dumps(windows["result"]), "live Calculator window absent")
    tree = send("resources/read", {"uri": "metadesk://ui-tree"})
    require("result" in tree and args.expect in json.dumps(tree["result"]), "required live application absent")

    def sse(extra=None, credential=True, expect=200):
        headers = {}
        if credential:
            headers["Authorization"] = (authorization(args.key, args.url, b"", "GET") if args.key
                                         else "Bearer " + args.token.read_text().strip())
        headers.update(extra or {})
        req = urllib.request.Request(args.url, headers=headers, method="GET")
        try:
            response = opener.open(req, timeout=40)
        except urllib.error.HTTPError as error:
            response = error
        # Explicit test cancellation after response headers, not a timer close.
        with response:
            require(response.status == expect, ("SSE GET", response.status, expect))
            if expect == 200:
                require(response.headers.get("Content-Type", "").startswith("text/event-stream"),
                        "SSE content type absent")

    sse(credential=False, expect=401)
    sse(extra={"Host": "foreign.invalid"}, expect=403)
    sse(extra={"Origin": "http://foreign.invalid"}, expect=403)
    sse()
    print(json.dumps({"auth": "NIP-98" if args.key else "bearer", "url": args.url,
                      "unauthenticated": 401, "foreign_host": 403, "origin": 403,
                      "initialize_tools_tree": "PASS", "required_application": args.expect,
                      "readonly_window_tool": "PASS", "input_tools_absent": True,
                      "sse_authenticated": 200, "sse_unauthenticated": 401,
                      "sse_foreign_host": 403, "sse_origin": 403}))


if __name__ == "__main__":
    main()
