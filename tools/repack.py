#!/usr/bin/env python3
"""Repackages a 32-bit-only APK so it installs and runs on arm64-only phones.

For every lib/armeabi-v7a/libX.so in the input APK, the output contains
  lib/arm64-v8a/libX.so        the stub (loads the runtime, then the original)
  lib/arm64-v8a/libX_arm32.so  the original 32-bit library, unchanged
  lib/arm64-v8a/libthumb.so  the translator runtime
The result is zip-aligned and signed with a local key (created on first use).

  tools/repack.py original.apk patched.apk
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDK = pathlib.Path(os.environ.get("ANDROID_SDK", "/home/powmy/android-sdk"))
NDK = pathlib.Path(os.environ.get("NDK", SDK / "android-ndk-r30"))
KEYSTORE = pathlib.Path(os.environ.get("THUMB_KEYSTORE", pathlib.Path.home() / ".config/thumb/thumb.keystore"))
KEY_PASS = "thumbkey1"  # local signing key only; it protects nothing


def build_tool(name):
    for d in sorted((SDK / "build-tools").iterdir(), reverse=True):
        if (d / name).exists():
            return str(d / name)
    sys.exit(f"{name} not found under {SDK}/build-tools")


def ensure_keystore():
    if KEYSTORE.exists():
        return
    KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["keytool", "-genkeypair", "-keystore", str(KEYSTORE), "-alias", "thumb", "-keyalg", "RSA",
                    "-keysize", "2048", "-validity", "10000", "-storepass", KEY_PASS, "-keypass", KEY_PASS,
                    "-dname", "CN=THUMB local"], check=True, capture_output=True)
    print(f"created signing key {KEYSTORE}")


def strip(src, dst):
    strip_bin = NDK / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
    subprocess.run([str(strip_bin), "--strip-unneeded", "-o", str(dst), str(src)], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--build", default=str(ROOT / "build-android"), help="directory with the Android build")
    args = ap.parse_args()

    build = pathlib.Path(args.build)
    runtime, stub = build / "libthumb.so", build / "libthumb_stub.so"
    for f in (runtime, stub):
        if not f.exists():
            sys.exit(f"missing {f}: run tools/build-android.sh first")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        strip(runtime, tmp / "runtime.so")
        strip(stub, tmp / "stub.so")
        unsigned = tmp / "unsigned.apk"

        src = zipfile.ZipFile(args.input)
        arm32 = [i for i in src.infolist() if i.filename.startswith("lib/armeabi-v7a/") and i.filename.endswith(".so")]
        if not arm32:
            sys.exit("no lib/armeabi-v7a libraries in this APK: nothing to do")
        other_abis = {i.filename.split("/")[1] for i in src.infolist() if i.filename.startswith("lib/")} - {"armeabi-v7a", "armeabi"}
        if other_abis:
            print(f"warning: APK also has {sorted(other_abis)}; those are dropped")

        with zipfile.ZipFile(unsigned, "w") as out:
            for info in src.infolist():
                name = info.filename
                if name.startswith("lib/"):
                    continue  # all native libs are rebuilt below
                if name.startswith("META-INF/") and name.upper().endswith((".SF", ".RSA", ".DSA", ".EC", ".MF")):
                    continue  # old signature
                out.writestr(info, src.read(info))  # keeps each entry's compression
            for info in arm32:
                lib = pathlib.PurePosixPath(info.filename).name  # libX.so
                orig = lib[:-3] + "_arm32.so"
                out.write(tmp / "stub.so", f"lib/arm64-v8a/{lib}", zipfile.ZIP_DEFLATED)
                out.writestr(zipfile.ZipInfo(f"lib/arm64-v8a/{orig}", info.date_time), src.read(info), zipfile.ZIP_DEFLATED)
                print(f"  {lib}: stub + {orig}")
            out.write(tmp / "runtime.so", "lib/arm64-v8a/libthumb.so", zipfile.ZIP_DEFLATED)

        aligned = tmp / "aligned.apk"
        subprocess.run([build_tool("zipalign"), "-p", "-f", "4", str(unsigned), str(aligned)], check=True)
        ensure_keystore()
        subprocess.run([build_tool("apksigner"), "sign", "--ks", str(KEYSTORE), "--ks-key-alias", "thumb",
                        "--ks-pass", f"pass:{KEY_PASS}", "--key-pass", f"pass:{KEY_PASS}",
                        "--out", args.output, str(aligned)], check=True)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
