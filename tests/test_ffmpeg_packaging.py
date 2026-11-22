"""Fixture tests of patch staging/fail-closed wiring, NOT FFmpeg build evidence."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "packaging/ffmpeg/build-minimal-lgpl.sh"
SH = shutil.which("sh")


@unittest.skipUnless(os.name != "nt" and SH and shutil.which("patch"), "requires POSIX paths, sh and patch")
class PatchWiring(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="md-ffmpeg-wiring-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "source"
        (self.source / "libavcodec").mkdir(parents=True)
        self.encoder = self.source / "libavcodec/mfenc.c"
        self.original = (
            "        if (avctx->flags & AV_CODEC_FLAG_LOW_DELAY)\n"
            "            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVLowLatencyMode, FF_VAL_VT_UI4(1));\n"
            "    }\n"
        )
        self.encoder.write_text(self.original)
        (self.source / "COPYING.LGPLv2.1").write_text("fixture license\n")
        configure = self.source / "configure"
        configure.write_text('''#!/bin/sh
set -eu
for arg; do
    case "$arg" in
        --prefix=*) printf '%s\\n' "${arg#*=}" > .prefix ;;
        --enable-encoder=*) encoders=${arg#*=} ;;
    esac
done
printf '#define CONFIG_H264_DECODER 1\\n#define CONFIG_H264_PARSER 1\\n' > config_components.h
for name in $(printf '%s' "$encoders" | tr ',' ' ' | tr '[:lower:]' '[:upper:]'); do
    printf '#define CONFIG_%s_ENCODER 1\\n' "$name" >> config_components.h
done
for name in GPL NONFREE VERSION3 LIBX264 LIBOPENH264; do
    printf '#define CONFIG_%s 0\\n' "$name" >> config.h
done
echo 'License: LGPL version 2.1 or later'
''')
        configure.chmod(0o755)
        tools = self.root / "tools"
        tools.mkdir()
        make = tools / "make"
        make.write_text('#!/bin/sh\nset -eu\nif [ "${1:-}" = install ]; then mkdir -p "$(cat .prefix)"; fi\n')
        make.chmod(0o755)
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])

    def run_profile(self, profile, arch=None, output="output"):
        env = dict(self.env)
        if arch:
            env["MD_FFMPEG_ARCH"] = arch
        return subprocess.run([SH, str(SCRIPT), profile, str(self.source), str(self.root / output)],
                              env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_arch_correct_qsv_inventory_and_flags(self):
        for profile in ('windows', 'container', 'flatpak'):
            for arch in ('aarch64', 'arm64', 'x86_64', 'x86', 'i686'):
                with self.subTest(profile=profile, arch=arch):
                    output = profile + '-' + arch
                    result = self.run_profile(profile, arch, output)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    args = (self.root / output / 'configure-args.txt').read_text().splitlines()
                    encoders = (self.root / output / 'encoders.txt').read_text().strip().split(',')
                    intel = arch in ('x86_64', 'x86', 'i686')
                    self.assertEqual('--enable-libvpl' in args, intel)
                    self.assertEqual('--disable-libvpl' in args, not intel)
                    self.assertEqual('h264_qsv' in encoders, intel)
                    self.assertIn('h264_nvenc', encoders)
                    self.assertIn('h264_amf', encoders)
                    self.assertIn('h264_mf' if profile == 'windows' else 'h264_vaapi', encoders)

    def test_windows_patches_private_copy_and_retains_receipt(self):
        result = self.run_profile("windows")
        self.assertEqual(result.returncode, 0, result.stdout)
        out = self.root / "output"
        self.assertEqual(self.encoder.read_text(), self.original)
        self.assertIn("FF_VAL_VT_BOOL(VARIANT_TRUE)", (out / "source/libavcodec/mfenc.c").read_text())
        self.assertTrue((out / "patches/0001-mf-low-latency-vt-bool.patch").is_file())
        args = (out / "configure-args.txt").read_text().splitlines()
        self.assertEqual(args[0], str(out / "source/configure"))
        self.assertIn("--enable-d3d11va", args)
        self.assertIn("--disable-gpl", args)

    def test_windows_rejects_patch_drift_before_configure(self):
        self.encoder.write_text("unrecognized upstream source\n")
        result = self.run_profile("windows")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MF low-latency patch failed", result.stdout)
        self.assertFalse((self.root / "output/configure-args.txt").exists())
        self.assertEqual(self.encoder.read_text(), "unrecognized upstream source\n")

    def test_windows_does_not_reverse_an_already_applied_fix(self):
        fixed = self.original.replace("FF_VAL_VT_UI4(1)", "FF_VAL_VT_BOOL(VARIANT_TRUE)")
        self.encoder.write_text(fixed)
        result = self.run_profile("windows")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MF low-latency patch failed", result.stdout)
        self.assertFalse((self.root / "output/configure-args.txt").exists())
        self.assertEqual(self.encoder.read_text(), fixed)
        self.assertEqual((self.root / "output/source/libavcodec/mfenc.c").read_text(), fixed)

    def test_macos_does_not_stage_windows_patch(self):
        result = self.run_profile("macos")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse((self.root / "output/source").exists())
        self.assertFalse((self.root / "output/patches").exists())
        self.assertEqual(self.encoder.read_text(), self.original)


if __name__ == "__main__":
    unittest.main()
