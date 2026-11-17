#!/usr/bin/env python3
# validated on Docker 29.1.3 / rootless Podman 4.9.3 as external GTK fixture.
# Test-only GTK app; never part of the image. See validation.md.
# Use a task-owned D-Bus session/XDG_RUNTIME_DIR; do not repair the user's bus.
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk

GLib.set_application_name("md-ah9.4 fixture")
window = Gtk.Window(title="md-ah9.4 fixture")
window.add(Gtk.Button(label="md-ah9.4 expected accessible button"))
window.connect("destroy", Gtk.main_quit)
window.show_all()
Gtk.main()
