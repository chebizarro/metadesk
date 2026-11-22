# W15 session parity

## md-wvd2 — negotiated quality

Optional `quality` objects carry `max_width`, `max_height`, `bitrate` (bps),
`fps` in the authenticated request/accept and secret-free bind ACK. Missing
means legacy host defaults; malformed values fail rather than default. Explicit
non-default preferences against an old host produce an upgrade error.

Host CLI settings are ceilings, intersected per dimension/rate with the client.
Bitrate is 100 Kbps–100 Mbps, FPS 1–240, dimensions 2–16384 or zero/unbounded.
Native capture retains the host cap (SCK hardware, DXGI GPU, PipeWire CPU).
An additional session downscale uses bounded reusable CPU scratch, preserving
native output coordinates. The capture owner reopens the encoder transactionally
for FPS, resizes via capture-sync, and paces submitted frames; RTT AIMD is bounded
by the negotiated ceiling. Settings are ceilings/targets, not a promise that a
hardware encoder emits an exact byte rate or a static desktop produces new frames.

Client: `--max-size 1280x720 --bitrate 2000000 --fps 30`, or F1 → Session quality.
Overlay changes are temporary; `--save-quality` persists CLI preferences per host
in self-encrypted NIP-78 kind 30078, d=`metadesk-session-preferences`, version 1
`hosts[hex].quality`. Unknown fields/other hosts are preserved. The host key is
inside ciphertext, not a public d-tag. Reads complete at EOSE; writes require
positive relay OK. Replacements use NIP-33 created-at/lowest-id ordering. Local
concurrent saves are rejected; concurrent writers on different devices remain
Nostr last-writer-wins, not distributed CAS. No grant is stored or inferred here.

Evidence so far: macOS full suite 61 pass / 1 explicit native AX skip before
preference integration; focused quality, real host-dispatch seam and encrypted
preference relay roundtrip 3/3 after integration. Real Mac SCK host/client through
private relay and external NIP-5F signers decoded 640x416 under requested
640x480, 1 Mbps, 15 FPS; host logged applied settings. No public relay or input
injection was used. Final cross-platform/sanitizer gates are recorded below when
run; this paragraph alone is not those qualifications.
