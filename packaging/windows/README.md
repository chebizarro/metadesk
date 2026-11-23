# Windows ARM64 MSI producer

This lane builds a **per-user**, native Windows ARM64 MSI using **WiX 5.0.2**
(v4 XML namespace). It installs under `%LOCALAPPDATA%\Programs\Metadesk`, adds
host/client Start-menu shortcuts and a **user** PATH entry. No elevation,
session-0 service, firewall rule, scheduled task, host process or relay session
is created. Capture/input must run in the logged-in user's interactive session.
Automatic logon startup is deliberately deferred until a credential/configuration
and opt-in lifecycle is available; a shortcut alone does not configure a host.
FIPS daemon is not bundled: mesh transport requires a separately installed and
configured Windows FIPS implementation. No authenticated/capture acceptance is
implied by `--help` smoke tests.

## Inputs and build

Use a CLANGARM64 shell in a private tree, **never** a sibling's build/install
paths. Prerequisites: MSYS2 clang, Python >=3.12, CMake, Meson, Ninja, pkgconf;
MSYS git/make/patch/diffutils; installed CLANGARM64 development/runtime packages
listed in `docs/testing/windows-build.md` plus libvpl. The VM qualification
extracts the three missing MSYS tools privately, without altering its package
database. All compile commands are bounded to `-j2`. In `sources.lock.json`,
`type: git` (FFmpeg, nv-codec-headers, nostrc and secp256k1) shallow-fetches
the full `commit`, verifies it and its `tree`, and exports only the locked
source. `type: git-blobs` verifies individual files by Git blob ID (AMF and
libyuv notices). `type: archive` verifies a published asset by `sha256`
(libidn2); `--cache` only applies to that type.
The current nostrc pin is `6f7a8d025768d497bef700fe55eea1c687dac328`; Windows
uses the selected CMake archives and the build-tree nostr-json import library.
AMF fetches only its pinned header subtree and license (58 Git-blob-hash-verified
files), not its multi-gigabyte repository containing unrelated SDK binaries.
Runtime DLL package versions are also pinned by the reviewed license policy.
Other MSYS2 development/tool package versions must match the qualification
inventory; package repositories are rolling, **not** an immutable SDK lock.
Do not call this a bit-for-bit reproducible release or a fully source-pinned
Windows dependency toolchain. Source tar mtimes, MSI IDs, Authenticode signatures
and RFC3161 timestamps are not normalized. File/component identities are stable.

```bash
export MSYSTEM=CLANGARM64
export PATH=/clangarm64/bin:/usr/bin
cd /c/Users/bizarro/md-w10
python metadesk/packaging/windows/fetch-sources.py inputs
bash metadesk/packaging/windows/build-arm64.sh inputs /c/Users/bizarro/md-w10/build
```

`build-arm64.sh` builds static MIT secp256k1 privately rather than inheriting the
VM's unowned DLL, sets FFmpeg's target explicitly to `mingw32/aarch64` (the MSYS shell itself is
x64-emulated), builds the complete Windows FFmpeg profile with the carried
MF `VT_BOOL` patch, then builds nostrc and both products. It never modifies
`packaging/ffmpeg/`. The stage contains only the recursive ARM64 PE import
closure, with an explicit SDL3 dlopen edge for sdl2-compat, not an indiscriminate
copy of MSYS2 bin. DLLs resolve only from the private FFmpeg/nostrc/product output and
CLANGARM64 prefix; API-set/System32 imports are OS-owned and not copied. Driver
DLLs dynamically loaded by NVENC/AMF are OS/user-owned, not bundled.
The ARM64 profile contains NVENC, AMF, and Media Foundation; QSV is x86-only.
Unknown runtime packages/versions, unresolved imports, non-ARM64 objects, forbidden codec
DLL names, unexpected FFmpeg codecs/configuration or MF patch drift fail.
The runtime inventory executable checks **the staged** avcodec/avutil DLLs.

`runtime-manifest.json` records hashes, imports, OS imports, package versions,
reviewed library license families and original package metadata. The VM's
libidn2 package omits license texts: the producer therefore requires hash-pinned
upstream 2.3.8 source and copies its licenses/library copyright header, also
shipping that upstream archive offline. This is not a substitute for publishing
the exact MSYS2 patches/recipe. libyuv LICENSE/PATENTS/AUTHORS are pinned to its
actual package source revision. `runtime-licenses.json`
is the versioned review allowlist; changing it requires inspecting the exact package's
upstream texts and actual library license, not guessing from its name. Dual
licensed libraries must use the documented non-GPL option. Full package/tool
versions and configure receipts remain in the build output. This gate does not
prove all possible runtime dlopens; installed help tests remove MSYS2/build paths
from DLL search and therefore catch missing startup libraries.

## MSI and signing

