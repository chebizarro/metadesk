#!/bin/sh
# validated on rootless Podman 4.9.3 / Ubuntu 24.04 aarch64; see video-validation.md.
# Only private container directories: never mount a host runtime or session bus.
set -eu
umask 077
if [ "${1:-}" != --private-session ]; then
    unset DISPLAY WAYLAND_DISPLAY SWAYSOCK DBUS_SESSION_BUS_ADDRESS AT_SPI_BUS_ADDRESS
    unset PIPEWIRE_REMOTE PIPEWIRE_RUNTIME_DIR PULSE_SERVER
    mkdir -p "$XDG_RUNTIME_DIR" "$XDG_STATE_HOME/metadesk" "$XDG_CONFIG_HOME"
    chmod 700 "$XDG_RUNTIME_DIR" "$XDG_STATE_HOME" "$XDG_STATE_HOME/metadesk"
    exec dbus-run-session -- "$0" --private-session "$@"
fi
shift
[ "$#" -gt 0 ] || { echo 'capture consumer command required' >&2; exit 2; }
pids=''
cleanup() {
    trap - EXIT TERM INT
    for pid in $pids; do kill "$pid" 2>/dev/null || :; done
    for pid in $pids; do wait "$pid" 2>/dev/null || :; done
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
cat > "$XDG_CONFIG_HOME/sway.conf" <<'EOF'
xwayland disable
output HEADLESS-1 mode 1920x1080@60Hz scale 1
output HEADLESS-1 bg #204060 solid_color
seat seat0 fallback true
EOF
mkdir -p "$XDG_CONFIG_HOME/xdg-desktop-portal" "$XDG_CONFIG_HOME/xdg-desktop-portal-wlr"
printf '[preferred]\ndefault=wlr\norg.freedesktop.impl.portal.ScreenCast=wlr\n' \
    > "$XDG_CONFIG_HOME/xdg-desktop-portal/portals.conf"
# Default deny. Opt-in grants all session clients capture of ONLY this output.
case "${MD_VIDEO_CONSENT:-deny}" in
    allow-isolated-output) chooser='chooser_type=none' ;;
    deny) chooser='chooser_type=simple
chooser_cmd=/bin/false' ;;
    *) echo 'MD_VIDEO_CONSENT must be deny or allow-isolated-output' >&2; exit 2 ;;
esac
printf '[screencast]\n%s\noutput_name=HEADLESS-1\nmax_fps=15\n' "$chooser" \
    > "$XDG_CONFIG_HOME/xdg-desktop-portal-wlr/config"
sway -d -c "$XDG_CONFIG_HOME/sway.conf" > "$XDG_RUNTIME_DIR/sway.log" 2>&1 &
pids="$! $pids"
# Startup readiness checks are bounded; capture itself is portal/PipeWire driven.
python3 - <<'PY'
import glob, os, time
end = time.monotonic() + 15
while time.monotonic() < end:
    wl = glob.glob(os.environ['XDG_RUNTIME_DIR'] + '/wayland-*')
    ipc = glob.glob(os.environ['XDG_RUNTIME_DIR'] + '/sway-ipc.*.sock')
    wl = [p for p in wl if not p.endswith('.lock')]
    if wl and ipc:
        with open(os.environ['XDG_RUNTIME_DIR'] + '/session.env', 'w') as f:
            f.write('export WAYLAND_DISPLAY=' + os.path.basename(wl[0]) + '\n')
            f.write('export SWAYSOCK=' + ipc[0] + '\n')
        break
    time.sleep(.05)
else:
    raise SystemExit('private Sway startup failed; see sway.log')
PY
# Generated above in the private mode-0700 runtime directory.
# shellcheck source=/dev/null
. "$XDG_RUNTIME_DIR/session.env"
dbus-update-activation-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE XDG_RUNTIME_DIR
pipewire > "$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
pids="$! $pids"
python3 - <<'PY'
import subprocess, time
end = time.monotonic() + 15
while time.monotonic() < end:
    try:
        ready = subprocess.run(['pw-cli', 'info', '0'], timeout=1,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if ready.returncode == 0:
            break
    except subprocess.TimeoutExpired:
        pass
    time.sleep(.05)
else:
    raise SystemExit('private PipeWire startup failed; see pipewire.log')
PY
wireplumber > "$XDG_RUNTIME_DIR/wireplumber.log" 2>&1 &
pids="$! $pids"
/usr/libexec/xdg-desktop-portal-wlr -l DEBUG > "$XDG_RUNTIME_DIR/xdpw.log" 2>&1 &
pids="$! $pids"
gdbus wait --session --timeout 20 org.freedesktop.impl.portal.desktop.wlr
/usr/libexec/xdg-desktop-portal > "$XDG_RUNTIME_DIR/portal.log" 2>&1 &
pids="$! $pids"
gdbus wait --session --timeout 20 org.freedesktop.portal.Desktop
swaymsg -t get_outputs > "$XDG_RUNTIME_DIR/outputs.json"
wayland-info > "$XDG_RUNTIME_DIR/wayland-info.txt"
gdbus call --session --dest org.freedesktop.portal.Desktop \
    --object-path /org/freedesktop/portal/desktop \
    --method org.freedesktop.DBus.Properties.GetAll org.freedesktop.portal.ScreenCast \
    > "$XDG_RUNTIME_DIR/screencast-properties.txt"
cat "$XDG_RUNTIME_DIR/screencast-properties.txt"
"$@" &
consumer=$!
pids="$consumer $pids"
wait "$consumer"
