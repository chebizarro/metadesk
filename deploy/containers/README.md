<!-- validated on Docker 29.1.3 / rootless Podman 4.9.3, fixture cells only; see reference/validation.md. -->
# Tier A: attached-desktop MCP observation

Local reference, not release packaging. Ubuntu 24.04/aarch64 evidence and
**pending/blocked cells**: [validation](reference/validation.md).
No FIPS/mesh, Kubernetes, video, devices, or registry publication.

## Prepare the operator-managed desktop

Reference identity is **1000:1000**. Other IDs require matching image build args,
manifest user/runtime paths, and host identity; they are unvalidated. Never use
`:Z` to relabel a live desktop runtime directory. Mounting buses grants broad
user-session access, **not read-only D-Bus**, even with read-only mounts.

```sh
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
# Query the actual session; do not assume the common AT-SPI path exists.
gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus \
  --method org.a11y.Bus.GetAddress
export AT_SPI_BUS_ADDRESS='unix:path=<discovered-socket-path>[,guid=<guid>]'
export AT_SPI_DIR='<directory-containing-that-socket>'
export SESSION_BUS_SOCKET=/run/user/1000/bus
```

Require filesystem Unix sockets owned by the desktop user, and at least one
registered accessible app. Abstract/TCP/proxy addresses fail preparation; no
host-network fallback. The AT-SPI directory is mounted at its **same absolute
path**, so socket replacement within it is visible. Session socket replacement
needs **container recreation**, not just readiness polling or assumed remounts.

## Stage and build (outside the image)

See [bundle contract](reference/bundle-contract.md). On matching Ubuntu:

```sh
./deploy/containers/reference/throwaway-bundle.sh \
  "$PWD" /home/bizarro/nostrc "$PWD/deploy/containers/runtime-bundle"
cd deploy/containers
export MD_BASE=ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8
sudo docker build --build-arg BASE="$MD_BASE" -f Dockerfile.agent \
  -t metadesk-agent:ah9-4 .
```

The image defaults to stdio. No secrets go in the build context:
provision a random, nonempty bearer token **outside this directory**, owned by
1000:1000 and mode **0600**. Export `MCP_TOKEN_FILE` to its absolute path.
Never log the token or commit it. Local Compose file secrets preserve source
ownership/mode; the entrypoint checks them rather than trusting mode remapping.

## Docker Compose

On AppArmor-enabled Ubuntu, load the explicit reference profile before starting:

```sh
sudo apparmor_parser -r reference/metadesk-agent.apparmor
# sudo normally clears variables; preserve only the required non-secret values.
sudo --preserve-env=AT_SPI_BUS_ADDRESS,AT_SPI_DIR,SESSION_BUS_SOCKET,MCP_TOKEN_FILE,MD_BASE \
  docker compose -f compose.agent.yaml up -d --no-build --wait --wait-timeout 45
```

`docker-default` denies D-Bus Hello on the tested Ubuntu host. This reference
loads a separate profile, permits session/accessibility D-Bus to unconfined
host peers, and still denies mount/kernel writes. It does not modify
`docker-default` or use `apparmor=unconfined`. Review it for your LSM/desktop;
SELinux and confined desktop peers are unvalidated. See also the
[Moby default-profile source](https://github.com/moby/profiles/blob/main/apparmor/template.go)
for the separate Docker-default policy; this reference is not its replacement
for arbitrary workloads.

HTTP listens on **127.0.0.1:7710 inside the private container netns**. There are
no published ports; `-p` cannot reach container loopback. Use `docker compose
exec -T agent` for a local MCP client/diagnostic, or an explicitly authorized
same-netns client. Remote access/proxy/holder topology belongs to md-ah9.5.

```sh
sudo --preserve-env=AT_SPI_BUS_ADDRESS,AT_SPI_DIR,SESSION_BUS_SOCKET,MCP_TOKEN_FILE,MD_BASE \
  docker compose -f compose.agent.yaml exec -T agent \
  python3 /opt/metadesk/reference/smoke_mcp.py --http --expect 'Known app label'
sudo --preserve-env=AT_SPI_BUS_ADDRESS,AT_SPI_DIR,SESSION_BUS_SOCKET,MCP_TOKEN_FILE,MD_BASE \
  docker compose -f compose.agent.yaml down --volumes
sudo apparmor_parser -R reference/metadesk-agent.apparmor
```

Keep the required environment exported for every Compose command. `/healthz`
checks transport alongside registry readiness; probes **never GET /mcp** or
perform MCP initialization. Docker `on-failure` supplies engine restart backoff;
health is readiness only and does not cause restarts. Known bus-loss detection
prevents logout recovery today (md-ah9.15). Do not test it by logging out a user.

## Rootless Podman (explicit CLI, not this Compose file)

Load/build the same image locally. Keep-ID preserves the desktop's host UID.
Podman 4.9.3 needs `U` on the **private tmpfs**; it rejects tmpfs `uid=/gid=`.
Never apply `U` to the host session directory. Its tested LSM state had AppArmor
**disabled for rootless containers**; no SELinux qualification is claimed.

```sh
podman run --rm -i --userns=keep-id --read-only --read-only-tmpfs=false \
  --cap-drop=ALL --security-opt=no-new-privileges \
  --tmpfs /tmp:rw,nosuid,nodev,noexec,mode=1777 \
  --tmpfs /run/user/1000:rw,nosuid,nodev,noexec,mode=0700,U \
  --mount type=bind,source="$SESSION_BUS_SOCKET",target=/run/user/1000/bus,readonly \
  --mount type=bind,source="$AT_SPI_DIR",target="$AT_SPI_DIR",readonly \
  -e AT_SPI_BUS_ADDRESS="$AT_SPI_BUS_ADDRESS" metadesk-agent:ah9-4
```

This starts stdio (JSON-RPC lines; **no TTY**). Startup can consume up to 30 s;
configure the MCP client's startup allowance accordingly. HTTP adds the secret
mount and replaces the default command with:

```sh
# Add before the image:
--mount type=bind,source="$MCP_TOKEN_FILE",target=/run/secrets/mcp-token,readonly
# Add after the image:
/opt/metadesk/bin/metadesk-host --mcp-http 7710 --mcp-http-bind 127.0.0.1 \
  --no-input --mcp-token-file /run/secrets/mcp-token
```

For persistent rootless HTTP, use user systemd as the **single** restart owner:
prefix the above HTTP `podman run --rm` command (omit `-i`) with
`systemd-run --user --unit=metadesk-agent --collect --property=Restart=on-failure
--property=RestartSec=5s --property=StartLimitIntervalSec=300s
--property=StartLimitBurst=5`. Do not also set Podman's native restart policy.
Stop with `systemctl --user stop metadesk-agent`; after a rate limit, inspect the
failure and explicitly restart. Fixed backoff was exercised at 2 s in validation;
the conservative 5 s operational setting is unvalidated. Recreate when session
socket identity/path changes. No claim of automatic logout/relogin recovery.

For controlled test reproduction use `reference/fixture.py`, `smoke_mcp.py`, and
`validate.py`; the results doc gives the isolated-session setup and cleanup.
Never repoint or kill the real desktop bus. Health logs do not contain tree data.
