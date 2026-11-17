#!/bin/sh
# validated on Docker 29.1.3 / rootless Podman 4.9.3, fixture cells; see reference/validation.md.
set -eu
# PID 1 must terminate even while preparing the desktop; exec host afterwards.
waiter=
trap '[ -z "$waiter" ] || kill "$waiter" 2>/dev/null || :; exit 143' TERM INT
fail() { echo "metadesk preparation: $*" >&2; exit 1; }
[ "$(id -u)" = "${MD_UID:?MD_UID required}" ] && [ "$(id -u)" != 0 ] || fail 'wrong desktop identity'
: "${AT_SPI_BUS_ADDRESS:?discover and mount the filesystem AT-SPI bus}"
# HTTP references always require the mounted secret, even on loopback.
http=false
for arg in "$@"; do [ "$arg" != --mcp-http ] || http=true; done
if "$http"; then
    [ -f /run/secrets/mcp-token ] || fail 'missing /run/secrets/mcp-token'
    [ "$(stat -c %a /run/secrets/mcp-token)" = 600 ] || fail 'token must be mode 0600'
    [ "$(stat -c %u /run/secrets/mcp-token)" = "$MD_UID" ] || fail 'token must belong to desktop UID'
    [ -s /run/secrets/mcp-token ] || fail 'empty token'
fi
# Missing mounts/incorrect identities are configuration errors, not startup delays.
[ -S "$XDG_RUNTIME_DIR/bus" ] || fail 'missing session bus socket'
[ "$(stat -c %u "$XDG_RUNTIME_DIR/bus")" = "$MD_UID" ] || fail 'wrong session bus owner'
atspi_path=${AT_SPI_BUS_ADDRESS#unix:path=}
atspi_path=${atspi_path%%,*}
[ -S "$atspi_path" ] || fail 'missing filesystem AT-SPI socket'
# Bound the entire wait, including D-Bus calls, not just the retry count.
timeout 30 sh -c 'until /opt/metadesk/reference/probe.py; do sleep 1; done' &
waiter=$!
wait "$waiter" || fail 'desktop not ready within 30 seconds'
waiter=
exec "$@"
