#!/bin/sh
# validated on Docker 29.1.3 / rootless Podman 4.9.3 (bundle consumed, fixture + live GNOME); see validation.md.
# Producer validated on Ubuntu 24.04 aarch64, outside either runtime image.
# THROWAWAY LOCAL PRODUCER: md-kb7 inherits bundle-contract.md, NOT this script.
# Run on matching Ubuntu 24.04/architecture; never runs inside the runtime image.
set -eu
[ "$#" = 3 ] || { echo "usage: $0 SOURCE NOSTRC_ROOT OUTPUT (new directory)" >&2; exit 2; }
src=$(realpath "$1")
nostrc=$(realpath "$2")
[ ! -e "$3" ] || { echo 'output must not exist' >&2; exit 2; }
mkdir -p "$3"
out=$(realpath "$3")
build="$src/build-container"
[ ! -e "$build" ] || { echo 'build-container must not exist' >&2; exit 2; }
. /etc/os-release
[ "$ID:$VERSION_ID" = ubuntu:24.04 ] || { echo 'requires Ubuntu 24.04' >&2; exit 2; }
meson setup "$build" "$src" -Dclient=false -Dnostrc_root="$nostrc" \
    -Dnostrc_build_dir="$nostrc/_build" --prefix=/opt/metadesk --libdir=lib
meson compile -C "$build" -j2
meson install -C "$build" --destdir "$build/destdir"
cp -a "$build/destdir/opt" "$out/"
# ldd resolves the full transitive DT_NEEDED closure of these trusted local ELFs.
# Keep loader/glibc/compiler runtime base-owned, copy everything else privately.
python3 - "$out" <<'PY'
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
root = Path(sys.argv[1]) / 'opt/metadesk'
lib = root / 'lib'
env = dict(os.environ, LD_LIBRARY_PATH=str(lib))
elfs = [root / 'bin/metadesk-host', *lib.glob('*.so*')]
base = re.compile(r'^(ld-linux-.*|lib(c|m|pthread|dl|rt|resolv|util|anl|nss_.*)\.so\..*|libgcc_s\.so\..*|libstdc\+\+\.so\..*)$')
for elf in elfs:
    output = subprocess.check_output(['ldd', str(elf)], env=env, text=True)
    if 'not found' in output:
        raise SystemExit(output)
    for line in output.splitlines():
        match = re.search(r'=> (/\S+)', line)
        if match and not base.match(Path(match[1]).name):
            target = lib / Path(match[1]).name
            if not target.exists():
                shutil.copy2(match[1], target)
# Dereference all private symlinks, then remove producer/build-tree RPATHs.
for path in [root / 'bin/metadesk-host', *lib.glob('*.so*')]:
    if path.is_symlink():
        data = path.read_bytes()
        path.unlink()
        path.write_bytes(data)
        path.chmod(0o755)
    subprocess.run(['patchelf', '--remove-rpath', str(path)], check=True)
manifest = []
for path in sorted(root.rglob('*')):
    if path.is_file():
        manifest.append(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + str(path.relative_to(root)))
(root / 'SHA256SUMS').write_text('\n'.join(manifest) + '\n')
PY
# Final DT_NEEDED validation MUST ALSO run inside the pinned runtime image.
echo "Throwaway bundle staged at $out; verify in-image ldd before acceptance."
