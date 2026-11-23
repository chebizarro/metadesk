# Source0: release source including packaging/linux. Source1: exact nostrc source.
# Offline builds: both archives and all submodules must be complete; no fetches.
# Bound GCC LTO workers as well as make/ninja on small shared builders.
%global _smp_build_ncpus 2
%global _smp_mflags -j2
%global _lto_cflags -flto=2 -ffat-lto-objects
%global nostrc_revision 6f7a8d025768d497bef700fe55eea1c687dac328
Name:           metadesk
Version:        0.1.0
Release:        1%{?dist}
Summary:        Authenticated desktop host and video client
License:        MIT AND Apache-2.0
URL:            https://github.com/chebizarro/metadesk
Source0:        metadesk-%{version}.tar.gz
Source1:        nostrc-%{nostrc_revision}.tar.gz
BuildRequires:  gcc gcc-c++ cmake meson ninja-build python3 binutils patchelf
BuildRequires:  pkgconfig(libavcodec) >= 60
BuildRequires:  pkgconfig(libavutil) >= 58
BuildRequires:  pkgconfig(sdl2) pkgconfig(libpipewire-0.3) pkgconfig(atspi-2)
BuildRequires:  pkgconfig(glib-2.0) pkgconfig(gio-unix-2.0) pkgconfig(libsecret-1)
BuildRequires:  pkgconfig(xcb-xinput) >= 1.15
BuildRequires:  pkgconfig(xcb) >= 1.15
BuildRequires:  pkgconfig(xau)
BuildRequires:  pkgconfig(libudev) pkgconfig(dbus-1) pkgconfig(uuid)
BuildRequires:  pkgconfig(libcurl) pkgconfig(libwebsockets) pkgconfig(openssl)
BuildRequires:  pkgconfig(libsecp256k1) pkgconfig(jansson) pkgconfig(libcjson)
BuildRequires:  pkgconfig(libb2) libyuv-devel libsodium-devel bzip2-devel
BuildRequires:  systemd-rpm-macros
Provides:       bundled(nsync) = 1.30.0
Recommends:     xdg-desktop-portal
Suggests:       fipsd

%description
Desktop accessibility, authenticated remote control and low-latency video.
Uses system FFmpeg. Cisco OpenH264 is optional and loaded at runtime.
OpenH264 is never bundled.

%prep
%autosetup -n metadesk-%{version}
%{__tar} -xf %{SOURCE1} -C ..
# Supplied source archive must identify the exact upstream revision.
test "$(cat ../nostrc/.nostrc-commit)" = "%{nostrc_revision}"

%build
work=$(realpath ..)
./packaging/linux/build-nsync.sh "$work/nostrc" "$work/nsync-build" "$work/nsync-prefix"
export CFLAGS="%{optflags} -I$work/nsync-prefix/include"
export CXXFLAGS="%{optflags} -I$work/nsync-prefix/include"
export LDFLAGS="%{build_ldflags} -L$work/nsync-prefix/lib"
export LIBRARY_PATH="$work/nsync-prefix/lib"
export C_INCLUDE_PATH="$work/nsync-prefix/include"
export CMAKE_PREFIX_PATH="$work/nsync-prefix"
./packaging/linux/build-nostrc.sh "$work/nostrc" "$work/nostrc-build"
./packaging/linux/static-json.py "$work/nostrc-build" "$work/nostrc-static"
./packaging/linux/build.sh native "$PWD" "$work/nostrc" "$work/nostrc-static" "$work/native-build"
./packaging/linux/produce.py native "$PWD" "$work/native-build" "$work/nostrc" "$work/native-output" --keep-debug

%install
mkdir -p %{buildroot}
cp -a ../native-output/tree/usr %{buildroot}/

%check
LD_LIBRARY_PATH="../native-output/tree/usr/lib/metadesk" ../native-output/tree/usr/bin/metadesk-host --help
LD_LIBRARY_PATH="../native-output/tree/usr/lib/metadesk" ../native-output/tree/usr/bin/metadesk-client --help

%files
%license /usr/share/doc/metadesk/LICENSE
%doc /usr/share/doc/metadesk/THIRD_PARTY_NOTICES.md
%license /usr/share/doc/metadesk/*-LICENSE*
%{_mandir}/man1/metadesk-*.1*
/usr/bin/metadesk-host
/usr/bin/metadesk-client
/usr/lib/metadesk/
/usr/lib/systemd/user/metadesk-host.service
%{_datadir}/metadesk/

%changelog
* Sun Nov 22 2026 Chris Daley <chebizarro@gmail.com> - 0.1.0-1
- Initial native package with system FFmpeg and pinned static nostrc.
