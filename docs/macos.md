\page tpde-macos macOS ARM64 (experimental)

TPDE supports Mach-O object emission and native JIT execution on macOS ARM64,
using the Darwin calling convention and DWARF unwinding.

The Mach-O LLVM frontend currently rejects thread-local storage, custom sections,
and COMDAT. Mach-O objects use a fixed macOS 11.0 deployment baseline and do not
declare an SDK version. Other Apple platforms are not supported.

Native JIT execution uses `MAP_JIT`. Hardened-runtime applications need the
`com.apple.security.cs.allow-jit` entitlement. The mapper allocates writable
globals separately from executable code; the executable range returned by
`get_mapped_range()` does not include every allocation owned by the mapper.
