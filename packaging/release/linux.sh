#!/usr/bin/env bash
# Native CI adapters; package creation remains with the platform producers.
# shellcheck source=packaging/release/common.sh
source "$(dirname "$0")/common.sh"
arch=$(uname -m)
format=$2
ubuntu=ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8
modules=$src/packaging/flatpak/modules.json
ffmpeg_pin=$(jq -ce '.modules[] | select(.name=="ffmpeg-lgpl") | .sources[] | select(.type=="archive")' "$modules")
ffmpeg_archive=$work/$(jq -er '.url | split("/") | last' <<< "$ffmpeg_pin")

install_tools() {
    local apt_options=(-o Acquire::Retries=2 -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15)
    local packages=(build-essential cmake meson ninja-build pkg-config patchelf
        libavcodec-dev libavutil-dev libyuv-dev libsdl2-dev libpipewire-0.3-dev libatspi2.0-dev
        libudev-dev libdbus-1-dev libxcb-xinput-dev libxcb1-dev libxau-dev libx11-dev
        libglib2.0-dev libsecret-1-dev uuid-dev libcurl4-openssl-dev libwebsockets-dev libssl-dev
        libsecp256k1-dev libjansson-dev libbz2-dev libcjson-dev libb2-dev libnsync-dev libsodium-dev libevent-dev)
    case $format in
        rpm) return ;;
        deb) packages+=(dpkg-dev lintian) ;;
        appimage)
            packages+=(libva-dev libdrm-dev nasm squashfs-tools podman)
            if [[ $arch == x86_64 ]]; then packages+=(libvpl-dev); fi ;;
        flatpak) packages=(flatpak flatpak-builder elfutils appstream) ;;
        *) return 2 ;;
    esac
    # Match the CI workaround for stalled EC2 mirrors (leave ports mirrors alone).
    sudo sed -Ei 's|://[a-z0-9-]+\.ec2\.archive\.ubuntu\.com/|://azure.archive.ubuntu.com/|g' \
        /etc/apt/sources.list.d/ubuntu.sources
    sudo apt-get "${apt_options[@]}" update --error-on=any
    sudo apt-get "${apt_options[@]}" install -y --no-install-recommends "${packages[@]}"
}

export_nostrc() {
    fetch_nostrc "$work/nostrc"
    git -C "$work/nostrc" submodule update --init --recursive --depth=1
    printf '%s\n' "$NOSTRC_REVISION" > "$work/nostrc/.nostrc-commit"
}

