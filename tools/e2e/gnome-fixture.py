#!/usr/bin/env python3
"""Owned GTK target on the existing desktop; no private-bus reset or input device."""
import os
import gi

gi.require_version('Gtk', '3.0')
from gi.repository import GLib, Gtk

window = Gtk.Window(title='md-l7 Owned GNOME Target')
window.set_default_size(640, 400)
window.set_border_width(24)
window.connect('destroy', Gtk.main_quit)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
window.add(box)
frames = Gtk.Label(label='md-l7 frame 0')
frames.set_xalign(0)
box.pack_start(frames, False, False, 0)
clicks = Gtk.Label(label='md-l7 clicks 0')
clicks.set_xalign(0)
box.pack_start(clicks, False, False, 0)
state = {'frames': 0, 'clicks': 0}


def clicked(_button):
    state['clicks'] += 1
    clicks.set_text("md-l7 clicks %d" % state['clicks'])
    print('owned button clicked count=%d' % state['clicks'], flush=True)


button = Gtk.Button.new_with_mnemonic('_Increment Counter')
button.get_accessible().set_name('md-l7 Increment Counter')
button.connect('clicked', clicked)
box.pack_start(button, False, False, 0)
entry = Gtk.Entry()
entry.set_placeholder_text('Test text (owned window only)')
entry.get_accessible().set_name('md-l7 owned text input')
box.pack_start(entry, False, False, 0)
scroll = Gtk.ScrolledWindow()
text = Gtk.TextView()
text.get_accessible().set_name('md-l7 owned scroll target')
text.get_buffer().set_text('\n'.join('md-l7 scroll line %d' % i for i in range(100)))
scroll.add(text)
box.pack_start(scroll, True, True, 0)


def tick():
    state['frames'] += 1
    frames.set_text('md-l7 frame %d' % state['frames'])
    return True


# This is display damage generation, not a protocol/readiness polling loop.
GLib.timeout_add(100, tick)
window.show_all()
print('fixture ready pid=%d' % os.getpid(), flush=True)
Gtk.main()
