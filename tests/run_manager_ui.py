#!/usr/bin/env python3
"""Run GTK smoke checks without borrowing the live desktop's bus or runtime."""
import os
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="md-w11-display-") as temporary:
    root = pathlib.Path(temporary)
    environment = os.environ.copy()
    for name in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE",
                 "AT_SPI_BUS_ADDRESS", "WAYLAND_DISPLAY", "SESSION_MANAGER"):
        environment.pop(name, None)
    for name, subdirectory in (("XDG_RUNTIME_DIR", "runtime"), ("XDG_CACHE_HOME", "cache")):
        directory = root / subdirectory
        directory.mkdir(mode=0o700)
        environment[name] = str(directory)
    environment.update(GTK_A11Y="none", GDK_BACKEND="x11", GDK_DEBUG="no-portals",
                       GSK_RENDERER="cairo", GIO_USE_VFS="local", GSETTINGS_BACKEND="memory",
                       MESA_SHADER_CACHE_DISABLE="true")
    # No service directories: toolkit probes cannot activate portals, AT-SPI,
    # keyrings, or other desktop helpers in this private test session.
    configuration = root / "bus.conf"
    configuration.write_text("""<busconfig>
<type>session</type><listen>unix:tmpdir=""" + environment["XDG_RUNTIME_DIR"] + """</listen>
<auth>EXTERNAL</auth><policy context="default">
<allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/>
</policy></busconfig>""", encoding="utf-8")
    result = subprocess.run([sys.argv[1], "--config-file=" + str(configuration), "--",
                             sys.argv[2], "-a", *sys.argv[3:]],
                            env=environment, check=False)
    returncode = result.returncode
sys.exit(returncode)