build_deb() {
    export_nostrc
    packaging/linux/build-nostrc.sh "$work/nostrc" "$work/nostrc-build"
    packaging/linux/static-json.py "$work/nostrc-build" "$work/nostrc-static"
    packaging/linux/build.sh native "$src" "$work/nostrc" "$work/nostrc-static" "$work/build"
    packaging/linux/produce.py native "$src" "$work/build" "$work/nostrc" "$work/native"
    packaging/linux/debian/build-deb.sh "$work/native/tree" "$version-1" "$work/deb"
    local deb=("$work/deb/"*.deb) name
    [[ ${#deb[@]} == 1 && -f ${deb[0]} ]]
    lintian "${deb[0]}"
    dpkg-deb -x "${deb[0]}" "$work/deb-payload"
    reject_codecs "$work/deb-payload"
    [[ -z $(find "$work/deb-payload" -iname 'libavcodec*' -o -iname 'libavutil*') ]]
    mkdir "$work/native-smoke"
    ln "${deb[0]}" "$work/native-smoke/metadesk.deb"
    docker run --rm -v "$work/native-smoke:/packages:ro" "$ubuntu" sh -ec '
        apt-get update --error-on=any
        apt-get install -y --no-install-recommends /packages/metadesk.deb
        metadesk-host --help
        metadesk-client --help
        ldd /usr/bin/metadesk-host > /tmp/closure
        ! grep -q "not found" /tmp/closure'
    name=$(basename "${deb[0]}")
    dpkg-deb -f "${deb[0]}" Depends > "$work/requires.txt"
    jq --rawfile requires "$work/requires.txt" '. + {system_dependencies: $requires}' \
        "$work/deb-payload/usr/share/metadesk/metadesk-bundle.json" > "$work/inventory.json"
    cp "${deb[0]}" "$upload/"
    record_asset "$name" deb "$arch" unsigned "$work/inventory.json"
}

build_rpm() {
    export_nostrc
    mkdir -p "$work/rpmbuild/SOURCES"
    git rev-parse HEAD > "$work/.metadesk-commit"
    git archive --format=tar --prefix="metadesk-$version/" --add-file="$work/.metadesk-commit" HEAD | \
        gzip -n > "$work/rpmbuild/SOURCES/metadesk-$version.tar.gz"
    tar -C "$work" --exclude=.git -czf "$work/rpmbuild/SOURCES/nostrc-$NOSTRC_REVISION.tar.gz" nostrc
    docker run --rm -v "$work:/work" -v "$src/packaging/linux/rpm:/spec:ro" "$FEDORA" bash -euc '
        dnf -y install rpm-build rpmlint dnf5-plugins
        dnf -y builddep /spec/metadesk.spec
        rpmbuild -ba --define "_topdir /work/rpmbuild" /spec/metadesk.spec
        cd /work/rpmbuild
        rpmlint RPMS/*/*.rpm SRPMS/*.rpm
        rpmfile=$(printf "%s\n" RPMS/*/metadesk-[0-9]*.rpm)
        rpm -qpl "$rpmfile" > /work/rpm-files.txt
        rpm -qpR "$rpmfile" > /work/requires.txt'
    if grep -Ei '/[^/]*(openh264|libx264|libx265|libavcodec|libavutil)[^/]*$' "$work/rpm-files.txt"; then
        echo 'Unexpected codec in native RPM payload' >&2
        return 1
    fi
    local rpm=("$work/rpmbuild/RPMS/$arch/"metadesk-[0-9]*.rpm) name
    [[ ${#rpm[@]} == 1 && -f ${rpm[0]} ]]
    mkdir "$work/native-smoke"
    ln "${rpm[0]}" "$work/native-smoke/metadesk.rpm"
    docker run --rm -v "$work/native-smoke:/packages:ro" "$FEDORA" bash -euc '
        dnf -y --setopt=install_weak_deps=False install /packages/metadesk.rpm
        rpm -V metadesk
        metadesk-host --help
        metadesk-client --help'
    name=$(basename "${rpm[0]}")
    jq --rawfile requires "$work/requires.txt" '. + {system_dependencies: $requires}' \
        "$work/rpmbuild/BUILD/metadesk-$version-build/native-output/tree/usr/share/metadesk/metadesk-bundle.json" \
        > "$work/inventory.json"
    cp "${rpm[0]}" "$upload/"
    record_asset "$name" rpm "$arch" unsigned "$work/inventory.json"
}

build_appimage() {
    export_nostrc
    local dep pin url rev name
    mkdir "$work/headers"
    for dep in nv-codec-headers amf-headers; do
        pin=$(jq -ce --arg dep "$dep" '.modules[] | select(.name==$dep) | .sources[0]' "$modules")
        url=$(jq -er .url <<< "$pin"); rev=$(jq -er .commit <<< "$pin")
        git init -q "$work/$dep"
        git -C "$work/$dep" fetch --depth=1 "$url" "$rev"
        git -C "$work/$dep" checkout -q --detach "$rev"
    done
    make -C "$work/nv-codec-headers" PREFIX="$work/headers" install
    cp -R "$work/amf-headers/amf/public/include" "$work/headers/include/AMF"
    locked_download "$ffmpeg_pin" "$ffmpeg_archive"
    mkdir "$work/ffmpeg-source"
    tar -xJf "$ffmpeg_archive" --strip-components=1 -C "$work/ffmpeg-source"
    if [[ ! -d $work/ffmpeg ]]; then
        PKG_CONFIG_PATH="$work/headers/lib/pkgconfig" MD_FFMPEG_CFLAGS="-I$work/headers/include" \
            packaging/ffmpeg/build-minimal-lgpl.sh container "$work/ffmpeg-source" "$work/ffmpeg"
    fi
    packaging/linux/build-nostrc.sh "$work/nostrc" "$work/nostrc-build" "$work/nostrc-prefix"
    PKG_CONFIG_PATH="$work/ffmpeg/install/lib/pkgconfig" \
        LD_LIBRARY_PATH="$work/ffmpeg/install/lib:$work/nostrc-prefix/lib" \
        packaging/flatpak/build-product.sh "$src" "$work/nostrc-prefix" "$work/build" "$work/client" client
    packaging/appimage/assemble.py "$work/client" "$work/ffmpeg/install" "$work/AppDir" --arch "$arch"
    reject_codecs "$work/AppDir"
    local lock=$src/packaging/appimage/tools.lock.json
    pin=$(jq -ce --arg suffix "appimagetool-$arch.AppImage" '.[] | select(.url | endswith($suffix))' "$lock")
    locked_download "$pin" "$work/appimagetool"
    pin=$(jq -ce --arg suffix "runtime-$arch" '.[] | select(.url | endswith($suffix))' "$lock")
    locked_download "$pin" "$work/runtime"
    chmod +x "$work/appimagetool"
    name=Metadesk-$TAG-$arch.AppImage
    packaging/appimage/make-image.sh "$work/AppDir" "$upload/$name" "$work/appimagetool" "$work/runtime"
    podman pull "$FEDORA"
    packaging/appimage/verify-container.sh "$upload/$name" "$FEDORA" "$arch" | tee "$work/codec-inventory.txt"
    copy_ffmpeg_records "$work/ffmpeg" "$work/ffmpeg-records"
    mv "$work/codec-inventory.txt" "$work/ffmpeg-records/"
    cp "$modules" "$work/ffmpeg-records/flatpak-module-pins.json"
    cc --version > "$work/ffmpeg-records/toolchain.txt"
    mkdir "$work/legal"
    cp "$work/ffmpeg-source/COPYING.LGPLv2.1" "$work/ffmpeg-source/LICENSE.md" "$work/legal/"
    ffmpeg_receipt "$ffmpeg_archive" "$work/ffmpeg-records" "$work/legal" "$name"
    record_asset "$name" appimage "$arch" unsigned "$work/AppDir/bundle.json"
}

build_flatpak() {
    flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
    flatpak install --user --noninteractive --no-related flathub org.gnome.Platform//49 org.gnome.Sdk//49
    locked_download "$ffmpeg_pin" "$ffmpeg_archive"
    local product id root name encoders
    encoders=$(packaging/ffmpeg/profile-encoders.sh flatpak "$arch" | tr ',' ' ')
    for product in Client Manager; do
        id=org.metadesk.$product
        appstreamcli validate --no-net "packaging/flatpak/assets/$id.metainfo.xml"
        root=$work/$product
        packaging/flatpak/build.sh "$product" "$root"
        local deployed
        deployed=$(flatpak info --user --show-location "$id")/files
        reject_codecs "$deployed"
        name=$id-$TAG-$arch.flatpak
        cp "$root/$id.flatpak" "$upload/$name"
        ffmpeg_receipt "$ffmpeg_archive" "$deployed/share/metadesk/ffmpeg-build" "$deployed/share/licenses/ffmpeg" "$name"
        # shellcheck disable=SC2086 # the encoder selector emits a vetted space-delimited list
        flatpak run --user --command=/app/libexec/metadesk/verify-codecs "$id" $encoders \
            > "$upload/$name.ffmpeg/records/codec-inventory.txt"
        cp "$modules" "$src/packaging/flatpak/build-ffmpeg.sh" "$upload/$name.ffmpeg/records/"
        flatpak list --user --columns=ref,commit > "$work/runtime-refs.txt"
        jq --rawfile refs "$work/runtime-refs.txt" \
            --arg app_commit "$(flatpak info --user --show-commit "$id")" \
            '. + {flatpak_refs:$refs, app_commit:$app_commit}' "$modules" > "$work/inventory.json"
        record_asset "$name" "flatpak-$product" "$arch" unsigned "$work/inventory.json"
        rm -rf "$root" # Only this job's private builder state; keep disk for the next app.
    done
}

case $1 in
    install) install_tools ;;
    build)
        case $format in
            deb) build_deb ;;
            rpm) build_rpm ;;
            appimage) build_appimage ;;
            flatpak) build_flatpak ;;
            *) exit 2 ;;
        esac ;;
    *) exit 2 ;;
esac
