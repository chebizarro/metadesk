#!/bin/sh
# validated on rootless Podman 4.9.3 + Docker Compose 5.5.1; see reference/mesh-validation.md (md-ba4).
# Podman-owned underlay + namespace holder for compose.mesh.yaml. Compose must
# not own it: Podman <5.4 reports a container's NetworkID as the network name,
# so Compose sees the holder as diverged and stops it under its consumers.
#   mesh-netns.sh up    create, or verify an identical running holder (idempotent)
#   mesh-netns.sh down  remove holder and network; Podman refuses while consumers exist
# Names are fixed to match compose.mesh.yaml; only addressing is configurable.
set -eu
cmd=${1:-}
: "${MD_BASE:?pinned Ubuntu digest}"
holder=ah9-5-netns
net=ah9-5-underlay
subnet=${MD_UNDERLAY_SUBNET:-10.89.95.0/24}
ip=${MD_NETNS_IP:-10.89.95.2}
owner=io.metadesk.netns
label=$owner.spec
# Capability-free keep-ID holder; no host ports and no restart policy: a
# restarted holder gets a new namespace that existing consumers never rejoin.
set -- --network "$net" --ip "$ip" --userns keep-id \
    --user 1000:1000 --read-only --cap-drop all --security-opt no-new-privileges \
    --init --stop-timeout 5 "$MD_BASE" sleep infinity
spec=$(printf '%s\n' "$subnet" "$@" | sha256sum | cut -c1-64)
die() { echo "mesh-netns: $*" >&2; exit 1; }
owned() {  # owned network|container NAME
    case $1 in network) f=.Labels ;; *) f=.Config.Labels ;; esac
    [ "$(podman "$1" inspect -f "{{index $f \"$owner\"}}" "$2")" = 1 ]
}

case $cmd in
up)
    if podman network exists "$net"; then
        owned network "$net" || die "network $net exists but is not managed by $0"
        have=$(podman network inspect -f '{{range .Subnets}}{{.Subnet}} {{end}}' "$net")
        [ "$have" = "$subnet " ] || die "network $net has subnet(s) $have, expected $subnet"
    else
        podman network create --label "$owner=1" --subnet "$subnet" "$net" >/dev/null
    fi
    if ! podman container exists "$holder"; then
        podman run -d --name "$holder" --label "$owner=1" --label "$label=$spec" "$@" >/dev/null
        echo "mesh-netns: created $holder"
        exit 0
    fi
    owned container "$holder" || die "$holder exists but is not managed by $0"
    have=$(podman container inspect -f "{{index .Config.Labels \"$label\"}}" "$holder")
    [ "$have" = "$spec" ] || die "$holder differs from this spec; run compose down, then $0 down and up"
    # A stopped holder's consumers lost their underlay; restarting cannot repair
    # them. Replace it only through Podman's dependency-checked removal.
    state=$(podman container inspect -f '{{.State.Status}}' "$holder")
    [ "$state" = running ] || die "$holder is $state; run compose down, then $0 down and up"
    live=$(podman container inspect -f \
        "{{.HostConfig.RestartPolicy.Name}}|{{with index .NetworkSettings.Networks \"$net\"}}{{.IPAddress}}{{end}}" "$holder")
    case $live in
    "|$ip" | "no|$ip") echo "mesh-netns: $holder up to date" ;;
    *) die "$holder runtime state drifted (restart policy|address: $live); run compose down, then $0 down and up" ;;
    esac
    ;;
down)
    if podman container exists "$holder"; then
        owned container "$holder" || die "$holder is not managed by $0; not removing it"
        podman rm -f -t 5 "$holder" >/dev/null
    fi
    if podman network exists "$net"; then
        owned network "$net" || die "network $net is not managed by $0; not removing it"
        podman network rm "$net" >/dev/null
    fi
    ;;
*)
    echo "usage: $0 up|down (environment: MD_BASE, optional MD_UNDERLAY_SUBNET, MD_NETNS_IP)" >&2
    exit 2
    ;;
esac
