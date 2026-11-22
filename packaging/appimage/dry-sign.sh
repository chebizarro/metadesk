#!/bin/sh
# Ephemeral, local-only demonstration; never use the user's GPG home or release key.
set -eu
[ "$#" = 2 ] || { echo 'usage: dry-sign.sh ARTIFACT NEW_PRIVATE_DIR' >&2; exit 2; }
artifact=$(realpath "$1") private=$2
[ ! -e "$private" ] || exit 2
mkdir -m700 "$private"
private=$(realpath "$private")
export GNUPGHOME="$private/gnupg"
mkdir -m700 "$GNUPGHOME"
trap 'gpgconf --kill all || :; rm -rf "$GNUPGHOME"' EXIT HUP INT TERM
# No passphrase only because this identity is disposable and never trusted for release.
gpg --batch --pinentry-mode loopback --passphrase '' --quick-generate-key 'W14 DRY RUN <md-w14@example.invalid>' ed25519 sign 1d
gpg --batch --armor --export > "$private/DRY-RUN-public.asc"
gpg --batch --armor --detach-sign --output "$private/DRY-RUN-artifact.asc" "$artifact"
gpg --batch --verify "$private/DRY-RUN-artifact.asc" "$artifact"
# Intentionally invalid endpoint; no network upload or update service is configured.
zsyncmake -u "https://example.invalid/DRY-RUN/$(basename "$artifact")" -o "$private/DRY-RUN.zsync" "$artifact"
printf '%s\n' 'DRY RUN ONLY: not a release identity, signature, or update endpoint.' > "$private/README.txt"
