#!/usr/bin/env bash
# shellcheck source=packaging/release/common.sh
source "$(dirname "$0")/common.sh"
[[ $(uname -m) == arm64 ]]
[[ $(jq -er '.[] | select(.name=="nostrc").commit' packaging/macos/deps.lock.json) == "$NOSTRC_REVISION" ]]
export MD_CODESIGN_IDENTITY=- MD_DMG=1 MD_DEPS_DOWNLOADS="$RUNNER_TEMP/md-downloads"
export MD_RELEASE_DEPS_DIR="$RUNNER_TEMP/md-macos-deps"
signing=ad-hoc
mountpoint=$work/mounted
keychain=$work/signing.keychain-db
cleanup() {
    if mount | grep -Fq " on $mountpoint ("; then hdiutil detach "$mountpoint"; fi
    if [[ -f $keychain ]]; then security delete-keychain "$keychain"; fi
    rm -f "$work/certificate.p12" "$work/notary.p8"
}
trap cleanup EXIT

if [[ -n ${APPLE_CERTIFICATE:-} && -n ${APPLE_CERTIFICATE_PASSWORD:-} && -n ${APPLE_SIGN_IDENTITY:-} &&
      -n ${APPLE_NOTARY_KEY:-} && -n ${APPLE_NOTARY_KEY_ID:-} && -n ${APPLE_NOTARY_ISSUER:-} ]]; then
    password=$(openssl rand -hex 24)
    echo "::add-mask::$password"
    umask 077
    printf '%s' "$APPLE_CERTIFICATE" | base64 -D > "$work/certificate.p12"
    printf '%s' "$APPLE_NOTARY_KEY" | base64 -D > "$work/notary.p8"
    security create-keychain -p "$password" "$keychain"
    security set-keychain-settings "$keychain"
    security unlock-keychain -p "$password" "$keychain"
    security import "$work/certificate.p12" -k "$keychain" -P "$APPLE_CERTIFICATE_PASSWORD" -T /usr/bin/codesign
    security set-key-partition-list -S apple-tool:,apple: -s -k "$password" "$keychain"
    security list-keychains -d user -s "$keychain"
    rm "$work/certificate.p12"
    umask 022
    MD_CODESIGN_IDENTITY=$APPLE_SIGN_IDENTITY
    signing='Developer ID + notarized'
else
    echo 'Complete signing/notarization credentials unavailable: unsigned DMG with ad-hoc app signature.'
fi
unset APPLE_CERTIFICATE APPLE_CERTIFICATE_PASSWORD APPLE_NOTARY_KEY
# Do not export notarization identifiers to the dependency build environment.
notary_id=${APPLE_NOTARY_KEY_ID:-} notary_issuer=${APPLE_NOTARY_ISSUER:-}
unset APPLE_NOTARY_KEY_ID APPLE_NOTARY_ISSUER APPLE_SIGN_IDENTITY

fetch_nostrc "$work/nostrc"
packaging/macos/build-release.sh "$work/nostrc" "$work/build"
name=Metadesk-$TAG-macos-arm64.dmg
if [[ $signing == ad-hoc ]]; then name=Metadesk-$TAG-macos-arm64-adhoc.dmg; fi
mv "$work/build/app/Metadesk-$version.dmg" "$upload/$name"
if [[ $signing != ad-hoc ]]; then
    xcrun notarytool submit "$upload/$name" --key "$work/notary.p8" \
        --key-id "$notary_id" --issuer "$notary_issuer" --wait
    xcrun stapler staple "$upload/$name"
    xcrun stapler validate "$upload/$name"
    spctl --assess --type open --context context:primary-signature --verbose=2 "$upload/$name"
fi

hdiutil attach -readonly -nobrowse -mountpoint "$mountpoint" "$upload/$name"
app=$mountpoint/Metadesk.app
MD_MACOS_MIN=13.0 packaging/macos/verify-app.sh "$app"
reject_codecs "$app/Contents/Frameworks"
deps=$MD_RELEASE_DEPS_DIR
fw=$app/Contents/Frameworks
# Enumerate the actual on-disk DMG libraries, not the producer prefix.
clang -I"$deps/prefix/include" packaging/ffmpeg/verify-codecs.c \
    "$fw"/libavcodec.*.dylib "$fw"/libavutil.*.dylib -Wl,-rpath,"$fw" -o "$work/verify-codecs"
copy_ffmpeg_records "$deps/build/ffmpeg" "$work/ffmpeg-records"
"$work/verify-codecs" h264_videotoolbox | tee "$work/ffmpeg-records/codec-inventory.txt"
cp "$deps/toolchain.txt" "$deps/deps.lock.json" "$work/ffmpeg-records/"
pin=$(jq -ce '.[] | select(.name=="ffmpeg")' "$deps/deps.lock.json")
archive=$(jq -er .archive <<< "$pin")
mkdir -p "$MD_DEPS_DOWNLOADS"
locked_download "$pin" "$MD_DEPS_DOWNLOADS/$archive"
ffmpeg_receipt "$MD_DEPS_DOWNLOADS/$archive" "$work/ffmpeg-records" "$deps/licenses/ffmpeg" "$name"
jq --slurpfile pins "$deps/deps.lock.json" '. + {pins:$pins[0]}' \
    "$app/Contents/Resources/metadesk-bundle.json" > "$work/inventory.json"
record_asset "$name" dmg arm64 "$signing" "$work/inventory.json"
