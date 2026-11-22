#!/bin/sh
# GNOME's preinstalled codecs-extra extension can precede /app in LD_LIBRARY_PATH.
# Scope our verified LGPL libraries first, preserving the GL driver search paths.
set -eu
[ "$#" = 1 ] || exit 2
case "$1" in client|manager) ;; *) exit 2 ;; esac
for file in "/app/bin/metadesk-$1" /app/libexec/metadesk/verify-codecs /app/libexec/metadesk/decode-smoke; do
  [ ! -e "$file.bin" ] || exit 2
  mv "$file" "$file.bin"
  cat > "$file" <<WRAPPER
#!/bin/sh
export LD_LIBRARY_PATH="/app/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec "$file.bin" "\$@"
WRAPPER
  chmod 755 "$file"
done
