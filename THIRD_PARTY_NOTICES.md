# Third-party components

THUMB is licensed under the GNU General Public License v3.0 (see `LICENSE`).
It includes or links the following components under their own licenses, all
compatible with GPL-3.0. Their full license texts are in their source trees.

| Component | Used for | License |
|---|---|---|
| [dynarmic](https://github.com/azahar-emu/dynarmic) | ARM32 → ARM64/x86-64 JIT | 0BSD |
| ↳ [oaknut](https://github.com/merryhime/oaknut) | ARM64 code emitter | MIT |
| ↳ [xbyak](https://github.com/herumi/xbyak) | x86-64 code emitter (Linux harness) | BSD-3-Clause |
| ↳ [zydis](https://github.com/zyantific/zydis), zycore | x86 disassembler (Linux harness) | MIT |
| ↳ [fmt](https://github.com/fmtlib/fmt) | formatting | MIT |
| ↳ [mcl](https://github.com/merryhime/mcl) | utilities | MIT |
| ↳ [robin-map](https://github.com/Tessil/robin-map) | hash maps | MIT |
| ↳ [biscuit](https://github.com/lioncash/biscuit) | RISC-V emitter (unused) | MIT |
| [TLSF](https://github.com/mattconte/tlsf) | guest heap allocator | BSD-3-Clause |
| [libffi](https://github.com/libffi/libffi) | Java → guest native trampolines | MIT |
| `jni.h` from [AOSP libnativehelper](https://android.googlesource.com/platform/libnativehelper/) | JNI types | Apache-2.0 |
| Boost (headers, build time only) | dynarmic dependency | BSL-1.0 |

THUMB contains no code or assets from any app or game it runs.
