#!/bin/sh
# Check an installed (including DESTDIR-staged) prefix without dyld overrides.
set -eu
[ "$#" = 1 ] || { echo "usage: $0 INSTALLED_PREFIX" >&2; exit 2; }
prefix=$(cd "$1" && pwd -P)
home=$(mktemp -d "${TMPDIR:-/private/tmp}/md-install-check.XXXXXX")
trap 'rm -rf "$home"' EXIT INT TERM
for binary in metadesk-host metadesk-client; do
    [ -x "$prefix/bin/$binary" ] || { [ "$binary" = metadesk-client ] && continue; exit 1; }
    rc=0
    # shellcheck disable=SC2016 # $ENV is Perl, not shell expansion
    env -i HOME="$home" TMPDIR="$home" PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        /usr/bin/perl -e '$ENV{DYLD_PRINT_LIBRARIES}=1; alarm shift; exec @ARGV or die' 20 \
        "$prefix/bin/$binary" --help >"$home/$binary.out" 2>"$home/$binary.err" || rc=$?
    cat "$home/$binary.out"
    grep -v '^dyld\[' "$home/$binary.err" || true
    grep -F "$prefix/" "$home/$binary.err" | grep -q 'libmetadesk' || {
        echo 'FAIL: installed binary did not load the staged libmetadesk' >&2; exit 1;
    }
    case "$binary:$rc" in metadesk-host:0|metadesk-client:0|metadesk-client:1) ;; *) exit 1 ;; esac
    grep -iq 'usage' "$home/$binary.out" "$home/$binary.err" || exit 1
    echo "PASS: installed $binary --help (exit $rc, no dyld overrides)"
done
