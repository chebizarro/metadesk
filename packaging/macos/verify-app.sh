#!/bin/sh
# Verify a Metadesk.app produced by bundle_app.py (md-kb7 prototype).
#
# Static gates mirror deploy/containers/reference/bundle-contract.md gates 2-3:
# every Mach-O references only base-owned (/usr/lib, /System) or bundled
# (@rpath -> Contents/Frameworks) libraries and carries no producer rpath.
# Runtime gates run each binary with an empty environment and a system-only
# PATH and record every image dyld actually loads, which also covers dlopen.
set -eu
[ "$#" -eq 1 ] || { echo "usage: $0 Metadesk.app" >&2; exit 2; }
app=$(cd "$1" && pwd)
fw="$app/Contents/Frameworks"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

for img in "$app"/Contents/MacOS/* "$fw"/*; do
    rel=${img#"$app"/}
    self=$(otool -D "$img" | sed -n 2p)
    for ref in $(otool -L "$img" | tail -n +2 | awk '{print $1}'); do
        [ "$ref" = "$self" ] && continue
        case "$ref" in
            /usr/lib/*|/System/*) ;;
            @rpath/*) [ -f "$fw/${ref#@rpath/}" ] || bad "$rel -> $ref not bundled" ;;
            *) bad "$rel -> $ref is outside the bundle" ;;
        esac
    done
    for rp in $(otool -l "$img" | awk '/cmd LC_RPATH/{getline; getline; print $2}'); do
        case "$rp" in
            @executable_path/../Frameworks|@loader_path) ;;
            *) bad "$rel has producer rpath $rp" ;;
        esac
    done
done

codesign --verify --strict --deep "$app" || bad "codesign verification"

# Every image's LC_BUILD_VERSION minos must not exceed the release floor;
# Homebrew bottles carry the builder's OS version and fail this.
min=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$app/Contents/Info.plist")
if [ -n "${MD_MACOS_MIN:-}" ]; then
    newest=$(printf '%s\n%s\n' "$MD_MACOS_MIN" "$min" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
    [ "$newest" = "$MD_MACOS_MIN" ] || bad "bundle needs macOS $min, release floor is $MD_MACOS_MIN"
fi
echo "minimum macOS: $min"
python3 "$(dirname "$0")/macho.py" "${MD_MACOS_MIN:-$min}" "$app" || bad "Mach-O deployment floor"

home=$(mktemp -d "${TMPDIR:-/tmp}/md-verify.XXXXXX")
trap 'rm -rf "$home"' EXIT INT TERM
# perl alarm: macOS has no coreutils timeout; a hang is a failure, not a wait.
# DYLD_PRINT_LIBRARIES is set inside perl: SIP strips DYLD_* from the
# environment of protected /usr/bin binaries before they can pass it on.
runclean() {
    log=$1; shift
    # shellcheck disable=SC2016 # $ENV is perl, not shell
    env -i HOME="$home" PATH=/usr/bin:/bin:/usr/sbin:/sbin TMPDIR="$home" \
        /usr/bin/perl -e '$ENV{DYLD_PRINT_LIBRARIES} = 1; alarm shift; exec @ARGV or die' 20 "$@" \
        >"$home/$log.out" 2>"$home/$log.err" && rc=0 || rc=$?
    sed -n 's/^dyld\[[0-9]*\]: <[^>]*> //p' "$home/$log.err" >"$home/$log.loaded"
    while IFS= read -r path; do
        case "$path" in
            "$app"/*|/usr/lib/*|/System/*) ;;
            *) bad "$log loaded $path from outside the bundle" ;;
        esac
    done <"$home/$log.loaded"
    echo "$log: exit $rc, $(wc -l <"$home/$log.loaded" | tr -d ' ') images, $(grep -c "^$app/" "$home/$log.loaded" || true) bundled"
}

runclean host-help "$app/Contents/MacOS/metadesk-host" --help
[ "$rc" -eq 0 ] || bad "metadesk-host --help exited $rc"
runclean client-help "$app/Contents/MacOS/metadesk-client" --help
[ "$rc" -le 1 ] || bad "metadesk-client --help exited $rc"
# Contract gate 4 analogue: with no signer/relay/desktop configuration the
# host must fail promptly with a diagnostic, not hang or start a substitute.
runclean host-bare "$app/Contents/MacOS/metadesk-host"
[ "$rc" -ne 0 ] && [ "$rc" -ne 142 ] || bad "bare metadesk-host exited $rc (142 = hung until alarm)"
[ -s "$home/host-bare.err" ] || bad "bare metadesk-host printed no diagnostic"
grep -qv '^dyld\[' "$home/host-bare.err" || bad "bare metadesk-host printed no diagnostic"

if grep -q '"libSDL3.dylib"' "$app/Contents/Resources/metadesk-bundle.json"; then
    # sdl2-compat dlopens SDL3 when SDL initialises; prove the bundled copy is
    # the one loaded by running a display-requiring client invocation.
    runclean client-sdl "$app/Contents/MacOS/metadesk-client" 127.0.0.1 --port 9 --timeout 500
    grep -qx "$fw/libSDL3.dylib" "$home/client-sdl.loaded" || bad "bundled libSDL3.dylib was not loaded"
fi

if [ "$fail" -ne 0 ]; then
    echo "verification failed" >&2
    exit 1
fi
echo "PASS: $app"
