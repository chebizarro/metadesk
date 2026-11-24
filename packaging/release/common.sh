# shellcheck shell=bash
# Shared release orchestration. Sourced by Bash on a checked-out release SHA.
set -euo pipefail
src=$(git rev-parse --show-toplevel)
work=${RUNNER_TEMP:?}/md-release
upload=$work/upload
mkdir -p "$upload"
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)
version=$(sed -nE 's/^Version:[[:space:]]+([^[:space:]]+).*$/\1/p' "$src/packaging/linux/rpm/metadesk.spec")
[[ $TAG =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z]+)*$ ]]
[[ ${TAG#v} == "$version" || ${TAG#v} == "$version-"* ]]

fetch_nostrc() {
    local repo=$1
    git init -q "$repo"
    git -C "$repo" fetch --depth=1 https://github.com/chebizarro/nostrc.git "$NOSTRC_REVISION"
    git -C "$repo" checkout -q --detach "$NOSTRC_REVISION"
}

# Fetch only bytes matching an existing producer lock. The caller chooses the
# path; never execute or extract anything before checking its digest.
locked_download() {
    local pin=$1 dest=$2 url sha
    url=$(jq -er .url <<< "$pin")
    sha=$(jq -er '.sha256 | select(test("^[0-9a-f]{64}$"))' <<< "$pin")
    if [[ ! -f $dest ]]; then
        curl --fail --location --proto '=https' --output "$dest.part" "$url"
        mv "$dest.part" "$dest"
    fi
    printf '%s  %s\n' "$sha" "$dest" | shasum -a 256 -c -
}

ffmpeg_receipt() {
    local archive=$1 records=$2 legal=$3 asset=$4 output
    output="$upload/$asset.ffmpeg"
    mkdir "$output"
    cp "$archive" "$output/"
    cp -R "$records" "$output/records"
    cp -R "$legal" "$output/notices"
    cp "$src/THIRD_PARTY_NOTICES.md" "$src/packaging/ffmpeg/README.md" \
        "$src/packaging/ffmpeg/build-minimal-lgpl.sh" "$src/packaging/ffmpeg/profile-encoders.sh" \
        "$src/packaging/ffmpeg/verify-codecs.c" "$output/"
}

copy_ffmpeg_records() {
    local build=$1 output=$2
    mkdir "$output"
    cp "$build/"configure*.txt "$build/profile.txt" "$build/encoders.txt" \
        "$build/build/"config*.h "$build/build/ffbuild/config.log" "$output/"
    if [[ -d $build/patches ]]; then
        cp -R "$build/patches" "$build/patches.txt" "$output/"
    fi
}

# Payload check (filesystem paths, including symlinks, not dependency strings
# or source/notice text referencing the optional runtime download).
reject_codecs() {
    local found
    found=$(find "$1" \( -type f -o -type l \) -iname '*openh264*' \
        -o \( -type f -o -type l \) \( -iname 'libx264*' -o -iname 'libx265*' \))
    [[ -z $found ]] || { printf 'Forbidden bundled codec:\n%s\n' "$found" >&2; return 1; }
}

record_asset() {
    # Last step after all package gates. Includes evidence inputs for the SBOM.
    local asset=$1 platform=$2 arch=$3 signing=$4 manifest=$5
    jq -n --arg asset "$asset" --arg platform "$platform" --arg arch "$arch" \
        --arg signing "$signing" --arg revision "$(git rev-parse HEAD)" \
        --arg nostrc "$NOSTRC_REVISION" --slurpfile inventory "$manifest" \
        '{asset:$asset, platform:$platform, arch:$arch, signing:$signing, revision:$revision,
          nostrc:$nostrc, inventory:$inventory[0]}' > "$upload/$asset.json"
}
