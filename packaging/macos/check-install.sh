#!/bin/sh
# Check an installed (including DESTDIR-staged) prefix without dyld overrides.
set -eu
[ "$#" = 1 ] || { echo "usage: $0 INSTALLED_PREFIX" >&2; exit 2; }
prefix=$(cd "$1" && pwd -P)
home=$(mktemp -d "${TMPDIR:-/private/tmp}/md-install-check.XXXXXX")
trap 'rm -rf "$home"' EXIT INT TERM
set -- metadesk-host metadesk-client
if [ -x "$prefix/bin/metadesk-macos-manager" ]; then
    set -- "$@" metadesk-macos-manager
fi
for binary do
    [ -x "$prefix/bin/$binary" ] || { [ "$binary" = metadesk-client ] && continue; exit 1; }
    arg=--help
    [ "$binary" != metadesk-macos-manager ] || arg=--print-client-path
    rc=0
    # shellcheck disable=SC2016 # $ENV is Perl, not shell expansion
    env -i HOME="$home" CFFIXED_USER_HOME="$home" TMPDIR="$home" PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        /usr/bin/perl -e '$ENV{DYLD_PRINT_LIBRARIES}=1; alarm shift; exec @ARGV or die' 20 \
        "$prefix/bin/$binary" "$arg" >"$home/$binary.out" 2>"$home/$binary.err" || rc=$?
    cat "$home/$binary.out"
    grep -v '^dyld\[' "$home/$binary.err" || true
    grep -F "$prefix/" "$home/$binary.err" | grep -q 'libmetadesk' || {
        echo 'FAIL: installed binary did not load the staged libmetadesk' >&2; exit 1;
    }
    case "$binary:$rc" in
        metadesk-host:0|metadesk-client:0|metadesk-client:1)
            grep -iq 'usage' "$home/$binary.out" "$home/$binary.err" || exit 1 ;;
        metadesk-macos-manager:0)
            grep -Fxq "$prefix/bin/metadesk-client" "$home/$binary.out" || exit 1 ;;
        *) exit 1 ;;
    esac
    echo "PASS: installed $binary $arg (exit $rc, no dyld overrides)"
done
