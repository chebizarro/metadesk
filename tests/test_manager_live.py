#!/usr/bin/env python3
"""Real host/client/relay run driven through production manager GTK controls."""
import os
from pathlib import Path
import sys
sys.dont_write_bytecode = True
from test_host_approval import main
if __name__ == '__main__':
    host, client, crypto, driver = map(os.path.abspath, sys.argv[1:])
    sys.argv = [sys.argv[0], host, client, crypto]
    wrapper = str(Path(__file__).with_name('run_manager_ui.py'))
    main([sys.executable, wrapper, 'dbus-run-session', 'xvfb-run', driver])
