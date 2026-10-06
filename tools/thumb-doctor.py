#!/usr/bin/env python3
"""THUMB Doctor: how ready is an APK for THUMB?

Reads every lib/armeabi-v7a/*.so in an APK (or .xapk/.apks bundle), lists the
symbols each one imports, and checks them against what THUMB implements.
Symbols exported by the app's other libraries count as satisfied.

  tools/thumb-doctor.py app.apk [--harness build/harness] [-v]
"""
import argparse
import collections
import io
import pathlib
import struct
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

PT_LOAD, PT_DYNAMIC = 1, 2
DT_NEEDED, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_GNU_HASH = 1, 4, 5, 6, 0x6FFFFEF5
STB_WEAK = 2

# Rough grouping of missing symbols, so the report says *what* is missing.
CATEGORIES = [
    ("OpenGL ES 2/3", lambda s: s.startswith("gl") and s[2:3].isupper()),
    ("EGL", lambda s: s.startswith("egl")),
    ("OpenSL ES (audio)", lambda s: s.startswith("SL") or s.startswith("slCreate")),
    ("AAudio (audio)", lambda s: s.startswith("AAudio")),
    ("NDK native window / assets / input", lambda s: s.startswith(("ANativeWindow", "AAsset", "AInput", "ALooper", "AConfiguration", "ANativeActivity", "ASensor", "AKey", "AMotion"))),
    ("OpenAL (audio)", lambda s: s.startswith(("al", "alc")) and s[2:3].isupper()),
    ("dynamic loading", lambda s: s in ("dlopen", "dlsym", "dlclose", "dlerror", "android_dlopen_ext")),
    ("C++ runtime (libstdc++/gnustl)", lambda s: s.startswith("_Z")),
    ("pthread", lambda s: s.startswith("pthread_") or s.startswith("sem_")),
    ("sockets / network", lambda s: s in ("socket", "bind", "listen", "accept", "connect", "send", "recv", "sendto", "recvfrom", "getsockopt", "setsockopt", "getaddrinfo", "gethostbyname", "inet_addr", "inet_pton", "inet_ntop", "shutdown", "getsockname", "getpeername")),
]


def category(sym):
    for name, test in CATEGORIES:
        if test(sym):
            return name
    return "libc / other"


class Elf32:
    """Minimal ELF32 reader using program headers only (like THUMB's loader)."""

    def __init__(self, data):
        self.d = data
        if data[:4] != b"\x7fELF" or data[4] != 1:
            raise ValueError("not a 32-bit ELF")
        phoff, = struct.unpack_from("<I", data, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
        self.loads, self.dynamic = [], None
        for i in range(phnum):
            p_type, p_off, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
            if p_type == PT_LOAD:
                self.loads.append((p_vaddr, p_off, p_filesz))
            elif p_type == PT_DYNAMIC:
                self.dynamic = p_vaddr
        if self.dynamic is None:
            raise ValueError("no PT_DYNAMIC")

    def off(self, vaddr):
        for v, o, sz in self.loads:
            if v <= vaddr < v + sz:
                return o + vaddr - v
        raise ValueError(f"address 0x{vaddr:x} not in file")

    def u32(self, vaddr):
        return struct.unpack_from("<I", self.d, self.off(vaddr))[0]

    def cstr(self, vaddr):
        o = self.off(vaddr)
        return self.d[o:self.d.index(b"\0", o)].decode("utf-8", "replace")

    def parse(self):
        tags, needed = {}, []
        a = self.dynamic
        while True:
            tag, val = struct.unpack_from("<iI", self.d, self.off(a))
            if tag == 0:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            else:
                tags[tag] = val
            a += 8
        strtab, symtab = tags[DT_STRTAB], tags[DT_SYMTAB]
        if DT_HASH in tags:
            nsyms = self.u32(tags[DT_HASH] + 4)
        else:  # GNU hash: walk to the last symbol
            gh = tags[DT_GNU_HASH]
            nb, symoff, bloom = self.u32(gh), self.u32(gh + 4), self.u32(gh + 8)
            buckets = gh + 16 + bloom * 4
            last = max([self.u32(buckets + 4 * i) for i in range(nb)] + [0])
            if last >= symoff:
                chains = buckets + nb * 4
                while not self.u32(chains + (last - symoff) * 4) & 1:
                    last += 1
                nsyms = last + 1
            else:
                nsyms = symoff
        imports, exports = {}, set()
        for i in range(1, nsyms):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", self.d, self.off(symtab + i * 16))
            if not st_name:
                continue
            name = self.cstr(strtab + st_name)
            if st_shndx == 0:
                imports[name] = (st_info >> 4) == STB_WEAK
            else:
                exports.add(name)
        return [self.cstr(strtab + n) for n in needed], imports, exports


def apk_libs(path):
    """Yields (lib name, bytes) for armeabi-v7a libs, looking inside bundles too."""
    def walk(zf, where):
        for info in zf.infolist():
            n = info.filename
            if n.endswith(".apk"):
                yield from walk(zipfile.ZipFile(io.BytesIO(zf.read(info))), f"{where}/{n}")
            elif n.startswith("lib/armeabi-v7a/") and n.endswith(".so"):
                yield pathlib.PurePosixPath(n).name, zf.read(info)
    yield from walk(zipfile.ZipFile(path), pathlib.Path(path).name)


def implemented(harness):
    out = subprocess.run([harness, "--list-thunks"], capture_output=True, text=True, check=True).stdout
    return set(out.split())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("apk")
    ap.add_argument("--harness", default=str(ROOT / "build/harness"))
    ap.add_argument("-v", "--verbose", action="store_true", help="list every missing symbol")
    args = ap.parse_args()

    have = implemented(args.harness)
    libs = {}
    for name, data in apk_libs(args.apk):
        try:
            libs[name] = Elf32(data).parse()
        except Exception as e:
            print(f"  {name}: cannot parse ({e})")
    if not libs:
        sys.exit("no armeabi-v7a libraries found: nothing for THUMB to do")

    app_exports = set().union(*(e for _, _, e in libs.values()))
    total_needed = total_ok = 0
    print(f"THUMB Doctor: {pathlib.Path(args.apk).name}\n")
    for name, (needed, imports, _) in sorted(libs.items()):
        system_needed = [n for n in needed if n not in libs]
        missing = sorted(s for s, weak in imports.items() if s not in have and s not in app_exports and not weak)
        ok = len(imports) - len(missing)
        total_needed += len(imports)
        total_ok += ok
        pct = 100 * ok // max(len(imports), 1)
        print(f"{name}: {ok}/{len(imports)} imports handled ({pct}%)")
        if system_needed:
            print(f"   system libs: {', '.join(system_needed)}")
        groups = collections.defaultdict(list)
        for s in missing:
            groups[category(s)].append(s)
        for cat, syms in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            shown = syms if args.verbose else syms[:6]
            more = "" if args.verbose or len(syms) <= 6 else f" ... +{len(syms) - 6}"
            print(f"   missing {cat} ({len(syms)}): {', '.join(shown)}{more}")
        print()
    pct = 100 * total_ok // max(total_needed, 1)
    verdict = "ready to try" if pct == 100 else "worth a try (missing calls are stubbed and may not matter)" if pct >= 90 else "needs work"
    print(f"Overall: {total_ok}/{total_needed} imports handled ({pct}%): {verdict}")


if __name__ == "__main__":
    main()