Obtain WiX 5.0.2 from its [official release](https://github.com/wixtoolset/wix/releases/tag/v5.0.2).
The VM uses process-only `powershell -NoProfile -ExecutionPolicy Bypass` to
run these unsigned development scripts; no persistent execution policy changes.
It uses an administrative extraction of `wix-cli-x64.msi` into the private
lane directory (x64 tool runs under Windows ARM64 emulation):

```powershell
msiexec /a C:\Users\bizarro\md-w10\wix-cli.msi /qn TARGETDIR=C:\Users\bizarro\md-w10\wix
$wix = 'C:\Users\bizarro\md-w10\wix\PFiles64\WiX Toolset v5.0\bin\wix.exe'
$python = 'C:\msys64\clangarm64\bin\python.exe'
.\metadesk\packaging\windows\build-msi.ps1 -Stage .\build\stage -Output .\unsigned -Wix $wix -Python $python
```

Default is **unsigned dry run**, with no certificate-store access. Opt-in
`-Sign -CertificateThumbprint <thumbprint> -TimestampUrl <RFC3161-URL>
-SignTool <SDK-signtool.exe>` signs each PE, updates its recorded hash, builds the
MSI, signs it and runs `signtool verify /pa /all /v`. It uses CurrentUser\My
(`/s My`, no `/sm`). The owner must provide a trusted code-signing certificate
with an accessible private key (or adapt the explicit signing boundary for
Azure Trusted Signing: account/profile, identity permissions, client plugin,
metadata and approved endpoint). A self-signed certificate is **not** a release
identity and does not establish SmartScreen reputation. No production keys or
credentials belong in this repository.

`test-signing.ps1 -Run` demonstrates the same path using a one-day throwaway
CurrentUser certificate, temporarily trusts only that certificate in
CurrentUser\Root, and deletes both entries and the private key in `finally`. The current VM's SSH
context rejects both .NET and certificate-only `certutil -user` Root imports
with `ERROR_NOT_SUPPORTED`; trusted self-signed verification is therefore
**not qualified** here. The exact console/profile/policy cause is not established.
Do not change machine trust, disable verification or automate a console prompt.
Microsoft documents the [Root store's protected nature](https://learn.microsoft.com/en-us/windows/win32/seccrypto/managing-certificates-with-certificate-stores)
and the [explicit user-store certutil operation](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/certutil#-addstore).
Use a disposable copy of the stage: signing changes DLL/exe bytes. Production
signatures need an owner-approved RFC3161 service; timestamp availability is a
hard failure, not a reason to silently omit it.

The [Environment authoring](https://docs.firegiant.com/wix/schema/wxs/environment/)
uses `Permanent=no`, `System=no`; uninstall removes just our PATH entry.
The [MajorUpgrade authoring](https://docs.firegiant.com/wix/schema/wxs/majorupgrade/)
blocks downgrades and removes the previous product inside the install transaction.
Same three-part-version rebuilds replace rather than co-install; four-part
versions are rejected. HKCU stores/restores the installed directory for normal
maintenance/uninstall; only the dedicated `Software\Metadesk.Installer` marker
tree is deleted, not application settings.
Never change UpgradeCode `0D682EE1-44D8-49AA-865E-EB91C50890FB` between releases.
Harvested per-user components use stable path-derived GUIDs and HKCU keypaths;
all authored subdirectories/shortcuts are removed on uninstall. User-created
files/configuration are never recursively deleted. Neither installer nor test
harness locks/disconnects the console or stops another lane's processes.

## Installed validation and winget (no submission)

```powershell
.\metadesk\packaging\windows\test-install.ps1 -Msi .\unsigned\metadesk-0.1.0-arm64.msi -EvidenceDirectory .\smoke
# Optional -UpgradeMsi points to a separately built higher three-part MSI version.
.\metadesk\packaging\windows\write-winget.ps1 -Msi .\unsigned\metadesk-0.1.0-arm64.msi -InstallerUrl https://example.invalid/metadesk/metadesk-0.1.0-arm64.msi -Output .\winget
winget validate .\winget
```

The test uses `%LOCALAPPDATA%\Programs\Metadesk-W10-Test`, refusing any existing
product/menu/installer key. It launches new `cmd /d` processes using persisted
machine/user PATH, without MSYS2 or build-tree paths, checks both `--help`s and
installed PE hashes, uninstalls and checks files/shortcuts/registry/PATH cleanup.
MSI registration/cache is managed by Windows Installer, not manually deleted.
Do not run against another lane's binary directory. It is not a factory-clean
VM or a cross-OS stream test. `test-packaging.py` tests authoring identities and
scope locally; it is not a substitute for native install/uninstall.

The checked-in three-file winget set is a **non-submittable qualification
sample**: its URL is intentionally unhosted, and its hash/ProductCode refer to
the recorded local unsigned artifact. Regenerate **after final release signing**
with the actual HTTPS release URL. Match MSI version, architecture, scope,
hash and ProductCode. `winget validate` schema success alone does not establish
hosted download/install acceptance. See Microsoft's [manifest documentation](https://github.com/microsoft/winget-pkgs/blob/master/doc/manifest/schema/1.6.0/installer.md).
No manifest submission or public upload is performed here.
`build-msi.ps1` also runs `wix msi validate` and fails on ICE errors. ICE91
warnings are expected for this intentionally per-user-only payload; ICE61 warns
about the explicit same-three-part-version upgrade policy. Warnings are retained,
not suppressed, and are not represented as errors or a warning-free result.

## Release gates

Owner-supplied trusted signing/Trusted Signing, immutable Windows dependency
source/package lock (including OpenSSL family reconciliation), exact sources
for every redistributed LGPL library, same-release FFmpeg source publication,
release notes from `packaging/RELEASE_NOTES_TEMPLATE.md`, SBOM and hosted winget
install acceptance remain release gates. Source and license material is not a
blanket H.264 patent clearance. No OpenH264 runtime download/UI is claimed by
this packaging work. x64 and older Windows support require separate native
build/install qualification; the available VM is Windows 11 ARM64 only.

Native x64 product/installer qualification is tracked by **md-sfxh**.

## Optional host logon controls (W17)

Install `ADDLOCAL=Main,HostLogon` to add the explicit Enable Host at logon action.
Provision a host account before enabling it. The task is InteractiveToken/Limited,
never a session-0 service; stop/uninstall in the same interactive session.
See [host lifecycle](../../docs/host-lifecycle.md) for stop, logs and limitations.
