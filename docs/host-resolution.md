# Connecting by host identity

`metadesk-client` accepts an `npub`, `nprofile`, 64-hex public key, or
`name@domain` (optionally prefixed by `nostr:`). A raw IP or hostname is only
an explicit `--host` route, never the identity. Keep the host key separately
and supply it with `--pin` when reconnecting by name:

```sh
metadesk-client --npub alice@example.org --pin "$SAVED_HOST_NPUB" --relay "$BOOTSTRAP_RELAY"
```

A pin mismatch stops before signaling or dialing the host. The CLI does not
save or replace pins. NIP-05 uses a ten-second, size-bounded HTTPS lookup
with certificate and hostname verification, no environment proxy, and a
socket check on the actual IP before connecting. NIP-05 redirect responses
are rejected without contacting the redirect destination. The NIP-05 insecure
and timeout environment settings are not honored.

## Relay selection

Unmanaged CLI invocations may use the built-in default. `--managed` requires
one or more explicitly configured `--relay` values and a bound client role;
without a bootstrap relay it refuses before any lookup. The Linux manager
already stores bootstrap choices locally and offers a NIP-11 authentication
check on each saved relay. No authentication requirement advertised by NIP-11
is not a guarantee that a relay will not send a NIP-42 challenge.

The client looks for signed NIP-65 and kind-10050 lists on bootstrap relays
and valid address hints. It subscribes for incoming DMs on its own inbox
list and publishes outgoing DMs to the recipient's inbox list. If the
recipient has no usable inbox targets, it selects the recipient's NIP-65 read
targets, or address hints and bootstrap relays if no read targets are usable. Without a local DM list it
receives on the explicit bootstrap relays for compatibility. Client
preference reads use bootstrap and own NIP-65 read/write targets; writes
prefer own NIP-65 write targets. Discovery closes only its scoped REQ at the
history boundary or its deadline; application subscriptions remain live.
The host's existing configured-relay workflow is unchanged: for a session
with a legacy host, its configured relays must reach both the request and
reply routes.

Automatic relay URLs must use `wss://` and pass a public-address check at
selection and again just before pool admission. The check and the nostrc
dial/reconnect resolve DNS independently: **this is not rebinding-proof**
(`md-dc2m`). In the time-of-check/time-of-use window a private address can
receive a connection attempt; verified TLS prevents a WebSocket session if
that address cannot present a matching certificate. An operator can explicitly
configure local and `ws://` relays; plaintext use is flagged.
