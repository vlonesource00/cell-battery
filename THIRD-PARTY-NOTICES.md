# Third-party notices

The delivered Cell 1.0.3 executable is built with Microsoft Visual C++ Build Tools and the Windows SDK. It statically incorporates Microsoft's native runtime and C++ standard-library support. Microsoft runtime components are copyright Microsoft Corporation and supplied under the applicable Microsoft toolchain redistribution terms.

The alternative Zig build script and original release use Zig 0.15.2's native C++ toolchain. The corresponding license texts are retained in **licenses/** for that build:

- **LLVM-libcxx.txt** — LLVM libc++.
- **LLVM-libcxxabi.txt** — LLVM libc++abi.
- **MinGW-w64.txt** — MinGW-w64 runtime/support license.
- **Zig.txt** — Zig compiler/runtime support license.

Windows system DLLs are supplied by the operating system and are not redistributed in this package.
