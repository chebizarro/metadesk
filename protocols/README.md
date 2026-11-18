# Wayland input protocol sources

These MIT-licensed upstream XMLs retain their copyright/license blocks. Meson
runs `wayland-scanner` only with `-Dwayland_input=true`; generated C/headers are
not checked in. Vendoring avoids a network dependency during configuration.

- `wlr-virtual-pointer-unstable-v1.xml`: swaywm/wlr-protocols,
  commit `b010a03648b88d143236de193bddbfea0c08bc84`,
  `unstable/wlr-virtual-pointer-unstable-v1.xml`.
  SHA256 `3ff6d540be0bc5228195bf072bde42117ea17945a5c2061add5d3cf97d6bb524`.
- `virtual-keyboard-unstable-v1.xml`: swaywm/wlroots,
  commit `0855cdacb2eeeff35849e2e9c4db0aa996d78d10`,
  `protocol/virtual-keyboard-unstable-v1.xml`.
  SHA256 `7ad7870003ecd592cae47dc19d277a609b7f18fd7b7be012623cf3225a7294f5`.
  This protocol is **not** shipped by the released wayland-protocols package;
  wlroots carries the input-method-derived virtual-keyboard XML itself.

Upstream sources:
https://github.com/swaywm/wlr-protocols/blob/b010a03648b88d143236de193bddbfea0c08bc84/unstable/wlr-virtual-pointer-unstable-v1.xml
https://github.com/swaywm/wlroots/blob/0855cdacb2eeeff35849e2e9c4db0aa996d78d10/protocol/virtual-keyboard-unstable-v1.xml
