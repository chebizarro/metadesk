#!/bin/sh
# Non-desktop gates only. No session/device/display/AT-SPI access is requested.
# Gate 5 MUST be separately authorized and exercised against a known application.
set -eu
[ "$#" = 2 ] || { echo 'usage: verify-image.sh IMAGE md-w9-NAME' >&2; exit 2; }
image=$1 name=$2
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
receipts=${MD_RECEIPT_DIR:-$script_dir/output/receipts}
case "$name" in md-w9-*) ;; *) echo 'private md-w9- name required' >&2; exit 2;; esac
mkdir -p "$receipts"
echo "Receipts: $receipts/$name.txt"
exec > "$receipts/$name.txt" 2>&1
cleanup() {
    # Missing --rm helpers must not prevent removal of the inspected container.
    for container in "$name" "${name}-hash" "${name}-direct"; do
        podman rm -f "$container" >/dev/null 2>&1 || :
    done
}
trap cleanup EXIT
podman run --rm --name "${name}-hash" --network=none --read-only --tmpfs /tmp:rw,noexec,nosuid --cap-drop=all --security-opt=no-new-privileges \
  --entrypoint sh "$image" -ec '
  cd /opt/metadesk
  sha256sum -c --quiet SHA256SUMS
  ldd bin/metadesk-host > /tmp/closure
  ! grep -q "not found" /tmp/closure
'
set +e
podman run --rm --name "${name}-direct" --network=none --read-only --tmpfs /tmp:rw,noexec,nosuid \
  --cap-drop=all --security-opt=no-new-privileges --entrypoint timeout \
  "$image" 10 /opt/metadesk/bin/metadesk-host --mcp --no-input > "$receipts/${name}-no-desktop.txt" 2>&1
status=$?
set -e
[ "$status" -eq 1 ]
grep -Fq 'host[mcp]: accessibility backend unavailable' "$receipts/${name}-no-desktop.txt"
echo "gate 4 no-desktop exit=$status"
# Inspect the launch configuration. This is NOT a SIGTERM or live-session test.
podman create --name "$name" --network=none --read-only --tmpfs /tmp:rw,noexec,nosuid \
  --cap-drop=all --security-opt=no-new-privileges "$image" >/dev/null
podman inspect "$name" --format '{{json .HostConfig}}'
podman start "$name" >/dev/null
podman wait "$name"
echo 'gate 5 NOT RUN; gate 6 mount/capability configuration only, not authenticated-session proof'
