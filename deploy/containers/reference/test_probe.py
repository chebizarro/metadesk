#!/usr/bin/env python3
"""Discovery/validation unit tests; live recovery evidence is in validation.md."""
import os
from pathlib import Path
import socket
import tempfile
import unittest
from unittest.mock import patch

import probe


@unittest.skipIf(os.getuid() == 0, "desktop identity must be non-root")
class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.root.chmod(0o700)
        for name in ("session", "bus", "bus_0"):
            sock = socket.socket(socket.AF_UNIX)
            sock.bind(str(self.root / name))
            self.addCleanup(sock.close)
        self.env = patch.dict(os.environ, {
            "MD_UID": str(os.getuid()), "XDG_RUNTIME_DIR": str(self.root),
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=" + str(self.root / "session"),
            "AT_SPI_BUS_ADDRESS": "unix:path=/stale/not-mounted",
        })
        self.env.start()
        self.addCleanup(self.env.stop)

    def discover(self, address, children="([(':1.2', objectpath '/org/a11y/atspi/accessible/root')],)"):
        with patch.object(probe, "call", side_effect=[repr((address,)), children]) as call:
            result = probe.desktop()
            self.assertEqual(call.call_args_list[-1].args[0], address)
            return result

    def test_rediscovers_replaced_socket_ignoring_stale_environment(self):
        for name in ("bus", "bus_0"):
            address = "unix:path=" + str(self.root / name) + ",guid=abcdef0123456789"
            self.assertEqual(self.discover(address), address)

    def test_rejects_nonfilesystem_addresses(self):
        for address in ("unix:abstract=bus", "tcp:host=localhost,port=1234", "unix:path=/x;unix:path=/y"):
            with self.subTest(address=address), self.assertRaises(ValueError):
                self.discover(address)

    def test_rejects_address_lists_and_encoding_even_with_literal_socket(self):
        # D-Bus parses separators/escapes, so stat must not validate a different
        # literal filename from the endpoint the host will actually connect to.
        for name in ("bus;unix:abstract=other", "bus%2fother"):
            with self.subTest(name=name), socket.socket(socket.AF_UNIX) as sock:
                path = self.root / name
                sock.bind(str(path))
                with self.assertRaises(ValueError):
                    self.discover("unix:path=" + str(path))

    def test_rejects_unmounted_or_regular_file(self):
        (self.root / "file").touch()
        for name in ("missing", "file"):
            with self.subTest(name=name), self.assertRaises((ValueError, OSError)):
                self.discover("unix:path=" + str(self.root / name))

    def test_rejects_wrong_socket_owner(self):
        with self.assertRaises(ValueError):
            probe.socket_address("unix:path=" + str(self.root / "bus"), os.getuid() + 1)

    def test_rejects_empty_registry(self):
        with self.assertRaisesRegex(ValueError, "no registered"):
            self.discover("unix:path=" + str(self.root / "bus"), "(@a(so) [],)")

    def test_rejects_malformed_discovery_reply(self):
        with patch.object(probe, "call", return_value="not a tuple"), self.assertRaisesRegex(ValueError, "invalid.*reply"):
            probe.desktop()


if __name__ == "__main__":
    unittest.main()
